#include "web/engine.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <utility>
#include <vector>

#include "web/css_parser.h"
#include "web/html.h"
#include "web/utf8.h"

namespace web {
namespace {

// iframe 嵌套的安全上限。文档深度超过该值时不再加载更内层文档。
constexpr int kMaximumFrameDepth = 16;
// iframe 缺少尺寸信息时使用的缺省视口，与替换元素的缺省尺寸一致。
constexpr float kDefaultFrameWidth = 300.0f;
constexpr float kDefaultFrameHeight = 150.0f;

// 单个 HTML/CSS 资源的读取上限，避免恶意构造的超大文件耗尽内存。
constexpr std::uintmax_t kMaximumTextFileBytes = 16u * 1024u * 1024u;

bool ReadTextFile(const std::string& path, std::string* content) {
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(std::filesystem::u8path(path), error);
    if (!error && size > kMaximumTextFileBytes) {
        return false;
    }
    std::ifstream stream(std::filesystem::u8path(path), std::ios::binary);
    if (!stream) {
        return false;
    }
    content->assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    return true;
}

std::string JoinPath(const std::string& base, const std::string& relative) {
    if (relative.empty()) {
        return base;
    }
    // 相对路径来自文档标记，可能不是合法 UTF-8：转换失败时返回空字符串，
    // 由调用方按资源缺失处理，避免路径构造抛出异常。
    std::filesystem::path path;
    try {
        path = std::filesystem::u8path(relative);
    } catch (const std::exception&) {
        return std::string();
    }
    if (path.is_absolute()) {
        return path.u8string();
    }
    if (base.empty()) {
        return path.u8string();
    }
    // 基准目录以 UTF-8 保存，必须用 u8path 还原，不能使用本地代码页的 path(string)。
    return (std::filesystem::u8path(base) / path).u8string();
}

bool IsRemoteSource(const std::string& source) {
    return source.find("://") != std::string::npos || source.compare(0, 2, "//") == 0;
}

// 把路径规范化为绝对路径并统一分隔符，供循环检测比较使用。
std::string NormalizeAbsolutePath(const std::string& path) {
    if (path.empty()) {
        return std::string();
    }
    std::filesystem::path filesystem_path = std::filesystem::u8path(path);
    if (!filesystem_path.is_absolute()) {
        filesystem_path = std::filesystem::absolute(filesystem_path);
    }
    filesystem_path = filesystem_path.lexically_normal();
    return filesystem_path.u8string();
}

// data: 开头的内联资源不是文件路径，跳过加载。
bool IsInlineSource(const std::string& source) {
    return source.size() >= 5 && AsciiLower(source.substr(0, 5)) == "data:";
}

// 解析 iframe 的 width/height 属性：属性值不带单位，缺失或非法时返回回退值。
float ParseFrameDimension(const std::string& value, float fallback) {
    std::string text = value;
    TrimAsciiSpace(&text);
    if (text.empty()) {
        return fallback;
    }
    char* end = nullptr;
    const float parsed = std::strtof(text.c_str(), &end);
    if (end == nullptr || end == text.c_str()) {
        return fallback;
    }
    if (*end != '\0' || !(parsed > 0.0f)) {
        return fallback;
    }
    return parsed;
}

// 在布局子树中按元素指针查找对应的布局盒；找不到返回空指针。
const LayoutBox* FindLayoutBoxForElement(const LayoutBox& box, const Element* element) {
    if (box.element == element) {
        return &box;
    }
    for (const std::unique_ptr<LayoutBox>& child : box.children) {
        if (!child) {
            continue;
        }
        const LayoutBox* found = FindLayoutBoxForElement(*child, element);
        if (found != nullptr) {
            return found;
        }
    }
    return nullptr;
}

}  // namespace

Engine::Engine() : document_(std::make_unique<Document>()) {}

Engine::~Engine() = default;

std::unique_ptr<LayoutResult> Engine::TakeLayout() {
    return std::move(layout_);
}

void Engine::SetFrameDepth(int depth) {
    frame_depth_ = depth > 0 ? depth : 0;
    dirty_ = true;
}

void Engine::SetTextService(TextService* text) {
    text_service_ = text;
    dirty_ = true;
}

bool Engine::LoadHtmlString(const std::string& html, std::string base_directory,
                            std::string* error) {
    document_ = std::make_unique<Document>();
    if (!ParseHtmlDocument(html, *document_, error)) {
        return false;
    }
    base_directory_ = std::move(base_directory);
    // 字符串载入没有文件来源，循环检测链由调用方通过祖先路径提供。
    document_path_.clear();
    layout_.reset();
    frames_.clear();
    dirty_ = true;
    return ParseStyleSources(error);
}

bool Engine::LoadFile(const std::string& path, std::string* error) {
    std::string source;
    if (!ReadTextFile(path, &source)) {
        if (error != nullptr) {
            *error = "无法读取 HTML 文件: " + path;
        }
        return false;
    }
    std::filesystem::path filesystem_path = std::filesystem::u8path(path);
    // 基准目录必须以 UTF-8 保存：path::string() 在 Windows 上返回本地代码页文本，
    // 含中文的目录会因此在后续 u8path 解析时失败。
    std::string base = filesystem_path.parent_path().u8string();
    if (!LoadHtmlString(source, base, error)) {
        return false;
    }
    document_path_ = NormalizeAbsolutePath(path);
    return true;
}

void Engine::SetAncestorDocumentPaths(std::vector<std::string> ancestor_paths) {
    ancestor_paths_ = std::move(ancestor_paths);
}

void Engine::SetViewport(float width, float page_content_height) {
    viewport_width_ = width > 1.0f ? width : 1.0f;
    page_content_height_ = page_content_height;
    dirty_ = true;
}

bool Engine::Relayout(std::string* error) {
    if (text_service_ == nullptr) {
        if (error != nullptr) {
            *error = "布局前必须设置 TextService";
        }
        return false;
    }
    // SVG 子树内部的变更不影响 HTML 布局：复用现有布局树，只增量刷新
    // SVG 计算样式。拖动、选择图元时避免整页重新排版。
    if (!dirty_ && svg_dirty_ && layout_ != nullptr) {
        RefreshSvgStyles(*document_, resolver_, layout_.get(), layout_.get());
        svg_dirty_ = false;
        return true;
    }
    bool styles_ok = ParseStyleSources(error);
    LayoutOptions options;
    options.viewport_width = viewport_width_;
    options.page_content_height = page_content_height_;
    // 动画时钟在布局前写入样式解析器，关键帧按该时刻采样。
    resolver_.SetAnimationTime(animation_time_);
    resolver_.ResetAnimationFlag();
    std::unique_ptr<LayoutResult> previous_layout = std::move(layout_);
    std::unique_ptr<LayoutResult> layout =
        LayoutDocument(*document_, resolver_, *text_service_, options);

    // 内嵌框架在替换 layout_ 之前完成布局：未变化的子框架复用原引擎，
    // 新布局与框架记录整体交接，避免出现悬空的子布局指针。
    std::map<const Element*, std::unique_ptr<FrameRecord>> next_frames;
    if (layout != nullptr) {
        layout->base_directory = base_directory_;
        LayoutEmbeddedFrames(layout.get(), &next_frames);
        // SVG 样式按元素签名增量刷新：样式表与 class/style/id 未变化的图元
        // 直接复用上一版结果，完整布局不再为数百个图元重新匹配选择器。
        RefreshSvgStyles(*document_, resolver_, layout.get(), previous_layout.get());
    }
    layout_ = std::move(layout);
    frames_ = std::move(next_frames);
    if (layout_ != nullptr) {
        layout_->has_animations = resolver_.animation_active();
    }
    dirty_ = false;
    svg_dirty_ = false;
    return styles_ok && layout_ != nullptr;
}

bool Engine::HasActiveAnimations() const {
    return layout_ != nullptr && layout_->has_animations;
}

// 加载并布局文档中的 iframe 子文档。子文档使用 iframe 内容盒尺寸作为视口，
// 因此子文档的块级流、行内断行与视口单位都按框架内部尺寸计算。
void Engine::LayoutEmbeddedFrames(
    LayoutResult* layout, std::map<const Element*, std::unique_ptr<FrameRecord>>* next_frames) {
    if (layout == nullptr || layout->root == nullptr || next_frames == nullptr) {
        return;
    }
    // 深度限制：子文档深度超过安全上限时不再加载更内层文档。
    if (frame_depth_ + 1 > kMaximumFrameDepth) {
        return;
    }

    std::vector<Element*> frame_elements;
    CollectElementsByTag(*document_, "iframe", &frame_elements);
    for (Element* element : frame_elements) {
        if (element == nullptr) {
            continue;
        }

        std::string source = element->GetAttribute("src");
        TrimAsciiSpace(&source);
        if (source.empty() || IsInlineSource(source) || IsRemoteSource(source)) {
            continue;
        }

        // 循环检测：子文档绝对路径已出现在当前加载链中时跳过，
        // 使指向自身或祖先文档的 iframe 保持空白而不递归。
        const std::string absolute_path =
            NormalizeAbsolutePath(JoinPath(base_directory_, source));
        bool cyclical = !document_path_.empty() && absolute_path == document_path_;
        for (const std::string& ancestor : ancestor_paths_) {
            if (absolute_path == ancestor) {
                cyclical = true;
                break;
            }
        }
        if (cyclical) {
            continue;
        }

        // 视口优先取布局后的内容盒尺寸；几何缺失时退回 width/height 属性。
        const LayoutBox* frame_box = FindLayoutBoxForElement(*layout->root, element);
        float viewport_width = frame_box != nullptr ? frame_box->content_box.Width() : 0.0f;
        float viewport_height = frame_box != nullptr ? frame_box->content_box.Height() : 0.0f;
        if (viewport_width <= 0.0f) {
            viewport_width = ParseFrameDimension(element->GetAttribute("width"),
                                                 kDefaultFrameWidth);
        }
        if (viewport_height <= 0.0f) {
            viewport_height = ParseFrameDimension(element->GetAttribute("height"),
                                                  kDefaultFrameHeight);
        }

        // 同一 iframe 元素且 src 未变时复用原记录，保留子 DOM、脚本宿主
        // 与事件状态；src 改变或元素消失时旧记录随本次替换一并释放。
        std::unique_ptr<FrameRecord> record;
        const auto existing = frames_.find(element);
        if (existing != frames_.end() && existing->second != nullptr &&
            existing->second->source == source) {
            record = std::move(existing->second);
            frames_.erase(existing);
        } else {
            record = std::make_unique<FrameRecord>();
            record->source = source;
            record->engine = std::make_unique<Engine>();
            record->engine->SetTextService(text_service_);
            record->engine->SetFrameDepth(frame_depth_ + 1);
            std::vector<std::string> ancestor_chain = ancestor_paths_;
            if (!document_path_.empty()) {
                ancestor_chain.push_back(document_path_);
            }
            record->engine->SetAncestorDocumentPaths(std::move(ancestor_chain));
            record->engine->SetRunScripts(run_scripts_);
            std::string load_error;
            if (!record->engine->LoadFile(absolute_path, &load_error)) {
                continue;
            }
        }
        if (record == nullptr || record->engine == nullptr) {
            continue;
        }

        record->engine->SetRunScripts(run_scripts_);
        record->engine->SetViewport(viewport_width, viewport_height);
        std::string nested_error;
        if (!record->engine->Relayout(&nested_error)) {
            continue;
        }
        // 脚本宿主只在首次加载时创建并执行脚本，后续重排复用脚本状态。
        if (record->script == nullptr) {
            StartFrameScripts(record.get());
        } else {
            // 框架尺寸可能随页面重排改变，同步更新脚本中的视口尺寸。
            record->script->SetViewport(viewport_width, viewport_height);
        }
        const LayoutResult* child_layout = record->engine->layout();
        if (child_layout == nullptr) {
            continue;
        }
        layout->embedded_frames[element] = child_layout;
        next_frames->emplace(element, std::move(record));
    }
}

// 为子文档创建常驻脚本宿主并执行其全部脚本。脚本改动 DOM 时子引擎置脏，
// 这里最多重新布局 3 轮，避免脚本自身触发无休止的重排。
bool Engine::StartFrameScripts(FrameRecord* record) {
    if (record == nullptr || record->engine == nullptr) {
        return false;
    }
    Engine* child = record->engine.get();
    const std::vector<std::string> scripts = child->CollectScripts();
    if (scripts.empty()) {
        return false;
    }

    ScriptHostOptions options;
    options.on_document_changed = [child]() { child->MarkDirty(); };
    // SVG 内部变更只需要子引擎刷新 SVG 样式，不重排子文档。
    options.on_svg_changed = [child]() { child->MarkSvgDirty(); };
    // 请求动画帧意味着下一帧可能改动文档，同样标记为需要重排。
    options.on_animation_frame = [child]() { child->MarkDirty(); };
    // 子文档的元素几何按子布局结果查询。
    options.get_element_client_rect = [child](const Element& element, float* left, float* top,
                                              float* width, float* height) {
        const LayoutResult* layout = child->layout();
        if (layout == nullptr || layout->root == nullptr) {
            return false;
        }
        const LayoutBox* box = FindLayoutBoxForElement(*layout->root, &element);
        if (box == nullptr) {
            return false;
        }
        *left = box->border_box.left;
        *top = box->border_box.top;
        *width = box->border_box.Width();
        *height = box->border_box.Height();
        return true;
    };

    std::unique_ptr<ScriptHost> host = ScriptHost::Create(&child->document(), options);
    if (!host) {
        return false;
    }
    host->SetViewport(child->viewport_width(), child->page_content_height());
    for (const std::string& source : scripts) {
        std::string exception;
        host->Evaluate(source, &exception);
    }
    record->script = std::move(host);

    for (int round = 0; round < 3 && child->dirty(); ++round) {
        std::string error;
        if (!child->Relayout(&error)) {
            break;
        }
    }
    return true;
}

bool Engine::AdvanceFrameScripts(float delta_ms) {
    bool relayouted = false;
    for (auto& entry : frames_) {
        FrameRecord* record = entry.second.get();
        if (record == nullptr || record->engine == nullptr) {
            continue;
        }
        // 先推进更深层框架，其重排结果由子引擎自行刷新。
        if (record->engine->AdvanceFrameScripts(delta_ms)) {
            relayouted = true;
        }
        if (record->script) {
            record->script->AdvanceTime(static_cast<double>(delta_ms));
            if (record->engine->dirty()) {
                std::string error;
                record->engine->Relayout(&error);
                relayouted = true;
            }
        }
    }
    // 子布局对象可能因重排被替换，统一刷新本层布局中的内嵌指针。
    RefreshEmbeddedPointers();
    return relayouted;
}

bool Engine::DispatchFrameClick(const Element& frame_element, const Element* target, float x,
                                float y) {
    const auto found = frames_.find(&frame_element);
    if (found != frames_.end() && found->second != nullptr &&
        found->second->engine != nullptr && found->second->script != nullptr) {
        FrameRecord* record = found->second.get();
        record->script->DispatchClick(target, x, y);
        if (record->engine->dirty()) {
            std::string error;
            record->engine->Relayout(&error);
        }
        RefreshEmbeddedPointers();
        return true;
    }

    // frame_element 可能属于更深层文档，逐层向下转交。
    for (auto& entry : frames_) {
        if (entry.second == nullptr || entry.second->engine == nullptr) {
            continue;
        }
        if (entry.second->engine->DispatchFrameClick(frame_element, target, x, y)) {
            RefreshEmbeddedPointers();
            return true;
        }
    }
    return false;
}

bool Engine::DocumentOwnsNode(const Node* node) const {
    if (node == nullptr) {
        return false;
    }
    if (node->OwnerDocument() == document_.get()) {
        return true;
    }
    for (const auto& entry : frames_) {
        const FrameRecord* record = entry.second.get();
        if (record != nullptr && record->engine != nullptr &&
            record->engine->DocumentOwnsNode(node)) {
            return true;
        }
    }
    return false;
}

bool Engine::DispatchEvent(const Element* target, const std::string& type,
                           const ScriptEventInit& init, bool* default_prevented) {
    if (target == nullptr) {
        return false;
    }
    // 顶层文档元素交给调用方注册的脚本宿主。
    if (target->OwnerDocument() == document_.get()) {
        bool prevented = false;
        if (top_level_script_ != nullptr) {
            prevented = top_level_script_->DispatchEvent(target, type, init);
        }
        if (default_prevented != nullptr) {
            *default_prevented = prevented;
        }
        return true;
    }
    // 子文档元素逐层转交，直到找到拥有该元素的子引擎。
    for (auto& entry : frames_) {
        FrameRecord* record = entry.second.get();
        if (record == nullptr || record->engine == nullptr) {
            continue;
        }
        if (!record->engine->DocumentOwnsNode(target)) {
            continue;
        }
        // 目标直接属于该子引擎的文档：交给记录中保存的子文档脚本宿主。
        if (record->engine->document_.get() == target->OwnerDocument()) {
            bool prevented = false;
            if (record->script) {
                prevented = record->script->DispatchEvent(target, type, init);
            }
            if (default_prevented != nullptr) {
                *default_prevented = prevented;
            }
            return true;
        }
        // 目标在更深层内嵌文档中，继续向下转交。
        return record->engine->DispatchEvent(target, type, init, default_prevented);
    }
    return false;
}

const Element* Engine::FrameElementForNode(const Node& node) const {
    for (const auto& entry : frames_) {
        const FrameRecord* record = entry.second.get();
        if (record != nullptr && record->engine != nullptr &&
            record->engine->DocumentOwnsNode(&node)) {
            return entry.first;
        }
    }
    return nullptr;
}

bool Engine::RelayoutFrame(const Element& frame_element) {
    const auto found = frames_.find(&frame_element);
    if (found != frames_.end() && found->second != nullptr &&
        found->second->engine != nullptr) {
        FrameRecord* record = found->second.get();
        // 框架尺寸可能随顶层重排改变，重新布局前按当前内容盒同步子文档视口。
        const LayoutBox* frame_box =
            layout_ != nullptr && layout_->root != nullptr
                ? FindLayoutBoxForElement(*layout_->root, &frame_element)
                : nullptr;
        if (frame_box != nullptr && frame_box->content_box.Width() > 0.0f &&
            frame_box->content_box.Height() > 0.0f) {
            record->engine->SetViewport(frame_box->content_box.Width(),
                                       frame_box->content_box.Height());
        }
        std::string error;
        record->engine->Relayout(&error);
        RefreshEmbeddedPointers();
        return true;
    }
    // frame_element 可能属于更深层文档，逐层向下查找。
    for (auto& entry : frames_) {
        FrameRecord* record = entry.second.get();
        if (record == nullptr || record->engine == nullptr) {
            continue;
        }
        if (record->engine->RelayoutFrame(frame_element)) {
            RefreshEmbeddedPointers();
            return true;
        }
    }
    return false;
}

void Engine::RefreshEmbeddedPointers() {
    if (layout_ == nullptr) {
        return;
    }
    for (auto& entry : frames_) {
        FrameRecord* record = entry.second.get();
        const LayoutResult* child_layout =
            record != nullptr && record->engine != nullptr ? record->engine->layout() : nullptr;
        if (child_layout != nullptr) {
            layout_->embedded_frames[entry.first] = child_layout;
        } else {
            layout_->embedded_frames.erase(entry.first);
        }
    }
}

bool Engine::ReloadStyles(std::string* error) {
    return ParseStyleSources(error);
}

std::vector<std::string> Engine::CollectScripts() const {
    std::vector<std::string> scripts;
    std::vector<Element*> elements;
    CollectElementsByTag(*document_, "script", &elements);
    for (Element* element : elements) {
        // 外链脚本按文档顺序就地加载，相对路径基于文档所在目录解析。
        const std::string source_path = element->GetAttribute("src");
        if (!source_path.empty() && !IsRemoteSource(source_path)) {
            std::string external;
            if (ReadTextFile(JoinPath(base_directory_, source_path), &external)) {
                scripts.push_back(std::move(external));
            }
            continue;
        }
        std::string source = TextContent(*element);
        if (!source.empty()) {
            scripts.push_back(std::move(source));
        }
    }
    return scripts;
}

bool Engine::ParseStyleSources(std::string* error) {
    style_source_.clear();

    std::vector<Element*> style_elements;
    CollectElementsByTag(*document_, "style", &style_elements);
    for (Element* element : style_elements) {
        std::string source = TextContent(*element);
        if (!source.empty()) {
            style_source_.append(source);
            style_source_.push_back('\n');
        }
    }

    std::vector<Element*> link_elements;
    CollectElementsByTag(*document_, "link", &link_elements);
    for (Element* element : link_elements) {
        if (AsciiLower(element->GetAttribute("rel")) != "stylesheet") {
            continue;
        }
        const std::string href = element->GetAttribute("href");
        if (href.empty() || IsRemoteSource(href)) {
            continue;
        }
        std::string source;
        if (ReadTextFile(JoinPath(base_directory_, href), &source)) {
            style_source_.append(source);
            style_source_.push_back('\n');
        }
    }

    stylesheet_.rules.clear();
    if (!style_source_.empty()) {
        std::string parse_error;
        /* 视口高度用于 vh 与 @media 求值：分页模式取每页内容区高度，
           未分页时按 800 像素处理（与布局阶段保持一致）。 */
        const float viewport_height = page_content_height_ > 0.0f ? page_content_height_ : 800.0f;
        if (!ParseStyleSheet(style_source_, viewport_width_, viewport_height, &stylesheet_,
                             &parse_error)) {
            if (error != nullptr) {
                *error = parse_error;
            }
            // 解析失败时仍然保留已解析出的规则，避免整页失去样式。
        }
    }
    resolver_.SetViewport(viewport_width_, page_content_height_ > 0.0f ? page_content_height_ : 800.0f);
    resolver_.SetStyleSheet(&stylesheet_);
    return true;
}

}  // namespace web
