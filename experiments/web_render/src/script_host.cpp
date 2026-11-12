#include "web/script.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "quickjs.h"
#include "web/canvas.h"
#include "web/clipboard.h"
#include "web/css.h"
#include "web/css_parser.h"
#include "web/dom.h"
#include "web/html.h"
#include "web/types.h"
#include "web/utf8.h"

namespace web {
namespace {

// 包装对象携带的数据。由 shared_ptr 管理的节点同时保存强引用，避免脚本持有
// 已从文档移除的节点时出现悬空指针；文档节点由调用方持有，只记录原始指针。
// 画布上下文包装对象额外持有绘制上下文，上下文本身只借用元素地址，
// 元素的生命周期由 owned 保证。
struct WrapperData {
    std::shared_ptr<Node> owned;
    Node* raw = nullptr;
    std::unique_ptr<CanvasRenderingContext2D> canvas_context;

    Node* Get() const { return owned ? owned.get() : raw; }
};

// 渐变包装对象：持有共享的渐变定义，addColorStop 直接修改该对象。
struct GradientWrapperData {
    std::shared_ptr<CanvasGradient> gradient;
};

// 图案包装对象：持有共享的图案定义，可赋给 fillStyle 或 strokeStyle。
struct PatternWrapperData {
    std::shared_ptr<CanvasPattern> pattern;
};

// 画布调试输出：仅在设置 WEB_CANVAS_DEBUG 环境变量时启用。
bool CanvasDebugEnabled() {
    static const bool enabled = []() {
        const char* value = std::getenv("WEB_CANVAS_DEBUG");
        return value != nullptr && value[0] != '\0';
    }();
    return enabled;
}

void ReportCanvasDebug(const char* stage, const CanvasSurface& surface) {
    if (!CanvasDebugEnabled()) {
        return;
    }
    std::fprintf(stderr, "[canvas] %s：%dx%d，revision %llu\n", stage, surface.width,
                 surface.height, static_cast<unsigned long long>(surface.revision));
}

void ReportCanvasContextDebug(const char* stage, CanvasRenderingContext2D* context) {
    if (!CanvasDebugEnabled() || context == nullptr) {
        return;
    }
    const CanvasSurface* surface = context->Surface();
    if (surface != nullptr) {
        ReportCanvasDebug(stage, *surface);
    }
}

// style 对象支持的属性：JS 属性名与 CSS 属性名一一对应，magic 即数组下标。
struct StyleBinding {
    const char* script_name;
    const char* css_name;
};

const StyleBinding kStyleBindings[] = {
    {"color", "color"},
    {"backgroundColor", "background-color"},
    {"fontSize", "font-size"},
    {"fontFamily", "font-family"},
    {"fontWeight", "font-weight"},
    {"fontStyle", "font-style"},
    {"lineHeight", "line-height"},
    {"display", "display"},
    {"textAlign", "text-align"},
    {"whiteSpace", "white-space"},
    {"width", "width"},
    {"height", "height"},
    {"margin", "margin"},
    {"marginTop", "margin-top"},
    {"marginRight", "margin-right"},
    {"marginBottom", "margin-bottom"},
    {"marginLeft", "margin-left"},
    {"padding", "padding"},
    {"paddingTop", "padding-top"},
    {"paddingRight", "padding-right"},
    {"paddingBottom", "padding-bottom"},
    {"paddingLeft", "padding-left"},
    {"border", "border"},
    {"borderWidth", "border-width"},
    {"borderColor", "border-color"},
    {"borderRadius", "border-radius"},
    {"letterSpacing", "letter-spacing"},
    {"listStyleType", "list-style-type"},
    {"textDecoration", "text-decoration"},
};

const int kStyleBindingCount = static_cast<int>(sizeof(kStyleBindings) / sizeof(kStyleBindings[0]));

std::string TrimSpace(const std::string& text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
        --end;
    }
    return text.substr(begin, end - begin);
}

// CSS 数字输出：使用最短的可用表示，不补齐小数位。
std::string FormatCssNumber(float value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%g", static_cast<double>(value));
    return std::string(buffer);
}

std::string QuoteCssString(const std::string& text) {
    std::string result;
    result.push_back('"');
    for (char character : text) {
        if (character == '\\' || character == '"') {
            result.push_back('\\');
        }
        result.push_back(character);
    }
    result.push_back('"');
    return result;
}

std::string SerializeCssLength(const CssLength& length) {
    if (length.unit == LengthUnit::Auto) {
        return "auto";
    }
    std::string text = FormatCssNumber(length.value);
    switch (length.unit) {
    case LengthUnit::Px:
        text += "px";
        break;
    case LengthUnit::Em:
        text += "em";
        break;
    case LengthUnit::Ex:
        text += "ex";
        break;
    case LengthUnit::Rem:
        text += "rem";
        break;
    case LengthUnit::Percent:
        text += "%";
        break;
    case LengthUnit::Pt:
        text += "pt";
        break;
    case LengthUnit::Pc:
        text += "pc";
        break;
    case LengthUnit::In:
        text += "in";
        break;
    case LengthUnit::Cm:
        text += "cm";
        break;
    case LengthUnit::Mm:
        text += "mm";
        break;
    case LengthUnit::Ch:
        text += "ch";
        break;
    case LengthUnit::Vw:
        text += "vw";
        break;
    case LengthUnit::Vh:
        text += "vh";
        break;
    case LengthUnit::Vmin:
        text += "vmin";
        break;
    case LengthUnit::Vmax:
        text += "vmax";
        break;
    case LengthUnit::Number:
    case LengthUnit::Auto:
        break;
    }
    return text;
}

std::string SerializeCssColor(const Color& color) {
    int red = static_cast<int>(std::lround(color.r * 255.0f));
    int green = static_cast<int>(std::lround(color.g * 255.0f));
    int blue = static_cast<int>(std::lround(color.b * 255.0f));
    red = red < 0 ? 0 : (red > 255 ? 255 : red);
    green = green < 0 ? 0 : (green > 255 ? 255 : green);
    blue = blue < 0 ? 0 : (blue > 255 ? 255 : blue);
    if (color.a >= 1.0f) {
        char buffer[16];
        std::snprintf(buffer, sizeof(buffer), "#%02x%02x%02x", red, green, blue);
        return std::string(buffer);
    }
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "rgba(%d, %d, %d, %s)", red, green, blue,
                  FormatCssNumber(color.a).c_str());
    return std::string(buffer);
}

// 把解析后的值还原为 CSS 文本。列表项默认以空格分隔；font-family 需要逗号。
std::string SerializeCssValue(const CssValue& value, const std::string& list_separator) {
    switch (value.kind) {
    case ValueKind::Keyword:
        return value.keyword;
    case ValueKind::Length:
        return SerializeCssLength(value.length);
    case ValueKind::String:
        return QuoteCssString(value.string_value);
    case ValueKind::Color:
        return SerializeCssColor(value.color.color);
    case ValueKind::Url:
        return "url(" + value.string_value + ")";
    case ValueKind::List: {
        std::string result;
        for (size_t index = 0; index < value.items.size(); ++index) {
            if (index != 0) {
                result += list_separator;
            }
            result += SerializeCssValue(value.items[index], list_separator);
        }
        return result;
    }
    }
    return std::string();
}

std::string StyleListSeparator(const std::string& css_name) {
    return css_name == "font-family" ? std::string(", ") : std::string(" ");
}

// 读取元素行内 style 中的单个属性：用 ParseDeclarationBlock 解析现有文本，
// 取同名的最后一条声明（CSS 层叠规则）。
std::string ReadStyleProperty(const Element& element, const std::string& css_name) {
    std::vector<Declaration> declarations;
    std::string error;
    ParseDeclarationBlock(element.GetAttribute("style"), &declarations, &error);
    for (size_t index = declarations.size(); index > 0; --index) {
        const Declaration& declaration = declarations[index - 1];
        if (declaration.property == css_name) {
            return SerializeCssValue(declaration.value, StyleListSeparator(css_name));
        }
    }
    return std::string();
}

struct StyleEntry {
    std::string property;
    std::string value;
    bool important = false;
};

// 写回元素行内 style：保留其它属性，同名的第一条声明替换为新值，空值表示删除。
void WriteStyleProperty(Element& element, const std::string& css_name,
                        const std::string& raw_value) {
    std::vector<Declaration> declarations;
    std::string error;
    ParseDeclarationBlock(element.GetAttribute("style"), &declarations, &error);

    const std::string value = TrimSpace(raw_value);
    std::vector<StyleEntry> entries;
    entries.reserve(declarations.size() + 1);

    bool replaced = false;
    for (const Declaration& declaration : declarations) {
        if (declaration.property == css_name) {
            if (replaced) {
                continue;
            }
            replaced = true;
            if (value.empty()) {
                continue;
            }
            StyleEntry entry;
            entry.property = css_name;
            entry.value = value;
            entry.important = false;
            entries.push_back(std::move(entry));
            continue;
        }
        StyleEntry entry;
        entry.property = declaration.property;
        entry.value = SerializeCssValue(declaration.value, StyleListSeparator(declaration.property));
        entry.important = declaration.important;
        entries.push_back(std::move(entry));
    }

    if (!replaced && !value.empty()) {
        StyleEntry entry;
        entry.property = css_name;
        entry.value = value;
        entries.push_back(std::move(entry));
    }

    std::string rebuilt;
    for (size_t index = 0; index < entries.size(); ++index) {
        if (index != 0) {
            rebuilt += "; ";
        }
        rebuilt += entries[index].property + ": " + entries[index].value;
        if (entries[index].important) {
            rebuilt += " !important";
        }
    }
    element.SetAttribute("style", rebuilt);
}

// 解析单个选择器到查询列表；语法错误时返回 false。
bool ParseSingleSelector(const std::string& text, std::vector<Selector>* selectors) {
    Selector selector;
    std::string error;
    if (!ParseSelector(text, &selector, &error)) {
        return false;
    }
    selectors->push_back(std::move(selector));
    return true;
}

using JSGetter = JSValue (*)(JSContext* ctx, JSValueConst this_val, int magic);
using JSSetter = JSValue (*)(JSContext* ctx, JSValueConst this_val, JSValueConst value, int magic);

class ScriptHostImpl final : public ScriptHost {
public:
    ScriptHostImpl(Document* document, ScriptHostOptions options)
        : document_(document), options_(std::move(options)) {}

    ~ScriptHostImpl() override { Shutdown(); }

    bool Initialize() {
        runtime_ = JS_NewRuntime();
        if (runtime_ == nullptr) {
            return false;
        }
        // 渲染外部网页时的资源上限：限制 JS 堆与原生栈，并给每次脚本执行设置
        // 时间预算，避免死循环或超大分配拖垮进程。
        JS_SetMemoryLimit(runtime_, 256u * 1024u * 1024u);
        JS_SetMaxStackSize(runtime_, 1024u * 1024u);
        JS_SetInterruptHandler(runtime_, &ScriptHostImpl::CheckInterrupt, this);
        ctx_ = JS_NewContext(runtime_);
        if (ctx_ == nullptr) {
            return false;
        }
        JS_SetContextOpaque(ctx_, this);

        node_class_id_ = JS_NewClassID(runtime_, &node_class_counter_);
        JSClassDef node_class{};
        node_class.class_name = "WebDOMNode";
        node_class.finalizer = &ScriptHostImpl::FinalizeWrapper;
        if (JS_NewClass(runtime_, node_class_id_, &node_class) < 0) {
            return false;
        }

        style_class_id_ = JS_NewClassID(runtime_, &style_class_counter_);
        JSClassDef style_class{};
        style_class.class_name = "WebCSSStyleDeclaration";
        style_class.finalizer = &ScriptHostImpl::FinalizeWrapper;
        if (JS_NewClass(runtime_, style_class_id_, &style_class) < 0) {
            return false;
        }

        canvas_class_id_ = JS_NewClassID(runtime_, &canvas_class_counter_);
        JSClassDef canvas_class{};
        canvas_class.class_name = "WebCanvasRenderingContext2D";
        canvas_class.finalizer = &ScriptHostImpl::FinalizeWrapper;
        if (JS_NewClass(runtime_, canvas_class_id_, &canvas_class) < 0) {
            return false;
        }

        gradient_class_id_ = JS_NewClassID(runtime_, &gradient_class_counter_);
        JSClassDef gradient_class{};
        gradient_class.class_name = "CanvasGradient";
        gradient_class.finalizer = &ScriptHostImpl::FinalizeGradient;
        if (JS_NewClass(runtime_, gradient_class_id_, &gradient_class) < 0) {
            return false;
        }

        pattern_class_id_ = JS_NewClassID(runtime_, &pattern_class_counter_);
        JSClassDef pattern_class{};
        pattern_class.class_name = "CanvasPattern";
        pattern_class.finalizer = &ScriptHostImpl::FinalizePattern;
        if (JS_NewClass(runtime_, pattern_class_id_, &pattern_class) < 0) {
            return false;
        }

        if (!CreatePrototypes()) {
            return false;
        }
        if (!InstallGlobals()) {
            return false;
        }
        return true;
    }

    bool Evaluate(const std::string& source, std::string* exception) override {
        if (ctx_ == nullptr) {
            if (exception != nullptr) {
                *exception = "JavaScript 宿主尚未初始化";
            }
            return false;
        }
        ExecutionGuard guard(this);
        JSValue result = JS_Eval(ctx_, source.c_str(), source.size(), "<script>",
                                 JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(result)) {
            if (exception != nullptr) {
                *exception = TakeExceptionText();
            } else {
                JS_FreeValue(ctx_, JS_GetException(ctx_));
            }
            JS_FreeValue(ctx_, result);
            DrainJobs("Promise 作业抛出异常");
            return false;
        }
        JS_FreeValue(ctx_, result);
        DrainJobs("Promise 作业抛出异常");
        if (exception != nullptr) {
            exception->clear();
        }
        return true;
    }

    void DispatchClick(const Element* target, float client_x, float client_y) override {
        ScriptEventInit init;
        init.client_x = client_x;
        init.client_y = client_y;
        DispatchEvent(target, "click", init);
    }

    bool DispatchEvent(const Element* target, const std::string& type,
                       const ScriptEventInit& init) override {
        if (target == nullptr) {
            return false;
        }
        return DispatchNodeEvent(const_cast<Element*>(target), type, init);
    }

    void DispatchDocumentEvent(const std::string& type) override {
        // DOMContentLoaded 对应文档的 interactive 阶段：事件处理器执行期间
        // readyState 为 interactive，处理完成后进入 complete。
        if (type == "DOMContentLoaded") {
            ready_state_ = "interactive";
            DispatchNodeEvent(document_, type, ScriptEventInit{});
            ready_state_ = "complete";
            return;
        }
        DispatchNodeEvent(document_, type, ScriptEventInit{});
    }

    // 事件派发主体：目标可以是元素或文档节点，冒泡链为目标到根，最后通知
    // window 上的监听器。返回值表示页面是否调用了 preventDefault。
    bool DispatchNodeEvent(Node* node, const std::string& type, const ScriptEventInit& init) {
        if (ctx_ == nullptr || node == nullptr || type.empty()) {
            return false;
        }
        ExecutionGuard guard(this);
        default_prevented_ = false;
        propagation_stopped_ = false;
        JSValue event = JS_NewObject(ctx_);
        if (JS_IsException(event)) {
            JS_FreeValue(ctx_, event);
            return false;
        }
        JS_SetPropertyStr(ctx_, event, "type", JS_NewString(ctx_, type.c_str()));
        JS_SetPropertyStr(ctx_, event, "clientX", JS_NewFloat64(ctx_, init.client_x));
        JS_SetPropertyStr(ctx_, event, "clientY", JS_NewFloat64(ctx_, init.client_y));
        if (!init.key.empty()) {
            JS_SetPropertyStr(ctx_, event, "key", JS_NewString(ctx_, init.key.c_str()));
        }
        if (!init.code.empty()) {
            JS_SetPropertyStr(ctx_, event, "code", JS_NewString(ctx_, init.code.c_str()));
        }
        JS_SetPropertyStr(ctx_, event, "shiftKey", JS_NewBool(ctx_, init.shift_key));
        JS_SetPropertyStr(ctx_, event, "ctrlKey", JS_NewBool(ctx_, init.ctrl_key));
        JS_SetPropertyStr(ctx_, event, "altKey", JS_NewBool(ctx_, init.alt_key));
        JS_SetPropertyStr(ctx_, event, "metaKey", JS_NewBool(ctx_, init.meta_key));
        JS_SetPropertyStr(ctx_, event, "button", JS_NewInt32(ctx_, init.button));
        JS_SetPropertyStr(ctx_, event, "deltaY", JS_NewFloat64(ctx_, init.delta_y));
        JS_SetPropertyStr(ctx_, event, "preventDefault",
                          JS_NewCFunction(ctx_, &ScriptHostImpl::PreventDefault,
                                          "preventDefault", 0));
        JS_SetPropertyStr(ctx_, event, "stopPropagation",
                          JS_NewCFunction(ctx_, &ScriptHostImpl::StopPropagation,
                                          "stopPropagation", 0));

        JSValue target_wrapper = WrapperFor(node);
        if (JS_IsException(target_wrapper)) {
            JS_FreeValue(ctx_, event);
            return false;
        }
        JS_SetPropertyStr(ctx_, event, "target", target_wrapper);

        JSValue event_argument = JS_DupValue(ctx_, event);
        // 事件处理器可能改动 DOM：先把目标到根的祖先链固定下来并建立包装对象，
        // 派发过程中不再读取父指针，节点也由包装对象保持存活。
        std::vector<Node*> path;
        for (Node* current = node; current != nullptr; current = current->Parent()) {
            path.push_back(current);
        }
        std::vector<JSValue> path_targets;
        path_targets.reserve(path.size());
        bool path_ready = true;
        for (Node* current : path) {
            JSValue wrapper = WrapperFor(current);
            if (JS_IsException(wrapper)) {
                JS_FreeValue(ctx_, wrapper);
                path_ready = false;
                break;
            }
            path_targets.push_back(wrapper);
        }
        if (path_ready) {
            for (size_t index = 0; index < path.size(); ++index) {
                Node* current = path[index];
                const JSValue current_target = path_targets[index];
                // 先复制当前节点的事件监听器：内联处理器可能在回调中增删监听器，
                // 复制后调用不会受监听器表重排影响。
                std::vector<JSValue> snapshot;
                auto group = listeners_.find(current);
                if (group != listeners_.end()) {
                    for (const ListenerBinding& binding : group->second.listeners) {
                        if (binding.type == type) {
                            snapshot.push_back(JS_DupValue(ctx_, binding.function));
                        }
                    }
                }
                // 内联 onclick 属性与 element.onclick 赋值的处理器先于监听器执行，
                // 与浏览器在目标节点上按注册顺序触发的结果一致。
                if (current->IsElement()) {
                    JS_SetPropertyStr(ctx_, event, "currentTarget",
                                      JS_DupValue(ctx_, current_target));
                    CallEventHandler(current, type, current_target, event_argument);
                }
                if (!snapshot.empty()) {
                    JS_SetPropertyStr(ctx_, event, "currentTarget",
                                      JS_DupValue(ctx_, current_target));
                    for (JSValue function : snapshot) {
                        JSValue result =
                            JS_Call(ctx_, function, current_target, 1, &event_argument);
                        if (JS_IsException(result)) {
                            ReportException(ctx_, "事件监听器抛出异常");
                        }
                        JS_FreeValue(ctx_, result);
                        if (propagation_stopped_) {
                            break;
                        }
                    }
                    for (JSValue function : snapshot) {
                        JS_FreeValue(ctx_, function);
                    }
                }
                if (propagation_stopped_) {
                    break;
                }
            }
        }
        for (JSValue wrapper : path_targets) {
            JS_FreeValue(ctx_, wrapper);
        }

        // 冒泡到 window：window 即全局对象，指针移动、抬起等事件在窗口层派发后
        // 由这里通知注册在 window 上的监听器。stopPropagation 阻止继续向上传播。
        std::vector<JSValue> window_snapshot;
        if (!propagation_stopped_) {
            for (const WindowListener& listener : window_listeners_) {
                if (listener.type == type) {
                    window_snapshot.push_back(JS_DupValue(ctx_, listener.function));
                }
            }
        }
        if (!window_snapshot.empty()) {
            JS_SetPropertyStr(ctx_, event, "currentTarget", JS_DupValue(ctx_, global_));
            for (JSValue function : window_snapshot) {
                JSValue result = JS_Call(ctx_, function, global_, 1, &event_argument);
                if (JS_IsException(result)) {
                    ReportException(ctx_, "window 事件监听器抛出异常");
                }
                JS_FreeValue(ctx_, result);
                if (propagation_stopped_) {
                    break;
                }
            }
            for (JSValue function : window_snapshot) {
                JS_FreeValue(ctx_, function);
            }
        }
        const bool prevented = default_prevented_;
        JS_FreeValue(ctx_, event_argument);
        JS_FreeValue(ctx_, event);
        // 事件处理器中排队的 Promise 作业在派发结束后立即执行。
        DrainJobs("Promise 作业抛出异常");
        return prevented;
    }

    void AdvanceTime(double delta_ms) override {
        if (ctx_ == nullptr) {
            return;
        }
        ExecutionGuard guard(this);
        double elapsed = delta_ms;
        if (!(elapsed >= 0.0)) {
            elapsed = 0.0;
        }
        clock_ms_ += elapsed;

        for (Timer& timer : timers_) {
            timer.remaining_ms -= elapsed;
        }
        std::vector<JSValue> due;
        for (size_t index = 0; index < timers_.size();) {
            Timer& timer = timers_[index];
            if (timer.remaining_ms <= 0.0) {
                due.push_back(JS_DupValue(ctx_, timer.function));
                if (timer.interval_ms > 0.0) {
                    // 重复定时器保留在队列中，按固定间隔继续排队。
                    timer.remaining_ms += timer.interval_ms;
                    if (timer.remaining_ms <= 0.0) {
                        timer.remaining_ms = timer.interval_ms;
                    }
                    ++index;
                } else {
                    JS_FreeValue(ctx_, timer.function);
                    timers_.erase(timers_.begin() + static_cast<ptrdiff_t>(index));
                }
            } else {
                ++index;
            }
        }
        for (JSValue function : due) {
            JSValue result = JS_Call(ctx_, function, JS_UNDEFINED, 0, nullptr);
            if (JS_IsException(result)) {
                ReportException(ctx_, "setTimeout 回调抛出异常");
            }
            JS_FreeValue(ctx_, result);
            JS_FreeValue(ctx_, function);
            DrainJobs("Promise 作业抛出异常");
        }

        if (!animation_frames_.empty()) {
            // 先移出当前帧的回调；回调内新注册的请求留到下一帧执行。
            std::vector<JSValue> frame;
            frame.swap(animation_frames_);
            for (JSValue function : frame) {
                JSValue argument = JS_NewFloat64(ctx_, clock_ms_);
                JSValue result = JS_Call(ctx_, function, JS_UNDEFINED, 1, &argument);
                if (JS_IsException(result)) {
                    ReportException(ctx_, "requestAnimationFrame 回调抛出异常");
                }
                JS_FreeValue(ctx_, result);
                JS_FreeValue(ctx_, argument);
                JS_FreeValue(ctx_, function);
                DrainJobs("Promise 作业抛出异常");
            }
        }
    }

