#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace web {

// RGBA8 像素缓冲，顶行优先，alpha 为不透明度（非预乘）。
struct ImageData {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> pixels;

    bool Valid() const {
        return width > 0 && height > 0 &&
               pixels.size() == static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
    }
};

// 使用 Windows Imaging Component 解码 PNG/JPEG/BMP/GIF 等常见位图。
bool LoadImageFile(const std::string& path, ImageData* image, std::string* error);

// 从内存中的编码数据解码图像，用于 data: URI 等内联资源。
bool LoadImageMemory(const uint8_t* data, size_t size, ImageData* image, std::string* error);

// 解码 data: URI：支持 base64 载荷与百分号编码的原始字节。
bool LoadImageDataUri(const std::string& uri, ImageData* image, std::string* error);

// 判断字符串是否以 data: 开头（忽略大小写）。
bool IsDataUri(const std::string& source);

// 使用 Windows Imaging Component 写出 PNG，输入为 RGBA8、顶行优先。
bool SavePng(const std::string& path, int width, int height, const uint8_t* rgba_pixels,
             std::string* error);

// 把 RGBA8 位图编码为内存中的 PNG 数据。
bool EncodePngToMemory(int width, int height, const uint8_t* rgba_pixels,
                       std::vector<uint8_t>* png, std::string* error);

// 标准 Base64 编码（含填充字符，不换行）。
std::string Base64Encode(const uint8_t* data, size_t size);

}  // namespace web
