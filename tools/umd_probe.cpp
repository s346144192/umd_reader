#include <windows.h>

#include <cstdio>
#include <string>

#include "umd/umd_parser.h"

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

std::wstring BuildPreview(const std::wstring& text, size_t limit) {
    std::wstring preview;
    preview.reserve(limit + 16);
    for (wchar_t ch : text) {
        if (preview.size() >= limit) {
            preview += L"…";
            break;
        }
        if (ch == L'\r') {
            continue;
        }
        preview.push_back(ch == L'\n' ? L' ' : ch);
    }
    return preview;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) {
        Print(L"用法：umd_probe <umd 文件路径>");
        return 2;
    }

    umd::Book book;
    std::wstring error;
    if (!umd::ParseFile(std::filesystem::path(argv[1]), book, error)) {
        Print(L"解析失败：" + error);
        return 1;
    }

    Print(L"文件：" + std::wstring(argv[1]));
    Print(L"标题：" + book.title);
    Print(L"作者：" + book.author);
    Print(L"出版日期：" + book.year + L"-" + book.month + L"-" + book.day);
    Print(L"分类：" + book.genre);
    Print(L"出版商：" + book.publisher);
    Print(L"零售商：" + book.retailer);
    Print(L"格式类型：" + std::to_wstring(book.formatType));
    Print(L"正文字符数（UTF-16 码元）：" + std::to_wstring(book.ContentCharCount()) +
          L"，声明字节数：" + std::to_wstring(book.declaredContentBytes));

    Print(L"章节数：" + std::to_wstring(book.chapters.size()));
    const size_t chapterLimit = book.chapters.size() < 50 ? book.chapters.size() : 50;
    for (size_t i = 0; i < chapterLimit; ++i) {
        const umd::Chapter& chapter = book.chapters[i];
        Print(L"  [" + std::to_wstring(i) + L"] 偏移 " + std::to_wstring(chapter.charOffset) +
              L"，长度 " + std::to_wstring(chapter.charLength) + L"，标题 " + chapter.title);
    }
    if (book.chapters.size() > chapterLimit) {
        Print(L"  …其余 " + std::to_wstring(book.chapters.size() - chapterLimit) + L" 个章节未显示");
    }

    Print(L"封面：" + (book.coverBytes.empty()
                           ? std::wstring(L"无")
                           : std::to_wstring(book.coverBytes.size()) + L" 字节（" +
                                 std::wstring(book.coverMimeType.begin(), book.coverMimeType.end()) + L"）"));
    Print(L"插图数量：" + std::to_wstring(book.images.size()));
    Print(L"正文预览：" + BuildPreview(book.content, 200));

    if (!book.warnings.empty()) {
        Print(L"解析提示：");
        for (const std::wstring& warning : book.warnings) {
            Print(L"  - " + warning);
        }
    }
    return 0;
}
