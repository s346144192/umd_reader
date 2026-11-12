#include "pdf/pdf_document.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <mutex>

#include "fpdf_doc.h"
#include "fpdf_edit.h"
#include "fpdf_text.h"
#include "fpdf_transformpage.h"
#include "fpdfview.h"

namespace pdf {

namespace {

// 渲染位图的单边像素上限，避免异常缩放导致超大内存分配。
constexpr int kMaxRenderPixels = 8192;
// 大纲递归深度上限，防止畸形文件造成的无限递归。
constexpr int kMaxOutlineDepth = 32;
// 同一缩放下缓存最近一页的位图。
constexpr float kScaleEpsilon = 0.0001f;

// PDFium 库为进程级资源，首次使用时初始化并保持到进程结束。
void EnsureLibraryInitialized() {
    static std::once_flag flag;
    std::call_once(flag, []() {
        FPDF_LIBRARY_CONFIG config{};
        config.version = 2;
        config.m_pUserFontPaths = nullptr;
        config.m_pIsolate = nullptr;
        config.m_v8EmbedderSlot = 0;
        FPDF_InitLibraryWithConfig(&config);
    });
}

bool IsLineBreak(unsigned int code) {
    return code == 0x0Au || code == 0x0Du;
}

bool IsWhitespace(unsigned int code) {
    if (code == 0x09u || code == 0x0Au || code == 0x0Bu || code == 0x0Cu || code == 0x0Du ||
        code == 0x20u || code == 0xA0u || code == 0x1680u || code == 0x2028u ||
        code == 0x2029u || code == 0x202Fu || code == 0x205Fu || code == 0x3000u ||
        code == 0xFEFFu) {
        return true;
    }
    return code >= 0x2000u && code <= 0x200Au;
}

bool IsPrivateUse(unsigned int code) {
    if (code >= 0xE000u && code <= 0xF8FFu) {
        return true;
    }
    if (code >= 0xF0000u && code <= 0xFFFFDu) {
        return true;
    }
    return code >= 0x100000u && code <= 0x10FFFDu;
}

// 判断字符是否属于中日韩文字或全角标点。此类字符之间不应补空格。
bool IsCjkLike(unsigned int code) {
    if (code >= 0x2E80u && code <= 0x9FFFu) {
        return true;
    }
    if (code >= 0xF900u && code <= 0xFAFFu) {
        return true;
    }
    if (code >= 0xFE30u && code <= 0xFE4Fu) {
        return true;
    }
    if (code >= 0xFF00u && code <= 0xFFEFu) {
        return true;
    }
    return code >= 0x20000u && code <= 0x3FFFFu;
}

void AppendCodePoint(std::wstring& text, unsigned int code) {
    if (code < 0x10000u) {
        text.push_back(static_cast<wchar_t>(code));
        return;
    }
    const unsigned int offset = code - 0x10000u;
    text.push_back(static_cast<wchar_t>(0xD800u + (offset >> 10)));
    text.push_back(static_cast<wchar_t>(0xDC00u + (offset & 0x3FFu)));
}

void TrimTrailingSpaces(std::wstring& text) {
    while (!text.empty() && text.back() == L' ') {
        text.pop_back();
    }
}

// PDFium 的标题与元数据均为 UTF-16LE，结尾带双字节零。
std::wstring WideFromUtf16(const unsigned short* units, size_t count) {
    std::wstring text;
    text.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        if (units[i] == 0) {
            break;
        }
        text.push_back(static_cast<wchar_t>(units[i]));
    }
    return text;
}

}  // namespace

// 自定义文件读取源。PDFium 打开文档时只记录读取回调，之后按需读取数据块，
// 因此文件不需要整份装入内存；回调与文档句柄在同一互斥量下串行执行。
struct Document::FileSource {
    std::ifstream stream;
    size_t length = 0;
    FPDF_FILEACCESS access{};

