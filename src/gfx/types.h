#pragma once

#include <cstdint>
#include <string>

namespace gfx {

// 与具体图形 API 无关的基础几何与颜色类型，单位为 DIP。
struct PointF {
    float x = 0.0f;
    float y = 0.0f;
};

struct RectF {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;

    float Width() const { return right - left; }
    float Height() const { return bottom - top; }
    bool IsEmpty() const { return right <= left || bottom <= top; }
};

struct ColorF {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;
};

inline ColorF ColorFromArgb(uint32_t argb) {
    ColorF color;
    color.a = static_cast<float>((argb >> 24) & 0xFFu) / 255.0f;
    color.r = static_cast<float>((argb >> 16) & 0xFFu) / 255.0f;
    color.g = static_cast<float>((argb >> 8) & 0xFFu) / 255.0f;
    color.b = static_cast<float>(argb & 0xFFu) / 255.0f;
    return color;
}

// 颜色缓存使用的 32 位键，各通道按 8 位量化，保留原有画刷复用行为。
inline uint32_t ColorKey(const ColorF& color) {
    const auto channel = [](float value) -> uint32_t {
        const float clamped = value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
        return static_cast<uint32_t>(clamped * 255.0f + 0.5f);
    };
    return (channel(color.a) << 24) | (channel(color.r) << 16) | (channel(color.g) << 8) |
           channel(color.b);
}

// 字体族条目：localized 为界面显示名（优先中文），canonical 为排版引擎使用的族名。
struct FontFamilyName {
    std::wstring localized;
    std::wstring canonical;
};

enum class BackendKind {
    Direct2D,
};

struct RendererCapabilities {
    BackendKind backend = BackendKind::Direct2D;
    bool antialiasedPrimitives = true;
    bool textLayout = true;
    bool bitmapImages = true;
    bool direct2dInterop = true;
};

}  // namespace gfx
