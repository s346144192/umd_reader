#include "web/text.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <dwrite.h>
#include <windows.h>
#include <wrl/client.h>

// windows.h 会把 CreateFont 映射为 CreateFontA/CreateFontW，这里恢复接口方法名。
#ifdef CreateFont
#undef CreateFont
#endif

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "web/utf8.h"

namespace web {

namespace {

using Microsoft::WRL::ComPtr;

// 图集初始边长与上限，以及相邻字形的像素间距。
constexpr int kInitialAtlasSize = 512;
constexpr int kMaxAtlasSize = 2048;
constexpr int kAtlasPadding = 1;

// 字体面槽位：主字体、回退字体（>= 0 为回退族下标）、未解析。
constexpr int kMainFaceSlot = -1;
constexpr int kUnresolvedFaceSlot = -2;

// 请求字体族为空或不存在时的默认字体顺序。
const wchar_t* const kDefaultFamilies[] = {
    L"Segoe UI",
    L"Microsoft YaHei UI",
    L"Microsoft YaHei",
    L"SimSun",
    L"Arial",
};

// 主字体缺少某个字形时依次尝试的回退字体族。
const wchar_t* const kFallbackFamilies[] = {
    L"Microsoft YaHei UI",
    L"Microsoft YaHei",
    L"SimSun",
    L"Segoe UI Symbol",
};

const size_t kDefaultFamilyCount = sizeof(kDefaultFamilies) / sizeof(kDefaultFamilies[0]);
const size_t kFallbackFamilyCount = sizeof(kFallbackFamilies) / sizeof(kFallbackFamilies[0]);

// 把 UTF-8 字符串转换为宽字符串；非法序列返回空串，由调用方走默认字体。
std::wstring Utf8ToWide(const std::string& value) {
    std::wstring result;
    if (value.empty()) {
        return result;
    }
    const int byte_count = static_cast<int>(value.size());
    const int wide_length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.c_str(),
                                                byte_count, nullptr, 0);
    if (wide_length <= 0) {
        return result;
    }
    result.assign(static_cast<size_t>(wide_length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.c_str(), byte_count,
                            result.data(), wide_length) <= 0) {
        result.clear();
    }
    return result;
}

// 空白码位不产生位图，但仍保留字体给出的步进宽度。
bool IsWhitespaceCodepoint(uint32_t codepoint) {
    switch (codepoint) {
    case 0x0009:
    case 0x000A:
    case 0x000B:
    case 0x000C:
    case 0x000D:
    case 0x0020:
    case 0x00A0:
    case 0x1680:
    case 0x2028:
    case 0x2029:
    case 0x202F:
    case 0x205F:
    case 0x3000:
        return true;
    default:
        break;
    }
    return (codepoint >= 0x2000 && codepoint <= 0x200A);
}

DWRITE_FONT_WEIGHT WeightFor(bool bold) {
    return bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL;
}

DWRITE_FONT_STYLE StyleFor(bool italic) {
    return italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL;
}

// 字体请求的缓存键：解析后的族名、字号、粗体与斜体。
struct FontKey {
    std::wstring family;
    float size = 0.0f;
    bool bold = false;
    bool italic = false;

    bool operator<(const FontKey& other) const {
        if (family != other.family) {
            return family < other.family;
        }
        if (size != other.size) {
            return size < other.size;
        }
        if (bold != other.bold) {
            return bold < other.bold;
        }
        return italic < other.italic;
    }
};

// 单个码位的字形解析结果，缓存字体面槽位、字形下标与像素步进。
struct GlyphResolution {
    int face_slot = kUnresolvedFaceSlot;
    uint16_t glyph_index = 0;
    float advance = 0.0f;

    bool Resolved() const { return face_slot != kUnresolvedFaceSlot; }
};

// 一个已创建字体记录的全部缓存。
struct FontRecord {
    std::wstring family;
    float em_size = 0.0f;
    bool bold = false;
    bool italic = false;
    uint16_t units_per_em = 0;
    ComPtr<IDWriteFontFace> face;
    FontMetrics metrics;

    // 码位到字形解析结果、码位到图集字形信息的缓存。
    std::unordered_map<uint32_t, GlyphResolution> resolutions;
    std::unordered_map<uint32_t, GlyphInfo> glyphs;

