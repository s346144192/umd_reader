#include "web/css_parser.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

extern "C" {
#include "css_builder.h"
#include "css_lexer.h"
#include "css_parser.h"
}

namespace web {
namespace {

/* ----------------------------------------------------------------
   文本工具
   ---------------------------------------------------------------- */

std::string AsciiLower(const std::string& text) {
    std::string result = text;
    for (size_t index = 0; index < result.size(); ++index) {
        char character = result[index];
        if (character >= 'A' && character <= 'Z') {
            result[index] = static_cast<char>(character - 'A' + 'a');
        }
    }
    return result;
}

std::string Trim(const std::string& text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
        begin += 1;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
        end -= 1;
    }
    return text.substr(begin, end - begin);
}

char* DuplicateCString(const std::string& text) {
    char* result = static_cast<char*>(std::malloc(text.size() + 1));
    if (result == nullptr) {
        return nullptr;
    }
    std::memcpy(result, text.c_str(), text.size() + 1);
    return result;
}

/* ----------------------------------------------------------------
   颜色与长度解析
   ---------------------------------------------------------------- */

struct NamedColorEntry {
    const char* name;
    // 0xAARRGGBB
    uint32_t argb;
};

const NamedColorEntry kNamedColors[] = {
    {"transparent", 0x00000000u},
    {"aliceblue", 0xFFF0F8FFu},
    {"antiquewhite", 0xFFFAEBD7u},
    {"aqua", 0xFF00FFFFu},
    {"aquamarine", 0xFF7FFFD4u},
    {"azure", 0xFFF0FFFFu},
    {"beige", 0xFFF5F5DCu},
    {"bisque", 0xFFFFE4C4u},
    {"black", 0xFF000000u},
    {"blanchedalmond", 0xFFFFEBCDu},
    {"blue", 0xFF0000FFu},
    {"blueviolet", 0xFF8A2BE2u},
    {"brown", 0xFFA52A2Au},
    {"burlywood", 0xFFDEB887u},
    {"cadetblue", 0xFF5F9EA0u},
    {"chartreuse", 0xFF7FFF00u},
    {"chocolate", 0xFFD2691Eu},
    {"coral", 0xFFFF7F50u},
    {"cornflowerblue", 0xFF6495EDu},
    {"cornsilk", 0xFFFFF8DCu},
    {"crimson", 0xFFDC143Cu},
    {"cyan", 0xFF00FFFFu},
    {"darkblue", 0xFF00008Bu},
    {"darkcyan", 0xFF008B8Bu},
    {"darkgoldenrod", 0xFFB8860Bu},
    {"darkgray", 0xFFA9A9A9u},
    {"darkgreen", 0xFF006400u},
    {"darkgrey", 0xFFA9A9A9u},
    {"darkkhaki", 0xFFBDB76Bu},
    {"darkmagenta", 0xFF8B008Bu},
    {"darkolivegreen", 0xFF556B2Fu},
    {"darkorange", 0xFFFF8C00u},
    {"darkorchid", 0xFF9932CCu},
    {"darkred", 0xFF8B0000u},
    {"darksalmon", 0xFFE9967Au},
    {"darkseagreen", 0xFF8FBC8Fu},
    {"darkslateblue", 0xFF483D8Bu},
    {"darkslategray", 0xFF2F4F4Fu},
    {"darkslategrey", 0xFF2F4F4Fu},
    {"darkturquoise", 0xFF00CED1u},
    {"darkviolet", 0xFF9400D3u},
    {"deeppink", 0xFFFF1493u},
    {"deepskyblue", 0xFF00BFFFu},
    {"dimgray", 0xFF696969u},
    {"dimgrey", 0xFF696969u},
    {"dodgerblue", 0xFF1E90FFu},
    {"firebrick", 0xFFB22222u},
    {"floralwhite", 0xFFFFFAF0u},
    {"forestgreen", 0xFF228B22u},
    {"fuchsia", 0xFFFF00FFu},
    {"gainsboro", 0xFFDCDCDCu},
    {"ghostwhite", 0xFFF8F8FFu},
    {"gold", 0xFFFFD700u},
    {"goldenrod", 0xFFDAA520u},
    {"gray", 0xFF808080u},
    {"green", 0xFF008000u},
    {"greenyellow", 0xFFADFF2Fu},
    {"grey", 0xFF808080u},
    {"honeydew", 0xFFF0FFF0u},
    {"hotpink", 0xFFFF69B4u},
    {"indianred", 0xFFCD5C5Cu},
    {"indigo", 0xFF4B0082u},
    {"ivory", 0xFFFFFFF0u},
    {"khaki", 0xFFF0E68Cu},
    {"lavender", 0xFFE6E6FAu},
    {"lavenderblush", 0xFFFFF0F5u},
    {"lawngreen", 0xFF7CFC00u},
    {"lemonchiffon", 0xFFFFFACDu},
    {"lightblue", 0xFFADD8E6u},
    {"lightcoral", 0xFFF08080u},
    {"lightcyan", 0xFFE0FFFFu},
    {"lightgoldenrodyellow", 0xFFFAFAD2u},
    {"lightgray", 0xFFD3D3D3u},
    {"lightgreen", 0xFF90EE90u},
    {"lightgrey", 0xFFD3D3D3u},
    {"lightpink", 0xFFFFB6C1u},
    {"lightsalmon", 0xFFFFA07Au},
    {"lightseagreen", 0xFF20B2AAu},
    {"lightskyblue", 0xFF87CEFAu},
    {"lightslategray", 0xFF778899u},
    {"lightslategrey", 0xFF778899u},
    {"lightsteelblue", 0xFFB0C4DEu},
    {"lightyellow", 0xFFFFFFE0u},
    {"lime", 0xFF00FF00u},
    {"limegreen", 0xFF32CD32u},
    {"linen", 0xFFFAF0E6u},
    {"magenta", 0xFFFF00FFu},
    {"maroon", 0xFF800000u},
    {"mediumaquamarine", 0xFF66CDAAu},
    {"mediumblue", 0xFF0000CDu},
    {"mediumorchid", 0xFFBA55D3u},
    {"mediumpurple", 0xFF9370DBu},
    {"mediumseagreen", 0xFF3CB371u},
    {"mediumslateblue", 0xFF7B68EEu},
    {"mediumspringgreen", 0xFF00FA9Au},
    {"mediumturquoise", 0xFF48D1CCu},
    {"mediumvioletred", 0xFFC71585u},
    {"midnightblue", 0xFF191970u},
    {"mintcream", 0xFFF5FFFAu},
    {"mistyrose", 0xFFFFE4E1u},
    {"moccasin", 0xFFFFE4B5u},
    {"navajowhite", 0xFFFFDEADu},
    {"navy", 0xFF000080u},
    {"oldlace", 0xFFFDF5E6u},
    {"olive", 0xFF808000u},
    {"olivedrab", 0xFF6B8E23u},
    {"orange", 0xFFFFA500u},
    {"orangered", 0xFFFF4500u},
    {"orchid", 0xFFDA70D6u},
    {"palegoldenrod", 0xFFEEE8AAu},
    {"palegreen", 0xFF98FB98u},
    {"paleturquoise", 0xFFAFEEEEu},
    {"palevioletred", 0xFFDB7093u},
    {"papayawhip", 0xFFFFEFD5u},
    {"peachpuff", 0xFFFFDAB9u},
    {"peru", 0xFFCD853Fu},
    {"pink", 0xFFFFC0CBu},
    {"plum", 0xFFDDA0DDu},
    {"powderblue", 0xFFB0E0E6u},
    {"purple", 0xFF800080u},
    {"rebeccapurple", 0xFF663399u},
    {"red", 0xFFFF0000u},
    {"rosybrown", 0xFFBC8F8Fu},
    {"royalblue", 0xFF4169E1u},
    {"saddlebrown", 0xFF8B4513u},
    {"salmon", 0xFFFA8072u},
    {"sandybrown", 0xFFF4A460u},
    {"seagreen", 0xFF2E8B57u},
    {"seashell", 0xFFFFF5EEu},
    {"sienna", 0xFFA0522Du},
    {"silver", 0xFFC0C0C0u},
    {"skyblue", 0xFF87CEEBu},
    {"slateblue", 0xFF6A5ACDu},
    {"slategray", 0xFF708090u},
    {"slategrey", 0xFF708090u},
    {"snow", 0xFFFFFAFAu},
    {"springgreen", 0xFF00FF7Fu},
    {"steelblue", 0xFF4682B4u},
    {"tan", 0xFFD2B48Cu},
    {"teal", 0xFF008080u},
    {"thistle", 0xFFD8BFD8u},
    {"tomato", 0xFFFF6347u},
    {"turquoise", 0xFF40E0D0u},
    {"violet", 0xFFEE82EEu},
    {"wheat", 0xFFF5DEB3u},
    {"white", 0xFFFFFFFFu},
    {"whitesmoke", 0xFFF5F5F5u},
    {"yellow", 0xFFFFFF00u},
    {"yellowgreen", 0xFF9ACD32u},
};

bool LookupNamedColor(const std::string& name, Color* output) {
    std::string lowered = AsciiLower(Trim(name));
    for (const NamedColorEntry& entry : kNamedColors) {
        if (lowered == entry.name) {
            const float red = static_cast<float>((entry.argb >> 16) & 0xFFu) / 255.0f;
            const float green = static_cast<float>((entry.argb >> 8) & 0xFFu) / 255.0f;
            const float blue = static_cast<float>(entry.argb & 0xFFu) / 255.0f;
            const float alpha = static_cast<float>((entry.argb >> 24) & 0xFFu) / 255.0f;
            *output = Rgba(red, green, blue, alpha);
            return true;
        }
    }
    return false;
}

bool ParseHexColor(const std::string& text, Color* output) {
    std::string digits = text;
    if (!digits.empty() && digits[0] == '#') {
        digits.erase(0, 1);
    }
    for (size_t index = 0; index < digits.size(); ++index) {
        if (std::isxdigit(static_cast<unsigned char>(digits[index])) == 0) {
            return false;
        }
    }
    unsigned int r = 0;
    unsigned int g = 0;
    unsigned int b = 0;
    unsigned int a = 255;
    if (digits.size() == 3 || digits.size() == 4) {
        auto nibble = [](char character) -> unsigned int {
            if (character >= '0' && character <= '9') {
                return static_cast<unsigned int>(character - '0');
            }
            return static_cast<unsigned int>(AsciiLower(std::string(1, character))[0] - 'a' + 10);
        };
        r = nibble(digits[0]) * 17u;
        g = nibble(digits[1]) * 17u;
        b = nibble(digits[2]) * 17u;
        if (digits.size() == 4) {
            a = nibble(digits[3]) * 17u;
        }
    } else if (digits.size() == 6 || digits.size() == 8) {
        auto byte_at = [&digits](size_t offset) -> unsigned int {
            auto nibble = [](char character) -> unsigned int {
                if (character >= '0' && character <= '9') {
                    return static_cast<unsigned int>(character - '0');
                }
                return static_cast<unsigned int>(AsciiLower(std::string(1, character))[0] - 'a' + 10);
            };
            return nibble(digits[offset]) * 16u + nibble(digits[offset + 1]);
        };
        r = byte_at(0);
        g = byte_at(2);
        b = byte_at(4);
        if (digits.size() == 8) {
            a = byte_at(6);
        }
    } else {
        return false;
    }
    *output = Rgba(static_cast<float>(r) / 255.0f, static_cast<float>(g) / 255.0f,
                   static_cast<float>(b) / 255.0f, static_cast<float>(a) / 255.0f);
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

bool UnitFromSuffix(const std::string& suffix, LengthUnit* output) {
    if (suffix.empty()) {
        *output = LengthUnit::Number;
        return true;
    }
    if (suffix == "px") {
        *output = LengthUnit::Px;
    } else if (suffix == "em") {
        *output = LengthUnit::Em;
    } else if (suffix == "ex") {
        *output = LengthUnit::Ex;
    } else if (suffix == "rem") {
        *output = LengthUnit::Rem;
    } else if (suffix == "%") {
        *output = LengthUnit::Percent;
    } else if (suffix == "pt") {
        *output = LengthUnit::Pt;
    } else if (suffix == "pc") {
        *output = LengthUnit::Pc;
    } else if (suffix == "in") {
        *output = LengthUnit::In;
    } else if (suffix == "cm") {
        *output = LengthUnit::Cm;
    } else if (suffix == "mm") {
        *output = LengthUnit::Mm;
    } else if (suffix == "ch") {
        *output = LengthUnit::Ch;
    } else if (suffix == "vw") {
        *output = LengthUnit::Vw;
    } else if (suffix == "vh") {
        *output = LengthUnit::Vh;
    } else if (suffix == "vmin") {
        *output = LengthUnit::Vmin;
    } else if (suffix == "vmax") {
        *output = LengthUnit::Vmax;
    } else {
        return false;
    }
    return true;
}

bool ParseLengthText(const std::string& text, CssLength* output) {
    size_t index = 0;
    bool has_digit = false;
    if (index < text.size() && (text[index] == '+' || text[index] == '-')) {
        index += 1;
    }
    while (index < text.size() && std::isdigit(static_cast<unsigned char>(text[index])) != 0) {
        index += 1;
        has_digit = true;
    }
    if (index < text.size() && text[index] == '.') {
        index += 1;
        while (index < text.size() && std::isdigit(static_cast<unsigned char>(text[index])) != 0) {
            index += 1;
            has_digit = true;
        }
    }
    if (!has_digit) {
        return false;
    }
    std::string number_part = text.substr(0, index);
    std::string unit_part = AsciiLower(text.substr(index));
    LengthUnit unit = LengthUnit::Auto;
    if (!UnitFromSuffix(unit_part, &unit)) {
        return false;
    }
    output->value = static_cast<float>(ParseNumberLiteral(number_part));
    output->unit = unit;
    return true;
}

/* 把一个数值文本转换为 0..1 的分量。
   alpha 为真时按 0..1 解读（可带百分比），否则按 0..255 解读。 */
bool ParseColorComponent(const std::string& text, bool alpha, float* output) {
    std::string part = Trim(text);
    if (part.empty()) {
        return false;
    }
    bool percent = part.size() > 1 && part[part.size() - 1] == '%';
    if (percent) {
        part.erase(part.size() - 1);
    }
    size_t index = 0;
    if (index < part.size() && (part[index] == '+' || part[index] == '-')) {
        index += 1;
    }
    bool has_digit = false;
    while (index < part.size() && std::isdigit(static_cast<unsigned char>(part[index])) != 0) {
        index += 1;
        has_digit = true;
    }
    if (index < part.size() && part[index] == '.') {
        index += 1;
        while (index < part.size() && std::isdigit(static_cast<unsigned char>(part[index])) != 0) {
            index += 1;
            has_digit = true;
        }
    }
    if (!has_digit || index != part.size()) {
        return false;
    }
    double value = ParseNumberLiteral(part);
    if (percent) {
        value = value / 100.0;
    } else if (!alpha) {
        value = value / 255.0;
    }
    if (value < 0.0) {
        value = 0.0;
    }
    if (value > 1.0) {
        value = 1.0;
    }
    *output = static_cast<float>(value);
    return true;
}

void SplitColorArguments(const std::string& arguments, std::vector<std::string>* output) {
    std::string current;
    for (size_t index = 0; index < arguments.size(); ++index) {
        char character = arguments[index];
        if (character == ',' || character == '/' ||
            std::isspace(static_cast<unsigned char>(character)) != 0) {
            if (!current.empty()) {
                output->push_back(current);
                current.clear();
            }
            continue;
        }
        current.push_back(character);
    }
    if (!current.empty()) {
        output->push_back(current);
    }
}

// 解析色相：支持 deg、grad、rad、turn 与无单位数值，结果规整到 [0, 360)。
bool ParseHueComponent(const std::string& text, float* output) {
    std::string value = Trim(text);
    const std::string lowered = AsciiLower(value);
    double scale = 1.0;
    size_t suffix_length = 0;
    if (lowered.size() > 4 && lowered.substr(lowered.size() - 4) == "grad") {
        suffix_length = 4;
        scale = 0.9;
    } else if (lowered.size() > 4 && lowered.substr(lowered.size() - 4) == "turn") {
        suffix_length = 4;
        scale = 360.0;
    } else if (lowered.size() > 3 && lowered.substr(lowered.size() - 3) == "deg") {
        suffix_length = 3;
    } else if (lowered.size() > 3 && lowered.substr(lowered.size() - 3) == "rad") {
        suffix_length = 3;
        scale = 180.0 / 3.14159265358979323846;
    }
    if (suffix_length > 0) {
        value = value.substr(0, value.size() - suffix_length);
    }
    if (value.empty()) {
        return false;
    }
    const char* begin = value.c_str();
    char* end = nullptr;
    const double parsed = std::strtod(begin, &end);
    if (end == begin || *end != '\0') {
        return false;
    }
    double hue = std::fmod(parsed * scale, 360.0);
    if (hue < 0.0) {
        hue += 360.0;
    }
    *output = static_cast<float>(hue);
    return true;
}

// 解析百分比分量（用于 hsl 的饱和度与亮度），结果规整到 [0, 1]。
bool ParsePercentComponent(const std::string& text, float* output) {
    std::string value = Trim(text);
    if (value.empty() || value.back() != '%') {
        return false;
    }
    value.pop_back();
    if (value.empty()) {
        return false;
    }
    const char* begin = value.c_str();
    char* end = nullptr;
    const double parsed = std::strtod(begin, &end);
    if (end == begin || *end != '\0') {
        return false;
    }
    double normalized = parsed / 100.0;
    normalized = std::clamp(normalized, 0.0, 1.0);
    *output = static_cast<float>(normalized);
    return true;
}

float HueChannel(float p, float q, float t) {
    if (t < 0.0f) {
        t += 1.0f;
    }
    if (t > 1.0f) {
        t -= 1.0f;
    }
    if (t < 1.0f / 6.0f) {
        return p + (q - p) * 6.0f * t;
    }
    if (t < 0.5f) {
        return q;
    }
    if (t < 2.0f / 3.0f) {
        return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
    }
    return p;
}

/* 解析 rgb()/rgba()/hsl()/hsla() 的完整文本（形如 "rgba(0,0,0,0.5)"）。 */
bool ParseColorFunction(const std::string& text, Color* output) {
    size_t open = text.find('(');
    if (open == std::string::npos || text.empty() || text[text.size() - 1] != ')') {
        return false;
    }
    std::string name = AsciiLower(Trim(text.substr(0, open)));
    const bool hsl = name == "hsl" || name == "hsla";
    if (!hsl && name != "rgb" && name != "rgba") {
        return false;
    }
    std::vector<std::string> parts;
    SplitColorArguments(text.substr(open + 1, text.size() - open - 2), &parts);
    if (parts.size() != 3 && parts.size() != 4) {
        return false;
    }
    float alpha = 1.0f;
    if (parts.size() == 4 && !ParseColorComponent(parts[3], true, &alpha)) {
        return false;
    }
    if (hsl) {
        float hue = 0.0f;
        float saturation = 0.0f;
        float lightness = 0.0f;
        if (!ParseHueComponent(parts[0], &hue) ||
            !ParsePercentComponent(parts[1], &saturation) ||
            !ParsePercentComponent(parts[2], &lightness)) {
            return false;
        }
        const float normalized_hue = hue / 360.0f;
        const float q = lightness < 0.5f
                            ? lightness * (1.0f + saturation)
                            : lightness + saturation - lightness * saturation;
        const float p = 2.0f * lightness - q;
        const float red = HueChannel(p, q, normalized_hue + 1.0f / 3.0f);
        const float green = HueChannel(p, q, normalized_hue);
        const float blue = HueChannel(p, q, normalized_hue - 1.0f / 3.0f);
        *output = Rgba(red, green, blue, alpha);
        return true;
    }
    float red = 0.0f;
    float green = 0.0f;
    float blue = 0.0f;
    if (!ParseColorComponent(parts[0], false, &red) ||
        !ParseColorComponent(parts[1], false, &green) ||
        !ParseColorComponent(parts[2], false, &blue)) {
        return false;
    }
    *output = Rgba(red, green, blue, alpha);
    return true;
}

/* 该属性是否可能接受颜色值，用于把命名颜色转换成颜色分量。 */
bool AcceptsColor(const std::string& property) {
    return property == "color" || property == "background" || property == "background-color" ||
           property == "outline" || property == "outline-color" ||
           property == "text-decoration-color" || property == "box-shadow" ||
           property == "text-shadow" || property == "fill" || property == "stroke" ||
           property.rfind("border", 0) == 0;
}

bool PreservesCase(const std::string& property) {
    return property == "font-family" || property == "font";
}

/* ----------------------------------------------------------------
   节点模型
   ---------------------------------------------------------------- */

enum class NodeKind {
    Value,
    ValueList,
    Compound,
    SimpleList,
    Steps,
    Selector,
    SelectorList,
    Declaration,
    DeclarationList,
    Args,
};

struct WebCssNode {
    NodeKind kind = NodeKind::Value;
    int item_kind = 0;
    std::string text;

    CssValue value;
    std::vector<CssValue> values;
    std::vector<WebCssNode*> raw_items;

    SimpleSelector simple;
    std::vector<SimpleSelector> simples;
    CompoundSelector compound;
    std::vector<SelectorStep> steps;
    Selector selector;
    std::vector<Selector> selectors;

    Declaration declaration;
    std::vector<Declaration> declarations;
};

WebCssNode* AsNode(void* pointer) {
    return static_cast<WebCssNode*>(pointer);
}

/* 由原始值分量生成 CSS 值；property 用于判断命名颜色与大小写策略。 */
CssValue ConvertItem(const WebCssNode& item, const std::string& property, bool allow_color) {
    CssValue result;
    switch (item.item_kind) {
    case WEBCSS_VALUE_IDENT: {
        std::string keyword = PreservesCase(property) ? item.text : AsciiLower(item.text);
        if (allow_color) {
            Color color;
            if (LookupNamedColor(item.text, &color)) {
                result.kind = ValueKind::Color;
                result.color.color = color;
                result.color.specified = true;
                return result;
            }
        }
        result.kind = ValueKind::Keyword;
        result.keyword = keyword;
        return result;
    }
    case WEBCSS_VALUE_STRING:
        result.kind = ValueKind::String;
        result.string_value = item.text;
        return result;
    case WEBCSS_VALUE_HASH: {
        Color color;
        if (ParseHexColor(item.text, &color)) {
            result.kind = ValueKind::Color;
            result.color.color = color;
            result.color.specified = true;
        } else {
            result.kind = ValueKind::Keyword;
            result.keyword = "#" + item.text;
        }
        return result;
    }
    case WEBCSS_VALUE_NUMBER: {
        CssLength length;
        if (ParseLengthText(item.text, &length)) {
            result.kind = ValueKind::Length;
            result.length = length;
        } else {
            result.kind = ValueKind::Keyword;
            result.keyword = AsciiLower(item.text);
        }
        return result;
    }
    case WEBCSS_VALUE_URL:
        result.kind = ValueKind::Url;
        result.string_value = item.text;
        return result;
    case WEBCSS_VALUE_DELIM:
        result.kind = ValueKind::Keyword;
        result.keyword = item.text;
        return result;
    case WEBCSS_VALUE_FUNCTION: {
        if (allow_color) {
            Color color;
            if (ParseColorFunction(item.text, &color)) {
                result.kind = ValueKind::Color;
                result.color.color = color;
                result.color.specified = true;
                return result;
            }
        }
        result.kind = ValueKind::Keyword;
        result.keyword = item.text;
        return result;
    }
    default:
        result.kind = ValueKind::Keyword;
        result.keyword = item.text;
        return result;
    }
}

/* ----------------------------------------------------------------
   解析入口的公共实现
   ---------------------------------------------------------------- */

bool RunParse(const std::string& source, StyleSheet* stylesheet, std::string* error) {
    std::string buffer = source;
    /* 词法器在输入末尾可能前读若干字节，补充哨兵字节避免越界。 */
    buffer.append(4, '\0');

    WebCssParseState state;
    std::memset(&state, 0, sizeof(state));
    state.input = buffer.data();
    state.cursor = buffer.data();
    state.limit = buffer.data() + source.size();
    state.line = 1;
    state.column = 1;
    state.stylesheet = stylesheet;

    int result = webcssparse(&state);
    bool succeeded = result == 0 && state.failed == 0 && state.error_count == 0;
    if (!succeeded && error != nullptr) {
        if (state.error_message[0] != '\0') {
            *error = state.error_message;
        } else {
            *error = "CSS 解析失败";
        }
    }
    webcss_state_reset(&state);
    return succeeded;
}

/* ----------------------------------------------------------------
   @media / @supports 预处理

   语法层把未知 at-rule 整块跳过，无法按视口条件筛选。这里在解析前做一次
   文本级展开：条件成立的块内容原样并入输出（保留换行以维持行号），不成立的
   块整体丢弃。其余 at-rule（@font-face、@keyframes 等）保持原样交由语法层跳过。
   ---------------------------------------------------------------- */

bool IsIdentChar(char character) {
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') || character == '-' || character == '_';
}

/* 从 begin 起跳过注释与字符串，返回下一个有效字符的位置；end 为上限。 */
size_t SkipNonCode(const std::string& source, size_t begin, size_t end) {
    size_t index = begin;
    while (index < end) {
        if (source[index] == '/' && index + 1 < end && source[index + 1] == '*') {
            index += 2;
            while (index + 1 < end && !(source[index] == '*' && source[index + 1] == '/')) {
                index += 1;
            }
            index = (index + 1 < end) ? index + 2 : end;
            continue;
        }
        if (source[index] == '"' || source[index] == '\'') {
            const char quote = source[index];
            index += 1;
            while (index < end) {
                if (source[index] == '\\' && index + 1 < end) {
                    index += 2;
                    continue;
                }
                if (source[index] == quote) {
                    index += 1;
                    break;
                }
                index += 1;
            }
            continue;
        }
        break;
    }
    return index;
}

/* 查找不在注释/字符串内的目标字符；找不到返回 end。 */
size_t FindCodeChar(const std::string& source, size_t begin, size_t end, char target) {
    size_t index = begin;
    while (index < end) {
        index = SkipNonCode(source, index, end);
        if (index >= end) {
            return end;
        }
        if (source[index] == target) {
            return index;
        }
        index += 1;
    }
    return end;
}

/* 从 '{' 起找到配对的 '}'，返回其后一位；不平衡时返回 end。 */
size_t FindBlockEnd(const std::string& source, size_t brace, size_t end) {
    int depth = 0;
    size_t index = brace;
    while (index < end) {
        index = SkipNonCode(source, index, end);
        if (index >= end) {
            return end;
        }
        if (source[index] == '{') {
            depth += 1;
        } else if (source[index] == '}') {
            depth -= 1;
            if (depth == 0) {
                return index + 1;
            }
        }
        index += 1;
    }
    return end;
}

/* 媒体条件的长度值：支持 px/em/rem/pt/in/cm/mm，em 与 rem 按 16px 折算。 */
bool ParseMediaLength(const std::string& text, float* output) {
    CssLength length;
    if (!ParseLengthText(Trim(text), &length)) {
        return false;
    }
    switch (length.unit) {
        case LengthUnit::Px:
        case LengthUnit::Number:
            *output = length.value;
            return true;
        case LengthUnit::Em:
        case LengthUnit::Rem:
            *output = length.value * 16.0f;
            return true;
        case LengthUnit::Pt:
            *output = length.value * 96.0f / 72.0f;
            return true;
        case LengthUnit::In:
            *output = length.value * 96.0f;
            return true;
        case LengthUnit::Cm:
            *output = length.value * 96.0f / 2.54f;
            return true;
        case LengthUnit::Mm:
            *output = length.value * 96.0f / 25.4f;
            return true;
        default:
            return false;
    }
}

/* 单个 (feature: value) 条件求值。未知特性按不成立处理。 */
bool EvaluateMediaFeature(const std::string& feature, float viewport_width,
                          float viewport_height) {
    std::string text = Trim(feature);
    std::string name = text;
    std::string value;
    const size_t colon = text.find(':');
    if (colon != std::string::npos) {
        name = Trim(text.substr(0, colon));
        value = Trim(text.substr(colon + 1));
    }
    name = AsciiLower(name);
    std::string lowered_value = AsciiLower(value);
    float number = 0.0f;

    if (name == "min-width") {
        return ParseMediaLength(value, &number) && viewport_width >= number;
    }
    if (name == "max-width") {
        return ParseMediaLength(value, &number) && viewport_width <= number;
    }
    if (name == "width") {
        return ParseMediaLength(value, &number) && viewport_width == number;
    }
    if (name == "min-height") {
        return ParseMediaLength(value, &number) && viewport_height >= number;
    }
    if (name == "max-height") {
        return ParseMediaLength(value, &number) && viewport_height <= number;
    }
    if (name == "orientation") {
        if (lowered_value == "portrait") {
            return viewport_height > viewport_width;
        }
        if (lowered_value == "landscape") {
            return viewport_width >= viewport_height;
        }
        return false;
    }
    if (name == "prefers-reduced-motion") {
        // 本引擎不播放动画，按用户偏好「减少动态」处理。
        return lowered_value.empty() || lowered_value == "reduce";
    }
    if (name == "prefers-color-scheme") {
        return lowered_value == "light" || lowered_value.empty();
    }
    if (name == "hover" || name == "any-hover") {
        return lowered_value == "none";
    }
    if (name == "pointer" || name == "any-pointer") {
        return lowered_value == "fine";
    }
    if (name == "min-resolution" || name == "max-resolution" || name == "resolution") {
        return false;
    }
    return false;
}

/* 媒体查询列表：逗号分隔为「或」；单个查询内 and 连接的条件全为真才成立。 */
bool EvaluateMediaQueryList(const std::string& prelude, float viewport_width,
                            float viewport_height) {
    const std::string lowered = AsciiLower(prelude);
    size_t begin = 0;
    while (begin <= lowered.size()) {
        size_t comma = lowered.find(',', begin);
        if (comma == std::string::npos) {
            comma = lowered.size();
        }
        const std::string query = Trim(lowered.substr(begin, comma - begin));
        begin = comma + 1;
        if (query.empty()) {
            continue;
        }

        bool negated = false;
        bool matched = true;
        bool has_condition = false;
        size_t index = 0;
        while (index < query.size()) {
            // 跳过空白与 and / only / not 等连接词。
            while (index < query.size() &&
                   (query[index] == ' ' || query[index] == '\t' || query[index] == '\n')) {
                index += 1;
            }
            if (index >= query.size()) {
                break;
            }
            if (query[index] == '(') {
                int depth = 0;
                size_t close = index;
                while (close < query.size()) {
                    if (query[close] == '(') {
                        depth += 1;
                    } else if (query[close] == ')') {
                        depth -= 1;
                        if (depth == 0) {
                            break;
                        }
                    }
                    close += 1;
                }
                if (close >= query.size()) {
                    matched = false;
                    break;
                }
                has_condition = true;
                const std::string feature = query.substr(index + 1, close - index - 1);
                if (!EvaluateMediaFeature(feature, viewport_width, viewport_height)) {
                    matched = false;
                }
                index = close + 1;
                continue;
            }
            size_t word_end = index;
            while (word_end < query.size() && IsIdentChar(query[word_end])) {
                word_end += 1;
            }
            const std::string word = query.substr(index, word_end - index);
            if (word.empty()) {
                index += 1;
                continue;
            }
            index = word_end;
            if (word == "and" || word == "only") {
                continue;
            }
            if (word == "not") {
                negated = true;
                continue;
            }
            // 媒体类型：screen / all 成立，print / speech 不成立。
            if (word == "screen" || word == "all") {
                continue;
            }
            matched = false;
        }

        if (!has_condition && !negated) {
            // 仅有媒体类型且不是 screen/all 时前面已置 matched = false。
        }
        if (negated) {
            matched = !matched;
        }
        if (matched) {
            return true;
        }
    }
    return false;
}

/* @supports 条件：只判断括号内声明的属性名是否在支持集合中。 */
bool EvaluateSupportsCondition(const std::string& prelude) {
    const std::string lowered = AsciiLower(prelude);
    size_t index = 0;
    bool matched = true;
    while (index < lowered.size()) {
        if (lowered[index] == '(') {
            int depth = 0;
            size_t close = index;
            while (close < lowered.size()) {
                if (lowered[close] == '(') {
                    depth += 1;
                } else if (lowered[close] == ')') {
                    depth -= 1;
                    if (depth == 0) {
                        break;
                    }
                }
                close += 1;
            }
            if (close >= lowered.size()) {
                return false;
            }
            const std::string declaration = lowered.substr(index + 1, close - index - 1);
            const size_t colon = declaration.find(':');
            if (colon == std::string::npos) {
                return false;
            }
            const std::string property = Trim(declaration.substr(0, colon));
            // 属性名前缀带厂商前缀时同样视为不受支持。
            if (property.empty() || property[0] == '-' || property[0] == '_') {
                return false;
            }
            index = close + 1;
            continue;
        }
        if (lowered[index] == ')') {
            index += 1;
            continue;
        }
        if (lowered[index] == ' ' || lowered[index] == '\n' || lowered[index] == '\t') {
            index += 1;
            continue;
        }
        size_t word_end = index;
        while (word_end < lowered.size() && IsIdentChar(lowered[word_end])) {
            word_end += 1;
        }
        if (word_end == index) {
            index += 1;
            continue;
        }
        const std::string word = lowered.substr(index, word_end - index);
        if (word == "and" || word == "not") {
            index = word_end;
            continue;
        }
        // selector(...)、font-format(...) 等其它条件不支持。
        matched = false;
        index = word_end;
    }
    return matched;
}

/* 把输出缓冲区中被替换的区间补足等量空白换行，维持原始行号。 */
void AppendLinePadding(const std::string& removed, std::string* output) {
    for (size_t index = 0; index < removed.size(); ++index) {
        output->push_back(removed[index] == '\n' ? '\n' : ' ');
    }
}

/* 递归展开 @media / @supports；其余内容原样保留。 */
void ExpandConditionalAtRules(const std::string& source, size_t begin, size_t end,
                              float viewport_width, float viewport_height, int depth,
                              std::string* output) {
    if (depth > 8) {
        output->append(source.substr(begin, end - begin));
        return;
    }
    size_t index = begin;
    size_t plain_start = begin;
    while (index < end) {
        index = SkipNonCode(source, index, end);
        if (index >= end) {
            break;
        }
        if (source[index] != '@') {
            index += 1;
            continue;
        }
        size_t name_end = index + 1;
        while (name_end < end && IsIdentChar(source[name_end])) {
            name_end += 1;
        }
        const std::string name = AsciiLower(source.substr(index + 1, name_end - index - 1));
        if (name != "media" && name != "supports") {
            index = name_end > index ? name_end : index + 1;
            continue;
        }
        const size_t brace = FindCodeChar(source, name_end, end, '{');
        if (brace >= end) {
            break;
        }
        const size_t block_end = FindBlockEnd(source, brace, end);
        const std::string prelude = source.substr(name_end, brace - name_end);
        bool keep = false;
        if (name == "media") {
            keep = viewport_width > 0.0f && viewport_height > 0.0f &&
                   EvaluateMediaQueryList(prelude, viewport_width, viewport_height);
        } else {
            keep = EvaluateSupportsCondition(prelude);
        }

        output->append(source.substr(plain_start, index - plain_start));
        if (keep) {
            const size_t content_begin = brace + 1;
            const size_t content_end = block_end > brace + 1 ? block_end - 1 : content_begin;
            // 保留换行以维持后续行号；块内容递归处理后并入。
            AppendLinePadding(source.substr(index, content_begin - index), output);
            ExpandConditionalAtRules(source, content_begin, content_end, viewport_width,
                                     viewport_height, depth + 1, output);
            if (block_end > content_end) {
                AppendLinePadding(source.substr(content_end, block_end - content_end), output);
            }
        } else {
            AppendLinePadding(source.substr(index, block_end - index), output);
        }
        plain_start = block_end;
        index = block_end;
    }
    if (plain_start < end) {
        output->append(source.substr(plain_start, end - plain_start));
    }
}

/* ----------------------------------------------------------------
   @keyframes 提取

   语法层把未知 at-rule 整块跳过。关键帧动画需要保留名称、百分比与声明，
   因此这里在解析前做一次文本级扫描：读取 @keyframes 名称与每个关键帧的
   选择器、声明块。声明解析复用 ParseDeclarationBlock。
   ---------------------------------------------------------------- */

void ExtractKeyframes(const std::string& source, std::vector<KeyframesRule>* output) {
    const size_t end = source.size();
    size_t index = 0;
    while (index < end) {
        index = SkipNonCode(source, index, end);
        if (index >= end) {
            break;
        }
        if (source[index] != '@') {
            index += 1;
            continue;
        }
        index += 1;
        const size_t name_begin = index;
        while (index < end && IsIdentChar(source[index])) {
            index += 1;
        }
        const std::string rule_name = AsciiLower(source.substr(name_begin, index - name_begin));
        const size_t brace = FindCodeChar(source, index, end, '{');
        if (brace >= end) {
            break;
        }
        const size_t block_end = FindBlockEnd(source, brace, end);
        const bool is_keyframes =
            rule_name == "keyframes" || rule_name == "-webkit-keyframes";
        if (!is_keyframes) {
            index = block_end;
            continue;
        }

        KeyframesRule rule;
        // 动画名称位于 at-rule 名称之后、左花括号之前；先跳过空白。
        size_t name_cursor = SkipNonCode(source, index, brace);
        while (name_cursor < brace && (source[name_cursor] == ' ' || source[name_cursor] == '\t' ||
                                       source[name_cursor] == '\n' || source[name_cursor] == '\r')) {
            name_cursor += 1;
        }
        while (name_cursor < brace && IsIdentChar(source[name_cursor])) {
            rule.name.push_back(source[name_cursor]);
            name_cursor += 1;
        }
        /* 标识符在样式解析阶段统一转为小写，关键帧名称同样处理，
           保证 animation 名称与 @keyframes 名称按同一规则比较。 */
        rule.name = AsciiLower(rule.name);
        const size_t content_end = block_end > brace + 1 ? block_end - 1 : block_end;
        size_t cursor = brace + 1;
        while (cursor < content_end) {
            cursor = SkipNonCode(source, cursor, content_end);
            if (cursor >= content_end) {
                break;
            }
            const size_t selector_begin = cursor;
            while (cursor < content_end && source[cursor] != '{' && source[cursor] != '}') {
                cursor += 1;
            }
            if (cursor >= content_end || source[cursor] != '{') {
                break;
            }
            const std::string selector_text =
                source.substr(selector_begin, cursor - selector_begin);
            const size_t frame_end = FindBlockEnd(source, cursor, content_end);
            if (frame_end <= cursor + 1) {
                cursor = frame_end;
                continue;
            }
            const std::string declarations_text =
                source.substr(cursor + 1, frame_end - cursor - 2);
            std::vector<Declaration> declarations;
            std::string declaration_error;
            ParseDeclarationBlock(declarations_text, &declarations, &declaration_error);

            std::vector<std::string> selectors;
            std::string current;
            for (char character : selector_text) {
                if (character == ',') {
                    selectors.push_back(current);
                    current.clear();
                } else {
                    current.push_back(character);
                }
            }
            selectors.push_back(current);
            for (std::string part : selectors) {
                size_t begin = 0;
                while (begin < part.size() && std::isspace(static_cast<unsigned char>(part[begin]))) {
                    begin += 1;
                }
                size_t finish = part.size();
                while (finish > begin &&
                       std::isspace(static_cast<unsigned char>(part[finish - 1]))) {
                    finish -= 1;
                }
                part = part.substr(begin, finish - begin);
                float offset = 0.0f;
                bool valid = false;
                if (part == "from") {
                    offset = 0.0f;
                    valid = true;
                } else if (part == "to") {
                    offset = 1.0f;
                    valid = true;
                } else if (!part.empty() && part.back() == '%') {
                    char* number_end = nullptr;
                    const float percent = std::strtof(part.c_str(), &number_end);
                    if (number_end != part.c_str() && percent >= 0.0f) {
                        offset = std::min(1.0f, percent / 100.0f);
                        valid = true;
                    }
                }
                if (!valid) {
                    continue;
                }
                Keyframe frame;
                frame.offset = offset;
                frame.declarations = declarations;
                rule.frames.push_back(std::move(frame));
            }
            cursor = frame_end;
        }

        if (!rule.name.empty() && !rule.frames.empty()) {
            std::stable_sort(rule.frames.begin(), rule.frames.end(),
                             [](const Keyframe& left, const Keyframe& right) {
                                 return left.offset < right.offset;
                             });
            output->push_back(std::move(rule));
        }
        index = block_end;
    }
}

}  // namespace

/* ----------------------------------------------------------------
   对外入口
   ---------------------------------------------------------------- */

bool ParseStyleSheet(const std::string& source, StyleSheet* stylesheet, std::string* error) {
    return ParseStyleSheet(source, -1.0f, -1.0f, stylesheet, error);
}

bool ParseStyleSheet(const std::string& source, float viewport_width, float viewport_height,
                     StyleSheet* stylesheet, std::string* error) {
    if (stylesheet == nullptr) {
        if (error != nullptr) {
            *error = "样式表指针为空";
        }
        return false;
    }
    stylesheet->rules.clear();
    stylesheet->keyframes.clear();

    std::string expanded_source;
    const std::string* parse_source = &source;
    if (viewport_width > 0.0f) {
        ExpandConditionalAtRules(source, 0, source.size(), viewport_width, viewport_height, 0,
                                 &expanded_source);
        parse_source = &expanded_source;
    }

    StyleSheet parsed;
    if (!RunParse(*parse_source, &parsed, error)) {
        return false;
    }
    stylesheet->rules = std::move(parsed.rules);
    ExtractKeyframes(*parse_source, &stylesheet->keyframes);
    return true;
}

bool ParseDeclarationBlock(const std::string& source, std::vector<Declaration>* output,
                           std::string* error) {
    if (output == nullptr) {
        if (error != nullptr) {
            *error = "声明输出指针为空";
        }
        return false;
    }

    /* 包装为 "*{...}" 复用正式语法；包装不引入换行，行号与原文保持一致。 */
    std::string wrapped = "*{";
    wrapped += source;
    wrapped += "}";

    StyleSheet sheet;
    if (!RunParse(wrapped, &sheet, error)) {
        return false;
    }
    if (sheet.rules.size() != 1) {
        if (error != nullptr) {
            *error = "声明块中出现了多余的顶层内容";
        }
        return false;
    }
    output->insert(output->end(), sheet.rules[0].declarations.begin(),
                   sheet.rules[0].declarations.end());
    return true;
}

bool ParseSelector(const std::string& source, Selector* selector, std::string* error) {
    if (selector == nullptr) {
        if (error != nullptr) {
            *error = "选择器输出指针为空";
        }
        return false;
    }

    /* 包装为 "选择器 {}" 复用正式语法；空声明块不会产生额外声明。 */
    std::string wrapped = source;
    wrapped += "{}";

    StyleSheet sheet;
    if (!RunParse(wrapped, &sheet, error)) {
        return false;
    }
    if (sheet.rules.size() != 1 || sheet.rules[0].selectors.size() != 1) {
        if (error != nullptr) {
            *error = "选择器必须恰好包含一个复合选择器链";
        }
        return false;
    }
    *selector = sheet.rules[0].selectors[0];
    return true;
}

}  // namespace web

