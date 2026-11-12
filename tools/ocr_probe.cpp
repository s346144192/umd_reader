#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "pdf/pdf_document.h"
#include "reader/page_text.h"

namespace {

void Print(const std::wstring& text) {
    const std::wstring line = text + L"\r\n";
    const HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (handle != INVALID_HANDLE_VALUE && GetConsoleMode(handle, &mode)) {
        DWORD written = 0;
        WriteConsoleW(handle, line.c_str(), static_cast<DWORD>(line.size()), &written, nullptr);
        return;
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return;
    }
    std::string utf8(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()), utf8.data(), size,
                        nullptr, nullptr);
    std::fwrite(utf8.data(), 1, utf8.size(), stdout);
    std::fflush(stdout);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        Print(L"用法：ocr_probe <pdf 文件路径> <页序号（0 起）> [--embedded] [--rects]");
        Print(L"  默认用系统 OCR 识别渲染后的页面；--embedded 直接读取 PDF 内嵌文本层");
        Print(L"  --rects 额外输出前若干字符的外框，用于核对选中高亮是否对齐");
        return 2;
    }
    const std::filesystem::path path(argv[1]);
    const int pageIndex = _wtoi(argv[2]);
    bool embeddedOnly = false;
    bool printRects = false;
    for (int i = 3; i < argc; ++i) {
        const std::wstring argument = argv[i];
        if (argument == L"--embedded") {
            embeddedOnly = true;
        } else if (argument == L"--rects") {
            printRects = true;
        }
    }

    pdf::Document document;
    std::wstring error;
    if (!document.Open(path, error)) {
        Print(L"打开失败：" + error);
        return 1;
    }
    if (pageIndex < 0 || pageIndex >= document.PageCount()) {
        Print(L"页序号超出范围，共 " + std::to_wstring(document.PageCount()) + L" 页");
        return 1;
    }

    float widthPt = 595.0f;
    float heightPt = 842.0f;
    const auto& pages = document.Pages();
    if (pages[static_cast<size_t>(pageIndex)].widthPt > 0.0f) {
        widthPt = pages[static_cast<size_t>(pageIndex)].widthPt;
    }
    if (pages[static_cast<size_t>(pageIndex)].heightPt > 0.0f) {
        heightPt = pages[static_cast<size_t>(pageIndex)].heightPt;
    }
    Print(L"页面尺寸：" + std::to_wstring(static_cast<int>(widthPt)) + L" × " +
          std::to_wstring(static_cast<int>(heightPt)) + L" pt");

    reader::PageTextLayer layer;
    if (embeddedOnly) {
        std::vector<pdf::TextLine> lines;
        if (!document.ReadPageLines(pageIndex, lines, error)) {
            Print(L"读取内嵌文本失败：" + error);
            return 1;
        }
        layer.fromOcr = false;
        size_t boxedCount = 0;
        for (const pdf::TextLine& line : lines) {
            reader::TextLayerLine item;
            item.text = line.text;
            for (const pdf::CharBox& box : line.boxes) {
                item.charRects.push_back(D2D1::RectF(box.left, box.top, box.right, box.bottom));
            }
            if (line.boxes.size() == line.text.size()) {
                ++boxedCount;
            }
            layer.lines.push_back(std::move(item));
        }
        Print(L"内嵌文本行：" + std::to_wstring(layer.lines.size()) + L"，外框完整的行 " +
              std::to_wstring(boxedCount));
    } else {
        pdf::PageBitmapPtr bitmap;
        const DWORD start = GetTickCount();
        if (!document.RenderPage(pageIndex, reader::kOcrRenderScale, bitmap, error) || !bitmap) {
            Print(L"渲染失败：" + error);
            return 1;
        }
        if (!reader::RecognizePageText(*bitmap, widthPt, heightPt, layer, error)) {
            Print(L"OCR 失败：" + error);
            return 1;
        }
        Print(L"OCR 耗时：" + std::to_wstring(GetTickCount() - start) + L" 毫秒");
    }

    const pdf::PageInfo& pageInfo = document.Pages()[static_cast<size_t>(pageIndex)];
    Print(L"字符数：" + std::to_wstring(layer.CharacterCount()) + L"，行数：" +
          std::to_wstring(layer.lines.size()) + L"，整页图像：" +
          (pageInfo.hasFullPageImage ? L"是" : L"否") + L"，文字层可疑：" +
          (pageInfo.textLayerSuspicious ? L"是" : L"否"));
    for (size_t i = 0; i < layer.lines.size() && i < 8; ++i) {
        const reader::TextLayerLine& line = layer.lines[i];
        Print(L"  [" + std::to_wstring(i) + L"] " + line.text.substr(0, 60));
    }
    if (printRects) {
        size_t printed = 0;
        for (size_t i = 0; i < layer.lines.size() && printed < 24; ++i) {
            const reader::TextLayerLine& line = layer.lines[i];
            for (size_t j = 0; j < line.text.size() && printed < 24; ++j) {
                if (j >= line.charRects.size()) {
                    break;
                }
                const D2D1_RECT_F& rect = line.charRects[j];
                wchar_t buffer[160] = {};
                swprintf_s(buffer, L"  rect '%wc' L%.1f R%.1f T%.1f B%.1f", line.text[j],
                           rect.left, rect.right, rect.top, rect.bottom);
                Print(buffer);
                ++printed;
            }
        }
    }
    return 0;
}
