#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <sapi.h>
#include <shellapi.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "data/library.h"
#include "ui/chapter_display.h"
#include "ui/reader_pager.h"
#include "ui/render_context.h"
#include "umd/umd_parser.h"

namespace ui {

class Application {
public:
    Application();
    ~Application();

    bool Initialize(HINSTANCE instance, int showCommand);
    int Run();

private:
    enum class ViewMode { Shelf, Reader };
    enum class PanelMode { None, Toc, Bookmarks, Search, Settings, Images, NoteEditor, Chat };
    enum class Gesture { Default, Highlight, EraseHighlight, Note, EraseNote, Copy, Speech };
    enum class ChatFocus { None, BaseUrl, ApiKey, Model, Message };

    enum class HitKind {
        AddBook,
        OpenDataFolder,
        ShelfSettings,
        ShelfSettingsClose,
        // 全局设置中的「设置快捷键」入口。
        OpenShortcutSettings,
        ShelfSearchBox,
        CategoryManager,
        CategoryManagerClose,
        BookCard,
        RemoveBook,
        BookCategoryButton,
        CategoryAll,
        CategoryUncategorized,
        CategoryItem,
        CategoryCreate,
        CategoryRename,
        CategoryDelete,
        CategoryNameInput,
        CategoryPickerItem,
        BackToShelf,
        // 菜单项通过索引对应当前菜单条目。
        MenuItem,
        OpenToc,
        OpenBookmarks,
        OpenSearch,
        OpenSettings,
        OpenImages,
        OpenGestureBar,
        ClosePanel,
        OpenChat,
        ChatBaseUrl,
        ChatApiKey,
        ChatModel,
        ChatInput,
        ChatSend,
        ChatStop,
        // 阅读助手面板的左边缘分割条与对话内容区的滚动条。
        ChatPanelSplitter,
        ChatScrollbar,
        // AI 对话的会话管理：打开对话记录、新建、列表项、删除、关闭，以及复制单条消息。
        ChatSessionOpen,
        ChatSessionNew,
        ChatSessionItem,
        ChatSessionDelete,
        ChatSessionClose,
        ChatCopyMessage,
        // 展开/折叠某条消息的思考内容。
        ChatThinkingToggle,
        ChatProtocolOpenAi,
        ChatProtocolAnthropic,
        // 思考等级按钮，index 为 0..3。
        AiThinkingLevel,
        OpenAiSettings,
        AiSettingsSave,
        AiSettingsClose,
        ChapterItem,
        BookmarkItem,
        AddBookmark,
        RemoveBookmark,
        SearchItem,
        SearchBox,
        // 字体选择弹框：打开、选择某一字族、关闭。
        FontPickerOpen,
        FontPickerItem,
        FontPickerClose,
        FontSmaller,
        FontLarger,
        SpacingSmaller,
        SpacingLarger,
        ThemeOption,
        // 阅读区背景：跟随主题、纯色色块、背景图片及其删除、导入。
        BackgroundTheme,
        BackgroundColor,
        BackgroundImage,
        BackgroundRemove,
        BackgroundImport,
        ScrollModeLine,
        ScrollModePage,
        ReaderScrollbar,
        // 目录与书签列表的竖向滚动条。
        PanelScrollbar,
        // 文件关联复选框：勾选时关联，取消勾选时取消关联。
        ToggleUmdAssociation,
        ToggleTxtAssociation,
        ToggleSpeech,
        GestureOption,
        HighlightColor,
        NoteInput,
        NoteSave,
        NoteCancel,
        // 快捷键映射窗口：行、关闭与恢复默认。
        ShortcutRow,
        ShortcutClose,
        ShortcutReset
    };

    struct HitRegion {
        D2D1_RECT_F rect{};
        HitKind kind = HitKind::AddBook;
        int index = -1;
        std::wstring value;
    };

    struct SearchResult {
        int chapterIndex = 0;
        uint32_t charOffset = 0;
        std::wstring context;
    };

    struct AnnotationHit {
        D2D1_RECT_F rect{};
        int index = -1;
        bool isNote = false;
    };

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

