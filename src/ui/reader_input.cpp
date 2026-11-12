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
#include <filesystem>
#include <iterator>
#include <regex>
#include <vector>

#include "core/text_util.h"
#include "nlohmann/json.hpp"
#include "ui/ui_internal.h"

namespace ui {
using namespace detail;

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

std::wstring Application::SelectedReaderText() const {
    if (!selectionActive_ || activeChapterRaw_.empty()) {
        return std::wstring();
    }
    const uint32_t startOriginal = chapterDisplay_.ToOriginal(selectionStartDisplay_);
    const uint32_t endOriginal = chapterDisplay_.ToOriginal(selectionEndDisplay_);
    if (endOriginal <= startOriginal) {
        return std::wstring();
    }
    const uint32_t limitedEnd =
        std::min(endOriginal, static_cast<uint32_t>(activeChapterRaw_.size()));
    if (limitedEnd <= startOriginal) {
        return std::wstring();
    }
    return activeChapterRaw_.substr(startOriginal, limitedEnd - startOriginal);
}

void Application::CopySelection() {
    const std::wstring text = SelectedReaderText();
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

std::wstring* Application::ActiveInputText() {
    if (lanShareVisible_ && lanAddressFocused_) {
        return &lanAddressDraft_;
    }
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
    if (lanShareVisible_ && lanAddressFocused_) {
        const D2D1_RECT_F box = LanAddressInputRect();
        return D2D1::RectF(box.left + 10.0f, box.top + 7.0f, box.right - 10.0f,
                           box.bottom - 7.0f);
    }
    if (view_ == ViewMode::Shelf && shelfSearchFocused_) {
        const D2D1_RECT_F box = ShelfSearchBoxRect();
        return D2D1::RectF(box.left + 10.0f, box.top + 6.0f, box.right - 10.0f,
                           box.bottom - 6.0f);
    }
    if (aiGenerateVisible_ && chatFocus_ == ChatFocus::GeneratePrompt) {
        const D2D1_RECT_F box = AiGeneratePromptRect();
        return D2D1::RectF(box.left + 10.0f, box.top + 5.0f, box.right - 10.0f,
                           box.bottom - 5.0f);
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
    if (lanShareVisible_ && lanAddressFocused_) {
        // 分享地址只需要主机名与端口。
        return 64;
    }
    if (aiSettingsVisible_) {
        // AI 接口设置弹框在书架与阅读视图中都可打开，不受书架输入长度限制。
        return 1024;
    }
    if (aiGenerateVisible_) {
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
    if (lanShareVisible_ && lanAddressFocused_) {
        const D2D1_RECT_F boxRect = LanAddressInputRect();
        const D2D1_RECT_F textRect =
            D2D1::RectF(boxRect.left + 10.0f, boxRect.top + 7.0f, boxRect.right - 10.0f,
                        boxRect.bottom - 7.0f);
        const float fallbackHeight = formats.listItem->GetFontSize() * 1.35f;
        caret = CaretPointAt(lanAddressDraft_, formats.listItem.Get(), textRect,
                             renderer_.WriteFactory(), false, inputCaret_, fallbackHeight,
                             lineHeight);
        return true;
    }
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
    if (aiGenerateVisible_ && chatFocus_ == ChatFocus::GeneratePrompt) {
        const D2D1_RECT_F boxRect = AiGeneratePromptRect();
        const D2D1_RECT_F textRect =
            D2D1::RectF(boxRect.left + 10.0f, boxRect.top + 5.0f, boxRect.right - 10.0f,
                        boxRect.bottom - 5.0f);
        const float fallbackHeight = formats.listItem->GetFontSize() * 1.35f;
        caret = CaretPointAt(aiGeneratePrompt_, formats.listItem.Get(), textRect,
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

}  // namespace ui
