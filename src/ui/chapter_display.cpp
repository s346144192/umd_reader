#include "ui/chapter_display.h"

#include <wrl/client.h>

#include <algorithm>

#include "ui/text_layout_chunks.h"

namespace ui {
namespace {

using Microsoft::WRL::ComPtr;

size_t NewlineLengthAt(const std::wstring& text, size_t position) {
    if (position >= text.size()) {
        return 0;
    }
    if (text[position] == L'\r') {
        return position + 1 < text.size() && text[position + 1] == L'\n' ? 2 : 1;
    }
    return text[position] == L'\n' ? 1 : 0;
}

struct LineRange {
    uint32_t start = 0;
    uint32_t contentEnd = 0;
};

struct PendingNote {
    int noteIndex = -1;
    uint32_t start = 0;
    uint32_t insertion = 0;
    uint32_t lineCount = 1;
};

uint32_t MeasureNoteLineCount(const std::wstring& text, IDWriteTextFormat* noteFormat,
                              IDWriteFactory* factory, float width) {
    if (text.empty() || !noteFormat || !factory || width <= 0.0f) {
        return 1;
    }
    ComPtr<IDWriteTextLayout> layout;
    factory->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), noteFormat, width,
                              1000000.0f, &layout);
    if (!layout) {
        return 1;
    }
    UINT32 lineCount = 0;
    layout->GetLineMetrics(nullptr, 0, &lineCount);
    return lineCount == 0 ? 1 : lineCount;
}

}  // namespace

void ChapterDisplay::Clear() {
    text_.clear();
    segments_.clear();
    placements_.clear();
    originalSize_ = 0;
    contentWidth_ = 0.0f;
    noteWidth_ = 0.0f;
    indentWidth_ = 0.0f;
}

