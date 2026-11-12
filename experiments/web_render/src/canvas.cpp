// Canvas 2D 模块实现：像素表面注册表、颜色解析、路径光栅化、矩阵变换、渐变、
// 图片绘制与文本绘制。本文件不依赖任何图形后端，绘制结果直接写入 RGBA8 像素缓冲。

#include "web/canvas.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "web/dom.h"
#include "web/image.h"
#include "web/utf8.h"

namespace web {
namespace {

constexpr int kDefaultCanvasWidth = 300;
constexpr int kDefaultCanvasHeight = 150;
// 曲线细分容差与递归深度上限：容差按用户坐标衡量。
constexpr float kCurveTolerance = 0.25f;
constexpr int kCurveDepthLimit = 16;
// 每像素横纵各 4 个子采样点，覆盖率精度为 1/16。
constexpr int kSupersample = 4;
constexpr float kSupersampleWeight =
    1.0f / static_cast<float>(kSupersample * kSupersample);
constexpr float kPi = 3.14159265358979323846f;

// ---------------------------------------------------------------
// 画布注册表、文本服务与图片缓存
// ---------------------------------------------------------------

std::mutex& SurfaceMutex() {
    static std::mutex mutex;
    return mutex;
}

std::unordered_map<const Element*, std::unique_ptr<CanvasSurface>>& Surfaces() {
    static std::unordered_map<const Element*, std::unique_ptr<CanvasSurface>> surfaces;
    return surfaces;
}

std::mutex& TextServiceMutex() {
    static std::mutex mutex;
    return mutex;
}

TextService*& InjectedTextService() {
    static TextService* service = nullptr;
    return service;
}

// 未注入共享实例时惰性创建，进程退出时释放；创建失败后不再重复尝试。
TextService* FallbackCanvasTextService() {
    static std::mutex mutex;
    static std::unique_ptr<TextService> service;
    static bool attempted = false;
    std::lock_guard<std::mutex> lock(mutex);
    if (!attempted) {
        attempted = true;
        service = TextService::Create();
        if (service && !service->Initialize()) {
            service.reset();
        }
    }
    return service.get();
}

TextService* CurrentCanvasTextService() {
    {
        std::lock_guard<std::mutex> lock(TextServiceMutex());
        if (InjectedTextService() != nullptr) {
            return InjectedTextService();
        }
    }
    return FallbackCanvasTextService();
}

// 画布图片缓存：按基准目录解析 src，解码成功的结果在进程内复用。
std::mutex& ImageCacheMutex() {
    static std::mutex mutex;
    return mutex;
}

struct CanvasImageCacheState {
    std::string base_directory;
    std::unordered_map<std::string, std::unique_ptr<ImageData>> images;
    std::unordered_set<std::string> failures;
};

CanvasImageCacheState& CanvasImageCache() {
    static CanvasImageCacheState state;
    return state;
}

// 拼接基准目录与相对路径，保留相对路径中的分隔符写法。
std::string JoinImagePath(const std::string& base_directory, const std::string& relative_path) {
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

bool IsAbsoluteImagePath(const std::string& path) {
    if (path.empty()) {
        return false;
    }
    if (path.size() >= 2 && path[1] == ':') {
        return true;
    }
    if (path.size() >= 2 && path[0] == '\\' && path[1] == '\\') {
        return true;
    }
    return path[0] == '\\' || path[0] == '/';
}

// ---------------------------------------------------------------
// 数值与尺寸解析
// ---------------------------------------------------------------

bool ParseFloatText(const std::string& text, float* value) {
    if (text.empty()) {
        return false;
    }
    const char* begin = text.c_str();
    char* end = nullptr;
    const float parsed = std::strtof(begin, &end);
    if (end == begin || *end != '\0' || !std::isfinite(parsed)) {
        return false;
    }
    *value = parsed;
    return true;
}

bool ParsePositiveFloatText(const std::string& text, float* value) {
    float parsed = 0.0f;
    if (!ParseFloatText(text, &parsed) || parsed <= 0.0f) {
        return false;
    }
    *value = parsed;
    return true;
}

// 解析画布属性尺寸：仅接受十进制整数，非法值或超出范围时使用回退值。
// allow_zero 为真时 0 视为合法尺寸（脚本把画布宽高设为 0）。
int ParseDimensionText(const std::string& value, int fallback, bool allow_zero) {
    std::string text = value;
    TrimAsciiSpace(&text);
    if (text.empty()) {
        return fallback;
    }
    const char* begin = text.c_str();
    char* end = nullptr;
    const long parsed = std::strtol(begin, &end, 10);
    const long minimum = allow_zero ? 0 : 1;
    if (end == begin || *end != '\0' || parsed < minimum || parsed > kMaxCanvasDimension) {
        return fallback;
    }
    return static_cast<int>(parsed);
}

struct CanvasSize {
    int width = kDefaultCanvasWidth;
    int height = kDefaultCanvasHeight;
};

CanvasSize ResolveCanvasSize(const Element& element) {
    CanvasSize size;
    size.width = ParseDimensionText(element.GetAttribute("width"), kDefaultCanvasWidth, true);
    size.height = ParseDimensionText(element.GetAttribute("height"), kDefaultCanvasHeight, true);
    return size;
}

// 按 "16px Microsoft YaHei UI" 解析字号与族名：首段可以是带 px 后缀或纯数字的字号，
// 其余部分为族名；首段不是字号时整串按族名处理，字号保持默认 16px。
void ParseFontText(const std::string& font, float* size, std::string* family) {
    *size = 16.0f;
    family->clear();

    std::string text = font;
    TrimAsciiSpace(&text);
    if (text.empty()) {
        return;
    }

    const size_t separator = text.find_first_of(" \t");
    std::string head = separator == std::string::npos ? text : text.substr(0, separator);
    std::string tail =
        separator == std::string::npos ? std::string() : text.substr(separator + 1);

    float parsed_size = 0.0f;
    bool size_found = false;
    const std::string head_lower = AsciiLower(head);
    if (head_lower.size() > 2 && head_lower.compare(head_lower.size() - 2, 2, "px") == 0) {
        size_found = ParsePositiveFloatText(head.substr(0, head.size() - 2), &parsed_size);
    } else {
        size_found = ParsePositiveFloatText(head, &parsed_size);
    }
    if (size_found) {
        *size = parsed_size;
    } else {
        tail = text;
    }

    std::string family_text = tail;
    TrimAsciiSpace(&family_text);
    if (family_text.empty()) {
        return;
    }
    // 逗号分隔的族名列表只取第一项，TextService 单次只接受一个族名。
    const size_t comma = family_text.find(',');
    if (comma != std::string::npos) {
        family_text = family_text.substr(0, comma);
    }
    TrimAsciiSpace(&family_text);
    if (!family_text.empty() && (family_text.front() == '"' || family_text.front() == '\'')) {
        family_text.erase(family_text.begin());
    }
    if (!family_text.empty() && (family_text.back() == '"' || family_text.back() == '\'')) {
        family_text.pop_back();
    }
    TrimAsciiSpace(&family_text);
    *family = family_text;
}

// ---------------------------------------------------------------
// 颜色解析
// ---------------------------------------------------------------

// 把 0..1 的浮点分量转为 8 位通道值。
uint8_t ToByte(float value) {
    if (!(value > 0.0f)) {
        return 0;
    }
    if (value >= 1.0f) {
        return 255;
    }
    return static_cast<uint8_t>(std::lround(value * 255.0f));
}

float ClampUnit(float value) {
    if (!(value > 0.0f)) {
        return 0.0f;
    }
    if (value > 1.0f) {
        return 1.0f;
    }
    return value;
}

CanvasColor OpaqueBlack() {
    return CanvasColor{};
}

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

bool ParseHexColor(const std::string& text, CanvasColor* color) {
    if (text.size() != 4 && text.size() != 7) {
        return false;
    }
    if (text.front() != '#') {
        return false;
    }
    if (text.size() == 4) {
        int digits[3] = {0, 0, 0};
        for (int index = 0; index < 3; ++index) {
            const int digit = HexDigit(text[static_cast<size_t>(index) + 1]);
            if (digit < 0) {
                return false;
            }
            digits[index] = digit * 17;
        }
        color->red = static_cast<uint8_t>(digits[0]);
        color->green = static_cast<uint8_t>(digits[1]);
        color->blue = static_cast<uint8_t>(digits[2]);
        color->alpha = 255;
        return true;
    }
    int digits[6] = {0, 0, 0, 0, 0, 0};
    for (int index = 0; index < 6; ++index) {
        const int digit = HexDigit(text[static_cast<size_t>(index) + 1]);
        if (digit < 0) {
            return false;
        }
        digits[index] = digit;
    }
    color->red = static_cast<uint8_t>(digits[0] * 16 + digits[1]);
    color->green = static_cast<uint8_t>(digits[2] * 16 + digits[3]);
    color->blue = static_cast<uint8_t>(digits[4] * 16 + digits[5]);
    color->alpha = 255;
    return true;
}

std::vector<std::string> SplitCommaList(const std::string& text) {
    std::vector<std::string> parts;
    size_t begin = 0;
    while (begin <= text.size()) {
        const size_t comma = text.find(',', begin);
        const size_t end = comma == std::string::npos ? text.size() : comma;
        std::string part = text.substr(begin, end - begin);
        TrimAsciiSpace(&part);
        parts.push_back(std::move(part));
        if (comma == std::string::npos) {
            break;
        }
        begin = comma + 1;
    }
    return parts;
}

// 颜色分量：纯数字按 0..最大值解释，带百分号按 0%..100% 解释。
bool ParseColorChannel(const std::string& text, uint8_t* channel) {
    std::string value = text;
    TrimAsciiSpace(&value);
    if (value.empty()) {
        return false;
    }
    bool percent = false;
    if (value.back() == '%') {
        percent = true;
        value.pop_back();
        TrimAsciiSpace(&value);
    }
    float number = 0.0f;
    if (!ParseFloatText(value, &number)) {
        return false;
    }
    const float normalized = percent ? ClampUnit(number / 100.0f) : ClampUnit(number / 255.0f);
    *channel = ToByte(normalized);
    return true;
}

// Alpha 分量：纯数字按 0..1 解释，带百分号按 0%..100% 解释。
bool ParseAlphaChannel(const std::string& text, uint8_t* channel) {
    std::string value = text;
    TrimAsciiSpace(&value);
    if (value.empty()) {
        return false;
    }
    bool percent = false;
    if (value.back() == '%') {
        percent = true;
        value.pop_back();
        TrimAsciiSpace(&value);
    }
    float number = 0.0f;
    if (!ParseFloatText(value, &number)) {
        return false;
    }
    *channel = ToByte(percent ? ClampUnit(number / 100.0f) : ClampUnit(number));
    return true;
}

bool ParseFunctionalColor(const std::string& text, CanvasColor* color) {
    const size_t open = text.find('(');
    if (open == std::string::npos || text.back() != ')') {
        return false;
    }
    const std::string name = text.substr(0, open);
    const std::string arguments = text.substr(open + 1, text.size() - open - 2);
    const std::vector<std::string> parts = SplitCommaList(arguments);

    CanvasColor parsed;
    if (name == "rgb" && parts.size() == 3) {
        if (!ParseColorChannel(parts[0], &parsed.red) ||
            !ParseColorChannel(parts[1], &parsed.green) ||
            !ParseColorChannel(parts[2], &parsed.blue)) {
            return false;
        }
        parsed.alpha = 255;
        *color = parsed;
        return true;
    }
    if (name == "rgba" && parts.size() == 4) {
        if (!ParseColorChannel(parts[0], &parsed.red) ||
            !ParseColorChannel(parts[1], &parsed.green) ||
            !ParseColorChannel(parts[2], &parsed.blue) ||
            !ParseAlphaChannel(parts[3], &parsed.alpha)) {
            return false;
        }
        *color = parsed;
        return true;
    }
    return false;
}

struct NamedColor {
    const char* name;
    uint8_t red;
    uint8_t green;
    uint8_t blue;
    uint8_t alpha;
};

const NamedColor kNamedColors[] = {
    {"black", 0, 0, 0, 255},
    {"white", 255, 255, 255, 255},
    {"red", 255, 0, 0, 255},
    {"green", 0, 128, 0, 255},
    {"blue", 0, 0, 255, 255},
    {"gray", 128, 128, 128, 255},
    {"grey", 128, 128, 128, 255},
    {"transparent", 0, 0, 0, 0},
    {"yellow", 255, 255, 0, 255},
    {"orange", 255, 165, 0, 255},
    {"purple", 128, 0, 128, 255},
    {"cyan", 0, 255, 255, 255},
    {"aqua", 0, 255, 255, 255},
    {"magenta", 255, 0, 255, 255},
    {"fuchsia", 255, 0, 255, 255},
    {"silver", 192, 192, 192, 255},
    {"maroon", 128, 0, 0, 255},
    {"olive", 128, 128, 0, 255},
    {"lime", 0, 255, 0, 255},
    {"teal", 0, 128, 128, 255},
    {"navy", 0, 0, 128, 255},
    {"brown", 165, 42, 42, 255},
    {"pink", 255, 192, 203, 255},
};

const size_t kNamedColorCount = sizeof(kNamedColors) / sizeof(kNamedColors[0]);

bool ParseNamedColor(const std::string& text, CanvasColor* color) {
    for (size_t index = 0; index < kNamedColorCount; ++index) {
        if (text == kNamedColors[index].name) {
            color->red = kNamedColors[index].red;
            color->green = kNamedColors[index].green;
            color->blue = kNamedColors[index].blue;
            color->alpha = kNamedColors[index].alpha;
            return true;
        }
    }
    return false;
}

CanvasColor BlendStopColor(const CanvasColor& from, const CanvasColor& to, float progress) {
    const float weight = ClampUnit(progress);
    CanvasColor result;
    result.red = static_cast<uint8_t>(
        std::lround(static_cast<float>(from.red) +
                    (static_cast<float>(to.red) - static_cast<float>(from.red)) * weight));
    result.green = static_cast<uint8_t>(
        std::lround(static_cast<float>(from.green) +
                    (static_cast<float>(to.green) - static_cast<float>(from.green)) * weight));
    result.blue = static_cast<uint8_t>(
        std::lround(static_cast<float>(from.blue) +
                    (static_cast<float>(to.blue) - static_cast<float>(from.blue)) * weight));
    result.alpha = static_cast<uint8_t>(
        std::lround(static_cast<float>(from.alpha) +
                    (static_cast<float>(to.alpha) - static_cast<float>(from.alpha)) * weight));
    return result;
}

// ---------------------------------------------------------------
// 矩阵运算
// ---------------------------------------------------------------

bool MatrixIsFinite(const CanvasMatrix2D& matrix) {
    return std::isfinite(matrix.a) && std::isfinite(matrix.b) && std::isfinite(matrix.c) &&
           std::isfinite(matrix.d) && std::isfinite(matrix.e) && std::isfinite(matrix.f);
}

CanvasMatrix2D MultiplyMatrices(const CanvasMatrix2D& left, const CanvasMatrix2D& right) {
    CanvasMatrix2D result;
    result.a = left.a * right.a + left.c * right.b;
    result.b = left.b * right.a + left.d * right.b;
    result.c = left.a * right.c + left.c * right.d;
    result.d = left.b * right.c + left.d * right.d;
    result.e = left.a * right.e + left.c * right.f + left.e;
    result.f = left.b * right.e + left.d * right.f + left.f;
    return result;
}

CanvasPathPoint TransformPoint(const CanvasMatrix2D& matrix, float x, float y) {
    CanvasPathPoint result;
    result.x = matrix.a * x + matrix.c * y + matrix.e;
    result.y = matrix.b * x + matrix.d * y + matrix.f;
    return result;
}

bool InvertMatrix(const CanvasMatrix2D& matrix, CanvasMatrix2D* inverse) {
    const float determinant = matrix.a * matrix.d - matrix.b * matrix.c;
    if (!std::isfinite(determinant) || std::fabs(determinant) < 1e-9f) {
        return false;
    }
    CanvasMatrix2D result;
    result.a = matrix.d / determinant;
    result.b = -matrix.b / determinant;
    result.c = -matrix.c / determinant;
    result.d = matrix.a / determinant;
    result.e = (matrix.c * matrix.f - matrix.d * matrix.e) / determinant;
    result.f = (matrix.b * matrix.e - matrix.a * matrix.f) / determinant;
    *inverse = result;
    return true;
}

// 矩阵的平均缩放系数：面积缩放因子的平方根，用于线宽与字号换算。
float MatrixScale(const CanvasMatrix2D& matrix) {
    const float determinant = matrix.a * matrix.d - matrix.b * matrix.c;
    if (!std::isfinite(determinant)) {
        return 0.0f;
    }
    return std::sqrt(std::fabs(determinant));
}

// ---------------------------------------------------------------
// 像素合成
// ---------------------------------------------------------------

// 直通 RGBA 的 source-over 混合：颜色分量与覆盖率共同决定源 Alpha。
void BlendPixel(uint8_t* pixel, float red, float green, float blue, float alpha) {
    if (alpha <= 0.0f) {
        return;
    }
    alpha = std::min(alpha, 1.0f);

    const float destination_alpha = static_cast<float>(pixel[3]) / 255.0f;
    const float output_alpha = alpha + destination_alpha * (1.0f - alpha);
    if (output_alpha <= 0.0f) {
        pixel[0] = 0;
        pixel[1] = 0;
        pixel[2] = 0;
        pixel[3] = 0;
        return;
    }

    const float destination_weight = destination_alpha * (1.0f - alpha);
    pixel[0] = ToByte((red * alpha + static_cast<float>(pixel[0]) / 255.0f * destination_weight) /
                      output_alpha);
    pixel[1] = ToByte((green * alpha + static_cast<float>(pixel[1]) / 255.0f * destination_weight) /
                      output_alpha);
    pixel[2] = ToByte((blue * alpha + static_cast<float>(pixel[2]) / 255.0f * destination_weight) /
                      output_alpha);
    pixel[3] = ToByte(output_alpha);
}

// 按 Porter-Duff 规则合成源像素与目标像素；source-over 走上面的快速路径。
// 颜色分量按非预乘保存，输出时除以结果 Alpha。
void BlendPixelMode(uint8_t* pixel, float red, float green, float blue, float alpha,
                    CanvasCompositeOperation operation) {
    if (pixel == nullptr) {
        return;
    }
    if (operation == CanvasCompositeOperation::SourceOver) {
        BlendPixel(pixel, red, green, blue, alpha);
        return;
    }
    const float source_alpha = std::clamp(alpha, 0.0f, 1.0f);
    const float destination_alpha = static_cast<float>(pixel[3]) / 255.0f;
    float source_factor = 1.0f;
    float destination_factor = 0.0f;
    switch (operation) {
        case CanvasCompositeOperation::SourceOver:
            break;
        case CanvasCompositeOperation::SourceIn:
            source_factor = destination_alpha;
            destination_factor = 0.0f;
            break;
        case CanvasCompositeOperation::SourceOut:
            source_factor = 1.0f - destination_alpha;
            destination_factor = 0.0f;
            break;
        case CanvasCompositeOperation::SourceAtop:
            source_factor = destination_alpha;
            destination_factor = 1.0f - source_alpha;
            break;
        case CanvasCompositeOperation::DestinationOver:
            source_factor = 1.0f - destination_alpha;
            destination_factor = 1.0f;
            break;
        case CanvasCompositeOperation::DestinationIn:
            source_factor = 0.0f;
            destination_factor = source_alpha;
            break;
        case CanvasCompositeOperation::DestinationOut:
            source_factor = 0.0f;
            destination_factor = 1.0f - source_alpha;
            break;
        case CanvasCompositeOperation::DestinationAtop:
            source_factor = 1.0f - destination_alpha;
            destination_factor = source_alpha;
            break;
        case CanvasCompositeOperation::Lighter:
            source_factor = 1.0f;
            destination_factor = 1.0f;
            break;
        case CanvasCompositeOperation::Copy:
            source_factor = 1.0f;
            destination_factor = 0.0f;
            break;
        case CanvasCompositeOperation::Xor:
            source_factor = 1.0f - destination_alpha;
            destination_factor = 1.0f - source_alpha;
            break;
    }
    const float output_alpha = std::min(
        1.0f, source_alpha * source_factor + destination_alpha * destination_factor);
    if (output_alpha <= 0.0f) {
        pixel[0] = 0;
        pixel[1] = 0;
        pixel[2] = 0;
        pixel[3] = 0;
        return;
    }
    const float destination_red = static_cast<float>(pixel[0]) / 255.0f;
    const float destination_green = static_cast<float>(pixel[1]) / 255.0f;
    const float destination_blue = static_cast<float>(pixel[2]) / 255.0f;
    pixel[0] = ToByte((source_alpha * source_factor * red +
                       destination_alpha * destination_factor * destination_red) /
                      output_alpha);
    pixel[1] = ToByte((source_alpha * source_factor * green +
                       destination_alpha * destination_factor * destination_green) /
                      output_alpha);
    pixel[2] = ToByte((source_alpha * source_factor * blue +
                       destination_alpha * destination_factor * destination_blue) /
                      output_alpha);
    pixel[3] = ToByte(output_alpha);
}

// 绘制来源：纯色、渐变或图案。渐变按设备坐标反查用户坐标后取值，
// 图案按创建时变换的逆矩阵映射到图案像素。
struct CanvasPaint {
    CanvasColor color;
    std::shared_ptr<CanvasGradient> gradient;
    std::shared_ptr<CanvasPattern> pattern;
};

CanvasPaint MakeCanvasPaint(const std::string& style,
                            const std::shared_ptr<CanvasGradient>& gradient,
                            const std::shared_ptr<CanvasPattern>& pattern) {
    CanvasPaint paint;
    paint.color = ParseCanvasColor(style);
    paint.gradient = gradient;
    paint.pattern = pattern;
    return paint;
}

CanvasColor SamplePaintDevice(const CanvasPaint& paint, const CanvasMatrix2D& inverse,
                              float device_x, float device_y) {
    if (paint.pattern) {
        return paint.pattern->ColorAtDevice(device_x, device_y);
    }
    if (!paint.gradient) {
        return paint.color;
    }
    const CanvasPathPoint user = TransformPoint(inverse, device_x, device_y);
    return paint.gradient->ColorAt(user.x, user.y);
}

// 覆盖率区域：相对画布左上角的矩形与逐像素覆盖率。
struct CoverageRegion {
    int left = 0;
    int top = 0;
    int width = 0;
    int height = 0;
    std::vector<float> coverage;

    bool Valid() const {
        return width > 0 && height > 0 &&
               coverage.size() == static_cast<size_t>(width) * static_cast<size_t>(height);
    }
};

struct CanvasEdge {
    float from_x = 0.0f;
    float from_y = 0.0f;
    float to_x = 0.0f;
    float to_y = 0.0f;
};

// 生成折线边：close_unclosed 为真时把未闭合的子路径首尾相连（填充语义）。
std::vector<CanvasEdge> BuildEdges(const std::vector<CanvasPathSubPath>& subpaths,
                                   bool close_unclosed) {
    std::vector<CanvasEdge> edges;
    for (const CanvasPathSubPath& subpath : subpaths) {
        const std::vector<CanvasPathPoint>& points = subpath.points;
        for (size_t index = 1; index < points.size(); ++index) {
            CanvasEdge edge;
            edge.from_x = points[index - 1].x;
            edge.from_y = points[index - 1].y;
            edge.to_x = points[index].x;
            edge.to_y = points[index].y;
            edges.push_back(edge);
        }
        if ((subpath.closed || close_unclosed) && points.size() >= 2) {
            const CanvasPathPoint& first = points.front();
            const CanvasPathPoint& last = points.back();
            if (first.x != last.x || first.y != last.y) {
                CanvasEdge edge;
                edge.from_x = last.x;
                edge.from_y = last.y;
                edge.to_x = first.x;
                edge.to_y = first.y;
                edges.push_back(edge);
            }
        }
    }
    return edges;
}

// 描边几何：把折线展开为可填充的多边形集合（线段四边形、连接与端帽）。
// 所有多边形统一为正向缠绕，重叠区域在 nonzero 规则下保持覆盖。

// 去掉相邻重复点；闭合子路径首尾重合时去掉末点。
std::vector<CanvasPathPoint> SimplifyStrokePoints(const std::vector<CanvasPathPoint>& points,
                                                  bool closed) {
    std::vector<CanvasPathPoint> result;
    result.reserve(points.size());
    for (const CanvasPathPoint& point : points) {
        if (!std::isfinite(point.x) || !std::isfinite(point.y)) {
            continue;
        }
        if (!result.empty()) {
            const float delta_x = point.x - result.back().x;
            const float delta_y = point.y - result.back().y;
            if (delta_x * delta_x + delta_y * delta_y <= 1e-12f) {
                continue;
            }
        }
        result.push_back(point);
    }
    if (closed && result.size() >= 2) {
        const float delta_x = result.front().x - result.back().x;
        const float delta_y = result.front().y - result.back().y;
        if (delta_x * delta_x + delta_y * delta_y <= 1e-12f) {
            result.pop_back();
        }
    }
    return result;
}

// 追加一个多边形，按有向面积统一为正向，避免重叠时环绕数相互抵消。
void AddStrokePolygon(std::vector<CanvasPathSubPath>* polygons,
                      std::vector<CanvasPathPoint> points) {
    if (polygons == nullptr || points.size() < 3) {
        return;
    }
    float area = 0.0f;
    for (size_t index = 0; index < points.size(); ++index) {
        const CanvasPathPoint& first = points[index];
        const CanvasPathPoint& second = points[(index + 1) % points.size()];
        area += first.x * second.y - second.x * first.y;
    }
    if (std::fabs(area) <= 1e-9f) {
        return;
    }
    if (area < 0.0f) {
        std::reverse(points.begin(), points.end());
    }
    CanvasPathSubPath subpath;
    subpath.points = std::move(points);
    subpath.closed = true;
    polygons->push_back(std::move(subpath));
}

void AddStrokeCircle(std::vector<CanvasPathSubPath>* polygons, float center_x, float center_y,
                     float radius) {
    if (polygons == nullptr || !(radius > 0.0f)) {
        return;
    }
    constexpr int kCircleSegments = 24;
    std::vector<CanvasPathPoint> points;
    points.reserve(kCircleSegments);
    for (int index = 0; index < kCircleSegments; ++index) {
        const float angle =
            2.0f * kPi * static_cast<float>(index) / static_cast<float>(kCircleSegments);
        points.push_back(CanvasPathPoint{center_x + radius * std::cos(angle),
                                         center_y + radius * std::sin(angle)});
    }
    AddStrokePolygon(polygons, std::move(points));
}

// 连接：previous、current、next 为连续三个顶点，half_width 为线宽一半。
// miter 在斜接长度超过 miter_limit 倍半线宽时退化为 bevel。
void AddStrokeJoin(const CanvasPathPoint& previous, const CanvasPathPoint& current,
                   const CanvasPathPoint& next, float half_width, const std::string& line_join,
                   float miter_limit, std::vector<CanvasPathSubPath>* polygons) {
    const float in_x = current.x - previous.x;
    const float in_y = current.y - previous.y;
    const float out_x = next.x - current.x;
    const float out_y = next.y - current.y;
    const float in_length = std::sqrt(in_x * in_x + in_y * in_y);
    const float out_length = std::sqrt(out_x * out_x + out_y * out_y);
    if (!(in_length > 0.0f) || !(out_length > 0.0f)) {
        return;
    }
    const float in_unit_x = in_x / in_length;
    const float in_unit_y = in_y / in_length;
    const float out_unit_x = out_x / out_length;
    const float out_unit_y = out_y / out_length;
    const float cross = in_unit_x * out_unit_y - in_unit_y * out_unit_x;
    if (std::fabs(cross) <= 1e-6f) {
        return;
    }
    // 转角外侧法线：cross 大于 0 时外侧为右法线。
    const float side = cross > 0.0f ? -1.0f : 1.0f;
    const float first_normal_x = -in_unit_y * side * half_width;
    const float first_normal_y = in_unit_x * side * half_width;
    const float second_normal_x = -out_unit_y * side * half_width;
    const float second_normal_y = out_unit_x * side * half_width;
    const CanvasPathPoint outer_first{current.x + first_normal_x, current.y + first_normal_y};
    const CanvasPathPoint outer_second{current.x + second_normal_x, current.y + second_normal_y};
    if (line_join == "round") {
        AddStrokeCircle(polygons, current.x, current.y, half_width);
        return;
    }
    if (line_join == "miter") {
        float bisector_x = first_normal_x + second_normal_x;
        float bisector_y = first_normal_y + second_normal_y;
        const float bisector_length =
            std::sqrt(bisector_x * bisector_x + bisector_y * bisector_y);
        if (bisector_length > 1e-6f) {
            bisector_x /= bisector_length;
            bisector_y /= bisector_length;
            const float cosine =
                (bisector_x * first_normal_x + bisector_y * first_normal_y) / half_width;
            if (cosine > 1e-6f) {
                const float ratio = 1.0f / cosine;
                if (ratio <= miter_limit) {
                    const CanvasPathPoint tip{
                        current.x + bisector_x * half_width * ratio,
                        current.y + bisector_y * half_width * ratio};
                    AddStrokePolygon(polygons, {current, outer_first, tip, outer_second});
                    return;
                }
            }
        }
    }
    AddStrokePolygon(polygons, {current, outer_first, outer_second});
}

std::vector<CanvasPathSubPath> BuildStrokeOutline(
    const std::vector<CanvasPathSubPath>& subpaths, float half_width,
    const std::string& line_cap, const std::string& line_join, float miter_limit) {
    std::vector<CanvasPathSubPath> polygons;
    if (!(half_width > 0.0f)) {
        return polygons;
    }
    for (const CanvasPathSubPath& subpath : subpaths) {
        const std::vector<CanvasPathPoint> points =
            SimplifyStrokePoints(subpath.points, subpath.closed);
        if (points.empty()) {
            continue;
        }
        if (points.size() == 1) {
            if (!subpath.closed && line_cap == "round") {
                AddStrokeCircle(&polygons, points.front().x, points.front().y, half_width);
            }
            continue;
        }
        const size_t count = points.size();
        const size_t segment_count = subpath.closed ? count : count - 1;
        for (size_t index = 0; index < segment_count; ++index) {
            const CanvasPathPoint& from = points[index];
            const CanvasPathPoint& to = points[(index + 1) % count];
            const float delta_x = to.x - from.x;
            const float delta_y = to.y - from.y;
            const float length = std::sqrt(delta_x * delta_x + delta_y * delta_y);
            if (!(length > 0.0f)) {
                continue;
            }
            const float normal_x = -delta_y / length * half_width;
            const float normal_y = delta_x / length * half_width;
            CanvasPathPoint start = from;
            CanvasPathPoint end = to;
            if (!subpath.closed && line_cap == "square") {
                const float unit_x = delta_x / length * half_width;
                const float unit_y = delta_y / length * half_width;
                if (index == 0) {
                    start.x -= unit_x;
                    start.y -= unit_y;
                }
                if (index + 1 == segment_count) {
                    end.x += unit_x;
                    end.y += unit_y;
                }
            }
            AddStrokePolygon(&polygons,
                             {CanvasPathPoint{start.x + normal_x, start.y + normal_y},
                              CanvasPathPoint{end.x + normal_x, end.y + normal_y},
                              CanvasPathPoint{end.x - normal_x, end.y - normal_y},
                              CanvasPathPoint{start.x - normal_x, start.y - normal_y}});
        }
        if (!subpath.closed && line_cap == "round") {
            AddStrokeCircle(&polygons, points.front().x, points.front().y, half_width);
            AddStrokeCircle(&polygons, points.back().x, points.back().y, half_width);
        }
        if (subpath.closed) {
            for (size_t index = 0; index < count; ++index) {
                AddStrokeJoin(points[(index + count - 1) % count], points[index],
                              points[(index + 1) % count], half_width, line_join, miter_limit,
                              &polygons);
            }
        } else if (count >= 3) {
            for (size_t index = 1; index + 1 < count; ++index) {
                AddStrokeJoin(points[index - 1], points[index], points[index + 1], half_width,
                              line_join, miter_limit, &polygons);
            }
        }
    }
    return polygons;
}

// 虚线切分：按设备坐标中的路径长度切分，返回若干开放子路径。
std::vector<CanvasPathSubPath> ApplyDashPattern(const std::vector<CanvasPathSubPath>& subpaths,
                                                const std::vector<float>& pattern, float offset) {
    std::vector<CanvasPathSubPath> result;
    if (pattern.empty()) {
        return subpaths;
    }
    float total = 0.0f;
    for (float value : pattern) {
        total += value;
    }
    if (!(total > 0.0f)) {
        return subpaths;
    }

    for (const CanvasPathSubPath& subpath : subpaths) {
        const std::vector<CanvasPathPoint> points =
            SimplifyStrokePoints(subpath.points, subpath.closed);
        if (points.size() < 2) {
            continue;
        }
        const size_t count = points.size();
        const size_t segment_count = subpath.closed ? count : count - 1;

        float phase = std::fmod(offset, total);
        if (phase < 0.0f) {
            phase += total;
        }
        size_t pattern_index = 0;
        bool dash_on = true;
        float remaining = pattern[0];
        int guard = 0;
        while (phase >= remaining && guard++ <= static_cast<int>(pattern.size()) * 2 + 2) {
            phase -= remaining;
            pattern_index = (pattern_index + 1) % pattern.size();
            dash_on = !dash_on;
            remaining = pattern[pattern_index];
        }
        remaining -= phase;

        std::vector<CanvasPathPoint> current;
        std::vector<std::vector<CanvasPathPoint>> dashes;
        const bool started_on = dash_on;
        for (size_t index = 0; index < segment_count; ++index) {
            const CanvasPathPoint& from = points[index];
            const CanvasPathPoint& to = points[(index + 1) % count];
            const float delta_x = to.x - from.x;
            const float delta_y = to.y - from.y;
            const float length = std::sqrt(delta_x * delta_x + delta_y * delta_y);
            if (!(length > 0.0f)) {
                continue;
            }
            float travelled = 0.0f;
            while (travelled < length - 1e-6f) {
                if (remaining <= 0.0f) {
                    // 推进到下一个非零样式段。
                    int advance_guard = 0;
                    do {
                        pattern_index = (pattern_index + 1) % pattern.size();
                        dash_on = !dash_on;
                        remaining += pattern[pattern_index];
                    } while (remaining <= 0.0f &&
                             advance_guard++ <= static_cast<int>(pattern.size()) * 2);
                }
                const float step = std::min(remaining, length - travelled);
                const float start_ratio = travelled / length;
                const float end_ratio = (travelled + step) / length;
                if (dash_on) {
                    if (current.empty()) {
                        current.push_back(CanvasPathPoint{from.x + delta_x * start_ratio,
                                                          from.y + delta_y * start_ratio});
                    }
                    current.push_back(CanvasPathPoint{from.x + delta_x * end_ratio,
                                                      from.y + delta_y * end_ratio});
                }
                travelled += step;
                remaining -= step;
                if (remaining <= 1e-6f && dash_on && !current.empty()) {
                    // 实线段结束：立即结束当前虚线子路径，避免跨过空白段连成直线。
                    dashes.push_back(std::move(current));
                    current.clear();
                }
            }
        }
        if (!current.empty()) {
            dashes.push_back(std::move(current));
        }
        // 闭合路径首尾都处于实线段时，两段虚线在起点相接，合并为一段。
        if (subpath.closed && started_on && dash_on && dashes.size() >= 2) {
            std::vector<CanvasPathPoint> merged = std::move(dashes.back());
            dashes.pop_back();
            if (!merged.empty() && !dashes.front().empty()) {
                merged.insert(merged.end(), dashes.front().begin(), dashes.front().end());
                dashes.front() = std::move(merged);
            }
        }
        for (std::vector<CanvasPathPoint>& dash : dashes) {
            if (dash.size() < 2) {
                continue;
            }
            CanvasPathSubPath segment;
            segment.closed = false;
            segment.points = std::move(dash);
            result.push_back(std::move(segment));
        }
    }
    return result;
}

bool ComputeGeometryBounds(const std::vector<CanvasPathSubPath>& subpaths, float padding,
                           int surface_width, int surface_height, int* left, int* top,
                           int* width, int* height) {
    if (subpaths.empty() || surface_width <= 0 || surface_height <= 0) {
        return false;
    }
    float min_x = 0.0f;
    float min_y = 0.0f;
    float max_x = 0.0f;
    float max_y = 0.0f;
    bool first_point = true;
    for (const CanvasPathSubPath& subpath : subpaths) {
        for (const CanvasPathPoint& point : subpath.points) {
            if (!std::isfinite(point.x) || !std::isfinite(point.y)) {
                continue;
            }
            if (first_point) {
                min_x = point.x;
                max_x = point.x;
                min_y = point.y;
                max_y = point.y;
                first_point = false;
            } else {
                min_x = std::min(min_x, point.x);
                max_x = std::max(max_x, point.x);
                min_y = std::min(min_y, point.y);
                max_y = std::max(max_y, point.y);
            }
        }
    }
    if (first_point) {
        return false;
    }
    min_x -= padding;
    min_y -= padding;
    max_x += padding;
    max_y += padding;

    const int first_x = std::max(0, static_cast<int>(std::floor(min_x)));
    const int last_x = std::min(surface_width, static_cast<int>(std::ceil(max_x)));
    const int first_y = std::max(0, static_cast<int>(std::floor(min_y)));
    const int last_y = std::min(surface_height, static_cast<int>(std::ceil(max_y)));
    if (last_x <= first_x || last_y <= first_y) {
        return false;
    }
    *left = first_x;
    *top = first_y;
    *width = last_x - first_x;
    *height = last_y - first_y;
    return true;
}

// 非零缠绕规则扫描线填充：每个像素行取 4 条子扫描线，每条区间按 4 个列子采样累加覆盖率。
bool ComputePolygonCoverage(const std::vector<CanvasPathSubPath>& subpaths, int surface_width,
                            int surface_height, CoverageRegion* region) {
    const std::vector<CanvasEdge> edges = BuildEdges(subpaths, true);
    if (edges.empty()) {
        return false;
    }
    int left = 0;
    int top = 0;
    int width = 0;
    int height = 0;
    if (!ComputeGeometryBounds(subpaths, 0.0f, surface_width, surface_height, &left, &top, &width,
                               &height)) {
        return false;
    }

    region->left = left;
    region->top = top;
    region->width = width;
    region->height = height;
    region->coverage.assign(static_cast<size_t>(width) * static_cast<size_t>(height), 0.0f);

    struct Crossing {
        float x = 0.0f;
        int direction = 0;
    };
    std::vector<Crossing> crossings;
    crossings.reserve(edges.size());

    for (int row = 0; row < height; ++row) {
        for (int sample_row = 0; sample_row < kSupersample; ++sample_row) {
            const float sample_y =
                static_cast<float>(top + row) +
                (static_cast<float>(sample_row) + 0.5f) / static_cast<float>(kSupersample);
            crossings.clear();
            for (const CanvasEdge& edge : edges) {
                int direction = 0;
                if (edge.from_y <= sample_y && sample_y < edge.to_y) {
                    direction = 1;
                } else if (edge.to_y <= sample_y && sample_y < edge.from_y) {
                    direction = -1;
                } else {
                    continue;
                }
                const float span = edge.to_y - edge.from_y;
                if (!std::isfinite(span) || std::fabs(span) < 1e-12f) {
                    continue;
                }
                const float progress = (sample_y - edge.from_y) / span;
                Crossing crossing;
                crossing.x = edge.from_x + (edge.to_x - edge.from_x) * progress;
                crossing.direction = direction;
                crossings.push_back(crossing);
            }
            if (crossings.size() < 2) {
                continue;
            }
            std::sort(crossings.begin(), crossings.end(),
                      [](const Crossing& first, const Crossing& second) {
                          return first.x < second.x;
                      });

            int winding = 0;
            for (size_t index = 0; index < crossings.size(); ++index) {
                winding += crossings[index].direction;
                if (winding == 0 || index + 1 >= crossings.size()) {
                    continue;
                }
                const float begin_x = crossings[index].x;
                const float end_x = crossings[index + 1].x;
                if (!(end_x > begin_x)) {
                    continue;
                }
                const float clamped_begin = std::max(begin_x, static_cast<float>(left));
                const float clamped_end =
                    std::min(end_x, static_cast<float>(left + width));
                if (!(clamped_end > clamped_begin)) {
                    continue;
                }
                const int first_column =
                    std::max(0, static_cast<int>(std::floor(clamped_begin)) - left);
                const int last_column =
                    std::min(width - 1, static_cast<int>(std::ceil(clamped_end)) - left);
                for (int column = first_column; column <= last_column; ++column) {
                    for (int sample_column = 0; sample_column < kSupersample; ++sample_column) {
                        const float sample_x =
                            static_cast<float>(left + column) +
                            (static_cast<float>(sample_column) + 0.5f) /
                                static_cast<float>(kSupersample);
                        if (sample_x >= begin_x && sample_x < end_x) {
                            region->coverage[static_cast<size_t>(row) *
                                                  static_cast<size_t>(width) +
                                              static_cast<size_t>(column)] +=
                                kSupersampleWeight;
                        }
                    }
                }
            }
        }
    }

    // 同一子像素可能被多个区间覆盖，最终覆盖率不超过 1。
    for (float& value : region->coverage) {
        value = std::min(value, 1.0f);
    }
    return true;
}

// 描边覆盖率：先按端点、连接与线宽生成描边轮廓多边形，再按 nonzero 规则
// 计算逐像素覆盖率。轮廓由线段四边形、连接楔形（miter/bevel）或圆盘（round）
// 以及方形/圆形端帽组成，重叠部分保持覆盖。
bool ComputeStrokeCoverage(const std::vector<CanvasPathSubPath>& subpaths, float half_width,
                           const std::string& line_cap, const std::string& line_join,
                           float miter_limit, int surface_width, int surface_height,
                           CoverageRegion* region) {
    const std::vector<CanvasPathSubPath> outline =
        BuildStrokeOutline(subpaths, half_width, line_cap, line_join, miter_limit);
    if (outline.empty()) {
        return false;
    }
    return ComputePolygonCoverage(outline, surface_width, surface_height, region);
}

// 点是否位于路径内部：对变换后的折线边做射线缠绕计数。
bool PointInSubpaths(const std::vector<CanvasPathSubPath>& subpaths, float x, float y,
                     bool even_odd) {
    const std::vector<CanvasEdge> edges = BuildEdges(subpaths, true);
    int winding = 0;
    for (const CanvasEdge& edge : edges) {
        const float cross = (edge.to_x - edge.from_x) * (y - edge.from_y) -
                            (x - edge.from_x) * (edge.to_y - edge.from_y);
        if (edge.from_y <= y) {
            if (edge.to_y > y && cross > 0.0f) {
                ++winding;
            }
        } else if (edge.to_y <= y && cross < 0.0f) {
            --winding;
        }
    }
    return even_odd ? (std::abs(winding) % 2 == 1) : (winding != 0);
}

// 这些合成操作在源 Alpha 为 0 的位置会把目标也置为透明，需要处理源覆盖范围以外的像素。
bool CompositeClearsOutsideSource(CanvasCompositeOperation operation) {
    return operation == CanvasCompositeOperation::SourceIn ||
           operation == CanvasCompositeOperation::SourceOut ||
           operation == CanvasCompositeOperation::DestinationIn ||
           operation == CanvasCompositeOperation::Copy;
}

// 对源覆盖范围为 0 的像素执行零 Alpha 合成；裁剪区域之外的像素保持不动。
void ClearCompositeOutsideSource(CanvasSurface& surface, const CoverageRegion* region,
                                 const CanvasCompositing& compositing) {
    if (!CompositeClearsOutsideSource(compositing.operation)) {
        return;
    }
    for (int device_y = 0; device_y < surface.height; ++device_y) {
        for (int device_x = 0; device_x < surface.width; ++device_x) {
            if (!(compositing.CoverageAt(device_x, device_y) > 0.0f)) {
                continue;
            }
            bool covered = false;
            if (region != nullptr && region->Valid() && device_x >= region->left &&
                device_x < region->left + region->width && device_y >= region->top &&
                device_y < region->top + region->height) {
                const float coverage =
                    region->coverage[static_cast<size_t>(device_y - region->top) *
                                         static_cast<size_t>(region->width) +
                                     static_cast<size_t>(device_x - region->left)];
                covered = coverage > 0.0f;
            }
            if (covered) {
                continue;
            }
            uint8_t* pixel =
                &surface.pixels[(static_cast<size_t>(device_y) * static_cast<size_t>(surface.width) +
                                 static_cast<size_t>(device_x)) *
                                4];
            BlendPixelMode(pixel, 0.0f, 0.0f, 0.0f, 0.0f, compositing.operation);
        }
    }
}

// 按覆盖率把绘制来源合成到表面；渐变逐像素采样，裁剪掩码与全局透明度参与合成。
bool ApplyCoveragePaint(CanvasSurface& surface, const CoverageRegion& region,
                        const CanvasPaint& paint, const CanvasMatrix2D& inverse,
                        const CanvasCompositing& compositing) {
    const bool clears = CompositeClearsOutsideSource(compositing.operation);
    if (!region.Valid() || surface.width <= 0 || surface.height <= 0 ||
        surface.pixels.empty()) {
        return false;
    }
    const bool dynamic_source = paint.gradient != nullptr || paint.pattern != nullptr;
    if ((!(compositing.global_alpha > 0.0f) || (!dynamic_source && paint.color.alpha == 0)) &&
        !clears) {
        return false;
    }
    ClearCompositeOutsideSource(surface, &region, compositing);
    const float plain_red = static_cast<float>(paint.color.red) / 255.0f;
    const float plain_green = static_cast<float>(paint.color.green) / 255.0f;
    const float plain_blue = static_cast<float>(paint.color.blue) / 255.0f;
    const float plain_alpha = static_cast<float>(paint.color.alpha) / 255.0f;

    bool changed = false;
    for (int row = 0; row < region.height; ++row) {
        const int device_y = region.top + row;
        if (device_y < 0 || device_y >= surface.height) {
            continue;
        }
        for (int column = 0; column < region.width; ++column) {
            const float coverage =
                region.coverage[static_cast<size_t>(row) * static_cast<size_t>(region.width) +
                                static_cast<size_t>(column)];
            if (!(coverage > 0.0f)) {
                continue;
            }
            const int device_x = region.left + column;
            if (device_x < 0 || device_x >= surface.width) {
                continue;
            }
            const float clip_coverage = compositing.CoverageAt(device_x, device_y);
            if (!(clip_coverage > 0.0f)) {
                continue;
            }
            uint8_t* pixel =
                &surface.pixels[(static_cast<size_t>(device_y) * static_cast<size_t>(surface.width) +
                                 static_cast<size_t>(device_x)) *
                                4];
            CanvasColor color = paint.color;
            if (dynamic_source) {
                color = SamplePaintDevice(paint, inverse, static_cast<float>(device_x) + 0.5f,
                                          static_cast<float>(device_y) + 0.5f);
                if (color.alpha == 0) {
                    // 源在该像素完全透明；对清除类合成操作仍要参与一次零 Alpha 合成。
                    if (clears) {
                        BlendPixelMode(pixel, 0.0f, 0.0f, 0.0f, 0.0f, compositing.operation);
                        changed = true;
                    }
                    continue;
                }
            }
            const float source_alpha = dynamic_source
                                           ? static_cast<float>(color.alpha) / 255.0f
                                           : plain_alpha;
            const float alpha = source_alpha * coverage * clip_coverage *
                                compositing.global_alpha;
            const float red = dynamic_source ? static_cast<float>(color.red) / 255.0f : plain_red;
            const float green =
                dynamic_source ? static_cast<float>(color.green) / 255.0f : plain_green;
            const float blue =
                dynamic_source ? static_cast<float>(color.blue) / 255.0f : plain_blue;
            BlendPixelMode(pixel, red, green, blue, alpha, compositing.operation);
            changed = true;
        }
    }
    return changed;
}

// 按覆盖率降低目标 Alpha，实现 clearRect 的抗锯齿清除。
bool ApplyCoverageClear(CanvasSurface& surface, const CoverageRegion& region) {
    if (!region.Valid() || surface.width <= 0 || surface.height <= 0 ||
        surface.pixels.empty()) {
        return false;
    }
    bool changed = false;
    for (int row = 0; row < region.height; ++row) {
        const int device_y = region.top + row;
        if (device_y < 0 || device_y >= surface.height) {
            continue;
        }
        for (int column = 0; column < region.width; ++column) {
            const int device_x = region.left + column;
            if (device_x < 0 || device_x >= surface.width) {
                continue;
            }
            const float coverage =
                region.coverage[static_cast<size_t>(row) * static_cast<size_t>(region.width) +
                                static_cast<size_t>(column)];
            if (!(coverage > 0.0f)) {
                continue;
            }
            uint8_t* pixel =
                &surface.pixels[(static_cast<size_t>(device_y) * static_cast<size_t>(surface.width) +
                                 static_cast<size_t>(device_x)) *
                                4];
            const uint8_t next_alpha =
                ToByte(static_cast<float>(pixel[3]) / 255.0f * (1.0f - coverage));
            if (pixel[3] != next_alpha) {
                pixel[3] = next_alpha;
                changed = true;
            }
        }
    }
    return changed;
}

// 双线性采样源像素；x、y 以像素为单位，像素 (i,j) 覆盖 [i,i+1)。
CanvasColor SampleImageBilinear(const uint8_t* pixels, int width, int height, float x, float y) {
    CanvasColor color;
    if (pixels == nullptr || width <= 0 || height <= 0 || !std::isfinite(x) ||
        !std::isfinite(y)) {
        color.alpha = 0;
        return color;
    }
    const float sample_x = std::max(0.0f, std::min(static_cast<float>(width) - 1.0f, x - 0.5f));
    const float sample_y = std::max(0.0f, std::min(static_cast<float>(height) - 1.0f, y - 0.5f));
    const int left = static_cast<int>(std::floor(sample_x));
    const int top = static_cast<int>(std::floor(sample_y));
    const int right = std::min(width - 1, left + 1);
    const int bottom = std::min(height - 1, top + 1);
    const float weight_x = sample_x - static_cast<float>(left);
    const float weight_y = sample_y - static_cast<float>(top);

    float channels[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    const int sample_xs[2] = {left, right};
    const int sample_ys[2] = {top, bottom};
    const float weights_x[2] = {1.0f - weight_x, weight_x};
    const float weights_y[2] = {1.0f - weight_y, weight_y};
    for (int row = 0; row < 2; ++row) {
        for (int column = 0; column < 2; ++column) {
            const uint8_t* pixel =
                &pixels[(static_cast<size_t>(sample_ys[row]) * static_cast<size_t>(width) +
                         static_cast<size_t>(sample_xs[column])) *
                        4];
            const float weight = weights_x[column] * weights_y[row];
            for (int channel = 0; channel < 4; ++channel) {
                channels[channel] += static_cast<float>(pixel[channel]) * weight;
            }
        }
    }
    color.red = static_cast<uint8_t>(std::lround(channels[0]));
    color.green = static_cast<uint8_t>(std::lround(channels[1]));
    color.blue = static_cast<uint8_t>(std::lround(channels[2]));
    color.alpha = static_cast<uint8_t>(std::lround(channels[3]));
    return color;
}

}  // namespace

float CanvasCompositing::CoverageAt(int device_x, int device_y) const {
    if (clip_mask == nullptr || clip_width <= 0 || clip_height <= 0) {
        return 1.0f;
    }
    if (device_x < 0 || device_x >= clip_width || device_y < 0 || device_y >= clip_height) {
        return 0.0f;
    }
    return static_cast<float>(
               clip_mask[static_cast<size_t>(device_y) * static_cast<size_t>(clip_width) +
                         static_cast<size_t>(device_x)]) /
           255.0f;
}

CanvasPattern::CanvasPattern(ImageData image, Repetition repetition, CanvasMatrix2D transform)
    : image_(std::move(image)), repetition_(repetition), transform_(transform) {
    invertible_ = InvertMatrix(transform_, &inverse_);
}

CanvasColor CanvasPattern::ColorAtDevice(float device_x, float device_y) const {
    const CanvasColor transparent{0, 0, 0, 0};
    if (!image_.Valid() || !invertible_) {
        return transparent;
    }
    const CanvasPathPoint user = TransformPoint(inverse_, device_x, device_y);
    const float width = static_cast<float>(image_.width);
    const float height = static_cast<float>(image_.height);
    float sample_x = user.x;
    float sample_y = user.y;
    const auto wrap = [](float value, float extent) {
        float result = std::fmod(value, extent);
        if (result < 0.0f) {
            result += extent;
        }
        return result;
    };
    switch (repetition_) {
        case Repetition::NoRepeat:
            if (sample_x < 0.0f || sample_y < 0.0f || sample_x >= width ||
                sample_y >= height) {
                return transparent;
            }
            break;
        case Repetition::RepeatX:
            if (sample_y < 0.0f || sample_y >= height) {
                return transparent;
            }
            sample_x = wrap(sample_x, width);
            break;
        case Repetition::RepeatY:
            if (sample_x < 0.0f || sample_x >= width) {
                return transparent;
            }
            sample_y = wrap(sample_y, height);
            break;
        case Repetition::Repeat:
            sample_x = wrap(sample_x, width);
            sample_y = wrap(sample_y, height);
            break;
    }
    return SampleImageBilinear(image_.pixels.data(), image_.width, image_.height, sample_x,
                               sample_y);
}

void CanvasSurface::Resize(int new_width, int new_height) {
    if (new_width <= 0 || new_height <= 0) {
        return;
    }
    width = new_width;
    height = new_height;
    pixels.assign(static_cast<size_t>(width) * static_cast<size_t>(height) * 4, 0);
    ++revision;
}

void CanvasSurface::Clear() {
    std::fill(pixels.begin(), pixels.end(), static_cast<uint8_t>(0));
    ++revision;
}

CanvasSurface* AcquireCanvasSurface(const Element& element, int width, int height) {
    const int target_width = std::clamp(width, 0, kMaxCanvasDimension);
    const int target_height = std::clamp(height, 0, kMaxCanvasDimension);

    std::lock_guard<std::mutex> lock(SurfaceMutex());
    auto& surfaces = Surfaces();
    std::unique_ptr<CanvasSurface>& slot = surfaces[&element];
    if (!slot) {
        slot = std::make_unique<CanvasSurface>();
    }
    // 宽或高为 0 时画布没有可绘制像素，保留 0×0 表面占位。
    if (target_width <= 0 || target_height <= 0) {
        if (slot->width != 0 || slot->height != 0 || !slot->pixels.empty()) {
            slot->width = 0;
            slot->height = 0;
            slot->pixels.clear();
            ++slot->revision;
        }
        return slot.get();
    }
    if (slot->width != target_width || slot->height != target_height) {
        slot->Resize(target_width, target_height);
    }
    return slot.get();
}

const CanvasSurface* FindCanvasSurface(const Element& element) {
    std::lock_guard<std::mutex> lock(SurfaceMutex());
    const auto& surfaces = Surfaces();
    const auto found = surfaces.find(&element);
    if (found == surfaces.end()) {
        return nullptr;
    }
    return found->second.get();
}

void ReleaseCanvasSurface(const Element& element) {
    std::lock_guard<std::mutex> lock(SurfaceMutex());
    Surfaces().erase(&element);
}

void ClearCanvasSurfaces() {
    std::lock_guard<std::mutex> lock(SurfaceMutex());
    Surfaces().clear();
}

CanvasPixelSize ResolveCanvasPixelSize(const Element& element) {
    const CanvasSize size = ResolveCanvasSize(element);
    CanvasPixelSize result;
    result.width = size.width;
    result.height = size.height;
    return result;
}

void SyncCanvasSurfaceSize(const Element& element) {
    if (FindCanvasSurface(element) == nullptr) {
        return;
    }
    const CanvasSize size = ResolveCanvasSize(element);
    AcquireCanvasSurface(element, size.width, size.height);
}

void SetCanvasBaseDirectory(std::string base_directory) {
    std::lock_guard<std::mutex> lock(ImageCacheMutex());
    CanvasImageCacheState& cache = CanvasImageCache();
    if (cache.base_directory == base_directory) {
        return;
    }
    cache.base_directory = std::move(base_directory);
    cache.images.clear();
    cache.failures.clear();
}

const ImageData* LoadCanvasImage(const std::string& source) {
    std::string trimmed_source = source;
    TrimAsciiSpace(&trimmed_source);
    if (trimmed_source.empty()) {
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(ImageCacheMutex());
    CanvasImageCacheState& cache = CanvasImageCache();
    const auto cached = cache.images.find(trimmed_source);
    if (cached != cache.images.end()) {
        return cached->second.get();
    }
    if (cache.failures.find(trimmed_source) != cache.failures.end()) {
        return nullptr;
    }

    std::unique_ptr<ImageData> image = std::make_unique<ImageData>();
    std::string error;
    bool loaded = false;
    if (IsDataUri(trimmed_source)) {
        loaded = LoadImageDataUri(trimmed_source, image.get(), &error);
    } else {
        const std::string path = IsAbsoluteImagePath(trimmed_source)
                                     ? trimmed_source
                                     : JoinImagePath(cache.base_directory, trimmed_source);
        loaded = LoadImageFile(path, image.get(), &error);
    }
    if (!loaded || !image->Valid()) {
        cache.failures.insert(trimmed_source);
        return nullptr;
    }
    std::unique_ptr<ImageData>& slot = cache.images[trimmed_source];
    slot = std::move(image);
    return slot.get();
}

void ClearCanvasImageCache() {
    std::lock_guard<std::mutex> lock(ImageCacheMutex());
    CanvasImageCacheState& cache = CanvasImageCache();
    cache.images.clear();
    cache.failures.clear();
}

void SetCanvasTextService(TextService* service) {
    std::lock_guard<std::mutex> lock(TextServiceMutex());
    InjectedTextService() = service;
}

// 解析颜色；无法识别时返回 false，不修改输出。
bool TryParseCanvasColor(const std::string& text, CanvasColor* color) {
    if (color == nullptr) {
        return false;
    }
    std::string value = text;
    TrimAsciiSpace(&value);
    if (value.empty()) {
        return false;
    }
    const std::string lowered = AsciiLower(value);
    return ParseHexColor(lowered, color) || ParseNamedColor(lowered, color) ||
           ParseFunctionalColor(lowered, color);
}

CanvasColor ParseCanvasColor(const std::string& text) {
    CanvasColor color = OpaqueBlack();
    if (TryParseCanvasColor(text, &color)) {
        return color;
    }
    return OpaqueBlack();
}

// ---------------------------------------------------------------
// 渐变
// ---------------------------------------------------------------

CanvasGradient::CanvasGradient(Type type, float x0, float y0, float x1, float y1, float r0,
                               float r1)
    : type_(type), x0_(x0), y0_(y0), x1_(x1), y1_(y1), r0_(r0), r1_(r1) {}

std::shared_ptr<CanvasGradient> CanvasGradient::CreateLinear(float x0, float y0, float x1,
                                                            float y1) {
    return std::shared_ptr<CanvasGradient>(
        new CanvasGradient(Type::Linear, x0, y0, x1, y1, 0.0f, 0.0f));
}

std::shared_ptr<CanvasGradient> CanvasGradient::CreateRadial(float x0, float y0, float r0,
                                                            float x1, float y1, float r1) {
    return std::shared_ptr<CanvasGradient>(
        new CanvasGradient(Type::Radial, x0, y0, x1, y1, r0, r1));
}

void CanvasGradient::AddColorStop(float offset, const std::string& color) {
    if (!std::isfinite(offset)) {
        return;
    }
    CanvasGradientStop stop;
    stop.offset = ClampUnit(offset);
    stop.color = ParseCanvasColor(color);
    stops_.push_back(stop);
    // 相同 offset 时保留先加入的停靠点顺序，因此使用稳定排序。
    std::stable_sort(stops_.begin(), stops_.end(),
                     [](const CanvasGradientStop& first, const CanvasGradientStop& second) {
                         return first.offset < second.offset;
                     });
}

CanvasColor CanvasGradient::ColorAt(float x, float y) const {
    if (stops_.empty()) {
        CanvasColor transparent;
        transparent.alpha = 0;
        return transparent;
    }
    if (stops_.size() == 1) {
        return stops_.front().color;
    }

    float position = 0.0f;
    if (type_ == Type::Linear) {
        const float direction_x = x1_ - x0_;
        const float direction_y = y1_ - y0_;
        const float length_squared =
            direction_x * direction_x + direction_y * direction_y;
        position = length_squared > 1e-12f
                       ? ((x - x0_) * direction_x + (y - y0_) * direction_y) / length_squared
                       : 0.0f;
    } else {
        const float center_x = x1_ - x0_;
        const float center_y = y1_ - y0_;
        const float radius_delta = r1_ - r0_;
        const float point_x = x0_ - x;
        const float point_y = y0_ - y;
        const float quadratic = center_x * center_x + center_y * center_y -
                                radius_delta * radius_delta;
        const float linear =
            -2.0f * (point_x * center_x + point_y * center_y + r0_ * radius_delta);
        const float constant = point_x * point_x + point_y * point_y - r0_ * r0_;
        if (std::fabs(quadratic) < 1e-9f) {
            position = std::fabs(linear) > 1e-9f ? -constant / linear : 0.0f;
        } else {
            const float discriminant = linear * linear - 4.0f * quadratic * constant;
            if (discriminant < 0.0f) {
                position = 0.0f;
            } else {
                const float square_root = std::sqrt(discriminant);
                const float first = (-linear - square_root) / (2.0f * quadratic);
                const float second = (-linear + square_root) / (2.0f * quadratic);
                position = radius_delta >= 0.0f ? std::max(first, second)
                                                : std::min(first, second);
            }
        }
    }

    if (!std::isfinite(position) || position <= stops_.front().offset) {
        return stops_.front().color;
    }
    if (position >= stops_.back().offset) {
        return stops_.back().color;
    }
    for (size_t index = 1; index < stops_.size(); ++index) {
        const CanvasGradientStop& previous = stops_[index - 1];
        const CanvasGradientStop& next = stops_[index];
        if (position > next.offset) {
            continue;
        }
        const float span = next.offset - previous.offset;
        if (!(span > 0.0f)) {
            return next.color;
        }
        return BlendStopColor(previous.color, next.color, (position - previous.offset) / span);
    }
    return stops_.back().color;
}

// ---------------------------------------------------------------
// 绘制上下文
// ---------------------------------------------------------------

CanvasRenderingContext2D::CanvasRenderingContext2D(Element* element) : element_(element) {
    ParseFontText(font_, &font_size_, &font_family_);
    if (element_ != nullptr) {
        const CanvasSize size = ResolveCanvasSize(*element_);
        surface_ = AcquireCanvasSurface(*element_, size.width, size.height);
    }
}

void CanvasRenderingContext2D::SetFillStyle(std::string value) {
    fill_style_ = std::move(value);
    fill_gradient_.reset();
    fill_pattern_.reset();
}

void CanvasRenderingContext2D::SetStrokeStyle(std::string value) {
    stroke_style_ = std::move(value);
    stroke_gradient_.reset();
    stroke_pattern_.reset();
}

void CanvasRenderingContext2D::SetFillGradient(std::shared_ptr<CanvasGradient> gradient) {
    fill_gradient_ = std::move(gradient);
    fill_pattern_.reset();
    fill_style_ = fill_gradient_ ? std::string("[object CanvasGradient]")
                                 : std::string("#000000");
}

void CanvasRenderingContext2D::SetStrokeGradient(std::shared_ptr<CanvasGradient> gradient) {
    stroke_gradient_ = std::move(gradient);
    stroke_pattern_.reset();
    stroke_style_ = stroke_gradient_ ? std::string("[object CanvasGradient]")
                                     : std::string("#000000");
}

void CanvasRenderingContext2D::SetFillPattern(std::shared_ptr<CanvasPattern> pattern) {
    fill_pattern_ = std::move(pattern);
    fill_gradient_.reset();
    fill_style_ = fill_pattern_ ? std::string("[object CanvasPattern]")
                                : std::string("#000000");
}

void CanvasRenderingContext2D::SetStrokePattern(std::shared_ptr<CanvasPattern> pattern) {
    stroke_pattern_ = std::move(pattern);
    stroke_gradient_.reset();
    stroke_style_ = stroke_pattern_ ? std::string("[object CanvasPattern]")
                                    : std::string("#000000");
}

void CanvasRenderingContext2D::SetFont(std::string value) {
    if (font_ == value) {
        return;
    }
    font_ = std::move(value);
    ParseFontText(font_, &font_size_, &font_family_);
    // 字体参数变化后需要重新请求字体句柄。
    font_id_ = 0;
    font_service_ = nullptr;
    scaled_font_id_ = 0;
    scaled_font_service_ = nullptr;
    scaled_font_size_ = 0.0f;
}

void CanvasRenderingContext2D::SetLineWidth(float value) {
    if (!std::isfinite(value) || value <= 0.0f) {
        return;
    }
    line_width_ = value;
}

void CanvasRenderingContext2D::SetGlobalAlpha(float value) {
    if (!std::isfinite(value) || value < 0.0f || value > 1.0f) {
        return;
    }
    global_alpha_ = value;
}

std::string CanvasRenderingContext2D::GlobalCompositeOperation() const {
    switch (composite_operation_) {
        case CanvasCompositeOperation::SourceOver:
            return "source-over";
        case CanvasCompositeOperation::SourceIn:
            return "source-in";
        case CanvasCompositeOperation::SourceOut:
            return "source-out";
        case CanvasCompositeOperation::SourceAtop:
            return "source-atop";
        case CanvasCompositeOperation::DestinationOver:
            return "destination-over";
        case CanvasCompositeOperation::DestinationIn:
            return "destination-in";
        case CanvasCompositeOperation::DestinationOut:
            return "destination-out";
        case CanvasCompositeOperation::DestinationAtop:
            return "destination-atop";
        case CanvasCompositeOperation::Lighter:
            return "lighter";
        case CanvasCompositeOperation::Copy:
            return "copy";
        case CanvasCompositeOperation::Xor:
            return "xor";
    }
    return "source-over";
}

void CanvasRenderingContext2D::SetGlobalCompositeOperation(std::string value) {
    if (value == "source-over") {
        composite_operation_ = CanvasCompositeOperation::SourceOver;
    } else if (value == "source-in") {
        composite_operation_ = CanvasCompositeOperation::SourceIn;
    } else if (value == "source-out") {
        composite_operation_ = CanvasCompositeOperation::SourceOut;
    } else if (value == "source-atop") {
        composite_operation_ = CanvasCompositeOperation::SourceAtop;
    } else if (value == "destination-over") {
        composite_operation_ = CanvasCompositeOperation::DestinationOver;
    } else if (value == "destination-in") {
        composite_operation_ = CanvasCompositeOperation::DestinationIn;
    } else if (value == "destination-out") {
        composite_operation_ = CanvasCompositeOperation::DestinationOut;
    } else if (value == "destination-atop") {
        composite_operation_ = CanvasCompositeOperation::DestinationAtop;
    } else if (value == "lighter") {
        composite_operation_ = CanvasCompositeOperation::Lighter;
    } else if (value == "copy") {
        composite_operation_ = CanvasCompositeOperation::Copy;
    } else if (value == "xor") {
        composite_operation_ = CanvasCompositeOperation::Xor;
    }
}

void CanvasRenderingContext2D::SetShadowColor(std::string value) {
    CanvasColor color;
    if (!TryParseCanvasColor(value, &color)) {
        return;
    }
    shadow_color_ = std::move(value);
    shadow_color_value_ = color;
}

void CanvasRenderingContext2D::SetShadowBlur(float value) {
    if (!std::isfinite(value) || value < 0.0f) {
        return;
    }
    shadow_blur_ = value;
}

void CanvasRenderingContext2D::SetShadowOffsetX(float value) {
    if (!std::isfinite(value)) {
        return;
    }
    shadow_offset_x_ = value;
}

void CanvasRenderingContext2D::SetShadowOffsetY(float value) {
    if (!std::isfinite(value)) {
        return;
    }
    shadow_offset_y_ = value;
}

namespace {

// 三次盒式模糊逼近高斯模糊；边界按 0 延拓（掩码外没有源）。
void BoxBlurMask(std::vector<float>* mask, int width, int height, int radius, int passes) {
    if (mask == nullptr || width <= 0 || height <= 0 || radius <= 0) {
        return;
    }
    std::vector<float> temporary(mask->size(), 0.0f);
    const float scale = 1.0f / static_cast<float>(2 * radius + 1);
    for (int pass = 0; pass < passes; ++pass) {
        // 水平方向
        for (int row = 0; row < height; ++row) {
            float sum = 0.0f;
            for (int offset = -radius; offset <= radius; ++offset) {
                const int column = std::clamp(offset, 0, width - 1);
                sum += (*mask)[static_cast<size_t>(row) * static_cast<size_t>(width) +
                               static_cast<size_t>(column)];
            }
            for (int column = 0; column < width; ++column) {
                temporary[static_cast<size_t>(row) * static_cast<size_t>(width) +
                          static_cast<size_t>(column)] = sum * scale;
                const int add_column = std::clamp(column + radius + 1, 0, width - 1);
                const int remove_column = std::clamp(column - radius, 0, width - 1);
                sum += (*mask)[static_cast<size_t>(row) * static_cast<size_t>(width) +
                               static_cast<size_t>(add_column)] -
                       (*mask)[static_cast<size_t>(row) * static_cast<size_t>(width) +
                               static_cast<size_t>(remove_column)];
            }
        }
        // 垂直方向
        for (int column = 0; column < width; ++column) {
            float sum = 0.0f;
            for (int offset = -radius; offset <= radius; ++offset) {
                const int row = std::clamp(offset, 0, height - 1);
                sum += temporary[static_cast<size_t>(row) * static_cast<size_t>(width) +
                                 static_cast<size_t>(column)];
            }
            for (int row = 0; row < height; ++row) {
                (*mask)[static_cast<size_t>(row) * static_cast<size_t>(width) +
                        static_cast<size_t>(column)] = sum * scale;
                const int add_row = std::clamp(row + radius + 1, 0, height - 1);
                const int remove_row = std::clamp(row - radius, 0, height - 1);
                sum += temporary[static_cast<size_t>(add_row) * static_cast<size_t>(width) +
                                 static_cast<size_t>(column)] -
                       temporary[static_cast<size_t>(remove_row) * static_cast<size_t>(width) +
                                 static_cast<size_t>(column)];
            }
        }
    }
}

// 按覆盖率生成阴影：整数偏移搬移掩码，三次盒式模糊逼近高斯模糊（σ = blur / 2），
// 最后用阴影颜色合成。阴影与源使用同一合成操作。
bool PaintShadowMask(CanvasSurface& surface, const CoverageRegion& coverage,
                     const CanvasCompositing& compositing, const CanvasColor& color,
                     float blur, float offset_x_value, float offset_y_value) {
    if (color.alpha == 0 || !coverage.Valid() || surface.width <= 0 || surface.height <= 0 ||
        surface.pixels.empty()) {
        return false;
    }
    const float sigma = blur * 0.5f;
    int box_radius = 0;
    if (sigma > 0.0f) {
        box_radius = static_cast<int>(
            std::ceil((std::sqrt(4.0f * sigma * sigma + 1.0f) - 1.0f) * 0.5f));
        box_radius = std::max(box_radius, 1);
    }
    const int offset_x = static_cast<int>(std::lround(offset_x_value));
    const int offset_y = static_cast<int>(std::lround(offset_y_value));
    const int margin = box_radius * 3;
    const int left = std::max(0, coverage.left + offset_x - margin);
    const int top = std::max(0, coverage.top + offset_y - margin);
    const int right =
        std::min(surface.width, coverage.left + coverage.width + offset_x + margin);
    const int bottom =
        std::min(surface.height, coverage.top + coverage.height + offset_y + margin);
    if (right <= left || bottom <= top) {
        return false;
    }
    const int width = right - left;
    const int height = bottom - top;
    std::vector<float> mask(static_cast<size_t>(width) * static_cast<size_t>(height), 0.0f);
    for (int row = 0; row < coverage.height; ++row) {
        const int target_y = coverage.top + row + offset_y - top;
        if (target_y < 0 || target_y >= height) {
            continue;
        }
        for (int column = 0; column < coverage.width; ++column) {
            const float value =
                coverage.coverage[static_cast<size_t>(row) * static_cast<size_t>(coverage.width) +
                                  static_cast<size_t>(column)];
            if (!(value > 0.0f)) {
                continue;
            }
            const int target_x = coverage.left + column + offset_x - left;
            if (target_x < 0 || target_x >= width) {
                continue;
            }
            float& slot = mask[static_cast<size_t>(target_y) * static_cast<size_t>(width) +
                               static_cast<size_t>(target_x)];
            slot = std::max(slot, value);
        }
    }
    if (box_radius > 0) {
        BoxBlurMask(&mask, width, height, box_radius, 3);
    }

    const float red = static_cast<float>(color.red) / 255.0f;
    const float green = static_cast<float>(color.green) / 255.0f;
    const float blue = static_cast<float>(color.blue) / 255.0f;
    const float color_alpha = static_cast<float>(color.alpha) / 255.0f;
    bool changed = false;
    for (int row = 0; row < height; ++row) {
        const int device_y = top + row;
        for (int column = 0; column < width; ++column) {
            const float value =
                mask[static_cast<size_t>(row) * static_cast<size_t>(width) +
                     static_cast<size_t>(column)];
            if (!(value > 0.0f)) {
                continue;
            }
            const int device_x = left + column;
            const float clip_coverage = compositing.CoverageAt(device_x, device_y);
            if (!(clip_coverage > 0.0f)) {
                continue;
            }
            const float alpha = value * color_alpha * compositing.global_alpha * clip_coverage;
            if (!(alpha > 0.0f)) {
                continue;
            }
            uint8_t* pixel =
                &surface.pixels[(static_cast<size_t>(device_y) * static_cast<size_t>(surface.width) +
                                 static_cast<size_t>(device_x)) *
                                4];
            BlendPixelMode(pixel, red, green, blue, alpha, compositing.operation);
            changed = true;
        }
    }
    return changed;
}

}  // namespace

void CanvasRenderingContext2D::SetLineCap(std::string value) {
    if (value != "butt" && value != "round" && value != "square") {
        return;
    }
    line_cap_ = std::move(value);
}

void CanvasRenderingContext2D::SetLineJoin(std::string value) {
    if (value != "miter" && value != "round" && value != "bevel") {
        return;
    }
    line_join_ = std::move(value);
}

void CanvasRenderingContext2D::SetMiterLimit(float value) {
    if (!std::isfinite(value) || value < 1.0f) {
        return;
    }
    miter_limit_ = value;
}

void CanvasRenderingContext2D::SetLineDash(std::vector<float> pattern) {
    if (pattern.empty()) {
        line_dash_.clear();
        return;
    }
    float total = 0.0f;
    for (float value : pattern) {
        if (!std::isfinite(value) || value < 0.0f) {
            return;
        }
        total += value;
    }
    if (!(total > 0.0f)) {
        return;
    }
    line_dash_ = std::move(pattern);
}

void CanvasRenderingContext2D::SetLineDashOffset(float value) {
    if (!std::isfinite(value)) {
        return;
    }
    line_dash_offset_ = value;
}

void CanvasRenderingContext2D::SetTextAlign(std::string value) {
    if (value != "start" && value != "end" && value != "left" && value != "right" &&
        value != "center") {
        return;
    }
    text_align_ = std::move(value);
}

void CanvasRenderingContext2D::SetDirection(std::string value) {
    if (value != "ltr" && value != "rtl") {
        return;
    }
    direction_ = std::move(value);
}

void CanvasRenderingContext2D::SetTextBaseline(std::string value) {
    if (value != "alphabetic" && value != "top" && value != "hanging" && value != "middle" &&
        value != "ideographic" && value != "bottom") {
        return;
    }
    text_baseline_ = std::move(value);
}

CanvasCompositing CanvasRenderingContext2D::Compositing() const {
    CanvasCompositing compositing;
    compositing.global_alpha = global_alpha_;
    compositing.operation = composite_operation_;
    if (clip_ != nullptr && !clip_->mask.empty()) {
        compositing.clip_mask = clip_->mask.data();
        compositing.clip_width = clip_->width;
        compositing.clip_height = clip_->height;
    }
    return compositing;
}

bool CanvasRenderingContext2D::DiscardClipForResizedSurface(const CanvasSurface& surface) {
    if (clip_ == nullptr) {
        return false;
    }
    if (clip_->width == surface.width && clip_->height == surface.height) {
        return false;
    }
    clip_.reset();
    return true;
}

CanvasSurface* CanvasRenderingContext2D::RefreshSurface() {
    if (element_ == nullptr) {
        surface_ = nullptr;
        return nullptr;
    }
    // 先确认注册表仍有该元素的表面：元素销毁后不再解引用其地址，也不再重建表面。
    const CanvasSurface* existing = FindCanvasSurface(*element_);
    if (existing == nullptr) {
        surface_ = nullptr;
        return nullptr;
    }
    CanvasSurface* found = const_cast<CanvasSurface*>(existing);
    const CanvasSize size = ResolveCanvasSize(*element_);
    if (found->width != size.width || found->height != size.height) {
        surface_ = AcquireCanvasSurface(*element_, size.width, size.height);
    } else {
        surface_ = found;
    }
    if (surface_ != nullptr) {
        // 画布尺寸变化后原有裁剪掩码不再适用，按规范丢弃裁剪区域。
        DiscardClipForResizedSurface(*surface_);
    }
    return surface_;
}

FontId CanvasRenderingContext2D::ResolveFont(TextService** service_out) {
    TextService* service = CurrentCanvasTextService();
    if (service_out != nullptr) {
        *service_out = service;
    }
    if (service == nullptr) {
        return 0;
    }
    if (font_id_ != 0 && font_service_ == service) {
        return font_id_;
    }

    FontRequest request;
    request.family = font_family_;
    request.size = font_size_;
    font_id_ = service->CreateFont(request);
    font_service_ = service;
    return font_id_;
}

FontId CanvasRenderingContext2D::ResolveFontWithSize(float size, TextService** service_out) {
    TextService* service = CurrentCanvasTextService();
    if (service_out != nullptr) {
        *service_out = service;
    }
    if (service == nullptr || !(size > 0.0f)) {
        return 0;
    }
    if (scaled_font_id_ != 0 && scaled_font_service_ == service &&
        std::fabs(scaled_font_size_ - size) <= 0.01f) {
        return scaled_font_id_;
    }

    FontRequest request;
    request.family = font_family_;
    request.size = size;
    scaled_font_id_ = service->CreateFont(request);
    scaled_font_size_ = size;
    scaled_font_service_ = service;
    return scaled_font_id_;
}

void CanvasRenderingContext2D::FillRect(float x, float y, float width, float height) {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(width) ||
        !std::isfinite(height)) {
        return;
    }
    CanvasSurface* surface = RefreshSurface();
    if (surface == nullptr) {
        return;
    }
    CanvasMatrix2D inverse;
    if (!InvertMatrix(matrix_, &inverse)) {
        return;
    }

    CanvasPathSubPath quad;
    quad.closed = true;
    quad.points.push_back(TransformPoint(matrix_, x, y));
    quad.points.push_back(TransformPoint(matrix_, x + width, y));
    quad.points.push_back(TransformPoint(matrix_, x + width, y + height));
    quad.points.push_back(TransformPoint(matrix_, x, y + height));

    std::vector<CanvasPathSubPath> subpaths;
    subpaths.push_back(std::move(quad));
    CoverageRegion region;
    if (!ComputePolygonCoverage(subpaths, surface->width, surface->height, &region)) {
        return;
    }
    const CanvasPaint paint =
        MakeCanvasPaint(fill_style_, fill_gradient_, fill_pattern_);
    const CanvasCompositing compositing = Compositing();
    bool painted = PaintShadowMask(*surface, region, compositing, shadow_color_value_,
                                   shadow_blur_, shadow_offset_x_, shadow_offset_y_);
    if (ApplyCoveragePaint(*surface, region, paint, inverse, compositing)) {
        painted = true;
    }
    if (painted) {
        ++surface->revision;
    }
}

void CanvasRenderingContext2D::ClearRect(float x, float y, float width, float height) {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(width) ||
        !std::isfinite(height)) {
        return;
    }
    CanvasSurface* surface = RefreshSurface();
    if (surface == nullptr) {
        return;
    }

    CanvasPathSubPath quad;
    quad.closed = true;
    quad.points.push_back(TransformPoint(matrix_, x, y));
    quad.points.push_back(TransformPoint(matrix_, x + width, y));
    quad.points.push_back(TransformPoint(matrix_, x + width, y + height));
    quad.points.push_back(TransformPoint(matrix_, x, y + height));

    std::vector<CanvasPathSubPath> subpaths;
    subpaths.push_back(std::move(quad));
    CoverageRegion region;
    if (!ComputePolygonCoverage(subpaths, surface->width, surface->height, &region)) {
        return;
    }
    if (ApplyCoverageClear(*surface, region)) {
        ++surface->revision;
    }
}

void CanvasRenderingContext2D::StrokeRect(float x, float y, float width, float height) {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(width) ||
        !std::isfinite(height)) {
        return;
    }
    if (!(line_width_ > 0.0f)) {
        return;
    }
    CanvasSurface* surface = RefreshSurface();
    if (surface == nullptr) {
        return;
    }

