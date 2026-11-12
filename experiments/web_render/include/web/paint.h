#pragma once

#include <cstdint>
#include <map>
#include <string>

#include "web/canvas.h"
#include "web/dom.h"
#include "web/form_events.h"
#include "web/layout.h"
#include "web/render.h"
#include "web/selection.h"
#include "web/text.h"

namespace web {

// 图片资源缓存：按相对 base_directory 的 src 加载文件并上传为设备纹理。
// 同一 src 复用同一句柄；加载失败返回 0，并在本次运行内记录失败状态。
class ImageCache {
public:
    ImageCache(RenderDevice& device, std::string base_directory);
    ~ImageCache();

    void SetBaseDirectory(std::string base_directory);
    const std::string& BaseDirectory() const;

    ImageHandle Get(const std::string& source);
    // 画布纹理：同一元素复用同一句柄，revision 变化时更新纹理内容。
    ImageHandle GetCanvas(const Element& element, const CanvasSurface& surface);
    // 已加载图片的像素尺寸；句柄无效时返回 0。用于 background-size 计算。
    int Width(ImageHandle image) const;
    int Height(ImageHandle image) const;
    void Clear();
    RenderDevice& Device() { return device_; }

private:
    struct CanvasImage {
        ImageHandle handle = 0;
        uint64_t revision = 0;
    };

    RenderDevice& device_;
    std::string base_directory_;
    std::map<std::string, ImageHandle> handles_;
    std::map<const Element*, CanvasImage> canvas_handles_;
    std::map<ImageHandle, std::pair<int, int>> sizes_;
};

struct HitResult {
    const Element* element = nullptr;
    const TextNode* node = nullptr;
    size_t offset = 0;
    // 命中元素所属内嵌文档的 iframe 元素；顶层文档命中时为空。
    // 命中更深层 iframe 时由最内层命中结果的最近 iframe 所有者填写。
    const Element* frame_element = nullptr;

    bool Valid() const { return element != nullptr; }
};

// 文本控件的焦点绘制状态：由交互层维护并传给绘制层。
// caret 与 anchor 为 UTF-8 码位索引；scroll_x 用于单行输入框的水平滚动，
// scroll_y 用于 textarea 的垂直滚动；caret_visible 控制光标的闪烁显示。
struct FormFocusState {
    const Element* element = nullptr;
    size_t caret = 0;
    size_t anchor = 0;
    float scroll_x = 0.0f;
    float scroll_y = 0.0f;
    bool caret_visible = false;
};

// 绘制指定页：调用前需已执行 RenderDevice::BeginFrame，且 TextService 已完成
// 本页所需字形的栅格化（PaintPage 内部负责栅格化与图集上传）。
// focus 为文本控件的焦点状态，可为空；selection 为文档选区，可为空。
// 无窗口渲染不传入。
void PaintPage(const LayoutResult& layout, int page_index, TextService& text,
               RenderDevice& device, ImageCache& images,
               const FormFocusState* focus = nullptr,
               const SelectionRange* selection = nullptr);

// 命中测试：坐标以页面左上角为原点（CSS 像素）。返回最深层命中的元素；
// 命中文字时同时给出文本节点与字节偏移。text 用于 SVG 文本包围盒测量，
// 为空时跳过 SVG 文本命中。
HitResult HitTestPage(const LayoutResult& layout, int page_index, float x, float y,
                      TextService* text = nullptr);

// select 下拉浮层：在 select 盒下方绘制选项列表。需要在 EndFrame 之前、
// PaintPage 之后调用；坐标为页面坐标。
void PaintSelectPopup(const LayoutResult& layout, int page_index, TextService& text,
                      RenderDevice& device, const SelectPopup& popup);

// 命中下拉浮层中的选项。坐标以页面左上角为原点；命中时返回选项索引，
// 未命中返回 -1（点击浮层外由调用方关闭浮层）。
int HitTestSelectPopup(const LayoutResult& layout, int page_index, const SelectPopup& popup,
                       float x, float y);

}  // namespace web