    void OnCreate();
    void OnPaint();
    void OnResize(UINT widthPixels, UINT heightPixels);
    void OnDpiChanged(const RECT& suggested);
    void OnMouseMove(int xPixels, int yPixels);
    void OnMouseLeave();
    void OnLeftButtonDown(int xPixels, int yPixels);
    void OnLeftButtonUp(int xPixels, int yPixels);
    // 返回是否已作为双击手势处理；未处理时由消息分发按普通单击继续处理，
    // 避免窗口类启用 CS_DBLCLKS 后第二次点击只收到双击消息而被丢弃。
    bool OnLeftButtonDoubleClick(int xPixels, int yPixels);
    void OnMouseWheel(int xPixels, int yPixels, int delta);
    void OnKeyDown(WPARAM key);
    void OnChar(wchar_t character);
    void OnUnicodeInput(uint32_t codePoint);
    void OnDropFiles(HDROP drop);
    void OnClose();

    void DrawShelf(const ThemeColors& colors);
    void DrawReader(const ThemeColors& colors);
    // 弹出菜单：工具栏入口与右键都使用同一套条目与绘制。
    struct MenuEntry {
        std::wstring id;
        std::wstring label;
        int index = -1;
        std::wstring value;
    };
    void OpenMenu(float x, float y, std::vector<MenuEntry> entries);
    void CloseMenu();
    void DrawMenu(const ThemeColors& colors);
    D2D1_RECT_F MenuRect() const;
    void ExecuteMenuEntry(const MenuEntry& entry);
    void OnRightButtonDown(int xPixels, int yPixels);
    std::vector<MenuEntry> ReaderMenuEntries(float x, float y);
    std::vector<MenuEntry> ShelfMenuEntries();
    // 按指定规则重新提取纯文本章节并保存。
    void ApplyChapterMode(const std::wstring& mode);
    // 阅读区背景：跟随主题时不额外绘制，纯色填充，图片按覆盖方式铺满。
    void DrawReaderBackground(const ThemeColors& colors);
    ID2D1Bitmap* BackgroundBitmap();
    // 背景列表缩略图，按需解码并缓存；sourceWidth/sourceHeight 为原图像素尺寸。
    struct BackgroundThumb {
        ComPtr<ID2D1Bitmap> bitmap;
        uint32_t sourceWidth = 0;
        uint32_t sourceHeight = 0;
    };
    const BackgroundThumb* BackgroundThumbFor(const std::wstring& fileName);
    void RefreshBackgroundList();
    void ImportBackgroundImages();
    void SelectBackground(const std::wstring& type, const std::wstring& value);
    void DrawReaderPanel(const ThemeColors& colors);
    void DrawTocPanel(const ThemeColors& colors, const D2D1_RECT_F& contentRect);
    void DrawBookmarksPanel(const ThemeColors& colors, const D2D1_RECT_F& contentRect);
    void DrawSearchPanel(const ThemeColors& colors, const D2D1_RECT_F& contentRect);
    void DrawSettingsPanel(const ThemeColors& colors, const D2D1_RECT_F& contentRect);
    void DrawImagesPanel(const ThemeColors& colors, const D2D1_RECT_F& contentRect);
    void DrawNoteEditorPanel(const ThemeColors& colors, const D2D1_RECT_F& contentRect);
    void DrawChatPanel(const ThemeColors& colors, const D2D1_RECT_F& contentRect);
    // AI 对话内容：按会话消息逐条绘制，支持滚动与逐条复制。
    void DrawChatConversation(const D2D1_RECT_F& rect, const ThemeColors& colors);
    // 对话气泡的排版结果：绘制、命中检测与选择共用同一套几何。
    struct ChatBubble {
        int messageIndex = -1;  // 会话内消息下标；流式回复为 -1
        bool isUser = false;
        D2D1_RECT_F bubble{};
        D2D1_RECT_F textRect{};
        ComPtr<IDWriteTextLayout> layout;
        float top = 0.0f;
        float height = 0.0f;
        // 思考内容与其展开状态；为空表示该消息没有思考过程。
        std::wstring thinking;
        bool thinkingExpanded = false;
        float thinkingHeight = 0.0f;
        ComPtr<IDWriteTextLayout> thinkingLayout;
        D2D1_RECT_F thinkingRect{};
        D2D1_RECT_F thinkingToggleRect{};
    };
    void BuildChatBubbles(const D2D1_RECT_F& rect, std::vector<ChatBubble>& bubbles);
    bool BeginChatSelection(float x, float y);
    void UpdateChatSelection(float x, float y);
    uint32_t ChatTextPositionAt(const ChatBubble& bubble, float x, float y) const;
    void DrawChatSelection(const ChatBubble& bubble, const D2D1_RECT_F& textRect,
                           const ThemeColors& colors);
    bool CopyChatSelection();
    void ClearChatSelection();
    // 对话记录弹框：切换、删除已保存的会话。
    void DrawChatSessionPicker(const ThemeColors& colors);
    D2D1_RECT_F ChatSessionPickerPanelRect() const;
    D2D1_RECT_F ChatSessionPickerListRect() const;
    data::ChatSession* ActiveChatSession();
    data::ChatSession* EnsureChatSession();
    data::ChatSession* CreateChatSession();
    void NewChatSession();
    void DeleteChatSession(const std::wstring& id);
    void SelectChatSession(const std::wstring& id);
    void SaveChatState();
    void CopyChatMessage(int messageIndex);
    bool CopyTextToClipboard(const std::wstring& text);
    void DrawAiSettings(const ThemeColors& colors);
    // 字体选择弹框：在独立弹框中枚举系统字族，避免把全部字体铺在设置页。
    void DrawFontPicker(const ThemeColors& colors);
    D2D1_RECT_F FontPickerPanelRect() const;
    D2D1_RECT_F FontPickerListRect() const;
    std::wstring CurrentFontDisplayName() const;
    void SelectFontFamily(const std::wstring& family);
    // 快捷键映射窗口。
    void DrawShortcutSettings(const ThemeColors& colors);
    D2D1_RECT_F ShortcutPanelRect() const;
    void OpenShortcutSettings();
    void CloseShortcutSettings();
    void AssignShortcut(UINT virtualKey);
    void ResetShortcuts();
    // AI 接口设置弹框的布局，供绘制与命中检测共用。
    D2D1_RECT_F AiSettingsPanelRect() const;
    D2D1_RECT_F AiSettingsFieldRect(ChatFocus field) const;
    // 阅读区底部居中的 AI 对话入口。
    D2D1_RECT_F AiChatEntryRect() const;
    void OpenAiSettings();
    void CloseAiSettings();
    void SaveAiSettings();
    void DrawGestureBar(const ThemeColors& colors);
    void DrawColorPicker(const ThemeColors& colors);
    void DrawCopyToast(const ThemeColors& colors);
    void DrawInputCaret(const ThemeColors& colors);
    void DrawHighlights(IDWriteTextLayout* pageLayout, uint32_t pageStart, uint32_t pageEnd,
                        const D2D1_RECT_F& textRect, const ThemeColors& colors);
    void DrawSelection(IDWriteTextLayout* pageLayout, uint32_t pageStart, uint32_t pageEnd,
                       const D2D1_RECT_F& textRect, const ThemeColors& colors);
    void DrawNotes(IDWriteTextLayout* pageLayout, uint32_t pageStart, uint32_t pageEnd,
                   const D2D1_RECT_F& textRect, const ThemeColors& colors);
    void DrawBookCard(const data::BookRecord& book, const D2D1_RECT_F& cardRect,
                      const ThemeColors& colors, int index);
    void DrawButton(const std::wstring& label, const D2D1_RECT_F& rect, HitKind kind,
                    const ThemeColors& colors, bool primary, bool active, int index = -1,
                    const std::wstring& value = std::wstring());
    // 非加粗按钮：用于对话面板内的次要操作（新建会话、复制消息）。
    void DrawPlainButton(const std::wstring& label, const D2D1_RECT_F& rect, HitKind kind,
                         const ThemeColors& colors, int index = -1,
                         const std::wstring& value = std::wstring());
    void DrawEmptyShelf(const ThemeColors& colors);
    void DrawShelfSettings(const ThemeColors& colors);
    void DrawCategoryManager(const ThemeColors& colors);
    void DrawCategorySidebar(const ThemeColors& colors);
    void DrawCategoryPicker(const ThemeColors& colors);
    D2D1_RECT_F CategoryNameInputRect() const;
    D2D1_RECT_F ShelfSearchBoxRect() const;
    void ApplyShelfSearch();
    void ClearShelfSearch();
    void CreateCategoryFromDraft();
    void RenameSelectedCategory();
    void DeleteSelectedCategory();

