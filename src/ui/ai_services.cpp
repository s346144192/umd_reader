#include "ui/ai_services.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "app_identity.h"
#include "core/text_util.h"
#include "nlohmann/json.hpp"

namespace ui {
namespace detail {
namespace {

struct HttpResponse {
    DWORD statusCode = 0;
    std::string body;
};

std::wstring TrimBaseUrl(const std::wstring& url) {
    std::wstring result = url;
    while (!result.empty() && (result.back() == L'/' || result.back() == L' ')) {
        result.pop_back();
    }
    return result;
}

// 发送一次同步 HTTP 请求；body 仅用于 POST。
bool HttpRequest(const std::wstring& method, const std::wstring& url,
                 const std::wstring& apiKey, const std::string& body,
                 HttpResponse& response, std::wstring& error) {
    response = HttpResponse{};
    error.clear();
    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &components)) {
        error = L"接口地址格式无效";
        return false;
    }
    const std::wstring host(components.lpszHostName, components.dwHostNameLength);
    std::wstring path(components.lpszUrlPath, components.dwUrlPathLength);
    if (components.dwExtraInfoLength > 0) {
        path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    }
    if (path.empty()) {
        path = L"/";
    }

    // 与对话接口保持一致：遵循系统代理，保证需要代理的接口可以访问。
    // AUTOMATIC_PROXY 在 Windows 8.1 之前不可用，失败时退回默认代理配置。
    HINTERNET session = WinHttpOpen(app_id::kHttpUserAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        session = WinHttpOpen(app_id::kHttpUserAgent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (!session) {
        error = L"无法初始化网络会话";
        return false;
    }
    WinHttpSetTimeouts(session, 10000, 10000, 30000, 300000);
    HINTERNET connection = WinHttpConnect(session, host.c_str(), components.nPort, 0);
    if (!connection) {
        WinHttpCloseHandle(session);
        error = L"无法连接接口主机";
        return false;
    }
    const DWORD flags =
        components.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = WinHttpOpenRequest(connection, method.c_str(), path.c_str(), nullptr,
                                           WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           flags);
    if (!request) {
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        error = L"无法创建请求";
        return false;
    }

    std::wstring headers;
    if (!apiKey.empty()) {
        headers += L"Authorization: Bearer " + apiKey + L"\r\n";
    }
    if (!body.empty()) {
        headers += L"Content-Type: application/json\r\n";
    }
    if (!headers.empty()) {
        WinHttpAddRequestHeaders(request, headers.c_str(), static_cast<DWORD>(headers.size()),
                                 WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    }

    const BOOL sent =
        WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                           body.empty() ? WINHTTP_NO_REQUEST_DATA
                                        : const_cast<char*>(body.data()),
                           static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0);
    BOOL received = FALSE;
    if (sent) {
        received = WinHttpReceiveResponse(request, nullptr);
    }
    if (!sent || !received) {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        error = L"请求失败，请检查网络、接口地址与密钥";
        return false;
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize,
                        WINHTTP_NO_HEADER_INDEX);
    response.statusCode = statusCode;

    std::vector<char> buffer(8192);
    for (;;) {
        DWORD read = 0;
        if (!WinHttpReadData(request, buffer.data(), static_cast<DWORD>(buffer.size()), &read) ||
            read == 0) {
            break;
        }
        response.body.append(buffer.data(), read);
        if (response.body.size() > 64ull * 1024 * 1024) {
            error = L"接口返回内容超出上限";
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connection);
            WinHttpCloseHandle(session);
            return false;
        }
    }
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return true;
}

std::wstring HttpErrorMessage(const HttpResponse& response) {
    std::string detail = response.body;
    try {
        const nlohmann::json parsed = nlohmann::json::parse(detail);
        if (parsed.contains("error") && parsed["error"].is_object()) {
            const nlohmann::json& error = parsed["error"];
            if (error.contains("message") && error["message"].is_string()) {
                detail = error["message"].get<std::string>();
            }
        }
    } catch (const std::exception&) {
        // 保留原始响应文本。
    }
    if (detail.size() > 300) {
        detail.resize(300);
    }
    return L"接口返回错误（HTTP " + std::to_wstring(response.statusCode) + L"）：" +
           util::FromUtf8(detail);
}

bool DecodeBase64(const std::string& encoded, std::vector<uint8_t>& bytes) {
    bytes.clear();
    std::vector<uint8_t> output;
    int quartet[4] = {0, 0, 0, 0};
    int count = 0;
    for (const char character : encoded) {
        if (character == '\r' || character == '\n' || character == ' ' || character == '\t') {
            continue;
        }
        int value = -1;
        if (character >= 'A' && character <= 'Z') {
            value = character - 'A';
        } else if (character >= 'a' && character <= 'z') {
            value = character - 'a' + 26;
        } else if (character >= '0' && character <= '9') {
            value = character - '0' + 52;
        } else if (character == '+') {
            value = 62;
        } else if (character == '/') {
            value = 63;
        } else if (character == '=') {
            break;
        } else {
            continue;
        }
        quartet[count++] = value;
        if (count == 4) {
            output.push_back(static_cast<uint8_t>((quartet[0] << 2) | (quartet[1] >> 4)));
            output.push_back(static_cast<uint8_t>((quartet[1] << 4) | (quartet[2] >> 2)));
            output.push_back(static_cast<uint8_t>((quartet[2] << 6) | quartet[3]));
            count = 0;
        }
    }
    if (count == 2) {
        output.push_back(static_cast<uint8_t>((quartet[0] << 2) | (quartet[1] >> 4)));
    } else if (count == 3) {
        output.push_back(static_cast<uint8_t>((quartet[0] << 2) | (quartet[1] >> 4)));
        output.push_back(static_cast<uint8_t>((quartet[1] << 4) | (quartet[2] >> 2)));
    }
    bytes = std::move(output);
    return !bytes.empty();
}

std::wstring GuessImageExtension(const std::vector<uint8_t>& bytes) {
    if (bytes.size() >= 3 && bytes[0] == 0xFF && bytes[1] == 0xD8 && bytes[2] == 0xFF) {
        return L".jpg";
    }
    if (bytes.size() >= 8 && bytes[0] == 0x89 && bytes[1] == 0x50 && bytes[2] == 0x4E &&
        bytes[3] == 0x47) {
        return L".png";
    }
    if (bytes.size() >= 4 && bytes[0] == 0x47 && bytes[1] == 0x49 && bytes[2] == 0x46) {
        return L".gif";
    }
    if (bytes.size() >= 12 && bytes[0] == 0x52 && bytes[1] == 0x49 && bytes[2] == 0x46 &&
        bytes[3] == 0x46 && bytes[8] == 0x57 && bytes[9] == 0x45) {
        return L".webp";
    }
    return L".png";
}

bool WriteBytes(const std::filesystem::path& path, const std::vector<uint8_t>& bytes,
                std::wstring& error) {
    std::error_code code;
    std::filesystem::create_directories(path.parent_path(), code);
    if (code) {
        error = L"无法创建输出目录：" + path.parent_path().wstring();
        return false;
    }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        error = L"无法写入文件：" + path.wstring();
        return false;
    }
    if (!bytes.empty()) {
        file.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    }
    if (!file) {
        error = L"写入文件失败：" + path.wstring();
        return false;
    }
    return true;
}

bool DownloadBinary(const std::wstring& url, const std::wstring& apiKey,
                    std::vector<uint8_t>& bytes, std::wstring& error) {
    HttpResponse response;
    if (!HttpRequest(L"GET", url, apiKey, std::string(), response, error)) {
        return false;
    }
    if (response.statusCode != 200) {
        error = HttpErrorMessage(response);
        return false;
    }
    bytes.assign(response.body.begin(), response.body.end());
    return !bytes.empty();
}

// 从 JSON 里读取第一个字符串字段，用于兼容不同服务商的字段名。
bool FindString(const nlohmann::json& object, std::initializer_list<const char*> keys,
                std::string& value) {
    for (const char* key : keys) {
        const auto it = object.find(key);
        if (it != object.end() && it->is_string() && !it->get<std::string>().empty()) {
            value = it->get<std::string>();
            return true;
        }
    }
    return false;
}

// 解析 WAV 的 fmt 与 data 块，用于合并多段语音。
bool ParseWav(const std::vector<uint8_t>& bytes, std::vector<uint8_t>& format,
              std::vector<uint8_t>& pcm) {
    format.clear();
    pcm.clear();
    if (bytes.size() < 12 || bytes[0] != 'R' || bytes[1] != 'I' || bytes[2] != 'F' ||
        bytes[3] != 'F' || bytes[8] != 'W' || bytes[9] != 'A' || bytes[10] != 'V' ||
        bytes[11] != 'E') {
        return false;
    }
    size_t offset = 12;
    while (offset + 8 <= bytes.size()) {
        const uint32_t size = static_cast<uint32_t>(bytes[offset + 4]) |
                              (static_cast<uint32_t>(bytes[offset + 5]) << 8) |
                              (static_cast<uint32_t>(bytes[offset + 6]) << 16) |
                              (static_cast<uint32_t>(bytes[offset + 7]) << 24);
        const std::string id(reinterpret_cast<const char*>(bytes.data() + offset), 4);
        const size_t dataStart = offset + 8;
        const size_t dataEnd =
            std::min(dataStart + static_cast<size_t>(size), bytes.size());
        if (id == "fmt ") {
            format.assign(bytes.begin() + static_cast<std::ptrdiff_t>(dataStart),
                          bytes.begin() + static_cast<std::ptrdiff_t>(dataEnd));
        } else if (id == "data") {
            pcm.insert(pcm.end(), bytes.begin() + static_cast<std::ptrdiff_t>(dataStart),
                       bytes.begin() + static_cast<std::ptrdiff_t>(dataEnd));
        }
        offset = dataEnd + (size & 1u);
    }
    return !format.empty() && !pcm.empty();
}

std::vector<std::wstring> SplitSpeechText(const std::wstring& text, size_t limit) {
    std::vector<std::wstring> chunks;
    size_t begin = 0;
    while (begin < text.size()) {
        size_t end = std::min(begin + limit, text.size());
        if (end < text.size()) {
            const size_t searchBegin = begin + limit / 2;
            size_t split = std::wstring::npos;
            for (size_t i = end; i > searchBegin; --i) {
                const wchar_t character = text[i - 1];
                if (character == L'。' || character == L'！' || character == L'？' ||
                    character == L'\n' || character == L'；' || character == L'.') {
                    split = i;
                    break;
                }
            }
            if (split != std::wstring::npos) {
                end = split;
            }
        }
        chunks.push_back(text.substr(begin, end - begin));
        begin = end;
        while (begin < text.size() && (text[begin] == L' ' || text[begin] == L'\r' ||
                                       text[begin] == L'\n')) {
            ++begin;
        }
    }
    return chunks;
}

}  // namespace

