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
        // 页面视图与缩放：仅 PDF 按页阅读时显示。
        {L"单页", HitKind::PdfPageLayout, PanelMode::None},
        {L"缩小", HitKind::PdfZoomOut, PanelMode::None},
        {L"放大", HitKind::PdfZoomIn, PanelMode::None},
    };

    std::vector<ToolbarItem> visibleItems;
    const data::BookRecord* toolbarRecord = library_.Find(activeBookId_);
    const bool hasImages = toolbarRecord && !toolbarRecord->imageFiles.empty();
    for (const ToolbarItem& item : items) {
        if (item.kind == HitKind::OpenImages && !hasImages) {
            continue;
        }
        if ((item.kind == HitKind::PdfPageLayout || item.kind == HitKind::PdfZoomOut ||
             item.kind == HitKind::PdfZoomIn) &&
            !fixedPages_.IsOpen()) {
            continue;
        }
        // 固定页 PDF 没有可检索、可标注的正文文本，也没有文本页书签，隐藏对应入口。
        if (fixedPages_.IsOpen() &&
            (item.kind == HitKind::OpenSearch || item.kind == HitKind::OpenGestureBar ||
             item.kind == HitKind::OpenBookmarks)) {
            continue;
        }
        visibleItems.push_back(item);
    }

    // 章节标题始终在窗口内居中，宽度不足时只调整右侧选项栏。
    const float headingRight = widthDip_ * 0.5f + 150.0f;
    constexpr float kButtonWidth = 60.0f;
    constexpr float kButtonHeight = 34.0f;
    constexpr float kButtonGap = 6.0f;
    const auto toolbarLeftFor = [&](size_t count) {
        const float rightLimit = widthDip_ - 16.0f;
        if (count == 0) {
            return rightLimit;
        }
        const float total = static_cast<float>(count) * kButtonWidth +
                            static_cast<float>(count - 1) * kButtonGap;
        return rightLimit - total;
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

    // 顶部标题显示当前章节并保持居中对齐，窗口较窄时对称收缩，避免与按钮重叠。
    const std::wstring chapterHeading =
        SanitizeLine(ChapterTitleAt(activeBook_, activeChapter_));
    const float headingCenter = widthDip_ * 0.5f;
    const float headingHalf =
        std::min(150.0f, std::max(0.0f, toolbarLeft - 12.0f - headingCenter));
    if (headingHalf > 20.0f && headingCenter - headingHalf > 148.0f) {
        renderer_.DrawTextBlock(
            chapterHeading,
            D2D1::RectF(headingCenter - headingHalf, 18.0f, headingCenter + headingHalf,
                        46.0f),
            formats.centered.Get(), colors.text);
    }

    float left = toolbarLeft;
    for (const ToolbarItem& item : visibleItems) {
        bool active = panel_ == item.mode;
        if (item.kind == HitKind::OpenGestureBar) {
            active = gestureBarVisible_;
        } else if (item.kind == HitKind::ToggleSpeech) {
            active = speaking_;
        }
        const D2D1_RECT_F buttonRect =
            D2D1::RectF(left, 15.0f, left + kButtonWidth, 15.0f + kButtonHeight);
        // PDF 页面视图与缩放按钮使用图标，悬停与选中样式与其他工具栏按钮一致。
        if (item.kind == HitKind::PdfPageLayout || item.kind == HitKind::PdfZoomOut ||
            item.kind == HitKind::PdfZoomIn) {
            const bool layoutButton = item.kind == HitKind::PdfPageLayout;
            const bool spread =
                layoutButton &&
                fixedPages_.Layout() == reader::FixedPageReader::PageLayout::Spread;
            if (IsHovered(buttonRect) || spread) {
                renderer_.FillRoundedRect(buttonRect, 9.0f, colors.surfaceAlt);
            }
            if (spread) {
                renderer_.StrokeRoundedRect(buttonRect, 9.0f, colors.accent, 1.0f);
            }
            const float centerX = (buttonRect.left + buttonRect.right) * 0.5f;
            const float centerY = (buttonRect.top + buttonRect.bottom) * 0.5f;
            if (layoutButton) {
                DrawPdfPageLayoutIcon(renderer_, centerX, centerY, spread,
                                      spread ? colors.accent : colors.text);
            } else {
                DrawPdfZoomIcon(renderer_, centerX, centerY, item.kind == HitKind::PdfZoomIn,
                                colors.text);
            }
            AddHit(buttonRect, item.kind);
            left += kButtonWidth + kButtonGap;
            continue;
        }
        // 朗读按钮在朗读进行中显示「停止」，便于直接中断。
        std::wstring label = item.label;
        if (item.kind == HitKind::ToggleSpeech) {
            label = speaking_ ? L"停止" : L"朗读";
        }
        DrawButton(label, buttonRect, item.kind, colors, false, active);
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

    // 固定页 PDF：按原版页面位图显示，正文区域不参与文本排版。
    if (fixedPages_.IsOpen()) {
        const float pageViewRight =
            widthDip_ - (panel_ != PanelMode::None ? PanelWidth() : 0.0f);
        const D2D1_RECT_F pageViewport =
            D2D1::RectF(0.0f, kReaderToolbarHeight, pageViewRight,
                        heightDip_ - kReaderFooterHeight);
        renderer_.PushClip(pageViewport);
        fixedPages_.Draw(renderer_, pageViewport);
        renderer_.PopClip();
    } else if (pager_.PageCount() > 0) {
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

    std::wstring pageText;
    if (fixedPages_.IsOpen()) {
        // 固定页阅读的进度以页码表示，没有字符百分比；双页视图显示当前一组的页范围。
        const int pageTotal = std::max(1, fixedPages_.PageCount());
        const int firstPage = fixedPages_.CurrentPage() + 1;
        const int lastPage = std::min(pageTotal, fixedPages_.CurrentPage() +
                                                      fixedPages_.VisiblePageCount());
        if (lastPage > firstPage) {
            pageText = L"第 " + std::to_wstring(firstPage) + L"–" + std::to_wstring(lastPage) +
                       L" / " + std::to_wstring(pageTotal) + L" 页";
        } else {
            pageText = L"第 " + std::to_wstring(firstPage) + L" / " +
                       std::to_wstring(pageTotal) + L" 页";
        }
        // 缩放比例与视图一致时省略，避免底栏长期显示无意义的 100%。
        const int zoomPercent = static_cast<int>(std::lround(fixedPages_.Zoom() * 100.0f));
        if (zoomPercent != 100) {
            pageText += L" · " + std::to_wstring(zoomPercent) + L"%";
        }
    } else {
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
        pageText = L"第 " + std::to_wstring(pageIndex + 1) + L" / " +
                   std::to_wstring(pageCount) + L" 页 · " +
                   std::to_wstring(percent) + L"%";
    }
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

    // 阅读区底部居中：阅读助手入口。PDF 按页阅读同样可用，提问时带上选中的文字。
    DrawButton(L"阅读助手", AiChatEntryRect(), HitKind::OpenChat, colors, false,
               panel_ == PanelMode::Chat);

    DrawReaderScrollbar(colors);
    if (!fixedPages_.IsOpen()) {
        DrawGestureBar(colors);
    }
    if (colorPickerVisible_) {
        DrawColorPicker(colors);
    }
    DrawReaderPanel(colors);
    DrawAiSettings(colors);
    DrawAiGenerate(colors);
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
    // 目录、书签与阅读助手面板左侧分割条：拖动可调整面板宽度。
    if (panel_ == PanelMode::Chat || panel_ == PanelMode::Toc ||
        panel_ == PanelMode::Bookmarks) {
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
    const size_t chapterCount = activeBook_.chapters.size();

    // 按折叠状态整理出可见行：章节按先序排列，父节点折叠后隐藏其后代。
    std::vector<int> visibleRows;
    visibleRows.reserve(chapterCount);
    std::vector<int> ancestors;
    for (size_t i = 0; i < chapterCount; ++i) {
        const int level = std::max(1, static_cast<int>(activeBook_.chapters[i].level));
        while (!ancestors.empty()) {
            const int ancestorLevel = std::max(
                1, static_cast<int>(
                       activeBook_.chapters[static_cast<size_t>(ancestors.back())].level));
            if (ancestorLevel < level) {
                break;
            }
            ancestors.pop_back();
        }
        bool hidden = false;
        for (const int ancestor : ancestors) {
            if (collapsedChapters_.count(ancestor) > 0) {
                hidden = true;
                break;
            }
        }
        if (!hidden) {
            visibleRows.push_back(static_cast<int>(i));
        }
        ancestors.push_back(static_cast<int>(i));
    }

    const float viewportHeight = contentRect.bottom - contentRect.top;
    const float totalHeight = static_cast<float>(visibleRows.size()) * itemHeight;
    panelScrollMax_ = std::max(0.0f, totalHeight - viewportHeight);
    panelScroll_ = std::clamp(panelScroll_, 0.0f, panelScrollMax_);

    BeginClip(contentRect);
    float y = contentRect.top - panelScroll_;
    for (const int index : visibleRows) {
        const umd::Chapter& chapter = activeBook_.chapters[static_cast<size_t>(index)];
        const int level = std::max(1, static_cast<int>(chapter.level));
        // 下一章节层级更深时，当前节点是父节点，可展开与折叠。
        const bool hasChildren =
            static_cast<size_t>(index + 1) < chapterCount &&
            std::max(1, static_cast<int>(
                            activeBook_.chapters[static_cast<size_t>(index + 1)].level)) > level;
        const bool collapsed = collapsedChapters_.count(index) > 0;

        const D2D1_RECT_F itemRect =
            D2D1::RectF(contentRect.left, y, contentRect.right, y + itemHeight);
        if (itemRect.bottom > contentRect.top && itemRect.top < contentRect.bottom) {
            const bool current = index == activeChapter_;
            if (current) {
                renderer_.FillRoundedRect(itemRect, 8.0f, colors.surfaceAlt);
                renderer_.FillRect(D2D1::RectF(itemRect.left, itemRect.top + 10.0f,
                                               itemRect.left + 3.0f, itemRect.bottom - 10.0f),
                                   colors.accent);
            } else if (IsHovered(itemRect)) {
                renderer_.FillRoundedRect(itemRect, 8.0f, Darken(colors.surfaceAlt, 1.02f));
            }
            const float indent = 12.0f + static_cast<float>(level - 1) * 16.0f;
            float titleLeft = itemRect.left + indent;
            if (hasChildren) {
                const D2D1_RECT_F foldRect =
                    D2D1::RectF(itemRect.left + indent - 2.0f, itemRect.top + 12.0f,
                                itemRect.left + indent + 18.0f, itemRect.bottom - 12.0f);
                renderer_.DrawTextBlock(collapsed ? L"▸" : L"▾", foldRect,
                                        formats.caption.Get(), colors.secondaryText);
                AddHit(foldRect, HitKind::ChapterFold, index);
                titleLeft += 22.0f;
            }
            std::wstring title = ChapterTitleAt(activeBook_, index);
            title = util::Ellipsize(SanitizeLine(title), 30);
            renderer_.DrawTextBlock(title,
                                    D2D1::RectF(titleLeft, itemRect.top + 13.0f,
                                                itemRect.right - 10.0f, itemRect.bottom - 10.0f),
                                    formats.listItem.Get(),
                                    current ? colors.accent : colors.text);
            AddHit(D2D1::RectF(titleLeft, itemRect.top, itemRect.right, itemRect.bottom),
                   HitKind::ChapterItem, index);
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
