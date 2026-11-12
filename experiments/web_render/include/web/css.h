#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "web/types.h"

namespace web {

// CSS 长度单位。Auto 表示未指定或 auto 关键字；Number 表示无单位数字
// （line-height、opacity、z-index 等按倍数或纯数字解释的属性使用）。
enum class LengthUnit {
    Auto,
    Px,
    Em,
    Ex,
    Rem,
    Percent,
    Pt,
    Pc,
    In,
    Cm,
    Mm,
    Ch,
    Vw,
    Vh,
    Vmin,
    Vmax,
    Number,
    // 百分比与像素的组合差值：calc(100% - 40px)。value 为百分比数值，
    // offset 为像素偏移量，布局阶段按 base * value / 100 + offset 解析。
    CalcPercent,
};

struct CssLength {
    float value = 0.0f;
    LengthUnit unit = LengthUnit::Auto;
    // 仅 CalcPercent 使用：附加的像素偏移。
    float offset = 0.0f;

    CssLength() = default;
    CssLength(float length_value, LengthUnit length_unit)
        : value(length_value), unit(length_unit) {}
    CssLength(float percent_value, float pixel_offset)
        : value(percent_value), unit(LengthUnit::CalcPercent), offset(pixel_offset) {}

    bool IsAuto() const { return unit == LengthUnit::Auto; }
    bool IsNumber() const { return unit == LengthUnit::Number; }
    bool IsZero() const { return value == 0.0f && unit != LengthUnit::Auto; }
    // 是否涉及百分比（含百分比与像素组合），用于判断能否解析为确定像素。
    bool IsPercentBased() const {
        return unit == LengthUnit::Percent || unit == LengthUnit::CalcPercent;
    }
};

struct CssColor {
    Color color = Black();
    bool specified = false;
};

enum class ValueKind {
    Keyword,
    Length,  // 含百分比与无单位数字
    String,
    Color,
    Url,
    List,
};

struct CssValue {
    ValueKind kind = ValueKind::Keyword;
    std::string keyword;
    CssLength length;
    CssColor color;
    std::string string_value;
    std::vector<CssValue> items;
};

struct Declaration {
    std::string property;  // 小写化后的属性名
    CssValue value;
    bool important = false;
};

struct Specificity {
    int ids = 0;
    int classes = 0;
    int types = 0;

    int Compare(const Specificity& other) const {
        if (ids != other.ids) {
            return ids < other.ids ? -1 : 1;
        }
        if (classes != other.classes) {
            return classes < other.classes ? -1 : 1;
        }
        if (types != other.types) {
            return types < other.types ? -1 : 1;
        }
        return 0;
    }
};

enum class Combinator {
    Descendant,
    Child,
    NextSibling,
    SubsequentSibling,
};

struct SimpleSelector {
    enum class Kind {
        Universal,
        Type,
        Class,
        Id,
        Attribute,
        PseudoClass,
    };

    Kind kind = Kind::Universal;
    std::string name;   // 类型名、属性名或伪类名（小写）
    std::string value;  // 类名、ID 或属性值
    bool case_insensitive = false;
};

struct CompoundSelector {
    std::vector<SimpleSelector> simples;

    Specificity ComputeSpecificity() const {
        Specificity result;
        for (const SimpleSelector& simple : simples) {
            switch (simple.kind) {
            case SimpleSelector::Kind::Id:
                result.ids += 1;
                break;
            case SimpleSelector::Kind::Class:
            case SimpleSelector::Kind::Attribute:
            case SimpleSelector::Kind::PseudoClass:
                result.classes += 1;
                break;
            case SimpleSelector::Kind::Type:
                result.types += 1;
                break;
            case SimpleSelector::Kind::Universal:
                break;
            }
        }
        return result;
    }
};

// 复合选择器链中的一环：combinator 描述本环与前一个复合选择器之间的关系，
// 第一个环的 combinator 无意义（固定为 Descendant）。
struct SelectorStep {
    Combinator combinator = Combinator::Descendant;
    CompoundSelector compound;
};

struct Selector {
    std::vector<SelectorStep> steps;
    Specificity specificity;

    bool Empty() const { return steps.empty(); }
};

struct Rule {
    std::vector<Selector> selectors;
    std::vector<Declaration> declarations;
    size_t order = 0;
};

// @keyframes 中的单个关键帧：offset 为 0~1 的归一化进度。
struct Keyframe {
    float offset = 0.0f;
    std::vector<Declaration> declarations;
};

// 一组关键帧动画。帧按 offset 升序保存。
struct KeyframesRule {
    std::string name;
    std::vector<Keyframe> frames;
};

struct StyleSheet {
    std::vector<Rule> rules;
    std::vector<KeyframesRule> keyframes;
};

}  // namespace web
