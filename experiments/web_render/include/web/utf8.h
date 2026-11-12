#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace web {

// UTF-8 解码结果：codepoint 为解码后的码位，length 为消耗的字节数。
// 非法序列返回 codepoint = 0 且 length = 1，调用方应原样跳过该字节。
struct Utf8Char {
    uint32_t codepoint = 0;
    size_t length = 0;
};

Utf8Char DecodeUtf8(const std::string& text, size_t offset);
std::string EncodeUtf8(uint32_t codepoint);
size_t Utf8Length(const std::string& text);

// ASCII 层面的字符工具，用于分词、空白折叠与大小写归一。
bool IsAsciiSpace(char character);
bool IsAsciiAlpha(char character);
bool IsAsciiDigit(char character);
bool IsAsciiHexDigit(char character);
char AsciiLowerChar(char character);
std::string AsciiLower(std::string value);
void TrimAsciiSpace(std::string* value);

// 判断是否为中日韩统一表意文字、假名、谚文或全角标点，排版断行时需要按字切分。
bool IsCjkCodepoint(uint32_t codepoint);
// 不允许出现在行首的标点（避头标点），例如句号、逗号、右括号。
bool IsHangingPunctuation(uint32_t codepoint);
// 不允许出现在行尾的标点（避尾标点），例如左括号、左引号。
bool IsLeadingPunctuation(uint32_t codepoint);

}  // namespace web
