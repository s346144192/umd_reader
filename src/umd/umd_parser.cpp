#include "umd/umd_parser.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <cstring>
#include <fstream>
#include <limits>

#include "miniz.h"

namespace umd {
namespace {

constexpr uint32_t kMagic = 0xDE9A9B89u;
constexpr size_t kMaxFileBytes = 512ull * 1024 * 1024;
constexpr size_t kMaxContentBytes = 512ull * 1024 * 1024;

// 顺序读取字节流，所有读取操作都做边界检查。
class Cursor {
public:
    Cursor(const uint8_t* data, size_t size) : data_(data), size_(size), position_(0) {}

    size_t Position() const { return position_; }
    size_t Remaining() const { return size_ - position_; }

    bool ReadU8(uint8_t& value) {
        if (Remaining() < 1) {
            return false;
        }
        value = data_[position_++];
        return true;
    }

    bool ReadU16LE(uint16_t& value) {
        if (Remaining() < 2) {
            return false;
        }
        value = static_cast<uint16_t>(data_[position_] | (data_[position_ + 1] << 8));
        position_ += 2;
        return true;
    }

    bool ReadU32LE(uint32_t& value) {
        if (Remaining() < 4) {
            return false;
        }
        value = static_cast<uint32_t>(data_[position_]) |
                (static_cast<uint32_t>(data_[position_ + 1]) << 8) |
                (static_cast<uint32_t>(data_[position_ + 2]) << 16) |
                (static_cast<uint32_t>(data_[position_ + 3]) << 24);
        position_ += 4;
        return true;
    }

    bool ReadBytes(size_t count, const uint8_t*& out) {
        if (Remaining() < count) {
            return false;
        }
        out = data_ + position_;
        position_ += count;
        return true;
    }

