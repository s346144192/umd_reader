#include "ui/application.h"

#include <windowsx.h>
#include <d2d1helper.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

#include "core/text_util.h"
#include "ui/ui_internal.h"

namespace ui {
using namespace detail;

namespace {

constexpr float kPanelPadding = 20.0f;
constexpr float kPanelHeaderHeight = 56.0f;
constexpr float kSummaryHeight = 176.0f;
constexpr float kSummaryGap = 14.0f;
constexpr float kBadgeGapX = 12.0f;
constexpr float kBadgeGapY = 12.0f;
// 章节停留达到该秒数才计为一次有效阅读，快速翻页经过的章节不计入。
constexpr uint32_t kChapterVisitMinSeconds = 3;

D2D1_RECT_F AchievementsPanelRect(float widthDip, float heightDip) {
    const float panelWidth = std::min(760.0f, std::max(480.0f, widthDip - 48.0f));
    const float panelHeight = std::min(600.0f, std::max(420.0f, heightDip - 32.0f));
    const float left = (widthDip - panelWidth) * 0.5f;
    const float top = (heightDip - panelHeight) * 0.5f;
    return D2D1::RectF(left, top, left + panelWidth, top + panelHeight);
}

D2D1_RECT_F LevelCardRect(const D2D1_RECT_F& panel) {
    const float innerWidth = panel.right - panel.left - kPanelPadding * 2.0f;
    const float levelWidth = innerWidth * 0.44f;
    const float top = panel.top + kPanelHeaderHeight;
    return D2D1::RectF(panel.left + kPanelPadding, top, panel.left + kPanelPadding + levelWidth,
                       top + kSummaryHeight);
}

D2D1_RECT_F CalendarCardRect(const D2D1_RECT_F& panel) {
    const D2D1_RECT_F level = LevelCardRect(panel);
    return D2D1::RectF(level.right + kSummaryGap, level.top, panel.right - kPanelPadding,
                       level.bottom);
}

}  // namespace

void Application::DrawReadingLevelChip(const ThemeColors& colors, const D2D1_RECT_F& rect) {
    const TextFormats& formats = renderer_.Formats();
    const data::ReadingLevelInfo info =
        data::EvaluateReadingLevel(library_.Settings().readingStats.totalSeconds, ReadBookCount());
    const bool hovered = IsHovered(rect);
    renderer_.FillRoundedRect(rect, 9.0f, hovered ? colors.surfaceAlt : colors.surface);
    renderer_.StrokeRoundedRect(rect, 9.0f, hovered ? colors.accent : colors.divider, 1.0f);

    const float textLeft = rect.left + 14.0f;
    const float textRight = rect.right - 6.0f;
    const bool compact = rect.bottom - rect.top < kReadingChipHeight;
    renderer_.DrawTextBlock(
        L"Lv." + std::to_wstring(info.level) + L" " + info.title.name,
        D2D1::RectF(textLeft, rect.top + (compact ? 1.0f : 4.0f), textRight,
                    rect.top + (compact ? 17.0f : 22.0f)),
        formats.caption.Get(), colors.text);

    // 经验进度条与数值：每级 60 分。
    const float barTop = rect.top + (compact ? 24.0f : 29.0f);
    const float barBottom = barTop + 5.0f;
    const float barRight = rect.right - 46.0f;
    const D2D1_RECT_F track = D2D1::RectF(textLeft, barTop, barRight, barBottom);
    renderer_.FillRoundedRect(track, 2.5f, colors.divider);
    const float ratio = static_cast<float>(info.scoreInLevel) /
                        static_cast<float>(data::kReadingScorePerLevel);
    if (ratio > 0.0f && track.right > track.left) {
        const D2D1_RECT_F fill = D2D1::RectF(track.left, track.top,
                                            track.left + (track.right - track.left) * ratio,
                                            track.bottom);
        renderer_.FillRoundedRect(fill, 2.5f, colors.accent);
    }
    renderer_.DrawTextBlock(std::to_wstring(info.scoreInLevel) + L"/" +
                                std::to_wstring(data::kReadingScorePerLevel),
                            D2D1::RectF(rect.right - 44.0f, rect.top + (compact ? 16.0f : 22.0f),
                                        rect.right - 6.0f, rect.top + (compact ? 31.0f : 38.0f)),
                            formats.captionRight.Get(), colors.secondaryText);
    AddHit(rect, HitKind::OpenReadingAchievements);
}

D2D1_RECT_F Application::ReadingAchievementsPanelRect() const {
    return AchievementsPanelRect(widthDip_, heightDip_);
}

D2D1_RECT_F Application::ReadingAchievementsCloseRect() const {
    const D2D1_RECT_F panel = ReadingAchievementsPanelRect();
    return D2D1::RectF(panel.right - kPanelPadding - 60.0f, panel.top + 14.0f,
                       panel.right - kPanelPadding, panel.top + 44.0f);
}

D2D1_RECT_F Application::ReadingCalendarPrevRect() const {
    const D2D1_RECT_F calendar = CalendarCardRect(ReadingAchievementsPanelRect());
    return D2D1::RectF(calendar.right - 84.0f, calendar.top + 8.0f, calendar.right - 58.0f,
                       calendar.top + 32.0f);
}

D2D1_RECT_F Application::ReadingCalendarNextRect() const {
    const D2D1_RECT_F calendar = CalendarCardRect(ReadingAchievementsPanelRect());
    return D2D1::RectF(calendar.right - 54.0f, calendar.top + 8.0f, calendar.right - 28.0f,
                       calendar.top + 32.0f);
}

void Application::OpenReadingAchievements() {
    const int64_t now = util::CurrentUnixSeconds();
    std::time_t timestamp = static_cast<std::time_t>(now);
    std::tm local{};
    localtime_s(&local, &timestamp);
    readingCalendarYear_ = local.tm_year + 1900;
    readingCalendarMonth_ = local.tm_mon + 1;
    readingAchievementsVisible_ = true;
    CloseMenu();
    RefreshReadingBadges();
    InvalidateRect(hwnd_, nullptr, FALSE);
    UpdateWindow(hwnd_);
}

void Application::CloseReadingAchievements() {
    readingAchievementsVisible_ = false;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Application::DrawReadingAchievements(const ThemeColors& colors) {
    const TextFormats& formats = renderer_.Formats();
    // 半透明遮罩，突出成就浮层。
    D2D1_COLOR_F mask = colors.text;
    mask.a = 0.45f;
    renderer_.FillRect(D2D1::RectF(0.0f, 0.0f, widthDip_, heightDip_), mask);

    const D2D1_RECT_F panel = ReadingAchievementsPanelRect();
    renderer_.FillRoundedRect(panel, 14.0f, colors.background);
    renderer_.StrokeRoundedRect(panel, 14.0f, colors.divider, 1.0f);
    renderer_.DrawTextBlock(L"阅读成就",
                            D2D1::RectF(panel.left + kPanelPadding, panel.top + 14.0f,
                                        panel.right - 100.0f, panel.top + 46.0f),
                            formats.section.Get(), colors.text);
    DrawPlainButton(L"关闭", ReadingAchievementsCloseRect(), HitKind::ReadingAchievementsClose,
                    colors);

    const D2D1_RECT_F levelCard = LevelCardRect(panel);
    const D2D1_RECT_F calendarCard = CalendarCardRect(panel);
    renderer_.FillRoundedRect(levelCard, 12.0f, colors.surface);
    renderer_.FillRoundedRect(calendarCard, 12.0f, colors.surface);

    // 左上：等级、经验进度与书籍要求。
    const data::ReadingLevelInfo info =
        data::EvaluateReadingLevel(library_.Settings().readingStats.totalSeconds, ReadBookCount());

    const float levelLeft = levelCard.left + 14.0f;
    const float levelRight = levelCard.right - 14.0f;
    renderer_.DrawTextBlock(L"阅读等级",
                            D2D1::RectF(levelLeft, levelCard.top + 10.0f, levelRight,
                                        levelCard.top + 28.0f),
                            formats.caption.Get(), colors.secondaryText);
    renderer_.DrawTextBlock(
        info.title.name + L"　Lv." + std::to_wstring(info.level),
        D2D1::RectF(levelLeft, levelCard.top + 28.0f, levelRight, levelCard.top + 58.0f),
        formats.section.Get(), colors.text);

    const float barTop = levelCard.top + 66.0f;
    const D2D1_RECT_F track =
        D2D1::RectF(levelLeft, barTop, levelRight, barTop + 8.0f);
    renderer_.FillRoundedRect(track, 4.0f, colors.surfaceAlt);
    const float ratio = static_cast<float>(info.scoreInLevel) /
                        static_cast<float>(data::kReadingScorePerLevel);
    if (ratio > 0.0f) {
        renderer_.FillRoundedRect(
            D2D1::RectF(track.left, track.top,
                        track.left + (track.right - track.left) * ratio, track.bottom),
            4.0f, colors.accent);
    }
    renderer_.DrawTextBlock(
        L"经验 " + std::to_wstring(info.scoreInLevel) + L" / " +
            std::to_wstring(data::kReadingScorePerLevel),
        D2D1::RectF(levelLeft, barTop + 10.0f, levelRight, barTop + 28.0f),
        formats.caption.Get(), colors.secondaryText);
    renderer_.DrawTextBlock(
        L"累计 " + std::to_wstring(info.totalScore) + L" 分（每阅读 10 分钟加 1 分）",
        D2D1::RectF(levelLeft, levelCard.top + 100.0f, levelRight, levelCard.top + 118.0f),
        formats.caption.Get(), colors.secondaryText);
    renderer_.DrawTextBlock(L"已读书籍 " + std::to_wstring(info.books) + L" 本",
                            D2D1::RectF(levelLeft, levelCard.top + 118.0f, levelRight,
                                        levelCard.top + 136.0f),
                            formats.caption.Get(), colors.secondaryText);

    // 下一称号同时要求等级与累计书籍数量，两项条件一并展示。
    const std::wstring requirement =
        L"下一称号 " + info.nextTitleName + L"：等级 Lv." +
        std::to_wstring(data::LevelRequiredForRank(info.nextRank)) + L"、累计 " +
        std::to_wstring(info.nextBooksRequired) + L" 本";
    renderer_.DrawTextBlock(requirement,
                            D2D1::RectF(levelLeft, levelCard.top + 134.0f, levelRight,
                                        levelCard.bottom - 2.0f),
                            formats.captionWrap.Get(), colors.accent);

    // 右上：月历，标记有阅读的日期，可切换月份。
    const std::wstring monthLabel =
        std::to_wstring(readingCalendarYear_) + L"年" +
        std::to_wstring(readingCalendarMonth_) + L"月";
    renderer_.DrawTextBlock(monthLabel,
                            D2D1::RectF(calendarCard.left + 12.0f, calendarCard.top + 8.0f,
                                        calendarCard.right - 88.0f, calendarCard.top + 32.0f),
                            formats.label.Get(), colors.text);
    DrawPlainButton(L"‹", ReadingCalendarPrevRect(), HitKind::ReadingCalendarPrev, colors);
    DrawPlainButton(L"›", ReadingCalendarNextRect(), HitKind::ReadingCalendarNext, colors);

    constexpr const wchar_t* kWeekdays[7] = {L"一", L"二", L"三", L"四", L"五", L"六", L"日"};
    const float gridLeft = calendarCard.left + 12.0f;
    const float gridRight = calendarCard.right - 12.0f;
    const float weekdayTop = calendarCard.top + 36.0f;
    const float cellWidth = (gridRight - gridLeft) / 7.0f;
    for (int column = 0; column < 7; ++column) {
        renderer_.DrawTextBlock(
            kWeekdays[column],
            D2D1::RectF(gridLeft + column * cellWidth, weekdayTop,
                        gridLeft + (column + 1) * cellWidth, weekdayTop + 18.0f),
            formats.buttonSmall.Get(), colors.secondaryText);
    }
    const float gridTop = calendarCard.top + 56.0f;
    const float gridBottom = calendarCard.bottom - 8.0f;
    const float rowHeight = (gridBottom - gridTop) / 6.0f;
    const int days = data::DaysInMonth(readingCalendarYear_, readingCalendarMonth_);
    const int firstWeekday =
        data::FirstWeekdayOfMonth(readingCalendarYear_, readingCalendarMonth_);
    const std::wstring todayKey = data::LocalDateKey(util::CurrentUnixSeconds());
    for (int day = 1; day <= days; ++day) {
        const int slot = firstWeekday + day - 1;
        const int column = slot % 7;
        const int row = slot / 7;
        if (row >= 6) {
            break;
        }
        const D2D1_RECT_F cell = D2D1::RectF(
            gridLeft + column * cellWidth + 2.0f, gridTop + row * rowHeight + 1.0f,
            gridLeft + (column + 1) * cellWidth - 2.0f, gridTop + (row + 1) * rowHeight - 1.0f);
        const bool hasReading =
            data::HasReadingOn(library_.Settings().readingStats, readingCalendarYear_,
                               readingCalendarMonth_, day);
        wchar_t dateBuffer[16];
        swprintf_s(dateBuffer, L"%04d-%02d-%02d", readingCalendarYear_, readingCalendarMonth_,
                   day);
        const bool isToday = todayKey == std::wstring(dateBuffer);
        if (hasReading) {
            D2D1_COLOR_F fill = colors.accent;
            fill.a = 0.22f;
            renderer_.FillRoundedRect(cell, 7.0f, fill);
        }
        if (isToday) {
            renderer_.StrokeRoundedRect(cell, 7.0f, colors.accent, 1.2f);
        }
        renderer_.DrawTextBlock(std::to_wstring(day), cell, formats.buttonSmall.Get(),
                                hasReading ? colors.accent : colors.text);
    }

    // 下方：徽章列表，两行排列，展示名称、获得状态与剩余有效期。
    const float badgesTop = calendarCard.bottom + 16.0f;
    renderer_.DrawTextBlock(
        L"徽章", D2D1::RectF(panel.left + kPanelPadding, badgesTop,
                             panel.left + kPanelPadding + 100.0f, badgesTop + 24.0f),
        formats.label.Get(), colors.text);
    renderer_.DrawTextBlock(
        L"已阅读 " + std::to_wstring(readingBadges_.booksRead) + L" 本　单本超过 8 小时 " +
            std::to_wstring(readingBadges_.qualifiedBooks) + L" 本",
        D2D1::RectF(panel.left + kPanelPadding + 100.0f, badgesTop, panel.right - kPanelPadding,
                    badgesTop + 24.0f),
        formats.captionRight.Get(), colors.secondaryText);

    const float gridTopBadges = badgesTop + 30.0f;
    const float gridBottomBadges = panel.bottom - 16.0f;
    const float cellHeight = (gridBottomBadges - gridTopBadges - kBadgeGapY) / 2.0f;
    const float cellWidthBadge =
        (panel.right - panel.left - kPanelPadding * 2.0f - kBadgeGapX * 3.0f) / 4.0f;
    for (size_t index = 0; index < readingBadges_.badges.size() && index < 8; ++index) {
        const int row = static_cast<int>(index / 4);
        const int column = static_cast<int>(index % 4);
        const D2D1_RECT_F cell = D2D1::RectF(
            panel.left + kPanelPadding +
                column * (cellWidthBadge + kBadgeGapX),
            gridTopBadges + row * (cellHeight + kBadgeGapY),
            panel.left + kPanelPadding + column * (cellWidthBadge + kBadgeGapX) +
                cellWidthBadge,
            gridTopBadges + row * (cellHeight + kBadgeGapY) + cellHeight);
        const data::ReadingBadgeView& badge = readingBadges_.badges[index];
        renderer_.FillRoundedRect(cell, 10.0f, colors.surface);
        if (badge.earned) {
            renderer_.StrokeRoundedRect(cell, 10.0f, colors.accent, 1.0f);
        }
        const float textLeft = cell.left + 10.0f;
        const float textRight = cell.right - 8.0f;
        renderer_.DrawTextBlock(badge.name,
                                D2D1::RectF(textLeft, cell.top + 8.0f, textRight,
                                            cell.top + 28.0f),
                                formats.label.Get(), colors.text);
        std::wstring status;
        if (badge.earned) {
            status = badge.permanent ? L"永久"
                                     : L"剩余 " + std::to_wstring(badge.daysRemaining) + L" 天";
        } else if (badge.requiredBooks > 0) {
            status = L"已达标 " + std::to_wstring(badge.qualifiedBooks) + L"/" +
                     std::to_wstring(badge.requiredBooks) + L" 本";
        } else {
            status = L"未获得";
        }
        renderer_.DrawTextBlock(status,
                                D2D1::RectF(textLeft, cell.top + 28.0f, textRight,
                                            cell.top + 46.0f),
                                formats.caption.Get(),
                                badge.earned ? colors.accent : colors.secondaryText);
        renderer_.DrawTextBlock(badge.description,
                                D2D1::RectF(cell.left + 10.0f, cell.top + 46.0f,
                                            cell.right - 8.0f, cell.bottom - 6.0f),
                                formats.captionWrap.Get(), colors.secondaryText);
    }
}

bool Application::ReadingCountsTime() const {
    if (view_ != ViewMode::Reader || activeBookId_.empty() || activeBook_.content.empty()) {
        return false;
    }
    // 成就浮层打开时视为查看统计，不计入阅读时长。
    if (readingAchievementsVisible_) {
        return false;
    }
    if (IsIconic(hwnd_)) {
        return false;
    }
    return GetForegroundWindow() == hwnd_;
}

void Application::TickReading() {
    const uint64_t nowMs = GetTickCount64();
    if (readingTickMs_ == 0) {
        readingTickMs_ = nowMs;
        return;
    }
    const uint64_t delta = nowMs - readingTickMs_;
    readingTickMs_ = nowMs;
    if (!ReadingCountsTime()) {
        readingPendingMs_ = 0;
        return;
    }
    readingPendingMs_ += std::min<uint64_t>(delta, kReadingMaxTickMs);
    const uint64_t wholeSeconds = readingPendingMs_ / 1000;
    if (wholeSeconds == 0) {
        return;
    }
    readingPendingMs_ -= wholeSeconds * 1000;
    ApplyReadingSeconds(wholeSeconds);
}

void Application::ApplyReadingSeconds(uint64_t seconds) {
    data::ReadingStats& stats = library_.Settings().readingStats;
    const int64_t now = util::CurrentUnixSeconds();
    const uint64_t previousScore = stats.totalSeconds / data::kReadingSecondsPerScore;
    stats.totalSeconds += seconds;

    const std::wstring dateKey = data::LocalDateKey(now);
    data::DailyReadingRecord& day = stats.daily[dateKey];
    day.seconds += static_cast<uint32_t>(seconds);
    if (data::IsNightReadingHour(data::LocalHourOf(now))) {
        day.nightSeconds += static_cast<uint32_t>(seconds);
    }

    // 切换书籍或章节时先结算上一段的阅读次数，再累计当前章节。
    if (readingStatBookId_ != activeBookId_ || readingStatChapter_ != activeChapter_) {
        CommitChapterVisit();
        readingStatBookId_ = activeBookId_;
        readingStatChapter_ = activeChapter_;
        readingStatChapterSeconds_ = 0;
    }

    data::BookRecord* book = library_.Find(activeBookId_);
    if (book != nullptr) {
        book->readingSeconds += seconds;
        const bool hasChapter =
            activeChapter_ >= 0 && activeChapter_ < static_cast<int>(activeBook_.chapters.size());
        if (hasChapter) {
            data::ChapterReadingStat& chapterStat = book->chapterStats[activeChapter_];
            chapterStat.seconds += seconds;
            readingStatChapterSeconds_ += static_cast<uint32_t>(seconds);
        }
        // 按日明细写入当前月份的阅读记录文件。
        readingLog_.AddSeconds(now, activeBookId_, book->title,
                               hasChapter ? activeChapter_ : -1,
                               static_cast<uint32_t>(seconds));
    }
    readingUnsavedSeconds_ += seconds;
    RefreshReadingBadges();
    if (readingUnsavedSeconds_ >= kReadingSaveIntervalSeconds) {
        FlushReadingStats(false);
    }
    // 经验分数发生变化时刷新界面，避免每秒重绘。
    const uint64_t currentScore = stats.totalSeconds / data::kReadingSecondsPerScore;
    if (currentScore != previousScore) {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void Application::CommitChapterVisit() {
    const uint32_t pendingSeconds = readingStatChapterSeconds_;
    const std::wstring bookId = readingStatBookId_;
    const int chapterIndex = readingStatChapter_;
    readingStatChapterSeconds_ = 0;
    if (pendingSeconds < kChapterVisitMinSeconds || bookId.empty() || chapterIndex < 0) {
        return;
    }
    data::BookRecord* book = library_.Find(bookId);
    if (book == nullptr || chapterIndex >= static_cast<int>(book->chapters.size())) {
        return;
    }
    book->chapterStats[chapterIndex].visits += 1;
    // 当天的章节明细同样记一次有效阅读。
    readingLog_.AddChapterVisit(util::CurrentUnixSeconds(), bookId, book->title, chapterIndex);
    // 章节切换频率有限，直接落盘，避免切换书籍或退出时丢失本次结算。
    std::wstring error;
    if (!library_.SaveBook(bookId, error) && !error.empty()) {
        ShowStatus(error);
    }
}

void Application::FlushReadingStats(bool force) {
    if (readingUnsavedSeconds_ == 0 && !force) {
        return;
    }
    readingUnsavedSeconds_ = 0;
    std::wstring error;
    const bool saved =
        activeBookId_.empty() ? library_.SaveIndex(error) : library_.SaveBook(activeBookId_, error);
    if (!saved && !error.empty()) {
        ShowStatus(error);
    }
    // 阅读记录按月份写入独立文件，与书籍统计数据分开落盘。
    std::wstring logError;
    if (!readingLog_.Save(logError) && !logError.empty()) {
        ShowStatus(logError);
    }
}

void Application::RefreshReadingBadges() {
    readingBadges_ =
        data::RefreshReadingBadges(library_.Settings().readingStats, BookReadingSeconds(),
                                   util::CurrentUnixSeconds());
}

int Application::ReadBookCount() const {
    int count = 0;
    for (const data::BookRecord& book : library_.Books()) {
        if (book.readingSeconds > 0) {
            ++count;
        }
    }
    return count;
}

std::vector<uint64_t> Application::BookReadingSeconds() const {
    std::vector<uint64_t> seconds;
    seconds.reserve(library_.Books().size());
    for (const data::BookRecord& book : library_.Books()) {
        seconds.push_back(book.readingSeconds);
    }
    return seconds;
}

}  // namespace ui
