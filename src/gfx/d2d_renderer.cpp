#include "gfx/d2d_renderer.h"

#include <d2d1helper.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "gfx/d2d_interop.h"

namespace gfx {
namespace {

using Microsoft::WRL::ComPtr;

D2D1_RECT_F ToD2D(const RectF& rect) {
    return D2D1::RectF(rect.left, rect.top, rect.right, rect.bottom);
}

D2D1_COLOR_F ToD2D(const ColorF& color) {
    return D2D1::ColorF(color.r, color.g, color.b, color.a);
}

DWRITE_FONT_WEIGHT ToDWriteWeight(TextWeight weight) {
    switch (weight) {
    case TextWeight::Light:
        return DWRITE_FONT_WEIGHT_LIGHT;
    case TextWeight::SemiBold:
        return DWRITE_FONT_WEIGHT_SEMI_BOLD;
    case TextWeight::Bold:
        return DWRITE_FONT_WEIGHT_BOLD;
    case TextWeight::Normal:
    default:
        return DWRITE_FONT_WEIGHT_NORMAL;
    }
}

DWRITE_TEXT_ALIGNMENT ToDWriteAlignment(TextAlignment alignment) {
    switch (alignment) {
    case TextAlignment::Center:
        return DWRITE_TEXT_ALIGNMENT_CENTER;
    case TextAlignment::Trailing:
        return DWRITE_TEXT_ALIGNMENT_TRAILING;
    case TextAlignment::Leading:
    default:
        return DWRITE_TEXT_ALIGNMENT_LEADING;
    }
}

DWRITE_PARAGRAPH_ALIGNMENT ToDWriteParagraphAlignment(ParagraphAlignment alignment) {
    switch (alignment) {
    case ParagraphAlignment::Center:
        return DWRITE_PARAGRAPH_ALIGNMENT_CENTER;
    case ParagraphAlignment::Far:
        return DWRITE_PARAGRAPH_ALIGNMENT_FAR;
    case ParagraphAlignment::Near:
    default:
        return DWRITE_PARAGRAPH_ALIGNMENT_NEAR;
    }
}

DWRITE_WORD_WRAPPING ToDWriteWrapping(WordWrapping wrapping) {
    return wrapping == WordWrapping::Wrap ? DWRITE_WORD_WRAPPING_WRAP
                                          : DWRITE_WORD_WRAPPING_NO_WRAP;
}

class D2DTextFormat final : public TextFormat {
public:
    explicit D2DTextFormat(ComPtr<IDWriteTextFormat> format) : format_(std::move(format)) {}

    IDWriteTextFormat* Native() const { return format_.Get(); }
    void* NativeHandle() const override { return format_.Get(); }

private:
    ComPtr<IDWriteTextFormat> format_;
};

class D2DTextLayout final : public TextLayout {
public:
    explicit D2DTextLayout(ComPtr<IDWriteTextLayout> layout) : layout_(std::move(layout)) {}

    IDWriteTextLayout* Native() const { return layout_.Get(); }
    void* NativeHandle() const override { return layout_.Get(); }

    float Width() const override {
        DWRITE_TEXT_METRICS metrics{};
        if (!layout_ || FAILED(layout_->GetMetrics(&metrics))) {
            return 0.0f;
        }
        return metrics.widthIncludingTrailingWhitespace;
    }

    float Height() const override {
        DWRITE_TEXT_METRICS metrics{};
        if (!layout_ || FAILED(layout_->GetMetrics(&metrics))) {
            return 0.0f;
        }
        return metrics.height;
    }

private:
    ComPtr<IDWriteTextLayout> layout_;
};

class D2DImage final : public Image {
public:
    explicit D2DImage(ComPtr<ID2D1Bitmap> bitmap) : bitmap_(std::move(bitmap)) {}

    uint32_t PixelWidth() const override {
        return bitmap_ ? static_cast<uint32_t>(bitmap_->GetSize().width) : 0;
    }
    uint32_t PixelHeight() const override {
        return bitmap_ ? static_cast<uint32_t>(bitmap_->GetSize().height) : 0;
    }
    ID2D1Bitmap* Native() const { return bitmap_.Get(); }

private:
    ComPtr<ID2D1Bitmap> bitmap_;
};

class D2DTextEngine final : public TextEngine {
public:
    bool Initialize() {
        writeFactory_.Reset();
        const HRESULT result =
            DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                reinterpret_cast<IUnknown**>(writeFactory_.GetAddressOf()));
        return SUCCEEDED(result) && writeFactory_ != nullptr;
    }

