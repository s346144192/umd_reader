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
#include <fstream>
#include <iterator>
#include <regex>
#include <vector>

#include "app_identity.h"
#include "core/text_util.h"
#include "nlohmann/json.hpp"
#include "ui/ui_internal.h"

namespace ui {
using namespace detail;

namespace {

// 按出现顺序提取命令参数中的无符号整数，最多 count 个。
// 解析时跳过分隔符，兼容「:」「-」「 」等模型可能采用的写法。
std::vector<uint32_t> ExtractCommandNumbers(const std::wstring& text, size_t count) {
    std::vector<uint32_t> numbers;
    size_t i = 0;
    while (i < text.size() && numbers.size() < count) {
        if (text[i] < L'0' || text[i] > L'9') {
            ++i;
            continue;
        }
        size_t end = i;
        while (end < text.size() && text[end] >= L'0' && text[end] <= L'9') {
            ++end;
        }
        const unsigned long long parsed = wcstoull(text.substr(i, end - i).c_str(), nullptr, 10);
        numbers.push_back(parsed > 0xFFFFFFFFull ? 0xFFFFFFFFu
                                                 : static_cast<uint32_t>(parsed));
        i = end;
    }
    return numbers;
}

// 配置字段在粘贴或输入时可能带首尾空白，发送前统一清理，
// 避免接口地址、模型名称或密钥因空白字符被服务端判为无效。
std::wstring TrimConfigValue(const std::wstring& value) {
    const auto isSpace = [](wchar_t character) {
        return character == L' ' || character == L'\t' || character == L'\r' ||
               character == L'\n';
    };
    size_t begin = 0;
    size_t end = value.size();
    while (begin < end && isSpace(value[begin])) {
        ++begin;
    }
    while (end > begin && isSpace(value[end - 1])) {
        --end;
    }
    return value.substr(begin, end - begin);
}

}  // namespace

void Application::SendAiMessage() {
    if (aiStreaming_) {
        aiStatus_ = L"上一轮回复尚未结束";
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    const std::wstring prompt = TrimNoteText(aiDraft_);
    if (prompt.empty()) {
        aiStatus_ = L"请输入问题";
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    aiDraft_.clear();
    // 清空输入框后必须把光标归位，否则下一次输入（尤其输入法上屏）会按旧下标插入。
    ResetInputCaretToEnd();
    StartAiRequest(prompt);
}

void Application::StartAiRequest(const std::wstring& prompt) {
    const data::ReaderSettings& settings = library_.Settings();
    const data::AiChatConfig& chat = data::ActiveAiChat(settings);
    std::wstring baseUrl = TrimConfigValue(chat.baseUrl);
    while (!baseUrl.empty() && baseUrl.back() == L'/') {
        baseUrl.pop_back();
    }
    const std::wstring model = TrimConfigValue(chat.model);
    if (baseUrl.empty() || model.empty()) {
        aiStatus_ = L"请先填写接口地址与模型名称";
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    data::ChatSession* session = EnsureChatSession();
    if (session == nullptr) {
        aiStatus_ = L"请先打开一本书再开始对话";
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    // 本轮问题作为用户消息写入会话，连续对话基于会话内的全部历史。
    data::ChatMessage userMessage;
    userMessage.role = L"user";
    userMessage.text = prompt;
    userMessage.createdAt = util::CurrentUnixSeconds();
    if (session->messages.empty()) {
        // 会话摘要取问题的第一行，保留足够长度便于在列表中辨别。
        std::wstring summary = prompt;
        const size_t lineBreak = summary.find_first_of(L"\r\n");
        if (lineBreak != std::wstring::npos) {
            summary.resize(lineBreak);
        }
        summary = TrimNoteText(summary);
        if (summary.empty()) {
            summary = TrimNoteText(prompt);
        }
        session->title = util::Ellipsize(summary, 60);
    }
    session->updatedAt = userMessage.createdAt;
    session->messages.push_back(std::move(userMessage));
    activeChatSessionId_ = session->id;
    SaveChatState();

    // 新一轮提问：清空上一轮遗留的隐藏上下文与轮数计数。
    aiTurnContext_.clear();
    aiRoundCount_ = 0;
    LaunchAiRequest();
}

void Application::LaunchAiRequest() {
    const data::ReaderSettings& settings = library_.Settings();
    const data::AiChatConfig& chat = data::ActiveAiChat(settings);
    std::wstring baseUrl = TrimConfigValue(chat.baseUrl);
    while (!baseUrl.empty() && baseUrl.back() == L'/') {
        baseUrl.pop_back();
    }
    const std::wstring model = TrimConfigValue(chat.model);
    if (baseUrl.empty() || model.empty()) {
        aiStatus_ = L"请先填写接口地址与模型名称";
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    // 会话消息是可见对话；隐藏上下文只随请求发送，不写入会话记录。
    std::vector<std::pair<std::wstring, std::wstring>> history;
    if (const data::ChatSession* session = ActiveChatSession()) {
        history.reserve(session->messages.size() + aiTurnContext_.size());
        for (const data::ChatMessage& message : session->messages) {
            history.emplace_back(message.role, message.text);
        }
    }
    for (const auto& item : aiTurnContext_) {
        history.emplace_back(item.first, item.second);
    }
    if (history.empty()) {
        aiStatus_ = L"没有可发送的对话内容";
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    aiAnswer_.clear();
    aiThinking_.clear();
    aiInputTokens_ = 0;
    aiCachedTokens_ = 0;
    aiOutputTokens_ = 0;
    // 状态栏同时显示本次实际使用的配置名称与模型，便于确认切换是否生效。
    std::wstring providerLabel = chat.name.empty() ? model : chat.name;
    if (model != chat.name) {
        providerLabel += L" / " + model;
    }
    aiStatus_ = L"正在生成…（" + providerLabel + L"）";
    aiStreaming_ = true;
    aiCancel_ = false;
    // 记录本轮请求实际使用的配置；若用户在生成期间切换设置，已发出的请求不受影响，
    // 下一轮请求会重新读取当前设置，从而立即生效。
    aiRequestConfigId_ = chat.id;
    aiRequestConfigName_ = chat.name;
    aiRequestModel_ = model;
    chatScrollMax_ = 0.0f;
    chatScroll_ = 0.0f;
    {
        std::lock_guard<std::mutex> lock(aiMutex_);
        aiPendingText_.clear();
        aiThinkingPending_.clear();
        aiPendingStatus_.clear();
        aiPendingInputTokens_ = 0;
        aiPendingCachedTokens_ = 0;
        aiPendingOutputTokens_ = 0;
        aiPendingDone_ = false;
    }
    if (aiThread_.joinable()) {
        aiThread_.join();
    }
    const bool anthropic = chat.protocol == L"anthropic";
    const std::wstring url = baseUrl + (anthropic ? L"/messages" : L"/chat/completions");
    const std::wstring apiKey = TrimConfigValue(chat.apiKey);
    // 诊断日志：记录本次请求实际使用的配置，确认切换是否作用到请求参数。
    {
        std::ofstream log(library_.DataDirectory() / "ai_request_debug.log", std::ios::app);
        if (log) {
            log << util::ToUtf8(util::FormatDateTime(util::CurrentUnixSeconds()))
                << " send active=" << util::ToUtf8(settings.activeAiChatId)
                << " id=" << util::ToUtf8(chat.id) << " name=" << util::ToUtf8(chat.name)
                << " model=" << util::ToUtf8(model) << " base=" << util::ToUtf8(baseUrl)
                << " keylen=" << apiKey.size() << "\n";
        }
    }
    const std::wstring systemPrompt = BuildAiSystemPrompt();
    aiThread_ = std::thread([this, url, apiKey, model, protocol = chat.protocol,
                             thinkingLevel = chat.thinkingLevel, systemPrompt,
                             history = std::move(history), target = hwnd_]() {
        RunAiRequest(url, apiKey, model, protocol, thinkingLevel, systemPrompt, history, target);
    });
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Application::StopAiRequest() {
    if (!aiStreaming_) {
        return;
    }
    aiCancel_ = true;
    if (aiThread_.joinable()) {
        aiThread_.join();
    }
    {
        std::lock_guard<std::mutex> lock(aiMutex_);
        // 把尚未分发的增量并入当前回复，停止前的输出仍然保留。
        aiAnswer_ += aiPendingText_;
        aiPendingText_.clear();
        aiThinking_ += aiThinkingPending_;
        aiThinkingPending_.clear();
        aiPendingStatus_.clear();
        aiPendingDone_ = false;
    }
    aiStreaming_ = false;
    CommitAiAnswer();
    aiStatus_ = L"已停止生成";
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Application::PushAiChunk(const std::wstring& text, HWND target) {
    if (aiCancel_) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(aiMutex_);
        aiPendingText_ += text;
    }
    PostMessageW(target, kAiStreamMessage, 0, 0);
}

void Application::PushAiThinking(const std::wstring& text, HWND target) {
    if (text.empty()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(aiMutex_);
        aiThinkingPending_ += text;
    }
    PostMessageW(target, kAiStreamMessage, 0, 0);
}

void Application::PushAiUsage(int inputTokens, int cachedTokens, int outputTokens,
                              HWND target) {
    {
        std::lock_guard<std::mutex> lock(aiMutex_);
        if (inputTokens > 0) {
            aiPendingInputTokens_ = inputTokens;
        }
        if (cachedTokens > 0) {
            aiPendingCachedTokens_ = cachedTokens;
        }
        if (outputTokens > 0) {
            aiPendingOutputTokens_ = outputTokens;
        }
    }
    PostMessageW(target, kAiStreamMessage, 0, 0);
}

void Application::PushAiDone(const std::wstring& status, HWND target) {
    {
        std::lock_guard<std::mutex> lock(aiMutex_);
        aiPendingStatus_ = status;
        aiPendingDone_ = true;
    }
    PostMessageW(target, kAiStreamMessage, 0, 0);
}

void Application::AppendAiChunks() {
    std::wstring chunk;
    std::wstring thinking;
    std::wstring status;
    bool done = false;
    int pendingInput = 0;
    int pendingCached = 0;
    int pendingOutput = 0;
    {
        std::lock_guard<std::mutex> lock(aiMutex_);
        chunk.swap(aiPendingText_);
        thinking.swap(aiThinkingPending_);
        pendingInput = aiPendingInputTokens_;
        pendingCached = aiPendingCachedTokens_;
        pendingOutput = aiPendingOutputTokens_;
        aiPendingInputTokens_ = 0;
        aiPendingCachedTokens_ = 0;
        aiPendingOutputTokens_ = 0;
        if (aiPendingDone_) {
            done = true;
            aiPendingDone_ = false;
            status.swap(aiPendingStatus_);
        }
    }
    if (!chunk.empty()) {
        aiAnswer_ += chunk;
    }
    if (!thinking.empty()) {
        aiThinking_ += thinking;
    }
    aiInputTokens_ = std::max(aiInputTokens_, pendingInput);
    aiCachedTokens_ = std::max(aiCachedTokens_, pendingCached);
    aiOutputTokens_ = std::max(aiOutputTokens_, pendingOutput);
    if (done) {
        aiStreaming_ = false;
        if (!aiAnswer_.empty()) {
            // 回复请求了章节数据时，会在同一轮内继续请求，此时保持「正在生成」状态。
            if (!CommitAiAnswer()) {
                aiStatus_ = status.empty() ? std::wstring(L"生成完成") : status;
            }
        } else {
            aiStatus_ = status.empty() ? std::wstring(L"没有收到回复内容") : status;
        }
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

bool Application::CommitAiAnswer() {
    if (aiAnswer_.empty()) {
        return false;
    }
    const std::wstring raw = aiAnswer_;
    aiAnswer_.clear();
    // 先执行回复中的控制命令（跳转、高亮、标注），并收集章节数据请求。
    std::wstring cleaned;
    AiDataRequest request;
    ProcessAiCommands(raw, cleaned, request);

    // 模型请求目录或正文时，把数据作为隐藏上下文追加，并在同一轮内继续请求。
    // 中间回复不写入会话、不显示在界面，用户只会看到最终的完整回答。
    constexpr int kMaxAiRounds = 8;
    if (!aiCancel_ && aiRoundCount_ < kMaxAiRounds &&
        (request.toc || !request.ranges.empty())) {
        std::wstring context = BuildAiDataContext(request);
        if (!context.empty()) {
            aiTurnContext_.emplace_back(L"assistant", raw);
            aiTurnContext_.emplace_back(
                L"user", L"以下是程序提供的书籍数据，仅用于回答当前问题，不要逐字复述：\n" + context);
            ++aiRoundCount_;
            LaunchAiRequest();
            return true;
        }
    }

    if (cleaned.empty()) {
        cleaned = L"（已执行阅读操作）";
    }
    data::ChatSession* session = EnsureChatSession();
    if (session == nullptr) {
        return false;
    }
    data::ChatMessage reply;
    reply.role = L"assistant";
    reply.text = cleaned;
    reply.thinking = aiThinking_;
    // 记录该条回复实际使用的接口配置与模型，切换 AI 后旧回复仍保留来源。
    reply.aiConfigId = aiRequestConfigId_;
    reply.aiConfigName = aiRequestConfigName_;
    reply.aiModel = aiRequestModel_;
    reply.inputTokens = aiInputTokens_;
    reply.cachedTokens = aiCachedTokens_;
    reply.outputTokens = aiOutputTokens_;
    aiThinking_.clear();
    reply.createdAt = util::CurrentUnixSeconds();
    session->updatedAt = reply.createdAt;
    session->messages.push_back(std::move(reply));
    activeChatSessionId_ = session->id;
    SaveChatState();
    // 本轮对话结束，丢弃为数据请求累积的隐藏上下文。
    aiTurnContext_.clear();
    return false;
}

std::wstring Application::BuildAiSystemPrompt() const {
    std::wstring prompt;
    prompt += L"你是本书的阅读助手，负责解答与本书相关的问题，并可协助定位内容、添加高亮与标注。\n";
    // 只提供书籍元数据；正文与目录不预先发送，由模型按需请求。
    if (!activeBook_.title.empty()) {
        prompt += L"书籍：《" + activeBook_.title + L"》\n";
    }
    if (!activeBook_.author.empty()) {
        prompt += L"作者：" + activeBook_.author + L"\n";
    }
    std::wstring bookType;
    if (!activeBook_.genre.empty()) {
        bookType = activeBook_.genre;
    } else {
        switch (activeBook_.formatType) {
        case 0: bookType = L"纯文本"; break;
        case 1: bookType = L"UMD 电子书"; break;
        case 2: bookType = L"UMD 动漫"; break;
        case 3: bookType = L"PDF 文档"; break;
        default: bookType = L"未知"; break;
        }
    }
    prompt += L"类型：" + bookType + L"\n";
    for (const data::BookRecord& record : library_.Books()) {
        if (record.id == activeBookId_) {
            prompt += L"文件大小：" + util::FormatFileSize(record.fileSize) + L"\n";
            break;
        }
    }
    prompt += L"章节数量：" + std::to_wstring(activeBook_.chapters.size()) + L"\n";
    // PDF 按原版页面显示，没有可检索的字符正文，改用页码描述位置并省略字符偏移命令。
    if (fixedPages_.IsOpen()) {
        const int pageCount = fixedPages_.PageCount();
        const int firstPage = fixedPages_.CurrentPage() + 1;
        const int lastPage =
            std::min(pageCount, fixedPages_.CurrentPage() + fixedPages_.VisiblePageCount());
        prompt += L"共 " + std::to_wstring(pageCount) + L" 页，当前显示第 " +
                  std::to_wstring(firstPage);
        if (lastPage > firstPage) {
            prompt += L"–" + std::to_wstring(lastPage);
        }
        prompt += L" 页。\n";
        prompt += L"本书按原版页面显示，正文以页面图像呈现；用户选中的文字会随问题一同发送。\n";
        prompt += L"回答时以页码说明位置，不要使用字符偏移或读取正文的命令。\n";
        return prompt;
    }
    const size_t chapterCount = activeBook_.chapters.size();
    prompt += L"共 " + std::to_wstring(chapterCount) + L" 章，全文 " +
              std::to_wstring(activeBook_.ContentCharCount()) + L" 个字符。\n";
    if (chapterCount > 0) {
        const int chapter = std::clamp(activeChapter_, 0, static_cast<int>(chapterCount) - 1);
        const uint32_t global = CurrentGlobalOffset();
        const uint32_t total = activeBook_.ContentCharCount();
        const int percent = total > 0
                                ? static_cast<int>(std::lround(100.0 * global / total))
                                : 0;
        prompt += L"当前阅读位置：第 " + std::to_wstring(chapter + 1) + L" 章《" +
                  ChapterTitleAt(activeBook_, chapter) + L"》，全书偏移 " +
                  std::to_wstring(global) + L"，进度 " + std::to_wstring(percent) + L"%。\n";
    }
    prompt +=
        L"正文与章节目录不会预先发送。需要时请在回复中输出下面的数据请求命令，"
        L"程序会在本次回答过程中内部提供数据并继续，这不是额外消息，也不会写入或显示在对话中。\n"
        L"偏移量为全文中的字符下标（从 0 开始）。\n"
        L"数据请求与控制命令（写在回复中，程序自动执行并从显示内容中移除；一条回复可包含多条）：\n"
        L"[[toc]] 获取章节目录（序号、标题、起始偏移、长度）\n"
        L"[[read:章节序号]] 获取整章正文（序号从 0 开始）\n"
        L"[[read:章节序号:章节内偏移:字符数]] 从章节内指定位置读取（偏移相对章节开头）\n"
        L"[[range:起始偏移:字符数]] 获取指定范围正文（偏移为全书字符下标）\n"
        L"[[search:关键词]] 检索关键词并获取其附近正文\n"
        L"[[goto:全书字符偏移]] 跳转到指定位置\n"
        L"[[highlight:起始偏移:长度:#RRGGBB]] 添加高亮\n"
        L"[[note:起始偏移:长度:标注文字]] 添加标注\n"
        L"每次回复请求的正文合计最多 24000 字符。每段正文头部标注 "
        L"range:起始偏移,本段字符数,max:全文字符数；需要后续内容时，以上一段的结束偏移继续请求。\n"
        L"正文尚未获取时不要猜测内容，先请求数据再回答；回答较长时连续输出直到结束。\n";
    return prompt;
}

void Application::ExecuteAiCommand(const std::wstring& body, AiDataRequest& request) {
    // 归一化全角字符后解析；归一化保持长度不变，标注文字的位置可直接映射回原文。
    const std::wstring normalized = NormalizeCommandText(body);
    const size_t colon = normalized.find(L':');
    const std::wstring name =
        TrimNoteText(colon == std::wstring::npos ? normalized : normalized.substr(0, colon));
    if (name == L"toc" || name == L"目录" || name == L"章节") {
        request.toc = true;
        return;
    }
    if (colon == std::wstring::npos) {
        return;
    }
    const std::wstring args = normalized.substr(colon + 1);
    if (name == L"goto" || name == L"跳转") {
        uint32_t offset = 0;
        if (ExtractFirstNumber(args, offset)) {
            GoToGlobalOffset(offset);
        }
        return;
    }
    if (name == L"read" || name == L"读取") {
        // 支持「章节序号」「章节序号:章节内偏移:字符数」以及章节标题三种写法。
        const std::vector<uint32_t> numbers = ExtractCommandNumbers(args, 3);
        size_t index = 0;
        bool matched = false;
        if (!numbers.empty() && numbers[0] < activeBook_.chapters.size()) {
            index = numbers[0];
            matched = true;
        } else {
            // 序号无效时按标题匹配；标题可能自带数字，因此只取第一个分隔符之前的部分。
            const size_t separator = args.find(L':');
            const std::wstring keyword =
                TrimNoteText(separator == std::wstring::npos ? args : args.substr(0, separator));
            if (!keyword.empty()) {
                for (size_t i = 0; i < activeBook_.chapters.size(); ++i) {
                    if (ChapterTitleAt(activeBook_, static_cast<int>(i)).find(keyword) !=
                        std::wstring::npos) {
                        index = i;
                        matched = true;
                        break;
                    }
                }
            }
        }
        if (matched) {
            const umd::Chapter& chapter = activeBook_.chapters[index];
            // 省略偏移与字符数时读取整章；偏移超出章节长度时不追加区间。
            const uint32_t offset =
                numbers.size() > 1 ? std::min(numbers[1], chapter.charLength) : 0;
            const uint32_t remaining = chapter.charLength - offset;
            const uint32_t length =
                numbers.size() > 2 ? std::min(numbers[2], remaining) : remaining;
            if (length > 0) {
                request.ranges.emplace_back(chapter.charOffset + offset, length);
            }
        }
        return;
    }
    if (name == L"range" || name == L"范围") {
        // 支持「起始偏移:字符数」、「起始偏移-结束偏移」、「起始偏移 结束偏移」等写法。
        const std::vector<uint32_t> numbers = ExtractCommandNumbers(args, 2);
        if (numbers.size() < 2) {
            return;
        }
        const uint32_t start = numbers[0];
        uint32_t length = 0;
        if (args.find(L':') != std::wstring::npos) {
            length = numbers[1];
        } else if (numbers[1] > start) {
            length = numbers[1] - start;
        }
        if (length > 0) {
            request.ranges.emplace_back(start, length);
        }
        return;
    }
    if (name == L"search" || name == L"find" || name == L"搜索" || name == L"查找") {
        // 关键词检索：先找当前阅读位置之后的首次出现，找不到再从全文开头查找。
        const std::wstring keyword = TrimNoteText(args);
        const uint32_t total = activeBook_.ContentCharCount();
        if (keyword.empty() || total == 0) {
            return;
        }
        const std::wstring loweredContent = LowerCase(activeBook_.content);
        const std::wstring loweredKeyword = LowerCase(keyword);
        const uint32_t current = std::min(CurrentGlobalOffset(), total);
        size_t position = loweredContent.find(loweredKeyword, current);
        if (position == std::wstring::npos) {
            position = loweredContent.find(loweredKeyword);
        }
        if (position != std::wstring::npos) {
            const uint32_t match = static_cast<uint32_t>(position);
            const uint32_t start = match > 600 ? match - 600 : 0;
            const uint32_t length =
                std::min<uint32_t>(static_cast<uint32_t>(keyword.size()) + 1800, total - start);
            request.ranges.emplace_back(start, length);
        }
        return;
    }
    if (name != L"highlight" && name != L"note" && name != L"高亮" && name != L"标注") {
        return;
    }
    const size_t first = args.find(L':');
    if (first == std::wstring::npos) {
        return;
    }
    uint32_t start = 0;
    if (!ExtractFirstNumber(args.substr(0, first), start)) {
        return;
    }
    const size_t second = args.find(L':', first + 1);
    const std::wstring lengthText =
        second == std::wstring::npos ? args.substr(first + 1)
                                     : args.substr(first + 1, second - first - 1);
    uint32_t length = 0;
    if (!ExtractFirstNumber(lengthText, length)) {
        return;
    }
    // 标注文字取原文字符串，保留模型输出的全角标点。
    const std::wstring extra =
        second == std::wstring::npos ? std::wstring()
                                     : body.substr(colon + 1 + second + 1);
    if (name == L"highlight" || name == L"高亮") {
        AddAiHighlight(start, length, extra);
    } else {
        AddAiNote(start, length, extra);
    }
}

void Application::ProcessAiCommands(const std::wstring& reply, std::wstring& cleaned,
                                    AiDataRequest& request) {
    cleaned.clear();
    request = AiDataRequest{};
    size_t position = 0;
    while (position < reply.size()) {
        const size_t commandOpen = reply.find(L"[[", position);
        if (commandOpen == std::wstring::npos) {
            cleaned.append(reply.substr(position));
            break;
        }
        cleaned.append(reply.substr(position, commandOpen - position));
        const size_t close = reply.find(L"]]", commandOpen + 2);
        if (close == std::wstring::npos) {
            // 未闭合的命令按普通文本保留，避免吞掉回复内容。
            cleaned.append(reply.substr(commandOpen));
            break;
        }
        ExecuteAiCommand(reply.substr(commandOpen + 2, close - commandOpen - 2), request);
        position = close + 2;
    }
}

std::wstring Application::BuildAiDataContext(const AiDataRequest& request) const {
    const std::wstring& content = activeBook_.content;
    const uint32_t total = static_cast<uint32_t>(content.size());
    std::wstring context;
    if (request.toc) {
        context += L"章节目录（序号｜标题｜起始偏移｜长度；全文共 " + std::to_wstring(total) +
                   L" 个字符）：\n";
        for (size_t i = 0; i < activeBook_.chapters.size(); ++i) {
            const umd::Chapter& chapter = activeBook_.chapters[i];
            context += std::to_wstring(i) + L"｜" +
                       ChapterTitleAt(activeBook_, static_cast<int>(i)) + L"｜" +
                       std::to_wstring(chapter.charOffset) + L"｜" +
                       std::to_wstring(chapter.charLength) + L"\n";
        }
    }
    // 单次上下文上限：多段正文合计不超过该值，避免挤占回答所需的 token。
    constexpr uint32_t kMaxContextChars = 24000;
    uint32_t remaining = kMaxContextChars;
    bool appendedBody = false;
    for (const auto& range : request.ranges) {
        if (remaining == 0) {
            break;
        }
        const uint32_t start = std::min(range.first, total);
        if (start >= total) {
            continue;
        }
        const uint32_t length = std::min(std::min(range.second, remaining), total - start);
        if (length == 0) {
            continue;
        }
        const uint32_t end = start + length;
        // 头部同时给出结构化范围（起始偏移、本段字符数、全文字符数）与中文说明，
        // 模型据此判断是否还有后续内容以及下一次请求的起始位置。
        context += L"正文（range:" + std::to_wstring(start) + L"," + std::to_wstring(length) +
                   L",max:" + std::to_wstring(total) + L"；全书偏移 " + std::to_wstring(start) +
                   L" - " + std::to_wstring(end) + L"）：\n";
        context += content.substr(start, length) + L"\n";
        remaining -= length;
        appendedBody = true;
        if (end >= total) {
            continue;
        }
        // 明确告知本次实际返回的范围与下一段的起点，模型据此按偏移继续读取。
        if (remaining > 0) {
            const uint32_t nextLength = std::min(remaining, total - end);
            context += L"（本次返回全书偏移 " + std::to_wstring(start) + L" - " +
                       std::to_wstring(end) + L"；后续内容请请求 [[range:" +
                       std::to_wstring(end) + L":" + std::to_wstring(nextLength) + L"]]）\n";
        } else {
            context += L"（本次返回全书偏移 " + std::to_wstring(start) + L" - " +
                       std::to_wstring(end) + L"；已达单次上限 " +
                       std::to_wstring(kMaxContextChars) + L" 字符，后续内容请请求 [[range:" +
                       std::to_wstring(end) + L":" +
                       std::to_wstring(std::min<uint32_t>(kMaxContextChars, total - end)) +
                       L"]]）\n";
        }
    }
    if (!request.ranges.empty() && !appendedBody) {
        context += L"（请求的正文偏移超出全文范围，全文共 " + std::to_wstring(total) +
                   L" 个字符）\n";
    }
    return context;
}

void Application::AddAiHighlight(uint32_t globalStart, uint32_t length,
                                 const std::wstring& color) {
    data::BookRecord* record = library_.Find(activeBookId_);
    if (record == nullptr || activeBook_.chapters.empty() || length == 0) {
        return;
    }
    int chapterIndex = -1;
    uint32_t chapterOffset = 0;
    for (size_t i = 0; i < activeBook_.chapters.size(); ++i) {
        const umd::Chapter& chapter = activeBook_.chapters[i];
        if (globalStart >= chapter.charOffset &&
            globalStart < chapter.charOffset + chapter.charLength) {
            chapterIndex = static_cast<int>(i);
            chapterOffset = chapter.charOffset;
            break;
        }
    }
    if (chapterIndex < 0) {
        return;
    }
    data::Highlight highlight;
    highlight.chapterIndex = chapterIndex;
    highlight.start = globalStart - chapterOffset;
    highlight.length = length;
    highlight.color = color.empty() ? std::wstring(L"#FFE082") : color;
    highlight.createdAt = util::CurrentUnixSeconds();
    record->highlights.push_back(highlight);
    std::wstring error;
    library_.SaveBook(activeBookId_, error);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Application::AddAiNote(uint32_t globalStart, uint32_t length, const std::wstring& text) {
    data::BookRecord* record = library_.Find(activeBookId_);
    if (record == nullptr || activeBook_.chapters.empty() || length == 0 || text.empty()) {
        return;
    }
    int chapterIndex = -1;
    uint32_t chapterOffset = 0;
    for (size_t i = 0; i < activeBook_.chapters.size(); ++i) {
        const umd::Chapter& chapter = activeBook_.chapters[i];
        if (globalStart >= chapter.charOffset &&
            globalStart < chapter.charOffset + chapter.charLength) {
            chapterIndex = static_cast<int>(i);
            chapterOffset = chapter.charOffset;
            break;
        }
    }
    if (chapterIndex < 0) {
        return;
    }
    data::Note note;
    note.chapterIndex = chapterIndex;
    note.start = globalStart - chapterOffset;
    note.length = length;
    note.text = text;
    note.textHash = util::Sha256Hex(util::ToUtf8(text)).substr(0, 32);
    note.createdAt = util::CurrentUnixSeconds();
    record->notes.push_back(note);
    std::wstring error;
    library_.SaveBook(activeBookId_, error);
    // 标注会插入到排版文本中，需要清空排版缓存后按当前位置重建。
    ClearLayoutCache();
    RebuildPagination(CurrentGlobalOffset());
    InvalidateRect(hwnd_, nullptr, FALSE);
}

namespace {

// 生成接口错误提示：优先展示服务端返回的 error.message，并补充按状态码的含义说明。
std::wstring DescribeAiApiError(DWORD statusCode, const std::string& body) {
    std::wstring detail;
    try {
        const nlohmann::json parsed = nlohmann::json::parse(body);
        if (parsed.is_object()) {
            const auto error = parsed.find("error");
            if (error != parsed.end()) {
                if (error->is_string()) {
                    detail = util::FromUtf8(error->get<std::string>());
                } else if (error->is_object()) {
                    const auto message = error->find("message");
                    if (message != error->end() && message->is_string()) {
                        detail = util::FromUtf8(message->get<std::string>());
                    }
                }
            }
            if (detail.empty()) {
                const auto message = parsed.find("message");
                if (message != parsed.end() && message->is_string()) {
                    detail = util::FromUtf8(message->get<std::string>());
                }
            }
        }
    } catch (const std::exception&) {
        // 响应不是 JSON 时按原文展示。
    }
    if (detail.empty()) {
        detail = util::FromUtf8(body);
    }

    std::wstring hint;
    switch (statusCode) {
    case 401:
        hint = L"密钥无效或未授权";
        break;
    case 402:
        hint = L"账户余额不足或配额已用尽";
        break;
    case 403:
        hint = L"当前密钥没有访问权限";
        break;
    case 404:
        hint = L"接口地址或模型名称不存在";
        break;
    case 429:
        hint = L"请求过于频繁，请稍后再试";
        break;
    default:
        break;
    }

    std::wstring message =
        L"接口返回错误（HTTP " + std::to_wstring(statusCode) + L"）";
    if (!hint.empty()) {
        message += L"：" + hint;
    }
    if (!detail.empty()) {
        message += L"\n" + detail;
    }
    return message;
}

}  // namespace

void Application::RunAiRequest(std::wstring url, std::wstring apiKey, std::wstring model,
                               std::wstring protocol, int thinkingLevel,
                               std::wstring systemPrompt,
                               std::vector<std::pair<std::wstring, std::wstring>> history,
                               HWND target) {
    const bool anthropic = protocol == L"anthropic";
    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &components)) {
        PushAiDone(L"接口地址格式无效", target);
        return;
    }
    const std::wstring host(components.lpszHostName, components.dwHostNameLength);
    std::wstring path(components.lpszUrlPath, components.dwUrlPathLength);
    if (components.dwExtraInfoLength > 0) {
        path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    }
    if (path.empty()) {
        path = L"/";
    }

    // 使用系统代理：部分接口在直连网络下不可达，必须遵循用户的代理配置。
    // AUTOMATIC_PROXY 在 Windows 8.1 之前不可用，失败时退回 WinHTTP 默认代理配置。
    HINTERNET session = WinHttpOpen(app_id::kHttpUserAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        session = WinHttpOpen(app_id::kHttpUserAgent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (!session) {
        PushAiDone(L"无法初始化网络会话", target);
        return;
    }
    WinHttpSetTimeouts(session, 5000, 5000, 15000, 30000);

    HINTERNET connection = WinHttpConnect(session, host.c_str(), components.nPort, 0);
    if (!connection) {
        WinHttpCloseHandle(session);
        PushAiDone(L"无法连接接口主机", target);
        return;
    }
    const DWORD requestFlags =
        components.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = WinHttpOpenRequest(connection, L"POST", path.c_str(), nullptr,
                                           WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           requestFlags);
    if (!request) {
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        PushAiDone(L"无法创建请求", target);
        return;
    }

    std::wstring headers = L"Content-Type: application/json\r\nAccept: text/event-stream\r\n";
    if (anthropic) {
        headers += L"anthropic-version: 2023-06-01\r\n";
        if (!apiKey.empty()) {
            headers += L"x-api-key: " + apiKey + L"\r\n";
        }
    } else if (!apiKey.empty()) {
        headers += L"Authorization: Bearer " + apiKey + L"\r\n";
    }
    WinHttpAddRequestHeaders(request, headers.c_str(), static_cast<DWORD>(headers.size()),
                             WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);

    nlohmann::json body;
    body["model"] = util::ToUtf8(model);
    body["stream"] = true;
    const int thinkingBudget =
        thinkingLevel <= 0 ? 0 : (thinkingLevel == 1 ? 1024 : (thinkingLevel == 2 ? 2048 : 4096));
    if (anthropic) {
        // Anthropic Messages 接口必须指定 max_tokens，系统提示使用顶层 system 字段。
        // 开启思考时 max_tokens 需大于思考预算。
        body["max_tokens"] = thinkingBudget + 1024;
        if (thinkingBudget > 0) {
            nlohmann::json thinking;
            thinking["type"] = "enabled";
            thinking["budget_tokens"] = thinkingBudget;
            body["thinking"] = std::move(thinking);
        }
        if (!systemPrompt.empty()) {
            body["system"] = util::ToUtf8(systemPrompt);
        }
    } else if (thinkingBudget > 0) {
        // OpenAI 兼容协议使用 reasoning_effort 表达思考等级。
        body["reasoning_effort"] =
            thinkingLevel == 1 ? "low" : (thinkingLevel == 2 ? "medium" : "high");
    }
    if (!anthropic) {
        // 请求流式响应在结束块中返回 token 用量。
        nlohmann::json streamOptions;
        streamOptions["include_usage"] = true;
        body["stream_options"] = std::move(streamOptions);
    }
    nlohmann::json messages = nlohmann::json::array();
    if (!anthropic && !systemPrompt.empty()) {
        // OpenAI 兼容协议把阅读助手身份与当前环境放在第一条 system 消息中。
        nlohmann::json systemMessage;
        systemMessage["role"] = "system";
        systemMessage["content"] = util::ToUtf8(systemPrompt);
        messages.push_back(std::move(systemMessage));
    }
    for (const auto& item : history) {
        nlohmann::json message;
        message["role"] = util::ToUtf8(item.first);
        message["content"] = util::ToUtf8(item.second);
        messages.push_back(std::move(message));
    }
    body["messages"] = std::move(messages);
    const std::string payload = body.dump();

    const BOOL sent = WinHttpSendRequest(
        request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, const_cast<char*>(payload.data()),
        static_cast<DWORD>(payload.size()), static_cast<DWORD>(payload.size()), 0);
    BOOL received = FALSE;
    if (sent) {
        received = WinHttpReceiveResponse(request, nullptr);
    }
    if (!sent || !received) {
        // 记录 WinHTTP 错误码，便于区分网络、代理与地址配置问题。
        const DWORD networkError = GetLastError();
        {
            std::ofstream log(library_.DataDirectory() / "ai_request_debug.log",
                              std::ios::app);
            if (log) {
                log << util::ToUtf8(util::FormatDateTime(util::CurrentUnixSeconds()))
                    << " failure stage=" << (sent ? "receive" : "send")
                    << " error=" << networkError << " url=" << util::ToUtf8(url) << "\n";
            }
        }
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        PushAiDone(L"请求失败（错误码 " + std::to_wstring(networkError) +
                       L"），请检查网络、接口地址、密钥与系统代理",
                   target);
        return;
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize,
                        WINHTTP_NO_HEADER_INDEX);
    if (statusCode != 200) {
        // 读取完整的错误响应，便于展示服务端给出的具体原因。
        std::string errorBody;
        char errorBuffer[1024];
        DWORD read = 0;
        while (WinHttpReadData(request, errorBuffer, sizeof(errorBuffer), &read) && read > 0) {
            errorBody.append(errorBuffer, read);
        }
        // 同时在诊断日志中保留状态码与服务端响应，便于复现后定位。
        {
            std::string detail = errorBody;
            if (detail.size() > 800) {
                detail.resize(800);
            }
            std::ofstream log(library_.DataDirectory() / "ai_request_debug.log",
                              std::ios::app);
            if (log) {
                log << util::ToUtf8(util::FormatDateTime(util::CurrentUnixSeconds()))
                    << " reply status=" << statusCode << " url=" << util::ToUtf8(url)
                    << " body=" << detail << "\n";
            }
        }
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        PushAiDone(DescribeAiApiError(statusCode, errorBody), target);
        return;
    }

    // 逐块读取 SSE 流，每行形如 "data: {json}"，结束标记为 "data: [DONE]"。
    std::string lineBuffer;
    std::vector<char> readBuffer(8192);
    std::wstring responseModel;
    bool done = false;
    while (!aiCancel_) {
        DWORD read = 0;
        if (!WinHttpReadData(request, readBuffer.data(),
                             static_cast<DWORD>(readBuffer.size()), &read) ||
            read == 0) {
            break;
        }
        lineBuffer.append(readBuffer.data(), read);
        size_t newline = 0;
        while ((newline = lineBuffer.find('\n')) != std::string::npos) {
            std::string line = lineBuffer.substr(0, newline);
            lineBuffer.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (line.rfind("data:", 0) != 0) {
                continue;
            }
            std::string data = line.substr(5);
            while (!data.empty() && data.front() == ' ') {
                data.erase(0, 1);
            }
            if (data == "[DONE]") {
                done = true;
                break;
            }
            try {
                const nlohmann::json chunk = nlohmann::json::parse(data);
                // 记录服务端回报的模型名，用于核对请求是否真的切换了模型。
                if (chunk.contains("model") && chunk["model"].is_string()) {
                    responseModel = util::FromUtf8(chunk["model"].get<std::string>());
                }
                if (anthropic) {
                    // Anthropic 的增量事件为 content_block_delta，结束事件为 message_stop。
                    const std::string type = chunk.value("type", std::string());
                    if (type == "message_stop") {
                        done = true;
                        break;
                    }
                    if (type == "message_start" && chunk.contains("message") &&
                        chunk["message"].is_object()) {
                        const nlohmann::json& message = chunk["message"];
                        if (message.contains("model") && message["model"].is_string()) {
                            responseModel = util::FromUtf8(message["model"].get<std::string>());
                        }
                        if (message.contains("usage") && message["usage"].is_object()) {
                            const nlohmann::json& usage = message["usage"];
                            PushAiUsage(usage.value("input_tokens", 0),
                                        usage.value("cache_read_input_tokens", 0), 0, target);
                        }
                    }
                    if (type == "message_delta" && chunk.contains("usage") &&
                        chunk["usage"].is_object()) {
                        PushAiUsage(0, 0, chunk["usage"].value("output_tokens", 0), target);
                    }
                    if (type == "content_block_delta" && chunk.contains("delta") &&
                        chunk["delta"].is_object()) {
                        const nlohmann::json& delta = chunk["delta"];
                        if (delta.value("type", std::string()) == "text_delta") {
                            const std::string text = delta.value("text", std::string());
                            if (!text.empty()) {
                                PushAiChunk(util::FromUtf8(text), target);
                            }
                        } else if (delta.value("type", std::string()) == "thinking_delta") {
                            // 思考内容单独流式输出，供界面折叠展示。
                            const std::string thinking = delta.value("thinking", std::string());
                            if (!thinking.empty()) {
                                PushAiThinking(util::FromUtf8(thinking), target);
                            }
                        }
                    }
                } else {
                    if (chunk.contains("usage") && chunk["usage"].is_object()) {
                        const nlohmann::json& usage = chunk["usage"];
                        const int inputTokens = usage.value("prompt_tokens", 0);
                        const int outputTokens = usage.value("completion_tokens", 0);
                        int cachedTokens = 0;
                        if (usage.contains("prompt_tokens_details") &&
                            usage["prompt_tokens_details"].is_object()) {
                            cachedTokens =
                                usage["prompt_tokens_details"].value("cached_tokens", 0);
                        }
                        if (cachedTokens == 0) {
                            // DeepSeek 等兼容实现使用 prompt_cache_hit_tokens。
                            cachedTokens = usage.value("prompt_cache_hit_tokens", 0);
                        }
                        PushAiUsage(inputTokens, cachedTokens, outputTokens, target);
                    }
                    if (!chunk.contains("choices") || !chunk["choices"].is_array() ||
                        chunk["choices"].empty()) {
                        continue;
                    }
                    const nlohmann::json& delta = chunk["choices"][0]["delta"];
                    if (delta.contains("reasoning_content") &&
                        delta["reasoning_content"].is_string()) {
                        // 常见兼容实现使用 reasoning_content 输出思考内容。
                        const std::string reasoning =
                            delta["reasoning_content"].get<std::string>();
                        if (!reasoning.empty()) {
                            PushAiThinking(util::FromUtf8(reasoning), target);
                        }
                    } else if (delta.contains("reasoning") && delta["reasoning"].is_string()) {
                        const std::string reasoning = delta["reasoning"].get<std::string>();
                        if (!reasoning.empty()) {
                            PushAiThinking(util::FromUtf8(reasoning), target);
                        }
                    }
                    if (delta.contains("content") && delta["content"].is_string()) {
                        const std::string content = delta["content"].get<std::string>();
                        if (!content.empty()) {
                            PushAiChunk(util::FromUtf8(content), target);
                        }
                    }
                }
            } catch (const std::exception&) {
                // 忽略无法解析的行，继续读取后续内容。
            }
        }
        if (done) {
            break;
        }
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    // 诊断日志：对照请求模型与服务端回报的模型。
    {
        std::ofstream log(library_.DataDirectory() / "ai_request_debug.log", std::ios::app);
        if (log) {
            log << util::ToUtf8(util::FormatDateTime(util::CurrentUnixSeconds()))
                << " reply sentModel=" << util::ToUtf8(model)
                << " serverModel=" << util::ToUtf8(responseModel) << "\n";
        }
    }
    PushAiDone(aiCancel_ ? std::wstring() : std::wstring(L"生成完成"), target);
}

}  // namespace ui
