#pragma once

// 应用标识的唯一来源。更名时只需修改下面这一行的宏，其余标识全部由它派生。
#define APP_BRAND L"UmdReader"

namespace app_id {

// 产品名与界面标题。
inline constexpr wchar_t kName[] = APP_BRAND;
inline constexpr wchar_t kWindowTitle[] = L"UMD 阅读器";

// 窗口类与单实例互斥体。
inline constexpr wchar_t kWindowClass[] = APP_BRAND L"Window";
inline constexpr wchar_t kInstanceMutexName[] = L"Local\\" APP_BRAND L"Instance";

// 用户数据目录名（%LOCALAPPDATA% 下的回落目录）。
inline constexpr wchar_t kDataFolderName[] = APP_BRAND;

// 网络请求标识与临时文件前缀。
inline constexpr wchar_t kHttpUserAgent[] = APP_BRAND L"/1.0";
inline constexpr wchar_t kTtsTempPrefix[] = APP_BRAND L"_tts_";

// 文件关联 ProgID。每个扩展名独立，互不覆盖。
inline constexpr wchar_t kDocumentProgId[] = APP_BRAND L".Document";
inline constexpr wchar_t kTextProgId[] = APP_BRAND L".Text";

}  // namespace app_id
