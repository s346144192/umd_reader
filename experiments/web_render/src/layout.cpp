#include "web/layout.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "web/utf8.h"

namespace web {
namespace {

constexpr float kEpsilon = 0.01f;

// ---------------------------------------------------------------------------
// 数值与样式工具
// ---------------------------------------------------------------------------

float ClampNonNegative(float value) {
    return value > 0.0f ? value : 0.0f;
}

// 解析 CSS 长度为像素值。percent_base 由调用方按属性语义给出：宽度、外边距、
// 内边距与高度都相对包含块内容宽度解析（本项目按任务约定统一采用宽度）。
// font_size 用于 em/ex/ch，root_font_size 用于 rem，视口单位使用视口尺寸。
float ResolveLength(const CssLength& length, float font_size, float root_font_size,
                    float percent_base, float viewport_width, float viewport_height) {
    switch (length.unit) {
        case LengthUnit::Auto:
            return 0.0f;
        case LengthUnit::Px:
        case LengthUnit::Number:
            return length.value;
        case LengthUnit::Em:
            return length.value * font_size;
        case LengthUnit::Ex:
            return length.value * font_size * 0.5f;
        case LengthUnit::Rem:
            return length.value * root_font_size;
        case LengthUnit::Percent:
            return length.value * percent_base / 100.0f;
        case LengthUnit::CalcPercent:
            return length.value * percent_base / 100.0f + length.offset;
        case LengthUnit::Pt:
            return length.value * 96.0f / 72.0f;
        case LengthUnit::Pc:
            return length.value * 16.0f;
        case LengthUnit::In:
            return length.value * 96.0f;
        case LengthUnit::Cm:
            return length.value * 96.0f / 2.54f;
        case LengthUnit::Mm:
            return length.value * 96.0f / 25.4f;
        case LengthUnit::Ch:
            return length.value * font_size * 0.5f;
        case LengthUnit::Vw:
            return length.value * viewport_width / 100.0f;
        case LengthUnit::Vh:
            return length.value * viewport_height / 100.0f;
        case LengthUnit::Vmin:
            return length.value * std::min(viewport_width, viewport_height) / 100.0f;
        case LengthUnit::Vmax:
            return length.value * std::max(viewport_width, viewport_height) / 100.0f;
    }
    return length.value;
}

float ResolveMarginLength(const CssLength& length, float font_size, float root_font_size,
                          float percent_base, float viewport_width, float viewport_height) {
    if (length.IsAuto()) {
        return 0.0f;
    }
    return ResolveLength(length, font_size, root_font_size, percent_base, viewport_width,
                         viewport_height);
}

// 两个相邻外边距合并。
// 两个都为正时取较大者，两个都为负时取绝对值较大的负值（即较小者），
// 一正一负时相加。这与 CSS 边距合并的常用规则一致。
float CollapseMargins(float first, float second) {
    if (first >= 0.0f && second >= 0.0f) {
        return std::max(first, second);
    }
    if (first <= 0.0f && second <= 0.0f) {
        return std::min(first, second);
    }
    return first + second;
}

// 解析 HTML width/height 属性中的像素或百分比尺寸；无法解析时返回 negative
// 表示“没有可用值”。
float ParseHtmlDimension(const std::string& value, float percent_base) {
    std::string trimmed = value;
    TrimAsciiSpace(&trimmed);
    if (trimmed.empty()) {
        return -1.0f;
    }
    if (trimmed.size() > 1 && trimmed.back() == '%') {
        std::string numeric = trimmed.substr(0, trimmed.size() - 1);
        TrimAsciiSpace(&numeric);
        char* end = nullptr;
        const float number = std::strtof(numeric.c_str(), &end);
        if (end == numeric.c_str() || end == nullptr || *end != '\0') {
            return -1.0f;
        }
        return number * percent_base / 100.0f;
    }
    if (trimmed.size() > 2 && trimmed[trimmed.size() - 2] == 'p' &&
        (trimmed.back() == 'x' || trimmed.back() == 'X')) {
        trimmed.resize(trimmed.size() - 2);
        TrimAsciiSpace(&trimmed);
    }
    char* end = nullptr;
    const float number = std::strtof(trimmed.c_str(), &end);
    if (end == trimmed.c_str() || end == nullptr || *end != '\0') {
        return -1.0f;
    }
    return number;
}

// 行内元素计算样式的稳定副本由 LayoutEngine 持有，并交由 LayoutResult 共享保存，
// 使 PlacedInline 中的样式指针在结果存活期间始终有效；嵌套文档各自使用独立副本，
// 不会互相清除。

// ---------------------------------------------------------------------------
// 字体与测量
// ---------------------------------------------------------------------------

struct FontKey {
    std::string family;
    float size = 16.0f;
    bool bold = false;
    bool italic = false;

    bool operator<(const FontKey& other) const {
        if (family != other.family) {
            return family < other.family;
        }
        if (size != other.size) {
            return size < other.size;
        }
        if (bold != other.bold) {
            return bold < other.bold;
        }
        return static_cast<int>(italic) < static_cast<int>(other.italic);
    }
};

struct EdgeValues {
    float margin_top = 0.0f;
    float margin_right = 0.0f;
    float margin_bottom = 0.0f;
    float margin_left = 0.0f;
    float padding_top = 0.0f;
    float padding_right = 0.0f;
    float padding_bottom = 0.0f;
    float padding_left = 0.0f;
    float border_top = 0.0f;
    float border_right = 0.0f;
    float border_bottom = 0.0f;
    float border_left = 0.0f;
};

// ---------------------------------------------------------------------------
// 行内排版数据结构
// ---------------------------------------------------------------------------

// 行内元素在某一行的片段盒（用于绘制行内背景、边框与圆角）。
struct InlineFrame {
    const Element* element = nullptr;
    const ComputedStyle* style = nullptr;
};

// 行内流中的一个不可再分的单元：文字片段、原子盒或强制换行。
struct InlineAtom {
    enum class Kind {
        Text,
        Atomic,
        Break,
    };

    Kind kind = Kind::Text;
    // 从外层到内层的行内元素链，用于生成 PlacedInline 片段。
    std::vector<InlineFrame> frames;

    // 文字与通用度量：ascent 为行顶到基线的距离，descent 为基线到行底的距离。
    const TextNode* node = nullptr;
    size_t offset = 0;
    std::string text;
    FontId font = 0;
    float width = 0.0f;
    float ascent = 0.0f;
    float descent = 0.0f;
    float line_height = 0.0f;
    float letter_spacing = 0.0f;
    Color color = Black();
    bool underline = false;
    // 文字阴影的来源样式；为空表示该片段没有阴影。
    const ComputedStyle* style = nullptr;
    bool space = false;
    bool collapsible_space = false;
    bool allow_wrap = true;
    // 任意字符断行（word-break: break-all）与长单词溢出时断行
    // （overflow-wrap: break-word/anywhere）。拆分在行排版前完成。
    bool break_all = false;
    bool break_anywhere = false;

    // 原子盒（<img> 与 inline-block）。
    const Element* atomic_element = nullptr;
    LayoutBox* atomic_box = nullptr;
    float atomic_margin_left = 0.0f;
    float atomic_margin_right = 0.0f;
    float atomic_margin_top = 0.0f;
    float atomic_margin_bottom = 0.0f;
    float atomic_border_width = 0.0f;
    float atomic_border_height = 0.0f;
    float atomic_outer_width = 0.0f;
    float atomic_baseline_offset = 0.0f;
};

struct LinePlan {
    std::vector<size_t> atom_indices;
    float width = 0.0f;
    bool ended_by_break = false;
    bool empty_line = false;
    float empty_ascent = 0.0f;
    float empty_descent = 0.0f;
};

// ---------------------------------------------------------------------------
// 表格布局数据结构
// ---------------------------------------------------------------------------

// 一个表格单元格的排版输入：元素、计算样式、盒模型边缘与列占位。
struct TableCellPlan {
    const Element* element = nullptr;
    ComputedStyle style;

    float padding_top = 0.0f;
    float padding_right = 0.0f;
    float padding_bottom = 0.0f;
    float padding_left = 0.0f;
    float border_top = 0.0f;
    float border_right = 0.0f;
    float border_bottom = 0.0f;
    float border_left = 0.0f;

    int column_index = 0;  // 起始列索引，按行内顺序分配并跳过被占用的位置
    int column_span = 1;   // colspan，缺省 1
    int row_span = 1;      // rowspan，缺省 1
    int row_index = 0;     // 所在行的索引，由收集阶段填写
    // 内容固有宽度加上左右内边距与左右边框，用于计算列宽。
    float intrinsic_width = 0.0f;
};

// 一行单元格。不保留行盒，行高由本行单元格的边框盒高度与跨行单元格分摊决定。
struct TableRowPlan {
    std::vector<TableCellPlan> cells;
    int column_count = 0;  // 本行按 colspan 累计占用的列数
};

// 已排布的单元格：盒与在网格中的行列范围，用于行高分配与最终定位。
struct LaidTableCell {
    std::unique_ptr<LayoutBox> box;
    size_t column_begin = 0;
    size_t column_span = 1;
    size_t row_begin = 0;
    size_t row_end = 1;  // 不含，等于 row_begin + rowspan（受总行数限制）
};

// 读取 colspan 属性：缺省或非法时按 1。异常的大数值限制为 1000，
// 避免属性错误导致列数过多而占用过大内存。
int ParseTableColumnSpan(const Element& element) {
    std::string text = element.GetAttribute("colspan");
    TrimAsciiSpace(&text);
    if (text.empty()) {
        return 1;
    }
    char* end = nullptr;
    const long parsed = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str() || end == nullptr || *end != '\0') {
        return 1;
    }
    if (parsed < 1) {
        return 1;
    }
    if (parsed > 1000) {
        return 1000;
    }
    return static_cast<int>(parsed);
}

// 读取 rowspan 属性：缺省或非法时按 1；小于 1 的取值（含 0）按 1 处理，
// 异常的大数值限制为 1000。
int ParseTableRowSpan(const Element& element) {
    std::string text = element.GetAttribute("rowspan");
    TrimAsciiSpace(&text);
    if (text.empty()) {
        return 1;
    }
    char* end = nullptr;
    const long parsed = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str() || end == nullptr || *end != '\0') {
        return 1;
    }
    if (parsed < 1) {
        return 1;
    }
    if (parsed > 1000) {
        return 1000;
    }
    return static_cast<int>(parsed);
}

// 整体平移布局子树。行内块先在原点完成内部布局，放置到行内时统一平移。
void TranslateBox(LayoutBox* box, float dx, float dy);
// 以 (center_x, center_y) 为原点缩放布局子树，用于 transform: scale()。
void ScaleBox(LayoutBox* box, float center_x, float center_y, float scale_x, float scale_y);

// 表格间距分量：正值为像素；负值为百分比记号（绝对值为百分比数值），
// 按基准宽度换算。结果不小于 0。
float ResolveBorderSpacingComponent(float stored, float percent_base) {
    if (stored < 0.0f) {
        return ClampNonNegative(-stored * percent_base / 100.0f);
    }
    return ClampNonNegative(stored);
}

// ---------------------------------------------------------------------------
// 布局引擎
// ---------------------------------------------------------------------------

class LayoutEngine {
public:
    LayoutEngine(const StyleResolver& resolver, TextService& text,
                 const LayoutOptions& options)
        : resolver_(resolver), text_(text), options_(options) {}

    std::unique_ptr<LayoutResult> Run(const Document& document);

private:
    struct FlowItem {
        bool is_block = false;
        const Element* element = nullptr;
        ComputedStyle style;
        std::vector<const Node*> inline_nodes;
        // 匿名行内片段承载伪元素内容时使用的标记（没有真实节点）。
        bool pseudo_before = false;
        bool pseudo_after = false;
        float margin_top = 0.0f;
        float margin_bottom = 0.0f;
        float margin_left = 0.0f;
        float margin_right = 0.0f;
        int list_ordinal = 0;
    };

    // 字体缓存：相同请求复用 FontId；字形步进按 (字体, 码位) 缓存。
    FontId FontFor(const ComputedStyle& style);
    const FontMetrics& MetricsFor(FontId font, float font_size);
    float Advance(FontId font, uint32_t codepoint);
    float TextWidth(FontId font, const std::string& text, float letter_spacing);
    float ResolveLineHeight(const ComputedStyle& style, const FontMetrics& metrics);

    EdgeValues ResolveEdges(const ComputedStyle& style, float percent_base, float root_font_size);
    float ViewportHeight() const;

    // 外边距合并辅助：首/末子块造成的合并结果。
    bool CanCollapseWithFirstChild(const ComputedStyle& style, const EdgeValues& edges) const;
    bool CanCollapseWithLastChild(const ComputedStyle& style, const EdgeValues& edges) const;
    float OuterMarginTop(const ComputedStyle& style, const Element* element,
                         float containing_width, float root_font_size);
    float OuterMarginBottom(const ComputedStyle& style, const Element* element,
                            float containing_width, float root_font_size);

    // 块级排版。containing_height 为包含块的确定高度：非负时百分比高度按它解析，
    // 为 -1 表示高度不确定（沿用百分比按宽度解析的兼容行为）。
    // height_override 非负时直接指定内容高度，用于弹性项目在主轴尺寸确定后重新排版。
    std::unique_ptr<LayoutBox> LayoutBlock(const Element* element, const ComputedStyle& style,
                                           float border_left, float border_top,
                                           float containing_width, float root_font_size,
                                           int list_ordinal, float width_override,
                                           float containing_height = -1.0f,
                                           float height_override = -1.0f);
    void BuildFlowItems(const Element& element, const ComputedStyle& parent_style,
                        float containing_width, float root_font_size, bool flex_items,
                        std::vector<FlowItem>* items);
    // 弹性盒排版：子盒追加到 box->children，返回容器内容区高度。
    // container_height 为容器的确定内容高度，-1 表示由内容决定。
    float LayoutFlexChildren(const ComputedStyle& style, const std::vector<FlowItem>& items,
                             float content_left, float content_top, float content_width,
                             float root_font_size, LayoutBox* box, float container_height);
    // 网格排版：子盒按列轨道放置，返回容器内容区高度。
    float LayoutGridChildren(const ComputedStyle& style, const std::vector<FlowItem>& items,
                             float content_left, float content_top, float content_width,
                             float root_font_size, LayoutBox* box);
    // 定位盒（absolute/fixed）排版：按包含块与偏移确定位置与尺寸。
    void LayoutPositioned(const FlowItem& item, const RectF& containing_block,
                          float root_font_size, LayoutBox* parent);
    std::string BuildMarker(const ComputedStyle& style, int list_ordinal) const;

    // 表格排版：以标签名 table 驱动，单元格按行顺序成为表格盒的子盒。
    std::unique_ptr<LayoutBox> LayoutTable(const Element* element, const ComputedStyle& style,
                                           float border_left, float border_top,
                                           float containing_width, float root_font_size,
                                           float width_override);
    void CollectTableRows(const Element& table, const ComputedStyle& table_style,
                          float containing_width, float root_font_size,
                          const Element** caption_element, std::vector<TableRowPlan>* rows);
    void CollectTableRow(const Element& row_element, const ComputedStyle& table_style,
                         float containing_width, float root_font_size, int row_index,
                         std::vector<int>* occupied_until, TableRowPlan* row);

    // 行内内容构建。
    void BuildInlineRun(const std::vector<const Node*>& nodes, const ComputedStyle& base_style,
                        float available_width, LayoutBox* owner,
                        std::vector<InlineAtom>* atoms);
    void BuildInlineNode(const Element& element, const ComputedStyle& style,
                         const std::vector<InlineFrame>& frames, float available_width,
                         LayoutBox* owner, std::vector<InlineAtom>* atoms);
    void AppendTextAtoms(const TextNode& node, const ComputedStyle& style,
                         const std::vector<InlineFrame>& frames, std::vector<InlineAtom>* atoms);
    // 由字符串生成文字原子，与 AppendTextAtoms 共享空白折叠与断行规则。
    // node 为来源文本节点；合成文本（表单控件取值）传 nullptr。
    void AppendTextAtomsFromString(const std::string& data, const TextNode* node,
                                   const ComputedStyle& style,
                                   const std::vector<InlineFrame>& frames,
                                   std::vector<InlineAtom>* atoms);
    void AppendBreakAtom(const ComputedStyle& style, const std::vector<InlineFrame>& frames,
                         std::vector<InlineAtom>* atoms);
    void AppendImageAtom(const Element& element, const ComputedStyle& style,
                         const std::vector<InlineFrame>& frames, float available_width,
                         LayoutBox* owner, std::vector<InlineAtom>* atoms);
    void AppendInlineBlockAtom(const Element& element, const ComputedStyle& style,
                               const std::vector<InlineFrame>& frames, float available_width,
                               LayoutBox* owner, std::vector<InlineAtom>* atoms);
    // input 与 select 的原子盒：按控件类型确定尺寸与标签文本。
    void AppendFormControlAtom(const Element& element, const ComputedStyle& style,
                               const std::vector<InlineFrame>& frames, float available_width,
                               LayoutBox* owner, std::vector<InlineAtom>* atoms);
    float MeasureInlineIntrinsicWidth(const Element& element, const ComputedStyle& style,
                                      float available_width);
    // 表格单元格固有宽度：行内内容沿用 MeasureInlineIntrinsicWidth；块级子元素
    // 递归统计其内容的最大固有宽度，并计入其自身的水平边缘。
    float MeasureTableCellIntrinsicWidth(const Element& element, const ComputedStyle& style,
                                         float available_width);

    // 行内元素计算样式的稳定副本；由 LayoutResult 共享持有。
    const ComputedStyle* StoreInlineStyle(const ComputedStyle& style);

    // 伪元素 ::before / ::after：存在 content 时生成文字原子；内容为空但有
    // 背景/边框/内边距时生成一个行内原子盒。owner 用于挂载原子盒。
    // 绝对定位与固定定位的伪元素不进入行内流，样式收集到 positioned_pseudos，
    // 由宿主布局完成后按定位规则单独生成盒子。
    bool BuildPseudoAtoms(const Element& element, const ComputedStyle& origin_style,
                          const std::string& pseudo_name, LayoutBox* owner,
                          std::vector<InlineAtom>* atoms,
                          std::vector<ComputedStyle>* positioned_pseudos = nullptr);

    // 行排版。
    float LayoutLines(const std::vector<InlineAtom>& atoms, float content_width,
                      const ComputedStyle& block_style, float content_left, float content_top,
                      LayoutBox* owner);
    void BuildLinePlans(const std::vector<InlineAtom>& atoms, float content_width,
                        float first_line_indent, std::vector<LinePlan>* plans);

    // 分页。
    void CollectBreakCandidates(const LayoutBox& box, float page_height,
                                std::vector<float>* candidates) const;
    void BuildPageStarts(std::unique_ptr<LayoutResult>* result);

    const StyleResolver& resolver_;
    TextService& text_;
    LayoutOptions options_;
    float root_font_size_ = 16.0f;
    float viewport_height_ = 0.0f;

    std::map<FontKey, FontId> fonts_;
    std::map<FontId, FontMetrics> metrics_;
    std::map<std::pair<FontId, uint32_t>, float> advances_;
    std::shared_ptr<std::deque<ComputedStyle>> inline_styles_;
    // 当前定位包含块（最近的非 static 祖先的 padding box，或视口）。
    RectF positioning_block_;
};

const ComputedStyle* LayoutEngine::StoreInlineStyle(const ComputedStyle& style) {
    if (!inline_styles_) {
        inline_styles_ = std::make_shared<std::deque<ComputedStyle>>();
    }
    inline_styles_->push_back(style);
    return &inline_styles_->back();
}

/* 伪元素内容：规则存在 content 且伪元素未被 display:none 隐藏时，把内容文本
   按伪元素自身的字体、颜色与空白规则生成行内原子。伪元素的背景与边框通过
   行内片段样式绘制，覆盖文字所占的水平区间。 */
bool LayoutEngine::BuildPseudoAtoms(const Element& element, const ComputedStyle& origin_style,
                                    const std::string& pseudo_name, LayoutBox* owner,
                                    std::vector<InlineAtom>* atoms,
                                    std::vector<ComputedStyle>* positioned_pseudos) {
    if (atoms == nullptr) {
        return false;
    }
    ComputedStyle pseudo;
    if (!resolver_.ComputePseudoFor(element, origin_style, pseudo_name, &pseudo)) {
        return false;
    }
    if (pseudo.display == Display::None) {
        return false;
    }
    /* 绝对定位与固定定位的伪元素脱离普通流：记录样式，稍后按定位规则生成盒子，
       不参与行内流，避免装饰性伪元素（按钮水波纹、光效层）撑大宿主盒。 */
    if (pseudo.position == Position::Absolute || pseudo.position == Position::Fixed) {
        if (positioned_pseudos != nullptr) {
            positioned_pseudos->push_back(pseudo);
        }
        return false;
    }
    if (pseudo.pseudo_content.empty()) {
        /* content 为空但仍设置背景、边框或内边距时（常见于色块装饰），
           生成一个无文字的行内原子盒，使装饰能够占据实际区域。 */
        const EdgeValues edges = ResolveEdges(pseudo, origin_style.width.IsAuto()
                                                         ? 300.0f
                                                         : ResolveLength(origin_style.width,
                                                                         origin_style.font_size,
                                                                         root_font_size_, 300.0f,
                                                                         options_.viewport_width,
                                                                         ViewportHeight()),
                                              root_font_size_);
        const bool decorated = pseudo.background_color.Visible() ||
                               edges.border_left > 0.0f || edges.border_right > 0.0f ||
                               edges.border_top > 0.0f || edges.border_bottom > 0.0f ||
                               edges.padding_left > 0.0f || edges.padding_right > 0.0f ||
                               edges.padding_top > 0.0f || edges.padding_bottom > 0.0f;
        if (!decorated || owner == nullptr) {
            return false;
        }
        const FontId font = FontFor(pseudo);
        const FontMetrics& metrics = MetricsFor(font, pseudo.font_size);
        const float line_height = ResolveLineHeight(pseudo, metrics);

        float width = edges.padding_left + edges.padding_right + edges.border_left +
                      edges.border_right;
        if (!pseudo.width.IsAuto()) {
            width = ClampNonNegative(ResolveLength(pseudo.width, pseudo.font_size,
                                                   root_font_size_, 300.0f,
                                                   options_.viewport_width, ViewportHeight())) +
                    edges.padding_left + edges.padding_right + edges.border_left +
                    edges.border_right;
        }
        float height = line_height + edges.padding_top + edges.padding_bottom +
                       edges.border_top + edges.border_bottom;
        if (!pseudo.height.IsAuto()) {
            height = ClampNonNegative(ResolveLength(pseudo.height, pseudo.font_size,
                                                    root_font_size_, 300.0f,
                                                    options_.viewport_width, ViewportHeight())) +
                     edges.padding_top + edges.padding_bottom + edges.border_top +
                     edges.border_bottom;
        }

        auto box = std::make_unique<LayoutBox>();
        box->kind = BoxKind::InlineBlock;
        box->atomic = true;
        box->element = nullptr;
        box->style = pseudo;
        box->padding_top = edges.padding_top;
        box->padding_right = edges.padding_right;
        box->padding_bottom = edges.padding_bottom;
        box->padding_left = edges.padding_left;
        box->border_top = edges.border_top;
        box->border_right = edges.border_right;
        box->border_bottom = edges.border_bottom;
        box->border_left = edges.border_left;
        box->border_box = RectF::FromLTRB(0.0f, 0.0f, width, height);
        box->content_box = RectF::FromLTRB(edges.border_left + edges.padding_left,
                                           edges.border_top + edges.padding_top,
                                           width - edges.border_right - edges.padding_right,
                                           height - edges.border_bottom - edges.padding_bottom);

        LayoutBox* box_pointer = box.get();
        owner->children.push_back(std::move(box));

        InlineAtom atom;
        atom.kind = InlineAtom::Kind::Atomic;
        atom.atomic_element = &element;
        atom.atomic_box = box_pointer;
        atom.atomic_border_width = width;
        atom.atomic_border_height = height;
        atom.atomic_outer_width = width;
        atom.width = width;
        atom.atomic_baseline_offset = height;
        atom.ascent = height;
        atom.descent = 0.0f;
        atom.allow_wrap = true;
        atoms->push_back(std::move(atom));
        return true;
    }
    std::vector<InlineFrame> frames;
    InlineFrame frame;
    frame.element = &element;
    frame.style = StoreInlineStyle(pseudo);
    frames.push_back(frame);
    AppendTextAtomsFromString(pseudo.pseudo_content, nullptr, pseudo, frames, atoms);
    return true;
}

bool IsCollapsibleWhitespaceOnly(const std::string& data) {
    for (char character : data) {
        if (!IsAsciiSpace(character)) {
            return false;
        }
    }
    return true;
}

// 口令控件按码位把文本替换为圆点，每个码位对应一个圆点。
std::string MaskPasswordText(const std::string& text) {
    std::string masked;
    size_t offset = 0;
    while (offset < text.size()) {
        const Utf8Char character = DecodeUtf8(text, offset);
        if (character.length == 0) {
            break;
        }
        masked += "\xE2\x80\xA2";  // U+2022 "•"
        offset += character.length;
    }
    return masked;
}

// 收集 select 下的全部 option，按文档顺序递归进入 optgroup。
void CollectSelectOptions(const Element& parent, std::vector<const Element*>* options) {
    for (const NodePtr& child : parent.Children()) {
        if (!child || !child->IsElement()) {
            continue;
        }
        const Element& element = static_cast<const Element&>(*child);
        if (element.TagName() == "option") {
            options->push_back(&element);
        } else if (element.TagName() == "optgroup") {
            CollectSelectOptions(element, options);
        }
    }
}

