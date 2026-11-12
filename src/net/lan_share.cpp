#include "net/lan_share.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <winhttp.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <sstream>
#include <unordered_set>
#include <utility>

#include "nlohmann/json.hpp"

namespace net {
namespace {

// 宽字符与 UTF-8 的转换只在网络边界使用，避免 net 模块依赖数据层。
std::string ToUtf8(const std::wstring& text) {
    if (text.empty()) {
        return std::string();
    }
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0, nullptr,
                                           nullptr);
    if (length <= 0) {
        return std::string();
    }
    std::string result(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(),
                        length, nullptr, nullptr);
    return result;
}

std::wstring FromUtf8(const std::string& text) {
    if (text.empty()) {
        return std::wstring();
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) {
        return std::wstring();
    }
    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(),
                        length);
    return result;
}

std::wstring TrimText(const std::wstring& text) {
    size_t begin = 0;
    while (begin < text.size() && iswspace(text[begin]) != 0) {
        ++begin;
    }
    size_t end = text.size();
    while (end > begin && iswspace(text[end - 1]) != 0) {
        --end;
    }
    return text.substr(begin, end - begin);
}

bool StartsWithIgnoreCase(const std::wstring& text, const wchar_t* prefix) {
    const size_t length = wcslen(prefix);
    if (text.size() < length) {
        return false;
    }
    for (size_t i = 0; i < length; ++i) {
        if (towlower(text[i]) != towlower(prefix[i])) {
            return false;
        }
    }
    return true;
}

