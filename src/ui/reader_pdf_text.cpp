#include <algorithm>
#include <cmath>
#include <string>

#include "core/text_util.h"
#include "ui/application.h"
#include "ui/ui_internal.h"

namespace ui {
using namespace detail;

D2D1_RECT_F Application::PdfViewportRect() const {
    // 与阅读界面绘制固定页页面时使用的区域保持一致。
    const float right = widthDip_ - (panel_ != PanelMode::None ? PanelWidth() : 0.0f);
    return D2D1::RectF(0.0f, kReaderToolbarHeight, std::max(1.0f, right),
                       std::max(kReaderToolbarHeight + 1.0f,
                                heightDip_ - kReaderFooterHeight));
}

void Application::ZoomPdfPage(bool zoomIn) {
    if (!fixedPages_.IsOpen()) {
        return;
    }
    const float before = fixedPages_.Zoom();
    if (zoomIn) {
        fixedPages_.ZoomIn();
    } else {
        fixedPages_.ZoomOut();
    }
    const float after = fixedPages_.Zoom();
    if (std::fabs(after - before) < 0.001f) {
        ShowStatus(zoomIn ? L"已达到最大缩放比例" : L"已达到最小缩放比例");
        return;
    }
    fixedPages_.InvalidateBitmap();
    // 缩放比例记录在当前书籍的数据中，只对本书生效。
    // 连续缩放时合并写入，停止缩放 kPdfZoomSaveDelayMs 后落盘一次。
    if (data::BookRecord* record = library_.Find(activeBookId_)) {
        record->pdfZoom = after;
        SetTimer(hwnd_, kPdfZoomTimerId, kPdfZoomSaveDelayMs, nullptr);
    }
    const int percent = static_cast<int>(std::lround(after * 100.0f));
    ShowStatus(L"页面缩放 " + std::to_wstring(percent) + L"%");
    InvalidateRect(hwnd_, nullptr, FALSE);
}

bool Application::BeginPdfSelection(float x, float y) {
    if (!fixedPages_.IsOpen()) {
        return false;
    }
    if (!PointInside(fixedPages_.PageRect(PdfViewportRect()), x, y)) {
        return false;
    }
    // 没有文本层时需要先做 OCR，耗时较长，给出等待光标。
    SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    std::wstring error;
    const bool ready = fixedPages_.EnsureTextLayer(error);
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    InvalidateRect(hwnd_, nullptr, FALSE);
    if (!ready) {
        ShowStatus(error.empty() ? L"本页没有可选择的文字" : error);
        return true;
    }
    pdfSelecting_ = true;
    SetCapture(hwnd_);
    fixedPages_.BeginSelection(x, y);
    InvalidateRect(hwnd_, nullptr, FALSE);
    return true;
}

void Application::UpdatePdfSelection(float x, float y) {
    if (!pdfSelecting_) {
        return;
    }
    fixedPages_.UpdateSelection(x, y);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Application::EndPdfSelection() {
    if (!pdfSelecting_) {
        return;
    }
    pdfSelecting_ = false;
    if (GetCapture() == hwnd_) {
        ReleaseCapture();
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

bool Application::CopyPdfSelection() {
    if (!fixedPages_.IsOpen() || !fixedPages_.HasSelection()) {
        return false;
    }
    const std::wstring text = fixedPages_.SelectedText();
    if (text.empty()) {
        return false;
    }
    if (!WriteClipboardText(text)) {
        ShowStatus(L"复制失败，剪贴板不可用");
        return true;
    }
    ShowStatus(L"已复制 " + std::to_wstring(text.size()) + L" 个字符");
    return true;
}

bool Application::BeginPdfPan(float x, float y) {
    if (!fixedPages_.IsOpen() || !fixedPages_.IsZoomed()) {
        return false;
    }
    if (!PointInside(fixedPages_.PageRect(PdfViewportRect()), x, y)) {
        return false;
    }
    pdfPanning_ = true;
    pdfPanLastX_ = x;
    pdfPanLastY_ = y;
    SetCapture(hwnd_);
    SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
    return true;
}

void Application::UpdatePdfPan(float x, float y) {
    if (!pdfPanning_) {
        return;
    }
    fixedPages_.PanBy(x - pdfPanLastX_, y - pdfPanLastY_);
    pdfPanLastX_ = x;
    pdfPanLastY_ = y;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Application::EndPdfPan() {
    if (!pdfPanning_) {
        return;
    }
    pdfPanning_ = false;
    if (GetCapture() == hwnd_) {
        ReleaseCapture();
    }
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Application::OnMiddleButtonDown(int xPixels, int yPixels) {
    if (view_ != ViewMode::Reader) {
        return;
    }
    const float scale = renderer_.DpiScale();
    BeginPdfPan(static_cast<float>(xPixels) / scale, static_cast<float>(yPixels) / scale);
}

}  // namespace ui
