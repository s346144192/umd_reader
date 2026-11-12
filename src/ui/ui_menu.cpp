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
#include "data/book_capabilities.h"
#include "nlohmann/json.hpp"
#include "ui/ui_internal.h"

namespace ui {
using namespace detail;

std::vector<Application::MenuEntry> Application::ReaderMenuEntries(float x, float y) {
    std::vector<MenuEntry> entries;
    if (panel_ == PanelMode::Toc) {
        // 目录右键：只在目录面板范围内响应，按不同规则重新提取纯文本章节，最多三级（集 → 章 → 节）。
        const D2D1_RECT_F panelRect =
            D2D1::RectF(widthDip_ - PanelWidth(), kReaderToolbarHeight, widthDip_, heightDip_);
        const data::BookRecord* record = library_.Find(activeBookId_);
        if (PointInside(panelRect, x, y) && record != nullptr &&
            data::CapabilitiesOf(*record).chapterUnitsEditable) {
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
    // 右键菜单只保留「复制」与「询问助手」，避免分散阅读注意力。
    if (fixedPages_.IsOpen() && fixedPages_.HasSelection() &&
        PointInside(fixedPages_.PageRect(PdfViewportRect()), x, y)) {
        entries.push_back(MenuEntry{L"copy-pdf-selection", L"复制", -1, std::wstring()});
        entries.push_back(MenuEntry{L"ask-assistant", L"询问助手", -1, std::wstring()});
        return entries;
    }
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
        entries.push_back(MenuEntry{L"ask-assistant", L"询问助手", -1, std::wstring()});
    }
    return entries;
}

std::vector<Application::MenuEntry> Application::ShelfMenuEntries() {
    return {MenuEntry{L"shelf-settings", L"全局设置", -1, std::wstring()},
            MenuEntry{L"shelf-categories", L"分类管理", -1, std::wstring()},
            MenuEntry{L"shelf-lan", L"局域网分享", -1, std::wstring()},
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
    } else if (entry.id == L"copy-pdf-selection") {
        CopyPdfSelection();
    } else if (entry.id == L"ask-assistant") {
        AskAssistantWithSelection();
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
    } else if (entry.id == L"shelf-settings") {
        shelfSettingsVisible_ = true;
        categoryManagerVisible_ = false;
        InvalidateRect(hwnd_, nullptr, FALSE);
    } else if (entry.id == L"shelf-categories") {
        categoryManagerVisible_ = true;
        shelfSettingsVisible_ = false;
        InvalidateRect(hwnd_, nullptr, FALSE);
    } else if (entry.id == L"shelf-lan") {
        OpenLanShare();
    } else if (entry.id == L"shelf-data") {
        const std::wstring directory = library_.DataDirectory().wstring();
        ShellExecuteW(hwnd_, L"open", directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

}  // namespace ui