    // 回退字体面按需创建，并记录是否已尝试过。
    std::vector<ComPtr<IDWriteFontFace>> fallback_faces;
    std::vector<uint8_t> fallback_attempted;
};

class TextServiceImpl : public TextService {
public:
    bool Initialize() override;
    FontId CreateFont(const FontRequest& request) override;
    FontMetrics Metrics(FontId font) const override;
    float Advance(FontId font, uint32_t codepoint) override;
    float Measure(FontId font, const std::string& utf8_text) override;
    GlyphInfo Rasterize(FontId font, uint32_t codepoint) override;

    int AtlasWidth() const override { return atlas_width_; }
    int AtlasHeight() const override { return atlas_height_; }
    const uint8_t* AtlasCoverage() const override {
        return atlas_.empty() ? nullptr : atlas_.data();
    }
    uint64_t AtlasVersion() const override { return atlas_version_; }

private:
    FontRecord* FindFont(FontId font);
    const FontRecord* FindFont(FontId font) const;

    bool FindFamilyIndex(const std::wstring& name, UINT32* index) const;
    std::wstring ResolveFamily(const std::string& requested);
    IDWriteFontFace* EnsureFallbackFace(FontRecord& record, size_t slot);
    GlyphResolution ResolveGlyph(FontRecord& record, uint32_t codepoint);
    bool QueryGlyphIndex(IDWriteFontFace* face, uint32_t codepoint, uint16_t* glyph_index) const;
    float DesignAdvance(IDWriteFontFace* face, uint16_t units_per_em, float em_size,
                        uint16_t glyph_index) const;

    bool GrowAtlas(int minimum_width, int minimum_height);
    bool PlaceAtlasRect(int width, int height, int* out_x, int* out_y);

    ComPtr<IDWriteFactory> factory_;
    ComPtr<IDWriteFontCollection> collection_;
    std::map<FontKey, FontId> font_keys_;
    std::unordered_map<FontId, std::unique_ptr<FontRecord>> fonts_;
    FontId next_font_id_ = 1;

    int atlas_width_ = 0;
    int atlas_height_ = 0;
    int cursor_x_ = 0;
    int cursor_y_ = 0;
    int shelf_height_ = 0;
    std::vector<uint8_t> atlas_;
    uint64_t atlas_version_ = 0;
};

bool TextServiceImpl::Initialize() {
    if (!factory_) {
        HRESULT result = DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(factory_.ReleaseAndGetAddressOf()));
        if (FAILED(result) || !factory_) {
            factory_.Reset();
            return false;
        }
    }
    if (!collection_) {
        HRESULT result = factory_->GetSystemFontCollection(collection_.ReleaseAndGetAddressOf(),
                                                           FALSE);
        if (FAILED(result) || !collection_) {
            collection_.Reset();
            return false;
        }
    }
    if (atlas_.empty()) {
        const size_t width = static_cast<size_t>(kInitialAtlasSize);
        const size_t height = static_cast<size_t>(kInitialAtlasSize);
        atlas_.assign(width * height, 0);
        atlas_width_ = kInitialAtlasSize;
        atlas_height_ = kInitialAtlasSize;
        cursor_x_ = 0;
        cursor_y_ = 0;
        shelf_height_ = 0;
        atlas_version_ = 0;
    }
    return true;
}

FontRecord* TextServiceImpl::FindFont(FontId font) {
    if (font == 0) {
        return nullptr;
    }
    auto iterator = fonts_.find(font);
    if (iterator == fonts_.end()) {
        return nullptr;
    }
    return iterator->second.get();
}

const FontRecord* TextServiceImpl::FindFont(FontId font) const {
    if (font == 0) {
        return nullptr;
    }
    auto iterator = fonts_.find(font);
    if (iterator == fonts_.end()) {
        return nullptr;
    }
    return iterator->second.get();
}

bool TextServiceImpl::FindFamilyIndex(const std::wstring& name, UINT32* index) const {
    if (!collection_ || name.empty()) {
        return false;
    }
    UINT32 found_index = 0;
    BOOL exists = FALSE;
    HRESULT result = collection_->FindFamilyName(name.c_str(), &found_index, &exists);
    if (FAILED(result) || !exists) {
        return false;
    }
    if (index != nullptr) {
        *index = found_index;
    }
    return true;
}

