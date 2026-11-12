#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "web/image.h"
#include "web/text.h"

namespace web {

class Element;

// 画布像素表面，尺寸以 CSS 像素为单位。
// 像素布局为 RGBA8、行优先，第一行为画布顶行；revision 在内容发生改动时递增，
// 供绘制层判断是否需要重新上传纹理。
struct CanvasSurface {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> pixels;
    uint64_t revision = 0;

    // 重建为指定尺寸并清空为透明；尺寸非正时不改动。
    void Resize(int new_width, int new_height);
    // 清空为全透明并递增 revision。
    void Clear();
};

// 画布注册表：以元素地址为键保存像素表面，进程内共享且带互斥锁。
// 不存在时创建并清空为透明；已有表面尺寸不同时按新尺寸重建。
CanvasSurface* AcquireCanvasSurface(const Element& element, int width, int height);
// 查询已有表面；不存在时返回 nullptr。返回的指针在下一次 Acquire 重建后失效。
const CanvasSurface* FindCanvasSurface(const Element& element);
// 元素即将销毁时释放其画布表面；DOM 析构函数会调用此函数，避免注册表残留失效地址。
void ReleaseCanvasSurface(const Element& element);
// 释放全部画布表面。
void ClearCanvasSurfaces();

// 画布位图尺寸（设备像素），与 <canvas> 的 width/height 属性一致。
struct CanvasPixelSize {
    int width = 300;
    int height = 150;
};

// 画布单个维度的像素上限，避免异常尺寸导致过量内存分配。
constexpr int kMaxCanvasDimension = 16384;

// 读取 <canvas> 的位图尺寸：属性优先，缺失或非法时使用 300×150。
CanvasPixelSize ResolveCanvasPixelSize(const Element& element);
// 按当前 width/height 属性同步已存在的画布表面；尚未创建表面时不新建。
void SyncCanvasSurfaceSize(const Element& element);

// 设置画布图片的解码基准目录；相对 src 按此目录解析，目录变化时清空图片缓存。
void SetCanvasBaseDirectory(std::string base_directory);
// 按基准目录解析并解码图片，成功时返回缓存中的像素缓冲，失败时返回 nullptr。
// 返回的指针在下一次 SetCanvasBaseDirectory 或 ClearCanvasImageCache 后失效。
const ImageData* LoadCanvasImage(const std::string& source);
// 清空画布图片缓存。
void ClearCanvasImageCache();

// 注入共享的文本服务；传入 nullptr 时由 canvas.cpp 惰性自建一个实例。
void SetCanvasTextService(TextService* service);

// 8 位 RGBA 颜色。
struct CanvasColor {
    uint8_t red = 0;
    uint8_t green = 0;
    uint8_t blue = 0;
    uint8_t alpha = 255;
};

// 解析颜色文本：支持 #rgb、#rrggbb、rgb(r,g,b)、rgba(r,g,b,a) 与常见颜色名；
// 无法识别时按不透明黑色处理。
CanvasColor ParseCanvasColor(const std::string& text);

// 渐变颜色停靠点：offset 为 0..1 的归一化位置。
struct CanvasGradientStop {
    float offset = 0.0f;
    CanvasColor color;
};

// 画布渐变对象：坐标与半径均使用创建时记录的用户坐标。
// 绘制时按当前矩阵的逆映射采样，因此渐变随变换一起移动与缩放。
class CanvasGradient {
public:
    enum class Type {
        Linear,
        Radial,
    };

    // 线性渐变：从 (x0,y0) 到 (x1,y1) 沿连线方向插值。
    static std::shared_ptr<CanvasGradient> CreateLinear(float x0, float y0, float x1, float y1);
    // 径向渐变：由起点圆 (x0,y0,r0) 过渡到终点圆 (x1,y1,r1)。
    static std::shared_ptr<CanvasGradient> CreateRadial(float x0, float y0, float r0, float x1,
                                                        float y1, float r1);

    // 添加颜色停靠点；offset 超出 0..1 时按端点截断，非法数值忽略。
    // 每次添加后按 offset 升序保持有序，相同 offset 保留先加入的停靠点。
    void AddColorStop(float offset, const std::string& color);

    // 在用户坐标 (x,y) 处取渐变颜色，颜色 Alpha 参与插值。
    CanvasColor ColorAt(float x, float y) const;

