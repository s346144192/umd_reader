#pragma once

#include <windows.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace ui {

// 分块布局使用的块大小与回退范围。分页与标注锚点必须使用相同的分块参数，
// 才能保证两者计算出的行尾一致。
constexpr uint32_t kLayoutChunkChars = 48000;
constexpr uint32_t kLayoutChunkBacktrack = 2000;

// 长文本的分块信息。块起点尽量对齐到换行符之后，这样在相同排版宽度下，
// 每一块的首行与整段文本的行首一致，分页与标注锚点才能共用同一套分行结果。
struct TextChunk {
    uint32_t begin = 0;
    uint32_t end = 0;
};

inline std::vector<TextChunk> SplitTextIntoChunks(const std::wstring& text, uint32_t chunkChars,
                                                  uint32_t backtrack) {
    std::vector<TextChunk> chunks;
    const uint32_t length = static_cast<uint32_t>(text.size());
    if (length == 0 || chunkChars == 0) {
        return chunks;
    }
    uint32_t start = 0;
    while (start < length) {
        uint32_t end = std::min<uint32_t>(start + chunkChars, length);
        if (end < length) {
            const uint32_t limit = end > backtrack ? end - backtrack : start;
            for (uint32_t probe = end; probe > limit; --probe) {
                if (text[probe - 1] == L'\n') {
                    end = probe;
                    break;
                }
            }
        }
        TextChunk chunk;
        chunk.begin = start;
        chunk.end = end;
        chunks.push_back(chunk);
        start = end;
    }
    return chunks;
}

// 为 text 中 [begin, begin + length) 这一块创建文本布局并读取行度量。
// 各行长度之和等于 length；返回 false 表示无法布局（调用方按单行降级处理）。
inline bool CollectLineMetrics(const std::wstring& text, uint32_t begin, uint32_t length,
                               IDWriteTextFormat* format, IDWriteFactory* factory, float width,
                               std::vector<DWRITE_LINE_METRICS>& metrics) {
    metrics.clear();
    if (!format || !factory || length == 0 || width <= 0.0f) {
        return false;
    }
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    factory->CreateTextLayout(text.c_str() + begin, length, format, width, 1000000.0f, &layout);
    if (!layout) {
        return false;
    }
    // 以零容量查询行数时，DirectWrite 会返回缓冲区不足并写出实际行数，
    // 因此这里只检查行数，不能把失败的 HRESULT 当作布局失败。
    UINT32 lineCount = 0;
    layout->GetLineMetrics(nullptr, 0, &lineCount);
    if (lineCount == 0) {
        return false;
    }
    metrics.resize(lineCount);
    if (FAILED(layout->GetLineMetrics(metrics.data(), lineCount, &lineCount))) {
        metrics.clear();
        return false;
    }
    metrics.resize(lineCount);
    return true;
}

}  // namespace ui
