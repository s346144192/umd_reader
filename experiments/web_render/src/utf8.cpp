#include "web/utf8.h"

#include <algorithm>

namespace web {

Utf8Char DecodeUtf8(const std::string& text, size_t offset) {
    Utf8Char result;
    if (offset >= text.size()) {
        return result;
    }

    const unsigned char first = static_cast<unsigned char>(text[offset]);
    if (first < 0x80) {
        result.codepoint = first;
        result.length = 1;
        return result;
    }

    // 2 字节：110xxxxx 10xxxxxx
    if ((first & 0xE0) == 0xC0 && offset + 1 < text.size()) {
        const unsigned char second = static_cast<unsigned char>(text[offset + 1]);
        if ((second & 0xC0) == 0x80) {
            result.codepoint = ((first & 0x1F) << 6) | (second & 0x3F);
            result.length = 2;
            return result;
        }
    }
    // 3 字节：1110xxxx 10xxxxxx 10xxxxxx
    if ((first & 0xF0) == 0xE0 && offset + 2 < text.size()) {
        const unsigned char second = static_cast<unsigned char>(text[offset + 1]);
        const unsigned char third = static_cast<unsigned char>(text[offset + 2]);
        if ((second & 0xC0) == 0x80 && (third & 0xC0) == 0x80) {
            result.codepoint = ((first & 0x0F) << 12) | ((second & 0x3F) << 6) | (third & 0x3F);
            result.length = 3;
            return result;
        }
    }
    // 4 字节：11110xxx 10xxxxxx 10xxxxxx 10xxxxxx
    if ((first & 0xF8) == 0xF0 && offset + 3 < text.size()) {
        const unsigned char second = static_cast<unsigned char>(text[offset + 1]);
        const unsigned char third = static_cast<unsigned char>(text[offset + 2]);
        const unsigned char fourth = static_cast<unsigned char>(text[offset + 3]);
        if ((second & 0xC0) == 0x80 && (third & 0xC0) == 0x80 && (fourth & 0xC0) == 0x80) {
            result.codepoint = ((first & 0x07) << 18) | ((second & 0x3F) << 12) |
                               ((third & 0x3F) << 6) | (fourth & 0x3F);
            result.length = 4;
            return result;
        }
    }

    // 非法序列：跳过单字节，返回 0 以便调用方原样复制。
    result.codepoint = 0;
    result.length = 1;
    return result;
}

std::string EncodeUtf8(uint32_t codepoint) {
    std::string result;
    if (codepoint <= 0x7F) {
        result.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7FF) {
        result.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
        result.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else if (codepoint <= 0xFFFF) {
        result.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
        result.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        result.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else {
        result.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
        result.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
        result.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        result.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
    return result;
}

size_t Utf8Length(const std::string& text) {
    size_t count = 0;
    size_t offset = 0;
    while (offset < text.size()) {
        offset += DecodeUtf8(text, offset).length;
        ++count;
    }
    return count;
}

bool IsAsciiSpace(char character) {
    return character == ' ' || character == '\t' || character == '\n' || character == '\r' ||
           character == '\f';
}

bool IsAsciiAlpha(char character) {
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z');
}

bool IsAsciiDigit(char character) {
    return character >= '0' && character <= '9';
}

bool IsAsciiHexDigit(char character) {
    return IsAsciiDigit(character) || (character >= 'a' && character <= 'f') ||
           (character >= 'A' && character <= 'F');
}

char AsciiLowerChar(char character) {
    if (character >= 'A' && character <= 'Z') {
        return static_cast<char>(character - 'A' + 'a');
    }
    return character;
}

std::string AsciiLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](char character) { return AsciiLowerChar(character); });
    return value;
}

void TrimAsciiSpace(std::string* value) {
    if (value == nullptr) {
        return;
    }
    size_t begin = 0;
    while (begin < value->size() && IsAsciiSpace((*value)[begin])) {
        ++begin;
    }
    size_t end = value->size();
    while (end > begin && IsAsciiSpace((*value)[end - 1])) {
        --end;
    }
    value->assign(value->c_str() + begin, end - begin);
}

bool IsCjkCodepoint(uint32_t codepoint) {
    return (codepoint >= 0x2E80 && codepoint <= 0x2EFF) ||
           (codepoint >= 0x3000 && codepoint <= 0x303F) ||
           (codepoint >= 0x3040 && codepoint <= 0x30FF) ||
           (codepoint >= 0x3100 && codepoint <= 0x318F) ||
           (codepoint >= 0x31A0 && codepoint <= 0x31BF) ||
           (codepoint >= 0x3400 && codepoint <= 0x4DBF) ||
           (codepoint >= 0x4E00 && codepoint <= 0x9FFF) ||
           (codepoint >= 0xA960 && codepoint <= 0xA97F) ||
           (codepoint >= 0xAC00 && codepoint <= 0xD7AF) ||
           (codepoint >= 0xF900 && codepoint <= 0xFAFF) ||
           (codepoint >= 0xFE10 && codepoint <= 0xFE1F) ||
           (codepoint >= 0xFF00 && codepoint <= 0xFFEF) ||
           (codepoint >= 0x20000 && codepoint <= 0x2FA1F);
}

bool IsHangingPunctuation(uint32_t codepoint) {
    switch (codepoint) {
        // 中文标点：句号、逗号、顿号、分号、冒号、问号、叹号
        case 0x3002:  // 。
        case 0xFF0C:  // ，
        case 0x3001:  // 、
        case 0xFF1B:  // ；
        case 0xFF1A:  // ：
        case 0xFF1F:  // ？
        case 0xFF01:  // ！
        // 右括号与右引号
        case 0xFF09:  // ）
        case 0x3011:  // 】
        case 0x300B:  // 》
        case 0x3009:  // 〉
        case 0x300D:  // 」
        case 0x300F:  // 』
        case 0x201D:  // ”
        case 0x2019:  // ’
        // 半角标点
        case '.':
        case ',':
        case ';':
        case ':':
        case '!':
        case '?':
        case ')':
        case ']':
        case '}':
        case '%':
            return true;
        default:
            return false;
    }
}

bool IsLeadingPunctuation(uint32_t codepoint) {
    switch (codepoint) {
        case 0xFF08:  // （
        case 0x3010:  // 【
        case 0x300A:  // 《
        case 0x3008:  // 〈
        case 0x3016:  // 〖
        case 0x300C:  // 「
        case 0x300E:  // 『
        case 0x201C:  // “
        case 0x2018:  // ‘
        case '(':
        case '[':
        case '{':
        case '$':
        case '#':
            return true;
        default:
            return false;
    }
}

}  // namespace web