    CanvasPathSubPath outline;
    outline.closed = true;
    outline.points.push_back(TransformPoint(matrix_, x, y));
    outline.points.push_back(TransformPoint(matrix_, x + width, y));
    outline.points.push_back(TransformPoint(matrix_, x + width, y + height));
    outline.points.push_back(TransformPoint(matrix_, x, y + height));

    std::vector<CanvasPathSubPath> subpaths;
    subpaths.push_back(std::move(outline));
    PaintStroke(*surface, std::move(subpaths));
}

void CanvasRenderingContext2D::FillText(const std::string& text, float x, float baseline_y) {
    if (!std::isfinite(x) || !std::isfinite(baseline_y)) {
        return;
    }
    const float scale = MatrixScale(matrix_);
    if (!(scale > 0.0f)) {
        return;
    }
    CanvasMatrix2D inverse;
    if (!InvertMatrix(matrix_, &inverse)) {
        return;
    }
    TextService* service = nullptr;
    const FontId font = ResolveFontWithSize(font_size_ * scale, &service);
    if (service == nullptr || font == 0) {
        return;
    }
    CanvasSurface* surface = RefreshSurface();
    if (surface == nullptr) {
        return;
    }
    const CanvasPaint paint =
        MakeCanvasPaint(fill_style_, fill_gradient_, fill_pattern_);

    // 基线偏移按设备像素计算：top/hanging 取 ascent，middle 取上下高度的中点，
    // bottom/ideographic 取 descent 下沿。
    const FontMetrics metrics = service->Metrics(font);
    float baseline_offset = 0.0f;
    if (text_baseline_ == "top" || text_baseline_ == "hanging") {
        baseline_offset = metrics.ascent;
    } else if (text_baseline_ == "middle") {
        baseline_offset = (metrics.ascent - metrics.descent) * 0.5f;
    } else if (text_baseline_ == "bottom" || text_baseline_ == "ideographic") {
        baseline_offset = -metrics.descent;
    }
    // start/end 随文本方向翻转；left/right 始终是物理方向。
    // 笔位始终是文本的起始端：ltr 为左端、rtl 为右端，因此 start 的偏移为 0。
    const bool right_to_left = direction_ == "rtl";
    float align_offset = 0.0f;
    const bool needs_text_width =
        text_align_ == "center" || text_align_ == "right" || text_align_ == "left" ||
        text_align_ == "end";
    if (needs_text_width) {
        const float text_width = service->Measure(font, text);
        if (text_align_ == "center") {
            align_offset = -0.5f * text_width;
        } else if (text_align_ == "end") {
            align_offset = right_to_left ? text_width : -text_width;
        } else if (text_align_ == "left") {
            align_offset = right_to_left ? text_width : 0.0f;
        } else if (text_align_ == "right") {
            align_offset = right_to_left ? 0.0f : -text_width;
        }
    }

    // 只有位置与字号参与矩阵变换，字形本身不做旋转：旋转后文字仍保持水平基线。
    CanvasPathPoint origin = TransformPoint(matrix_, x, baseline_y);
    origin.x += align_offset;
    origin.y += baseline_offset;

    // 先收集字形位置，再把整段文本的覆盖率累积到一个区域，最后统一合成一次。
    // 这样 source-in 等需要处理源范围外像素的合成操作只作用于整段文本。
    struct PlacedGlyph {
        GlyphInfo glyph;
        int x = 0;
        int y = 0;
    };
    std::vector<PlacedGlyph> placed;
    int min_x = 1 << 30;
    int min_y = 1 << 30;
    int max_x = -(1 << 30);
    int max_y = -(1 << 30);
    float pen_x = origin.x;
    size_t offset = 0;
    while (offset < text.size()) {
        const Utf8Char character = DecodeUtf8(text, offset);
        if (character.length == 0) {
            break;
        }
        const GlyphInfo glyph = service->Rasterize(font, character.codepoint);
        if (glyph.has_bitmap && glyph.atlas_w > 0 && glyph.atlas_h > 0) {
            PlacedGlyph entry;
            entry.glyph = glyph;
            // 从右向左时以笔位为字形右端，字形向左延伸。
            const float glyph_origin = right_to_left ? pen_x - glyph.advance : pen_x;
            entry.x = static_cast<int>(std::floor(glyph_origin + glyph.bearing_x + 0.5f));
            entry.y = static_cast<int>(std::floor(origin.y - glyph.bearing_y + 0.5f));
            min_x = std::min(min_x, entry.x);
            min_y = std::min(min_y, entry.y);
            max_x = std::max(max_x, entry.x + glyph.atlas_w);
            max_y = std::max(max_y, entry.y + glyph.atlas_h);
            placed.push_back(entry);
        }
        pen_x += right_to_left ? -glyph.advance : glyph.advance;
        offset += character.length;
    }
    if (placed.empty()) {
        return;
    }

    const CanvasCompositing compositing = Compositing();
    const int left = std::max(0, min_x);
    const int top = std::max(0, min_y);
    const int right = std::min(surface->width, max_x);
    const int bottom = std::min(surface->height, max_y);
    if (right <= left || bottom <= top) {
        // 文本完全位于画布之外：清除类合成操作仍按空源处理整块裁剪区域。
        if (CompositeClearsOutsideSource(compositing.operation)) {
            ClearCompositeOutsideSource(*surface, nullptr, compositing);
            ++surface->revision;
        }
        return;
    }

    CoverageRegion region;
    region.left = left;
    region.top = top;
    region.width = right - left;
    region.height = bottom - top;
    region.coverage.assign(static_cast<size_t>(region.width) * static_cast<size_t>(region.height),
                           0.0f);
    for (const PlacedGlyph& entry : placed) {
        // 栅格化可能扩展图集并重新分配覆盖缓冲，这里每次重新读取。
        const uint8_t* atlas = service->AtlasCoverage();
        const int atlas_width = service->AtlasWidth();
        const int atlas_height = service->AtlasHeight();
        if (atlas == nullptr) {
            break;
        }
        for (int row = 0; row < entry.glyph.atlas_h; ++row) {
            const int target_y = entry.y + row;
            if (target_y < top || target_y >= bottom) {
                continue;
            }
            const int atlas_row = entry.glyph.atlas_y + row;
            if (atlas_row < 0 || atlas_row >= atlas_height) {
                continue;
            }
            for (int column = 0; column < entry.glyph.atlas_w; ++column) {
                const int target_x = entry.x + column;
                if (target_x < left || target_x >= right) {
                    continue;
                }
                const int atlas_column = entry.glyph.atlas_x + column;
                if (atlas_column < 0 || atlas_column >= atlas_width) {
                    continue;
                }
                const uint8_t value =
                    atlas[static_cast<size_t>(atlas_row) * static_cast<size_t>(atlas_width) +
                          static_cast<size_t>(atlas_column)];
                if (value == 0) {
                    continue;
                }
                float& target =
                    region.coverage[static_cast<size_t>(target_y - top) *
                                        static_cast<size_t>(region.width) +
                                    static_cast<size_t>(target_x - left)];
                target = std::max(target, static_cast<float>(value) / 255.0f);
            }
        }
    }
    bool painted = PaintShadowMask(*surface, region, compositing, shadow_color_value_,
                                   shadow_blur_, shadow_offset_x_, shadow_offset_y_);
    if (ApplyCoveragePaint(*surface, region, paint, inverse, compositing)) {
        painted = true;
    }
    if (painted) {
        ++surface->revision;
    }
}

float CanvasRenderingContext2D::MeasureText(const std::string& text) {
    TextService* service = nullptr;
    const FontId font = ResolveFont(&service);
    if (service == nullptr || font == 0) {
        return 0.0f;
    }
    return service->Measure(font, text);
}

// ---------------------------------------------------------------
// 路径录入
// ---------------------------------------------------------------

void CanvasRenderingContext2D::AppendPathPoint(float x, float y) {
    if (path_.empty()) {
        CanvasPathSubPath subpath;
        subpath.points.push_back(CanvasPathPoint{x, y});
        path_.push_back(std::move(subpath));
    } else {
        path_.back().points.push_back(CanvasPathPoint{x, y});
    }
    current_point_ = CanvasPathPoint{x, y};
    has_current_point_ = true;
}

void CanvasRenderingContext2D::BeginPath() {
    path_.clear();
    has_current_point_ = false;
    current_point_ = CanvasPathPoint{};
    subpath_start_ = CanvasPathPoint{};
}

void CanvasRenderingContext2D::MoveTo(float x, float y) {
    if (!std::isfinite(x) || !std::isfinite(y)) {
        return;
    }
    CanvasPathSubPath subpath;
    subpath.points.push_back(CanvasPathPoint{x, y});
    path_.push_back(std::move(subpath));
    current_point_ = CanvasPathPoint{x, y};
    subpath_start_ = current_point_;
    has_current_point_ = true;
}

void CanvasRenderingContext2D::LineTo(float x, float y) {
    if (!std::isfinite(x) || !std::isfinite(y)) {
        return;
    }
    if (!has_current_point_) {
        MoveTo(x, y);
        return;
    }
    AppendPathPoint(x, y);
}

void CanvasRenderingContext2D::FlattenQuadratic(const CanvasPathPoint& from,
                                                const CanvasPathPoint& control,
                                                const CanvasPathPoint& to, int depth) {
    const float chord_x = to.x - from.x;
    const float chord_y = to.y - from.y;
    const float control_x = control.x - from.x;
    const float control_y = control.y - from.y;
    // 控制点到弦的偏离量近似为曲线扁平程度。
    const float deviation =
        std::fabs(control_x * chord_y - control_y * chord_x) /
        std::sqrt(chord_x * chord_x + chord_y * chord_y + 1e-12f);
    const float midpoint_tolerance = std::sqrt(
        (control_x - chord_x * 0.5f) * (control_x - chord_x * 0.5f) +
        (control_y - chord_y * 0.5f) * (control_y - chord_y * 0.5f));
    if (depth >= kCurveDepthLimit ||
        (deviation <= kCurveTolerance && midpoint_tolerance <= kCurveTolerance * 2.0f)) {
        AppendPathPoint(to.x, to.y);
        return;
    }

    const CanvasPathPoint start_control{
        from.x + (control.x - from.x) * 0.5f,
        from.y + (control.y - from.y) * 0.5f,
    };
    const CanvasPathPoint end_control{
        control.x + (to.x - control.x) * 0.5f,
        control.y + (to.y - control.y) * 0.5f,
    };
    const CanvasPathPoint midpoint{
        start_control.x + (end_control.x - start_control.x) * 0.5f,
        start_control.y + (end_control.y - start_control.y) * 0.5f,
    };
    FlattenQuadratic(from, start_control, midpoint, depth + 1);
    FlattenQuadratic(midpoint, end_control, to, depth + 1);
}

void CanvasRenderingContext2D::FlattenCubic(const CanvasPathPoint& from,
                                            const CanvasPathPoint& control1,
                                            const CanvasPathPoint& control2,
                                            const CanvasPathPoint& to, int depth) {
    // 两个控制点到弦的距离取较大值作为扁平程度。
    const float chord_x = to.x - from.x;
    const float chord_y = to.y - from.y;
    const float chord_length = std::sqrt(chord_x * chord_x + chord_y * chord_y + 1e-12f);
    const float deviation1 =
        std::fabs((control1.x - from.x) * chord_y - (control1.y - from.y) * chord_x) /
        chord_length;
    const float deviation2 =
        std::fabs((control2.x - from.x) * chord_y - (control2.y - from.y) * chord_x) /
        chord_length;
    if (depth >= kCurveDepthLimit ||
        std::max(deviation1, deviation2) <= kCurveTolerance) {
        AppendPathPoint(to.x, to.y);
        return;
    }

    // de Casteljau 对分：得到两段三次贝塞尔的控制点与中点。
    const CanvasPathPoint first_split{
        from.x + (control1.x - from.x) * 0.5f,
        from.y + (control1.y - from.y) * 0.5f,
    };
    const CanvasPathPoint middle_split{
        control1.x + (control2.x - control1.x) * 0.5f,
        control1.y + (control2.y - control1.y) * 0.5f,
    };
    const CanvasPathPoint second_split{
        control2.x + (to.x - control2.x) * 0.5f,
        control2.y + (to.y - control2.y) * 0.5f,
    };
    const CanvasPathPoint left_control{
        first_split.x + (middle_split.x - first_split.x) * 0.5f,
        first_split.y + (middle_split.y - first_split.y) * 0.5f,
    };
    const CanvasPathPoint right_control{
        middle_split.x + (second_split.x - middle_split.x) * 0.5f,
        middle_split.y + (second_split.y - middle_split.y) * 0.5f,
    };
    const CanvasPathPoint midpoint{
        left_control.x + (right_control.x - left_control.x) * 0.5f,
        left_control.y + (right_control.y - left_control.y) * 0.5f,
    };
    FlattenCubic(from, first_split, left_control, midpoint, depth + 1);
    FlattenCubic(midpoint, right_control, second_split, to, depth + 1);
}

void CanvasRenderingContext2D::QuadraticCurveTo(float control_x, float control_y, float x,
                                                float y) {
    if (!std::isfinite(control_x) || !std::isfinite(control_y) || !std::isfinite(x) ||
        !std::isfinite(y)) {
        return;
    }
    if (!has_current_point_) {
        // 无当前点时按规范以控制点作为起点。
        CanvasPathSubPath subpath;
        subpath.points.push_back(CanvasPathPoint{control_x, control_y});
        path_.push_back(std::move(subpath));
        current_point_ = CanvasPathPoint{control_x, control_y};
        subpath_start_ = current_point_;
        has_current_point_ = true;
    }
    const CanvasPathPoint from = current_point_;
    const CanvasPathPoint control{control_x, control_y};
    const CanvasPathPoint to{x, y};
    FlattenQuadratic(from, control, to, 0);
}

void CanvasRenderingContext2D::BezierCurveTo(float control1_x, float control1_y,
                                             float control2_x, float control2_y, float x,
                                             float y) {
    if (!std::isfinite(control1_x) || !std::isfinite(control1_y) ||
        !std::isfinite(control2_x) || !std::isfinite(control2_y) || !std::isfinite(x) ||
        !std::isfinite(y)) {
        return;
    }
    if (!has_current_point_) {
        CanvasPathSubPath subpath;
        subpath.points.push_back(CanvasPathPoint{control1_x, control1_y});
        path_.push_back(std::move(subpath));
        current_point_ = CanvasPathPoint{control1_x, control1_y};
        subpath_start_ = current_point_;
        has_current_point_ = true;
    }
    const CanvasPathPoint from = current_point_;
    const CanvasPathPoint control1{control1_x, control1_y};
    const CanvasPathPoint control2{control2_x, control2_y};
    const CanvasPathPoint to{x, y};
    FlattenCubic(from, control1, control2, to, 0);
}

void CanvasRenderingContext2D::Arc(float x, float y, float radius, float start_angle,
                                   float end_angle, bool counterclockwise) {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(radius) ||
        !std::isfinite(start_angle) || !std::isfinite(end_angle) || !(radius > 0.0f)) {
        return;
    }
    // 方向归一化：顺时针取正的角增量，逆时针取负的角增量。
    float delta = end_angle - start_angle;
    if (counterclockwise) {
        while (delta > 0.0f) {
            delta -= 2.0f * kPi;
        }
    } else {
        while (delta < 0.0f) {
            delta += 2.0f * kPi;
        }
    }

