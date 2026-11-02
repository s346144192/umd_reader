#pragma once

#include <cstdint>
#include <string>

namespace util {

std::string ToUtf8(const std::wstring& text);
std::wstring FromUtf8(const std::string& text);

// 返回 64 位小写十六进制 SHA-256 摘要，失败时返回空串。
std::wstring Sha256Hex(const std::string& data);

int64_t CurrentUnixSeconds();
std::wstring FormatDateTime(int64_t unixSeconds);
std::wstring FormatFileSize(uint64_t bytes);

std::wstring ToLowerAscii(std::wstring text);
std::wstring Ellipsize(const std::wstring& text, size_t maxChars);

// 根据图片 MIME 类型给出文件扩展名，未知类型返回空串。
std::wstring ExtensionForMime(const std::string& mimeType);

}  // namespace util
