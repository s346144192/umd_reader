#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <vector>

#include "app_identity.h"
#include "data/library.h"
#include "ui/render_context.h"
#include "umd/umd_parser.h"

namespace ui {
namespace detail {

// 窗口与整体布局常量。窗口类、标题、互斥体与 ProgID 统一在 app_identity.h 中定义。
inline constexpr ULONG_PTR kCopyDataOpenFile = 0x554D4431;  // 'UMD1'，标识转交文件路径的消息
inline constexpr float kShelfToolbarHeight = 88.0f;
inline constexpr float kReaderToolbarHeight = 64.0f;
inline constexpr float kReaderFooterHeight = 44.0f;
inline constexpr float kPanelWidth = 372.0f;
inline constexpr float kPanelHeaderHeight = 56.0f;
inline constexpr float kShelfCardWidth = 204.0f;
inline constexpr float kShelfCardHeight = 392.0f;
inline constexpr float kShelfCardGapX = 28.0f;
inline constexpr float kShelfCardGapY = 34.0f;
inline constexpr float kShelfStartY = 128.0f;
inline constexpr float kShelfMargin = 44.0f;

// 字体与主题预设。
struct FontOption {
    const wchar_t* label;
    const wchar_t* family;
};

inline constexpr FontOption kFontOptions[] = {
    {L"微软雅黑", L"Microsoft YaHei"},
    {L"宋体", L"SimSun"},
    {L"黑体", L"SimHei"},
    {L"楷体", L"KaiTi"},
};
inline constexpr size_t kFontOptionCount = sizeof(kFontOptions) / sizeof(kFontOptions[0]);

struct ThemeOption {
    const wchar_t* label;
    const wchar_t* name;
};

inline constexpr ThemeOption kThemeOptions[] = {
    {L"浅色", L"light"},
    {L"护眼", L"sepia"},
    {L"深色", L"dark"},
};
inline constexpr size_t kThemeOptionCount = sizeof(kThemeOptions) / sizeof(kThemeOptions[0]);

// 阅读区纯色背景预设，均为浅色，保证与主题文字颜色形成足够对比。
inline constexpr const wchar_t* kBackgroundColors[] = {
    L"#FFFFFF", L"#F7F3E8", L"#EAF4EA", L"#E8F1F8", L"#FFF9E6", L"#F7EFF7", L"#EDEDED",
};
inline constexpr size_t kBackgroundColorCount =
    sizeof(kBackgroundColors) / sizeof(kBackgroundColors[0]);

// 比例小字统一以宽度 16 为基准换算高度。
inline constexpr float kViewRatioBaseWidth = 16.0f;
// 背景缩略图的解码上限（像素），限制列表显示的显存占用。
inline constexpr float kBackgroundThumbMaxWidth = 320.0f;
inline constexpr float kBackgroundThumbMaxHeight = 220.0f;

inline constexpr int kMaxNoteChars = 255;
// 行滚动模式下每次布局的文本跨度。按整页可见行数的数倍取值，
// 既保证填满视口，又避免为每次重绘创建超大的文本布局。
inline constexpr uint32_t kLinePreviewChars = 4000;
inline constexpr float kGestureBarHeight = 44.0f;
inline constexpr float kGestureButtonWidth = 84.0f;
inline constexpr float kGestureButtonHeight = 30.0f;
inline constexpr float kGestureButtonGap = 6.0f;
inline constexpr float kGestureBarPadding = 6.0f;
inline constexpr wchar_t kUncategorizedFilter[] = L"__none__";
inline constexpr float kCategorySidebarWidth = 220.0f;
inline constexpr UINT_PTR kStatusTimerId = 1;
inline constexpr UINT_PTR kCopyToastTimerId = 2;
inline constexpr UINT_PTR kCaretTimerId = 3;
// 阅读计时器：每秒检查一次，仅在阅读页前台阅读时累计时长。
inline constexpr UINT_PTR kReadingTimerId = 4;
inline constexpr UINT kReadingTimerIntervalMs = 1000;
// 单次计时增量的上限，避免系统休眠或长时间消息延迟造成异常的时长累计。
inline constexpr uint64_t kReadingMaxTickMs = 2000;
// 阅读统计落盘间隔（秒）。
inline constexpr uint64_t kReadingSaveIntervalSeconds = 300;
// PDF 缩放比例落盘延迟：连续缩放时合并写入，停止缩放后写入一次。
inline constexpr UINT_PTR kPdfZoomTimerId = 5;
inline constexpr UINT kPdfZoomSaveDelayMs = 400;
// 书架右上角等级芯片尺寸。
inline constexpr float kReadingChipWidth = 124.0f;
inline constexpr float kReadingChipHeight = 40.0f;
inline constexpr UINT kCaretBlinkIntervalMs = 500;
inline constexpr UINT kAiStreamMessage = WM_APP + 1;  // 后台线程通知主线程追加流式内容
inline constexpr UINT kAiTaskMessage = WM_APP + 2;    // AI 语音/图像/视频任务完成通知
// 局域网分享默认监听端口与后台网络任务完成通知。
inline constexpr uint16_t kLanSharePort = 8712;
inline constexpr UINT kLanJobMessage = WM_APP + 3;
inline constexpr int64_t kCopyToastSeconds = 3;
inline constexpr float kCopyToastWidth = 108.0f;
inline constexpr float kCopyToastHeight = 36.0f;
inline constexpr float kCopyToastBottomGap = 18.0f;
inline constexpr float kSearchBoxHeight = 42.0f;
inline constexpr float kSearchTextPaddingX = 14.0f;
inline constexpr float kSearchTextPaddingTop = 11.0f;
inline constexpr float kSearchTextPaddingBottom = 8.0f;
inline constexpr float kNoteInputHeight = 150.0f;
inline constexpr float kNoteInputPaddingX = 12.0f;
inline constexpr float kNoteInputPaddingY = 10.0f;
// 对话气泡排版参数：角色行高、内边距、间距与最大宽度占比。
inline constexpr float kChatBubblePadding = 10.0f;
inline constexpr float kChatBubbleHeader = 20.0f;
inline constexpr float kChatBubbleGap = 8.0f;
inline constexpr float kChatBubbleWidthRatio = 0.86f;
// 弹出菜单尺寸。
inline constexpr float kMenuWidth = 186.0f;
inline constexpr float kMenuItemHeight = 32.0f;
inline constexpr float kMenuPadding = 6.0f;

// 比例数值：小数部分为 0 时取整，否则保留 1 位小数。
std::wstring FormatRatioValue(float value);
// 图片比例小字：较长边固定为 16，短边按实际比例换算。
std::wstring ImageRatioLabel(uint32_t width, uint32_t height);
// 阅读视图比例：以宽度 16 为基准换算高度。
std::wstring ViewRatioLabel(float width, float height);

// 返回平移后的矩形，用于把相对排版坐标换算为窗口坐标。
D2D1_RECT_F OffsetRect(const D2D1_RECT_F& rect, float dx, float dy);

// 快捷键解析与组合键生成。
struct ShortcutSpec {
    bool ctrl = false;
    bool shift = false;
    bool alt = false;
    std::wstring key;
};

std::wstring KeyNameFromVk(UINT vk);
UINT VkFromKeyName(const std::wstring& name);
// 解析 Ctrl+Shift+X 形式的组合键描述。
ShortcutSpec ParseShortcut(const std::wstring& combo);
// 用当前按下的键与修饰键生成组合键描述。
std::wstring ShortcutFromKey(UINT vk);
// 判断当前按键是否匹配组合键描述。
bool ShortcutMatches(const std::wstring& combo, UINT vk);
// 读取指定动作的组合键；未配置时使用默认值。
std::wstring ShortcutCombo(const data::ReaderSettings& settings, const wchar_t* action,
                           const wchar_t* fallback);

struct ShortcutRow {
    const wchar_t* action;
    const wchar_t* label;
};

inline constexpr ShortcutRow kShortcutRows[] = {
    {data::kShortcutPrevPage, L"上一页"},
    {data::kShortcutNextPage, L"下一页"},
    {data::kShortcutSearch, L"全文检索"},
    {data::kShortcutBookmark, L"添加书签"},
    {data::kShortcutCopy, L"复制选中文字"},
    {data::kShortcutChatSend, L"发送对话"},
};
inline constexpr size_t kShortcutRowCount = sizeof(kShortcutRows) / sizeof(kShortcutRows[0]);

inline constexpr const wchar_t* kHighlightColors[16] = {
    L"#FFE082", L"#FFCC80", L"#FFAB91", L"#EF9A9A", L"#F48FB1", L"#CE93D8",
    L"#B39DDB", L"#9FA8DA", L"#90CAF9", L"#81D4FA", L"#80DEEA", L"#80CBC4",
    L"#A5D6A7", L"#C5E1A5", L"#E6EE9C", L"#D7CCC8",
};

D2D1_COLOR_F ParseHexColor(const std::wstring& hex, float alpha);
std::wstring TrimNoteText(const std::wstring& text);

// 命令文本归一化：全角冒号、逗号、连字符、波浪号与数字转为半角。
// 逐字符替换，长度保持不变，便于用同一位置在原文本中切分标注文字。
std::wstring NormalizeCommandText(const std::wstring& text);
// 提取文本中的第一个连续整数，找不到时返回 false。
bool ExtractFirstNumber(const std::wstring& text, uint32_t& value);

D2D1_COLOR_F NoteTextColor();
D2D1_COLOR_F NoteFrameColor();
D2D1_COLOR_F Darken(const D2D1_COLOR_F& color, float factor);
D2D1_SIZE_F ImageDisplaySize(ID2D1Bitmap* bitmap, float maxWidth, float maxHeight);
bool PointInside(const D2D1_RECT_F& rect, float x, float y);
D2D1_RECT_F IntersectRect(const D2D1_RECT_F& a, const D2D1_RECT_F& b);
std::wstring LowerCase(std::wstring text);
// 把换行与制表符压缩为单个空格，便于单行展示。
std::wstring SanitizeLine(const std::wstring& text);
std::wstring ChapterTitleAt(const umd::Book& book, int index);
void RemoveLastCodeUnit(std::wstring& text);
std::wstring FormatCountLine(size_t bookCount);

// 计算指定插入位置的底部坐标（DIP）与行高，供 IME 窗口与光标绘制使用。
D2D1_POINT_2F CaretPointAt(const std::wstring& text, IDWriteTextFormat* format,
                           const D2D1_RECT_F& textRect, IDWriteFactory* factory, bool wrap,
                           size_t requestedIndex, float fallbackLineHeight, float& lineHeight);
// 把点击坐标换算为插入位置（码元索引）。
size_t CaretIndexFromPoint(const std::wstring& text, IDWriteTextFormat* format, bool wrap,
                           const D2D1_RECT_F& textRect, IDWriteFactory* factory, float x, float y);

bool WriteClipboardText(const std::wstring& text);
bool ReadClipboardText(std::wstring& text);
bool ReadRegistryString(HKEY root, const std::wstring& subKey, const wchar_t* valueName,
                        std::wstring& value);
bool WriteRegistryString(HKEY root, const std::wstring& subKey, const wchar_t* valueName,
                         const std::wstring& value);
std::wstring ExecutablePath();
// 判断扩展名当前是否已指向本程序的 ProgID（当前用户级）。
bool IsFileExtensionAssociated(const std::wstring& extension, const std::wstring& progId);
// 为单个扩展名注册当前用户级文件关联。
bool RegisterFileAssociation(const std::wstring& extension, const std::wstring& progId,
                             const std::wstring& description, const std::wstring& executable);
// 移除单个扩展名的当前用户级文件关联；只清理本程序写入的 ProgID。
bool RemoveFileAssociation(const std::wstring& extension, const std::wstring& progId);

// 分类图标：文件夹、网格、收件托盘与滑杆。
void DrawFolderIcon(RenderContext& renderer, float left, float top, const D2D1_COLOR_F& color);
void DrawGridIcon(RenderContext& renderer, float left, float top, const D2D1_COLOR_F& color);
void DrawInboxIcon(RenderContext& renderer, float left, float top, const D2D1_COLOR_F& color);
void DrawSlidersIcon(RenderContext& renderer, float left, float top, const D2D1_COLOR_F& color);
// PDF 页面视图图标：单页或左右双页，centerX / centerY 为图标中心。
void DrawPdfPageLayoutIcon(RenderContext& renderer, float centerX, float centerY,
                           bool twoPageLayout, const D2D1_COLOR_F& color);
// PDF 页面缩放图标：zoomIn 为 true 时镜片内显示加号，否则显示减号。
void DrawPdfZoomIcon(RenderContext& renderer, float centerX, float centerY, bool zoomIn,
                     const D2D1_COLOR_F& color);

                     

}  // namespace detail
}  // namespace ui
