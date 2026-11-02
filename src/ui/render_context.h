#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace ui {

using Microsoft::WRL::ComPtr;

// 系统字族条目：localized 为界面显示名（优先中文），canonical 为 DirectWrite 使用的族名。
struct FontFamilyName {
    std::wstring localized;
    std::wstring canonical;
};

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
    ComPtr<IDWriteTextFormat> centered;    // 居中提示
    ComPtr<IDWriteTextFormat> button;      // 按钮文字
    ComPtr<IDWriteTextFormat> buttonSmall; // 小按钮文字
    ComPtr<IDWriteTextFormat> hero;        // 封面占位文字
    ComPtr<IDWriteTextFormat> note;        // 标注文字（比正文小）
};

class RenderContext {
public:
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
    IDWriteFactory* WriteFactory() const { return writeFactory_.Get(); }

private:
    bool CreateDeviceResources();
    void DiscardDeviceResources();

    ComPtr<ID2D1DCRenderTarget> renderTarget_;
    ComPtr<ID2D1Factory> d2dFactory_;
    ComPtr<IDWriteFactory> writeFactory_;
    ComPtr<IWICImagingFactory> wicFactory_;
    std::unordered_map<uint32_t, ComPtr<ID2D1SolidColorBrush>> brushCache_;
    TextFormats formats_;
    HWND hwnd_ = nullptr;
    UINT dpi_ = 96;
    bool initialized_ = false;
};

}  // namespace ui
