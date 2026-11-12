#include "web/style.h"

#include "web/css_parser.h"

#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace web {
namespace {

/* ----------------------------------------------------------------
   内置 UA 默认样式（浏览器默认样式的最小子集）

   说明：
   - html/body 为块级，外边距 8px，字号 16px；
   - 块级标签按 HTML 排版习惯设为 block，列表项为 list-item；
   - 标题字号以 em 指定（相对父元素字号），并在样式阶段解析为像素；
   - head/style/script 等不可见元素设为 display:none，由布局阶段排除；
   - 未出现在此表中的元素保持初始的 inline 显示方式。
   ---------------------------------------------------------------- */

const char kUserAgentStyleSheet[] = R"CSS(
html, body { display: block; margin: 8px; font-size: 16px; }
body { color: #1a1a1a; background-color: #ffffff; }
div, p, article, section, header, footer, main, nav, aside, figure, figcaption,
blockquote, pre, form, dl, dt, dd, details, summary, hr,
address, dialog, video, audio, object, hgroup, search { display: block; }
h1, h2, h3, h4, h5, h6 { display: block; margin: 1em 0; }
pre, code, kbd, samp, tt { font-family: Consolas; }
pre { white-space: pre; margin: 1em 0; }
code, kbd, samp { font-size: 0.95em; }
strong, b { font-weight: bold; }
em, i, cite, var { font-style: italic; }
small { font-size: 0.83em; }
big { font-size: 1.17em; }
mark { background-color: #ffff00; color: #000000; }
blockquote { margin: 1em 40px; }
figure { margin: 1em 40px; }
hr { margin: 0.5em auto; border: 0; border-top: 1px solid #c8c8c8; height: 0; }
fieldset { margin: 0 2px; padding: 0.35em 0.75em; border: 1px solid #c8c8c8; }
legend { padding: 0 2px; }
details { margin: 0; }
summary { cursor: pointer; }
dialog { position: absolute; margin: auto; border: 1px solid #767676; padding: 1em;
         background: #ffffff; color: #1a1a1a; }
dialog:not([open]) { display: none; }
[hidden] { display: none; }
progress, meter { display: inline-block; width: 10em; height: 1em; border: 1px solid #767676;
                  background-color: #eeeeee; padding: 0; }
video, audio, object { display: inline-block; width: 300px; height: 150px; }
input[type="range"] { display: inline-block; width: 160px; height: 16px; padding: 0;
                      border: 0; background-color: transparent; }
input[type="color"] { display: inline-block; width: 44px; height: 28px; padding: 0;
                      border: 1px solid #767676; border-radius: 2px; background-color: #ffffff; }
input[type="date"], input[type="time"], input[type="datetime-local"],
input[type="month"], input[type="week"] {
    display: inline-block; background-color: #ffffff; border: 1px solid #767676;
    padding: 2px 4px; width: 10em;
}
h1 { font-size: 2em; font-weight: bold; }
h2 { font-size: 1.5em; font-weight: bold; }
h3 { font-size: 1.17em; font-weight: bold; }
h4 { font-size: 1em; font-weight: bold; }
h5 { font-size: 0.83em; font-weight: bold; }
h6 { font-size: 0.67em; font-weight: bold; }
p { margin: 1em 0; }
ul, ol { display: block; padding-left: 40px; margin: 1em 0; }
ul { list-style-type: disc; }
ol { list-style-type: decimal; }
li { display: list-item; }
a { color: #0645ad; text-decoration: underline; }
span { display: inline; }
img { display: inline-block; }
canvas { display: inline-block; }
iframe { display: inline-block; }
br { display: inline; }
/* 表单控件：文本域与按钮按行内块排版，文本域保留空白与换行；
   提交、重置与普通按钮在没有作者样式时使用浏览器默认外观。 */
textarea { display: inline-block; white-space: pre-wrap; }
button { display: inline-block; text-align: center; }
input[type="submit"], input[type="reset"], input[type="button"] {
    display: inline-block; background: #efefef; color: #2b2723; border: 1px solid #767676;
    border-radius: 3px; padding: 2px 10px;
}
table { display: block; }
caption { display: block; text-align: center; }
thead, tbody, tfoot, tr { display: block; }
th { font-weight: bold; text-align: center; }
td { display: block; }
head, title, meta, link, base, style, script { display: none; }
)CSS";

const StyleSheet& UserAgentStyleSheet() {
    static const StyleSheet sheet = []() {
        StyleSheet parsed;
        std::string error;
        ParseStyleSheet(kUserAgentStyleSheet, &parsed, &error);
        return parsed;
    }();
    return sheet;
}

/* ----------------------------------------------------------------
   文本与取值工具
   ---------------------------------------------------------------- */

char LowerAsciiChar(char character) {
    if (character >= 'A' && character <= 'Z') {
        return static_cast<char>(character - 'A' + 'a');
    }
    return character;
}

std::string LowerAsciiText(const std::string& text) {
    std::string result = text;
    for (size_t index = 0; index < result.size(); ++index) {
        result[index] = LowerAsciiChar(result[index]);
    }
    return result;
}

CssValue MakeKeyword(const std::string& keyword) {
    CssValue value;
    value.kind = ValueKind::Keyword;
    value.keyword = keyword;
    return value;
}

CssValue MakeLength(CssLength length) {
    CssValue value;
    value.kind = ValueKind::Length;
    value.length = length;
    return value;
}

CssValue MakeString(const std::string& text) {
    CssValue value;
    value.kind = ValueKind::String;
    value.string_value = text;
    return value;
}

CssValue MakeColor(Color color) {
    CssValue value;
    value.kind = ValueKind::Color;
    value.color.color = color;
    value.color.specified = true;
    return value;
}

/* 把声明值展开成按顺序排列的分量；空声明返回空列表。 */
std::vector<CssValue> FlattenValue(const CssValue& value) {
    if (value.kind == ValueKind::List) {
        return value.items;
    }
    if (value.kind == ValueKind::Keyword && value.keyword.empty()) {
        return std::vector<CssValue>();
    }
    return std::vector<CssValue>(1, value);
}

bool IsSlash(const CssValue& value) {
    return value.kind == ValueKind::Keyword && value.keyword == "/";
}

/* 边长取值：长度或 auto；无单位数字按像素处理。 */
bool TryGetEdgeLength(const CssValue& value, CssLength* output) {
    if (value.kind == ValueKind::Keyword && LowerAsciiText(value.keyword) == "auto") {
        *output = CssLength{0.0f, LengthUnit::Auto};
        return true;
    }
    if (value.kind != ValueKind::Length) {
        return false;
    }
    CssLength length = value.length;
    if (length.unit == LengthUnit::Number) {
        length.unit = LengthUnit::Px;
    }
    *output = length;
    return true;
}

bool IsBorderStyleKeyword(const std::string& keyword) {
    return keyword == "solid" || keyword == "dashed" || keyword == "dotted" ||
           keyword == "double" || keyword == "groove" || keyword == "ridge" ||
           keyword == "inset" || keyword == "outset";
}

/* thin/medium/thick 对应像素宽度。 */
bool BorderWidthKeyword(const std::string& keyword, float* output) {
    if (keyword == "thin") {
        *output = 1.0f;
        return true;
    }
    if (keyword == "medium") {
        *output = 3.0f;
        return true;
    }
    if (keyword == "thick") {
        *output = 5.0f;
        return true;
    }
    return false;
}

/* ----------------------------------------------------------------
   简写展开
   ---------------------------------------------------------------- */

struct Longhand {
    std::string property;
    CssValue value;
};

void AddLonghand(std::vector<Longhand>* output, const std::string& property, const CssValue& value) {
    Longhand longhand;
    longhand.property = property;
    longhand.value = value;
    output->push_back(longhand);
}

/* 1~4 个值的四边分布规则：上、右、下、左。 */
bool AssignFourSides(const std::vector<CssValue>& items, CssValue* top, CssValue* right,
                     CssValue* bottom, CssValue* left) {
    switch (items.size()) {
    case 1:
        *top = items[0];
        *right = items[0];
        *bottom = items[0];
        *left = items[0];
        return true;
    case 2:
        *top = items[0];
        *bottom = items[0];
        *right = items[1];
        *left = items[1];
        return true;
    case 3:
        *top = items[0];
        *right = items[1];
        *left = items[1];
        *bottom = items[2];
        return true;
    case 4:
        *top = items[0];
        *right = items[1];
        *bottom = items[2];
        *left = items[3];
        return true;
    default:
        return false;
    }
}

/* margin / padding 简写。 */
void ExpandEdgeShorthand(const std::string& prefix, const CssValue& value,
                         std::vector<Longhand>* output) {
    std::vector<CssValue> items = FlattenValue(value);
    CssValue top;
    CssValue right;
    CssValue bottom;
    CssValue left;
    if (!AssignFourSides(items, &top, &right, &bottom, &left)) {
        return;
    }
    for (const CssValue& item : items) {
        CssLength length;
        if (!TryGetEdgeLength(item, &length)) {
            return;
        }
    }
    AddLonghand(output, prefix + "-top", top);
    AddLonghand(output, prefix + "-right", right);
    AddLonghand(output, prefix + "-bottom", bottom);
    AddLonghand(output, prefix + "-left", left);
}

/* border-width / border-color 简写。 */
void ExpandBorderFourSides(const std::string& prefix, const std::string& suffix,
                           const CssValue& value, std::vector<Longhand>* output) {
    std::vector<CssValue> items = FlattenValue(value);
    CssValue top;
    CssValue right;
    CssValue bottom;
    CssValue left;
    if (!AssignFourSides(items, &top, &right, &bottom, &left)) {
        return;
    }
    static const char* const kSides[] = {"top", "right", "bottom", "left"};
    const CssValue* values[] = {&top, &right, &bottom, &left};
    for (size_t index = 0; index < 4; ++index) {
        const CssValue& item = *values[index];
        if (suffix == "-width") {
            CssLength length;
            float keyword_width = 0.0f;
            bool valid = TryGetEdgeLength(item, &length);
            if (!valid && item.kind == ValueKind::Keyword &&
                BorderWidthKeyword(LowerAsciiText(item.keyword), &keyword_width)) {
                length = CssLength{keyword_width, LengthUnit::Px};
                valid = true;
            }
            if (!valid) {
                return;
            }
            AddLonghand(output, prefix + "-" + kSides[index] + suffix, MakeLength(length));
        } else {
            if (item.kind != ValueKind::Color) {
                return;
            }
            AddLonghand(output, prefix + "-" + kSides[index] + suffix, item);
        }
    }
}

/* border / border-top / border-right / border-bottom / border-left 简写。
   side 为空表示四边；计算样式不保存边框线型，none 会把宽度归零。 */
void ExpandBorderShorthand(const CssValue& value, const std::string& side,
                           std::vector<Longhand>* output) {
    std::vector<CssValue> items = FlattenValue(value);
    if (items.empty()) {
        return;
    }
    bool has_width = false;
    bool has_color = false;
    bool has_style = false;
    bool style_none = false;
    CssValue width;
    CssValue color;
    CssValue line_style;
    for (const CssValue& item : items) {
        if (item.kind == ValueKind::Length) {
            CssLength length = item.length;
            if (length.unit == LengthUnit::Number) {
                length.unit = LengthUnit::Px;
            }
            width = MakeLength(length);
            has_width = true;
            continue;
        }
        if (item.kind == ValueKind::Color) {
            color = item;
            has_color = true;
            continue;
        }
        if (item.kind == ValueKind::Keyword) {
            std::string keyword = LowerAsciiText(item.keyword);
            float keyword_width = 0.0f;
            if (keyword == "none") {
                style_none = true;
                has_style = true;
            } else if (IsBorderStyleKeyword(keyword)) {
                has_style = true;
                line_style = item;
            } else if (BorderWidthKeyword(keyword, &keyword_width)) {
                width = MakeLength(CssLength{keyword_width, LengthUnit::Px});
                has_width = true;
            }
        }
    }
    if (!has_width && !has_color && !has_style) {
        return;
    }
    if (style_none) {
        width = MakeLength(CssLength{0.0f, LengthUnit::Px});
        has_width = true;
        line_style = MakeKeyword("none");
    } else if (has_style && !has_width) {
        /* 只写了线型时按 CSS 初始值 medium（3px）。 */
        width = MakeLength(CssLength{3.0f, LengthUnit::Px});
        has_width = true;
    }

    static const char* const kSides[] = {"top", "right", "bottom", "left"};
    if (side.empty()) {
        for (const char* name : kSides) {
            if (has_width) {
                AddLonghand(output, "border-" + std::string(name) + "-width", width);
            }
            if (has_color) {
                AddLonghand(output, "border-" + std::string(name) + "-color", color);
            }
            if (has_style) {
                AddLonghand(output, "border-" + std::string(name) + "-style", line_style);
            }
        }
        return;
    }
    if (has_width) {
        AddLonghand(output, "border-" + side + "-width", width);
    }
    if (has_color) {
        AddLonghand(output, "border-" + side + "-color", color);
    }
    if (has_style) {
        AddLonghand(output, "border-" + side + "-style", line_style);
    }
}

/* border-style 简写：1~4 个线型关键字按上、右、下、左分布。 */
void ExpandBorderStyle(const CssValue& value, std::vector<Longhand>* output) {
    std::vector<CssValue> items = FlattenValue(value);
    CssValue top;
    CssValue right;
    CssValue bottom;
    CssValue left;
    if (!AssignFourSides(items, &top, &right, &bottom, &left)) {
        return;
    }
    for (const CssValue& item : items) {
        if (item.kind != ValueKind::Keyword) {
            return;
        }
        const std::string keyword = LowerAsciiText(item.keyword);
        if (keyword != "none" && !IsBorderStyleKeyword(keyword)) {
            return;
        }
    }
    static const char* const kSides[] = {"top", "right", "bottom", "left"};
    const CssValue* values[] = {&top, &right, &bottom, &left};
    for (size_t index = 0; index < 4; ++index) {
        AddLonghand(output, "border-" + std::string(kSides[index]) + "-style", *values[index]);
        // none 同时把该边宽度归零，避免残留宽度造成实线。
        if (LowerAsciiText(values[index]->keyword) == "none") {
            AddLonghand(output, "border-" + std::string(kSides[index]) + "-width",
                        MakeLength(CssLength{0.0f, LengthUnit::Px}));
        }
    }
}

/* border-radius 简写：1~4 个长度按左上、右上、右下、左下分布。
   斜杠后跟垂直半径，数量规则与水平半径相同；只有一个水平半径时垂直半径相同。 */
void ExpandBorderRadius(const CssValue& value, std::vector<Longhand>* output) {
    const std::vector<CssValue> items = FlattenValue(value);
    std::vector<CssValue> horizontal;
    std::vector<CssValue> vertical;
    bool after_slash = false;
    for (const CssValue& item : items) {
        if (IsSlash(item)) {
            after_slash = true;
            continue;
        }
        (after_slash ? vertical : horizontal).push_back(item);
    }
    if (horizontal.empty()) {
        return;
    }

    CssValue top_left;
    CssValue top_right;
    CssValue bottom_right;
    CssValue bottom_left;
    if (!AssignFourSides(horizontal, &top_left, &top_right, &bottom_right, &bottom_left)) {
        return;
    }
    CssValue vertical_top_left = top_left;
    CssValue vertical_top_right = top_right;
    CssValue vertical_bottom_right = bottom_right;
    CssValue vertical_bottom_left = bottom_left;
    if (!vertical.empty() &&
        !AssignFourSides(vertical, &vertical_top_left, &vertical_top_right, &vertical_bottom_right,
                         &vertical_bottom_left)) {
        return;
    }

    // 水平与垂直半径都可能含百分比，且各自相对边框盒宽、高解析，
    // 因此以两元素列表原样传给长写属性，由计算样式阶段判定单位。
    const CssValue* horizontal_values[4] = {&top_left, &top_right, &bottom_right, &bottom_left};
    const CssValue* vertical_values[4] = {&vertical_top_left, &vertical_top_right,
                                          &vertical_bottom_right, &vertical_bottom_left};
    static const char* const kCorners[4] = {"border-top-left-radius", "border-top-right-radius",
                                           "border-bottom-right-radius",
                                           "border-bottom-left-radius"};
    for (size_t index = 0; index < 4; ++index) {
        CssValue combined;
        combined.kind = ValueKind::List;
        combined.items.push_back(*horizontal_values[index]);
        combined.items.push_back(*vertical_values[index]);
        AddLonghand(output, kCorners[index], combined);
    }
}

/* ----------------------------------------------------------------
   渐变色标解析
   ---------------------------------------------------------------- */

// 前向声明：以下函数在值解析区定义，渐变解析先用到它们。
bool ParseValueComponents(const std::string& text, std::vector<CssValue>* output);
double ParseNumberLiteral(const std::string& literal);

bool IsSpaceCharacter(char character) {
    return character == ' ' || character == '\t' || character == '\n' || character == '\r' ||
           character == '\f';
}

/* 按顶层逗号分割（忽略括号内的逗号）。 */
std::vector<std::string> SplitTopLevelCommas(const std::string& text) {
    std::vector<std::string> parts;
    std::string current;
    int depth = 0;
    for (size_t index = 0; index < text.size(); ++index) {
        const char character = text[index];
        if (character == '(') {
            depth += 1;
        } else if (character == ')') {
            depth -= 1;
        }
        if (character == ',' && depth == 0) {
            parts.push_back(current);
            current.clear();
            continue;
        }
        current.push_back(character);
    }
    if (!current.empty() || !parts.empty()) {
        parts.push_back(current);
    }
    for (size_t index = 0; index < parts.size(); ++index) {
        size_t begin = 0;
        size_t end = parts[index].size();
        while (begin < end && IsSpaceCharacter(parts[index][begin])) {
            begin += 1;
        }
        while (end > begin && IsSpaceCharacter(parts[index][end - 1])) {
            end -= 1;
        }
        parts[index] = parts[index].substr(begin, end - begin);
    }
    return parts;
}

/* 解析一个色标：颜色 + 可选位置（百分比或长度按 100px 折算为比例）。 */
bool ParseGradientStop(const std::string& text, GradientStop* output) {
    std::vector<CssValue> parts;
    ParseValueComponents(text, &parts);
    bool has_color = false;
    for (const CssValue& part : parts) {
        if (part.kind == ValueKind::Color && !has_color) {
            output->color = part.color.color;
            has_color = true;
            continue;
        }
        if (part.kind == ValueKind::Length && !output->has_offset) {
            if (part.length.unit == LengthUnit::Percent) {
                output->offset = part.length.value / 100.0f;
                output->has_offset = true;
            } else if (part.length.unit == LengthUnit::Number) {
                output->offset = part.length.value / 100.0f;
                output->has_offset = true;
            }
        }
    }
    return has_color;
}

/* 方向关键字换算为角度（0 度指向上，顺时针）。 */
bool GradientDirectionAngle(const std::string& text, float* angle) {
    std::string value = LowerAsciiText(text);
    if (value == "to top") {
        *angle = 0.0f;
        return true;
    }
    if (value == "to right") {
        *angle = 90.0f;
        return true;
    }
    if (value == "to bottom") {
        *angle = 180.0f;
        return true;
    }
    if (value == "to left") {
        *angle = 270.0f;
        return true;
    }
    if (value == "to top right" || value == "to right top") {
        *angle = 45.0f;
        return true;
    }
    if (value == "to bottom right" || value == "to right bottom") {
        *angle = 135.0f;
        return true;
    }
    if (value == "to bottom left" || value == "to left bottom") {
        *angle = 225.0f;
        return true;
    }
    if (value == "to top left" || value == "to left top") {
        *angle = 315.0f;
        return true;
    }
    if (value.size() > 3 && value.compare(value.size() - 3, 3, "deg") == 0) {
        *angle = static_cast<float>(ParseNumberLiteral(value.substr(0, value.size() - 3)));
        return true;
    }
    if (value.size() > 4 && value.compare(value.size() - 4, 4, "turn") == 0) {
        *angle = static_cast<float>(ParseNumberLiteral(value.substr(0, value.size() - 4))) *
                 360.0f;
        return true;
    }
    if (value.size() > 4 && value.compare(value.size() - 4, 4, "grad") == 0) {
        *angle = static_cast<float>(ParseNumberLiteral(value.substr(0, value.size() - 4))) *
                 0.9f;
        return true;
    }
    if (value.size() > 3 && value.compare(value.size() - 3, 3, "rad") == 0) {
        *angle = static_cast<float>(ParseNumberLiteral(value.substr(0, value.size() - 3))) *
                 180.0f / 3.14159265f;
        return true;
    }
    return false;
}

/* 解析 radial-gradient 的中心描述：circle/ellipse [at x y]。 */
void ParseRadialPosition(const std::string& text, BackgroundImage* image) {
    const std::string lowered = LowerAsciiText(text);
    const size_t at = lowered.find("at");
    if (at == std::string::npos) {
        return;
    }
    const std::string position = lowered.substr(at + 2);
    std::vector<CssValue> parts;
    ParseValueComponents(position, &parts);
    size_t component = 0;
    for (const CssValue& part : parts) {
        if (part.kind != ValueKind::Length) {
            continue;
        }
        float ratio = 0.5f;
        if (part.length.unit == LengthUnit::Percent) {
            ratio = part.length.value / 100.0f;
        } else if (part.length.unit == LengthUnit::Number || part.length.unit == LengthUnit::Px) {
            ratio = part.length.value / 100.0f;
        }
        if (component == 0) {
            image->center_x = ratio;
        } else if (component == 1) {
            image->center_y = ratio;
        }
        component += 1;
        if (component >= 2) {
            break;
        }
    }
}

/* 解析渐变函数文本；非渐变返回 false。 */
bool ParseGradientText(const std::string& text, BackgroundImage* output) {
    const std::string lowered = LowerAsciiText(text);
    const size_t open = lowered.find('(');
    if (open == std::string::npos || lowered.size() < open + 2) {
        return false;
    }
    const std::string name = lowered.substr(0, open);
    if (name != "linear-gradient" && name != "radial-gradient" &&
        name != "repeating-linear-gradient" && name != "repeating-radial-gradient") {
        return false;
    }
    if (lowered[lowered.size() - 1] != ')') {
        return false;
    }
    const bool linear = name.find("linear") != std::string::npos;
    const std::string inner = text.substr(open + 1, text.size() - open - 2);
    std::vector<std::string> parts = SplitTopLevelCommas(inner);
    if (parts.empty()) {
        return false;
    }

    BackgroundImage image;
    image.kind = linear ? BackgroundImage::Kind::Linear : BackgroundImage::Kind::Radial;
    size_t first_stop = 0;
    if (linear) {
        // 首段若为方向描述（to … 或角度），则不是色标。
        if (GradientDirectionAngle(parts[0], &image.angle_deg)) {
            first_stop = 1;
        } else if (!parts[0].empty() && parts[0][0] != '#' &&
                   LowerAsciiText(parts[0]).find("rgb") == std::string::npos) {
            // 无颜色且不是方向时，视为非法首段并跳过。
            GradientStop probe;
            if (!ParseGradientStop(parts[0], &probe)) {
                first_stop = 1;
            }
        }
    } else {
        const std::string lowered_first = LowerAsciiText(parts[0]);
        if (lowered_first.find("circle") != std::string::npos ||
            lowered_first.find("ellipse") != std::string::npos ||
            lowered_first.find("closest") != std::string::npos ||
            lowered_first.find("farthest") != std::string::npos ||
            lowered_first.find("at ") != std::string::npos) {
            ParseRadialPosition(parts[0], &image);
            first_stop = 1;
        }
    }

    for (size_t index = first_stop; index < parts.size(); ++index) {
        GradientStop stop;
        if (!ParseGradientStop(parts[index], &stop)) {
            continue;
        }
        image.stops.push_back(stop);
    }
    if (image.stops.size() < 2) {
        return false;
    }
    *output = image;
    return true;
}

double ParseNumberLiteral(const std::string& literal) {
    size_t index = 0;
    bool negative = false;
    if (index < literal.size() && (literal[index] == '+' || literal[index] == '-')) {
        negative = literal[index] == '-';
        index += 1;
    }
    double value = 0.0;
    while (index < literal.size() && literal[index] >= '0' && literal[index] <= '9') {
        value = value * 10.0 + static_cast<double>(literal[index] - '0');
        index += 1;
    }
    if (index < literal.size() && literal[index] == '.') {
        index += 1;
        double scale = 0.1;
        while (index < literal.size() && literal[index] >= '0' && literal[index] <= '9') {
            value += static_cast<double>(literal[index] - '0') * scale;
            scale *= 0.1;
            index += 1;
        }
    }
    return negative ? -value : value;
}

/* background 简写：颜色分量、渐变（background-image）与图片地址。 */
void ExpandBackground(const CssValue& value, std::vector<Longhand>* output) {
    for (const CssValue& item : FlattenValue(value)) {
        if (item.kind == ValueKind::Color) {
            AddLonghand(output, "background-color", item);
            continue;
        }
        if (item.kind == ValueKind::Url) {
            AddLonghand(output, "background-image-url", item);
            continue;
        }
        if (item.kind == ValueKind::Keyword) {
            const std::string keyword = LowerAsciiText(item.keyword);
            if (keyword == "none") {
                AddLonghand(output, "background-color", MakeColor(Transparent()));
                continue;
            }
            BackgroundImage image;
            if (ParseGradientText(item.keyword, &image)) {
                AddLonghand(output, "background-image", item);
            }
        }
    }
}

bool IsFontWeightKeyword(const std::string& keyword, int* output) {
    if (keyword == "bold") {
        *output = 700;
        return true;
    }
    if (keyword == "bolder") {
        *output = 900;
        return true;
    }
    if (keyword == "lighter") {
        *output = 300;
        return true;
    }
    if (keyword == "normal") {
        *output = 400;
        return true;
    }
    return false;
}

/* font 简写：字号、行高、字重、斜体与族名。 */
void ExpandFontShorthand(const CssValue& value, std::vector<Longhand>* output) {
    std::vector<CssValue> items = FlattenValue(value);
    bool size_found = false;
    bool family_found = false;
    std::string family;
    size_t index = 0;
    while (index < items.size()) {
        const CssValue& item = items[index];
        if (item.kind == ValueKind::Length && !size_found) {
            AddLonghand(output, "font-size", item);
            size_found = true;
            index += 1;
            if (index + 1 < items.size() && IsSlash(items[index]) &&
                items[index + 1].kind == ValueKind::Length) {
                AddLonghand(output, "line-height", items[index + 1]);
                index += 2;
            }
            continue;
        }
        if (item.kind == ValueKind::String) {
            if (!family_found) {
                family = item.string_value;
                family_found = true;
            }
            index += 1;
            continue;
        }
        if (item.kind == ValueKind::Keyword) {
            std::string keyword = LowerAsciiText(item.keyword);
            if (keyword == "/") {
                index += 1;
                continue;
            }
            if (keyword == "italic" || keyword == "oblique") {
                AddLonghand(output, "font-style", MakeKeyword("italic"));
                index += 1;
                continue;
            }
            int weight = 0;
            if (IsFontWeightKeyword(keyword, &weight)) {
                if (keyword != "normal") {
                    AddLonghand(output, "font-weight", MakeLength(
                        CssLength{static_cast<float>(weight), LengthUnit::Number}));
                }
                index += 1;
                continue;
            }
            if (size_found && !family_found && keyword != "normal") {
                family = item.keyword;
                family_found = true;
            }
            index += 1;
            continue;
        }
        index += 1;
    }
    if (family_found) {
        AddLonghand(output, "font-family", MakeString(family));
    }
}

/* 列表样式类型关键字。 */
bool ListStyleKeyword(const std::string& keyword, ListStyleType* output) {
    if (keyword == "none") {
        *output = ListStyleType::None;
        return true;
    }
    if (keyword == "disc") {
        *output = ListStyleType::Disc;
        return true;
    }
    if (keyword == "circle") {
        *output = ListStyleType::Circle;
        return true;
    }
    if (keyword == "square") {
        *output = ListStyleType::Square;
        return true;
    }
    if (keyword == "decimal") {
        *output = ListStyleType::Decimal;
        return true;
    }
    if (keyword == "lower-roman") {
        *output = ListStyleType::LowerRoman;
        return true;
    }
    if (keyword == "upper-roman") {
        *output = ListStyleType::UpperRoman;
        return true;
    }
    if (keyword == "lower-alpha" || keyword == "lower-latin") {
        *output = ListStyleType::LowerAlpha;
        return true;
    }
    if (keyword == "upper-alpha" || keyword == "upper-latin") {
        *output = ListStyleType::UpperAlpha;
        return true;
    }
    return false;
}

/* list-style 简写：识别类型与位置关键字（list-style-image 暂不参与绘制）。 */
void ExpandListStyle(const CssValue& value, std::vector<Longhand>* output) {
    std::vector<CssValue> items = FlattenValue(value);
    for (const CssValue& item : items) {
        if (item.kind != ValueKind::Keyword) {
            continue;
        }
        const std::string keyword = LowerAsciiText(item.keyword);
        if (keyword == "inside" || keyword == "outside") {
            AddLonghand(output, "list-style-position", MakeKeyword(keyword));
            continue;
        }
        ListStyleType type = ListStyleType::None;
        if (ListStyleKeyword(keyword, &type)) {
            AddLonghand(output, "list-style-type", MakeKeyword(keyword));
        }
    }
}

/* flex 简写：none/auto 关键字，或「grow shrink basis」的自由顺序组合。
   只给出数字时按规范补 0% 基准，便于 flex:1 这类均分写法。 */
void ExpandFlex(const CssValue& value, std::vector<Longhand>* output) {
    std::vector<CssValue> items = FlattenValue(value);
    if (items.empty()) {
        return;
    }
    float grow = 0.0f;
    float shrink = 1.0f;
    bool has_grow = false;
    bool has_shrink = false;
    bool has_basis = false;
    CssValue basis;
    if (items.size() == 1 && items[0].kind == ValueKind::Keyword) {
        std::string keyword = LowerAsciiText(items[0].keyword);
        if (keyword == "none") {
            grow = 0.0f;
            shrink = 0.0f;
            basis = MakeKeyword("auto");
            has_basis = true;
        } else if (keyword == "auto") {
            grow = 1.0f;
            shrink = 1.0f;
            basis = MakeKeyword("auto");
            has_basis = true;
        } else {
            return;
        }
    } else {
        for (const CssValue& item : items) {
            if (item.kind == ValueKind::Length && item.length.IsNumber()) {
                if (!has_grow) {
                    grow = item.length.value;
                    has_grow = true;
                } else if (!has_shrink) {
                    shrink = item.length.value;
                    has_shrink = true;
                }
                continue;
            }
            if (item.kind == ValueKind::Length || item.kind == ValueKind::Keyword) {
                if (has_basis) {
                    return;
                }
                basis = item;
                has_basis = true;
                continue;
            }
        }
        if (!has_basis) {
            basis = MakeLength(CssLength{0.0f, LengthUnit::Percent});
            has_basis = true;
        }
    }
    AddLonghand(output, "flex-grow", MakeLength(CssLength{grow, LengthUnit::Number}));
    AddLonghand(output, "flex-shrink", MakeLength(CssLength{shrink, LengthUnit::Number}));
    if (has_basis) {
        AddLonghand(output, "flex-basis", basis);
    }
}

/* flex-flow 简写：方向与换行关键字的任意组合。 */
void ExpandFlexFlow(const CssValue& value, std::vector<Longhand>* output) {
    for (const CssValue& item : FlattenValue(value)) {
        if (item.kind != ValueKind::Keyword) {
            continue;
        }
        std::string keyword = LowerAsciiText(item.keyword);
        if (keyword == "row" || keyword == "row-reverse" || keyword == "column" ||
            keyword == "column-reverse") {
            AddLonghand(output, "flex-direction", MakeKeyword(keyword));
            continue;
        }
        if (keyword == "nowrap" || keyword == "wrap" || keyword == "wrap-reverse") {
            AddLonghand(output, "flex-wrap", MakeKeyword(keyword));
        }
    }
}

/* gap 简写：单值同时用于行列，两值依次为行间距、列间距。 */
void ExpandGap(const CssValue& value, std::vector<Longhand>* output) {
    std::vector<CssValue> items = FlattenValue(value);
    if (items.empty() || items.size() > 2) {
        return;
    }
    for (const CssValue& item : items) {
        if (item.kind != ValueKind::Length || item.length.value < 0.0f) {
            return;
        }
    }
    AddLonghand(output, "row-gap", items[0]);
    AddLonghand(output, "column-gap", items.size() > 1 ? items[1] : items[0]);
}

/* border-spacing 简写：单值同时用于水平与垂直，两值依次为水平、垂直。
   只接受非负长度；无单位数字按像素处理。 */
void ExpandBorderSpacing(const CssValue& value, std::vector<Longhand>* output) {
    std::vector<CssValue> items = FlattenValue(value);
    if (items.empty() || items.size() > 2) {
        return;
    }
    CssLength lengths[2];
    for (size_t index = 0; index < items.size(); ++index) {
        if (items[index].kind != ValueKind::Length) {
            return;
        }
        CssLength length = items[index].length;
        if (length.unit == LengthUnit::Number) {
            length.unit = LengthUnit::Px;
        }
        if (length.value < 0.0f) {
            return;
        }
        lengths[index] = length;
    }
    if (items.size() == 1) {
        lengths[1] = lengths[0];
    }
    AddLonghand(output, "border-spacing-x", MakeLength(lengths[0]));
    AddLonghand(output, "border-spacing-y", MakeLength(lengths[1]));
}

/* 把一条声明展开为若干长属性值。 */
void ExpandDeclaration(const Declaration& declaration, std::vector<Longhand>* output) {
    const std::string& property = declaration.property;
    if (property == "margin" || property == "padding") {
        ExpandEdgeShorthand(property, declaration.value, output);
        return;
    }
    if (property == "border") {
        ExpandBorderShorthand(declaration.value, std::string(), output);
        return;
    }
    if (property == "border-top" || property == "border-right" ||
        property == "border-bottom" || property == "border-left") {
        ExpandBorderShorthand(declaration.value, property.substr(std::string("border-").size()),
                              output);
        return;
    }
    if (property == "border-width" || property == "border-color") {
        ExpandBorderFourSides("border", property == "border-width" ? "-width" : "-color",
                              declaration.value, output);
        return;
    }
    if (property == "border-style") {
        ExpandBorderStyle(declaration.value, output);
        return;
    }
    if (property == "border-radius") {
        ExpandBorderRadius(declaration.value, output);
        return;
    }
    if (property == "border-spacing") {
        ExpandBorderSpacing(declaration.value, output);
        return;
    }
    if (property == "background") {
        ExpandBackground(declaration.value, output);
        return;
    }
    if (property == "font") {
        ExpandFontShorthand(declaration.value, output);
        return;
    }
    if (property == "list-style") {
        ExpandListStyle(declaration.value, output);
        return;
    }
    if (property == "flex") {
        ExpandFlex(declaration.value, output);
        return;
    }
    if (property == "flex-flow") {
        ExpandFlexFlow(declaration.value, output);
        return;
    }
    if (property == "gap" || property == "grid-gap") {
        ExpandGap(declaration.value, output);
        return;
    }
    AddLonghand(output, property, declaration.value);
}

/* ----------------------------------------------------------------
   层叠
   ---------------------------------------------------------------- */

enum Origin {
    kOriginUserAgent = 0,
    kOriginAuthor = 1,
    kOriginInline = 2,
};

struct Candidate {
    int important = 0;
    int origin = kOriginUserAgent;
    Specificity specificity;
    size_t sequence = 0;
    CssValue value;
};

bool IsBetter(const Candidate& candidate, const Candidate& current) {
    if (candidate.important != current.important) {
        return candidate.important > current.important;
    }
    if (candidate.origin != current.origin) {
        return candidate.origin > current.origin;
    }
    int compare = candidate.specificity.Compare(current.specificity);
    if (compare != 0) {
        return compare > 0;
    }
    return candidate.sequence > current.sequence;
}

using WinnerMap = std::map<std::string, Candidate>;

// 动画简写解析在层叠应用之后实现；此处先声明供 ApplyWinners 调用。
void ParseAnimationShorthand(const CssValue& value, std::vector<AnimationSpec>* output);

void ConsiderDeclaration(const Declaration& declaration, const Specificity& specificity, int origin,
                         size_t* sequence, WinnerMap* winners) {
    std::vector<Longhand> longhands;
    ExpandDeclaration(declaration, &longhands);
    for (const Longhand& longhand : longhands) {
        Candidate candidate;
        candidate.important = declaration.important ? 1 : 0;
        candidate.origin = origin;
        candidate.specificity = specificity;
        candidate.sequence = *sequence;
        candidate.value = longhand.value;
        *sequence += 1;

        WinnerMap::iterator position = winners->find(longhand.property);
        if (position == winners->end()) {
            winners->insert(std::make_pair(longhand.property, candidate));
        } else if (IsBetter(candidate, position->second)) {
            position->second = candidate;
        }
    }
}

/* 层叠前的原始声明。var() 需要先用本元素的自定义属性展开，才能做简写展开，
   因此匹配阶段只记录声明与来源信息，展开与简写展开延后到 ComputeFor 中。 */
struct RawDeclaration {
    Declaration declaration;
    Specificity specificity;
    int origin = kOriginUserAgent;
    size_t sequence = 0;
};

void CollectRawFromStyleSheet(const StyleSheet& sheet, const Element& element, int origin,
                              size_t* sequence, std::vector<RawDeclaration>* output) {
    for (const Rule& rule : sheet.rules) {
        bool matched = false;
        Specificity specificity;
        for (const Selector& selector : rule.selectors) {
            if (!MatchesSelector(selector, element)) {
                continue;
            }
            if (!matched || selector.specificity.Compare(specificity) > 0) {
                specificity = selector.specificity;
            }
            matched = true;
        }
        if (!matched) {
            continue;
        }
        for (const Declaration& declaration : rule.declarations) {
            RawDeclaration raw;
            raw.declaration = declaration;
            raw.specificity = specificity;
            raw.origin = origin;
            raw.sequence = *sequence;
            *sequence += 1;
            output->push_back(raw);
        }
    }
}

/* 选择器的最后一环是否带有指定伪元素（::before / ::after）。命中时把该
   简单选择器从副本中移除，其余部分按普通选择器与元素匹配。 */
bool SelectorTargetsPseudo(const Selector& selector, const Element& element,
                           const std::string& pseudo_name, Specificity* specificity) {
    if (selector.Empty()) {
        return false;
    }
    Selector filtered = selector;
    CompoundSelector& last = filtered.steps.back().compound;
    bool found = false;
    for (size_t index = 0; index < last.simples.size(); ++index) {
        const SimpleSelector& simple = last.simples[index];
        if (simple.kind == SimpleSelector::Kind::PseudoClass && simple.name == pseudo_name) {
            // 只允许一个伪元素简单选择器。
            if (found) {
                return false;
            }
            last.simples.erase(last.simples.begin() + static_cast<std::ptrdiff_t>(index));
            found = true;
            break;
        }
    }
    if (!found) {
        return false;
    }
    filtered.specificity = filtered.steps.back().compound.ComputeSpecificity();
    if (!MatchesSelector(filtered, element)) {
        return false;
    }
    *specificity = filtered.specificity;
    return true;
}

/* 收集伪元素规则中的声明。宿主元素不参与 :hover 等动态伪类的匹配。 */
void CollectRawFromPseudoStyleSheet(const StyleSheet& sheet, const Element& element,
                                    const std::string& pseudo_name, int origin,
                                    size_t* sequence, std::vector<RawDeclaration>* output) {
    for (const Rule& rule : sheet.rules) {
        bool matched = false;
        Specificity specificity;
        for (const Selector& selector : rule.selectors) {
            Specificity candidate;
            if (!SelectorTargetsPseudo(selector, element, pseudo_name, &candidate)) {
                continue;
            }
            if (!matched || candidate.Compare(specificity) > 0) {
                specificity = candidate;
            }
            matched = true;
        }
        if (!matched) {
            continue;
        }
        for (const Declaration& declaration : rule.declarations) {
            RawDeclaration raw;
            raw.declaration = declaration;
            raw.specificity = specificity;
            raw.origin = origin;
            raw.sequence = *sequence;
            *sequence += 1;
            output->push_back(raw);
        }
    }
}

/* 行内样式按来源优先级最高、specificity (1,0,0,0) 参与层叠。 */
void CollectRawInlineStyle(const Element& element, size_t* sequence,
                           std::vector<RawDeclaration>* output) {
    std::string text = element.GetAttribute("style");
    if (text.empty()) {
        return;
    }
    std::vector<Declaration> declarations;
    std::string error;
    if (!ParseDeclarationBlock(text, &declarations, &error)) {
        return;
    }
    Specificity inline_specificity;
    inline_specificity.ids = 1;
    for (const Declaration& declaration : declarations) {
        RawDeclaration raw;
        raw.declaration = declaration;
        raw.specificity = inline_specificity;
        raw.origin = kOriginInline;
        raw.sequence = *sequence;
        *sequence += 1;
        output->push_back(raw);
    }
}

/* ----------------------------------------------------------------
   自定义属性与 var()
   ---------------------------------------------------------------- */

bool IsCustomPropertyName(const std::string& property) {
    return property.size() > 2 && property[0] == '-' && property[1] == '-';
}

/* 该值是否为 var(--name[, fallback]) 函数。词法器已把函数名转成小写。 */
bool IsVarReference(const CssValue& value) {
    if (value.kind != ValueKind::Keyword) {
        return false;
    }
    const std::string& text = value.keyword;
    return text.size() > 5 && text.compare(0, 4, "var(") == 0 && text[text.size() - 1] == ')';
}

bool ContainsVarReference(const CssValue& value) {
    if (IsVarReference(value)) {
        return true;
    }
    if (value.kind == ValueKind::List) {
        for (const CssValue& item : value.items) {
            if (ContainsVarReference(item)) {
                return true;
            }
        }
    }
    return false;
}

std::string TrimAsciiSpace(const std::string& text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\n' ||
                           text[begin] == '\r' || text[begin] == '\f')) {
        begin += 1;
    }
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' ||
                           text[end - 1] == '\n' || text[end - 1] == '\r' ||
                           text[end - 1] == '\f')) {
        end -= 1;
    }
    return text.substr(begin, end - begin);
}

/* 拆出 var() 的实参：第一个顶层逗号之前为属性名，之后为回退值。 */
void SplitVarArguments(const std::string& keyword, std::string* name, std::string* fallback) {
    std::string inner = keyword.substr(4, keyword.size() - 5);
    int depth = 0;
    size_t split = std::string::npos;
    for (size_t index = 0; index < inner.size(); ++index) {
        char character = inner[index];
        if (character == '(') {
            depth += 1;
        } else if (character == ')') {
            depth -= 1;
        } else if (character == ',' && depth == 0) {
            split = index;
            break;
        }
    }
    if (split == std::string::npos) {
        *name = TrimAsciiSpace(inner);
        fallback->clear();
        return;
    }
    *name = TrimAsciiSpace(inner.substr(0, split));
    *fallback = TrimAsciiSpace(inner.substr(split + 1));
}

/* 把一段文本按值分量重新解析（用于 var() 的回退值与内联自定义属性）。 */
bool ParseValueComponents(const std::string& text, std::vector<CssValue>* output) {
    static const char kDummyProperty[] = "x";
    std::vector<Declaration> declarations;
    std::string error;
    std::string source = kDummyProperty;
    source += ": ";
    source += text;
    if (!ParseDeclarationBlock(source, &declarations, &error) || declarations.empty()) {
        return false;
    }
    *output = FlattenValue(declarations[0].value);
    return true;
}

std::vector<CssValue> ExpandVarList(const std::vector<CssValue>& items,
                                    const std::map<std::string, CssValue>& properties, int depth);

/* 取出一条 var() 引用对应的分量列表；未定义且无回退值时返回空列表。 */
std::vector<CssValue> ResolveVarReference(const std::string& keyword,
                                          const std::map<std::string, CssValue>& properties,
                                          int depth) {
    std::string name;
    std::string fallback;
    SplitVarArguments(keyword, &name, &fallback);
    if (depth <= 0) {
        return std::vector<CssValue>();
    }
    std::map<std::string, CssValue>::const_iterator found = properties.find(name);
    if (found != properties.end()) {
        return ExpandVarList(FlattenValue(found->second), properties, depth - 1);
    }
    std::vector<CssValue> parsed;
    if (!fallback.empty() && ParseValueComponents(fallback, &parsed)) {
        return ExpandVarList(parsed, properties, depth - 1);
    }
    return std::vector<CssValue>();
}

std::vector<CssValue> ExpandVarList(const std::vector<CssValue>& items,
                                    const std::map<std::string, CssValue>& properties,
                                    int depth) {
    std::vector<CssValue> result;
    for (const CssValue& item : items) {
        if (IsVarReference(item)) {
            std::vector<CssValue> resolved = ResolveVarReference(item.keyword, properties, depth);
            result.insert(result.end(), resolved.begin(), resolved.end());
            continue;
        }
        if (item.kind == ValueKind::List) {
            std::vector<CssValue> resolved = ExpandVarList(item.items, properties, depth);
            result.insert(result.end(), resolved.begin(), resolved.end());
            continue;
        }
        result.push_back(item);
    }
    return result;
}

/* 展开一条声明值中的 var()。整条声明无法解析时返回 false（按无效声明丢弃）。 */
bool ExpandVariables(const CssValue& value, const std::map<std::string, CssValue>& properties,
                     CssValue* output) {
    if (!ContainsVarReference(value)) {
        *output = value;
        return true;
    }
    std::vector<CssValue> items = ExpandVarList(FlattenValue(value), properties, 8);
    if (items.empty()) {
        return false;
    }
    if (items.size() == 1) {
        *output = items[0];
        return true;
    }
    output->kind = ValueKind::List;
    output->keyword.clear();
    output->items = items;
    return true;
}

/* ----------------------------------------------------------------
   长度函数求值：calc / min / max / clamp

   求值在样式计算阶段完成：此时字号、根字号与视口尺寸已知，em/rem/vw/vh 均可
   换算为像素；百分比保留为百分比（必要时与像素组合成 CalcPercent）。
   ---------------------------------------------------------------- */

// 长度表达式的线性值：百分比分量与像素分量，二者可相加。
struct LengthValue {
    float percent = 0.0f;
    float pixels = 0.0f;
    bool unitless = false;  // 无单位数字，可作乘除因子
    bool valid = false;
};

bool IsSpaceChar(char character) {
    return character == ' ' || character == '\t' || character == '\n' || character == '\r' ||
           character == '\f';
}

/* 读取一个数字记号（含正负号、小数与单位），推进 index。 */
bool ParseNumberToken(const std::string& text, size_t* index, float* value, std::string* unit) {
    size_t position = *index;
    if (position >= text.size()) {
        return false;
    }
    const size_t start = position;
    if (text[position] == '+' || text[position] == '-') {
        position += 1;
    }
    bool has_digit = false;
    while (position < text.size() && text[position] >= '0' && text[position] <= '9') {
        position += 1;
        has_digit = true;
    }
    if (position < text.size() && text[position] == '.') {
        position += 1;
        while (position < text.size() && text[position] >= '0' && text[position] <= '9') {
            position += 1;
            has_digit = true;
        }
    }
    if (!has_digit) {
        return false;
    }
    *value = static_cast<float>(ParseNumberLiteral(text.substr(start, position - start)));
    unit->clear();
    if (position < text.size() && text[position] == '%') {
        *unit = "%";
        position += 1;
    } else {
        while (position < text.size()) {
            const char character = text[position];
            const bool is_letter = (character >= 'a' && character <= 'z') ||
                                   (character >= 'A' && character <= 'Z');
            if (!is_letter) {
                break;
            }
            unit->push_back(static_cast<char>(LowerAsciiChar(character)));
            position += 1;
        }
    }
    *index = position;
    return true;
}

// 把带单位的字面量换算为 LengthValue。
bool ConvertLiteralTerm(float number, const std::string& unit, float font_size,
                        float root_font_size, float viewport_width, float viewport_height,
                        LengthValue* output) {
    LengthValue result;
    if (unit.empty()) {
        result.pixels = number;
        result.unitless = true;
    } else if (unit == "%") {
        result.percent = number;
    } else if (unit == "px") {
        result.pixels = number;
    } else if (unit == "em") {
        result.pixels = number * font_size;
    } else if (unit == "ex") {
        result.pixels = number * font_size * 0.5f;
    } else if (unit == "ch") {
        result.pixels = number * font_size * 0.5f;
    } else if (unit == "rem") {
        result.pixels = number * root_font_size;
    } else if (unit == "pt") {
        result.pixels = number * 96.0f / 72.0f;
    } else if (unit == "pc") {
        result.pixels = number * 16.0f;
    } else if (unit == "in") {
        result.pixels = number * 96.0f;
    } else if (unit == "cm") {
        result.pixels = number * 96.0f / 2.54f;
    } else if (unit == "mm") {
        result.pixels = number * 96.0f / 25.4f;
    } else if (unit == "vw") {
        result.pixels = number * viewport_width / 100.0f;
    } else if (unit == "vh") {
        result.pixels = number * viewport_height / 100.0f;
    } else if (unit == "vmin") {
        result.pixels = number * std::min(viewport_width, viewport_height) / 100.0f;
    } else if (unit == "vmax") {
        result.pixels = number * std::max(viewport_width, viewport_height) / 100.0f;
    } else {
        return false;
    }
    result.valid = true;
    *output = result;
    return true;
}

// 线性组合：左值 + 右值（符号为 -1 时相减）。
void CombineLengthValue(const LengthValue& left, const LengthValue& right, float sign,
                        LengthValue* output) {
    output->percent = left.percent + right.percent * sign;
    output->pixels = left.pixels + right.pixels * sign;
    output->unitless = false;
    output->valid = left.valid && right.valid;
}

// 单独存放求值上下文，避免递归函数传参过长。
struct LengthEvalContext {
    float font_size = 16.0f;
    float root_font_size = 16.0f;
    float viewport_width = 860.0f;
    float viewport_height = 800.0f;
};

/* 递归下降求值器：表达式 → 项 → 因子。支持 + - * / 与括号、嵌套函数。 */
class LengthEvaluator {
public:
    LengthEvaluator(const std::string& text, const LengthEvalContext& context)
        : text_(text), context_(context) {}

    bool Evaluate(LengthValue* output) {
        if (!EvaluateExpression(output)) {
            return false;
        }
        SkipSpace();
        return index_ >= text_.size();
    }

private:
    void SkipSpace() {
        while (index_ < text_.size() && IsSpaceChar(text_[index_])) {
            index_ += 1;
        }
    }

    bool EvaluateExpression(LengthValue* output) {
        LengthValue left;
        if (!EvaluateTerm(&left)) {
            return false;
        }
        while (true) {
            SkipSpace();
            if (index_ >= text_.size()) {
                break;
            }
            const char op = text_[index_];
            if (op != '+' && op != '-') {
                break;
            }
            index_ += 1;
            LengthValue right;
            if (!EvaluateTerm(&right)) {
                return false;
            }
            CombineLengthValue(left, right, op == '+' ? 1.0f : -1.0f, &left);
            if (!left.valid) {
                return false;
            }
        }
        *output = left;
        return true;
    }

    bool EvaluateTerm(LengthValue* output) {
        LengthValue left;
        if (!EvaluateFactor(&left)) {
            return false;
        }
        while (true) {
            SkipSpace();
            if (index_ >= text_.size()) {
                break;
            }
            const char op = text_[index_];
            if (op != '*' && op != '/') {
                break;
            }
            index_ += 1;
            LengthValue right;
            if (!EvaluateFactor(&right)) {
                return false;
            }
            // 乘除只允许「长度 × 数字」或「长度 ÷ 数字」。
            if (left.unitless && !right.unitless) {
                std::swap(left, right);
            }
            if (!right.unitless || !right.valid || !left.valid) {
                return false;
            }
            if (op == '*') {
                left.percent *= right.pixels;
                left.pixels *= right.pixels;
            } else {
                if (right.pixels == 0.0f) {
                    return false;
                }
                left.percent /= right.pixels;
                left.pixels /= right.pixels;
            }
            left.unitless = false;
        }
        *output = left;
        return true;
    }

    bool EvaluateFactor(LengthValue* output) {
        SkipSpace();
        if (index_ >= text_.size()) {
            return false;
        }
        if (text_[index_] == '(') {
            index_ += 1;
            if (!EvaluateExpression(output)) {
                return false;
            }
            SkipSpace();
            if (index_ >= text_.size() || text_[index_] != ')') {
                return false;
            }
            index_ += 1;
            return true;
        }
        if (text_[index_] == '+' || text_[index_] == '-') {
            const float sign = text_[index_] == '-' ? -1.0f : 1.0f;
            index_ += 1;
            LengthValue inner;
            if (!EvaluateFactor(&inner)) {
                return false;
            }
            inner.percent *= sign;
            inner.pixels *= sign;
            *output = inner;
            return true;
        }

        // 函数名 (…)：calc / min / max / clamp。
        const size_t name_start = index_;
        while (index_ < text_.size()) {
            const char character = text_[index_];
            const bool is_letter = (character >= 'a' && character <= 'z') ||
                                   (character >= 'A' && character <= 'Z');
            if (!is_letter) {
                break;
            }
            index_ += 1;
        }
        if (index_ > name_start) {
            const std::string name = LowerAsciiText(text_.substr(name_start, index_ - name_start));
            SkipSpace();
            if (index_ < text_.size() && text_[index_] == '(') {
                index_ += 1;
                if (!EvaluateFunction(name, output)) {
                    return false;
                }
                SkipSpace();
                if (index_ >= text_.size() || text_[index_] != ')') {
                    return false;
                }
                index_ += 1;
                return true;
            }
            // 不是函数：退回数字解析起点。
            index_ = name_start;
        }

        float number = 0.0f;
        std::string unit;
        if (!ParseNumberToken(text_, &index_, &number, &unit)) {
            return false;
        }
        return ConvertLiteralTerm(number, unit, context_.font_size, context_.root_font_size,
                                  context_.viewport_width, context_.viewport_height, output);
    }

    // 读取函数实参列表；结束时停在不消耗 ')' 的位置。
    bool EvaluateArguments(std::vector<LengthValue>* output) {
        while (true) {
            SkipSpace();
            if (index_ >= text_.size()) {
                return false;
            }
            if (text_[index_] == ')') {
                return true;
            }
            LengthValue value;
            if (!EvaluateExpression(&value)) {
                return false;
            }
            output->push_back(value);
            SkipSpace();
            if (index_ < text_.size() && text_[index_] == ',') {
                index_ += 1;
                continue;
            }
            return true;
        }
    }

    bool EvaluateFunction(const std::string& name, LengthValue* output) {
        std::vector<LengthValue> arguments;
        if (!EvaluateArguments(&arguments) || arguments.empty()) {
            return false;
        }
        for (const LengthValue& argument : arguments) {
            if (!argument.valid) {
                return false;
            }
        }

        if (name == "calc") {
            *output = arguments[0];
            return arguments.size() == 1;
        }
        if (name == "min" || name == "max") {
            // 全部为纯像素时可以直接取极值；含百分比时返回第一个分量。
            bool all_pure = true;
            for (const LengthValue& argument : arguments) {
                if (argument.percent != 0.0f) {
                    all_pure = false;
                    break;
                }
            }
            if (!all_pure) {
                *output = arguments[0];
                return true;
            }
            LengthValue best = arguments[0];
            for (const LengthValue& argument : arguments) {
                if (name == "min") {
                    if (argument.pixels < best.pixels) {
                        best = argument;
                    }
                } else {
                    if (argument.pixels > best.pixels) {
                        best = argument;
                    }
                }
            }
            *output = best;
            return true;
        }
        if (name == "clamp") {
            if (arguments.size() != 3) {
                return false;
            }
            const LengthValue& lower = arguments[0];
            const LengthValue& preferred = arguments[1];
            const LengthValue& upper = arguments[2];
            if (lower.percent == 0.0f && upper.percent == 0.0f && preferred.percent == 0.0f) {
                LengthValue result = preferred;
                if (result.pixels < lower.pixels) {
                    result.pixels = lower.pixels;
                }
                if (result.pixels > upper.pixels) {
                    result.pixels = upper.pixels;
                }
                *output = result;
                return true;
            }
            // 含百分比时无法线性折叠：保留首选值（其百分比与像素组合仍然正确）。
            *output = preferred;
            return true;
        }
        return false;
    }

    const std::string& text_;
    const LengthEvalContext& context_;
    size_t index_ = 0;
};

/* 把 Keyword 形式的长度函数求值为 CssLength。成功返回 true。 */
bool EvaluateLengthFunctionText(const std::string& text, const LengthEvalContext& context,
                                CssLength* output) {
    LengthEvaluator evaluator(text, context);
    LengthValue value;
    if (!evaluator.Evaluate(&value) || !value.valid) {
        return false;
    }
    if (value.percent == 0.0f) {
        *output = CssLength(value.pixels, LengthUnit::Px);
    } else {
        *output = CssLength(value.percent, value.pixels);
    }
    return true;
}

bool IsLengthFunctionText(const std::string& keyword) {
    const std::string lowered = LowerAsciiText(keyword);
    return lowered.compare(0, 5, "calc(") == 0 || lowered.compare(0, 4, "min(") == 0 ||
           lowered.compare(0, 4, "max(") == 0 || lowered.compare(0, 6, "clamp(") == 0;
}

/* 递归归一化值中的长度函数：Keyword 形式的 calc/min/max/clamp 就地转为 Length。
   函数文本不合法时返回 false，调用方按无效声明丢弃。 */
bool NormalizeLengthFunctions(const CssValue& value, const LengthEvalContext& context,
                              CssValue* output) {
    if (value.kind == ValueKind::Keyword && IsLengthFunctionText(value.keyword)) {
        CssLength length;
        if (!EvaluateLengthFunctionText(value.keyword, context, &length)) {
            return false;
        }
        output->kind = ValueKind::Length;
        output->length = length;
        output->keyword.clear();
        return true;
    }
    if (value.kind == ValueKind::List) {
        CssValue result;
        result.kind = ValueKind::List;
        for (const CssValue& item : value.items) {
            CssValue normalized;
            if (!NormalizeLengthFunctions(item, context, &normalized)) {
                return false;
            }
            result.items.push_back(normalized);
        }
        *output = result;
        return true;
    }
    *output = value;
    return true;
}

/* 自定义属性参与层叠（不做简写展开），结果先继承父元素再被本元素覆盖。 */
void ResolveCustomProperties(const std::vector<RawDeclaration>& declarations,
                             const std::map<std::string, CssValue>& inherited,
                             std::map<std::string, CssValue>* output) {
    *output = inherited;
    WinnerMap winners;
    for (const RawDeclaration& raw : declarations) {
        if (!IsCustomPropertyName(raw.declaration.property)) {
            continue;
        }
        Candidate candidate;
        candidate.important = raw.declaration.important ? 1 : 0;
        candidate.origin = raw.origin;
        candidate.specificity = raw.specificity;
        candidate.sequence = raw.sequence;
        candidate.value = raw.declaration.value;

        WinnerMap::iterator position = winners.find(raw.declaration.property);
        if (position == winners.end()) {
            winners.insert(std::make_pair(raw.declaration.property, candidate));
        } else if (IsBetter(candidate, position->second)) {
            position->second = candidate;
        }
    }
    for (const auto& entry : winners) {
        (*output)[entry.first] = entry.second.value;
    }
}

/* ----------------------------------------------------------------
   阴影、变换、网格轨道解析
   ---------------------------------------------------------------- */

float ResolvePixelsForStyle(const CssLength& length, float font_size, float root_font_size,
                            float viewport_width, float viewport_height) {
    switch (length.unit) {
        case LengthUnit::Px:
        case LengthUnit::Number:
            return length.value;
        case LengthUnit::Em:
            return length.value * font_size;
        case LengthUnit::Ex:
        case LengthUnit::Ch:
            return length.value * font_size * 0.5f;
        case LengthUnit::Rem:
            return length.value * root_font_size;
        case LengthUnit::Percent:
            return length.value * 0.01f;
        case LengthUnit::CalcPercent:
            return length.value * 0.01f + length.offset;
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
        case LengthUnit::Vw:
            return length.value * viewport_width / 100.0f;
        case LengthUnit::Vh:
            return length.value * viewport_height / 100.0f;
        case LengthUnit::Vmin:
            return length.value * std::min(viewport_width, viewport_height) / 100.0f;
        case LengthUnit::Vmax:
            return length.value * std::max(viewport_width, viewport_height) / 100.0f;
        default:
            return 0.0f;
    }
}

/* 阴影列表：每个阴影由长度（依次偏移、模糊、扩展）、颜色与 inset 组成。 */
bool ParseShadowList(const CssValue& value, float font_size, float root_font_size,
                     float viewport_width, float viewport_height,
                     std::vector<Shadow>* output) {
    std::vector<CssValue> items = FlattenValue(value);
    std::vector<std::vector<CssValue>> groups;
    std::vector<CssValue> current;
    for (const CssValue& item : items) {
        if (item.kind == ValueKind::Keyword && item.keyword == ",") {
            if (!current.empty()) {
                groups.push_back(current);
                current.clear();
            }
            continue;
        }
        current.push_back(item);
    }
    if (!current.empty()) {
        groups.push_back(current);
    }
    if (groups.empty()) {
        return false;
    }

    for (const std::vector<CssValue>& group : groups) {
        Shadow shadow;
        size_t length_index = 0;
        bool has_color = false;
        for (const CssValue& item : group) {
            if (item.kind == ValueKind::Color) {
                shadow.color = item.color.color;
                has_color = true;
                continue;
            }
            if (item.kind == ValueKind::Keyword) {
                const std::string keyword = LowerAsciiText(item.keyword);
                if (keyword == "inset") {
                    shadow.inset = true;
                    continue;
                }
                if (keyword == "none") {
                    return false;
                }
            }
            CssLength length;
            if (!TryGetEdgeLength(item, &length)) {
                return false;
            }
            const float pixels = ResolvePixelsForStyle(length, font_size, root_font_size,
                                                       viewport_width, viewport_height);
            if (length_index == 0) {
                shadow.offset_x = pixels;
            } else if (length_index == 1) {
                shadow.offset_y = pixels;
            } else if (length_index == 2) {
                shadow.blur = pixels < 0.0f ? 0.0f : pixels;
            } else if (length_index == 3) {
                shadow.spread = pixels;
            } else {
                return false;
            }
            length_index += 1;
        }
        if (length_index < 2) {
            return false;
        }
        if (!has_color) {
            shadow.color = Rgba(0.0f, 0.0f, 0.0f, 0.5f);
        }
        output->push_back(shadow);
    }
    return true;
}

/* transform：解析 translate / scale 系列函数。偏移保留为长度，布局阶段按盒尺寸
   解析百分比（translate(-50%) 依赖盒宽）。 */
bool ParseTransformText(const std::string& text, ComputedStyle* style) {
    const std::string lowered = LowerAsciiText(text);
    size_t index = 0;
    CssLength translate_x{0.0f, LengthUnit::Px};
    CssLength translate_y{0.0f, LengthUnit::Px};
    float scale_x = 1.0f;
    float scale_y = 1.0f;

    while (index < lowered.size()) {
        const size_t open = lowered.find('(', index);
        if (open == std::string::npos) {
            break;
        }
        const std::string name = lowered.substr(index, open - index);
        const size_t close = lowered.find(')', open);
        if (close == std::string::npos) {
            return false;
        }
        const std::string inner = text.substr(open + 1, close - open - 1);
        std::vector<CssValue> arguments;
        ParseValueComponents(inner, &arguments);
        std::vector<CssValue> values;
        for (const CssValue& item : arguments) {
            if (item.kind == ValueKind::Keyword && item.keyword == ",") {
                continue;
            }
            values.push_back(item);
        }
        index = close + 1;
        if (values.empty()) {
            continue;
        }

        if (name.find("translate") != std::string::npos) {
            CssLength first;
            if (!TryGetEdgeLength(values[0], &first)) {
                return false;
            }
            if (name.find("translatey") != std::string::npos) {
                translate_y = first;
            } else if (name.find("translatez") != std::string::npos) {
                continue;
            } else {
                translate_x = first;
                if (values.size() > 1) {
                    CssLength second;
                    if (!TryGetEdgeLength(values[1], &second)) {
                        return false;
                    }
                    translate_y = second;
                }
            }
            continue;
        }
        if (name.find("scale") != std::string::npos) {
            if (values[0].kind != ValueKind::Length || !values[0].length.IsNumber()) {
                return false;
            }
            const float sx = values[0].length.value;
            if (name.find("scaley") != std::string::npos) {
                scale_y = sx;
            } else if (name.find("scalez") != std::string::npos) {
                continue;
            } else {
                scale_x = sx;
                scale_y = values.size() > 1 && values[1].kind == ValueKind::Length &&
                                  values[1].length.IsNumber()
                              ? values[1].length.value
                              : sx;
            }
            continue;
        }
        // 其它变换函数（rotate/skew/matrix）不受支持，按恒等处理。
    }
    style->transform_translate_x = translate_x;
    style->transform_translate_y = translate_y;
    style->scale_x = scale_x;
    style->scale_y = scale_y;
    return true;
}

/* 网格轨道：支持长度、fr、repeat()、minmax() 与 auto。 */
bool ParseGridTracks(const std::string& text, int repeat_depth, std::vector<GridTrack>* output);

bool ParseSingleGridTrack(const std::string& text, GridTrack* output) {
    const std::string lowered = LowerAsciiText(text);
    if (lowered == "auto" || lowered == "min-content" || lowered == "max-content" ||
        lowered == "fit-content") {
        output->length = CssLength{0.0f, LengthUnit::Auto};
        output->fraction = 0.0f;
        return true;
    }
    if (lowered.size() > 2 && lowered.compare(lowered.size() - 2, 2, "fr") == 0) {
        output->fraction =
            static_cast<float>(ParseNumberLiteral(lowered.substr(0, lowered.size() - 2)));
        output->length = CssLength{0.0f, LengthUnit::Px};
        return output->fraction > 0.0f;
    }
    CssLength length;
    std::vector<CssValue> parsed;
    ParseValueComponents(lowered, &parsed);
    if (parsed.empty() || parsed[0].kind != ValueKind::Length) {
        return false;
    }
    length = parsed[0].length;
    if (length.unit == LengthUnit::Number) {
        length.unit = LengthUnit::Px;
    }
    output->length = length;
    output->fraction = 0.0f;
    return true;
}

bool ParseGridTracks(const std::string& text, int repeat_depth, std::vector<GridTrack>* output) {
    if (repeat_depth > 4) {
        return false;
    }
    // 按空白切分顶层记号，括号内内容保持完整。
    std::vector<std::string> tokens;
    std::string current;
    int depth = 0;
    for (size_t index = 0; index < text.size(); ++index) {
        const char character = text[index];
        if (character == '(') {
            depth += 1;
        } else if (character == ')') {
            depth -= 1;
        }
        if (depth == 0 && IsSpaceCharacter(character)) {
            if (!current.empty()) {
                tokens.push_back(current);
                current.clear();
            }
            continue;
        }
        current.push_back(character);
    }
    if (!current.empty()) {
        tokens.push_back(current);
    }

    for (const std::string& token : tokens) {
        if (token.empty()) {
            continue;
        }
        const std::string lowered = LowerAsciiText(token);
        if (lowered.compare(0, 7, "repeat(") == 0 && lowered.size() > 8) {
            // repeat(次数, 轨道列表)：次数为整数时重复展开。
            const size_t comma = lowered.find(',');
            if (comma == std::string::npos) {
                return false;
            }
            const std::string count_text = lowered.substr(7, comma - 7);
            const bool auto_fill = count_text == "auto-fill" || count_text == "auto-fit";
            std::string inner = token.substr(comma + 1);
            if (!inner.empty() && inner[inner.size() - 1] == ')') {
                inner.erase(inner.size() - 1);
            }
            if (auto_fill) {
                // auto-fill / auto-fit：轨道数量由布局阶段按容器宽度决定，
                // 这里保留一条带标记的轨道作为模板。
                std::vector<GridTrack> repeated;
                if (!ParseGridTracks(inner, repeat_depth + 1, &repeated) || repeated.empty()) {
                    return false;
                }
                for (GridTrack track : repeated) {
                    track.auto_repeat = true;
                    track.auto_fit = count_text == "auto-fit";
                    output->push_back(track);
                }
                continue;
            }
            const int count = static_cast<int>(ParseNumberLiteral(count_text));
            if (count <= 0 || count > 64) {
                return false;
            }
            std::vector<GridTrack> repeated;
            if (!ParseGridTracks(inner, repeat_depth + 1, &repeated) || repeated.empty()) {
                return false;
            }
            for (int index = 0; index < count; ++index) {
                for (const GridTrack& track : repeated) {
                    output->push_back(track);
                }
            }
            continue;
        }
        if (lowered.compare(0, 7, "minmax(") == 0) {
            // minmax(最小值, 最大值)：按最大值参与分配。
            const size_t comma = lowered.find(',');
            if (comma == std::string::npos) {
                return false;
            }
            std::string minimum = token.substr(7, comma - 7);
            std::string maximum = token.substr(comma + 1);
            if (!maximum.empty() && maximum[maximum.size() - 1] == ')') {
                maximum.erase(maximum.size() - 1);
            }
            GridTrack track;
            if (!ParseSingleGridTrack(maximum, &track)) {
                return false;
            }
            GridTrack minimum_track;
            if (ParseSingleGridTrack(minimum, &minimum_track)) {
                track.minimum = minimum_track.length;
            }
            output->push_back(track);
            continue;
        }
        GridTrack track;
        if (!ParseSingleGridTrack(token, &track)) {
            return false;
        }
        output->push_back(track);
    }
    return !output->empty();
}

/* 把值序列还原为文本，供需要原文的属性（网格轨道、变换）继续解析。 */
std::string ValueListText(const CssValue& value) {
    if (value.kind != ValueKind::List) {
        if (value.kind == ValueKind::Keyword) {
            return value.keyword;
        }
        if (value.kind == ValueKind::Length) {
            const CssLength& length = value.length;
            std::string text = std::to_string(length.value);
            // 去掉小数部分的尾随零，保持文本紧凑（整数部分不受影响）。
            if (text.find('.') != std::string::npos) {
                while (!text.empty() && text.back() == '0') {
                    text.pop_back();
                }
                if (!text.empty() && text.back() == '.') {
                    text.pop_back();
                }
            }
            switch (length.unit) {
                case LengthUnit::Px:
                    return text + "px";
                case LengthUnit::Em:
                    return text + "em";
                case LengthUnit::Rem:
                    return text + "rem";
                case LengthUnit::Percent:
                    return text + "%";
                case LengthUnit::Vw:
                    return text + "vw";
                case LengthUnit::Vh:
                    return text + "vh";
                case LengthUnit::Vmin:
                    return text + "vmin";
                case LengthUnit::Vmax:
                    return text + "vmax";
                case LengthUnit::Number:
                    return text;
                default:
                    return text + "px";
            }
        }
        return std::string();
    }
    std::string result;
    for (const CssValue& item : value.items) {
        std::string piece = ValueListText(item);
        if (piece.empty()) {
            continue;
        }
        if (!result.empty() && item.kind == ValueKind::Keyword && item.keyword == ",") {
            result += piece;
            continue;
        }
        if (!result.empty() && result.back() != ',') {
            result.push_back(' ');
        }
        result += piece;
    }
    return result;
}

/* ----------------------------------------------------------------
   应用层叠结果
   ---------------------------------------------------------------- */

/* 字号统一解析为像素。em 与百分比相对父元素字号；rem 相对根字号；
   pt/pc/in/cm/mm 按 96dpi 换算。其余相对单位在样式阶段无法换算，
   保留其数值（布局阶段可再按视口与字体度量处理）。 */
float ResolveFontSizePixels(const CssValue& value, float parent_font_size) {
    if (value.kind != ValueKind::Length) {
        return parent_font_size;
    }
    const float root_font_size = 16.0f;
    float number = value.length.value;
    switch (value.length.unit) {
    case LengthUnit::Px:
    case LengthUnit::Number:
        return number;
    case LengthUnit::Em:
        return parent_font_size * number;
    case LengthUnit::Percent:
        return parent_font_size * number / 100.0f;
    case LengthUnit::Rem:
        return root_font_size * number;
    case LengthUnit::Pt:
        return number * 96.0f / 72.0f;
    case LengthUnit::Pc:
        return number * 16.0f;
    case LengthUnit::In:
        return number * 96.0f;
    case LengthUnit::Cm:
        return number * 96.0f / 2.54f;
    case LengthUnit::Mm:
        return number * 96.0f / 25.4f;
    default:
        return number;
    }
}

/* border-spacing 分量在样式阶段换算：px 与无单位数字直接取值；em/ex/ch
   相对元素字号，rem 相对根字号；百分比在样式阶段没有内容宽度基准，保存为
   负值记号（绝对值为百分比数值），由布局阶段按表格内容宽度换算。
   视口单位在没有视口尺寸时保留 0。 */
float ResolveBorderSpacingComponent(const CssLength& length, float font_size) {
    switch (length.unit) {
    case LengthUnit::Px:
    case LengthUnit::Number:
        return length.value;
    case LengthUnit::Em:
        return length.value * font_size;
    case LengthUnit::Ex:
    case LengthUnit::Ch:
        return length.value * font_size * 0.5f;
    case LengthUnit::Rem:
        return length.value * 16.0f;
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
    case LengthUnit::Percent:
        return -length.value;
    default:
        return 0.0f;
    }
}

bool ApplyDisplay(const CssValue& value, Display* display) {
    if (value.kind != ValueKind::Keyword) {
        return false;
    }
    std::string keyword = LowerAsciiText(value.keyword);
    if (keyword == "none") {
        *display = Display::None;
    } else if (keyword == "block") {
        *display = Display::Block;
    } else if (keyword == "inline") {
        *display = Display::Inline;
    } else if (keyword == "inline-block") {
        *display = Display::InlineBlock;
    } else if (keyword == "list-item") {
        *display = Display::ListItem;
    } else if (keyword == "flex") {
        *display = Display::Flex;
    } else if (keyword == "inline-flex") {
        *display = Display::InlineFlex;
    } else if (keyword == "grid") {
        *display = Display::Grid;
    } else if (keyword == "inline-grid") {
        *display = Display::InlineGrid;
    } else {
        return false;
    }
    return true;
}

/* 边框线型关键字解析。groove/ridge/inset/outset 按实线绘制。 */
bool ApplyBorderStyleKeyword(const std::string& keyword, BorderStyle* output) {
    if (keyword == "none" || keyword == "hidden") {
        *output = BorderStyle::None;
        return true;
    }
    if (keyword == "dashed") {
        *output = BorderStyle::Dashed;
        return true;
    }
    if (keyword == "dotted") {
        *output = BorderStyle::Dotted;
        return true;
    }
    if (keyword == "double") {
        *output = BorderStyle::Double;
        return true;
    }
    if (IsBorderStyleKeyword(keyword)) {
        *output = BorderStyle::Solid;
        return true;
    }
    return false;
}

bool ApplyTextAlign(const CssValue& value, TextAlign* align) {
    if (value.kind != ValueKind::Keyword) {
        return false;
    }
    std::string keyword = LowerAsciiText(value.keyword);
    if (keyword == "left") {
        *align = TextAlign::Left;
    } else if (keyword == "center") {
        *align = TextAlign::Center;
    } else if (keyword == "right") {
        *align = TextAlign::Right;
    } else if (keyword == "justify") {
        *align = TextAlign::Justify;
    } else if (keyword == "start") {
        *align = TextAlign::Start;
    } else if (keyword == "end") {
        *align = TextAlign::End;
    } else {
        return false;
    }
    return true;
}

bool ApplyWhiteSpace(const CssValue& value, WhiteSpace* white_space) {
    if (value.kind != ValueKind::Keyword) {
        return false;
    }
    std::string keyword = LowerAsciiText(value.keyword);
    if (keyword == "normal") {
        *white_space = WhiteSpace::Normal;
    } else if (keyword == "pre") {
        *white_space = WhiteSpace::Pre;
    } else if (keyword == "nowrap") {
        *white_space = WhiteSpace::NoWrap;
    } else if (keyword == "pre-wrap") {
        *white_space = WhiteSpace::PreWrap;
    } else if (keyword == "pre-line") {
        *white_space = WhiteSpace::PreLine;
    } else {
        return false;
    }
    return true;
}

/* flex 容器与项目属性的关键字解析。 */
bool ApplyFlexDirection(const CssValue& value, FlexDirection* direction) {
    if (value.kind != ValueKind::Keyword) {
        return false;
    }
    std::string keyword = LowerAsciiText(value.keyword);
    if (keyword == "row") {
        *direction = FlexDirection::Row;
    } else if (keyword == "row-reverse") {
        *direction = FlexDirection::RowReverse;
    } else if (keyword == "column") {
        *direction = FlexDirection::Column;
    } else if (keyword == "column-reverse") {
        *direction = FlexDirection::ColumnReverse;
    } else {
        return false;
    }
    return true;
}

bool ApplyFlexWrap(const CssValue& value, FlexWrap* wrap) {
    if (value.kind != ValueKind::Keyword) {
        return false;
    }
    std::string keyword = LowerAsciiText(value.keyword);
    if (keyword == "nowrap") {
        *wrap = FlexWrap::NoWrap;
    } else if (keyword == "wrap") {
        *wrap = FlexWrap::Wrap;
    } else if (keyword == "wrap-reverse") {
        *wrap = FlexWrap::WrapReverse;
    } else {
        return false;
    }
    return true;
}

bool ApplyJustifyContent(const CssValue& value, JustifyContent* justify) {
    if (value.kind != ValueKind::Keyword) {
        return false;
    }
    std::string keyword = LowerAsciiText(value.keyword);
    if (keyword == "flex-start" || keyword == "start" || keyword == "left") {
        *justify = JustifyContent::FlexStart;
    } else if (keyword == "flex-end" || keyword == "end" || keyword == "right") {
        *justify = JustifyContent::FlexEnd;
    } else if (keyword == "center") {
        *justify = JustifyContent::Center;
    } else if (keyword == "space-between") {
        *justify = JustifyContent::SpaceBetween;
    } else if (keyword == "space-around") {
        *justify = JustifyContent::SpaceAround;
    } else if (keyword == "space-evenly") {
        *justify = JustifyContent::SpaceEvenly;
    } else {
        return false;
    }
    return true;
}

bool ApplyAlignItems(const CssValue& value, bool allow_auto, AlignItems* align) {
    if (value.kind != ValueKind::Keyword) {
        return false;
    }
    std::string keyword = LowerAsciiText(value.keyword);
    if (keyword == "auto") {
        if (!allow_auto) {
            return false;
        }
        *align = AlignItems::Auto;
    } else if (keyword == "stretch" || keyword == "normal") {
        *align = AlignItems::Stretch;
    } else if (keyword == "flex-start" || keyword == "start" || keyword == "self-start") {
        *align = AlignItems::FlexStart;
    } else if (keyword == "flex-end" || keyword == "end" || keyword == "self-end") {
        *align = AlignItems::FlexEnd;
    } else if (keyword == "center") {
        *align = AlignItems::Center;
    } else if (keyword == "baseline") {
        *align = AlignItems::Baseline;
    } else {
        return false;
    }
    return true;
}

/* 交叉轴多行对齐：align-content 与 grid 行分布共用同一组关键字。 */
bool ApplyAlignContent(const CssValue& value, AlignContent* align) {
    if (value.kind != ValueKind::Keyword) {
        return false;
    }
    std::string keyword = LowerAsciiText(value.keyword);
    if (keyword == "stretch" || keyword == "normal") {
        *align = AlignContent::Stretch;
    } else if (keyword == "flex-start" || keyword == "start") {
        *align = AlignContent::FlexStart;
    } else if (keyword == "flex-end" || keyword == "end") {
        *align = AlignContent::FlexEnd;
    } else if (keyword == "center") {
        *align = AlignContent::Center;
    } else if (keyword == "space-between") {
        *align = AlignContent::SpaceBetween;
    } else if (keyword == "space-around") {
        *align = AlignContent::SpaceAround;
    } else if (keyword == "space-evenly") {
        *align = AlignContent::SpaceEvenly;
    } else {
        return false;
    }
    return true;
}

bool ApplyWordBreak(const CssValue& value, WordBreak* word_break) {
    if (value.kind != ValueKind::Keyword) {
        return false;
    }
    std::string keyword = LowerAsciiText(value.keyword);
    if (keyword == "normal") {
        *word_break = WordBreak::Normal;
    } else if (keyword == "break-all") {
        *word_break = WordBreak::BreakAll;
    } else if (keyword == "keep-all") {
        *word_break = WordBreak::KeepAll;
    } else {
        return false;
    }
    return true;
}

bool ApplyListStylePosition(const CssValue& value, ListStylePosition* position) {
    if (value.kind != ValueKind::Keyword) {
        return false;
    }
    std::string keyword = LowerAsciiText(value.keyword);
    if (keyword == "outside") {
        *position = ListStylePosition::Outside;
    } else if (keyword == "inside") {
        *position = ListStylePosition::Inside;
    } else {
        return false;
    }
    return true;
}

bool ApplyObjectFit(const CssValue& value, ObjectFit* fit) {
    if (value.kind != ValueKind::Keyword) {
        return false;
    }
    std::string keyword = LowerAsciiText(value.keyword);
    if (keyword == "fill") {
        *fit = ObjectFit::Fill;
    } else if (keyword == "contain") {
        *fit = ObjectFit::Contain;
    } else if (keyword == "cover") {
        *fit = ObjectFit::Cover;
    } else if (keyword == "none") {
        *fit = ObjectFit::None;
    } else if (keyword == "scale-down") {
        *fit = ObjectFit::ScaleDown;
    } else {
        return false;
    }
    return true;
}

/* object-position：关键字或百分比，第一个分量为水平、第二个为垂直。
   像素位置需要元素尺寸，样式阶段无法换算，按居中处理。 */
void ApplyObjectPosition(const CssValue& value, ComputedStyle* style) {
    int component = 0;
    for (const CssValue& item : FlattenValue(value)) {
        float* target = component == 0 ? &style->object_position_x : &style->object_position_y;
        if (item.kind == ValueKind::Keyword) {
            const std::string keyword = LowerAsciiText(item.keyword);
            if (keyword == "left") {
                style->object_position_x = 0.0f;
            } else if (keyword == "right") {
                style->object_position_x = 1.0f;
            } else if (keyword == "top") {
                style->object_position_y = 0.0f;
            } else if (keyword == "bottom") {
                style->object_position_y = 1.0f;
            } else if (keyword == "center") {
                *target = 0.5f;
            }
        } else if (item.kind == ValueKind::Length &&
                   (item.length.unit == LengthUnit::Percent ||
                    item.length.unit == LengthUnit::Number)) {
            *target = item.length.value / 100.0f;
        }
        component += 1;
        if (component >= 2) {
            break;
        }
    }
}

/* 滤镜列表：只保留能在矩形绘制中表达的系数，未知函数忽略。
   同一函数出现多次时按从左到右的顺序叠加（本引擎保存单组系数，后出现的
   同名函数覆盖先前取值）。 */
bool ApplyFilter(const CssValue& value, ComputedStyle* style) {
    bool any = false;
    for (const CssValue& item : FlattenValue(value)) {
        if (item.kind == ValueKind::Keyword) {
            const std::string text = LowerAsciiText(item.keyword);
            if (text == "none") {
                style->filter_brightness = 1.0f;
                style->filter_contrast = 1.0f;
                style->filter_saturate = 1.0f;
                style->filter_grayscale = 0.0f;
                style->filter_sepia = 0.0f;
                style->filter_invert = 0.0f;
                style->has_filter = false;
                continue;
            }
            const std::vector<std::string> names = {
                "brightness(", "contrast(", "saturate(", "grayscale(", "sepia(", "invert("};
            for (const std::string& name : names) {
                if (text.compare(0, name.size(), name) != 0) {
                    continue;
                }
                std::string inner = text.substr(name.size());
                if (!inner.empty() && inner[inner.size() - 1] == ')') {
                    inner.erase(inner.size() - 1);
                }
                const bool percent = !inner.empty() && inner[inner.size() - 1] == '%';
                float number = static_cast<float>(ParseNumberLiteral(inner));
                if (percent) {
                    number /= 100.0f;
                }
                if (name == "brightness(") {
                    style->filter_brightness = number;
                } else if (name == "contrast(") {
                    style->filter_contrast = number;
                } else if (name == "saturate(") {
                    style->filter_saturate = number;
                } else if (name == "grayscale(") {
                    style->filter_grayscale = number;
                } else if (name == "sepia(") {
                    style->filter_sepia = number;
                } else {
                    style->filter_invert = number;
                }
                any = true;
                break;
            }
        }
    }
    if (any) {
        style->has_filter = true;
    }
    return any;
}

/* clip-path: inset(上 右 下 左)：取值顺序与 margin 简写一致；百分比在绘制
   阶段按盒子宽高解析。其它路径函数暂不支持，保持未裁剪。 */
void ApplyClipPath(const CssValue& value, ComputedStyle* style) {
    std::string text;
    if (value.kind == ValueKind::Keyword) {
        text = LowerAsciiText(value.keyword);
    } else {
        text = LowerAsciiText(ValueListText(value));
    }
    if (text == "none" || text.empty()) {
        style->has_clip_path = false;
        return;
    }
    if (text.compare(0, 6, "inset(") != 0 || text.back() != ')') {
        return;
    }
    std::string inner = text.substr(6, text.size() - 7);
    std::vector<CssLength> lengths;
    std::string token;
    for (size_t index = 0; index <= inner.size(); ++index) {
        if (index == inner.size() || IsSpaceCharacter(inner[index])) {
            if (token.empty()) {
                continue;
            }
            std::vector<CssValue> parsed;
            ParseValueComponents(token, &parsed);
            if (parsed.empty() || parsed[0].kind != ValueKind::Length) {
                return;
            }
            CssLength length = parsed[0].length;
            if (length.unit == LengthUnit::Number) {
                length.unit = LengthUnit::Px;
            }
            lengths.push_back(length);
            token.clear();
            continue;
        }
        token.push_back(inner[index]);
    }
    if (lengths.empty() || lengths.size() > 4) {
        return;
    }
    CssLength top = lengths[0];
    CssLength right = lengths.size() > 1 ? lengths[1] : top;
    CssLength bottom = lengths.size() > 2 ? lengths[2] : top;
    CssLength left = lengths.size() > 3 ? lengths[3] : right;
    style->clip_inset_top = top;
    style->clip_inset_right = right;
    style->clip_inset_bottom = bottom;
    style->clip_inset_left = left;
    style->has_clip_path = true;
}

/* 网格线位置：整数或 auto；span 形式在样式阶段只记录跨度（由调用方处理）。 */
bool ParseGridLineNumber(const std::string& text, int* line) {
    const std::string trimmed = TrimAsciiSpace(text);
    if (trimmed.empty() || LowerAsciiText(trimmed) == "auto") {
        *line = 0;
        return true;
    }
    char* end = nullptr;
    const long parsed = std::strtol(trimmed.c_str(), &end, 10);
    if (end == trimmed.c_str() || end == nullptr || *end != '\0') {
        return false;
    }
    // 显式网格线编号范围为 -1000..1000，0 不合法（视为 auto）。
    if (parsed < -1000 || parsed > 1000) {
        return false;
    }
    *line = static_cast<int>(parsed);
    return true;
}

/* grid-column / grid-row 的取值解析。返回值表示是否解析成功；
   span_count 输出 span 形式的跨度量，start/end 输出显式起止线。 */
bool ParseGridPlacement(const std::string& text, int* span_count, int* start, int* end) {
    const std::string lowered = LowerAsciiText(text);
    std::vector<std::string> tokens;
    std::string current;
    for (size_t index = 0; index < lowered.size(); ++index) {
        const char character = lowered[index];
        if (IsSpaceCharacter(character) || character == '/') {
            if (!current.empty()) {
                tokens.push_back(current);
                current.clear();
            }
            continue;
        }
        current.push_back(character);
    }
    if (!current.empty()) {
        tokens.push_back(current);
    }
    if (tokens.empty()) {
        return false;
    }
    // 纯 span N 形式。
    if (tokens.size() == 2 && tokens[0] == "span") {
        const int count = static_cast<int>(ParseNumberLiteral(tokens[1]));
        if (count <= 0) {
            return false;
        }
        *span_count = count;
        return true;
    }
    // 起点线 / span N 形式（grid-column: 1 / span 2）。
    if (tokens.size() >= 3 && tokens[1] == "span") {
        int start_line = 0;
        if (!ParseGridLineNumber(tokens[0], &start_line)) {
            return false;
        }
        const int count = static_cast<int>(ParseNumberLiteral(tokens[2]));
        if (count <= 0) {
            return false;
        }
        *start = start_line;
        *span_count = count;
        return true;
    }
    int first = 0;
    int second = 0;
    if (!ParseGridLineNumber(tokens[0], &first)) {
        return false;
    }
    if (tokens.size() > 1 && !ParseGridLineNumber(tokens[1], &second)) {
        return false;
    }
    *start = first;
    *end = second;
    return true;
}

/* grid-template-areas：引号包裹的区域名按行拼接，同一行的名称以空格分隔，
   连续的句点表示未命名格。 */
void ParseGridTemplateAreas(const CssValue& value, ComputedStyle* style) {
    std::vector<std::vector<std::string>> rows;
    // 值序列中的字符串项即一行；同一行的名字允许被空白拆分到多个字符串中。
    std::vector<std::string> row_tokens;
    auto flush_row = [&]() {
        if (row_tokens.empty()) {
            return;
        }
        std::vector<std::string> row;
        for (const std::string& token : row_tokens) {
            if (token == "." || token.empty()) {
                row.push_back(std::string());
            } else {
                row.push_back(token);
            }
        }
        rows.push_back(std::move(row));
        row_tokens.clear();
    };
    const std::vector<CssValue> items = FlattenValue(value);
    for (const CssValue& item : items) {
        if (item.kind == ValueKind::String) {
            flush_row();
            std::string line = item.string_value;
            std::string token;
            for (size_t index = 0; index <= line.size(); ++index) {
                if (index == line.size() || IsSpaceCharacter(line[index])) {
                    if (!token.empty()) {
                        row_tokens.push_back(token);
                        token.clear();
                    }
                    continue;
                }
                token.push_back(line[index]);
            }
            flush_row();
        }
    }
    if (!rows.empty()) {
        style->grid_template_areas = std::move(rows);
    }
}

/* content 属性：拼接字符串与 attr() 引用；counter()/open-quote 等无法静态
   求值的函数忽略。返回是否得到非空内容。 */
bool ExtractPseudoContent(const CssValue& value, const Element& element, std::string* output) {
    const std::vector<CssValue> items = FlattenValue(value);
    std::string text;
    bool declared = false;
    for (const CssValue& item : items) {
        if (item.kind == ValueKind::String) {
            text += item.string_value;
            declared = true;
            continue;
        }
        if (item.kind != ValueKind::Keyword) {
            continue;
        }
        const std::string lowered = LowerAsciiText(item.keyword);
        if (lowered == "none" || lowered == "normal") {
            continue;
        }
        if (lowered.compare(0, 5, "attr(") == 0 && lowered.size() > 6) {
            std::string name = item.keyword.substr(5);
            if (!name.empty() && name[name.size() - 1] == ')') {
                name.erase(name.size() - 1);
            }
            name = TrimAsciiSpace(name);
            if (!name.empty()) {
                text += element.GetAttribute(name);
                declared = true;
            }
            continue;
        }
        if (lowered == "open-quote" || lowered == "close-quote") {
            // 未定义 quotes 时使用 CSS 初始引号（左右双引号 U+201C/U+201D）。
            text += lowered == "open-quote" ? "\xE2\x80\x9C" : "\xE2\x80\x9D";
            declared = true;
            continue;
        }
    }
    if (declared) {
        *output = text;
    }
    return declared;
}

/* font-family：保留完整回退链，族名以逗号分隔，由文本服务依次尝试创建。 */
std::string ExtractFontFamily(const CssValue& value) {
    if (value.kind == ValueKind::String) {
        return value.string_value;
    }
    if (value.kind == ValueKind::Keyword) {
        return value.keyword;
    }
    if (value.kind == ValueKind::List) {
        std::string result;
        for (const CssValue& item : value.items) {
            std::string piece;
            if (item.kind == ValueKind::String) {
                piece = item.string_value;
            } else if (item.kind == ValueKind::Keyword && item.keyword != ",") {
                piece = item.keyword;
            }
            if (piece.empty()) {
                continue;
            }
            if (!result.empty()) {
                result += ", ";
            }
            result += piece;
        }
        return result;
    }
    return std::string();
}

void AssignEdgeLength(EdgeLengths* edges, const std::string& side, const CssLength& length) {
    if (side == "top") {
        edges->top = length;
    } else if (side == "right") {
        edges->right = length;
    } else if (side == "bottom") {
        edges->bottom = length;
    } else if (side == "left") {
        edges->left = length;
    }
}

void AssignBorderWidth(ComputedStyle* style, const std::string& side, const CssLength& length) {
    if (side == "top") {
        style->border_width.top = length;
    } else if (side == "right") {
        style->border_width.right = length;
    } else if (side == "bottom") {
        style->border_width.bottom = length;
    } else if (side == "left") {
        style->border_width.left = length;
    }
}

void AssignBorderColor(ComputedStyle* style, const std::string& side, const Color& color) {
    if (side == "top") {
        style->border_color_top = color;
    } else if (side == "right") {
        style->border_color_right = color;
    } else if (side == "bottom") {
        style->border_color_bottom = color;
    } else if (side == "left") {
        style->border_color_left = color;
    }
}

/* 把 "margin-top" 之类的属性名拆成前缀与边名。 */
bool SplitSideProperty(const std::string& property, const std::string& prefix,
                       std::string* side) {
    if (property.size() <= prefix.size() + 1 ||
        property.compare(0, prefix.size(), prefix) != 0) {
        return false;
    }
    *side = property.substr(prefix.size());
    return *side == "top" || *side == "right" || *side == "bottom" || *side == "left";
}

/* 圆角长写属性的单个半径分量。百分比（含 calc 的百分比部分）依赖边框盒尺寸，
   保留到绘制阶段解析；其余单位在此换算为像素。 */
void AssignCornerRadiusLength(const CssLength& length, float font_size,
                              const LengthEvalContext& context, float* value, float* offset,
                              bool* percent) {
    *offset = 0.0f;
    *percent = false;
    if (length.unit == LengthUnit::Percent) {
        *percent = true;
        *value = length.value;
        return;
    }
    if (length.unit == LengthUnit::CalcPercent) {
        *percent = true;
        *value = length.value;
        *offset = length.offset;
        return;
    }
    *value = ResolvePixelsForStyle(length, font_size, context.root_font_size,
                                   context.viewport_width, context.viewport_height);
}

/* SVG fill/stroke：接受颜色或 none 关键字。url(#id) 引用由绘制阶段按
   呈现属性解析，样式表中出现时按无效声明忽略。 */
bool ApplySvgPaintValue(const CssValue& value, bool* is_none, Color* color) {
    if (value.kind == ValueKind::Color) {
        *is_none = false;
        *color = value.color.color;
        return true;
    }
    if (value.kind == ValueKind::Keyword && LowerAsciiText(value.keyword) == "none") {
        *is_none = true;
        return true;
    }
    return false;
}

/* SVG 长度换算为像素：无单位数字按用户单位（等同像素）处理。 */
bool ResolveSvgLengthValue(const CssValue& value, float font_size,
                           const LengthEvalContext& context, float* output) {
    if (value.kind != ValueKind::Length) {
        return false;
    }
    CssLength length = value.length;
    if (length.unit == LengthUnit::Number) {
        length.unit = LengthUnit::Px;
    }
    if (length.IsPercentBased()) {
        return false;
    }
    *output = ResolvePixelsForStyle(length, font_size, context.root_font_size,
                                    context.viewport_width, context.viewport_height);
    return true;
}

void ApplyWinners(const WinnerMap& winners, float parent_font_size,
                  const LengthEvalContext& context, ComputedStyle* style) {
    for (const auto& entry : winners) {
        const std::string& property = entry.first;
        const CssValue& value = entry.second.value;

        if (property == "display") {
            ApplyDisplay(value, &style->display);
            continue;
        }
        if (property == "width" || property == "height") {
            CssLength length{0.0f, LengthUnit::Auto};
            if (TryGetEdgeLength(value, &length)) {
                if (property == "width") {
                    style->width = length;
                } else {
                    style->height = length;
                }
            }
            continue;
        }
        if (property == "box-sizing") {
            if (value.kind == ValueKind::Keyword) {
                const std::string keyword = LowerAsciiText(value.keyword);
                if (keyword == "border-box") {
                    style->border_box = true;
                } else if (keyword == "content-box") {
                    style->border_box = false;
                }
            }
            continue;
        }
        if (property == "min-width" || property == "max-width" ||
            property == "min-height" || property == "max-height") {
            CssLength length{0.0f, LengthUnit::Auto};
            bool valid = false;
            if (value.kind == ValueKind::Keyword &&
                LowerAsciiText(value.keyword) == "none") {
                valid = true;
            } else if (TryGetEdgeLength(value, &length) &&
                       length.unit != LengthUnit::Auto) {
                valid = true;
            }
            if (!valid) {
                continue;
            }
            if (property == "min-width") {
                style->min_width = length;
            } else if (property == "max-width") {
                style->max_width = length;
            } else if (property == "min-height") {
                style->min_height = length;
            } else {
                style->max_height = length;
            }
            continue;
        }
        std::string side;
        if (SplitSideProperty(property, "margin-", &side)) {
            CssLength length;
            if (TryGetEdgeLength(value, &length)) {
                AssignEdgeLength(&style->margin, side, length);
            }
            continue;
        }
        if (SplitSideProperty(property, "padding-", &side)) {
            CssLength length;
            if (TryGetEdgeLength(value, &length)) {
                if (length.unit == LengthUnit::Auto) {
                    length = CssLength{0.0f, LengthUnit::Px};
                }
                AssignEdgeLength(&style->padding, side, length);
            }
            continue;
        }
        if (property.size() > std::string("border-").size() &&
            property.compare(0, std::string("border-").size(), "border-") == 0 &&
            property.size() > std::string("-width").size() &&
            property.compare(property.size() - std::string("-width").size(),
                             std::string("-width").size(), "-width") == 0) {
            std::string middle = property.substr(std::string("border-").size(),
                                                 property.size() - std::string("border-").size() -
                                                     std::string("-width").size());
            CssLength length;
            float keyword_width = 0.0f;
            bool valid = TryGetEdgeLength(value, &length);
            if (!valid && value.kind == ValueKind::Keyword &&
                BorderWidthKeyword(LowerAsciiText(value.keyword), &keyword_width)) {
                length = CssLength{keyword_width, LengthUnit::Px};
                valid = true;
            }
            if (valid) {
                AssignBorderWidth(style, middle, length);
            }
            continue;
        }
        if (property.size() > std::string("border-").size() &&
            property.compare(0, std::string("border-").size(), "border-") == 0 &&
            property.size() > std::string("-color").size() &&
            property.compare(property.size() - std::string("-color").size(),
                             std::string("-color").size(), "-color") == 0) {
            std::string middle = property.substr(std::string("border-").size(),
                                                 property.size() - std::string("border-").size() -
                                                     std::string("-color").size());
            if (value.kind == ValueKind::Color) {
                AssignBorderColor(style, middle, value.color.color);
            }
            continue;
        }
        if (property == "border-top-left-radius" || property == "border-top-right-radius" ||
            property == "border-bottom-right-radius" ||
            property == "border-bottom-left-radius") {
            // 长写属性接受 1 或 2 个长度：水平半径，以及可选的垂直半径。
            // 百分比保留原值，由绘制阶段按边框盒宽、高解析。
            const std::vector<CssValue> parts = FlattenValue(value);
            if (parts.size() >= 1 && parts.size() <= 2 && parts[0].kind == ValueKind::Length &&
                (parts.size() == 1 || parts[1].kind == ValueKind::Length)) {
                CornerRadius radius;
                AssignCornerRadiusLength(parts[0].length, style->font_size, context,
                                         &radius.horizontal, &radius.horizontal_offset,
                                         &radius.horizontal_percent);
                if (parts.size() == 2) {
                    AssignCornerRadiusLength(parts[1].length, style->font_size, context,
                                             &radius.vertical, &radius.vertical_offset,
                                             &radius.vertical_percent);
                } else {
                    radius.vertical = radius.horizontal;
                    radius.vertical_offset = radius.horizontal_offset;
                    radius.vertical_percent = radius.horizontal_percent;
                }
                if (property == "border-top-left-radius") {
                    style->border_radius_top_left = radius;
                } else if (property == "border-top-right-radius") {
                    style->border_radius_top_right = radius;
                } else if (property == "border-bottom-right-radius") {
                    style->border_radius_bottom_right = radius;
                } else {
                    style->border_radius_bottom_left = radius;
                }
            }
            continue;
        }
        if (property == "border-top-style" || property == "border-right-style" ||
            property == "border-bottom-style" || property == "border-left-style") {
            if (value.kind == ValueKind::Keyword) {
                BorderStyle line_style = BorderStyle::Solid;
                if (ApplyBorderStyleKeyword(LowerAsciiText(value.keyword), &line_style)) {
                    if (property == "border-top-style") {
                        style->border_style_top = line_style;
                    } else if (property == "border-right-style") {
                        style->border_style_right = line_style;
                    } else if (property == "border-bottom-style") {
                        style->border_style_bottom = line_style;
                    } else {
                        style->border_style_left = line_style;
                    }
                }
            }
            continue;
        }
        if (property == "border-style") {
            // 简写已在 ExpandDeclaration 中展开为四边 longhand，这里兜底处理单值。
            if (value.kind == ValueKind::Keyword) {
                BorderStyle line_style = BorderStyle::Solid;
                if (ApplyBorderStyleKeyword(LowerAsciiText(value.keyword), &line_style)) {
                    style->border_style_top = line_style;
                    style->border_style_right = line_style;
                    style->border_style_bottom = line_style;
                    style->border_style_left = line_style;
                }
            }
            continue;
        }
        if (property == "border-spacing-x" || property == "border-spacing-y") {
            if (value.kind == ValueKind::Length) {
                const float pixels = ResolveBorderSpacingComponent(value.length, style->font_size);
                if (property == "border-spacing-x") {
                    style->border_spacing_x = pixels;
                } else {
                    style->border_spacing_y = pixels;
                }
            }
            continue;
        }
        if (property == "border-collapse") {
            // 缺省或未知取值按 separate（计算样式的初始值即为 separate）。
            style->border_collapse = BorderCollapse::Separate;
            if (value.kind == ValueKind::Keyword &&
                LowerAsciiText(value.keyword) == "collapse") {
                style->border_collapse = BorderCollapse::Collapse;
            }
            continue;
        }
        if (property == "font-size") {
            style->font_size = ResolveFontSizePixels(value, parent_font_size);
            style->has_svg_font_size = true;
            continue;
        }
        if (property == "font-family") {
            style->font_family = ExtractFontFamily(value);
            style->has_svg_font_family = true;
            continue;
        }
        if (property == "font-weight") {
            if (value.kind == ValueKind::Length && value.length.IsNumber()) {
                style->font_weight = static_cast<int>(value.length.value);
                style->has_svg_font_weight = true;
            } else if (value.kind == ValueKind::Keyword) {
                int weight = 0;
                if (IsFontWeightKeyword(LowerAsciiText(value.keyword), &weight)) {
                    style->font_weight = weight;
                    style->has_svg_font_weight = true;
                }
            }
            continue;
        }
        if (property == "font-style") {
            if (value.kind == ValueKind::Keyword) {
                std::string keyword = LowerAsciiText(value.keyword);
                if (keyword == "normal") {
                    style->italic = false;
                    style->has_svg_font_style = true;
                } else if (keyword == "italic" || keyword == "oblique") {
                    style->italic = true;
                    style->has_svg_font_style = true;
                }
            }
            continue;
        }
        if (property == "color") {
            if (value.kind == ValueKind::Color) {
                style->color = value.color.color;
            }
            continue;
        }
        if (property == "text-fill-color" || property == "-webkit-text-fill-color") {
            // 文字填充色：与 color 等价，常用于把文字设为透明以显示渐变。
            if (value.kind == ValueKind::Color) {
                style->color = value.color.color;
            }
            continue;
        }
        if (property == "fill") {
            if (ApplySvgPaintValue(value, &style->svg_fill_none, &style->svg_fill)) {
                style->has_svg_fill = true;
            }
            continue;
        }
        if (property == "fill-opacity") {
            if (value.kind == ValueKind::Length && value.length.IsNumber()) {
                float opacity = value.length.value;
                style->svg_fill_opacity = opacity < 0.0f ? 0.0f : (opacity > 1.0f ? 1.0f : opacity);
                style->has_svg_fill_opacity = true;
            }
            continue;
        }
        if (property == "stroke") {
            if (ApplySvgPaintValue(value, &style->svg_stroke_none, &style->svg_stroke)) {
                style->has_svg_stroke = true;
            }
            continue;
        }
        if (property == "stroke-opacity") {
            if (value.kind == ValueKind::Length && value.length.IsNumber()) {
                float opacity = value.length.value;
                style->svg_stroke_opacity =
                    opacity < 0.0f ? 0.0f : (opacity > 1.0f ? 1.0f : opacity);
                style->has_svg_stroke_opacity = true;
            }
            continue;
        }
        if (property == "stroke-width") {
            float width = 0.0f;
            if (ResolveSvgLengthValue(value, style->font_size, context, &width)) {
                style->svg_stroke_width = width < 0.0f ? 0.0f : width;
                style->has_svg_stroke_width = true;
            }
            continue;
        }
        if (property == "stroke-linecap") {
            if (value.kind == ValueKind::Keyword) {
                const std::string keyword = LowerAsciiText(value.keyword);
                if (keyword == "butt") {
                    style->svg_line_cap = SvgLineCap::Butt;
                    style->has_svg_line_cap = true;
                } else if (keyword == "round") {
                    style->svg_line_cap = SvgLineCap::Round;
                    style->has_svg_line_cap = true;
                } else if (keyword == "square") {
                    style->svg_line_cap = SvgLineCap::Square;
                    style->has_svg_line_cap = true;
                }
            }
            continue;
        }
        if (property == "stroke-linejoin") {
            if (value.kind == ValueKind::Keyword) {
                const std::string keyword = LowerAsciiText(value.keyword);
                if (keyword == "miter") {
                    style->svg_line_join = SvgLineJoin::Miter;
                    style->has_svg_line_join = true;
                } else if (keyword == "round") {
                    style->svg_line_join = SvgLineJoin::Round;
                    style->has_svg_line_join = true;
                } else if (keyword == "bevel") {
                    style->svg_line_join = SvgLineJoin::Bevel;
                    style->has_svg_line_join = true;
                }
            }
            continue;
        }
        if (property == "stroke-dasharray") {
            if (value.kind == ValueKind::Keyword && LowerAsciiText(value.keyword) == "none") {
                style->svg_dash_array.clear();
                style->has_svg_dash_array = true;
                continue;
            }
            std::vector<CssValue> items = FlattenValue(value);
            std::vector<float> dashes;
            bool valid = !items.empty();
            for (const CssValue& item : items) {
                float dash = 0.0f;
                if (!ResolveSvgLengthValue(item, style->font_size, context, &dash) || dash < 0.0f) {
                    valid = false;
                    break;
                }
                dashes.push_back(dash);
            }
            if (valid) {
                style->svg_dash_array = std::move(dashes);
                style->has_svg_dash_array = true;
            }
            continue;
        }
        if (property == "stroke-dashoffset") {
            float offset = 0.0f;
            if (ResolveSvgLengthValue(value, style->font_size, context, &offset)) {
                style->svg_dash_offset = offset;
                style->has_svg_dash_offset = true;
            }
            continue;
        }
        if (property == "text-anchor") {
            if (value.kind == ValueKind::Keyword) {
                const std::string keyword = LowerAsciiText(value.keyword);
                if (keyword == "start") {
                    style->svg_text_anchor = SvgTextAnchor::Start;
                    style->has_svg_text_anchor = true;
                } else if (keyword == "middle") {
                    style->svg_text_anchor = SvgTextAnchor::Middle;
                    style->has_svg_text_anchor = true;
                } else if (keyword == "end") {
                    style->svg_text_anchor = SvgTextAnchor::End;
                    style->has_svg_text_anchor = true;
                }
            }
            continue;
        }
        if (property == "dominant-baseline") {
            if (value.kind == ValueKind::Keyword) {
                const std::string keyword = LowerAsciiText(value.keyword);
                if (keyword == "alphabetic" || keyword == "auto") {
                    style->svg_baseline = SvgBaseline::Alphabetic;
                    style->has_svg_baseline = true;
                } else if (keyword == "central") {
                    style->svg_baseline = SvgBaseline::Central;
                    style->has_svg_baseline = true;
                } else if (keyword == "middle") {
                    style->svg_baseline = SvgBaseline::Middle;
                    style->has_svg_baseline = true;
                } else if (keyword == "hanging") {
                    style->svg_baseline = SvgBaseline::Hanging;
                    style->has_svg_baseline = true;
                }
            }
            continue;
        }
        if (property == "paint-order") {
            std::vector<CssValue> items = FlattenValue(value);
            style->has_svg_paint_order = true;
            style->svg_stroke_first =
                !items.empty() && items[0].kind == ValueKind::Keyword &&
                LowerAsciiText(items[0].keyword) == "stroke";
            continue;
        }
        if (property == "pointer-events") {
            if (value.kind == ValueKind::Keyword) {
                const std::string keyword = LowerAsciiText(value.keyword);
                SvgPointerEvents pointer_events = SvgPointerEvents::Auto;
                bool known = true;
                if (keyword == "none") {
                    pointer_events = SvgPointerEvents::None;
                } else if (keyword == "auto") {
                    pointer_events = SvgPointerEvents::Auto;
                } else if (keyword == "visiblepainted") {
                    pointer_events = SvgPointerEvents::VisiblePainted;
                } else if (keyword == "visiblefill") {
                    pointer_events = SvgPointerEvents::VisibleFill;
                } else if (keyword == "visiblestroke") {
                    pointer_events = SvgPointerEvents::VisibleStroke;
                } else if (keyword == "fill") {
                    pointer_events = SvgPointerEvents::Fill;
                } else if (keyword == "stroke") {
                    pointer_events = SvgPointerEvents::Stroke;
                } else if (keyword == "all") {
                    pointer_events = SvgPointerEvents::All;
                } else if (keyword == "bounding-box") {
                    pointer_events = SvgPointerEvents::BoundingBox;
                } else {
                    known = false;
                }
                if (known) {
                    style->svg_pointer_events = pointer_events;
                    style->has_svg_pointer_events = true;
                }
            }
            continue;
        }
        if (property == "background-color") {
            if (value.kind == ValueKind::Color) {
                style->background_color = value.color.color;
            }
            continue;
        }
        if (property == "line-height") {
            if (value.kind == ValueKind::Length) {
                /* 无单位数字保留 Number 单位，表示字号倍数。 */
                style->line_height = value.length;
            } else if (value.kind == ValueKind::Keyword &&
                       LowerAsciiText(value.keyword) == "normal") {
                style->line_height = CssLength{0.0f, LengthUnit::Auto};
            }
            continue;
        }
        if (property == "text-align") {
            ApplyTextAlign(value, &style->text_align);
            continue;
        }
        if (property == "white-space") {
            ApplyWhiteSpace(value, &style->white_space);
            continue;
        }
        if (property == "letter-spacing") {
            if (value.kind == ValueKind::Length) {
                style->letter_spacing = value.length.value;
            } else if (value.kind == ValueKind::Keyword &&
                       LowerAsciiText(value.keyword) == "normal") {
                style->letter_spacing = 0.0f;
            }
            continue;
        }
        if (property == "text-indent") {
            CssLength length{0.0f, LengthUnit::Px};
            if (TryGetEdgeLength(value, &length)) {
                style->text_indent = length;
            }
            continue;
        }
        if (property == "text-overflow") {
            if (value.kind == ValueKind::Keyword) {
                const std::string keyword = LowerAsciiText(value.keyword);
                style->text_overflow_ellipsis = keyword == "ellipsis";
            }
            continue;
        }
        if (property == "word-break") {
            ApplyWordBreak(value, &style->word_break);
            continue;
        }
        if (property == "overflow-wrap" || property == "word-wrap") {
            if (value.kind == ValueKind::Keyword) {
                const std::string keyword = LowerAsciiText(value.keyword);
                style->overflow_wrap_anywhere = keyword == "break-word" || keyword == "anywhere";
            }
            continue;
        }
        if (property == "list-style-position") {
            ApplyListStylePosition(value, &style->list_style_position);
            continue;
        }
        if (property == "order") {
            if (value.kind == ValueKind::Length && value.length.IsNumber()) {
                style->order = static_cast<int>(value.length.value);
            }
            continue;
        }
        if (property == "align-content") {
            ApplyAlignContent(value, &style->align_content);
            continue;
        }
        if (property == "object-fit") {
            ApplyObjectFit(value, &style->object_fit);
            continue;
        }
        if (property == "object-position") {
            ApplyObjectPosition(value, style);
            continue;
        }
        if (property == "filter" || property == "-webkit-filter") {
            ApplyFilter(value, style);
            continue;
        }
        if (property == "clip-path") {
            ApplyClipPath(value, style);
            continue;
        }
        if (property == "grid-template-areas") {
            ParseGridTemplateAreas(value, style);
            continue;
        }
        if (property == "text-decoration" || property == "text-decoration-line") {
            std::vector<CssValue> items = FlattenValue(value);
            for (const CssValue& item : items) {
                if (item.kind != ValueKind::Keyword) {
                    continue;
                }
                std::string keyword = LowerAsciiText(item.keyword);
                if (keyword == "underline") {
                    style->underline = true;
                } else if (keyword == "none") {
                    style->underline = false;
                }
            }
            continue;
        }
        if (property == "list-style-type") {
            if (value.kind == ValueKind::Keyword) {
                ListStyleType type = ListStyleType::None;
                if (ListStyleKeyword(LowerAsciiText(value.keyword), &type)) {
                    style->list_style_type = type;
                }
            }
            continue;
        }
        if (property == "flex-direction") {
            ApplyFlexDirection(value, &style->flex_direction);
            continue;
        }
        if (property == "flex-wrap") {
            ApplyFlexWrap(value, &style->flex_wrap);
            continue;
        }
        if (property == "justify-content") {
            ApplyJustifyContent(value, &style->justify_content);
            continue;
        }
        if (property == "align-items") {
            ApplyAlignItems(value, false, &style->align_items);
            continue;
        }
        if (property == "align-self") {
            ApplyAlignItems(value, true, &style->align_self);
            continue;
        }
        if (property == "flex-grow" || property == "flex-shrink") {
            if (value.kind == ValueKind::Length && value.length.IsNumber()) {
                const float factor = value.length.value < 0.0f ? 0.0f : value.length.value;
                if (property == "flex-grow") {
                    style->flex_grow = factor;
                } else {
                    style->flex_shrink = factor;
                }
            }
            continue;
        }
        if (property == "flex-basis") {
            CssLength length{0.0f, LengthUnit::Auto};
            if (TryGetEdgeLength(value, &length)) {
                style->flex_basis = length;
            }
            continue;
        }
        if (property == "row-gap" || property == "column-gap") {
            if (value.kind == ValueKind::Length && !value.length.IsNumber()) {
                if (property == "row-gap") {
                    style->row_gap = value.length;
                } else {
                    style->column_gap = value.length;
                }
            } else if (value.kind == ValueKind::Keyword &&
                       LowerAsciiText(value.keyword) == "normal") {
                if (property == "row-gap") {
                    style->row_gap = CssLength{0.0f, LengthUnit::Px};
                } else {
                    style->column_gap = CssLength{0.0f, LengthUnit::Px};
                }
            }
            continue;
        }
        if (property == "grid-template-columns" || property == "grid-template-rows") {
            // 值可能是单个关键字，也可能是长度/关键字序列，统一还原为文本后解析轨道。
            const std::string text = ValueListText(value);
            if (!text.empty()) {
                std::vector<GridTrack> tracks;
                if (LowerAsciiText(text) != "none" && ParseGridTracks(text, 0, &tracks)) {
                    if (property == "grid-template-columns") {
                        style->grid_columns = tracks;
                    } else {
                        style->grid_rows = tracks;
                    }
                }
            }
            continue;
        }
        if (property == "grid-column" || property == "grid-row") {
            int span_count = 0;
            int start = 0;
            int end = 0;
            if (ParseGridPlacement(ValueListText(value), &span_count, &start, &end)) {
                if (property == "grid-column") {
                    if (span_count > 0) {
                        style->grid_column_span = span_count;
                    }
                    style->grid_column_start = start;
                    style->grid_column_end = end;
                } else {
                    if (span_count > 0) {
                        style->grid_row_span = span_count;
                    }
                    style->grid_row_start = start;
                    style->grid_row_end = end;
                }
            }
            continue;
        }
        if (property == "grid-area") {
            // 单标识符形式引用 grid-template-areas 中的区域名；四值形式
            // （行起/列起/行止/列止）解析为显式网格线。
            const std::vector<CssValue> items = FlattenValue(value);
            if (items.size() == 1 && items[0].kind == ValueKind::Keyword) {
                const std::string name = LowerAsciiText(items[0].keyword);
                if (name != "auto") {
                    style->grid_area_name = name;
                }
            }
            continue;
        }
        if (property == "position") {
            if (value.kind == ValueKind::Keyword) {
                const std::string keyword = LowerAsciiText(value.keyword);
                if (keyword == "static") {
                    style->position = Position::Static;
                } else if (keyword == "relative") {
                    style->position = Position::Relative;
                } else if (keyword == "absolute") {
                    style->position = Position::Absolute;
                } else if (keyword == "fixed") {
                    style->position = Position::Fixed;
                } else if (keyword == "sticky") {
                    style->position = Position::Sticky;
                }
            }
            continue;
        }
        if (property == "top" || property == "right" || property == "bottom" ||
            property == "left") {
            CssLength length{0.0f, LengthUnit::Auto};
            if (TryGetEdgeLength(value, &length)) {
                if (property == "top") {
                    style->position_top = length;
                } else if (property == "right") {
                    style->position_right = length;
                } else if (property == "bottom") {
                    style->position_bottom = length;
                } else {
                    style->position_left = length;
                }
            }
            continue;
        }
        if (property == "float") {
            if (value.kind == ValueKind::Keyword) {
                const std::string keyword = LowerAsciiText(value.keyword);
                if (keyword == "left") {
                    style->float_side = FloatSide::Left;
                } else if (keyword == "right") {
                    style->float_side = FloatSide::Right;
                } else if (keyword == "none") {
                    style->float_side = FloatSide::None;
                }
            }
            continue;
        }
        if (property == "clear") {
            if (value.kind == ValueKind::Keyword) {
                const std::string keyword = LowerAsciiText(value.keyword);
                if (keyword == "left") {
                    style->clear_side = ClearSide::Left;
                } else if (keyword == "right") {
                    style->clear_side = ClearSide::Right;
                } else if (keyword == "both") {
                    style->clear_side = ClearSide::Both;
                } else if (keyword == "none") {
                    style->clear_side = ClearSide::None;
                }
            }
            continue;
        }
        if (property == "z-index") {
            if (value.kind == ValueKind::Length && value.length.IsNumber()) {
                style->z_index = static_cast<int>(value.length.value);
                style->z_index_auto = false;
            } else if (value.kind == ValueKind::Keyword &&
                       LowerAsciiText(value.keyword) == "auto") {
                style->z_index = 0;
                style->z_index_auto = true;
            }
            continue;
        }
        if (property == "opacity") {
            if (value.kind == ValueKind::Length && value.length.IsNumber()) {
                float opacity = value.length.value;
                if (opacity < 0.0f) {
                    opacity = 0.0f;
                }
                if (opacity > 1.0f) {
                    opacity = 1.0f;
                }
                style->opacity = opacity;
            }
            continue;
        }
        if (property == "background-image") {
            if (value.kind == ValueKind::Keyword) {
                BackgroundImage image;
                if (LowerAsciiText(value.keyword) == "none") {
                    style->background_image = BackgroundImage();
                    style->background_image_url.clear();
                } else if (ParseGradientText(value.keyword, &image)) {
                    style->background_image = image;
                    style->background_image_url.clear();
                }
            } else if (value.kind == ValueKind::Url) {
                style->background_image_url = value.string_value;
                style->background_image = BackgroundImage();
            }
            continue;
        }
        if (property == "background-image-url") {
            if (value.kind == ValueKind::Url) {
                style->background_image_url = value.string_value;
                style->background_image = BackgroundImage();
            }
            continue;
        }
        if (property == "background-size") {
            std::vector<CssValue> items = FlattenValue(value);
            BackgroundSize size;
            bool valid = true;
            for (const CssValue& item : items) {
                if (item.kind == ValueKind::Keyword) {
                    const std::string keyword = LowerAsciiText(item.keyword);
                    if (keyword == "cover") {
                        size.kind = BackgroundSizeKind::Cover;
                    } else if (keyword == "contain") {
                        size.kind = BackgroundSizeKind::Contain;
                    } else if (keyword == "auto") {
                        size.kind = BackgroundSizeKind::Auto;
                    } else {
                        valid = false;
                    }
                } else if (item.kind == ValueKind::Length) {
                    if (size.kind == BackgroundSizeKind::Length) {
                        size.height = item.length;
                    } else {
                        size.kind = BackgroundSizeKind::Length;
                        size.width = item.length;
                    }
                } else {
                    valid = false;
                }
            }
            if (valid) {
                style->background_size = size;
            }
            continue;
        }
        if (property == "background-position" || property == "background-position-x" ||
            property == "background-position-y") {
            int component = 0;
            for (const CssValue& item : FlattenValue(value)) {
                float* target = nullptr;
                if (property == "background-position-x") {
                    target = &style->background_position_x;
                } else if (property == "background-position-y") {
                    target = &style->background_position_y;
                } else {
                    target = component == 0 ? &style->background_position_x
                                            : &style->background_position_y;
                }
                if (item.kind == ValueKind::Keyword) {
                    const std::string keyword = LowerAsciiText(item.keyword);
                    if (keyword == "left") {
                        style->background_position_x = 0.0f;
                    } else if (keyword == "right") {
                        style->background_position_x = 1.0f;
                    } else if (keyword == "top") {
                        style->background_position_y = 0.0f;
                        component += 1;
                        continue;
                    } else if (keyword == "bottom") {
                        style->background_position_y = 1.0f;
                        component += 1;
                        continue;
                    } else if (keyword == "center") {
                        *target = 0.5f;
                    }
                } else if (item.kind == ValueKind::Length) {
                    if (item.length.unit == LengthUnit::Percent) {
                        *target = item.length.value / 100.0f;
                    } else if (item.length.unit == LengthUnit::Number) {
                        *target = item.length.value;
                    }
                    // 像素位置需要包含块尺寸，样式阶段无法换算，按左/上对齐处理。
                }
                component += 1;
                if (component >= 2) {
                    break;
                }
            }
            continue;
        }
        if (property == "background-repeat") {
            std::vector<CssValue> items = FlattenValue(value);
            if (!items.empty() && items[0].kind == ValueKind::Keyword) {
                const std::string keyword = LowerAsciiText(items[0].keyword);
                if (keyword == "no-repeat") {
                    style->background_repeat = BackgroundRepeat::NoRepeat;
                } else if (keyword == "repeat-x") {
                    style->background_repeat = BackgroundRepeat::RepeatX;
                } else if (keyword == "repeat-y") {
                    style->background_repeat = BackgroundRepeat::RepeatY;
                } else if (keyword == "repeat") {
                    style->background_repeat = BackgroundRepeat::Repeat;
                }
            }
            continue;
        }
        if (property == "background-clip" || property == "-webkit-background-clip" ||
            property == "background-clip-text") {
            if (value.kind == ValueKind::Keyword) {
                const std::string keyword = LowerAsciiText(value.keyword);
                if (keyword == "text") {
                    style->background_clip_text = true;
                } else if (keyword == "border-box" || keyword == "padding-box" ||
                           keyword == "content-box") {
                    style->background_clip_text = false;
                }
            }
            continue;
        }
        if (property == "overflow" || property == "overflow-x" || property == "overflow-y") {
            if (value.kind == ValueKind::Keyword) {
                const std::string keyword = LowerAsciiText(value.keyword);
                const bool clipped =
                    keyword == "hidden" || keyword == "clip" || keyword == "auto" ||
                    keyword == "scroll" || keyword == "overlay";
                if (keyword == "visible" || keyword == "initial") {
                    style->overflow_hidden = false;
                } else if (clipped) {
                    style->overflow_hidden = true;
                }
            }
            continue;
        }
        if (property == "box-shadow" || property == "text-shadow") {
            std::vector<Shadow> shadows;
            if (ParseShadowList(value, style->font_size, context.root_font_size,
                                 context.viewport_width, context.viewport_height, &shadows)) {
                if (property == "box-shadow") {
                    style->box_shadows = shadows;
                } else {
                    style->text_shadows = shadows;
                }
            } else if (value.kind == ValueKind::Keyword &&
                       LowerAsciiText(value.keyword) == "none") {
                if (property == "box-shadow") {
                    style->box_shadows.clear();
                } else {
                    style->text_shadows.clear();
                }
            }
            continue;
        }
        if (property == "animation") {
            style->animations.clear();
            ParseAnimationShorthand(value, &style->animations);
            continue;
        }
        if (property == "transform") {
            const std::string text = LowerAsciiText(ValueListText(value));
            if (text == "none") {
                style->has_transform = false;
                style->transform_translate_x = CssLength{0.0f, LengthUnit::Px};
                style->transform_translate_y = CssLength{0.0f, LengthUnit::Px};
                style->scale_x = 1.0f;
                style->scale_y = 1.0f;
            } else if (!text.empty()) {
                if (ParseTransformText(text, style)) {
                    style->has_transform = true;
                }
            }
            continue;
        }
    }
}

/* ----------------------------------------------------------------
   CSS 动画采样

   动画在每次计算样式时按时钟采样：先按关键帧声明生成两个相邻帧的
   计算样式，再按缓动后的进度插值回当前样式。插值覆盖 background-color、
   color、边框颜色、opacity、transform（平移与缩放）、box-shadow 与
   text-shadow。
   ---------------------------------------------------------------- */

// 判断关键字是否为时间值（秒或毫秒）并换算为秒。
bool ParseTimeKeyword(const std::string& text, float* seconds) {
    size_t index = 0;
    if (index < text.size() && (text[index] == '+' || text[index] == '-')) {
        index += 1;
    }
    bool has_digit = false;
    while (index < text.size() && std::isdigit(static_cast<unsigned char>(text[index]))) {
        index += 1;
        has_digit = true;
    }
    if (index < text.size() && text[index] == '.') {
        index += 1;
        while (index < text.size() && std::isdigit(static_cast<unsigned char>(text[index]))) {
            index += 1;
            has_digit = true;
        }
    }
    if (!has_digit) {
        return false;
    }
    const std::string number_part = text.substr(0, index);
    const std::string unit_part = LowerAsciiText(text.substr(index));
    char* end = nullptr;
    const float number = std::strtof(number_part.c_str(), &end);
    if (unit_part == "s") {
        *seconds = number;
        return true;
    }
    if (unit_part == "ms") {
        *seconds = number * 0.001f;
        return true;
    }
    return false;
}

// 解析 animation 简写：名称、两个时间值、缓动、循环次数、方向与填充方式。
void ParseAnimationShorthand(const CssValue& value, std::vector<AnimationSpec>* output) {
    const std::vector<CssValue> items = FlattenValue(value);
    AnimationSpec current;
    bool in_spec = false;
    bool has_duration = false;
    const auto flush = [&]() {
        if (in_spec && !current.name.empty()) {
            output->push_back(current);
        }
        current = AnimationSpec();
        in_spec = false;
        has_duration = false;
    };
    for (const CssValue& item : items) {
        if (item.kind == ValueKind::Keyword && item.keyword == ",") {
            flush();
            continue;
        }
        if (item.kind != ValueKind::Keyword) {
            continue;
        }
        const std::string keyword = LowerAsciiText(item.keyword);
        if (keyword == "infinite") {
            current.iteration_count = -1.0f;
            in_spec = true;
            continue;
        }
        if (keyword == "linear" || keyword == "ease" || keyword == "ease-in" ||
            keyword == "ease-out" || keyword == "ease-in-out") {
            if (keyword == "linear") {
                current.timing = AnimationTiming::Linear;
            } else if (keyword == "ease-in") {
                current.timing = AnimationTiming::EaseIn;
            } else if (keyword == "ease-out") {
                current.timing = AnimationTiming::EaseOut;
            } else if (keyword == "ease-in-out") {
                current.timing = AnimationTiming::EaseInOut;
            } else {
                current.timing = AnimationTiming::Ease;
            }
            in_spec = true;
            continue;
        }
        if (keyword == "normal" || keyword == "reverse" || keyword == "alternate" ||
            keyword == "alternate-reverse") {
            if (keyword == "reverse") {
                current.direction = AnimationDirection::Reverse;
            } else if (keyword == "alternate") {
                current.direction = AnimationDirection::Alternate;
            } else if (keyword == "alternate-reverse") {
                current.direction = AnimationDirection::AlternateReverse;
            } else {
                current.direction = AnimationDirection::Normal;
            }
            in_spec = true;
            continue;
        }
        if (keyword == "forwards" || keyword == "backwards" || keyword == "both") {
            if (keyword == "forwards") {
                current.fill = AnimationFill::Forwards;
            } else if (keyword == "backwards") {
                current.fill = AnimationFill::Backwards;
            } else {
                current.fill = AnimationFill::Both;
            }
            in_spec = true;
            continue;
        }
        if (keyword == "none") {
            continue;
        }
        float seconds = 0.0f;
        if (ParseTimeKeyword(keyword, &seconds)) {
            if (!has_duration) {
                current.duration = seconds;
                has_duration = true;
            } else {
                current.delay = seconds;
            }
            in_spec = true;
            continue;
        }
        if (current.name.empty()) {
            current.name = item.keyword;
            in_spec = true;
        }
    }
    flush();
}

// 三次贝塞尔缓动：给定 x 求解曲线上的 y。
float EvaluateCubicBezier(float x1, float y1, float x2, float y2, float x) {
    const auto sample = [](float first, float second, float t) {
        const float inverse = 1.0f - t;
        return 3.0f * inverse * inverse * t * first +
               3.0f * inverse * t * t * second + t * t * t;
    };
    float low = 0.0f;
    float high = 1.0f;
    float t = x;
    for (int iteration = 0; iteration < 24; ++iteration) {
        const float value = sample(x1, x2, t);
        if (std::fabs(value - x) < 1e-4f) {
            break;
        }
        if (value < x) {
            low = t;
        } else {
            high = t;
        }
        t = (low + high) * 0.5f;
    }
    return sample(y1, y2, t);
}

float ApplyAnimationTiming(AnimationTiming timing, float progress) {
    switch (timing) {
        case AnimationTiming::Linear:
            return progress;
        case AnimationTiming::Ease:
            return EvaluateCubicBezier(0.25f, 0.1f, 0.25f, 1.0f, progress);
        case AnimationTiming::EaseIn:
            return EvaluateCubicBezier(0.42f, 0.0f, 1.0f, 1.0f, progress);
        case AnimationTiming::EaseOut:
            return EvaluateCubicBezier(0.0f, 0.0f, 0.58f, 1.0f, progress);
        case AnimationTiming::EaseInOut:
            return EvaluateCubicBezier(0.42f, 0.0f, 0.58f, 1.0f, progress);
    }
    return progress;
}

// 当前时刻在动画中的进度；返回 false 表示动画不参与本帧绘制。
bool ComputeAnimationProgress(const AnimationSpec& animation, double time, float* progress) {
    const double duration = animation.duration > 0.0f ? animation.duration : 0.0;
    const double delay = animation.delay;
    const double local = time - delay;
    const bool infinite = animation.iteration_count < 0.0f;
    const double iterations =
        infinite ? 0.0 : static_cast<double>(animation.iteration_count);
    if (local < 0.0) {
        if (animation.fill == AnimationFill::Backwards ||
            animation.fill == AnimationFill::Both) {
            const bool reverse = animation.direction == AnimationDirection::Reverse ||
                                 animation.direction == AnimationDirection::AlternateReverse;
            *progress = reverse ? 1.0f : 0.0f;
            return true;
        }
        return false;
    }
    if (duration <= 0.0) {
        *progress = 1.0f;
        return true;
    }
    if (!infinite && local >= duration * iterations) {
        if (animation.fill == AnimationFill::Forwards ||
            animation.fill == AnimationFill::Both) {
            bool reverse = animation.direction == AnimationDirection::Reverse;
            if (animation.direction == AnimationDirection::Alternate ||
                animation.direction == AnimationDirection::AlternateReverse) {
                const long long last_index =
                    static_cast<long long>(std::max(0.0, iterations - 1.0));
                const bool odd = (last_index % 2) != 0;
                reverse = animation.direction == AnimationDirection::Alternate ? odd : !odd;
            }
            *progress = reverse ? 0.0f : 1.0f;
            return true;
        }
        return false;
    }
    const double iteration_index = std::floor(local / duration);
    double fraction = (local - iteration_index * duration) / duration;
    bool reverse = false;
    switch (animation.direction) {
        case AnimationDirection::Normal:
            reverse = false;
            break;
        case AnimationDirection::Reverse:
            reverse = true;
            break;
        case AnimationDirection::Alternate:
            reverse = (static_cast<long long>(iteration_index) % 2) != 0;
            break;
        case AnimationDirection::AlternateReverse:
            reverse = (static_cast<long long>(iteration_index) % 2) == 0;
            break;
    }
    if (reverse) {
        fraction = 1.0 - fraction;
    }
    *progress = ApplyAnimationTiming(animation.timing, static_cast<float>(fraction));
    return true;
}

const KeyframesRule* FindKeyframesRule(const StyleSheet& sheet, const std::string& name) {
    for (const KeyframesRule& rule : sheet.keyframes) {
        if (rule.name == name) {
            return &rule;
        }
    }
    return nullptr;
}

float LerpValue(float from, float to, float ratio) {
    return from + (to - from) * ratio;
}

Color LerpColor(const Color& from, const Color& to, float ratio) {
    Color result;
    result.r = LerpValue(from.r, to.r, ratio);
    result.g = LerpValue(from.g, to.g, ratio);
    result.b = LerpValue(from.b, to.b, ratio);
    result.a = LerpValue(from.a, to.a, ratio);
    return result;
}

CssLength LerpLength(const CssLength& from, const CssLength& to, float ratio) {
    if (from.unit != to.unit) {
        return ratio < 0.5f ? from : to;
    }
    CssLength result = from;
    result.value = LerpValue(from.value, to.value, ratio);
    result.offset = LerpValue(from.offset, to.offset, ratio);
    return result;
}

std::vector<Shadow> LerpShadows(const std::vector<Shadow>& from, const std::vector<Shadow>& to,
                                float ratio) {
    if (from.size() != to.size()) {
        return ratio < 0.5f ? from : to;
    }
    std::vector<Shadow> result;
    result.reserve(from.size());
    for (size_t index = 0; index < from.size(); ++index) {
        Shadow shadow;
        shadow.offset_x = LerpValue(from[index].offset_x, to[index].offset_x, ratio);
        shadow.offset_y = LerpValue(from[index].offset_y, to[index].offset_y, ratio);
        shadow.blur = LerpValue(from[index].blur, to[index].blur, ratio);
        shadow.spread = LerpValue(from[index].spread, to[index].spread, ratio);
        shadow.color = LerpColor(from[index].color, to[index].color, ratio);
        shadow.inset = ratio < 0.5f ? from[index].inset : to[index].inset;
        result.push_back(shadow);
    }
    return result;
}

// 按关键帧声明生成计算样式副本，用于相邻帧之间的插值。
void BuildKeyframeStyle(const Keyframe& frame, float parent_font_size,
                        const LengthEvalContext& context, ComputedStyle* style) {
    WinnerMap winners;
    size_t sequence = 0;
    for (const Declaration& declaration : frame.declarations) {
        if (declaration.important) {
            continue;
        }
        ConsiderDeclaration(declaration, Specificity(), kOriginAuthor, &sequence, &winners);
    }
    ApplyWinners(winners, parent_font_size, context, style);
}

void ApplyAnimations(const StyleSheet& sheet, double time, float parent_font_size,
                     const LengthEvalContext& context, ComputedStyle* style) {
    if (style->animations.empty() || sheet.keyframes.empty()) {
        return;
    }
    for (const AnimationSpec& animation : style->animations) {
        const KeyframesRule* rule = FindKeyframesRule(sheet, animation.name);
        if (rule == nullptr || rule->frames.empty()) {
            continue;
        }
        float progress = 0.0f;
        if (!ComputeAnimationProgress(animation, time, &progress)) {
            continue;
        }
        const std::vector<Keyframe>& frames = rule->frames;
        const Keyframe* before = &frames.front();
        const Keyframe* after = &frames.front();
        for (const Keyframe& frame : frames) {
            if (frame.offset <= progress) {
                before = &frame;
            }
        }
        for (const Keyframe& frame : frames) {
            if (frame.offset >= progress) {
                after = &frame;
                break;
            }
        }
        const float span = after->offset - before->offset;
        float ratio = span > 0.0f ? (progress - before->offset) / span : 1.0f;
        ratio = ratio < 0.0f ? 0.0f : (ratio > 1.0f ? 1.0f : ratio);

        ComputedStyle from_style = *style;
        ComputedStyle to_style = *style;
        BuildKeyframeStyle(*before, parent_font_size, context, &from_style);
        BuildKeyframeStyle(*after, parent_font_size, context, &to_style);

        style->background_color =
            LerpColor(from_style.background_color, to_style.background_color, ratio);
        style->color = LerpColor(from_style.color, to_style.color, ratio);
        style->border_color_top =
            LerpColor(from_style.border_color_top, to_style.border_color_top, ratio);
        style->border_color_right =
            LerpColor(from_style.border_color_right, to_style.border_color_right, ratio);
        style->border_color_bottom =
            LerpColor(from_style.border_color_bottom, to_style.border_color_bottom, ratio);
        style->border_color_left =
            LerpColor(from_style.border_color_left, to_style.border_color_left, ratio);
        style->opacity = LerpValue(from_style.opacity, to_style.opacity, ratio);
        style->transform_translate_x =
            LerpLength(from_style.transform_translate_x, to_style.transform_translate_x, ratio);
        style->transform_translate_y =
            LerpLength(from_style.transform_translate_y, to_style.transform_translate_y, ratio);
        style->scale_x = LerpValue(from_style.scale_x, to_style.scale_x, ratio);
        style->scale_y = LerpValue(from_style.scale_y, to_style.scale_y, ratio);
        style->has_transform = from_style.has_transform || to_style.has_transform;
        style->box_shadows = LerpShadows(from_style.box_shadows, to_style.box_shadows, ratio);
        style->text_shadows = LerpShadows(from_style.text_shadows, to_style.text_shadows, ratio);
    }
}

}  // namespace

/* ----------------------------------------------------------------
   StyleResolver
   ---------------------------------------------------------------- */

StyleResolver::StyleResolver() = default;

StyleResolver::~StyleResolver() = default;

void StyleResolver::SetStyleSheet(const StyleSheet* stylesheet) {
    stylesheet_ = stylesheet;
}

void StyleResolver::SetViewport(float width, float height) {
    viewport_width_ = width > 1.0f ? width : 1.0f;
    viewport_height_ = height > 1.0f ? height : 1.0f;
}

void StyleResolver::SetAnimationTime(double seconds) {
    animation_time_ = seconds;
}

ComputedStyle StyleResolver::InitialStyle() const {
    ComputedStyle style;
    style.display = Display::Block;
    style.font_size = 16.0f;
    style.root_font_size = 16.0f;
    style.color = Rgba(26.0f / 255.0f, 26.0f / 255.0f, 26.0f / 255.0f, 1.0f);
    style.background_color = White();
    return style;
}

ComputedStyle StyleResolver::ComputeFor(const Element& element,
                                        const ComputedStyle& parent_style) const {
    ComputedStyle style;

    /* 可继承属性先取父元素的计算值。 */
    style.font_size = parent_style.font_size;
    style.font_family = parent_style.font_family;
    style.font_weight = parent_style.font_weight;
    style.italic = parent_style.italic;
    style.color = parent_style.color;
    style.line_height = parent_style.line_height;
    style.text_align = parent_style.text_align;
    style.white_space = parent_style.white_space;
    style.letter_spacing = parent_style.letter_spacing;
    style.list_style_type = parent_style.list_style_type;
    style.list_style_position = parent_style.list_style_position;
    style.underline = parent_style.underline;
    style.text_indent = parent_style.text_indent;
    style.word_break = parent_style.word_break;
    style.overflow_wrap_anywhere = parent_style.overflow_wrap_anywhere;
    style.root_font_size = parent_style.root_font_size > 0.0f ? parent_style.root_font_size
                                                              : parent_style.font_size;

    std::vector<RawDeclaration> raw_declarations;
    size_t sequence = 0;
    CollectRawFromStyleSheet(UserAgentStyleSheet(), element, kOriginUserAgent, &sequence,
                             &raw_declarations);
    if (stylesheet_ != nullptr) {
        CollectRawFromStyleSheet(*stylesheet_, element, kOriginAuthor, &sequence,
                                 &raw_declarations);
    }
    CollectRawInlineStyle(element, &sequence, &raw_declarations);

    /* 自定义属性先层叠，再用于展开其余声明中的 var() 引用。 */
    ResolveCustomProperties(raw_declarations, parent_style.custom_properties,
                            &style.custom_properties);

    LengthEvalContext context;
    context.font_size = parent_style.font_size;
    context.root_font_size = style.root_font_size;
    context.viewport_width = viewport_width_;
    context.viewport_height = viewport_height_;

    WinnerMap winners;
    sequence = 0;
    for (const RawDeclaration& raw : raw_declarations) {
        if (IsCustomPropertyName(raw.declaration.property)) {
            continue;
        }
        Declaration declaration = raw.declaration;
        if (!ExpandVariables(raw.declaration.value, style.custom_properties, &declaration.value)) {
            continue;
        }
        /* 长度函数（calc/min/max/clamp）在层叠前求值，使简写展开与属性映射
           都能按普通长度处理；求值失败视为无效声明。 */
        CssValue normalized;
        if (!NormalizeLengthFunctions(declaration.value, context, &normalized)) {
            continue;
        }
        declaration.value = normalized;
        ConsiderDeclaration(declaration, raw.specificity, raw.origin, &sequence, &winners);
    }

    ApplyWinners(winners, parent_style.font_size, context, &style);
    /* CSS 动画：按动画时钟采样关键帧并覆盖对应属性。 */
    if (!style.animations.empty()) {
        animation_active_ = true;
        if (stylesheet_ != nullptr) {
            ApplyAnimations(*stylesheet_, animation_time_, parent_style.font_size, context,
                            &style);
        }
    }
    /* 根元素的字号作为全文档 rem 基准向下传递。 */
    if (style.root_font_size <= 0.0f) {
        style.root_font_size = 16.0f;
    }
    return style;
}

bool StyleResolver::ComputePseudoFor(const Element& element, const ComputedStyle& origin_style,
                                     const std::string& pseudo_name,
                                     ComputedStyle* output) const {
    if (output == nullptr || (pseudo_name != "before" && pseudo_name != "after")) {
        return false;
    }

    /* 伪元素同样从宿主元素继承可继承属性，初始 display 为 inline。 */
    ComputedStyle style;
    style.display = Display::Inline;
    style.font_size = origin_style.font_size;
    style.font_family = origin_style.font_family;
    style.font_weight = origin_style.font_weight;
    style.italic = origin_style.italic;
    style.color = origin_style.color;
    style.line_height = origin_style.line_height;
    style.text_align = origin_style.text_align;
    style.white_space = origin_style.white_space;
    style.letter_spacing = origin_style.letter_spacing;
    style.list_style_type = origin_style.list_style_type;
    style.list_style_position = origin_style.list_style_position;
    style.underline = origin_style.underline;
    style.text_indent = origin_style.text_indent;
    style.word_break = origin_style.word_break;
    style.overflow_wrap_anywhere = origin_style.overflow_wrap_anywhere;
    style.root_font_size = origin_style.root_font_size > 0.0f ? origin_style.root_font_size
                                                             : origin_style.font_size;

    std::vector<RawDeclaration> raw_declarations;
    size_t sequence = 0;
    CollectRawFromPseudoStyleSheet(UserAgentStyleSheet(), element, pseudo_name, kOriginUserAgent,
                                   &sequence, &raw_declarations);
    if (stylesheet_ != nullptr) {
        CollectRawFromPseudoStyleSheet(*stylesheet_, element, pseudo_name, kOriginAuthor,
                                       &sequence, &raw_declarations);
    }
    CollectRawInlineStyle(element, &sequence, &raw_declarations);

    ResolveCustomProperties(raw_declarations, origin_style.custom_properties,
                            &style.custom_properties);

    LengthEvalContext context;
    context.font_size = origin_style.font_size;
    context.root_font_size = style.root_font_size;
    context.viewport_width = viewport_width_;
    context.viewport_height = viewport_height_;

    WinnerMap winners;
    sequence = 0;
    CssValue content_value;
    bool has_content_declaration = false;
    for (const RawDeclaration& raw : raw_declarations) {
        if (IsCustomPropertyName(raw.declaration.property)) {
            continue;
        }
        Declaration declaration = raw.declaration;
        CssValue expanded;
        if (!ExpandVariables(raw.declaration.value, style.custom_properties, &expanded)) {
            continue;
        }
        if (declaration.property == "content") {
            content_value = expanded;
            has_content_declaration = true;
            continue;
        }
        CssValue normalized;
        if (!NormalizeLengthFunctions(expanded, context, &normalized)) {
            continue;
        }
        declaration.value = normalized;
        ConsiderDeclaration(declaration, raw.specificity, raw.origin, &sequence, &winners);
    }

    if (!has_content_declaration) {
        return false;
    }
    std::string content;
    if (!ExtractPseudoContent(content_value, element, &content)) {
        return false;
    }

    ApplyWinners(winners, origin_style.font_size, context, &style);
    style.pseudo_content = content;
    style.pseudo_name = pseudo_name;
    if (style.root_font_size <= 0.0f) {
        style.root_font_size = 16.0f;
    }
    *output = style;
    return true;
}

}  // namespace web