    void AddHit(const D2D1_RECT_F& rect, HitKind kind, int index = -1,
                const std::wstring& value = std::wstring());
    bool IsHovered(const D2D1_RECT_F& rect) const;
    const HitRegion* HitTest(float x, float y) const;
    void BeginClip(const D2D1_RECT_F& rect);
    void EndClip();

    void RefreshShelfOrder();
    void ShowStatus(const std::wstring& text);
    void ShowCopyToast();
    void ImportFiles(const std::vector<std::filesystem::path>& paths);
    void OpenFileDialog();
    // 处理命令行与已运行实例转交的文件路径。
    void OpenStartupFiles();
    void OpenPath(const std::wstring& path);
    void OpenExternalFile(const std::wstring& path);
    bool ForwardToExistingInstance() const;
    bool HandleCopyData(const COPYDATASTRUCT& data);
    // 按单个扩展名注册当前用户级文件关联。
    void AssociateFileExtension(const std::wstring& extension, const std::wstring& progId,
                                const std::wstring& description);
    // 取消单个扩展名的当前用户级文件关联。
    void DisassociateFileExtension(const std::wstring& extension, const std::wstring& progId);
    // 语音朗读当前页：再次触发即停止。
    void ToggleSpeech();
    void StopSpeech();
    std::wstring BuildSpeechText() const;
    void OpenBook(const std::wstring& id);
    void CloseBook();
    void LoadChapter(int chapterIndex, uint32_t localOffset);
    void RebuildPagination(uint32_t preserveGlobalOffset);
    // 整章显示文本与分页的缓存。打开目录、书签、标注等侧栏会改变排版宽度，
    // 缓存两种宽度下的结果即可避免反复重建整章分页。
    struct ChapterLayout {
        ChapterDisplay display;
        ChapterPager pager;
    };
    struct ChapterLayoutCacheEntry {
        int chapterIndex = -1;
        float width = 0.0f;
        float height = 0.0f;
        ChapterLayout layout;
    };
    ChapterLayout* FindCachedLayout(int chapterIndex, float width, float height);
    void StoreCachedLayout(int chapterIndex, float width, float height);
    void ClearLayoutCache();
    uint32_t CurrentGlobalOffset() const;
    void GoToGlobalOffset(uint32_t globalOffset, bool closePanel = true);
    void NextPage();
    void PreviousPage();
    // 行滚动阅读：以视口顶部对齐的行首偏移驱动连续滚动。
    bool LineScrollMode() const;
    uint32_t ViewportStartOffset() const;
    uint32_t NextLineStart(uint32_t from);
    uint32_t PreviousLineStart(uint32_t from);
    void ScrollReadingLines(int direction, int lines);
    void AlignLineOffsetToCurrentPage();
    float ScrollRatioFromY(float y) const;
    void GoToScrollRatio(float ratio);
    void DrawReaderScrollbar(const ThemeColors& colors);
    void AddBookmark();
    void RemoveBookmark(int bookmarkIndex);
    void RunSearch();
    void ApplySettings();
    void ChangePanel(PanelMode next);
    // 自绘输入框的输入法（IME）定位：让组合窗口与候选窗口跟随插入符。
    bool FocusedInputCaret(D2D1_POINT_2F& caret, float& lineHeight);
    void UpdateImePosition();
    // 自绘输入框的编辑能力：选区、全选、删除、复制、粘贴与光标移动。
    std::wstring* ActiveInputText();
    D2D1_RECT_F ActiveInputTextRect() const;
    IDWriteTextFormat* ActiveInputFormat() const;
    size_t ActiveInputLimit() const;
    size_t InputCaretIndexAt(float x, float y);
    void ResetInputCaretToEnd();
    void InsertInputText(const std::wstring& value);
    void DeleteInputBackward();
    void DeleteInputForward();
    void SelectInputAll();
    bool CopyInputSelection();
    bool CutInputSelection();
    void PasteIntoInput();
    void MoveInputCaret(int direction, bool extend);
    bool HandleInputKey(WPARAM key, bool controlPressed, bool shiftPressed);
    void BeginInputSelection(float x, float y);
    void UpdateInputSelection(float x, float y);
    void DrawInputSelection(const D2D1_RECT_F& textRect, IDWriteTextFormat* format, bool wrap,
                            const ThemeColors& colors);
    size_t SelectionBegin() const;
    size_t SelectionEnd() const;
    bool HasInputSelection() const;
    void StartCaretBlink();
    void StopCaretBlink();
    D2D1_RECT_F SearchBoxRect() const;
    D2D1_RECT_F NoteInputBoxRect() const;
    D2D1_RECT_F ReaderPanelContentRect() const;
    // 侧栏宽度：阅读助手面板可由分割条调整，其余面板固定。
    float PanelWidth() const;
    D2D1_RECT_F ChatFieldRect(ChatFocus field) const;
    D2D1_RECT_F ChatConversationRect() const;
    D2D1_RECT_F ChatScrollbarTrackRect() const;
    // 按鼠标纵坐标把对话内容滚动到对应比例。
    void ScrollChatFromY(float y);
    // 目录与书签列表的滚动条：轨道、绘制与拖动定位。
    D2D1_RECT_F PanelScrollbarTrackRect() const;
    void DrawPanelScrollbar(const ThemeColors& colors);
    void ScrollPanelFromY(float y);
    D2D1_RECT_F ChatStatusRect() const;
    D2D1_RECT_F ChatSendRect() const;
    D2D1_RECT_F ChatStopRect() const;
    std::wstring* ActiveChatField();
    // AI 对话：OpenAI 兼容接口的流式请求。
    void SendAiMessage();
    void StartAiRequest(const std::wstring& prompt);
    // 用已有会话历史发起一次请求（不追加用户消息），供自动读取章节后追问。
    void LaunchAiRequest(std::vector<std::pair<std::wstring, std::wstring>> history);
    // 阅读助手的系统提示：身份、书籍信息、当前阅读位置与可用控制命令。
    std::wstring BuildAiSystemPrompt() const;
    // 解析回复中的控制命令并执行，cleaned 为去掉命令后的展示文本。
    struct AiReadRange {
        bool valid = false;
        uint32_t start = 0;
        uint32_t length = 0;
    };
    void ProcessAiCommands(const std::wstring& reply, std::wstring& cleaned,
                           AiReadRange& readRange);
    void ExecuteAiCommand(const std::wstring& body, AiReadRange& readRange);
    void AddAiHighlight(uint32_t globalStart, uint32_t length, const std::wstring& color);
    void AddAiNote(uint32_t globalStart, uint32_t length, const std::wstring& text);
    void StartAiFollowUp(uint32_t start, uint32_t length);
    void StopAiRequest();
    // 连续对话：按历史消息（角色, 内容）构造请求，保序发送。
    void RunAiRequest(std::wstring url, std::wstring apiKey, std::wstring model,
                      std::wstring protocol, int thinkingLevel, std::wstring systemPrompt,
                      std::vector<std::pair<std::wstring, std::wstring>> history, HWND target);
    void PushAiChunk(const std::wstring& text, HWND target);
    // 思考内容增量（reasoning_content / thinking_delta）。
    void PushAiThinking(const std::wstring& text, HWND target);
    void PushAiDone(const std::wstring& status, HWND target);
    void AppendAiChunks();
    // 把当前流式回复作为 assistant 消息写入会话并保存。
    void CommitAiAnswer(bool allowFollowUp);
    void BeginSelection(float x, float y);
    void UpdateSelection(float x, float y);
    void ClearSelection();
    void CopySelection();
    void ApplyHighlight(const std::wstring& color);
    void DeleteHighlight(int index);
    void DeleteNote(int index);
    void OpenNoteEditor();
    void SaveNote();
    void CancelNoteEditor();
    uint32_t DisplayOffsetAt(float x, float y) const;
    bool EnsurePageLayout();
    void InvalidatePageCache();
    float LineTopForOffset(IDWriteTextLayout* layout, uint32_t offset) const;
    float FirstLineHeight(IDWriteTextLayout* layout) const;
    ID2D1Bitmap* CoverBitmap(const data::BookRecord& book);
    ID2D1Bitmap* ImageBitmap(const std::wstring& relativePath);
    void ApplySearchHighlight(IDWriteTextLayout* layout, const std::wstring& pageText,
                              const ThemeColors& colors);

