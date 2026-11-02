#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace umd {

// 章节信息。偏移与长度均以 UTF-16 码元为单位，与 UMD 正文编码一致。
struct Chapter {
    std::wstring title;
    uint32_t charOffset = 0;
    uint32_t charLength = 0;
};

struct ImageResource {
    std::vector<uint8_t> bytes;
    std::string mimeType;
};

struct Book {
    uint8_t formatType = 0;  // 1 = 文本格式，2 = 动漫格式
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
    std::vector<std::wstring> warnings;

    uint32_t ContentCharCount() const { return static_cast<uint32_t>(content.size()); }
};

bool ParseFile(const std::filesystem::path& path, Book& book, std::wstring& error);

// 解析纯文本文件（支持 UTF-8、UTF-16 与系统 ANSI 编码），并自动识别章节。
bool ParseTextFile(const std::filesystem::path& path, Book& book, std::wstring& error);

// 按扩展名分派：.txt 走文本解析，其余按 UMD 解析。
bool ParseBookFile(const std::filesystem::path& path, Book& book, std::wstring& error);

}  // namespace umd