    const CanvasPathPoint arc_start{
        x + radius * std::cos(start_angle),
        y + radius * std::sin(start_angle),
    };
    if (!has_current_point_) {
        CanvasPathSubPath subpath;
        subpath.points.push_back(arc_start);
        path_.push_back(std::move(subpath));
        subpath_start_ = arc_start;
        has_current_point_ = true;
    } else if (std::fabs(current_point_.x - arc_start.x) > 1e-5f ||
               std::fabs(current_point_.y - arc_start.y) > 1e-5f) {
        LineTo(arc_start.x, arc_start.y);
    }
    if (std::fabs(delta) < 1e-6f) {
        current_point_ = arc_start;
        return;
    }

    // 按圆弧扁平误差计算角步长，单段不超过四分之一圆周。
    float max_step = kPi * 0.5f;
    if (radius > kCurveTolerance) {
        max_step = 2.0f * std::acos(1.0f - kCurveTolerance / radius);
    }
    max_step = std::max(0.01f, std::min(max_step, kPi * 0.5f));
    const int steps =
        std::min(512, std::max(2, static_cast<int>(std::ceil(std::fabs(delta) / max_step))));
    for (int step = 1; step <= steps; ++step) {
        const float angle = start_angle + delta * static_cast<float>(step) /
                                               static_cast<float>(steps);
        AppendPathPoint(x + radius * std::cos(angle), y + radius * std::sin(angle));
    }
}

