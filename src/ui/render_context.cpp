#include "ui/render_context.h"

#include <d2d1helper.h>

#include <algorithm>

#include "gfx/renderer_factory.h"

namespace ui {
namespace {

gfx::ColorF ToGfx(const D2D1_COLOR_F& color) {
    return gfx::ColorF{color.r, color.g, color.b, color.a};
}

gfx::RectF ToGfx(const D2D1_RECT_F& rect) {
    return gfx::RectF{rect.left, rect.top, rect.right, rect.bottom};
}

gfx::PointF ToGfx(D2D1_POINT_2F point) {
    return gfx::PointF{point.x, point.y};
}

// 从 gfx 文本对象取出原生 DirectWrite 指针，并增加一次引用供兼容层的 ComPtr 持有。
template <typename Native>
ComPtr<Native> TakeNative(gfx::TextFormat& format) {
    ComPtr<Native> native;
    Native* raw = static_cast<Native*>(format.NativeHandle());
    if (raw != nullptr) {
        raw->AddRef();
        native.Attach(raw);
    }
    return native;
}

template <typename Native>
ComPtr<Native> TakeNative(gfx::TextLayout& layout) {
    ComPtr<Native> native;
    Native* raw = static_cast<Native*>(layout.NativeHandle());
    if (raw != nullptr) {
        raw->AddRef();
        native.Attach(raw);
    }
    return native;
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

RenderContext::RenderContext() = default;
RenderContext::~RenderContext() = default;

bool RenderContext::Initialize(HWND hwnd) {
    hwnd_ = hwnd;
    renderer_ = gfx::CreateRendererFromEnvironment();
    if (!renderer_ || !renderer_->Initialize(hwnd)) {
        renderer_.reset();
        d2dInterop_ = nullptr;
        return false;
    }
    d2dInterop_ = renderer_->Direct2DInterop();
    dpi_ = renderer_->Dpi();
    initialized_ = true;
    return true;
}

void RenderContext::Shutdown() {
    ResetTextFormats();
    if (renderer_) {
        renderer_->Shutdown();
        renderer_.reset();
    }
    d2dInterop_ = nullptr;
    initialized_ = false;
}

bool RenderContext::Resize(UINT widthPx, UINT heightPx, UINT dpi) {
    if (!initialized_ || !renderer_) {
        return false;
    }
    const bool recreated = renderer_->Resize(widthPx, heightPx, dpi);
    dpi_ = dpi;
    return recreated;
}

bool RenderContext::BeginDraw(HDC deviceContext, const RECT& targetRect) {
    if (!renderer_) {
        return false;
    }
    return renderer_->Graphics().BeginFrame(deviceContext, targetRect);
}

bool RenderContext::EndDraw() {
    if (!renderer_) {
        return false;
    }
    return renderer_->Graphics().EndFrame();
}

void RenderContext::Clear(const D2D1_COLOR_F& color) {
    if (renderer_) {
        renderer_->Graphics().Clear(ToGfx(color));
    }
}

ID2D1SolidColorBrush* RenderContext::SolidBrush(const D2D1_COLOR_F& color) {
    if (!d2dInterop_) {
        return nullptr;
    }
    return d2dInterop_->SolidBrush(ToGfx(color));
}

void RenderContext::FillRect(const D2D1_RECT_F& rect, const D2D1_COLOR_F& color) {
    if (renderer_) {
        renderer_->Graphics().FillRect(ToGfx(rect), ToGfx(color));
    }
}

void RenderContext::FillRoundedRect(const D2D1_RECT_F& rect, float radius,
                                    const D2D1_COLOR_F& color) {
    if (renderer_) {
        renderer_->Graphics().FillRoundedRect(ToGfx(rect), radius, ToGfx(color));
    }
}

void RenderContext::StrokeRoundedRect(const D2D1_RECT_F& rect, float radius,
                                      const D2D1_COLOR_F& color, float strokeWidth) {
    if (renderer_) {
        renderer_->Graphics().StrokeRoundedRect(ToGfx(rect), radius, ToGfx(color), strokeWidth);
    }
}

void RenderContext::DrawLine(D2D1_POINT_2F start, D2D1_POINT_2F end, const D2D1_COLOR_F& color,
                             float strokeWidth) {
    if (renderer_) {
        renderer_->Graphics().DrawLine(ToGfx(start), ToGfx(end), ToGfx(color), strokeWidth);
    }
}

void RenderContext::DrawTextBlock(const std::wstring& text, const D2D1_RECT_F& rect,
                                  IDWriteTextFormat* format, const D2D1_COLOR_F& color) {
    if (d2dInterop_) {
        d2dInterop_->DrawTextNative(text, ToGfx(rect), format, ToGfx(color));
    }
}

void RenderContext::DrawLayout(IDWriteTextLayout* layout, D2D1_POINT_2F origin,
                               const D2D1_COLOR_F& color) {
    if (d2dInterop_) {
        d2dInterop_->DrawLayoutNative(layout, ToGfx(origin), ToGfx(color));
    }
}

void RenderContext::PushClip(const D2D1_RECT_F& rect) {
    if (renderer_) {
        renderer_->Graphics().PushClip(ToGfx(rect));
    }
}

void RenderContext::PopClip() {
    if (renderer_) {
        renderer_->Graphics().PopClip();
    }
}

void RenderContext::DrawBitmap(ID2D1Bitmap* bitmap, const D2D1_RECT_F& rect, float opacity) {
    if (d2dInterop_) {
        d2dInterop_->DrawBitmapNative(bitmap, ToGfx(rect), opacity);
    }
}

ComPtr<ID2D1Bitmap> RenderContext::LoadBitmap(const std::filesystem::path& path) {
    if (!d2dInterop_) {
        return nullptr;
    }
    return d2dInterop_->LoadBitmapFile(path);
}

ComPtr<ID2D1Bitmap> RenderContext::LoadScaledBitmap(const std::filesystem::path& path,
                                                    float maxWidth, float maxHeight,
                                                    uint32_t& sourceWidth,
                                                    uint32_t& sourceHeight) {
    if (!d2dInterop_) {
        sourceWidth = 0;
        sourceHeight = 0;
        return nullptr;
    }
    return d2dInterop_->LoadScaledBitmapFile(path, maxWidth, maxHeight, sourceWidth,
                                             sourceHeight);
}

ComPtr<ID2D1Bitmap> RenderContext::CreateBitmapFromPixels(const uint32_t* pixels, uint32_t width,
                                                          uint32_t height) {
    if (!d2dInterop_) {
        return nullptr;
    }
    return d2dInterop_->CreateBitmapFromPixels(pixels, width, height);
}

void RenderContext::ResetTextFormats() {
    formats_ = TextFormats{};
    formatLookup_.clear();
    ownedFormats_.clear();
}

bool RenderContext::CreateTextFormats(const std::wstring& fontFamily, float bodySize,
                                      float lineSpacing, float noteSize) {
    if (!renderer_) {
        return false;
    }
    ResetTextFormats();
    noteSize = std::clamp(noteSize, 10.0f, 28.0f);
    gfx::TextEngine& engine = renderer_->Text();

    const auto createFormat = [this, &engine](const gfx::TextStyle& style,
                                              ComPtr<IDWriteTextFormat>& format) -> bool {
        std::unique_ptr<gfx::TextFormat> created = engine.CreateFormat(style);
        if (!created) {
            return false;
        }
        ComPtr<IDWriteTextFormat> native = TakeNative<IDWriteTextFormat>(*created);
        if (!native) {
            return false;
        }
        IDWriteTextFormat* raw = native.Get();
        formatLookup_[raw] = created.get();
        ownedFormats_.push_back(std::move(created));
        format = std::move(native);
        return true;
    };

    const auto makeStyle = [&fontFamily](float size, gfx::TextWeight weight,
                                         gfx::TextAlignment horizontalAlignment,
                                         gfx::ParagraphAlignment verticalAlignment,
                                         gfx::WordWrapping wrapping) {
        gfx::TextStyle style;
        style.fontFamily = fontFamily;
        style.fontSize = size;
        style.weight = weight;
        style.textAlignment = horizontalAlignment;
        style.paragraphAlignment = verticalAlignment;
        style.wrapping = wrapping;
        style.ellipsisTrimming = true;
        return style;
    };

    constexpr float kTitleSize = 27.0f;
    constexpr float kSectionSize = 19.0f;
    constexpr float kLabelSize = 16.0f;
    constexpr float kItemSize = 15.0f;
    constexpr float kSmallSize = 13.0f;
    constexpr float kButtonSize = 15.0f;
    constexpr float kHeroSize = 54.0f;

    gfx::TextStyle bodyStyle = makeStyle(bodySize, gfx::TextWeight::Normal,
                                         gfx::TextAlignment::Leading,
                                         gfx::ParagraphAlignment::Near, gfx::WordWrapping::Wrap);
    bodyStyle.uniformLineSpacing = true;
    bodyStyle.lineSpacing = bodySize * lineSpacing;
    bodyStyle.baseline = bodySize * 1.18f;

    gfx::TextStyle noteStyle = makeStyle(noteSize, gfx::TextWeight::Normal,
                                         gfx::TextAlignment::Leading,
                                         gfx::ParagraphAlignment::Near, gfx::WordWrapping::Wrap);
    noteStyle.uniformLineSpacing = true;
    noteStyle.lineSpacing = noteSize * 1.35f;
    noteStyle.baseline = noteSize * 1.18f;

    if (!createFormat(makeStyle(kTitleSize, gfx::TextWeight::SemiBold,
                                gfx::TextAlignment::Leading, gfx::ParagraphAlignment::Near,
                                gfx::WordWrapping::Wrap),
                      formats_.title) ||
        !createFormat(makeStyle(kSectionSize, gfx::TextWeight::SemiBold,
                                gfx::TextAlignment::Leading, gfx::ParagraphAlignment::Near,
                                gfx::WordWrapping::Wrap),
                      formats_.section) ||
        !createFormat(makeStyle(kLabelSize, gfx::TextWeight::SemiBold,
                                gfx::TextAlignment::Leading, gfx::ParagraphAlignment::Near,
                                gfx::WordWrapping::Wrap),
                      formats_.label) ||
        !createFormat(makeStyle(kItemSize, gfx::TextWeight::Normal,
                                gfx::TextAlignment::Leading, gfx::ParagraphAlignment::Near,
                                gfx::WordWrapping::NoWrap),
                      formats_.listItem) ||
        !createFormat(bodyStyle, formats_.body) ||
        !createFormat(makeStyle(kSmallSize, gfx::TextWeight::Normal,
                                gfx::TextAlignment::Leading, gfx::ParagraphAlignment::Near,
                                gfx::WordWrapping::NoWrap),
                      formats_.caption) ||
        !createFormat(makeStyle(kSmallSize, gfx::TextWeight::Normal,
                                gfx::TextAlignment::Trailing, gfx::ParagraphAlignment::Near,
                                gfx::WordWrapping::NoWrap),
                      formats_.captionRight) ||
        !createFormat(makeStyle(kSmallSize, gfx::TextWeight::Normal,
                                gfx::TextAlignment::Leading, gfx::ParagraphAlignment::Near,
                                gfx::WordWrapping::Wrap),
                      formats_.captionWrap) ||
        !createFormat(makeStyle(kLabelSize, gfx::TextWeight::Normal,
                                gfx::TextAlignment::Center, gfx::ParagraphAlignment::Center,
                                gfx::WordWrapping::NoWrap),
                      formats_.centered) ||
        !createFormat(makeStyle(kButtonSize, gfx::TextWeight::SemiBold,
                                gfx::TextAlignment::Center, gfx::ParagraphAlignment::Center,
                                gfx::WordWrapping::NoWrap),
                      formats_.button) ||
        !createFormat(makeStyle(kSmallSize, gfx::TextWeight::Normal,
                                gfx::TextAlignment::Center, gfx::ParagraphAlignment::Center,
                                gfx::WordWrapping::NoWrap),
                      formats_.buttonSmall) ||
        !createFormat(makeStyle(kHeroSize, gfx::TextWeight::Light,
                                gfx::TextAlignment::Center, gfx::ParagraphAlignment::Center,
                                gfx::WordWrapping::NoWrap),
                      formats_.hero) ||
        !createFormat(noteStyle, formats_.note)) {
        ResetTextFormats();
        return false;
    }
    return true;
}

ComPtr<IDWriteTextLayout> RenderContext::CreateLayout(const std::wstring& text,
                                                      IDWriteTextFormat* format, float width,
                                                      float maxHeight) {
    ComPtr<IDWriteTextLayout> layout;
    if (!renderer_ || !format || text.empty() || width <= 0.0f) {
        return layout;
    }
    const auto found = formatLookup_.find(format);
    if (found != formatLookup_.end()) {
        std::unique_ptr<gfx::TextLayout> created =
            renderer_->Text().CreateLayout(text, *found->second, width, maxHeight);
        if (created) {
            layout = TakeNative<IDWriteTextLayout>(*created);
        }
        return layout;
    }

    // 未通过 CreateTextFormats 创建的格式仍按原有路径创建，保持兼容。
    IDWriteFactory* factory = WriteFactory();
    if (!factory) {
        return layout;
    }
    factory->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format, width,
                              maxHeight, &layout);
    return layout;
}

ComPtr<IDWriteTextLayout> RenderContext::CreateBodyLayout(const std::wstring& text, float width,
                                                          float maxHeight) {
    return CreateLayout(text, formats_.body.Get(), width, maxHeight);
}

float RenderContext::MeasureTextWidth(const std::wstring& text, IDWriteTextFormat* format) const {
    if (!renderer_ || !format || text.empty()) {
        return 0.0f;
    }
    const auto found = formatLookup_.find(format);
    if (found != formatLookup_.end()) {
        return renderer_->Text().MeasureWidth(text, *found->second);
    }

    IDWriteFactory* factory = WriteFactory();
    if (!factory) {
        return 0.0f;
    }
    ComPtr<IDWriteTextLayout> layout;
    factory->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format, 4096.0f,
                              200.0f, &layout);
    if (!layout) {
        return 0.0f;
    }
    DWRITE_TEXT_METRICS metrics{};
    layout->GetMetrics(&metrics);
    return metrics.widthIncludingTrailingWhitespace;
}

std::vector<FontFamilyName> RenderContext::SystemFontFamilies() const {
    if (!renderer_) {
        return std::vector<FontFamilyName>();
    }
    return renderer_->Text().SystemFontFamilies();
}

IDWriteFactory* RenderContext::WriteFactory() const {
    return d2dInterop_ ? d2dInterop_->WriteFactory() : nullptr;
}

gfx::BackendKind RenderContext::Backend() const {
    return renderer_ ? renderer_->Capabilities().backend : gfx::BackendKind::Direct2D;
}

std::wstring RenderContext::BackendName() const {
    return gfx::BackendDisplayName(Backend());
}

}  // namespace ui