bool SynthesizeSpeech(const data::AiServiceConfig& config, const std::wstring& text,
                      std::wstring& outputPath, std::wstring& error) {
    outputPath.clear();
    error.clear();
    if (text.empty()) {
        error = L"没有可朗读的文本";
        return false;
    }
    const std::wstring baseUrl = TrimBaseUrl(config.baseUrl);
    if (baseUrl.empty()) {
        error = L"未配置语音合成接口地址";
        return false;
    }
    const std::wstring model = config.model.empty() ? std::wstring(L"tts-1") : config.model;
    const std::wstring voice = config.voice.empty() ? std::wstring(L"alloy") : config.voice;
    const std::vector<std::wstring> chunks = SplitSpeechText(text, 3500);
    std::vector<uint8_t> format;
    std::vector<uint8_t> pcm;
    for (const std::wstring& chunk : chunks) {
        nlohmann::json body;
        body["model"] = util::ToUtf8(model);
        body["input"] = util::ToUtf8(chunk);
        body["voice"] = util::ToUtf8(voice);
        body["response_format"] = "wav";
        HttpResponse response;
        if (!HttpRequest(L"POST", baseUrl + L"/audio/speech", config.apiKey, body.dump(),
                         response, error)) {
            return false;
        }
        if (response.statusCode != 200) {
            error = HttpErrorMessage(response);
            return false;
        }
        const std::vector<uint8_t> bytes(response.body.begin(), response.body.end());
        std::vector<uint8_t> chunkFormat;
        std::vector<uint8_t> chunkPcm;
        if (!ParseWav(bytes, chunkFormat, chunkPcm)) {
            error = L"语音合成返回的不是有效 WAV 数据";
            return false;
        }
        if (format.empty()) {
            format = chunkFormat;
        } else if (format != chunkFormat) {
            error = L"多段语音的音频格式不一致，无法合并播放";
            return false;
        }
        pcm.insert(pcm.end(), chunkPcm.begin(), chunkPcm.end());
    }
    std::vector<uint8_t> wav;
    wav.reserve(44 + pcm.size());
    wav.push_back('R');
    wav.push_back('I');
    wav.push_back('F');
    wav.push_back('F');
    const uint32_t riffSize = 20 + static_cast<uint32_t>(format.size()) +
                              static_cast<uint32_t>(pcm.size());
    wav.push_back(static_cast<uint8_t>(riffSize & 0xFF));
    wav.push_back(static_cast<uint8_t>((riffSize >> 8) & 0xFF));
    wav.push_back(static_cast<uint8_t>((riffSize >> 16) & 0xFF));
    wav.push_back(static_cast<uint8_t>((riffSize >> 24) & 0xFF));
    wav.push_back('W');
    wav.push_back('A');
    wav.push_back('V');
    wav.push_back('E');
    wav.push_back('f');
    wav.push_back('m');
    wav.push_back('t');
    wav.push_back(' ');
    const uint32_t formatSize = static_cast<uint32_t>(format.size());
    wav.push_back(static_cast<uint8_t>(formatSize & 0xFF));
    wav.push_back(static_cast<uint8_t>((formatSize >> 8) & 0xFF));
    wav.push_back(static_cast<uint8_t>((formatSize >> 16) & 0xFF));
    wav.push_back(static_cast<uint8_t>((formatSize >> 24) & 0xFF));
    wav.insert(wav.end(), format.begin(), format.end());
    wav.push_back('d');
    wav.push_back('a');
    wav.push_back('t');
    wav.push_back('a');
    const uint32_t dataSize = static_cast<uint32_t>(pcm.size());
    wav.push_back(static_cast<uint8_t>(dataSize & 0xFF));
    wav.push_back(static_cast<uint8_t>((dataSize >> 8) & 0xFF));
    wav.push_back(static_cast<uint8_t>((dataSize >> 16) & 0xFF));
    wav.push_back(static_cast<uint8_t>((dataSize >> 24) & 0xFF));
    wav.insert(wav.end(), pcm.begin(), pcm.end());

    wchar_t tempDirectory[MAX_PATH] = {};
    if (!GetTempPathW(MAX_PATH, tempDirectory)) {
        error = L"无法获取临时目录";
        return false;
    }
    const std::filesystem::path path =
        std::filesystem::path(tempDirectory) /
        (std::wstring(app_id::kTtsTempPrefix) + std::to_wstring(GetCurrentProcessId()) + L"_" +
         std::to_wstring(GetTickCount()) + L".wav");
    if (!WriteBytes(path, wav, error)) {
        return false;
    }
    outputPath = path.wstring();
    return true;
}