    bool Skip(size_t count) {
        if (Remaining() < count) {
            return false;
        }
        position_ += count;
        return true;
    }

private:
    const uint8_t* data_;
    size_t size_;
    size_t position_;
};

std::wstring DecodeUtf16Le(const uint8_t* data, size_t byteLength, bool& hasOddTail) {
    hasOddTail = (byteLength % 2) != 0;
    const size_t unitCount = byteLength / 2;
    std::wstring result;
    result.reserve(unitCount);
    for (size_t i = 0; i < unitCount; ++i) {
        const uint16_t unit = static_cast<uint16_t>(data[i * 2] | (data[i * 2 + 1] << 8));
        result.push_back(static_cast<wchar_t>(unit));
    }
    return result;
}

// 把孤立代理项替换为替换字符，避免排版与渲染出现异常。
void SanitizeSurrogates(std::wstring& text) {
    for (size_t i = 0; i < text.size(); ++i) {
        const wchar_t value = text[i];
        if (value >= 0xD800 && value <= 0xDBFF) {
            if (i + 1 < text.size() && text[i + 1] >= 0xDC00 && text[i + 1] <= 0xDFFF) {
                ++i;
                continue;
            }
            text[i] = static_cast<wchar_t>(0xFFFD);
        } else if (value >= 0xDC00 && value <= 0xDFFF) {
            text[i] = static_cast<wchar_t>(0xFFFD);
        }
    }
}

std::string DetectImageMimeType(const std::vector<uint8_t>& bytes) {
    if (bytes.size() >= 3 && bytes[0] == 0xFF && bytes[1] == 0xD8 && bytes[2] == 0xFF) {
        return "image/jpeg";
    }
    if (bytes.size() >= 8 && bytes[0] == 0x89 && bytes[1] == 0x50 && bytes[2] == 0x4E && bytes[3] == 0x47) {
        return "image/png";
    }
    if (bytes.size() >= 4 && bytes[0] == 0x47 && bytes[1] == 0x49 && bytes[2] == 0x46) {
        return "image/gif";
    }
    if (bytes.size() >= 2 && bytes[0] == 0x42 && bytes[1] == 0x4D) {
        return "image/bmp";
    }
    if (bytes.size() >= 12 && bytes[0] == 0x52 && bytes[1] == 0x49 && bytes[2] == 0x46 && bytes[3] == 0x46 &&
        bytes[8] == 0x57 && bytes[9] == 0x45 && bytes[10] == 0x42 && bytes[11] == 0x50) {
        return "image/webp";
    }
    return std::string();
}

// 使用 miniz 的流式 inflate 接口解压单个 zlib 数据块。
bool InflateBlock(const uint8_t* data, size_t size, std::vector<uint8_t>& contentBytes, std::wstring& error) {
    if (size == 0) {
        return true;
    }
    if (size > std::numeric_limits<mz_uint>::max()) {
        error = L"正文压缩块长度超出解压器上限";
        return false;
    }
    mz_stream stream{};
    stream.next_in = const_cast<mz_uint8*>(data);
    stream.avail_in = static_cast<mz_uint>(size);
    if (mz_inflateInit(&stream) != MZ_OK) {
        error = L"初始化 zlib 解压器失败";
        return false;
    }

    std::vector<uint8_t> buffer(64 * 1024);
    bool succeed = true;
    for (;;) {
        if (stream.total_out >= buffer.size()) {
            buffer.resize(buffer.size() * 2);
        }
        stream.next_out = buffer.data() + stream.total_out;
        stream.avail_out = static_cast<mz_uint>(buffer.size() - stream.total_out);
        const int status = mz_inflate(&stream, MZ_NO_FLUSH);
        if (status == MZ_STREAM_END) {
            break;
        }
        if (status == MZ_OK || status == MZ_BUF_ERROR) {
            if (stream.avail_out == 0) {
                continue;  // 输出缓冲区已满，扩容后继续
            }
            if (stream.avail_in == 0) {
                succeed = false;
                error = L"正文压缩块数据不完整";
                break;
            }
            continue;
        }
        succeed = false;
        error = L"正文压缩块解压失败（错误码 " + std::to_wstring(status) + L"）";
        break;
    }

    if (succeed) {
        if (contentBytes.size() + stream.total_out > kMaxContentBytes) {
            succeed = false;
            error = L"正文解压后的总长度超出上限";
        } else {
            contentBytes.insert(contentBytes.end(), buffer.begin(),
                                buffer.begin() + static_cast<std::ptrdiff_t>(stream.total_out));
        }
    }
    mz_inflateEnd(&stream);
    return succeed;
}

void AddWarning(Book& book, const std::wstring& message) {
    book.warnings.push_back(message);
}

}  // namespace

bool ParseFile(const std::filesystem::path& path, Book& book, std::wstring& error) {
    book = Book{};
    error.clear();

    std::error_code fileError;
    const uintmax_t fileSize = std::filesystem::file_size(path, fileError);
    if (fileError || fileSize == 0) {
        error = L"无法读取文件：" + path.wstring();
        return false;
    }
    if (fileSize > kMaxFileBytes) {
        error = L"文件超过 512MB，超出阅读器处理范围";
        return false;
    }

    std::vector<uint8_t> fileBytes(static_cast<size_t>(fileSize));
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = L"无法打开文件：" + path.wstring();
        return false;
    }
    file.read(reinterpret_cast<char*>(fileBytes.data()), static_cast<std::streamsize>(fileBytes.size()));
    if (!file) {
        error = L"读取文件内容失败：" + path.wstring();
        return false;
    }

    Cursor cursor(fileBytes.data(), fileBytes.size());
    uint32_t magic = 0;
    if (!cursor.ReadU32LE(magic) || magic != kMagic) {
        error = L"不是有效的 UMD 文件：文件标识不匹配";
        return false;
    }

    bool finished = false;
    bool sawTypeBlock = false;
    uint16_t carrierType = 0;      // 当前承载附加数据的块类型
    uint32_t carrierCheck = 0;     // 块头中记录的校验随机数
    uint32_t declaredContentBytes = 0;
    std::vector<uint32_t> chapterOffsets;
    std::vector<std::wstring> chapterTitles;
    std::vector<uint8_t> contentBytes;
    std::vector<uint8_t> coverBytes;
    std::vector<ImageResource> images;
    size_t unknownBytes = 0;

