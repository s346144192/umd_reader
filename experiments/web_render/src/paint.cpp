// 绘制层实现：把布局结果绘制到 RenderDevice，并在页面坐标下完成命中测试。
// 本文件只使用公共头文件与标准库，不依赖任何图形后端的具体实现。

#include "web/paint.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "web/image.h"
#include "web/svg.h"
#include "web/utf8.h"

namespace web {
namespace {

// 单侧边框宽度，四边可不同。
struct BorderWidths {
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
    float left = 0.0f;
};

// 单侧边框颜色，四边可不同。
struct BorderColors {
    Color top = Transparent();
    Color right = Transparent();
    Color bottom = Transparent();
    Color left = Transparent();
};

// 单侧边框线型，四边可不同。
struct BorderStyles {
    BorderStyle top = BorderStyle::None;
    BorderStyle right = BorderStyle::None;
    BorderStyle bottom = BorderStyle::None;
    BorderStyle left = BorderStyle::None;
};

// 四角圆角半径。每角分别记录水平与垂直半径，百分比半径因此可以形成椭圆角。
struct CornerRadii {
    float top_left_x = 0.0f;
    float top_left_y = 0.0f;
    float top_right_x = 0.0f;
    float top_right_y = 0.0f;
    float bottom_right_x = 0.0f;
    float bottom_right_y = 0.0f;
    float bottom_left_x = 0.0f;
    float bottom_left_y = 0.0f;

