#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <imm.h>
#include <objbase.h>
#include <shellapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "web/canvas.h"
#include "web/clipboard.h"
#include "web/engine.h"
#include "web/form_events.h"
#include "web/image.h"
#include "web/paint.h"
#include "web/render.h"
#include "web/script.h"
#include "web/selection.h"
#include "web/text.h"
#include "web/utf8.h"

namespace {

const char* kFallbackHtml = R"HTML(<!DOCTYPE html>
<html><head><meta charset="utf-8"><style>
body { margin: 0; padding: 40px; font-family: "Microsoft YaHei UI"; font-size: 16px; color: #222; background: #fff; }
h1 { font-size: 28px; color: #1a4971; margin: 0 0 12px 0; }
p { line-height: 1.7; margin: 0 0 12px 0; }
</style></head><body>
<h1>自研排版引擎</h1>
<p>这是内置回退示例页面。未找到 samples/article.html 时使用本页验证解析、布局与绘制链路。</p>
</body></html>)HTML";

struct Options {
    std::string html_path;
    std::string out_dir = "web_render_out";
    float width = 900.0f;
    float page_height = 1200.0f;
    int page = 0;  // -1 表示输出全部页
    bool direct3d = true;
    bool opengl = true;
    bool run_script = false;
    bool window = false;
    bool compare = false;
    // 交互模式：直接打开预览窗口（双击可执行文件、拖入 HTML 或传入文件路径时启用）。
    bool interactive = false;
    // 无窗口模式下模拟一次点击：页面坐标，负值表示不点击。
    float click_x = -1.0f;
    float click_y = -1.0f;
    // 无窗口模式下在指定坐标处展开 select 下拉浮层，便于静态校验浮层外观。
    float popup_x = -1.0f;
    float popup_y = -1.0f;
};

float ParseFloat(const std::string& value, float fallback) {
    try {
        return std::stof(value);
    } catch (...) {
        return fallback;
    }
}

int ParseInt(const std::string& value, int fallback) {
    try {
        return std::stoi(value);
    } catch (...) {
        return fallback;
    }
}

void PrintUsage() {
    std::cout << "用法: web_render [选项] [HTML 文件]\n"
                 "  双击可执行文件或传入 HTML 文件路径时直接打开预览窗口。\n"
                 "  --html <路径>       载入 HTML 文件（缺省时使用内置示例）\n"
                 "  --out <目录>        输出 PNG 目录，默认 web_render_out\n"
                 "  --width <像素>      视口宽度，默认 900\n"
                 "  --height <像素>     每页内容高度，默认 1200，<=0 表示不分页\n"
                 "  --page <序号>       输出指定页（从 0 开始，默认 0，-1 表示全部）\n"
                 "  --backend <dx|gl|both>  渲染后端，默认 both\n"
                 "  --js                执行页面中的 <script>\n"
                 "  --click <x,y>       在页面坐标处模拟一次点击并重新布局（需与 --js 同时使用）\n"
                 "  --popup <x,y>       在坐标处命中 select 并展开下拉浮层（用于静态校验）\n"
                 "  --compare           比较两个后端同一页的像素差异\n"
                 "  --window            打开交互预览窗口\n";
}

bool ParseArguments(const std::vector<std::string>& arguments, Options* options,
                    std::string* error) {
    for (size_t index = 1; index < arguments.size(); ++index) {
        const std::string& argument = arguments[index];
        const auto next_value = [&](const std::string& name) -> std::string {
            if (index + 1 >= arguments.size()) {
                if (error != nullptr) {
                    *error = "选项缺少参数: " + name;
                }
                return std::string();
            }
            ++index;
            return arguments[index];
        };
        if (argument == "--html") {
            options->html_path = next_value(argument);
        } else if (argument == "--out") {
            options->out_dir = next_value(argument);
        } else if (argument == "--width") {
            options->width = ParseFloat(next_value(argument), options->width);
        } else if (argument == "--height") {
            options->page_height = ParseFloat(next_value(argument), options->page_height);
        } else if (argument == "--page") {
            options->page = ParseInt(next_value(argument), options->page);
        } else if (argument == "--backend") {
            const std::string value = next_value(argument);
            options->direct3d = value == "dx" || value == "both";
            options->opengl = value == "gl" || value == "both";
            if (!options->direct3d && !options->opengl) {
                if (error != nullptr) {
                    *error = "未知后端: " + value;
                }
                return false;
            }
        } else if (argument == "--js") {
            options->run_script = true;
        } else if (argument == "--click") {
            const std::string value = next_value(argument);
            const size_t comma = value.find(',');
            if (comma == std::string::npos) {
                if (error != nullptr) {
                    *error = "点击坐标格式应为 x,y: " + value;
                }
                return false;
            }
            options->click_x = ParseFloat(value.substr(0, comma), -1.0f);
            options->click_y = ParseFloat(value.substr(comma + 1), -1.0f);
        } else if (argument == "--popup") {
            const std::string value = next_value(argument);
            const size_t comma = value.find(',');
            if (comma == std::string::npos) {
                if (error != nullptr) {
                    *error = "浮层坐标格式应为 x,y: " + value;
                }
                return false;
            }
            options->popup_x = ParseFloat(value.substr(0, comma), -1.0f);
            options->popup_y = ParseFloat(value.substr(comma + 1), -1.0f);
        } else if (argument == "--compare") {
            options->compare = true;
        } else if (argument == "--window") {
            options->window = true;
        } else if (argument == "--help" || argument == "-h") {
            PrintUsage();
            return false;
        } else if (argument.size() > 2 && argument[0] != '-') {
            /* 位置参数：HTML 文件路径（支持把文件拖到可执行文件上）。
               与双击一致，直接以交互预览方式打开。 */
            options->html_path = argument;
            options->window = true;
            options->interactive = true;
        } else if (argument.size() > 2 && argument[0] == '-' && argument[1] != '-') {
            /* 形如 -html 的旧式短选项不识别。 */
            if (error != nullptr) {
                *error = "无法识别的参数: " + argument;
            }
            return false;
        } else {
            if (error != nullptr) {
                *error = "无法识别的参数: " + argument;
            }
            return false;
        }
    }
    return true;
}

/* 可执行文件所在目录（宽字符），用作文件对话框的初始位置。 */
std::wstring ExecutableDirectory() {
    wchar_t module_path[MAX_PATH] = L"";
    if (GetModuleFileNameW(nullptr, module_path, MAX_PATH) == 0) {
        return std::wstring();
    }
    std::wstring path(module_path);
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return std::wstring();
    }
    return path.substr(0, slash);
}

std::string WideToUtf8(const std::wstring& text) {
    if (text.empty()) {
        return std::string();
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                         static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return std::string();
    }
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), result.data(), size,
                        nullptr, nullptr);
    return result;
}

std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty()) {
        return std::wstring();
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                         static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) {
        return std::wstring();
    }
    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), result.data(), size);
    return result;
}

/* 取出 UTF-8 编码的命令行参数。CRT 按本地代码页转换命令行，含中文的路径在
   main 的 argv 中不是 UTF-8；这里改用宽字符命令行重新解析并统一转换为 UTF-8，
   保证文件路径、目录名与文档内部字符串使用同一种编码。 */
std::vector<std::string> Utf8CommandLineArguments() {
    std::vector<std::string> arguments;
    int count = 0;
    LPWSTR* wide_arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (wide_arguments == nullptr) {
        return arguments;
    }
    arguments.reserve(static_cast<size_t>(count));
    for (int index = 0; index < count; ++index) {
        arguments.push_back(WideToUtf8(std::wstring(wide_arguments[index])));
    }
    LocalFree(wide_arguments);
    return arguments;
}

/* 打开文件选择对话框；取消或失败时返回空字符串。 */
std::string OpenHtmlFileDialog() {
    wchar_t file_name[MAX_PATH] = L"";
    const std::wstring initial_directory = ExecutableDirectory();
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = nullptr;
    dialog.lpstrFilter = L"HTML 文件 (*.html;*.htm)\0*.html;*.htm\0所有文件 (*.*)\0*.*\0";
    dialog.lpstrFile = file_name;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrTitle = L"选择要预览的 HTML 文件";
    dialog.lpstrInitialDir = initial_directory.empty() ? nullptr : initial_directory.c_str();
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&dialog) == 0) {
        return std::string();
    }
    return WideToUtf8(std::wstring(file_name));
}

/* 窗口默认尺寸：取工作区的 86%，并限制在合理区间内，避免小屏上窗口超出桌面。 */
void DefaultWindowSize(int* width, int* height) {
    RECT work_area{};
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &work_area, 0) == 0) {
        *width = 1280;
        *height = 900;
        return;
    }
    const int available_width = static_cast<int>(work_area.right - work_area.left);
    const int available_height = static_cast<int>(work_area.bottom - work_area.top);
    int target_width = available_width * 86 / 100;
    int target_height = available_height * 86 / 100;
    if (target_width > 1440) {
        target_width = 1440;
    }
    if (target_height > 1000) {
        target_height = 1000;
    }
    if (target_width < 640) {
        target_width = 640;
    }
    if (target_height < 480) {
        target_height = 480;
    }
    *width = target_width;
    *height = target_height;
}

std::string LocateSampleHtml() {
    const char* candidates[] = {
        "experiments/web_render/samples/article.html",
        "samples/article.html",
        "../../../experiments/web_render/samples/article.html",
        "../../../../experiments/web_render/samples/article.html",
        "../../../../../experiments/web_render/samples/article.html",
    };
    for (const char* candidate : candidates) {
        if (std::filesystem::exists(std::filesystem::u8path(candidate))) {
            return candidate;
        }
    }
    /* 从可执行文件所在目录逐级向上查找工程内的样例目录：
       双击运行时工作目录通常是产物目录，相对路径无法直接命中。 */
    std::wstring directory = ExecutableDirectory();
    for (int level = 0; level < 6 && !directory.empty(); ++level) {
        const std::wstring candidate =
            directory + L"\\experiments\\web_render\\samples\\article.html";
        if (std::filesystem::exists(std::filesystem::path(candidate))) {
            return WideToUtf8(candidate);
        }
        const size_t slash = directory.find_last_of(L"\\/");
        if (slash == std::wstring::npos) {
            break;
        }
        directory = directory.substr(0, slash);
    }
    return std::string();
}

struct RenderOutput {
    bool ok = false;
    std::string error;
    int width = 0;
    int height = 0;
    double milliseconds = 0.0;
    std::vector<uint8_t> pixels;
};

bool RenderPageWithBackend(web::Engine& engine, web::TextService& text, web::RenderBackend backend,
                           int page_index, RenderOutput* output,
                           const web::SelectPopup* select_popup = nullptr) {
    const web::LayoutResult* layout = engine.layout();
    if (layout == nullptr || page_index < 0 || page_index >= layout->PageCount()) {
        output->error = "页码超出范围";
        return false;
    }

    const auto start_time = std::chrono::steady_clock::now();
    std::unique_ptr<web::RenderDevice> device = web::CreateRenderDevice(backend);
    if (!device) {
        output->error = "渲染后端不可用（设备或上下文创建失败）";
        return false;
    }

    int width = static_cast<int>(std::ceil(layout->viewport_width));
    int height = static_cast<int>(
        std::ceil(layout->page_content_height > 0.0f ? layout->page_content_height
                                                     : layout->content_height));
    if (width < 1 || height < 1) {
        output->error = "页面尺寸无效";
        return false;
    }

    if (!device->Initialize(width, height)) {
        output->error = "渲染后端初始化失败";
        return false;
    }

    web::ImageCache images(*device, engine.base_directory());
    device->BeginFrame(web::White());
    web::PaintPage(*layout, page_index, text, *device, images);
    if (select_popup != nullptr && select_popup->open) {
        web::PaintSelectPopup(*layout, page_index, text, *device, *select_popup);
    }
    if (!device->EndFrame()) {
        output->error = "渲染帧提交失败";
        device->Shutdown();
        return false;
    }
    if (!device->ReadPixels(&output->pixels)) {
        output->error = "像素回读失败";
        device->Shutdown();
        return false;
    }

    const auto end_time = std::chrono::steady_clock::now();
    output->milliseconds =
        std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count() / 1000.0;
    output->width = width;
    output->height = height;
    output->ok = true;
    device->Shutdown();
    return true;
}

bool SaveRenderOutput(const std::string& path, const RenderOutput& output, std::string* error) {
    std::filesystem::create_directories(std::filesystem::u8path(path).parent_path());
    return web::SavePng(path, output.width, output.height, output.pixels.data(), error);
}

void CompareOutputs(const RenderOutput& left, const RenderOutput& right) {
    if (!left.ok || !right.ok || left.width != right.width || left.height != right.height) {
        std::cout << "像素比较跳过：输出不完整或尺寸不一致\n";
        return;
    }
    int64_t total_difference = 0;
    int64_t max_difference = 0;
    size_t mismatched_pixels = 0;
    const size_t pixel_count = static_cast<size_t>(left.width) * static_cast<size_t>(left.height);
    for (size_t index = 0; index < pixel_count; ++index) {
        const size_t offset = index * 4;
        int64_t pixel_max = 0;
        for (int channel = 0; channel < 3; ++channel) {
            const int64_t difference =
                std::llabs(static_cast<int64_t>(left.pixels[offset + channel]) -
                           static_cast<int64_t>(right.pixels[offset + channel]));
            pixel_max = std::max(pixel_max, difference);
            total_difference += difference;
        }
        max_difference = std::max(max_difference, pixel_max);
        if (pixel_max > 8) {
            ++mismatched_pixels;
        }
    }
    const double mean_difference =
        static_cast<double>(total_difference) / (static_cast<double>(pixel_count) * 3.0);
    std::cout << "后端像素差异：平均 " << mean_difference << "，最大 " << max_difference
              << "，差值超过 8 的像素比例 "
              << (static_cast<double>(mismatched_pixels) / static_cast<double>(pixel_count) * 100.0)
              << "%\n";
}

