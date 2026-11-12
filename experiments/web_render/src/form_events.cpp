/* 表单控件事件与 select 下拉浮层状态的实现。
   本模块只读取或修改元素属性，不改变 DOM 结构。 */
#include "web/form_events.h"

#include <cstddef>
#include <string>
#include <vector>

#include "web/utf8.h"

namespace web {
namespace {

// 归一化类型标记：去除首尾 ASCII 空白并转换为小写。
std::string NormalizeTypeToken(const std::string& value) {
    std::string token = value;
    TrimAsciiSpace(&token);
    return AsciiLower(token);
}

// input 的 type 属性；缺省时按 HTML 语义视为 text。
std::string InputType(const Element& element) {
    const std::string type = NormalizeTypeToken(element.GetAttribute("type"));
    return type.empty() ? std::string("text") : type;
}

// button 的 type 属性；缺省时按 HTML 语义视为 submit。
std::string ButtonType(const Element& element) {
    const std::string type = NormalizeTypeToken(element.GetAttribute("type"));
    return type.empty() ? std::string("submit") : type;
}

// 按文档顺序收集后代元素。这里使用 DOM 提供的 CollectElementsByTag：
// 该接口接受 const 根节点并返回非 const 元素指针，因此恢复默认值时可以直接修改属性，
// 不需要对根元素使用 const_cast。
std::vector<Element*> DescendantElements(const Node& root, const std::string& tag_name) {
    std::vector<Element*> elements;
    CollectElementsByTag(root, tag_name, &elements);
    return elements;
}

// 记录单个元素的初始状态。
void CaptureElement(const Element& element, FormDefaults& defaults) {
    const std::string& tag_name = element.TagName();
    if (tag_name == "input") {
        // 缺少 value 属性时记录空字符串，reset 时据此移除该属性。
        defaults.values[&element] = element.GetAttribute("value");
        defaults.checked[&element] = element.HasAttribute("checked");
    } else if (tag_name == "textarea") {
        defaults.values[&element] = TextContent(element);
    } else if (tag_name == "select") {
        defaults.selected_index[&element] = SelectSelectedIndex(element);
    }
}

// 递归遍历文档子树，记录所有表单控件的初始状态。
void CaptureSubtree(const Node& node, FormDefaults& defaults) {
    for (const NodePtr& child : node.Children()) {
        if (!child) {
            continue;
        }
        if (child->IsElement()) {
            CaptureElement(static_cast<const Element&>(*child), defaults);
        }
        CaptureSubtree(*child, defaults);
    }
}

// 按快照恢复表单内 input 的 value 与 checked 属性。
void RestoreInputDefaults(Element& form, const FormDefaults& defaults) {
    const std::vector<Element*> inputs = DescendantElements(form, "input");
    for (Element* input : inputs) {
        if (input == nullptr) {
            continue;
        }
        const auto value_iterator = defaults.values.find(input);
        if (value_iterator != defaults.values.end()) {
            input->SetAttribute("value", value_iterator->second);
        } else {
            input->RemoveAttribute("value");
        }
        const auto checked_iterator = defaults.checked.find(input);
        if (checked_iterator != defaults.checked.end() && checked_iterator->second) {
            input->SetAttribute("checked", "checked");
        } else {
            input->RemoveAttribute("checked");
        }
    }
}

// 按快照恢复表单内 select 的选中项。
void RestoreSelectDefaults(Element& form, const FormDefaults& defaults) {
    const std::vector<Element*> selects = DescendantElements(form, "select");
    for (Element* select : selects) {
        if (select == nullptr) {
            continue;
        }
        const auto selected_iterator = defaults.selected_index.find(select);
        if (selected_iterator == defaults.selected_index.end()) {
            continue;
        }
        const std::vector<Element*> options = DescendantElements(*select, "option");
        for (Element* option : options) {
            if (option != nullptr) {
                option->RemoveAttribute("selected");
            }
        }
        if (selected_iterator->second >= 0 &&
            static_cast<std::size_t>(selected_iterator->second) < options.size()) {
            options[static_cast<std::size_t>(selected_iterator->second)]
                ->SetAttribute("selected", "selected");
        }
    }
}

// 清除同一表单内同名 radio 的 checked 属性，保留当前元素自身。
void ClearRadioGroup(Element& element) {
    Element* form = FindOwnerForm(element);
    if (form == nullptr) {
        return;
    }
    const std::string group_name = element.GetAttribute("name");
    const std::vector<Element*> inputs = DescendantElements(*form, "input");
    for (Element* input : inputs) {
        if (input == nullptr || input == &element) {
            continue;
        }
        if (InputType(*input) != "radio") {
            continue;
        }
        if (input->GetAttribute("name") != group_name) {
            continue;
        }
        input->RemoveAttribute("checked");
    }
}

// 提交：派发到所属表单；不在表单内时退化为普通点击。
ControlEvent SubmitControl(Element& element) {
    Element* form = FindOwnerForm(element);
    if (form != nullptr) {
        return ControlEvent{ControlEventType::Submit, form, false};
    }
    return ControlEvent{ControlEventType::Click, &element, false};
}

// 重置：按快照恢复所属表单，值变化可能影响布局。
ControlEvent ResetControl(Element& element, const FormDefaults& defaults) {
    Element* form = FindOwnerForm(element);
    if (form == nullptr) {
        return ControlEvent{ControlEventType::Click, &element, false};
    }
    RestoreInputDefaults(*form, defaults);
    RestoreSelectDefaults(*form, defaults);
    return ControlEvent{ControlEventType::Reset, form, true};
}

}  // namespace

void FormDefaults::Capture(const Document& document) {
    CaptureSubtree(document, *this);
}

Element* FindOwnerForm(Element& element) {
    Node* current = element.Parent();
    while (current != nullptr) {
        if (current->IsElement()) {
            Element* parent_element = static_cast<Element*>(current);
            if (parent_element->TagName() == "form") {
                return parent_element;
            }
        }
        current = current->Parent();
    }
    return nullptr;
}

bool IsActivatableControl(const Element& element) {
    const std::string& tag_name = element.TagName();
    if (tag_name == "a") {
        return element.HasAttribute("href");
    }
    if (tag_name == "button" || tag_name == "select" || tag_name == "option" ||
        tag_name == "summary") {
        return true;
    }
    if (tag_name == "input") {
        const std::string type = InputType(element);
        return type == "checkbox" || type == "radio" || type == "submit" || type == "reset" ||
               type == "button";
    }
    return false;
}

Element* FindActivatableAncestor(Element& element) {
    Element* current = &element;
    while (current != nullptr) {
        if (IsActivatableControl(*current) && !current->HasAttribute("disabled")) {
            return current;
        }
        current = current->ParentElement();
    }
    return nullptr;
}

ControlEvent ActivateControl(Element& element, const FormDefaults& defaults) {
    const std::string& tag_name = element.TagName();
    if (tag_name == "input") {
        const std::string type = InputType(element);
        if (type == "checkbox") {
            if (element.HasAttribute("checked")) {
                element.RemoveAttribute("checked");
            } else {
                element.SetAttribute("checked", "checked");
            }
            return ControlEvent{ControlEventType::Change, &element, false};
        }
        if (type == "radio") {
            element.SetAttribute("checked", "checked");
            ClearRadioGroup(element);
            return ControlEvent{ControlEventType::Change, &element, false};
        }
        if (type == "submit") {
            return SubmitControl(element);
        }
        if (type == "reset") {
            return ResetControl(element, defaults);
        }
    } else if (tag_name == "button") {
        const std::string type = ButtonType(element);
        if (type == "submit") {
            return SubmitControl(element);
        }
        if (type == "reset") {
            return ResetControl(element, defaults);
        }
    } else if (tag_name == "summary") {
        // summary 切换所属 details 的 open 属性；展开状态改变布局。
        Element* parent = element.ParentElement();
        if (parent != nullptr && parent->TagName() == "details") {
            if (parent->HasAttribute("open")) {
                parent->RemoveAttribute("open");
            } else {
                parent->SetAttribute("open", "open");
            }
            return ControlEvent{ControlEventType::Click, &element, true};
        }
    }
    // 链接、select、option、文本输入框以及其余元素均按普通点击处理。
    return ControlEvent{ControlEventType::Click, &element, false};
}

std::vector<std::string> SelectOptions(const Element& select) {
    const std::vector<Element*> options = DescendantElements(select, "option");
    std::vector<std::string> result;
    result.reserve(options.size());
    for (Element* option : options) {
        if (option == nullptr) {
            continue;
        }
        if (option->HasAttribute("value")) {
            result.push_back(option->GetAttribute("value"));
        } else {
            std::string text = TextContent(*option);
            TrimAsciiSpace(&text);
            result.push_back(text);
        }
    }
    return result;
}

int SelectSelectedIndex(const Element& select) {
    const std::vector<Element*> options = DescendantElements(select, "option");
    if (options.empty()) {
        return -1;
    }
    for (std::size_t index = 0; index < options.size(); ++index) {
        if (options[index] != nullptr && options[index]->HasAttribute("selected")) {
            return static_cast<int>(index);
        }
    }
    return 0;
}

Element* SelectOptionAt(const Element& select, int index) {
    if (index < 0) {
        return nullptr;
    }
    const std::vector<Element*> options = DescendantElements(select, "option");
    const std::size_t position = static_cast<std::size_t>(index);
    if (position >= options.size()) {
        return nullptr;
    }
    return options[position];
}

void ToggleSelectPopup(SelectPopup* popup, const Element* select, int index) {
    if (popup == nullptr) {
        return;
    }
    popup->select = select;
    popup->highlighted = index;
    popup->open = select != nullptr && index >= 0;
}

bool CommitSelectPopup(SelectPopup* popup, int index) {
    if (popup == nullptr || !popup->open) {
        return false;
    }
    const Element* select = popup->select;
    if (select == nullptr) {
        popup->open = false;
        return false;
    }
    Element* option = SelectOptionAt(*select, index);
    if (option == nullptr) {
        popup->open = false;
        return false;
    }
    // 选中项没有变化时只关闭浮层。
    if (SelectSelectedIndex(*select) == index) {
        popup->open = false;
        return false;
    }
    const std::vector<Element*> options = DescendantElements(*select, "option");
    for (Element* candidate : options) {
        if (candidate != nullptr) {
            candidate->RemoveAttribute("selected");
        }
    }
    option->SetAttribute("selected", "selected");
    popup->open = false;
    return true;
}

}  // namespace web