// 下拉框显示文本：优先取带 selected 属性的 option，否则取第一个 option。
// 没有任何 option 时返回空字符串。
std::string SelectControlLabel(const Element& element) {
    std::vector<const Element*> options;
    CollectSelectOptions(element, &options);
    const Element* chosen = nullptr;
    for (const Element* option : options) {
        if (option->HasAttribute("selected")) {
            chosen = option;
            break;
        }
    }
    if (chosen == nullptr && !options.empty()) {
        chosen = options.front();
    }
    if (chosen == nullptr) {
        return std::string();
    }
    std::string text = TextContent(*chosen);
    TrimAsciiSpace(&text);
    return text;
}

// ---------------------------------------------------------------------------
// 字体缓存与度量
// ---------------------------------------------------------------------------

FontId LayoutEngine::FontFor(const ComputedStyle& style) {
    FontKey key;
    key.family = style.font_family;
    key.size = style.font_size > 0.0f ? style.font_size : 16.0f;
    key.bold = style.font_weight >= 600;
    key.italic = style.italic;

    auto found = fonts_.find(key);
    if (found != fonts_.end()) {
        return found->second;
    }

    FontRequest request;
    request.family = key.family;
    request.size = key.size;
    request.bold = key.bold;
    request.italic = key.italic;
    const FontId font = text_.CreateFont(request);
    fonts_.emplace(key, font);
    if (font != 0) {
        metrics_[font] = text_.Metrics(font);
    }
    return font;
}

const FontMetrics& LayoutEngine::MetricsFor(FontId font, float font_size) {
    auto found = metrics_.find(font);
    if (found != metrics_.end()) {
        return found->second;
    }

    FontMetrics metrics;
    if (font != 0) {
        metrics = text_.Metrics(font);
    }
    if (metrics.height <= 0.0f) {
        // 字体服务未给出有效度量时用字号推导，保证行盒仍有高度。
        const float size = font_size > 0.0f ? font_size : 16.0f;
        metrics.ascent = size * 0.8f;
        metrics.descent = size * 0.2f;
        metrics.line_gap = 0.0f;
        metrics.height = metrics.ascent + metrics.descent;
    }
    auto inserted = metrics_.emplace(font, metrics);
    return inserted.first->second;
}

float LayoutEngine::Advance(FontId font, uint32_t codepoint) {
    if (font == 0) {
        return 0.0f;
    }
    const std::pair<FontId, uint32_t> key(font, codepoint);
    auto found = advances_.find(key);
    if (found != advances_.end()) {
        return found->second;
    }
    const float advance = text_.Advance(font, codepoint);
    advances_.emplace(key, advance);
    return advance;
}

float LayoutEngine::TextWidth(FontId font, const std::string& text, float letter_spacing) {
    float width = 0.0f;
    size_t offset = 0;
    while (offset < text.size()) {
        const Utf8Char character = DecodeUtf8(text, offset);
        if (character.length == 0) {
            break;
        }
        width += Advance(font, character.codepoint) + letter_spacing;
        offset += character.length;
    }
    return width;
}

// line-height 为 Number 时是字号倍数，Auto 取字体度量高度，其余按长度解析
// （百分比相对字号）。
float LayoutEngine::ResolveLineHeight(const ComputedStyle& style, const FontMetrics& metrics) {
    const CssLength& line_height = style.line_height;
    float height = 0.0f;
    if (line_height.IsNumber()) {
        height = line_height.value * style.font_size;
    } else if (line_height.IsAuto()) {
        height = metrics.height;
    } else {
        height = ResolveLength(line_height, style.font_size, root_font_size_, style.font_size,
                               options_.viewport_width, ViewportHeight());
    }
    return ClampNonNegative(height);
}

float LayoutEngine::ViewportHeight() const {
    return viewport_height_;
}

// ---------------------------------------------------------------------------
// 盒模型边缘解析
// ---------------------------------------------------------------------------

EdgeValues LayoutEngine::ResolveEdges(const ComputedStyle& style, float percent_base,
                                      float root_font_size) {
    const float font_size = style.font_size;
    const float viewport_width = options_.viewport_width;
    const float viewport_height = ViewportHeight();

    EdgeValues edges;
    edges.margin_top = ResolveMarginLength(style.margin.top, font_size, root_font_size,
                                           percent_base, viewport_width, viewport_height);
    edges.margin_right = ResolveMarginLength(style.margin.right, font_size, root_font_size,
                                             percent_base, viewport_width, viewport_height);
    edges.margin_bottom = ResolveMarginLength(style.margin.bottom, font_size, root_font_size,
                                              percent_base, viewport_width, viewport_height);
    edges.margin_left = ResolveMarginLength(style.margin.left, font_size, root_font_size,
                                            percent_base, viewport_width, viewport_height);

    edges.padding_top = ClampNonNegative(ResolveLength(style.padding.top, font_size, root_font_size,
                                                       percent_base, viewport_width,
                                                       viewport_height));
    edges.padding_right = ClampNonNegative(ResolveLength(style.padding.right, font_size,
                                                         root_font_size, percent_base,
                                                         viewport_width, viewport_height));
    edges.padding_bottom = ClampNonNegative(ResolveLength(style.padding.bottom, font_size,
                                                          root_font_size, percent_base,
                                                          viewport_width, viewport_height));
    edges.padding_left = ClampNonNegative(ResolveLength(style.padding.left, font_size,
                                                        root_font_size, percent_base,
                                                        viewport_width, viewport_height));

    edges.border_top = ClampNonNegative(ResolveLength(style.border_width.top, font_size,
                                                      root_font_size, percent_base,
                                                      viewport_width, viewport_height));
    edges.border_right = ClampNonNegative(ResolveLength(style.border_width.right, font_size,
                                                        root_font_size, percent_base,
                                                        viewport_width, viewport_height));
    edges.border_bottom = ClampNonNegative(ResolveLength(style.border_width.bottom, font_size,
                                                         root_font_size, percent_base,
                                                         viewport_width, viewport_height));
    edges.border_left = ClampNonNegative(ResolveLength(style.border_width.left, font_size,
                                                       root_font_size, percent_base,
                                                       viewport_width, viewport_height));
    return edges;
}

// ---------------------------------------------------------------------------
// 垂直外边距合并
// ---------------------------------------------------------------------------

bool LayoutEngine::CanCollapseWithFirstChild(const ComputedStyle& style,
                                             const EdgeValues& edges) const {
    if (style.display != Display::Block && style.display != Display::ListItem) {
        return false;
    }
    return edges.border_top <= 0.0f && edges.padding_top <= 0.0f;
}

bool LayoutEngine::CanCollapseWithLastChild(const ComputedStyle& style,
                                            const EdgeValues& edges) const {
    if (style.display != Display::Block && style.display != Display::ListItem) {
        return false;
    }
    if (!style.height.IsAuto()) {
        return false;
    }
    return edges.border_bottom <= 0.0f && edges.padding_bottom <= 0.0f;
}

// 计算块在父流中的外侧上边距：当自身没有上边框与上内边距时，与第一个
// 流内块级子块（或匿名块，其边距为 0）的上边距合并。
float LayoutEngine::OuterMarginTop(const ComputedStyle& style, const Element* element,
                                   float containing_width, float root_font_size) {
    const EdgeValues edges = ResolveEdges(style, containing_width, root_font_size);
    const float margin_top = edges.margin_top;
    if (element == nullptr || !CanCollapseWithFirstChild(style, edges)) {
        return margin_top;
    }

    for (const NodePtr& child : element->Children()) {
        if (!child) {
            continue;
        }
        if (child->IsText()) {
            const TextNode& text_node = static_cast<const TextNode&>(*child);
            const bool collapses = style.white_space == WhiteSpace::Normal ||
                                   style.white_space == WhiteSpace::NoWrap ||
                                   style.white_space == WhiteSpace::PreLine;
            if (collapses && IsCollapsibleWhitespaceOnly(text_node.Data())) {
                continue;
            }
            // 首个流内内容是行内内容，构成外边距为 0 的匿名块。
            return CollapseMargins(margin_top, 0.0f);
        }
        if (!child->IsElement()) {
            continue;
        }
        const Element& child_element = static_cast<const Element&>(*child);
        const ComputedStyle child_style = resolver_.ComputeFor(child_element, style);
        if (child_style.display == Display::None) {
            continue;
        }
        if (child_style.IsBlockLevel()) {
            const EdgeValues child_edges =
                ResolveEdges(child_style, containing_width, root_font_size);
            return CollapseMargins(margin_top, child_edges.margin_top);
        }
        if (child_element.ChildCount() == 0 && child_element.TagName() != "br" &&
            child_element.TagName() != "img" && child_element.TagName() != "iframe" &&
            child_element.TagName() != "svg") {
            // 空的普通行内元素不产生行盒，不影响外边距合并。
            continue;
        }
        return CollapseMargins(margin_top, 0.0f);
    }
    return margin_top;
}

// 计算块在父流中的外侧下边距：当高度为 auto 且没有下边框与下内边距时，
// 与最后一个流内块级子块的下边距合并。
float LayoutEngine::OuterMarginBottom(const ComputedStyle& style, const Element* element,
                                      float containing_width, float root_font_size) {
    const EdgeValues edges = ResolveEdges(style, containing_width, root_font_size);
    const float margin_bottom = edges.margin_bottom;
    if (element == nullptr || !CanCollapseWithLastChild(style, edges)) {
        return margin_bottom;
    }

    const std::vector<NodePtr>& children = element->Children();
    for (auto iterator = children.rbegin(); iterator != children.rend(); ++iterator) {
        const NodePtr& child = *iterator;
        if (!child) {
            continue;
        }
        if (child->IsText()) {
            const TextNode& text_node = static_cast<const TextNode&>(*child);
            const bool collapses = style.white_space == WhiteSpace::Normal ||
                                   style.white_space == WhiteSpace::NoWrap ||
                                   style.white_space == WhiteSpace::PreLine;
            if (collapses && IsCollapsibleWhitespaceOnly(text_node.Data())) {
                continue;
            }
            return CollapseMargins(margin_bottom, 0.0f);
        }
        if (!child->IsElement()) {
            continue;
        }
        const Element& child_element = static_cast<const Element&>(*child);
        const ComputedStyle child_style = resolver_.ComputeFor(child_element, style);
        if (child_style.display == Display::None) {
            continue;
        }
        if (child_style.IsBlockLevel()) {
            const EdgeValues child_edges =
                ResolveEdges(child_style, containing_width, root_font_size);
            return CollapseMargins(margin_bottom, child_edges.margin_bottom);
        }
        if (child_element.ChildCount() == 0 && child_element.TagName() != "br" &&
            child_element.TagName() != "img" && child_element.TagName() != "iframe" &&
            child_element.TagName() != "svg") {
            continue;
        }
        return CollapseMargins(margin_bottom, 0.0f);
    }
    return margin_bottom;
}

// ---------------------------------------------------------------------------
// 布局入口
// ---------------------------------------------------------------------------

std::unique_ptr<LayoutResult> LayoutEngine::Run(const Document& document) {
    // 每次布局使用独立的行内样式副本，避免嵌套文档或连续布局清除彼此的数据。
    inline_styles_ = std::make_shared<std::deque<ComputedStyle>>();

    auto result = std::make_unique<LayoutResult>();
    result->inline_styles = inline_styles_;
    result->viewport_width = options_.viewport_width;
    result->page_content_height =
        options_.page_content_height > 0.0f ? options_.page_content_height : 0.0f;

    // 视口高度用于 vh/vmin/vmax；无分页时取页面高度的近似值。
    viewport_height_ = result->page_content_height > 0.0f ? result->page_content_height : 800.0f;
    // 初始定位包含块为视口：根级 absolute 与 fixed 都以视口为参照。
    positioning_block_ = RectF::FromLTRB(0.0f, 0.0f, options_.viewport_width, viewport_height_);

    const ComputedStyle initial = resolver_.InitialStyle();
    root_font_size_ = initial.font_size > 0.0f ? initial.font_size : 16.0f;
    // 根元素的计算样式同时作为 body 的父样式，保证 html 上声明的可继承属性
    // （含 :root 中的自定义属性）能向下传递。
    ComputedStyle root_style = initial;
    if (const Element* root_element = document.RootElement()) {
        root_style = resolver_.ComputeFor(*root_element, initial);
        if (root_style.font_size > 0.0f) {
            root_font_size_ = root_style.font_size;
        }
    }
    // html 上设定了 font-size 时，该值即全文档的 rem 基准，需写回根样式再向下传递。
    root_style.root_font_size = root_font_size_;

    const Element* body = document.Body();
    if (body == nullptr) {
        auto empty_root = std::make_unique<LayoutBox>();
        empty_root->kind = BoxKind::Block;
        empty_root->style = initial;
        empty_root->border_box = RectF::FromLTRB(0.0f, 0.0f, 0.0f, 0.0f);
        empty_root->content_box = empty_root->border_box;
        result->root = std::move(empty_root);
        result->content_height = 0.0f;
        BuildPageStarts(&result);
        return result;
    }

    const ComputedStyle body_style = resolver_.ComputeFor(*body, root_style);
    if (body_style.display == Display::None) {
        auto empty_root = std::make_unique<LayoutBox>();
        empty_root->kind = BoxKind::Block;
        empty_root->style = body_style;
        empty_root->border_box = RectF::FromLTRB(0.0f, 0.0f, 0.0f, 0.0f);
        empty_root->content_box = empty_root->border_box;
        result->root = std::move(empty_root);
        result->content_height = 0.0f;
        BuildPageStarts(&result);
        return result;
    }

    const EdgeValues body_edges = ResolveEdges(body_style, options_.viewport_width, root_font_size_);
    const float border_left = body_edges.margin_left;
    // 根元素的上外边距同样与首个可见子块的上外边距合并。
    const float border_top =
        OuterMarginTop(body_style, body, options_.viewport_width, root_font_size_);

    std::unique_ptr<LayoutBox> root =
        LayoutBlock(body, body_style, border_left, border_top, options_.viewport_width,
                    root_font_size_, 0, -1.0f, ViewportHeight());
    const float root_margin_bottom =
        OuterMarginBottom(body_style, body, options_.viewport_width, root_font_size_);

    result->content_height =
        ClampNonNegative(root->border_box.bottom + root_margin_bottom);
    result->root = std::move(root);
    BuildPageStarts(&result);
    return result;
}

// ---------------------------------------------------------------------------
// 块级流构建
// ---------------------------------------------------------------------------

// 判断行内原子序列中是否存在会占据空间的可见内容。仅由可折叠空白组成的
// 匿名块不会生成行盒，应整体跳过。
bool HasVisibleAtoms(const std::vector<InlineAtom>& atoms) {
    for (const InlineAtom& atom : atoms) {
        if (atom.kind != InlineAtom::Kind::Text) {
            return true;
        }
        if (!atom.space || !atom.collapsible_space) {
            return true;
        }
    }
    return false;
}

std::string RomanMarker(int value, bool upper_case) {
    static const int kValues[] = {1000, 900, 500, 400, 100, 90, 50, 40, 10, 9, 5, 4, 1};
    static const char* kSymbols[] = {"M",  "CM", "D",  "CD", "C",  "XC", "L",
                                     "XL", "X",  "IX", "V",  "IV", "I"};
    int number = value > 0 ? value : 1;
    std::string result;
    for (size_t index = 0; index < sizeof(kValues) / sizeof(kValues[0]); ++index) {
        while (number >= kValues[index]) {
            result += kSymbols[index];
            number -= kValues[index];
        }
    }
    if (!upper_case) {
        for (char& character : result) {
            character = AsciiLowerChar(character);
        }
    }
    return result;
}

std::string AlphaMarker(int value, bool upper_case) {
    int number = value > 0 ? value : 1;
    std::string result;
    while (number > 0) {
        const int remainder = (number - 1) % 26;
        result.insert(result.begin(),
                      static_cast<char>((upper_case ? 'A' : 'a') + remainder));
        number = (number - 1) / 26;
    }
    return result;
}

std::string LayoutEngine::BuildMarker(const ComputedStyle& style, int list_ordinal) const {
    switch (style.list_style_type) {
        case ListStyleType::None:
            return std::string();
        case ListStyleType::Disc:
            return "\xE2\x80\xA2";  // "•"
        case ListStyleType::Circle:
            return "\xE2\x97\x8B";  // "◦"
        case ListStyleType::Square:
            return "\xE2\x96\xAA";  // "▪" U+25AA
        case ListStyleType::Decimal: {
            const int number = list_ordinal > 0 ? list_ordinal : 1;
            return std::to_string(number) + ".";
        }
        case ListStyleType::LowerRoman:
            return RomanMarker(list_ordinal, false) + ".";
        case ListStyleType::UpperRoman:
            return RomanMarker(list_ordinal, true) + ".";
        case ListStyleType::LowerAlpha:
            return AlphaMarker(list_ordinal, false) + ".";
        case ListStyleType::UpperAlpha:
            return AlphaMarker(list_ordinal, true) + ".";
    }
    return std::string();
}

// 把元素的子节点划分为块级流项与连续的纯行内片段；display:none 子树跳过。
// flex_items 为真时用于弹性盒容器：每个元素子节点独立成项（行内元素也被
// 块化），纯空白文本节点不产生匿名项目。
void LayoutEngine::BuildFlowItems(const Element& element, const ComputedStyle& parent_style,
                                  float containing_width, float root_font_size, bool flex_items,
                                  std::vector<FlowItem>* items) {
    FlowItem inline_item;
    inline_item.is_block = false;
    bool has_inline_nodes = false;
    int list_ordinal = 0;

    auto flush_inline = [&]() {
        if (!has_inline_nodes) {
            return;
        }
        items->push_back(inline_item);
        inline_item = FlowItem();
        inline_item.is_block = false;
        has_inline_nodes = false;
    };

    auto append_block_item = [&](const Element& child_element, const ComputedStyle& child_style) {
        flush_inline();
        FlowItem block_item;
        block_item.is_block = true;
        block_item.element = &child_element;
        block_item.style = child_style;
        const EdgeValues child_edges =
            ResolveEdges(child_style, containing_width, root_font_size);
        block_item.margin_left = child_edges.margin_left;
        block_item.margin_right = child_edges.margin_right;
        block_item.margin_top =
            OuterMarginTop(child_style, &child_element, containing_width, root_font_size);
        block_item.margin_bottom =
            OuterMarginBottom(child_style, &child_element, containing_width, root_font_size);
        if (child_style.display == Display::ListItem) {
            ++list_ordinal;
            block_item.list_ordinal = list_ordinal;
        }
        items->push_back(std::move(block_item));
    };

    // 未打开的 details 只显示第一个 summary，其余子内容隐藏（HTML5 默认行为）。
    const bool details_closed =
        element.TagName() == "details" && !element.HasAttribute("open");
    bool summary_taken = false;

    for (const NodePtr& child : element.Children()) {
        if (!child) {
            continue;
        }
        if (details_closed && child->IsElement()) {
            const Element& candidate = static_cast<const Element&>(*child);
            if (candidate.TagName() != "summary" || summary_taken) {
                continue;
            }
            summary_taken = true;
        } else if (details_closed && child->IsText()) {
            continue;
        }
        if (child->IsText()) {
            if (flex_items) {
                const std::string& data = static_cast<const TextNode&>(*child).Data();
                bool blank = true;
                for (size_t index = 0; index < data.size(); ++index) {
                    if (data[index] != ' ' && data[index] != '\t' && data[index] != '\n' &&
                        data[index] != '\r' && data[index] != '\f') {
                        blank = false;
                        break;
                    }
                }
                if (blank) {
                    continue;
                }
            }
            inline_item.inline_nodes.push_back(child.get());
            has_inline_nodes = true;
            continue;
        }
        if (!child->IsElement()) {
            continue;
        }
        const Element& child_element = static_cast<const Element&>(*child);
        const ComputedStyle child_style = resolver_.ComputeFor(child_element, parent_style);
        if (child_style.display == Display::None) {
            continue;
        }
        if (!child_style.IsBlockLevel()) {
            if (flex_items) {
                append_block_item(child_element, child_style);
                continue;
            }
            inline_item.inline_nodes.push_back(child.get());
            has_inline_nodes = true;
            continue;
        }
        append_block_item(child_element, child_style);
    }
    flush_inline();
}

// ---------------------------------------------------------------------------
// 弹性盒排版
// ---------------------------------------------------------------------------

namespace {

// 一个弹性项目的排版计划。基础尺寸在主轴上先于子盒布局确定，交叉轴尺寸
// 在子盒布局完成后回填。
struct FlexItemPlan {
    bool anonymous = false;
    const Element* element = nullptr;
    ComputedStyle style;
    std::vector<const Node*> inline_nodes;
    int list_ordinal = 0;

    float margin_left = 0.0f;
    float margin_right = 0.0f;
    float margin_top = 0.0f;
    float margin_bottom = 0.0f;
    // 主轴方向的 padding + border 总和（不含外边距）。
    float frame_main = 0.0f;
    // 交叉轴方向的 padding + border 总和；列方向拉伸时用于把边框盒宽度换算为内容宽度。
    float frame_cross = 0.0f;

    float base_main = 0.0f;   // grow/shrink 之前的主轴内容尺寸
    float main_size = 0.0f;   // 分配后的主轴内容尺寸
    float outer_main = 0.0f;  // 主轴外侧尺寸（含外边距）
    float outer_cross = 0.0f; // 交叉轴外侧尺寸
    // 首次排版使用的宽度，主轴或交叉轴尺寸确定后按同一宽度重新排版。
    float layout_width = 0.0f;
    // 首次排版得到的内容高度，用于判断是否需要按最终尺寸重新排版。
    float initial_content_height = -1.0f;

    std::unique_ptr<LayoutBox> box;
};

}  // namespace