    void Shutdown() { writeFactory_.Reset(); }

    IDWriteFactory* WriteFactory() const { return writeFactory_.Get(); }

    std::unique_ptr<TextFormat> CreateFormat(const TextStyle& style) override {
        if (!writeFactory_ || style.fontSize <= 0.0f) {
            return nullptr;
        }
        ComPtr<IDWriteTextFormat> format;
        HRESULT result = writeFactory_->CreateTextFormat(
            style.fontFamily.c_str(), nullptr, ToDWriteWeight(style.weight),
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, style.fontSize,
            style.locale.c_str(), &format);
        if (FAILED(result) || !format) {
            format.Reset();
            result = writeFactory_->CreateTextFormat(
                style.fallbackFamily.c_str(), nullptr, ToDWriteWeight(style.weight),
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, style.fontSize,
                style.locale.c_str(), &format);
            if (FAILED(result) || !format) {
                return nullptr;
            }
        }
        format->SetTextAlignment(ToDWriteAlignment(style.textAlignment));
        format->SetParagraphAlignment(ToDWriteParagraphAlignment(style.paragraphAlignment));
        format->SetWordWrapping(ToDWriteWrapping(style.wrapping));

        if (style.ellipsisTrimming) {
            DWRITE_TRIMMING trimming{};
            trimming.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
            ComPtr<IDWriteInlineObject> ellipsis;
            if (SUCCEEDED(writeFactory_->CreateEllipsisTrimmingSign(format.Get(), &ellipsis))) {
                format->SetTrimming(&trimming, ellipsis.Get());
            }
        }
        if (style.uniformLineSpacing) {
            format->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, style.lineSpacing,
                                   style.baseline);
        }
        return std::make_unique<D2DTextFormat>(std::move(format));
    }