    static int ReadBlock(void* param, unsigned long position, unsigned char* buffer,
                         unsigned long size) {
        FileSource* source = static_cast<FileSource*>(param);
        if (source == nullptr || buffer == nullptr || size == 0) {
            return 0;
        }
        if (static_cast<size_t>(position) + static_cast<size_t>(size) > source->length) {
            return 0;
        }
        source->stream.clear();
        source->stream.seekg(static_cast<std::streamoff>(position), std::ios::beg);
        if (!source->stream) {
            return 0;
        }
        source->stream.read(reinterpret_cast<char*>(buffer),
                            static_cast<std::streamsize>(size));
        return source->stream ? 1 : 0;
    }
};

Document::Document() = default;

Document::~Document() {
    std::lock_guard<std::mutex> lock(mutex_);
    CloseInternal();
}

bool Document::Open(const std::filesystem::path& path, std::wstring& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    CloseInternal();

    std::error_code sizeError;
    const uint64_t fileSize = std::filesystem::file_size(path, sizeError);
    if (sizeError || fileSize == 0) {
        error = L"无法读取文件：" + path.wstring();
        return false;
    }
    // PDFium 的自定义读取接口使用 32 位无符号长度，超过 4 GB 的文件无法装载。
    if (fileSize > 0xFFFFFFFFull) {
        error = L"文件超过 4 GB，暂不支持：" + path.wstring();
        return false;
    }

    auto source = std::make_unique<FileSource>();
    source->stream.open(path, std::ios::binary);
    if (!source->stream) {
        error = L"无法读取文件：" + path.wstring();
        return false;
    }
    source->length = static_cast<size_t>(fileSize);
    source->access.m_FileLen = static_cast<unsigned long>(fileSize);
    source->access.m_GetBlock = &FileSource::ReadBlock;
    source->access.m_Param = source.get();

    EnsureLibraryInitialized();

    FPDF_DOCUMENT document = FPDF_LoadCustomDocument(&source->access, nullptr);
    if (!document) {
        error = L"无法打开 PDF：" + DescribeError(FPDF_GetLastError());
        return false;
    }

    const int pageCount = FPDF_GetPageCount(document);
    if (pageCount <= 0) {
        FPDF_CloseDocument(document);
        error = L"PDF 中没有可显示的页面";
        return false;
    }

    fileSource_ = std::move(source);
    document_ = document;
    pageCount_ = pageCount;
    pages_.assign(static_cast<size_t>(pageCount), PageInfo{});
    for (int i = 0; i < pageCount; ++i) {
        FS_SIZEF size{};
        if (FPDF_GetPageSizeByIndexF(document, i, &size) && size.width > 0.0f &&
            size.height > 0.0f) {
            pages_[static_cast<size_t>(i)].widthPt = size.width;
            pages_[static_cast<size_t>(i)].heightPt = size.height;
        } else {
            // 尺寸读取失败时按 A4 兜底，保证后续渲染仍有可用画布。
            pages_[static_cast<size_t>(i)].widthPt = 595.0f;
            pages_[static_cast<size_t>(i)].heightPt = 842.0f;
        }
    }

    LoadMetadata();
    LoadOutline();
    return true;
}

void Document::Close() {
    std::lock_guard<std::mutex> lock(mutex_);
    CloseInternal();
}

void Document::CloseInternal() {
    if (document_) {
        FPDF_CloseDocument(static_cast<FPDF_DOCUMENT>(document_));
        document_ = nullptr;
    }
    fileSource_.reset();
    pageCount_ = 0;
    pages_.clear();
    outline_.clear();
    title_.clear();
    author_.clear();
    cachedBitmap_.reset();
    cachedPage_ = -1;
    cachedScale_ = 0.0f;
}

void Document::ClearCache() {
    std::lock_guard<std::mutex> lock(mutex_);
    cachedBitmap_.reset();
    cachedPage_ = -1;
    cachedScale_ = 0.0f;
}

