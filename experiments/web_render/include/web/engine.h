#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "web/css.h"
#include "web/dom.h"
#include "web/layout.h"
#include "web/paint.h"
#include "web/script.h"
#include "web/style.h"
#include "web/text.h"

namespace web {

// 高层引擎：持有文档、样式表与布局结果，把解析、层叠、布局、分页串起来。
// 渲染后端与脚本宿主由调用方提供，引擎不直接依赖具体图形 API。
class Engine {
public:
    Engine();
    ~Engine();

    Document& document() { return *document_; }
    const Document& document() const { return *document_; }

    // 文本服务用于测量与字形栅格化，生命周期由调用方管理；必须在布局前设置。
    void SetTextService(TextService* text);
    TextService* text_service() const { return text_service_; }

    bool LoadHtmlString(const std::string& html, std::string base_directory, std::string* error);
    // 读取 UTF-8 文件；base_directory 自动设为文件所在目录。
    bool LoadFile(const std::string& path, std::string* error);

    // 页面内容区尺寸（CSS 像素）。高度 <= 0 表示不分页。
    void SetViewport(float width, float page_content_height);
    float viewport_width() const { return viewport_width_; }
    float page_content_height() const { return page_content_height_; }

    // CSS 动画时钟（秒）。下一次 Relayout 按该时刻采样关键帧。
    void SetAnimationTime(double seconds) { animation_time_ = seconds; }
    double animation_time() const { return animation_time_; }
    // 当前布局是否包含带动画的元素；窗口层据此决定是否按帧重绘。
    bool HasActiveAnimations() const;

    // 解析样式来源（<style> 与本地 <link rel="stylesheet">）并完成布局。
    bool Relayout(std::string* error);
    // 只重新解析样式，不重建 DOM（脚本修改 style 属性后仍要走 Relayout）。
    bool ReloadStyles(std::string* error);

    const LayoutResult* layout() const { return layout_.get(); }
    // 交出布局结果的所有权，调用后 layout() 返回空指针。
    // 内嵌框架布局不再使用本接口，子布局始终由子引擎持有以保持脚本与事件状态。
    std::unique_ptr<LayoutResult> TakeLayout();

    // 设置当前文档在 iframe 嵌套层级中的深度，顶层文档为 0。
    // 达到安全上限后不再加载更内层的文档，避免过深嵌套耗尽资源。
    void SetFrameDepth(int depth);
    int frame_depth() const { return frame_depth_; }

    // 设置祖先文档的绝对路径链（不含本文档自身），用于检测 iframe 循环引用。
    void SetAncestorDocumentPaths(std::vector<std::string> ancestor_paths);
    const std::vector<std::string>& ancestor_document_paths() const {
        return ancestor_paths_;
    }
    const std::string& document_path() const { return document_path_; }

    // 是否执行内嵌子文档中的脚本。需要在 Relayout 之前设置。
    void SetRunScripts(bool enabled) { run_scripts_ = enabled; }
    bool run_scripts() const { return run_scripts_; }

    // 推进所有内嵌文档脚本的时间（setTimeout 与 requestAnimationFrame）。
    // 子文档脚本改动文档时重新布局对应子文档，并刷新顶层布局中的内嵌指针。
    // 返回是否发生了重新布局。
    bool AdvanceFrameScripts(float delta_ms);

    // 向命中元素所属内嵌文档派发点击。frame_element 为命中元素所在子文档的
    // iframe 元素（可以是更深层 iframe）；x/y 为子文档坐标。
    // 返回是否找到对应子文档并完成派发。
    bool DispatchFrameClick(const Element& frame_element, const Element* target, float x, float y);

    // 注册顶层文档的脚本宿主（由调用方持有），供事件按文档归属统一派发。
    void SetTopLevelScriptHost(ScriptHost* host) { top_level_script_ = host; }

    // 派发事件到目标元素：顶层元素交给顶层脚本宿主，内嵌文档元素逐层转交到
    // 对应子引擎的脚本宿主。返回是否找到拥有该元素的脚本宿主；
    // default_prevented 非空时写入页面是否调用了 preventDefault。
    bool DispatchEvent(const Element* target, const std::string& type,
                       const ScriptEventInit& init, bool* default_prevented = nullptr);

