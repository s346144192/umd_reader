#pragma once

#include <cstdint>
#include <memory>
#include <string>

// wingdi.h 中的 CreateFont 宏会重命名同名成员函数，包含顺序不同会导致成员名不一致。
#if defined(CreateFont)
#undef CreateFont
#endif

namespace web {

// 字体句柄：由 TextService 创建，0 表示无效。
using FontId = uint32_t;

struct FontRequest {
    // 字体族名，空字符串表示使用系统默认 UI 字体（实现内部会做中文回退）。
    std::string family;
    float size = 16.0f;
    bool bold = false;
    bool italic = false;
};

struct FontMetrics {
    float ascent = 0.0f;   // 基线以上高度
    float descent = 0.0f;  // 基线以下高度（正数）
    float line_gap = 0.0f;
    float height = 0.0f;  // ascent + descent + line_gap
};

// 字形图集信息。atlas_x/atlas_y/atlas_w/atlas_h 为像素坐标下的覆盖位图矩形；
// bearing_x 为笔位置到图块左边的水平偏移，bearing_y 为基线上方的垂直偏移（向上为正）。
struct GlyphInfo {
    float advance = 0.0f;
    float bearing_x = 0.0f;
    float bearing_y = 0.0f;
    int atlas_x = 0;
    int atlas_y = 0;
    int atlas_w = 0;
    int atlas_h = 0;
    bool has_bitmap = false;
};

// 文本服务：使用 DirectWrite 提供字体度量与字形位图，供布局测量与各渲染后端共用。
// 单一实例，单线程使用；图集为 8 位覆盖图（A8），行优先。
class TextService {
public:
    virtual ~TextService() = default;

    virtual bool Initialize() = 0;

    // 创建（或复用）字体；相同请求返回相同句柄。
    virtual FontId CreateFont(const FontRequest& request) = 0;
    virtual FontMetrics Metrics(FontId font) const = 0;

    // 单个码位的步进宽度（不栅格化），用于断行与测量。
    virtual float Advance(FontId font, uint32_t codepoint) = 0;
    virtual float Measure(FontId font, const std::string& utf8_text) = 0;

    // 栅格化并放入图集；返回图块位置。空白字符与无轮廓字符返回 has_bitmap = false。
    virtual GlyphInfo Rasterize(FontId font, uint32_t codepoint) = 0;

    virtual int AtlasWidth() const = 0;
    virtual int AtlasHeight() const = 0;
    virtual const uint8_t* AtlasCoverage() const = 0;
    // 每次图集内容变化时递增，渲染后端据此决定是否重新上传纹理。
    virtual uint64_t AtlasVersion() const = 0;

    static std::unique_ptr<TextService> Create();
};

}  // namespace web