    void ComputeTextMetrics(float& textWidth, float& textHeight, D2D1_RECT_F& textRect) const;

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    int showCommand_ = SW_SHOWNORMAL;
    bool comInitialized_ = false;
    RenderContext renderer_;
    data::Library library_;

    ViewMode view_ = ViewMode::Shelf;
    PanelMode panel_ = PanelMode::None;

    float widthDip_ = 1180.0f;
    float heightDip_ = 780.0f;
    float mouseX_ = -1.0f;
    float mouseY_ = -1.0f;
    bool mouseInside_ = false;
    bool mouseTracking_ = false;
    bool resizing_ = false;         // 正在拖动窗口边框调整尺寸
    bool relayoutPending_ = false;  // 拖动期间尺寸已变化，待结束拖动后统一重排

    std::vector<size_t> shelfOrder_;
    float shelfScroll_ = 0.0f;
    float shelfScrollMax_ = 0.0f;

    std::wstring activeBookId_;
    umd::Book activeBook_;
    std::wstring activeChapterRaw_;
    ChapterDisplay chapterDisplay_;
    ChapterPager pager_;
    // activeChapterRaw_ 对应的章节序号，避免同一章节反复截取整段正文。
    int activeChapterRawChapter_ = -1;
    // 当前 chapterDisplay_ / pager_ 对应的章节与排版尺寸；三者一致时无需重建。
    int layoutChapter_ = -1;
    float layoutWidth_ = 0.0f;
    float layoutHeight_ = 0.0f;
    std::vector<ChapterLayoutCacheEntry> layoutCache_;
    int activeChapter_ = 0;
    size_t activePage_ = 0;