    // 按元素查找其所属文档最外层 iframe 元素；顶层文档元素返回空指针。
    const Element* FrameElementForNode(const Node& node) const;

    // 重新布局指定 iframe 的子文档并刷新各层布局中的内嵌指针。
    // frame_element 可以属于顶层文档或更深层子文档；返回是否找到对应框架。
    bool RelayoutFrame(const Element& frame_element);

    const StyleSheet& stylesheet() const { return stylesheet_; }
    const StyleResolver& style_resolver() const { return resolver_; }

    int PageCount() const { return layout_ ? layout_->PageCount() : 0; }

    // 依次返回文档内 <script> 的文本内容（不含外链脚本）。
    std::vector<std::string> CollectScripts() const;

    // 脚本改动 DOM 后调用；下一次 Relayout 重建样式与布局。
    void MarkDirty() { dirty_ = true; }
    // SVG 子树内部（图元、class、变换）发生变化：不影响 HTML 布局，
    // 只需刷新 SVG 计算样式并重绘。
    void MarkSvgDirty() { svg_dirty_ = true; }
    bool dirty() const { return dirty_ || svg_dirty_; }

    const std::string& base_directory() const { return base_directory_; }

private:
    // 内嵌框架的运行状态：子引擎持有子文档 DOM、布局与更深层框架；
    // 常驻脚本宿主与子引擎同等存活，避免每次重新布局丢失脚本状态。
    struct FrameRecord {
        std::unique_ptr<Engine> engine;
        // 声明在 engine 之后，析构时先于子文档释放脚本宿主。
        std::unique_ptr<ScriptHost> script;
        // 上次加载的 src 属性原文；src 改变时重建子引擎。
        std::string source;
    };

    bool ParseStyleSources(std::string* error);
    // 递归收集文档中的 iframe，复用或创建子引擎、布局子文档并登记内嵌布局。
    // next_frames 接收本次布局使用的框架记录；布局结束后整体替换旧记录。
    void LayoutEmbeddedFrames(
        LayoutResult* layout,
        std::map<const Element*, std::unique_ptr<FrameRecord>>* next_frames);
    // 为子文档创建常驻脚本宿主并执行其全部脚本；脚本改动文档时最多重排 3 轮。
    bool StartFrameScripts(FrameRecord* record);
    // 用当前子引擎的布局结果刷新顶层布局中的内嵌指针。
    void RefreshEmbeddedPointers();
    // 判断节点（含更深层内嵌文档中的节点）是否属于本文档的文档树。
    bool DocumentOwnsNode(const Node* node) const;

    std::unique_ptr<Document> document_;
    StyleSheet stylesheet_;
    StyleResolver resolver_;
    std::unique_ptr<LayoutResult> layout_;
    // 内嵌框架记录：键为 iframe 元素。记录在 Relayout 时整体替换，
    // 未变化的子框架复用原引擎，DOM、脚本宿主与事件状态因此保持稳定。
    std::map<const Element*, std::unique_ptr<FrameRecord>> frames_;
    // 本文档自身的绝对路径（由文件载入时填写），用于循环检测。
    std::string document_path_;
    // 祖先文档的绝对路径链，用于循环检测。
    std::vector<std::string> ancestor_paths_;
    int frame_depth_ = 0;
    bool run_scripts_ = false;
    TextService* text_service_ = nullptr;
    // 顶层文档脚本宿主，不拥有对象，生命周期由调用方管理。
    ScriptHost* top_level_script_ = nullptr;
    std::string base_directory_;
    std::string style_source_;
    float viewport_width_ = 860.0f;
    float page_content_height_ = 1200.0f;
    // CSS 动画时钟（秒），布局前传给样式解析器。
    double animation_time_ = 0.0;
    bool dirty_ = true;
    // SVG 内部变更标记。与 dirty_ 分开记录，Relayout 可以走增量样式刷新，
    // 避免为图元拖动重新排版整个 HTML 文档。
    bool svg_dirty_ = false;
};

}  // namespace web