    Type type() const { return type_; }
    size_t StopCount() const { return stops_.size(); }

private:
    CanvasGradient(Type type, float x0, float y0, float x1, float y1, float r0, float r1);

    Type type_;
    float x0_ = 0.0f;
    float y0_ = 0.0f;
    float x1_ = 0.0f;
    float y1_ = 0.0f;
    float r0_ = 0.0f;
    float r1_ = 0.0f;
    std::vector<CanvasGradientStop> stops_;
};

// 2x3 仿射矩阵：设备坐标 = (a*x + c*y + e, b*x + d*y + f)，与 Canvas 规范一致。
struct CanvasMatrix2D {
    float a = 1.0f;
    float b = 0.0f;
    float c = 0.0f;
    float d = 1.0f;
    float e = 0.0f;
    float f = 0.0f;
};

// 画布图案：以位图为内容，按重复模式平铺。创建时记录当时的变换，
// 之后按该变换的逆矩阵把设备坐标映射回图案像素。
class CanvasPattern {
public:
    enum class Repetition {
        Repeat,
        RepeatX,
        RepeatY,
        NoRepeat,
    };

    CanvasPattern(ImageData image, Repetition repetition, CanvasMatrix2D transform);

    const ImageData& image() const { return image_; }
    Repetition repetition() const { return repetition_; }
    const CanvasMatrix2D& transform() const { return transform_; }

    // 在设备坐标处采样图案颜色；无重复模式且坐标越界时返回透明黑。
    CanvasColor ColorAtDevice(float device_x, float device_y) const;

private:
    ImageData image_;
    Repetition repetition_ = Repetition::Repeat;
    CanvasMatrix2D transform_;
    // 创建时预计算的逆矩阵；矩阵不可逆时 invertible_ 为 false，图案不绘制。
    CanvasMatrix2D inverse_;
    bool invertible_ = false;
};

// 路径点与路径子段：曲线录入时按自适应细分转换为折线点列，closed 表示闭合子路径。
struct CanvasPathPoint {
    float x = 0.0f;
    float y = 0.0f;
};

struct CanvasPathSubPath {
    std::vector<CanvasPathPoint> points;
    bool closed = false;
};

// 合成操作，取值与 Canvas 的 globalCompositeOperation 一致。
enum class CanvasCompositeOperation {
    SourceOver,
    SourceIn,
    SourceOut,
    SourceAtop,
    DestinationOver,
    DestinationIn,
    DestinationOut,
    DestinationAtop,
    Lighter,
    Copy,
    Xor,
};

// 画布合成参数：全局透明度、合成操作与按设备像素保存的裁剪覆盖率掩码。
struct CanvasCompositing {
    float global_alpha = 1.0f;
    CanvasCompositeOperation operation = CanvasCompositeOperation::SourceOver;
    const uint8_t* clip_mask = nullptr;
    int clip_width = 0;
    int clip_height = 0;

    // 指定设备像素处的裁剪覆盖率；未设置裁剪掩码时为 1，掩码范围外为 0。
    float CoverageAt(int device_x, int device_y) const;
};

// 最小 2D 绘制上下文：直接在 CanvasSurface 上进行 CPU 光栅化，不依赖图形后端。
// 上下文记录所属元素的地址，每次绘制前从注册表取回当前表面，尺寸变化后仍可正确绘制。
class CanvasRenderingContext2D {
public:
    explicit CanvasRenderingContext2D(Element* element);

    const std::string& FillStyle() const { return fill_style_; }
    // 设置颜色文本；同时清除已设置的渐变。
    void SetFillStyle(std::string value);

    const std::string& StrokeStyle() const { return stroke_style_; }
    void SetStrokeStyle(std::string value);

    // 使用渐变作为填充或描边来源；fillStyle/strokeStyle 的文本读回为渐变占位串。
    void SetFillGradient(std::shared_ptr<CanvasGradient> gradient);
    void SetStrokeGradient(std::shared_ptr<CanvasGradient> gradient);
    const std::shared_ptr<CanvasGradient>& FillGradient() const { return fill_gradient_; }
    const std::shared_ptr<CanvasGradient>& StrokeGradient() const { return stroke_gradient_; }

