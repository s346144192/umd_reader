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
#include <fstream>
#include <iterator>
#include <regex>
#include <vector>

#include "core/text_util.h"
#include "nlohmann/json.hpp"
#include "ui/ui_internal.h"

namespace ui {
using namespace detail;


Application::Application() = default;

Application::~Application() {
    // 先结束后台请求线程，避免其继续访问已析构的成员。
    lanCancel_ = true;
    if (lanThread_.joinable()) {
        lanThread_.join();
    }
    if (lanServer_) {
        lanServer_->Stop();
    }
    aiCancel_ = true;
    if (aiThread_.joinable()) {
        aiThread_.join();
    }
    aiSpeechCancel_ = true;
    aiGenerateCancel_ = true;
    if (aiSpeechThread_.joinable()) {
        aiSpeechThread_.join();
    }
    if (aiGenerateThread_.joinable()) {
        aiGenerateThread_.join();
    }
    PlaySoundW(nullptr, nullptr, 0);
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
    instanceMutex_ = CreateMutexW(nullptr, TRUE, app_id::kInstanceMutexName);
    if (instanceMutex_ != nullptr && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (ForwardToExistingInstance()) {
            exitImmediately_ = true;
            return true;
        }
    }

    std::wstring error;
    if (!library_.Initialize(error)) {
        MessageBoxW(nullptr, error.c_str(), app_id::kWindowTitle, MB_ICONERROR);
        return false;
    }
    // Initialize 成功时 error 可能携带非致命提示（数据目录回退、索引重建等）。
    startupNotice_ = error;
    // 阅读记录按月份独立存放，每月一个文件，避免单文件持续增长。
    readingLog_.Initialize(library_.DataDirectory() / L"reading_logs");

    // 诊断日志：记录启动时加载的 AI 配置与当前选择。
    {
        std::ofstream log(library_.DataDirectory() / "ai_request_debug.log", std::ios::app);
        if (log) {
            const data::ReaderSettings& settings = library_.Settings();
            log << util::ToUtf8(util::FormatDateTime(util::CurrentUnixSeconds()))
                << " startup active=" << util::ToUtf8(settings.activeAiChatId)
                << " count=" << settings.aiChatConfigs.size();
            for (const data::AiChatConfig& config : settings.aiChatConfigs) {
                log << " [" << util::ToUtf8(config.id) << "|" << util::ToUtf8(config.name) << "|"
                    << util::ToUtf8(config.model) << "|" << util::ToUtf8(config.baseUrl) << "]";
            }
            log << "\n";
        }
    }

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance_;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = app_id::kWindowClass;
    RegisterClassExW(&windowClass);

    const data::ReaderSettings& settings = library_.Settings();
    RECT frameRect{0, 0, settings.windowWidth, settings.windowHeight};
    AdjustWindowRectExForDpi(&frameRect, WS_OVERLAPPEDWINDOW, FALSE, 0, GetDpiForSystem());
    const int windowWidth = frameRect.right - frameRect.left;
    const int windowHeight = frameRect.bottom - frameRect.top;

    hwnd_ = CreateWindowExW(0, app_id::kWindowClass, app_id::kWindowTitle, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                            CW_USEDEFAULT, windowWidth, windowHeight, nullptr, nullptr, instance_,
                            this);
    if (!hwnd_) {
        MessageBoxW(nullptr, L"创建主窗口失败。", app_id::kWindowTitle, MB_ICONERROR);
        return false;
    }
    if (!renderer_.WriteFactory()) {
        MessageBoxW(nullptr, L"Direct2D 与 DirectWrite 初始化失败。", app_id::kWindowTitle, MB_ICONERROR);
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
    case WM_MBUTTONDOWN:
        OnMiddleButtonDown(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;
    case WM_MBUTTONUP:
        EndPdfPan();
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
    case kAiTaskMessage:
        OnAiTaskMessage(static_cast<UINT>(wParam));
        return 0;
    case kLanJobMessage:
        OnLanJobDone();
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
        } else if (wParam == kReadingTimerId) {
            TickReading();
        } else if (wParam == kPdfZoomTimerId) {
            // 缩放停止后写入书籍数据，记录只属于当前书籍的缩放比例。
            KillTimer(hwnd, kPdfZoomTimerId);
            if (!activeBookId_.empty()) {
                std::wstring error;
                library_.SaveBook(activeBookId_, error);
            }
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
    // 阅读计时器每秒检查一次，仅在阅读页前台阅读时累计时长。
    readingTickMs_ = GetTickCount64();
    SetTimer(hwnd_, kReadingTimerId, kReadingTimerIntervalMs, nullptr);
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
        // 阅读成就浮层为模态界面，绘制与命中最上层。
        if (readingAchievementsVisible_) {
            DrawReadingAchievements(colors);
        }
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
        const float width = std::clamp(widthDip_ - mouseX_, minimum, maximum);
        // 每个可调宽面板各自记忆宽度，切换面板时保持上次的尺寸。
        if (panel_ == PanelMode::Toc) {
            tocPanelWidth_ = width;
        } else if (panel_ == PanelMode::Bookmarks) {
            bookmarksPanelWidth_ = width;
        } else {
            chatPanelWidth_ = width;
        }
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
    if (pdfPanning_) {
        UpdatePdfPan(mouseX_, mouseY_);
        return;
    }
    if (pdfSelecting_) {
        UpdatePdfSelection(mouseX_, mouseY_);
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

    // 指向可调宽面板的左边缘时显示横向调整光标。
    if (panel_ == PanelMode::Chat || panel_ == PanelMode::Toc ||
        panel_ == PanelMode::Bookmarks) {
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
    // 阅读成就浮层为模态界面：点击浮层外关闭，浮层内只响应自身控件。
    if (readingAchievementsVisible_) {
        if (!PointInside(ReadingAchievementsPanelRect(), x, y)) {
            CloseReadingAchievements();
            return;
        }
        if (hit != nullptr) {
            if (hit->kind == HitKind::ReadingAchievementsClose) {
                CloseReadingAchievements();
            } else if (hit->kind == HitKind::ReadingCalendarPrev) {
                readingCalendarMonth_ -= 1;
                if (readingCalendarMonth_ < 1) {
                    readingCalendarMonth_ = 12;
                    readingCalendarYear_ -= 1;
                }
                InvalidateRect(hwnd_, nullptr, FALSE);
            } else if (hit->kind == HitKind::ReadingCalendarNext) {
                readingCalendarMonth_ += 1;
                if (readingCalendarMonth_ > 12) {
                    readingCalendarMonth_ = 1;
                    readingCalendarYear_ += 1;
                }
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
        }
        return;
    }
    // AI 生成弹框位于最上层。
    if (aiGenerateVisible_) {
        if (!PointInside(AiGeneratePanelRect(), x, y)) {
            CloseAiGenerate();
            return;
        }
        const auto isGenerateControl = [](HitKind kind) {
            switch (kind) {
            case HitKind::AiGenerateClose:
            case HitKind::AiGenerateTabImage:
            case HitKind::AiGenerateTabVideo:
            case HitKind::AiGeneratePrompt:
            case HitKind::AiGenerateRun:
            case HitKind::AiGenerateOpenResult:
                return true;
            default:
                return false;
            }
        };
        if (!hit || !isGenerateControl(hit->kind)) {
            return;
        }
    }
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
        if (!PointInside(AiSettingsPanelRect(), x, y)) {
            CloseAiSettings();
            return;
        }
        const auto isSettingsControl = [](HitKind kind) {
            switch (kind) {
            case HitKind::ChatBaseUrl:
            case HitKind::ChatApiKey:
            case HitKind::ChatModel:
            case HitKind::ChatContextTokens:
            case HitKind::ChatProtocolOpenAi:
            case HitKind::ChatProtocolAnthropic:
            case HitKind::AiThinkingLevel:
            case HitKind::AiSettingsSave:
            case HitKind::AiSettingsClose:
            case HitKind::AiTabChat:
            case HitKind::AiTabSpeech:
            case HitKind::AiTabImage:
            case HitKind::AiTabVideo:
            case HitKind::AiConfigPrev:
            case HitKind::AiConfigNext:
            case HitKind::AiConfigAdd:
            case HitKind::AiConfigDelete:
            case HitKind::AiConfigName:
            case HitKind::AiSpeechEngineBuiltin:
            case HitKind::AiSpeechEngineAi:
            case HitKind::AiSpeechBaseUrl:
            case HitKind::AiSpeechApiKey:
            case HitKind::AiSpeechModel:
            case HitKind::AiSpeechVoice:
            case HitKind::AiImageBaseUrl:
            case HitKind::AiImageApiKey:
            case HitKind::AiImageModel:
            case HitKind::AiVideoBaseUrl:
            case HitKind::AiVideoApiKey:
            case HitKind::AiVideoModel:
                return true;
            default:
                return false;
            }
        };
        if (!hit || !isSettingsControl(hit->kind)) {
            return;
        }
    }
    // 局域网分享浮层：点击浮层外关闭，浮层内只响应自身控件。
    if (lanShareVisible_) {
        if (!PointInside(LanSharePanelRect(), x, y)) {
            CloseLanShare();
            return;
        }
        if (hit == nullptr) {
            if (lanAddressFocused_) {
                lanAddressFocused_ = false;
                StopCaretBlink();
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return;
        }
        switch (hit->kind) {
        case HitKind::LanShareClose:
            CloseLanShare();
            return;
        case HitKind::LanShareToggle:
            ToggleLanSharing();
            return;
        case HitKind::LanConnect:
            lanAddressFocused_ = false;
            StopCaretBlink();
            StartLanFetch();
            return;
        case HitKind::LanScan:
            StartLanScan();
            return;
        case HitKind::LanConnectHost:
            if (hit->index >= 0 &&
                hit->index < static_cast<int>(lanDiscoveredHosts_.size())) {
                lanAddressDraft_ = lanDiscoveredHosts_[static_cast<size_t>(hit->index)];
                lanAddressFocused_ = false;
                StopCaretBlink();
                StartLanFetch();
            }
            return;
        case HitKind::LanDownload:
            StartLanDownload(hit->index);
            return;
        case HitKind::LanAddressInput:
            lanAddressFocused_ = true;
            StartCaretBlink();
            UpdateImePosition();
            BeginInputSelection(x, y);
            return;
        default:
            if (lanAddressFocused_) {
                lanAddressFocused_ = false;
                StopCaretBlink();
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
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
            // PDF 页面：放大后按住空格拖动为平移；文字层已就绪时，在空白处按下也平移；
            // 其余位置按下开始文字选择。
            if (fixedPages_.IsOpen()) {
                const bool spaceHeld = (GetKeyState(VK_SPACE) & 0x8000) != 0;
                if (fixedPages_.IsZoomed()) {
                    if (spaceHeld && BeginPdfPan(x, y)) {
                        return;
                    }
                    if (fixedPages_.HasTextLayer() && !fixedPages_.HasTextAt(x, y, 6.0f) &&
                        BeginPdfPan(x, y)) {
                        return;
                    }
                }
                if (BeginPdfSelection(x, y)) {
                    return;
                }
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
            if (IsFileExtensionAssociated(L".umd", app_id::kDocumentProgId)) {
                DisassociateFileExtension(L".umd", app_id::kDocumentProgId);
            } else {
                AssociateFileExtension(L".umd", app_id::kDocumentProgId, L"UMD 电子书");
            }
            return;
        }
        if (action.kind == HitKind::ToggleTxtAssociation) {
            if (IsFileExtensionAssociated(L".txt", app_id::kTextProgId)) {
                DisassociateFileExtension(L".txt", app_id::kTextProgId);
            } else {
                AssociateFileExtension(L".txt", app_id::kTextProgId, L"文本电子书");
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
    case HitKind::OpenLanShare:
        OpenLanShare();
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
                // 分享开启期间移除书籍后同步更新对外书单。
                if (lanServer_) {
                    RefreshLanShareBooks();
                }
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
        const umd::Chapter& chapter = activeBook_.chapters[static_cast<size_t>(action.index)];
        // PDF 按原版页面阅读：目录条目直接跳到目标页。
        if (fixedPages_.IsOpen()) {
            if (chapter.pageIndex >= static_cast<uint32_t>(fixedPages_.PageCount())) {
                break;
            }
            activeChapter_ = action.index;
            fixedPages_.SetCurrentPage(static_cast<int>(chapter.pageIndex));
            fixedPages_.InvalidateBitmap();
            PersistFixedPageProgress();
            InvalidateRect(hwnd_, nullptr, FALSE);
            break;
        }
        GoToGlobalOffset(chapter.charOffset);
        break;
    }
    case HitKind::ChapterFold: {
        if (action.index < 0 ||
            action.index >= static_cast<int>(activeBook_.chapters.size())) {
            break;
        }
        if (collapsedChapters_.count(action.index) > 0) {
            collapsedChapters_.erase(action.index);
        } else {
            collapsedChapters_.insert(action.index);
        }
        panelScroll_ = std::clamp(panelScroll_, 0.0f, panelScrollMax_);
        InvalidateRect(hwnd_, nullptr, FALSE);
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
    case HitKind::PdfPageLayout:
        TogglePdfPageLayout();
        break;
    case HitKind::PdfZoomIn:
        ZoomPdfPage(true);
        break;
    case HitKind::PdfZoomOut:
        ZoomPdfPage(false);
        break;
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
        data::AiChatConfig& chat = data::ActiveAiChat(settings);
        const bool toAnthropic = action.kind == HitKind::ChatProtocolAnthropic;
        const std::wstring openAiBase = L"https://api.openai.com/v1";
        const std::wstring anthropicBase = L"https://api.anthropic.com/v1";
        const std::wstring openAiModel = L"gpt-4o-mini";
        const std::wstring anthropicModel = L"claude-3-5-sonnet-20241022";
        // 仅当地址或模型仍是原协议的默认值时才替换为目标协议的默认值：
        // 自定义值不被覆盖，留空的值继续保持为空。
        if (toAnthropic) {
            if (chat.baseUrl == openAiBase) {
                chat.baseUrl = anthropicBase;
            }
            if (chat.model == openAiModel) {
                chat.model = anthropicModel;
            }
        } else {
            if (chat.baseUrl == anthropicBase) {
                chat.baseUrl = openAiBase;
            }
            if (chat.model == anthropicModel) {
                chat.model = openAiModel;
            }
        }
        chat.protocol = toAnthropic ? std::wstring(L"anthropic") : std::wstring(L"openai");
        std::wstring error;
        library_.Save(error);
        UpdateImePosition();
        InvalidateRect(hwnd_, nullptr, FALSE);
        break;
    }
    case HitKind::AiThinkingLevel:
        // 思考等级即时生效，随「保存」写入设置。
        data::ActiveAiChat(library_.Settings()).thinkingLevel = std::clamp(action.index, 0, 3);
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
    case HitKind::AiTabChat:
    case HitKind::AiTabSpeech:
    case HitKind::AiTabImage:
    case HitKind::AiTabVideo:
        aiSettingsTab_ = std::clamp(action.index, 0, 3);
        chatFocus_ = ChatFocus::None;
        StopCaretBlink();
        InvalidateRect(hwnd_, nullptr, FALSE);
        break;
    case HitKind::AiConfigPrev:
        StepAiChatConfig(-1);
        break;
    case HitKind::AiConfigNext:
        StepAiChatConfig(1);
        break;
    case HitKind::AiConfigAdd:
        AddAiChatConfig();
        break;
    case HitKind::AiConfigDelete:
        DeleteAiChatConfig();
        break;
    case HitKind::AiConfigName:
    case HitKind::ChatContextTokens:
    case HitKind::AiSpeechBaseUrl:
    case HitKind::AiSpeechApiKey:
    case HitKind::AiSpeechModel:
    case HitKind::AiSpeechVoice:
    case HitKind::AiImageBaseUrl:
    case HitKind::AiImageApiKey:
    case HitKind::AiImageModel:
    case HitKind::AiVideoBaseUrl:
    case HitKind::AiVideoApiKey:
    case HitKind::AiVideoModel: {
        ChatFocus focus = ChatFocus::None;
        switch (action.kind) {
        case HitKind::AiConfigName: focus = ChatFocus::Name; break;
        case HitKind::ChatContextTokens: focus = ChatFocus::ContextTokens; break;
        case HitKind::AiSpeechBaseUrl: focus = ChatFocus::SpeechBaseUrl; break;
        case HitKind::AiSpeechApiKey: focus = ChatFocus::SpeechApiKey; break;
        case HitKind::AiSpeechModel: focus = ChatFocus::SpeechModel; break;
        case HitKind::AiSpeechVoice: focus = ChatFocus::SpeechVoice; break;
        case HitKind::AiImageBaseUrl: focus = ChatFocus::ImageBaseUrl; break;
        case HitKind::AiImageApiKey: focus = ChatFocus::ImageApiKey; break;
        case HitKind::AiImageModel: focus = ChatFocus::ImageModel; break;
        case HitKind::AiVideoBaseUrl: focus = ChatFocus::VideoBaseUrl; break;
        case HitKind::AiVideoApiKey: focus = ChatFocus::VideoApiKey; break;
        case HitKind::AiVideoModel: focus = ChatFocus::VideoModel; break;
        default: break;
        }
        if (focus != ChatFocus::None) {
            chatFocus_ = focus;
            StartCaretBlink();
            UpdateImePosition();
            BeginInputSelection(x, y);
        }
        break;
    }
    case HitKind::AiSpeechEngineBuiltin:
    case HitKind::AiSpeechEngineAi:
        library_.Settings().speechEngine = action.kind == HitKind::AiSpeechEngineAi
                                               ? std::wstring(L"ai")
                                               : std::wstring(L"builtin");
        // 切换到内置语音时下方的 AI 配置会隐藏，同时清除输入焦点。
        chatFocus_ = ChatFocus::None;
        StopCaretBlink();
        InvalidateRect(hwnd_, nullptr, FALSE);
        break;
    case HitKind::AiGenerateOpen:
        OpenAiGenerate();
        break;
    case HitKind::AiGenerateClose:
        CloseAiGenerate();
        break;
    case HitKind::AiGenerateTabImage:
    case HitKind::AiGenerateTabVideo:
        aiGenerateTab_ = action.kind == HitKind::AiGenerateTabVideo ? 1 : 0;
        aiGenerateStatus_.clear();
        aiGenerateResult_.clear();
        aiGeneratePreview_.Reset();
        aiGeneratePreviewPath_.clear();
        InvalidateRect(hwnd_, nullptr, FALSE);
        break;
    case HitKind::AiGeneratePrompt:
        chatFocus_ = ChatFocus::GeneratePrompt;
        StartCaretBlink();
        UpdateImePosition();
        BeginInputSelection(x, y);
        break;
    case HitKind::AiGenerateRun:
        StartAiGenerate();
        break;
    case HitKind::AiGenerateOpenResult:
        if (!aiGenerateResult_.empty()) {
            ShellExecuteW(hwnd_, L"open", aiGenerateResult_.c_str(), nullptr, nullptr,
                          SW_SHOWNORMAL);
        }
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
    case HitKind::OpenReadingAchievements:
        OpenReadingAchievements();
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
    // AI 生成弹框与快捷键映射窗口打开时，滚轮不滚动底层页面。
    if (aiGenerateVisible_ || shortcutSettingsVisible_ || readingAchievementsVisible_) {
        return;
    }

    // 局域网分享浮层打开时，滚轮只滚动远端书单。
    if (lanShareVisible_) {
        if (PointInside(LanShareListRect(), x, y)) {
            lanListScroll_ = std::clamp(lanListScroll_ - steps * 60.0f, 0.0f, lanListScrollMax_);
            InvalidateRect(hwnd_, nullptr, FALSE);
            UpdateWindow(hwnd_);
        }
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
        if (fixedPages_.IsOpen()) {
            // 固定页阅读：Ctrl + 滚轮调整页面缩放。滚轮向前（远离使用者）放大，
            // 向后缩小，与常见阅读器的方向一致。
            if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) {
                if (steps > 0.0f) {
                    ZoomPdfPage(true);
                } else if (steps < 0.0f) {
                    ZoomPdfPage(false);
                }
                UpdateWindow(hwnd_);
                return;
            }
            if (fixedPages_.IsZoomed()) {
                // 已放大：滚轮平移页面，Shift + 滚轮横向平移。
                // 平移量取正表示内容向右、向下移动，因此滚轮向后滚动时页面内容向上移动，
                // 显示页面下半部分，与滚动方向一致。
                const float stepDip = steps * 80.0f;
                const bool horizontal = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                const bool moved = fixedPages_.PanBy(horizontal ? stepDip : 0.0f,
                                                    horizontal ? 0.0f : stepDip);
                if (moved) {
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    UpdateWindow(hwnd_);
                    return;
                }
                // 该方向已到平移边界时改为翻页，避免滚轮在该方向上无响应。
            }
            if (steps < 0.0f) {
                NextPage();
            } else if (steps > 0.0f) {
                PreviousPage();
            }
            return;
        }
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
    if (pdfPanning_) {
        EndPdfPan();
        return;
    }
    if (pdfSelecting_) {
        EndPdfSelection();
        return;
    }
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
    if (view_ != ViewMode::Reader ||
        (activeBook_.content.empty() && !fixedPages_.IsOpen()) || aiSettingsVisible_ ||
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
    // 目录、书签与阅读助手面板可拖动左边缘调整宽度，其余面板使用固定宽度。
    float preferred = 0.0f;
    switch (panel_) {
    case PanelMode::Chat:
        preferred = chatPanelWidth_;
        break;
    case PanelMode::Toc:
        preferred = tocPanelWidth_;
        break;
    case PanelMode::Bookmarks:
        preferred = bookmarksPanelWidth_;
        break;
    default:
        return kPanelWidth;
    }
    const float maximum = std::max(300.0f, std::min(760.0f, widthDip_ - 420.0f));
    return std::clamp(preferred, 300.0f, maximum);
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




void Application::OnKeyDown(WPARAM key) {
    // 阅读成就浮层打开时只响应 Esc 关闭。
    if (readingAchievementsVisible_) {
        if (key == VK_ESCAPE) {
            CloseReadingAchievements();
        }
        return;
    }
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
    // 局域网分享浮层打开时，键盘输入只作用于地址框；Esc 先取消焦点再关闭浮层。
    if (lanShareVisible_) {
        if (key == VK_ESCAPE) {
            if (lanAddressFocused_) {
                lanAddressFocused_ = false;
                StopCaretBlink();
            } else {
                CloseLanShare();
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        if (lanAddressFocused_) {
            const bool controlPressed = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            const bool shiftPressed = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            if (HandleInputKey(key, controlPressed, shiftPressed)) {
                return;
            }
            if (key == VK_RETURN) {
                StartLanFetch();
            }
        }
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

    if (aiGenerateVisible_) {
        if (key == VK_ESCAPE) {
            CloseAiGenerate();
            return;
        }
        if (chatFocus_ == ChatFocus::GeneratePrompt) {
            const bool shiftPressed = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            HandleInputKey(key, controlPressed, shiftPressed);
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
            // 对话面板内的文字选择优先于正文选择，其次是 PDF 页面选择。
            if (panel_ == PanelMode::Chat && CopyChatSelection()) {
                return;
            }
            if (fixedPages_.IsOpen() && CopyPdfSelection()) {
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
    KillTimer(hwnd_, kReadingTimerId);
    StopAiRequest();
    // 停止局域网分享并等待下载任务结束，避免后台线程访问已销毁的窗口。
    lanCancel_ = true;
    if (lanThread_.joinable()) {
        lanThread_.join();
    }
    if (lanServer_) {
        lanServer_->Stop();
    }
    lanSharing_ = false;
    // 关闭前丢弃 AI 设置弹框中未保存的编辑，避免随本次落盘写入。
    CloseAiSettings();
    if (view_ == ViewMode::Reader && !activeBookId_.empty()) {
        library_.TouchBook(activeBookId_, CurrentGlobalOffset(),
                           fixedPages_.IsOpen()
                               ? static_cast<uint32_t>(fixedPages_.CurrentPage())
                               : 0);
    }
    // 关闭前先结算最后一段章节阅读，再强制写盘，确保时长与阅读次数都不丢失。
    CommitChapterVisit();
    FlushReadingStats(true);
    data::ReaderSettings& settings = library_.Settings();
    settings.windowWidth = std::max(720, static_cast<int>(std::lround(widthDip_)));
    settings.windowHeight = std::max(520, static_cast<int>(std::lround(heightDip_)));
    std::wstring error;
    library_.Save(error);
    DestroyWindow(hwnd_);
}




void Application::OnRightButtonDown(int xPixels, int yPixels) {
    const float scale = renderer_.DpiScale();
    const float x = static_cast<float>(xPixels) / scale;
    const float y = static_cast<float>(yPixels) / scale;
    // 弹框打开时不响应右键菜单。
    if (chatSessionPickerVisible_ || fontPickerVisible_ || aiSettingsVisible_ ||
        shortcutSettingsVisible_ || aiGenerateVisible_) {
        return;
    }
    if (view_ == ViewMode::Shelf &&
        (shelfSettingsVisible_ || categoryManagerVisible_ || lanShareVisible_ ||
         !shelfCategoryPickerBookId_.empty())) {
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


void Application::ShowStatus(const std::wstring& text) {
    statusText_ = text;
    statusExpiry_ = util::CurrentUnixSeconds() + 8;
    SetTimer(hwnd_, 1, 1000, nullptr);
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
        // AI 接口设置与生成弹框依附于 AI 对话面板，面板切换时一并关闭；
        // 未保存的接口设置在此回滚。
        CloseAiSettings();
        aiGenerateVisible_ = false;
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

}  // namespace ui