    bool HasPendingTimers() const override { return !timers_.empty(); }

    bool HasPendingAnimationFrame() const override { return !animation_frames_.empty(); }

    void SetViewport(float width, float height) override {
        viewport_width_ = width;
        viewport_height_ = height;
        if (ctx_ == nullptr) {
            return;
        }
        JS_SetPropertyStr(ctx_, global_, "innerWidth", JS_NewFloat64(ctx_, width));
        JS_SetPropertyStr(ctx_, global_, "innerHeight", JS_NewFloat64(ctx_, height));
    }

private:
    // 单次脚本执行（含事件派发与定时器回调）的时间预算。
    static constexpr int kExecutionBudgetMs = 2000;

    // 进入脚本执行时启动计时，离开时撤销；嵌套调用只在外层生效。
    struct ExecutionGuard {
        ScriptHostImpl* host = nullptr;

        explicit ExecutionGuard(ScriptHostImpl* value) : host(value) {
            host->BeginExecution();
        }
        ~ExecutionGuard() { host->EndExecution(); }

        ExecutionGuard(const ExecutionGuard&) = delete;
        ExecutionGuard& operator=(const ExecutionGuard&) = delete;
    };

    void BeginExecution() {
        if (execution_depth_++ == 0) {
            deadline_ = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(kExecutionBudgetMs);
            deadline_active_ = true;
        }
    }

    void EndExecution() {
        if (--execution_depth_ == 0) {
            deadline_active_ = false;
        }
    }

    static int CheckInterrupt(JSContext*, void* opaque) {
        auto* host = static_cast<ScriptHostImpl*>(opaque);
        if (host == nullptr || !host->deadline_active_) {
            return 0;
        }
        return std::chrono::steady_clock::now() > host->deadline_ ? 1 : 0;
    }

    // 执行已排队的 Promise 作业（微任务）。每次脚本执行、事件派发与定时器
    // 回调结束后调用，保证 then/catch/finally 与 await 之后的代码得到运行。
    // 作业自身继续排队的作业在同一轮内反复执行，直到队列为空。
    void DrainJobs(const char* label) {
        if (runtime_ == nullptr) {
            return;
        }
        for (int round = 0; round < 10000; ++round) {
            JSContext* job_context = nullptr;
            const int result = JS_ExecutePendingJob(runtime_, &job_context);
            if (result == 0) {
                break;
            }
            if (result < 0) {
                if (job_context != nullptr) {
                    ReportException(job_context, label);
                }
                break;
            }
        }
    }

    struct Timer {
        uint32_t id = 0;
        double remaining_ms = 0.0;
        // 重复定时器的固定间隔；0 表示一次性定时器。
        double interval_ms = 0.0;
        JSValue function = JS_UNDEFINED;
    };

    struct ListenerBinding {
        std::string type;
        JSValue function = JS_UNDEFINED;
    };

    struct ListenerGroup {
        // 与包装对象共享节点所有权，保证监听器表中的 Node* 始终有效。
        std::shared_ptr<Node> keep_alive;
        std::vector<ListenerBinding> listeners;
    };

    // 单个事件类型的处理器：脚本赋值的函数（element.onclick = fn）与内联属性
    // （onclick="..."）共用一个槽位。属性文本在派发时编译并缓存，文本变化时重新编译。
    struct EventHandlerSlot {
        std::string attribute_source;
        JSValue compiled = JS_UNDEFINED;
        JSValue assigned = JS_UNDEFINED;
    };

    // window 上的事件监听器：window 即全局对象，这些监听器在事件冒泡结束后触发。
    struct WindowListener {
        std::string type;
        JSValue function = JS_UNDEFINED;
    };

    // onclick、onchange 等事件处理器属性对应的类型名表。
    static const char* const* EventHandlerTypes(size_t* count) {
        static const char* const kTypes[] = {
            "click",     "change",      "input",     "submit",    "reset",
            "focus",     "blur",        "keydown",   "keyup",     "keypress",
            "mousedown", "mouseup",     "mousemove", "dblclick",  "load",
            "scroll",    "select",      "contextmenu", "pointerdown", "pointermove",
            "pointerup", "pointercancel", "wheel",
        };
        *count = sizeof(kTypes) / sizeof(kTypes[0]);
        return kTypes;
    }

    static ScriptHostImpl* HostOf(JSContext* ctx) {
        return static_cast<ScriptHostImpl*>(JS_GetContextOpaque(ctx));
    }

    static WrapperData* WrapperOf(JSContext* ctx, JSValueConst value, JSClassID class_id) {
        (void)ctx;
        return static_cast<WrapperData*>(JS_GetOpaque(value, class_id));
    }

    static Node* NodeOf(JSContext* ctx, JSValueConst value) {
        ScriptHostImpl* host = HostOf(ctx);
        if (host == nullptr) {
            return nullptr;
        }
        WrapperData* data = WrapperOf(ctx, value, host->node_class_id_);
        return data != nullptr ? data->Get() : nullptr;
    }

    static Element* ElementOf(JSContext* ctx, JSValueConst value) {
        Node* node = NodeOf(ctx, value);
        if (node == nullptr || !node->IsElement()) {
            return nullptr;
        }
        return static_cast<Element*>(node);
    }

    static Element* StyleElementOf(JSContext* ctx, JSValueConst value) {
        ScriptHostImpl* host = HostOf(ctx);
        if (host == nullptr) {
            return nullptr;
        }
        WrapperData* data = WrapperOf(ctx, value, host->style_class_id_);
        if (data == nullptr) {
            return nullptr;
        }
        Node* node = data->Get();
        if (node == nullptr || !node->IsElement()) {
            return nullptr;
        }
        return static_cast<Element*>(node);
    }

    static CanvasRenderingContext2D* CanvasContextOf(JSContext* ctx, JSValueConst value) {
        ScriptHostImpl* host = HostOf(ctx);
        if (host == nullptr) {
            return nullptr;
        }
        WrapperData* data = WrapperOf(ctx, value, host->canvas_class_id_);
        if (data == nullptr || !data->canvas_context) {
            return nullptr;
        }
        return data->canvas_context.get();
    }

    static GradientWrapperData* GradientWrapperOf(JSContext* ctx, JSValueConst value) {
        ScriptHostImpl* host = HostOf(ctx);
        if (host == nullptr) {
            return nullptr;
        }
        return static_cast<GradientWrapperData*>(
            JS_GetOpaque(value, host->gradient_class_id_));
    }

    static PatternWrapperData* PatternWrapperOf(JSContext* ctx, JSValueConst value) {
        ScriptHostImpl* host = HostOf(ctx);
        if (host == nullptr) {
            return nullptr;
        }
        return static_cast<PatternWrapperData*>(
            JS_GetOpaque(value, host->pattern_class_id_));
    }

    static CanvasGradient* GradientOf(JSContext* ctx, JSValueConst value) {
        GradientWrapperData* data = GradientWrapperOf(ctx, value);
        return data != nullptr ? data->gradient.get() : nullptr;
    }

    // 读取数值参数；失败时异常已由 QuickJS 设置。
    static bool ReadNumber(JSContext* ctx, JSValueConst value, float* output) {
        double number = 0.0;
        if (JS_ToFloat64(ctx, &number, value) < 0) {
            return false;
        }
        *output = static_cast<float>(number);
        return true;
    }

    // 依次读取 count 个数值参数；任一参数不是数值时返回 false。
    static bool ReadNumbers(JSContext* ctx, JSValueConst* argv, int count, float* values) {
        for (int index = 0; index < count; ++index) {
            if (!ReadNumber(ctx, argv[index], &values[index])) {
                return false;
            }
        }
        return true;
    }

    static JSValue MakeMeasureText(JSContext* ctx, float width) {
        JSValue result = JS_NewObject(ctx);
        if (JS_IsException(result)) {
            return result;
        }
        JS_SetPropertyStr(ctx, result, "width", JS_NewFloat64(ctx, width));
        return result;
    }

    // 事件对象的 preventDefault / stopPropagation。默认动作（翻页、文本输入等）
    // 由宿主在派发结束后按标记决定是否执行。
    static JSValue PreventDefault(JSContext* ctx, JSValueConst, int, JSValueConst*) {
        ScriptHostImpl* host = HostOf(ctx);
        if (host != nullptr) {
            host->default_prevented_ = true;
        }
        return JS_UNDEFINED;
    }

    static JSValue StopPropagation(JSContext* ctx, JSValueConst, int, JSValueConst*) {
        ScriptHostImpl* host = HostOf(ctx);
        if (host != nullptr) {
            host->propagation_stopped_ = true;
        }
        return JS_UNDEFINED;
    }

    // localStorage 持久化使用十六进制文本，避免值中出现换行或分隔符。
    static std::string HexEncode(const std::string& text) {
        static const char kDigits[] = "0123456789abcdef";
        std::string output;
        output.reserve(text.size() * 2);
        for (unsigned char byte : text) {
            output.push_back(kDigits[byte >> 4]);
            output.push_back(kDigits[byte & 0x0F]);
        }
        return output;
    }

    static std::string HexDecode(const std::string& text) {
        std::string output;
        output.reserve(text.size() / 2);
        auto value_of = [](char character) -> int {
            if (character >= '0' && character <= '9') {
                return character - '0';
            }
            if (character >= 'a' && character <= 'f') {
                return character - 'a' + 10;
            }
            if (character >= 'A' && character <= 'F') {
                return character - 'A' + 10;
            }
            return -1;
        };
        for (size_t index = 0; index + 1 < text.size(); index += 2) {
            const int high = value_of(text[index]);
            const int low = value_of(text[index + 1]);
            if (high < 0 || low < 0) {
                continue;
            }
            output.push_back(static_cast<char>(high * 16 + low));
        }
        return output;
    }

    static JSValue StorageGetItem(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
        ScriptHostImpl* host = HostOf(ctx);
        if (host == nullptr || argc < 1) {
            return JS_NULL;
        }
        const char* key = JS_ToCString(ctx, argv[0]);
        if (key == nullptr) {
            return JS_EXCEPTION;
        }
        const auto found = host->storage_.find(key);
        JS_FreeCString(ctx, key);
        if (found == host->storage_.end()) {
            return JS_NULL;
        }
        return JS_NewString(ctx, found->second.c_str());
    }

    static JSValue StorageSetItem(JSContext* ctx, JSValueConst this_val, int argc,
                                  JSValueConst* argv) {
        ScriptHostImpl* host = HostOf(ctx);
        if (host == nullptr || argc < 2) {
            return JS_UNDEFINED;
        }
        const char* key = JS_ToCString(ctx, argv[0]);
        if (key == nullptr) {
            return JS_EXCEPTION;
        }
        const char* value = JS_ToCString(ctx, argv[1]);
        if (value == nullptr) {
            JS_FreeCString(ctx, key);
            return JS_EXCEPTION;
        }
        host->storage_[key] = value;
        JS_FreeCString(ctx, key);
        JS_FreeCString(ctx, value);
        host->SaveLocalStorage();
        JS_SetPropertyStr(ctx, this_val, "length",
                          JS_NewInt32(ctx, static_cast<int32_t>(host->storage_.size())));
        return JS_UNDEFINED;
    }

    static JSValue StorageRemoveItem(JSContext* ctx, JSValueConst this_val, int argc,
                                     JSValueConst* argv) {
        ScriptHostImpl* host = HostOf(ctx);
        if (host == nullptr || argc < 1) {
            return JS_UNDEFINED;
        }
        const char* key = JS_ToCString(ctx, argv[0]);
        if (key == nullptr) {
            return JS_EXCEPTION;
        }
        host->storage_.erase(key);
        JS_FreeCString(ctx, key);
        host->SaveLocalStorage();
        JS_SetPropertyStr(ctx, this_val, "length",
                          JS_NewInt32(ctx, static_cast<int32_t>(host->storage_.size())));
        return JS_UNDEFINED;
    }

    static JSValue StorageClear(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
        ScriptHostImpl* host = HostOf(ctx);
        if (host == nullptr) {
            return JS_UNDEFINED;
        }
        host->storage_.clear();
        host->SaveLocalStorage();
        JS_SetPropertyStr(ctx, this_val, "length", JS_NewInt32(ctx, 0));
        return JS_UNDEFINED;
    }

    static JSValue StorageKey(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
        ScriptHostImpl* host = HostOf(ctx);
        if (host == nullptr || argc < 1) {
            return JS_NULL;
        }
        int32_t index = 0;
        if (JS_ToInt32(ctx, &index, argv[0]) < 0) {
            return JS_EXCEPTION;
        }
        if (index < 0 || static_cast<size_t>(index) >= host->storage_.size()) {
            return JS_NULL;
        }
        auto iterator = host->storage_.begin();
        std::advance(iterator, index);
        return JS_NewString(ctx, iterator->first.c_str());
    }

    static JSValue ThrowType(JSContext* ctx, const char* message) {
        return JS_ThrowTypeError(ctx, "%s", message);
    }

    // 取出并清空当前异常，返回可读文本。脚本异常带有 stack 时一并附带，
    // 便于定位出错的文件与行号。
    static std::string TakeExceptionText(JSContext* ctx) {
        JSValue exception = JS_GetException(ctx);
        const char* text = JS_ToCString(ctx, exception);
        std::string message;
        if (text != nullptr) {
            message = text;
            JS_FreeCString(ctx, text);
        } else {
            JSValue pending = JS_GetException(ctx);
            JS_FreeValue(ctx, pending);
            message = "脚本异常";
        }
        JSValue stack = JS_GetPropertyStr(ctx, exception, "stack");
        if (JS_IsString(stack)) {
            const char* stack_text = JS_ToCString(ctx, stack);
            if (stack_text != nullptr) {
                if (!message.empty()) {
                    message.append("\n");
                }
                message.append(stack_text);
                JS_FreeCString(ctx, stack_text);
            }
        }
        JS_FreeValue(ctx, stack);
        JS_FreeValue(ctx, exception);
        return message;
    }

    std::string TakeExceptionText() { return TakeExceptionText(ctx_); }

    static void ReportException(JSContext* ctx, const char* label) {
        std::string message = TakeExceptionText(ctx);
        std::fprintf(stderr, "%s: %s\n", label, message.c_str());
    }

    static void FinalizeWrapper(JSRuntime*, JSValueConst value) {
        JSClassID class_id = JS_INVALID_CLASS_ID;
        WrapperData* data = static_cast<WrapperData*>(JS_GetAnyOpaque(value, &class_id));
        delete data;
    }

    static void FinalizeGradient(JSRuntime*, JSValueConst value) {
        JSClassID class_id = JS_INVALID_CLASS_ID;
        GradientWrapperData* data =
            static_cast<GradientWrapperData*>(JS_GetAnyOpaque(value, &class_id));
        delete data;
    }

    static void FinalizePattern(JSRuntime*, JSValueConst value) {
        JSClassID class_id = JS_INVALID_CLASS_ID;
        PatternWrapperData* data =
            static_cast<PatternWrapperData*>(JS_GetAnyOpaque(value, &class_id));
        delete data;
    }

    // 变更通知：位于 SVG 根元素内部（不含根元素自身）的变更不影响 HTML
    // 布局，走 SVG 增量刷新；其余变更要求完整重新布局。changed 为空时按
    // 完整布局处理。
    void NotifyChanged(const Node* changed = nullptr) {
        if (changed != nullptr) {
            for (const Node* node = changed; node != nullptr; node = node->Parent()) {
                if (!node->IsElement()) {
                    continue;
                }
                if (static_cast<const Element*>(node)->TagName() == "svg") {
                    if (node != changed && options_.on_svg_changed) {
                        options_.on_svg_changed();
                        return;
                    }
                    break;
                }
            }
        }
        if (options_.on_document_changed) {
            options_.on_document_changed();
        }
    }

    // 从持久化文件载入 localStorage。文件缺失或格式不完整时按空存储处理。
    void LoadLocalStorage() {
        storage_.clear();
        if (options_.local_storage_path.empty()) {
            return;
        }
        std::ifstream input(std::filesystem::u8path(options_.local_storage_path),
                            std::ios::binary);
        if (!input) {
            return;
        }
        std::string line;
        while (std::getline(input, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            const size_t separator = line.find('\t');
            if (separator == std::string::npos) {
                continue;
            }
            const std::string key = HexDecode(line.substr(0, separator));
            const std::string value = HexDecode(line.substr(separator + 1));
            if (!key.empty()) {
                storage_[key] = value;
            }
        }
    }

    // 把 localStorage 写入持久化文件。目录不存在时自动创建。
    void SaveLocalStorage() const {
        if (options_.local_storage_path.empty()) {
            return;
        }
        std::error_code error;
        const std::filesystem::path path =
            std::filesystem::u8path(options_.local_storage_path);
        if (path.has_parent_path()) {
            std::filesystem::create_directories(path.parent_path(), error);
        }
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output) {
            return;
        }
        for (const auto& entry : storage_) {
            output << HexEncode(entry.first) << '\t' << HexEncode(entry.second) << '\n';
        }
    }

    // 编译内联事件属性文本为函数。编译失败时输出诊断并返回 undefined。
    JSValue CompileEventHandlerSource(const std::string& source) {
        if (ctx_ == nullptr || source.empty()) {
            return JS_UNDEFINED;
        }
        // 属性文本按函数体执行，事件对象以 event 参数传入，this 指向当前元素。
        const std::string wrapper = "(function(event){\n" + source + "\n})";
        JSValue function =
            JS_Eval(ctx_, wrapper.c_str(), wrapper.size(), "<事件属性>", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(function)) {
            ReportException(ctx_, "内联事件属性编译失败");
            JS_FreeValue(ctx_, function);
            return JS_UNDEFINED;
        }
        if (!JS_IsFunction(ctx_, function)) {
            JS_FreeValue(ctx_, function);
            return JS_UNDEFINED;
        }
        return function;
    }

    // 返回节点上指定类型的事件处理器函数（新引用）；没有时返回 undefined。
    // 脚本赋值的函数优先于内联属性，与浏览器的事件处理器 IDL 属性一致。
    JSValue EventHandlerFor(Node* node, const std::string& type) {
        if (ctx_ == nullptr || node == nullptr || !node->IsElement()) {
            return JS_UNDEFINED;
        }
        Element* element = static_cast<Element*>(node);
        const std::string attribute_source = element->GetAttribute("on" + type);
        EventHandlerSlot* slot = nullptr;
        auto node_entry = event_handlers_.find(node);
        if (node_entry != event_handlers_.end()) {
            auto type_entry = node_entry->second.find(type);
            if (type_entry != node_entry->second.end()) {
                slot = &type_entry->second;
            }
        }
        if (slot != nullptr && JS_IsFunction(ctx_, slot->assigned)) {
            return JS_DupValue(ctx_, slot->assigned);
        }
        if (attribute_source.empty()) {
            return JS_UNDEFINED;
        }
        if (slot == nullptr) {
            slot = &event_handlers_[node][type];
        }
        if (slot->attribute_source != attribute_source ||
            !JS_IsFunction(ctx_, slot->compiled)) {
            JS_FreeValue(ctx_, slot->compiled);
            slot->compiled = CompileEventHandlerSource(attribute_source);
            slot->attribute_source = attribute_source;
        }
        return JS_DupValue(ctx_, slot->compiled);
    }

    // 调用节点上的事件处理器；没有处理器时直接返回。
    void CallEventHandler(Node* node, const std::string& type, JSValue this_value,
                          JSValueConst event_argument) {
        JSValue function = EventHandlerFor(node, type);
        if (JS_IsUndefined(function)) {
            return;
        }
        JSValue result = JS_Call(ctx_, function, this_value, 1, &event_argument);
        if (JS_IsException(result)) {
            ReportException(ctx_, "事件处理器抛出异常");
        }
        JS_FreeValue(ctx_, result);
        JS_FreeValue(ctx_, function);
    }

    // 赋值或清除脚本事件处理器属性；非函数值按清除处理，与 null 语义一致。
    void SetEventHandlerFunction(Node* node, const std::string& type, JSValueConst value) {
        if (node == nullptr || !node->IsElement()) {
            return;
        }
        EventHandlerSlot& slot = event_handlers_[node][type];
        JS_FreeValue(ctx_, slot.assigned);
        slot.assigned = JS_IsFunction(ctx_, value) ? JS_DupValue(ctx_, value) : JS_UNDEFINED;
    }

    JSValue EventHandlerValue(Node* node, const std::string& type) {
        if (node == nullptr) {
            return JS_NULL;
        }
        auto node_entry = event_handlers_.find(node);
        if (node_entry == event_handlers_.end()) {
            return JS_NULL;
        }
        auto type_entry = node_entry->second.find(type);
        if (type_entry == node_entry->second.end()) {
            return JS_NULL;
        }
        if (JS_IsFunction(ctx_, type_entry->second.assigned)) {
            return JS_DupValue(ctx_, type_entry->second.assigned);
        }
        return JS_NULL;
    }

    JSValue WrapperFor(Node* node) {
        if (node == nullptr) {
            return JS_NULL;
        }
        auto found = node_wrappers_.find(node);
        if (found != node_wrappers_.end()) {
            return JS_DupValue(ctx_, found->second);
        }
        JSValue proto = node_proto_;
        if (node->IsDocument()) {
            proto = document_proto_;
        } else if (node->IsElement()) {
            proto = element_proto_;
        }
        JSValue wrapper = JS_NewObjectProtoClass(ctx_, proto, node_class_id_);
        if (JS_IsException(wrapper)) {
            return wrapper;
        }
        WrapperData* data = new WrapperData();
        data->raw = node;
        data->owned = node->weak_from_this().lock();
        if (JS_SetOpaque(wrapper, data) < 0) {
            delete data;
            JS_FreeValue(ctx_, wrapper);
            return JS_EXCEPTION;
        }
        node_wrappers_.emplace(node, JS_DupValue(ctx_, wrapper));
        return wrapper;
    }

    JSValue WrapperOrNull(Node* node) {
        if (node == nullptr) {
            return JS_NULL;
        }
        return WrapperFor(node);
    }

