#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "web/image.h"
#include "web/types.h"

namespace web {

enum class RenderBackend {
    Direct3D11,
    OpenGL33,
};

using ImageHandle = uint32_t;  // 0 表示无效句柄

// 单个字形的屏幕四边形与图集纹理坐标（UV 取 0..1 归一化）。
struct GlyphQuad {
    float x0 = 0.0f;
    float y0 = 0.0f;
    float x1 = 0.0f;
    float y1 = 0.0f;
    float u0 = 0.0f;
    float v0 = 0.0f;
    float u1 = 0.0f;
    float v1 = 0.0f;
    Color color = Black();
};

// 离屏渲染设备：所有后端都渲染到一张 RGBA8 目标纹理，ReadPixels 取回像素
// （顶行优先），用于生成 PNG 校验图或拷贝到预览窗口。
class RenderDevice {
public:
    virtual ~RenderDevice() = default;

    virtual bool Initialize(int width, int height) = 0;
    virtual void Shutdown() = 0;
    virtual bool Resize(int width, int height) = 0;

    virtual int Width() const = 0;
    virtual int Height() const = 0;
    virtual RenderBackend Kind() const = 0;

    // 清空整张目标。BeginFrame 后此前的裁剪栈被重置。
    virtual void BeginFrame(const Color& clear_color) = 0;
    virtual bool EndFrame() = 0;

    virtual void FillRect(const RectF& rect, const Color& color) = 0;
    virtual void FillRoundedRect(const RectF& rect, float radius, const Color& color) = 0;
    // 描边沿矩形内侧绘制，线宽以 CSS 像素计。
    virtual void StrokeRect(const RectF& rect, float width, const Color& color) = 0;
    virtual void StrokeRoundedRect(const RectF& rect, float radius, float width,
                                   const Color& color) = 0;

    // 文本：使用最近一次 UpdateFontAtlas 上传的 A8 图集。
    virtual void UpdateFontAtlas(const uint8_t* coverage, int width, int height) = 0;
    virtual void DrawGlyphs(const GlyphQuad* quads, size_t count) = 0;

    // 图片：ImageData 为 RGBA8、顶行优先；句柄由设备管理，Shutdown 时统一释放。
    virtual ImageHandle CreateImage(const ImageData& image) = 0;
    // 更新已有纹理内容；尺寸可以与创建时不同，设备内部按需重建纹理。
    virtual void UpdateImage(ImageHandle image, const ImageData& image_data) = 0;
    // corner_radius 按外接矩形内切处理，用于按 CSS border-radius 裁剪图片内容。
    virtual void DrawImage(ImageHandle image, const RectF& rect, float opacity,
                           float corner_radius = 0.0f) = 0;
    // uv 为纹理坐标范围（左、上、右、下，取值 0..1），把纹理的一部分映射到 rect。
    // object-fit: cover 等目标矩形超出内容区的场景用它只绘制可见部分，
    // 圆角仍按 rect 裁剪，避免出现直角残影。
    virtual void DrawImageSubRect(ImageHandle image, const RectF& rect, const RectF& uv,
                                  float opacity, float corner_radius) = 0;

    // 裁剪栈：矩形与栈顶求交，PopClip 恢复上一层。
    virtual void PushClip(const RectF& rect) = 0;
    virtual void PopClip() = 0;

    virtual bool ReadPixels(std::vector<uint8_t>* rgba_pixels) = 0;
};

// 按后端类型创建离屏渲染设备；创建失败返回空指针。
std::unique_ptr<RenderDevice> CreateRenderDevice(RenderBackend backend);

}  // namespace web