void CanvasRenderingContext2D::Rect(float x, float y, float width, float height) {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(width) ||
        !std::isfinite(height)) {
        return;
    }
    CanvasPathSubPath subpath;
    subpath.closed = true;
    subpath.points.push_back(CanvasPathPoint{x, y});
    subpath.points.push_back(CanvasPathPoint{x + width, y});
    subpath.points.push_back(CanvasPathPoint{x + width, y + height});
    subpath.points.push_back(CanvasPathPoint{x, y + height});
    path_.push_back(std::move(subpath));
    current_point_ = CanvasPathPoint{x, y};
    subpath_start_ = current_point_;
    has_current_point_ = true;
}

void CanvasRenderingContext2D::Ellipse(float x, float y, float radius_x, float radius_y,
                                       float rotation, float start_angle, float end_angle,
                                       bool counterclockwise) {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(radius_x) ||
        !std::isfinite(radius_y) || !std::isfinite(rotation) || !std::isfinite(start_angle) ||
        !std::isfinite(end_angle) || radius_x < 0.0f || radius_y < 0.0f) {
        return;
    }
    if (radius_x == 0.0f || radius_y == 0.0f) {
        LineTo(x, y);
        return;
    }
    // 方向归一化与 Arc 一致：顺时针取正的角增量，逆时针取负的角增量。
    float delta = end_angle - start_angle;
    if (counterclockwise) {
        while (delta > 0.0f) {
            delta -= 2.0f * kPi;
        }
    } else {
        while (delta < 0.0f) {
            delta += 2.0f * kPi;
        }
    }

    const float cos_rotation = std::cos(rotation);
    const float sin_rotation = std::sin(rotation);
    const auto point_at = [&](float angle) {
        const float local_x = radius_x * std::cos(angle);
        const float local_y = radius_y * std::sin(angle);
        return CanvasPathPoint{x + local_x * cos_rotation - local_y * sin_rotation,
                               y + local_x * sin_rotation + local_y * cos_rotation};
    };

    const CanvasPathPoint ellipse_start = point_at(start_angle);
    if (!has_current_point_) {
        CanvasPathSubPath subpath;
        subpath.points.push_back(ellipse_start);
        path_.push_back(std::move(subpath));
        subpath_start_ = ellipse_start;
        has_current_point_ = true;
    } else if (std::fabs(current_point_.x - ellipse_start.x) > 1e-5f ||
               std::fabs(current_point_.y - ellipse_start.y) > 1e-5f) {
        LineTo(ellipse_start.x, ellipse_start.y);
    }
    if (std::fabs(delta) < 1e-6f) {
        current_point_ = ellipse_start;
        return;
    }

    const float max_radius = std::max(radius_x, radius_y);
    float max_step = kPi * 0.5f;
    if (max_radius > kCurveTolerance) {
        max_step = 2.0f * std::acos(1.0f - kCurveTolerance / max_radius);
    }
    max_step = std::max(0.01f, std::min(max_step, kPi * 0.5f));
    const int steps =
        std::min(512, std::max(2, static_cast<int>(std::ceil(std::fabs(delta) / max_step))));
    for (int step = 1; step <= steps; ++step) {
        const float angle =
            start_angle + delta * static_cast<float>(step) / static_cast<float>(steps);
        const CanvasPathPoint point = point_at(angle);
        AppendPathPoint(point.x, point.y);
    }
}

