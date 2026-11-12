#include "html_builder.h"

#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "html_lexer.h"

/* bison 生成的 html_parser.h 没有 extern "C" 包裹，而 webhtmlparse 由 C 源文件
   实现，因此在包含该头文件之前显式指定 C 链接方式。 */
extern "C" {
#include "html_parser.h"
}

#include "web/dom.h"
#include "web/html.h"
#include "web/utf8.h"

namespace web {

// 由 dom.cpp 提供的内部工厂。公共头文件没有创建 Doctype 文本节点的入口，
// 而该节点的构造器为私有（Document 是唯一友元），因此在此声明内部接口。
TextNodePtr CreateDoctypeTextNode(std::string data);

}  // namespace web

/* 构建器内部结构：不对外暴露，仅通过 html_builder.h 中的不透明指针访问。 */
struct WebHtmlBuilder {
    web::Document* document = nullptr;
    web::Element* root = nullptr; /* 文档模式下自动建立的 <html> */
    web::Element* head = nullptr;
    web::Element* body = nullptr;
    bool fragment = false;
    bool body_started = false;
    bool finished = false;
    bool failed = false;

    /* 打开元素栈；第 0 项为文档模式的 <html> 或片段模式的父元素。 */
    std::vector<web::Element*> stack;

    int error_count = 0;
    std::string first_error;
};

namespace {

bool IsHeadElementTag(const std::string& tag) {
    return tag == "title" || tag == "meta" || tag == "link" || tag == "style" || tag == "base";
}

/* 会触发 <p> 自动闭合的块级开始标签。 */
bool IsBlockTag(const std::string& tag) {
    return tag == "address" || tag == "article" || tag == "aside" || tag == "blockquote" ||
           tag == "dd" || tag == "div" || tag == "dl" || tag == "dt" || tag == "fieldset" ||
           tag == "figcaption" || tag == "figure" || tag == "footer" || tag == "form" ||
           tag == "h1" || tag == "h2" || tag == "h3" || tag == "h4" || tag == "h5" ||
           tag == "h6" || tag == "header" || tag == "hr" || tag == "li" || tag == "main" ||
           tag == "nav" || tag == "ol" || tag == "p" || tag == "pre" || tag == "section" ||
           tag == "table" || tag == "ul" || tag == "body" || tag == "head" || tag == "html";
}

bool IsWhitespaceOnly(const std::string& text) {
    for (char character : text) {
        if (!web::IsAsciiSpace(character)) {
            return false;
        }
    }
    return true;
}

/* 开始标签的隐式闭合规则。栈的第 0 项是挂载根，任何情况下都不弹出。 */
void CloseForStartTag(WebHtmlBuilder* builder, const std::string& tag) {
    // <p> 遇到块级开始标签时自动闭合。
    if (IsBlockTag(tag)) {
        while (builder->stack.size() > 1 && builder->stack.back()->TagName() == "p") {
            builder->stack.pop_back();
        }
    }
    // 新的 <li> 关闭仍处于打开状态的前一个 <li>。
    if (tag == "li") {
        while (builder->stack.size() > 1 && builder->stack.back()->TagName() == "li") {
            builder->stack.pop_back();
        }
    }
    // <a> 内再次出现 <a> 时先关闭外层 <a>。
    if (tag == "a") {
        for (size_t index = builder->stack.size(); index > 1; --index) {
            if (builder->stack[index - 1]->TagName() == "a") {
                builder->stack.resize(index - 1);
                break;
            }
        }
    }
}

/* 结束标签：从栈顶向下查找同名元素，找到后连同其上层元素一起关闭。 */
void PopThroughTag(WebHtmlBuilder* builder, const std::string& tag) {
    for (size_t index = builder->stack.size(); index > 1; --index) {
        if (builder->stack[index - 1]->TagName() == tag) {
            builder->stack.resize(index - 1);
            return;
        }
    }
}

/* 计算当前插入位置。for_element 表示本次插入的是元素，text_has_content 表示
   文本含有非空白字符（仅 for_element 为 false 时有意义）。 */
web::Element* InsertionTarget(WebHtmlBuilder* builder, bool for_element,
                              const std::string& tag, bool text_has_content) {
    if (builder->fragment) {
        return builder->stack.empty() ? nullptr : builder->stack.back();
    }

    // 注释既不算正文也不算元素，直接留在当前容器内，不触发 head/body 切换。
    if (!for_element && !text_has_content) {
        if (builder->stack.size() > 1) {
            return builder->stack.back();
        }
        return builder->body_started ? builder->body : builder->head;
    }

    // <head> 仍打开时遇到非 head 内容，按 HTML 的容错规则隐式关闭 <head>。
    if (builder->stack.size() > 1 && builder->stack.back() == builder->head) {
        const bool stays_in_head = for_element && IsHeadElementTag(tag);
        if (!stays_in_head) {
            builder->stack.pop_back();
            builder->body_started = true;
        }
    }
    if (builder->stack.size() > 1) {
        return builder->stack.back();
    }
    // 纯空白文本已在上面返回；这里只剩有内容的文本，应进入 body。
    if (!builder->body_started && for_element && IsHeadElementTag(tag)) {
        return builder->head;
    }
    builder->body_started = true;
    return builder->body;
}

/* 把词法器记录的源位置写入节点。 */
void ApplySpan(web::Node* node, size_t begin, size_t end, int line) {
    if (node == nullptr) {
        return;
    }
    node->span.begin = begin;
    node->span.end = end;
    node->span.line = line;
}

/* 把属性数组写入元素；重复属性名以第一次出现者为准。 */
void ApplyAttributes(web::Element* element, const char* const* names, const char* const* values,
                     int count) {
    if (element == nullptr || names == nullptr || values == nullptr) {
        return;
    }
    for (int index = 0; index < count; ++index) {
        if (names[index] == nullptr) {
            continue;
        }
        if (element->HasAttribute(names[index])) {
            continue;
        }
        element->SetAttribute(names[index],
                              values[index] != nullptr ? std::string(values[index]) : std::string());
    }
}

}  // namespace

