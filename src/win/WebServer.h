#pragma once

#include <atomic>
#include <string>
#include <thread>
#include <vector>

// WinSock2 头文件需在 windows.h 之前包含以避免 winsock.h 冲突
#include <winsock2.h>
#include <ws2tcpip.h>

namespace lxe {

// 内置 HTTP 服务器：监听 127.0.0.1:<随机端口>，对外提供 webapp/ 目录的静态文件。
// 仅服务于本进程内的 WebView2，不对外暴露。
class WebServer {
public:
    WebServer() = default;
    ~WebServer();

    WebServer(const WebServer&) = delete;
    WebServer& operator=(const WebServer&) = delete;

    // 启动服务器。rootFolder 为 webapp 根目录（绝对路径）。
    // 内部绑定 127.0.0.1:0 由系统分配随机端口，可通过 BaseUrl() 获取。
    // 返回 false 表示启动失败（WSAStartup / socket / bind / listen 任一失败）。
    bool Start(const std::wstring& rootFolder);

    // 停止服务器并释放监听 socket 与工作线程。
    void Stop();

    // 返回形如 "http://127.0.0.1:51837/" 的根 URL；未启动时返回空串。
    const std::wstring& BaseUrl() const { return base_url_; }

    // 判断某 URL 是否本服务器（前缀匹配 BaseUrl）。
    bool IsOwnUri(const std::wstring& uri) const;

private:
    void ListenLoop();
    void HandleClient(SOCKET client);

    // 将请求路径映射到磁盘文件路径，并返回其 MIME 类型。
    // 成功返回 true 且填入 file_path；否则返回 false。
    bool ResolveRequest(const std::string& request_path,
                        std::wstring& file_path,
                        std::string& mime_type) const;

    static std::string GuessMimeType(const std::wstring& path);
    static std::wstring UrlDecode(const std::string& s);

    std::wstring root_folder_;
    std::wstring base_url_;  // 形如 "http://127.0.0.1:51837/"
    SOCKET listen_sock_ = INVALID_SOCKET;
    std::thread thread_;
    std::atomic<bool> stopping_{ false };
};

} // namespace lxe
