#include "web/clipboard.h"

#include <windows.h>

#include <string>

namespace web {
namespace {

std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty()) {
        return std::wstring();
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                         static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) {
        return std::wstring();
    }
    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), result.data(),
                        size);
    return result;
}

std::string WideToUtf8(const std::wstring& text) {
    if (text.empty()) {
        return std::string();
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                         static_cast<int>(text.size()), nullptr, 0, nullptr,
                                         nullptr);
    if (size <= 0) {
        return std::string();
    }
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), result.data(),
                        size, nullptr, nullptr);
    return result;
}

}  // namespace

bool WriteClipboardText(const std::string& utf8_text) {
    const std::wstring wide = Utf8ToWide(utf8_text);
    if (wide.empty() && !utf8_text.empty()) {
        return false;
    }
    if (OpenClipboard(nullptr) == 0) {
        return false;
    }
    bool ok = false;
    if (EmptyClipboard() != 0) {
        /* 剪贴板数据需要以 GMEM_MOVEABLE 分配的全局内存，系统接管所有权。 */
        const size_t bytes = (wide.size() + 1) * sizeof(wchar_t);
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (memory != nullptr) {
            void* target = GlobalLock(memory);
            if (target != nullptr) {
                std::memcpy(target, wide.c_str(), bytes);
                GlobalUnlock(memory);
                if (SetClipboardData(CF_UNICODETEXT, memory) != nullptr) {
                    ok = true;
                } else {
                    GlobalFree(memory);
                }
            } else {
                GlobalFree(memory);
            }
        }
    }
    CloseClipboard();
    return ok;
}

bool ReadClipboardText(std::string* utf8_text) {
    if (utf8_text == nullptr) {
        return false;
    }
    if (OpenClipboard(nullptr) == 0) {
        return false;
    }
    bool ok = false;
    HANDLE handle = GetClipboardData(CF_UNICODETEXT);
    if (handle != nullptr) {
        const wchar_t* source = static_cast<const wchar_t*>(GlobalLock(handle));
        if (source != nullptr) {
            *utf8_text = WideToUtf8(std::wstring(source));
            GlobalUnlock(handle);
            ok = true;
        }
    }
    CloseClipboard();
    return ok;
}

}  // namespace web
