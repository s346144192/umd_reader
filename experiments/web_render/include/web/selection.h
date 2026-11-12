#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "web/dom.h"
#include "web/layout.h"
#include "web/text.h"
#include "web/types.h"

namespace web {

// 文档中的文本位置：文本节点 + 码位偏移（0 表示节点开头，等于节点长度表示末尾）。
struct TextPosition {
    const TextNode* node = nullptr;
    size_t offset = 0;
    bool valid = false;
};

bool operator==(const TextPosition& left, const TextPosition& right);
bool operator<(const TextPosition& left, const TextPosition& right);

// 选区：anchor 为起点，focus 为活动端。collapsed 表示没有选中任何字符。
struct SelectionRange {
    TextPosition anchor;
    TextPosition focus;
    bool collapsed = true;
};

// 页面坐标（左上角为原点）→ 最近的文本位置。
// y 落在某个行盒内时，在行内按字符边界取最近位置；否则取页面上最近的文本位置。
TextPosition HitTestTextPosition(const LayoutResult& layout, int page_index, float x, float y,
                                 TextService& text);

// 选区高亮矩形（页面坐标，按行切分，每行一个矩形）。collapsed 时返回空列表。
std::vector<RectF> SelectionRects(const LayoutResult& layout, int page_index,
                                  const SelectionRange& range, TextService& text);

// 选区纯文本：按文档顺序拼接；块级边界插入换行，行内边界原样连接。
std::string SelectedText(const Document& document, const SelectionRange& range);

// 全选整篇文档；文档没有文本时返回 collapsed 范围。
SelectionRange SelectAll(const Document& document);

// 双击选词（按空白与常见标点切分）、三击选段（按块的文本节点范围）。
SelectionRange ExpandToWord(const Document& document, const TextPosition& position);
SelectionRange ExpandToParagraph(const Document& document, const TextPosition& position);

// 把选区文本写入系统剪贴板；没有选中内容时返回 false。
bool CopySelectionToClipboard(const Document& document, const SelectionRange& range);

}  // namespace web
