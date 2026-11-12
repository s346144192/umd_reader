#pragma once

#include <windows.h>

#include <cstdint>
#include <string>

#include "gfx/types.h"

namespace gfx {

class TextFormat;
class TextLayout;

// 解码后的位图资源。具体后端持有原生位图对象，此处只暴露尺寸。
class Image {
public:
    virtual ~Image() = default;
    virtual uint32_t PixelWidth() const = 0;
    virtual uint32_t PixelHeight() const = 0;
};

// 绘制画布：与具体图形 API 无关的图元与裁剪接口。
// BeginFrame / EndFrame 与窗口设备上下文绑定；同一时刻只对应一个窗口。
class Canvas {
public:
    virtual ~Canvas() = default;

    virtual bool BeginFrame(HDC deviceContext, const RECT& targetRect) = 0;
    // 返回 false 表示渲染目标丢失，调用方需要清空依赖设备的资源。
    virtual bool EndFrame() = 0;
    virtual bool HasTarget() const = 0;

    virtual void Clear(const ColorF& color) = 0;
    virtual void FillRect(const RectF& rect, const ColorF& color) = 0;
    virtual void FillRoundedRect(const RectF& rect, float radius, const ColorF& color) = 0;
    virtual void StrokeRoundedRect(const RectF& rect, float radius, const ColorF& color,
                                   float strokeWidth) = 0;
    virtual void DrawLine(PointF start, PointF end, const ColorF& color, float strokeWidth) = 0;
    virtual void DrawText(const std::wstring& text, const RectF& rect, const TextFormat& format,
                          const ColorF& color) = 0;
    virtual void DrawLayout(const TextLayout& layout, PointF origin, const ColorF& color) = 0;
    virtual void DrawImage(const Image& image, const RectF& rect, float opacity) = 0;

    virtual void PushClip(const RectF& rect) = 0;
    virtual void PopClip() = 0;
};

}  // namespace gfx
