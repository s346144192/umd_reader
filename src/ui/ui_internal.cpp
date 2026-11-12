#include "ui/ui_internal.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <string>
#include <vector>

namespace ui {
namespace detail {

std::wstring FormatRatioValue(float value) {
    const float rounded = std::round(value * 10.0f) / 10.0f;
    if (std::abs(rounded - static_cast<float>(std::lround(rounded))) < 0.05f) {
        return std::to_wstring(std::lround(rounded));
    }
    wchar_t buffer[16] = {};
    swprintf_s(buffer, L"%.1f", rounded);
    return buffer;
}

std::wstring ImageRatioLabel(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) {
        return std::wstring();
    }
    const bool landscape = width >= height;
    const float other = landscape ? kViewRatioBaseWidth * static_cast<float>(height) /
                                        static_cast<float>(width)
                                  : kViewRatioBaseWidth * static_cast<float>(width) /
                                        static_cast<float>(height);
    const std::wstring otherText = FormatRatioValue(other);
    return landscape ? L"16:" + otherText : otherText + L":16";
}

std::wstring ViewRatioLabel(float width, float height) {
    if (width <= 0.0f || height <= 0.0f) {
        return std::wstring();
    }
    return L"16:" + FormatRatioValue(kViewRatioBaseWidth * height / width);
}

D2D1_RECT_F OffsetRect(const D2D1_RECT_F& rect, float dx, float dy) {
    return D2D1::RectF(rect.left + dx, rect.top + dy, rect.right + dx, rect.bottom + dy);
}

std::wstring KeyNameFromVk(UINT vk) {
    switch (vk) {
    case VK_LEFT:
        return L"←";
    case VK_RIGHT:
        return L"→";
    case VK_UP:
        return L"↑";
    case VK_DOWN:
        return L"↓";
    case VK_PRIOR:
        return L"PageUp";
    case VK_NEXT:
        return L"PageDown";
    case VK_HOME:
        return L"Home";
    case VK_END:
        return L"End";
    case VK_SPACE:
        return L"Space";
    case VK_ESCAPE:
        return L"Esc";
    case VK_RETURN:
        return L"Enter";
    case VK_TAB:
        return L"Tab";
    case VK_BACK:
        return L"Backspace";
    default:
        break;
    }
    if (vk >= 'A' && vk <= 'Z') {
        return std::wstring(1, static_cast<wchar_t>(vk));
    }
    if (vk >= '0' && vk <= '9') {
        return std::wstring(1, static_cast<wchar_t>(vk));
    }
    if (vk >= VK_F1 && vk <= VK_F24) {
        return L"F" + std::to_wstring(vk - VK_F1 + 1);
    }
    return L"VK" + std::to_wstring(vk);
}

UINT VkFromKeyName(const std::wstring& name) {
    if (name.size() == 1) {
        const wchar_t character = name[0];
        if (character >= L'A' && character <= L'Z') {
            return static_cast<UINT>(character);
        }
        if (character >= L'0' && character <= L'9') {
            return static_cast<UINT>(character);
        }
    }
    if (name == L"←") return VK_LEFT;
    if (name == L"→") return VK_RIGHT;
    if (name == L"↑") return VK_UP;
    if (name == L"↓") return VK_DOWN;
    if (name == L"PageUp") return VK_PRIOR;
    if (name == L"PageDown") return VK_NEXT;
    if (name == L"Home") return VK_HOME;
    if (name == L"End") return VK_END;
    if (name == L"Space") return VK_SPACE;
    if (name == L"Esc") return VK_ESCAPE;
    if (name == L"Enter") return VK_RETURN;
    if (name == L"Tab") return VK_TAB;
    if (name == L"Backspace") return VK_BACK;
    if (name.size() >= 2 && name[0] == L'F') {
        int number = 0;
        try {
            number = std::stoi(name.substr(1));
        } catch (const std::exception&) {
            return 0;
        }
        if (number >= 1 && number <= 24) {
            return VK_F1 + static_cast<UINT>(number - 1);
        }
    }
    return 0;
}

ShortcutSpec ParseShortcut(const std::wstring& combo) {
    ShortcutSpec spec;
    std::wstring rest = combo;
    while (true) {
        const size_t plus = rest.find(L'+');
        if (plus == std::wstring::npos) {
            spec.key = rest;
            break;
        }
        const std::wstring token = rest.substr(0, plus);
        if (token == L"Ctrl") {
            spec.ctrl = true;
        } else if (token == L"Shift") {
            spec.shift = true;
        } else if (token == L"Alt") {
            spec.alt = true;
        } else {
            spec.key = rest;
            break;
        }
        rest = rest.substr(plus + 1);
    }
    return spec;
}

std::wstring ShortcutFromKey(UINT vk) {
    std::wstring text;
    if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) {
        text += L"Ctrl+";
    }
    if ((GetKeyState(VK_SHIFT) & 0x8000) != 0) {
        text += L"Shift+";
    }
    if ((GetKeyState(VK_MENU) & 0x8000) != 0) {
        text += L"Alt+";
    }
    text += KeyNameFromVk(vk);
    return text;
}