float LayoutEngine::LayoutFlexChildren(const ComputedStyle& style,
                                       const std::vector<FlowItem>& items,
                                       float content_left, float content_top,
                                       float content_width, float root_font_size,
                                       LayoutBox* box, float container_height) {
    const bool row_axis = style.flex_direction == FlexDirection::Row ||
                          style.flex_direction == FlexDirection::RowReverse;
    const bool reverse_main = style.flex_direction == FlexDirection::RowReverse ||
                              style.flex_direction == FlexDirection::ColumnReverse;
    const bool wrap = row_axis && style.flex_wrap != FlexWrap::NoWrap;

    auto resolve = [&](const CssLength& length, float base) {
        return ResolveLength(length, style.font_size, root_font_size, base,
                             options_.viewport_width, ViewportHeight());
    };

    const float column_gap = ClampNonNegative(resolve(style.column_gap, content_width));
    const float row_gap = ClampNonNegative(resolve(style.row_gap, content_width));
    const float main_gap = row_axis ? column_gap : row_gap;

    // 容器高度由调用方给出（样式指定高度或祖先的确定高度），-1 表示由内容决定。
    const float container_main = row_axis ? content_width : container_height;

    if (items.empty()) {
        return container_height >= 0.0f ? container_height : 0.0f;
    }

    // 匿名弹性项目（容器内的行内内容）的固有宽度。
    auto intrinsic_inline_width = [&](const std::vector<const Node*>& nodes,
                                      const ComputedStyle& item_style) {
        LayoutBox scratch;
        std::vector<InlineAtom> atoms;
        BuildInlineRun(nodes, item_style, content_width, &scratch, &atoms);
        float total = 0.0f;
        for (const InlineAtom& atom : atoms) {
            if (atom.kind == InlineAtom::Kind::Text) {
                total += atom.width;
            } else if (atom.kind == InlineAtom::Kind::Atomic) {
                total += atom.atomic_outer_width;
            }
        }
        return total;
    };

    std::vector<FlexItemPlan> plans;
    plans.reserve(items.size());
    for (const FlowItem& item : items) {
        FlexItemPlan plan;
        plan.anonymous = !item.is_block;
        plan.element = item.element;
        plan.style = item.style;
        plan.inline_nodes = item.inline_nodes;
        plan.list_ordinal = item.list_ordinal;
        if (plan.anonymous) {
            // 行内内容包裹成匿名项目：沿用容器文字样式，不参与 grow。
            plan.style.display = Display::Block;
            plan.style.width = CssLength();
            plan.style.height = CssLength();
            plan.style.margin = EdgeLengths();
            plan.style.padding = EdgeLengths();
            plan.style.border_width = EdgeLengths();
            plan.style.background_color = Transparent();
            plan.style.flex_grow = 0.0f;
            plan.style.flex_shrink = 1.0f;
            plan.style.flex_basis = CssLength();
        }
        const EdgeValues edges = ResolveEdges(plan.style, content_width, root_font_size);
        plan.margin_left = edges.margin_left;
        plan.margin_right = edges.margin_right;
        plan.margin_top = edges.margin_top;
        plan.margin_bottom = edges.margin_bottom;
        plan.frame_main = row_axis ? edges.padding_left + edges.padding_right +
                                         edges.border_left + edges.border_right
                                   : edges.padding_top + edges.padding_bottom +
                                         edges.border_top + edges.border_bottom;
        plan.frame_cross = row_axis ? edges.padding_top + edges.padding_bottom +
                                          edges.border_top + edges.border_bottom
                                    : edges.padding_left + edges.padding_right +
                                          edges.border_left + edges.border_right;

        float base = 0.0f;
        if (!plan.style.flex_basis.IsAuto()) {
            const float basis = resolve(plan.style.flex_basis, content_width);
            base = plan.style.border_box ? ClampNonNegative(basis - plan.frame_main) : basis;
        } else if (row_axis && !plan.style.width.IsAuto()) {
            const float specified = resolve(plan.style.width, content_width);
            base = plan.style.border_box ? ClampNonNegative(specified - plan.frame_main)
                                         : specified;
        } else if (!row_axis && !plan.style.height.IsAuto()) {
            // 列方向的主轴基准是高度：容器高度确定时按百分比解析，否则沿用宽度基准。
            const float specified =
                resolve(plan.style.height,
                        container_height >= 0.0f ? container_height : content_width);
            base = plan.style.border_box ? ClampNonNegative(specified - plan.frame_main)
                                         : specified;
        } else if (row_axis) {
            base = plan.anonymous
                       ? intrinsic_inline_width(plan.inline_nodes, plan.style)
                       : MeasureTableCellIntrinsicWidth(*plan.element, plan.style, content_width);
        }
        plan.base_main = ClampNonNegative(base);
        plan.main_size = plan.base_main;
        plans.push_back(std::move(plan));
    }

    /* order：按数值稳定排序，未指定 order 的项目保持文档顺序。 */
    std::stable_sort(plans.begin(), plans.end(),
                     [](const FlexItemPlan& left, const FlexItemPlan& right) {
                         return left.style.order < right.style.order;
                     });

    // 主轴空间分配：按 grow 放大、按 shrink 收缩，随后按 justify-content 定位。
    auto resolve_main_sizes = [&](const std::vector<size_t>& line) {
        float total_outer = 0.0f;
        float total_grow = 0.0f;
        float total_shrink = 0.0f;
        for (size_t index : line) {
            FlexItemPlan& plan = plans[index];
            plan.main_size = plan.base_main;
            total_outer += plan.base_main + plan.frame_main + plan.margin_left + plan.margin_right;
            total_grow += plan.style.flex_grow;
            total_shrink += plan.style.flex_shrink * plan.base_main;
        }
        if (line.size() > 1) {
            total_outer += main_gap * static_cast<float>(line.size() - 1);
        }
        if (container_main >= 0.0f) {
            const float free = container_main - total_outer;
            if (free > 0.0f && total_grow > 0.0f) {
                for (size_t index : line) {
                    FlexItemPlan& plan = plans[index];
                    plan.main_size += free * plan.style.flex_grow / total_grow;
                }
            } else if (free < 0.0f && total_shrink > 0.0f) {
                const float overflow = -free;
                for (size_t index : line) {
                    FlexItemPlan& plan = plans[index];
                    const float weight = plan.style.flex_shrink * plan.base_main / total_shrink;
                    plan.main_size = ClampNonNegative(plan.base_main - overflow * weight);
                }
            }
        }
        for (size_t index : line) {
            FlexItemPlan& plan = plans[index];
            plan.outer_main = plan.main_size + plan.frame_main + plan.margin_left + plan.margin_right;
        }
    };

    // 计算本行在主轴上的起始偏移与项目间距。
    auto compute_justify = [&](const std::vector<size_t>& line, float* lead,
                               float* between) {
        float used = 0.0f;
        for (size_t index : line) {
            used += plans[index].outer_main;
        }
        if (line.size() > 1) {
            used += main_gap * static_cast<float>(line.size() - 1);
        }
        float free = container_main >= 0.0f ? container_main - used : 0.0f;
        if (free < 0.0f) {
            free = 0.0f;
        }
        *lead = 0.0f;
        *between = main_gap;
        switch (style.justify_content) {
        case JustifyContent::FlexEnd:
            *lead = free;
            break;
        case JustifyContent::Center:
            *lead = free * 0.5f;
            break;
        case JustifyContent::SpaceBetween:
            if (line.size() > 1) {
                *between += free / static_cast<float>(line.size() - 1);
            }
            break;
        case JustifyContent::SpaceAround:
            if (line.size() > 0) {
                *lead = free / static_cast<float>(line.size()) * 0.5f;
                *between += free / static_cast<float>(line.size());
            }
            break;
        case JustifyContent::SpaceEvenly:
            if (line.size() > 0) {
                *lead = free / static_cast<float>(line.size() + 1);
                *between += free / static_cast<float>(line.size() + 1);
            }
            break;
        case JustifyContent::FlexStart:
            break;
        }
    };

    auto item_align = [&](const FlexItemPlan& plan) {
        return plan.style.align_self == AlignItems::Auto ? style.align_items
                                                         : plan.style.align_self;
    };

    // 匿名项目没有对应元素，直接按行内流排版成盒。
    auto layout_anonymous = [&](FlexItemPlan& plan, float left, float top, float width) {
        auto anonymous = std::make_unique<LayoutBox>();
        anonymous->kind = BoxKind::Block;
        anonymous->style = plan.style;
        std::vector<InlineAtom> atoms;
        BuildInlineRun(plan.inline_nodes, style, width, anonymous.get(), &atoms);
        const float height = LayoutLines(atoms, width, style, left, top, anonymous.get());
        anonymous->border_box = RectF::FromLTRB(left, top, left + width, top + height);
        anonymous->content_box = anonymous->border_box;
        plan.box = std::move(anonymous);
    };

    // 替换元素的内容尺寸由属性或固有尺寸决定，不参与按弹性尺寸重新排版。
    auto is_replaced = [](const FlexItemPlan& plan) {
        if (plan.element == nullptr) {
            return false;
        }
        const std::string& tag = plan.element->TagName();
        return tag == "img" || tag == "canvas" || tag == "iframe" || tag == "table";
    };

    // 主轴或交叉轴尺寸确定后按最终高度重新排版项目，使子元素获得确定高度，
    // 百分比高度、绝对定位与背景填充才能按最终尺寸生效。
    auto relayout_item = [&](FlexItemPlan& plan, float content_height) {
        plan.box = LayoutBlock(plan.element, plan.style, plan.box->border_box.left,
                               plan.box->border_box.top, content_width, root_font_size,
                               plan.list_ordinal, plan.layout_width, container_height,
                               ClampNonNegative(content_height));
    };

    auto commit_children = [&]() {
        for (FlexItemPlan& plan : plans) {
            if (plan.box) {
                box->children.push_back(std::move(plan.box));
            }
        }
    };

    if (row_axis) {
        // 分行：按基础主轴尺寸贪心放置，仅 wrap 时产生多行。
        std::vector<std::vector<size_t>> lines;
        if (!wrap) {
            std::vector<size_t> single;
            for (size_t index = 0; index < plans.size(); ++index) {
                single.push_back(index);
            }
            lines.push_back(std::move(single));
        } else {
            std::vector<size_t> current;
            float used = 0.0f;
            for (size_t index = 0; index < plans.size(); ++index) {
                const FlexItemPlan& plan = plans[index];
                const float outer =
                    plan.base_main + plan.frame_main + plan.margin_left + plan.margin_right;
                if (!current.empty() && used + main_gap + outer > content_width) {
                    lines.push_back(current);
                    current.clear();
                    used = 0.0f;
                }
                used += (current.empty() ? 0.0f : main_gap) + outer;
                current.push_back(index);
            }
            if (!current.empty()) {
                lines.push_back(current);
            }
        }

        // wrap-reverse：交叉轴起点在底部，行顺序整体反转。
        if (style.flex_wrap == FlexWrap::WrapReverse) {
            std::reverse(lines.begin(), lines.end());
        }

        float cross_cursor = 0.0f;
        std::vector<float> line_tops;
        std::vector<float> line_heights;
        for (size_t line_index = 0; line_index < lines.size(); ++line_index) {
            const std::vector<size_t> line = lines[line_index];
            resolve_main_sizes(line);
            float lead = 0.0f;
            float between = main_gap;
            compute_justify(line, &lead, &between);

            std::vector<size_t> order = line;
            if (reverse_main) {
                std::reverse(order.begin(), order.end());
            }

            float position = lead;
            float line_cross = 0.0f;
            for (size_t index : order) {
                FlexItemPlan& plan = plans[index];
                const float child_left = content_left + position + plan.margin_left;
                const float child_top = content_top + cross_cursor + plan.margin_top;
                if (plan.anonymous) {
                    layout_anonymous(plan, child_left, child_top, plan.main_size);
                } else {
                    plan.layout_width = plan.main_size;
                    plan.box = LayoutBlock(plan.element, plan.style, child_left, child_top,
                                           content_width, root_font_size, plan.list_ordinal,
                                           plan.main_size, container_height);
                }
                plan.outer_cross =
                    plan.box->border_box.Height() + plan.margin_top + plan.margin_bottom;
                line_cross = std::max(line_cross, plan.outer_cross);
                position += plan.outer_main + between;
            }
            // 单行且容器高度确定时，行交叉轴尺寸取容器高度，便于 stretch。
            if (lines.size() == 1 && container_height >= 0.0f) {
                line_cross = std::max(line_cross, container_height);
            }

            // baseline：以本行内基线对齐项目的第一行基线为准，取最大偏移。
            float max_baseline = 0.0f;
            bool has_baseline = false;
            for (size_t index : order) {
                const FlexItemPlan& plan = plans[index];
                if (item_align(plan) != AlignItems::Baseline || !plan.box ||
                    plan.box->lines.empty()) {
                    continue;
                }
                const float baseline_offset =
                    plan.box->lines.front().baseline - plan.box->border_box.top;
                max_baseline = std::max(max_baseline, baseline_offset);
                has_baseline = true;
            }

            for (size_t index : order) {
                FlexItemPlan& plan = plans[index];
                const float free = line_cross - plan.outer_cross;
                float dy = 0.0f;
                const AlignItems align = item_align(plan);
                if (align == AlignItems::Center) {
                    dy = free * 0.5f;
                } else if (align == AlignItems::FlexEnd) {
                    dy = free;
                } else if (align == AlignItems::Baseline && has_baseline && !plan.box->lines.empty()) {
                    const float baseline_offset =
                        plan.box->lines.front().baseline - plan.box->border_box.top;
                    dy = max_baseline - baseline_offset;
                } else if (align == AlignItems::Stretch && plan.style.height.IsAuto() &&
                           free > 0.0f) {
                    // 高度自适应时按行交叉轴尺寸重新排版，让子元素的百分比高度与
                    // 背景填充按拉伸后的高度生效。
                    if (!plan.anonymous && !is_replaced(plan)) {
                        relayout_item(plan, plan.box->content_box.Height() + free);
                    } else {
                        plan.box->border_box.bottom += free;
                        plan.box->content_box.bottom += free;
                    }
                    plan.outer_cross += free;
                }
                if (dy != 0.0f) {
                    TranslateBox(plan.box.get(), 0.0f, dy);
                }
            }

            line_tops.push_back(cross_cursor);
            line_heights.push_back(line_cross);
            cross_cursor += line_cross;
            if (line_index + 1 < lines.size()) {
                cross_cursor += row_gap;
            }
        }

        /* align-content：容器高度确定且存在多行时，把各行整体按剩余空间分布。
           单行容器由 align-items 决定，不受 align-content 影响。 */
        if (container_height >= 0.0f && lines.size() > 1) {
            float total_cross = 0.0f;
            for (float height : line_heights) {
                total_cross += height;
            }
            total_cross += row_gap * static_cast<float>(lines.size() - 1);
            float free = container_height - total_cross;
            if (free < 0.0f) {
                free = 0.0f;
            }
            float lead = 0.0f;
            float between = row_gap;
            const float count = static_cast<float>(lines.size());
            switch (style.align_content) {
            case AlignContent::FlexEnd:
                lead = free;
                break;
            case AlignContent::Center:
                lead = free * 0.5f;
                break;
            case AlignContent::SpaceBetween:
                between += free / (count - 1.0f);
                break;
            case AlignContent::SpaceAround:
                lead = free / count * 0.5f;
                between += free / count;
                break;
            case AlignContent::SpaceEvenly:
                lead = free / (count + 1.0f);
                between += free / (count + 1.0f);
                break;
            case AlignContent::Stretch:
                between += free / count;
                break;
            case AlignContent::FlexStart:
                break;
            }
            float target = lead;
            for (size_t line_index = 0; line_index < lines.size(); ++line_index) {
                const float delta = target - line_tops[line_index];
                if (delta != 0.0f) {
                    for (size_t index : lines[line_index]) {
                        if (plans[index].box) {
                            TranslateBox(plans[index].box.get(), 0.0f, delta);
                        }
                    }
                }
                target += line_heights[line_index] + between;
            }
            cross_cursor = container_height;
        }
        if (container_height >= 0.0f) {
            commit_children();
            return container_height;
        }
        commit_children();
        return ClampNonNegative(cross_cursor);
    }

    // 列方向：主轴为竖直方向，先按顺序排版测量高度，再分配剩余空间与对齐。
    std::vector<size_t> all;
    all.reserve(plans.size());
    for (size_t index = 0; index < plans.size(); ++index) {
        all.push_back(index);
    }
    // column-reverse：主轴起点在底部，按逆序从起点方向堆叠。
    if (reverse_main) {
        std::reverse(all.begin(), all.end());
    }

    float position = 0.0f;
    float total_main = 0.0f;
    for (size_t index : all) {
        FlexItemPlan& plan = plans[index];
        const float available =
            ClampNonNegative(content_width - plan.margin_left - plan.margin_right);
        // 交叉轴宽度：指定宽度交给 LayoutBlock 按 box-sizing 解析；auto 时拉伸到
        // 可用宽度（换算为内容宽度），非 stretch 时按内容宽度收缩适应。
        float content_override = -1.0f;
        if (plan.style.width.IsAuto()) {
            if (item_align(plan) == AlignItems::Stretch) {
                content_override = ClampNonNegative(available - plan.frame_cross);
            } else {
                const float intrinsic =
                    plan.anonymous
                        ? intrinsic_inline_width(plan.inline_nodes, plan.style)
                        : MeasureTableCellIntrinsicWidth(*plan.element, plan.style, available);
                content_override = std::min(available, ClampNonNegative(intrinsic));
            }
        }
        if (plan.anonymous) {
            const float anonymous_width =
                content_override >= 0.0f ? content_override : available;
            layout_anonymous(plan, content_left + plan.margin_left,
                             content_top + position + plan.margin_top, anonymous_width);
        } else {
            plan.layout_width = content_override;
            plan.box = LayoutBlock(plan.element, plan.style, content_left + plan.margin_left,
                                   content_top + position + plan.margin_top, available,
                                   root_font_size, plan.list_ordinal, content_override,
                                   container_height);
        }
        plan.initial_content_height = plan.box->content_box.Height();
        const float border_height = plan.box->border_box.Height();
        plan.base_main = border_height;
        plan.main_size = border_height;
        plan.outer_main = border_height + plan.margin_top + plan.margin_bottom;
        plan.outer_cross = plan.box->border_box.Width() + plan.margin_left + plan.margin_right;
        position += plan.outer_main + main_gap;
        total_main += plan.outer_main + main_gap;
    }
    if (!all.empty()) {
        total_main -= main_gap;
    }

    // 容器高度确定时按 grow 放大、按 shrink 收缩主轴尺寸（通过拉伸盒高度实现）。
    if (container_height >= 0.0f) {
        float total_grow = 0.0f;
        float total_shrink = 0.0f;
        for (size_t index : all) {
            total_grow += plans[index].style.flex_grow;
            total_shrink += plans[index].style.flex_shrink * plans[index].base_main;
        }
        const float free = container_height - total_main;
        if (free > 0.0f && total_grow > 0.0f) {
            for (size_t index : all) {
                FlexItemPlan& plan = plans[index];
                const float extra = free * plan.style.flex_grow / total_grow;
                plan.box->border_box.bottom += extra;
                plan.box->content_box.bottom += extra;
                plan.outer_main += extra;
            }
            total_main = container_height;
        } else if (free < 0.0f && total_shrink > 0.0f) {
            const float overflow = -free;
            for (size_t index : all) {
                FlexItemPlan& plan = plans[index];
                const float weight = plan.style.flex_shrink * plan.base_main / total_shrink;
                const float target =
                    ClampNonNegative(plan.base_main - overflow * weight);
                const float delta = plan.base_main - target;
                plan.box->border_box.bottom -= delta;
                plan.box->content_box.bottom -= delta;
                plan.outer_main -= delta;
            }
            total_main = container_height;
        }

        // 主轴尺寸确定后按最终内容高度重新排版各项目：子元素由此获得确定高度，
        // 百分比高度、绝对定位包含块与背景填充都与最终尺寸一致。
        for (size_t index : all) {
            FlexItemPlan& plan = plans[index];
            if (plan.anonymous || plan.box == nullptr || is_replaced(plan)) {
                continue;
            }
            const float target_content = plan.box->content_box.Height();
            if (std::fabs(target_content - plan.initial_content_height) < 0.01f) {
                continue;
            }
            relayout_item(plan, target_content);
        }
    }

    float lead = 0.0f;
    float between = main_gap;
    compute_justify(all, &lead, &between);

    std::vector<size_t> order = all;
    if (reverse_main) {
        std::reverse(order.begin(), order.end());
    }
    float cursor = lead;
    for (size_t index : order) {
        FlexItemPlan& plan = plans[index];
        const float target_top = content_top + cursor + plan.margin_top;
        const float dy = target_top - plan.box->border_box.top;
        if (dy != 0.0f) {
            TranslateBox(plan.box.get(), 0.0f, dy);
        }
        const float free_cross = content_width - plan.outer_cross;
        const AlignItems align = item_align(plan);
        float dx = 0.0f;
        if (align == AlignItems::Center) {
            dx = free_cross * 0.5f;
        } else if (align == AlignItems::FlexEnd) {
            dx = free_cross;
        }
        if (dx != 0.0f) {
            TranslateBox(plan.box.get(), dx, 0.0f);
        }
        cursor += plan.outer_main + between;
    }

    const float used_height = lead + total_main;
    commit_children();
    if (container_height >= 0.0f) {
        return container_height;
    }
    return ClampNonNegative(used_height);
}

// ---------------------------------------------------------------------------
// 定位盒与网格排版
// ---------------------------------------------------------------------------

void LayoutEngine::LayoutPositioned(const FlowItem& item, const RectF& containing_block,
                                    float root_font_size, LayoutBox* parent) {
    if (item.element == nullptr) {
        return;
    }
    const ComputedStyle& style = item.style;
    const float block_width = containing_block.Width();
    const float block_height = containing_block.Height();
    const EdgeValues edges = ResolveEdges(style, block_width, root_font_size);

    auto resolve_offset = [&](const CssLength& length, float base) -> float {
        if (length.IsAuto()) {
            return -1.0f;
        }
        return ResolveLength(length, style.font_size, root_font_size, base,
                             options_.viewport_width, ViewportHeight());
    };
    const float left = resolve_offset(style.position_left, block_width);
    const float right = resolve_offset(style.position_right, block_width);
    const float top = resolve_offset(style.position_top, block_height);
    const float bottom = resolve_offset(style.position_bottom, block_height);

    const float frame_horizontal = edges.padding_left + edges.padding_right +
                                  edges.border_left + edges.border_right;
    const float frame_vertical = edges.padding_top + edges.padding_bottom +
                                 edges.border_top + edges.border_bottom;

    float content_width = -1.0f;
    if (!style.width.IsAuto()) {
        content_width = ClampNonNegative(ResolveLength(style.width, style.font_size,
                                                      root_font_size, block_width,
                                                      options_.viewport_width, ViewportHeight()));
    } else if (left >= 0.0f && right >= 0.0f) {
        content_width = ClampNonNegative(block_width - left - right -
                                        edges.margin_left - edges.margin_right -
                                        frame_horizontal);
    } else {
        const float intrinsic =
            MeasureTableCellIntrinsicWidth(*item.element, style, block_width);
        const float limit = ClampNonNegative(block_width - edges.margin_left -
                                             edges.margin_right - frame_horizontal);
        content_width = ClampNonNegative(std::min(intrinsic, limit));
    }

    float x = containing_block.left + edges.margin_left;
    if (left >= 0.0f) {
        x = containing_block.left + left + edges.margin_left;
    } else if (right >= 0.0f) {
        x = containing_block.right - right - edges.margin_right -
            (content_width + frame_horizontal);
    }

    float y = containing_block.top + edges.margin_top;
    if (top >= 0.0f) {
        y = containing_block.top + top + edges.margin_top;
    }

    /* dialog 未指定水平偏移时按视口/包含块居中（浏览器默认行为）。 */
    const bool is_dialog = item.element != nullptr && item.element->TagName() == "dialog";
    if (is_dialog && left < 0.0f && right < 0.0f) {
        x = containing_block.left +
            ClampNonNegative((block_width - content_width - frame_horizontal) * 0.5f) +
            edges.margin_left;
    }

    std::unique_ptr<LayoutBox> child =
        LayoutBlock(item.element, style, x, y, block_width, root_font_size,
                    item.list_ordinal, content_width, block_height);

    /* dialog 未指定垂直偏移时在包含块内垂直居中。 */
    if (is_dialog && top < 0.0f && bottom < 0.0f) {
        const float target_top =
            containing_block.top +
            ClampNonNegative((block_height - child->border_box.Height()) * 0.5f) +
            edges.margin_top;
        const float dy = target_top - child->border_box.top;
        if (dy != 0.0f) {
            TranslateBox(child.get(), 0.0f, dy);
        }
    }

    // 同时给出 top 与 bottom 且高度自适应时，拉伸到两侧偏移之间的高度。
    if (top >= 0.0f && bottom >= 0.0f && style.height.IsAuto()) {
        const float target_bottom = containing_block.bottom - bottom - edges.margin_bottom;
        child->border_box.bottom = target_bottom;
        child->content_box.bottom = target_bottom - edges.padding_bottom - edges.border_bottom;
        child->content_box.top = child->border_box.top + edges.border_top + edges.padding_top;
    } else if (top < 0.0f && bottom >= 0.0f) {
        const float target_bottom = containing_block.bottom - bottom - edges.margin_bottom;
        const float dy = target_bottom - child->border_box.bottom;
        if (dy != 0.0f) {
            TranslateBox(child.get(), 0.0f, dy);
        }
    }
    (void)frame_vertical;
    parent->children.push_back(std::move(child));
}