void CanvasRenderingContext2D::ArcTo(float x1, float y1, float x2, float y2, float radius) {
    if (!std::isfinite(x1) || !std::isfinite(y1) || !std::isfinite(x2) || !std::isfinite(y2) ||
        !std::isfinite(radius) || radius < 0.0f) {
        return;
    }
    if (!has_current_point_) {
        MoveTo(x1, y1);
        return;
    }
    if (radius == 0.0f) {
        LineTo(x1, y1);
        return;
    }

    float in_x = current_point_.x - x1;
    float in_y = current_point_.y - y1;
    float out_x = x2 - x1;
    float out_y = y2 - y1;
    const float in_length = std::sqrt(in_x * in_x + in_y * in_y);
    const float out_length = std::sqrt(out_x * out_x + out_y * out_y);
    if (!(in_length > 1e-6f) || !(out_length > 1e-6f)) {
        LineTo(x1, y1);
        return;
    }
    in_x /= in_length;
    in_y /= in_length;
    out_x /= out_length;
    out_y /= out_length;
    const float cosine = std::clamp(in_x * out_x + in_y * out_y, -1.0f, 1.0f);
    const float angle = std::acos(cosine);
    if (angle <= 1e-4f || angle >= kPi - 1e-4f) {
        LineTo(x1, y1);
        return;
    }

    // 切点沿两条边距控制点 radius / tan(angle/2)；圆心位于角平分线上。
    const float tangent_distance = radius / std::tan(angle * 0.5f);
    const CanvasPathPoint tangent_in{x1 + in_x * tangent_distance,
                                     y1 + in_y * tangent_distance};
    const CanvasPathPoint tangent_out{x1 + out_x * tangent_distance,
                                      y1 + out_y * tangent_distance};
    float bisector_x = in_x + out_x;
    float bisector_y = in_y + out_y;
    const float bisector_length = std::sqrt(bisector_x * bisector_x + bisector_y * bisector_y);
    if (!(bisector_length > 1e-6f)) {
        LineTo(x1, y1);
        return;
    }
    bisector_x /= bisector_length;
    bisector_y /= bisector_length;
    const float center_distance = radius / std::sin(angle * 0.5f);
    const CanvasPathPoint center{x1 + bisector_x * center_distance,
                                 y1 + bisector_y * center_distance};

    LineTo(tangent_in.x, tangent_in.y);
    const float start_angle = std::atan2(tangent_in.y - center.y, tangent_in.x - center.x);
    const float end_angle = std::atan2(tangent_out.y - center.y, tangent_out.x - center.x);
    float delta = end_angle - start_angle;
    while (delta > kPi) {
        delta -= 2.0f * kPi;
    }
    while (delta < -kPi) {
        delta += 2.0f * kPi;
    }

    float max_step = kPi * 0.5f;
    if (radius > kCurveTolerance) {
        max_step = 2.0f * std::acos(1.0f - kCurveTolerance / radius);
    }
    max_step = std::max(0.01f, std::min(max_step, kPi * 0.5f));
    const int steps =
        std::min(256, std::max(2, static_cast<int>(std::ceil(std::fabs(delta) / max_step))));
    for (int step = 1; step <= steps; ++step) {
        const float step_angle =
            start_angle + delta * static_cast<float>(step) / static_cast<float>(steps);
        AppendPathPoint(center.x + radius * std::cos(step_angle),
                        center.y + radius * std::sin(step_angle));
    }
}