    std::unique_ptr<TextLayout> CreateLayout(const std::wstring& text, const TextFormat& format,
                                             float width, float maxHeight) override {
        if (!writeFactory_ || text.empty() || width <= 0.0f) {
            return nullptr;
        }
        const D2DTextFormat& d2dFormat = static_cast<const D2DTextFormat&>(format);
        ComPtr<IDWriteTextLayout> layout;
        writeFactory_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()),
                                        d2dFormat.Native(), width, maxHeight, &layout);
        if (!layout) {
            return nullptr;
        }
        return std::make_unique<D2DTextLayout>(std::move(layout));
    }

    float MeasureWidth(const std::wstring& text, const TextFormat& format) override {
        if (!writeFactory_ || text.empty()) {
            return 0.0f;
        }
        const D2DTextFormat& d2dFormat = static_cast<const D2DTextFormat&>(format);
        ComPtr<IDWriteTextLayout> layout;
        writeFactory_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()),
                                        d2dFormat.Native(), 4096.0f, 200.0f, &layout);
        if (!layout) {
            return 0.0f;
        }
        DWRITE_TEXT_METRICS metrics{};
        layout->GetMetrics(&metrics);
        return metrics.widthIncludingTrailingWhitespace;
    }

    std::vector<FontFamilyName> SystemFontFamilies() const override {
        std::vector<FontFamilyName> families;
        if (!writeFactory_) {
            return families;
        }
        ComPtr<IDWriteFontCollection> collection;
        if (FAILED(writeFactory_->GetSystemFontCollection(&collection, FALSE)) || !collection) {
            return families;
        }

        const auto localeIndex = [](IDWriteLocalizedStrings* strings, const wchar_t* locale) {
            UINT32 index = 0;
            BOOL exists = FALSE;
            if (strings == nullptr || FAILED(strings->FindLocaleName(locale, &index, &exists)) ||
                !exists) {
                return UINT32_MAX;
            }
            return index;
        };
        const auto localizedText = [](IDWriteLocalizedStrings* strings, UINT32 index) {
            if (strings == nullptr || index == UINT32_MAX) {
                return std::wstring();
            }
            UINT32 length = 0;
            if (FAILED(strings->GetStringLength(index, &length)) || length == 0) {
                return std::wstring();
            }
            std::vector<wchar_t> buffer(length + 1, L'\0');
            if (FAILED(strings->GetString(index, buffer.data(), length + 1))) {
                return std::wstring();
            }
            return std::wstring(buffer.data(), length);
        };

        std::unordered_set<std::wstring> seen;
        const UINT32 familyCount = collection->GetFontFamilyCount();
        for (UINT32 i = 0; i < familyCount; ++i) {
            ComPtr<IDWriteFontFamily> family;
            if (FAILED(collection->GetFontFamily(i, &family)) || !family) {
                continue;
            }
            ComPtr<IDWriteFont> font;
            if (FAILED(family->GetFont(0, &font)) || !font) {
                continue;
            }
            // 只保留包含中文字形的字族，避免列出纯符号或图标字体。
            BOOL hasHan = FALSE;
            if (FAILED(font->HasCharacter(0x4E2D, &hasHan)) || !hasHan) {
                continue;
            }
            ComPtr<IDWriteLocalizedStrings> names;
            if (FAILED(family->GetFamilyNames(&names)) || !names) {
                continue;
            }
            if (names->GetCount() == 0) {
                continue;
            }

            UINT32 zhIndex = localeIndex(names.Get(), L"zh-cn");
            if (zhIndex == UINT32_MAX) {
                zhIndex = localeIndex(names.Get(), L"zh-hans");
            }
            if (zhIndex == UINT32_MAX) {
                zhIndex = localeIndex(names.Get(), L"zh");
            }
            const UINT32 enIndex = localeIndex(names.Get(), L"en-us");

            // 界面显示优先中文名，设置保存英文族名。
            const UINT32 displayIndex =
                zhIndex != UINT32_MAX ? zhIndex : (enIndex != UINT32_MAX ? enIndex : 0);
            const UINT32 canonicalIndex = enIndex != UINT32_MAX ? enIndex : displayIndex;
            FontFamilyName entry;
            entry.canonical = localizedText(names.Get(), canonicalIndex);
            entry.localized = localizedText(names.Get(), displayIndex);
            if (entry.canonical.empty()) {
                continue;
            }
            if (entry.localized.empty()) {
                entry.localized = entry.canonical;
            }
            if (!seen.insert(entry.canonical).second) {
                continue;
            }
            families.push_back(std::move(entry));
        }

        std::sort(families.begin(), families.end(),
                  [](const FontFamilyName& first, const FontFamilyName& second) {
                      return first.localized < second.localized;
                  });
        return families;
    }

private:
    ComPtr<IDWriteFactory> writeFactory_;
};

class D2DCanvas final : public Canvas {
public:
    bool Initialize(ID2D1Factory* factory, unsigned dpi) {
        factory_ = factory;
        dpi_ = dpi;
        return EnsureTarget();
    }

    void Shutdown() {
        brushCache_.clear();
        renderTarget_.Reset();
        factory_ = nullptr;
    }

    bool Recreate(unsigned dpi) {
        dpi_ = dpi;
        brushCache_.clear();
        renderTarget_.Reset();
        return EnsureTarget();
    }

    bool BeginFrame(HDC deviceContext, const RECT& targetRect) override {
        if (!EnsureTarget()) {
            return false;
        }
        if (FAILED(renderTarget_->BindDC(deviceContext, &targetRect))) {
            return false;
        }
        renderTarget_->BeginDraw();
        return true;
    }

    bool EndFrame() override {
        if (!renderTarget_) {
            return false;
        }
        const HRESULT result = renderTarget_->EndDraw();
        if (result == D2DERR_RECREATE_TARGET) {
            brushCache_.clear();
            renderTarget_.Reset();
            return false;
        }
        return true;
    }

    bool HasTarget() const override { return renderTarget_ != nullptr; }

    ID2D1RenderTarget* Target() const { return renderTarget_.Get(); }

    void Clear(const ColorF& color) override {
        if (renderTarget_) {
            renderTarget_->Clear(ToD2D(color));
        }
    }

    void FillRect(const RectF& rect, const ColorF& color) override {
        ID2D1SolidColorBrush* brush = Brush(color);
        if (brush) {
            renderTarget_->FillRectangle(ToD2D(rect), brush);
        }
    }