    JSValue StyleWrapperFor(Element* element) {
        auto found = style_wrappers_.find(element);
        if (found != style_wrappers_.end()) {
            return JS_DupValue(ctx_, found->second);
        }
        JSValue wrapper = JS_NewObjectProtoClass(ctx_, style_proto_, style_class_id_);
        if (JS_IsException(wrapper)) {
            return wrapper;
        }
        WrapperData* data = new WrapperData();
        data->raw = element;
        data->owned = element->weak_from_this().lock();
        if (JS_SetOpaque(wrapper, data) < 0) {
            delete data;
            JS_FreeValue(ctx_, wrapper);
            return JS_EXCEPTION;
        }
        style_wrappers_.emplace(element, JS_DupValue(ctx_, wrapper));
        return wrapper;
    }

    // 为元素创建（或复用）2D 上下文包装对象；上下文内部保存元素地址与绘制状态。
    JSValue CanvasContextFor(Element* element) {
        auto found = canvas_wrappers_.find(element);
        if (found != canvas_wrappers_.end()) {
            return JS_DupValue(ctx_, found->second);
        }
        JSValue wrapper = JS_NewObjectProtoClass(ctx_, canvas_context_proto_, canvas_class_id_);
        if (JS_IsException(wrapper)) {
            return wrapper;
        }
        WrapperData* data = new WrapperData();
        data->raw = element;
        data->owned = element->weak_from_this().lock();
        data->canvas_context = std::make_unique<CanvasRenderingContext2D>(element);
        if (JS_SetOpaque(wrapper, data) < 0) {
            delete data;
            JS_FreeValue(ctx_, wrapper);
            return JS_EXCEPTION;
        }
        CanvasSurface* surface = data->canvas_context->Surface();
        if (surface != nullptr) {
            ReportCanvasDebug("getContext", *surface);
        }
        canvas_wrappers_.emplace(element, JS_DupValue(ctx_, wrapper));
        return wrapper;
    }

    // 画布宽高变化后重置上下文：清空位图并恢复全部绘制状态。
    void ResetCanvasContext(Element* element) {
        auto found = canvas_wrappers_.find(element);
        if (found == canvas_wrappers_.end()) {
            return;
        }
        WrapperData* data = WrapperOf(ctx_, found->second, canvas_class_id_);
        if (data != nullptr && data->canvas_context) {
            data->canvas_context->Reset();
        }
    }

    // 为渐变创建独立包装对象；对象生命周期由 QuickJS 垃圾回收管理。
    JSValue GradientWrapperFor(std::shared_ptr<CanvasGradient> gradient) {
        JSValue wrapper = JS_NewObjectProtoClass(ctx_, gradient_proto_, gradient_class_id_);
        if (JS_IsException(wrapper)) {
            return wrapper;
        }
        GradientWrapperData* data = new GradientWrapperData();
        data->gradient = std::move(gradient);
        if (JS_SetOpaque(wrapper, data) < 0) {
            delete data;
            JS_FreeValue(ctx_, wrapper);
            return JS_EXCEPTION;
        }
        return wrapper;
    }

    // 为图案创建独立包装对象；对象生命周期由 QuickJS 垃圾回收管理。
    JSValue PatternWrapperFor(std::shared_ptr<CanvasPattern> pattern) {
        JSValue wrapper = JS_NewObjectProtoClass(ctx_, pattern_proto_, pattern_class_id_);
        if (JS_IsException(wrapper)) {
            return wrapper;
        }
        PatternWrapperData* data = new PatternWrapperData();
        data->pattern = std::move(pattern);
        if (JS_SetOpaque(wrapper, data) < 0) {
            delete data;
            JS_FreeValue(ctx_, wrapper);
            return JS_EXCEPTION;
        }
        return wrapper;
    }

    void DefineMethod(JSValue target, const char* name, JSCFunction* function, int length) {
        JSValue value = JS_NewCFunction2(ctx_, function, name, length, JS_CFUNC_generic, 0);
        JS_SetPropertyStr(ctx_, target, name, value);
    }