std::string PercentEncode(const std::string& text) {
    static const char kHexDigits[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(text.size());
    for (const unsigned char value : text) {
        const bool unreserved =
            (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
            (value >= '0' && value <= '9') || value == '-' || value == '_' || value == '.' ||
            value == '~';
        if (unreserved) {
            result.push_back(static_cast<char>(value));
        } else {
            result.push_back('%');
            result.push_back(kHexDigits[value >> 4]);
            result.push_back(kHexDigits[value & 0x0F]);
        }
    }
    return result;
}

int HexDigitValue(char value) {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

std::string UrlDecode(const std::string& text) {
    std::string result;
    result.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size()) {
            const int high = HexDigitValue(text[i + 1]);
            const int low = HexDigitValue(text[i + 2]);
            if (high >= 0 && low >= 0) {
                result.push_back(static_cast<char>((high << 4) | low));
                i += 2;
                continue;
            }
        }
        result.push_back(text[i]);
    }
    return result;
}

std::wstring JsonWString(const nlohmann::json& item, const char* key) {
    const auto found = item.find(key);
    if (found == item.end() || !found->is_string()) {
        return std::wstring();
    }
    return FromUtf8(found->get<std::string>());
}

uint64_t JsonUint64(const nlohmann::json& item, const char* key) {
    const auto found = item.find(key);
    if (found == item.end()) {
        return 0;
    }
    if (found->is_number_unsigned()) {
        return found->get<uint64_t>();
    }
    if (found->is_number_integer()) {
        const int64_t value = found->get<int64_t>();
        return value > 0 ? static_cast<uint64_t>(value) : 0;
    }
    if (found->is_string()) {
        try {
            return std::stoull(found->get<std::string>());
        } catch (...) {
            return 0;
        }
    }
    return 0;
}

int JsonInt(const nlohmann::json& item, const char* key) {
    const auto found = item.find(key);
    if (found == item.end()) {
        return 0;
    }
    if (found->is_number_integer()) {
        return found->get<int>();
    }
    if (found->is_number_unsigned()) {
        const uint64_t value = found->get<uint64_t>();
        return value > 0x7FFFFFFFull ? 0 : static_cast<int>(value);
    }
    if (found->is_string()) {
        try {
            return std::stoi(found->get<std::string>());
        } catch (...) {
            return 0;
        }
    }
    return 0;
}

// WinHTTP 句柄的持有者，保证任意返回路径都能正确释放。
struct HttpHandles {
    HINTERNET session = nullptr;
    HINTERNET connect = nullptr;
    HINTERNET request = nullptr;

    ~HttpHandles() {
        if (request != nullptr) {
            WinHttpCloseHandle(request);
        }
        if (connect != nullptr) {
            WinHttpCloseHandle(connect);
        }
        if (session != nullptr) {
            WinHttpCloseHandle(session);
        }
    }
};

std::wstring WinHttpErrorText(const wchar_t* action) {
    return std::wstring(action) + L"失败（错误码 " + std::to_wstring(GetLastError()) + L"）";
}

bool OpenHttpGet(HttpHandles& handles, const std::wstring& host, uint16_t port,
                 const std::wstring& path, std::wstring& error) {
    // 局域网直连不经过系统代理，避免代理配置把内网地址转发到外部。
    handles.session = WinHttpOpen(L"UmdReader/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (handles.session == nullptr) {
        error = WinHttpErrorText(L"初始化网络会话");
        return false;
    }
    WinHttpSetTimeouts(handles.session, 5000, 5000, 10000, 8000);
    handles.connect = WinHttpConnect(handles.session, host.c_str(), port, 0);
    if (handles.connect == nullptr) {
        error = L"无法连接 " + host + L":" + std::to_wstring(port) + L"（" +
                std::to_wstring(GetLastError()) + L"）";
        return false;
    }
    handles.request = WinHttpOpenRequest(handles.connect, L"GET", path.c_str(), nullptr,
                                         WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (handles.request == nullptr) {
        error = WinHttpErrorText(L"创建请求");
        return false;
    }
    if (!WinHttpSendRequest(handles.request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        error = WinHttpErrorText(L"发送请求");
        return false;
    }
    if (!WinHttpReceiveResponse(handles.request, nullptr)) {
        error = WinHttpErrorText(L"接收响应");
        return false;
    }
    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    if (!WinHttpQueryHeaders(handles.request,
                             WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize,
                             WINHTTP_NO_HEADER_INDEX)) {
        error = WinHttpErrorText(L"读取响应状态");
        return false;
    }
    if (statusCode != 200) {
        error = L"服务器返回状态码 " + std::to_wstring(statusCode);
        return false;
    }
    return true;
}

}  // namespace

std::vector<LanInterface> LocalIpv4Interfaces() {
    std::vector<LanInterface> interfaces;
    ULONG bufferSize = 16 * 1024;
    std::vector<char> buffer(bufferSize);
    const ULONG flags =
        GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG status = GetAdaptersAddresses(
        AF_INET, flags, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()),
        &bufferSize);
    if (status == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(bufferSize);
        status = GetAdaptersAddresses(
            AF_INET, flags, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()),
            &bufferSize);
    }
    if (status != NO_ERROR) {
        return interfaces;
    }
    for (IP_ADAPTER_ADDRESSES* adapter =
             reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
         adapter != nullptr; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp) {
            continue;
        }
        if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
            continue;
        }
        for (IP_ADAPTER_UNICAST_ADDRESS* unicast = adapter->FirstUnicastAddress;
             unicast != nullptr; unicast = unicast->Next) {
            const sockaddr* address = unicast->Address.lpSockaddr;
            if (address == nullptr || address->sa_family != AF_INET) {
                continue;
            }
            const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(address);
            wchar_t text[INET_ADDRSTRLEN] = {};
            if (InetNtopW(AF_INET, &ipv4->sin_addr, text, INET_ADDRSTRLEN) == nullptr) {
                continue;
            }
            const std::wstring value(text);
            if (value.rfind(L"127.", 0) == 0 || value == L"0.0.0.0") {
                continue;
            }
            const bool duplicate =
                std::any_of(interfaces.begin(), interfaces.end(),
                            [&value](const LanInterface& entry) { return entry.address == value; });
            if (!duplicate) {
                LanInterface entry;
                entry.address = value;
                entry.prefixLength = unicast->OnLinkPrefixLength;
                interfaces.push_back(std::move(entry));
            }
        }
    }
    // 家用局域网通常使用 192.168 网段，其次为 10 与 172 网段，按此顺序优先展示。
    const auto score = [](const std::wstring& address) {
        if (address.rfind(L"192.168.", 0) == 0) {
            return 0;
        }
        if (address.rfind(L"10.", 0) == 0) {
            return 1;
        }
        if (address.rfind(L"172.", 0) == 0) {
            return 2;
        }
        return 3;
    };
    std::stable_sort(interfaces.begin(), interfaces.end(),
                     [&score](const LanInterface& first, const LanInterface& second) {
                         return score(first.address) < score(second.address);
                     });
    return interfaces;
}

