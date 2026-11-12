#include "web/image.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace web {

namespace {

using Microsoft::WRL::ComPtr;

// 记录失败阶段与 HRESULT 码，便于调用方定位问题。
std::string DescribeFailure(const char* stage, HRESULT result) {
    char buffer[192];
    std::snprintf(buffer, sizeof(buffer), "%s（HRESULT 0x%08X）", stage,
                  static_cast<unsigned int>(result));
    return std::string(buffer);
}

void SetError(std::string* error, const std::string& message) {
    if (error != nullptr) {
        *error = message;
    }
}

std::string LowerAscii(std::string text) {
    for (char& character : text) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return text;
}

int HexValue(char character) {
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }
    return -1;
}

int Base64Value(char character) {
    if (character >= 'A' && character <= 'Z') {
        return character - 'A';
    }
    if (character >= 'a' && character <= 'z') {
        return character - 'a' + 26;
    }
    if (character >= '0' && character <= '9') {
        return character - '0' + 52;
    }
    if (character == '+') {
        return 62;
    }
    if (character == '/') {
        return 63;
    }
    return -1;
}

// 标准 Base64 解码：忽略空白与换行，遇到填充字符结束。
bool Base64Decode(const std::string& text, std::vector<uint8_t>* output) {
    if (output == nullptr) {
        return false;
    }
    output->clear();
    int buffer = 0;
    int bits = 0;
    for (char character : text) {
        if (character == '=') {
            break;
        }
        if (character == '\r' || character == '\n' || character == ' ' || character == '\t') {
            continue;
        }
        const int value = Base64Value(character);
        if (value < 0) {
            return false;
        }
        buffer = (buffer << 6) | value;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            output->push_back(static_cast<uint8_t>((buffer >> bits) & 0xFF));
        }
    }
    return true;
}

// 百分号编码解码；非法转义按原字符保留。
void PercentDecode(const std::string& text, std::vector<uint8_t>* output) {
    output->clear();
    output->reserve(text.size());
    for (size_t index = 0; index < text.size(); ++index) {
        if (text[index] == '%' && index + 2 < text.size()) {
            const int high = HexValue(text[index + 1]);
            const int low = HexValue(text[index + 2]);
            if (high >= 0 && low >= 0) {
                output->push_back(static_cast<uint8_t>(high * 16 + low));
                index += 2;
                continue;
            }
        }
        output->push_back(static_cast<uint8_t>(text[index]));
    }
}

// 将 UTF-8 路径转换为宽字符；非法编码返回 false。
bool Utf8PathToWide(const std::string& path, std::wstring* wide_path, std::string* error) {
    if (wide_path == nullptr) {
        SetError(error, "路径输出参数为空");
        return false;
    }
    wide_path->clear();
    if (path.empty()) {
        SetError(error, "路径为空");
        return false;
    }

    const int byte_count = static_cast<int>(path.size());
    const int wide_length =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.c_str(), byte_count, nullptr, 0);
    if (wide_length <= 0) {
        SetError(error, "路径不是合法的 UTF-8 编码");
        return false;
    }

    wide_path->assign(static_cast<size_t>(wide_length), L'\0');
    const int converted = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.c_str(),
                                              byte_count, wide_path->data(), wide_length);
    if (converted <= 0) {
        wide_path->clear();
        SetError(error, "无法把路径转换为宽字符");
        return false;
    }
    return true;
}

// 创建 WIC 工厂；COM 由调用方初始化，此处不重复调用 CoInitializeEx。
bool CreateWicFactory(ComPtr<IWICImagingFactory>* factory, std::string* error) {
    if (factory == nullptr) {
        SetError(error, "WIC 工厂输出参数为空");
        return false;
    }
    HRESULT result = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(factory->ReleaseAndGetAddressOf()));
    if (FAILED(result) || !*factory) {
        factory->Reset();
        SetError(error, DescribeFailure("无法创建 WIC 工厂", result));
        return false;
    }
    return true;
}

