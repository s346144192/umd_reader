#include "ui/application.h"

#include <windowsx.h>
#include <d2d1helper.h>
#include <imm.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <winhttp.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <regex>
#include <filesystem>
#include <iterator>
#include <vector>

#include "core/text_util.h"
#include "nlohmann/json.hpp"

namespace ui {
namespace {

constexpr wchar_t kWindowClass[] = L"UmdReaderWindow";
constexpr wchar_t kWindowTitle[] = L"UMD 阅读器";
constexpr wchar_t kInstanceMutexName[] = L"Local\\UmdReaderInstance";
constexpr ULONG_PTR kCopyDataOpenFile = 0x554D4431;  // 'UMD1'，标识转交文件路径的消息
constexpr wchar_t kDocumentProgId[] = L"UmdReader.Document";
constexpr wchar_t kTextProgId[] = L"UmdReader.Text";
constexpr float kShelfToolbarHeight = 88.0f;
constexpr float kReaderToolbarHeight = 64.0f;
constexpr float kReaderFooterHeight = 44.0f;
constexpr float kPanelWidth = 372.0f;
constexpr float kPanelHeaderHeight = 56.0f;
constexpr float kShelfCardWidth = 204.0f;
constexpr float kShelfCardHeight = 392.0f;
constexpr float kShelfCardGapX = 28.0f;
constexpr float kShelfCardGapY = 34.0f;
constexpr float kShelfStartY = 128.0f;
constexpr float kShelfMargin = 44.0f;

struct FontOption {
    const wchar_t* label;
    const wchar_t* family;
};

constexpr FontOption kFontOptions[] = {
    {L"微软雅黑", L"Microsoft YaHei"},
    {L"宋体", L"SimSun"},
    {L"黑体", L"SimHei"},
    {L"楷体", L"KaiTi"},
};
constexpr size_t kFontOptionCount = sizeof(kFontOptions) / sizeof(kFontOptions[0]);

struct ThemeOption {
    const wchar_t* label;
    const wchar_t* name;
};

constexpr ThemeOption kThemeOptions[] = {
    {L"浅色", L"light"},
    {L"护眼", L"sepia"},
    {L"深色", L"dark"},
};
constexpr size_t kThemeOptionCount = sizeof(kThemeOptions) / sizeof(kThemeOptions[0]);

// 阅读区纯色背景预设，均为浅色，保证与主题文字颜色形成足够对比。
constexpr const wchar_t* kBackgroundColors[] = {
    L"#FFFFFF", L"#F7F3E8", L"#EAF4EA", L"#E8F1F8", L"#FFF9E6", L"#F7EFF7", L"#EDEDED",
};
constexpr size_t kBackgroundColorCount =
    sizeof(kBackgroundColors) / sizeof(kBackgroundColors[0]);
// 比例小字统一以宽度 16 为基准换算高度。
constexpr float kViewRatioBaseWidth = 16.0f;
// 背景缩略图的解码上限（像素），限制列表显示的显存占用。
constexpr float kBackgroundThumbMaxWidth = 320.0f;
constexpr float kBackgroundThumbMaxHeight = 220.0f;

// 比例数值：小数部分为 0 时取整，否则保留 1 位小数。
std::wstring FormatRatioValue(float value) {
    const float rounded = std::round(value * 10.0f) / 10.0f;
    if (std::abs(rounded - static_cast<float>(std::lround(rounded))) < 0.05f) {
        return std::to_wstring(std::lround(rounded));
    }
    wchar_t buffer[16] = {};
    swprintf_s(buffer, L"%.1f", rounded);
    return buffer;
}

// 图片比例小字：较长边固定为 16，短边按实际比例换算。
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

// 阅读视图比例：以宽度 16 为基准换算高度。
std::wstring ViewRatioLabel(float width, float height) {
    if (width <= 0.0f || height <= 0.0f) {
        return std::wstring();
    }
    return L"16:" + FormatRatioValue(kViewRatioBaseWidth * height / width);
}

constexpr int kMaxNoteChars = 255;
// 行滚动模式下每次布局的文本跨度。按整页可见行数的数倍取值，
// 既保证填满视口，又避免为每次重绘创建超大的文本布局。
constexpr uint32_t kLinePreviewChars = 4000;
constexpr float kGestureBarHeight = 44.0f;
constexpr float kGestureButtonWidth = 84.0f;
constexpr float kGestureButtonHeight = 30.0f;
constexpr float kGestureButtonGap = 6.0f;
constexpr float kGestureBarPadding = 6.0f;
constexpr wchar_t kUncategorizedFilter[] = L"__none__";
constexpr float kCategorySidebarWidth = 220.0f;
constexpr UINT_PTR kStatusTimerId = 1;
constexpr UINT_PTR kCopyToastTimerId = 2;
constexpr UINT_PTR kCaretTimerId = 3;
constexpr UINT kCaretBlinkIntervalMs = 500;
constexpr UINT kAiStreamMessage = WM_APP + 1;  // 后台线程通知主线程追加流式内容
constexpr int64_t kCopyToastSeconds = 3;
constexpr float kCopyToastWidth = 108.0f;
constexpr float kCopyToastHeight = 36.0f;
constexpr float kCopyToastBottomGap = 18.0f;
constexpr float kSearchBoxHeight = 42.0f;
constexpr float kSearchTextPaddingX = 14.0f;
constexpr float kSearchTextPaddingTop = 11.0f;
constexpr float kSearchTextPaddingBottom = 8.0f;
constexpr float kNoteInputHeight = 150.0f;
constexpr float kNoteInputPaddingX = 12.0f;
constexpr float kNoteInputPaddingY = 10.0f;
// 对话气泡排版参数：角色行高、内边距、间距与最大宽度占比。
constexpr float kChatBubblePadding = 10.0f;
constexpr float kChatBubbleHeader = 20.0f;
constexpr float kChatBubbleGap = 8.0f;
constexpr float kChatBubbleWidthRatio = 0.86f;
// 弹出菜单尺寸。
constexpr float kMenuWidth = 186.0f;
constexpr float kMenuItemHeight = 32.0f;
constexpr float kMenuPadding = 6.0f;

// 返回平移后的矩形，用于把相对排版坐标换算为窗口坐标。
D2D1_RECT_F OffsetRect(const D2D1_RECT_F& rect, float dx, float dy) {
    return D2D1::RectF(rect.left + dx, rect.top + dy, rect.right + dx, rect.bottom + dy);
}

// 按键的显示名：方向键用箭头，字母数字按原样，功能键用 F1…F24。
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

struct ShortcutSpec {
    bool ctrl = false;
    bool shift = false;
    bool alt = false;
    std::wstring key;
};

// 解析 Ctrl+Shift+X 形式的组合键描述。
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

// 用当前按下的键与修饰键生成组合键描述。
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

// 判断当前按键是否匹配组合键描述。
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

// 读取指定动作的组合键；未配置时使用默认值。
std::wstring ShortcutCombo(const data::ReaderSettings& settings, const wchar_t* action,
                           const wchar_t* fallback) {
    for (const data::ShortcutBinding& binding : settings.shortcuts) {
        if (binding.action == action && !binding.combo.empty()) {
            return binding.combo;
        }
    }
    return fallback;
}

struct ShortcutRow {
    const wchar_t* action;
    const wchar_t* label;
};

constexpr ShortcutRow kShortcutRows[] = {
    {data::kShortcutPrevPage, L"上一页"},
    {data::kShortcutNextPage, L"下一页"},
    {data::kShortcutSearch, L"全文检索"},
    {data::kShortcutBookmark, L"添加书签"},
    {data::kShortcutCopy, L"复制选中文字"},
    {data::kShortcutChatSend, L"发送对话"},
};
constexpr size_t kShortcutRowCount = sizeof(kShortcutRows) / sizeof(kShortcutRows[0]);

constexpr const wchar_t* kHighlightColors[16] = {
    L"#FFE082", L"#FFCC80", L"#FFAB91", L"#EF9A9A", L"#F48FB1", L"#CE93D8",
    L"#B39DDB", L"#9FA8DA", L"#90CAF9", L"#81D4FA", L"#80DEEA", L"#80CBC4",
    L"#A5D6A7", L"#C5E1A5", L"#E6EE9C", L"#D7CCC8",
};

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

// 命令文本归一化：全角冒号、逗号、连字符、波浪号与数字转为半角。
// 逐字符替换，长度保持不变，便于用同一位置在原文本中切分标注文字。
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

// 提取文本中的第一个连续整数，找不到时返回 false。
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

// 提取文本中的前两个连续整数，用于解析「正文范围 A - B」这类范围说明。
bool ExtractTwoNumbers(const std::wstring& text, uint32_t& first, uint32_t& second) {
    int count = 0;
    for (size_t i = 0; i < text.size() && count < 2; ++i) {
        if (text[i] < L'0' || text[i] > L'9') {
            continue;
        }
        size_t end = i;
        while (end < text.size() && text[end] >= L'0' && text[end] <= L'9') {
            ++end;
        }
        const unsigned long long parsed =
            wcstoull(text.substr(i, end - i).c_str(), nullptr, 10);
        const uint32_t value =
            parsed > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(parsed);
        if (count == 0) {
            first = value;
        } else {
            second = value;
        }
        ++count;
        i = end - 1;
    }
    return count == 2;
}

// 判断回复是否只是在表达「准备继续读取正文」的意向，而没有给出读取命令。
// 仅针对简短回复，避免把完整的讲解误判为读取请求。
bool WantsMoreText(const std::wstring& text) {
    if (text.size() > 100) {
        return false;
    }
    static const wchar_t* kKeywords[] = {
        L"读取", L"读一段", L"读几处", L"读几个", L"往后读", L"往下读",
        L"继续读", L"再读", L"找一段", L"往后找", L"往回找", L"往前找",
    };
    for (const wchar_t* keyword : kKeywords) {
        if (text.find(keyword) != std::wstring::npos) {
            return true;
        }
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

// 把换行与制表符压缩为单个空格，便于单行展示。
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

// 计算指定插入位置的底部坐标（DIP）与行高，供 IME 窗口与光标绘制使用。
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

// 把点击坐标换算为插入位置（码元索引）。
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

// 判断扩展名当前是否已指向本程序的 ProgID（当前用户级）。
// 只读取本程序维护的 Classes 默认值，保证复选框状态与勾选操作一致。
bool IsFileExtensionAssociated(const std::wstring& extension, const std::wstring& progId) {
    std::wstring currentProgId;
    return ReadRegistryString(HKEY_CURRENT_USER, L"Software\\Classes\\" + extension, nullptr,
                              currentProgId) &&
           currentProgId == progId;
}

// 为单个扩展名注册当前用户级文件关联。
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

// 移除单个扩展名的当前用户级文件关联；只清理本程序写入的 ProgID。
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

// 绘制分类用的简化文件夹图标，避免依赖额外图标资源。
void DrawFolderIcon(RenderContext& renderer, float left, float top, const D2D1_COLOR_F& color) {
    renderer.FillRoundedRect(D2D1::RectF(left, top + 3.0f, left + 13.0f, top + 12.0f), 2.0f,
                             color);
    renderer.FillRoundedRect(D2D1::RectF(left + 1.0f, top, left + 8.0f, top + 4.0f), 1.5f,
                             color);
}

// “全部显示”用网格图标：表示集合/全部条目，比文件夹更符合语义。
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

// “未分类”用收件托盘图标：表示尚未归档的内容，与文件夹区分。
void DrawInboxIcon(RenderContext& renderer, float left, float top, const D2D1_COLOR_F& color) {
    renderer.StrokeRoundedRect(D2D1::RectF(left, top + 1.0f, left + 13.0f, top + 12.0f), 2.0f,
                               color, 1.0f);
    renderer.FillRect(D2D1::RectF(left + 2.0f, top + 7.0f, left + 11.0f, top + 8.2f), color);
}

// “管理分类”用滑杆图标：表示设置与管理入口，不再使用文件夹图标。
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

}  // namespace

Application::Application() = default;

Application::~Application() {
    // 先结束后台请求线程，避免其继续访问已析构的成员。
    aiCancel_ = true;
    if (aiThread_.joinable()) {
        aiThread_.join();
    }
    renderer_.Shutdown();
    if (instanceMutex_ != nullptr) {
        ReleaseMutex(instanceMutex_);
        CloseHandle(instanceMutex_);
        instanceMutex_ = nullptr;
    }
    if (comInitialized_) {
        CoUninitialize();
    }
}

bool Application::Initialize(HINSTANCE instance, int showCommand) {
    instance_ = instance;
    showCommand_ = showCommand;

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    comInitialized_ = SUCCEEDED(comResult);

    // 收集命令行中实际存在的文件路径，供导入并在首帧打开。
    int argumentCount = 0;
    PWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (arguments) {
        for (int i = 1; i < argumentCount; ++i) {
            if (arguments[i] == nullptr || arguments[i][0] == L'\0') {
                continue;
            }
            std::error_code errorCode;
            if (std::filesystem::exists(arguments[i], errorCode)) {
                startupPaths_.emplace_back(arguments[i]);
            }
        }
        LocalFree(arguments);
    }

    // 单实例：已有实例在运行时，把文件转交过去并直接退出，避免两个进程写同一份数据。
    instanceMutex_ = CreateMutexW(nullptr, TRUE, kInstanceMutexName);
    if (instanceMutex_ != nullptr && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (ForwardToExistingInstance()) {
            exitImmediately_ = true;
            return true;
        }
    }

    std::wstring error;
    if (!library_.Initialize(error)) {
        MessageBoxW(nullptr, error.c_str(), kWindowTitle, MB_ICONERROR);
        return false;
    }
    // Initialize 成功时 error 可能携带非致命提示（数据目录回退、索引重建等）。
    startupNotice_ = error;

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance_;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = kWindowClass;
    RegisterClassExW(&windowClass);

    const data::ReaderSettings& settings = library_.Settings();
    RECT frameRect{0, 0, settings.windowWidth, settings.windowHeight};
    AdjustWindowRectExForDpi(&frameRect, WS_OVERLAPPEDWINDOW, FALSE, 0, GetDpiForSystem());
    const int windowWidth = frameRect.right - frameRect.left;
    const int windowHeight = frameRect.bottom - frameRect.top;

    hwnd_ = CreateWindowExW(0, kWindowClass, kWindowTitle, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                            CW_USEDEFAULT, windowWidth, windowHeight, nullptr, nullptr, instance_,
                            this);
    if (!hwnd_) {
        MessageBoxW(nullptr, L"创建主窗口失败。", kWindowTitle, MB_ICONERROR);
        return false;
    }
    if (!renderer_.WriteFactory()) {
        MessageBoxW(nullptr, L"Direct2D 与 DirectWrite 初始化失败。", kWindowTitle, MB_ICONERROR);
        return false;
    }
    return true;
}

int Application::Run() {
    if (exitImmediately_) {
        return 0;
    }
    if (!hwnd_) {
        return 1;
    }
    ShowWindow(hwnd_, showCommand_);
    UpdateWindow(hwnd_);
    OpenStartupFiles();

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}

LRESULT CALLBACK Application::WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    Application* application =
        reinterpret_cast<Application*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_CREATE) {
        auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
        application = static_cast<Application*>(createStruct->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(application));
        if (application) {
            application->hwnd_ = hwnd;
        }
    }
    if (application) {
        return application->HandleMessage(hwnd, message, wParam, lParam);
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT Application::HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        const UINT dpi = GetDpiForWindow(hwnd);
        const float scale = static_cast<float>(dpi) / 96.0f;
        // 最小客户区 720×520 DIP，反算为含边框的窗口外框尺寸。
        RECT frameRect{0, 0, static_cast<LONG>(std::lround(720.0f * scale)),
                       static_cast<LONG>(std::lround(520.0f * scale))};
        AdjustWindowRectExForDpi(&frameRect, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
        info->ptMinTrackSize.x = frameRect.right - frameRect.left;
        info->ptMinTrackSize.y = frameRect.bottom - frameRect.top;
        return 0;
    }
    case WM_CREATE:
        OnCreate();
        return 0;
    case WM_PAINT:
        OnPaint();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_ENTERSIZEMOVE:
        resizing_ = true;
        return 0;
    case WM_EXITSIZEMOVE:
        resizing_ = false;
        if (relayoutPending_) {
            relayoutPending_ = false;
            if (view_ == ViewMode::Reader && !activeBook_.content.empty()) {
                RebuildPagination(CurrentGlobalOffset());
            }
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            OnResize(LOWORD(lParam), HIWORD(lParam));
        }
        return 0;
    case WM_DPICHANGED:
        OnDpiChanged(*reinterpret_cast<const RECT*>(lParam));
        return 0;
    case WM_MOUSEMOVE:
        OnMouseMove(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;
    case WM_MOUSELEAVE:
        OnMouseLeave();
        return 0;
    case WM_LBUTTONDOWN:
        OnLeftButtonDown(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;
    case WM_LBUTTONDBLCLK: {
        const int doubleClickX = GET_X_LPARAM(lParam);
        const int doubleClickY = GET_Y_LPARAM(lParam);
        // 窗口类启用了 CS_DBLCLKS：快速连点时第二次只收到双击消息。
        // 双击手势仅用于阅读区中部打开设置，其余位置按普通单击分发，
        // 否则目录、书签等按钮会出现一次点击无响应。
        if (!OnLeftButtonDoubleClick(doubleClickX, doubleClickY)) {
            OnLeftButtonDown(doubleClickX, doubleClickY);
        }
        return 0;
    }
    case WM_LBUTTONUP:
        OnLeftButtonUp(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;
    case WM_RBUTTONDOWN:
        OnRightButtonDown(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;
    case WM_MOUSEWHEEL: {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(hwnd, &point);
        OnMouseWheel(point.x, point.y, GET_WHEEL_DELTA_WPARAM(wParam));
        return 0;
    }
    case WM_KEYDOWN:
        OnKeyDown(wParam);
        return 0;
    case WM_CHAR:
        OnChar(static_cast<wchar_t>(wParam));
        return 0;
    case WM_UNICHAR:
        if (wParam == UNICODE_NOCHAR) {
            return TRUE;
        }
        OnUnicodeInput(static_cast<uint32_t>(wParam));
        return 0;
    case WM_IME_STARTCOMPOSITION:
        // 组合开始时把组合窗口定位到插入符，随后交回默认处理以保留系统输入法界面。
        UpdateImePosition();
        return DefWindowProcW(hwnd, message, wParam, lParam);
    case WM_IME_COMPOSITION:
        // 组合字符串变化时同步位置；字符转译仍由默认处理负责，保留既有 WM_CHAR 路径。
        UpdateImePosition();
        return DefWindowProcW(hwnd, message, wParam, lParam);
    case WM_IME_SETCONTEXT:
        if (wParam) {
            UpdateImePosition();
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    case WM_COPYDATA: {
        auto* data = reinterpret_cast<const COPYDATASTRUCT*>(lParam);
        if (data != nullptr && HandleCopyData(*data)) {
            if (IsIconic(hwnd)) {
                ShowWindow(hwnd, SW_RESTORE);
            }
            SetForegroundWindow(hwnd);
            return TRUE;
        }
        return FALSE;
    }
    case WM_DROPFILES:
        OnDropFiles(reinterpret_cast<HDROP>(wParam));
        return 0;
    case kAiStreamMessage:
        AppendAiChunks();
        return 0;
    case WM_TIMER:
        if (wParam == kStatusTimerId &&
            statusExpiry_ > 0 && util::CurrentUnixSeconds() >= statusExpiry_) {
            statusText_.clear();
            statusExpiry_ = 0;
            KillTimer(hwnd, kStatusTimerId);
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (wParam == kCopyToastTimerId && copyToastVisible_ &&
                   util::CurrentUnixSeconds() >= copyToastExpiry_) {
            copyToastVisible_ = false;
            copyToastExpiry_ = 0;
            KillTimer(hwnd, kCopyToastTimerId);
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (wParam == kCaretTimerId) {
            caretBlinkVisible_ = !caretBlinkVisible_;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_CLOSE:
        OnClose();
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

void Application::OnCreate() {
    renderer_.Initialize(hwnd_);
    const data::ReaderSettings& settings = library_.Settings();
    renderer_.CreateTextFormats(settings.fontFamily, settings.fontSize, settings.lineSpacing,
                                settings.noteFontSize);
    // 创建语音合成引擎；失败时朗读按钮会给出提示，不影响其余功能。
    ISpVoice* voice = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_INPROC_SERVER, IID_ISpVoice,
                                   reinterpret_cast<void**>(&voice))) &&
        voice != nullptr) {
        voice_.Attach(voice);
    }
    DragAcceptFiles(hwnd_, TRUE);
    RefreshShelfOrder();
    RefreshBackgroundList();
    // 枚举系统中可显示中文的字族；枚举失败时退回内置预设。
    fontFamilies_ = renderer_.SystemFontFamilies();
    if (fontFamilies_.empty()) {
        for (const FontOption& option : kFontOptions) {
            fontFamilies_.push_back(FontFamilyName{option.label, option.family});
        }
    }
    if (!startupNotice_.empty()) {
        ShowStatus(startupNotice_);
        startupNotice_.clear();
    }
}

void Application::OnPaint() {
    PAINTSTRUCT paint{};
    HDC deviceContext = BeginPaint(hwnd_, &paint);
    RECT clientRect{};
    GetClientRect(hwnd_, &clientRect);

    const bool beginSucceeded = renderer_.BeginDraw(deviceContext, clientRect);
    if (beginSucceeded) {
        hitRegions_.clear();
        annotationHits_.clear();
        clipStack_.clear();
        const ThemeColors colors = ThemeByName(library_.Settings().theme);
        if (view_ == ViewMode::Shelf) {
            DrawShelf(colors);
        } else {
            DrawReader(colors);
        }
        // 弹出菜单绘制在两种视图之上，命中区域最后追加。
        DrawMenu(colors);
        const bool endSucceeded = renderer_.EndDraw();
        if (!endSucceeded) {
            coverCache_.clear();
            backgroundBitmap_.Reset();
            backgroundThumbCache_.clear();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
    } else {
        coverCache_.clear();
        backgroundBitmap_.Reset();
        backgroundThumbCache_.clear();
    }

    EndPaint(hwnd_, &paint);
}

void Application::OnResize(UINT widthPixels, UINT heightPixels) {
    if (!renderer_.WriteFactory()) {
        return;
    }
    const UINT dpi = GetDpiForWindow(hwnd_);
    const bool recreated = renderer_.Resize(widthPixels, heightPixels, dpi);
    const float scale = renderer_.DpiScale();
    widthDip_ = static_cast<float>(widthPixels) / scale;
    heightDip_ = static_cast<float>(heightPixels) / scale;
    if (recreated) {
        coverCache_.clear();
        backgroundBitmap_.Reset();
        backgroundThumbCache_.clear();
    }

    // 命中矩形、hover 索引全部失效，鼠标可能在新 client rect 之外
    hitRegions_.clear();
    annotationHits_.clear();
    clipStack_.clear();
    hoverIndex_ = -1;
    mouseInside_ = false;
    mouseTracking_ = false;

    if (view_ == ViewMode::Reader && !activeBook_.content.empty()) {
        // 拖动边框期间连续收到大量 WM_SIZE，逐次整章重排会造成明显迟滞；
        // 这里只记录待重排标记，等 WM_EXITSIZEMOVE 结束后一次性重排。
        if (resizing_) {
            relayoutPending_ = true;
        } else {
            relayoutPending_ = false;
            RebuildPagination(CurrentGlobalOffset());
        }
    } else {
        // 书架视图：重新计算最大滚动并 clamp
        shelfScroll_ = std::clamp(shelfScroll_, 0.0f, shelfScrollMax_);
    }
    panelScroll_ = std::clamp(panelScroll_, 0.0f, panelScrollMax_);
    UpdateImePosition();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Application::OnDpiChanged(const RECT& suggested) {
    SetWindowPos(hwnd_, nullptr, suggested.left, suggested.top, suggested.right - suggested.left,
                 suggested.bottom - suggested.top, SWP_NOZORDER | SWP_NOACTIVATE);
}

void Application::OnMouseMove(int xPixels, int yPixels) {
    const float scale = renderer_.DpiScale();
    mouseX_ = static_cast<float>(xPixels) / scale;
    mouseY_ = static_cast<float>(yPixels) / scale;
    mouseInside_ = true;
    if (chatPanelResizing_) {
        const float minimum = 300.0f;
        const float maximum = std::max(minimum, std::min(760.0f, widthDip_ - 420.0f));
        chatPanelWidth_ = std::clamp(widthDip_ - mouseX_, minimum, maximum);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (chatScrollDragging_) {
        ScrollChatFromY(mouseY_);
        return;
    }
    if (panelScrollDragging_) {
        ScrollPanelFromY(mouseY_);
        return;
    }
    if (chatSelecting_) {
        UpdateChatSelection(mouseX_, mouseY_);
        return;
    }
    if (inputSelecting_) {
        UpdateInputSelection(mouseX_, mouseY_);
        return;
    }
    if (scrollDragging_) {
        const float ratio = ScrollRatioFromY(mouseY_);
        if (std::abs(ratio - lastScrollRatio_) > 0.003f) {
            lastScrollRatio_ = ratio;
            GoToScrollRatio(ratio);
        }
        return;
    }
    if (!mouseTracking_) {
        TRACKMOUSEEVENT tracking{};
        tracking.cbSize = sizeof(tracking);
        tracking.dwFlags = TME_LEAVE;
        tracking.hwndTrack = hwnd_;
        TrackMouseEvent(&tracking);
        mouseTracking_ = true;
    }

    const HitRegion* hit = HitTest(mouseX_, mouseY_);
    const int newHover = hit ? static_cast<int>(hit - hitRegions_.data()) : -1;
    if (newHover != hoverIndex_) {
        hoverIndex_ = newHover;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    // 指向阅读助手面板左边缘时显示横向调整光标。
    if (panel_ == PanelMode::Chat) {
        const float splitterX = widthDip_ - PanelWidth();
        SetCursor(LoadCursorW(nullptr, std::abs(mouseX_ - splitterX) <= 4.0f ? IDC_SIZEWE
                                                                            : IDC_ARROW));
    }

    if (selecting_) {
        UpdateSelection(mouseX_, mouseY_);
    }
}

void Application::UpdateSelection(float x, float y) {
    const uint32_t offset = DisplayOffsetAt(x, y);
    const uint32_t low = std::min(selectionAnchorDisplay_, offset);
    const uint32_t high = std::max(selectionAnchorDisplay_, offset);
    if (low == selectionStartDisplay_ && high == selectionEndDisplay_) {
        return;
    }
    selectionStartDisplay_ = low;
    selectionEndDisplay_ = high;
    selectionActive_ = high > low;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Application::OnMouseLeave() {
    mouseInside_ = false;
    mouseTracking_ = false;
    if (hoverIndex_ != -1) {
        hoverIndex_ = -1;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void Application::OnLeftButtonDown(int xPixels, int yPixels) {
    const float scale = renderer_.DpiScale();
    const float x = static_cast<float>(xPixels) / scale;
    const float y = static_cast<float>(yPixels) / scale;

    const HitRegion* hit = HitTest(x, y);
    // 快捷键映射窗口位于最上层。
    if (shortcutSettingsVisible_) {
        if (!PointInside(ShortcutPanelRect(), x, y)) {
            CloseShortcutSettings();
            return;
        }
        if (hit != nullptr) {
            if (hit->kind == HitKind::ShortcutRow && hit->index >= 0 &&
                hit->index < static_cast<int>(kShortcutRowCount)) {
                shortcutCaptureIndex_ = hit->index;
                InvalidateRect(hwnd_, nullptr, FALSE);
                UpdateWindow(hwnd_);
            } else if (hit->kind == HitKind::ShortcutClose) {
                CloseShortcutSettings();
            } else if (hit->kind == HitKind::ShortcutReset) {
                ResetShortcuts();
            }
        }
        return;
    }
    // 对话记录弹框位于最上层：点击弹框外关闭，弹框内只响应自身控件。
    if (chatSessionPickerVisible_) {
        if (!PointInside(ChatSessionPickerPanelRect(), x, y)) {
            chatSessionPickerVisible_ = false;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        if (hit != nullptr && hit->kind == HitKind::ChatSessionItem) {
            SelectChatSession(hit->value);
        } else if (hit != nullptr && hit->kind == HitKind::ChatSessionDelete) {
            DeleteChatSession(hit->value);
        } else if (hit != nullptr && hit->kind == HitKind::ChatSessionClose) {
            chatSessionPickerVisible_ = false;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return;
    }
    // 字体选择弹框位于最上层：点击弹框外关闭，弹框内只响应自身控件。
    if (fontPickerVisible_) {
        if (!PointInside(FontPickerPanelRect(), x, y)) {
            fontPickerVisible_ = false;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        if (hit != nullptr && hit->kind == HitKind::FontPickerItem) {
            SelectFontFamily(hit->value);
        } else if (hit != nullptr && hit->kind == HitKind::FontPickerClose) {
            fontPickerVisible_ = false;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return;
    }
    if (aiSettingsVisible_) {
        // 点击弹框外部关闭弹框；弹框内空白区域与控件分别处理。
        if (!PointInside(AiSettingsPanelRect
              (), x, y)) {
            CloseAiSettings();
            return;
        }
        const bool insideSettings =
            hit && (hit->kind == HitKind::ChatBaseUrl || hit->kind == HitKind::ChatApiKey ||
                    hit->kind == HitKind::ChatModel ||
                    hit->kind == HitKind::ChatProtocolOpenAi ||
                    hit->kind == HitKind::ChatProtocolAnthropic ||
                    hit->kind == HitKind::AiThinkingLevel ||
                    hit->kind == HitKind::AiSettingsSave ||
                    hit->kind == HitKind::AiSettingsClose);
        if (!insideSettings) {
            return;
        }
    }
    // 弹出菜单位于普通界面之上：点击菜单项执行，点击外部收起。
    if (menuVisible_) {
        const D2D1_RECT_F menuRect = MenuRect();
        if (!PointInside(menuRect, x, y)) {
            CloseMenu();
            return;
        }
        if (hit != nullptr && hit->kind == HitKind::MenuItem && hit->index >= 0 &&
            hit->index < static_cast<int>(menuEntries_.size())) {
            const MenuEntry entry = menuEntries_[static_cast<size_t>(hit->index)];
            CloseMenu();
            ExecuteMenuEntry(entry);
        }
        return;
    }
    // 点击对话区以外位置时取消对话内的文字选择。
    if (panel_ == PanelMode::Chat && !PointInside(ChatConversationRect(), x, y)) {
        ClearChatSelection();
    }
    if (view_ == ViewMode::Shelf && shelfSearchFocused_ &&
        (!hit || hit->kind != HitKind::ShelfSearchBox)) {
        // 点击书架其他位置时，搜索框失去焦点。
        shelfSearchFocused_ = false;
        StopCaretBlink();
    }
    if (!hit) {
        if (view_ == ViewMode::Reader) {
            // 对话内容区：命中消息文字时开始选择，命中空白则清除选择。
            if (panel_ == PanelMode::Chat && PointInside(ChatConversationRect(), x, y)) {
                BeginChatSelection(x, y);
                return;
            }
            float textWidth = 0.0f;
            float textHeight = 0.0f;
            D2D1_RECT_F textRect{};
            ComputeTextMetrics(textWidth, textHeight, textRect);
            if (PointInside(textRect, x, y)) {
                if (gesture_ == Gesture::EraseHighlight || gesture_ == Gesture::EraseNote) {
                    const bool wantNote = gesture_ == Gesture::EraseNote;
                    for (auto it = annotationHits_.rbegin(); it != annotationHits_.rend(); ++it) {
                        if (it->isNote != wantNote) {
                            continue;
                        }
                        if (PointInside(it->rect, x, y)) {
                            if (wantNote) {
                                DeleteNote(it->index);
                            } else {
                                DeleteHighlight(it->index);
                            }
                            return;
                        }
                    }
                    ShowStatus(wantNote ? L"该位置没有标注" : L"该位置没有高亮");
                    return;
                }
                BeginSelection(x, y);
                return;
            }
        }
        if (panel_ != PanelMode::None) {
            // 点击右侧面板内部的空白区域不应关闭面板，只有面板外的点击才收起。
            const bool insidePanel = x >= widthDip_ - PanelWidth() && y >= kReaderToolbarHeight;
            if (!insidePanel) {
                ChangePanel(PanelMode::None);
            }
        }
        return;
    }
    const HitRegion action = *hit;

    // 书架设置浮层打开时优先处理浮层内的操作，其余点击一律关闭浮层。
    if (!shelfCategoryPickerBookId_.empty()) {
        if (action.kind == HitKind::CategoryPickerItem) {
            std::wstring error;
            library_.SetBookCategory(shelfCategoryPickerBookId_, action.value, error);
            shelfCategoryPickerBookId_.clear();
            RefreshShelfOrder();
            InvalidateRect(hwnd_, nullptr, FALSE);
        } else {
            shelfCategoryPickerBookId_.clear();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return;
    }

    if (categoryManagerVisible_) {
        switch (action.kind) {
        case HitKind::CategoryNameInput:
            categoryNameFocused_ = true;
            StartCaretBlink();
            BeginInputSelection(x, y);
            return;
        case HitKind::CategoryCreate:
            CreateCategoryFromDraft();
            return;
        case HitKind::CategoryRename:
            RenameSelectedCategory();
            return;
        case HitKind::CategoryDelete:
            DeleteSelectedCategory();
            return;
        default:
            categoryManagerVisible_ = false;
            categoryNameFocused_ = false;
            StopCaretBlink();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
    }

    if (shelfSettingsVisible_) {
        if (action.kind == HitKind::ToggleUmdAssociation) {
            if (IsFileExtensionAssociated(L".umd", kDocumentProgId)) {
                DisassociateFileExtension(L".umd", kDocumentProgId);
            } else {
                AssociateFileExtension(L".umd", kDocumentProgId, L"UMD 电子书");
            }
            return;
        }
        if (action.kind == HitKind::ToggleTxtAssociation) {
            if (IsFileExtensionAssociated(L".txt", kTextProgId)) {
                DisassociateFileExtension(L".txt", kTextProgId);
            } else {
                AssociateFileExtension(L".txt", kTextProgId, L"文本电子书");
            }
            return;
        }
        if (action.kind == HitKind::OpenAiSettings) {
            // AI 接口设置作为更上层弹框打开，全局设置窗口保持可见。
            OpenAiSettings();
            return;
        }
        if (action.kind == HitKind::OpenShortcutSettings) {
            // 快捷键设置作为更上层弹框打开，全局设置窗口保持可见。
            OpenShortcutSettings();
            return;
        }
        if (action.kind == HitKind::OpenDataFolder) {
            const std::wstring directory = library_.DataDirectory().wstring();
            ShellExecuteW(hwnd_, L"open", directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return;
        }
        if (!aiSettingsVisible_) {
            shelfSettingsVisible_ = false;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        // AI 设置弹框处于最上层时，其控件继续交给下方的通用分发处理，
        // 全局设置窗口保持打开，关闭 AI 设置后回到全局设置。
    }

    switch (action.kind) {
    case HitKind::ShelfSearchBox:
        shelfSearchFocused_ = true;
        categoryNameFocused_ = false;
        StartCaretBlink();
        UpdateImePosition();
        BeginInputSelection(x, y);
        break;
    case HitKind::CategoryAll:
    case HitKind::CategoryUncategorized:
    case HitKind::CategoryItem:
        shelfFilterCategory_ =
            action.kind == HitKind::CategoryUncategorized
                ? std::wstring(kUncategorizedFilter)
                : (action.kind == HitKind::CategoryAll ? std::wstring() : action.value);
        RefreshShelfOrder();
        shelfScroll_ = 0.0f;
        InvalidateRect(hwnd_, nullptr, FALSE);
        break;
    case HitKind::BookCategoryButton:
        shelfCategoryPickerBookId_ = action.value;
        InvalidateRect(hwnd_, nullptr, FALSE);
        break;
    case HitKind::ShelfSettings:
        shelfSettingsVisible_ = true;
        categoryManagerVisible_ = false;
        InvalidateRect(hwnd_, nullptr, FALSE);
        break;
    case HitKind::CategoryManager:
        categoryManagerVisible_ = true;
        shelfSettingsVisible_ = false;
        InvalidateRect(hwnd_, nullptr, FALSE);
        break;
    case HitKind::AddBook:
        OpenFileDialog();
        break;
    case HitKind::OpenDataFolder: {
        const std::wstring directory = library_.DataDirectory().wstring();
        ShellExecuteW(hwnd_, L"open", directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        break;
    }
    case HitKind::BookCard:
        OpenBook(action.value);
        break;
    case HitKind::RemoveBook: {
        const data::BookRecord* record = library_.Find(action.value);
        if (!record) {
            break;
        }
        const std::wstring message =
            L"要从书架移除《" + record->title + L"》吗？\n原 UMD 文件不会被删除。";
        if (MessageBoxW(hwnd_, message.c_str(), L"移除书籍", MB_YESNO | MB_ICONQUESTION) == IDYES) {
            std::wstring error;
            if (library_.RemoveBook(action.value, error)) {
                coverCache_.erase(action.value);
                if (activeBookId_ == action.value) {
                    CloseBook();
                }
                RefreshShelfOrder();
                ShowStatus(L"已从书架移除该书");
            } else {
                ShowStatus(error);
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        break;
    }
    case HitKind::BackToShelf:
        CloseBook();
        break;
    case HitKind::OpenToc:
        ChangePanel(panel_ == PanelMode::Toc ? PanelMode::None : PanelMode::Toc);
        break;
    case HitKind::OpenBookmarks:
        ChangePanel(panel_ == PanelMode::Bookmarks ? PanelMode::None : PanelMode::Bookmarks);
        break;
    case HitKind::OpenSearch:
        ChangePanel(panel_ == PanelMode::Search ? PanelMode::None : PanelMode::Search);
        break;
    case HitKind::OpenSettings:
        ChangePanel(panel_ == PanelMode::Settings ? PanelMode::None : PanelMode::Settings);
        break;
    case HitKind::OpenImages:
        ChangePanel(panel_ == PanelMode::Images ? PanelMode::None : PanelMode::Images);
        break;
    case HitKind::ClosePanel:
        ChangePanel(PanelMode::None);
        break;
    case HitKind::ChapterItem: {
        if (action.index < 0 || action.index >= static_cast<int>(activeBook_.chapters.size())) {
            break;
        }
        const uint32_t offset = activeBook_.chapters[static_cast<size_t>(action.index)].charOffset;
        GoToGlobalOffset(offset);
        break;
    }
    case HitKind::BookmarkItem: {
        data::BookRecord* record = library_.Find(activeBookId_);
        if (!record || action.index < 0 ||
            action.index >= static_cast<int>(record->bookmarks.size())) {
            break;
        }
        const data::Bookmark& bookmark = record->bookmarks[static_cast<size_t>(action.index)];
        if (bookmark.chapterIndex < 0 ||
            bookmark.chapterIndex >= static_cast<int>(activeBook_.chapters.size())) {
            ShowStatus(L"书签指向的章节已不存在");
            break;
        }
        const uint32_t offset = activeBook_.chapters[static_cast<size_t>(bookmark.chapterIndex)]
                                    .charOffset +
                                bookmark.charOffset;
        searchHighlight_.clear();
        GoToGlobalOffset(offset);
        break;
    }
    case HitKind::AddBookmark:
        AddBookmark();
        break;
    case HitKind::RemoveBookmark:
        RemoveBookmark(action.index);
        break;
    case HitKind::SearchItem: {
        if (action.index < 0 || action.index >= static_cast<int>(searchResults_.size())) {
            break;
        }
        const SearchResult& result = searchResults_[static_cast<size_t>(action.index)];
        if (result.chapterIndex < 0 ||
            result.chapterIndex >= static_cast<int>(activeBook_.chapters.size())) {
            break;
        }
        const uint32_t offset =
            activeBook_.chapters[static_cast<size_t>(result.chapterIndex)].charOffset +
            result.charOffset;
        searchHighlight_ = searchQuery_;
        GoToGlobalOffset(offset);
        break;
    }
    case HitKind::SearchBox:
        searchFocused_ = true;
        StartCaretBlink();
        UpdateImePosition();
        BeginInputSelection(x, y);
        break;
    case HitKind::FontPickerOpen:
        fontPickerVisible_ = true;
        fontPickerScroll_ = 0.0f;
        InvalidateRect(hwnd_, nullptr, FALSE);
        // 弹框控件需要立即可用，先同步建立命中区域。
        UpdateWindow(hwnd_);
        break;
    case HitKind::FontPickerItem:
        SelectFontFamily(action.value);
        break;
    case HitKind::FontPickerClose:
        fontPickerVisible_ = false;
        InvalidateRect(hwnd_, nullptr, FALSE);
        UpdateWindow(hwnd_);
        break;
    case HitKind::FontSmaller: {
        data::ReaderSettings& settings = library_.Settings();
        settings.fontSize = std::clamp(settings.fontSize - 2.0f, 14.0f, 34.0f);
        ApplySettings();
        break;
    }
    case HitKind::FontLarger: {
        data::ReaderSettings& settings = library_.Settings();
        settings.fontSize = std::clamp(settings.fontSize + 2.0f, 14.0f, 34.0f);
        ApplySettings();
        break;
    }
    case HitKind::SpacingSmaller: {
        data::ReaderSettings& settings = library_.Settings();
        settings.lineSpacing = std::clamp(settings.lineSpacing - 0.1f, 1.2f, 2.4f);
        ApplySettings();
        break;
    }
    case HitKind::SpacingLarger: {
        data::ReaderSettings& settings = library_.Settings();
        settings.lineSpacing = std::clamp(settings.lineSpacing + 0.1f, 1.2f, 2.4f);
        ApplySettings();
        break;
    }
    case HitKind::ThemeOption: {
        data::ReaderSettings& settings = library_.Settings();
        settings.theme = action.value;
        std::wstring error;
        library_.Save(error);
        InvalidateRect(hwnd_, nullptr, FALSE);
        break;
    }
    case HitKind::BackgroundTheme:
        SelectBackground(L"theme", std::wstring());
        break;
    case HitKind::BackgroundColor:
        SelectBackground(L"color", action.value);
        break;
    case HitKind::BackgroundImage:
        SelectBackground(L"image", action.value);
        break;
    case HitKind::BackgroundRemove: {
        const std::wstring name = action.value;
        // 先释放该图片的缓存位图，避免解码器仍占用文件而无法删除。
        backgroundThumbCache_.erase(name);
        if (backgroundBitmapName_ == name) {
            backgroundBitmap_.Reset();
            backgroundBitmapName_.clear();
        }
        std::wstring error;
        if (!library_.RemoveBackgroundImage(name, error)) {
            ShowStatus(error);
            InvalidateRect(hwnd_, nullptr, FALSE);
            break;
        }
        data::ReaderSettings& settings = library_.Settings();
        if (settings.backgroundType == L"image" && settings.backgroundImage == name) {
            settings.backgroundType = L"theme";
            settings.backgroundImage.clear();
        }
        RefreshBackgroundList();
        std::wstring saveError;
        library_.Save(saveError);
        ShowStatus(L"已删除背景图片");
        InvalidateRect(hwnd_, nullptr, FALSE);
        // 列表发生变化后立即重绘，避免下一次点击命中已删除的缩略图区域。
        UpdateWindow(hwnd_);
        break;
    }
    case HitKind::BackgroundImport:
        ImportBackgroundImages();
        break;
    case HitKind::ScrollModeLine:
    case HitKind::ScrollModePage: {
        data::ReaderSettings& settings = library_.Settings();
        settings.readerScrollMode =
            action.kind == HitKind::ScrollModeLine ? std::wstring(L"line") : std::wstring(L"page");
        std::wstring error;
        library_.Save(error);
        InvalidatePageCache();
        if (LineScrollMode()) {
            AlignLineOffsetToCurrentPage();
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        break;
    }
    case HitKind::ReaderScrollbar:
        scrollDragging_ = true;
        lastScrollRatio_ = ScrollRatioFromY(y);
        SetCapture(hwnd_);
        GoToScrollRatio(lastScrollRatio_);
        break;
    case HitKind::ToggleSpeech:
        ToggleSpeech();
        break;
    case HitKind::OpenChat:
        ChangePanel(panel_ == PanelMode::Chat ? PanelMode::None : PanelMode::Chat);
        break;
    case HitKind::ChatBaseUrl:
    case HitKind::ChatApiKey:
    case HitKind::ChatModel:
        chatFocus_ = action.kind == HitKind::ChatBaseUrl
                         ? ChatFocus::BaseUrl
                         : (action.kind == HitKind::ChatApiKey ? ChatFocus::ApiKey
                                                               : ChatFocus::Model);
        StartCaretBlink();
        UpdateImePosition();
        BeginInputSelection(x, y);
        break;
    case HitKind::ChatProtocolOpenAi:
    case HitKind::ChatProtocolAnthropic: {
        data::ReaderSettings& settings = library_.Settings();
        const bool toAnthropic = action.kind == HitKind::ChatProtocolAnthropic;
        const std::wstring openAiBase = L"https://api.openai.com/v1";
        const std::wstring anthropicBase = L"https://api.anthropic.com/v1";
        const std::wstring openAiModel = L"gpt-4o-mini";
        const std::wstring anthropicModel = L"claude-3-5-sonnet-20241022";
        // 若当前地址或模型仍是另一协议的默认值，则一并替换，避免切换后请求必然失败。
        settings.aiBaseUrl = toAnthropic ? anthropicBase : openAiBase;
        settings.aiModel = toAnthropic ? anthropicModel : openAiModel;
        settings.aiProtocol = toAnthropic ? std::wstring(L"anthropic") : std::wstring(L"openai");
        std::wstring error;
        library_.Save(error);
        UpdateImePosition();
        InvalidateRect(hwnd_, nullptr, FALSE);
        break;
    }
    case HitKind::AiThinkingLevel:
        // 思考等级即时生效，随「保存」写入设置。
        library_.Settings().aiThinkingLevel = std::clamp(action.index, 0, 3);
        InvalidateRect(hwnd_, nullptr, FALSE);
        break;
    case HitKind::ChatInput:
        chatFocus_ = ChatFocus::Message;
        StartCaretBlink();
        UpdateImePosition();
        BeginInputSelection(x, y);
        break;
    case HitKind::ChatSend:
        SendAiMessage();
        break;
    case HitKind::ChatStop:
        StopAiRequest();
        break;
    case HitKind::ChatPanelSplitter:
        chatPanelResizing_ = true;
        SetCapture(hwnd_);
        break;
    case HitKind::ChatScrollbar:
        chatScrollDragging_ = true;
        SetCapture(hwnd_);
        ScrollChatFromY(y);
        break;
    case HitKind::PanelScrollbar:
        panelScrollDragging_ = true;
        SetCapture(hwnd_);
        ScrollPanelFromY(y);
        break;
    case HitKind::OpenAiSettings:
        OpenAiSettings();
        break;
    case HitKind::AiSettingsSave:
        SaveAiSettings();
        break;
    case HitKind::AiSettingsClose:
        CloseAiSettings();
        break;
    case HitKind::ChatSessionOpen:
        chatSessionPickerVisible_ = true;
        chatSessionPickerScroll_ = 0.0f;
        InvalidateRect(hwnd_, nullptr, FALSE);
        // 弹框控件需要立即可用，先同步建立命中区域。
        UpdateWindow(hwnd_);
        break;
    case HitKind::ChatSessionNew:
        NewChatSession();
        break;
    case HitKind::ChatCopyMessage:
        CopyChatMessage(action.index);
        break;
    case HitKind::ChatThinkingToggle: {
        if (chatThinkingExpanded_.count(action.index) > 0) {
            chatThinkingExpanded_.erase(action.index);
        } else {
            chatThinkingExpanded_.insert(action.index);
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        // 折叠状态会改变气泡高度，立即重绘以同步命中区域。
        UpdateWindow(hwnd_);
        break;
    }
    case HitKind::OpenGestureBar: {
        const uint32_t offset = CurrentGlobalOffset();
        panel_ = PanelMode::None;
        panelScroll_ = 0.0f;
        searchFocused_ = false;
        noteFocused_ = false;
        StopCaretBlink();
        ClearSelection();
        gestureBarVisible_ = !gestureBarVisible_;
        gesture_ = Gesture::Default;
        colorPickerVisible_ = false;
        if (!activeBook_.content.empty()) {
            RebuildPagination(offset);
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        break;
    }
    case HitKind::GestureOption:
        if (action.index >= 0 && action.index <= static_cast<int>(Gesture::Speech)) {
            gesture_ = static_cast<Gesture>(action.index);
        }
        colorPickerVisible_ = false;
        ClearSelection();
        InvalidateRect(hwnd_, nullptr, FALSE);
        break;
    case HitKind::HighlightColor:
        ApplyHighlight(action.value);
        break;
    case HitKind::NoteInput:
        noteFocused_ = true;
        StartCaretBlink();
        UpdateImePosition();
        BeginInputSelection(x, y);
        break;
    case HitKind::NoteSave:
        SaveNote();
        break;
    case HitKind::NoteCancel:
        CancelNoteEditor();
        break;
    default:
        break;
    }
}

void Application::OnMouseWheel(int xPixels, int yPixels, int delta) {
    const float scale = renderer_.DpiScale();
    const float x = static_cast<float>(xPixels) / scale;
    const float y = static_cast<float>(yPixels) / scale;
    const float steps = static_cast<float>(delta) / 120.0f;

    // 弹出菜单打开时，滚轮不滚动底层页面。
    if (menuVisible_) {
        CloseMenu();
        return;
    }
    // 快捷键映射窗口打开时，滚轮不滚动底层页面。
    if (shortcutSettingsVisible_) {
        return;
    }

    // 对话记录弹框打开时，滚轮只滚动会话列表。
    if (chatSessionPickerVisible_) {
        if (PointInside(ChatSessionPickerPanelRect(), x, y)) {
            chatSessionPickerScroll_ = std::clamp(chatSessionPickerScroll_ - steps * 64.0f, 0.0f,
                                                 chatSessionPickerScrollMax_);
            InvalidateRect(hwnd_, nullptr, FALSE);
            UpdateWindow(hwnd_);
        }
        return;
    }

    // 字体选择弹框打开时，滚轮只滚动字体列表，不影响底层页面。
    if (fontPickerVisible_) {
        if (PointInside(FontPickerPanelRect(), x, y)) {
            fontPickerScroll_ =
                std::clamp(fontPickerScroll_ - steps * 72.0f, 0.0f, fontPickerScrollMax_);
            InvalidateRect(hwnd_, nullptr, FALSE);
            UpdateWindow(hwnd_);
        }
        return;
    }

    // 对话内容区优先滚动：指针位于对话区域时滚动消息列表，而不是整个侧栏。
    if (panel_ == PanelMode::Chat && PointInside(ChatConversationRect(), x, y)) {
        chatScroll_ = std::clamp(chatScroll_ - steps * 60.0f, 0.0f, chatScrollMax_);
        InvalidateRect(hwnd_, nullptr, FALSE);
        // 消息位置变化后立即重绘，保证“复制”按钮的命中区域同步。
        UpdateWindow(hwnd_);
        return;
    }

    if (panel_ != PanelMode::None && x >= widthDip_ - PanelWidth() && y >= kReaderToolbarHeight) {
        panelScroll_ = std::clamp(panelScroll_ - steps * 72.0f, 0.0f, panelScrollMax_);
        InvalidateRect(hwnd_, nullptr, FALSE);
        // 滚动后控件位置变化，立即重绘以同步命中区域，避免点击落到滚动前的位置。
        UpdateWindow(hwnd_);
        return;
    }

    if (view_ == ViewMode::Reader) {
        if (LineScrollMode()) {
            // 按行滚动：每格滚轮移动 3 行。
            constexpr int kLinesPerWheel = 3;
            if (steps < 0.0f) {
                ScrollReadingLines(1, kLinesPerWheel);
            } else if (steps > 0.0f) {
                ScrollReadingLines(-1, kLinesPerWheel);
            }
        } else if (steps < 0.0f) {
            NextPage();
        } else if (steps > 0.0f) {
            PreviousPage();
        }
        return;
    }

    shelfScroll_ = std::clamp(shelfScroll_ - steps * 88.0f, 0.0f, shelfScrollMax_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Application::OnLeftButtonUp(int xPixels, int yPixels) {
    if (chatSelecting_) {
        chatSelecting_ = false;
        if (GetCapture() == hwnd_) {
            ReleaseCapture();
        }
        // 空选区视为取消选择。
        if (chatSelectionStart_ == chatSelectionEnd_) {
            ClearChatSelection();
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (inputSelecting_) {
        inputSelecting_ = false;
        if (GetCapture() == hwnd_) {
            ReleaseCapture();
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (scrollDragging_) {
        scrollDragging_ = false;
        if (GetCapture() == hwnd_) {
            ReleaseCapture();
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (chatPanelResizing_) {
        chatPanelResizing_ = false;
        if (GetCapture() == hwnd_) {
            ReleaseCapture();
        }
        // 面板宽度决定正文可用宽度，拖动结束后统一重排一次。
        ClearLayoutCache();
        InvalidatePageCache();
        RebuildPagination(CurrentGlobalOffset());
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (chatScrollDragging_) {
        chatScrollDragging_ = false;
        if (GetCapture() == hwnd_) {
            ReleaseCapture();
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (panelScrollDragging_) {
        panelScrollDragging_ = false;
        if (GetCapture() == hwnd_) {
            ReleaseCapture();
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (!selecting_) {
        return;
    }
    selecting_ = false;
    if (GetCapture() == hwnd_) {
        ReleaseCapture();
    }
    const float scale = renderer_.DpiScale();
    UpdateSelection(static_cast<float>(xPixels) / scale, static_cast<float>(yPixels) / scale);
    if (!selectionActive_) {
        ClearSelection();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (gesture_ == Gesture::Highlight) {
        colorPickerVisible_ = true;
        ShowStatus(L"选择一种高亮颜色");
    } else if (gesture_ == Gesture::Note) {
        OpenNoteEditor();
        return;
    } else if (gesture_ == Gesture::Copy) {
        // 复制手势：选中结束即写入剪贴板，并显示底部居中提示
        CopySelection();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    } else if (gesture_ == Gesture::Speech) {
        // 朗读手势：选中结束即朗读所选文本；未选中时不朗读整页。
        if (selectionActive_) {
            if (speaking_) {
                StopSpeech();
            }
            ToggleSpeech();
            return;
        }
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

bool Application::OnLeftButtonDoubleClick(int xPixels, int yPixels) {
    const float scale = renderer_.DpiScale();
    const float x = static_cast<float>(xPixels) / scale;
    const float y = static_cast<float>(yPixels) / scale;
    if (view_ != ViewMode::Reader || activeBook_.content.empty() || aiSettingsVisible_ ||
        fontPickerVisible_ || chatSessionPickerVisible_) {
        return false;
    }
    // 只响应正文阅读区域，右侧面板与工具栏不在该矩形内。
    float textWidth = 0.0f;
    float textHeight = 0.0f;
    D2D1_RECT_F textRect{};
    ComputeTextMetrics(textWidth, textHeight, textRect);
    if (!PointInside(textRect, x, y)) {
        return false;
    }
    // 阅读区域纵向分为上、中、下三块，仅中间一块响应双击。
    const float third = (textRect.bottom - textRect.top) / 3.0f;
    if (y < textRect.top + third || y > textRect.bottom - third) {
        return false;
    }
    if (panel_ == PanelMode::Settings) {
        return false;
    }
    ChangePanel(PanelMode::Settings);
    return true;
}

void Application::BeginSelection(float x, float y) {
    if (!pageLayoutValid_ && !EnsurePageLayout()) {
        return;
    }
    const uint32_t offset = DisplayOffsetAt(x, y);
    selecting_ = true;
    selectionAnchorDisplay_ = offset;
    selectionStartDisplay_ = offset;
    selectionEndDisplay_ = offset;
    selectionActive_ = false;
    colorPickerVisible_ = false;
    SetCapture(hwnd_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Application::ClearSelection() {
    selecting_ = false;
    selectionActive_ = false;
    selectionAnchorDisplay_ = 0;
    selectionStartDisplay_ = 0;
    selectionEndDisplay_ = 0;
    if (GetCapture() == hwnd_) {
        ReleaseCapture();
    }
}

void Application::CopySelection() {
    if (!selectionActive_ || activeChapterRaw_.empty()) {
        return;
    }
    const uint32_t startOriginal = chapterDisplay_.ToOriginal(selectionStartDisplay_);
    const uint32_t endOriginal = chapterDisplay_.ToOriginal(selectionEndDisplay_);
    if (endOriginal <= startOriginal) {
        return;
    }
    const uint32_t limitedEnd =
        std::min(endOriginal, static_cast<uint32_t>(activeChapterRaw_.size()));
    if (limitedEnd <= startOriginal) {
        return;
    }
    const std::wstring text = activeChapterRaw_.substr(startOriginal, limitedEnd - startOriginal);
    if (text.empty()) {
        return;
    }
    if (CopyTextToClipboard(text)) {
        ShowCopyToast();
    } else {
        ShowStatus(L"复制到剪贴板失败");
    }
}

void Application::ShowCopyToast() {
    copyToastVisible_ = true;
    copyToastExpiry_ = util::CurrentUnixSeconds() + kCopyToastSeconds;
    SetTimer(hwnd_, kCopyToastTimerId, 500, nullptr);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Application::StartCaretBlink() {
    caretBlinkVisible_ = true;
    SetTimer(hwnd_, kCaretTimerId, kCaretBlinkIntervalMs, nullptr);
}

void Application::StopCaretBlink() {
    KillTimer(hwnd_, kCaretTimerId);
    caretBlinkVisible_ = false;
}

D2D1_RECT_F Application::ReaderPanelContentRect() const {
    const float panelLeft = widthDip_ - PanelWidth();
    return D2D1::RectF(panelLeft + 20.0f, kReaderToolbarHeight + kPanelHeaderHeight,
                       widthDip_ - 20.0f, heightDip_ - 20.0f);
}

float Application::PanelWidth() const {
    if (panel_ != PanelMode::Chat) {
        return kPanelWidth;
    }
    const float maximum = std::max(300.0f, std::min(760.0f, widthDip_ - 420.0f));
    return std::clamp(chatPanelWidth_, 300.0f, maximum);
}

D2D1_RECT_F Application::SearchBoxRect() const {
    const D2D1_RECT_F contentRect = ReaderPanelContentRect();
    return D2D1::RectF(contentRect.left, contentRect.top, contentRect.right,
                       contentRect.top + kSearchBoxHeight);
}

D2D1_RECT_F Application::NoteInputBoxRect() const {
    const D2D1_RECT_F contentRect = ReaderPanelContentRect();
    // 与 DrawNoteEditorPanel 的纵向排布一致：标题 24 + 预览 48 + 标题 24。
    const float top = contentRect.top + 96.0f;
    return D2D1::RectF(contentRect.left, top, contentRect.right, top + kNoteInputHeight);
}

D2D1_RECT_F Application::ChatFieldRect(ChatFocus field) const {
    const D2D1_RECT_F contentRect = ReaderPanelContentRect();
    if (field != ChatFocus::Message) {
        return D2D1::RectF();
    }
    // 多行输入框横跨整个面板宽度，发送按钮放在其下方的底部。
    return D2D1::RectF(contentRect.left, contentRect.bottom - 104.0f, contentRect.right,
                       contentRect.bottom - 44.0f);
}

D2D1_RECT_F Application::AiChatEntryRect() const {
    const float panelReserve = panel_ != PanelMode::None ? PanelWidth() : 0.0f;
    const float availableWidth = widthDip_ - panelReserve;
    constexpr float kEntryWidth = 112.0f;
    constexpr float kEntryHeight = 30.0f;
    const float centerX = availableWidth * 0.5f;
    const float bottom = heightDip_ - 7.0f;
    return D2D1::RectF(centerX - kEntryWidth * 0.5f, bottom - kEntryHeight,
                       centerX + kEntryWidth * 0.5f, bottom);
}

D2D1_RECT_F Application::AiSettingsPanelRect() const {
    constexpr float kSettingsWidth = 460.0f;
    constexpr float kSettingsHeight = 460.0f;
    const float left = (widthDip_ - kSettingsWidth) * 0.5f;
    const float top = (heightDip_ - kSettingsHeight) * 0.5f;
    return D2D1::RectF(left, top, left + kSettingsWidth, top + kSettingsHeight);
}

D2D1_RECT_F Application::AiSettingsFieldRect(ChatFocus field) const {
    const D2D1_RECT_F panel = AiSettingsPanelRect();
    float top = 0.0f;
    switch (field) {
    case ChatFocus::BaseUrl:
        top = panel.top + 160.0f;
        break;
    case ChatFocus::ApiKey:
        top = panel.top + 222.0f;
        break;
    case ChatFocus::Model:
        top = panel.top + 284.0f;
        break;
    default:
        return D2D1::RectF();
    }
    return D2D1::RectF(panel.left + 24.0f, top, panel.right - 24.0f, top + 34.0f);
}

D2D1_RECT_F Application::ChatConversationRect() const {
    const D2D1_RECT_F contentRect = ReaderPanelContentRect();
    // 顶部为接口信息行与会话栏，对话内容区从其下方开始。
    return D2D1::RectF(contentRect.left, contentRect.top + 74.0f, contentRect.right,
                       contentRect.bottom - 148.0f);
}

D2D1_RECT_F Application::ChatScrollbarTrackRect() const {
    const D2D1_RECT_F rect = ChatConversationRect();
    constexpr float kTrackWidth = 8.0f;
    return D2D1::RectF(rect.right - kTrackWidth - 2.0f, rect.top + 2.0f, rect.right - 2.0f,
                       rect.bottom - 2.0f);
}

void Application::ScrollChatFromY(float y) {
    if (chatScrollMax_ <= 0.0f) {
        return;
    }
    const D2D1_RECT_F track = ChatScrollbarTrackRect();
    const float trackHeight = track.bottom - track.top;
    const float contentHeight = trackHeight + chatScrollMax_;
    const float thumbHeight = std::max(28.0f, trackHeight * trackHeight / contentHeight);
    const float usable = std::max(1.0f, trackHeight - thumbHeight);
    const float ratio = std::clamp((y - track.top - thumbHeight * 0.5f) / usable, 0.0f, 1.0f);
    chatScroll_ = ratio * chatScrollMax_;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

D2D1_RECT_F Application::PanelScrollbarTrackRect() const {
    const D2D1_RECT_F contentRect = ReaderPanelContentRect();
    float top = contentRect.top;
    // 书签列表位于「添加当前页书签」按钮行下方。
    if (panel_ == PanelMode::Bookmarks) {
        top += 48.0f;
    }
    constexpr float kTrackWidth = 8.0f;
    return D2D1::RectF(contentRect.right - kTrackWidth, top + 2.0f, contentRect.right,
                       contentRect.bottom - 2.0f);
}

void Application::DrawPanelScrollbar(const ThemeColors& colors) {
    if ((panel_ != PanelMode::Toc && panel_ != PanelMode::Bookmarks) || panelScrollMax_ <= 0.0f) {
        return;
    }
    const D2D1_RECT_F track = PanelScrollbarTrackRect();
    renderer_.FillRoundedRect(track, 4.0f, colors.divider);
    const float trackHeight = track.bottom - track.top;
    const float contentHeight = trackHeight + panelScrollMax_;
    const float thumbHeight = std::max(28.0f, trackHeight * trackHeight / contentHeight);
    const float ratio = panelScroll_ / panelScrollMax_;
    const float thumbTop = track.top + (trackHeight - thumbHeight) * ratio;
    renderer_.FillRoundedRect(
        D2D1::RectF(track.left, thumbTop, track.right, thumbTop + thumbHeight), 4.0f,
        colors.accent);
    AddHit(D2D1::RectF(track.left - 2.0f, track.top, track.right + 2.0f, track.bottom),
           HitKind::PanelScrollbar);
}

void Application::ScrollPanelFromY(float y) {
    if (panelScrollMax_ <= 0.0f) {
        return;
    }
    const D2D1_RECT_F track = PanelScrollbarTrackRect();
    const float trackHeight = track.bottom - track.top;
    const float contentHeight = trackHeight + panelScrollMax_;
    const float thumbHeight = std::max(28.0f, trackHeight * trackHeight / contentHeight);
    const float usable = std::max(1.0f, trackHeight - thumbHeight);
    const float ratio = std::clamp((y - track.top - thumbHeight * 0.5f) / usable, 0.0f, 1.0f);
    panelScroll_ = ratio * panelScrollMax_;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

D2D1_RECT_F Application::ChatStatusRect() const {
    const D2D1_RECT_F contentRect = ReaderPanelContentRect();
    return D2D1::RectF(contentRect.left, contentRect.bottom - 140.0f, contentRect.right - 96.0f,
                       contentRect.bottom - 112.0f);
}

D2D1_RECT_F Application::ChatSendRect() const {
    const D2D1_RECT_F contentRect = ReaderPanelContentRect();
    return D2D1::RectF(contentRect.right - 88.0f, contentRect.bottom - 38.0f, contentRect.right,
                       contentRect.bottom - 4.0f);
}

D2D1_RECT_F Application::ChatStopRect() const {
    const D2D1_RECT_F contentRect = ReaderPanelContentRect();
    return D2D1::RectF(contentRect.right - 88.0f, contentRect.bottom - 140.0f,
                       contentRect.right, contentRect.bottom - 112.0f);
}

std::wstring* Application::ActiveChatField() {
    data::ReaderSettings& settings = library_.Settings();
    if (aiSettingsVisible_) {
        switch (chatFocus_) {
        case ChatFocus::BaseUrl:
            return &settings.aiBaseUrl;
        case ChatFocus::ApiKey:
            return &settings.aiApiKey;
        case ChatFocus::Model:
            return &settings.aiModel;
        default:
            return nullptr;
        }
    }
    if (panel_ != PanelMode::Chat) {
        return nullptr;
    }
    switch (chatFocus_) {
    case ChatFocus::BaseUrl:
        return &settings.aiBaseUrl;
    case ChatFocus::ApiKey:
        return &settings.aiApiKey;
    case ChatFocus::Model:
        return &settings.aiModel;
    case ChatFocus::Message:
        return &aiDraft_;
    default:
        return nullptr;
    }
}

std::wstring* Application::ActiveInputText() {
    if (view_ == ViewMode::Shelf && shelfSearchFocused_) {
        return &shelfSearchQuery_;
    }
    if (view_ == ViewMode::Shelf && categoryManagerVisible_ && categoryNameFocused_) {
        return &categoryDraft_;
    }
    if (aiSettingsVisible_ && chatFocus_ != ChatFocus::None) {
        return ActiveChatField();
    }
    if (panel_ == PanelMode::Chat && chatFocus_ != ChatFocus::None) {
        return ActiveChatField();
    }
    if (noteFocused_ && panel_ == PanelMode::NoteEditor) {
        return &noteDraft_;
    }
    if (searchFocused_ && panel_ == PanelMode::Search) {
        return &searchQuery_;
    }
    return nullptr;
}

D2D1_RECT_F Application::ActiveInputTextRect() const {
    if (view_ == ViewMode::Shelf && shelfSearchFocused_) {
        const D2D1_RECT_F box = ShelfSearchBoxRect();
        return D2D1::RectF(box.left + 10.0f, box.top + 6.0f, box.right - 10.0f,
                           box.bottom - 6.0f);
    }
    if (view_ == ViewMode::Shelf && categoryManagerVisible_ && categoryNameFocused_) {
        const D2D1_RECT_F box = CategoryNameInputRect();
        return D2D1::RectF(box.left + 10.0f, box.top + 8.0f, box.right - 10.0f,
                           box.bottom - 8.0f);
    }
    if (aiSettingsVisible_ && chatFocus_ != ChatFocus::None) {
        const D2D1_RECT_F box = AiSettingsFieldRect(chatFocus_);
        return D2D1::RectF(box.left + 10.0f, box.top + 7.0f, box.right - 10.0f,
                           box.bottom - 7.0f);
    }
    if (panel_ == PanelMode::Chat && chatFocus_ != ChatFocus::None) {
        const D2D1_RECT_F box = ChatFieldRect(chatFocus_);
        return D2D1::RectF(box.left + 10.0f, box.top + 7.0f, box.right - 10.0f,
                           box.bottom - 7.0f);
    }
    if (noteFocused_ && panel_ == PanelMode::NoteEditor) {
        const D2D1_RECT_F box = NoteInputBoxRect();
        return D2D1::RectF(box.left + kNoteInputPaddingX, box.top + kNoteInputPaddingY,
                           box.right - kNoteInputPaddingX, box.bottom - kNoteInputPaddingY);
    }
    if (searchFocused_ && panel_ == PanelMode::Search) {
        const D2D1_RECT_F box = SearchBoxRect();
        return D2D1::RectF(box.left + kSearchTextPaddingX, box.top + kSearchTextPaddingTop,
                           box.right - kSearchTextPaddingX,
                           box.bottom - kSearchTextPaddingBottom);
    }
    return D2D1::RectF();
}

IDWriteTextFormat* Application::ActiveInputFormat() const {
    const TextFormats& formats = renderer_.Formats();
    if (view_ == ViewMode::Shelf) {
        return formats.listItem.Get();
    }
    if (noteFocused_ && panel_ == PanelMode::NoteEditor) {
        return formats.label.Get();
    }
    return formats.listItem.Get();
}

size_t Application::ActiveInputLimit() const {
    if (aiSettingsVisible_) {
        // AI 接口设置弹框在书架与阅读视图中都可打开，不受书架输入长度限制。
        return 1024;
    }
    if (view_ == ViewMode::Shelf && shelfSearchFocused_) {
        return 160;
    }
    if (view_ == ViewMode::Shelf) {
        return 40;
    }
    if (panel_ == PanelMode::Chat) {
        return 1024;
    }
    if (noteFocused_ && panel_ == PanelMode::NoteEditor) {
        return static_cast<size_t>(kMaxNoteChars);
    }
    return 64;
}

size_t Application::SelectionBegin() const {
    return std::min(inputCaret_, inputAnchor_);
}

size_t Application::SelectionEnd() const {
    return std::max(inputCaret_, inputAnchor_);
}

bool Application::HasInputSelection() const {
    return inputCaret_ != inputAnchor_;
}

size_t Application::InputCaretIndexAt(float x, float y) {
    const bool wrap = (noteFocused_ && panel_ == PanelMode::NoteEditor) ||
                      (panel_ == PanelMode::Chat && chatFocus_ == ChatFocus::Message);
    const std::wstring* source = ActiveInputText();
    std::wstring text = source != nullptr ? *source : std::wstring();
    if (aiSettingsVisible_ && chatFocus_ == ChatFocus::ApiKey) {
        // 掩码显示时，鼠标命中位置也按圆点文本换算，保证点击落点与光标一致。
        text.assign(text.size(), L'\u2022');
    }
    return CaretIndexFromPoint(text, ActiveInputFormat(), wrap, ActiveInputTextRect(),
                               renderer_.WriteFactory(), x, y);
}

void Application::ResetInputCaretToEnd() {
    const std::wstring* text = ActiveInputText();
    inputCaret_ = text != nullptr ? text->size() : 0;
    inputAnchor_ = inputCaret_;
}

void Application::InsertInputText(const std::wstring& value) {
    std::wstring* text = ActiveInputText();
    if (text == nullptr || value.empty()) {
        return;
    }
    // 光标可能因清空输入框、切换焦点而落后于实际长度，先钳制再拼接，避免越界抛出异常。
    const size_t length = text->size();
    const size_t begin = std::min(SelectionBegin(), length);
    const size_t end = std::min(SelectionEnd(), length);
    *text = text->substr(0, begin) + value + text->substr(end);
    inputCaret_ = begin + value.size();
    inputAnchor_ = inputCaret_;
    StartCaretBlink();
    UpdateImePosition();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Application::DeleteInputBackward() {
    std::wstring* text = ActiveInputText();
    if (text == nullptr || text->empty()) {
        return;
    }
    // 光标越界时先归位，避免按下标访问越界。
    inputCaret_ = std::min(inputCaret_, text->size());
    inputAnchor_ = std::min(inputAnchor_, text->size());
    if (HasInputSelection()) {
        const size_t begin = SelectionBegin();
        const size_t end = SelectionEnd();
        text->erase(begin, end - begin);
        inputCaret_ = begin;
        inputAnchor_ = begin;
    } else {
        if (inputCaret_ == 0) {
            return;
        }
        size_t begin = inputCaret_ - 1;
        if ((*text)[begin] >= 0xDC00 && (*text)[begin] <= 0xDFFF && begin > 0 &&
            (*text)[begin - 1] >= 0xD800 && (*text)[begin - 1] <= 0xDBFF) {
            --begin;
        }
        text->erase(begin, inputCaret_ - begin);
        inputCaret_ = begin;
        inputAnchor_ = begin;
    }
    StartCaretBlink();
    UpdateImePosition();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Application::DeleteInputForward() {
    std::wstring* text = ActiveInputText();
    if (text == nullptr || text->empty()) {
        return;
    }
    if (HasInputSelection()) {
        const size_t begin = SelectionBegin();
        const size_t end = SelectionEnd();
        text->erase(begin, end - begin);
        inputCaret_ = begin;
        inputAnchor_ = begin;
    } else {
        if (inputCaret_ >= text->size()) {
            return;
        }
        size_t end = inputCaret_ + 1;
        if ((*text)[inputCaret_] >= 0xD800 && (*text)[inputCaret_] <= 0xDBFF &&
            end < text->size() && (*text)[end] >= 0xDC00 && (*text)[end] <= 0xDFFF) {
            ++end;
        }
        text->erase(inputCaret_, end - inputCaret_);
        inputAnchor_ = inputCaret_;
    }
    StartCaretBlink();
    UpdateImePosition();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Application::SelectInputAll() {
    const std::wstring* text = ActiveInputText();
    if (text == nullptr) {
        return;
    }
    inputAnchor_ = 0;
    inputCaret_ = text->size();
    StartCaretBlink();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

bool Application::CopyInputSelection() {
    const std::wstring* text = ActiveInputText();
    if (text == nullptr || !HasInputSelection()) {
        return false;
    }
    const size_t length = text->size();
    const size_t begin = std::min(SelectionBegin(), length);
    const size_t end = std::min(SelectionEnd(), length);
    if (begin >= end) {
        return false;
    }
    const std::wstring selected = text->substr(begin, end - begin);
    if (selected.empty()) {
        return false;
    }
    if (!WriteClipboardText(selected)) {
        ShowStatus(L"复制到剪贴板失败");
        return false;
    }
    ShowStatus(L"已复制选中内容");
    return true;
}

bool Application::CutInputSelection() {
    if (!CopyInputSelection()) {
        return false;
    }
    DeleteInputBackward();
    return true;
}

void Application::PasteIntoInput() {
    std::wstring* text = ActiveInputText();
    if (text == nullptr) {
        return;
    }
    std::wstring content;
    if (!ReadClipboardText(content) || content.empty()) {
        ShowStatus(L"剪贴板中没有文本");
        return;
    }
    // 过滤控制字符；单行输入框把换行替换为空格。
    const bool multiline = noteFocused_ && panel_ == PanelMode::NoteEditor;
    std::wstring filtered;
    filtered.reserve(content.size());
    for (wchar_t character : content) {
        if (character == L'\r') {
            continue;
        }
        if (character == L'\n') {
            if (multiline) {
                filtered.push_back(character);
            } else {
                filtered.push_back(L' ');
            }
            continue;
        }
        if (character < 0x20) {
            continue;
        }
        filtered.push_back(character);
    }
    if (filtered.empty()) {
        return;
    }
    const size_t limit = ActiveInputLimit();
    const size_t existing = text->size() - (SelectionEnd() - SelectionBegin());
    if (existing >= limit) {
        ShowStatus(L"已达到输入长度上限");
        return;
    }
    if (existing + filtered.size() > limit) {
        filtered.resize(limit - existing);
    }
    InsertInputText(filtered);
}

void Application::MoveInputCaret(int direction, bool extend) {
    std::wstring* text = ActiveInputText();
    if (text == nullptr) {
        return;
    }
    const size_t length = text->size();
    size_t target = inputCaret_;
    if (!extend && HasInputSelection()) {
        target = direction < 0 ? SelectionBegin() : SelectionEnd();
    } else if (direction < 0) {
        if (target > 0) {
            --target;
            if (target > 0 && (*text)[target] >= 0xDC00 && (*text)[target] <= 0xDFFF &&
                (*text)[target - 1] >= 0xD800 && (*text)[target - 1] <= 0xDBFF) {
                --target;
            }
        }
    } else if (direction > 0 && target < length) {
        ++target;
        if (target < length && (*text)[target] >= 0xDC00 && (*text)[target] <= 0xDFFF) {
            ++target;
        }
    }
    inputCaret_ = std::min(target, length);
    if (!extend) {
        inputAnchor_ = inputCaret_;
    }
    StartCaretBlink();
    UpdateImePosition();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

bool Application::HandleInputKey(WPARAM key, bool controlPressed, bool shiftPressed) {
    if (ActiveInputText() == nullptr) {
        return false;
    }
    if (key == VK_ESCAPE || key == VK_RETURN) {
        return false;  // 由各输入框分支单独处理
    }
    if (controlPressed) {
        switch (key) {
        case 'A':
            SelectInputAll();
            return true;
        case 'C':
            CopyInputSelection();
            return true;
        case 'X':
            CutInputSelection();
            return true;
        case 'V':
            PasteIntoInput();
            return true;
        default:
            return false;
        }
    }
    switch (key) {
    case VK_BACK:
        DeleteInputBackward();
        return true;
    case VK_DELETE:
        DeleteInputForward();
        return true;
    case VK_LEFT:
        MoveInputCaret(-1, shiftPressed);
        return true;
    case VK_RIGHT:
        MoveInputCaret(1, shiftPressed);
        return true;
    case VK_HOME:
        inputCaret_ = 0;
        if (!shiftPressed) {
            inputAnchor_ = 0;
        }
        StartCaretBlink();
        UpdateImePosition();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return true;
    case VK_END: {
        const std::wstring* text = ActiveInputText();
        inputCaret_ = text != nullptr ? text->size() : 0;
        if (!shiftPressed) {
            inputAnchor_ = inputCaret_;
        }
        StartCaretBlink();
        UpdateImePosition();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return true;
    }
    default:
        return false;
    }
}

void Application::BeginInputSelection(float x, float y) {
    if (ActiveInputText() == nullptr) {
        return;
    }
    inputCaret_ = InputCaretIndexAt(x, y);
    inputAnchor_ = inputCaret_;
    inputSelecting_ = true;
    SetCapture(hwnd_);
    StartCaretBlink();
    UpdateImePosition();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Application::UpdateInputSelection(float x, float y) {
    if (!inputSelecting_ || ActiveInputText() == nullptr) {
        return;
    }
    inputCaret_ = InputCaretIndexAt(x, y);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Application::DrawInputSelection(const D2D1_RECT_F& textRect, IDWriteTextFormat* format,
                                     bool wrap, const ThemeColors& colors) {
    const std::wstring* text = ActiveInputText();
    if (text == nullptr || !HasInputSelection() || format == nullptr) {
        return;
    }
    const size_t begin = SelectionBegin();
    const size_t end = SelectionEnd();
    if (begin >= end) {
        return;
    }
    const float width = std::max(1.0f, textRect.right - textRect.left);
    ComPtr<IDWriteTextLayout> layout = renderer_.CreateLayout(*text, format, width, 100000.0f);
    if (!layout) {
        return;
    }
    if (wrap) {
        layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    }
    UINT32 count = 0;
    layout->HitTestTextRange(static_cast<UINT32>(begin), static_cast<UINT32>(end - begin), 0.0f,
                             0.0f, nullptr, 0, &count);
    if (count == 0) {
        return;
    }
    std::vector<DWRITE_HIT_TEST_METRICS> metrics(count);
    if (FAILED(layout->HitTestTextRange(static_cast<UINT32>(begin),
                                        static_cast<UINT32>(end - begin), 0.0f, 0.0f,
                                        metrics.data(), count, &count))) {
        return;
    }
    const D2D1_COLOR_F selectionColor = ParseHexColor(L"#4A90D9", 0.35f);
    for (UINT32 i = 0; i < count; ++i) {
        renderer_.FillRect(
            D2D1::RectF(textRect.left + metrics[i].left, textRect.top + metrics[i].top,
                        textRect.left + metrics[i].left + metrics[i].width,
                        textRect.top + metrics[i].top + metrics[i].height),
            selectionColor);
    }
}

bool Application::FocusedInputCaret(D2D1_POINT_2F& caret, float& lineHeight) {
    lineHeight = 0.0f;
    const TextFormats& formats = renderer_.Formats();
    if (view_ == ViewMode::Shelf && shelfSearchFocused_) {
        const D2D1_RECT_F boxRect = ShelfSearchBoxRect();
        const D2D1_RECT_F textRect =
            D2D1::RectF(boxRect.left + 10.0f, boxRect.top + 6.0f, boxRect.right - 10.0f,
                        boxRect.bottom - 6.0f);
        const float fallbackHeight = formats.listItem->GetFontSize() * 1.35f;
        caret = CaretPointAt(shelfSearchQuery_, formats.listItem.Get(), textRect,
                             renderer_.WriteFactory(), false, inputCaret_, fallbackHeight,
                             lineHeight);
        return true;
    }
    if (view_ == ViewMode::Shelf && categoryManagerVisible_ && categoryNameFocused_) {
        const D2D1_RECT_F boxRect = CategoryNameInputRect();
        const D2D1_RECT_F textRect =
            D2D1::RectF(boxRect.left + 10.0f, boxRect.top + 8.0f, boxRect.right - 10.0f,
                        boxRect.bottom - 8.0f);
        const float fallbackHeight = formats.listItem->GetFontSize() * 1.35f;
        caret = CaretPointAt(categoryDraft_, formats.listItem.Get(), textRect,
                             renderer_.WriteFactory(), false, inputCaret_, fallbackHeight,
                             lineHeight);
        return true;
    }
    if (aiSettingsVisible_ && chatFocus_ != ChatFocus::None) {
        const D2D1_RECT_F boxRect = AiSettingsFieldRect(chatFocus_);
        const D2D1_RECT_F textRect =
            D2D1::RectF(boxRect.left + 10.0f, boxRect.top + 6.0f, boxRect.right - 10.0f,
                        boxRect.bottom - 6.0f);
        const float fallbackHeight = formats.listItem->GetFontSize() * 1.35f;
        const std::wstring* field = ActiveChatField();
        std::wstring text = field != nullptr ? *field : std::wstring();
        if (chatFocus_ == ChatFocus::ApiKey) {
            // 密钥以圆点掩码显示，光标必须按掩码文本计算，才能与可见字符位置一致。
            text.assign(text.size(), L'\u2022');
        }
        caret = CaretPointAt(text, formats.listItem.Get(), textRect, renderer_.WriteFactory(),
                             false, inputCaret_, fallbackHeight, lineHeight);
        return true;
    }
    if (panel_ == PanelMode::Chat && chatFocus_ != ChatFocus::None) {
        const D2D1_RECT_F boxRect = ChatFieldRect(chatFocus_);
        const D2D1_RECT_F textRect =
            D2D1::RectF(boxRect.left + 10.0f, boxRect.top + 7.0f, boxRect.right - 10.0f,
                        boxRect.bottom - 7.0f);
        const float fallbackHeight = formats.listItem->GetFontSize() * 1.35f;
        const std::wstring* field = ActiveChatField();
        const std::wstring text = field != nullptr ? *field : std::wstring();
        caret = CaretPointAt(text, formats.listItem.Get(), textRect, renderer_.WriteFactory(),
                             chatFocus_ == ChatFocus::Message, inputCaret_, fallbackHeight,
                             lineHeight);
        return true;
    }
    if (noteFocused_ && panel_ == PanelMode::NoteEditor) {
        const D2D1_RECT_F boxRect = NoteInputBoxRect();
        const D2D1_RECT_F textRect =
            D2D1::RectF(boxRect.left + kNoteInputPaddingX, boxRect.top + kNoteInputPaddingY,
                        boxRect.right - kNoteInputPaddingX, boxRect.bottom - kNoteInputPaddingY);
        const float fallbackHeight = formats.label->GetFontSize() * 1.35f;
        caret = CaretPointAt(noteDraft_, formats.label.Get(), textRect, renderer_.WriteFactory(),
                             true, inputCaret_, fallbackHeight, lineHeight);
        return true;
    }
    if (searchFocused_ && panel_ == PanelMode::Search) {
        const D2D1_RECT_F boxRect = SearchBoxRect();
        const D2D1_RECT_F textRect =
            D2D1::RectF(boxRect.left + kSearchTextPaddingX, boxRect.top + kSearchTextPaddingTop,
                        boxRect.right - kSearchTextPaddingX,
                        boxRect.bottom - kSearchTextPaddingBottom);
        const float fallbackHeight = formats.listItem->GetFontSize() * 1.35f;
        caret = CaretPointAt(searchQuery_, formats.listItem.Get(), textRect,
                             renderer_.WriteFactory(), false, inputCaret_, fallbackHeight,
                             lineHeight);
        return true;
    }
    return false;
}

void Application::UpdateImePosition() {
    D2D1_POINT_2F caret{};
    float caretLineHeight = 0.0f;
    if (!FocusedInputCaret(caret, caretLineHeight)) {
        return;
    }
    const float scale = renderer_.DpiScale();
    const LONG clientWidth = static_cast<LONG>(std::lround(widthDip_ * scale));
    const LONG clientHeight = static_cast<LONG>(std::lround(heightDip_ * scale));
    POINT point{};
    point.x = std::clamp<LONG>(static_cast<LONG>(std::lround(caret.x * scale)), 0, clientWidth);
    point.y = std::clamp<LONG>(static_cast<LONG>(std::lround(caret.y * scale)), 0, clientHeight);

    HIMC context = ImmGetContext(hwnd_);
    if (!context) {
        return;
    }
    COMPOSITIONFORM composition{};
    composition.dwStyle = CFS_POINT;
    composition.ptCurrentPos = point;
    ImmSetCompositionWindow(context, &composition);

    CANDIDATEFORM candidate{};
    candidate.dwIndex = 0;
    candidate.dwStyle = CFS_CANDIDATEPOS;
    candidate.ptCurrentPos = point;
    ImmSetCandidateWindow(context, &candidate);

    // 组合字符串使用微软雅黑、常规字重与较大字号，避免拼音字母过小过粗。
    LOGFONTW compositionFont{};
    compositionFont.lfHeight = -static_cast<LONG>(std::lround(20.0f * scale));
    compositionFont.lfWeight = FW_NORMAL;
    compositionFont.lfCharSet = DEFAULT_CHARSET;
    compositionFont.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(compositionFont.lfFaceName, L"Microsoft YaHei");
    ImmSetCompositionFontW(context, &compositionFont);
    ImmReleaseContext(hwnd_, context);
}

void Application::ApplyHighlight(const std::wstring& color) {
    data::BookRecord* record = library_.Find(activeBookId_);
    colorPickerVisible_ = false;
    if (!record || !selectionActive_ || color.empty()) {
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    const uint32_t start = chapterDisplay_.ToOriginal(selectionStartDisplay_);
    const uint32_t end = chapterDisplay_.ToOriginal(selectionEndDisplay_);
    if (end <= start) {
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    data::Highlight highlight;
    highlight.chapterIndex = activeChapter_;
    highlight.start = start;
    highlight.length = end - start;
    highlight.color = color;
    highlight.createdAt = util::CurrentUnixSeconds();
    record->highlights.push_back(highlight);
    std::wstring error;
    library_.SaveBook(activeBookId_, error);
    ClearSelection();
    ShowStatus(L"已添加高亮");
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Application::DeleteHighlight(int index) {
    data::BookRecord* record = library_.Find(activeBookId_);
    if (!record || index < 0 || index >= static_cast<int>(record->highlights.size())) {
        return;
    }
    record->highlights.erase(record->highlights.begin() + index);
    std::wstring error;
    library_.SaveBook(activeBookId_, error);
    annotationHits_.clear();
    ShowStatus(L"已删除高亮");
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Application::DeleteNote(int index) {
    data::BookRecord* record = library_.Find(activeBookId_);
    if (!record || index < 0 || index >= static_cast<int>(record->notes.size())) {
        return;
    }
    const uint32_t originalStart = record->notes[index].start;
    record->notes.erase(record->notes.begin() + index);
    std::wstring error;
    library_.SaveBook(activeBookId_, error);
    ClearLayoutCache();
    annotationHits_.clear();
    ShowStatus(L"已删除标注");
    if (activeBook_.chapters.empty()) {
        return;
    }
    const int chapter = std::clamp(activeChapter_, 0,
                                   static_cast<int>(activeBook_.chapters.size()) - 1);
    RebuildPagination(activeBook_.chapters[static_cast<size_t>(chapter)].charOffset +
                      originalStart);
}

void Application::OpenNoteEditor() {
    if (!selectionActive_ || activeChapterRaw_.empty()) {
        return;
    }
    const uint32_t start = chapterDisplay_.ToOriginal(selectionStartDisplay_);
    const uint32_t end = chapterDisplay_.ToOriginal(selectionEndDisplay_);
    if (end <= start) {
        return;
    }
    pendingNoteStart_ = start;
    pendingNoteLength_ = end - start;
    pendingNotePreview_ = activeChapterRaw_.substr(start, pendingNoteLength_);
    noteDraft_.clear();
    noteFocused_ = true;
    colorPickerVisible_ = false;
    ChangePanel(PanelMode::NoteEditor);
    selectionStartDisplay_ = chapterDisplay_.ToDisplay(pendingNoteStart_);
    selectionEndDisplay_ = chapterDisplay_.ToDisplay(pendingNoteStart_ + pendingNoteLength_);
    selectionActive_ = selectionEndDisplay_ > selectionStartDisplay_;
    if (pager_.PageCount() > 0) {
        activePage_ = pager_.PageIndexForChar(selectionStartDisplay_);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Application::SaveNote() {
    if (panel_ != PanelMode::NoteEditor) {
        return;
    }
    const std::wstring text = TrimNoteText(noteDraft_);
    if (text.empty()) {
        ShowStatus(L"标注内容不能为空");
        return;
    }
    if (text.size() > static_cast<size_t>(kMaxNoteChars)) {
        ShowStatus(L"标注最多 255 个字符");
        return;
    }
    data::BookRecord* record = library_.Find(activeBookId_);
    if (!record) {
        return;
    }
    data::Note note;
    note.chapterIndex = activeChapter_;
    note.start = pendingNoteStart_;
    note.length = pendingNoteLength_;
    note.text = text;
    note.textHash = util::Sha256Hex(util::ToUtf8(text)).substr(0, 32);
    note.createdAt = util::CurrentUnixSeconds();
    record->notes.push_back(note);
    std::wstring error;
    library_.SaveBook(activeBookId_, error);
    ClearLayoutCache();

    panel_ = PanelMode::None;
    noteFocused_ = false;
    StopCaretBlink();
    noteDraft_.clear();
    ClearSelection();
    if (!activeBook_.chapters.empty()) {
        const int chapter = std::clamp(activeChapter_, 0,
                                       static_cast<int>(activeBook_.chapters.size()) - 1);
        RebuildPagination(activeBook_.chapters[static_cast<size_t>(chapter)].charOffset +
                          pendingNoteStart_);
    }
    ShowStatus(L"已添加标注");
}

void Application::CancelNoteEditor() {
    if (panel_ != PanelMode::NoteEditor) {
        return;
    }
    const uint32_t offset = CurrentGlobalOffset();
    panel_ = PanelMode::None;
    noteFocused_ = false;
    StopCaretBlink();
    noteDraft_.clear();
    ClearSelection();
    RebuildPagination(offset);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

uint32_t Application::DisplayOffsetAt(float x, float y) const {
    if (!pageLayout_ || !pageLayoutValid_) {
        return pageLayoutStart_;
    }
    const float width = std::max(1.0f, pageLayoutRect_.right - pageLayoutRect_.left);
    const float height = std::max(1.0f, pageLayoutRect_.bottom - pageLayoutRect_.top);
    const float localX = std::clamp(x - pageLayoutRect_.left, 0.0f, width - 1.0f);
    const float localY = std::clamp(y - pageLayoutRect_.top, 0.0f, height - 1.0f);
    BOOL trailing = FALSE;
    BOOL inside = FALSE;
    DWRITE_HIT_TEST_METRICS metrics{};
    if (FAILED(pageLayout_->HitTestPoint(localX, localY, &trailing, &inside, &metrics))) {
        return pageLayoutStart_;
    }
    uint32_t offset = pageLayoutStart_ + metrics.textPosition;
    if (trailing != FALSE && inside != FALSE) {
        ++offset;
    }
    return std::min(offset, pageLayoutEnd_);
}

bool Application::EnsurePageLayout() {
    if (pageLayoutValid_ && pageLayout_) {
        return true;
    }
    if (pager_.PageCount() == 0) {
        return false;
    }
    float textWidth = 0.0f;
    float textHeight = 0.0f;
    D2D1_RECT_F textRect{};
    ComputeTextMetrics(textWidth, textHeight, textRect);
    const std::wstring& displayText = chapterDisplay_.Text();
    const uint32_t length = static_cast<uint32_t>(displayText.size());
    const uint32_t start = ViewportStartOffset();
    const uint32_t end =
        LineScrollMode()
            ? std::min<uint32_t>(start + kLinePreviewChars, length)
            : std::min(pager_.Page(std::min(activePage_, pager_.PageCount() - 1)).endChar,
                       length);
    const std::wstring pageText = displayText.substr(start, end - start);
    ComPtr<IDWriteTextLayout> layout =
        renderer_.CreateBodyLayout(pageText, textWidth, textHeight + 16.0f);
    if (!layout) {
        return false;
    }
    pageLayout_ = layout;
    pageLayoutStart_ = start;
    pageLayoutEnd_ = end;
    pageLayoutRect_ = textRect;
    pageLayoutValid_ = true;
    return true;
}

void Application::InvalidatePageCache() {
    pageLayoutValid_ = false;
    pageLayout_.Reset();
}

float Application::LineTopForOffset(IDWriteTextLayout* layout, uint32_t offset) const {
    if (!layout) {
        return 0.0f;
    }
    UINT32 count = 0;
    layout->GetLineMetrics(nullptr, 0, &count);
    if (count == 0) {
        return 0.0f;
    }
    std::vector<DWRITE_LINE_METRICS> metrics(count);
    layout->GetLineMetrics(metrics.data(), count, &count);
    float top = 0.0f;
    uint32_t start = 0;
    for (UINT32 i = 0; i < count; ++i) {
        if (offset < start + metrics[i].length) {
            return top;
        }
        top += metrics[i].height;
        start += metrics[i].length;
    }
    return top;
}

float Application::FirstLineHeight(IDWriteTextLayout* layout) const {
    if (!layout) {
        return 0.0f;
    }
    DWRITE_LINE_METRICS metrics{};
    UINT32 actual = 0;
    if (FAILED(layout->GetLineMetrics(&metrics, 1, &actual)) || actual == 0) {
        return 0.0f;
    }
    return metrics.height;
}

void Application::OnKeyDown(WPARAM key) {
    if (shortcutSettingsVisible_) {
        const bool modifierOnly = key == VK_CONTROL || key == VK_SHIFT || key == VK_MENU ||
                                  key == VK_LWIN || key == VK_RWIN;
        if (key == VK_ESCAPE) {
            if (shortcutCaptureIndex_ >= 0) {
                shortcutCaptureIndex_ = -1;
                InvalidateRect(hwnd_, nullptr, FALSE);
            } else {
                CloseShortcutSettings();
            }
            return;
        }
        if (shortcutCaptureIndex_ >= 0 && !modifierOnly) {
            AssignShortcut(static_cast<UINT>(key));
        }
        return;
    }
    if (chatSessionPickerVisible_) {
        // 对话记录弹框打开时只响应 Esc 关闭。
        if (key == VK_ESCAPE) {
            chatSessionPickerVisible_ = false;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return;
    }
    if (fontPickerVisible_) {
        // 字体弹框打开时只响应 Esc 关闭，避免误触底层快捷键。
        if (key == VK_ESCAPE) {
            fontPickerVisible_ = false;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return;
    }
    if (menuVisible_) {
        // 菜单打开时 Esc 收起菜单。
        if (key == VK_ESCAPE) {
            CloseMenu();
        }
        return;
    }
    if (aiSettingsVisible_ && key == VK_ESCAPE) {
        CloseAiSettings();
        return;
    }
    if (view_ == ViewMode::Shelf && key == VK_ESCAPE) {
        if (shelfSearchFocused_) {
            ClearShelfSearch();
            return;
        }
        if (categoryNameFocused_) {
            categoryNameFocused_ = false;
            StopCaretBlink();
        } else if (categoryManagerVisible_) {
            categoryManagerVisible_ = false;
        } else if (shelfSettingsVisible_) {
            shelfSettingsVisible_ = false;
        } else if (!shelfCategoryPickerBookId_.empty()) {
            shelfCategoryPickerBookId_.clear();
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    const bool controlPressed = (GetKeyState(VK_CONTROL) & 0x8000) != 0;

    if (aiSettingsVisible_ && chatFocus_ == ChatFocus::None) {
        // 弹框打开且没有焦点输入框时，不响应底层阅读快捷键。
        return;
    }

    if (view_ == ViewMode::Shelf && shelfSearchFocused_) {
        const bool shiftPressed = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        if (HandleInputKey(key, controlPressed, shiftPressed)) {
            return;
        }
        if (key == VK_RETURN) {
            ApplyShelfSearch();
            return;
        }
        return;
    }

    if (view_ == ViewMode::Shelf && categoryManagerVisible_ && categoryNameFocused_) {
        const bool shiftPressed = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        if (HandleInputKey(key, controlPressed, shiftPressed)) {
            return;
        }
        if (key == VK_RETURN) {
            CreateCategoryFromDraft();
            return;
        }
        return;
    }

    if (noteFocused_ && panel_ == PanelMode::NoteEditor) {
        const bool shiftPressed = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        if (HandleInputKey(key, controlPressed, shiftPressed)) {
            return;
        }
        if (key == VK_ESCAPE) {
            CancelNoteEditor();
            return;
        }
        if (key == VK_RETURN) {
            // 在插入符位置换行，替代原来的尾部追加。
            const size_t caret = std::min(inputCaret_, noteDraft_.size());
            if (noteDraft_.size() + 2 <= static_cast<size_t>(kMaxNoteChars)) {
                noteDraft_.insert(caret, L"\r\n");
                inputCaret_ = caret + 2;
                inputAnchor_ = inputCaret_;
                UpdateImePosition();
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return;
        }
        if (controlPressed && key == 'S') {
            SaveNote();
            return;
        }
        return;
    }

    if (searchFocused_) {
        const bool shiftPressed = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        if (HandleInputKey(key, controlPressed, shiftPressed)) {
            return;
        }
        if (key == VK_RETURN) {
            RunSearch();
            return;
        }
        if (key == VK_ESCAPE) {
            ChangePanel(PanelMode::None);
        }
        return;
    }

    if (aiSettingsVisible_ && chatFocus_ != ChatFocus::None) {
        const bool shiftPressed = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        if (HandleInputKey(key, controlPressed, shiftPressed)) {
            return;
        }
        if (key == VK_RETURN) {
            // 回车保存并关闭设置弹框，避免每次按键都写文件。
            SaveAiSettings();
            return;
        }
        return;
    }

    if (panel_ == PanelMode::Chat && chatFocus_ != ChatFocus::None) {
        const bool shiftPressed = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        if (controlPressed && key == 'C' && inputAnchor_ == inputCaret_ && selectionActive_) {
            // 对话输入框没有选中内容时，Ctrl+C 复制阅读页中已选中的文字。
            CopySelection();
            return;
        }
        if (HandleInputKey(key, controlPressed, shiftPressed)) {
            return;
        }
        if (key == VK_ESCAPE) {
            chatFocus_ = ChatFocus::None;
            StopCaretBlink();
            UpdateImePosition();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        if (key == VK_RETURN) {
            if (chatFocus_ == ChatFocus::Message) {
                // 多行输入：发送键可自定义，默认为 Ctrl+回车。
                if (ShortcutMatches(ShortcutCombo(library_.Settings(), data::kShortcutChatSend,
                                                  L"Ctrl+Enter"),
                                    key)) {
                    SendAiMessage();
                } else {
                    InsertInputText(L"\r\n");
                }
            } else {
                // 配置项在回车时落盘，避免每次按键都写文件。
                std::wstring error;
                library_.Save(error);
                ShowStatus(L"AI 配置已保存");
            }
            return;
        }
        return;
    }

    if (view_ == ViewMode::Reader) {
        const data::ReaderSettings& shortcut = library_.Settings();
        if (ShortcutMatches(ShortcutCombo(shortcut, data::kShortcutCopy, L"Ctrl+C"), key)) {
            // 对话面板内的文字选择优先于正文选择。
            if (panel_ == PanelMode::Chat && CopyChatSelection()) {
                return;
            }
            CopySelection();
            return;
        }
        if (ShortcutMatches(ShortcutCombo(shortcut, data::kShortcutSearch, L"Ctrl+F"), key)) {
            ChangePanel(PanelMode::Search);
            return;
        }
        if (ShortcutMatches(ShortcutCombo(shortcut, data::kShortcutBookmark, L"Ctrl+D"), key)) {
            AddBookmark();
            return;
        }
        if (ShortcutMatches(ShortcutCombo(shortcut, data::kShortcutPrevPage, L"←"), key)) {
            PreviousPage();
            return;
        }
        if (ShortcutMatches(ShortcutCombo(shortcut, data::kShortcutNextPage, L"→"), key)) {
            NextPage();
            return;
        }
        switch (key) {
        case VK_UP:
        case VK_PRIOR:
            PreviousPage();
            return;
        case VK_DOWN:
        case VK_NEXT:
        case VK_SPACE:
            NextPage();
            return;
        case VK_HOME:
            activePage_ = 0;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        case VK_END:
            activePage_ = pager_.PageCount() > 0 ? pager_.PageCount() - 1 : 0;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        case VK_ESCAPE:
            if (colorPickerVisible_) {
                colorPickerVisible_ = false;
                InvalidateRect(hwnd_, nullptr, FALSE);
            } else if (gestureBarVisible_) {
                gestureBarVisible_ = false;
                gesture_ = Gesture::Default;
                ClearSelection();
                InvalidateRect(hwnd_, nullptr, FALSE);
            } else if (panel_ != PanelMode::None) {
                ChangePanel(PanelMode::None);
            } else {
                CloseBook();
            }
            return;
        default:
            break;
        }
        return;
    }

    if (controlPressed && key == 'O') {
        OpenFileDialog();
        return;
    }
    if (key == VK_ESCAPE && panel_ != PanelMode::None) {
        panel_ = PanelMode::None;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void Application::OnChar(wchar_t character) {
    if (character < 0x20 || character == L'\x7F') {
        return;
    }
    std::wstring* text = ActiveInputText();
    if (text == nullptr || text->size() >= ActiveInputLimit()) {
        return;
    }
    InsertInputText(std::wstring(1, character));
}

void Application::OnUnicodeInput(uint32_t codePoint) {
    if (codePoint > 0x10FFFF || (codePoint >= 0xD800 && codePoint <= 0xDFFF)) {
        return;
    }
    std::wstring* text = ActiveInputText();
    if (text == nullptr || text->size() >= ActiveInputLimit()) {
        return;
    }
    std::wstring value;
    if (codePoint <= 0xFFFF) {
        value.push_back(static_cast<wchar_t>(codePoint));
    } else {
        const uint32_t adjusted = codePoint - 0x10000;
        value.push_back(static_cast<wchar_t>(0xD800 + (adjusted >> 10)));
        value.push_back(static_cast<wchar_t>(0xDC00 + (adjusted & 0x3FF)));
    }
    if (value.size() + text->size() > ActiveInputLimit()) {
        return;
    }
    InsertInputText(value);
}

void Application::OnDropFiles(HDROP drop) {
    const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    std::vector<std::filesystem::path> paths;
    for (UINT index = 0; index < count; ++index) {
        const UINT length = DragQueryFileW(drop, index, nullptr, 0);
        if (length == 0) {
            continue;
        }
        std::wstring buffer(length + 1, L'\0');
        if (DragQueryFileW(drop, index, buffer.data(), length + 1) > 0) {
            buffer.resize(length);
            paths.emplace_back(buffer);
        }
    }
    DragFinish(drop);
    if (!paths.empty()) {
        ImportFiles(paths);
    }
}

void Application::OnClose() {
    StopAiRequest();
    if (view_ == ViewMode::Reader && !activeBookId_.empty()) {
        library_.TouchBook(activeBookId_, CurrentGlobalOffset());
    }
    data::ReaderSettings& settings = library_.Settings();
    settings.windowWidth = std::max(720, static_cast<int>(std::lround(widthDip_)));
    settings.windowHeight = std::max(520, static_cast<int>(std::lround(heightDip_)));
    std::wstring error;
    library_.Save(error);
    DestroyWindow(hwnd_);
}

void Application::DrawShelf(const ThemeColors& colors) {
    renderer_.Clear(colors.background);
    const TextFormats& formats = renderer_.Formats();

    renderer_.DrawTextBlock(L"书架", D2D1::RectF(44.0f, 20.0f, 400.0f, 56.0f), formats.title.Get(),
                            colors.text);
    renderer_.DrawTextBlock(FormatCountLine(library_.Books().size()),
                            D2D1::RectF(44.0f, 60.0f, widthDip_ - 340.0f, 82.0f),
                            formats.caption.Get(), colors.secondaryText);

    const float buttonTop = 24.0f;
    const float buttonHeight = 40.0f;
    const D2D1_RECT_F addRect = D2D1::RectF(widthDip_ - 44.0f - 128.0f, buttonTop,
                                            widthDip_ - 44.0f, buttonTop + buttonHeight);
    const D2D1_RECT_F associateRect =
        D2D1::RectF(addRect.left - 12.0f - 128.0f, buttonTop, addRect.left - 12.0f,
                    buttonTop + buttonHeight);
    DrawButton(L"添加书籍", addRect, HitKind::AddBook, colors, true, false);
    DrawButton(L"设置", associateRect, HitKind::ShelfSettings, colors, false,
               shelfSettingsVisible_);

    renderer_.DrawLine(D2D1::Point2F(44.0f, kShelfToolbarHeight),
                       D2D1::Point2F(widthDip_ - 44.0f, kShelfToolbarHeight), colors.divider, 1.0f);

    // 书架正则搜索框：回车执行，Esc 清除。
    const D2D1_RECT_F searchBox = ShelfSearchBoxRect();
    renderer_.FillRoundedRect(searchBox, 8.0f, colors.surface);
    renderer_.StrokeRoundedRect(searchBox, 8.0f,
                                shelfSearchFocused_ ? colors.accent : colors.divider,
                                shelfSearchFocused_ ? 1.6f : 1.0f);
    const D2D1_RECT_F searchTextRect =
        D2D1::RectF(searchBox.left + 10.0f, searchBox.top + 6.0f, searchBox.right - 10.0f,
                    searchBox.bottom - 6.0f);
    const bool searchEmpty = shelfSearchQuery_.empty();
    renderer_.DrawTextBlock(searchEmpty ? std::wstring(L"正则搜索书名、作者、文件名，回车执行")
                                        : shelfSearchQuery_,
                            searchTextRect, formats.listItem.Get(),
                            searchEmpty ? colors.secondaryText : colors.text);
    if (shelfSearchFocused_) {
        DrawInputSelection(searchTextRect, formats.listItem.Get(), false, colors);
    }
    AddHit(searchBox, HitKind::ShelfSearchBox);

    // 状态提示显示在搜索框右侧，避免与输入框重叠。
    if (!statusText_.empty()) {
        const float statusLeft = searchBox.right + 12.0f;
        if (statusLeft < widthDip_ - 44.0f) {
            renderer_.DrawTextBlock(util::Ellipsize(statusText_, 60),
                                    D2D1::RectF(statusLeft, searchBox.top, widthDip_ - 44.0f,
                                                searchBox.bottom),
                                    formats.caption.Get(), colors.accent);
        }
    }

    DrawCategorySidebar(colors);

    if (shelfOrder_.empty()) {
        DrawEmptyShelf(colors);
        DrawShelfSettings(colors);
        DrawCategoryManager(colors);
        DrawCategoryPicker(colors);
        DrawAiSettings(colors);
        DrawShortcutSettings(colors);
        DrawInputCaret(colors);
        return;
    }

    const float gridLeft = kShelfMargin + kCategorySidebarWidth + 20.0f;
    const float availableWidth = widthDip_ - gridLeft - kShelfMargin;
    const int maximumColumns = std::max(
        static_cast<int>((availableWidth + kShelfCardGapX) / (kShelfCardWidth + kShelfCardGapX)), 1);
    const int columns =
        std::min(maximumColumns, std::max(static_cast<int>(shelfOrder_.size()), 1));
    const float gridWidth =
        columns * kShelfCardWidth + (columns - 1) * kShelfCardGapX;
    const float startX = gridLeft + (availableWidth - gridWidth) / 2.0f;
    const size_t rows = (shelfOrder_.size() + static_cast<size_t>(columns) - 1) /
                        static_cast<size_t>(columns);
    const float contentHeight =
        rows * kShelfCardHeight + (rows > 1 ? (rows - 1) * kShelfCardGapY : 0.0f);
    shelfScrollMax_ = std::max(0.0f, contentHeight - (heightDip_ - kShelfStartY - 20.0f));
    shelfScroll_ = std::clamp(shelfScroll_, 0.0f, shelfScrollMax_);

    BeginClip(D2D1::RectF(0.0f, kShelfStartY - 10.0f, widthDip_, heightDip_));
    for (size_t i = 0; i < shelfOrder_.size(); ++i) {
        const int column = static_cast<int>(i % static_cast<size_t>(columns));
        const int row = static_cast<int>(i / static_cast<size_t>(columns));
        const float x = startX + column * (kShelfCardWidth + kShelfCardGapX);
        const float y =
            kShelfStartY + row * (kShelfCardHeight + kShelfCardGapY) - shelfScroll_;
        if (y + kShelfCardHeight < kShelfStartY - 20.0f || y > heightDip_) {
            continue;
        }
        const size_t bookIndex = shelfOrder_[i];
        DrawBookCard(library_.Books()[bookIndex],
                     D2D1::RectF(x, y, x + kShelfCardWidth, y + kShelfCardHeight), colors,
                     static_cast<int>(bookIndex));
    }
    EndClip();
    DrawShelfSettings(colors);
    DrawCategoryManager(colors);
    DrawCategoryPicker(colors);
    DrawAiSettings(colors);
    DrawShortcutSettings(colors);
    DrawInputCaret(colors);
}

D2D1_RECT_F Application::CategoryNameInputRect() const {
    constexpr float kPanelWidth = 460.0f;
    constexpr float kPanelHeight = 300.0f;
    const float left = (widthDip_ - kPanelWidth) * 0.5f;
    const float top = (heightDip_ - kPanelHeight) * 0.5f;
    return D2D1::RectF(left + 24.0f, top + 94.0f, left + kPanelWidth - 24.0f, top + 130.0f);
}

D2D1_RECT_F Application::ShelfSearchBoxRect() const {
    // 底边与左侧分类栏上沿对齐，并留出 2 DIP 间隙；高度按 15 DIP 字号留足行高。
    const float bottom = kShelfStartY - 2.0f;
    const float height = 34.0f;
    return D2D1::RectF(44.0f, bottom - height, 44.0f + 320.0f, bottom);
}

void Application::DrawCategorySidebar(const ThemeColors& colors) {
    const TextFormats& formats = renderer_.Formats();
    const float left = kShelfMargin;
    const float top = kShelfStartY;
    const D2D1_RECT_F panel =
        D2D1::RectF(left, top, left + kCategorySidebarWidth, heightDip_ - 20.0f);
    renderer_.FillRoundedRect(panel, 10.0f, colors.surface);

    // 分类管理入口：新建、重命名与删除统一放在书架设置浮层中。
    DrawButton(L"管理分类", D2D1::RectF(left + 12.0f, top + 12.0f,
                                        left + kCategorySidebarWidth - 12.0f, top + 40.0f),
               HitKind::CategoryManager, colors, false, categoryManagerVisible_);
    DrawSlidersIcon(renderer_, left + 16.0f, top + 19.0f, colors.secondaryText);

    BeginClip(D2D1::RectF(left + 4.0f, top + 48.0f, left + kCategorySidebarWidth - 4.0f,
                          heightDip_ - 24.0f));
    float y = top + 50.0f;
    constexpr float kRowHeight = 28.0f;

    // 固定项：全部显示用网格图标，未分类用收件托盘图标，均不使用文件夹。
    enum class FixedIcon { Grid, Inbox };
    const auto drawFixed = [&](const wchar_t* label, HitKind kind, bool active,
                               FixedIcon icon, const D2D1_RECT_F& rowRect) {
        if (active) {
            renderer_.FillRoundedRect(rowRect, 6.0f, colors.surfaceAlt);
        }
        const D2D1_COLOR_F iconColor = active ? colors.accent : colors.secondaryText;
        if (icon == FixedIcon::Grid) {
            DrawGridIcon(renderer_, rowRect.left + 10.0f, rowRect.top + 8.0f, iconColor);
        } else {
            DrawInboxIcon(renderer_, rowRect.left + 10.0f, rowRect.top + 8.0f, iconColor);
        }
        renderer_.DrawTextBlock(label,
                                D2D1::RectF(rowRect.left + 28.0f, rowRect.top + 4.0f,
                                            rowRect.right - 6.0f, rowRect.bottom - 4.0f),
                                formats.listItem.Get(),
                                active ? colors.accent : colors.text);
        AddHit(rowRect, kind);
    };
    drawFixed(L"全部显示", HitKind::CategoryAll, shelfFilterCategory_.empty(), FixedIcon::Grid,
              D2D1::RectF(left + 8.0f, y, left + kCategorySidebarWidth - 8.0f, y + kRowHeight));
    y += kRowHeight + 2.0f;
    drawFixed(L"未分类", HitKind::CategoryUncategorized,
              shelfFilterCategory_ == kUncategorizedFilter, FixedIcon::Inbox,
              D2D1::RectF(left + 8.0f, y, left + kCategorySidebarWidth - 8.0f, y + kRowHeight));
    y += kRowHeight + 6.0f;

    // 分类树按层级缩进显示。
    struct TreeNode {
        std::wstring parentId;
        int depth;
    };
    std::vector<TreeNode> queue;
    queue.push_back({std::wstring(), 0});
    size_t head = 0;
    while (head < queue.size()) {
        const TreeNode node = queue[head++];
        for (const data::Category& category : library_.Categories()) {
            if (category.parentId != node.parentId) {
                continue;
            }
            const float indent = 8.0f + node.depth * 16.0f;
            const D2D1_RECT_F rowRect = D2D1::RectF(left + 8.0f, y,
                                                    left + kCategorySidebarWidth - 8.0f,
                                                    y + kRowHeight);
            const bool active = shelfFilterCategory_ == category.id;
            if (active) {
                renderer_.FillRoundedRect(rowRect, 6.0f, colors.surfaceAlt);
            }
            renderer_.DrawTextBlock(util::Ellipsize(category.name, 12),
                                    D2D1::RectF(rowRect.left + indent + 18.0f, rowRect.top + 4.0f,
                                                rowRect.right - 6.0f, rowRect.bottom - 4.0f),
                                    formats.listItem.Get(),
                                    active ? colors.accent : colors.text);
            DrawFolderIcon(renderer_, rowRect.left + indent, rowRect.top + 8.0f,
                           active ? colors.accent : colors.secondaryText);
            AddHit(rowRect, HitKind::CategoryItem, -1, category.id);
            y += kRowHeight;
            queue.push_back({category.id, node.depth + 1});
        }
    }
    EndClip();
}

void Application::DrawCategoryPicker(const ThemeColors& colors) {
    if (shelfCategoryPickerBookId_.empty()) {
        return;
    }
    const TextFormats& formats = renderer_.Formats();
    renderer_.FillRect(D2D1::RectF(0.0f, 0.0f, widthDip_, heightDip_),
                       ParseHexColor(L"#000000", 0.35f));

    constexpr float kPanelWidth = 380.0f;
    constexpr float kPanelHeight = 320.0f;
    const float left = (widthDip_ - kPanelWidth) * 0.5f;
    const float top = (heightDip_ - kPanelHeight) * 0.5f;
    const D2D1_RECT_F panel =
        D2D1::RectF(left, top, left + kPanelWidth, top + kPanelHeight);
    renderer_.FillRoundedRect(panel, 12.0f, colors.surface);
    renderer_.StrokeRoundedRect(panel, 12.0f, colors.divider, 1.0f);
    renderer_.DrawTextBlock(L"选择分类",
                            D2D1::RectF(left + 24.0f, top + 16.0f, left + kPanelWidth - 24.0f,
                                        top + 42.0f),
                            formats.title.Get(), colors.text);

    BeginClip(D2D1::RectF(left + 12.0f, top + 50.0f, left + kPanelWidth - 12.0f,
                          top + kPanelHeight - 12.0f));
    float y = top + 52.0f;
    constexpr float kRowHeight = 30.0f;
    const std::wstring currentCategory = [&]() {
        const data::BookRecord* record = library_.Find(shelfCategoryPickerBookId_);
        return record != nullptr ? record->categoryId : std::wstring();
    }();

    const auto drawRow = [&](const std::wstring& label, const std::wstring& value, float indent) {
        const D2D1_RECT_F rowRect =
            D2D1::RectF(left + 16.0f, y, left + kPanelWidth - 16.0f, y + kRowHeight);
        const bool active = currentCategory == value;
        if (active) {
            renderer_.FillRoundedRect(rowRect, 6.0f, colors.surfaceAlt);
        }
        renderer_.DrawTextBlock(util::Ellipsize(label, 20),
                                D2D1::RectF(rowRect.left + indent, rowRect.top + 5.0f,
                                            rowRect.right - 6.0f, rowRect.bottom - 5.0f),
                                formats.listItem.Get(), active ? colors.accent : colors.text);
        AddHit(rowRect, HitKind::CategoryPickerItem, -1, value);
        y += kRowHeight;
    };

    drawRow(L"未分类", std::wstring(), 10.0f);
    struct TreeNode {
        std::wstring parentId;
        int depth;
    };
    std::vector<TreeNode> queue;
    queue.push_back({std::wstring(), 0});
    size_t head = 0;
    while (head < queue.size()) {
        const TreeNode node = queue[head++];
        for (const data::Category& category : library_.Categories()) {
            if (category.parentId != node.parentId) {
                continue;
            }
            drawRow(category.name, category.id, 10.0f + node.depth * 16.0f);
            queue.push_back({category.id, node.depth + 1});
        }
    }
    EndClip();
}

void Application::CreateCategoryFromDraft() {
    std::wstring name = TrimNoteText(categoryDraft_);
    const std::wstring parentId =
        shelfFilterCategory_.empty() || shelfFilterCategory_ == kUncategorizedFilter
            ? std::wstring()
            : shelfFilterCategory_;
    if (name.empty()) {
        name = L"新建分类";
    }
    std::wstring error;
    if (!library_.AddCategory(name, parentId, error)) {
        ShowStatus(L"新建分类失败：" + error);
        return;
    }
    categoryDraft_.clear();
    ResetInputCaretToEnd();
    ShowStatus(L"已创建分类：" + name);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Application::RenameSelectedCategory() {
    const std::wstring name = TrimNoteText(categoryDraft_);
    if (shelfFilterCategory_.empty() || shelfFilterCategory_ == kUncategorizedFilter) {
        ShowStatus(L"请先在左侧选中一个分类");
        return;
    }
    if (name.empty()) {
        ShowStatus(L"请输入新的分类名称");
        return;
    }
    std::wstring error;
    if (!library_.RenameCategory(shelfFilterCategory_, name, error)) {
        ShowStatus(L"重命名失败：" + error);
        return;
    }
    categoryDraft_.clear();
    ResetInputCaretToEnd();
    ShowStatus(L"已重命名分类");
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Application::DeleteSelectedCategory() {
    if (shelfFilterCategory_.empty() || shelfFilterCategory_ == kUncategorizedFilter) {
        ShowStatus(L"请先在左侧选中一个分类");
        return;
    }
    const std::wstring message = L"要删除该分类吗？子分类会上移一级，书籍回到未分类。";
    if (MessageBoxW(hwnd_, message.c_str(), L"删除分类", MB_YESNO | MB_ICONQUESTION) != IDYES) {
        return;
    }
    std::wstring error;
    if (!library_.RemoveCategory(shelfFilterCategory_, error)) {
        ShowStatus(L"删除分类失败：" + error);
        return;
    }
    shelfFilterCategory_.clear();
    RefreshShelfOrder();
    ShowStatus(L"已删除分类");
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Application::DrawShelfSettings(const ThemeColors& colors) {
    if (!shelfSettingsVisible_) {
        return;
    }
    const TextFormats& formats = renderer_.Formats();
    // 半透明遮罩，突出设置浮层。
    renderer_.FillRect(D2D1::RectF(0.0f, 0.0f, widthDip_, heightDip_),
                       ParseHexColor(L"#000000", 0.35f));

    constexpr float kPanelWidth = 460.0f;
    constexpr float kPanelHeight = 520.0f;
    const float left = (widthDip_ - kPanelWidth) * 0.5f;
    const float top = (heightDip_ - kPanelHeight) * 0.5f;
    const D2D1_RECT_F panel =
        D2D1::RectF(left, top, left + kPanelWidth, top + kPanelHeight);
    renderer_.FillRoundedRect(panel, 12.0f, colors.surface);
    renderer_.StrokeRoundedRect(panel, 12.0f, colors.divider, 1.0f);

    // 标题区域留足 42 DIP，避免标题行高溢出遮挡下方内容。
    renderer_.DrawTextBlock(L"全局设置",
                            D2D1::RectF(left + 24.0f, top + 16.0f, left + kPanelWidth - 24.0f,
                                        top + 58.0f),
                            formats.title.Get(), colors.text);
    renderer_.DrawLine(D2D1::Point2F(left + 24.0f, top + 66.0f),
                       D2D1::Point2F(left + kPanelWidth - 24.0f, top + 66.0f),
                       colors.divider, 1.0f);
    renderer_.DrawTextBlock(
        L"勾选即关联到本程序，取消勾选即取消关联（仅当前用户）。",
        D2D1::RectF(left + 24.0f, top + 76.0f, left + kPanelWidth - 24.0f, top + 112.0f),
        formats.caption.Get(), colors.secondaryText);

    // 文件关联行：复选框勾选状态来自注册表，点击整行即切换。
    const auto drawAssociationRow = [&](float rowTop, const std::wstring& label, bool checked,
                                        HitKind kind) {
        const D2D1_RECT_F checkbox =
            D2D1::RectF(left + 24.0f, rowTop + 10.0f, left + 42.0f, rowTop + 28.0f);
        renderer_.FillRoundedRect(checkbox, 4.0f, checked ? colors.accent : colors.surfaceAlt);
        renderer_.StrokeRoundedRect(checkbox, 4.0f, checked ? colors.accent : colors.divider,
                                    1.0f);
        if (checked) {
            renderer_.DrawLine(D2D1::Point2F(checkbox.left + 4.0f, checkbox.top + 9.0f),
                               D2D1::Point2F(checkbox.left + 8.0f, checkbox.top + 13.0f),
                               colors.accentText, 2.0f);
            renderer_.DrawLine(D2D1::Point2F(checkbox.left + 8.0f, checkbox.top + 13.0f),
                               D2D1::Point2F(checkbox.left + 14.0f, checkbox.top + 5.0f),
                               colors.accentText, 2.0f);
        }
        renderer_.DrawTextBlock(label,
                                D2D1::RectF(checkbox.right + 10.0f, rowTop + 6.0f,
                                            left + kPanelWidth - 24.0f, rowTop + 34.0f),
                                formats.listItem.Get(), colors.text);
        AddHit(D2D1::RectF(left + 24.0f, rowTop, left + kPanelWidth - 24.0f, rowTop + 38.0f),
               kind);
    };
    drawAssociationRow(top + 118.0f, L".umd 文件",
                       IsFileExtensionAssociated(L".umd", kDocumentProgId),
                       HitKind::ToggleUmdAssociation);
    drawAssociationRow(top + 164.0f, L".txt 文件",
                       IsFileExtensionAssociated(L".txt", kTextProgId),
                       HitKind::ToggleTxtAssociation);

    renderer_.DrawLine(D2D1::Point2F(left + 24.0f, top + 218.0f),
                       D2D1::Point2F(left + kPanelWidth - 24.0f, top + 218.0f),
                       colors.divider, 1.0f);
    renderer_.DrawTextBlock(L"AI 接口",
                            D2D1::RectF(left + 24.0f, top + 226.0f,
                                        left + kPanelWidth - 24.0f, top + 246.0f),
                            formats.caption.Get(), colors.secondaryText);
    DrawButton(L"AI 接口设置",
               D2D1::RectF(left + 24.0f, top + 250.0f, left + 164.0f, top + 288.0f),
               HitKind::OpenAiSettings, colors, false, false);

    renderer_.DrawLine(D2D1::Point2F(left + 24.0f, top + 300.0f),
                       D2D1::Point2F(left + kPanelWidth - 24.0f, top + 300.0f),
                       colors.divider, 1.0f);
    renderer_.DrawTextBlock(L"快捷键",
                            D2D1::RectF(left + 24.0f, top + 308.0f,
                                        left + kPanelWidth - 24.0f, top + 328.0f),
                            formats.caption.Get(), colors.secondaryText);
    DrawButton(L"设置快捷键",
               D2D1::RectF(left + 24.0f, top + 334.0f, left + 164.0f, top + 372.0f),
               HitKind::OpenShortcutSettings, colors, false, false);

    renderer_.DrawLine(D2D1::Point2F(left + 24.0f, top + 388.0f),
                       D2D1::Point2F(left + kPanelWidth - 24.0f, top + 388.0f),
                       colors.divider, 1.0f);
    renderer_.DrawTextBlock(L"数据目录",
                            D2D1::RectF(left + 24.0f, top + 396.0f,
                                        left + kPanelWidth - 24.0f, top + 416.0f),
                            formats.caption.Get(), colors.secondaryText);
    DrawButton(L"打开数据目录",
               D2D1::RectF(left + 24.0f, top + 422.0f, left + 164.0f, top + 460.0f),
               HitKind::OpenDataFolder, colors, false, false);

    DrawButton(L"关闭",
               D2D1::RectF(left + kPanelWidth - 124.0f, top + 470.0f,
                           left + kPanelWidth - 24.0f, top + 508.0f),
               HitKind::ShelfSettingsClose, colors, false, false);
}

void Application::DrawCategoryManager(const ThemeColors& colors) {
    if (!categoryManagerVisible_) {
        return;
    }
    const TextFormats& formats = renderer_.Formats();
    renderer_.FillRect(D2D1::RectF(0.0f, 0.0f, widthDip_, heightDip_),
                       ParseHexColor(L"#000000", 0.35f));

    constexpr float kPanelWidth = 460.0f;
    constexpr float kPanelHeight = 300.0f;
    const float left = (widthDip_ - kPanelWidth) * 0.5f;
    const float top = (heightDip_ - kPanelHeight) * 0.5f;
    const D2D1_RECT_F panel =
        D2D1::RectF(left, top, left + kPanelWidth, top + kPanelHeight);
    renderer_.FillRoundedRect(panel, 12.0f, colors.surface);
    renderer_.StrokeRoundedRect(panel, 12.0f, colors.divider, 1.0f);

    renderer_.DrawTextBlock(L"分类管理",
                            D2D1::RectF(left + 24.0f, top + 16.0f, left + kPanelWidth - 24.0f,
                                        top + 58.0f),
                            formats.title.Get(), colors.text);
    renderer_.DrawLine(D2D1::Point2F(left + 24.0f, top + 66.0f),
                       D2D1::Point2F(left + kPanelWidth - 24.0f, top + 66.0f),
                       colors.divider, 1.0f);
    renderer_.DrawTextBlock(L"分类名称",
                            D2D1::RectF(left + 24.0f, top + 76.0f, left + kPanelWidth - 24.0f,
                                        top + 92.0f),
                            formats.caption.Get(), colors.secondaryText);

    const D2D1_RECT_F inputRect = CategoryNameInputRect();
    renderer_.FillRoundedRect(inputRect, 8.0f, colors.surfaceAlt);
    renderer_.StrokeRoundedRect(inputRect, 8.0f,
                                categoryNameFocused_ ? colors.accent : colors.divider,
                                categoryNameFocused_ ? 1.6f : 1.0f);
    const D2D1_RECT_F inputTextRect =
        D2D1::RectF(inputRect.left + 10.0f, inputRect.top + 8.0f, inputRect.right - 10.0f,
                    inputRect.bottom - 8.0f);
    renderer_.DrawTextBlock(categoryDraft_.empty() ? std::wstring(L"输入分类名称")
                                                   : categoryDraft_,
                            inputTextRect, formats.listItem.Get(),
                            categoryDraft_.empty() ? colors.secondaryText : colors.text);
    if (categoryNameFocused_) {
        DrawInputSelection(inputTextRect, formats.listItem.Get(), false, colors);
    }
    AddHit(inputRect, HitKind::CategoryNameInput);

    const float actionTop = top + 140.0f;
    DrawButton(L"新建分类",
               D2D1::RectF(left + 24.0f, actionTop, left + 152.0f, actionTop + 34.0f),
               HitKind::CategoryCreate, colors, true, false);
    DrawButton(L"重命名",
               D2D1::RectF(left + 162.0f, actionTop, left + 290.0f, actionTop + 34.0f),
               HitKind::CategoryRename, colors, false, false);
    DrawButton(L"删除",
               D2D1::RectF(left + 300.0f, actionTop, left + 396.0f, actionTop + 34.0f),
               HitKind::CategoryDelete, colors, false, false);

    std::wstring currentName;
    for (const data::Category& category : library_.Categories()) {
        if (category.id == shelfFilterCategory_) {
            currentName = category.name;
            break;
        }
    }
    renderer_.DrawTextBlock(
        currentName.empty() ? std::wstring(L"当前未选中分类；新建的分类会放在根级")
                            : std::wstring(L"当前分类：") + currentName,
        D2D1::RectF(left + 24.0f, top + 186.0f, left + kPanelWidth - 24.0f, top + 208.0f),
        formats.caption.Get(), colors.secondaryText);

    DrawButton(L"关闭",
               D2D1::RectF(left + kPanelWidth - 124.0f, top + 236.0f,
                           left + kPanelWidth - 24.0f, top + 274.0f),
               HitKind::CategoryManagerClose, colors, false, false);
}

void Application::DrawBookCard(const data::BookRecord& book, const D2D1_RECT_F& cardRect,
                               const ThemeColors& colors, int index) {
    const TextFormats& formats = renderer_.Formats();
    // 书架弹框打开时，弹框下方的书籍卡片不响应悬停，避免设置界面上移动鼠标时卡片被高亮。
    const bool overlayOpen = shelfSettingsVisible_ || categoryManagerVisible_ ||
                             aiSettingsVisible_ || !shelfCategoryPickerBookId_.empty();
    const bool hovered = !overlayOpen && IsHovered(cardRect);
    if (hovered) {
        renderer_.FillRoundedRect(cardRect, 12.0f, colors.surface);
    }

    const D2D1_RECT_F coverRect =
        D2D1::RectF(cardRect.left, cardRect.top + 8.0f, cardRect.right,
                    cardRect.top + 8.0f + 272.0f);
    renderer_.FillRect(coverRect, colors.surfaceAlt);

    ID2D1Bitmap* bitmap = CoverBitmap(book);
    if (bitmap) {
        renderer_.DrawBitmap(bitmap, coverRect, 1.0f);
    } else {
        std::wstring initial = L"书";
        if (!book.title.empty()) {
            initial = book.title.substr(0, 1);
        }
        renderer_.DrawTextBlock(initial, coverRect, formats.hero.Get(), colors.secondaryText);
    }
    renderer_.StrokeRoundedRect(coverRect, 6.0f, hovered ? colors.accent : colors.divider,
                                hovered ? 2.0f : 1.0f);

    if (!book.FileExists()) {
        const D2D1_RECT_F warningRect =
            D2D1::RectF(coverRect.left + 8.0f, coverRect.bottom - 32.0f, coverRect.right - 8.0f,
                        coverRect.bottom - 8.0f);
        renderer_.FillRoundedRect(warningRect, 6.0f, colors.surface);
        renderer_.DrawTextBlock(L"原文件缺失", warningRect, formats.buttonSmall.Get(),
                                colors.secondaryText);
    }

    const float titleTop = coverRect.bottom + 12.0f;
    renderer_.DrawTextBlock(book.title,
                            D2D1::RectF(cardRect.left, titleTop, cardRect.right, titleTop + 42.0f),
                            formats.label.Get(), colors.text);
    const std::wstring author = book.author.empty() ? L"未知作者" : book.author;
    renderer_.DrawTextBlock(util::Ellipsize(SanitizeLine(author), 12),
                            D2D1::RectF(cardRect.left, titleTop + 46.0f, cardRect.right - 52.0f,
                                        titleTop + 66.0f),
                            formats.caption.Get(), colors.secondaryText);

    const double ratio =
        book.contentChars > 0
            ? static_cast<double>(std::min(book.progressCharOffset, book.contentChars)) /
                  static_cast<double>(book.contentChars)
            : 0.0;
    const int percent = static_cast<int>(std::lround(ratio * 100.0));
    const std::wstring percentText = std::to_wstring(percent) + L"%";
    renderer_.DrawTextBlock(percentText,
                            D2D1::RectF(cardRect.right - 48.0f, titleTop + 46.0f, cardRect.right,
                                        titleTop + 66.0f),
                            formats.captionRight.Get(), colors.secondaryText);

    const D2D1_RECT_F barRect = D2D1::RectF(cardRect.left, cardRect.bottom - 20.0f,
                                            cardRect.right, cardRect.bottom - 16.0f);
    renderer_.FillRoundedRect(barRect, 2.0f, colors.surfaceAlt);
    if (percent > 0) {
        const float fillWidth =
            (barRect.right - barRect.left) * static_cast<float>(std::clamp(ratio, 0.0, 1.0));
        if (fillWidth > 0.0f) {
            renderer_.FillRoundedRect(
                D2D1::RectF(barRect.left, barRect.top, barRect.left + fillWidth, barRect.bottom),
                2.0f, colors.accent);
        }
    }

    AddHit(cardRect, HitKind::BookCard, index, book.id);

    if (hovered) {
        const D2D1_RECT_F removeRect = D2D1::RectF(cardRect.right - 32.0f, cardRect.top + 14.0f,
                                                    cardRect.right - 6.0f, cardRect.top + 40.0f);
        renderer_.FillRoundedRect(removeRect, 8.0f, colors.surface);
        renderer_.StrokeRoundedRect(removeRect, 8.0f, colors.divider, 1.0f);
        renderer_.DrawTextBlock(L"×", removeRect, formats.buttonSmall.Get(),
                                colors.secondaryText);
        AddHit(removeRect, HitKind::RemoveBook, index, book.id);
    }

    // 归类入口：打开分类选择浮层。
    const D2D1_RECT_F categoryRect = D2D1::RectF(cardRect.right - 56.0f,
                                                  cardRect.bottom - 30.0f, cardRect.right - 6.0f,
                                                  cardRect.bottom - 8.0f);
    renderer_.FillRoundedRect(categoryRect, 6.0f, colors.surface);
    renderer_.StrokeRoundedRect(categoryRect, 6.0f, colors.divider, 1.0f);
    renderer_.DrawTextBlock(L"归类", categoryRect, formats.buttonSmall.Get(),
                            colors.secondaryText);
    AddHit(categoryRect, HitKind::BookCategoryButton, index, book.id);
}

void Application::DrawEmptyShelf(const ThemeColors& colors) {
    const TextFormats& formats = renderer_.Formats();
    const float centerY = (kShelfToolbarHeight + heightDip_) / 2.0f - 40.0f;
    renderer_.DrawTextBlock(L"书架为空", D2D1::RectF(0.0f, centerY, widthDip_, centerY + 32.0f),
                            formats.centered.Get(), colors.secondaryText);
    renderer_.DrawTextBlock(L"点击右上角「添加书籍」导入 .umd 或 .txt 文件，或把文件直接拖入窗口",
                            D2D1::RectF(0.0f, centerY + 36.0f, widthDip_, centerY + 62.0f),
                            formats.centered.Get(), colors.secondaryText);
}

void Application::DrawReader(const ThemeColors& colors) {
    renderer_.Clear(colors.background);
    // 阅读区背景在工具栏、正文与面板之前绘制，作为最底层。
    DrawReaderBackground(colors);
    const TextFormats& formats = renderer_.Formats();

    renderer_.FillRect(D2D1::RectF(0.0f, 0.0f, widthDip_, kReaderToolbarHeight), colors.surface);
    renderer_.DrawLine(D2D1::Point2F(0.0f, kReaderToolbarHeight),
                       D2D1::Point2F(widthDip_, kReaderToolbarHeight), colors.divider, 1.0f);

    DrawButton(L"返回书架", D2D1::RectF(16.0f, 15.0f, 132.0f, 49.0f), HitKind::BackToShelf,
               colors, false, false);

    struct ToolbarItem {
        const wchar_t* label;
        HitKind kind;
        PanelMode mode;
    };
    const ToolbarItem items[] = {
        {L"目录", HitKind::OpenToc, PanelMode::Toc},
        {L"书签", HitKind::OpenBookmarks, PanelMode::Bookmarks},
        {L"检索", HitKind::OpenSearch, PanelMode::Search},
        {L"插图", HitKind::OpenImages, PanelMode::Images},
        {L"标注", HitKind::OpenGestureBar, PanelMode::None},
    };

    std::vector<ToolbarItem> visibleItems;
    const data::BookRecord* toolbarRecord = library_.Find(activeBookId_);
    const bool hasImages = toolbarRecord && !toolbarRecord->imageFiles.empty();
    for (const ToolbarItem& item : items) {
        if (item.kind == HitKind::OpenImages && !hasImages) {
            continue;
        }
        visibleItems.push_back(item);
    }

    // 章节标题始终在窗口内居中，宽度不足时只调整右侧选项栏。
    const float headingLeft = widthDip_ * 0.5f - 170.0f;
    const float headingRight = widthDip_ * 0.5f + 170.0f;
    constexpr float kButtonWidth = 60.0f;
    constexpr float kButtonHeight = 34.0f;
    constexpr float kButtonGap = 6.0f;
    const auto toolbarLeftFor = [&](size_t count) {
        if (count == 0) {
            return widthDip_ - 16.0f;
        }
        const float total = static_cast<float>(count) * kButtonWidth +
                            static_cast<float>(count - 1) * kButtonGap;
        return widthDip_ - 16.0f - total;
    };
    // 隐藏顺序：优先隐藏次要入口，目录与检索始终保留。
    const HitKind hideOrder[] = {HitKind::OpenImages, HitKind::OpenBookmarks,
                                 HitKind::OpenGestureBar};
    float toolbarLeft = toolbarLeftFor(visibleItems.size());
    for (const HitKind kind : hideOrder) {
        if (toolbarLeft >= headingRight + 12.0f) {
            break;
        }
        const auto target = std::find_if(visibleItems.begin(), visibleItems.end(),
                                         [kind](const ToolbarItem& item) {
                                             return item.kind == kind;
                                         });
        if (target == visibleItems.end()) {
            continue;
        }
        visibleItems.erase(target);
        toolbarLeft = toolbarLeftFor(visibleItems.size());
    }

    // 顶部标题显示当前章节并保持居中对齐，超出宽度时由文本格式的省略号截断。
    const std::wstring chapterHeading =
        SanitizeLine(ChapterTitleAt(activeBook_, activeChapter_));
    renderer_.DrawTextBlock(chapterHeading,
                            D2D1::RectF(headingLeft, 18.0f, headingRight, 46.0f),
                            formats.centered.Get(), colors.text);

    float left = toolbarLeft;
    for (const ToolbarItem& item : visibleItems) {
        bool active = panel_ == item.mode;
        if (item.kind == HitKind::OpenGestureBar) {
            active = gestureBarVisible_;
        } else if (item.kind == HitKind::ToggleSpeech) {
            active = speaking_;
        }
        // 朗读按钮在朗读进行中显示「停止」，便于直接中断。
        const std::wstring label = item.kind == HitKind::ToggleSpeech
                                       ? (speaking_ ? std::wstring(L"停止") : std::wstring(L"朗读"))
                                       : std::wstring(item.label);
        DrawButton(label,
                   D2D1::RectF(left, 15.0f, left + kButtonWidth, 15.0f + kButtonHeight),
                   item.kind, colors, false, active);
        left += kButtonWidth + kButtonGap;
    }

    float textWidth = 0.0f;
    float textHeight = 0.0f;
    D2D1_RECT_F textRect{};
    ComputeTextMetrics(textWidth, textHeight, textRect);

    if (!statusText_.empty()) {
        renderer_.DrawTextBlock(util::Ellipsize(statusText_, 70),
                                D2D1::RectF(64.0f, kReaderToolbarHeight + 4.0f, widthDip_ - 64.0f,
                                            kReaderToolbarHeight + 24.0f),
                                formats.caption.Get(), colors.accent);
    }

    // 复用已缓存的正文布局：只有滚动位置、排版宽度或搜索高亮变化时才重建，
    // 避免每次重绘都为同一段文本重新创建布局。
    if (pager_.PageCount() > 0) {
        if (EnsurePageLayout() && pageLayout_ && pageLayoutEnd_ > pageLayoutStart_) {
            const uint32_t start = pageLayoutStart_;
            const uint32_t end = pageLayoutEnd_;
            const std::wstring pageText = chapterDisplay_.Text().substr(start, end - start);
            ApplySearchHighlight(pageLayout_.Get(), pageText, colors);
            renderer_.PushClip(textRect);
            DrawHighlights(pageLayout_.Get(), start, end, textRect, colors);
            DrawSelection(pageLayout_.Get(), start, end, textRect, colors);
            renderer_.DrawLayout(pageLayout_.Get(), D2D1::Point2F(textRect.left, textRect.top),
                                 colors.text);
            renderer_.PopClip();

            const D2D1_RECT_F noteClip =
                D2D1::RectF(textRect.left - 10.0f, textRect.top - 10.0f, textRect.right + 10.0f,
                            textRect.bottom + 10.0f);
            renderer_.PushClip(noteClip);
            DrawNotes(pageLayout_.Get(), start, end, textRect, colors);
            renderer_.PopClip();
        } else {
            renderer_.DrawTextBlock(L"（本章没有可显示的正文）",
                                    D2D1::RectF(textRect.left, textRect.top, textRect.right,
                                                textRect.top + 40.0f),
                                    formats.listItem.Get(), colors.secondaryText);
        }
    }

    renderer_.DrawLine(D2D1::Point2F(0.0f, heightDip_ - kReaderFooterHeight),
                       D2D1::Point2F(widthDip_, heightDip_ - kReaderFooterHeight), colors.divider,
                       1.0f);
    // 底部文字都避开居中的 AI 对话入口，窄窗口下也不会相互重叠。
    const D2D1_RECT_F chatEntry = AiChatEntryRect();
    const float footerGap = 12.0f;
    // 左下角显示书名。
    const std::wstring bookFooter =
        activeBook_.title.empty() ? std::wstring(L"未命名书籍")
                                  : util::Ellipsize(SanitizeLine(activeBook_.title), 56);
    const float titleRight = std::max(160.0f, chatEntry.left - footerGap);
    renderer_.DrawTextBlock(bookFooter,
                            D2D1::RectF(64.0f, heightDip_ - 32.0f, titleRight,
                                        heightDip_ - 10.0f),
                            formats.caption.Get(), colors.secondaryText);

    const size_t pageCount = std::max<size_t>(pager_.PageCount(), 1);
    // 页码以视口最上面一行所在的页为准，避免行滚动时页码与实际显示不一致。
    const size_t pageIndex =
        pager_.PageCount() > 0
            ? std::min(pager_.PageIndexForChar(ViewportStartOffset()), pager_.PageCount() - 1)
            : 0;
    const uint32_t offset = CurrentGlobalOffset();
    const uint32_t totalChars = activeBook_.ContentCharCount();
    const int percent = totalChars > 0
                            ? static_cast<int>(
                                  std::lround(static_cast<double>(offset) * 100.0 / totalChars))
                            : 0;
    const std::wstring pageText = L"第 " + std::to_wstring(pageIndex + 1) + L" / " +
                                  std::to_wstring(pageCount) + L" 页 · " +
                                  std::to_wstring(percent) + L"%";
    const float pageLeft = std::max(widthDip_ - 392.0f, chatEntry.right + footerGap);
    renderer_.DrawTextBlock(pageText,
                            D2D1::RectF(pageLeft, heightDip_ - 32.0f,
                                        widthDip_ - 172.0f, heightDip_ - 10.0f),
                            formats.captionRight.Get(), colors.secondaryText);

    // 右下角显示阅读视图比例，便于按视图比例选择背景图片。
    const float viewWidth = widthDip_ - (panel_ != PanelMode::None ? PanelWidth() : 0.0f);
    const float viewHeight =
        std::max(1.0f, heightDip_ - kReaderToolbarHeight - kReaderFooterHeight);
    renderer_.DrawTextBlock(ViewRatioLabel(viewWidth, viewHeight),
                            D2D1::RectF(widthDip_ - 160.0f, heightDip_ - 32.0f,
                                        widthDip_ - 64.0f, heightDip_ - 10.0f),
                            formats.captionRight.Get(), colors.secondaryText);

    // 阅读区底部居中：阅读助手入口。
    DrawButton(L"阅读助手", AiChatEntryRect(), HitKind::OpenChat, colors, false,
               panel_ == PanelMode::Chat);

    DrawReaderScrollbar(colors);
    DrawGestureBar(colors);
    if (colorPickerVisible_) {
        DrawColorPicker(colors);
    }
    DrawReaderPanel(colors);
    DrawAiSettings(colors);
    DrawFontPicker(colors);
    DrawChatSessionPicker(colors);
    DrawShortcutSettings(colors);
    DrawCopyToast(colors);
    DrawInputCaret(colors);
}

void Application::DrawReaderBackground(const ThemeColors& colors) {
    const data::ReaderSettings& settings = library_.Settings();
    const D2D1_RECT_F fullRect = D2D1::RectF(0.0f, 0.0f, widthDip_, heightDip_);
    if (settings.backgroundType == L"color" && !settings.backgroundColor.empty()) {
        renderer_.FillRect(fullRect, ParseHexColor(settings.backgroundColor, 1.0f));
    } else if (settings.backgroundType == L"image") {
        // 图片保持比例覆盖整个阅读视图，超出部分居中裁切。
        ID2D1Bitmap* bitmap = BackgroundBitmap();
        if (!bitmap) {
            return;
        }
        renderer_.DrawBitmap(bitmap, fullRect, 1.0f);
        // 图片上再铺一层主题底色的半透明衬底，保证正文对比度：
        // 正文随后绘制，始终位于背景图片之上。
        float textWidth = 0.0f;
        float textHeight = 0.0f;
        D2D1_RECT_F textRect{};
        ComputeTextMetrics(textWidth, textHeight, textRect);
        const D2D1_COLOR_F base = colors.background;
        renderer_.FillRect(textRect, D2D1::ColorF(base.r, base.g, base.b, 0.72f));
    }
}

std::vector<Application::MenuEntry> Application::ReaderMenuEntries(float x, float y) {
    std::vector<MenuEntry> entries;
    if (panel_ == PanelMode::Toc) {
        // 目录右键：只在目录面板范围内响应，按不同规则重新提取纯文本章节，最多三级（集 → 章 → 节）。
        const D2D1_RECT_F panelRect =
            D2D1::RectF(widthDip_ - PanelWidth(), kReaderToolbarHeight, widthDip_, heightDip_);
        const data::BookRecord* record = library_.Find(activeBookId_);
        if (PointInside(panelRect, x, y) && record != nullptr && record->formatType == 0) {
            const std::wstring current = record->chapterMode.empty() ? L"auto" : record->chapterMode;
            const auto labelFor = [&current](const wchar_t* mode, const wchar_t* base) {
                std::wstring label = base;
                if (current == mode) {
                    label += L"（当前）";
                }
                return label;
            };
            entries.push_back(MenuEntry{L"chapters-auto", labelFor(L"auto", L"自动识别章节"), -1,
                                        std::wstring()});
            entries.push_back(MenuEntry{L"chapters-hui", labelFor(L"hui", L"按「回」提取"), -1,
                                        std::wstring()});
            entries.push_back(MenuEntry{L"chapters-zhang", labelFor(L"zhang", L"按「章」提取"), -1,
                                        std::wstring()});
            entries.push_back(MenuEntry{L"chapters-jie", labelFor(L"jie", L"按「节」提取"), -1,
                                        std::wstring()});
            entries.push_back(MenuEntry{L"chapters-jizhang",
                                        labelFor(L"jizhang", L"按「集」「章」提取"), -1,
                                        std::wstring()});
            entries.push_back(MenuEntry{L"chapters-jizhangjie",
                                        labelFor(L"jizhangjie", L"按「集」「章」「节」提取"), -1,
                                        std::wstring()});
            return entries;
        }
    }
    // 右键菜单只保留「复制」，避免分散阅读注意力。
    if (panel_ == PanelMode::Chat && PointInside(ChatConversationRect(), x, y)) {
        const uint32_t begin = std::min(chatSelectionStart_, chatSelectionEnd_);
        const uint32_t end = std::max(chatSelectionStart_, chatSelectionEnd_);
        const data::ChatSession* session = ActiveChatSession();
        if (session != nullptr && chatSelectionMessage_ >= 0 &&
            chatSelectionMessage_ < static_cast<int>(session->messages.size()) &&
            begin < end) {
            entries.push_back(MenuEntry{L"copy-chat-selection", L"复制", -1,
                                        std::wstring()});
        }
        return entries;
    }
    if (selectionActive_) {
        entries.push_back(MenuEntry{L"copy-selection", L"复制", -1, std::wstring()});
    }
    return entries;
}

std::vector<Application::MenuEntry> Application::ShelfMenuEntries() {
    return {MenuEntry{L"shelf-add", L"添加书籍", -1, std::wstring()},
            MenuEntry{L"shelf-settings", L"全局设置", -1, std::wstring()},
            MenuEntry{L"shelf-categories", L"分类管理", -1, std::wstring()},
            MenuEntry{L"shelf-data", L"打开数据目录", -1, std::wstring()}};
}

void Application::OpenMenu(float x, float y, std::vector<MenuEntry> entries) {
    if (entries.empty()) {
        return;
    }
    menuEntries_ = std::move(entries);
    menuOrigin_ = D2D1::Point2F(x, y);
    menuVisible_ = true;
    InvalidateRect(hwnd_, nullptr, FALSE);
    // 菜单项命中区域需要立即可用。
    UpdateWindow(hwnd_);
}

void Application::CloseMenu() {
    if (!menuVisible_) {
        return;
    }
    menuVisible_ = false;
    menuEntries_.clear();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

D2D1_RECT_F Application::MenuRect() const {
    const float height =
        kMenuPadding * 2.0f + static_cast<float>(menuEntries_.size()) * kMenuItemHeight;
    float left = std::clamp(menuOrigin_.x, 8.0f, std::max(8.0f, widthDip_ - kMenuWidth - 8.0f));
    const float topLimit = view_ == ViewMode::Reader ? kReaderToolbarHeight + 4.0f : 8.0f;
    float top = std::clamp(menuOrigin_.y, topLimit,
                           std::max(topLimit, heightDip_ - height - 8.0f));
    return D2D1::RectF(left, top, left + kMenuWidth, top + height);
}

void Application::DrawMenu(const ThemeColors& colors) {
    if (!menuVisible_ || menuEntries_.empty()) {
        return;
    }
    const TextFormats& formats = renderer_.Formats();
    const D2D1_RECT_F rect = MenuRect();
    renderer_.FillRoundedRect(rect, 8.0f, colors.surface);
    renderer_.StrokeRoundedRect(rect, 8.0f, colors.divider, 1.0f);
    float top = rect.top + kMenuPadding;
    for (size_t i = 0; i < menuEntries_.size(); ++i) {
        const D2D1_RECT_F itemRect =
            D2D1::RectF(rect.left + 4.0f, top, rect.right - 4.0f, top + kMenuItemHeight);
        if (IsHovered(itemRect)) {
            renderer_.FillRoundedRect(itemRect, 6.0f, colors.surfaceAlt);
        }
        renderer_.DrawTextBlock(menuEntries_[i].label,
                                D2D1::RectF(itemRect.left + 12.0f, itemRect.top,
                                            itemRect.right - 10.0f, itemRect.bottom),
                                formats.listItem.Get(), colors.text);
        AddHit(itemRect, HitKind::MenuItem, static_cast<int>(i));
        top += kMenuItemHeight;
    }
}

void Application::ExecuteMenuEntry(const MenuEntry& entry) {
    if (entry.id == L"copy-selection") {
        CopySelection();
    } else if (entry.id == L"copy-chat-selection") {
        CopyChatSelection();
    } else if (entry.id == L"chapters-auto") {
        ApplyChapterMode(L"auto");
    } else if (entry.id == L"chapters-hui") {
        ApplyChapterMode(L"hui");
    } else if (entry.id == L"chapters-zhang") {
        ApplyChapterMode(L"zhang");
    } else if (entry.id == L"chapters-jie") {
        ApplyChapterMode(L"jie");
    } else if (entry.id == L"chapters-jizhang") {
        ApplyChapterMode(L"jizhang");
    } else if (entry.id == L"chapters-jizhangjie") {
        ApplyChapterMode(L"jizhangjie");
    } else if (entry.id == L"shelf-add") {
        OpenFileDialog();
    } else if (entry.id == L"shelf-settings") {
        shelfSettingsVisible_ = true;
        categoryManagerVisible_ = false;
        InvalidateRect(hwnd_, nullptr, FALSE);
    } else if (entry.id == L"shelf-categories") {
        categoryManagerVisible_ = true;
        shelfSettingsVisible_ = false;
        InvalidateRect(hwnd_, nullptr, FALSE);
    } else if (entry.id == L"shelf-data") {
        const std::wstring directory = library_.DataDirectory().wstring();
        ShellExecuteW(hwnd_, L"open", directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Application::ApplyChapterMode(const std::wstring& mode) {
    data::BookRecord* record = library_.Find(activeBookId_);
    if (record == nullptr || activeBook_.formatType != 0) {
        ShowStatus(L"仅纯文本文件支持重新提取章节");
        return;
    }
    const std::wstring normalized = mode.empty() ? L"auto" : mode;
    if (record->chapterMode == normalized) {
        ShowStatus(L"当前已使用该章节提取方式");
        return;
    }
    std::vector<umd::Chapter> chapters =
        normalized == L"auto"
            ? umd::DetectDefaultChapters(activeBook_.content)
            : umd::DetectChaptersByUnits(activeBook_.content, umd::ChapterUnitsForMode(normalized));
    if (chapters.empty()) {
        ShowStatus(L"未识别到符合条件的章节");
        return;
    }

    const uint32_t globalOffset = CurrentGlobalOffset();
    const std::vector<umd::Chapter> previous = activeBook_.chapters;
    // 旧章节内的偏移先换算为全书偏移，再定位到新章节，保证书签、高亮、标注不丢失。
    const auto relocate = [&chapters](uint32_t global, int& chapterIndex, uint32_t& localOffset) {
        chapterIndex = 0;
        for (size_t i = 0; i < chapters.size(); ++i) {
            if (chapters[i].charOffset <= global) {
                chapterIndex = static_cast<int>(i);
            } else {
                break;
            }
        }
        const uint32_t base = chapters[static_cast<size_t>(chapterIndex)].charOffset;
        localOffset = global > base ? global - base : 0;
    };
    const auto previousBase = [&previous](int chapterIndex) {
        if (chapterIndex < 0 || chapterIndex >= static_cast<int>(previous.size())) {
            return static_cast<uint32_t>(0);
        }
        return previous[static_cast<size_t>(chapterIndex)].charOffset;
    };

    for (data::Highlight& highlight : record->highlights) {
        int chapterIndex = 0;
        uint32_t localOffset = 0;
        relocate(previousBase(highlight.chapterIndex) + highlight.start, chapterIndex, localOffset);
        highlight.chapterIndex = chapterIndex;
        highlight.start = localOffset;
    }
    for (data::Note& note : record->notes) {
        int chapterIndex = 0;
        uint32_t localOffset = 0;
        relocate(previousBase(note.chapterIndex) + note.start, chapterIndex, localOffset);
        note.chapterIndex = chapterIndex;
        note.start = localOffset;
    }
    for (data::Bookmark& bookmark : record->bookmarks) {
        int chapterIndex = 0;
        uint32_t localOffset = 0;
        relocate(previousBase(bookmark.chapterIndex) + bookmark.charOffset, chapterIndex,
                 localOffset);
        bookmark.chapterIndex = chapterIndex;
        bookmark.charOffset = localOffset;
    }

    activeBook_.chapters = std::move(chapters);
    record->chapters.clear();
    for (const umd::Chapter& chapter : activeBook_.chapters) {
        data::ChapterInfo info;
        info.title = chapter.title;
        info.charOffset = chapter.charOffset;
        info.charLength = chapter.charLength;
        record->chapters.push_back(std::move(info));
    }
    record->chapterMode = normalized;

    if (!searchResults_.empty()) {
        searchResults_.clear();
        searchStatus_ = L"章节已重新提取，请重新检索";
    }

    std::wstring saveError;
    library_.SaveBook(activeBookId_, saveError);
    ClearLayoutCache();
    InvalidatePageCache();
    activeChapterRaw_.clear();
    activeChapterRawChapter_ = -1;
    RebuildPagination(globalOffset);
    ShowStatus(L"已重新提取 " + std::to_wstring(activeBook_.chapters.size()) + L" 个章节");
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Application::OnRightButtonDown(int xPixels, int yPixels) {
    const float scale = renderer_.DpiScale();
    const float x = static_cast<float>(xPixels) / scale;
    const float y = static_cast<float>(yPixels) / scale;
    // 弹框打开时不响应右键菜单。
    if (chatSessionPickerVisible_ || fontPickerVisible_ || aiSettingsVisible_ ||
        shortcutSettingsVisible_) {
        return;
    }
    if (view_ == ViewMode::Shelf &&
        (shelfSettingsVisible_ || categoryManagerVisible_ || !shelfCategoryPickerBookId_.empty())) {
        return;
    }
    if (menuVisible_) {
        CloseMenu();
        return;
    }
    if (view_ == ViewMode::Reader) {
        OpenMenu(x, y, ReaderMenuEntries(x, y));
    } else {
        OpenMenu(x, y, ShelfMenuEntries());
    }
}

void Application::DrawReaderPanel(const ThemeColors& colors) {
    if (panel_ == PanelMode::None) {
        return;
    }
    const TextFormats& formats = renderer_.Formats();
    const float left = widthDip_ - PanelWidth();
    const D2D1_RECT_F panelRect =
        D2D1::RectF(left, kReaderToolbarHeight, widthDip_, heightDip_);
    renderer_.FillRect(panelRect, colors.surface);
    renderer_.DrawLine(D2D1::Point2F(left, kReaderToolbarHeight),
                       D2D1::Point2F(left, heightDip_), colors.divider, 1.0f);
    // 阅读助手面板左侧分割条：拖动可调整面板宽度。
    if (panel_ == PanelMode::Chat) {
        const D2D1_RECT_F splitter =
            D2D1::RectF(left - 5.0f, kReaderToolbarHeight, left + 5.0f, heightDip_);
        if (chatPanelResizing_ || IsHovered(splitter)) {
            renderer_.FillRoundedRect(D2D1::RectF(left - 1.5f, kReaderToolbarHeight + 24.0f,
                                                  left + 1.5f, heightDip_ - 24.0f),
                                      1.5f, colors.accent);
        }
        AddHit(splitter, HitKind::ChatPanelSplitter);
    }

    std::wstring title;
    switch (panel_) {
    case PanelMode::Toc:
        title = L"目录 · " + std::to_wstring(activeBook_.chapters.size()) + L" 章";
        break;
    case PanelMode::Bookmarks: {
        const data::BookRecord* record = library_.Find(activeBookId_);
        const size_t count = record ? record->bookmarks.size() : 0;
        title = L"书签 · " + std::to_wstring(count) + L" 条";
        break;
    }
    case PanelMode::Search:
        title = L"全文检索";
        break;
    case PanelMode::Settings:
        title = L"阅读设置";
        break;
    case PanelMode::Images: {
        const data::BookRecord* record = library_.Find(activeBookId_);
        const size_t count = record ? record->imageFiles.size() : 0;
        title = L"插图 · " + std::to_wstring(count) + L" 张";
        break;
    }
    case PanelMode::NoteEditor:
        title = L"添加标注";
        break;
    case PanelMode::Chat:
        title = L"阅读助手";
        break;
    default:
        return;
    }

    renderer_.DrawTextBlock(title,
                            D2D1::RectF(left + 24.0f, kReaderToolbarHeight + 16.0f,
                                        widthDip_ - 70.0f, kReaderToolbarHeight + 46.0f),
                            formats.section.Get(), colors.text);
    DrawButton(L"×", D2D1::RectF(widthDip_ - 46.0f, kReaderToolbarHeight + 14.0f,
                                 widthDip_ - 14.0f, kReaderToolbarHeight + 46.0f),
               HitKind::ClosePanel, colors, false, false);

    const D2D1_RECT_F contentRect =
        D2D1::RectF(left + 20.0f, kReaderToolbarHeight + kPanelHeaderHeight, widthDip_ - 20.0f,
                    heightDip_ - 20.0f);
    switch (panel_) {
    case PanelMode::Toc:
        DrawTocPanel(colors, contentRect);
        break;
    case PanelMode::Bookmarks:
        DrawBookmarksPanel(colors, contentRect);
        break;
    case PanelMode::Search:
        DrawSearchPanel(colors, contentRect);
        break;
    case PanelMode::Settings:
        DrawSettingsPanel(colors, contentRect);
        break;
    case PanelMode::Images:
        DrawImagesPanel(colors, contentRect);
        break;
    case PanelMode::NoteEditor:
        DrawNoteEditorPanel(colors, contentRect);
        break;
    case PanelMode::Chat:
        DrawChatPanel(colors, contentRect);
        break;
    default:
        break;
    }
}

void Application::DrawTocPanel(const ThemeColors& colors, const D2D1_RECT_F& contentRect) {
    const TextFormats& formats = renderer_.Formats();
    const float itemHeight = 46.0f;
    const float viewportHeight = contentRect.bottom - contentRect.top;
    const float totalHeight = activeBook_.chapters.size() * itemHeight;
    panelScrollMax_ = std::max(0.0f, totalHeight - viewportHeight);
    panelScroll_ = std::clamp(panelScroll_, 0.0f, panelScrollMax_);

    BeginClip(contentRect);
    float y = contentRect.top - panelScroll_;
    for (size_t i = 0; i < activeBook_.chapters.size(); ++i) {
        const D2D1_RECT_F itemRect =
            D2D1::RectF(contentRect.left, y, contentRect.right, y + itemHeight);
        if (itemRect.bottom > contentRect.top && itemRect.top < contentRect.bottom) {
            const bool current = static_cast<int>(i) == activeChapter_;
            if (current) {
                renderer_.FillRoundedRect(itemRect, 8.0f, colors.surfaceAlt);
                renderer_.FillRect(D2D1::RectF(itemRect.left, itemRect.top + 10.0f,
                                               itemRect.left + 3.0f, itemRect.bottom - 10.0f),
                                   colors.accent);
            } else if (IsHovered(itemRect)) {
                renderer_.FillRoundedRect(itemRect, 8.0f, Darken(colors.surfaceAlt, 1.02f));
            }
            std::wstring title = ChapterTitleAt(activeBook_, static_cast<int>(i));
            title = util::Ellipsize(SanitizeLine(title), 32);
            renderer_.DrawTextBlock(title,
                                    D2D1::RectF(itemRect.left + 14.0f, itemRect.top + 13.0f,
                                                itemRect.right - 10.0f, itemRect.bottom - 10.0f),
                                    formats.listItem.Get(),
                                    current ? colors.accent : colors.text);
            AddHit(itemRect, HitKind::ChapterItem, static_cast<int>(i));
        }
        y += itemHeight;
    }
    EndClip();
    DrawPanelScrollbar(colors);
}

void Application::DrawBookmarksPanel(const ThemeColors& colors, const D2D1_RECT_F& contentRect) {
    const TextFormats& formats = renderer_.Formats();
    const data::BookRecord* record = library_.Find(activeBookId_);

    const D2D1_RECT_F addRect =
        D2D1::RectF(contentRect.left, contentRect.top, contentRect.left + 168.0f,
                    contentRect.top + 36.0f);
    DrawButton(L"添加当前页书签", addRect, HitKind::AddBookmark, colors, true, false);
    renderer_.DrawTextBlock(L"快捷键 Ctrl+D",
                            D2D1::RectF(contentRect.left + 180.0f, contentRect.top + 8.0f,
                                        contentRect.right, contentRect.top + 30.0f),
                            formats.caption.Get(), colors.secondaryText);

    const float listTop = contentRect.top + 48.0f;
    if (!record || record->bookmarks.empty()) {
        renderer_.DrawTextBlock(L"尚无书签。阅读时可用上方按钮或 Ctrl+D 添加当前页书签。",
                                D2D1::RectF(contentRect.left, listTop, contentRect.right,
                                            listTop + 48.0f),
                                formats.caption.Get(), colors.secondaryText);
        panelScrollMax_ = 0.0f;
        panelScroll_ = 0.0f;
        return;
    }

    const float itemHeight = 68.0f;
    const float totalHeight = record->bookmarks.size() * itemHeight;
    panelScrollMax_ = std::max(0.0f, totalHeight - (contentRect.bottom - listTop));
    panelScroll_ = std::clamp(panelScroll_, 0.0f, panelScrollMax_);

    const D2D1_RECT_F listRect =
        D2D1::RectF(contentRect.left, listTop, contentRect.right, contentRect.bottom);
    BeginClip(listRect);
    float y = listTop - panelScroll_;
    for (size_t i = 0; i < record->bookmarks.size(); ++i) {
        const data::Bookmark& bookmark = record->bookmarks[i];
        const D2D1_RECT_F itemRect =
            D2D1::RectF(listRect.left, y, listRect.right, y + itemHeight);
        if (itemRect.bottom > listRect.top && itemRect.top < listRect.bottom) {
            if (IsHovered(itemRect)) {
                renderer_.FillRoundedRect(itemRect, 8.0f, colors.surfaceAlt);
            }
            const std::wstring chapter = util::Ellipsize(
                SanitizeLine(ChapterTitleAt(activeBook_, bookmark.chapterIndex)), 26);
            renderer_.DrawTextBlock(chapter,
                                    D2D1::RectF(itemRect.left + 12.0f, itemRect.top + 8.0f,
                                                itemRect.right - 60.0f, itemRect.top + 28.0f),
                                    formats.caption.Get(), colors.secondaryText);
            renderer_.DrawTextBlock(util::Ellipsize(bookmark.title, 40),
                                    D2D1::RectF(itemRect.left + 12.0f, itemRect.top + 30.0f,
                                                itemRect.right - 60.0f, itemRect.top + 52.0f),
                                    formats.listItem.Get(), colors.text);
            const D2D1_RECT_F removeRect =
                D2D1::RectF(itemRect.right - 54.0f, itemRect.top + 18.0f, itemRect.right - 10.0f,
                            itemRect.top + 44.0f);
            renderer_.FillRoundedRect(removeRect, 7.0f, colors.surfaceAlt);
            renderer_.DrawTextBlock(L"删除", removeRect, formats.buttonSmall.Get(),
                                    colors.secondaryText);
            AddHit(itemRect, HitKind::BookmarkItem, static_cast<int>(i));
            AddHit(removeRect, HitKind::RemoveBookmark, static_cast<int>(i));
        }
        y += itemHeight;
    }
    EndClip();
    DrawPanelScrollbar(colors);
}

void Application::DrawSearchPanel(const ThemeColors& colors, const D2D1_RECT_F& contentRect) {
    const TextFormats& formats = renderer_.Formats();
    const D2D1_RECT_F boxRect = SearchBoxRect();
    renderer_.FillRoundedRect(boxRect, 9.0f, colors.surfaceAlt);
    renderer_.StrokeRoundedRect(boxRect, 9.0f, searchFocused_ ? colors.accent : colors.divider,
                               searchFocused_ ? 1.6f : 1.0f);
    const std::wstring display =
        searchQuery_.empty() ? L"输入关键词，回车检索" : searchQuery_;
    renderer_.DrawTextBlock(display,
                            D2D1::RectF(boxRect.left + kSearchTextPaddingX,
                                        boxRect.top + kSearchTextPaddingTop,
                                        boxRect.right - kSearchTextPaddingX,
                                        boxRect.bottom - kSearchTextPaddingBottom),
                            formats.listItem.Get(),
                            searchQuery_.empty() ? colors.secondaryText : colors.text);
    if (searchFocused_) {
        DrawInputSelection(D2D1::RectF(boxRect.left + kSearchTextPaddingX,
                                       boxRect.top + kSearchTextPaddingTop,
                                       boxRect.right - kSearchTextPaddingX,
                                       boxRect.bottom - kSearchTextPaddingBottom),
                           formats.listItem.Get(), false, colors);
    }
    AddHit(boxRect, HitKind::SearchBox);

    float y = boxRect.bottom + 8.0f;
    if (!searchStatus_.empty()) {
        renderer_.DrawTextBlock(searchStatus_,
                                D2D1::RectF(contentRect.left, y, contentRect.right, y + 20.0f),
                                formats.caption.Get(), colors.secondaryText);
    }
    y += 26.0f;

    const float itemHeight = 76.0f;
    const float listTop = y;
    const float viewportHeight = contentRect.bottom - listTop;
    const float totalHeight = searchResults_.size() * itemHeight;
    panelScrollMax_ = std::max(0.0f, totalHeight - viewportHeight);
    panelScroll_ = std::clamp(panelScroll_, 0.0f, panelScrollMax_);

    if (searchResults_.empty()) {
        return;
    }

    const D2D1_RECT_F listRect =
        D2D1::RectF(contentRect.left, listTop, contentRect.right, contentRect.bottom);
    BeginClip(listRect);
    float itemTop = listTop - panelScroll_;
    for (size_t i = 0; i < searchResults_.size(); ++i) {
        const SearchResult& result = searchResults_[i];
        const D2D1_RECT_F itemRect =
            D2D1::RectF(listRect.left, itemTop, listRect.right, itemTop + itemHeight);
        if (itemRect.bottom > listRect.top && itemRect.top < listRect.bottom) {
            if (IsHovered(itemRect)) {
                renderer_.FillRoundedRect(itemRect, 8.0f, colors.surfaceAlt);
            }
            const std::wstring chapter = util::Ellipsize(
                SanitizeLine(ChapterTitleAt(activeBook_, result.chapterIndex)), 28);
            renderer_.DrawTextBlock(chapter,
                                    D2D1::RectF(itemRect.left + 10.0f, itemRect.top + 8.0f,
                                                itemRect.right - 10.0f, itemRect.top + 28.0f),
                                    formats.caption.Get(), colors.secondaryText);
            renderer_.DrawTextBlock(util::Ellipsize(result.context, 44),
                                    D2D1::RectF(itemRect.left + 10.0f, itemRect.top + 32.0f,
                                                itemRect.right - 10.0f, itemRect.top + 56.0f),
                                    formats.listItem.Get(), colors.text);
            AddHit(itemRect, HitKind::SearchItem, static_cast<int>(i));
        }
        itemTop += itemHeight;
    }
    EndClip();
}

void Application::DrawSettingsPanel(const ThemeColors& colors, const D2D1_RECT_F& contentRect) {
    const TextFormats& formats = renderer_.Formats();
    const data::ReaderSettings& settings = library_.Settings();
    const float left = contentRect.left;
    const float width = contentRect.right - contentRect.left;
    panelScroll_ = std::clamp(panelScroll_, 0.0f, panelScrollMax_);
    BeginClip(contentRect);
    float y = contentRect.top - panelScroll_;

    renderer_.DrawTextBlock(L"字体 · " + CurrentFontDisplayName(),
                            D2D1::RectF(left, y + 4.0f, left + width - 78.0f, y + 26.0f),
                            formats.caption.Get(), colors.secondaryText);
    // 「全部」打开字体弹框，列出全部可用字族；按钮整体落在行内，避免被滚动区域上边缘裁掉。
    DrawButton(L"全部", D2D1::RectF(left + width - 64.0f, y, left + width, y + 30.0f),
               HitKind::FontPickerOpen, colors, false, false);
    y += 36.0f;
    // 四个常用字体保留在设置页直接选择。
    const float presetGap = 8.0f;
    const float presetWidth = (width - presetGap * static_cast<float>(kFontOptionCount - 1)) /
                              static_cast<float>(kFontOptionCount);
    for (size_t i = 0; i < kFontOptionCount; ++i) {
        const float x = left + static_cast<float>(i) * (presetWidth + presetGap);
        DrawButton(kFontOptions[i].label,
                   D2D1::RectF(x, y, x + presetWidth, y + 32.0f), HitKind::FontPickerItem, colors,
                   false, settings.fontFamily == kFontOptions[i].family, -1,
                   kFontOptions[i].family);
    }
    y += 32.0f + 24.0f;

    renderer_.DrawTextBlock(L"字号", D2D1::RectF(left, y, left + width, y + 20.0f),
                            formats.caption.Get(), colors.secondaryText);
    y += 26.0f;
    DrawButton(L"−", D2D1::RectF(left, y, left + 36.0f, y + 34.0f), HitKind::FontSmaller, colors,
               false, false);
    renderer_.DrawTextBlock(std::to_wstring(static_cast<int>(std::lround(settings.fontSize))),
                            D2D1::RectF(left + 44.0f, y, left + 104.0f, y + 34.0f),
                            formats.centered.Get(), colors.text);
    DrawButton(L"＋", D2D1::RectF(left + 112.0f, y, left + 148.0f, y + 34.0f),
               HitKind::FontLarger, colors, false, false);
    y += 58.0f;

    renderer_.DrawTextBlock(L"行距", D2D1::RectF(left, y, left + width, y + 20.0f),
                            formats.caption.Get(), colors.secondaryText);
    y += 26.0f;
    DrawButton(L"−", D2D1::RectF(left, y, left + 36.0f, y + 34.0f), HitKind::SpacingSmaller,
               colors, false, false);
    wchar_t spacingText[16] = {};
    swprintf_s(spacingText, L"%.1f", settings.lineSpacing);
    renderer_.DrawTextBlock(spacingText,
                            D2D1::RectF(left + 44.0f, y, left + 104.0f, y + 34.0f),
                            formats.centered.Get(), colors.text);
    DrawButton(L"＋", D2D1::RectF(left + 112.0f, y, left + 148.0f, y + 34.0f),
               HitKind::SpacingLarger, colors, false, false);
    y += 58.0f;

    renderer_.DrawTextBlock(L"滚动方式", D2D1::RectF(left, y, left + width, y + 20.0f),
                            formats.caption.Get(), colors.secondaryText);
    y += 26.0f;
    const bool lineScroll = settings.readerScrollMode != L"page";
    DrawButton(L"按行滚动", D2D1::RectF(left, y, left + 128.0f, y + 34.0f),
               HitKind::ScrollModeLine, colors, false, lineScroll);
    DrawButton(L"按页翻页", D2D1::RectF(left + 138.0f, y, left + 266.0f, y + 34.0f),
               HitKind::ScrollModePage, colors, false, !lineScroll);
    y += 58.0f;

    renderer_.DrawTextBlock(L"主题", D2D1::RectF(left, y, left + width, y + 20.0f),
                            formats.caption.Get(), colors.secondaryText);
    y += 26.0f;
    const float themeWidth = 96.0f;
    const float themeGap = 8.0f;
    for (size_t i = 0; i < kThemeOptionCount; ++i) {
        const float x = left + static_cast<float>(i) * (themeWidth + themeGap);
        DrawButton(kThemeOptions[i].label, D2D1::RectF(x, y, x + themeWidth, y + 34.0f),
                   HitKind::ThemeOption, colors, false, settings.theme == kThemeOptions[i].name,
                   -1, kThemeOptions[i].name);
    }
    y += 58.0f;

    // 背景：预设纯色色块。
    renderer_.DrawTextBlock(L"背景", D2D1::RectF(left, y, left + width, y + 20.0f),
                            formats.caption.Get(), colors.secondaryText);
    y += 26.0f;

    const float swatchWidth = 54.0f;
    const float swatchHeight = 30.0f;
    const float swatchGap = 8.0f;
    constexpr size_t kSwatchesPerRow = 5;
    // 首个色块表示跟随主题背景，其后为预设纯色。
    const size_t swatchCount = kBackgroundColorCount + 1;
    for (size_t i = 0; i < swatchCount; ++i) {
        const size_t row = i / kSwatchesPerRow;
        const size_t column = i % kSwatchesPerRow;
        const float x = left + static_cast<float>(column) * (swatchWidth + swatchGap);
        const float top = y + static_cast<float>(row) * (swatchHeight + swatchGap);
        const D2D1_RECT_F rect = D2D1::RectF(x, top, x + swatchWidth, top + swatchHeight);
        const std::wstring hex =
            i == 0 ? std::wstring() : std::wstring(kBackgroundColors[i - 1]);
        const bool selected =
            i == 0 ? settings.backgroundType == L"theme"
                   : (settings.backgroundType == L"color" && settings.backgroundColor == hex);
        renderer_.FillRoundedRect(rect, 6.0f,
                                  i == 0 ? colors.background : ParseHexColor(hex, 1.0f));
        renderer_.StrokeRoundedRect(rect, 6.0f, selected ? colors.accent : colors.divider,
                                    selected ? 2.0f : 1.0f);
        if (i == 0) {
            renderer_.DrawTextBlock(L"默认", rect, formats.centered.Get(),
                                    colors.secondaryText);
        }
        AddHit(rect, i == 0 ? HitKind::BackgroundTheme : HitKind::BackgroundColor, -1, hex);
    }
    const size_t swatchRows = (swatchCount + kSwatchesPerRow - 1) / kSwatchesPerRow;
    y += static_cast<float>(swatchRows) * (swatchHeight + swatchGap) + 8.0f;

    // 图片背景：标签右侧为导入入口，缩略图排在下方。
    renderer_.DrawTextBlock(L"图片背景",
                            D2D1::RectF(left, y + 3.0f, left + width - 72.0f, y + 25.0f),
                            formats.caption.Get(), colors.secondaryText);
    DrawButton(L"导入", D2D1::RectF(left + width - 64.0f, y, left + width, y + 30.0f),
               HitKind::BackgroundImport, colors, false, false);
    y += 36.0f;

    // 缩略图为四列竖屏比例（宽:高 = 10:16，接近手机壁纸），
    // 右上角标注比例，左上角提供删除入口。
    constexpr float kThumbGap = 10.0f;
    constexpr size_t kThumbColumns = 4;
    const float thumbWidth = (width - kThumbGap * static_cast<float>(kThumbColumns - 1)) /
                             static_cast<float>(kThumbColumns);
    const float thumbHeight = thumbWidth * 16.0f / 10.0f;
    if (backgroundFiles_.empty()) {
        renderer_.DrawTextBlock(L"尚未导入背景图片。",
                                D2D1::RectF(left, y, left + width, y + 20.0f),
                                formats.caption.Get(), colors.secondaryText);
        y += 24.0f;
    } else {
        for (size_t i = 0; i < backgroundFiles_.size(); ++i) {
            const size_t row = i / kThumbColumns;
            const size_t column = i % kThumbColumns;
            const std::wstring& name = backgroundFiles_[i];
            const float x = left + static_cast<float>(column) * (thumbWidth + kThumbGap);
            const float top = y + static_cast<float>(row) * (thumbHeight + kThumbGap);
            const D2D1_RECT_F thumbRect = D2D1::RectF(x, top, x + thumbWidth, top + thumbHeight);
            renderer_.FillRect(thumbRect, colors.surfaceAlt);
            const BackgroundThumb* thumb = BackgroundThumbFor(name);
            if (thumb != nullptr && thumb->bitmap) {
                renderer_.DrawBitmap(thumb->bitmap.Get(), thumbRect, 1.0f);
            }
            const bool selected = settings.backgroundType == L"image" &&
                                  settings.backgroundImage == name;
            renderer_.StrokeRoundedRect(thumbRect, 4.0f,
                                        selected ? colors.accent : colors.divider,
                                        selected ? 2.0f : 1.0f);
            // 右上角比例小字：按文字实际宽度绘制底板，避免长比例被裁掉。
            const std::wstring ratio = thumb != nullptr
                                           ? ImageRatioLabel(thumb->sourceWidth, thumb->sourceHeight)
                                           : std::wstring();
            if (!ratio.empty()) {
                const float ratioTextWidth =
                    renderer_.MeasureTextWidth(ratio, formats.buttonSmall.Get());
                const float pillWidth =
                    std::min(thumbWidth - 8.0f, ratioTextWidth + 10.0f);
                const D2D1_RECT_F ratioRect =
                    D2D1::RectF(thumbRect.right - 4.0f - pillWidth, thumbRect.top + 4.0f,
                                thumbRect.right - 4.0f, thumbRect.top + 20.0f);
                renderer_.FillRoundedRect(ratioRect, 4.0f, ParseHexColor(L"#000000", 0.5f));
                renderer_.DrawTextBlock(ratio, ratioRect, formats.buttonSmall.Get(),
                                        colors.accentText);
            }
            // 左上角删除按钮：显示 18×18，命中区域外扩到 26×26，便于点击。
            const D2D1_RECT_F removeRect =
                D2D1::RectF(thumbRect.left + 4.0f, thumbRect.top + 4.0f,
                            thumbRect.left + 22.0f, thumbRect.top + 22.0f);
            renderer_.FillRoundedRect(removeRect, 5.0f, ParseHexColor(L"#000000", 0.6f));
            renderer_.DrawTextBlock(L"×", removeRect, formats.buttonSmall.Get(),
                                    colors.accentText);
            AddHit(thumbRect, HitKind::BackgroundImage, -1, name);
            AddHit(D2D1::RectF(removeRect.left - 4.0f, removeRect.top - 4.0f,
                               removeRect.right + 4.0f, removeRect.bottom + 4.0f),
                   HitKind::BackgroundRemove, -1, name);
        }
        const size_t thumbnailRows =
            (backgroundFiles_.size() + kThumbColumns - 1) / kThumbColumns;
        y += static_cast<float>(thumbnailRows) * (thumbHeight + kThumbGap) + 4.0f;
    }

    const float contentHeight = (y + 8.0f) - contentRect.top + panelScroll_;
    panelScrollMax_ = std::max(0.0f, contentHeight - (contentRect.bottom - contentRect.top));
    panelScroll_ = std::clamp(panelScroll_, 0.0f, panelScrollMax_);
    EndClip();
}

void Application::DrawImagesPanel(const ThemeColors& colors, const D2D1_RECT_F& contentRect) {
    const TextFormats& formats = renderer_.Formats();
    const data::BookRecord* record = library_.Find(activeBookId_);
    if (!record || record->imageFiles.empty()) {
        renderer_.DrawTextBlock(L"本书没有可显示的内嵌插图。",
                                D2D1::RectF(contentRect.left, contentRect.top, contentRect.right,
                                            contentRect.top + 32.0f),
                                formats.caption.Get(), colors.secondaryText);
        panelScrollMax_ = 0.0f;
        panelScroll_ = 0.0f;
        return;
    }

    const float contentWidth = contentRect.right - contentRect.left;
    const float captionHeight = 22.0f;
    const float spacing = 18.0f;

    float totalHeight = 0.0f;
    for (const std::wstring& path : record->imageFiles) {
        ID2D1Bitmap* bitmap = ImageBitmap(path);
        const D2D1_SIZE_F size = ImageDisplaySize(bitmap, contentWidth, 300.0f);
        totalHeight += captionHeight + size.height + spacing;
    }
    panelScrollMax_ = std::max(0.0f, totalHeight - (contentRect.bottom - contentRect.top));
    panelScroll_ = std::clamp(panelScroll_, 0.0f, panelScrollMax_);

    BeginClip(contentRect);
    float y = contentRect.top - panelScroll_;
    for (size_t i = 0; i < record->imageFiles.size(); ++i) {
        ID2D1Bitmap* bitmap = ImageBitmap(record->imageFiles[i]);
        const D2D1_SIZE_F size = ImageDisplaySize(bitmap, contentWidth, 300.0f);
        const float itemBottom = y + captionHeight + size.height;
        if (itemBottom > contentRect.top && y < contentRect.bottom) {
            const std::wstring caption = L"插图 " + std::to_wstring(i + 1);
            renderer_.DrawTextBlock(caption,
                                    D2D1::RectF(contentRect.left, y, contentRect.right,
                                                y + captionHeight),
                                    formats.caption.Get(), colors.secondaryText);
            const float imageLeft = contentRect.left + (contentWidth - size.width) / 2.0f;
            const D2D1_RECT_F imageRect =
                D2D1::RectF(imageLeft, y + captionHeight, imageLeft + size.width,
                            y + captionHeight + size.height);
            renderer_.FillRect(imageRect, colors.surfaceAlt);
            if (bitmap) {
                renderer_.DrawBitmap(bitmap, imageRect, 1.0f);
            } else {
                renderer_.DrawTextBlock(L"图片无法解码", imageRect, formats.centered.Get(),
                                        colors.secondaryText);
            }
        }
        y += captionHeight + size.height + spacing;
    }
    EndClip();
}

void Application::DrawGestureBar(const ThemeColors& colors) {
    if (!gestureBarVisible_ || panel_ != PanelMode::None) {
        return;
    }

    struct GestureDef {
        const wchar_t* label;
        Gesture value;
    };
    const GestureDef gestures[] = {
        {L"默认", Gesture::Default},            {L"选中高亮", Gesture::Highlight},
        {L"擦除高亮", Gesture::EraseHighlight}, {L"选中标注", Gesture::Note},
        {L"擦除标注", Gesture::EraseNote},      {L"复制", Gesture::Copy},
        {L"朗读", Gesture::Speech},
    };
    constexpr size_t kGestureCount = sizeof(gestures) / sizeof(gestures[0]);

    const float barWidth = kGestureBarPadding * 2.0f +
                           static_cast<float>(kGestureCount) * kGestureButtonWidth +
                           static_cast<float>(kGestureCount - 1) * kGestureButtonGap;
    const float x = widthDip_ - 16.0f - barWidth;
    const float y = kReaderToolbarHeight + 8.0f;
    const D2D1_RECT_F barRect = D2D1::RectF(x, y, x + barWidth, y + kGestureBarHeight);
    renderer_.FillRoundedRect(barRect, 10.0f, colors.surface);
    renderer_.StrokeRoundedRect(barRect, 10.0f, colors.divider, 1.0f);

    float left = x + kGestureBarPadding;
    for (size_t i = 0; i < kGestureCount; ++i) {
        DrawButton(gestures[i].label,
                   D2D1::RectF(left, y + 7.0f, left + kGestureButtonWidth,
                               y + 7.0f + kGestureButtonHeight),
                   HitKind::GestureOption, colors, false, gesture_ == gestures[i].value,
                   static_cast<int>(i));
        left += kGestureButtonWidth + kGestureButtonGap;
    }
}

void Application::DrawColorPicker(const ThemeColors& colors) {
    if (!colorPickerVisible_ || !gestureBarVisible_ || panel_ != PanelMode::None) {
        return;
    }
    const float cell = 30.0f;
    const float gap = 6.0f;
    const float gridSize = 4.0f * cell + 3.0f * gap;
    const float x = widthDip_ - 16.0f - gridSize;
    const float y = kReaderToolbarHeight + 8.0f + kGestureBarHeight + 8.0f;
    const D2D1_RECT_F panelRect =
        D2D1::RectF(x - 8.0f, y - 8.0f, x + gridSize + 8.0f, y + gridSize + 8.0f);
    renderer_.FillRoundedRect(panelRect, 10.0f, colors.surface);
    renderer_.StrokeRoundedRect(panelRect, 10.0f, colors.divider, 1.0f);
    for (int i = 0; i < 16; ++i) {
        const int row = i / 4;
        const int column = i % 4;
        const float swatchLeft = x + column * (cell + gap);
        const float swatchTop = y + row * (cell + gap);
        const D2D1_RECT_F swatch =
            D2D1::RectF(swatchLeft, swatchTop, swatchLeft + cell, swatchTop + cell);
        renderer_.FillRoundedRect(swatch, 6.0f, ParseHexColor(kHighlightColors[i], 1.0f));
        renderer_.StrokeRoundedRect(swatch, 6.0f, colors.divider, 1.0f);
        AddHit(swatch, HitKind::HighlightColor, i, kHighlightColors[i]);
    }
}

void Application::DrawCopyToast(const ThemeColors& colors) {
    if (!copyToastVisible_) {
        return;
    }
    const TextFormats& formats = renderer_.Formats();
    const float left = (widthDip_ - kCopyToastWidth) * 0.5f;
    const float top =
        heightDip_ - kReaderFooterHeight - kCopyToastBottomGap - kCopyToastHeight;
    const D2D1_RECT_F rect =
        D2D1::RectF(left, top, left + kCopyToastWidth, top + kCopyToastHeight);
    renderer_.FillRoundedRect(rect, 9.0f, colors.surface);
    renderer_.StrokeRoundedRect(rect, 9.0f, colors.accent, 1.2f);
    renderer_.DrawTextBlock(L"已复制", rect, formats.centered.Get(), colors.accent);
}

void Application::DrawInputCaret(const ThemeColors& colors) {
    if (!caretBlinkVisible_) {
        return;
    }
    D2D1_POINT_2F caret{};
    float lineHeight = 0.0f;
    if (!FocusedInputCaret(caret, lineHeight) || lineHeight <= 0.0f) {
        return;
    }
    const float top = caret.y - lineHeight;
    renderer_.FillRect(D2D1::RectF(caret.x, top, caret.x + 1.6f, caret.y), colors.accent);
}

void Application::DrawHighlights(IDWriteTextLayout* pageLayout, uint32_t pageStart,
                                 uint32_t pageEnd, const D2D1_RECT_F& textRect,
                                 const ThemeColors& colors) {
    (void)colors;
    const data::BookRecord* record = library_.Find(activeBookId_);
    if (!record || !pageLayout || record->highlights.empty() || pageEnd <= pageStart) {
        return;
    }
    for (size_t i = 0; i < record->highlights.size(); ++i) {
        const data::Highlight& highlight = record->highlights[i];
        if (highlight.chapterIndex != activeChapter_ || highlight.length == 0) {
            continue;
        }
        const uint32_t displayStart = chapterDisplay_.ToDisplay(highlight.start);
        const uint32_t displayEnd = chapterDisplay_.ToDisplay(highlight.start + highlight.length);
        const uint32_t from = std::max(displayStart, pageStart);
        const uint32_t to = std::min(displayEnd, pageEnd);
        if (to <= from) {
            continue;
        }
        const uint32_t localFrom = from - pageStart;
        const uint32_t localLength = to - from;
        UINT32 count = 0;
        pageLayout->HitTestTextRange(localFrom, localLength, 0.0f, 0.0f, nullptr, 0, &count);
        if (count == 0) {
            continue;
        }
        std::vector<DWRITE_HIT_TEST_METRICS> metrics(count);
        pageLayout->HitTestTextRange(localFrom, localLength, 0.0f, 0.0f, metrics.data(), count,
                                     &count);
        const D2D1_COLOR_F color = ParseHexColor(highlight.color, 0.55f);
        for (UINT32 m = 0; m < count; ++m) {
            const D2D1_RECT_F rect =
                D2D1::RectF(textRect.left + metrics[m].left, textRect.top + metrics[m].top,
                            textRect.left + metrics[m].left + metrics[m].width,
                            textRect.top + metrics[m].top + metrics[m].height);
            renderer_.FillRect(rect, color);
            AnnotationHit hit;
            hit.rect = rect;
            hit.index = static_cast<int>(i);
            hit.isNote = false;
            annotationHits_.push_back(hit);
        }
    }
}

void Application::DrawSelection(IDWriteTextLayout* pageLayout, uint32_t pageStart,
                                uint32_t pageEnd, const D2D1_RECT_F& textRect,
                                const ThemeColors& colors) {
    if (!selectionActive_ || !pageLayout || pageEnd <= pageStart) {
        return;
    }
    const uint32_t from = std::max(selectionStartDisplay_, pageStart);
    const uint32_t to = std::min(selectionEndDisplay_, pageEnd);
    if (to <= from) {
        return;
    }
    UINT32 count = 0;
    pageLayout->HitTestTextRange(from - pageStart, to - from, 0.0f, 0.0f, nullptr, 0, &count);
    if (count == 0) {
        return;
    }
    std::vector<DWRITE_HIT_TEST_METRICS> metrics(count);
    pageLayout->HitTestTextRange(from - pageStart, to - from, 0.0f, 0.0f, metrics.data(), count,
                                 &count);
    const D2D1_COLOR_F color =
        D2D1::ColorF(colors.accent.r, colors.accent.g, colors.accent.b, 0.26f);
    for (UINT32 m = 0; m < count; ++m) {
        renderer_.FillRect(D2D1::RectF(textRect.left + metrics[m].left,
                                       textRect.top + metrics[m].top,
                                       textRect.left + metrics[m].left + metrics[m].width,
                                       textRect.top + metrics[m].top + metrics[m].height),
                           color);
    }
}

void Application::DrawNotes(IDWriteTextLayout* pageLayout, uint32_t pageStart, uint32_t pageEnd,
                            const D2D1_RECT_F& textRect, const ThemeColors& colors) {
    (void)colors;
    const data::BookRecord* record = library_.Find(activeBookId_);
    if (!record || !pageLayout || chapterDisplay_.Placements().empty() || pageEnd <= pageStart) {
        return;
    }
    const TextFormats& formats = renderer_.Formats();
    const float indent = chapterDisplay_.IndentWidth();
    const float noteWidth = chapterDisplay_.NoteWidth();
    const float pageHeight = textRect.bottom - textRect.top;
    const float bodyLineHeight = FirstLineHeight(pageLayout);
    const float padding = 4.0f;

    for (const NotePlacement& placement : chapterDisplay_.Placements()) {
        const int index = placement.noteIndex;
        if (index < 0 || index >= static_cast<int>(record->notes.size())) {
            continue;
        }
        const data::Note& note = record->notes[static_cast<size_t>(index)];
        if (note.chapterIndex != activeChapter_ || note.length == 0 || note.text.empty()) {
            continue;
        }

        // ---- 计算标注框在页面中的位置 ----
        float top = 0.0f;
        bool visible = false;
        if (placement.displayStart >= pageStart) {
            const uint32_t local = placement.displayStart - pageStart;
            if (local > pageEnd - pageStart) {
                continue;
            }
            top = LineTopForOffset(pageLayout, local);
            visible = top < pageHeight;
        } else {
            if (bodyLineHeight <= 0.0f) {
                continue;
            }
            const uint32_t rowOffset = pageStart - placement.displayStart;
            top = -static_cast<float>(rowOffset) * bodyLineHeight;
            visible = top + static_cast<float>(placement.lineCount) * bodyLineHeight > 0.0f &&
                      top < pageHeight;
        }
        if (!visible) {
            continue;
        }

        // ---- 渲染标注文字（使用独立的小字号字体）----
        ComPtr<IDWriteTextLayout> noteLayout =
            renderer_.CreateLayout(note.text, formats.note.Get(), noteWidth, 100000.0f);
        if (!noteLayout) {
            continue;
        }
        DWRITE_TEXT_METRICS noteMetrics{};
        noteLayout->GetMetrics(&noteMetrics);
        const float noteLeft = textRect.left + indent;
        const float noteTop = textRect.top + top;
        renderer_.DrawLayout(noteLayout.Get(), D2D1::Point2F(noteLeft, noteTop), NoteTextColor());

        D2D1_RECT_F noteFrame =
            D2D1::RectF(noteLeft - padding, noteTop - padding,
                        noteLeft + noteMetrics.widthIncludingTrailingWhitespace + padding,
                        noteTop + noteMetrics.height + padding);
        renderer_.StrokeRoundedRect(noteFrame, 6.0f, NoteFrameColor(), 1.4f);

        // ---- 绘制原文中被标注片段的红色下划线 ----
        const uint32_t targetDisplayStart = chapterDisplay_.ToDisplay(note.start);
        const uint32_t targetDisplayEnd = chapterDisplay_.ToDisplay(note.start + note.length);
        const uint32_t from = std::max(targetDisplayStart, pageStart);
        const uint32_t to = std::min(targetDisplayEnd, pageEnd);
        bool targetVisible = false;
        DWRITE_HIT_TEST_METRICS lastTargetMetrics{};  // 记录最后一行，用于箭头起点
        if (to > from) {
            const uint32_t localFrom = from - pageStart;
            const uint32_t localLength = to - from;
            UINT32 count = 0;
            pageLayout->HitTestTextRange(localFrom, localLength, 0.0f, 0.0f, nullptr, 0, &count);
            if (count > 0) {
                std::vector<DWRITE_HIT_TEST_METRICS> metrics(count);
                pageLayout->HitTestTextRange(localFrom, localLength, 0.0f, 0.0f, metrics.data(),
                                             count, &count);
                for (UINT32 m = 0; m < count; ++m) {
                    targetVisible = true;
                    const float lineLeft = textRect.left + metrics[m].left;
                    const float lineRight =
                        textRect.left + metrics[m].left + metrics[m].width;
                    const float lineBottom =
                        textRect.top + metrics[m].top + metrics[m].height;
                    // 下划线在文字底部再低 2 个像素，与正文基线之间留有间距
                    renderer_.DrawLine(
                        D2D1::Point2F(lineLeft, lineBottom + 2.0f),
                        D2D1::Point2F(lineRight, lineBottom + 2.0f), NoteFrameColor(), 1.5f);
                    lastTargetMetrics = metrics[m];
                }
            }
        }

        // ---- 绘制箭头：原文最后一行下划线末端 → 标注框顶部 ----
        if (targetVisible) {
            const float arrowOriginX = textRect.left + lastTargetMetrics.left +
                                       lastTargetMetrics.width;
            const float arrowOriginY =
                textRect.top + lastTargetMetrics.top + lastTargetMetrics.height + 2.0f +
                1.5f;  // 下划线底部（含线粗一半）
            // 箭头尖端指向标注框顶部的"尾部"——如果原文比标注框宽，取标注框顶中心；
            // 如果标注框在原文右边，取标注框左上角。
            float arrowTipX;
            if (noteFrame.left > arrowOriginX + 2.0f) {
                arrowTipX = noteFrame.left;  // 标注框在右侧，箭头朝右下
            } else {
                arrowTipX = noteFrame.left + (noteFrame.right - noteFrame.left) * 0.5f;
            }
            const float arrowTipY = noteFrame.top;
            // 只画箭头当标注框确实出现在原文下方（起点低于尖端则跳过）
            if (arrowTipY > arrowOriginY + 4.0f) {
                const D2D1_COLOR_F arrowColor = NoteFrameColor();
                renderer_.DrawLine(D2D1::Point2F(arrowOriginX, arrowOriginY),
                                   D2D1::Point2F(arrowTipX, arrowTipY), arrowColor, 1.3f);
                // 箭头头部：两条短斜线构成 V 形
                constexpr float kArrowHeadSize = 6.0f;
                constexpr float kArrowHeadAngle = 0.45f;  // 弧度 ≈ 25°
                const float angleX = arrowTipX - arrowOriginX;
                const float angleY = arrowTipY - arrowOriginY;
                const float length = std::hypot(angleX, angleY);
                if (length > 0.1f) {
                    const float dirX = angleX / length;
                    const float dirY = angleY / length;
                    const float leftWingX = arrowTipX - dirX * std::cos(kArrowHeadAngle) *
                                                        kArrowHeadSize +
                                            dirY * std::sin(kArrowHeadAngle) * kArrowHeadSize;
                    const float leftWingY = arrowTipY - dirY * std::cos(kArrowHeadAngle) *
                                                        kArrowHeadSize -
                                            dirX * std::sin(kArrowHeadAngle) * kArrowHeadSize;
                    const float rightWingX = arrowTipX - dirX * std::cos(kArrowHeadAngle) *
                                                         kArrowHeadSize -
                                             dirY * std::sin(kArrowHeadAngle) * kArrowHeadSize;
                    const float rightWingY = arrowTipY - dirY * std::cos(kArrowHeadAngle) *
                                                         kArrowHeadSize +
                                             dirX * std::sin(kArrowHeadAngle) * kArrowHeadSize;
                    renderer_.DrawLine(D2D1::Point2F(arrowTipX, arrowTipY),
                                       D2D1::Point2F(leftWingX, leftWingY), arrowColor, 1.3f);
                    renderer_.DrawLine(D2D1::Point2F(arrowTipX, arrowTipY),
                                       D2D1::Point2F(rightWingX, rightWingY), arrowColor, 1.3f);
                }
            } else if (arrowTipY < arrowOriginY - 4.0f) {
                // 标注框在原文上方（极少见），箭头朝上
                const D2D1_COLOR_F arrowColor = NoteFrameColor();
                renderer_.DrawLine(D2D1::Point2F(arrowOriginX, arrowOriginY),
                                   D2D1::Point2F(arrowTipX, arrowTipY), arrowColor, 1.3f);
                constexpr float kArrowHeadSize = 6.0f;
                constexpr float kArrowHeadAngle = 0.45f;
                const float dirX = (arrowTipX - arrowOriginX) /
                                   std::max(0.1f, std::hypot(arrowTipX - arrowOriginX,
                                                             arrowTipY - arrowOriginY));
                const float dirY = (arrowTipY - arrowOriginY) /
                                   std::max(0.1f, std::hypot(arrowTipX - arrowOriginX,
                                                             arrowTipY - arrowOriginY));
                const float leftWingX = arrowTipX - dirX * std::cos(kArrowHeadAngle) *
                                                    kArrowHeadSize +
                                        dirY * std::sin(kArrowHeadAngle) * kArrowHeadSize;
                const float leftWingY = arrowTipY - dirY * std::cos(kArrowHeadAngle) *
                                                    kArrowHeadSize -
                                        dirX * std::sin(kArrowHeadAngle) * kArrowHeadSize;
                const float rightWingX = arrowTipX - dirX * std::cos(kArrowHeadAngle) *
                                                     kArrowHeadSize -
                                         dirY * std::sin(kArrowHeadAngle) * kArrowHeadSize;
                const float rightWingY = arrowTipY - dirY * std::cos(kArrowHeadAngle) *
                                                     kArrowHeadSize +
                                         dirX * std::sin(kArrowHeadAngle) * kArrowHeadSize;
                renderer_.DrawLine(D2D1::Point2F(arrowTipX, arrowTipY),
                                   D2D1::Point2F(leftWingX, leftWingY), arrowColor, 1.3f);
                renderer_.DrawLine(D2D1::Point2F(arrowTipX, arrowTipY),
                                   D2D1::Point2F(rightWingX, rightWingY), arrowColor, 1.3f);
            }
        }

        AnnotationHit hit;
        hit.rect = noteFrame;
        hit.index = index;
        hit.isNote = true;
        annotationHits_.push_back(hit);
    }
}

void Application::DrawNoteEditorPanel(const ThemeColors& colors,
                                      const D2D1_RECT_F& contentRect) {
    const TextFormats& formats = renderer_.Formats();
    const float left = contentRect.left;
    const float width = contentRect.right - contentRect.left;
    float y = contentRect.top;

    renderer_.DrawTextBlock(L"选中文字", D2D1::RectF(left, y, left + width, y + 20.0f),
                            formats.caption.Get(), colors.secondaryText);
    y += 24.0f;
    renderer_.DrawTextBlock(util::Ellipsize(SanitizeLine(pendingNotePreview_), 60),
                            D2D1::RectF(left, y, contentRect.right, y + 40.0f),
                            formats.listItem.Get(), colors.secondaryText);
    y += 48.0f;

    renderer_.DrawTextBlock(L"标注内容（最多 255 字符）",
                            D2D1::RectF(left, y, left + width, y + 20.0f),
                            formats.caption.Get(), colors.secondaryText);
    y += 24.0f;
    const D2D1_RECT_F inputRect = NoteInputBoxRect();
    renderer_.FillRoundedRect(inputRect, 9.0f, colors.surfaceAlt);
    renderer_.StrokeRoundedRect(inputRect, 9.0f,
                                noteFocused_ ? colors.accent : colors.divider,
                                noteFocused_ ? 1.6f : 1.0f);
    const D2D1_RECT_F inputTextRect =
        D2D1::RectF(inputRect.left + kNoteInputPaddingX, inputRect.top + kNoteInputPaddingY,
                    inputRect.right - kNoteInputPaddingX,
                    inputRect.bottom - kNoteInputPaddingY);
    if (noteDraft_.empty()) {
        renderer_.DrawTextBlock(L"在此输入标注文字，回车换行",
                                inputTextRect, formats.listItem.Get(), colors.secondaryText);
    } else {
        ComPtr<IDWriteTextLayout> layout = renderer_.CreateLayout(
            noteDraft_, formats.label.Get(), inputTextRect.right - inputTextRect.left,
            inputTextRect.bottom - inputTextRect.top);
        if (layout) {
            renderer_.PushClip(inputTextRect);
            renderer_.DrawLayout(layout.Get(),
                                 D2D1::Point2F(inputTextRect.left, inputTextRect.top),
                                 colors.text);
            renderer_.PopClip();
        }
    }
    if (noteFocused_) {
        DrawInputSelection(inputTextRect, formats.label.Get(), true, colors);
    }
    AddHit(inputRect, HitKind::NoteInput);
    y = inputRect.bottom + 8.0f;
    renderer_.DrawTextBlock(std::to_wstring(noteDraft_.size()) + L" / 255",
                            D2D1::RectF(left, y, contentRect.right, y + 20.0f),
                            formats.captionRight.Get(), colors.secondaryText);
    y += 30.0f;

    const float buttonHeight = 38.0f;
    DrawButton(L"保存标注", D2D1::RectF(left, y, left + 120.0f, y + buttonHeight),
               HitKind::NoteSave, colors, true, false);
    DrawButton(L"取消", D2D1::RectF(left + 130.0f, y, left + 226.0f, y + buttonHeight),
               HitKind::NoteCancel, colors, false, false);
}

void Application::DrawChatPanel(const ThemeColors& colors, const D2D1_RECT_F& contentRect) {
    const TextFormats& formats = renderer_.Formats();
    const data::ReaderSettings& settings = library_.Settings();

    // 接口配置已移至独立弹框，这里仅保留入口与当前模型提示。
    const std::wstring protocol =
        settings.aiProtocol == L"anthropic" ? std::wstring(L"Anthropic") : std::wstring(L"OpenAI");
    const std::wstring modelLabel =
        settings.aiModel.empty() ? std::wstring(L"未设置模型") : settings.aiModel;
    renderer_.DrawTextBlock(util::Ellipsize(protocol + L" · " + modelLabel, 26),
                            D2D1::RectF(contentRect.left, contentRect.top, contentRect.right - 96.0f,
                                        contentRect.top + 30.0f),
                            formats.caption.Get(), colors.secondaryText);
    DrawButton(L"接口设置",
               D2D1::RectF(contentRect.right - 88.0f, contentRect.top, contentRect.right,
                           contentRect.top + 30.0f),
               HitKind::OpenAiSettings, colors, false, false);

    // 会话栏：打开对话记录、新建会话，中间显示当前会话标题。
    const D2D1_RECT_F sessionBar =
        D2D1::RectF(contentRect.left, contentRect.top + 38.0f, contentRect.right,
                    contentRect.top + 68.0f);
    DrawButton(L"会话", D2D1::RectF(sessionBar.left, sessionBar.top, sessionBar.left + 56.0f,
                                    sessionBar.top + 30.0f),
               HitKind::ChatSessionOpen, colors, false, chatSessionPickerVisible_);
    DrawPlainButton(L"新建",
                    D2D1::RectF(sessionBar.right - 56.0f, sessionBar.top, sessionBar.right,
                                sessionBar.top + 30.0f),
                    HitKind::ChatSessionNew, colors);
    const data::ChatSession* session = ActiveChatSession();
    const std::wstring sessionTitle =
        session != nullptr && !session->title.empty() ? session->title : std::wstring(L"新对话");
    renderer_.DrawTextBlock(util::Ellipsize(sessionTitle, 16),
                            D2D1::RectF(sessionBar.left + 62.0f, sessionBar.top + 4.0f,
                                        sessionBar.right - 62.0f, sessionBar.top + 26.0f),
                            formats.caption.Get(), colors.secondaryText);

    // 对话内容区：逐条消息绘制，超出时可滚动查看。
    const D2D1_RECT_F conversation = ChatConversationRect();
    DrawChatConversation(conversation, colors);

    // 状态行与停止按钮
    renderer_.DrawTextBlock(util::Ellipsize(aiStatus_, 40), ChatStatusRect(),
                            formats.caption.Get(), colors.secondaryText);
    if (aiStreaming_) {
        DrawButton(L"停止生成", ChatStopRect(), HitKind::ChatStop, colors, false, false);
    }

    // 问题输入框与发送按钮
    const D2D1_RECT_F inputRect = ChatFieldRect(ChatFocus::Message);
    const bool inputFocused = chatFocus_ == ChatFocus::Message;
    renderer_.FillRoundedRect(inputRect, 9.0f, colors.surfaceAlt);
    renderer_.StrokeRoundedRect(inputRect, 9.0f,
                                inputFocused ? colors.accent : colors.divider,
                                inputFocused ? 1.6f : 1.0f);
    const D2D1_RECT_F inputTextRect =
        D2D1::RectF(inputRect.left + 10.0f, inputRect.top + 7.0f, inputRect.right - 10.0f,
                    inputRect.bottom - 7.0f);
    if (aiDraft_.empty()) {
        renderer_.DrawTextBlock(L"输入问题，Ctrl+回车发送，回车换行", inputTextRect,
                                formats.listItem.Get(), colors.secondaryText);
    } else {
        // 多行输入：按输入框宽度换行显示，超出高度时裁剪。
        ComPtr<IDWriteTextLayout> inputLayout =
            renderer_.CreateLayout(aiDraft_, formats.listItem.Get(),
                                   inputTextRect.right - inputTextRect.left, 100000.0f);
        if (inputLayout) {
            inputLayout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
            renderer_.PushClip(inputTextRect);
            renderer_.DrawLayout(inputLayout.Get(),
                                 D2D1::Point2F(inputTextRect.left, inputTextRect.top),
                                 colors.text);
            renderer_.PopClip();
        }
    }
    if (inputFocused) {
        DrawInputSelection(inputTextRect, formats.listItem.Get(), true, colors);
    }
    AddHit(inputRect, HitKind::ChatInput);
    DrawButton(L"发送", ChatSendRect(), HitKind::ChatSend, colors, true, false);
}

data::ChatSession* Application::ActiveChatSession() {
    data::BookRecord* record = library_.Find(activeBookId_);
    if (record == nullptr || activeChatSessionId_.empty()) {
        return nullptr;
    }
    for (data::ChatSession& session : record->chatSessions) {
        if (session.id == activeChatSessionId_) {
            return &session;
        }
    }
    return nullptr;
}

data::ChatSession* Application::CreateChatSession() {
    data::BookRecord* record = library_.Find(activeBookId_);
    if (record == nullptr) {
        return nullptr;
    }
    data::ChatSession session;
    session.createdAt = util::CurrentUnixSeconds();
    session.updatedAt = session.createdAt;
    session.title = L"新对话";
    const std::wstring digest =
        util::Sha256Hex(util::ToUtf8(activeBookId_ + L"|" + std::to_wstring(session.createdAt)));
    session.id = L"chat-" + digest.substr(0, 12);
    int suffix = 0;
    while (std::any_of(record->chatSessions.begin(), record->chatSessions.end(),
                       [&session](const data::ChatSession& other) {
                           return other.id == session.id;
                       })) {
        session.id = L"chat-" + digest.substr(0, 12) + L"-" + std::to_wstring(++suffix);
    }
    activeChatSessionId_ = session.id;
    record->chatSessions.push_back(std::move(session));
    return &record->chatSessions.back();
}

data::ChatSession* Application::EnsureChatSession() {
    data::ChatSession* session = ActiveChatSession();
    if (session != nullptr) {
        return session;
    }
    return CreateChatSession();
}

void Application::NewChatSession() {
    if (activeBookId_.empty()) {
        return;
    }
    if (aiStreaming_) {
        StopAiRequest();
    }
    aiAnswer_.clear();
    aiDraft_.clear();
    ResetInputCaretToEnd();
    aiStatus_.clear();
    chatScroll_ = 0.0f;
    chatSessionPickerVisible_ = false;
    CreateChatSession();
    SaveChatState();
    InvalidateRect(hwnd_, nullptr, FALSE);
    UpdateWindow(hwnd_);
}

void Application::DeleteChatSession(const std::wstring& id) {
    data::BookRecord* record = library_.Find(activeBookId_);
    if (record == nullptr) {
        return;
    }
    const bool deletingActive = activeChatSessionId_ == id;
    if (deletingActive && aiStreaming_) {
        StopAiRequest();
    }
    record->chatSessions.erase(
        std::remove_if(record->chatSessions.begin(), record->chatSessions.end(),
                       [&id](const data::ChatSession& session) { return session.id == id; }),
        record->chatSessions.end());
    if (deletingActive) {
        aiAnswer_.clear();
        activeChatSessionId_.clear();
        chatScroll_ = 0.0f;
        // 自动切换到最近更新的会话；全部删除后保持“新对话”的空状态。
        int64_t newestTime = -1;
        for (const data::ChatSession& session : record->chatSessions) {
            if (session.updatedAt >= newestTime) {
                newestTime = session.updatedAt;
                activeChatSessionId_ = session.id;
            }
        }
    }
    SaveChatState();
    InvalidateRect(hwnd_, nullptr, FALSE);
    UpdateWindow(hwnd_);
}

void Application::SelectChatSession(const std::wstring& id) {
    if (aiStreaming_) {
        StopAiRequest();
    }
    aiAnswer_.clear();
    activeChatSessionId_ = id;
    // 切换后默认显示最新消息，绘制时会被钳制到实际最大滚动量。
    chatScroll_ = 1.0e9f;
    chatSessionPickerVisible_ = false;
    InvalidateRect(hwnd_, nullptr, FALSE);
    UpdateWindow(hwnd_);
}

void Application::SaveChatState() {
    if (activeBookId_.empty()) {
        return;
    }
    std::wstring error;
    library_.SaveBook(activeBookId_, error);
}

bool Application::CopyTextToClipboard(const std::wstring& text) {
    if (text.empty()) {
        return false;
    }
    bool copied = false;
    if (OpenClipboard(hwnd_)) {
        EmptyClipboard();
        const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (memory) {
            void* target = GlobalLock(memory);
            if (target) {
                std::memcpy(target, text.c_str(), bytes);
                GlobalUnlock(memory);
                if (SetClipboardData(CF_UNICODETEXT, memory) != nullptr) {
                    copied = true;
                } else {
                    GlobalFree(memory);
                }
            } else {
                GlobalFree(memory);
            }
        }
        CloseClipboard();
    }
    return copied;
}

void Application::CopyChatMessage(int messageIndex) {
    const data::ChatSession* session = ActiveChatSession();
    if (session == nullptr || messageIndex < 0 ||
        messageIndex >= static_cast<int>(session->messages.size())) {
        return;
    }
    if (!CopyTextToClipboard(session->messages[static_cast<size_t>(messageIndex)].text)) {
        aiStatus_ = L"复制失败";
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    // 对话内的复制提示显示在对话面板，不在阅读页底部弹出「已复制」。
    aiStatus_ = L"已复制";
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Application::BuildChatBubbles(const D2D1_RECT_F& rect, std::vector<ChatBubble>& bubbles) {
    bubbles.clear();
    const TextFormats& formats = renderer_.Formats();
    // 右侧预留滚动条的位置。
    const float available = std::max(120.0f, rect.right - rect.left - 34.0f);
    const float bubbleWidth = available * kChatBubbleWidthRatio;
    const float textWidth = std::max(48.0f, bubbleWidth - kChatBubblePadding * 2.0f);
    float cursor = 8.0f;

    const auto appendBubble = [&](const std::wstring& text, bool isUser, int messageIndex,
                                  const std::wstring& thinking) {
        ChatBubble bubble;
        bubble.messageIndex = messageIndex;
        bubble.isUser = isUser;
        bubble.layout =
            renderer_.CreateLayout(text, formats.listItem.Get(), textWidth, 100000.0f);
        float textHeight = 18.0f;
        if (bubble.layout) {
            bubble.layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
            DWRITE_TEXT_METRICS metrics{};
            bubble.layout->GetMetrics(&metrics);
            textHeight = std::max(18.0f, metrics.height);
        }
        // 思考内容默认折叠，展开后占用独立高度。
        float thinkingBlock = 0.0f;
        if (!thinking.empty()) {
            bubble.thinking = thinking;
            bubble.thinkingExpanded = chatThinkingExpanded_.count(messageIndex) > 0;
            bubble.thinkingLayout =
                renderer_.CreateLayout(thinking, formats.caption.Get(), textWidth, 100000.0f);
            if (bubble.thinkingLayout) {
                bubble.thinkingLayout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
                DWRITE_TEXT_METRICS metrics{};
                bubble.thinkingLayout->GetMetrics(&metrics);
                bubble.thinkingHeight = std::max(18.0f, metrics.height);
            }
            thinkingBlock = 18.0f;
            if (bubble.thinkingExpanded) {
                thinkingBlock += bubble.thinkingHeight + 6.0f;
            }
        }
        bubble.height = kChatBubbleHeader + thinkingBlock + textHeight + kChatBubblePadding;
        bubble.top = cursor;
        // 用户消息右对齐，AI 消息左对齐。
        const float left =
            isUser ? rect.right - 12.0f - bubbleWidth : rect.left + 12.0f;
        bubble.bubble = D2D1::RectF(left, cursor, left + bubbleWidth, cursor + bubble.height);
        bubble.thinkingToggleRect =
            D2D1::RectF(left + kChatBubblePadding, cursor + kChatBubbleHeader,
                        left + kChatBubblePadding + 100.0f, cursor + kChatBubbleHeader + 18.0f);
        if (thinkingBlock > 0.0f) {
            bubble.thinkingRect =
                D2D1::RectF(left + kChatBubblePadding, cursor + kChatBubbleHeader + 18.0f,
                            left + bubbleWidth - kChatBubblePadding,
                            cursor + kChatBubbleHeader + 18.0f + bubble.thinkingHeight);
        }
        bubble.textRect = D2D1::RectF(
            left + kChatBubblePadding, cursor + kChatBubbleHeader + thinkingBlock,
            left + bubbleWidth - kChatBubblePadding,
            cursor + bubble.height - kChatBubblePadding * 0.4f);
        cursor += bubble.height + kChatBubbleGap;
        bubbles.push_back(std::move(bubble));
    };

    const data::ChatSession* session = ActiveChatSession();
    if (session != nullptr) {
        for (size_t i = 0; i < session->messages.size(); ++i) {
            const data::ChatMessage& message = session->messages[i];
            appendBubble(message.text, message.role == L"user", static_cast<int>(i),
                         message.thinking);
        }
    }
    if (aiStreaming_) {
        const std::wstring streamingText =
            aiAnswer_.empty() ? std::wstring(L"正在生成…") : aiAnswer_;
        appendBubble(streamingText, false, -1, aiThinking_);
    }
}

uint32_t Application::ChatTextPositionAt(const ChatBubble& bubble, float x, float y) const {
    if (!bubble.layout) {
        return 0;
    }
    const float width = std::max(1.0f, bubble.textRect.right - bubble.textRect.left);
    const float height = std::max(1.0f, bubble.textRect.bottom - bubble.textRect.top);
    const float localX = std::clamp(x - bubble.textRect.left, 0.0f, width - 1.0f);
    const float localY = std::clamp(y - bubble.textRect.top, 0.0f, height - 1.0f);
    BOOL trailing = FALSE;
    BOOL inside = FALSE;
    DWRITE_HIT_TEST_METRICS metrics{};
    if (FAILED(bubble.layout->HitTestPoint(localX, localY, &trailing, &inside, &metrics))) {
        return 0;
    }
    uint32_t position = metrics.textPosition;
    if (trailing != FALSE && inside != FALSE) {
        ++position;
    }
    return position;
}

void Application::DrawChatSelection(const ChatBubble& bubble, const D2D1_RECT_F& textRect,
                                    const ThemeColors& colors) {
    if (!bubble.layout || chatSelectionMessage_ != bubble.messageIndex) {
        return;
    }
    const uint32_t begin = std::min(chatSelectionStart_, chatSelectionEnd_);
    const uint32_t end = std::max(chatSelectionStart_, chatSelectionEnd_);
    if (begin >= end) {
        return;
    }
    UINT32 count = 0;
    bubble.layout->HitTestTextRange(begin, end - begin, 0.0f, 0.0f, nullptr, 0, &count);
    if (count == 0) {
        return;
    }
    std::vector<DWRITE_HIT_TEST_METRICS> metrics(count);
    if (FAILED(bubble.layout->HitTestTextRange(begin, end - begin, 0.0f, 0.0f, metrics.data(),
                                              count, &count))) {
        return;
    }
    const D2D1_COLOR_F selectionColor = ParseHexColor(L"#4A90D9", 0.35f);
    for (UINT32 i = 0; i < count; ++i) {
        renderer_.FillRect(D2D1::RectF(textRect.left + metrics[i].left,
                                       textRect.top + metrics[i].top,
                                       textRect.left + metrics[i].left + metrics[i].width,
                                       textRect.top + metrics[i].top + metrics[i].height),
                           selectionColor);
    }
}

bool Application::BeginChatSelection(float x, float y) {
    const D2D1_RECT_F rect = ChatConversationRect();
    std::vector<ChatBubble> bubbles;
    BuildChatBubbles(rect, bubbles);
    const float offset = rect.top - chatScroll_;
    for (const ChatBubble& bubble : bubbles) {
        if (bubble.messageIndex < 0 || !bubble.layout) {
            continue;
        }
        ChatBubble absolute = bubble;
        absolute.bubble = OffsetRect(bubble.bubble, 0.0f, offset);
        absolute.textRect = OffsetRect(bubble.textRect, 0.0f, offset);
        if (!PointInside(absolute.textRect, x, y)) {
            continue;
        }
        const uint32_t position = ChatTextPositionAt(absolute, x, y);
        chatSelectionMessage_ = bubble.messageIndex;
        chatSelectionStart_ = position;
        chatSelectionEnd_ = position;
        chatSelecting_ = true;
        SetCapture(hwnd_);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return true;
    }
    ClearChatSelection();
    InvalidateRect(hwnd_, nullptr, FALSE);
    return false;
}

void Application::UpdateChatSelection(float x, float y) {
    if (!chatSelecting_ || chatSelectionMessage_ < 0) {
        return;
    }
    const D2D1_RECT_F rect = ChatConversationRect();
    std::vector<ChatBubble> bubbles;
    BuildChatBubbles(rect, bubbles);
    const float offset = rect.top - chatScroll_;
    for (const ChatBubble& bubble : bubbles) {
        if (bubble.messageIndex != chatSelectionMessage_) {
            continue;
        }
        ChatBubble absolute = bubble;
        absolute.bubble = OffsetRect(bubble.bubble, 0.0f, offset);
        absolute.textRect = OffsetRect(bubble.textRect, 0.0f, offset);
        const uint32_t position = ChatTextPositionAt(absolute, x, y);
        if (position != chatSelectionEnd_) {
            chatSelectionEnd_ = position;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return;
    }
}

void Application::ClearChatSelection() {
    chatSelecting_ = false;
    chatSelectionMessage_ = -1;
    chatSelectionStart_ = 0;
    chatSelectionEnd_ = 0;
    if (GetCapture() == hwnd_) {
        ReleaseCapture();
    }
}

bool Application::CopyChatSelection() {
    const data::ChatSession* session = ActiveChatSession();
    if (session == nullptr || chatSelectionMessage_ < 0 ||
        chatSelectionMessage_ >= static_cast<int>(session->messages.size())) {
        return false;
    }
    const uint32_t begin = std::min(chatSelectionStart_, chatSelectionEnd_);
    const uint32_t end = std::max(chatSelectionStart_, chatSelectionEnd_);
    const std::wstring& text = session->messages[static_cast<size_t>(chatSelectionMessage_)].text;
    if (begin >= end || begin >= text.size()) {
        return false;
    }
    const std::wstring selected = text.substr(begin, std::min<uint32_t>(end, static_cast<uint32_t>(text.size())) - begin);
    if (!CopyTextToClipboard(selected)) {
        aiStatus_ = L"复制失败";
        InvalidateRect(hwnd_, nullptr, FALSE);
        return true;
    }
    // 提示显示在对话面板内，不使用阅读页底部的「已复制」提示。
    aiStatus_ = L"已复制";
    InvalidateRect(hwnd_, nullptr, FALSE);
    return true;
}

void Application::DrawChatConversation(const D2D1_RECT_F& rect, const ThemeColors& colors) {
    const TextFormats& formats = renderer_.Formats();
    renderer_.FillRoundedRect(rect, 9.0f, colors.surfaceAlt);

    std::vector<ChatBubble> bubbles;
    BuildChatBubbles(rect, bubbles);
    if (bubbles.empty()) {
        renderer_.DrawTextBlock(aiStatus_.empty() ? L"输入问题后发送，回复将流式显示。" : aiStatus_,
                                D2D1::RectF(rect.left + 12.0f, rect.top + 12.0f,
                                            rect.right - 12.0f, rect.top + 36.0f),
                                formats.caption.Get(), colors.secondaryText);
        chatScrollMax_ = 0.0f;
        chatScroll_ = 0.0f;
        return;
    }

    const float contentHeight = bubbles.back().top + bubbles.back().height + 8.0f;
    chatScrollMax_ = std::max(0.0f, contentHeight - (rect.bottom - rect.top));
    if (aiStreaming_) {
        // 流式输出时始终跟随最新内容。
        chatScroll_ = chatScrollMax_;
    }
    chatScroll_ = std::clamp(chatScroll_, 0.0f, chatScrollMax_);
    const float offset = rect.top - chatScroll_;

    BeginClip(rect);
    for (const ChatBubble& bubble : bubbles) {
        const D2D1_RECT_F bubbleRect = OffsetRect(bubble.bubble, 0.0f, offset);
        if (bubbleRect.bottom < rect.top || bubbleRect.top > rect.bottom) {
            continue;
        }
        const D2D1_RECT_F textRect = OffsetRect(bubble.textRect, 0.0f, offset);
        renderer_.FillRoundedRect(bubbleRect, 8.0f, colors.surface);
        if (bubble.isUser) {
            // 用户消息在右侧，强调色竖条放在右边缘。
            renderer_.FillRect(D2D1::RectF(bubbleRect.right - 3.0f, bubbleRect.top + 8.0f,
                                           bubbleRect.right, bubbleRect.bottom - 8.0f),
                               colors.accent);
        }
        renderer_.DrawTextBlock(bubble.isUser ? L"你" : L"AI",
                                D2D1::RectF(bubbleRect.left + 14.0f, bubbleRect.top + 4.0f,
                                            bubbleRect.right - 60.0f,
                                            bubbleRect.top + kChatBubbleHeader + 4.0f),
                                formats.caption.Get(),
                                bubble.isUser ? colors.accent : colors.secondaryText);
        if (bubble.messageIndex >= 0) {
            DrawPlainButton(L"复制",
                            D2D1::RectF(bubbleRect.right - 50.0f, bubbleRect.top + 4.0f,
                                        bubbleRect.right - 8.0f, bubbleRect.top + 22.0f),
                            HitKind::ChatCopyMessage, colors, bubble.messageIndex);
        }
        if (!bubble.thinking.empty()) {
            // 思考过程：点击标题行展开或折叠。
            const D2D1_RECT_F toggleRect = OffsetRect(bubble.thinkingToggleRect, 0.0f, offset);
            renderer_.DrawTextBlock(bubble.thinkingExpanded ? L"思考过程 ▴" : L"思考过程 ▾",
                                    toggleRect, formats.caption.Get(), colors.secondaryText);
            AddHit(toggleRect, HitKind::ChatThinkingToggle, bubble.messageIndex);
            if (bubble.thinkingExpanded && bubble.thinkingLayout) {
                const D2D1_RECT_F thinkingRect = OffsetRect(bubble.thinkingRect, 0.0f, offset);
                renderer_.DrawLayout(bubble.thinkingLayout.Get(),
                                     D2D1::Point2F(thinkingRect.left, thinkingRect.top),
                                     colors.secondaryText);
            }
        }
        if (bubble.layout) {
            DrawChatSelection(bubble, textRect, colors);
            renderer_.DrawLayout(bubble.layout.Get(),
                                 D2D1::Point2F(textRect.left, textRect.top), colors.text);
        }
    }
    EndClip();

    // 对话内容滚动条：内容超出可视高度时显示，可拖动定位。
    if (chatScrollMax_ > 0.0f) {
        const D2D1_RECT_F track = ChatScrollbarTrackRect();
        renderer_.FillRoundedRect(track, 4.0f, colors.divider);
        const float trackHeight = track.bottom - track.top;
        const float contentHeight = trackHeight + chatScrollMax_;
        const float thumbHeight = std::max(28.0f, trackHeight * trackHeight / contentHeight);
        const float ratio = chatScroll_ / chatScrollMax_;
        const float thumbTop = track.top + (trackHeight - thumbHeight) * ratio;
        renderer_.FillRoundedRect(
            D2D1::RectF(track.left, thumbTop, track.right, thumbTop + thumbHeight), 4.0f,
            colors.accent);
        AddHit(D2D1::RectF(track.left - 2.0f, track.top, track.right + 2.0f, track.bottom),
               HitKind::ChatScrollbar);
    }
}

D2D1_RECT_F Application::ChatSessionPickerPanelRect() const {
    const float width = std::min(480.0f, widthDip_ - 80.0f);
    const float height = std::min(440.0f, heightDip_ - 100.0f);
    const float left = (widthDip_ - width) * 0.5f;
    const float top = (heightDip_ - height) * 0.5f;
    return D2D1::RectF(left, top, left + width, top + height);
}

D2D1_RECT_F Application::ChatSessionPickerListRect() const {
    const D2D1_RECT_F panel = ChatSessionPickerPanelRect();
    return D2D1::RectF(panel.left + 20.0f, panel.top + 76.0f, panel.right - 20.0f,
                       panel.bottom - 16.0f);
}

D2D1_RECT_F Application::ShortcutPanelRect() const {
    const float width = std::min(470.0f, widthDip_ - 80.0f);
    const float height = std::min(420.0f, heightDip_ - 100.0f);
    const float left = (widthDip_ - width) * 0.5f;
    const float top = (heightDip_ - height) * 0.5f;
    return D2D1::RectF(left, top, left + width, top + height);
}

void Application::OpenShortcutSettings() {
    shortcutSettingsVisible_ = true;
    shortcutCaptureIndex_ = -1;
    InvalidateRect(hwnd_, nullptr, FALSE);
    UpdateWindow(hwnd_);
}

void Application::CloseShortcutSettings() {
    shortcutSettingsVisible_ = false;
    shortcutCaptureIndex_ = -1;
    InvalidateRect(hwnd_, nullptr, FALSE);
    UpdateWindow(hwnd_);
}

void Application::AssignShortcut(UINT virtualKey) {
    if (shortcutCaptureIndex_ < 0 ||
        shortcutCaptureIndex_ >= static_cast<int>(kShortcutRowCount)) {
        return;
    }
    if (VkFromKeyName(KeyNameFromVk(virtualKey)) == 0) {
        return;
    }
    const std::wstring combo = ShortcutFromKey(virtualKey);
    const std::wstring action = kShortcutRows[shortcutCaptureIndex_].action;
    data::ReaderSettings& settings = library_.Settings();
    bool updated = false;
    for (data::ShortcutBinding& binding : settings.shortcuts) {
        if (binding.action == action) {
            binding.combo = combo;
            updated = true;
            break;
        }
    }
    if (!updated) {
        settings.shortcuts.push_back(data::ShortcutBinding{action, combo});
    }
    shortcutCaptureIndex_ = -1;
    std::wstring error;
    library_.Save(error);
    InvalidateRect(hwnd_, nullptr, FALSE);
    UpdateWindow(hwnd_);
}

void Application::ResetShortcuts() {
    library_.Settings().shortcuts = data::DefaultShortcuts();
    shortcutCaptureIndex_ = -1;
    std::wstring error;
    library_.Save(error);
    InvalidateRect(hwnd_, nullptr, FALSE);
    UpdateWindow(hwnd_);
}

void Application::DrawShortcutSettings(const ThemeColors& colors) {
    if (!shortcutSettingsVisible_) {
        return;
    }
    const TextFormats& formats = renderer_.Formats();
    const data::ReaderSettings& settings = library_.Settings();
    renderer_.FillRect(D2D1::RectF(0.0f, 0.0f, widthDip_, heightDip_),
                       ParseHexColor(L"#000000", 0.35f));

    const D2D1_RECT_F panel = ShortcutPanelRect();
    renderer_.FillRoundedRect(panel, 12.0f, colors.surface);
    renderer_.StrokeRoundedRect(panel, 12.0f, colors.divider, 1.0f);
    renderer_.DrawTextBlock(L"快捷键映射",
                            D2D1::RectF(panel.left + 24.0f, panel.top + 16.0f,
                                        panel.right - 150.0f, panel.top + 52.0f),
                            formats.title.Get(), colors.text);
    DrawButton(L"恢复默认",
               D2D1::RectF(panel.right - 136.0f, panel.top + 14.0f, panel.right - 24.0f,
                           panel.top + 46.0f),
               HitKind::ShortcutReset, colors, false, false);
    renderer_.DrawLine(D2D1::Point2F(panel.left + 24.0f, panel.top + 60.0f),
                       D2D1::Point2F(panel.right - 24.0f, panel.top + 60.0f), colors.divider,
                       1.0f);

    const std::vector<data::ShortcutBinding> defaults = data::DefaultShortcuts();
    const auto defaultCombo = [&defaults](const wchar_t* action) {
        for (const data::ShortcutBinding& binding : defaults) {
            if (binding.action == action) {
                return binding.combo;
            }
        }
        return std::wstring();
    };

    float rowTop = panel.top + 72.0f;
    for (size_t i = 0; i < kShortcutRowCount; ++i) {
        const D2D1_RECT_F rowRect =
            D2D1::RectF(panel.left + 16.0f, rowTop, panel.right - 16.0f, rowTop + 40.0f);
        renderer_.DrawTextBlock(kShortcutRows[i].label,
                                D2D1::RectF(rowRect.left + 10.0f, rowRect.top,
                                            rowRect.right - 170.0f, rowRect.bottom),
                                formats.listItem.Get(), colors.text);
        const D2D1_RECT_F comboRect =
            D2D1::RectF(rowRect.right - 160.0f, rowRect.top + 5.0f, rowRect.right - 8.0f,
                        rowRect.top + 35.0f);
        const bool capturing =
            shortcutCaptureIndex_ >= 0 &&
            shortcutCaptureIndex_ == static_cast<int>(i);
        renderer_.FillRoundedRect(comboRect, 7.0f, colors.surfaceAlt);
        renderer_.StrokeRoundedRect(comboRect, 7.0f,
                                    capturing ? colors.accent : colors.divider,
                                    capturing ? 1.6f : 1.0f);
        const std::wstring text =
            capturing ? std::wstring(L"按下新的组合键…")
                      : ShortcutCombo(settings, kShortcutRows[i].action,
                                      defaultCombo(kShortcutRows[i].action).c_str());
        renderer_.DrawTextBlock(text, comboRect, formats.centered.Get(), colors.text);
        AddHit(rowRect, HitKind::ShortcutRow, static_cast<int>(i));
        rowTop += 44.0f;
    }

    renderer_.DrawTextBlock(
        L"点击一行后按下新的组合键，Esc 取消。上一页/下一页另支持 ↑ ↓ PageUp PageDown Space。",
        D2D1::RectF(panel.left + 24.0f, rowTop + 2.0f, panel.right - 24.0f, rowTop + 24.0f),
        formats.caption.Get(), colors.secondaryText);
    DrawButton(L"关闭",
               D2D1::RectF(panel.right - 136.0f, panel.bottom - 50.0f, panel.right - 24.0f,
                           panel.bottom - 14.0f),
               HitKind::ShortcutClose, colors, false, false);
}

void Application::DrawChatSessionPicker(const ThemeColors& colors) {
    if (!chatSessionPickerVisible_) {
        return;
    }
    const TextFormats& formats = renderer_.Formats();
    renderer_.FillRect(D2D1::RectF(0.0f, 0.0f, widthDip_, heightDip_),
                       ParseHexColor(L"#000000", 0.35f));

    const D2D1_RECT_F panel = ChatSessionPickerPanelRect();
    renderer_.FillRoundedRect(panel, 12.0f, colors.surface);
    renderer_.StrokeRoundedRect(panel, 12.0f, colors.divider, 1.0f);
    renderer_.DrawTextBlock(L"对话记录",
                            D2D1::RectF(panel.left + 24.0f, panel.top + 16.0f,
                                        panel.right - 70.0f, panel.top + 52.0f),
                            formats.title.Get(), colors.text);
    DrawButton(L"×",
               D2D1::RectF(panel.right - 52.0f, panel.top + 14.0f, panel.right - 18.0f,
                           panel.top + 46.0f),
               HitKind::ChatSessionClose, colors, false, false);
    renderer_.DrawLine(D2D1::Point2F(panel.left + 24.0f, panel.top + 60.0f),
                       D2D1::Point2F(panel.right - 24.0f, panel.top + 60.0f), colors.divider,
                       1.0f);

    const D2D1_RECT_F listRect = ChatSessionPickerListRect();
    const data::BookRecord* record = library_.Find(activeBookId_);
    if (record == nullptr || record->chatSessions.empty()) {
        renderer_.DrawTextBlock(L"还没有对话记录。",
                                D2D1::RectF(listRect.left, listRect.top, listRect.right,
                                            listRect.top + 24.0f),
                                formats.caption.Get(), colors.secondaryText);
        chatSessionPickerScrollMax_ = 0.0f;
        chatSessionPickerScroll_ = 0.0f;
        return;
    }

    // 按更新时间倒序显示，最近对话在最上方。
    std::vector<const data::ChatSession*> ordered;
    ordered.reserve(record->chatSessions.size());
    for (const data::ChatSession& session : record->chatSessions) {
        ordered.push_back(&session);
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const data::ChatSession* first, const data::ChatSession* second) {
                  return first->updatedAt > second->updatedAt;
              });

    constexpr float kRowHeight = 62.0f;
    const float contentHeight = static_cast<float>(ordered.size()) * kRowHeight;
    chatSessionPickerScrollMax_ =
        std::max(0.0f, contentHeight - (listRect.bottom - listRect.top));
    chatSessionPickerScroll_ =
        std::clamp(chatSessionPickerScroll_, 0.0f, chatSessionPickerScrollMax_);

    BeginClip(listRect);
    for (size_t i = 0; i < ordered.size(); ++i) {
        const data::ChatSession* session = ordered[i];
        const float top = listRect.top - chatSessionPickerScroll_ +
                          static_cast<float>(i) * kRowHeight;
        const D2D1_RECT_F rowRect =
            D2D1::RectF(listRect.left, top, listRect.right, top + kRowHeight - 6.0f);
        const bool current = session->id == activeChatSessionId_;
        if (current) {
            renderer_.FillRoundedRect(rowRect, 8.0f, colors.surfaceAlt);
            renderer_.FillRect(D2D1::RectF(rowRect.left, rowRect.top + 10.0f,
                                           rowRect.left + 3.0f, rowRect.bottom - 10.0f),
                               colors.accent);
        } else if (IsHovered(rowRect)) {
            renderer_.FillRoundedRect(rowRect, 8.0f, colors.surfaceAlt);
        }
        renderer_.DrawTextBlock(util::Ellipsize(session->title, 22),
                                D2D1::RectF(rowRect.left + 14.0f, rowRect.top + 9.0f,
                                            rowRect.right - 66.0f, rowRect.top + 33.0f),
                                formats.listItem.Get(),
                                current ? colors.accent : colors.text);
        const std::wstring subtitle =
            std::to_wstring(session->messages.size()) + L" 条消息 · " +
            util::FormatDateTime(session->updatedAt);
        renderer_.DrawTextBlock(util::Ellipsize(subtitle, 30),
                                D2D1::RectF(rowRect.left + 14.0f, rowRect.top + 33.0f,
                                            rowRect.right - 66.0f, rowRect.top + 53.0f),
                                formats.caption.Get(), colors.secondaryText);
        const D2D1_RECT_F deleteRect =
            D2D1::RectF(rowRect.right - 58.0f, rowRect.top + 18.0f, rowRect.right - 10.0f,
                        rowRect.top + 44.0f);
        DrawButton(L"删除", deleteRect, HitKind::ChatSessionDelete, colors, false, false, -1,
                   session->id);
        AddHit(rowRect, HitKind::ChatSessionItem, -1, session->id);
    }
    EndClip();
}

void Application::DrawAiSettings(const ThemeColors& colors) {
    if (!aiSettingsVisible_) {
        return;
    }
    const TextFormats& formats = renderer_.Formats();
    const data::ReaderSettings& settings = library_.Settings();
    renderer_.FillRect(D2D1::RectF(0.0f, 0.0f, widthDip_, heightDip_),
                       ParseHexColor(L"#000000", 0.35f));

    const D2D1_RECT_F panel = AiSettingsPanelRect();
    renderer_.FillRoundedRect(panel, 12.0f, colors.surface);
    renderer_.StrokeRoundedRect(panel, 12.0f, colors.divider, 1.0f);
    renderer_.DrawTextBlock(L"AI 接口设置",
                            D2D1::RectF(panel.left + 24.0f, panel.top + 16.0f,
                                        panel.right - 24.0f, panel.top + 58.0f),
                            formats.title.Get(), colors.text);
    renderer_.DrawLine(D2D1::Point2F(panel.left + 24.0f, panel.top + 66.0f),
                       D2D1::Point2F(panel.right - 24.0f, panel.top + 66.0f), colors.divider,
                       1.0f);

    // 协议选择：OpenAI 兼容与 Anthropic Messages。
    const bool anthropic = settings.aiProtocol == L"anthropic";
    renderer_.DrawTextBlock(L"接口协议",
                            D2D1::RectF(panel.left + 24.0f, panel.top + 76.0f,
                                        panel.right - 24.0f, panel.top + 96.0f),
                            formats.caption.Get(), colors.secondaryText);
    DrawButton(L"OpenAI 兼容",
               D2D1::RectF(panel.left + 24.0f, panel.top + 100.0f, panel.left + 174.0f,
                           panel.top + 134.0f),
               HitKind::ChatProtocolOpenAi, colors, false, !anthropic);
    DrawButton(L"Anthropic",
               D2D1::RectF(panel.left + 184.0f, panel.top + 100.0f, panel.left + 334.0f,
                           panel.top + 134.0f),
               HitKind::ChatProtocolAnthropic, colors, false, anthropic);

    struct FieldDef {
        ChatFocus focus;
        const wchar_t* label;
        HitKind kind;
        const std::wstring* value;
    };
    const FieldDef fields[] = {
        {ChatFocus::BaseUrl, L"接口地址", HitKind::ChatBaseUrl, &settings.aiBaseUrl},
        {ChatFocus::ApiKey, L"接口密钥", HitKind::ChatApiKey, &settings.aiApiKey},
        {ChatFocus::Model, L"模型名称", HitKind::ChatModel, &settings.aiModel},
    };
    for (const FieldDef& field : fields) {
        const D2D1_RECT_F boxRect = AiSettingsFieldRect(field.focus);
        renderer_.DrawTextBlock(field.label,
                                D2D1::RectF(boxRect.left, boxRect.top - 20.0f, boxRect.right,
                                            boxRect.top - 2.0f),
                                formats.caption.Get(), colors.secondaryText);
        const bool focused = chatFocus_ == field.focus;
        renderer_.FillRoundedRect(boxRect, 8.0f, colors.surfaceAlt);
        renderer_.StrokeRoundedRect(boxRect, 8.0f, focused ? colors.accent : colors.divider,
                                    focused ? 1.6f : 1.0f);
        // 密钥以圆点掩码显示，避免肩窥。
        const std::wstring shown =
            field.focus == ChatFocus::ApiKey ? std::wstring(field.value->size(), L'\u2022')
                                             : *field.value;
        const D2D1_RECT_F textRect =
            D2D1::RectF(boxRect.left + 10.0f, boxRect.top + 7.0f, boxRect.right - 10.0f,
                        boxRect.bottom - 7.0f);
        renderer_.DrawTextBlock(shown, textRect, formats.listItem.Get(),
                                field.value->empty() ? colors.secondaryText : colors.text);
        if (focused && field.focus != ChatFocus::ApiKey) {
            DrawInputSelection(textRect, formats.listItem.Get(), false, colors);
        }
        AddHit(boxRect, field.kind);
    }

    // 思考等级：关闭 / 低 / 中 / 高，按协议分别映射到 reasoning_effort 与 thinking 预算。
    renderer_.DrawTextBlock(L"思考等级",
                            D2D1::RectF(panel.left + 24.0f, panel.top + 326.0f,
                                        panel.right - 24.0f, panel.top + 346.0f),
                            formats.caption.Get(), colors.secondaryText);
    static const wchar_t kThinkingLabels[4][8] = {L"关闭", L"低", L"中", L"高"};
    for (int i = 0; i < 4; ++i) {
        const float left = panel.left + 24.0f + static_cast<float>(i) * 82.0f;
        DrawButton(kThinkingLabels[i],
                   D2D1::RectF(left, panel.top + 350.0f, left + 74.0f, panel.top + 382.0f),
                   HitKind::AiThinkingLevel, colors, false, settings.aiThinkingLevel == i, i);
    }

    DrawButton(L"保存",
               D2D1::RectF(panel.left + 24.0f, panel.top + 400.0f, panel.left + 144.0f,
                           panel.top + 438.0f),
               HitKind::AiSettingsSave, colors, true, false);
    DrawButton(L"关闭",
               D2D1::RectF(panel.right - 124.0f, panel.top + 400.0f, panel.right - 24.0f,
                           panel.top + 438.0f),
               HitKind::AiSettingsClose, colors, false, false);
}

D2D1_RECT_F Application::FontPickerPanelRect() const {
    constexpr float kPanelWidthDip = 560.0f;
    constexpr float kPanelHeightDip = 520.0f;
    const float width = std::min(kPanelWidthDip, widthDip_ - 80.0f);
    const float height = std::min(kPanelHeightDip, heightDip_ - 100.0f);
    const float left = (widthDip_ - width) * 0.5f;
    const float top = (heightDip_ - height) * 0.5f;
    return D2D1::RectF(left, top, left + width, top + height);
}

D2D1_RECT_F Application::FontPickerListRect() const {
    const D2D1_RECT_F panel = FontPickerPanelRect();
    return D2D1::RectF(panel.left + 20.0f, panel.top + 76.0f, panel.right - 20.0f,
                       panel.bottom - 16.0f);
}

std::wstring Application::CurrentFontDisplayName() const {
    const std::wstring& family = library_.Settings().fontFamily;
    for (const FontFamilyName& entry : fontFamilies_) {
        if (entry.canonical == family || entry.localized == family) {
            return entry.localized;
        }
    }
    return family;
}

void Application::SelectFontFamily(const std::wstring& family) {
    data::ReaderSettings& settings = library_.Settings();
    settings.fontFamily = family;
    fontPickerVisible_ = false;
    // ApplySettings 会重建文本格式、清空排版缓存并落盘。
    ApplySettings();
    // 弹框已关闭，立即重绘以同步命中区域。
    UpdateWindow(hwnd_);
}

void Application::DrawFontPicker(const ThemeColors& colors) {
    if (!fontPickerVisible_) {
        return;
    }
    const TextFormats& formats = renderer_.Formats();
    const data::ReaderSettings& settings = library_.Settings();
    renderer_.FillRect(D2D1::RectF(0.0f, 0.0f, widthDip_, heightDip_),
                       ParseHexColor(L"#000000", 0.35f));

    const D2D1_RECT_F panel = FontPickerPanelRect();
    renderer_.FillRoundedRect(panel, 12.0f, colors.surface);
    renderer_.StrokeRoundedRect(panel, 12.0f, colors.divider, 1.0f);
    renderer_.DrawTextBlock(L"选择字体",
                            D2D1::RectF(panel.left + 24.0f, panel.top + 16.0f,
                                        panel.right - 70.0f, panel.top + 52.0f),
                            formats.title.Get(), colors.text);
    DrawButton(L"×",
               D2D1::RectF(panel.right - 52.0f, panel.top + 14.0f, panel.right - 18.0f,
                           panel.top + 46.0f),
               HitKind::FontPickerClose, colors, false, false);
    renderer_.DrawLine(D2D1::Point2F(panel.left + 24.0f, panel.top + 60.0f),
                       D2D1::Point2F(panel.right - 24.0f, panel.top + 60.0f), colors.divider,
                       1.0f);

    const D2D1_RECT_F listRect = FontPickerListRect();
    if (fontFamilies_.empty()) {
        renderer_.DrawTextBlock(L"未找到可显示中文的字体。",
                                D2D1::RectF(listRect.left, listRect.top, listRect.right,
                                            listRect.top + 24.0f),
                                formats.caption.Get(), colors.secondaryText);
        return;
    }

    constexpr size_t kColumns = 3;
    const float columnGap = 8.0f;
    const float rowGap = 8.0f;
    const float itemHeight = 34.0f;
    const float itemWidth =
        (listRect.right - listRect.left - columnGap * static_cast<float>(kColumns - 1)) /
        static_cast<float>(kColumns);
    const size_t rows = (fontFamilies_.size() + kColumns - 1) / kColumns;
    const float contentHeight =
        rows > 0 ? static_cast<float>(rows) * (itemHeight + rowGap) - rowGap : 0.0f;
    fontPickerScrollMax_ = std::max(0.0f, contentHeight - (listRect.bottom - listRect.top));
    fontPickerScroll_ = std::clamp(fontPickerScroll_, 0.0f, fontPickerScrollMax_);

    BeginClip(listRect);
    for (size_t i = 0; i < fontFamilies_.size(); ++i) {
        const size_t row = i / kColumns;
        const size_t column = i % kColumns;
        const float x = listRect.left + static_cast<float>(column) * (itemWidth + columnGap);
        const float top = listRect.top - fontPickerScroll_ +
                          static_cast<float>(row) * (itemHeight + rowGap);
        const D2D1_RECT_F itemRect = D2D1::RectF(x, top, x + itemWidth, top + itemHeight);
        DrawButton(fontFamilies_[i].localized, itemRect, HitKind::FontPickerItem, colors, false,
                   settings.fontFamily == fontFamilies_[i].canonical, -1,
                   fontFamilies_[i].canonical);
    }
    EndClip();
}

void Application::OpenAiSettings() {
    aiSettingsVisible_ = true;
    chatFocus_ = ChatFocus::None;
    StopCaretBlink();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Application::CloseAiSettings() {
    aiSettingsVisible_ = false;
    chatFocus_ = ChatFocus::None;
    StopCaretBlink();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Application::SaveAiSettings() {
    std::wstring error;
    const bool saved = library_.Save(error);
    aiSettingsVisible_ = false;
    chatFocus_ = ChatFocus::None;
    StopCaretBlink();
    ShowStatus(saved ? L"AI 接口设置已保存" : L"保存 AI 接口设置失败：" + error);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Application::DrawButton(const std::wstring& label, const D2D1_RECT_F& rect, HitKind kind,
                             const ThemeColors& colors, bool primary, bool active, int index,
                             const std::wstring& value) {
    const TextFormats& formats = renderer_.Formats();
    const bool hovered = IsHovered(rect);
    if (primary) {
        const D2D1_COLOR_F fill = hovered ? Darken(colors.accent, 0.9f) : colors.accent;
        renderer_.FillRoundedRect(rect, 9.0f, fill);
        renderer_.DrawTextBlock(label, rect, formats.button.Get(), colors.accentText);
    } else {
        if (hovered || active) {
            renderer_.FillRoundedRect(rect, 9.0f, colors.surfaceAlt);
        }
        renderer_.DrawTextBlock(label, rect, formats.button.Get(),
                                active ? colors.accent : colors.text);
        if (active) {
            renderer_.StrokeRoundedRect(rect, 9.0f, colors.accent, 1.0f);
        }
    }
    AddHit(rect, kind, index, value);
}

void Application::DrawPlainButton(const std::wstring& label, const D2D1_RECT_F& rect,
                                  HitKind kind, const ThemeColors& colors, int index,
                                  const std::wstring& value) {
    const TextFormats& formats = renderer_.Formats();
    if (IsHovered(rect)) {
        renderer_.FillRoundedRect(rect, 9.0f, colors.surfaceAlt);
    }
    // 常规字重，避免次要操作显得过于突出。
    renderer_.DrawTextBlock(label, rect, formats.centered.Get(), colors.text);
    AddHit(rect, kind, index, value);
}

void Application::AddHit(const D2D1_RECT_F& rect, HitKind kind, int index,
                         const std::wstring& value) {
    D2D1_RECT_F visible = rect;
    if (!clipStack_.empty()) {
        visible = IntersectRect(rect, clipStack_.back());
    }
    if (visible.right <= visible.left || visible.bottom <= visible.top) {
        return;
    }
    HitRegion region;
    region.rect = visible;
    region.kind = kind;
    region.index = index;
    region.value = value;
    hitRegions_.push_back(std::move(region));
}

bool Application::IsHovered(const D2D1_RECT_F& rect) const {
    return mouseInside_ && PointInside(rect, mouseX_, mouseY_);
}

const Application::HitRegion* Application::HitTest(float x, float y) const {
    for (auto it = hitRegions_.rbegin(); it != hitRegions_.rend(); ++it) {
        if (PointInside(it->rect, x, y)) {
            return &(*it);
        }
    }
    return nullptr;
}

void Application::BeginClip(const D2D1_RECT_F& rect) {
    clipStack_.push_back(rect);
    renderer_.PushClip(rect);
}

void Application::EndClip() {
    if (clipStack_.empty()) {
        return;
    }
    clipStack_.pop_back();
    renderer_.PopClip();
}

void Application::RefreshShelfOrder() {
    shelfOrder_.clear();
    const std::vector<data::BookRecord>& books = library_.Books();
    // 正则搜索：匹配书名、作者与文件名；表达式无效时不应用该条件。
    bool hasPattern = false;
    std::wregex pattern;
    if (!shelfSearchQuery_.empty()) {
        try {
            pattern = std::wregex(shelfSearchQuery_, std::regex_constants::icase);
            hasPattern = true;
        } catch (const std::regex_error&) {
            hasPattern = false;
        }
    }
    // 按当前虚拟目录筛选：空表示全部显示。
    for (size_t i = 0; i < books.size(); ++i) {
        bool include = true;
        if (shelfFilterCategory_ == kUncategorizedFilter) {
            include = books[i].categoryId.empty();
        } else if (!shelfFilterCategory_.empty()) {
            include = books[i].categoryId == shelfFilterCategory_;
        }
        if (include && hasPattern) {
            include = std::regex_search(books[i].title, pattern) ||
                      std::regex_search(books[i].author, pattern) ||
                      std::regex_search(books[i].fileName, pattern);
        }
        if (include) {
            shelfOrder_.push_back(i);
        }
    }
    std::sort(shelfOrder_.begin(), shelfOrder_.end(), [&books](size_t a, size_t b) {
        if (books[a].lastOpenedAt != books[b].lastOpenedAt) {
            return books[a].lastOpenedAt > books[b].lastOpenedAt;
        }
        return books[a].addedAt > books[b].addedAt;
    });
}

void Application::ApplyShelfSearch() {
    if (!shelfSearchQuery_.empty()) {
        try {
            std::wregex probe(shelfSearchQuery_, std::regex_constants::icase);
        } catch (const std::regex_error&) {
            ShowStatus(L"正则表达式无效，请检查括号与转义字符");
            return;
        }
    }
    shelfScroll_ = 0.0f;
    RefreshShelfOrder();
    ShowStatus(shelfSearchQuery_.empty()
                   ? std::wstring(L"已清除书架搜索")
                   : L"正则搜索：找到 " + std::to_wstring(shelfOrder_.size()) + L" 本");
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Application::ClearShelfSearch() {
    shelfSearchFocused_ = false;
    StopCaretBlink();
    if (!shelfSearchQuery_.empty()) {
        shelfSearchQuery_.clear();
        shelfScroll_ = 0.0f;
        RefreshShelfOrder();
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Application::ShowStatus(const std::wstring& text) {
    statusText_ = text;
    statusExpiry_ = util::CurrentUnixSeconds() + 8;
    SetTimer(hwnd_, 1, 1000, nullptr);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Application::ImportFiles(const std::vector<std::filesystem::path>& paths) {
    int added = 0;
    int existed = 0;
    std::wstring lastError;

    for (const std::filesystem::path& path : paths) {
        bool already = false;
        std::wstring error;
        if (library_.AddBook(path, error, already)) {
            if (already) {
                ++existed;
            } else {
                ++added;
            }
        } else {
            lastError = error;
        }
    }
    coverCache_.clear();
    RefreshShelfOrder();

    if (added > 0) {
        if (view_ == ViewMode::Reader) {
            CloseBook();
        }
        std::wstring message = L"已导入 " + std::to_wstring(added) + L" 本";
        if (existed > 0) {
            message += L"，另有 " + std::to_wstring(existed) + L" 本已在书架中";
        }
        ShowStatus(message);
    } else if (existed > 0) {
        ShowStatus(L"所选书籍已存在于书架中");
    } else if (!lastError.empty()) {
        ShowStatus(L"导入失败：" + lastError);
    } else {
        ShowStatus(L"没有可导入的文件");
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Application::OpenFileDialog() {
    ComPtr<IFileOpenDialog> dialog;
    const HRESULT created = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                             IID_PPV_ARGS(&dialog));
    if (FAILED(created) || !dialog) {
        ShowStatus(L"无法创建文件选择对话框");
        return;
    }

    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_ALLOWMULTISELECT | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
    const COMDLG_FILTERSPEC filters[] = {
        {L"电子书（UMD / 文本）", L"*.umd;*.txt"},
        {L"UMD 电子书", L"*.umd"},
        {L"文本文件", L"*.txt"},
        {L"所有文件", L"*.*"}};
    dialog->SetFileTypes(4, filters);
    dialog->SetTitle(L"选择电子书（UMD 或 TXT）");

    // 恢复到上一次打开文件所在的目录，避免每次从固定位置重新浏览。
    const std::wstring lastDirectory = library_.Settings().lastOpenDirectory;
    if (!lastDirectory.empty()) {
        std::error_code errorCode;
        if (std::filesystem::exists(lastDirectory, errorCode)) {
            ComPtr<IShellItem> folder;
            if (SUCCEEDED(SHCreateItemFromParsingName(lastDirectory.c_str(), nullptr,
                                                      IID_PPV_ARGS(&folder))) &&
                folder) {
                dialog->SetFolder(folder.Get());
            }
        }
    }

    if (FAILED(dialog->Show(hwnd_))) {
        return;
    }

    ComPtr<IShellItemArray> results;
    if (FAILED(dialog->GetResults(&results)) || !results) {
        return;
    }
    DWORD count = 0;
    results->GetCount(&count);
    std::vector<std::filesystem::path> paths;
    for (DWORD i = 0; i < count; ++i) {
        ComPtr<IShellItem> item;
        if (FAILED(results->GetItemAt(i, &item)) || !item) {
            continue;
        }
        PWSTR rawPath = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &rawPath)) && rawPath) {
            paths.emplace_back(rawPath);
            CoTaskMemFree(rawPath);
        }
    }
    if (!paths.empty()) {
        const std::filesystem::path directory = paths.front().parent_path();
        if (!directory.empty() &&
            directory.wstring() != library_.Settings().lastOpenDirectory) {
            library_.Settings().lastOpenDirectory = directory.wstring();
            std::wstring saveError;
            library_.Save(saveError);
        }
        ImportFiles(paths);
    }
}

void Application::OpenStartupFiles() {
    if (startupPaths_.empty()) {
        return;
    }
    const std::vector<std::wstring> paths = std::move(startupPaths_);
    startupPaths_.clear();

    std::vector<std::filesystem::path> files;
    files.reserve(paths.size());
    for (const std::wstring& path : paths) {
        files.emplace_back(path);
    }
    ImportFiles(files);
    for (const std::wstring& path : paths) {
        OpenPath(path);
        // 仅打开第一个成功匹配的书籍，其余保留在书架中。
        if (view_ == ViewMode::Reader) {
            break;
        }
    }
}

void Application::OpenPath(const std::wstring& path) {
    std::error_code errorCode;
    std::filesystem::path target = std::filesystem::weakly_canonical(path, errorCode);
    if (errorCode) {
        target = std::filesystem::absolute(path, errorCode);
    }
    if (errorCode) {
        return;
    }
    for (const data::BookRecord& record : library_.Books()) {
        std::error_code compareError;
        std::filesystem::path recordPath =
            std::filesystem::weakly_canonical(record.path, compareError);
        if (compareError) {
            recordPath = std::filesystem::path(record.path);
        }
        if (recordPath == target) {
            OpenBook(record.id);
            return;
        }
    }
}

void Application::OpenExternalFile(const std::wstring& path) {
    std::error_code errorCode;
    if (!std::filesystem::exists(path, errorCode)) {
        ShowStatus(L"文件不存在：" + path);
        return;
    }
    ImportFiles({std::filesystem::path(path)});
    OpenPath(path);
}

bool Application::ForwardToExistingInstance() const {
    // 新实例可能比已有窗口更早完成启动，稍作重试等待窗口就绪。
    HWND target = nullptr;
    for (int attempt = 0; attempt < 30 && target == nullptr; ++attempt) {
        target = FindWindowW(kWindowClass, nullptr);
        if (target == nullptr) {
            Sleep(100);
        }
    }
    if (target == nullptr) {
        return false;
    }
    for (const std::wstring& path : startupPaths_) {
        COPYDATASTRUCT data{};
        data.dwData = kCopyDataOpenFile;
        data.cbData = static_cast<DWORD>((path.size() + 1) * sizeof(wchar_t));
        data.lpData = const_cast<wchar_t*>(path.c_str());
        DWORD_PTR result = 0;
        SendMessageTimeoutW(target, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data),
                            SMTO_ABORTIFHUNG | SMTO_BLOCK, 4000, &result);
    }
    if (IsIconic(target)) {
        ShowWindow(target, SW_RESTORE);
    }
    SetForegroundWindow(target);
    return true;
}

bool Application::HandleCopyData(const COPYDATASTRUCT& data) {
    if (data.dwData != kCopyDataOpenFile || data.lpData == nullptr ||
        data.cbData < sizeof(wchar_t)) {
        return false;
    }
    const size_t count = data.cbData / sizeof(wchar_t);
    std::wstring path(static_cast<const wchar_t*>(data.lpData), count);
    while (!path.empty() && path.back() == L'\0') {
        path.pop_back();
    }
    if (path.empty()) {
        return false;
    }
    OpenExternalFile(path);
    return true;
}

void Application::AssociateFileExtension(const std::wstring& extension,
                                         const std::wstring& progId,
                                         const std::wstring& description) {
    const std::wstring executable = ExecutablePath();
    if (executable.empty()) {
        ShowStatus(L"无法获取程序路径，注册文件关联失败");
        return;
    }
    std::wstring message = L"要把 " + extension + L" 文件关联到本程序吗？\n";
    if (extension == L".txt") {
        message += L"这会更改当前用户对 .txt 的默认打开方式（例如不再使用记事本）。\n";
    }
    message += L"仅修改当前用户设置，可在系统「默认应用」中随时改回。";
    if (MessageBoxW(hwnd_, message.c_str(), L"文件关联", MB_YESNO | MB_ICONQUESTION) != IDYES) {
        return;
    }

    const bool succeeded = RegisterFileAssociation(extension, progId, description, executable);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    ShowStatus(succeeded ? L"已将 " + extension + L" 文件关联到本程序"
                         : L"注册 " + extension + L" 文件关联失败，请检查当前用户注册表写入权限");
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Application::DisassociateFileExtension(const std::wstring& extension,
                                            const std::wstring& progId) {
    const std::wstring message = L"要取消 " + extension + L" 文件与本程序的关联吗？\n" +
                                 L"仅移除本程序写入的当前用户注册表项。";
    if (MessageBoxW(hwnd_, message.c_str(), L"取消文件关联", MB_YESNO | MB_ICONQUESTION) !=
        IDYES) {
        return;
    }
    const bool succeeded = RemoveFileAssociation(extension, progId);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    ShowStatus(succeeded ? L"已取消 " + extension + L" 文件关联"
                         : L"取消 " + extension + L" 文件关联失败");
    InvalidateRect(hwnd_, nullptr, FALSE);
}

std::wstring Application::BuildSpeechText() const {
    // 有选中文本时优先朗读选区，否则朗读当前页。
    if (selectionActive_ && !activeChapterRaw_.empty()) {
        const uint32_t limit = static_cast<uint32_t>(activeChapterRaw_.size());
        const uint32_t from = std::min(chapterDisplay_.ToOriginal(selectionStartDisplay_), limit);
        const uint32_t to = std::min(chapterDisplay_.ToOriginal(selectionEndDisplay_), limit);
        std::wstring text;
        if (to > from) {
            const std::wstring& selected = activeChapterRaw_;
            text.reserve(to - from);
            for (uint32_t i = from; i < to; ++i) {
                const wchar_t character = selected[i];
                if (character == 0x3000 || character == L'\t' || character == L'\r') {
                    continue;
                }
                // 换行处补一个句号，避免跨行文字连读成一串。
                text.push_back(character == L'\n' ? L'。' : character);
            }
        }
        // 选区存在时只朗读选区，不再回退到整页。
        return text;
    }

    if (pager_.PageCount() == 0) {
        return std::wstring();
    }
    const std::wstring& displayText = chapterDisplay_.Text();
    if (displayText.empty()) {
        return std::wstring();
    }
    const size_t pageIndex = std::min(activePage_, pager_.PageCount() - 1);
    const PageInfo& page = pager_.Page(pageIndex);
    const uint32_t length = static_cast<uint32_t>(displayText.size());
    const uint32_t start = std::min(page.startChar, length);
    const uint32_t end = std::min(page.endChar, length);
    if (end <= start) {
        return std::wstring();
    }
    std::wstring text;
    text.reserve(end - start);
    for (uint32_t i = start; i < end; ++i) {
        const wchar_t character = displayText[i];
        if (character == 0x3000 || character == L'\t' || character == L'\r') {
            continue;
        }
        // 换行处补一个句号，避免整页文字连读成一串。
        text.push_back(character == L'\n' ? L'。' : character);
    }
    return text;
}

void Application::ToggleSpeech() {
    if (speaking_) {
        StopSpeech();
        return;
    }
    if (!voice_) {
        ShowStatus(L"系统语音引擎不可用");
        return;
    }
    const std::wstring text = BuildSpeechText();
    if (text.empty()) {
        ShowStatus(selectionActive_ ? L"选中内容没有可朗读的文字"
                                    : L"当前页没有可朗读的文字");
        return;
    }
    const HRESULT result = voice_->Speak(text.c_str(), SPF_ASYNC | SPF_PURGEBEFORESPEAK, nullptr);
    if (FAILED(result)) {
        speaking_ = false;
        ShowStatus(L"朗读失败");
    } else {
        speaking_ = true;
        ShowStatus(selectionActive_ ? L"正在朗读选中文本" : L"正在朗读当前页");
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Application::StopSpeech() {
    if (voice_) {
        voice_->Speak(nullptr, SPF_PURGEBEFORESPEAK, nullptr);
    }
    speaking_ = false;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Application::SendAiMessage() {
    if (aiStreaming_) {
        aiStatus_ = L"上一轮回复尚未结束";
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    const std::wstring prompt = TrimNoteText(aiDraft_);
    if (prompt.empty()) {
        aiStatus_ = L"请输入问题";
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    aiDraft_.clear();
    // 清空输入框后必须把光标归位，否则下一次输入（尤其输入法上屏）会按旧下标插入。
    ResetInputCaretToEnd();
    StartAiRequest(prompt);
}

void Application::StartAiRequest(const std::wstring& prompt) {
    const data::ReaderSettings& settings = library_.Settings();
    std::wstring baseUrl = settings.aiBaseUrl;
    while (!baseUrl.empty() && (baseUrl.back() == L'/' || baseUrl.back() == L' ')) {
        baseUrl.pop_back();
    }
    if (baseUrl.empty() || settings.aiModel.empty()) {
        aiStatus_ = L"请先填写接口地址与模型名称";
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    data::ChatSession* session = EnsureChatSession();
    if (session == nullptr) {
        aiStatus_ = L"请先打开一本书再开始对话";
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    // 本轮问题作为用户消息写入会话，连续对话基于会话内的全部历史。
    data::ChatMessage userMessage;
    userMessage.role = L"user";
    userMessage.text = prompt;
    userMessage.createdAt = util::CurrentUnixSeconds();
    if (session->messages.empty()) {
        session->title = util::Ellipsize(prompt, 18);
    }
    session->updatedAt = userMessage.createdAt;
    session->messages.push_back(std::move(userMessage));
    activeChatSessionId_ = session->id;
    SaveChatState();

    std::vector<std::pair<std::wstring, std::wstring>> history;
    history.reserve(session->messages.size());
    for (const data::ChatMessage& message : session->messages) {
        history.emplace_back(message.role, message.text);
    }
    // 新一轮提问重置自动读取轮数。
    aiFollowUpCount_ = 0;
    aiLastReadEnd_ = CurrentGlobalOffset();
    LaunchAiRequest(std::move(history));
}

void Application::LaunchAiRequest(std::vector<std::pair<std::wstring, std::wstring>> history) {
    const data::ReaderSettings& settings = library_.Settings();
    std::wstring baseUrl = settings.aiBaseUrl;
    while (!baseUrl.empty() && (baseUrl.back() == L'/' || baseUrl.back() == L' ')) {
        baseUrl.pop_back();
    }
    if (baseUrl.empty() || settings.aiModel.empty()) {
        aiStatus_ = L"请先填写接口地址与模型名称";
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (history.empty()) {
        aiStatus_ = L"没有可发送的对话内容";
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    aiAnswer_.clear();
    aiThinking_.clear();
    aiStatus_ = L"正在生成…";
    aiStreaming_ = true;
    aiCancel_ = false;
    chatScrollMax_ = 0.0f;
    chatScroll_ = 0.0f;
    {
        std::lock_guard<std::mutex> lock(aiMutex_);
        aiPendingText_.clear();
        aiThinkingPending_.clear();
        aiPendingStatus_.clear();
        aiPendingDone_ = false;
    }
    if (aiThread_.joinable()) {
        aiThread_.join();
    }
    const bool anthropic = settings.aiProtocol == L"anthropic";
    const std::wstring url = baseUrl + (anthropic ? L"/messages" : L"/chat/completions");
    const std::wstring systemPrompt = BuildAiSystemPrompt();
    aiThread_ = std::thread([this, url, key = settings.aiApiKey, model = settings.aiModel,
                             protocol = settings.aiProtocol,
                             thinkingLevel = settings.aiThinkingLevel, systemPrompt,
                             history = std::move(history), target = hwnd_]() {
        RunAiRequest(url, key, model, protocol, thinkingLevel, systemPrompt, history, target);
    });
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Application::StopAiRequest() {
    if (!aiStreaming_) {
        return;
    }
    aiCancel_ = true;
    if (aiThread_.joinable()) {
        aiThread_.join();
    }
    {
        std::lock_guard<std::mutex> lock(aiMutex_);
        // 把尚未分发的增量并入当前回复，停止前的输出仍然保留。
        aiAnswer_ += aiPendingText_;
        aiPendingText_.clear();
        aiThinking_ += aiThinkingPending_;
        aiThinkingPending_.clear();
        aiPendingStatus_.clear();
        aiPendingDone_ = false;
    }
    aiStreaming_ = false;
    CommitAiAnswer(false);
    aiStatus_ = L"已停止生成";
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Application::PushAiChunk(const std::wstring& text, HWND target) {
    if (aiCancel_) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(aiMutex_);
        aiPendingText_ += text;
    }
    PostMessageW(target, kAiStreamMessage, 0, 0);
}

void Application::PushAiThinking(const std::wstring& text, HWND target) {
    if (text.empty()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(aiMutex_);
        aiThinkingPending_ += text;
    }
    PostMessageW(target, kAiStreamMessage, 0, 0);
}

void Application::PushAiDone(const std::wstring& status, HWND target) {
    {
        std::lock_guard<std::mutex> lock(aiMutex_);
        aiPendingStatus_ = status;
        aiPendingDone_ = true;
    }
    PostMessageW(target, kAiStreamMessage, 0, 0);
}

void Application::AppendAiChunks() {
    std::wstring chunk;
    std::wstring thinking;
    std::wstring status;
    bool done = false;
    {
        std::lock_guard<std::mutex> lock(aiMutex_);
        chunk.swap(aiPendingText_);
        thinking.swap(aiThinkingPending_);
        if (aiPendingDone_) {
            done = true;
            aiPendingDone_ = false;
            status.swap(aiPendingStatus_);
        }
    }
    if (!chunk.empty()) {
        aiAnswer_ += chunk;
    }
    if (!thinking.empty()) {
        aiThinking_ += thinking;
    }
    if (done) {
        aiStreaming_ = false;
        if (!aiAnswer_.empty()) {
            CommitAiAnswer(true);
            // 自动读取正文会立即发起下一轮请求，此时保持「正在生成」状态。
            if (!aiStreaming_) {
                aiStatus_ = status.empty() ? std::wstring(L"生成完成") : status;
            }
        } else {
            aiStatus_ = status.empty() ? std::wstring(L"没有收到回复内容") : status;
        }
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Application::CommitAiAnswer(bool allowFollowUp) {
    if (aiAnswer_.empty()) {
        return;
    }
    const std::wstring raw = aiAnswer_;
    aiAnswer_.clear();
    // 先执行回复中的控制命令（跳转、高亮、标注、读取章节或范围），展示文本中不保留命令。
    std::wstring cleaned;
    AiReadRange readRange;
    ProcessAiCommands(raw, cleaned, readRange);
    if (cleaned.empty()) {
        cleaned = L"（已执行阅读操作）";
    }
    data::ChatSession* session = EnsureChatSession();
    if (session == nullptr) {
        return;
    }
    data::ChatMessage reply;
    reply.role = L"assistant";
    reply.text = cleaned;
    reply.thinking = aiThinking_;
    aiThinking_.clear();
    reply.createdAt = util::CurrentUnixSeconds();
    session->updatedAt = reply.createdAt;
    session->messages.push_back(std::move(reply));
    activeChatSessionId_ = session->id;
    SaveChatState();
    // 模型可连续读取正文：每轮回复中的读取命令都会自动追加正文并继续请求，
    // 直到回复不再要求读取或达到轮数上限，避免命令循环。
    constexpr int kMaxAiFollowUps = 8;
    constexpr uint32_t kIntentReadChars = 6000;
    if (allowFollowUp && !aiCancel_ && aiFollowUpCount_ < kMaxAiFollowUps) {
        uint32_t nextStart = 0;
        uint32_t nextLength = 0;
        if (readRange.valid) {
            nextStart = readRange.start;
            nextLength = readRange.length;
        } else if (WantsMoreText(cleaned)) {
            // 模型只说明要读取却未给出范围时，从上次读取结束处继续读取一段正文。
            nextStart = aiLastReadEnd_;
            nextLength = kIntentReadChars;
        }
        if (nextLength > 0) {
            ++aiFollowUpCount_;
            StartAiFollowUp(nextStart, nextLength);
        }
    }
}

std::wstring Application::BuildAiSystemPrompt() const {
    std::wstring prompt;
    prompt += L"你是本书的阅读助手，负责解答与本书相关的问题，并可协助定位内容、添加高亮与标注。\n";
    if (!activeBook_.title.empty()) {
        prompt += L"书籍：《" + activeBook_.title + L"》";
        if (!activeBook_.author.empty()) {
            prompt += L"，作者：" + activeBook_.author;
        }
        prompt += L"\n";
    }
    const size_t chapterCount = activeBook_.chapters.size();
    prompt += L"共 " + std::to_wstring(chapterCount) + L" 章，全文 " +
              std::to_wstring(activeBook_.ContentCharCount()) + L" 个字符。\n";
    if (chapterCount > 0) {
        const int chapter = std::clamp(activeChapter_, 0, static_cast<int>(chapterCount) - 1);
        const uint32_t global = CurrentGlobalOffset();
        const uint32_t total = activeBook_.ContentCharCount();
        const int percent = total > 0
                                ? static_cast<int>(std::lround(100.0 * global / total))
                                : 0;
        prompt += L"当前阅读位置：第 " + std::to_wstring(chapter + 1) + L" 章《" +
                  ChapterTitleAt(activeBook_, chapter) + L"》，全书偏移 " +
                  std::to_wstring(global) + L"，进度 " + std::to_wstring(percent) + L"%。\n";
        const std::wstring& content = activeBook_.content;
        const uint32_t begin = global > 400 ? global - 400 : 0;
        const uint32_t end =
            std::min<uint32_t>(begin + 800, static_cast<uint32_t>(content.size()));
        prompt += L"当前位置附近文本：\n" + content.substr(begin, end - begin) + L"\n";
    }
    prompt +=
        L"偏移量为全文中的字符下标（从 0 开始）。\n"
        L"可用命令（写在回复中即可，程序会自动执行并从显示内容中移除；一条回复可包含多条命令）：\n"
        L"[[goto:全书字符偏移]] 跳转到指定位置\n"
        L"[[highlight:起始偏移:长度:#RRGGBB]] 添加高亮\n"
        L"[[note:起始偏移:长度:标注文字]] 添加标注\n"
        L"[[read:章节序号]] 读取该章全文（序号从 0 开始）\n"
        L"[[range:起始偏移:字符数]] 读取全文指定范围（单次最多 24000 字符，正文会自动追加到对话中）\n"
        L"[[search:关键词]] 在全书检索关键词并读取其附近正文\n"
        L"读取正文时必须输出上述读取命令，程序会自动追加正文并继续请求；"
        L"不要只写「【正文范围 A - B】」这类说明，也不要要求用户输入「继续」。"
        L"回答较长时请连续输出直到结束，不要分轮等待用户确认。\n";
    if (chapterCount > 0) {
        prompt += L"章节大纲：\n";
        const size_t limit = std::min<size_t>(chapterCount, 200);
        for (size_t i = 0; i < limit; ++i) {
            const umd::Chapter& item = activeBook_.chapters[i];
            prompt += std::to_wstring(i) + L". 《" +
                      ChapterTitleAt(activeBook_, static_cast<int>(i)) + L"》 偏移 " +
                      std::to_wstring(item.charOffset) + L"，长度 " +
                      std::to_wstring(item.charLength) + L"\n";
        }
        if (chapterCount > limit) {
            prompt += L"……共 " + std::to_wstring(chapterCount) + L" 章\n";
        }
    }
    return prompt;
}

void Application::ExecuteAiCommand(const std::wstring& body, AiReadRange& readRange) {
    // 归一化全角字符后解析；归一化保持长度不变，标注文字的位置可直接映射回原文。
    const std::wstring normalized = NormalizeCommandText(body);
    const size_t colon = normalized.find(L':');
    if (colon == std::wstring::npos) {
        return;
    }
    const std::wstring name = TrimNoteText(normalized.substr(0, colon));
    const std::wstring args = normalized.substr(colon + 1);
    if (name == L"goto" || name == L"跳转") {
        uint32_t offset = 0;
        if (ExtractFirstNumber(args, offset)) {
            GoToGlobalOffset(offset);
        }
        return;
    }
    if (name == L"read" || name == L"读取") {
        // 支持章节序号与章节标题两种写法。
        uint32_t index = 0;
        if (!readRange.valid) {
            bool matched = false;
            if (ExtractFirstNumber(args, index) && index < activeBook_.chapters.size()) {
                matched = true;
            } else {
                const std::wstring keyword = TrimNoteText(args);
                if (!keyword.empty()) {
                    for (size_t i = 0; i < activeBook_.chapters.size(); ++i) {
                        if (ChapterTitleAt(activeBook_, static_cast<int>(i)).find(keyword) !=
                            std::wstring::npos) {
                            index = static_cast<uint32_t>(i);
                            matched = true;
                            break;
                        }
                    }
                }
            }
            if (matched) {
                const umd::Chapter& chapter = activeBook_.chapters[index];
                readRange.valid = true;
                readRange.start = chapter.charOffset;
                readRange.length = chapter.charLength;
            }
        }
        return;
    }
    if (name == L"range" || name == L"范围") {
        // 支持「起始偏移:字符数」、「起始偏移-结束偏移」、「起始偏移 结束偏移」等写法。
        uint32_t start = 0;
        uint32_t second = 0;
        if (!ExtractTwoNumbers(args, start, second)) {
            return;
        }
        uint32_t length = 0;
        if (args.find(L':') != std::wstring::npos) {
            length = second;
        } else if (second > start) {
            length = second - start;
        }
        if (length > 0 && !readRange.valid) {
            readRange.valid = true;
            readRange.start = start;
            readRange.length = length;
        }
        return;
    }
    if (name == L"search" || name == L"find" || name == L"搜索" || name == L"查找") {
        // 关键词检索：先找当前阅读位置之后的首次出现，找不到再从全文开头查找。
        const std::wstring keyword = TrimNoteText(args);
        const uint32_t total = activeBook_.ContentCharCount();
        if (keyword.empty() || readRange.valid || total == 0) {
            return;
        }
        const std::wstring loweredContent = LowerCase(activeBook_.content);
        const std::wstring loweredKeyword = LowerCase(keyword);
        const uint32_t current = std::min(CurrentGlobalOffset(), total);
        size_t position = loweredContent.find(loweredKeyword, current);
        if (position == std::wstring::npos) {
            position = loweredContent.find(loweredKeyword);
        }
        if (position != std::wstring::npos) {
            const uint32_t match = static_cast<uint32_t>(position);
            const uint32_t start = match > 600 ? match - 600 : 0;
            const uint32_t length =
                std::min<uint32_t>(static_cast<uint32_t>(keyword.size()) + 1800, total - start);
            readRange.valid = true;
            readRange.start = start;
            readRange.length = length;
        }
        return;
    }
    if (name != L"highlight" && name != L"note" && name != L"高亮" && name != L"标注") {
        return;
    }
    const size_t first = args.find(L':');
    if (first == std::wstring::npos) {
        return;
    }
    uint32_t start = 0;
    if (!ExtractFirstNumber(args.substr(0, first), start)) {
        return;
    }
    const size_t second = args.find(L':', first + 1);
    const std::wstring lengthText =
        second == std::wstring::npos ? args.substr(first + 1)
                                     : args.substr(first + 1, second - first - 1);
    uint32_t length = 0;
    if (!ExtractFirstNumber(lengthText, length)) {
        return;
    }
    // 标注文字取原文字符串，保留模型输出的全角标点。
    const std::wstring extra =
        second == std::wstring::npos ? std::wstring()
                                     : body.substr(colon + 1 + second + 1);
    if (name == L"highlight" || name == L"高亮") {
        AddAiHighlight(start, length, extra);
    } else {
        AddAiNote(start, length, extra);
    }
}

void Application::ProcessAiCommands(const std::wstring& reply, std::wstring& cleaned,
                                    AiReadRange& readRange) {
    cleaned.clear();
    readRange = AiReadRange{};
    // 模型常模仿正文注入消息，直接写出「【正文范围 A - B】」而不输出标准命令；
    // 这里把这类范围说明也识别为读取请求，并从显示内容中移除。
    struct RangeLabel {
        const wchar_t* prefix;
        size_t prefixLength;
        wchar_t closing;
    };
    const RangeLabel labels[] = {
        {L"【正文范围", 5, L'】'},
        {L"【读取范围", 5, L'】'},
        {L"[正文范围", 5, L']'},
        {L"[读取范围", 5, L']'},
    };
    size_t position = 0;
    while (position < reply.size()) {
        const size_t commandOpen = reply.find(L"[[", position);
        size_t labelOpen = std::wstring::npos;
        size_t labelIndex = 0;
        for (size_t i = 0; i < sizeof(labels) / sizeof(labels[0]); ++i) {
            const size_t found = reply.find(labels[i].prefix, position);
            if (found != std::wstring::npos &&
                (labelOpen == std::wstring::npos || found < labelOpen)) {
                labelOpen = found;
                labelIndex = i;
            }
        }
        size_t tokenOpen = commandOpen;
        if (labelOpen != std::wstring::npos &&
            (commandOpen == std::wstring::npos || labelOpen < commandOpen)) {
            tokenOpen = labelOpen;
        }
        if (tokenOpen == std::wstring::npos) {
            cleaned.append(reply.substr(position));
            break;
        }
        cleaned.append(reply.substr(position, tokenOpen - position));
        if (tokenOpen == commandOpen) {
            const size_t close = reply.find(L"]]", commandOpen + 2);
            if (close == std::wstring::npos) {
                cleaned.append(reply.substr(commandOpen));
                break;
            }
            ExecuteAiCommand(reply.substr(commandOpen + 2, close - commandOpen - 2),
                             readRange);
            position = close + 2;
            continue;
        }
        const RangeLabel& label = labels[labelIndex];
        const size_t close = reply.find(label.closing, labelOpen + label.prefixLength);
        if (close == std::wstring::npos) {
            cleaned.append(reply.substr(labelOpen));
            break;
        }
        const std::wstring inner = NormalizeCommandText(
            reply.substr(labelOpen + label.prefixLength,
                         close - labelOpen - label.prefixLength));
        uint32_t begin = 0;
        uint32_t end = 0;
        if (!readRange.valid && ExtractTwoNumbers(inner, begin, end) && end > begin) {
            readRange.valid = true;
            readRange.start = begin;
            readRange.length = end - begin;
        }
        position = close + 1;
    }
}

void Application::AddAiHighlight(uint32_t globalStart, uint32_t length,
                                 const std::wstring& color) {
    data::BookRecord* record = library_.Find(activeBookId_);
    if (record == nullptr || activeBook_.chapters.empty() || length == 0) {
        return;
    }
    int chapterIndex = -1;
    uint32_t chapterOffset = 0;
    for (size_t i = 0; i < activeBook_.chapters.size(); ++i) {
        const umd::Chapter& chapter = activeBook_.chapters[i];
        if (globalStart >= chapter.charOffset &&
            globalStart < chapter.charOffset + chapter.charLength) {
            chapterIndex = static_cast<int>(i);
            chapterOffset = chapter.charOffset;
            break;
        }
    }
    if (chapterIndex < 0) {
        return;
    }
    data::Highlight highlight;
    highlight.chapterIndex = chapterIndex;
    highlight.start = globalStart - chapterOffset;
    highlight.length = length;
    highlight.color = color.empty() ? std::wstring(L"#FFE082") : color;
    highlight.createdAt = util::CurrentUnixSeconds();
    record->highlights.push_back(highlight);
    std::wstring error;
    library_.SaveBook(activeBookId_, error);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Application::AddAiNote(uint32_t globalStart, uint32_t length, const std::wstring& text) {
    data::BookRecord* record = library_.Find(activeBookId_);
    if (record == nullptr || activeBook_.chapters.empty() || length == 0 || text.empty()) {
        return;
    }
    int chapterIndex = -1;
    uint32_t chapterOffset = 0;
    for (size_t i = 0; i < activeBook_.chapters.size(); ++i) {
        const umd::Chapter& chapter = activeBook_.chapters[i];
        if (globalStart >= chapter.charOffset &&
            globalStart < chapter.charOffset + chapter.charLength) {
            chapterIndex = static_cast<int>(i);
            chapterOffset = chapter.charOffset;
            break;
        }
    }
    if (chapterIndex < 0) {
        return;
    }
    data::Note note;
    note.chapterIndex = chapterIndex;
    note.start = globalStart - chapterOffset;
    note.length = length;
    note.text = text;
    note.textHash = util::Sha256Hex(util::ToUtf8(text)).substr(0, 32);
    note.createdAt = util::CurrentUnixSeconds();
    record->notes.push_back(note);
    std::wstring error;
    library_.SaveBook(activeBookId_, error);
    // 标注会插入到排版文本中，需要清空排版缓存后按当前位置重建。
    ClearLayoutCache();
    RebuildPagination(CurrentGlobalOffset());
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Application::StartAiFollowUp(uint32_t start, uint32_t length) {
    data::ChatSession* session = EnsureChatSession();
    if (session == nullptr || activeBook_.content.empty()) {
        return;
    }
    const uint32_t total = static_cast<uint32_t>(activeBook_.content.size());
    if (start >= total) {
        return;
    }
    constexpr uint32_t kMaxReadChars = 24000;
    const uint32_t limited = std::min(length, kMaxReadChars);
    const uint32_t available = std::min(limited, total - start);
    aiLastReadEnd_ = start + available;
    std::wstring text = activeBook_.content.substr(start, available);
    if (available < length) {
        text += L"\n（超出可读取范围，已截取到 " + std::to_wstring(available) + L" 个字符）";
    }
    const int64_t now = util::CurrentUnixSeconds();
    data::ChatMessage message;
    message.role = L"user";
    message.text = L"【正文范围 " + std::to_wstring(start) + L" - " +
                   std::to_wstring(start + available) + L"】\n" + text;
    message.createdAt = now;
    session->messages.push_back(std::move(message));
    session->updatedAt = now;
    SaveChatState();
    std::vector<std::pair<std::wstring, std::wstring>> history;
    history.reserve(session->messages.size());
    for (const data::ChatMessage& item : session->messages) {
        history.emplace_back(item.role, item.text);
    }
    LaunchAiRequest(std::move(history));
}

void Application::RunAiRequest(std::wstring url, std::wstring apiKey, std::wstring model,
                               std::wstring protocol, int thinkingLevel,
                               std::wstring systemPrompt,
                               std::vector<std::pair<std::wstring, std::wstring>> history,
                               HWND target) {
    const bool anthropic = protocol == L"anthropic";
    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &components)) {
        PushAiDone(L"接口地址格式无效", target);
        return;
    }
    const std::wstring host(components.lpszHostName, components.dwHostNameLength);
    std::wstring path(components.lpszUrlPath, components.dwUrlPathLength);
    if (components.dwExtraInfoLength > 0) {
        path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    }
    if (path.empty()) {
        path = L"/";
    }

    HINTERNET session = WinHttpOpen(L"UmdReader/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        PushAiDone(L"无法初始化网络会话", target);
        return;
    }
    WinHttpSetTimeouts(session, 5000, 5000, 15000, 30000);

    HINTERNET connection = WinHttpConnect(session, host.c_str(), components.nPort, 0);
    if (!connection) {
        WinHttpCloseHandle(session);
        PushAiDone(L"无法连接接口主机", target);
        return;
    }
    const DWORD requestFlags =
        components.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = WinHttpOpenRequest(connection, L"POST", path.c_str(), nullptr,
                                           WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           requestFlags);
    if (!request) {
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        PushAiDone(L"无法创建请求", target);
        return;
    }

    std::wstring headers = L"Content-Type: application/json\r\nAccept: text/event-stream\r\n";
    if (anthropic) {
        headers += L"anthropic-version: 2023-06-01\r\n";
        if (!apiKey.empty()) {
            headers += L"x-api-key: " + apiKey + L"\r\n";
        }
    } else if (!apiKey.empty()) {
        headers += L"Authorization: Bearer " + apiKey + L"\r\n";
    }
    WinHttpAddRequestHeaders(request, headers.c_str(), static_cast<DWORD>(headers.size()),
                             WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);

    nlohmann::json body;
    body["model"] = util::ToUtf8(model);
    body["stream"] = true;
    const int thinkingBudget =
        thinkingLevel <= 0 ? 0 : (thinkingLevel == 1 ? 1024 : (thinkingLevel == 2 ? 2048 : 4096));
    if (anthropic) {
        // Anthropic Messages 接口必须指定 max_tokens，系统提示使用顶层 system 字段。
        // 开启思考时 max_tokens 需大于思考预算。
        body["max_tokens"] = thinkingBudget + 1024;
        if (thinkingBudget > 0) {
            nlohmann::json thinking;
            thinking["type"] = "enabled";
            thinking["budget_tokens"] = thinkingBudget;
            body["thinking"] = std::move(thinking);
        }
        if (!systemPrompt.empty()) {
            body["system"] = util::ToUtf8(systemPrompt);
        }
    } else if (thinkingBudget > 0) {
        // OpenAI 兼容协议使用 reasoning_effort 表达思考等级。
        body["reasoning_effort"] =
            thinkingLevel == 1 ? "low" : (thinkingLevel == 2 ? "medium" : "high");
    }
    nlohmann::json messages = nlohmann::json::array();
    if (!anthropic && !systemPrompt.empty()) {
        // OpenAI 兼容协议把阅读助手身份与当前环境放在第一条 system 消息中。
        nlohmann::json systemMessage;
        systemMessage["role"] = "system";
        systemMessage["content"] = util::ToUtf8(systemPrompt);
        messages.push_back(std::move(systemMessage));
    }
    for (const auto& item : history) {
        nlohmann::json message;
        message["role"] = util::ToUtf8(item.first);
        message["content"] = util::ToUtf8(item.second);
        messages.push_back(std::move(message));
    }
    body["messages"] = std::move(messages);
    const std::string payload = body.dump();

    const BOOL sent = WinHttpSendRequest(
        request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, const_cast<char*>(payload.data()),
        static_cast<DWORD>(payload.size()), static_cast<DWORD>(payload.size()), 0);
    BOOL received = FALSE;
    if (sent) {
        received = WinHttpReceiveResponse(request, nullptr);
    }
    if (!sent || !received) {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        PushAiDone(L"请求失败，请检查网络、地址与密钥", target);
        return;
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize,
                        WINHTTP_NO_HEADER_INDEX);
    if (statusCode != 200) {
        std::string errorBody;
        char errorBuffer[1024];
        DWORD read = 0;
        while (errorBody.size() < 600 &&
               WinHttpReadData(request, errorBuffer, sizeof(errorBuffer), &read) && read > 0) {
            errorBody.append(errorBuffer, read);
        }
        if (errorBody.size() > 200) {
            errorBody.resize(200);
        }
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        PushAiDone(L"接口返回错误（HTTP " + std::to_wstring(statusCode) + L"）：" +
                       util::FromUtf8(errorBody),
                   target);
        return;
    }

    // 逐块读取 SSE 流，每行形如 "data: {json}"，结束标记为 "data: [DONE]"。
    std::string lineBuffer;
    std::vector<char> readBuffer(8192);
    bool done = false;
    while (!aiCancel_) {
        DWORD read = 0;
        if (!WinHttpReadData(request, readBuffer.data(),
                             static_cast<DWORD>(readBuffer.size()), &read) ||
            read == 0) {
            break;
        }
        lineBuffer.append(readBuffer.data(), read);
        size_t newline = 0;
        while ((newline = lineBuffer.find('\n')) != std::string::npos) {
            std::string line = lineBuffer.substr(0, newline);
            lineBuffer.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (line.rfind("data:", 0) != 0) {
                continue;
            }
            std::string data = line.substr(5);
            while (!data.empty() && data.front() == ' ') {
                data.erase(0, 1);
            }
            if (data == "[DONE]") {
                done = true;
                break;
            }
            try {
                const nlohmann::json chunk = nlohmann::json::parse(data);
                if (anthropic) {
                    // Anthropic 的增量事件为 content_block_delta，结束事件为 message_stop。
                    const std::string type = chunk.value("type", std::string());
                    if (type == "message_stop") {
                        done = true;
                        break;
                    }
                    if (type == "content_block_delta" && chunk.contains("delta") &&
                        chunk["delta"].is_object()) {
                        const nlohmann::json& delta = chunk["delta"];
                        if (delta.value("type", std::string()) == "text_delta") {
                            const std::string text = delta.value("text", std::string());
                            if (!text.empty()) {
                                PushAiChunk(util::FromUtf8(text), target);
                            }
                        } else if (delta.value("type", std::string()) == "thinking_delta") {
                            // 思考内容单独流式输出，供界面折叠展示。
                            const std::string thinking = delta.value("thinking", std::string());
                            if (!thinking.empty()) {
                                PushAiThinking(util::FromUtf8(thinking), target);
                            }
                        }
                    }
                } else {
                    if (!chunk.contains("choices") || !chunk["choices"].is_array() ||
                        chunk["choices"].empty()) {
                        continue;
                    }
                    const nlohmann::json& delta = chunk["choices"][0]["delta"];
                    if (delta.contains("reasoning_content") &&
                        delta["reasoning_content"].is_string()) {
                        // 常见兼容实现使用 reasoning_content 输出思考内容。
                        const std::string reasoning =
                            delta["reasoning_content"].get<std::string>();
                        if (!reasoning.empty()) {
                            PushAiThinking(util::FromUtf8(reasoning), target);
                        }
                    } else if (delta.contains("reasoning") && delta["reasoning"].is_string()) {
                        const std::string reasoning = delta["reasoning"].get<std::string>();
                        if (!reasoning.empty()) {
                            PushAiThinking(util::FromUtf8(reasoning), target);
                        }
                    }
                    if (delta.contains("content") && delta["content"].is_string()) {
                        const std::string content = delta["content"].get<std::string>();
                        if (!content.empty()) {
                            PushAiChunk(util::FromUtf8(content), target);
                        }
                    }
                }
            } catch (const std::exception&) {
                // 忽略无法解析的行，继续读取后续内容。
            }
        }
        if (done) {
            break;
        }
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    PushAiDone(aiCancel_ ? std::wstring() : std::wstring(L"生成完成"), target);
}

void Application::OpenBook(const std::wstring& id) {
    data::BookRecord* record = library_.Find(id);
    if (!record) {
        ShowStatus(L"书籍记录不存在");
        return;
    }
    if (!record->FileExists()) {
        ShowStatus(L"原文件已被移动或删除：" + record->path);
        return;
    }

    umd::Book parsed;
    std::wstring error;
    SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    const bool succeed = umd::ParseBookFile(std::filesystem::path(record->path), parsed, error);
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    if (!succeed) {
        ShowStatus(L"打开失败：" + error);
        return;
    }

    const uint32_t resumeOffset = record->progressCharOffset;
    activeBookId_ = id;
    activeBook_ = std::move(parsed);
    // 纯文本按已保存的提取方式重建章节，保证章节偏移与书签、高亮、标注中记录的章节下标一致。
    if (activeBook_.formatType == 0 && record->chapterMode != L"auto") {
        std::vector<umd::Chapter> chapters =
            umd::DetectChaptersByUnits(activeBook_.content,
                                       umd::ChapterUnitsForMode(record->chapterMode));
        if (!chapters.empty()) {
            activeBook_.chapters = std::move(chapters);
        } else {
            record->chapterMode = L"auto";
        }
    }
    activeChapter_ = 0;
    activePage_ = 0;
    lineOffset_ = 0;
    activeChapterRaw_.clear();
    activeChapterRawChapter_ = -1;
    chapterDisplay_.Clear();
    pager_.Clear();
    ClearLayoutCache();
    InvalidatePageCache();
    ClearSelection();
    gesture_ = Gesture::Default;
    gestureBarVisible_ = false;
    colorPickerVisible_ = false;
    noteFocused_ = false;
    noteDraft_.clear();
    StopCaretBlink();
    shelfSettingsVisible_ = false;
    categoryManagerVisible_ = false;
    aiSettingsVisible_ = false;
    chatFocus_ = ChatFocus::None;
    panel_ = PanelMode::None;
    searchFocused_ = false;
    searchQuery_.clear();
    searchStatus_.clear();
    searchResults_.clear();
    searchHighlight_.clear();
    panelScroll_ = 0.0f;
    statusText_.clear();
    // 对话状态归位：默认打开最近更新的会话，其余会话保留在对话记录中。
    aiDraft_.clear();
    aiAnswer_.clear();
    aiStatus_.clear();
    chatScroll_ = 0.0f;
    chatScrollMax_ = 0.0f;
    chatSessionPickerVisible_ = false;
    fontPickerVisible_ = false;
    activeChatSessionId_.clear();
    int64_t latestChatTime = -1;
    for (const data::ChatSession& session : record->chatSessions) {
        if (session.updatedAt >= latestChatTime) {
            latestChatTime = session.updatedAt;
            activeChatSessionId_ = session.id;
        }
    }

    library_.TouchBook(id, resumeOffset);
    SetWindowTextW(hwnd_, (activeBook_.title + L" - " + kWindowTitle).c_str());
    GoToGlobalOffset(resumeOffset);

    // 分区头校验：内容变化时提示高亮与标注可能失效；旧记录则补记编码并重建分区头。
    // 同时记录正文格式：UMD 的高亮与标注按章节内偏移保存，纯文本才使用分区偏移。
    record->formatType = activeBook_.formatType;
    if (record->partitionHeaders.empty() || record->textEncoding == 0) {
        record->textEncoding = activeBook_.sourceCodePage;
        record->partitionHeaders =
            data::BuildPartitionHeaders(activeBook_.content, record->textEncoding);
    } else if (data::CountChangedPartitions(activeBook_.content, record->partitionHeaders,
                                            record->textEncoding) > 0) {
        ShowStatus(L"文件内容已更改，高亮与标注可能无法对应原文");
    }

    std::wstring saveError;
    library_.SaveBook(activeBookId_, saveError);
}

void Application::CloseBook() {
    // 结束未完成的对话请求，避免把回复写入已关闭的书籍。
    if (aiStreaming_) {
        StopAiRequest();
    }
    if (!activeBookId_.empty()) {
        library_.TouchBook(activeBookId_, CurrentGlobalOffset());
        std::wstring error;
        library_.SaveBook(activeBookId_, error);
    }
    activeBook_ = umd::Book{};
    activeChapterRaw_.clear();
    activeChapterRawChapter_ = -1;
    chapterDisplay_.Clear();
    pager_.Clear();
    ClearLayoutCache();
    InvalidatePageCache();
    ClearSelection();
    gesture_ = Gesture::Default;
    gestureBarVisible_ = false;
    colorPickerVisible_ = false;
    noteFocused_ = false;
    noteDraft_.clear();
    StopCaretBlink();
    StopSpeech();
    activeBookId_.clear();
    activeChapter_ = 0;
    activePage_ = 0;
    lineOffset_ = 0;
    view_ = ViewMode::Shelf;
    panel_ = PanelMode::None;
    aiSettingsVisible_ = false;
    chatFocus_ = ChatFocus::None;
    searchFocused_ = false;
    searchQuery_.clear();
    searchStatus_.clear();
    searchResults_.clear();
    searchHighlight_.clear();
    panelScroll_ = 0.0f;
    aiAnswer_.clear();
    aiStatus_.clear();
    activeChatSessionId_.clear();
    chatScroll_ = 0.0f;
    chatScrollMax_ = 0.0f;
    chatSessionPickerVisible_ = false;
    fontPickerVisible_ = false;
    SetWindowTextW(hwnd_, kWindowTitle);
    RefreshShelfOrder();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Application::LoadChapter(int chapterIndex, uint32_t localOffset) {
    if (speaking_) {
        StopSpeech();
    }
    if (activeBook_.chapters.empty()) {
        return;
    }
    const int clamped =
        std::clamp(chapterIndex, 0, static_cast<int>(activeBook_.chapters.size()) - 1);
    activeChapter_ = clamped;
    const umd::Chapter& chapter = activeBook_.chapters[static_cast<size_t>(clamped)];
    if (activeChapterRawChapter_ != clamped || activeChapterRaw_.empty()) {
        activeChapterRaw_ = activeBook_.content.substr(chapter.charOffset, chapter.charLength);
        activeChapterRawChapter_ = clamped;
    }

    float textWidth = 0.0f;
    float textHeight = 0.0f;
    D2D1_RECT_F textRect{};
    ComputeTextMetrics(textWidth, textHeight, textRect);

    const bool layoutMatched =
        layoutChapter_ == clamped && std::abs(layoutWidth_ - textWidth) < 0.01f &&
        std::abs(layoutHeight_ - textHeight) < 0.01f && !chapterDisplay_.Text().empty();
    if (!layoutMatched) {
        ChapterLayout* cached = FindCachedLayout(clamped, textWidth, textHeight);
        if (cached) {
            chapterDisplay_ = cached->display;
            pager_ = cached->pager;
        } else {
            static const std::vector<data::Note> kEmptyNotes;
            const data::BookRecord* record = library_.Find(activeBookId_);
            const std::vector<data::Note>& notes = record ? record->notes : kEmptyNotes;
            const std::wstring indent(4, L'\u3000');
            const float indentWidth =
                renderer_.MeasureTextWidth(indent, renderer_.Formats().body.Get());
            chapterDisplay_.Build(activeChapterRaw_, notes, activeChapter_,
                                  renderer_.Formats().body.Get(), renderer_.Formats().note.Get(),
                                  renderer_.WriteFactory(), textWidth, indentWidth);
            pager_.Build(chapterDisplay_.Text(), renderer_.Formats().body.Get(),
                         renderer_.WriteFactory(), textWidth, textHeight);
            StoreCachedLayout(clamped, textWidth, textHeight);
        }
        layoutChapter_ = clamped;
        layoutWidth_ = textWidth;
        layoutHeight_ = textHeight;
    }

    const uint32_t displayLength = static_cast<uint32_t>(chapterDisplay_.Text().size());
    const uint32_t displayOffset =
        std::min(chapterDisplay_.ToDisplay(localOffset), displayLength);
    activePage_ = pager_.PageIndexForChar(displayOffset);
    if (pager_.PageCount() == 0) {
        activePage_ = 0;
    } else if (activePage_ >= pager_.PageCount()) {
        activePage_ = pager_.PageCount() - 1;
    }
    // 按行滚动时以实际位置作为视口起点；按页翻页时对齐到页首。
    if (LineScrollMode() && displayOffset < displayLength) {
        lineOffset_ = displayOffset;
    } else {
        lineOffset_ = pager_.PageCount() > 0 ? pager_.Page(activePage_).startChar : 0;
    }
    InvalidatePageCache();
    ClearSelection();
    colorPickerVisible_ = false;
}

Application::ChapterLayout* Application::FindCachedLayout(int chapterIndex, float width,
                                                          float height) {
    for (ChapterLayoutCacheEntry& entry : layoutCache_) {
        if (entry.chapterIndex == chapterIndex && std::abs(entry.width - width) < 0.01f &&
            std::abs(entry.height - height) < 0.01f) {
            return &entry.layout;
        }
    }
    return nullptr;
}

void Application::StoreCachedLayout(int chapterIndex, float width, float height) {
    // 只保留两项：无侧栏与有侧栏两种排版宽度，内存占用可控。
    if (layoutCache_.size() >= 2) {
        layoutCache_.erase(layoutCache_.begin());
    }
    ChapterLayoutCacheEntry entry;
    entry.chapterIndex = chapterIndex;
    entry.width = width;
    entry.height = height;
    entry.layout.display = chapterDisplay_;
    entry.layout.pager = pager_;
    layoutCache_.push_back(std::move(entry));
}

void Application::ClearLayoutCache() {
    layoutCache_.clear();
    layoutChapter_ = -1;
    layoutWidth_ = 0.0f;
    layoutHeight_ = 0.0f;
}

void Application::RebuildPagination(uint32_t preserveGlobalOffset) {
    if (activeBook_.chapters.empty()) {
        return;
    }
    const uint32_t total = activeBook_.ContentCharCount();
    const uint32_t offset = std::min(preserveGlobalOffset, total);
    int chapterIndex = 0;
    for (size_t i = 0; i < activeBook_.chapters.size(); ++i) {
        if (activeBook_.chapters[i].charOffset <= offset) {
            chapterIndex = static_cast<int>(i);
        } else {
            break;
        }
    }
    const uint32_t localOffset =
        offset - activeBook_.chapters[static_cast<size_t>(chapterIndex)].charOffset;
    LoadChapter(chapterIndex, localOffset);
    if (view_ == ViewMode::Reader) {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

uint32_t Application::CurrentGlobalOffset() const {
    if (activeBook_.chapters.empty()) {
        return 0;
    }
    if (activeChapter_ < 0 ||
        activeChapter_ >= static_cast<int>(activeBook_.chapters.size())) {
        return 0;
    }
    const uint32_t chapterOffset =
        activeBook_.chapters[static_cast<size_t>(activeChapter_)].charOffset;
    if (pager_.PageCount() == 0) {
        return chapterOffset;
    }
    const uint32_t displayOffset = ViewportStartOffset();
    return chapterOffset + chapterDisplay_.ToOriginal(displayOffset);
}

void Application::GoToGlobalOffset(uint32_t globalOffset, bool closePanel) {
    if (activeBook_.chapters.empty()) {
        return;
    }
    const uint32_t total = activeBook_.ContentCharCount();
    const uint32_t offset = std::min(globalOffset, total);
    int chapterIndex = 0;
    for (size_t i = 0; i < activeBook_.chapters.size(); ++i) {
        if (activeBook_.chapters[i].charOffset <= offset) {
            chapterIndex = static_cast<int>(i);
        } else {
            break;
        }
    }
    const uint32_t localOffset =
        offset - activeBook_.chapters[static_cast<size_t>(chapterIndex)].charOffset;
    view_ = ViewMode::Reader;
    // 目录、书签、检索跳转需要收起侧栏；拖动滑块定位时保持侧栏打开。
    if (closePanel) {
        panel_ = PanelMode::None;
        searchFocused_ = false;
    }
    if (chapterIndex == activeChapter_ && !chapterDisplay_.Text().empty() &&
        pager_.PageCount() > 0) {
        // 同一章节内跳转只更新视口位置，避免重复构建整章显示文本与分页。
        if (speaking_) {
            StopSpeech();
        }
        const uint32_t displayLength = static_cast<uint32_t>(chapterDisplay_.Text().size());
        const uint32_t displayOffset =
            std::min(chapterDisplay_.ToDisplay(localOffset), displayLength);
        activePage_ = pager_.PageIndexForChar(displayOffset);
        if (activePage_ >= pager_.PageCount()) {
            activePage_ = pager_.PageCount() - 1;
        }
        if (LineScrollMode() && displayOffset < displayLength) {
            lineOffset_ = displayOffset;
        } else {
            lineOffset_ = pager_.Page(activePage_).startChar;
        }
        InvalidatePageCache();
        ClearSelection();
        colorPickerVisible_ = false;
    } else {
        LoadChapter(chapterIndex, localOffset);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Application::NextPage() {
    if (speaking_) {
        StopSpeech();
    }
    if (pager_.PageCount() == 0) {
        return;
    }
    if (activePage_ + 1 < pager_.PageCount()) {
        ++activePage_;
        if (LineScrollMode()) {
            // 行滚动模式下按页翻页时同步移动视口起点，否则画面不会变化。
            lineOffset_ = pager_.Page(activePage_).startChar;
        }
        InvalidatePageCache();
        ClearSelection();
        colorPickerVisible_ = false;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (activeChapter_ + 1 < static_cast<int>(activeBook_.chapters.size())) {
        LoadChapter(activeChapter_ + 1, 0);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    ShowStatus(L"已经到达全书最后一页");
}

void Application::PreviousPage() {
    if (speaking_) {
        StopSpeech();
    }
    if (pager_.PageCount() == 0) {
        return;
    }
    if (activePage_ > 0) {
        --activePage_;
        if (LineScrollMode()) {
            // 行滚动模式下按页翻页时同步移动视口起点，否则画面不会变化。
            lineOffset_ = pager_.Page(activePage_).startChar;
        }
        InvalidatePageCache();
        ClearSelection();
        colorPickerVisible_ = false;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (activeChapter_ > 0) {
        const int previous = activeChapter_ - 1;
        const uint32_t chapterLength =
            activeBook_.chapters[static_cast<size_t>(previous)].charLength;
        LoadChapter(previous, chapterLength);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    ShowStatus(L"已经到达全书第一页");
}

bool Application::LineScrollMode() const {
    return library_.Settings().readerScrollMode != L"page";
}

uint32_t Application::ViewportStartOffset() const {
    const std::wstring& displayText = chapterDisplay_.Text();
    const uint32_t length = static_cast<uint32_t>(displayText.size());
    if (length == 0) {
        return 0;
    }
    if (!LineScrollMode()) {
        if (pager_.PageCount() == 0) {
            return 0;
        }
        const size_t pageIndex = std::min(activePage_, pager_.PageCount() - 1);
        return std::min(pager_.Page(pageIndex).startChar, length);
    }
    return std::min(lineOffset_, length);
}

uint32_t Application::NextLineStart(uint32_t from) {
    const std::wstring& displayText = chapterDisplay_.Text();
    const uint32_t length = static_cast<uint32_t>(displayText.size());
    if (from >= length) {
        return length;
    }
    float textWidth = 0.0f;
    float textHeight = 0.0f;
    D2D1_RECT_F textRect{};
    ComputeTextMetrics(textWidth, textHeight, textRect);
    const uint32_t span = std::min<uint32_t>(length - from, 4096);
    ComPtr<IDWriteTextLayout> layout =
        renderer_.CreateBodyLayout(displayText.substr(from, span), textWidth, 100000.0f);
    if (!layout) {
        return length;
    }
    UINT32 lineCount = 0;
    layout->GetLineMetrics(nullptr, 0, &lineCount);
    if (lineCount == 0) {
        return length;
    }
    std::vector<DWRITE_LINE_METRICS> lines(lineCount);
    layout->GetLineMetrics(lines.data(), lineCount, &lineCount);
    const uint32_t step = lines[0].length;
    if (step == 0) {
        return length;
    }
    return from + step;
}

uint32_t Application::PreviousLineStart(uint32_t from) {
    if (from == 0) {
        return 0;
    }
    const std::wstring& displayText = chapterDisplay_.Text();
    const uint32_t length = static_cast<uint32_t>(displayText.size());
    float textWidth = 0.0f;
    float textHeight = 0.0f;
    D2D1_RECT_F textRect{};
    ComputeTextMetrics(textWidth, textHeight, textRect);
    // 回看一段文本，取最后一个早于当前偏移的行首，保证向上滚动精确到行。
    constexpr uint32_t kLookback = 8000;
    const uint32_t begin = from > kLookback ? from - kLookback : 0;
    const uint32_t span = std::min<uint32_t>(length - begin, from - begin + 1);
    ComPtr<IDWriteTextLayout> layout =
        renderer_.CreateBodyLayout(displayText.substr(begin, span), textWidth, 100000.0f);
    if (!layout) {
        return 0;
    }
    UINT32 lineCount = 0;
    layout->GetLineMetrics(nullptr, 0, &lineCount);
    if (lineCount == 0) {
        return 0;
    }
    std::vector<DWRITE_LINE_METRICS> lines(lineCount);
    layout->GetLineMetrics(lines.data(), lineCount, &lineCount);
    uint32_t position = begin;
    uint32_t result = begin;
    for (UINT32 i = 0; i < lineCount; ++i) {
        if (position >= from) {
            break;
        }
        result = position;
        position += lines[i].length;
    }
    return result;
}

void Application::ScrollReadingLines(int direction, int lines) {
    if (pager_.PageCount() == 0) {
        return;
    }
    const std::wstring& displayText = chapterDisplay_.Text();
    const uint32_t length = static_cast<uint32_t>(displayText.size());
    // 本章最后一页的页首是行滚动的终点；越过它就该进入下一章。
    const uint32_t lastPageStart =
        std::min(pager_.Page(pager_.PageCount() - 1).startChar, length);
    uint32_t offset = ViewportStartOffset();
    bool chapterTurned = false;

    if (direction < 0 && offset == 0) {
        if (activeChapter_ <= 0) {
            return;
        }
        // 本章顶部继续向上滚动：转到上一章末尾。
        const uint32_t previousLength =
            activeBook_.chapters[static_cast<size_t>(activeChapter_ - 1)].charLength;
        LoadChapter(activeChapter_ - 1, previousLength);
        chapterTurned = true;
    } else if (direction > 0 && offset >= lastPageStart) {
        if (activeChapter_ + 1 >= static_cast<int>(activeBook_.chapters.size())) {
            return;
        }
        // 已到达本章末页：继续向下滚动即进入下一章。
        LoadChapter(activeChapter_ + 1, 0);
        chapterTurned = true;
    }

    if (!chapterTurned) {
        for (int i = 0; i < lines; ++i) {
            uint32_t next = offset;
            if (direction > 0) {
                next = NextLineStart(offset);
                if (next > lastPageStart) {
                    next = lastPageStart;
                }
            } else {
                next = PreviousLineStart(offset);
            }
            if (next == offset) {
                break;
            }
            offset = next;
        }
        if (offset == lineOffset_) {
            return;
        }
        lineOffset_ = offset;
        activePage_ = pager_.PageIndexForChar(offset);
        InvalidatePageCache();
    }

    std::wstring error;
    library_.TouchBook(activeBookId_, CurrentGlobalOffset());
    library_.SaveBook(activeBookId_, error);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Application::AlignLineOffsetToCurrentPage() {
    if (pager_.PageCount() == 0) {
        lineOffset_ = 0;
        return;
    }
    const size_t pageIndex = std::min(activePage_, pager_.PageCount() - 1);
    lineOffset_ = pager_.Page(pageIndex).startChar;
    InvalidatePageCache();
}

float Application::ScrollRatioFromY(float y) const {
    float textWidth = 0.0f;
    float textHeight = 0.0f;
    D2D1_RECT_F textRect{};
    ComputeTextMetrics(textWidth, textHeight, textRect);
    constexpr float kThumbHeight = 64.0f;
    const float usable = std::max(1.0f, (textRect.bottom - textRect.top) - kThumbHeight);
    return std::clamp((y - textRect.top - kThumbHeight * 0.5f) / usable, 0.0f, 1.0f);
}

void Application::GoToScrollRatio(float ratio) {
    const uint32_t total = activeBook_.ContentCharCount();
    if (total == 0) {
        return;
    }
    const uint32_t target =
        static_cast<uint32_t>(std::clamp(ratio, 0.0f, 1.0f) * static_cast<float>(total));
    // 直接定位到实际偏移；行滚动模式下不再强制回到页首，保证位置精确。
    // 滑块拖动期间保持阅读设置等侧栏打开，避免面板被收起。
    GoToGlobalOffset(target, false);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Application::DrawReaderScrollbar(const ThemeColors& colors) {
    if (pager_.PageCount() == 0) {
        return;
    }
    float textWidth = 0.0f;
    float textHeight = 0.0f;
    D2D1_RECT_F textRect{};
    ComputeTextMetrics(textWidth, textHeight, textRect);
    const float left =
        panel_ == PanelMode::None ? widthDip_ - 20.0f : widthDip_ - PanelWidth() - 24.0f;
    if (left <= textRect.right) {
        return;
    }
    constexpr float kTrackWidth = 6.0f;
    constexpr float kThumbHeight = 64.0f;
    const D2D1_RECT_F track =
        D2D1::RectF(left, textRect.top, left + kTrackWidth, textRect.bottom);
    renderer_.FillRoundedRect(track, 3.0f, colors.divider);

    const uint32_t total = activeBook_.ContentCharCount();
    const float ratio =
        total > 0 ? std::clamp(static_cast<float>(CurrentGlobalOffset()) /
                                   static_cast<float>(total),
                               0.0f, 1.0f)
                  : 0.0f;
    const float thumbTop =
        textRect.top + (textRect.bottom - textRect.top - kThumbHeight) * ratio;
    const D2D1_RECT_F thumb = D2D1::RectF(left - 3.0f, thumbTop, left + kTrackWidth + 3.0f,
                                          thumbTop + kThumbHeight);
    renderer_.FillRoundedRect(thumb, 6.0f, colors.accent);
    AddHit(D2D1::RectF(left - 8.0f, textRect.top, left + kTrackWidth + 8.0f, textRect.bottom),
           HitKind::ReaderScrollbar);
}

void Application::AddBookmark() {
    data::BookRecord* record = library_.Find(activeBookId_);
    if (!record || activeBook_.chapters.empty() || pager_.PageCount() == 0) {
        return;
    }
    const size_t pageIndex = std::min(activePage_, pager_.PageCount() - 1);
    const PageInfo& page = pager_.Page(pageIndex);
    const uint32_t originalStart = chapterDisplay_.ToOriginal(page.startChar);
    for (const data::Bookmark& existing : record->bookmarks) {
        if (existing.chapterIndex == activeChapter_ && existing.charOffset == originalStart) {
            ShowStatus(L"当前页已经添加过书签");
            return;
        }
    }

    const uint32_t chapterLength = static_cast<uint32_t>(activeChapterRaw_.size());
    const uint32_t start = std::min(originalStart, chapterLength);
    std::wstring snippet =
        start < chapterLength ? activeChapterRaw_.substr(start, std::min<uint32_t>(64, chapterLength - start))
                              : std::wstring();

    data::Bookmark bookmark;
    bookmark.chapterIndex = activeChapter_;
    bookmark.charOffset = originalStart;
    bookmark.title = util::Ellipsize(SanitizeLine(snippet), 24);
    bookmark.createdAt = util::CurrentUnixSeconds();
    record->bookmarks.push_back(std::move(bookmark));

    std::wstring error;
    library_.SaveBook(activeBookId_, error);
    ShowStatus(L"已为当前页添加书签");
}

void Application::RemoveBookmark(int bookmarkIndex) {
    data::BookRecord* record = library_.Find(activeBookId_);
    if (!record || bookmarkIndex < 0 ||
        bookmarkIndex >= static_cast<int>(record->bookmarks.size())) {
        return;
    }
    record->bookmarks.erase(record->bookmarks.begin() + bookmarkIndex);
    std::wstring error;
    library_.SaveBook(activeBookId_, error);
    ShowStatus(L"已删除书签");
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Application::RunSearch() {
    searchResults_.clear();
    searchStatus_.clear();
    panelScroll_ = 0.0f;
    if (searchQuery_.empty()) {
        searchStatus_ = L"请输入检索关键词";
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (activeBook_.content.empty()) {
        searchStatus_ = L"当前书籍没有正文内容";
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    const std::wstring loweredContent = LowerCase(activeBook_.content);
    const std::wstring loweredQuery = LowerCase(searchQuery_);
    if (loweredQuery.empty()) {
        searchStatus_ = L"关键词无效";
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    constexpr size_t kMaxResults = 200;
    int chapterIndex = 0;
    size_t position = 0;
    while (searchResults_.size() < kMaxResults) {
        position = loweredContent.find(loweredQuery, position);
        if (position == std::wstring::npos) {
            break;
        }
        while (chapterIndex + 1 < static_cast<int>(activeBook_.chapters.size()) &&
               position >=
                   activeBook_.chapters[static_cast<size_t>(chapterIndex + 1)].charOffset) {
            ++chapterIndex;
        }
        SearchResult result;
        result.chapterIndex = chapterIndex;
        result.charOffset =
            static_cast<uint32_t>(position -
                                  activeBook_.chapters[static_cast<size_t>(chapterIndex)].charOffset);
        const size_t contextStart = position > 24 ? position - 24 : 0;
        const size_t contextEnd = std::min(position + loweredQuery.size() + 24,
                                           activeBook_.content.size());
        std::wstring context =
            activeBook_.content.substr(contextStart, contextEnd - contextStart);
        context = SanitizeLine(context);
        if (contextStart > 0) {
            context = L"…" + context;
        }
        if (contextEnd < activeBook_.content.size()) {
            context += L"…";
        }
        result.context = util::Ellipsize(context, 50);
        searchResults_.push_back(std::move(result));
        position += loweredQuery.size();
    }

    if (searchResults_.empty()) {
        searchStatus_ = L"未找到匹配内容";
    } else {
        searchStatus_ = L"找到 " + std::to_wstring(searchResults_.size()) + L" 处匹配";
        if (searchResults_.size() == kMaxResults) {
            searchStatus_ += L"（仅显示前 200 处）";
        }
        searchHighlight_ = searchQuery_;
    }
    // 检索高亮变化后需要重建正文布局，清除上一次叠加的高亮范围。
    InvalidatePageCache();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Application::ApplySettings() {
    const data::ReaderSettings& settings = library_.Settings();
    renderer_.CreateTextFormats(settings.fontFamily, settings.fontSize, settings.lineSpacing,
                                settings.noteFontSize);
    if (view_ == ViewMode::Reader && !activeBook_.content.empty()) {
        ClearLayoutCache();
        RebuildPagination(CurrentGlobalOffset());
    }
    std::wstring error;
    library_.Save(error);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Application::ChangePanel(PanelMode next) {
    const bool hasContent = view_ == ViewMode::Reader && !activeBook_.content.empty();
    const uint32_t offset = hasContent ? CurrentGlobalOffset() : 0;
    panel_ = next;
    // 字体与对话记录弹框依附于阅读面板，切换面板时一并关闭。
    fontPickerVisible_ = false;
    chatSessionPickerVisible_ = false;
    CloseMenu();
    CloseShortcutSettings();
    if (next != PanelMode::Chat) {
        // AI 接口设置弹框依附于 AI 对话面板，面板切换时一并关闭。
        aiSettingsVisible_ = false;
    }
    panelScroll_ = 0.0f;
    // 打开阅读设置时同步背景目录，外部增删的图片也能及时反映。
    if (next == PanelMode::Settings) {
        RefreshBackgroundList();
    }
    searchFocused_ = (next == PanelMode::Search);
    if (next != PanelMode::NoteEditor) {
        noteFocused_ = false;
    }
    if (next == PanelMode::Chat) {
        if (chatFocus_ == ChatFocus::None) {
            chatFocus_ = ChatFocus::Message;
        }
    } else {
        chatFocus_ = ChatFocus::None;
    }
    if (next != PanelMode::None) {
        gestureBarVisible_ = false;
        colorPickerVisible_ = false;
    }
    ClearSelection();
    ClearChatSelection();
    if (hasContent) {
        RebuildPagination(offset);
    }
    if (searchFocused_ || noteFocused_ || chatFocus_ != ChatFocus::None) {
        ResetInputCaretToEnd();
        StartCaretBlink();
    } else {
        StopCaretBlink();
    }
    UpdateImePosition();
    InvalidateRect(hwnd_, nullptr, FALSE);
    // 面板切换会改变可见控件与命中区域。此处立即重绘一次，
    // 让 hitRegions_ 与新布局同步，避免紧随其后的点击命中旧面板区域而失效。
    UpdateWindow(hwnd_);
}

ID2D1Bitmap* Application::CoverBitmap(const data::BookRecord& book) {
    if (book.coverFile.empty()) {
        return nullptr;
    }
    const auto found = coverCache_.find(book.id);
    if (found != coverCache_.end()) {
        return found->second.Get();
    }
    ComPtr<ID2D1Bitmap> bitmap = renderer_.LoadBitmap(library_.ResolveDataPath(book.coverFile));
    if (!bitmap) {
        return nullptr;
    }
    ComPtr<ID2D1Bitmap>& slot = coverCache_[book.id];
    slot = std::move(bitmap);
    return slot.Get();
}

ID2D1Bitmap* Application::ImageBitmap(const std::wstring& relativePath) {
    if (relativePath.empty()) {
        return nullptr;
    }
    const auto found = coverCache_.find(relativePath);
    if (found != coverCache_.end()) {
        return found->second.Get();
    }
    ComPtr<ID2D1Bitmap> bitmap = renderer_.LoadBitmap(library_.ResolveDataPath(relativePath));
    if (!bitmap) {
        return nullptr;
    }
    ComPtr<ID2D1Bitmap>& slot = coverCache_[relativePath];
    slot = std::move(bitmap);
    return slot.Get();
}

ID2D1Bitmap* Application::BackgroundBitmap() {
    const std::wstring& name = library_.Settings().backgroundImage;
    if (name.empty()) {
        return nullptr;
    }
    if (backgroundBitmap_ && backgroundBitmapName_ == name) {
        return backgroundBitmap_.Get();
    }
    backgroundBitmapName_ = name;
    backgroundBitmap_ = renderer_.LoadBitmap(library_.BackgroundDirectory() / name);
    return backgroundBitmap_.Get();
}

const Application::BackgroundThumb* Application::BackgroundThumbFor(const std::wstring& fileName) {
    const auto found = backgroundThumbCache_.find(fileName);
    if (found != backgroundThumbCache_.end()) {
        return &found->second;
    }
    BackgroundThumb entry;
    entry.bitmap = renderer_.LoadScaledBitmap(library_.BackgroundDirectory() / fileName,
                                              kBackgroundThumbMaxWidth,
                                              kBackgroundThumbMaxHeight, entry.sourceWidth,
                                              entry.sourceHeight);
    auto inserted = backgroundThumbCache_.emplace(fileName, std::move(entry));
    return &inserted.first->second;
}

void Application::RefreshBackgroundList() {
    backgroundFiles_ = library_.BackgroundImageNames();
    // 清理已不存在图片的缩略图缓存。
    for (auto it = backgroundThumbCache_.begin(); it != backgroundThumbCache_.end();) {
        if (std::find(backgroundFiles_.begin(), backgroundFiles_.end(), it->first) ==
            backgroundFiles_.end()) {
            if (backgroundBitmapName_ == it->first) {
                backgroundBitmap_.Reset();
                backgroundBitmapName_.clear();
            }
            it = backgroundThumbCache_.erase(it);
        } else {
            ++it;
        }
    }
    // 当前背景图片已被删除时回退到主题背景。
    data::ReaderSettings& settings = library_.Settings();
    if (settings.backgroundType == L"image" &&
        std::find(backgroundFiles_.begin(), backgroundFiles_.end(), settings.backgroundImage) ==
            backgroundFiles_.end()) {
        settings.backgroundType = L"theme";
        settings.backgroundImage.clear();
    }
}

void Application::SelectBackground(const std::wstring& type, const std::wstring& value) {
    data::ReaderSettings& settings = library_.Settings();
    settings.backgroundType = type;
    if (type == L"color") {
        settings.backgroundColor = value;
    } else if (type == L"image") {
        settings.backgroundImage = value;
    }
    std::wstring error;
    library_.Save(error);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Application::ImportBackgroundImages() {
    if (backgroundFiles_.size() >= data::kMaxBackgroundImages) {
        ShowStatus(L"背景图片最多 " + std::to_wstring(data::kMaxBackgroundImages) +
                   L" 张，请先删除后再导入");
        return;
    }
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog))) ||
        !dialog) {
        ShowStatus(L"无法创建图片选择对话框");
        return;
    }
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_ALLOWMULTISELECT | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
    const COMDLG_FILTERSPEC filters[] = {
        {L"图片（PNG / JPEG / BMP / GIF / WebP）", L"*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.webp"},
        {L"所有文件", L"*.*"}};
    dialog->SetFileTypes(2, filters);
    dialog->SetTitle(L"选择背景图片（最多 32 张，可多选）");
    if (FAILED(dialog->Show(hwnd_))) {
        return;
    }
    ComPtr<IShellItemArray> results;
    if (FAILED(dialog->GetResults(&results)) || !results) {
        return;
    }
    DWORD resultCount = 0;
    results->GetCount(&resultCount);
    std::wstring firstImported;
    std::wstring lastError;
    size_t importedCount = 0;
    for (DWORD i = 0; i < resultCount; ++i) {
        if (backgroundFiles_.size() + importedCount >= data::kMaxBackgroundImages) {
            lastError = L"背景图片最多 " + std::to_wstring(data::kMaxBackgroundImages) +
                        L" 张，其余图片未导入";
            break;
        }
        ComPtr<IShellItem> item;
        if (FAILED(results->GetItemAt(i, &item)) || !item) {
            continue;
        }
        PWSTR rawPath = nullptr;
        if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &rawPath)) || !rawPath) {
            continue;
        }
        const std::filesystem::path source(rawPath);
        CoTaskMemFree(rawPath);
        std::wstring error;
        std::wstring name;
        if (library_.ImportBackgroundImage(source, name, error)) {
            ++importedCount;
            if (firstImported.empty()) {
                firstImported = name;
            }
        } else {
            lastError = error;
        }
    }
    RefreshBackgroundList();
    if (!firstImported.empty()) {
        // 导入后直接应用第一张，方便立即看到背景效果。
        SelectBackground(L"image", firstImported);
        ShowStatus(L"已导入 " + std::to_wstring(importedCount) + L" 张背景图片");
    } else if (!lastError.empty()) {
        ShowStatus(lastError);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
    // 导入后列表条目增加，立即重绘以同步命中区域。
    UpdateWindow(hwnd_);
}

void Application::ApplySearchHighlight(IDWriteTextLayout* layout, const std::wstring& pageText,
                                       const ThemeColors& colors) {
    if (!layout || searchHighlight_.empty()) {
        return;
    }
    const std::wstring query = LowerCase(searchHighlight_);
    if (query.empty()) {
        return;
    }
    ID2D1SolidColorBrush* brush = renderer_.SolidBrush(colors.accent);
    if (!brush) {
        return;
    }
    const std::wstring lowered = LowerCase(pageText);
    size_t position = 0;
    int count = 0;
    while (count < 200) {
        position = lowered.find(query, position);
        if (position == std::wstring::npos) {
            break;
        }
        DWRITE_TEXT_RANGE range{};
        range.startPosition = static_cast<UINT32>(position);
        range.length = static_cast<UINT32>(query.size());
        layout->SetDrawingEffect(brush, range);
        position += query.size();
        ++count;
    }
}

void Application::ComputeTextMetrics(float& textWidth, float& textHeight,
                                     D2D1_RECT_F& textRect) const {
    const float panelReserve = panel_ != PanelMode::None ? PanelWidth() : 0.0f;
    const float availableWidth = widthDip_ - panelReserve;
    const float horizontalPadding = 64.0f;
    textWidth = std::min(availableWidth - horizontalPadding * 2.0f, 860.0f);
    textWidth = std::max(textWidth, 200.0f);
    const float gestureOffset = gestureBarVisible_ ? 52.0f : 0.0f;
    const float top = kReaderToolbarHeight + 30.0f + gestureOffset;
    const float bottom = heightDip_ - kReaderFooterHeight - 26.0f;
    textHeight = std::max(bottom - top, 80.0f);
    const float left = (availableWidth - textWidth) / 2.0f;
    textRect = D2D1::RectF(left, top, left + textWidth, top + textHeight);
}

}  // namespace ui