std::vector<std::wstring> LocalIpv4Addresses() {
    std::vector<std::wstring> addresses;
    for (const LanInterface& entry : LocalIpv4Interfaces()) {
        addresses.push_back(entry.address);
    }
    return addresses;
}

namespace {

// 单次扫描的候选地址上限、并发线程数与响应读取上限。
constexpr size_t kScanMaxCandidates = 800;
constexpr size_t kScanWorkerCount = 32;
constexpr size_t kScanMaxResponseBytes = 1024;

bool ParseIPv4HostOrder(const std::wstring& text, uint32_t& value) {
    IN_ADDR address{};
    if (InetPtonW(AF_INET, text.c_str(), &address) != 1) {
        return false;
    }
    value = ntohl(address.S_un.S_addr);
    return true;
}

std::wstring IPv4Text(uint32_t hostOrder) {
    IN_ADDR address{};
    address.S_un.S_addr = htonl(hostOrder);
    wchar_t text[INET_ADDRSTRLEN] = {};
    if (InetNtopW(AF_INET, &address, text, INET_ADDRSTRLEN) == nullptr) {
        return std::wstring();
    }
    return std::wstring(text);
}

void AppendScanRange(uint32_t first, uint32_t last, uint32_t localAddress,
                     std::vector<uint32_t>& candidates, std::unordered_set<uint32_t>& seen) {
    if (first > last) {
        return;
    }
    for (uint32_t address = first;; ++address) {
        if (candidates.size() >= kScanMaxCandidates) {
            return;
        }
        if (address != localAddress && seen.insert(address).second) {
            candidates.push_back(address);
        }
        if (address == last) {
            return;
        }
    }
}

std::vector<uint32_t> BuildScanCandidates(const std::vector<LanInterface>& interfaces) {
    std::vector<uint32_t> candidates;
    std::unordered_set<uint32_t> seen;
    for (const LanInterface& entry : interfaces) {
        uint32_t local = 0;
        if (!ParseIPv4HostOrder(entry.address, local)) {
            continue;
        }
        uint32_t prefix = entry.prefixLength;
        if (prefix == 0) {
            // 未提供子网前缀时按常见的 /24 处理。
            prefix = 24;
        }
        if (prefix >= 32) {
            continue;
        }
        if (prefix == 31) {
            // 点对点网段只有对端一个地址。
            const uint32_t network = local & 0xFFFFFFFEu;
            const uint32_t other = local == network ? network + 1 : network;
            if (other != local && candidates.size() < kScanMaxCandidates &&
                seen.insert(other).second) {
                candidates.push_back(other);
            }
            continue;
        }
        if (prefix >= 24) {
            const uint32_t mask = 0xFFFFFFFFu << (32 - prefix);
            const uint32_t network = local & mask;
            const uint32_t broadcast = network | ~mask;
            AppendScanRange(network + 1, broadcast - 1, local, candidates, seen);
        } else {
            // 大型网段只扫描本机所在的 /24，避免逐个探测数万个地址。
            const uint32_t base = local & 0xFFFFFF00u;
            AppendScanRange(base + 1, base + 254, local, candidates, seen);
        }
    }
    return candidates;
}

bool ProbeLanHost(uint32_t hostOrderAddress, uint16_t port, uint32_t connectTimeoutMs,
                  const std::atomic<bool>* cancel) {
    if (cancel != nullptr && cancel->load()) {
        return false;
    }
    const SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        return false;
    }
    u_long nonBlocking = 1;
    ioctlsocket(sock, FIONBIO, &nonBlocking);
    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_port = htons(port);
    target.sin_addr.s_addr = htonl(hostOrderAddress);
    bool connected = false;
    if (connect(sock, reinterpret_cast<sockaddr*>(&target), sizeof(target)) == 0) {
        connected = true;
    } else {
        const int connectError = WSAGetLastError();
        if (connectError == WSAEWOULDBLOCK || connectError == WSAEINPROGRESS) {
            fd_set writeSet;
            FD_ZERO(&writeSet);
            FD_SET(sock, &writeSet);
            timeval timeout{};
            timeout.tv_sec = static_cast<long>(connectTimeoutMs / 1000);
            timeout.tv_usec = static_cast<long>((connectTimeoutMs % 1000) * 1000);
            if (select(0, nullptr, &writeSet, nullptr, &timeout) > 0) {
                int socketError = 0;
                int length = sizeof(socketError);
                if (getsockopt(sock, SOL_SOCKET, SO_ERROR,
                               reinterpret_cast<char*>(&socketError), &length) == 0 &&
                    socketError == 0) {
                    connected = true;
                }
            }
        }
    }
    if (!connected) {
        closesocket(sock);
        return false;
    }
    u_long blocking = 0;
    ioctlsocket(sock, FIONBIO, &blocking);
    DWORD ioTimeout = 900;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ioTimeout),
               sizeof(ioTimeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&ioTimeout),
               sizeof(ioTimeout));
    static const char kPingRequest[] =
        "GET /api/ping HTTP/1.1\r\nHost: umd-reader\r\nConnection: close\r\n\r\n";
    bool matched = false;
    if (send(sock, kPingRequest, static_cast<int>(sizeof(kPingRequest) - 1), 0) > 0) {
        std::string response;
        char buffer[256];
        while (response.size() < kScanMaxResponseBytes) {
            if (cancel != nullptr && cancel->load()) {
                break;
            }
            const int received = recv(sock, buffer, sizeof(buffer), 0);
            if (received <= 0) {
                break;
            }
            response.append(buffer, static_cast<size_t>(received));
            if (response.find("umd_reader") != std::string::npos) {
                matched = true;
                break;
            }
        }
    }
    closesocket(sock);
    return matched;
}

}  // namespace