    while (cursor.Remaining() > 0 && !finished) {
        uint8_t tag = 0;
        if (!cursor.ReadU8(tag)) {
            break;
        }

        if (tag == '#') {
            uint16_t type = 0;
            uint8_t flag = 0;
            uint8_t lengthByte = 0;
            if (!cursor.ReadU16LE(type) || !cursor.ReadU8(flag) || !cursor.ReadU8(lengthByte)) {
                error = L"功能块头部不完整，文件可能已损坏";
                return false;
            }
            const size_t payloadLength = lengthByte >= 5 ? lengthByte - 5 : 0;

            switch (type) {
            case 0x01: {  // 文件起始块，包含格式类型
                uint8_t umdType = 0;
                if (!cursor.ReadU8(umdType) || !cursor.Skip(2)) {
                    error = L"文件起始块不完整";
                    return false;
                }
                sawTypeBlock = true;
                book.formatType = umdType;
                if (umdType != 1) {
                    error = L"当前版本仅支持文本格式 UMD（类型 1），检测到类型 " +
                            std::to_wstring(umdType) + L"（动漫格式暂不支持）";
                    return false;
                }
                break;
            }
            case 0x02:
            case 0x03:
            case 0x04:
            case 0x05:
            case 0x06:
            case 0x07:
            case 0x08:
            case 0x09: {
                const uint8_t* payload = nullptr;
                if (!cursor.ReadBytes(payloadLength, payload)) {
                    error = L"元数据块长度超出文件范围";
                    return false;
                }
                bool hasOddTail = false;
                std::wstring text = DecodeUtf16Le(payload, payloadLength, hasOddTail);
                if (hasOddTail) {
                    AddWarning(book, L"元数据字段长度为奇数，已忽略末尾单字节");
                }
                switch (type) {
                case 0x02: book.title = std::move(text); break;
                case 0x03: book.author = std::move(text); break;
                case 0x04: book.year = std::move(text); break;
                case 0x05: book.month = std::move(text); break;
                case 0x06: book.day = std::move(text); break;
                case 0x07: book.genre = std::move(text); break;
                case 0x08: book.publisher = std::move(text); break;
                case 0x09: book.retailer = std::move(text); break;
                default: break;
                }
                break;
            }
            case 0x0A:  // 内容标识
                if (!cursor.Skip(payloadLength)) {
                    error = L"内容标识块长度超出文件范围";
                    return false;
                }
                break;
            case 0x0B:
                if (!cursor.ReadU32LE(declaredContentBytes)) {
                    error = L"正文长度块不完整";
                    return false;
                }
                break;
            case 0x0C: {  // 文件结束块
                uint32_t declaredFileSize = 0;
                if (!cursor.ReadU32LE(declaredFileSize)) {
                    error = L"文件结束块不完整";
                    return false;
                }
                if (declaredFileSize != 0 && declaredFileSize != fileBytes.size()) {
                    AddWarning(book, L"声明的文件长度（" + std::to_wstring(declaredFileSize) +
                                          L" 字节）与实际文件大小（" +
                                          std::to_wstring(fileBytes.size()) + L" 字节）不一致");
                }
                finished = true;
                break;
            }
            case 0x0E:
            case 0x0F:  // 图片资源块，实际数据由后续 '$' 块承载
                if (!cursor.Skip(payloadLength)) {
                    error = L"图片元数据块长度超出文件范围";
                    return false;
                }
                carrierType = type;
                break;
            case 0x81:
            case 0x83:
            case 0x84:  // 正文结束、章节偏移、章节标题/正文
                if (!cursor.ReadU32LE(carrierCheck)) {
                    error = L"功能块校验字段不完整";
                    return false;
                }
                carrierType = type;
                break;
            case 0x82:  // 封面
                if (!cursor.Skip(1) || !cursor.ReadU32LE(carrierCheck)) {
                    error = L"封面块头部不完整";
                    return false;
                }
                carrierType = type;
                break;
            case 0x87:  // 页面偏移参数：字号、屏宽、数据块随机数
                if (!cursor.Skip(2) || !cursor.Skip(4)) {
                    error = L"页面偏移块不完整";
                    return false;
                }
                break;
            case 0xF1:  // 许可证块，不改变当前承载类型
                if (!cursor.Skip(std::min<size_t>(payloadLength, 16))) {
                    error = L"许可证块长度超出文件范围";
                    return false;
                }
                break;
            default:
                if (!cursor.Skip(payloadLength)) {
                    error = L"未知功能块长度超出文件范围";
                    return false;
                }
                break;
            }
            continue;
        }

        if (tag == '$') {
            uint32_t check = 0;
            uint32_t lengthField = 0;
            if (!cursor.ReadU32LE(check) || !cursor.ReadU32LE(lengthField)) {
                error = L"附加数据块头部不完整";
                return false;
            }
            if (lengthField < 9) {
                error = L"附加数据块长度字段非法";
                return false;
            }
            const size_t payloadLength = lengthField - 9;
            const uint8_t* payload = nullptr;
            if (!cursor.ReadBytes(payloadLength, payload)) {
                error = L"附加数据块长度超出文件范围";
                return false;
            }

            switch (carrierType) {
            case 0x81:  // 页面偏移数据，阅读器按字符偏移重新分页，无需使用
                break;
            case 0x82:
                coverBytes.assign(payload, payload + payloadLength);
                break;
            case 0x83: {
                chapterOffsets.clear();
                if (payloadLength % 4 != 0) {
                    AddWarning(book, L"章节偏移表长度不是 4 的倍数，末尾数据已忽略");
                }
                const size_t count = payloadLength / 4;
                chapterOffsets.reserve(count);
                bool hasOddOffset = false;
                for (size_t i = 0; i < count; ++i) {
                    const uint8_t* item = payload + i * 4;
                    const uint32_t raw = static_cast<uint32_t>(item[0]) |
                                         (static_cast<uint32_t>(item[1]) << 8) |
                                         (static_cast<uint32_t>(item[2]) << 16) |
                                         (static_cast<uint32_t>(item[3]) << 24);
                    if (raw % 2 != 0) {
                        hasOddOffset = true;
                    }
                    chapterOffsets.push_back(raw / 2);
                }
                if (hasOddOffset) {
                    AddWarning(book, L"章节偏移不是 UTF-16 码元对齐值，已向下取整");
                }
                break;
            }
            case 0x84:
                if (check == carrierCheck) {
                    // 校验随机数与块头一致，说明是章节标题列表
                    size_t offset = 0;
                    while (offset < payloadLength) {
                        const uint8_t titleLength = payload[offset++];
                        if (offset + titleLength > payloadLength) {
                            AddWarning(book, L"章节标题数据被截断");
                            break;
                        }
                        bool hasOddTail = false;
                        std::wstring title = DecodeUtf16Le(payload + offset, titleLength, hasOddTail);
                        if (hasOddTail) {
                            AddWarning(book, L"章节标题长度为奇数，已忽略末尾单字节");
                        }
                        chapterTitles.push_back(std::move(title));
                        offset += titleLength;
                    }
                } else {
                    // 校验随机数不同，说明是 zlib 压缩的正文数据块
                    if (!InflateBlock(payload, payloadLength, contentBytes, error)) {
                        return false;
                    }
                }
                break;
            case 0x0E:
            case 0x0F: {
                ImageResource image;
                image.bytes.assign(payload, payload + payloadLength);
                image.mimeType = DetectImageMimeType(image.bytes);
                if (image.mimeType.empty()) {
                    AddWarning(book, L"插图块内容不是可识别的图片格式，已跳过");
                } else {
                    images.push_back(std::move(image));
                }
                break;
            }
            default:
                break;
            }
            continue;
        }

        ++unknownBytes;  // 容错跳过无法识别的填充字节
    }

