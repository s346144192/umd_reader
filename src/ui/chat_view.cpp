#include "ui/application.h"

#include <windowsx.h>
#include <d2d1helper.h>
#include <imm.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <winhttp.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <iterator>
#include <regex>
#include <vector>

#include "core/text_util.h"
#include "nlohmann/json.hpp"
#include "ui/ui_internal.h"

namespace ui {
using namespace detail;

namespace {

// 会话删除按钮的图标：垃圾桶轮廓，替代文字标签，节省行内宽度。
void DrawTrashIcon(ui::RenderContext& renderer, const D2D1_RECT_F& rect,
                   const D2D1_COLOR_F& color) {
    const float width = rect.right - rect.left;
    const float height = rect.bottom - rect.top;
    const float side = std::min(width, height);
    const float iconWidth = side * 0.46f;
    const float iconHeight = iconWidth * 1.18f;
    const float left = (rect.left + rect.right) * 0.5f - iconWidth * 0.5f;
    const float top = (rect.top + rect.bottom) * 0.5f - iconHeight * 0.5f;
    const float right = left + iconWidth;
    const float bottom = top + iconHeight;
    const float lidY = top + iconHeight * 0.16f;
    // 桶盖与提手。
    renderer.DrawLine(D2D1::Point2F(left - 1.0f, lidY), D2D1::Point2F(right + 1.0f, lidY), color,
                      1.6f);
    renderer.DrawLine(D2D1::Point2F(left + iconWidth * 0.34f, lidY - iconHeight * 0.12f),
                      D2D1::Point2F(right - iconWidth * 0.34f, lidY - iconHeight * 0.12f), color,
                      1.6f);
    // 桶身。
    const D2D1_RECT_F body =
        D2D1::RectF(left + iconWidth * 0.12f, lidY + 2.0f, right - iconWidth * 0.12f, bottom);
    renderer.StrokeRoundedRect(body, 1.5f, color, 1.4f);
    // 两道竖纹，示意垃圾桶。
    const float stripeTop = body.top + iconHeight * 0.16f;
    const float stripeBottom = body.bottom - iconHeight * 0.12f;
    renderer.DrawLine(D2D1::Point2F(left + iconWidth * 0.38f, stripeTop),
                      D2D1::Point2F(left + iconWidth * 0.38f, stripeBottom), color, 1.2f);
    renderer.DrawLine(D2D1::Point2F(left + iconWidth * 0.62f, stripeTop),
                      D2D1::Point2F(left + iconWidth * 0.62f, stripeBottom), color, 1.2f);
}

// 按可用像素宽度截断文字并补省略号：比按字符数截断更贴合实际可用空间。
std::wstring EllipsizeToWidth(ui::RenderContext& renderer, const std::wstring& text,
                              IDWriteTextFormat* format, float maxWidth) {
    if (text.empty() || format == nullptr || maxWidth <= 0.0f) {
        return text;
    }
    if (renderer.MeasureTextWidth(text, format) <= maxWidth) {
        return text;
    }
    const std::wstring suffix = L"…";
    const float suffixWidth = renderer.MeasureTextWidth(suffix, format);
    size_t low = 0;
    size_t high = text.size();
    while (low < high) {
        const size_t mid = (low + high + 1) / 2;
        if (renderer.MeasureTextWidth(text.substr(0, mid), format) + suffixWidth <= maxWidth) {
            low = mid;
        } else {
            high = mid - 1;
        }
    }
    return text.substr(0, low) + suffix;
}

}  // namespace

D2D1_RECT_F Application::ChatFieldRect(ChatFocus field) const {
    const D2D1_RECT_F contentRect = ReaderPanelContentRect();
    if (field != ChatFocus::Message) {
        return D2D1::RectF();
    }
    // 多行输入框横跨整个面板宽度，发送按钮放在其下方的底部。
    return D2D1::RectF(contentRect.left, contentRect.bottom - 104.0f, contentRect.right,
                       contentRect.bottom - 44.0f);
}

D2D1_RECT_F Application::AiChatEntryRect() const {
    const float panelReserve = panel_ != PanelMode::None ? PanelWidth() : 0.0f;
    const float availableWidth = widthDip_ - panelReserve;
    constexpr float kEntryWidth = 112.0f;
    constexpr float kEntryHeight = 30.0f;
    const float centerX = availableWidth * 0.5f;
    const float bottom = heightDip_ - 7.0f;
    return D2D1::RectF(centerX - kEntryWidth * 0.5f, bottom - kEntryHeight,
                       centerX + kEntryWidth * 0.5f, bottom);
}

D2D1_RECT_F Application::AiSettingsPanelRect() const {
    constexpr float kSettingsWidth = 660.0f;
    constexpr float kSettingsHeight = 520.0f;
    const float left = (widthDip_ - kSettingsWidth) * 0.5f;
    const float top = (heightDip_ - kSettingsHeight) * 0.5f;
    return D2D1::RectF(left, top, left + kSettingsWidth, top + kSettingsHeight);
}

D2D1_RECT_F Application::AiSettingsFieldRect(ChatFocus field) const {
    const D2D1_RECT_F panel = AiSettingsPanelRect();
    float top = 0.0f;
    switch (field) {
    case ChatFocus::Name:
        top = panel.top + 226.0f;
        break;
    case ChatFocus::BaseUrl:
        top = panel.top + 274.0f;
        break;
    case ChatFocus::ApiKey:
        top = panel.top + 322.0f;
        break;
    case ChatFocus::Model:
        top = panel.top + 370.0f;
        break;
    case ChatFocus::SpeechBaseUrl:
        top = panel.top + 232.0f;
        break;
    case ChatFocus::SpeechApiKey:
        top = panel.top + 280.0f;
        break;
    case ChatFocus::SpeechModel:
        top = panel.top + 328.0f;
        break;
    case ChatFocus::SpeechVoice:
        top = panel.top + 376.0f;
        break;
    case ChatFocus::ImageBaseUrl:
    case ChatFocus::VideoBaseUrl:
        top = panel.top + 176.0f;
        break;
    case ChatFocus::ImageApiKey:
    case ChatFocus::VideoApiKey:
        top = panel.top + 224.0f;
        break;
    case ChatFocus::ImageModel:
    case ChatFocus::VideoModel:
        top = panel.top + 272.0f;
        break;
    case ChatFocus::ContextTokens:
        return D2D1::RectF(panel.left + 356.0f, panel.top + 428.0f, panel.right - 24.0f,
                           panel.top + 458.0f);
    default:
        return D2D1::RectF();
    }
    return D2D1::RectF(panel.left + 24.0f, top, panel.right - 24.0f, top + 30.0f);
}

D2D1_RECT_F Application::ChatConversationRect() const {
    const D2D1_RECT_F contentRect = ReaderPanelContentRect();
    // 顶部为接口信息行与会话栏，对话内容区从其下方开始。
    return D2D1::RectF(contentRect.left, contentRect.top + 74.0f, contentRect.right,
                       contentRect.bottom - 148.0f);
}

D2D1_RECT_F Application::ChatScrollbarTrackRect() const {
    const D2D1_RECT_F rect = ChatConversationRect();
    constexpr float kTrackWidth = 8.0f;
    return D2D1::RectF(rect.right - kTrackWidth - 2.0f, rect.top + 2.0f, rect.right - 2.0f,
                       rect.bottom - 2.0f);
}

void Application::ScrollChatFromY(float y) {
    if (chatScrollMax_ <= 0.0f) {
        return;
    }
    const D2D1_RECT_F track = ChatScrollbarTrackRect();
    const float trackHeight = track.bottom - track.top;
    const float contentHeight = trackHeight + chatScrollMax_;
    const float thumbHeight = std::max(28.0f, trackHeight * trackHeight / contentHeight);
    const float usable = std::max(1.0f, trackHeight - thumbHeight);
    const float ratio = std::clamp((y - track.top - thumbHeight * 0.5f) / usable, 0.0f, 1.0f);
    chatScroll_ = ratio * chatScrollMax_;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

D2D1_RECT_F Application::ChatStatusRect() const {
    const D2D1_RECT_F contentRect = ReaderPanelContentRect();
    return D2D1::RectF(contentRect.left, contentRect.bottom - 140.0f, contentRect.right - 96.0f,
                       contentRect.bottom - 112.0f);
}

D2D1_RECT_F Application::ChatSendRect() const {
    const D2D1_RECT_F contentRect = ReaderPanelContentRect();
    return D2D1::RectF(contentRect.right - 88.0f, contentRect.bottom - 38.0f, contentRect.right,
                       contentRect.bottom - 4.0f);
}

D2D1_RECT_F Application::ChatStopRect() const {
    const D2D1_RECT_F contentRect = ReaderPanelContentRect();
    return D2D1::RectF(contentRect.right - 88.0f, contentRect.bottom - 140.0f,
                       contentRect.right, contentRect.bottom - 112.0f);
}

std::wstring* Application::ActiveChatField() {
    data::ReaderSettings& settings = library_.Settings();
    if (aiSettingsVisible_ || panel_ == PanelMode::Chat) {
        data::AiChatConfig& chat = data::ActiveAiChat(settings);
        switch (chatFocus_) {
        case ChatFocus::Name:
            return aiSettingsVisible_ ? &chat.name : nullptr;
        case ChatFocus::BaseUrl:
            return &chat.baseUrl;
        case ChatFocus::ApiKey:
            return &chat.apiKey;
        case ChatFocus::Model:
            return &chat.model;
        case ChatFocus::SpeechBaseUrl:
            return aiSettingsVisible_ ? &settings.aiSpeech.baseUrl : nullptr;
        case ChatFocus::SpeechApiKey:
            return aiSettingsVisible_ ? &settings.aiSpeech.apiKey : nullptr;
        case ChatFocus::SpeechModel:
            return aiSettingsVisible_ ? &settings.aiSpeech.model : nullptr;
        case ChatFocus::SpeechVoice:
            return aiSettingsVisible_ ? &settings.aiSpeech.voice : nullptr;
        case ChatFocus::ImageBaseUrl:
            return aiSettingsVisible_ ? &settings.aiImage.baseUrl : nullptr;
        case ChatFocus::ImageApiKey:
            return aiSettingsVisible_ ? &settings.aiImage.apiKey : nullptr;
        case ChatFocus::ImageModel:
            return aiSettingsVisible_ ? &settings.aiImage.model : nullptr;
        case ChatFocus::VideoBaseUrl:
            return aiSettingsVisible_ ? &settings.aiVideo.baseUrl : nullptr;
        case ChatFocus::VideoApiKey:
            return aiSettingsVisible_ ? &settings.aiVideo.apiKey : nullptr;
        case ChatFocus::VideoModel:
            return aiSettingsVisible_ ? &settings.aiVideo.model : nullptr;
        case ChatFocus::ContextTokens:
            return aiSettingsVisible_ ? &aiContextTokensDraft_ : nullptr;
        case ChatFocus::GeneratePrompt:
            return aiGenerateVisible_ ? &aiGeneratePrompt_ : nullptr;
        case ChatFocus::Message:
            return panel_ == PanelMode::Chat ? &aiDraft_ : nullptr;
        default:
            return nullptr;
        }
    }
    return nullptr;
}

void Application::DrawChatPanel(const ThemeColors& colors, const D2D1_RECT_F& contentRect) {
    const TextFormats& formats = renderer_.Formats();
    const data::ReaderSettings& settings = library_.Settings();

    // 接口配置已移至独立弹框，这里仅保留入口与当前模型提示。
    const data::AiChatConfig& chat = data::ActiveAiChat(settings);
    const std::wstring protocol =
        chat.protocol == L"anthropic" ? std::wstring(L"Anthropic") : std::wstring(L"OpenAI");
    const std::wstring modelLabel =
        chat.model.empty() ? std::wstring(L"未设置模型") : chat.model;
    renderer_.DrawTextBlock(util::Ellipsize(protocol + L" · " + modelLabel, 26),
                            D2D1::RectF(contentRect.left, contentRect.top, contentRect.right - 164.0f,
                                        contentRect.top + 30.0f),
                            formats.caption.Get(), colors.secondaryText);
    DrawPlainButton(L"生成",
                    D2D1::RectF(contentRect.right - 156.0f, contentRect.top,
                                contentRect.right - 96.0f, contentRect.top + 30.0f),
                    HitKind::AiGenerateOpen, colors);
    DrawButton(L"接口设置",
               D2D1::RectF(contentRect.right - 88.0f, contentRect.top, contentRect.right,
                           contentRect.top + 30.0f),
               HitKind::OpenAiSettings, colors, false, false);

    // 会话栏：打开对话记录、新建会话，中间显示当前会话标题。
    const D2D1_RECT_F sessionBar =
        D2D1::RectF(contentRect.left, contentRect.top + 38.0f, contentRect.right,
                    contentRect.top + 68.0f);
    DrawButton(L"会话", D2D1::RectF(sessionBar.left, sessionBar.top, sessionBar.left + 56.0f,
                                    sessionBar.top + 30.0f),
               HitKind::ChatSessionOpen, colors, false, chatSessionPickerVisible_);
    DrawPlainButton(L"新建",
                    D2D1::RectF(sessionBar.right - 56.0f, sessionBar.top, sessionBar.right,
                                sessionBar.top + 30.0f),
                    HitKind::ChatSessionNew, colors);
    const data::ChatSession* session = ActiveChatSession();
    const std::wstring sessionTitle =
        session != nullptr && !session->title.empty() ? session->title : std::wstring(L"新对话");
    // 标题按会话栏中两个按钮之间的实际宽度截断，尽量多显示文字。
    renderer_.DrawTextBlock(EllipsizeToWidth(renderer_, sessionTitle, formats.caption.Get(),
                                             (sessionBar.right - 62.0f) -
                                                 (sessionBar.left + 62.0f)),
                            D2D1::RectF(sessionBar.left + 62.0f, sessionBar.top + 4.0f,
                                        sessionBar.right - 62.0f, sessionBar.top + 26.0f),
                            formats.caption.Get(), colors.secondaryText);

    // 对话内容区：逐条消息绘制，超出时可滚动查看。
    const D2D1_RECT_F conversation = ChatConversationRect();
    DrawChatConversation(conversation, colors);

    // 状态行与停止按钮
    // 上下文占用：取最近一条带回传统计的助手消息，按输入 + 输出估算。
    int contextUsed = 0;
    if (session != nullptr) {
        for (auto it = session->messages.rbegin(); it != session->messages.rend(); ++it) {
            if (it->role == L"assistant" &&
                (it->inputTokens > 0 || it->outputTokens > 0)) {
                contextUsed = it->inputTokens + it->outputTokens;
                break;
            }
        }
    }
    const uint32_t contextMax = std::max<uint32_t>(1024, chat.contextTokens);
    const int contextPercent =
        contextUsed > 0
            ? std::clamp(static_cast<int>(std::lround(
                             100.0 * static_cast<double>(contextUsed) / contextMax)),
                         0, 100)
            : 0;
    const auto formatContextTokens = [](int tokens) -> std::wstring {
        if (tokens < 1000) {
            return std::to_wstring(tokens);
        }
        return FormatRatioValue(static_cast<float>(tokens) / 1000.0f) + L"k";
    };

    D2D1_RECT_F statusRect = ChatStatusRect();
    D2D1_RECT_F contextHoverRect{};
    // 生成中「停止生成」按钮占用状态行右侧，此时暂不绘制上下文指示。
    if (contextPercent > 0 && !aiStreaming_) {
        statusRect.right -= 84.0f;
        const D2D1_RECT_F circleRect =
            D2D1::RectF(statusRect.right + 60.0f, statusRect.top + 2.0f,
                        statusRect.right + 84.0f, statusRect.top + 26.0f);
        const D2D1_POINT_2F center = D2D1::Point2F(
            (circleRect.left + circleRect.right) * 0.5f,
            (circleRect.top + circleRect.bottom) * 0.5f);
        constexpr int kSegments = 24;
        const float kPi = 3.14159265f;
        const int filled =
            static_cast<int>(std::lround(contextPercent / 100.0 * kSegments));
        for (int i = 0; i < kSegments; ++i) {
            const float angle = -kPi * 0.5f + kPi * 2.0f * static_cast<float>(i) / kSegments;
            const float outerX = center.x + 10.0f * std::cos(angle);
            const float outerY = center.y + 10.0f * std::sin(angle);
            const float innerX = center.x + 7.0f * std::cos(angle);
            const float innerY = center.y + 7.0f * std::sin(angle);
            renderer_.DrawLine(D2D1::Point2F(innerX, innerY),
                               D2D1::Point2F(outerX, outerY),
                               i < filled ? colors.accent : colors.divider, 1.6f);
        }
        renderer_.DrawTextBlock(std::to_wstring(contextPercent) + L"%",
                                D2D1::RectF(statusRect.right + 8.0f, statusRect.top + 2.0f,
                                            statusRect.right + 56.0f,
                                            statusRect.top + 26.0f),
                                formats.captionRight.Get(), colors.secondaryText);
        contextHoverRect = D2D1::RectF(statusRect.right + 4.0f, statusRect.top,
                                       statusRect.right + 84.0f, statusRect.top + 28.0f);
        AddHit(contextHoverRect, HitKind::ChatContextUsage);
    }
    renderer_.DrawTextBlock(util::Ellipsize(aiStatus_, 40), statusRect,
                            formats.caption.Get(), colors.secondaryText);
    if (aiStreaming_) {
        DrawButton(L"停止生成", ChatStopRect(), HitKind::ChatStop, colors, false, false);
    }
    // 悬停显示已用 / 上限，上限取当前对话配置的上下文最大值。
    if (contextPercent > 0 && !aiStreaming_ && IsHovered(contextHoverRect)) {
        const D2D1_RECT_F tipRect =
            D2D1::RectF(contextHoverRect.right - 116.0f, contextHoverRect.top - 30.0f,
                        contextHoverRect.right, contextHoverRect.top - 4.0f);
        renderer_.FillRoundedRect(tipRect, 6.0f, colors.surface);
        renderer_.StrokeRoundedRect(tipRect, 6.0f, colors.divider, 1.0f);
        renderer_.DrawTextBlock(formatContextTokens(contextUsed) + L"/" +
                                    formatContextTokens(static_cast<int>(contextMax)),
                                tipRect, formats.caption.Get(), colors.text);
    }

    // 问题输入框与发送按钮
    const D2D1_RECT_F inputRect = ChatFieldRect(ChatFocus::Message);
    const bool inputFocused = chatFocus_ == ChatFocus::Message;
    renderer_.FillRoundedRect(inputRect, 9.0f, colors.surface);
    renderer_.StrokeRoundedRect(inputRect, 9.0f,
                                inputFocused ? colors.accent : colors.divider,
                                inputFocused ? 1.6f : 1.0f);
    const D2D1_RECT_F inputTextRect =
        D2D1::RectF(inputRect.left + 10.0f, inputRect.top + 7.0f, inputRect.right - 10.0f,
                    inputRect.bottom - 7.0f);
    if (aiDraft_.empty()) {
        renderer_.DrawTextBlock(L"输入问题，Ctrl+回车发送，回车换行", inputTextRect,
                                formats.listItem.Get(), colors.secondaryText);
    } else {
        // 多行输入：按输入框宽度换行显示，超出高度时裁剪。
        ComPtr<IDWriteTextLayout> inputLayout =
            renderer_.CreateLayout(aiDraft_, formats.listItem.Get(),
                                   inputTextRect.right - inputTextRect.left, 100000.0f);
        if (inputLayout) {
            inputLayout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
            renderer_.PushClip(inputTextRect);
            renderer_.DrawLayout(inputLayout.Get(),
                                 D2D1::Point2F(inputTextRect.left, inputTextRect.top),
                                 colors.text);
            renderer_.PopClip();
        }
    }
    if (inputFocused) {
        DrawInputSelection(inputTextRect, formats.listItem.Get(), true, colors);
    }
    AddHit(inputRect, HitKind::ChatInput);
    DrawButton(L"发送", ChatSendRect(), HitKind::ChatSend, colors, true, false);
}

data::ChatSession* Application::ActiveChatSession() {
    data::BookRecord* record = library_.Find(activeBookId_);
    if (record == nullptr || activeChatSessionId_.empty()) {
        return nullptr;
    }
    for (data::ChatSession& session : record->chatSessions) {
        if (session.id == activeChatSessionId_) {
            return &session;
        }
    }
    return nullptr;
}

data::ChatSession* Application::CreateChatSession() {
    data::BookRecord* record = library_.Find(activeBookId_);
    if (record == nullptr) {
        return nullptr;
    }
    data::ChatSession session;
    session.createdAt = util::CurrentUnixSeconds();
    session.updatedAt = session.createdAt;
    session.title = L"新对话";
    const std::wstring digest =
        util::Sha256Hex(util::ToUtf8(activeBookId_ + L"|" + std::to_wstring(session.createdAt)));
    session.id = L"chat-" + digest.substr(0, 12);
    int suffix = 0;
    while (std::any_of(record->chatSessions.begin(), record->chatSessions.end(),
                       [&session](const data::ChatSession& other) {
                           return other.id == session.id;
                       })) {
        session.id = L"chat-" + digest.substr(0, 12) + L"-" + std::to_wstring(++suffix);
    }
    activeChatSessionId_ = session.id;
    record->chatSessions.push_back(std::move(session));
    return &record->chatSessions.back();
}

data::ChatSession* Application::EnsureChatSession() {
    data::ChatSession* session = ActiveChatSession();
    if (session != nullptr) {
        return session;
    }
    return CreateChatSession();
}

void Application::NewChatSession() {
    if (activeBookId_.empty()) {
        return;
    }
    if (aiStreaming_) {
        StopAiRequest();
    }
    aiAnswer_.clear();
    aiDraft_.clear();
    ResetInputCaretToEnd();
    aiStatus_.clear();
    chatScroll_ = 0.0f;
    chatSessionPickerVisible_ = false;
    CreateChatSession();
    SaveChatState();
    InvalidateRect(hwnd_, nullptr, FALSE);
    UpdateWindow(hwnd_);
}

void Application::DeleteChatSession(const std::wstring& id) {
    data::BookRecord* record = library_.Find(activeBookId_);
    if (record == nullptr) {
        return;
    }
    const bool deletingActive = activeChatSessionId_ == id;
    if (deletingActive && aiStreaming_) {
        StopAiRequest();
    }
    record->chatSessions.erase(
        std::remove_if(record->chatSessions.begin(), record->chatSessions.end(),
                       [&id](const data::ChatSession& session) { return session.id == id; }),
        record->chatSessions.end());
    if (deletingActive) {
        aiAnswer_.clear();
        activeChatSessionId_.clear();
        chatScroll_ = 0.0f;
        // 自动切换到最近更新的会话；全部删除后保持“新对话”的空状态。
        int64_t newestTime = -1;
        for (const data::ChatSession& session : record->chatSessions) {
            if (session.updatedAt >= newestTime) {
                newestTime = session.updatedAt;
                activeChatSessionId_ = session.id;
            }
        }
    }
    SaveChatState();
    InvalidateRect(hwnd_, nullptr, FALSE);
    UpdateWindow(hwnd_);
}

void Application::SelectChatSession(const std::wstring& id) {
    if (aiStreaming_) {
        StopAiRequest();
    }
    aiAnswer_.clear();
    activeChatSessionId_ = id;
    // 切换后默认显示最新消息，绘制时会被钳制到实际最大滚动量。
    chatScroll_ = 1.0e9f;
    chatSessionPickerVisible_ = false;
    InvalidateRect(hwnd_, nullptr, FALSE);
    UpdateWindow(hwnd_);
}

void Application::SaveChatState() {
    if (activeBookId_.empty()) {
        return;
    }
    std::wstring error;
    library_.SaveBook(activeBookId_, error);
}

bool Application::CopyTextToClipboard(const std::wstring& text) {
    if (text.empty()) {
        return false;
    }
    bool copied = false;
    if (OpenClipboard(hwnd_)) {
        EmptyClipboard();
        const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (memory) {
            void* target = GlobalLock(memory);
            if (target) {
                std::memcpy(target, text.c_str(), bytes);
                GlobalUnlock(memory);
                if (SetClipboardData(CF_UNICODETEXT, memory) != nullptr) {
                    copied = true;
                } else {
                    GlobalFree(memory);
                }
            } else {
                GlobalFree(memory);
            }
        }
        CloseClipboard();
    }
    return copied;
}

void Application::CopyChatMessage(int messageIndex) {
    const data::ChatSession* session = ActiveChatSession();
    if (session == nullptr || messageIndex < 0 ||
        messageIndex >= static_cast<int>(session->messages.size())) {
        return;
    }
    if (!CopyTextToClipboard(session->messages[static_cast<size_t>(messageIndex)].text)) {
        aiStatus_ = L"复制失败";
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    // 对话内的复制提示显示在对话面板，不在阅读页底部弹出「已复制」。
    aiStatus_ = L"已复制";
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Application::BuildChatBubbles(const D2D1_RECT_F& rect, std::vector<ChatBubble>& bubbles) {
    bubbles.clear();
    const TextFormats& formats = renderer_.Formats();
    // 右侧预留滚动条的位置。
    const float available = std::max(120.0f, rect.right - rect.left - 34.0f);
    const float bubbleWidth = available * kChatBubbleWidthRatio;
    const float textWidth = std::max(48.0f, bubbleWidth - kChatBubblePadding * 2.0f);
    float cursor = 8.0f;

    // token 数量展示：小于 1000 显示原值，超过时以 k 为单位保留一位小数。
    const auto formatTokens = [](int tokens) -> std::wstring {
        if (tokens <= 0) {
            return L"0";
        }
        if (tokens < 1000) {
            return std::to_wstring(tokens);
        }
        return FormatRatioValue(static_cast<float>(tokens) / 1000.0f) + L"k";
    };

    const auto appendBubble = [&](const std::wstring& text, bool isUser, int messageIndex,
                                  const std::wstring& thinking, const std::wstring& usage) {
        ChatBubble bubble;
        bubble.messageIndex = messageIndex;
        bubble.isUser = isUser;
        bubble.layout =
            renderer_.CreateLayout(text, formats.listItem.Get(), textWidth, 100000.0f);
        float textHeight = 18.0f;
        if (bubble.layout) {
            bubble.layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
            DWRITE_TEXT_METRICS metrics{};
            bubble.layout->GetMetrics(&metrics);
            textHeight = std::max(18.0f, metrics.height);
        }
        // 思考内容默认折叠，展开后占用独立高度。
        float thinkingBlock = 0.0f;
        if (!thinking.empty()) {
            bubble.thinking = thinking;
            bubble.thinkingExpanded = chatThinkingExpanded_.count(messageIndex) > 0;
            bubble.thinkingLayout =
                renderer_.CreateLayout(thinking, formats.caption.Get(), textWidth, 100000.0f);
            if (bubble.thinkingLayout) {
                bubble.thinkingLayout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
                DWRITE_TEXT_METRICS metrics{};
                bubble.thinkingLayout->GetMetrics(&metrics);
                bubble.thinkingHeight = std::max(18.0f, metrics.height);
            }
            thinkingBlock = 18.0f;
            if (bubble.thinkingExpanded) {
                thinkingBlock += bubble.thinkingHeight + 6.0f;
            }
        }
        bubble.height = kChatBubbleHeader + thinkingBlock + textHeight + kChatBubblePadding +
                        (usage.empty() ? 0.0f : 18.0f);
        bubble.top = cursor;
        // 用户消息右对齐，AI 消息左对齐。
        const float left =
            isUser ? rect.right - 12.0f - bubbleWidth : rect.left + 12.0f;
        bubble.bubble = D2D1::RectF(left, cursor, left + bubbleWidth, cursor + bubble.height);
        bubble.thinkingToggleRect =
            D2D1::RectF(left + kChatBubblePadding, cursor + kChatBubbleHeader,
                        left + kChatBubblePadding + 100.0f, cursor + kChatBubbleHeader + 18.0f);
        if (thinkingBlock > 0.0f) {
            bubble.thinkingRect =
                D2D1::RectF(left + kChatBubblePadding, cursor + kChatBubbleHeader + 18.0f,
                            left + bubbleWidth - kChatBubblePadding,
                            cursor + kChatBubbleHeader + 18.0f + bubble.thinkingHeight);
        }
        bubble.usage = usage;
        bubble.usageHeight = usage.empty() ? 0.0f : 18.0f;
        bubble.textRect = D2D1::RectF(
            left + kChatBubblePadding, cursor + kChatBubbleHeader + thinkingBlock,
            left + bubbleWidth - kChatBubblePadding,
            cursor + bubble.height - kChatBubblePadding * 0.4f - bubble.usageHeight);
        if (!usage.empty()) {
            bubble.usageRect =
                D2D1::RectF(left + kChatBubblePadding, cursor + bubble.height - 18.0f,
                            left + bubbleWidth - kChatBubblePadding,
                            cursor + bubble.height - 2.0f);
        }
        cursor += bubble.height + kChatBubbleGap;
        bubbles.push_back(std::move(bubble));
    };

    const data::ChatSession* session = ActiveChatSession();
    if (session != nullptr) {
        for (size_t i = 0; i < session->messages.size(); ++i) {
            const data::ChatMessage& message = session->messages[i];
            std::wstring usage;
            if (message.role == L"assistant" &&
                (message.inputTokens > 0 || message.outputTokens > 0)) {
                usage = L"输入 " + formatTokens(message.inputTokens) + L" · 缓存 " +
                        formatTokens(message.cachedTokens) + L" · 输出 " +
                        formatTokens(message.outputTokens);
            }
            appendBubble(message.text, message.role == L"user", static_cast<int>(i),
                         message.thinking, usage);
        }
    }
    if (aiStreaming_) {
        const std::wstring streamingText =
            aiAnswer_.empty() ? std::wstring(L"正在生成…") : aiAnswer_;
        appendBubble(streamingText, false, -1, aiThinking_, std::wstring());
    }
}

uint32_t Application::ChatTextPositionAt(const ChatBubble& bubble, float x, float y) const {
    if (!bubble.layout) {
        return 0;
    }
    const float width = std::max(1.0f, bubble.textRect.right - bubble.textRect.left);
    const float height = std::max(1.0f, bubble.textRect.bottom - bubble.textRect.top);
    const float localX = std::clamp(x - bubble.textRect.left, 0.0f, width - 1.0f);
    const float localY = std::clamp(y - bubble.textRect.top, 0.0f, height - 1.0f);
    BOOL trailing = FALSE;
    BOOL inside = FALSE;
    DWRITE_HIT_TEST_METRICS metrics{};
    if (FAILED(bubble.layout->HitTestPoint(localX, localY, &trailing, &inside, &metrics))) {
        return 0;
    }
    uint32_t position = metrics.textPosition;
    if (trailing != FALSE && inside != FALSE) {
        ++position;
    }
    return position;
}

void Application::DrawChatSelection(const ChatBubble& bubble, const D2D1_RECT_F& textRect,
                                    const ThemeColors& colors) {
    if (!bubble.layout || chatSelectionMessage_ != bubble.messageIndex) {
        return;
    }
    const uint32_t begin = std::min(chatSelectionStart_, chatSelectionEnd_);
    const uint32_t end = std::max(chatSelectionStart_, chatSelectionEnd_);
    if (begin >= end) {
        return;
    }
    UINT32 count = 0;
    bubble.layout->HitTestTextRange(begin, end - begin, 0.0f, 0.0f, nullptr, 0, &count);
    if (count == 0) {
        return;
    }
    std::vector<DWRITE_HIT_TEST_METRICS> metrics(count);
    if (FAILED(bubble.layout->HitTestTextRange(begin, end - begin, 0.0f, 0.0f, metrics.data(),
                                              count, &count))) {
        return;
    }
    const D2D1_COLOR_F selectionColor = ParseHexColor(L"#4A90D9", 0.35f);
    for (UINT32 i = 0; i < count; ++i) {
        renderer_.FillRect(D2D1::RectF(textRect.left + metrics[i].left,
                                       textRect.top + metrics[i].top,
                                       textRect.left + metrics[i].left + metrics[i].width,
                                       textRect.top + metrics[i].top + metrics[i].height),
                           selectionColor);
    }
}

bool Application::BeginChatSelection(float x, float y) {
    const D2D1_RECT_F rect = ChatConversationRect();
    std::vector<ChatBubble> bubbles;
    BuildChatBubbles(rect, bubbles);
    const float offset = rect.top - chatScroll_;
    for (const ChatBubble& bubble : bubbles) {
        if (bubble.messageIndex < 0 || !bubble.layout) {
            continue;
        }
        ChatBubble absolute = bubble;
        absolute.bubble = OffsetRect(bubble.bubble, 0.0f, offset);
        absolute.textRect = OffsetRect(bubble.textRect, 0.0f, offset);
        if (!PointInside(absolute.textRect, x, y)) {
            continue;
        }
        const uint32_t position = ChatTextPositionAt(absolute, x, y);
        chatSelectionMessage_ = bubble.messageIndex;
        chatSelectionStart_ = position;
        chatSelectionEnd_ = position;
        chatSelecting_ = true;
        SetCapture(hwnd_);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return true;
    }
    ClearChatSelection();
    InvalidateRect(hwnd_, nullptr, FALSE);
    return false;
}

void Application::UpdateChatSelection(float x, float y) {
    if (!chatSelecting_ || chatSelectionMessage_ < 0) {
        return;
    }
    const D2D1_RECT_F rect = ChatConversationRect();
    std::vector<ChatBubble> bubbles;
    BuildChatBubbles(rect, bubbles);
    const float offset = rect.top - chatScroll_;
    for (const ChatBubble& bubble : bubbles) {
        if (bubble.messageIndex != chatSelectionMessage_) {
            continue;
        }
        ChatBubble absolute = bubble;
        absolute.bubble = OffsetRect(bubble.bubble, 0.0f, offset);
        absolute.textRect = OffsetRect(bubble.textRect, 0.0f, offset);
        const uint32_t position = ChatTextPositionAt(absolute, x, y);
        if (position != chatSelectionEnd_) {
            chatSelectionEnd_ = position;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return;
    }
}

void Application::ClearChatSelection() {
    chatSelecting_ = false;
    chatSelectionMessage_ = -1;
    chatSelectionStart_ = 0;
    chatSelectionEnd_ = 0;
    if (GetCapture() == hwnd_) {
        ReleaseCapture();
    }
}

bool Application::CopyChatSelection() {
    const data::ChatSession* session = ActiveChatSession();
    if (session == nullptr || chatSelectionMessage_ < 0 ||
        chatSelectionMessage_ >= static_cast<int>(session->messages.size())) {
        return false;
    }
    const uint32_t begin = std::min(chatSelectionStart_, chatSelectionEnd_);
    const uint32_t end = std::max(chatSelectionStart_, chatSelectionEnd_);
    const std::wstring& text = session->messages[static_cast<size_t>(chatSelectionMessage_)].text;
    if (begin >= end || begin >= text.size()) {
        return false;
    }
    const std::wstring selected = text.substr(begin, std::min<uint32_t>(end, static_cast<uint32_t>(text.size())) - begin);
    if (!CopyTextToClipboard(selected)) {
        aiStatus_ = L"复制失败";
        InvalidateRect(hwnd_, nullptr, FALSE);
        return true;
    }
    // 提示显示在对话面板内，不使用阅读页底部的「已复制」提示。
    aiStatus_ = L"已复制";
    InvalidateRect(hwnd_, nullptr, FALSE);
    return true;
}

void Application::DrawChatConversation(const D2D1_RECT_F& rect, const ThemeColors& colors) {
    const TextFormats& formats = renderer_.Formats();
    // 对话区与 AI 输出使用同一种底色，避免灰底与气泡之间出现明显色差。
    renderer_.FillRoundedRect(rect, 9.0f, colors.surface);

    std::vector<ChatBubble> bubbles;
    BuildChatBubbles(rect, bubbles);
    if (bubbles.empty()) {
        renderer_.DrawTextBlock(aiStatus_.empty() ? L"输入问题后发送，回复将流式显示。" : aiStatus_,
                                D2D1::RectF(rect.left + 12.0f, rect.top + 12.0f,
                                            rect.right - 12.0f, rect.top + 36.0f),
                                formats.caption.Get(), colors.secondaryText);
        chatScrollMax_ = 0.0f;
        chatScroll_ = 0.0f;
        return;
    }

    const float contentHeight = bubbles.back().top + bubbles.back().height + 8.0f;
    chatScrollMax_ = std::max(0.0f, contentHeight - (rect.bottom - rect.top));
    if (aiStreaming_) {
        // 流式输出时始终跟随最新内容。
        chatScroll_ = chatScrollMax_;
    }
    chatScroll_ = std::clamp(chatScroll_, 0.0f, chatScrollMax_);
    const float offset = rect.top - chatScroll_;

    // 自动追加的正文读取消息属于特殊内容，使用不同的背景色区分。
    const data::ChatSession* session = ActiveChatSession();
    const auto isSpecialMessage = [session](int index) {
        if (session == nullptr || index < 0 ||
            index >= static_cast<int>(session->messages.size())) {
            return false;
        }
        const std::wstring& text = session->messages[static_cast<size_t>(index)].text;
        return text.rfind(L"【正文范围", 0) == 0 || text.rfind(L"【已读取正文", 0) == 0;
    };
    // 助手消息标题附带该条回复实际使用的模型，切换接口后旧回复仍可辨认来源。
    const auto assistantLabel = [session](int index) {
        if (session == nullptr || index < 0 ||
            index >= static_cast<int>(session->messages.size())) {
            return std::wstring(L"AI");
        }
        const data::ChatMessage& message = session->messages[static_cast<size_t>(index)];
        const std::wstring& source =
            message.aiModel.empty() ? message.aiConfigName : message.aiModel;
        if (source.empty()) {
            return std::wstring(L"AI");
        }
        return L"AI · " + util::Ellipsize(source, 40);
    };

    BeginClip(rect);
    bool firstVisibleBubble = true;
    for (const ChatBubble& bubble : bubbles) {
        const D2D1_RECT_F bubbleRect = OffsetRect(bubble.bubble, 0.0f, offset);
        if (bubbleRect.bottom < rect.top || bubbleRect.top > rect.bottom) {
            continue;
        }
        const D2D1_RECT_F textRect = OffsetRect(bubble.textRect, 0.0f, offset);
        // 每次对话之间用一条横线分隔。
        if (!firstVisibleBubble) {
            renderer_.DrawLine(
                D2D1::Point2F(rect.left + 12.0f, bubbleRect.top - kChatBubbleGap * 0.5f),
                D2D1::Point2F(rect.right - 12.0f, bubbleRect.top - kChatBubbleGap * 0.5f),
                colors.divider, 1.0f);
        }
        firstVisibleBubble = false;
        if (bubble.isUser) {
            // 用户消息使用圆角框；自动读取正文的消息改用强调色的浅色底。
            const bool special = isSpecialMessage(bubble.messageIndex);
            const D2D1_COLOR_F fill =
                special ? D2D1::ColorF(colors.accent.r, colors.accent.g, colors.accent.b, 0.10f)
                        : colors.surfaceAlt;
            renderer_.FillRoundedRect(bubbleRect, 10.0f, fill);
            renderer_.StrokeRoundedRect(bubbleRect, 10.0f, colors.divider, 1.0f);
        }
        renderer_.DrawTextBlock(bubble.isUser ? L"你" : assistantLabel(bubble.messageIndex),
                                D2D1::RectF(bubbleRect.left + 14.0f, bubbleRect.top + 4.0f,
                                            bubbleRect.right - 14.0f,
                                            bubbleRect.top + kChatBubbleHeader + 4.0f),
                                formats.caption.Get(),
                                bubble.isUser ? colors.accent : colors.secondaryText);
        if (!bubble.thinking.empty()) {
            // 思考过程：点击标题行展开或折叠。
            const D2D1_RECT_F toggleRect = OffsetRect(bubble.thinkingToggleRect, 0.0f, offset);
            renderer_.DrawTextBlock(bubble.thinkingExpanded ? L"思考过程 ▴" : L"思考过程 ▾",
                                    toggleRect, formats.caption.Get(), colors.secondaryText);
            AddHit(toggleRect, HitKind::ChatThinkingToggle, bubble.messageIndex);
            if (bubble.thinkingExpanded && bubble.thinkingLayout) {
                const D2D1_RECT_F thinkingRect = OffsetRect(bubble.thinkingRect, 0.0f, offset);
                // 思考内容使用独立底色，与正文区分。
                renderer_.FillRoundedRect(
                    D2D1::RectF(thinkingRect.left - 6.0f, thinkingRect.top - 4.0f,
                                thinkingRect.right + 6.0f, thinkingRect.bottom + 4.0f),
                    6.0f, colors.surfaceAlt);
                renderer_.DrawLayout(bubble.thinkingLayout.Get(),
                                     D2D1::Point2F(thinkingRect.left, thinkingRect.top),
                                     colors.secondaryText);
            }
        }
        if (bubble.layout) {
            DrawChatSelection(bubble, textRect, colors);
            renderer_.DrawLayout(bubble.layout.Get(),
                                 D2D1::Point2F(textRect.left, textRect.top), colors.text);
        }
        if (!bubble.usage.empty()) {
            renderer_.DrawTextBlock(bubble.usage, OffsetRect(bubble.usageRect, 0.0f, offset),
                                    formats.caption.Get(), colors.secondaryText);
        }
    }
    EndClip();

    // 对话内容滚动条：内容超出可视高度时显示，可拖动定位。
    if (chatScrollMax_ > 0.0f) {
        const D2D1_RECT_F track = ChatScrollbarTrackRect();
        renderer_.FillRoundedRect(track, 4.0f, colors.divider);
        const float trackHeight = track.bottom - track.top;
        const float contentHeight = trackHeight + chatScrollMax_;
        const float thumbHeight = std::max(28.0f, trackHeight * trackHeight / contentHeight);
        const float ratio = chatScroll_ / chatScrollMax_;
        const float thumbTop = track.top + (trackHeight - thumbHeight) * ratio;
        renderer_.FillRoundedRect(
            D2D1::RectF(track.left, thumbTop, track.right, thumbTop + thumbHeight), 4.0f,
            colors.accent);
        AddHit(D2D1::RectF(track.left - 2.0f, track.top, track.right + 2.0f, track.bottom),
               HitKind::ChatScrollbar);
    }
}

void Application::AskAssistantWithSelection() {
    // 优先取 PDF 页面选择，其次是正文字流选择。
    std::wstring text;
    if (fixedPages_.IsOpen() && fixedPages_.HasSelection()) {
        text = fixedPages_.SelectedText();
    } else {
        text = SelectedReaderText();
    }
    text = TrimNoteText(text);
    if (text.empty()) {
        ShowStatus(L"请先选中要询问的文字");
        return;
    }
    if (activeBookId_.empty()) {
        ShowStatus(L"请先打开一本书");
        return;
    }
    // 过长的整段选择会超出接口上限，截断后再提问。
    constexpr size_t kMaximumPromptCharacters = 6000;
    if (text.size() > kMaximumPromptCharacters) {
        text.resize(kMaximumPromptCharacters);
        text += L"…";
    }
    if (aiStreaming_) {
        StopAiRequest();
    }
    ChangePanel(PanelMode::Chat);
    // 每次提问都从新对话开始，选择内容的上下文不与之前的对话混合。
    NewChatSession();
    StartAiRequest(L"请解释下面这段内容：\n\n" + text);
}

D2D1_RECT_F Application::ChatSessionPickerPanelRect() const {
    const float width = std::min(480.0f, widthDip_ - 80.0f);
    const float height = std::min(440.0f, heightDip_ - 100.0f);
    const float left = (widthDip_ - width) * 0.5f;
    const float top = (heightDip_ - height) * 0.5f;
    return D2D1::RectF(left, top, left + width, top + height);
}

D2D1_RECT_F Application::ChatSessionPickerListRect() const {
    const D2D1_RECT_F panel = ChatSessionPickerPanelRect();
    return D2D1::RectF(panel.left + 20.0f, panel.top + 76.0f, panel.right - 20.0f,
                       panel.bottom - 16.0f);
}

D2D1_RECT_F Application::ShortcutPanelRect() const {
    const float width = std::min(470.0f, widthDip_ - 80.0f);
    const float height = std::min(420.0f, heightDip_ - 100.0f);
    const float left = (widthDip_ - width) * 0.5f;
    const float top = (heightDip_ - height) * 0.5f;
    return D2D1::RectF(left, top, left + width, top + height);
}

void Application::OpenShortcutSettings() {
    shortcutSettingsVisible_ = true;
    shortcutCaptureIndex_ = -1;
    InvalidateRect(hwnd_, nullptr, FALSE);
    UpdateWindow(hwnd_);
}

void Application::CloseShortcutSettings() {
    shortcutSettingsVisible_ = false;
    shortcutCaptureIndex_ = -1;
    InvalidateRect(hwnd_, nullptr, FALSE);
    UpdateWindow(hwnd_);
}

void Application::AssignShortcut(UINT virtualKey) {
    if (shortcutCaptureIndex_ < 0 ||
        shortcutCaptureIndex_ >= static_cast<int>(kShortcutRowCount)) {
        return;
    }
    if (VkFromKeyName(KeyNameFromVk(virtualKey)) == 0) {
        return;
    }
    const std::wstring combo = ShortcutFromKey(virtualKey);
    const std::wstring action = kShortcutRows[shortcutCaptureIndex_].action;
    data::ReaderSettings& settings = library_.Settings();
    bool updated = false;
    for (data::ShortcutBinding& binding : settings.shortcuts) {
        if (binding.action == action) {
            binding.combo = combo;
            updated = true;
            break;
        }
    }
    if (!updated) {
        settings.shortcuts.push_back(data::ShortcutBinding{action, combo});
    }
    shortcutCaptureIndex_ = -1;
    std::wstring error;
    library_.Save(error);
    InvalidateRect(hwnd_, nullptr, FALSE);
    UpdateWindow(hwnd_);
}

void Application::ResetShortcuts() {
    library_.Settings().shortcuts = data::DefaultShortcuts();
    shortcutCaptureIndex_ = -1;
    std::wstring error;
    library_.Save(error);
    InvalidateRect(hwnd_, nullptr, FALSE);
    UpdateWindow(hwnd_);
}

void Application::DrawShortcutSettings(const ThemeColors& colors) {
    if (!shortcutSettingsVisible_) {
        return;
    }
    const TextFormats& formats = renderer_.Formats();
    const data::ReaderSettings& settings = library_.Settings();
    renderer_.FillRect(D2D1::RectF(0.0f, 0.0f, widthDip_, heightDip_),
                       ParseHexColor(L"#000000", 0.35f));

    const D2D1_RECT_F panel = ShortcutPanelRect();
    renderer_.FillRoundedRect(panel, 12.0f, colors.surface);
    renderer_.StrokeRoundedRect(panel, 12.0f, colors.divider, 1.0f);
    renderer_.DrawTextBlock(L"快捷键映射",
                            D2D1::RectF(panel.left + 24.0f, panel.top + 16.0f,
                                        panel.right - 150.0f, panel.top + 52.0f),
                            formats.title.Get(), colors.text);
    DrawButton(L"恢复默认",
               D2D1::RectF(panel.right - 136.0f, panel.top + 14.0f, panel.right - 24.0f,
                           panel.top + 46.0f),
               HitKind::ShortcutReset, colors, false, false);
    renderer_.DrawLine(D2D1::Point2F(panel.left + 24.0f, panel.top + 60.0f),
                       D2D1::Point2F(panel.right - 24.0f, panel.top + 60.0f), colors.divider,
                       1.0f);

    const std::vector<data::ShortcutBinding> defaults = data::DefaultShortcuts();
    const auto defaultCombo = [&defaults](const wchar_t* action) {
        for (const data::ShortcutBinding& binding : defaults) {
            if (binding.action == action) {
                return binding.combo;
            }
        }
        return std::wstring();
    };

    float rowTop = panel.top + 72.0f;
    for (size_t i = 0; i < kShortcutRowCount; ++i) {
        const D2D1_RECT_F rowRect =
            D2D1::RectF(panel.left + 16.0f, rowTop, panel.right - 16.0f, rowTop + 40.0f);
        renderer_.DrawTextBlock(kShortcutRows[i].label,
                                D2D1::RectF(rowRect.left + 10.0f, rowRect.top,
                                            rowRect.right - 170.0f, rowRect.bottom),
                                formats.listItem.Get(), colors.text);
        const D2D1_RECT_F comboRect =
            D2D1::RectF(rowRect.right - 160.0f, rowRect.top + 5.0f, rowRect.right - 8.0f,
                        rowRect.top + 35.0f);
        const bool capturing =
            shortcutCaptureIndex_ >= 0 &&
            shortcutCaptureIndex_ == static_cast<int>(i);
        renderer_.FillRoundedRect(comboRect, 7.0f, colors.surfaceAlt);
        renderer_.StrokeRoundedRect(comboRect, 7.0f,
                                    capturing ? colors.accent : colors.divider,
                                    capturing ? 1.6f : 1.0f);
        const std::wstring text =
            capturing ? std::wstring(L"按下新的组合键…")
                      : ShortcutCombo(settings, kShortcutRows[i].action,
                                      defaultCombo(kShortcutRows[i].action).c_str());
        renderer_.DrawTextBlock(text, comboRect, formats.centered.Get(), colors.text);
        AddHit(rowRect, HitKind::ShortcutRow, static_cast<int>(i));
        rowTop += 44.0f;
    }

    renderer_.DrawTextBlock(
        L"点击一行后按下新的组合键，Esc 取消。上一页/下一页另支持 ↑ ↓ PageUp PageDown Space。",
        D2D1::RectF(panel.left + 24.0f, rowTop + 2.0f, panel.right - 24.0f, rowTop + 24.0f),
        formats.caption.Get(), colors.secondaryText);
    DrawButton(L"关闭",
               D2D1::RectF(panel.right - 136.0f, panel.bottom - 50.0f, panel.right - 24.0f,
                           panel.bottom - 14.0f),
               HitKind::ShortcutClose, colors, false, false);
}

void Application::DrawChatSessionPicker(const ThemeColors& colors) {
    if (!chatSessionPickerVisible_) {
        return;
    }
    const TextFormats& formats = renderer_.Formats();
    renderer_.FillRect(D2D1::RectF(0.0f, 0.0f, widthDip_, heightDip_),
                       ParseHexColor(L"#000000", 0.35f));

    const D2D1_RECT_F panel = ChatSessionPickerPanelRect();
    renderer_.FillRoundedRect(panel, 12.0f, colors.surface);
    renderer_.StrokeRoundedRect(panel, 12.0f, colors.divider, 1.0f);
    renderer_.DrawTextBlock(L"对话记录",
                            D2D1::RectF(panel.left + 24.0f, panel.top + 16.0f,
                                        panel.right - 70.0f, panel.top + 52.0f),
                            formats.title.Get(), colors.text);
    DrawButton(L"×",
               D2D1::RectF(panel.right - 52.0f, panel.top + 14.0f, panel.right - 18.0f,
                           panel.top + 46.0f),
               HitKind::ChatSessionClose, colors, false, false);
    renderer_.DrawLine(D2D1::Point2F(panel.left + 24.0f, panel.top + 60.0f),
                       D2D1::Point2F(panel.right - 24.0f, panel.top + 60.0f), colors.divider,
                       1.0f);

    const D2D1_RECT_F listRect = ChatSessionPickerListRect();
    const data::BookRecord* record = library_.Find(activeBookId_);
    if (record == nullptr || record->chatSessions.empty()) {
        renderer_.DrawTextBlock(L"还没有对话记录。",
                                D2D1::RectF(listRect.left, listRect.top, listRect.right,
                                            listRect.top + 24.0f),
                                formats.caption.Get(), colors.secondaryText);
        chatSessionPickerScrollMax_ = 0.0f;
        chatSessionPickerScroll_ = 0.0f;
        return;
    }

    // 按更新时间倒序显示，最近对话在最上方。
    std::vector<const data::ChatSession*> ordered;
    ordered.reserve(record->chatSessions.size());
    for (const data::ChatSession& session : record->chatSessions) {
        ordered.push_back(&session);
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const data::ChatSession* first, const data::ChatSession* second) {
                  return first->updatedAt > second->updatedAt;
              });

    constexpr float kRowHeight = 62.0f;
    const float contentHeight = static_cast<float>(ordered.size()) * kRowHeight;
    chatSessionPickerScrollMax_ =
        std::max(0.0f, contentHeight - (listRect.bottom - listRect.top));
    chatSessionPickerScroll_ =
        std::clamp(chatSessionPickerScroll_, 0.0f, chatSessionPickerScrollMax_);

    BeginClip(listRect);
    for (size_t i = 0; i < ordered.size(); ++i) {
        const data::ChatSession* session = ordered[i];
        const float top = listRect.top - chatSessionPickerScroll_ +
                          static_cast<float>(i) * kRowHeight;
        const D2D1_RECT_F rowRect =
            D2D1::RectF(listRect.left, top, listRect.right, top + kRowHeight - 6.0f);
        const bool current = session->id == activeChatSessionId_;
        if (current) {
            renderer_.FillRoundedRect(rowRect, 8.0f, colors.surfaceAlt);
            renderer_.FillRect(D2D1::RectF(rowRect.left, rowRect.top + 10.0f,
                                           rowRect.left + 3.0f, rowRect.bottom - 10.0f),
                               colors.accent);
        } else if (IsHovered(rowRect)) {
            renderer_.FillRoundedRect(rowRect, 8.0f, colors.surfaceAlt);
        }
        renderer_.DrawTextBlock(
            EllipsizeToWidth(renderer_, session->title, formats.listItem.Get(),
                             (rowRect.right - 66.0f) - (rowRect.left + 14.0f)),
            D2D1::RectF(rowRect.left + 14.0f, rowRect.top + 9.0f, rowRect.right - 66.0f,
                        rowRect.top + 33.0f),
            formats.listItem.Get(), current ? colors.accent : colors.text);
        // 会话列表同时显示最近一条回复使用的模型，便于区分不同 AI 的对话记录。
        std::wstring lastAi;
        for (auto it = session->messages.rbegin(); it != session->messages.rend(); ++it) {
            if (it->role == L"assistant" &&
                (!it->aiModel.empty() || !it->aiConfigName.empty())) {
                lastAi = it->aiModel.empty() ? it->aiConfigName : it->aiModel;
                break;
            }
        }
        std::wstring subtitle = std::to_wstring(session->messages.size()) + L" 条消息";
        if (!lastAi.empty()) {
            subtitle += L" · " + lastAi;
        }
        subtitle += L" · " + util::FormatDateTime(session->updatedAt);
        renderer_.DrawTextBlock(
            EllipsizeToWidth(renderer_, subtitle, formats.caption.Get(),
                             (rowRect.right - 66.0f) - (rowRect.left + 14.0f)),
            D2D1::RectF(rowRect.left + 14.0f, rowRect.top + 33.0f, rowRect.right - 66.0f,
                        rowRect.top + 53.0f),
            formats.caption.Get(), colors.secondaryText);
        const D2D1_RECT_F deleteRect =
            D2D1::RectF(rowRect.right - 46.0f, rowRect.top + 14.0f, rowRect.right - 12.0f,
                        rowRect.top + 48.0f);
        // 先登记整行命中，再登记删除按钮：命中测试从后往前，按钮优先于整行。
        AddHit(rowRect, HitKind::ChatSessionItem, -1, session->id);
        if (IsHovered(deleteRect)) {
            renderer_.FillRoundedRect(deleteRect, 7.0f, colors.surfaceAlt);
        }
        DrawTrashIcon(renderer_, deleteRect, colors.secondaryText);
        AddHit(deleteRect, HitKind::ChatSessionDelete, -1, session->id);
    }
    EndClip();
}

void Application::DrawAiSettings(const ThemeColors& colors) {
    if (!aiSettingsVisible_) {
        return;
    }
    const TextFormats& formats = renderer_.Formats();
    const data::ReaderSettings& settings = library_.Settings();
    renderer_.FillRect(D2D1::RectF(0.0f, 0.0f, widthDip_, heightDip_),
                       ParseHexColor(L"#000000", 0.35f));

    const D2D1_RECT_F panel = AiSettingsPanelRect();
    renderer_.FillRoundedRect(panel, 12.0f, colors.surface);
    renderer_.StrokeRoundedRect(panel, 12.0f, colors.divider, 1.0f);
    renderer_.DrawTextBlock(L"AI 接口设置",
                            D2D1::RectF(panel.left + 24.0f, panel.top + 16.0f,
                                        panel.right - 24.0f, panel.top + 58.0f),
                            formats.title.Get(), colors.text);
    renderer_.DrawLine(D2D1::Point2F(panel.left + 24.0f, panel.top + 66.0f),
                       D2D1::Point2F(panel.right - 24.0f, panel.top + 66.0f), colors.divider,
                       1.0f);

    // 分页：对话接口 / 语音合成 / 图像生成 / 视频生成。
    struct TabDef {
        const wchar_t* label;
        HitKind kind;
    };
    const TabDef tabs[] = {
        {L"对话接口", HitKind::AiTabChat},
        {L"语音合成", HitKind::AiTabSpeech},
        {L"图像生成", HitKind::AiTabImage},
        {L"视频生成", HitKind::AiTabVideo},
    };
    for (int i = 0; i < 4; ++i) {
        const float left = panel.left + 24.0f + static_cast<float>(i) * 148.0f;
        DrawButton(tabs[i].label,
                   D2D1::RectF(left, panel.top + 76.0f, left + 140.0f, panel.top + 106.0f),
                   tabs[i].kind, colors, false, aiSettingsTab_ == i, i);
    }

    const auto drawField = [&](ChatFocus focus, const wchar_t* label, HitKind kind,
                               const std::wstring& value, bool mask) {
        const D2D1_RECT_F boxRect = AiSettingsFieldRect(focus);
        renderer_.DrawTextBlock(label,
                                D2D1::RectF(boxRect.left, boxRect.top - 20.0f, boxRect.right,
                                            boxRect.top - 2.0f),
                                formats.caption.Get(), colors.secondaryText);
        const bool focused = chatFocus_ == focus;
        renderer_.FillRoundedRect(boxRect, 8.0f, colors.surfaceAlt);
        renderer_.StrokeRoundedRect(boxRect, 8.0f, focused ? colors.accent : colors.divider,
                                    focused ? 1.6f : 1.0f);
        const std::wstring shown = mask ? std::wstring(value.size(), L'\u2022') : value;
        const D2D1_RECT_F textRect =
            D2D1::RectF(boxRect.left + 10.0f, boxRect.top + 5.0f, boxRect.right - 10.0f,
                        boxRect.bottom - 5.0f);
        renderer_.DrawTextBlock(shown, textRect, formats.listItem.Get(),
                                value.empty() ? colors.secondaryText : colors.text);
        if (focused && !mask) {
            DrawInputSelection(textRect, formats.listItem.Get(), false, colors);
        }
        AddHit(boxRect, kind);
    };

    if (aiSettingsTab_ == 0) {
        const data::AiChatConfig& chat = data::ActiveAiChat(settings);
        size_t configIndex = 0;
        for (size_t i = 0; i < settings.aiChatConfigs.size(); ++i) {
            if (settings.aiChatConfigs[i].id == settings.activeAiChatId) {
                configIndex = i;
                break;
            }
        }
        const D2D1_RECT_F bar = D2D1::RectF(panel.left + 24.0f, panel.top + 112.0f,
                                           panel.right - 24.0f, panel.top + 146.0f);
        renderer_.FillRoundedRect(bar, 8.0f, colors.surfaceAlt);
        const std::wstring configTitle =
            chat.name + L"（" + std::to_wstring(configIndex + 1) + L" / " +
            std::to_wstring(settings.aiChatConfigs.size()) + L"）";
        renderer_.DrawTextBlock(util::Ellipsize(configTitle, 28),
                                D2D1::RectF(bar.left + 12.0f, bar.top + 6.0f, bar.right - 244.0f,
                                            bar.bottom - 6.0f),
                                formats.listItem.Get(), colors.text);
        DrawPlainButton(L"‹", D2D1::RectF(bar.right - 232.0f, bar.top + 4.0f,
                                         bar.right - 200.0f, bar.bottom - 4.0f),
                        HitKind::AiConfigPrev, colors);
        DrawPlainButton(L"›", D2D1::RectF(bar.right - 196.0f, bar.top + 4.0f,
                                         bar.right - 164.0f, bar.bottom - 4.0f),
                        HitKind::AiConfigNext, colors);
        DrawPlainButton(L"新增", D2D1::RectF(bar.right - 156.0f, bar.top + 4.0f,
                                           bar.right - 84.0f, bar.bottom - 4.0f),
                        HitKind::AiConfigAdd, colors);
        DrawPlainButton(L"删除", D2D1::RectF(bar.right - 76.0f, bar.top + 4.0f,
                                           bar.right - 8.0f, bar.bottom - 4.0f),
                        HitKind::AiConfigDelete, colors);

        const bool anthropic = chat.protocol == L"anthropic";
        renderer_.DrawTextBlock(L"接口协议",
                                D2D1::RectF(panel.left + 24.0f, panel.top + 154.0f,
                                            panel.right - 24.0f, panel.top + 170.0f),
                                formats.caption.Get(), colors.secondaryText);
        DrawButton(L"OpenAI 兼容",
                   D2D1::RectF(panel.left + 24.0f, panel.top + 174.0f, panel.left + 174.0f,
                               panel.top + 204.0f),
                   HitKind::ChatProtocolOpenAi, colors, false, !anthropic);
        DrawButton(L"Anthropic",
                   D2D1::RectF(panel.left + 184.0f, panel.top + 174.0f, panel.left + 334.0f,
                               panel.top + 204.0f),
                   HitKind::ChatProtocolAnthropic, colors, false, anthropic);

        drawField(ChatFocus::Name, L"配置名称", HitKind::AiConfigName, chat.name, false);
        drawField(ChatFocus::BaseUrl, L"接口地址", HitKind::ChatBaseUrl, chat.baseUrl, false);
        drawField(ChatFocus::ApiKey, L"接口密钥", HitKind::ChatApiKey, chat.apiKey, true);
        drawField(ChatFocus::Model, L"模型名称", HitKind::ChatModel, chat.model, false);

        renderer_.DrawTextBlock(L"思考等级",
                                D2D1::RectF(panel.left + 24.0f, panel.top + 408.0f,
                                            panel.right - 24.0f, panel.top + 424.0f),
                                formats.caption.Get(), colors.secondaryText);
        static const wchar_t kThinkingLabels[4][8] = {L"关闭", L"低", L"中", L"高"};
        for (int i = 0; i < 4; ++i) {
            const float left = panel.left + 24.0f + static_cast<float>(i) * 82.0f;
            DrawButton(kThinkingLabels[i],
                       D2D1::RectF(left, panel.top + 428.0f, left + 74.0f, panel.top + 458.0f),
                       HitKind::AiThinkingLevel, colors, false, chat.thinkingLevel == i, i);
        }
        drawField(ChatFocus::ContextTokens, L"上下文最大值（token）", HitKind::ChatContextTokens,
                  aiContextTokensDraft_, false);
    } else if (aiSettingsTab_ == 1) {
        renderer_.DrawTextBlock(L"朗读引擎",
                                D2D1::RectF(panel.left + 24.0f, panel.top + 154.0f,
                                            panel.right - 24.0f, panel.top + 170.0f),
                                formats.caption.Get(), colors.secondaryText);
        const bool aiSpeech = settings.speechEngine == L"ai";
        DrawButton(L"内置语音",
                   D2D1::RectF(panel.left + 24.0f, panel.top + 174.0f, panel.left + 174.0f,
                               panel.top + 204.0f),
                   HitKind::AiSpeechEngineBuiltin, colors, false, !aiSpeech);
        DrawButton(L"AI 语音",
                   D2D1::RectF(panel.left + 184.0f, panel.top + 174.0f, panel.left + 334.0f,
                               panel.top + 204.0f),
                   HitKind::AiSpeechEngineAi, colors, false, aiSpeech);
        if (aiSpeech) {
            drawField(ChatFocus::SpeechBaseUrl, L"接口地址", HitKind::AiSpeechBaseUrl,
                      settings.aiSpeech.baseUrl, false);
            drawField(ChatFocus::SpeechApiKey, L"接口密钥", HitKind::AiSpeechApiKey,
                      settings.aiSpeech.apiKey, true);
            drawField(ChatFocus::SpeechModel, L"模型名称", HitKind::AiSpeechModel,
                      settings.aiSpeech.model, false);
            drawField(ChatFocus::SpeechVoice, L"音色", HitKind::AiSpeechVoice,
                      settings.aiSpeech.voice, false);
            renderer_.DrawTextBlock(
                L"AI 语音按 OpenAI 兼容 /audio/speech 接口合成（WAV 格式播放）。\n"
                L"接口地址或密钥留空时，沿用当前对话接口配置。",
                D2D1::RectF(panel.left + 24.0f, panel.top + 412.0f, panel.right - 24.0f,
                            panel.top + 462.0f),
                formats.caption.Get(), colors.secondaryText);
        } else {
            renderer_.DrawTextBlock(
                L"当前使用系统内置语音朗读，无需填写 AI 接口地址与密钥。\n"
                L"切换到 AI 语音后将在此处显示接口配置。",
                D2D1::RectF(panel.left + 24.0f, panel.top + 232.0f, panel.right - 24.0f,
                            panel.top + 300.0f),
                formats.caption.Get(), colors.secondaryText);
        }
    } else if (aiSettingsTab_ == 2 || aiSettingsTab_ == 3) {
        const bool video = aiSettingsTab_ == 3;
        const data::AiServiceConfig& service = video ? settings.aiVideo : settings.aiImage;
        drawField(video ? ChatFocus::VideoBaseUrl : ChatFocus::ImageBaseUrl, L"接口地址",
                  video ? HitKind::AiVideoBaseUrl : HitKind::AiImageBaseUrl, service.baseUrl,
                  false);
        drawField(video ? ChatFocus::VideoApiKey : ChatFocus::ImageApiKey, L"接口密钥",
                  video ? HitKind::AiVideoApiKey : HitKind::AiImageApiKey, service.apiKey, true);
        drawField(video ? ChatFocus::VideoModel : ChatFocus::ImageModel, L"模型名称",
                  video ? HitKind::AiVideoModel : HitKind::AiImageModel, service.model, false);
        const wchar_t* hint =
            video ? L"视频生成按 OpenAI 兼容 /videos 异步接口：创建任务后轮询状态，完成后下载视频。\n"
                    L"生成结果保存在数据目录的 generated/videos 下。\n"
                    L"接口地址或密钥留空时，沿用当前对话接口配置。"
                  : L"图像生成按 OpenAI 兼容 /images/generations 接口调用，"
                    L"返回 b64_json 或图片地址均可。\n"
                    L"生成结果保存在数据目录的 generated/images 下。\n"
                    L"接口地址或密钥留空时，沿用当前对话接口配置。";
        renderer_.DrawTextBlock(
            hint,
            D2D1::RectF(panel.left + 24.0f, panel.top + 314.0f, panel.right - 24.0f,
                        panel.top + 456.0f),
            formats.caption.Get(), colors.secondaryText);
    }

    DrawButton(L"保存",
               D2D1::RectF(panel.left + 24.0f, panel.top + 470.0f, panel.left + 124.0f,
                           panel.top + 506.0f),
               HitKind::AiSettingsSave, colors, true, false);
    DrawButton(L"关闭",
               D2D1::RectF(panel.right - 124.0f, panel.top + 470.0f, panel.right - 24.0f,
                           panel.top + 506.0f),
               HitKind::AiSettingsClose, colors, false, false);
}

void Application::OpenAiSettings() {
    data::ReaderSettings& settings = library_.Settings();
    aiSettingsVisible_ = true;
    chatFocus_ = ChatFocus::None;
    // 打开时记录快照；未点「保存」直接关闭时恢复，编辑过的内容不会留在设置里。
    aiSettingsChatSnapshot_ = settings.aiChatConfigs;
    aiSettingsActiveChatSnapshot_ = settings.activeAiChatId;
    aiSettingsSpeechSnapshot_ = settings.aiSpeech;
    aiSettingsImageSnapshot_ = settings.aiImage;
    aiSettingsVideoSnapshot_ = settings.aiVideo;
    aiSettingsSpeechEngineSnapshot_ = settings.speechEngine;
    aiSettingsHasSnapshot_ = true;
    aiContextTokensDraft_ =
        std::to_wstring(data::ActiveAiChat(library_.Settings()).contextTokens);
    StopCaretBlink();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Application::CloseAiSettings() {
    if (aiSettingsHasSnapshot_) {
        // 放弃本次编辑：把设置恢复到打开弹框时的状态。
        data::ReaderSettings& settings = library_.Settings();
        settings.aiChatConfigs = aiSettingsChatSnapshot_;
        // 当前配置属于选择，不回滚；只有它已不在恢复后的列表中时才退回打开弹框时的配置。
        const auto activeExists = [&settings](const std::wstring& id) {
            return std::any_of(settings.aiChatConfigs.begin(), settings.aiChatConfigs.end(),
                               [&id](const data::AiChatConfig& config) {
                                   return config.id == id;
                               });
        };
        if (!activeExists(settings.activeAiChatId)) {
            settings.activeAiChatId = aiSettingsActiveChatSnapshot_;
            if (!activeExists(settings.activeAiChatId) && !settings.aiChatConfigs.empty()) {
                settings.activeAiChatId = settings.aiChatConfigs.front().id;
            }
        }
        settings.aiSpeech = aiSettingsSpeechSnapshot_;
        settings.aiImage = aiSettingsImageSnapshot_;
        settings.aiVideo = aiSettingsVideoSnapshot_;
        settings.speechEngine = aiSettingsSpeechEngineSnapshot_;
        aiSettingsHasSnapshot_ = false;
        aiContextTokensDraft_ =
            std::to_wstring(data::ActiveAiChat(settings).contextTokens);
        // 编辑期间可能有阅读统计等写入，把回滚后的设置重新落盘，确保磁盘与界面一致。
        std::wstring error;
        library_.SaveIndex(error);
    }
    aiSettingsVisible_ = false;
    chatFocus_ = ChatFocus::None;
    StopCaretBlink();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Application::SaveAiSettings() {
    // 上下文最大值按数字解析，非法输入保留原值。
    data::AiChatConfig& chat = data::ActiveAiChat(library_.Settings());
    uint32_t contextTokens = chat.contextTokens;
    if (ExtractFirstNumber(aiContextTokensDraft_, contextTokens)) {
        chat.contextTokens = std::clamp(contextTokens, 1024u, 10000000u);
    } else {
        chat.contextTokens = 200000;
    }
    aiContextTokensDraft_ = std::to_wstring(chat.contextTokens);
    std::wstring error;
    const bool saved = library_.Save(error);
    // 已提交，关闭弹框时不再回滚。
    aiSettingsHasSnapshot_ = false;
    aiSettingsVisible_ = false;
    chatFocus_ = ChatFocus::None;
    StopCaretBlink();
    ShowStatus(saved ? L"AI 接口设置已保存" : L"保存 AI 接口设置失败：" + error);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Application::StepAiChatConfig(int direction) {
    data::ReaderSettings& settings = library_.Settings();
    if (settings.aiChatConfigs.empty()) {
        return;
    }
    const int count = static_cast<int>(settings.aiChatConfigs.size());
    int index = 0;
    for (int i = 0; i < count; ++i) {
        if (settings.aiChatConfigs[static_cast<size_t>(i)].id == settings.activeAiChatId) {
            index = i;
            break;
        }
    }
    index = ((index + direction) % count + count) % count;
    settings.activeAiChatId = settings.aiChatConfigs[static_cast<size_t>(index)].id;
    aiContextTokensDraft_ =
        std::to_wstring(settings.aiChatConfigs[static_cast<size_t>(index)].contextTokens);
    // 切换服务商属于选择而不是字段编辑：立即生效。
    // 关闭弹框时只回滚未保存的字段内容，不回滚这次选择。
    chatFocus_ = ChatFocus::None;
    StopCaretBlink();
    InvalidateRect(hwnd_, nullptr, FALSE);
    UpdateWindow(hwnd_);
}

void Application::AddAiChatConfig() {
    data::ReaderSettings& settings = library_.Settings();
    data::AiChatConfig config;
    config.name = L"接口 " + std::to_wstring(settings.aiChatConfigs.size() + 1);
    config.id = L"chat-" +
                util::Sha256Hex(util::ToUtf8(
                                    config.name + std::to_wstring(util::CurrentUnixSeconds())))
                    .substr(0, 10);
    size_t index = settings.aiChatConfigs.size();
    for (size_t i = 0; i < settings.aiChatConfigs.size(); ++i) {
        if (settings.aiChatConfigs[i].id == settings.activeAiChatId) {
            index = i + 1;
            break;
        }
    }
    settings.aiChatConfigs.insert(settings.aiChatConfigs.begin() +
                                  static_cast<std::ptrdiff_t>(index),
                                  std::move(config));
    settings.activeAiChatId =
        settings.aiChatConfigs[index < settings.aiChatConfigs.size() ? index
                                                                     : settings.aiChatConfigs.size() - 1]
            .id;
    aiContextTokensDraft_ = std::to_wstring(
        settings.aiChatConfigs[index < settings.aiChatConfigs.size() ? index
                                                                     : settings.aiChatConfigs.size() - 1]
            .contextTokens);
    // 新增只在内存中生效，由「保存」统一落盘；未保存关闭时整体回滚。
    chatFocus_ = ChatFocus::None;
    StopCaretBlink();
    InvalidateRect(hwnd_, nullptr, FALSE);
    UpdateWindow(hwnd_);
}

void Application::DeleteAiChatConfig() {
    data::ReaderSettings& settings = library_.Settings();
    if (settings.aiChatConfigs.size() <= 1) {
        return;
    }
    size_t index = 0;
    for (size_t i = 0; i < settings.aiChatConfigs.size(); ++i) {
        if (settings.aiChatConfigs[i].id == settings.activeAiChatId) {
            index = i;
            break;
        }
    }
    const size_t removedIndex = index;
    settings.aiChatConfigs.erase(settings.aiChatConfigs.begin() +
                                 static_cast<std::ptrdiff_t>(removedIndex));
    // 新增项固定插在原当前项之后，因此删除后回到它前面的那一项，
    // 也就是新增之前的当前配置；被删项是首项时取新的首项。
    index = removedIndex > 0 ? removedIndex - 1 : 0;
    if (index >= settings.aiChatConfigs.size()) {
        index = settings.aiChatConfigs.size() - 1;
    }
    settings.activeAiChatId = settings.aiChatConfigs[index].id;
    aiContextTokensDraft_ = std::to_wstring(settings.aiChatConfigs[index].contextTokens);
    // 删除只在内存中生效，由「保存」统一落盘；未保存关闭时整体回滚。
    chatFocus_ = ChatFocus::None;
    StopCaretBlink();
    InvalidateRect(hwnd_, nullptr, FALSE);
    UpdateWindow(hwnd_);
}

}  // namespace ui
