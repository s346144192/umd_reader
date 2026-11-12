#pragma once

#include <map>
#include <string>

#include "web/css.h"
#include "web/dom.h"
#include "web/types.h"

namespace web {

enum class Display {
    None,
    Block,
    Inline,
    InlineBlock,
    ListItem,
    Flex,
    InlineFlex,
    Grid,
    InlineGrid,
};

// 定位方式。Static 为常规文档流；Relative 保留原位并按偏移平移；Absolute/Fixed
// 脱离文档流，按最近的非 static 祖先（或视口）定位。
enum class Position {
    Static,
    Relative,
    Absolute,
    Fixed,
    Sticky,
};

// 浮动方向。Left/Right 使盒子脱离普通流，靠容器左/右边缘依次排列。
enum class FloatSide {
    None,
    Left,
    Right,
};

// 清除浮动方向。Both 同时避让左右两侧的浮动。
enum class ClearSide {
    None,
    Left,
    Right,
    Both,
};

// 渐变色标。offset 为 0~1 的归一化位置，has_offset 为 false 时按顺序均分。
struct GradientStop {
    Color color = Black();
    float offset = 0.0f;
    bool has_offset = false;
};

// background-image 的渐变描述。linear 使用方向角（0 度指向上，顺时针），
// radial 使用相对盒尺寸的中心点（0~1）。
struct BackgroundImage {
    enum class Kind {
        None,
        Linear,
        Radial,
    };