    Gesture gesture_ = Gesture::Default;
    bool gestureBarVisible_ = false;
    bool colorPickerVisible_ = false;
    bool selecting_ = false;
    bool selectionActive_ = false;
    uint32_t selectionAnchorDisplay_ = 0;
    uint32_t selectionStartDisplay_ = 0;
    uint32_t selectionEndDisplay_ = 0;

    std::wstring noteDraft_;
    bool noteFocused_ = false;
    uint32_t pendingNoteStart_ = 0;
    uint32_t pendingNoteLength_ = 0;
    std::wstring pendingNotePreview_;

    ComPtr<IDWriteTextLayout> pageLayout_;
    uint32_t pageLayoutStart_ = 0;
    uint32_t pageLayoutEnd_ = 0;
    uint32_t lineOffset_ = 0;       // 行滚动模式视口顶部对应的显示文本偏移
    bool scrollDragging_ = false;   // 正在拖动阅读进度滑块
    float lastScrollRatio_ = 0.0f;  // 滑块拖动时的上一次比例，用于节流
    D2D1_RECT_F pageLayoutRect_{};
    bool pageLayoutValid_ = false;
    std::vector<AnnotationHit> annotationHits_;

    float panelScroll_ = 0.0f;
    float panelScrollMax_ = 0.0f;
    bool panelScrollDragging_ = false;
    std::wstring searchQuery_;
    bool searchFocused_ = false;
    std::vector<SearchResult> searchResults_;
    std::wstring searchStatus_;
    std::wstring searchHighlight_;

