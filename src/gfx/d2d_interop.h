#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <cstdint>
#include <filesystem>
#include <string>

#include "gfx/types.h"

namespace gfx {

// Direct2D 后端的原生互操作接口，供尚未迁移到 Canvas 抽象的旧阅读界面使用。
// 非 Direct2D 后端返回 nullptr，调用方需要按后端能力降级或改用 Canvas。
class D2DInterop {
public:
    virtual ~D2DInterop() = default;

    virtual IDWriteFactory* WriteFactory() const = 0;
    virtual ID2D1SolidColorBrush* SolidBrush(const ColorF& color) = 0;
    virtual void DrawTextNative(const std::wstring& text, const RectF& rect,
                                IDWriteTextFormat* format, const ColorF& color) = 0;
    virtual void DrawLayoutNative(IDWriteTextLayout* layout, PointF origin,
                                  const ColorF& color) = 0;
    virtual void DrawBitmapNative(ID2D1Bitmap* bitmap, const RectF& rect, float opacity) = 0;
    virtual Microsoft::WRL::ComPtr<ID2D1Bitmap> LoadBitmapFile(
        const std::filesystem::path& path) = 0;

    virtual Microsoft::WRL::ComPtr<ID2D1Bitmap> LoadScaledBitmapFile(
        const std::filesystem::path& path, float maxWidth, float maxHeight,
        uint32_t& sourceWidth, uint32_t& sourceHeight) = 0;

    // 由内存中的 BGRA 像素创建位图。pixels 每行从上到下，每像素 4 字节，
    // Alpha 按不透明处理；用于绘制由 PDF 页面等来源渲染出的位图。
    virtual Microsoft::WRL::ComPtr<ID2D1Bitmap> CreateBitmapFromPixels(const uint32_t* pixels,
                                                                       uint32_t width,
                                                                       uint32_t height) = 0;
};

}  // namespace gfx
