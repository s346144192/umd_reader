#include "umd/umd_parser.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "pdf/pdf_document.h"

namespace umd {
namespace {

std::wstring TrimSpaces(const std::wstring& text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && (text[begin] == L' ' || text[begin] == L'\t')) {
        ++begin;
    }
    while (end > begin && (text[end - 1] == L' ' || text[end - 1] == L'\t')) {
        --end;
    }
    return text.substr(begin, end - begin);
}

// 章节名比较用的规范化：去掉空白与书名号、括号、间隔号等修饰字符，
// 只保留文字内容，便于识别「《书名》」这类根节点。
std::wstring NormalizeTitleKey(const std::wstring& text) {
    std::wstring key;
    key.reserve(text.size());
    for (const wchar_t character : text) {
        switch (character) {
        case L' ':
        case L'\t':
        case 0x3000:
        case L'《':
        case L'》':
        case L'〈':
        case L'〉':
        case L'（':
        case L'）':
        case L'(':
        case L')':
        case L'·':
        case L'•':
        case L'-':
        case L'—':
        case L':':
        case L'：':
        case L',':
        case L'，':
            continue;
        default:
            key.push_back(character);
            break;
        }
    }
    return key;
}

// 判断大纲条目是否为书籍名称节点。这类根节点只用于分组，
// 直接跳过，避免每条章节名前面都重复书籍名称。
bool IsBookTitleNode(const std::wstring& titleKey, const std::wstring& bookKey, int level) {
    if (titleKey.size() < 2 || bookKey.size() < 2) {
        return false;
    }
    if (titleKey == bookKey) {
        return true;
    }
    // 顶层条目常写成「《书名》」「书名（第 N 版）」等形式，包含书名即视为书名节点。
    return level <= 1 && titleKey.find(bookKey) != std::wstring::npos;
}

// 用 PDF 大纲生成目录树。章节按先序排列并记录层级，名称只用条目自身文字；
// 书籍名称根节点只用于分组，直接跳过，其子节点整体上移一层。
void BuildPageChapters(const std::vector<pdf::OutlineEntry>& outline, Book& book) {
    const std::wstring bookKey = NormalizeTitleKey(book.title);
    std::vector<Chapter> chapters;
    // 书名根节点的层级；为 0 表示当前不在书名分组内。
    int bookRootLevel = 0;
    for (const pdf::OutlineEntry& entry : outline) {
        const std::wstring title = TrimSpaces(entry.title);
        if (title.empty()) {
            continue;
        }
        const int rawLevel = std::max(entry.level, 1);
        if (IsBookTitleNode(NormalizeTitleKey(title), bookKey, rawLevel)) {
            bookRootLevel = rawLevel;
            continue;
        }
        // 没有目标页的条目通常是阅读器的导航指令（返回、打印等），不进入目录。
        if (entry.pageIndex < 0 ||
            static_cast<size_t>(entry.pageIndex) >= book.pdfPages.size()) {
            continue;
        }
        int level = rawLevel;
        if (bookRootLevel > 0) {
            if (rawLevel > bookRootLevel) {
                level = rawLevel - bookRootLevel;
            } else {
                // 回到书名根节点同级或更浅的位置，说明书名分组已经结束。
                bookRootLevel = 0;
            }
        }
        Chapter chapter;
        chapter.title = title;
        chapter.pageIndex = static_cast<uint32_t>(entry.pageIndex);
        chapter.level = static_cast<uint8_t>(std::clamp(level, 1, 9));
        chapters.push_back(std::move(chapter));
    }
    book.chapters = std::move(chapters);
}

}  // namespace

bool ParsePdfFile(const std::filesystem::path& path, Book& book, std::wstring& error) {
    error.clear();
    book = Book{};
    book.formatType = 3;
    book.sourceCodePage = 1200;

    // 书籍名称使用文件名（去掉扩展名），与书架中其他书籍的命名方式一致。
    book.title = path.stem().wstring();
    if (book.title.empty()) {
        book.title = path.filename().wstring();
    }

    // 按原版页面阅读只需要页面尺寸、页数与大纲，不做文本层提取，
    // 打开与导入的耗时与页数基本无关，大文件也能立即显示。
    pdf::Document document;
    if (!document.Open(path, error)) {
        return false;
    }
    const int pageCount = document.PageCount();
    if (pageCount <= 0) {
        error = L"PDF 文件没有页面：" + path.wstring();
        return false;
    }

    book.author = TrimSpaces(document.Author());
    const std::vector<pdf::PageInfo>& pages = document.Pages();
    book.pdfPages.resize(static_cast<size_t>(pageCount));
    for (int index = 0; index < pageCount; ++index) {
        const pdf::PageInfo& info = pages[static_cast<size_t>(index)];
        PdfPageText& page = book.pdfPages[static_cast<size_t>(index)];
        page.widthPt = info.widthPt > 0.0f ? info.widthPt : 595.0f;
        page.heightPt = info.heightPt > 0.0f ? info.heightPt : 842.0f;
        page.hasText = false;
        page.charOffset = 0;
        page.charLength = 0;
    }

    BuildPageChapters(document.Outline(), book);
    // 没有大纲时提供一个整本章节，目录与章节标题仍然可用。
    if (book.chapters.empty()) {
        Chapter whole;
        whole.title = L"正文";
        whole.pageIndex = 0;
        whole.level = 1;
        book.chapters.push_back(std::move(whole));
    }
    return true;
}

}  // namespace umd