bool ShortcutMatches(const std::wstring& combo, UINT vk) {
    const ShortcutSpec spec = ParseShortcut(combo);
    if (spec.key.empty() || VkFromKeyName(spec.key) != vk) {
        return false;
    }
    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
    return spec.ctrl == ctrl && spec.shift == shift && spec.alt == alt;
}

std::wstring ShortcutCombo(const data::ReaderSettings& settings, const wchar_t* action,
                           const wchar_t* fallback) {
    for (const data::ShortcutBinding& binding : settings.shortcuts) {
        if (binding.action == action && !binding.combo.empty()) {
            return binding.combo;
        }
    }
    return fallback;
}

D2D1_COLOR_F ParseHexColor(const std::wstring& hex, float alpha) {
    const wchar_t* text = hex.c_str();
    if (!hex.empty() && text[0] == L'#') {
        ++text;
    }
    const unsigned int value =
        static_cast<unsigned int>(wcstoul(text, nullptr, 16)) & 0xFFFFFFu;
    return D2D1::ColorF(((value >> 16) & 0xFF) / 255.0f, ((value >> 8) & 0xFF) / 255.0f,
                        (value & 0xFF) / 255.0f, alpha);
}

std::wstring TrimNoteText(const std::wstring& text) {
    size_t begin = 0;
    size_t end = text.size();
    const auto isSpace = [](wchar_t character) {
        return character == L' ' || character == L'\t' || character == L'\r' ||
               character == L'\n' || character == 0x3000;
    };
    while (begin < end && isSpace(text[begin])) {
        ++begin;
    }
    while (end > begin && isSpace(text[end - 1])) {
        --end;
    }
    return text.substr(begin, end - begin);
}

std::wstring NormalizeCommandText(const std::wstring& text) {
    std::wstring result = text;
    for (wchar_t& character : result) {
        switch (character) {
        case 0xFF1A:  // ：
            character = L':';
            break;
        case 0xFF0C:  // ，
            character = L',';
            break;
        case 0xFF0D:  // －
            character = L'-';
            break;
        case 0xFF5E:  // ～
            character = L'~';
            break;
        default:
            break;
        }
        if (character >= 0xFF10 && character <= 0xFF19) {
            character = static_cast<wchar_t>(L'0' + (character - 0xFF10));
        }
    }
    return result;
}

bool ExtractFirstNumber(const std::wstring& text, uint32_t& value) {
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] < L'0' || text[i] > L'9') {
            continue;
        }
        size_t end = i;
        while (end < text.size() && text[end] >= L'0' && text[end] <= L'9') {
            ++end;
        }
        const unsigned long long parsed = wcstoull(text.substr(i, end - i).c_str(), nullptr, 10);
        value = parsed > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(parsed);
        return true;
    }
    return false;
}

D2D1_COLOR_F NoteTextColor() {
    return D2D1::ColorF(0xC62828);
}

D2D1_COLOR_F NoteFrameColor() {
    return D2D1::ColorF(0xD32F2F);
}

D2D1_COLOR_F Darken(const D2D1_COLOR_F& color, float factor) {
    return D2D1::ColorF(color.r * factor, color.g * factor, color.b * factor, color.a);
}

