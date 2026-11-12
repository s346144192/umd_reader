#pragma once

#include <string>

#include "web/dom.h"

namespace web {

// 解析入口：供引擎、脚本宿主（innerHTML）与测试程序使用。
// source 视为 UTF-8 编码；解析器仅实现 HTML5 的一个可用子集：
// 标签、属性、文本、注释、DOCTYPE、实体引用、script/style 原始文本。
// 不支持表格、表单控件、SVG、MathML、模板与自定义元素。
bool ParseHtmlDocument(const std::string& source, Document& document, std::string* error);

// 片段解析：把 source 解析为 parent 的子节点（innerHTML 语义）。
// 遇到不匹配的结束标签时按 HTML 的容错规则处理，而不是报错退出。
bool ParseHtmlFragment(const std::string& source, Document& document, Element& parent,
                       std::string* error);

// 序列化：SerializeNode 输出 outerHTML，SerializeChildren 输出 innerHTML。
std::string SerializeNode(const Node& node);
std::string SerializeChildren(const Node& node);

// 解码 HTML 实体引用（&amp; &#x4E2D; 等）；未被识别的引用保持原样。
std::string DecodeHtmlEntities(const std::string& text);

}  // namespace web