float LayoutEngine::LayoutGridChildren(const ComputedStyle& style,
                                       const std::vector<FlowItem>& items,
                                       float content_left, float content_top,
                                       float content_width, float root_font_size,
                                       LayoutBox* box) {
    auto resolve = [&](const CssLength& length, float base) {
        return ResolveLength(length, style.font_size, root_font_size, base,
                             options_.viewport_width, ViewportHeight());
    };
    const float column_gap = ClampNonNegative(resolve(style.column_gap, content_width));
    const float row_gap = ClampNonNegative(resolve(style.row_gap, content_width));

    float specified_height = -1.0f;
    if (!style.height.IsAuto()) {
        specified_height = ClampNonNegative(resolve(style.height, content_width));
    }

    // 列轨道：未指定时退化为单列。
    std::vector<GridTrack> columns = style.grid_columns;
    if (columns.empty()) {
        GridTrack fallback;
        fallback.fraction = 1.0f;
        fallback.length = CssLength{0.0f, LengthUnit::Px};
        columns.push_back(fallback);
    }

    /* repeat(auto-fill/auto-fit, ...)：按容器宽度与轨道最小尺寸计算可以重复
       的次数。fr 轨道没有最小尺寸信息，按 1 次处理以免无限重复。 */
    {
        bool has_auto_repeat = false;
        for (const GridTrack& track : columns) {
            if (track.auto_repeat) {
                has_auto_repeat = true;
                break;
            }
        }
        if (has_auto_repeat) {
            std::vector<GridTrack> expanded_columns;
            for (const GridTrack& track : columns) {
                if (!track.auto_repeat) {
                    expanded_columns.push_back(track);
                    continue;
                }
                float minimum = 0.0f;
                if (track.fraction <= 0.0f && !track.length.IsAuto()) {
                    minimum = ClampNonNegative(resolve(track.length, content_width));
                }
                if (minimum <= 0.0f && !track.minimum.IsAuto() && track.minimum.unit !=
                                                                      LengthUnit::Auto) {
                    minimum = ClampNonNegative(resolve(track.minimum, content_width));
                }
                int repeat_count = 1;
                if (minimum > 0.0f) {
                    const float pitch = minimum + column_gap;
                    repeat_count = static_cast<int>(
                        (content_width + column_gap) / (pitch > 0.0f ? pitch : 1.0f));
                }
                if (repeat_count < 1) {
                    repeat_count = 1;
                }
                if (repeat_count > 64) {
                    repeat_count = 64;
                }
                for (int copy = 0; copy < repeat_count; ++copy) {
                    GridTrack item = track;
                    item.auto_repeat = false;
                    item.auto_fit = false;
                    expanded_columns.push_back(item);
                }
            }
            columns.swap(expanded_columns);
        }
    }
    const int column_count = static_cast<int>(columns.size());

    // 固定轨道先占位，fr 轨道再按剩余空间等分。
    std::vector<float> column_widths(columns.size(), 0.0f);
    float fixed_total = 0.0f;
    float fraction_total = 0.0f;
    for (size_t index = 0; index < columns.size(); ++index) {
        const GridTrack& track = columns[index];
        if (track.fraction > 0.0f) {
            fraction_total += track.fraction;
            continue;
        }
        const float width = track.length.IsAuto()
                                ? 0.0f
                                : ClampNonNegative(resolve(track.length, content_width));
        column_widths[index] = width;
        fixed_total += width;
    }
    const float gaps_total =
        column_count > 1 ? column_gap * static_cast<float>(column_count - 1) : 0.0f;
    const float remaining =
        ClampNonNegative(content_width - fixed_total - gaps_total);
    if (fraction_total > 0.0f) {
        for (size_t index = 0; index < columns.size(); ++index) {
            if (columns[index].fraction > 0.0f) {
                column_widths[index] = remaining * columns[index].fraction / fraction_total;
            }
        }
    }

    // 网格单元：显式起止线、grid-template-areas 与自动流都归一到行列范围。
    struct GridCell {
        const FlowItem* item = nullptr;
        std::unique_ptr<LayoutBox> layout;
        int column = 0;
        int span = 1;
        int row = 0;
        int row_span = 1;
    };
    std::vector<GridCell> cells;

    // 显式网格线编号：正数从起点计数，负数从终点计数；结果限制在有效范围。
    const auto resolve_column_line = [&](int line) {
        if (line > 0) {
            return std::min(line - 1, column_count - 1);
        }
        return std::max(column_count + line, 0);
    };

    // 占用表：记录已被跨行/跨列单元占据的格。
    std::vector<std::vector<unsigned char>> occupied;
    const auto ensure_rows = [&](int rows) {
        while (static_cast<int>(occupied.size()) < rows) {
            occupied.push_back(std::vector<unsigned char>(static_cast<size_t>(column_count), 0));
        }
    };
    const auto cell_is_free = [&](int row, int column, int span, int row_span) {
        if (column + span > column_count) {
            return false;
        }
        ensure_rows(row + row_span);
        for (int row_offset = 0; row_offset < row_span; ++row_offset) {
            for (int column_offset = 0; column_offset < span; ++column_offset) {
                if (occupied[static_cast<size_t>(row + row_offset)]
                            [static_cast<size_t>(column + column_offset)] != 0) {
                    return false;
                }
            }
        }
        return true;
    };
    const auto mark_cell = [&](int row, int column, int span, int row_span) {
        ensure_rows(row + row_span);
        for (int row_offset = 0; row_offset < row_span; ++row_offset) {
            for (int column_offset = 0; column_offset < span; ++column_offset) {
                occupied[static_cast<size_t>(row + row_offset)]
                        [static_cast<size_t>(column + column_offset)] = 1;
            }
        }
    };

    // 区域名到矩形范围的查找；未定义时返回 false。
    const auto find_area = [&](const std::string& name, int* column, int* row, int* span,
                               int* row_span) {
        if (name.empty() || style.grid_template_areas.empty()) {
            return false;
        }
        const std::string lowered_name = AsciiLower(name);
        int minimum_column = -1;
        int maximum_column = -1;
        int minimum_row = -1;
        int maximum_row = -1;
        for (size_t row_index = 0; row_index < style.grid_template_areas.size(); ++row_index) {
            const std::vector<std::string>& area_row = style.grid_template_areas[row_index];
            for (size_t column_index = 0; column_index < area_row.size(); ++column_index) {
                if (AsciiLower(area_row[column_index]) != lowered_name) {
                    continue;
                }
                const int column_value = static_cast<int>(column_index);
                const int row_value = static_cast<int>(row_index);
                if (minimum_column < 0 || column_value < minimum_column) {
                    minimum_column = column_value;
                }
                if (maximum_column < column_value) {
                    maximum_column = column_value;
                }
                if (minimum_row < 0 || row_value < minimum_row) {
                    minimum_row = row_value;
                }
                if (maximum_row < row_value) {
                    maximum_row = row_value;
                }
            }
        }
        if (minimum_column < 0) {
            return false;
        }
        *column = minimum_column;
        *row = minimum_row;
        *span = maximum_column - minimum_column + 1;
        *row_span = maximum_row - minimum_row + 1;
        return true;
    };

    int auto_column = 0;
    int auto_row = 0;
    for (const FlowItem& item : items) {
        const ComputedStyle& child_style = item.style;
        int span = std::max(1, std::min(child_style.grid_column_span, column_count));
        int row_span = std::max(1, child_style.grid_row_span);
        int column = -1;
        int row = -1;

        if (find_area(child_style.grid_area_name, &column, &row, &span, &row_span)) {
            // 区域已给出完整范围。
        } else {
            if (child_style.grid_column_start != 0) {
                const int start = resolve_column_line(child_style.grid_column_start);
                if (child_style.grid_column_end != 0) {
                    const int end = resolve_column_line(child_style.grid_column_end);
                    const int lower = std::min(start, end);
                    const int upper = std::max(start, end);
                    column = lower;
                    span = std::max(1, upper - lower);
                } else {
                    column = start;
                }
                span = std::min(span, column_count - column);
            }
            if (child_style.grid_row_start > 0) {
                const int start = child_style.grid_row_start - 1;
                if (child_style.grid_row_end > 0) {
                    row = std::min(start, child_style.grid_row_end - 1);
                    row_span = std::max(1, std::abs(child_style.grid_row_end -
                                                   child_style.grid_row_start));
                } else {
                    row = start;
                }
            }
        }

        // 自动流：在未被占用的位置寻找能容纳跨度的格子。
        if (column < 0 && row < 0) {
            while (true) {
                if (auto_column + span > column_count) {
                    auto_column = 0;
                    auto_row += 1;
                }
                if (cell_is_free(auto_row, auto_column, span, row_span)) {
                    column = auto_column;
                    row = auto_row;
                    auto_column += span;
                    if (auto_column >= column_count) {
                        auto_column = 0;
                        auto_row += 1;
                    }
                    break;
                }
                auto_column += 1;
            }
        } else if (column < 0) {
            column = 0;
            while (!cell_is_free(row, column, span, row_span)) {
                column += 1;
                if (column + span > column_count) {
                    row += 1;
                    column = 0;
                }
            }
        } else if (row < 0) {
            row = auto_row;
            while (!cell_is_free(row, column, span, row_span)) {
                row += 1;
            }
        }
        mark_cell(row, column, span, row_span);

        GridCell cell;
        cell.item = &item;
        cell.column = column;
        cell.span = span;
        cell.row = row;
        cell.row_span = row_span;

        float spanned_width = gaps_total > 0.0f ? column_gap * static_cast<float>(span - 1) : 0.0f;
        for (int offset = 0; offset < span; ++offset) {
            spanned_width += column_widths[static_cast<size_t>(column + offset)];
        }
        float offset_x = 0.0f;
        for (int column_index = 0; column_index < column; ++column_index) {
            offset_x += column_widths[static_cast<size_t>(column_index)] + column_gap;
        }
        const EdgeValues child_edges =
            ResolveEdges(child_style, spanned_width, root_font_size);
        cell.layout = LayoutBlock(item.element, child_style,
                                  content_left + offset_x + child_edges.margin_left,
                                  content_top, spanned_width, root_font_size, 0, -1.0f);
        cells.push_back(std::move(cell));
    }

    // 行高：每行取该行内单元格高度最大值（含外边距）；跨行单元格不足时
    // 把差额计入其最后一行，保证内容不被裁剪。
    int total_rows = 1;
    for (const GridCell& cell : cells) {
        total_rows = std::max(total_rows, cell.row + cell.row_span);
    }
    total_rows = std::max(total_rows, static_cast<int>(style.grid_rows.size()));
    std::vector<float> row_heights(static_cast<size_t>(total_rows), 0.0f);
    // 显式 grid-template-rows：固定轨道高度作为该行的最小高度。
    for (size_t row = 0; row < style.grid_rows.size(); ++row) {
        const GridTrack& track = style.grid_rows[row];
        if (track.fraction > 0.0f || track.length.IsAuto()) {
            continue;
        }
        const float height = ClampNonNegative(resolve(track.length, content_width));
        row_heights[row] = std::max(row_heights[row], height);
    }
    for (const GridCell& cell : cells) {
        const float height = cell.layout->border_box.Height() + cell.item->margin_top +
                             cell.item->margin_bottom;
        if (cell.row_span == 1) {
            row_heights[static_cast<size_t>(cell.row)] =
                std::max(row_heights[static_cast<size_t>(cell.row)], height);
        }
    }
    for (const GridCell& cell : cells) {
        if (cell.row_span <= 1) {
            continue;
        }
        float available = row_gap * static_cast<float>(cell.row_span - 1);
        for (int offset = 0; offset < cell.row_span; ++offset) {
            available += row_heights[static_cast<size_t>(cell.row + offset)];
        }
        const float height = cell.layout->border_box.Height() + cell.item->margin_top +
                             cell.item->margin_bottom;
        if (height > available) {
            row_heights[static_cast<size_t>(cell.row + cell.row_span - 1)] += height - available;
        }
    }
    float cursor_y = content_top;
    std::vector<float> row_offsets(row_heights.size(), 0.0f);
    for (size_t row = 0; row < row_heights.size(); ++row) {
        row_offsets[row] = cursor_y;
        cursor_y += row_heights[row] + (row + 1 < row_heights.size() ? row_gap : 0.0f);
    }
    const float grid_height = cursor_y - content_top;

    /* 容器高度确定且行总高有余量时按 align-content 分布行。 */
    if (specified_height >= 0.0f && grid_height < specified_height &&
        row_heights.size() > 1) {
        const float extra = specified_height - grid_height;
        float lead = 0.0f;
        float between = 0.0f;
        const float count = static_cast<float>(row_heights.size());
        switch (style.align_content) {
        case AlignContent::FlexEnd:
            lead = extra;
            break;
        case AlignContent::Center:
            lead = extra * 0.5f;
            break;
        case AlignContent::SpaceBetween:
            between = extra / (count - 1.0f);
            break;
        case AlignContent::SpaceAround:
            lead = extra / count * 0.5f;
            between = extra / count;
            break;
        case AlignContent::SpaceEvenly:
            lead = extra / (count + 1.0f);
            between = extra / (count + 1.0f);
            break;
        case AlignContent::Stretch:
            between = extra / (count - 1.0f);
            break;
        case AlignContent::FlexStart:
            break;
        }
        float position = content_top + lead;
        for (size_t row = 0; row < row_heights.size(); ++row) {
            row_offsets[row] = position;
            position += row_heights[row];
            if (row + 1 < row_heights.size()) {
                position += row_gap + between;
            }
        }
    }

    for (GridCell& cell : cells) {
        const float target_top = row_offsets[static_cast<size_t>(cell.row)] +
                                 cell.item->margin_top;
        const float target_bottom = row_offsets[static_cast<size_t>(cell.row)] +
                                    row_gap * static_cast<float>(cell.row_span - 1);
        float area_bottom = target_bottom;
        for (int offset = 0; offset < cell.row_span; ++offset) {
            area_bottom += row_heights[static_cast<size_t>(cell.row + offset)];
        }
        const float dy = target_top - cell.layout->border_box.top;
        if (dy != 0.0f) {
            TranslateBox(cell.layout.get(), 0.0f, dy);
        }
        // 默认 stretch：高度自适应且单元格低于区域高度时向下拉伸。
        if (cell.item->style.height.IsAuto() &&
            cell.layout->border_box.bottom < area_bottom - cell.item->margin_bottom - 0.5f) {
            const float extra =
                area_bottom - cell.item->margin_bottom - cell.layout->border_box.bottom;
            cell.layout->border_box.bottom += extra;
            cell.layout->content_box.bottom += extra;
        }
        box->children.push_back(std::move(cell.layout));
    }

    if (specified_height >= 0.0f) {
        return specified_height;
    }
    return ClampNonNegative(grid_height);
}

// 块级布局：自上而下确定盒模型、子流位置与高度。width_override 为负表示按
// 样式宽度或包含块宽度计算；非负用于行内块的收缩适应宽度。
// containing_height 为包含块的确定高度，用于解析百分比高度；height_override
// 为调用方指定的内容高度（弹性项目按最终尺寸重新排版时使用）。
std::unique_ptr<LayoutBox> LayoutEngine::LayoutBlock(const Element* element,
                                                     const ComputedStyle& style,
                                                     float border_left, float border_top,
                                                     float containing_width,
                                                     float root_font_size, int list_ordinal,
                                                     float width_override,
                                                     float containing_height,
                                                     float height_override) {
    const EdgeValues edges = ResolveEdges(style, containing_width, root_font_size);
    const float box_frame_horizontal = edges.padding_left + edges.padding_right +
                                       edges.border_left + edges.border_right;
    const float box_frame_vertical = edges.padding_top + edges.padding_bottom +
                                     edges.border_top + edges.border_bottom;
    // 百分比高度按包含块确定高度解析；无确定高度时沿用宽度基准（与 height 一致）。
    const float height_base = containing_height >= 0.0f ? containing_height : containing_width;
    const auto resolve_limit = [&](const CssLength& length, float base) -> float {
        if (length.IsAuto()) {
            return -1.0f;
        }
        return ClampNonNegative(ResolveLength(length, style.font_size, root_font_size, base,
                                              options_.viewport_width, ViewportHeight()));
    };
    // border-box 下指定的宽高包含内边距与边框，换算为内容尺寸。
    const auto to_content_size = [&](float specified, float frame) {
        return style.border_box ? ClampNonNegative(specified - frame) : specified;
    };
    const bool is_canvas = element != nullptr && element->TagName() == "canvas";
    // iframe 与 img、canvas 同属替换元素：盒尺寸由 CSS 或属性给出，内部内容
    // 由嵌套文档的布局结果在绘制阶段填入。SVG 同样是替换元素：其子树不进入
    // HTML 排版，尺寸由 CSS 或 width/height 属性给出，内容由 SVG 绘制层完成。
    const bool is_iframe = element != nullptr && element->TagName() == "iframe";
    const bool is_svg = element != nullptr && element->TagName() == "svg";
    const bool is_image =
        element != nullptr &&
        (element->TagName() == "img" || is_canvas || is_iframe || is_svg);

    // 表格使用独立排版路径：行与单元格由标签结构驱动，不进入通用块级流。
    if (!is_image && element != nullptr && element->TagName() == "table") {
        return LayoutTable(element, style, border_left, border_top, containing_width,
                           root_font_size, width_override);
    }

    // SVG 子树的计算样式不参与 HTML 排版，由 RefreshSvgStyles 在布局后增量
    // 刷新，避免每次拖动或选择都重新匹配全部选择器。
    float content_width = 0.0f;
    if (width_override >= 0.0f) {
        content_width = width_override;
    } else if (!style.width.IsAuto()) {
        const float specified = ResolveLength(style.width, style.font_size, root_font_size,
                                              containing_width, options_.viewport_width,
                                              ViewportHeight());
        content_width = to_content_size(specified, box_frame_horizontal);
    } else if (is_image) {
        const float attribute_width =
            ParseHtmlDimension(element->GetAttribute("width"), containing_width);
        content_width = attribute_width >= 0.0f ? attribute_width : 300.0f;
    } else {
        content_width = containing_width - edges.margin_left - edges.margin_right -
                        edges.border_left - edges.border_right - edges.padding_left -
                        edges.padding_right;
    }
    content_width = ClampNonNegative(content_width);

    // 最小/最大宽度：百分比相对包含块宽度解析，border-box 时换算为内容尺寸。
    const float min_width_value = resolve_limit(style.min_width, containing_width);
    if (min_width_value >= 0.0f) {
        content_width =
            std::max(content_width, to_content_size(min_width_value, box_frame_horizontal));
    }
    const float max_width_value = resolve_limit(style.max_width, containing_width);
    if (max_width_value >= 0.0f) {
        content_width =
            std::min(content_width, to_content_size(max_width_value, box_frame_horizontal));
    }
    content_width = ClampNonNegative(content_width);

    // 本盒的确定内容高度：由调用方指定或样式给出，其余情况由内容决定。
    // 确定高度同时作为子级百分比高度的解析基准。
    float definite_content_height = -1.0f;
    if (height_override >= 0.0f) {
        definite_content_height = ClampNonNegative(height_override);
    } else if (!style.height.IsAuto()) {
        const float specified = ResolveLength(style.height, style.font_size, root_font_size,
                                              height_base, options_.viewport_width,
                                              ViewportHeight());
        definite_content_height = to_content_size(specified, box_frame_vertical);
    }

    auto box = std::make_unique<LayoutBox>();
    box->element = element;
    box->style = style;
    if (is_image) {
        box->kind = BoxKind::Image;
    } else if (style.display == Display::ListItem) {
        box->kind = BoxKind::ListItem;
    } else {
        box->kind = BoxKind::Block;
    }

    box->padding_top = edges.padding_top;
    box->padding_right = edges.padding_right;
    box->padding_bottom = edges.padding_bottom;
    box->padding_left = edges.padding_left;
    box->border_top = edges.border_top;
    box->border_right = edges.border_right;
    box->border_bottom = edges.border_bottom;
    box->border_left = edges.border_left;

    const float content_left = border_left + edges.border_left + edges.padding_left;
    const float content_top = border_top + edges.border_top + edges.padding_top;
    box->border_box = RectF::FromLTRB(
        border_left, border_top,
        content_left + content_width + edges.padding_right + edges.border_right,
        content_top);
    box->content_box = RectF::FromLTRB(content_left, content_top, content_left + content_width,
                                       content_top);

    if (box->kind == BoxKind::ListItem) {
        box->marker = BuildMarker(style, list_ordinal);
    }

    std::vector<FlowItem> items;
    std::vector<FlowItem> positioned_items;
    if (element != nullptr && !is_image) {
        std::vector<FlowItem> collected;
        BuildFlowItems(*element, style, content_width, root_font_size,
                       style.IsFlexContainer() || style.IsGridContainer(), &collected);
        // 定位盒（absolute/fixed）脱离文档流，稍后按定位包含块单独排版。
        for (FlowItem& item : collected) {
            if (item.is_block && item.element != nullptr && item.style.IsOutOfFlow()) {
                positioned_items.push_back(std::move(item));
            } else {
                items.push_back(std::move(item));
            }
        }
    }

    const bool collapse_top = CanCollapseWithFirstChild(style, edges);
    const bool collapse_bottom = CanCollapseWithLastChild(style, edges);

    // 本元素的伪元素内容。没有真实子节点时也能单独构成匿名行内片段。
    std::vector<InlineAtom> pseudo_before_atoms;
    std::vector<InlineAtom> pseudo_after_atoms;
    std::vector<ComputedStyle> positioned_pseudos;
    if (element != nullptr && !is_image) {
        BuildPseudoAtoms(*element, style, "before", box.get(), &pseudo_before_atoms,
                         &positioned_pseudos);
        BuildPseudoAtoms(*element, style, "after", box.get(), &pseudo_after_atoms,
                         &positioned_pseudos);
    }
    /* summary 的展开箭头：未打开时显示向右三角形，打开时显示向下三角形。 */
    if (element != nullptr && !is_image && element->TagName() == "summary") {
        bool open = false;
        for (Node* ancestor = element->Parent(); ancestor != nullptr;
             ancestor = ancestor->Parent()) {
            if (ancestor->IsElement() &&
                static_cast<const Element*>(ancestor)->TagName() == "details") {
                open = static_cast<const Element*>(ancestor)->HasAttribute("open");
                break;
            }
        }
        std::vector<InlineAtom> marker_atoms;
        const std::vector<InlineFrame> no_frames;
        const std::string marker = open ? "\xE2\x96\xBC " : "\xE2\x96\xB6 ";
        AppendTextAtomsFromString(marker, nullptr, style, no_frames, &marker_atoms);
        pseudo_before_atoms.insert(pseudo_before_atoms.begin(), marker_atoms.begin(),
                                   marker_atoms.end());
    }
    /* list-style-position: inside：标记进入首行行内流，绘制层不再在盒外绘制。 */
    if (box->kind == BoxKind::ListItem && style.list_style_position == ListStylePosition::Inside &&
        !box->marker.empty()) {
        std::vector<InlineAtom> marker_atoms;
        const std::vector<InlineFrame> no_frames;
        AppendTextAtomsFromString(box->marker + " ", nullptr, style, no_frames, &marker_atoms);
        pseudo_before_atoms.insert(pseudo_before_atoms.begin(), marker_atoms.begin(),
                                   marker_atoms.end());
        box->marker.clear();
    }
    if (items.empty() &&
        (!pseudo_before_atoms.empty() || !pseudo_after_atoms.empty())) {
        FlowItem pseudo_item;
        pseudo_item.is_block = false;
        pseudo_item.pseudo_before = !pseudo_before_atoms.empty();
        pseudo_item.pseudo_after = !pseudo_after_atoms.empty();
        items.push_back(std::move(pseudo_item));
    }

    const bool inline_only = !items.empty() && items.size() == 1 && !items[0].is_block;
    float cursor = content_top;

    // 弹性盒容器使用独立排版路径：主轴分配剩余空间，不做外边距合并。
    if (element != nullptr && !is_image && style.IsFlexContainer()) {
        cursor = content_top + LayoutFlexChildren(style, items, content_left, content_top,
                                                  content_width, root_font_size, box.get(),
                                                  definite_content_height);
    } else if (element != nullptr && !is_image && style.IsGridContainer()) {
        cursor = content_top + LayoutGridChildren(style, items, content_left, content_top,
                                                  content_width, root_font_size, box.get());
    } else if (inline_only) {
        std::vector<InlineAtom> atoms = pseudo_before_atoms;
        BuildInlineRun(items[0].inline_nodes, style, content_width, box.get(), &atoms);
        atoms.insert(atoms.end(), pseudo_after_atoms.begin(), pseudo_after_atoms.end());
        const float lines_height =
            LayoutLines(atoms, content_width, style, content_left, content_top, box.get());
        cursor = content_top + lines_height;
    } else if (!items.empty()) {
        // 普通流中伪元素内容位于第一个与最后一个行内片段之间。
        size_t first_inline_index = items.size();
        size_t last_inline_index = items.size();
        for (size_t index = 0; index < items.size(); ++index) {
            if (!items[index].is_block) {
                if (first_inline_index == items.size()) {
                    first_inline_index = index;
                }
                last_inline_index = index;
            }
        }
        float pending_margin = 0.0f;
        bool has_previous = false;
        bool last_was_block = false;
        /* 浮动行状态：当前浮动行顶部、底部与左右已占用宽度。浮动元素不推进
           普通流游标，但后续普通流块需避让尚未结束的浮动行。 */
        float float_line_top = -1.0f;
        float float_line_bottom = -1.0f;
        float float_left_used = 0.0f;
        float float_right_used = 0.0f;
        for (size_t index = 0; index < items.size(); ++index) {
            FlowItem& item = items[index];

            if (!item.is_block) {
                std::vector<InlineAtom> atoms;
                if ((item.pseudo_before || index == first_inline_index) &&
                    !pseudo_before_atoms.empty()) {
                    atoms = pseudo_before_atoms;
                }
                BuildInlineRun(item.inline_nodes, style, content_width, box.get(), &atoms);
                if ((item.pseudo_after || index == last_inline_index) &&
                    !pseudo_after_atoms.empty()) {
                    atoms.insert(atoms.end(), pseudo_after_atoms.begin(),
                                 pseudo_after_atoms.end());
                }
                if (!HasVisibleAtoms(atoms)) {
                    // 纯空白片段不生成盒，上下块之间仍可继续合并外边距。
                    continue;
                }

                const float gap = has_previous ? CollapseMargins(pending_margin, 0.0f) : 0.0f;
                cursor += gap;

                ComputedStyle anonymous_style = style;
                anonymous_style.display = Display::Block;
                anonymous_style.width = CssLength();
                anonymous_style.height = CssLength();
                anonymous_style.margin = EdgeLengths();
                anonymous_style.padding = EdgeLengths();
                anonymous_style.border_width = EdgeLengths();
                anonymous_style.border_color_top = Transparent();
                anonymous_style.border_color_right = Transparent();
                anonymous_style.border_color_bottom = Transparent();
                anonymous_style.border_color_left = Transparent();
                anonymous_style.border_radius_top_left = CornerRadius();
                anonymous_style.border_radius_top_right = CornerRadius();
                anonymous_style.border_radius_bottom_right = CornerRadius();
                anonymous_style.border_radius_bottom_left = CornerRadius();
                anonymous_style.background_color = Transparent();
                anonymous_style.list_style_type = ListStyleType::None;

                auto anonymous_box = std::make_unique<LayoutBox>();
                anonymous_box->kind = BoxKind::Block;
                anonymous_box->element = nullptr;
                anonymous_box->style = anonymous_style;

                const float anonymous_top = cursor;
                const float lines_height =
                    LayoutLines(atoms, content_width, style, content_left, anonymous_top,
                                anonymous_box.get());
                anonymous_box->border_box =
                    RectF::FromLTRB(content_left, anonymous_top, content_left + content_width,
                                    anonymous_top + lines_height);
                anonymous_box->content_box = anonymous_box->border_box;
                cursor = anonymous_top + lines_height;

                box->children.push_back(std::move(anonymous_box));
                pending_margin = 0.0f;
                has_previous = true;
                last_was_block = false;
                continue;
            }

            /* 浮动元素脱离普通流：按当前浮动行靠左或靠右排列，超出容器宽度时
               换到下一浮动行。浮动不推进普通流游标，也不参与外边距合并。 */
            if (item.element != nullptr && item.style.float_side != FloatSide::None) {
                if (float_line_top < 0.0f || float_line_bottom <= cursor) {
                    float_line_top = cursor;
                    float_line_bottom = cursor;
                    float_left_used = 0.0f;
                    float_right_used = 0.0f;
                }
                const EdgeValues float_edges =
                    ResolveEdges(item.style, content_width, root_font_size);
                const float float_frame =
                    float_edges.padding_left + float_edges.padding_right +
                    float_edges.border_left + float_edges.border_right;
                float float_width = 0.0f;
                if (!item.style.width.IsAuto()) {
                    const float specified =
                        ResolveLength(item.style.width, item.style.font_size, root_font_size,
                                      content_width, options_.viewport_width, ViewportHeight());
                    float_width = item.style.border_box
                                      ? ClampNonNegative(specified - float_frame)
                                      : ClampNonNegative(specified);
                } else {
                    float_width = ClampNonNegative(
                        MeasureTableCellIntrinsicWidth(*item.element, item.style, content_width));
                }
                const float float_outer = float_width + float_frame + item.margin_left +
                                          item.margin_right;
                const bool to_right = item.style.float_side == FloatSide::Right;
                if ((to_right ? float_right_used : float_left_used) + float_outer >
                    content_width + kEpsilon) {
                    float_line_top = float_line_bottom;
                    float_line_bottom = float_line_top;
                    float_left_used = 0.0f;
                    float_right_used = 0.0f;
                }
                float float_left = content_left + float_left_used + item.margin_left;
                if (to_right) {
                    float_left = content_left + content_width - float_right_used -
                                 float_outer + item.margin_left;
                }
                std::unique_ptr<LayoutBox> float_box =
                    LayoutBlock(item.element, item.style, float_left, float_line_top,
                                content_width, root_font_size, item.list_ordinal, float_width,
                                definite_content_height);
                const float float_outer_height = float_box->border_box.Height() +
                                                 item.margin_top + item.margin_bottom;
                if (to_right) {
                    float_right_used += float_outer;
                } else {
                    float_left_used += float_outer;
                }
                float_line_bottom =
                    std::max(float_line_bottom, float_line_top + float_outer_height);
                box->children.push_back(std::move(float_box));
                continue;
            }

            float top_margin = item.margin_top;
            if (!has_previous && collapse_top) {
                // 与父块上外边距合并的首个可见块级子块，其自身边距已计入父块外侧。
                top_margin = 0.0f;
            }
            const float gap = has_previous ? CollapseMargins(pending_margin, top_margin)
                                           : top_margin;
            cursor += gap;
            /* clear 与普通流避让：尚未结束的浮动行会把后续块级盒下移，
               避免块盒与浮动元素重叠。行内文字暂不支持绕排。 */
            if (float_line_bottom > cursor) {
                cursor = float_line_bottom;
            }

            const float child_left = content_left + item.margin_left;
            std::unique_ptr<LayoutBox> child =
                LayoutBlock(item.element, item.style, child_left, cursor, content_width,
                            root_font_size, item.list_ordinal, -1.0f,
                            definite_content_height);
            // 水平外边距为 auto 时吸收剩余空间：两侧 auto 居中，仅左侧 auto 右对齐。
            if (item.element != nullptr &&
                (item.style.margin.left.IsAuto() || item.style.margin.right.IsAuto())) {
                const float free = content_width - child->border_box.Width() -
                                   item.margin_left - item.margin_right;
                if (free > 0.0f) {
                    const bool left_auto = item.style.margin.left.IsAuto();
                    const bool right_auto = item.style.margin.right.IsAuto();
                    float shift = 0.0f;
                    if (left_auto && right_auto) {
                        shift = free * 0.5f;
                    } else if (left_auto) {
                        shift = free;
                    }
                    if (shift > 0.0f) {
                        TranslateBox(child.get(), shift, 0.0f);
                    }
                }
            }
            cursor = child->border_box.bottom;
            box->children.push_back(std::move(child));

            pending_margin = item.margin_bottom;
            has_previous = true;
            last_was_block = true;
        }

        if (has_previous) {
            if (collapse_bottom && last_was_block) {
                // 末个子块的下外边距已并入父块外侧。
                pending_margin = 0.0f;
            }
            cursor += pending_margin;
        }
    }

    float content_height = 0.0f;
    if (definite_content_height >= 0.0f) {
        content_height = definite_content_height;
    } else if (is_image) {
        const float attribute_height =
            ParseHtmlDimension(element->GetAttribute("height"), containing_width);
        if (attribute_height >= 0.0f) {
            content_height = attribute_height;
        } else {
            content_height = is_canvas || is_iframe || is_svg ? 150.0f : 200.0f;
        }
    } else {
        content_height = ClampNonNegative(cursor - content_top);
    }

    /* textarea 的 rows 属性给出可见行数：内容不足时仍保留对应高度，与浏览器
       一致；未指定 rows 时按默认 2 行。作者的 height 与 min-height 仍优先。 */
    if (element != nullptr && AsciiLower(element->TagName()) == "textarea" &&
        style.height.IsAuto()) {
        int rows = 2;
        const std::string rows_text = element->GetAttribute("rows");
        if (!rows_text.empty()) {
            char* end = nullptr;
            const long parsed = std::strtol(rows_text.c_str(), &end, 10);
            if (end != rows_text.c_str() && end != nullptr && *end == '\0' && parsed >= 1) {
                rows = parsed > 1000 ? 1000 : static_cast<int>(parsed);
            }
        }
        const FontMetrics& rows_metrics = MetricsFor(FontFor(style), style.font_size);
        const float rows_height =
            ResolveLineHeight(style, rows_metrics) * static_cast<float>(rows);
        content_height = std::max(content_height, rows_height);
    }

    // 最小/最大高度：百分比按包含块确定高度解析，border-box 时换算为内容尺寸。
    const float min_height_value = resolve_limit(style.min_height, height_base);
    if (min_height_value >= 0.0f) {
        content_height =
            std::max(content_height, to_content_size(min_height_value, box_frame_vertical));
    }
    const float max_height_value = resolve_limit(style.max_height, height_base);
    if (max_height_value >= 0.0f) {
        content_height =
            std::min(content_height, to_content_size(max_height_value, box_frame_vertical));
    }
    content_height = ClampNonNegative(content_height);

    box->content_box = RectF::FromLTRB(content_left, content_top, content_left + content_width,
                                       content_top + content_height);
    box->border_box = RectF::FromLTRB(
        border_left, border_top,
        content_left + content_width + edges.padding_right + edges.border_right,
        content_top + content_height + edges.padding_bottom + edges.border_bottom);

    /* 定位盒排版：absolute 相对最近的非 static 祖先的内边距盒，fixed 相对视口。
       本盒自身为定位元素时，其内边距盒成为子级定位盒的包含块。 */
    if (!positioned_items.empty()) {
        RectF outer_block = positioning_block_;
        if (style.position != Position::Static) {
            positioning_block_ = RectF::FromLTRB(
                box->content_box.left - edges.padding_left,
                box->content_box.top - edges.padding_top,
                box->content_box.right + edges.padding_right,
                box->content_box.bottom + edges.padding_bottom);
        }
        for (FlowItem& item : positioned_items) {
            const RectF& block = item.style.position == Position::Fixed
                                     ? RectF::FromLTRB(0.0f, 0.0f, options_.viewport_width,
                                                       ViewportHeight())
                                     : positioning_block_;
            LayoutPositioned(item, block, root_font_size, box.get());
        }
        positioning_block_ = outer_block;
    }

    /* 绝对定位伪元素：不参与普通流，按包含块与 top/left/width/height 独立排版。
       宿主为定位元素时包含块为其内边距盒，否则沿用当前定位包含块；
       固定定位伪元素相对视口。 */
    if (!positioned_pseudos.empty()) {
        const RectF owner_padding = RectF::FromLTRB(
            box->content_box.left - edges.padding_left,
            box->content_box.top - edges.padding_top,
            box->content_box.right + edges.padding_right,
            box->content_box.bottom + edges.padding_bottom);
        for (const ComputedStyle& pseudo : positioned_pseudos) {
            const RectF pseudo_block =
                pseudo.position == Position::Fixed
                    ? RectF::FromLTRB(0.0f, 0.0f, options_.viewport_width, ViewportHeight())
                    : (style.position != Position::Static ? owner_padding : positioning_block_);
            const float pseudo_block_width = std::max(0.0f, pseudo_block.Width());
            const float pseudo_block_height = std::max(0.0f, pseudo_block.Height());
            const EdgeValues pseudo_edges =
                ResolveEdges(pseudo, pseudo_block_width, root_font_size);
            const float pseudo_frame_width =
                pseudo_edges.padding_left + pseudo_edges.padding_right +
                pseudo_edges.border_left + pseudo_edges.border_right;
            const float pseudo_frame_height =
                pseudo_edges.padding_top + pseudo_edges.padding_bottom +
                pseudo_edges.border_top + pseudo_edges.border_bottom;
            float pseudo_content_width = 0.0f;
            if (!pseudo.width.IsAuto()) {
                const float specified = ClampNonNegative(ResolveLength(
                    pseudo.width, pseudo.font_size, root_font_size, pseudo_block_width,
                    options_.viewport_width, ViewportHeight()));
                pseudo_content_width = pseudo.border_box
                                           ? ClampNonNegative(specified - pseudo_frame_width)
                                           : specified;
            }
            float pseudo_content_height = 0.0f;
            if (!pseudo.height.IsAuto()) {
                const float specified = ClampNonNegative(ResolveLength(
                    pseudo.height, pseudo.font_size, root_font_size, pseudo_block_height,
                    options_.viewport_width, ViewportHeight()));
                pseudo_content_height = pseudo.border_box
                                            ? ClampNonNegative(specified - pseudo_frame_height)
                                            : specified;
            }
            const float pseudo_border_width = pseudo_content_width + pseudo_frame_width;
            const float pseudo_border_height = pseudo_content_height + pseudo_frame_height;

            float pseudo_left = pseudo_block.left;
            if (!pseudo.position_left.IsAuto()) {
                pseudo_left =
                    pseudo_block.left +
                    ResolveLength(pseudo.position_left, pseudo.font_size, root_font_size,
                                  pseudo_block_width, options_.viewport_width, ViewportHeight());
            } else if (!pseudo.position_right.IsAuto()) {
                pseudo_left =
                    pseudo_block.right -
                    ResolveLength(pseudo.position_right, pseudo.font_size, root_font_size,
                                  pseudo_block_width, options_.viewport_width, ViewportHeight()) -
                    pseudo_border_width;
            }
            float pseudo_top = pseudo_block.top;
            if (!pseudo.position_top.IsAuto()) {
                pseudo_top =
                    pseudo_block.top +
                    ResolveLength(pseudo.position_top, pseudo.font_size, root_font_size,
                                  pseudo_block_height, options_.viewport_width, ViewportHeight());
            } else if (!pseudo.position_bottom.IsAuto()) {
                pseudo_top =
                    pseudo_block.bottom -
                    ResolveLength(pseudo.position_bottom, pseudo.font_size, root_font_size,
                                  pseudo_block_height, options_.viewport_width, ViewportHeight()) -
                    pseudo_border_height;
            }
            /* transform 平移：百分比相对自身边框盒尺寸，用于 translate(-50%,-50%)
               这类以中心对齐的写法。 */
            if (pseudo.has_transform) {
                pseudo_left += ResolveLength(pseudo.transform_translate_x, pseudo.font_size,
                                             root_font_size, pseudo_border_width,
                                             options_.viewport_width, ViewportHeight());
                pseudo_top += ResolveLength(pseudo.transform_translate_y, pseudo.font_size,
                                            root_font_size, pseudo_border_height,
                                            options_.viewport_width, ViewportHeight());
            }

            auto pseudo_box = std::make_unique<LayoutBox>();
            pseudo_box->kind = BoxKind::Block;
            pseudo_box->style = pseudo;
            pseudo_box->padding_top = pseudo_edges.padding_top;
            pseudo_box->padding_right = pseudo_edges.padding_right;
            pseudo_box->padding_bottom = pseudo_edges.padding_bottom;
            pseudo_box->padding_left = pseudo_edges.padding_left;
            pseudo_box->border_top = pseudo_edges.border_top;
            pseudo_box->border_right = pseudo_edges.border_right;
            pseudo_box->border_bottom = pseudo_edges.border_bottom;
            pseudo_box->border_left = pseudo_edges.border_left;
            pseudo_box->border_box =
                RectF::FromLTRB(pseudo_left, pseudo_top, pseudo_left + pseudo_border_width,
                                pseudo_top + pseudo_border_height);
            pseudo_box->content_box = RectF::FromLTRB(
                pseudo_left + pseudo_edges.border_left + pseudo_edges.padding_left,
                pseudo_top + pseudo_edges.border_top + pseudo_edges.padding_top,
                pseudo_left + pseudo_border_width - pseudo_edges.border_right -
                    pseudo_edges.padding_right,
                pseudo_top + pseudo_border_height - pseudo_edges.border_bottom -
                    pseudo_edges.padding_bottom);
            /* transform: scale()：以伪元素盒中心为原点缩放，用于 scaleY(0/1)
               这类展开动画的静态状态。 */
            if (pseudo.has_transform && (pseudo.scale_x != 1.0f || pseudo.scale_y != 1.0f)) {
                const float center_x =
                    (pseudo_box->border_box.left + pseudo_box->border_box.right) * 0.5f;
                const float center_y =
                    (pseudo_box->border_box.top + pseudo_box->border_box.bottom) * 0.5f;
                ScaleBox(pseudo_box.get(), center_x, center_y, pseudo.scale_x, pseudo.scale_y);
            }
            box->children.push_back(std::move(pseudo_box));
        }
    }

    /* relative 偏移与 transform 平移：整体平移本盒及其子树，不改变兄弟盒位置。
       百分比平移分别相对自身盒宽与盒高。 */
    float shift_x = 0.0f;
    float shift_y = 0.0f;
    /* sticky 没有滚动容器信息时按普通流处理：不应用 top/left 偏移，避免在
       静态排版中整体下移；relative 才应用偏移。 */
    if (style.position == Position::Relative) {
        shift_x = ResolveLength(style.position_left, style.font_size, root_font_size,
                                box->border_box.Width(), options_.viewport_width,
                                ViewportHeight());
        if (style.position_left.IsAuto()) {
            shift_x = -ResolveLength(style.position_right, style.font_size, root_font_size,
                                     box->border_box.Width(), options_.viewport_width,
                                     ViewportHeight());
        }
        shift_y = ResolveLength(style.position_top, style.font_size, root_font_size,
                                box->border_box.Height(), options_.viewport_width,
                                ViewportHeight());
        if (style.position_top.IsAuto()) {
            shift_y = -ResolveLength(style.position_bottom, style.font_size, root_font_size,
                                     box->border_box.Height(), options_.viewport_width,
                                     ViewportHeight());
        }
    }
    if (style.has_transform) {
        shift_x += ResolveLength(style.transform_translate_x, style.font_size, root_font_size,
                                 box->border_box.Width(), options_.viewport_width,
                                 ViewportHeight());
        shift_y += ResolveLength(style.transform_translate_y, style.font_size, root_font_size,
                                 box->border_box.Height(), options_.viewport_width,
                                 ViewportHeight());
    }
    if (shift_x != 0.0f || shift_y != 0.0f) {
        TranslateBox(box.get(), shift_x, shift_y);
    }
    /* transform: scale()：以盒中心为原点缩放盒与子树（默认 transform-origin
       为 50% 50%）。缩放不改变兄弟盒位置。 */
    if (style.has_transform && (style.scale_x != 1.0f || style.scale_y != 1.0f)) {
        const float center_x = (box->border_box.left + box->border_box.right) * 0.5f;
        const float center_y = (box->border_box.top + box->border_box.bottom) * 0.5f;
        ScaleBox(box.get(), center_x, center_y, style.scale_x, style.scale_y);
    }
    return box;
}

