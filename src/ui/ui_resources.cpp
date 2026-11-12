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

D2D1_RECT_F Application::FontPickerPanelRect() const {
    constexpr float kPanelWidthDip = 560.0f;
    constexpr float kPanelHeightDip = 520.0f;
    const float width = std::min(kPanelWidthDip, widthDip_ - 80.0f);
    const float height = std::min(kPanelHeightDip, heightDip_ - 100.0f);
    const float left = (widthDip_ - width) * 0.5f;
    const float top = (heightDip_ - height) * 0.5f;
    return D2D1::RectF(left, top, left + width, top + height);
}

D2D1_RECT_F Application::FontPickerListRect() const {
    const D2D1_RECT_F panel = FontPickerPanelRect();
    return D2D1::RectF(panel.left + 20.0f, panel.top + 76.0f, panel.right - 20.0f,
                       panel.bottom - 16.0f);
}

std::wstring Application::CurrentFontDisplayName() const {
    const std::wstring& family = library_.Settings().fontFamily;
    for (const FontFamilyName& entry : fontFamilies_) {
        if (entry.canonical == family || entry.localized == family) {
            return entry.localized;
        }
    }
    return family;
}

void Application::SelectFontFamily(const std::wstring& family) {
    data::ReaderSettings& settings = library_.Settings();
    settings.fontFamily = family;
    fontPickerVisible_ = false;
    // ApplySettings 会重建文本格式、清空排版缓存并落盘。
    ApplySettings();
    // 弹框已关闭，立即重绘以同步命中区域。
    UpdateWindow(hwnd_);
}

void Application::DrawFontPicker(const ThemeColors& colors) {
    if (!fontPickerVisible_) {
        return;
    }
    const TextFormats& formats = renderer_.Formats();
    const data::ReaderSettings& settings = library_.Settings();
    renderer_.FillRect(D2D1::RectF(0.0f, 0.0f, widthDip_, heightDip_),
                       ParseHexColor(L"#000000", 0.35f));

    const D2D1_RECT_F panel = FontPickerPanelRect();
    renderer_.FillRoundedRect(panel, 12.0f, colors.surface);
    renderer_.StrokeRoundedRect(panel, 12.0f, colors.divider, 1.0f);
    renderer_.DrawTextBlock(L"选择字体",
                            D2D1::RectF(panel.left + 24.0f, panel.top + 16.0f,
                                        panel.right - 70.0f, panel.top + 52.0f),
                            formats.title.Get(), colors.text);
    DrawButton(L"×",
               D2D1::RectF(panel.right - 52.0f, panel.top + 14.0f, panel.right - 18.0f,
                           panel.top + 46.0f),
               HitKind::FontPickerClose, colors, false, false);
    renderer_.DrawLine(D2D1::Point2F(panel.left + 24.0f, panel.top + 60.0f),
                       D2D1::Point2F(panel.right - 24.0f, panel.top + 60.0f), colors.divider,
                       1.0f);

    const D2D1_RECT_F listRect = FontPickerListRect();
    if (fontFamilies_.empty()) {
        renderer_.DrawTextBlock(L"未找到可显示中文的字体。",
                                D2D1::RectF(listRect.left, listRect.top, listRect.right,
                                            listRect.top + 24.0f),
                                formats.caption.Get(), colors.secondaryText);
        return;
    }

    constexpr size_t kColumns = 3;
    const float columnGap = 8.0f;
    const float rowGap = 8.0f;
    const float itemHeight = 34.0f;
    const float itemWidth =
        (listRect.right - listRect.left - columnGap * static_cast<float>(kColumns - 1)) /
        static_cast<float>(kColumns);
    const size_t rows = (fontFamilies_.size() + kColumns - 1) / kColumns;
    const float contentHeight =
        rows > 0 ? static_cast<float>(rows) * (itemHeight + rowGap) - rowGap : 0.0f;
    fontPickerScrollMax_ = std::max(0.0f, contentHeight - (listRect.bottom - listRect.top));
    fontPickerScroll_ = std::clamp(fontPickerScroll_, 0.0f, fontPickerScrollMax_);

    BeginClip(listRect);
    for (size_t i = 0; i < fontFamilies_.size(); ++i) {
        const size_t row = i / kColumns;
        const size_t column = i % kColumns;
        const float x = listRect.left + static_cast<float>(column) * (itemWidth + columnGap);
        const float top = listRect.top - fontPickerScroll_ +
                          static_cast<float>(row) * (itemHeight + rowGap);
        const D2D1_RECT_F itemRect = D2D1::RectF(x, top, x + itemWidth, top + itemHeight);
        DrawButton(fontFamilies_[i].localized, itemRect, HitKind::FontPickerItem, colors, false,
                   settings.fontFamily == fontFamilies_[i].canonical, -1,
                   fontFamilies_[i].canonical);
    }
    EndClip();
}



ID2D1Bitmap* Application::CoverBitmap(const data::BookRecord& book) {
    if (book.coverFile.empty()) {
        return nullptr;
    }
    const auto found = coverCache_.find(book.id);
    if (found != coverCache_.end()) {
        return found->second.Get();
    }
    ComPtr<ID2D1Bitmap> bitmap = renderer_.LoadBitmap(library_.ResolveDataPath(book.coverFile));
    if (!bitmap) {
        return nullptr;
    }
    ComPtr<ID2D1Bitmap>& slot = coverCache_[book.id];
    slot = std::move(bitmap);
    return slot.Get();
}