bool ProbeLanShareHost(const std::wstring& address, uint16_t port,
                       uint32_t connectTimeoutMs) {
    uint32_t hostOrder = 0;
    if (!ParseIPv4HostOrder(address, hostOrder)) {
        return false;
    }
    WSADATA wsaData{};
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        return false;
    }
    const bool found = ProbeLanHost(hostOrder, port, connectTimeoutMs, nullptr);
    WSACleanup();
    return found;
}

bool ScanLanHosts(uint16_t port, uint32_t connectTimeoutMs, std::vector<std::wstring>& hosts,
                  std::wstring& error, const std::atomic<bool>* cancel) {
    error.clear();
    hosts.clear();
    if (cancel != nullptr && cancel->load()) {
        error = L"已取消";
        return false;
    }
    const std::vector<uint32_t> candidates = BuildScanCandidates(LocalIpv4Interfaces());
    if (candidates.empty()) {
        error = L"未找到可扫描的局域网网段";
        return false;
    }
    WSADATA wsaData{};
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        error = L"网络组件初始化失败";
        return false;
    }
    std::atomic<size_t> nextIndex{0};
    std::mutex resultMutex;
    std::vector<uint32_t> found;
    const size_t workerCount = std::min<size_t>(kScanWorkerCount, candidates.size());
    std::vector<std::thread> workers;
    workers.reserve(workerCount);
    for (size_t worker = 0; worker < workerCount; ++worker) {
        workers.emplace_back([&]() {
            while (true) {
                const size_t index = nextIndex.fetch_add(1);
                if (index >= candidates.size()) {
                    return;
                }
                if (cancel != nullptr && cancel->load()) {
                    return;
                }
                if (!ProbeLanHost(candidates[index], port, connectTimeoutMs, cancel)) {
                    continue;
                }
                std::lock_guard<std::mutex> lock(resultMutex);
                found.push_back(candidates[index]);
            }
        });
    }
    for (std::thread& worker : workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    std::sort(found.begin(), found.end());
    hosts.reserve(found.size());
    for (const uint32_t address : found) {
        const std::wstring text = IPv4Text(address);
        if (!text.empty()) {
            hosts.push_back(text);
        }
    }
    WSACleanup();
    return true;
}