void CanvasRenderingContext2D::ClosePath() {
    if (path_.empty() || !has_current_point_) {
        return;
    }
    CanvasPathSubPath& subpath = path_.back();
    if (subpath.points.empty()) {
        return;
    }
    const CanvasPathPoint& first = subpath.points.front();
    const CanvasPathPoint& last = subpath.points.back();
    if (first.x != last.x || first.y != last.y) {
        subpath.points.push_back(first);
    }
    subpath.closed = true;
    current_point_ = first;
    subpath_start_ = first;
}

std::vector<CanvasPathSubPath> CanvasRenderingContext2D::DeviceSubPaths() const {
    std::vector<CanvasPathSubPath> result;
    result.reserve(path_.size());
    for (const CanvasPathSubPath& subpath : path_) {
        CanvasPathSubPath device;
        device.closed = subpath.closed;
        device.points.reserve(subpath.points.size());
        for (const CanvasPathPoint& point : subpath.points) {
            device.points.push_back(TransformPoint(matrix_, point.x, point.y));
        }
        result.push_back(std::move(device));
    }
    return result;
}

bool CanvasRenderingContext2D::PaintStroke(CanvasSurface& surface,
                                           std::vector<CanvasPathSubPath> subpaths) {
    if (!(line_width_ > 0.0f) || subpaths.empty()) {
        return false;
    }
    const float scale = MatrixScale(matrix_);
    if (!(scale > 0.0f)) {
        return false;
    }
    CanvasMatrix2D inverse;
    if (!InvertMatrix(matrix_, &inverse)) {
        return false;
    }
    if (!line_dash_.empty()) {
        // 虚线长度按用户坐标给出，先换算到设备像素。
        std::vector<float> pattern = line_dash_;
        for (float& value : pattern) {
            value *= scale;
        }
        subpaths = ApplyDashPattern(subpaths, pattern, line_dash_offset_ * scale);
        if (subpaths.empty()) {
            return false;
        }
    }
    const std::vector<CanvasPathSubPath> outline = BuildStrokeOutline(
        subpaths, line_width_ * scale * 0.5f, line_cap_, line_join_, miter_limit_);
    if (outline.empty()) {
        return false;
    }
    CoverageRegion region;
    if (!ComputePolygonCoverage(outline, surface.width, surface.height, &region)) {
        return false;
    }
    const CanvasPaint paint =
        MakeCanvasPaint(stroke_style_, stroke_gradient_, stroke_pattern_);
    const CanvasCompositing compositing = Compositing();
    bool painted = PaintShadowMask(surface, region, compositing, shadow_color_value_,
                                   shadow_blur_, shadow_offset_x_, shadow_offset_y_);
    if (ApplyCoveragePaint(surface, region, paint, inverse, compositing)) {
        painted = true;
    }
    if (painted) {
        ++surface.revision;
    }
    return painted;
}

