#include "web/dom.h"

#include <utility>

#include "web/canvas.h"
#include "web/utf8.h"

namespace web {

namespace {

// 判断 candidate 是否为 node 自身或其祖先。挂载前用它阻止形成环。
bool IsSelfOrAncestor(const Node* candidate, const Node* node) {
    for (const Node* current = node; current != nullptr; current = current->Parent()) {
        if (current == candidate) {
            return true;
        }
    }
    return false;
}

// 判断字符是否为 HTML 空白（用于属性与类名分词）。
bool IsSpaceCharacter(char character) {
    return IsAsciiSpace(character);
}

// 按空白切分类名字符串，忽略空片段。
std::vector<std::string> SplitOnSpace(const std::string& value) {
    std::vector<std::string> result;
    size_t begin = 0;
    size_t cursor = 0;
    while (cursor <= value.size()) {
        if (cursor == value.size() || IsSpaceCharacter(value[cursor])) {
            if (cursor > begin) {
                result.push_back(value.substr(begin, cursor - begin));
            }
            begin = cursor + 1;
        }
        ++cursor;
    }
    return result;
}

// 在 node 及其后代中查找指定 ID 的元素，返回文档顺序中的第一个。
Element* FindElementById(const Node& node, const std::string& element_id) {
    if (node.IsElement()) {
        Element* element = const_cast<Element*>(static_cast<const Element*>(&node));
        if (element->Id() == element_id) {
            return element;
        }
    }
    for (const NodePtr& child : node.Children()) {
        if (!child) {
            continue;
        }
        if (Element* found = FindElementById(*child, element_id)) {
            return found;
        }
    }
    return nullptr;
}

// 前序遍历 node 及其后代，收集标签名匹配的元素；wanted 为 "*" 表示全部。
void CollectElements(const Node& node, const std::string& wanted,
                     std::vector<Element*>* output) {
    if (node.IsElement()) {
        Element* element = const_cast<Element*>(static_cast<const Element*>(&node));
        if (wanted == "*" || element->TagName() == wanted) {
            output->push_back(element);
        }
    }
    for (const NodePtr& child : node.Children()) {
        if (!child) {
            continue;
        }
        CollectElements(*child, wanted, output);
    }
}

Element* FindElement(const Node& node, const std::string& wanted) {
    if (node.IsElement()) {
        Element* element = const_cast<Element*>(static_cast<const Element*>(&node));
        if (wanted == "*" || element->TagName() == wanted) {
            return element;
        }
    }
    for (const NodePtr& child : node.Children()) {
        if (!child) {
            continue;
        }
        if (Element* found = FindElement(*child, wanted)) {
            return found;
        }
    }
    return nullptr;
}

// TextNode 的构造器为私有，Document 是唯一友元，而公共头文件没有提供创建
// Doctype 文本节点的入口。为在不修改公共头文件的前提下支持 DOCTYPE 节点，
// 在 Document 的构造器中登记一个具备相同访问权限的工厂函数。
using DoctypeNodeFactory = TextNodePtr (*)(std::string);

DoctypeNodeFactory g_doctype_node_factory = nullptr;

}  // namespace

TextNodePtr CreateDoctypeTextNode(std::string data) {
    if (g_doctype_node_factory == nullptr) {
        return TextNodePtr();
    }
    return g_doctype_node_factory(std::move(data));
}

Node::Node(NodeType type) : type_(type) {}

Node::~Node() {
    // 画布注册表以元素地址为键：元素销毁前先移除表项，避免画布上下文访问失效地址。
    if (type_ == NodeType::Element) {
        ReleaseCanvasSurface(static_cast<const Element&>(*this));
    }
}

Element* Node::ParentElement() const {
    if (parent_ != nullptr && parent_->IsElement()) {
        return static_cast<Element*>(parent_);
    }
    return nullptr;
}

Document* Node::OwnerDocument() const {
    const Node* current = this;
    while (current != nullptr) {
        if (current->IsDocument()) {
            return const_cast<Document*>(static_cast<const Document*>(current));
        }
        current = current->parent_;
    }
    return nullptr;
}

Node* Node::ChildAt(size_t index) const {
    if (index >= children_.size()) {
        return nullptr;
    }
    return children_[index].get();
}

Node* Node::PreviousSibling() const {
    if (parent_ == nullptr) {
        return nullptr;
    }
    const std::vector<NodePtr>& siblings = parent_->children_;
    for (size_t index = 0; index < siblings.size(); ++index) {
        if (siblings[index].get() == this) {
            return index == 0 ? nullptr : siblings[index - 1].get();
        }
    }
    return nullptr;
}

Node* Node::NextSibling() const {
    if (parent_ == nullptr) {
        return nullptr;
    }
    const std::vector<NodePtr>& siblings = parent_->children_;
    for (size_t index = 0; index < siblings.size(); ++index) {
        if (siblings[index].get() == this) {
            return index + 1 < siblings.size() ? siblings[index + 1].get() : nullptr;
        }
    }
    return nullptr;
}

size_t Node::IndexInParent() const {
    if (parent_ == nullptr) {
        return 0;
    }
    const std::vector<NodePtr>& siblings = parent_->children_;
    for (size_t index = 0; index < siblings.size(); ++index) {
        if (siblings[index].get() == this) {
            return index;
        }
    }
    return 0;
}

Element* Node::FirstChildElement() const {
    for (const NodePtr& child : children_) {
        if (child && child->IsElement()) {
            return static_cast<Element*>(child.get());
        }
    }
    return nullptr;
}

Element* Node::NextSiblingElement() const {
    if (parent_ == nullptr) {
        return nullptr;
    }
    const std::vector<NodePtr>& siblings = parent_->children_;
    size_t index = 0;
    while (index < siblings.size() && siblings[index].get() != this) {
        ++index;
    }
    for (++index; index < siblings.size(); ++index) {
        if (siblings[index] && siblings[index]->IsElement()) {
            return static_cast<Element*>(siblings[index].get());
        }
    }
    return nullptr;
}

TextNode::TextNode(std::string data, NodeType type) : Node(type), data_(std::move(data)) {}

void TextNode::SetData(std::string data) {
    data_ = std::move(data);
}

Element::Element(std::string tag_name, Document* owner) : Node(NodeType::Element) {
    // 元素的归属文档由所在树推导，owner 仅用于保持构造器签名。
    (void)owner;
    tag_name_ = AsciiLower(std::move(tag_name));
}

bool Element::HasAttribute(const std::string& name) const {
    const std::string lowered = AsciiLower(name);
    for (const Attribute& attribute : attributes_) {
        if (attribute.name == lowered) {
            return true;
        }
    }
    return false;
}

std::string Element::GetAttribute(const std::string& name) const {
    const std::string lowered = AsciiLower(name);
    for (const Attribute& attribute : attributes_) {
        if (attribute.name == lowered) {
            return attribute.value;
        }
    }
    return std::string();
}

void Element::SetAttribute(const std::string& name, std::string value) {
    const std::string lowered = AsciiLower(name);
    for (Attribute& attribute : attributes_) {
        if (attribute.name == lowered) {
            attribute.value = std::move(value);
            return;
        }
    }
    Attribute attribute;
    attribute.name = lowered;
    attribute.value = std::move(value);
    attributes_.push_back(std::move(attribute));
}

void Element::RemoveAttribute(const std::string& name) {
    const std::string lowered = AsciiLower(name);
    for (auto iterator = attributes_.begin(); iterator != attributes_.end(); ++iterator) {
        if (iterator->name == lowered) {
            attributes_.erase(iterator);
            return;
        }
    }
}

std::string Element::Id() const {
    return GetAttribute("id");
}

std::string Element::ClassName() const {
    return GetAttribute("class");
}

std::vector<std::string> Element::ClassList() const {
    return SplitOnSpace(ClassName());
}

bool Element::HasClass(const std::string& class_name) const {
    for (const std::string& item : ClassList()) {
        if (item == class_name) {
            return true;
        }
    }
    return false;
}

bool Element::IsVoidTag(const std::string& tag_name) {
    const std::string tag = AsciiLower(tag_name);
    return tag == "area" || tag == "base" || tag == "br" || tag == "col" || tag == "embed" ||
           tag == "hr" || tag == "img" || tag == "input" || tag == "link" || tag == "meta" ||
           tag == "param" || tag == "source" || tag == "track" || tag == "wbr";
}

Document::Document() : Node(NodeType::Document) {
    // 局部类具有与所在成员函数相同的访问权限，可调用 TextNode 的私有构造器。
    struct LocalDoctypeFactory {
        static TextNodePtr Make(std::string data) {
            return TextNodePtr(new TextNode(std::move(data), NodeType::Doctype));
        }
    };
    if (g_doctype_node_factory == nullptr) {
        g_doctype_node_factory = &LocalDoctypeFactory::Make;
    }
}

Document::~Document() = default;

Element* Document::RootElement() const {
    return FirstChildElement();
}

Element* Document::Head() const {
    Element* root = RootElement();
    if (root == nullptr) {
        return nullptr;
    }
    for (const NodePtr& child : root->Children()) {
        if (child && child->IsElement()) {
            Element* element = static_cast<Element*>(child.get());
            if (element->TagName() == "head") {
                return element;
            }
        }
    }
    return nullptr;
}

Element* Document::Body() const {
    Element* root = RootElement();
    if (root == nullptr) {
        return nullptr;
    }
    for (const NodePtr& child : root->Children()) {
        if (child && child->IsElement()) {
            Element* element = static_cast<Element*>(child.get());
            if (element->TagName() == "body") {
                return element;
            }
        }
    }
    return nullptr;
}

ElementPtr Document::CreateElement(std::string tag_name) {
    return ElementPtr(new Element(std::move(tag_name), this));
}

TextNodePtr Document::CreateTextNode(std::string data) {
    return TextNodePtr(new TextNode(std::move(data), NodeType::Text));
}

TextNodePtr Document::CreateComment(std::string data) {
    return TextNodePtr(new TextNode(std::move(data), NodeType::Comment));
}

NodePtr Document::Detach(Node& child) {
    Node* parent = child.parent_;
    if (parent == nullptr) {
        return nullptr;
    }
    std::vector<NodePtr>& siblings = parent->children_;
    for (auto iterator = siblings.begin(); iterator != siblings.end(); ++iterator) {
        if (iterator->get() == &child) {
            NodePtr owned = std::move(*iterator);
            siblings.erase(iterator);
            child.parent_ = nullptr;
            return owned;
        }
    }
    // 父指针与实际子节点列表不一致时，只清理父指针并交回空结果。
    child.parent_ = nullptr;
    return nullptr;
}

void Document::AppendChild(Node& parent, NodePtr child) {
    if (!child) {
        return;
    }
    if (child.get() == &parent || child->IsDocument()) {
        return;
    }
    if (IsSelfOrAncestor(child.get(), &parent)) {
        return;
    }
    if (child->Parent() != nullptr) {
        NodePtr detached = Detach(*child);
        if (!detached) {
            // 原父节点并未真正持有该节点，放弃本次挂载以避免共享所有权歧义。
            return;
        }
    }
    parent.children_.push_back(std::move(child));
    parent.children_.back()->parent_ = &parent;
}

void Document::InsertBefore(Node& parent, NodePtr child, Node* reference) {
    if (!child) {
        return;
    }
    if (child.get() == &parent || child->IsDocument()) {
        return;
    }
    if (reference == child.get()) {
        return;
    }
    if (IsSelfOrAncestor(child.get(), &parent)) {
        return;
    }
    if (child->Parent() != nullptr) {
        NodePtr detached = Detach(*child);
        if (!detached) {
            return;
        }
    }

    size_t index = parent.children_.size();
    if (reference != nullptr && reference->Parent() == &parent) {
        for (size_t current = 0; current < parent.children_.size(); ++current) {
            if (parent.children_[current].get() == reference) {
                index = current;
                break;
            }
        }
    }
    parent.children_.insert(parent.children_.begin() + static_cast<ptrdiff_t>(index),
                            std::move(child));
    parent.children_[index]->parent_ = &parent;
}

NodePtr Document::RemoveChild(Node& parent, Node& child) {
    if (&parent == &child) {
        return nullptr;
    }
    if (child.Parent() != &parent) {
        return nullptr;
    }
    return Detach(child);
}

void Document::ReplaceChildren(Node& parent, std::vector<NodePtr> children) {
    std::vector<NodePtr> accepted;
    accepted.reserve(children.size());
    for (NodePtr& child : children) {
        if (!child) {
            continue;
        }
        if (child.get() == &parent || child->IsDocument()) {
            continue;
        }
        if (IsSelfOrAncestor(child.get(), &parent)) {
            continue;
        }
        if (child->Parent() != nullptr) {
            NodePtr detached = Detach(*child);
            if (!detached) {
                continue;
            }
        }
        accepted.push_back(std::move(child));
    }

    RemoveAllChildren(parent);
    parent.children_ = std::move(accepted);
    for (NodePtr& child : parent.children_) {
        child->parent_ = &parent;
    }
}

void Document::RemoveAllChildren(Node& parent) {
    for (NodePtr& child : parent.children_) {
        if (child) {
            child->parent_ = nullptr;
        }
    }
    parent.children_.clear();
}

Element* Document::GetElementById(const std::string& element_id) const {
    for (const NodePtr& child : children_) {
        if (!child) {
            continue;
        }
        if (Element* found = FindElementById(*child, element_id)) {
            return found;
        }
    }
    return nullptr;
}

std::vector<Element*> Document::GetElementsByTagName(const std::string& tag_name) const {
    std::vector<Element*> result;
    const std::string wanted = AsciiLower(tag_name);
    for (const NodePtr& child : children_) {
        if (!child) {
            continue;
        }
        CollectElements(*child, wanted, &result);
    }
    return result;
}

std::string TextContent(const Node& node) {
    if (node.IsText()) {
        return static_cast<const TextNode&>(node).Data();
    }
    if (node.IsComment() || node.Type() == NodeType::Doctype) {
        return std::string();
    }
    std::string result;
    for (const NodePtr& child : node.Children()) {
        if (child) {
            result += TextContent(*child);
        }
    }
    return result;
}

void CollectElementsByTag(const Node& root, const std::string& tag_name,
                          std::vector<Element*>* output) {
    if (output == nullptr) {
        return;
    }
    const std::string wanted = AsciiLower(tag_name);
    for (const NodePtr& child : root.Children()) {
        if (!child) {
            continue;
        }
        CollectElements(*child, wanted, output);
    }
}

Element* FindFirstElementByTag(const Node& root, const std::string& tag_name) {
    const std::string wanted = AsciiLower(tag_name);
    for (const NodePtr& child : root.Children()) {
        if (!child) {
            continue;
        }
        if (Element* found = FindElement(*child, wanted)) {
            return found;
        }
    }
    return nullptr;
}

}  // namespace web