bool ParseShareAddress(const std::wstring& text, uint16_t defaultPort, std::wstring& host,
                       uint16_t& port) {
    std::wstring value = TrimText(text);
    if (value.empty()) {
        return false;
    }
    if (StartsWithIgnoreCase(value, L"http://")) {
        value = value.substr(7);
    } else if (StartsWithIgnoreCase(value, L"https://")) {
        value = value.substr(8);
    }
    const size_t pathStart = value.find_first_of(L"/?#");
    if (pathStart != std::wstring::npos) {
        value = value.substr(0, pathStart);
    }
    value = TrimText(value);
    if (value.empty() || value.front() == L'[') {
        return false;
    }
    port = defaultPort;
    const size_t colon = value.rfind(L':');
    if (colon != std::wstring::npos) {
        const std::wstring portText = value.substr(colon + 1);
        if (!portText.empty()) {
            const bool allDigits =
                std::all_of(portText.begin(), portText.end(),
                            [](wchar_t character) { return character >= L'0' && character <= L'9'; });
            if (!allDigits) {
                return false;
            }
            wchar_t* end = nullptr;
            const unsigned long parsed = wcstoul(portText.c_str(), &end, 10);
            if (end == nullptr || *end != L'\0' || parsed == 0 || parsed > 65535) {
                return false;
            }
            port = static_cast<uint16_t>(parsed);
            value = value.substr(0, colon);
        } else {
            value = value.substr(0, colon);
        }
    }
    value = TrimText(value);
    if (value.empty()) {
        return false;
    }
    for (const wchar_t character : value) {
        const bool allowed = (character >= L'0' && character <= L'9') ||
                             (character >= L'a' && character <= L'z') ||
                             (character >= L'A' && character <= L'Z') || character == L'.' ||
                             character == L'-';
        if (!allowed) {
            return false;
        }
    }
    host = value;
    return true;
}

LanShareServer::LanShareServer() = default;

LanShareServer::~LanShareServer() {
    Stop();
}

bool LanShareServer::Running() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return running_;
}

uint16_t LanShareServer::Port() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return port_;
}

void LanShareServer::SetBooks(std::vector<SharedBook> books) {
    std::lock_guard<std::mutex> lock(mutex_);
    books_ = std::move(books);
}

bool LanShareServer::Start(uint16_t port, std::wstring& error) {
    error.clear();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_) {
            return true;
        }
    }
    WSADATA wsaData{};
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        error = L"网络组件初始化失败";
        return false;
    }
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        error = L"创建监听套接字失败";
        WSACleanup();
        return false;
    }
    BOOL reuse = TRUE;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
               sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);
    if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
        error = L"端口 " + std::to_wstring(port) + L" 无法绑定，可能已被其他程序占用";
        closesocket(listener);
        WSACleanup();
        return false;
    }
    if (listen(listener, SOMAXCONN) == SOCKET_ERROR) {
        error = L"启动监听失败";
        closesocket(listener);
        WSACleanup();
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        listener_ = static_cast<std::uintptr_t>(listener);
        port_ = port;
        stopping_ = false;
        running_ = true;
    }
    acceptThread_ = std::thread(&LanShareServer::AcceptLoop, this);
    for (int i = 0; i < 4; ++i) {
        workers_.emplace_back(&LanShareServer::WorkerLoop, this);
    }
    return true;
}

