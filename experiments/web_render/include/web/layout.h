#pragma once

#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "web/dom.h"
#include "web/style.h"
#include "web/text.h"
#include "web/types.h"

namespace web {

struct LayoutBox;

enum class BoxKind {
    Block,        // 块级元素或匿名块
    ListItem,     // display:list-item
    InlineBlock,  // 行内块（作为原子盒参与行内排版）
    Image,        // 替换元素（img、canvas、iframe）
};

// 一行内的文字片段。offset 为本行文字在源文本节点中的起始字节偏移，
// 用于阅读进度、标注与朗读定位。
struct PlacedText {
    const TextNode* node = nullptr;
    size_t offset = 0;
    std::string text;
    FontId font = 0;
    float x = 0.0f;        // 笔起点
    float baseline = 0.0f;
    float width = 0.0f;
    float letter_spacing = 0.0f;
    Color color = Black();
    bool underline = false;
    // 文字阴影的来源样式；为空表示该片段没有阴影（普通片段不保存副本）。
    const ComputedStyle* shadow_style = nullptr;
};

// 行内元素在某一行的片段盒（用于绘制行内背景、边框与圆角）。
struct PlacedInline {
    const Element* element = nullptr;
    const ComputedStyle* style = nullptr;
    RectF rect;
};

// 表单控件类型。None 表示非表单控件；其余取值由绘制层进入专用绘制分支，
// 用于区分复选框、单选框、下拉框与各类文本输入。
enum class FormControlKind {
    None,
    TextInput,
    PasswordInput,
    Checkbox,
    Radio,
    Select,
    Submit,
    Reset,
    Button,
};

// 行内原子盒（<img>、<canvas>、<iframe> 与 inline-block），按行内基线排布。
struct PlacedAtomic {
    const Element* element = nullptr;
    const ComputedStyle* style = nullptr;
    // 对应的布局盒：绘制时通过它递归绘制行内块的内部内容。
    const LayoutBox* box = nullptr;
    RectF border_box;
};

struct LineFragment {
    RectF rect;             // 行盒（含行高）在文档坐标中的位置
    float baseline = 0.0f;  // 基线 y
    std::vector<PlacedText> texts;
    std::vector<PlacedInline> inlines;  // 先绘制外层再绘制内层
    std::vector<PlacedAtomic> atomics;
};

// 布局盒。文档坐标：原点在页面内容区左上角，y 向下增大。
struct LayoutBox {
    BoxKind kind = BoxKind::Block;
    const Element* element = nullptr;  // 匿名块为 nullptr
    ComputedStyle style;

    RectF border_box;
    RectF content_box;

    float padding_top = 0.0f;
    float padding_right = 0.0f;
    float padding_bottom = 0.0f;
    float padding_left = 0.0f;

    float border_top = 0.0f;
    float border_right = 0.0f;
    float border_bottom = 0.0f;
    float border_left = 0.0f;

    // 子盒按文档顺序排列：块级子盒、原子盒（img/inline-block）。
    std::vector<std::unique_ptr<LayoutBox>> children;
    // 本盒直接容纳的行盒（不包含子盒内部的行）。
    std::vector<LineFragment> lines;

    // 列表标记文本（"•"、"1." 等），空表示没有标记。
    std::string marker;

    // 表单控件类型与选中状态。form_kind 为 None 表示普通盒；checkbox/radio
    // 的选中状态取自 checked 属性，绘制层据此决定是否绘制选中标记。
    FormControlKind form_kind = FormControlKind::None;
    bool form_checked = false;

    // 是否为行内原子盒（由所属行的原子条目负责绘制）。块级排版产生的
    // Image/InlineBlock 盒为 false，按普通子盒递归绘制。
    bool atomic = false;

