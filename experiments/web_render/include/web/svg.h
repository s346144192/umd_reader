#pragma once

#include <map>
#include <utility>

#include "web/layout.h"
#include "web/render.h"
#include "web/text.h"

namespace web {

// 字形索引：字体句柄与码位。HTML 文本与 SVG 文本共用同一图集与索引类型。
using GlyphKey = std::pair<FontId, uint32_t>;
using GlyphMap = std::map<GlyphKey, GlyphInfo>;

// SVG 绘制参数。坐标换算与 HTML 绘制层一致：页面坐标 = 文档坐标 +
// (offset_x, offset_y - page_top)。
struct SvgPaintParams {
    RenderDevice* device = nullptr;
    TextService* text = nullptr;
    const GlyphMap* glyphs = nullptr;
    int atlas_width = 0;
    int atlas_height = 0;
    // 外层累计透明度（祖先盒的 opacity 与滤镜合成结果）。
    float opacity = 1.0f;
    float page_top = 0.0f;
    float offset_x = 0.0f;
    float offset_y = 0.0f;
};

// 绘制布局盒对应的内联 SVG 内容。box.element 必须是 svg 元素，绘制区域为
// 该盒的内容盒；内容盒之外的图元被裁剪。
void PaintSvgBox(const LayoutBox& box, const LayoutResult& layout,
                 const SvgPaintParams& params);

// 在 SVG 布局盒内按文档坐标执行命中测试，返回最内层命中的 SVG 元素。
// 坐标不在内容盒内或没有元素命中时返回空指针；text 用于测量文本包围盒，
// 为空时跳过文本命中。
const Element* HitTestSvgBox(const LayoutBox& box, const LayoutResult& layout, float x, float y,
                             TextService* text);

// 收集 SVG 子树中 <text> 需要进入字形图集的码位，必须在图集上传前调用。
void CollectSvgBoxGlyphs(const LayoutBox& box, const LayoutResult& layout,
                         TextService& text, GlyphMap* glyphs);

}  // namespace web