    bool Empty() const {
        return top_left_x <= 0.0f && top_left_y <= 0.0f && top_right_x <= 0.0f &&
               top_right_y <= 0.0f && bottom_right_x <= 0.0f && bottom_right_y <= 0.0f &&
               bottom_left_x <= 0.0f && bottom_left_y <= 0.0f;
    }
    float Maximum() const {
        return std::max(std::max(std::max(top_left_x, top_left_y), std::max(top_right_x, top_right_y)),
                        std::max(std::max(bottom_right_x, bottom_right_y),
                                 std::max(bottom_left_x, bottom_left_y)));
    }
    // 四角半径完全一致且每角为圆角时，可以使用后端只接受单一半径的绘制接口。
    bool UniformCircular() const {
        return top_left_x == top_left_y && top_right_x == top_right_y &&
               bottom_right_x == bottom_right_y && bottom_left_x == bottom_left_y &&
               top_left_x == top_right_x && top_left_x == bottom_right_x &&
               top_left_x == bottom_left_x;
    }
};

// 单页绘制期间共享的上下文。绘制内嵌文档时 page_top/page_bottom 切换到子文档
// 坐标，offset_x/offset_y 记录子文档原点到当前页面坐标的平移量，
// layout 指向当前正在绘制的布局结果（子文档有自己的 iframe 映射表）。
struct PaintContext {
    RenderDevice* device = nullptr;
    ImageCache* image_cache = nullptr;
    TextService* text_service = nullptr;
    const std::map<GlyphKey, GlyphInfo>* glyph_info = nullptr;
    int atlas_width = 0;
    int atlas_height = 0;
    float page_top = 0.0f;
    float page_bottom = 0.0f;
    float offset_x = 0.0f;
    float offset_y = 0.0f;
    const LayoutResult* layout = nullptr;
    // 文本控件焦点状态；无窗口渲染与未聚焦页面为空指针。
    const FormFocusState* focus = nullptr;
    // 当前累计透明度（opacity 沿子树相乘）；绘制时把颜色 alpha 乘以该值。
    float opacity = 1.0f;
    // 当前累计滤镜系数：filter 沿子树生效，子盒继承父盒的滤镜。
    float filter_brightness = 1.0f;
    float filter_contrast = 1.0f;
    float filter_saturate = 1.0f;
    float filter_grayscale = 0.0f;
    float filter_sepia = 0.0f;
    float filter_invert = 0.0f;
};

// 颜色分量限制在 0..1。
float ClampColorComponent(float value) {
    return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
}

// 按滤镜系数变换颜色。滤镜只影响颜色，不影响几何；图片纹理不受影响。
Color ApplyFilter(const Color& color, const PaintContext& context) {
    if (context.filter_brightness == 1.0f && context.filter_contrast == 1.0f &&
        context.filter_saturate == 1.0f && context.filter_grayscale <= 0.0f &&
        context.filter_sepia <= 0.0f && context.filter_invert <= 0.0f) {
        return color;
    }
    float r = color.r;
    float g = color.g;
    float b = color.b;

    // 可选：亮度与对比度按 CSS filter 的线性变换。
    r = (r * context.filter_brightness - 0.5f) * context.filter_contrast + 0.5f;
    g = (g * context.filter_brightness - 0.5f) * context.filter_contrast + 0.5f;
    b = (b * context.filter_brightness - 0.5f) * context.filter_contrast + 0.5f;

    const float luma = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    if (context.filter_saturate != 1.0f) {
        r = luma + (r - luma) * context.filter_saturate;
        g = luma + (g - luma) * context.filter_saturate;
        b = luma + (b - luma) * context.filter_saturate;
    }
    if (context.filter_grayscale > 0.0f) {
        const float amount = ClampColorComponent(context.filter_grayscale);
        r += (luma - r) * amount;
        g += (luma - g) * amount;
        b += (luma - b) * amount;
    }
    if (context.filter_sepia > 0.0f) {
        const float amount = ClampColorComponent(context.filter_sepia);
        const float sepia_r = 0.393f * r + 0.769f * g + 0.189f * b;
        const float sepia_g = 0.349f * r + 0.686f * g + 0.168f * b;
        const float sepia_b = 0.272f * r + 0.534f * g + 0.131f * b;
        r += (sepia_r - r) * amount;
        g += (sepia_g - g) * amount;
        b += (sepia_b - b) * amount;
    }
    if (context.filter_invert > 0.0f) {
        const float amount = ClampColorComponent(context.filter_invert);
        r += (1.0f - 2.0f * r) * amount;
        g += (1.0f - 2.0f * g) * amount;
        b += (1.0f - 2.0f * b) * amount;
    }

    Color result;
    result.r = ClampColorComponent(r);
    result.g = ClampColorComponent(g);
    result.b = ClampColorComponent(b);
    result.a = color.a;
    return result;
}

// 按累计透明度与滤镜合成颜色。
Color ApplyOpacity(const Color& color, const PaintContext& context) {
    Color result = ApplyFilter(color, context);
    if (context.opacity >= 1.0f) {
        return result;
    }
    result.a = result.a * (context.opacity < 0.0f ? 0.0f : context.opacity);
    return result;
}

// 把当前文档坐标换算为页面坐标（页面左上角为原点）。
RectF ToPageRect(const RectF& rect, const PaintContext& context) {
    return rect.Offset(context.offset_x, context.offset_y - context.page_top);
}

// 圆角半径不超过矩形短边的一半，避免后端产生无效几何。
float ClampRadius(const RectF& rect, float radius) {
    if (radius <= 0.0f || rect.IsEmpty()) {
        return 0.0f;
    }
    const float limit = std::min(rect.Width(), rect.Height()) * 0.5f;
    return std::min(radius, limit);
}

// 判断矩形在竖直方向上是否与页面区间重叠。
bool IntersectsPage(const RectF& rect, float page_top, float page_bottom) {
    return rect.bottom > page_top && rect.top < page_bottom;
}

// 把 CSS 长度解析为像素。百分比、视口单位等含外部参照的单位在没有参照时使用回退值。
float ResolveLength(const CssLength& length, float font_size, float fallback) {
    switch (length.unit) {
    case LengthUnit::Px:
    case LengthUnit::Number:
        return length.value;
    case LengthUnit::Em:
        return length.value * font_size;
    case LengthUnit::Rem:
        // 根元素字号不在本单元上下文内，按当前字号换算。
        return length.value * font_size;
    case LengthUnit::Pt:
        return length.value * (96.0f / 72.0f);
    case LengthUnit::Pc:
        return length.value * 16.0f;
    case LengthUnit::In:
        return length.value * 96.0f;
    case LengthUnit::Cm:
        return length.value * (96.0f / 2.54f);
    case LengthUnit::Mm:
        return length.value * (96.0f / 25.4f);
    default:
        return fallback;
    }
}

// 解析 clip-path 内缩量：百分比相对参照盒宽（水平）或高（垂直）。
float ResolveClipInset(const CssLength& length, float base, float font_size) {
    switch (length.unit) {
    case LengthUnit::Percent:
        return base * length.value / 100.0f;
    case LengthUnit::CalcPercent:
        return base * length.value / 100.0f + length.offset;
    case LengthUnit::Em:
        return length.value * font_size;
    case LengthUnit::Rem:
        return length.value * 16.0f;
    case LengthUnit::Px:
    case LengthUnit::Number:
        return length.value;
    default:
        return 0.0f;
    }
}

BorderWidths StyleBorderWidths(const ComputedStyle& style) {
    BorderWidths widths;
    widths.top = ResolveLength(style.border_width.top, style.font_size, 0.0f);
    widths.right = ResolveLength(style.border_width.right, style.font_size, 0.0f);
    widths.bottom = ResolveLength(style.border_width.bottom, style.font_size, 0.0f);
    widths.left = ResolveLength(style.border_width.left, style.font_size, 0.0f);
    return widths;
}

BorderColors StyleBorderColors(const ComputedStyle& style) {
    BorderColors colors;
    colors.top = style.border_color_top;
    colors.right = style.border_color_right;
    colors.bottom = style.border_color_bottom;
    colors.left = style.border_color_left;
    return colors;
}

// 读取样式中的四条线型；未声明线型时按实线绘制（兼容仅设置 border 颜色的旧页面）。
BorderStyles StyleBorderStyles(const ComputedStyle& style) {
    BorderStyles styles;
    styles.top = style.border_style_top;
    styles.right = style.border_style_right;
    styles.bottom = style.border_style_bottom;
    styles.left = style.border_style_left;
    // 旧页面可能只写 border: 1px solid，若线型为 None 但有宽度与颜色，按实线绘制。
    auto resolve = [](BorderStyle line_style, const CssLength& width, const Color& color) {
        if (line_style != BorderStyle::None) {
            return line_style;
        }
        if (width.value > 0.0f && color.Visible()) {
            return BorderStyle::Solid;
        }
        return BorderStyle::None;
    };
    styles.top = resolve(styles.top, style.border_width.top, style.border_color_top);
    styles.right = resolve(styles.right, style.border_width.right, style.border_color_right);
    styles.bottom = resolve(styles.bottom, style.border_width.bottom, style.border_color_bottom);
    styles.left = resolve(styles.left, style.border_width.left, style.border_color_left);
    return styles;
}

// 布局盒已解析出像素边框宽度；若布局未填写而样式中有边框，则按样式长度解析。
BorderWidths BoxBorderWidths(const LayoutBox& box) {
    BorderWidths widths;
    widths.top = box.border_top;
    widths.right = box.border_right;
    widths.bottom = box.border_bottom;
    widths.left = box.border_left;
    if (!box.HasBorder()) {
        return StyleBorderWidths(box.style);
    }
    return widths;
}

/* ----------------------------------------------------------------
   渐变与阴影绘制
   ---------------------------------------------------------------- */

// 渐变条带数量：固定值保证两种后端得到完全相同的几何与颜色。
constexpr int kGradientBands = 96;

Color MixColor(const Color& from, const Color& to, float ratio) {
    const float t = ratio < 0.0f ? 0.0f : (ratio > 1.0f ? 1.0f : ratio);
    Color result;
    result.r = from.r + (to.r - from.r) * t;
    result.g = from.g + (to.g - from.g) * t;
    result.b = from.b + (to.b - from.b) * t;
    result.a = from.a + (to.a - from.a) * t;
    return result;
}

// 色标按偏移排序并补成 0~1 的单调序列；未指定位置时按顺序均分。
std::vector<GradientStop> NormalizeStops(const std::vector<GradientStop>& stops) {
    std::vector<GradientStop> result = stops;
    const size_t count = result.size();
    if (count == 0) {
        return result;
    }
    for (size_t index = 0; index < count; ++index) {
        if (!result[index].has_offset) {
            result[index].offset = count > 1 ? static_cast<float>(index) /
                                                   static_cast<float>(count - 1)
                                             : 0.0f;
            result[index].has_offset = true;
        }
    }
    for (size_t index = 1; index < count; ++index) {
        if (result[index].offset < result[index - 1].offset) {
            result[index].offset = result[index - 1].offset;
        }
    }
    return result;
}

Color SampleGradient(const std::vector<GradientStop>& stops, float position) {
    if (stops.empty()) {
        return Transparent();
    }
    if (position <= stops.front().offset) {
        return stops.front().color;
    }
    if (position >= stops.back().offset) {
        return stops.back().color;
    }
    for (size_t index = 1; index < stops.size(); ++index) {
        if (position <= stops[index].offset) {
            const float span = stops[index].offset - stops[index - 1].offset;
            const float ratio = span > 0.0f ? (position - stops[index - 1].offset) / span : 0.0f;
            return MixColor(stops[index - 1].color, stops[index].color, ratio);
        }
    }
    return stops.back().color;
}

// 圆角内缩量：条带中心距上下（或左右）边界 d 小于半径时，边界按圆的方程内缩。
float CornerInset(float center, float near_edge, float far_edge, float radius) {
    if (radius <= 0.0f) {
        return 0.0f;
    }
    const float distance = std::min(center - near_edge, far_edge - center);
    if (distance >= radius) {
        return 0.0f;
    }
    const float remaining = radius - distance;
    const float span = radius * radius - remaining * remaining;
    if (span <= 0.0f) {
        return radius;
    }
    return radius - std::sqrt(span);
}

// 线性渐变：按主轴方向的归一化位置取色，用等宽条带填充。
// 主轴取投影跨度更大的一轴；条带边界对齐整数像素并让圆角边界内缩，
// 因此相邻条带无缝隙，整体轮廓保持圆角。
void PaintLinearGradient(RenderDevice& device, const RectF& rect,
                         const BackgroundImage& image, float radius, float opacity) {
    std::vector<GradientStop> stops = NormalizeStops(image.stops);
    if (stops.empty()) {
        return;
    }
    const float radians = image.angle_deg * 3.14159265f / 180.0f;
    // 方向向量：0 度指向上（屏幕坐标 y 向下，故 dy 为负）。
    const float dx = std::sin(radians);
    const float dy = -std::cos(radians);
    const float width = rect.Width();
    const float height = rect.Height();
    if (width <= 0.0f || height <= 0.0f) {
        return;
    }
    const float clamped_radius = ClampRadius(rect, radius);
    const float center_x = (rect.left + rect.right) * 0.5f;
    const float center_y = (rect.top + rect.bottom) * 0.5f;
    // 竖直条带：沿 x 轴变化（更接近左右渐变）；否则水平条带（上下渐变）。
    const bool vertical_bands = std::fabs(dx) * width >= std::fabs(dy) * height;

    for (int band = 0; band < kGradientBands; ++band) {
        const float t0 = static_cast<float>(band) / static_cast<float>(kGradientBands);
        const float t1 = static_cast<float>(band + 1) / static_cast<float>(kGradientBands);
        Color color = Transparent();
        RectF band_rect;
        float inset = 0.0f;
        if (!vertical_bands) {
            const float y0 = band == 0 ? rect.top
                                       : std::floor(rect.top + height * t0);
            const float y1 = band + 1 == kGradientBands
                                 ? rect.bottom
                                 : std::floor(rect.top + height * t1);
            const float ratio = ((y0 + y1) * 0.5f - center_y) / height;
            const float sign = dy >= 0.0f ? 1.0f : -1.0f;
            color = SampleGradient(stops, 0.5f + ratio * sign);
            band_rect = RectF::FromLTRB(rect.left, y0, rect.right, y1);
            inset = CornerInset((y0 + y1) * 0.5f, rect.top, rect.bottom, clamped_radius);
        } else {
            const float x0 = band == 0 ? rect.left
                                       : std::floor(rect.left + width * t0);
            const float x1 = band + 1 == kGradientBands
                                 ? rect.right
                                 : std::floor(rect.left + width * t1);
            const float ratio = ((x0 + x1) * 0.5f - center_x) / width;
            const float sign = dx >= 0.0f ? 1.0f : -1.0f;
            color = SampleGradient(stops, 0.5f + ratio * sign);
            band_rect = RectF::FromLTRB(x0, rect.top, x1, rect.bottom);
            inset = CornerInset((x0 + x1) * 0.5f, rect.left, rect.right, clamped_radius);
        }
        if (!color.Visible()) {
            continue;
        }
        color.a *= opacity;
        if (inset > 0.0f) {
            if (!vertical_bands) {
                band_rect.left += inset;
                band_rect.right -= inset;
            } else {
                band_rect.top += inset;
                band_rect.bottom -= inset;
            }
            if (band_rect.IsEmpty()) {
                continue;
            }
        }
        device.FillRect(band_rect, color);
    }
}

// 径向渐变：由外向内叠加同心矩形，形成近似径向过渡；每层沿用盒子的圆角。
void PaintRadialGradient(RenderDevice& device, const RectF& rect,
                         const BackgroundImage& image, float radius, float opacity) {
    std::vector<GradientStop> stops = NormalizeStops(image.stops);
    if (stops.empty()) {
        return;
    }
    const float center_x = rect.left + rect.Width() * image.center_x;
    const float center_y = rect.top + rect.Height() * image.center_y;
    // 外接半径：取到四边的最远距离。
    const float extent = std::max(std::max(std::fabs(rect.right - center_x),
                                           std::fabs(center_x - rect.left)),
                                  std::max(std::fabs(rect.bottom - center_y),
                                           std::fabs(center_y - rect.top)));
    if (extent <= 0.0f) {
        return;
    }
    const float clamped_radius = ClampRadius(rect, radius);
    for (int band = kGradientBands - 1; band >= 0; --band) {
        const float t0 = static_cast<float>(band) / static_cast<float>(kGradientBands);
        const float t1 = static_cast<float>(band + 1) / static_cast<float>(kGradientBands);
        Color color = SampleGradient(stops, (t0 + t1) * 0.5f);
        if (!color.Visible()) {
            continue;
        }
        color.a *= opacity;
        const float band_radius = extent * t1;
        const RectF band_rect = RectF::FromLTRB(center_x - band_radius, center_y - band_radius,
                                                center_x + band_radius, center_y + band_radius);
        const RectF clipped = RectF::FromLTRB(std::max(band_rect.left, rect.left),
                                             std::max(band_rect.top, rect.top),
                                             std::min(band_rect.right, rect.right),
                                             std::min(band_rect.bottom, rect.bottom));
        if (clipped.IsEmpty()) {
            continue;
        }
        if (clamped_radius > 0.0f) {
            device.FillRoundedRect(clipped, clamped_radius, color);
        } else {
            device.FillRect(clipped, color);
        }
    }
}

// 背景渐变入口。
void PaintBackgroundImage(RenderDevice& device, const RectF& rect,
                          const BackgroundImage& image, float radius, float opacity) {
    if (rect.IsEmpty() || image.kind == BackgroundImage::Kind::None) {
        return;
    }
    if (image.kind == BackgroundImage::Kind::Linear) {
        PaintLinearGradient(device, rect, image, radius, opacity);
    } else {
        PaintRadialGradient(device, rect, image, radius, opacity);
    }
}

/* 背景图片绘制：按 background-size / position / repeat 计算目标矩形。
   图片句柄来自 context.image_cache，按相对文档基准目录解析。 */
void PaintBackgroundImageUrl(const LayoutResult* layout, const std::string& source,
                             const BackgroundSize& size, float position_x, float position_y,
                             BackgroundRepeat repeat, const RectF& rect,
                             const CornerRadii& radii, PaintContext& context) {
    if (rect.IsEmpty() || source.empty() || context.image_cache == nullptr) {
        return;
    }
    const ImageHandle handle = context.image_cache->Get(source);
    if (handle == 0) {
        return;
    }
    (void)layout;

    // 圆角裁剪：整图绘制后再用四角遮罩无法取到下层颜色，改用圆角条带逐行绘制图片。
    const float intrinsic_width = static_cast<float>(context.image_cache->Width(handle));
    const float intrinsic_height = static_cast<float>(context.image_cache->Height(handle));
    if (intrinsic_width <= 0.0f || intrinsic_height <= 0.0f) {
        return;
    }

    float tile_width = intrinsic_width;
    float tile_height = intrinsic_height;
    if (size.kind == BackgroundSizeKind::Cover || size.kind == BackgroundSizeKind::Contain) {
        const float scale_x = rect.Width() / intrinsic_width;
        const float scale_y = rect.Height() / intrinsic_height;
        const float scale = size.kind == BackgroundSizeKind::Cover ? std::max(scale_x, scale_y)
                                                                   : std::min(scale_x, scale_y);
        tile_width = intrinsic_width * scale;
        tile_height = intrinsic_height * scale;
    }

    const float origin_x = rect.left + (rect.Width() - tile_width) * position_x;
    const float origin_y = rect.top + (rect.Height() - tile_height) * position_y;

    // 平铺范围：repeat 全部时从盒子边界向外扩展到最近的可覆盖位置。
    const int repeat_columns = repeat == BackgroundRepeat::RepeatY ? 1 : -1;
    (void)repeat_columns;
    const bool tile_x = repeat == BackgroundRepeat::Repeat || repeat == BackgroundRepeat::RepeatX;
    const bool tile_y = repeat == BackgroundRepeat::Repeat || repeat == BackgroundRepeat::RepeatY;

    auto first_origin = [](float origin, float tile, float rect_start, bool repeat_tiles) {
        if (!repeat_tiles || tile <= 0.0f) {
            return origin;
        }
        float value = origin;
        while (value > rect_start) {
            value -= tile;
        }
        while (value + tile <= rect_start) {
            value += tile;
        }
        return value;
    };
    const float start_x = first_origin(origin_x, tile_width, rect.left, tile_x);
    const float start_y = first_origin(origin_y, tile_height, rect.top, tile_y);

    for (float y = start_y; y < rect.bottom; y += tile_height) {
        if (!tile_y && y >= origin_y + tile_height) {
            break;
        }
        for (float x = start_x; x < rect.right; x += tile_width) {
            if (!tile_x && x >= origin_x + tile_width) {
                break;
            }
            const RectF tile = RectF::FromXYWH(x, y, tile_width, tile_height);
            if (tile.Width() <= 0.0f || tile.Height() <= 0.0f) {
                continue;
            }
            // 超出盒子边界的图块只绘制相交部分，纹理坐标按相交区域换算，
            // 否则边缘图块会被压缩变形。
            const RectF target = RectF::FromLTRB(std::max(tile.left, rect.left),
                                                 std::max(tile.top, rect.top),
                                                 std::min(tile.right, rect.right),
                                                 std::min(tile.bottom, rect.bottom));
            if (target.IsEmpty()) {
                continue;
            }
            const RectF uv = RectF::FromLTRB((target.left - tile.left) / tile.Width(),
                                             (target.top - tile.top) / tile.Height(),
                                             (target.right - tile.left) / tile.Width(),
                                             (target.bottom - tile.top) / tile.Height());
            const float radius = ClampRadius(target, radii.Maximum());
            context.device->DrawImageSubRect(handle, target, uv, context.opacity, radius);
        }
        if (tile_height <= 0.0f) {
            break;
        }
    }
}

// 盒阴影：按扩散后的矩形与模糊半径叠加多层，模拟柔和边缘。
// draw_inset 为 true 时绘制内阴影（应画在背景之上），false 时只画外阴影（画在背景之下）。
void PaintBoxShadows(RenderDevice& device, const RectF& rect, const CornerRadii& radii,
                     const std::vector<Shadow>& shadows, float opacity, bool draw_inset) {
    for (const Shadow& shadow : shadows) {
        if (!shadow.color.Visible() || shadow.inset != draw_inset) {
            continue;
        }
        Color color = shadow.color;
        color.a *= opacity;
        if (shadow.inset) {
            const float spread = std::max(0.0f, shadow.spread);
            const int layers = shadow.blur > 0.0f
                                   ? static_cast<int>(std::min(8.0f, std::floor(shadow.blur))) + 1
                                   : 1;
            for (int layer = 0; layer < layers; ++layer) {
                const float grow = shadow.blur * static_cast<float>(layer) /
                                   static_cast<float>(layers);
                Color layer_color = color;
                layer_color.a *= 1.0f / static_cast<float>(layers);
                const float inset = spread + grow + std::max(std::fabs(shadow.offset_x),
                                                             std::fabs(shadow.offset_y));
                const RectF band = RectF::FromLTRB(rect.left + shadow.offset_x + inset,
                                                  rect.top + shadow.offset_y + inset,
                                                  rect.right + shadow.offset_x - inset,
                                                  rect.bottom + shadow.offset_y - inset);
                if (band.IsEmpty()) {
                    continue;
                }
                // 用四条内缘色带近似内阴影，宽度为 inset。圆角盒改用圆角描边，
                // 使内阴影沿圆角内缘收拢。
                const float thickness = inset;
                if (!radii.Empty()) {
                    const float radius =
                        std::min(radii.Maximum(), std::min(rect.Width(), rect.Height()) * 0.5f);
                    device.StrokeRoundedRect(rect, radius, thickness, layer_color);
                    continue;
                }
                device.FillRect(RectF::FromLTRB(rect.left, rect.top, rect.right,
                                                rect.top + thickness),
                                layer_color);
                device.FillRect(RectF::FromLTRB(rect.left, rect.bottom - thickness, rect.right,
                                                rect.bottom),
                                layer_color);
                device.FillRect(RectF::FromLTRB(rect.left, rect.top + thickness,
                                                rect.left + thickness, rect.bottom - thickness),
                                layer_color);
                device.FillRect(RectF::FromLTRB(rect.right - thickness, rect.top + thickness,
                                                rect.right, rect.bottom - thickness),
                                layer_color);
            }
            continue;
        }
        const float spread_radius = radii.Maximum() + shadow.spread;
        const float blur_extent = std::max(0.0f, shadow.blur);
        // 模糊层的半径从阴影轮廓向外扩展 blur_extent，层数取扩展量的两倍，
        // 使每一层的间距不超过半个像素，边缘过渡连续。
        const int blur_layers =
            blur_extent > 0.0f
                ? static_cast<int>(std::min(64.0f, std::ceil(blur_extent * 2.0f))) + 1
                : 1;
        // 单层不透明度按叠加关系反解：从外层到内层累计 blur_layers 次后
        // 接近阴影本身的不透明度，避免最外层出现等高的实心色带。
        float layer_alpha = color.a;
        if (blur_layers > 1) {
            const float clamped_alpha = std::min(1.0f, std::max(0.0f, color.a));
            layer_alpha =
                1.0f - std::pow(1.0f - clamped_alpha, 1.0f / static_cast<float>(blur_layers));
        }
        for (int layer = 0; layer < blur_layers; ++layer) {
            const float ratio = blur_layers > 1
                                    ? static_cast<float>(layer) / static_cast<float>(blur_layers - 1)
                                    : 1.0f;
            const float grow = blur_extent * 2.0f * (1.0f - ratio);
            Color layer_color = color;
            layer_color.a = layer_alpha;
            const RectF shadow_rect = RectF::FromLTRB(
                rect.left + shadow.offset_x - grow * 0.5f,
                rect.top + shadow.offset_y - grow * 0.5f,
                rect.right + shadow.offset_x + grow * 0.5f,
                rect.bottom + shadow.offset_y + grow * 0.5f);
            if (shadow_rect.IsEmpty()) {
                continue;
            }
            const float clamped = ClampRadius(shadow_rect, spread_radius + grow * 0.5f);
            if (clamped > 0.0f) {
                device.FillRoundedRect(shadow_rect, clamped, layer_color);
            } else {
                device.FillRect(shadow_rect, layer_color);
            }
        }
    }
}

/* 椭圆角内缩量：distance 为条带中心到该角水平边的距离，radius_x/radius_y 为该角
   水平与垂直半径。圆角是 radius_x == radius_y 的特例。 */
float EllipseInset(float distance, float radius_x, float radius_y) {
    if (radius_x <= 0.0f || radius_y <= 0.0f || distance >= radius_y) {
        return 0.0f;
    }
    const float remaining = radius_y - distance;
    const float ratio = remaining / radius_y;
    const float span = 1.0f - ratio * ratio;
    if (span <= 0.0f) {
        return radius_x;
    }
    return radius_x * (1.0f - std::sqrt(span));
}

/* 按 CSS 规则等比缩小四角半径：相邻两角半径之和超过边长时，全部半径乘以同一比例，
   保证圆角彼此不重叠。 */
CornerRadii ClampCornerRadii(const RectF& rect, const CornerRadii& radii) {
    const float width = rect.Width();
    const float height = rect.Height();
    if (width <= 0.0f || height <= 0.0f) {
        return CornerRadii();
    }

    float scale = 1.0f;
    auto limit = [&scale](float sum, float extent) {
        if (sum > extent && sum > 0.0f) {
            scale = std::min(scale, extent / sum);
        }
    };
    limit(radii.top_left_x + radii.top_right_x, width);
    limit(radii.bottom_left_x + radii.bottom_right_x, width);
    limit(radii.top_left_y + radii.bottom_left_y, height);
    limit(radii.top_right_y + radii.bottom_right_y, height);
    if (scale >= 1.0f) {
        return radii;
    }

    CornerRadii result;
    result.top_left_x = radii.top_left_x * scale;
    result.top_left_y = radii.top_left_y * scale;
    result.top_right_x = radii.top_right_x * scale;
    result.top_right_y = radii.top_right_y * scale;
    result.bottom_right_x = radii.bottom_right_x * scale;
    result.bottom_right_y = radii.bottom_right_y * scale;
    result.bottom_left_x = radii.bottom_left_x * scale;
    result.bottom_left_y = radii.bottom_left_y * scale;
    return result;
}

/* 四角独立圆角的实心填充。按整数像素行走扫描线：相邻行如果左右内缩相同就合并为
   一块矩形，既保证圆角外轮廓逐行精确，也避免整块填充与逐行填充在像素边界
   重叠导致颜色变浅。 */
void FillCornersRect(RenderDevice& device, const RectF& rect, const CornerRadii& raw_radii,
                     const Color& color) {
    const float width = rect.Width();
    const float height = rect.Height();
    if (width <= 0.0f || height <= 0.0f || !color.Visible()) {
        return;
    }

    const CornerRadii radii = ClampCornerRadii(rect, raw_radii);
    if (radii.Empty()) {
        device.FillRect(rect, color);
        return;
    }

    const int first_row = static_cast<int>(std::floor(rect.top));
    const int last_row = static_cast<int>(std::ceil(rect.bottom));
    bool run_open = false;
    float run_top = 0.0f;
    float run_bottom = 0.0f;
    float run_left = 0.0f;
    float run_right = 0.0f;
    auto flush_run = [&]() {
        if (!run_open) {
            return;
        }
        const RectF run_rect = RectF::FromLTRB(rect.left + run_left, run_top,
                                               rect.right - run_right, run_bottom);
        if (!run_rect.IsEmpty()) {
            device.FillRect(run_rect, color);
        }
        run_open = false;
    };

    for (int row = first_row; row < last_row; ++row) {
        const float row_top = std::max(rect.top, static_cast<float>(row));
        const float row_bottom = std::min(rect.bottom, static_cast<float>(row + 1));
        if (row_bottom <= row_top) {
            continue;
        }
        const float center = (row_top + row_bottom) * 0.5f;
        const float distance_top = center - rect.top;
        const float distance_bottom = rect.bottom - center;
        const float left_inset = std::max(
            EllipseInset(distance_top, radii.top_left_x, radii.top_left_y),
            EllipseInset(distance_bottom, radii.bottom_left_x, radii.bottom_left_y));
        const float right_inset = std::max(
            EllipseInset(distance_top, radii.top_right_x, radii.top_right_y),
            EllipseInset(distance_bottom, radii.bottom_right_x, radii.bottom_right_y));

        const bool same_run = run_open && std::fabs(left_inset - run_left) <= 0.01f &&
                              std::fabs(right_inset - run_right) <= 0.01f &&
                              std::fabs(row_top - run_bottom) <= 0.01f;
        if (same_run) {
            run_bottom = row_bottom;
        } else {
            flush_run();
            run_open = true;
            run_top = row_top;
            run_bottom = row_bottom;
            run_left = left_inset;
            run_right = right_inset;
        }
    }
    flush_run();
}

/* 解析单个角的半径分量：百分比相对参照长度换算，再叠加 calc() 的像素偏移。 */
float ResolveCornerComponent(float value, float offset, bool percent, float base) {
    return (percent ? base * value * 0.01f : value) + offset;
}

/* 读取样式中的四角半径，并按边框盒尺寸解析百分比。 */
CornerRadii StyleCornerRadii(const ComputedStyle& style, const RectF& rect) {
    const float width = rect.Width();
    const float height = rect.Height();
    CornerRadii radii;
    radii.top_left_x = ResolveCornerComponent(
        style.border_radius_top_left.horizontal, style.border_radius_top_left.horizontal_offset,
        style.border_radius_top_left.horizontal_percent, width);
    radii.top_left_y = ResolveCornerComponent(
        style.border_radius_top_left.vertical, style.border_radius_top_left.vertical_offset,
        style.border_radius_top_left.vertical_percent, height);
    radii.top_right_x = ResolveCornerComponent(
        style.border_radius_top_right.horizontal, style.border_radius_top_right.horizontal_offset,
        style.border_radius_top_right.horizontal_percent, width);
    radii.top_right_y = ResolveCornerComponent(
        style.border_radius_top_right.vertical, style.border_radius_top_right.vertical_offset,
        style.border_radius_top_right.vertical_percent, height);
    radii.bottom_right_x = ResolveCornerComponent(
        style.border_radius_bottom_right.horizontal,
        style.border_radius_bottom_right.horizontal_offset,
        style.border_radius_bottom_right.horizontal_percent, width);
    radii.bottom_right_y = ResolveCornerComponent(
        style.border_radius_bottom_right.vertical, style.border_radius_bottom_right.vertical_offset,
        style.border_radius_bottom_right.vertical_percent, height);
    radii.bottom_left_x = ResolveCornerComponent(
        style.border_radius_bottom_left.horizontal,
        style.border_radius_bottom_left.horizontal_offset,
        style.border_radius_bottom_left.horizontal_percent, width);
    radii.bottom_left_y = ResolveCornerComponent(
        style.border_radius_bottom_left.vertical, style.border_radius_bottom_left.vertical_offset,
        style.border_radius_bottom_left.vertical_percent, height);
    return ClampCornerRadii(rect, radii);
}

void PaintBackgroundFill(RenderDevice& device, const RectF& rect, const Color& color,
                         const CornerRadii& radii) {
    if (rect.IsEmpty() || !color.Visible()) {
        return;
    }
    FillCornersRect(device, rect, radii, color);
}

void PaintBackgroundFill(RenderDevice& device, const RectF& rect, const Color& color,
                         float radius) {
    CornerRadii radii;
    radii.top_left_x = radius;
    radii.top_left_y = radius;
    radii.top_right_x = radius;
    radii.top_right_y = radius;
    radii.bottom_right_x = radius;
    radii.bottom_right_y = radius;
    radii.bottom_left_x = radius;
    radii.bottom_left_y = radius;
    PaintBackgroundFill(device, rect, color, radii);
}

/* 按线型绘制一条边：实线整块填充，虚线/点线按段铺设，双线拆成两条细线。 */
void PaintEdgeLine(RenderDevice& device, const RectF& edge, bool horizontal, BorderStyle style,
                   const Color& color) {
    if (edge.IsEmpty() || !color.Visible() || style == BorderStyle::None) {
        return;
    }
    const float thickness = horizontal ? edge.Height() : edge.Width();
    if (thickness <= 0.0f) {
        return;
    }
    if (style == BorderStyle::Double) {
        const float thin = std::max(1.0f, thickness / 3.0f);
        if (horizontal) {
            device.FillRect(RectF::FromLTRB(edge.left, edge.top, edge.right, edge.top + thin),
                            color);
            device.FillRect(RectF::FromLTRB(edge.left, edge.bottom - thin, edge.right, edge.bottom),
                            color);
        } else {
            device.FillRect(RectF::FromLTRB(edge.left, edge.top, edge.left + thin, edge.bottom),
                            color);
            device.FillRect(RectF::FromLTRB(edge.right - thin, edge.top, edge.right, edge.bottom),
                            color);
        }
        return;
    }

    float dash_length = 0.0f;
    float gap_length = 0.0f;
    if (style == BorderStyle::Dashed) {
        dash_length = thickness * 3.0f;
        gap_length = thickness * 2.0f;
    } else if (style == BorderStyle::Dotted) {
        dash_length = thickness;
        gap_length = thickness * 1.5f;
    } else {
        device.FillRect(edge, color);
        return;
    }

    const float start = horizontal ? edge.left : edge.top;
    const float length = horizontal ? edge.Width() : edge.Height();
    for (float offset = 0.0f; offset < length; offset += dash_length + gap_length) {
        const float segment_end = std::min(offset + dash_length, length);
        if (horizontal) {
            device.FillRect(
                RectF::FromLTRB(start + offset, edge.top, start + segment_end, edge.bottom), color);
        } else {
            device.FillRect(
                RectF::FromLTRB(edge.left, start + offset, edge.right, start + segment_end), color);
        }
    }
}

// 圆角为 0 时用四条矩形绘制，四边颜色、宽度与线型可以不同。
void PaintRectBorders(RenderDevice& device, const RectF& rect, const BorderWidths& widths,
                      const BorderColors& colors, const BorderStyles& styles) {
    if (rect.IsEmpty()) {
        return;
    }

    const float width = rect.Width();
    const float height = rect.Height();

    if (widths.top > 0.0f && colors.top.Visible()) {
        const float clipped = std::min(widths.top, height);
        PaintEdgeLine(device,
                      RectF::FromLTRB(rect.left, rect.top, rect.right, rect.top + clipped), true,
                      styles.top, colors.top);
    }
    if (widths.bottom > 0.0f && colors.bottom.Visible()) {
        const float clipped = std::min(widths.bottom, height);
        PaintEdgeLine(device,
                      RectF::FromLTRB(rect.left, rect.bottom - clipped, rect.right, rect.bottom),
                      true, styles.bottom, colors.bottom);
    }
    if (widths.left > 0.0f && colors.left.Visible()) {
        const float strip_width = std::min(widths.left, width);
        const float strip_top = rect.top + std::min(widths.top, height);
        const float strip_bottom = rect.bottom - std::min(widths.bottom, height);
        if (strip_bottom > strip_top) {
            PaintEdgeLine(
                device, RectF::FromLTRB(rect.left, strip_top, rect.left + strip_width,
                                        strip_bottom),
                false, styles.left, colors.left);
        }
    }
    if (widths.right > 0.0f && colors.right.Visible()) {
        const float strip_width = std::min(widths.right, width);
        const float strip_top = rect.top + std::min(widths.top, height);
        const float strip_bottom = rect.bottom - std::min(widths.bottom, height);
        if (strip_bottom > strip_top) {
            PaintEdgeLine(
                device, RectF::FromLTRB(rect.right - strip_width, strip_top, rect.right,
                                        strip_bottom),
                false, styles.right, colors.right);
        }
    }
}

/* 内轮廓半径：CSS 规定内半径 = 外半径 - 对应边框宽度，负值取 0。
   四条边的宽度分别作用于左右、上下的半径分量。 */
CornerRadii InnerCornerRadii(const CornerRadii& radii, const BorderWidths& widths) {
    CornerRadii inner;
    inner.top_left_x = std::max(0.0f, radii.top_left_x - widths.left);
    inner.top_left_y = std::max(0.0f, radii.top_left_y - widths.top);
    inner.top_right_x = std::max(0.0f, radii.top_right_x - widths.right);
    inner.top_right_y = std::max(0.0f, radii.top_right_y - widths.top);
    inner.bottom_right_x = std::max(0.0f, radii.bottom_right_x - widths.right);
    inner.bottom_right_y = std::max(0.0f, radii.bottom_right_y - widths.bottom);
    inner.bottom_left_x = std::max(0.0f, radii.bottom_left_x - widths.left);
    inner.bottom_left_y = std::max(0.0f, radii.bottom_left_y - widths.bottom);
    return inner;
}

/* 逐行绘制圆角描边环。外轮廓按椭圆角内缩；内轮廓取边框盒向内收缩后的矩形，
   半径按对应边宽同步收缩（CSS 规则：内半径 = 外半径 - 边框宽度）。
   用于四角半径不同或含椭圆角的边框，外轮廓与填充背景使用同一套圆角几何。
   按整数像素行绘制并合并内缩相同的相邻行，避免行边界重叠使边框色变浅。 */
void PaintRoundedRing(RenderDevice& device, const RectF& rect, const BorderWidths& widths,
                      const CornerRadii& radii, const Color& color) {
    const RectF inner =
        RectF::FromLTRB(rect.left + widths.left, rect.top + widths.top,
                        rect.right - widths.right, rect.bottom - widths.bottom);

    CornerRadii inner_radii = InnerCornerRadii(radii, widths);
    const bool has_inner = !inner.IsEmpty();
    if (has_inner) {
        inner_radii = ClampCornerRadii(inner, inner_radii);
    }

    // 单行的四个边界，inner_left/inner_right 在外轮廓整行都是边框时取外轮廓位置。
    struct RingRow {
        float left = 0.0f;
        float inner_left = 0.0f;
        float inner_right = 0.0f;
        float right = 0.0f;
        bool full = false;
    };
    auto row_at = [&](float center) {
        RingRow row;
        row.left = rect.left +
                   std::max(EllipseInset(center - rect.top, radii.top_left_x, radii.top_left_y),
                            EllipseInset(rect.bottom - center, radii.bottom_left_x,
                                         radii.bottom_left_y));
        row.right = rect.right -
                    std::max(EllipseInset(center - rect.top, radii.top_right_x,
                                          radii.top_right_y),
                             EllipseInset(rect.bottom - center, radii.bottom_right_x,
                                          radii.bottom_right_y));
        row.full = true;
        if (!has_inner || center <= inner.top || center >= inner.bottom) {
            return row;
        }
        row.inner_left =
            inner.left + std::max(EllipseInset(center - inner.top, inner_radii.top_left_x,
                                               inner_radii.top_left_y),
                                  EllipseInset(inner.bottom - center,
                                               inner_radii.bottom_left_x,
                                               inner_radii.bottom_left_y));
        row.inner_right =
            inner.right - std::max(EllipseInset(center - inner.top, inner_radii.top_right_x,
                                                inner_radii.top_right_y),
                                   EllipseInset(inner.bottom - center,
                                                inner_radii.bottom_right_x,
                                                inner_radii.bottom_right_y));
        row.full = row.inner_left >= row.inner_right;
        return row;
    };

    const int first_row = static_cast<int>(std::floor(rect.top));
    const int last_row = static_cast<int>(std::ceil(rect.bottom));
    bool run_open = false;
    float run_top = 0.0f;
    float run_bottom = 0.0f;
    RingRow run{};
    auto flush_run = [&]() {
        if (!run_open) {
            return;
        }
        if (run.full) {
            const RectF band = RectF::FromLTRB(run.left, run_top, run.right, run_bottom);
            if (!band.IsEmpty()) {
                device.FillRect(band, color);
            }
        } else {
            if (run.inner_left > run.left) {
                const RectF left_band =
                    RectF::FromLTRB(run.left, run_top, run.inner_left, run_bottom);
                if (!left_band.IsEmpty()) {
                    device.FillRect(left_band, color);
                }
            }
            if (run.inner_right < run.right) {
                const RectF right_band =
                    RectF::FromLTRB(run.inner_right, run_top, run.right, run_bottom);
                if (!right_band.IsEmpty()) {
                    device.FillRect(right_band, color);
                }
            }
        }
        run_open = false;
    };

    for (int index = first_row; index < last_row; ++index) {
        const float row_top = std::max(rect.top, static_cast<float>(index));
        const float row_bottom = std::min(rect.bottom, static_cast<float>(index + 1));
        if (row_bottom <= row_top) {
            continue;
        }
        const RingRow row = row_at((row_top + row_bottom) * 0.5f);
        const bool same_run =
            run_open && row.full == run.full &&
            std::fabs(row.left - run.left) <= 0.01f && std::fabs(row.right - run.right) <= 0.01f &&
            std::fabs(row.inner_left - run.inner_left) <= 0.01f &&
            std::fabs(row.inner_right - run.inner_right) <= 0.01f &&
            std::fabs(row_top - run_bottom) <= 0.01f;
        if (same_run) {
            run_bottom = row_bottom;
        } else {
            flush_run();
            run_open = true;
            run_top = row_top;
            run_bottom = row_bottom;
            run = row;
        }
    }
    flush_run();
}

// 边框绘制入口：圆角为 0 使用矩形拼接；圆角大于 0 且有效边颜色一致时绘制圆角描边。
// 四角半径一致的圆角交给后端着色器（边缘平滑），四角不同或含椭圆角时逐行绘制；
// 颜色不一致时用单色矩形描边近似。
void PaintBoxBorders(RenderDevice& device, const RectF& rect, const BorderWidths& widths,
                     const BorderColors& colors, const BorderStyles& styles,
                     const CornerRadii& raw_radii) {
    if (rect.IsEmpty()) {
        return;
    }

    const float max_width = std::max(std::max(widths.top, widths.right),
                                     std::max(widths.bottom, widths.left));
    if (max_width <= 0.0f) {
        return;
    }

    const CornerRadii radii = ClampCornerRadii(rect, raw_radii);
    // 有线型（虚线/点线/双线）时用矩形拼接，确保线型正确；此时圆角不参与描边。
    const bool has_non_solid = (widths.top > 0.0f && styles.top != BorderStyle::Solid) ||
                               (widths.right > 0.0f && styles.right != BorderStyle::Solid) ||
                               (widths.bottom > 0.0f && styles.bottom != BorderStyle::Solid) ||
                               (widths.left > 0.0f && styles.left != BorderStyle::Solid);
    if (radii.Empty() || has_non_solid) {
        PaintRectBorders(device, rect, widths, colors, styles);
        return;
    }

    // 只比较实际有宽度的边；零宽边的颜色不参与判断。
    bool same_color = true;
    bool has_reference = false;
    Color reference = Transparent();
    const float side_widths[4] = {widths.top, widths.right, widths.bottom, widths.left};
    const Color side_colors[4] = {colors.top, colors.right, colors.bottom, colors.left};
    for (size_t index = 0; index < 4; ++index) {
        if (side_widths[index] <= 0.0f) {
            continue;
        }
        if (!has_reference) {
            reference = side_colors[index];
            has_reference = true;
        } else if (side_colors[index] != reference) {
            same_color = false;
            break;
        }
    }
    if (!has_reference) {
        return;
    }

    if (same_color && reference.Visible()) {
        if (radii.UniformCircular()) {
            device.StrokeRoundedRect(rect, ClampRadius(rect, radii.Maximum()), max_width,
                                     reference);
        } else {
            PaintRoundedRing(device, rect, widths, radii, reference);
        }
        return;
    }

    Color fallback = Transparent();
    for (size_t index = 0; index < 4; ++index) {
        if (side_widths[index] > 0.0f && side_colors[index].Visible()) {
            fallback = side_colors[index];
            break;
        }
    }
    if (fallback.Visible()) {
        device.StrokeRect(rect, max_width, fallback);
    }
}

// 按 object-fit 与 object-position 计算图片在内容区内的实际绘制矩形。
RectF ObjectFitRect(const RectF& content, float natural_width, float natural_height,
                    ObjectFit fit, float position_x, float position_y) {
    if (content.IsEmpty() || natural_width <= 0.0f || natural_height <= 0.0f) {
        return content;
    }
    float scale = 1.0f;
    if (fit == ObjectFit::Fill) {
        return content;
    } else if (fit == ObjectFit::Contain) {
        scale = std::min(content.Width() / natural_width, content.Height() / natural_height);
    } else if (fit == ObjectFit::Cover) {
        scale = std::max(content.Width() / natural_width, content.Height() / natural_height);
    } else if (fit == ObjectFit::ScaleDown) {
        scale = std::min(1.0f, std::min(content.Width() / natural_width,
                                        content.Height() / natural_height));
    } else {
        scale = 1.0f;
    }
    const float width = natural_width * scale;
    const float height = natural_height * scale;
    const float left = content.left + (content.Width() - width) * position_x;
    const float top = content.top + (content.Height() - height) * position_y;
    return RectF::FromLTRB(left, top, left + width, top + height);
}

void PaintImageContent(const Element& element, const RectF& rect, const CornerRadii& radii,
                       const ComputedStyle& style, PaintContext& context) {
    if (rect.IsEmpty() || context.device == nullptr) {
        return;
    }

    const std::string source = element.GetAttribute("src");
    ImageHandle handle = 0;
    if (!source.empty() && context.image_cache != nullptr) {
        handle = context.image_cache->Get(source);
    }
    if (handle != 0) {
        RectF target = rect;
        if (context.image_cache != nullptr) {
            const int natural_width = context.image_cache->Width(handle);
            const int natural_height = context.image_cache->Height(handle);
            target = ObjectFitRect(rect, static_cast<float>(natural_width),
                                   static_cast<float>(natural_height), style.object_fit,
                                   style.object_position_x, style.object_position_y);
        }
        // cover 等模式会超出内容区：只绘制与内容区相交的部分，并把相交区域换算成
        // 纹理坐标范围。dest 因此始终落在内容区内，圆角裁剪不会出现直角残影。
        RectF dest = target;
        RectF uv = RectF::FromLTRB(0.0f, 0.0f, 1.0f, 1.0f);
        if (target.left < rect.left || target.top < rect.top || target.right > rect.right ||
            target.bottom > rect.bottom) {
            if (target.Width() <= 0.0f || target.Height() <= 0.0f) {
                return;
            }
            dest = RectF::FromLTRB(std::max(target.left, rect.left),
                                   std::max(target.top, rect.top),
                                   std::min(target.right, rect.right),
                                   std::min(target.bottom, rect.bottom));
            if (dest.IsEmpty()) {
                return;
            }
            uv.left = (dest.left - target.left) / target.Width();
            uv.top = (dest.top - target.top) / target.Height();
            uv.right = (dest.right - target.left) / target.Width();
            uv.bottom = (dest.bottom - target.top) / target.Height();
        }
        // 图片着色器只接受单一半径，四角不同时取最大半径。
        context.device->DrawImageSubRect(handle, dest, uv, 1.0f, radii.Maximum());
        return;
    }
    // 加载失败或没有 src 时绘制占位灰块，便于定位缺失资源。
    PaintBackgroundFill(*context.device, rect, Rgba(0.86f, 0.86f, 0.86f, 1.0f), radii);
}

// 画布内容：从画布注册表取回像素表面并作为纹理绘制，圆角按元素样式裁剪。
void PaintCanvasContent(const Element& element, const RectF& rect, const CornerRadii& radii,
                        PaintContext& context) {
    if (rect.IsEmpty() || context.device == nullptr || context.image_cache == nullptr) {
        return;
    }
    const CanvasSurface* surface = FindCanvasSurface(element);
    if (surface == nullptr) {
        return;
    }
    const ImageHandle handle = context.image_cache->GetCanvas(element, *surface);
    if (handle != 0) {
        // 图片着色器只接受单一半径，四角不同时取最大半径。
        context.device->DrawImage(handle, rect, 1.0f, radii.Maximum());
    }
}

// 递归绘制布局盒。paint_own_decoration 为 false 时跳过本盒的背景与边框；
// page_test 为 false 表示整盒属于已经绘制的行内原子盒，不再按页面边界过滤。
// ancestor_background 为当前盒背后最近的页面或祖先背景色，用于内嵌框架圆角遮罩。
void PaintLayoutBox(const LayoutBox& box, PaintContext& context, bool paint_own_decoration,
                    bool page_test, const Color& ancestor_background);

// 在矩形四角绘制 1/4 圆之外的遮罩，使内嵌内容不越出圆角。
void PaintCornerMask(RenderDevice& device, const RectF& rect, float radius, const Color& color) {
    if (rect.IsEmpty() || !color.Visible()) {
        return;
    }
    const float clamped_radius = ClampRadius(rect, radius);
    if (clamped_radius <= 0.0f) {
        return;
    }

    // 圆角半径为 r：圆心位于角内侧 (r, r)。距角顶 d 处的水平覆盖宽度为
    // r - sqrt(r² - (r - d)²)，逐行填充该宽度即可覆盖四角外侧区域。
    const int row_count = static_cast<int>(std::ceil(clamped_radius));
    const float radius_squared = clamped_radius * clamped_radius;
    for (int row = 0; row < row_count; ++row) {
        const float distance_from_center =
            clamped_radius - (static_cast<float>(row) + 0.5f);
        float covered_width = clamped_radius;
        if (distance_from_center > 0.0f) {
            const float remaining = radius_squared - distance_from_center * distance_from_center;
            if (remaining > 0.0f) {
                covered_width = clamped_radius - std::sqrt(remaining);
            } else {
                covered_width = 0.0f;
            }
        }
        if (covered_width <= 0.0f) {
            continue;
        }
        const float band_top = rect.top + static_cast<float>(row);
        const float band_bottom = band_top + 1.0f;
        // 左上与右上角按距顶行数镜像；左下与右下角从底边向上逐行填充。
        const float bottom_band_top = rect.bottom - static_cast<float>(row) - 1.0f;
        const float bottom_band_bottom = bottom_band_top + 1.0f;
        device.FillRect(RectF::FromLTRB(rect.left, band_top, rect.left + covered_width,
                                        band_bottom), color);
        device.FillRect(RectF::FromLTRB(rect.right - covered_width, band_top, rect.right,
                                        band_bottom), color);
        device.FillRect(RectF::FromLTRB(rect.left, bottom_band_top, rect.left + covered_width,
                                        bottom_band_bottom), color);
        device.FillRect(RectF::FromLTRB(rect.right - covered_width, bottom_band_top, rect.right,
                                        bottom_band_bottom), color);
    }
}

// 内嵌框架内容：在目标区域内裁剪绘制子文档布局。target 为 iframe 内容区在页面
// 坐标中的矩形；offset_x/offset_y 累加目标左上角，使子文档坐标落到页面坐标。
// 子布局缺失时保持调用方已绘制好的元素背景，相当于空白框架。
// corner_radius 为框架元素的圆角半径，用于绘制四角遮罩；ancestor_background 为
// 框架背后最近的页面或祖先背景色，遮罩以此覆盖子内容越出圆角的部分。
void PaintEmbeddedFrame(const Element& element, const RectF& target, const CornerRadii& radii,
                        const Color& ancestor_background, PaintContext& context) {
    if (context.device == nullptr || context.layout == nullptr || target.IsEmpty()) {
        return;
    }
    const auto found = context.layout->embedded_frames.find(&element);
    if (found == context.layout->embedded_frames.end() || found->second == nullptr) {
        return;
    }
    const LayoutResult& frame_layout = *found->second;
    if (!frame_layout.root) {
        return;
    }

    context.device->PushClip(target);

    // 子文档图片的相对路径以其自身基准目录解析；绘制结束后恢复顶层目录。
    std::string saved_base_directory;
    if (context.image_cache != nullptr) {
        saved_base_directory = context.image_cache->BaseDirectory();
        context.image_cache->SetBaseDirectory(frame_layout.base_directory);
    }

    // 子文档根背景铺满整个框架内容区，与顶层绘制时纸张底色的处理一致。
    if (frame_layout.root->HasBackground()) {
        PaintBackgroundFill(*context.device, target, frame_layout.root->style.background_color,
                            0.0f);
    }

    const float saved_page_top = context.page_top;
    const float saved_page_bottom = context.page_bottom;
    const float saved_offset_x = context.offset_x;
    const float saved_offset_y = context.offset_y;
    const LayoutResult* saved_layout = context.layout;

    context.page_top = 0.0f;
    context.page_bottom = frame_layout.content_height > 0.0f
                              ? frame_layout.content_height
                              : target.Height();
    if (context.page_bottom <= 0.0f) {
        context.page_bottom = 1.0f;
    }
    context.offset_x = saved_offset_x + target.left;
    context.offset_y = saved_offset_y + target.top;
    // 子布局持有自己的 iframe 映射表，绘制时切换到子布局。
    context.layout = &frame_layout;

    const Color frame_background = frame_layout.root->HasBackground()
                                       ? frame_layout.root->style.background_color
                                       : ancestor_background;
    PaintLayoutBox(*frame_layout.root, context, true, false, frame_background);

    context.layout = saved_layout;
    context.offset_y = saved_offset_y;
    context.offset_x = saved_offset_x;
    context.page_bottom = saved_page_bottom;
    context.page_top = saved_page_top;

    // 子内容绘制完成后用祖先背景覆盖四角圆弧之外的区域。
    // 遮罩只支持单一半径，四角不同时取最大半径。
    const Color mask_color = ancestor_background.Visible() ? ancestor_background : White();
    PaintCornerMask(*context.device, target, radii.Maximum(), mask_color);

    if (context.image_cache != nullptr) {
        context.image_cache->SetBaseDirectory(saved_base_directory);
    }

    context.device->PopClip();
}

// 前向声明：文字四边形生成在文件后部定义。
void AppendTextQuads(const PlacedText& placed, PaintContext& context,
                     std::vector<GlyphQuad>* quads, float dx = 0.0f, float dy = 0.0f,
                     const Color* color_override = nullptr);

// 文本类表单控件：文字由表单绘制路径自行绘制，需要按滚动偏移平移并裁剪。
// checkbox/radio 的勾选标记仍走通用行盒绘制，不属于此类。
bool IsTextFormControl(const LayoutBox& box) {
    if (box.element != nullptr && AsciiLower(box.element->TagName()) == "textarea") {
        return true;
    }
    switch (box.form_kind) {
        case FormControlKind::TextInput:
        case FormControlKind::PasswordInput:
        case FormControlKind::Select:
        case FormControlKind::Submit:
        case FormControlKind::Reset:
        case FormControlKind::Button:
            return true;
        default:
            return false;
    }
}

// 第 index 个码位的 UTF-8 字符；越界返回 length = 0。
Utf8Char Utf8CharAtIndex(const std::string& text, size_t index) {
    size_t offset = 0;
    size_t current = 0;
    while (offset < text.size()) {
        const Utf8Char character = DecodeUtf8(text, offset);
        if (character.length == 0) {
            break;
        }
        if (current == index) {
            return character;
        }
        offset += character.length;
        ++current;
    }
    return Utf8Char{};
}

// 从 begin_index 起测量 count 个码位的累计宽度，字距逐字符累加。
float CodepointRunWidth(const std::string& text, size_t begin_index, size_t count, FontId font,
                        float letter_spacing, TextService& text_service) {
    float width = 0.0f;
    for (size_t step = 0; step < count; ++step) {
        const Utf8Char character = Utf8CharAtIndex(text, begin_index + step);
        if (character.length == 0) {
            break;
        }
        width += text_service.Advance(font, character.codepoint) + letter_spacing;
    }
    return width;
}

// 文本控件实际使用的字体：优先取行盒中已排版的字体，缺失时按样式创建。
FontId FormFontOf(const LayoutBox& box, TextService& text_service) {
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

// 文本控件内容：按水平/垂直滚动偏移平移行盒后绘制，用内容盒裁剪。
// 聚焦时绘制选区高亮与光标；非聚焦时按零偏移绘制。
void PaintFormText(const LayoutBox& box, PaintContext& context) {
    if (context.device == nullptr || box.element == nullptr) {
        return;
    }
    const RectF content = ToPageRect(box.content_box, context);
    if (content.IsEmpty()) {
        return;
    }

    const bool focused = context.focus != nullptr && context.focus->element == box.element;
    const float scroll_x = focused ? context.focus->scroll_x : 0.0f;
    const float scroll_y = focused ? context.focus->scroll_y : 0.0f;

    context.device->PushClip(content);

    // 选区高亮：锚点到光标之间的每个码位按字体步进绘制半透明底色。
    if (focused && context.text_service != nullptr) {
        const size_t caret = context.focus->caret;
        const size_t anchor = context.focus->anchor;
        const size_t selection_begin = std::min(caret, anchor);
        const size_t selection_end = std::max(caret, anchor);
        if (selection_end > selection_begin) {
            const std::string text = FormControlText(*box.element);
            const std::string display = FormDisplayText(*box.element);
            const FontId font = FormFontOf(box, *context.text_service);
            const Color selection_color = Rgba(0.2f, 0.4f, 0.8f, 0.35f);
            for (size_t line_index = 0;; ++line_index) {
                const FormLineRange range =
                    GetFormLineRange(box, text, line_index, *context.text_service);
                if (!range.valid) {
                    break;
                }
                const size_t begin = std::max(selection_begin, range.begin_index);
                const size_t end = std::min(selection_end, range.end_index);
                if (begin < end) {
                    const float x0 = CodepointRunWidth(display, range.begin_index,
                                                      begin - range.begin_index, font,
                                                      box.style.letter_spacing,
                                                      *context.text_service);
                    const float x1 = CodepointRunWidth(display, range.begin_index,
                                                      end - range.begin_index, font,
                                                      box.style.letter_spacing,
                                                      *context.text_service);
                    const RectF highlight_doc = RectF::FromLTRB(
                        box.content_box.left + x0 - scroll_x, range.line_top - scroll_y,
                        box.content_box.left + x1 - scroll_x, range.line_bottom - scroll_y);
                    context.device->FillRect(ToPageRect(highlight_doc, context),
                                             selection_color);
                }
            }
        }
    }

    // 文字按滚动偏移平移后绘制；行盒本身保留用于字形预栅格化。
    std::vector<GlyphQuad> quads;
    for (const LineFragment& line : box.lines) {
        for (const PlacedText& placed : line.texts) {
            PlacedText shifted = placed;
            shifted.x = placed.x - scroll_x;
            shifted.baseline = placed.baseline - scroll_y;
            AppendTextQuads(shifted, context, &quads);
        }
    }
    /* 值为空时绘制 placeholder 占位文本。占位文本没有行盒，基线沿用首行；
       空控件没有行盒时按字号估算基线。 */
    const std::string tag_name = AsciiLower(box.element->TagName());
    const std::string placeholder = box.element->GetAttribute("placeholder");
    if (!placeholder.empty() && (tag_name == "textarea" || tag_name == "input") &&
        FormDisplayText(*box.element).empty()) {
        PlacedText hint;
        hint.text = placeholder;
        hint.font = FormFontOf(box, *context.text_service);
        hint.x = box.content_box.left - scroll_x;
        if (!box.lines.empty()) {
            hint.baseline = box.lines.front().baseline - scroll_y;
        } else {
            const float font_size = box.style.font_size > 0.0f ? box.style.font_size : 16.0f;
            hint.baseline = box.content_box.top + font_size * 0.8f - scroll_y;
        }
        hint.width = CodepointRunWidth(hint.text, 0, Utf8Length(hint.text), hint.font,
                                       box.style.letter_spacing, *context.text_service);
        hint.color = Rgba(0.46f, 0.46f, 0.46f, 1.0f);
        AppendTextQuads(hint, context, &quads);
    }
    if (!quads.empty()) {
        context.device->DrawGlyphs(quads.data(), quads.size());
    }

    // 光标：1px 竖线，高度取文字行高，位置按光标前缀宽度测量。
    if (focused && context.focus->caret_visible && context.text_service != nullptr) {
        const std::string text = FormControlText(*box.element);
        const FormCaretPosition position =
            LocateFormCaret(box, text, context.focus->caret, *context.text_service);
        if (position.valid) {
            const float caret_x = box.content_box.left + position.x - scroll_x;
            const RectF caret_doc =
                RectF::FromLTRB(caret_x, position.line_top - scroll_y, caret_x + 1.0f,
                                position.line_bottom - scroll_y);
            context.device->FillRect(ToPageRect(caret_doc, context),
                                     Rgba(0.15f, 0.15f, 0.15f, 1.0f));
        }
    }

    context.device->PopClip();
}

// 表单控件专用内容：checkbox/radio 绘制外框与选中标记，select 绘制右侧的
// 向下箭头。背景、边框与文本行由通用绘制逻辑负责，这里不重复绘制。
void PaintFormControl(const LayoutBox& box, PaintContext& context) {
    if (context.device == nullptr || box.form_kind == FormControlKind::None) {
        return;
    }
    const RectF content = ToPageRect(box.content_box, context);
    if (content.IsEmpty()) {
        return;
    }

    const bool disabled = box.element != nullptr && box.element->HasAttribute("disabled");
    // 标记颜色：禁用时统一使用 50% 灰度，其余取文字颜色。
    const Color marker_color =
        disabled ? Rgba(0.5f, 0.5f, 0.5f, 1.0f)
                 : (box.style.color.Visible() ? box.style.color : Rgba(0.2f, 0.2f, 0.2f, 1.0f));

    if (box.form_kind == FormControlKind::Checkbox ||
        box.form_kind == FormControlKind::Radio) {
        const float side = std::min(content.Width(), content.Height());
        // 禁用控件的边框改用更浅的灰色。
        const Color border_color =
            disabled ? Rgba(0.75f, 0.75f, 0.75f, 1.0f) : Rgba(0.46f, 0.46f, 0.46f, 1.0f);
        const float border_width = std::min(1.0f, side * 0.5f);
        if (box.form_kind == FormControlKind::Radio) {
            context.device->StrokeRoundedRect(content, side * 0.5f, border_width, border_color);
            if (box.form_checked) {
                // 单选框的选中标记为居中的实心圆点。
                const float inset = side * 0.3f;
                const RectF dot =
                    RectF::FromLTRB(content.left + inset, content.top + inset,
                                    content.right - inset, content.bottom - inset);
                context.device->FillRoundedRect(dot, std::min(dot.Width(), dot.Height()) * 0.5f,
                                                marker_color);
            }
        } else {
            const float radius = std::min(2.0f, side * 0.25f);
            context.device->StrokeRoundedRect(content, radius, border_width, border_color);
            if (box.form_checked) {
                // 复选框的选中底色；勾形由盒内行盒的勾选字形绘制。
                const float inset = std::min(1.0f, side * 0.25f);
                const RectF fill =
                    RectF::FromLTRB(content.left + inset, content.top + inset,
                                    content.right - inset, content.bottom - inset);
                context.device->FillRoundedRect(fill, ClampRadius(fill, radius), marker_color);
            }
        }
        return;
    }

    if (box.form_kind == FormControlKind::Select) {
        // 向下箭头位于内容区右侧预留的 16px 区域中央，由逐行收窄的横条组成。
        const float area = std::min(16.0f, content.Width());
        const float center_x = content.right - area * 0.5f;
        const float center_y = content.top + content.Height() * 0.5f;
        const float half_width = std::min(4.0f, area * 0.4f);
        const float arrow_height = 6.0f;
        const int row_count = 6;
        const Color arrow_color =
            disabled ? Rgba(0.5f, 0.5f, 0.5f, 1.0f)
                     : (box.style.color.Visible() ? box.style.color
                                                  : Rgba(0.2f, 0.2f, 0.2f, 1.0f));
        const float arrow_top = center_y - arrow_height * 0.5f;
        for (int row = 0; row < row_count; ++row) {
            const float strip_width =
                half_width * 2.0f *
                (1.0f - static_cast<float>(row) / static_cast<float>(row_count));
            const float strip_top = arrow_top + static_cast<float>(row);
            const RectF strip =
                RectF::FromLTRB(center_x - strip_width * 0.5f, strip_top,
                                center_x + strip_width * 0.5f, strip_top + 1.0f);
            context.device->FillRect(strip, arrow_color);
        }
    }
}

// 读取 HTML 数值属性；缺失或非法时返回回退值。
float ParseNumericAttribute(const Element& element, const std::string& name, float fallback) {
    if (!element.HasAttribute(name)) {
        return fallback;
    }
    const std::string text = element.GetAttribute(name);
    try {
        return std::stof(text);
    } catch (...) {
        return fallback;
    }
}

// 解析颜色属性（如 input[type=color] 的 value）为 RGB；失败时返回黑色。
Color ParseColorAttribute(const std::string& text) {
    std::string value = text;
    if (!value.empty() && value[0] == '#') {
        value.erase(value.begin());
    }
    if (value.size() == 3) {
        std::string expanded;
        for (char digit : value) {
            expanded.push_back(digit);
            expanded.push_back(digit);
        }
        value = expanded;
    }
    if (value.size() != 6) {
        return Black();
    }
    const auto component = [&](size_t index) {
        const std::string part = value.substr(index, 2);
        int number = 0;
        try {
            number = std::stoi(part, nullptr, 16);
        } catch (...) {
            number = 0;
        }
        return static_cast<float>(number) / 255.0f;
    };
    return Rgba(component(0), component(2), component(4), 1.0f);
}

/* progress、meter、input[type=range] 与 input[type=color] 的原生外观。
   这些控件没有子盒内容，由这里按 value/max 属性绘制指示条或色块。 */
void PaintNativeWidget(const LayoutBox& box, PaintContext& context) {
    if (context.device == nullptr || box.element == nullptr) {
        return;
    }
    const std::string tag_name = AsciiLower(box.element->TagName());
    const RectF content = ToPageRect(box.content_box, context);
    if (content.IsEmpty()) {
        return;
    }

    if (tag_name == "progress" || tag_name == "meter") {
        float minimum = 0.0f;
        float maximum = 1.0f;
        if (tag_name == "meter") {
            minimum = ParseNumericAttribute(*box.element, "min", 0.0f);
            maximum = ParseNumericAttribute(*box.element, "max", 1.0f);
        } else {
            maximum = ParseNumericAttribute(*box.element, "max", 1.0f);
        }
        const float value = ParseNumericAttribute(*box.element, "value", minimum);
        const float range = maximum - minimum;
        float fraction = range > 0.0f ? (value - minimum) / range : 0.0f;
        fraction = fraction < 0.0f ? 0.0f : (fraction > 1.0f ? 1.0f : fraction);
        const float inset = std::min(2.0f, std::min(content.Width(), content.Height()) * 0.2f);
        const RectF track = RectF::FromLTRB(content.left + inset, content.top + inset,
                                            content.right - inset, content.bottom - inset);
        Color fill = tag_name == "meter" ? Rgba(0.09f, 0.64f, 0.29f, 1.0f)
                                         : Rgba(0.23f, 0.51f, 0.96f, 1.0f);
        const float fill_width = track.Width() * fraction;
        if (fill_width > 0.0f) {
            context.device->FillRect(
                RectF::FromLTRB(track.left, track.top, track.left + fill_width, track.bottom),
                ApplyOpacity(fill, context));
        }
        return;
    }

    if (tag_name == "input") {
        const std::string type_name = AsciiLower(box.element->GetAttribute("type"));
        if (type_name == "range") {
            const float minimum = ParseNumericAttribute(*box.element, "min", 0.0f);
            const float maximum = ParseNumericAttribute(*box.element, "max", 100.0f);
            float value = ParseNumericAttribute(*box.element, "value",
                                                (minimum + maximum) * 0.5f);
            value = std::max(minimum, std::min(maximum, value));
            const float range = maximum - minimum;
            const float fraction = range > 0.0f ? (value - minimum) / range : 0.0f;
            const float track_height = std::min(4.0f, content.Height() * 0.25f);
            const float center_y = content.top + content.Height() * 0.5f;
            const RectF track = RectF::FromLTRB(content.left + 8.0f,
                                                center_y - track_height * 0.5f,
                                                content.right - 8.0f,
                                                center_y + track_height * 0.5f);
            if (!track.IsEmpty()) {
                context.device->FillRoundedRect(track, track_height * 0.5f,
                                                ApplyOpacity(Rgba(0.8f, 0.8f, 0.8f, 1.0f),
                                                             context));
                const float filled_width = track.Width() * fraction;
                if (filled_width > 0.0f) {
                    context.device->FillRoundedRect(
                        RectF::FromLTRB(track.left, track.top, track.left + filled_width,
                                        track.bottom),
                        track_height * 0.5f,
                        ApplyOpacity(Rgba(0.23f, 0.51f, 0.96f, 1.0f), context));
                }
            }
            const float thumb_radius = std::min(8.0f, content.Height() * 0.5f);
            const float thumb_x = track.left + track.Width() * fraction;
            const RectF thumb =
                RectF::FromLTRB(thumb_x - thumb_radius, center_y - thumb_radius,
                                thumb_x + thumb_radius, center_y + thumb_radius);
            context.device->FillRoundedRect(thumb, thumb_radius,
                                            ApplyOpacity(Rgba(0.25f, 0.25f, 0.25f, 1.0f),
                                                         context));
            return;
        }
        if (type_name == "color") {
            const Color color = ParseColorAttribute(box.element->GetAttribute("value"));
            context.device->FillRect(content, ApplyOpacity(color, context));
            return;
        }
    }
}

// 由绘制上下文生成 SVG 绘制参数：坐标换算与透明度沿用当前文档。
SvgPaintParams MakeSvgPaintParams(const PaintContext& context) {
    SvgPaintParams params;
    params.device = context.device;
    params.text = context.text_service;
    params.glyphs = context.glyph_info;
    params.atlas_width = context.atlas_width;
    params.atlas_height = context.atlas_height;
    params.opacity = context.opacity;
    params.page_top = context.page_top;
    params.offset_x = context.offset_x;
    params.offset_y = context.offset_y;
    return params;
}

// 块级盒或行内块盒自身的外观：阴影、背景、渐变、图片内容（仅 img）与边框。
void PaintBoxDecoration(const LayoutBox& box, PaintContext& context,
                        const Color& ancestor_background) {
    const RectF rect = ToPageRect(box.border_box, context);
    if (rect.IsEmpty()) {
        return;
    }

    const CornerRadii radii = StyleCornerRadii(box.style, rect);
    // 外阴影绘制在背景之下；内阴影绘制在背景之上，避免被不透明背景覆盖。
    PaintBoxShadows(*context.device, rect, radii, box.style.box_shadows, context.opacity, false);
    PaintBackgroundFill(*context.device, rect, ApplyOpacity(box.style.background_color, context),
                        radii);
    // 渐变裁剪到文字时只保留文字颜色，不再绘制渐变色块。
    if (!box.style.background_clip_text) {
        PaintBackgroundImage(*context.device, rect, box.style.background_image, radii.Maximum(),
                             context.opacity);
        PaintBackgroundImageUrl(context.layout, box.style.background_image_url,
                                box.style.background_size, box.style.background_position_x,
                                box.style.background_position_y, box.style.background_repeat,
                                rect, radii, context);
    }
    // 内阴影在背景与图片之上、内容之下。
    PaintBoxShadows(*context.device, rect, radii, box.style.box_shadows, context.opacity, true);

    // 内容裁剪使用内轮廓半径：边框宽度从对应半径中扣除，与填充和边框的圆角一致。
    const BorderWidths widths = BoxBorderWidths(box);
    const CornerRadii content_radii = InnerCornerRadii(radii, widths);

    if (box.kind == BoxKind::Image && box.element != nullptr) {
        const std::string tag_name = AsciiLower(box.element->TagName());
        if (tag_name == "canvas") {
            // 画布绘制在整个边框盒上，圆角使用边框盒半径。
            PaintCanvasContent(*box.element, rect, radii, context);
        } else if (tag_name == "iframe") {
            // 框架内容绘制在内容区内，边框随后由通用逻辑覆盖在外圈。
            PaintEmbeddedFrame(*box.element, ToPageRect(box.content_box, context), content_radii,
                               ancestor_background, context);
        } else if (tag_name == "svg") {
            // 内联 SVG 在内容区内绘制，内容超出视口的部分被裁剪。
            if (context.layout != nullptr) {
                PaintSvgBox(box, *context.layout, MakeSvgPaintParams(context));
            }
        } else {
            // 图片绘制在内容区内，object-fit 与 object-position 按内容区计算。
            PaintImageContent(*box.element, ToPageRect(box.content_box, context), content_radii,
                              box.style, context);
        }
    }

    PaintNativeWidget(box, context);

    BorderColors colors = StyleBorderColors(box.style);
    colors.top = ApplyOpacity(colors.top, context);
    colors.right = ApplyOpacity(colors.right, context);
    colors.bottom = ApplyOpacity(colors.bottom, context);
    colors.left = ApplyOpacity(colors.left, context);
    PaintBoxBorders(*context.device, rect, widths, colors, StyleBorderStyles(box.style), radii);
}

// 行内片段盒的背景与边框（行内盒覆盖区域由布局给出）。
void PaintInlineDecoration(const PlacedInline& inline_box, PaintContext& context) {
    if (inline_box.style == nullptr || context.device == nullptr) {
        return;
    }
    const RectF rect = ToPageRect(inline_box.rect, context);
    if (rect.IsEmpty()) {
        return;
    }

    const ComputedStyle& style = *inline_box.style;
    const CornerRadii radii = StyleCornerRadii(style, rect);
    PaintBackgroundFill(*context.device, rect, ApplyOpacity(style.background_color, context),
                        radii);
    // 行内元素同样遵循 background-clip: text：不绘制渐变色块，只保留文字颜色。
    if (!style.background_clip_text) {
        PaintBackgroundImage(*context.device, rect, style.background_image, radii.Maximum(),
                             context.opacity);
        PaintBackgroundImageUrl(context.layout, style.background_image_url, style.background_size,
                                style.background_position_x, style.background_position_y,
                                style.background_repeat, rect, radii, context);
    }

    const BorderWidths widths = StyleBorderWidths(style);
    BorderColors colors = StyleBorderColors(style);
    colors.top = ApplyOpacity(colors.top, context);
    colors.right = ApplyOpacity(colors.right, context);
    colors.bottom = ApplyOpacity(colors.bottom, context);
    colors.left = ApplyOpacity(colors.left, context);
    PaintBoxBorders(*context.device, rect, widths, colors, StyleBorderStyles(style), radii);
}

// 按码位逐个推进笔位置，生成字形四边形。UV 由图集像素矩形归一化得到。
// dx/dy 用于文字阴影的偏移，color_override 非空时替换片段自身颜色。
void AppendTextQuads(const PlacedText& placed, PaintContext& context,
                     std::vector<GlyphQuad>* quads, float dx, float dy,
                     const Color* color_override) {
    if (placed.font == 0 || !placed.color.Visible() || context.glyph_info == nullptr) {
        return;
    }
    if (context.atlas_width <= 0 || context.atlas_height <= 0) {
        return;
    }
    const Color color =
        ApplyOpacity(color_override != nullptr ? *color_override : placed.color, context);
    if (!color.Visible()) {
        return;
    }

    const float atlas_width = static_cast<float>(context.atlas_width);
    const float atlas_height = static_cast<float>(context.atlas_height);
    // 笔起点与基线需要加上内嵌文档的绘制偏移，顶层绘制时偏移为 0。
    float pen_x = placed.x + context.offset_x + dx;

    size_t offset = 0;
    while (offset < placed.text.size()) {
        const Utf8Char character = DecodeUtf8(placed.text, offset);
        if (character.length == 0) {
            break;
        }

        GlyphInfo glyph;
        const auto found =
            context.glyph_info->find(GlyphKey(placed.font, character.codepoint));
        if (found != context.glyph_info->end()) {
            glyph = found->second;
        }

        if (glyph.has_bitmap && glyph.atlas_w > 0 && glyph.atlas_h > 0) {
            // 字形位图落到整数像素：位置与尺寸均为整数时，线性采样恰好取到纹素中心，
            // 不会因亚像素偏移产生额外的模糊。
            const float x0 = std::floor(pen_x + glyph.bearing_x + 0.5f);
            const float y0 =
                std::floor(placed.baseline + dy + context.offset_y - glyph.bearing_y -
                           context.page_top + 0.5f);
            const float x1 = x0 + static_cast<float>(glyph.atlas_w);
            const float y1 = y0 + static_cast<float>(glyph.atlas_h);

            GlyphQuad quad;
            quad.x0 = x0;
            quad.y0 = y0;
            quad.x1 = x1;
            quad.y1 = y1;
            quad.u0 = static_cast<float>(glyph.atlas_x) / atlas_width;
            quad.v0 = static_cast<float>(glyph.atlas_y) / atlas_height;
            quad.u1 = static_cast<float>(glyph.atlas_x + glyph.atlas_w) / atlas_width;
            quad.v1 = static_cast<float>(glyph.atlas_y + glyph.atlas_h) / atlas_height;
            quad.color = color;
            quads->push_back(quad);
        }

        pen_x += glyph.advance + placed.letter_spacing;
        offset += character.length;
    }
}

// 下划线位于基线下 2px，厚度 1px，宽度取片段宽度。
void PaintTextUnderline(const PlacedText& placed, PaintContext& context) {
    if (context.device == nullptr || !placed.color.Visible() || placed.width <= 0.0f) {
        return;
    }
    const float top =
        std::floor(placed.baseline + 2.0f + context.offset_y - context.page_top + 0.5f);
    const RectF rect = RectF::FromXYWH(placed.x + context.offset_x, top, placed.width, 1.0f);
    context.device->FillRect(rect, placed.color);
}

// 递归绘制布局盒。paint_own_decoration 为 false 时跳过本盒的背景与边框；
// page_test 为 false 表示整盒属于已经绘制的行内原子盒，不再按页面边界过滤。
void PaintLayoutBox(const LayoutBox& box, PaintContext& context, bool paint_own_decoration,
                    bool page_test, const Color& ancestor_background);

// 绘制一行：先绘制行内片段背景与边框，再绘制文字字形（含下划线），最后绘制原子盒。
// 行内块原子盒的装饰与内部内容都在这里绘制：原子盒属于哪一行，就随哪一行落到同一页。
void PaintLineContent(const LineFragment& line, PaintContext& context,
                      const Color& ancestor_background) {
    for (const PlacedInline& inline_box : line.inlines) {
        PaintInlineDecoration(inline_box, context);
    }

    std::vector<GlyphQuad> quads;
    for (const PlacedText& placed : line.texts) {
        if (placed.underline) {
            PaintTextUnderline(placed, context);
        }
        // 文字阴影先于字形绘制，按偏移叠放，不参与下划线。
        if (placed.shadow_style != nullptr) {
            for (const Shadow& shadow : placed.shadow_style->text_shadows) {
                if (!shadow.color.Visible()) {
                    continue;
                }
                AppendTextQuads(placed, context, &quads, shadow.offset_x, shadow.offset_y,
                                &shadow.color);
            }
        }
        AppendTextQuads(placed, context, &quads);
    }
    if (!quads.empty()) {
        context.device->DrawGlyphs(quads.data(), quads.size());
    }

    for (const PlacedAtomic& atomic_box : line.atomics) {
        if (atomic_box.element == nullptr || context.device == nullptr) {
            continue;
        }
        const RectF rect = ToPageRect(atomic_box.border_box, context);
        if (rect.IsEmpty()) {
            continue;
        }

        const std::string tag_name = AsciiLower(atomic_box.element->TagName());
        // 原子盒的背景、内容裁剪与边框共用同一套圆角几何：四角半径由样式按
        // 边框盒尺寸解析，百分比半径在此换算为像素。
        static const ComputedStyle kDefaultAtomicStyle;
        const ComputedStyle& atomic_style =
            atomic_box.style != nullptr ? *atomic_box.style : kDefaultAtomicStyle;
        const CornerRadii radii = StyleCornerRadii(atomic_style, rect);
        // 内容区在边框内侧，图片与框架的裁剪半径按边框宽度收缩。
        const CornerRadii content_radii = InnerCornerRadii(radii, StyleBorderWidths(atomic_style));
        if (tag_name == "img") {
            // 图片先绘制内容，再绘制边框；圆角由 images 着色器按距离场裁剪。
            const RectF content =
                atomic_box.box != nullptr ? ToPageRect(atomic_box.box->content_box, context)
                                          : rect;
            PaintImageContent(*atomic_box.element, content, content_radii, atomic_style, context);
        } else if (tag_name == "canvas") {
            // 画布像素通常带透明区域，先绘制元素自身的背景色再叠加画布内容。
            PaintBackgroundFill(*context.device, rect, atomic_style.background_color, radii);
            PaintCanvasContent(*atomic_box.element, rect, radii, context);
        } else if (tag_name == "iframe") {
            // 框架先绘制元素背景，再在内容区内绘制子文档；边框最后统一绘制。
            PaintBackgroundFill(*context.device, rect, atomic_style.background_color, radii);
            const RectF content_rect =
                atomic_box.box != nullptr ? ToPageRect(atomic_box.box->content_box, context)
                                          : rect;
            PaintEmbeddedFrame(*atomic_box.element, content_rect, content_radii,
                               ancestor_background, context);
        } else if (tag_name == "svg") {
            // 行内 SVG：背景之后在内容区内绘制 SVG 子树，边框最后统一绘制。
            PaintBackgroundFill(*context.device, rect, atomic_style.background_color, radii);
            if (atomic_box.box != nullptr && context.layout != nullptr) {
                PaintSvgBox(*atomic_box.box, *context.layout, MakeSvgPaintParams(context));
            }
        } else {
            // 通用行内原子盒（含行内块）先绘制背景色，再绘制渐变与背景图片：
            // 行内流中的按钮等控件依赖该路径，若只填充纯色会丢失渐变背景。
            PaintBackgroundFill(*context.device, rect, atomic_style.background_color, radii);
            if (!atomic_style.background_clip_text) {
                PaintBackgroundImage(*context.device, rect, atomic_style.background_image,
                                     radii.Maximum(), context.opacity);
                PaintBackgroundImageUrl(context.layout, atomic_style.background_image_url,
                                        atomic_style.background_size,
                                        atomic_style.background_position_x,
                                        atomic_style.background_position_y,
                                        atomic_style.background_repeat, rect, radii, context);
            }
        }

        if (atomic_box.style != nullptr) {
            const BorderWidths widths = StyleBorderWidths(atomic_style);
            const BorderColors colors = StyleBorderColors(atomic_style);
            PaintBoxBorders(*context.device, rect, widths, colors,
                            StyleBorderStyles(atomic_style), radii);
        }

        // 原生控件外观（progress/meter/range/color）与表单专用标记。
        if (atomic_box.box != nullptr) {
            PaintNativeWidget(*atomic_box.box, context);
        }
        // 表单控件在通用背景与边框之后绘制专用标记；文本行由其内部盒的
        // 行盒绘制，因此这里不再重复绘制文字。
        if (atomic_box.box != nullptr &&
            atomic_box.box->form_kind != FormControlKind::None) {
            PaintFormControl(*atomic_box.box, context);
        }

        // 行内块内部可能包含匿名块、图注与更深层的原子盒：
        // 作为原子盒整体在本行绘制，内部内容随原子盒一起绘制到同一页。
        if (atomic_box.box != nullptr && atomic_box.box->kind == BoxKind::InlineBlock) {
            Color inner_background = ancestor_background;
            if (atomic_box.style != nullptr && atomic_box.style->background_color.Visible() &&
                atomic_box.style->background_color.a >= 0.999f) {
                inner_background = atomic_box.style->background_color;
            }
            PaintLayoutBox(*atomic_box.box, context, false, false, inner_background);
        }
    }
}

// 列表标记使用首行首个文字片段的字体；找不到字体时返回 0。
FontId FirstLineFont(const LayoutBox& box) {
    if (box.lines.empty()) {
        return 0;
    }
    for (const PlacedText& placed : box.lines.front().texts) {
        if (placed.font != 0) {
            return placed.font;
        }
    }
    return 0;
}

// 按字形步进累加文本宽度；图集中缺失的字形按步进 0 处理。
float MeasureStringWidth(const std::string& text, FontId font,
                         const std::map<GlyphKey, GlyphInfo>& glyph_info) {
    float width = 0.0f;
    size_t offset = 0;
    while (offset < text.size()) {
        const Utf8Char character = DecodeUtf8(text, offset);
        if (character.length == 0) {
            break;
        }
        const auto found = glyph_info.find(GlyphKey(font, character.codepoint));
        if (found != glyph_info.end()) {
            width += found->second.advance;
        }
        offset += character.length;
    }
    return width;
}

// 列表标记：右边缘距内容盒左侧 8px，基线与首行基线对齐。
void PaintListMarker(const LayoutBox& box, PaintContext& context) {
    if (box.marker.empty() || box.lines.empty() || context.device == nullptr ||
        context.glyph_info == nullptr) {
        return;
    }
    const LineFragment& first_line = box.lines.front();
    if (!IntersectsPage(first_line.rect, context.page_top, context.page_bottom)) {
        return;
    }

    const FontId font = FirstLineFont(box);
    if (font == 0) {
        return;
    }

    const float width = MeasureStringWidth(box.marker, font, *context.glyph_info);
    PlacedText placed;
    placed.font = font;
    placed.text = box.marker;
    placed.x = box.content_box.left - 8.0f - width;
    placed.baseline = first_line.baseline;
    placed.width = width;
    placed.color = box.style.color;

    std::vector<GlyphQuad> quads;
    AppendTextQuads(placed, context, &quads);
    if (!quads.empty()) {
        context.device->DrawGlyphs(quads.data(), quads.size());
    }
}

// 递归绘制布局盒。paint_own_decoration 为 false 时跳过本盒背景与边框，
// 用于行内块内部内容随原子盒一起绘制的情形。
// page_test 为 false 表示整盒属于已经绘制的行内原子盒，不再按页面边界过滤，
// 避免行盒与原子盒的坐标在分页边界处出现微差时只绘出背景而丢失文字。
void PaintLayoutBox(const LayoutBox& box, PaintContext& context, bool paint_own_decoration,
                    bool page_test, const Color& ancestor_background) {
    if (page_test && !IntersectsPage(box.border_box, context.page_top, context.page_bottom)) {
        return;
    }

    /* clip-path: inset(...) 以矩形裁剪近似，裁剪本盒的背景、边框、文字与子盒。 */
    const bool clip_path_active = box.style.has_clip_path;
    if (clip_path_active) {
        const RectF box_rect = ToPageRect(box.border_box, context);
        const float inset_top =
            ResolveClipInset(box.style.clip_inset_top, box_rect.Height(), box.style.font_size);
        const float inset_right =
            ResolveClipInset(box.style.clip_inset_right, box_rect.Width(), box.style.font_size);
        const float inset_bottom =
            ResolveClipInset(box.style.clip_inset_bottom, box_rect.Height(), box.style.font_size);
        const float inset_left =
            ResolveClipInset(box.style.clip_inset_left, box_rect.Width(), box.style.font_size);
        const RectF clip_rect = RectF::FromLTRB(box_rect.left + inset_left,
                                                box_rect.top + inset_top,
                                                box_rect.right - inset_right,
                                                box_rect.bottom - inset_bottom);
        context.device->PushClip(clip_rect);
    }

    // opacity 沿子树累乘：本盒与后代的所有颜色按累计值合成。
    const float saved_opacity = context.opacity;
    context.opacity *= box.style.opacity;

    // filter 同样沿子树生效：多个元素的滤镜按效果叠加（灰度/棕褐色取
    // 累积比例，亮度/对比度/饱和度相乘）。
    const float saved_brightness = context.filter_brightness;
    const float saved_contrast = context.filter_contrast;
    const float saved_saturate = context.filter_saturate;
    const float saved_grayscale = context.filter_grayscale;
    const float saved_sepia = context.filter_sepia;
    const float saved_invert = context.filter_invert;
    if (box.style.has_filter) {
        context.filter_brightness *= box.style.filter_brightness;
        context.filter_contrast *= box.style.filter_contrast;
        context.filter_saturate *= box.style.filter_saturate;
        context.filter_grayscale =
            1.0f - (1.0f - context.filter_grayscale) * (1.0f - box.style.filter_grayscale);
        context.filter_sepia =
            1.0f - (1.0f - context.filter_sepia) * (1.0f - box.style.filter_sepia);
        context.filter_invert =
            1.0f - (1.0f - context.filter_invert) * (1.0f - box.style.filter_invert);
    }

    if (paint_own_decoration) {
        PaintBoxDecoration(box, context, ancestor_background);
    }

    /* overflow 裁剪：内容与子盒裁剪到内边距盒，背景与边框不受影响。 */
    const bool clip_content = box.style.overflow_hidden;
    if (clip_content) {
        const RectF padding_box =
            RectF::FromLTRB(box.content_box.left - box.padding_left,
                            box.content_box.top - box.padding_top,
                            box.content_box.right + box.padding_right,
                            box.content_box.bottom + box.padding_bottom);
        context.device->PushClip(ToPageRect(padding_box, context));
    }

    PaintListMarker(box, context);

    const bool text_control = IsTextFormControl(box);
    if (text_control) {
        // 文本控件由表单绘制路径按滚动偏移绘制，行盒仍用于字形预栅格化。
        PaintFormText(box, context);
    } else {
        for (const LineFragment& line : box.lines) {
            if (page_test && !IntersectsPage(line.rect, context.page_top, context.page_bottom)) {
                continue;
            }
            PaintLineContent(line, context, ancestor_background);
        }
    }

    // 当前盒的不透明背景成为子盒背后最近的背景色；半透明或透明背景继续向上继承。
    Color child_background = ancestor_background;
    if (box.HasBackground() && box.style.background_color.a >= 0.999f) {
        child_background = box.style.background_color;
    }

    // 子盒按 z-index 稳定排序：负层级先绘制，正层级覆盖普通流内容；
    // 相同层级保持文档顺序。
    std::vector<const LayoutBox*> ordered;
    ordered.reserve(box.children.size());
    for (const std::unique_ptr<LayoutBox>& child : box.children) {
        if (!child) {
            continue;
        }
        if (child->atomic &&
            (child->kind == BoxKind::Image || child->kind == BoxKind::InlineBlock)) {
            continue;
        }
        ordered.push_back(child.get());
    }
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const LayoutBox* left, const LayoutBox* right) {
                         const int left_index = left->style.z_index_auto ? 0 : left->style.z_index;
                         const int right_index =
                             right->style.z_index_auto ? 0 : right->style.z_index;
                         return left_index < right_index;
                     });

