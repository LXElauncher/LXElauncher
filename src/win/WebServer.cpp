#include "WebServer.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>

#include "Utf.h"

#pragma comment(lib, "ws2_32.lib")

namespace lxe {

namespace {

constexpr wchar_t kLocalHost[] = L"127.0.0.1";

// 简单的 HTTP 响应行 + 头部拼接。
std::string BuildResponseHeader(int status,
                                 const std::string& reason,
                                 const std::string& content_type,
                                 size_t content_length) {
    std::ostringstream oss;
    oss << "HTTP/1.1 " << status << " " << reason << "\r\n"
        << "Content-Type: " << content_type << "\r\n"
        << "Content-Length: " << content_length << "\r\n"
        << "Connection: close\r\n"
        << "Cache-Control: no-store\r\n"
        << "\r\n";
    return oss.str();
}

// 读取整个文件为字节序列。失败时返回空 vector 且 ok=false。
bool ReadFileAll(const std::wstring& path, std::vector<char>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.unsetf(std::ios::skipws);
    f.seekg(0, std::ios::end);
    auto size = f.tellg();
    if (size < 0) return false;
    f.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(size));
    if (size > 0) {
        f.read(out.data(), size);
    }
    return f.good() || f.eof();
}

} // namespace

WebServer::~WebServer() {
    Stop();
}