void Document::LoadMetadata() {
    title_ = MetaText(document_, "Title");
    author_ = MetaText(document_, "Author");
}

std::wstring Document::MetaText(void* document, const char* tag) {
    if (!document) {
        return std::wstring();
    }
    const unsigned long length =
        FPDF_GetMetaText(static_cast<FPDF_DOCUMENT>(document), tag, nullptr, 0);
    if (length <= 2) {
        return std::wstring();
    }
    std::vector<unsigned short> units(length / 2 + 1, 0);
    FPDF_GetMetaText(static_cast<FPDF_DOCUMENT>(document), tag, units.data(),
                     static_cast<unsigned long>(units.size() * 2));
    return WideFromUtf16(units.data(), units.size());
}

void Document::LoadOutline() {
    outline_.clear();
    if (!document_) {
        return;
    }
    std::vector<void*> visited;
    WalkOutline(nullptr, 1, 0, visited);
}

void Document::WalkOutline(void* parent, int level, int depth, std::vector<void*>& visited) {
    FPDF_DOCUMENT document = static_cast<FPDF_DOCUMENT>(document_);
    if (!document || depth > kMaxOutlineDepth) {
        return;
    }
    for (FPDF_BOOKMARK bookmark =
             FPDFBookmark_GetFirstChild(document, static_cast<FPDF_BOOKMARK>(parent));
         bookmark != nullptr;
         bookmark = FPDFBookmark_GetNextSibling(document, bookmark)) {
        // 畸形文件可能出现书签循环引用，遇到已访问条目即停止该层遍历。
        if (std::find(visited.begin(), visited.end(), static_cast<void*>(bookmark)) !=
            visited.end()) {
            return;
        }
        visited.push_back(static_cast<void*>(bookmark));

        OutlineEntry entry;
        entry.level = level;

        const unsigned long titleLength = FPDFBookmark_GetTitle(bookmark, nullptr, 0);
        if (titleLength > 2) {
            std::vector<unsigned short> units(titleLength / 2 + 1, 0);
            FPDFBookmark_GetTitle(bookmark, units.data(),
                                  static_cast<unsigned long>(units.size() * 2));
            entry.title = WideFromUtf16(units.data(), units.size());
        }

        FPDF_DEST dest = FPDFBookmark_GetDest(document, bookmark);
        if (dest) {
            const int pageIndex = FPDFDest_GetDestPageIndex(document, dest);
            if (pageIndex >= 0 && pageIndex < pageCount_) {
                entry.pageIndex = pageIndex;
            }
            FPDF_BOOL hasX = 0;
            FPDF_BOOL hasY = 0;
            FPDF_BOOL hasZoom = 0;
            FS_FLOAT x = 0.0f;
            FS_FLOAT y = 0.0f;
            FS_FLOAT zoom = 0.0f;
            if (FPDFDest_GetLocationInPage(dest, &hasX, &hasY, &hasZoom, &x, &y, &zoom) &&
                hasY) {
                entry.hasPosition = true;
                entry.positionY = y;
            }
        }
        outline_.push_back(std::move(entry));

        WalkOutline(static_cast<void*>(bookmark), level + 1, depth + 1, visited);
    }
}

bool Document::AnalyzePage(int pageIndex, std::wstring& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    ExtractResult result;
    return ExtractPage(pageIndex, result, error);
}

bool Document::AnalyzeAllPages(std::wstring& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!document_) {
        error = L"文档尚未打开";
        return false;
    }
    ExtractResult result;
    for (int i = 0; i < pageCount_; ++i) {
        if (!ExtractPage(i, result, error)) {
            return false;
        }
    }
    return true;
}

bool Document::ReadPageLines(int pageIndex, std::vector<TextLine>& lines, std::wstring& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    ExtractResult result;
    if (!ExtractPage(pageIndex, result, error)) {
        return false;
    }
    lines = std::move(result.lines);
    return true;
}

