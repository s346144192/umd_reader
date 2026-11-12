#pragma once

#include <string>

namespace web {

// 系统剪贴板文本读写（UTF-8 接口，内部转换为 UTF-16）。
// 打开剪贴板失败（被其他进程占用）时返回 false。
bool WriteClipboardText(const std::string& utf8_text);
bool ReadClipboardText(std::string* utf8_text);

}  // namespace web
