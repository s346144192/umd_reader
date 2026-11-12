#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace umd {

// 章节信息。偏移与长度均以 UTF-16 码元为单位，与 UMD 正文编码一致。
// 按原版页面阅读的 PDF 使用 pageIndex 定位章节，此时偏移与长度均为 0。
// 章节按目录树的先序排列：父节点在前，子节点紧随其后，level 表示层级。
struct Chapter {
    std::wstring title;
    uint32_t charOffset = 0;
    uint32_t charLength = 0;
    uint32_t pageIndex = 0;  // 0 起页序号，仅 PDF 按页阅读使用
    uint8_t level = 1;       // 目录层级，从 1 开始
};

struct ImageResource {
    std::vector<uint8_t> bytes;
    std::string mimeType;
};

// PDF 逐页信息：页面尺寸与文本层标记。按原版页面阅读时
// charOffset 与 charLength 保持为 0；hasText 仅用于诊断，不参与阅读流程。
struct PdfPageText {
    uint32_t charOffset = 0;
    uint32_t charLength = 0;
    bool hasText = false;  // 文本层诊断标记，按页阅读不使用
    float widthPt = 0.0f;
    float heightPt = 0.0f;
};

struct Book {
    uint8_t formatType = 0;  // 0 = 纯文本，1 = UMD 文本，2 = UMD 动漫，3 = PDF
    std::wstring title;
    std::wstring author;
    std::wstring year;
    std::wstring month;
    std::wstring day;
    std::wstring genre;
    std::wstring publisher;
    std::wstring retailer;

    uint32_t declaredContentBytes = 0;  // 0x0B 块声明的正文长度（字节）
    std::wstring content;               // 完整正文
    // 正文来源编码（Windows 代码页）。UMD 正文为 UTF-16LE（1200）；
    // 文本文件为检测到的编码，用于按原编码计算分区头样本的字节数。
    uint32_t sourceCodePage = 1200;
    std::vector<Chapter> chapters;
    std::vector<uint8_t> coverBytes;
    std::string coverMimeType;
    std::vector<ImageResource> images;
    // 仅 PDF 使用：逐页文本标记与页面尺寸，用于区分可重排页面与需按原版显示的页面。
    std::vector<PdfPageText> pdfPages;
    std::vector<std::wstring> warnings;

    uint32_t ContentCharCount() const { return static_cast<uint32_t>(content.size()); }
};

bool ParseFile(const std::filesystem::path& path, Book& book, std::wstring& error);

// 解析纯文本文件（支持 UTF-8、UTF-16 与系统 ANSI 编码），并自动识别章节。
bool ParseTextFile(const std::filesystem::path& path, Book& book, std::wstring& error);

// 解析 PDF 文件。按原版页面阅读：读取页面尺寸、页数与大纲，
// 章节按目标页序号定位；不提取文本层，导入与打开无需逐页解码。
bool ParsePdfFile(const std::filesystem::path& path, Book& book, std::wstring& error);

// 按扩展名分派：.txt 走文本解析，.pdf 走 PDF 解析，其余按 UMD 解析。
bool ParseBookFile(const std::filesystem::path& path, Book& book, std::wstring& error);

// 按指定单位提取章节。hierarchical 为 true 时 units 按层级从高到低排列，
// 用于「集 → 章 → 节」这类多级目录；为 false 时所有单位视为同级标题（自动识别）。
// 章节按目录树的先序排列，level 表示层级。
std::vector<Chapter> DetectChaptersByUnits(const std::wstring& text,
                                           const std::vector<wchar_t>& units, bool hierarchical);

// 使用默认单位集合（章、回、节、卷、篇、幕）识别章节，与纯文本首次解析的结果一致。
std::vector<Chapter> DetectDefaultChapters(const std::wstring& text);

// 章节提取方式（auto/hui/zhang/jie/jizhang/jizhangjie）对应的层级单位。
std::vector<wchar_t> ChapterUnitsForMode(const std::wstring& mode);

}  // namespace umd
