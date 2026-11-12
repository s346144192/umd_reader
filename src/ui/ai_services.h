#pragma once

#include <string>

#include "data/library.h"

namespace ui {
namespace detail {

// 用 OpenAI 兼容 /audio/speech 合成语音，返回 WAV 临时文件路径。
bool SynthesizeSpeech(const data::AiServiceConfig& config, const std::wstring& text,
                      std::wstring& outputPath, std::wstring& error);

// 用 OpenAI 兼容 /images/generations 生成图片，返回保存后的文件路径。
bool GenerateImage(const data::AiServiceConfig& config, const std::wstring& prompt,
                   const std::wstring& outputDirectory, std::wstring& outputPath,
                   std::wstring& error);

// 用 OpenAI 兼容 /videos 异步任务生成视频，返回保存后的文件路径。
bool GenerateVideo(const data::AiServiceConfig& config, const std::wstring& prompt,
                   const std::wstring& outputDirectory, std::wstring& outputPath,
                   std::wstring& error);

}  // namespace detail
}  // namespace ui