    // 使用图案作为填充或描边来源；文本读回为图案占位串。
    void SetFillPattern(std::shared_ptr<CanvasPattern> pattern);
    void SetStrokePattern(std::shared_ptr<CanvasPattern> pattern);
    const std::shared_ptr<CanvasPattern>& FillPattern() const { return fill_pattern_; }
    const std::shared_ptr<CanvasPattern>& StrokePattern() const { return stroke_pattern_; }

    const std::string& Font() const { return font_; }
    void SetFont(std::string value);

    float LineWidth() const { return line_width_; }
    // 非正数或非有限值被忽略，保留原线宽。
    void SetLineWidth(float value);

    float GlobalAlpha() const { return global_alpha_; }
    // 仅接受 0..1 之间的有限值，其它输入被忽略，保留原值。
    void SetGlobalAlpha(float value);

    // 返回规范写法；未识别的新值时返回 "source-over"。
    std::string GlobalCompositeOperation() const;
    // 只接受规范中的合成操作名，其它值被忽略。
    void SetGlobalCompositeOperation(std::string value);

    const std::string& ShadowColor() const { return shadow_color_; }
    // 只接受可解析的颜色，其它输入被忽略。
    void SetShadowColor(std::string value);

    float ShadowBlur() const { return shadow_blur_; }
    // 只接受不小于 0 的有限值。
    void SetShadowBlur(float value);

    float ShadowOffsetX() const { return shadow_offset_x_; }
    void SetShadowOffsetX(float value);

    float ShadowOffsetY() const { return shadow_offset_y_; }
    void SetShadowOffsetY(float value);

    const std::string& LineCap() const { return line_cap_; }
    // 仅接受 butt、round、square，其它值被忽略。
    void SetLineCap(std::string value);

    const std::string& LineJoin() const { return line_join_; }
    // 仅接受 miter、round、bevel，其它值被忽略。
    void SetLineJoin(std::string value);

    float MiterLimit() const { return miter_limit_; }
    // 只接受不小于 1 的有限值，其它输入被忽略。
    void SetMiterLimit(float value);

    const std::vector<float>& LineDash() const { return line_dash_; }
    // 设置虚线样式：元素为交替的实线段与空白段长度，必须全部为有限非负数。
    // 含非法值或全为 0 时保留原样式；空数组表示实线。
    void SetLineDash(std::vector<float> pattern);
    float LineDashOffset() const { return line_dash_offset_; }
    // 只接受有限值，其它输入被忽略。
    void SetLineDashOffset(float value);

    const std::string& TextAlign() const { return text_align_; }
    // 仅接受 start、end、left、right、center，其它值被忽略。
    void SetTextAlign(std::string value);

    const std::string& Direction() const { return direction_; }
    // 仅接受 ltr 与 rtl；其它值被忽略。
    void SetDirection(std::string value);

    const std::string& TextBaseline() const { return text_baseline_; }
    // 仅接受 alphabetic、top、hanging、middle、ideographic、bottom，其它值被忽略。
    void SetTextBaseline(std::string value);

    // 以下坐标与尺寸均为 CSS 像素；负宽度或负高度按矩形规范化处理。
    void FillRect(float x, float y, float width, float height);
    void ClearRect(float x, float y, float width, float height);
    void StrokeRect(float x, float y, float width, float height);
    // 基线坐标为文本框基线；文本按 UTF-8 逐个码位绘制。
    void FillText(const std::string& text, float x, float baseline_y);
    // 返回按字形 advance 累加得到的宽度。
    float MeasureText(const std::string& text);

    // 当前路径：用户坐标下的折线子路径。曲线在录入时按自适应细分转换为折线。
    void BeginPath();
    void MoveTo(float x, float y);
    void LineTo(float x, float y);
    void QuadraticCurveTo(float control_x, float control_y, float x, float y);
    void BezierCurveTo(float control1_x, float control1_y, float control2_x, float control2_y,
                       float x, float y);
    // 圆弧：角度为弧度；counterclockwise 为真时按逆时针方向从起始角走到终止角。
    void Arc(float x, float y, float radius, float start_angle, float end_angle,
             bool counterclockwise);
    // 追加独立闭合矩形子路径，不连接当前点。
    void Rect(float x, float y, float width, float height);
    // 追加椭圆弧：rotation 为弧度，radius_x/radius_y 为两轴半径。
    void Ellipse(float x, float y, float radius_x, float radius_y, float rotation,
                 float start_angle, float end_angle, bool counterclockwise);
    // 追加与当前点、控制点两侧方向相切的圆弧，radius 为圆半径。
    void ArcTo(float x1, float y1, float x2, float y2, float radius);
    void ClosePath();
    // 以 nonzero 缠绕规则填充当前路径，使用 fillStyle 或填充渐变。
    void Fill();
    // 按当前线宽描边当前路径；连接与端点用折线距离场近似为圆头。
    void Stroke();
    // 判断点是否位于当前路径内部；even_odd 为真时使用奇偶规则，否则使用 nonzero。
    bool IsPointInPath(float x, float y, bool even_odd) const;