    void DefineAccessor(JSValue target, const char* name, JSGetter getter, JSSetter setter,
                        int magic) {
        JSAtom atom = JS_NewAtom(ctx_, name);
        JSCFunctionType getter_type;
        getter_type.getter_magic = getter;
        JSValue getter_value =
            JS_NewCFunction2(ctx_, getter_type.generic, name, 0, JS_CFUNC_getter_magic, magic);
        JSValue setter_value = JS_UNDEFINED;
        if (setter != nullptr) {
            JSCFunctionType setter_type;
            setter_type.setter_magic = setter;
            setter_value = JS_NewCFunction2(ctx_, setter_type.generic, name, 1,
                                            JS_CFUNC_setter_magic, magic);
        }
        JS_DefinePropertyGetSet(ctx_, target, atom, getter_value, setter_value,
                                JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
        JS_FreeAtom(ctx_, atom);
    }

    bool CreatePrototypes() {
        node_proto_ = JS_NewObject(ctx_);
        if (JS_IsException(node_proto_)) {
            return false;
        }
        DefineMethod(node_proto_, "appendChild", &ScriptHostImpl::AppendChild, 1);
        DefineMethod(node_proto_, "insertBefore", &ScriptHostImpl::InsertBefore, 2);
        DefineMethod(node_proto_, "removeChild", &ScriptHostImpl::RemoveChild, 1);
        DefineMethod(node_proto_, "remove", &ScriptHostImpl::RemoveNode, 0);
        DefineMethod(node_proto_, "getAttribute", &ScriptHostImpl::GetAttribute, 1);
        DefineMethod(node_proto_, "setAttribute", &ScriptHostImpl::SetAttribute, 2);
        DefineMethod(node_proto_, "removeAttribute", &ScriptHostImpl::RemoveAttribute, 1);
        DefineMethod(node_proto_, "hasAttribute", &ScriptHostImpl::HasAttribute, 1);
        DefineMethod(node_proto_, "getElementsByTagName",
                     &ScriptHostImpl::GetElementsByTagName, 1);
        DefineMethod(node_proto_, "addEventListener", &ScriptHostImpl::AddEventListener, 2);
        DefineMethod(node_proto_, "removeEventListener", &ScriptHostImpl::RemoveEventListener, 2);
        DefineAccessor(node_proto_, "parentNode", &ScriptHostImpl::ParentNodeGet, nullptr, 0);
        DefineAccessor(node_proto_, "firstChild", &ScriptHostImpl::FirstChildGet, nullptr, 0);
        DefineAccessor(node_proto_, "nextSibling", &ScriptHostImpl::NextSiblingGet, nullptr, 0);
        DefineAccessor(node_proto_, "children", &ScriptHostImpl::ChildrenGet, nullptr, 0);
        DefineAccessor(node_proto_, "textContent", &ScriptHostImpl::TextContentGet,
                       &ScriptHostImpl::TextContentSet, 0);
        DefineAccessor(node_proto_, "innerHTML", &ScriptHostImpl::InnerHtmlGet,
                       &ScriptHostImpl::InnerHtmlSet, 0);

        element_proto_ = JS_NewObjectProto(ctx_, node_proto_);
        if (JS_IsException(element_proto_)) {
            return false;
        }
        DefineAccessor(element_proto_, "tagName", &ScriptHostImpl::TagNameGet, nullptr, 0);
        DefineAccessor(element_proto_, "id", &ScriptHostImpl::ElementIdGet,
                       &ScriptHostImpl::ElementIdSet, 0);
        DefineAccessor(element_proto_, "className", &ScriptHostImpl::ClassNameGet,
                       &ScriptHostImpl::ClassNameSet, 0);
        DefineAccessor(element_proto_, "classList", &ScriptHostImpl::ClassListGet, nullptr, 0);
        DefineAccessor(element_proto_, "dataset", &ScriptHostImpl::DatasetGet, nullptr, 0);
        DefineAccessor(element_proto_, "value", &ScriptHostImpl::ElementValueGet,
                       &ScriptHostImpl::ElementValueSet, 0);
        // width/height：canvas 元素读写位图尺寸，其它元素返回 undefined。
        DefineAccessor(element_proto_, "width", &ScriptHostImpl::CanvasDimensionGet,
                       &ScriptHostImpl::CanvasDimensionSet, 0);
        DefineAccessor(element_proto_, "height", &ScriptHostImpl::CanvasDimensionGet,
                       &ScriptHostImpl::CanvasDimensionSet, 1);
        DefineAccessor(element_proto_, "checked", &ScriptHostImpl::ElementCheckedGet,
                       &ScriptHostImpl::ElementCheckedSet, 0);
        DefineAccessor(element_proto_, "disabled", &ScriptHostImpl::ElementDisabledGet,
                       &ScriptHostImpl::ElementDisabledSet, 0);
        DefineAccessor(element_proto_, "style", &ScriptHostImpl::StyleGet, nullptr, 0);
        DefineMethod(element_proto_, "getContext", &ScriptHostImpl::GetContext, 1);
        DefineMethod(element_proto_, "getBoundingClientRect",
                     &ScriptHostImpl::GetBoundingClientRect, 0);
        // 元素同样提供选择器查询，作用范围限定为该元素的子树。
        DefineMethod(element_proto_, "querySelector", &ScriptHostImpl::QuerySelector, 1);
        DefineMethod(element_proto_, "querySelectorAll", &ScriptHostImpl::QuerySelectorAll, 1);
        DefineMethod(element_proto_, "closest", &ScriptHostImpl::Closest, 1);
        // 文本框全选：copy 按钮通常先 select() 再 execCommand('copy')。
        DefineMethod(element_proto_, "select", &ScriptHostImpl::ElementSelect, 0);
        // 事件处理器属性：element.onclick = fn 与内联 onclick 属性共用派发路径。
        size_t handler_type_count = 0;
        const char* const* handler_types = EventHandlerTypes(&handler_type_count);
        for (size_t index = 0; index < handler_type_count; ++index) {
            const std::string property_name = std::string("on") + handler_types[index];
            DefineAccessor(element_proto_, property_name.c_str(),
                           &ScriptHostImpl::EventHandlerPropertyGet,
                           &ScriptHostImpl::EventHandlerPropertySet,
                           static_cast<int>(index));
        }

        document_proto_ = JS_NewObjectProto(ctx_, node_proto_);
        if (JS_IsException(document_proto_)) {
            return false;
        }
        DefineMethod(document_proto_, "getElementById", &ScriptHostImpl::GetElementById, 1);
        // getElementsByTagName 由 node_proto_ 提供，文档接收者查询整篇文档。
        DefineMethod(document_proto_, "querySelector", &ScriptHostImpl::QuerySelector, 1);
        DefineMethod(document_proto_, "querySelectorAll", &ScriptHostImpl::QuerySelectorAll, 1);
        DefineMethod(document_proto_, "createElement", &ScriptHostImpl::CreateElement, 1);
        DefineMethod(document_proto_, "createTextNode", &ScriptHostImpl::CreateTextNode, 1);
        DefineAccessor(document_proto_, "body", &ScriptHostImpl::BodyGet, nullptr, 0);
        DefineAccessor(document_proto_, "documentElement",
                       &ScriptHostImpl::DocumentElementGet, nullptr, 0);
        DefineAccessor(document_proto_, "readyState", &ScriptHostImpl::ReadyStateGet, nullptr, 0);
        DefineAccessor(document_proto_, "cookie", &ScriptHostImpl::CookieGet,
                       &ScriptHostImpl::CookieSet, 0);
        DefineAccessor(document_proto_, "activeElement", &ScriptHostImpl::ActiveElementGet,
                       nullptr, 0);
        DefineMethod(document_proto_, "execCommand", &ScriptHostImpl::DocumentExecCommand, 1);

        style_proto_ = JS_NewObject(ctx_);
        if (JS_IsException(style_proto_)) {
            return false;
        }
        for (int index = 0; index < kStyleBindingCount; ++index) {
            DefineAccessor(style_proto_, kStyleBindings[index].script_name,
                           &ScriptHostImpl::StylePropertyGet,
                           &ScriptHostImpl::StylePropertySet, index);
        }

        canvas_context_proto_ = JS_NewObject(ctx_);
        if (JS_IsException(canvas_context_proto_)) {
            return false;
        }
        DefineAccessor(canvas_context_proto_, "fillStyle",
                       &ScriptHostImpl::CanvasFillStyleGet,
                       &ScriptHostImpl::CanvasFillStyleSet, 0);
        DefineAccessor(canvas_context_proto_, "strokeStyle",
                       &ScriptHostImpl::CanvasStrokeStyleGet,
                       &ScriptHostImpl::CanvasStrokeStyleSet, 0);
        DefineAccessor(canvas_context_proto_, "font", &ScriptHostImpl::CanvasFontGet,
                       &ScriptHostImpl::CanvasFontSet, 0);
        DefineAccessor(canvas_context_proto_, "lineWidth",
                       &ScriptHostImpl::CanvasLineWidthGet,
                       &ScriptHostImpl::CanvasLineWidthSet, 0);
        DefineAccessor(canvas_context_proto_, "globalAlpha",
                       &ScriptHostImpl::CanvasGlobalAlphaGet,
                       &ScriptHostImpl::CanvasGlobalAlphaSet, 0);
        DefineAccessor(canvas_context_proto_, "globalCompositeOperation",
                       &ScriptHostImpl::CanvasGlobalCompositeOperationGet,
                       &ScriptHostImpl::CanvasGlobalCompositeOperationSet, 0);
        DefineAccessor(canvas_context_proto_, "shadowColor",
                       &ScriptHostImpl::CanvasShadowColorGet,
                       &ScriptHostImpl::CanvasShadowColorSet, 0);
        DefineAccessor(canvas_context_proto_, "shadowBlur",
                       &ScriptHostImpl::CanvasShadowBlurGet,
                       &ScriptHostImpl::CanvasShadowBlurSet, 0);
        DefineAccessor(canvas_context_proto_, "shadowOffsetX",
                       &ScriptHostImpl::CanvasShadowOffsetXGet,
                       &ScriptHostImpl::CanvasShadowOffsetXSet, 0);
        DefineAccessor(canvas_context_proto_, "shadowOffsetY",
                       &ScriptHostImpl::CanvasShadowOffsetYGet,
                       &ScriptHostImpl::CanvasShadowOffsetYSet, 0);
        DefineAccessor(canvas_context_proto_, "lineCap", &ScriptHostImpl::CanvasLineCapGet,
                       &ScriptHostImpl::CanvasLineCapSet, 0);
        DefineAccessor(canvas_context_proto_, "lineJoin", &ScriptHostImpl::CanvasLineJoinGet,
                       &ScriptHostImpl::CanvasLineJoinSet, 0);
        DefineAccessor(canvas_context_proto_, "miterLimit",
                       &ScriptHostImpl::CanvasMiterLimitGet,
                       &ScriptHostImpl::CanvasMiterLimitSet, 0);
        DefineAccessor(canvas_context_proto_, "lineDashOffset",
                       &ScriptHostImpl::CanvasLineDashOffsetGet,
                       &ScriptHostImpl::CanvasLineDashOffsetSet, 0);
        DefineAccessor(canvas_context_proto_, "textAlign", &ScriptHostImpl::CanvasTextAlignGet,
                       &ScriptHostImpl::CanvasTextAlignSet, 0);
        DefineAccessor(canvas_context_proto_, "direction", &ScriptHostImpl::CanvasDirectionGet,
                       &ScriptHostImpl::CanvasDirectionSet, 0);
        DefineAccessor(canvas_context_proto_, "textBaseline",
                       &ScriptHostImpl::CanvasTextBaselineGet,
                       &ScriptHostImpl::CanvasTextBaselineSet, 0);
        DefineMethod(canvas_context_proto_, "fillRect", &ScriptHostImpl::CanvasFillRect, 4);
        DefineMethod(canvas_context_proto_, "clearRect", &ScriptHostImpl::CanvasClearRect, 4);
        DefineMethod(canvas_context_proto_, "strokeRect", &ScriptHostImpl::CanvasStrokeRect, 4);
        DefineMethod(canvas_context_proto_, "fillText", &ScriptHostImpl::CanvasFillText, 3);
        DefineMethod(canvas_context_proto_, "measureText", &ScriptHostImpl::CanvasMeasureText, 1);
        DefineMethod(canvas_context_proto_, "beginPath", &ScriptHostImpl::CanvasBeginPath, 0);
        DefineMethod(canvas_context_proto_, "moveTo", &ScriptHostImpl::CanvasMoveTo, 2);
        DefineMethod(canvas_context_proto_, "lineTo", &ScriptHostImpl::CanvasLineTo, 2);
        DefineMethod(canvas_context_proto_, "quadraticCurveTo",
                     &ScriptHostImpl::CanvasQuadraticCurveTo, 4);
        DefineMethod(canvas_context_proto_, "bezierCurveTo",
                     &ScriptHostImpl::CanvasBezierCurveTo, 6);
        DefineMethod(canvas_context_proto_, "arc", &ScriptHostImpl::CanvasArc, 5);
        DefineMethod(canvas_context_proto_, "rect", &ScriptHostImpl::CanvasRect, 4);
        DefineMethod(canvas_context_proto_, "ellipse", &ScriptHostImpl::CanvasEllipse, 7);
        DefineMethod(canvas_context_proto_, "arcTo", &ScriptHostImpl::CanvasArcTo, 5);
        DefineMethod(canvas_context_proto_, "closePath", &ScriptHostImpl::CanvasClosePath, 0);
        DefineMethod(canvas_context_proto_, "fill", &ScriptHostImpl::CanvasFillPath, 0);
        DefineMethod(canvas_context_proto_, "stroke", &ScriptHostImpl::CanvasStrokePath, 0);
        DefineMethod(canvas_context_proto_, "clip", &ScriptHostImpl::CanvasClip, 0);
        DefineMethod(canvas_context_proto_, "isPointInPath",
                     &ScriptHostImpl::CanvasIsPointInPath, 2);
        DefineMethod(canvas_context_proto_, "toDataURL",
                     &ScriptHostImpl::CanvasToDataURL, 0);
        DefineMethod(canvas_context_proto_, "setLineDash",
                     &ScriptHostImpl::CanvasSetLineDash, 1);
        DefineMethod(canvas_context_proto_, "getLineDash",
                     &ScriptHostImpl::CanvasGetLineDash, 0);
        DefineMethod(canvas_context_proto_, "save", &ScriptHostImpl::CanvasSave, 0);
        DefineMethod(canvas_context_proto_, "restore", &ScriptHostImpl::CanvasRestore, 0);
        DefineMethod(canvas_context_proto_, "translate", &ScriptHostImpl::CanvasTranslate, 2);
        DefineMethod(canvas_context_proto_, "scale", &ScriptHostImpl::CanvasScale, 2);
        DefineMethod(canvas_context_proto_, "rotate", &ScriptHostImpl::CanvasRotate, 1);
        DefineMethod(canvas_context_proto_, "transform", &ScriptHostImpl::CanvasTransform, 6);
        DefineMethod(canvas_context_proto_, "setTransform", &ScriptHostImpl::CanvasSetTransform,
                     6);
        DefineMethod(canvas_context_proto_, "resetTransform",
                     &ScriptHostImpl::CanvasResetTransform, 0);
        DefineMethod(canvas_context_proto_, "createLinearGradient",
                     &ScriptHostImpl::CanvasCreateLinearGradient, 4);
        DefineMethod(canvas_context_proto_, "createRadialGradient",
                     &ScriptHostImpl::CanvasCreateRadialGradient, 6);
        DefineMethod(canvas_context_proto_, "createPattern",
                     &ScriptHostImpl::CanvasCreatePattern, 2);
        DefineMethod(canvas_context_proto_, "drawImage", &ScriptHostImpl::CanvasDrawImage, 3);
        DefineMethod(canvas_context_proto_, "getImageData",
                     &ScriptHostImpl::CanvasGetImageData, 4);
        DefineMethod(canvas_context_proto_, "putImageData",
                     &ScriptHostImpl::CanvasPutImageData, 3);
        DefineMethod(canvas_context_proto_, "createImageData",
                     &ScriptHostImpl::CanvasCreateImageData, 2);

        gradient_proto_ = JS_NewObject(ctx_);
        if (JS_IsException(gradient_proto_)) {
            return false;
        }
        DefineMethod(gradient_proto_, "addColorStop",
                     &ScriptHostImpl::GradientAddColorStop, 2);

        pattern_proto_ = JS_NewObject(ctx_);
        if (JS_IsException(pattern_proto_)) {
            return false;
        }
        return true;
    }

    bool InstallGlobals() {
        global_ = JS_GetGlobalObject(ctx_);
        if (JS_IsException(global_)) {
            return false;
        }

        JS_SetPropertyStr(ctx_, global_, "window", JS_DupValue(ctx_, global_));
        JS_SetPropertyStr(ctx_, global_, "addEventListener",
                          JS_NewCFunction(ctx_, &ScriptHostImpl::WindowAddEventListener,
                                          "addEventListener", 2));
        JS_SetPropertyStr(ctx_, global_, "removeEventListener",
                          JS_NewCFunction(ctx_, &ScriptHostImpl::WindowRemoveEventListener,
                                          "removeEventListener", 2));
        JS_SetPropertyStr(ctx_, global_, "innerWidth", JS_NewFloat64(ctx_, viewport_width_));
        JS_SetPropertyStr(ctx_, global_, "innerHeight", JS_NewFloat64(ctx_, viewport_height_));
        JS_SetPropertyStr(ctx_, global_, "devicePixelRatio", JS_NewInt32(ctx_, 1));

        JSValue console = JS_NewObject(ctx_);
        if (JS_IsException(console)) {
            return false;
        }
        JS_SetPropertyStr(ctx_, console, "log",
                          JS_NewCFunction(ctx_, &ScriptHostImpl::ConsoleLog, "log", 1));
        JS_SetPropertyStr(ctx_, console, "warn",
                          JS_NewCFunction(ctx_, &ScriptHostImpl::ConsoleLog, "warn", 1));
        JS_SetPropertyStr(ctx_, console, "error",
                          JS_NewCFunction(ctx_, &ScriptHostImpl::ConsoleLog, "error", 1));
        JS_SetPropertyStr(ctx_, global_, "console", console);

        JSValue document_wrapper = WrapperFor(document_);
        if (JS_IsException(document_wrapper)) {
            return false;
        }
        JS_SetPropertyStr(ctx_, global_, "document", document_wrapper);

        // localStorage：键值均为字符串。持久化文件存在时在初始化阶段载入。
        LoadLocalStorage();
        JSValue storage = JS_NewObject(ctx_);
        if (JS_IsException(storage)) {
            return false;
        }
        JS_SetPropertyStr(ctx_, storage, "getItem",
                          JS_NewCFunction(ctx_, &ScriptHostImpl::StorageGetItem, "getItem", 1));
        JS_SetPropertyStr(ctx_, storage, "setItem",
                          JS_NewCFunction(ctx_, &ScriptHostImpl::StorageSetItem, "setItem", 2));
        JS_SetPropertyStr(ctx_, storage, "removeItem",
                          JS_NewCFunction(ctx_, &ScriptHostImpl::StorageRemoveItem,
                                          "removeItem", 1));
        JS_SetPropertyStr(ctx_, storage, "clear",
                          JS_NewCFunction(ctx_, &ScriptHostImpl::StorageClear, "clear", 0));
        JS_SetPropertyStr(ctx_, storage, "key",
                          JS_NewCFunction(ctx_, &ScriptHostImpl::StorageKey, "key", 1));
        JS_SetPropertyStr(ctx_, storage, "length",
                          JS_NewInt32(ctx_, static_cast<int32_t>(storage_.size())));
        JS_SetPropertyStr(ctx_, global_, "localStorage", storage);

        JS_SetPropertyStr(ctx_, global_, "requestAnimationFrame",
                          JS_NewCFunction(ctx_, &ScriptHostImpl::RequestAnimationFrame,
                                         "requestAnimationFrame", 1));
        JS_SetPropertyStr(ctx_, global_, "setTimeout",
                          JS_NewCFunction(ctx_, &ScriptHostImpl::SetTimeout, "setTimeout", 2));
        JS_SetPropertyStr(ctx_, global_, "setInterval",
                          JS_NewCFunction(ctx_, &ScriptHostImpl::SetInterval, "setInterval", 2));
        JS_SetPropertyStr(ctx_, global_, "clearTimeout",
                          JS_NewCFunction(ctx_, &ScriptHostImpl::ClearTimeout, "clearTimeout", 1));
        JS_SetPropertyStr(ctx_, global_, "clearInterval",
                          JS_NewCFunction(ctx_, &ScriptHostImpl::ClearTimeout, "clearInterval", 1));
        return true;
    }

    void Shutdown() {
        if (ctx_ == nullptr) {
            if (runtime_ != nullptr) {
                JS_FreeRuntime(runtime_);
                runtime_ = nullptr;
            }
            return;
        }
        for (auto& group : listeners_) {
            for (ListenerBinding& binding : group.second.listeners) {
                JS_FreeValue(ctx_, binding.function);
            }
        }
        listeners_.clear();

        for (auto& node_entry : event_handlers_) {
            for (auto& type_entry : node_entry.second) {
                JS_FreeValue(ctx_, type_entry.second.compiled);
                JS_FreeValue(ctx_, type_entry.second.assigned);
            }
        }
        event_handlers_.clear();

        for (WindowListener& listener : window_listeners_) {
            JS_FreeValue(ctx_, listener.function);
        }
        window_listeners_.clear();

        for (Timer& timer : timers_) {
            JS_FreeValue(ctx_, timer.function);
        }
        timers_.clear();

        for (JSValue function : animation_frames_) {
            JS_FreeValue(ctx_, function);
        }
        animation_frames_.clear();

        // 先释放全部包装对象，让 finalizer 在上下文仍然有效时执行。
        for (auto& entry : canvas_wrappers_) {
            JS_FreeValue(ctx_, entry.second);
        }
        canvas_wrappers_.clear();
        for (auto& entry : style_wrappers_) {
            JS_FreeValue(ctx_, entry.second);
        }
        style_wrappers_.clear();
        for (auto& entry : node_wrappers_) {
            JS_FreeValue(ctx_, entry.second);
        }
        node_wrappers_.clear();

        JS_FreeValue(ctx_, global_);
        global_ = JS_UNDEFINED;
        JS_FreeValue(ctx_, gradient_proto_);
        gradient_proto_ = JS_UNDEFINED;
        JS_FreeValue(ctx_, pattern_proto_);
        pattern_proto_ = JS_UNDEFINED;
        JS_FreeValue(ctx_, canvas_context_proto_);
        canvas_context_proto_ = JS_UNDEFINED;
        JS_FreeValue(ctx_, style_proto_);
        style_proto_ = JS_UNDEFINED;
        JS_FreeValue(ctx_, document_proto_);
        document_proto_ = JS_UNDEFINED;
        JS_FreeValue(ctx_, element_proto_);
        element_proto_ = JS_UNDEFINED;
        JS_FreeValue(ctx_, node_proto_);
        node_proto_ = JS_UNDEFINED;

        JS_FreeContext(ctx_);
        ctx_ = nullptr;
        JS_FreeRuntime(runtime_);
        runtime_ = nullptr;
    }

    // 查询结果使用数组承载：调用方普遍对结果直接调用 forEach/map 等数组方法。
    JSValue MakeNodeList(const std::vector<Element*>& elements) {
        JSValue list = JS_NewArray(ctx_);
        if (JS_IsException(list)) {
            return list;
        }
        for (size_t index = 0; index < elements.size(); ++index) {
            JSValue wrapper = WrapperFor(elements[index]);
            if (JS_IsException(wrapper)) {
                JS_FreeValue(ctx_, list);
                return wrapper;
            }
            JS_SetPropertyUint32(ctx_, list, static_cast<uint32_t>(index), wrapper);
        }
        return list;
    }

    // ---------------------------------------------------------------
    // 全局函数：console 与动画/定时器
    // ---------------------------------------------------------------
    static JSValue ConsoleLog(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
        std::string line;
        for (int index = 0; index < argc; ++index) {
            const char* text = JS_ToCString(ctx, argv[index]);
            if (index != 0) {
                line += " ";
            }
            if (text != nullptr) {
                line += text;
                JS_FreeCString(ctx, text);
            } else {
                JSValue pending = JS_GetException(ctx);
                JS_FreeValue(ctx, pending);
            }
        }
        std::fprintf(stderr, "%s\n", line.c_str());
        return JS_UNDEFINED;
    }

    static JSValue RequestAnimationFrame(JSContext* ctx, JSValueConst, int argc,
                                         JSValueConst* argv) {
        ScriptHostImpl* host = HostOf(ctx);
        if (host == nullptr) {
            return JS_UNDEFINED;
        }
        if (argc < 1 || !JS_IsFunction(ctx, argv[0])) {
            return ThrowType(ctx, "requestAnimationFrame 需要一个函数");
        }
        host->animation_frames_.push_back(JS_DupValue(ctx, argv[0]));
        if (host->options_.on_animation_frame) {
            host->options_.on_animation_frame();
        }
        return JS_UNDEFINED;
    }

    // 定时器统一入口：repeating 为真时按固定间隔重复，否则只触发一次。
    static JSValue ScheduleTimer(JSContext* ctx, int argc, JSValueConst* argv, bool repeating) {
        ScriptHostImpl* host = HostOf(ctx);
        if (host == nullptr) {
            return JS_UNDEFINED;
        }
        if (argc < 1 || !JS_IsFunction(ctx, argv[0])) {
            return ThrowType(ctx, repeating ? "setInterval 需要一个函数"
                                            : "setTimeout 需要一个函数");
        }
        double delay = 0.0;
        if (argc >= 2) {
            if (JS_ToFloat64(ctx, &delay, argv[1]) < 0) {
                return JS_EXCEPTION;
            }
            if (!(delay >= 0.0)) {
                delay = 0.0;
            }
        }
        Timer timer;
        timer.id = host->next_timer_id_++;
        if (repeating) {
            // 零间隔重复定时器按 1 毫秒处理，避免单帧内无限触发。
            timer.interval_ms = std::max(delay, 1.0);
            timer.remaining_ms = timer.interval_ms;
        } else {
            timer.remaining_ms = delay;
        }
        timer.function = JS_DupValue(ctx, argv[0]);
        host->timers_.push_back(timer);
        return JS_NewUint32(ctx, timer.id);
    }

    static JSValue SetTimeout(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
        return ScheduleTimer(ctx, argc, argv, false);
    }

    static JSValue SetInterval(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
        return ScheduleTimer(ctx, argc, argv, true);
    }

    static JSValue ClearTimeout(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
        ScriptHostImpl* host = HostOf(ctx);
        if (host == nullptr) {
            return JS_UNDEFINED;
        }
        if (argc < 1) {
            return JS_UNDEFINED;
        }
        int32_t timer_id = 0;
        if (JS_ToInt32(ctx, &timer_id, argv[0]) < 0) {
            return JS_EXCEPTION;
        }
        for (size_t index = 0; index < host->timers_.size(); ++index) {
            if (static_cast<int32_t>(host->timers_[index].id) == timer_id) {
                JS_FreeValue(ctx, host->timers_[index].function);
                host->timers_.erase(host->timers_.begin() + static_cast<ptrdiff_t>(index));
                break;
            }
        }
        return JS_UNDEFINED;
    }

    // ---------------------------------------------------------------
    // 节点方法
    // ---------------------------------------------------------------
    static JSValue AppendChild(JSContext* ctx, JSValueConst this_val, int argc,
                               JSValueConst* argv) {
        ScriptHostImpl* host = HostOf(ctx);
        if (host == nullptr) {
            return JS_UNDEFINED;
        }
        Node* parent = NodeOf(ctx, this_val);
        if (parent == nullptr) {
            return ThrowType(ctx, "appendChild 的接收者不是节点");
        }
        if (argc < 1) {
            return ThrowType(ctx, "appendChild 需要一个节点参数");
        }
        Node* child = NodeOf(ctx, argv[0]);
        if (child == nullptr) {
            return ThrowType(ctx, "appendChild 的参数不是节点");
        }
        std::shared_ptr<Node> owned = child->weak_from_this().lock();
        if (!owned) {
            return ThrowType(ctx, "只能挂载由文档创建的节点");
        }
        host->document_->AppendChild(*parent, owned);
        host->NotifyChanged(parent);
        return host->WrapperFor(child);
    }

    static JSValue InsertBefore(JSContext* ctx, JSValueConst this_val, int argc,
                                JSValueConst* argv) {
        ScriptHostImpl* host = HostOf(ctx);
        if (host == nullptr) {
            return JS_UNDEFINED;
        }
        Node* parent = NodeOf(ctx, this_val);
        if (parent == nullptr) {
            return ThrowType(ctx, "insertBefore 的接收者不是节点");
        }
        if (argc < 1) {
            return ThrowType(ctx, "insertBefore 需要一个节点参数");
        }
        Node* child = NodeOf(ctx, argv[0]);
        if (child == nullptr) {
            return ThrowType(ctx, "insertBefore 的参数不是节点");
        }
        Node* reference = nullptr;
        if (argc >= 2) {
            reference = NodeOf(ctx, argv[1]);
        }
        std::shared_ptr<Node> owned = child->weak_from_this().lock();
        if (!owned) {
            return ThrowType(ctx, "只能挂载由文档创建的节点");
        }
        host->document_->InsertBefore(*parent, owned, reference);
        host->NotifyChanged(parent);
        return host->WrapperFor(child);
    }

    static JSValue RemoveChild(JSContext* ctx, JSValueConst this_val, int argc,
                               JSValueConst* argv) {
        ScriptHostImpl* host = HostOf(ctx);
        if (host == nullptr) {
            return JS_UNDEFINED;
        }
        Node* parent = NodeOf(ctx, this_val);
        if (parent == nullptr) {
            return ThrowType(ctx, "removeChild 的接收者不是节点");
        }
        if (argc < 1) {
            return ThrowType(ctx, "removeChild 需要一个节点参数");
        }
        Node* child = NodeOf(ctx, argv[0]);
        if (child == nullptr) {
            return ThrowType(ctx, "removeChild 的参数不是节点");
        }
        host->document_->RemoveChild(*parent, *child);
        host->NotifyChanged(parent);
        return host->WrapperFor(child);
    }

    static JSValue RemoveNode(JSContext* ctx, JSValueConst this_val, int,
                              JSValueConst*) {
        ScriptHostImpl* host = HostOf(ctx);
        if (host == nullptr) {
            return JS_UNDEFINED;
        }
        Node* node = NodeOf(ctx, this_val);
        if (node == nullptr) {
            return JS_UNDEFINED;
        }
        Node* parent = node->Parent();
        if (parent != nullptr) {
            host->document_->RemoveChild(*parent, *node);
            host->NotifyChanged(node);
        }
        return JS_UNDEFINED;
    }

    static JSValue GetAttribute(JSContext* ctx, JSValueConst this_val, int argc,
                                JSValueConst* argv) {
        Element* element = ElementOf(ctx, this_val);
        if (element == nullptr) {
            return JS_NULL;
        }
        if (argc < 1) {
            return JS_NULL;
        }
        const char* name = JS_ToCString(ctx, argv[0]);
        if (name == nullptr) {
            return JS_EXCEPTION;
        }
        // 属性缺失返回 null，属性存在但值为空时返回空字符串。
        if (!element->HasAttribute(name)) {
            JS_FreeCString(ctx, name);
            return JS_NULL;
        }
        std::string value = element->GetAttribute(name);
        JS_FreeCString(ctx, name);
        return JS_NewString(ctx, value.c_str());
    }

    static JSValue SetAttribute(JSContext* ctx, JSValueConst this_val, int argc,
                                JSValueConst* argv) {
        ScriptHostImpl* host = HostOf(ctx);
        Element* element = ElementOf(ctx, this_val);
        if (host == nullptr || element == nullptr) {
            return ThrowType(ctx, "setAttribute 的接收者不是元素");
        }
        if (argc < 2) {
            return ThrowType(ctx, "setAttribute 需要名称与值");
        }
        const char* name = JS_ToCString(ctx, argv[0]);
        if (name == nullptr) {
            return JS_EXCEPTION;
        }
        const char* value = JS_ToCString(ctx, argv[1]);
        if (value == nullptr) {
            JS_FreeCString(ctx, name);
            return JS_EXCEPTION;
        }
        element->SetAttribute(name, value);
        const bool reset_canvas = IsCanvasDimensionAttribute(*element, name);
        JS_FreeCString(ctx, name);
        JS_FreeCString(ctx, value);
        if (reset_canvas) {
            host->ResetCanvasContext(element);
        }
        host->NotifyChanged(element);
        return JS_UNDEFINED;
    }

    static JSValue RemoveAttribute(JSContext* ctx, JSValueConst this_val, int argc,
                                   JSValueConst* argv) {
        ScriptHostImpl* host = HostOf(ctx);
        Element* element = ElementOf(ctx, this_val);
        if (host == nullptr || element == nullptr) {
            return JS_UNDEFINED;
        }
        if (argc < 1) {
            return JS_UNDEFINED;
        }
        const char* name = JS_ToCString(ctx, argv[0]);
        if (name == nullptr) {
            return JS_EXCEPTION;
        }
        element->RemoveAttribute(name);
        const bool reset_canvas = IsCanvasDimensionAttribute(*element, name);
        JS_FreeCString(ctx, name);
        if (reset_canvas) {
            host->ResetCanvasContext(element);
        }
        host->NotifyChanged(element);
        return JS_UNDEFINED;
    }

    // getBoundingClientRect：返回元素边框盒在视口中的位置与尺寸。引擎通过
    // ScriptHostOptions 提供几何查询；查询不到时返回全零矩形。
    static JSValue GetBoundingClientRect(JSContext* ctx, JSValueConst this_val, int,
                                         JSValueConst*) {
        ScriptHostImpl* host = HostOf(ctx);
        Element* element = ElementOf(ctx, this_val);
        if (host == nullptr || element == nullptr) {
            return JS_NULL;
        }
        float left = 0.0f;
        float top = 0.0f;
        float width = 0.0f;
        float height = 0.0f;
        if (host->options_.get_element_client_rect) {
            host->options_.get_element_client_rect(*element, &left, &top, &width, &height);
        }
        JSValue result = JS_NewObject(ctx);
        if (JS_IsException(result)) {
            return result;
        }
        JS_SetPropertyStr(ctx, result, "x", JS_NewFloat64(ctx, left));
        JS_SetPropertyStr(ctx, result, "y", JS_NewFloat64(ctx, top));
        JS_SetPropertyStr(ctx, result, "left", JS_NewFloat64(ctx, left));
        JS_SetPropertyStr(ctx, result, "top", JS_NewFloat64(ctx, top));
        JS_SetPropertyStr(ctx, result, "right", JS_NewFloat64(ctx, left + width));
        JS_SetPropertyStr(ctx, result, "bottom", JS_NewFloat64(ctx, top + height));
        JS_SetPropertyStr(ctx, result, "width", JS_NewFloat64(ctx, width));
        JS_SetPropertyStr(ctx, result, "height", JS_NewFloat64(ctx, height));
        return result;
    }

    static JSValue HasAttribute(JSContext* ctx, JSValueConst this_val, int argc,
                                JSValueConst* argv) {
        Element* element = ElementOf(ctx, this_val);
        if (element == nullptr || argc < 1) {
            return JS_NewBool(ctx, false);
        }
        const char* name = JS_ToCString(ctx, argv[0]);
        if (name == nullptr) {
            return JS_EXCEPTION;
        }
        const bool present = element->HasAttribute(name);
        JS_FreeCString(ctx, name);
        return JS_NewBool(ctx, present);
    }

    static JSValue AddEventListener(JSContext* ctx, JSValueConst this_val, int argc,
                                    JSValueConst* argv) {
        ScriptHostImpl* host = HostOf(ctx);
        if (host == nullptr) {
            return JS_UNDEFINED;
        }
        Node* node = NodeOf(ctx, this_val);
        if (node == nullptr) {
            return ThrowType(ctx, "addEventListener 的接收者不是节点");
        }
        if (argc < 2 || !JS_IsFunction(ctx, argv[1])) {
            return ThrowType(ctx, "addEventListener 需要事件类型与函数");
        }
        const char* type = JS_ToCString(ctx, argv[0]);
        if (type == nullptr) {
            return JS_EXCEPTION;
        }
        WrapperData* data = WrapperOf(ctx, this_val, host->node_class_id_);
        ListenerGroup& group = host->listeners_[node];
        if (!group.keep_alive && data != nullptr) {
            group.keep_alive = data->owned;
        }
        ListenerBinding binding;
        binding.type = type;
        binding.function = JS_DupValue(ctx, argv[1]);
        group.listeners.push_back(std::move(binding));
        JS_FreeCString(ctx, type);
        return JS_UNDEFINED;
    }

    // window 即全局对象：这里提供 addEventListener/removeEventListener，
    // 监听器在事件冒泡结束后统一调用。
    static JSValue WindowAddEventListener(JSContext* ctx, JSValueConst, int argc,
                                          JSValueConst* argv) {
        ScriptHostImpl* host = HostOf(ctx);
        if (host == nullptr || argc < 2 || !JS_IsFunction(ctx, argv[1])) {
            return JS_UNDEFINED;
        }
        const char* type = JS_ToCString(ctx, argv[0]);
        if (type == nullptr) {
            return JS_EXCEPTION;
        }
        WindowListener listener;
        listener.type = type;
        listener.function = JS_DupValue(ctx, argv[1]);
        host->window_listeners_.push_back(std::move(listener));
        JS_FreeCString(ctx, type);
        return JS_UNDEFINED;
    }

    static JSValue WindowRemoveEventListener(JSContext* ctx, JSValueConst, int argc,
                                             JSValueConst* argv) {
        ScriptHostImpl* host = HostOf(ctx);
        if (host == nullptr || argc < 2 || !JS_IsFunction(ctx, argv[1])) {
            return JS_UNDEFINED;
        }
        const char* type = JS_ToCString(ctx, argv[0]);
        if (type == nullptr) {
            return JS_EXCEPTION;
        }
        const std::string event_type = type;
        JS_FreeCString(ctx, type);
        std::vector<WindowListener>& listeners = host->window_listeners_;
        for (size_t index = 0; index < listeners.size(); ++index) {
            if (listeners[index].type != event_type) {
                continue;
            }
            if (!JS_IsSameValue(ctx, listeners[index].function, argv[1])) {
                continue;
            }
            JS_FreeValue(ctx, listeners[index].function);
            listeners.erase(listeners.begin() + static_cast<ptrdiff_t>(index));
            break;
        }
        return JS_UNDEFINED;
    }

    // 事件处理器属性（element.onclick 等）的读写：写入的函数进入统一的派发路径。
    static JSValue EventHandlerPropertyGet(JSContext* ctx, JSValueConst this_val, int magic) {
        ScriptHostImpl* host = HostOf(ctx);
        Node* node = NodeOf(ctx, this_val);
        size_t count = 0;
        const char* const* types = EventHandlerTypes(&count);
        if (host == nullptr || node == nullptr || magic < 0 ||
            static_cast<size_t>(magic) >= count) {
            return JS_NULL;
        }
        return host->EventHandlerValue(node, types[magic]);
    }

    static JSValue EventHandlerPropertySet(JSContext* ctx, JSValueConst this_val,
                                           JSValueConst value, int magic) {
        ScriptHostImpl* host = HostOf(ctx);
        Node* node = NodeOf(ctx, this_val);
        size_t count = 0;
        const char* const* types = EventHandlerTypes(&count);
        if (host == nullptr || node == nullptr || magic < 0 ||
            static_cast<size_t>(magic) >= count) {
            return JS_UNDEFINED;
        }
        host->SetEventHandlerFunction(node, types[magic], value);
        return JS_UNDEFINED;
    }

    static JSValue RemoveEventListener(JSContext* ctx, JSValueConst this_val, int argc,
                                       JSValueConst* argv) {
        ScriptHostImpl* host = HostOf(ctx);
        if (host == nullptr) {
            return JS_UNDEFINED;
        }
        Node* node = NodeOf(ctx, this_val);
        if (node == nullptr || argc < 2 || !JS_IsFunction(ctx, argv[1])) {
            return JS_UNDEFINED;
        }
        const char* type = JS_ToCString(ctx, argv[0]);
        if (type == nullptr) {
            return JS_EXCEPTION;
        }
        const std::string event_type = type;
        JS_FreeCString(ctx, type);
        auto group = host->listeners_.find(node);
        if (group == host->listeners_.end()) {
            return JS_UNDEFINED;
        }
        std::vector<ListenerBinding>& listeners = group->second.listeners;
        for (size_t index = 0; index < listeners.size(); ++index) {
            if (listeners[index].type != event_type) {
                continue;
            }
            if (!JS_IsSameValue(ctx, listeners[index].function, argv[1])) {
                continue;
            }
            JS_FreeValue(ctx, listeners[index].function);
            listeners.erase(listeners.begin() + static_cast<ptrdiff_t>(index));
            break;
        }
        return JS_UNDEFINED;
    }

    // ---------------------------------------------------------------
    // 节点属性
    // ---------------------------------------------------------------
    static JSValue ParentNodeGet(JSContext* ctx, JSValueConst this_val, int) {
        ScriptHostImpl* host = HostOf(ctx);
        Node* node = NodeOf(ctx, this_val);
        if (host == nullptr || node == nullptr) {
            return JS_NULL;
        }
        return host->WrapperOrNull(node->Parent());
    }

    static JSValue FirstChildGet(JSContext* ctx, JSValueConst this_val, int) {
        ScriptHostImpl* host = HostOf(ctx);
        Node* node = NodeOf(ctx, this_val);
        if (host == nullptr || node == nullptr || node->ChildCount() == 0) {
            return JS_NULL;
        }
        return host->WrapperOrNull(node->ChildAt(0));
    }

    static JSValue NextSiblingGet(JSContext* ctx, JSValueConst this_val, int) {
        ScriptHostImpl* host = HostOf(ctx);
        Node* node = NodeOf(ctx, this_val);
        if (host == nullptr || node == nullptr) {
            return JS_NULL;
        }
        return host->WrapperOrNull(node->NextSibling());
    }

    static JSValue ChildrenGet(JSContext* ctx, JSValueConst this_val, int) {
        ScriptHostImpl* host = HostOf(ctx);
        Node* node = NodeOf(ctx, this_val);
        if (host == nullptr || node == nullptr) {
            return host != nullptr ? host->MakeNodeList({}) : JS_UNDEFINED;
        }
        std::vector<Element*> children;
        for (const NodePtr& child : node->Children()) {
            if (child && child->IsElement()) {
                children.push_back(static_cast<Element*>(child.get()));
            }
        }
        return host->MakeNodeList(children);
    }

    static JSValue TextContentGet(JSContext* ctx, JSValueConst this_val, int) {
        Node* node = NodeOf(ctx, this_val);
        if (node == nullptr) {
            return JS_UNDEFINED;
        }
        return JS_NewString(ctx, TextContent(*node).c_str());
    }

    static JSValue TextContentSet(JSContext* ctx, JSValueConst this_val, JSValueConst value,
                                  int) {
        ScriptHostImpl* host = HostOf(ctx);
        Node* node = NodeOf(ctx, this_val);
        if (host == nullptr || node == nullptr) {
            return JS_UNDEFINED;
        }
        const char* text = JS_ToCString(ctx, value);
        if (text == nullptr) {
            return JS_EXCEPTION;
        }
        if (node->IsText()) {
            static_cast<TextNode*>(node)->SetData(text);
        } else if (node->IsElement()) {
            host->document_->RemoveAllChildren(*node);
            host->document_->AppendChild(*node, host->document_->CreateTextNode(text));
        } else {
            JS_FreeCString(ctx, text);
            return JS_UNDEFINED;
        }
        JS_FreeCString(ctx, text);
        host->NotifyChanged(node);
        return JS_UNDEFINED;
    }

    static JSValue InnerHtmlGet(JSContext* ctx, JSValueConst this_val, int) {
        Node* node = NodeOf(ctx, this_val);
        if (node == nullptr) {
            return JS_UNDEFINED;
        }
        return JS_NewString(ctx, SerializeChildren(*node).c_str());
    }

    static JSValue InnerHtmlSet(JSContext* ctx, JSValueConst this_val, JSValueConst value,
                                int) {
        ScriptHostImpl* host = HostOf(ctx);
        Element* element = ElementOf(ctx, this_val);
        if (host == nullptr || element == nullptr) {
            return ThrowType(ctx, "innerHTML 只能设置在元素上");
        }
        const char* source = JS_ToCString(ctx, value);
        if (source == nullptr) {
            return JS_EXCEPTION;
        }
        const std::string fragment = source;
        JS_FreeCString(ctx, source);
        host->document_->RemoveAllChildren(*element);
        std::string error;
        ParseHtmlFragment(fragment, *host->document_, *element, &error);
        host->NotifyChanged(element);
        return JS_UNDEFINED;
    }

    static JSValue TagNameGet(JSContext* ctx, JSValueConst this_val, int) {
        Element* element = ElementOf(ctx, this_val);
        if (element == nullptr) {
            return JS_UNDEFINED;
        }
        std::string tag = element->TagName();
        for (char& character : tag) {
            character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
        }
        return JS_NewString(ctx, tag.c_str());
    }

    static JSValue ElementIdGet(JSContext* ctx, JSValueConst this_val, int) {
        Element* element = ElementOf(ctx, this_val);
        if (element == nullptr) {
            return JS_UNDEFINED;
        }
        return JS_NewString(ctx, element->Id().c_str());
    }

    static JSValue ElementIdSet(JSContext* ctx, JSValueConst this_val, JSValueConst value,
                                int) {
        ScriptHostImpl* host = HostOf(ctx);
        Element* element = ElementOf(ctx, this_val);
        if (host == nullptr || element == nullptr) {
            return JS_UNDEFINED;
        }
        const char* text = JS_ToCString(ctx, value);
        if (text == nullptr) {
            return JS_EXCEPTION;
        }
        element->SetAttribute("id", text);
        JS_FreeCString(ctx, text);
        host->NotifyChanged(element);
        return JS_UNDEFINED;
    }

    static JSValue ClassNameGet(JSContext* ctx, JSValueConst this_val, int) {
        Element* element = ElementOf(ctx, this_val);
        if (element == nullptr) {
            return JS_UNDEFINED;
        }
        return JS_NewString(ctx, element->ClassName().c_str());
    }

    static JSValue ClassNameSet(JSContext* ctx, JSValueConst this_val, JSValueConst value,
                                int) {
        ScriptHostImpl* host = HostOf(ctx);
        Element* element = ElementOf(ctx, this_val);
        if (host == nullptr || element == nullptr) {
            return JS_UNDEFINED;
        }
        const char* text = JS_ToCString(ctx, value);
        if (text == nullptr) {
            return JS_EXCEPTION;
        }
        element->SetAttribute("class", text);
        JS_FreeCString(ctx, text);
        host->NotifyChanged(element);
        return JS_UNDEFINED;
    }

    // ---------------------------------------------------------------
    // classList 与 dataset
    // ---------------------------------------------------------------
    // 按空白切分 class 属性中的类名。
    static std::vector<std::string> SplitClassTokens(const std::string& text) {
        std::vector<std::string> tokens;
        std::string current;
        for (char character : text) {
            if (character == ' ' || character == '\t' || character == '\n' ||
                character == '\r' || character == '\f') {
                if (!current.empty()) {
                    tokens.push_back(current);
                    current.clear();
                }
                continue;
            }
            current.push_back(character);
        }
        if (!current.empty()) {
            tokens.push_back(current);
        }
        return tokens;
    }

    static bool HasClassToken(const std::vector<std::string>& tokens, const std::string& token) {
        return std::find(tokens.begin(), tokens.end(), token) != tokens.end();
    }

    static void WriteClassTokens(Element& element, const std::vector<std::string>& tokens) {
        std::string text;
        for (size_t index = 0; index < tokens.size(); ++index) {
            if (index > 0) {
                text.push_back(' ');
            }
            text.append(tokens[index]);
        }
        element.SetAttribute("class", text);
    }

    // classList 对象通过 __node 属性持有所属元素，方法据此读写 class 属性。
    static Element* ClassListOwner(JSContext* ctx, JSValueConst this_val) {
        JSValue owner = JS_GetPropertyStr(ctx, this_val, "__node");
        Element* element = ElementOf(ctx, owner);
        JS_FreeValue(ctx, owner);
        return element;
    }

    static JSValue ClassListGet(JSContext* ctx, JSValueConst this_val, int) {
        Element* element = ElementOf(ctx, this_val);
        if (element == nullptr) {
            return JS_NULL;
        }
        JSValue list = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, list, "__node", JS_DupValue(ctx, this_val));
        JS_SetPropertyStr(ctx, list, "add", JS_NewCFunction(ctx, ClassListAdd, "add", 1));
        JS_SetPropertyStr(ctx, list, "remove",
                          JS_NewCFunction(ctx, ClassListRemove, "remove", 1));
        JS_SetPropertyStr(ctx, list, "toggle",
                          JS_NewCFunction(ctx, ClassListToggle, "toggle", 2));
        JS_SetPropertyStr(ctx, list, "contains",
                          JS_NewCFunction(ctx, ClassListContains, "contains", 1));
        return list;
    }