// ---------------------------------------------------------------------------
// 表格排版
// ---------------------------------------------------------------------------

// 收集一行内的直接 td/th 子元素。display:none 的单元格跳过。列索引从当前行
// 的第一个空闲列开始，逐格累加 colspan 并跳过被上方跨行单元格占用的位置；
// 每个单元格按 rowspan 把所覆盖的列登记为占用，后续行据此预留空位。
// 若 colspan 覆盖到已被占用的列，把跨度收窄到该列之前，避免单元格相互重叠。
void LayoutEngine::CollectTableRow(const Element& row_element, const ComputedStyle& table_style,
                                   float containing_width, float root_font_size, int row_index,
                                   std::vector<int>* occupied_until, TableRowPlan* row) {
    // 从 from 起查找第一个未被跨行单元格占用的列。
    const auto first_free_column = [occupied_until, row_index](int from) {
        int column = from;
        while (column < static_cast<int>(occupied_until->size()) &&
               (*occupied_until)[column] > row_index) {
            ++column;
        }
        return column;
    };

    int column_index = first_free_column(0);
    int furthest_column = column_index;
    for (const NodePtr& child : row_element.Children()) {
        if (!child || !child->IsElement()) {
            continue;
        }
        const Element& cell_element = static_cast<const Element&>(*child);
        const std::string& tag_name = cell_element.TagName();
        if (tag_name != "td" && tag_name != "th") {
            // 行内的文字、注释与其他元素不构成单元格。
            continue;
        }

        TableCellPlan cell;
        cell.element = &cell_element;
        cell.style = resolver_.ComputeFor(cell_element, table_style);
        if (cell.style.display == Display::None) {
            continue;
        }

        const EdgeValues cell_edges = ResolveEdges(cell.style, containing_width, root_font_size);
        cell.padding_top = cell_edges.padding_top;
        cell.padding_right = cell_edges.padding_right;
        cell.padding_bottom = cell_edges.padding_bottom;
        cell.padding_left = cell_edges.padding_left;
        cell.border_top = cell_edges.border_top;
        cell.border_right = cell_edges.border_right;
        cell.border_bottom = cell_edges.border_bottom;
        cell.border_left = cell_edges.border_left;

        cell.column_index = column_index;
        cell.column_span = ParseTableColumnSpan(cell_element);
        cell.row_span = ParseTableRowSpan(cell_element);
        cell.row_index = row_index;

        // 跨列范围若与上方跨行单元格的占用区重叠，收窄跨度。
        for (int offset = 1; offset < cell.column_span; ++offset) {
            const int column = column_index + offset;
            if (column < static_cast<int>(occupied_until->size()) &&
                (*occupied_until)[column] > row_index) {
                cell.column_span = offset;
                break;
            }
        }

        const float content_intrinsic =
            MeasureTableCellIntrinsicWidth(cell_element, cell.style, containing_width);
        cell.intrinsic_width = ClampNonNegative(content_intrinsic + cell.padding_left +
                                                cell.padding_right + cell.border_left +
                                                cell.border_right);

        // 登记本单元格占用的网格，行索引为收集顺序（跳过 display:none 的行）。
        const int span_end = column_index + cell.column_span;
        if (static_cast<int>(occupied_until->size()) < span_end) {
            occupied_until->resize(static_cast<size_t>(span_end), 0);
        }
        for (int column = column_index; column < span_end; ++column) {
            const int occupied_row = row_index + cell.row_span;
            if ((*occupied_until)[column] < occupied_row) {
                (*occupied_until)[column] = occupied_row;
            }
        }

        furthest_column = std::max(furthest_column, span_end);
        column_index = first_free_column(span_end);
        furthest_column = std::max(furthest_column, column_index);
        row->cells.push_back(std::move(cell));
    }
    row->column_count = furthest_column;
}

// 收集表格中的行：直接 tr 子元素，以及 thead/tbody/tfoot 容器下的 tr。
// 同时记录表格中出现的第一个 caption 元素，其余 caption 忽略。
void LayoutEngine::CollectTableRows(const Element& table, const ComputedStyle& table_style,
                                    float containing_width, float root_font_size,
                                    const Element** caption_element,
                                    std::vector<TableRowPlan>* rows) {
    // 每列被跨行单元格占用到的行索引（不含）；跨行跨度以收集顺序的行号为基准。
    std::vector<int> occupied_until;
    int row_index = 0;

    const auto collect_row = [&](const Element& row_element) {
        const ComputedStyle row_style = resolver_.ComputeFor(row_element, table_style);
        if (row_style.display == Display::None) {
            return;
        }
        TableRowPlan row;
        CollectTableRow(row_element, table_style, containing_width, root_font_size, row_index,
                        &occupied_until, &row);
        rows->push_back(std::move(row));
        ++row_index;
    };

    for (const NodePtr& child : table.Children()) {
        if (!child || !child->IsElement()) {
            continue;
        }
        const Element& child_element = static_cast<const Element&>(*child);
        const std::string& tag_name = child_element.TagName();

        if (tag_name == "caption") {
            if (*caption_element == nullptr) {
                *caption_element = &child_element;
            }
            continue;
        }

        if (tag_name == "tr") {
            collect_row(child_element);
            continue;
        }

        if (tag_name != "thead" && tag_name != "tbody" && tag_name != "tfoot") {
            continue;
        }
        const ComputedStyle section_style = resolver_.ComputeFor(child_element, table_style);
        if (section_style.display == Display::None) {
            continue;
        }
        for (const NodePtr& section_child : child_element.Children()) {
            if (!section_child || !section_child->IsElement()) {
                continue;
            }
            const Element& row_element = static_cast<const Element&>(*section_child);
            if (row_element.TagName() != "tr") {
                continue;
            }
            collect_row(row_element);
        }
    }
}

// 表格排版：列宽由单元格内容固有宽度推导，行高先由本行单行单元格决定，
// 再把跨行单元格的高度差额分摊到所跨各行。caption 按表格内容宽度排在所有行
// 之上；border-spacing 在单元格之间与表格内边缘留出间距。border-collapse 为
// collapse 时，非首列单元格的左边框与非首行单元格的上边框清零，使相邻单元格
// 共享的边框线只绘制一次。
std::unique_ptr<LayoutBox> LayoutEngine::LayoutTable(const Element* element,
                                                     const ComputedStyle& style,
                                                     float border_left, float border_top,
                                                     float containing_width,
                                                     float root_font_size, float width_override) {
    const EdgeValues table_edges = ResolveEdges(style, containing_width, root_font_size);

    // 表格可用内容宽度：包含块宽度减去表格自身左右外边距、边框与内边距。
    const float available_content_width = ClampNonNegative(
        containing_width - table_edges.margin_left - table_edges.margin_right -
        table_edges.border_left - table_edges.border_right - table_edges.padding_left -
        table_edges.padding_right);

    // 显式宽度（含 width_override）直接采用；自动宽度在得到列固有宽度后按
    // 收缩适应计算，不再无条件铺满包含块。
    float specified_content_width = -1.0f;
    if (width_override >= 0.0f) {
        specified_content_width = ClampNonNegative(width_override);
    } else if (!style.width.IsAuto()) {
        specified_content_width = ClampNonNegative(
            ResolveLength(style.width, style.font_size, root_font_size, containing_width,
                          options_.viewport_width, ViewportHeight()));
    }

    // 间距百分比基准：显式宽度时取该内容宽度，自动宽度时取可用内容宽度。
    const float spacing_percent_base =
        specified_content_width >= 0.0f ? specified_content_width : available_content_width;
    const float border_spacing_x =
        ResolveBorderSpacingComponent(style.border_spacing_x, spacing_percent_base);
    const float border_spacing_y =
        ResolveBorderSpacingComponent(style.border_spacing_y, spacing_percent_base);
    const bool collapse_borders = style.border_collapse == BorderCollapse::Collapse;

    std::vector<TableRowPlan> rows;
    const Element* caption_element = nullptr;
    if (element != nullptr) {
        CollectTableRows(*element, style, containing_width, root_font_size, &caption_element,
                         &rows);
    }

    // 列数取所有单元格的列终点与跨行占用的最大列数。
    size_t column_count = 0;
    for (const TableRowPlan& row : rows) {
        column_count = std::max(column_count, static_cast<size_t>(row.column_count));
        for (const TableCellPlan& cell : row.cells) {
            column_count = std::max(
                column_count, static_cast<size_t>(cell.column_index + cell.column_span));
        }
    }

    // 列固有宽度：先取该列所有单列单元格（colspan=1）内容宽度的最大值。
    std::vector<float> column_widths(column_count, 0.0f);
    for (const TableRowPlan& row : rows) {
        for (const TableCellPlan& cell : row.cells) {
            if (cell.column_span != 1) {
                continue;
            }
            const size_t column = static_cast<size_t>(cell.column_index);
            if (column >= column_count) {
                continue;
            }
            column_widths[column] = std::max(column_widths[column], cell.intrinsic_width);
        }
    }

    // 跨列单元格的宽度均分到所跨各列，只补足现有列宽之和（含跨度内部间距）
    // 不足的部分。
    for (const TableRowPlan& row : rows) {
        for (const TableCellPlan& cell : row.cells) {
            if (cell.column_span <= 1) {
                continue;
            }
            const size_t begin = static_cast<size_t>(cell.column_index);
            if (begin >= column_count) {
                continue;
            }
            const size_t span = std::min(static_cast<size_t>(cell.column_span),
                                         column_count - begin);
            float spanned_width = static_cast<float>(span - 1) * border_spacing_x;
            for (size_t column = begin; column < begin + span; ++column) {
                spanned_width += column_widths[column];
            }
            if (cell.intrinsic_width <= spanned_width) {
                continue;
            }
            const float extra = (cell.intrinsic_width - spanned_width) / static_cast<float>(span);
            for (size_t column = begin; column < begin + span; ++column) {
                column_widths[column] += extra;
            }
        }
    }

    // 表格内容宽度与列宽目标值：自动宽度取 min(可用内容宽度, 固有总宽)，
    // 显式宽度按给定值；两者都要为单元格之间的间距留出空间。
    const float spacing_total_x = static_cast<float>(column_count + 1) * border_spacing_x;
    float natural_total = 0.0f;
    for (float column_width : column_widths) {
        natural_total += column_width;
    }

    float content_width = 0.0f;
    if (specified_content_width >= 0.0f) {
        content_width = specified_content_width;
    } else {
        content_width = ClampNonNegative(
            std::min(available_content_width, natural_total + spacing_total_x));
    }
    const float target_column_total = ClampNonNegative(content_width - spacing_total_x);

    // 把列宽调整到目标宽度：偏窄时各列等增量扩宽，偏宽时按比例收窄。
    if (column_count > 0) {
        if (natural_total <= 0.0f) {
            const float share = target_column_total / static_cast<float>(column_count);
            for (float& column_width : column_widths) {
                column_width = share;
            }
        } else {
            const float difference = target_column_total - natural_total;
            if (difference >= 0.0f) {
                const float increment = difference / static_cast<float>(column_count);
                for (float& column_width : column_widths) {
                    column_width += increment;
                }
            } else {
                const float ratio = target_column_total / natural_total;
                for (float& column_width : column_widths) {
                    column_width *= ratio;
                }
            }
        }

        // 每列宽度必须为正：分配后仍为 0 的列补足 1px；列宽总和因此变大时，
        // 表格内容宽度随之取实际总和，避免单元格相互覆盖。
        float adjusted_total = 0.0f;
        for (float& column_width : column_widths) {
            if (column_width < 1.0f) {
                column_width = 1.0f;
            }
            adjusted_total += column_width;
        }
        content_width = adjusted_total + spacing_total_x;
    }

    auto box = std::make_unique<LayoutBox>();
    box->kind = BoxKind::Block;
    box->element = element;
    box->style = style;
    box->padding_top = table_edges.padding_top;
    box->padding_right = table_edges.padding_right;
    box->padding_bottom = table_edges.padding_bottom;
    box->padding_left = table_edges.padding_left;
    box->border_top = table_edges.border_top;
    box->border_right = table_edges.border_right;
    box->border_bottom = table_edges.border_bottom;
    box->border_left = table_edges.border_left;

    const float content_left = border_left + table_edges.border_left + table_edges.padding_left;
    const float content_top = border_top + table_edges.border_top + table_edges.padding_top;

    // caption 按表格内容宽度排版，放在所有行之上；其高度（含上下外边距）
    // 计入表格内容盒高度，并使单元格起始 y 随之下移。
    std::unique_ptr<LayoutBox> caption_box;
    float rows_content_top = content_top;
    if (caption_element != nullptr) {
        const ComputedStyle caption_style = resolver_.ComputeFor(*caption_element, style);
        if (caption_style.display != Display::None) {
            const EdgeValues caption_edges =
                ResolveEdges(caption_style, content_width, root_font_size);
            const float caption_top = content_top + caption_edges.margin_top;
            caption_box = LayoutBlock(caption_element, caption_style,
                                      content_left + caption_edges.margin_left, caption_top,
                                      content_width, root_font_size, 0, -1.0f);
            rows_content_top = caption_box->border_box.bottom + caption_edges.margin_bottom;
        }
    }

    // 单元格先按最终列宽在原点完成内部排版，记录自然高度；随后按网格平移，
    // 再按行高与跨行高度拉伸底边。单元格自身外边距按 0 处理。
    std::vector<LaidTableCell> laid_cells;
    for (size_t row_index = 0; row_index < rows.size(); ++row_index) {
        const TableRowPlan& row = rows[row_index];
        for (const TableCellPlan& cell : row.cells) {
            const size_t begin = static_cast<size_t>(cell.column_index);
            if (begin >= column_count) {
                continue;
            }
            LaidTableCell laid;
            laid.column_begin = begin;
            laid.column_span = std::min(static_cast<size_t>(cell.column_span),
                                        column_count - begin);
            laid.row_begin = row_index;
            laid.row_end = std::min(rows.size(),
                                    row_index + static_cast<size_t>(cell.row_span));

            float spanned_width =
                static_cast<float>(laid.column_span - 1) * border_spacing_x;
            for (size_t column = begin; column < begin + laid.column_span; ++column) {
                spanned_width += column_widths[column];
            }

            // 内容宽度扣掉单元格自身的左右内边距与边框，保证文字在列宽内排版。
            const float cell_content_width = ClampNonNegative(
                spanned_width - cell.padding_left - cell.padding_right - cell.border_left -
                cell.border_right);
            laid.box = LayoutBlock(cell.element, cell.style, 0.0f, 0.0f, spanned_width,
                                   root_font_size, 0, cell_content_width);
            laid_cells.push_back(std::move(laid));
        }
    }

    // 行高：先取本行 rowspan=1 单元格边框盒高度的最大值；跨行单元格的总高度
    // 若超出所跨各行（含跨度内部间距）的已有高度，差额均分到所跨各行。
    std::vector<float> row_heights(rows.size(), 0.0f);
    for (const LaidTableCell& laid : laid_cells) {
        if (laid.row_end - laid.row_begin <= 1) {
            row_heights[laid.row_begin] =
                std::max(row_heights[laid.row_begin], laid.box->border_box.Height());
        }
    }
    for (const LaidTableCell& laid : laid_cells) {
        const size_t spanned_rows = laid.row_end - laid.row_begin;
        if (spanned_rows <= 1) {
            continue;
        }
        float allocated_height =
            static_cast<float>(spanned_rows - 1) * border_spacing_y;
        for (size_t row_index = laid.row_begin; row_index < laid.row_end; ++row_index) {
            allocated_height += row_heights[row_index];
        }
        const float required_height = laid.box->border_box.Height();
        if (required_height <= allocated_height) {
            continue;
        }
        const float extra =
            (required_height - allocated_height) / static_cast<float>(spanned_rows);
        for (size_t row_index = laid.row_begin; row_index < laid.row_end; ++row_index) {
            row_heights[row_index] += extra;
        }
    }

    // 网格起点：首尾都留出间距，列/行起点随之累加。
    std::vector<float> column_offsets(column_count, 0.0f);
    float cursor_x = content_left + border_spacing_x;
    for (size_t column = 0; column < column_count; ++column) {
        column_offsets[column] = cursor_x;
        cursor_x += column_widths[column] + border_spacing_x;
    }

    std::vector<float> row_offsets(rows.size(), 0.0f);
    float cursor_y = rows_content_top + border_spacing_y;
    for (size_t row_index = 0; row_index < rows.size(); ++row_index) {
        row_offsets[row_index] = cursor_y;
        cursor_y += row_heights[row_index] + border_spacing_y;
    }
    const float table_content_bottom = cursor_y;

    // 平移单元格到网格位置，拉伸到所跨行的底边；collapse 时清零内部共享边框。
    for (LaidTableCell& laid : laid_cells) {
        const float target_left = column_offsets[laid.column_begin];
        const float target_top = row_offsets[laid.row_begin];
        TranslateBox(laid.box.get(), target_left, target_top);

        const float span_bottom = row_offsets[laid.row_end - 1] + row_heights[laid.row_end - 1];
        laid.box->border_box.bottom = span_bottom;
        laid.box->content_box.bottom = span_bottom - laid.box->padding_bottom -
                                       laid.box->border_bottom;

        if (collapse_borders) {
            if (laid.column_begin > 0) {
                laid.box->border_left = 0.0f;
            }
            if (laid.row_begin > 0) {
                laid.box->border_top = 0.0f;
            }
        }
    }

    // 子盒按绘制顺序排列：caption 在前，单元格按行顺序在后。
    if (caption_box) {
        box->children.push_back(std::move(caption_box));
    }
    for (LaidTableCell& laid : laid_cells) {
        box->children.push_back(std::move(laid.box));
    }

    const float content_height = ClampNonNegative(table_content_bottom - content_top);
    box->content_box = RectF::FromLTRB(content_left, content_top, content_left + content_width,
                                       content_top + content_height);
    box->border_box = RectF::FromLTRB(
        border_left, border_top,
        content_left + content_width + table_edges.padding_right + table_edges.border_right,
        content_top + content_height + table_edges.padding_bottom + table_edges.border_bottom);
    return box;
}