ID2D1Bitmap* Application::ImageBitmap(const std::wstring& relativePath) {
    if (relativePath.empty()) {
        return nullptr;
    }
    const auto found = coverCache_.find(relativePath);
    if (found != coverCache_.end()) {
        return found->second.Get();
    }
    ComPtr<ID2D1Bitmap> bitmap = renderer_.LoadBitmap(library_.ResolveDataPath(relativePath));
    if (!bitmap) {
        return nullptr;
    }
    ComPtr<ID2D1Bitmap>& slot = coverCache_[relativePath];
    slot = std::move(bitmap);
    return slot.Get();
}

ID2D1Bitmap* Application::BackgroundBitmap() {
    const std::wstring& name = library_.Settings().backgroundImage;
    if (name.empty()) {
        return nullptr;
    }
    if (backgroundBitmap_ && backgroundBitmapName_ == name) {
        return backgroundBitmap_.Get();
    }
    backgroundBitmapName_ = name;
    backgroundBitmap_ = renderer_.LoadBitmap(library_.BackgroundDirectory() / name);
    return backgroundBitmap_.Get();
}

const Application::BackgroundThumb* Application::BackgroundThumbFor(const std::wstring& fileName) {
    const auto found = backgroundThumbCache_.find(fileName);
    if (found != backgroundThumbCache_.end()) {
        return &found->second;
    }
    BackgroundThumb entry;
    entry.bitmap = renderer_.LoadScaledBitmap(library_.BackgroundDirectory() / fileName,
                                              kBackgroundThumbMaxWidth,
                                              kBackgroundThumbMaxHeight, entry.sourceWidth,
                                              entry.sourceHeight);
    auto inserted = backgroundThumbCache_.emplace(fileName, std::move(entry));
    return &inserted.first->second;
}

void Application::RefreshBackgroundList() {
    backgroundFiles_ = library_.BackgroundImageNames();
    // 清理已不存在图片的缩略图缓存。
    for (auto it = backgroundThumbCache_.begin(); it != backgroundThumbCache_.end();) {
        if (std::find(backgroundFiles_.begin(), backgroundFiles_.end(), it->first) ==
            backgroundFiles_.end()) {
            if (backgroundBitmapName_ == it->first) {
                backgroundBitmap_.Reset();
                backgroundBitmapName_.clear();
            }
            it = backgroundThumbCache_.erase(it);
        } else {
            ++it;
        }
    }
    // 当前背景图片已被删除时回退到主题背景。
    data::ReaderSettings& settings = library_.Settings();
    if (settings.backgroundType == L"image" &&
        std::find(backgroundFiles_.begin(), backgroundFiles_.end(), settings.backgroundImage) ==
            backgroundFiles_.end()) {
        settings.backgroundType = L"theme";
        settings.backgroundImage.clear();
    }
}

void Application::SelectBackground(const std::wstring& type, const std::wstring& value) {
    data::ReaderSettings& settings = library_.Settings();
    settings.backgroundType = type;
    if (type == L"color") {
        settings.backgroundColor = value;
    } else if (type == L"image") {
        settings.backgroundImage = value;
    }
    std::wstring error;
    library_.Save(error);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Application::ImportBackgroundImages() {
    if (backgroundFiles_.size() >= data::kMaxBackgroundImages) {
        ShowStatus(L"背景图片最多 " + std::to_wstring(data::kMaxBackgroundImages) +
                   L" 张，请先删除后再导入");
        return;
    }
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog))) ||
        !dialog) {
        ShowStatus(L"无法创建图片选择对话框");
        return;
    }
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_ALLOWMULTISELECT | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
    const COMDLG_FILTERSPEC filters[] = {
        {L"图片（PNG / JPEG / BMP / GIF / WebP）", L"*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.webp"},
        {L"所有文件", L"*.*"}};
    dialog->SetFileTypes(2, filters);
    dialog->SetTitle(L"选择背景图片（最多 32 张，可多选）");
    if (FAILED(dialog->Show(hwnd_))) {
        return;
    }
    ComPtr<IShellItemArray> results;
    if (FAILED(dialog->GetResults(&results)) || !results) {
        return;
    }
    DWORD resultCount = 0;
    results->GetCount(&resultCount);
    std::wstring firstImported;
    std::wstring lastError;
    size_t importedCount = 0;
    for (DWORD i = 0; i < resultCount; ++i) {
        if (backgroundFiles_.size() + importedCount >= data::kMaxBackgroundImages) {
            lastError = L"背景图片最多 " + std::to_wstring(data::kMaxBackgroundImages) +
                        L" 张，其余图片未导入";
            break;
        }
        ComPtr<IShellItem> item;
        if (FAILED(results->GetItemAt(i, &item)) || !item) {
            continue;
        }
        PWSTR rawPath = nullptr;
        if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &rawPath)) || !rawPath) {
            continue;
        }
        const std::filesystem::path source(rawPath);
        CoTaskMemFree(rawPath);
        std::wstring error;
        std::wstring name;
        if (library_.ImportBackgroundImage(source, name, error)) {
            ++importedCount;
            if (firstImported.empty()) {
                firstImported = name;
            }
        } else {
            lastError = error;
        }
    }
    RefreshBackgroundList();
    if (!firstImported.empty()) {
        // 导入后直接应用第一张，方便立即看到背景效果。
        SelectBackground(L"image", firstImported);
        ShowStatus(L"已导入 " + std::to_wstring(importedCount) + L" 张背景图片");
    } else if (!lastError.empty()) {
        ShowStatus(lastError);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
    // 导入后列表条目增加，立即重绘以同步命中区域。
    UpdateWindow(hwnd_);
}


}  // namespace ui