extern "C" struct WebHtmlBuilder* webhtml_builder_create(void* document_pointer,
                                                        void* parent_pointer) {
    if (document_pointer == nullptr) {
        return nullptr;
    }
    WebHtmlBuilder* builder = new WebHtmlBuilder();
    builder->document = static_cast<web::Document*>(document_pointer);

    if (parent_pointer != nullptr) {
        builder->fragment = true;
        builder->stack.push_back(static_cast<web::Element*>(parent_pointer));
        return builder;
    }

    web::ElementPtr root = builder->document->CreateElement("html");
    web::ElementPtr head = builder->document->CreateElement("head");
    web::ElementPtr body = builder->document->CreateElement("body");
    if (!root || !head || !body) {
        builder->failed = true;
        return builder;
    }
    builder->root = root.get();
    builder->head = head.get();
    builder->body = body.get();
    builder->document->AppendChild(*builder->document, std::move(root));
    builder->document->AppendChild(*builder->root, std::move(head));
    builder->document->AppendChild(*builder->root, std::move(body));
    builder->stack.push_back(builder->root);
    return builder;
}

extern "C" void webhtml_builder_destroy(struct WebHtmlBuilder* builder) {
    delete builder;
}

extern "C" void webhtml_builder_begin_tag(struct WebHtmlParseState* state,
                                          const char* const* names,
                                          const char* const* values,
                                          int count,
                                          int self_closing) {
    if (state == nullptr || state->builder == nullptr) {
        return;
    }
    WebHtmlBuilder* builder = state->builder;

    std::string tag;
    if (state->open_tag_name != nullptr) {
        tag = web::AsciiLower(std::string(state->open_tag_name));
    }
    if (tag.empty()) {
        webhtml_pending_reset(state);
        return;
    }

    CloseForStartTag(builder, tag);

    // 文档模式下 <html>/<head>/<body> 使用自动建立的节点，避免重复创建。
    if (!builder->fragment) {
        if (tag == "html") {
            ApplyAttributes(builder->root, names, values, count);
            ApplySpan(builder->root, state->open_tag_begin, state->open_tag_end,
                      state->open_tag_line);
            webhtml_pending_reset(state);
            return;
        }
        if (tag == "head" && !builder->body_started) {
            ApplyAttributes(builder->head, names, values, count);
            ApplySpan(builder->head, state->open_tag_begin, state->open_tag_end,
                      state->open_tag_line);
            if (!self_closing) {
                builder->stack.push_back(builder->head);
            }
            webhtml_pending_reset(state);
            return;
        }
        if (tag == "body") {
            ApplyAttributes(builder->body, names, values, count);
            ApplySpan(builder->body, state->open_tag_begin, state->open_tag_end,
                      state->open_tag_line);
            builder->body_started = true;
            if (!self_closing) {
                builder->stack.push_back(builder->body);
            }
            webhtml_pending_reset(state);
            return;
        }
    }

    web::Element* parent = InsertionTarget(builder, true, tag, true);
    if (parent == nullptr) {
        webhtml_pending_reset(state);
        return;
    }

    web::ElementPtr element = builder->document->CreateElement(tag);
    if (!element) {
        builder->failed = true;
        webhtml_pending_reset(state);
        return;
    }
    ApplyAttributes(element.get(), names, values, count);
    web::Element* element_raw = element.get();
    ApplySpan(element_raw, state->open_tag_begin, state->open_tag_end, state->open_tag_line);
    builder->document->AppendChild(*parent, std::move(element));

    if (!self_closing && !web::Element::IsVoidTag(tag)) {
        builder->stack.push_back(element_raw);
    }
    webhtml_pending_reset(state);
}

