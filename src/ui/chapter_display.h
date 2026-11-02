#pragma once

#include <windows.h>
#include <dwrite.h>

#include <cstdint>
#include <string>
#include <vector>

#include "data/library.h"

namespace ui {

// 显示文本中的一段：普通正文段，或为标注预留的空行段。
struct DisplaySegment {
    uint32_t displayStart = 0;
    uint32_t displayLength = 0;
    uint32_t originalStart = 0;
    uint32_t originalLength = 0;
    int noteIndex = -1;
};

struct NotePlacement {
    int noteIndex = -1;
    uint32_t displayStart = 0;  // 占位首行在显示文本中的偏移
    uint32_t lineCount = 0;
};

// 把章节原始文本与标注合成显示文本：标注在其锚点行之后插入对应行数的空行，
// 并维护显示偏移与原始偏移的双向映射。
class ChapterDisplay {
public:
    void Build(const std::wstring& original, const std::vector<data::Note>& notes, int chapterIndex,
               IDWriteTextFormat* bodyFormat, IDWriteTextFormat* noteFormat, IDWriteFactory* factory,
               float contentWidth, float indentWidth);
    void Clear();

    const std::wstring& Text() const { return text_; }
    const std::vector<DisplaySegment>& Segments() const { return segments_; }
    const std::vector<NotePlacement>& Placements() const { return placements_; }
    float NoteWidth() const { return noteWidth_; }
    float IndentWidth() const { return indentWidth_; }

    uint32_t ToOriginal(uint32_t displayOffset) const;
    uint32_t ToDisplay(uint32_t originalOffset) const;

private:
    std::wstring text_;
    std::vector<DisplaySegment> segments_;
    std::vector<NotePlacement> placements_;
    uint32_t originalSize_ = 0;
    float contentWidth_ = 0.0f;
    float noteWidth_ = 0.0f;
    float indentWidth_ = 0.0f;
};

}  // namespace ui