    for (const LayoutBox* child : ordered) {
        PaintLayoutBox(*child, context, true, page_test, child_background);
    }

    if (clip_content) {
        context.device->PopClip();
    }
    if (clip_path_active) {
        context.device->PopClip();
    }
    context.filter_invert = saved_invert;
    context.filter_sepia = saved_sepia;
    context.filter_grayscale = saved_grayscale;
    context.filter_saturate = saved_saturate;
    context.filter_contrast = saved_contrast;
    context.filter_brightness = saved_brightness;
    context.opacity = saved_opacity;
}

// 把一段文本的码位放入图集缓存；字体为 0 时跳过。
void CollectStringGlyphs(const std::string& text, FontId font, TextService& text_service,
                         std::map<GlyphKey, GlyphInfo>* glyph_info) {
    if (font == 0 || glyph_info == nullptr) {
        return;
    }
    size_t offset = 0;
    while (offset < text.size()) {
        const Utf8Char character = DecodeUtf8(text, offset);
        if (character.length == 0) {
            break;
        }
        const GlyphKey key(font, character.codepoint);
        if (glyph_info->find(key) == glyph_info->end()) {
            (*glyph_info)[key] = text_service.Rasterize(font, character.codepoint);
        }
        offset += character.length;
    }
}

// 预栅格化：收集本页所有可见文字片段（含列表标记）的码位并放入图集。
// page_test 为 false 表示整盒属于已经绘制的行内原子盒，其内部文字全部需要进入图集。
void CollectPageGlyphs(const LayoutBox& box, float page_top, float page_bottom,
                       TextService& text_service, const LayoutResult& layout,
                       std::map<GlyphKey, GlyphInfo>* glyph_info, bool page_test = true) {
    if (page_test && !IntersectsPage(box.border_box, page_top, page_bottom)) {
        return;
    }

    // SVG 子树的文字不进入 HTML 行盒，按自身坐标系单独收集字形。
    if (box.element != nullptr && AsciiLower(box.element->TagName()) == "svg") {
        CollectSvgBoxGlyphs(box, layout, text_service, glyph_info);
        return;
    }

    for (const LineFragment& line : box.lines) {
        if (page_test && !IntersectsPage(line.rect, page_top, page_bottom)) {
            continue;
        }
        for (const PlacedText& placed : line.texts) {
            CollectStringGlyphs(placed.text, placed.font, text_service, glyph_info);
        }
    }

    // 列表标记与首行同页时才需要进入图集。
    if (!box.marker.empty() && !box.lines.empty() &&
        IntersectsPage(box.lines.front().rect, page_top, page_bottom)) {
        CollectStringGlyphs(box.marker, FirstLineFont(box), text_service, glyph_info);
    }

    /* 表单控件的 placeholder 没有行盒，需要在此单独收集字形，
       否则绘制占位文本时图集中缺少对应位图。textarea 不使用表单控件
       排版路径，form_kind 为 None，需按标签名单独识别。 */
    const bool placeholder_capable =
        box.form_kind != FormControlKind::None ||
        (box.element != nullptr && AsciiLower(box.element->TagName()) == "textarea");
    if (placeholder_capable && box.element != nullptr) {
        const std::string placeholder = box.element->GetAttribute("placeholder");
        if (!placeholder.empty() && FormDisplayText(*box.element).empty()) {
            CollectStringGlyphs(placeholder, FormFontOf(box, text_service), text_service,
                                glyph_info);
        }
    }

    for (const std::unique_ptr<LayoutBox>& child : box.children) {
        if (!child) {
            continue;
        }
        const bool atomic =
            child->atomic &&
            (child->kind == BoxKind::Image || child->kind == BoxKind::InlineBlock);
        CollectPageGlyphs(*child, page_top, page_bottom, text_service, layout, glyph_info,
                          atomic ? false : page_test);
    }
}

// 递归收集内嵌子文档需要的字形。子文档有独立的文档坐标与分页范围，
// 绘制时整篇内容都会进入框架裁剪区，因此按子布局内容高度全量收集。
void CollectEmbeddedGlyphs(const LayoutResult& layout, TextService& text_service,
                           std::map<GlyphKey, GlyphInfo>* glyph_info) {
    for (const auto& entry : layout.embedded_frames) {
        const LayoutResult* frame_layout = entry.second;
        if (frame_layout == nullptr || !frame_layout->root) {
            continue;
        }
        float frame_bottom = frame_layout->content_height;
        if (frame_bottom <= 0.0f) {
            frame_bottom = frame_layout->page_content_height;
        }
        if (frame_bottom <= 0.0f) {
            frame_bottom = 1.0f;
        }
        CollectPageGlyphs(*frame_layout->root, 0.0f, frame_bottom, text_service, *frame_layout,
                          glyph_info, false);
        CollectEmbeddedGlyphs(*frame_layout, text_service, glyph_info);
    }
}

// 返回文本节点所属的最近元素祖先。
const Element* ElementOfNode(const TextNode* node) {
    if (node == nullptr) {
        return nullptr;
    }
    const Node* current = node->Parent();
    while (current != nullptr) {
        if (current->IsElement()) {
            return static_cast<const Element*>(current);
        }
        current = current->Parent();
    }
    return nullptr;
}

// 命中文档中的一个文字片段：按 UTF-8 解码逐个字符累计横向区间，
// 返回字符起点的字节偏移（相对于整个文本节点）。
HitResult HitTestPlacedText(const LineFragment& line, const PlacedText& placed, float x, float y) {
    HitResult result;
    if (placed.node == nullptr || placed.width <= 0.0f) {
        return result;
    }
    if (!line.rect.Contains(x, y)) {
        return result;
    }
    if (x < placed.x || x >= placed.x + placed.width) {
        return result;
    }

    size_t character_count = 0;
    size_t scan_offset = 0;
    while (scan_offset < placed.text.size()) {
        const Utf8Char character = DecodeUtf8(placed.text, scan_offset);
        if (character.length == 0) {
            break;
        }
        scan_offset += character.length;
        ++character_count;
    }
    if (character_count == 0) {
        return result;
    }

    // 命中测试不持有 TextService，按片段总宽度与字距均匀分配每个字符的步进。
    const float spacing_total =
        placed.letter_spacing * static_cast<float>(character_count - 1);
    float cell_width =
        (placed.width - spacing_total) / static_cast<float>(character_count);
    if (cell_width < 0.0f) {
        cell_width = 0.0f;
    }

    float pen_x = placed.x;
    size_t byte_offset = 0;
    for (size_t index = 0; index < character_count; ++index) {
        const Utf8Char character = DecodeUtf8(placed.text, byte_offset);
        if (character.length == 0) {
            break;
        }

        float cell_end = pen_x + cell_width;
        if (index + 1 < character_count) {
            cell_end += placed.letter_spacing;
        } else {
            cell_end = placed.x + placed.width;
        }

        if (x >= pen_x && x < cell_end) {
            const Element* element = ElementOfNode(placed.node);
            if (element == nullptr) {
                return HitResult{};
            }
            result.element = element;
            result.node = placed.node;
            result.offset = placed.offset + byte_offset;
            return result;
        }

        pen_x = cell_end;
        byte_offset += character.length;
    }
    return result;
}

// 命中内嵌文档：把文档坐标平移到子布局内容盒原点后，调用子布局的命中测试。
// 命中元素保留为子文档元素，frame_element 记录其所属的最近 iframe；
// 若子布局内又命中更深 iframe，则由更内层结果携带其最近的 iframe 所有者。
HitResult HitTestEmbeddedFrame(const Element& element, const LayoutBox& frame_box,
                               const LayoutResult& layout, float x, float y,
                               TextService* text) {
    HitResult empty;
    const auto found = layout.embedded_frames.find(&element);
    if (found == layout.embedded_frames.end() || found->second == nullptr) {
        return empty;
    }
    const LayoutResult& child_layout = *found->second;
    if (!child_layout.root || child_layout.PageCount() == 0) {
        return empty;
    }
    const RectF content = frame_box.content_box;
    if (content.IsEmpty() || !content.Contains(x, y)) {
        return empty;
    }
    HitResult inner =
        HitTestPage(child_layout, 0, x - content.left, y - content.top, text);
    if (!inner.Valid()) {
        return empty;
    }
    if (inner.frame_element == nullptr) {
        inner.frame_element = &element;
    }
    return inner;
}

// 按绘制顺序的逆序递归遍历：子盒优先，其次原子盒，再文字，最后盒自身。
// x/y 为当前文档坐标；layout 提供当前文档的 iframe 映射表。
HitResult HitTestLayoutBox(const LayoutBox& box, const LayoutResult& layout, float x, float y,
                           TextService* text) {
    for (auto child = box.children.rbegin(); child != box.children.rend(); ++child) {
        if (!*child) {
            continue;
        }
        const HitResult result = HitTestLayoutBox(**child, layout, x, y, text);
        if (result.Valid()) {
            return result;
        }
    }

    for (auto line = box.lines.rbegin(); line != box.lines.rend(); ++line) {
        for (auto atomic_box = line->atomics.rbegin(); atomic_box != line->atomics.rend();
             ++atomic_box) {
            if (atomic_box->element != nullptr && atomic_box->border_box.Contains(x, y)) {
                // 行内块内部可能包含更深层的可命中内容（例如嵌在容器里的表单控件），
                // 先递归其布局子树，再退回行内块自身的元素。
                if (atomic_box->box != nullptr) {
                    const HitResult inner =
                        HitTestLayoutBox(*atomic_box->box, layout, x, y, text);
                    if (inner.Valid()) {
                        return inner;
                    }
                }
                // iframe 作为行内原子盒时，命中测试进入子文档。
                if (AsciiLower(atomic_box->element->TagName()) == "iframe" &&
                    atomic_box->box != nullptr) {
                    const HitResult inner = HitTestEmbeddedFrame(
                        *atomic_box->element, *atomic_box->box, layout, x, y, text);
                    if (inner.Valid()) {
                        return inner;
                    }
                }
                // 行内 SVG：按图元几何继续命中内部元素，未命中时退回 svg 自身。
                if (AsciiLower(atomic_box->element->TagName()) == "svg" &&
                    atomic_box->box != nullptr) {
                    HitResult result;
                    const Element* inner =
                        HitTestSvgBox(*atomic_box->box, layout, x, y, text);
                    result.element = inner != nullptr ? inner : atomic_box->element;
                    return result;
                }
                HitResult result;
                result.element = atomic_box->element;
                return result;
            }
        }
        for (auto placed = line->texts.rbegin(); placed != line->texts.rend(); ++placed) {
            const HitResult result = HitTestPlacedText(*line, *placed, x, y);
            if (result.Valid()) {
                return result;
            }
        }
    }

    if (box.element != nullptr && box.border_box.Contains(x, y)) {
        // 块级 iframe 命中时同样进入子文档，未命中子内容时退回 iframe 自身。
        if (box.kind == BoxKind::Image && AsciiLower(box.element->TagName()) == "iframe") {
            const HitResult inner = HitTestEmbeddedFrame(*box.element, box, layout, x, y, text);
            if (inner.Valid()) {
                return inner;
            }
        }
        // 块级 SVG：返回最内层命中的图元，便于脚本按元素处理选择与拖拽。
        if (box.kind == BoxKind::Image && AsciiLower(box.element->TagName()) == "svg") {
            HitResult result;
            const Element* inner = HitTestSvgBox(box, layout, x, y, text);
            result.element = inner != nullptr ? inner : box.element;
            return result;
        }
        HitResult result;
        result.element = box.element;
        return result;
    }
    return HitResult{};
}

// 判断 src 是否为绝对路径（盘符、UNC、根目录开头）。
bool IsAbsolutePath(const std::string& path) {
    if (path.empty()) {
        return false;
    }
    if (path.size() >= 3 && IsAsciiAlpha(path[0]) && path[1] == ':' &&
        (path[2] == '\\' || path[2] == '/')) {
        return true;
    }
    if (path.size() >= 2 && path[0] == '\\' && path[1] == '\\') {
        return true;
    }
    return path[0] == '\\' || path[0] == '/';
}

// 拼接基准目录与相对路径，保留相对路径中的分隔符写法。
std::string JoinPath(const std::string& base_directory, const std::string& relative_path) {
    if (base_directory.empty()) {
        return relative_path;
    }
    std::string path = base_directory;
    const char last = path.back();
    if (last != '\\' && last != '/') {
        path.push_back('\\');
    }
    path += relative_path;
    return path;
}

// data: 开头的内联资源暂不支持解码，直接按失败处理。
bool IsInlineDataSource(const std::string& source) {
    return source.size() >= 5 && AsciiLower(source.substr(0, 5)) == "data:";
}

}  // namespace