// ---------------------------------------------------------------------------
// 行内内容构建
// ---------------------------------------------------------------------------

void LayoutEngine::BuildInlineRun(const std::vector<const Node*>& nodes,
                                  const ComputedStyle& base_style, float available_width,
                                  LayoutBox* owner, std::vector<InlineAtom>* atoms) {
    const std::vector<InlineFrame> no_frames;
    for (const Node* node : nodes) {
        if (node == nullptr) {
            continue;
        }
        if (node->IsText()) {
            const TextNode& text_node = static_cast<const TextNode&>(*node);
            AppendTextAtoms(text_node, base_style, no_frames, atoms);
            continue;
        }
        if (!node->IsElement()) {
            continue;
        }
        const Element& element = static_cast<const Element&>(*node);
        const ComputedStyle style = resolver_.ComputeFor(element, base_style);
        if (style.display == Display::None) {
            continue;
        }
        BuildInlineNode(element, style, no_frames, available_width, owner, atoms);
    }
}

void LayoutEngine::BuildInlineNode(const Element& element, const ComputedStyle& style,
                                   const std::vector<InlineFrame>& frames, float available_width,
                                   LayoutBox* owner, std::vector<InlineAtom>* atoms) {
    const std::string& tag_name = element.TagName();
    if (tag_name == "br") {
        AppendBreakAtom(style, frames, atoms);
        return;
    }
    if (tag_name == "img" || tag_name == "canvas" || tag_name == "iframe" ||
        tag_name == "svg") {
        AppendImageAtom(element, style, frames, available_width, owner, atoms);
        return;
    }
    if (tag_name == "input" || tag_name == "select") {
        // 表单控件按控件类型自行决定尺寸与标签文本，不进入通用行内块路径。
        AppendFormControlAtom(element, style, frames, available_width, owner, atoms);
        return;
    }
    if (style.display == Display::InlineBlock || style.display == Display::Block ||
        style.display == Display::ListItem) {
        // 行内流中出现的块级盒按行内块处理，保证内容仍然参与排布。
        AppendInlineBlockAtom(element, style, frames, available_width, owner, atoms);
        return;
    }

    // 普通行内元素：记录片段样式并递归其子节点。
    std::vector<InlineFrame> extended = frames;
    InlineFrame frame;
    frame.element = &element;
    frame.style = StoreInlineStyle(style);
    extended.push_back(frame);

    // 行内元素自身的伪元素内容位于其子节点之前与之后。
    BuildPseudoAtoms(element, style, "before", owner, atoms);

    for (const NodePtr& child : element.Children()) {
        if (!child) {
            continue;
        }
        if (child->IsText()) {
            const TextNode& text_node = static_cast<const TextNode&>(*child);
            AppendTextAtoms(text_node, style, extended, atoms);
            continue;
        }
        if (!child->IsElement()) {
            continue;
        }
        const Element& child_element = static_cast<const Element&>(*child);
        const ComputedStyle child_style = resolver_.ComputeFor(child_element, style);
        if (child_style.display == Display::None) {
            continue;
        }
        BuildInlineNode(child_element, child_style, extended, available_width, owner, atoms);
    }

    BuildPseudoAtoms(element, style, "after", owner, atoms);
}

// 按空白处理规则把文本节点切分为行内原子。空白折叠跨节点进行（连续的
// 可折叠空白在行断行阶段合并成一个空格），CJK 逐字成原子以便任意断行。
void LayoutEngine::AppendTextAtoms(const TextNode& node, const ComputedStyle& style,
                                   const std::vector<InlineFrame>& frames,
                                   std::vector<InlineAtom>* atoms) {
    AppendTextAtomsFromString(node.Data(), &node, style, frames, atoms);
}

void LayoutEngine::AppendTextAtomsFromString(const std::string& data, const TextNode* node,
                                             const ComputedStyle& style,
                                             const std::vector<InlineFrame>& frames,
                                             std::vector<InlineAtom>* atoms) {
    if (data.empty()) {
        return;
    }

    const WhiteSpace mode = style.white_space;
    const bool preserve_spaces = mode == WhiteSpace::Pre || mode == WhiteSpace::PreWrap;
    const bool preserve_newlines = preserve_spaces || mode == WhiteSpace::PreLine;
    // pre 只在换行处断行；pre-wrap 与 pre-line 允许自动换行；nowrap 禁止自动断行。
    const bool allow_wrap =
        mode != WhiteSpace::NoWrap && mode != WhiteSpace::Pre;

    const FontId font = FontFor(style);
    const FontMetrics& metrics = MetricsFor(font, style.font_size);
    const float line_height = ResolveLineHeight(style, metrics);
    const float leading = line_height - (metrics.ascent + metrics.descent);
    const float ascent = std::max(metrics.ascent + leading * 0.5f, 0.0f);
    const float descent = std::max(metrics.descent + leading * 0.5f, 0.0f);
    const float letter_spacing = style.letter_spacing;

    auto create_atom = [&](size_t begin, size_t end, bool is_space, bool collapsible) {
        InlineAtom atom;
        atom.kind = InlineAtom::Kind::Text;
        atom.frames = frames;
        atom.node = node;
        atom.offset = begin;
        atom.text = is_space ? std::string(" ") : data.substr(begin, end - begin);
        atom.font = font;
        atom.width = TextWidth(font, atom.text, letter_spacing);
        atom.ascent = ascent;
        atom.descent = descent;
        atom.line_height = line_height;
        atom.letter_spacing = letter_spacing;
        atom.color = style.color;
        // 渐变文字：文字颜色为透明时改用渐变的实色，保证文字可见。
        if (style.background_clip_text && !atom.color.Visible() &&
            !style.background_image.stops.empty()) {
            for (const GradientStop& stop : style.background_image.stops) {
                if (stop.color.a >= 0.5f) {
                    atom.color = stop.color;
                    break;
                }
            }
        }
        atom.underline = style.underline;
        // 文字阴影需要样式副本；仅在存在阴影时登记，避免为每个片段复制整份样式。
        atom.style = style.text_shadows.empty() ? nullptr : &style;
        atom.space = is_space;
        atom.collapsible_space = is_space && collapsible;
        atom.allow_wrap = allow_wrap;
        atom.break_all = style.word_break == WordBreak::BreakAll && !is_space;
        atom.break_anywhere = style.overflow_wrap_anywhere && !is_space;
        // keep-all 禁止在 CJK 文本内部自动断行。
        if (style.word_break == WordBreak::KeepAll && !is_space) {
            const Utf8Char first_character = DecodeUtf8(atom.text, 0);
            if (first_character.length != 0 && IsCjkCodepoint(first_character.codepoint)) {
                atom.allow_wrap = false;
            }
        }
        atoms->push_back(std::move(atom));
    };

    size_t offset = 0;
    size_t word_begin = std::string::npos;
    size_t word_end = 0;

    auto flush_word = [&]() {
        if (word_begin == std::string::npos) {
            return;
        }
        if (style.word_break == WordBreak::BreakAll) {
            // break-all：单词内每个码位各自成原子，任意位置都能断行。
            size_t cursor = word_begin;
            while (cursor < word_end) {
                const Utf8Char character = DecodeUtf8(data, cursor);
                if (character.length == 0) {
                    break;
                }
                create_atom(cursor, cursor + character.length, false, false);
                cursor += character.length;
            }
        } else {
            create_atom(word_begin, word_end, false, false);
        }
        word_begin = std::string::npos;
    };

    while (offset < data.size()) {
        const Utf8Char character = DecodeUtf8(data, offset);
        if (character.length == 0) {
            break;
        }

        const bool ascii_space = character.length == 1 && IsAsciiSpace(data[offset]);
        if (ascii_space) {
            flush_word();
            const char raw = data[offset];
            if (preserve_newlines && (raw == '\n' || raw == '\r')) {
                // CRLF 只产生一个换行。
                if (raw == '\r' && offset + 1 < data.size() && data[offset + 1] == '\n') {
                    ++offset;
                    continue;
                }
                AppendBreakAtom(style, frames, atoms);
                offset += character.length;
                continue;
            }
            if (preserve_spaces) {
                create_atom(offset, offset + character.length, true, false);
                offset += character.length;
                continue;
            }

            // 折叠连续空白为单个空格；保留换行的模式在换行处停止折叠。
            size_t run_end = offset + character.length;
            while (run_end < data.size()) {
                const char next = data[run_end];
                if (!IsAsciiSpace(next)) {
                    break;
                }
                if (preserve_newlines && (next == '\n' || next == '\r')) {
                    break;
                }
                ++run_end;
            }
            create_atom(offset, run_end, true, true);
            offset = run_end;
            continue;
        }

        if (IsCjkCodepoint(character.codepoint)) {
            flush_word();
            create_atom(offset, offset + character.length, false, false);
            offset += character.length;
            continue;
        }

        if (word_begin == std::string::npos) {
            word_begin = offset;
        }
        word_end = offset + character.length;
        offset += character.length;
        // 连字符之后允许断行。
        if (character.codepoint == '-' || character.codepoint == 0x2010) {
            flush_word();
        }
    }
    flush_word();
}

void LayoutEngine::AppendBreakAtom(const ComputedStyle& style,
                                   const std::vector<InlineFrame>& frames,
                                   std::vector<InlineAtom>* atoms) {
    const FontId font = FontFor(style);
    const FontMetrics& metrics = MetricsFor(font, style.font_size);
    const float line_height = ResolveLineHeight(style, metrics);
    const float leading = line_height - (metrics.ascent + metrics.descent);

    InlineAtom atom;
    atom.kind = InlineAtom::Kind::Break;
    atom.frames = frames;
    atom.font = font;
    atom.line_height = line_height;
    atom.ascent = std::max(metrics.ascent + leading * 0.5f, 0.0f);
    atom.descent = std::max(metrics.descent + leading * 0.5f, 0.0f);
    atoms->push_back(std::move(atom));
}

// <img>、<canvas> 与 <iframe> 作为原子盒放入行内流；尺寸优先取 CSS，其次取
// width/height 属性，最后使用缺省占位尺寸（布局层不读取图片文件、画布像素
// 与子文档内容）。
void LayoutEngine::AppendImageAtom(const Element& element, const ComputedStyle& style,
                                   const std::vector<InlineFrame>& frames, float available_width,
                                   LayoutBox* owner, std::vector<InlineAtom>* atoms) {
    if (owner == nullptr) {
        return;
    }

    const EdgeValues edges = ResolveEdges(style, available_width, root_font_size_);
    const bool is_canvas = element.TagName() == "canvas";
    const bool is_iframe = element.TagName() == "iframe";
    const bool is_svg = element.TagName() == "svg";

    float content_width = 0.0f;
    if (!style.width.IsAuto()) {
        content_width = ResolveLength(style.width, style.font_size, root_font_size_,
                                      available_width, options_.viewport_width, ViewportHeight());
    } else {
        content_width = ParseHtmlDimension(element.GetAttribute("width"), available_width);
        if (content_width < 0.0f) {
            content_width = 300.0f;
        }
    }
    content_width = ClampNonNegative(content_width);

    float content_height = 0.0f;
    if (!style.height.IsAuto()) {
        content_height = ResolveLength(style.height, style.font_size, root_font_size_,
                                       available_width, options_.viewport_width, ViewportHeight());
    } else {
        content_height = ParseHtmlDimension(element.GetAttribute("height"), available_width);
        if (content_height < 0.0f) {
            content_height = is_canvas || is_iframe || is_svg ? 150.0f : 200.0f;
        }
    }
    content_height = ClampNonNegative(content_height);

    auto box = std::make_unique<LayoutBox>();
    box->kind = BoxKind::Image;
    box->atomic = true;
    box->element = &element;
    box->style = style;
    box->padding_top = edges.padding_top;
    box->padding_right = edges.padding_right;
    box->padding_bottom = edges.padding_bottom;
    box->padding_left = edges.padding_left;
    box->border_top = edges.border_top;
    box->border_right = edges.border_right;
    box->border_bottom = edges.border_bottom;
    box->border_left = edges.border_left;

    const float border_width = ClampNonNegative(
        content_width + edges.padding_left + edges.padding_right + edges.border_left +
        edges.border_right);
    const float border_height = ClampNonNegative(
        content_height + edges.padding_top + edges.padding_bottom + edges.border_top +
        edges.border_bottom);

    // 临时几何，行内放置时重设。
    box->border_box = RectF::FromXYWH(0.0f, 0.0f, border_width, border_height);
    box->content_box = RectF::FromLTRB(
        edges.border_left + edges.padding_left, edges.border_top + edges.padding_top,
        edges.border_left + edges.padding_left + content_width,
        edges.border_top + edges.padding_top + content_height);
    LayoutBox* box_pointer = box.get();
    owner->children.push_back(std::move(box));

    InlineAtom atom;
    atom.kind = InlineAtom::Kind::Atomic;
    atom.frames = frames;
    atom.atomic_element = &element;
    atom.atomic_box = box_pointer;
    atom.atomic_margin_left = edges.margin_left;
    atom.atomic_margin_right = edges.margin_right;
    atom.atomic_margin_top = edges.margin_top;
    atom.atomic_margin_bottom = edges.margin_bottom;
    atom.atomic_border_width = border_width;
    atom.atomic_border_height = border_height;
    atom.atomic_outer_width = ClampNonNegative(
        edges.margin_left + border_width + edges.margin_right);
    atom.width = atom.atomic_outer_width;
    // 替换元素的基线与下外边距边对齐。
    atom.atomic_baseline_offset = border_height + edges.margin_bottom;
    atom.ascent = std::max(edges.margin_top + atom.atomic_baseline_offset, 0.0f);
    atom.descent = 0.0f;
    atom.allow_wrap = true;
    atoms->push_back(std::move(atom));
}

// inline-block 按内部布局计算尺寸；宽度为 auto 时先测量行内内容的最大宽度，
// 再限制在可用宽度内（收缩适应）。
void LayoutEngine::AppendInlineBlockAtom(const Element& element, const ComputedStyle& style,
                                         const std::vector<InlineFrame>& frames,
                                         float available_width, LayoutBox* owner,
                                         std::vector<InlineAtom>* atoms) {
    if (owner == nullptr) {
        return;
    }

    const EdgeValues edges = ResolveEdges(style, available_width, root_font_size_);

    float width_override = -1.0f;
    if (style.width.IsAuto()) {
        const float intrinsic =
            MeasureInlineIntrinsicWidth(element, style, available_width);
        const float maximum = available_width - edges.margin_left - edges.margin_right -
                              edges.border_left - edges.border_right - edges.padding_left -
                              edges.padding_right;
        width_override = ClampNonNegative(std::min(intrinsic, ClampNonNegative(maximum)));
    }

    std::unique_ptr<LayoutBox> box =
        LayoutBlock(&element, style, 0.0f, 0.0f, available_width, root_font_size_, 0,
                    width_override);
    box->kind = BoxKind::InlineBlock;
    box->atomic = true;
    const float border_width = box->border_box.Width();
    const float border_height = box->border_box.Height();

    float baseline_offset = border_height + edges.margin_bottom;
    if (!box->lines.empty()) {
        baseline_offset = box->lines.back().baseline - box->border_box.top;
    }

    LayoutBox* box_pointer = box.get();
    owner->children.push_back(std::move(box));

    InlineAtom atom;
    atom.kind = InlineAtom::Kind::Atomic;
    atom.frames = frames;
    atom.atomic_element = &element;
    atom.atomic_box = box_pointer;
    atom.atomic_margin_left = edges.margin_left;
    atom.atomic_margin_right = edges.margin_right;
    atom.atomic_margin_top = edges.margin_top;
    atom.atomic_margin_bottom = edges.margin_bottom;
    atom.atomic_border_width = border_width;
    atom.atomic_border_height = border_height;
    atom.atomic_outer_width = ClampNonNegative(
        edges.margin_left + border_width + edges.margin_right);
    atom.width = atom.atomic_outer_width;
    atom.atomic_baseline_offset = ClampNonNegative(baseline_offset);
    atom.ascent = std::max(edges.margin_top + atom.atomic_baseline_offset, 0.0f);
    atom.descent = std::max(border_height + edges.margin_bottom - atom.atomic_baseline_offset,
                            0.0f);
    atom.allow_wrap = true;
    atoms->push_back(std::move(atom));
}