    static JSValue ClassListAdd(JSContext* ctx, JSValueConst this_val, int argc,
                                JSValueConst* argv) {
        ScriptHostImpl* host = HostOf(ctx);
        Element* element = ClassListOwner(ctx, this_val);
        if (element == nullptr) {
            return JS_UNDEFINED;
        }
        std::vector<std::string> tokens = SplitClassTokens(element->ClassName());
        bool changed = false;
        for (int index = 0; index < argc; ++index) {
            const char* text = JS_ToCString(ctx, argv[index]);
            if (text == nullptr) {
                return JS_EXCEPTION;
            }
            const std::string token = text;
            JS_FreeCString(ctx, text);
            if (token.empty() || HasClassToken(tokens, token)) {
                continue;
            }
            tokens.push_back(token);
            changed = true;
        }
        if (changed) {
            WriteClassTokens(*element, tokens);
            if (host != nullptr) {
                host->NotifyChanged(element);
            }
        }
        return JS_UNDEFINED;
    }

    static JSValue ClassListRemove(JSContext* ctx, JSValueConst this_val, int argc,
                                   JSValueConst* argv) {
        ScriptHostImpl* host = HostOf(ctx);
        Element* element = ClassListOwner(ctx, this_val);
        if (element == nullptr) {
            return JS_UNDEFINED;
        }
        std::vector<std::string> tokens = SplitClassTokens(element->ClassName());
        bool changed = false;
        for (int index = 0; index < argc; ++index) {
            const char* text = JS_ToCString(ctx, argv[index]);
            if (text == nullptr) {
                return JS_EXCEPTION;
            }
            const std::string token = text;
            JS_FreeCString(ctx, text);
            const auto found = std::find(tokens.begin(), tokens.end(), token);
            if (found == tokens.end()) {
                continue;
            }
            tokens.erase(found);
            changed = true;
        }
        if (changed) {
            WriteClassTokens(*element, tokens);
            if (host != nullptr) {
                host->NotifyChanged(element);
            }
        }
        return JS_UNDEFINED;
    }

    static JSValue ClassListContains(JSContext* ctx, JSValueConst this_val, int argc,
                                      JSValueConst* argv) {
        Element* element = ClassListOwner(ctx, this_val);
        if (element == nullptr || argc < 1) {
            return JS_FALSE;
        }
        const char* text = JS_ToCString(ctx, argv[0]);
        if (text == nullptr) {
            return JS_EXCEPTION;
        }
        const bool present = HasClassToken(SplitClassTokens(element->ClassName()), text);
        JS_FreeCString(ctx, text);
        return JS_NewBool(ctx, present);
    }

    // toggle(token, force)：force 为真时添加、为假时移除，缺省时按当前状态取反。
    static JSValue ClassListToggle(JSContext* ctx, JSValueConst this_val, int argc,
                                   JSValueConst* argv) {
        ScriptHostImpl* host = HostOf(ctx);
        Element* element = ClassListOwner(ctx, this_val);
        if (element == nullptr || argc < 1) {
            return JS_FALSE;
        }
        const char* text = JS_ToCString(ctx, argv[0]);
        if (text == nullptr) {
            return JS_EXCEPTION;
        }
        const std::string token = text;
        JS_FreeCString(ctx, text);

        std::vector<std::string> tokens = SplitClassTokens(element->ClassName());
        const bool present = HasClassToken(tokens, token);
        bool add = !present;
        if (argc >= 2 && !JS_IsUndefined(argv[1])) {
            add = JS_ToBool(ctx, argv[1]) > 0;
        }
        if (add == present) {
            return JS_NewBool(ctx, add);
        }
        if (add) {
            tokens.push_back(token);
        } else {
            tokens.erase(std::find(tokens.begin(), tokens.end(), token));
        }
        WriteClassTokens(*element, tokens);
        if (host != nullptr) {
            host->NotifyChanged(element);
        }
        return JS_NewBool(ctx, add);
    }

    // dataset 视图：由 data-* 属性生成，键名按 dataset 规则转为小驼峰。
    static std::string DatasetKeyName(const std::string& attribute_name) {
        std::string key;
        bool uppercase_next = false;
        for (size_t index = 5; index < attribute_name.size(); ++index) {
            const char character = attribute_name[index];
            if (character == '-') {
                uppercase_next = true;
                continue;
            }
            if (uppercase_next && character >= 'a' && character <= 'z') {
                key.push_back(static_cast<char>(character - 'a' + 'A'));
                uppercase_next = false;
                continue;
            }
            uppercase_next = false;
            key.push_back(character);
        }
        return key;
    }

    static JSValue DatasetGet(JSContext* ctx, JSValueConst this_val, int) {
        Element* element = ElementOf(ctx, this_val);
        if (element == nullptr) {
            return JS_NULL;
        }
        JSValue dataset = JS_NewObject(ctx);
        for (const Attribute& attribute : element->Attributes()) {
            if (attribute.name.size() <= 5 || attribute.name.compare(0, 5, "data-") != 0) {
                continue;
            }
            const std::string key = DatasetKeyName(attribute.name);
            if (key.empty()) {
                continue;
            }
            JS_SetPropertyStr(ctx, dataset, key.c_str(),
                              JS_NewString(ctx, attribute.value.c_str()));
        }
        return dataset;
    }

    // option 的取值：有 value 属性时取属性值，否则取去除首尾空白的文本内容。
    static std::string OptionValueText(const Element& option) {
        if (option.HasAttribute("value")) {
            return option.GetAttribute("value");
        }
        std::string text = TextContent(option);
        TrimAsciiSpace(&text);
        return text;
    }

    // 下拉框当前选中项：优先取首个带 selected 属性的 option，否则取第一个 option。
    // 收集 select 下的全部 option，按文档顺序递归进入 optgroup。
    static void CollectSelectOptions(const Element& parent,
                                     std::vector<const Element*>* options) {
        for (const NodePtr& child : parent.Children()) {
            if (!child || !child->IsElement()) {
                continue;
            }
            const Element& element = static_cast<const Element&>(*child);
            if (element.TagName() == "option") {
                options->push_back(&element);
            } else if (element.TagName() == "optgroup") {
                CollectSelectOptions(element, options);
            }
        }
    }

    static const Element* SelectedOption(const Element& select) {
        std::vector<const Element*> options;
        CollectSelectOptions(select, &options);
        for (const Element* option : options) {
            if (option->HasAttribute("selected")) {
                return option;
            }
        }
        return options.empty() ? nullptr : options.front();
    }

    static JSValue ElementValueGet(JSContext* ctx, JSValueConst this_val, int) {
        Element* element = ElementOf(ctx, this_val);
        if (element == nullptr) {
            return JS_UNDEFINED;
        }
        const std::string tag_name = AsciiLower(element->TagName());
        if (tag_name == "textarea") {
            return JS_NewString(ctx, TextContent(*element).c_str());
        }
        if (tag_name == "select") {
            const Element* option = SelectedOption(*element);
            return JS_NewString(ctx, option != nullptr ? OptionValueText(*option).c_str() : "");
        }
        return JS_NewString(ctx, element->GetAttribute("value").c_str());
    }

    static JSValue ElementValueSet(JSContext* ctx, JSValueConst this_val, JSValueConst value,
                                   int) {
        ScriptHostImpl* host = HostOf(ctx);
        Element* element = ElementOf(ctx, this_val);
        if (host == nullptr || element == nullptr) {
            return JS_UNDEFINED;
        }
        const char* text = JS_ToCString(ctx, value);
        if (text == nullptr) {
            return JS_EXCEPTION;
        }
        const std::string new_value = text;
        JS_FreeCString(ctx, text);

        const std::string tag_name = AsciiLower(element->TagName());
        bool changed = false;
        if (tag_name == "textarea") {
            host->document_->RemoveAllChildren(*element);
            host->document_->AppendChild(*element, host->document_->CreateTextNode(new_value));
            changed = true;
        } else if (tag_name == "select") {
            // 按 value 属性或文本匹配 option；没有匹配项时不修改选中状态。
            const Element* matched = nullptr;
            for (const NodePtr& child : element->Children()) {
                if (!child || !child->IsElement()) {
                    continue;
                }
                const Element& option = static_cast<const Element&>(*child);
                if (option.TagName() != "option") {
                    continue;
                }
                if (OptionValueText(option) == new_value) {
                    matched = &option;
                    break;
                }
            }
            if (matched != nullptr) {
                for (const NodePtr& child : element->Children()) {
                    if (!child || !child->IsElement()) {
                        continue;
                    }
                    Element& option = static_cast<Element&>(*child);
                    if (option.TagName() != "option") {
                        continue;
                    }
                    if (&option == matched) {
                        option.SetAttribute("selected", "selected");
                    } else {
                        option.RemoveAttribute("selected");
                    }
                }
                changed = true;
            }
        } else {
            element->SetAttribute("value", new_value);
            changed = true;
        }

        if (changed) {
            host->NotifyChanged(element);
        }
        return JS_UNDEFINED;
    }

    // element.select()：文本框与输入框全选，记录文本供 document.execCommand('copy') 使用。
    static JSValue ElementSelect(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
        ScriptHostImpl* host = HostOf(ctx);
        Element* element = ElementOf(ctx, this_val);
        if (host == nullptr || element == nullptr) {
            return JS_UNDEFINED;
        }
        const std::string tag_name = AsciiLower(element->TagName());
        if (tag_name == "textarea") {
            host->selected_text_ = TextContent(*element);
        } else if (tag_name == "input") {
            host->selected_text_ = element->GetAttribute("value");
        }
        return JS_UNDEFINED;
    }

    // 读取布尔属性；转换失败时异常已由 QuickJS 设置。
    static bool ReadBoolean(JSContext* ctx, JSValueConst value, bool* output) {
        const int converted = JS_ToBool(ctx, value);
        if (converted < 0) {
            return false;
        }
        *output = converted != 0;
        return true;
    }

    static JSValue ElementCheckedGet(JSContext* ctx, JSValueConst this_val, int) {
        Element* element = ElementOf(ctx, this_val);
        if (element == nullptr) {
            return JS_UNDEFINED;
        }
        return JS_NewBool(ctx, element->HasAttribute("checked"));
    }

    static JSValue ElementCheckedSet(JSContext* ctx, JSValueConst this_val, JSValueConst value,
                                     int) {
        ScriptHostImpl* host = HostOf(ctx);
        Element* element = ElementOf(ctx, this_val);
        if (host == nullptr || element == nullptr) {
            return JS_UNDEFINED;
        }
        bool checked = false;
        if (!ReadBoolean(ctx, value, &checked)) {
            return JS_EXCEPTION;
        }
        if (checked) {
            element->SetAttribute("checked", "checked");
        } else {
            element->RemoveAttribute("checked");
        }
        host->NotifyChanged(element);
        return JS_UNDEFINED;
    }

    static JSValue ElementDisabledGet(JSContext* ctx, JSValueConst this_val, int) {
        Element* element = ElementOf(ctx, this_val);
        if (element == nullptr) {
            return JS_UNDEFINED;
        }
        return JS_NewBool(ctx, element->HasAttribute("disabled"));
    }

    static JSValue ElementDisabledSet(JSContext* ctx, JSValueConst this_val, JSValueConst value,
                                      int) {
        ScriptHostImpl* host = HostOf(ctx);
        Element* element = ElementOf(ctx, this_val);
        if (host == nullptr || element == nullptr) {
            return JS_UNDEFINED;
        }
        bool disabled = false;
        if (!ReadBoolean(ctx, value, &disabled)) {
            return JS_EXCEPTION;
        }
        if (disabled) {
            element->SetAttribute("disabled", "disabled");
        } else {
            element->RemoveAttribute("disabled");
        }
        host->NotifyChanged(element);
        return JS_UNDEFINED;
    }

    static JSValue StyleGet(JSContext* ctx, JSValueConst this_val, int) {
        ScriptHostImpl* host = HostOf(ctx);
        Element* element = ElementOf(ctx, this_val);
        if (host == nullptr || element == nullptr) {
            return JS_NULL;
        }
        return host->StyleWrapperFor(element);
    }

    // ---------------------------------------------------------------
    // 文档方法
    // ---------------------------------------------------------------
    static JSValue GetElementById(JSContext* ctx, JSValueConst, int argc,
                                  JSValueConst* argv) {
        ScriptHostImpl* host = HostOf(ctx);
        if (host == nullptr) {
            return JS_UNDEFINED;
        }
        if (argc < 1) {
            return JS_NULL;
        }
        const char* element_id = JS_ToCString(ctx, argv[0]);
        if (element_id == nullptr) {
            return JS_EXCEPTION;
        }
        Element* element = host->document_->GetElementById(element_id);
        JS_FreeCString(ctx, element_id);
        return host->WrapperOrNull(element);
    }

    // getElementsByTagName：以接收者为根收集后代元素（不含根自身）。
    // 文档接收者查询整篇文档，元素接收者只查询自身子树。
    static JSValue GetElementsByTagName(JSContext* ctx, JSValueConst this_val, int argc,
                                        JSValueConst* argv) {
        ScriptHostImpl* host = HostOf(ctx);
        if (host == nullptr) {
            return JS_UNDEFINED;
        }
        if (argc < 1) {
            return host->MakeNodeList({});
        }
        const char* tag = JS_ToCString(ctx, argv[0]);
        if (tag == nullptr) {
            return JS_EXCEPTION;
        }
        const Node* node = NodeOf(ctx, this_val);
        const Node* root = node != nullptr ? node : host->document_;
        std::vector<Element*> elements;
        web::CollectElementsByTag(*root, tag, &elements);
        JS_FreeCString(ctx, tag);
        return host->MakeNodeList(elements);
    }

    // 查询根节点：document 查询整篇文档，元素查询自身子树。
    static const Node* QueryRoot(JSContext* ctx, JSValueConst this_val) {
        Node* node = NodeOf(ctx, this_val);
        if (node != nullptr) {
            return node;
        }
        ScriptHostImpl* host = HostOf(ctx);
        return host != nullptr ? host->document_ : nullptr;
    }

    // closest：从当前元素开始沿祖先链查找第一个匹配选择器的元素。
    static JSValue Closest(JSContext* ctx, JSValueConst this_val, int argc,
                           JSValueConst* argv) {
        ScriptHostImpl* host = HostOf(ctx);
        Element* element = ElementOf(ctx, this_val);
        if (host == nullptr || element == nullptr || argc < 1) {
            return JS_NULL;
        }
        const char* text = JS_ToCString(ctx, argv[0]);
        if (text == nullptr) {
            return JS_EXCEPTION;
        }
        std::vector<Selector> selectors;
        const bool parsed = ParseSingleSelector(text, &selectors);
        JS_FreeCString(ctx, text);
        if (!parsed || selectors.empty()) {
            return JS_NULL;
        }
        for (Node* node = element; node != nullptr; node = node->Parent()) {
            if (!node->IsElement()) {
                continue;
            }
            Element* candidate = static_cast<Element*>(node);
            if (web::MatchesSelector(selectors.front(), *candidate)) {
                return host->WrapperFor(candidate);
            }
        }
        return JS_NULL;
    }