void CanvasRenderingContext2D::Fill() {
    CanvasSurface* surface = RefreshSurface();
    if (surface == nullptr || path_.empty()) {
        return;
    }
    CanvasMatrix2D inverse;
    if (!InvertMatrix(matrix_, &inverse)) {
        return;
    }
    const std::vector<CanvasPathSubPath> subpaths = DeviceSubPaths();
    CoverageRegion region;
    if (!ComputePolygonCoverage(subpaths, surface->width, surface->height, &region)) {
        return;
    }
    const CanvasPaint paint =
        MakeCanvasPaint(fill_style_, fill_gradient_, fill_pattern_);
    const CanvasCompositing compositing = Compositing();
    bool painted = PaintShadowMask(*surface, region, compositing, shadow_color_value_,
                                   shadow_blur_, shadow_offset_x_, shadow_offset_y_);
    if (ApplyCoveragePaint(*surface, region, paint, inverse, compositing)) {
        painted = true;
    }
    if (painted) {
        ++surface->revision;
    }
}

void CanvasRenderingContext2D::Stroke() {
    if (!(line_width_ > 0.0f) || path_.empty()) {
        return;
    }
    CanvasSurface* surface = RefreshSurface();
    if (surface == nullptr) {
        return;
    }
    PaintStroke(*surface, DeviceSubPaths());
}

bool CanvasRenderingContext2D::IsPointInPath(float x, float y, bool even_odd) const {
    if (!std::isfinite(x) || !std::isfinite(y) || path_.empty()) {
        return false;
    }
    const CanvasPathPoint device = TransformPoint(matrix_, x, y);
    return PointInSubpaths(DeviceSubPaths(), device.x, device.y, even_odd);
}

void CanvasRenderingContext2D::Reset() {
    // 先按新尺寸取回表面；尺寸变化时注册表会重建并清空像素。
    CanvasSurface* surface = RefreshSurface();
    if (surface != nullptr) {
        surface->Clear();
    }
    matrix_ = CanvasMatrix2D{};
    fill_style_ = "#000000";
    stroke_style_ = "#000000";
    fill_gradient_.reset();
    stroke_gradient_.reset();
    fill_pattern_.reset();
    stroke_pattern_.reset();
    font_ = "16px";
    font_size_ = 16.0f;
    font_family_.clear();
    font_id_ = 0;
    font_service_ = nullptr;
    scaled_font_id_ = 0;
    scaled_font_service_ = nullptr;
    scaled_font_size_ = 0.0f;
    line_width_ = 1.0f;
    global_alpha_ = 1.0f;
    composite_operation_ = CanvasCompositeOperation::SourceOver;
    shadow_color_ = "rgba(0, 0, 0, 0)";
    shadow_color_value_ = CanvasColor{0, 0, 0, 0};
    shadow_blur_ = 0.0f;
    shadow_offset_x_ = 0.0f;
    shadow_offset_y_ = 0.0f;
    line_cap_ = "butt";
    line_join_ = "miter";
    miter_limit_ = 10.0f;
    line_dash_.clear();
    line_dash_offset_ = 0.0f;
    text_align_ = "start";
    text_baseline_ = "alphabetic";
    direction_ = "ltr";
    state_stack_.clear();
    path_.clear();
    current_point_ = CanvasPathPoint{};
    subpath_start_ = CanvasPathPoint{};
    has_current_point_ = false;
    clip_.reset();
}

void CanvasRenderingContext2D::Clip() {
    CanvasSurface* surface = RefreshSurface();
    if (surface == nullptr || surface->width <= 0 || surface->height <= 0) {
        return;
    }
    const int width = surface->width;
    const int height = surface->height;
    const bool had_clip =
        clip_ != nullptr && clip_->width == width && clip_->height == height;
    if (!had_clip) {
        // 尚无裁剪区域时从全零掩码开始，只保留当前路径覆盖的像素。
        clip_ = std::make_shared<ClipState>();
        clip_->width = width;
        clip_->height = height;
        clip_->mask.assign(static_cast<size_t>(width) * static_cast<size_t>(height),
                           static_cast<uint8_t>(0));
    } else if (!clip_.unique()) {
        // 该裁剪区域仍被保存的状态引用，修改前先复制。
        clip_ = std::make_shared<ClipState>(*clip_);
    }
    // 空路径产生空裁剪区域，之后的所有绘制都被裁剪掉。
    if (path_.empty()) {
        std::fill(clip_->mask.begin(), clip_->mask.end(), static_cast<uint8_t>(0));
        return;
    }
    const std::vector<CanvasPathSubPath> subpaths = DeviceSubPaths();
    CoverageRegion region;
    if (!ComputePolygonCoverage(subpaths, width, height, &region)) {
        std::fill(clip_->mask.begin(), clip_->mask.end(), static_cast<uint8_t>(0));
        return;
    }
    // 交集按全画布计算：当前路径覆盖范围之外一律为 0，已有裁剪区域参与相乘。
    std::vector<uint8_t> updated(static_cast<size_t>(width) * static_cast<size_t>(height),
                                 static_cast<uint8_t>(0));
    const size_t mask_width = static_cast<size_t>(clip_->width);
    for (int row = 0; row < region.height; ++row) {
        const int device_y = region.top + row;
        if (device_y < 0 || device_y >= clip_->height) {
            continue;
        }
        for (int column = 0; column < region.width; ++column) {
            const int device_x = region.left + column;
            if (device_x < 0 || device_x >= clip_->width) {
                continue;
            }
            const size_t index = static_cast<size_t>(device_y) * mask_width +
                                 static_cast<size_t>(device_x);
            const float coverage =
                region.coverage[static_cast<size_t>(row) * static_cast<size_t>(region.width) +
                                static_cast<size_t>(column)];
            const float previous =
                had_clip ? static_cast<float>(clip_->mask[index]) / 255.0f : 1.0f;
            updated[index] = static_cast<uint8_t>(
                std::lround(std::clamp(previous * coverage, 0.0f, 1.0f) * 255.0f));
        }
    }
    clip_->mask.swap(updated);
}

void CanvasRenderingContext2D::GetImageData(int x, int y, int width, int height,
                                            int* output_width, int* output_height,
                                            std::vector<uint8_t>* pixels) {
    if (output_width != nullptr) {
        *output_width = width > 0 ? width : 0;
    }
    if (output_height != nullptr) {
        *output_height = height > 0 ? height : 0;
    }
    if (pixels == nullptr) {
        return;
    }
    if (width <= 0 || height <= 0) {
        pixels->clear();
        return;
    }
    const size_t row_bytes = static_cast<size_t>(width) * 4u;
    pixels->assign(static_cast<size_t>(height) * row_bytes, 0);
    CanvasSurface* surface = RefreshSurface();
    if (surface == nullptr || surface->width <= 0 || surface->height <= 0) {
        return;
    }
    const int first_x = std::max(0, x);
    const int last_x = static_cast<int>(
        std::min<long long>(surface->width, static_cast<long long>(x) + width));
    const int first_y = std::max(0, y);
    const int last_y = static_cast<int>(
        std::min<long long>(surface->height, static_cast<long long>(y) + height));
    if (last_x <= first_x || last_y <= first_y) {
        return;
    }
    const size_t copy_bytes = static_cast<size_t>(last_x - first_x) * 4u;
    for (int device_y = first_y; device_y < last_y; ++device_y) {
        uint8_t* target = pixels->data() +
                          static_cast<size_t>(device_y - y) * row_bytes +
                          static_cast<size_t>(first_x - x) * 4u;
        const uint8_t* source =
            surface->pixels.data() +
            (static_cast<size_t>(device_y) * static_cast<size_t>(surface->width) +
             static_cast<size_t>(first_x)) *
                4u;
        std::memcpy(target, source, copy_bytes);
    }
}

void CanvasRenderingContext2D::PutImageData(const uint8_t* pixels, int width, int height, int x,
                                            int y) {
    if (pixels == nullptr || width <= 0 || height <= 0) {
        return;
    }
    CanvasSurface* surface = RefreshSurface();
    if (surface == nullptr || surface->width <= 0 || surface->height <= 0) {
        return;
    }
    const int first_x = std::max(0, x);
    const int last_x = static_cast<int>(
        std::min<long long>(surface->width, static_cast<long long>(x) + width));
    const int first_y = std::max(0, y);
    const int last_y = static_cast<int>(
        std::min<long long>(surface->height, static_cast<long long>(y) + height));
    if (last_x <= first_x || last_y <= first_y) {
        return;
    }
    const size_t copy_bytes = static_cast<size_t>(last_x - first_x) * 4u;
    for (int device_y = first_y; device_y < last_y; ++device_y) {
        const uint8_t* source =
            pixels + (static_cast<size_t>(device_y - y) * static_cast<size_t>(width) +
                      static_cast<size_t>(first_x - x)) *
                         4u;
        uint8_t* target =
            surface->pixels.data() +
            (static_cast<size_t>(device_y) * static_cast<size_t>(surface->width) +
             static_cast<size_t>(first_x)) *
                4u;
        std::memcpy(target, source, copy_bytes);
    }
    ++surface->revision;
}

// ---------------------------------------------------------------
// 状态栈与变换
// ---------------------------------------------------------------