// input 与 select 的原子盒。内容尺寸优先取 CSS width/height，其次取同名的
// HTML 属性，最后按控件类型取默认值：checkbox/radio 为 14x14，文本输入为
// 160px 宽，其余按标签文本宽度加下拉箭头预留。文本类控件的标签在盒内建立
// 行盒并在内容区内垂直居中，checkbox 的勾选标记也通过行盒绘制。
void LayoutEngine::AppendFormControlAtom(const Element& element, const ComputedStyle& style,
                                         const std::vector<InlineFrame>& frames,
                                         float available_width, LayoutBox* owner,
                                         std::vector<InlineAtom>* atoms) {
    if (owner == nullptr) {
        return;
    }

    const EdgeValues edges = ResolveEdges(style, available_width, root_font_size_);
    const bool from_select = element.TagName() == "select";

    FormControlKind kind = FormControlKind::None;
    std::string label;
    if (from_select) {
        kind = FormControlKind::Select;
        label = SelectControlLabel(element);
    } else {
        std::string type_name = element.GetAttribute("type");
        TrimAsciiSpace(&type_name);
        type_name = AsciiLower(type_name);
        if (type_name == "password") {
            kind = FormControlKind::PasswordInput;
            label = MaskPasswordText(element.GetAttribute("value"));
        } else if (type_name == "checkbox") {
            kind = FormControlKind::Checkbox;
        } else if (type_name == "radio") {
            kind = FormControlKind::Radio;
        } else if (type_name == "range" || type_name == "color") {
            // 滑块与颜色选择器由绘制层按原生外观绘制，不显示文本标签。
            kind = FormControlKind::TextInput;
            label.clear();
        } else if (type_name == "submit" || type_name == "reset" || type_name == "button") {
            kind = type_name == "submit" ? FormControlKind::Submit
                                         : (type_name == "reset" ? FormControlKind::Reset
                                                                 : FormControlKind::Button);
            label = element.GetAttribute("value");
            if (label.empty()) {
                label = kind == FormControlKind::Submit
                            ? std::string("Submit")
                            : (kind == FormControlKind::Reset ? std::string("Reset")
                                                              : std::string("Button"));
            }
        } else {
            // 未指定或无法识别的类型按单行文本框处理。
            kind = FormControlKind::TextInput;
            label = element.GetAttribute("value");
        }
    }

    const bool is_box_control =
        kind == FormControlKind::Checkbox || kind == FormControlKind::Radio;
    const bool is_button_like = kind == FormControlKind::Submit ||
                                kind == FormControlKind::Reset ||
                                kind == FormControlKind::Button;
    const bool disabled = element.HasAttribute("disabled");
    const bool checked = element.HasAttribute("checked");

    const FontId plain_font = FontFor(style);
    const FontMetrics& plain_metrics = MetricsFor(plain_font, style.font_size);
    const float plain_line_height = ResolveLineHeight(style, plain_metrics);

    // 内容宽度：CSS 优先，其次 HTML width 属性，最后按控件类型取默认值。
    float content_width = 0.0f;
    if (!style.width.IsAuto()) {
        content_width = ResolveLength(style.width, style.font_size, root_font_size_,
                                      available_width, options_.viewport_width,
                                      ViewportHeight());
    } else {
        const float attribute_width =
            ParseHtmlDimension(element.GetAttribute("width"), available_width);
        if (attribute_width >= 0.0f) {
            content_width = attribute_width;
        } else if (is_box_control) {
            content_width = 14.0f;
        } else if (kind == FormControlKind::TextInput ||
                   kind == FormControlKind::PasswordInput) {
            content_width = 160.0f;
        } else {
            const float text_width =
                label.empty() ? 0.0f : TextWidth(plain_font, label, style.letter_spacing);
            // 下拉框在右侧预留 16px 绘制向下箭头。
            content_width = from_select ? text_width + 16.0f : text_width;
        }
    }
    content_width = ClampNonNegative(content_width);

    float content_height = 0.0f;
    if (!style.height.IsAuto()) {
        content_height = ResolveLength(style.height, style.font_size, root_font_size_,
                                       available_width, options_.viewport_width,
                                       ViewportHeight());
    } else {
        const float attribute_height =
            ParseHtmlDimension(element.GetAttribute("height"), available_width);
        if (attribute_height >= 0.0f) {
            content_height = attribute_height;
        } else if (is_box_control) {
            content_height = 14.0f;
        } else {
            content_height = plain_line_height;
        }
    }
    content_height = ClampNonNegative(content_height);

    // 标签样式：控件内文字单行显示，按钮类标签水平居中。
    ComputedStyle label_style = style;
    label_style.white_space = WhiteSpace::NoWrap;
    if (is_button_like) {
        label_style.text_align = TextAlign::Center;
    }
    if (disabled) {
        // 禁用控件只把文字改为 50% 灰度，背景颜色保持样式值。
        label_style.color = Rgba(0.5f, 0.5f, 0.5f, 1.0f);
    }

    // 勾选标记：方框以白或灰的字形绘制，字号按内容高度换算。
    ComputedStyle marker_style = style;
    marker_style.white_space = WhiteSpace::NoWrap;
    marker_style.text_align = TextAlign::Center;
    marker_style.color = White();
    marker_style.font_size = std::max(9.0f, std::min(content_height * 0.8f, 18.0f));
    marker_style.line_height = CssLength{content_height, LengthUnit::Px};

    // 行盒在内容区内垂直居中。
    float control_line_height = plain_line_height;
    if (is_box_control) {
        control_line_height = content_height;
    }
    const float line_top_offset =
        ClampNonNegative((content_height - control_line_height) * 0.5f);

    auto box = std::make_unique<LayoutBox>();
    box->kind = BoxKind::InlineBlock;
    box->atomic = true;
    box->element = &element;
    box->style = style;
    box->form_kind = kind;
    box->form_checked = is_box_control && checked;
    box->padding_top = edges.padding_top;
    box->padding_right = edges.padding_right;
    box->padding_bottom = edges.padding_bottom;
    box->padding_left = edges.padding_left;
    box->border_top = edges.border_top;
    box->border_right = edges.border_right;
    box->border_bottom = edges.border_bottom;
    box->border_left = edges.border_left;

    const float border_width = ClampNonNegative(
        content_width + edges.padding_left + edges.padding_right + edges.border_left +
        edges.border_right);
    const float border_height = ClampNonNegative(
        content_height + edges.padding_top + edges.padding_bottom + edges.border_top +
        edges.border_bottom);
    const float content_left = edges.border_left + edges.padding_left;
    const float content_top = edges.border_top + edges.padding_top;

    box->border_box = RectF::FromXYWH(0.0f, 0.0f, border_width, border_height);
    box->content_box = RectF::FromLTRB(content_left, content_top, content_left + content_width,
                                       content_top + content_height);

    std::vector<InlineAtom> control_atoms;
    const std::vector<InlineFrame> no_frames;
    if (kind == FormControlKind::Checkbox && checked) {
        AppendTextAtomsFromString("\xE2\x9C\x93", nullptr, marker_style, no_frames,
                                  &control_atoms);
    } else if (!is_box_control && !label.empty()) {
        AppendTextAtomsFromString(label, nullptr, label_style, no_frames, &control_atoms);
    }
    if (!control_atoms.empty()) {
        const ComputedStyle& line_style =
            kind == FormControlKind::Checkbox ? marker_style : label_style;
        LayoutLines(control_atoms, content_width, line_style, content_left,
                    content_top + line_top_offset, box.get());
    }

    float baseline_offset = border_height + edges.margin_bottom;
    if (!box->lines.empty()) {
        baseline_offset = box->lines.front().baseline - box->border_box.top;
    }

    LayoutBox* box_pointer = box.get();
    owner->children.push_back(std::move(box));

    InlineAtom atom;
    atom.kind = InlineAtom::Kind::Atomic;
    atom.frames = frames;
    atom.atomic_element = &element;
    atom.atomic_box = box_pointer;
    atom.atomic_margin_left = edges.margin_left;
    atom.atomic_margin_right = edges.margin_right;
    atom.atomic_margin_top = edges.margin_top;
    atom.atomic_margin_bottom = edges.margin_bottom;
    atom.atomic_border_width = border_width;
    atom.atomic_border_height = border_height;
    atom.atomic_outer_width =
        ClampNonNegative(edges.margin_left + border_width + edges.margin_right);
    atom.width = atom.atomic_outer_width;
    atom.atomic_baseline_offset = ClampNonNegative(baseline_offset);
    atom.ascent = std::max(edges.margin_top + atom.atomic_baseline_offset, 0.0f);
    atom.descent =
        std::max(border_height + edges.margin_bottom - atom.atomic_baseline_offset, 0.0f);
    atom.allow_wrap = true;
    atoms->push_back(std::move(atom));
}

// 测量元素行内内容在不换行时的总宽度，用于 inline-block 的收缩适应。
float LayoutEngine::MeasureInlineIntrinsicWidth(const Element& element,
                                                const ComputedStyle& style,
                                                float available_width) {
    const float measure_width = std::max(available_width, 1000000.0f);
    LayoutBox scratch;
    std::vector<InlineAtom> atoms;
    const std::vector<InlineFrame> no_frames;
    for (const NodePtr& child : element.Children()) {
        if (!child) {
            continue;
        }
        if (child->IsText()) {
            const TextNode& text_node = static_cast<const TextNode&>(*child);
            AppendTextAtoms(text_node, style, no_frames, &atoms);
            continue;
        }
        if (!child->IsElement()) {
            continue;
        }
        const Element& child_element = static_cast<const Element&>(*child);
        const ComputedStyle child_style = resolver_.ComputeFor(child_element, style);
        if (child_style.display == Display::None) {
            continue;
        }
        // 块级子元素在收缩适应中按可用宽度排版，不参与行内内容的固有宽度累加。
        if (child_style.IsBlockLevel()) {
            continue;
        }
        BuildInlineNode(child_element, child_style, no_frames, measure_width, &scratch, &atoms);
    }

    float total = 0.0f;
    for (const InlineAtom& atom : atoms) {
        if (atom.kind == InlineAtom::Kind::Text) {
            total += atom.width;
        } else if (atom.kind == InlineAtom::Kind::Atomic) {
            total += atom.atomic_outer_width;
        }
    }
    return total;
}

// 表格单元格固有宽度：行内内容沿用 MeasureInlineIntrinsicWidth 的测量结果；
// 块级子元素（p、div 等）不参与行内累加，改为递归统计其自身内容的最大固有
// 宽度，并计入该子元素的水平外边距、内边距与边框。返回单元格内容区所需的
// 最小宽度，单元格自身的边缘由调用方累加。
float LayoutEngine::MeasureTableCellIntrinsicWidth(const Element& element,
                                                   const ComputedStyle& style,
                                                   float available_width) {
    // 弹性盒容器：主轴为行时各项目固有宽度相加并计入间距，列方向取最大值。
    if (style.IsFlexContainer()) {
        const bool row_axis = style.flex_direction == FlexDirection::Row ||
                              style.flex_direction == FlexDirection::RowReverse;
        const float gap = ResolveLength(row_axis ? style.column_gap : style.row_gap,
                                        style.font_size, root_font_size_, available_width,
                                        options_.viewport_width, ViewportHeight());
        const std::vector<InlineFrame> no_frames;
        float total = 0.0f;
        float maximum = 0.0f;
        size_t count = 0;
        for (const NodePtr& child : element.Children()) {
            if (!child) {
                continue;
            }
            if (child->IsText()) {
                const TextNode& text_node = static_cast<const TextNode&>(*child);
                std::vector<InlineAtom> atoms;
                AppendTextAtoms(text_node, style, no_frames, &atoms);
                float text_width = 0.0f;
                for (const InlineAtom& atom : atoms) {
                    if (atom.kind == InlineAtom::Kind::Text) {
                        text_width += atom.width;
                    }
                }
                if (text_width > 0.0f) {
                    total += text_width;
                    maximum = std::max(maximum, text_width);
                    count += 1;
                }
                continue;
            }
            if (!child->IsElement()) {
                continue;
            }
            const Element& child_element = static_cast<const Element&>(*child);
            const ComputedStyle child_style = resolver_.ComputeFor(child_element, style);
            if (child_style.display == Display::None) {
                continue;
            }
            const float content_intrinsic =
                MeasureTableCellIntrinsicWidth(child_element, child_style, available_width);
            const EdgeValues child_edges =
                ResolveEdges(child_style, available_width, root_font_size_);
            const float child_intrinsic =
                content_intrinsic + child_edges.margin_left + child_edges.margin_right +
                child_edges.padding_left + child_edges.padding_right + child_edges.border_left +
                child_edges.border_right;
            total += child_intrinsic;
            maximum = std::max(maximum, child_intrinsic);
            count += 1;
        }
        if (row_axis) {
            if (count > 1) {
                total += gap * static_cast<float>(count - 1);
            }
            return total;
        }
        return maximum;
    }
    float maximum = MeasureInlineIntrinsicWidth(element, style, available_width);
    for (const NodePtr& child : element.Children()) {
        if (!child || !child->IsElement()) {
            continue;
        }
        const Element& child_element = static_cast<const Element&>(*child);
        const ComputedStyle child_style = resolver_.ComputeFor(child_element, style);
        if (child_style.display == Display::None || !child_style.IsBlockLevel()) {
            continue;
        }
        const float content_intrinsic =
            MeasureTableCellIntrinsicWidth(child_element, child_style, available_width);
        const EdgeValues child_edges = ResolveEdges(child_style, available_width, root_font_size_);
        const float child_intrinsic =
            content_intrinsic + child_edges.margin_left + child_edges.margin_right +
            child_edges.padding_left + child_edges.padding_right + child_edges.border_left +
            child_edges.border_right;
        maximum = std::max(maximum, child_intrinsic);
    }
    return maximum;
}

// ---------------------------------------------------------------------------
// 行排版
// ---------------------------------------------------------------------------

uint32_t FirstCodepoint(const std::string& text) {
    if (text.empty()) {
        return 0;
    }
    return DecodeUtf8(text, 0).codepoint;
}

uint32_t LastCodepoint(const std::string& text) {
    uint32_t last = 0;
    size_t offset = 0;
    while (offset < text.size()) {
        const Utf8Char character = DecodeUtf8(text, offset);
        if (character.length == 0) {
            break;
        }
        last = character.codepoint;
        offset += character.length;
    }
    return last;
}

// 整体平移布局子树。行内块先在原点完成内部布局，放置到行内时统一平移。
void TranslateBox(LayoutBox* box, float dx, float dy) {
    if (box == nullptr) {
        return;
    }
    if (dx == 0.0f && dy == 0.0f) {
        return;
    }
    box->border_box = box->border_box.Offset(dx, dy);
    box->content_box = box->content_box.Offset(dx, dy);

    for (LineFragment& line : box->lines) {
        line.rect = line.rect.Offset(dx, dy);
        line.baseline += dy;
        for (PlacedText& placed : line.texts) {
            placed.x += dx;
            placed.baseline += dy;
        }
        for (PlacedInline& inline_box : line.inlines) {
            inline_box.rect = inline_box.rect.Offset(dx, dy);
        }
        for (PlacedAtomic& atomic_box : line.atomics) {
            atomic_box.border_box = atomic_box.border_box.Offset(dx, dy);
        }
    }

    for (const std::unique_ptr<LayoutBox>& child : box->children) {
        TranslateBox(child.get(), dx, dy);
    }
}

/* 缩放布局子树：盒矩形、行盒与文字位置都按同一个原点缩放，使 transform:
   scale() 在视觉上生效。文字宽度按水平缩放系数近似，字号本身不变。 */
void ScaleBox(LayoutBox* box, float center_x, float center_y, float scale_x, float scale_y) {
    if (box == nullptr || (scale_x == 1.0f && scale_y == 1.0f)) {
        return;
    }
    const auto scale_x_value = [&](float value) {
        return center_x + (value - center_x) * scale_x;
    };
    const auto scale_y_value = [&](float value) {
        return center_y + (value - center_y) * scale_y;
    };

    box->border_box = RectF::FromLTRB(scale_x_value(box->border_box.left),
                                      scale_y_value(box->border_box.top),
                                      scale_x_value(box->border_box.right),
                                      scale_y_value(box->border_box.bottom));
    box->content_box = RectF::FromLTRB(scale_x_value(box->content_box.left),
                                       scale_y_value(box->content_box.top),
                                       scale_x_value(box->content_box.right),
                                       scale_y_value(box->content_box.bottom));

    for (LineFragment& line : box->lines) {
        line.rect = RectF::FromLTRB(scale_x_value(line.rect.left), scale_y_value(line.rect.top),
                                    scale_x_value(line.rect.right),
                                    scale_y_value(line.rect.bottom));
        line.baseline = scale_y_value(line.baseline);
        for (PlacedText& placed : line.texts) {
            placed.x = scale_x_value(placed.x);
            placed.baseline = scale_y_value(placed.baseline);
            placed.width = std::max(0.0f, placed.width * scale_x);
        }
        for (PlacedInline& inline_box : line.inlines) {
            inline_box.rect = RectF::FromLTRB(
                scale_x_value(inline_box.rect.left), scale_y_value(inline_box.rect.top),
                scale_x_value(inline_box.rect.right), scale_y_value(inline_box.rect.bottom));
        }
        for (PlacedAtomic& atomic_box : line.atomics) {
            atomic_box.border_box = RectF::FromLTRB(
                scale_x_value(atomic_box.border_box.left),
                scale_y_value(atomic_box.border_box.top),
                scale_x_value(atomic_box.border_box.right),
                scale_y_value(atomic_box.border_box.bottom));
        }
    }

    for (const std::unique_ptr<LayoutBox>& child : box->children) {
        ScaleBox(child.get(), center_x, center_y, scale_x, scale_y);
    }
}

void AddInlineFrames(LineFragment* line, const std::vector<InlineFrame>& frames, float left,
                     float right, float top, float bottom) {
    for (const InlineFrame& frame : frames) {
        if (frame.element == nullptr) {
            continue;
        }
        bool merged = false;
        for (PlacedInline& existing : line->inlines) {
            if (existing.element == frame.element) {
                existing.rect.left = std::min(existing.rect.left, left);
                existing.rect.right = std::max(existing.rect.right, right);
                existing.rect.top = std::min(existing.rect.top, top);
                existing.rect.bottom = std::max(existing.rect.bottom, bottom);
                merged = true;
                break;
            }
        }
        if (merged) {
            continue;
        }
        PlacedInline placed;
        placed.element = frame.element;
        placed.style = frame.style;
        placed.rect = RectF::FromLTRB(left, top, right, bottom);
        line->inlines.push_back(placed);
    }
}

// 按可用宽度把行内原子组合成行：英文按空格与连字符断行，CJK 逐字断行；
// 实现简化避头尾规则，放不下时允许标点悬挂。
void LayoutEngine::BuildLinePlans(const std::vector<InlineAtom>& atoms, float content_width,
                                  float first_line_indent, std::vector<LinePlan>* plans) {
    LinePlan current;
    float line_width = 0.0f;
    size_t pending_space = std::string::npos;
    bool first_line = true;

    // 首行可用宽度扣除缩进；负缩进（悬挂缩进）使首行可以更宽。
    const float first_line_limit = content_width - first_line_indent;

    auto append_atom = [&](size_t index) {
        current.atom_indices.push_back(index);
        line_width += atoms[index].width;
    };

    auto flush_line = [&](bool ended_by_break, const InlineAtom* break_atom) {
        if (current.atom_indices.empty() && !ended_by_break) {
            return;
        }
        LinePlan plan;
        plan.atom_indices = std::move(current.atom_indices);
        plan.width = line_width;
        plan.ended_by_break = ended_by_break;
        if (plan.atom_indices.empty() && break_atom != nullptr) {
            plan.empty_line = true;
            plan.empty_ascent = break_atom->ascent;
            plan.empty_descent = break_atom->descent;
        }
        plans->push_back(std::move(plan));
        current = LinePlan();
        line_width = 0.0f;
        pending_space = std::string::npos;
        first_line = false;
    };

    const auto current_limit = [&]() {
        return first_line ? first_line_limit : content_width;
    };

    for (size_t index = 0; index < atoms.size(); ++index) {
        const InlineAtom& atom = atoms[index];

        if (atom.kind == InlineAtom::Kind::Break) {
            flush_line(true, &atom);
            continue;
        }

        if (atom.kind == InlineAtom::Kind::Text && atom.space) {
            if (atom.collapsible_space) {
                if (current.atom_indices.empty()) {
                    // 行首的可折叠空白丢弃。
                    continue;
                }
                pending_space = index;
                continue;
            }
            // 保留空白：直接计入当前行；放不下时在空白前断行（空白保留到下一行）。
            if (!current.atom_indices.empty() && atom.allow_wrap &&
                line_width + atom.width > current_limit() + kEpsilon) {
                flush_line(false, nullptr);
            }
            append_atom(index);
            continue;
        }

        const size_t space_index = pending_space;
        const float space_width =
            space_index != std::string::npos ? atoms[space_index].width : 0.0f;
        const bool can_break = !current.atom_indices.empty() && atom.allow_wrap;
        const bool overflowing =
            line_width + space_width + atom.width > current_limit() + kEpsilon;

        if (overflowing && can_break) {
            if (IsHangingPunctuation(FirstCodepoint(atom.text))) {
                // 避头标点不允许出现在行首：允许悬挂在行尾，行宽可略微超出。
                append_atom(index);
                pending_space = std::string::npos;
                continue;
            }

            // 避尾标点不允许留在行尾：把行尾这类原子移到下一行。
            std::vector<size_t> carry;
            while (current.atom_indices.size() > 1) {
                const size_t last_index = current.atom_indices.back();
                const InlineAtom& last_atom = atoms[last_index];
                if (last_atom.kind != InlineAtom::Kind::Text || last_atom.space) {
                    break;
                }
                if (!IsLeadingPunctuation(LastCodepoint(last_atom.text))) {
                    break;
                }
                carry.insert(carry.begin(), last_index);
                current.atom_indices.pop_back();
                line_width -= last_atom.width;
            }

            flush_line(false, nullptr);
            for (size_t carry_index : carry) {
                append_atom(carry_index);
            }
            append_atom(index);
            pending_space = std::string::npos;
            continue;
        }

        if (space_index != std::string::npos) {
            append_atom(space_index);
        }
        pending_space = std::string::npos;
        append_atom(index);
    }
    flush_line(false, nullptr);
}

// 生成行盒与文字片段，返回行内容总高度。
float LayoutEngine::LayoutLines(const std::vector<InlineAtom>& atoms, float content_width,
                                const ComputedStyle& block_style, float content_left,
                                float content_top, LayoutBox* owner) {
    if (owner == nullptr) {
        return 0.0f;
    }

    /* overflow-wrap: break-word/anywhere 的超长单词：在排版前按码位拆成
       可独立断行的原子，避免单个单词整行溢出。 */
    std::vector<InlineAtom> expanded;
    const std::vector<InlineAtom>* source_atoms = &atoms;
    bool needs_split = false;
    for (const InlineAtom& atom : atoms) {
        if (atom.kind == InlineAtom::Kind::Text && !atom.space && atom.break_anywhere &&
            content_width > 0.0f && atom.width > content_width) {
            needs_split = true;
            break;
        }
    }
    if (needs_split) {
        expanded.reserve(atoms.size() * 2);
        for (const InlineAtom& atom : atoms) {
            if (atom.kind != InlineAtom::Kind::Text || atom.space || !atom.break_anywhere ||
                content_width <= 0.0f || atom.width <= content_width) {
                expanded.push_back(atom);
                continue;
            }
            size_t byte_offset = 0;
            while (byte_offset < atom.text.size()) {
                const Utf8Char character = DecodeUtf8(atom.text, byte_offset);
                if (character.length == 0) {
                    break;
                }
                InlineAtom piece = atom;
                piece.text = atom.text.substr(byte_offset, character.length);
                piece.offset = atom.offset + byte_offset;
                piece.width = Advance(atom.font, character.codepoint) + atom.letter_spacing;
                piece.allow_wrap = true;
                expanded.push_back(std::move(piece));
                byte_offset += character.length;
            }
        }
        source_atoms = &expanded;
    }

    // 首行缩进按内容宽度解析百分比。
    float indent = 0.0f;
    if (!block_style.text_indent.IsAuto()) {
        indent = ResolveLength(block_style.text_indent, block_style.font_size, root_font_size_,
                               content_width, options_.viewport_width, ViewportHeight());
    }

    std::vector<LinePlan> plans;
    BuildLinePlans(*source_atoms, content_width, indent, &plans);
    const std::vector<InlineAtom>& line_atoms = *source_atoms;

    float y = content_top;
    for (size_t line_index = 0; line_index < plans.size(); ++line_index) {
        const LinePlan& plan = plans[line_index];

        float ascent = 0.0f;
        float descent = 0.0f;
        if (plan.empty_line) {
            ascent = plan.empty_ascent;
            descent = plan.empty_descent;
        }
        for (size_t atom_index : plan.atom_indices) {
            const InlineAtom& atom = line_atoms[atom_index];
            ascent = std::max(ascent, atom.ascent);
            descent = std::max(descent, atom.descent);
        }
        const float line_height = ascent + descent;
        const float line_top = y;
        const float baseline = line_top + ascent;

        // 首行可用宽度扣除缩进；对齐与溢出判断都以该宽度为基准。
        const float line_indent = line_index == 0 ? indent : 0.0f;
        const float line_limit = content_width - line_indent;

        // 对齐：justify 只作用于非最后一行且不是由强制换行结束的行。
        const float extra = line_limit - plan.width;
        const bool justify = block_style.text_align == TextAlign::Justify && extra > 0.0f &&
                             !plan.ended_by_break && !plan.empty_line &&
                             line_index + 1 < plans.size() &&
                             plan.atom_indices.size() > 1;

        size_t space_count = 0;
        for (size_t atom_index : plan.atom_indices) {
            if (line_atoms[atom_index].space) {
                ++space_count;
            }
        }
        float space_bonus = 0.0f;
        float gap_bonus = 0.0f;
        if (justify) {
            if (space_count > 0) {
                space_bonus = extra / static_cast<float>(space_count);
            } else {
                gap_bonus = extra / static_cast<float>(plan.atom_indices.size() - 1);
            }
        }

        float offset = line_indent;
        if (!justify && extra > 0.0f) {
            if (block_style.text_align == TextAlign::Center) {
                offset += extra * 0.5f;
            } else if (block_style.text_align == TextAlign::Right ||
                       block_style.text_align == TextAlign::End) {
                offset += extra;
            }
        }

        LineFragment line;
        line.baseline = baseline;
        float x = content_left + offset;
        bool first_on_line = true;
        // 超出行宽且启用 text-overflow: ellipsis 时，末尾用省略号截断。
        const float right_edge = content_left + line_indent + line_limit;
        const bool ellipsize = block_style.overflow_hidden && block_style.text_overflow_ellipsis &&
                               plan.width > line_limit + 0.5f;
        bool truncated_line = false;
        const std::string ellipsis_text = "\xE2\x80\xA6";

        for (size_t atom_index : plan.atom_indices) {
            if (truncated_line) {
                break;
            }
            const InlineAtom& atom = line_atoms[atom_index];
            if (!first_on_line) {
                x += gap_bonus;
            }
            first_on_line = false;

            float atom_width = atom.width + (atom.space ? space_bonus : 0.0f);
            std::string text_override;
            bool override_text = false;
            if (ellipsize && x + atom_width > right_edge + kEpsilon) {
                truncated_line = true;
                if (atom.kind == InlineAtom::Kind::Text && !atom.space) {
                    // 保留能完整显示的前缀，再接省略号。
                    const float ellipsis_width =
                        TextWidth(atom.font, ellipsis_text, atom.letter_spacing);
                    const float available = right_edge - x;
                    std::string prefix;
                    float prefix_width = 0.0f;
                    size_t cursor = 0;
                    while (cursor < atom.text.size()) {
                        const Utf8Char character = DecodeUtf8(atom.text, cursor);
                        if (character.length == 0) {
                            break;
                        }
                        const float advance =
                            Advance(atom.font, character.codepoint) + atom.letter_spacing;
                        if (prefix_width + advance + ellipsis_width > available - kEpsilon) {
                            break;
                        }
                        prefix.append(atom.text, cursor, character.length);
                        prefix_width += advance;
                        cursor += character.length;
                    }
                    text_override = prefix + ellipsis_text;
                    override_text = true;
                    atom_width = TextWidth(atom.font, text_override, atom.letter_spacing);
                } else {
                    // 原子盒或空白放不下：直接丢弃该原子。
                    atom_width = 0.0f;
                }
            }

            if (atom.kind == InlineAtom::Kind::Text && atom_width > 0.0f) {
                PlacedText placed;
                placed.node = atom.node;
                placed.offset = atom.offset;
                placed.text = override_text ? text_override : atom.text;
                placed.font = atom.font;
                placed.x = x;
                placed.baseline = baseline;
                placed.width = atom_width;
                placed.letter_spacing = atom.letter_spacing;
                placed.color = atom.color;
                placed.underline = atom.underline;
                if (atom.style != nullptr) {
                    placed.shadow_style = StoreInlineStyle(*atom.style);
                }
                line.texts.push_back(std::move(placed));
                AddInlineFrames(&line, atom.frames, x, x + atom_width, line_top,
                                line_top + line_height);
            } else if (atom.kind == InlineAtom::Kind::Atomic && !truncated_line) {
                const float border_left = x + atom.atomic_margin_left;
                const float border_top = baseline - atom.atomic_baseline_offset;
                if (atom.atomic_box != nullptr) {
                    TranslateBox(atom.atomic_box, border_left, border_top);
                    PlacedAtomic placed;
                    placed.element = atom.atomic_element;
                    placed.style = &atom.atomic_box->style;
                    placed.box = atom.atomic_box;
                    placed.border_box = atom.atomic_box->border_box;
                    line.atomics.push_back(placed);
                }
                AddInlineFrames(&line, atom.frames, x,
                                x + atom.atomic_margin_left + atom.atomic_border_width +
                                    atom.atomic_margin_right,
                                line_top, line_top + line_height);
            }
            x += atom_width;
        }

        line.rect = RectF::FromLTRB(content_left, line_top, content_left + content_width,
                                    line_top + line_height);
        owner->lines.push_back(std::move(line));
        y = line_top + line_height;
    }

    return y - content_top;
}