    Kind kind = Kind::None;
    float angle_deg = 180.0f;
    float center_x = 0.5f;
    float center_y = 0.5f;
    float radius_x = 0.5f;
    float radius_y = 0.5f;
    std::vector<GradientStop> stops;
};

// 阴影：box-shadow 与 text-shadow 共用。blur 为模糊半径，spread 仅 box-shadow
// 使用；inset 仅 box-shadow 使用。
struct Shadow {
    float offset_x = 0.0f;
    float offset_y = 0.0f;
    float blur = 0.0f;
    float spread = 0.0f;
    Color color = Transparent();
    bool inset = false;
};

// 动画缓动函数。
enum class AnimationTiming {
    Linear,
    Ease,
    EaseIn,
    EaseOut,
    EaseInOut,
};

// 动画播放方向。
enum class AnimationDirection {
    Normal,
    Reverse,
    Alternate,
    AlternateReverse,
};

// 动画结束后的填充方式。
enum class AnimationFill {
    None,
    Forwards,
    Backwards,
    Both,
};

// 单条 CSS 动画。iteration_count 为负表示无限循环。
struct AnimationSpec {
    std::string name;
    float duration = 0.0f;
    float delay = 0.0f;
    float iteration_count = 1.0f;
    AnimationTiming timing = AnimationTiming::Ease;
    AnimationDirection direction = AnimationDirection::Normal;
    AnimationFill fill = AnimationFill::None;
};

// 网格轨道。fraction 大于 0 表示弹性单位 fr，此时忽略 length。
// auto_repeat 为 true 时表示 repeat(auto-fill/auto-fit, ...) 生成的轨道，
// 轨道数量由布局阶段按容器宽度决定；auto_fit 区分 auto-fit 与 auto-fill。
struct GridTrack {
    CssLength length;
    float fraction = 0.0f;
    bool auto_repeat = false;
    bool auto_fit = false;
    // minmax() 的最小值；auto-fill/auto-fit 计算重复次数时作为轨道最小宽度。
    CssLength minimum;
};

// 边框线型。None 表示不绘制该边；Double 用两条实线近似。
enum class BorderStyle {
    None,
    Solid,
    Dashed,
    Dotted,
    Double,
};

// 背景图片的尺寸策略。
enum class BackgroundSizeKind {
    Auto,
    Cover,
    Contain,
    Length,
};

struct BackgroundSize {
    BackgroundSizeKind kind = BackgroundSizeKind::Auto;
    CssLength width;
    CssLength height;
};

// 背景图片的平铺方式。
enum class BackgroundRepeat {
    Repeat,
    NoRepeat,
    RepeatX,
    RepeatY,
};

// 弹性盒的主轴方向与换行方式。
enum class FlexDirection {
    Row,
    RowReverse,
    Column,
    ColumnReverse,
};

enum class FlexWrap {
    NoWrap,
    Wrap,
    WrapReverse,
};

enum class JustifyContent {
    FlexStart,
    FlexEnd,
    Center,
    SpaceBetween,
    SpaceAround,
    SpaceEvenly,
};

// 交叉轴对齐。Auto 只用于 align-self，表示沿用容器的 align-items。
enum class AlignItems {
    Auto,
    Stretch,
    FlexStart,
    FlexEnd,
    Center,
    Baseline,
};

enum class TextAlign {
    Left,
    Center,
    Right,
    Justify,
    // start/end 与文字方向相关；本引擎按从左到右的书写方向解释。
    Start,
    End,
};

// 交叉轴多行对齐（flex 的 align-content 与 grid 的行分布共用）。
enum class AlignContent {
    Stretch,
    FlexStart,
    FlexEnd,
    Center,
    SpaceBetween,
    SpaceAround,
    SpaceEvenly,
};

// 文本断词策略。
enum class WordBreak {
    Normal,
    BreakAll,   // 任意字符之间都可断行
    KeepAll,    // 禁止在 CJK 文本中自动断行
};

// 列表标记相对内容的位置。
enum class ListStylePosition {
    Outside,
    Inside,
};

// 替换元素的尺寸适配方式。
enum class ObjectFit {
    Fill,
    Contain,
    Cover,
    None,
    ScaleDown,
};

// 表格相邻边框的处理方式。Separate 为分离边框模型，相邻单元格各自绘制边框；
// Collapse 为合并边框模型，相邻单元格共享一条边框线，只绘制一次。
enum class BorderCollapse {
    Separate,
    Collapse,
};

enum class WhiteSpace {
    Normal,
    Pre,
    NoWrap,
    PreWrap,
    PreLine,
};

// SVG 描边的线端形状。
enum class SvgLineCap {
    Butt,
    Round,
    Square,
};

// SVG 描边在折点处的连接形状。
enum class SvgLineJoin {
    Miter,
    Round,
    Bevel,
};

// SVG 文本锚点：x 坐标分别对应文本的起点、中点与终点。
enum class SvgTextAnchor {
    Start,
    Middle,
    End,
};

// SVG 基线对齐方式。本引擎只区分字母基线、中央基线与悬挂基线，
// 其他取值按字母基线处理。
enum class SvgBaseline {
    Alphabetic,
    Central,
    Middle,
    Hanging,
};

// SVG 命中测试策略，对应 pointer-events 属性的常用取值。Auto 表示按
// visiblePainted 处理：填充区域与描边区域都可命中。
enum class SvgPointerEvents {
    Auto,
    None,
    VisiblePainted,
    VisibleFill,
    VisibleStroke,
    Fill,
    Stroke,
    All,
    BoundingBox,
};

enum class ListStyleType {
    None,
    Disc,
    Circle,
    Square,
    Decimal,
    LowerRoman,
    UpperRoman,
    LowerAlpha,
    UpperAlpha,
};

struct EdgeLengths {
    CssLength top = CssLength{0.0f, LengthUnit::Px};
    CssLength right = CssLength{0.0f, LengthUnit::Px};
    CssLength bottom = CssLength{0.0f, LengthUnit::Px};
    CssLength left = CssLength{0.0f, LengthUnit::Px};
};

// 单个角的圆角半径指定值。horizontal/vertical 为水平与垂直半径：
// 对应 percent 标记为 true 时表示相对边框盒宽、高的百分比，否则为像素值；
// offset 为 calc() 表达式中的像素分量。未写斜杠语法时垂直半径等于水平半径。
struct CornerRadius {
    float horizontal = 0.0f;
    float vertical = 0.0f;
    float horizontal_offset = 0.0f;
    float vertical_offset = 0.0f;
    bool horizontal_percent = false;
    bool vertical_percent = false;
};

// 计算样式：所有可继承属性已从父元素继承，百分比与 em 仍以指定值保存，
// 由布局阶段按包含块与字号解析为像素。
struct ComputedStyle {
    Display display = Display::Inline;

    // 未指定时 width/height 为 Auto，由内容决定。
    CssLength width;
    CssLength height;