bool WebServer::Start(const std::wstring& rootFolder) {
    if (listen_sock_ != INVALID_SOCKET) return true;  // 已启动

    WSADATA wsaData{};
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) return false;

    root_folder_ = rootFolder;

    listen_sock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock_ == INVALID_SOCKET) {
        WSACleanup();
        return false;
    }

    // 允许快速重用地址
    BOOL reuse = TRUE;
    setsockopt(listen_sock_, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    // 用 InetPton 替代已弃用的 inet_addr
    if (InetPton(AF_INET, kLocalHost, &addr.sin_addr) != 1) {
        closesocket(listen_sock_);
        listen_sock_ = INVALID_SOCKET;
        WSACleanup();
        return false;
    }
    addr.sin_port = htons(0);  // 由系统分配随机端口

    if (bind(listen_sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        closesocket(listen_sock_);
        listen_sock_ = INVALID_SOCKET;
        WSACleanup();
        return false;
    }

    sockaddr_in bound{};
    int boundLen = sizeof(bound);
    if (getsockname(listen_sock_, reinterpret_cast<sockaddr*>(&bound), &boundLen) == SOCKET_ERROR) {
        closesocket(listen_sock_);
        listen_sock_ = INVALID_SOCKET;
        WSACleanup();
        return false;
    }

    if (listen(listen_sock_, SOMAXCONN) == SOCKET_ERROR) {
        closesocket(listen_sock_);
        listen_sock_ = INVALID_SOCKET;
        WSACleanup();
        return false;
    }

    int port = ntohs(bound.sin_port);
    wchar_t buf[64];
    swprintf_s(buf, L"http://%s:%d/", kLocalHost, port);
    base_url_ = buf;

    stopping_ = false;
    thread_ = std::thread([this] { ListenLoop(); });
    return true;
}

void WebServer::Stop() {
    if (listen_sock_ == INVALID_SOCKET) return;

    stopping_ = true;
    // 关闭监听 socket 以唤醒 accept()，使工作线程退出
    shutdown(listen_sock_, SD_BOTH);
    closesocket(listen_sock_);
    listen_sock_ = INVALID_SOCKET;

    if (thread_.joinable()) thread_.join();

    WSACleanup();
    base_url_.clear();
}

bool WebServer::IsOwnUri(const std::wstring& uri) const {
    if (base_url_.empty()) return false;
    return uri.rfind(base_url_, 0) == 0;
}

void WebServer::ListenLoop() {
    while (!stopping_) {
        sockaddr_in clientAddr{};
        int clientLen = sizeof(clientAddr);
        SOCKET client = accept(listen_sock_,
                               reinterpret_cast<sockaddr*>(&clientAddr),
                               &clientLen);
        if (client == INVALID_SOCKET) break;
        if (stopping_) {
            closesocket(client);
            break;
        }
        // 每个连接用一个独立短线程处理；浏览器一次连接只发一个请求。
        std::thread([this, client] {
            HandleClient(client);
            closesocket(client);
        }).detach();
    }
}

void WebServer::HandleClient(SOCKET client) {
    char buf[8192];
    std::string request;
    while (request.find("\r\n\r\n") == std::string::npos) {
        int n = recv(client, buf, sizeof(buf), 0);
        if (n <= 0) return;
        request.append(buf, buf + n);
        if (request.size() > 16384) return;  // 防止异常大请求
    }

    // 解析请求行: "GET /path HTTP/1.1"
    std::string path;
    {
        std::istringstream iss(request);
        std::string method, full_path, version;
        iss >> method >> full_path >> version;
        if (method != "GET" && method != "HEAD") {
            std::string body = "405 Method Not Allowed";
            std::string header = BuildResponseHeader(405, "Method Not Allowed",
                                                     "text/plain; charset=utf-8", body.size());
            send(client, header.data(), static_cast<int>(header.size()), 0);
            send(client, body.data(), static_cast<int>(body.size()), 0);
            return;
        }
        path = full_path;
    }

    std::wstring file_path;
    std::string mime_type;
    bool ok = ResolveRequest(path, file_path, mime_type);

    if (!ok) {
        std::string body = "404 Not Found";
        std::string header = BuildResponseHeader(404, "Not Found",
                                                 "text/plain; charset=utf-8", body.size());
        send(client, header.data(), static_cast<int>(header.size()), 0);
        send(client, body.data(), static_cast<int>(body.size()), 0);
        return;
    }

    std::vector<char> body;
    if (!ReadFileAll(file_path, body)) {
        std::string err = "500 Internal Server Error";
        std::string header = BuildResponseHeader(500, "Internal Server Error",
                                                 "text/plain; charset=utf-8", err.size());
        send(client, header.data(), static_cast<int>(header.size()), 0);
        send(client, err.data(), static_cast<int>(err.size()), 0);
        return;
    }

    std::string header = BuildResponseHeader(200, "OK", mime_type, body.size());
    send(client, header.data(), static_cast<int>(header.size()), 0);
    if (!body.empty()) {
        send(client, body.data(), static_cast<int>(body.size()), 0);
    }
}

bool WebServer::ResolveRequest(const std::string& request_path,
                                std::wstring& file_path,
                                std::string& mime_type) const {
    // 取 ? 之前的部分
    std::string p = request_path;
    auto q = p.find('?');
    if (q != std::string::npos) p = p.substr(0, q);

    // URL 解码
    std::wstring decoded = UrlDecode(p);

    // 路径规范化：以 / 开头，去除前后斜杠
    if (decoded.empty() || decoded[0] != L'/') return false;
    while (decoded.size() > 1 && decoded.back() == L'/') decoded.pop_back();
    if (decoded.empty()) decoded = L"/";

    // 默认文档
    std::wstring rel = decoded;
    if (rel == L"/") rel = L"/index.html";

    // 路径遍历防护：禁止出现 .. 或绝对路径
    if (rel.find(L"..") != std::wstring::npos) return false;

    // 拼接磁盘路径
    std::filesystem::path full = std::filesystem::path(root_folder_) / rel.substr(1);
    // 进一步校验：规范化后必须位于 root_folder_ 之内
    std::error_code ec;
    auto canonical_root = std::filesystem::weakly_canonical(root_folder_, ec);
    if (ec) return false;
    auto canonical_full = std::filesystem::weakly_canonical(full, ec);
    if (ec) return false;
    auto rel_check = std::filesystem::relative(canonical_full, canonical_root, ec);
    if (ec) return false;
    std::wstring rel_str = rel_check.wstring();
    if (!rel_str.empty() && rel_str[0] == L'.') {
        // 例如 ".." 或 "\.." 形式
        if (rel_str.size() >= 2 && rel_str[0] == L'.' && rel_str[1] == L'.') return false;
    }
    // canonical_root 内的相对路径不应以 .. 开头
    if (rel_str.rfind(L"..", 0) == 0) return false;

    if (!std::filesystem::is_regular_file(canonical_full, ec)) return false;

    file_path = canonical_full.wstring();
    mime_type = GuessMimeType(canonical_full);
    return true;
}

std::string WebServer::GuessMimeType(const std::wstring& path) {
    static const struct { const wchar_t* ext; const char* mime; } table[] = {
        { L".html", "text/html; charset=utf-8" },
        { L".htm",  "text/html; charset=utf-8" },
        { L".css",  "text/css; charset=utf-8" },
        { L".js",   "application/javascript; charset=utf-8" },
        { L".mjs",  "application/javascript; charset=utf-8" },
        { L".json", "application/json; charset=utf-8" },
        { L".svg",  "image/svg+xml" },
        { L".png",  "image/png" },
        { L".jpg",  "image/jpeg" },
        { L".jpeg", "image/jpeg" },
        { L".gif",  "image/gif" },
        { L".webp", "image/webp" },
        { L".ico",  "image/x-icon" },
        { L".woff", "font/woff" },
        { L".woff2","font/woff2" },
        { L".ttf",  "font/ttf" },
        { L".otf",  "font/otf" },
        { L".txt",  "text/plain; charset=utf-8" },
        { L".wasm", "application/wasm" },
    };
    std::wstring ext = std::filesystem::path(path).extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
    for (const auto& e : table) {
        if (ext == e.ext) return e.mime;
    }
    return "application/octet-stream";
}

std::wstring WebServer::UrlDecode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '+') {
            out.push_back(' ');
        } else if (c == '%' && i + 2 < s.size()) {
            auto hex = [](char ch) -> int {
                if (ch >= '0' && ch <= '9') return ch - '0';
                if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
                if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
                return -1;
            };
            int hi = hex(s[i + 1]);
            int lo = hex(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
            } else {
                out.push_back(c);
            }
        } else {
            out.push_back(c);
        }
    }
    // 字节序列按 UTF-8 解码为宽字符
    return Utf8ToWide(out);
}

} // namespace lxe