    if (!sawTypeBlock) {
        AddWarning(book, L"文件中缺少文件类型块，按文本格式继续解析");
    }
    if (!finished) {
        AddWarning(book, L"未找到文件结束块（0x0C），文件可能被截断");
    }
    if (unknownBytes > 0) {
        AddWarning(book, L"跳过了 " + std::to_wstring(unknownBytes) + L" 个无法识别的字节");
    }
    if (contentBytes.empty()) {
        error = L"文件中没有可读取的正文数据";
        return false;
    }
    if (contentBytes.size() % 2 != 0) {
        contentBytes.pop_back();
        AddWarning(book, L"正文总字节数不是偶数，已忽略末尾单字节");
    }

    book.declaredContentBytes = declaredContentBytes;
    bool hasOddTail = false;
    book.content = DecodeUtf16Le(contentBytes.data(), contentBytes.size(), hasOddTail);
    if (hasOddTail) {
        AddWarning(book, L"正文末尾存在未配对的单字节，已忽略");
    }
    SanitizeSurrogates(book.content);

    if (declaredContentBytes != 0 && declaredContentBytes != contentBytes.size()) {
        AddWarning(book, L"声明的正文长度（" + std::to_wstring(declaredContentBytes) +
                              L" 字节）与实际解压长度（" + std::to_wstring(contentBytes.size()) +
                              L" 字节）不一致，已按实际内容处理");
    }