D2D1_SIZE_F ImageDisplaySize(ID2D1Bitmap* bitmap, float maxWidth, float maxHeight) {
    if (!bitmap) {
        return D2D1::SizeF(maxWidth, 160.0f);
    }
    const D2D1_SIZE_F size = bitmap->GetSize();
    if (size.width <= 0.0f || size.height <= 0.0f) {
        return D2D1::SizeF(maxWidth, 160.0f);
    }
    float width = maxWidth;
    float height = width * size.height / size.width;
    if (height > maxHeight) {
        height = maxHeight;
        width = height * size.width / size.height;
    }
    return D2D1::SizeF(width, height);
}

bool PointInside(const D2D1_RECT_F& rect, float x, float y) {
    return x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom;
}

D2D1_RECT_F IntersectRect(const D2D1_RECT_F& a, const D2D1_RECT_F& b) {
    return D2D1::RectF(std::max(a.left, b.left), std::max(a.top, b.top), std::min(a.right, b.right),
                       std::min(a.bottom, b.bottom));
}

std::wstring LowerCase(std::wstring text) {
    for (wchar_t& character : text) {
        character = static_cast<wchar_t>(std::towlower(character));
    }
    return text;
}

std::wstring SanitizeLine(const std::wstring& text) {
    std::wstring result;
    result.reserve(text.size());
    bool pendingSpace = false;
    for (const wchar_t character : text) {
        if (character == L'\r' || character == L'\n' || character == L'\t') {
            if (!result.empty()) {
                pendingSpace = true;
            }
            continue;
        }
        if (pendingSpace) {
            result.push_back(L' ');
            pendingSpace = false;
        }
        result.push_back(character);
    }
    while (!result.empty() && result.front() == L' ') {
        result.erase(result.begin());
    }
    while (!result.empty() && result.back() == L' ') {
        result.pop_back();
    }
    return result;
}

std::wstring ChapterTitleAt(const umd::Book& book, int index) {
    if (index < 0 || index >= static_cast<int>(book.chapters.size())) {
        return std::wstring();
    }
    const std::wstring& title = book.chapters[static_cast<size_t>(index)].title;
    if (title.empty()) {
        return L"第 " + std::to_wstring(index + 1) + L" 章";
    }
    return title;
}

void RemoveLastCodeUnit(std::wstring& text) {
    if (text.empty()) {
        return;
    }
    const wchar_t last = text.back();
    text.pop_back();
    if (last >= 0xDC00 && last <= 0xDFFF && !text.empty() && text.back() >= 0xD800 &&
        text.back() <= 0xDBFF) {
        text.pop_back();
    }
}

std::wstring FormatCountLine(size_t bookCount) {
    return L"共 " + std::to_wstring(bookCount) + L" 本 · 书籍记录保存在 data/library.json";
}

D2D1_POINT_2F CaretPointAt(const std::wstring& text, IDWriteTextFormat* format,
                           const D2D1_RECT_F& textRect, IDWriteFactory* factory, bool wrap,
                           size_t requestedIndex, float fallbackLineHeight, float& lineHeight) {
    lineHeight = fallbackLineHeight;
    D2D1_POINT_2F caret = D2D1::Point2F(textRect.left, textRect.top + fallbackLineHeight);
    if (!format || !factory) {
        return caret;
    }
    const uint32_t length = static_cast<uint32_t>(text.size());
    if (length == 0) {
        return caret;
    }
    const float width = std::max(1.0f, textRect.right - textRect.left);
    ComPtr<IDWriteTextLayout> layout;
    factory->CreateTextLayout(text.c_str(), length, format, width, 100000.0f, &layout);
    if (!layout) {
        return caret;
    }
    if (wrap) {
        layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    }

    const uint32_t index = static_cast<uint32_t>(std::min<size_t>(requestedIndex, length));
    FLOAT pointX = 0.0f;
    FLOAT pointY = 0.0f;
    DWRITE_HIT_TEST_METRICS metrics{};
    HRESULT result = E_FAIL;
    if (index == 0) {
        result = layout->HitTestTextPosition(0, FALSE, &pointX, &pointY, &metrics);
    } else if (index == length && (text.back() == L'\n' || text.back() == L'\r')) {
        // 光标位于换行之后：按行度量定位到末行行首。
        UINT32 lineCount = 0;
        layout->GetLineMetrics(nullptr, 0, &lineCount);
        if (lineCount == 0) {
            return caret;
        }
        std::vector<DWRITE_LINE_METRICS> lines(lineCount);
        layout->GetLineMetrics(lines.data(), lineCount, &lineCount);
        uint32_t lastLineStart = 0;
        for (UINT32 i = 0; i + 1 < lineCount; ++i) {
            lastLineStart += lines[i].length;
        }
        result = layout->HitTestTextPosition(lastLineStart, FALSE, &pointX, &pointY, &metrics);
    } else {
        result = layout->HitTestTextPosition(index - 1, TRUE, &pointX, &pointY, &metrics);
    }
    if (FAILED(result)) {
        return caret;
    }
    const float measuredHeight = metrics.height > 0.0f ? metrics.height : fallbackLineHeight;
    lineHeight = measuredHeight;
    caret.x = std::clamp(textRect.left + pointX, textRect.left, textRect.right);
    caret.y = std::clamp(textRect.top + pointY + measuredHeight, textRect.top, textRect.bottom);
    return caret;
}