    // 以当前路径建立裁剪区域，与已有裁剪区域相交；空路径产生空裁剪区域。
    void Clip();

    // 重置位图与全部绘制状态（变换、样式、字体、路径、裁剪与状态栈）。
    // 画布尺寸变化时按规范调用；上下文对象本身保持有效。
    void Reset();

    // 读取设备像素区域：忽略当前变换、裁剪与全局透明度，区域超出画布的部分
    // 返回透明黑。宽度或高度非正时输出为空。
    void GetImageData(int x, int y, int width, int height, int* output_width,
                      int* output_height, std::vector<uint8_t>* pixels);
    // 写入设备像素区域：按行优先 RGBA8 直接覆盖目标像素，忽略当前变换、
    // 裁剪与全局透明度；超出画布的部分被丢弃。
    void PutImageData(const uint8_t* pixels, int width, int height, int x, int y);

    // 绘制状态栈：保存与恢复矩阵、填充/描边来源、字体与线宽；当前路径不参与保存。
    void Save();
    void Restore();
    void Translate(float x, float y);
    void Scale(float x, float y);
    // 角度为弧度。
    void Rotate(float angle);
    // 以当前矩阵右乘传入矩阵（传入变换先作用于点）。
    void Transform(float a, float b, float c, float d, float e, float f);
    void SetTransform(float a, float b, float c, float d, float e, float f);
    void ResetTransform();

    // 图片绘制：源为画布表面或解码后的位图，目标矩形经当前矩阵变换，
    // 旋转与斜切由逆矩阵反查源像素实现。
    void DrawImage(const CanvasSurface& source, float dx, float dy);
    void DrawImage(const CanvasSurface& source, float dx, float dy, float width, float height);
    void DrawImage(const CanvasSurface& source, float sx, float sy, float source_width,
                   float source_height, float dx, float dy, float width, float height);
    void DrawImage(const ImageData& source, float dx, float dy);
    void DrawImage(const ImageData& source, float dx, float dy, float width, float height);
    void DrawImage(const ImageData& source, float sx, float sy, float source_width,
                   float source_height, float dx, float dy, float width, float height);

    // 当前画布表面；元素为空、尚未创建或元素已销毁时返回 nullptr。
    CanvasSurface* Surface() { return surface_; }
    const Element* TargetElement() const { return element_; }
    // 当前变换矩阵；创建图案时用于记录图案坐标系。
    const CanvasMatrix2D& CurrentTransform() const { return matrix_; }

private:
    // 裁剪区域：按设备像素保存 0..255 覆盖率。保存/恢复状态之间共享，
    // 修改前若被多个状态引用则先复制。
    struct ClipState {
        int width = 0;
        int height = 0;
        std::vector<uint8_t> mask;
    };

    struct DrawingState {
        CanvasMatrix2D matrix;
        std::string fill_style;
        std::shared_ptr<CanvasGradient> fill_gradient;
        std::shared_ptr<CanvasPattern> fill_pattern;
        std::string stroke_style;
        std::shared_ptr<CanvasGradient> stroke_gradient;
        std::shared_ptr<CanvasPattern> stroke_pattern;
        std::string font;
        float font_size = 16.0f;
        std::string font_family;
        float line_width = 1.0f;
        float global_alpha = 1.0f;
        CanvasCompositeOperation operation = CanvasCompositeOperation::SourceOver;
        std::string shadow_color;
        CanvasColor shadow_color_value;
        float shadow_blur = 0.0f;
        float shadow_offset_x = 0.0f;
        float shadow_offset_y = 0.0f;
        std::string line_cap;
        std::string line_join;
        float miter_limit = 10.0f;
        std::vector<float> line_dash;
        float line_dash_offset = 0.0f;
        std::string text_align;
        std::string text_baseline;
        // 文本方向：ltr 或 rtl。
        std::string direction;
        std::shared_ptr<ClipState> clip;
    };

