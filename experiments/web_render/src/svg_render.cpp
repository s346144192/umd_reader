// SVG 子集光栅化：把内联 SVG 的图元、描边、渐变、图案与文本绘制到 RenderDevice。
// 渲染设备只提供轴对齐矩形与圆角矩形原语，因此多边形与描边在本文件内按
// 扫描线聚合为像素行的矩形跨度，并做垂直 4 次采样的抗锯齿。
// 坐标约定：几何属性先在自己的用户坐标系中解析，再经变换矩阵换算到页面坐标。

#include "web/svg.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "web/utf8.h"

namespace web {
namespace {

constexpr float kPi = 3.14159265358979323846f;

// ---------------------------------------------------------------------------
// 基础几何
// ---------------------------------------------------------------------------

struct SvgPoint {
    float x = 0.0f;
    float y = 0.0f;

    SvgPoint() = default;
    SvgPoint(float x_value, float y_value) : x(x_value), y(y_value) {}
};

// 二维仿射矩阵：x' = a*x + c*y + e，y' = b*x + d*y + f。
struct SvgMatrix {
    float a = 1.0f;
    float b = 0.0f;
    float c = 0.0f;
    float d = 1.0f;
    float e = 0.0f;
    float f = 0.0f;
};

SvgPoint TransformPoint(const SvgMatrix& matrix, const SvgPoint& point) {
    SvgPoint result;
    result.x = matrix.a * point.x + matrix.c * point.y + matrix.e;
    result.y = matrix.b * point.x + matrix.d * point.y + matrix.f;
    return result;
}

// m1 * m2：先应用 m2，再应用 m1。
SvgMatrix MultiplyMatrix(const SvgMatrix& m1, const SvgMatrix& m2) {
    SvgMatrix result;
    result.a = m1.a * m2.a + m1.c * m2.b;
    result.b = m1.b * m2.a + m1.d * m2.b;
    result.c = m1.a * m2.c + m1.c * m2.d;
    result.d = m1.b * m2.c + m1.d * m2.d;
    result.e = m1.a * m2.e + m1.c * m2.f + m1.e;
    result.f = m1.b * m2.e + m1.d * m2.f + m1.f;
    return result;
}

SvgMatrix TranslationMatrix(float x, float y) {
    SvgMatrix matrix;
    matrix.e = x;
    matrix.f = y;
    return matrix;
}

SvgMatrix ScalingMatrix(float x, float y) {
    SvgMatrix matrix;
    matrix.a = x;
    matrix.d = y;
    return matrix;
}

SvgMatrix RotationMatrix(float degrees) {
    const float radians = degrees * kPi / 180.0f;
    const float cosine = std::cos(radians);
    const float sine = std::sin(radians);
    SvgMatrix matrix;
    matrix.a = cosine;
    matrix.b = sine;
    matrix.c = -sine;
    matrix.d = cosine;
    return matrix;
}

bool InvertMatrix(const SvgMatrix& matrix, SvgMatrix* output) {
    const float determinant = matrix.a * matrix.d - matrix.b * matrix.c;
    if (std::fabs(determinant) < 1e-7f) {
        return false;
    }
    output->a = matrix.d / determinant;
    output->b = -matrix.b / determinant;
    output->c = -matrix.c / determinant;
    output->d = matrix.a / determinant;
    output->e = (matrix.c * matrix.f - matrix.d * matrix.e) / determinant;
    output->f = (matrix.b * matrix.e - matrix.a * matrix.f) / determinant;
    return true;
}

float HorizontalScale(const SvgMatrix& matrix) {
    return std::sqrt(matrix.a * matrix.a + matrix.b * matrix.b);
}

float VerticalScale(const SvgMatrix& matrix) {
    return std::sqrt(matrix.c * matrix.c + matrix.d * matrix.d);
}

struct SvgSubpath {
    std::vector<SvgPoint> points;
    bool closed = false;
};

RectF BoundsOfPoints(const std::vector<SvgSubpath>& subpaths) {
    RectF bounds;
    bool has_point = false;
    for (const SvgSubpath& subpath : subpaths) {
        for (const SvgPoint& point : subpath.points) {
            if (!has_point) {
                bounds.left = point.x;
                bounds.right = point.x;
                bounds.top = point.y;
                bounds.bottom = point.y;
                has_point = true;
            } else {
                bounds.left = std::min(bounds.left, point.x);
                bounds.right = std::max(bounds.right, point.x);
                bounds.top = std::min(bounds.top, point.y);
                bounds.bottom = std::max(bounds.bottom, point.y);
            }
        }
    }
    return bounds;
}

// 变换一组子路径到页面坐标。填充与描边都以页面坐标栅格化。
std::vector<std::vector<SvgPoint>> TransformSubpaths(const std::vector<SvgSubpath>& subpaths,
                                                     const SvgMatrix& matrix) {
    std::vector<std::vector<SvgPoint>> result;
    result.reserve(subpaths.size());
    for (const SvgSubpath& subpath : subpaths) {
        std::vector<SvgPoint> points;
        points.reserve(subpath.points.size());
        for (const SvgPoint& point : subpath.points) {
            points.push_back(TransformPoint(matrix, point));
        }
        result.push_back(std::move(points));
    }
    return result;
}

RectF BoundsOfPolygons(const std::vector<std::vector<SvgPoint>>& polygons) {
    RectF bounds;
    bool has_point = false;
    for (const std::vector<SvgPoint>& polygon : polygons) {
        for (const SvgPoint& point : polygon) {
            if (!has_point) {
                bounds.left = point.x;
                bounds.right = point.x;
                bounds.top = point.y;
                bounds.bottom = point.y;
                has_point = true;
            } else {
                bounds.left = std::min(bounds.left, point.x);
                bounds.right = std::max(bounds.right, point.x);
                bounds.top = std::min(bounds.top, point.y);
                bounds.bottom = std::max(bounds.bottom, point.y);
            }
        }
    }
    return bounds;
}

// ---------------------------------------------------------------------------
// 数值与属性解析
// ---------------------------------------------------------------------------

void SkipNumberSeparators(const std::string& text, size_t* index) {
    while (*index < text.size()) {
        const char character = text[*index];
        if (character == ' ' || character == '\t' || character == '\n' || character == '\r' ||
            character == ',') {
            ++(*index);
        } else {
            break;
        }
    }
}

// 读取一个数值。支持符号、小数与指数，例如 "-1.5e-3"。
bool ReadNumber(const std::string& text, size_t* index, float* output) {
    SkipNumberSeparators(text, index);
    const size_t begin = *index;
    if (*index < text.size() && (text[*index] == '+' || text[*index] == '-')) {
        ++(*index);
    }
    bool has_digit = false;
    while (*index < text.size() && text[*index] >= '0' && text[*index] <= '9') {
        has_digit = true;
        ++(*index);
    }
    if (*index < text.size() && text[*index] == '.') {
        ++(*index);
        while (*index < text.size() && text[*index] >= '0' && text[*index] <= '9') {
            has_digit = true;
            ++(*index);
        }
    }
    if (!has_digit) {
        *index = begin;
        return false;
    }
    if (*index < text.size() && (text[*index] == 'e' || text[*index] == 'E')) {
        size_t exponent = *index + 1;
        if (exponent < text.size() && (text[exponent] == '+' || text[exponent] == '-')) {
            ++exponent;
        }
        if (exponent < text.size() && text[exponent] >= '0' && text[exponent] <= '9') {
            *index = exponent;
            while (*index < text.size() && text[*index] >= '0' && text[*index] <= '9') {
                ++(*index);
            }
        }
    }
    *output = std::strtof(text.substr(begin, *index - begin).c_str(), nullptr);
    return true;
}

bool ReadNumberList(const std::string& text, std::vector<float>* values) {
    size_t index = 0;
    while (index < text.size()) {
        float value = 0.0f;
        const size_t before = index;
        SkipNumberSeparators(text, &index);
        if (index >= text.size()) {
            break;
        }
        if (!ReadNumber(text, &index, &value)) {
            index = before + 1;
            continue;
        }
        values->push_back(value);
    }
    return !values->empty();
}

float AttributeNumber(const Element& element, const char* name, float fallback) {
    if (!element.HasAttribute(name)) {
        return fallback;
    }
    std::string text = element.GetAttribute(name);
    TrimAsciiSpace(&text);
    if (text.empty()) {
        return fallback;
    }
    // 数值可以带 px 单位；百分比在几何属性中按回退值处理。
    if (text.size() > 1 && text.back() == '%') {
        return fallback;
    }
    if (text.size() > 2 && (text[text.size() - 2] == 'p' || text[text.size() - 2] == 'P') &&
        (text.back() == 'x' || text.back() == 'X')) {
        text.resize(text.size() - 2);
        TrimAsciiSpace(&text);
    }
    size_t index = 0;
    float value = fallback;
    if (!ReadNumber(text, &index, &value)) {
        return fallback;
    }
    return value;
}

// 读取可能带百分号的数值：percent_scale 为 100% 对应的数值。
float AttributeRatio(const Element& element, const char* name, float fallback,
                     float percent_scale) {
    if (!element.HasAttribute(name)) {
        return fallback;
    }
    std::string text = element.GetAttribute(name);
    TrimAsciiSpace(&text);
    if (text.empty()) {
        return fallback;
    }
    bool percent = false;
    if (text.back() == '%') {
        percent = true;
        text.resize(text.size() - 1);
    }
    size_t index = 0;
    float value = fallback;
    if (!ReadNumber(text, &index, &value)) {
        return fallback;
    }
    if (percent) {
        return value * percent_scale / 100.0f;
    }
    return value;
}

// ---------------------------------------------------------------------------
// 颜色解析
// ---------------------------------------------------------------------------

int HexDigit(char character) {
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }
    return -1;
}

bool ParseHexColor(const std::string& text, Color* output) {
    std::string digits = text.substr(1);
    if (digits.size() == 3) {
        std::string expanded;
        for (char digit : digits) {
            expanded.push_back(digit);
            expanded.push_back(digit);
        }
        digits = expanded;
    }
    if (digits.size() != 6 && digits.size() != 8) {
        return false;
    }
    int components[4] = {0, 0, 0, 255};
    for (size_t index = 0; index < digits.size() / 2; ++index) {
        const int high = HexDigit(digits[index * 2]);
        const int low = HexDigit(digits[index * 2 + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        components[index] = high * 16 + low;
    }
    output->r = static_cast<float>(components[0]) / 255.0f;
    output->g = static_cast<float>(components[1]) / 255.0f;
    output->b = static_cast<float>(components[2]) / 255.0f;
    output->a = static_cast<float>(components[3]) / 255.0f;
    return true;
}

bool ParseColorFunction(const std::string& text, Color* output) {
    const size_t open = text.find('(');
    if (open == std::string::npos || text.empty() || text.back() != ')') {
        return false;
    }
    const std::string name = AsciiLower(text.substr(0, open));
    if (name != "rgb" && name != "rgba") {
        return false;
    }
    std::vector<std::string> parts;
    std::string current;
    for (size_t index = open + 1; index + 1 < text.size(); ++index) {
        const char character = text[index];
        if (character == ',') {
            parts.push_back(current);
            current.clear();
        } else {
            current.push_back(character);
        }
    }
    parts.push_back(current);
    if (parts.size() != 3 && parts.size() != 4) {
        return false;
    }
    float values[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    for (size_t index = 0; index < parts.size(); ++index) {
        std::string part = parts[index];
        TrimAsciiSpace(&part);
        bool percent = !part.empty() && part.back() == '%';
        if (percent) {
            part.resize(part.size() - 1);
        }
        char* end = nullptr;
        const float number = std::strtof(part.c_str(), &end);
        if (end == part.c_str() || end == nullptr || *end != '\0') {
            return false;
        }
        values[index] = percent ? number / 100.0f : (index == 3 ? number : number / 255.0f);
    }
    output->r = std::min(std::max(values[0], 0.0f), 1.0f);
    output->g = std::min(std::max(values[1], 0.0f), 1.0f);
    output->b = std::min(std::max(values[2], 0.0f), 1.0f);
    output->a = std::min(std::max(values[3], 0.0f), 1.0f);
    return true;
}

bool ParseNamedColor(const std::string& name, Color* output) {
    if (name == "black") {
        *output = Black();
        return true;
    }
    if (name == "white") {
        *output = White();
        return true;
    }
    if (name == "transparent") {
        *output = Transparent();
        return true;
    }
    if (name == "currentcolor") {
        return false;
    }
    return false;
}

bool ParseSvgColor(const std::string& text, Color* output) {
    std::string value = text;
    TrimAsciiSpace(&value);
    if (value.empty()) {
        return false;
    }
    const std::string lowered = AsciiLower(value);
    if (!value.empty() && value[0] == '#') {
        return ParseHexColor(value, output);
    }
    if (lowered.rfind("rgb", 0) == 0) {
        return ParseColorFunction(lowered, output);
    }
    return ParseNamedColor(lowered, output);
}

// 判断并拆分 url(#id) 引用。返回真表示该值是一条引用。
bool ParsePaintReference(const std::string& text, std::string* identifier) {
    std::string value = text;
    TrimAsciiSpace(&value);
    if (value.rfind("url(", 0) != 0 || value.empty() || value.back() != ')') {
        return false;
    }
    std::string inner = value.substr(4, value.size() - 5);
    TrimAsciiSpace(&inner);
    if (!inner.empty() && (inner.front() == '"' || inner.front() == '\'')) {
        inner.erase(inner.begin());
    }
    if (!inner.empty() && (inner.back() == '"' || inner.back() == '\'')) {
        inner.pop_back();
    }
    TrimAsciiSpace(&inner);
    if (inner.empty() || inner[0] != '#') {
        return false;
    }
    identifier->assign(inner.begin() + 1, inner.end());
    return true;
}

// ---------------------------------------------------------------------------
// 呈现样式
// ---------------------------------------------------------------------------

struct SvgGradientStop {
    float offset = 0.0f;
    Color color = Black();
};

struct SvgStyle {
    bool fill_none = false;
    Color fill = Black();
    std::string fill_reference;
    float fill_opacity = 1.0f;

    bool stroke_none = true;
    Color stroke = Transparent();
    std::string stroke_reference;
    float stroke_opacity = 1.0f;
    float stroke_width = 1.0f;
    SvgLineCap line_cap = SvgLineCap::Butt;
    SvgLineJoin line_join = SvgLineJoin::Miter;
    std::vector<float> dash_array;
    float dash_offset = 0.0f;

    // 元素自身的 opacity，不随继承传递。
    float opacity = 1.0f;

    SvgTextAnchor text_anchor = SvgTextAnchor::Start;
    SvgBaseline baseline = SvgBaseline::Alphabetic;
    bool stroke_first = false;
    SvgPointerEvents pointer_events = SvgPointerEvents::Auto;

    FontRequest font;
};

// 取字体族列表中的第一项，去掉引号与空白。
std::string FirstFontFamily(const std::string& text) {
    std::string value = text;
    const size_t comma = value.find(',');
    if (comma != std::string::npos) {
        value.resize(comma);
    }
    TrimAsciiSpace(&value);
    if (!value.empty() && (value.front() == '"' || value.front() == '\'')) {
        value.erase(value.begin());
    }
    if (!value.empty() && (value.back() == '"' || value.back() == '\'')) {
        value.pop_back();
    }
    TrimAsciiSpace(&value);
    return value;
}

bool ParsePaintAttribute(const std::string& text, bool* is_none, Color* color,
                         std::string* reference) {
    std::string value = text;
    TrimAsciiSpace(&value);
    if (value.empty()) {
        return false;
    }
    if (AsciiLower(value) == "none") {
        *is_none = true;
        reference->clear();
        return true;
    }
    std::string identifier;
    if (ParsePaintReference(value, &identifier)) {
        *is_none = false;
        reference->assign(identifier);
        return true;
    }
    if (ParseSvgColor(value, color)) {
        *is_none = false;
        reference->clear();
        return true;
    }
    return false;
}

// 从元素上读取呈现属性。属性名在 DOM 中统一小写。
void ApplyPresentationAttributes(const Element& element, SvgStyle* style) {
    if (element.HasAttribute("fill")) {
        bool none = false;
        Color color = style->fill;
        std::string reference;
        if (ParsePaintAttribute(element.GetAttribute("fill"), &none, &color, &reference)) {
            style->fill_none = none;
            style->fill = color;
            style->fill_reference = reference;
        }
    }
    if (element.HasAttribute("fill-opacity")) {
        const float value = AttributeNumber(element, "fill-opacity", style->fill_opacity);
        style->fill_opacity = std::min(std::max(value, 0.0f), 1.0f);
    }
    if (element.HasAttribute("stroke")) {
        bool none = false;
        Color color = style->stroke;
        std::string reference;
        if (ParsePaintAttribute(element.GetAttribute("stroke"), &none, &color, &reference)) {
            style->stroke_none = none;
            style->stroke = color;
            style->stroke_reference = reference;
        }
    }
    if (element.HasAttribute("stroke-opacity")) {
        const float value = AttributeNumber(element, "stroke-opacity", style->stroke_opacity);
        style->stroke_opacity = std::min(std::max(value, 0.0f), 1.0f);
    }
    if (element.HasAttribute("stroke-width")) {
        style->stroke_width = std::max(AttributeNumber(element, "stroke-width", 1.0f), 0.0f);
    }
    if (element.HasAttribute("stroke-linecap")) {
        const std::string value = AsciiLower(element.GetAttribute("stroke-linecap"));
        if (value == "round") {
            style->line_cap = SvgLineCap::Round;
        } else if (value == "square") {
            style->line_cap = SvgLineCap::Square;
        } else {
            style->line_cap = SvgLineCap::Butt;
        }
    }
    if (element.HasAttribute("stroke-linejoin")) {
        const std::string value = AsciiLower(element.GetAttribute("stroke-linejoin"));
        if (value == "round") {
            style->line_join = SvgLineJoin::Round;
        } else if (value == "bevel") {
            style->line_join = SvgLineJoin::Bevel;
        } else {
            style->line_join = SvgLineJoin::Miter;
        }
    }
    if (element.HasAttribute("stroke-dasharray")) {
        std::vector<float> dashes;
        const std::string value = AsciiLower(element.GetAttribute("stroke-dasharray"));
        if (value == "none") {
            style->dash_array.clear();
        } else if (ReadNumberList(element.GetAttribute("stroke-dasharray"), &dashes)) {
            style->dash_array.clear();
            for (float dash : dashes) {
                if (dash >= 0.0f) {
                    style->dash_array.push_back(dash);
                }
            }
        }
    }
    if (element.HasAttribute("stroke-dashoffset")) {
        style->dash_offset = AttributeNumber(element, "stroke-dashoffset", 0.0f);
    }
    if (element.HasAttribute("opacity")) {
        style->opacity = std::min(std::max(AttributeNumber(element, "opacity", 1.0f), 0.0f), 1.0f);
    }
    if (element.HasAttribute("text-anchor")) {
        const std::string value = AsciiLower(element.GetAttribute("text-anchor"));
        if (value == "middle") {
            style->text_anchor = SvgTextAnchor::Middle;
        } else if (value == "end") {
            style->text_anchor = SvgTextAnchor::End;
        } else {
            style->text_anchor = SvgTextAnchor::Start;
        }
    }
    if (element.HasAttribute("dominant-baseline")) {
        const std::string value = AsciiLower(element.GetAttribute("dominant-baseline"));
        if (value == "central") {
            style->baseline = SvgBaseline::Central;
        } else if (value == "middle") {
            style->baseline = SvgBaseline::Middle;
        } else if (value == "hanging") {
            style->baseline = SvgBaseline::Hanging;
        } else {
            style->baseline = SvgBaseline::Alphabetic;
        }
    }
    if (element.HasAttribute("paint-order")) {
        const std::string value = AsciiLower(element.GetAttribute("paint-order"));
        style->stroke_first = value.rfind("stroke", 0) == 0;
    }
    if (element.HasAttribute("pointer-events")) {
        const std::string value = AsciiLower(element.GetAttribute("pointer-events"));
        if (value == "none") {
            style->pointer_events = SvgPointerEvents::None;
        } else if (value == "visiblepainted") {
            style->pointer_events = SvgPointerEvents::VisiblePainted;
        } else if (value == "visiblefill") {
            style->pointer_events = SvgPointerEvents::VisibleFill;
        } else if (value == "visiblestroke") {
            style->pointer_events = SvgPointerEvents::VisibleStroke;
        } else if (value == "fill") {
            style->pointer_events = SvgPointerEvents::Fill;
        } else if (value == "stroke") {
            style->pointer_events = SvgPointerEvents::Stroke;
        } else if (value == "all") {
            style->pointer_events = SvgPointerEvents::All;
        } else if (value == "bounding-box") {
            style->pointer_events = SvgPointerEvents::BoundingBox;
        } else {
            style->pointer_events = SvgPointerEvents::Auto;
        }
    }
    if (element.HasAttribute("font-size")) {
        style->font.size = std::max(AttributeNumber(element, "font-size", style->font.size), 0.0f);
    }
    if (element.HasAttribute("font-family")) {
        style->font.family = FirstFontFamily(element.GetAttribute("font-family"));
    }
    if (element.HasAttribute("font-weight")) {
        const std::string value = AsciiLower(element.GetAttribute("font-weight"));
        if (value == "bold" || value == "bolder") {
            style->font.bold = true;
        } else if (value == "normal" || value == "lighter") {
            style->font.bold = false;
        } else {
            style->font.bold = AttributeNumber(element, "font-weight", 400.0f) >= 600.0f;
        }
    }
}

// CSS 声明优先于同名呈现属性：只覆盖本元素上确实声明过的项目。
void ApplyComputedStyle(const ComputedStyle& style, SvgStyle* output) {
    if (style.has_svg_fill) {
        output->fill_none = style.svg_fill_none;
        output->fill = style.svg_fill;
        if (style.svg_fill_none) {
            output->fill_reference.clear();
        }
    }
    if (style.has_svg_fill_opacity) {
        output->fill_opacity = style.svg_fill_opacity;
    }
    if (style.has_svg_stroke) {
        output->stroke_none = style.svg_stroke_none;
        output->stroke = style.svg_stroke;
        if (style.svg_stroke_none) {
            output->stroke_reference.clear();
        }
    }
    if (style.has_svg_stroke_opacity) {
        output->stroke_opacity = style.svg_stroke_opacity;
    }
    if (style.has_svg_stroke_width) {
        output->stroke_width = style.svg_stroke_width;
    }
    if (style.has_svg_line_cap) {
        output->line_cap = style.svg_line_cap;
    }
    if (style.has_svg_line_join) {
        output->line_join = style.svg_line_join;
    }
    if (style.has_svg_dash_array) {
        output->dash_array = style.svg_dash_array;
    }
    if (style.has_svg_dash_offset) {
        output->dash_offset = style.svg_dash_offset;
    }
    if (style.has_svg_text_anchor) {
        output->text_anchor = style.svg_text_anchor;
    }
    if (style.has_svg_baseline) {
        output->baseline = style.svg_baseline;
    }
    if (style.has_svg_paint_order) {
        output->stroke_first = style.svg_stroke_first;
    }
    if (style.has_svg_pointer_events) {
        output->pointer_events = style.svg_pointer_events;
    }
    output->opacity *= style.opacity;
    // 只有本元素声明过的字体属性才覆盖呈现属性；未声明的字体由父级
    // SvgStyle 继承而来，保证元素上的 font-size 等属性优先于继承值。
    if (style.has_svg_font_family) {
        output->font.family = style.font_family;
    }
    if (style.has_svg_font_size) {
        output->font.size = style.font_size;
    }
    if (style.has_svg_font_weight) {
        output->font.bold = style.font_weight >= 600;
    }
    if (style.has_svg_font_style) {
        output->font.italic = style.italic;
    }
}

SvgStyle ResolveStyle(const Element& element, const SvgStyle& parent,
                      const ComputedStyle* computed) {
    // 可继承属性先取父级结果；opacity 不继承。
    SvgStyle style = parent;
    style.opacity = 1.0f;
    ApplyPresentationAttributes(element, &style);
    if (computed != nullptr) {
        ApplyComputedStyle(*computed, &style);
    }
    return style;
}

// ---------------------------------------------------------------------------
// transform 属性
// ---------------------------------------------------------------------------

// 读取函数名之后的括号参数。
bool ReadTransformArguments(const std::string& text, size_t* index,
                            std::vector<float>* arguments) {
    SkipNumberSeparators(text, index);
    if (*index >= text.size() || text[*index] != '(') {
        return false;
    }
    ++(*index);
    while (*index < text.size() && text[*index] != ')') {
        float value = 0.0f;
        if (!ReadNumber(text, index, &value)) {
            ++(*index);
            continue;
        }
        arguments->push_back(value);
    }
    if (*index < text.size() && text[*index] == ')') {
        ++(*index);
    }
    return true;
}

SvgMatrix ParseTransform(const std::string& text) {
    SvgMatrix result;
    size_t index = 0;
    while (index < text.size()) {
        while (index < text.size() &&
               (text[index] == ' ' || text[index] == '\t' || text[index] == '\n' ||
                text[index] == '\r' || text[index] == ',')) {
            ++index;
        }
        const size_t name_begin = index;
        while (index < text.size() && ((text[index] >= 'a' && text[index] <= 'z') ||
                                       (text[index] >= 'A' && text[index] <= 'Z'))) {
            ++index;
        }
        if (index == name_begin) {
            break;
        }
        const std::string name = AsciiLower(text.substr(name_begin, index - name_begin));
        std::vector<float> arguments;
        if (!ReadTransformArguments(text, &index, &arguments)) {
            break;
        }
        SvgMatrix operation;
        if (name == "translate" && !arguments.empty()) {
            operation = TranslationMatrix(arguments[0],
                                          arguments.size() > 1 ? arguments[1] : 0.0f);
        } else if (name == "scale" && !arguments.empty()) {
            operation = ScalingMatrix(arguments[0],
                                      arguments.size() > 1 ? arguments[1] : arguments[0]);
        } else if (name == "rotate" && !arguments.empty()) {
            const SvgMatrix rotation = RotationMatrix(arguments[0]);
            if (arguments.size() >= 3) {
                const SvgMatrix to_origin = TranslationMatrix(-arguments[1], -arguments[2]);
                const SvgMatrix back = TranslationMatrix(arguments[1], arguments[2]);
                operation = MultiplyMatrix(back, MultiplyMatrix(rotation, to_origin));
            } else {
                operation = rotation;
            }
        } else if (name == "matrix" && arguments.size() >= 6) {
            operation.a = arguments[0];
            operation.b = arguments[1];
            operation.c = arguments[2];
            operation.d = arguments[3];
            operation.e = arguments[4];
            operation.f = arguments[5];
        } else if (name == "skewx" && !arguments.empty()) {
            operation.c = std::tan(arguments[0] * kPi / 180.0f);
        } else if (name == "skewy" && !arguments.empty()) {
            operation.b = std::tan(arguments[0] * kPi / 180.0f);
        } else {
            continue;
        }
        result = MultiplyMatrix(result, operation);
    }
    return result;
}

// ---------------------------------------------------------------------------
// 路径与图元
// ---------------------------------------------------------------------------

void AppendPoint(std::vector<SvgPoint>* points, float x, float y) {
    if (!points->empty()) {
        const SvgPoint& back = points->back();
        if (std::fabs(back.x - x) < 1e-4f && std::fabs(back.y - y) < 1e-4f) {
            return;
        }
    }
    SvgPoint point;
    point.x = x;
    point.y = y;
    points->push_back(point);
}

void FlattenCubic(const SvgPoint& p0, const SvgPoint& p1, const SvgPoint& p2,
                  const SvgPoint& p3, std::vector<SvgPoint>* points) {
    const float estimate = std::fabs(p0.x - p1.x) + std::fabs(p0.y - p1.y) +
                           std::fabs(p1.x - p2.x) + std::fabs(p1.y - p2.y) +
                           std::fabs(p2.x - p3.x) + std::fabs(p2.y - p3.y);
    int steps = static_cast<int>(estimate / 3.0f) + 4;
    steps = std::min(std::max(steps, 4), 128);
    for (int step = 1; step <= steps; ++step) {
        const float t = static_cast<float>(step) / static_cast<float>(steps);
        const float u = 1.0f - t;
        const float x = u * u * u * p0.x + 3.0f * u * u * t * p1.x + 3.0f * u * t * t * p2.x +
                        t * t * t * p3.x;
        const float y = u * u * u * p0.y + 3.0f * u * u * t * p1.y + 3.0f * u * t * t * p2.y +
                        t * t * t * p3.y;
        AppendPoint(points, x, y);
    }
}

void FlattenQuadratic(const SvgPoint& p0, const SvgPoint& p1, const SvgPoint& p2,
                      std::vector<SvgPoint>* points) {
    const SvgPoint c1(p0.x + (p1.x - p0.x) * 2.0f / 3.0f, p0.y + (p1.y - p0.y) * 2.0f / 3.0f);
    const SvgPoint c2(p2.x + (p1.x - p2.x) * 2.0f / 3.0f, p2.y + (p1.y - p2.y) * 2.0f / 3.0f);
    FlattenCubic(p0, c1, c2, p2, points);
}

// 椭圆弧按端点参数化转换为多段折线。算法与 SVG 规范的端点参数转换一致。
void FlattenArc(const SvgPoint& from, float radius_x, float radius_y, float rotation,
                bool large_arc, bool sweep, const SvgPoint& to,
                std::vector<SvgPoint>* points) {
    if (radius_x <= 0.0f || radius_y <= 0.0f) {
        AppendPoint(points, to.x, to.y);
        return;
    }
    const float radians = rotation * kPi / 180.0f;
    const float cosine = std::cos(radians);
    const float sine = std::sin(radians);
    const float dx = (from.x - to.x) * 0.5f;
    const float dy = (from.y - to.y) * 0.5f;
    const float x1 = cosine * dx + sine * dy;
    const float y1 = -sine * dx + cosine * dy;

    float rx = std::fabs(radius_x);
    float ry = std::fabs(radius_y);
    const float lambda = (x1 * x1) / (rx * rx) + (y1 * y1) / (ry * ry);
    if (lambda > 1.0f) {
        const float scale = std::sqrt(lambda);
        rx *= scale;
        ry *= scale;
    }

    const float rx_square = rx * rx;
    const float ry_square = ry * ry;
    const float numerator = rx_square * ry_square - rx_square * y1 * y1 - ry_square * x1 * x1;
    const float denominator = rx_square * y1 * y1 + ry_square * x1 * x1;
    float factor = denominator > 0.0f ? std::sqrt(std::max(numerator / denominator, 0.0f)) : 0.0f;
    if (large_arc == sweep) {
        factor = -factor;
    }
    const float center_x1 = factor * rx * y1 / ry;
    const float center_y1 = -factor * ry * x1 / rx;
    const float center_x = cosine * center_x1 - sine * center_y1 + (from.x + to.x) * 0.5f;
    const float center_y = sine * center_x1 + cosine * center_y1 + (from.y + to.y) * 0.5f;

    const float start_vector_x = (x1 - center_x1) / rx;
    const float start_vector_y = (y1 - center_y1) / ry;
    const float end_vector_x = (-x1 - center_x1) / rx;
    const float end_vector_y = (-y1 - center_y1) / ry;
    float start_angle = std::atan2(start_vector_y, start_vector_x);
    float end_angle = std::atan2(end_vector_y, end_vector_x);
    float sweep_angle = end_angle - start_angle;
    if (!sweep && sweep_angle > 0.0f) {
        sweep_angle -= 2.0f * kPi;
    } else if (sweep && sweep_angle < 0.0f) {
        sweep_angle += 2.0f * kPi;
    }

    const float approximate_length = std::fabs(sweep_angle) * std::max(rx, ry);
    int steps = static_cast<int>(approximate_length / 3.0f) + 4;
    steps = std::min(std::max(steps, 4), 256);
    for (int step = 1; step <= steps; ++step) {
        const float angle =
            start_angle + sweep_angle * static_cast<float>(step) / static_cast<float>(steps);
        const float local_x = rx * std::cos(angle);
        const float local_y = ry * std::sin(angle);
        const float x = cosine * local_x - sine * local_y + center_x;
        const float y = sine * local_x + cosine * local_y + center_y;
        AppendPoint(points, x, y);
    }
}

void AddEllipsePoints(float center_x, float center_y, float radius_x, float radius_y,
                      int segments, std::vector<SvgPoint>* points) {
    const int steps = std::max(segments, 8);
    for (int step = 0; step < steps; ++step) {
        const float angle = 2.0f * kPi * static_cast<float>(step) / static_cast<float>(steps);
        points->push_back(SvgPoint(center_x + radius_x * std::cos(angle),
                                   center_y + radius_y * std::sin(angle)));
    }
}

std::vector<SvgSubpath> BuildPathGeometry(const std::string& data) {
    std::vector<SvgSubpath> subpaths;
    SvgSubpath current;
    SvgPoint current_point;
    SvgPoint start_point;
    SvgPoint previous_control;
    bool has_control = false;
    size_t index = 0;
    char command = 0;

    while (index < data.size()) {
        while (index < data.size() &&
               (data[index] == ' ' || data[index] == '\t' || data[index] == '\n' ||
                data[index] == '\r' || data[index] == ',')) {
            ++index;
        }
        if (index >= data.size()) {
            break;
        }
        const char next = data[index];
        if ((next >= 'a' && next <= 'z') || (next >= 'A' && next <= 'Z')) {
            command = next;
            ++index;
        } else if (command == 0) {
            break;
        }

        const bool relative = command >= 'a' && command <= 'z';
        const char upper = static_cast<char>(command >= 'a' && command <= 'z' ? command - 'a' + 'A'
                                                                            : command);
        auto read_point = [&](SvgPoint* point) {
            float x = 0.0f;
            float y = 0.0f;
            if (!ReadNumber(data, &index, &x) || !ReadNumber(data, &index, &y)) {
                return false;
            }
            point->x = relative ? current_point.x + x : x;
            point->y = relative ? current_point.y + y : y;
            return true;
        };

        if (upper == 'Z') {
            if (!current.points.empty()) {
                current.closed = true;
                subpaths.push_back(current);
                current = SvgSubpath();
            }
            current_point = start_point;
            has_control = false;
            continue;
        }
        if (upper == 'M') {
            SvgPoint point;
            if (!read_point(&point)) {
                break;
            }
            if (!current.points.empty()) {
                subpaths.push_back(current);
                current = SvgSubpath();
            }
            current.points.push_back(point);
            current_point = point;
            start_point = point;
            has_control = false;
            // 后续坐标对按隐式 lineto 处理。
            command = relative ? 'l' : 'L';
            continue;
        }
        if (upper == 'L') {
            SvgPoint point;
            if (!read_point(&point)) {
                break;
            }
            if (current.points.empty()) {
                current.points.push_back(current_point);
            }
            AppendPoint(&current.points, point.x, point.y);
            current_point = point;
            has_control = false;
            continue;
        }
        if (upper == 'H') {
            float x = 0.0f;
            if (!ReadNumber(data, &index, &x)) {
                break;
            }
            const float target = relative ? current_point.x + x : x;
            if (current.points.empty()) {
                current.points.push_back(current_point);
            }
            AppendPoint(&current.points, target, current_point.y);
            current_point.x = target;
            has_control = false;
            continue;
        }
        if (upper == 'V') {
            float y = 0.0f;
            if (!ReadNumber(data, &index, &y)) {
                break;
            }
            const float target = relative ? current_point.y + y : y;
            if (current.points.empty()) {
                current.points.push_back(current_point);
            }
            AppendPoint(&current.points, current_point.x, target);
            current_point.y = target;
            has_control = false;
            continue;
        }
        if (upper == 'C' || upper == 'S') {
            SvgPoint control1;
            SvgPoint control2;
            SvgPoint end;
            if (upper == 'C') {
                if (!read_point(&control1) || !read_point(&control2) || !read_point(&end)) {
                    break;
                }
            } else {
                if (!read_point(&control2) || !read_point(&end)) {
                    break;
                }
                if (has_control) {
                    control1.x = current_point.x * 2.0f - previous_control.x;
                    control1.y = current_point.y * 2.0f - previous_control.y;
                } else {
                    control1 = current_point;
                }
            }
            if (current.points.empty()) {
                current.points.push_back(current_point);
            }
            FlattenCubic(current_point, control1, control2, end, &current.points);
            previous_control = control2;
            has_control = true;
            current_point = end;
            continue;
        }
        if (upper == 'Q' || upper == 'T') {
            SvgPoint control;
            SvgPoint end;
            if (upper == 'Q') {
                if (!read_point(&control) || !read_point(&end)) {
                    break;
                }
            } else {
                if (!read_point(&end)) {
                    break;
                }
                if (has_control) {
                    control.x = current_point.x * 2.0f - previous_control.x;
                    control.y = current_point.y * 2.0f - previous_control.y;
                } else {
                    control = current_point;
                }
            }
            if (current.points.empty()) {
                current.points.push_back(current_point);
            }
            FlattenQuadratic(current_point, control, end, &current.points);
            previous_control = control;
            has_control = true;
            current_point = end;
            continue;
        }
        if (upper == 'A') {
            float radius_x = 0.0f;
            float radius_y = 0.0f;
            float rotation = 0.0f;
            float large_arc = 0.0f;
            float sweep = 0.0f;
            SvgPoint end;
            if (!ReadNumber(data, &index, &radius_x) || !ReadNumber(data, &index, &radius_y) ||
                !ReadNumber(data, &index, &rotation) || !ReadNumber(data, &index, &large_arc) ||
                !ReadNumber(data, &index, &sweep) || !read_point(&end)) {
                break;
            }
            if (current.points.empty()) {
                current.points.push_back(current_point);
            }
            FlattenArc(current_point, radius_x, radius_y, rotation, large_arc != 0.0f,
                       sweep != 0.0f, end, &current.points);
            current_point = end;
            has_control = false;
            continue;
        }
        // 未知命令：跳过该字符，避免死循环。
        ++index;
    }
    if (current.points.size() >= 2) {
        subpaths.push_back(current);
    }
    return subpaths;
}

// 图元几何。is_circle 与圆心半径用于小圆的圆角矩形快速路径。
struct ShapeGeometry {
    std::vector<SvgSubpath> subpaths;
    bool is_circle = false;
    SvgPoint circle_center;
    float circle_radius = 0.0f;
};

ShapeGeometry BuildShapeGeometry(const Element& element) {
    ShapeGeometry geometry;
    const std::string& tag = element.TagName();
    if (tag == "rect") {
        const float x = AttributeNumber(element, "x", 0.0f);
        const float y = AttributeNumber(element, "y", 0.0f);
        const float width = AttributeNumber(element, "width", 0.0f);
        const float height = AttributeNumber(element, "height", 0.0f);
        if (width <= 0.0f || height <= 0.0f) {
            return geometry;
        }
        float radius_x = AttributeNumber(element, "rx", -1.0f);
        float radius_y = AttributeNumber(element, "ry", -1.0f);
        if (radius_x < 0.0f && radius_y < 0.0f) {
            radius_x = 0.0f;
            radius_y = 0.0f;
        } else if (radius_x < 0.0f) {
            radius_x = radius_y;
        } else if (radius_y < 0.0f) {
            radius_y = radius_x;
        }
        radius_x = std::min(std::max(radius_x, 0.0f), width * 0.5f);
        radius_y = std::min(std::max(radius_y, 0.0f), height * 0.5f);
        SvgSubpath subpath;
        if (radius_x <= 0.0f || radius_y <= 0.0f) {
            AppendPoint(&subpath.points, x, y);
            AppendPoint(&subpath.points, x + width, y);
            AppendPoint(&subpath.points, x + width, y + height);
            AppendPoint(&subpath.points, x, y + height);
        } else {
            const int steps = 8;
            // 左上角
            for (int step = 0; step <= steps; ++step) {
                const float angle = kPi + kPi * 0.5f * static_cast<float>(step) /
                                                 static_cast<float>(steps);
                AppendPoint(&subpath.points, x + radius_x + radius_x * std::cos(angle),
                            y + radius_y + radius_y * std::sin(angle));
            }
            // 右上角
            for (int step = 0; step <= steps; ++step) {
                const float angle = -kPi * 0.5f + kPi * 0.5f * static_cast<float>(step) /
                                                       static_cast<float>(steps);
                AppendPoint(&subpath.points, x + width - radius_x + radius_x * std::cos(angle),
                            y + radius_y + radius_y * std::sin(angle));
            }
            // 右下角
            for (int step = 0; step <= steps; ++step) {
                const float angle = kPi * 0.5f * static_cast<float>(step) /
                                    static_cast<float>(steps);
                AppendPoint(&subpath.points, x + width - radius_x + radius_x * std::cos(angle),
                            y + height - radius_y + radius_y * std::sin(angle));
            }
            // 左下角
            for (int step = 0; step <= steps; ++step) {
                const float angle = kPi * 0.5f + kPi * 0.5f * static_cast<float>(step) /
                                                       static_cast<float>(steps);
                AppendPoint(&subpath.points, x + radius_x + radius_x * std::cos(angle),
                            y + height - radius_y + radius_y * std::sin(angle));
            }
        }
        subpath.closed = true;
        geometry.subpaths.push_back(std::move(subpath));
        return geometry;
    }
    if (tag == "circle" || tag == "ellipse") {
        const float center_x = AttributeNumber(element, "cx", 0.0f);
        const float center_y = AttributeNumber(element, "cy", 0.0f);
        float radius_x = 0.0f;
        float radius_y = 0.0f;
        if (tag == "circle") {
            radius_x = AttributeNumber(element, "r", 0.0f);
            radius_y = radius_x;
        } else {
            radius_x = AttributeNumber(element, "rx", 0.0f);
            radius_y = AttributeNumber(element, "ry", 0.0f);
        }
        if (radius_x <= 0.0f || radius_y <= 0.0f) {
            return geometry;
        }
        SvgSubpath subpath;
        const float extent = std::max(radius_x, radius_y);
        int segments = static_cast<int>(extent * 1.5f) + 12;
        segments = std::min(std::max(segments, 16), 192);
        AddEllipsePoints(center_x, center_y, radius_x, radius_y, segments, &subpath.points);
        subpath.closed = true;
        geometry.subpaths.push_back(std::move(subpath));
        if (tag == "circle") {
            geometry.is_circle = true;
            geometry.circle_center = SvgPoint(center_x, center_y);
            geometry.circle_radius = radius_x;
        }
        return geometry;
    }
    if (tag == "line") {
        SvgSubpath subpath;
        subpath.points.push_back(SvgPoint(AttributeNumber(element, "x1", 0.0f),
                                          AttributeNumber(element, "y1", 0.0f)));
        subpath.points.push_back(SvgPoint(AttributeNumber(element, "x2", 0.0f),
                                          AttributeNumber(element, "y2", 0.0f)));
        geometry.subpaths.push_back(std::move(subpath));
        return geometry;
    }
    if (tag == "polyline" || tag == "polygon") {
        std::vector<float> values;
        if (!ReadNumberList(element.GetAttribute("points"), &values) || values.size() < 4) {
            return geometry;
        }
        SvgSubpath subpath;
        for (size_t index = 0; index + 1 < values.size(); index += 2) {
            AppendPoint(&subpath.points, values[index], values[index + 1]);
        }
        subpath.closed = tag == "polygon";
        geometry.subpaths.push_back(std::move(subpath));
        return geometry;
    }
    if (tag == "path") {
        geometry.subpaths = BuildPathGeometry(element.GetAttribute("d"));
        return geometry;
    }
    return geometry;
}

// ---------------------------------------------------------------------------
// 扫描线填充
// ---------------------------------------------------------------------------

// 计算一条像素行的覆盖率。裁剪区左边界为 clip_left，覆盖率数组长度等于裁剪区宽度。
void AddSpanCoverage(float start_x, float end_x, int clip_left, float weight,
                     std::vector<float>* coverage) {
    if (!(end_x > start_x)) {
        return;
    }
    const int size = static_cast<int>(coverage->size());
    int first = static_cast<int>(std::floor(start_x));
    int last = static_cast<int>(std::floor(end_x - 1e-5f));
    for (int pixel = first; pixel <= last; ++pixel) {
        const int local = pixel - clip_left;
        if (local < 0 || local >= size) {
            continue;
        }
        const float left = std::max(start_x, static_cast<float>(pixel));
        const float right = std::min(end_x, static_cast<float>(pixel + 1));
        if (right > left) {
            (*coverage)[static_cast<size_t>(local)] += (right - left) * weight;
        }
    }
}

struct Crossing {
    float x = 0.0f;
    int winding = 0;
};

void ComputeRowCoverage(const std::vector<std::vector<SvgPoint>>& polygons, int row,
                        const RectF& clip, std::vector<float>* coverage) {
    const int clip_left = static_cast<int>(std::floor(clip.left));
    const int clip_right = static_cast<int>(std::ceil(clip.right));
    const int width = std::max(clip_right - clip_left, 0);
    coverage->assign(static_cast<size_t>(width), 0.0f);
    if (width == 0) {
        return;
    }

    constexpr int kSamples = 4;
    const float weight = 1.0f / static_cast<float>(kSamples);
    std::vector<Crossing> crossings;
    for (int sample = 0; sample < kSamples; ++sample) {
        const float y = static_cast<float>(row) +
                        (static_cast<float>(sample) + 0.5f) / static_cast<float>(kSamples);
        crossings.clear();
        for (const std::vector<SvgPoint>& polygon : polygons) {
            const size_t count = polygon.size();
            for (size_t index = 0; index < count; ++index) {
                const SvgPoint& p0 = polygon[index];
                const SvgPoint& p1 = polygon[(index + 1) % count];
                if (p0.y == p1.y) {
                    continue;
                }
                if ((p0.y <= y && p1.y > y) || (p1.y <= y && p0.y > y)) {
                    const float ratio = (y - p0.y) / (p1.y - p0.y);
                    Crossing crossing;
                    crossing.x = p0.x + (p1.x - p0.x) * ratio;
                    crossing.winding = p1.y > p0.y ? 1 : -1;
                    crossings.push_back(crossing);
                }
            }
        }
        std::sort(crossings.begin(), crossings.end(),
                  [](const Crossing& left, const Crossing& right) { return left.x < right.x; });
        int winding = 0;
        float span_start = 0.0f;
        for (const Crossing& crossing : crossings) {
            const int before = winding;
            winding += crossing.winding;
            if (before == 0 && winding != 0) {
                span_start = crossing.x;
            } else if (before != 0 && winding == 0) {
                AddSpanCoverage(span_start, crossing.x, clip_left, weight, coverage);
            }
        }
    }
}

// 纯色填充：整像素覆盖的连续区间合并为一个矩形，边缘像素单独输出。
void FillPolygonsSolid(RenderDevice& device, const RectF& clip,
                       const std::vector<std::vector<SvgPoint>>& polygons,
                       const Color& color) {
    if (!color.Visible() || polygons.empty()) {
        return;
    }
    const RectF bounds = BoundsOfPolygons(polygons);
    const RectF visible = clip.Intersect(bounds);
    if (visible.IsEmpty()) {
        return;
    }
    const int first_row = static_cast<int>(std::floor(visible.top));
    const int last_row = static_cast<int>(std::ceil(visible.bottom));
    const int clip_left = static_cast<int>(std::floor(clip.left));
    std::vector<float> coverage;
    for (int row = first_row; row < last_row; ++row) {
        ComputeRowCoverage(polygons, row, clip, &coverage);
        const int width = static_cast<int>(coverage.size());
        int pixel = 0;
        while (pixel < width) {
            const float value = coverage[static_cast<size_t>(pixel)];
            if (value >= 0.996f) {
                const int begin = pixel;
                while (pixel < width && coverage[static_cast<size_t>(pixel)] >= 0.996f) {
                    ++pixel;
                }
                device.FillRect(RectF::FromLTRB(static_cast<float>(clip_left + begin),
                                                 static_cast<float>(row),
                                                 static_cast<float>(clip_left + pixel),
                                                 static_cast<float>(row + 1)),
                                color);
            } else if (value > 0.004f) {
                Color edge = color;
                edge.a = color.a * value;
                device.FillRect(RectF::FromLTRB(static_cast<float>(clip_left + pixel),
                                                 static_cast<float>(row),
                                                 static_cast<float>(clip_left + pixel + 1),
                                                 static_cast<float>(row + 1)),
                                edge);
                ++pixel;
            } else {
                ++pixel;
            }
        }
    }
}

// 逐像素着色填充：渐变与图案无法合并为同色区间，按像素输出。
void FillPolygonsShaded(RenderDevice& device, const RectF& clip,
                        const std::vector<std::vector<SvgPoint>>& polygons,
                        const std::function<Color(float, float)>& shader) {
    if (polygons.empty()) {
        return;
    }
    const RectF bounds = BoundsOfPolygons(polygons);
    const RectF visible = clip.Intersect(bounds);
    if (visible.IsEmpty()) {
        return;
    }
    const int first_row = static_cast<int>(std::floor(visible.top));
    const int last_row = static_cast<int>(std::ceil(visible.bottom));
    const int clip_left = static_cast<int>(std::floor(clip.left));
    std::vector<float> coverage;
    for (int row = first_row; row < last_row; ++row) {
        ComputeRowCoverage(polygons, row, clip, &coverage);
        const int width = static_cast<int>(coverage.size());
        for (int pixel = 0; pixel < width; ++pixel) {
            const float value = coverage[static_cast<size_t>(pixel)];
            if (value <= 0.004f) {
                continue;
            }
            Color color = shader(static_cast<float>(clip_left + pixel) + 0.5f,
                                 static_cast<float>(row) + 0.5f);
            color.a *= value;
            if (!color.Visible()) {
                continue;
            }
            device.FillRect(RectF::FromLTRB(static_cast<float>(clip_left + pixel),
                                             static_cast<float>(row),
                                             static_cast<float>(clip_left + pixel + 1),
                                             static_cast<float>(row + 1)),
                            color);
        }
    }
}

// ---------------------------------------------------------------------------
// 描边几何
// ---------------------------------------------------------------------------

void AppendCirclePolygon(float center_x, float center_y, float radius,
                         std::vector<std::vector<SvgPoint>>* polygons) {
    if (radius <= 0.0f) {
        return;
    }
    int segments = static_cast<int>(radius * 2.0f) + 12;
    segments = std::min(std::max(segments, 12), 96);
    std::vector<SvgPoint> polygon;
    polygon.reserve(static_cast<size_t>(segments));
    for (int step = 0; step < segments; ++step) {
        const float angle = 2.0f * kPi * static_cast<float>(step) / static_cast<float>(segments);
        polygon.push_back(SvgPoint(center_x + radius * std::cos(angle),
                                   center_y + radius * std::sin(angle)));
    }
    polygons->push_back(std::move(polygon));
}

void AppendSegmentQuad(const SvgPoint& from, const SvgPoint& to, float half_width,
                       std::vector<std::vector<SvgPoint>>* polygons) {
    const float dx = to.x - from.x;
    const float dy = to.y - from.y;
    const float length = std::sqrt(dx * dx + dy * dy);
    if (length < 1e-6f) {
        return;
    }
    const float normal_x = -dy / length * half_width;
    const float normal_y = dx / length * half_width;
    std::vector<SvgPoint> polygon;
    polygon.push_back(SvgPoint(from.x + normal_x, from.y + normal_y));
    polygon.push_back(SvgPoint(to.x + normal_x, to.y + normal_y));
    polygon.push_back(SvgPoint(to.x - normal_x, to.y - normal_y));
    polygon.push_back(SvgPoint(from.x - normal_x, from.y - normal_y));
    polygons->push_back(std::move(polygon));
}

// 把虚线数组切分为实线段。offset 取正时向前移动图案起点。
std::vector<std::vector<SvgPoint>> SplitDashes(const std::vector<SvgPoint>& points, bool closed,
                                               const std::vector<float>& dashes,
                                               float offset) {
    std::vector<std::vector<SvgPoint>> result;
    if (dashes.empty()) {
        result.push_back(points);
        if (closed && !points.empty()) {
            result.back().push_back(points.front());
        }
        return result;
    }
    float period = 0.0f;
    for (float dash : dashes) {
        period += std::max(dash, 0.0f);
    }
    if (period <= 1e-6f) {
        result.push_back(points);
        return result;
    }

    float phase = std::fmod(offset, period);
    if (phase < 0.0f) {
        phase += period;
    }
    size_t dash_index = 0;
    float remaining = std::max(dashes[0], 0.0f);
    while (phase > remaining && remaining >= 0.0f) {
        phase -= remaining;
        dash_index = (dash_index + 1) % dashes.size();
        remaining = std::max(dashes[dash_index], 0.0f);
        if (remaining <= 0.0f && dash_index == 0) {
            break;
        }
    }
    bool drawing = dash_index % 2 == 0;
    remaining = std::max(dashes[dash_index], 0.0f) - phase;
    if (remaining < 0.0f) {
        remaining = 0.0f;
    }

    std::vector<SvgPoint> current;
    auto finish = [&]() {
        if (current.size() >= 2) {
            result.push_back(current);
        }
        current.clear();
    };

    const size_t segment_count = closed ? points.size() : points.size() - 1;
    for (size_t index = 0; index < segment_count; ++index) {
        const SvgPoint& from = points[index];
        const SvgPoint& to = points[(index + 1) % points.size()];
        const float dx = to.x - from.x;
        const float dy = to.y - from.y;
        const float length = std::sqrt(dx * dx + dy * dy);
        if (length < 1e-6f) {
            continue;
        }
        float travelled = 0.0f;
        while (travelled < length) {
            if (remaining <= 1e-6f) {
                if (drawing) {
                    finish();
                }
                dash_index = (dash_index + 1) % dashes.size();
                remaining = std::max(dashes[dash_index], 0.0f);
                drawing = dash_index % 2 == 0;
                continue;
            }
            const float step = std::min(remaining, length - travelled);
            const float start_ratio = travelled / length;
            const float end_ratio = (travelled + step) / length;
            if (drawing) {
                const SvgPoint start(from.x + dx * start_ratio, from.y + dy * start_ratio);
                const SvgPoint end(from.x + dx * end_ratio, from.y + dy * end_ratio);
                if (current.empty()) {
                    current.push_back(start);
                }
                current.push_back(end);
            } else if (!current.empty()) {
                finish();
            }
            travelled += step;
            remaining -= step;
        }
    }
    finish();
    return result;
}

// 构造描边覆盖区域：每段线生成矩形，折点与端点按线帽、连接方式补齐。
void BuildStrokePolygons(const std::vector<SvgSubpath>& subpaths, float stroke_width,
                         SvgLineCap line_cap, SvgLineJoin line_join,
                         const std::vector<float>& dash_array, float dash_offset,
                         std::vector<std::vector<SvgPoint>>* polygons) {
    const float half_width = stroke_width * 0.5f;
    if (half_width <= 0.0f) {
        return;
    }
    for (const SvgSubpath& subpath : subpaths) {
        if (subpath.points.empty()) {
            continue;
        }
        std::vector<std::vector<SvgPoint>> pieces =
            SplitDashes(subpath.points, subpath.closed, dash_array, dash_offset);
        for (const std::vector<SvgPoint>& piece : pieces) {
            if (piece.size() < 2) {
                continue;
            }
            for (size_t index = 0; index + 1 < piece.size(); ++index) {
                AppendSegmentQuad(piece[index], piece[index + 1], half_width, polygons);
            }
            const bool piece_closed = subpath.closed && dash_array.empty();
            // 折点补圆或斜接，避免出现缺口。
            const size_t first_join = piece_closed ? 0 : 1;
            const size_t last_join = piece_closed ? piece.size() : piece.size() - 1;
            for (size_t index = first_join; index < last_join; ++index) {
                const SvgPoint& previous = piece[(index + piece.size() - 1) % piece.size()];
                const SvgPoint& vertex = piece[index];
                const SvgPoint& next = piece[(index + 1) % piece.size()];
                if (line_join == SvgLineJoin::Round) {
                    AppendCirclePolygon(vertex.x, vertex.y, half_width, polygons);
                } else if (line_join == SvgLineJoin::Miter) {
                    const float in_x = vertex.x - previous.x;
                    const float in_y = vertex.y - previous.y;
                    const float out_x = next.x - vertex.x;
                    const float out_y = next.y - vertex.y;
                    const float in_length = std::sqrt(in_x * in_x + in_y * in_y);
                    const float out_length = std::sqrt(out_x * out_x + out_y * out_y);
                    if (in_length > 1e-6f && out_length > 1e-6f) {
                        const float ux = in_x / in_length + out_x / out_length;
                        const float uy = in_y / in_length + out_y / out_length;
                        const float length = std::sqrt(ux * ux + uy * uy);
                        if (length > 1e-6f) {
                            const float miter = half_width / std::max(length * 0.5f, 0.1f);
                            std::vector<SvgPoint> wedge;
                            wedge.push_back(vertex);
                            wedge.push_back(SvgPoint(vertex.x + ux / length * std::min(miter, half_width * 4.0f),
                                                     vertex.y + uy / length * std::min(miter, half_width * 4.0f)));
                            polygons->push_back(std::move(wedge));
                        }
                    }
                }
            }
            if (!piece_closed) {
                if (line_cap == SvgLineCap::Round) {
                    AppendCirclePolygon(piece.front().x, piece.front().y, half_width, polygons);
                    AppendCirclePolygon(piece.back().x, piece.back().y, half_width, polygons);
                } else if (line_cap == SvgLineCap::Square) {
                    const SvgPoint& first = piece.front();
                    const SvgPoint& second = piece[1];
                    const SvgPoint& last = piece.back();
                    const SvgPoint& before_last = piece[piece.size() - 2];
                    const float dx0 = first.x - second.x;
                    const float dy0 = first.y - second.y;
                    const float length0 = std::sqrt(dx0 * dx0 + dy0 * dy0);
                    const float dx1 = last.x - before_last.x;
                    const float dy1 = last.y - before_last.y;
                    const float length1 = std::sqrt(dx1 * dx1 + dy1 * dy1);
                    if (length0 > 1e-6f) {
                        AppendSegmentQuad(first, SvgPoint(first.x + dx0 / length0 * half_width,
                                                          first.y + dy0 / length0 * half_width),
                                          half_width, polygons);
                    }
                    if (length1 > 1e-6f) {
                        AppendSegmentQuad(last, SvgPoint(last.x + dx1 / length1 * half_width,
                                                         last.y + dy1 / length1 * half_width),
                                          half_width, polygons);
                    }
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// 渐变与图案
// ---------------------------------------------------------------------------

struct GradientGeometry {
    bool radial = false;
    SvgPoint start;  // 线性渐变起点；径向渐变圆心
    SvgPoint end;    // 线性渐变终点
    float radius = 0.0f;
    std::vector<SvgGradientStop> stops;
};

bool ParseGradientStop(const Element& element, float previous_offset, SvgGradientStop* stop) {
    float offset = previous_offset;
    if (element.HasAttribute("offset")) {
        offset = AttributeRatio(element, "offset", previous_offset, 1.0f);
    }
    offset = std::min(std::max(offset, 0.0f), 1.0f);

    Color color = Black();
    if (element.HasAttribute("stop-color")) {
        ParseSvgColor(element.GetAttribute("stop-color"), &color);
    }
    if (element.HasAttribute("style")) {
        // 内联 style 中的 stop-color 与 stop-opacity。
        const std::string style_text = element.GetAttribute("style");
        size_t begin = 0;
        while (begin < style_text.size()) {
            size_t end = style_text.find(';', begin);
            if (end == std::string::npos) {
                end = style_text.size();
            }
            const std::string item = style_text.substr(begin, end - begin);
            const size_t colon = item.find(':');
            if (colon != std::string::npos) {
                std::string name = AsciiLower(item.substr(0, colon));
                std::string value = item.substr(colon + 1);
                TrimAsciiSpace(&name);
                TrimAsciiSpace(&value);
                if (name == "stop-color") {
                    ParseSvgColor(value, &color);
                }
            }
            begin = end + 1;
        }
    }
    float opacity = 1.0f;
    if (element.HasAttribute("stop-opacity")) {
        opacity = AttributeNumber(element, "stop-opacity", 1.0f);
    }
    color.a *= std::min(std::max(opacity, 0.0f), 1.0f);
    stop->offset = offset;
    stop->color = color;
    return true;
}

// 从当前 SVG 用户空间出发构造渐变几何。objectBoundingBox 的坐标按元素边界换算，
// userSpaceOnUse 的百分比按 SVG 视口尺寸换算。
bool BuildGradientGeometry(const Element& gradient, const RectF& user_bounds,
                           const SvgMatrix& user_to_page, float viewport_width,
                           float viewport_height, GradientGeometry* output) {
    const bool radial = gradient.TagName() == "radialgradient";
    bool user_space = false;
    if (gradient.HasAttribute("gradientunits")) {
        user_space = AsciiLower(gradient.GetAttribute("gradientunits")) == "userspaceonuse";
    }
    auto resolve_x = [&](const char* name, float fallback) {
        if (!gradient.HasAttribute(name)) {
            return fallback;
        }
        if (user_space) {
            return AttributeRatio(gradient, name, fallback, viewport_width);
        }
        return AttributeRatio(gradient, name, fallback, 1.0f) * user_bounds.Width() +
               user_bounds.left;
    };
    auto resolve_y = [&](const char* name, float fallback) {
        if (!gradient.HasAttribute(name)) {
            return fallback;
        }
        if (user_space) {
            return AttributeRatio(gradient, name, fallback, viewport_height);
        }
        return AttributeRatio(gradient, name, fallback, 1.0f) * user_bounds.Height() +
               user_bounds.top;
    };

    output->radial = radial;
    if (radial) {
        const float center_x = resolve_x("cx", user_space ? 0.0f : user_bounds.left + user_bounds.Width() * 0.5f);
        const float center_y = resolve_y("cy", user_space ? 0.0f : user_bounds.top + user_bounds.Height() * 0.5f);
        float radius_user = 0.0f;
        if (user_space) {
            if (gradient.HasAttribute("r")) {
                radius_user = AttributeRatio(gradient, "r", 0.5f, viewport_width);
            } else {
                // 缺省半径为 50%，userSpaceOnUse 的百分比以视口归一化对角线为基准。
                radius_user = 0.5f * std::sqrt(viewport_width * viewport_width +
                                               viewport_height * viewport_height) /
                              std::sqrt(2.0f);
            }
        } else {
            const float ratio =
                gradient.HasAttribute("r") ? AttributeRatio(gradient, "r", 0.5f, 1.0f) : 0.5f;
            radius_user = ratio * std::sqrt(user_bounds.Width() * user_bounds.Width() +
                                            user_bounds.Height() * user_bounds.Height()) /
                          std::sqrt(2.0f);
        }
        const SvgPoint center = TransformPoint(user_to_page, SvgPoint(center_x, center_y));
        const SvgPoint edge =
            TransformPoint(user_to_page, SvgPoint(center_x + radius_user, center_y));
        const float dx = edge.x - center.x;
        const float dy = edge.y - center.y;
        output->start = center;
        output->radius = std::sqrt(dx * dx + dy * dy);
    } else {
        const float x1 = resolve_x("x1", user_space ? 0.0f : user_bounds.left);
        const float y1 = resolve_y("y1", user_space ? 0.0f : user_bounds.top);
        const float x2 = resolve_x("x2", user_space ? viewport_width : user_bounds.right);
        const float y2 = resolve_y("y2", user_space ? 0.0f : user_bounds.top);
        output->start = TransformPoint(user_to_page, SvgPoint(x1, y1));
        output->end = TransformPoint(user_to_page, SvgPoint(x2, y2));
    }

    output->stops.clear();
    float previous_offset = 0.0f;
    for (const NodePtr& child : gradient.Children()) {
        if (!child || !child->IsElement()) {
            continue;
        }
        const Element& stop_element = static_cast<const Element&>(*child);
        if (stop_element.TagName() != "stop") {
            continue;
        }
        SvgGradientStop stop;
        if (!ParseGradientStop(stop_element, previous_offset, &stop)) {
            continue;
        }
        previous_offset = stop.offset;
        output->stops.push_back(stop);
    }
    return !output->stops.empty();
}

Color GradientColorAt(const std::vector<SvgGradientStop>& stops, float offset) {
    if (stops.empty()) {
        return Transparent();
    }
    if (offset <= stops.front().offset) {
        return stops.front().color;
    }
    for (size_t index = 1; index < stops.size(); ++index) {
        if (offset <= stops[index].offset) {
            const SvgGradientStop& before = stops[index - 1];
            const SvgGradientStop& after = stops[index];
            const float span = after.offset - before.offset;
            const float ratio = span > 1e-6f ? (offset - before.offset) / span : 0.0f;
            Color color;
            color.r = before.color.r + (after.color.r - before.color.r) * ratio;
            color.g = before.color.g + (after.color.g - before.color.g) * ratio;
            color.b = before.color.b + (after.color.b - before.color.b) * ratio;
            color.a = before.color.a + (after.color.a - before.color.a) * ratio;
            return color;
        }
    }
    return stops.back().color;
}

// ---------------------------------------------------------------------------
// 渲染器
// ---------------------------------------------------------------------------

// 图案内容中的叶子图元：样式、几何与内容坐标内的变换在铺排前只解析一次，
// 每个图块只需组合平移矩阵即可绘制。
struct PatternLeaf {
    const Element* element = nullptr;
    SvgStyle style;
    ShapeGeometry geometry;
    SvgMatrix matrix;
    bool is_text = false;
};

class SvgRenderer {
public:
    SvgRenderer(const SvgPaintParams& params, const LayoutResult& layout, const RectF& clip,
                float viewport_width, float viewport_height)
        : params_(params),
          layout_(layout),
          clip_(clip),
          viewport_width_(viewport_width),
          viewport_height_(viewport_height) {}

    void RenderRoot(const Element& svg_element);

private:
    const ComputedStyle* FindComputedStyle(const Element& element) const {
        const auto found = layout_.svg_styles.find(&element);
        return found == layout_.svg_styles.end() ? nullptr : &found->second;
    }

    const Element* FindReference(const Element& from, const std::string& identifier) const {
        if (identifier.empty()) {
            return nullptr;
        }
        const Element* origin = root_element_ != nullptr ? root_element_ : &from;
        const Document* document = origin->OwnerDocument();
        if (document == nullptr) {
            return nullptr;
        }
        return document->GetElementById(identifier);
    }

    void RenderElement(const Element& element, const SvgStyle& parent_style,
                       const SvgMatrix& parent_matrix, float opacity);
    void RenderShape(const Element& element, const SvgStyle& style, const SvgMatrix& matrix,
                     const ShapeGeometry& geometry, float opacity);
    void RenderText(const Element& element, const SvgStyle& style, const SvgMatrix& matrix,
                    float opacity);
    void RenderPatternFill(const Element& pattern, const SvgMatrix& matrix,
                           const RectF& user_bounds, float opacity);
    // 解析图案内容，把图形与文本收集为叶子，供逐图块铺排时复用。
    void CollectPatternLeaves(const Element& element, const SvgStyle& parent_style,
                              const SvgMatrix& parent_matrix,
                              std::vector<PatternLeaf>* leaves);
    Color ResolveTextFill(const Element& element, const SvgStyle& style, float opacity) const;

    bool BuildFillPaint(const Element& element, const SvgStyle& style, const RectF& user_bounds,
                        const SvgMatrix& matrix, GradientGeometry* gradient,
                        const Element** pattern) const;

    void DrawGlyphRun(const std::string& text, FontId font, float anchor_x, float baseline_y,
                      float scale, const Color& color);

    SvgPaintParams params_;
    const LayoutResult& layout_;
    RectF clip_;
    float viewport_width_ = 0.0f;
    float viewport_height_ = 0.0f;
    // 当前正在绘制的 SVG 根元素，用于按 #id 查找渐变与图案定义。
    const Element* root_element_ = nullptr;
};

// 判断标签是否属于不绘制的定义类元素。
bool IsNonRenderedTag(const std::string& tag) {
    return tag == "defs" || tag == "lineargradient" || tag == "radialgradient" ||
           tag == "pattern" || tag == "stop" || tag == "clippath" || tag == "mask" ||
           tag == "marker" || tag == "symbol" || tag == "title" || tag == "desc" ||
           tag == "metadata" || tag == "style" || tag == "script" || tag == "filter";
}

bool IsShapeTag(const std::string& tag) {
    return tag == "rect" || tag == "circle" || tag == "ellipse" || tag == "line" ||
           tag == "polyline" || tag == "polygon" || tag == "path";
}

// 构造 SVG 用户坐标到页面坐标的根矩阵：viewBox 按 preserveAspectRatio 的
// 默认值 xMidYMid meet 缩放并居中；没有 viewBox 时用户单位等于像素。
SvgMatrix BuildSvgViewportMatrix(const Element& svg_element, const RectF& content) {
    SvgMatrix base = TranslationMatrix(content.left, content.top);
    std::vector<float> view_box;
    if (!svg_element.HasAttribute("viewbox") ||
        !ReadNumberList(svg_element.GetAttribute("viewbox"), &view_box) || view_box.size() < 4 ||
        view_box[2] <= 0.0f || view_box[3] <= 0.0f) {
        return base;
    }
    const float scale_x = content.Width() / view_box[2];
    const float scale_y = content.Height() / view_box[3];
    const bool stretch = svg_element.HasAttribute("preserveaspectratio") &&
                         AsciiLower(svg_element.GetAttribute("preserveaspectratio"))
                                 .find("none") != std::string::npos;
    const float scale = std::min(scale_x, scale_y);
    float offset_x = (content.Width() - view_box[2] * scale) * 0.5f;
    float offset_y = (content.Height() - view_box[3] * scale) * 0.5f;
    float scale_horizontal = scale;
    float scale_vertical = scale;
    if (stretch) {
        scale_horizontal = scale_x;
        scale_vertical = scale_y;
        offset_x = 0.0f;
        offset_y = 0.0f;
    }
    SvgMatrix view_box_matrix;
    view_box_matrix.a = scale_horizontal;
    view_box_matrix.d = scale_vertical;
    view_box_matrix.e = offset_x - view_box[0] * scale_horizontal;
    view_box_matrix.f = offset_y - view_box[1] * scale_vertical;
    return MultiplyMatrix(TranslationMatrix(content.left, content.top), view_box_matrix);
}

void SvgRenderer::RenderRoot(const Element& svg_element) {
    root_element_ = &svg_element;
    SvgStyle root_style;
    root_style.font.size = 16.0f;
    const ComputedStyle* computed = FindComputedStyle(svg_element);
    root_style = ResolveStyle(svg_element, root_style, computed);
    // 根元素的 CSS opacity 已由 HTML 绘制层计入 params_.opacity，这里只补充
    // SVG 根元素自身的 opacity 属性，避免同一透明度被应用两次。
    float root_opacity = 1.0f;
    if (svg_element.HasAttribute("opacity")) {
        root_opacity = std::min(std::max(AttributeNumber(svg_element, "opacity", 1.0f), 0.0f),
                                1.0f);
    }
    const float total_opacity = params_.opacity * root_opacity;

    const RectF content = clip_;
    const SvgMatrix base = BuildSvgViewportMatrix(svg_element, content);

    for (const NodePtr& child : svg_element.Children()) {
        if (!child || !child->IsElement()) {
            continue;
        }
        RenderElement(static_cast<const Element&>(*child), root_style, base, total_opacity);
    }
}

void SvgRenderer::RenderElement(const Element& element, const SvgStyle& parent_style,
                                const SvgMatrix& parent_matrix, float opacity) {
    const ComputedStyle* computed = FindComputedStyle(element);
    if (computed != nullptr && computed->display == Display::None) {
        return;
    }
    const std::string& tag = element.TagName();
    if (IsNonRenderedTag(tag)) {
        return;
    }

    const SvgStyle style = ResolveStyle(element, parent_style, computed);
    const float element_opacity = opacity * style.opacity;
    SvgMatrix matrix = parent_matrix;
    if (element.HasAttribute("transform")) {
        matrix = MultiplyMatrix(matrix, ParseTransform(element.GetAttribute("transform")));
    }

    if (tag == "text") {
        RenderText(element, style, matrix, element_opacity);
        return;
    }
    if (IsShapeTag(tag)) {
        const ShapeGeometry geometry = BuildShapeGeometry(element);
        if (!geometry.subpaths.empty()) {
            RenderShape(element, style, matrix, geometry, element_opacity);
        }
        return;
    }

    // 组容器与未知的包装元素：继续渲染子元素。
    for (const NodePtr& child : element.Children()) {
        if (!child || !child->IsElement()) {
            continue;
        }
        RenderElement(static_cast<const Element&>(*child), style, matrix, element_opacity);
    }
}

bool SvgRenderer::BuildFillPaint(const Element& element, const SvgStyle& style,
                                 const RectF& user_bounds, const SvgMatrix& matrix,
                                 GradientGeometry* gradient, const Element** pattern) const {
    *pattern = nullptr;
    if (style.fill_reference.empty()) {
        return false;
    }
    const Element* reference = FindReference(element, style.fill_reference);
    if (reference == nullptr) {
        return false;
    }
    if (reference->TagName() == "lineargradient" || reference->TagName() == "radialgradient") {
        return BuildGradientGeometry(*reference, user_bounds, matrix, viewport_width_,
                                     viewport_height_, gradient);
    }
    if (reference->TagName() == "pattern") {
        *pattern = reference;
        return true;
    }
    return false;
}

void SvgRenderer::RenderShape(const Element& element, const SvgStyle& style,
                              const SvgMatrix& matrix, const ShapeGeometry& geometry,
                              float opacity) {
    const std::vector<std::vector<SvgPoint>> polygons = TransformSubpaths(geometry.subpaths, matrix);
    const RectF user_bounds = BoundsOfPoints(geometry.subpaths);
    // 注意：水平线与垂直线的填充几何外接矩形高度或宽度为零，不能据此提前返回，
    // 否则描边会被一并跳过。填充与描边各自在内部按裁剪区判断可见性。

    GradientGeometry gradient;
    const Element* pattern = nullptr;
    const bool has_paint =
        BuildFillPaint(element, style, user_bounds, matrix, &gradient, &pattern);

    if (!style.fill_none && !style.fill_reference.empty() && !has_paint) {
        // 引用无法解析时按无填充处理，与浏览器对无效 paint 的处理一致。
    } else if (!style.fill_none) {
        const float fill_alpha = style.fill_opacity * opacity;
        if (!has_paint) {
            Color color = style.fill;
            color.a *= fill_alpha;
            // 小圆使用后端的圆角矩形距离场，避免为图案中的大量圆点生成扫描线。
            bool drawn_fast = false;
            if (geometry.is_circle) {
                const SvgPoint center = TransformPoint(matrix, geometry.circle_center);
                const float scale =
                    (HorizontalScale(matrix) + VerticalScale(matrix)) * 0.5f;
                const float radius = geometry.circle_radius * scale;
                if (radius > 0.0f && radius <= 5.0f && clip_.Contains(center.x, center.y)) {
                    const RectF circle_rect =
                        RectF::FromLTRB(center.x - radius, center.y - radius,
                                        center.x + radius, center.y + radius)
                            .Intersect(clip_);
                    if (!circle_rect.IsEmpty()) {
                        params_.device->FillRoundedRect(
                            circle_rect, std::min(radius, std::min(circle_rect.Width(),
                                                                   circle_rect.Height()) *
                                                                  0.5f),
                            color);
                        drawn_fast = true;
                    }
                }
            }
            if (!drawn_fast) {
                FillPolygonsSolid(*params_.device, clip_, polygons, color);
            }
        } else if (pattern != nullptr) {
            RenderPatternFill(*pattern, matrix, user_bounds, fill_alpha);
        } else if (gradient.radial) {
            const float center_x = gradient.start.x;
            const float center_y = gradient.start.y;
            const float radius = gradient.radius;
            if (radius > 0.0f) {
                FillPolygonsShaded(*params_.device, clip_, polygons,
                                   [&gradient, center_x, center_y, radius,
                                    fill_alpha](float x, float y) {
                                       const float dx = x - center_x;
                                       const float dy = y - center_y;
                                       const float offset =
                                           std::sqrt(dx * dx + dy * dy) / radius;
                                       Color color =
                                           GradientColorAt(gradient.stops,
                                                           std::min(std::max(offset, 0.0f), 1.0f));
                                       color.a *= fill_alpha;
                                       return color;
                                   });
            }
        } else {
            const float dx = gradient.end.x - gradient.start.x;
            const float dy = gradient.end.y - gradient.start.y;
            const float length_square = dx * dx + dy * dy;
            if (length_square > 1e-6f) {
                FillPolygonsShaded(*params_.device, clip_, polygons,
                                   [&gradient, dx, dy, length_square, fill_alpha](float x, float y) {
                                       const float offset =
                                           ((x - gradient.start.x) * dx +
                                            (y - gradient.start.y) * dy) /
                                           length_square;
                                       Color color =
                                           GradientColorAt(gradient.stops,
                                                           std::min(std::max(offset, 0.0f), 1.0f));
                                       color.a *= fill_alpha;
                                       return color;
                                   });
            }
        }
    }

    if (!style.stroke_none && style.stroke_width > 0.0f && style.stroke_opacity > 0.0f) {
        std::vector<std::vector<SvgPoint>> stroke_polygons;
        BuildStrokePolygons(geometry.subpaths, style.stroke_width, style.line_cap, style.line_join,
                            style.dash_array, style.dash_offset, &stroke_polygons);
        if (!stroke_polygons.empty()) {
            std::vector<SvgSubpath> stroke_subpaths;
            stroke_subpaths.reserve(stroke_polygons.size());
            for (const std::vector<SvgPoint>& polygon : stroke_polygons) {
                SvgSubpath subpath;
                subpath.points = polygon;
                subpath.closed = true;
                stroke_subpaths.push_back(std::move(subpath));
            }
            const std::vector<std::vector<SvgPoint>> page_stroke =
                TransformSubpaths(stroke_subpaths, matrix);
            Color color = style.stroke;
            color.a *= style.stroke_opacity * opacity;
            FillPolygonsSolid(*params_.device, clip_, page_stroke, color);
        }
    }
}

void SvgRenderer::RenderPatternFill(const Element& pattern, const SvgMatrix& matrix,
                                    const RectF& user_bounds, float opacity) {
    const bool user_space = !pattern.HasAttribute("patternunits") ||
                            AsciiLower(pattern.GetAttribute("patternunits")) !=
                                "objectboundingbox";
    const float tile_width = pattern.HasAttribute("width")
                                 ? AttributeNumber(pattern, "width", 0.0f)
                                 : 0.0f;
    const float tile_height = pattern.HasAttribute("height")
                                  ? AttributeNumber(pattern, "height", 0.0f)
                                  : 0.0f;
    if (tile_width <= 0.0f || tile_height <= 0.0f) {
        return;
    }

    float tile_width_user = tile_width;
    float tile_height_user = tile_height;
    float base_x = 0.0f;
    float base_y = 0.0f;
    if (user_space) {
        base_x = pattern.HasAttribute("x") ? AttributeNumber(pattern, "x", 0.0f) : 0.0f;
        base_y = pattern.HasAttribute("y") ? AttributeNumber(pattern, "y", 0.0f) : 0.0f;
    } else {
        tile_width_user = tile_width * user_bounds.Width();
        tile_height_user = tile_height * user_bounds.Height();
        base_x = user_bounds.left + (pattern.HasAttribute("x")
                                         ? AttributeNumber(pattern, "x", 0.0f) *
                                               user_bounds.Width()
                                         : 0.0f);
        base_y = user_bounds.top + (pattern.HasAttribute("y")
                                        ? AttributeNumber(pattern, "y", 0.0f) *
                                              user_bounds.Height()
                                        : 0.0f);
    }
    if (tile_width_user <= 0.0f || tile_height_user <= 0.0f) {
        return;
    }

    // 只在与页面裁剪区相交的范围内铺排图块：把裁剪区反变换回用户坐标。
    SvgMatrix inverse;
    if (!InvertMatrix(matrix, &inverse)) {
        return;
    }
    RectF local_clip;
    const SvgPoint corners[4] = {
        TransformPoint(inverse, SvgPoint(clip_.left, clip_.top)),
        TransformPoint(inverse, SvgPoint(clip_.right, clip_.top)),
        TransformPoint(inverse, SvgPoint(clip_.right, clip_.bottom)),
        TransformPoint(inverse, SvgPoint(clip_.left, clip_.bottom)),
    };
    local_clip.left = corners[0].x;
    local_clip.right = corners[0].x;
    local_clip.top = corners[0].y;
    local_clip.bottom = corners[0].y;
    for (int index = 1; index < 4; ++index) {
        local_clip.left = std::min(local_clip.left, corners[index].x);
        local_clip.right = std::max(local_clip.right, corners[index].x);
        local_clip.top = std::min(local_clip.top, corners[index].y);
        local_clip.bottom = std::max(local_clip.bottom, corners[index].y);
    }
    const RectF tile_area = local_clip.Intersect(user_bounds);
    if (tile_area.IsEmpty()) {
        return;
    }

    int first_column = static_cast<int>(
        std::floor((tile_area.left - base_x) / tile_width_user));
    int last_column = static_cast<int>(
        std::floor((tile_area.right - base_x) / tile_width_user));
    int first_row = static_cast<int>(std::floor((tile_area.top - base_y) / tile_height_user));
    int last_row = static_cast<int>(std::floor((tile_area.bottom - base_y) / tile_height_user));

    // 限制图块总数，避免异常的宽高导致绘制量失控。
    const int64_t columns = static_cast<int64_t>(last_column - first_column + 1);
    const int64_t rows = static_cast<int64_t>(last_row - first_row + 1);
    constexpr int64_t kMaximumTiles = 40000;
    if (columns <= 0 || rows <= 0 || columns * rows > kMaximumTiles) {
        return;
    }

    SvgStyle pattern_style;
    const ComputedStyle* pattern_computed = FindComputedStyle(pattern);
    pattern_style = ResolveStyle(pattern, pattern_style, pattern_computed);

    // 图案内容只解析一次：逐图块铺排时不再重复匹配选择器与构造几何。
    // 从图案元素的子节点开始收集，图案元素自身属于定义类标签，不参与绘制。
    std::vector<PatternLeaf> leaves;
    for (const NodePtr& child : pattern.Children()) {
        if (!child || !child->IsElement()) {
            continue;
        }
        CollectPatternLeaves(static_cast<const Element&>(*child), pattern_style, SvgMatrix(),
                             &leaves);
    }
    if (leaves.empty()) {
        return;
    }
    // 内容若全部位于一个图块内，逐块绘制时无需裁剪；否则仍需按图块裁剪。
    bool content_inside_tile = !pattern.HasAttribute("viewbox");
    if (content_inside_tile) {
        for (const PatternLeaf& leaf : leaves) {
            if (leaf.is_text || leaf.geometry.subpaths.empty()) {
                content_inside_tile = false;
                break;
            }
            const std::vector<std::vector<SvgPoint>> polygons =
                TransformSubpaths(leaf.geometry.subpaths, leaf.matrix);
            const RectF bounds = BoundsOfPolygons(polygons);
            const float margin = leaf.style.stroke_width * 0.5f + 1.0f;
            if (bounds.left - margin < 0.0f || bounds.top - margin < 0.0f ||
                bounds.right + margin > tile_width_user ||
                bounds.bottom + margin > tile_height_user) {
                content_inside_tile = false;
                break;
            }
        }
    }

    for (int row = first_row; row <= last_row; ++row) {
        for (int column = first_column; column <= last_column; ++column) {
            const float origin_x = base_x + static_cast<float>(column) * tile_width_user;
            const float origin_y = base_y + static_cast<float>(row) * tile_height_user;
            const SvgMatrix tile_matrix =
                MultiplyMatrix(matrix, TranslationMatrix(origin_x, origin_y));
            if (!content_inside_tile) {
                // 图块在页面上的外接矩形。旋转由矩阵决定，这里取四个角的外接范围。
                const SvgPoint tile_corners[4] = {
                    TransformPoint(matrix, SvgPoint(origin_x, origin_y)),
                    TransformPoint(matrix, SvgPoint(origin_x + tile_width_user, origin_y)),
                    TransformPoint(matrix, SvgPoint(origin_x + tile_width_user,
                                                    origin_y + tile_height_user)),
                    TransformPoint(matrix, SvgPoint(origin_x, origin_y + tile_height_user)),
                };
                RectF tile_page;
                tile_page.left = tile_corners[0].x;
                tile_page.right = tile_corners[0].x;
                tile_page.top = tile_corners[0].y;
                tile_page.bottom = tile_corners[0].y;
                for (int index = 1; index < 4; ++index) {
                    tile_page.left = std::min(tile_page.left, tile_corners[index].x);
                    tile_page.right = std::max(tile_page.right, tile_corners[index].x);
                    tile_page.top = std::min(tile_page.top, tile_corners[index].y);
                    tile_page.bottom = std::max(tile_page.bottom, tile_corners[index].y);
                }
                tile_page = tile_page.Intersect(clip_);
                if (tile_page.IsEmpty()) {
                    continue;
                }
                params_.device->PushClip(tile_page);
            }
            for (const PatternLeaf& leaf : leaves) {
                const SvgMatrix leaf_matrix = MultiplyMatrix(tile_matrix, leaf.matrix);
                const float leaf_opacity = opacity * leaf.style.opacity;
                if (leaf.is_text) {
                    RenderText(*leaf.element, leaf.style, leaf_matrix, leaf_opacity);
                    continue;
                }
                // 小圆填充直接使用后端圆角矩形，避免为每个圆点构造扫描线。
                if (leaf.style.fill_reference.empty() && !leaf.style.fill_none &&
                    leaf.style.stroke_none && leaf.geometry.is_circle) {
                    const SvgPoint center =
                        TransformPoint(leaf_matrix, leaf.geometry.circle_center);
                    const float scale =
                        (HorizontalScale(leaf_matrix) + VerticalScale(leaf_matrix)) * 0.5f;
                    const float radius = leaf.geometry.circle_radius * scale;
                    if (radius > 0.0f && radius <= 5.0f) {
                        Color color = leaf.style.fill;
                        color.a *= leaf.style.fill_opacity * leaf_opacity;
                        const RectF circle_rect =
                            RectF::FromLTRB(center.x - radius, center.y - radius,
                                            center.x + radius, center.y + radius)
                                .Intersect(clip_);
                        if (!circle_rect.IsEmpty()) {
                            params_.device->FillRoundedRect(
                                circle_rect,
                                std::min(radius, std::min(circle_rect.Width(),
                                                          circle_rect.Height()) *
                                                     0.5f),
                                color);
                            continue;
                        }
                    }
                }
                RenderShape(*leaf.element, leaf.style, leaf_matrix, leaf.geometry, leaf_opacity);
            }
            if (!content_inside_tile) {
                params_.device->PopClip();
            }
        }
    }
}

void SvgRenderer::CollectPatternLeaves(const Element& element, const SvgStyle& parent_style,
                                       const SvgMatrix& parent_matrix,
                                       std::vector<PatternLeaf>* leaves) {
    const ComputedStyle* computed = FindComputedStyle(element);
    if (computed != nullptr && computed->display == Display::None) {
        return;
    }
    const std::string& tag = element.TagName();
    if (IsNonRenderedTag(tag)) {
        return;
    }
    const SvgStyle style = ResolveStyle(element, parent_style, computed);
    SvgMatrix matrix = parent_matrix;
    if (element.HasAttribute("transform")) {
        matrix = MultiplyMatrix(matrix, ParseTransform(element.GetAttribute("transform")));
    }
    if (IsShapeTag(tag)) {
        PatternLeaf leaf;
        leaf.element = &element;
        leaf.style = style;
        leaf.geometry = BuildShapeGeometry(element);
        leaf.matrix = matrix;
        if (!leaf.geometry.subpaths.empty()) {
            leaves->push_back(std::move(leaf));
        }
        return;
    }
    if (tag == "text") {
        PatternLeaf leaf;
        leaf.element = &element;
        leaf.style = style;
        leaf.matrix = matrix;
        leaf.is_text = true;
        leaves->push_back(std::move(leaf));
        return;
    }
    for (const NodePtr& child : element.Children()) {
        if (!child || !child->IsElement()) {
            continue;
        }
        CollectPatternLeaves(static_cast<const Element&>(*child), style, matrix, leaves);
    }
}

void SvgRenderer::DrawGlyphRun(const std::string& text, FontId font, float anchor_x,
                               float baseline_y, float scale, const Color& color) {
    if (font == 0 || !color.Visible() || params_.glyphs == nullptr || params_.atlas_width <= 0 ||
        params_.atlas_height <= 0) {
        return;
    }
    const float atlas_width = static_cast<float>(params_.atlas_width);
    const float atlas_height = static_cast<float>(params_.atlas_height);
    std::vector<GlyphQuad> quads;
    float pen_x = anchor_x;
    size_t offset = 0;
    while (offset < text.size()) {
        const Utf8Char character = DecodeUtf8(text, offset);
        if (character.length == 0) {
            break;
        }
        GlyphInfo glyph;
        const auto found = params_.glyphs->find(GlyphKey(font, character.codepoint));
        if (found != params_.glyphs->end()) {
            glyph = found->second;
        }
        if (glyph.has_bitmap && glyph.atlas_w > 0 && glyph.atlas_h > 0) {
            GlyphQuad quad;
            quad.x0 = std::floor(pen_x + glyph.bearing_x * scale + 0.5f);
            quad.y0 = std::floor(baseline_y - glyph.bearing_y * scale + 0.5f);
            quad.x1 = quad.x0 + static_cast<float>(glyph.atlas_w) * scale;
            quad.y1 = quad.y0 + static_cast<float>(glyph.atlas_h) * scale;
            quad.u0 = static_cast<float>(glyph.atlas_x) / atlas_width;
            quad.v0 = static_cast<float>(glyph.atlas_y) / atlas_height;
            quad.u1 = static_cast<float>(glyph.atlas_x + glyph.atlas_w) / atlas_width;
            quad.v1 = static_cast<float>(glyph.atlas_y + glyph.atlas_h) / atlas_height;
            quad.color = color;
            quads.push_back(quad);
        }
        pen_x += glyph.advance * scale;
        offset += character.length;
    }
    if (!quads.empty()) {
        params_.device->DrawGlyphs(quads.data(), quads.size());
    }
}

Color SvgRenderer::ResolveTextFill(const Element& element, const SvgStyle& style,
                                   float opacity) const {
    // 文本无法携带渐变或图案裁剪，取引用的首个有效颜色作为近似填充色。
    Color color = style.fill;
    if (!style.fill_reference.empty()) {
        const Element* reference = FindReference(element, style.fill_reference);
        if (reference != nullptr) {
            if (reference->TagName() == "lineargradient" ||
                reference->TagName() == "radialgradient") {
                for (const NodePtr& child : reference->Children()) {
                    if (!child || !child->IsElement() ||
                        static_cast<const Element&>(*child).TagName() != "stop") {
                        continue;
                    }
                    Color stop_color = Black();
                    if (static_cast<const Element&>(*child).HasAttribute("stop-color")) {
                        ParseSvgColor(static_cast<const Element&>(*child).GetAttribute("stop-color"),
                                      &stop_color);
                    }
                    color = stop_color;
                    break;
                }
            } else if (reference->TagName() == "pattern") {
                for (const NodePtr& child : reference->Children()) {
                    if (!child || !child->IsElement()) {
                        continue;
                    }
                    const Element& shape = static_cast<const Element&>(*child);
                    if (shape.HasAttribute("fill")) {
                        Color shape_color = Black();
                        if (ParseSvgColor(shape.GetAttribute("fill"), &shape_color)) {
                            color = shape_color;
                            break;
                        }
                    }
                }
            }
        }
    }
    color.a *= style.fill_opacity * opacity;
    return color;
}

void SvgRenderer::RenderText(const Element& element, const SvgStyle& style,
                             const SvgMatrix& matrix, float opacity) {
    if (style.fill_none && style.stroke_none) {
        return;
    }
    std::string text = TextContent(element);
    TrimAsciiSpace(&text);
    if (text.empty()) {
        return;
    }
    FontId font = params_.text->CreateFont(style.font);
    if (font == 0) {
        return;
    }
    const float x = AttributeNumber(element, "x", 0.0f) + AttributeNumber(element, "dx", 0.0f);
    const float y = AttributeNumber(element, "y", 0.0f) + AttributeNumber(element, "dy", 0.0f);
    const float width = params_.text->Measure(font, text);
    float anchor_x = x;
    if (style.text_anchor == SvgTextAnchor::Middle) {
        anchor_x -= width * 0.5f;
    } else if (style.text_anchor == SvgTextAnchor::End) {
        anchor_x -= width;
    }
    const FontMetrics metrics = params_.text->Metrics(font);
    float baseline = y;
    if (style.baseline == SvgBaseline::Central || style.baseline == SvgBaseline::Middle) {
        baseline += (metrics.ascent - metrics.descent) * 0.5f;
    } else if (style.baseline == SvgBaseline::Hanging) {
        baseline += metrics.ascent * 0.8f;
    }

    const SvgPoint anchor = TransformPoint(matrix, SvgPoint(anchor_x, baseline));
    const float scale = (HorizontalScale(matrix) + VerticalScale(matrix)) * 0.5f;
    if (scale <= 0.0f) {
        return;
    }

    const Color fill_color = ResolveTextFill(element, style, opacity);
    if (!style.fill_none && fill_color.Visible()) {
        DrawGlyphRun(text, font, anchor.x, anchor.y, scale, fill_color);
    }
}

// ---------------------------------------------------------------------------
// 命中测试
// ---------------------------------------------------------------------------

// 非零环绕规则判断点是否位于填充区域内。
bool PointInPolygons(const std::vector<SvgSubpath>& subpaths, float x, float y) {
    int winding = 0;
    for (const SvgSubpath& subpath : subpaths) {
        const size_t count = subpath.points.size();
        if (count < 3) {
            continue;
        }
        for (size_t index = 0; index < count; ++index) {
            const SvgPoint& p0 = subpath.points[index];
            const SvgPoint& p1 = subpath.points[(index + 1) % count];
            if (p0.y == p1.y) {
                continue;
            }
            if ((p0.y <= y && p1.y > y) || (p1.y <= y && p0.y > y)) {
                const float ratio = (y - p0.y) / (p1.y - p0.y);
                const float crossing = p0.x + (p1.x - p0.x) * ratio;
                if (crossing > x) {
                    winding += p1.y > p0.y ? 1 : -1;
                }
            }
        }
    }
    return winding != 0;
}

float DistanceToSegment(float x, float y, const SvgPoint& from, const SvgPoint& to) {
    const float dx = to.x - from.x;
    const float dy = to.y - from.y;
    const float length_square = dx * dx + dy * dy;
    if (length_square < 1e-8f) {
        const float px = x - from.x;
        const float py = y - from.y;
        return std::sqrt(px * px + py * py);
    }
    float ratio = ((x - from.x) * dx + (y - from.y) * dy) / length_square;
    ratio = std::min(std::max(ratio, 0.0f), 1.0f);
    const float px = x - (from.x + dx * ratio);
    const float py = y - (from.y + dy * ratio);
    return std::sqrt(px * px + py * py);
}

// 点到折线（含闭合边）的最小距离。
bool PointNearSubpaths(const std::vector<SvgSubpath>& subpaths, float x, float y,
                       float tolerance) {
    for (const SvgSubpath& subpath : subpaths) {
        const size_t count = subpath.points.size();
        if (count < 2) {
            continue;
        }
        const size_t segment_count = subpath.closed ? count : count - 1;
        for (size_t index = 0; index < segment_count; ++index) {
            if (DistanceToSegment(x, y, subpath.points[index],
                                  subpath.points[(index + 1) % count]) <= tolerance) {
                return true;
            }
        }
    }
    return false;
}

// 图元的命中判断。scale 为用户单位到页面像素的比例，用于把 1 像素的
// 容差换算回用户单位。
bool ShapeHitTest(const SvgStyle& style, const ShapeGeometry& geometry, const SvgPoint& point,
                  float scale) {
    const SvgPointerEvents mode = style.pointer_events;
    if (mode == SvgPointerEvents::None) {
        return false;
    }
    if (mode == SvgPointerEvents::BoundingBox) {
        const RectF bounds = BoundsOfPoints(geometry.subpaths);
        return point.x >= bounds.left && point.x <= bounds.right &&
               point.y >= bounds.top && point.y <= bounds.bottom;
    }
    const float pixel_tolerance = scale > 1e-6f ? 1.0f / scale : 1.0f;
    const float stroke_tolerance =
        std::max(style.stroke_width, 0.0f) * 0.5f + pixel_tolerance;
    if (mode == SvgPointerEvents::Fill || mode == SvgPointerEvents::VisibleFill) {
        return PointInPolygons(geometry.subpaths, point.x, point.y);
    }
    if (mode == SvgPointerEvents::Stroke || mode == SvgPointerEvents::VisibleStroke) {
        return PointNearSubpaths(geometry.subpaths, point.x, point.y, stroke_tolerance);
    }
    if (mode == SvgPointerEvents::All) {
        return PointInPolygons(geometry.subpaths, point.x, point.y) ||
               PointNearSubpaths(geometry.subpaths, point.x, point.y, stroke_tolerance);
    }
    // Auto 与 visiblePainted：填充区域与描边区域都可命中，透明填充同样算作
    // 已绘制，因此这里只判断 fill/stroke 是否为 none。
    const bool fill_hit =
        !style.fill_none && PointInPolygons(geometry.subpaths, point.x, point.y);
    const bool stroke_hit =
        !style.stroke_none && style.stroke_width > 0.0f &&
        PointNearSubpaths(geometry.subpaths, point.x, point.y, stroke_tolerance);
    return fill_hit || stroke_hit;
}

// 文本命中：按字体度量构造包围盒。字形轮廓不参与判断，与浏览器对文本
// 命中范围的近似一致。
bool TextHitTest(const Element& element, const SvgStyle& style, const SvgPoint& point,
                 float scale, TextService* text) {
    if (text == nullptr) {
        return false;
    }
    const bool painted = !style.fill_none || (!style.stroke_none && style.stroke_width > 0.0f);
    if (style.pointer_events != SvgPointerEvents::All &&
        style.pointer_events != SvgPointerEvents::BoundingBox && !painted) {
        return false;
    }
    std::string content = TextContent(element);
    TrimAsciiSpace(&content);
    if (content.empty()) {
        return false;
    }
    const FontId font = text->CreateFont(style.font);
    if (font == 0) {
        return false;
    }
    const float width = text->Measure(font, content);
    float anchor_x = AttributeNumber(element, "x", 0.0f) + AttributeNumber(element, "dx", 0.0f);
    if (style.text_anchor == SvgTextAnchor::Middle) {
        anchor_x -= width * 0.5f;
    } else if (style.text_anchor == SvgTextAnchor::End) {
        anchor_x -= width;
    }
    const FontMetrics metrics = text->Metrics(font);
    float baseline = AttributeNumber(element, "y", 0.0f) + AttributeNumber(element, "dy", 0.0f);
    if (style.baseline == SvgBaseline::Central || style.baseline == SvgBaseline::Middle) {
        baseline += (metrics.ascent - metrics.descent) * 0.5f;
    } else if (style.baseline == SvgBaseline::Hanging) {
        baseline += metrics.ascent * 0.8f;
    }
    const float pixel_tolerance = scale > 1e-6f ? 1.0f / scale : 1.0f;
    return point.x >= anchor_x - pixel_tolerance && point.x <= anchor_x + width + pixel_tolerance &&
           point.y >= baseline - metrics.ascent - pixel_tolerance &&
           point.y <= baseline + metrics.descent + pixel_tolerance;
}

}  // namespace

// ---------------------------------------------------------------------------
// 公共入口
// ---------------------------------------------------------------------------

void PaintSvgBox(const LayoutBox& box, const LayoutResult& layout, const SvgPaintParams& params) {
    if (box.element == nullptr || params.device == nullptr || params.text == nullptr) {
        return;
    }
    if (box.element->TagName() != "svg") {
        return;
    }
    const RectF content =
        box.content_box.Offset(params.offset_x, params.offset_y - params.page_top);
    if (content.IsEmpty()) {
        return;
    }
    // SVG 视口尺寸按内容盒换算成用户单位，供 userSpaceOnUse 百分比使用。
    float viewport_width = content.Width();
    float viewport_height = content.Height();
    SvgRenderer renderer(params, layout, content, viewport_width, viewport_height);
    params.device->PushClip(content);
    renderer.RenderRoot(*box.element);
    params.device->PopClip();
}

const Element* HitTestSvgBox(const LayoutBox& box, const LayoutResult& layout, float x, float y,
                             TextService* text) {
    if (box.element == nullptr || box.element->TagName() != "svg") {
        return nullptr;
    }
    const RectF content = box.content_box;
    if (!content.Contains(x, y)) {
        return nullptr;
    }

    auto find_computed = [&layout](const Element& element) -> const ComputedStyle* {
        const auto found = layout.svg_styles.find(&element);
        return found == layout.svg_styles.end() ? nullptr : &found->second;
    };

    SvgStyle root_style;
    root_style.font.size = 16.0f;
    root_style = ResolveStyle(*box.element, root_style, find_computed(*box.element));
    const SvgMatrix base = BuildSvgViewportMatrix(*box.element, content);

    // 按绘制顺序的逆序遍历：后绘制的元素覆盖先绘制的元素。
    std::function<const Element*(const Element&, const SvgStyle&, const SvgMatrix&)> hit_element =
        [&](const Element& element, const SvgStyle& parent_style,
            const SvgMatrix& parent_matrix) -> const Element* {
            const ComputedStyle* computed = find_computed(element);
            if (computed != nullptr && computed->display == Display::None) {
                return nullptr;
            }
            const std::string& tag = element.TagName();
            if (IsNonRenderedTag(tag)) {
                return nullptr;
            }
            const SvgStyle style = ResolveStyle(element, parent_style, computed);
            if (style.pointer_events == SvgPointerEvents::None) {
                return nullptr;
            }
            SvgMatrix matrix = parent_matrix;
            if (element.HasAttribute("transform")) {
                matrix = MultiplyMatrix(matrix, ParseTransform(element.GetAttribute("transform")));
            }
            SvgMatrix inverse;
            if (!InvertMatrix(matrix, &inverse)) {
                return nullptr;
            }
            const SvgPoint local = TransformPoint(inverse, SvgPoint(x, y));
            const float scale = (HorizontalScale(matrix) + VerticalScale(matrix)) * 0.5f;
            if (scale <= 1e-6f) {
                return nullptr;
            }

            if (IsShapeTag(tag)) {
                const ShapeGeometry geometry = BuildShapeGeometry(element);
                if (!geometry.subpaths.empty() &&
                    ShapeHitTest(style, geometry, local, scale)) {
                    return &element;
                }
                return nullptr;
            }
            if (tag == "text") {
                if (TextHitTest(element, style, local, scale, text)) {
                    return &element;
                }
                return nullptr;
            }
            const std::vector<NodePtr>& children = element.Children();
            for (auto child = children.rbegin(); child != children.rend(); ++child) {
                if (!*child || !(*child)->IsElement()) {
                    continue;
                }
                const Element* hit = hit_element(static_cast<const Element&>(**child), style,
                                                 matrix);
                if (hit != nullptr) {
                    return hit;
                }
            }
            return nullptr;
        };

    const std::vector<NodePtr>& children = box.element->Children();
    for (auto child = children.rbegin(); child != children.rend(); ++child) {
        if (!*child || !(*child)->IsElement()) {
            continue;
        }
        const Element* hit =
            hit_element(static_cast<const Element&>(**child), root_style, base);
        if (hit != nullptr) {
            return hit;
        }
    }
    return nullptr;
}

void CollectSvgBoxGlyphs(const LayoutBox& box, const LayoutResult& layout, TextService& text,
                         GlyphMap* glyphs) {
    if (glyphs == nullptr || box.element == nullptr || box.element->TagName() != "svg") {
        return;
    }

    // 遍历顺序与样式合成规则必须与绘制阶段完全一致，否则字形会按不同字体句柄
    // 进入图集，绘制时查不到位图。
    std::function<void(const Element&, const SvgStyle&)> visit =
        [&](const Element& element, const SvgStyle& parent_style) {
            const auto found = layout.svg_styles.find(&element);
            const ComputedStyle* computed =
                found == layout.svg_styles.end() ? nullptr : &found->second;
            if (computed != nullptr && computed->display == Display::None) {
                return;
            }
            const std::string& tag = element.TagName();
            if (IsNonRenderedTag(tag)) {
                return;
            }
            const SvgStyle style = ResolveStyle(element, parent_style, computed);
            if (tag == "text") {
                std::string content = TextContent(element);
                TrimAsciiSpace(&content);
                if (content.empty()) {
                    return;
                }
                const FontId font = text.CreateFont(style.font);
                if (font == 0) {
                    return;
                }
                size_t offset = 0;
                while (offset < content.size()) {
                    const Utf8Char character = DecodeUtf8(content, offset);
                    if (character.length == 0) {
                        break;
                    }
                    const GlyphKey key(font, character.codepoint);
                    if (glyphs->find(key) == glyphs->end()) {
                        (*glyphs)[key] = text.Rasterize(font, character.codepoint);
                    }
                    offset += character.length;
                }
                return;
            }
            for (const NodePtr& child : element.Children()) {
                if (child && child->IsElement()) {
                    visit(static_cast<const Element&>(*child), style);
                }
            }
        };

    SvgStyle root_style;
    root_style.font.size = 16.0f;
    const auto root_found = layout.svg_styles.find(box.element);
    const ComputedStyle* root_computed =
        root_found == layout.svg_styles.end() ? nullptr : &root_found->second;
    root_style = ResolveStyle(*box.element, root_style, root_computed);
    for (const NodePtr& child : box.element->Children()) {
        if (child && child->IsElement()) {
            visit(static_cast<const Element&>(*child), root_style);
        }
    }
}

}  // namespace web