namespace {

// 递归累计图像对象覆盖的页面面积。扫描页的扫描图可能放在表单对象内部，
// 也可能被拆成多条图像，因此需要逐层展开后累加。
void AccumulateImageArea(FPDF_PAGEOBJECT object, float pageWidthPt, float pageHeightPt,
                         float cropLeft, float cropBottom, double& coveredArea) {
    if (object == nullptr) {
        return;
    }
    const int objectType = FPDFPageObj_GetType(object);
    if (objectType == FPDF_PAGEOBJ_IMAGE) {
        FS_RECTF bounds{};
        if (!FPDFPageObj_GetBounds(object, &bounds.left, &bounds.bottom, &bounds.right,
                                   &bounds.top)) {
            return;
        }
        const float left = std::max(0.0f, bounds.left - cropLeft);
        const float right = std::min(pageWidthPt, bounds.right - cropLeft);
        const float bottom = std::max(0.0f, bounds.bottom - cropBottom);
        const float top = std::min(pageHeightPt, bounds.top - cropBottom);
        if (right > left && top > bottom) {
            coveredArea += static_cast<double>(right - left) * (top - bottom);
        }
        return;
    }
    if (objectType == FPDF_PAGEOBJ_FORM) {
        const int count = FPDFFormObj_CountObjects(object);
        for (int i = 0; i < count; ++i) {
            AccumulateImageArea(FPDFFormObj_GetObject(object, static_cast<unsigned long>(i)),
                                pageWidthPt, pageHeightPt, cropLeft, cropBottom, coveredArea);
        }
    }
}

// 判断字符是否属于中文书籍中很少见、但在字体映射缺失时大量出现的 ASCII 符号。
bool IsSuspiciousAsciiSymbol(wchar_t character) {
    switch (character) {
    case L'!':
    case L'"':
    case L'#':
    case L'$':
    case L'%':
    case L'&':
    case L'\'':
    case L'*':
    case L'+':
    case L'/':
    case L'<':
    case L'=':
    case L'>':
    case L'@':
    case L'[':
    case L'\\':
    case L']':
    case L'^':
    case L'_':
    case L'`':
    case L'{':
    case L'|':
    case L'}':
    case L'~':
        return true;
    default:
        return false;
    }
}

// 判断页面是否由图像覆盖大部分区域，用于识别扫描页。
// 扫描页自带的文字层来自扫描软件，可能缺少拼音、与图像错位或存在乱码。
bool HasFullPageImage(FPDF_PAGE page, float pageWidthPt, float pageHeightPt, float cropLeft,
                      float cropBottom) {
    if (page == nullptr || pageWidthPt <= 0.0f || pageHeightPt <= 0.0f) {
        return false;
    }
    double coveredArea = 0.0;
    const int objectCount = FPDFPage_CountObjects(page);
    for (int i = 0; i < objectCount; ++i) {
        AccumulateImageArea(FPDFPage_GetObject(page, i), pageWidthPt, pageHeightPt, cropLeft,
                            cropBottom, coveredArea);
    }
    const double pageArea = static_cast<double>(pageWidthPt) * pageHeightPt;
    return pageArea > 0.0 && coveredArea >= pageArea * 0.6;
}

}  // namespace

