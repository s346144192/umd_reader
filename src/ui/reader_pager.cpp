#include "ui/reader_pager.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "ui/text_layout_chunks.h"

namespace ui {
namespace {

struct LineRecord {
    uint32_t startChar = 0;
    float height = 0.0f;
};

}  // namespace

void ChapterPager::Clear() {
    pages_.clear();
}

void ChapterPager::Build(const std::wstring& text, IDWriteTextFormat* format,
                         IDWriteFactory* factory, float width, float pageHeight) {
    Clear();
    if (!factory || !format || text.empty() || width <= 0.0f) {
        PageInfo page;
        page.startChar = 0;
        page.endChar = static_cast<uint32_t>(text.size());
        pages_.push_back(page);
        return;
    }

    const uint32_t textLength = static_cast<uint32_t>(text.size());
    const std::vector<TextChunk> chunks =
        SplitTextIntoChunks(text, kLayoutChunkChars, kLayoutChunkBacktrack);

    // 分行分析是分页的主要开销，各文本块互不依赖，可以并行计算。
    std::vector<std::vector<DWRITE_LINE_METRICS>> chunkMetrics(chunks.size());
    const size_t hardwareThreads =
        static_cast<size_t>(std::max(1u, std::thread::hardware_concurrency()));
    const size_t workerCount = std::min(chunks.size(), hardwareThreads);

    std::atomic<size_t> nextChunk{0};
    const auto runWorker = [&]() {
        for (;;) {
            const size_t index = nextChunk.fetch_add(1);
            if (index >= chunks.size()) {
                return;
            }
            const TextChunk& chunk = chunks[index];
            CollectLineMetrics(text, chunk.begin, chunk.end - chunk.begin, format, factory, width,
                               chunkMetrics[index]);
        }
    };
    if (workerCount <= 1) {
        runWorker();
    } else {
        std::vector<std::thread> workers;
        workers.reserve(workerCount);
        for (size_t i = 0; i < workerCount; ++i) {
            workers.emplace_back(runWorker);
        }
        for (std::thread& worker : workers) {
            worker.join();
        }
    }

    std::vector<LineRecord> lines;
    for (size_t index = 0; index < chunks.size(); ++index) {
        const TextChunk& chunk = chunks[index];
        const std::vector<DWRITE_LINE_METRICS>& metrics = chunkMetrics[index];
        uint32_t lineStart = chunk.begin;
        if (metrics.empty()) {
            LineRecord record;
            record.startChar = chunk.begin;
            record.height = 1.0f;
            lines.push_back(record);
            continue;
        }
        for (const DWRITE_LINE_METRICS& metric : metrics) {
            LineRecord record;
            record.startChar = lineStart;
            record.height = metric.height > 0.0f ? metric.height : 1.0f;
            lines.push_back(record);
            lineStart += metric.length;
        }
    }

    if (lines.empty()) {
        PageInfo page;
        page.startChar = 0;
        page.endChar = textLength;
        pages_.push_back(page);
        return;
    }

    const float usableHeight = std::max(pageHeight, 1.0f);
    size_t lineIndex = 0;
    while (lineIndex < lines.size()) {
        const size_t pageStartLine = lineIndex;
        float usedHeight = 0.0f;
        while (lineIndex < lines.size()) {
            const float lineHeight = lines[lineIndex].height;
            if (lineIndex > pageStartLine && usedHeight + lineHeight > usableHeight + 0.01f) {
                break;
            }
            usedHeight += lineHeight;
            ++lineIndex;
        }

        PageInfo page;
        page.startChar = lines[pageStartLine].startChar;
        page.endChar = lineIndex < lines.size() ? lines[lineIndex].startChar : textLength;
        pages_.push_back(page);
    }
}

size_t ChapterPager::PageIndexForChar(uint32_t charOffset) const {
    if (pages_.empty()) {
        return 0;
    }
    const auto it = std::upper_bound(
        pages_.begin(), pages_.end(), charOffset,
        [](uint32_t value, const PageInfo& page) { return value < page.startChar; });
    if (it == pages_.begin()) {
        return 0;
    }
    return static_cast<size_t>(it - pages_.begin() - 1);
}

}  // namespace ui
