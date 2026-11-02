#pragma once

#include <windows.h>
#include <dwrite.h>

#include <string>
#include <vector>

namespace ui {

struct PageInfo {
    uint32_t startChar = 0;  // 章节内 UTF-16 码元偏移
    uint32_t endChar = 0;
};

// 按行高度把一章正文切分为固定高度的页。
class ChapterPager {
public:
    void Build(const std::wstring& text, IDWriteTextFormat* format, IDWriteFactory* factory,
               float width, float pageHeight);
    void Clear();

    size_t PageCount() const { return pages_.size(); }
    const PageInfo& Page(size_t index) const { return pages_[index]; }
    size_t PageIndexForChar(uint32_t charOffset) const;

private:
    std::vector<PageInfo> pages_;
};

}  // namespace ui
