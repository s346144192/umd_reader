#include "ui/render_context.h"

#include <d2d1helper.h>

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace ui {
namespace {

uint32_t ColorKey(const D2D1_COLOR_F& color) {
    const auto channel = [](float value) -> uint32_t {
        const float clamped = std::clamp(value, 0.0f, 1.0f);
        return static_cast<uint32_t>(clamped * 255.0f + 0.5f);
    };
    return (channel(color.a) << 24) | (channel(color.r) << 16) | (channel(color.g) << 8) |
           channel(color.b);
}

}  // namespace

ThemeColors ThemeByName(const std::wstring& name) {
    ThemeColors theme{};
    if (name == L"sepia") {
        theme.background = D2D1::ColorF(0xE8EFDC);
        theme.surface = D2D1::ColorF(0xF2F6E9);
        theme.surfaceAlt = D2D1::ColorF(0xD9E3CB);
        theme.text = D2D1::ColorF(0x25301F);
        theme.secondaryText = D2D1::ColorF(0x5F6E55);
        theme.accent = D2D1::ColorF(0x4A6B3F);
        theme.accentText = D2D1::ColorF(0xFFFFFF);
        theme.divider = D2D1::ColorF(0xC6D3B6);
        theme.highlight = D2D1::ColorF(0xE7C76A, 0.55f);
        return theme;
    }
    if (name == L"dark") {
        // 深色主题保持原有配色，不使用蓝色点缀。
        theme.background = D2D1::ColorF(0x16181A);
        theme.surface = D2D1::ColorF(0x212427);
        theme.surfaceAlt = D2D1::ColorF(0x2B3034);
        theme.text = D2D1::ColorF(0xE3E4E5);
        theme.secondaryText = D2D1::ColorF(0x9AA0A6);
        theme.accent = D2D1::ColorF(0x7CC7A4);
        theme.accentText = D2D1::ColorF(0x102018);
        theme.divider = D2D1::ColorF(0x343A3F);
        theme.highlight = D2D1::ColorF(0xC8A23A, 0.45f);
        return theme;
    }

    // 浅色主题保留原有底色，主色改为淡咖啡色。
    theme.background = D2D1::ColorF(0xF6F4EF);
    theme.surface = D2D1::ColorF(0xFFFFFF);
    theme.surfaceAlt = D2D1::ColorF(0xEDE8DF);
    theme.text = D2D1::ColorF(0x24211D);
    theme.secondaryText = D2D1::ColorF(0x756F66);
    theme.accent = D2D1::ColorF(0xA9825F);
    theme.accentText = D2D1::ColorF(0xFFFFFF);
    theme.divider = D2D1::ColorF(0xE1DACD);
    theme.highlight = D2D1::ColorF(0xE9C96A, 0.55f);
    return theme;
}

bool RenderContext::Initialize(HWND hwnd) {
    hwnd_ = hwnd;
    dpi_ = GetDpiForWindow(hwnd_);

    const HRESULT factoryResult = D2D1CreateFactory(
        D2D1_FACTORY_TYPE_SINGLE_THREADED, d2dFactory_.GetAddressOf());
    if (FAILED(factoryResult)) {
        return false;
    }
    const HRESULT writeResult =
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                            reinterpret_cast<IUnknown**>(writeFactory_.GetAddressOf()));
    if (FAILED(writeResult)) {
        return false;
    }
    const HRESULT wicResult = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                               CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wicFactory_));
    if (FAILED(wicResult)) {
        return false;
    }

    initialized_ = true;
    return CreateDeviceResources();
}

void RenderContext::Shutdown() {
    DiscardDeviceResources();
    formats_ = TextFormats{};
    brushCache_.clear();
    wicFactory_.Reset();
    writeFactory_.Reset();
    d2dFactory_.Reset();
    initialized_ = false;
}

bool RenderContext::CreateDeviceResources() {
    if (!d2dFactory_) {
        return false;
    }
    renderTarget_.Reset();
    const D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
        static_cast<float>(dpi_), static_cast<float>(dpi_));
    const HRESULT result = d2dFactory_->CreateDCRenderTarget(
        &properties, renderTarget_.GetAddressOf());
    if (FAILED(result) || !renderTarget_) {
        renderTarget_.Reset();
        return false;
    }
    renderTarget_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    renderTarget_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
    brushCache_.clear();
    return true;
}

