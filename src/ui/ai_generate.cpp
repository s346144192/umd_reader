#include "ui/application.h"

#include <windows.h>
#include <mmsystem.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "core/text_util.h"
#include "ui/ai_services.h"
#include "ui/ui_internal.h"

namespace ui {
using namespace detail;

D2D1_RECT_F Application::AiGeneratePanelRect() const {
    constexpr float kWidth = 660.0f;
    constexpr float kHeight = 500.0f;
    const float left = (widthDip_ - kWidth) * 0.5f;
    const float top = (heightDip_ - kHeight) * 0.5f;
    return D2D1::RectF(left, top, left + kWidth, top + kHeight);
}

D2D1_RECT_F Application::AiGeneratePromptRect() const {
    const D2D1_RECT_F panel = AiGeneratePanelRect();
    return D2D1::RectF(panel.left + 24.0f, panel.top + 130.0f, panel.left + 416.0f,
                       panel.top + 168.0f);
}

void Application::OpenAiGenerate() {
    aiGenerateVisible_ = true;
    aiGenerateStatus_.clear();
    chatFocus_ = ChatFocus::GeneratePrompt;
    StartCaretBlink();
    ResetInputCaretToEnd();
    InvalidateRect(hwnd_, nullptr, FALSE);
    UpdateWindow(hwnd_);
}

void Application::CloseAiGenerate() {
    aiGenerateVisible_ = false;
    aiGenerateCancel_ = true;
    chatFocus_ = ChatFocus::None;
    StopCaretBlink();
    InvalidateRect(hwnd_, nullptr, FALSE);
    UpdateWindow(hwnd_);
}

void Application::StartAiSpeechJob(const std::wstring& text) {
    data::ReaderSettings& settings = library_.Settings();
    data::AiServiceConfig config = settings.aiSpeech;
    const data::AiChatConfig& chat = data::ActiveAiChat(settings);
    if (config.baseUrl.empty()) {
        config.baseUrl = chat.baseUrl;
    }
    if (config.apiKey.empty()) {
        config.apiKey = chat.apiKey;
    }
    if (config.baseUrl.empty()) {
        ShowStatus(L"请先在 AI 接口设置中填写语音合成接口地址");
        return;
    }
    if (aiSpeechThread_.joinable()) {
        aiSpeechThread_.join();
    }
    aiSpeechCancel_ = false;
    aiSpeechLoading_ = true;
    speaking_ = true;
    ShowStatus(L"正在合成语音…");
    InvalidateRect(hwnd_, nullptr, FALSE);
    aiSpeechThread_ = std::thread([this, config, text]() {
        std::wstring path;
        std::wstring error;
        const bool succeeded = detail::SynthesizeSpeech(config, text, path, error);
        {
            std::lock_guard<std::mutex> lock(aiSpeechMutex_);
            if (aiSpeechCancel_) {
                return;
            }
            aiSpeechPendingResult_ = succeeded ? path : std::wstring();
            aiSpeechPendingError_ = succeeded ? std::wstring() : error;
            aiSpeechDone_ = true;
        }
        PostMessageW(hwnd_, kAiTaskMessage, 1, 0);
    });
}

void Application::StartAiGenerate() {
    if (aiGenerateBusy_) {
        aiGenerateStatus_ = L"上一个生成任务尚未结束";
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    const std::wstring prompt = TrimNoteText(aiGeneratePrompt_);
    if (prompt.empty()) {
        aiGenerateStatus_ = L"请输入提示词";
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    data::ReaderSettings& settings = library_.Settings();
    const bool video = aiGenerateTab_ == 1;
    data::AiServiceConfig config = video ? settings.aiVideo : settings.aiImage;
    const data::AiChatConfig& chat = data::ActiveAiChat(settings);
    if (config.baseUrl.empty()) {
        config.baseUrl = chat.baseUrl;
    }
    if (config.apiKey.empty()) {
        config.apiKey = chat.apiKey;
    }
    if (config.baseUrl.empty()) {
        aiGenerateStatus_ = L"请先在 AI 接口设置中填写接口地址与密钥";
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    const std::filesystem::path outputDirectory =
        library_.DataDirectory() / (video ? L"generated/videos" : L"generated/images");
    if (aiGenerateThread_.joinable()) {
        aiGenerateThread_.join();
    }
    aiGenerateCancel_ = false;
    aiGenerateBusy_ = true;
    aiGenerateResult_.clear();
    aiGeneratePreview_.Reset();
    aiGeneratePreviewPath_.clear();
    aiGenerateStatus_ = video ? L"正在生成视频，可能需要数分钟…" : L"正在生成图像…";
    InvalidateRect(hwnd_, nullptr, FALSE);
    const int kind = video ? 3 : 2;
    aiGenerateThread_ = std::thread([this, config, prompt, outputDirectory, video, kind]() {
        std::wstring path;
        std::wstring error;
        const bool succeeded =
            video ? detail::GenerateVideo(config, prompt, outputDirectory.wstring(), path, error)
                  : detail::GenerateImage(config, prompt, outputDirectory.wstring(), path, error);
        if (aiGenerateCancel_) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(aiGenerateMutex_);
            aiGeneratePendingKind_ = kind;
            aiGeneratePendingResult_ = succeeded ? path : std::wstring();
            aiGeneratePendingError_ = succeeded ? std::wstring() : error;
            aiGenerateDone_ = true;
        }
        PostMessageW(hwnd_, kAiTaskMessage, static_cast<WPARAM>(kind), 0);
    });
}

void Application::OnAiTaskMessage(UINT kind) {
    if (kind == 1) {
        std::wstring result;
        std::wstring error;
        {
            std::lock_guard<std::mutex> lock(aiSpeechMutex_);
            if (!aiSpeechDone_) {
                return;
            }
            aiSpeechDone_ = false;
            result.swap(aiSpeechPendingResult_);
            error.swap(aiSpeechPendingError_);
        }
        aiSpeechLoading_ = false;
        if (aiSpeechCancel_) {
            aiSpeechCancel_ = false;
            speaking_ = false;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        if (result.empty()) {
            speaking_ = false;
            ShowStatus(error.empty() ? L"语音合成失败" : L"语音合成失败：" + error);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        PlaySoundW(result.c_str(), nullptr,
                   SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
        speaking_ = true;
        ShowStatus(L"正在朗读");
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    std::wstring result;
    std::wstring error;
    {
        std::lock_guard<std::mutex> lock(aiGenerateMutex_);
        if (!aiGenerateDone_ || aiGeneratePendingKind_ != static_cast<int>(kind)) {
            return;
        }
        aiGenerateDone_ = false;
        result.swap(aiGeneratePendingResult_);
        error.swap(aiGeneratePendingError_);
    }
    aiGenerateBusy_ = false;
    if (aiGenerateCancel_) {
        aiGenerateCancel_ = false;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (result.empty()) {
        aiGenerateStatus_ = error.empty() ? L"生成失败" : L"生成失败：" + error;
        InvalidateRect(hwnd_, nullptr, FALSE);
        UpdateWindow(hwnd_);
        return;
    }
    aiGenerateResult_ = result;
    aiGenerateStatus_ = kind == 2 ? L"图像生成完成" : L"视频生成完成";
    if (kind == 2) {
        aiGeneratePreview_ = renderer_.LoadBitmap(std::filesystem::path(result));
        aiGeneratePreviewPath_ = result;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
    UpdateWindow(hwnd_);
}

void Application::DrawAiGenerate(const ThemeColors& colors) {
    if (!aiGenerateVisible_) {
        return;
    }
    const TextFormats& formats = renderer_.Formats();
    renderer_.FillRect(D2D1::RectF(0.0f, 0.0f, widthDip_, heightDip_),
                       ParseHexColor(L"#000000", 0.35f));
    const D2D1_RECT_F panel = AiGeneratePanelRect();
    renderer_.FillRoundedRect(panel, 12.0f, colors.surface);
    renderer_.StrokeRoundedRect(panel, 12.0f, colors.divider, 1.0f);
    renderer_.DrawTextBlock(L"AI 生成",
                            D2D1::RectF(panel.left + 24.0f, panel.top + 16.0f,
                                        panel.right - 24.0f, panel.top + 56.0f),
                            formats.title.Get(), colors.text);
    renderer_.DrawLine(D2D1::Point2F(panel.left + 24.0f, panel.top + 64.0f),
                       D2D1::Point2F(panel.right - 24.0f, panel.top + 64.0f), colors.divider,
                       1.0f);

    DrawButton(L"图像生成",
               D2D1::RectF(panel.left + 24.0f, panel.top + 72.0f, panel.left + 164.0f,
                           panel.top + 102.0f),
               HitKind::AiGenerateTabImage, colors, false, aiGenerateTab_ == 0, 0);
    DrawButton(L"视频生成",
               D2D1::RectF(panel.left + 172.0f, panel.top + 72.0f, panel.left + 312.0f,
                           panel.top + 102.0f),
               HitKind::AiGenerateTabVideo, colors, false, aiGenerateTab_ == 1, 1);

    const bool video = aiGenerateTab_ == 1;
    renderer_.DrawTextBlock(L"提示词",
                            D2D1::RectF(panel.left + 24.0f, panel.top + 110.0f,
                                        panel.left + 416.0f, panel.top + 126.0f),
                            formats.caption.Get(), colors.secondaryText);
    const D2D1_RECT_F promptRect = AiGeneratePromptRect();
    const bool focused = chatFocus_ == ChatFocus::GeneratePrompt;
    renderer_.FillRoundedRect(promptRect, 8.0f, colors.surfaceAlt);
    renderer_.StrokeRoundedRect(promptRect, 8.0f, focused ? colors.accent : colors.divider,
                                focused ? 1.6f : 1.0f);
    const D2D1_RECT_F promptTextRect =
        D2D1::RectF(promptRect.left + 10.0f, promptRect.top + 5.0f, promptRect.right - 10.0f,
                    promptRect.bottom - 5.0f);
    if (aiGeneratePrompt_.empty()) {
        renderer_.DrawTextBlock(L"描述想要生成的画面", promptTextRect, formats.listItem.Get(),
                                colors.secondaryText);
    } else {
        renderer_.DrawTextBlock(aiGeneratePrompt_, promptTextRect, formats.listItem.Get(),
                                colors.text);
        if (focused) {
            DrawInputSelection(promptTextRect, formats.listItem.Get(), false, colors);
        }
    }
    AddHit(promptRect, HitKind::AiGeneratePrompt);

    renderer_.DrawTextBlock(video ? L"尺寸 1280x720 · 时长 4 秒"
                                  : L"尺寸 1024x1024 · 输出 PNG/JPEG",
                            D2D1::RectF(panel.left + 24.0f, panel.top + 176.0f,
                                        panel.left + 416.0f, panel.top + 196.0f),
                            formats.caption.Get(), colors.secondaryText);

    DrawButton(aiGenerateBusy_ ? L"生成中…" : L"开始生成",
               D2D1::RectF(panel.left + 24.0f, panel.top + 206.0f, panel.left + 144.0f,
                           panel.top + 242.0f),
               HitKind::AiGenerateRun, colors, true, !aiGenerateBusy_);
    if (!aiGenerateResult_.empty()) {
        DrawButton(L"打开结果",
                   D2D1::RectF(panel.left + 154.0f, panel.top + 206.0f, panel.left + 274.0f,
                               panel.top + 242.0f),
                   HitKind::AiGenerateOpenResult, colors, false, false);
    }

    renderer_.DrawTextBlock(aiGenerateStatus_,
                            D2D1::RectF(panel.left + 24.0f, panel.top + 252.0f,
                                        panel.left + 416.0f, panel.top + 330.0f),
                            formats.caption.Get(), colors.secondaryText);

    // 右侧预览区域：图像显示生成结果，视频显示保存路径。
    const D2D1_RECT_F previewRect =
        D2D1::RectF(panel.left + 436.0f, panel.top + 110.0f, panel.right - 24.0f,
                    panel.top + 400.0f);
    renderer_.FillRoundedRect(previewRect, 9.0f, colors.surfaceAlt);
    if (!video && aiGeneratePreview_) {
        const D2D1_SIZE_F size =
            ImageDisplaySize(aiGeneratePreview_.Get(), previewRect.right - previewRect.left - 16.0f,
                             previewRect.bottom - previewRect.top - 16.0f);
        const D2D1_RECT_F imageRect =
            D2D1::RectF(previewRect.left + (previewRect.right - previewRect.left - size.width) * 0.5f,
                        previewRect.top + (previewRect.bottom - previewRect.top - size.height) * 0.5f,
                        previewRect.left + (previewRect.right - previewRect.left + size.width) * 0.5f,
                        previewRect.top + (previewRect.bottom - previewRect.top + size.height) * 0.5f);
        renderer_.DrawBitmap(aiGeneratePreview_.Get(), imageRect, 1.0f);
    } else if (video && !aiGenerateResult_.empty()) {
        const std::wstring name = std::filesystem::path(aiGenerateResult_).filename().wstring();
        renderer_.DrawTextBlock(L"视频已保存：\n" + name,
                                D2D1::RectF(previewRect.left + 16.0f, previewRect.top + 16.0f,
                                            previewRect.right - 16.0f, previewRect.top + 90.0f),
                                formats.caption.Get(), colors.text);
    } else {
        renderer_.DrawTextBlock(video ? L"视频生成结果将保存在数据目录"
                                      : L"图像预览",
                                D2D1::RectF(previewRect.left + 16.0f, previewRect.top + 16.0f,
                                            previewRect.right - 16.0f, previewRect.top + 60.0f),
                                formats.caption.Get(), colors.secondaryText);
    }

    DrawButton(L"关闭",
               D2D1::RectF(panel.right - 124.0f, panel.top + 440.0f, panel.right - 24.0f,
                           panel.top + 476.0f),
               HitKind::AiGenerateClose, colors, false, false);
}

}  // namespace ui