bool Document::ExtractPage(int pageIndex, ExtractResult& result, std::wstring& error) {
    result = ExtractResult{};
    if (!document_) {
        error = L"文档尚未打开";
        return false;
    }
    if (pageIndex < 0 || pageIndex >= pageCount_) {
        error = L"页码超出范围：" + std::to_wstring(pageIndex);
        return false;
    }

    FPDF_DOCUMENT document = static_cast<FPDF_DOCUMENT>(document_);
    FPDF_PAGE page = FPDF_LoadPage(document, pageIndex);
    if (!page) {
        error = L"无法加载第 " + std::to_wstring(pageIndex + 1) + L" 页：" +
                DescribeError(FPDF_GetLastError());
        return false;
    }

    // FPDFText 返回的字符外框以页面原始坐标为准，而页面尺寸与渲染使用裁剪框。
    // 裁剪框原点不在 (0, 0) 时两者相差一个常量位移，会让选中高亮整体偏移；
    // 这里取出裁剪框原点，生成外框时统一扣除。
    double cropLeft = 0.0;
    double cropBottom = 0.0;
    {
        FS_RECTF cropRect{};
        if (FPDFPage_GetCropBox(page, &cropRect.left, &cropRect.bottom, &cropRect.right,
                                &cropRect.top)) {
            cropLeft = cropRect.left;
            cropBottom = cropRect.bottom;
        }
    }

    PageInfo& info = pages_[static_cast<size_t>(pageIndex)];
    info.hasFullPageImage =
        HasFullPageImage(page, info.widthPt, info.heightPt, static_cast<float>(cropLeft),
                         static_cast<float>(cropBottom));
    FPDF_TEXTPAGE textPage = FPDFText_LoadPage(page);
    if (!textPage) {
        // 没有文本层属于正常情况，由上层按固定页面显示。
        info.analyzed = true;
        info.textCharCount = 0;
        info.privateUseCharCount = 0;
        FPDF_ClosePage(page);
        return true;
    }

    TextLine line;
    bool hasLine = false;
    unsigned int lastCode = 0;   // 上一有效字符的码位，用于判断是否补空格
    bool pendingSpace = false;   // 已遇到空白，等待下一个有效字符决定是否写入
    const auto flush_line = [&]() {
        if (hasLine) {
            TrimTrailingSpaces(line.text);
            // 去掉行尾空格后同步裁剪外框，保持 text 与 boxes 等长。
            while (line.boxes.size() > line.text.size()) {
                line.boxes.pop_back();
            }
            if (!line.text.empty()) {
                result.lines.push_back(line);
            }
        }
        line = TextLine{};
        hasLine = false;
        lastCode = 0;
        pendingSpace = false;
    };

    const int charCount = FPDFText_CountChars(textPage);
    for (int i = 0; i < charCount; ++i) {
        const unsigned int code = FPDFText_GetUnicode(textPage, i);
        if (code == 0) {
            continue;
        }
        if (IsLineBreak(code)) {
            flush_line();
            continue;
        }
        if (IsPrivateUse(code)) {
            ++result.privateUseCharCount;
            continue;
        }

        double rawLeft = 0.0;
        double rawRight = 0.0;
        double rawBottom = 0.0;
        double rawTop = 0.0;
        const bool hasBox =
            FPDFText_GetCharBox(textPage, i, &rawLeft, &rawRight, &rawBottom, &rawTop) != 0;

        if (IsWhitespace(code)) {
            pendingSpace = true;
            continue;
        }
        ++result.textCharCount;

        // 空白只在西文之间保留；中文与全角标点前后不补空格。
        if (pendingSpace) {
            if (hasLine && !line.text.empty() && lastCode != 0 &&
                !(IsCjkLike(lastCode) && IsCjkLike(code))) {
                line.text.push_back(L' ');
                // 词间空格没有独立外框，取当前行右边界作为零宽位置。
                CharBox spaceBox;
                spaceBox.left = line.right;
                spaceBox.right = line.right;
                spaceBox.bottom = line.bottom;
                spaceBox.top = line.top;
                line.boxes.push_back(spaceBox);
            }
            pendingSpace = false;
        }

        // 扣除裁剪框原点，使外框与渲染后的页面坐标一致。
        const double charLeft = hasBox ? std::min(rawLeft, rawRight) - cropLeft : 0.0;
        const double charRight = hasBox ? std::max(rawLeft, rawRight) - cropLeft : 0.0;
        const double charBottom = hasBox ? std::min(rawBottom, rawTop) - cropBottom : 0.0;
        const double charTop = hasBox ? std::max(rawBottom, rawTop) - cropBottom : 0.0;
        const double rawFontSize = FPDFText_GetFontSize(textPage, i);
        const double charFontSize = rawFontSize > 0.0 ? rawFontSize : 0.0;

        if (hasLine) {
            const double overlapTop = std::min(charTop, static_cast<double>(line.top));
            const double overlapBottom =
                std::max(charBottom, static_cast<double>(line.bottom));
            const double overlap = overlapTop - overlapBottom;
            const double lineHeight =
                static_cast<double>(line.top) - static_cast<double>(line.bottom);
            const double charHeight = charTop - charBottom;
            const double minHeight = std::min(lineHeight, charHeight);
            // 无字符外框的字符（通常是生成字符）一律延续当前行。
            const bool sameLine =
                !hasBox || (minHeight > 0.0 && overlap > minHeight * 0.5);
            if (!sameLine) {
                flush_line();
            }
        }

        if (!hasLine) {
            hasLine = true;
            line.left = static_cast<float>(charLeft);
            line.right = static_cast<float>(charRight);
            line.bottom = static_cast<float>(charBottom);
            line.top = static_cast<float>(charTop);
            line.fontSize = static_cast<float>(charFontSize);
        } else {
            if (hasBox) {
                const double referenceSize =
                    std::max(static_cast<double>(line.fontSize), charFontSize);
                // 词间空格由文本流中的空白字符给出；PDFium 会为纯位置间隙生成空白字符，
                // 因此这里不再按字符外框间隙补空格，避免在标点前后插入多余空格。
                line.left = std::min(line.left, static_cast<float>(charLeft));
                line.right = std::max(line.right, static_cast<float>(charRight));
                line.bottom = std::min(line.bottom, static_cast<float>(charBottom));
                line.top = std::max(line.top, static_cast<float>(charTop));
                line.fontSize = static_cast<float>(referenceSize);
            }
        }
        AppendCodePoint(line.text, code);
        // 每个 UTF-16 码元各记一份外框，代理对的两个码元共用同一外框。
        CharBox charBox;
        charBox.left = static_cast<float>(charLeft);
        charBox.right = static_cast<float>(charRight);
        charBox.bottom = static_cast<float>(charBottom);
        charBox.top = static_cast<float>(charTop);
        while (line.boxes.size() < line.text.size()) {
            line.boxes.push_back(charBox);
        }
        lastCode = code;
    }
    flush_line();

    // 统计 ASCII 符号占比。字体缺少 Unicode 映射时，拼音等西文字符会被解析成
    // !"#$ 之类的符号串；仅对以中文为主、几乎没有 ASCII 字母的页面启用该判断，
    // 避免误伤英文书籍与代码类页面。
    size_t textCharacters = 0;
    size_t symbolCharacters = 0;
    size_t asciiLetters = 0;
    for (const TextLine& item : result.lines) {
        for (const wchar_t character : item.text) {
            if (character == L' ' || character == L'\t' || character == 0x3000) {
                continue;
            }
            ++textCharacters;
            if ((character >= L'a' && character <= L'z') ||
                (character >= L'A' && character <= L'Z')) {
                ++asciiLetters;
            } else if (IsSuspiciousAsciiSymbol(character)) {
                ++symbolCharacters;
            }
        }
    }
    info.textLayerSuspicious = textCharacters >= 40 &&
                               symbolCharacters * 25 >= textCharacters &&
                               asciiLetters * 10 < textCharacters;

    FPDFText_ClosePage(textPage);
    FPDF_ClosePage(page);

    info.analyzed = true;
    info.textCharCount = result.textCharCount;
    info.privateUseCharCount = result.privateUseCharCount;
    return true;
}

