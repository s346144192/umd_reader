#include "reader/fixed_page_reader.h"

#include <d2d1helper.h>

#include <algorithm>
#include <cmath>
#include <cfloat>

namespace reader {
namespace {

// 页面尺寸缺失时的默认值，对应 A4 纵向。
constexpr float kDefaultPageWidthPt = 595.0f;
constexpr float kDefaultPageHeightPt = 842.0f;
// 两次渲染比例的差值在此范围内视为同一比例，避免窗口尺寸微小变化时反复渲染。
constexpr float kScaleEpsilon = 0.01f;
// 每次缩放调整的幅度。
constexpr float kZoomStep = 0.2f;
// 页面四周留出的空白，避免页面紧贴工具栏与底栏。
constexpr float kPageMargin = 16.0f;
// 双页视图中两页之间的间隔，单位为 DIP。
constexpr float kSpreadGap = 12.0f;
// 选择高亮的填充色：半透明强调色，覆盖在页面位图之上。
constexpr D2D1_COLOR_F kSelectionColor = {0.20f, 0.47f, 0.87f, 0.35f};

// 把页面坐标（原点左下，单位 point）的矩形换算为显示矩形（DIP，原点左上）。
D2D1_RECT_F ToDisplayRect(const D2D1_RECT_F& rectPt, const D2D1_RECT_F& pageRect, float widthPt,
                          float heightPt) {
    const float scaleX = (pageRect.right - pageRect.left) / std::max(widthPt, 1.0f);
    const float scaleY = (pageRect.bottom - pageRect.top) / std::max(heightPt, 1.0f);
    return D2D1::RectF(pageRect.left + rectPt.left * scaleX,
                       pageRect.top + (heightPt - rectPt.top) * scaleY,
                       pageRect.left + rectPt.right * scaleX,
                       pageRect.top + (heightPt - rectPt.bottom) * scaleY);
}

}  // namespace

bool FixedPageReader::Open(const std::filesystem::path& path, int pageIndex,
                           std::wstring& error) {
    Close();
    if (!document_.Open(path, error)) {
        return false;
    }
    if (document_.PageCount() <= 0) {
        error = L"文档不包含可显示的页面";
        Close();
        return false;
    }
    currentPage_ = std::clamp(pageIndex, 0, document_.PageCount() - 1);
    if (layout_ == PageLayout::Spread) {
        // 双页视图以偶数页为左页，定位到包含目标页的一组。
        currentPage_ -= currentPage_ % 2;
    }
    zoom_ = 1.0f;
    panX_ = 0.0f;
    panY_ = 0.0f;
    return true;
}

void FixedPageReader::Close() {
    InvalidateBitmap();
    document_.Close();
    currentPage_ = 0;
    zoom_ = 1.0f;
    panX_ = 0.0f;
    panY_ = 0.0f;
    textLayers_.clear();
    textLayerOrder_.clear();
    InvalidateSelection();
}

int FixedPageReader::VisiblePageCount() const {
    if (!document_.IsOpen() || document_.PageCount() <= 0) {
        return 0;
    }
    if (layout_ == PageLayout::Single) {
        return 1;
    }
    return std::min(kMaxVisiblePages, document_.PageCount() - currentPage_);
}

void FixedPageReader::SetCurrentPage(int pageIndex) {
    if (!document_.IsOpen() || document_.PageCount() <= 0) {
        currentPage_ = 0;
        return;
    }
    const int clamped = std::clamp(pageIndex, 0, document_.PageCount() - 1);
    const int aligned = layout_ == PageLayout::Spread ? clamped - clamped % 2 : clamped;
    if (aligned != currentPage_) {
        InvalidateSelection();
        panX_ = 0.0f;
        panY_ = 0.0f;
    }
    currentPage_ = aligned;
}

bool FixedPageReader::NextPage() {
    if (!document_.IsOpen() || document_.PageCount() <= 0) {
        return false;
    }
    const int step = layout_ == PageLayout::Spread ? kMaxVisiblePages : 1;
    if (currentPage_ + step >= document_.PageCount()) {
        return false;
    }
    currentPage_ += step;
    InvalidateSelection();
    panX_ = 0.0f;
    panY_ = 0.0f;
    return true;
}

bool FixedPageReader::PreviousPage() {
    if (!document_.IsOpen() || currentPage_ <= 0) {
        return false;
    }
    const int step = layout_ == PageLayout::Spread ? kMaxVisiblePages : 1;
    currentPage_ = std::max(0, currentPage_ - step);
    InvalidateSelection();
    panX_ = 0.0f;
    panY_ = 0.0f;
    return true;
}

void FixedPageReader::SetLayout(PageLayout layout) {
    if (layout_ == layout) {
        return;
    }
    layout_ = layout;
    if (layout_ == PageLayout::Spread) {
        // 转入双页视图时保证当前页仍在显示范围内，左右两页成组稳定。
        currentPage_ -= currentPage_ % 2;
    }
    // 成组变化后页面宽度可能改变，先回到居中位置。
    panX_ = 0.0f;
    panY_ = 0.0f;
    InvalidateBitmap();
    InvalidateSelection();
}

void FixedPageReader::SetZoom(float zoom) {
    if (!std::isfinite(zoom)) {
        return;
    }
    const float clamped = std::clamp(zoom, kMinZoom, kMaxZoom);
    if (std::fabs(clamped - zoom_) < 0.0001f) {
        return;
    }
    zoom_ = clamped;
    // 缩放以视图中心为基准，先回到居中位置，避免平移量在不同比例下失真。
    panX_ = 0.0f;
    panY_ = 0.0f;
}

void FixedPageReader::ZoomIn() {
    SetZoom(zoom_ + kZoomStep);
}

void FixedPageReader::ZoomOut() {
    SetZoom(zoom_ - kZoomStep);
}

void FixedPageReader::ResetZoom() {
    zoom_ = 1.0f;
    panX_ = 0.0f;
    panY_ = 0.0f;
}

bool FixedPageReader::PanBy(float dxDip, float dyDip) {
    if (!IsZoomed()) {
        return false;
    }
    const float previousX = panX_;
    const float previousY = panY_;
    panX_ += dxDip;
    panY_ += dyDip;
    ClampPan();
    return std::fabs(panX_ - previousX) > 0.001f || std::fabs(panY_ - previousY) > 0.001f;
}

void FixedPageReader::ClampPan() {
    D2D1_RECT_F rects[kMaxVisiblePages]{};
    const int visible = ComputeCenteredPageRects(lastViewport_, rects);
    if (visible <= 0) {
        panX_ = 0.0f;
        panY_ = 0.0f;
        return;
    }
    D2D1_RECT_F bounds = rects[0];
    for (int i = 1; i < visible; ++i) {
        bounds.left = std::min(bounds.left, rects[i].left);
        bounds.top = std::min(bounds.top, rects[i].top);
        bounds.right = std::max(bounds.right, rects[i].right);
        bounds.bottom = std::max(bounds.bottom, rects[i].bottom);
    }
    const float overflowX =
        (bounds.right - bounds.left) - (lastViewport_.right - lastViewport_.left);
    const float overflowY =
        (bounds.bottom - bounds.top) - (lastViewport_.bottom - lastViewport_.top);
    const float limitX = overflowX > 0.0f ? overflowX * 0.5f : 0.0f;
    const float limitY = overflowY > 0.0f ? overflowY * 0.5f : 0.0f;
    panX_ = std::clamp(panX_, -limitX, limitX);
    panY_ = std::clamp(panY_, -limitY, limitY);
}

void FixedPageReader::PageSize(int pageIndex, float& widthPt, float& heightPt) const {
    widthPt = kDefaultPageWidthPt;
    heightPt = kDefaultPageHeightPt;
    if (!document_.IsOpen()) {
        return;
    }
    const std::vector<pdf::PageInfo>& pages = document_.Pages();
    if (pageIndex < 0 || pageIndex >= static_cast<int>(pages.size())) {
        return;
    }
    const pdf::PageInfo& info = pages[static_cast<size_t>(pageIndex)];
    if (info.widthPt > 0.0f) {
        widthPt = info.widthPt;
    }
    if (info.heightPt > 0.0f) {
        heightPt = info.heightPt;
    }
}

int FixedPageReader::ComputeCenteredPageRects(const D2D1_RECT_F& viewport,
                                              D2D1_RECT_F rects[kMaxVisiblePages]) const {
    const int visible = VisiblePageCount();
    if (visible <= 0) {
        return 0;
    }
    const float availableWidth =
        std::max(1.0f, (viewport.right - viewport.left) - kPageMargin * 2.0f);
    const float availableHeight =
        std::max(1.0f, (viewport.bottom - viewport.top) - kPageMargin * 2.0f);

    float widthsPt[kMaxVisiblePages] = {kDefaultPageWidthPt, kDefaultPageWidthPt};
    float heightsPt[kMaxVisiblePages] = {kDefaultPageHeightPt, kDefaultPageHeightPt};
    float totalWidthPt = 0.0f;
    float maximumHeightPt = 0.0f;
    for (int i = 0; i < visible; ++i) {
        PageSize(currentPage_ + i, widthsPt[i], heightsPt[i]);
        totalWidthPt += widthsPt[i];
        maximumHeightPt = std::max(maximumHeightPt, heightsPt[i]);
    }
    const float gap = visible > 1 ? kSpreadGap : 0.0f;
    // 当前视图整体适应可用区域后，再按缩放比例放大或缩小；比例 1.0 即适应视图。
    const float fitScale = std::min((availableWidth - gap) / totalWidthPt,
                                    availableHeight / maximumHeightPt) *
                           zoom_;
    const float scale = std::max(fitScale, 0.01f);

    float totalWidthDip = gap;
    float maximumHeightDip = 0.0f;
    for (int i = 0; i < visible; ++i) {
        totalWidthDip += widthsPt[i] * scale;
        maximumHeightDip = std::max(maximumHeightDip, heightsPt[i] * scale);
    }
    float left = viewport.left + (viewport.right - viewport.left - totalWidthDip) * 0.5f;
    const float top = viewport.top + (viewport.bottom - viewport.top - maximumHeightDip) * 0.5f;
    for (int i = 0; i < visible; ++i) {
        const float widthDip = widthsPt[i] * scale;
        const float heightDip = heightsPt[i] * scale;
        // 各页按自身高度在视图内纵向居中，纸张尺寸不一致时也不会错位。
        const float pageTop = top + (maximumHeightDip - heightDip) * 0.5f;
        rects[i] = D2D1::RectF(left, pageTop, left + widthDip, pageTop + heightDip);
        left += widthDip + (i + 1 < visible ? gap : 0.0f);
    }
    return visible;
}

int FixedPageReader::ComputePageRects(const D2D1_RECT_F& viewport,
                                      D2D1_RECT_F rects[kMaxVisiblePages]) const {
    const int visible = ComputeCenteredPageRects(viewport, rects);
    // 放大后按平移量整体移动页面；平移量在 ClampPan 中已限制在有效范围内。
    for (int i = 0; i < visible; ++i) {
        rects[i].left += panX_;
        rects[i].right += panX_;
        rects[i].top += panY_;
        rects[i].bottom += panY_;
    }
    return visible;
}

D2D1_RECT_F FixedPageReader::PageRect(const D2D1_RECT_F& viewport) const {
    D2D1_RECT_F rects[kMaxVisiblePages]{};
    const int visible = ComputePageRects(viewport, rects);
    if (visible <= 0) {
        return D2D1::RectF(viewport.left, viewport.top, viewport.left, viewport.top);
    }
    D2D1_RECT_F bounds = rects[0];
    for (int i = 1; i < visible; ++i) {
        bounds.left = std::min(bounds.left, rects[i].left);
        bounds.top = std::min(bounds.top, rects[i].top);
        bounds.right = std::max(bounds.right, rects[i].right);
        bounds.bottom = std::max(bounds.bottom, rects[i].bottom);
    }
    return bounds;
}

bool FixedPageReader::EnsureBitmap(ui::RenderContext& renderer, int slot, int pageIndex,
                                   float renderScale) {
    if (slot < 0 || slot >= kMaxVisiblePages) {
        return false;
    }
    if (bitmaps_[slot] && bitmapPages_[slot] == pageIndex &&
        std::fabs(bitmapScales_[slot] - renderScale) <= kScaleEpsilon) {
        return true;
    }
    pdf::PageBitmapPtr rendered;
    std::wstring error;
    if (!document_.RenderPage(pageIndex, renderScale, rendered, error) || !rendered) {
        return false;
    }
    bitmaps_[slot] = renderer.CreateBitmapFromPixels(
        reinterpret_cast<const uint32_t*>(rendered->pixels.data()),
        static_cast<uint32_t>(rendered->width), static_cast<uint32_t>(rendered->height));
    bitmapPages_[slot] = pageIndex;
    bitmapScales_[slot] = renderScale;
    return bitmaps_[slot] != nullptr;
}

void FixedPageReader::Draw(ui::RenderContext& renderer, const D2D1_RECT_F& viewport) {
    lastViewport_ = viewport;
    if (!document_.IsOpen() || document_.PageCount() <= 0) {
        return;
    }
    D2D1_RECT_F rects[kMaxVisiblePages]{};
    const int visible = ComputePageRects(viewport, rects);
    for (int slot = 0; slot < visible; ++slot) {
        const int pageIndex = currentPage_ + slot;
        float widthPt = kDefaultPageWidthPt;
        float heightPt = kDefaultPageHeightPt;
        PageSize(pageIndex, widthPt, heightPt);
        // 渲染比例：每 point 对应的像素数，使位图像素尺寸与显示尺寸在当前 DPI 下一致。
        const float displayWidth = std::max(1.0f, rects[slot].right - rects[slot].left);
        const float renderScale = displayWidth / widthPt * renderer.DpiScale();
        if (EnsureBitmap(renderer, slot, pageIndex, renderScale)) {
            renderer.DrawBitmap(bitmaps_[slot].Get(), rects[slot], 1.0f);
        }
    }
    DrawSelection(renderer);
}

void FixedPageReader::InvalidateBitmap() {
    for (int slot = 0; slot < kMaxVisiblePages; ++slot) {
        bitmaps_[slot].Reset();
        bitmapPages_[slot] = -1;
        bitmapScales_[slot] = 0.0f;
    }
}

bool FixedPageReader::PageViewRect(int slot, D2D1_RECT_F& rect, float& widthPt,
                                   float& heightPt) const {
    D2D1_RECT_F rects[kMaxVisiblePages]{};
    const int visible = ComputePageRects(lastViewport_, rects);
    if (slot < 0 || slot >= visible) {
        return false;
    }
    rect = rects[slot];
    PageSize(currentPage_ + slot, widthPt, heightPt);
    return true;
}

const PageTextLayer* FixedPageReader::TextLayerForPage(int pageIndex, std::wstring& error) {
    const auto cached = textLayers_.find(pageIndex);
    if (cached != textLayers_.end()) {
        return &cached->second;
    }

    float widthPt = kDefaultPageWidthPt;
    float heightPt = kDefaultPageHeightPt;
    PageSize(pageIndex, widthPt, heightPt);

    PageTextLayer layer;
    // 先取 PDF 内嵌文本层；有足够文字时直接使用，无需识别。
    std::vector<pdf::TextLine> lines;
    if (!document_.ReadPageLines(pageIndex, lines, error)) {
        return nullptr;
    }
    size_t embeddedCharacters = 0;
    for (const pdf::TextLine& line : lines) {
        for (const wchar_t character : line.text) {
            if (character != L' ' && character != L'\t') {
                ++embeddedCharacters;
            }
        }
    }
    // 扫描页自带的文字层来自扫描软件，可能缺少拼音、与图像错位或存在乱码；
    // 字体映射缺失的页面也会把拼音解析成符号串。这两类页面改用系统 OCR 重新识别，
    // 保证选中的文字与图像一致且包含拼音。
    const pdf::PageInfo& pageInfo = document_.Pages()[static_cast<size_t>(pageIndex)];
    const bool unreliableTextLayer = pageInfo.hasFullPageImage || pageInfo.textLayerSuspicious;
    if (!unreliableTextLayer && embeddedCharacters >= kMinimumEmbeddedCharacters) {
        layer.fromOcr = false;
        layer.lines.reserve(lines.size());
        for (const pdf::TextLine& line : lines) {
            TextLayerLine item;
            item.text = line.text;
            item.charRects.reserve(line.boxes.size());
            for (const pdf::CharBox& box : line.boxes) {
                item.charRects.push_back(D2D1::RectF(box.left, box.top, box.right, box.bottom));
            }
            // 个别字符可能没有外框，用零尺寸补齐，保证与文本等长。
            while (item.charRects.size() < item.text.size()) {
                item.charRects.push_back(D2D1::RectF());
            }
            layer.lines.push_back(std::move(item));
        }
    } else {
        // 没有可用文本层：渲染页面后交给系统 OCR 识别。
        pdf::PageBitmapPtr bitmap;
        if (!document_.RenderPage(pageIndex, kOcrRenderScale, bitmap, error) || !bitmap) {
            return nullptr;
        }
        if (!RecognizePageText(*bitmap, widthPt, heightPt, layer, error)) {
            return nullptr;
        }
    }

    const auto inserted = textLayers_.emplace(pageIndex, std::move(layer));
    textLayerOrder_.push_back(pageIndex);
    // 超出缓存上限时丢弃最早且当前不在显示的页，避免整本识别结果常驻内存。
    while (textLayerOrder_.size() > kTextLayerCacheLimit) {
        bool removed = false;
        for (auto it = textLayerOrder_.begin(); it != textLayerOrder_.end(); ++it) {
            if (*it == currentPage_ || *it == currentPage_ + 1) {
                continue;
            }
            textLayers_.erase(*it);
            textLayerOrder_.erase(it);
            removed = true;
            break;
        }
        if (!removed) {
            break;
        }
    }
    return &inserted.first->second;
}

bool FixedPageReader::EnsureTextLayer(std::wstring& error) {
    error.clear();
    flatChars_.clear();
    selectionStart_ = -1;
    selectionEnd_ = -1;
    if (!document_.IsOpen()) {
        error = L"文档尚未打开";
        return false;
    }
    const int visible = VisiblePageCount();
    std::wstring lastError;
    bool hasText = false;
    for (int slot = 0; slot < visible; ++slot) {
        const int pageIndex = currentPage_ + slot;
        std::wstring pageError;
        const PageTextLayer* layer = TextLayerForPage(pageIndex, pageError);
        if (layer == nullptr) {
            if (lastError.empty()) {
                lastError = pageError;
            }
            continue;
        }
        hasText = hasText || layer->CharacterCount() > 0;
        for (size_t lineIndex = 0; lineIndex < layer->lines.size(); ++lineIndex) {
            const TextLayerLine& line = layer->lines[lineIndex];
            for (size_t i = 0; i < line.text.size(); ++i) {
                FlatChar flat;
                flat.ch = line.text[i];
                flat.rectPt = i < line.charRects.size() ? line.charRects[i] : D2D1::RectF();
                flat.slot = slot;
                flat.lineIndex = static_cast<int>(lineIndex);
                flatChars_.push_back(flat);
            }
        }
    }
    if (!hasText) {
        error = lastError.empty() ? L"本页没有可选择的文字" : lastError;
        flatChars_.clear();
        return false;
    }
    return true;
}

bool FixedPageReader::HasSelection() const {
    // 只按下未拖动时区间长度为 0，视为没有选择，避免误复制单个字符。
    return !flatChars_.empty() && selectionStart_ >= 0 && selectionEnd_ >= 0 &&
           selectionStart_ != selectionEnd_;
}

void FixedPageReader::InvalidateSelection() {
    selectionStart_ = -1;
    selectionEnd_ = -1;
    flatChars_.clear();
}

void FixedPageReader::BeginSelection(float x, float y) {
    const int index = HitTestChar(x, y);
    selectionStart_ = index;
    selectionEnd_ = index;
}

void FixedPageReader::UpdateSelection(float x, float y) {
    if (selectionStart_ < 0) {
        return;
    }
    const int index = HitTestChar(x, y);
    if (index >= 0) {
        selectionEnd_ = index;
    }
}

bool FixedPageReader::HasTextAt(float x, float y, float toleranceDip) const {
    if (flatChars_.empty()) {
        return false;
    }
    struct PageMapping {
        bool valid = false;
        D2D1_RECT_F rect{};
        float widthPt = 0.0f;
        float heightPt = 0.0f;
    };
    PageMapping mappings[kMaxVisiblePages];
    for (int slot = 0; slot < kMaxVisiblePages; ++slot) {
        mappings[slot].valid =
            PageViewRect(slot, mappings[slot].rect, mappings[slot].widthPt, mappings[slot].heightPt);
    }
    const float tolerance = std::max(toleranceDip, 0.0f);
    for (const FlatChar& flat : flatChars_) {
        if (flat.ch == L' ' || flat.ch == L'\t') {
            continue;
        }
        const PageMapping& mapping = mappings[flat.slot];
        if (!mapping.valid) {
            continue;
        }
        const D2D1_RECT_F display =
            ToDisplayRect(flat.rectPt, mapping.rect, mapping.widthPt, mapping.heightPt);
        if (x >= display.left - tolerance && x <= display.right + tolerance &&
            y >= display.top - tolerance && y <= display.bottom + tolerance) {
            return true;
        }
    }
    return false;
}

int FixedPageReader::HitTestChar(float x, float y) const {
    if (flatChars_.empty()) {
        return -1;
    }
    struct PageMapping {
        bool valid = false;
        D2D1_RECT_F rect{};
        float widthPt = 0.0f;
        float heightPt = 0.0f;
    };
    PageMapping mappings[kMaxVisiblePages];
    for (int slot = 0; slot < kMaxVisiblePages; ++slot) {
        mappings[slot].valid =
            PageViewRect(slot, mappings[slot].rect, mappings[slot].widthPt, mappings[slot].heightPt);
    }

    int bestIndex = -1;
    float bestDistance = FLT_MAX;
    for (size_t index = 0; index < flatChars_.size(); ++index) {
        const FlatChar& flat = flatChars_[index];
        const PageMapping& mapping = mappings[flat.slot];
        if (!mapping.valid) {
            continue;
        }
        const D2D1_RECT_F display =
            ToDisplayRect(flat.rectPt, mapping.rect, mapping.widthPt, mapping.heightPt);
        const float dx = x < display.left ? display.left - x
                                          : (x > display.right ? x - display.right : 0.0f);
        const float dy = y < display.top ? display.top - y
                                         : (y > display.bottom ? y - display.bottom : 0.0f);
        const float distance = dx * dx + dy * dy;
        if (distance < bestDistance) {
            bestDistance = distance;
            bestIndex = static_cast<int>(index);
        }
    }
    return bestIndex;
}

void FixedPageReader::DrawSelection(ui::RenderContext& renderer) const {
    if (!HasSelection()) {
        return;
    }
    struct PageMapping {
        bool valid = false;
        D2D1_RECT_F rect{};
        float widthPt = 0.0f;
        float heightPt = 0.0f;
    };
    PageMapping mappings[kMaxVisiblePages];
    for (int slot = 0; slot < kMaxVisiblePages; ++slot) {
        mappings[slot].valid =
            PageViewRect(slot, mappings[slot].rect, mappings[slot].widthPt, mappings[slot].heightPt);
    }

    const int begin = std::min(selectionStart_, selectionEnd_);
    const int end = std::max(selectionStart_, selectionEnd_);
    int index = begin;
    while (index <= end && index < static_cast<int>(flatChars_.size())) {
        const FlatChar& first = flatChars_[static_cast<size_t>(index)];
        const PageMapping& mapping = mappings[first.slot];
        if (!mapping.valid) {
            ++index;
            continue;
        }
        // 同一页同一行的连续字符合并为一个填充矩形，避免逐字填充出现接缝。
        D2D1_RECT_F run =
            ToDisplayRect(first.rectPt, mapping.rect, mapping.widthPt, mapping.heightPt);
        int last = index;
        while (last + 1 <= end && last + 1 < static_cast<int>(flatChars_.size())) {
            const FlatChar& next = flatChars_[static_cast<size_t>(last + 1)];
            if (next.slot != first.slot || next.lineIndex != first.lineIndex) {
                break;
            }
            // 拼音与汉字的字高不同，词与词之间也可能有明显空白。
            // 垂直范围或水平间距超出容差时拆成独立矩形，
            // 避免合并出一条覆盖两行高度的粗矩形。
            const FlatChar& previous = flatChars_[static_cast<size_t>(last)];
            constexpr float kVerticalTolerancePt = 1.2f;
            // 汉字之间常有 3–5 pt 的间隔，按前一个字宽的比例放宽，
            // 既能把同一行的字连成连续矩形，又不会跨过明显空白。
            const float previousWidth = previous.rectPt.right - previous.rectPt.left;
            const float horizontalGapLimit = std::max(1.5f, previousWidth * 0.6f);
            if (std::fabs(next.rectPt.top - previous.rectPt.top) > kVerticalTolerancePt ||
                std::fabs(next.rectPt.bottom - previous.rectPt.bottom) > kVerticalTolerancePt ||
                next.rectPt.left - previous.rectPt.right > horizontalGapLimit) {
                break;
            }
            const D2D1_RECT_F nextRect =
                ToDisplayRect(next.rectPt, mapping.rect, mapping.widthPt, mapping.heightPt);
            run.left = std::min(run.left, nextRect.left);
            run.right = std::max(run.right, nextRect.right);
            run.top = std::min(run.top, nextRect.top);
            run.bottom = std::max(run.bottom, nextRect.bottom);
            ++last;
        }
        if (run.right > run.left && run.bottom > run.top) {
            renderer.FillRect(run, kSelectionColor);
        }
        index = last + 1;
    }
}

std::wstring FixedPageReader::SelectedText() const {
    if (!HasSelection()) {
        return std::wstring();
    }
    const int begin = std::min(selectionStart_, selectionEnd_);
    const int end = std::min(std::max(selectionStart_, selectionEnd_),
                             static_cast<int>(flatChars_.size()) - 1);
    std::wstring text;
    int lastSlot = -1;
    int lastLine = -1;
    for (int index = begin; index <= end; ++index) {
        const FlatChar& flat = flatChars_[static_cast<size_t>(index)];
        if (lastSlot >= 0 && (flat.slot != lastSlot || flat.lineIndex != lastLine)) {
            text.push_back(L'\n');
        }
        if (flat.ch != 0) {
            text.push_back(flat.ch);
        }
        lastSlot = flat.slot;
        lastLine = flat.lineIndex;
    }
    return text;
}

}  // namespace reader
