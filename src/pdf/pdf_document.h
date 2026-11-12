#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// PDF 文档层：封装 PDFium 句柄、逐页文本层统计、文本行提取、大纲与位图渲染。
// 本层不依赖界面，也不参与 UMD 数据模型；阅读界面通过 reader 子系统消费其结果。
// PDFium 的文档句柄不是线程安全的，同一句柄同一时间只允许一个线程使用，
// 因此本类全部公开方法都在内部互斥量下串行执行。
namespace pdf {

// 页面信息。宽高单位为 PDF point（1 point = 1/72 英寸），与 RenderPage 输出位图的比例一致。
struct PageInfo {
    float widthPt = 0.0f;
    float heightPt = 0.0f;
    // 以下各项在 AnalyzePage 或 ReadPageLines 之后有效。
    bool analyzed = false;
    uint32_t textCharCount = 0;     // 有效文本字符数：非空白、非零、非私用区
    uint32_t privateUseCharCount = 0;  // 私用区字符数：U+E000 至 U+F8FF
    // 页面大部分区域由图像覆盖，通常是扫描页。此类页面的文字层来自
    // 扫描软件的识别结果，可能缺少拼音或与图像错位，选择文字时应改用系统 OCR。
    bool hasFullPageImage = false;
    // 文字层中成串的 ASCII 符号比例异常偏高，通常表示字体缺少正确的 Unicode 映射，
    // 拼音等西文字符被解析成符号。此类页面同样应改用系统 OCR。
    bool textLayerSuspicious = false;
};

// 大纲条目。positionY 为页面内纵向位置，单位 point，原点在页面左下角。
struct OutlineEntry {
    std::wstring title;
    int level = 1;          // 从 1 开始
    int pageIndex = -1;     // 0 起页序号；-1 表示该条目没有可定位的目标页
    bool hasPosition = false;
    float positionY = 0.0f;
};

// 单个字符的外框，坐标与 TextLine 相同（原点在页面左下角，单位 point）。
struct CharBox {
    float left = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
    float top = 0.0f;
};

// 一页中同一基线上的文本行，坐标为 PDF 用户空间（原点在页面左下角）。
struct TextLine {
    std::wstring text;
    // 与 text 等长的逐字外框。文本流中的空白字符外框宽度为 0，
    // 位置取相邻字符交界处，供选择与复制使用。
    std::vector<CharBox> boxes;
    float left = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
    float top = 0.0f;
    float fontSize = 0.0f;
};

// 页面位图，像素格式为 BGRA，每行从上到下，Alpha 固定为不透明。
struct PageBitmap {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> pixels;
};

using PageBitmapPtr = std::shared_ptr<const PageBitmap>;

class Document {
public:
    Document();
    ~Document();

    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;

    // 打开 PDF 文件。路径支持任意 Unicode 字符，文件按需分块读取，
    // 不把整份文件内容常驻内存。
    bool Open(const std::filesystem::path& path, std::wstring& error);
    void Close();

    bool IsOpen() const { return document_ != nullptr; }
    int PageCount() const { return pageCount_; }
    const std::vector<PageInfo>& Pages() const { return pages_; }
    const std::wstring& Title() const { return title_; }
    const std::wstring& Author() const { return author_; }
    const std::vector<OutlineEntry>& Outline() const { return outline_; }

    // 统计单页文本层字符数。页信息中的 analyzed 置位后即可通过 PageAt 读取。
    bool AnalyzePage(int pageIndex, std::wstring& error);
    // 统计全部页面文本层。耗时与页数成正比，供导入阶段一次性判定。
    bool AnalyzeAllPages(std::wstring& error);

    // 提取单页文本行，同时更新该页文本层统计。lines 按 PDF 文本流顺序排列，
    // 尚未做分栏重排；分栏与页眉页脚处理由重排层负责。
    bool ReadPageLines(int pageIndex, std::vector<TextLine>& lines, std::wstring& error);

    // 渲染页面为 BGRA 位图。scale 为每 point 对应的像素数。
    // 返回的位图由文档层缓存持有，内容不可修改；缓存只保留最近一页。
    bool RenderPage(int pageIndex, float scale, PageBitmapPtr& bitmap, std::wstring& error);

    void ClearCache();

private:
    // 自定义文件读取源：PDFium 通过回调按块读取，文件句柄由本类持有。
    struct FileSource;

    struct ExtractResult {
        std::vector<TextLine> lines;
        uint32_t textCharCount = 0;
        uint32_t privateUseCharCount = 0;
    };

    void CloseInternal();
    void LoadMetadata();
    void LoadOutline();
    void WalkOutline(void* parent, int level, int depth, std::vector<void*>& visited);
    bool ExtractPage(int pageIndex, ExtractResult& result, std::wstring& error);

    static std::wstring DescribeError(int code);
    static std::wstring MetaText(void* document, const char* tag);

    void* document_ = nullptr;  // FPDF_DOCUMENT
    std::unique_ptr<FileSource> fileSource_;  // 打开期间必须保持有效的读取源
    int pageCount_ = 0;
    std::vector<PageInfo> pages_;
    std::vector<OutlineEntry> outline_;
    std::wstring title_;
    std::wstring author_;

    mutable std::mutex mutex_;
    PageBitmapPtr cachedBitmap_;
    int cachedPage_ = -1;
    float cachedScale_ = 0.0f;
};

}  // namespace pdf
