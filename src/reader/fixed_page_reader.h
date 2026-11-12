#pragma once

#include <windows.h>
#include <d2d1.h>
#include <wrl/client.h>

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "pdf/pdf_document.h"
#include "reader/page_text.h"
#include "ui/render_context.h"

namespace reader {

// 固定页阅读器：按原版页面位图显示 PDF。
// 只负责文档打开、页码、视图布局与缩放，绘制经由界面渲染层的 RenderContext；
// 不依赖 Application，也不参与文本流的排版、检索与标注。
class FixedPageReader {
public:
    // 页面视图：单页逐页显示，或左右双页并排显示。
    enum class PageLayout {
        Single,
        Spread,
    };

    // 打开 PDF 并定位到指定页；pageIndex 超出范围时定位到首页。
    // 失败时返回 false 并给出原因，此时保持关闭状态。
    bool Open(const std::filesystem::path& path, int pageIndex, std::wstring& error);
    void Close();
    bool IsOpen() const { return document_.IsOpen(); }

    int PageCount() const { return document_.PageCount(); }
    // 当前视图中的起始页序号（0 起）。双页视图中为左侧页。
    int CurrentPage() const { return currentPage_; }
    // 当前视图显示的最大页数：单页为 1，双页为 2。
    int VisiblePageCount() const;
    // 定位到指定页，超出范围时取最近的边界页。双页视图会定位到包含该页的一组。
    void SetCurrentPage(int pageIndex);

    // 翻页；单页视图每次一页，双页视图每次一组。
    // 返回 false 表示已处于首组或末组，没有发生翻页。
    bool NextPage();
    bool PreviousPage();

    // 视图布局。切换布局时保持当前阅读页仍在显示范围内。
    PageLayout Layout() const { return layout_; }
    void SetLayout(PageLayout layout);

    // 缩放比例：1.0 表示当前视图整体适应可用区域，取值限制在 kMinZoom 与 kMaxZoom 之间。
    float Zoom() const { return zoom_; }
    void SetZoom(float zoom);
    void ZoomIn();
    void ZoomOut();
    void ResetZoom();
    // 放大后可平移页面；单位为 DIP，正值表示内容向右、向下移动。
    // 平移范围自动限制在页面边缘不进入视图内部。
    // 返回是否实际发生了平移；已到达边界时返回 false。
    bool PanBy(float dxDip, float dyDip);
    bool IsZoomed() const { return zoom_ > 1.001f; }

    // 当前视图全部页面在可用区域内按原比例居中后的外接矩形，单位为 DIP。
    D2D1_RECT_F PageRect(const D2D1_RECT_F& viewport) const;

    // 绘制当前视图。渲染目标重建后需要调用 InvalidateBitmap 丢弃设备相关资源。
    void Draw(ui::RenderContext& renderer, const D2D1_RECT_F& viewport);
    void InvalidateBitmap();

    // 为当前视图准备文字层：优先使用 PDF 内嵌文本层，没有文本层时用系统 OCR 识别。
    // 返回 false 表示当前视图没有可选择的文字，error 给出原因。
    bool EnsureTextLayer(std::wstring& error);
    bool HasSelection() const;
    // 以 DIP 坐标设置选择范围，指针就近吸附到字符；可跨行、跨双页。
    void BeginSelection(float x, float y);
    void UpdateSelection(float x, float y);
    // 当前选择对应的文本，行与行之间以换行分隔；没有选择时为空。
    std::wstring SelectedText() const;
    // 指针周围 toleranceDip 范围内是否有文字，用于区分拖动页面与选择文字。
    bool HasTextAt(float x, float y, float toleranceDip) const;
    // 当前视图的文字层是否已准备好。
    bool HasTextLayer() const { return !flatChars_.empty(); }

    static constexpr float kMinZoom = 0.25f;
    static constexpr float kMaxZoom = 4.0f;

private:
    // 双页视图最多同时显示两页。
    static constexpr int kMaxVisiblePages = 2;
    // 文字层缓存页数上限，避免整本 OCR 结果常驻内存。
    static constexpr size_t kTextLayerCacheLimit = 8;

    // 选择使用的扁平字符单元，按页、行、字符的阅读顺序排列。
    struct FlatChar {
        wchar_t ch = L' ';
        D2D1_RECT_F rectPt{};  // 页面坐标，原点左下，单位 point
        int slot = 0;
        int lineIndex = 0;
    };

    // 按指定像素比例渲染某页并创建绘制用位图；页码与比例未变化时复用已有位图。
    bool EnsureBitmap(ui::RenderContext& renderer, int slot, int pageIndex, float renderScale);
    // 页面宽高，单位 point；页面信息缺失时按 A4 尺寸处理。
    void PageSize(int pageIndex, float& widthPt, float& heightPt) const;
    // 计算当前视图各页的显示矩形（含平移），返回实际页数。
    int ComputePageRects(const D2D1_RECT_F& viewport,
                         D2D1_RECT_F rects[kMaxVisiblePages]) const;
    // 只按缩放居中计算显示矩形，不含平移，用于限制平移范围。
    int ComputeCenteredPageRects(const D2D1_RECT_F& viewport,
                                 D2D1_RECT_F rects[kMaxVisiblePages]) const;
    // 把平移量限制在页面边缘不进入视图内部的范围内。
    void ClampPan();

    // 取某页文字层，必要时先提取内嵌文本或调用 OCR；失败时返回空指针。
    const PageTextLayer* TextLayerForPage(int pageIndex, std::wstring& error);
    // 某页在最近一次绘制中的显示矩形与页面尺寸。
    bool PageViewRect(int slot, D2D1_RECT_F& rect, float& widthPt, float& heightPt) const;
    // 指针位置吸附到最近的字符，返回扁平字符下标；没有文字层时返回 -1。
    int HitTestChar(float x, float y) const;
    void DrawSelection(ui::RenderContext& renderer) const;
    void InvalidateSelection();

    pdf::Document document_;
    int currentPage_ = 0;
    PageLayout layout_ = PageLayout::Single;
    float zoom_ = 1.0f;
    // 放大后的平移量，单位为 DIP；未放大时保持为 0。
    float panX_ = 0.0f;
    float panY_ = 0.0f;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmaps_[kMaxVisiblePages];
    int bitmapPages_[kMaxVisiblePages] = {-1, -1};
    float bitmapScales_[kMaxVisiblePages] = {0.0f, 0.0f};

    std::map<int, PageTextLayer> textLayers_;
    std::vector<int> textLayerOrder_;
    std::vector<FlatChar> flatChars_;
    int selectionStart_ = -1;
    int selectionEnd_ = -1;
    // 最近一次绘制的可用区域，供指针命中测试换算坐标。
    D2D1_RECT_F lastViewport_{};
};

}  // namespace reader