// 把解码器首帧转换为 RGBA8 顶行优先像素。
bool DecodeBitmapFrame(IWICImagingFactory* factory, IWICBitmapDecoder* decoder,
                       ImageData* image, std::string* error) {
    ComPtr<IWICBitmapFrameDecode> frame;
    HRESULT result = decoder->GetFrame(0, frame.GetAddressOf());
    if (FAILED(result) || !frame) {
        SetError(error, DescribeFailure("无法读取图像帧", result));
        return false;
    }

    // 统一转换为 32bpp RGBA8（非预乘 alpha、顶行优先）。
    ComPtr<IWICFormatConverter> converter;
    result = factory->CreateFormatConverter(converter.GetAddressOf());
    if (FAILED(result) || !converter) {
        SetError(error, DescribeFailure("无法创建像素格式转换器", result));
        return false;
    }

    result = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
                                   WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeCustom);
    if (FAILED(result)) {
        SetError(error, DescribeFailure("无法转换图像像素格式", result));
        return false;
    }

    UINT width = 0;
    UINT height = 0;
    result = converter->GetSize(&width, &height);
    if (FAILED(result) || width == 0 || height == 0) {
        SetError(error, DescribeFailure("无法读取图像尺寸", result));
        return false;
    }
    if (width > static_cast<UINT>(std::numeric_limits<int>::max()) ||
        height > static_cast<UINT>(std::numeric_limits<int>::max())) {
        SetError(error, "图像尺寸超出可支持范围");
        return false;
    }

    const uint64_t stride_bytes = static_cast<uint64_t>(width) * 4u;
    const uint64_t total_bytes = stride_bytes * static_cast<uint64_t>(height);
    if (stride_bytes > std::numeric_limits<UINT>::max() ||
        total_bytes > std::numeric_limits<UINT>::max()) {
        SetError(error, "图像像素数据超出可支持范围");
        return false;
    }

    image->width = static_cast<int>(width);
    image->height = static_cast<int>(height);
    image->pixels.assign(static_cast<size_t>(total_bytes), 0);

    result = converter->CopyPixels(nullptr, static_cast<UINT>(stride_bytes),
                                   static_cast<UINT>(total_bytes), image->pixels.data());
    if (FAILED(result)) {
        *image = ImageData();
        SetError(error, DescribeFailure("无法读取图像像素", result));
        return false;
    }
    return true;
}

// 把 PNG 编码到已初始化的 WIC 流。
bool EncodePngToStream(IWICImagingFactory* factory, IWICStream* stream, int width, int height,
                       const uint8_t* rgba_pixels, std::string* error) {
    ComPtr<IWICBitmapEncoder> encoder;
    HRESULT result = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr,
                                            encoder.GetAddressOf());
    if (FAILED(result) || !encoder) {
        SetError(error, DescribeFailure("无法创建 PNG 编码器", result));
        return false;
    }

    result = encoder->Initialize(stream, WICBitmapEncoderNoCache);
    if (FAILED(result)) {
        SetError(error, DescribeFailure("PNG 编码器初始化失败", result));
        return false;
    }

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> frame_options;
    result = encoder->CreateNewFrame(frame.GetAddressOf(), frame_options.GetAddressOf());
    if (FAILED(result) || !frame) {
        SetError(error, DescribeFailure("无法创建 PNG 帧", result));
        return false;
    }

    result = frame->Initialize(frame_options.Get());
    if (FAILED(result)) {
        SetError(error, DescribeFailure("PNG 帧初始化失败", result));
        return false;
    }

    result = frame->SetSize(static_cast<UINT>(width), static_cast<UINT>(height));
    if (FAILED(result)) {
        SetError(error, DescribeFailure("无法设置 PNG 尺寸", result));
        return false;
    }

    GUID pixel_format = GUID_WICPixelFormat32bppRGBA;
    result = frame->SetPixelFormat(&pixel_format);
    if (FAILED(result)) {
        SetError(error, DescribeFailure("无法设置 PNG 像素格式", result));
        return false;
    }

    const uint64_t stride_bytes = static_cast<uint64_t>(width) * 4u;
    const uint64_t total_bytes = stride_bytes * static_cast<uint64_t>(height);
    if (IsEqualGUID(pixel_format, GUID_WICPixelFormat32bppRGBA)) {
        result = frame->WritePixels(static_cast<UINT>(height), static_cast<UINT>(stride_bytes),
                                    static_cast<UINT>(total_bytes),
                                    const_cast<BYTE*>(rgba_pixels));
        if (FAILED(result)) {
            SetError(error, DescribeFailure("无法写入 PNG 像素", result));
            return false;
        }
    } else {
        // WIC 的 PNG 编码器通常只接受 BGRA 系列格式，先把 RGBA8 内存位图转换为
        // 编码器选定的格式，再由帧编码器写入。
        ComPtr<IWICBitmap> source_bitmap;
        result = factory->CreateBitmapFromMemory(
            static_cast<UINT>(width), static_cast<UINT>(height), GUID_WICPixelFormat32bppRGBA,
            static_cast<UINT>(stride_bytes), static_cast<UINT>(total_bytes),
            const_cast<BYTE*>(rgba_pixels), source_bitmap.GetAddressOf());
        if (FAILED(result) || !source_bitmap) {
            SetError(error, DescribeFailure("无法创建 PNG 源位图", result));
            return false;
        }

        ComPtr<IWICFormatConverter> converter;
        result = factory->CreateFormatConverter(converter.GetAddressOf());
        if (FAILED(result) || !converter) {
            SetError(error, DescribeFailure("无法创建 PNG 像素格式转换器", result));
            return false;
        }

        result = converter->Initialize(source_bitmap.Get(), pixel_format,
                                       WICBitmapDitherTypeNone, nullptr, 0.0,
                                       WICBitmapPaletteTypeCustom);
        if (FAILED(result)) {
            SetError(error, DescribeFailure("无法转换 PNG 像素格式", result));
            return false;
        }

        result = frame->WriteSource(converter.Get(), nullptr);
        if (FAILED(result)) {
            SetError(error, DescribeFailure("无法写入 PNG 像素", result));
            return false;
        }
    }

    result = frame->Commit();
    if (FAILED(result)) {
        SetError(error, DescribeFailure("无法提交 PNG 帧", result));
        return false;
    }

    result = encoder->Commit();
    if (FAILED(result)) {
        SetError(error, DescribeFailure("无法提交 PNG 编码器", result));
        return false;
    }
    return true;
}

}  // namespace