    void FillRoundedRect(const RectF& rect, float radius, const ColorF& color) override {
        ID2D1SolidColorBrush* brush = Brush(color);
        if (brush) {
            renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(ToD2D(rect), radius, radius),
                                                brush);
        }
    }

    void StrokeRoundedRect(const RectF& rect, float radius, const ColorF& color,
                           float strokeWidth) override {
        ID2D1SolidColorBrush* brush = Brush(color);
        if (brush) {
            renderTarget_->DrawRoundedRectangle(D2D1::RoundedRect(ToD2D(rect), radius, radius),
                                                brush, strokeWidth);
        }
    }

    void DrawLine(PointF start, PointF end, const ColorF& color, float strokeWidth) override {
        ID2D1SolidColorBrush* brush = Brush(color);
        if (brush) {
            renderTarget_->DrawLine(D2D1::Point2F(start.x, start.y),
                                    D2D1::Point2F(end.x, end.y), brush, strokeWidth);
        }
    }

    void DrawText(const std::wstring& text, const RectF& rect, const TextFormat& format,
                  const ColorF& color) override {
        const IDWriteTextFormat* nativeFormat =
            static_cast<const D2DTextFormat&>(format).Native();
        DrawTextNative(text, rect, const_cast<IDWriteTextFormat*>(nativeFormat), color);
    }

    void DrawLayout(const TextLayout& layout, PointF origin, const ColorF& color) override {
        DrawLayoutNative(static_cast<const D2DTextLayout&>(layout).Native(), origin, color);
    }

    void DrawImage(const Image& image, const RectF& rect, float opacity) override {
        DrawBitmapNative(static_cast<const D2DImage&>(image).Native(), rect, opacity);
    }

    void PushClip(const RectF& rect) override {
        if (renderTarget_) {
            renderTarget_->PushAxisAlignedClip(ToD2D(rect), D2D1_ANTIALIAS_MODE_ALIASED);
        }
    }

    void PopClip() override {
        if (renderTarget_) {
            renderTarget_->PopAxisAlignedClip();
        }
    }

    void DrawTextNative(const std::wstring& text, const RectF& rect, IDWriteTextFormat* format,
                        const ColorF& color) {
        ID2D1SolidColorBrush* brush = Brush(color);
        if (renderTarget_ && brush && format) {
            renderTarget_->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), format,
                                     ToD2D(rect), brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }

    void DrawLayoutNative(IDWriteTextLayout* layout, PointF origin, const ColorF& color) {
        ID2D1SolidColorBrush* brush = Brush(color);
        if (renderTarget_ && brush && layout) {
            renderTarget_->DrawTextLayout(D2D1::Point2F(origin.x, origin.y), layout, brush,
                                          D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }

    void DrawBitmapNative(ID2D1Bitmap* bitmap, const RectF& rect, float opacity) {
        if (!renderTarget_ || !bitmap) {
            return;
        }
        const D2D1_SIZE_F source = bitmap->GetSize();
        const float destinationWidth = rect.Width();
        const float destinationHeight = rect.Height();
        if (source.width <= 0.0f || source.height <= 0.0f || destinationWidth <= 0.0f ||
            destinationHeight <= 0.0f) {
            return;
        }

        // 按覆盖方式裁切源图中心区域，保持原有封面与背景的显示效果。
        const float scale =
            std::max(destinationWidth / source.width, destinationHeight / source.height);
        const float visibleWidth = destinationWidth / scale;
        const float visibleHeight = destinationHeight / scale;
        const D2D1_RECT_F sourceRect = D2D1::RectF(
            (source.width - visibleWidth) / 2.0f, (source.height - visibleHeight) / 2.0f,
            (source.width + visibleWidth) / 2.0f, (source.height + visibleHeight) / 2.0f);
        renderTarget_->DrawBitmap(bitmap, ToD2D(rect), opacity,
                                  D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, sourceRect);
    }

    ID2D1SolidColorBrush* Brush(const ColorF& color) {
        if (!renderTarget_) {
            return nullptr;
        }
        const uint32_t key = ColorKey(color);
        const auto found = brushCache_.find(key);
        if (found != brushCache_.end()) {
            return found->second.Get();
        }
        ComPtr<ID2D1SolidColorBrush> brush;
        renderTarget_->CreateSolidColorBrush(ToD2D(color), &brush);
        if (!brush) {
            return nullptr;
        }
        brushCache_.emplace(key, brush);
        return brush.Get();
    }

    unsigned Dpi() const { return dpi_; }

private:
    bool EnsureTarget() {
        if (renderTarget_) {
            return true;
        }
        if (!factory_) {
            return false;
        }
        const D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
            static_cast<float>(dpi_), static_cast<float>(dpi_));
        const HRESULT result =
            factory_->CreateDCRenderTarget(&properties, renderTarget_.GetAddressOf());
        if (FAILED(result) || !renderTarget_) {
            renderTarget_.Reset();
            return false;
        }
        renderTarget_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        renderTarget_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
        brushCache_.clear();
        return true;
    }

    ID2D1Factory* factory_ = nullptr;
    ComPtr<ID2D1DCRenderTarget> renderTarget_;
    std::unordered_map<uint32_t, ComPtr<ID2D1SolidColorBrush>> brushCache_;
    unsigned dpi_ = 96;
};