    std::vector<HitRegion> hitRegions_;
    std::vector<D2D1_RECT_F> clipStack_;
    int hoverIndex_ = -1;
    // 弹出菜单状态：条目、弹出位置与可见性。
    std::vector<MenuEntry> menuEntries_;
    D2D1_POINT_2F menuOrigin_{};
    bool menuVisible_ = false;
    std::unordered_map<std::wstring, ComPtr<ID2D1Bitmap>> coverCache_;
    // 系统可用中文字族，供阅读设置的字体列表使用。
    std::vector<FontFamilyName> fontFamilies_;
    // 背景目录中的图片文件名与缩略图缓存，以及当前背景大图。
    std::vector<std::wstring> backgroundFiles_;
    std::unordered_map<std::wstring, BackgroundThumb> backgroundThumbCache_;
    ComPtr<ID2D1Bitmap> backgroundBitmap_;
    std::wstring backgroundBitmapName_;

    std::wstring statusText_;
    int64_t statusExpiry_ = 0;
    bool copyToastVisible_ = false;
    int64_t copyToastExpiry_ = 0;
    bool caretBlinkVisible_ = true;  // 输入框插入符当前是否可见
    size_t inputCaret_ = 0;          // 当前输入框插入符位置（码元索引）
    size_t inputAnchor_ = 0;         // 选区锚点
    bool inputSelecting_ = false;    // 正在拖动选择
    bool shelfSettingsVisible_ = false;  // 书架设置浮层（文件关联）
    bool categoryManagerVisible_ = false;  // 分类管理浮层
    std::wstring shelfFilterCategory_;   // 空=全部；__none__=未分类；否则分类 id
    std::wstring categoryDraft_;         // 分类名称输入
    bool categoryNameFocused_ = false;
    std::wstring shelfCategoryPickerBookId_;  // 非空表示正在为该书籍选择分类
    bool shelfSearchFocused_ = false;         // 书架正则搜索框是否获得焦点
    std::wstring shelfSearchQuery_;           // 书架正则搜索输入内容
    ComPtr<ISpVoice> voice_;         // 语音合成引擎
    bool speaking_ = false;          // 是否正在朗读