    // 尺寸限制；Auto 表示 none（无限制）。
    CssLength min_width;
    CssLength max_width;
    CssLength min_height;
    CssLength max_height;

    // box-sizing：false 为 content-box（默认），true 为 border-box。
    bool border_box = false;

    EdgeLengths margin;
    EdgeLengths padding;
    EdgeLengths border_width;

    Color border_color_top = Transparent();
    Color border_color_right = Transparent();
    Color border_color_bottom = Transparent();
    Color border_color_left = Transparent();

    // 四条边的线型。None 时该边不绘制（与 border-width 为 0 等效）。
    BorderStyle border_style_top = BorderStyle::None;
    BorderStyle border_style_right = BorderStyle::None;
    BorderStyle border_style_bottom = BorderStyle::None;
    BorderStyle border_style_left = BorderStyle::None;

    // 四角独立圆角。未指定时为 0，即直角。
    CornerRadius border_radius_top_left;
    CornerRadius border_radius_top_right;
    CornerRadius border_radius_bottom_right;
    CornerRadius border_radius_bottom_left;

    float font_size = 16.0f;
    std::string font_family;  // 首选族名，空表示系统默认字体
    int font_weight = 400;
    bool italic = false;

    Color color = Black();
    Color background_color = Transparent();

    // line-height：Number 单位表示字号倍数，Auto 表示 normal（取字体度量）。
    CssLength line_height;

    TextAlign text_align = TextAlign::Left;
    WhiteSpace white_space = WhiteSpace::Normal;
    float letter_spacing = 0.0f;
    bool underline = false;

    // 首行缩进：仅作用于块容器排出的第一行，按内容宽度解析百分比。
    CssLength text_indent;
    // text-overflow: ellipsis。仅在 overflow 裁剪且文本超出行宽时生效。
    bool text_overflow_ellipsis = false;
    // 断词策略与长单词溢出时的任意断行（overflow-wrap: break-word/anywhere）。
    WordBreak word_break = WordBreak::Normal;
    bool overflow_wrap_anywhere = false;
    // 列表标记位置；Inside 时标记计入首行行内流。
    ListStylePosition list_style_position = ListStylePosition::Outside;

    // 表格专用属性。border-spacing 的分量以像素保存；负值表示百分比间距，
    // 绝对值为百分比数值（例如 -5.0f 表示内容宽度的 5%），布局阶段按表格
    // 内容宽度换算。默认 0 表示没有间距。
    float border_spacing_x = 0.0f;
    float border_spacing_y = 0.0f;
    BorderCollapse border_collapse = BorderCollapse::Separate;

    ListStyleType list_style_type = ListStyleType::None;
    // 列表项标记文本（由布局阶段根据 list_style_type 与序号生成）。

    // CSS 自定义属性（名称以 -- 开头）。值保留解析后的分量，随继承传递，
    // 由 var() 展开时按需取用；未解析的引用在展开阶段忽略。
    std::map<std::string, CssValue> custom_properties;

    // 弹性盒属性。flex_basis 为 Auto 时依次回退到主轴尺寸与内容尺寸；
    // 间距在布局阶段按字体与包含块解析。
    FlexDirection flex_direction = FlexDirection::Row;
    FlexWrap flex_wrap = FlexWrap::NoWrap;
    JustifyContent justify_content = JustifyContent::FlexStart;
    AlignItems align_items = AlignItems::Stretch;
    AlignItems align_self = AlignItems::Auto;
    float flex_grow = 0.0f;
    float flex_shrink = 1.0f;
    CssLength flex_basis;
    CssLength row_gap;
    CssLength column_gap;

    // 交叉轴多行对齐。flex 容器的 align-content 与 grid 容器的行分布共用。
    AlignContent align_content = AlignContent::Stretch;
    // 弹性项目的排列次序：数值小的排在主轴起点方向更靠前。
    int order = 0;