std::wstring TextServiceImpl::ResolveFamily(const std::string& requested) {
    if (!collection_) {
        return std::wstring();
    }
    /* font-family 回退链：逗号分隔的族名依次尝试，直到系统安装的字体命中。
       通用族名映射到本机常用字体。 */
    size_t offset = 0;
    while (offset <= requested.size()) {
        const size_t comma = requested.find(',', offset);
        const size_t end = comma == std::string::npos ? requested.size() : comma;
        std::string family = requested.substr(offset, end - offset);
        // 去除空格与引号。
        size_t begin = 0;
        while (begin < family.size() &&
               (family[begin] == ' ' || family[begin] == '\t' || family[begin] == '"' ||
                family[begin] == '\'')) {
            ++begin;
        }
        size_t finish = family.size();
        while (finish > begin &&
               (family[finish - 1] == ' ' || family[finish - 1] == '\t' ||
                family[finish - 1] == '"' || family[finish - 1] == '\'')) {
            --finish;
        }
        family = family.substr(begin, finish - begin);
        std::string lowered = family;
        for (char& character : lowered) {
            if (character >= 'A' && character <= 'Z') {
                character = static_cast<char>(character - 'A' + 'a');
            }
        }
        std::wstring candidate;
        if (lowered == "monospace") {
            candidate = L"Consolas";
        } else if (lowered == "serif") {
            candidate = L"Times New Roman";
        } else if (lowered == "sans-serif" || lowered == "system-ui" ||
                   lowered == "ui-sans-serif") {
            candidate = L"Microsoft YaHei UI";
        } else {
            candidate = Utf8ToWide(family);
        }
        if (!candidate.empty() && FindFamilyIndex(candidate, nullptr)) {
            return candidate;
        }
        if (comma == std::string::npos) {
            break;
        }
        offset = comma + 1;
    }
    for (size_t slot = 0; slot < kDefaultFamilyCount; ++slot) {
        const std::wstring candidate(kDefaultFamilies[slot]);
        if (FindFamilyIndex(candidate, nullptr)) {
            return candidate;
        }
    }

    // 默认族都不存在时，退到系统字体集合中的第一个族。
    if (collection_->GetFontFamilyCount() > 0) {
        ComPtr<IDWriteFontFamily> first_family;
        if (SUCCEEDED(collection_->GetFontFamily(0, first_family.GetAddressOf())) &&
            first_family) {
            ComPtr<IDWriteLocalizedStrings> names;
            if (SUCCEEDED(first_family->GetFamilyNames(names.GetAddressOf())) && names) {
                UINT32 length = 0;
                if (SUCCEEDED(names->GetStringLength(0, &length)) && length > 0) {
                    std::wstring name(static_cast<size_t>(length) + 1, L'\0');
                    if (SUCCEEDED(names->GetString(0, name.data(), length + 1))) {
                        name.resize(static_cast<size_t>(length));
                        return name;
                    }
                }
            }
        }
    }
    return std::wstring();
}

FontId TextServiceImpl::CreateFont(const FontRequest& request) {
    if (!Initialize()) {
        return 0;
    }

    // 字号非法时使用 16 DIP，避免零尺寸或极端尺寸破坏度量。
    float em_size = request.size;
    if (!(em_size > 0.0f) || em_size > 4096.0f) {
        em_size = 16.0f;
    }

    const std::wstring family = ResolveFamily(request.family);
    if (family.empty()) {
        return 0;
    }

    FontKey key;
    key.family = family;
    key.size = em_size;
    key.bold = request.bold;
    key.italic = request.italic;

    auto cached = font_keys_.find(key);
    if (cached != font_keys_.end()) {
        return cached->second;
    }

    UINT32 family_index = 0;
    if (!FindFamilyIndex(family, &family_index)) {
        return 0;
    }

    ComPtr<IDWriteFontFamily> font_family;
    if (FAILED(collection_->GetFontFamily(family_index, font_family.GetAddressOf())) ||
        !font_family) {
        return 0;
    }

    ComPtr<IDWriteFont> matching_font;
    HRESULT result = font_family->GetFirstMatchingFont(
        WeightFor(request.bold), DWRITE_FONT_STRETCH_NORMAL, StyleFor(request.italic),
        matching_font.GetAddressOf());
    if (FAILED(result) || !matching_font) {
        return 0;
    }

    ComPtr<IDWriteFontFace> font_face;
    result = matching_font->CreateFontFace(font_face.GetAddressOf());
    if (FAILED(result) || !font_face) {
        return 0;
    }

    DWRITE_FONT_METRICS font_metrics = {};
    font_face->GetMetrics(&font_metrics);
    if (font_metrics.designUnitsPerEm == 0) {
        return 0;
    }

    // 设计单位到像素的换算比例：emSize / designUnitsPerEm。
    const float scale = em_size / static_cast<float>(font_metrics.designUnitsPerEm);
    FontMetrics metrics;
    metrics.ascent = static_cast<float>(font_metrics.ascent) * scale;
    metrics.descent = static_cast<float>(font_metrics.descent) * scale;
    metrics.line_gap = static_cast<float>(font_metrics.lineGap) * scale;
    metrics.height = metrics.ascent + metrics.descent + metrics.line_gap;

    std::unique_ptr<FontRecord> record(new FontRecord());
    record->family = family;
    record->em_size = em_size;
    record->bold = request.bold;
    record->italic = request.italic;
    record->units_per_em = font_metrics.designUnitsPerEm;
    record->face = font_face;
    record->metrics = metrics;
    record->fallback_faces.resize(kFallbackFamilyCount);
    record->fallback_attempted.assign(kFallbackFamilyCount, 0);

    const FontId font_id = next_font_id_;
    if (font_id == 0) {
        return 0;
    }
    ++next_font_id_;
    fonts_.emplace(font_id, std::move(record));
    font_keys_.emplace(key, font_id);
    return font_id;
}