ImageCache::ImageCache(RenderDevice& device, std::string base_directory)
    : device_(device), base_directory_(std::move(base_directory)) {}

ImageCache::~ImageCache() = default;

void ImageCache::SetBaseDirectory(std::string base_directory) {
    if (base_directory_ == base_directory) {
        return;
    }
    // 基准目录变化后相对路径的解析结果随之变化，旧映射不再有效。
    base_directory_ = std::move(base_directory);
    handles_.clear();
}

const std::string& ImageCache::BaseDirectory() const {
    return base_directory_;
}

ImageHandle ImageCache::Get(const std::string& source) {
    const auto cached = handles_.find(source);
    if (cached != handles_.end()) {
        return cached->second;
    }

    ImageHandle handle = 0;
    std::string trimmed_source = source;
    TrimAsciiSpace(&trimmed_source);
    if (!trimmed_source.empty()) {
        ImageData image;
        std::string error;
        bool loaded = false;
        if (IsDataUri(trimmed_source)) {
            // data: 内联图像直接从内存解码，不经过文件缓存。
            loaded = LoadImageDataUri(trimmed_source, &image, &error) && image.Valid();
        } else if (!IsInlineDataSource(trimmed_source)) {
            const std::string path = IsAbsolutePath(trimmed_source)
                                         ? trimmed_source
                                         : JoinPath(base_directory_, trimmed_source);
            loaded = LoadImageFile(path, &image, &error) && image.Valid();
        }
        if (loaded) {
            handle = device_.CreateImage(image);
            sizes_[handle] = std::make_pair(image.width, image.height);
        }
    }

    // 失败结果同样写入映射，后续同 src 不再重复解码。
    handles_.emplace(source, handle);
    return handle;
}