    // 网格容器属性。轨道按解析后的像素与 fr 分配；子项的 grid_column_span /
    // grid_row_span 大于 1 时跨多列或多行。
    std::vector<GridTrack> grid_columns;
    std::vector<GridTrack> grid_rows;
    int grid_column_span = 1;
    int grid_row_span = 1;
    // 显式网格线位置：正数从起点计数，负数从终点计数，0 表示 auto。
    // 同时给出起止线时优先按起止线放置，跨度由两端之差决定。
    int grid_column_start = 0;
    int grid_column_end = 0;
    int grid_row_start = 0;
    int grid_row_end = 0;
    // grid-template-areas：按行保存的命名区域，空字符串表示该格不命名。
    std::vector<std::vector<std::string>> grid_template_areas;
    // grid-area 的区域名（单标识符形式），与 grid-template-areas 中的名称对应。
    std::string grid_area_name;

    // 替换元素（img）的内容适配方式与位置（0~1 比例）。
    ObjectFit object_fit = ObjectFit::Fill;
    float object_position_x = 0.5f;
    float object_position_y = 0.5f;

    // 滤镜的静态近似：绘制层按这些系数变换颜色。blur 等无法在 2D 矩形绘制
    // 中表达的效果忽略（保持原样）。
    float filter_brightness = 1.0f;
    float filter_contrast = 1.0f;
    float filter_saturate = 1.0f;
    float filter_grayscale = 0.0f;
    float filter_sepia = 0.0f;
    float filter_invert = 0.0f;
    bool has_filter = false;

    // 伪元素 ::before / ::after 的内容文本与来源标记。
    // 由 StyleResolver::ComputePseudoFor 填充，参与布局时作为行内内容。
    std::string pseudo_content;
    std::string pseudo_name;  // "before" 或 "after"；为空表示普通元素样式

    // 定位属性。absolute/fixed 盒子脱离文档流，按 position_* 偏移定位；
    // 未指定的偏移为 auto，由布局按静态位置确定。
    Position position = Position::Static;
    CssLength position_top;
    CssLength position_right;
    CssLength position_bottom;
    CssLength position_left;
    int z_index = 0;
    bool z_index_auto = true;

    // 浮动与清除浮动。
    FloatSide float_side = FloatSide::None;
    ClearSide clear_side = ClearSide::None;

    // 透明度作用于整个盒子及其子树，绘制层按该值合成颜色。
    float opacity = 1.0f;

    /* SVG 呈现属性。CSS 声明优先于同名 XML 属性，因此这些字段只记录本元素
       上由样式表或内联 style 给出的值；绘制 SVG 时按“自身 CSS → 自身属性 →
       父级继承值”的顺序合成。字体相关属性复用上面的 font_* 字段。 */
    bool has_svg_fill = false;
    bool svg_fill_none = false;
    Color svg_fill = Black();
    bool has_svg_fill_opacity = false;
    float svg_fill_opacity = 1.0f;
    bool has_svg_stroke = false;
    bool svg_stroke_none = true;
    Color svg_stroke = Transparent();
    bool has_svg_stroke_opacity = false;
    float svg_stroke_opacity = 1.0f;
    bool has_svg_stroke_width = false;
    float svg_stroke_width = 1.0f;
    bool has_svg_line_cap = false;
    SvgLineCap svg_line_cap = SvgLineCap::Butt;
    bool has_svg_line_join = false;
    SvgLineJoin svg_line_join = SvgLineJoin::Miter;
    bool has_svg_dash_array = false;
    std::vector<float> svg_dash_array;
    bool has_svg_dash_offset = false;
    float svg_dash_offset = 0.0f;
    bool has_svg_text_anchor = false;
    SvgTextAnchor svg_text_anchor = SvgTextAnchor::Start;
    bool has_svg_baseline = false;
    SvgBaseline svg_baseline = SvgBaseline::Alphabetic;
    bool has_svg_paint_order = false;
    bool svg_stroke_first = false;
    // 字体属性是否由本元素的样式声明给出。SVG 呈现属性优先于继承值，
    // 因此绘制阶段需要区分“本元素声明的字体”与“从父级继承的字体”。
    bool has_svg_font_family = false;
    bool has_svg_font_size = false;
    bool has_svg_font_weight = false;
    bool has_svg_font_style = false;
    // 命中测试策略。未声明时按 Auto（visiblePainted）处理。
    bool has_svg_pointer_events = false;
    SvgPointerEvents svg_pointer_events = SvgPointerEvents::Auto;

