#pragma once

#include <functional>
#include <memory>
#include <string>

#include "web/dom.h"

namespace web {

struct ScriptHostOptions {
    // DOM 结构或样式发生变更后回调，引擎据此重新计算布局并重绘。
    std::function<void()> on_document_changed;
    // SVG 子树内部发生变化（图元、变换、class）：不影响 HTML 布局，
    // 引擎只需刷新 SVG 计算样式并重绘。
    std::function<void()> on_svg_changed;
    // 页面请求下一帧动画时回调（requestAnimationFrame）。
    std::function<void()> on_animation_frame;
    // 查询元素边框盒在视口中的位置与尺寸，供 getBoundingClientRect 使用。
    // 元素没有对应布局盒时返回 false，脚本侧得到全零矩形。
    std::function<bool(const Element&, float* left, float* top, float* width, float* height)>
        get_element_client_rect;
    // localStorage 的持久化文件路径。为空时仅保存在内存中，退出后丢弃。
    std::string local_storage_path;
};

// 派发到页面脚本的事件参数。click 使用 client_x/client_y；键盘事件使用
// key/code 与修饰键状态；pointer/wheel 事件使用 client_x/client_y 与
// button/delta_y；input/change/focus/blur 只需要 type 与 target。
struct ScriptEventInit {
    float client_x = 0.0f;
    float client_y = 0.0f;
    std::string key;
    std::string code;
    bool shift_key = false;
    bool ctrl_key = false;
    bool alt_key = false;
    bool meta_key = false;
    // 指针按键：0 左键、1 中键、2 右键。
    int button = 0;
    // wheel 事件的纵向滚动量。
    float delta_y = 0.0f;
};

// JavaScript 宿主（QuickJS）：暴露最小 DOM 绑定，使页面脚本可以查询、修改文档。
// 目前支持的接口：
//   document.getElementById / getElementsByTagName / querySelector / querySelectorAll /
//   createElement / createTextNode / body / documentElement / readyState / cookie /
//   activeElement / execCommand("copy") / addEventListener
//   元素：id、className、classList、dataset、value、checked、disabled、style、
//         width/height（canvas）、textContent、innerHTML、getAttribute / setAttribute /
//         removeAttribute / hasAttribute、getElementsByTagName、querySelector /
//         querySelectorAll / closest、select（文本框全选）、getBoundingClientRect、
//         getContext("2d")、appendChild / insertBefore / removeChild、addEventListener
//   canvas 2D：fillRect / clearRect / strokeRect、fillText / measureText、
//         beginPath / moveTo / lineTo / quadraticCurveTo / bezierCurveTo / arc /
//         rect / ellipse / arcTo / closePath / fill / stroke / clip / isPointInPath、
//         save / restore、translate / scale / rotate / transform / setTransform /
//         resetTransform、渐变、图案（createPattern）、drawImage、
//         getImageData / putImageData / createImageData、toDataURL、
//         lineWidth / lineCap / lineJoin / miterLimit / globalAlpha /
//         globalCompositeOperation / shadowColor / shadowBlur / shadowOffsetX /
//         shadowOffsetY / setLineDash / getLineDash / lineDashOffset、
//         textAlign / textBaseline / direction（ltr / rtl）
//   全局：console.log / console.warn / console.error、requestAnimationFrame、
//         setTimeout / clearTimeout / setInterval / clearInterval、
//         innerWidth / innerHeight、devicePixelRatio、localStorage、
//         Promise/async 微任务（每次脚本执行、事件派发与定时器回调后执行）
class ScriptHost {
public:
    virtual ~ScriptHost() = default;

    static std::unique_ptr<ScriptHost> Create(Document* document, ScriptHostOptions options);

    // 执行脚本；失败时把异常文本写入 exception（可为空）。
    virtual bool Evaluate(const std::string& source, std::string* exception) = 0;

    // 派发 click 事件到目标元素（冒泡路径为 target → 祖先）。
    virtual void DispatchClick(const Element* target, float client_x, float client_y) = 0;

    // 派发任意类型事件到目标元素，冒泡路径为 target → 祖先。
    // 支持的 type 包括 click、pointerdown/pointermove/pointerup、wheel、
    // keydown/keyup、input/change/focus/blur、contextmenu 等。
    // 返回值表示页面是否调用了 preventDefault。
    virtual bool DispatchEvent(const Element* target, const std::string& type,
                               const ScriptEventInit& init) = 0;

    // 在文档节点上派发事件（DOMContentLoaded 等），随后冒泡到 window。
    virtual void DispatchDocumentEvent(const std::string& type) = 0;

    // 推进时间：触发到期的 setTimeout 与待执行的 requestAnimationFrame 回调。
    virtual void AdvanceTime(double delta_ms) = 0;

    virtual bool HasPendingTimers() const = 0;
    virtual bool HasPendingAnimationFrame() const = 0;

    // 设置视口尺寸，供 innerWidth / innerHeight 使用。
    virtual void SetViewport(float width, float height) = 0;
};

}  // namespace web