int ImageCache::Width(ImageHandle image) const {
    const auto found = sizes_.find(image);
    return found != sizes_.end() ? found->second.first : 0;
}

int ImageCache::Height(ImageHandle image) const {
    const auto found = sizes_.find(image);
    return found != sizes_.end() ? found->second.second : 0;
}

void ImageCache::Clear() {
    handles_.clear();
    canvas_handles_.clear();
    sizes_.clear();
}

ImageHandle ImageCache::GetCanvas(const Element& element, const CanvasSurface& surface) {
    if (surface.width <= 0 || surface.height <= 0 ||
        surface.pixels.size() !=
            static_cast<size_t>(surface.width) * static_cast<size_t>(surface.height) * 4u) {
        return 0;
    }

    ImageData image;
    image.width = surface.width;
    image.height = surface.height;
    image.pixels = surface.pixels;

    const auto found = canvas_handles_.find(&element);
    if (found != canvas_handles_.end()) {
        if (found->second.handle != 0 && found->second.revision != surface.revision) {
            device_.UpdateImage(found->second.handle, image);
        }
        found->second.revision = surface.revision;
        return found->second.handle;
    }

    CanvasImage record;
    record.handle = device_.CreateImage(image);
    record.revision = surface.revision;
    canvas_handles_.emplace(&element, record);
    return record.handle;
}