void CanvasRenderingContext2D::Save() {
    DrawingState state;
    state.matrix = matrix_;
    state.fill_style = fill_style_;
    state.fill_gradient = fill_gradient_;
    state.fill_pattern = fill_pattern_;
    state.stroke_style = stroke_style_;
    state.stroke_gradient = stroke_gradient_;
    state.stroke_pattern = stroke_pattern_;
    state.font = font_;
    state.font_size = font_size_;
    state.font_family = font_family_;
    state.line_width = line_width_;
    state.global_alpha = global_alpha_;
    state.operation = composite_operation_;
    state.shadow_color = shadow_color_;
    state.shadow_color_value = shadow_color_value_;
    state.shadow_blur = shadow_blur_;
    state.shadow_offset_x = shadow_offset_x_;
    state.shadow_offset_y = shadow_offset_y_;
    state.line_cap = line_cap_;
    state.line_join = line_join_;
    state.miter_limit = miter_limit_;
    state.line_dash = line_dash_;
    state.line_dash_offset = line_dash_offset_;
    state.text_align = text_align_;
    state.text_baseline = text_baseline_;
    state.direction = direction_;
    state.clip = clip_;
    state_stack_.push_back(std::move(state));
}

void CanvasRenderingContext2D::Restore() {
    if (state_stack_.empty()) {
        return;
    }
    const DrawingState state = state_stack_.back();
    state_stack_.pop_back();
    matrix_ = state.matrix;
    fill_style_ = state.fill_style;
    fill_gradient_ = state.fill_gradient;
    fill_pattern_ = state.fill_pattern;
    stroke_style_ = state.stroke_style;
    stroke_gradient_ = state.stroke_gradient;
    stroke_pattern_ = state.stroke_pattern;
    font_ = state.font;
    font_size_ = state.font_size;
    font_family_ = state.font_family;
    line_width_ = state.line_width;
    global_alpha_ = state.global_alpha;
    composite_operation_ = state.operation;
    shadow_color_ = state.shadow_color;
    shadow_color_value_ = state.shadow_color_value;
    shadow_blur_ = state.shadow_blur;
    shadow_offset_x_ = state.shadow_offset_x;
    shadow_offset_y_ = state.shadow_offset_y;
    line_cap_ = state.line_cap;
    line_join_ = state.line_join;
    miter_limit_ = state.miter_limit;
    line_dash_ = state.line_dash;
    line_dash_offset_ = state.line_dash_offset;
    text_align_ = state.text_align;
    text_baseline_ = state.text_baseline;
    direction_ = state.direction;
    clip_ = state.clip;
    // 字体参数随状态恢复，句柄需要重新请求。
    font_id_ = 0;
    font_service_ = nullptr;
    scaled_font_id_ = 0;
    scaled_font_service_ = nullptr;
    scaled_font_size_ = 0.0f;
}

void CanvasRenderingContext2D::Translate(float x, float y) {
    if (!std::isfinite(x) || !std::isfinite(y)) {
        return;
    }
    CanvasMatrix2D translation;
    translation.e = x;
    translation.f = y;
    matrix_ = MultiplyMatrices(matrix_, translation);
}

void CanvasRenderingContext2D::Scale(float x, float y) {
    if (!std::isfinite(x) || !std::isfinite(y)) {
        return;
    }
    CanvasMatrix2D scaling;
    scaling.a = x;
    scaling.d = y;
    matrix_ = MultiplyMatrices(matrix_, scaling);
}

void CanvasRenderingContext2D::Rotate(float angle) {
    if (!std::isfinite(angle)) {
        return;
    }
    const float cosine = std::cos(angle);
    const float sine = std::sin(angle);
    CanvasMatrix2D rotation;
    rotation.a = cosine;
    rotation.b = sine;
    rotation.c = -sine;
    rotation.d = cosine;
    matrix_ = MultiplyMatrices(matrix_, rotation);
}

void CanvasRenderingContext2D::Transform(float a, float b, float c, float d, float e,
                                         float f) {
    CanvasMatrix2D transform;
    transform.a = a;
    transform.b = b;
    transform.c = c;
    transform.d = d;
    transform.e = e;
    transform.f = f;
    if (!MatrixIsFinite(transform)) {
        return;
    }
    matrix_ = MultiplyMatrices(matrix_, transform);
}

void CanvasRenderingContext2D::SetTransform(float a, float b, float c, float d, float e,
                                            float f) {
    CanvasMatrix2D transform;
    transform.a = a;
    transform.b = b;
    transform.c = c;
    transform.d = d;
    transform.e = e;
    transform.f = f;
    if (!MatrixIsFinite(transform)) {
        return;
    }
    matrix_ = transform;
}

void CanvasRenderingContext2D::ResetTransform() {
    matrix_ = CanvasMatrix2D{};
}

// ---------------------------------------------------------------
// 图片绘制
// ---------------------------------------------------------------

namespace {

// 把位图目标矩形的源 Alpha 采样为覆盖率区域，用于绘制阴影。
bool BuildBitmapCoverage(const uint8_t* pixels, int source_width, int source_height, float sx,
                         float sy, float source_region_width, float source_region_height,
                         float dx, float dy, float destination_width, float destination_height,
                         const CanvasMatrix2D& matrix, CanvasSurface& surface,
                         CoverageRegion* region) {
    if (pixels == nullptr || source_width <= 0 || source_height <= 0 || region == nullptr ||
        !(source_region_width > 0.0f) || !(source_region_height > 0.0f) ||
        !(destination_width > 0.0f) || !(destination_height > 0.0f)) {
        return false;
    }
    CanvasMatrix2D inverse;
    if (!InvertMatrix(matrix, &inverse)) {
        return false;
    }
    const CanvasPathPoint corners[4] = {
        TransformPoint(matrix, dx, dy),
        TransformPoint(matrix, dx + destination_width, dy),
        TransformPoint(matrix, dx + destination_width, dy + destination_height),
        TransformPoint(matrix, dx, dy + destination_height),
    };
    float min_x = corners[0].x;
    float max_x = corners[0].x;
    float min_y = corners[0].y;
    float max_y = corners[0].y;
    for (const CanvasPathPoint& corner : corners) {
        min_x = std::min(min_x, corner.x);
        max_x = std::max(max_x, corner.x);
        min_y = std::min(min_y, corner.y);
        max_y = std::max(max_y, corner.y);
    }
    const int first_x = std::max(0, static_cast<int>(std::floor(min_x)));
    const int last_x = std::min(surface.width, static_cast<int>(std::ceil(max_x)));
    const int first_y = std::max(0, static_cast<int>(std::floor(min_y)));
    const int last_y = std::min(surface.height, static_cast<int>(std::ceil(max_y)));
    if (last_x <= first_x || last_y <= first_y) {
        return false;
    }
    region->left = first_x;
    region->top = first_y;
    region->width = last_x - first_x;
    region->height = last_y - first_y;
    region->coverage.assign(static_cast<size_t>(region->width) * static_cast<size_t>(region->height),
                            0.0f);
    for (int device_y = first_y; device_y < last_y; ++device_y) {
        for (int device_x = first_x; device_x < last_x; ++device_x) {
            const CanvasPathPoint user = TransformPoint(
                inverse, static_cast<float>(device_x) + 0.5f,
                static_cast<float>(device_y) + 0.5f);
            const float normalized_x = (user.x - dx) / destination_width;
            const float normalized_y = (user.y - dy) / destination_height;
            if (normalized_x < -1e-5f || normalized_x > 1.0f + 1e-5f ||
                normalized_y < -1e-5f || normalized_y > 1.0f + 1e-5f) {
                continue;
            }
            const float source_x = sx + ClampUnit(normalized_x) * source_region_width;
            const float source_y = sy + ClampUnit(normalized_y) * source_region_height;
            const CanvasColor color =
                SampleImageBilinear(pixels, source_width, source_height, source_x, source_y);
            region->coverage[
                static_cast<size_t>(device_y - first_y) * static_cast<size_t>(region->width) +
                static_cast<size_t>(device_x - first_x)] =
                static_cast<float>(color.alpha) / 255.0f;
        }
    }
    return true;
}

// 以逆矩阵反查目标矩形对应的用户坐标，再从源位图双线性采样；
// 旋转与斜切由这一映射自然支持。
void DrawBitmap(const uint8_t* pixels, int source_width, int source_height, float sx, float sy,
                float source_region_width, float source_region_height, float dx, float dy,
                float destination_width, float destination_height, CanvasSurface& surface,
                const CanvasMatrix2D& matrix, const CanvasCompositing& compositing) {
    if (pixels == nullptr || source_width <= 0 || source_height <= 0 ||
        !std::isfinite(sx) || !std::isfinite(sy) || !std::isfinite(source_region_width) ||
        !std::isfinite(source_region_height) || !std::isfinite(dx) || !std::isfinite(dy) ||
        !std::isfinite(destination_width) || !std::isfinite(destination_height)) {
        return;
    }
    const bool clears = CompositeClearsOutsideSource(compositing.operation);
    if (!(compositing.global_alpha > 0.0f) && !clears) {
        return;
    }
    if (!(source_region_width > 0.0f) || !(source_region_height > 0.0f) ||
        !(destination_width > 0.0f) || !(destination_height > 0.0f)) {
        return;
    }
    CanvasMatrix2D inverse;
    if (!InvertMatrix(matrix, &inverse)) {
        return;
    }

    // 目标矩形四角变换后求设备包围盒。
    const CanvasPathPoint corners[4] = {
        TransformPoint(matrix, dx, dy),
        TransformPoint(matrix, dx + destination_width, dy),
        TransformPoint(matrix, dx + destination_width, dy + destination_height),
        TransformPoint(matrix, dx, dy + destination_height),
    };
    float min_x = corners[0].x;
    float max_x = corners[0].x;
    float min_y = corners[0].y;
    float max_y = corners[0].y;
    for (const CanvasPathPoint& corner : corners) {
        min_x = std::min(min_x, corner.x);
        max_x = std::max(max_x, corner.x);
        min_y = std::min(min_y, corner.y);
        max_y = std::max(max_y, corner.y);
    }
    const int first_x = std::max(0, static_cast<int>(std::floor(min_x)));
    const int last_x = std::min(surface.width, static_cast<int>(std::ceil(max_x)));
    const int first_y = std::max(0, static_cast<int>(std::floor(min_y)));
    const int last_y = std::min(surface.height, static_cast<int>(std::ceil(max_y)));
    if (last_x <= first_x || last_y <= first_y) {
        if (clears) {
            ClearCompositeOutsideSource(surface, nullptr, compositing);
        }
        return;
    }

    // 清除类合成操作：源包围盒之外、且位于裁剪区域内的像素按零 Alpha 合成。
    if (clears) {
        for (int device_y = 0; device_y < surface.height; ++device_y) {
            for (int device_x = 0; device_x < surface.width; ++device_x) {
                if (!(compositing.CoverageAt(device_x, device_y) > 0.0f)) {
                    continue;
                }
                if (device_x >= first_x && device_x < last_x && device_y >= first_y &&
                    device_y < last_y) {
                    continue;
                }
                uint8_t* pixel =
                    &surface.pixels[(static_cast<size_t>(device_y) *
                                         static_cast<size_t>(surface.width) +
                                     static_cast<size_t>(device_x)) *
                                    4];
                BlendPixelMode(pixel, 0.0f, 0.0f, 0.0f, 0.0f, compositing.operation);
            }
        }
    }

    for (int device_y = first_y; device_y < last_y; ++device_y) {
        for (int device_x = first_x; device_x < last_x; ++device_x) {
            const CanvasPathPoint user = TransformPoint(
                inverse, static_cast<float>(device_x) + 0.5f,
                static_cast<float>(device_y) + 0.5f);
            const float normalized_x = (user.x - dx) / destination_width;
            const float normalized_y = (user.y - dy) / destination_height;
            if (normalized_x < -1e-5f || normalized_x > 1.0f + 1e-5f ||
                normalized_y < -1e-5f || normalized_y > 1.0f + 1e-5f) {
                uint8_t* pixel =
                    &surface.pixels[(static_cast<size_t>(device_y) *
                                         static_cast<size_t>(surface.width) +
                                     static_cast<size_t>(device_x)) *
                                    4];
                if (clears && compositing.CoverageAt(device_x, device_y) > 0.0f) {
                    BlendPixelMode(pixel, 0.0f, 0.0f, 0.0f, 0.0f, compositing.operation);
                }
                continue;
            }
            const float source_x = sx + ClampUnit(normalized_x) * source_region_width;
            const float source_y = sy + ClampUnit(normalized_y) * source_region_height;
            const CanvasColor color =
                SampleImageBilinear(pixels, source_width, source_height, source_x, source_y);
            const float clip_coverage = compositing.CoverageAt(device_x, device_y);
            if (!(clip_coverage > 0.0f)) {
                continue;
            }
            uint8_t* pixel =
                &surface.pixels[(static_cast<size_t>(device_y) * static_cast<size_t>(surface.width) +
                                 static_cast<size_t>(device_x)) *
                                4];
            if (color.alpha == 0) {
                if (clears) {
                    BlendPixelMode(pixel, 0.0f, 0.0f, 0.0f, 0.0f, compositing.operation);
                }
                continue;
            }
            BlendPixelMode(pixel, static_cast<float>(color.red) / 255.0f,
                           static_cast<float>(color.green) / 255.0f,
                           static_cast<float>(color.blue) / 255.0f,
                           static_cast<float>(color.alpha) / 255.0f * clip_coverage *
                               compositing.global_alpha,
                           compositing.operation);
        }
    }
}

}  // namespace

void CanvasRenderingContext2D::DrawImage(const CanvasSurface& source, float dx, float dy) {
    DrawImage(source, 0.0f, 0.0f, static_cast<float>(source.width),
              static_cast<float>(source.height), dx, dy, static_cast<float>(source.width),
              static_cast<float>(source.height));
}

void CanvasRenderingContext2D::DrawImage(const CanvasSurface& source, float dx, float dy,
                                         float width, float height) {
    DrawImage(source, 0.0f, 0.0f, static_cast<float>(source.width),
              static_cast<float>(source.height), dx, dy, width, height);
}

void CanvasRenderingContext2D::DrawImage(const CanvasSurface& source, float sx, float sy,
                                         float source_width, float source_height, float dx,
                                         float dy, float width, float height) {
    if (source.width <= 0 || source.height <= 0 || source.pixels.empty()) {
        return;
    }
    CanvasSurface* surface = RefreshSurface();
    if (surface == nullptr) {
        return;
    }
    const size_t expected_size =
        static_cast<size_t>(source.width) * static_cast<size_t>(source.height) * 4u;
    if (source.pixels.size() < expected_size) {
        return;
    }
    const CanvasCompositing compositing = Compositing();
    if (shadow_color_value_.alpha > 0) {
        CoverageRegion shadow_coverage;
        if (BuildBitmapCoverage(source.pixels.data(), source.width, source.height, sx, sy,
                                source_width, source_height, dx, dy, width, height, matrix_,
                                *surface, &shadow_coverage)) {
            PaintShadowMask(*surface, shadow_coverage, compositing, shadow_color_value_,
                            shadow_blur_, shadow_offset_x_, shadow_offset_y_);
        }
    }
    DrawBitmap(source.pixels.data(), source.width, source.height, sx, sy, source_width,
               source_height, dx, dy, width, height, *surface, matrix_, compositing);
    ++surface->revision;
}

void CanvasRenderingContext2D::DrawImage(const ImageData& source, float dx, float dy) {
    DrawImage(source, 0.0f, 0.0f, static_cast<float>(source.width),
              static_cast<float>(source.height), dx, dy, static_cast<float>(source.width),
              static_cast<float>(source.height));
}

void CanvasRenderingContext2D::DrawImage(const ImageData& source, float dx, float dy,
                                         float width, float height) {
    DrawImage(source, 0.0f, 0.0f, static_cast<float>(source.width),
              static_cast<float>(source.height), dx, dy, width, height);
}

void CanvasRenderingContext2D::DrawImage(const ImageData& source, float sx, float sy,
                                         float source_width, float source_height, float dx,
                                         float dy, float width, float height) {
    if (!source.Valid()) {
        return;
    }
    CanvasSurface* surface = RefreshSurface();
    if (surface == nullptr) {
        return;
    }
    const CanvasCompositing compositing = Compositing();
    if (shadow_color_value_.alpha > 0) {
        CoverageRegion shadow_coverage;
        if (BuildBitmapCoverage(source.pixels.data(), source.width, source.height, sx, sy,
                                source_width, source_height, dx, dy, width, height, matrix_,
                                *surface, &shadow_coverage)) {
            PaintShadowMask(*surface, shadow_coverage, compositing, shadow_color_value_,
                            shadow_blur_, shadow_offset_x_, shadow_offset_y_);
        }
    }
    DrawBitmap(source.pixels.data(), source.width, source.height, sx, sy, source_width,
               source_height, dx, dy, width, height, *surface, matrix_, compositing);
    ++surface->revision;
}

}  // namespace web