void ChapterDisplay::Build(const std::wstring& original, const std::vector<data::Note>& notes,
                           int chapterIndex, IDWriteTextFormat* bodyFormat,
                           IDWriteTextFormat* noteFormat, IDWriteFactory* factory,
                           float contentWidth, float indentWidth) {
    Clear();
    originalSize_ = static_cast<uint32_t>(original.size());
    contentWidth_ = std::max(contentWidth, 60.0f);
    indentWidth_ = std::max(indentWidth, 0.0f);
    noteWidth_ = std::max(contentWidth_ - indentWidth_, 60.0f);

    text_ = original;
    DisplaySegment whole;
    whole.displayLength = originalSize_;
    whole.originalLength = originalSize_;
    segments_.push_back(whole);
    if (original.empty() || notes.empty() || !bodyFormat || !noteFormat || !factory) {
        return;
    }

    // 标注锚点只需要其所在行的行尾位置。为整章建立一次超大文本布局代价过高，
    // 因此按与分页相同的块划分，只对包含锚点的块计算分行结果。
    const std::vector<TextChunk> chunks =
        SplitTextIntoChunks(original, kLayoutChunkChars, kLayoutChunkBacktrack);
    std::vector<std::vector<LineRange>> chunkLines(chunks.size());
    std::vector<bool> chunkLoaded(chunks.size(), false);

    const auto loadChunkLines = [&](size_t chunkIndex) {
        if (chunkLoaded[chunkIndex]) {
            return;
        }
        chunkLoaded[chunkIndex] = true;
        const TextChunk& chunk = chunks[chunkIndex];
        std::vector<DWRITE_LINE_METRICS> metrics;
        if (!CollectLineMetrics(original, chunk.begin, chunk.end - chunk.begin, bodyFormat,
                                factory, contentWidth_, metrics)) {
            LineRange single{chunk.begin, chunk.end};
            chunkLines[chunkIndex].push_back(single);
            return;
        }
        std::vector<LineRange>& lines = chunkLines[chunkIndex];
        lines.reserve(metrics.size());
        uint32_t lineStart = chunk.begin;
        for (const DWRITE_LINE_METRICS& metric : metrics) {
            uint32_t contentEnd = lineStart + metric.length;
            if (contentEnd >= metric.newlineLength) {
                contentEnd -= metric.newlineLength;
            }
            if (contentEnd > chunk.end) {
                contentEnd = chunk.end;
            }
            lines.push_back({lineStart, contentEnd});
            lineStart += metric.length;
        }
    };

    const auto chunkIndexFor = [&chunks](uint32_t target) -> size_t {
        const auto it = std::upper_bound(
            chunks.begin(), chunks.end(), target,
            [](uint32_t value, const TextChunk& chunk) { return value < chunk.begin; });
        if (it == chunks.begin()) {
            return 0;
        }
        return static_cast<size_t>(it - chunks.begin() - 1);
    };

    std::vector<PendingNote> pending;
    for (size_t i = 0; i < notes.size(); ++i) {
        const data::Note& note = notes[i];
        if (note.chapterIndex != chapterIndex || note.text.empty()) {
            continue;
        }
        const uint32_t start = std::min(note.start, originalSize_);
        const uint64_t rawEnd = static_cast<uint64_t>(note.start) + note.length;
        const uint32_t end =
            static_cast<uint32_t>(std::min<uint64_t>(rawEnd, originalSize_));
        const uint32_t target = end > start ? end - 1 : start;

        uint32_t insertion = start;
        if (!chunks.empty()) {
            const size_t chunkIndex = chunkIndexFor(target);
            loadChunkLines(chunkIndex);
            for (const LineRange& line : chunkLines[chunkIndex]) {
                if (target >= line.start && target <= line.contentEnd) {
                    insertion = line.contentEnd;
                    break;
                }
                if (target < line.start) {
                    break;
                }
            }
        }
        insertion = std::max(insertion, start);
        insertion = std::min(insertion, originalSize_);

        PendingNote item;
        item.noteIndex = static_cast<int>(i);
        item.start = start;
        item.insertion = insertion;
        item.lineCount = MeasureNoteLineCount(note.text, noteFormat, factory, noteWidth_);
        pending.push_back(item);
    }
    if (pending.empty()) {
        return;
    }

    std::sort(pending.begin(), pending.end(), [](const PendingNote& a, const PendingNote& b) {
        if (a.insertion != b.insertion) {
            return a.insertion < b.insertion;
        }
        return a.start < b.start;
    });

    text_.clear();
    segments_.clear();
    placements_.clear();
    uint32_t cursor = 0;
    for (const PendingNote& note : pending) {
        const uint32_t insertion = std::max(note.insertion, cursor);
        if (insertion > cursor) {
            DisplaySegment segment;
            segment.displayStart = static_cast<uint32_t>(text_.size());
            segment.displayLength = insertion - cursor;
            segment.originalStart = cursor;
            segment.originalLength = insertion - cursor;
            segments_.push_back(segment);
            text_.append(original, cursor, insertion - cursor);
            cursor = insertion;
        }

        const uint32_t blockStart = static_cast<uint32_t>(text_.size());
        text_ += L"\r\n";
        for (uint32_t line = 0; line < note.lineCount; ++line) {
            text_ += L"\u3000\r\n";
        }

        DisplaySegment block;
        block.displayStart = blockStart;
        block.displayLength = static_cast<uint32_t>(text_.size()) - blockStart;
        block.originalStart = cursor;
        block.originalLength = 0;
        block.noteIndex = note.noteIndex;
        segments_.push_back(block);

        NotePlacement placement;
        placement.noteIndex = note.noteIndex;
        placement.displayStart = blockStart + 2;
        placement.lineCount = note.lineCount;
        placements_.push_back(placement);
    }

    if (cursor < originalSize_) {
        DisplaySegment tail;
        tail.displayStart = static_cast<uint32_t>(text_.size());
        tail.displayLength = originalSize_ - cursor;
        tail.originalStart = cursor;
        tail.originalLength = originalSize_ - cursor;
        segments_.push_back(tail);
        text_.append(original, cursor, originalSize_ - cursor);
    }
}

uint32_t ChapterDisplay::ToOriginal(uint32_t displayOffset) const {
    if (segments_.empty()) {
        return 0;
    }
    if (displayOffset >= text_.size()) {
        return originalSize_;
    }
    for (const DisplaySegment& segment : segments_) {
        if (displayOffset < segment.displayStart) {
            continue;
        }
        const uint32_t end = segment.displayStart + segment.displayLength;
        if (displayOffset < end) {
            if (segment.originalLength == 0) {
                return segment.originalStart;
            }
            const uint32_t delta = displayOffset - segment.displayStart;
            return segment.originalStart + std::min(delta, segment.originalLength);
        }
    }
    return originalSize_;
}

uint32_t ChapterDisplay::ToDisplay(uint32_t originalOffset) const {
    if (segments_.empty()) {
        return 0;
    }
    if (originalOffset >= originalSize_) {
        return static_cast<uint32_t>(text_.size());
    }
    for (const DisplaySegment& segment : segments_) {
        if (segment.originalLength == 0) {
            continue;
        }
        if (originalOffset < segment.originalStart) {
            return segment.displayStart;
        }
        if (originalOffset < segment.originalStart + segment.originalLength) {
            return segment.displayStart + (originalOffset - segment.originalStart);
        }
    }
    return static_cast<uint32_t>(text_.size());
}

}  // namespace ui