    const uint32_t totalChars = book.ContentCharCount();
    if (totalChars == 0) {
        error = L"正文内容为空";
        return false;
    }

    if (chapterOffsets.empty()) {
        Chapter chapter;
        chapter.title = chapterTitles.empty() ? L"正文" : chapterTitles.front();
        if (chapter.title.empty()) {
            chapter.title = L"正文";
        }
        chapter.charOffset = 0;
        chapter.charLength = totalChars;
        book.chapters.push_back(chapter);
    } else {
        for (size_t i = 0; i < chapterOffsets.size(); ++i) {
            if (chapterOffsets[i] > totalChars) {
                AddWarning(book, L"章节偏移超出正文范围，已约束到正文末尾");
                chapterOffsets[i] = totalChars;
            }
            if (i > 0 && chapterOffsets[i] < chapterOffsets[i - 1]) {
                AddWarning(book, L"章节偏移未按升序排列，已修正");
                chapterOffsets[i] = chapterOffsets[i - 1];
            }
        }
        if (chapterTitles.size() > chapterOffsets.size()) {
            AddWarning(book, L"章节标题数量多于偏移数量，多余标题已忽略");
        }

        if (chapterOffsets.front() > 0) {
            Chapter preface;
            preface.title = L"前言";
            preface.charOffset = 0;
            preface.charLength = chapterOffsets.front();
            book.chapters.push_back(preface);
        }

        for (size_t i = 0; i < chapterOffsets.size(); ++i) {
            Chapter chapter;
            chapter.charOffset = chapterOffsets[i];
            const uint32_t end = (i + 1 < chapterOffsets.size()) ? chapterOffsets[i + 1] : totalChars;
            chapter.charLength = end > chapter.charOffset ? end - chapter.charOffset : 0;
            chapter.title = i < chapterTitles.size() ? chapterTitles[i]
                                                     : L"第 " + std::to_wstring(i + 1) + L" 章";
            if (chapter.title.empty()) {
                chapter.title = L"第 " + std::to_wstring(i + 1) + L" 章";
            }
            book.chapters.push_back(chapter);
        }
    }

    if (!coverBytes.empty()) {
        book.coverMimeType = DetectImageMimeType(coverBytes);
        if (book.coverMimeType.empty()) {
            AddWarning(book, L"封面数据不是可识别的图片格式，已忽略");
            coverBytes.clear();
        }
        book.coverBytes = std::move(coverBytes);
    }

    book.images = std::move(images);
    if (book.title.empty()) {
        book.title = path.stem().wstring();
    }
    return true;
}