void RenderContext::DiscardDeviceResources() {
    brushCache_.clear();
    renderTarget_.Reset();
}

bool RenderContext::Resize(UINT widthPx, UINT heightPx, UINT dpi) {
    if (!initialized_) {
        return false;
    }
    (void)widthPx;
    (void)heightPx;
    if (dpi != dpi_ || !renderTarget_) {
        dpi_ = dpi;
        renderTarget_.Reset();
        brushCache_.clear();
        return CreateDeviceResources();
    }
    return false;
}

bool RenderContext::BeginDraw(HDC deviceContext, const RECT& targetRect) {
    if (!renderTarget_ && !CreateDeviceResources()) {
        return false;
    }
    if (FAILED(renderTarget_->BindDC(deviceContext, &targetRect))) {
        return false;
    }
    renderTarget_->BeginDraw();
    return true;
}

bool RenderContext::EndDraw() {
    if (!renderTarget_) {
        return false;
    }
    const HRESULT result = renderTarget_->EndDraw();
    if (result == D2DERR_RECREATE_TARGET) {
        DiscardDeviceResources();
        return false;
    }
    return true;
}

void RenderContext::Clear(const D2D1_COLOR_F& color) {
    if (renderTarget_) {
        renderTarget_->Clear(color);
    }
}

ID2D1SolidColorBrush* RenderContext::SolidBrush(const D2D1_COLOR_F& color) {
    if (!renderTarget_) {
        return nullptr;
    }
    const uint32_t key = ColorKey(color);
    const auto found = brushCache_.find(key);
    if (found != brushCache_.end()) {
        return found->second.Get();
    }
    ComPtr<ID2D1SolidColorBrush> brush;
    renderTarget_->CreateSolidColorBrush(color, &brush);
    if (!brush) {
        return nullptr;
    }
    brushCache_.emplace(key, brush);
    return brush.Get();
}

void RenderContext::FillRect(const D2D1_RECT_F& rect, const D2D1_COLOR_F& color) {
    ID2D1SolidColorBrush* brush = SolidBrush(color);
    if (brush) {
        renderTarget_->FillRectangle(rect, brush);
    }
}

void RenderContext::FillRoundedRect(const D2D1_RECT_F& rect, float radius,
                                    const D2D1_COLOR_F& color) {
    ID2D1SolidColorBrush* brush = SolidBrush(color);
    if (brush) {
        renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush);
    }
}

void RenderContext::StrokeRoundedRect(const D2D1_RECT_F& rect, float radius,
                                      const D2D1_COLOR_F& color, float strokeWidth) {
    ID2D1SolidColorBrush* brush = SolidBrush(color);
    if (brush) {
        renderTarget_->DrawRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush,
                                           strokeWidth);
    }
}

void RenderContext::DrawLine(D2D1_POINT_2F start, D2D1_POINT_2F end, const D2D1_COLOR_F& color,
                             float strokeWidth) {
    ID2D1SolidColorBrush* brush = SolidBrush(color);
    if (brush) {
        renderTarget_->DrawLine(start, end, brush, strokeWidth);
    }
}

void RenderContext::DrawTextBlock(const std::wstring& text, const D2D1_RECT_F& rect,
                                  IDWriteTextFormat* format, const D2D1_COLOR_F& color) {
    ID2D1SolidColorBrush* brush = SolidBrush(color);
    if (brush && format) {
        renderTarget_->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), format, rect,
                                 brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
}