size_t CaretIndexFromPoint(const std::wstring& text, IDWriteTextFormat* format, bool wrap,
                           const D2D1_RECT_F& textRect, IDWriteFactory* factory, float x, float y) {
    if (!format || !factory || text.empty()) {
        return 0;
    }
    const float width = std::max(1.0f, textRect.right - textRect.left);
    ComPtr<IDWriteTextLayout> layout;
    factory->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format, width,
                              100000.0f, &layout);
    if (!layout) {
        return text.size();
    }
    if (wrap) {
        layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    }
    const float localX = std::clamp(x - textRect.left, 0.0f, width);
    const float localY = std::max(0.0f, y - textRect.top);
    BOOL trailingHit = FALSE;
    BOOL inside = FALSE;
    DWRITE_HIT_TEST_METRICS metrics{};
    if (FAILED(layout->HitTestPoint(localX, localY, &trailingHit, &inside, &metrics))) {
        return text.size();
    }
    size_t index = static_cast<size_t>(metrics.textPosition);
    if (trailingHit && metrics.length > 0) {
        index += metrics.length;
    }
    return std::min(index, text.size());
}

bool WriteClipboardText(const std::wstring& text) {
    if (text.empty() || !OpenClipboard(nullptr)) {
        return false;
    }
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    bool succeeded = false;
    if (memory) {
        void* target = GlobalLock(memory);
        if (target) {
            std::memcpy(target, text.c_str(), bytes);
            GlobalUnlock(memory);
            if (SetClipboardData(CF_UNICODETEXT, memory) != nullptr) {
                succeeded = true;
            } else {
                GlobalFree(memory);
            }
        } else {
            GlobalFree(memory);
        }
    }
    CloseClipboard();
    return succeeded;
}

bool ReadClipboardText(std::wstring& text) {
    text.clear();
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !OpenClipboard(nullptr)) {
        return false;
    }
    bool succeeded = false;
    HANDLE handle = GetClipboardData(CF_UNICODETEXT);
    if (handle) {
        const wchar_t* data = static_cast<const wchar_t*>(GlobalLock(handle));
        if (data) {
            text.assign(data);
            GlobalUnlock(handle);
            succeeded = true;
        }
    }
    CloseClipboard();
    return succeeded;
}

bool ReadRegistryString(HKEY root, const std::wstring& subKey, const wchar_t* valueName,
                        std::wstring& value) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subKey.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return false;
    }
    DWORD type = 0;
    DWORD size = 0;
    LONG result = RegQueryValueExW(key, valueName, nullptr, &type, nullptr, &size);
    if (result != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ) || size < sizeof(wchar_t)) {
        RegCloseKey(key);
        return false;
    }
    std::vector<wchar_t> buffer(size / sizeof(wchar_t) + 1, L'\0');
    result = RegQueryValueExW(key, valueName, nullptr, &type,
                              reinterpret_cast<LPBYTE>(buffer.data()), &size);
    RegCloseKey(key);
    if (result != ERROR_SUCCESS) {
        return false;
    }
    value.assign(buffer.data());
    return true;
}