extern "C" void webhtml_builder_end_tag(struct WebHtmlParseState* state, const char* tag_name) {
    if (state == nullptr || state->builder == nullptr || tag_name == nullptr) {
        return;
    }
    WebHtmlBuilder* builder = state->builder;
    const std::string tag = web::AsciiLower(std::string(tag_name));
    if (tag.empty()) {
        return;
    }

    if (!builder->fragment) {
        if (tag == "html") {
            // <html> 是文档模式的栈底元素，不随结束标签弹出。
            return;
        }
        if (tag == "head") {
            PopThroughTag(builder, "head");
            builder->body_started = true;
            return;
        }
    }
    PopThroughTag(builder, tag);
}

extern "C" void webhtml_builder_text(struct WebHtmlParseState* state, const char* text) {
    if (state == nullptr || state->builder == nullptr || text == nullptr) {
        return;
    }
    WebHtmlBuilder* builder = state->builder;
    const std::string data(text);
    web::Element* parent = InsertionTarget(builder, false, std::string(), !IsWhitespaceOnly(data));
    if (parent == nullptr) {
        return;
    }
    web::TextNodePtr node = builder->document->CreateTextNode(data);
    if (!node) {
        builder->failed = true;
        return;
    }
    ApplySpan(node.get(), state->token.begin, state->token.end, state->token.line);
    builder->document->AppendChild(*parent, std::move(node));
}

extern "C" void webhtml_builder_comment(struct WebHtmlParseState* state, const char* text) {
    if (state == nullptr || state->builder == nullptr || text == nullptr) {
        return;
    }
    WebHtmlBuilder* builder = state->builder;
    web::Element* parent = InsertionTarget(builder, false, std::string(), false);
    if (parent == nullptr) {
        return;
    }
    web::TextNodePtr node = builder->document->CreateComment(std::string(text));
    if (!node) {
        builder->failed = true;
        return;
    }
    ApplySpan(node.get(), state->token.begin, state->token.end, state->token.line);
    builder->document->AppendChild(*parent, std::move(node));
}

extern "C" void webhtml_builder_doctype(struct WebHtmlParseState* state, const char* text) {
    if (state == nullptr || state->builder == nullptr || text == nullptr) {
        return;
    }
    WebHtmlBuilder* builder = state->builder;
    if (builder->fragment) {
        // 片段解析不产生文档类型声明。
        return;
    }
    web::TextNodePtr node = web::CreateDoctypeTextNode(std::string(text));
    if (!node) {
        return;
    }
    ApplySpan(node.get(), state->token.begin, state->token.end, state->token.line);
    // 构建器创建时已经建立 <html>，而 DOCTYPE 出现在文档最前面，
    // 因此插入到根元素之前，保证序列化顺序为 DOCTYPE 在前。
    if (builder->root != nullptr) {
        builder->document->InsertBefore(*builder->document, std::move(node), builder->root);
    } else {
        builder->document->AppendChild(*builder->document, std::move(node));
    }
}

extern "C" void webhtml_builder_note_error(struct WebHtmlParseState* state, const char* message) {
    if (state == nullptr) {
        return;
    }
    state->error_count += 1;
    WebHtmlBuilder* builder = state->builder;
    if (builder == nullptr) {
        return;
    }
    builder->error_count += 1;
    if (builder->first_error.empty() && message != nullptr && message[0] != '\0') {
        builder->first_error = message;
    }
}

extern "C" void webhtml_builder_finish(struct WebHtmlParseState* state) {
    if (state == nullptr || state->builder == nullptr) {
        return;
    }
    WebHtmlBuilder* builder = state->builder;
    builder->finished = true;
    // 输入结束时仍未闭合的元素按文档顺序全部关闭。
    if (builder->stack.size() > 1) {
        builder->stack.resize(1);
    }
    webhtml_pending_reset(state);
}

extern "C" int webhtml_builder_failed(const struct WebHtmlBuilder* builder) {
    return builder != nullptr && builder->failed ? 1 : 0;
}

extern "C" int webhtml_builder_error_count(const struct WebHtmlBuilder* builder) {
    return builder != nullptr ? builder->error_count : 0;
}

extern "C" const char* webhtml_builder_error_message(const struct WebHtmlBuilder* builder) {
    if (builder == nullptr || builder->first_error.empty()) {
        return "";
    }
    return builder->first_error.c_str();
}