    static JSValue QuerySelector(JSContext* ctx, JSValueConst this_val, int argc,
                                 JSValueConst* argv) {
        ScriptHostImpl* host = HostOf(ctx);
        const Node* root = QueryRoot(ctx, this_val);
        if (host == nullptr || root == nullptr) {
            return JS_UNDEFINED;
        }
        if (argc < 1) {
            return JS_NULL;
        }
        const char* text = JS_ToCString(ctx, argv[0]);
        if (text == nullptr) {
            return JS_EXCEPTION;
        }
        std::vector<Selector> selectors;
        const bool parsed = ParseSingleSelector(text, &selectors);
        JS_FreeCString(ctx, text);
        if (!parsed) {
            return JS_NULL;
        }
        std::vector<Element*> matches = web::QuerySelectorAll(*root, selectors);
        if (matches.empty()) {
            return JS_NULL;
        }
        return host->WrapperFor(matches.front());
    }

    static JSValue QuerySelectorAll(JSContext* ctx, JSValueConst this_val, int argc,
                                    JSValueConst* argv) {
        ScriptHostImpl* host = HostOf(ctx);
        const Node* root = QueryRoot(ctx, this_val);
        if (host == nullptr || root == nullptr) {
            return JS_UNDEFINED;
        }
        if (argc < 1) {
            return host->MakeNodeList({});
        }
        const char* text = JS_ToCString(ctx, argv[0]);
        if (text == nullptr) {
            return JS_EXCEPTION;
        }
        std::vector<Selector> selectors;
        const bool parsed = ParseSingleSelector(text, &selectors);
        JS_FreeCString(ctx, text);
        if (!parsed) {
            return host->MakeNodeList({});
        }
        return host->MakeNodeList(web::QuerySelectorAll(*root, selectors));
    }

    static JSValue CreateElement(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
        ScriptHostImpl* host = HostOf(ctx);
        if (host == nullptr) {
            return JS_UNDEFINED;
        }
        if (argc < 1) {
            return ThrowType(ctx, "createElement 需要一个标签名");
        }
        const char* tag = JS_ToCString(ctx, argv[0]);
        if (tag == nullptr) {
            return JS_EXCEPTION;
        }
        ElementPtr element = host->document_->CreateElement(tag);
        JS_FreeCString(ctx, tag);
        return host->WrapperFor(element.get());
    }

    static JSValue CreateTextNode(JSContext* ctx, JSValueConst, int argc,
                                  JSValueConst* argv) {
        ScriptHostImpl* host = HostOf(ctx);
        if (host == nullptr) {
            return JS_UNDEFINED;
        }
        if (argc < 1) {
            return ThrowType(ctx, "createTextNode 需要一个文本参数");
        }
        const char* text = JS_ToCString(ctx, argv[0]);
        if (text == nullptr) {
            return JS_EXCEPTION;
        }
        TextNodePtr node = host->document_->CreateTextNode(text);
        JS_FreeCString(ctx, text);
        return host->WrapperFor(node.get());
    }

    static JSValue BodyGet(JSContext* ctx, JSValueConst, int) {
        ScriptHostImpl* host = HostOf(ctx);
        if (host == nullptr) {
            return JS_NULL;
        }
        return host->WrapperOrNull(host->document_->Body());
    }

    static JSValue DocumentElementGet(JSContext* ctx, JSValueConst, int) {
        ScriptHostImpl* host = HostOf(ctx);
        if (host == nullptr) {
            return JS_NULL;
        }
        return host->WrapperOrNull(host->document_->RootElement());
    }

    // document.readyState：脚本执行阶段为 loading，DOMContentLoaded 派发后为
    // interactive，随后置为 complete。
    static JSValue ReadyStateGet(JSContext* ctx, JSValueConst, int) {
        ScriptHostImpl* host = HostOf(ctx);
        const char* state = host != nullptr ? host->ready_state_.c_str() : "complete";
        return JS_NewString(ctx, state);
    }

    // document.cookie：以“名称=值”保存，读取时用 "; " 连接。路径、过期时间
    // 等属性当前不参与存储。
    static JSValue CookieGet(JSContext* ctx, JSValueConst, int) {
        ScriptHostImpl* host = HostOf(ctx);
        if (host == nullptr) {
            return JS_NewString(ctx, "");
        }
        std::string text;
        for (const auto& entry : host->cookies_) {
            if (!text.empty()) {
                text += "; ";
            }
            text += entry.first + "=" + entry.second;
        }
        return JS_NewString(ctx, text.c_str());
    }

    static JSValue CookieSet(JSContext* ctx, JSValueConst, JSValueConst value, int) {
        ScriptHostImpl* host = HostOf(ctx);
        if (host == nullptr) {
            return JS_UNDEFINED;
        }
        const char* text = JS_ToCString(ctx, value);
        if (text == nullptr) {
            return JS_EXCEPTION;
        }
        const std::string raw = text;
        JS_FreeCString(ctx, text);
        // 第一段是 名称=值，其余段是属性（expires、path 等）。
        const size_t semicolon = raw.find(';');
        const std::string pair = semicolon == std::string::npos ? raw : raw.substr(0, semicolon);
        const size_t equals = pair.find('=');
        if (equals != std::string::npos) {
            const std::string name = TrimSpace(pair.substr(0, equals));
            if (!name.empty()) {
                host->cookies_[name] = TrimSpace(pair.substr(equals + 1));
            }
        }
        return JS_UNDEFINED;
    }

    // 深度优先查找当前获得焦点的元素，供 document.activeElement 使用。
    static const Element* FindFocusedElement(const Node& node) {
        if (node.IsElement() && static_cast<const Element&>(node).focused) {
            return static_cast<const Element*>(&node);
        }
        for (const NodePtr& child : node.Children()) {
            if (!child) {
                continue;
            }
            const Element* found = FindFocusedElement(*child);
            if (found != nullptr) {
                return found;
            }
        }
        return nullptr;
    }

    static JSValue ActiveElementGet(JSContext* ctx, JSValueConst, int) {
        ScriptHostImpl* host = HostOf(ctx);
        if (host == nullptr || host->document_ == nullptr) {
            return JS_NULL;
        }
        const Element* focused = FindFocusedElement(*host->document_);
        return host->WrapperOrNull(const_cast<Element*>(focused));
    }

    // document.execCommand：目前只处理 copy，把 element.select() 记录的
    // 选中文本写入系统剪贴板。
    static JSValue DocumentExecCommand(JSContext* ctx, JSValueConst, int argc,
                                       JSValueConst* argv) {
        ScriptHostImpl* host = HostOf(ctx);
        if (host == nullptr || argc < 1) {
            return JS_FALSE;
        }
        const char* command = JS_ToCString(ctx, argv[0]);
        if (command == nullptr) {
            return JS_EXCEPTION;
        }
        const std::string name = AsciiLower(command);
        JS_FreeCString(ctx, command);
        if (name != "copy") {
            return JS_FALSE;
        }
        if (host->selected_text_.empty()) {
            return JS_TRUE;
        }
        return JS_NewBool(ctx, WriteClipboardText(host->selected_text_));
    }

    // ---------------------------------------------------------------
    // style 对象
    // ---------------------------------------------------------------
    static JSValue StylePropertyGet(JSContext* ctx, JSValueConst this_val, int magic) {
        if (magic < 0 || magic >= kStyleBindingCount) {
            return JS_UNDEFINED;
        }
        Element* element = StyleElementOf(ctx, this_val);
        if (element == nullptr) {
            return JS_UNDEFINED;
        }
        std::string value = ReadStyleProperty(*element, kStyleBindings[magic].css_name);
        return JS_NewString(ctx, value.c_str());
    }

    static JSValue StylePropertySet(JSContext* ctx, JSValueConst this_val, JSValueConst value,
                                    int magic) {
        if (magic < 0 || magic >= kStyleBindingCount) {
            return JS_UNDEFINED;
        }
        ScriptHostImpl* host = HostOf(ctx);
        Element* element = StyleElementOf(ctx, this_val);
        if (host == nullptr || element == nullptr) {
            return JS_UNDEFINED;
        }
        const char* text = JS_ToCString(ctx, value);
        if (text == nullptr) {
            return JS_EXCEPTION;
        }
        WriteStyleProperty(*element, kStyleBindings[magic].css_name, text);
        JS_FreeCString(ctx, text);
        host->NotifyChanged(element);
        return JS_UNDEFINED;
    }

    // ---------------------------------------------------------------
    // Canvas 2D 上下文
    // ---------------------------------------------------------------
    // 仅接受 "2d" 类型，其它类型返回 null。
    static JSValue GetContext(JSContext* ctx, JSValueConst this_val, int argc,
                              JSValueConst* argv) {
        ScriptHostImpl* host = HostOf(ctx);
        Element* element = ElementOf(ctx, this_val);
        if (host == nullptr || element == nullptr || argc < 1) {
            return JS_NULL;
        }
        const char* type = JS_ToCString(ctx, argv[0]);
        if (type == nullptr) {
            return JS_EXCEPTION;
        }
        const std::string type_text = type;
        JS_FreeCString(ctx, type);
        if (type_text != "2d") {
            return JS_NULL;
        }
        return host->CanvasContextFor(element);
    }

    static JSValue CanvasFillStyleGet(JSContext* ctx, JSValueConst this_val, int) {
        CanvasRenderingContext2D* context = CanvasContextOf(ctx, this_val);
        if (context == nullptr) {
            return JS_UNDEFINED;
        }
        return JS_NewString(ctx, context->FillStyle().c_str());
    }

    static JSValue CanvasFillStyleSet(JSContext* ctx, JSValueConst this_val, JSValueConst value,
                                      int) {
        CanvasRenderingContext2D* context = CanvasContextOf(ctx, this_val);
        if (context == nullptr) {
            return JS_UNDEFINED;
        }
        GradientWrapperData* gradient = GradientWrapperOf(ctx, value);
        if (gradient != nullptr) {
            context->SetFillGradient(gradient->gradient);
            return JS_UNDEFINED;
        }
        PatternWrapperData* pattern = PatternWrapperOf(ctx, value);
        if (pattern != nullptr && pattern->pattern) {
            context->SetFillPattern(pattern->pattern);
            return JS_UNDEFINED;
        }
        const char* text = JS_ToCString(ctx, value);
        if (text == nullptr) {
            return JS_EXCEPTION;
        }
        const std::string style = text;
        JS_FreeCString(ctx, text);
        context->SetFillStyle(style);
        return JS_UNDEFINED;
    }

    static JSValue CanvasStrokeStyleGet(JSContext* ctx, JSValueConst this_val, int) {
        CanvasRenderingContext2D* context = CanvasContextOf(ctx, this_val);
        if (context == nullptr) {
            return JS_UNDEFINED;
        }
        return JS_NewString(ctx, context->StrokeStyle().c_str());
    }

    static JSValue CanvasStrokeStyleSet(JSContext* ctx, JSValueConst this_val,
                                        JSValueConst value, int) {
        CanvasRenderingContext2D* context = CanvasContextOf(ctx, this_val);
        if (context == nullptr) {
            return JS_UNDEFINED;
        }
        GradientWrapperData* gradient = GradientWrapperOf(ctx, value);
        if (gradient != nullptr) {
            context->SetStrokeGradient(gradient->gradient);
            return JS_UNDEFINED;
        }
        PatternWrapperData* pattern = PatternWrapperOf(ctx, value);
        if (pattern != nullptr && pattern->pattern) {
            context->SetStrokePattern(pattern->pattern);
            return JS_UNDEFINED;
        }
        const char* text = JS_ToCString(ctx, value);
        if (text == nullptr) {
            return JS_EXCEPTION;
        }
        const std::string style = text;
        JS_FreeCString(ctx, text);
        context->SetStrokeStyle(style);
        return JS_UNDEFINED;
    }

    static JSValue CanvasFontGet(JSContext* ctx, JSValueConst this_val, int) {
        CanvasRenderingContext2D* context = CanvasContextOf(ctx, this_val);
        if (context == nullptr) {
            return JS_UNDEFINED;
        }
        return JS_NewString(ctx, context->Font().c_str());
    }

    static JSValue CanvasFontSet(JSContext* ctx, JSValueConst this_val, JSValueConst value,
                                 int) {
        CanvasRenderingContext2D* context = CanvasContextOf(ctx, this_val);
        if (context == nullptr) {
            return JS_UNDEFINED;
        }
        const char* text = JS_ToCString(ctx, value);
        if (text == nullptr) {
            return JS_EXCEPTION;
        }
        const std::string font = text;
        JS_FreeCString(ctx, text);
        context->SetFont(font);
        return JS_UNDEFINED;
    }

    static JSValue CanvasLineWidthGet(JSContext* ctx, JSValueConst this_val, int) {
        CanvasRenderingContext2D* context = CanvasContextOf(ctx, this_val);
        if (context == nullptr) {
            return JS_UNDEFINED;
        }
        return JS_NewFloat64(ctx, context->LineWidth());
    }

    static JSValue CanvasLineWidthSet(JSContext* ctx, JSValueConst this_val, JSValueConst value,
                                      int) {
        CanvasRenderingContext2D* context = CanvasContextOf(ctx, this_val);
        if (context == nullptr) {
            return JS_UNDEFINED;
        }
        double number = 0.0;
        if (JS_ToFloat64(ctx, &number, value) < 0) {
            return JS_EXCEPTION;
        }
        context->SetLineWidth(static_cast<float>(number));
        return JS_UNDEFINED;
    }

    static JSValue CanvasFillRect(JSContext* ctx, JSValueConst this_val, int argc,
                                  JSValueConst* argv) {
        CanvasRenderingContext2D* context = CanvasContextOf(ctx, this_val);
        if (context == nullptr || argc < 4) {
            return JS_UNDEFINED;
        }
        float x = 0.0f;
        float y = 0.0f;
        float width = 0.0f;
        float height = 0.0f;
        if (!ReadNumber(ctx, argv[0], &x) || !ReadNumber(ctx, argv[1], &y) ||
            !ReadNumber(ctx, argv[2], &width) || !ReadNumber(ctx, argv[3], &height)) {
            return JS_EXCEPTION;
        }
        context->FillRect(x, y, width, height);
        ReportCanvasContextDebug("fillRect", context);
        return JS_UNDEFINED;
    }

    static JSValue CanvasClearRect(JSContext* ctx, JSValueConst this_val, int argc,
                                   JSValueConst* argv) {
        CanvasRenderingContext2D* context = CanvasContextOf(ctx, this_val);
        if (context == nullptr || argc < 4) {
            return JS_UNDEFINED;
        }
        float x = 0.0f;
        float y = 0.0f;
        float width = 0.0f;
        float height = 0.0f;
        if (!ReadNumber(ctx, argv[0], &x) || !ReadNumber(ctx, argv[1], &y) ||
            !ReadNumber(ctx, argv[2], &width) || !ReadNumber(ctx, argv[3], &height)) {
            return JS_EXCEPTION;
        }
        context->ClearRect(x, y, width, height);
        ReportCanvasContextDebug("clearRect", context);
        return JS_UNDEFINED;
    }

    static JSValue CanvasStrokeRect(JSContext* ctx, JSValueConst this_val, int argc,
                                    JSValueConst* argv) {
        CanvasRenderingContext2D* context = CanvasContextOf(ctx, this_val);
        if (context == nullptr || argc < 4) {
            return JS_UNDEFINED;
        }
        float x = 0.0f;
        float y = 0.0f;
        float width = 0.0f;
        float height = 0.0f;
        if (!ReadNumber(ctx, argv[0], &x) || !ReadNumber(ctx, argv[1], &y) ||
            !ReadNumber(ctx, argv[2], &width) || !ReadNumber(ctx, argv[3], &height)) {
            return JS_EXCEPTION;
        }
        context->StrokeRect(x, y, width, height);
        ReportCanvasContextDebug("strokeRect", context);
        return JS_UNDEFINED;
    }

    static JSValue CanvasFillText(JSContext* ctx, JSValueConst this_val, int argc,
                                  JSValueConst* argv) {
        CanvasRenderingContext2D* context = CanvasContextOf(ctx, this_val);
        if (context == nullptr || argc < 3) {
            return JS_UNDEFINED;
        }
        const char* text = JS_ToCString(ctx, argv[0]);
        if (text == nullptr) {
            return JS_EXCEPTION;
        }
        const std::string content = text;
        JS_FreeCString(ctx, text);
        float x = 0.0f;
        float y = 0.0f;
        if (!ReadNumber(ctx, argv[1], &x) || !ReadNumber(ctx, argv[2], &y)) {
            return JS_EXCEPTION;
        }
        context->FillText(content, x, y);
        ReportCanvasContextDebug("fillText", context);
        return JS_UNDEFINED;
    }

    static JSValue CanvasMeasureText(JSContext* ctx, JSValueConst this_val, int argc,
                                     JSValueConst* argv) {
        CanvasRenderingContext2D* context = CanvasContextOf(ctx, this_val);
        if (context == nullptr) {
            return MakeMeasureText(ctx, 0.0f);
        }
        if (argc < 1) {
            return MakeMeasureText(ctx, 0.0f);
        }
        const char* text = JS_ToCString(ctx, argv[0]);
        if (text == nullptr) {
            return JS_EXCEPTION;
        }
        const std::string content = text;
        JS_FreeCString(ctx, text);
        return MakeMeasureText(ctx, context->MeasureText(content));
    }

    // ---------------------------------------------------------------
    // Canvas 路径、状态栈、变换、渐变与图片
    // ---------------------------------------------------------------
    static JSValue CanvasBeginPath(JSContext* ctx, JSValueConst this_val, int,
                                   JSValueConst*) {
        CanvasRenderingContext2D* context = CanvasContextOf(ctx, this_val);
        if (context != nullptr) {
            context->BeginPath();
        }
        return JS_UNDEFINED;
    }

    static JSValue CanvasMoveTo(JSContext* ctx, JSValueConst this_val, int argc,
                                JSValueConst* argv) {
        CanvasRenderingContext2D* context = CanvasContextOf(ctx, this_val);
        if (context == nullptr || argc < 2) {
            return JS_UNDEFINED;
        }
        float values[2];
        if (!ReadNumbers(ctx, argv, 2, values)) {
            return JS_EXCEPTION;
        }
        context->MoveTo(values[0], values[1]);
        return JS_UNDEFINED;
    }

    static JSValue CanvasLineTo(JSContext* ctx, JSValueConst this_val, int argc,
                                JSValueConst* argv) {
        CanvasRenderingContext2D* context = CanvasContextOf(ctx, this_val);
        if (context == nullptr || argc < 2) {
            return JS_UNDEFINED;
        }
        float values[2];
        if (!ReadNumbers(ctx, argv, 2, values)) {
            return JS_EXCEPTION;
        }
        context->LineTo(values[0], values[1]);
        return JS_UNDEFINED;
    }

    static JSValue CanvasQuadraticCurveTo(JSContext* ctx, JSValueConst this_val, int argc,
                                          JSValueConst* argv) {
        CanvasRenderingContext2D* context = CanvasContextOf(ctx, this_val);
        if (context == nullptr || argc < 4) {
            return JS_UNDEFINED;
        }
        float values[4];
        if (!ReadNumbers(ctx, argv, 4, values)) {
            return JS_EXCEPTION;
        }
        context->QuadraticCurveTo(values[0], values[1], values[2], values[3]);
        return JS_UNDEFINED;
    }

    static JSValue CanvasBezierCurveTo(JSContext* ctx, JSValueConst this_val, int argc,
                                       JSValueConst* argv) {
        CanvasRenderingContext2D* context = CanvasContextOf(ctx, this_val);
        if (context == nullptr || argc < 6) {
            return JS_UNDEFINED;
        }
        float values[6];
        if (!ReadNumbers(ctx, argv, 6, values)) {
            return JS_EXCEPTION;
        }
        context->BezierCurveTo(values[0], values[1], values[2], values[3], values[4], values[5]);
        return JS_UNDEFINED;
    }

    static JSValue CanvasArc(JSContext* ctx, JSValueConst this_val, int argc,
                             JSValueConst* argv) {
        CanvasRenderingContext2D* context = CanvasContextOf(ctx, this_val);
        if (context == nullptr || argc < 5) {
            return JS_UNDEFINED;
        }
        float values[5];
        if (!ReadNumbers(ctx, argv, 5, values)) {
            return JS_EXCEPTION;
        }
        bool counterclockwise = false;
        if (argc >= 6) {
            const int flag = JS_ToBool(ctx, argv[5]);
            if (flag < 0) {
                return JS_EXCEPTION;
            }
            counterclockwise = flag != 0;
        }
        context->Arc(values[0], values[1], values[2], values[3], values[4], counterclockwise);
        return JS_UNDEFINED;
    }

    static JSValue CanvasClosePath(JSContext* ctx, JSValueConst this_val, int,
                                   JSValueConst*) {
        CanvasRenderingContext2D* context = CanvasContextOf(ctx, this_val);
        if (context != nullptr) {
            context->ClosePath();
        }
        return JS_UNDEFINED;
    }

    static JSValue CanvasFillPath(JSContext* ctx, JSValueConst this_val, int,
                                  JSValueConst*) {
        CanvasRenderingContext2D* context = CanvasContextOf(ctx, this_val);
        if (context == nullptr) {
            return JS_UNDEFINED;
        }
        context->Fill();
        ReportCanvasContextDebug("fill", context);
        return JS_UNDEFINED;
    }

    static JSValue CanvasStrokePath(JSContext* ctx, JSValueConst this_val, int,
                                    JSValueConst*) {
        CanvasRenderingContext2D* context = CanvasContextOf(ctx, this_val);
        if (context == nullptr) {
            return JS_UNDEFINED;
        }
        context->Stroke();
        ReportCanvasContextDebug("stroke", context);
        return JS_UNDEFINED;
    }

    static JSValue CanvasSave(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
        CanvasRenderingContext2D* context = CanvasContextOf(ctx, this_val);
        if (context != nullptr) {
            context->Save();
        }
        return JS_UNDEFINED;
    }

    static JSValue CanvasRestore(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
        CanvasRenderingContext2D* context = CanvasContextOf(ctx, this_val);
        if (context != nullptr) {
            context->Restore();
        }
        return JS_UNDEFINED;
    }