class D2DRenderer final : public Renderer, public D2DInterop {
public:
    bool Initialize(HWND hwnd) override {
        hwnd_ = hwnd;
        const unsigned windowDpi = hwnd != nullptr ? GetDpiForWindow(hwnd) : 96;
        HRESULT result =
            D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2dFactory_.GetAddressOf());
        if (FAILED(result) || !d2dFactory_) {
            return false;
        }
        if (!textEngine_.Initialize()) {
            Shutdown();
            return false;
        }
        result = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&wicFactory_));
        if (FAILED(result) || !wicFactory_) {
            Shutdown();
            return false;
        }
        if (!canvas_.Initialize(d2dFactory_.Get(), windowDpi)) {
            Shutdown();
            return false;
        }
        return true;
    }

    void Shutdown() override {
        canvas_.Shutdown();
        textEngine_.Shutdown();
        wicFactory_.Reset();
        d2dFactory_.Reset();
        hwnd_ = nullptr;
    }

    bool Resize(unsigned widthPixels, unsigned heightPixels, unsigned dpi) override {
        (void)widthPixels;
        (void)heightPixels;
        if (dpi != canvas_.Dpi() || !canvas_.HasTarget()) {
            return canvas_.Recreate(dpi);
        }
        return false;
    }

    float DpiScale() const override { return static_cast<float>(canvas_.Dpi()) / 96.0f; }
    unsigned Dpi() const override { return canvas_.Dpi(); }

    Canvas& Graphics() override { return canvas_; }
    TextEngine& Text() override { return textEngine_; }
    const RendererCapabilities& Capabilities() const override { return capabilities_; }
    D2DInterop* Direct2DInterop() override { return this; }

    IDWriteFactory* WriteFactory() const override { return textEngine_.WriteFactory(); }

    ID2D1SolidColorBrush* SolidBrush(const ColorF& color) override {
        return canvas_.Brush(color);
    }

    void DrawTextNative(const std::wstring& text, const RectF& rect, IDWriteTextFormat* format,
                        const ColorF& color) override {
        canvas_.DrawTextNative(text, rect, format, color);
    }

    void DrawLayoutNative(IDWriteTextLayout* layout, PointF origin, const ColorF& color) override {
        canvas_.DrawLayoutNative(layout, origin, color);
    }

    void DrawBitmapNative(ID2D1Bitmap* bitmap, const RectF& rect, float opacity) override {
        canvas_.DrawBitmapNative(bitmap, rect, opacity);
    }

    ComPtr<ID2D1Bitmap> LoadBitmapFile(const std::filesystem::path& path) override {
        if (!wicFactory_ || !canvas_.HasTarget() || path.empty()) {
            return nullptr;
        }
        ComPtr<IWICBitmapDecoder> decoder;
        if (FAILED(wicFactory_->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                          WICDecodeMetadataCacheOnLoad,
                                                          &decoder))) {
            return nullptr;
        }
        ComPtr<IWICBitmapFrameDecode> frame;
        if (FAILED(decoder->GetFrame(0, &frame))) {
            return nullptr;
        }
        ComPtr<IWICFormatConverter> converter;
        if (FAILED(wicFactory_->CreateFormatConverter(&converter))) {
            return nullptr;
        }
        if (FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
                                         WICBitmapDitherTypeNone, nullptr, 0.0f,
                                         WICBitmapPaletteTypeCustom))) {
            return nullptr;
        }
        ComPtr<ID2D1Bitmap> bitmap;
        if (FAILED(canvas_.Target()->CreateBitmapFromWicBitmap(converter.Get(), nullptr, &bitmap))) {
            return nullptr;
        }
        return bitmap;
    }

    ComPtr<ID2D1Bitmap> CreateBitmapFromPixels(const uint32_t* pixels, uint32_t width,
                                               uint32_t height) override {
        if (!canvas_.HasTarget() || pixels == nullptr || width == 0 || height == 0) {
            return nullptr;
        }
        D2D1_BITMAP_PROPERTIES properties{};
        properties.pixelFormat =
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE);
        properties.dpiX = 96.0f;
        properties.dpiY = 96.0f;
        ComPtr<ID2D1Bitmap> bitmap;
        if (FAILED(canvas_.Target()->CreateBitmap(
                D2D1::SizeU(width, height), static_cast<const void*>(pixels), width * 4,
                &properties, &bitmap))) {
            return nullptr;
        }
        return bitmap;
    }

    ComPtr<ID2D1Bitmap> LoadScaledBitmapFile(const std::filesystem::path& path, float maxWidth,
                                             float maxHeight, uint32_t& sourceWidth,
                                             uint32_t& sourceHeight) override {
        sourceWidth = 0;
        sourceHeight = 0;
        if (!wicFactory_ || !canvas_.HasTarget() || path.empty() || maxWidth <= 0.0f ||
            maxHeight <= 0.0f) {
            return nullptr;
        }
        ComPtr<IWICBitmapDecoder> decoder;
        if (FAILED(wicFactory_->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                          WICDecodeMetadataCacheOnLoad,
                                                          &decoder))) {
            return nullptr;
        }
        ComPtr<IWICBitmapFrameDecode> frame;
        if (FAILED(decoder->GetFrame(0, &frame))) {
            return nullptr;
        }
        UINT frameWidth = 0;
        UINT frameHeight = 0;
        if (FAILED(frame->GetSize(&frameWidth, &frameHeight)) || frameWidth == 0 ||
            frameHeight == 0) {
            return nullptr;
        }
        sourceWidth = frameWidth;
        sourceHeight = frameHeight;

        const float scale =
            std::min(1.0f, std::min(maxWidth / static_cast<float>(frameWidth),
                                    maxHeight / static_cast<float>(frameHeight)));
        ComPtr<IWICBitmapSource> scaledSource;
        ComPtr<IWICBitmapScaler> scaler;
        if (scale < 1.0f) {
            const UINT targetWidth =
                static_cast<UINT>(std::max(1.0f, std::round(frameWidth * scale)));
            const UINT targetHeight =
                static_cast<UINT>(std::max(1.0f, std::round(frameHeight * scale)));
            if (FAILED(wicFactory_->CreateBitmapScaler(&scaler))) {
                return nullptr;
            }
            if (FAILED(scaler->Initialize(frame.Get(), targetWidth, targetHeight,
                                          WICBitmapInterpolationModeFant))) {
                return nullptr;
            }
            scaledSource = scaler;
        } else {
            scaledSource = frame;
        }

        ComPtr<IWICFormatConverter> converter;
        if (FAILED(wicFactory_->CreateFormatConverter(&converter))) {
            return nullptr;
        }
        if (FAILED(converter->Initialize(scaledSource.Get(), GUID_WICPixelFormat32bppPBGRA,
                                         WICBitmapDitherTypeNone, nullptr, 0.0f,
                                         WICBitmapPaletteTypeCustom))) {
            return nullptr;
        }
        ComPtr<ID2D1Bitmap> bitmap;
        if (FAILED(canvas_.Target()->CreateBitmapFromWicBitmap(converter.Get(), nullptr, &bitmap))) {
            return nullptr;
        }
        return bitmap;
    }

private:
    HWND hwnd_ = nullptr;
    ComPtr<ID2D1Factory> d2dFactory_;
    ComPtr<IWICImagingFactory> wicFactory_;
    D2DCanvas canvas_;
    D2DTextEngine textEngine_;
    RendererCapabilities capabilities_{};
};

}  // namespace

namespace detail {

std::unique_ptr<Renderer> CreateDirect2DRenderer() {
    return std::make_unique<D2DRenderer>();
}

}  // namespace detail

}  // namespace gfx