    // 背景渐变（覆盖在 background-color 之上）与阴影列表。
    BackgroundImage background_image;
    // 背景图片（background-image: url(...)）。相对路径按文档基准目录解析。
    std::string background_image_url;
    BackgroundSize background_size;
    // 背景位置：0~1 比例，0.5 表示居中。
    float background_position_x = 0.0f;
    float background_position_y = 0.0f;
    BackgroundRepeat background_repeat = BackgroundRepeat::Repeat;
    std::vector<Shadow> box_shadows;
    std::vector<Shadow> text_shadows;
    // background-clip: text：渐变只作为文字填充。本引擎不支持按字形裁剪，
    // 布局阶段改用渐变的起始色作为文字颜色，并停止绘制背景渐变。
    bool background_clip_text = false;
    // overflow 为 hidden/clip/auto/scroll 时，内容与子盒裁剪到内边距盒。
    bool overflow_hidden = false;
    // clip-path: inset(...)。四个内缩量按盒子宽高解析百分比；本引擎以矩形
    // 裁剪近似，circle()/polygon() 等曲线路径暂不参与裁剪。
    bool has_clip_path = false;
    CssLength clip_inset_top;
    CssLength clip_inset_right;
    CssLength clip_inset_bottom;
    CssLength clip_inset_left;

    // 变换：仅支持平移与缩放。平移保留长度值，布局阶段按盒尺寸解析百分比
    // （translateX(-50%) 依赖盒宽）。
    CssLength transform_translate_x;
    CssLength transform_translate_y;
    float scale_x = 1.0f;
    float scale_y = 1.0f;
    bool has_transform = false;

    // CSS 动画列表。布局前由 StyleResolver 按时钟采样，覆盖对应属性。
    std::vector<AnimationSpec> animations;

    // 根元素字号，供 rem 与长度函数求值使用；随继承逐层传递。
    float root_font_size = 16.0f;

    bool IsFlexContainer() const {
        return display == Display::Flex || display == Display::InlineFlex;
    }
    bool IsGridContainer() const {
        return display == Display::Grid || display == Display::InlineGrid;
    }
    // 是否为脱离文档流的定位盒。
    bool IsOutOfFlow() const {
        return position == Position::Absolute || position == Position::Fixed;
    }

    bool IsInlineLevel() const {
        return display == Display::Inline || display == Display::InlineBlock ||
               display == Display::InlineFlex || display == Display::InlineGrid;
    }
    bool IsBlockLevel() const {
        return display == Display::Block || display == Display::ListItem ||
               display == Display::Flex || display == Display::Grid;
    }
};

// 层叠与继承：内部持有作者样式表与内置的浏览器默认样式（UA 样式）。
class StyleResolver {
public:
    StyleResolver();
    ~StyleResolver();

    void SetStyleSheet(const StyleSheet* stylesheet);

    // 视口尺寸：长度函数中的 vw/vh/vmin/vmax 与 @media 条件按此求值。
    void SetViewport(float width, float height);

    // 动画时钟（秒）。每次布局前设置，计算样式时按该时刻采样关键帧。
    void SetAnimationTime(double seconds);

    // 最近一次样式计算中是否出现过带动画的元素，供窗口层决定按帧重绘。
    void ResetAnimationFlag() const { animation_active_ = false; }
    bool animation_active() const { return animation_active_; }

    // 文档根元素使用的初始样式（已应用 UA 默认值）。
    ComputedStyle InitialStyle() const;

    // 计算元素样式：匹配作者规则、按优先级与来源顺序层叠、处理继承与行内样式。
    ComputedStyle ComputeFor(const Element& element, const ComputedStyle& parent_style) const;

    // 计算伪元素 ::before / ::after 的样式。origin_style 为宿主元素的计算样式，
    // 伪元素的继承属性由它取得。规则中存在匹配的伪元素且 content 不为 none 时
    // 返回 true 并写入 output（含 pseudo_content 与 pseudo_name）。
    bool ComputePseudoFor(const Element& element, const ComputedStyle& origin_style,
                          const std::string& pseudo_name, ComputedStyle* output) const;

private:
    const StyleSheet* stylesheet_ = nullptr;
    float viewport_width_ = 860.0f;
    float viewport_height_ = 800.0f;
    double animation_time_ = 0.0;
    mutable bool animation_active_ = false;
};

}  // namespace web