void LanShareServer::Stop() {
    std::deque<std::uintptr_t> pending;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) {
            return;
        }
        stopping_ = true;
        pending.swap(pendingClients_);
        // 正在传输的连接先关闭双向通道，阻塞中的发送或接收会立即返回。
        for (const std::uintptr_t client : activeClients_) {
            shutdown(static_cast<SOCKET>(client), SD_BOTH);
        }
    }
    condition_.notify_all();
    for (const std::uintptr_t client : pending) {
        closesocket(static_cast<SOCKET>(client));
    }
    if (acceptThread_.joinable()) {
        acceptThread_.join();
    }
    for (std::thread& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
        stopping_ = false;
    }
    WSACleanup();
}

void LanShareServer::AcceptLoop() {
    const SOCKET listener = static_cast<SOCKET>(listener_);
    while (true) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) {
                break;
            }
        }
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(listener, &readSet);
        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = 200000;
        const int ready = select(0, &readSet, nullptr, nullptr, &timeout);
        if (ready <= 0) {
            continue;
        }
        const SOCKET client = accept(listener, nullptr, nullptr);
        if (client == INVALID_SOCKET) {
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) {
                closesocket(client);
                break;
            }
            pendingClients_.push_back(static_cast<std::uintptr_t>(client));
        }
        condition_.notify_one();
    }
    closesocket(listener);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        listener_ = 0;
    }
}

void LanShareServer::WorkerLoop() {
    while (true) {
        std::uintptr_t client = 0;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            condition_.wait(lock, [this] { return stopping_ || !pendingClients_.empty(); });
            if (pendingClients_.empty()) {
                if (stopping_) {
                    return;
                }
                continue;
            }
            client = pendingClients_.front();
            pendingClients_.pop_front();
            activeClients_.push_back(client);
        }
        HandleClient(client);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto found = std::find(activeClients_.begin(), activeClients_.end(), client);
            if (found != activeClients_.end()) {
                activeClients_.erase(found);
            }
        }
        closesocket(static_cast<SOCKET>(client));
    }
}

void LanShareServer::HandleClient(std::uintptr_t clientValue) {
    const SOCKET client = static_cast<SOCKET>(clientValue);
    DWORD receiveTimeout = 5000;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&receiveTimeout), sizeof(receiveTimeout));
    DWORD sendTimeout = 30000;
    setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&sendTimeout),
               sizeof(sendTimeout));

    std::string request;
    char buffer[2048];
    while (request.size() < 16384) {
        const int received = recv(client, buffer, sizeof(buffer), 0);
        if (received <= 0) {
            break;
        }
        request.append(buffer, static_cast<size_t>(received));
        if (request.find("\r\n\r\n") != std::string::npos) {
            break;
        }
    }
    const size_t lineEnd = request.find("\r\n");
    if (lineEnd == std::string::npos) {
        SendTextResponse(clientValue, 400, "Bad Request", "text/plain; charset=utf-8",
                         "bad request");
        return;
    }
    std::istringstream stream(request.substr(0, lineEnd));
    std::string method;
    std::string target;
    std::string version;
    stream >> method >> target >> version;
    if (method != "GET") {
        SendTextResponse(clientValue, 405, "Method Not Allowed", "text/plain; charset=utf-8",
                         "only GET is supported");
        return;
    }
    const size_t queryStart = target.find('?');
    if (queryStart != std::string::npos) {
        target.resize(queryStart);
    }
    const std::string path = UrlDecode(target);
    if (path == "/api/ping") {
        SendTextResponse(clientValue, 200, "OK", "application/json; charset=utf-8",
                         "{\"app\":\"umd_reader\",\"version\":1}");
        return;
    }
    if (path == "/api/books") {
        SendTextResponse(clientValue, 200, "OK", "application/json; charset=utf-8",
                         BookListJson());
        return;
    }
    const std::string kBookPrefix = "/api/books/";
    const std::string kFileSuffix = "/file";
    if (path.rfind(kBookPrefix, 0) == 0 && path.size() > kBookPrefix.size() + kFileSuffix.size() &&
        path.compare(path.size() - kFileSuffix.size(), kFileSuffix.size(), kFileSuffix) == 0) {
        const std::string encodedId =
            path.substr(kBookPrefix.size(),
                        path.size() - kBookPrefix.size() - kFileSuffix.size());
        const std::wstring id = FromUtf8(encodedId);
        SharedBook book;
        bool found = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const SharedBook& candidate : books_) {
                if (candidate.id == id) {
                    book = candidate;
                    found = true;
                    break;
                }
            }
        }
        if (!found) {
            SendTextResponse(clientValue, 404, "Not Found", "text/plain; charset=utf-8",
                             "book not found");
            return;
        }
        SendFileResponse(clientValue, book);
        return;
    }
    SendTextResponse(clientValue, 404, "Not Found", "text/plain; charset=utf-8", "not found");
}

