#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include "pdf/pdf_document.h"

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

std::wstring Fixed(double value, int digits) {
    std::wstringstream stream;
    stream << std::fixed << std::setprecision(digits) << value;
    return stream.str();
}

std::wstring Milliseconds(double value) {
    return Fixed(value, 2) + L" 毫秒";
}

std::wstring Megabytes(uint64_t bytes) {
    return Fixed(static_cast<double>(bytes) / (1024.0 * 1024.0), 2) + L" MB";
}

double ElapsedMilliseconds(const std::chrono::steady_clock::time_point& start,
                           const std::chrono::steady_clock::time_point& end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

uint64_t WorkingSetBytes() {
    PROCESS_MEMORY_COUNTERS counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) == 0) {
        return 0;
    }
    return static_cast<uint64_t>(counters.WorkingSetSize);
}

// 单行文本预览：控制字符替换为空格，超长截断。
std::wstring LinePreview(const std::wstring& text, size_t limit) {
    std::wstring preview;
    preview.reserve(limit + 1);
    for (wchar_t ch : text) {
        if (preview.size() >= limit) {
            preview += L"…";
            break;
        }
        preview.push_back(ch < 0x20 ? L' ' : ch);
    }
    return preview;
}

bool ParseArguments(int argc, wchar_t** argv, int& pageLimit, int& renderLimit, int& lineLimit,
                    float& scale, std::filesystem::path& file) {
    if (argc < 2) {
        return false;
    }
    file = std::filesystem::path(argv[1]);
    for (int i = 2; i < argc; i += 2) {
        if (i + 1 >= argc) {
            return false;
        }
        const std::wstring key = argv[i];
        if (key == L"--pages") {
            pageLimit = _wtoi(argv[i + 1]);
        } else if (key == L"--render") {
            renderLimit = _wtoi(argv[i + 1]);
        } else if (key == L"--lines") {
            lineLimit = _wtoi(argv[i + 1]);
        } else if (key == L"--scale") {
            scale = static_cast<float>(_wtof(argv[i + 1]));
        } else {
            return false;
        }
    }
    return pageLimit > 0 && renderLimit >= 0 && lineLimit >= 0 && scale > 0.0f;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    int pageLimit = 50;
    int renderLimit = 5;
    int lineLimit = 2;
    float scale = 2.0f;
    std::filesystem::path file;

    if (!ParseArguments(argc, argv, pageLimit, renderLimit, lineLimit, scale, file)) {
        Print(L"用法：pdf_probe <pdf 文件路径> [--pages N] [--render N] [--lines N] [--scale S]");
        Print(L"  --pages  列出前 N 页的文本层数据，默认 50");
        Print(L"  --render 渲染前 N 页并计时，默认 5，0 表示跳过");
        Print(L"  --lines  每页打印的文本行数，默认 2");
        Print(L"  --scale  渲染缩放：每 point 的像素数，默认 2.0");
        return 2;
    }

    pdf::Document document;
    std::wstring error;

    const auto openStart = std::chrono::steady_clock::now();
    if (!document.Open(file, error)) {
        Print(L"打开失败：" + error);
        return 1;
    }
    const auto openEnd = std::chrono::steady_clock::now();

    uint64_t fileSize = 0;
    std::error_code sizeError;
    const uint64_t reportedSize = std::filesystem::file_size(file, sizeError);
    if (!sizeError) {
        fileSize = reportedSize;
    }

    Print(L"文件：" + file.wstring());
    Print(L"文件大小：" + Megabytes(fileSize));
    Print(L"标题：" + (document.Title().empty() ? L"（无）" : document.Title()));
    Print(L"作者：" + (document.Author().empty() ? L"（无）" : document.Author()));
    Print(L"页数：" + std::to_wstring(document.PageCount()));
    Print(L"打开耗时：" + Milliseconds(ElapsedMilliseconds(openStart, openEnd)));

    const auto analyzeStart = std::chrono::steady_clock::now();
    if (!document.AnalyzeAllPages(error)) {
        Print(L"文本层统计失败：" + error);
        return 1;
    }
    const auto analyzeEnd = std::chrono::steady_clock::now();

    const std::vector<pdf::PageInfo>& pages = document.Pages();
    uint64_t totalChars = 0;
    uint64_t totalPrivateUse = 0;
    uint32_t pagesWithText = 0;      // 有效字符 >= 1
    uint32_t pagesWithSomeText = 0;  // 有效字符 >= 20
    uint32_t pagesWithRichText = 0;  // 有效字符 >= 50
    for (const pdf::PageInfo& info : pages) {
        totalChars += info.textCharCount;
        totalPrivateUse += info.privateUseCharCount;
        if (info.textCharCount >= 1) {
            ++pagesWithText;
        }
        if (info.textCharCount >= 20) {
            ++pagesWithSomeText;
        }
        if (info.textCharCount >= 50) {
            ++pagesWithRichText;
        }
    }

    Print(L"文本层统计耗时：" + Milliseconds(ElapsedMilliseconds(analyzeStart, analyzeEnd)));
    Print(L"有文本层的页数：≥1 字符 " + std::to_wstring(pagesWithText) + L" 页，≥20 字符 " +
          std::to_wstring(pagesWithSomeText) + L" 页，≥50 字符 " +
          std::to_wstring(pagesWithRichText) + L" 页，共 " +
          std::to_wstring(pages.size()) + L" 页");
    Print(L"有效字符总数：" + std::to_wstring(totalChars) + L"，私用区字符总数：" +
          std::to_wstring(totalPrivateUse));
    if (!pages.empty()) {
        Print(L"平均每页有效字符：" + Fixed(static_cast<double>(totalChars) / pages.size(), 1));
    }

    Print(L"");
    Print(L"逐页文本层：");

    const int listedPages = std::min<int>(document.PageCount(), pageLimit);
    uint64_t listedChars = 0;
    uint64_t listedLines = 0;
    uint64_t listedPrivateUse = 0;
    for (int i = 0; i < listedPages; ++i) {
        const pdf::PageInfo& info = pages[static_cast<size_t>(i)];
        std::vector<pdf::TextLine> lines;
        if (!document.ReadPageLines(i, lines, error)) {
            Print(L"  [" + std::to_wstring(i) + L"] 读取失败：" + error);
            continue;
        }
        listedChars += info.textCharCount;
        listedLines += lines.size();
        listedPrivateUse += info.privateUseCharCount;
        const double averageLineChars =
            lines.empty() ? 0.0 : static_cast<double>(info.textCharCount) / lines.size();
        Print(L"  [" + std::to_wstring(i) + L"] 尺寸 " + Fixed(info.widthPt, 1) + L" × " +
              Fixed(info.heightPt, 1) + L" pt，字符 " + std::to_wstring(info.textCharCount) +
              L"，私用区 " + std::to_wstring(info.privateUseCharCount) + L"，行数 " +
              std::to_wstring(lines.size()) + L"，平均行字符 " + Fixed(averageLineChars, 1));
        for (int k = 0; k < lineLimit && k < static_cast<int>(lines.size()); ++k) {
            Print(L"      " + LinePreview(lines[static_cast<size_t>(k)].text, 60));
        }
    }
    if (listedPages > 0) {
        Print(L"  上述 " + std::to_wstring(listedPages) + L" 页合计：字符 " +
              std::to_wstring(listedChars) + L"，行数 " + std::to_wstring(listedLines) +
              L"，私用区 " + std::to_wstring(listedPrivateUse));
    }

    Print(L"");
    Print(L"大纲条目：" + std::to_wstring(document.Outline().size()));
    for (const pdf::OutlineEntry& entry : document.Outline()) {
        std::wstring indent(static_cast<size_t>(entry.level) * 2, L' ');
        std::wstring target = entry.pageIndex >= 0
                                  ? L"第 " + std::to_wstring(entry.pageIndex + 1) + L" 页"
                                  : L"无可定位页";
        if (entry.hasPosition) {
            target += L"，位置 " + Fixed(entry.positionY, 1) + L" pt";
        }
        Print(indent + (entry.title.empty() ? L"（无标题）" : entry.title) + L" → " + target);
    }

    if (renderLimit > 0) {
        Print(L"");
        Print(L"渲染测试：缩放 " + Fixed(scale, 2) + L" 像素/point");
        const int renderPages = std::min<int>(document.PageCount(), renderLimit);
        const uint64_t workingSetBefore = WorkingSetBytes();
        uint64_t bitmapBytes = 0;
        double totalRenderMilliseconds = 0.0;
        for (int i = 0; i < renderPages; ++i) {
            const auto renderStart = std::chrono::steady_clock::now();
            pdf::PageBitmapPtr bitmap;
            if (!document.RenderPage(i, scale, bitmap, error)) {
                Print(L"  [" + std::to_wstring(i) + L"] 渲染失败：" + error);
                continue;
            }
            const auto renderEnd = std::chrono::steady_clock::now();
            const double elapsed = ElapsedMilliseconds(renderStart, renderEnd);
            totalRenderMilliseconds += elapsed;
            bitmapBytes += bitmap ? bitmap->pixels.size() : 0;
            Print(L"  [" + std::to_wstring(i) + L"] " + std::to_wstring(bitmap->width) + L" × " +
                  std::to_wstring(bitmap->height) + L" 像素，位图 " +
                  Megabytes(bitmap->pixels.size()) + L"，" + Milliseconds(elapsed));
        }
        const uint64_t workingSetAfter = WorkingSetBytes();
        if (renderPages > 0 && totalRenderMilliseconds > 0.0) {
            Print(L"  渲染合计：" + Milliseconds(totalRenderMilliseconds) + L"，平均 " +
                  Milliseconds(totalRenderMilliseconds / renderPages) + L"，位图累计 " +
                  Megabytes(bitmapBytes));
        }
        if (workingSetBefore > 0 && workingSetAfter > 0) {
            const int64_t delta = static_cast<int64_t>(workingSetAfter) -
                                  static_cast<int64_t>(workingSetBefore);
            Print(L"  工作集合：" + Megabytes(workingSetBefore) + L" → " +
                  Megabytes(workingSetAfter) + L"，变化 " +
                  (delta >= 0 ? L"+" : L"-") + Megabytes(static_cast<uint64_t>(delta >= 0 ? delta : -delta)));
        }
    }

    return 0;
}
