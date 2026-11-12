#pragma once

#include <memory>
#include <string>
#include <vector>

#include "gfx/types.h"

namespace gfx {

enum class TextWeight {
    Light,
    Normal,
    SemiBold,
    Bold,
};

enum class TextAlignment {
    Leading,
    Center,
    Trailing,
};

enum class ParagraphAlignment {
    Near,
    Center,
    Far,
};

enum class WordWrapping {
    Wrap,
    NoWrap,
};

// 文本样式的后端无关描述。lineSpacing / baseline 仅在 uniformLineSpacing 为 true 时生效，
// 单位均为 DIP；baseline 为相邻两行基线之间的距离。
struct TextStyle {
    std::wstring fontFamily;
    std::wstring fallbackFamily = L"Segoe UI";
    std::wstring locale = L"zh-cn";
    float fontSize = 14.0f;
    TextWeight weight = TextWeight::Normal;
    TextAlignment textAlignment = TextAlignment::Leading;
    ParagraphAlignment paragraphAlignment = ParagraphAlignment::Near;
    WordWrapping wrapping = WordWrapping::Wrap;
    bool ellipsisTrimming = true;
    bool uniformLineSpacing = false;
    float lineSpacing = 0.0f;
    float baseline = 0.0f;
};

// 排版对象由 TextEngine 创建，生命周期由持有者管理。
class TextFormat {
public:
    virtual ~TextFormat() = default;
    // 与后端原生对象互操作时使用；其中 Direct2D 后端返回 IDWriteTextFormat*。
    virtual void* NativeHandle() const = 0;
};

class TextLayout {
public:
    virtual ~TextLayout() = default;
    virtual void* NativeHandle() const = 0;
    virtual float Width() const = 0;
    virtual float Height() const = 0;
};

class TextEngine {
public:
    virtual ~TextEngine() = default;

    virtual std::unique_ptr<TextFormat> CreateFormat(const TextStyle& style) = 0;
    virtual std::unique_ptr<TextLayout> CreateLayout(const std::wstring& text,
                                                     const TextFormat& format, float width,
                                                     float maxHeight) = 0;
    // 测量单行文本宽度；按后端允许的最大行宽排版后取含尾随空白的宽度。
    virtual float MeasureWidth(const std::wstring& text, const TextFormat& format) = 0;
    // 枚举系统中可显示中文的字体族。
    virtual std::vector<FontFamilyName> SystemFontFamilies() const = 0;
};

}  // namespace gfx