bool GenerateImage(const data::AiServiceConfig& config, const std::wstring& prompt,
                   const std::wstring& outputDirectory, std::wstring& outputPath,
                   std::wstring& error) {
    outputPath.clear();
    error.clear();
    const std::wstring baseUrl = TrimBaseUrl(config.baseUrl);
    if (baseUrl.empty() || prompt.empty()) {
        error = L"图像生成需要接口地址与提示词";
        return false;
    }
    const std::wstring model = config.model.empty() ? std::wstring(L"gpt-image-1") : config.model;
    nlohmann::json body;
    body["model"] = util::ToUtf8(model);
    body["prompt"] = util::ToUtf8(prompt);
    body["n"] = 1;
    body["size"] = "1024x1024";
    body["response_format"] = "b64_json";
    HttpResponse response;
    if (!HttpRequest(L"POST", baseUrl + L"/images/generations", config.apiKey, body.dump(),
                     response, error)) {
        return false;
    }
    if (response.statusCode != 200) {
        error = HttpErrorMessage(response);
        return false;
    }
    std::vector<uint8_t> bytes;
    try {
        const nlohmann::json parsed = nlohmann::json::parse(response.body);
        if (!parsed.contains("data") || !parsed["data"].is_array() ||
            parsed["data"].empty()) {
            error = L"图像接口未返回图片数据";
            return false;
        }
        const nlohmann::json& item = parsed["data"][0];
        std::string base64;
        std::string url;
        if (FindString(item, {"b64_json", "b64_json_data", "image_base64"}, base64)) {
            if (!DecodeBase64(base64, bytes)) {
                error = L"图像数据解码失败";
                return false;
            }
        } else if (FindString(item, {"url", "image_url"}, url)) {
            if (!DownloadBinary(util::FromUtf8(url), config.apiKey, bytes, error)) {
                return false;
            }
        } else {
            error = L"图像接口返回格式不受支持";
            return false;
        }
    } catch (const std::exception&) {
        error = L"图像接口返回内容不是有效 JSON";
        return false;
    }
    const std::filesystem::path directory(outputDirectory);
    const std::filesystem::path path =
        directory / (L"image-" + std::to_wstring(util::CurrentUnixSeconds()) + L"-" +
                     std::to_wstring(GetTickCount()) + GuessImageExtension(bytes));
    if (!WriteBytes(path, bytes, error)) {
        return false;
    }
    outputPath = path.wstring();
    return true;
}

