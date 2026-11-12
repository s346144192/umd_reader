#include "web/css_parser.h"

#include <string>
#include <vector>

namespace web {
namespace {

/* ----------------------------------------------------------------
   文本比较工具
   ---------------------------------------------------------------- */

char LowerAsciiChar(char character) {
    if (character >= 'A' && character <= 'Z') {
        return static_cast<char>(character - 'A' + 'a');
    }
    return character;
}

bool AsciiEqualsIgnoreCase(const std::string& left, const std::string& right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (size_t index = 0; index < left.size(); ++index) {
        if (LowerAsciiChar(left[index]) != LowerAsciiChar(right[index])) {
            return false;
        }
    }
    return true;
}

std::string LowerAsciiText(const std::string& text) {
    std::string result = text;
    for (size_t index = 0; index < result.size(); ++index) {
        result[index] = LowerAsciiChar(result[index]);
    }
    return result;
}

bool IsCssSpace(char character) {
    return character == ' ' || character == '\t' || character == '\n' || character == '\r' ||
           character == '\f';
}

/* ----------------------------------------------------------------
   属性选择器
   ---------------------------------------------------------------- */

/* 头文件的 SimpleSelector 只有 name/value 两个文本字段，属性选择器的
   运算符以 name 前缀编码（例如 "~=class"）。这里把前缀拆回运算符与属性名。 */
void SplitAttributeName(const std::string& encoded, std::string* operation, std::string* name) {
    static const char* const kOperators[] = {"~=", "|=", "^=", "$=", "*=", "="};
    for (const char* candidate : kOperators) {
        std::string prefix(candidate);
        if (encoded.size() >= prefix.size() &&
            encoded.compare(0, prefix.size(), prefix) == 0) {
            *operation = prefix;
            *name = encoded.substr(prefix.size());
            return;
        }
    }
    *operation = std::string();
    *name = encoded;
}

/* 空格分隔的属性值中出现某个完整词语。 */
bool ContainsWord(const std::string& text, const std::string& word) {
    size_t index = 0;
    while (index < text.size()) {
        while (index < text.size() && IsCssSpace(text[index])) {
            index += 1;
        }
        size_t begin = index;
        while (index < text.size() && !IsCssSpace(text[index])) {
            index += 1;
        }
        if (index > begin && text.compare(begin, index - begin, word) == 0 &&
            word.size() == index - begin) {
            return true;
        }
    }
    return false;
}

bool MatchAttributeSelector(const SimpleSelector& simple, const Element& element) {
    std::string operation;
    std::string attribute_name;
    SplitAttributeName(simple.name, &operation, &attribute_name);
    if (!element.HasAttribute(attribute_name)) {
        return false;
    }
    if (operation.empty()) {
        return true;
    }

    std::string actual = element.GetAttribute(attribute_name);
    std::string expected = simple.value;
    if (simple.case_insensitive) {
        actual = LowerAsciiText(actual);
        expected = LowerAsciiText(expected);
    }

    if (operation == "=") {
        return actual == expected;
    }
    if (operation == "~=") {
        if (expected.empty()) {
            return false;
        }
        return ContainsWord(actual, expected);
    }
    if (operation == "|=") {
        if (actual == expected) {
            return true;
        }
        return actual.size() > expected.size() && actual.compare(0, expected.size(), expected) == 0 &&
               actual[expected.size()] == '-';
    }
    if (operation == "^=") {
        if (expected.empty()) {
            return false;
        }
        return actual.size() >= expected.size() && actual.compare(0, expected.size(), expected) == 0;
    }
    if (operation == "$=") {
        if (expected.empty()) {
            return false;
        }
        return actual.size() >= expected.size() &&
               actual.compare(actual.size() - expected.size(), expected.size(), expected) == 0;
    }
    if (operation == "*=") {
        if (expected.empty()) {
            return false;
        }
        return actual.find(expected) != std::string::npos;
    }
    return false;
}

/* ----------------------------------------------------------------
   伪类
   ---------------------------------------------------------------- */

/* 元素在兄弟节点中的序号，只统计元素节点，从 1 开始。 */
size_t ElementPosition(const Element& element) {
    size_t position = 1;
    for (Node* sibling = element.PreviousSibling(); sibling != nullptr;
         sibling = sibling->PreviousSibling()) {
        if (sibling->IsElement()) {
            position += 1;
        }
    }
    return position;
}

bool IsFirstChild(const Element& element) {
    for (Node* sibling = element.PreviousSibling(); sibling != nullptr;
         sibling = sibling->PreviousSibling()) {
        if (sibling->IsElement()) {
            return false;
        }
    }
    return true;
}

bool IsLastChild(const Element& element) {
    for (Node* sibling = element.NextSibling(); sibling != nullptr; sibling = sibling->NextSibling()) {
        if (sibling->IsElement()) {
            return false;
        }
    }
    return true;
}

/* 解析 An+B 表达式，a 与 b 为输出。支持 odd、even、整数与 "an+b" 形式。 */
bool ParseNthExpression(const std::string& expression, int* a_value, int* b_value) {
    std::string text;
    for (size_t index = 0; index < expression.size(); ++index) {
        if (!IsCssSpace(expression[index])) {
            text.push_back(LowerAsciiChar(expression[index]));
        }
    }
    if (text.empty()) {
        return false;
    }
    if (text == "odd") {
        *a_value = 2;
        *b_value = 1;
        return true;
    }
    if (text == "even") {
        *a_value = 2;
        *b_value = 0;
        return true;
    }

    size_t index = 0;
    int sign = 1;
    if (text[index] == '+' || text[index] == '-') {
        sign = text[index] == '-' ? -1 : 1;
        index += 1;
    }
    std::string digits;
    while (index < text.size() && text[index] >= '0' && text[index] <= '9') {
        digits.push_back(text[index]);
        index += 1;
    }

    if (index < text.size() && text[index] == 'n') {
        int coefficient = 1;
        if (!digits.empty()) {
            int parsed = 0;
            for (size_t position = 0; position < digits.size(); ++position) {
                parsed = parsed * 10 + (digits[position] - '0');
            }
            coefficient = parsed;
        }
        coefficient *= sign;
        index += 1;

        int offset = 0;
        if (index < text.size()) {
            int offset_sign = 1;
            if (text[index] == '+' || text[index] == '-') {
                offset_sign = text[index] == '-' ? -1 : 1;
                index += 1;
            }
            std::string offset_digits;
            while (index < text.size() && text[index] >= '0' && text[index] <= '9') {
                offset_digits.push_back(text[index]);
                index += 1;
            }
            if (offset_digits.empty()) {
                return false;
            }
            int parsed = 0;
            for (size_t position = 0; position < offset_digits.size(); ++position) {
                parsed = parsed * 10 + (offset_digits[position] - '0');
            }
            offset = offset_sign * parsed;
        }
        if (index != text.size()) {
            return false;
        }
        *a_value = coefficient;
        *b_value = offset;
        return true;
    }

    /* 纯整数形式：An+B 中的 a 为 0。 */
    if (!digits.empty() && index == text.size()) {
        int parsed = 0;
        for (size_t position = 0; position < digits.size(); ++position) {
            parsed = parsed * 10 + (digits[position] - '0');
        }
        *a_value = 0;
        *b_value = sign * parsed;
        return true;
    }
    return false;
}

bool MatchesNthChild(const SimpleSelector& simple, const Element& element) {
    int a_value = 0;
    int b_value = 0;
    if (!ParseNthExpression(simple.value, &a_value, &b_value)) {
        return false;
    }
    long position = static_cast<long>(ElementPosition(element));
    if (a_value == 0) {
        return position == b_value;
    }
    long difference = position - b_value;
    if (difference % a_value != 0) {
        return false;
    }
    /* 序号 n 必须为非负整数。 */
    return difference / a_value >= 0;
}

/* :not() 的实参保存为简单选择器原文，这里复用正式语法解析后按复合选择器匹配。 */
bool MatchesNegation(const SimpleSelector& simple, const Element& element) {
    if (simple.value.empty()) {
        return false;
    }
    Selector inner;
    std::string error;
    if (!ParseSelector(simple.value, &inner, &error)) {
        return false;
    }
    if (inner.steps.size() != 1) {
        return false;
    }
    return !MatchesCompound(inner.steps[0].compound, element);
}

// 同标签兄弟中是否还有指定方向上的元素。
bool HasSameTypeSibling(const Element& element, bool forward) {
    for (Node* sibling = forward ? element.NextSibling() : element.PreviousSibling();
         sibling != nullptr; sibling = forward ? sibling->NextSibling() : sibling->PreviousSibling()) {
        if (sibling->IsElement() &&
            AsciiEqualsIgnoreCase(static_cast<Element*>(sibling)->TagName(),
                                  element.TagName())) {
            return true;
        }
    }
    return false;
}

bool MatchPseudoClass(const SimpleSelector& simple, const Element& element) {
    if (simple.name == "first-child") {
        return IsFirstChild(element);
    }
    if (simple.name == "last-child") {
        return IsLastChild(element);
    }
    if (simple.name == "only-child") {
        return IsFirstChild(element) && IsLastChild(element);
    }
    if (simple.name == "first-of-type") {
        return !HasSameTypeSibling(element, false);
    }
    if (simple.name == "last-of-type") {
        return !HasSameTypeSibling(element, true);
    }
    if (simple.name == "only-of-type") {
        return !HasSameTypeSibling(element, false) && !HasSameTypeSibling(element, true);
    }
    if (simple.name == "nth-child") {
        return MatchesNthChild(simple, element);
    }
    if (simple.name == "not") {
        return MatchesNegation(simple, element);
    }
    if (simple.name == "root") {
        const Node* parent = element.Parent();
        return parent != nullptr && parent->IsDocument();
    }
    // 静态状态伪类：按 HTML 属性判断，交互态伪类仍视为不成立。
    if (simple.name == "checked") {
        return element.HasAttribute("checked") || element.HasAttribute("selected");
    }
    if (simple.name == "disabled") {
        return element.HasAttribute("disabled");
    }
    if (simple.name == "enabled") {
        if (element.HasAttribute("disabled")) {
            return false;
        }
        const std::string& tag = element.TagName();
        return tag == "input" || tag == "button" || tag == "select" || tag == "textarea" ||
               tag == "option" || tag == "fieldset";
    }
    if (simple.name == "required") {
        return element.HasAttribute("required");
    }
    if (simple.name == "optional") {
        return !element.HasAttribute("required");
    }
    if (simple.name == "link" || simple.name == "any-link") {
        return element.TagName() == "a" && element.HasAttribute("href");
    }
    if (simple.name == "empty") {
        if (element.ChildCount() == 0) {
            return true;
        }
        // 只含空白文本的容器按空元素处理（注释不计入可见内容）。
        for (const NodePtr& child : element.Children()) {
            if (!child) {
                continue;
            }
            if (child->IsElement()) {
                return false;
            }
            if (child->IsText()) {
                const std::string& data = static_cast<const TextNode&>(*child).Data();
                for (char character : data) {
                    if (character != ' ' && character != '\t' && character != '\n' &&
                        character != '\r' && character != '\f') {
                        return false;
                    }
                }
            }
        }
        return true;
    }
    /* 动态伪类：读取窗口层维护的交互状态。:focus-visible 按 :focus 处理，
       :focus-within 在自身或任一祖先获得焦点时成立。 */
    if (simple.name == "hover") {
        return element.hovered;
    }
    if (simple.name == "active") {
        return element.active;
    }
    if (simple.name == "focus" || simple.name == "focus-visible") {
        return element.focused;
    }
    if (simple.name == "focus-within") {
        for (const Element* current = &element; current != nullptr;
             current = current->ParentElement()) {
            if (current->focused) {
                return true;
            }
        }
        return false;
    }
    /* 其余动态伪类（:visited、:target 等）静态匹配时视为不成立；
       未知伪类同样返回不匹配。 */
    return false;
}

/* ----------------------------------------------------------------
   选择器链匹配
   ---------------------------------------------------------------- */

bool MatchSelectorStep(const Selector& selector, size_t index, const Element& element) {
    if (!MatchesCompound(selector.steps[index].compound, element)) {
        return false;
    }
    if (index == 0) {
        return true;
    }

    switch (selector.steps[index].combinator) {
    case Combinator::Descendant: {
        for (Element* ancestor = element.ParentElement(); ancestor != nullptr;
             ancestor = ancestor->ParentElement()) {
            if (MatchSelectorStep(selector, index - 1, *ancestor)) {
                return true;
            }
        }
        return false;
    }
    case Combinator::Child: {
        Element* parent = element.ParentElement();
        return parent != nullptr && MatchSelectorStep(selector, index - 1, *parent);
    }
    case Combinator::NextSibling: {
        for (Node* sibling = element.PreviousSibling(); sibling != nullptr;
             sibling = sibling->PreviousSibling()) {
            if (sibling->IsElement()) {
                return MatchSelectorStep(selector, index - 1, *static_cast<Element*>(sibling));
            }
        }
        return false;
    }
    case Combinator::SubsequentSibling: {
        for (Node* sibling = element.PreviousSibling(); sibling != nullptr;
             sibling = sibling->PreviousSibling()) {
            if (sibling->IsElement() &&
                MatchSelectorStep(selector, index - 1, *static_cast<Element*>(sibling))) {
                return true;
            }
        }
        return false;
    }
    }
    return false;
}

/* 按文档顺序（前序遍历）遍历 root 的后代元素。 */
void TraverseElements(const Node& root, const std::vector<Selector>& selectors,
                      std::vector<Element*>* output) {
    for (const NodePtr& child : root.Children()) {
        if (child->IsElement()) {
            Element* element = static_cast<Element*>(child.get());
            for (const Selector& selector : selectors) {
                if (!selector.Empty() && MatchesSelector(selector, *element)) {
                    output->push_back(element);
                    break;
                }
            }
        }
        TraverseElements(*child, selectors, output);
    }
}

}  // namespace

/* ----------------------------------------------------------------
   对外接口
   ---------------------------------------------------------------- */

bool MatchesCompound(const CompoundSelector& compound, const Element& element) {
    for (const SimpleSelector& simple : compound.simples) {
        switch (simple.kind) {
        case SimpleSelector::Kind::Universal:
            break;
        case SimpleSelector::Kind::Type:
            if (!AsciiEqualsIgnoreCase(simple.name, element.TagName())) {
                return false;
            }
            break;
        case SimpleSelector::Kind::Class:
            if (!element.HasClass(simple.value)) {
                return false;
            }
            break;
        case SimpleSelector::Kind::Id:
            if (element.GetAttribute("id") != simple.value) {
                return false;
            }
            break;
        case SimpleSelector::Kind::Attribute:
            if (!MatchAttributeSelector(simple, element)) {
                return false;
            }
            break;
        case SimpleSelector::Kind::PseudoClass:
            if (!MatchPseudoClass(simple, element)) {
                return false;
            }
            break;
        }
    }
    return true;
}

bool MatchesSelector(const Selector& selector, const Element& element) {
    if (selector.Empty()) {
        return false;
    }
    return MatchSelectorStep(selector, selector.steps.size() - 1, element);
}

std::vector<Element*> QuerySelectorAll(const Node& root, const std::vector<Selector>& selectors) {
    std::vector<Element*> output;
    if (selectors.empty()) {
        return output;
    }
    TraverseElements(root, selectors, &output);
    return output;
}

}  // namespace web
