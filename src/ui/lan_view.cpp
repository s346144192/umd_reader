#include "ui/application.h"

#include <d2d1helper.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <vector>

#include "core/text_util.h"
#include "ui/ui_internal.h"

namespace ui {
using namespace detail;
namespace {

constexpr float kLanPanelMaxWidth = 760.0f;
constexpr float kLanPanelMaxHeight = 620.0f;
constexpr float kLanPanelMinWidth = 560.0f;
constexpr float kLanPanelMinHeight = 420.0f;
constexpr float kLanListRowHeight = 52.0f;
constexpr float kLanListPadding = 6.0f;

std::wstring LanFormatLabel(int formatType) {
    switch (formatType) {
    case 0:
        return L"文本";
    case 1:
        return L"UMD 文本";
    case 2:
        return L"UMD 动漫";
    case 3:
        return L"PDF";
    default:
        return L"未知格式";
    }
}

std::wstring LanExtensionForFormat(int formatType) {
    switch (formatType) {
    case 0:
        return L".txt";
    case 3:
        return L".pdf";
    default:
        return L".umd";
    }
}

// 远端文件名只取文件名部分，并替换 Windows 不允许出现在文件路径中的字符。
std::wstring SanitizeLanFileName(const std::wstring& name, int formatType) {
    std::wstring base = std::filesystem::path(name).filename().wstring();
    for (wchar_t& character : base) {
        const bool invalid = character == L'<' || character == L'>' || character == L':' ||
                             character == L'"' || character == L'/' || character == L'\\' ||
                             character == L'|' || character == L'?' || character == L'*' ||
                             character < 0x20;
        if (invalid) {
            character = L'_';
        }
    }
    while (!base.empty() && (base.back() == L'.' || base.back() == L' ')) {
        base.pop_back();
    }
    constexpr size_t kMaxFileNameChars = 120;
    if (base.size() > kMaxFileNameChars) {
        const std::wstring extension = std::filesystem::path(base).extension().wstring();
        const size_t keep = extension.size() < kMaxFileNameChars
                                ? kMaxFileNameChars - extension.size()
                                : kMaxFileNameChars;
        base = base.substr(0, keep) + (extension.size() < kMaxFileNameChars ? extension
                                                                           : std::wstring());
    }
    if (base.empty()) {
        base = L"book";
    }
    if (std::filesystem::path(base).extension().empty()) {
        base += LanExtensionForFormat(formatType);
    }
    return base;
}

std::wstring ChapterCountLabel(int chapterCount) {
    if (chapterCount <= 0) {
        return L"未分章";
    }
    return std::to_wstring(chapterCount) + L" 章";
}

}  // namespace

D2D1_RECT_F Application::LanSharePanelRect() const {
    const float panelWidth =
        std::min(kLanPanelMaxWidth, std::max(kLanPanelMinWidth, widthDip_ - 60.0f));
    const float panelHeight =
        std::min(kLanPanelMaxHeight, std::max(kLanPanelMinHeight, heightDip_ - 60.0f));
    const float left = (widthDip_ - panelWidth) * 0.5f;
    const float top = (heightDip_ - panelHeight) * 0.5f;
    return D2D1::RectF(left, top, left + panelWidth, top + panelHeight);
}

D2D1_RECT_F Application::LanShareListRect() const {
    const D2D1_RECT_F panel = LanSharePanelRect();
    return D2D1::RectF(panel.left + 24.0f, panel.top + 286.0f, panel.right - 24.0f,
                       panel.bottom - 52.0f);
}

D2D1_RECT_F Application::LanAddressInputRect() const {
    const D2D1_RECT_F panel = LanSharePanelRect();
    // 左侧留出「扫描」按钮，右侧留出「获取书单」按钮。
    return D2D1::RectF(panel.left + 24.0f + 84.0f, panel.top + 224.0f,
                       panel.right - 24.0f - 118.0f, panel.top + 256.0f);
}

void Application::DrawLanShare(const ThemeColors& colors) {
    if (!lanShareVisible_) {
        return;
    }
    const TextFormats& formats = renderer_.Formats();
    renderer_.FillRect(D2D1::RectF(0.0f, 0.0f, widthDip_, heightDip_),
                       ParseHexColor(L"#000000", 0.35f));

    const D2D1_RECT_F panel = LanSharePanelRect();
    renderer_.FillRoundedRect(panel, 12.0f, colors.surface);
    renderer_.StrokeRoundedRect(panel, 12.0f, colors.divider, 1.0f);

    const float left = panel.left + 24.0f;
    const float right = panel.right - 24.0f;
    const float top = panel.top;

    renderer_.DrawTextBlock(L"局域网分享", D2D1::RectF(left, top + 14.0f, right, top + 50.0f),
                            formats.title.Get(), colors.text);
    renderer_.DrawLine(D2D1::Point2F(left, top + 56.0f), D2D1::Point2F(right, top + 56.0f),
                       colors.divider, 1.0f);

    // 本机分享：开关按钮与状态说明。
    renderer_.DrawTextBlock(L"本机分享", D2D1::RectF(left, top + 64.0f, right, top + 82.0f),
                            formats.caption.Get(), colors.secondaryText);
    DrawButton(lanSharing_ ? L"停止分享" : L"开始分享",
               D2D1::RectF(left, top + 86.0f, left + 132.0f, top + 118.0f),
               HitKind::LanShareToggle, colors, !lanSharing_, lanSharing_);
    std::wstring shareState;
    if (lanSharing_) {
        shareState = L"正在分享 " + std::to_wstring(lanSharedBookCount_) + L" 本书";
    } else {
        shareState = L"分享已关闭";
    }
    renderer_.DrawTextBlock(shareState,
                            D2D1::RectF(left + 144.0f, top + 90.0f, right, top + 118.0f),
                            formats.listItem.Get(),
                            lanSharing_ ? colors.accent : colors.secondaryText);

    // 分享地址：每行一个局域网地址，最多显示两个，其余以数量说明。
    renderer_.DrawTextBlock(L"分享地址",
                            D2D1::RectF(left, top + 124.0f, right, top + 140.0f),
                            formats.caption.Get(), colors.secondaryText);
    float addressY = top + 142.0f;
    if (lanLocalAddresses_.empty()) {
        renderer_.DrawTextBlock(L"未检测到可用的局域网地址",
                                D2D1::RectF(left, addressY, right, addressY + 18.0f),
                                formats.caption.Get(), colors.secondaryText);
    } else {
        const size_t shown = std::min<size_t>(lanLocalAddresses_.size(), 2);
        for (size_t i = 0; i < shown; ++i) {
            std::wstring line = L"http://" + lanLocalAddresses_[i] + L":" +
                                std::to_wstring(lanServer_ ? lanServer_->Port() : kLanSharePort);
            if (i + 1 == shown && lanLocalAddresses_.size() > shown) {
                line += L"（另有 " + std::to_wstring(lanLocalAddresses_.size() - shown) +
                        L" 个地址）";
            }
            renderer_.DrawTextBlock(util::Ellipsize(line, 80),
                                    D2D1::RectF(left, addressY, right, addressY + 18.0f),
                                    formats.caption.Get(),
                                    lanSharing_ ? colors.text : colors.secondaryText);
            addressY += 18.0f;
        }
    }
    renderer_.DrawTextBlock(
        util::Ellipsize(
            L"另一台电脑运行本程序，在下方输入上述地址即可同步书架并下载阅读。", 80),
        D2D1::RectF(left, top + 180.0f, right, top + 198.0f), formats.caption.Get(),
        colors.secondaryText);
    renderer_.DrawLine(D2D1::Point2F(left, top + 204.0f), D2D1::Point2F(right, top + 204.0f),
                       colors.divider, 1.0f);

    // 连接其他设备：地址输入与书单获取。
    renderer_.DrawTextBlock(L"连接其他设备",
                            D2D1::RectF(left, top + 206.0f, right, top + 222.0f),
                            formats.caption.Get(), colors.secondaryText);
    const D2D1_RECT_F inputRect = LanAddressInputRect();
    renderer_.FillRoundedRect(inputRect, 8.0f, colors.surfaceAlt);
    renderer_.StrokeRoundedRect(inputRect, 8.0f,
                                lanAddressFocused_ ? colors.accent : colors.divider,
                                lanAddressFocused_ ? 1.6f : 1.0f);
    const D2D1_RECT_F inputTextRect =
        D2D1::RectF(inputRect.left + 10.0f, inputRect.top + 7.0f, inputRect.right - 10.0f,
                    inputRect.bottom - 7.0f);
    const bool addressEmpty = lanAddressDraft_.empty();
    renderer_.DrawTextBlock(
        addressEmpty ? std::wstring(L"对方地址，如 192.168.1.100") : lanAddressDraft_,
        inputTextRect, formats.listItem.Get(),
        addressEmpty ? colors.secondaryText : colors.text);
    if (lanAddressFocused_) {
        DrawInputSelection(inputTextRect, formats.listItem.Get(), false, colors);
    }
    AddHit(inputRect, HitKind::LanAddressInput);

    DrawButton(lanJobRunning_ && lanJobKind_ == 3 ? L"扫描中" : L"扫描",
               D2D1::RectF(left, top + 224.0f, left + 76.0f, top + 256.0f), HitKind::LanScan,
               colors, false, lanJobRunning_ && lanJobKind_ == 3);

    DrawButton(lanJobRunning_ && lanJobKind_ == 1 ? L"获取中…" : L"获取书单",
               D2D1::RectF(right - 110.0f, top + 224.0f, right, top + 256.0f),
               HitKind::LanConnect, colors, true, false);

    renderer_.DrawTextBlock(util::Ellipsize(lanStatus_, 90),
                            D2D1::RectF(left, top + 262.0f, right, top + 280.0f),
                            formats.caption.Get(), colors.secondaryText);

    // 列表区域：显示扫描发现的设备，或所选设备的远端书籍。
    const bool showDevices = lanShowDevices_;
    const size_t itemCount =
        showDevices ? lanDiscoveredHosts_.size() : lanRemoteBooks_.size();
    const D2D1_RECT_F listRect = LanShareListRect();
    renderer_.FillRoundedRect(listRect, 8.0f, colors.surfaceAlt);
    const float contentHeight =
        static_cast<float>(itemCount) * kLanListRowHeight + kLanListPadding * 2.0f;
    lanListScrollMax_ = std::max(0.0f, contentHeight - (listRect.bottom - listRect.top));
    lanListScroll_ = std::clamp(lanListScroll_, 0.0f, lanListScrollMax_);

    if (itemCount == 0) {
        std::wstring hint;
        if (showDevices) {
            hint = lanJobRunning_ && lanJobKind_ == 3
                       ? std::wstring(L"正在扫描局域网…")
                       : std::wstring(L"未发现设备；请确认对方已开启分享，并允许防火墙访问。");
        } else {
            hint = lanJobRunning_ && lanJobKind_ == 1
                       ? std::wstring(L"正在从对方设备获取书单…")
                       : std::wstring(L"输入对方地址，或从扫描结果中选择设备。");
        }
        renderer_.DrawTextBlock(hint,
                                D2D1::RectF(listRect.left + 12.0f, listRect.top,
                                            listRect.right - 12.0f, listRect.bottom),
                                formats.centered.Get(), colors.secondaryText);
    } else {
        BeginClip(listRect);
        float rowTop = listRect.top + kLanListPadding - lanListScroll_;
        for (size_t i = 0; i < itemCount; ++i) {
            const D2D1_RECT_F rowRect =
                D2D1::RectF(listRect.left + 6.0f, rowTop, listRect.right - 6.0f,
                            rowTop + kLanListRowHeight - 4.0f);
            rowTop += kLanListRowHeight;
            if (rowRect.bottom < listRect.top || rowRect.top > listRect.bottom) {
                continue;
            }
            if (IsHovered(rowRect)) {
                renderer_.FillRoundedRect(rowRect, 8.0f, colors.surface);
            }
            if (showDevices) {
                // 设备行：地址与连接入口。
                renderer_.DrawTextBlock(lanDiscoveredHosts_[i],
                                        D2D1::RectF(rowRect.left + 10.0f, rowRect.top + 5.0f,
                                                    rowRect.right - 90.0f, rowRect.top + 25.0f),
                                        formats.listItem.Get(), colors.text);
                renderer_.DrawTextBlock(L"运行本程序的设备，点击连接后获取书单",
                                        D2D1::RectF(rowRect.left + 10.0f, rowRect.top + 25.0f,
                                                    rowRect.right - 90.0f, rowRect.top + 45.0f),
                                        formats.caption.Get(), colors.secondaryText);
                DrawPlainButton(L"连接",
                                D2D1::RectF(rowRect.right - 70.0f, rowRect.top + 10.0f,
                                            rowRect.right - 8.0f, rowRect.top + 42.0f),
                                HitKind::LanConnectHost, colors, static_cast<int>(i));
                continue;
            }
            const net::RemoteBook& book = lanRemoteBooks_[i];
            const std::wstring title = book.title.empty() ? book.fileName : book.title;
            renderer_.DrawTextBlock(util::Ellipsize(title, 46),
                                    D2D1::RectF(rowRect.left + 10.0f, rowRect.top + 5.0f,
                                                rowRect.right - 158.0f, rowRect.top + 25.0f),
                                    formats.listItem.Get(), colors.text);
            const std::wstring author =
                book.author.empty() ? std::wstring(L"未知作者") : book.author;
            const std::wstring caption =
                util::Ellipsize(author, 22) + L" · " + LanFormatLabel(book.formatType) + L" · " +
                ChapterCountLabel(book.chapterCount);
            renderer_.DrawTextBlock(util::Ellipsize(caption, 46),
                                    D2D1::RectF(rowRect.left + 10.0f, rowRect.top + 25.0f,
                                                rowRect.right - 158.0f, rowRect.top + 45.0f),
                                    formats.caption.Get(), colors.secondaryText);
            renderer_.DrawTextBlock(util::FormatFileSize(book.fileSize),
                                    D2D1::RectF(rowRect.right - 154.0f, rowRect.top + 5.0f,
                                                rowRect.right - 66.0f, rowRect.top + 25.0f),
                                    formats.captionRight.Get(), colors.secondaryText);
            DrawPlainButton(L"下载",
                            D2D1::RectF(rowRect.right - 62.0f, rowRect.top + 10.0f,
                                        rowRect.right - 8.0f, rowRect.top + 42.0f),
                            HitKind::LanDownload, colors, static_cast<int>(i));
        }
        EndClip();
    }

    DrawButton(L"关闭",
               D2D1::RectF(panel.right - 124.0f, panel.bottom - 42.0f, panel.right - 24.0f,
                           panel.bottom - 10.0f),
               HitKind::LanShareClose, colors, false, false);
}

void Application::OpenLanShare() {
    lanShareVisible_ = true;
    shelfSettingsVisible_ = false;
    categoryManagerVisible_ = false;
    shelfCategoryPickerBookId_.clear();
    shelfSearchFocused_ = false;
    categoryNameFocused_ = false;
    StopCaretBlink();
    CloseMenu();
    lanLocalAddresses_ = net::LocalIpv4Addresses();
    if (lanStatus_.empty()) {
        lanStatus_ = L"开启分享后，另一台电脑即可在此获取书单并下载。";
    }
    RefreshLanShareBooks();
    InvalidateRect(hwnd_, nullptr, FALSE);
    // 浮层控件需要立即可用，先同步建立命中区域。
    UpdateWindow(hwnd_);
    // 首次打开时自动扫描局域网；已有扫描结果或书单时不打断当前内容。
    if (lanDiscoveredHosts_.empty() && lanRemoteBooks_.empty() && !lanJobRunning_) {
        StartLanScan();
    }
}

void Application::CloseLanShare() {
    lanShareVisible_ = false;
    lanAddressFocused_ = false;
    StopCaretBlink();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Application::ToggleLanSharing() {
    if (lanSharing_) {
        if (lanServer_) {
            lanServer_->Stop();
        }
        lanSharing_ = false;
        lanStatus_ = L"分享已停止";
        InvalidateRect(hwnd_, nullptr, FALSE);
        UpdateWindow(hwnd_);
        return;
    }
    if (!lanServer_) {
        lanServer_ = std::make_unique<net::LanShareServer>();
    }
    RefreshLanShareBooks();
    std::wstring error;
    if (!lanServer_->Start(kLanSharePort, error)) {
        lanStatus_ = L"启动分享失败：" + error;
        lanSharing_ = false;
        InvalidateRect(hwnd_, nullptr, FALSE);
        UpdateWindow(hwnd_);
        return;
    }
    lanSharing_ = true;
    lanLocalAddresses_ = net::LocalIpv4Addresses();
    lanStatus_ = L"分享已开启，端口 " + std::to_wstring(kLanSharePort) +
                 L"；若 Windows 防火墙提示，请允许本程序访问网络。";
    InvalidateRect(hwnd_, nullptr, FALSE);
    UpdateWindow(hwnd_);
}

void Application::RefreshLanShareBooks() {
    std::vector<net::SharedBook> shared;
    shared.reserve(library_.Books().size());
    for (const data::BookRecord& book : library_.Books()) {
        if (!book.FileExists()) {
            continue;
        }
        net::SharedBook entry;
        entry.id = book.id;
        entry.title = book.title;
        entry.author = book.author;
        entry.fileName = book.fileName;
        entry.path = std::filesystem::path(book.path);
        entry.fileSize = book.fileSize;
        entry.formatType = book.formatType;
        entry.chapterCount = static_cast<int>(book.chapters.size());
        shared.push_back(std::move(entry));
    }
    lanSharedBookCount_ = shared.size();
    if (lanServer_) {
        lanServer_->SetBooks(std::move(shared));
    }
}

void Application::StartLanScan() {
    if (lanJobRunning_) {
        lanStatus_ = L"正在处理上一个网络任务，请稍候。";
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    lanCancel_.store(false);
    {
        std::lock_guard<std::mutex> lock(lanMutex_);
        lanJobDone_ = false;
        lanJobSucceeded_ = false;
        lanJobKind_ = 3;
        lanJobError_.clear();
        lanJobHosts_.clear();
    }
    if (lanThread_.joinable()) {
        lanThread_.join();
    }
    lanJobRunning_ = true;
    lanShowDevices_ = true;
    lanListScroll_ = 0.0f;
    lanStatus_ = L"正在扫描局域网…";
    lanThread_ = std::thread([this]() {
        std::vector<std::wstring> hosts;
        std::wstring error;
        const bool succeeded = net::ScanLanHosts(kLanSharePort, 260, hosts, error, &lanCancel_);
        {
            std::lock_guard<std::mutex> lock(lanMutex_);
            lanJobKind_ = 3;
            lanJobSucceeded_ = succeeded;
            lanJobError_ = std::move(error);
            lanJobHosts_ = std::move(hosts);
            lanJobDone_ = true;
        }
        if (hwnd_ != nullptr) {
            PostMessageW(hwnd_, kLanJobMessage, 0, 0);
        }
    });
    InvalidateRect(hwnd_, nullptr, FALSE);
    UpdateWindow(hwnd_);
}

void Application::StartLanFetch() {
    if (lanJobRunning_) {
        lanStatus_ = L"正在处理上一个网络任务，请稍候。";
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    std::wstring host;
    uint16_t port = 0;
    if (!net::ParseShareAddress(lanAddressDraft_, kLanSharePort, host, port)) {
        lanStatus_ = L"地址格式无效，请输入 192.168.1.100:8712 形式。";
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    lanCancel_.store(false);
    {
        std::lock_guard<std::mutex> lock(lanMutex_);
        lanJobDone_ = false;
        lanJobSucceeded_ = false;
        lanJobKind_ = 1;
        lanJobError_.clear();
        lanJobBooks_.clear();
    }
    if (lanThread_.joinable()) {
        lanThread_.join();
    }
    lanJobRunning_ = true;
    // 切换到书籍视图并清空旧书单，避免显示上一台设备的书籍。
    lanShowDevices_ = false;
    lanRemoteBooks_.clear();
    lanListScroll_ = 0.0f;
    lanStatus_ = L"正在获取书单…";
    lanThread_ = std::thread([this, host, port]() {
        std::vector<net::RemoteBook> books;
        std::wstring error;
        const bool succeeded =
            net::LanShareClient::FetchBooks(host, port, books, error, &lanCancel_);
        {
            std::lock_guard<std::mutex> lock(lanMutex_);
            lanJobKind_ = 1;
            lanJobSucceeded_ = succeeded;
            lanJobError_ = std::move(error);
            lanJobBooks_ = std::move(books);
            lanJobDone_ = true;
        }
        if (hwnd_ != nullptr) {
            PostMessageW(hwnd_, kLanJobMessage, 0, 0);
        }
    });
    InvalidateRect(hwnd_, nullptr, FALSE);
    UpdateWindow(hwnd_);
}

void Application::StartLanDownload(int index) {
    if (lanJobRunning_) {
        lanStatus_ = L"正在处理上一个网络任务，请稍候。";
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (index < 0 || index >= static_cast<int>(lanRemoteBooks_.size())) {
        return;
    }
    std::wstring host;
    uint16_t port = 0;
    if (!net::ParseShareAddress(lanAddressDraft_, kLanSharePort, host, port)) {
        lanStatus_ = L"地址格式无效，请输入 192.168.1.100:8712 形式。";
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    const net::RemoteBook book = lanRemoteBooks_[static_cast<size_t>(index)];
    const std::wstring remoteName = book.fileName.empty() ? book.title : book.fileName;
    // 书架中已有同名且同大小的书籍时不重复下载，避免产生重复条目。
    for (const data::BookRecord& record : library_.Books()) {
        if (record.fileSize == book.fileSize && record.formatType == book.formatType &&
            record.fileName == remoteName) {
            lanStatus_ = L"书架中已有《" + record.title + L"》，无需重复下载。";
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
    }
    const std::filesystem::path incoming = library_.DataDirectory() / L"incoming";
    std::error_code errorCode;
    std::filesystem::create_directories(incoming, errorCode);
    if (errorCode) {
        lanStatus_ = L"无法创建下载目录：" + incoming.wstring();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    const std::wstring fileName = SanitizeLanFileName(remoteName, book.formatType);
    const std::wstring stem = std::filesystem::path(fileName).stem().wstring();
    const std::wstring extension = std::filesystem::path(fileName).extension().wstring();
    std::filesystem::path target = incoming / fileName;
    for (int suffix = 2; std::filesystem::exists(target, errorCode); ++suffix) {
        target = incoming / (stem + L" (" + std::to_wstring(suffix) + L")" + extension);
    }

    lanCancel_.store(false);
    {
        std::lock_guard<std::mutex> lock(lanMutex_);
        lanJobDone_ = false;
        lanJobSucceeded_ = false;
        lanJobKind_ = 2;
        lanJobError_.clear();
        lanJobDownloadPath_.clear();
    }
    if (lanThread_.joinable()) {
        lanThread_.join();
    }
    lanJobRunning_ = true;
    lanStatus_ = L"正在下载《" + book.title + L"》…";
    lanThread_ = std::thread([this, host, port, book, target]() {
        std::wstring error;
        const bool succeeded =
            net::LanShareClient::DownloadBook(host, port, book, target, error, &lanCancel_);
        {
            std::lock_guard<std::mutex> lock(lanMutex_);
            lanJobKind_ = 2;
            lanJobSucceeded_ = succeeded;
            lanJobError_ = std::move(error);
            lanJobDownloadPath_ = target.wstring();
            lanJobDone_ = true;
        }
        if (hwnd_ != nullptr) {
            PostMessageW(hwnd_, kLanJobMessage, 0, 0);
        }
    });
    InvalidateRect(hwnd_, nullptr, FALSE);
    UpdateWindow(hwnd_);
}

void Application::OnLanJobDone() {
    bool succeeded = false;
    int kind = 0;
    std::wstring error;
    std::wstring downloadPath;
    std::vector<net::RemoteBook> books;
    std::vector<std::wstring> hosts;
    {
        std::lock_guard<std::mutex> lock(lanMutex_);
        if (!lanJobDone_) {
            return;
        }
        lanJobDone_ = false;
        succeeded = lanJobSucceeded_;
        kind = lanJobKind_;
        error = lanJobError_;
        downloadPath = lanJobDownloadPath_;
        books = lanJobBooks_;
        hosts = lanJobHosts_;
        lanJobBooks_.clear();
        lanJobHosts_.clear();
        lanJobDownloadPath_.clear();
    }
    lanJobRunning_ = false;
    if (lanThread_.joinable()) {
        lanThread_.join();
    }
    if (kind == 3) {
        // 扫描结果切换到设备视图；只发现一台时直接获取该书单。
        lanShowDevices_ = true;
        if (succeeded) {
            lanDiscoveredHosts_ = std::move(hosts);
            lanListScroll_ = 0.0f;
            if (lanDiscoveredHosts_.empty()) {
                lanStatus_ = L"未发现运行本程序的局域网设备。";
            } else {
                lanStatus_ = L"扫描完成，发现 " +
                             std::to_wstring(lanDiscoveredHosts_.size()) + L" 台设备。";
                if (lanDiscoveredHosts_.size() == 1 && lanShareVisible_) {
                    lanAddressDraft_ = lanDiscoveredHosts_.front();
                    StartLanFetch();
                }
            }
        } else {
            lanStatus_ = L"扫描失败：" + error;
        }
    } else if (kind == 2) {
        if (succeeded) {
            lanStatus_ = L"下载完成，正在导入…";
            ImportLanBook(downloadPath);
        } else {
            lanStatus_ = L"下载失败：" + error;
        }
    } else if (succeeded) {
        lanRemoteBooks_ = std::move(books);
        lanShowDevices_ = false;
        lanListScroll_ = 0.0f;
        lanStatus_ = L"已获取 " + std::to_wstring(lanRemoteBooks_.size()) + L" 本书";
    } else {
        // 获取失败时回到已扫描到的设备列表，方便改选其他设备。
        lanShowDevices_ = !lanDiscoveredHosts_.empty();
        lanStatus_ = L"获取书单失败：" + error;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Application::ImportLanBook(const std::wstring& path) {
    bool alreadyExists = false;
    std::wstring error;
    // 与本地导入一致：解析书籍期间显示等待光标。
    SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    const bool added = library_.AddBook(std::filesystem::path(path), error, alreadyExists);
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    if (!added) {
        lanStatus_ = L"导入失败：" + error;
        return;
    }
    coverCache_.clear();
    RefreshShelfOrder();
    RefreshLanShareBooks();
    lanStatus_ = alreadyExists ? L"该书已在书架中，已刷新书籍信息"
                               : L"已从局域网导入并加入书架";
}

}  // namespace ui