void PaintPage(const LayoutResult& layout, int page_index, TextService& text,
               RenderDevice& device, ImageCache& images, const FormFocusState* focus,
               const SelectionRange* selection) {
    if (!layout.root) {
        return;
    }
    if (page_index < 0 || page_index >= layout.PageCount()) {
        return;
    }

    const float page_top = layout.page_starts[static_cast<size_t>(page_index)];
    float page_height = layout.page_content_height;
    if (page_height <= 0.0f) {
        page_height = layout.content_height;
    }
    if (page_height < 1.0f) {
        page_height = 1.0f;
    }
    // 页底取下一页起点，避免把属于下一页的行盒在本页重复绘制。
    float page_bottom = page_top + page_height;
    const int page_count = layout.PageCount();
    if (page_index + 1 < page_count) {
        page_bottom = std::min(page_bottom, layout.page_starts[static_cast<size_t>(page_index + 1)]);
    }

    float viewport_width = layout.viewport_width;
    if (viewport_width <= 0.0f) {
        viewport_width = static_cast<float>(device.Width());
    }
    if (viewport_width < 1.0f) {
        viewport_width = 1.0f;
    }

    const RectF page_rect = RectF::FromXYWH(0.0f, 0.0f, viewport_width, page_height);

    // 根元素背景传播到页面画布：每一页都铺满同一纸张底色，而非只覆盖内容区。
    const Color canvas_color =
        layout.root->HasBackground() ? layout.root->style.background_color : White();
    device.FillRect(page_rect, canvas_color);
    device.PushClip(page_rect);

    std::map<GlyphKey, GlyphInfo> glyph_info;
    CollectPageGlyphs(*layout.root, page_top, page_bottom, text, layout, &glyph_info);
    // 内嵌文档的文字使用各自的坐标与分页范围，需单独进入图集，否则子文档
    // 绘制时取不到字形位图而只剩空白。
    CollectEmbeddedGlyphs(layout, text, &glyph_info);

    const uint8_t* coverage = text.AtlasCoverage();
    const int atlas_width = text.AtlasWidth();
    const int atlas_height = text.AtlasHeight();
    if (coverage != nullptr && atlas_width > 0 && atlas_height > 0) {
        device.UpdateFontAtlas(coverage, atlas_width, atlas_height);
    }

    PaintContext context;
    context.device = &device;
    context.image_cache = &images;
    context.text_service = &text;
    context.focus = focus;
    context.glyph_info = &glyph_info;
    context.atlas_width = atlas_width;
    context.atlas_height = atlas_height;
    context.page_top = page_top;
    context.page_bottom = page_bottom;
    context.offset_x = 0.0f;
    context.offset_y = 0.0f;
    context.layout = &layout;

    PaintLayoutBox(*layout.root, context, true, true, canvas_color);

    // 选区高亮绘制在内容之上：半透明底色，按行切分。
    if (selection != nullptr && !selection->collapsed) {
        const std::vector<RectF> rects = SelectionRects(layout, page_index, *selection, text);
        const Color highlight = Rgba(0.20f, 0.45f, 0.85f, 0.35f);
        for (const RectF& rect : rects) {
            if (rect.IsEmpty()) {
                continue;
            }
            device.FillRect(rect, highlight);
        }
    }

    device.PopClip();
}