/* ----------------------------------------------------------------
   C 桥接接口实现
   ---------------------------------------------------------------- */

/* 桥接函数在全局作用域实现（与 generate 的 C 代码保持 C 链接），
   这里把内部辅助设施引入全局作用域，避免逐个书写命名空间限定。 */
using web::AcceptsColor;
using web::AsNode;
using web::Combinator;
using web::ConvertItem;
using web::CssValue;
using web::Declaration;
using web::DuplicateCString;
using web::NodeKind;
using web::Rule;
using web::SelectorStep;
using web::SimpleSelector;
using web::Specificity;
using web::StyleSheet;
using web::ValueKind;
using web::WebCssNode;

extern "C" {

void webcss_node_free(void* node) {
    WebCssNode* item = AsNode(node);
    if (item == nullptr) {
        return;
    }
    for (WebCssNode* child : item->raw_items) {
        webcss_node_free(child);
    }
    item->raw_items.clear();
    delete item;
}

char* webcss_text_create(const char* text) {
    std::string content = text != nullptr ? text : "";
    return web::DuplicateCString(content);
}

char* webcss_text_concat_quoted(char* text) {
    std::string content = text != nullptr ? text : "";
    std::free(text);
    std::string quoted = "\"";
    for (size_t index = 0; index < content.size(); ++index) {
        char character = content[index];
        if (character == '"' || character == '\\') {
            quoted.push_back('\\');
        }
        quoted.push_back(character);
    }
    quoted.push_back('"');
    return web::DuplicateCString(quoted);
}

int webcss_attr_flag(char* text) {
    int flag = 0;
    if (text != nullptr && (text[0] == 'i' || text[0] == 'I') && text[1] == '\0') {
        flag = 1;
    }
    std::free(text);
    return flag;
}

void* webcss_simple_create(int kind, char* name, char* value, int case_insensitive) {
    /* 单元素包装成 SimpleList，由 simple_list_prepend 读取其中的 simple。 */
    WebCssNode* node = new WebCssNode();
    node->kind = NodeKind::SimpleList;
    node->simple.kind = static_cast<SimpleSelector::Kind>(kind);
    node->simple.name = name != nullptr ? web::AsciiLower(name) : std::string();
    node->simple.value = value != nullptr ? std::string(value) : std::string();
    node->simple.case_insensitive = case_insensitive != 0;
    node->simples.push_back(node->simple);
    std::free(name);
    std::free(value);
    return node;
}

void* webcss_attribute_create(char* operator_text, char* name, char* value, int case_insensitive) {
    WebCssNode* node = new WebCssNode();
    node->kind = NodeKind::SimpleList;
    SimpleSelector simple;
    simple.kind = SimpleSelector::Kind::Attribute;
    std::string attribute = name != nullptr ? web::AsciiLower(name) : std::string();
    std::string operation = operator_text != nullptr ? std::string(operator_text) : std::string();
    /* 头文件固定了 SimpleSelector 字段，运算符以 name 前缀编码，
       具体规则见 css_selector.cpp 中的 SplitAttributeName。 */
    simple.name = operation + attribute;
    simple.value = value != nullptr ? std::string(value) : std::string();
    simple.case_insensitive = case_insensitive != 0;
    node->simple = simple;
    node->simples.push_back(simple);
    std::free(operator_text);
    std::free(name);
    std::free(value);
    return node;
}

void* webcss_pseudo_create(char* name, void* args) {
    WebCssNode* node = new WebCssNode();
    node->kind = NodeKind::SimpleList;
    SimpleSelector simple;
    simple.kind = SimpleSelector::Kind::PseudoClass;
    simple.name = name != nullptr ? web::AsciiLower(name) : std::string();
    std::string arguments;
    if (args != nullptr) {
        WebCssNode* args_node = AsNode(args);
        arguments = args_node->text;
        webcss_node_free(args);
    }
    simple.value = arguments;
    node->simple = simple;
    node->simples.push_back(simple);
    std::free(name);
    return node;
}

void* webcss_args_create(void) {
    WebCssNode* node = new WebCssNode();
    node->kind = NodeKind::Args;
    return node;
}

void* webcss_args_append(void* args, char* text) {
    WebCssNode* node = AsNode(args);
    if (node == nullptr) {
        std::free(text);
        return nullptr;
    }
    if (text != nullptr) {
        node->text += text;
        std::free(text);
    }
    return node;
}

char* webcss_args_function_text(char* name, void* args) {
    std::string result = name != nullptr ? std::string(name) : std::string();
    result.push_back('(');
    if (args != nullptr) {
        WebCssNode* args_node = AsNode(args);
        result += args_node->text;
        webcss_node_free(args);
    }
    result.push_back(')');
    std::free(name);
    return web::DuplicateCString(result);
}

void* webcss_simple_list_create(void) {
    WebCssNode* node = new WebCssNode();
    node->kind = NodeKind::SimpleList;
    return node;
}

void* webcss_simple_list_prepend(void* simple_selector, void* tail) {
    WebCssNode* simple = AsNode(simple_selector);
    WebCssNode* list = AsNode(tail);
    if (simple == nullptr || list == nullptr) {
        webcss_node_free(simple_selector);
        webcss_node_free(tail);
        return nullptr;
    }
    list->simples.insert(list->simples.begin(), simple->simple);
    webcss_node_free(simple_selector);
    return list;
}

void* webcss_compound_create(void* simple_selector, void* tail) {
    WebCssNode* node = new WebCssNode();
    node->kind = NodeKind::Compound;
    WebCssNode* simple = AsNode(simple_selector);
    if (simple != nullptr) {
        node->compound.simples.push_back(simple->simple);
        webcss_node_free(simple_selector);
    }
    WebCssNode* list = AsNode(tail);
    if (list != nullptr) {
        node->compound.simples.insert(node->compound.simples.end(), list->simples.begin(),
                                      list->simples.end());
        webcss_node_free(tail);
    }
    return node;
}

void* webcss_steps_create(void) {
    WebCssNode* node = new WebCssNode();
    node->kind = NodeKind::Steps;
    return node;
}

void* webcss_steps_prepend(int combinator, void* compound, void* tail) {
    WebCssNode* list = AsNode(tail);
    WebCssNode* compound_node = AsNode(compound);
    if (list == nullptr) {
        webcss_node_free(compound);
        return nullptr;
    }
    SelectorStep step;
    step.combinator = static_cast<Combinator>(combinator);
    if (compound_node != nullptr) {
        step.compound = compound_node->compound;
        webcss_node_free(compound);
    }
    list->steps.insert(list->steps.begin(), step);
    return list;
}

void* webcss_selector_create(void* compound, void* steps) {
    WebCssNode* node = new WebCssNode();
    node->kind = NodeKind::Selector;
    WebCssNode* compound_node = AsNode(compound);
    if (compound_node != nullptr) {
        SelectorStep first;
        first.combinator = Combinator::Descendant;
        first.compound = compound_node->compound;
        node->selector.steps.push_back(first);
        webcss_node_free(compound);
    }
    WebCssNode* step_list = AsNode(steps);
    if (step_list != nullptr) {
        node->selector.steps.insert(node->selector.steps.end(), step_list->steps.begin(),
                                    step_list->steps.end());
        webcss_node_free(steps);
    }
    Specificity total;
    for (const SelectorStep& step : node->selector.steps) {
        Specificity part = step.compound.ComputeSpecificity();
        total.ids += part.ids;
        total.classes += part.classes;
        total.types += part.types;
    }
    node->selector.specificity = total;
    return node;
}

void* webcss_selector_list_create(void* selector) {
    WebCssNode* node = new WebCssNode();
    node->kind = NodeKind::SelectorList;
    WebCssNode* item = AsNode(selector);
    if (item != nullptr) {
        node->selectors.push_back(item->selector);
        webcss_node_free(selector);
    }
    return node;
}

void* webcss_selector_list_append(void* list, void* selector) {
    WebCssNode* node = AsNode(list);
    WebCssNode* item = AsNode(selector);
    if (node == nullptr) {
        webcss_node_free(selector);
        return nullptr;
    }
    if (item != nullptr) {
        node->selectors.push_back(item->selector);
        webcss_node_free(selector);
    }
    return node;
}

void* webcss_value_item(int kind, char* text) {
    WebCssNode* node = new WebCssNode();
    node->kind = NodeKind::Value;
    node->item_kind = kind;
    if (text != nullptr) {
        node->text = text;
        std::free(text);
    }
    return node;
}

void* webcss_value_list_create(void) {
    WebCssNode* node = new WebCssNode();
    node->kind = NodeKind::ValueList;
    return node;
}

void* webcss_value_list_append(void* list, void* item) {
    WebCssNode* node = AsNode(list);
    if (node == nullptr) {
        webcss_node_free(item);
        return nullptr;
    }
    if (item != nullptr) {
        node->raw_items.push_back(AsNode(item));
    }
    return node;
}

void* webcss_value_function(char* name, void* args) {
    WebCssNode* node = new WebCssNode();
    node->kind = NodeKind::Value;
    node->item_kind = WEBCSS_VALUE_FUNCTION;
    std::string function_name = name != nullptr ? std::string(name) : std::string();
    node->text = function_name;
    node->text.push_back('(');
    if (args != nullptr) {
        WebCssNode* args_node = AsNode(args);
        /* 函数实参之间原本的空白在语法层被丢弃，重组文本时若相邻分量的
           首尾都是字母或数字则补一个空格，避免 "to right" 粘成 "toright"、
           "#fff 0%" 粘成 "#fff0%" 而改变语义。 */
        auto is_alnum = [](char character) {
            return (character >= 'a' && character <= 'z') ||
                   (character >= 'A' && character <= 'Z') ||
                   (character >= '0' && character <= '9');
        };
        std::string previous;
        for (WebCssNode* item : args_node->raw_items) {
            /* HASH 分量的文本已去掉前导 #，重组函数文本时需要补回，
               否则 var(--x, #fff) 的回退值会退化成不带 # 的标识。 */
            std::string piece = item->text;
            if (item->item_kind == WEBCSS_VALUE_HASH) {
                piece.insert(piece.begin(), '#');
            }
            if (!previous.empty() && !piece.empty() && is_alnum(previous.back()) &&
                is_alnum(piece.front())) {
                node->text.push_back(' ');
            }
            node->text += piece;
            previous = piece;
        }
        webcss_node_free(args);
    }
    node->text.push_back(')');
    std::free(name);
    return node;
}

void* webcss_declaration_create(struct WebCssParseState* state, char* property, void* value_list,
                                int important) {
    (void)state;
    WebCssNode* node = new WebCssNode();
    node->kind = NodeKind::Declaration;
    std::string name = property != nullptr ? web::AsciiLower(property) : std::string();
    std::free(property);

    std::vector<CssValue> values;
    if (value_list != nullptr) {
        WebCssNode* list = AsNode(value_list);
        bool allow_color = web::AcceptsColor(name);
        for (WebCssNode* item : list->raw_items) {
            if (item == nullptr) {
                continue;
            }
            /* 顶层逗号是值列表分隔符（font-family 等），不作为值分量保留。 */
            if (item->item_kind == WEBCSS_VALUE_DELIM && item->text == ",") {
                continue;
            }
            values.push_back(web::ConvertItem(*item, name, allow_color));
        }
        webcss_node_free(value_list);
    }

    node->declaration.property = name;
    node->declaration.important = important != 0;
    if (values.empty()) {
        node->declaration.value.kind = ValueKind::Keyword;
        node->declaration.value.keyword.clear();
    } else if (values.size() == 1) {
        node->declaration.value = values[0];
    } else {
        node->declaration.value.kind = ValueKind::List;
        node->declaration.value.items = values;
    }
    return node;
}

void* webcss_declaration_list_create(void) {
    WebCssNode* node = new WebCssNode();
    node->kind = NodeKind::DeclarationList;
    return node;
}

void* webcss_declaration_list_append(void* list, void* declaration) {
    WebCssNode* node = AsNode(list);
    WebCssNode* item = AsNode(declaration);
    if (node == nullptr) {
        webcss_node_free(declaration);
        return nullptr;
    }
    if (item != nullptr) {
        node->declarations.push_back(item->declaration);
        webcss_node_free(declaration);
    }
    return node;
}

void webcss_emit_rule(struct WebCssParseState* state, void* selector_list, void* declaration_list) {
    WebCssNode* selectors = AsNode(selector_list);
    WebCssNode* declarations = AsNode(declaration_list);
    StyleSheet* sheet = state != nullptr ? static_cast<StyleSheet*>(state->stylesheet) : nullptr;
    if (sheet != nullptr && selectors != nullptr && declarations != nullptr &&
        !selectors->selectors.empty()) {
        Rule rule;
        rule.selectors = selectors->selectors;
        rule.declarations = declarations->declarations;
        rule.order = sheet->rules.size();
        sheet->rules.push_back(std::move(rule));
    }
    webcss_node_free(selector_list);
    webcss_node_free(declaration_list);
}

void webcss_note_at_rule(struct WebCssParseState* state, char* name, int has_block) {
    (void)has_block;
    if (state != nullptr) {
        state->skipped_at_rules += 1;
    }
    std::free(name);
}

void webcss_note_error(struct WebCssParseState* state, const char* message) {
    if (state == nullptr) {
        return;
    }
    if (state->error_count == 0 && state->error_message[0] == '\0') {
        std::snprintf(state->error_message, sizeof(state->error_message), "%s",
                      message != nullptr ? message : "CSS 语法错误");
        state->error_line = state->token.line;
        state->error_column = state->token.column;
    }
    state->error_count += 1;
}

}  // extern "C"