bool GenerateVideo(const data::AiServiceConfig& config, const std::wstring& prompt,
                   const std::wstring& outputDirectory, std::wstring& outputPath,
                   std::wstring& error) {
    outputPath.clear();
    error.clear();
    const std::wstring baseUrl = TrimBaseUrl(config.baseUrl);
    if (baseUrl.empty() || prompt.empty()) {
        error = L"视频生成需要接口地址与提示词";
        return false;
    }
    const std::wstring model = config.model.empty() ? std::wstring(L"sora-2") : config.model;
    nlohmann::json body;
    body["model"] = util::ToUtf8(model);
    body["prompt"] = util::ToUtf8(prompt);
    body["size"] = "1280x720";
    body["seconds"] = "4";
    HttpResponse response;
    if (!HttpRequest(L"POST", baseUrl + L"/videos", config.apiKey, body.dump(), response,
                     error)) {
        return false;
    }
    if (response.statusCode != 200 && response.statusCode != 201) {
        error = HttpErrorMessage(response);
        return false;
    }
    std::string taskId;
    std::string directUrl;
    try {
        const nlohmann::json parsed = nlohmann::json::parse(response.body);
        FindString(parsed, {"id", "task_id", "video_id"}, taskId);
        FindString(parsed, {"url", "video_url"}, directUrl);
    } catch (const std::exception&) {
        error = L"视频接口返回内容不是有效 JSON";
        return false;
    }
    std::vector<uint8_t> bytes;
    if (!directUrl.empty()) {
        if (!DownloadBinary(util::FromUtf8(directUrl), config.apiKey, bytes, error)) {
            return false;
        }
    } else {
        if (taskId.empty()) {
            error = L"视频接口未返回任务编号";
            return false;
        }
        const std::wstring statusUrl =
            baseUrl + L"/videos/" + util::FromUtf8(taskId);
        bool completed = false;
        for (int attempt = 0; attempt < 120; ++attempt) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            HttpResponse statusResponse;
            if (!HttpRequest(L"GET", statusUrl, config.apiKey, std::string(), statusResponse,
                             error)) {
                return false;
            }
            if (statusResponse.statusCode != 200) {
                error = HttpErrorMessage(statusResponse);
                return false;
            }
            std::string status;
            std::string url;
            try {
                const nlohmann::json parsed = nlohmann::json::parse(statusResponse.body);
                FindString(parsed, {"status", "state"}, status);
                FindString(parsed, {"url", "video_url"}, url);
                if (url.empty() && parsed.contains("output") && parsed["output"].is_object()) {
                    FindString(parsed["output"], {"url", "video_url"}, url);
                }
                if (status == "failed" || status == "error" || status == "cancelled") {
                    error = L"视频生成失败：" + util::FromUtf8(status);
                    return false;
                }
                if (status == "completed" || status == "succeeded" || status == "success" ||
                    status == "done") {
                    completed = true;
                }
            } catch (const std::exception&) {
                // 状态行解析失败时继续轮询。
            }
            if (completed) {
                if (!url.empty()) {
                    if (!DownloadBinary(util::FromUtf8(url), config.apiKey, bytes, error)) {
                        return false;
                    }
                } else {
                    if (!DownloadBinary(statusUrl + L"/content", config.apiKey, bytes, error)) {
                        return false;
                    }
                }
                break;
            }
        }
        if (!completed || bytes.empty()) {
            error = L"视频生成超时或未返回视频内容";
            return false;
        }
    }
    if (bytes.empty()) {
        error = L"视频内容为空";
        return false;
    }
    const std::filesystem::path directory(outputDirectory);
    const std::filesystem::path path =
        directory / (L"video-" + std::to_wstring(util::CurrentUnixSeconds()) + L"-" +
                     std::to_wstring(GetTickCount()) + L".mp4");
    if (!WriteBytes(path, bytes, error)) {
        return false;
    }
    outputPath = path.wstring();
    return true;
}

}  // namespace detail
}  // namespace ui