const char* BackendName(web::RenderBackend backend) {
    return backend == web::RenderBackend::Direct3D11 ? "dx" : "gl";
}

// 性能诊断开关：设置环境变量 WEB_RENDER_PROFILE 后，交互路径会把各阶段
// 耗时输出到标准输出，便于定位卡顿来源；未设置时没有额外开销。
bool ProfileEnabled() {
    static const bool enabled = std::getenv("WEB_RENDER_PROFILE") != nullptr;
    return enabled;
}

double MillisecondsSince(const std::chrono::steady_clock::time_point& start) {
    const auto now = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(now - start).count();
}

// 在布局树中按元素指针查找布局盒；用于把点击坐标换算到 iframe 子文档坐标系。
const web::LayoutBox* FindLayoutBoxForElement(const web::LayoutBox& box,
                                               const web::Element* element) {
    if (box.element == element) {
        return &box;
    }
    for (const std::unique_ptr<web::LayoutBox>& child : box.children) {
        if (!child) {
            continue;
        }
        const web::LayoutBox* found = FindLayoutBoxForElement(*child, element);
        if (found != nullptr) {
            return found;
        }
    }
    return nullptr;
}

// localStorage 的持久化文件路径：位于本地应用数据目录，文件名由文档绝对路径
// 转换得到，非 ASCII 字节与路径分隔符统一替换为下划线。
std::string LocalStoragePathForDocument(const std::string& document_path) {
    if (document_path.empty()) {
        return std::string();
    }
    const char* local_app_data = std::getenv("LOCALAPPDATA");
    if (local_app_data == nullptr || *local_app_data == '\0') {
        return std::string();
    }
    std::string name;
    name.reserve(document_path.size());
    for (unsigned char byte : document_path) {
        const bool keep = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                          (byte >= '0' && byte <= '9') || byte == '.' || byte == '-' ||
                          byte == '_';
        name.push_back(keep ? static_cast<char>(byte) : '_');
    }
    return std::string(local_app_data) + "\\web_render\\local_storage\\" + name + ".txt";
}

// ---------------------------------------------------------------------------
// 表单控件交互辅助
// ---------------------------------------------------------------------------

// 读取 input 的 type 属性并归一化为小写。
std::string FormControlType(const web::Element& element) {
    std::string type_name = element.GetAttribute("type");
    web::TrimAsciiSpace(&type_name);
    return web::AsciiLower(type_name);
}

// 文本类控件：textarea 与单行输入框可获得键盘焦点；按钮类与勾选类控件不接收文字。
bool IsTextFieldControl(const web::Element* element) {
    if (element == nullptr) {
        return false;
    }
    const std::string& tag_name = element->TagName();
    if (tag_name == "textarea") {
        return true;
    }
    if (tag_name != "input") {
        return false;
    }
    const std::string type_name = FormControlType(*element);
    return type_name != "checkbox" && type_name != "radio" && type_name != "submit" &&
           type_name != "reset" && type_name != "button";
}

// 码位索引转换为 UTF-8 字节偏移。
size_t CodepointByteOffset(const std::string& text, size_t index) {
    size_t offset = 0;
    for (size_t position = 0; position < index && offset < text.size(); ++position) {
        const web::Utf8Char character = web::DecodeUtf8(text, offset);
        if (character.length == 0) {
            break;
        }
        offset += character.length;
    }
    return offset;
}

// 第 index 个码位的字符；越界返回 length = 0。
web::Utf8Char Utf8CharAtIndex(const std::string& text, size_t index) {
    size_t offset = 0;
    size_t current = 0;
    while (offset < text.size()) {
        const web::Utf8Char character = web::DecodeUtf8(text, offset);
        if (character.length == 0) {
            break;
        }
        if (current == index) {
            return character;
        }
        offset += character.length;
        ++current;
    }
    return web::Utf8Char{};
}

// 按样式的字体族、字号与字重创建（或复用）字体句柄。
web::FontId FontForStyle(web::TextService& text_service, const web::ComputedStyle& style) {
    web::FontRequest request;
    request.family = style.font_family;
    request.size = style.font_size > 0.0f ? style.font_size : 16.0f;
    request.bold = style.font_weight >= 600;
    request.italic = style.italic;
    return text_service.CreateFont(request);
}

// 文本控件实际使用的字体：优先取行盒中已排版的字体。
web::FontId FontForControl(const web::LayoutBox& box, web::TextService& text_service) {
    for (const web::LineFragment& line : box.lines) {
        for (const web::PlacedText& placed : line.texts) {
            if (placed.font != 0) {
                return placed.font;
            }
        }
    }
    return FontForStyle(text_service, box.style);
}

// 在布局结果（含内嵌子布局）中按元素指针查找布局盒。
const web::LayoutBox* FindBoxInLayout(const web::LayoutResult& layout,
                                       const web::Element* element) {
    if (layout.root) {
        const web::LayoutBox* box = FindLayoutBoxForElement(*layout.root, element);
        if (box != nullptr) {
            return box;
        }
    }
    for (const auto& entry : layout.embedded_frames) {
        if (entry.second == nullptr) {
            continue;
        }
        const web::LayoutBox* box = FindBoxInLayout(*entry.second, element);
        if (box != nullptr) {
            return box;
        }
    }
    return nullptr;
}

// 在布局结果中查找元素的布局盒，并把页面坐标换算为元素所在文档的坐标。
// offset_y 记录内嵌文档逐层换算后的文档坐标 y。
bool FindBoxAndPoint(const web::LayoutResult& layout, const web::Element* element, float page_x,
                     float page_y, float page_top, const web::LayoutBox** out_box,
                     float* out_x, float* out_y) {
    if (layout.root) {
        const web::LayoutBox* box = FindLayoutBoxForElement(*layout.root, element);
        if (box != nullptr) {
            *out_box = box;
            *out_x = page_x;
            *out_y = page_y + page_top;
            return true;
        }
    }
    for (const auto& entry : layout.embedded_frames) {
        if (entry.second == nullptr || layout.root == nullptr) {
            continue;
        }
        const web::LayoutBox* frame_box = FindLayoutBoxForElement(*layout.root, entry.first);
        if (frame_box == nullptr) {
            continue;
        }
        const float child_x = page_x - frame_box->content_box.left;
        const float child_y = page_y - (frame_box->content_box.top - page_top);
        if (FindBoxAndPoint(*entry.second, element, child_x, child_y, 0.0f, out_box, out_x,
                            out_y)) {
            return true;
        }
    }
    return false;
}

// 按点击位置估算光标码位索引：横向按字符步进累加，textarea 先按行盒定位行。
size_t CaretIndexFromPoint(const web::LayoutBox& box, const web::Element& element,
                           float document_x, float document_y, float scroll_x, float scroll_y,
                           web::TextService& text_service) {
    const std::string text = web::FormControlText(element);
    const bool is_textarea = web::AsciiLower(element.TagName()) == "textarea";
    const web::FontId font = FontForControl(box, text_service);
    const float letter_spacing = box.style.letter_spacing;

    if (!is_textarea) {
        const std::string display = web::FormDisplayText(element);
        const float local_x = document_x - box.content_box.left + scroll_x;
        if (local_x <= 0.0f) {
            return 0;
        }
        float pen_x = 0.0f;
        size_t index = 0;
        while (index < display.size()) {
            const web::Utf8Char character = Utf8CharAtIndex(display, index);
            if (character.length == 0) {
                break;
            }
            const float advance =
                text_service.Advance(font, character.codepoint) + letter_spacing;
            if (local_x < pen_x + advance) {
                return index;
            }
            pen_x += advance;
            ++index;
        }
        return index;
    }

    if (box.lines.empty()) {
        return 0;
    }
    const float local_y = document_y - box.content_box.top + scroll_y;
    size_t line_index = 0;
    bool found_line = false;
    for (size_t index = 0; index < box.lines.size(); ++index) {
        const float top = box.lines[index].rect.top - box.content_box.top;
        const float bottom = box.lines[index].rect.bottom - box.content_box.top;
        if (local_y >= top && local_y < bottom) {
            line_index = index;
            found_line = true;
            break;
        }
    }
    if (!found_line) {
        const float first_top = box.lines.front().rect.top - box.content_box.top;
        line_index = local_y < first_top ? 0 : box.lines.size() - 1;
    }

    const web::FormLineRange range =
        web::GetFormLineRange(box, text, line_index, text_service);
    if (!range.valid) {
        return 0;
    }
    const float local_x = document_x - box.content_box.left;
    float pen_x = 0.0f;
    size_t count = 0;
    for (size_t index = range.begin_index; index < range.end_index; ++index) {
        const web::Utf8Char character = Utf8CharAtIndex(text, index);
        if (character.length == 0) {
            break;
        }
        const float advance =
            text_service.Advance(font, character.codepoint) + letter_spacing;
        if (local_x < pen_x + advance) {
            break;
        }
        pen_x += advance;
        ++count;
    }
    return range.begin_index + count;
}

// 按文档顺序收集可获得键盘焦点的文本控件，跳过带 disabled 属性的控件。
void CollectTextControls(const web::Node& node, std::vector<const web::Element*>* output) {
    for (const web::NodePtr& child : node.Children()) {
        if (!child || !child->IsElement()) {
            continue;
        }
        const web::Element& element = static_cast<const web::Element&>(*child);
        if (IsTextFieldControl(&element) && !element.HasAttribute("disabled")) {
            output->push_back(&element);
        }
        CollectTextControls(element, output);
    }
}

class PreviewWindow {
public:
    PreviewWindow(web::Engine& engine, web::TextService& text, web::ScriptHost* script,
                  web::RenderBackend backend)
        : engine_(engine), text_(text), script_(script), backend_(backend) {}

    bool Create(int width, int height, const std::wstring& title) {
        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
        window_class.lpfnWndProc = &PreviewWindow::WindowProcedure;
        window_class.hInstance = GetModuleHandleW(nullptr);
        window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        window_class.lpszClassName = L"WebRenderPreviewWindow";
        RegisterClassExW(&window_class);

        hwnd_ = CreateWindowExW(0, window_class.lpszClassName,
                                title.empty() ? L"自研排版引擎预览" : title.c_str(),
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, width, height,
                                nullptr, nullptr, window_class.hInstance, this);
        if (hwnd_ == nullptr) {
            return false;
        }
        // 记录表单控件的初始值，供表单重置恢复。
        form_defaults_.Capture(engine_.document());
        ShowWindow(hwnd_, SW_SHOW);
        UpdateWindow(hwnd_);
        SetTimer(hwnd_, 1, 16, nullptr);
        // 光标闪烁：每 500ms 切换一次可见状态并重绘。
        SetTimer(hwnd_, 2, 500, nullptr);
        return true;
    }

    int Run() {
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return static_cast<int>(message.wParam);
    }

private:
    static LRESULT CALLBACK WindowProcedure(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        PreviewWindow* self = reinterpret_cast<PreviewWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            CREATESTRUCTW* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            self = reinterpret_cast<PreviewWindow*>(create->lpCreateParams);
            self->hwnd_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (self == nullptr) {
            return DefWindowProcW(hwnd, message, wparam, lparam);
        }
        return self->HandleMessage(message, wparam, lparam);
    }

