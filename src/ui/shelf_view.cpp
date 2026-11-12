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

void Application::DrawShelf(const ThemeColors& colors) {
    renderer_.Clear(colors.background);
    const TextFormats& formats = renderer_.Formats();

    // 右上角：等级芯片与添加书籍、设置按钮自右向左排列，芯片占用最右侧位置。
    const float buttonTop = 24.0f;
    const float buttonHeight = 40.0f;
    const float rightEdge = widthDip_ - 44.0f;
    const D2D1_RECT_F chipRect =
        D2D1::RectF(rightEdge - kReadingChipWidth, buttonTop, rightEdge,
                    buttonTop + kReadingChipHeight);
    const D2D1_RECT_F addRect =
        D2D1::RectF(chipRect.left - 12.0f - 128.0f, buttonTop, chipRect.left - 12.0f,
                    buttonTop + buttonHeight);
    const D2D1_RECT_F associateRect =
        D2D1::RectF(addRect.left - 12.0f - 128.0f, buttonTop, addRect.left - 12.0f,
                    buttonTop + buttonHeight);
    // 局域网分享入口位于设置按钮左侧。
    const D2D1_RECT_F lanRect =
        D2D1::RectF(associateRect.left - 12.0f - 108.0f, buttonTop, associateRect.left - 12.0f,
                    buttonTop + buttonHeight);

    renderer_.DrawTextBlock(L"书架", D2D1::RectF(44.0f, 20.0f, 400.0f, 56.0f), formats.title.Get(),
                            colors.text);
    renderer_.DrawTextBlock(FormatCountLine(library_.Books().size()),
                            D2D1::RectF(44.0f, 60.0f, lanRect.left - 16.0f, 82.0f),
                            formats.caption.Get(), colors.secondaryText);

    DrawReadingLevelChip(colors, chipRect);
    DrawButton(L"添加书籍", addRect, HitKind::AddBook, colors, true, false);
    DrawButton(L"设置", associateRect, HitKind::ShelfSettings, colors, false,
               shelfSettingsVisible_);
    DrawButton(L"局域网", lanRect, HitKind::OpenLanShare, colors, false,
               lanShareVisible_ || lanSharing_);

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
        DrawLanShare(colors);
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
    DrawLanShare(colors);
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
                       IsFileExtensionAssociated(L".umd", app_id::kDocumentProgId),
                       HitKind::ToggleUmdAssociation);
    drawAssociationRow(top + 164.0f, L".txt 文件",
                       IsFileExtensionAssociated(L".txt", app_id::kTextProgId),
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
    // 书架弹框或阅读成就浮层打开时，其下方的书籍卡片不响应悬停，
    // 避免移动鼠标时底层卡片被高亮。
    const bool overlayOpen = shelfSettingsVisible_ || categoryManagerVisible_ ||
                             aiSettingsVisible_ || lanShareVisible_ ||
                             readingAchievementsVisible_ ||
                             !shelfCategoryPickerBookId_.empty();
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

    double ratio = 0.0;
    if (book.contentChars > 0) {
        ratio = static_cast<double>(std::min(book.progressCharOffset, book.contentChars)) /
                static_cast<double>(book.contentChars);
    } else if (book.pdfPageCount > 0) {
        // 固定页 PDF 以页序号记录进度，按页数换算百分比。
        ratio = static_cast<double>(std::min(book.progressPageIndex, book.pdfPageCount)) /
                static_cast<double>(book.pdfPageCount);
    }
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


void Application::ImportFiles(const std::vector<std::filesystem::path>& paths) {
    int added = 0;
    int existed = 0;
    std::wstring lastError;

    // 大型 PDF 的逐页解码耗时较长，整个导入过程显示等待光标。
    SetCursor(LoadCursorW(nullptr, IDC_WAIT));
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
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    coverCache_.clear();
    RefreshShelfOrder();
    // 分享开启期间导入新书后立即更新对外书单。
    if (lanServer_) {
        RefreshLanShareBooks();
    }

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
        {L"电子书（UMD / 文本 / PDF）", L"*.umd;*.txt;*.pdf"},
        {L"UMD 电子书", L"*.umd"},
        {L"文本文件", L"*.txt"},
        {L"PDF 文档", L"*.pdf"},
        {L"所有文件", L"*.*"}};
    dialog->SetFileTypes(5, filters);
    dialog->SetTitle(L"选择电子书（UMD、TXT 或 PDF）");

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
        target = FindWindowW(app_id::kWindowClass, nullptr);
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

}  // namespace ui