    // 依据元素当前尺寸从注册表取回表面；注册表已无该元素（已销毁）时返回 nullptr。
    CanvasSurface* RefreshSurface();
    // 解析 font 字符串并创建（或复用）字体句柄；service_out 返回所用文本服务。
    FontId ResolveFont(TextService** service_out);
    // 按指定字号（可为变换缩放后的字号）创建字体句柄。
    FontId ResolveFontWithSize(float size, TextService** service_out);

    // 追加路径点：没有当前子路径时以该点开始一个新的子路径。
    void AppendPathPoint(float x, float y);
    // 以自适应细分把二次/三次贝塞尔曲线追加为折线。
    void FlattenQuadratic(const CanvasPathPoint& from, const CanvasPathPoint& control,
                          const CanvasPathPoint& to, int depth);
    void FlattenCubic(const CanvasPathPoint& from, const CanvasPathPoint& control1,
                      const CanvasPathPoint& control2, const CanvasPathPoint& to, int depth);

    // 把当前路径变换到设备坐标，用于填充与描边。
    std::vector<CanvasPathSubPath> DeviceSubPaths() const;

    // 当前全局透明度与裁剪掩码组成的合成参数。
    CanvasCompositing Compositing() const;
    // 对设备坐标子路径执行描边：虚线切分、端点与连接几何、渐变或纯色合成。
    // 返回是否修改了表面（修改时已递增 revision）。
    bool PaintStroke(CanvasSurface& surface, std::vector<CanvasPathSubPath> subpaths);
    // 画布尺寸变化后丢弃裁剪掩码；返回是否刚刚发生了尺寸变化。
    bool DiscardClipForResizedSurface(const CanvasSurface& surface);

    Element* element_ = nullptr;
    CanvasSurface* surface_ = nullptr;

    std::string fill_style_ = "#000000";
    std::string stroke_style_ = "#000000";
    std::shared_ptr<CanvasGradient> fill_gradient_;
    std::shared_ptr<CanvasGradient> stroke_gradient_;
    std::shared_ptr<CanvasPattern> fill_pattern_;
    std::shared_ptr<CanvasPattern> stroke_pattern_;
    std::string font_ = "16px";
    float line_width_ = 1.0f;
    float global_alpha_ = 1.0f;
    CanvasCompositeOperation composite_operation_ = CanvasCompositeOperation::SourceOver;
    std::string shadow_color_ = "rgba(0, 0, 0, 0)";
    CanvasColor shadow_color_value_ = CanvasColor{0, 0, 0, 0};
    float shadow_blur_ = 0.0f;
    float shadow_offset_x_ = 0.0f;
    float shadow_offset_y_ = 0.0f;
    // 端点样式：butt、round、square。
    std::string line_cap_ = "butt";
    // 连接样式：miter、round、bevel；默认 miter，与 Canvas 规范一致。
    std::string line_join_ = "miter";
    float miter_limit_ = 10.0f;
    // 虚线样式与相位，长度按用户坐标保存，描边时按当前缩放换算到设备像素。
    std::vector<float> line_dash_;
    float line_dash_offset_ = 0.0f;
    // 文本对齐与基线，取值见 SetTextAlign / SetTextBaseline。
    std::string text_align_ = "start";
    std::string text_baseline_ = "alphabetic";
    // 文本方向：ltr 或 rtl；rtl 时字符从右向左排布，start/end 随之翻转。
    std::string direction_ = "ltr";

    // 裁剪区域：按设备像素保存 0..255 覆盖率；尺寸与画布表面一致。
    std::shared_ptr<ClipState> clip_;

    float font_size_ = 16.0f;
    std::string font_family_;
    FontId font_id_ = 0;
    TextService* font_service_ = nullptr;
    // 变换缩放后的字形句柄缓存，避免每个文本调用都重新创建字体。
    float scaled_font_size_ = 0.0f;
    FontId scaled_font_id_ = 0;
    TextService* scaled_font_service_ = nullptr;

    CanvasMatrix2D matrix_;
    std::vector<DrawingState> state_stack_;
    std::vector<CanvasPathSubPath> path_;
    CanvasPathPoint current_point_;
    CanvasPathPoint subpath_start_;
    bool has_current_point_ = false;
};

}  // namespace web