void RenderContext::DrawLayout(IDWriteTextLayout* layout, D2D1_POINT_2F origin,
                               const D2D1_COLOR_F& color) {
    ID2D1SolidColorBrush* brush = SolidBrush(color);
    if (brush && layout) {
        renderTarget_->DrawTextLayout(origin, layout, brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
}

void RenderContext::PushClip(const D2D1_RECT_F& rect) {
    if (renderTarget_) {
        renderTarget_->PushAxisAlignedClip(rect, D2D1_ANTIALIAS_MODE_ALIASED);
    }
}

void RenderContext::PopClip() {
    if (renderTarget_) {
        renderTarget_->PopAxisAlignedClip();
    }
}

void RenderContext::DrawBitmap(ID2D1Bitmap* bitmap, const D2D1_RECT_F& rect, float opacity) {
    if (!renderTarget_ || !bitmap) {
        return;
    }
    const D2D1_SIZE_F source = bitmap->GetSize();
    const float destinationWidth = rect.right - rect.left;
    const float destinationHeight = rect.bottom - rect.top;
    if (source.width <= 0.0f || source.height <= 0.0f || destinationWidth <= 0.0f ||
        destinationHeight <= 0.0f) {
        return;
    }

    const float scale =
        std::max(destinationWidth / source.width, destinationHeight / source.height);
    const float visibleWidth = destinationWidth / scale;
    const float visibleHeight = destinationHeight / scale;
    const D2D1_RECT_F sourceRect =
        D2D1::RectF((source.width - visibleWidth) / 2.0f, (source.height - visibleHeight) / 2.0f,
                    (source.width + visibleWidth) / 2.0f, (source.height + visibleHeight) / 2.0f);
    renderTarget_->DrawBitmap(bitmap, rect, opacity, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,
                              sourceRect);
}

ComPtr<ID2D1Bitmap> RenderContext::LoadBitmap(const std::filesystem::path& path) {
    if (!wicFactory_ || !renderTarget_ || path.empty()) {
        return nullptr;
    }
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(wicFactory_->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                      WICDecodeMetadataCacheOnLoad, &decoder))) {
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
    if (FAILED(renderTarget_->CreateBitmapFromWicBitmap(converter.Get(), nullptr, &bitmap))) {
        return nullptr;
    }
    return bitmap;
}

ComPtr<ID2D1Bitmap> RenderContext::LoadScaledBitmap(const std::filesystem::path& path,
                                                    float maxWidth, float maxHeight,
                                                    uint32_t& sourceWidth,
                                                    uint32_t& sourceHeight) {
    sourceWidth = 0;
    sourceHeight = 0;
    if (!wicFactory_ || !renderTarget_ || path.empty() || maxWidth <= 0.0f || maxHeight <= 0.0f) {
        return nullptr;
    }
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(wicFactory_->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                      WICDecodeMetadataCacheOnLoad, &decoder))) {
        return nullptr;
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) {
        return nullptr;
    }
    UINT frameWidth = 0;
    UINT frameHeight = 0;
    if (FAILED(frame->GetSize(&frameWidth, &frameHeight)) || frameWidth == 0 || frameHeight == 0) {
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
        const UINT targetWidth = static_cast<UINT>(std::max(1.0f, std::round(frameWidth * scale)));
        const UINT targetHeight = static_cast<UINT>(std::max(1.0f, std::round(frameHeight * scale)));
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
    if (FAILED(renderTarget_->CreateBitmapFromWicBitmap(converter.Get(), nullptr, &bitmap))) {
        return nullptr;
    }
    return bitmap;
}

