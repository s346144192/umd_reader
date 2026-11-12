#pragma once

#include <algorithm>
#include <cstdint>

namespace web {

// 与图形 API 无关的几何与颜色类型，坐标单位为 CSS 像素（1 CSS px = 1 DIP）。

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

    bool Contains(float x, float y) const {
        return x >= left && x < right && y >= top && y < bottom;
    }

    RectF Offset(float dx, float dy) const {
        RectF result;
        result.left = left + dx;
        result.top = top + dy;
        result.right = right + dx;
        result.bottom = bottom + dy;
        return result;
    }

    RectF Intersect(const RectF& other) const {
        RectF result;
        result.left = std::max(left, other.left);
        result.top = std::max(top, other.top);
        result.right = std::min(right, other.right);
        result.bottom = std::min(bottom, other.bottom);
        if (result.IsEmpty()) {
            result.right = result.left;
            result.bottom = result.top;
        }
        return result;
    }

    RectF Union(const RectF& other) const {
        if (IsEmpty()) {
            return other;
        }
        if (other.IsEmpty()) {
            return *this;
        }
        RectF result;
        result.left = std::min(left, other.left);
        result.top = std::min(top, other.top);
        result.right = std::max(right, other.right);
        result.bottom = std::max(bottom, other.bottom);
        return result;
    }

    static RectF FromXYWH(float x, float y, float width, float height) {
        RectF result;
        result.left = x;
        result.top = y;
        result.right = x + width;
        result.bottom = y + height;
        return result;
    }

    static RectF FromLTRB(float leftValue, float topValue, float rightValue, float bottomValue) {
        RectF result;
        result.left = leftValue;
        result.top = topValue;
        result.right = rightValue;
        result.bottom = bottomValue;
        return result;
    }
};

struct Color {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;

    bool Visible() const { return a > 0.0f; }

    bool operator==(const Color& other) const {
        return r == other.r && g == other.g && b == other.b && a == other.a;
    }
    bool operator!=(const Color& other) const { return !(*this == other); }
};

inline Color Rgba(float r, float g, float b, float a = 1.0f) {
    Color color;
    color.r = r;
    color.g = g;
    color.b = b;
    color.a = a;
    return color;
}

inline Color Transparent() {
    return Rgba(0.0f, 0.0f, 0.0f, 0.0f);
}

inline Color Black() {
    return Rgba(0.0f, 0.0f, 0.0f, 1.0f);
}

inline Color White() {
    return Rgba(1.0f, 1.0f, 1.0f, 1.0f);
}

}  // namespace web