namespace {

std::wstring TrimLine(const std::wstring& text) {
    size_t begin = 0;
    size_t end = text.size();
    const auto isSpace = [](wchar_t character) {
        return character == L' ' || character == L'\t' || character == L'\r' ||
               character == L'\n' || character == 0x3000;
    };
    while (begin < end && isSpace(text[begin])) {
        ++begin;
    }
    while (end > begin && isSpace(text[end - 1])) {
        --end;
    }
    return text.substr(begin, end - begin);
}

bool ConvertCodePage(UINT codePage, DWORD flags, const uint8_t* data, size_t size,
                     std::wstring& text) {
    text.clear();
    if (size == 0) {
        return true;
    }
    const int byteCount = static_cast<int>(size);
    const char* bytes = reinterpret_cast<const char*>(data);
    const int length = MultiByteToWideChar(codePage, flags, bytes, byteCount, nullptr, 0);
    if (length <= 0) {
        return false;
    }
    text.resize(static_cast<size_t>(length));
    const int written =
        MultiByteToWideChar(codePage, flags, bytes, byteCount, text.data(), length);
    if (written <= 0) {
        text.clear();
        return false;
    }
    text.resize(static_cast<size_t>(written));
    return true;
}

// 按检测到的编码解码文本，并通过 codePage 返回实际使用的 Windows 代码页。
bool DecodeTextBytes(const std::vector<uint8_t>& bytes, std::wstring& text,
                     uint32_t& codePage) {
    text.clear();
    codePage = 1200;
    if (bytes.empty()) {
        return true;
    }
    const uint8_t* data = bytes.data();
    const size_t size = bytes.size();

    if (size >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) {
        codePage = CP_UTF8;
        return ConvertCodePage(CP_UTF8, MB_ERR_INVALID_CHARS, data + 3, size - 3, text) ||
               ConvertCodePage(CP_UTF8, 0, data + 3, size - 3, text);
    }
    if (size >= 2 && data[0] == 0xFF && data[1] == 0xFE) {
        codePage = 1200;  // UTF-16LE
        const size_t units = (size - 2) / 2;
        text.resize(units);
        for (size_t i = 0; i < units; ++i) {
            text[i] = static_cast<wchar_t>(data[2 + i * 2] | (data[3 + i * 2] << 8));
        }
        return true;
    }
    if (size >= 2 && data[0] == 0xFE && data[1] == 0xFF) {
        codePage = 1201;  // UTF-16BE
        const size_t units = (size - 2) / 2;
        text.resize(units);
        for (size_t i = 0; i < units; ++i) {
            text[i] = static_cast<wchar_t>((data[2 + i * 2] << 8) | data[3 + i * 2]);
        }
        return true;
    }
    // 无 BOM：优先按 UTF-8 严格解码，失败后回退到系统 ANSI 代码页（简体中文通常为 GBK）。
    if (ConvertCodePage(CP_UTF8, MB_ERR_INVALID_CHARS, data, size, text)) {
        codePage = CP_UTF8;
        return true;
    }
    codePage = static_cast<uint32_t>(GetACP());
    return ConvertCodePage(CP_ACP, 0, data, size, text);
}

bool IsChapterNumber(wchar_t character) {
    if (character >= L'0' && character <= L'9') {
        return true;
    }
    switch (character) {
    case L'一':
    case L'二':
    case L'三':
    case L'四':
    case L'五':
    case L'六':
    case L'七':
    case L'八':
    case L'九':
    case L'十':
    case L'百':
    case L'千':
    case L'零':
    case L'〇':
    case L'两':
        return true;
    default:
        return false;
    }
}

// 识别「第X章/回/节/卷/篇/幕」形式的标题行。
// 规则收紧以减少正文误判：单位后必须结束或紧跟分隔符，整行不含句末标点，长度不超过 60。
// 允许「第 12 章」「第 一 章」这类数字前后带空白的写法。
bool MatchChapterHeading(const std::wstring& line, const std::vector<wchar_t>& units,
                         wchar_t& matchedUnit) {
    if (line.size() > 60) {
        return false;
    }
    const auto isSpace = [](wchar_t character) {
        return character == L' ' || character == L'\t' || character == 0x3000;
    };
    size_t position = 0;
    while (position < line.size() && isSpace(line[position])) {
        ++position;
    }
    if (position >= line.size() || line[position] != L'第') {
        return false;
    }
    ++position;
    while (position < line.size() && isSpace(line[position])) {
        ++position;
    }
    size_t digits = 0;
    while (position < line.size() && IsChapterNumber(line[position])) {
        ++position;
        ++digits;
    }
    if (digits == 0) {
        return false;
    }
    while (position < line.size() && isSpace(line[position])) {
        ++position;
    }
    if (position >= line.size()) {
        return false;
    }
    const wchar_t unit = line[position];
    if (std::find(units.begin(), units.end(), unit) == units.end()) {
        return false;
    }
    ++position;
    // 单位之后必须是行尾或标题分隔符；「第X章的第…段」属于正文。
    if (position < line.size()) {
        const wchar_t next = line[position];
        const bool separator = next == L' ' || next == L'\t' || next == 0x3000 ||
                               next == L'：' || next == L':' || next == L'、' ||
                               next == L'-' || next == L'—' || next == L'·';
        if (!separator) {
            return false;
        }
    }
    // 标题行通常不含句末标点与分句逗号。
    for (const wchar_t character : line) {
        if (character == L'。' || character == L'！' || character == L'？' ||
            character == L'；' || character == L'，' || character == L'…') {
            return false;
        }
    }
    matchedUnit = unit;
    return true;
}

std::vector<Chapter> DetectChapters(const std::wstring& text) {
    return DetectChaptersByUnits(text, {L'章', L'回', L'节', L'卷', L'篇', L'幕'});
}

}  // namespace