bool Document::RenderPage(int pageIndex, float scale, PageBitmapPtr& bitmap, std::wstring& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!document_) {
        error = L"文档尚未打开";
        return false;
    }
    if (pageIndex < 0 || pageIndex >= pageCount_) {
        error = L"页码超出范围：" + std::to_wstring(pageIndex);
        return false;
    }
    if (!std::isfinite(scale) || scale <= 0.0f) {
        error = L"无效的渲染缩放比例：" + std::to_wstring(scale);
        return false;
    }

    if (cachedBitmap_ && cachedPage_ == pageIndex &&
        std::fabs(cachedScale_ - scale) <= kScaleEpsilon) {
        bitmap = cachedBitmap_;
        return true;
    }

    const PageInfo& info = pages_[static_cast<size_t>(pageIndex)];
    const float widthPt = info.widthPt > 0.0f ? info.widthPt : 595.0f;
    const float heightPt = info.heightPt > 0.0f ? info.heightPt : 842.0f;

    float effectiveScale = scale;
    const float limitWidth = static_cast<float>(kMaxRenderPixels) / widthPt;
    const float limitHeight = static_cast<float>(kMaxRenderPixels) / heightPt;
    effectiveScale = std::min(effectiveScale, std::min(limitWidth, limitHeight));

    int pixelWidth = static_cast<int>(std::round(widthPt * effectiveScale));
    int pixelHeight = static_cast<int>(std::round(heightPt * effectiveScale));
    pixelWidth = std::max(1, std::min(pixelWidth, kMaxRenderPixels));
    pixelHeight = std::max(1, std::min(pixelHeight, kMaxRenderPixels));

    FPDF_DOCUMENT document = static_cast<FPDF_DOCUMENT>(document_);
    FPDF_PAGE page = FPDF_LoadPage(document, pageIndex);
    if (!page) {
        error = L"无法加载第 " + std::to_wstring(pageIndex + 1) + L" 页：" +
                DescribeError(FPDF_GetLastError());
        return false;
    }

    auto rendered = std::make_shared<PageBitmap>();
    rendered->width = pixelWidth;
    rendered->height = pixelHeight;
    rendered->pixels.assign(static_cast<size_t>(pixelWidth) * pixelHeight * 4, 0xFF);

    FPDF_BITMAP target = FPDFBitmap_CreateEx(pixelWidth, pixelHeight, FPDFBitmap_BGRA,
                                             rendered->pixels.data(), pixelWidth * 4);
    if (!target) {
        FPDF_ClosePage(page);
        error = L"无法为第 " + std::to_wstring(pageIndex + 1) + L" 页创建位图";
        return false;
    }
    FPDFBitmap_FillRect(target, 0, 0, pixelWidth, pixelHeight, 0xFFFFFFFFu);
    FPDF_RenderPageBitmap(target, page, 0, 0, pixelWidth, pixelHeight, 0, FPDF_ANNOT);
    FPDFBitmap_Destroy(target);
    FPDF_ClosePage(page);

    cachedBitmap_ = rendered;
    cachedPage_ = pageIndex;
    cachedScale_ = effectiveScale;
    bitmap = cachedBitmap_;
    return true;
}

std::wstring Document::DescribeError(int code) {
    switch (code) {
        case FPDF_ERR_SUCCESS:
            return L"没有错误";
        case FPDF_ERR_UNKNOWN:
            return L"未知错误";
        case FPDF_ERR_FILE:
            return L"文件无法打开或已损坏";
        case FPDF_ERR_FORMAT:
            return L"不是有效的 PDF 文件";
        case FPDF_ERR_PASSWORD:
            return L"文件需要密码";
        case FPDF_ERR_SECURITY:
            return L"不支持的加密方案";
        case FPDF_ERR_PAGE:
            return L"页面内容损坏";
        default:
            return L"错误码 " + std::to_wstring(code);
    }
}

}  // namespace pdf
