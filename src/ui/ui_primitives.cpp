#include "ui/application.h"

#include <windowsx.h>
#include <d2d1helper.h>
#include <imm.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <winhttp.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <iterator>
#include <regex>
#include <vector>

#include "core/text_util.h"
#include "nlohmann/json.hpp"
#include "ui/ui_internal.h"

namespace ui {
using namespace detail;

void Application::DrawButton(const std::wstring& label, const D2D1_RECT_F& rect, HitKind kind,
                             const ThemeColors& colors, bool primary, bool active, int index,
                             const std::wstring& value) {
    const TextFormats& formats = renderer_.Formats();
    const bool hovered = IsHovered(rect);
    if (primary) {
        const D2D1_COLOR_F fill = hovered ? Darken(colors.accent, 0.9f) : colors.accent;
        renderer_.FillRoundedRect(rect, 9.0f, fill);
        renderer_.DrawTextBlock(label, rect, formats.button.Get(), colors.accentText);
    } else {
        if (hovered || active) {
            renderer_.FillRoundedRect(rect, 9.0f, colors.surfaceAlt);
        }
        renderer_.DrawTextBlock(label, rect, formats.button.Get(),
                                active ? colors.accent : colors.text);
        if (active) {
            renderer_.StrokeRoundedRect(rect, 9.0f, colors.accent, 1.0f);
        }
    }
    AddHit(rect, kind, index, value);
}

void Application::DrawPlainButton(const std::wstring& label, const D2D1_RECT_F& rect,
                                  HitKind kind, const ThemeColors& colors, int index,
                                  const std::wstring& value) {
    const TextFormats& formats = renderer_.Formats();
    if (IsHovered(rect)) {
        renderer_.FillRoundedRect(rect, 9.0f, colors.surfaceAlt);
    }
    // 常规字重，避免次要操作显得过于突出。
    renderer_.DrawTextBlock(label, rect, formats.centered.Get(), colors.text);
    AddHit(rect, kind, index, value);
}

void Application::AddHit(const D2D1_RECT_F& rect, HitKind kind, int index,
                         const std::wstring& value) {
    D2D1_RECT_F visible = rect;
    if (!clipStack_.empty()) {
        visible = IntersectRect(rect, clipStack_.back());
    }
    if (visible.right <= visible.left || visible.bottom <= visible.top) {
        return;
    }
    HitRegion region;
    region.rect = visible;
    region.kind = kind;
    region.index = index;
    region.value = value;
    hitRegions_.push_back(std::move(region));
}

bool Application::IsHovered(const D2D1_RECT_F& rect) const {
    return mouseInside_ && PointInside(rect, mouseX_, mouseY_);
}

const Application::HitRegion* Application::HitTest(float x, float y) const {
    for (auto it = hitRegions_.rbegin(); it != hitRegions_.rend(); ++it) {
        if (PointInside(it->rect, x, y)) {
            return &(*it);
        }
    }
    return nullptr;
}

void Application::BeginClip(const D2D1_RECT_F& rect) {
    clipStack_.push_back(rect);
    renderer_.PushClip(rect);
}

void Application::EndClip() {
    if (clipStack_.empty()) {
        return;
    }
    clipStack_.pop_back();
    renderer_.PopClip();
}


}  // namespace ui