std::vector<Chapter> DetectChaptersByUnits(const std::wstring& text,
                                           const std::vector<wchar_t>& units) {
    std::vector<Chapter> chapters;
    if (text.empty() || units.empty() || units.size() > 3) {
        return chapters;
    }

    // 先按行扫描出所有标题，记录其层级与所在偏移。
    struct Heading {
        int rank = 0;
        uint32_t offset = 0;
        std::wstring title;
    };
    std::vector<Heading> headings;
    const size_t size = text.size();
    size_t lineStart = 0;
    while (lineStart < size) {
        size_t lineEnd = lineStart;
        while (lineEnd < size && text[lineEnd] != L'\r' && text[lineEnd] != L'\n') {
            ++lineEnd;
        }
        wchar_t unit = 0;
        const std::wstring line = text.substr(lineStart, lineEnd - lineStart);
        if (MatchChapterHeading(line, units, unit)) {
            Heading heading;
            heading.rank =
                static_cast<int>(std::find(units.begin(), units.end(), unit) - units.begin());
            heading.offset = static_cast<uint32_t>(lineStart);
            heading.title = TrimLine(line);
            headings.push_back(std::move(heading));
        }
        if (lineEnd >= size) {
            break;
        }
        size_t next = lineEnd + 1;
        if (text[lineEnd] == L'\r' && next < size && text[next] == L'\n') {
            ++next;
        }
        lineStart = next;
    }
    if (headings.empty()) {
        return chapters;
    }

    // 依据层级建立父子关系：父标题是其之前最近的更高层标题。
    std::vector<int> parentOf(headings.size(), -1);
    std::vector<int> stack;
    for (size_t i = 0; i < headings.size(); ++i) {
        while (!stack.empty() && headings[stack.back()].rank >= headings[i].rank) {
            stack.pop_back();
        }
        parentOf[i] = stack.empty() ? -1 : stack.back();
        stack.push_back(static_cast<int>(i));
    }
    std::vector<bool> hasChild(headings.size(), false);
    for (size_t i = 0; i < headings.size(); ++i) {
        if (parentOf[i] >= 0) {
            hasChild[static_cast<size_t>(parentOf[i])] = true;
        }
    }

    // 只有最深层标题成为章节；标题按「上级 · 本级」拼接，便于在扁平目录中区分层级。
    for (size_t i = 0; i < headings.size(); ++i) {
        if (hasChild[i]) {
            continue;
        }
        std::vector<const std::wstring*> titles;
        int cursor = static_cast<int>(i);
        while (cursor >= 0) {
            titles.push_back(&headings[static_cast<size_t>(cursor)].title);
            cursor = parentOf[static_cast<size_t>(cursor)];
        }
        std::reverse(titles.begin(), titles.end());
        std::wstring joined;
        for (size_t k = 0; k < titles.size(); ++k) {
            if (k > 0) {
                joined += L" · ";
            }
            joined += *titles[k];
        }
        Chapter chapter;
        chapter.title = joined;
        chapter.charOffset = headings[i].offset;
        chapters.push_back(std::move(chapter));
    }
    if (chapters.empty()) {
        return chapters;
    }

    for (size_t i = 0; i < chapters.size(); ++i) {
        const uint32_t end = i + 1 < chapters.size() ? chapters[i + 1].charOffset
                                                     : static_cast<uint32_t>(size);
        chapters[i].charLength =
            end > chapters[i].charOffset ? end - chapters[i].charOffset : 0;
    }
    // 第一个标题之前的内容单独成章，避免这部分正文无法阅读。
    if (chapters.front().charOffset > 0) {
        Chapter preface;
        preface.title = L"正文";
        preface.charOffset = 0;
        preface.charLength = chapters.front().charOffset;
        chapters.insert(chapters.begin(), std::move(preface));
    }
    return chapters;
}

