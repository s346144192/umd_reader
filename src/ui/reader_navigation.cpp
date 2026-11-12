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

void Application::ApplyChapterMode(const std::wstring& mode) {
    data::BookRecord* record = library_.Find(activeBookId_);
    if (record == nullptr || !data::CapabilitiesOf(*record).chapterUnitsEditable) {
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
            : umd::DetectChaptersByUnits(activeBook_.content,
                                         umd::ChapterUnitsForMode(normalized), true);
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
    // 重新提取章节后层级关系可能变化，折叠状态需要重置。
    collapsedChapters_.clear();
    record->chapters.clear();
    for (const umd::Chapter& chapter : activeBook_.chapters) {
        data::ChapterInfo info;
        info.title = chapter.title;
        info.charOffset = chapter.charOffset;
        info.charLength = chapter.charLength;
        info.pageIndex = chapter.pageIndex;
        info.level = chapter.level;
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
    if (speaking_ || aiSpeechLoading_) {
        StopSpeech();
        return;
    }
    if (fixedPages_.IsOpen()) {
        ShowStatus(L"PDF 按原版页面阅读，暂不支持朗读");
        return;
    }
    const std::wstring text = BuildSpeechText();
    if (text.empty()) {
        ShowStatus(selectionActive_ ? L"选中内容没有可朗读的文字"
                                    : L"当前页没有可朗读的文字");
        return;
    }
    if (library_.Settings().speechEngine == L"ai") {
        StartAiSpeechJob(text);
        return;
    }
    if (!voice_) {
        ShowStatus(L"系统语音引擎不可用");
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
    PlaySoundW(nullptr, nullptr, 0);
    if (aiSpeechLoading_) {
        aiSpeechCancel_ = true;
        aiSpeechLoading_ = false;
    }
    if (voice_) {
        voice_->Speak(nullptr, SPF_PURGEBEFORESPEAK, nullptr);
    }
    speaking_ = false;
    InvalidateRect(hwnd_, nullptr, FALSE);
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

    // 无文本层的 PDF 只能按原版页面显示；其余格式统一走文本流重排。
    const bool fixedPage = parsed.formatType == 3 && parsed.content.empty();
    fixedPages_.Close();
    if (fixedPage) {
        std::wstring pageError;
        if (!fixedPages_.Open(std::filesystem::path(record->path),
                              static_cast<int>(record->progressPageIndex), pageError)) {
            ShowStatus(L"打开失败：" + pageError);
            return;
        }
        // 恢复上次选择的页面视图与缩放比例，双页视图会按组重新对齐进度页。
        ApplyPdfPageLayout();
        fixedPages_.SetZoom(record->pdfZoom);
        fixedPages_.SetCurrentPage(static_cast<int>(record->progressPageIndex));
    }

    const uint32_t resumeOffset = record->progressCharOffset;
    // 切换到新书之前，先结算上一本书当前章节的阅读次数，再写入统计与徽章状态。
    CommitChapterVisit();
    FlushReadingStats(true);
    activeBookId_ = id;
    activeBook_ = std::move(parsed);
    // 纯文本按已保存的提取方式重建章节，保证章节偏移与书签、高亮、标注中记录的章节下标一致。
    if (activeBook_.formatType == 0 && record->chapterMode != L"auto") {
        std::vector<umd::Chapter> chapters =
            umd::DetectChaptersByUnits(activeBook_.content,
                                       umd::ChapterUnitsForMode(record->chapterMode), true);
        if (!chapters.empty()) {
            activeBook_.chapters = std::move(chapters);
        } else {
            record->chapterMode = L"auto";
        }
    }
    // PDF 的章节名随解析规则更新：打开时同步到书籍记录，已导入的书无需重新导入。
    if (activeBook_.formatType == 3) {
        record->chapters.clear();
        for (const umd::Chapter& chapter : activeBook_.chapters) {
            data::ChapterInfo info;
            info.title = chapter.title;
            info.charOffset = chapter.charOffset;
            info.charLength = chapter.charLength;
            info.pageIndex = chapter.pageIndex;
            info.level = chapter.level;
            record->chapters.push_back(std::move(info));
        }
    }
    activeChapter_ = 0;
    activePage_ = 0;
    lineOffset_ = 0;
    // 新书的目录树默认全部展开。
    collapsedChapters_.clear();
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
    // 关闭 AI 设置弹框并丢弃未保存的编辑。
    CloseAiSettings();
    aiGenerateVisible_ = false;
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

    library_.TouchBook(id, resumeOffset,
                       fixedPage ? static_cast<uint32_t>(fixedPages_.CurrentPage()) : 0);
    SetWindowTextW(hwnd_, (activeBook_.title + L" - " + app_id::kWindowTitle).c_str());
    if (fixedPage) {
        // 固定页阅读按页序号定位，不涉及字符偏移。
        view_ = ViewMode::Reader;
        if (!activeBook_.warnings.empty()) {
            ShowStatus(activeBook_.warnings.front());
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
    } else {
        GoToGlobalOffset(resumeOffset);
    }

    // 分区头校验：内容变化时提示高亮与标注可能失效；旧记录则补记编码并重建分区头。
    // 同时记录正文格式：UMD 的高亮与标注按章节内偏移保存，纯文本才使用分区偏移。
    record->formatType = activeBook_.formatType;
    // PDF 按页阅读没有正文文本，同步正文长度，书架进度改为按页码计算。
    record->contentChars = activeBook_.ContentCharCount();
    record->pdfPageCount = static_cast<uint32_t>(activeBook_.pdfPages.size());
    if (record->pdfPageCount > 0) {
        record->progressPageIndex = static_cast<uint32_t>(fixedPages_.CurrentPage());
    } else {
        record->progressPageIndex = 0;
    }
    // 固定页模式没有可重排正文，不维护分区头样本。
    if (!fixedPage && (record->partitionHeaders.empty() || record->textEncoding == 0)) {
        record->textEncoding = activeBook_.sourceCodePage;
        record->partitionHeaders =
            data::BuildPartitionHeaders(activeBook_.content, record->textEncoding);
    } else if (!fixedPage &&
               data::CountChangedPartitions(activeBook_.content, record->partitionHeaders,
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
    const uint32_t fixedPageIndex =
        fixedPages_.IsOpen() ? static_cast<uint32_t>(fixedPages_.CurrentPage()) : 0;
    if (!activeBookId_.empty()) {
        library_.TouchBook(activeBookId_, CurrentGlobalOffset(), fixedPageIndex);
        std::wstring error;
        library_.SaveBook(activeBookId_, error);
        // 书籍详情与索引已包含累计阅读秒数，重置未落盘标记。
        readingUnsavedSeconds_ = 0;
    }
    activeBook_ = umd::Book{};
    fixedPages_.Close();
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
    collapsedChapters_.clear();
    activeChapter_ = 0;
    activePage_ = 0;
    lineOffset_ = 0;
    view_ = ViewMode::Shelf;
    panel_ = PanelMode::None;
    // 关闭 AI 设置弹框并丢弃未保存的编辑。
    CloseAiSettings();
    aiGenerateVisible_ = false;
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
    SetWindowTextW(hwnd_, app_id::kWindowTitle);
    RefreshShelfOrder();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

namespace {

// 判断自动分配的正文章节是否为空时，最多扫描的字符数。
constexpr size_t kBlankPrefaceScanLimit = 255;

// 不可见空白字符：C0 控制字符、DEL、常见 Unicode 空格与零宽字符。
bool IsInvisibleWhitespace(wchar_t character) {
    if (character < 0x20 || character == 0x7F) {
        return true;
    }
    if (character >= 0x2000 && character <= 0x200A) {
        return true;
    }
    switch (character) {
    case 0x00A0:  // 不换行空格
    case 0x200B:  // 零宽空格
    case 0x200C:  // 零宽非连接符
    case 0x200D:  // 零宽连接符
    case 0x2028:  // 行分隔符
    case 0x2029:  // 段分隔符
    case 0x202F:  // 窄不换行空格
    case 0x205F:  // 中数学空格
    case 0x2060:  // 词连接符
    case 0x3000:  // 全角空格
    case 0xFEFF:  // 字节顺序标记
        return true;
    default:
        return false;
    }
}

// 纯文本自动分配的正文章节（第一个标题之前的首章）如果只包含空白字符，
// 阅读时应跳过它进入第一章；没有第一章时保留该章节。
bool IsSkippableBlankPreface(const umd::Book& book, int chapterIndex) {
    if (book.formatType != 0 || chapterIndex != 0 || book.chapters.size() < 2) {
        return false;
    }
    const umd::Chapter& preface = book.chapters.front();
    if (preface.title != L"正文") {
        return false;
    }
    const size_t begin = std::min<size_t>(preface.charOffset, book.content.size());
    const size_t limit = std::min<size_t>(preface.charLength, kBlankPrefaceScanLimit);
    const size_t end = std::min(begin + limit, book.content.size());
    for (size_t i = begin; i < end; ++i) {
        if (!IsInvisibleWhitespace(book.content[i])) {
            return false;
        }
    }
    return true;
}

}  // namespace

void Application::LoadChapter(int chapterIndex, uint32_t localOffset) {
    if (speaking_) {
        StopSpeech();
    }
    if (activeBook_.chapters.empty()) {
        return;
    }
    int clamped =
        std::clamp(chapterIndex, 0, static_cast<int>(activeBook_.chapters.size()) - 1);
    if (IsSkippableBlankPreface(activeBook_, clamped)) {
        // 空正文章节没有可阅读内容，直接定位到第一章开头。
        clamped = 1;
        localOffset = 0;
    }
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
    // 固定页阅读按页面翻页，与文本分页无关；双页视图每次翻一组。
    if (fixedPages_.IsOpen()) {
        if (!fixedPages_.NextPage()) {
            ShowStatus(L"已经到达全书末尾");
            return;
        }
        fixedPages_.InvalidateBitmap();
        PersistFixedPageProgress();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
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
    // 固定页阅读按页面翻页，与文本分页无关；双页视图每次翻一组。
    if (fixedPages_.IsOpen()) {
        if (!fixedPages_.PreviousPage()) {
            ShowStatus(L"已经到达全书开头");
            return;
        }
        fixedPages_.InvalidateBitmap();
        PersistFixedPageProgress();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
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

void Application::PersistFixedPageProgress() {
    if (activeBookId_.empty() || !fixedPages_.IsOpen()) {
        return;
    }
    // 目录中按当前页同步选中的章节，翻页后高亮与“当前章”提示保持一致。
    const uint32_t currentPage = static_cast<uint32_t>(fixedPages_.CurrentPage());
    int chapterIndex = 0;
    for (size_t index = 0; index < activeBook_.chapters.size(); ++index) {
        if (activeBook_.chapters[index].pageIndex <= currentPage) {
            chapterIndex = static_cast<int>(index);
        } else {
            break;
        }
    }
    activeChapter_ = chapterIndex;

    if (library_.Find(activeBookId_) == nullptr) {
        return;
    }
    library_.TouchBook(activeBookId_, 0, currentPage);
    std::wstring error;
    library_.SaveBook(activeBookId_, error);
}

void Application::ApplyPdfPageLayout() {
    if (!fixedPages_.IsOpen()) {
        return;
    }
    const data::ReaderSettings& settings = library_.Settings();
    fixedPages_.SetLayout(settings.pdfPageLayout == L"spread"
                              ? reader::FixedPageReader::PageLayout::Spread
                              : reader::FixedPageReader::PageLayout::Single);
}

void Application::TogglePdfPageLayout() {
    if (!fixedPages_.IsOpen()) {
        return;
    }
    const bool spread = fixedPages_.Layout() == reader::FixedPageReader::PageLayout::Spread;
    fixedPages_.SetLayout(spread ? reader::FixedPageReader::PageLayout::Single
                                 : reader::FixedPageReader::PageLayout::Spread);
    data::ReaderSettings& settings = library_.Settings();
    settings.pdfPageLayout = spread ? L"single" : L"spread";
    std::wstring error;
    library_.SaveIndex(error);
    // 双页与单页的页码含义不同，立即按新视图重新记录进度。
    PersistFixedPageProgress();
    ShowStatus(spread ? L"已切换为单页视图" : L"已切换为双页视图");
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Application::ScrollReadingLines(int direction, int lines) {
    if (fixedPages_.IsOpen()) {
        return;
    }
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
    library_.TouchBook(activeBookId_, CurrentGlobalOffset(), 0);
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
    // 固定页阅读按页码定位：滑块顶端对应首页，底端对应末页。
    if (fixedPages_.IsOpen()) {
        const int pageCount = fixedPages_.PageCount();
        if (pageCount <= 0) {
            return;
        }
        int targetPage =
            static_cast<int>(std::lround(std::clamp(ratio, 0.0f, 1.0f) *
                                         static_cast<float>(pageCount - 1)));
        // 双页视图以偶数页为左页，先对齐到包含目标页的组首。
        if (fixedPages_.Layout() == reader::FixedPageReader::PageLayout::Spread) {
            targetPage -= targetPage % 2;
        }
        if (targetPage == fixedPages_.CurrentPage()) {
            return;
        }
        fixedPages_.SetCurrentPage(targetPage);
        fixedPages_.InvalidateBitmap();
        PersistFixedPageProgress();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
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
    if (!fixedPages_.IsOpen() && pager_.PageCount() == 0) {
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

    float ratio = 0.0f;
    if (fixedPages_.IsOpen()) {
        // 页码进度：首页在顶端，末页在底端。
        const int pageCount = fixedPages_.PageCount();
        if (pageCount > 1) {
            ratio = static_cast<float>(fixedPages_.CurrentPage()) /
                    static_cast<float>(pageCount - 1);
        }
    } else {
        const uint32_t total = activeBook_.ContentCharCount();
        ratio = total > 0 ? std::clamp(static_cast<float>(CurrentGlobalOffset()) /
                                           static_cast<float>(total),
                                       0.0f, 1.0f)
                          : 0.0f;
    }
    ratio = std::clamp(ratio, 0.0f, 1.0f);
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
    if (fixedPages_.IsOpen()) {
        searchStatus_ = L"PDF 按原版页面阅读，暂不支持全文检索";
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


}  // namespace ui
