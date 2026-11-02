#include "core/text_util.h"

#include <windows.h>
#include <bcrypt.h>

#include <chrono>
#include <cstdio>
#include <cwchar>

namespace util {

std::string ToUtf8(const std::wstring& text) {
    if (text.empty()) {
        return std::string();
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return std::string();
    }
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size,
                        nullptr, nullptr);
    return result;
}

std::wstring FromUtf8(const std::string& text) {
    if (text.empty()) {
        return std::wstring();
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0);
    if (size <= 0) {
        return std::wstring();
    }
    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
}

std::wstring Sha256Hex(const std::string& data) {
    if (data.size() > 0xFFFFFFFFull) {
        return std::wstring();
    }
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
        return std::wstring();
    }
    unsigned char digest[32] = {};
    const NTSTATUS status =
        BCryptHash(algorithm, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())),
                   static_cast<ULONG>(data.size()), digest, static_cast<ULONG>(sizeof(digest)));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0) {
        return std::wstring();
    }

    static const wchar_t kHexDigits[] = L"0123456789abcdef";
    std::wstring result;
    result.reserve(64);
    for (const unsigned char byte : digest) {
        result.push_back(kHexDigits[byte >> 4]);
        result.push_back(kHexDigits[byte & 0x0F]);
    }
    return result;
}

int64_t CurrentUnixSeconds() {
    const auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
}

std::wstring FormatDateTime(int64_t unixSeconds) {
    if (unixSeconds <= 0) {
        return L"—";
    }
    __time64_t value = static_cast<__time64_t>(unixSeconds);
    struct tm localTime {};
    if (_localtime64_s(&localTime, &value) != 0) {
        return L"—";
    }
    wchar_t buffer[32] = {};
    swprintf_s(buffer, L"%04d-%02d-%02d %02d:%02d", localTime.tm_year + 1900, localTime.tm_mon + 1,
               localTime.tm_mday, localTime.tm_hour, localTime.tm_min);
    return buffer;
}

std::wstring FormatFileSize(uint64_t bytes) {
    wchar_t buffer[64] = {};
    if (bytes < 1024) {
        swprintf_s(buffer, L"%llu B", static_cast<unsigned long long>(bytes));
    } else if (bytes < 1024ull * 1024) {
        swprintf_s(buffer, L"%.1f KB", static_cast<double>(bytes) / 1024.0);
    } else {
        swprintf_s(buffer, L"%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    }
    return buffer;
}

std::wstring ToLowerAscii(std::wstring text) {
    for (wchar_t& ch : text) {
        if (ch >= L'A' && ch <= L'Z') {
            ch = static_cast<wchar_t>(ch - L'A' + L'a');
        }
    }
    return text;
}

std::wstring Ellipsize(const std::wstring& text, size_t maxChars) {
    if (text.size() <= maxChars || maxChars == 0) {
        return text;
    }
    size_t length = maxChars - 1;
    if (length > 0 && text[length - 1] >= 0xD800 && text[length - 1] <= 0xDBFF) {
        --length;
    }
    std::wstring result = text.substr(0, length);
    result += L"…";
    return result;
}

std::wstring ExtensionForMime(const std::string& mimeType) {
    if (mimeType == "image/jpeg") {
        return L"jpg";
    }
    if (mimeType == "image/png") {
        return L"png";
    }
    if (mimeType == "image/gif") {
        return L"gif";
    }
    if (mimeType == "image/bmp") {
        return L"bmp";
    }
    if (mimeType == "image/webp") {
        return L"webp";
    }
    return std::wstring();
}

}  // namespace util
