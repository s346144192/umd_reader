#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace web {

enum class NodeType {
    Document,
    Element,
    Text,
    Comment,
    Doctype,
};

// 源文档位置，用于后续的阅读进度映射、标注与朗读高亮。
struct SourceSpan {
    size_t begin = 0;
    size_t end = 0;
    int line = 0;

    bool Valid() const { return end >= begin && (end > begin); }
};

struct Attribute {
    std::string name;
    std::string value;
};

class Document;
class Element;
class TextNode;

using NodePtr = std::shared_ptr<class Node>;
using ElementPtr = std::shared_ptr<Element>;
using TextNodePtr = std::shared_ptr<TextNode>;

// DOM 节点基类。所有权由父节点持有（shared_ptr），脚本宿主可额外持有引用，
// 因此节点在从文档移除后仍能安全存活；Parent() 会被置空。
class Node : public std::enable_shared_from_this<Node> {
public:
    Node(const Node&) = delete;
    Node& operator=(const Node&) = delete;
    virtual ~Node();

    NodeType Type() const { return type_; }
    bool IsDocument() const { return type_ == NodeType::Document; }
    bool IsElement() const { return type_ == NodeType::Element; }
    bool IsText() const { return type_ == NodeType::Text; }
    bool IsComment() const { return type_ == NodeType::Comment; }

    Node* Parent() const { return parent_; }
    Element* ParentElement() const;
    Document* OwnerDocument() const;

    size_t ChildCount() const { return children_.size(); }
    Node* ChildAt(size_t index) const;
    const std::vector<NodePtr>& Children() const { return children_; }

    Node* PreviousSibling() const;
    Node* NextSibling() const;
    size_t IndexInParent() const;

    Element* FirstChildElement() const;
    Element* NextSiblingElement() const;

    // 源文档区间；文本节点与元素节点均记录各自在原文中的范围。
    SourceSpan span;

protected:
    explicit Node(NodeType type);

    NodeType type_;
    Node* parent_ = nullptr;
    std::vector<NodePtr> children_;

    friend class Document;
};

class TextNode final : public Node {
public:
    const std::string& Data() const { return data_; }
    void SetData(std::string data);

private:
    friend class Document;
    TextNode(std::string data, NodeType type);

    std::string data_;
};

class Element final : public Node {
public:
    const std::string& TagName() const { return tag_name_; }

    const std::vector<Attribute>& Attributes() const { return attributes_; }
    bool HasAttribute(const std::string& name) const;
    // 缺少属性时返回空字符串。
    std::string GetAttribute(const std::string& name) const;
    void SetAttribute(const std::string& name, std::string value);
    void RemoveAttribute(const std::string& name);

    std::string Id() const;
    std::string ClassName() const;
    bool HasClass(const std::string& class_name) const;
    std::vector<std::string> ClassList() const;

    // HTML 的空元素（br、img、meta、link、input、hr 等）不能包含子节点。
    static bool IsVoidTag(const std::string& tag_name);

    /* 交互状态：由窗口层在指针与焦点变化时维护，选择器匹配 :hover、:active、
       :focus 时读取。状态不参与 DOM 树结构，也不影响序列化。 */
    bool hovered = false;
    bool active = false;
    bool focused = false;

private:
    friend class Document;
    Element(std::string tag_name, Document* owner);

    std::string tag_name_;
    std::vector<Attribute> attributes_;
};

// 文档节点，同时是整棵树的根。子节点一般只有一个 <html> 元素。
class Document final : public Node {
public:
    Document();
    ~Document() override;

    Element* RootElement() const;
    Element* Head() const;
    Element* Body() const;

    // 创建游离节点，交由调用方通过 AppendChild / ReplaceChildren 挂入文档。
    ElementPtr CreateElement(std::string tag_name);
    TextNodePtr CreateTextNode(std::string data);
    TextNodePtr CreateComment(std::string data);

    void AppendChild(Node& parent, NodePtr child);
    void InsertBefore(Node& parent, NodePtr child, Node* reference);
    // 把节点从父节点摘下并返回所有权；节点不在该父节点下时返回 nullptr。
    NodePtr RemoveChild(Node& parent, Node& child);
    void ReplaceChildren(Node& parent, std::vector<NodePtr> children);
    void RemoveAllChildren(Node& parent);

    Element* GetElementById(const std::string& element_id) const;
    std::vector<Element*> GetElementsByTagName(const std::string& tag_name) const;

private:
    NodePtr Detach(Node& child);
};

// 递归拼接节点及其后代的文本内容（不含注释与脚本、样式内的文本以外内容）。
std::string TextContent(const Node& node);

// 按标签名收集后代元素，用于提取 <style>、<link>、<script> 等。
void CollectElementsByTag(const Node& root, const std::string& tag_name,
                          std::vector<Element*>* output);
Element* FindFirstElementByTag(const Node& root, const std::string& tag_name);

}  // namespace web
