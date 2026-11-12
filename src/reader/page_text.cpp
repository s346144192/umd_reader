#include "reader/page_text.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <thread>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Security.Cryptography.h>
#include <winrt/Windows.Storage.Streams.h>

namespace reader {
namespace {

using winrt::Windows::Globalization::Language;
using winrt::Windows::Graphics::Imaging::BitmapAlphaMode;
using winrt::Windows::Graphics::Imaging::BitmapPixelFormat;
using winrt::Windows::Graphics::Imaging::SoftwareBitmap;
using winrt::Windows::Media::Ocr::OcrEngine;
using winrt::Windows::Media::Ocr::OcrLine;
using winrt::Windows::Media::Ocr::OcrResult;
using winrt::Windows::Media::Ocr::OcrWord;

// 判断字符是否属于中日韩文字或全角符号。此类字符之间不补空格。
bool IsCjkLike(wchar_t character) {
    const uint32_t code = static_cast<uint32_t>(character);
    return (code >= 0x2E80u && code <= 0x9FFFu) || (code >= 0xF900u && code <= 0xFAFFu) ||
           (code >= 0xFE30u && code <= 0xFE4Fu) || (code >= 0xFF00u && code <= 0xFFEFu) ||
           (code >= 0xD840u && code <= 0xD87Fu);
}

// 在 OCR 词框内按墨迹细化字符的水平边界，并把外框裁剪到实际墨迹范围。
// OCR 只给出整词外框，直接按字符数均分会把字距、拼音的窄字母与汉字混为一谈；
// 这里以均分位置为起点，把内部分界挪到附近墨迹最少的列，贴合字符之间的空白。
// 位图缺少足够墨迹时返回 false，调用方退回均分。
bool RefineWordCharRects(const pdf::PageBitmap& bitmap, int wordLeft, int wordTop, int wordRight,
                         int wordBottom, size_t charCount, float scaleX, float scaleY,
                         float pageHeightPt, std::vector<D2D1_RECT_F>& rects) {
    rects.clear();
    if (charCount == 0 || bitmap.pixels.empty()) {
        return false;
    }
    const int imageWidth = bitmap.width;
    const int imageHeight = bitmap.height;
    if (imageWidth <= 0 || imageHeight <= 0) {
        return false;
    }
    const int left = std::clamp(wordLeft, 0, imageWidth - 1);
    const int top = std::clamp(wordTop, 0, imageHeight - 1);
    const int right = std::clamp(wordRight, left + 1, imageWidth);
    const int bottom = std::clamp(wordBottom, top + 1, imageHeight);
    const int cropWidth = right - left;
    const int cropHeight = bottom - top;
    if (cropWidth < static_cast<int>(charCount) || cropHeight < 2) {
        return false;
    }

    // 灰度直方图与 Otsu 阈值：扫描页面的底纹与噪点不会把整块区域都判成墨迹。
    int histogram[256] = {0};
    size_t pixelCount = 0;
    for (int y = top; y < bottom; ++y) {
        const uint8_t* row = bitmap.pixels.data() + static_cast<size_t>(y) * imageWidth * 4;
        for (int x = left; x < right; ++x) {
            const int gray = (row[x * 4 + 2] * 299 + row[x * 4 + 1] * 587 + row[x * 4] * 114) / 1000;
            ++histogram[gray];
            ++pixelCount;
        }
    }
    if (pixelCount == 0) {
        return false;
    }
    int threshold = 128;
    {
        double totalSum = 0.0;
        for (int i = 0; i < 256; ++i) {
            totalSum += static_cast<double>(i) * histogram[i];
        }
        double backgroundSum = 0.0;
        size_t backgroundWeight = 0;
        double bestVariance = -1.0;
        for (int i = 0; i < 256; ++i) {
            backgroundWeight += static_cast<size_t>(histogram[i]);
            if (backgroundWeight == 0) {
                continue;
            }
            const size_t foregroundWeight = pixelCount - backgroundWeight;
            if (foregroundWeight == 0) {
                break;
            }
            backgroundSum += static_cast<double>(i) * histogram[i];
            const double backgroundMean = backgroundSum / static_cast<double>(backgroundWeight);
            const double foregroundMean =
                (totalSum - backgroundSum) / static_cast<double>(foregroundWeight);
            const double difference = backgroundMean - foregroundMean;
            const double variance =
                static_cast<double>(backgroundWeight) * static_cast<double>(foregroundWeight) *
                difference * difference;
            if (variance > bestVariance) {
                bestVariance = variance;
                threshold = i;
            }
        }
    }

    std::vector<int> columnInk(static_cast<size_t>(cropWidth), 0);
    std::vector<int> rowInk(static_cast<size_t>(cropHeight), 0);
    for (int y = 0; y < cropHeight; ++y) {
        const uint8_t* row =
            bitmap.pixels.data() + static_cast<size_t>(top + y) * imageWidth * 4;
        for (int x = 0; x < cropWidth; ++x) {
            const int gray = (row[x * 4 + 2] * 299 + row[x * 4 + 1] * 587 + row[x * 4] * 114) / 1000;
            if (gray <= threshold) {
                ++columnInk[static_cast<size_t>(x)];
                ++rowInk[static_cast<size_t>(y)];
            }
        }
    }

    const int minimumColumnInk = std::max(1, cropHeight / 16);
    int inkLeft = 0;
    while (inkLeft < cropWidth && columnInk[static_cast<size_t>(inkLeft)] < minimumColumnInk) {
        ++inkLeft;
    }
    int inkRight = cropWidth;
    while (inkRight > inkLeft && columnInk[static_cast<size_t>(inkRight - 1)] < minimumColumnInk) {
        --inkRight;
    }
    int inkTop = 0;
    while (inkTop < cropHeight && rowInk[static_cast<size_t>(inkTop)] == 0) {
        ++inkTop;
    }
    int inkBottom = cropHeight;
    while (inkBottom > inkTop && rowInk[static_cast<size_t>(inkBottom - 1)] == 0) {
        --inkBottom;
    }
    if (inkRight - inkLeft < static_cast<int>(charCount) || inkBottom <= inkTop) {
        return false;
    }

    // 外侧边界取整词框，内部边界只做位置微调：这样同一词的相邻字符共用边界，
    // 选区矩形不会在字与字之间断开，同时保留词左右两侧的原始留白。
    std::vector<int> boundaries(charCount + 1, 0);
    boundaries[0] = 0;
    boundaries[charCount] = cropWidth;
    const double cellWidth = static_cast<double>(cropWidth) / static_cast<double>(charCount);
    const int searchRange = std::max(1, static_cast<int>(cellWidth * 0.35));
    for (size_t i = 1; i < charCount; ++i) {
        const int nominal = static_cast<int>(std::lround(cellWidth * static_cast<double>(i)));
        const int minimum = boundaries[i - 1] + 1;
        const int maximum = cropWidth - static_cast<int>(charCount - i);
        if (minimum > maximum) {
            return false;
        }
        int best = std::clamp(nominal, minimum, maximum);
        int bestInk = std::numeric_limits<int>::max();
        const int searchBegin = std::max(minimum, nominal - searchRange);
        const int searchEnd = std::min(maximum, nominal + searchRange);
        for (int candidate = searchBegin; candidate <= searchEnd; ++candidate) {
            const int ink = columnInk[static_cast<size_t>(candidate)];
            if (ink < bestInk ||
                (ink == bestInk && std::abs(candidate - nominal) < std::abs(best - nominal))) {
                bestInk = ink;
                best = candidate;
            }
        }
        boundaries[i] = best;
    }

    rects.reserve(charCount);
    for (size_t i = 0; i < charCount; ++i) {
        D2D1_RECT_F rect{};
        rect.left = static_cast<float>(left + boundaries[i]) / scaleX;
        rect.right = static_cast<float>(left + boundaries[i + 1]) / scaleX;
        rect.top = pageHeightPt - static_cast<float>(top + inkTop) / scaleY;
        rect.bottom = pageHeightPt - static_cast<float>(top + inkBottom) / scaleY;
        rects.push_back(rect);
    }
    return true;
}

// 选择识别语言：优先简体中文，其次繁体中文，最后取系统第一个可用语言。
Language ChooseLanguage() {
    const auto languages = OcrEngine::AvailableRecognizerLanguages();
    Language simplified{nullptr};
    Language traditional{nullptr};
    for (const Language& language : languages) {
        const std::wstring tag = language.LanguageTag().c_str();
        if (tag == L"zh-Hans" || tag.rfind(L"zh-Hans-", 0) == 0) {
            if (!simplified) {
                simplified = language;
            }
        } else if (tag == L"zh-Hant" || tag.rfind(L"zh-Hant-", 0) == 0) {
            if (!traditional) {
                traditional = language;
            }
        }
    }
    if (simplified) {
        return simplified;
    }
    if (traditional) {
        return traditional;
    }
    return languages.Size() > 0 ? languages.GetAt(0) : Language{nullptr};
}

// 在工作线程中执行识别：C++/WinRT 的异步等待在 MTA 线程上不会与界面线程互相等待。
bool RecognizeOnWorker(const pdf::PageBitmap& bitmap, float pageWidthPt, float pageHeightPt,
                       PageTextLayer& layer, std::wstring& error) {
    // 新线程未初始化 COM，先进入多线程单元；已经初始化则忽略。
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (const winrt::hresult_error&) {
    }

    try {
        const Language language = ChooseLanguage();
        if (!language) {
            error = L"系统未安装可用的 OCR 识别语言";
            return false;
        }
        OcrEngine engine = OcrEngine::TryCreateFromLanguage(language);
        if (!engine) {
            engine = OcrEngine::TryCreateFromUserProfileLanguages();
        }
        if (!engine) {
            error = L"无法创建系统 OCR 引擎，请在系统设置中安装识别语言";
            return false;
        }

        const auto bytes = winrt::array_view<const uint8_t>(
            bitmap.pixels.data(), bitmap.pixels.data() + bitmap.pixels.size());
        const auto buffer =
            winrt::Windows::Security::Cryptography::CryptographicBuffer::CreateFromByteArray(bytes);
        const SoftwareBitmap software = SoftwareBitmap::CreateCopyFromBuffer(
            buffer, BitmapPixelFormat::Bgra8, bitmap.width, bitmap.height,
            BitmapAlphaMode::Ignore);
        const OcrResult result = engine.RecognizeAsync(software).get();

        const float scaleX = static_cast<float>(bitmap.width) / std::max(pageWidthPt, 1.0f);
        const float scaleY = static_cast<float>(bitmap.height) / std::max(pageHeightPt, 1.0f);
        layer.lines.clear();
        layer.fromOcr = true;

        // 先把所有词换算为页面坐标。系统 OCR 在分栏、表格等复杂排版中
        // 会把左右两栏的文字归入同一个 OcrLine，需要按分栏边界重新拆行。
        struct CollectedWord {
            int lineIndex = 0;
            std::wstring text;
            float leftPt = 0.0f;
            float rightPt = 0.0f;
            float topPt = 0.0f;
            float bottomPt = 0.0f;
            // 位图上的像素范围，用于按墨迹细化逐字外框。
            int pixelLeft = 0;
            int pixelTop = 0;
            int pixelRight = 0;
            int pixelBottom = 0;
        };
        std::vector<CollectedWord> words;
        int sourceLineIndex = 0;
        for (const OcrLine& ocrLine : result.Lines()) {
            for (const OcrWord& word : ocrLine.Words()) {
                const std::wstring text = word.Text().c_str();
                if (text.empty()) {
                    continue;
                }
                const auto rect = word.BoundingRect();
                CollectedWord item;
                item.lineIndex = sourceLineIndex;
                item.text = text;
                item.leftPt = rect.X / scaleX;
                item.rightPt = item.leftPt + rect.Width / scaleX;
                item.topPt = pageHeightPt - rect.Y / scaleY;
                item.bottomPt = pageHeightPt - (rect.Y + rect.Height) / scaleY;
                item.pixelLeft = static_cast<int>(std::floor(rect.X));
                item.pixelTop = static_cast<int>(std::floor(rect.Y));
                item.pixelRight = static_cast<int>(std::ceil(rect.X + rect.Width));
                item.pixelBottom = static_cast<int>(std::ceil(rect.Y + rect.Height));
                words.push_back(std::move(item));
            }
            ++sourceLineIndex;
        }

        // 统计水平方向的词覆盖次数。贯通全页的分栏空白几乎没有词覆盖，
        // 而普通词间空格会被其他行的文字覆盖，据此区分分栏边界与普通空格。
        std::vector<std::pair<float, float>> gutters;
        const int binCount = std::max(1, static_cast<int>(std::ceil(pageWidthPt)));
        std::vector<int> coverage(static_cast<size_t>(binCount), 0);
        for (const CollectedWord& word : words) {
            const int begin =
                std::clamp(static_cast<int>(std::floor(word.leftPt)), 0, binCount - 1);
            const int end =
                std::clamp(static_cast<int>(std::ceil(word.rightPt)), begin + 1, binCount);
            for (int bin = begin; bin < end; ++bin) {
                ++coverage[static_cast<size_t>(bin)];
            }
        }
        int maxCoverage = 0;
        for (const int count : coverage) {
            maxCoverage = std::max(maxCoverage, count);
        }
        const int gutterLimit = std::max(1, maxCoverage / 12);
        int lowRunStart = -1;
        for (int bin = 0; bin <= binCount; ++bin) {
            const bool low = bin < binCount && coverage[static_cast<size_t>(bin)] <= gutterLimit;
            if (low) {
                if (lowRunStart < 0) {
                    lowRunStart = bin;
                }
            } else if (lowRunStart >= 0) {
                const float gutterLeft = static_cast<float>(lowRunStart);
                const float gutterRight = static_cast<float>(bin);
                // 过窄的空白视为普通空格；贴边的空白不作为分栏边界。
                if (gutterRight - gutterLeft >= 4.0f &&
                    gutterLeft >= pageWidthPt * 0.08f &&
                    gutterRight <= pageWidthPt * 0.92f) {
                    gutters.emplace_back(gutterLeft, gutterRight);
                }
                lowRunStart = -1;
            }
        }
        const auto crossesGutter = [&gutters](float leftPt, float rightPt) {
            for (const auto& gutter : gutters) {
                if (leftPt < gutter.first && rightPt > gutter.second) {
                    return true;
                }
            }
            return false;
        };

        TextLayerLine line;
        int currentLineIndex = -1;
        float previousRightPt = 0.0f;
        bool hasPrevious = false;
        const auto flushLine = [&layer, &line]() {
            if (!line.text.empty()) {
                layer.lines.push_back(std::move(line));
                line = TextLayerLine{};
            }
        };
        for (const CollectedWord& word : words) {
            if (word.lineIndex != currentLineIndex) {
                flushLine();
                currentLineIndex = word.lineIndex;
                hasPrevious = false;
            }
            // 相邻词跨越分栏空白时拆成独立行，避免同一行的选中矩形横跨两栏；
            // 词序明显回退时同样拆行，处理 OCR 输出顺序错乱的情况。
            if (hasPrevious &&
                (crossesGutter(previousRightPt, word.leftPt) ||
                 word.leftPt < previousRightPt - 1.0f)) {
                flushLine();
            }
            const bool needsSpace = !line.text.empty() && !IsCjkLike(line.text.back()) &&
                                    !IsCjkLike(word.text.front());
            if (needsSpace) {
                // 西文词之间补一个空格，外框为零宽，位于两词交界处。
                D2D1_RECT_F space{};
                space.left = line.charRects.back().right;
                space.right = space.left;
                space.top = line.charRects.back().top;
                space.bottom = line.charRects.back().bottom;
                line.text.push_back(L' ');
                line.charRects.push_back(space);
            }
            // 优先按页面墨迹细化逐字外框；无法判断墨迹时退回按字符数均分。
            std::vector<D2D1_RECT_F> refinedRects;
            const bool refined =
                RefineWordCharRects(bitmap, word.pixelLeft, word.pixelTop, word.pixelRight,
                                    word.pixelBottom, word.text.size(), scaleX, scaleY,
                                    pageHeightPt, refinedRects);
            if (refined) {
                for (size_t i = 0; i < word.text.size(); ++i) {
                    line.text.push_back(word.text[i]);
                    line.charRects.push_back(refinedRects[i]);
                }
            } else {
                const float step =
                    (word.rightPt - word.leftPt) / static_cast<float>(word.text.size());
                for (size_t i = 0; i < word.text.size(); ++i) {
                    D2D1_RECT_F charRect{};
                    charRect.left = word.leftPt + step * static_cast<float>(i);
                    charRect.right = charRect.left + step;
                    charRect.top = word.topPt;
                    charRect.bottom = word.bottomPt;
                    line.text.push_back(word.text[i]);
                    line.charRects.push_back(charRect);
                }
            }
            previousRightPt = word.rightPt;
            hasPrevious = true;
        }
        flushLine();

        // 检测到单一分栏空白时按列优先重排行序：先读完左栏，再读右栏。
        // 否则按行交叉的顺序会让跨行拖选同时命中两栏，选中的文字与高亮都会错位。
        if (gutters.size() == 1) {
            const auto& gutter = gutters.front();
            std::vector<size_t> leftLines;
            std::vector<size_t> rightLines;
            std::vector<size_t> spanningLines;
            for (size_t i = 0; i < layer.lines.size(); ++i) {
                float lineLeft = std::numeric_limits<float>::max();
                float lineRight = std::numeric_limits<float>::lowest();
                for (const D2D1_RECT_F& rect : layer.lines[i].charRects) {
                    lineLeft = std::min(lineLeft, rect.left);
                    lineRight = std::max(lineRight, rect.right);
                }
                if (lineRight <= gutter.first) {
                    leftLines.push_back(i);
                } else if (lineLeft >= gutter.second) {
                    rightLines.push_back(i);
                } else {
                    spanningLines.push_back(i);
                }
            }
            const size_t columnLines = leftLines.size() + rightLines.size();
            // 只有两栏都包含足够多的行时才按列重排，避免单栏页面被误判打乱顺序。
            if (columnLines > 0 && leftLines.size() * 4 >= columnLines &&
                rightLines.size() * 4 >= columnLines) {
                std::vector<TextLayerLine> ordered;
                ordered.reserve(layer.lines.size());
                const auto appendLines = [&layer, &ordered](const std::vector<size_t>& indices) {
                    for (const size_t index : indices) {
                        ordered.push_back(std::move(layer.lines[index]));
                    }
                };
                appendLines(spanningLines);
                appendLines(leftLines);
                appendLines(rightLines);
                layer.lines = std::move(ordered);
            }
        }
        if (layer.CharacterCount() == 0) {
            error = L"本页没有识别出文字";
            return false;
        }
        return true;
    } catch (const winrt::hresult_error& exception) {
        error = L"系统 OCR 识别失败，错误码 " +
                std::to_wstring(static_cast<long>(exception.code().value));
        return false;
    } catch (...) {
        error = L"系统 OCR 识别失败";
        return false;
    }
}

}  // namespace

size_t PageTextLayer::CharacterCount() const {
    size_t count = 0;
    for (const TextLayerLine& line : lines) {
        for (const wchar_t character : line.text) {
            if (character != L' ' && character != L'\t' && character != 0x3000) {
                ++count;
            }
        }
    }
    return count;
}

bool RecognizePageText(const pdf::PageBitmap& bitmap, float pageWidthPt, float pageHeightPt,
                       PageTextLayer& layer, std::wstring& error) {
    layer = PageTextLayer{};
    error.clear();
    if (bitmap.width <= 0 || bitmap.height <= 0 || bitmap.pixels.empty()) {
        error = L"页面位图为空，无法识别文字";
        return false;
    }
    bool succeeded = false;
    std::wstring message;
    std::thread worker([&]() {
        succeeded = RecognizeOnWorker(bitmap, pageWidthPt, pageHeightPt, layer, message);
    });
    worker.join();
    error = message;
    return succeeded;
}

}  // namespace reader