bool LanShareServer::SendBytes(std::uintptr_t clientValue, const char* data, size_t length) {
    const SOCKET client = static_cast<SOCKET>(clientValue);
    size_t offset = 0;
    while (offset < length) {
        const int chunk = static_cast<int>(std::min<size_t>(length - offset, 64 * 1024));
        const int sent = send(client, data + offset, chunk, 0);
        if (sent <= 0) {
            return false;
        }
        offset += static_cast<size_t>(sent);
    }
    return true;
}

bool LanShareServer::SendTextResponse(std::uintptr_t clientValue, int statusCode,
                                      const char* reason, const char* contentType,
                                      const std::string& body) {
    char header[512] = {};
    const int headerLength = _snprintf_s(
        header, sizeof(header), _TRUNCATE,
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n"
        "Cache-Control: no-store\r\n\r\n",
        statusCode, reason, contentType, body.size());
    if (headerLength <= 0) {
        return false;
    }
    if (!SendBytes(clientValue, header, static_cast<size_t>(headerLength))) {
        return false;
    }
    return body.empty() || SendBytes(clientValue, body.data(), body.size());
}

bool LanShareServer::SendFileResponse(std::uintptr_t clientValue, const SharedBook& book) {
    HANDLE file = CreateFileW(book.path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return SendTextResponse(clientValue, 404, "Not Found", "text/plain; charset=utf-8",
                                "book file unavailable");
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size)) {
        CloseHandle(file);
        return SendTextResponse(clientValue, 500, "Internal Server Error",
                                "text/plain; charset=utf-8", "cannot read file size");
    }
    const std::string fileName = PercentEncode(ToUtf8(book.fileName));
    char header[1024] = {};
    const int headerLength = _snprintf_s(
        header, sizeof(header), _TRUNCATE,
        "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: %lld\r\n"
        "Connection: close\r\nCache-Control: no-store\r\n"
        "Content-Disposition: attachment; filename*=UTF-8''%s\r\n\r\n",
        static_cast<long long>(size.QuadPart), fileName.c_str());
    if (headerLength <= 0 || !SendBytes(clientValue, header, static_cast<size_t>(headerLength))) {
        CloseHandle(file);
        return false;
    }
    std::vector<char> buffer(64 * 1024);
    DWORD read = 0;
    bool success = true;
    while (ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) &&
           read > 0) {
        if (!SendBytes(clientValue, buffer.data(), static_cast<size_t>(read))) {
            success = false;
            break;
        }
    }
    CloseHandle(file);
    return success;
}

std::string LanShareServer::BookListJson() const {
    std::vector<SharedBook> snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot = books_;
    }
    nlohmann::json list = nlohmann::json::array();
    for (const SharedBook& book : snapshot) {
        nlohmann::json item;
        item["id"] = ToUtf8(book.id);
        item["title"] = ToUtf8(book.title);
        item["author"] = ToUtf8(book.author);
        item["fileName"] = ToUtf8(book.fileName);
        item["fileSize"] = book.fileSize;
        item["formatType"] = book.formatType;
        item["chapterCount"] = book.chapterCount;
        list.push_back(std::move(item));
    }
    return list.dump();
}

