/* 文本选择与选区几何实现。
   坐标约定：布局结果使用文档坐标（y 向下），本文件对外返回页面坐标；
   文本位置使用文本节点 + 码位偏移（不是字节偏移）。 */
#include "web/selection.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <vector>

#include "web/clipboard.h"
#include "web/utf8.h"

namespace web {
namespace {

const size_t kInvalidIndex = static_cast<size_t>(-1);

// 块级标签集合：用于选区的换行插入与段落扩展。
bool IsBlockTagName(const std::string& tag_name) {
    static const char* const kBlockTagNames[] = {
        "div",  "p",     "h1",    "h2",   "h3",    "h4",   "h5",
        "h6",   "li",    "ul",    "ol",   "br",    "section", "article",
        "header", "footer", "main", "nav", "aside", "figure", "blockquote",
        "pre",  "table", "tr",    "td",   "form",  "dl",   "dt",
        "dd",
    };
    for (const char* name : kBlockTagNames) {
        if (tag_name == name) {
            return true;
        }
    }
    return false;
}

// 统计 UTF-8 文本的码位数。
size_t CountCodepoints(const std::string& text) {
    size_t count = 0;
    size_t offset = 0;
    while (offset < text.size()) {
        const Utf8Char character = DecodeUtf8(text, offset);
        if (character.length == 0) {
            break;
        }
        offset += character.length;
        ++count;
    }
    return count;
}

// 把文本中的字节偏移换算成码位索引；偏移越界时返回文本码位总数。
size_t CodepointIndexAtByteOffset(const std::string& text, size_t byte_offset) {
    size_t index = 0;
    size_t offset = 0;
    while (offset < text.size() && offset < byte_offset) {
        const Utf8Char character = DecodeUtf8(text, offset);
        if (character.length == 0) {
            break;
        }
        offset += character.length;
        ++index;
    }
    return index;
}

// 把码位索引换算成字节偏移；索引越界时返回文本总字节数。
size_t ByteOffsetAtCodepointIndex(const std::string& text, size_t codepoint_index) {
    size_t index = 0;
    size_t offset = 0;
    while (index < codepoint_index && offset < text.size()) {
        const Utf8Char character = DecodeUtf8(text, offset);
        if (character.length == 0) {
            break;
        }
        offset += character.length;
        ++index;
    }
    return offset;
}

// 追加 [begin_index, end_index) 码位区间的原始文本。
void AppendCodepointSlice(const std::string& text, size_t begin_index, size_t end_index,
                          std::string* output) {
    const size_t begin_byte = ByteOffsetAtCodepointIndex(text, begin_index);
    const size_t end_byte = ByteOffsetAtCodepointIndex(text, end_index);
    if (end_byte > begin_byte) {
        output->append(text, begin_byte, end_byte - begin_byte);
    }
}

// 按文档顺序收集所有文本节点。
void CollectTextNodes(const Node& node, std::vector<const TextNode*>* output) {
    if (node.IsText()) {
        output->push_back(static_cast<const TextNode*>(&node));
        return;
    }
    for (const NodePtr& child : node.Children()) {
        if (child) {
            CollectTextNodes(*child, output);
        }
    }
}

// 选区中的一段可见文字片段，附带它在文本节点中的码位起点与长度。
struct FragmentInfo {
    const PlacedText* placed = nullptr;
    const LineFragment* line = nullptr;
    size_t start_codepoint = 0;
    size_t codepoint_count = 0;
};

// 按布局树的遍历顺序收集所有非空文字片段。
void CollectFragments(const LayoutBox& box, std::vector<FragmentInfo>* output) {
    for (const LineFragment& line : box.lines) {
        for (const PlacedText& placed : line.texts) {
            if (placed.node == nullptr || placed.text.empty()) {
                continue;
            }
            FragmentInfo info;
            info.placed = &placed;
            info.line = &line;
            info.start_codepoint =
                CodepointIndexAtByteOffset(placed.node->Data(), placed.offset);
            info.codepoint_count = CountCodepoints(placed.text);
            output->push_back(info);
        }
    }
    for (const std::unique_ptr<LayoutBox>& child : box.children) {
        if (child) {
            CollectFragments(*child, output);
        }
    }
}

// 页面竖直范围；无分页时下界为无穷大。
bool PageVerticalBounds(const LayoutResult& layout, int page_index, float* page_top,
                        float* page_bottom) {
    if (layout.page_starts.empty() || page_index < 0 ||
        page_index >= static_cast<int>(layout.page_starts.size())) {
        return false;
    }
    *page_top = layout.page_starts[static_cast<size_t>(page_index)];
    *page_bottom = layout.page_content_height > 0.0f
                       ? *page_top + layout.page_content_height
                       : std::numeric_limits<float>::infinity();
    return true;
}

bool LineOverlapsPage(const RectF& rect, float page_top, float page_bottom) {
    return rect.bottom > page_top && rect.top < page_bottom;
}

// 行盒到查询点的竖直距离；点落在行盒内时为 0。
float LineVerticalDistance(const RectF& rect, float y) {
    if (y >= rect.top && y < rect.bottom) {
        return 0.0f;
    }
    return std::min(std::fabs(y - rect.top), std::fabs(y - rect.bottom));
}

// 命中测试的候选状态：竖直距离优先，其次水平距离。
struct HitTestState {
    bool found = false;
    float best_vertical_distance = std::numeric_limits<float>::infinity();
    float best_horizontal_distance = std::numeric_limits<float>::infinity();
    TextPosition best_position;
};

// 递归遍历布局盒，寻找与查询点最近的字符边界。
void VisitForHitTest(const LayoutBox& box, float document_y, float x, float page_top,
                     float page_bottom, TextService& text, HitTestState* state) {
    for (const LineFragment& line : box.lines) {
        if (!LineOverlapsPage(line.rect, page_top, page_bottom)) {
            continue;
        }

        bool line_has_text = false;
        float line_best_horizontal = std::numeric_limits<float>::infinity();
        TextPosition line_best_position;
        for (const PlacedText& placed : line.texts) {
            if (placed.node == nullptr || placed.text.empty()) {
                continue;
            }
            line_has_text = true;
            const size_t start_codepoint =
                CodepointIndexAtByteOffset(placed.node->Data(), placed.offset);

            float pen_x = placed.x;
            size_t codepoint_offset = 0;
            size_t byte_offset = 0;
            float horizontal = std::fabs(pen_x - x);
            if (horizontal < line_best_horizontal) {
                line_best_horizontal = horizontal;
                line_best_position.node = placed.node;
                line_best_position.offset = start_codepoint;
                line_best_position.valid = true;
            }

            while (byte_offset < placed.text.size()) {
                const Utf8Char character = DecodeUtf8(placed.text, byte_offset);
                if (character.length == 0) {
                    break;
                }
                pen_x += text.Advance(placed.font, character.codepoint) + placed.letter_spacing;
                byte_offset += character.length;
                ++codepoint_offset;

                horizontal = std::fabs(pen_x - x);
                if (horizontal < line_best_horizontal) {
                    line_best_horizontal = horizontal;
                    line_best_position.node = placed.node;
                    line_best_position.offset = start_codepoint + codepoint_offset;
                    line_best_position.valid = true;
                }
            }
        }
        if (!line_has_text) {
            continue;
        }

        const float line_vertical_distance = LineVerticalDistance(line.rect, document_y);
        if (!state->found || line_vertical_distance < state->best_vertical_distance ||
            (line_vertical_distance == state->best_vertical_distance &&
             line_best_horizontal < state->best_horizontal_distance)) {
            state->found = true;
            state->best_vertical_distance = line_vertical_distance;
            state->best_horizontal_distance = line_best_horizontal;
            state->best_position = line_best_position;
        }
    }

    for (const std::unique_ptr<LayoutBox>& child : box.children) {
        if (child) {
            VisitForHitTest(*child, document_y, x, page_top, page_bottom, text, state);
        }
    }
}

// 片段内前 codepoint_count 个码位的累计宽度。
float PrefixWidth(const PlacedText& placed, size_t codepoint_count, TextService& text) {
    float width = 0.0f;
    size_t offset = 0;
    size_t count = 0;
    while (offset < placed.text.size() && count < codepoint_count) {
        const Utf8Char character = DecodeUtf8(placed.text, offset);
        if (character.length == 0) {
            break;
        }
        width += text.Advance(placed.font, character.codepoint) + placed.letter_spacing;
        offset += character.length;
        ++count;
    }
    return width;
}

// 字符是否作为选词分隔符。
bool IsWordSeparator(uint32_t codepoint) {
    if (codepoint == 0x0020 || codepoint == 0x0009 || codepoint == 0x000A ||
        codepoint == 0x000D || codepoint == 0x000C) {
        return true;
    }
    switch (codepoint) {
        // 半角逗号、句号、分号、冒号、感叹号、问号与顿号、省略号。
        case 0x002C:  // ,
        case 0x002E:  // .
        case 0x003B:  // ;
        case 0x003A:  // :
        case 0x0021:  // !
        case 0x003F:  // ?
        case 0x3001:  // 、
        case 0x3002:  // 。
        case 0xFF0C:  // ，
        case 0xFF1B:  // ；
        case 0xFF1A:  // ：
        case 0xFF01:  // ！
        case 0xFF1F:  // ？
        case 0x2026:  // …
        // 引号。
        case 0x0022:  // "
        case 0x0027:  // '
        case 0x2018:  // ‘
        case 0x2019:  // ’
        case 0x201C:  // “
        case 0x201D:  // ”
        case 0x300C:  // 「
        case 0x300D:  // 」
        case 0x300E:  // 『
        case 0x300F:  // 』
        // 括号与书名号。
        case 0x0028:  // (
        case 0x0029:  // )
        case 0x005B:  // [
        case 0x005D:  // ]
        case 0x007B:  // {
        case 0x007D:  // }
        case 0x003C:  // <
        case 0x003E:  // >
        case 0xFF08:  // （
        case 0xFF09:  // ）
        case 0xFF3B:  // ［
        case 0xFF3D:  // ］
        case 0xFF5B:  // ｛
        case 0xFF5D:  // ｝
        case 0x3010:  // 【
        case 0x3011:  // 】
        case 0x3008:  // 〈
        case 0x3009:  // 〉
        case 0x300A:  // 《
        case 0x300B:  // 》
            return true;
        default:
            return false;
    }
}

// 把节点向量转换为从头到尾的选区；没有有效文本时返回 collapsed。
SelectionRange RangeForTextNodes(const std::vector<const TextNode*>& nodes) {
    SelectionRange range;
    if (nodes.empty()) {
        return range;
    }
    size_t total_codepoints = 0;
    for (const TextNode* node : nodes) {
        if (node != nullptr) {
            total_codepoints += CountCodepoints(node->Data());
        }
    }
    if (total_codepoints == 0) {
        return range;
    }
    range.anchor.node = nodes.front();
    range.anchor.offset = 0;
    range.anchor.valid = true;
    range.focus.node = nodes.back();
    range.focus.offset = CountCodepoints(nodes.back()->Data());
    range.focus.valid = true;
    range.collapsed = false;
    return range;
}

// 选区端点在某一段文字片段中的位置。
struct EndpointInFragments {
    size_t fragment_index = kInvalidIndex;
    size_t offset_in_fragment = 0;
};

// 把文本位置映射到片段队列中的索引与片段内码位偏移。
EndpointInFragments LocateEndpoint(const std::vector<FragmentInfo>& fragments,
                                   const TextPosition& position) {
    EndpointInFragments result;
    if (!position.valid || position.node == nullptr) {
        return result;
    }
    size_t last_index = kInvalidIndex;
    for (size_t index = 0; index < fragments.size(); ++index) {
        const FragmentInfo& fragment = fragments[index];
        if (fragment.placed->node != position.node) {
            continue;
        }
        last_index = index;
        const size_t fragment_end = fragment.start_codepoint + fragment.codepoint_count;
        if (position.offset < fragment_end) {
            result.fragment_index = index;
            result.offset_in_fragment =
                position.offset > fragment.start_codepoint
                    ? position.offset - fragment.start_codepoint
                    : 0;
            if (result.offset_in_fragment > fragment.codepoint_count) {
                result.offset_in_fragment = fragment.codepoint_count;
            }
            return result;
        }
    }
    if (last_index != kInvalidIndex) {
        result.fragment_index = last_index;
        result.offset_in_fragment = fragments[last_index].codepoint_count;
    }
    return result;
}

// 判断两个片段端点先后；同一片段时比较片段内偏移。
bool EndpointBefore(size_t left_index, size_t left_offset, size_t right_index,
                    size_t right_offset) {
    if (left_index != right_index) {
        return left_index < right_index;
    }
    return left_offset < right_offset;
}

// 选中文本的遍历状态。
struct SelectedTextState {
    size_t first_index = 0;
    size_t last_index = 0;
    size_t first_offset = 0;
    size_t last_offset = 0;
    size_t next_text_index = 0;
    std::string output;
};

void AppendNewline(std::string* output) {
    if (!output->empty() && output->back() != '\n') {
        output->push_back('\n');
    }
}

// 按文档顺序输出选区覆盖的文字，并在块级边界补换行。
bool EmitSelectedText(const Node& node, SelectedTextState* state) {
    if (node.IsText()) {
        const size_t index = state->next_text_index++;
        if (index < state->first_index || index > state->last_index) {
            return false;
        }
        const TextNode& text_node = static_cast<const TextNode&>(node);
        const std::string& data = text_node.Data();
        const size_t codepoint_count = CountCodepoints(data);
        size_t begin = index == state->first_index ? state->first_offset : 0;
        size_t end = index == state->last_index ? state->last_offset : codepoint_count;
        begin = std::min(begin, codepoint_count);
        end = std::min(end, codepoint_count);
        if (end <= begin) {
            return false;
        }
        AppendCodepointSlice(data, begin, end, &state->output);
        return true;
    }
    if (!node.IsElement()) {
        return false;
    }

    const Element& element = static_cast<const Element&>(node);
    if (element.TagName() == "br") {
        // 换行元素位于选区起点之后、选区终点对应的文本节点之前时输出换行。
        if (state->next_text_index > state->first_index &&
            state->next_text_index <= state->last_index) {
            AppendNewline(&state->output);
            return true;
        }
        return false;
    }

    const bool block_level = IsBlockTagName(element.TagName());
    if (block_level) {
        // 块级元素开始前的边界；上方已有内容时补一个换行。
        AppendNewline(&state->output);
    }

    bool emitted = false;
    for (const NodePtr& child : element.Children()) {
        if (child && EmitSelectedText(*child, state)) {
            emitted = true;
        }
    }
    if (emitted && block_level) {
        // 块级元素结束后的边界，与下一个块的起始换行合并为一行。
        AppendNewline(&state->output);
    }
    return emitted;
}

}  // namespace

bool operator==(const TextPosition& left, const TextPosition& right) {
    return left.node == right.node && left.offset == right.offset;
}

bool operator<(const TextPosition& left, const TextPosition& right) {
    // 同一文本节点内按码位偏移比较。不同节点的文档先后无法由指针决定，
    // 因此返回 false，调用方需先按文档顺序整理 anchor 与 focus。
    if (left.node == right.node) {
        return left.offset < right.offset;
    }
    return false;
}

TextPosition HitTestTextPosition(const LayoutResult& layout, int page_index, float x, float y,
                                 TextService& text) {
    TextPosition result;
    if (!layout.root) {
        return result;
    }
    float page_top = 0.0f;
    float page_bottom = 0.0f;
    if (!PageVerticalBounds(layout, page_index, &page_top, &page_bottom)) {
        return result;
    }

    const float document_y = y + page_top;
    HitTestState state;
    VisitForHitTest(*layout.root, document_y, x, page_top, page_bottom, text, &state);
    if (state.found) {
        result = state.best_position;
    }
    return result;
}

std::vector<RectF> SelectionRects(const LayoutResult& layout, int page_index,
                                  const SelectionRange& range, TextService& text) {
    std::vector<RectF> rectangles;
    if (range.collapsed || !layout.root || range.anchor.node == nullptr ||
        range.focus.node == nullptr) {
        return rectangles;
    }
    float page_top = 0.0f;
    float page_bottom = 0.0f;
    if (!PageVerticalBounds(layout, page_index, &page_top, &page_bottom)) {
        return rectangles;
    }

    std::vector<FragmentInfo> fragments;
    CollectFragments(*layout.root, &fragments);

    const EndpointInFragments anchor = LocateEndpoint(fragments, range.anchor);
    const EndpointInFragments focus = LocateEndpoint(fragments, range.focus);
    if (anchor.fragment_index == kInvalidIndex || focus.fragment_index == kInvalidIndex) {
        return rectangles;
    }

    size_t first_index = anchor.fragment_index;
    size_t first_offset = anchor.offset_in_fragment;
    size_t last_index = focus.fragment_index;
    size_t last_offset = focus.offset_in_fragment;
    if (EndpointBefore(last_index, last_offset, first_index, first_offset)) {
        std::swap(first_index, last_index);
        std::swap(first_offset, last_offset);
    }

    for (size_t index = first_index; index <= last_index && index < fragments.size();
         ++index) {
        const FragmentInfo& fragment = fragments[index];
        const PlacedText& placed = *fragment.placed;
        size_t begin = 0;
        size_t end = fragment.codepoint_count;
        if (first_index == last_index) {
            begin = first_offset;
            end = last_offset;
        } else if (index == first_index) {
            begin = first_offset;
        } else if (index == last_index) {
            end = last_offset;
        }
        begin = std::min(begin, fragment.codepoint_count);
        end = std::min(end, fragment.codepoint_count);
        if (end <= begin) {
            continue;
        }
        if (!LineOverlapsPage(fragment.line->rect, page_top, page_bottom)) {
            continue;
        }

        const float left = placed.x + PrefixWidth(placed, begin, text);
        const float right = placed.x + PrefixWidth(placed, end, text);
        if (right <= left) {
            continue;
        }
        RectF rectangle;
        rectangle.left = left;
        rectangle.right = right;
        rectangle.top = fragment.line->rect.top - page_top;
        rectangle.bottom = fragment.line->rect.bottom - page_top;
        if (rectangle.IsEmpty()) {
            continue;
        }
        rectangles.push_back(rectangle);
    }
    return rectangles;
}

std::string SelectedText(const Document& document, const SelectionRange& range) {
    if (range.collapsed || range.anchor.node == nullptr || range.focus.node == nullptr) {
        return std::string();
    }

    std::vector<const TextNode*> nodes;
    CollectTextNodes(document, &nodes);
    if (nodes.empty()) {
        return std::string();
    }

    size_t anchor_index = kInvalidIndex;
    size_t focus_index = kInvalidIndex;
    for (size_t index = 0; index < nodes.size(); ++index) {
        if (nodes[index] == range.anchor.node) {
            anchor_index = index;
        }
        if (nodes[index] == range.focus.node) {
            focus_index = index;
        }
    }
    if (anchor_index == kInvalidIndex || focus_index == kInvalidIndex) {
        return std::string();
    }

    SelectedTextState state;
    if (anchor_index < focus_index) {
        state.first_index = anchor_index;
        state.first_offset = range.anchor.offset;
        state.last_index = focus_index;
        state.last_offset = range.focus.offset;
    } else if (anchor_index > focus_index) {
        state.first_index = focus_index;
        state.first_offset = range.focus.offset;
        state.last_index = anchor_index;
        state.last_offset = range.anchor.offset;
    } else {
        state.first_index = anchor_index;
        state.last_index = anchor_index;
        state.first_offset = std::min(range.anchor.offset, range.focus.offset);
        state.last_offset = std::max(range.anchor.offset, range.focus.offset);
    }

    EmitSelectedText(document, &state);
    while (!state.output.empty() && state.output.back() == '\n') {
        state.output.pop_back();
    }
    return state.output;
}

SelectionRange SelectAll(const Document& document) {
    std::vector<const TextNode*> nodes;
    CollectTextNodes(document, &nodes);
    return RangeForTextNodes(nodes);
}

SelectionRange ExpandToWord(const Document& document, const TextPosition& position) {
    (void)document;
    SelectionRange range;
    if (!position.valid || position.node == nullptr) {
        return range;
    }

    const std::string& data = position.node->Data();
    std::vector<uint32_t> codepoints;
    size_t offset = 0;
    while (offset < data.size()) {
        const Utf8Char character = DecodeUtf8(data, offset);
        if (character.length == 0) {
            break;
        }
        codepoints.push_back(character.codepoint);
        offset += character.length;
    }

    size_t caret = std::min(position.offset, codepoints.size());
    size_t left = caret;
    while (left > 0 && !IsWordSeparator(codepoints[left - 1])) {
        --left;
    }
    size_t right = caret;
    while (right < codepoints.size() && !IsWordSeparator(codepoints[right])) {
        ++right;
    }
    if (left == right) {
        return range;
    }

    range.anchor.node = position.node;
    range.anchor.offset = left;
    range.anchor.valid = true;
    range.focus.node = position.node;
    range.focus.offset = right;
    range.focus.valid = true;
    range.collapsed = false;
    return range;
}

SelectionRange ExpandToParagraph(const Document& document, const TextPosition& position) {
    SelectionRange range;
    if (!position.valid || position.node == nullptr) {
        return range;
    }

    const Node* current = position.node;
    const Node* block = nullptr;
    while (current != nullptr) {
        Node* parent = current->Parent();
        if (parent == nullptr) {
            break;
        }
        if (parent->IsElement()) {
            const Element& element = static_cast<const Element&>(*parent);
            if (IsBlockTagName(element.TagName())) {
                block = &element;
                break;
            }
        }
        current = parent;
    }

    std::vector<const TextNode*> nodes;
    if (block != nullptr) {
        CollectTextNodes(*block, &nodes);
    } else {
        CollectTextNodes(document, &nodes);
    }
    return RangeForTextNodes(nodes);
}

bool CopySelectionToClipboard(const Document& document, const SelectionRange& range) {
    const std::string selected = SelectedText(document, range);
    if (selected.empty()) {
        return false;
    }
    return WriteClipboardText(selected);
}

}  // namespace web
