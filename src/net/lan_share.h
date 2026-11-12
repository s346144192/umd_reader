#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace net {

// 服务端对外公布的一本书：只包含书单展示需要的元数据与本机文件路径。
struct SharedBook {
    std::wstring id;
    std::wstring title;
    std::wstring author;
    std::wstring fileName;
    std::filesystem::path path;
    uint64_t fileSize = 0;
    int formatType = 0;  // 0=txt，1=UMD 文本，2=UMD 动漫，3=PDF
    int chapterCount = 0;
};

// 客户端从远端书单解析出的书籍条目。
struct RemoteBook {
    std::wstring id;
    std::wstring title;
    std::wstring author;
    std::wstring fileName;
    uint64_t fileSize = 0;
    int formatType = 0;
    int chapterCount = 0;
};

// 本机 IPv4 接口：地址与子网前缀长度，用于确定需要扫描的局域网网段。
struct LanInterface {
    std::wstring address;
    uint8_t prefixLength = 24;
};

// 返回本机所有已启用的 IPv4 接口（不含回环地址），局域网接口排在前面。
std::vector<LanInterface> LocalIpv4Interfaces();

// 返回本机所有已启用的 IPv4 地址（不含回环地址），局域网地址排在前面。
std::vector<std::wstring> LocalIpv4Addresses();

// 扫描局域网中运行本程序分享服务的设备，结果按地址数值升序排列。
// 每个网段最多扫描本机所在的 /24 范围，避免大型网络扫描耗时过长。
bool ScanLanHosts(uint16_t port, uint32_t connectTimeoutMs, std::vector<std::wstring>& hosts,
                  std::wstring& error, const std::atomic<bool>* cancel = nullptr);

// 探测单个地址是否为运行本程序的分享服务。
bool ProbeLanShareHost(const std::wstring& address, uint16_t port,
                       uint32_t connectTimeoutMs);

// 解析「主机」或「主机:端口」形式的分享地址；未写端口时使用 defaultPort。
bool ParseShareAddress(const std::wstring& text, uint16_t defaultPort, std::wstring& host,
                       uint16_t& port);

// 局域网书籍分享服务：监听所有网卡，提供书单查询与原始文件下载的只读 HTTP 接口。
class LanShareServer {
public:
    LanShareServer();
    ~LanShareServer();

    LanShareServer(const LanShareServer&) = delete;
    LanShareServer& operator=(const LanShareServer&) = delete;

    bool Start(uint16_t port, std::wstring& error);
    void Stop();
    bool Running() const;
    uint16_t Port() const;
    // 替换对外分享的书籍列表；服务运行期间可随时调用。
    void SetBooks(std::vector<SharedBook> books);

private:
    void AcceptLoop();
    void WorkerLoop();
    void HandleClient(std::uintptr_t clientValue);
    bool SendBytes(std::uintptr_t clientValue, const char* data, size_t length);
    bool SendTextResponse(std::uintptr_t clientValue, int statusCode, const char* reason,
                          const char* contentType, const std::string& body);
    bool SendFileResponse(std::uintptr_t clientValue, const SharedBook& book);
    std::string BookListJson() const;

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<std::uintptr_t> pendingClients_;
    std::vector<std::uintptr_t> activeClients_;
    std::vector<SharedBook> books_;
    std::thread acceptThread_;
    std::vector<std::thread> workers_;
    std::uintptr_t listener_ = 0;
    uint16_t port_ = 0;
    bool running_ = false;
    bool stopping_ = false;
};

// 局域网客户端：通过 WinHTTP 获取远端书单并下载原始书籍文件。
class LanShareClient {
public:
    static bool FetchBooks(const std::wstring& host, uint16_t port,
                           std::vector<RemoteBook>& books, std::wstring& error,
                           const std::atomic<bool>* cancel = nullptr);
    // 把远端书籍下载到 target；失败时删除未完成的文件。
    static bool DownloadBook(const std::wstring& host, uint16_t port, const RemoteBook& book,
                             const std::filesystem::path& target, std::wstring& error,
                             const std::atomic<bool>* cancel = nullptr);
};

}  // namespace net