bool LoadImageFile(const std::string& path, ImageData* image, std::string* error) {
    if (image == nullptr) {
        SetError(error, "图像输出参数为空");
        return false;
    }
    *image = ImageData();

    std::wstring wide_path;
    if (!Utf8PathToWide(path, &wide_path, error)) {
        return false;
    }

    ComPtr<IWICImagingFactory> factory;
    if (!CreateWicFactory(&factory, error)) {
        return false;
    }

    ComPtr<IWICBitmapDecoder> decoder;
    HRESULT result =
        factory->CreateDecoderFromFilename(wide_path.c_str(), nullptr, GENERIC_READ,
                                           WICDecodeMetadataCacheOnDemand, decoder.GetAddressOf());
    if (FAILED(result) || !decoder) {
        SetError(error, DescribeFailure("无法打开图像文件", result));
        return false;
    }

    return DecodeBitmapFrame(factory.Get(), decoder.Get(), image, error);
}

bool IsDataUri(const std::string& source) {
    return source.size() >= 5 && LowerAscii(source.substr(0, 5)) == "data:";
}

bool LoadImageMemory(const uint8_t* data, size_t size, ImageData* image, std::string* error) {
    if (image == nullptr) {
        SetError(error, "图像输出参数为空");
        return false;
    }
    *image = ImageData();
    if (data == nullptr || size == 0) {
        SetError(error, "图像数据为空");
        return false;
    }

    ComPtr<IWICImagingFactory> factory;
    if (!CreateWicFactory(&factory, error)) {
        return false;
    }

    ComPtr<IWICStream> stream;
    HRESULT result = factory->CreateStream(stream.GetAddressOf());
    if (FAILED(result) || !stream) {
        SetError(error, DescribeFailure("无法创建图像内存流", result));
        return false;
    }
    result = stream->InitializeFromMemory(const_cast<BYTE*>(data), static_cast<DWORD>(size));
    if (FAILED(result)) {
        SetError(error, DescribeFailure("图像内存流初始化失败", result));
        return false;
    }

    ComPtr<IWICBitmapDecoder> decoder;
    result = factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand,
                                              decoder.GetAddressOf());
    if (FAILED(result) || !decoder) {
        SetError(error, DescribeFailure("无法解码内存图像", result));
        return false;
    }
    return DecodeBitmapFrame(factory.Get(), decoder.Get(), image, error);
}