FontMetrics TextServiceImpl::Metrics(FontId font) const {
    const FontRecord* record = FindFont(font);
    if (record == nullptr) {
        return FontMetrics();
    }
    return record->metrics;
}

bool TextServiceImpl::QueryGlyphIndex(IDWriteFontFace* face, uint32_t codepoint,
                                      uint16_t* glyph_index) const {
    if (face == nullptr || glyph_index == nullptr || codepoint > 0x10FFFF) {
        return false;
    }
    UINT32 code_point = codepoint;
    UINT16 index = 0;
    HRESULT result = face->GetGlyphIndices(&code_point, 1, &index);
    if (FAILED(result) || index == 0) {
        return false;
    }
    *glyph_index = index;
    return true;
}

float TextServiceImpl::DesignAdvance(IDWriteFontFace* face, uint16_t units_per_em, float em_size,
                                     uint16_t glyph_index) const {
    if (face == nullptr || units_per_em == 0 || glyph_index == 0) {
        return 0.0f;
    }
    DWRITE_GLYPH_METRICS design_metrics = {};
    HRESULT result = face->GetDesignGlyphMetrics(&glyph_index, 1, &design_metrics, FALSE);
    if (FAILED(result)) {
        return 0.0f;
    }
    const float scale = em_size / static_cast<float>(units_per_em);
    return static_cast<float>(design_metrics.advanceWidth) * scale;
}

IDWriteFontFace* TextServiceImpl::EnsureFallbackFace(FontRecord& record, size_t slot) {
    if (slot >= kFallbackFamilyCount || slot >= record.fallback_attempted.size()) {
        return nullptr;
    }
    if (record.fallback_attempted[slot]) {
        return record.fallback_faces[slot].Get();
    }
    record.fallback_attempted[slot] = 1;

    // 与主字体同名的族无需重复尝试。
    const std::wstring candidate(kFallbackFamilies[slot]);
    if (record.family == candidate) {
        return nullptr;
    }

    UINT32 family_index = 0;
    if (!FindFamilyIndex(candidate, &family_index)) {
        return nullptr;
    }

    ComPtr<IDWriteFontFamily> font_family;
    if (FAILED(collection_->GetFontFamily(family_index, font_family.GetAddressOf())) ||
        !font_family) {
        return nullptr;
    }

    ComPtr<IDWriteFont> matching_font;
    HRESULT result = font_family->GetFirstMatchingFont(
        WeightFor(record.bold), DWRITE_FONT_STRETCH_NORMAL, StyleFor(record.italic),
        matching_font.GetAddressOf());
    if (FAILED(result) || !matching_font) {
        return nullptr;
    }

    ComPtr<IDWriteFontFace> font_face;
    result = matching_font->CreateFontFace(font_face.GetAddressOf());
    if (FAILED(result) || !font_face) {
        return nullptr;
    }

    record.fallback_faces[slot] = font_face;
    return record.fallback_faces[slot].Get();
}

