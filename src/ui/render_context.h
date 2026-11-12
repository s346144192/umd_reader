#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "gfx/d2d_interop.h"
#include "gfx/renderer.h"
#include "gfx/types.h"

namespace ui {

using Microsoft::WRL::ComPtr;

// 字体族条目来自 gfx 的排版引擎，此处保留原名字以便现有界面代码继续使用。
using FontFamilyName = gfx::FontFamilyName;

struct ThemeColors {
    D2D1_COLOR_F background{};
    D2D1_COLOR_F surface{};
    D2D1_COLOR_F surfaceAlt{};
    D2D1_COLOR_F text{};
    D2D1_COLOR_F secondaryText{};
    D2D1_COLOR_F accent{};
    D2D1_COLOR_F accentText{};
    D2D1_COLOR_F divider{};
    D2D1_COLOR_F highlight{};
};

ThemeColors ThemeByName(const std::wstring& name);

struct TextFormats {
    ComPtr<IDWriteTextFormat> title;       // 页面主标题
    ComPtr<IDWriteTextFormat> section;     // 面板标题
    ComPtr<IDWriteTextFormat> label;       // 卡片标题
    ComPtr<IDWriteTextFormat> listItem;    // 列表项
    ComPtr<IDWriteTextFormat> body;        // 正文
    ComPtr<IDWriteTextFormat> caption;      // 辅助文字
    ComPtr<IDWriteTextFormat> captionRight; // 右对齐辅助文字
    // 自动换行的辅助文字，用于成就说明等可能超出一行的文案。
    ComPtr<IDWriteTextFormat> captionWrap;
    ComPtr<IDWriteTextFormat> centered;    // 居中提示
    ComPtr<IDWriteTextFormat> button;      // 按钮文字
    ComPtr<IDWriteTextFormat> buttonSmall; // 小按钮文字
    ComPtr<IDWriteTextFormat> hero;        // 封面占位文字
    ComPtr<IDWriteTextFormat> note;        // 标注文字（比正文小）
};

// 阅读界面的绘制入口。底层设备资源与图元绘制由 gfx 渲染后端承载；
// 本类保留原有 Direct2D / DirectWrite 公开签名，作为尚未迁移界面的兼容适配层。
class RenderContext {
public:
    RenderContext();
    ~RenderContext();

    bool Initialize(HWND hwnd);
    void Shutdown();

    // 返回 true 表示渲染目标被重建，调用方需要清空依赖设备的资源。
    bool Resize(UINT widthPx, UINT heightPx, UINT dpi);
    float DpiScale() const { return static_cast<float>(dpi_) / 96.0f; }
    UINT Dpi() const { return dpi_; }

    bool BeginDraw(HDC deviceContext, const RECT& targetRect);
    bool EndDraw();  // 返回 false 表示渲染目标丢失，需要重建
    void Clear(const D2D1_COLOR_F& color);

    void FillRect(const D2D1_RECT_F& rect, const D2D1_COLOR_F& color);
    void FillRoundedRect(const D2D1_RECT_F& rect, float radius, const D2D1_COLOR_F& color);
    void StrokeRoundedRect(const D2D1_RECT_F& rect, float radius, const D2D1_COLOR_F& color,
                           float strokeWidth);
    void DrawLine(D2D1_POINT_2F start, D2D1_POINT_2F end, const D2D1_COLOR_F& color,
                  float strokeWidth);
    void DrawTextBlock(const std::wstring& text, const D2D1_RECT_F& rect, IDWriteTextFormat* format,
                       const D2D1_COLOR_F& color);
    void DrawLayout(IDWriteTextLayout* layout, D2D1_POINT_2F origin, const D2D1_COLOR_F& color);
    void PushClip(const D2D1_RECT_F& rect);
    void PopClip();
    void DrawBitmap(ID2D1Bitmap* bitmap, const D2D1_RECT_F& rect, float opacity);

    ID2D1SolidColorBrush* SolidBrush(const D2D1_COLOR_F& color);
    ComPtr<ID2D1Bitmap> LoadBitmap(const std::filesystem::path& path);
    // 按目标尺寸解码图片，用于背景缩略图，避免按原始分辨率占用显存；
    // sourceWidth / sourceHeight 返回图片的原始像素尺寸。
    ComPtr<ID2D1Bitmap> LoadScaledBitmap(const std::filesystem::path& path, float maxWidth,
                                         float maxHeight, uint32_t& sourceWidth,
                                         uint32_t& sourceHeight);
    // 由内存中的 BGRA 像素创建位图，每行从上到下，Alpha 按不透明处理。
    // 渲染目标重建后已创建的位图随之失效，需要重新创建。
    ComPtr<ID2D1Bitmap> CreateBitmapFromPixels(const uint32_t* pixels, uint32_t width,
                                               uint32_t height);

    bool CreateTextFormats(const std::wstring& fontFamily, float bodySize, float lineSpacing,
                           float noteSize);
    const TextFormats& Formats() const { return formats_; }

    ComPtr<IDWriteTextLayout> CreateBodyLayout(const std::wstring& text, float width,
                                              float maxHeight);
    ComPtr<IDWriteTextLayout> CreateLayout(const std::wstring& text, IDWriteTextFormat* format,
                                           float width, float maxHeight);
    float MeasureTextWidth(const std::wstring& text, IDWriteTextFormat* format) const;
    // 枚举系统字族中可显示中文的字体，供阅读设置选择。
    std::vector<FontFamilyName> SystemFontFamilies() const;
    IDWriteFactory* WriteFactory() const;

    // 当前实际使用的渲染后端；启动时按环境变量选择，无法识别时回退 Direct2D。
    gfx::BackendKind Backend() const;
    std::wstring BackendName() const;

private:
    void ResetTextFormats();

    // gfx 渲染后端持有工厂、设备无关的绘制画布与排版引擎。
    std::unique_ptr<gfx::Renderer> renderer_;
    // Direct2D 后端的原生互操作入口，供本兼容层绘制文本与位图。
    gfx::D2DInterop* d2dInterop_ = nullptr;
    // 保留排版对象的所有权，并把原生格式指针映射到 gfx 的文本格式对象。
    std::vector<std::unique_ptr<gfx::TextFormat>> ownedFormats_;
    std::unordered_map<const IDWriteTextFormat*, gfx::TextFormat*> formatLookup_;
    TextFormats formats_;
    HWND hwnd_ = nullptr;
    UINT dpi_ = 96;
    bool initialized_ = false;
};

}  // namespace ui