// ---------------------------------------------------------------------------
// 分页
// ---------------------------------------------------------------------------

/* 判断块盒是否带可见装饰。带背景、边框、圆角或阴影的叶子块在分页时应整体保留，
   避免背景与文字被页边界拆到两页。 */
bool HasVisibleBlockDecoration(const LayoutBox& box) {
    if (box.HasBackground()) {
        return true;
    }
    if (box.border_top > 0.0f || box.border_right > 0.0f || box.border_bottom > 0.0f ||
        box.border_left > 0.0f) {
        return true;
    }
    if (!box.style.box_shadows.empty()) {
        return true;
    }
    const auto radius_visible = [](const CornerRadius& radius) {
        return radius.horizontal > 0.0f || radius.vertical > 0.0f;
    };
    return radius_visible(box.style.border_radius_top_left) ||
           radius_visible(box.style.border_radius_top_right) ||
           radius_visible(box.style.border_radius_bottom_right) ||
           radius_visible(box.style.border_radius_bottom_left);
}

void LayoutEngine::CollectBreakCandidates(const LayoutBox& box, float page_height,
                                          std::vector<float>* candidates) const {
    /* 绝对定位与固定定位的子盒脱离普通流，不影响块盒能否整体分页；
       只有普通流子盒才意味着块盒内部存在可切分的结构。 */
    bool has_flow_child = false;
    for (const std::unique_ptr<LayoutBox>& child : box.children) {
        if (!child) {
            continue;
        }
        if (child->style.position == Position::Absolute ||
            child->style.position == Position::Fixed) {
            continue;
        }
        has_flow_child = true;
        break;
    }
    /* 带装饰且能整体放入一页的叶子块（按钮、列表项、窄卡片等）不可拆分：
       只把块盒顶部作为候选，不把其内部行盒顶部作为切页点。 */
    const bool indivisible = page_height > 0.0f && !has_flow_child && !box.lines.empty() &&
                             box.border_box.Height() <= page_height &&
                             HasVisibleBlockDecoration(box);
    if (indivisible) {
        candidates->push_back(box.border_box.top);
        return;
    }
    for (const LineFragment& line : box.lines) {
        if (line.rect.Height() > 0.0f) {
            candidates->push_back(line.rect.top);
        }
    }
    for (const std::unique_ptr<LayoutBox>& child : box.children) {
        if (!child) {
            continue;
        }
        // 行内原子盒（图片、行内块）不可分割，内部行盒不能作为切页边界；
        // 其所属行盒的顶部已经作为候选加入，因此直接跳过整个原子盒子树。
        if (child->kind == BoxKind::Image || child->kind == BoxKind::InlineBlock) {
            continue;
        }
        CollectBreakCandidates(*child, page_height, candidates);
    }
}

void LayoutEngine::BuildPageStarts(std::unique_ptr<LayoutResult>* result) {
    std::unique_ptr<LayoutResult>& holder = *result;
    if (!holder) {
        return;
    }
    LayoutResult& layout = *holder;

    layout.page_starts.clear();
    layout.page_starts.push_back(0.0f);

    const float page_height = layout.page_content_height;
    if (page_height <= 0.0f) {
        return;
    }

    std::vector<float> candidates;
    if (layout.root) {
        CollectBreakCandidates(*layout.root, page_height, &candidates);
    }
    candidates.push_back(0.0f);
    std::sort(candidates.begin(), candidates.end());

    std::vector<float> unique_candidates;
    for (float candidate : candidates) {
        if (unique_candidates.empty() ||
            std::fabs(candidate - unique_candidates.back()) > kEpsilon) {
            unique_candidates.push_back(candidate);
        }
    }

    // 贪心切页：每页尽量放进一个完整页高内的最大行盒/原子盒边界。
    // 若区间内没有任何边界（例如超高的行盒或图片），退回固定页高以保证推进。
    float page_start = 0.0f;
    const size_t maximum_pages = 1000000;
    while (page_start + page_height < layout.content_height - kEpsilon) {
        const float target = page_start + page_height;
        float chosen = target;
        bool found = false;
        for (float candidate : unique_candidates) {
            if (candidate <= page_start + kEpsilon) {
                continue;
            }
            if (candidate > target + kEpsilon) {
                break;
            }
            chosen = candidate;
            found = true;
        }
        if (!found) {
            chosen = target;
        }
        if (chosen <= page_start + kEpsilon) {
            break;
        }
        page_start = chosen;
        layout.page_starts.push_back(page_start);
        if (layout.page_starts.size() > maximum_pages) {
            break;
        }
    }
}

}  // namespace

namespace {

// 单个 option 的取值：有 value 属性时取属性值，否则取去除首尾空白的文本内容。
std::string OptionValueText(const Element& option) {
    if (option.HasAttribute("value")) {
        return option.GetAttribute("value");
    }
    std::string text = TextContent(option);
    TrimAsciiSpace(&text);
    return text;
}

// 表单控件显示字体：优先取行盒中已排版的字体，缺失时按盒样式创建。
FontId FormControlFont(const LayoutBox& box, TextService& text_service) {
    for (const LineFragment& line : box.lines) {
        for (const PlacedText& placed : line.texts) {
            if (placed.font != 0) {
                return placed.font;
            }
        }
    }
    FontRequest request;
    request.family = box.style.font_family;
    request.size = box.style.font_size > 0.0f ? box.style.font_size : 16.0f;
    request.bold = box.style.font_weight >= 600;
    request.italic = box.style.italic;
    return text_service.CreateFont(request);
}

// 文本控件单行高度：行盒为空时按样式的 line-height 与字体度量估算。
float FormControlLineHeight(const LayoutBox& box, TextService& text_service) {
    const FontId font = FormControlFont(box, text_service);
    if (font == 0) {
        return box.style.font_size > 0.0f ? box.style.font_size : 16.0f;
    }
    const FontMetrics metrics = text_service.Metrics(font);
    const CssLength& line_height = box.style.line_height;
    if (line_height.IsNumber()) {
        const float resolved = line_height.value * box.style.font_size;
        return resolved > 0.0f ? resolved : metrics.height;
    }
    if (line_height.IsAuto()) {
        return metrics.height;
    }
    switch (line_height.unit) {
        case LengthUnit::Px:
        case LengthUnit::Number:
            return line_height.value;
        case LengthUnit::Em:
            return line_height.value * box.style.font_size;
        case LengthUnit::Rem:
            // 根元素字号在该接口内不可得，按当前字号换算。
            return line_height.value * box.style.font_size;
        case LengthUnit::Pt:
            return line_height.value * (96.0f / 72.0f);
        case LengthUnit::Pc:
            return line_height.value * 16.0f;
        case LengthUnit::In:
            return line_height.value * 96.0f;
        case LengthUnit::Cm:
            return line_height.value * (96.0f / 2.54f);
        case LengthUnit::Mm:
            return line_height.value * (96.0f / 25.4f);
        default:
            return metrics.height;
    }
}

size_t Utf8ByteOffsetForIndex(const std::string& text, size_t index) {
    size_t offset = 0;
    for (size_t position = 0; position < index && offset < text.size(); ++position) {
        const Utf8Char character = DecodeUtf8(text, offset);
        if (character.length == 0) {
            break;
        }
        offset += character.length;
    }
    return offset;
}

size_t Utf8IndexForByteOffset(const std::string& text, size_t byte_offset) {
    size_t offset = 0;
    size_t index = 0;
    while (offset < byte_offset && offset < text.size()) {
        const Utf8Char character = DecodeUtf8(text, offset);
        if (character.length == 0) {
            break;
        }
        offset += character.length;
        ++index;
    }
    return index;
}

// 按字体步进测量文本前 count 个码位的宽度，字距逐个码位累加。
float MeasureCodepointPrefix(TextService& text_service, FontId font, const std::string& text,
                             size_t count, float letter_spacing) {
    float width = 0.0f;
    size_t offset = 0;
    size_t index = 0;
    while (offset < text.size() && index < count) {
        const Utf8Char character = DecodeUtf8(text, offset);
        if (character.length == 0) {
            break;
        }
        width += text_service.Advance(font, character.codepoint) + letter_spacing;
        offset += character.length;
        ++index;
    }
    return width;
}

// 从行首测量到指定源字节偏移的宽度；只统计属于文本节点的片段。
float MeasureLinePrefixToByte(const LineFragment& line, size_t byte_offset,
                              TextService& text_service) {
    float width = 0.0f;
    for (const PlacedText& placed : line.texts) {
        if (placed.node == nullptr) {
            continue;
        }
        size_t local_offset = 0;
        while (local_offset < placed.text.size()) {
            const Utf8Char character = DecodeUtf8(placed.text, local_offset);
            if (character.length == 0) {
                break;
            }
            const size_t absolute_offset = placed.offset + local_offset;
            if (absolute_offset >= byte_offset) {
                return width;
            }
            width += text_service.Advance(placed.font, character.codepoint) +
                     placed.letter_spacing;
            local_offset += character.length;
        }
    }
    return width;
}

float MeasureLineWidth(const LineFragment& line) {
    float width = 0.0f;
    for (const PlacedText& placed : line.texts) {
        width += placed.width;
    }
    return width;
}

// 一行对应的源文本字节区间。文字行直接取行内文字片段的源偏移；
// 空行（连续换行产生）通过扫描源文本中的换行序列推断起点。
struct LineSourceRange {
    bool has_text = false;
    size_t begin_byte = 0;
    size_t end_byte = 0;
};

// 越过光标处的一个换行序列（支持 LF 与 CRLF）；非换行时保持原位。
void AdvancePastNewline(const std::string& text, size_t* cursor) {
    if (*cursor >= text.size()) {
        return;
    }
    const char character = text[*cursor];
    if (character == '\r') {
        ++*cursor;
        if (*cursor < text.size() && text[*cursor] == '\n') {
            ++*cursor;
        }
        return;
    }
    if (character == '\n') {
        ++*cursor;
    }
}

void BuildLineSourceRanges(const LayoutBox& box, const std::string& text,
                           std::vector<LineSourceRange>* ranges) {
    // 第一遍：记录有文字行的源区间，空行暂记为未知。
    for (const LineFragment& line : box.lines) {
        size_t begin = static_cast<size_t>(-1);
        size_t end = 0;
        for (const PlacedText& placed : line.texts) {
            if (placed.node == nullptr) {
                continue;
            }
            begin = std::min(begin, placed.offset);
            end = std::max(end, placed.offset + placed.text.size());
        }
        LineSourceRange range;
        if (begin != static_cast<size_t>(-1)) {
            range.has_text = true;
            range.begin_byte = begin;
            range.end_byte = end;
        } else {
            range.begin_byte = static_cast<size_t>(-1);
            range.end_byte = static_cast<size_t>(-1);
        }
        ranges->push_back(range);
    }

    // 第二遍：按文字行结束位置与换行序列推断空行起点。
    size_t cursor = 0;
    bool previous_has_text = false;
    size_t previous_end = 0;
    for (size_t index = 0; index < ranges->size(); ++index) {
        LineSourceRange& range = (*ranges)[index];
        if (range.has_text) {
            previous_has_text = true;
            previous_end = range.end_byte;
            cursor = range.end_byte;
            continue;
        }
        if (previous_has_text) {
            cursor = previous_end;
            AdvancePastNewline(text, &cursor);
        }
        range.begin_byte = cursor;
        range.end_byte = cursor;
        AdvancePastNewline(text, &cursor);
        previous_has_text = false;
        previous_end = range.end_byte;
    }
}

bool IsTextareaElement(const Element& element) {
    return AsciiLower(element.TagName()) == "textarea";
}

}  // namespace

std::string FormControlText(const Element& element) {
    const std::string tag_name = AsciiLower(element.TagName());
    if (tag_name == "textarea") {
        return TextContent(element);
    }
    if (tag_name == "select") {
        const Element* first_option = nullptr;
        const Element* selected_option = nullptr;
        for (const NodePtr& child : element.Children()) {
            if (!child || !child->IsElement()) {
                continue;
            }
            const Element& option = static_cast<const Element&>(*child);
            if (option.TagName() != "option") {
                continue;
            }
            if (first_option == nullptr) {
                first_option = &option;
            }
            if (option.HasAttribute("selected")) {
                selected_option = &option;
                break;
            }
        }
        const Element* chosen = selected_option != nullptr ? selected_option : first_option;
        if (chosen == nullptr) {
            return std::string();
        }
        return OptionValueText(*chosen);
    }
    return element.GetAttribute("value");
}

std::string FormDisplayText(const Element& element) {
    const std::string tag_name = AsciiLower(element.TagName());
    if (tag_name == "textarea") {
        return TextContent(element);
    }
    if (tag_name == "select") {
        // 下拉框显示选中 option 的文本内容，value 属性只用于取值匹配。
        const Element* first_option = nullptr;
        const Element* selected_option = nullptr;
        for (const NodePtr& child : element.Children()) {
            if (!child || !child->IsElement()) {
                continue;
            }
            const Element& option = static_cast<const Element&>(*child);
            if (option.TagName() != "option") {
                continue;
            }
            if (first_option == nullptr) {
                first_option = &option;
            }
            if (option.HasAttribute("selected")) {
                selected_option = &option;
                break;
            }
        }
        const Element* chosen = selected_option != nullptr ? selected_option : first_option;
        if (chosen == nullptr) {
            return std::string();
        }
        std::string text = TextContent(*chosen);
        TrimAsciiSpace(&text);
        return text;
    }
    if (tag_name != "input") {
        return element.GetAttribute("value");
    }
    std::string type_name = element.GetAttribute("type");
    TrimAsciiSpace(&type_name);
    if (AsciiLower(type_name) == "password") {
        return MaskPasswordText(element.GetAttribute("value"));
    }
    return element.GetAttribute("value");
}

FormCaretPosition LocateFormCaret(const LayoutBox& box, const std::string& text,
                                  size_t caret_index, TextService& text_service) {
    FormCaretPosition result;
    if (box.element == nullptr) {
        return result;
    }

    const FontId font = FormControlFont(box, text_service);
    const float line_height = FormControlLineHeight(box, text_service);

    if (!IsTextareaElement(*box.element)) {
        const std::string display = FormDisplayText(*box.element);
        const size_t display_length = Utf8Length(display);
        const size_t clamped = std::min(caret_index, display_length);
        result.x = MeasureCodepointPrefix(text_service, font, display, clamped,
                                          box.style.letter_spacing);
        result.line_index = 0;
        if (!box.lines.empty()) {
            result.line_top = box.lines.front().rect.top;
            result.line_bottom = box.lines.front().rect.bottom;
        } else {
            result.line_top = box.content_box.top;
            result.line_bottom = box.content_box.top + line_height;
        }
        result.valid = true;
        return result;
    }

    if (box.lines.empty()) {
        result.x = 0.0f;
        result.line_index = 0;
        result.line_top = box.content_box.top;
        result.line_bottom = box.content_box.top + line_height;
        result.valid = true;
        return result;
    }

    std::vector<LineSourceRange> ranges;
    BuildLineSourceRanges(box, text, &ranges);
    const size_t caret_byte =
        Utf8ByteOffsetForIndex(text, std::min(caret_index, Utf8Length(text)));

    bool has_end_candidate = false;
    FormCaretPosition end_candidate;
    for (size_t index = 0; index < box.lines.size() && index < ranges.size(); ++index) {
        const LineFragment& line = box.lines[index];
        const LineSourceRange& range = ranges[index];
        if (range.has_text && caret_byte >= range.begin_byte && caret_byte < range.end_byte) {
            result.line_index = index;
            result.x = MeasureLinePrefixToByte(line, caret_byte, text_service);
            result.line_top = line.rect.top;
            result.line_bottom = line.rect.bottom;
            result.valid = true;
            return result;
        }
        if (!range.has_text && caret_byte == range.begin_byte) {
            result.line_index = index;
            result.x = 0.0f;
            result.line_top = line.rect.top;
            result.line_bottom = line.rect.bottom;
            result.valid = true;
            return result;
        }
        if (range.has_text && caret_byte == range.end_byte) {
            // 记录行末位置；若下一行与行末共享同一字节（软换行），由下一行优先命中。
            has_end_candidate = true;
            end_candidate.line_index = index;
            end_candidate.x = MeasureLineWidth(line);
            end_candidate.line_top = line.rect.top;
            end_candidate.line_bottom = line.rect.bottom;
        }
    }
    if (has_end_candidate) {
        end_candidate.valid = true;
        return end_candidate;
    }

    // 光标位于首个文字行之前或最后一个文字行之后。
    const size_t fallback_index =
        caret_byte == 0 ? 0 : box.lines.size() - 1;
    const LineFragment& line = box.lines[fallback_index];
    result.line_index = fallback_index;
    result.x = caret_byte == 0 ? 0.0f : MeasureLineWidth(line);
    result.line_top = line.rect.top;
    result.line_bottom = line.rect.bottom;
    result.valid = true;
    return result;
}

FormLineRange GetFormLineRange(const LayoutBox& box, const std::string& text, size_t line_index,
                               TextService& text_service) {
    FormLineRange result;
    if (box.element == nullptr) {
        return result;
    }

    const FontId font = FormControlFont(box, text_service);
    const float line_height = FormControlLineHeight(box, text_service);

    if (!IsTextareaElement(*box.element)) {
        if (line_index != 0) {
            return result;
        }
        const std::string display = FormDisplayText(*box.element);
        result.begin_index = 0;
        result.end_index = Utf8Length(display);
        result.width = MeasureCodepointPrefix(text_service, font, display, result.end_index,
                                              box.style.letter_spacing);
        if (!box.lines.empty()) {
            result.line_top = box.lines.front().rect.top;
            result.line_bottom = box.lines.front().rect.bottom;
        } else {
            result.line_top = box.content_box.top;
            result.line_bottom = box.content_box.top + line_height;
        }
        result.valid = true;
        return result;
    }

    if (box.lines.empty()) {
        // 空文本域没有行盒，按单行空文本返回，便于点击定位与光标绘制。
        if (line_index != 0) {
            return result;
        }
        result.begin_index = 0;
        result.end_index = Utf8Length(text);
        result.width = 0.0f;
        result.line_top = box.content_box.top;
        result.line_bottom = box.content_box.top + line_height;
        result.valid = true;
        return result;
    }
    if (line_index >= box.lines.size()) {
        return result;
    }
    std::vector<LineSourceRange> ranges;
    BuildLineSourceRanges(box, text, &ranges);
    if (line_index >= ranges.size()) {
        return result;
    }
    const LineSourceRange& range = ranges[line_index];
    const LineFragment& line = box.lines[line_index];
    result.begin_index = Utf8IndexForByteOffset(text, range.begin_byte);
    result.end_index = Utf8IndexForByteOffset(text, range.end_byte);
    result.width = MeasureLineWidth(line);
    result.line_top = line.rect.top;
    result.line_bottom = line.rect.bottom;
    result.valid = true;
    return result;
}

std::unique_ptr<LayoutResult> LayoutDocument(const Document& document,
                                             const StyleResolver& resolver,
                                             TextService& text,
                                             const LayoutOptions& options) {
    LayoutEngine engine(resolver, text, options);
    return engine.Run(document);
}

namespace {

// 收集文档中的 SVG 根元素。嵌套 SVG 由外层根节点的遍历覆盖，这里不重复收集。
void CollectSvgRoots(const Element& element, std::vector<const Element*>* roots) {
    if (element.TagName() == "svg") {
        roots->push_back(&element);
        return;
    }
    for (const NodePtr& child : element.Children()) {
        if (!child || !child->IsElement()) {
            continue;
        }
        CollectSvgRoots(static_cast<const Element&>(*child), roots);
    }
}

// 祖先链签名：SVG 根的继承值取决于 HTML 祖宗的 class/style/id。
std::string AncestorSignature(const Element& element, const Element* stop) {
    std::vector<std::string> parts;
    for (const Node* node = element.Parent(); node != nullptr && node != stop;
         node = node->Parent()) {
        if (!node->IsElement()) {
            continue;
        }
        const Element& ancestor = static_cast<const Element&>(*node);
        parts.push_back(ancestor.TagName());
        parts.push_back(ancestor.GetAttribute("class"));
        parts.push_back(ancestor.GetAttribute("style"));
        parts.push_back(ancestor.GetAttribute("id"));
    }
    std::string signature;
    for (const std::string& part : parts) {
        signature.append(part);
        signature.push_back('\x1f');
    }
    return signature;
}

// 计算 SVG 根元素的计算样式：从文档根元素沿祖先链逐层计算，保证自定义属性、
// 字体与颜色继承与布局阶段完全一致。
ComputedStyle ComputeSvgRootStyle(const Element& svg_root, const Document& document,
                                  const StyleResolver& resolver) {
    const ComputedStyle initial = resolver.InitialStyle();
    ComputedStyle style = initial;
    float root_font_size = initial.font_size > 0.0f ? initial.font_size : 16.0f;
    const Element* root_element = document.RootElement();
    if (root_element != nullptr) {
        style = resolver.ComputeFor(*root_element, initial);
        if (style.font_size > 0.0f) {
            root_font_size = style.font_size;
        }
    }
    style.root_font_size = root_font_size;

    std::vector<const Element*> chain;
    for (const Node* node = svg_root.Parent(); node != nullptr; node = node->Parent()) {
        if (!node->IsElement()) {
            continue;
        }
        if (node == root_element) {
            break;
        }
        chain.push_back(static_cast<const Element*>(node));
    }
    std::reverse(chain.begin(), chain.end());
    for (const Element* ancestor : chain) {
        style = resolver.ComputeFor(*ancestor, style);
    }
    return resolver.ComputeFor(svg_root, style);
}

}  // namespace

void RefreshSvgStyles(const Document& document, const StyleResolver& resolver,
                      LayoutResult* layout, const LayoutResult* previous) {
    if (layout == nullptr) {
        return;
    }
    // previous 可以与 layout 相同（SVG 内部变更后的原地刷新），此时在重建
    // 完成前不要修改原映射表。
    const std::map<const Element*, ComputedStyle>* previous_styles = &layout->svg_styles;
    const std::map<const Element*, SvgStyleSignature>* previous_signatures =
        &layout->svg_style_signatures;
    if (previous != nullptr && previous != layout) {
        previous_styles = &previous->svg_styles;
        previous_signatures = &previous->svg_style_signatures;
    }
    const bool has_previous = previous != nullptr;

    std::map<const Element*, ComputedStyle> next_styles;
    std::map<const Element*, SvgStyleSignature> next_signatures;

    auto make_signature = [](const Element& element) {
        SvgStyleSignature signature;
        signature.class_value = element.GetAttribute("class");
        signature.style_value = element.GetAttribute("style");
        signature.id_value = element.GetAttribute("id");
        return signature;
    };
    auto signature_matches = [](const SvgStyleSignature& left, const SvgStyleSignature& right) {
        return left.class_value == right.class_value && left.style_value == right.style_value &&
               left.id_value == right.id_value;
    };

    std::function<void(const Element&, const ComputedStyle&, bool, const ComputedStyle*)> visit =
        [&](const Element& element, const ComputedStyle& parent_style, bool parent_recomputed,
            const ComputedStyle* precomputed) {
            const SvgStyleSignature signature = make_signature(element);
            const ComputedStyle* reused = precomputed;
            if (reused == nullptr && has_previous && !parent_recomputed) {
                const auto style_it = previous_styles->find(&element);
                const auto signature_it = previous_signatures->find(&element);
                if (style_it != previous_styles->end() &&
                    signature_it != previous_signatures->end() &&
                    signature_matches(signature_it->second, signature)) {
                    reused = &style_it->second;
                }
            }
            const ComputedStyle computed =
                reused != nullptr ? *reused : resolver.ComputeFor(element, parent_style);
            const bool recomputed = reused == nullptr;
            next_signatures[&element] = signature;
            next_styles[&element] = computed;
            for (const NodePtr& child : element.Children()) {
                if (!child || !child->IsElement()) {
                    continue;
                }
                visit(static_cast<const Element&>(*child), computed, recomputed, nullptr);
            }
        };

    std::vector<const Element*> roots;
    if (const Element* document_element = document.RootElement()) {
        CollectSvgRoots(*document_element, &roots);
    }
    for (const Element* root : roots) {
        const Element* document_element = document.RootElement();
        const std::string ancestor_signature = AncestorSignature(*root, document_element);
        const ComputedStyle* reused_root = nullptr;
        if (has_previous) {
            const auto style_it = previous_styles->find(root);
            const auto signature_it = previous_signatures->find(root);
            if (style_it != previous_styles->end() &&
                signature_it != previous_signatures->end() &&
                signature_matches(signature_it->second, make_signature(*root)) &&
                signature_it->second.ancestor_value == ancestor_signature) {
                reused_root = &style_it->second;
            }
        }
        ComputedStyle computed_root;
        if (reused_root == nullptr) {
            computed_root = ComputeSvgRootStyle(*root, document, resolver);
            reused_root = &computed_root;
        }
        // 根节点自身的签名需要额外记录祖先签名，供下次判断复用。
        const ComputedStyle& root_style = *reused_root;
        visit(*root, root_style, false, reused_root);
        // visit 写入的签名补上祖先链信息。
        next_signatures[root].ancestor_value = ancestor_signature;
    }
    layout->svg_styles = std::move(next_styles);
    layout->svg_style_signatures = std::move(next_signatures);
}

}  // namespace web