GlyphResolution TextServiceImpl::ResolveGlyph(FontRecord& record, uint32_t codepoint) {
    auto cached = record.resolutions.find(codepoint);
    if (cached != record.resolutions.end()) {
        return cached->second;
    }

    GlyphResolution resolution;
    if (codepoint > 0x10FFFF) {
        record.resolutions.emplace(codepoint, resolution);
        return resolution;
    }

    // 先尝试主字体。
    uint16_t glyph_index = 0;
    if (QueryGlyphIndex(record.face.Get(), codepoint, &glyph_index)) {
        resolution.face_slot = kMainFaceSlot;
        resolution.glyph_index = glyph_index;
        resolution.advance =
            DesignAdvance(record.face.Get(), record.units_per_em, record.em_size, glyph_index);
        record.resolutions.emplace(codepoint, resolution);
        return resolution;
    }

    // 主字体返回 .notdef 时按回退顺序查找首个存在该字形的字体。
    for (size_t slot = 0; slot < kFallbackFamilyCount; ++slot) {
        IDWriteFontFace* fallback_face = EnsureFallbackFace(record, slot);
        if (fallback_face == nullptr) {
            continue;
        }
        uint16_t fallback_index = 0;
        if (!QueryGlyphIndex(fallback_face, codepoint, &fallback_index)) {
            continue;
        }
        DWRITE_FONT_METRICS fallback_metrics = {};
        fallback_face->GetMetrics(&fallback_metrics);
        if (fallback_metrics.designUnitsPerEm == 0) {
            continue;
        }
        resolution.face_slot = static_cast<int>(slot);
        resolution.glyph_index = fallback_index;
        resolution.advance = DesignAdvance(fallback_face, fallback_metrics.designUnitsPerEm,
                                          record.em_size, fallback_index);
        record.resolutions.emplace(codepoint, resolution);
        return resolution;
    }

    record.resolutions.emplace(codepoint, resolution);
    return resolution;
}

float TextServiceImpl::Advance(FontId font, uint32_t codepoint) {
    FontRecord* record = FindFont(font);
    if (record == nullptr) {
        return 0.0f;
    }
    // 控制字符不参与排版步进。
    if (codepoint < 0x20) {
        return 0.0f;
    }
    return ResolveGlyph(*record, codepoint).advance;
}

float TextServiceImpl::Measure(FontId font, const std::string& utf8_text) {
    FontRecord* record = FindFont(font);
    if (record == nullptr) {
        return 0.0f;
    }
    float total = 0.0f;
    size_t offset = 0;
    while (offset < utf8_text.size()) {
        const Utf8Char decoded = DecodeUtf8(utf8_text, offset);
        if (decoded.length == 0) {
            break;
        }
        offset += decoded.length;
        if (decoded.codepoint == 0 || decoded.codepoint < 0x20) {
            continue;
        }
        total += ResolveGlyph(*record, decoded.codepoint).advance;
    }
    return total;
}

bool TextServiceImpl::GrowAtlas(int minimum_width, int minimum_height) {
    const int target_width = std::max(minimum_width, atlas_width_);
    const int target_height = std::max(minimum_height, atlas_height_);
    if (target_width > kMaxAtlasSize || target_height > kMaxAtlasSize) {
        return false;
    }

    int new_width = (atlas_width_ > 0) ? atlas_width_ : kInitialAtlasSize;
    int new_height = (atlas_height_ > 0) ? atlas_height_ : kInitialAtlasSize;
    while (new_width < target_width) {
        new_width *= 2;
    }
    while (new_height < target_height) {
        new_height *= 2;
    }
    new_width = std::min(new_width, kMaxAtlasSize);
    new_height = std::min(new_height, kMaxAtlasSize);
    if (new_width < target_width || new_height < target_height) {
        return false;
    }
    if (new_width == atlas_width_ && new_height == atlas_height_) {
        return false;
    }

    // 扩展时按行复制旧内容，已有字形的图集坐标保持不变。
    const size_t buffer_size = static_cast<size_t>(new_width) * static_cast<size_t>(new_height);
    std::vector<uint8_t> next_atlas(buffer_size, 0);
    if (atlas_width_ > 0 && atlas_height_ > 0 && !atlas_.empty()) {
        const size_t copy_width = static_cast<size_t>(atlas_width_);
        for (int row = 0; row < atlas_height_; ++row) {
            const size_t source_offset = static_cast<size_t>(row) * copy_width;
            const size_t target_offset = static_cast<size_t>(row) * static_cast<size_t>(new_width);
            std::copy(atlas_.begin() + static_cast<std::ptrdiff_t>(source_offset),
                      atlas_.begin() + static_cast<std::ptrdiff_t>(source_offset + copy_width),
                      next_atlas.begin() + static_cast<std::ptrdiff_t>(target_offset));
        }
    }

    atlas_ = std::move(next_atlas);
    atlas_width_ = new_width;
    atlas_height_ = new_height;
    ++atlas_version_;
    return true;
}