    static JSValue CanvasTranslate(JSContext* ctx, JSValueConst this_val, int argc,
                                   JSValueConst* argv) {
        CanvasRenderingContext2D* context = CanvasContextOf(ctx, this_val);
        if (context == nullptr || argc < 2) {
            return JS_UNDEFINED;
        }
        float values[2];
        if (!ReadNumbers(ctx, argv, 2, values)) {
            return JS_EXCEPTION;
        }
        context->Translate(values[0], values[1]);
        return JS_UNDEFINED;
    }

    static JSValue CanvasScale(JSContext* ctx, JSValueConst this_val, int argc,
                               JSValueConst* argv) {
        CanvasRenderingContext2D* context = CanvasContextOf(ctx, this_val);
        if (context == nullptr || argc < 2) {
            return JS_UNDEFINED;
        }
        float values[2];
        if (!ReadNumbers(ctx, argv, 2, values)) {
            return JS_EXCEPTION;
        }
        context->Scale(values[0], values[1]);
        return JS_UNDEFINED;
    }

    static JSValue CanvasRotate(JSContext* ctx, JSValueConst this_val, int argc,
                                JSValueConst* argv) {
        CanvasRenderingContext2D* context = CanvasContextOf(ctx, this_val);
        if (context == nullptr || argc < 1) {
            return JS_UNDEFINED;
        }
        float angle = 0.0f;
        if (!ReadNumber(ctx, argv[0], &angle)) {
            return JS_EXCEPTION;
        }
        context->Rotate(angle);
        return JS_UNDEFINED;
    }

    static JSValue CanvasTransform(JSContext* ctx, JSValueConst this_val, int argc,
                                   JSValueConst* argv) {
        CanvasRenderingContext2D* context = CanvasContextOf(ctx, this_val);
        if (context == nullptr || argc < 6) {
            return JS_UNDEFINED;
        }
        float values[6];
        if (!ReadNumbers(ctx, argv, 6, values)) {
            return JS_EXCEPTION;
        }
        context->Transform(values[0], values[1], values[2], values[3], values[4], values[5]);
        return JS_UNDEFINED;
    }

    static JSValue CanvasSetTransform(JSContext* ctx, JSValueConst this_val, int argc,
                                      JSValueConst* argv) {
        CanvasRenderingContext2D* context = CanvasContextOf(ctx, this_val);
        if (context == nullptr || argc < 6) {
            return JS_UNDEFINED;
        }
        float values[6];
        if (!ReadNumbers(ctx, argv, 6, values)) {
            return JS_EXCEPTION;
        }
        context->SetTransform(values[0], values[1], values[2], values[3], values[4], values[5]);
        return JS_UNDEFINED;
    }

    static JSValue CanvasResetTransform(JSContext* ctx, JSValueConst this_val, int,
                                        JSValueConst*) {
        CanvasRenderingContext2D* context = CanvasContextOf(ctx, this_val);
        if (context != nullptr) {
            context->ResetTransform();
        }
        return JS_UNDEFINED;
    }

    static JSValue CanvasCreateLinearGradient(JSContext* ctx, JSValueConst this_val, int argc,
                                              JSValueConst* argv) {
        ScriptHostImpl* host = HostOf(ctx);
        CanvasRenderingContext2D* context = CanvasContextOf(ctx, this_val);
        if (host == nullptr || context == nullptr) {
            return JS_NULL;
        }
        if (argc < 4) {
            return ThrowType(ctx, "createLinearGradient 需要四个数值参数");
        }
        float values[4];
        if (!ReadNumbers(ctx, argv, 4, values)) {
            return JS_EXCEPTION;
        }
        return host->GradientWrapperFor(CanvasGradient::CreateLinear(
            values[0], values[1], values[2], values[3]));
    }

    static JSValue CanvasCreateRadialGradient(JSContext* ctx, JSValueConst this_val, int argc,
                                              JSValueConst* argv) {
        ScriptHostImpl* host = HostOf(ctx);
        CanvasRenderingContext2D* context = CanvasContextOf(ctx, this_val);
        if (host == nullptr || context == nullptr) {
            return JS_NULL;
        }
        if (argc < 6) {
            return ThrowType(ctx, "createRadialGradient 需要六个数值参数");
        }
        float values[6];
        if (!ReadNumbers(ctx, argv, 6, values)) {
            return JS_EXCEPTION;
        }
        return host->GradientWrapperFor(CanvasGradient::CreateRadial(
            values[0], values[1], values[2], values[3], values[4], values[5]));
    }

    // 解析 createPattern 的源：canvas 复制表面像素，img 解码位图。
    static bool ResolvePatternImage(JSContext* ctx, ScriptHostImpl* host, JSValueConst value,
                                    ImageData* image) {
        WrapperData* context_data = WrapperOf(ctx, value, host->canvas_class_id_);
        Node* node = context_data != nullptr ? context_data->Get() : NodeOf(ctx, value);
        if (node == nullptr || !node->IsElement()) {
            return false;
        }
        Element* element = static_cast<Element*>(node);
        const std::string tag = AsciiLower(element->TagName());
        if (tag == "canvas") {
            const CanvasSurface* surface = FindCanvasSurface(*element);
            if (surface == nullptr || surface->width <= 0 || surface->height <= 0) {
                return false;
            }
            image->width = surface->width;
            image->height = surface->height;
            image->pixels = surface->pixels;
            return image->Valid();
        }
        if (tag == "img") {
            const ImageData* loaded = LoadCanvasImage(element->GetAttribute("src"));
            if (loaded == nullptr || !loaded->Valid()) {
                return false;
            }
            *image = *loaded;
            return true;
        }
        return false;
    }

    static JSValue CanvasCreatePattern(JSContext* ctx, JSValueConst this_val, int argc,
                                       JSValueConst* argv) {
        ScriptHostImpl* host = HostOf(ctx);
        CanvasRenderingContext2D* context = CanvasContextOf(ctx, this_val);
        if (host == nullptr || context == nullptr || argc < 1) {
            return JS_NULL;
        }
        std::string repetition = "repeat";
        if (argc >= 2) {
            const char* text = JS_ToCString(ctx, argv[1]);
            if (text == nullptr) {
                return JS_EXCEPTION;
            }
            const std::string value = text;
            JS_FreeCString(ctx, text);
            if (value == "repeat" || value == "repeat-x" || value == "repeat-y" ||
                value == "no-repeat") {
                repetition = value;
            }
        }
        ImageData image;
        if (!ResolvePatternImage(ctx, host, argv[0], &image)) {
            return JS_NULL;
        }
        CanvasPattern::Repetition mode = CanvasPattern::Repetition::Repeat;
        if (repetition == "repeat-x") {
            mode = CanvasPattern::Repetition::RepeatX;
        } else if (repetition == "repeat-y") {
            mode = CanvasPattern::Repetition::RepeatY;
        } else if (repetition == "no-repeat") {
            mode = CanvasPattern::Repetition::NoRepeat;
        }
        auto pattern = std::make_shared<CanvasPattern>(std::move(image), mode,
                                                       context->CurrentTransform());
        return host->PatternWrapperFor(std::move(pattern));
    }

    static JSValue GradientAddColorStop(JSContext* ctx, JSValueConst this_val, int argc,
                                        JSValueConst* argv) {
        CanvasGradient* gradient = GradientOf(ctx, this_val);
        if (gradient == nullptr) {
            return ThrowType(ctx, "addColorStop 的接收者不是渐变对象");
        }
        if (argc < 2) {
            return ThrowType(ctx, "addColorStop 需要位置与颜色");
        }
        float offset = 0.0f;
        if (!ReadNumber(ctx, argv[0], &offset)) {
            return JS_EXCEPTION;
        }
        const char* text = JS_ToCString(ctx, argv[1]);
        if (text == nullptr) {
            return JS_EXCEPTION;
        }
        const std::string color = text;
        JS_FreeCString(ctx, text);
        gradient->AddColorStop(offset, color);
        return JS_UNDEFINED;
    }

    // 解析 drawImage 的源：画布上下文或画布元素取像素表面，img 元素解码位图；
    // 其它类型视为非法源，由调用方静默忽略。
    static bool ResolveDrawImageSource(JSContext* ctx, ScriptHostImpl* host,
                                       JSValueConst value, const CanvasSurface** surface,
                                       const ImageData** image) {
        WrapperData* context_data = WrapperOf(ctx, value, host->canvas_class_id_);
        Node* node = context_data != nullptr ? context_data->Get() : NodeOf(ctx, value);
        if (node == nullptr || !node->IsElement()) {
            return false;
        }
        Element* element = static_cast<Element*>(node);
        const std::string tag = AsciiLower(element->TagName());
        if (tag == "canvas") {
            *surface = FindCanvasSurface(*element);
            return *surface != nullptr;
        }
        if (tag == "img") {
            *image = LoadCanvasImage(element->GetAttribute("src"));
            return *image != nullptr;
        }
        return false;
    }

    static JSValue CanvasDrawImage(JSContext* ctx, JSValueConst this_val, int argc,
                                   JSValueConst* argv) {
        ScriptHostImpl* host = HostOf(ctx);
        CanvasRenderingContext2D* context = CanvasContextOf(ctx, this_val);
        if (host == nullptr || context == nullptr || argc < 3) {
            return JS_UNDEFINED;
        }
        const CanvasSurface* source_surface = nullptr;
        const ImageData* source_image = nullptr;
        if (!ResolveDrawImageSource(ctx, host, argv[0], &source_surface, &source_image)) {
            return JS_UNDEFINED;
        }

        if (argc >= 9) {
            float values[8];
            if (!ReadNumbers(ctx, argv + 1, 8, values)) {
                return JS_EXCEPTION;
            }
            if (source_surface != nullptr) {
                context->DrawImage(*source_surface, values[0], values[1], values[2], values[3],
                                   values[4], values[5], values[6], values[7]);
            } else {
                context->DrawImage(*source_image, values[0], values[1], values[2], values[3],
                                   values[4], values[5], values[6], values[7]);
            }
            ReportCanvasContextDebug("drawImage", context);
            return JS_UNDEFINED;
        }
        if (argc >= 5) {
            float values[4];
            if (!ReadNumbers(ctx, argv + 1, 4, values)) {
                return JS_EXCEPTION;
            }
            if (source_surface != nullptr) {
                context->DrawImage(*source_surface, values[0], values[1], values[2], values[3]);
            } else {
                context->DrawImage(*source_image, values[0], values[1], values[2], values[3]);
            }
            ReportCanvasContextDebug("drawImage", context);
            return JS_UNDEFINED;
        }
        float offset_x = 0.0f;
        float offset_y = 0.0f;
        if (!ReadNumber(ctx, argv[1], &offset_x) || !ReadNumber(ctx, argv[2], &offset_y)) {
            return JS_EXCEPTION;
        }
        if (source_surface != nullptr) {
            context->DrawImage(*source_surface, offset_x, offset_y);
        } else {
            context->DrawImage(*source_image, offset_x, offset_y);
        }
        ReportCanvasContextDebug("drawImage", context);
        return JS_UNDEFINED;
    }

    // 单次 ImageData 读写的像素数据上限，避免脚本申请超大缓冲。
    static constexpr size_t kMaxImageDataBytes = 64u * 1024u * 1024u;

    static bool ImageDataSizeAllowed(int width, int height) {
        if (width <= 0 || height <= 0) {
            return false;
        }
        const size_t bytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
        return bytes <= kMaxImageDataBytes;
    }

    // 创建 ImageData 对象：{ width, height, data: Uint8ClampedArray }。
    // pixels 为空时填充透明黑；长度不足时按实际大小复制。
    static JSValue MakeImageData(JSContext* ctx, int width, int height, const uint8_t* pixels) {
        const size_t byte_length = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
        JSValue size_argument = JS_NewInt64(ctx, static_cast<int64_t>(byte_length));
        if (JS_IsException(size_argument)) {
            return size_argument;
        }
        JSValue data = JS_NewTypedArray(ctx, 1, &size_argument, JS_TYPED_ARRAY_UINT8C);
        JS_FreeValue(ctx, size_argument);
        if (JS_IsException(data)) {
            return data;
        }
        size_t byte_offset = 0;
        size_t buffer_length = 0;
        size_t bytes_per_element = 1;
        JSValue buffer =
            JS_GetTypedArrayBuffer(ctx, data, &byte_offset, &buffer_length, &bytes_per_element);
        if (!JS_IsException(buffer)) {
            size_t buffer_size = 0;
            uint8_t* bytes = JS_GetArrayBuffer(ctx, &buffer_size, buffer);
            if (bytes != nullptr && buffer_size >= byte_offset + byte_length) {
                if (pixels != nullptr) {
                    std::memcpy(bytes + byte_offset, pixels, byte_length);
                } else {
                    std::memset(bytes + byte_offset, 0, byte_length);
                }
            }
            JS_FreeValue(ctx, buffer);
        } else {
            // 取底层缓冲失败时清除异常，仍返回带空数据的对象。
            JS_FreeValue(ctx, JS_GetException(ctx));
        }

        JSValue object = JS_NewObject(ctx);
        if (JS_IsException(object)) {
            JS_FreeValue(ctx, data);
            return object;
        }
        JS_SetPropertyStr(ctx, object, "width", JS_NewInt32(ctx, width));
        JS_SetPropertyStr(ctx, object, "height", JS_NewInt32(ctx, height));
        JS_SetPropertyStr(ctx, object, "data", data);
        return object;
    }

    // 读取 ImageData 的像素：支持 Uint8ClampedArray、Uint8Array 与普通数组。
    // 尺寸非法或 data 类型不支持时返回 false。
    static bool ReadImageData(JSContext* ctx, JSValueConst value, int* width, int* height,
                              std::vector<uint8_t>* pixels) {
        JSValue width_value = JS_GetPropertyStr(ctx, value, "width");
        JSValue height_value = JS_GetPropertyStr(ctx, value, "height");
        int32_t pixel_width = 0;
        int32_t pixel_height = 0;
        const int width_status = JS_ToInt32(ctx, &pixel_width, width_value);
        const int height_status = JS_ToInt32(ctx, &pixel_height, height_value);
        JS_FreeValue(ctx, width_value);
        JS_FreeValue(ctx, height_value);
        if (width_status < 0 || height_status < 0 || pixel_width <= 0 || pixel_height <= 0 ||
            !ImageDataSizeAllowed(pixel_width, pixel_height)) {
            return false;
        }

        JSValue data = JS_GetPropertyStr(ctx, value, "data");
        if (JS_IsException(data)) {
            return false;
        }
        const size_t expected =
            static_cast<size_t>(pixel_width) * static_cast<size_t>(pixel_height) * 4u;
        bool ok = false;
        const int typed_array_type = JS_GetTypedArrayType(data);
        if (typed_array_type == JS_TYPED_ARRAY_UINT8C ||
            typed_array_type == JS_TYPED_ARRAY_UINT8) {
            size_t byte_offset = 0;
            size_t byte_length = 0;
            size_t bytes_per_element = 1;
            JSValue buffer = JS_GetTypedArrayBuffer(ctx, data, &byte_offset, &byte_length,
                                                    &bytes_per_element);
            if (!JS_IsException(buffer)) {
                size_t buffer_size = 0;
                uint8_t* bytes = JS_GetArrayBuffer(ctx, &buffer_size, buffer);
                if (bytes != nullptr && buffer_size >= byte_offset + byte_length) {
                    pixels->assign(expected, 0);
                    std::memcpy(pixels->data(), bytes + byte_offset,
                                std::min(expected, byte_length));
                    ok = true;
                }
                JS_FreeValue(ctx, buffer);
            } else {
                JS_FreeValue(ctx, JS_GetException(ctx));
            }
        } else if (JS_IsArray(data)) {
            pixels->assign(expected, 0);
            for (size_t index = 0; index < expected; ++index) {
                JSValue item = JS_GetPropertyUint32(ctx, data, static_cast<uint32_t>(index));
                uint32_t channel = 0;
                if (JS_ToUint32(ctx, &channel, item) < 0) {
                    JS_FreeValue(ctx, item);
                    JS_FreeValue(ctx, data);
                    return false;
                }
                JS_FreeValue(ctx, item);
                (*pixels)[index] = static_cast<uint8_t>(channel > 255u ? 255u : channel);
            }
            ok = true;
        }
        JS_FreeValue(ctx, data);
        if (ok) {
            *width = pixel_width;
            *height = pixel_height;
        }
        return ok;
    }

    static JSValue CanvasGlobalAlphaGet(JSContext* ctx, JSValueConst this_val, int) {
        CanvasRenderingContext2D* context = CanvasContextOf(ctx, this_val);
        if (context == nullptr) {
            return JS_UNDEFINED;
        }
        return JS_NewFloat64(ctx, static_cast<double>(context->GlobalAlpha()));
    }

    static JSValue CanvasGlobalAlphaSet(JSContext* ctx, JSValueConst this_val, JSValueConst value,
                                        int) {
        CanvasRenderingContext2D* context = CanvasContextOf(ctx, this_val);
        if (context == nullptr) {
            return JS_UNDEFINED;
        }
        double number = 0.0;
        if (JS_ToFloat64(ctx, &number, value) < 0) {
            return JS_EXCEPTION;
        }
        context->SetGlobalAlpha(static_cast<float>(number));
        return JS_UNDEFINED;
    }

    static JSValue CanvasGlobalCompositeOperationGet(JSContext* ctx, JSValueConst this_val,
                                                     int) {
        CanvasRenderingContext2D* context = CanvasContextOf(ctx, this_val);
        if (context == nullptr) {
            return JS_UNDEFINED;
        }
        return JS_NewString(ctx, context->GlobalCompositeOperation().c_str());
    }

    static JSValue CanvasGlobalCompositeOperationSet(JSContext* ctx, JSValueConst this_val,
                                                     JSValueConst value, int) {
        CanvasRenderingContext2D* context = CanvasContextOf(ctx, this_val);
        if (context == nullptr) {
            return JS_UNDEFINED;
        }
        const char* text = JS_ToCString(ctx, value);
        if (text == nullptr) {
            return JS_EXCEPTION;
        }
        std::string operation = text;
        JS_FreeCString(ctx, text);
        context->SetGlobalCompositeOperation(std::move(operation));
        return JS_UNDEFINED;
    }

    static JSValue CanvasShadowColorGet(JSContext* ctx, JSValueConst this_val, int) {
        CanvasRenderingContext2D* context = CanvasContextOf(ctx, this_val);
        if (context == nullptr) {
            return JS_UNDEFINED;
        }
        return JS_NewString(ctx, context->ShadowColor().c_str());
    }

    static JSValue CanvasShadowColorSet(JSContext* ctx, JSValueConst this_val,
                                        JSValueConst value, int) {
        CanvasRenderingContext2D* context = CanvasContextOf(ctx, this_val);
        if (context == nullptr) {
            return JS_UNDEFINED;
        }
        const char* text = JS_ToCString(ctx, value);
        if (text == nullptr) {
            return JS_EXCEPTION;
        }
        std::string color = text;
        JS_FreeCString(ctx, text);
        context->SetShadowColor(std::move(color));
        return JS_UNDEFINED;
    }

    static JSValue CanvasShadowBlurGet(JSContext* ctx, JSValueConst this_val, int) {
        CanvasRenderingContext2D* context = CanvasContextOf(ctx, this_val);
        if (context == nullptr) {
            return JS_UNDEFINED;
        }
        return JS_NewFloat64(ctx, static_cast<double>(context->ShadowBlur()));
    }

    static JSValue CanvasShadowBlurSet(JSContext* ctx, JSValueConst this_val, JSValueConst value,
                                       int) {
        CanvasRenderingContext2D* context = CanvasContextOf(ctx, this_val);
        if (context == nullptr) {
            return JS_UNDEFINED;
        }
        double number = 0.0;
        if (JS_ToFloat64(ctx, &number, value) < 0) {
            return JS_EXCEPTION;
        }
        context->SetShadowBlur(static_cast<float>(number));
        return JS_UNDEFINED;
    }

    static JSValue CanvasShadowOffsetXGet(JSContext* ctx, JSValueConst this_val, int) {
        CanvasRenderingContext2D* context = CanvasContextOf(ctx, this_val);
        if (context == nullptr) {
            return JS_UNDEFINED;
        }
        return JS_NewFloat64(ctx, static_cast<double>(context->ShadowOffsetX()));
    }

    static JSValue CanvasShadowOffsetXSet(JSContext* ctx, JSValueConst this_val,
                                          JSValueConst value, int) {
        CanvasRenderingContext2D* context = CanvasContextOf(ctx, this_val);
        if (context == nullptr) {
            return JS_UNDEFINED;
        }
        double number = 0.0;
        if (JS_ToFloat64(ctx, &number, value) < 0) {
            return JS_EXCEPTION;
        }
        context->SetShadowOffsetX(static_cast<float>(number));
        return JS_UNDEFINED;
    }

    static JSValue CanvasShadowOffsetYGet(JSContext* ctx, JSValueConst this_val, int) {
        CanvasRenderingContext2D* context = CanvasContextOf(ctx, this_val);
        if (context == nullptr) {
            return JS_UNDEFINED;
        }
        return JS_NewFloat64(ctx, static_cast<double>(context->ShadowOffsetY()));
    }

    static JSValue CanvasShadowOffsetYSet(JSContext* ctx, JSValueConst this_val,
                                          JSValueConst value, int) {
        CanvasRenderingContext2D* context = CanvasContextOf(ctx, this_val);
        if (context == nullptr) {
            return JS_UNDEFINED;
        }
        double number = 0.0;
        if (JS_ToFloat64(ctx, &number, value) < 0) {
            return JS_EXCEPTION;
        }
        context->SetShadowOffsetY(static_cast<float>(number));
        return JS_UNDEFINED;
    }

    static JSValue CanvasLineCapGet(JSContext* ctx, JSValueConst this_val, int) {
        CanvasRenderingContext2D* context = CanvasContextOf(ctx, this_val);
        if (context == nullptr) {
            return JS_UNDEFINED;
        }
        return JS_NewString(ctx, context->LineCap().c_str());
    }

    static JSValue CanvasLineCapSet(JSContext* ctx, JSValueConst this_val, JSValueConst value,
                                    int) {
        CanvasRenderingContext2D* context = CanvasContextOf(ctx, this_val);
        if (context == nullptr) {
            return JS_UNDEFINED;
        }
        const char* text = JS_ToCString(ctx, value);
        if (text == nullptr) {
            return JS_EXCEPTION;
        }
        std::string style = text;
        JS_FreeCString(ctx, text);
        context->SetLineCap(std::move(style));
        return JS_UNDEFINED;
    }