HitResult HitTestPage(const LayoutResult& layout, int page_index, float x, float y,
                      TextService* text) {
    HitResult result;
    if (!layout.root) {
        return result;
    }
    if (page_index < 0 || page_index >= layout.PageCount()) {
        return result;
    }

    const float page_top = layout.page_starts[static_cast<size_t>(page_index)];
    return HitTestLayoutBox(*layout.root, layout, x, y + page_top, text);
}

namespace {

// 在布局树中查找元素对应的盒；行内原子盒也通过 children 递归到达。
const LayoutBox* FindBoxForElement(const LayoutBox& box, const Element* element) {
    if (element == nullptr) {
        return nullptr;
    }
    if (box.element == element) {
        return &box;
    }
    for (const std::unique_ptr<LayoutBox>& child : box.children) {
        if (!child) {
            continue;
        }
        const LayoutBox* found = FindBoxForElement(*child, element);
        if (found != nullptr) {
            return found;
        }
    }
    return nullptr;
}

// 下拉浮层的几何：浮层位于 select 盒下方，最多显示 10 项，超出时随高亮项滚动。
struct SelectPopupLayout {
    bool valid = false;
    RectF popup;
    float item_height = 20.0f;
    int first_visible = 0;
    int visible_count = 0;
    int option_count = 0;
    std::vector<std::string> options;
};

SelectPopupLayout ComputeSelectPopupLayout(const LayoutResult& layout, int page_index,
                                           const SelectPopup& popup) {
    SelectPopupLayout result;
    if (!popup.open || popup.select == nullptr || !layout.root) {
        return result;
    }
    if (page_index < 0 || page_index >= layout.PageCount()) {
        return result;
    }
    const LayoutBox* box = FindBoxForElement(*layout.root, popup.select);
    if (box == nullptr) {
        return result;
    }
    result.options = SelectOptions(*popup.select);
    if (result.options.empty()) {
        return result;
    }
    const int count = static_cast<int>(result.options.size());
    result.option_count = count;
    result.item_height = std::max(20.0f, box->style.font_size + 8.0f);
    const int maximum_visible = 10;
    result.visible_count = std::min(count, maximum_visible);
    if (count > maximum_visible) {
        result.first_visible = popup.highlighted - maximum_visible + 1;
        if (result.first_visible < 0) {
            result.first_visible = 0;
        }
        if (result.first_visible > count - maximum_visible) {
            result.first_visible = count - maximum_visible;
        }
    }
    const float page_top = layout.page_starts[static_cast<size_t>(page_index)];
    const RectF select_page =
        box->border_box.Offset(0.0f, -page_top);
    const float width = std::max(select_page.Width(), 120.0f);
    const float height = result.item_height * static_cast<float>(result.visible_count) + 2.0f;
    result.popup = RectF::FromLTRB(select_page.left, select_page.bottom + 2.0f,
                                   select_page.left + width,
                                   select_page.bottom + 2.0f + height);
    result.valid = true;
    return result;
}

}  // namespace