bool TextServiceImpl::PlaceAtlasRect(int width, int height, int* out_x, int* out_y) {
    if (width <= 0 || height <= 0 || out_x == nullptr || out_y == nullptr) {
        return false;
    }
    if (width > kMaxAtlasSize || height > kMaxAtlasSize) {
        return false;
    }

    // 当前宽度放不下一行时先扩宽。
    if (width > atlas_width_) {
        if (!GrowAtlas(width, atlas_height_)) {
            return false;
        }
    }
    // 当前高度放不下一列时先扩高。
    if (height > atlas_height_) {
        if (!GrowAtlas(atlas_width_, height)) {
            return false;
        }
    }

    // 当前货架放不下时换行，货架高度加 1 像素间距。
    if (cursor_x_ + width > atlas_width_) {
        cursor_x_ = 0;
        cursor_y_ += shelf_height_ + kAtlasPadding;
        shelf_height_ = 0;
    }
    while (cursor_y_ + height > atlas_height_) {
        const int next_height = atlas_height_ * 2;
        if (next_height <= atlas_height_) {
            return false;
        }
        if (!GrowAtlas(atlas_width_, std::min(next_height, kMaxAtlasSize))) {
            return false;
        }
    }

    *out_x = cursor_x_;
    *out_y = cursor_y_;
    cursor_x_ += width + kAtlasPadding;
    shelf_height_ = std::max(shelf_height_, height);
    return true;
}