bool LoadImageDataUri(const std::string& uri, ImageData* image, std::string* error) {
    if (!IsDataUri(uri)) {
        SetError(error, "不是 data: URI");
        return false;
    }
    const size_t comma = uri.find(',');
    if (comma == std::string::npos) {
        SetError(error, "data: URI 缺少数据部分");
        return false;
    }
    const std::string header = uri.substr(5, comma - 5);
    const std::string payload = uri.substr(comma + 1);

    bool base64 = false;
    size_t start = 0;
    while (start <= header.size()) {
        const size_t end = header.find(';', start);
        const std::string part =
            header.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (LowerAscii(part) == "base64") {
            base64 = true;
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }

    std::vector<uint8_t> bytes;
    if (base64) {
        if (!Base64Decode(payload, &bytes)) {
            SetError(error, "data: URI 的 Base64 载荷无效");
            return false;
        }
    } else {
        PercentDecode(payload, &bytes);
    }
    return LoadImageMemory(bytes.data(), bytes.size(), image, error);
}

bool SavePng(const std::string& path, int width, int height, const uint8_t* rgba_pixels,
             std::string* error) {
    if (width <= 0 || height <= 0 || rgba_pixels == nullptr) {
        SetError(error, "PNG 输出参数无效");
        return false;
    }

    const uint64_t stride_bytes = static_cast<uint64_t>(width) * 4u;
    const uint64_t total_bytes = stride_bytes * static_cast<uint64_t>(height);
    if (stride_bytes > std::numeric_limits<UINT>::max() ||
        total_bytes > std::numeric_limits<UINT>::max()) {
        SetError(error, "PNG 像素数据超出可支持范围");
        return false;
    }

    std::wstring wide_path;
    if (!Utf8PathToWide(path, &wide_path, error)) {
        return false;
    }

    ComPtr<IWICImagingFactory> factory;
    if (!CreateWicFactory(&factory, error)) {
        return false;
    }

    ComPtr<IWICStream> stream;
    HRESULT result = factory->CreateStream(stream.GetAddressOf());
    if (FAILED(result) || !stream) {
        SetError(error, DescribeFailure("无法创建 WIC 流", result));
        return false;
    }

    result = stream->InitializeFromFilename(wide_path.c_str(), GENERIC_WRITE);
    if (FAILED(result)) {
        SetError(error, DescribeFailure("无法打开 PNG 输出文件", result));
        return false;
    }

    return EncodePngToStream(factory.Get(), stream.Get(), width, height, rgba_pixels, error);
}

bool EncodePngToMemory(int width, int height, const uint8_t* rgba_pixels,
                       std::vector<uint8_t>* png, std::string* error) {
    if (png == nullptr) {
        SetError(error, "PNG 输出参数为空");
        return false;
    }
    png->clear();
    if (width <= 0 || height <= 0 || rgba_pixels == nullptr) {
        SetError(error, "PNG 输出参数无效");
        return false;
    }

    ComPtr<IWICImagingFactory> factory;
    if (!CreateWicFactory(&factory, error)) {
        return false;
    }

    // 以 HGLOBAL 支撑的 IStream 作为编码目标，提交后一次性读回全部字节。
    ComPtr<IStream> memory_stream;
    HRESULT result = CreateStreamOnHGlobal(nullptr, TRUE, memory_stream.GetAddressOf());
    if (FAILED(result) || !memory_stream) {
        SetError(error, DescribeFailure("无法创建 PNG 内存流", result));
        return false;
    }

    ComPtr<IWICStream> stream;
    result = factory->CreateStream(stream.GetAddressOf());
    if (FAILED(result) || !stream) {
        SetError(error, DescribeFailure("无法创建 WIC 流", result));
        return false;
    }
    result = stream->InitializeFromIStream(memory_stream.Get());
    if (FAILED(result)) {
        SetError(error, DescribeFailure("WIC 流初始化失败", result));
        return false;
    }

    if (!EncodePngToStream(factory.Get(), stream.Get(), width, height, rgba_pixels, error)) {
        return false;
    }

    STATSTG stat{};
    result = memory_stream->Stat(&stat, STATFLAG_NONAME);
    if (FAILED(result)) {
        SetError(error, DescribeFailure("无法读取 PNG 数据长度", result));
        return false;
    }
    const ULONGLONG byte_count = stat.cbSize.QuadPart;
    if (byte_count == 0 ||
        byte_count > static_cast<ULONGLONG>(std::numeric_limits<ULONG>::max())) {
        SetError(error, "PNG 数据长度超出可支持范围");
        return false;
    }

    png->assign(static_cast<size_t>(byte_count), 0);
    LARGE_INTEGER offset{};
    result = memory_stream->Seek(offset, STREAM_SEEK_SET, nullptr);
    if (FAILED(result)) {
        png->clear();
        SetError(error, DescribeFailure("无法定位 PNG 数据起点", result));
        return false;
    }
    ULONG received = 0;
    result = memory_stream->Read(png->data(), static_cast<ULONG>(byte_count), &received);
    if (FAILED(result) || received != static_cast<ULONG>(byte_count)) {
        png->clear();
        SetError(error, DescribeFailure("无法读取 PNG 数据", result));
        return false;
    }
    return true;
}

std::string Base64Encode(const uint8_t* data, size_t size) {
    static const char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    if (data == nullptr || size == 0) {
        return result;
    }
    result.reserve((size + 2) / 3 * 4);
    for (size_t index = 0; index < size; index += 3) {
        const uint32_t first = data[index];
        const uint32_t second = index + 1 < size ? data[index + 1] : 0u;
        const uint32_t third = index + 2 < size ? data[index + 2] : 0u;
        const uint32_t combined = (first << 16) | (second << 8) | third;
        result.push_back(kAlphabet[(combined >> 18) & 0x3F]);
        result.push_back(kAlphabet[(combined >> 12) & 0x3F]);
        result.push_back(index + 1 < size ? kAlphabet[(combined >> 6) & 0x3F] : '=');
        result.push_back(index + 2 < size ? kAlphabet[combined & 0x3F] : '=');
    }
    return result;
}

}  // namespace web