namespace web {

namespace {

/* 执行一次完整的词法与语法分析。parent 为 nullptr 时按整文档解析。 */
bool RunHtmlParser(const std::string& source, Document& document, Element* parent,
                   std::string* error) {
    // 词法器在输入末尾最多前读两个字节，追加哨兵保证读取始终落在缓冲内。
    std::string buffer = source;
    buffer.append(2, '\0');

    WebHtmlParseState state;
    std::memset(&state, 0, sizeof(state));
    state.input = buffer.data();
    state.cursor = state.input;
    state.limit = state.input + source.size();
    state.line = 1;
    state.builder = webhtml_builder_create(&document, parent);
    if (state.builder == nullptr) {
        if (error != nullptr) {
            *error = "无法创建 HTML 构建器";
        }
        return false;
    }

    const int parse_result = webhtmlparse(&state);
    std::string message = webhtml_builder_error_message(state.builder);
    const bool failed = webhtml_builder_failed(state.builder) != 0 || parse_result == 2;
    webhtml_state_reset(&state);
    webhtml_builder_destroy(state.builder);

    if (failed) {
        if (error != nullptr) {
            *error = message.empty() ? std::string("HTML 解析失败") : message;
        }
        return false;
    }
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

bool IsRawTextElement(const Node& node) {
    if (!node.IsElement()) {
        return false;
    }
    const std::string& tag = static_cast<const Element&>(node).TagName();
    return tag == "script" || tag == "style";
}

void EscapeText(const std::string& text, std::string* output) {
    for (char character : text) {
        switch (character) {
            case '&':
                output->append("&amp;");
                break;
            case '<':
                output->append("&lt;");
                break;
            case '>':
                output->append("&gt;");
                break;
            default:
                output->push_back(character);
                break;
        }
    }
}

void EscapeAttribute(const std::string& text, std::string* output) {
    for (char character : text) {
        switch (character) {
            case '&':
                output->append("&amp;");
                break;
            case '<':
                output->append("&lt;");
                break;
            case '>':
                output->append("&gt;");
                break;
            case '"':
                output->append("&quot;");
                break;
            default:
                output->push_back(character);
                break;
        }
    }
}

void SerializeNodeImpl(const Node& node, bool raw_text, std::string* output);

void SerializeChildrenImpl(const Node& node, std::string* output) {
    const bool raw_text = IsRawTextElement(node);
    for (const NodePtr& child : node.Children()) {
        if (!child) {
            continue;
        }
        SerializeNodeImpl(*child, raw_text, output);
    }
}

void SerializeNodeImpl(const Node& node, bool raw_text, std::string* output) {
    switch (node.Type()) {
        case NodeType::Document:
            SerializeChildrenImpl(node, output);
            break;
        case NodeType::Element: {
            const Element& element = static_cast<const Element&>(node);
            const std::string& tag = element.TagName();
            output->push_back('<');
            output->append(tag);
            for (const Attribute& attribute : element.Attributes()) {
                output->push_back(' ');
                output->append(attribute.name);
                output->append("=\"");
                EscapeAttribute(attribute.value, output);
                output->push_back('"');
            }
            output->push_back('>');
            if (!Element::IsVoidTag(tag)) {
                SerializeChildrenImpl(element, output);
                output->append("</");
                output->append(tag);
                output->push_back('>');
            }
            break;
        }
        case NodeType::Text: {
            const std::string& data = static_cast<const TextNode&>(node).Data();
            if (raw_text) {
                output->append(data);
            } else {
                EscapeText(data, output);
            }
            break;
        }
        case NodeType::Comment:
            output->append("<!--");
            output->append(static_cast<const TextNode&>(node).Data());
            output->append("-->");
            break;
        case NodeType::Doctype:
            output->append("<!DOCTYPE ");
            output->append(static_cast<const TextNode&>(node).Data());
            output->push_back('>');
            break;
    }
}

}  // namespace

bool ParseHtmlDocument(const std::string& source, Document& document, std::string* error) {
    document.RemoveAllChildren(document);
    return RunHtmlParser(source, document, nullptr, error);
}

bool ParseHtmlFragment(const std::string& source, Document& document, Element& parent,
                       std::string* error) {
    return RunHtmlParser(source, document, &parent, error);
}

std::string SerializeNode(const Node& node) {
    std::string output;
    SerializeNodeImpl(node, false, &output);
    return output;
}

std::string SerializeChildren(const Node& node) {
    std::string output;
    SerializeChildrenImpl(node, &output);
    return output;
}

std::string DecodeHtmlEntities(const std::string& text) {
    char* decoded = webhtml_decode_entities(text.c_str(), text.c_str() + text.size());
    if (decoded == nullptr) {
        return text;
    }
    std::string result(decoded);
    webhtml_free_string(decoded);
    return result;
}

}  // namespace web
