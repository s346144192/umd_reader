#pragma once

#include <d2d1.h>

#include <string>
#include <vector>

#include "pdf/pdf_document.h"

namespace reader {

// OCR 渲染比例：每 point 对应的像素数。文字过小时识别率下降，过大则耗时与内存上升。
constexpr float kOcrRenderScale = 2.0f;
// 单页有效字符低于该值视为没有文字层，改用 OCR。
constexpr size_t kMinimumEmbeddedCharacters = 8;

// 页面文字层的一行：文本与其逐字外框，坐标与 PDF 页面一致，
// 原点在页面左下角，单位 point。裁剪范围用同一套坐标。
struct TextLayerLine {
    std::wstring text;
    std::vector<D2D1_RECT_F> charRects;
};

struct PageTextLayer {
    std::vector<TextLayerLine> lines;
    bool fromOcr = false;

    // 有效文字字符数（不含空白），用于判断文字层是否可用。
    size_t CharacterCount() const;
};

// 用系统内置 OCR 识别页面位图并生成文字层。
// 位图为 BGRA、每行自上而下；pageWidthPt / pageHeightPt 为该页的 PDF 尺寸。
// 系统未安装可用 OCR 语言包或识别失败时返回 false，并给出原因。
bool RecognizePageText(const pdf::PageBitmap& bitmap, float pageWidthPt, float pageHeightPt,
                       PageTextLayer& layer, std::wstring& error);

}  // namespace reader