bool WriteRegistryString(HKEY root, const std::wstring& subKey, const wchar_t* valueName,
                         const std::wstring& value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(root, subKey.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) !=
        ERROR_SUCCESS) {
        return false;
    }
    const LONG result = RegSetValueExW(
        key, valueName, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
        static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

std::wstring ExecutablePath() {
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const DWORD length =
            GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return std::wstring();
        }
        if (length < buffer.size()) {
            return std::wstring(buffer.data(), length);
        }
        buffer.resize(buffer.size() * 2);
    }
}

bool IsFileExtensionAssociated(const std::wstring& extension, const std::wstring& progId) {
    std::wstring currentProgId;
    return ReadRegistryString(HKEY_CURRENT_USER, L"Software\\Classes\\" + extension, nullptr,
                              currentProgId) &&
           currentProgId == progId;
}

bool RegisterFileAssociation(const std::wstring& extension, const std::wstring& progId,
                             const std::wstring& description, const std::wstring& executable) {
    const std::wstring classesRoot = L"Software\\Classes\\";
    const std::wstring openCommand = L"\"" + executable + L"\" \"%1\"";
    bool succeeded = true;
    succeeded = WriteRegistryString(HKEY_CURRENT_USER, classesRoot + extension, nullptr,
                                    progId.c_str()) && succeeded;
    succeeded = WriteRegistryString(HKEY_CURRENT_USER, classesRoot + progId, nullptr,
                                    description) && succeeded;
    succeeded = WriteRegistryString(HKEY_CURRENT_USER, classesRoot + progId,
                                    L"FriendlyTypeName", description) && succeeded;
    succeeded = WriteRegistryString(HKEY_CURRENT_USER,
                                    classesRoot + progId + L"\\DefaultIcon", nullptr,
                                    executable + L",0") && succeeded;
    succeeded = WriteRegistryString(HKEY_CURRENT_USER,
                                    classesRoot + progId + L"\\shell\\open\\command", nullptr,
                                    openCommand) && succeeded;
    return succeeded;
}

bool RemoveFileAssociation(const std::wstring& extension, const std::wstring& progId) {
    const std::wstring classesRoot = L"Software\\Classes\\";
    bool succeeded = true;
    std::wstring currentProgId;
    // 只有当扩展名仍指向本程序的 ProgID 时才删除默认值，避免覆盖用户后来选择的默认应用。
    if (ReadRegistryString(HKEY_CURRENT_USER, classesRoot + extension, nullptr, currentProgId) &&
        currentProgId == progId) {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, (classesRoot + extension).c_str(), 0, KEY_SET_VALUE,
                          &key) == ERROR_SUCCESS) {
            if (RegDeleteValueW(key, nullptr) != ERROR_SUCCESS) {
                succeeded = false;
            }
            RegCloseKey(key);
        } else {
            succeeded = false;
        }
    }
    const LONG removed = RegDeleteTreeW(HKEY_CURRENT_USER, (classesRoot + progId).c_str());
    if (removed != ERROR_SUCCESS && removed != ERROR_FILE_NOT_FOUND) {
        succeeded = false;
    }
    return succeeded;
}

void DrawFolderIcon(RenderContext& renderer, float left, float top, const D2D1_COLOR_F& color) {
    renderer.FillRoundedRect(D2D1::RectF(left, top + 3.0f, left + 13.0f, top + 12.0f), 2.0f,
                             color);
    renderer.FillRoundedRect(D2D1::RectF(left + 1.0f, top, left + 8.0f, top + 4.0f), 1.5f,
                             color);
}

void DrawGridIcon(RenderContext& renderer, float left, float top, const D2D1_COLOR_F& color) {
    constexpr float kCell = 5.0f;
    constexpr float kGap = 2.0f;
    for (int row = 0; row < 2; ++row) {
        for (int column = 0; column < 2; ++column) {
            const float x = left + static_cast<float>(column) * (kCell + kGap);
            const float y = top + static_cast<float>(row) * (kCell + kGap);
            renderer.FillRoundedRect(D2D1::RectF(x, y, x + kCell, y + kCell), 1.0f, color);
        }
    }
}