    static JSValue CanvasTextAlignGet(JSContext* ctx, JSValueConst this_val, int) {
        CanvasRenderingContext2D* context = CanvasContextOf(ctx, this_val);
        if (context == nullptr) {
            return JS_UNDEFINED;
        }
        return JS_NewString(ctx, context->TextAlign().c_str());
    }

    static JSValue CanvasTextAlignSet(JSContext* ctx, JSValueConst this_val, JSValueConst value,
                                      int) {
        CanvasRenderingContext2D* context = CanvasContextOf(ctx, this_val);
        if (context == nullptr) {
            return JS_UNDEFINED;
        }
        const char* text = JS_ToCString(ctx, value);
        if (text == nullptr) {
            return JS_EXCEPTION;
        }
        std::string style = text;
        JS_FreeCString(ctx, text);
        context->SetTextAlign(std::move(style));
        return JS_UNDEFINED;
    }

    static JSValue CanvasDirectionGet(JSContext* ctx, JSValueConst this_val, int) {
        CanvasRenderingContext2D* context = CanvasContextOf(ctx, this_val);
        if (context == nullptr) {
            return JS_UNDEFINED;
        }
        return JS_NewString(ctx, context->Direction().c_str());
    }

    static JSValue CanvasDirectionSet(JSContext* ctx, JSValueConst this_val, JSValueConst value,
                                      int) {
        CanvasRenderingContext2D* context = CanvasContextOf(ctx, this_val);
        if (context == nullptr) {
            return JS_UNDEFINED;
        }
        const char* text = JS_ToCString(ctx, value);
        if (text == nullptr) {
            return JS_EXCEPTION;
        }
        std::string direction = text;
        JS_FreeCString(ctx, text);
        context->SetDirection(std::move(direction));
        return JS_UNDEFINED;
    }

    static JSValue CanvasTextBaselineGet(JSContext* ctx, JSValueConst this_val, int) {
        CanvasRenderingContext2D* context = CanvasContextOf(ctx, this_val);
        if (context == nullptr) {
            return JS_UNDEFINED;
        }
        return JS_NewString(ctx, context->TextBaseline().c_str());
    }

    static JSValue CanvasTextBaselineSet(JSContext* ctx, JSValueConst this_val,
                                         JSValueConst value, int) {
        CanvasRenderingContext2D* context = CanvasContextOf(ctx, this_val);
        if (context == nullptr) {
            return JS_UNDEFINED;
        }
        const char* text = JS_ToCString(ctx, value);
        if (text == nullptr) {
            return JS_EXCEPTION;
        }
        std::string style = text;
        JS_FreeCString(ctx, text);
        context->SetTextBaseline(std::move(style));
        return JS_UNDEFINED;
    }

    static JSValue CanvasClip(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
        CanvasRenderingContext2D* context = CanvasContextOf(ctx, this_val);
        if (context == nullptr) {
            return JS_UNDEFINED;
        }
        context->Clip();
        return JS_UNDEFINED;
    }

    static JSValue CanvasLineJoinGet(JSContext* ctx, JSValueConst this_val, int) {
        CanvasRenderingContext2D* context = CanvasContextOf(ctx, this_val);
        if (context == nullptr) {
            return JS_UNDEFINED;
        }
        return JS_NewString(ctx, context->LineJoin().c_str());
    }

    static JSValue CanvasLineJoinSet(JSContext* ctx, JSValueConst this_val, JSValueConst value,
                                     int) {
        CanvasRenderingContext2D* context = CanvasContextOf(ctx, this_val);
        if (context == nullptr) {
            return JS_UNDEFINED;
        }
        const char* text = JS_ToCString(ctx, value);
        if (text == nullptr) {
            return JS_EXCEPTION;
        }
        std::string style = text;
        JS_FreeCString(ctx, text);
        context->SetLineJoin(std::move(style));
        return JS_UNDEFINED;
    }

    static JSValue CanvasMiterLimitGet(JSContext* ctx, JSValueConst this_val, int) {
        CanvasRenderingContext2D* context = CanvasContextOf(ctx, this_val);
        if (context == nullptr) {
            return JS_UNDEFINED;
        }
        return JS_NewFloat64(ctx, static_cast<double>(context->MiterLimit()));
    }

    static JSValue CanvasMiterLimitSet(JSContext* ctx, JSValueConst this_val, JSValueConst value,
                                       int) {
        CanvasRenderingContext2D* context = CanvasContextOf(ctx, this_val);
        if (context == nullptr) {
            return JS_UNDEFINED;
        }
        double number = 0.0;
        if (JS_ToFloat64(ctx, &number, value) < 0) {
            return JS_EXCEPTION;
        }
        context->SetMiterLimit(static_cast<float>(number));
        return JS_UNDEFINED;
    }

    static JSValue CanvasLineDashOffsetGet(JSContext* ctx, JSValueConst this_val, int) {
        CanvasRenderingContext2D* context = CanvasContextOf(ctx, this_val);
        if (context == nullptr) {
            return JS_UNDEFINED;
        }
        return JS_NewFloat64(ctx, static_cast<double>(context->LineDashOffset()));
    }

    static JSValue CanvasLineDashOffsetSet(JSContext* ctx, JSValueConst this_val,
                                           JSValueConst value, int) {
        CanvasRenderingContext2D* context = CanvasContextOf(ctx, this_val);
        if (context == nullptr) {
            return JS_UNDEFINED;
        }
        double number = 0.0;
        if (JS_ToFloat64(ctx, &number, value) < 0) {
            return JS_EXCEPTION;
        }
        context->SetLineDashOffset(static_cast<float>(number));
        return JS_UNDEFINED;
    }

    static JSValue CanvasSetLineDash(JSContext* ctx, JSValueConst this_val, int argc,
                                     JSValueConst* argv) {
        CanvasRenderingContext2D* context = CanvasContextOf(ctx, this_val);
        if (context == nullptr) {
            return JS_UNDEFINED;
        }
        if (argc < 1 || !JS_IsArray(argv[0])) {
            return ThrowType(ctx, "setLineDash 需要一个数组");
        }
        uint32_t length = 0;
        JSValue length_value = JS_GetPropertyStr(ctx, argv[0], "length");
        const int length_status = JS_ToUint32(ctx, &length, length_value);
        JS_FreeValue(ctx, length_value);
        if (length_status < 0) {
            return JS_EXCEPTION;
        }
        std::vector<float> pattern;
        pattern.reserve(length);
        for (uint32_t index = 0; index < length; ++index) {
            JSValue item = JS_GetPropertyUint32(ctx, argv[0], index);
            double number = 0.0;
            if (JS_ToFloat64(ctx, &number, item) < 0) {
                JS_FreeValue(ctx, item);
                return JS_EXCEPTION;
            }
            JS_FreeValue(ctx, item);
            pattern.push_back(static_cast<float>(number));
        }
        context->SetLineDash(std::move(pattern));
        return JS_UNDEFINED;
    }

    static JSValue CanvasGetLineDash(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
        CanvasRenderingContext2D* context = CanvasContextOf(ctx, this_val);
        if (context == nullptr) {
            return JS_UNDEFINED;
        }
        const std::vector<float>& pattern = context->LineDash();
        JSValue result = JS_NewArray(ctx);
        if (JS_IsException(result)) {
            return result;
        }
        for (size_t index = 0; index < pattern.size(); ++index) {
            JS_SetPropertyUint32(ctx, result, static_cast<uint32_t>(index),
                                 JS_NewFloat64(ctx, static_cast<double>(pattern[index])));
        }
        return result;
    }

    static JSValue CanvasRect(JSContext* ctx, JSValueConst this_val, int argc,
                              JSValueConst* argv) {
        CanvasRenderingContext2D* context = CanvasContextOf(ctx, this_val);
        if (context == nullptr || argc < 4) {
            return JS_UNDEFINED;
        }
        float values[4];
        if (!ReadNumbers(ctx, argv, 4, values)) {
            return JS_EXCEPTION;
        }
        context->Rect(values[0], values[1], values[2], values[3]);
        return JS_UNDEFINED;
    }

    static JSValue CanvasEllipse(JSContext* ctx, JSValueConst this_val, int argc,
                                 JSValueConst* argv) {
        CanvasRenderingContext2D* context = CanvasContextOf(ctx, this_val);
        if (context == nullptr || argc < 7) {
            return JS_UNDEFINED;
        }
        float values[7];
        if (!ReadNumbers(ctx, argv, 7, values)) {
            return JS_EXCEPTION;
        }
        bool counterclockwise = false;
        if (argc >= 8) {
            const int flag = JS_ToBool(ctx, argv[7]);
            if (flag < 0) {
                return JS_EXCEPTION;
            }
            counterclockwise = flag != 0;
        }
        context->Ellipse(values[0], values[1], values[2], values[3], values[4], values[5],
                         values[6], counterclockwise);
        return JS_UNDEFINED;
    }

    static JSValue CanvasArcTo(JSContext* ctx, JSValueConst this_val, int argc,
                               JSValueConst* argv) {
        CanvasRenderingContext2D* context = CanvasContextOf(ctx, this_val);
        if (context == nullptr || argc < 5) {
            return JS_UNDEFINED;
        }
        float values[5];
        if (!ReadNumbers(ctx, argv, 5, values)) {
            return JS_EXCEPTION;
        }
        context->ArcTo(values[0], values[1], values[2], values[3], values[4]);
        return JS_UNDEFINED;
    }

    static JSValue CanvasIsPointInPath(JSContext* ctx, JSValueConst this_val, int argc,
                                       JSValueConst* argv) {
        CanvasRenderingContext2D* context = CanvasContextOf(ctx, this_val);
        if (context == nullptr || argc < 2) {
            return JS_NewBool(ctx, false);
        }
        float values[2];
        if (!ReadNumbers(ctx, argv, 2, values)) {
            return JS_EXCEPTION;
        }
        bool even_odd = false;
        if (argc >= 3) {
            const char* rule = JS_ToCString(ctx, argv[2]);
            if (rule == nullptr) {
                return JS_EXCEPTION;
            }
            even_odd = AsciiLower(rule) == "evenodd";
            JS_FreeCString(ctx, rule);
        }
        return JS_NewBool(ctx, context->IsPointInPath(values[0], values[1], even_odd));
    }

    // PNG Data URL：画布内容编码为内存 PNG 后再做 Base64。
    static JSValue CanvasToDataURL(JSContext* ctx, JSValueConst this_val, int argc,
                                   JSValueConst* argv) {
        CanvasRenderingContext2D* context = CanvasContextOf(ctx, this_val);
        if (context == nullptr) {
            return JS_UNDEFINED;
        }
        if (argc >= 1 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0])) {
            const char* type = JS_ToCString(ctx, argv[0]);
            if (type == nullptr) {
                return JS_EXCEPTION;
            }
            const std::string mime = AsciiLower(type);
            JS_FreeCString(ctx, type);
            if (!mime.empty() && mime != "image/png") {
                return JS_NewString(ctx, "data:,");
            }
        }
        CanvasSurface* surface = context->Surface();
        if (surface == nullptr || surface->width <= 0 || surface->height <= 0 ||
            surface->pixels.empty()) {
            return JS_NewString(ctx, "data:,");
        }
        std::vector<uint8_t> png;
        std::string error;
        if (!EncodePngToMemory(surface->width, surface->height, surface->pixels.data(), &png,
                               &error)) {
            return JS_NewString(ctx, "data:,");
        }
        const std::string url = "data:image/png;base64," + Base64Encode(png.data(), png.size());
        return JS_NewString(ctx, url.c_str());
    }

    // 把数值截断为设备像素坐标；超出 int 范围视为非法。
    static bool ReadDeviceCoordinate(double value, int* output) {
        if (!std::isfinite(value) || value < -2147483648.0 || value > 2147483647.0) {
            return false;
        }
        *output = static_cast<int>(std::trunc(value));
        return true;
    }

    static JSValue CanvasGetImageData(JSContext* ctx, JSValueConst this_val, int argc,
                                      JSValueConst* argv) {
        CanvasRenderingContext2D* context = CanvasContextOf(ctx, this_val);
        if (context == nullptr) {
            return JS_UNDEFINED;
        }
        if (argc < 4) {
            return ThrowType(ctx, "getImageData 需要四个数值参数");
        }
        float values[4];
        if (!ReadNumbers(ctx, argv, 4, values)) {
            return JS_EXCEPTION;
        }
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
        if (!ReadDeviceCoordinate(values[0], &x) || !ReadDeviceCoordinate(values[1], &y) ||
            !ReadDeviceCoordinate(values[2], &width) || !ReadDeviceCoordinate(values[3], &height)) {
            return JS_ThrowRangeError(ctx, "getImageData 的坐标或尺寸超出可用范围");
        }
        if (width <= 0 || height <= 0) {
            return JS_ThrowRangeError(ctx, "getImageData 的宽度与高度必须为正数");
        }
        if (!ImageDataSizeAllowed(width, height)) {
            return JS_ThrowRangeError(ctx, "getImageData 请求的像素数据超过 64 MiB 上限");
        }
        std::vector<uint8_t> pixels;
        int output_width = width;
        int output_height = height;
        context->GetImageData(x, y, width, height, &output_width, &output_height, &pixels);
        return MakeImageData(ctx, output_width, output_height, pixels.data());
    }

    static JSValue CanvasPutImageData(JSContext* ctx, JSValueConst this_val, int argc,
                                      JSValueConst* argv) {
        CanvasRenderingContext2D* context = CanvasContextOf(ctx, this_val);
        if (context == nullptr || argc < 3) {
            return JS_UNDEFINED;
        }
        int width = 0;
        int height = 0;
        std::vector<uint8_t> pixels;
        if (!ReadImageData(ctx, argv[0], &width, &height, &pixels)) {
            return ThrowType(ctx, "putImageData 的第一个参数不是有效的 ImageData");
        }
        float position[2];
        if (!ReadNumbers(ctx, argv + 1, 2, position)) {
            return JS_EXCEPTION;
        }
        int x = 0;
        int y = 0;
        if (!ReadDeviceCoordinate(position[0], &x) || !ReadDeviceCoordinate(position[1], &y)) {
            return JS_ThrowRangeError(ctx, "putImageData 的坐标超出可用范围");
        }

        // 七参数形式带脏矩形：只写入源图中指定区域。
        if (argc >= 7) {
            float dirty[4];
            if (!ReadNumbers(ctx, argv + 3, 4, dirty)) {
                return JS_EXCEPTION;
            }
            int dirty_x = 0;
            int dirty_y = 0;
            int dirty_width = 0;
            int dirty_height = 0;
            if (!ReadDeviceCoordinate(dirty[0], &dirty_x) ||
                !ReadDeviceCoordinate(dirty[1], &dirty_y) ||
                !ReadDeviceCoordinate(dirty[2], &dirty_width) ||
                !ReadDeviceCoordinate(dirty[3], &dirty_height)) {
                return JS_ThrowRangeError(ctx, "putImageData 的脏矩形超出可用范围");
            }
            const int first_x = std::max(0, dirty_x);
            const int first_y = std::max(0, dirty_y);
            const int last_x = static_cast<int>(
                std::min<long long>(width, static_cast<long long>(dirty_x) + dirty_width));
            const int last_y = static_cast<int>(
                std::min<long long>(height, static_cast<long long>(dirty_y) + dirty_height));
            if (last_x <= first_x || last_y <= first_y) {
                return JS_UNDEFINED;
            }
            const int cropped_width = last_x - first_x;
            const int cropped_height = last_y - first_y;
            std::vector<uint8_t> cropped(static_cast<size_t>(cropped_width) *
                                         static_cast<size_t>(cropped_height) * 4u);
            const size_t source_row_bytes = static_cast<size_t>(width) * 4u;
            const size_t cropped_row_bytes = static_cast<size_t>(cropped_width) * 4u;
            for (int row = 0; row < cropped_height; ++row) {
                std::memcpy(cropped.data() + static_cast<size_t>(row) * cropped_row_bytes,
                            pixels.data() +
                                static_cast<size_t>(first_y + row) * source_row_bytes +
                                static_cast<size_t>(first_x) * 4u,
                            cropped_row_bytes);
            }
            context->PutImageData(cropped.data(), cropped_width, cropped_height, x + first_x,
                                  y + first_y);
            return JS_UNDEFINED;
        }

        context->PutImageData(pixels.data(), width, height, x, y);
        return JS_UNDEFINED;
    }

    static JSValue CanvasCreateImageData(JSContext* ctx, JSValueConst this_val, int argc,
                                         JSValueConst* argv) {
        CanvasRenderingContext2D* context = CanvasContextOf(ctx, this_val);
        if (context == nullptr) {
            return JS_UNDEFINED;
        }
        if (argc >= 2) {
            int32_t width = 0;
            int32_t height = 0;
            if (JS_ToInt32(ctx, &width, argv[0]) < 0 ||
                JS_ToInt32(ctx, &height, argv[1]) < 0) {
                return JS_EXCEPTION;
            }
            if (!ImageDataSizeAllowed(width, height)) {
                return JS_ThrowRangeError(ctx,
                                          "createImageData 的宽高必须为正数且不超过 64 MiB");
            }
            return MakeImageData(ctx, width, height, nullptr);
        }
        if (argc >= 1) {
            int width = 0;
            int height = 0;
            std::vector<uint8_t> ignored;
            if (!ReadImageData(ctx, argv[0], &width, &height, &ignored)) {
                return ThrowType(ctx, "createImageData 的第一个参数不是有效的 ImageData");
            }
            return MakeImageData(ctx, width, height, nullptr);
        }
        return ThrowType(ctx, "createImageData 需要宽度与高度");
    }

    // canvas.width / canvas.height：读写位图尺寸，非 canvas 元素返回 undefined。
    static Element* CanvasElementOf(JSContext* ctx, JSValueConst this_val) {
        Element* element = ElementOf(ctx, this_val);
        if (element == nullptr || AsciiLower(element->TagName()) != "canvas") {
            return nullptr;
        }
        return element;
    }

    // 画布宽高属性变化后需要重置上下文（清空位图与绘制状态）。
    static bool IsCanvasDimensionAttribute(const Element& element, const char* name) {
        if (element.TagName() != "canvas" || name == nullptr) {
            return false;
        }
        const std::string property = AsciiLower(name);
        return property == "width" || property == "height";
    }

    static JSValue CanvasDimensionGet(JSContext* ctx, JSValueConst this_val, int magic) {
        Element* element = CanvasElementOf(ctx, this_val);
        if (element == nullptr) {
            return JS_UNDEFINED;
        }
        const CanvasPixelSize size = ResolveCanvasPixelSize(*element);
        return JS_NewInt32(ctx, magic == 0 ? size.width : size.height);
    }

    static JSValue CanvasDimensionSet(JSContext* ctx, JSValueConst this_val, JSValueConst value,
                                      int magic) {
        ScriptHostImpl* host = HostOf(ctx);
        Element* element = CanvasElementOf(ctx, this_val);
        if (host == nullptr || element == nullptr) {
            return JS_UNDEFINED;
        }
        double number = 0.0;
        if (JS_ToFloat64(ctx, &number, value) < 0) {
            return JS_EXCEPTION;
        }
        if (!std::isfinite(number)) {
            return JS_UNDEFINED;
        }
        const double truncated = std::trunc(number);
        if (truncated < 0.0 || truncated > static_cast<double>(web::kMaxCanvasDimension)) {
            return JS_UNDEFINED;
        }
        element->SetAttribute(magic == 0 ? "width" : "height",
                              std::to_string(static_cast<int>(truncated)));
        host->ResetCanvasContext(element);
        SyncCanvasSurfaceSize(*element);
        host->NotifyChanged(element);
        return JS_UNDEFINED;
    }

    Document* document_ = nullptr;
    // document.readyState 的当前值：脚本执行阶段、DOMContentLoaded 前后依次为
    // loading、interactive、complete。
    std::string ready_state_ = "loading";
    // document.cookie 的键值表；路径与过期时间等属性不参与存储。
    std::map<std::string, std::string> cookies_;
    // 最近一次 element.select() 选中的文本，供 execCommand('copy') 写入剪贴板。
    std::string selected_text_;
    ScriptHostOptions options_;
    // 当前事件派发期间由 preventDefault / stopPropagation 设置的标记。
    bool default_prevented_ = false;
    bool propagation_stopped_ = false;
    // localStorage 的键值表。存在持久化路径时与文件同步。
    std::map<std::string, std::string> storage_;
    JSRuntime* runtime_ = nullptr;
    JSContext* ctx_ = nullptr;

    JSClassID node_class_counter_ = 0;
    JSClassID node_class_id_ = 0;
    JSClassID style_class_counter_ = 0;
    JSClassID style_class_id_ = 0;
    JSClassID canvas_class_counter_ = 0;
    JSClassID canvas_class_id_ = 0;
    JSClassID gradient_class_counter_ = 0;
    JSClassID gradient_class_id_ = 0;
    JSClassID pattern_class_counter_ = 0;
    JSClassID pattern_class_id_ = 0;

    JSValue global_ = JS_UNDEFINED;
    JSValue node_proto_ = JS_UNDEFINED;
    JSValue element_proto_ = JS_UNDEFINED;
    JSValue document_proto_ = JS_UNDEFINED;
    JSValue style_proto_ = JS_UNDEFINED;
    JSValue canvas_context_proto_ = JS_UNDEFINED;
    JSValue gradient_proto_ = JS_UNDEFINED;
    JSValue pattern_proto_ = JS_UNDEFINED;

    std::unordered_map<Node*, JSValue> node_wrappers_;
    std::unordered_map<Node*, JSValue> style_wrappers_;
    std::unordered_map<Node*, JSValue> canvas_wrappers_;

    int execution_depth_ = 0;
    bool deadline_active_ = false;
    std::chrono::steady_clock::time_point deadline_{};

    std::vector<Timer> timers_;
    uint32_t next_timer_id_ = 1;
    std::vector<JSValue> animation_frames_;

    std::unordered_map<Node*, ListenerGroup> listeners_;
    // 事件处理器槽位：键为节点，内层键为事件类型。
    std::unordered_map<Node*, std::unordered_map<std::string, EventHandlerSlot>> event_handlers_;
    // 注册在 window（全局对象）上的事件监听器。
    std::vector<WindowListener> window_listeners_;

    float viewport_width_ = 0.0f;
    float viewport_height_ = 0.0f;
    double clock_ms_ = 0.0;
};

}  // namespace

std::unique_ptr<ScriptHost> ScriptHost::Create(Document* document, ScriptHostOptions options) {
    if (document == nullptr) {
        return nullptr;
    }
    auto host = std::unique_ptr<ScriptHostImpl>(new ScriptHostImpl(document, std::move(options)));
    if (!host->Initialize()) {
        return nullptr;
    }
    return host;
}

}  // namespace web
