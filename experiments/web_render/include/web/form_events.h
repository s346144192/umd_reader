#pragma once

#include <map>
#include <string>
#include <vector>

#include "web/dom.h"

namespace web {

// 控件激活后应派发的事件类型。
enum class ControlEventType {
    None,
    Click,   // 普通元素点击
    Change,  // checkbox / radio / select 的值变化
    Input,   // 文本输入带来的值变化
    Submit,  // 表单提交
    Reset,   // 表单重置
};

struct ControlEvent {
    ControlEventType type = ControlEventType::None;
    // 事件目标：submit/reset 为 form 元素，其余为被激活的控件本身。
    const Element* target = nullptr;
    // 是否改变了文档结构或盒尺寸（需要重新布局）。
    bool layout_changed = false;
};

// 表单默认值快照：用于 reset 恢复。键为元素地址，生命周期与文档一致。
struct FormDefaults {
    std::map<const Element*, std::string> values;
    std::map<const Element*, bool> checked;
    std::map<const Element*, int> selected_index;

    // 记录文档中所有表单控件的初始值。
    void Capture(const Document& document);
};

// 元素所属的表单（向上查找 form）；不在表单内时返回 nullptr。
Element* FindOwnerForm(Element& element);

// 执行控件的默认行为并返回应派发的事件：
// - checkbox 切换 checked；radio 选中自身并清除同 name 的其他项；
// - button[type=submit] / input[type=submit] 返回 Submit（目标为所属表单）；
// - button[type=reset] / input[type=reset] 按快照恢复表单值并返回 Reset；
// - 其余元素返回 Click。
ControlEvent ActivateControl(Element& element, const FormDefaults& defaults);

// 判断元素是否是可激活控件（按钮、复选、单选、链接、下拉、可选中的选项）。
bool IsActivatableControl(const Element& element);

// 从元素自身向上查找最近的可激活控件。命中按钮内部的 span 等子元素时，
// 默认动作应作用于该子元素所属的按钮；找不到时返回 nullptr。
Element* FindActivatableAncestor(Element& element);

// ---------------------------------------------------------------------------
// select 下拉浮层
// ---------------------------------------------------------------------------

struct SelectPopup {
    const Element* select = nullptr;
    int highlighted = 0;
    bool open = false;
};

// 选项文本：优先取 value 属性，其次取文本内容。
std::vector<std::string> SelectOptions(const Element& select);

// 当前选中项索引；没有 selected 时返回 0，无选项时返回 -1。
int SelectSelectedIndex(const Element& select);

// 第 index 个 option 元素；越界返回 nullptr。
Element* SelectOptionAt(const Element& select, int index);

// 打开浮层并高亮首次选中的项；select 为空指针或 index 小于 0 时关闭。
void ToggleSelectPopup(SelectPopup* popup, const Element* select, int index);

// 关闭浮层，把第 index 项写为 selected 并返回是否发生了变化。
bool CommitSelectPopup(SelectPopup* popup, int index);

}  // namespace web