    bool HasBackground() const { return style.background_color.Visible(); }
    bool HasBorder() const {
        return border_top > 0.0f || border_right > 0.0f || border_bottom > 0.0f ||
               border_left > 0.0f;
    }
};

struct LayoutOptions {
    float viewport_width = 860.0f;
    // 每页内容区高度；<= 0 表示不分页（整篇连续）。
    float page_content_height = 1200.0f;
};

// SVG 元素样式签名字段：上次计算样式时元素的 class/style/id 取值。
// 拖动、选择等交互只改变 transform 或替换图元，这些字段不变，样式可以复用。
struct SvgStyleSignature {
    std::string class_value;
    std::string style_value;
    std::string id_value;
    // 仅 SVG 根元素填写：HTML 祖先链的标签与 class/style/id 串联，
    // 用于判断继承来源是否发生变化。
    std::string ancestor_value;
};

struct LayoutResult {
    std::unique_ptr<LayoutBox> root;
    float viewport_width = 0.0f;
    float content_height = 0.0f;
    float page_content_height = 0.0f;
    // 行内片段使用的样式副本。PlacedInline 只保存指针，由本结果持有副本，
    // 保证结果存活期间指针始终有效；浅拷贝结果时副本共享，指针仍然有效。
    std::shared_ptr<std::deque<ComputedStyle>> inline_styles;
    // 每页内容区顶部在文档坐标中的 y；至少包含一个元素（0）。分页模式下由
    // 行盒边界决定切分位置，不会把一行文字切成两半。
    std::vector<float> page_starts;

    // 内嵌文档布局：键为 iframe 元素，值为对应子文档的布局结果。
    // 指针不拥有对象，生命周期与持有本结果的 Engine 一致。
    std::map<const Element*, const LayoutResult*> embedded_frames;
    // SVG 子树的元素计算样式：键为 SVG 根元素及其全部后代元素。
    // 绘制阶段据此读取 fill/stroke 等呈现属性与文本字体设置；嵌套文档
    // 各自持有独立的映射表。
    std::map<const Element*, ComputedStyle> svg_styles;
    // 与 svg_styles 对应的样式签名，供 RefreshSvgStyles 判断是否可以复用。
    std::map<const Element*, SvgStyleSignature> svg_style_signatures;
    // 本布局对应文档的基准目录，用于解析图片等相对资源路径。
    // 顶层文档取引擎自身基准目录，子文档取子引擎的基准目录。
    std::string base_directory;
    // 文档中是否至少有一个元素带 CSS 动画。窗口层据此决定是否需要按帧重绘。
    bool has_animations = false;

    int PageCount() const { return static_cast<int>(page_starts.size()); }
};

// 布局入口：从 DOM 计算样式并完成块级流、行内排版与分页。
// 同一 LayoutResult 在文档未变更时可重复用于绘制多页。
std::unique_ptr<LayoutResult> LayoutDocument(const Document& document,
                                             const StyleResolver& resolver,
                                             TextService& text,
                                             const LayoutOptions& options);

// 刷新布局结果中的 SVG 计算样式。previous 提供可复用的上次样式（可以与
// layout 是同一对象）；元素的 class/style/id 未变化且父级样式未重算时，
// 直接复用上次结果，避免每次交互重新匹配全部选择器。
void RefreshSvgStyles(const Document& document, const StyleResolver& resolver,
                      LayoutResult* layout, const LayoutResult* previous = nullptr);

// ---------------------------------------------------------------------------
// 表单文本控件辅助
// ---------------------------------------------------------------------------

// 可编辑文本：input 取 value 属性，textarea 取文本内容，
// select 取选中 option 的 value 属性或文本。其他元素返回空字符串。
std::string FormControlText(const Element& element);

// 控件显示文本：口令控件按码位替换为圆点，其余与 FormControlText 相同。
std::string FormDisplayText(const Element& element);

// 文本控件光标位置：所在行索引、行内水平偏移与行盒竖直范围（文档坐标）。
// 单行输入框始终位于第 0 行；textarea 按行盒中的文字偏移定位。
struct FormCaretPosition {
    bool valid = false;
    size_t line_index = 0;
    float x = 0.0f;
    float line_top = 0.0f;
    float line_bottom = 0.0f;
};

// 按码位索引计算光标几何。caret_index 超出文本长度时按文本末尾处理。
FormCaretPosition LocateFormCaret(const LayoutBox& box, const std::string& text,
                                  size_t caret_index, TextService& text_service);

// 文本控件某一行对应的码位区间、行宽与行盒竖直范围。
struct FormLineRange {
    bool valid = false;
    size_t begin_index = 0;
    size_t end_index = 0;
    float width = 0.0f;
    float line_top = 0.0f;
    float line_bottom = 0.0f;
};

// 查询第 line_index 行的码位区间。单行控件无论 line_index 取值都返回整段文本；
// line_index 超出文本行数时返回无效结果。
FormLineRange GetFormLineRange(const LayoutBox& box, const std::string& text, size_t line_index,
                               TextService& text_service);

}  // namespace web