void DrawInboxIcon(RenderContext& renderer, float left, float top, const D2D1_COLOR_F& color) {
    renderer.StrokeRoundedRect(D2D1::RectF(left, top + 1.0f, left + 13.0f, top + 12.0f), 2.0f,
                               color, 1.0f);
    renderer.FillRect(D2D1::RectF(left + 2.0f, top + 7.0f, left + 11.0f, top + 8.2f), color);
}

void DrawSlidersIcon(RenderContext& renderer, float left, float top, const D2D1_COLOR_F& color) {
    const float knobX[3] = {7.0f, 2.0f, 9.0f};
    for (int i = 0; i < 3; ++i) {
        const float y = top + 2.0f + static_cast<float>(i) * 4.0f;
        renderer.FillRect(D2D1::RectF(left, y, left + 13.0f, y + 1.2f), color);
        renderer.FillRoundedRect(
            D2D1::RectF(left + knobX[i], y - 1.4f, left + knobX[i] + 3.0f, y + 2.8f), 1.0f,
            color);
    }
}

void DrawPdfPageLayoutIcon(RenderContext& renderer, float centerX, float centerY,
                           bool twoPageLayout, const D2D1_COLOR_F& color) {
    constexpr float kSinglePageWidth = 13.0f;
    constexpr float kPageHeight = 18.0f;
    constexpr float kSpreadPageWidth = 11.0f;
    constexpr float kSpreadGap = 3.0f;
    const float pageTop = centerY - kPageHeight * 0.5f;
    const float pageBottom = centerY + kPageHeight * 0.5f;

    // 页面轮廓加两条短横线示意文字行；单页居中，双页左右并排。
    const auto drawPage = [&](float pageLeft, float pageWidth) {
        renderer.StrokeRoundedRect(
            D2D1::RectF(pageLeft, pageTop, pageLeft + pageWidth, pageBottom), 1.5f, color,
            1.4f);
        renderer.FillRect(
            D2D1::RectF(pageLeft + 2.5f, pageTop + 5.0f, pageLeft + pageWidth - 2.5f,
                        pageTop + 6.2f),
            color);
        renderer.FillRect(
            D2D1::RectF(pageLeft + 2.5f, pageTop + 9.0f, pageLeft + pageWidth - 2.5f,
                        pageTop + 10.2f),
            color);
    };

    if (twoPageLayout) {
        const float leftPageLeft = centerX - kSpreadGap * 0.5f - kSpreadPageWidth;
        drawPage(leftPageLeft, kSpreadPageWidth);
        drawPage(centerX + kSpreadGap * 0.5f, kSpreadPageWidth);
    } else {
        drawPage(centerX - kSinglePageWidth * 0.5f, kSinglePageWidth);
    }
}

void DrawPdfZoomIcon(RenderContext& renderer, float centerX, float centerY, bool zoomIn,
                     const D2D1_COLOR_F& color) {
    constexpr float kLensRadius = 6.0f;
    constexpr float kSymbolHalf = 2.8f;
    // 镜片略偏左上，手柄从镜片右下角延伸到图标右下角。
    const float lensCenterX = centerX - 1.5f;
    const float lensCenterY = centerY - 1.5f;
    renderer.StrokeRoundedRect(
        D2D1::RectF(lensCenterX - kLensRadius, lensCenterY - kLensRadius,
                    lensCenterX + kLensRadius, lensCenterY + kLensRadius),
        kLensRadius, color, 1.4f);
    renderer.DrawLine(D2D1::Point2F(lensCenterX + 4.4f, lensCenterY + 4.4f),
                      D2D1::Point2F(centerX + 6.5f, centerY + 6.5f), color, 1.6f);
    renderer.DrawLine(D2D1::Point2F(lensCenterX - kSymbolHalf, lensCenterY),
                      D2D1::Point2F(lensCenterX + kSymbolHalf, lensCenterY), color, 1.4f);
    if (zoomIn) {
        renderer.DrawLine(D2D1::Point2F(lensCenterX, lensCenterY - kSymbolHalf),
                          D2D1::Point2F(lensCenterX, lensCenterY + kSymbolHalf), color, 1.4f);
    }
}

}  // namespace detail
}  // namespace ui
