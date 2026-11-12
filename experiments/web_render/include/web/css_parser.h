#pragma once

#include <string>
#include <vector>

#include "web/css.h"
#include "web/dom.h"

namespace web {

// 词法由 re2c 生成，语法由 bison 生成。解析器只覆盖渲染所需子集：
// 类型/类/ID/属性/后代/子代/相邻兄弟/通用兄弟选择器、伪类
// (:first-child、:last-child、:nth-child(an+b)、:not、:hover 忽略)，
// 以及常规声明块；@media、@font-face 等 at-rule 会被跳过。

// 解析样式表。viewport_width / viewport_height 大于 0 时按该视口求值
// @media 查询：条件成立的块展开为普通规则，不成立的块整体丢弃。
// 传入非正值则保持旧行为（跳过所有 at-rule）。
bool ParseStyleSheet(const std::string& source, StyleSheet* stylesheet, std::string* error);
bool ParseStyleSheet(const std::string& source, float viewport_width, float viewport_height,
                     StyleSheet* stylesheet, std::string* error);

// 解析行内 style 属性或任意声明块（不含外层花括号），结果追加到 output。
bool ParseDeclarationBlock(const std::string& source, std::vector<Declaration>* output,
                           std::string* error);

// 解析单个选择器（用于 querySelector）。复合选择器链不合法时返回 false。
bool ParseSelector(const std::string& source, Selector* selector, std::string* error);

// 选择器匹配与查询。
bool MatchesCompound(const CompoundSelector& compound, const Element& element);
bool MatchesSelector(const Selector& selector, const Element& element);
// 按文档顺序返回匹配任一选择器的后代元素（不含 root 自身）。
std::vector<Element*> QuerySelectorAll(const Node& root,
                                       const std::vector<Selector>& selectors);

}  // namespace web