std::vector<Chapter> DetectDefaultChapters(const std::wstring& text) {
    return DetectChapters(text);
}

std::vector<wchar_t> ChapterUnitsForMode(const std::wstring& mode) {
    if (mode == L"hui") {
        return {L'回'};
    }
    if (mode == L"zhang") {
        return {L'章'};
    }
    if (mode == L"jie") {
        return {L'节'};
    }
    if (mode == L"jizhang") {
        return {L'集', L'章'};
    }
    if (mode == L"jizhangjie") {
        return {L'集', L'章', L'节'};
    }
    return {L'章', L'回', L'节', L'卷', L'篇', L'幕'};
}

bool ParseTextFile(const std::filesystem::path& path, Book& book, std::wstring& error) {
    error.clear();
    book = Book{};
    book.formatType = 0;

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = L"无法读取文本文件：" + path.wstring();
        return false;
    }
    file.seekg(0, std::ios::end);
    const std::streamoff length = file.tellg();
    if (length < 0) {
        error = L"无法获取文本文件大小：" + path.wstring();
        return false;
    }
    if (static_cast<uint64_t>(length) > kMaxFileBytes) {
        error = L"文本文件过大，超过 512 MB 限制";
        return false;
    }
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(static_cast<size_t>(length));
    if (!bytes.empty()) {
        file.read(reinterpret_cast<char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        if (!file) {
            error = L"读取文本文件失败：" + path.wstring();
            return false;
        }
    }

    std::wstring content;
    uint32_t codePage = 1200;
    if (!DecodeTextBytes(bytes, content, codePage)) {
        error = L"无法识别文本编码：" + path.wstring();
        return false;
    }
    if (content.empty()) {
        error = L"文本文件内容为空：" + path.wstring();
        return false;
    }

    book.title = path.stem().wstring();
    if (book.title.empty()) {
        book.title = path.filename().wstring();
    }
    book.sourceCodePage = codePage;
    book.content = std::move(content);
    book.chapters = DetectChapters(book.content);
    if (book.chapters.empty()) {
        Chapter whole;
        whole.title = book.title;
        whole.charOffset = 0;
        whole.charLength = static_cast<uint32_t>(book.content.size());
        book.chapters.push_back(std::move(whole));
    }
    return true;
}

bool ParseBookFile(const std::filesystem::path& path, Book& book, std::wstring& error) {
    std::wstring extension = path.extension().wstring();
    for (wchar_t& character : extension) {
        character = static_cast<wchar_t>(std::towlower(character));
    }
    if (extension == L".txt") {
        return ParseTextFile(path, book, error);
    }
    return ParseFile(path, book, error);
}

}  // namespace umd