GlyphInfo TextServiceImpl::Rasterize(FontId font, uint32_t codepoint) {
    GlyphInfo info;
    FontRecord* record = FindFont(font);
    if (record == nullptr || codepoint > 0x10FFFF) {
        return info;
    }

    auto cached = record->glyphs.find(codepoint);
    if (cached != record->glyphs.end()) {
        return cached->second;
    }

    if (codepoint < 0x20) {
        record->glyphs.emplace(codepoint, info);
        return info;
    }

    const GlyphResolution resolution = ResolveGlyph(*record, codepoint);
    info.advance = resolution.advance;
    if (!resolution.Resolved() || IsWhitespaceCodepoint(codepoint)) {
        record->glyphs.emplace(codepoint, info);
        return info;
    }

    IDWriteFontFace* face = (resolution.face_slot == kMainFaceSlot)
                                ? record->face.Get()
                                : record->fallback_faces[static_cast<size_t>(
                                      resolution.face_slot)].Get();
    if (face == nullptr) {
        record->glyphs.emplace(codepoint, info);
        return info;
    }

    // 用 ClearType 渲染以获得亚像素覆盖率，分析对象建立后按 3x1 纹理取回灰度覆盖率。
    // pixelsPerDip 取 1 使像素与 DIP 对齐。
    float glyph_advance = resolution.advance;
    DWRITE_GLYPH_RUN glyph_run = {};
    glyph_run.fontFace = face;
    glyph_run.fontEmSize = record->em_size;
    glyph_run.glyphCount = 1;
    glyph_run.glyphIndices = &resolution.glyph_index;
    glyph_run.glyphAdvances = &glyph_advance;
    glyph_run.glyphOffsets = nullptr;
    glyph_run.isSideways = FALSE;
    glyph_run.bidiLevel = 0;

    ComPtr<IDWriteGlyphRunAnalysis> analysis;
    HRESULT result = factory_->CreateGlyphRunAnalysis(
        &glyph_run, 1.0f, nullptr, DWRITE_RENDERING_MODE_CLEARTYPE_NATURAL_SYMMETRIC,
        DWRITE_MEASURING_MODE_NATURAL, 0.0f, 0.0f, analysis.GetAddressOf());
    if (FAILED(result) || !analysis) {
        record->glyphs.emplace(codepoint, info);
        return info;
    }

    DWRITE_TEXTURE_TYPE texture_type = DWRITE_TEXTURE_CLEARTYPE_3x1;
    RECT bounds = {};
    result = analysis->GetAlphaTextureBounds(texture_type, &bounds);
    if (SUCCEEDED(result) && (bounds.right <= bounds.left || bounds.bottom <= bounds.top)) {
        // 该字形没有 ClearType 覆盖率（例如仅含点阵字形），退回到单色位图路径。
        result = factory_->CreateGlyphRunAnalysis(
            &glyph_run, 1.0f, nullptr, DWRITE_RENDERING_MODE_ALIASED,
            DWRITE_MEASURING_MODE_NATURAL, 0.0f, 0.0f, analysis.ReleaseAndGetAddressOf());
        if (FAILED(result) || !analysis) {
            record->glyphs.emplace(codepoint, info);
            return info;
        }
        texture_type = DWRITE_TEXTURE_ALIASED_1x1;
        result = analysis->GetAlphaTextureBounds(texture_type, &bounds);
    }
    if (FAILED(result) || bounds.right <= bounds.left || bounds.bottom <= bounds.top) {
        record->glyphs.emplace(codepoint, info);
        return info;
    }

    const int bitmap_width = static_cast<int>(bounds.right - bounds.left);
    const int bitmap_height = static_cast<int>(bounds.bottom - bounds.top);
    if (bitmap_width <= 0 || bitmap_height <= 0 || bitmap_width > kMaxAtlasSize ||
        bitmap_height > kMaxAtlasSize) {
        record->glyphs.emplace(codepoint, info);
        return info;
    }

    // ClearType 纹理每像素 3 字节（RGB 三个子像素），单色纹理每像素 1 字节。
    const size_t pixel_count =
        static_cast<size_t>(bitmap_width) * static_cast<size_t>(bitmap_height);
    const size_t buffer_size =
        pixel_count * (texture_type == DWRITE_TEXTURE_CLEARTYPE_3x1 ? 3 : 1);
    std::vector<uint8_t> coverage(pixel_count, 0);
    std::vector<uint8_t> raw_texture(buffer_size, 0);
    result = analysis->CreateAlphaTexture(texture_type, &bounds, raw_texture.data(),
                                          static_cast<UINT32>(buffer_size));
    if (FAILED(result)) {
        record->glyphs.emplace(codepoint, info);
        return info;
    }

    if (texture_type == DWRITE_TEXTURE_CLEARTYPE_3x1) {
        // 三个子像素通道取平均得到 8 位灰度覆盖率，供单通道图集与灰度抗锯齿使用。
        for (size_t index = 0; index < pixel_count; ++index) {
            const size_t source = index * 3;
            const uint32_t sum = static_cast<uint32_t>(raw_texture[source]) +
                                 static_cast<uint32_t>(raw_texture[source + 1]) +
                                 static_cast<uint32_t>(raw_texture[source + 2]);
            coverage[index] = static_cast<uint8_t>((sum + 1) / 3);
        }
    } else {
        coverage = std::move(raw_texture);
    }

    int atlas_x = 0;
    int atlas_y = 0;
    if (!PlaceAtlasRect(bitmap_width, bitmap_height, &atlas_x, &atlas_y)) {
        record->glyphs.emplace(codepoint, info);
        return info;
    }

    // 覆盖位图按行复制到图集，行优先、8 位覆盖值。
    for (int row = 0; row < bitmap_height; ++row) {
        const size_t source_offset = static_cast<size_t>(row) * static_cast<size_t>(bitmap_width);
        const size_t target_offset = static_cast<size_t>(atlas_y + row) *
                                         static_cast<size_t>(atlas_width_) +
                                     static_cast<size_t>(atlas_x);
        std::copy(coverage.begin() + static_cast<std::ptrdiff_t>(source_offset),
                  coverage.begin() + static_cast<std::ptrdiff_t>(source_offset + bitmap_width),
                  atlas_.begin() + static_cast<std::ptrdiff_t>(target_offset));
    }
    ++atlas_version_;

    info.bearing_x = static_cast<float>(bounds.left);
    info.bearing_y = static_cast<float>(-bounds.top);
    info.atlas_x = atlas_x;
    info.atlas_y = atlas_y;
    info.atlas_w = bitmap_width;
    info.atlas_h = bitmap_height;
    info.has_bitmap = true;

    record->glyphs.emplace(codepoint, info);
    return info;
}

}  // namespace

std::unique_ptr<TextService> TextService::Create() {
    return std::unique_ptr<TextService>(new TextServiceImpl());
}

}  // namespace web