    ChatFocus chatFocus_ = ChatFocus::None;
    bool aiSettingsVisible_ = false;  // AI 接口设置弹框是否打开
    // 字体选择弹框状态与其列表滚动量。
    bool fontPickerVisible_ = false;
    float fontPickerScroll_ = 0.0f;
    float fontPickerScrollMax_ = 0.0f;
    // 快捷键映射窗口状态：是否打开、正在等待按键的行。
    bool shortcutSettingsVisible_ = false;
    int shortcutCaptureIndex_ = -1;
    std::wstring aiDraft_;           // 问题输入框内容
    std::wstring aiAnswer_;          // 当前流式回复（完成后写入会话）
    std::wstring aiThinking_;        // 当前流式思考内容（完成后写入会话）
    std::wstring aiThinkingPending_; // 后台线程累积的思考增量
    std::wstring aiStatus_;          // 请求状态提示
    // 当前会话标识，以及对话内容区的滚动量。
    std::wstring activeChatSessionId_;
    float chatScroll_ = 0.0f;
    float chatScrollMax_ = 0.0f;
    // 对话内容区滚动条拖动，以及阅读助手面板宽度调整。
    bool chatScrollDragging_ = false;
    float chatPanelWidth_ = 372.0f;
    bool chatPanelResizing_ = false;
    // 对话气泡内的文字选择：消息下标与选区起止（消息文本内偏移）。
    int chatSelectionMessage_ = -1;
    uint32_t chatSelectionStart_ = 0;
    uint32_t chatSelectionEnd_ = 0;
    bool chatSelecting_ = false;
    // 已展开思考内容的消息下标；流式回复以小标 -1 记录。
    std::unordered_set<int> chatThinkingExpanded_;
    // 对话记录弹框状态与其列表滚动量。
    bool chatSessionPickerVisible_ = false;
    float chatSessionPickerScroll_ = 0.0f;
    float chatSessionPickerScrollMax_ = 0.0f;
    bool aiStreaming_ = false;
    // 每轮用户提问自动读取正文的轮数，达到上限后停止，避免命令循环。
    int aiFollowUpCount_ = 0;
    // 最近一次自动读取正文区间的结束偏移，用于模型只表达读取意向时的续读起点。
    uint32_t aiLastReadEnd_ = 0;
    std::thread aiThread_;
    std::mutex aiMutex_;
    std::wstring aiPendingText_;
    std::wstring aiPendingStatus_;
    bool aiPendingDone_ = false;
    std::atomic<bool> aiCancel_{false};
    std::wstring startupNotice_;  // 数据目录回退或索引重建等非致命提示，首帧展示
    std::vector<std::wstring> startupPaths_;  // 命令行传入的文件路径
    HANDLE instanceMutex_ = nullptr;          // 单实例互斥体
    bool exitImmediately_ = false;            // 已把文件转交给现有实例，无需创建窗口
};

}  // namespace ui