bool RenderContext::CreateTextFormats(const std::wstring& fontFamily, float bodySize,
                                      float lineSpacing, float noteSize) {
    if (!writeFactory_) {
        return false;
    }
    noteSize = std::clamp(noteSize, 10.0f, 28.0f);

    const auto createFormat = [this, &fontFamily](
                                  float size, DWRITE_FONT_WEIGHT weight,
                                  DWRITE_TEXT_ALIGNMENT horizontalAlignment,
                                  DWRITE_PARAGRAPH_ALIGNMENT verticalAlignment,
                                  DWRITE_WORD_WRAPPING wrapping,
                                  ComPtr<IDWriteTextFormat>& format) -> bool {
        format.Reset();
        HRESULT result = writeFactory_->CreateTextFormat(
            fontFamily.c_str(), nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, size, L"zh-cn", &format);
        if (FAILED(result) || !format) {
            result = writeFactory_->CreateTextFormat(
                L"Segoe UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                size, L"zh-cn", &format);
            if (FAILED(result) || !format) {
                return false;
            }
        }
        format->SetTextAlignment(horizontalAlignment);
        format->SetParagraphAlignment(verticalAlignment);
        format->SetWordWrapping(wrapping);

        DWRITE_TRIMMING trimming{};
        trimming.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
        ComPtr<IDWriteInlineObject> ellipsis;
        if (SUCCEEDED(writeFactory_->CreateEllipsisTrimmingSign(format.Get(), &ellipsis))) {
            format->SetTrimming(&trimming, ellipsis.Get());
        }
        return true;
    };

    constexpr float kTitleSize = 27.0f;
    constexpr float kSectionSize = 19.0f;
    constexpr float kLabelSize = 16.0f;
    constexpr float kItemSize = 15.0f;
    constexpr float kSmallSize = 13.0f;
    constexpr float kButtonSize = 15.0f;
    constexpr float kHeroSize = 54.0f;

    if (!createFormat(kTitleSize, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING,
                      DWRITE_PARAGRAPH_ALIGNMENT_NEAR, DWRITE_WORD_WRAPPING_WRAP,
                      formats_.title) ||
        !createFormat(kSectionSize, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING,
                      DWRITE_PARAGRAPH_ALIGNMENT_NEAR, DWRITE_WORD_WRAPPING_WRAP,
                      formats_.section) ||
        !createFormat(kLabelSize, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING,
                      DWRITE_PARAGRAPH_ALIGNMENT_NEAR, DWRITE_WORD_WRAPPING_WRAP,
                      formats_.label) ||
        !createFormat(kItemSize, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING,
                      DWRITE_PARAGRAPH_ALIGNMENT_NEAR, DWRITE_WORD_WRAPPING_NO_WRAP,
                      formats_.listItem) ||
        !createFormat(bodySize, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING,
                      DWRITE_PARAGRAPH_ALIGNMENT_NEAR, DWRITE_WORD_WRAPPING_WRAP, formats_.body) ||
        !createFormat(kSmallSize, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING,
                      DWRITE_PARAGRAPH_ALIGNMENT_NEAR, DWRITE_WORD_WRAPPING_NO_WRAP,
                      formats_.caption) ||
        !createFormat(kSmallSize, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING,
                      DWRITE_PARAGRAPH_ALIGNMENT_NEAR, DWRITE_WORD_WRAPPING_NO_WRAP,
                      formats_.captionRight) ||
        !createFormat(kLabelSize, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER,
                      DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP,
                      formats_.centered) ||
        !createFormat(kButtonSize, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER,
                      DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP,
                      formats_.button) ||
        !createFormat(kSmallSize, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER,
                      DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP,
                      formats_.buttonSmall) ||
        !createFormat(kHeroSize, DWRITE_FONT_WEIGHT_LIGHT, DWRITE_TEXT_ALIGNMENT_CENTER,
                      DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP,
                      formats_.hero) ||
        !createFormat(noteSize, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING,
                      DWRITE_PARAGRAPH_ALIGNMENT_NEAR, DWRITE_WORD_WRAPPING_WRAP,
                      formats_.note)) {
        return false;
    }

    formats_.body->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, bodySize * lineSpacing,
                                  bodySize * 1.18f);
    formats_.note->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, noteSize * 1.35f,
                                  noteSize * 1.18f);
    return true;
}

ComPtr<IDWriteTextLayout> RenderContext::CreateLayout(const std::wstring& text,
                                                      IDWriteTextFormat* format, float width,
                                                      float maxHeight) {
    ComPtr<IDWriteTextLayout> layout;
    if (!writeFactory_ || !format || text.empty() || width <= 0.0f) {
        return layout;
    }
    writeFactory_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format, width,
                                    maxHeight, &layout);
    return layout;
}

ComPtr<IDWriteTextLayout> RenderContext::CreateBodyLayout(const std::wstring& text, float width,
                                                          float maxHeight) {
    return CreateLayout(text, formats_.body.Get(), width, maxHeight);
}

float RenderContext::MeasureTextWidth(const std::wstring& text, IDWriteTextFormat* format) const {
    if (!writeFactory_ || !format || text.empty()) {
        return 0.0f;
    }
    ComPtr<IDWriteTextLayout> layout;
    writeFactory_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format, 4096.0f,
                                    200.0f, &layout);
    if (!layout) {
        return 0.0f;
    }
    DWRITE_TEXT_METRICS metrics{};
    layout->GetMetrics(&metrics);
    return metrics.widthIncludingTrailingWhitespace;
}

std::vector<FontFamilyName> RenderContext::SystemFontFamilies() const {
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
        if (strings == nullptr ||
            FAILED(strings->FindLocaleName(locale, &index, &exists)) || !exists) {
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
        const UINT32 nameCount = names->GetCount();
        if (nameCount == 0) {
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

        // 界面显示优先中文名，设置保存英文族名，DirectWrite 与旧设置都能直接匹配。
        const UINT32 displayIndex = zhIndex != UINT32_MAX ? zhIndex : (enIndex != UINT32_MAX ? enIndex : 0);
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

}  // namespace ui