bool LanShareClient::FetchBooks(const std::wstring& host, uint16_t port,
                                std::vector<RemoteBook>& books, std::wstring& error,
                                const std::atomic<bool>* cancel) {
    error.clear();
    books.clear();
    if (cancel != nullptr && cancel->load()) {
        error = L"已取消";
        return false;
    }
    HttpHandles handles;
    if (!OpenHttpGet(handles, host, port, L"/api/books", error)) {
        return false;
    }
    std::string body;
    DWORD available = 0;
    while (WinHttpQueryDataAvailable(handles.request, &available) && available > 0) {
        if (cancel != nullptr && cancel->load()) {
            error = L"已取消";
            return false;
        }
        std::string chunk(static_cast<size_t>(available), '\0');
        DWORD read = 0;
        if (!WinHttpReadData(handles.request, chunk.data(), available, &read)) {
            error = WinHttpErrorText(L"读取书单");
            return false;
        }
        chunk.resize(static_cast<size_t>(read));
        body += chunk;
        if (body.size() > 8u * 1024u * 1024u) {
            error = L"书单数据超过大小上限";
            return false;
        }
    }
    nlohmann::json parsed = nlohmann::json::parse(body, nullptr, false);
    if (parsed.is_discarded()) {
        error = L"书单数据格式无效";
        return false;
    }
    if (!parsed.is_array()) {
        error = L"书单数据不是书籍列表";
        return false;
    }
    for (const nlohmann::json& item : parsed) {
        if (!item.is_object()) {
            continue;
        }
        RemoteBook book;
        book.id = JsonWString(item, "id");
        if (book.id.empty()) {
            continue;
        }
        book.title = JsonWString(item, "title");
        book.author = JsonWString(item, "author");
        book.fileName = JsonWString(item, "fileName");
        book.fileSize = JsonUint64(item, "fileSize");
        book.formatType = JsonInt(item, "formatType");
        book.chapterCount = JsonInt(item, "chapterCount");
        books.push_back(std::move(book));
    }
    return true;
}

bool LanShareClient::DownloadBook(const std::wstring& host, uint16_t port,
                                  const RemoteBook& book, const std::filesystem::path& target,
                                  std::wstring& error, const std::atomic<bool>* cancel) {
    error.clear();
    if (cancel != nullptr && cancel->load()) {
        error = L"已取消";
        return false;
    }
    const std::wstring path = L"/api/books/" +
                              FromUtf8(PercentEncode(ToUtf8(book.id))) + L"/file";
    HttpHandles handles;
    if (!OpenHttpGet(handles, host, port, path, error)) {
        return false;
    }
    HANDLE file = CreateFileW(target.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = L"无法创建下载文件：" + target.wstring();
        return false;
    }
    bool completed = false;
    while (true) {
        if (cancel != nullptr && cancel->load()) {
            error = L"已取消";
            break;
        }
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(handles.request, &available)) {
            error = WinHttpErrorText(L"读取下载数据");
            break;
        }
        if (available == 0) {
            completed = true;
            break;
        }
        const DWORD chunkSize = std::min<DWORD>(available, 256 * 1024);
        std::vector<char> buffer(chunkSize);
        DWORD read = 0;
        if (!WinHttpReadData(handles.request, buffer.data(), chunkSize, &read)) {
            error = WinHttpErrorText(L"读取下载数据");
            break;
        }
        if (read == 0) {
            completed = true;
            break;
        }
        DWORD written = 0;
        if (!WriteFile(file, buffer.data(), read, &written, nullptr) || written != read) {
            error = L"写入下载文件失败";
            break;
        }
    }
    CloseHandle(file);
    if (!completed) {
        DeleteFileW(target.c_str());
        return false;
    }
    return true;
}

}  // namespace net