void PaintSelectPopup(const LayoutResult& layout, int page_index, TextService& text,
                      RenderDevice& device, const SelectPopup& popup) {
    const SelectPopupLayout geometry = ComputeSelectPopupLayout(layout, page_index, popup);
    if (!geometry.valid) {
        return;
    }
    const LayoutBox* box = FindBoxForElement(*layout.root, popup.select);
    if (box == nullptr) {
        return;
    }

    device.FillRect(geometry.popup, White());
    device.StrokeRect(geometry.popup, 1.0f, Rgba(0.46f, 0.46f, 0.46f, 1.0f));

    // 选项文字所用字体与 select 一致。
    FontRequest request;
    request.family = box->style.font_family;
    request.size = box->style.font_size > 0.0f ? box->style.font_size : 16.0f;
    request.bold = box->style.font_weight >= 600;
    request.italic = box->style.italic;
    const FontId font = text.CreateFont(request);
    const FontMetrics metrics = text.Metrics(font);

    std::map<GlyphKey, GlyphInfo> glyph_info;
    for (int index = geometry.first_visible;
         index < geometry.first_visible + geometry.visible_count; ++index) {
        const std::string& option = geometry.options[static_cast<size_t>(index)];
        size_t offset = 0;
        while (offset < option.size()) {
            const Utf8Char character = DecodeUtf8(option, offset);
            if (character.length == 0) {
                break;
            }
            const GlyphKey key(font, character.codepoint);
            if (glyph_info.find(key) == glyph_info.end()) {
                glyph_info.emplace(key, text.Rasterize(font, character.codepoint));
            }
            offset += character.length;
        }
    }
    const uint8_t* coverage = text.AtlasCoverage();
    const int atlas_width = text.AtlasWidth();
    const int atlas_height = text.AtlasHeight();
    if (coverage != nullptr && atlas_width > 0 && atlas_height > 0) {
        device.UpdateFontAtlas(coverage, atlas_width, atlas_height);
    }

    PaintContext context;
    context.device = &device;
    context.text_service = &text;
    context.glyph_info = &glyph_info;
    context.atlas_width = atlas_width;
    context.atlas_height = atlas_height;
    context.page_top = 0.0f;
    context.page_bottom = static_cast<float>(device.Height());
    context.opacity = 1.0f;

    const Color text_color =
        box->style.color.Visible() ? box->style.color : Rgba(0.15f, 0.15f, 0.15f, 1.0f);
    std::vector<GlyphQuad> quads;
    for (int row = 0; row < geometry.visible_count; ++row) {
        const int option_index = geometry.first_visible + row;
        const float top = geometry.popup.top + 1.0f +
                          geometry.item_height * static_cast<float>(row);
        const RectF row_rect = RectF::FromLTRB(geometry.popup.left + 1.0f, top,
                                               geometry.popup.right - 1.0f,
                                               top + geometry.item_height);
        if (option_index == popup.highlighted) {
            device.FillRect(row_rect, Rgba(0.91f, 0.94f, 0.99f, 1.0f));
        }
        const std::string& option = geometry.options[static_cast<size_t>(option_index)];
        PlacedText placed;
        placed.node = nullptr;
        placed.text = option;
        placed.font = font;
        placed.x = row_rect.left + 6.0f;
        placed.baseline =
            top + geometry.item_height * 0.5f + (metrics.ascent - metrics.descent) * 0.5f;
        placed.color = text_color;
        AppendTextQuads(placed, context, &quads);
    }
    device.PushClip(geometry.popup);
    if (!quads.empty()) {
        device.DrawGlyphs(quads.data(), quads.size());
    }
    device.PopClip();
}

int HitTestSelectPopup(const LayoutResult& layout, int page_index, const SelectPopup& popup,
                       float x, float y) {
    const SelectPopupLayout geometry = ComputeSelectPopupLayout(layout, page_index, popup);
    if (!geometry.valid || !geometry.popup.Contains(x, y)) {
        return -1;
    }
    const float local_y = y - (geometry.popup.top + 1.0f);
    if (local_y < 0.0f) {
        return -1;
    }
    const int row = static_cast<int>(local_y / geometry.item_height);
    if (row < 0 || row >= geometry.visible_count) {
        return -1;
    }
    return geometry.first_visible + row;
}

}  // namespace web