    LRESULT HandleMessage(UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
            case WM_SIZE:
                OnResize(LOWORD(lparam), HIWORD(lparam));
                return 0;
            case WM_ENTERSIZEMOVE:
                // 进入窗口边框拖动缩放：按帧节流重排，重排间隙保留上一帧原尺寸。
                resizing_ = true;
                return 0;
            case WM_EXITSIZEMOVE:
                resizing_ = false;
                KillTimer(hwnd_, 3);
                resize_timer_active_ = false;
                ApplyPendingResize();
                return 0;
            case WM_ERASEBKGND:
                // 每帧整块重绘客户区，跳过背景擦除以避免闪烁。
                return 1;
            case WM_PAINT: {
                PAINTSTRUCT paint{};
                HDC device_context = BeginPaint(hwnd_, &paint);
                Draw(device_context);
                EndPaint(hwnd_, &paint);
                return 0;
            }
            case WM_KEYDOWN:
                OnKeyDown(wparam);
                return 0;
            case WM_KEYUP:
                OnKeyUp(wparam);
                return 0;
            case WM_CHAR:
                OnChar(wparam);
                return 0;
            case WM_LBUTTONDOWN:
                OnMouseDown(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
                return 0;
            case WM_MOUSEMOVE:
                OnMouseMove(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
                return 0;
            case WM_MOUSELEAVE:
                UpdateHover(nullptr);
                return 0;
            case WM_LBUTTONUP:
                OnMouseUp(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
                return 0;
            case WM_MBUTTONDOWN:
                DispatchPointerButton(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam), 1, true);
                return 0;
            case WM_MBUTTONUP:
                DispatchPointerButton(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam), 1, false);
                return 0;
            case WM_RBUTTONDOWN:
                DispatchContextMenu(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
                return 0;
            case WM_CAPTURECHANGED:
                // 捕获被其他窗口取走时结束滚动条拖动，避免鼠标在窗口外松开后
                // 仍按拖动状态跟随指针。
                if (scrollbar_capture_ || scrollbar_dragging_) {
                    scrollbar_capture_ = false;
                    scrollbar_dragging_ = false;
                    InvalidateRect(hwnd_, nullptr, FALSE);
                }
                return 0;
            case WM_MOUSEWHEEL: {
                // 滚轮的坐标是屏幕坐标，先换算为客户区坐标再派发 wheel 事件。
                POINT point = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                ScreenToClient(hwnd_, &point);
                DispatchWheel(point.x, point.y, GET_WHEEL_DELTA_WPARAM(wparam));
                return 0;
            }
            case WM_TIMER:
                OnTimer(static_cast<UINT_PTR>(wparam));
                return 0;
            case WM_IME_SETCONTEXT:
                // 组合串由控件内部绘制，隐藏系统自带的组合窗口；候选窗口保留。
                if (wparam == TRUE) {
                    lparam &= ~static_cast<LPARAM>(ISC_SHOWUICOMPOSITIONWINDOW);
                }
                return DefWindowProcW(hwnd_, message, wparam, lparam);
            case WM_IME_STARTCOMPOSITION:
                OnImeStartComposition();
                return 0;
            case WM_IME_COMPOSITION:
                OnImeComposition(static_cast<LPARAM>(lparam));
                return 0;
            case WM_IME_ENDCOMPOSITION:
                OnImeEndComposition();
                return 0;
            case WM_IME_NOTIFY:
                if (wparam == IMN_OPENCANDIDATE || wparam == IMN_CHANGECANDIDATE) {
                    PositionImeWindows();
                }
                return DefWindowProcW(hwnd_, message, wparam, lparam);
            case WM_DESTROY:
                KillTimer(hwnd_, 1);
                KillTimer(hwnd_, 2);
                KillTimer(hwnd_, 3);
                device_.reset();
                PostQuitMessage(0);
                return 0;
            default:
                return DefWindowProcW(hwnd_, message, wparam, lparam);
        }
    }

    // 客户区尺寸变化：非拖动状态下立即应用；拖动过程中只记录尺寸并请求重绘，
    // 由定时器在鼠标停稳后统一重排，避免每个 WM_SIZE 都完整重排文档。
    void OnResize(int width, int height) {
        if (width < 1 || height < 1) {
            return;
        }
        pending_width_ = width;
        pending_height_ = height;
        client_height_ = height;
        resize_pending_ = true;
        if (!resizing_) {
            ApplyPendingResize();
            return;
        }
        // 拖动期间按帧节流：定时器已在等待时不重置，否则连续的 WM_SIZE 会把
        // 重排一直推迟到鼠标停下，只剩拉伸的旧画面。
        if (!resize_timer_active_) {
            resize_timer_active_ = true;
            SetTimer(hwnd_, 3, kResizeThrottleMs, nullptr);
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    // 应用挂起的客户区尺寸：重排文档并让当前页落在有效范围内。
    void ApplyPendingResize() {
        if (!resize_pending_) {
            return;
        }
        resize_pending_ = false;
        const int width = pending_width_;
        const int height = pending_height_;
        const auto layout_start = std::chrono::steady_clock::now();
        engine_.SetViewport(static_cast<float>(width), static_cast<float>(height));
        if (!engine_.Relayout(nullptr)) {
            return;
        }
        ClampScroll();
        if (ProfileEnabled()) {
            std::cout << "PROF resize layout=" << MillisecondsSince(layout_start)
                      << " size=" << width << "x" << height << "\n";
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    bool EnsureLayout() {
        if (!engine_.dirty() && engine_.layout() != nullptr) {
            return true;
        }
        if (!engine_.Relayout(nullptr)) {
            return false;
        }
        return engine_.layout() != nullptr;
    }

    // -----------------------------------------------------------------------
    // 垂直滚动：布局仍按页切分，视口以像素为单位在文档内滑动。视口跨越页
    // 边界时分别渲染相邻两页，再按滚动偏移拼接到客户区位图。
    // -----------------------------------------------------------------------

    static constexpr float kWheelScrollStep = 72.0f;
    static constexpr float kKeyboardScrollStep = 48.0f;

    // 可滚动范围：内容高度减去客户区高度。
    float MaxScroll() const {
        const web::LayoutResult* layout = engine_.layout();
        if (layout == nullptr) {
            return 0.0f;
        }
        const float viewport = static_cast<float>(std::max(client_height_, 1));
        return std::max(0.0f, layout->content_height - viewport);
    }

    // 滚动偏移所在的页：取最后一个起点不大于偏移的页。
    int PageIndexForScroll(const web::LayoutResult& layout) const {
        int page = 0;
        const int count = layout.PageCount();
        for (int index = 0; index < count; ++index) {
            if (layout.page_starts[static_cast<size_t>(index)] <= scroll_y_ + 0.5f) {
                page = index;
            } else {
                break;
            }
        }
        return page;
    }

    // 把滚动偏移与当前页收敛到合法范围；布局变化后也调用。
    void ClampScroll() {
        const web::LayoutResult* layout = engine_.layout();
        if (layout == nullptr || layout->PageCount() == 0) {
            scroll_y_ = 0.0f;
            page_ = 0;
            return;
        }
        scroll_y_ = std::clamp(scroll_y_, 0.0f, MaxScroll());
        page_ = PageIndexForScroll(*layout);
    }

    // 客户区纵向坐标到页面坐标的偏移量。
    float PageYOffset() const {
        const web::LayoutResult* layout = engine_.layout();
        if (layout == nullptr || layout->page_starts.empty()) {
            return scroll_y_;
        }
        const int count = layout->PageCount();
        const int index = std::clamp(page_, 0, count - 1);
        return scroll_y_ - layout->page_starts[static_cast<size_t>(index)];
    }

    void ScrollTo(float y) {
        if (!EnsureLayout()) {
            return;
        }
        const float target = std::clamp(y, 0.0f, MaxScroll());
        if (std::fabs(target - scroll_y_) < 0.01f) {
            return;
        }
        scroll_y_ = target;
        ClampScroll();
        // 滚动后光标位置变化，正在组合时同步输入法窗口位置。
        if (ime_composing_) {
            PositionImeWindows();
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void ScrollBy(float delta) { ScrollTo(scroll_y_ + delta); }

    // 视口底边越过下一页起点时返回下一页序号，否则返回 -1。
    int NextVisiblePage(const web::LayoutResult& layout, int page) const {
        if (page + 1 >= layout.PageCount()) {
            return -1;
        }
        const float viewport_bottom = scroll_y_ + static_cast<float>(std::max(client_height_, 1));
        if (viewport_bottom <= layout.page_starts[static_cast<size_t>(page + 1)] + 0.01f) {
            return -1;
        }
        return page + 1;
    }

    // 渲染一页到 RGBA 像素缓冲；paint_ms 与 readback_ms 累加各页耗时。
    bool RenderPagePixels(const web::LayoutResult& layout, int page_index,
                          std::vector<uint8_t>* pixels, double* paint_ms, double* readback_ms) {
        const auto paint_start = std::chrono::steady_clock::now();
        device_->BeginFrame(web::White());
        web::PaintPage(layout, page_index, text_, *device_, *images_, &focus_state_, &selection_);
        if (select_popup_.open && page_index == page_) {
            web::PaintSelectPopup(layout, page_index, text_, *device_, select_popup_);
        }
        const bool submitted = device_->EndFrame();
        *paint_ms += MillisecondsSince(paint_start);
        if (!submitted) {
            return false;
        }
        const auto readback_start = std::chrono::steady_clock::now();
        const bool read = device_->ReadPixels(pixels);
        *readback_ms += MillisecondsSince(readback_start);
        return read;
    }

    // 按滚动偏移把两页像素拼到客户区位图，并把 RGBA 转为 GDI 的 BGRA。
    void ComposeFrame(const web::LayoutResult& layout, int width, int height, int page_a,
                      const std::vector<uint8_t>& page_a_pixels, int page_b,
                      const std::vector<uint8_t>& page_b_pixels) {
        const size_t pixel_count = static_cast<size_t>(width) * static_cast<size_t>(height);
        if (dib_pixels_.size() != pixel_count * 4) {
            dib_pixels_.resize(pixel_count * 4);
        }
        const size_t stride = static_cast<size_t>(width) * 4;
        const float page_a_top = layout.page_starts[static_cast<size_t>(page_a)];
        const float page_b_top =
            page_b >= 0 ? layout.page_starts[static_cast<size_t>(page_b)] : 0.0f;
        for (int y = 0; y < height; ++y) {
            const float document_y = scroll_y_ + static_cast<float>(y);
            const std::vector<uint8_t>* source = &page_a_pixels;
            float source_top = page_a_top;
            if (page_b >= 0 && document_y >= page_b_top) {
                source = &page_b_pixels;
                source_top = page_b_top;
            }
            int row = static_cast<int>(document_y - source_top);
            row = std::clamp(row, 0, height - 1);
            const uint8_t* input = source->data() + static_cast<size_t>(row) * stride;
            uint8_t* output = dib_pixels_.data() + static_cast<size_t>(y) * stride;
            for (int x = 0; x < width; ++x) {
                const size_t offset = static_cast<size_t>(x) * 4;
                output[offset] = input[offset + 2];
                output[offset + 1] = input[offset + 1];
                output[offset + 2] = input[offset];
                output[offset + 3] = input[offset + 3];
            }
        }
    }

    // 滚动条几何：轨道贴右边缘，滑块高度与视口、内容高度之比成正比。
    void UpdateScrollbarGeometry(int width, int height) {
        const float maximum = MaxScroll();
        scrollbar_visible_ = maximum > 0.5f;
        if (!scrollbar_visible_) {
            scrollbar_track_ = web::RectF();
            scrollbar_thumb_ = web::RectF();
            return;
        }
        const float left = static_cast<float>(width) - 11.0f;
        const float right = static_cast<float>(width) - 3.0f;
        scrollbar_track_ =
            web::RectF::FromLTRB(left, 2.0f, right, static_cast<float>(height) - 2.0f);
        const float track_height = scrollbar_track_.Height();
        const web::LayoutResult* layout = engine_.layout();
        const float content =
            layout != nullptr ? std::max(layout->content_height, static_cast<float>(height))
                              : static_cast<float>(height);
        float thumb_height = track_height * static_cast<float>(height) / content;
        thumb_height = std::clamp(thumb_height, 28.0f, track_height);
        const float travel = track_height - thumb_height;
        const float ratio = maximum > 0.0f ? scroll_y_ / maximum : 0.0f;
        const float top = scrollbar_track_.top + travel * std::clamp(ratio, 0.0f, 1.0f);
        scrollbar_thumb_ = web::RectF::FromLTRB(left, top, right, top + thumb_height);
    }

    void FillDibRect(int width, int height, const web::RectF& rect, uint8_t red, uint8_t green,
                     uint8_t blue) {
        int left = static_cast<int>(std::floor(rect.left));
        int top = static_cast<int>(std::floor(rect.top));
        int right = static_cast<int>(std::ceil(rect.right));
        int bottom = static_cast<int>(std::ceil(rect.bottom));
        left = std::clamp(left, 0, width);
        right = std::clamp(right, 0, width);
        top = std::clamp(top, 0, height);
        bottom = std::clamp(bottom, 0, height);
        for (int y = top; y < bottom; ++y) {
            uint8_t* row = dib_pixels_.data() + static_cast<size_t>(y) * static_cast<size_t>(width) * 4;
            for (int x = left; x < right; ++x) {
                uint8_t* pixel = row + static_cast<size_t>(x) * 4;
                pixel[0] = blue;
                pixel[1] = green;
                pixel[2] = red;
                pixel[3] = 255;
            }
        }
    }

    void DrawScrollbarOverlay(int width, int height) {
        if (!scrollbar_visible_) {
            return;
        }
        FillDibRect(width, height, scrollbar_track_, 238, 238, 238);
        const bool active = scrollbar_dragging_ || scrollbar_capture_;
        const uint8_t level = active ? 118 : 158;
        FillDibRect(width, height, scrollbar_thumb_, level, level, level);
    }

    // 把滑块顶端映射回滚动偏移。
    void ScrollToThumb(float thumb_top) {
        const float travel = scrollbar_track_.Height() - scrollbar_thumb_.Height();
        if (travel <= 0.0f) {
            return;
        }
        const float ratio = std::clamp((thumb_top - scrollbar_track_.top) / travel, 0.0f, 1.0f);
        ScrollTo(ratio * MaxScroll());
    }

    // 把上一帧按原像素尺寸贴到客户区左上角。拖动缩放期间新尺寸尚未重排完成，
    // 用上一帧保持画面连续；不缩放位图，避免拖动过程中出现明显拉伸变形。
    // 新增区域取上一帧左下角像素作为页面底色填充，深色页面不会闪白。
    void PresentFrame(HDC device_context, int width, int height) {
        if (!frame_valid_ || frame_width_ < 1 || frame_height_ < 1 || dib_pixels_.empty()) {
            return;
        }
        if (width != frame_width_ || height != frame_height_) {
            const uint8_t* last_row =
                dib_pixels_.data() +
                static_cast<size_t>(frame_height_ - 1) * static_cast<size_t>(frame_width_) * 4;
            HBRUSH brush = CreateSolidBrush(RGB(last_row[2], last_row[1], last_row[0]));
            RECT client{0, 0, width, height};
            FillRect(device_context, &client, brush);
            DeleteObject(brush);
        }
        BITMAPINFO bitmap_info{};
        bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bitmap_info.bmiHeader.biWidth = frame_width_;
        bitmap_info.bmiHeader.biHeight = -frame_height_;
        bitmap_info.bmiHeader.biPlanes = 1;
        bitmap_info.bmiHeader.biBitCount = 32;
        bitmap_info.bmiHeader.biCompression = BI_RGB;
        StretchDIBits(device_context, 0, 0, frame_width_, frame_height_, 0, 0, frame_width_,
                      frame_height_, dib_pixels_.data(), &bitmap_info, DIB_RGB_COLORS, SRCCOPY);
    }

    void Draw(HDC device_context) {
        RECT client{};
        GetClientRect(hwnd_, &client);
        const int width = client.right - client.left;
        const int height = client.bottom - client.top;
        if (width < 1 || height < 1) {
            return;
        }

        // 拖动缩放尚未重排：直接拉伸上一帧，避免每个 WM_SIZE 都重建布局与渲染目标。
        if (resize_pending_) {
            PresentFrame(device_context, width, height);
            return;
        }

        client_height_ = height;
        if (!EnsureLayout()) {
            return;
        }
        const web::LayoutResult* layout = engine_.layout();
        if (layout == nullptr || layout->PageCount() == 0) {
            return;
        }
        ClampScroll();

        if (!device_) {
            device_ = web::CreateRenderDevice(backend_);
            if (!device_ || !device_->Initialize(width, height)) {
                device_.reset();
                return;
            }
            // 新设备的纹理句柄全部失效，图片缓存必须重建。
            images_.reset();
            device_width_ = width;
            device_height_ = height;
        } else if (device_width_ != width || device_height_ != height) {
            // 复用同一设备，只重建渲染目标；已上传的纹理继续有效。
            if (device_->Resize(width, height)) {
                device_width_ = width;
                device_height_ = height;
            } else {
                device_.reset();
                images_.reset();
                InvalidateRect(hwnd_, nullptr, FALSE);
                return;
            }
        }
        if (!images_) {
            images_ = std::make_unique<web::ImageCache>(*device_, engine_.base_directory());
        }

        const int page_a = page_;
        const int page_b = NextVisiblePage(*layout, page_a);
        double paint_ms = 0.0;
        double readback_ms = 0.0;
        if (!RenderPagePixels(*layout, page_a, &page_pixels_a_, &paint_ms, &readback_ms)) {
            return;
        }
        if (page_b >= 0 &&
            !RenderPagePixels(*layout, page_b, &page_pixels_b_, &paint_ms, &readback_ms)) {
            return;
        }
        ComposeFrame(*layout, width, height, page_a, page_pixels_a_, page_b, page_pixels_b_);
        UpdateScrollbarGeometry(width, height);
        DrawScrollbarOverlay(width, height);
        frame_width_ = width;
        frame_height_ = height;
        frame_valid_ = true;
        const auto present_start = std::chrono::steady_clock::now();
        PresentFrame(device_context, width, height);
        if (ProfileEnabled()) {
            std::cout << "PROF paint paint=" << paint_ms << " readback=" << readback_ms
                      << " present=" << MillisecondsSince(present_start)
                      << " pages=" << (page_b >= 0 ? 2 : 1)
                      << " scroll=" << scroll_y_ << "\n";
        }
    }

    // 当前焦点元素的所属文档；无焦点时返回顶层文档。
    web::Document* FocusDocument() const {
        if (focus_state_.element != nullptr) {
            web::Document* document = focus_state_.element->OwnerDocument();
            if (document != nullptr) {
                return document;
            }
        }
        return &engine_.document();
    }

    bool FocusedTextControl() const {
        return IsTextFieldControl(focus_state_.element) &&
               !focus_state_.element->HasAttribute("disabled");
    }

    const web::LayoutBox* FocusedBox() const {
        const web::LayoutResult* layout = engine_.layout();
        if (layout == nullptr || focus_state_.element == nullptr) {
            return nullptr;
        }
        return FindBoxInLayout(*layout, focus_state_.element);
    }

    // 派发事件到元素，并在事件处理可能改动文档后重新布局；
    // 元素属于内嵌文档时通过其 iframe 重新布局子文档并刷新内嵌指针。
    void DispatchEventTo(const web::Element* element, const std::string& type,
                         const web::ScriptEventInit& init) {
        if (element == nullptr) {
            return;
        }
        const web::Element* frame_element = engine_.FrameElementForNode(*element);
        engine_.DispatchEvent(element, type, init);
        if (frame_element != nullptr) {
            engine_.RelayoutFrame(*frame_element);
        } else if (engine_.dirty()) {
            engine_.Relayout(nullptr);
        }
    }

    // 递归清除元素子树的交互状态位：bits 1 为 hover，2 为 active，4 为 focus。
    static void ClearElementState(web::Element& element, int bits) {
        if ((bits & 1) != 0) {
            element.hovered = false;
        }
        if ((bits & 2) != 0) {
            element.active = false;
        }
        if ((bits & 4) != 0) {
            element.focused = false;
        }
        for (const web::NodePtr& child : element.Children()) {
            if (child && child->IsElement()) {
                ClearElementState(static_cast<web::Element&>(*child), bits);
            }
        }
    }

    void ClearDocumentState(int bits) {
        web::Element* root = engine_.document().RootElement();
        if (root != nullptr) {
            ClearElementState(*root, bits);
        }
    }

    // 悬停目标变化时更新 :hover 状态并重新排版。
    void UpdateHover(const web::Element* target) {
        if (target == hovered_element_) {
            return;
        }
        hovered_element_ = target;
        ClearDocumentState(1);
        for (web::Element* current = const_cast<web::Element*>(target); current != nullptr;
             current = current->ParentElement()) {
            current->hovered = true;
        }
        engine_.MarkDirty();
        engine_.Relayout(nullptr);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    // 指针按下：设置命中元素及其祖先的 :active 状态并重新排版。
    void SetActiveTarget(const web::Element* target) {
        ClearDocumentState(2);
        for (web::Element* current = const_cast<web::Element*>(target); current != nullptr;
             current = current->ParentElement()) {
            current->active = true;
        }
        active_target_set_ = target != nullptr;
        engine_.MarkDirty();
        engine_.Relayout(nullptr);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    // 切换焦点：旧的文本控件先派发 change（若有编辑）与 blur，新控件派发 focus。
    void SetFocus(const web::Element* element) {
        if (focus_state_.element == element) {
            return;
        }
        // 切换控件时丢弃未提交的组合串，避免其写入新控件。
        if (ime_composing_) {
            ime_composing_ = false;
            ime_composition_.clear();
        }
        // 同步 :focus 状态：旧元素清除，新元素设置。
        if (focus_state_.element != nullptr) {
            const_cast<web::Element*>(focus_state_.element)->focused = false;
        }
        if (element != nullptr) {
            const_cast<web::Element*>(element)->focused = true;
        }
        if (focus_state_.element != nullptr) {
            if (focus_text_edited_) {
                DispatchEventTo(focus_state_.element, "change", web::ScriptEventInit{});
            }
            DispatchEventTo(focus_state_.element, "blur", web::ScriptEventInit{});
        }
        focus_text_edited_ = false;
        focus_state_.element = element;
        focus_state_.scroll_x = 0.0f;
        focus_state_.scroll_y = 0.0f;
        focus_state_.caret_visible = true;
        if (element != nullptr) {
            const size_t length = web::Utf8Length(web::FormControlText(*element));
            focus_state_.caret = length;
            focus_state_.anchor = length;
            DispatchEventTo(element, "focus", web::ScriptEventInit{});
        }
        // 焦点变化会影响 :focus / :focus-within 样式，需要重新匹配与排版。
        engine_.MarkDirty();
        engine_.Relayout(nullptr);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    std::string FocusedText() const {
        if (focus_state_.element == nullptr) {
            return std::string();
        }
        return web::FormControlText(*focus_state_.element);
    }

    // 写回焦点控件文本：textarea 替换文本子节点，input 写 value 属性。
    void SetFocusedText(const std::string& text) {
        if (focus_state_.element == nullptr) {
            return;
        }
        web::Element& element = const_cast<web::Element&>(*focus_state_.element);
        web::Document* document = element.OwnerDocument();
        if (document == nullptr) {
            document = &engine_.document();
        }
        if (web::AsciiLower(element.TagName()) == "textarea") {
            std::vector<web::NodePtr> children;
            children.push_back(document->CreateTextNode(text));
            document->ReplaceChildren(element, std::move(children));
        } else {
            element.SetAttribute("value", text);
        }
    }

    void RelayoutFocusedDocument() {
        if (focus_state_.element == nullptr) {
            return;
        }
        if (const web::Element* frame = engine_.FrameElementForNode(*focus_state_.element)) {
            engine_.RelayoutFrame(*frame);
        } else {
            engine_.MarkDirty();
            engine_.Relayout(nullptr);
        }
    }

    void ClampFocusCaret() {
        if (focus_state_.element == nullptr) {
            return;
        }
        const size_t length = web::Utf8Length(FocusedText());
        if (focus_state_.caret > length) {
            focus_state_.caret = length;
        }
        if (focus_state_.anchor > length) {
            focus_state_.anchor = length;
        }
    }

    // 光标前缀宽度：按字体步进逐码位累加，textarea 使用真实文本，单行使用显示文本。
    float DisplayPrefixWidth(const web::LayoutBox& box, const std::string& display,
                             size_t count) {
        const web::FontId font = FontForControl(box, text_);
        float width = 0.0f;
        for (size_t index = 0; index < count; ++index) {
            const web::Utf8Char character = Utf8CharAtIndex(display, index);
            if (character.length == 0) {
                break;
            }
            width += text_.Advance(font, character.codepoint) + box.style.letter_spacing;
        }
        return width;
    }

    // 按光标位置更新滚动偏移，保证光标位于内容盒内。
    void UpdateFocusScroll() {
        const web::Element* element = focus_state_.element;
        const web::LayoutBox* box = FocusedBox();
        if (element == nullptr || box == nullptr) {
            return;
        }
        const std::string text = web::FormControlText(*element);
        const bool is_textarea = web::AsciiLower(element->TagName()) == "textarea";
        const float content_width = box->content_box.Width();
        const float content_height = box->content_box.Height();
        if (content_width <= 0.0f || content_height <= 0.0f) {
            return;
        }

        if (is_textarea) {
            const web::FormCaretPosition position =
                web::LocateFormCaret(*box, text, focus_state_.caret, text_);
            if (position.valid) {
                const float caret_top = position.line_top - box->content_box.top;
                const float caret_bottom = position.line_bottom - box->content_box.top;
                if (caret_top - focus_state_.scroll_y < 0.0f) {
                    focus_state_.scroll_y = caret_top;
                }
                if (caret_bottom - focus_state_.scroll_y > content_height) {
                    focus_state_.scroll_y = caret_bottom - content_height;
                }
            }
            const float total_height = box->lines.empty()
                                           ? 0.0f
                                           : box->lines.back().rect.bottom - box->content_box.top;
            const float maximum_scroll = std::max(0.0f, total_height - content_height);
            if (focus_state_.scroll_y < 0.0f) {
                focus_state_.scroll_y = 0.0f;
            }
            if (focus_state_.scroll_y > maximum_scroll) {
                focus_state_.scroll_y = maximum_scroll;
            }
            focus_state_.scroll_x = 0.0f;
            return;
        }

        const std::string display = web::FormDisplayText(*element);
        const float caret_x = DisplayPrefixWidth(*box, display, focus_state_.caret);
        if (caret_x - focus_state_.scroll_x < 0.0f) {
            focus_state_.scroll_x = caret_x;
        }
        if (caret_x - focus_state_.scroll_x > content_width - 1.0f) {
            focus_state_.scroll_x = caret_x - content_width + 1.0f;
        }
        const float total_width = DisplayPrefixWidth(*box, display, web::Utf8Length(display));
        const float maximum_scroll = std::max(0.0f, total_width - content_width);
        if (focus_state_.scroll_x < 0.0f) {
            focus_state_.scroll_x = 0.0f;
        }
        if (focus_state_.scroll_x > maximum_scroll) {
            focus_state_.scroll_x = maximum_scroll;
        }
    }

    // 文本改动后重新布局、调整滚动并派发 input 事件（脚本可能再次改动文档）。
    void AfterTextEdit() {
        focus_text_edited_ = true;
        focus_state_.caret_visible = true;
        ClampFocusCaret();
        RelayoutFocusedDocument();
        UpdateFocusScroll();
        const web::Element* element = focus_state_.element;
        if (element != nullptr) {
            DispatchEventTo(element, "input", web::ScriptEventInit{});
            UpdateFocusScroll();
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    // 光标在文档坐标中的位置；textarea 使用行盒，单行输入框使用内容区。
    bool CaretDocumentPosition(float* x, float* top, float* bottom) {
        if (!FocusedTextControl()) {
            return false;
        }
        const web::LayoutBox* box = FocusedBox();
        if (box == nullptr) {
            return false;
        }
        const std::string text = FocusedText();
        if (web::AsciiLower(focus_state_.element->TagName()) == "textarea") {
            const web::FormCaretPosition position =
                web::LocateFormCaret(*box, text, focus_state_.caret, text_);
            if (!position.valid) {
                return false;
            }
            *x = box->content_box.left + position.x - focus_state_.scroll_x;
            *top = position.line_top - focus_state_.scroll_y;
            *bottom = position.line_bottom - focus_state_.scroll_y;
            return true;
        }
        const std::string display = web::FormDisplayText(*focus_state_.element);
        const float caret_x = DisplayPrefixWidth(*box, display, focus_state_.caret);
        *x = box->content_box.left + caret_x - focus_state_.scroll_x;
        *top = box->content_box.top;
        *bottom = box->content_box.bottom;
        return true;
    }

    // 按光标位置设置输入法组合窗口与候选窗口，客户区坐标换算为屏幕坐标。
    void PositionImeWindows() {
        float x = 0.0f;
        float top = 0.0f;
        float bottom = 0.0f;
        if (!CaretDocumentPosition(&x, &top, &bottom)) {
            return;
        }
        POINT point;
        point.x = static_cast<LONG>(std::lround(x));
        point.y = static_cast<LONG>(std::lround(bottom - scroll_y_));
        ClientToScreen(hwnd_, &point);

        HIMC context = ImmGetContext(hwnd_);
        if (context == nullptr) {
            return;
        }
        COMPOSITIONFORM composition{};
        composition.dwStyle = CFS_POINT;
        composition.ptCurrentPos = point;
        ImmSetCompositionWindow(context, &composition);

        CANDIDATEFORM candidate{};
        candidate.dwIndex = 0;
        candidate.dwStyle = CFS_CANDIDATEPOS;
        candidate.ptCurrentPos = point;
        ImmSetCandidateWindow(context, &candidate);
        ImmReleaseContext(hwnd_, context);
    }

    // 读取输入法组合串；没有内容时返回空字符串。
    std::wstring ImeString(DWORD kind) {
        HIMC context = ImmGetContext(hwnd_);
        if (context == nullptr) {
            return std::wstring();
        }
        const LONG bytes = ImmGetCompositionStringW(context, kind, nullptr, 0);
        std::wstring result;
        if (bytes > 0) {
            result.resize(static_cast<size_t>(bytes) / sizeof(wchar_t));
            ImmGetCompositionStringW(context, kind, &result[0], bytes);
        }
        ImmReleaseContext(hwnd_, context);
        return result;
    }

    // 组合期间把“基础文本 + 组合串”写入控件并按新文本重排，不派发 input。
    void UpdateCompositionDisplay() {
        if (focus_state_.element == nullptr) {
            return;
        }
        const size_t base_length = web::Utf8Length(ime_base_text_);
        const size_t begin = std::min(ime_base_caret_, base_length);
        const size_t begin_byte = CodepointByteOffset(ime_base_text_, begin);
        std::string updated = ime_base_text_.substr(0, begin_byte);
        updated += ime_composition_;
        updated += ime_base_text_.substr(begin_byte);
        SetFocusedText(updated);
        focus_state_.caret = begin + web::Utf8Length(ime_composition_);
        focus_state_.anchor = focus_state_.caret;
        focus_state_.caret_visible = true;
        RelayoutFocusedDocument();
        UpdateFocusScroll();
        InvalidateRect(hwnd_, nullptr, FALSE);
        PositionImeWindows();
    }

    // 放弃组合串，恢复组合开始前的文本（不派发 input）。
    void RestoreCompositionBase() {
        if (focus_state_.element == nullptr) {
            return;
        }
        SetFocusedText(ime_base_text_);
        const size_t length = web::Utf8Length(ime_base_text_);
        focus_state_.caret = std::min(ime_base_caret_, length);
        focus_state_.anchor = focus_state_.caret;
        RelayoutFocusedDocument();
        UpdateFocusScroll();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void OnImeStartComposition() {
        if (!FocusedTextControl()) {
            return;
        }
        ime_composing_ = true;
        ime_base_text_ = FocusedText();
        const size_t length = web::Utf8Length(ime_base_text_);
        ime_base_caret_ = std::min(focus_state_.caret, length);
        ime_composition_.clear();
        PositionImeWindows();
    }

    void OnImeComposition(LPARAM flags) {
        if (!ime_composing_ || !FocusedTextControl()) {
            return;
        }
        if ((flags & GCS_RESULTSTR) != 0) {
            // 提交结果：恢复基础文本后按普通编辑替换选区，并派发 input。
            const std::string result = WideToUtf8(ImeString(GCS_RESULTSTR));
            ime_composing_ = false;
            ime_composition_.clear();
            SetFocusedText(ime_base_text_);
            const size_t length = web::Utf8Length(ime_base_text_);
            focus_state_.caret = std::min(ime_base_caret_, length);
            focus_state_.anchor = focus_state_.caret;
            ReplaceSelection(result);
            return;
        }
        if ((flags & GCS_COMPSTR) != 0) {
            ime_composition_ = WideToUtf8(ImeString(GCS_COMPSTR));
            UpdateCompositionDisplay();
        }
    }

    void OnImeEndComposition() {
        if (!ime_composing_) {
            return;
        }
        // 未提交结果时结束组合：丢弃组合串并恢复原文。
        ime_composing_ = false;
        ime_composition_.clear();
        RestoreCompositionBase();
    }

    // 用 inserted 替换当前选区（无选区时插入到光标处）。
    void ReplaceSelection(const std::string& inserted) {
        if (focus_state_.element == nullptr) {
            return;
        }
        const std::string text = FocusedText();
        const size_t selection_begin = std::min(focus_state_.caret, focus_state_.anchor);
        const size_t selection_end = std::max(focus_state_.caret, focus_state_.anchor);
        const size_t begin_byte = CodepointByteOffset(text, selection_begin);
        const size_t end_byte = CodepointByteOffset(text, selection_end);
        std::string updated = text.substr(0, begin_byte);
        updated += inserted;
        if (end_byte < text.size()) {
            updated += text.substr(end_byte);
        }
        SetFocusedText(updated);
        focus_state_.caret = selection_begin + web::Utf8Length(inserted);
        focus_state_.anchor = focus_state_.caret;
        AfterTextEdit();
    }

    void ReplaceCodepointRange(size_t begin, size_t end, const std::string& inserted) {
        focus_state_.caret = begin;
        focus_state_.anchor = end;
        ReplaceSelection(inserted);
    }

    void DeleteBackward() {
        const size_t selection_begin = std::min(focus_state_.caret, focus_state_.anchor);
        const size_t selection_end = std::max(focus_state_.caret, focus_state_.anchor);
        if (selection_begin != selection_end) {
            ReplaceCodepointRange(selection_begin, selection_end, std::string());
            return;
        }
        if (focus_state_.caret > 0) {
            ReplaceCodepointRange(focus_state_.caret - 1, focus_state_.caret, std::string());
        }
    }

    void DeleteForward() {
        const size_t selection_begin = std::min(focus_state_.caret, focus_state_.anchor);
        const size_t selection_end = std::max(focus_state_.caret, focus_state_.anchor);
        if (selection_begin != selection_end) {
            ReplaceCodepointRange(selection_begin, selection_end, std::string());
            return;
        }
        const size_t length = web::Utf8Length(FocusedText());
        if (focus_state_.caret < length) {
            ReplaceCodepointRange(focus_state_.caret, focus_state_.caret + 1, std::string());
        }
    }

    void MoveCaretHorizontal(int delta, bool extend) {
        const size_t length = web::Utf8Length(FocusedText());
        if (!extend) {
            const size_t selection_begin =
                std::min(focus_state_.caret, focus_state_.anchor);
            const size_t selection_end = std::max(focus_state_.caret, focus_state_.anchor);
            if (selection_begin != selection_end) {
                focus_state_.caret = delta < 0 ? selection_begin : selection_end;
                focus_state_.anchor = focus_state_.caret;
                UpdateFocusScroll();
                InvalidateRect(hwnd_, nullptr, FALSE);
                return;
            }
        }
        if (delta < 0) {
            if (focus_state_.caret > 0) {
                --focus_state_.caret;
            }
        } else if (focus_state_.caret < length) {
            ++focus_state_.caret;
        }
        if (!extend) {
            focus_state_.anchor = focus_state_.caret;
        }
        UpdateFocusScroll();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void MoveCaretToLineBoundary(bool to_end, bool extend) {
        const size_t length = web::Utf8Length(FocusedText());
        if (web::AsciiLower(focus_state_.element->TagName()) != "textarea") {
            focus_state_.caret = to_end ? length : 0;
            if (!extend) {
                focus_state_.anchor = focus_state_.caret;
            }
            UpdateFocusScroll();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        const web::LayoutBox* box = FocusedBox();
        if (box == nullptr) {
            return;
        }
        const std::string text = FocusedText();
        const web::FormCaretPosition position =
            web::LocateFormCaret(*box, text, focus_state_.caret, text_);
        if (!position.valid) {
            return;
        }
        const web::FormLineRange range =
            web::GetFormLineRange(*box, text, position.line_index, text_);
        if (!range.valid) {
            return;
        }
        focus_state_.caret = to_end ? range.end_index : range.begin_index;
        if (!extend) {
            focus_state_.anchor = focus_state_.caret;
        }
        UpdateFocusScroll();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    // 上下方向键在相邻行近似移动：按光标行内水平位置估算目标行码位。
    void MoveCaretVertical(int direction, bool extend) {
        if (web::AsciiLower(focus_state_.element->TagName()) != "textarea") {
            return;
        }
        const web::LayoutBox* box = FocusedBox();
        if (box == nullptr) {
            return;
        }
        const std::string text = FocusedText();
        const web::FormCaretPosition position =
            web::LocateFormCaret(*box, text, focus_state_.caret, text_);
        if (!position.valid) {
            return;
        }
        const int target_line = static_cast<int>(position.line_index) + direction;
        if (target_line < 0 ||
            target_line >= static_cast<int>(box->lines.size())) {
            return;
        }
        const web::FormLineRange range = web::GetFormLineRange(
            *box, text, static_cast<size_t>(target_line), text_);
        if (!range.valid) {
            return;
        }
        const web::FontId font = FontForControl(*box, text_);
        const float desired_x = position.x;
        float pen_x = 0.0f;
        size_t count = 0;
        for (size_t index = range.begin_index; index < range.end_index; ++index) {
            const web::Utf8Char character = Utf8CharAtIndex(text, index);
            if (character.length == 0) {
                break;
            }
            const float advance =
                text_.Advance(font, character.codepoint) + box->style.letter_spacing;
            if (desired_x < pen_x + advance) {
                break;
            }
            pen_x += advance;
            ++count;
        }
        focus_state_.caret = range.begin_index + count;
        if (!extend) {
            focus_state_.anchor = focus_state_.caret;
        }
        UpdateFocusScroll();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void SwitchTextFocus(bool backwards) {
        std::vector<const web::Element*> controls;
        CollectTextControls(*FocusDocument(), &controls);
        if (controls.empty()) {
            return;
        }
        size_t current = controls.size();
        for (size_t index = 0; index < controls.size(); ++index) {
            if (controls[index] == focus_state_.element) {
                current = index;
                break;
            }
        }
        size_t next = 0;
        if (current >= controls.size()) {
            next = backwards ? controls.size() - 1 : 0;
        } else if (backwards) {
            next = (current + controls.size() - 1) % controls.size();
        } else {
            next = (current + 1) % controls.size();
        }
        if (controls[next] == focus_state_.element) {
            return;
        }
        SetFocus(controls[next]);
        UpdateFocusScroll();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    static std::string KeyNameForVirtualKey(WPARAM key) {
        if (key >= 'A' && key <= 'Z') {
            return std::string(1, static_cast<char>(key + ('a' - 'A')));
        }
        if (key >= '0' && key <= '9') {
            return std::string(1, static_cast<char>(key));
        }
        switch (key) {
            case VK_LEFT:
                return "ArrowLeft";
            case VK_RIGHT:
                return "ArrowRight";
            case VK_UP:
                return "ArrowUp";
            case VK_DOWN:
                return "ArrowDown";
            case VK_HOME:
                return "Home";
            case VK_END:
                return "End";
            case VK_BACK:
                return "Backspace";
            case VK_DELETE:
                return "Delete";
            case VK_RETURN:
                return "Enter";
            case VK_TAB:
                return "Tab";
            case VK_ESCAPE:
                return "Escape";
            case VK_SPACE:
                return " ";
            case VK_PRIOR:
                return "PageUp";
            case VK_NEXT:
                return "PageDown";
            default:
                return "Unidentified";
        }
    }

    static web::ScriptEventInit MakeKeyEvent(WPARAM key) {
        web::ScriptEventInit init;
        init.key = KeyNameForVirtualKey(key);
        // 任务约定 code 可与 key 相同。
        init.code = init.key;
        init.shift_key = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        init.ctrl_key = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        init.alt_key = (GetKeyState(VK_MENU) & 0x8000) != 0;
        return init;
    }

    void OnKeyDown(WPARAM key) {
        if (FocusedTextControl()) {
            DispatchEventTo(focus_state_.element, "keydown", MakeKeyEvent(key));
            if (!FocusedTextControl()) {
                return;
            }
            // Esc 仍按原有行为关闭窗口；其余按键由文本编辑路径处理，避免误触翻页。
            if (key == VK_ESCAPE) {
                DestroyWindow(hwnd_);
                return;
            }
            const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            if (control && (key == 'A' || key == 'a')) {
                focus_state_.anchor = 0;
                focus_state_.caret = web::Utf8Length(FocusedText());
                focus_state_.caret_visible = true;
                UpdateFocusScroll();
                InvalidateRect(hwnd_, nullptr, FALSE);
                return;
            }
            focus_state_.caret_visible = true;
            switch (key) {
                case VK_LEFT:
                    MoveCaretHorizontal(-1, shift);
                    return;
                case VK_RIGHT:
                    MoveCaretHorizontal(1, shift);
                    return;
                case VK_UP:
                    MoveCaretVertical(-1, shift);
                    return;
                case VK_DOWN:
                    MoveCaretVertical(1, shift);
                    return;
                case VK_HOME:
                    MoveCaretToLineBoundary(false, shift);
                    return;
                case VK_END:
                    MoveCaretToLineBoundary(true, shift);
                    return;
                case VK_BACK:
                    DeleteBackward();
                    return;
                case VK_DELETE:
                    DeleteForward();
                    return;
                case VK_RETURN:
                    if (web::AsciiLower(focus_state_.element->TagName()) == "textarea") {
                        ReplaceSelection("\n");
                    }
                    return;
                case VK_TAB:
                    SwitchTextFocus(shift);
                    return;
                default:
                    // 可打印字符由 WM_CHAR 处理。
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return;
            }
        }
        // 下拉浮层展开时的键盘导航。
        if (select_popup_.open) {
            const int count = static_cast<int>(web::SelectOptions(*select_popup_.select).size());
            if (key == VK_UP || key == VK_DOWN) {
                const int step = key == VK_UP ? -1 : 1;
                int next = select_popup_.highlighted + step;
                if (next < 0) {
                    next = 0;
                }
                if (next >= count) {
                    next = count - 1;
                }
                select_popup_.highlighted = next;
                InvalidateRect(hwnd_, nullptr, FALSE);
                return;
            }
            if (key == VK_RETURN) {
                web::Element* select = const_cast<web::Element*>(select_popup_.select);
                const int option = select_popup_.highlighted;
                const bool changed = web::CommitSelectPopup(&select_popup_, option);
                if (changed && select != nullptr) {
                    RelayoutAfterMutation(select);
                    DispatchEventTo(select, "change", web::ScriptEventInit{});
                }
                InvalidateRect(hwnd_, nullptr, FALSE);
                return;
            }
            if (key == VK_ESCAPE) {
                select_popup_ = web::SelectPopup();
                InvalidateRect(hwnd_, nullptr, FALSE);
                return;
            }
        }

        if (key == '1') {
            backend_ = web::RenderBackend::Direct3D11;
            device_.reset();
            images_.reset();
            InvalidateRect(hwnd_, nullptr, FALSE);
        } else if (key == '2') {
            backend_ = web::RenderBackend::OpenGL33;
            device_.reset();
            images_.reset();
            InvalidateRect(hwnd_, nullptr, FALSE);
        } else if ((GetKeyState(VK_CONTROL) & 0x8000) != 0 && (key == 'C' || key == 'c')) {
            CopySelectedText();
        } else if ((GetKeyState(VK_CONTROL) & 0x8000) != 0 && (key == 'A' || key == 'a')) {
            // 无文本控件焦点时全选整篇文档。
            selection_ = web::SelectAll(engine_.document());
            InvalidateRect(hwnd_, nullptr, FALSE);
        } else if (key == VK_UP) {
            ScrollBy(-kKeyboardScrollStep);
        } else if (key == VK_DOWN) {
            ScrollBy(kKeyboardScrollStep);
        } else if (key == VK_PRIOR) {
            ScrollBy(-static_cast<float>(std::max(client_height_, 1)) * 0.9f);
        } else if (key == VK_NEXT) {
            ScrollBy(static_cast<float>(std::max(client_height_, 1)) * 0.9f);
        } else if (key == VK_HOME) {
            ScrollTo(0.0f);
        } else if (key == VK_END) {
            ScrollTo(MaxScroll());
        } else if (key == VK_LEFT) {
            ChangePage(-1);
        } else if (key == VK_RIGHT) {
            ChangePage(1);
        } else if (key == VK_ESCAPE) {
            DestroyWindow(hwnd_);
        }
    }

    void OnKeyUp(WPARAM key) {
        if (focus_state_.element == nullptr) {
            return;
        }
        DispatchEventTo(focus_state_.element, "keyup", MakeKeyEvent(key));
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void ChangePage(int delta) {
        const web::LayoutResult* layout = engine_.layout();
        if (layout == nullptr) {
            return;
        }
        const int count = layout->PageCount();
        if (count <= 0) {
            return;
        }
        const int next = std::clamp(page_ + delta, 0, count - 1);
        if (next == page_) {
            return;
        }
        scroll_y_ = layout->page_starts[static_cast<size_t>(next)];
        ClampScroll();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    // 按点击位置更新焦点控件的码位索引；keep_anchor 为 true 时保留拖选锚点。
    void UpdateCaretFromPoint(const web::Element* element, int x, int y, bool keep_anchor) {
        const web::LayoutResult* layout = engine_.layout();
        if (layout == nullptr || element == nullptr) {
            return;
        }
        const float page_start =
            layout->page_starts.empty() ? 0.0f
                                        : layout->page_starts[static_cast<size_t>(page_)];
        const web::LayoutBox* box = nullptr;
        float document_x = 0.0f;
        float document_y = 0.0f;
        if (!FindBoxAndPoint(*layout, element, static_cast<float>(x),
                             static_cast<float>(y) + PageYOffset(), page_start, &box,
                             &document_x, &document_y)) {
            return;
        }
        const size_t caret = CaretIndexFromPoint(*box, *element, document_x, document_y,
                                                 focus_state_.scroll_x, focus_state_.scroll_y,
                                                 text_);
        focus_state_.caret = caret;
        if (!keep_anchor) {
            focus_state_.anchor = caret;
        }
        focus_state_.caret_visible = true;
        UpdateFocusScroll();
    }

    // 把点击派发到命中元素；命中内嵌文档时换算坐标后交给子文档脚本宿主。
    void DispatchHitClick(const web::HitResult& hit, int x, int y) {
        const web::LayoutResult* layout = engine_.layout();
        if (layout == nullptr || !hit.Valid()) {
            return;
        }
        if (hit.frame_element != nullptr) {
            float frame_x = static_cast<float>(x);
            float frame_y = static_cast<float>(y);
            const web::LayoutBox* frame_box =
                FindLayoutBoxForElement(*layout->root, hit.frame_element);
            if (frame_box != nullptr) {
                frame_x = static_cast<float>(x) - frame_box->content_box.left;
                frame_y = static_cast<float>(y) + scroll_y_ - frame_box->content_box.top;
            }
            engine_.DispatchFrameClick(*hit.frame_element, hit.element, frame_x, frame_y);
        } else if (script_ != nullptr) {
            script_->DispatchClick(hit.element, static_cast<float>(x), static_cast<float>(y));
        }
    }

    void OnMouseDown(int x, int y) {
        // 上一事件可能改动了 DOM：命中测试前先同步布局，避免访问已移除的元素。
        EnsureLayout();
        const web::LayoutResult* layout = engine_.layout();
        if (layout == nullptr) {
            return;
        }
        ClampScroll();
        // 滚动条优先接管鼠标：按最新客户区尺寸重算滑块几何，命中后拖动或跳转，
        // 不派发页面点击事件。
        RECT client{};
        GetClientRect(hwnd_, &client);
        UpdateScrollbarGeometry(client.right - client.left, client.bottom - client.top);
        if (scrollbar_visible_ && static_cast<float>(x) >= scrollbar_track_.left - 2.0f) {
            if (select_popup_.open) {
                select_popup_ = web::SelectPopup();
            }
            scrollbar_capture_ = true;
            scrollbar_dragging_ = true;
            SetCapture(hwnd_);
            if (scrollbar_thumb_.Contains(static_cast<float>(x), static_cast<float>(y))) {
                scrollbar_grab_offset_ = static_cast<float>(y) - scrollbar_thumb_.top;
            } else {
                scrollbar_grab_offset_ = scrollbar_thumb_.Height() * 0.5f;
            }
            ScrollToThumb(static_cast<float>(y) - scrollbar_grab_offset_);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        scrollbar_capture_ = false;
        // 下拉浮层展开时优先处理选项点击；点击浮层外关闭浮层并继续常规命中。
        if (select_popup_.open) {
            const int option = web::HitTestSelectPopup(
                *layout, page_, select_popup_, static_cast<float>(x),
                static_cast<float>(y) + PageYOffset());
            if (option >= 0) {
                web::Element* select = const_cast<web::Element*>(select_popup_.select);
                const bool changed = web::CommitSelectPopup(&select_popup_, option);
                if (changed && select != nullptr) {
                    RelayoutAfterMutation(select);
                    DispatchEventTo(select, "change", web::ScriptEventInit{});
                }
                InvalidateRect(hwnd_, nullptr, FALSE);
                return;
            }
            select_popup_ = web::SelectPopup();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }

        const web::HitResult hit =
            web::HitTestPage(*layout, page_, static_cast<float>(x),
                             static_cast<float>(y) + PageYOffset(), &text_);
        if (!hit.Valid()) {
            // 点击空白处清除文本控件焦点与文档选区，并派发 blur；指针按下事件
            // 仍派发到 body，使窗口级指针监听器维持正确的交互状态。
            web::ScriptEventInit pointer_init;
            pointer_init.client_x = static_cast<float>(x);
            pointer_init.client_y = static_cast<float>(y);
            pointer_init.button = 0;
            if (engine_.document().Body() != nullptr) {
                engine_.DispatchEvent(engine_.document().Body(), "pointerdown", pointer_init);
            }
            SetFocus(nullptr);
            ClearSelection();
            dragging_ = false;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }

        /* 指针按下：命中元素及其祖先进入 :active，按钮等控件据此显示按压反馈。 */
        SetActiveTarget(hit.element);

        if (IsTextFieldControl(hit.element) && !hit.element->HasAttribute("disabled")) {
            // 文本控件：聚焦并派发 focus，随后按点击位置设置光标、开始拖选。
            const bool same_element = focus_state_.element == hit.element;
            SetFocus(hit.element);
            UpdateCaretFromPoint(hit.element, x, y, same_element);
            dragging_ = true;
            DispatchHitClick(hit, x, y);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }

        // 其他控件或文字：先派发 blur，再执行控件默认行为与事件。
        dragging_ = false;
        web::Element* element = const_cast<web::Element*>(hit.element);
        // 命中按钮内部的 span 等子元素时，默认动作应作用于最近的可激活祖先控件。
        web::Element* control_element = web::FindActivatableAncestor(*element);
        /* 浏览器点击按钮、链接或下拉框时会把焦点移到该控件；页面脚本常用
           document.activeElement 判断触发来源，因此焦点跟随可激活控件。 */
        SetFocus(control_element);

        // 下拉框：展开选项浮层，点击选项后提交并派发 change。
        if (control_element != nullptr && control_element->TagName() == "select") {
            const std::vector<std::string> options = web::SelectOptions(*control_element);
            if (!options.empty()) {
                const int current = web::SelectSelectedIndex(*control_element);
                web::ToggleSelectPopup(&select_popup_, control_element, current < 0 ? 0 : current);
            }
            DispatchHitClick(hit, x, y);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }

        // 表单控件与链接：交给控件事件模块处理 submit/reset/勾选。
        const web::ControlEvent control =
            control_element != nullptr
                ? web::ActivateControl(*control_element, form_defaults_)
                : web::ControlEvent{web::ControlEventType::Click, element, false};
        if (control.type == web::ControlEventType::Change) {
            RelayoutAfterMutation(control_element);
            DispatchEventTo(control.target, "change", web::ScriptEventInit{});
        } else if (control.type == web::ControlEventType::Submit) {
            DispatchEventTo(control.target, "submit", web::ScriptEventInit{});
        } else if (control.type == web::ControlEventType::Reset) {
            if (control.layout_changed) {
                if (engine_.MarkDirty(), engine_.Relayout(nullptr)) {
                    // 重置后重新同步文本控件显示。
                }
            }
            DispatchEventTo(control.target, "reset", web::ScriptEventInit{});
        } else if (control.layout_changed) {
            // details/summary 展开状态改变等需要重新排版。
            RelayoutAfterMutation(control_element);
        }
        DispatchHitClick(hit, x, y);

        // 文字上的按下同时开始文档选区（与控件点击互不影响）。
        BeginSelection(x, y, hit);

        // 指针按下事件放在所有默认处理之后派发：页面处理器可能替换 DOM，
        // 此后再使用命中结果会失效。目标取最内层命中的元素。
        web::ScriptEventInit pointer_init;
        pointer_init.client_x = static_cast<float>(x);
        pointer_init.client_y = static_cast<float>(y);
        pointer_init.button = 0;
        engine_.DispatchEvent(const_cast<web::Element*>(hit.element), "pointerdown",
                               pointer_init);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    // DOM 变更后的重新布局：内嵌文档走子引擎，顶层文档整体重新排版。
    void RelayoutAfterMutation(const web::Element* element) {
        // 布局结果即将重建，浮层中保存的元素与几何信息不再可靠。
        select_popup_ = web::SelectPopup();
        if (const web::Element* frame = engine_.FrameElementForNode(*element)) {
            engine_.RelayoutFrame(*frame);
            return;
        }
        engine_.MarkDirty();
        engine_.Relayout(nullptr);
    }

    void ClearSelection() {
        selection_ = web::SelectionRange();
        selecting_ = false;
    }

    // 复制当前选中文本：文本控件内复制控件选区，否则复制文档选区。
    void CopySelectedText() {
        if (FocusedTextControl()) {
            const std::string text = web::FormControlText(*focus_state_.element);
            const size_t begin = std::min(focus_state_.caret, focus_state_.anchor);
            const size_t end = std::max(focus_state_.caret, focus_state_.anchor);
            if (end > begin) {
                // 按码位截取 UTF-8 子串。
                size_t byte_begin = 0;
                size_t byte_end = text.size();
                size_t offset = 0;
                size_t index = 0;
                while (offset < text.size()) {
                    const web::Utf8Char character = web::DecodeUtf8(text, offset);
                    if (character.length == 0) {
                        break;
                    }
                    if (index == begin) {
                        byte_begin = offset;
                    }
                    if (index == end) {
                        byte_end = offset;
                        break;
                    }
                    offset += character.length;
                    index += 1;
                }
                web::WriteClipboardText(text.substr(byte_begin, byte_end - byte_begin));
            }
            return;
        }
        if (!selection_.collapsed) {
            web::CopySelectionToClipboard(engine_.document(), selection_);
        }
    }

    // 开始一次选区拖拽：锚点取点击处的字符位置。
    void BeginSelection(int x, int y, const web::HitResult& hit) {
        const web::LayoutResult* layout = engine_.layout();
        if (layout == nullptr || hit.node == nullptr) {
            ClearSelection();
            return;
        }
        const web::TextPosition position = web::HitTestTextPosition(
            *layout, page_, static_cast<float>(x), static_cast<float>(y) + PageYOffset(), text_);
        if (!position.valid) {
            ClearSelection();
            return;
        }
        selection_.anchor = position;
        selection_.focus = position;
        selection_.collapsed = true;
        selecting_ = true;
    }

    void OnMouseMove(int x, int y) {
        // 注册离开通知，指针移出客户区时清除悬停状态。
        TRACKMOUSEEVENT track{};
        track.cbSize = sizeof(track);
        track.dwFlags = TME_LEAVE;
        track.hwndTrack = hwnd_;
        TrackMouseEvent(&track);
        if (scrollbar_dragging_) {
            ScrollToThumb(static_cast<float>(y) - scrollbar_grab_offset_);
            return;
        }
        const auto profile_start = std::chrono::steady_clock::now();
        // 指针移动事件：窗口级监听器（拖拽、悬停反馈）依赖它。
        EnsureLayout();
        /* 悬停反馈：命中元素及其祖先设置 :hover。目标变化时重新匹配样式与排版，
           之后重新获取布局结果，避免使用已释放的旧布局树。 */
        if (const web::LayoutResult* hover_layout = engine_.layout()) {
            ClampScroll();
            const web::HitResult hover_hit = web::HitTestPage(
                *hover_layout, page_, static_cast<float>(x),
                static_cast<float>(y) + PageYOffset(), &text_);
            UpdateHover(hover_hit.Valid() ? hover_hit.element : nullptr);
        }
        const double profile_layout = MillisecondsSince(profile_start);
        const web::LayoutResult* layout = engine_.layout();
        ClampScroll();
        double profile_hit = 0.0;
        double profile_dispatch = 0.0;
        if (layout != nullptr && script_ != nullptr) {
            const auto hit_start = std::chrono::steady_clock::now();
            const web::HitResult hit = web::HitTestPage(
                *layout, page_, static_cast<float>(x),
                static_cast<float>(y) + PageYOffset(), &text_);
            profile_hit = MillisecondsSince(hit_start);
            web::Element* target =
                hit.Valid() ? const_cast<web::Element*>(hit.element) : engine_.document().Body();
            if (target != nullptr) {
                web::ScriptEventInit pointer_init;
                pointer_init.client_x = static_cast<float>(x);
                pointer_init.client_y = static_cast<float>(y);
                const auto dispatch_start = std::chrono::steady_clock::now();
                engine_.DispatchEvent(target, "pointermove", pointer_init);
                profile_dispatch = MillisecondsSince(dispatch_start);
            }
        }
        if (ProfileEnabled()) {
            std::cout << "PROF move layout=" << profile_layout << " hit=" << profile_hit
                      << " dispatch=" << profile_dispatch
                      << " total=" << MillisecondsSince(profile_start) << "\n";
        }
        if (select_popup_.open) {
            const web::LayoutResult* layout = engine_.layout();
            if (layout != nullptr) {
                const int option = web::HitTestSelectPopup(
                    *layout, page_, select_popup_, static_cast<float>(x),
                    static_cast<float>(y) + PageYOffset());
                if (option >= 0 && option != select_popup_.highlighted) {
                    select_popup_.highlighted = option;
                    InvalidateRect(hwnd_, nullptr, FALSE);
                }
            }
        }
        if (selecting_) {
            const web::LayoutResult* layout = engine_.layout();
            if (layout != nullptr) {
                const web::TextPosition position = web::HitTestTextPosition(
                    *layout, page_, static_cast<float>(x),
                    static_cast<float>(y) + PageYOffset(), text_);
                if (position.valid) {
                    selection_.focus = position;
                    selection_.collapsed = selection_.anchor == selection_.focus;
                    InvalidateRect(hwnd_, nullptr, FALSE);
                }
            }
            return;
        }
        if (!dragging_ || focus_state_.element == nullptr) {
            return;
        }
        UpdateCaretFromPoint(focus_state_.element, x, y, true);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void OnMouseUp(int x, int y) {
        if (scrollbar_capture_) {
            scrollbar_capture_ = false;
            scrollbar_dragging_ = false;
            ReleaseCapture();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        dragging_ = false;
        selecting_ = false;
        // 松开指针：清除 :active，按钮恢复常态。
        if (active_target_set_) {
            active_target_set_ = false;
            ClearDocumentState(2);
            engine_.MarkDirty();
            engine_.Relayout(nullptr);
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        EnsureLayout();
        ClampScroll();
        // 指针抬起事件：目标取松开位置的最内层元素，未命中时交给 body。
        const web::LayoutResult* layout = engine_.layout();
        if (layout != nullptr && script_ != nullptr) {
            const web::HitResult hit = web::HitTestPage(
                *layout, page_, static_cast<float>(x),
                static_cast<float>(y) + PageYOffset(), &text_);
            web::Element* target =
                hit.Valid() ? const_cast<web::Element*>(hit.element) : engine_.document().Body();
            if (target != nullptr) {
                web::ScriptEventInit pointer_init;
                pointer_init.client_x = static_cast<float>(x);
                pointer_init.client_y = static_cast<float>(y);
                engine_.DispatchEvent(target, "pointerup", pointer_init);
            }
        }
    }

    // 派生指针事件的目标：最内层命中元素，未命中时使用 body。
    web::Element* PointerEventTarget(float x, float y) {
        EnsureLayout();
        const web::LayoutResult* layout = engine_.layout();
        if (layout == nullptr) {
            return nullptr;
        }
        const web::HitResult hit =
            web::HitTestPage(*layout, page_, x, y + PageYOffset(), &text_);
        return hit.Valid() ? const_cast<web::Element*>(hit.element) : engine_.document().Body();
    }

    // 中键与右键的指针事件。中键用于画布平移，右键用于结束连线等操作。
    void DispatchPointerButton(int x, int y, int button, bool down) {
        if (script_ == nullptr) {
            return;
        }
        web::Element* target = PointerEventTarget(static_cast<float>(x), static_cast<float>(y));
        if (target == nullptr) {
            return;
        }
        web::ScriptEventInit init;
        init.client_x = static_cast<float>(x);
        init.client_y = static_cast<float>(y);
        init.button = button;
        engine_.DispatchEvent(target, down ? "pointerdown" : "pointerup", init);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void DispatchContextMenu(int x, int y) {
        if (script_ == nullptr) {
            return;
        }
        web::Element* target = PointerEventTarget(static_cast<float>(x), static_cast<float>(y));
        if (target == nullptr) {
            return;
        }
        web::ScriptEventInit init;
        init.client_x = static_cast<float>(x);
        init.client_y = static_cast<float>(y);
        init.button = 2;
        engine_.DispatchEvent(target, "contextmenu", init);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    // 滚轮：先交给页面处理（SVG 画布缩放），页面调用 preventDefault 时不滚动。
    void DispatchWheel(int x, int y, int delta) {
        const float step =
            static_cast<float>(delta) / static_cast<float>(WHEEL_DELTA) * kWheelScrollStep;
        if (script_ == nullptr) {
            ScrollBy(-step);
            return;
        }
        web::Element* target = PointerEventTarget(static_cast<float>(x), static_cast<float>(y));
        bool prevented = false;
        if (target != nullptr) {
            web::ScriptEventInit init;
            init.client_x = static_cast<float>(x);
            init.client_y = static_cast<float>(y);
            init.delta_y = -static_cast<float>(delta);
            engine_.DispatchEvent(target, "wheel", init, &prevented);
        }
        if (!prevented) {
            if (select_popup_.open) {
                select_popup_ = web::SelectPopup();
            }
            ScrollBy(-step);
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    // WM_CHAR：可打印字符替换选区并派发 input；代理对合并为一个码位。
    void OnChar(WPARAM character) {
        // 输入法组合期间的字符由组合消息处理，避免组合串与结果重复插入。
        if (ime_composing_) {
            return;
        }
        if (!FocusedTextControl()) {
            return;
        }
        uint32_t codepoint = static_cast<uint32_t>(static_cast<wchar_t>(character));
        if (codepoint >= 0xD800u && codepoint <= 0xDBFFu) {
            pending_high_surrogate_ = codepoint;
            return;
        }
        if (codepoint >= 0xDC00u && codepoint <= 0xDFFFu) {
            if (pending_high_surrogate_ == 0) {
                return;
            }
            codepoint = 0x10000u + ((pending_high_surrogate_ - 0xD800u) << 10) +
                        (codepoint - 0xDC00u);
            pending_high_surrogate_ = 0;
        } else {
            pending_high_surrogate_ = 0;
        }
        // 退格、回车与其他控制字符由键盘消息处理，不进入文本。
        if (codepoint < 0x20u || codepoint == 0x7Fu) {
            return;
        }
        ReplaceSelection(web::EncodeUtf8(codepoint));
    }

    void OnTimer(UINT_PTR timer_id) {
        if (timer_id == 2) {
            // 光标闪烁：仅焦点文本控件在闪烁，重绘让新状态生效。
            if (FocusedTextControl()) {
                focus_state_.caret_visible = !focus_state_.caret_visible;
                InvalidateRect(hwnd_, nullptr, FALSE);
            } else {
                focus_state_.caret_visible = true;
            }
            return;
        }
        if (timer_id == 3) {
            // 拖动缩放的节流定时器：按最新客户区尺寸重排一次，随后由新的
            // WM_SIZE 再次启动，使拖动过程中持续输出新画面。
            KillTimer(hwnd_, 3);
            resize_timer_active_ = false;
            ApplyPendingResize();
            return;
        }
        bool relayouted = engine_.AdvanceFrameScripts(16.0f);
        if (script_ != nullptr) {
            script_->AdvanceTime(16.0);
        }
        /* CSS 动画：推进动画时钟；存在活动动画时标记为需要重新排版。
           脚本与动画的排版需求合并为一次，避免同一帧重复布局。 */
        animation_time_ += 16.0 / 1000.0;
        if (engine_.HasActiveAnimations()) {
            engine_.SetAnimationTime(animation_time_);
            engine_.MarkDirty();
        }
        if (engine_.dirty()) {
            const auto layout_start = std::chrono::steady_clock::now();
            engine_.Relayout(nullptr);
            if (ProfileEnabled()) {
                std::cout << "PROF timer layout=" << MillisecondsSince(layout_start) << "\n";
            }
            relayouted = true;
        }
        if (relayouted) {
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
    }

    web::Engine& engine_;
    web::TextService& text_;
    web::ScriptHost* script_ = nullptr;
    web::RenderBackend backend_ = web::RenderBackend::Direct3D11;
    HWND hwnd_ = nullptr;
    int page_ = 0;
    // 垂直滚动偏移（文档坐标）。布局仍按页切分，视口以像素为单位滑动；
    // 视口跨越页边界时分别渲染相邻两页再按偏移拼接。
    float scroll_y_ = 0.0f;
    int client_height_ = 0;
    // 滚动条几何（客户区坐标）。内容不高于视口时隐藏。
    bool scrollbar_visible_ = false;
    web::RectF scrollbar_track_;
    web::RectF scrollbar_thumb_;
    bool scrollbar_dragging_ = false;
    bool scrollbar_capture_ = false;
    float scrollbar_grab_offset_ = 0.0f;
    // 逐页渲染缓冲：当前页与视口内可见的下一页。
    std::vector<uint8_t> page_pixels_a_;
    std::vector<uint8_t> page_pixels_b_;
    // 拖动缩放的重排节流间隔（毫秒）：约一帧一次，拖动过程中持续输出新画面。
    static constexpr UINT kResizeThrottleMs = 16;
    // 拖动缩放状态：resizing_ 表示正在拖动窗口边框；resize_pending_ 表示客户区
    // 新尺寸尚未应用，绘制时先按原尺寸显示上一帧。
    bool resizing_ = false;
    bool resize_pending_ = false;
    bool resize_timer_active_ = false;
    int pending_width_ = 0;
    int pending_height_ = 0;
    // 上一帧位图的尺寸，用于客户区尺寸变化时按原尺寸显示。
    bool frame_valid_ = false;
    int frame_width_ = 0;
    int frame_height_ = 0;
    // 焦点状态：元素指针、光标与选区锚点（码位索引）、滚动偏移与光标可见状态。
    web::FormFocusState focus_state_;
    // 文档选区（跨元素选择与复制）。collapsed 表示没有选中内容。
    web::SelectionRange selection_;
    bool selecting_ = false;
    // 下拉框展开浮层。open 为 true 时绘制选项列表并接管点击。
    web::SelectPopup select_popup_;
    // 表单控件的初始值快照，用于表单重置。
    web::FormDefaults form_defaults_;
    bool focus_text_edited_ = false;
    bool dragging_ = false;
    uint32_t pending_high_surrogate_ = 0;
    // 当前悬停命中的元素（用于判断 :hover 目标是否变化）与指针按压状态标记。
    const web::Element* hovered_element_ = nullptr;
    bool active_target_set_ = false;
    // CSS 动画时钟（秒），由帧定时器推进。
    double animation_time_ = 0.0;
    // 输入法组合状态：组合串、组合开始前的控件文本与光标，用于预编辑显示与回退。
    bool ime_composing_ = false;
    std::string ime_composition_;
    std::string ime_base_text_;
    size_t ime_base_caret_ = 0;
    std::unique_ptr<web::RenderDevice> device_;
    std::unique_ptr<web::ImageCache> images_;
    int device_width_ = 0;
    int device_height_ = 0;
    std::vector<uint8_t> dib_pixels_;
};

bool EvaluatePageScripts(web::Engine& engine, web::ScriptHost& script) {
    const std::vector<std::string> scripts = engine.CollectScripts();
    for (size_t index = 0; index < scripts.size(); ++index) {
        std::string exception;
        if (!script.Evaluate(scripts[index], &exception)) {
            std::cerr << "脚本执行失败（第 " << index + 1 << " 段）: " << exception << "\n";
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // 参数统一按 UTF-8 解释，含中文路径的文件才能正确定位与显示。
    const std::vector<std::string> arguments = Utf8CommandLineArguments();

    Options options;
    std::string argument_error;
    if (!ParseArguments(arguments, &options, &argument_error)) {
        if (!argument_error.empty()) {
            std::cerr << argument_error << "\n";
        }
        CoUninitialize();
        return argument_error.empty() ? 0 : 2;
    }

    /* 无参数启动（例如双击可执行文件）：先让用户选择要预览的 HTML 文件；
       取消选择时回退到工程内的样例页，保证双击后立即有内容可看。 */
    if (arguments.size() <= 1) {
        const std::string chosen = OpenHtmlFileDialog();
        if (!chosen.empty()) {
            options.html_path = chosen;
        }
        options.window = true;
        options.interactive = true;
    }

    if (options.interactive) {
        /* 双击启动时隐藏随附的控制台窗口，只保留预览窗口。
           命令行输出模式（--out 等）不受影响。 */
        if (HWND console = GetConsoleWindow()) {
            ShowWindow(console, SW_HIDE);
        }
        int window_width = 0;
        int window_height = 0;
        DefaultWindowSize(&window_width, &window_height);
        options.width = static_cast<float>(window_width);
        options.page_height = static_cast<float>(window_height);
    }

    std::unique_ptr<web::TextService> text = web::TextService::Create();
    if (!text || !text->Initialize()) {
        std::cerr << "DirectWrite 文本服务初始化失败\n";
        CoUninitialize();
        return 3;
    }
    // 画布文字绘制与页面排版共享同一文本服务与字形图集。
    web::SetCanvasTextService(text.get());

    web::Engine engine;
    engine.SetTextService(text.get());

    bool script_dirty = false;
    web::ScriptHostOptions script_options;
    script_options.on_document_changed = [&engine]() { engine.MarkDirty(); };
    // SVG 内部变更只需刷新 SVG 样式，不重新排版 HTML。
    script_options.on_svg_changed = [&engine]() { engine.MarkSvgDirty(); };
    script_options.on_animation_frame = [&script_dirty]() { script_dirty = true; };
    // 元素几何查询：按当前布局结果查找边框盒，坐标为文档坐标（第 0 页视口）。
    script_options.get_element_client_rect =
        [&engine](const web::Element& element, float* left, float* top, float* width,
                  float* height) {
            const web::LayoutResult* layout = engine.layout();
            if (layout == nullptr || layout->root == nullptr) {
                return false;
            }
            const web::LayoutBox* box = FindLayoutBoxForElement(*layout->root, &element);
            if (box == nullptr) {
                return false;
            }
            *left = box->border_box.left;
            *top = box->border_box.top;
            *width = box->border_box.Width();
            *height = box->border_box.Height();
            return true;
        };

    std::unique_ptr<web::ScriptHost> script;

    std::string load_error;
    bool loaded = false;
    if (!options.html_path.empty()) {
        loaded = engine.LoadFile(options.html_path, &load_error);
    } else {
        const std::string sample = LocateSampleHtml();
        if (!sample.empty()) {
            loaded = engine.LoadFile(sample, &load_error);
        } else {
            loaded = engine.LoadHtmlString(kFallbackHtml, std::string(), &load_error);
        }
    }
    if (!loaded) {
        std::cerr << "载入 HTML 失败: " << load_error << "\n";
        CoUninitialize();
        return 4;
    }
    if (!load_error.empty()) {
        std::cerr << "样式解析警告: " << load_error << "\n";
    }
    // 每个文档使用独立的 localStorage 持久化文件，页面的设计状态可跨运行保留。
    script_options.local_storage_path =
        LocalStoragePathForDocument(engine.document_path());

    // 画布 drawImage 的 img 源按页面基准目录解析相对路径。
    web::SetCanvasBaseDirectory(engine.base_directory());

    // 内嵌子文档脚本与顶层脚本由同一开关控制，必须在首次布局前设置。
    engine.SetRunScripts(options.run_script || options.window);
    engine.SetViewport(options.width, options.page_height);
    if (!options.window) {
        /* 静态截图取 2 秒时刻：一次性入场动画已完成，无限循环动画按该时刻取帧，
           避免首帧 opacity 为 0 导致内容不可见。 */
        engine.SetAnimationTime(2.0);
    }
    if (!engine.Relayout(&load_error)) {
        std::cerr << "布局失败: " << load_error << "\n";
        CoUninitialize();
        return 5;
    }

    // 脚本宿主必须在文档载入之后创建，ScriptHost 内部保存的是当前 Document 的地址。
    if (options.run_script || options.window) {
        script = web::ScriptHost::Create(&engine.document(), script_options);
        if (!script) {
            std::cerr << "QuickJS 脚本宿主创建失败\n";
        }
    }

    if (script) {
        // 顶层脚本宿主注册到引擎，事件按元素所属文档统一派发。
        engine.SetTopLevelScriptHost(script.get());
        script->SetViewport(options.width, options.page_height);
        EvaluatePageScripts(engine, *script);
        // 页面初始化通常注册在 DOMContentLoaded 上；脚本执行完成后派发一次，
        // 使这些初始化逻辑在首次布局前生效。
        script->DispatchDocumentEvent("DOMContentLoaded");
        if (engine.dirty() && !engine.Relayout(&load_error)) {
            std::cerr << "脚本改动后重新布局失败: " << load_error << "\n";
        }
        // 无窗口渲染推进一帧脚本时间：页面常在 requestAnimationFrame 回调中
        // 依据元素尺寸完成初始布局（例如画布缩放与居中）。
        if (options.run_script) {
            script->AdvanceTime(16.0);
            if (engine.dirty() && !engine.Relayout(&load_error)) {
                std::cerr << "首帧脚本后重新布局失败: " << load_error << "\n";
            }
        }
    }

    // 表单控件的初始值快照，供模拟点击触发的 reset 默认动作恢复。
    web::FormDefaults form_defaults;
    form_defaults.Capture(engine.document());

    // 无窗口模式下按页面坐标模拟点击，用于验证事件派发与 DOM 变更后的重新布局。
    if (script && options.click_x >= 0.0f && options.click_y >= 0.0f) {
        const web::LayoutResult* current = engine.layout();
        const int click_page =
            options.page < 0 ? 0
                             : std::min(options.page, std::max(current->PageCount() - 1, 0));
        const web::HitResult hit =
            web::HitTestPage(*current, click_page, options.click_x, options.click_y, text.get());
        if (!hit.Valid()) {
            std::cerr << "点击位置未命中内容: " << options.click_x << "," << options.click_y << "\n";
        } else {
            std::cout << "点击命中元素: " << hit.element->TagName() << "\n";
            if (hit.frame_element != nullptr) {
                // 命中内嵌文档：换算为子文档坐标并派发到对应子文档脚本宿主。
                float frame_x = options.click_x;
                float frame_y = options.click_y;
                const web::LayoutBox* frame_box =
                    FindLayoutBoxForElement(*current->root, hit.frame_element);
                if (frame_box != nullptr) {
                    const float page_start =
                        current->page_starts.empty()
                            ? 0.0f
                            : current->page_starts[static_cast<size_t>(click_page)];
                    frame_x = options.click_x - frame_box->content_box.left;
                    frame_y = options.click_y - (frame_box->content_box.top - page_start);
                }
                engine.DispatchFrameClick(*hit.frame_element, hit.element, frame_x, frame_y);
            } else {
                // 与预览窗口路径一致：先执行命中元素所属可激活控件的默认动作，
                // 再派发 click 事件（click 沿祖先链冒泡到按钮）。
                web::Element* hit_element = const_cast<web::Element*>(hit.element);
                web::Element* control_element = web::FindActivatableAncestor(*hit_element);
                if (control_element != nullptr) {
                    const web::ControlEvent control =
                        web::ActivateControl(*control_element, form_defaults);
                    const char* action = nullptr;
                    if (control.type == web::ControlEventType::Change) {
                        action = "change";
                    } else if (control.type == web::ControlEventType::Submit) {
                        action = "submit";
                    } else if (control.type == web::ControlEventType::Reset) {
                        action = "reset";
                    }
                    if (action != nullptr) {
                        std::cout << "控件默认动作: " << control_element->TagName() << " → "
                                  << action << "\n";
                    }
                    if (control.type == web::ControlEventType::Change) {
                        engine.MarkDirty();
                        if (!engine.Relayout(&load_error)) {
                            std::cerr << "控件状态改变后重新布局失败: " << load_error << "\n";
                        }
                        engine.DispatchEvent(control.target, "change", web::ScriptEventInit{});
                    } else if (control.type == web::ControlEventType::Submit) {
                        engine.DispatchEvent(control.target, "submit", web::ScriptEventInit{});
                    } else if (control.type == web::ControlEventType::Reset) {
                        if (control.layout_changed) {
                            engine.MarkDirty();
                            if (!engine.Relayout(&load_error)) {
                                std::cerr << "表单重置后重新布局失败: " << load_error << "\n";
                            }
                        }
                        engine.DispatchEvent(control.target, "reset", web::ScriptEventInit{});
                    } else if (control.layout_changed &&
                               (engine.MarkDirty(), !engine.Relayout(&load_error))) {
                        std::cerr << "控件展开状态改变后重新布局失败: " << load_error << "\n";
                    }
                }
                script->DispatchClick(hit.element, options.click_x, options.click_y);
                // 无窗口模式同样派发指针按下与抬起：使用 pointer 事件的页面
                // 依赖它们完成选择、拖拽等交互，静态截图才能反映交互结果。
                web::ScriptEventInit pointer_init;
                pointer_init.client_x = options.click_x;
                pointer_init.client_y = options.click_y;
                engine.DispatchEvent(hit.element, "pointerdown", pointer_init);
                if (engine.document().Body() != nullptr) {
                    engine.DispatchEvent(engine.document().Body(), "pointerup", pointer_init);
                }
                if (engine.dirty() && !engine.Relayout(&load_error)) {
                    std::cerr << "点击后重新布局失败: " << load_error << "\n";
                }
            }
        }
    }

    const web::LayoutResult* layout = engine.layout();
    std::cout << "布局完成：视口 " << layout->viewport_width << "，内容高 " << layout->content_height
              << "，页数 " << layout->PageCount() << "\n";

    // 无窗口模式下按坐标展开 select 浮层，供静态截图校验。
    web::SelectPopup select_popup;
    if (options.popup_x >= 0.0f && options.popup_y >= 0.0f) {
        const int popup_page =
            options.page < 0 ? 0 : std::min(options.page, std::max(layout->PageCount() - 1, 0));
        const web::HitResult hit =
            web::HitTestPage(*layout, popup_page, options.popup_x, options.popup_y, text.get());
        if (hit.Valid() && hit.element->TagName() == "select") {
            web::Element* select = const_cast<web::Element*>(hit.element);
            const int index = std::max(0, web::SelectSelectedIndex(*select));
            web::ToggleSelectPopup(&select_popup, select, index);
        } else {
            std::cerr << "浮层坐标未命中 select 元素\n";
        }
    }

    if (options.window) {
        PreviewWindow preview(engine, *text, script.get(),
                              options.direct3d ? web::RenderBackend::Direct3D11
                                               : web::RenderBackend::OpenGL33);
        const int width = static_cast<int>(options.width);
        const int height = static_cast<int>(options.page_height > 0 ? options.page_height
                                                                    : layout->content_height);
        // 标题显示当前文件名，便于同时打开多个页面时区分。
        std::wstring title;
        if (engine.document_path().empty()) {
            title = L"自研排版引擎预览";
        } else {
            const std::string file_name =
                std::filesystem::u8path(engine.document_path()).filename().u8string();
            title = L"自研排版引擎预览 — " + Utf8ToWide(file_name);
        }
        if (!preview.Create(width, height, title)) {
            std::cerr << "创建预览窗口失败\n";
            CoUninitialize();
            return 6;
        }
        const int result = preview.Run();
        script.reset();
        CoUninitialize();
        return result;
    }

    std::filesystem::create_directories(std::filesystem::u8path(options.out_dir));

    const int first_page = options.page < 0 ? 0 : options.page;
    const int last_page =
        options.page < 0 ? layout->PageCount() - 1 : std::min(options.page, layout->PageCount() - 1);
    if (layout->PageCount() == 0 || first_page >= layout->PageCount()) {
        std::cerr << "没有可输出的页面\n";
        CoUninitialize();
        return 7;
    }

    bool all_ok = true;
    for (int page = first_page; page <= last_page; ++page) {
        RenderOutput direct3d_output;
        RenderOutput opengl_output;

        if (options.direct3d) {
            if (RenderPageWithBackend(engine, *text, web::RenderBackend::Direct3D11, page,
                                      &direct3d_output, &select_popup)) {
                const std::string path = options.out_dir + "/dx_page" + std::to_string(page) + ".png";
                std::string save_error;
                if (SaveRenderOutput(path, direct3d_output, &save_error)) {
                    std::cout << "输出 " << path << "（" << direct3d_output.width << "x"
                              << direct3d_output.height << "，" << direct3d_output.milliseconds
                              << " ms）\n";
                } else {
                    std::cerr << "保存 " << path << " 失败: " << save_error << "\n";
                    all_ok = false;
                }
            } else {
                std::cerr << "D3D11 渲染第 " << page << " 页失败: " << direct3d_output.error << "\n";
                all_ok = false;
            }
        }

        if (options.opengl) {
            if (RenderPageWithBackend(engine, *text, web::RenderBackend::OpenGL33, page,
                                      &opengl_output, &select_popup)) {
                const std::string path = options.out_dir + "/gl_page" + std::to_string(page) + ".png";
                std::string save_error;
                if (SaveRenderOutput(path, opengl_output, &save_error)) {
                    std::cout << "输出 " << path << "（" << opengl_output.width << "x"
                              << opengl_output.height << "，" << opengl_output.milliseconds
                              << " ms）\n";
                } else {
                    std::cerr << "保存 " << path << " 失败: " << save_error << "\n";
                    all_ok = false;
                }
            } else {
                std::cerr << "OpenGL 渲染第 " << page << " 页失败: " << opengl_output.error << "\n";
                all_ok = false;
            }
        }

        if (options.compare && options.direct3d && options.opengl) {
            CompareOutputs(direct3d_output, opengl_output);
        }
    }

    script.reset();
    CoUninitialize();
    return all_ok ? 0 : 8;
}
