#include "Services.h"

// BMCLAPI OptiFine 版本目录特例（temp/OptiFine设计-净室版.md §3.2）：MC 1.8/1.9 这类无补丁号版本
// （启动器版本 id 即 "1.8"）在 BMCLAPI /optifine/ 目录中按三位写作 1.8.0 / 1.9.0，需补零才能命中。
// 仅限次版本 0~9 的二段式（1.0~1.9 首版 id 缩写）；1.10/1.11/1.12 等次版本 >=10 的独立版本 id 不补零。
static std::string BmclOptifineVersionPath(const std::string& mcVersion) {
    size_t p1 = mcVersion.find('.');
    if (p1 == std::string::npos) return mcVersion;
    size_t p2 = mcVersion.find('.', p1 + 1);
    if (p2 != std::string::npos) return mcVersion; // 已是三位
    std::string minor = mcVersion.substr(p1 + 1);
    if (minor.empty()) return mcVersion;
    int m = 0;
    for (char c : minor) { if (c < '0' || c > '9') return mcVersion; m = m * 10 + (c - '0'); }
    if (m < 10) return mcVersion + ".0";
    return mcVersion;
}

// MC 版本归一化比较：1.8 与 1.8.0 视为同一版本（官方文件名内嵌的 MC 号可能比用户版本 id 多补丁位）
static std::string NormalizeMcVersionForCompare(const std::string& v) {
    return BmclOptifineVersionPath(v);
}

#include <atomic>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cwctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <regex>
#include <sstream>
#include <thread>
#include <vector>

#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

#include <shlobj.h>
#pragma comment(lib, "shell32.lib")

#include <wincrypt.h>
#pragma comment(lib, "advapi32.lib")

#include "../win/Utf.h"

namespace lxe {

// 前向声明：Java 扫描 / 用户设置持久化 的这些函数定义在本文件后部，
// 但异步扫描任务在 2361-2479 附近（更靠前）就会引用，故在此预声明。
// InstalledJava 在此完整定义，避免匿名命名空间内 std::vector<InstalledJava> 依赖不完整类型。
struct InstalledJava {
    std::wstring path;
    std::wstring version;
    int major = 0;
    bool is64 = true;
    std::wstring fileEncoding;
    std::wstring nativeEncoding;
};
static Json LoadSettingsFile();
static Json SaveSettingsFile(const Json& patch);

// 模组/资源包/光影 子目录映射（kind: mods | resourcepacks | shaderpacks）
static std::wstring PackSubDirOf(const std::string& kind) {
    if (kind == "resourcepacks") return L"resourcepacks";
    if (kind == "shaderpacks") return L"shaderpacks";
    return L"mods";
}

// 游戏版本清单路径（由 LXElauncher.cpp 设置）
static std::wstring g_manifestPath;
void SetManifestPath(const std::wstring& manifestJsonPath) {
    g_manifestPath = manifestJsonPath;
}

// exe 同级目录（用于查找 aria2c.exe、build_counter.txt 等工具/数据）
static std::wstring g_exeDir;
void SetExeDir(const std::wstring& exeDir) {
    g_exeDir = exeDir;
}

// 窗口圆角档位（跨 RPC/WM_SIZE 共享；启动时从 settings.json 读取，默认 medium）
// "none" | "small" | "medium" | "large"
static std::mutex g_cornerMu;
static std::string g_cornerMode = "medium";

// 与网页 CSS 最大圆角（16~18px）匹配；DPI 感知下在 ApplyWindowCorner 中用 MulDiv 缩放
static constexpr int kLargeCornerRadius = 24;

// .minecraft 根目录：默认 exe 同级 .minecraft/，可通过 mc.setMcRoot 切换（导入）
static std::wstring g_mcRoot;
static std::wstring GetMcRoot() {
    if (!g_mcRoot.empty()) return g_mcRoot;
    if (g_exeDir.empty()) return L".minecraft";
    std::wstring newDir = g_exeDir + L"\\.minecraft";
    // 迁移：如果旧 mc 文件夹存在且 .minecraft 不存在，则重命名
    std::wstring oldDir = g_exeDir + L"\\mc";
    if (std::filesystem::is_directory(oldDir) && !std::filesystem::is_directory(newDir)) {
        std::error_code ec;
        std::filesystem::rename(oldDir, newDir, ec);
    }
    return newDir;
}

// 版本隔离下的包文件目录：version 非空 → .minecraft/versions/<version>/<kind>；否则全局 .minecraft/<kind>。
// 仅接收安全的版本名（拒绝路径穿越与非法 Windows 文件名字符）。
static std::wstring PackDirFor(const std::string& kind, const std::string& version) {
    std::wstring base = GetMcRoot();
    if (!version.empty()) {
        std::wstring wv = lxe::Utf8ToWide(version);
        if (wv.find(L"..") == std::wstring::npos && wv.find_first_of(L"\\/:*?\"<>|") == std::wstring::npos)
            base = base + L"\\versions\\" + wv;
    }
    return base + L"\\" + PackSubDirOf(kind);
}

// 静默执行命令行（不弹出 CMD 窗口），等待完成
static bool RunSilent(const std::wstring& cmdLine) {
    STARTUPINFOW si{}; si.cb = sizeof(si);
    // 注意：CREATE_NO_WINDOW 已阻止控制台窗口创建，无需 STARTF_USESHOWWINDOW
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cmdLine.begin(), cmdLine.end());
    buf.push_back(L'\0');
    BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (!ok) return false;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return exitCode == 0;
}

// 静默执行命令行（支持工作目录、环境变量覆盖），返回退出码（<0 表示启动失败）
static int RunProcessSilent(const std::wstring& cmdLine,
                            const std::wstring& workDir = L"",
                            const std::vector<std::pair<std::wstring, std::wstring>>& envOverrides = {}) {
    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cmdLine.begin(), cmdLine.end());
    buf.push_back(L'\0');
    void* envBlock = nullptr;
    std::wstring envData;
    if (!envOverrides.empty()) {
        // 读取当前环境，应用覆盖后构造 UNICODE 环境块（以 \0 双终止）
        std::map<std::wstring, std::wstring> env;
        wchar_t* curEnv = GetEnvironmentStringsW();
        if (curEnv) {
            for (wchar_t* p = curEnv; *p; p += wcslen(p) + 1) {
                const wchar_t* eq = wcschr(p, L'=');
                if (eq) env[std::wstring(p, eq - p)] = std::wstring(eq + 1);
            }
            FreeEnvironmentStringsW(curEnv);
        }
        for (const auto& kv : envOverrides) env[kv.first] = kv.second;
        for (const auto& kv : env) {
            envData += kv.first + L"=" + kv.second + L"\0";
        }
        envData += L"\0";
        envBlock = envData.data();
    }
    BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
                             CREATE_NO_WINDOW | (envOverrides.empty() ? 0 : CREATE_UNICODE_ENVIRONMENT),
                             envBlock, workDir.empty() ? nullptr : workDir.c_str(), &si, &pi);
    if (!ok) return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return static_cast<int>(exitCode);
}

// 静默执行命令行并捕获标准输出+标准错误（合并，UTF-8），支持环境变量覆盖（同 RunProcessSilent）。
// 用于安装器副作用监控（设计文档 §4.1.5）：实时消费输出流防止管道阻塞，并检查异常堆栈判断失败。
// 返回退出码（<0 表示启动失败），输出文本写入 outText。
static int RunProcessCaptureEx(const std::wstring& cmdLine,
                               const std::wstring& workDir,
                               const std::vector<std::pair<std::wstring, std::wstring>>& envOverrides,
                               std::string& outText) {
    SECURITY_ATTRIBUTES sa{}; sa.nLength = sizeof(sa); sa.bInheritHandle = TRUE;
    HANDLE hRead = nullptr, hWrite = nullptr;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) return -1;
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hWrite;
    si.hStdError = hWrite;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cmdLine.begin(), cmdLine.end());
    buf.push_back(L'\0');
    void* envBlock = nullptr;
    std::wstring envData;
    if (!envOverrides.empty()) {
        std::map<std::wstring, std::wstring> env;
        wchar_t* curEnv = GetEnvironmentStringsW();
        if (curEnv) {
            for (wchar_t* p = curEnv; *p; p += wcslen(p) + 1) {
                const wchar_t* eq = wcschr(p, L'=');
                if (eq) env[std::wstring(p, eq - p)] = std::wstring(eq + 1);
            }
            FreeEnvironmentStringsW(curEnv);
        }
        for (const auto& kv : envOverrides) env[kv.first] = kv.second;
        for (const auto& kv : env) envData += kv.first + L"=" + kv.second + L"\0";
        envData += L"\0";
        envBlock = envData.data();
    }
    BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW | (envOverrides.empty() ? 0 : CREATE_UNICODE_ENVIRONMENT),
                             envBlock, workDir.empty() ? nullptr : workDir.c_str(), &si, &pi);
    CloseHandle(hWrite);
    if (!ok) { CloseHandle(hRead); return -1; }
    std::string data;
    char tmp[4096];
    DWORD rd = 0;
    while (ReadFile(hRead, tmp, sizeof(tmp), &rd, nullptr) && rd > 0) data.append(tmp, rd);
    CloseHandle(hRead);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    outText = std::move(data);
    return static_cast<int>(exitCode);
}

// 运行命令并捕获标准输出+标准错误（合并，UTF-8），用于探测 java -version 等
static std::wstring RunCapture(const std::wstring& cmdLine) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE hRead = nullptr, hWrite = nullptr;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) return L"";
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = hWrite;
    si.hStdError = hWrite;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cmdLine.begin(), cmdLine.end());
    buf.push_back(L'\0');
    BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(hWrite);
    if (!ok) { CloseHandle(hRead); return L""; }
    std::string data;
    char tmp[4096];
    DWORD rd = 0;
    while (ReadFile(hRead, tmp, sizeof(tmp), &rd, nullptr) && rd > 0) data.append(tmp, rd);
    CloseHandle(hRead);
    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    int wlen = MultiByteToWideChar(CP_UTF8, 0, data.c_str(), (int)data.size(), nullptr, 0);
    std::wstring res(wlen, L'\0');
    if (wlen > 0) MultiByteToWideChar(CP_UTF8, 0, data.c_str(), (int)data.size(), &res[0], wlen);
    return res;
}

// Maven 坐标 "group:artifact:version[:classifier]" -> 库文件相对路径（Part 1/3 规则）
// 例：org.ow2.asm:asm:9.7.1 -> org/ow2/asm/asm/9.7.1/asm-9.7.1.jar
static std::string MavenCoordToPath(const std::string& coord) {
    if (coord.empty()) return coord;
    std::string base = coord, ext = "jar";
    if (coord.find('@') != std::string::npos) {
        size_t at = coord.rfind('@');
        base = coord.substr(0, at);
        ext = coord.substr(at + 1);
        if (ext.empty()) ext = "jar";
    }
    std::vector<std::string> sp;
    std::istringstream iss(base);
    std::string tok;
    while (std::getline(iss, tok, ':')) sp.push_back(tok);
    if (sp.size() < 3) return "";
    std::string pkg = sp[0];
    for (char& c : pkg) if (c == '.') c = '/';
    std::string res = pkg + "/" + sp[1] + "/" + sp[2] + "/" + sp[1] + "-" + sp[2];
    for (size_t i = 3; i < sp.size(); ++i) res += "-" + sp[i];
    return res + "." + ext;
}

// 游戏进程跟踪（多实例支持）
static std::vector<std::pair<HANDLE, DWORD>> g_gameInstances;
static std::mutex g_gameInstancesMutex;
static std::atomic<bool> g_monitorRunning{false};
// 最近一次启动游戏使用的 Java 主版本与实际游戏目录，崩溃事件携带给插件做精确判定
static std::atomic<int> g_lastLaunchJavaMajor{0};
static std::wstring g_lastLaunchGameDir;
static std::mutex g_lastLaunchMu;
static std::thread g_monitorThread;
static Bridge* g_bridgeForMonitor = nullptr;

// 构建版本号/构建时间 —— 计数器持久化到 build_counter.txt
// 优先级：exe 同级 build_counter.txt → temp\build_counter.txt
// 格式：<YYYYMMDD> <计数>，每构建日首次重置为 1，同日内连续构建每次 +1
struct BuildMeta {
    std::string buildNumber; // 20260812-03
    std::string buildTime;   // 2026-08-12 10:30:15
};
static BuildMeta g_buildMeta{};

// ============ 下载任务级取消标志注册表 ============
// submitDownloadList / CompleteVersionFilesWorker / java.download / mc.javaAutoInstall /
// auth.ensureInjector 按 taskId 在此登记取消标志；download.cancel RPC 置位后，
// 各 worker 的 aria2 进度回调返回 false → TerminateProcess 终止下载进程。
static std::mutex g_dlCancelMu;
static std::map<std::string, std::shared_ptr<std::atomic<bool>>> g_dlCancelFlags;

// 获取/登记某任务的取消标志（不存在则新建）。返回的 shared_ptr 生命周期不受任务结束影响。
static std::shared_ptr<std::atomic<bool>> DLCancelFlag(const std::string& taskId) {
    std::lock_guard<std::mutex> lock(g_dlCancelMu);
    auto it = g_dlCancelFlags.find(taskId);
    if (it != g_dlCancelFlags.end()) return it->second;
    auto f = std::make_shared<std::atomic<bool>>(false);
    g_dlCancelFlags[taskId] = f;
    return f;
}
// 任务结束后清理登记，避免 registry 无限增长
static void DLCancelFlagRemove(const std::string& taskId) {
    std::lock_guard<std::mutex> lock(g_dlCancelMu);
    g_dlCancelFlags.erase(taskId);
}
// download.cancel 使用：置位指定任务的取消标志
static void DLCancelFlagSet(const std::string& taskId) {
    std::lock_guard<std::mutex> lock(g_dlCancelMu);
    auto it = g_dlCancelFlags.find(taskId);
    if (it != g_dlCancelFlags.end()) it->second->store(true);
}

static int GetAndBumpDailyCounter(const std::string& ymd) {

    if (g_exeDir.empty()) return 1;
    std::wstring counterPath = g_exeDir + L"\\build_counter.txt";
    // 也尝试项目根 temp 目录（开发期 exe 在 x64\Debug\ 下）
    if (GetFileAttributesW(counterPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        counterPath = g_exeDir + L"\\..\\..\\temp\\build_counter.txt";
    }
    int prevCount = 0;
    std::string prevDate;
    {
        std::ifstream ifs(counterPath);
        if (ifs.is_open()) {
            std::string line;
            if (std::getline(ifs, line)) {
                std::istringstream iss(line);
                iss >> prevDate >> prevCount;
            }
        }
    }
    int newCount = (prevDate == ymd) ? prevCount + 1 : 1;
    if (newCount < 1) newCount = 1;
    {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(counterPath).parent_path(), ec);
        std::ofstream ofs(counterPath, std::ios::trunc);
        if (ofs.is_open()) {
            ofs << ymd << ' ' << newCount << '\n';
        }
    }
    return newCount;
}
static BuildMeta ComputeBuildMeta() {
    BuildMeta m{};
    auto tp = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
    localtime_s(&tm, &t);
    {
        std::ostringstream ss;
        ss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
        m.buildTime = ss.str();
    }
    std::ostringstream ymdSS;
    ymdSS << std::put_time(&tm, "%Y%m%d");
    std::string ymd = ymdSS.str();
    int dailyCount = GetAndBumpDailyCounter(ymd);
    {
        std::ostringstream ss;
        ss << ymd << '-' << std::setfill('0') << std::setw(2) << dailyCount;
        m.buildNumber = ss.str();
    }
    return m;
}
static BuildMeta& GetBuildMeta() {
    static BuildMeta once = ComputeBuildMeta();
    return once;
}

// 检查 aria2c.exe 是否存在于 exe 同级目录
static bool IsAria2Available() {
    if (g_exeDir.empty()) return false;
    std::wstring path = g_exeDir + L"\\aria2c.exe";
    DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

// §5 文件下载源开关（定义见 settings 区）：官方/镜像/自动
static std::string FileUrlForDownload(const std::string& url);
// §5.4 限速设置（定义见 settings 区）：返回 0.1 MB/s 的倍数，0 表示不限速
static int SettingInt(const std::string& key, int def);

// 解析 URL 为 host + path，供 WinHTTP 使用
struct UrlParts {
    std::wstring host;
    std::wstring path;
    int port = 0;
    bool https = false;
};
static bool ParseUrl(const std::wstring& url, UrlParts& out) {
    if (url.rfind(L"https://", 0) == 0) {
        out.https = true; out.port = 443;
        out.host = url.substr(8);
    } else if (url.rfind(L"http://", 0) == 0) {
        out.https = false; out.port = 80;
        out.host = url.substr(7);
    } else {
        return false;
    }
    auto slash = out.host.find(L'/');
    if (slash == std::wstring::npos) {
        out.path = L"/";
    } else {
        out.path = out.host.substr(slash);
        out.host = out.host.substr(0, slash);
    }
    auto colon = out.host.find(L':');
    if (colon != std::wstring::npos) {
        std::wstring portStr = out.host.substr(colon + 1);
        try { out.port = std::stoi(portStr); } catch (...) { return false; }
        out.host = out.host.substr(0, colon);
    }
    return true;
}

// 用 WinHTTP 下载文本内容（用于版本 JSON 等小文件）
static std::string UrlEncode(const std::string& s) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    return out;
}

static std::string HttpFetchText(const std::wstring& url) {
    UrlParts parts;
    if (!ParseUrl(url, parts)) return {};
    HINTERNET hSession = WinHttpOpen(L"LXElauncher/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return {};
    HINTERNET hConnect = WinHttpConnect(hSession, parts.host.c_str(),
        parts.port, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return {}; }
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", parts.path.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        parts.https ? WINHTTP_FLAG_SECURE : 0);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return {}; }
    BOOL bResult = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (!bResult) { WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return {}; }
    if (!WinHttpReceiveResponse(hRequest, nullptr)) {
        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return {};
    }
    std::string body;
    DWORD dwSize = 0;
    do {
        DWORD dwDownloaded = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &dwSize)) break;
        if (dwSize == 0) break;
        std::vector<char> buf(dwSize);
        if (!WinHttpReadData(hRequest, buf.data(), dwSize, &dwDownloaded)) break;
        body.append(buf.data(), dwDownloaded);
    } while (dwSize > 0);
    WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
    return body;
}

// 用 WinHTTP 拉取文本（带自定义请求头，如 CurseForge 的 x-api-key）
static std::string HttpFetchTextWithHeader(const std::wstring& url, const std::wstring& header) {
    UrlParts parts;
    if (!ParseUrl(url, parts)) return {};
    HINTERNET hSession = WinHttpOpen(L"LXElauncher/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return {};
    HINTERNET hConnect = WinHttpConnect(hSession, parts.host.c_str(),
        parts.port, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return {}; }
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", parts.path.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        parts.https ? WINHTTP_FLAG_SECURE : 0);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return {}; }
    LPCWSTR rawHeaders = header.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : header.c_str();
    DWORD headerLen = header.empty() ? 0 : (DWORD)header.size();
    BOOL bResult = WinHttpSendRequest(hRequest, rawHeaders, headerLen,
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (!bResult) { WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return {}; }
    if (!WinHttpReceiveResponse(hRequest, nullptr)) {
        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return {};
    }
    DWORD statusCode = 0, statusLen = sizeof(statusCode);
    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusLen, WINHTTP_NO_HEADER_INDEX);
    if (statusCode != 200) {
        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
        return {};
    }
    std::string body;
    DWORD dwSize = 0;
    do {
        DWORD dwDownloaded = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &dwSize)) break;
        if (dwSize == 0) break;
        std::vector<char> buf(dwSize);
        if (!WinHttpReadData(hRequest, buf.data(), dwSize, &dwDownloaded)) break;
        body.append(buf.data(), dwDownloaded);
    } while (dwSize > 0);
    WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
    return body;
}

// 用 WinHTTP 下载文件到指定路径（回退方案，当 aria2 不可用时）
static bool HttpDownloadFile(const std::wstring& url, const std::wstring& filePath) {
    UrlParts parts;
    if (!ParseUrl(url, parts)) return false;
    HINTERNET hSession = WinHttpOpen(L"LXElauncher/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;
    HINTERNET hConnect = WinHttpConnect(hSession, parts.host.c_str(), parts.port, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return false; }
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", parts.path.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        parts.https ? WINHTTP_FLAG_SECURE : 0);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false; }
    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false;
    }
    if (!WinHttpReceiveResponse(hRequest, nullptr)) {
        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false;
    }
    // 确保目录存在
    std::filesystem::path p(filePath);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path());
    std::ofstream ofs(filePath, std::ios::binary);
    if (!ofs.is_open()) {
        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false;
    }
    DWORD dwSize = 0;
    do {
        DWORD dwDownloaded = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &dwSize)) break;
        if (dwSize == 0) break;
        std::vector<char> buf(dwSize);
        if (!WinHttpReadData(hRequest, buf.data(), dwSize, &dwDownloaded)) break;
        ofs.write(buf.data(), dwDownloaded);
    } while (dwSize > 0);
    ofs.close();
    WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
    return true;
}

// aria2 进度信息（从 stdout 行解析）
struct Aria2Progress {
    int percent = 0;        // 0-100
    std::string speed;      // "5.2MiB/s"
    std::string eta;        // "12m39s" or "??"
    long long downloadedBytes = 0;
    long long totalBytes = 0;
    int connections = 0;    // CN: 当前活动连接数（各线程）
};

// 从 aria2 stdout 行解析进度。格式：[#abc 1.2GiB/4.4GiB(27%) CN:16 DL:5.2MiB ETA:12m39s]
static bool ParseAria2Line(const std::string& line, Aria2Progress& out) {
    // 找百分比的括号
    auto parenStart = line.find('(');
    auto parenEnd = line.find(')', parenStart);
    if (parenStart == std::string::npos || parenEnd == std::string::npos) return false;
    std::string pctStr = line.substr(parenStart + 1, parenEnd - parenStart - 1);
    // 去掉 %
    if (!pctStr.empty() && pctStr.back() == '%') pctStr.pop_back();
    try { out.percent = std::stoi(pctStr); } catch (...) { return false; }
    if (out.percent < 0) out.percent = 0;
    if (out.percent > 100) out.percent = 100;
    // 解析 CN: 连接数
    auto cnPos = line.find("CN:");
    if (cnPos != std::string::npos) {
        auto end = line.find(' ', cnPos + 3);
        std::string cnStr = line.substr(cnPos + 3, end == std::string::npos ? std::string::npos : end - cnPos - 3);
        try { out.connections = std::stoi(cnStr); } catch (...) { out.connections = 0; }
    }
    // 解析 DL: 速度
    auto dlPos = line.find("DL:");
    if (dlPos != std::string::npos) {
        auto end = line.find(' ', dlPos + 3);
        out.speed = line.substr(dlPos + 3, end == std::string::npos ? std::string::npos : end - dlPos - 3);
    }
    // 解析 ETA
    auto etaPos = line.find("ETA:");
    if (etaPos != std::string::npos) {
        auto end = line.find(']', etaPos + 4);
        out.eta = line.substr(etaPos + 4, end == std::string::npos ? std::string::npos : end - etaPos - 4);
    }
    return true;
}

// 用 aria2c 下载文件，实时回调进度。progressCb 返回 false 可取消。
// 使用管道捕获 stdout，解析 --summary-interval=1 的进度行。
static bool Aria2DownloadWithProgress(
    const std::wstring& url,
    const std::wstring& outDir,
    const std::wstring& outName,
    std::function<bool(const Aria2Progress&)> progressCb = nullptr,
    double maxRateMBps = 0)
{
    if (g_exeDir.empty()) return false;
    std::wstring exe = g_exeDir + L"\\aria2c.exe";
    // --summary-interval=1 每秒输出一行进度到 stdout；--max-download-limit 实现整体限速（§5.4 令牌桶近似）。
    // --check-certificate=false：aria2(schannel) 默认做 CRL 吊销检查，国内吊销服务器经常不可达，
    // 导致 TLS 握手直接失败（CRYPT_E_REVOCATION_OFFLINE 80092013）→ 所有 HTTPS 下载失败。跳过证书验证以恢复下载。
    std::wstring cmd = L"\"" + exe + L"\" -x 16 -s 16 --console-log-level=error --summary-interval=1 --check-certificate=false";
    if (maxRateMBps > 0) {
        long long kb = (long long)(maxRateMBps * 1024.0 + 0.5);
        if (kb < 1) kb = 1;
        cmd += L" --max-download-limit=" + std::to_wstring(kb) + L"K";
    }
    cmd += L" -d \"" + outDir + L"\" -o \"" + outName + L"\" \"" + url + L"\"";

    SECURITY_ATTRIBUTES sa{}; sa.nLength = sizeof(sa); sa.bInheritHandle = TRUE;
    HANDLE hReadPipe = nullptr, hWritePipe = nullptr;
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) return false;
    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;
    PROCESS_INFORMATION pi{};
    std::wstring mutableCmd = cmd;
    BOOL ok = CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(hWritePipe);
    if (!ok) { CloseHandle(hReadPipe); return false; }

    // 读取 stdout
    std::string accumulated;
    char buf[4096];
    DWORD bytesRead = 0;
    while (ReadFile(hReadPipe, buf, sizeof(buf), &bytesRead, nullptr) && bytesRead > 0) {
        accumulated.append(buf, bytesRead);
        // 按行处理
        size_t pos = 0;
        while (true) {
            auto nl = accumulated.find('\n', pos);
            if (nl == std::string::npos) break;
            std::string line = accumulated.substr(pos, nl - pos);
            pos = nl + 1;
            // 去掉 \r
            if (!line.empty() && line.back() == '\r') line.pop_back();
            Aria2Progress p;
            if (ParseAria2Line(line, p)) {
                if (progressCb && !progressCb(p)) {
                    // 取消：终止进程
                    TerminateProcess(pi.hProcess, 1);
                    break;
                }
            }
        }
        accumulated.erase(0, pos);
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    CloseHandle(hReadPipe);

    // 完成时回调 100%
    if (exitCode == 0 && progressCb) {
        Aria2Progress done; done.percent = 100;
        progressCb(done);
    }
    return exitCode == 0;
}

// 下载核心（Raw 版）：直接使用传入 URL，不做任何源切换/镜像重写。
// progressCb 仅 aria2 模式有效。
// maxRateMBps：>0 表示本会话限速值；<0 表示取全局设置（settings.json 的 rateLimitMBps10）；==0 不限速。
static bool DownloadFileSmartRaw(
    const std::wstring& url,
    const std::wstring& outDir,
    const std::wstring& outName,
    std::function<bool(const Aria2Progress&)> progressCb = nullptr,
    double maxRateMBps = -1)
{
    // 确保目录存在
    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);
    if (maxRateMBps < 0) maxRateMBps = (double)SettingInt("rateLimitMBps10", 0) / 10.0;
    if (IsAria2Available()) {
        return Aria2DownloadWithProgress(url, outDir, outName, progressCb, maxRateMBps);
    }
    // 回退：WinHTTP（无进度回调）
    std::wstring fullPath = outDir;
    if (!fullPath.empty() && fullPath.back() != L'\\' && fullPath.back() != L'/') fullPath += L'\\';
    fullPath += outName;
    return HttpDownloadFile(url, fullPath);
}

// 下载入口（带源切换）：§5.1 文件下载源开关（官方/镜像/自动）决定实际 URL。
static bool DownloadFileSmart(
    const std::wstring& url,
    const std::wstring& outDir,
    const std::wstring& outName,
    std::function<bool(const Aria2Progress&)> progressCb = nullptr,
    double maxRateMBps = -1)
{
    std::wstring finalUrl = lxe::Utf8ToWide(FileUrlForDownload(lxe::WideToUtf8(url)));
    return DownloadFileSmartRaw(finalUrl, outDir, outName, progressCb, maxRateMBps);
}

// 整合包文件下载：优先直连整合包声明的原始 URL，失败后按源开关（镜像）重试。
// 切换前清理半成品与 .aria2 控制文件，避免不同 URL 续传同一文件导致内容错乱。
static bool DownloadFileSmartPreferOriginal(
    const std::wstring& url,
    const std::wstring& outDir,
    const std::wstring& outName,
    std::function<bool(const Aria2Progress&)> progressCb = nullptr,
    double maxRateMBps = -1)
{
    // 瞬时网络错误重试：同一 URL 最多尝试 3 次（CDN 限流/抖动/握手失败时常见，避免一次失败即判文件失败）
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (DownloadFileSmartRaw(url, outDir, outName, progressCb, maxRateMBps)) return true;
        std::error_code rce;
        std::wstring fullPath = outDir;
        if (!fullPath.empty() && fullPath.back() != L'\\' && fullPath.back() != L'/') fullPath += L'\\';
        fullPath += outName;
        std::filesystem::remove(fullPath, rce);
        std::filesystem::remove(fullPath + L".aria2", rce);
    }
    // 镜像回退：FileUrlForDownload 在 mirror 源/官方不可用时可能返回不同 URL（如 libraries.minecraft.net → BMCLAPI）
    std::wstring mirrorUrl = lxe::Utf8ToWide(FileUrlForDownload(lxe::WideToUtf8(url)));
    if (mirrorUrl == url) return false;
    std::error_code rce;
    std::wstring fullPath = outDir;
    if (!fullPath.empty() && fullPath.back() != L'\\' && fullPath.back() != L'/') fullPath += L'\\';
    fullPath += outName;
    std::filesystem::remove(fullPath, rce);
    std::filesystem::remove(fullPath + L".aria2", rce);
    return DownloadFileSmartRaw(mirrorUrl, outDir, outName, progressCb, maxRateMBps);
}

// 安装器内容校验：zip/jar 头两个字节为 'PK'。BMCLAPI 等镜像在不可用/限流时可能返回
// HTML 错误页（同样落盘成功），启动阶段再动主类才会发现"安装器无效"，故下载后立即校验。
static bool FileLooksLikeZip(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return false;
    unsigned char h[2] = { 0 };
    f.read(reinterpret_cast<char*>(h), 2);
    return h[0] == 'P' && h[1] == 'K';
}

namespace {

// 前向声明：ScanInstalledJavasNow 的实现（ScanInstalledJavas 包装）位于本命名空间
// 更靠后的位置，但 AsyncScanJava 等在 2413 起就会引用，故在此预声明。
static std::vector<InstalledJava> ScanInstalledJavasNow();
// java.download 安装完成后需即时并入"已找到的 Java"列表（定义见 2393/2612 附近）
static InstalledJava ProbeJavaExe(const std::wstring& exe);
static void InsertJavaFoundAndNotify(const InstalledJava& j);
static bool PeIs64(const std::wstring& exe, bool& is64);

// 本地版本列表缓存：按 mcRoot 缓存扫描结果，仅在版本新增/删除/目录切换时失效，
// 避免每次打开下载中心/切换文件夹都重新读盘解析全部 version.json。
static std::mutex g_localVersionsCacheMutex;
static std::wstring g_localVersionsCacheRoot;
static Json g_localVersionsCacheResult;
static bool g_localVersionsCacheDirty = true;
static void InvalidateLocalVersionsCache() {
    std::lock_guard<std::mutex> lock(g_localVersionsCacheMutex);
    g_localVersionsCacheDirty = true;
}

using HandlerResult = Bridge::HandlerResult;
using Handler = Bridge::Handler;

HandlerResult Ok(const Json& result) {
    HandlerResult r;
    r.ok = true;
    r.result = result;
    return r;
}

HandlerResult Err(int code, const std::string& msg) {
    HandlerResult r;
    r.ok = false;
    r.errCode = code;
    r.errMsg = msg;
    return r;
}

// 读取 exe 同级路径 <name> 的全路径
std::wstring ExeSiblingPath(const wchar_t* name) {
    wchar_t buf[MAX_PATH]{};
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L"";
    std::filesystem::path p(buf);
    return (p.parent_path() / name).wstring();
}

// 简易 JSON 解析：只支持扁平对象 { "key": "value", ... } 和 数字/布尔/字符串。
// 这是为了读取 appsettings.json 里的 version 等基础字段而实现的，
// 不依赖外部 JSON 库。如果解析失败返回空 Json()。
Json ParseFlatJson(const std::string& text) {
    Json obj = Json::object();
    size_t i = 0;
    auto skip = [&]() {
        while (i < text.size()) {
            char c = text[i];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') ++i;
            else break;
        }
    };
    auto expect = [&](char c) -> bool {
        skip();
        if (i < text.size() && text[i] == c) { ++i; return true; }
        return false;
    };
    auto parseString = [&]() -> std::string {
        std::string s;
        if (!expect('"')) return {};
        while (i < text.size()) {
            char c = text[i++];
            if (c == '"') return s;
            if (c == '\\' && i < text.size()) {
                char e = text[i++];
                switch (e) {
                    case '"': s.push_back('"'); break;
                    case '\\': s.push_back('\\'); break;
                    case '/': s.push_back('/'); break;
                    case 'b': s.push_back('\b'); break;
                    case 'f': s.push_back('\f'); break;
                    case 'n': s.push_back('\n'); break;
                    case 'r': s.push_back('\r'); break;
                    case 't': s.push_back('\t'); break;
                    case 'u': {
                        if (i + 4 > text.size()) return s;
                        int v = 0;
                        for (int k = 0; k < 4; ++k) {
                            char h = text[i + k];
                            v <<= 4;
                            if (h >= '0' && h <= '9') v |= h - '0';
                            else if (h >= 'a' && h <= 'f') v |= h - 'a' + 10;
                            else if (h >= 'A' && h <= 'F') v |= h - 'A' + 10;
                        }
                        i += 4;
                        // 仅处理 U+0000 - U+FFFF，UTF-8 编码
                        if (v < 0x80) {
                            s.push_back(static_cast<char>(v));
                        } else if (v < 0x800) {
                            s.push_back(static_cast<char>(0xC0 | (v >> 6)));
                            s.push_back(static_cast<char>(0x80 | (v & 0x3F)));
                        } else {
                            s.push_back(static_cast<char>(0xE0 | (v >> 12)));
                            s.push_back(static_cast<char>(0x80 | ((v >> 6) & 0x3F)));
                            s.push_back(static_cast<char>(0x80 | (v & 0x3F)));
                        }
                        break;
                    }
                    default: s.push_back(e);
                }
            } else {
                s.push_back(c);
            }
        }
        return s;
    };
    auto parseValue = [&]() -> bool {
        skip();
        if (i >= text.size()) return false;
        char c = text[i];
        if (c == '"') {
            std::string s = parseString();
            // 最近记住的 key 由外部 push；这里需要上下文，改用外层直接处理
            // 返回 bool 表示成功，字符串结果用下面的 lambda 捕获变量
            return true;
        }
        return false;
    };

    // 解析对象版本
    skip();
    if (!expect('{')) return Json();
    for (int iter = 0; iter < 256; ++iter) {
        skip();
        if (expect('}')) return obj;
        if (iter > 0 && !expect(',')) return Json();
        skip();
        std::string key = parseString();
        if (key.empty()) return Json();
        skip();
        if (!expect(':')) return Json();
        skip();
        if (i >= text.size()) return Json();
        char c = text[i];
        if (c == '"') {
            std::string v = parseString();
            obj[key] = v;
        } else if (c == 't' || c == 'f') {
            bool v = (c == 't');
            std::string kw = (c == 't') ? "true" : "false";
            if (i + kw.size() <= text.size() && text.substr(i, kw.size()) == kw) {
                i += kw.size();
                obj[key] = v;
            } else return Json();
        } else if (c == '-' || (c >= '0' && c <= '9')) {
            size_t j = i;
            if (c == '-') ++j;
            while (j < text.size() && (text[j] >= '0' && text[j] <= '9')) ++j;
            if (j < text.size() && text[j] == '.') {
                ++j;
                while (j < text.size() && (text[j] >= '0' && text[j] <= '9')) ++j;
            }
            std::string num = text.substr(i, j - i);
            i = j;
            try {
                if (num.find('.') == std::string::npos) {
                    obj[key] = static_cast<int64_t>(std::stoll(num));
                } else {
                    obj[key] = std::stod(num);
                }
            } catch (...) {
                return Json();
            }
        } else {
            return Json();
        }
    }
    return Json();
}

// 读取 appsettings.json，返回配置对象。
std::pair<std::string, bool> ReadFileUtf8(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return { "", false };
    std::stringstream ss; ss << f.rdbuf();
    std::string s = ss.str();
    // 去掉 UTF-8 BOM
    if (s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) {
        s = s.substr(3);
    }
    return { s, true };
}

// MultiMC（§2.4）占位符替换复制：递归复制 sourceDir → targetDir，文本类文件执行 $INST_* / ${INST_*} 占位符替换。
// placeholders 为空时退化为普通递归复制。仅对含 '$' 且非二进制的文件做替换，其余直接复制保留原始字节。
static void CopyDirWithPlaceholders(const std::wstring& sourceDir, const std::wstring& targetDir,
                                    const std::map<std::wstring, std::wstring>& placeholders,
                                    std::error_code& ecOut) {
    std::error_code ec;
    if (!std::filesystem::is_directory(sourceDir, ec)) { ecOut = ec; return; }
    std::filesystem::recursive_directory_iterator it(
        sourceDir, std::filesystem::directory_options::skip_permission_denied, ec);
    if (ec) { ecOut = ec; return; }
    std::filesystem::recursive_directory_iterator end;
    static const wchar_t* kBinExts[] = {
        L".jar", L".zip", L".7z", L".gz", L".lz4", L".bin",
        L".png", L".jpg", L".jpeg", L".gif", L".webp", L".ico", L".bmp",
        L".ogg", L".mp3", L".wav", L".flac", L".mp4", L".webm",
        L".ttf", L".otf", L".dll", L".exe", L".pak", L".class",
        L".nbt", L".mca", L".mcr", L".mcc", L".schematic", L".dat",
    };
    for (; it != end; it.increment(ec)) {
        if (ec) break;
        const auto& de = *it;
        std::error_code e2;
        std::wstring rel = de.path().wstring().substr(sourceDir.size());
        while (!rel.empty() && (rel.front() == L'\\' || rel.front() == L'/')) rel.erase(rel.begin());
        std::wstring dst = targetDir + L"\\" + rel;
        if (de.is_directory(e2)) { std::filesystem::create_directories(dst, e2); continue; }
        if (!de.is_regular_file(e2)) continue;
        std::error_code e3;
        std::filesystem::create_directories(std::filesystem::path(dst).parent_path(), e3);
        std::wstring ext = de.path().extension().wstring();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
        bool isBin = false;
        for (const wchar_t* e : kBinExts) if (ext == e) { isBin = true; break; }
        if (isBin || placeholders.empty()) {
            std::filesystem::copy_file(de.path(), dst, std::filesystem::copy_options::overwrite_existing, e3);
            continue;
        }
        auto [text, ok] = ReadFileUtf8(de.path().wstring());
        if (!ok || text.find('$') == std::string::npos) {
            // 非文本或未含占位符：直接复制保留原始字节
            std::filesystem::copy_file(de.path(), dst, std::filesystem::copy_options::overwrite_existing, e3);
            continue;
        }
        std::wstring wtext = lxe::Utf8ToWide(text);
        for (const auto& kv : placeholders) {
            std::wstring braced = L"${" + kv.first + L"}";
            std::wstring plain = L"$" + kv.first;
            size_t pos = 0;
            while ((pos = wtext.find(braced, pos)) != std::wstring::npos) {
                wtext.replace(pos, braced.size(), kv.second); pos += kv.second.size();
            }
            pos = 0;
            while ((pos = wtext.find(plain, pos)) != std::wstring::npos) {
                wtext.replace(pos, plain.size(), kv.second); pos += kv.second.size();
            }
        }
        std::string out = lxe::WideToUtf8(wtext);
        std::ofstream ofs(dst, std::ios::binary);
        if (ofs.is_open()) { ofs.write(out.data(), (std::streamsize)out.size()); ofs.close(); }
    }
    ecOut = ec;
}

Json LoadAppSettings() {
    std::wstring path = ExeSiblingPath(L"appsettings.json");
    auto [s, ok] = ReadFileUtf8(path);
    if (!ok || s.empty()) return Json::object();
    try {
        return ParseFlatJson(s);
    } catch (...) {
        return Json::object();
    }
}

void RegisterPing(Bridge& bridge) {
    bridge.Register("app.ping", [](const Json&) {
        Json result = Json::object();
        result["app"] = "LXElauncher";
        Json cfg = LoadAppSettings();
        if (cfg.isObject() && cfg.contains("version")) {
            result["version"] = cfg.at("version").asString();
        } else {
            result["version"] = "0.1.0";
        }
        result["bridge"] = "jsonrpc";
        return Ok(result);
    });
}

void RegisterAppConfig(Bridge& bridge) {
    bridge.Register("app.config", [](const Json&) {
        Json cfg = LoadAppSettings();
        if (!cfg.isObject()) cfg = Json::object();
        // 兜底：没有配置文件时的默认值
        if (!cfg.contains("version")) cfg["version"] = "0.1.0";
        if (!cfg.contains("title")) cfg["title"] = "LXElauncher";
        return Ok(cfg);
    });
}


void RegisterSystemInfo(Bridge& bridge) {
    bridge.Register("system.info", [](const Json&) {
        Json result = Json::object();

        SYSTEM_INFO si{};
        GetNativeSystemInfo(&si);
        std::string arch;
        switch (si.wProcessorArchitecture) {
            case PROCESSOR_ARCHITECTURE_AMD64: arch = "x64"; break;
            case PROCESSOR_ARCHITECTURE_INTEL: arch = "x86"; break;
            case PROCESSOR_ARCHITECTURE_ARM64: arch = "arm64"; break;
            default: arch = "unknown"; break;
        }
        result["arch"] = arch;

#if defined(_DEBUG)
        result["build"] = "debug";
#else
        result["build"] = "release";
#endif

        MEMORYSTATUSEX mem{};
        mem.dwLength = sizeof(mem);
        if (GlobalMemoryStatusEx(&mem)) {
            result["totalMemMB"] = static_cast<int64_t>(mem.ullTotalPhys / (1024 * 1024));
            result["availMemMB"] = static_cast<int64_t>(mem.ullAvailPhys / (1024 * 1024));
        }

        std::wstring exePath;
        {
            wchar_t buf[MAX_PATH]{};
            DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
            if (n > 0 && n < MAX_PATH) exePath.assign(buf, n);
        }
        if (!exePath.empty()) {
            ULARGE_INTEGER total{}, freeBytes{};
            if (GetDiskFreeSpaceExW(exePath.c_str(), &freeBytes, &total, nullptr)) {
                result["freeDiskMB"] = static_cast<int64_t>(freeBytes.QuadPart / (1024 * 1024));
            }
        }
        return Ok(result);
    });
}

void RegisterSubscribe(Bridge& bridge) {
    bridge.Register("event.subscribe", [](const Json& params) {
        if (!params.isArray() && !(params.isObject() && params.contains("events") && params.at("events").isArray())) {
            return Err(-32602, "'events' array required");
        }
        Json result = Json::object();
        result["ok"] = true;
        result["count"] = static_cast<int>(params.size());
        return Ok(result);
    });
}

void RegisterDownloadSimulate(Bridge& bridge) {
    bridge.Register("download.simulate", [&bridge](const Json&) {
        static std::atomic<int> taskSeq{0};
        int taskId = ++taskSeq;

        Json result = Json::object();
        result["taskId"] = std::to_string(taskId);
        result["started"] = true;
        result["note"] = "simulated task running in backend";

        std::thread([&bridge, taskId]() {
            try {
            for (int pct = 0; pct <= 100; pct += 10) {
                if (pct == 0) {
                    Json ev = Json::object();
                    ev["taskId"] = std::to_string(taskId);
                    ev["state"] = "started";
                    bridge.PostEvent("download.state", ev);
                }
                Json prog = Json::object();
                prog["taskId"] = std::to_string(taskId);
                prog["percent"] = pct;
                prog["speed"] = "12.5 MB/s";
                bridge.PostEvent("download.progress", prog);
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
            }
            Json done = Json::object();
            done["taskId"] = std::to_string(taskId);
            done["state"] = "done";
            bridge.PostEvent("download.state", done);
            } catch (...) {}
        }).detach();

        return Ok(result);
    });
}

// ============ 构建元信息 ============
void RegisterBuildMeta(Bridge& bridge) {
    bridge.Register("app.buildInfo", [](const Json&) {
        Json result = Json::object();
        auto& m = GetBuildMeta();
        result["buildNumber"] = m.buildNumber;
        result["buildTime"] = m.buildTime;
        return Ok(result);
    });
}

// ============ 后端日志导出 ============
void RegisterLogExport(Bridge& bridge) {
    bridge.Register("app.exportLogs", [&bridge](const Json& params) {
        // 允许前端把自己的日志也一起合并上传
        std::string frontLogText;
        if (params.isObject() && params.contains("frontendLogs") && params.at("frontendLogs").isString()) {
            frontLogText = params.at("frontendLogs").asString();
        }
        // 生成日志文件到 exe 同级 logs\LXElauncher-<timestamp>.log
        if (g_exeDir.empty()) return Err(-32000, "没有 exe 目录");
        std::wstring logDir = g_exeDir + L"\\logs";
        std::error_code ec;
        std::filesystem::create_directories(logDir, ec);

        auto tp = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(tp);
        std::tm tm{}; localtime_s(&tm, &t);
        std::ostringstream fss;
        fss << "LXElauncher-" << std::put_time(&tm, "%Y%m%d-%H%M%S") << ".log";
        std::wstring logPath = logDir + L"\\" + lxe::Utf8ToWide(fss.str());
        std::ofstream ofs(logPath, std::ios::binary);
        if (!ofs.is_open()) return Err(-32001, "无法写入日志文件");

        // BOM 防乱码
        const char bom[] = "\xEF\xBB\xBF"; ofs.write(bom, 3);

        auto fmtNow = []() -> std::string {
            auto tp = std::chrono::system_clock::now();
            std::time_t t = std::chrono::system_clock::to_time_t(tp);
            std::tm tm{}; localtime_s(&tm, &t);
            std::ostringstream ss;
            ss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
            return ss.str();
        };
        auto& m = GetBuildMeta();
        auto snap = bridge.SnapshotActivityLog();

        ofs << "===== LXElauncher 日志导出 =====\r\n";
        ofs << "导出时间: " << fmtNow() << "\r\n";
        ofs << "构建号: " << m.buildNumber << "   构建时间: " << m.buildTime << "\r\n";
        ofs << "aria2 可用: " << (IsAria2Available() ? "是" : "否") << "\r\n";
        ofs << "Exe 目录: " << lxe::WideToUtf8(g_exeDir) << "\r\n\r\n";

        ofs << "===== [1/3] 后端活动日志（bridge.activity）共 " << snap.size() << " 条 =====\r\n";
        for (const auto& e : snap) {
            ofs << '[' << e.time << "] " << e.method << ' ' << (e.ok ? "OK" : "ERR");
            if (!e.error.empty()) ofs << ": " << e.error;
            ofs << "\r\n";
        }
        ofs << "\r\n===== [2/3] 下载任务状态摘要 =====\r\n";
        ofs << "(详细下载进度请查看前端日志部分)\r\n";

        ofs << "\r\n===== [3/3] 前端日志（若提供）=====\r\n";
        if (!frontLogText.empty()) {
            ofs.write(frontLogText.data(), frontLogText.size());
            if (!frontLogText.empty() && frontLogText.back() != '\n') ofs << "\r\n";
        } else {
            ofs << "(未提供前端日志)\r\n";
        }
        ofs.close();

        // 用 ShellExecute 打开 log 所在目录（让用户直接看到文件）
        ShellExecuteW(nullptr, L"open", logDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);

        Json r = Json::object();
        r["success"] = true;
        r["path"] = lxe::WideToUtf8(logPath);
        r["dir"] = lxe::WideToUtf8(logDir);
        r["activityCount"] = (int)snap.size();
        return Ok(r);
    });
}

// 目标盘剩余可用字节数（path 不存在时也按所在卷根计算）；失败返回 -1
static long long DiskFreeBytesOf(const std::wstring& path) {
    std::wstring root = path;
    if (root.size() >= 2 && root[1] == L':') root = root.substr(0, 3); // "C:\"
    ULARGE_INTEGER total{}, freeBytes{};
    if (GetDiskFreeSpaceExW(root.c_str(), &freeBytes, &total, nullptr)) {
        return static_cast<long long>(freeBytes.QuadPart);
    }
    return -1;
}

// ============ 前端解析文件列表 → 下发下载 ============
// 前端负责解析 version JSON，整理出 [{url, outDir, outName, sha1, size, label}]
// 提交给后端。后端返回 taskId，随后通过 download.progress / download.state 事件报告进度。
void RegisterSubmitDownloadList(Bridge& bridge) {
    bridge.Register("mc.submitDownloadList", [&bridge](const Json& params) {
        if (!params.isObject()) return Err(-32602, "params must be object");
        std::string taskName = params.contains("taskName") && params.at("taskName").isString()
            ? params.at("taskName").asString() : "下载任务";
        if (!params.contains("items") || !params.at("items").isArray())
            return Err(-32602, "missing items[] array");
        const auto& items = params.at("items").asArray();
        if (items.empty()) return Err(-32602, "items[] 为空");

        int maxConcurrent = 8;
        if (params.contains("maxConcurrent") && params.at("maxConcurrent").isNumber()) {
            maxConcurrent = (int)params.at("maxConcurrent").asNumber();
            if (maxConcurrent < 1) maxConcurrent = 1;
            if (maxConcurrent > 16) maxConcurrent = 16;
        } else {
            // §5.1 下载线程数（默认 64；超过 100 前端提示警告）
            maxConcurrent = SettingInt("downloadThreads", 64);
            if (maxConcurrent < 1) maxConcurrent = 1;
            if (maxConcurrent > 999) maxConcurrent = 999;
        }

        static std::atomic<int> listTaskSeq{20000};
        int taskId = ++listTaskSeq;
        auto cancelFlag = DLCancelFlag(std::to_string(taskId));
        Json result = Json::object();
        result["taskId"] = std::to_string(taskId);
        result["started"] = true;
        result["itemCount"] = (int)items.size();
        result["maxConcurrent"] = maxConcurrent;

        struct DLItem { std::string url; std::wstring outDir; std::wstring outName; std::string label; long long size = 0; std::string sha1; };
        auto dlItems = std::make_shared<std::vector<DLItem>>();
        dlItems->reserve(items.size());
        for (const auto& it : items) {
            if (!it.isObject()) continue;
            DLItem d;
            if (it.contains("url") && it.at("url").isString()) d.url = it.at("url").asString();
            if (it.contains("outDir") && it.at("outDir").isString()) d.outDir = lxe::Utf8ToWide(it.at("outDir").asString());
            if (it.contains("outName") && it.at("outName").isString()) d.outName = lxe::Utf8ToWide(it.at("outName").asString());
            if (it.contains("label") && it.at("label").isString()) d.label = it.at("label").asString();
            if (it.contains("size") && it.at("size").isNumber()) d.size = (long long)it.at("size").asNumber();
            if (it.contains("sha1") && it.at("sha1").isString()) d.sha1 = it.at("sha1").asString();
            if (d.outName.empty()) continue;
            std::wstring absPath;
            if (d.outDir.empty()) absPath = d.outName;
            else absPath = d.outDir + L"\\" + d.outName;
            if (std::filesystem::exists(absPath)) {
                if (d.size > 0) {
                    std::error_code ec;
                    auto fsize = std::filesystem::file_size(absPath, ec);
                    if (!ec && fsize == (uintmax_t)d.size) continue;
                } else {
                    continue;
                }
            }
            if (d.url.empty()) continue;
            dlItems->push_back(std::move(d));
        }
        int skipped = (int)items.size() - (int)dlItems->size();
        int totalFiles = (int)dlItems->size();

        std::thread([&bridge, taskId, taskName, dlItems, skipped, totalFiles, maxConcurrent, cancelFlag]() {
            struct ThreadInfo { std::string label; int pct = 0; std::string speed; std::string eta; int connections = 0; };
            auto progressMutex = std::make_shared<std::mutex>();
            // 每文件状态（pending/downloading/done/error），供前端着色展示
            auto filesState = std::make_shared<std::vector<Json>>(totalFiles);
            {
                std::lock_guard<std::mutex> lock(*progressMutex);
                for (int i = 0; i < totalFiles; ++i) {
                    Json fs = Json::object();
                    fs["name"] = dlItems->at(i).label.empty() ? lxe::WideToUtf8(dlItems->at(i).outName) : dlItems->at(i).label;
                    fs["state"] = "pending";
                    (*filesState)[i] = fs;
                }
            }
            // 磁盘空间不足标记：任一文件检查发现目标盘剩余空间不够时置位，
            // 前端据此在下载进度区显示"磁盘空间不足"提示
            auto diskLow = std::make_shared<std::atomic<bool>>(false);
            auto diskFreeMB = std::make_shared<std::atomic<long long>>(0);
            auto postProgress = [&](int pct, const std::string& stage, const std::string& speed, const std::string& eta, const std::string& size, const std::vector<ThreadInfo>* threads = nullptr, int doneFiles = -1) {
                Json prog = Json::object();
                prog["taskId"] = std::to_string(taskId);
                prog["percent"] = pct;
                prog["stage"] = stage;
                prog["name"] = taskName;
                prog["speed"] = speed;
                prog["eta"] = eta;
                prog["size"] = size;
                prog["totalFiles"] = totalFiles;
                prog["doneFiles"] = doneFiles < 0 ? 0 : doneFiles;
                prog["remainingFiles"] = totalFiles - (doneFiles < 0 ? 0 : doneFiles);
                prog["diskLow"] = diskLow->load();
                prog["diskFreeMB"] = diskLow->load() ? diskFreeMB->load() : 0;
                // 每文件状态 + 线程状态快照（同一把锁内拷贝，避免与 aria2 回调的写并发）
                {
                    std::lock_guard<std::mutex> lock(*progressMutex);
                    Json fArr = Json::array();
                    for (const auto& fs : *filesState) fArr.asArray().push_back(fs);
                    prog["files"] = fArr;
                    if (threads) {
                        Json tArr = Json::array();
                        for (const auto& t : *threads) {
                            Json to = Json::object();
                            to["label"] = t.label;
                            to["pct"] = t.pct;
                            to["speed"] = t.speed;
                            to["eta"] = t.eta;
                            to["connections"] = t.connections;
                            tArr.asArray().push_back(to);
                        }
                        prog["threads"] = tArr;
                    }
                }
                bridge.PostEvent("download.progress", prog);
            };
            auto postState = [&](const std::string& state) {
                Json ev = Json::object();
                ev["taskId"] = std::to_string(taskId);
                ev["state"] = state;
                ev["name"] = taskName;
                bridge.PostEvent("download.state", ev);
            };
            postState("started");
            auto fmtSize = [](long long bytes) -> std::string {
                if (bytes <= 0) return "";
                std::ostringstream ss;
                if (bytes < 1024 * 1024) ss << std::fixed << std::setprecision(0) << (bytes / 1024.0) << " KB";
                else ss << std::fixed << std::setprecision(1) << (bytes / (1024.0 * 1024.0)) << " MB";
                return ss.str();
            };
            try {
            if (dlItems->empty()) {
                std::ostringstream ss;
                ss << "全部就绪（跳过 " << skipped << " 项已存在文件）";
                postProgress(100, ss.str(), "", "", "");
                DLCancelFlagRemove(std::to_string(taskId));
                postState("done");
                return;
            }

            // 预检磁盘空间：剩余空间不足以容纳全部待下载文件时提前标记（前端显示"磁盘空间不足"）
            {
                long long totalNeed = 0;
                for (const auto& d : *dlItems) totalNeed += d.size;
                const std::wstring firstDir = dlItems->at(0).outDir.empty() ? GetMcRoot() : dlItems->at(0).outDir;
                long long freeBytes = DiskFreeBytesOf(firstDir);
                if (totalNeed > 0 && freeBytes >= 0 && freeBytes < totalNeed + 256LL * 1024 * 1024) {
                    diskLow->store(true);
                    diskFreeMB->store(freeBytes / (1024 * 1024));
                }
            }

            auto completedCount = std::make_shared<std::atomic<int>>(0);
            auto errorFlag = std::make_shared<std::atomic<bool>>(false);
            auto threadInfo = std::make_shared<std::vector<ThreadInfo>>(maxConcurrent);
            auto worker = [&, completedCount, errorFlag, progressMutex, threadInfo, filesState, diskLow, diskFreeMB, cancelFlag](int idx, int workerSlot) {
                try {
                const auto& it = dlItems->at(idx);
                std::string label = it.label.empty() ? (it.outName.empty() ? std::string("下载中") : lxe::WideToUtf8(it.outName)) : it.label;
                std::string stage = "(" + std::to_string(idx + 1) + "/" + std::to_string(totalFiles) + ") " + label;
                {
                    std::lock_guard<std::mutex> lock(*progressMutex);
                    (*threadInfo)[workerSlot].label = label;
                    (*threadInfo)[workerSlot].pct = 0;
                    (*threadInfo)[workerSlot].speed.clear();
                    (*threadInfo)[workerSlot].eta.clear();
                    (*threadInfo)[workerSlot].connections = 0;
                    if ((*filesState)[idx].contains("state")) (*filesState)[idx]["state"] = "downloading";
                }

                auto cb = [&](const Aria2Progress& p) -> bool {
                    if (errorFlag->load()) return false;
                    if (cancelFlag->load()) return false;
                    int overallPct = 0;
                    {
                        std::lock_guard<std::mutex> lock(*progressMutex);
                        (*threadInfo)[workerSlot].pct = p.percent;
                        (*threadInfo)[workerSlot].speed = p.speed;
                        (*threadInfo)[workerSlot].eta = p.eta;
                        (*threadInfo)[workerSlot].connections = p.connections;
                        int done = completedCount->load();
                        overallPct = (int)(100.0 * (done + p.percent / 100.0) / totalFiles);
                        if (overallPct < 0) overallPct = 0;
                        if (overallPct > 100) overallPct = 100;
                    }
                    std::ostringstream ss; ss << p.speed;
                    // 回调内不持锁调用 postProgress（否则与内部快照锁自锁死锁）
                    postProgress(overallPct, stage, ss.str(), p.eta, fmtSize(it.size), threadInfo.get(), completedCount->load());
                    return true;
                };

                // 磁盘空间检查：目标盘剩余空间不足以容纳本文件时标记，前端下载进度区提示
                {
                    const std::wstring dlDir = it.outDir.empty() ? GetMcRoot() : it.outDir;
                    long long freeBytes = DiskFreeBytesOf(dlDir);
                    long long needBytes = it.size + 128LL * 1024 * 1024; // 128MB 缓冲，避免小文件边界抖动
                    if (freeBytes >= 0 && freeBytes < needBytes) {
                        diskLow->store(true);
                        diskFreeMB->store(freeBytes / (1024 * 1024));
                    }
                }

                bool ok = DownloadFileSmart(lxe::Utf8ToWide(it.url), it.outDir, it.outName, cb);
                if (ok) {
                    int newDone = ++(*completedCount);
                    int overallPct = (int)(100.0 * newDone / totalFiles);
                    if (overallPct > 100) overallPct = 100;
                    {
                        std::lock_guard<std::mutex> lock(*progressMutex);
                        (*threadInfo)[workerSlot].pct = 100;
                        (*threadInfo)[workerSlot].label.clear();
                        if ((*filesState)[idx].contains("state")) (*filesState)[idx]["state"] = "done";
                    }
                    postProgress(overallPct, stage + " ✓", "", "", fmtSize(it.size), threadInfo.get(), newDone);
                } else {
                    errorFlag->store(true);
                    {
                        std::lock_guard<std::mutex> lock(*progressMutex);
                        (*threadInfo)[workerSlot].pct = -1;
                        if ((*filesState)[idx].contains("state")) (*filesState)[idx]["state"] = cancelFlag->load() ? "cancelled" : "error";
                    }
                    postProgress((int)(100.0 * completedCount->load() / totalFiles), stage + (cancelFlag->load() ? " ✕" : " ✗"), "", "", fmtSize(it.size), threadInfo.get(), completedCount->load());
                }
                } catch (const std::exception& e) {
                    errorFlag->store(true);
                    {
                        std::lock_guard<std::mutex> lock(*progressMutex);
                        (*threadInfo)[workerSlot].pct = -1;
                        if ((*filesState)[idx].contains("state")) (*filesState)[idx]["state"] = cancelFlag->load() ? "cancelled" : "error";
                    }
                    postProgress((int)(100.0 * completedCount->load() / totalFiles), "下载异常：" + std::string(e.what()), "", "", "", threadInfo.get(), completedCount->load());
                } catch (...) {
                    errorFlag->store(true);
                    {
                        std::lock_guard<std::mutex> lock(*progressMutex);
                        (*threadInfo)[workerSlot].pct = -1;
                        if ((*filesState)[idx].contains("state")) (*filesState)[idx]["state"] = cancelFlag->load() ? "cancelled" : "error";
                    }
                    postProgress((int)(100.0 * completedCount->load() / totalFiles), "下载异常", "", "", "", threadInfo.get(), completedCount->load());
                }
            };

            auto nextIndex = std::make_shared<std::atomic<int>>(0);

            auto threadFunc = [worker, nextIndex, totalFiles, errorFlag, threadInfo, progressMutex, cancelFlag](int slot) {
                while (!errorFlag->load() && !cancelFlag->load()) {
                    int idx = nextIndex->fetch_add(1);
                    if (idx >= totalFiles) break;
                    worker(idx, slot);
                }
                // 该线程全部结束后清空标签
                std::lock_guard<std::mutex> lock(*progressMutex);
                (*threadInfo)[slot].label.clear();
            };

            std::vector<std::thread> workers;
            workers.reserve(maxConcurrent);
            for (int i = 0; i < maxConcurrent; ++i) {
                workers.emplace_back(threadFunc, i);
            }

            for (auto& t : workers) {
                if (t.joinable()) t.join();
            }

            if (errorFlag->load() || cancelFlag->load()) {
                // 原子回退：删除所有未完成文件（含 .aria2 控制文件），避免残留半成品
                {
                    std::lock_guard<std::mutex> lock(*progressMutex);
                    for (int i = 0; i < totalFiles; ++i) {
                        std::string st = (*filesState)[i].contains("state") && (*filesState)[i].at("state").isString()
                            ? (*filesState)[i].at("state").asString() : "pending";
                        if (st == "done") continue;
                        const auto& it = dlItems->at(i);
                        std::wstring absPath = it.outDir.empty() ? it.outName : (it.outDir + L"\\" + it.outName);
                        std::error_code ec;
                        std::filesystem::remove(absPath, ec);
                        std::filesystem::remove(absPath + L".aria2", ec);
                    }
                }
                DLCancelFlagRemove(std::to_string(taskId));
                if (cancelFlag->load()) {
                    postState("cancelled");
                } else {
                    postState("error");
                }
                return;
            }

            postProgress(100, "完成" + (skipped > 0 ? std::string(" · 跳过 ") + std::to_string(skipped) + " 项已存在" : ""), "", "", "");
            postState("done");
            DLCancelFlagRemove(std::to_string(taskId));
            InvalidateLocalVersionsCache();
            } catch (const std::exception& e) {
                DLCancelFlagRemove(std::to_string(taskId));
                postState("error");
                postProgress(0, "下载异常：" + std::string(e.what()), "", "", "");
            } catch (...) {
                DLCancelFlagRemove(std::to_string(taskId));
                postState("error");
                postProgress(0, "下载未知异常", "", "", "");
            }
        }).detach();

        return Ok(result);
    });

    // ============ 下载任务取消（download.cancel）：置位任务取消标志 → 各 worker 的 aria2 进程被终止 ============
    bridge.Register("download.cancel", [](const Json& params) {
        std::string taskId;
        if (params.isObject() && params.contains("taskId") && params.at("taskId").isString())
            taskId = params.at("taskId").asString();
        if (taskId.empty()) return Err(-32602, "missing taskId");
        DLCancelFlagSet(taskId);
        Json r = Json::object();
        r["ok"] = true;
        return Ok(r);
    });
}

// ============ aria2 工具状态 ============
void RegisterAria2(Bridge& bridge) {
    bridge.Register("aria2.available", [](const Json&) {
        Json r = Json::object();
        r["available"] = IsAria2Available();
        r["path"] = lxe::WideToUtf8(g_exeDir + L"\\aria2c.exe");
        return Ok(r);
    });
}

// ============ 游戏版本清单读取 ============
Json ParseVersionsFromManifest(const std::string& text) {
    // 直接解析完整 JSON 而不是扁平对象
    Json root;
    try {
        root = Json::parse(text);
    } catch (...) {
        root = Json::object();
    }
    Json out = Json::object();
    Json latest = Json::object();
    if (root.isObject() && root.contains("latest") && root.at("latest").isObject()) {
        const auto& l = root.at("latest");
        if (l.contains("release") && l.at("release").isString()) latest["release"] = l.at("release").asString();
        if (l.contains("snapshot") && l.at("snapshot").isString()) latest["snapshot"] = l.at("snapshot").asString();
    }
    out["latest"] = latest;
    Json arr = Json::array();
    if (root.isObject() && root.contains("versions") && root.at("versions").isArray()) {
        for (const auto& v : root.at("versions").asArray()) {
            if (!v.isObject()) continue;
            // 默认只显示 release（官方正式版）；可通过传 includeSnapshots:true 全显
            Json o = Json::object();
            auto getStr = [&](const char* k) -> std::string {
                if (v.contains(k) && v.at(k).isString()) return v.at(k).asString();
                return "";
            };
            o["id"] = getStr("id");
            o["type"] = getStr("type");
            o["url"] = getStr("url");
            o["releaseTime"] = getStr("releaseTime");
            o["time"] = getStr("time");
            arr.asArray().push_back(o);
        }
    }
    out["versions"] = arr;
    return out;
}

struct ExitedInstance { DWORD pid; DWORD exitCode; };

static void GameMonitorThread() {
    while (g_monitorRunning.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        if (!g_bridgeForMonitor) continue;
        try {
        std::vector<ExitedInstance> exitedInstances;
        {
            std::lock_guard<std::mutex> lock(g_gameInstancesMutex);
            for (auto it = g_gameInstances.begin(); it != g_gameInstances.end(); ) {
                HANDLE hProcess = it->first;
                DWORD pid = it->second;
                if (WaitForSingleObject(hProcess, 0) == WAIT_OBJECT_0) {
                    DWORD exitCode = 0;
                    GetExitCodeProcess(hProcess, &exitCode);
                    exitedInstances.push_back({pid, exitCode});
                    CloseHandle(hProcess);
                    it = g_gameInstances.erase(it);
                } else {
                    ++it;
                }
            }
        }

        for (const auto& inst : exitedInstances) {
            DWORD pid = inst.pid;
            DWORD exitCode = inst.exitCode;
            if (exitCode == 0) {
                Json ev = Json::object();
                ev["pid"] = (int)pid;
                ev["reason"] = "normal";
                {
                    std::lock_guard<std::mutex> lock(g_lastLaunchMu);
                    if (g_lastLaunchJavaMajor.load() > 0) ev["javaMajor"] = g_lastLaunchJavaMajor.load();
                    if (!g_lastLaunchGameDir.empty()) ev["mcRoot"] = lxe::WideToUtf8(g_lastLaunchGameDir);
                }
                g_bridgeForMonitor->PostEvent("mc.stopped", ev);
            } else {
                Json ev = Json::object();
                ev["pid"] = (int)pid;
                ev["exitCode"] = (int)exitCode;
                ev["reason"] = "crashed";
                {
                    std::lock_guard<std::mutex> lock(g_lastLaunchMu);
                    if (g_lastLaunchJavaMajor.load() > 0) ev["javaMajor"] = g_lastLaunchJavaMajor.load();
                    if (!g_lastLaunchGameDir.empty()) ev["mcRoot"] = lxe::WideToUtf8(g_lastLaunchGameDir);
                }
                g_bridgeForMonitor->PostEvent("mc.crashed", ev);
            }
        }
        } catch (...) {}
    }
}

// 启动监控线程
static void StartGameMonitor(Bridge& bridge) {
    if (g_monitorRunning.exchange(true)) return;
    g_bridgeForMonitor = &bridge;
    g_monitorThread = std::thread(GameMonitorThread);
}

void RegisterMinecraftVersions(Bridge& bridge) {
    bridge.Register("mc.versions", [](const Json& params) {
        std::string typeFilter = "release";
        bool includeSnapshots = false;
        if (params.isObject()) {
            if (params.contains("typeFilter") && params.at("typeFilter").isString()) {
                typeFilter = params.at("typeFilter").asString();
            }
            if (params.contains("includeSnapshots") && params.at("includeSnapshots").isBool()) {
                includeSnapshots = params.at("includeSnapshots").asBool();
            }
        }
        if (includeSnapshots) {
            typeFilter = "all";
        }

        Json result = Json::object();
        result["versions"] = Json::array();
        result["latest"] = Json::object();

        if (g_manifestPath.empty()) return Err(-32001, "manifest path not configured");
        auto [text, ok] = ReadFileUtf8(g_manifestPath);
        if (!ok) return Err(-32002, "manifest file not found");
        auto parsed = ParseVersionsFromManifest(text);
        if (parsed.contains("latest")) result["latest"] = parsed.at("latest");

        Json filtered = Json::array();
        if (parsed.contains("versions") && parsed.at("versions").isArray()) {
            for (const auto& v : parsed.at("versions").asArray()) {
                if (!v.isObject()) continue;
                const auto& type = v.contains("type") && v.at("type").isString() ? v.at("type").asString() : "";
                if (typeFilter != "all" && type != typeFilter) continue;
                filtered.asArray().push_back(v);
            }
        }
        result["versions"] = filtered;
        return Ok(result);
    });
}

// ============ 原版安装（模拟进度 + 可扩展为真实下载） ============
// 检查库的 OS 规则是否允许在 Windows 上加载
// 规则结构: [{"action":"allow","os":{"name":"windows"}}, {"action":"disallow","os":{"name":"osx"}}]
// 无 rules → 通用库，允许
static bool IsLibAllowedOnWindows(const Json& lib) {
    if (!lib.contains("rules") || !lib.at("rules").isArray() || lib.at("rules").asArray().empty()) {
        return true; // 无规则 = 通用
    }
    bool allowed = false;
    for (const auto& rule : lib.at("rules").asArray()) {
        if (!rule.isObject()) continue;
        std::string action = rule.contains("action") && rule.at("action").isString() ? rule.at("action").asString() : "";
        // 检查规则是否适用于当前 OS
        bool applies = true;
        if (rule.contains("os") && rule.at("os").isObject()) {
            const auto& os = rule.at("os");
            std::string osName = os.contains("name") && os.at("name").isString() ? os.at("name").asString() : "";
            applies = (osName == "windows" || osName.empty());
        }
        if (applies) {
            if (action == "allow") allowed = true;
            else if (action == "disallow") allowed = false;
        }
    }
    return allowed;
}

// 从本地 manifest 查找指定版本的元数据 URL
static std::string FindVersionUrl(const std::string& versionId) {
    if (g_manifestPath.empty()) return {};
    auto [text, ok] = ReadFileUtf8(g_manifestPath);
    if (!ok) return {};
    try {
        Json root = Json::parse(text);
        if (root.isObject() && root.contains("versions") && root.at("versions").isArray()) {
            for (const auto& v : root.at("versions").asArray()) {
                if (v.isObject() && v.contains("id") && v.at("id").isString() &&
                    v.at("id").asString() == versionId) {
                    if (v.contains("url") && v.at("url").isString()) {
                        return v.at("url").asString();
                    }
                }
            }
        }
    } catch (...) {}
    return {};
}

void RegisterMinecraftInstall(Bridge& bridge) {
    bridge.Register("mc.installVanilla", [&bridge](const Json& params) {
        std::string version;
        if (params.isObject() && params.contains("version") && params.at("version").isString())
            version = params.at("version").asString();
        if (version.empty()) return Err(-32602, "missing version");

        static std::atomic<int> taskSeq{10000};
        int taskId = ++taskSeq;
        Json result = Json::object();
        result["taskId"] = std::to_string(taskId);
        result["started"] = true;
        result["version"] = version;

        std::thread([&bridge, taskId, version]() {
            auto postProgress = [&](int pct, const std::string& stage, const std::string& speed = "", const std::string& eta = "", const std::string& size = "") {
                Json prog = Json::object();
                prog["taskId"] = std::to_string(taskId);
                prog["percent"] = pct;
                prog["stage"] = stage;
                prog["name"] = "原版 " + version;
                prog["speed"] = speed;
                prog["eta"] = eta;
                prog["size"] = size;
                bridge.PostEvent("download.progress", prog);
            };
            auto postState = [&](const std::string& state) {
                Json ev = Json::object();
                ev["taskId"] = std::to_string(taskId);
                ev["state"] = state;
                ev["name"] = "原版 " + version;
                bridge.PostEvent("download.state", ev);
            };

            try {
            postState("started");

            // MC 根目录：.minecraft/
            std::wstring mcRoot = GetMcRoot();
            std::wstring verDir = mcRoot + L"\\versions\\" + lxe::Utf8ToWide(version);
            std::wstring libDir = mcRoot + L"\\libraries";

            // ===== 阶段 1：查找版本 URL 并下载版本 JSON =====
            postProgress(2, "获取版本信息");
            std::string versionUrl = FindVersionUrl(version);
            if (versionUrl.empty()) {
                postProgress(0, "错误：未找到版本 URL");
                postState("error");
                return;
            }

            std::string versionJsonText = HttpFetchText(lxe::Utf8ToWide(versionUrl));
            if (versionJsonText.empty()) {
                postProgress(0, "错误：版本 JSON 下载失败");
                postState("error");
                return;
            }

            Json versionJson;
            try {
                versionJson = Json::parse(versionJsonText);
            } catch (...) {
                postProgress(0, "错误：版本 JSON 解析失败");
                postState("error");
                return;
            }

            // 保存版本 JSON 到 versions/<id>/<id>.json
            {
                std::wstring jsonPath = verDir + L"\\" + lxe::Utf8ToWide(version) + L".json";
                std::error_code ec;
                std::filesystem::create_directories(verDir, ec);
                std::ofstream ofs(jsonPath, std::ios::binary);
                if (ofs.is_open()) { ofs.write(versionJsonText.data(), versionJsonText.size()); ofs.close(); }
            }
            postProgress(5, "版本信息已获取");

            // ===== 阶段 2：下载客户端 JAR（5% - 50%） =====
            std::string clientJarUrl;
            long long clientJarSize = 0;
            if (versionJson.contains("downloads") && versionJson.at("downloads").isObject() &&
                versionJson.at("downloads").contains("client") &&
                versionJson.at("downloads").at("client").isObject()) {
                const auto& client = versionJson.at("downloads").at("client");
                if (client.contains("url") && client.at("url").isString())
                    clientJarUrl = client.at("url").asString();
                if (client.contains("size") && client.at("size").isNumber())
                    clientJarSize = (long long)client.at("size").asNumber();
            }

            if (clientJarUrl.empty()) {
                postProgress(0, "错误：未找到客户端 JAR URL");
                postState("error");
                return;
            }

            {
                std::wstring jarName = lxe::Utf8ToWide(version) + L".jar";
                // 格式化文件大小
                auto fmtSize = [](long long bytes) -> std::string {
                    if (bytes <= 0) return "";
                    std::ostringstream ss;
                    if (bytes < 1024 * 1024) ss << std::fixed << std::setprecision(0) << (bytes / 1024.0) << " KB";
                    else ss << std::fixed << std::setprecision(1) << (bytes / (1024.0 * 1024.0)) << " MB";
                    return ss.str();
                };
                std::string sizeStr = fmtSize(clientJarSize);
                // aria2 进度映射到 5%-50%
                auto cb = [&](const Aria2Progress& p) -> bool {
                    int mapped = 5 + p.percent * 45 / 100;
                    std::ostringstream ss;
                    ss << p.speed;
                    postProgress(mapped, "下载客户端 JAR", ss.str(), p.eta, sizeStr);
                    return true;
                };
                bool ok = DownloadFileSmart(lxe::Utf8ToWide(clientJarUrl), verDir, jarName, cb);
                if (!ok) {
                    // 回退：删除半成品 jar
                    std::error_code ec;
                    std::filesystem::remove(verDir + L"\\" + jarName, ec);
                    std::filesystem::remove(verDir + L"\\" + jarName + L".aria2", ec);
                    postProgress(0, "错误：客户端 JAR 下载失败");
                    postState("error");
                    return;
                }
            }
            postProgress(50, "客户端 JAR 下载完成");

            // ===== 阶段 3：下载 Libraries（50% - 90%） =====
            struct LibItem {
                std::string path;  // 相对路径如 "com/mojang/authlib/10.0.76/authlib-10.0.76.jar"
                std::string url;
                long long size;
            };
            std::vector<LibItem> libs;
            if (versionJson.contains("libraries") && versionJson.at("libraries").isArray()) {
                for (const auto& lib : versionJson.at("libraries").asArray()) {
                    if (!lib.isObject()) continue;
                    if (!IsLibAllowedOnWindows(lib)) continue;
                    if (lib.contains("downloads") && lib.at("downloads").isObject() &&
                        lib.at("downloads").contains("artifact") &&
                        lib.at("downloads").at("artifact").isObject()) {
                        const auto& art = lib.at("downloads").at("artifact");
                        LibItem item;
                        if (art.contains("path") && art.at("path").isString())
                            item.path = art.at("path").asString();
                        if (art.contains("url") && art.at("url").isString())
                            item.url = art.at("url").asString();
                        if (art.contains("size") && art.at("size").isNumber())
                            item.size = (long long)art.at("size").asNumber();
                        if (!item.path.empty() && !item.url.empty())
                            libs.push_back(item);
                    }
                }
            }

            bool anyLibError = false;
            for (size_t i = 0; i < libs.size(); ++i) {
                const auto& lib = libs[i];
                if (anyLibError) break;
                // 计算每库在 50%-90% 范围内的进度区间
                int libStart = 50 + (int)(40.0 * i / libs.size());
                int libEnd = 50 + (int)(40.0 * (i + 1) / libs.size());

                // 库的输出目录：libDir + path 的父目录
                std::filesystem::path libPath(lxe::Utf8ToWide(lib.path));
                std::wstring outDir = libDir + L"\\" + libPath.parent_path().wstring();
                std::wstring outName = libPath.filename().wstring();

                auto cb = [&](const Aria2Progress& p) -> bool {
                    int mapped = libStart + p.percent * (libEnd - libStart) / 100;
                    std::ostringstream ss;
                    ss << p.speed;
                    postProgress(mapped, "下载依赖库 " + lib.path, ss.str(), p.eta);
                    return true;
                };
                bool dlOk = DownloadFileSmart(lxe::Utf8ToWide(lib.url), outDir, outName, cb);
                if (!dlOk) {
                    // 回退：删除半成品库文件
                    std::error_code ec;
                    std::filesystem::remove(outDir + L"\\" + outName, ec);
                    std::filesystem::remove(outDir + L"\\" + outName + L".aria2", ec);
                    anyLibError = true;
                    break;
                }
                postProgress(libEnd, "下载依赖库 " + lib.path);
            }
            if (anyLibError) {
                postState("error");
                return;
            }
            postProgress(90, "依赖库下载完成");

            // ===== 阶段 4：写入版本元数据完成（90% - 100%） =====
            postProgress(95, "写入版本元数据");
            // 版本 JSON 已在阶段 1 保存，此处仅更新进度
            postProgress(100, "安装完成");
            postState("done");
            InvalidateLocalVersionsCache();
            } catch (const std::exception& e) {
                postState("error");
                postProgress(0, "错误：安装异常 " + std::string(e.what()));
            } catch (...) {
                postState("error");
                postProgress(0, "错误：安装未知异常");
            }
        }).detach();

        return Ok(result);
    });
}

// ============ 本地版本扫描 ============
// 扫描 .minecraft/versions/<id>/<id>.json，返回已安装版本列表
static std::string DetectVersionType(const Json& vj, const std::string& id);
void RegisterMinecraftLocalVersions(Bridge& bridge) {
    // 异步执行：扫描 + 解析所有版本 JSON 放到后台线程，避免阻塞 WebView2 桥消息线程导致界面卡顿
    bridge.RegisterAsync("mc.localVersions", [](const Json& params, const std::function<void(HandlerResult)>& done) {
        bool force = params.isObject() && params.contains("force") && params.at("force").isBool() && params.at("force").asBool();
        std::thread([done, force]() {
            std::wstring mcRoot;
            Json result = Json::object();
            Json arr = Json::array();
            try {
            mcRoot = GetMcRoot();
            {
                std::lock_guard<std::mutex> lock(g_localVersionsCacheMutex);
                if (!force && !g_localVersionsCacheDirty && g_localVersionsCacheRoot == mcRoot) {
                    done(Ok(g_localVersionsCacheResult));
                    return;
                }
            }
            std::wstring versionsDir = mcRoot + L"\\versions";

            if (std::filesystem::exists(versionsDir)) {
                std::error_code ec;
                for (const auto& entry : std::filesystem::directory_iterator(versionsDir, ec)) {
                    if (!entry.is_directory()) continue;
                    std::wstring dirName = entry.path().filename().wstring();
                    std::wstring jsonPath = entry.path().wstring() + L"\\" + dirName + L".json";
                    if (!std::filesystem::exists(jsonPath)) continue;

                    auto [text, ok] = ReadFileUtf8(jsonPath);
                    if (!ok || text.empty()) continue;

                    Json vj;
                    try { vj = Json::parse(text); } catch (...) { continue; }
                    if (!vj.isObject()) continue;

                    std::string id = lxe::WideToUtf8(dirName);
                    Json o = Json::object();
                    o["id"] = id;
                    std::string mainClass;
                    if (vj.contains("mainClass") && vj.at("mainClass").isString())
                        mainClass = vj.at("mainClass").asString();
                    o["mainClass"] = mainClass;
                    // 优化后的版本类型判断（forge/fabric/quilt/neoforge/optifine/vanilla）
                    o["type"] = DetectVersionType(vj, id);
                    // 记录基础 MC 版本（继承父版本时取 inheritsFrom；否则自身 id 即版本）
                    if (vj.contains("inheritsFrom") && vj.at("inheritsFrom").isString())
                        o["inheritsFrom"] = vj.at("inheritsFrom").asString();
                    if (vj.contains("id") && vj.at("id").isString())
                        o["mcVersion"] = vj.at("id").asString();
                    if (vj.contains("releaseTime") && vj.at("releaseTime").isString())
                        o["releaseTime"] = vj.at("releaseTime").asString();

                    // 检查 jar 是否存在
                    std::wstring jarPath = entry.path().wstring() + L"\\" + dirName + L".jar";
                    o["jarExists"] = std::filesystem::exists(jarPath);

                    arr.asArray().push_back(o);
                }
            }

            result["versions"] = arr;
            result["mcRoot"] = lxe::WideToUtf8(mcRoot);
            {
                std::lock_guard<std::mutex> lock(g_localVersionsCacheMutex);
                g_localVersionsCacheRoot = mcRoot;
                g_localVersionsCacheResult = result;
                g_localVersionsCacheDirty = false;
            }
            done(Ok(result));
            } catch (...) {
                done(Ok(result));
            }
        }).detach();
    });
}

// ============ 导入 .minecraft 文件夹 ============
// 弹出文件夹选择对话框，将选中路径设为 MC 根目录
void RegisterMinecraftImportFolder(Bridge& bridge) {
    bridge.Register("mc.importFolder", [](const Json& params) {
        // 支持直接传 path 参数（前端可预设），否则弹出文件夹选择对话框
        std::wstring folder;
        if (params.isObject() && params.contains("path") && params.at("path").isString()) {
            folder = lxe::Utf8ToWide(params.at("path").asString());
        }
        if (folder.empty()) {
            // 使用 IFileDialog (Vista+) 选择文件夹
            IFileDialog* pfd = nullptr;
            if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                            IID_PPV_ARGS(&pfd)))) {
                DWORD dwOptions = 0;
                if (SUCCEEDED(pfd->GetOptions(&dwOptions))) {
                    pfd->SetOptions(dwOptions | FOS_PICKFOLDERS);
                }
                pfd->SetTitle(L"选择 .minecraft 文件夹");
                if (SUCCEEDED(pfd->Show(nullptr))) {
                    IShellItem* psi = nullptr;
                    if (SUCCEEDED(pfd->GetResult(&psi))) {
                        PWSTR pszPath = nullptr;
                        if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &pszPath))) {
                            folder = pszPath;
                            CoTaskMemFree(pszPath);
                        }
                        psi->Release();
                    }
                }
                pfd->Release();
            }
        }
        if (folder.empty()) return Err(-32001, "未选择文件夹");

        // 验证文件夹存在
        if (!std::filesystem::is_directory(folder)) return Err(-32002, "路径不是有效文件夹");

        // 验证是否包含 versions 子目录（宽松检查：有 versions 或至少有 libraries）
        bool hasVersions = std::filesystem::exists(folder + L"\\versions");
        bool hasLibraries = std::filesystem::exists(folder + L"\\libraries");
        if (!hasVersions && !hasLibraries) return Err(-32003, "该文件夹不包含 versions 或 libraries，可能不是 .minecraft 目录");

        // 设置为 MC 根目录
        g_mcRoot = folder;
        InvalidateLocalVersionsCache();

        Json result = Json::object();
        result["mcRoot"] = lxe::WideToUtf8(folder);
        result["hasVersions"] = hasVersions;
        result["hasLibraries"] = hasLibraries;
        return Ok(result);
    });

    // 获取/设置 MC 根目录
    bridge.Register("mc.getMcRoot", [](const Json&) {
        Json result = Json::object();
        result["mcRoot"] = lxe::WideToUtf8(GetMcRoot());
        return Ok(result);
    });

    // 文件选择对话框（用于选择 javaw.exe 等可执行文件）
    bridge.Register("dialog.openFile", [](const Json& params) {
        std::wstring title = L"选择文件";
        if (params.isObject() && params.contains("title") && params.at("title").isString())
            title = lxe::Utf8ToWide(params.at("title").asString());
        std::wstring filterName = L"可执行文件";
        std::wstring filterSpec = L"*.exe";
        if (params.isObject() && params.contains("filter") && params.at("filter").isString()) {
            filterSpec = L"*" + lxe::Utf8ToWide(params.at("filter").asString());
        }

        std::wstring resultPath;
        IFileDialog* pfd = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                        IID_PPV_ARGS(&pfd)))) {
            pfd->SetTitle(title.c_str());
            COMDLG_FILTERSPEC filters[] = { { filterName.c_str(), filterSpec.c_str() }, { L"所有文件", L"*.*" } };
            pfd->SetFileTypes(2, filters);
            if (SUCCEEDED(pfd->Show(nullptr))) {
                IShellItem* psi = nullptr;
                if (SUCCEEDED(pfd->GetResult(&psi))) {
                    PWSTR pszPath = nullptr;
                    if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &pszPath))) {
                        resultPath = pszPath;
                        CoTaskMemFree(pszPath);
                    }
                    psi->Release();
                }
            }
            pfd->Release();
        }
        if (resultPath.empty()) return Err(-32004, "未选择文件");
        Json result = Json::object();
        result["path"] = lxe::WideToUtf8(resultPath);
        return Ok(result);
    });

    // ===================== OptiFine 仅保存安装包（temp/OptiFine设计-净室版.md §6） =====================
    // 复用 §3.2 的多源地址获取（BMCLAPI 直链拼址 + 官方中转页兜底），但不执行任何安装步骤；
    // 弹“保存文件”对话框选择落点；最小体积校验放宽（仅需为 ZIP/PK 头，不必满足安装器自检）。
    bridge.Register("mc.optifineSave", [](const Json& params) {
        std::string filename, type, patch, mcVersion;
        if (params.isObject()) {
            if (params.contains("filename") && params.at("filename").isString()) filename = params.at("filename").asString();
            if (params.contains("type") && params.at("type").isString()) type = params.at("type").asString();
            if (params.contains("patch") && params.at("patch").isString()) patch = params.at("patch").asString();
            if (params.contains("mcVersion") && params.at("mcVersion").isString()) mcVersion = params.at("mcVersion").asString();
        }
        if (filename.empty() || type.empty() || patch.empty() || mcVersion.empty())
            return Err(-32001, "缺少 OptiFine 版本信息");

        std::wstring savePath;
        {
            IFileSaveDialog* pfd = nullptr;
            if (SUCCEEDED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
                                            IID_PPV_ARGS(&pfd)))) {
                pfd->SetTitle(L"保存 OptiFine 安装包");
                COMDLG_FILTERSPEC filters[] = { { L"Jar 文件", L"*.jar" }, { L"所有文件", L"*.*" } };
                pfd->SetFileTypes(2, filters);
                std::wstring defName = lxe::Utf8ToWide(filename);
                if (!defName.empty()) pfd->SetFileName(defName.c_str());
                if (SUCCEEDED(pfd->Show(nullptr))) {
                    IShellItem* psi = nullptr;
                    if (SUCCEEDED(pfd->GetResult(&psi))) {
                        PWSTR psz = nullptr;
                        if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &psz))) {
                            savePath = psz;
                            CoTaskMemFree(psz);
                        }
                        psi->Release();
                    }
                }
                pfd->Release();
            }
        }
        if (savePath.empty()) return Err(-32004, "未选择保存位置");

        std::wstring outDir = std::filesystem::path(savePath).parent_path().wstring();
        std::wstring outName = std::filesystem::path(savePath).filename().wstring();
        std::wstring rel = lxe::Utf8ToWide(BmclOptifineVersionPath(mcVersion) + "/" + type + "/" + patch);
        std::vector<std::wstring> candidates = {
            L"https://bmclapi2.bangbang93.com/optifine/" + rel,
            L"https://bmclapi.bangbang93.com/optifine/" + rel,
            L"http://bmclapi2.bangbang93.com/optifine/" + rel,
        };
        auto cleanTmp = [&]() {
            std::error_code ec;
            std::filesystem::remove(savePath, ec);
            std::filesystem::remove(savePath + L".aria2", ec);
        };
        bool ok = false;
        for (size_t i = 0; i < candidates.size() && !ok; ++i) {
            cleanTmp();
            if (!DownloadFileSmart(candidates[i], outDir, outName, nullptr)) continue;
            if (!FileLooksLikeZip(savePath)) { cleanTmp(); continue; }
            ok = true;
        }
        if (!ok) {
            // 官方源无稳定直链：先请求中转页抽 x token，再取 downloadx 直链（§3.2）
            std::string adUrl = "https://optifine.net/adloadx?f=" + filename;
            std::string html = HttpFetchTextWithHeader(lxe::Utf8ToWide(adUrl),
                L"Referer: https://optifine.net/downloads\r\nUser-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64)\r\n");
            std::regex tokenRe(R"(downloadx\?f=[^'\"&]*&x=([0-9a-fA-F]{32}))");
            std::smatch m;
            if (!html.empty() && std::regex_search(html, m, tokenRe)) {
                std::string dlUrl = "https://optifine.net/downloadx?f=" + filename + "&x=" + m[1].str();
                cleanTmp();
                if (DownloadFileSmart(lxe::Utf8ToWide(dlUrl), outDir, outName, nullptr) && FileLooksLikeZip(savePath))
                    ok = true;
            }
        }
        if (!ok) { cleanTmp(); return Err(-32005, "OptiFine 安装包下载失败（官方源与 BMCLAPI 镜像均不可用）"); }
        Json r = Json::object();
        r["path"] = lxe::WideToUtf8(savePath);
        r["size"] = (double)std::filesystem::file_size(savePath);
        return Ok(r);
    });

    // 下载 Java 运行时（从 Adoptium 下载 JRE ZIP 并解压到 runtime/java-<version>/）
    // ===================== mc.javaList：Java 运行时可选源列表（异步） =====================
    // source: 'official'（内置 adoptium 直链）| 'bmclapi'（BMCLAPI /java/list）| 'yours'（自定义源，base 必填）
    // 返回 {list:[{name,version,url,desc}]}，url 即下载直链，传给 java.download 即可
    bridge.RegisterAsync("mc.javaList", [](const Json& params, const std::function<void(HandlerResult)>& done) {
        std::string source = "official", base;
        if (params.isObject() && params.contains("source") && params.at("source").isString())
            source = params.at("source").asString();
        if (params.isObject() && params.contains("base") && params.at("base").isString())
            base = params.at("base").asString();
        if (source.empty()) source = "official";

        std::thread([source, base, done]() {
            try {
            Json list = Json::array();
            if (source == "bmclapi" || source == "yours") {
                std::string listUrl = "https://bmclapi2.bangbang93.com/java/list";
                if (source == "yours" && !base.empty()) {
                    std::string b = base;
                    if (b.size() > 1 && b.back() == '/') b.pop_back();
                    listUrl = b + "/java/list";
                }
                std::string text = HttpFetchText(lxe::Utf8ToWide(listUrl));
                if (!text.empty()) {
                    try {
                        Json data = Json::parse(text);
                        if (data.isArray()) {
                            for (const auto& it : data.asArray()) {
                                if (!it.isObject()) continue;
                                std::string title, file;
                                if (it.contains("title") && it.at("title").isString()) title = it.at("title").asString();
                                if (it.contains("file") && it.at("file").isString()) file = it.at("file").asString();
                                if (file.empty()) continue;
                                Json o = Json::object();
                                o["name"] = title.empty() ? file : title;
                                o["version"] = title.empty() ? file : title;
                                o["desc"] = std::string("来自 ") + (source == "yours" ? "自定义源" : "BMCLAPI 源");
                                o["url"] = (source == "yours" && !base.empty()
                                    ? (base + (base.back() == '/' ? "" : "/") + "java/" + file)
                                    : ("https://bmclapi.bangbang93.com/java/" + file));
                                list.asArray().push_back(o);
                            }
                        }
                    } catch (...) {}
                }
            } else {
                // 官方源：Adoptium 最新 GA 直链
                auto mk = [&](const std::string& name, const std::string& ver, const std::string& desc, const std::string& major) {
                    Json o = Json::object();
                    o["name"] = name;
                    o["version"] = ver;
                    o["desc"] = desc;
                    o["url"] = "https://api.adoptium.net/v3/binary/latest/" + major + "/ga/windows/x64/jre/hotspot/normal/eclipse";
                    list.asArray().push_back(o);
                };
                mk("Java 8 (JRE 8u412)", "8", "适用于 Minecraft 1.7 ~ 1.16", "8");
                mk("Java 17 (JRE 17.0.11)", "17", "适用于 Minecraft 1.17 ~ 1.20.4", "17");
                mk("Java 21 (JRE 21.0.3)", "21", "适用于 Minecraft 1.20.5 ~ 1.21.3", "21");
                mk("Java 25 (JRE 25.x)", "25", "适用于 Minecraft 1.21.4+ / 最新版本", "25");
            }
            Json result = Json::object();
            result["list"] = list;
            done(Ok(result));
            } catch (const std::exception& e) {
                done(Err(-32050, std::string("获取 Java 列表异常：") + e.what()));
            } catch (...) {
                done(Err(-32050, "获取 Java 列表未知异常"));
            }
        }).detach();
    });

    // ===================== mc.logList：列出 logs 目录下的日志文件（按修改时间倒序）=====================
    bridge.RegisterAsync("mc.logList", [](const Json& params, const std::function<void(HandlerResult)>& done) {
        std::thread([done]() {
            Json list = Json::array();
            try {
                std::wstring logsDir = GetMcRoot() + L"\\logs";
                std::error_code ec;
                if (std::filesystem::is_directory(logsDir, ec)) {
                    std::vector<std::pair<long long, Json>> acc;
                    for (const auto& de : std::filesystem::directory_iterator(logsDir, ec)) {
                        if (de.is_directory(ec)) continue;
                        std::filesystem::path p = de.path();
                        if (p.extension().wstring() != L".log") continue;
                        std::error_code fsec;
                        auto lastWrite = std::filesystem::last_write_time(p, fsec);
                        long long ms = 0;
                        if (!fsec) ms = std::chrono::duration_cast<std::chrono::milliseconds>(lastWrite.time_since_epoch()).count();
                        std::error_code szec;
                        uintmax_t sz = std::filesystem::file_size(p, szec);
                        Json o = Json::object();
                        o["name"] = lxe::WideToUtf8(p.filename().wstring());
                        o["size"] = (long long)(szec ? 0 : sz);
                        o["mtime"] = ms;
                        acc.push_back({ ms, o });
                    }
                    std::sort(acc.begin(), acc.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
                    for (auto& kv : acc) list.asArray().push_back(kv.second);
                }
            } catch (const std::exception& e) {
                done(Err(-32052, std::string("枚举日志文件异常：") + e.what()));
                return;
            } catch (...) {
                done(Err(-32052, "枚举日志文件未知异常"));
                return;
            }
            Json result = Json::object();
            result["list"] = list;
            done(Ok(result));
        }).detach();
    });

    // ===================== mc.readGameLog：读取游戏日志尾部（异步） =====================
    // 优先级：latest.log → 启动器捕获的 stdout → stderr。最多返回 256KB。可传 file 指定日志文件名
    bridge.RegisterAsync("mc.readGameLog", [](const Json& params, const std::function<void(HandlerResult)>& done) {
        std::thread([done, params]() {
            try {
            auto readTail = [](const std::wstring& p, size_t maxBytes) -> std::string {
                HANDLE h = CreateFileW(p.c_str(), GENERIC_READ,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                       nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (h == INVALID_HANDLE_VALUE) return "";
                std::string out;
                LARGE_INTEGER size;
                if (GetFileSizeEx(h, &size) && size.QuadPart > 0) {
                    LARGE_INTEGER off;
                    off.QuadPart = size.QuadPart > (LONGLONG)maxBytes ? size.QuadPart - (LONGLONG)maxBytes : 0;
                    SetFilePointerEx(h, off, nullptr, FILE_BEGIN);
                    DWORD toRead = (DWORD)(size.QuadPart - off.QuadPart);
                    std::vector<char> buf(toRead + 1, 0);
                    DWORD read = 0;
                    if (ReadFile(h, buf.data(), toRead, &read, nullptr)) out.assign(buf.data(), read);
                }
                CloseHandle(h);
                return out;
            };
            std::wstring base = GetMcRoot() + L"\\logs";
            std::string reqFile;
            if (params.isObject() && params.contains("file") && params.at("file").isString())
                reqFile = params.at("file").asString();
            std::vector<std::pair<std::wstring, std::wstring>> candidates;
            if (!reqFile.empty()) {
                // 指定日志文件：名称做基本安全校验，仅允许纯文件名
                if (reqFile.find('/') == std::string::npos && reqFile.find('\\') == std::string::npos &&
                    reqFile.find("..") == std::string::npos && reqFile.find(':') == std::string::npos)
                    candidates.push_back({ base + L"\\" + lxe::Utf8ToWide(reqFile), lxe::Utf8ToWide(reqFile) });
            }
            if (candidates.empty()) {
                const std::pair<std::wstring, std::wstring> defaults[] = {
                    { base + L"\\latest.log", L"latest.log" },
                    { base + L"\\lxe-launcher-std.log", L"lxe-launcher-std.log" },
                    { base + L"\\lxe-launcher-crash.log", L"lxe-launcher-crash.log" },
                };
                for (const auto& c : defaults) candidates.push_back(c);
            }
            std::string log;
            std::wstring path;
            for (const auto& c : candidates) {
                std::error_code ec;
                if (!std::filesystem::is_regular_file(c.first, ec)) continue;
                log = readTail(c.first, 256 * 1024);
                if (!log.empty()) { path = c.first; break; }
            }
Json result = Json::object();
            result["log"] = log;
            if (!path.empty()) result["path"] = lxe::WideToUtf8(path);
            done(Ok(result));
            } catch (const std::exception& e) {
                done(Err(-32051, std::string("读取游戏日志异常：") + e.what()));
            } catch (...) {
                done(Err(-32051, "读取游戏日志未知异常"));
            }
        }).detach();
    });

    bridge.Register("java.download", [&bridge](const Json& params) {
        std::wstring url, version;
        if (params.isObject() && params.contains("url") && params.at("url").isString())
            url = lxe::Utf8ToWide(params.at("url").asString());
        if (params.isObject() && params.contains("version") && params.at("version").isString())
            version = lxe::Utf8ToWide(params.at("version").asString());
        if (url.empty() || version.empty()) return Err(-32602, "missing url or version");

        // 目标目录：<exe_dir>/runtime/java-<version>/
        std::wstring runtimeDir = g_exeDir + L"\\runtime\\java-" + version;
        std::error_code ec;
        std::filesystem::create_directories(runtimeDir, ec);

        std::wstring taskId = L"java-" + version;
        std::string taskIdUtf8 = lxe::WideToUtf8(taskId);
        std::string versionUtf8 = lxe::WideToUtf8(version);
        auto cancelFlag = DLCancelFlag(taskIdUtf8);
        // 发送 started 事件
        {
            Json ev = Json::object(); ev["taskId"] = taskIdUtf8; ev["state"] = "started"; ev["name"] = "Java " + versionUtf8;
            bridge.PostEvent("download.state", ev);
        }
        // 异步下载 + 解压
        std::thread([url, runtimeDir, versionUtf8, taskIdUtf8, cancelFlag, &bridge]() {
            auto postState = [&](const std::string& state, const std::string& err) {
                Json ev = Json::object(); ev["taskId"] = taskIdUtf8; ev["state"] = state;
                if (!err.empty()) ev["error"] = err;
                bridge.PostEvent("download.state", ev);
            };
            try {
            std::wstring zipPath = runtimeDir + L"\\jre-" + lxe::Utf8ToWide(versionUtf8) + L".zip";
            // 下载（带进度回调，支持取消）
            auto cb = [&](const Aria2Progress& p) -> bool {
                if (cancelFlag->load()) return false;
                Json ev = Json::object();
                ev["taskId"] = taskIdUtf8;
                ev["percent"] = p.percent;
                ev["speed"] = p.speed;
                ev["eta"] = p.eta;
                ev["stage"] = "下载 Java " + versionUtf8 + " 运行时";
                ev["name"] = "Java " + versionUtf8;
                bridge.PostEvent("download.progress", ev);
                return true;
            };
            bool ok = DownloadFileSmart(url, runtimeDir, L"jre-" + lxe::Utf8ToWide(versionUtf8) + L".zip", cb);
            if (!ok) {
                DLCancelFlagRemove(taskIdUtf8);
                if (cancelFlag->load()) postState("cancelled", "");
                else postState("error", "下载失败");
                return;
            }
            if (cancelFlag->load()) {
                DLCancelFlagRemove(taskIdUtf8);
                std::error_code ec;
                std::filesystem::remove(zipPath, ec);
                postState("cancelled", "");
                return;
            }
            // 用 tar.exe 解压（Windows 10 1803+ 内置）—— 静默执行
            std::wstring cmd = L"cmd /c tar -xf \"" + zipPath + L"\" -C \"" + runtimeDir + L"\"";
            if (!RunSilent(cmd)) {
                DLCancelFlagRemove(taskIdUtf8);
                postState("error", "解压失败，请手动解压 JRE ZIP");
                std::error_code ec;
                std::filesystem::remove(zipPath, ec);
                return;
            }
            // 删除 ZIP
            std::error_code ec;
            std::filesystem::remove(zipPath, ec);
            // 查找 javaw.exe
            std::wstring javaPath;
            for (auto& entry : std::filesystem::recursive_directory_iterator(runtimeDir, ec)) {
                if (entry.path().filename() == L"javaw.exe") {
                    javaPath = entry.path().wstring();
                    break;
                }
            }
            // 探测新安装的 Java 并即时并入"已找到的 Java"列表（内存+磁盘缓存+广播 java.found，前端自动追加）
            if (!javaPath.empty()) {
                InstalledJava j = ProbeJavaExe(javaPath);
                // javaw.exe 无控制台输出，管道探测拿不到版本号 → 回退同目录 java.exe 再探测
                if (j.major <= 0) {
                    std::wstring alt = std::filesystem::path(javaPath).parent_path().wstring() + L"\\java.exe";
                    std::error_code aec;
                    if (std::filesystem::exists(alt, aec)) j = ProbeJavaExe(alt);
                }
                // 仍失败：用安装参数携带的版本号兜底（如 "8"/"17"/"21"/"25"）
                if (j.major <= 0) {
                    try { j.major = std::stoi(versionUtf8); } catch (...) {}
                    PeIs64(javaPath, j.is64);
                }
                if (j.major > 0) InsertJavaFoundAndNotify(j);
            }
            // 发送 done 事件 + javaPath
            {
                DLCancelFlagRemove(taskIdUtf8);
                Json ev = Json::object(); ev["taskId"] = taskIdUtf8; ev["state"] = "done"; ev["javaPath"] = lxe::WideToUtf8(javaPath);
                bridge.PostEvent("download.state", ev);
            }
            } catch (const std::exception& e) {
                DLCancelFlagRemove(taskIdUtf8);
                postState("error", std::string("安装异常：") + e.what());
            } catch (...) {
                DLCancelFlagRemove(taskIdUtf8);
                postState("error", "安装未知异常");
            }
        }).detach();

        Json result = Json::object();
        result["taskId"] = lxe::WideToUtf8(taskId);
        result["status"] = "downloading";
        return Ok(result);
    });
}

// ============ Java 查找 ============
// 从 "java -version" / "-XshowSettings:properties -version" 输出解析主版本号
// （1.8 → 8；17.0.x → 17；25.0.1 → 25；无法解析返回 0）。
// 优先解析 -XshowSettings 的属性行（java.specification.version / java.version，值不带引号），
// 再取引号内版本号，最后才兜底找数字序列——避免截断输出里 "UTF-8" / "Win32" 等被误当版本。
static int JavaMajorFromVersionText(const std::wstring& out) {
    std::wstring s = out;
    // 1) -XshowSettings:properties 输出：属性行值不带引号
    //    （"java.specification.version = 1.8"、"java.version = 25.0.1"），此处解析最可靠
    auto versionFromProp = [&s](const wchar_t* key) -> int {
        size_t p = s.find(key);
        if (p == std::wstring::npos) return 0;
        size_t eq = s.find(L'=', p);
        if (eq == std::wstring::npos) return 0;
        size_t a = eq + 1;
        while (a < s.size() && (s[a] == L' ' || s[a] == L'\t')) ++a;
        int first = 0;
        while (a < s.size() && s[a] >= L'0' && s[a] <= L'9') first = first * 10 + (int)(s[a++] - L'0');
        if (first == 0) return 0;
        // 老格式 "1.8(.y...)" → 8
        if (first == 1 && a < s.size() && s[a] == L'.') {
            ++a;
            int second = 0;
            while (a < s.size() && s[a] >= L'0' && s[a] <= L'9') second = second * 10 + (int)(s[a++] - L'0');
            if (second > 0) return second;
        }
        return first;
    };
    int m = versionFromProp(L"java.specification.version");
    if (m > 0) return m;
    m = versionFromProp(L"java.version");
    if (m > 0) return m;
    // 2) 引号内版本号（标准 java -version 输出：version "25.0.1" / "1.8.0_311"）
    auto pos = s.find(L"version");
    if (pos == std::wstring::npos) pos = 0;
    pos = s.find(L'"', pos);
    if (pos != std::wstring::npos) {
        ++pos;
        size_t end = s.find(L'"', pos);
        if (end == std::wstring::npos) end = s.size();
        std::wstring ver = s.substr(pos, end - pos);
        if (!ver.empty()) {
            // 老格式 "1.x(.y...)" → x
            if (ver.size() >= 3 && ver[0] == L'1' && ver[1] == L'.') {
                int maj = 0;
                for (size_t i = 2; i < ver.size(); ++i) {
                    if (ver[i] < L'0' || ver[i] > L'9') break;
                    maj = maj * 10 + (int)(ver[i] - L'0');
                }
                return maj;
            }
            // 新格式 "17.0.11" → 17
            int maj = 0;
            for (size_t i = 0; i < ver.size(); ++i) {
                if (ver[i] < L'0' || ver[i] > L'9') break;
                maj = maj * 10 + (int)(ver[i] - L'0');
            }
            return maj;
        }
    }
    // 3) 兜底：找第一段"数字.数字"（如部分 JRE 输出 "openjdk 17.0.11 ..."）。
    //    要求含小数点，避免把 "UTF-8"、"Win32"、位数描述等当成版本。
    for (size_t i = 0; i + 1 < s.size(); ++i) {
        if (!iswdigit(s[i])) continue;
        size_t j = i;
        while (j < s.size() && (iswdigit(s[j]) || s[j] == L'.')) ++j;
        std::wstring ver = s.substr(i, j - i);
        if (ver.find(L'.') != std::wstring::npos) {
            int maj = 0;
            for (size_t k = 0; k < ver.size(); ++k) {
                if (ver[k] < L'0' || ver[k] > L'9') break;
                maj = maj * 10 + (int)(ver[k] - L'0');
            }
            return maj;
        }
        i = j; // 该数字段无小数点，跳过继续找
    }
    return 0;
}

// 读取 PE 头的 Machine 字段判断可执行文件是否 64 位（0x8664=AMD64），返回 false 表示无法解析/非 PE
static bool PeIs64(const std::wstring& exe, bool& is64) {
    is64 = false;
    std::ifstream f(exe, std::ios::binary | std::ios::ate);
    if (!f.is_open()) return false;
    std::streamoff sz = f.tellg();
    if (sz < 0x40) return false;
    f.seekg(0x3C, std::ios::beg);
    unsigned char b4[4] = {0,0,0,0};
    f.read(reinterpret_cast<char*>(b4), 4);
    unsigned long e_lfanew = b4[0] | (b4[1] << 8) | (b4[2] << 16) | ((unsigned long)b4[3] << 24);
    if (e_lfanew + 6 > (unsigned long)sz) return false;
    f.seekg((std::streamoff)e_lfanew, std::ios::beg);
    unsigned char sig[4] = {0,0,0,0};
    f.read(reinterpret_cast<char*>(sig), 4);
    if (sig[0] != 'P' || sig[1] != 'E' || sig[2] != 0 || sig[3] != 0) return false;
    unsigned char mach[2] = {0,0};
    f.read(reinterpret_cast<char*>(mach), 2);
    unsigned short machine = (unsigned short)(mach[0] | (mach[1] << 8));
    is64 = (machine == 0x8664);
    return true;
}

// 带超时的进程输出捕获：cmd = 将要执行(不含重定向)；argPath 拼在前面；命令超时则杀进程返回空
static std::wstring RunCaptureTimeout(const std::wstring& cmdLine, int timeoutMs) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE hRead = nullptr, hWrite = nullptr;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) return L"";
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = hWrite;
    si.hStdError = hWrite;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cmdLine.begin(), cmdLine.end());
    buf.push_back(L'\0');
    BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(hWrite);
    if (!ok) { CloseHandle(hRead); return L""; }
    std::string data;
    char tmp[4096];
    DWORD rd = 0;
    auto start = std::chrono::steady_clock::now();
    while (true) {
        if (!PeekNamedPipe(hRead, nullptr, 0, nullptr, &rd, nullptr)) break;
        if (rd > 0) {
            DWORD n = 0;
            BOOL rr = ReadFile(hRead, tmp, (rd > sizeof(tmp)) ? (DWORD)sizeof(tmp) : rd, &n, nullptr);
            if (rr && n > 0) data.append(tmp, n);
            else break;
        } else if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(timeoutMs)) {
            TerminateProcess(pi.hProcess, 1);
            break;
        } else {
            if (WaitForSingleObject(pi.hProcess, 200) == WAIT_OBJECT_0) break;
        }
    }
    // 进程已退出：管道中可能还有未读数据（写入与退出存在竞态，直接 break 会丢尾块，
    // 导致 -XshowSettings 属性与 -version 引号块被截断 → 版本解析失败）。排空后再结束。
    for (;;) {
        DWORD rd2 = 0;
        if (!PeekNamedPipe(hRead, nullptr, 0, nullptr, &rd2, nullptr)) break;
        if (rd2 == 0) break;
        DWORD n = 0;
        BOOL rr = ReadFile(hRead, tmp, (rd2 > sizeof(tmp)) ? (DWORD)sizeof(tmp) : rd2, &n, nullptr);
        if (rr && n > 0) data.append(tmp, n);
        else break;
    }
    CloseHandle(hRead);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    int wlen = MultiByteToWideChar(CP_UTF8, 0, data.c_str(), (int)data.size(), nullptr, 0);
    std::wstring res(wlen, L'\0');
    if (wlen > 0) MultiByteToWideChar(CP_UTF8, 0, data.c_str(), (int)data.size(), &res[0], wlen);
    return res;
}

// 探测单个 java 可执行文件：-XshowSettings:properties -version 一次拿到版本号 / 架构 / 编码（§1.2 单进程双取）
static InstalledJava ProbeJavaExe(const std::wstring& exe) {
    InstalledJava r;
    r.path = exe;
    r.is64 = false;
    PeIs64(exe, r.is64);
    std::wstring out = RunCaptureTimeout(L"\"" + exe + L"\" -XshowSettings:properties -version", 15000);
    // javaw.exe 是 GUI 程序，管道拿不到输出 → 回退同目录 java.exe 再探测（§1.2）
    if (out.empty()) {
        std::wstring fn = std::filesystem::path(exe).filename().wstring();
        std::wstring fl = fn;
        std::transform(fl.begin(), fl.end(), fl.begin(), ::towlower);
        if (fl == L"javaw.exe") {
            std::wstring alt = std::filesystem::path(exe).parent_path().wstring() + L"\\java.exe";
            std::error_code aec;
            if (std::filesystem::exists(alt, aec)) {
                std::wstring altOut = RunCaptureTimeout(L"\"" + alt + L"\" -XshowSettings:properties -version", 15000);
                if (!altOut.empty()) out = altOut;
            }
        }
        // 兜底：纯 -version（个别发行版对 -XshowSettings 无输出）
        if (out.empty()) out = RunCaptureTimeout(L"\"" + exe + L"\" -version", 15000);
    }
    if (out.empty()) return r; // 静默失败/无输出（§1.2 无效输出过滤）
    // 架构：sun.arch.data.model = 32|64（输出已合并；个别 JRE 不带时以 PE 头为准）
    {
        std::wstring lo = out;
        std::transform(lo.begin(), lo.end(), lo.begin(), ::towlower);
        size_t p0 = lo.find(L"sun.arch.data.model");
        if (p0 != std::wstring::npos) {
            size_t eq = lo.find(L"=", p0);
            if (eq != std::wstring::npos) {
                std::wstring v = lo.substr(eq + 1);
                // 去空格与换行
                std::wstring t;
                for (wchar_t c : v) { if (c != L' ' && c != L'\t' && c != L'\r' && c != L'\n') t += c; }
                if (t == L"64") r.is64 = true;
                else if (t == L"32") r.is64 = false;
            }
        }
    }
    r.major = JavaMajorFromVersionText(out);
    // 编码提取：file.encoding / native.encoding（native 缺失回退 file.encoding）
    size_t pfe = out.find(L"file.encoding");
    if (pfe != std::wstring::npos) {
        size_t eq = out.find(L"=", pfe);
        if (eq != std::wstring::npos) {
            std::wstring v = out.substr(eq + 1);
            std::wstring t;
            for (wchar_t c : v) { if (c != L' ' && c != L'\t' && c != L'\r' && c != L'\n' && c != L'"') t += c; }
            if (!t.empty()) r.fileEncoding = t;
        }
    }
    size_t pne = out.find(L"native.encoding");
    if (pne != std::wstring::npos) {
        size_t eq = out.find(L"=", pne);
        if (eq != std::wstring::npos) {
            std::wstring v = out.substr(eq + 1);
            std::wstring t;
            for (wchar_t c : v) { if (c != L' ' && c != L'\t' && c != L'\r' && c != L'\n' && c != L'"') t += c; }
            if (!t.empty()) r.nativeEncoding = t;
        }
    }
    if (r.nativeEncoding.empty()) r.nativeEncoding = r.fileEncoding;
    return r;
}

// 深度枚举辅助：递归查找 java.exe，按重解析点规则过滤（§1.3）
static void DeepEnumJava(const std::wstring& root, std::vector<std::wstring>& out,
                         int depth, std::atomic<int>& scanned) {
    if (depth > 8) return;
    std::error_code ec;
    // 特殊目录直接跳过（系统目录 / javapath 公共目录 / 临时目录）
    std::wstring name = std::filesystem::path(root).filename().wstring();
    std::wstring nlower = name;
    std::transform(nlower.begin(), nlower.end(), nlower.begin(), ::towlower);
    if (nlower == L"windows" || nlower == L"winsxs" || nlower == L"system32" ||
        nlower == L"syswow64" || nlower == L"programdata" || nlower == L"$recycle.bin" ||
        nlower == L"system volume information" || nlower == L"temp" || nlower == L"tmp" ||
        nlower == L"cache" || nlower == L"javapath" || nlower == L"oracle") {
        // javapath 是 Oracle 符号链接目录，但本身不含真正 java.exe；直接跳过其自身，避免死循环
        if (nlower == L"javapath" || nlower == L"system32" || nlower == L"syswow64") return;
    }
    // 自下载运行时目录（启动器 runtime）直接在 ScanInstalledJavas 单独加入，这里遇到也跳过（§1.3 豁免）
    if (!g_exeDir.empty() && root.rfind(g_exeDir + L"\\runtime", 0) == 0) return;

    std::vector<std::wstring> subdirs;
    try {
        for (auto& de : std::filesystem::directory_iterator(root, ec)) {
            if (ec) break;
            std::error_code e2;
            if (de.is_regular_file(e2)) {
                if (de.path().filename() == L"java.exe" || de.path().filename() == L"javaw.exe")
                    out.push_back(de.path().wstring());
                continue;
            }
            if (de.is_directory(e2)) {
                std::wstring sp = de.path().wstring();
                DWORD attrs = GetFileAttributesW(sp.c_str());
                if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) continue; // 重解析点过滤
                subdirs.push_back(sp);
            }
        }
    } catch (...) {}
    if (++scanned > 200000) return; // 全盘扫描上限，避免极端情况下跑太久
    for (const auto& sd : subdirs) DeepEnumJava(sd, out, depth + 1, scanned);
}

// 知名 Minecraft 启动器自带 JRE 的预置路径（Lunar / CurseForge / Badlion / LabyMod /
// Modrinth / 官方启动器 / TLauncher / Prism / MultiMC / GDLauncher / Feather 等）。
// 各启动器 JRE 的子目录名随版本变化（如 Lunar 的 zulu17.40...-win_x64），因此按目录递归
// 查找 javaw.exe（找不到再收 java.exe），深度受限 + 总结果上限，避免启动变慢。
static void AddKnownLauncherJava(std::vector<std::wstring>& out) {
    wchar_t buf[MAX_PATH]{};
    std::wstring profile, local, appdata, pfX86;
    if (GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH)) profile = buf;
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH)) local = buf;
    if (GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH)) appdata = buf;
    if (GetEnvironmentVariableW(L"ProgramFiles(x86)", buf, MAX_PATH)) pfX86 = buf;

    std::vector<std::wstring> bases;
    auto add = [&](const std::wstring& b) { if (!b.empty()) bases.push_back(b); };
    add(profile + L"\\.lunarclient\\jre");     // Lunar Client（新版，jre 下为随机版本子目录）
    add(profile + L"\\.lunarclient\\client");  // Lunar Client（旧版本地 jre）
    add(profile + L"\\.tlauncher");                        // TLauncher
    add(local + L"\\Programs\\CurseForge\\runtime\\java"); // CurseForge（Overwolf）
    add(local + L"\\CurseForge\\Install\\runtime\\java");  // CurseForge（旧版）
    add(local + L"\\Badlion Client");                      // Badlion
    add(appdata + L"\\LabyMod");                           // LabyMod（4 代）
    add(local + L"\\Programs\\LabyMod");                   // LabyMod 启动器
    add(appdata + L"\\ModrinthApp");                       // Modrinth App
    add(local + L"\\Programs\\modrinth-app");              // Modrinth App（Electron 版）
    add(pfX86 + L"\\Minecraft Launcher\\runtime");         // 官方启动器
    add(appdata + L"\\PrismLauncher");                     // Prism Launcher
    add(appdata + L"\\MultiMC");                           // MultiMC
    add(appdata + L"\\GDLauncher");                        // GDLauncher
    add(local + L"\\Programs\\Feather");                   // Feather Client

    std::error_code ec;
    std::function<void(const std::wstring&, int)> scan;
    scan = [&](const std::wstring& root, int depth) {
        if (depth > 5) return;
        std::error_code e;
        std::filesystem::directory_iterator it(root, e), end;
        if (e) return;
        for (; it != end; it.increment(e)) {
            if (e) break;
            const auto& p = it->path();
            const std::wstring fn = p.filename().wstring();
            if (fn == L"javaw.exe") { out.push_back(p.wstring()); if (out.size() >= 24) return; continue; }
            if (fn == L"java.exe")  { out.push_back(p.wstring()); if (out.size() >= 24) return; continue; }
            std::error_code isd;
            if (std::filesystem::is_directory(p, isd)) scan(p.wstring(), depth + 1);
            if (out.size() >= 24) return;
        }
    };
    for (const auto& b : bases) {
        std::error_code e;
        if (std::filesystem::is_directory(b, e)) scan(b, 0);
        if (out.size() >= 24) break;
    }
}

// 扫描系统中已安装的 Java（注册表 / 常见路径 / 启动器 runtime + 深度枚举），并探测版本号
// 全局：要求下一次 ScanInstalledJavas 跳过 30 秒缓存（§1.6/§1.4 全盘重扫用）
static std::atomic<bool> g_forceJavaScan{false};

// ---- Java 扫描缓存持久化与静默验证（跨重启复用 + 后台定期校验）----
// 扫描结果写入 settings.json 的 javaCache（数组）+ javaCacheTime（unix 秒）。
// 缓存有效期 7 天：启动/首次调用时若磁盘缓存仍有效，先直接用（避免每次冷启动全盘枚举）；
// 随后在后台线程重新扫描并回写，实现「先用旧缓存秒回、再静默刷新」。
static constexpr int kJavaCacheMaxAgeSeconds = 7 * 24 * 3600;

// 内存缓存（含 30 秒短缓存 + 后台预扫描标记）
static std::mutex g_javaScanMtx;
static std::vector<InstalledJava> g_javaScanCache;
static std::chrono::steady_clock::time_point g_javaScanLast;
static std::atomic<bool> g_javaScanRunning{false};

// 后台扫描线程向前端推送即时报到桥（注册桥后赋值，用于 java.found 增量事件）
static Bridge* g_bridgeForJavaScan = nullptr;

// 从 settings.json 读取持久化 Java 缓存的原始条目（不做有效期/存在性过滤，供定期验证重扫用）
static std::vector<InstalledJava> ReadJavaCacheEntriesFromSettings(const Json& s) {
    std::vector<InstalledJava> out;
    if (!s.isObject() || !s.contains("javaCache") || !s.at("javaCache").isArray()) return out;
    for (const auto& o : s.at("javaCache").asArray()) {
        if (!o.isObject()) continue;
        InstalledJava j;
        if (o.contains("path") && o.at("path").isString())
            j.path = lxe::Utf8ToWide(o.at("path").asString());
        if (o.contains("major") && o.at("major").isNumber())
            j.major = (int)o.at("major").asNumber();
        if (o.contains("is64") && o.at("is64").isBool())
            j.is64 = o.at("is64").asBool();
        if (o.contains("fileEncoding") && o.at("fileEncoding").isString())
            j.fileEncoding = lxe::Utf8ToWide(o.at("fileEncoding").asString());
        if (o.contains("nativeEncoding") && o.at("nativeEncoding").isString())
            j.nativeEncoding = lxe::Utf8ToWide(o.at("nativeEncoding").asString());
        if (j.major <= 0) continue;
        out.push_back(j);
    }
    return out;
}

// 从 settings.json 读取持久化 Java 缓存（major>0、未过期且路径存在才采纳）
static void LoadJavaCacheFromDisk(std::vector<InstalledJava>& out) {
    Json s = LoadSettingsFile();
    if (!s.isObject() || !s.contains("javaCache")) return;
    if (!s.at("javaCache").isArray()) return;
    // 超过有效期直接丢弃（不刷新 mtime，后台扫描后会重写）
    if (s.contains("javaCacheTime") && s.at("javaCacheTime").isNumber()) {
        time_t saved = (time_t)s.at("javaCacheTime").asNumber();
        time_t now = time(nullptr);
        if (saved > 0 && now - saved > kJavaCacheMaxAgeSeconds) return;
    }
    out = ReadJavaCacheEntriesFromSettings(s);
    // 静默验证：磁盘缓存里的路径已不存在 → 该条过期
    std::error_code ec;
    out.erase(std::remove_if(out.begin(), out.end(), [&](const InstalledJava& j) {
        return !std::filesystem::exists(j.path, ec);
    }), out.end());
}

// 将内存最新扫描结果写入 settings.json（javaCache/javaCacheTime 两个键）
static void SaveJavaCacheToDisk(const std::vector<InstalledJava>& javas) {
    try {
        Json patch = Json::object();
        Json arr = Json::array();
        for (const auto& j : javas) {
            Json o = Json::object();
            o["path"] = lxe::WideToUtf8(j.path);
            o["major"] = j.major;
            o["is64"] = j.is64;
            if (!j.fileEncoding.empty()) o["fileEncoding"] = lxe::WideToUtf8(j.fileEncoding);
            if (!j.nativeEncoding.empty()) o["nativeEncoding"] = lxe::WideToUtf8(j.nativeEncoding);
            arr.asArray().push_back(o);
        }
        patch["javaCache"] = arr;
        patch["javaCacheTime"] = (double)time(nullptr);
        SaveSettingsFile(patch);
    } catch (...) {}
}

// 探测到一个可用 Java 时立即：并入内存缓存 + 回写磁盘缓存 + 向前端广播 java.found 增量事件。
// 满足「找到一个就加进列表 / 每次检测到就直接缓存」，前端收到事件即可即时追加渲染。
static void InsertJavaFoundAndNotify(const InstalledJava& j) {
    if (j.major <= 0 || !j.is64) return;
    bool added = false;
    std::vector<InstalledJava> snapshot;
    {
        std::lock_guard<std::mutex> lock(g_javaScanMtx);
        bool exists = false;
        for (const auto& c : g_javaScanCache)
            if (_wcsicmp(c.path.c_str(), j.path.c_str()) == 0) { exists = true; break; }
        if (!exists) g_javaScanCache.push_back(j);
        added = !exists;
        if (added) snapshot = g_javaScanCache;
    }
    if (added) SaveJavaCacheToDisk(snapshot);
    if (!g_bridgeForJavaScan) return;
    try {
        Json ev = Json::object();
        ev["path"] = lxe::WideToUtf8(j.path);
        ev["major"] = j.major;
        ev["is64"] = j.is64;
        if (!j.fileEncoding.empty()) ev["fileEncoding"] = lxe::WideToUtf8(j.fileEncoding);
        if (!j.nativeEncoding.empty()) ev["nativeEncoding"] = lxe::WideToUtf8(j.nativeEncoding);
        g_bridgeForJavaScan->PostEvent("java.found", ev);
    } catch (...) {}
}

// 将某个 Java 从「已找到」列表删除：移除内存缓存 + 回写磁盘缓存 + 通知前端 java.removed。
// 用于缓存过期验证时路径已不存在、或启动器再也找不到该 Java 的情况（§15 续期/删除）。
static void RemoveJavaFromCacheAndNotify(const std::wstring& path) {
    {
        std::lock_guard<std::mutex> lock(g_javaScanMtx);
        auto& v = g_javaScanCache;
        for (auto it = v.begin(); it != v.end(); ++it) {
            if (_wcsicmp(it->path.c_str(), path.c_str()) == 0) { v.erase(it); break; }
        }
    }
    // 回写磁盘（删除该条；SaveJavaCacheToDisk 会更新时间戳 = 续期）
    std::vector<InstalledJava> snap;
    {
        std::lock_guard<std::mutex> lock(g_javaScanMtx);
        snap = g_javaScanCache;
    }
    SaveJavaCacheToDisk(snap);
    if (!g_bridgeForJavaScan) return;
    try {
        Json ev = Json::object();
        ev["path"] = lxe::WideToUtf8(path);
        g_bridgeForJavaScan->PostEvent("java.removed", ev);
    } catch (...) {}
}

// 定期验证：逐条校验磁盘缓存路径是否仍存在。不存在的条目直接删除（内存+磁盘+前端通知）；
// 缓存过期时把仍存在的条目续期（重写时间戳）并触发后台重扫发现新安装的 Java。
static void PruneAndRefreshJavaCache(bool expired) {
    std::error_code ec;
    std::vector<InstalledJava> disk = ReadJavaCacheEntriesFromSettings(LoadSettingsFile());
    std::vector<InstalledJava> mem;
    {
        std::lock_guard<std::mutex> lock(g_javaScanMtx);
        mem = g_javaScanCache;
    }
    bool anyRemoved = false;
    std::vector<InstalledJava> kept;
    kept.reserve(disk.size() + mem.size());
    for (const auto& j : disk) {
        if (std::filesystem::exists(j.path, ec)) kept.push_back(j);
        else { RemoveJavaFromCacheAndNotify(j.path); anyRemoved = true; }
    }
    // 并入本会话内存缓存中新增（磁盘可能尚未回写的）条目
    for (const auto& j : mem) {
        bool has = false;
        for (const auto& k : kept) if (_wcsicmp(k.path.c_str(), j.path.c_str()) == 0) { has = true; break; }
        if (!has && std::filesystem::exists(j.path, ec)) kept.push_back(j);
    }
    {
        std::lock_guard<std::mutex> lock(g_javaScanMtx);
        g_javaScanCache = kept;
    }
    // 有过删除或缓存过期 → 回写磁盘（SaveJavaCacheToDisk 刷新时间戳 = 续期）
    if (anyRemoved || expired) SaveJavaCacheToDisk(kept);
}

// 触发一次全量扫描（深枚举可能数秒）：作为后台线程执行，不阻塞调用方
static void AsyncScanJava() {
    if (g_javaScanRunning.exchange(true)) return;
    std::thread([]() {
        try {
            ScanInstalledJavasNow();
        } catch (...) {}
        g_javaScanRunning.store(false);
    }).detach();
}

// 定期自动验证 Java 缓存：每 kPeriodic 秒在后台轻量校验缓存路径是否仍存在；
// 路径不存在的条目直接从列表删除（含前端通知）；缓存过期时把仍存在的条目续期（重写
// 时间戳），并触发一次后台重扫以发现新安装的 Java。启动时先同步刷新一次缓存，
// 避免启动后较长时间内 ScanInstalledJavas 仍返回旧列表。
static void StartPeriodicJavaVerify() {
    constexpr int kPeriodicSeconds = 600; // 10 分钟
    std::thread([]{ // 首次延迟 3s，避开启动期 IO 高峰
        std::this_thread::sleep_for(std::chrono::seconds(3));
        for (;;) {
            try {
                Json s = LoadSettingsFile();
                time_t saved = 0;
                if (s.isObject() && s.contains("javaCacheTime") && s.at("javaCacheTime").isNumber())
                    saved = (time_t)s.at("javaCacheTime").asNumber();
                time_t now = time(nullptr);
                bool expired = (saved <= 0) || (now - saved > kJavaCacheMaxAgeSeconds);
                // 逐条验证：仍存在的续期，缺失的直接删除；过期时重写时间戳 = 续期
                PruneAndRefreshJavaCache(expired);
                if (expired) {
                    // 续期后仍需发现新安装的 Java：后台重扫并回写（互斥防并发）
                    if (!g_javaScanRunning.load()) {
                        AsyncScanJava();
                    } else {
                        g_forceJavaScan.store(true);
                    }
                }
            } catch (...) {}
            std::this_thread::sleep_for(std::chrono::seconds(kPeriodicSeconds));
        }
    }).detach();
}

static std::vector<InstalledJava> ScanInstalledJavas() {
    static std::mutex s_mtx;
    static std::vector<InstalledJava> s_cache;
    static std::chrono::steady_clock::time_point s_last;
    {
        std::lock_guard<std::mutex> lock(s_mtx);
        auto now = std::chrono::steady_clock::now();
        bool force = g_forceJavaScan.exchange(false);
        if (!s_cache.empty() && !force && now - s_last < std::chrono::seconds(30))
            return s_cache;
    }
    // 首次调用（内存缓存为空）时：先尝试磁盘持久化缓存，命中则先用，同时后台续扫并回写（体验：秒回 + 静默刷新）
    {
        std::lock_guard<std::mutex> lock(g_javaScanMtx);
        if (s_cache.empty() && g_javaScanCache.empty()) {
            std::vector<InstalledJava> disk;
            LoadJavaCacheFromDisk(disk);
            if (!disk.empty()) {
                g_javaScanCache = disk;
                s_last = std::chrono::steady_clock::now();
                std::lock_guard<std::mutex> lock2(s_mtx);
                s_cache = disk;
                AsyncScanJava(); // 后台静默重新扫描并回写磁盘缓存
                return s_cache;
            }
        }
    }
    std::vector<InstalledJava> out;
    std::vector<std::wstring> candidates;
    const wchar_t* roots[] = {
        L"SOFTWARE\\JavaSoft\\Java Development Kit",
        L"SOFTWARE\\JavaSoft\\JDK",
        L"SOFTWARE\\JavaSoft\\Java Runtime Environment",
        L"SOFTWARE\\WOW6432Node\\JavaSoft\\Java Development Kit",
        L"SOFTWARE\\WOW6432Node\\JavaSoft\\JDK",
        L"SOFTWARE\\WOW6432Node\\JavaSoft\\Java Runtime Environment",
    };
    for (const auto* root : roots) {
        HKEY hKey = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, root, 0, KEY_READ, &hKey) != ERROR_SUCCESS) continue;
        wchar_t sub[256];
        DWORD subLen = 256;
        for (DWORD i = 0; RegEnumKeyExW(hKey, i, sub, &subLen, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
             ++i, subLen = 256) {
            HKEY hVer = nullptr;
            std::wstring verPath = std::wstring(root) + L"\\" + sub;
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, verPath.c_str(), 0, KEY_READ, &hVer) != ERROR_SUCCESS) continue;
            wchar_t home[MAX_PATH]{};
            DWORD sz = sizeof(home);
            if (RegQueryValueExW(hVer, L"JavaHome", nullptr, nullptr, reinterpret_cast<LPBYTE>(home), &sz) == ERROR_SUCCESS) {
                std::wstring base = home;
                std::wstring p = base + L"\\bin\\javaw.exe";
                std::error_code sec;
                if (std::filesystem::exists(p, sec)) candidates.push_back(p);
                else {
                    std::wstring p2 = base + L"\\bin\\java.exe";
                    std::error_code sec2;
                    if (std::filesystem::exists(p2, sec2)) candidates.push_back(p2);
                }
            }
            RegCloseKey(hVer);
        }
        RegCloseKey(hKey);
    }
    const wchar_t* fixed[] = {
        L"C:\\Program Files\\Java\\jre-21\\bin\\javaw.exe",
        L"C:\\Program Files\\Java\\jdk-21\\bin\\javaw.exe",
        L"C:\\Program Files\\Java\\jre-17\\bin\\javaw.exe",
        L"C:\\Program Files\\Java\\jdk-17\\bin\\javaw.exe",
        L"C:\\Program Files\\Java\\jre-8\\bin\\javaw.exe",
        L"C:\\Program Files\\Java\\jdk-8\\bin\\javaw.exe",
        L"C:\\Program Files (x86)\\Java\\jre-8\\bin\\javaw.exe",
        L"C:\\Program Files\\Eclipse Adoptium\\jdk-21\\bin\\javaw.exe",
        L"C:\\Program Files\\Eclipse Adoptium\\jdk-17\\bin\\javaw.exe",
        L"C:\\Program Files\\Eclipse Adoptium\\jdk-8\\bin\\javaw.exe",
    };
    std::error_code fec;
    for (auto p : fixed) if (std::filesystem::exists(p, fec)) candidates.push_back(p);

    // 知名启动器自带 JRE（Lunar/CurseForge/Badlion/LabyMod/Modrinth/官方启动器等）
    AddKnownLauncherJava(candidates);

    // §1.3 预定义目录（懒加载）：环境变量 JAVA_HOME / JDK_HOME
    {
        auto envDir = [&](const wchar_t* var) {
            wchar_t buf[0x8000] = {};
            DWORD n = GetEnvironmentVariableW(var, buf, 0x8000);
            if (n == 0 || n >= 0x8000) return;
            std::wstring base = buf;
            std::wstring p = base + L"\\bin\\javaw.exe";
            std::error_code e;
            if (std::filesystem::exists(p, e)) candidates.push_back(p);
            else {
                std::wstring p2 = base + L"\\bin\\java.exe";
                std::error_code e2;
                if (std::filesystem::exists(p2, e2)) candidates.push_back(p2);
            }
        };
        envDir(L"JAVA_HOME");
        envDir(L"JDK_HOME");
    }
    if (!g_exeDir.empty()) {
        std::wstring runtimeDir = g_exeDir + L"\\runtime";
        std::error_code ec;
        for (auto& entry : std::filesystem::directory_iterator(runtimeDir, ec)) {
            if (!entry.is_directory()) continue;
            std::wstring dirName = entry.path().filename().wstring();
            if (dirName.rfind(L"java-", 0) != 0) continue;
            for (auto& sub : std::filesystem::recursive_directory_iterator(entry.path(), ec)) {
                if (sub.path().filename() == L"javaw.exe") { candidates.push_back(sub.path().wstring()); break; }
            }
        }
    }
    // 候选分级：预定义目录（注册表/常见路径/JAVA_HOME/runtime）等级 0，深度枚举等级 1（§1.3/§1.4 排序）
    struct Cand { std::wstring path; int level; };
    std::vector<Cand> cands;
    auto pushCand = [&](const std::wstring& p, int lvl) {
        for (const auto& c : cands) if (_wcsicmp(c.path.c_str(), p.c_str()) == 0) return;
        cands.push_back({ p, lvl });
    };
    for (const auto& p : candidates) pushCand(p, 0);

    // §1.3 深度枚举：所有可用分区并行搜索 java.exe（并行任务数上限 4）
    {
        std::vector<std::wstring> roots;
        DWORD mask = GetLogicalDrives();
        for (int i = 0; i < 26; ++i) {
            if (mask & (1u << i)) {
                std::wstring root = std::wstring(1, L'A' + (wchar_t)i) + L":\\";
                roots.push_back(root);
            }
        }
        std::vector<std::wstring> deepRes;
        std::atomic<int> nextRoot{0};
        std::atomic<int> scanned{0};
        auto deepFn = [&]() {
            while (true) {
                int i = nextRoot.fetch_add(1);
                if (i >= (int)roots.size()) break;
                DeepEnumJava(roots[i], deepRes, 0, scanned);
            }
        };
        int deepThreads = (int)roots.size();
        if (deepThreads < 1) deepThreads = 1;
        if (deepThreads > 4) deepThreads = 4;
        std::vector<std::thread> dpool;
        for (int t = 0; t < deepThreads; ++t) dpool.emplace_back(deepFn);
        for (auto& t : dpool) t.join();
        for (const auto& p : deepRes) pushCand(p, 1);
    }

    std::vector<Cand> toProbe;
    for (auto& c : cands) toProbe.push_back(c);
    // 并行探测版本号：多套 JDK 时显著提速（串行需逐一启动 JVM，很慢）
    std::vector<InstalledJava> probed(toProbe.size());
    std::atomic<int> probeNext{0};
    std::mutex foundMtx;
    auto probeFn = [&]() {
        while (true) {
            int i = probeNext.fetch_add(1);
            if (i >= (int)toProbe.size()) break;
            InstalledJava p = ProbeJavaExe(toProbe[i].path);
            p.path = toProbe[i].path;
            probed[i] = p;
            // 每发现一个可用 Java 立即增量加入（§2.6：找到即上报，不等全盘扫完）
            if (p.major > 0 && p.is64) {
                InsertJavaFoundAndNotify(p);
            }
        }
    };
    int probeThreads = (int)toProbe.size();
    if (probeThreads < 1) probeThreads = 1;
    if (probeThreads > 8) probeThreads = 8;
    std::vector<std::thread> pool;
    for (int t = 0; t < probeThreads; ++t) pool.emplace_back(probeFn);
    for (auto& t : pool) t.join();
    // §1.2 仅接受 64 位 / 版本可解析
    for (int i = 0; i < (int)toProbe.size(); ++i) {
        const auto& j = probed[i];
        if (j.major <= 0) continue;
        if (!j.is64) continue;
        InstalledJava ji = j;
        ji.path = toProbe[i].path;
        out.push_back(ji);
    }
    // §1.4 排序：预定义目录优先，再按 |主版本-21| 差值升序（越接近 21 越靠前）
    std::stable_sort(out.begin(), out.end(), [&](const InstalledJava& a, const InstalledJava& b) {
        int la = 0, lb = 1;
        for (size_t i = 0; i < toProbe.size(); ++i) {
            if (_wcsicmp(toProbe[i].path.c_str(), a.path.c_str()) == 0) { la = toProbe[i].level; break; }
        }
        for (size_t i = 0; i < toProbe.size(); ++i) {
            if (_wcsicmp(toProbe[i].path.c_str(), b.path.c_str()) == 0) { lb = toProbe[i].level; break; }
        }
        if (la != lb) return la < lb;
        int da = std::abs(a.major - 21), db = std::abs(b.major - 21);
        if (da != db) return da < db;
        return a.major < b.major;
    });
    {
        std::lock_guard<std::mutex> lock(s_mtx);
        s_cache = out;
        s_last = std::chrono::steady_clock::now();
        g_javaScanCache = out;
        g_javaScanLast = s_last;
    }
    // 回写磁盘缓存（持久化），在锁外串行执行
    SaveJavaCacheToDisk(out);
    return out;
}

// 强制忽略 30 秒缓存重新扫描（§1.6 第 2 步：重新执行全盘扫描后再次选取）
static std::vector<InstalledJava> ScanInstalledJavasNow() {
    g_forceJavaScan.store(true);
    return ScanInstalledJavas();
}

// 根据 MC 版本推荐所需 Java 主版本（<1.17 → 8；1.17~1.20.4 → 17；1.20.5+ → 21）
static int RecommendedJavaMajor(const std::string& mcVersion) {
    std::string v = mcVersion;
    size_t p1 = v.find('.');
    if (p1 == std::string::npos) return 8; // 无点号：snapshot / alpha / beta / rd-* 等一律按旧版处理
    std::string maj = v.substr(0, p1);
    bool majIsOne = (maj == "1");
    if (!majIsOne) {
        // 主段非 "1"：老版本（如 c0.0.13a_03、b1.7.3）→ 8；纯数字的 "2.x" 等 → 21
        bool numeric = !maj.empty() && std::all_of(maj.begin(), maj.end(), [](char c) { return c >= '0' && c <= '9'; });
        return numeric ? 21 : 8;
    }
    size_t p2 = v.find('.', p1 + 1);
    std::string min = v.substr(p1 + 1, (p2 == std::string::npos) ? std::string::npos : (p2 - p1 - 1));
    std::string patch = (p2 == std::string::npos) ? "" : v.substr(p2 + 1);
    int minor = 0;
    for (char c : min) { if (c < '0' || c > '9') break; minor = minor * 10 + (c - '0'); }
    if (minor >= 21) return 21;
    if (minor == 20) {
        int pat = 0;
        for (char c : patch) { if (c < '0' || c > '9') break; pat = pat * 10 + (c - '0'); }
        if (pat >= 5) return 21;
        return 17;
    }
    if (minor >= 17) return 17;
    return 8;
}

// 从合并后的 version.json 读取 javaVersion.majorVersion（原版 values may be taken from
// Mojang 官方 manifest / loaders 合成 JSON 生成），找不到时回退到版本号推导。
static int JavaMajorFromVersionJson(const Json& vj, const std::string& mcVersion) {
    if (vj.isObject() && vj.contains("javaVersion") && vj.at("javaVersion").isObject()) {
        const auto& jv = vj.at("javaVersion");
        if (jv.contains("majorVersion") && jv.at("majorVersion").isNumber()) {
            int m = (int)jv.at("majorVersion").asNumber();
            if (m > 0) return m;
        }
    }
    return RecommendedJavaMajor(mcVersion);
}

// 从可用 Java 列表中选择匹配主版本的路径；无匹配则取第一个可用
static std::wstring PickJavaForMajor(const std::vector<InstalledJava>& javas, int major) {
    std::wstring first;
    for (const auto& j : javas) {
        if (first.empty()) first = j.path;
        if (j.major == major) return j.path;
    }
    return first;
}

// §1.5 需求版本区间：由 加载器兼容矩阵 + 原版推荐 + 离线最低要求 求交集。
// 返回 {loMajor, hiMajor, note}；loMajor>hiMajor 表示该组合不支持。userMajor>0 时用户指定直接覆盖。
// vjMajor>0 时表示 version.json 的 javaVersion.majorVersion（最准），优先于版本号推导。
struct JavaReq { int lo; int hi; std::string note; };
static JavaReq JavaRequirement(const std::string& mcVersion, const std::string& loaderId,
                               bool offline, int userMajor, int vjMajor = 0) {
    if (userMajor > 0) return { userMajor, userMajor, "用户指定" };
    int base = vjMajor > 0 ? vjMajor : RecommendedJavaMajor(mcVersion);
    int lo = base, hi = base;
    std::string note;
    std::string lw = loaderId;
    std::transform(lw.begin(), lw.end(), lw.begin(), ::tolower);
    // 加载器兼容矩阵（§1.5.3）：按游戏主版本查表。主版本 >20.5 一律 21+；1.17~1.20.4 一律 17。
    bool is21 = (base == 21), is17 = (base == 17), is8 = (base == 8);
    if (lw == "neoforge") {
        if (is8) { lo = 8; hi = 8; note = "NeoForge 仅支持 1.20.1 起的版本（Java 8 不适用）"; }
        else if (is17) { lo = 17; hi = 17; note = "NeoForge 1.20.1~1.20.4 需要 Java 17"; }
        else { lo = 21; hi = 21; note = "NeoForge 1.20.5+ 需要 Java 21"; }
    } else if (lw == "forge") {
        if (is8) { lo = 8; hi = 8; note = "Forge 旧版本需要 Java 8"; }
        else if (is17) { lo = 17; hi = 17; note = "Forge 1.17~1.20.4 需要 Java 17"; }
        else { lo = 21; hi = 21; note = "Forge 1.20.5+ 需要 Java 21"; }
    } else if (lw == "fabric" || lw == "quilt" || lw == "liteloader") {
        if (is8) { lo = 8; hi = 17; note = "Fabric/Quilt 旧版本最低 Java 8（部分需 17）"; }
        else { lo = 17; hi = 17; note = "Fabric/Quilt 1.17+ 需要 Java 17"; }
        if (lw == "liteloader") { lo = 8; hi = 8; note = "LiteLoader 需要 Java 8"; }
    } else if (lw == "optifine") {
        note = "OptiFine 与对应 MC 版本一致";
    } else {
        // 原版：按版本阈值
        note = is8 ? "Minecraft 旧版本推荐 Java 8" : (is17 ? "Minecraft 1.17~1.20.4 推荐 Java 17" : "Minecraft 1.20.5+ 推荐 Java 21");
    }
    // 离线登录最低要求 8u141+（主版本 8 合格，具体 build 由参数/校验把握）
    if (offline && lo < 8) { lo = 8; note += "；离线模式最低 Java 8u141+"; }
    else if (offline && lo == 8) { note += "；离线模式需 8u141+"; }
    // Java 向后兼容：现代版本（需求下限 ≥17）接受更高主版本（如 Java 21/25），
    // 避免把更高版本的合法 JVM 判为无效，也避免误触发自动下载；老版本（下限 8）保持窄区间。
    if (lo >= 17 && hi < 90) hi = 90;
    return { lo, hi, note };
}

// 生成人可读需求描述（§1.6 失败提示不暴露原始数据）
static std::string JavaRequirementText(const JavaReq& r) {
    if (r.note.empty()) return std::string();
    if (r.lo == r.hi) {
        if (!r.note.empty() && r.note.find("离线") != std::string::npos) return r.note;
        return r.note;
    }
    return r.note;
}

// 优化后的版本类型判断：主类 / inheritsFrom / libraries / 版本名 多重判定（forge/fabric/quilt/neoforge/optifine/vanilla）
static std::string DetectVersionType(const Json& vj, const std::string& id) {
    std::string mainClass;
    if (vj.contains("mainClass") && vj.at("mainClass").isString()) mainClass = vj.at("mainClass").asString();
    std::string mc = mainClass;
    std::transform(mc.begin(), mc.end(), mc.begin(), ::tolower);
    if (mc.find("neoforge") != std::string::npos) return "neoforge";
    if (mc.find("fabric") != std::string::npos) return "fabric";
    if (mc.find("quilt") != std::string::npos) return "quilt";
    if (mc.find("forge") != std::string::npos) return "forge";
    if (vj.contains("inheritsFrom") && vj.at("inheritsFrom").isString()) {
        std::string parent = vj.at("inheritsFrom").asString();
        std::string lp = parent;
        std::transform(lp.begin(), lp.end(), lp.begin(), ::tolower);
        if (lp.find("neoforge") != std::string::npos) return "neoforge";
        if (lp.find("fabric") != std::string::npos) return "fabric";
        if (lp.find("quilt") != std::string::npos) return "quilt";
        if (lp.find("forge") != std::string::npos) return "forge";
    }
    if (vj.contains("libraries") && vj.at("libraries").isArray()) {
        for (const auto& lib : vj.at("libraries").asArray()) {
            if (!lib.isObject() || !lib.contains("name") || !lib.at("name").isString()) continue;
            std::string name = lib.at("name").asString();
            std::string ln = name;
            std::transform(ln.begin(), ln.end(), ln.begin(), ::tolower);
            if (ln.find("net.neoforged") != std::string::npos || ln.find("org.neoforge") != std::string::npos) return "neoforge";
            if (ln.find("net.fabricmc") != std::string::npos) return "fabric";
            if (ln.find("org.quiltmc") != std::string::npos) return "quilt";
            if (ln.find("optifine") != std::string::npos) return "optifine";
            if (ln.find("net.minecraftforge") != std::string::npos) return "forge";
        }
    }
    std::string lid = id;
    std::transform(lid.begin(), lid.end(), lid.begin(), ::tolower);
    if (lid.find("optifine") != std::string::npos || lid.find(" of") != std::string::npos || lid.rfind("of", 0) == 0) return "optifine";
    if (lid.find("neoforge") != std::string::npos) return "neoforge";
    if (lid.find("fabric") != std::string::npos) return "fabric";
    if (lid.find("quilt") != std::string::npos) return "quilt";
    if (lid.find("forge") != std::string::npos) return "forge";
    return "vanilla";
}

// 确保 log4j 配置文件存在（缺失时按 version.json logging.client 下载），返回可用路径（无 logging 段返回空）
static std::wstring EnsureLoggingConfig(const Json& vj, const std::wstring& mcRoot) {
    if (!vj.contains("logging") || !vj.at("logging").isObject()) return L"";
    const auto& lg = vj.at("logging");
    if (!lg.contains("client") || !lg.at("client").isObject()) return L"";
    const auto& c = lg.at("client");
    std::string id, url;
    if (c.contains("file") && c.at("file").isObject()) {
        const auto& f = c.at("file");
        if (f.contains("id") && f.at("id").isString()) id = f.at("id").asString();
        if (f.contains("url") && f.at("url").isString()) url = f.at("url").asString();
    }
    if (id.empty()) return L"";
    std::wstring cfgDir = mcRoot + L"\\assets\\log_configs";
    std::wstring cfgPath = cfgDir + L"\\" + lxe::Utf8ToWide(id);
    if (std::filesystem::exists(cfgPath)) return cfgPath;
    if (!url.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(cfgDir, ec);
        DownloadFileSmart(lxe::Utf8ToWide(url), cfgDir, lxe::Utf8ToWide(id), nullptr);
        if (std::filesystem::exists(cfgPath)) return cfgPath;
    }
    return L"";
}

// 确保资源索引文件存在（缺失时下载），返回索引路径
static std::wstring EnsureAssetIndex(const Json& vj, const std::wstring& mcRoot) {
    if (!vj.contains("assetIndex") || !vj.at("assetIndex").isObject()) return L"";
    const auto& ai = vj.at("assetIndex");
    std::string id, url;
    if (ai.contains("id") && ai.at("id").isString()) id = ai.at("id").asString();
    if (ai.contains("url") && ai.at("url").isString()) url = ai.at("url").asString();
    if (id.empty()) return L"";
    std::wstring idxDir = mcRoot + L"\\assets\\indexes";
    std::wstring idxPath = idxDir + L"\\" + lxe::Utf8ToWide(id) + L".json";
    if (std::filesystem::exists(idxPath)) return idxPath;
    if (!url.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(idxDir, ec);
        DownloadFileSmart(lxe::Utf8ToWide(url), idxDir, lxe::Utf8ToWide(id) + L".json", nullptr);
    }
    return std::filesystem::exists(idxPath) ? idxPath : L"";
}

// 补全全部资源文件（objects + virtual/legacy 拷贝），返回成功数量；progress 回调 (当前, 总数)
static int CompleteAssets(const Json& vj, const std::wstring& mcRoot,
                          const std::function<void(int, int, const std::wstring&)>& progress) {
    std::wstring idxPath = EnsureAssetIndex(vj, mcRoot);
    if (idxPath.empty()) return 0;
    auto [text, ok] = ReadFileUtf8(idxPath);
    if (!ok || text.empty()) return 0;
    Json idx;
    try { idx = Json::parse(text); } catch (...) { return 0; }
    if (!idx.contains("objects") || !idx.at("objects").isObject()) return 0;
    const auto& objects = idx.at("objects");
    std::wstring objectsDir = mcRoot + L"\\assets\\objects";
    std::wstring virtualDir = mcRoot + L"\\assets\\virtual\\legacy";
    std::vector<std::pair<std::string, Json>> entries;
    for (const auto& kv : objects.asObject()) entries.push_back(kv);
    int total = (int)entries.size(), done = 0;
    std::error_code ec;
    for (const auto& kv : entries) {
        if (progress) progress(done, total, lxe::Utf8ToWide(kv.first));
        if (!kv.second.isObject()) { done++; continue; }
        const auto& o = kv.second;
        if (!o.contains("hash") || !o.at("hash").isString()) { done++; continue; }
        std::string hash = o.at("hash").asString();
        if (hash.size() < 2) { done++; continue; }
        std::wstring rel = lxe::Utf8ToWide(hash.substr(0, 2)) + L"\\" + lxe::Utf8ToWide(hash);
        std::wstring objPath = objectsDir + L"\\" + rel;
        if (!std::filesystem::exists(objPath)) {
            std::filesystem::create_directories(std::filesystem::path(objPath).parent_path(), ec);
            std::string url = "https://resources.download.minecraft.net/" + hash.substr(0, 2) + "/" + hash;
            DownloadFileSmart(lxe::Utf8ToWide(url),
                              std::filesystem::path(objPath).parent_path(),
                              lxe::Utf8ToWide(hash), nullptr);
        }
        done++;
    }
    if (progress) progress(total, total, L"");
    return total;
}

static std::wstring FindJavaPath() {
    // 1) 检查注册表 JavaSoft（按优先级逐个尝试，记住使用的分支以便拼接版本路径）
    struct RegBranch { const wchar_t* root; bool isJre; };
    const RegBranch branches[] = {
        { L"SOFTWARE\\JavaSoft\\Java Development Kit", false },
        { L"SOFTWARE\\JavaSoft\\JDK",                     false },
        { L"SOFTWARE\\WOW6432Node\\JavaSoft\\Java Runtime Environment", true  },
        { L"SOFTWARE\\JavaSoft\\Java Runtime Environment", true  },
    };
    for (const auto& br : branches) {
        HKEY hKey = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, br.root, 0, KEY_READ, &hKey) != ERROR_SUCCESS) continue;
        wchar_t subKeyName[256];
        DWORD subKeyLen = 256;
        FILETIME ft;
        if (RegEnumKeyExW(hKey, 0, subKeyName, &subKeyLen, nullptr, nullptr, nullptr, &ft) == ERROR_SUCCESS) {
            HKEY hVer = nullptr;
            std::wstring verPath = std::wstring(br.root) + L"\\" + std::wstring(subKeyName);
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, verPath.c_str(), 0, KEY_READ, &hVer) == ERROR_SUCCESS) {
                wchar_t javaHome[MAX_PATH]{};
                DWORD sz = sizeof(javaHome);
                if (RegQueryValueExW(hVer, L"JavaHome", nullptr, nullptr, reinterpret_cast<LPBYTE>(javaHome), &sz) == ERROR_SUCCESS) {
                    RegCloseKey(hVer);
                    RegCloseKey(hKey);
                    return std::wstring(javaHome) + L"\\bin\\javaw.exe";
                }
                RegCloseKey(hVer);
            }
        }
        RegCloseKey(hKey);
    }

    // 2) 检查常见路径
    const wchar_t* candidates[] = {
        L"C:\\Program Files\\Java\\jre-21\\bin\\javaw.exe",
        L"C:\\Program Files\\Java\\jdk-21\\bin\\javaw.exe",
        L"C:\\Program Files\\Java\\jre-17\\bin\\javaw.exe",
        L"C:\\Program Files\\Java\\jdk-17\\bin\\javaw.exe",
        L"C:\\Program Files (x86)\\Java\\jre-8\\bin\\javaw.exe",
        L"C:\\Program Files\\Eclipse Adoptium\\jdk-21\\bin\\javaw.exe",
        L"C:\\Program Files\\Eclipse Adoptium\\jdk-17\\bin\\javaw.exe",
    };
    for (auto p : candidates) {
        if (GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES) return p;
    }

    // 3) 知名启动器自带 JRE（Lunar/CurseForge/Badlion/LabyMod/Modrinth/官方启动器等）
    {
        std::vector<std::wstring> launcherJavas;
        AddKnownLauncherJava(launcherJavas);
        for (auto p : launcherJavas) {
            if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p;
        }
    }

    // 4) 检查启动器下载的 Java（runtime/java-* 目录）
    if (!g_exeDir.empty()) {
        std::wstring runtimeDir = g_exeDir + L"\\runtime";
        std::error_code ec;
        for (auto& entry : std::filesystem::directory_iterator(runtimeDir, ec)) {
            if (!entry.is_directory()) continue;
            std::wstring dirName = entry.path().filename().wstring();
            if (dirName.rfind(L"java-", 0) != 0) continue;
            // 递归查找 javaw.exe
            for (auto& sub : std::filesystem::recursive_directory_iterator(entry.path(), ec)) {
                if (sub.path().filename() == L"javaw.exe") {
                    return sub.path().wstring();
                }
            }
        }
    }
    return L"";
}

// 按教程规则计算 Forge artifact 版本字符串（用于安装器 maven 下载路径与安装后生成的版本目录名）
// minor==8 且 build==8/空（1.8、1.8.8）→ {mc}-{forge}；minor==7/8（1.7.x、1.8.9）→ {mc}-{forge}-{mc}；其余 → {mc}-{forge}
static std::string ForgeArtifactVersion(const std::string& mcVersion, const std::string& forgeVersion) {
    size_t p1 = mcVersion.find('.');
    size_t p2 = (p1 == std::string::npos) ? std::string::npos : mcVersion.find('.', p1 + 1);
    if (p1 != std::string::npos && p1 > 0 && mcVersion.compare(0, 1, "1") == 0 && p2 != std::string::npos) {
        std::string min = mcVersion.substr(p1 + 1, p2 - p1 - 1);
        std::string patch = mcVersion.substr(p2 + 1);
        int minor = 0;
        for (char c : min) { if (c < '0' || c > '9') break; minor = minor * 10 + (c - '0'); }
        if (minor == 8) {
            int build = -1;
            if (!patch.empty()) { build = 0; for (char c : patch) { if (c < '0' || c > '9') break; build = build * 10 + (c - '0'); } }
            if (build == 8 || build == -1) return mcVersion + "-" + forgeVersion;
            return mcVersion + "-" + forgeVersion + "-" + mcVersion;
        }
        if (minor == 7) return mcVersion + "-" + forgeVersion + "-" + mcVersion;
    }
    return mcVersion + "-" + forgeVersion;
}

// 将官方 maven 下载地址转换为 BMCLAPI maven 镜像地址（教程推荐镜像加速）
static std::string BmclMavenMirror(const std::string& url) {
    std::string mirror = "https://bmclapi2.bangbang93.com/maven/";
    size_t slash = url.find("//");
    if (slash == std::string::npos) return mirror;
    size_t pathStart = url.find('/', slash + 2);
    if (pathStart == std::string::npos) return mirror;
    return mirror + url.substr(pathStart + 1);
}

// maven 坐标 -> .minecraft\libraries 下的完整文件路径
static std::wstring ForgeLibFullPath(const std::wstring& libDir, const std::string& coord) {
    std::string rel = MavenCoordToPath(coord);
    if (rel.empty()) return L"";
    std::wstring wr = lxe::Utf8ToWide(rel);
    std::replace(wr.begin(), wr.end(), L'/', L'\\');
    return libDir + L"\\" + wr;
}

// 从 zip/jar 中解压单个条目到 outDir（借助系统 bsdtar；.jar 本质是 zip）
static bool ExtractZipEntry(const std::wstring& archivePath, const std::string& entry, const std::wstring& outDir) {
    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);
    std::wstring cmd = L"tar -xf \"" + archivePath + L"\" -C \"" + outDir + L"\" \"" + lxe::Utf8ToWide(entry) + L"\"";
    return RunProcessSilent(cmd, outDir) == 0;
}

// 读取 jar 的 META-INF/MANIFEST.MF 中的 Main-Class（processor 主类名）
static std::string ReadJarMainClass(const std::wstring& jarPath) {
    std::wstring cmd = L"tar -xOf \"" + jarPath + L"\" \"META-INF/MANIFEST.MF\"";
    std::wstring out = RunCapture(cmd);
    std::string text = lxe::WideToUtf8(out);
    std::istringstream iss(text);
    std::string line;
    while (std::getline(iss, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.rfind("Main-Class:", 0) == 0) {
            std::string v = line.substr(11);
            size_t b = v.find_first_not_of(" \t");
            size_t e = v.find_last_not_of(" \t");
            if (b != std::string::npos) v = v.substr(b, e - b + 1);
            return v;
        }
    }
    return "";
}

// 读取 jar 内某个 .class 条目的字节码版本号（class 文件头 offset 6-7，大端 major version）。
// 用于 OptiFine 安装器入口类（optifine/Installer.class）反推其要求的最低 Java 大版本（JavaN = major - 44）。
// 返回 0 表示读取失败（提取失败 / 非 class 文件），调用方应回退到版本号推导。
static int ReadJarEntryClassMajor(const std::wstring& jarPath, const std::string& entry, const std::wstring& extractDir) {
    std::error_code ec;
    std::filesystem::remove_all(extractDir, ec);
    if (!ExtractZipEntry(jarPath, entry, extractDir)) return 0;
    std::wstring rel = lxe::Utf8ToWide(entry);
    std::replace(rel.begin(), rel.end(), L'/', L'\\');
    std::ifstream ifs(extractDir + L"\\" + rel, std::ios::binary);
    if (!ifs.is_open()) return 0;
    unsigned char hdr[8];
    ifs.read(reinterpret_cast<char*>(hdr), 8);
    if (ifs.gcount() < 8) return 0;
    if (hdr[0] != 0xCA || hdr[1] != 0xFE || hdr[2] != 0xBA || hdr[3] != 0xBE) return 0;
    return (hdr[6] << 8) | hdr[7];
}

// OptiFine 新旧版本判定（temp/OptiFine设计-净室版.md §3.1）：
// 版本号含未发布分支标记（无点号/非 1.x 形式）或游戏主版本号达到里程碑阈值（>=1.13）→ 新版本（安装器必需）；
// 1.x 且 <1.13 → 旧版本（OptiFine 以 launchwrapper tweak 方式工作，可纯手工构造）。
static bool IsOldOptifineMc(const std::string& mcVersion) {
    size_t p1 = mcVersion.find('.');
    if (p1 == std::string::npos) return false;
    if (mcVersion.compare(0, p1, "1") != 0) return false;
    size_t p2 = mcVersion.find('.', p1 + 1);
    std::string min = mcVersion.substr(p1 + 1, (p2 == std::string::npos) ? std::string::npos : (p2 - p1 - 1));
    int minor = 0;
    for (char c : min) { if (c < '0' || c > '9') return false; minor = minor * 10 + (c - '0'); }
    return minor < 13;
}

// 从安装器内嵌 maven/ 目录复制库文件到 {libDir}/{path}（新版安装器把启动库/universal 等内嵌，避免 403）
// zip 条目名用正斜杠（maven/net/...），落盘路径用反斜杠；extractRoot 用于临时解压复用目录
static bool CopyEmbeddedMavenLib(const std::wstring& installerJar, const std::string& path,
                                 const std::wstring& libDir, const std::wstring& extractRoot) {
    if (path.empty()) return false;
    std::wstring rel = lxe::Utf8ToWide(path);
    std::replace(rel.begin(), rel.end(), L'/', L'\\');
    std::wstring dst = libDir + L"\\" + rel;
    if (std::filesystem::exists(dst)) return true;
    std::error_code ec;
    std::filesystem::create_directories(extractRoot, ec);
    std::string entry = "maven/" + path;
    if (!ExtractZipEntry(installerJar, entry, extractRoot)) return false;
    std::wstring src = extractRoot + L"\\maven\\" + rel;
    if (!std::filesystem::exists(src)) return false;
    std::filesystem::create_directories(std::filesystem::path(dst).parent_path(), ec);
    std::filesystem::copy_file(src, dst, std::filesystem::copy_options::overwrite_existing, ec);
    return std::filesystem::exists(dst);
}

// 从 version.json 解析 natives 规则并提取 native 库
struct NativeLib {
    std::wstring path; // libraries/ 下的相对路径
    std::string classifier;
    std::wstring url;  // 分类器 jar 的下载地址（补下缺失 jar 用）
};
static std::vector<NativeLib> ExtractNatives(const Json& versionJson, const std::wstring& mcRoot) {
    std::vector<NativeLib> natives;
    if (!versionJson.contains("libraries") || !versionJson.at("libraries").isArray()) return natives;

    for (const auto& lib : versionJson.at("libraries").asArray()) {
        if (!lib.isObject()) continue;
        if (!IsLibAllowedOnWindows(lib)) continue;
        if (!lib.contains("downloads") || !lib.at("downloads").isObject()) continue;
        const auto& dl = lib.at("downloads");
        if (!dl.contains("classifiers") || !dl.at("classifiers").isObject()) continue;
        const auto& classifiers = dl.at("classifiers");

        // 找 natives 中的 windows 分类器
        std::string nativeKey;
        if (lib.contains("natives") && lib.at("natives").isObject()) {
            const auto& nat = lib.at("natives");
            if (nat.contains("windows") && nat.at("windows").isString()) {
                nativeKey = nat.at("windows").asString();
                // 替换 ${arch} 为 64
                size_t pos = nativeKey.find("${arch}");
                if (pos != std::string::npos) nativeKey.replace(pos, 6, "64");
            }
        }
        if (nativeKey.empty()) continue;

        if (classifiers.contains(nativeKey) && classifiers.at(nativeKey).isObject()) {
            const auto& clf = classifiers.at(nativeKey);
            if (clf.contains("path") && clf.at("path").isString()) {
                NativeLib nl;
                nl.path = lxe::Utf8ToWide(clf.at("path").asString());
                nl.classifier = nativeKey;
                if (clf.contains("url") && clf.at("url").isString())
                    nl.url = lxe::Utf8ToWide(clf.at("url").asString());
                natives.push_back(nl);
            }
        }
    }
    return natives;
}

// 补下缺失/0 字节 的 natives 分类器 jar（用 downloads.classifiers.<key>.url）。
// 解压 natives 前必须调用：否则 natives 目录会空，Java 找不到 lwjgl.dll 直接报错。
static void EnsureNativeJars(const std::vector<NativeLib>& natives, const std::wstring& mcRoot) {
    for (const auto& nl : natives) {
        std::wstring jarPath = mcRoot + L"\\libraries\\" + nl.path;
        std::error_code ec;
        if (std::filesystem::exists(jarPath, ec) && std::filesystem::file_size(jarPath, ec) > 0) continue;
        if (nl.url.empty()) continue;
        size_t slash = jarPath.find_last_of(L"\\");
        if (slash == std::wstring::npos) continue;
        std::wstring dir = jarPath.substr(0, slash);
        std::wstring fname = jarPath.substr(slash + 1);
        std::filesystem::create_directories(dir, ec);
        DownloadFileSmart(nl.url, dir, fname, nullptr);
    }
}

// 把 natives 库解压到 <versions>\<版本>-natives 目录
// 先清空已有文件再逐个解压，避免残留损坏的 0 字节 DLL 或旧版本残留导致 LWJGL 加载失败
static void ExtractNativesToDir(const std::vector<NativeLib>& natives, const std::wstring& mcRoot,
                                const std::wstring& nativesDir) {
    // 解压缓存签名：native jar 的 路径|大小|修改时间 拼接。签名一致 → 已解压过且未变化，直接跳过
    // （避免每次启动都清空并重解全部 native 库，重复启动可显著提速）
    std::string sig;
    for (const auto& nl : natives) {
        std::wstring nativeJar = mcRoot + L"\\libraries\\" + nl.path;
        std::error_code ecj, ecs;
        auto st = std::filesystem::status(nativeJar, ecs);
        if (ecs || !std::filesystem::is_regular_file(st)) continue;
        uintmax_t sz = std::filesystem::file_size(nativeJar, ecj);
        auto ft = std::filesystem::last_write_time(nativeJar, ecj);
        sig += lxe::WideToUtf8(nl.path) + "|" + std::to_string((long long)sz) + "|" +
               std::to_string(ft.time_since_epoch().count()) + ";";
    }
    std::wstring cacheFile = nativesDir + L"\\.lxe-natives-cache";
    if (!sig.empty()) {
        auto [old, ok] = ReadFileUtf8(cacheFile);
        if (ok && old == sig) return; // 缓存有效：natives 已按相同源解压过，跳过清空与解压
    }
    std::error_code ec;
    std::filesystem::create_directories(nativesDir, ec);
    // 清空旧文件：旧的损坏/0 字节 dll、不同版本的残留（如 3.2.1 与 3.2.2 混存）必须清掉
    for (const auto& entry : std::filesystem::directory_iterator(nativesDir, ec)) {
        std::error_code ec2;
        if (entry.is_regular_file(ec2)) std::filesystem::remove(entry.path(), ec2);
    }
    for (const auto& nl : natives) {
        std::wstring nativeJar = mcRoot + L"\\libraries\\" + nl.path;
        std::error_code ecj;
        // 跳过不存在的 jar；仅当 jar 非空才解压（0 字节 jar 解压不出任何东西，且会导致残留空文件）
        if (!std::filesystem::exists(nativeJar, ecj)) continue;
        if (std::filesystem::file_size(nativeJar, ecj) == 0) continue;
        // 解压 natives：用系统 tar（.jar 本质是 zip，Expand-Archive 会因扩展名拒绝 .jar）
        std::wstring cmd = L"cmd /c tar -xf \"" + nativeJar + L"\" -C \"" + nativesDir + L"\"";
        RunSilent(cmd);
    }
    // 冗余保险：解压后仍存在 0 字节的 dll 则移除（防 LWJGL 定位到空文件）
    for (const auto& entry : std::filesystem::directory_iterator(nativesDir, ec)) {
        std::error_code ec2;
        if (!entry.is_regular_file(ec2)) continue;
        if (entry.path().extension() == L".dll" && entry.file_size(ec2) == 0)
            std::filesystem::remove(entry.path(), ec2);
    }
    // 解压完成：写入缓存签名，下次启动同源时直接跳过
    if (!sig.empty()) {
        try {
            std::ofstream ofs(cacheFile, std::ios::binary | std::ios::trunc);
            if (ofs) ofs << sig;
        } catch (...) {}
    }
}

// 从 version.json 构建 classpath
// 无 downloads 的库（如 Forge 安装器产物 net.minecraftforge:forge:*，官方 json 的 downloads 为空）：
// 按 Maven 坐标 group:artifact:version[:classifier] 推导 libraries 路径，文件存在才返回
static std::wstring LibPathFromName(const std::wstring& mcRoot, const std::string& name) {
    auto c1 = name.find(':');
    if (c1 == std::string::npos) return L"";
    auto c2 = name.find(':', c1 + 1);
    if (c2 == std::string::npos) return L"";
    std::string g = name.substr(0, c1);
    std::string a = name.substr(c1 + 1, c2 - c1 - 1);
    // 第 4 段（classifier）可选：三段的 group:artifact:version（如 optifine:launchwrapper-of:2.1）也能正确推导
    std::string v, cls;
    auto c3 = name.find(':', c2 + 1);
    if (c3 == std::string::npos) {
        v = name.substr(c2 + 1);
    } else {
        v = name.substr(c2 + 1, c3 - c2 - 1);
        cls = name.substr(c3 + 1);
    }
    std::string gp;
    for (char ch : g) gp += (ch == '.') ? '/' : ch;
    std::string file = a + "-" + v + (cls.empty() ? "" : ("-" + cls)) + ".jar";
    std::wstring full = mcRoot + L"\\libraries\\" + lxe::Utf8ToWide(gp + "/" + a + "/" + v + "/" + file);
    return std::filesystem::exists(full) ? full : L"";
}

// 下载 authlib-injector.jar：官方下载 API 为 GET /artifact/latest.json（返回 JSON，含 download_url 与版本号），
// BMCLAPI 镜像入口为 mirrors/authlib-injector/（注意末尾斜杠，镜像根路径只返回 HTML 页面）。
// 旧代码直接 GET /artifact/latest（404）或镜像根（HTML）→ 下载到非 jar 文件 → JVM 报找不到/损坏。
// 正确流程：先取 latest.json 元数据，再按其 download_url 下载真实 jar；BMCLAPI → 官方 → GitHub 三级兜底。
static bool DownloadAuthlibInjector(const std::wstring& outDir, const std::wstring& outName,
                                    std::function<bool(const Aria2Progress&)> progressCb = nullptr) {
    auto downloadFromMeta = [&](const std::wstring& metaUrl) -> bool {
        std::string text = HttpFetchText(metaUrl);
        if (text.empty()) return false;
        Json meta;
        try { meta = Json::parse(text); } catch (...) { return false; }
        if (!meta.isObject()) return false;
        if (!meta.contains("download_url") || !meta.at("download_url").isString()) return false;
        std::string url = meta.at("download_url").asString();
        if (url.empty()) return false;
        return DownloadFileSmart(lxe::Utf8ToWide(url), outDir, outName, progressCb);
    };
    if (downloadFromMeta(L"https://bmclapi2.bangbang93.com/mirrors/authlib-injector/artifact/latest.json")) return true;
    if (downloadFromMeta(L"https://authlib-injector.yushi.moe/artifact/latest.json")) return true;
    // 最后兜底：GitHub Releases API，取最新 release 的 jar asset
    try {
        std::string gh = HttpFetchText(L"https://api.github.com/repos/yushijinhun/authlib-injector/releases/latest");
        Json g = Json::parse(gh);
        if (g.isObject() && g.contains("assets") && g.at("assets").isArray()) {
            for (const auto& a : g.at("assets").asArray()) {
                if (!a.isObject()) continue;
                if (a.contains("browser_download_url") && a.at("browser_download_url").isString()) {
                    std::string u = a.at("browser_download_url").asString();
                    if (u.find(".jar") != std::string::npos &&
                        DownloadFileSmart(lxe::Utf8ToWide(u), outDir, outName, progressCb))
                        return true;
                }
            }
        }
    } catch (...) {}
    return false;
}

// Maven 版本号比较（逐数字段比较，如 2.8.1 < 2.15.0；支持 1.16.5-36.2.55 这类带后缀的加载器版本）
// 返回：a>b → 1；a<b → -1；相等 → 0
static int MavenVersionCmp(const std::string& a, const std::string& b) {
    auto seg = [](const std::string& s, std::vector<std::string>& out) {
        std::string cur;
        for (char c : s) {
            if (c == '.' || c == '-') { if (!cur.empty()) { out.push_back(cur); cur.clear(); } }
            else cur += c;
        }
        if (!cur.empty()) out.push_back(cur);
    };
    std::vector<std::string> sa, sb;
    seg(a, sa); seg(b, sb);
    size_t n = (std::max)(sa.size(), sb.size());
    for (size_t i = 0; i < n; ++i) {
        std::string x = i < sa.size() ? sa[i] : "0";
        std::string y = i < sb.size() ? sb[i] : "0";
        // 纯数字段按数值比较，否则按字符串
        bool xn = !x.empty() && std::all_of(x.begin(), x.end(), ::isdigit);
        bool yn = !y.empty() && std::all_of(y.begin(), y.end(), ::isdigit);
        if (xn && yn) {
            long long vx = _strtoi64(x.c_str(), nullptr, 10);
            long long vy = _strtoi64(y.c_str(), nullptr, 10);
            if (vx != vy) return vx > vy ? 1 : -1;
        } else {
            int c = x.compare(y);
            if (c != 0) return c > 0 ? 1 : -1;
        }
    }
    return 0;
}

static std::wstring BuildClasspath(const Json& versionJson, const std::wstring& mcRoot, const std::wstring& versionId) {
    // 收集 classpath 项（路径 + Maven 坐标 group:artifact + 版本），最后按坐标去重：
    // 原版 json 与加载器（Forge/OptiFine/Fabric）合并后，同一坐标可能出现多个版本
    // （如 Forge 1.16.5 同时带 log4j 2.8.1 与 2.15.0）——若不去重，classpath 里新旧两版并存，
    // JVM 按顺序先加载旧版，加载器代码调用新版才有的方法 → NoSuchMethodError。
    struct CpItem { std::wstring path; std::string coord; std::string version; };
    std::vector<CpItem> items;
    auto addItem = [&items](std::wstring path, std::string coord, std::string version) {
        // 精确路径去重（同一文件只出现一次）
        for (auto& it : items) {
            if (_wcsicmp(it.path.c_str(), path.c_str()) == 0) return;
        }
        items.push_back({ std::move(path), std::move(coord), std::move(version) });
    };
    // 从 Maven 坐标 name（group:artifact:version[:classifier]）提取 group/artifact/version
    auto coordParts = [](const std::string& name, std::string& g, std::string& a, std::string& v, std::string& cls) {
        auto c1 = name.find(':');
        if (c1 == std::string::npos) return;
        auto c2 = name.find(':', c1 + 1);
        if (c2 == std::string::npos) return;
        g = name.substr(0, c1);
        a = name.substr(c1 + 1, c2 - c1 - 1);
        auto c3 = name.find(':', c2 + 1);
        if (c3 == std::string::npos) v = name.substr(c2 + 1);
        else { v = name.substr(c2 + 1, c3 - c2 - 1); cls = name.substr(c3 + 1); }
    };
    // 1) libraries
    if (versionJson.contains("libraries") && versionJson.at("libraries").isArray()) {
        for (const auto& lib : versionJson.at("libraries").asArray()) {
            if (!lib.isObject()) continue;
            if (!IsLibAllowedOnWindows(lib)) continue;
            bool dlObj = lib.contains("downloads") && lib.at("downloads").isObject();
            bool added = false;
            std::string coordG, coordA, coordV, coordCls;
            if (dlObj) {
                const auto& dl = lib.at("downloads");
                // 只取 artifact（不含 downloads.classifiers/natives）
                if (dl.contains("artifact") && dl.at("artifact").isObject()) {
                    const auto& art = dl.at("artifact");
                    if (art.contains("path") && art.at("path").isString()) {
                        std::wstring p = mcRoot + L"\\libraries\\" + lxe::Utf8ToWide(art.at("path").asString());
                        // 优先从 Maven name（group:artifact:version[:classifier]）取坐标：
                        // 某些版本 json（如 26.2）把 lwjgl 的 natives 也写成独立 library 且带
                        // downloads.artifact。若从文件名反推版本，natives 文件名（如
                        // lwjgl-glfw-3.4.1-natives-windows.jar）最后一个 '-' 之后是
                        // "windows" 会被误当成版本号，与主 jar 撞同坐标、去重时把主 jar 顶掉
                        // → 主库类找不到（GLFWErrorCallbackI 等）。classifier 纳入去重键，
                        // 保证 natives 与主 jar 视为不同库。
                        if (lib.contains("name") && lib.at("name").isString()) {
                            coordParts(lib.at("name").asString(), coordG, coordA, coordV, coordCls);
                        }
                        // 无 name 时从 artifact path 反推（org/.../<artifact>/<version>/<file>.jar）
                        if (coordA.empty()) {
                            std::string rel = art.at("path").asString();
                            std::vector<std::string> segs;
                            std::string cur;
                            for (char c : rel) { if (c == '/') { if (!cur.empty()) { segs.push_back(cur); cur.clear(); } } else cur += c; }
                            if (!cur.empty()) segs.push_back(cur);
                            if (segs.size() >= 4) {
                                std::string file = segs.back();
                                coordA = segs[segs.size() - 3];
                                coordV = segs[segs.size() - 2];   // 版本目录最可靠
                                for (size_t i = 0; i + 3 < segs.size(); ++i) {
                                    if (!coordG.empty()) coordG += ".";
                                    coordG += segs[i];
                                }
                                // 文件名 <artifact>-<version>[-<classifier>].jar → 提取 classifier
                                std::string prefix = coordA + "-" + coordV + "-";
                                if (file.size() > prefix.size() && file.compare(0, prefix.size(), prefix) == 0) {
                                    coordCls = file.substr(prefix.size(), file.size() - prefix.size() - 4);
                                }
                            }
                        }
                        std::string key = coordG + ":" + coordA;
                        if (!coordCls.empty()) key += ":" + coordCls;
                        addItem(std::move(p), key, coordV);
                        added = true;
                    }
                }
            }
            // 无 downloads 或仅含 classifiers 的库（Forge/OptiFine/Fabric 等第三方安装器产物）：
            // 按 Maven 坐标 group:artifact:version[:classifier] 推导路径，文件存在才加入 classpath
            if (!added && lib.contains("name") && lib.at("name").isString()) {
                std::wstring lp = LibPathFromName(mcRoot, lib.at("name").asString());
                if (!lp.empty()) {
                    std::string g, a, v, cls;
                    coordParts(lib.at("name").asString(), g, a, v, cls);
                    addItem(std::move(lp), g + ":" + a, v);
                }
            }
        }
    }
    // 2) 版本 jar
    items.push_back({ mcRoot + L"\\versions\\" + versionId + L"\\" + versionId + L".jar", "", "" });
    // 3) 按 Maven 坐标去重：同一 group:artifact 只保留最高版本（classifier 不同视为不同库，不参与合并）
    {
        // coord -> 保留项下标；若同 coord 出现更高版本则替换
        std::map<std::string, size_t> best;
        std::vector<CpItem> deduped;
        for (size_t i = 0; i < items.size(); ++i) {
            const auto& it = items[i];
            if (it.coord.empty() || it.version.empty()) { deduped.push_back(it); continue; }
            auto found = best.find(it.coord);
            if (found == best.end()) {
                best[it.coord] = deduped.size();
                deduped.push_back(it);
            } else {
                CpItem& cur = deduped[found->second];
                if (MavenVersionCmp(it.version, cur.version) > 0) {
                    cur.path = it.path; cur.version = it.version; // 新版替换旧版（保持原位置）
                }
            }
        }
        items.swap(deduped);
    }
    std::wstring cp;
    for (const auto& it : items) {
        if (!cp.empty()) cp += L";";
        cp += it.path;
    }
    return cp;
}

// 解析 inheritsFrom 链，合并父版本的 libraries / arguments
static Json ResolveVersionJson(const std::wstring& mcRoot, const std::wstring& versionId) {
    std::wstring jsonPath = mcRoot + L"\\versions\\" + versionId + L"\\" + versionId + L".json";
    auto [text, ok] = ReadFileUtf8(jsonPath);
    if (!ok) return Json::object();
    Json vj;
    try { vj = Json::parse(text); } catch (...) { return Json::object(); }
    if (!vj.isObject()) return Json::object();

    // 处理 inheritsFrom
    if (vj.contains("inheritsFrom") && vj.at("inheritsFrom").isString()) {
        std::string parentId = vj.at("inheritsFrom").asString();
        Json parent = ResolveVersionJson(mcRoot, lxe::Utf8ToWide(parentId));
        // 合并 libraries（父+子）
        if (parent.contains("libraries") && parent.at("libraries").isArray()) {
            if (!vj.contains("libraries")) vj["libraries"] = Json::array();
            if (vj.at("libraries").isArray()) {
                // 先放父库，再放子库
                Json merged = Json::array();
                for (const auto& l : parent.at("libraries").asArray()) merged.asArray().push_back(l);
                for (const auto& l : vj.at("libraries").asArray()) merged.asArray().push_back(l);
                vj["libraries"] = merged;
            }
        }
        // 合并 mainClass（子优先）
        if (!vj.contains("mainClass") && parent.contains("mainClass"))
            vj["mainClass"] = parent.at("mainClass");
        // 合并 assetIndex
        if (!vj.contains("assetIndex") && parent.contains("assetIndex"))
            vj["assetIndex"] = parent.at("assetIndex");
        // 合并 downloads（子优先，但父的 client jar 是 fallback）
        if (!vj.contains("downloads") && parent.contains("downloads"))
            vj["downloads"] = parent.at("downloads");
        // 合并 arguments / minecraftArguments
        if (!vj.contains("arguments") && parent.contains("arguments"))
            vj["arguments"] = parent.at("arguments");
        if (!vj.contains("minecraftArguments") && parent.contains("minecraftArguments"))
            vj["minecraftArguments"] = parent.at("minecraftArguments");
        // 合并 logging
        if (!vj.contains("logging") && parent.contains("logging"))
            vj["logging"] = parent.at("logging");
        // 合并 type
        if (!vj.contains("type") && parent.contains("type"))
            vj["type"] = parent.at("type");
        // 合并 javaVersion（子优先，父的 javaVersion 是 fallback——加载器子 json 常不写、
        // 父原版 json 才有 majorVersion，Java 匹配需读到合并后的值）
        if (!vj.contains("javaVersion") && parent.contains("javaVersion"))
            vj["javaVersion"] = parent.at("javaVersion");
    }
    return vj;
}

// ===================== CompleteVersionFilesWorker：后端并发补全版本全部文件 =====================
// 供 mc.completeVersion 与 mc.installLoader(complete=true) 复用：下载 客户端JAR/依赖库/natives分类器/全部资源对象，
// 多线程并发（进度事件带 files[]/threads[] 详情），收尾解压 natives；失败原子回退删除半成品。
// 内部自行发送 download.progress 与 download.state 事件（dev 同一 taskId）。
static bool CompleteVersionFilesWorker(Bridge& bridge, int taskId, const std::string& version, const std::string& displayName) {
    auto postState = [&](const std::string& state) {
        Json ev = Json::object();
        ev["taskId"] = std::to_string(taskId);
        ev["state"] = state;
        ev["name"] = displayName;
        bridge.PostEvent("download.state", ev);
    };
    const std::string taskIdStr = std::to_string(taskId);
    const std::string taskName = displayName;
    auto cancelFlag = DLCancelFlag(taskIdStr);
    try {
    postState("started");
    std::wstring mcRoot = GetMcRoot();
    std::wstring wVer = lxe::Utf8ToWide(version);
    Json vj = ResolveVersionJson(mcRoot, wVer);
    if (!vj.isObject() || vj.size() == 0) { DLCancelFlagRemove(taskIdStr); postState("error"); return false; }
    std::wstring verDir = mcRoot + L"\\versions\\" + wVer;

    // 日志配置 + 资源索引：小文件，直接补下（资源索引随后用于枚举 objects）
    EnsureLoggingConfig(vj, mcRoot);
    std::wstring idxPath = EnsureAssetIndex(vj, mcRoot);

    // ============ 构造完整并发下载清单（客户端JAR / 依赖库 / natives分类器 / 全部资源对象）============
    struct CVItem { std::string url; std::string label; std::wstring outDir; std::wstring outName; long long size = 0; };
    std::vector<CVItem> items;
    auto addMissing = [&](const std::wstring& fullPath, const std::string& url, const std::string& label) {
        std::error_code e1;
        if (std::filesystem::exists(fullPath, e1)) return;
        if (url.empty()) return;
        size_t slash = fullPath.find_last_of(L"\\");
        if (slash == std::wstring::npos) return;
        CVItem it;
        it.url = url; it.label = label;
        it.outDir = fullPath.substr(0, slash);
        it.outName = fullPath.substr(slash + 1);
        items.push_back(std::move(it));
    };

    // 1) 客户端 JAR
    if (vj.contains("downloads") && vj.at("downloads").isObject()) {
        const auto& dl = vj.at("downloads");
        if (dl.contains("client") && dl.at("client").isObject()) {
            const auto& c = dl.at("client");
            std::string url = c.contains("url") && c.at("url").isString() ? c.at("url").asString() : "";
            addMissing(verDir + L"\\" + wVer + L".jar", url, "客户端 JAR · " + version);
        }
    }

    // 2) 依赖库 artifact + natives 分类器（缺失/0字节 一律补下）
    if (vj.contains("libraries") && vj.at("libraries").isArray()) {
        for (const auto& lib : vj.at("libraries").asArray()) {
            if (!lib.isObject()) continue;
            if (!IsLibAllowedOnWindows(lib)) continue;
            bool handledArtifact = false;
            if (lib.contains("downloads") && lib.at("downloads").isObject()) {
                const auto& dl = lib.at("downloads");
                if (dl.contains("artifact") && dl.at("artifact").isObject()) {
                    const auto& art = dl.at("artifact");
                    if (art.contains("path") && art.at("path").isString()) {
                        std::wstring libFile = mcRoot + L"\\libraries\\" + lxe::Utf8ToWide(art.at("path").asString());
                        std::string url = art.contains("url") && art.at("url").isString() ? art.at("url").asString() : "";
                        // url 非空才算正规覆盖（addMissing 对空 url 静默跳过，缺失文件留给兜底按 name 补下）
                        if (!url.empty()) handledArtifact = true;
                        std::string fname = lxe::WideToUtf8(libFile.substr(libFile.find_last_of(L"\\") + 1));
                        addMissing(libFile, url, "依赖库 · " + fname);
                    }
                }
                if (dl.contains("classifiers") && dl.at("classifiers").isObject()) {
                    const auto& clf = dl.at("classifiers");
                    std::string nativeKey;
                    if (lib.contains("natives") && lib.at("natives").isObject()) {
                        const auto& nat = lib.at("natives");
                        if (nat.contains("windows") && nat.at("windows").isString()) {
                            nativeKey = nat.at("windows").asString();
                            size_t p = nativeKey.find("${arch}");
                            if (p != std::string::npos) nativeKey.replace(p, 6, "64");
                        }
                    }
                    if (!nativeKey.empty() && clf.contains(nativeKey) && clf.at(nativeKey).isObject()) {
                        const auto& n = clf.at(nativeKey);
                        if (n.contains("path") && n.at("path").isString()) {
                            std::wstring nJar = mcRoot + L"\\libraries\\" + lxe::Utf8ToWide(n.at("path").asString());
                            std::string url = n.contains("url") && n.at("url").isString() ? n.at("url").asString() : "";
                            std::error_code e2;
                            bool missing = !std::filesystem::exists(nJar, e2) || std::filesystem::file_size(nJar, e2) == 0;
                            if (missing && !url.empty()) {
                                std::string fname = lxe::WideToUtf8(nJar.substr(nJar.find_last_of(L"\\") + 1));
                                addMissing(nJar, url, "Natives · " + fname);
                            }
                        }
                    }
                }
            }
            // 安装器产物兜底：Forge/Fabric/OptiFine 合并 version.json 中部分库无 downloads
            // 或 url 为空（如 launchwrapper / forge 各运行库、Fabric loader 库），缺失时按
            // maven 坐标推导路径并从官方 maven 补下——否则 classpath 缺库启动即 NoClassDefFoundError。
            // 仅处理无 classifier 的普通 artifact（3 段坐标）；classifier（含 natives）已由上方 downloads 覆盖。
            if (!handledArtifact && lib.contains("name") && lib.at("name").isString()) {
                std::string coord = lib.at("name").asString();
                int segs = 0;
                {
                    std::istringstream iss(coord);
                    // group:artifact:version[:classifier]——@ 后缀（如 @lzma）不算额外段，从末尾剥离
                    std::string base = coord;
                    if (base.find('@') != std::string::npos) base = base.substr(0, base.find('@'));
                    for (char c : base) if (c == ':') ++segs;
                    segs += 1;
                }
                if (segs >= 3 && segs <= 3) {
                    std::string rel = MavenCoordToPath(coord);
                    if (!rel.empty()) {
                        std::wstring wrel = lxe::Utf8ToWide(rel);
                        std::replace(wrel.begin(), wrel.end(), L'/', L'\\');
                        std::wstring full = mcRoot + L"\\libraries\\" + wrel;
                        std::error_code e4;
                        bool missing = !std::filesystem::exists(full, e4) || std::filesystem::file_size(full, e4) == 0;
                        if (missing) {
                            size_t slash = full.find_last_of(L"\\");
                            if (slash != std::wstring::npos) {
                                CVItem it;
                                it.url = "https://files.minecraftforge.net/maven/" + rel;
                                it.label = "安装器库 · " + rel;
                                it.outDir = full.substr(0, slash);
                                it.outName = full.substr(slash + 1);
                                items.push_back(std::move(it));
                            }
                        }
                    }
                }
            }
        }
    }

    // 3) 全部资源对象（可能上千个 → 并发下载）
    if (!idxPath.empty()) {
        auto [text, ok] = ReadFileUtf8(idxPath);
        if (ok && !text.empty()) {
            Json idx;
            try { idx = Json::parse(text); } catch (...) { idx = Json(); }
            if (idx.isObject() && idx.contains("objects") && idx.at("objects").isObject()) {
                for (const auto& kv : idx.at("objects").asObject()) {
                    if (!kv.second.isObject()) continue;
                    const auto& o = kv.second;
                    if (!o.contains("hash") || !o.at("hash").isString()) continue;
                    std::string hash = o.at("hash").asString();
                    if (hash.size() < 2) continue;
                    std::wstring objPath = mcRoot + L"\\assets\\objects\\" + lxe::Utf8ToWide(hash.substr(0, 2)) + L"\\" + lxe::Utf8ToWide(hash);
                    std::error_code e3;
                    if (std::filesystem::exists(objPath, e3)) continue;
                    CVItem it;
                    it.url = "https://resources.download.minecraft.net/" + hash.substr(0, 2) + "/" + hash;
                    it.outDir = std::filesystem::path(objPath).parent_path().wstring();
                    it.outName = lxe::Utf8ToWide(hash);
                    if (o.contains("size") && o.at("size").isNumber()) it.size = (long long)o.at("size").asNumber();
                    it.label = "资源 · " + kv.first;
                    items.push_back(std::move(it));
                }
            }
        }
    }

    int totalItems = (int)items.size();
    if (totalItems == 0) {
        // 无缺失：直接解压 natives 收尾
        auto natives0 = ExtractNatives(vj, mcRoot);
        ExtractNativesToDir(natives0, mcRoot, verDir + L"\\" + wVer + L"-natives");
        Json done0 = Json::object();
        done0["taskId"] = taskIdStr;
        done0["percent"] = 100;
        done0["stage"] = "补全完成（无缺失文件）";
        done0["name"] = taskName;
        bridge.PostEvent("download.progress", done0);
        DLCancelFlagRemove(taskIdStr);
        postState("done");
        InvalidateLocalVersionsCache();
        return true;
    }

    // ============ 并发下载（进度 schema 与 mc.submitDownloadList 一致：files[]/threads[] 详情）============
    struct CVThread { std::string label; int pct = 0; std::string speed; std::string eta; int connections = 0; };
    int maxConcurrent = 8;
    auto progressMutex = std::make_shared<std::mutex>();
    auto filesState = std::make_shared<std::vector<Json>>(totalItems);
    {
        std::lock_guard<std::mutex> lock(*progressMutex);
        for (int i = 0; i < totalItems; ++i) {
            Json fs = Json::object();
            fs["name"] = items[i].label.empty() ? lxe::WideToUtf8(items[i].outName) : items[i].label;
            fs["state"] = "pending";
            (*filesState)[i] = fs;
        }
    }
    auto threadInfo = std::make_shared<std::vector<CVThread>>(maxConcurrent);
    auto evSeq = std::make_shared<std::atomic<int>>(0);
    auto postProgress = [&](int pct, const std::string& stage, const std::string& speed, const std::string& eta, const std::vector<CVThread>* threads = nullptr, int doneFiles = -1, bool forceFiles = false) {
        Json prog = Json::object();
        prog["taskId"] = taskIdStr;
        prog["percent"] = pct;
        prog["stage"] = stage;
        prog["name"] = taskName;
        prog["speed"] = speed;
        prog["eta"] = eta;
        prog["totalFiles"] = totalItems;
        prog["doneFiles"] = doneFiles < 0 ? 0 : doneFiles;
        prog["remainingFiles"] = totalItems - (doneFiles < 0 ? 0 : doneFiles);
        {
            std::lock_guard<std::mutex> lock(*progressMutex);
            // 文件列表可能上千项：仅每 ~10 个进度事件或收尾/失败时附带，避免刷爆前端消息
            bool includeFiles = forceFiles || ((int)(++(*evSeq)) % 10 == 0);
            if (includeFiles) {
                Json fArr = Json::array();
                for (const auto& fs : *filesState) fArr.asArray().push_back(fs);
                prog["files"] = fArr;
            }
            if (threads) {
                Json tArr = Json::array();
                for (const auto& t : *threads) {
                    Json to = Json::object();
                    to["label"] = t.label;
                    to["pct"] = t.pct;
                    to["speed"] = t.speed;
                    to["eta"] = t.eta;
                    to["connections"] = t.connections;
                    tArr.asArray().push_back(to);
                }
                prog["threads"] = tArr;
            }
        }
        bridge.PostEvent("download.progress", prog);
    };

    auto completedCount = std::make_shared<std::atomic<int>>(0);
    auto errorFlag = std::make_shared<std::atomic<bool>>(false);
    auto worker = [&, completedCount, errorFlag, progressMutex, threadInfo, filesState, cancelFlag](int idx, int slot) {
        try {
            const auto& it = items[idx];
            const std::string label = it.label.empty() ? lxe::WideToUtf8(it.outName) : it.label;
            const std::string stage = "(" + std::to_string(idx + 1) + "/" + std::to_string(totalItems) + ") " + label;
            {
                std::lock_guard<std::mutex> lock(*progressMutex);
                (*threadInfo)[slot].label = label;
                (*threadInfo)[slot].pct = 0;
                (*threadInfo)[slot].speed.clear();
                (*threadInfo)[slot].eta.clear();
                (*threadInfo)[slot].connections = 0;
                if ((*filesState)[idx].contains("state")) (*filesState)[idx]["state"] = "downloading";
            }
            auto cb = [&](const Aria2Progress& p) -> bool {
                if (errorFlag->load()) return false;
                if (cancelFlag->load()) return false;
                int overallPct = 0;
                {
                    std::lock_guard<std::mutex> lock(*progressMutex);
                    (*threadInfo)[slot].pct = p.percent;
                    (*threadInfo)[slot].speed = p.speed;
                    (*threadInfo)[slot].eta = p.eta;
                    (*threadInfo)[slot].connections = p.connections;
                    int done = completedCount->load();
                    overallPct = (int)(100.0 * (done + p.percent / 100.0) / totalItems);
                    if (overallPct < 0) overallPct = 0;
                    if (overallPct > 100) overallPct = 100;
                }
                std::ostringstream ss; ss << p.speed;
                postProgress(overallPct, stage, ss.str(), p.eta, threadInfo.get(), completedCount->load());
                return true;
            };
            std::error_code ec;
            std::filesystem::create_directories(it.outDir, ec);
            if (DownloadFileSmart(lxe::Utf8ToWide(it.url), it.outDir, it.outName, cb)) {
                int newDone = ++(*completedCount);
                int overallPct = (int)(100.0 * newDone / totalItems);
                if (overallPct > 100) overallPct = 100;
                {
                    std::lock_guard<std::mutex> lock(*progressMutex);
                    (*threadInfo)[slot].pct = 100;
                    (*threadInfo)[slot].label.clear();
                    if ((*filesState)[idx].contains("state")) (*filesState)[idx]["state"] = "done";
                }
                postProgress(overallPct, stage + " ✓", "", "", threadInfo.get(), newDone, true);
            } else {
                errorFlag->store(true);
                {
                    std::lock_guard<std::mutex> lock(*progressMutex);
                    (*threadInfo)[slot].pct = -1;
                    if ((*filesState)[idx].contains("state")) (*filesState)[idx]["state"] = cancelFlag->load() ? "cancelled" : "error";
                }
                postProgress((int)(100.0 * completedCount->load() / totalItems), stage + (cancelFlag->load() ? " ✕" : " ✗"), "", "", threadInfo.get(), completedCount->load(), true);
            }
        } catch (const std::exception& e) {
            errorFlag->store(true);
            {
                std::lock_guard<std::mutex> lock(*progressMutex);
                (*threadInfo)[slot].pct = -1;
                if ((*filesState)[idx].contains("state")) (*filesState)[idx]["state"] = cancelFlag->load() ? "cancelled" : "error";
            }
            postProgress((int)(100.0 * completedCount->load() / totalItems), "下载异常：" + std::string(e.what()), "", "", threadInfo.get(), completedCount->load(), true);
        } catch (...) {
            errorFlag->store(true);
            {
                std::lock_guard<std::mutex> lock(*progressMutex);
                (*threadInfo)[slot].pct = -1;
                if ((*filesState)[idx].contains("state")) (*filesState)[idx]["state"] = cancelFlag->load() ? "cancelled" : "error";
            }
            postProgress((int)(100.0 * completedCount->load() / totalItems), "下载异常", "", "", threadInfo.get(), completedCount->load(), true);
        }
    };
    auto nextIndex = std::make_shared<std::atomic<int>>(0);
    auto threadFunc = [worker, nextIndex, totalItems, errorFlag, threadInfo, progressMutex, cancelFlag](int slot) {
        while (!errorFlag->load() && !cancelFlag->load()) {
            int idx = nextIndex->fetch_add(1);
            if (idx >= totalItems) break;
            worker(idx, slot);
        }
        std::lock_guard<std::mutex> lock(*progressMutex);
        (*threadInfo)[slot].label.clear();
    };
    std::vector<std::thread> workers;
    workers.reserve(maxConcurrent);
    for (int i = 0; i < maxConcurrent; ++i) workers.emplace_back(threadFunc, i);
    for (auto& t : workers) if (t.joinable()) t.join();

    if (errorFlag->load() || cancelFlag->load()) {
        // 原子回退：删除未完成文件（含 .aria2 控制文件）
        {
            std::lock_guard<std::mutex> lock(*progressMutex);
            for (int i = 0; i < totalItems; ++i) {
                std::string st = (*filesState)[i].contains("state") && (*filesState)[i].at("state").isString()
                    ? (*filesState)[i].at("state").asString() : "pending";
                if (st == "done") continue;
                const auto& it = items[i];
                std::wstring absPath = it.outDir + L"\\" + it.outName;
                std::error_code ec;
                std::filesystem::remove(absPath, ec);
                std::filesystem::remove(absPath + L".aria2", ec);
            }
        }
        DLCancelFlagRemove(taskIdStr);
        if (cancelFlag->load()) {
            postState("cancelled");
        } else {
            postState("error");
        }
        return false;
    }

    // 收尾：解压 natives
    auto natives = ExtractNatives(vj, mcRoot);
    std::wstring nativesDir = verDir + L"\\" + wVer + L"-natives";
    EnsureNativeJars(natives, mcRoot);
    ExtractNativesToDir(natives, mcRoot, nativesDir);

    Json fin = Json::object();
    fin["taskId"] = taskIdStr;
    fin["percent"] = 100;
    fin["stage"] = "补全完成";
    fin["name"] = taskName;
    bridge.PostEvent("download.progress", fin);
    DLCancelFlagRemove(taskIdStr);
    postState("done");
    InvalidateLocalVersionsCache();
    return true;
    } catch (const std::exception& e) {
        DLCancelFlagRemove(taskIdStr);
        Json errEv = Json::object();
        errEv["taskId"] = taskIdStr;
        errEv["percent"] = 0;
        errEv["stage"] = "补全异常：" + std::string(e.what());
        errEv["name"] = taskName;
        bridge.PostEvent("download.progress", errEv);
        postState("error");
        return false;
    } catch (...) {
        DLCancelFlagRemove(taskIdStr);
        postState("error");
        return false;
    }
}

// 离线账号确定性 UUID（HMCL/PCL 同款：MD5("OfflinePlayer:<name>") 版本3/变体，与 Java UUID.nameUUIDFromBytes 一致）
static std::string OfflinePlayerUuid(const std::string& name) {
    std::string src = "OfflinePlayer:" + name;
    HCRYPTPROV prov = 0; HCRYPTHASH hash = 0;
    BYTE d[16] = { 0 }; DWORD len = 16;
    if (CryptAcquireContextW(&prov, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
        if (CryptCreateHash(prov, CALG_MD5, 0, 0, &hash)) {
            CryptHashData(hash, (const BYTE*)src.data(), (DWORD)src.size(), 0);
            CryptGetHashParam(hash, HP_HASHVAL, d, &len, 0);
            CryptDestroyHash(hash);
        }
        CryptReleaseContext(prov, 0);
    }
    d[6] = (d[6] & 0x0f) | 0x30; // version 3
    d[8] = (d[8] & 0x3f) | 0x80; // IETF variant
    char hex[40];
    snprintf(hex, sizeof(hex), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7], d[8], d[9], d[10], d[11], d[12], d[13], d[14], d[15]);
    return std::string(hex);
}

// 文件 SHA-1 哈希（小写十六进制 40 位）。Req4：本地 mod 反查 Modrinth 用。
// §29 教训：全程在调用线程读取（可能几十 MB 大文件），由调用方决定放后台线程。
static std::string FileSha1Hex(const std::wstring& path) {
    HCRYPTPROV prov = 0; HCRYPTHASH hash = 0;
    if (!CryptAcquireContextW(&prov, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) return {};
    std::string out;
    do {
        if (!CryptCreateHash(prov, CALG_SHA1, 0, 0, &hash)) break;
        std::ifstream f(path, std::ios::binary);
        if (!f.is_open()) break;
        char buf[65536];
        while (f) {
            f.read(buf, sizeof(buf));
            std::streamsize got = f.gcount();
            if (got > 0) CryptHashData(hash, (const BYTE*)buf, (DWORD)got, 0);
        }
        BYTE d[20] = { 0 }; DWORD len = 20;
        if (!CryptGetHashParam(hash, HP_HASHVAL, d, &len, 0)) break;
        char hex[41];
        snprintf(hex, sizeof(hex), "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
                 d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7], d[8], d[9], d[10], d[11], d[12], d[13], d[14], d[15],
                 d[16], d[17], d[18], d[19]);
        out = std::string(hex);
    } while (0);
    if (hash) CryptDestroyHash(hash);
    CryptReleaseContext(prov, 0);
    return out;
}

// ============ 启动游戏 ============
void RegisterMinecraftLaunch(Bridge& bridge) {
    g_bridgeForJavaScan = &bridge; // 后台 Java 扫描据此推送 java.found 增量事件
    bridge.RegisterAsync("mc.launch", [&bridge](const Json& params, const std::function<void(HandlerResult)>& done) {
        // 后台线程执行启动（含可能的补下/解压），避免阻塞 WebView2 桥消息线程导致启动器无响应；
        // params 按值捕获（深拷贝），bridge 引用程序生命周期，线程内使用安全。
        std::thread([&bridge, params, done]() {
            auto run = [&bridge, params]() -> HandlerResult {
        // 参数: { version, username, uuid, accessToken, accountType, memMB, width, height, customResolution }
        std::string version;
        if (params.isObject() && params.contains("version") && params.at("version").isString())
            version = params.at("version").asString();
        if (version.empty()) return Err(-32602, "missing version");

        // 账号类型（禁止无账号启动）：offline | thirdparty | premium
        std::string accountType;
        if (params.isObject() && params.contains("accountType") && params.at("accountType").isString())
            accountType = params.at("accountType").asString();
        if (accountType != "offline" && accountType != "thirdparty" && accountType != "premium")
            return Err(-32060, "未选择账号，请先在 设置-账号管理 中添加并选择账号");

        std::string username = "Player";
        if (params.isObject() && params.contains("username") && params.at("username").isString())
            username = params.at("username").asString();

        std::string uuid = "00000000-0000-0000-0000-000000000000";
        if (params.isObject() && params.contains("uuid") && params.at("uuid").isString())
            uuid = params.at("uuid").asString();

        std::string accessToken = "0";
        if (params.isObject() && params.contains("accessToken") && params.at("accessToken").isString())
            accessToken = params.at("accessToken").asString();

        // 第三方认证（外置登录）参数
        std::string userType = "mojang";
        if (params.isObject() && params.contains("userType") && params.at("userType").isString())
            userType = params.at("userType").asString();
        std::string authServer;
        if (params.isObject() && params.contains("authServer") && params.at("authServer").isString())
            authServer = params.at("authServer").asString();
        std::string prefetched;
        if (params.isObject() && params.contains("prefetched") && params.at("prefetched").isString())
            prefetched = params.at("prefetched").asString();
        std::string injectorPath;
        if (params.isObject() && params.contains("authlibInjectorPath") && params.at("authlibInjectorPath").isString())
            injectorPath = params.at("authlibInjectorPath").asString();

        // §4.6 在线登录：profile JSON 注入 ${user_properties}（皮肤与披风渲染），仅 MSA/正版账号携带
        std::string userProperties = "{}";
        if (params.isObject() && params.contains("userProperties") && params.at("userProperties").isString()) {
            std::string up = params.at("userProperties").asString();
            if (!up.empty()) userProperties = up;
        }

        // 离线账号规范化：userType=legacy + 确定性 UUID（未提供真实 UUID 时按昵称生成）
        if (accountType == "offline") {
            if (userType.empty() || userType == "mojang") userType = "legacy";
            if (uuid.empty() || uuid == "00000000-0000-0000-0000-000000000000")
                uuid = OfflinePlayerUuid(username);
        }

        // 是否启用自定义分辨率（决定 --width/--height 是否随 has_custom_resolution 规则传入）
        bool customResolution = false;
        if (params.isObject() && params.contains("customResolution") && params.at("customResolution").isBool())
            customResolution = params.at("customResolution").asBool();

        int memMB = 2048;
        if (params.isObject() && params.contains("memMB") && params.at("memMB").isNumber())
            memMB = (int)params.at("memMB").asNumber();

        int width = 854, height = 480;
        if (params.isObject() && params.contains("width") && params.at("width").isNumber())
            width = (int)params.at("width").asNumber();
        if (params.isObject() && params.contains("height") && params.at("height").isNumber())
            height = (int)params.at("height").asNumber();

        bool dryRun = false;
        if (params.isObject() && params.contains("dryRun") && params.at("dryRun").isBool())
            dryRun = params.at("dryRun").asBool();

        bool versionIsolation = false;
        if (params.isObject() && params.contains("versionIsolation") && params.at("versionIsolation").isBool())
            versionIsolation = params.at("versionIsolation").asBool();

        std::wstring mcRoot = GetMcRoot();
        std::wstring wVersion = lxe::Utf8ToWide(version);
        std::wstring launchLibDir = mcRoot + L"\\libraries";

        // 1) 用户手动指定的 Java（自定义路径）
        std::wstring javaPath;
        if (params.isObject() && params.contains("javaPath") && params.at("javaPath").isString()) {
            std::string jp = params.at("javaPath").asString();
            if (!jp.empty()) javaPath = lxe::Utf8ToWide(jp);
        }

        // 2) 解析 version.json（含 inheritsFrom 合并）
        Json vj = ResolveVersionJson(mcRoot, wVersion);
        if (!vj.isObject() || vj.size() == 0) return Err(-32012, "版本 JSON 解析失败: " + version);

        // 3) 自动选择 Java：优先读取 version.json 的 javaVersion.majorVersion（最准），
        // 缺省时按 MC 版本推荐主版本（<1.17→8；1.17~1.20.4→17；1.20.5+→21），从已装 Java 中挑选，避免用错 Java 启动
        std::wstring autoJavaPath;
        int recommendedMajor = JavaMajorFromVersionJson(vj, version);
        // 3.1) javaVersion 字段（version.json）：游戏 JSON 明确指定所需 Java 主版本（如 8/17/21）时，
        //      用它覆盖按 MC 版本号的推荐值（快照/未来版本/加载器自定义 JSON 常在此字段给真实需求）
        if (vj.contains("javaVersion") && vj.at("javaVersion").isObject() &&
            vj.at("javaVersion").contains("majorVersion") &&
            (vj.at("javaVersion").at("majorVersion").isNumber() || vj.at("javaVersion").at("majorVersion").isString())) {
            int jvMajor = 0;
            if (vj.at("javaVersion").at("majorVersion").isNumber())
                jvMajor = (int)vj.at("javaVersion").at("majorVersion").asNumber();
            else {
                try { jvMajor = std::stoi(vj.at("javaVersion").at("majorVersion").asString()); } catch (...) {}
            }
            if (jvMajor >= 8) recommendedMajor = jvMajor;
        }
        // 已指定具体 Java 路径时无需全盘扫描（省去注册表+常见路径+深度枚举的 I/O）；
        // 仅当路径为空（自动检测）或为 "Java N" 标签（需按主版本挑选）时才扫描已装 Java。
        bool needScan = javaPath.empty() ||
                        (javaPath.size() > 5 && javaPath.rfind(L"Java ", 0) == 0);
        std::vector<InstalledJava> javas;
        if (needScan) javas = ScanInstalledJavas();
        // 前端 Java 下拉固定档位（如 "Java 8" / "Java 17" / "Java 25"）→ 按主版本从已装 Java 中挑选实际路径
        if (javaPath.size() > 5 && javaPath.rfind(L"Java ", 0) == 0) {
            int want = 0;
            try { want = std::stoi(javaPath.substr(5)); } catch (...) {}
            if (want > 0) {
                std::wstring picked = PickJavaForMajor(javas, want);
                // 无该主版本运行时：清空标签，交给下方自动选择（避免残留 "Java N" 标签被当成路径 → 无效错误）
                if (!picked.empty()) javaPath = picked;
                else javaPath.clear();
            }
        }
        autoJavaPath = PickJavaForMajor(javas, recommendedMajor);
        if (javaPath.empty()) {
            javaPath = autoJavaPath;
            if (javaPath.empty()) javaPath = FindJavaPath();
        }
        // 取消严格 Java 环境检测：只要路径里有可执行的 .exe 文件即可继续。指定路径为目录时，自动在该目录内
        // 查找 javaw.exe / java.exe / 任意 .exe，作为有效 Java 继续启动，不再强制匹配推荐版本。
        if (!javaPath.empty()) {
            std::error_code jdec;
            if (std::filesystem::is_directory(javaPath, jdec)) {
                std::wstring found;
                std::wstring w1 = javaPath + L"\\javaw.exe";
                std::wstring w2 = javaPath + L"\\java.exe";
                if (std::filesystem::exists(w1, jdec)) found = w1;
                else if (std::filesystem::exists(w2, jdec)) found = w2;
                else {
                    for (auto& e : std::filesystem::directory_iterator(javaPath, jdec)) {
                        if (!e.is_regular_file(jdec)) continue;
                        std::wstring n = e.path().filename().wstring();
                        if (n.size() > 4 && _wcsicmp(n.substr(n.size() - 4).c_str(), L".exe") == 0) {
                            found = e.path().wstring();
                            break;
                        }
                    }
                }
                if (!found.empty()) javaPath = found;
            }
        }
        if (javaPath.empty()) return Err(-32010, "未找到 Java，请安装 Java 运行时或在设置中指定 Java 路径");
        if (GetFileAttributesW(javaPath.c_str()) == INVALID_FILE_ATTRIBUTES)
            return Err(-32011, "Java 路径无效: " + lxe::WideToUtf8(javaPath));
        // 探测实际 Java 主版本：用于按版本过滤已移除/不兼容的 JVM 参数（如 Java 24+ 已删除
        // --sun-misc-unsafe-memory-access，Java 21 以下仍可用；探测失败则视为 0=未知，仅过滤高版本场景）
        int actualJavaMajor = 0;
        {
            std::wstring out = RunCaptureTimeout(L"\"" + javaPath + L"\" -XshowSettings:properties -version", 15000);
            actualJavaMajor = JavaMajorFromVersionText(out);
        }
        // 记录本次启动的 Java 主版本与游戏目录，供崩溃/退出事件携带（插件据此精确判定）
        {
            std::lock_guard<std::mutex> lock(g_lastLaunchMu);
            g_lastLaunchJavaMajor = actualJavaMajor;
            g_lastLaunchGameDir = mcRoot;
        }

        // 4) 检查 client jar 存在
        std::wstring jarPath = mcRoot + L"\\versions\\" + wVersion + L"\\" + wVersion + L".jar";
        if (!std::filesystem::exists(jarPath)) {
            // 缺失时尝试自动补下原版客户端 jar（版本 json 的 downloads.client.url）
            std::string jurl;
            if (vj.contains("downloads") && vj.at("downloads").isObject() &&
                vj.at("downloads").contains("client") && vj.at("downloads").at("client").isObject()) {
                const auto& c = vj.at("downloads").at("client");
                if (c.contains("url") && c.at("url").isString()) jurl = c.at("url").asString();
            }
            if (!jurl.empty()) {
                std::error_code ecdl;
                std::filesystem::create_directories(std::filesystem::path(jarPath).parent_path(), ecdl);
                if (DownloadFileSmart(lxe::Utf8ToWide(jurl), std::filesystem::path(jarPath).parent_path(), lxe::Utf8ToWide(version) + L".jar", nullptr))
                    { /* 补下成功 */ }
            }
            if (!std::filesystem::exists(jarPath))
                return Err(-32013, "版本 JAR 不存在: " + lxe::WideToUtf8(jarPath));
        }

        // 4.1) 校验/补下 libraries：缺失时按 downloads.artifact.url 尝试下载（best-effort）
        {
            std::error_code ec0;
            if (vj.contains("libraries") && vj.at("libraries").isArray()) {
                for (const auto& lib : vj.at("libraries").asArray()) {
                    if (!lib.isObject() || !IsLibAllowedOnWindows(lib)) continue;
                    bool handled = false;
                    if (lib.contains("downloads") && lib.at("downloads").isObject()) {
                        const auto& dl = lib.at("downloads");
                        if (dl.contains("artifact") && dl.at("artifact").isObject()) {
                            const auto& art = dl.at("artifact");
                            if (art.contains("path") && art.at("path").isString()) {
                                std::wstring libFile = mcRoot + L"\\libraries\\" + lxe::Utf8ToWide(art.at("path").asString());
                                if (art.contains("url") && art.at("url").isString()) {
                                    if (std::filesystem::exists(libFile)) { handled = true; continue; }
                                    std::wstring libUrl = lxe::Utf8ToWide(art.at("url").asString());
                                    size_t slash = libFile.find_last_of(L"\\");
                                    if (slash != std::wstring::npos) {
                                        std::wstring dir = libFile.substr(0, slash);
                                        std::wstring fname = libFile.substr(slash + 1);
                                        std::filesystem::create_directories(dir, ec0);
                                        DownloadFileSmart(libUrl, dir, fname, nullptr);
                                    }
                                    handled = true;
                                }
                            }
                        }
                    }
                    // 安装器产物兜底：无 downloads 或 artifact.url 为空（Forge/Fabric/OptiFine 合并 json，
                    // 如 launchwrapper / forge 各运行库）→ 按 3 段 maven 坐标从官方 maven 补下，
                    // 否则 classpath 缺库启动即 NoClassDefFoundError。
                    if (!handled && lib.contains("name") && lib.at("name").isString()) {
                        std::string coord = lib.at("name").asString();
                        int segs = 0;
                        {
                            std::string base = coord;
                            if (base.find('@') != std::string::npos) base = base.substr(0, base.find('@'));
                            for (char c : base) if (c == ':') ++segs;
                            segs += 1;
                        }
                        if (segs >= 3 && segs <= 3) {
                            std::string rel = MavenCoordToPath(coord);
                            if (!rel.empty()) {
                                std::wstring wrel = lxe::Utf8ToWide(rel);
                                std::replace(wrel.begin(), wrel.end(), L'/', L'\\');
                                std::wstring full = mcRoot + L"\\libraries\\" + wrel;
                                if (std::filesystem::exists(full)) continue;
                                size_t slash = full.find_last_of(L"\\");
                                if (slash != std::wstring::npos) {
                                    std::wstring dir = full.substr(0, slash);
                                    std::wstring fname = full.substr(slash + 1);
                                    std::filesystem::create_directories(dir, ec0);
                                    DownloadFileSmart(lxe::Utf8ToWide("https://files.minecraftforge.net/maven/" + rel), dir, fname, nullptr);
                                }
                            }
                        }
                    }
                }
            }
        }

        // 4) 构建 classpath（必须放在 4.1 补下之后，否则缺库被 LibPathFromName 静默跳过）
        std::wstring classpath = BuildClasspath(vj, mcRoot, wVersion);

        // 5) 主类
        std::string mainClass = "net.minecraft.client.main.Main";
        if (vj.contains("mainClass") && vj.at("mainClass").isString())
            mainClass = vj.at("mainClass").asString();

        // 6) natives 目录（PCL2 兼容命名：<版本>-natives，如 1.16.5OP-natives）
        std::wstring nativesDir = mcRoot + L"\\versions\\" + wVersion + L"\\" + wVersion + L"-natives";

        // 解压 natives 库（先清空旧文件再解压，防 0 字节/残留 DLL 导致 LWJGL 加载失败）
        // 补下缺失/0 字节的 natives 分类器 jar 后再解压：[launch 原流程只补 artifact，不补 classifier，
        // 若 natives jar 缺失会静默跳过 → natives 目录为空 → 找不到 lwjgl.dll]
        auto natives = ExtractNatives(vj, mcRoot);
        EnsureNativeJars(natives, mcRoot);
        ExtractNativesToDir(natives, mcRoot, nativesDir);

        // 7) 构建 JVM 参数
        std::wstring assetIndexName;
        if (vj.contains("assetIndex") && vj.at("assetIndex").isObject() &&
            vj.at("assetIndex").contains("id") && vj.at("assetIndex").at("id").isString()) {
            assetIndexName = lxe::Utf8ToWide(vj.at("assetIndex").at("id").asString());
        }
        // 启动前确保资源索引与日志配置就绪（缺失则自动补下）
        EnsureAssetIndex(vj, mcRoot);
        std::wstring logCfg = EnsureLoggingConfig(vj, mcRoot);

        std::wstring assetsDir = mcRoot + L"\\assets";
        std::wstring gameDir = mcRoot;
        // 版本隔离：游戏数据目录指向 versions/<version>，整合包/隔离存档不污染全局 mcRoot（saves/config/mods 各版本独立）
        if (versionIsolation) {
            std::error_code eciso;
            std::filesystem::create_directories(mcRoot + L"\\versions\\" + wVersion, eciso);
            gameDir = mcRoot + L"\\versions\\" + wVersion;
        }

        std::vector<std::wstring> jvmArgs;
        jvmArgs.push_back(L"-Xmx" + std::to_wstring(memMB) + L"m");
        jvmArgs.push_back(L"-Xms" + std::to_wstring(memMB / 4) + L"m");
        jvmArgs.push_back(L"-XX:+UseG1GC");
        jvmArgs.push_back(L"-XX:-UseAdaptiveSizePolicy");
        jvmArgs.push_back(L"-XX:-OmitStackTraceInFastThrow");
        jvmArgs.push_back(L"-XX:HeapDumpPath=MojangTricksIntelDriversForPerformance_javaw.exe_minecraft.exe.heapdump");
        jvmArgs.push_back(L"-Dos.name=Windows 10");
        jvmArgs.push_back(L"-Dos.version=10.0");
        jvmArgs.push_back(L"-Dminecraft.launcher.brand=LXElauncher");
        jvmArgs.push_back(L"-Dminecraft.launcher.version=1.0");
        jvmArgs.push_back(L"-Djava.library.path=" + nativesDir);
        // 日志配置：优先使用 version.json 提供的 log4j 配置（assets/log_configs/client-*.xml），缺失时跳过
        // （旧版本/硬编码的 log4j2.xml 不存在会导致启动失败）
        if (!logCfg.empty()) jvmArgs.push_back(L"-Dlog4j.configurationFile=" + logCfg);
        // 旧版本 / 加载器常见参数（启动脚本采样 + 教程：低于 1.13 的版本与 Forge 需要）
        jvmArgs.push_back(L"-Djdk.lang.Process.allowAmbiguousCommands=True");
        jvmArgs.push_back(L"-Dfml.ignoreInvalidMinecraftCertificates=True");
        jvmArgs.push_back(L"-Dfml.ignorePatchDiscrepancies=True");
        jvmArgs.push_back(L"-Dlog4j2.formatMsgNoLookups=true");
        // 追加 version.json 的 arguments.jvm（修复：Forge/Fabric 常依赖其中的 -Dlegacy 等参数）
        // 字符串项直接加入；带 rules 的对象项按 Windows 平台判定后展开 value
        if (vj.contains("arguments") && vj.at("arguments").isObject() &&
            vj.at("arguments").contains("jvm") && vj.at("arguments").at("jvm").isArray()) {
            for (const auto& arg : vj.at("arguments").at("jvm").asArray()) {
                if (arg.isString()) {
                    std::wstring a = lxe::Utf8ToWide(arg.asString());
                    // 新版 version.json（Forge 1.21+ / 2026 快照起）会把 -cp 与 ${classpath} 作为独立字符串项列出。
                    // 我们统一在下方追加 -cp <classpath>，因此凡是仍含 ${classpath} 字面量的项一律跳过，
                    // 避免残留字样被 JVM 当成主类（ClassNotFoundException: ${classpath}）。
                    // 同时替换新版用到的新占位符：${natives_directory} / ${launcher_name} / ${launcher_version}
                    {
                        auto replaceAll = [&a](const std::wstring& pat, const std::wstring& rep) {
                            size_t p = 0;
                            while ((p = a.find(pat, p)) != std::wstring::npos) { a.replace(p, pat.size(), rep); p += rep.size(); }
                        };
                        std::wstring pat1 = L"${library_directory}";
                        size_t p1 = 0;
                        while ((p1 = a.find(pat1, p1)) != std::wstring::npos) { a.replace(p1, pat1.size(), launchLibDir); p1 += launchLibDir.size(); }
                        std::wstring pat2 = L"${classpath_separator}";
                        size_t p2 = 0;
                        while ((p2 = a.find(pat2, p2)) != std::wstring::npos) { a.replace(p2, pat2.size(), L";"); p2 += 1; }
                        replaceAll(L"${natives_directory}", nativesDir);
                        replaceAll(L"${launcher_name}", L"LXElauncher");
                        replaceAll(L"${launcher_version}", L"1.0");
                    }
                    // ${classpath} 字面量（含带空格/拼接等变体）一律丢弃：classpath 由下方自行追加，避免泄漏为主类
                    bool hasClasspathLeftover = (a.find(L"${classpath}") != std::wstring::npos);
                    if (hasClasspathLeftover) continue;
                    if (!a.empty()) {
                        static const wchar_t* kSkipPrefix[] = { L"-xmx", L"-xms", L"-cp", L"-classpath" };
                        static const wchar_t* kSkipExact[] = { L"-cp", L"-classpath", L"-class-path" };
                        std::wstring lower = a;
                        std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
                        bool skip = false;
                        for (auto sp : kSkipPrefix)
                            if (lower.rfind(sp, 0) == 0) { skip = true; break; }
                        for (auto se : kSkipExact)
                            if (lower == se) { skip = true; break; }
                        if (lower.rfind(L"-javaagent", 0) == 0 || lower.rfind(L"-djava.library.path", 0) == 0) skip = true;
                        // Java 24+ 已移除 --sun-misc-unsafe-memory-access（旧版 version.json / 第三方 json 仍会写），
                        // 不剔除会导致 JVM 启动即 "Unrecognized option" 退出
                        if (actualJavaMajor >= 24 && lower.rfind(L"--sun-misc-unsafe-memory-access", 0) == 0) skip = true;
                        // 尺寸类 JVM 参数（-Xmx/-Xms）已被上面 kSkipPrefix 剔除，但第三方 json 偶发写成
                        // -Xmx3072m（大写）或 -Xms 带单位间隙，统一兜底：短划线 -X/-XX 前缀的非引导类参数
                        // 一律跳过 —— 仅保留显式需要的构造参数（引导类 --add-*/-p/-Dlauncherforgepath 等以
                        // -D 开头的一般参数会被 -djava.library.path 前缀规则误杀，故此处只剔除 -X 头）。
                        if (lower.size() >= 2 && lower[0] == L'-' && lower[1] == L'X') skip = true;
                        if (skip) continue;
                        // 保留 -p/--add-modules/--add-opens 等的成对参数值（Forge bootstraplauncher 需要）
                        jvmArgs.push_back(a);
                    }
                } else if (arg.isObject() && IsLibAllowedOnWindows(arg) && arg.contains("value")) {
                    const Json& val = arg.at("value");
                    if (val.isString()) {
                        std::wstring a = lxe::Utf8ToWide(val.asString());
                        if (!a.empty()) jvmArgs.push_back(a);
                    } else if (val.isArray()) {
                        for (const auto& va : val.asArray()) {
                            if (!va.isString()) continue;
                            std::wstring a = lxe::Utf8ToWide(va.asString());
                            if (!a.empty()) jvmArgs.push_back(a);
                        }
                    }
                }
            }
        }
        jvmArgs.push_back(L"-cp");
        jvmArgs.push_back(classpath);

        // 第三方认证：注入 authlib-injector javaagent 与元数据预取参数
        if (!injectorPath.empty() && !authServer.empty()) {
            // 确保 authlib-injector.jar 存在：缺失时先尝试下载（BMCLAPI 镜像 → 官方源），
            // 否则 JVM 启动即报 "Error opening zip file or JAR manifest missing : authlib-injector.jar"
            std::filesystem::path injPath(lxe::Utf8ToWide(injectorPath));
            std::error_code iec;
            bool injExists = std::filesystem::is_regular_file(injPath, iec) && std::filesystem::file_size(injPath, iec) > 0;
            if (!injExists) {
                std::wstring baseDir = injPath.parent_path().wstring();
                std::wstring baseName = injPath.filename().wstring();
                if (baseDir.empty()) { baseDir = mcRoot; baseName = L"authlib-injector.jar"; }
                std::filesystem::create_directories(baseDir, iec);
                // 走官方下载 API：GET /artifact/latest.json → download_url（BMCLAPI → 官方 → GitHub 兜底）
                bool got = DownloadAuthlibInjector(baseDir, baseName);
                if (!got) return Err(-32057, "authlib-injector.jar 缺失且下载失败，请检查外置登录配置");
            }
            jvmArgs.push_back(L"-javaagent:" + lxe::Utf8ToWide(injectorPath) + L"=" + lxe::Utf8ToWide(authServer));
            if (!prefetched.empty())
                jvmArgs.push_back(L"-Dauthlibinjector.yggdrasil.prefetched=" + lxe::Utf8ToWide(prefetched));
        }

        // 8) 构建游戏参数
        // 优先使用 arguments.game（1.13+），否则使用 minecraftArguments（旧版本）
        std::vector<std::wstring> gameArgs;
        bool hasNewArgs = vj.contains("arguments") && vj.at("arguments").isObject() &&
                          vj.at("arguments").contains("game") && vj.at("arguments").at("game").isArray();

        // 模板替换函数
        auto replaceVar = [](std::wstring s, const std::wstring& key, const std::wstring& val) -> std::wstring {
            std::wstring pattern = L"${" + key + L"}";
            size_t pos = 0;
            while ((pos = s.find(pattern, pos)) != std::string::npos) {
                s.replace(pos, pattern.size(), val);
                pos += val.size();
            }
            return s;
        };

        std::wstring wUsername = lxe::Utf8ToWide(username);
        std::wstring wUuid = lxe::Utf8ToWide(uuid);
        std::wstring wToken = lxe::Utf8ToWide(accessToken);
        std::wstring wUserType = lxe::Utf8ToWide(userType);
        std::wstring wVersionType = L"LXElauncher";

        // 规则判定（version.json arguments 中带 rules 的对象参数）：
        // 语义与官方一致——默认不包含；命中 allow 才包含；命中 disallow 立即排除。
        // 实际 feature 值：isDemoUser 恒为 false（本启动器无 demo 账号）、
        // hasQuickPlaysSupport 恒为 false（不支持快速开始）、hasCustomRes 由启动设置决定。
        auto rulesAllow = [](const Json& arg, bool hasCustomRes, bool isDemoUser, bool hasQuickPlays) -> bool {
            if (!arg.contains("rules") || !arg.at("rules").isArray()) return true;
            bool allowed = false;
            for (const auto& r : arg.at("rules").asArray()) {
                if (!r.isObject()) continue;
                std::string action = "allow";
                if (r.contains("action") && r.at("action").isString()) action = r.at("action").asString();
                bool match = true;
                if (r.contains("os") && r.at("os").isObject()) {
                    const auto& os = r.at("os");
                    if (os.contains("name") && os.at("name").isString())
                        match = match && (os.at("name").asString() == "windows");
                }
                if (r.contains("features") && r.at("features").isObject()) {
                    const auto& f = r.at("features");
                    if (f.contains("is_demo_user") && f.at("is_demo_user").isBool())
                        match = match && (f.at("is_demo_user").asBool() == isDemoUser);
                    if (f.contains("has_custom_resolution") && f.at("has_custom_resolution").isBool())
                        match = match && (f.at("has_custom_resolution").asBool() == hasCustomRes);
                    if (f.contains("has_quick_plays_support") && f.at("has_quick_plays_support").isBool())
                        match = match && (f.at("has_quick_plays_support").asBool() == hasQuickPlays);
                }
                if (match) {
                    if (action == "disallow") return false;
                    allowed = true;
                }
            }
            return allowed;
        };
        // 模板替换（游戏参数）
        auto applyGameVars = [&](std::wstring a) -> std::wstring {
            a = replaceVar(a, L"auth_player_name", wUsername);
            a = replaceVar(a, L"version_name", wVersion);
            a = replaceVar(a, L"game_directory", gameDir);
            a = replaceVar(a, L"assets_root", assetsDir);
            a = replaceVar(a, L"assets_index_name", assetIndexName);
            a = replaceVar(a, L"auth_uuid", wUuid);
            a = replaceVar(a, L"auth_access_token", wToken);
            a = replaceVar(a, L"user_type", wUserType);
            a = replaceVar(a, L"version_type", wVersionType);
            a = replaceVar(a, L"resolution_width", std::to_wstring(width));
            a = replaceVar(a, L"resolution_height", std::to_wstring(height));
            // 新版（2026 快照起）模板变量：--clientId / --xuid；第三方登录无 xuid 时给固定值避免空参错位
            a = replaceVar(a, L"clientid", L"lxelauncher-mc");
            a = replaceVar(a, L"auth_xuid", L"0");
            // 旧版本（1.8 及更早）模板变量
            a = replaceVar(a, L"user_properties", lxe::Utf8ToWide(userProperties));
            a = replaceVar(a, L"auth_session", wToken);
            a = replaceVar(a, L"profile_name", wUsername);
            a = replaceVar(a, L"game_assets", assetsDir + L"\\virtual\\legacy");
            return a;
        };
        // 引号感知分割（minecraftArguments 中 gameDir 等路径可能含空格）
        auto splitArgs = [](const std::wstring& s) -> std::vector<std::wstring> {
            std::vector<std::wstring> out;
            std::wstring cur;
            bool inQ = false;
            for (size_t i = 0; i < s.size(); ++i) {
                wchar_t c = s[i];
                if (c == L'"') { inQ = !inQ; continue; }
                if (c == L' ' && !inQ) { if (!cur.empty()) { out.push_back(cur); cur.clear(); } continue; }
                cur += c;
            }
            if (!cur.empty()) out.push_back(cur);
            return out;
        };

        if (hasNewArgs) {
            // 解析 arguments.game 数组（含 rules 对象，如 has_custom_resolution 的 --width/--height）
            for (const auto& arg : vj.at("arguments").at("game").asArray()) {
                if (arg.isString()) {
                    std::wstring a = applyGameVars(lxe::Utf8ToWide(arg.asString()));
                    gameArgs.push_back(a);
                } else if (arg.isObject() && rulesAllow(arg, customResolution, false, false) && arg.contains("value")) {
                    const Json& val = arg.at("value");
                    if (val.isString()) {
                        gameArgs.push_back(applyGameVars(lxe::Utf8ToWide(val.asString())));
                    } else if (val.isArray()) {
                        for (const auto& va : val.asArray()) {
                            if (va.isString()) gameArgs.push_back(applyGameVars(lxe::Utf8ToWide(va.asString())));
                        }
                    }
                }
            }
        } else if (vj.contains("minecraftArguments") && vj.at("minecraftArguments").isString()) {
            // 旧版本：空格分隔的模板字符串（引号感知分割）
            std::wstring mcArgs = applyGameVars(lxe::Utf8ToWide(vj.at("minecraftArguments").asString()));
            for (auto& t : splitArgs(mcArgs)) gameArgs.push_back(t);
        }

        // 剔除 Minecraft quick play 参数：版本 json 可能残留未替换的 ${quickPlay*} 模板变量，
        // 且游戏只允许一个 quickPlay 选项、多个同时指定会直接 IllegalArgument 崩溃 → 一律移除（连同其值）。
        {
            std::vector<std::wstring> clean;
            for (size_t i = 0; i < gameArgs.size(); ++i) {
                const std::wstring& a = gameArgs[i];
                if (a.size() > 11 && a.rfind(L"--quickPlay", 0) == 0) {
                    // 当前项为 --quickPlayXxx 且不带 '='：其后一个元素是它的值，一并跳过
                    if (a.find(L'=') == std::wstring::npos && i + 1 < gameArgs.size() &&
                        gameArgs[i + 1].rfind(L"--", 0) != 0)
                        ++i;
                    continue;
                }
                clean.push_back(a);
            }
            gameArgs.swap(clean);
        }

        // §4.3 启动装配：OptiFine 与 Forge 并存时，把 OptiFine 的启动入口替换为 Forge 专用变体
        // （Forge 环境必须用 optifine.OptiFineForgeTweaker 而非通用 OptiFineTweaker），
        // 并保证该入口位于游戏参数末尾（launchwrapper 按序执行 tweak，Forge 的需最后挂载）。
        {
            bool hasForge = false;
            if (vj.contains("libraries") && vj.at("libraries").isArray()) {
                for (const auto& lib : vj.at("libraries").asArray()) {
                    if (!lib.isObject() || !lib.contains("name") || !lib.at("name").isString()) continue;
                    if (lib.at("name").asString().rfind("net.minecraftforge", 0) == 0) { hasForge = true; break; }
                }
            }
            if (hasForge) {
                std::vector<std::wstring> kept;
                std::wstring movedTweak;
                for (size_t i = 0; i < gameArgs.size(); ++i) {
                    if (gameArgs[i] == L"--tweakClass" && i + 1 < gameArgs.size() &&
                        gameArgs[i + 1] == L"optifine.OptiFineTweaker") {
                        movedTweak = L"optifine.OptiFineForgeTweaker";
                        ++i; // 跳过被替换的原值，入口整体移到末尾
                        continue;
                    }
                    kept.push_back(gameArgs[i]);
                }
                if (!movedTweak.empty()) {
                    kept.push_back(L"--tweakClass");
                    kept.push_back(movedTweak);
                    gameArgs.swap(kept);
                }
            }
        }

        // 9) 组装完整命令行
        // 最终形态：java <JVM 参数> -cp <classpath> <主类> <游戏参数>
        std::wstring cmdLine = L"\"" + javaPath + L"\"";
        // 9.1) 防御性替换：确保 JVM 段里不残留任何 ${...} 占位符（尤其是 ${classpath}）
        //       —— 遍历 jvmArgs，凡仍含 ${ 的项 且 不含已拼好的实际路径 一律剔除；
        //       否则会把 "${classpath}" 之类字样传给 JVM，启动即 ClassNotFoundException/找不到主类。
        {
            std::vector<std::wstring> clean;
            for (auto& a : jvmArgs) {
                if (a.find(L"${") != std::wstring::npos &&
                    (a.find(L"${classpath}") != std::wstring::npos || a.find(L"${") == 0))
                    continue; // 剔除字面量占位符项（cp 已在下方追加）
                clean.push_back(a);
            }
            jvmArgs.swap(clean);
        }
        for (const auto& a : jvmArgs) {
            cmdLine += L" ";
            // 如果参数含空格，加引号
            if (a.find(L' ') != std::wstring::npos || a.find(L';') != std::wstring::npos)
                cmdLine += L"\"" + a + L"\"";
            else
                cmdLine += a;
        }
        // 9.2) 主类防御：若 mainClass 本身仍是占位符（如 ${main_class}），替换为原版默认主类
        std::string finalMainClass = mainClass;
        if (finalMainClass.find("${") != std::string::npos) finalMainClass = "net.minecraft.client.main.Main";
        cmdLine += L" " + lxe::Utf8ToWide(finalMainClass);
        for (const auto& a : gameArgs) {
            cmdLine += L" ";
            if (a.find(L' ') != std::wstring::npos)
                cmdLine += L"\"" + a + L"\"";
            else
                cmdLine += a;
        }

        // 10) dryRun：只返回完整命令行，不真正启动（用于导出启动脚本）
        std::wstring command = cmdLine;
        if (dryRun) {
            Json dr = Json::object();
            dr["success"] = true;
            dr["dryRun"] = true;
            dr["command"] = lxe::WideToUtf8(command);
            dr["mcRoot"] = lxe::WideToUtf8(mcRoot);
            dr["javaPath"] = lxe::WideToUtf8(javaPath);
            dr["version"] = version;
            dr["mainClass"] = mainClass;
            dr["recommendedMajor"] = recommendedMajor;
            return Ok(dr);
        }

        // 11) 日志重定向：stdout → logs/lxe-launcher-std.log，stderr → logs/lxe-launcher-crash.log
        std::wstring logsDir = mcRoot + L"\\logs";
        std::error_code ec;
        std::filesystem::create_directories(logsDir, ec);
        SECURITY_ATTRIBUTES sa{}; sa.nLength = sizeof(sa); sa.bInheritHandle = TRUE;
        HANDLE hStd = CreateFileW((logsDir + L"\\lxe-launcher-std.log").c_str(), FILE_APPEND_DATA,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        HANDLE hErr = CreateFileW((logsDir + L"\\lxe-launcher-crash.log").c_str(), FILE_APPEND_DATA,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

        STARTUPINFOW si{}; si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
        si.wShowWindow = SW_SHOWNORMAL;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = hStd == INVALID_HANDLE_VALUE ? nullptr : hStd;
        si.hStdError = hErr == INVALID_HANDLE_VALUE ? nullptr : hErr;
        PROCESS_INFORMATION pi{};

        // 设置工作目录为 mcRoot
        std::wstring workingDir = mcRoot;

        // CreateProcessW 需要可写的命令行缓冲区
        std::vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
        cmdBuf.push_back(L'\0');

        BOOL ok = CreateProcessW(
            nullptr,
            cmdBuf.data(),
            nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
            nullptr,
            workingDir.c_str(),
            &si, &pi
        );

        if (hStd != INVALID_HANDLE_VALUE) CloseHandle(hStd);
        if (hErr != INVALID_HANDLE_VALUE) CloseHandle(hErr);

        if (!ok) {
            DWORD err = GetLastError();
            return Err(-32020, "CreateProcess 失败: 错误码 " + std::to_string(err));
        }

        // 保存游戏进程句柄（多实例）
        {
            std::lock_guard<std::mutex> lock(g_gameInstancesMutex);
            g_gameInstances.push_back({pi.hProcess, pi.dwProcessId});
        }
        CloseHandle(pi.hThread);

        Json result = Json::object();
        result["success"] = true;
        result["pid"] = (int)pi.dwProcessId;
        result["javaPath"] = lxe::WideToUtf8(javaPath);
        result["mcRoot"] = lxe::WideToUtf8(mcRoot);
        result["version"] = version;
        result["mainClass"] = mainClass;
        result["command"] = lxe::WideToUtf8(command);
        result["recommendedMajor"] = recommendedMajor;

        // 通知前端启动事件
        Json ev = Json::object();
        ev["version"] = version;
        ev["pid"] = (int)pi.dwProcessId;
        bridge.PostEvent("mc.launched", ev);

        return Ok(result);
            };
            HandlerResult r;
            try { r = run(); } catch (const std::exception& e) { r = Err(-32030, std::string("启动异常：") + e.what()); }
            catch (...) { r = Err(-32030, "启动未知异常"); }
            done(r);
        }).detach();
    });

    // 已安装 Java 列表 + 按 MC 版本推荐（异步，供前端展示/自动选 Java）
    bridge.RegisterAsync("mc.javaInfo", [](const Json& params, const std::function<void(HandlerResult)>& done) {
        std::thread([params, done]() {
            try {
            std::string version;
            if (params.isObject() && params.contains("version") && params.at("version").isString())
                version = params.at("version").asString();
            Json result = Json::object();
            result["recommendedMajor"] = version.empty() ? 0 : RecommendedJavaMajor(version);
            // §1.5：需求区间（按请求）。优先读 version.json 的 javaVersion.majorVersion（最准）
            {
                std::string loaderFor; bool offlineFor = false; int userMajor = 0;
                if (params.isObject() && params.contains("loaderId") && params.at("loaderId").isString())
                    loaderFor = params.at("loaderId").asString();
                if (params.isObject() && params.contains("offline") && params.at("offline").isBool())
                    offlineFor = params.at("offline").asBool();
                if (params.isObject() && params.contains("userMajor") && params.at("userMajor").isNumber())
                    userMajor = (int)params.at("userMajor").asNumber();
                int vjMajor = 0;
                if (!version.empty()) {
                    Json vj = ResolveVersionJson(GetMcRoot(), lxe::Utf8ToWide(version));
                    vjMajor = JavaMajorFromVersionJson(vj, version);
                }
                JavaReq req = JavaRequirement(version, loaderFor, offlineFor, userMajor, vjMajor);
                result["loMajor"] = req.lo;
                result["hiMajor"] = req.hi;
                result["reqNote"] = JavaRequirementText(req);
            }
            // §2.6：列表直接用当前缓存（秒回，不阻塞），随后后台增量扫描找到即加
            Json list = Json::array();
            {
                bool haveCache = false;
                {
                    std::lock_guard<std::mutex> lock(g_javaScanMtx);
                    haveCache = !g_javaScanCache.empty();
                }
                if (!haveCache) {
                    // 无内存缓存：尝试磁盘缓存（静默验证路径存在），仍空则后台启动增量扫描
                    std::vector<InstalledJava> disk;
                    LoadJavaCacheFromDisk(disk);
                    if (!disk.empty()) {
                        std::lock_guard<std::mutex> lock(g_javaScanMtx);
                        g_javaScanCache = disk;
                        haveCache = true;
                    }
                }
                if (!haveCache) AsyncScanJava(); // 后台探测，逐条推送 java.found
            }
            {
                std::lock_guard<std::mutex> lock(g_javaScanMtx);
                for (const auto& j : g_javaScanCache) {
                    Json o = Json::object();
                    o["path"] = lxe::WideToUtf8(j.path);
                    o["major"] = j.major;
                    o["is64"] = j.is64;
                    if (!j.fileEncoding.empty()) o["fileEncoding"] = lxe::WideToUtf8(j.fileEncoding);
                    if (!j.nativeEncoding.empty()) o["nativeEncoding"] = lxe::WideToUtf8(j.nativeEncoding);
                    list.asArray().push_back(o);
                }
            }
            result["list"] = list;
            done(Ok(result));
            } catch (const std::exception& e) {
                done(Err(-32052, std::string("获取 Java 信息异常：") + e.what()));
            } catch (...) {
                done(Err(-32052, "获取 Java 信息未知异常"));
            }
        }).detach();
    });

    // 主动静默验证 Java（后台线程，定期校验缓存路径可用性）
    // 前端「确定扫描」/启动时调用：立即触发一次全量后台重扫并返回当前内存缓存（秒回）。
    bridge.Register("mc.rescanJava", [](const Json&) {
        Json result = Json::object();
        result["started"] = true;
        AsyncScanJava();
        std::vector<InstalledJava> javas;
        {
            std::lock_guard<std::mutex> lock(g_javaScanMtx);
            javas = g_javaScanCache;
        }
        if (javas.empty()) javas = ScanInstalledJavas(); // 内存缓存都为空时同步扫一次（首次）
        Json arr = Json::array();
        for (const auto& j : javas) {
            Json o = Json::object();
            o["path"] = lxe::WideToUtf8(j.path);
            o["major"] = j.major;
            o["is64"] = j.is64;
            if (!j.fileEncoding.empty()) o["fileEncoding"] = lxe::WideToUtf8(j.fileEncoding);
            if (!j.nativeEncoding.empty()) o["nativeEncoding"] = lxe::WideToUtf8(j.nativeEncoding);
            arr.asArray().push_back(o);
        }
        result["list"] = arr;
        return Ok(result);
    });

    // 拖入 java.exe / javaw.exe 时：探测版本并加入「已找到的 Java」列表（内存+磁盘持久化+前端增量）。
    // 返回 {path, major, is64, fileEncoding, nativeEncoding}；major<=0 表示未能识别为有效 Java。
    // 探测需启动 JVM，走 RegisterAsync + 后台线程，避免阻塞桥消息线程（§10/§14 教训）。
    bridge.RegisterAsync("mc.addJavaRuntime", [](const Json& params, const std::function<void(HandlerResult)>& done) {
        std::thread([params, done]() {
            try {
                std::wstring path;
                if (params.isObject() && params.contains("path") && params.at("path").isString())
                    path = lxe::Utf8ToWide(params.at("path").asString());
                if (path.empty()) { done(Err(-32602, "缺少 path 参数")); return; }
                std::error_code ec;
                if (!std::filesystem::exists(path, ec)) { done(Err(-32053, "Java 文件不存在")); return; }
                InstalledJava j = ProbeJavaExe(path);
                Json result = Json::object();
                if (j.major <= 0 || !j.is64) {
                    result["path"] = lxe::WideToUtf8(path);
                    result["major"] = 0;
                    result["is64"] = j.is64;
                    done(Ok(result));
                    return;
                }
                InsertJavaFoundAndNotify(j); // 并入缓存 + 回写磁盘 + 推送 java.found
                result["path"] = lxe::WideToUtf8(j.path);
                result["major"] = j.major;
                result["is64"] = j.is64;
                if (!j.fileEncoding.empty()) result["fileEncoding"] = lxe::WideToUtf8(j.fileEncoding);
                if (!j.nativeEncoding.empty()) result["nativeEncoding"] = lxe::WideToUtf8(j.nativeEncoding);
                done(Ok(result));
            } catch (const std::exception& e) {
                done(Err(-32054, std::string("识别 Java 运行时异常：") + e.what()));
            } catch (...) {
                done(Err(-32054, "识别 Java 运行时未知异常"));
            }
        }).detach();
    });
    bridge.Register("mc.isGameRunning", [](const Json&) {
        Json result = Json::object();
        Json instances = Json::array();
        bool running = false;
        {
            std::lock_guard<std::mutex> lock(g_gameInstancesMutex);
            for (const auto& inst : g_gameInstances) {
                HANDLE hProcess = inst.first;
                DWORD pid = inst.second;
                bool alive = WaitForSingleObject(hProcess, 0) == WAIT_TIMEOUT;
                Json o = Json::object();
                o["pid"] = (int)pid;
                o["alive"] = alive;
                instances.asArray().push_back(o);
                if (alive) running = true;
            }
        }
        result["running"] = running;
        result["instances"] = instances;
        return Ok(result);
    });

    // 真实关闭游戏进程（支持指定 pid，不传则关闭所有）
    bridge.Register("mc.stopGame", [&bridge](const Json& params) {
        DWORD targetPid = 0;
        if (params.isObject() && params.contains("pid") && params.at("pid").isNumber()) {
            targetPid = (DWORD)params.at("pid").asNumber();
        }

        std::vector<std::pair<HANDLE, DWORD>> toKill;
        {
            std::lock_guard<std::mutex> lock(g_gameInstancesMutex);
            if (g_gameInstances.empty()) {
                return Err(-32030, "没有运行中的游戏进程");
            }
            if (targetPid != 0) {
                for (auto it = g_gameInstances.begin(); it != g_gameInstances.end(); ++it) {
                    if (it->second == targetPid) {
                        toKill.push_back(*it);
                        g_gameInstances.erase(it);
                        break;
                    }
                }
                if (toKill.empty()) {
                    return Err(-32031, "未找到指定 PID 的进程");
                }
            } else {
                toKill = g_gameInstances;
                g_gameInstances.clear();
            }
        }

        Json killedPids = Json::array();
        for (const auto& inst : toKill) {
            HANDLE hProcess = inst.first;
            DWORD pid = inst.second;
            TerminateProcess(hProcess, 0);
            CloseHandle(hProcess);
            killedPids.asArray().push_back((int)pid);

            Json ev = Json::object();
            ev["pid"] = (int)pid;
            ev["reason"] = "stopped";
            bridge.PostEvent("mc.stopped", ev);
        }

        Json result = Json::object();
        result["killed"] = true;
        result["pids"] = killedPids;
        return Ok(result);
    });
}

// ============ Java/MC 版本 安装状态检测与卸载 ============
void RegisterInstallStatus(Bridge& bridge) {

    // Java 已安装检测（仅本启动器下载：runtime/java-<version>）返回 {installed, javaPath, runtimeDir}
    bridge.Register("java.isInstalled", [](const Json& params) {
        std::wstring version;
        if (params.isObject() && params.contains("version") && params.at("version").isString())
            version = lxe::Utf8ToWide(params.at("version").asString());
        Json result = Json::object();
        result["installed"] = false;
        if (!version.empty() && !g_exeDir.empty()) {
            std::wstring runtimeDir = g_exeDir + L"\\runtime\\java-" + version;
            if (std::filesystem::is_directory(runtimeDir)) {
                std::wstring javaPath;
                std::error_code ec;
                for (auto& sub : std::filesystem::recursive_directory_iterator(runtimeDir, ec)) {
                    if (sub.path().filename() == L"javaw.exe") {
                        javaPath = sub.path().wstring();
                        break;
                    }
                }
                if (!javaPath.empty()) {
                    result["installed"] = true;
                    result["javaPath"] = lxe::WideToUtf8(javaPath);
                    result["runtimeDir"] = lxe::WideToUtf8(runtimeDir);
                }
            }
        }
        return Ok(result);
    });

    // 卸载 Java（删除 runtime/java-<version> 目录）
    bridge.Register("java.uninstall", [](const Json& params) {
        std::wstring version;
        if (params.isObject() && params.contains("version") && params.at("version").isString())
            version = lxe::Utf8ToWide(params.at("version").asString());
        if (version.empty()) return Err(-32602, "missing version");
        if (g_exeDir.empty()) return Err(-32001, "exe dir not set");
        std::wstring runtimeDir = g_exeDir + L"\\runtime\\java-" + version;
        Json result = Json::object();
        if (!std::filesystem::exists(runtimeDir)) {
            result["ok"] = false;
            result["reason"] = "not_installed";
            return Ok(result);
        }
        std::error_code ec;
        std::filesystem::remove_all(runtimeDir, ec);
        result["ok"] = !ec;
        if (ec) result["error"] = ec.message();
        return Ok(result);
    });

    // 列出本启动器已安装 Java 列表
    bridge.Register("java.listInstalled", [](const Json&) {
        Json arr = Json::array();
        if (!g_exeDir.empty()) {
            std::wstring runtimeDir = g_exeDir + L"\\runtime";
            std::error_code ec;
            for (auto& entry : std::filesystem::directory_iterator(runtimeDir, ec)) {
                if (!entry.is_directory()) continue;
                std::wstring dirName = entry.path().filename().wstring();
                if (dirName.rfind(L"java-", 0) != 0) continue;
                std::wstring javaPath;
                for (auto& sub : std::filesystem::recursive_directory_iterator(entry.path(), ec)) {
                    if (sub.path().filename() == L"javaw.exe") {
                        javaPath = sub.path().wstring();
                        break;
                    }
                }
                Json o = Json::object();
                o["version"] = lxe::WideToUtf8(dirName.substr(5));
                o["runtimeDir"] = lxe::WideToUtf8(entry.path().wstring());
                o["javaPath"] = lxe::WideToUtf8(javaPath);
                arr.asArray().push_back(o);
            }
        }
        Json result = Json::object();
        result["items"] = arr;
        return Ok(result);
    });

    // §1.6 自动获取满足需求的 Java（mc.javaAutoInstall）：
    // 触发顺序：①现有列表选取 → ②全盘重扫后再选 → ③官方清单(仅 windows-x64) → ④下载缺失包 → ⑤再选。
    bridge.RegisterAsync("mc.javaAutoInstall", [&bridge](const Json& params, const std::function<void(HandlerResult)>& done) {
        std::string mcVersion, loaderId;
        bool offline = false; int userMajor = 0;
        if (params.isObject() && params.contains("version") && params.at("version").isString())
            mcVersion = params.at("version").asString();
        if (params.isObject() && params.contains("loaderId") && params.at("loaderId").isString())
            loaderId = params.at("loaderId").asString();
        if (params.isObject() && params.contains("offline") && params.at("offline").isBool())
            offline = params.at("offline").asBool();
        if (params.isObject() && params.contains("userMajor") && params.at("userMajor").isNumber())
            userMajor = (int)params.at("userMajor").asNumber();
        if (mcVersion.empty()) { done(Err(-32602, "missing version")); return; }

        std::thread([mcVersion, loaderId, offline, userMajor, done, &bridge]() {
            try {
            int vjMajor = 0;
            {
                Json vj = ResolveVersionJson(GetMcRoot(), lxe::Utf8ToWide(mcVersion));
                vjMajor = JavaMajorFromVersionJson(vj, mcVersion);
            }
            JavaReq req = JavaRequirement(mcVersion, loaderId, offline, userMajor, vjMajor);
            // 区间内选择：先找主版本==推荐（优先 version.json 的 major），再找区间内任意项
            auto pickWithin = [&](const std::vector<InstalledJava>& javas) -> InstalledJava {
                InstalledJava best; best.major = 0;
                int rec = vjMajor > 0 ? vjMajor : RecommendedJavaMajor(mcVersion);
                for (const auto& j : javas) {
                    if (j.is64 && j.major >= req.lo && j.major <= req.hi) {
                        if (best.major == 0) best = j;
                        if (j.major == rec) { best = j; break; }
                    }
                }
                return best;
            };
            Json result = Json::object();
            // ①现有列表
            {
                InstalledJava j = pickWithin(ScanInstalledJavas());
                if (j.major > 0) {
                    result["ok"] = true; result["source"] = "installed";
                    result["javaPath"] = lxe::WideToUtf8(j.path); result["major"] = j.major;
                    done(Ok(result)); return;
                }
            }
            // ②全盘重扫
            {
                InstalledJava j = pickWithin(ScanInstalledJavasNow());
                if (j.major > 0) {
                    result["ok"] = true; result["source"] = "rescan";
                    result["javaPath"] = lxe::WideToUtf8(j.path); result["major"] = j.major;
                    done(Ok(result)); return;
                }
            }
            // ③④ 请求官方清单（仅 windows-x64）+ 下载缺失包：选区间内首选主版本（用户指定>version.json/major>推荐>区间下限）
            int want = userMajor > 0 ? userMajor : (req.lo == req.hi ? req.lo : (vjMajor > 0 ? vjMajor : RecommendedJavaMajor(mcVersion)));
            if (want < req.lo || want > req.hi) want = req.lo;
            std::wstring runtimeDir = g_exeDir + L"\\runtime\\java-" + std::to_wstring(want);
            std::error_code ec;
            std::filesystem::remove_all(runtimeDir, ec);
            std::filesystem::create_directories(runtimeDir, ec);
            // 官方清单：Adoptium 最新 GA 直链（windows x64 JRE）
            std::string pkgUrl = "https://api.adoptium.net/v3/binary/latest/" + std::to_string(want) +
                                 "/ga/windows/x64/jre/hotspot/normal/eclipse";
            std::wstring zipName = L"jre-" + std::to_wstring(want) + L".zip";
            // 并入统一下载任务列表（taskId=java-autoinstall，可取消、带进度）
            const std::string autoTaskId = "java-autoinstall";
            auto cancelFlag = DLCancelFlag(autoTaskId);
            {
                Json ev = Json::object(); ev["taskId"] = autoTaskId; ev["state"] = "started";
                ev["name"] = "自动安装 Java " + std::to_string(want);
                bridge.PostEvent("download.state", ev);
            }
            auto cb = [&](const Aria2Progress& p) -> bool {
                if (cancelFlag->load()) return false;
                Json ev = Json::object();
                ev["taskId"] = autoTaskId;
                ev["percent"] = p.percent;
                ev["speed"] = p.speed;
                ev["eta"] = p.eta;
                ev["stage"] = "下载 Java " + std::to_string(want) + " 运行时";
                ev["name"] = "自动安装 Java " + std::to_string(want);
                bridge.PostEvent("download.progress", ev);
                return true;
            };
            if (!DownloadFileSmart(lxe::Utf8ToWide(pkgUrl), runtimeDir, zipName, cb)) {
                DLCancelFlagRemove(autoTaskId);
                if (cancelFlag->load()) {
                    Json ev = Json::object(); ev["taskId"] = autoTaskId; ev["state"] = "cancelled";
                    bridge.PostEvent("download.state", ev);
                    done(Err(-32064, "自动安装 Java 已取消"));
                    return;
                }
                Json ev = Json::object(); ev["taskId"] = autoTaskId; ev["state"] = "error"; ev["error"] = "下载失败";
                bridge.PostEvent("download.state", ev);
                done(Err(-32060, "需要 Java " + std::to_string(req.lo) + (req.hi > req.lo ? " 至 " + std::to_string(req.hi) : "") +
                    "，自动下载失败：请联系网络或手动安装"));
                return;
            }
            if (cancelFlag->load()) {
                DLCancelFlagRemove(autoTaskId);
                std::filesystem::remove(runtimeDir + L"\\" + zipName, ec);
                Json ev = Json::object(); ev["taskId"] = autoTaskId; ev["state"] = "cancelled";
                bridge.PostEvent("download.state", ev);
                done(Err(-32064, "自动安装 Java 已取消"));
                return;
            }
            std::wstring zipPath = runtimeDir + L"\\" + zipName;
            std::wstring cmd = L"cmd /c tar -xf \"" + zipPath + L"\" -C \"" + runtimeDir + L"\"";
            if (RunProcessSilent(cmd, runtimeDir) != 0) {
                DLCancelFlagRemove(autoTaskId);
                std::filesystem::remove(zipPath, ec);
                Json ev = Json::object(); ev["taskId"] = autoTaskId; ev["state"] = "error"; ev["error"] = "解压失败";
                bridge.PostEvent("download.state", ev);
                done(Err(-32061, "下载的 Java 包解压失败"));
                return;
            }
            std::filesystem::remove(zipPath, ec);
            // ⑤再次选取
            {
                InstalledJava j = pickWithin(ScanInstalledJavasNow());
                if (j.major > 0) {
                    DLCancelFlagRemove(autoTaskId);
                    {
                        Json ev = Json::object(); ev["taskId"] = autoTaskId; ev["state"] = "done";
                        bridge.PostEvent("download.state", ev);
                    }
                    result["ok"] = true; result["source"] = "downloaded";
                    result["javaPath"] = lxe::WideToUtf8(j.path); result["major"] = j.major;
                    done(Ok(result)); return;
                }
            }
            DLCancelFlagRemove(autoTaskId);
            {
                Json ev = Json::object(); ev["taskId"] = autoTaskId; ev["state"] = "error"; ev["error"] = "安装后仍未匹配";
                bridge.PostEvent("download.state", ev);
            }
            done(Err(-32062, "需要 Java " + std::to_string(req.lo) + (req.hi > req.lo ? " 至 " + std::to_string(req.hi) : "") +
                "，自动安装后仍未匹配，请在 下载中心-Java 运行时 手动安装"));
            } catch (const std::exception& e) {
                done(Err(-32063, std::string("自动安装 Java 异常：") + e.what()));
            } catch (...) {
                done(Err(-32063, "自动安装 Java 未知异常"));
            }
        }).detach();
    });

    // 检测 MC 版本是否已安装（version/<id>/<id>.jar && version/<id>/<id>.json）
    bridge.Register("mc.versionIsInstalled", [](const Json& params) {
        std::string version;
        if (params.isObject() && params.contains("version") && params.at("version").isString())
            version = params.at("version").asString();
        Json result = Json::object();
        result["installed"] = false;
        result["jarExists"] = false;
        result["jsonExists"] = false;
        if (version.empty()) return Ok(result);
        std::wstring mcRoot = GetMcRoot();
        std::wstring wVer = lxe::Utf8ToWide(version);
        std::wstring verDir = mcRoot + L"\\versions\\" + wVer;
        std::wstring jarPath = verDir + L"\\" + wVer + L".jar";
        std::wstring jsonPath = verDir + L"\\" + wVer + L".json";
        bool jarExists = std::filesystem::exists(jarPath);
        bool jsonExists = std::filesystem::exists(jsonPath);
        result["installed"] = jarExists && jsonExists;
        result["jarExists"] = jarExists;
        result["jsonExists"] = jsonExists;
        result["versionDir"] = lxe::WideToUtf8(verDir);
        result["jarPath"] = lxe::WideToUtf8(jarPath);
        return Ok(result);
    });

    // 补全版本文件：校验并补下 客户端JAR / 依赖库 / natives / 资源文件 / 日志配置（后台线程，进度走下载队列）
    // 可传可选 name 覆盖显示名（安装流程用任务名，避免导航栏显示被顶替）
    bridge.Register("mc.completeVersion", [&bridge](const Json& params) {
        std::string version;
        if (params.isObject() && params.contains("version") && params.at("version").isString())
            version = params.at("version").asString();
        if (version.empty()) return Err(-32602, "missing version");
        std::string displayName = "补全文件 " + version;
        if (params.isObject() && params.contains("name") && params.at("name").isString() && !params.at("name").asString().empty())
            displayName = params.at("name").asString();
        static std::atomic<int> compSeq{50000};
        int taskId = ++compSeq;
        Json result = Json::object();
        result["taskId"] = std::to_string(taskId);
        result["started"] = true;
        result["version"] = version;

        std::thread([&bridge, taskId, version, displayName]() {
            CompleteVersionFilesWorker(bridge, taskId, version, displayName);
        }).detach();

        return Ok(result);
    });

    // 删除本地版本目录（卸载 MC 版本）
    bridge.Register("mc.deleteVersion", [](const Json& params) {
        std::string version;
        if (params.isObject() && params.contains("version") && params.at("version").isString())
            version = params.at("version").asString();
        if (version.empty()) return Err(-32602, "missing version");
        std::wstring mcRoot = GetMcRoot();
        std::wstring wVer = lxe::Utf8ToWide(version);
        std::wstring verDir = mcRoot + L"\\versions\\" + wVer;
        Json result = Json::object();
        if (!std::filesystem::exists(verDir)) {
            result["ok"] = false;
            result["reason"] = "not_installed";
            return Ok(result);
        }
        // 游戏运行中禁止删除
        {
            std::lock_guard<std::mutex> lock(g_gameInstancesMutex);
            if (!g_gameInstances.empty()) {
                // 允许删除但给出提示（这里不强制阻止）
            }
        }
        std::error_code ec;
        std::filesystem::remove_all(verDir, ec);
        result["ok"] = !ec;
        if (ec) result["error"] = ec.message();
        InvalidateLocalVersionsCache();
        return Ok(result);
    });

    // HTTP 请求：获取指定 URL 的文本（供前端获取 version.json / assetIndex / BMCLAPI 加载器列表等）
    // 异步执行：网络请求放后台线程，避免阻塞 WebView2 桥消息线程导致整个 UI 卡死
    bridge.RegisterAsync("http.fetchText", [](const Json& params, const std::function<void(HandlerResult)>& done) {
        std::wstring url;
        if (params.isObject() && params.contains("url") && params.at("url").isString())
            url = lxe::Utf8ToWide(params.at("url").asString());
        if (url.empty()) { done(Err(-32602, "missing url")); return; }
        std::thread([url, done]() {
            try {
                std::string text = HttpFetchText(url);
                if (text.empty()) { done(Err(-32040, "fetch failed")); return; }
                Json result = Json::object();
                result["text"] = text;
                result["bytes"] = (int)text.size();
                done(Ok(result));
            } catch (const std::exception& e) {
                done(Err(-32041, std::string("fetch exception: ") + e.what()));
            } catch (...) {
                done(Err(-32041, "fetch unknown exception"));
            }
        }).detach();
    });

    // 返回 MC 根目录及常用子目录（absolute path for frontend 拼接 outDir）
    bridge.Register("mc.paths", [](const Json&) {
        std::wstring mcRoot = GetMcRoot();
        Json r = Json::object();
        r["mcRoot"] = lxe::WideToUtf8(mcRoot);
        r["versionsDir"] = lxe::WideToUtf8(mcRoot + L"\\versions");
        r["librariesDir"] = lxe::WideToUtf8(mcRoot + L"\\libraries");
        r["assetsDir"] = lxe::WideToUtf8(mcRoot + L"\\assets");
        r["assetsObjectsDir"] = lxe::WideToUtf8(mcRoot + L"\\assets\\objects");
        r["assetsIndexesDir"] = lxe::WideToUtf8(mcRoot + L"\\assets\\indexes");
        r["nativesBaseDir"] = lxe::WideToUtf8(mcRoot + L"\\versions");
        if (!g_exeDir.empty()) r["runtimeDir"] = lxe::WideToUtf8(g_exeDir + L"\\runtime");
        return Ok(r);
    });
}

// ============ 第三方认证（外置登录 / authlib-injector） ============

// Base64 编码（用于 authlib-injector 元数据预取 prefetched 参数）
static std::string Base64Encode(const std::string& in) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    unsigned val = 0;
    int bits = -6;
    for (unsigned char c : in) {
        val = (val << 8) + c;
        bits += 8;
        while (bits >= 0) {
            out.push_back(tbl[(val >> bits) & 0x3F]);
            bits -= 6;
        }
    }
    if (bits > -6) out.push_back(tbl[((val << 8) >> (bits + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
}

struct HttpResp {
    bool ok = false;
    int status = 0;
    std::string body;
    std::string aliLocation; // X-Authlib-Injector-API-Location 响应头
};

// 通用 WinHTTP 请求（GET / POST / PUT / DELETE，可自定义 Content-Type 与请求头）。
// content: 请求体；extraHeaders 为额外的 "Header: value\r\n" 字符串。
// 供 MSA 设备码流程（表单体 + Bearer 头）、皮肤/披风 API 使用。
static HttpResp HttpRequestEx(const std::wstring& method, const std::wstring& url,
                              const std::string& contentType, const std::string& extraHeaders,
                              const std::string& body) {
    HttpResp resp;
    UrlParts parts;
    if (!ParseUrl(url, parts)) return resp;
    HINTERNET hSession = WinHttpOpen(L"LXElauncher/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return resp;
    WinHttpSetTimeouts(hSession, 10000, 10000, 10000, 30000);
    HINTERNET hConnect = WinHttpConnect(hSession, parts.host.c_str(), parts.port, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return resp; }
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, method.c_str(), parts.path.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        parts.https ? WINHTTP_FLAG_SECURE : 0);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return resp; }
    std::wstring hdrs = L"Accept: application/json\r\n";
    if (!contentType.empty()) hdrs += lxe::Utf8ToWide("Content-Type: " + contentType + "\r\n");
    if (!extraHeaders.empty()) hdrs += lxe::Utf8ToWide(extraHeaders);
    if (!hdrs.empty() && hdrs.back() != L'\n') hdrs += L"\r\n";
    DWORD bodyLen = (DWORD)body.size();
    BOOL bResult = WinHttpSendRequest(hRequest, hdrs.c_str(), (DWORD)hdrs.size(),
        body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(),
        bodyLen, bodyLen, 0);
    if (bResult) bResult = WinHttpReceiveResponse(hRequest, nullptr);
    if (bResult) {
        DWORD status = 0; DWORD statusSize = sizeof(status);
        if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX))
            resp.status = (int)status;
        std::string out;
        DWORD dwSize = 0;
        do {
            if (!WinHttpQueryDataAvailable(hRequest, &dwSize)) break;
            if (dwSize == 0) break;
            std::vector<char> buf(dwSize);
            DWORD dwDownloaded = 0;
            if (!WinHttpReadData(hRequest, buf.data(), dwSize, &dwDownloaded)) break;
            out.append(buf.data(), dwDownloaded);
        } while (dwSize > 0);
        resp.body = std::move(out);
        resp.ok = true;
    }
    WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
    return resp;
}

// 解析字符串形式的 JSON 字段（异常安全）
static std::string JsonStr(const Json& obj, const std::string& key, const std::string& fallback = "") {
    if (obj.isObject() && obj.contains(key) && obj.at(key).isString())
        return obj.at(key).asString();
    return fallback;
}

// ---------- MSA 微软账户设备码流程（§4.2） ----------
// 官方 Minecraft 启动器客户端 ID（公共客户端，用于设备码/刷新令牌流程）
static const std::string kMsaClientId = "00000000402b5328";
static const std::wstring kMsaDeviceUrl = L"https://login.microsoftonline.com/consumers/oauth2/v2.0/devicecode";
static const std::wstring kMsaTokenUrl = L"https://login.microsoftonline.com/consumers/oauth2/v2.0/token";
static const std::wstring kXblAuthUrl = L"https://user.auth.xboxlive.com/user/authenticate";
static const std::wstring kXstsAuthUrl = L"https://xsts.auth.xboxlive.com/xsts/authorize";
static const std::wstring kMcLoginUrl = L"https://api.minecraftservices.com/authentication/login_with_xbox";
static const std::wstring kMcProfileUrl = L"https://api.minecraftservices.com/minecraft/profile";
// LXE 登录服务器（正版登录协议，见 temp/lxe正版登录协议.md）
static const std::wstring kLxeLoginBase = L"111.838483.xyz";

struct MsaDeviceInfo {
    std::string deviceCode, userCode, verificationUri, message;
    int interval = 5, expiresIn = 900;
};

// 第 1 步：申请设备码
static bool MsaRequestDeviceCode(MsaDeviceInfo& out) {
    std::string form = "client_id=" + kMsaClientId + "&scope=" + UrlEncode("XboxLive.signin offline_access");
    HttpResp r = HttpRequestEx(L"POST", kMsaDeviceUrl, "application/x-www-form-urlencoded", "", form);
    if (!r.ok || r.status != 200) return false;
    Json j;
    try { j = Json::parse(r.body); } catch (...) { return false; }
    if (!j.isObject() || !j.contains("device_code")) return false;
    out.deviceCode = JsonStr(j, "device_code");
    out.userCode = JsonStr(j, "user_code");
    out.verificationUri = JsonStr(j, "verification_uri", "https://microsoft.com/link");
    out.message = JsonStr(j, "message");
    if (j.contains("interval") && j.at("interval").isNumber()) out.interval = (int)j.at("interval").asNumber();
    if (j.contains("expires_in") && j.at("expires_in").isNumber()) out.expiresIn = (int)j.at("expires_in").asNumber();
    return true;
}

// 第 2 步：轮询授权结果。返回"授权码"/刷新令牌（JSON），仍在等待则返回空。
static Json MsaPollToken(const std::string& deviceCode) {
    std::string form = "grant_type=" + UrlEncode("urn:ietf:params:oauth:grant-type:device_code") +
                       "&client_id=" + kMsaClientId + "&device_code=" + UrlEncode(deviceCode);
    HttpResp r = HttpRequestEx(L"POST", kMsaTokenUrl, "application/x-www-form-urlencoded", "", form);
    if (!r.ok) return Json();
    try {
        Json j = Json::parse(r.body);
        return j.isObject() ? j : Json();
    } catch (...) { return Json(); }
}

// 第 3 步：交换 Xbox Live 令牌
static bool MsaXblAuth(const std::string& msToken, std::string& xblToken) {
    Json req = Json::object();
    Json props = Json::object();
    props["AuthMethod"] = "RPS";
    props["SiteName"] = "user.auth.xboxlive.com";
    props["RpsTicket"] = "d=" + msToken;
    Json properties = Json::object();
    properties["Properties"] = props;
    properties["RelyingParty"] = "http://auth.xboxlive.com";
    properties["TokenType"] = "JWT";
    req = properties;
    HttpResp r = HttpRequestEx(L"POST", kXblAuthUrl, "application/json", "", req.dump());
    if (!r.ok) return false;
    Json j;
    try { j = Json::parse(r.body); } catch (...) { return false; }
    xblToken = JsonStr(j, "Token");
    return !xblToken.empty();
}

// 第 4 步：交换 XSTS 令牌（携 userhash）
static bool MsaXsts(const std::string& xblToken, std::string& xstsToken, std::string& userHash) {
    Json props = Json::object();
    Json properties = Json::object();
    properties["SandboxId"] = "RETAIL";
    Json tokens = Json::array();
    tokens.asArray().push_back(xblToken);
    properties["UserTokens"] = tokens;
    props["Properties"] = properties;
    props["RelyingParty"] = "rp://api.minecraftservices.com/";
    props["TokenType"] = "JWT";
    HttpResp r = HttpRequestEx(L"POST", kXstsAuthUrl, "application/json", "", props.dump());
    if (!r.ok) return false;
    Json j;
    try { j = Json::parse(r.body); } catch (...) { return false; }
    xstsToken = JsonStr(j, "Token");
    if (j.isObject() && j.contains("DisplayClaims") && j.at("DisplayClaims").isObject()) {
        const Json& dc = j.at("DisplayClaims");
        if (dc.contains("xui") && dc.at("xui").isArray() && dc.at("xui").size() > 0) {
            const Json& xui = dc.at("xui").asArray()[0];
            userHash = JsonStr(xui, "uhs");
        }
    }
    return !xstsToken.empty() && !userHash.empty();
}

// 第 5 步：Minecraft 登录，获得游戏访问令牌
static bool MsaMcLogin(const std::string& xstsToken, const std::string& userHash, std::string& mcToken) {
    Json req = Json::object();
    req["identityToken"] = "XBL3.0 x=" + userHash + ";" + xstsToken;
    HttpResp r = HttpRequestEx(L"POST", kMcLoginUrl, "application/json", "", req.dump());
    if (!r.ok) return false;
    Json j;
    try { j = Json::parse(r.body); } catch (...) { return false; }
    mcToken = JsonStr(j, "access_token");
    return !mcToken.empty();
}

// 第 6 步：请求用户 UUID / 用户名 / profile JSON（皮肤、披风）
static Json MsaGetProfile(const std::string& mcToken) {
    HttpResp r = HttpRequestEx(L"GET", kMcProfileUrl, "", "Authorization: Bearer " + mcToken + "\r\n", "");
    if (!r.ok) return Json();
    try {
        Json j = Json::parse(r.body);
        return j.isObject() ? j : Json();
    } catch (...) { return Json(); }
}

// 刷新微软令牌（Addendum I）
static Json MsaRefreshToken(const std::string& refreshToken) {
    std::string form = "grant_type=refresh_token&client_id=" + kMsaClientId +
                       "&scope=" + UrlEncode("XboxLive.signin offline_access") +
                       "&refresh_token=" + UrlEncode(refreshToken);
    HttpResp r = HttpRequestEx(L"POST", kMsaTokenUrl, "application/x-www-form-urlencoded", "", form);
    if (!r.ok) return Json();
    try {
        Json j = Json::parse(r.body);
        return j.isObject() ? j : Json();
    } catch (...) { return Json(); }
}

// 完整登录链：微软令牌 → 游戏令牌 → profile。成功返回 profile JSON。
static Json MsaCompleteLogin(const std::string& msToken) {
    std::string xbl, xsts, uhs, mcToken;
    if (!MsaXblAuth(msToken, xbl)) return Json();
    if (!MsaXsts(xbl, xsts, uhs)) return Json();
    if (!MsaMcLogin(xsts, uhs, mcToken)) return Json();
    return MsaGetProfile(mcToken);
}

// ---------- LXE 正版登录（游客模式，temp/lxe正版登录协议.md） ----------
// 第 1 步：发起设备码登录。guest 模式无需 sungbly 证书。
static bool LxeRequestDeviceCode(MsaDeviceInfo& out) {
    Json req = Json::object();
    req["type"] = "microsoft";
    req["mode"] = "guest";
    req["port"] = 25565;
    HttpResp r = HttpRequestEx(L"POST", kLxeLoginBase + L"/auth/login", "application/json", "", req.dump());
    if (!r.ok || r.status != 200) return false;
    Json j;
    try { j = Json::parse(r.body); } catch (...) { return false; }
    if (!j.isObject() || !j.contains("device_code")) return false;
    out.deviceCode = JsonStr(j, "device_code");
    out.userCode = JsonStr(j, "user_code");
    out.verificationUri = JsonStr(j, "verification_uri", "https://microsoft.com/devicelogin");
    out.message = JsonStr(j, "message");
    if (j.contains("interval") && j.at("interval").isNumber()) out.interval = (int)j.at("interval").asNumber();
    if (j.contains("expires_in") && j.at("expires_in").isNumber()) out.expiresIn = (int)j.at("expires_in").asNumber();
    return true;
}

// 第 2 步：轮询登录状态。返回响应 JSON（guest 完成时含 minecraft_token）。
static Json LxePollLogin(const std::string& deviceCode) {
    std::wstring url = kLxeLoginBase + L"/auth/poll?device_code=" + lxe::Utf8ToWide(UrlEncode(deviceCode));
    HttpResp r = HttpRequestEx(L"GET", url, "", "", "");
    if (!r.ok) return Json();
    try {
        Json j = Json::parse(r.body);
        return j.isObject() ? j : Json();
    } catch (...) { return Json(); }
}

// 披风 id 映射为可读中文名称（§4.4），未知 id 保持原样
static std::string CapeDisplayName(const std::string& id) {
    static const std::vector<std::pair<std::string, std::string>> known = {
        {"capes-founders", "创始者披风"}, {"capes-coaster", "过山车披风"},
        {"capes-rainbow", "彩虹披风"}, {"capes-monochrome", "单色披风"},
        {"capes-mincer", "青翠披风"}, {"capes-dannybstyle", "DannyB 披风"},
    };
    for (const auto& p : known) if (id == p.first) return p.second;
    return id;
}

// 通用 WinHTTP 请求（GET / POST，JSON 体），读取 ALI 响应头用于 API Root 重定向
static HttpResp HttpRequest(const std::wstring& method, const std::wstring& url,
                            const std::string& body) {
    HttpResp resp;
    UrlParts parts;
    if (!ParseUrl(url, parts)) return resp;
    HINTERNET hSession = WinHttpOpen(L"LXElauncher/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return resp;
    WinHttpSetTimeouts(hSession, 5000, 5000, 5000, 10000);
    HINTERNET hConnect = WinHttpConnect(hSession, parts.host.c_str(), parts.port, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return resp; }
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, method.c_str(), parts.path.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        parts.https ? WINHTTP_FLAG_SECURE : 0);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return resp; }
    const wchar_t* hdrs = L"Content-Type: application/json\r\nAccept: application/json\r\n";
    DWORD bodyLen = (DWORD)body.size();
    BOOL bResult = WinHttpSendRequest(hRequest, hdrs, (DWORD)wcslen(hdrs),
        body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(),
        bodyLen, bodyLen, 0);
    if (bResult) bResult = WinHttpReceiveResponse(hRequest, nullptr);
    if (bResult) {
        DWORD status = 0; DWORD statusSize = sizeof(status);
        if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX))
            resp.status = (int)status;
        DWORD len = 0;
        if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_CUSTOM, L"X-Authlib-Injector-API-Location",
                WINHTTP_NO_OUTPUT_BUFFER, &len, WINHTTP_NO_HEADER_INDEX)) {
            std::wstring val(len / sizeof(wchar_t) + 1, L'\0');
            if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_CUSTOM, L"X-Authlib-Injector-API-Location",
                    &val[0], &len, WINHTTP_NO_HEADER_INDEX)) {
                resp.aliLocation = lxe::WideToUtf8(val.c_str());
            }
        }
        std::string out;
        DWORD dwSize = 0;
        do {
            if (!WinHttpQueryDataAvailable(hRequest, &dwSize)) break;
            if (dwSize == 0) break;
            std::vector<char> buf(dwSize);
            DWORD dwDownloaded = 0;
            if (!WinHttpReadData(hRequest, buf.data(), dwSize, &dwDownloaded)) break;
            out.append(buf.data(), dwDownloaded);
        } while (dwSize > 0);
        resp.body = std::move(out);
        resp.ok = true;
    }
    WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
    return resp;
}

// 规范：拖拽数据中的 API 地址是 encodeURIComponent 编码过的（见"通过拖拽设置"），先解码
static std::string UrlDecode(const std::string& s) {
    auto hexVal = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int h = hexVal(s[i + 1]), l = hexVal(s[i + 2]);
            if (h >= 0 && l >= 0) {
                out.push_back(static_cast<char>((h << 4) | l));
                i += 2;
                continue;
            }
        }
        out.push_back(s[i]);
    }
    return out;
}

// 规范：ALI 头（X-Authlib-Injector-API-Location）既可以是绝对 URL，也可以是相对 URL
//（如 /api/yggdrasil/），需基于当前 URL 解析成绝对地址。
static std::string ResolveAliUrl(const std::string& base, const std::string& location) {
    if (location.rfind("http://", 0) == 0 || location.rfind("https://", 0) == 0)
        return location;
    std::string prefix = (base.rfind("https://", 0) == 0) ? "https://" : "http://";
    size_t hostEnd = base.find('/', prefix.size());
    if (hostEnd == std::string::npos)
        return prefix + base.substr(prefix.size()) + location; // http://host + "/api..."
    if (location.rfind('/', 0) == 0)
        return prefix + base.substr(prefix.size(), hostEnd - prefix.size()) + location;
    // 相对路径：取 base 目录
    size_t lastSlash = base.rfind('/');
    if (lastSlash == std::string::npos || lastSlash < prefix.size())
        return prefix + base.substr(prefix.size(), hostEnd - prefix.size()) + "/" + location;
    return base.substr(0, lastSlash + 1) + location;
}

// 规范化认证服务器地址：剥去 authlib-injector:yggdrasil-server: 前缀、URL 解码、补全协议并统一末尾斜杠
static std::string NormalizeServerInput(const std::string& input) {
    const std::string prefix = "authlib-injector:yggdrasil-server:";
    std::string s = input;
    if (s.rfind(prefix, 0) == 0) s = s.substr(prefix.size());
    s = UrlDecode(s);
    if (s.rfind("https://", 0) != 0 && s.rfind("http://", 0) != 0) s = "https://" + s;
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    s += "/";
    return s;
}

// 探测认证服务器：请求 API Root → 跟随 ALI 头（可相对、可指向自身）→ 获取元数据（serverName）
static bool ProbeAuthServer(const std::string& serverInput, std::string& apiRootOut,
                            std::string& serverNameOut, std::string& metaJsonOut) {
    std::string apiRoot = NormalizeServerInput(serverInput);
    // 规范：GET（跟随重定向）→ 若带 ALI 头且指向非当前 URL，则以其为新地址继续；否则当前 URL 即 API 地址
    for (int i = 0; i < 5; ++i) {
        HttpResp r = HttpRequest(L"GET", lxe::Utf8ToWide(apiRoot), "");
        if (!r.ok || r.body.empty()) return false;
        if (r.aliLocation.empty()) {
            metaJsonOut = r.body;
            break;
        }
        std::string newUrl = ResolveAliUrl(apiRoot, r.aliLocation);
        newUrl = NormalizeServerInput(newUrl);
        if (newUrl == apiRoot) { metaJsonOut = r.body; break; }
        apiRoot = newUrl;
    }
    if (metaJsonOut.empty()) return false;
    std::string serverName;
    try {
        Json meta = Json::parse(metaJsonOut);
        if (meta.isObject() && meta.contains("meta") && meta.at("meta").isObject() &&
            meta.at("meta").contains("serverName") && meta.at("meta").at("serverName").isString())
            serverName = meta.at("meta").at("serverName").asString();
    } catch (...) {}
    apiRootOut = apiRoot;
    serverNameOut = serverName.empty() ? apiRoot : serverName;
    return true;
}

// 解析 Yggdrasil authenticate/refresh 响应中的游戏角色（profile）信息
static bool ParseYggdrasilProfile(const Json& res, std::string& profileId, std::string& playerName) {
    if (res.isObject() && res.contains("selectedProfile") && res.at("selectedProfile").isObject()) {
        const Json& sp = res.at("selectedProfile");
        if (sp.contains("id") && sp.at("id").isString()) profileId = sp.at("id").asString();
        if (sp.contains("name") && sp.at("name").isString()) playerName = sp.at("name").asString();
    }
    if (profileId.empty() && res.isObject() && res.contains("availableProfiles") &&
        res.at("availableProfiles").isArray() && res.at("availableProfiles").size() > 0) {
        const Json& sp = res.at("availableProfiles").asArray()[0];
        if (sp.isObject()) {
            if (sp.contains("id") && sp.at("id").isString()) profileId = sp.at("id").asString();
            if (sp.contains("name") && sp.at("name").isString()) playerName = sp.at("name").asString();
        }
    }
    return !profileId.empty() && !playerName.empty();
}

void RegisterAuthServices(Bridge& bridge) {
    // 探测认证服务器：返回 API Root / 服务器名 / 预取元数据（Base64，用于 prefetched 参数）
    bridge.Register("auth.probe", [](const Json& params) {
        std::string server;
        if (params.isObject() && params.contains("server") && params.at("server").isString())
            server = params.at("server").asString();
        if (server.empty()) return Err(-32602, "missing server");
        std::string apiRoot, serverName, metaJson;
        if (!ProbeAuthServer(server, apiRoot, serverName, metaJson))
            return Err(-32050, "无法连接认证服务器或响应无效: " + server);
        Json result = Json::object();
        result["apiRoot"] = apiRoot;
        result["serverName"] = serverName;
        result["prefetched"] = Base64Encode(metaJson);
        return Ok(result);
    });

    // 第三方认证登录（Yggdrasil authenticate）
    bridge.Register("auth.login", [](const Json& params) {
        std::string server, username, password;
        if (params.isObject() && params.contains("server") && params.at("server").isString())
            server = params.at("server").asString();
        if (params.isObject() && params.contains("username") && params.at("username").isString())
            username = params.at("username").asString();
        if (params.isObject() && params.contains("password") && params.at("password").isString())
            password = params.at("password").asString();
        if (server.empty() || username.empty() || password.empty())
            return Err(-32602, "server/username/password required");

        std::string apiRoot, serverName, metaJson;
        if (!ProbeAuthServer(server, apiRoot, serverName, metaJson))
            return Err(-32050, "无法连接认证服务器或响应无效: " + server);

        Json req = Json::object();
        Json agent = Json::object();
        agent["name"] = "Minecraft";
        agent["version"] = 1;
        req["agent"] = agent;
        req["username"] = username;
        req["password"] = password;
        req["requestUser"] = true;
        HttpResp r = HttpRequest(L"POST", lxe::Utf8ToWide(apiRoot) + L"authserver/authenticate", req.dump());
        if (!r.ok) return Err(-32051, "认证请求失败，请检查网络或服务器地址");
        Json res;
        try { res = Json::parse(r.body); }
        catch (...) { return Err(-32052, "认证响应解析失败"); }
        if (res.isObject() && res.contains("error")) {
            std::string em = res.contains("errorMessage") && res.at("errorMessage").isString()
                ? res.at("errorMessage").asString() : res.at("error").asString();
            return Err(-32053, em.empty() ? "认证失败" : em);
        }
        if (!res.isObject() || !res.contains("accessToken") || !res.at("accessToken").isString())
            return Err(-32054, "认证响应缺少 accessToken");
        std::string accessToken = res.at("accessToken").asString();
        std::string clientToken = res.contains("clientToken") && res.at("clientToken").isString()
            ? res.at("clientToken").asString() : "";
        std::string profileId, playerName;
        if (!ParseYggdrasilProfile(res, profileId, playerName))
            return Err(-32055, "该账号没有任何可用的游戏角色（profile）");

        Json result = Json::object();
        result["apiRoot"] = apiRoot;
        result["serverName"] = serverName;
        result["prefetched"] = Base64Encode(metaJson);
        result["accessToken"] = accessToken;
        result["clientToken"] = clientToken;
        result["uuid"] = profileId;
        result["playerName"] = playerName;
        return Ok(result);
    });

    // 刷新令牌（Yggdrasil refresh）
    bridge.Register("auth.refresh", [](const Json& params) {
        std::string server, accessToken, clientToken;
        if (params.isObject() && params.contains("server") && params.at("server").isString())
            server = params.at("server").asString();
        if (params.isObject() && params.contains("accessToken") && params.at("accessToken").isString())
            accessToken = params.at("accessToken").asString();
        if (params.isObject() && params.contains("clientToken") && params.at("clientToken").isString())
            clientToken = params.at("clientToken").asString();
        if (server.empty() || accessToken.empty()) return Err(-32602, "server/accessToken required");

        std::string apiRoot, serverName, metaJson;
        if (!ProbeAuthServer(server, apiRoot, serverName, metaJson))
            return Err(-32050, "无法连接认证服务器: " + server);

        Json req = Json::object();
        req["accessToken"] = accessToken;
        if (!clientToken.empty()) req["clientToken"] = clientToken;
        HttpResp r = HttpRequest(L"POST", lxe::Utf8ToWide(apiRoot) + L"authserver/refresh", req.dump());
        if (!r.ok) return Err(-32051, "刷新请求失败");
        Json res;
        try { res = Json::parse(r.body); }
        catch (...) { return Err(-32052, "刷新响应解析失败"); }
        if (res.isObject() && res.contains("error")) {
            std::string em = res.contains("errorMessage") && res.at("errorMessage").isString()
                ? res.at("errorMessage").asString() : res.at("error").asString();
            return Err(-32053, em.empty() ? "刷新失败" : em);
        }
        if (!res.isObject() || !res.contains("accessToken") || !res.at("accessToken").isString())
            return Err(-32054, "刷新响应缺少 accessToken");
        std::string profileId, playerName;
        ParseYggdrasilProfile(res, profileId, playerName);
        Json result = Json::object();
        result["accessToken"] = res.at("accessToken").asString();
        if (!profileId.empty()) result["uuid"] = profileId;
        if (!playerName.empty()) result["playerName"] = playerName;
        return Ok(result);
    });

    // 验证令牌（Yggdrasil validate）：
    // 规范"凭证有效性的确认"第一步；204 表示有效，403/错误 JSON 表示失效。
    bridge.Register("auth.validate", [](const Json& params) {
        std::string server, accessToken, clientToken;
        if (params.isObject() && params.contains("server") && params.at("server").isString())
            server = params.at("server").asString();
        if (params.isObject() && params.contains("accessToken") && params.at("accessToken").isString())
            accessToken = params.at("accessToken").asString();
        if (params.isObject() && params.contains("clientToken") && params.at("clientToken").isString())
            clientToken = params.at("clientToken").asString();
        if (server.empty() || accessToken.empty()) return Err(-32602, "server/accessToken required");

        std::string apiRoot, serverName, metaJson;
        if (!ProbeAuthServer(server, apiRoot, serverName, metaJson))
            return Err(-32050, "无法连接认证服务器: " + server);

        Json req = Json::object();
        req["accessToken"] = accessToken;
        if (!clientToken.empty()) req["clientToken"] = clientToken;
        HttpResp r = HttpRequest(L"POST", lxe::Utf8ToWide(apiRoot) + L"authserver/validate", req.dump());
        if (!r.ok) return Err(-32051, "验证请求失败");
        if (r.status == 204) { Json result = Json::object(); result["valid"] = true; return Ok(result); }
        if (!r.body.empty()) {
            Json res;
            try { res = Json::parse(r.body); }
            catch (...) { return Err(-32052, "验证响应解析失败"); }
            if (res.isObject() && res.contains("error")) {
                std::string em = res.contains("errorMessage") && res.at("errorMessage").isString()
                    ? res.at("errorMessage").asString() : res.at("error").asString();
                return Err(-32053, em.empty() ? "令牌已失效" : em);
            }
        }
        Json result = Json::object();
        result["valid"] = false;
        return Ok(result);
    });

    // authlib-injector.jar 路径（位于 MC 文件夹）
    bridge.Register("auth.injectorPath", [](const Json&) {
        std::wstring jarPath = GetMcRoot() + L"\\authlib-injector.jar";
        Json result = Json::object();
        result["path"] = lxe::WideToUtf8(jarPath);
        result["exists"] = std::filesystem::exists(jarPath);
        return Ok(result);
    });

    // 确保 authlib-injector.jar 存在（缺失时从 BMCLAPI 镜像下载到 MC 文件夹，并入统一下载任务列表，可取消）
    bridge.RegisterAsync("auth.ensureInjector", [&bridge](const Json&, const std::function<void(HandlerResult)>& done) {
        std::wstring mcRoot = GetMcRoot();
        std::wstring jarPath = mcRoot + L"\\authlib-injector.jar";
        Json result = Json::object();
        result["path"] = lxe::WideToUtf8(jarPath);
        result["exists"] = std::filesystem::exists(jarPath);
        if (std::filesystem::exists(jarPath)) { result["downloaded"] = false; done(Ok(result)); return; }
        const std::string taskId = "auth-injector";
        auto cancelFlag = DLCancelFlag(taskId);
        {
            Json ev = Json::object(); ev["taskId"] = taskId; ev["state"] = "started"; ev["name"] = "authlib-injector";
            bridge.PostEvent("download.state", ev);
        }
        std::thread([mcRoot, jarPath, cancelFlag, taskId, &bridge, done, result]() mutable {
            auto cb = [&](const Aria2Progress& p) -> bool {
                if (cancelFlag->load()) return false;
                Json ev = Json::object();
                ev["taskId"] = taskId;
                ev["percent"] = p.percent;
                ev["speed"] = p.speed;
                ev["eta"] = p.eta;
                ev["stage"] = "下载 authlib-injector";
                ev["name"] = "authlib-injector";
                bridge.PostEvent("download.progress", ev);
                return true;
            };
            bool ok = DownloadAuthlibInjector(mcRoot, L"authlib-injector.jar", cb);
            DLCancelFlagRemove(taskId);
            if (!ok) {
                Json ev = Json::object(); ev["taskId"] = taskId;
                if (cancelFlag->load()) {
                    ev["state"] = "cancelled";
                    bridge.PostEvent("download.state", ev);
                    result["downloaded"] = false; result["cancelled"] = true;
                    done(Ok(result));
                } else {
                    ev["state"] = "error"; ev["error"] = "下载失败";
                    bridge.PostEvent("download.state", ev);
                    done(Err(-32056, "authlib-injector 下载失败"));
                }
                return;
            }
            {
                Json ev = Json::object(); ev["taskId"] = taskId; ev["state"] = "done";
                bridge.PostEvent("download.state", ev);
            }
            result["downloaded"] = true;
            result["exists"] = std::filesystem::exists(jarPath);
            done(Ok(result));
        }).detach();
    });

    // ===================== §4 MSA 微软账户（设备码流程） =====================
    // 外部打开浏览器（供设备码授权页跳转）
    bridge.Register("shell.openUrl", [](const Json& params) {
        std::string url;
        if (params.isObject() && params.contains("url") && params.at("url").isString())
            url = params.at("url").asString();
        if (url.empty()) return Err(-32602, "missing url");
        ShellExecuteW(nullptr, L"open", lxe::Utf8ToWide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        Json r = Json::object(); r["ok"] = true; return Ok(r);
    });

    // 第 1 步：申请设备码（用户需在浏览器打开验证链接输入 user_code，游客模式）
    bridge.RegisterAsync("msa.requestCode", [](const Json&, const std::function<void(HandlerResult)>& done) {
        std::thread([done]() {
            MsaDeviceInfo info;
            if (!LxeRequestDeviceCode(info)) { done(Err(-32080, "申请登录设备码失败，请检查网络")); return; }
            Json r = Json::object();
            r["deviceCode"] = info.deviceCode;
            r["userCode"] = info.userCode;
            r["verificationUri"] = info.verificationUri;
            r["message"] = info.message;
            r["interval"] = info.interval;
            r["expiresIn"] = info.expiresIn;
            done(Ok(r));
        }).detach();
    });

    // 第 2~6 步：轮询授权并完成登录链。deviceCode 仍在等待时返回 status=pending。
    bridge.RegisterAsync("msa.completeLogin", [](const Json& params, const std::function<void(HandlerResult)>& done) {
        std::string deviceCode;
        if (params.isObject() && params.contains("deviceCode") && params.at("deviceCode").isString())
            deviceCode = params.at("deviceCode").asString();
        if (deviceCode.empty()) { done(Err(-32602, "missing deviceCode")); return; }
        std::thread([deviceCode, done]() {
            Json tok = LxePollLogin(deviceCode);
            if (!tok.isObject()) { done(Err(-32081, "轮询登录状态失败，请检查网络")); return; }
            std::string status = JsonStr(tok, "status");
            if (status == "pending") { Json r = Json::object(); r["status"] = "pending"; done(Ok(r)); return; }
            std::string err = JsonStr(tok, "error");
            if (!err.empty()) {
                if (err == "authorization_pending") { Json r = Json::object(); r["status"] = "pending"; done(Ok(r)); return; }
                if (err == "authorization_declined") { done(Err(-32082, "您已拒绝授权")); return; }
                if (err == "expired_token") { done(Err(-32083, "授权已过期，请重新开始")); return; }
                done(Err(-32084, "登录失败：" + err)); return;
            }
            // 新协议（temp/lxe正版登录协议.md）：poll 完成后返回的是微软 access_token/refresh_token，
            // 需由启动器自行完成 Xbox→Minecraft 令牌交换；兼容旧协议直接返回 minecraft_token 的情况。
            std::string mcToken = JsonStr(tok, "minecraft_token");
            std::string msToken = JsonStr(tok, "access_token");
            std::string refreshToken = JsonStr(tok, "refresh_token");
            Json profile;
            if (!mcToken.empty()) {
                profile = MsaGetProfile(mcToken);
            } else if (!msToken.empty()) {
                std::string xbl, xsts, uhs;
                if (!MsaXblAuth(msToken, xbl) || !MsaXsts(xbl, xsts, uhs) || !MsaMcLogin(xsts, uhs, mcToken)) {
                    done(Err(-32085, "登录响应无效，未获取到游戏令牌")); return;
                }
                profile = MsaGetProfile(mcToken);
            }
            if (mcToken.empty()) { done(Err(-32085, "登录响应无效，未获取到游戏令牌")); return; }
            if (!profile.isObject() || !profile.contains("id") || !profile.contains("name")) {
                done(Err(-32086, "Minecraft 登录失败：该账号可能没有购买 Minecraft 或获批访问权限"));
                return;
            }
            std::string uuid = JsonStr(profile, "id");
            std::string name = JsonStr(profile, "name");
            Json r = Json::object();
            r["status"] = "done";
            r["mode"] = JsonStr(tok, "mode", "guest");
            r["uuid"] = uuid;
            r["playerName"] = name;
            r["accountName"] = name + " (Microsoft)";
            r["accessToken"] = mcToken;
            // refresh_token 作为本地保存的凭证，供启动时自动刷新会话（保证令牌不过期）
            r["refreshToken"] = refreshToken;
            r["profileJson"] = profile.dump();
            r["capes"] = profile.contains("capes") ? profile.at("capes") : Json::array();
            int64_t expIn = 86400;
            if (tok.contains("expires_in") && tok.at("expires_in").isNumber())
                expIn = (int64_t)tok.at("expires_in").asNumber();
            else if (tok.contains("microsoft_expires_in") && tok.at("microsoft_expires_in").isNumber())
                expIn = (int64_t)tok.at("microsoft_expires_in").asNumber();
            r["expiresIn"] = expIn;
            if (tok.contains("microsoft_expires_in") && tok.at("microsoft_expires_in").isNumber())
                r["microsoftExpiresIn"] = (int64_t)tok.at("microsoft_expires_in").asNumber();
            done(Ok(r));
        }).detach();
    });

    // 刷新微软令牌 + 重走登录链（保持 profile 内 access_token 仅当前会话有效，前端持久化 refreshToken）
    bridge.RegisterAsync("msa.refresh", [](const Json& params, const std::function<void(HandlerResult)>& done) {
        std::string refreshToken;
        if (params.isObject() && params.contains("refreshToken") && params.at("refreshToken").isString())
            refreshToken = params.at("refreshToken").asString();
        if (refreshToken.empty()) { done(Err(-32602, "missing refreshToken")); return; }
        std::thread([refreshToken, done]() {
            Json tok = MsaRefreshToken(refreshToken);
            if (!tok.isObject() || !tok.contains("access_token")) {
                done(Err(-32088, "刷新微软令牌失败，请重新登录"));
                return;
            }
            std::string msToken = JsonStr(tok, "access_token");
            std::string newRefresh = JsonStr(tok, "refresh_token");
            if (newRefresh.empty()) newRefresh = refreshToken;
            std::string mcToken;
            Json profile;
            {
                std::string xbl, xsts, uhs;
                if (!MsaXblAuth(msToken, xbl) || !MsaXsts(xbl, xsts, uhs) || !MsaMcLogin(xsts, uhs, mcToken)) {
                    done(Err(-32089, "刷新后 Minecraft 登录失败，请重新登录"));
                    return;
                }
                profile = MsaGetProfile(mcToken);
            }
            if (!profile.isObject() || !profile.contains("id") || !profile.contains("name")) {
                done(Err(-32090, "刷新后获取用户资料失败"));
                return;
            }
            Json r = Json::object();
            r["status"] = "done";
            r["uuid"] = JsonStr(profile, "id");
            r["playerName"] = JsonStr(profile, "name");
            r["accessToken"] = mcToken;
            r["refreshToken"] = newRefresh;
            r["profileJson"] = profile.dump();
            r["capes"] = profile.contains("capes") ? profile.at("capes") : Json::array();
            done(Ok(r));
        }).detach();
    });

    // 获取当前会员资料（含皮肤/披风列表）
    bridge.Register("msa.profile", [](const Json& params) {
        std::string accessToken;
        if (params.isObject() && params.contains("accessToken") && params.at("accessToken").isString())
            accessToken = params.at("accessToken").asString();
        if (accessToken.empty()) return Err(-32602, "missing accessToken");
        Json profile = MsaGetProfile(accessToken);
        if (!profile.isObject()) return Err(-32091, "获取用户资料失败");
        if (profile.contains("error") && profile.at("error").isString())
            return Err(-32092, JsonStr(profile, "errorMessage", JsonStr(profile, "error")));
        Json r = Json::object();
        r["id"] = JsonStr(profile, "id");
        r["name"] = JsonStr(profile, "name");
        r["skins"] = profile.contains("skins") ? profile.at("skins") : Json::array();
        r["capes"] = profile.contains("capes") ? profile.at("capes") : Json::array();
        r["profileJson"] = profile.dump();
        return Ok(r);
    });

    // §4.3 皮肤上传：multipart PUT skins 端点，model∈{classic,slim}
    bridge.Register("msa.skin", [](const Json& params) {
        std::string accessToken, model, filePath;
        if (params.isObject() && params.contains("accessToken") && params.at("accessToken").isString())
            accessToken = params.at("accessToken").asString();
        if (params.isObject() && params.contains("model") && params.at("model").isString())
            model = params.at("model").asString();
        if (params.isObject() && params.contains("path") && params.at("path").isString())
            filePath = params.at("path").asString();
        if (accessToken.empty()) return Err(-32602, "missing accessToken");
        if (model != "classic" && model != "slim") model = "classic";
        if (filePath.empty()) return Err(-32602, "missing path");
        std::wstring wPath = lxe::Utf8ToWide(filePath);
        std::ifstream f(wPath, std::ios::binary);
        if (!f) return Err(-32093, "无法读取皮肤文件");
        std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (data.empty()) return Err(-32094, "皮肤文件为空");
        // 构造 multipart/form-data（§4.3：文件 + 模型类型）
        std::string boundary = "----LXElauncher" + std::to_string(GetTickCount64());
        std::string body;
        body += "--" + boundary + "\r\n";
        body += "Content-Disposition: form-data; name=\"variant\"\r\n\r\n";
        body += model + "\r\n";
        body += "--" + boundary + "\r\n";
        body += "Content-Disposition: form-data; name=\"file\"; filename=\"skin.png\"\r\n";
        body += "Content-Type: image/png\r\n\r\n";
        body += data;
        body += "\r\n--" + boundary + "--\r\n";
        std::string contentType = "multipart/form-data; boundary=" + boundary;
        HttpResp r = HttpRequestEx(L"PUT", L"https://api.minecraftservices.com/minecraft/profile/skins",
                                   contentType, "Authorization: Bearer " + accessToken + "\r\n", body);
        if (!r.ok) return Err(-32095, "皮肤上传请求失败");
        if (r.status == 401) return Err(-32096, "登录已失效，请重新登录");
        if (r.status != 200 && r.status != 204) {
            std::string msg = "皮肤上传失败（HTTP " + std::to_string(r.status) + "）";
            if (!r.body.empty()) {
                try { Json j = Json::parse(r.body); msg = JsonStr(j, "errorMessage", msg); } catch (...) {}
            }
            return Err(-32097, msg);
        }
        // 上传成功后立即刷新缓存 profile（§4.3）
        Json profile = MsaGetProfile(accessToken);
        Json result = Json::object();
        result["ok"] = true;
        result["profileJson"] = profile.isObject() ? profile.dump() : "";
        result["skins"] = profile.isObject() && profile.contains("skins") ? profile.at("skins") : Json::array();
        return Ok(result);
    });

    // §4.4 披风列表：读取缓存 profile 的 capes，映射可读中文名称
    bridge.Register("msa.capes", [](const Json& params) {
        std::string accessToken;
        if (params.isObject() && params.contains("accessToken") && params.at("accessToken").isString())
            accessToken = params.at("accessToken").asString();
        if (accessToken.empty()) return Err(-32602, "missing accessToken");
        Json profile = MsaGetProfile(accessToken);
        if (!profile.isObject()) return Err(-32091, "获取用户资料失败");
        Json arr = Json::array();
        if (profile.contains("capes") && profile.at("capes").isArray()) {
            for (const auto& c : profile.at("capes").asArray()) {
                if (!c.isObject()) continue;
                Json o = Json::object();
                std::string id = JsonStr(c, "id");
                std::string state = JsonStr(c, "state");
                o["id"] = id;
                o["name"] = CapeDisplayName(id);
                o["active"] = (state == "ACTIVE");
                o["url"] = JsonStr(c, "url");
                arr.asArray().push_back(o);
            }
        }
        Json r = Json::object();
        r["capes"] = arr;
        r["profileJson"] = profile.dump();
        return Ok(r);
    });

    // §4.4 披风启用/移除：capeId 为空表示移除
    bridge.RegisterAsync("msa.capeActive", [](const Json& params, const std::function<void(HandlerResult)>& done) {
        std::string accessToken, capeId;
        if (params.isObject() && params.contains("accessToken") && params.at("accessToken").isString())
            accessToken = params.at("accessToken").asString();
        if (params.isObject() && params.contains("capeId") && params.at("capeId").isString())
            capeId = params.at("capeId").asString();
        if (accessToken.empty()) { done(Err(-32602, "missing accessToken")); return; }
        std::thread([accessToken, capeId, done]() {
            std::string method = capeId.empty() ? "DELETE" : "PUT";
            std::string body;
            if (!capeId.empty()) {
                Json b = Json::object();
                b["capeId"] = capeId;
                body = b.dump();
            }
            HttpResp r = HttpRequestEx(lxe::Utf8ToWide(method),
                                       L"https://api.minecraftservices.com/minecraft/profile/capes/active",
                                       "application/json", "Authorization: Bearer " + accessToken + "\r\n", body);
            if (!r.ok) { done(Err(-32098, "披风请求失败")); return; }
            if (r.status == 401) { done(Err(-32096, "登录已失效，请重新登录")); return; }
            if (r.status != 200 && r.status != 204) {
                std::string msg = "披风设置失败（HTTP " + std::to_string(r.status) + "）";
                if (!r.body.empty()) {
                    try { Json j = Json::parse(r.body); msg = JsonStr(j, "errorMessage", msg); } catch (...) {}
                }
                done(Err(-32099, msg)); return;
            }
            // 成功后本地重取 profile 并持久化由前端处理
            Json profile = MsaGetProfile(accessToken);
            Json result = Json::object();
            result["ok"] = true;
            result["profileJson"] = profile.isObject() ? profile.dump() : "";
            done(Ok(result));
        }).detach();
    });
}

// ============ MC 文件夹列表管理 ============
#define MCFOLDERS_REG_KEY L"Software\\LXElauncher\\McFolders"

static std::vector<std::pair<std::wstring, std::wstring>> RegEnumMcFolders() {
    std::vector<std::pair<std::wstring, std::wstring>> result;
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, MCFOLDERS_REG_KEY, 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return result;
    }
    wchar_t valueName[MAX_PATH];
    DWORD valueNameLen = MAX_PATH;
    DWORD dataType = 0;
    std::vector<wchar_t> dataBuf(MAX_PATH);
    DWORD dataLen = (DWORD)(dataBuf.size() * sizeof(wchar_t));
    DWORD index = 0;
    while (RegEnumValueW(hKey, index, valueName, &valueNameLen, nullptr, &dataType,
                         reinterpret_cast<LPBYTE>(dataBuf.data()), &dataLen) == ERROR_SUCCESS) {
        if (dataType == REG_SZ) {
            std::wstring path(valueName, valueNameLen);
            std::wstring name(dataBuf.data(), dataLen / sizeof(wchar_t));
            if (!name.empty() && name.back() == L'\0') name.pop_back();
            result.push_back({path, name});
        }
        valueNameLen = MAX_PATH;
        dataLen = (DWORD)(dataBuf.size() * sizeof(wchar_t));
        ++index;
    }
    RegCloseKey(hKey);
    return result;
}

static std::wstring RegGetActiveFolder() {
    HKEY hKey = nullptr;
    std::wstring result;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, MCFOLDERS_REG_KEY, 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return result;
    }
    wchar_t buf[MAX_PATH]{};
    DWORD bufLen = sizeof(buf);
    if (RegQueryValueExW(hKey, L"ActiveFolder", nullptr, nullptr,
                         reinterpret_cast<LPBYTE>(buf), &bufLen) == ERROR_SUCCESS) {
        result = buf;
    }
    RegCloseKey(hKey);
    return result;
}

static bool RegSetActiveFolder(const std::wstring& path) {
    HKEY hKey = nullptr;
    DWORD disp = 0;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, MCFOLDERS_REG_KEY, 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, &disp) != ERROR_SUCCESS) {
        return false;
    }
    bool ok = RegSetValueExW(hKey, L"ActiveFolder", 0, REG_SZ,
                             reinterpret_cast<const BYTE*>(path.c_str()),
                             (DWORD)((path.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
    RegCloseKey(hKey);
    return ok;
}

static bool RegAddMcFolder(const std::wstring& path, const std::wstring& name) {
    HKEY hKey = nullptr;
    DWORD disp = 0;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, MCFOLDERS_REG_KEY, 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, &disp) != ERROR_SUCCESS) {
        return false;
    }
    bool ok = RegSetValueExW(hKey, path.c_str(), 0, REG_SZ,
                             reinterpret_cast<const BYTE*>(name.c_str()),
                             (DWORD)((name.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
    RegCloseKey(hKey);
    return ok;
}

static bool RegRemoveMcFolder(const std::wstring& path) {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, MCFOLDERS_REG_KEY, 0, KEY_SET_VALUE, &hKey) != ERROR_SUCCESS) {
        return false;
    }
    bool ok = RegDeleteValueW(hKey, path.c_str()) == ERROR_SUCCESS;
    RegCloseKey(hKey);
    return ok;
}

void RegisterMcFolders(Bridge& bridge) {
    // 启动时恢复上次激活的 MC 文件夹（否则 GetMcRoot() 会回退到 exe 同级 .minecraft，导致"版本 JSON 解析失败"）
    std::wstring activeFolder = RegGetActiveFolder();
    if (!activeFolder.empty() && std::filesystem::is_directory(activeFolder)) {
        g_mcRoot = activeFolder;
    }
    bridge.Register("mc.listMcFolders", [](const Json&) {
        auto folders = RegEnumMcFolders();
        std::wstring active = RegGetActiveFolder();
        Json arr = Json::array();
        for (const auto& f : folders) {
            if (f.first == L"ActiveFolder") continue;
            Json o = Json::object();
            o["path"] = lxe::WideToUtf8(f.first);
            o["name"] = lxe::WideToUtf8(f.second);
            o["active"] = (f.first == active);
            arr.asArray().push_back(o);
        }
        Json result = Json::object();
        result["folders"] = arr;
        return Ok(result);
    });

    bridge.Register("mc.addMcFolder", [](const Json& params) {
        std::wstring path;
        if (params.isObject() && params.contains("path") && params.at("path").isString()) {
            path = lxe::Utf8ToWide(params.at("path").asString());
        }
        if (path.empty()) return Err(-32602, "missing path");
        if (!std::filesystem::is_directory(path)) return Err(-32002, "路径不是有效文件夹");

        std::wstring name;
        if (params.isObject() && params.contains("name") && params.at("name").isString()) {
            name = lxe::Utf8ToWide(params.at("name").asString());
        }
        if (name.empty()) {
            std::filesystem::path p(path);
            name = p.filename().wstring();
            if (name.empty()) name = path;
        }

        bool ok = RegAddMcFolder(path, name);
        Json result = Json::object();
        result["ok"] = ok;
        if (ok) {
            result["path"] = lxe::WideToUtf8(path);
            result["name"] = lxe::WideToUtf8(name);
        }
        return Ok(result);
    });

    bridge.Register("mc.removeMcFolder", [](const Json& params) {
        std::wstring path;
        if (params.isObject() && params.contains("path") && params.at("path").isString()) {
            path = lxe::Utf8ToWide(params.at("path").asString());
        }
        if (path.empty()) return Err(-32602, "missing path");

        bool ok = RegRemoveMcFolder(path);
        Json result = Json::object();
        result["ok"] = ok;
        return Ok(result);
    });

    bridge.Register("mc.setActiveMcFolder", [](const Json& params) {
        std::wstring path;
        if (params.isObject() && params.contains("path") && params.at("path").isString()) {
            path = lxe::Utf8ToWide(params.at("path").asString());
        }
        if (path.empty()) return Err(-32602, "missing path");

        g_mcRoot = path;
        InvalidateLocalVersionsCache();
        bool ok = RegSetActiveFolder(path);
        Json result = Json::object();
        result["ok"] = ok;
        result["mcRoot"] = lxe::WideToUtf8(path);
        return Ok(result);
    });

    // ============ 智能拖放导入（前端把 nativeDrop 事件里的真实路径送来） ============
    // 探测 zip 类型：resourcepack（含 pack.mcmeta）/ shader（含 shaders 目录）/
    // modpack（含 mods|versions|.minecraft 结构），供前端决定导入到哪个目录。
    bridge.Register("mc.probeZip", [](const Json& params) {
        std::wstring path;
        if (params.isObject() && params.contains("path") && params.at("path").isString())
            path = lxe::Utf8ToWide(params.at("path").asString());
        if (path.empty()) return Err(-32602, "missing path");
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec)) return Err(-32002, "不是有效文件");
        std::wstring cmd = L"tar -tf \"" + path + L"\"";
        std::string entries = lxe::WideToUtf8(RunCapture(cmd));
        std::transform(entries.begin(), entries.end(), entries.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        std::string kind;
        if (entries.find("pack.mcmeta") != std::string::npos) kind = "resourcepack";
        else if (entries.find("shaders/") != std::string::npos || entries.find("shaders.json") != std::string::npos) kind = "shader";
        else if (entries.find("mods/") != std::string::npos || entries.find("versions/") != std::string::npos || entries.find(".minecraft") != std::string::npos) kind = "modpack";
        else kind = "other";
        Json result = Json::object();
        result["kind"] = kind;
        return Ok(result);
    });

    // 探测拖入文件夹类型：mcroot（.minecraft 结构）/ version（单个版本目录）/ other
    bridge.Register("mc.probeFolder", [](const Json& params) {
        std::wstring path;
        if (params.isObject() && params.contains("path") && params.at("path").isString())
            path = lxe::Utf8ToWide(params.at("path").asString());
        if (path.empty()) return Err(-32602, "missing path");
        std::error_code ec;
        if (!std::filesystem::is_directory(path, ec)) return Err(-32002, "不是有效文件夹");
        auto has = [&](const std::wstring& sub) { return std::filesystem::exists(path + L"\\" + sub, ec); };
        Json result = Json::object();
        if (has(L"versions") || has(L"libraries") || has(L"assets")) result["kind"] = "mcroot";
        else if (has(L"version.json")) result["kind"] = "version";
        else result["kind"] = "other";
        return Ok(result);
    });

    // 拖入文件自动导入：kind ∈ authlib | mod | resourcepack | shader | modpack
    // 未指定 kind 时按扩展名/文件名兜底：.jar 按名称含 authlib 判断，.zip 自动探测内容。
    bridge.Register("mc.dropImport", [](const Json& params) {
        std::wstring path, kind, version;
        if (params.isObject() && params.contains("path") && params.at("path").isString())
            path = lxe::Utf8ToWide(params.at("path").asString());
        if (params.isObject() && params.contains("kind") && params.at("kind").isString())
            kind = lxe::Utf8ToWide(params.at("kind").asString());
        if (params.isObject() && params.contains("version") && params.at("version").isString())
            version = lxe::Utf8ToWide(params.at("version").asString());
        if (path.empty()) return Err(-32602, "missing path");
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) return Err(-32002, "文件不存在");

        std::wstring srcName = std::filesystem::path(path).filename().wstring();
        if (srcName.empty()) return Err(-32003, "无效路径");
        std::wstring lower = srcName;
        for (auto& c : lower) c = (wchar_t)std::towlower(c);

        if (kind.empty()) {
            std::wstring ext = std::filesystem::path(path).extension().wstring();
            for (auto& c : ext) c = (wchar_t)std::towlower(c);
            // .jar.disabled：禁用的模组文件（复制时保留 .disabled 后缀，游戏不会加载，版本设置页可识别）
            std::wstring stem = std::filesystem::path(path).stem().wstring();
            for (auto& c : stem) c = (wchar_t)std::towlower(c);
            if (ext == L".jar") {
                kind = (lower.find(L"authlib") != std::wstring::npos) ? L"authlib" : L"mod";
            } else if (ext == L".disabled" && stem.rfind(L".jar") == stem.size() - 4) {
                kind = (stem.find(L"authlib") != std::wstring::npos) ? L"authlib" : L"mod";
            } else if (ext == L".zip") {
                std::wstring cmd = L"tar -tf \"" + path + L"\"";
                std::string entries = lxe::WideToUtf8(RunCapture(cmd));
                std::transform(entries.begin(), entries.end(), entries.begin(),
                               [](unsigned char c) { return (char)std::tolower(c); });
                if (entries.find("pack.mcmeta") != std::string::npos) kind = L"resourcepack";
                else if (entries.find("shaders/") != std::string::npos || entries.find("shaders.json") != std::string::npos) kind = L"shader";
                else if (entries.find("mods/") != std::string::npos || entries.find("versions/") != std::string::npos || entries.find(".minecraft") != std::string::npos) kind = L"modpack";
                else kind = L"other";
            } else {
                return Err(-32004, "不支持的文件类型：" + lxe::WideToUtf8(srcName));
            }
        }

        std::wstring mcRoot = GetMcRoot();
        std::wstring destDir, dest;
        if (kind == L"authlib") {
            dest = mcRoot + L"\\authlib-injector.jar";
        } else if (kind == L"mod" || kind == L"resourcepack" || kind == L"shader") {
            // 版本隔离：version 非空 → 导入到 versions/<version>/<kind>；否则全局 .minecraft/<kind>
            destDir = PackDirFor(lxe::WideToUtf8(kind), lxe::WideToUtf8(version));
            dest = destDir + L"\\" + srcName;
        } else if (kind == L"modpack") {
            destDir = mcRoot + L"\\modpacks";
            dest = destDir + L"\\" + srcName;
        } else {
            return Err(-32005, "未知导入类型：" + lxe::WideToUtf8(kind));
        }

        if (!destDir.empty()) std::filesystem::create_directories(destDir, ec);
        std::filesystem::copy_file(path, dest, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec || !std::filesystem::exists(dest, ec)) {
            return Err(-32006, "复制失败：" + lxe::WideToUtf8(srcName));
        }
        Json result = Json::object();
        result["ok"] = true;
        result["kind"] = lxe::WideToUtf8(kind);
        result["dest"] = lxe::WideToUtf8(dest);
        result["name"] = lxe::WideToUtf8(srcName);
        return Ok(result);
    });

    // 读取本地文件为 Data URL（拖入背景图时使用；>16MB 拒绝，避免前端卡顿）
    bridge.Register("app.readFileDataUrl", [](const Json& params) {
        std::wstring path;
        if (params.isObject() && params.contains("path") && params.at("path").isString())
            path = lxe::Utf8ToWide(params.at("path").asString());
        if (path.empty()) return Err(-32602, "missing path");
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec)) return Err(-32002, "不是有效文件");
        long long sz = std::filesystem::file_size(path, ec);
        if (sz > 16LL * 1024 * 1024) return Err(-32007, "文件过大（>16MB）");
        std::ifstream f(path, std::ios::binary);
        if (!f) return Err(-32008, "无法读取文件");
        std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        std::wstring ext = std::filesystem::path(path).extension().wstring();
        for (auto& c : ext) c = (wchar_t)std::towlower(c);
        std::string mime = "image/png";
        if (ext == L".jpg" || ext == L".jpeg") mime = "image/jpeg";
        else if (ext == L".webp") mime = "image/webp";
        else if (ext == L".gif") mime = "image/gif";
        else if (ext == L".bmp") mime = "image/bmp";
        Json result = Json::object();
        result["dataUrl"] = "data:" + mime + ";base64," + Base64Encode(data);
        return Ok(result);
    });

    // ============ 模组/资源包/光影 管理 ============
    // kind: mods | resourcepacks | shaderpacks
    bridge.Register("mc.listPackFiles", [](const Json& params) {
        std::string kind, version;
        if (params.isObject() && params.contains("kind") && params.at("kind").isString()) kind = params.at("kind").asString();
        if (params.isObject() && params.contains("version") && params.at("version").isString()) version = params.at("version").asString();
        std::wstring dir = PackDirFor(kind, version);
        Json arr = Json::array();
        long long totalSize = 0;
        // 递归计算文件/目录大小（资源包/光影可能是子目录）
        std::function<long long(const std::wstring&)> dirSize = [&](const std::wstring& p) -> long long {
            long long sum = 0;
            std::error_code sec;
            if (std::filesystem::is_directory(p, sec)) {
                for (const auto& e : std::filesystem::recursive_directory_iterator(p, sec)) {
                    if (sec) break;
                    if (e.is_regular_file(sec)) sum += (long long)e.file_size(sec);
                }
            } else {
                sum += (long long)std::filesystem::file_size(p, sec);
            }
            return sum;
        };
        std::error_code ec;
        if (std::filesystem::exists(dir)) {
            // 同名合并：foo.jar 与 foo.jar.disabled 并存时只返回一条。
            // 禁用状态 = 「活动文件不存在且 .disabled 存在」，否则视为启用（游戏会加载活动副本），
            // 避免禁用/启用混排时出现同名重复条目导致禁用状态丢失或操作互相冲突。
            std::map<std::string, bool> hasActive;
            std::map<std::string, long long> entrySize;
            std::vector<std::string> order;
            for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
                if (ec) break;
                std::string fn = lxe::WideToUtf8(e.path().filename().wstring());
                if (fn.empty() || fn[0] == '.') continue;
                bool disabled = false;
                if (fn.size() >= 9 && fn.compare(fn.size() - 9, 9, ".disabled") == 0) {
                    disabled = true;
                    fn = fn.substr(0, fn.size() - 9);
                }
                if (fn.empty()) continue;
                if (!hasActive.count(fn)) order.push_back(fn);
                if (disabled) {
                    if (!hasActive[fn]) entrySize[fn] = dirSize(e.path().wstring());
                } else {
                    hasActive[fn] = true;
                    entrySize[fn] = dirSize(e.path().wstring());
                }
            }
            for (const auto& name : order) {
                Json o = Json::object();
                o["name"] = name;
                o["disabled"] = !hasActive[name];
                long long sz = entrySize[name];
                o["size"] = sz;
                totalSize += sz;
                arr.asArray().push_back(o);
            }
        }
        Json result = Json::object();
        result["list"] = arr;
        result["totalSize"] = totalSize;
        return Ok(result);
    });

    bridge.Register("mc.deletePackFile", [](const Json& params) {
        std::string kind, name, version;
        if (params.isObject() && params.contains("kind") && params.at("kind").isString()) kind = params.at("kind").asString();
        if (params.isObject() && params.contains("name") && params.at("name").isString()) name = params.at("name").asString();
        if (params.isObject() && params.contains("version") && params.at("version").isString()) version = params.at("version").asString();
        if (name.empty()) return Err(-32602, "missing name");
        std::wstring dir = PackDirFor(kind, version);
        std::wstring wname = lxe::Utf8ToWide(name);
        if (wname.find(L"..") != std::wstring::npos) return Err(-32603, "invalid name");
        std::error_code ec;
        bool ok = false;
        // 同名重复（foo.jar 与 foo.jar.disabled 并存）时两个实体一并删除
        if (std::filesystem::exists(dir + L"\\" + wname)) {
            ok = std::filesystem::remove(dir + L"\\" + wname, ec);
            std::error_code ec2;
            if (std::filesystem::exists(dir + L"\\" + wname + L".disabled"))
                std::filesystem::remove(dir + L"\\" + wname + L".disabled", ec2);
        } else if (std::filesystem::exists(dir + L"\\" + wname + L".disabled")) {
            ok = std::filesystem::remove(dir + L"\\" + wname + L".disabled", ec);
        }
        Json result = Json::object();
        result["ok"] = ok;
        return Ok(result);
    });

    bridge.Register("mc.togglePackFile", [](const Json& params) {
        std::string kind, name, version;
        bool disable = true;
        if (params.isObject() && params.contains("kind") && params.at("kind").isString()) kind = params.at("kind").asString();
        if (params.isObject() && params.contains("name") && params.at("name").isString()) name = params.at("name").asString();
        if (params.isObject() && params.contains("disable") && params.at("disable").isBool()) disable = params.at("disable").asBool();
        if (params.isObject() && params.contains("version") && params.at("version").isString()) version = params.at("version").asString();
        if (name.empty()) return Err(-32602, "missing name");
        std::wstring dir = PackDirFor(kind, version);
        std::wstring wname = lxe::Utf8ToWide(name);
        if (wname.find(L"..") != std::wstring::npos) return Err(-32603, "invalid name");
        std::error_code ec;
        bool ok = false;
        std::wstring src = dir + L"\\" + wname;
        std::wstring dst = dir + L"\\" + wname + L".disabled";
        if (disable) {
            if (std::filesystem::exists(src)) {
                // 目标 .disabled 已存在（同名重复）时先清理，避免 rename 覆盖失败
                if (std::filesystem::exists(dst)) std::filesystem::remove(dst, ec);
                std::filesystem::rename(src, dst, ec); ok = !ec;
            }
        } else {
            if (std::filesystem::exists(dst)) {
                // 活动文件已存在（同名重复）说明已在启用态，仅清理 .disabled 副本
                if (std::filesystem::exists(src)) { std::filesystem::remove(dst, ec); ok = !ec; }
                else { std::filesystem::rename(dst, src, ec); ok = !ec; }
            }
        }
        Json result = Json::object();
        result["ok"] = ok;
        return Ok(result);
    });

    // 批量操作：toggle/delete 多个文件（前端批量选中后调用）
    bridge.Register("mc.togglePackFiles", [](const Json& params) {
        std::string kind, version; bool disable = true;
        if (params.isObject() && params.contains("kind") && params.at("kind").isString()) kind = params.at("kind").asString();
        if (params.isObject() && params.contains("disable") && params.at("disable").isBool()) disable = params.at("disable").asBool();
        if (params.isObject() && params.contains("version") && params.at("version").isString()) version = params.at("version").asString();
        std::vector<std::string> names;
        if (params.isObject() && params.contains("names") && params.at("names").isArray()) {
            for (const auto& n : params.at("names").asArray())
                if (n.isString()) names.push_back(n.asString());
        }
        if (names.empty()) return Err(-32602, "missing names[]");
        std::wstring dir = PackDirFor(kind, version);
        int done = 0;
        for (const auto& name : names) {
            std::wstring wname = lxe::Utf8ToWide(name);
            if (wname.find(L"..") != std::wstring::npos) continue;
            std::error_code ec;
            std::wstring src = dir + L"\\" + wname;
            std::wstring dst = dir + L"\\" + wname + L".disabled";
            if (disable) {
                if (std::filesystem::exists(src)) {
                    if (std::filesystem::exists(dst)) std::filesystem::remove(dst, ec);
                    std::filesystem::rename(src, dst, ec); if (!ec) ++done;
                }
            } else {
                if (std::filesystem::exists(dst)) {
                    if (std::filesystem::exists(src)) { std::filesystem::remove(dst, ec); if (!ec) ++done; }
                    else { std::filesystem::rename(dst, src, ec); if (!ec) ++done; }
                }
            }
        }
        Json result = Json::object();
        result["ok"] = done > 0;
        result["done"] = done;
        return Ok(result);
    });

    bridge.Register("mc.deletePackFiles", [](const Json& params) {
        std::string kind, version;
        if (params.isObject() && params.contains("kind") && params.at("kind").isString()) kind = params.at("kind").asString();
        if (params.isObject() && params.contains("version") && params.at("version").isString()) version = params.at("version").asString();
        std::vector<std::string> names;
        if (params.isObject() && params.contains("names") && params.at("names").isArray()) {
            for (const auto& n : params.at("names").asArray())
                if (n.isString()) names.push_back(n.asString());
        }
        if (names.empty()) return Err(-32602, "missing names[]");
        std::wstring dir = PackDirFor(kind, version);
        int done = 0;
        for (const auto& name : names) {
            std::wstring wname = lxe::Utf8ToWide(name);
            if (wname.find(L"..") != std::wstring::npos) continue;
            std::error_code ec;
            // 同名重复（foo.jar 与 foo.jar.disabled 并存）时两个实体一并删除
            if (std::filesystem::exists(dir + L"\\" + wname)) {
                if (std::filesystem::remove(dir + L"\\" + wname, ec)) ++done;
                std::error_code ec2;
                if (std::filesystem::exists(dir + L"\\" + wname + L".disabled"))
                    std::filesystem::remove(dir + L"\\" + wname + L".disabled", ec2);
            }
            else if (std::filesystem::exists(dir + L"\\" + wname + L".disabled")) {
                if (std::filesystem::remove(dir + L"\\" + wname + L".disabled", ec)) ++done;
            }
        }
        Json result = Json::object();
        result["ok"] = done > 0;
        result["done"] = done;
        return Ok(result);
    });

    // 版本隔离：把选中的全局（.minecraft/<kind>）文件复制进指定版本的 versions/<version>/<kind>。
    // 复制而非移动（全局原文件保留）；资源包/光影等子目录类型递归复制。
    // 普通名与 .disabled 后缀一并复制（.disabled 到版本目录后保持禁用状态）。
    bridge.Register("mc.importIntoVersion", [](const Json& params) {
        std::string kind, version;
        if (params.isObject() && params.contains("kind") && params.at("kind").isString()) kind = params.at("kind").asString();
        if (params.isObject() && params.contains("version") && params.at("version").isString()) version = params.at("version").asString();
        std::vector<std::string> names;
        if (params.isObject() && params.contains("names") && params.at("names").isArray()) {
            for (const auto& n : params.at("names").asArray())
                if (n.isString()) names.push_back(n.asString());
        }
        if (version.empty() || names.empty()) return Err(-32602, "missing version/names[]");
        std::wstring srcDir = PackDirFor(kind, "");
        std::wstring dstDir = PackDirFor(kind, version);
        std::error_code ec;
        std::filesystem::create_directories(dstDir, ec);
        int done = 0;
        for (const auto& name : names) {
            std::wstring wname = lxe::Utf8ToWide(name);
            if (wname.find(L"..") != std::wstring::npos) continue;
            if (wname.empty()) continue;
            // 依次复制 普通名 / .disabled 两个实体
            for (const wchar_t* suffix : { L"", L".disabled" }) {
                std::wstring src = srcDir + L"\\" + wname + suffix;
                std::wstring dst = dstDir + L"\\" + wname + suffix;
                std::error_code sec;
                if (!std::filesystem::exists(src, sec)) continue;
                if (src == dst) continue; // 同名同目录防御
                std::error_code cec;
                if (std::filesystem::is_directory(src, sec)) {
                    std::filesystem::copy(src, dst,
                        std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, cec);
                } else {
                    std::filesystem::copy_file(src, dst, std::filesystem::copy_options::overwrite_existing, cec);
                }
                if (cec) continue;
                ++done;
            }
        }
        Json result = Json::object();
        result["ok"] = done > 0;
        result["done"] = done;
        return Ok(result);
    });

    // 版本隔离：本地 mod 自动匹配云端（Req4）。给定 kind(须含 "mod")/version/name，
    // 计算文件 SHA-1 后调 Modrinth /version_file/{hash}/sha1 反查版本及其 project，返回项目详情。
    // 全程后台线程（30MB+ 大文件哈希 + HTTP 反查），避免阻塞桥线程。
    bridge.RegisterAsync("mc.matchLocalMod", [](const Json& params, const std::function<void(HandlerResult)>& done) {
        std::string kind, version, name;
        if (params.isObject() && params.contains("kind") && params.at("kind").isString()) kind = params.at("kind").asString();
        if (params.isObject() && params.contains("version") && params.at("version").isString()) version = params.at("version").asString();
        if (params.isObject() && params.contains("name") && params.at("name").isString()) name = params.at("name").asString();
        if (name.empty()) { done(Err(-32602, "missing name")); return; }
        std::wstring dir = PackDirFor(kind, version);
        std::wstring wname = lxe::Utf8ToWide(name);
        if (wname.find(L"..") != std::wstring::npos) { done(Err(-32603, "invalid name")); return; }
        // 解析不可见路径：names 可能带 .disabled 后缀或子目录（递归查找同名实体）
        std::vector<std::wstring> candidates;
        {
            std::wstring probe = dir + L"\\" + wname;
            std::error_code pec;
            if (std::filesystem::exists(probe, pec)) candidates.push_back(probe);
            if (std::filesystem::exists(probe + L".disabled", pec)) candidates.push_back(probe + L".disabled");
            if (candidates.empty() && std::filesystem::is_directory(dir, pec)) {
                // 子目录类型（资源包/光影目录）/ 兜底：装载目录内含该子串的文件（仅 mods 类）
                int hits = 0;
                for (const auto& e : std::filesystem::recursive_directory_iterator(dir, pec)) {
                    if (pec) break;
                    if (!e.is_regular_file(pec)) continue;
                    std::wstring fn = e.path().filename().wstring();
                    if (fn.find(wname) != std::wstring::npos) { candidates.push_back(e.path().wstring()); if (++hits >= 3) break; }
                }
            }
        }
        std::thread([done, candidates, version]() {
            try {
                for (const auto& filePath : candidates) {
                    std::string sha1 = FileSha1Hex(filePath);
                    if (sha1.empty()) continue;
                    std::string url = "https://api.modrinth.com/v2/version_file/" + sha1 + "/sha1";
                    std::string vtext = HttpFetchText(lxe::Utf8ToWide(url));
                    if (vtext.empty()) continue;
                    std::string projectId, projUrl;
                    try {
                        Json vd = Json::parse(vtext);
                        if (vd.isObject() && vd.contains("project_id") && vd.at("project_id").isString()) {
                            projectId = vd.at("project_id").asString();
                            if (vd.contains("files") && vd.at("files").isArray()) {
                                for (const auto& f : vd.at("files").asArray()) {
                                    if (f.isObject() && f.contains("url") && f.at("url").isString()) { projUrl = f.at("url").asString(); break; }
                                }
                            }
                        }
                    } catch (...) { continue; }
                    if (projectId.empty()) continue;
                    // 反查项目详情（标题/图标/简介）
                    std::string ptext = HttpFetchText(lxe::Utf8ToWide("https://api.modrinth.com/v2/project/" + UrlEncode(projectId)));
                    Json o = Json::object();
                    o["matched"] = true;
                    o["projectId"] = projectId;
                    if (!projUrl.empty()) o["projectUrl"] = projUrl;
                    if (!ptext.empty()) {
                        try {
                            Json pd = Json::parse(ptext);
                            if (pd.isObject()) {
                                if (pd.contains("slug") && pd.at("slug").isString()) o["slug"] = pd.at("slug").asString();
                                if (pd.contains("title") && pd.at("title").isString()) o["title"] = pd.at("title").asString();
                                if (pd.contains("description") && pd.at("description").isString()) o["description"] = pd.at("description").asString();
                                if (pd.contains("icon_url") && pd.at("icon_url").isString()) o["icon"] = pd.at("icon_url").asString();
                                if (pd.contains("project_type") && pd.at("project_type").isString()) o["projectType"] = pd.at("project_type").asString();
                                if (pd.contains("downloads") && pd.at("downloads").isNumber()) o["downloads"] = (long long)pd.at("downloads").asNumber();
                            }
                        } catch (...) {}
                    }
                    done(Ok(o));
                    return;
                }
                Json no = Json::object();
                no["matched"] = false;
                done(Ok(no));
            } catch (const std::exception& e) {
                done(Err(-32060, std::string("云端匹配异常：") + e.what()));
            } catch (...) {
                done(Err(-32060, "云端匹配未知异常"));
            }
        }).detach();
    });

    bridge.Register("mc.openPackFolder", [](const Json& params) {
        std::string kind, version;
        if (params.isObject() && params.contains("kind") && params.at("kind").isString()) kind = params.at("kind").asString();
        if (params.isObject() && params.contains("version") && params.at("version").isString()) version = params.at("version").asString();
        std::wstring dir = PackDirFor(kind, version);
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        Json result = Json::object();
        result["ok"] = true;
        return Ok(result);
    });

    // 打开 MC 根目录（自定义右键菜单：打开 mc 文件夹）
    bridge.Register("mc.openMcFolder", [](const Json&) {
        std::wstring dir = GetMcRoot();
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        Json result = Json::object();
        result["ok"] = true;
        return Ok(result);
    });
}

} // namespace

// ---- 用户设置持久化（settings.json，位于 exe 同级，跨启动保留前端参数） ----
static std::mutex g_settingsMutex;

static std::wstring SettingsFilePath() {
    return ExeSiblingPath(L"settings.json");
}

// 读取 settings.json 全文为对象，失败/损坏时返回空对象。
static Json LoadSettingsFile() {
    std::lock_guard<std::mutex> lk(g_settingsMutex);
    auto [s, ok] = ReadFileUtf8(SettingsFilePath());
    if (!ok || s.empty()) return Json::object();
    try {
        Json j = Json::parse(s);
        return j.isObject() ? j : Json::object();
    } catch (...) {
        return Json::object();
    }
}

// 合并 patch 后写回 settings.json。value 为 null 的键表示删除。
static Json SaveSettingsFile(const Json& patch) {
    std::lock_guard<std::mutex> lk(g_settingsMutex);
    Json cur = Json::object();
    auto [s, ok] = ReadFileUtf8(SettingsFilePath());
    if (ok && !s.empty()) {
        try {
            Json j = Json::parse(s);
            if (j.isObject()) cur = j;
        } catch (...) {}
    }
    if (patch.isObject()) {
        for (const auto& [k, v] : patch.asObject()) {
            if (v.isNull()) cur.asObject().erase(k);
            else cur[k] = v;
        }
    }
    std::ofstream f(SettingsFilePath(), std::ios::binary | std::ios::trunc);
    if (f) {
        const std::string out = cur.dump();
        f.write(out.data(), static_cast<std::streamsize>(out.size()));
        f.flush();
    }
    return cur;
}

// ===================== §5 镜像源测速与切换 =====================
// 会话内官方源可用性（§5.2：仅存进程内存，不持久化；首次请求官方版本列表时计时）
static std::atomic<bool> g_officialProbeStarted{false};
static std::atomic<bool> g_officialUsable{false};
static std::mutex g_officialProbeMutex;

static const char* kOfficialManifestUrl =
    "https://launchermeta.mojang.com/mc/game/version_manifest_v2.json";
static const char* kMirrorManifestUrl =
    "https://bmclapi2.bangbang93.com/mc/game/version_manifest.json";

// 读取字符串/数字设置项（settings.json，异常安全）
static std::string SettingString(const std::string& key, const std::string& def) {
    Json s = LoadSettingsFile();
    if (s.isObject() && s.contains(key) && s.at(key).isString()) {
        std::string v = s.at(key).asString();
        if (!v.empty()) return v;
    }
    return def;
}
static int SettingInt(const std::string& key, int def) {
    Json s = LoadSettingsFile();
    if (s.isObject() && s.contains(key) && s.at(key).isNumber())
        return (int)s.at(key).asNumber();
    return def;
}

// 官方源可用性探测：请求官方版本列表并计时；< 4 秒标记可用（本会话偏向官方源）
static void EnsureOfficialProbe() {
    if (g_officialProbeStarted.load()) return;
    std::lock_guard<std::mutex> lk(g_officialProbeMutex);
    if (g_officialProbeStarted.load()) return;
    g_officialProbeStarted.store(true);
    auto t0 = std::chrono::steady_clock::now();
    std::string body = HttpFetchText(lxe::Utf8ToWide(kOfficialManifestUrl));
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    g_officialUsable.store(!body.empty() && ms < 4000);
}

// 解析某类源选择（§5.1 两个独立开关 + §5.2 自动探测）
// kind ∈ {file, version} → 返回 "official" | "mirror"
static std::string ResolveSource(const std::string& kind) {
    std::string pref = SettingString(kind == "version" ? "versionSource" : "fileSource", "auto");
    if (pref == "official") return "official";
    if (pref == "mirror") return "mirror";
    EnsureOfficialProbe();
    return g_officialUsable.load() ? "official" : "mirror";
}

// 官方文件 URL → BMCLAPI 镜像 URL（库/资源/版本列表各有不同替换规则，§3.2/§5）
static std::string MirrorFileUrl(const std::string& url) {
    if (url.rfind("https://", 0) == 0) {
        const std::string libMark = "libraries.minecraft.net";
        const std::string assetMark = "resources.download.minecraft.net";
        auto libPos = url.find(libMark);
        if (libPos != std::string::npos) {
            std::string path = url.substr(libPos + libMark.size());
            return "https://bmclapi2.bangbang93.com/maven" + path;
        }
        auto assetPos = url.find(assetMark);
        if (assetPos != std::string::npos) {
            std::string path = url.substr(assetPos + assetMark.size());
            return "https://bmclapi2.bangbang93.com/assets" + path;
        }
    }
    if (!url.empty() && url[0] == '/')
        return "https://bmclapi2.bangbang93.com" + url;
    return url;
}

// 文件下载统一入口：按“文件下载源”开关返回实际 URL
static std::string FileUrlForDownload(const std::string& url) {
    if (ResolveSource("file") == "mirror") return MirrorFileUrl(url);
    return url;
}

// 版本列表（manifest）统一入口：按“版本列表源”开关返回实际 URL
static std::string ManifestUrlForFetch() {
    if (ResolveSource("version") == "mirror")
        return std::string(kMirrorManifestUrl);
    return std::string(kOfficialManifestUrl);
}

void RegisterSettings(Bridge& bridge) {
    // 读/写与 exe 同级 settings.json。插件 storage 的持久化走这里，必须异步化：
    // 崩溃分析等场景的 bundle 可达数 KB~数十KB，同步写盘会阻塞桥消息线程 → 主窗口卡死/白屏。
    // 写盘在后台线程进行，Load/Save 内部 g_settingsMutex 保证读-合并-写原子串行，无丢更新。
    bridge.RegisterAsync("settings.get", [](const Json&, const std::function<void(HandlerResult)>& done) {
        std::thread([done]() {
            try { done(Ok(LoadSettingsFile())); }
            catch (const std::exception& e) { done(Err(-32000, std::string("读取 settings 异常：") + e.what())); }
            catch (...) { done(Err(-32000, "读取 settings 未知异常")); }
        }).detach();
    });
    bridge.RegisterAsync("settings.set", [](const Json& params, const std::function<void(HandlerResult)>& done) {
        Json patch = Json::object();
        if (params.isObject() && params.contains("key") && params.at("key").isString()) {
            patch[params.at("key").asString()] =
                params.contains("value") ? params.at("value") : Json(nullptr);
        } else if (params.isObject() && params.contains("settings") && params.at("settings").isObject()) {
            patch = params.at("settings");
        } else if (params.isObject()) {
            patch = params;
        }
        std::thread([patch, done]() {
            try { done(Ok(SaveSettingsFile(patch))); }
            catch (const std::exception& e) { done(Err(-32000, std::string("写入 settings 异常：") + e.what())); }
            catch (...) { done(Err(-32000, "写入 settings 未知异常")); }
        }).detach();
    });

    // ===================== §5 镜像源设置与测速 =====================
    // 读取源设置 + 官方源探测结果（§5.1 两个独立开关 + §5.2 会话内可用性）
    bridge.Register("source.info", [](const Json&) {
        Json r = Json::object();
        r["fileSource"] = SettingString("fileSource", "auto");
        r["versionSource"] = SettingString("versionSource", "auto");
        r["downloadThreads"] = SettingInt("downloadThreads", 64);
        r["rateLimitMBps"] = (double)SettingInt("rateLimitMBps10", 0) / 10.0;
        r["officialProbeStarted"] = g_officialProbeStarted.load();
        r["officialUsable"] = g_officialUsable.load();
        return Ok(r);
    });

    // 触发官方源耗时探测（§5.2：自动模式首次请求官方版本列表时计时；耗时 <4s 标记可用）
    bridge.RegisterAsync("source.probe", [](const Json&, const std::function<void(HandlerResult)>& done) {
        std::thread([done]() {
            Json r = Json::object();
            EnsureOfficialProbe();
            r["officialUsable"] = g_officialUsable.load();
            done(Ok(r));
        }).detach();
    });

    // 版本列表源切换后重新拉取 manifest（§5.1 版本列表源：官方/镜像/自动）
    bridge.RegisterAsync("mc.refreshVersionList", [](const Json&, const std::function<void(HandlerResult)>& done) {
        std::thread([done]() {
            if (g_manifestPath.empty()) { done(Err(-32001, "manifest path not configured")); return; }
            std::string url = ManifestUrlForFetch();
            std::string body = HttpFetchText(lxe::Utf8ToWide(url));
            if (body.empty()) { done(Err(-32003, "版本列表拉取失败")); return; }
            std::error_code ec;
            std::filesystem::create_directories(std::filesystem::path(g_manifestPath).parent_path(), ec);
            std::ofstream f(g_manifestPath, std::ios::binary | std::ios::trunc);
            if (!f) { done(Err(-32004, "写入版本列表失败")); return; }
            f.write(body.data(), (std::streamsize)body.size());
            f.flush();
            Json result = Json::object();
            result["ok"] = true;
            result["source"] = ResolveSource("version");
            result["bytes"] = (long long)body.size();
            done(Ok(result));
        }).detach();
    });
}

// 关闭后台服务：停止游戏进程监控线程并回收资源。
// 必须在 WebViewHost/Bridge 析构之前调用，否则进程退出时 joinable 线程会触发 abort。
void ShutdownServices() {
    g_bridgeForMonitor = nullptr;
    g_monitorRunning.store(false, std::memory_order_release);
    if (g_monitorThread.joinable()) g_monitorThread.join();
    std::lock_guard<std::mutex> lock(g_gameInstancesMutex);
    for (auto& inst : g_gameInstances) {
        if (inst.first) CloseHandle(inst.first);
    }
    g_gameInstances.clear();
}

// ============ 加载器官方源：版本列表 + 完整安装 ============
// 覆盖 Fabric / Forge / OptiFine；Neoforge、Quilt 不在范围内（继续走 BMCLAPI 原流程）。
// 安装采用官方源：Fabric 直接合并 profile json；Forge/OptiFine 运行官方安装器后合并产物到 installName。

// 将 HTML 按 <tr>...</tr> 切分为行片段
static std::vector<std::string> SplitTableRows(const std::string& html) {
    std::vector<std::string> rows;
    size_t p = 0;
    while (true) {
        size_t start = html.find("<tr", p);
        if (start == std::string::npos) break;
        size_t end = html.find("</tr>", start);
        if (end == std::string::npos) break;
        rows.push_back(html.substr(start, end - start + 5));
        p = end + 5;
    }
    return rows;
}

// 简易 HTML 实体反转义（&amp; &lt; &gt; &quot; &#39; &#xxx;），解析 OptiFine 下载页等栏目文本
static void HtmlUnescapeInPlace(std::string& s) {
    auto replaceAll = [&](const std::string& from, const std::string& to) {
        size_t p = 0;
        while ((p = s.find(from, p)) != std::string::npos) { s.replace(p, from.size(), to); p += to.size(); }
    };
    replaceAll("&amp;", "&"); replaceAll("&lt;", "<"); replaceAll("&gt;", ">");
    replaceAll("&quot;", "\""); replaceAll("&#39;", "'");
    std::regex numRe(R"(&#([0-9]+);)");
    std::string out;
    out.reserve(s.size());
    size_t pos = 0;
    for (auto it = std::sregex_iterator(s.begin(), s.end(), numRe), end = std::sregex_iterator(); it != end; ++it) {
        out.append(s, pos, (size_t)it->position() - pos);
        unsigned long cp = 0;
        try { cp = (unsigned long)std::stoul((*it)[1].str()); }
        catch (...) { out.append((*it)[0].str()); pos = (size_t)it->position() + (size_t)it->length(); continue; }
        std::string u8;  // UTF-8 编码 codepoint
        if (cp < 0x80) { u8.push_back((char)cp); }
        else if (cp < 0x800) { u8.push_back((char)(0xC0 | (cp >> 6))); u8.push_back((char)(0x80 | (cp & 0x3F))); }
        else if (cp < 0x10000) { u8.push_back((char)(0xE0 | (cp >> 12))); u8.push_back((char)(0x80 | ((cp >> 6) & 0x3F))); u8.push_back((char)(0x80 | (cp & 0x3F))); }
        else { u8.push_back((char)(0xF0 | (cp >> 18))); u8.push_back((char)(0x80 | ((cp >> 12) & 0x3F))); u8.push_back((char)(0x80 | ((cp >> 6) & 0x3F))); u8.push_back((char)(0x80 | (cp & 0x3F))); }
        out += u8;
        pos = (size_t)it->position() + (size_t)it->length();
    }
    out.append(s, pos, std::string::npos);
    s.swap(out);
}

// 取标签属性值：attr="v" 或 attr='v'
static bool TagAttr(const std::string& row, const std::string& attr, std::string& out) {
    size_t p = row.find(attr);
    if (p == std::string::npos) return false;
    p += attr.size();
    while (p < row.size() && (row[p] == ' ' || row[p] == '\t')) ++p;
    if (p >= row.size() || (row[p] != '=' && row[p] != '>')) return false;
    if (row[p] == '>') return false; // 属性后紧跟 >（无值），跳过
    ++p;
    while (p < row.size() && (row[p] == ' ' || row[p] == '\t')) ++p;
    if (p >= row.size()) return false;
    char q = row[p];
    if (q != '"' && q != '\'') return false;
    ++p;
    size_t end = row.find(q, p);
    if (end == std::string::npos) return false;
    out = row.substr(p, end - p);
    return true;
}

// 在 <td class='colX'>...</td> 片段内提取无标签纯文本
static std::string TdText(const std::string& cell) {
    std::string s = cell;
    // 去掉所有 <...> 标签
    std::regex tagRe("<[^>]*>");
    s = std::regex_replace(s, tagRe, "");
    HtmlUnescapeInPlace(s);
    // 压缩空白
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    if (a == std::string::npos) return std::string();
    return s.substr(a, b - a + 1);
}

// 按 class 名切出一个单元格：<td class='colX'...>...</td>
static bool ExtractCell(const std::string& row, const std::string& colClass, std::string& cellOut) {
    std::string marker = "class='" + colClass + "'";
    size_t p = row.find(marker);
    if (p == std::string::npos) {
        marker = std::string("class=\"") + colClass + "\"";
        p = row.find(marker);
        if (p == std::string::npos) return false;
    }
    size_t tdStart = row.rfind("<td", p);
    if (tdStart == std::string::npos) return false;
    size_t tdEnd = row.find("</td>", tdStart);
    if (tdEnd == std::string::npos) return false;
    cellOut = row.substr(tdStart, tdEnd - tdStart + 5);
    return true;
}

// 解析 OptiFine 官方下载页 https://optifine.net/downloads
// 按 <h2>Minecraft X.Y.Z</h2> 分节，节内表格每行抽 colFile / adloadx 文件名 / colForge / colDate。
// 对齐校验：同行栏目必须齐全（colFile + adloadx 文件名 + colForge + colDate）；该 MC 节条目数过少（<2）判整节失败。
// 返回统一条目数组；解析失败或某节不合格时该节被丢弃（无任何合格节时返回空数组，调用方回退镜像）。
static Json ParseOptifineDownloadsHtml(const std::string& html) {
    Json result = Json::array();
    // 1) 找出所有 <h2>Minecraft X.Y.Z</h2> 的绝对位置
    struct H2 { size_t pos; std::string mc; };
    std::vector<H2> h2s;
    {
        size_t hp = 0;
        while (true) {
            size_t b = html.find("<h2>Minecraft ", hp);
            if (b == std::string::npos) break;
            size_t e = html.find("</h2>", b);
            if (e == std::string::npos) break;
            std::string mc = html.substr(b + 14, e - b - 14);
            size_t tb = mc.find_first_not_of(" \t\r\n"); size_t te = mc.find_last_not_of(" \t\r\n");
            if (tb != std::string::npos && te >= tb) mc = mc.substr(tb, te - tb + 1);
            h2s.push_back({ b, mc });
            hp = e + 6;
        }
    }
    // 2) 逐行归属到 MC 段：行之前最近的 h2 即其 MC
    std::vector<std::pair<std::string, std::vector<Json>>> sections;
    for (const auto& row : SplitTableRows(html)) {
        size_t rowPos = html.find(row, 0);
        if (rowPos == std::string::npos) continue;
        std::string mcForRow;
        for (const auto& h : h2s) { if (h.pos < rowPos) mcForRow = h.mc; else break; }
        if (mcForRow.empty()) continue;
        // 3) 抽栏目：colFile / adloadx 文件名 / colForge / colDate 必须全齐（对齐校验 = 行栏目一致性）
        std::string fileCell, forgeCell, dateCell;
        bool okFile = ExtractCell(row, "colFile", fileCell);
        bool okForge = ExtractCell(row, "colForge", forgeCell);
        bool okDate = ExtractCell(row, "colDate", dateCell);
        if (!okFile || !okForge || !okDate) continue; // 栏目缺失 -> 该行无效
        const std::string marker = "adloadx?f=";
        size_t mp = row.find(marker);
        if (mp == std::string::npos) continue; // 无真实下载链接 -> 该行无效
        std::string fname = row.substr(mp + marker.size(), row.find_first_of("'\"&", mp + marker.size()) - mp - marker.size());
        if (fname.empty()) continue;
        if (fname.back() == '/') fname.pop_back();
        bool isPreview = fname.rfind("preview_", 0) == 0;
        std::string base = isPreview ? fname.substr(8) : fname;
        if (base.rfind("OptiFine_", 0) != 0) continue;
        std::string rest = base.substr(8); // <mc>_<type>_<patch>.jar
        size_t d1 = rest.find('_');
        if (d1 == std::string::npos) continue;
        std::string mcv = rest.substr(0, d1);
        std::string rest2 = rest.substr(d1 + 1);
        size_t d2 = rest2.find('_');
        if (d2 == std::string::npos) continue;
        std::string type = rest2.substr(0, d2);
        std::string patch = rest2.substr(d2 + 1);
        size_t dot = patch.rfind(".jar");
        if (dot != std::string::npos) patch = patch.substr(0, dot);
        if (type.empty() || patch.empty()) continue;
        // forge 兼容信息三态：any / incompatible / 版本串（#NNN 或完整版号）
        std::string forgeText = TdText(forgeCell);
        std::string forgeCompat = "any";
        std::string fw = forgeText;
        std::transform(fw.begin(), fw.end(), fw.begin(), ::tolower);
        if (fw.find("n/a") != std::string::npos) {
            forgeCompat = "incompatible";
        } else {
            std::regex numRe(R"(#?[0-9][0-9.]*)");
            std::smatch nm;
            if (std::regex_search(forgeText, nm, numRe)) {
                std::string v = nm[0].str();
                forgeCompat = v; // 保留原样；# 前缀表示"仅构建号"（与完整版号区分）
            }
        }
        std::string built = (forgeCompat.size() > 1 && forgeCompat[0] == '#') ? forgeCompat.substr(1) : "";
        Json o = Json::object();
        o["displayName"] = TdText(fileCell);
        o["filename"] = fname;
        o["type"] = type;
        o["patch"] = patch;
        o["mcversion"] = mcv;
        o["isPreview"] = isPreview;
        o["forgeCompatibility"] = forgeCompat;
        o["forgeBuilt"] = built;
        o["date"] = TdText(dateCell);
        bool foundSec = false;
        for (auto& s : sections) if (s.first == mcv) { s.second.push_back(o); foundSec = true; break; }
        if (!foundSec) sections.push_back({ mcv, { o } });
    }
    // 4) 汇总：仅保留条目数>=2 的 MC 节（该节校验通过）；过少视为该 MC 解析失败
    for (auto& s : sections) {
        if (s.second.size() < 2) continue;
        for (auto& o : s.second) result.asArray().push_back(o);
    }
    return result;
}

// 按版本列表源（官方/镜像/自动）决定 OptiFine 双源竞争结果。
// official 先启动，5000ms 未返回则放行镜像源；先成功者胜出。返回统一条目数组（空=双双失败）。
static Json OptifineListDualSource(const std::string& mcVersion, int* usedSource) {
    std::string source = ResolveSource("version"); // official / mirror / auto
    auto fetchOfficial = [&]() -> Json {
        std::string html = HttpFetchText(L"https://optifine.net/downloads");
        if (html.empty()) return Json::array();
        return ParseOptifineDownloadsHtml(html);
    };
    auto fetchMirror = [&]() -> Json {
        std::string url = "https://bmclapi2.bangbang93.com/optifine/" + BmclOptifineVersionPath(mcVersion);
        std::string text = HttpFetchText(lxe::Utf8ToWide(url));
        Json list = Json::array();
        if (!text.empty()) {
            try {
                Json data = Json::parse(text);
                if (data.isArray()) {
                    for (const auto& item : data.asArray()) {
                        if (!item.isObject()) continue;
                        std::string type, patch, filename;
                        if (item.contains("type") && item.at("type").isString()) type = item.at("type").asString();
                        if (item.contains("patch") && item.at("patch").isString()) patch = item.at("patch").asString();
                        if (item.contains("filename") && item.at("filename").isString()) filename = item.at("filename").asString();
                        if (type.empty() || patch.empty()) continue;
                        std::string forgeCompat = "any";
                        if (item.contains("forge") && item.at("forge").isString()) {
                            std::string fw = item.at("forge").asString();
                            std::transform(fw.begin(), fw.end(), fw.begin(), ::tolower);
                            if (fw.find("n/a") != std::string::npos) forgeCompat = "incompatible";
                            else {
                                // "Forge #1902" → "#1902"（仅构建号）；纯版本串保留；空串视为不兼容
                                std::regex hashRe(R"(#([0-9]+))");
                                std::smatch hm;
                                if (std::regex_search(fw, hm, hashRe)) forgeCompat = "#" + hm[1].str();
                                else if (fw.find("forge") != std::string::npos || fw.empty()) forgeCompat = "incompatible";
                            }
                        } else if (item.contains("forge") && item.at("forge").isNull()) forgeCompat = "incompatible";
                        Json o = Json::object();
                        o["displayName"] = type + " " + patch;
                        o["filename"] = filename;
                        o["type"] = type;
                        o["patch"] = patch;
                        o["mcversion"] = mcVersion;
                        o["isPreview"] = filename.rfind("preview_", 0) == 0;
                        o["forgeCompatibility"] = forgeCompat;
                        o["forgeBuilt"] = (forgeCompat.size() > 1 && forgeCompat[0] == '#') ? forgeCompat.substr(1) : "";
                        o["date"] = "";
                        list.asArray().push_back(o);
                    }
                }
            } catch (...) {}
        }
        return list;
    };
    // 过滤：仅保留当前 MC 的条目（官方源是全量，需按 mcVersion 过滤；1.8 与 1.8.0 视作同一版本）
    auto filterByMc = [&](Json list) -> Json {
        Json out = Json::array();
        if (mcVersion.empty()) return list;
        std::string want = NormalizeMcVersionForCompare(mcVersion);
        for (const auto& o : list.asArray()) {
            if (o.contains("mcversion") && o.at("mcversion").isString() &&
                NormalizeMcVersionForCompare(o.at("mcversion").asString()) == want)
                out.asArray().push_back(o);
        }
        return out;
    };
    if (source == "mirror") {
        if (usedSource) *usedSource = 2;
        return filterByMc(fetchMirror());
    }
    // official 或 auto：先官方，超时才放行镜像
    auto fut = std::async(std::launch::async, fetchOfficial);
    auto status = fut.wait_for(std::chrono::milliseconds(5000));
    if (status == std::future_status::ready) {
        Json r = fut.get();
        Json f = filterByMc(r);
        if (f.asArray().size() > 0) { if (usedSource) *usedSource = 1; return f; }
    }
    Json m = filterByMc(fetchMirror());
    if (m.asArray().size() > 0) { if (usedSource) *usedSource = 2; return m; }
    if (status == std::future_status::ready) { Json r = fut.get(); Json f = filterByMc(r); if (f.asArray().size() > 0) { if (usedSource) *usedSource = 1; return f; } }
    if (usedSource) *usedSource = 0;
    return Json::array();
}

// 加载器安装核心（文件作用域 static，供后台线程随时调用）：
// mc.installLoader 经后台线程调用，整合包导入(mc.installModpack) 在自身后台线程内直接调用——两者复用同一套安装逻辑
static std::function<void(Bridge&, int, const std::string&, const std::string&, const std::string&, const std::string&, const Json&, bool)> g_installLoaderImpl;

void RegisterLoaderServices(Bridge& bridge) {
    // ===================== mc.loaderVersions：官方源加载器版本列表 =====================
    bridge.RegisterAsync("mc.loaderVersions", [](const Json& params, const std::function<void(HandlerResult)>& done) {
        std::string loaderId, mcVersion;
        if (params.isObject() && params.contains("loaderId") && params.at("loaderId").isString())
            loaderId = params.at("loaderId").asString();
        if (params.isObject() && params.contains("mcVersion") && params.at("mcVersion").isString())
            mcVersion = params.at("mcVersion").asString();
        if (loaderId.empty()) { done(Err(-32602, "missing loaderId")); return; }

        // 后台线程抓取+解析，避免阻塞 WebView2 消息线程（否则网络慢时整个桥卡死、前端全部转圈）
        std::thread([loaderId, mcVersion, done]() {
        Json list = Json::array();
        try {
            if (loaderId == "fabric") {
                // https://meta.fabricmc.net/v2/versions/loader/{mc}
                std::string url = "https://meta.fabricmc.net/v2/versions/loader/" + mcVersion;
                std::string text = HttpFetchText(lxe::Utf8ToWide(url));
                if (!text.empty()) {
                    Json data = Json::parse(text);
                    if (data.isArray()) {
                        for (const auto& item : data.asArray()) {
                            Json o = Json::object();
                            if (item.contains("loader") && item.at("loader").isObject()) {
                                const auto& l = item.at("loader");
                                if (l.contains("version") && l.at("version").isString())
                                    o["version"] = l.at("version").asString();
                                if (l.contains("stable") && l.at("stable").isBool())
                                    o["stable"] = l.at("stable").asBool();
                            }
                            if (item.contains("intermediary") && item.at("intermediary").isObject() &&
                                item.at("intermediary").contains("version") && item.at("intermediary").at("version").isString())
                                o["intermediary"] = item.at("intermediary").at("version").asString();
                            list.asArray().push_back(o);
                        }
                    }
                }
            } else if (loaderId == "forge") {
                // https://files.minecraftforge.net/net/minecraftforge/forge/index_{mc}.html
                // 教程 Part 2：1.7.10-pre4 之类的版本要把 - 换成 _
                std::string fmc = mcVersion;
                std::replace(fmc.begin(), fmc.end(), '-', '_');
                std::string url = "https://files.minecraftforge.net/net/minecraftforge/forge/index_" + fmc + ".html";
                std::string text = HttpFetchText(lxe::Utf8ToWide(url));
                if (!text.empty()) {
                    for (const auto& row : SplitTableRows(text)) {
                        std::smatch m;
                        std::regex verRe("<td class=\"download-version\">\\s*([0-9][0-9.]*)");
                        if (!std::regex_search(row, m, verRe)) continue;
                        std::string version = m[1].str();
                        Json o = Json::object();
                        o["version"] = version;
                        o["build"] = version;
                        std::regex timeRe("<td class=\"download-time\" title=\"([^\"]+)\"");
                        if (std::regex_search(row, m, timeRe)) o["modified"] = m[1].str();
                        o["latest"] = row.find("promo-latest") != std::string::npos;
                        o["recommended"] = row.find("promo-recommended") != std::string::npos;
                        Json files = Json::array();
                        std::regex urlRe("https://maven\\.minecraftforge\\.net/[^\"']+installer\\.jar");
                        if (std::regex_search(row, m, urlRe)) {
                            Json f = Json::object();
                            f["classifier"] = "installer";
                            f["url"] = m[0].str();
                            files.asArray().push_back(f);
                        }
                        o["files"] = files;
                        list.asArray().push_back(o);
                    }
                }
} else if (loaderId == "optifine") {
                // 官方下载页 HTML 解析 + BMCLAPI 镜像 双源竞争（设计 §2）：按"版本列表源"设置定先后与等待预算，
                // 先成功者胜出。HTML 解析带栏位对齐校验与条目数下限校验，失败自动回退镜像。
                int usedSrc = 0;
                Json items = OptifineListDualSource(mcVersion, &usedSrc);
                for (const auto& o : items.asArray()) {
                    if (!o.isObject()) continue;
                    std::string type, patch, filename, fcompat;
                    if (o.contains("type") && o.at("type").isString()) type = o.at("type").asString();
                    if (o.contains("patch") && o.at("patch").isString()) patch = o.at("patch").asString();
                    if (o.contains("filename") && o.at("filename").isString()) filename = o.at("filename").asString();
                    if (o.contains("forgeCompatibility") && o.at("forgeCompatibility").isString())
                        fcompat = o.at("forgeCompatibility").asString();
                    if (type.empty() || patch.empty()) continue;
                    Json item = Json::object();
                    item["version"] = type + "_" + patch;
                    item["type"] = type;
                    item["patch"] = patch;
                    item["filename"] = filename;
                    item["size"] = 0;
                    item["mcversion"] = mcVersion.empty() ? (o.contains("mcversion") && o.at("mcversion").isString() ? o.at("mcversion").asString() : std::string()) : mcVersion;
                    item["displayName"] = o.contains("displayName") ? o.at("displayName") : Json(type + " " + patch);
                    item["isPreview"] = o.contains("isPreview") ? o.at("isPreview") : Json(false);
                    item["forgeCompatibility"] = fcompat.empty() ? Json("any") : o.at("forgeCompatibility");
                    item["forgeBuilt"] = o.contains("forgeBuilt") ? o.at("forgeBuilt") : Json("");
                    item["date"] = o.contains("date") ? o.at("date") : Json("");
                    item["source"] = std::to_string(usedSrc); // 1=官方 2=镜像 0=都失败
                    list.asArray().push_back(item);
                }
                // 双源全部失败时给出明确错误
                if (list.asArray().empty()) {
                    done(Err(-32005, "OptiFine 版本列表获取失败（官方源与 BMCLAPI 镜像均不可用）"));
                    return;
                }
            } else if (loaderId == "neoforge") {
                // NeoForge：官方 Maven 版本列表 + 镜像加速（devskill 允许 BMCLAPI 加速 Neoforge 列表）。
                // 新版坐标 net/neoforged/neoforge（20.4+，版本名如 20.4.237-beta），旧版坐标 net/neoforged/forge
                // （1.20.1~1.20.3，版本名如 1.20.1-47.1.106，前缀即 MC 版本）。
                auto parseMavenVersions = [&](const std::string& xml, const std::string& mc) {
                    // 提取 <version>...</version>
                    std::vector<std::string> vers;
                    size_t p = 0;
                    while (true) {
                        size_t b = xml.find("<version>", p);
                        if (b == std::string::npos) break;
                        size_t e = xml.find("</version>", b + 9);
                        if (e == std::string::npos) break;
                        vers.push_back(xml.substr(b + 9, e - b - 9));
                        p = e + 10;
                    }
                    // 过滤属于该 MC 版本的条目：旧式前缀 {mc}-；新式两段头与 MC 映射（见下）
                    auto mcFromHeader = [](const std::string& hdr) -> std::string {
                        if (hdr == "20.4") return "1.20.4";
                        if (hdr == "20.5") return "1.20.5";
                        if (hdr == "21.0") return "1.20.5";
                        if (hdr == "21.1") return "1.20.6";
                        if (hdr == "21.3") return "1.21";
                        if (hdr == "21.4") return "1.21.1";
                        if (hdr == "21.5") return "1.21.2";
                        if (hdr == "21.6") return "1.21.4";
                        if (hdr == "21.7") return "1.21.5";
                        if (hdr == "21.8") return "1.21.6";
                        if (hdr == "21.9") return "1.21.7";
                        if (hdr == "22.0") return "1.21.9";
                        if (hdr == "25.0") return "1.21.10";
                        if (hdr == "25.1") return "1.21.11";
                        return "";
                    };
                    for (const auto& v : vers) {
                        std::string mcOf = mc;
                        bool stable = true;
                        std::string lv = v;
                        std::transform(lv.begin(), lv.end(), lv.begin(), ::tolower);
                        if (lv.find("-beta") != std::string::npos || lv.find("-alpha") != std::string::npos ||
                            lv.find("-pre") != std::string::npos || lv.find("-snapshot") != std::string::npos)
                            stable = false;
                        // 旧式 1.20.1-47.1.106：前缀即 MC 版本
                        size_t dash = v.find('-');
                        if (dash != std::string::npos) {
                            std::string pre = v.substr(0, dash);
                            if (pre.find('.') != std::string::npos && pre.find("repack") == std::string::npos) {
                                bool numeric = !pre.empty() && std::all_of(pre.begin(), pre.end(),
                                    [](char c) { return (c >= '0' && c <= '9') || c == '.'; });
                                if (numeric && mc.find("1.") == 0 && pre != "20.4" && pre != "21.0" &&
                                    pre.find("20.4") != 0 && pre.find("21.0") != 0 && pre.find("21.") != 0) {
                                    mcOf = pre; // 旧式：MC 版本在版本名里
                                }
                            }
                        }
                        if (!mc.empty() && mcOf != mc) continue;
                        Json o = Json::object();
                        o["version"] = v;
                        o["build"] = v;
                        o["stable"] = stable;
                        if (!mcOf.empty()) o["mcVersion"] = mcOf;
                        list.asArray().push_back(o);
                    }
                };
                // 先官方 Maven，再镜像
                std::string xml = HttpFetchText(lxe::Utf8ToWide(
                    "https://maven.neoforged.net/releases/net/neoforged/neoforge/maven-metadata.xml"));
                bool okMaven = !xml.empty();
                if (okMaven) {
                    parseMavenVersions(xml, mcVersion);
                    // 新版坐标可能不含旧 MC（<20.4）；对 1.20.1~1.20.3 用旧坐标 net/neoforged/forge
                    if (list.asArray().empty() && (mcVersion == "1.20.1" || mcVersion == "1.20.2" || mcVersion == "1.20.3")) {
                        std::string xmlOld = HttpFetchText(lxe::Utf8ToWide(
                            "https://maven.neoforged.net/releases/net/neoforged/forge/maven-metadata.xml"));
                        if (!xmlOld.empty()) parseMavenVersions(xmlOld, mcVersion);
                    }
                }
                if (list.asArray().empty()) {
                    // 镜像加速：/neoforge/list/{mc} 返回版本名数组
                    std::string t = HttpFetchText(lxe::Utf8ToWide("https://bmclapi2.bangbang93.com/neoforge/list/" + mcVersion));
                    if (!t.empty()) {
                        try {
                            Json data = Json::parse(t);
                            if (data.isArray()) {
                                for (const auto& it : data.asArray()) {
                                    if (it.isString()) {
                                        Json o = Json::object();
                                        std::string v = it.asString();
                                        o["version"] = v;
                                        o["build"] = v;
                                        o["stable"] = true;
                                        if (!mcVersion.empty()) o["mcVersion"] = mcVersion;
                                        list.asArray().push_back(o);
                                    }
                                }
                            }
                        } catch (...) {}
                    }
                }
            } else if (loaderId == "liteloader") {
                // LiteLoader：versions.json，包含 artefacts（正式版）与 snapshots（快照）两类条目。
                // 官方源 dl.liteloader.com，失败回退 BMCLAPI maven 镜像（devskill 允许镜像加速 LiteLoader 列表）。
                std::string url = "https://dl.liteloader.com/versions/versions.json";
                std::string text = HttpFetchText(lxe::Utf8ToWide(url));
                if (text.empty())
                    text = HttpFetchText(lxe::Utf8ToWide("https://bmclapi2.bangbang93.com/maven/com/mumfrey/liteloader/versions.json"));
                if (!text.empty()) {
                    try {
                        Json data = Json::parse(text);
                        if (data.isObject() && data.contains("versions") && data.at("versions").isArray()) {
                            for (const auto& v : data.at("versions").asArray()) {
                                if (!v.isObject() || !v.contains("name") || !v.at("name").isString()) continue;
                                if (v.at("name").asString() != mcVersion) continue;
                                auto collect = [&](const Json& arr, bool snapshot) {
                                    if (!arr.isArray()) return;
                                    for (const auto& a : arr.asArray()) {
                                        if (!a.isObject()) continue;
                                        if (!a.contains("file") || !a.at("file").isString()) continue;
                                        Json o = Json::object();
                                        std::string ver = a.contains("version") && a.at("version").isString()
                                            ? a.at("version").asString() : a.at("file").asString();
                                        o["version"] = ver;
                                        o["build"] = ver;
                                        o["file"] = a.at("file").asString();
                                        if (snapshot) o["stable"] = false;
                                        else o["stable"] = true;
                                        if (a.contains("branch") && a.at("branch").isString())
                                            o["branch"] = a.at("branch").asString();
                                        if (a.contains("tweakClass") && a.at("tweakClass").isString())
                                            o["tweakClass"] = a.at("tweakClass").asString();
                                        if (snapshot) o["snapshot"] = true;
                                        list.asArray().push_back(o);
                                    }
                                };
                                if (v.contains("artefacts")) collect(v.at("artefacts"), false);
                                if (v.contains("snapshots")) collect(v.at("snapshots"), true);
                            }
                        }
                    } catch (...) {}
                }
            }
                } catch (...) {}
            Json result = Json::object();
            result["loaderId"] = loaderId;
        result["list"] = list;
        done(Ok(result));
        }).detach();
    });

    // ===================== mc.searchMods：Modrinth 官方搜索（模组/整合包） =====================
    // Modrinth 官方 API（无需 API Key）。projectType: 'mod' | 'modpack'，gameVersion 可选过滤
    // —— 中文译名反查（Req3）：查询串含汉字时先向 MC百科查英文名，再按英文名搜 Modrinth ——
    // 检测 UTF-8 字符串是否含 CJK 汉字（主区 E4-E9 开头；扩展区/部首区次要场景忽略）
    static auto QueryHasCJK = [](const std::string& s) -> bool {
        for (size_t i = 0; i + 2 < s.size(); ++i) {
            unsigned char c = (unsigned char)s[i];
            if ((c >= 0xE4 && c <= 0xE9) || (c >= 0xF0 && c <= 0xF9)) return true;
        }
        return false;
    };
    // 解析 MC百科搜索结果页 → {中文名, 英文名} 列表（返回前 maxCount 条）
    // 页面为服务端直出、无登录门槛；每行 <div class="result-item"> 内 anchor 的末尾圆括号即英文名
    static auto McModCnEnNames = [](const std::string& cjkQuery, int maxCount = 8) -> std::vector<std::pair<std::string, std::string>> {
        std::vector<std::pair<std::string, std::string>> out;
        std::string url = "https://search.mcmod.cn/s?key=" + UrlEncode(cjkQuery) + "&filter=1";
        std::string text = HttpFetchText(lxe::Utf8ToWide(url));
        if (text.empty()) return out;
        const std::string rowMark = "<div class=\"result-item\">";
        size_t pos = 0;
        while ((int)out.size() < maxCount) {
            size_t start = text.find(rowMark, pos);
            if (start == std::string::npos) break;
            size_t end = text.find(rowMark, start + rowMark.size());
            std::string row = end == std::string::npos ? text.substr(start) : text.substr(start, end - start);
            pos = end == std::string::npos ? text.size() : end;
            size_t href = row.find("https://www.mcmod.cn/class/");
            if (href == std::string::npos) continue;
            size_t gt = row.find('>', href);
            if (gt == std::string::npos) continue;
            size_t close = row.find("</a>", gt);
            if (close == std::string::npos) continue;
            std::string name = row.substr(gt + 1, close - gt - 1);
            std::string clean;
            for (size_t i = 0; i < name.size(); ++i) {
                if (name[i] == '<') { while (i < name.size() && name[i] != '>') ++i; continue; }
                clean.push_back(name[i]);
            }
            auto trims = [](std::string& x) { size_t a = x.find_first_not_of(" \t\r\n"); size_t b = x.find_last_not_of(" \t\r\n"); x = (a == std::string::npos) ? std::string() : x.substr(a, b - a + 1); };
            size_t lp = clean.rfind('(');
            size_t rp = clean.rfind(')');
            if (lp == std::string::npos || rp == std::string::npos || rp <= lp) continue;
            std::string en = clean.substr(lp + 1, rp - lp - 1);
            std::string cn = clean.substr(0, lp);
            trims(cn); trims(en);
            if (cn.empty() || en.empty()) continue;
            if (QueryHasCJK(en)) continue;  // 英文名不应含汉字
            bool dup = false;
            for (const auto& p : out) if (p.second == en || p.first == cn) { dup = true; break; }
            if (dup) continue;
            out.push_back({ cn, en });
        }
        return out;
    };

    bridge.RegisterAsync("mc.searchMods", [](const Json& params, const std::function<void(HandlerResult)>& done) {
        std::string query, projectType, gameVersion;
        int limit = 30, offset = 0;
        if (params.isObject() && params.contains("query") && params.at("query").isString())
            query = params.at("query").asString();
        if (params.isObject() && params.contains("projectType") && params.at("projectType").isString())
            projectType = params.at("projectType").asString();
        if (projectType.empty()) projectType = "mod";
        if (params.isObject() && params.contains("gameVersion") && params.at("gameVersion").isString())
            gameVersion = params.at("gameVersion").asString();
        if (params.isObject() && params.contains("limit") && params.at("limit").isNumber()) {
            limit = (int)params.at("limit").asNumber();
            if (limit < 1) limit = 1;
            if (limit > 100) limit = 100;
        }
        if (params.isObject() && params.contains("offset") && params.at("offset").isNumber()) {
            offset = (int)params.at("offset").asNumber();
            if (offset < 0) offset = 0;
        }
        std::thread([query, projectType, gameVersion, limit, offset, done]() {
            try {
            // facets: [["project_type:mod"],["versions:1.20.1"]]（gameVersion 非空时追加版本过滤）
            // 修复：同时加入主版本标签（如 1.20.4 时同时匹配 versions:1.20），否则只精确匹配导致结果极少
            std::string facets = "%5B%5B%22project_type%3A" + projectType + "%22%5D";
            if (!gameVersion.empty()) {
                std::string shortV;
                {
                    std::string g = gameVersion;
                    size_t p1 = g.find('.');
                    if (p1 != std::string::npos) {
                        size_t p2 = g.find('.', p1 + 1);
                        if (p2 != std::string::npos) {
                            std::string majorMinor = g.substr(0, p2);
                            if (majorMinor != g) shortV = majorMinor;
                        }
                    }
                }
                if (!shortV.empty())
                    facets += "%2C%5B%22versions%3A" + UrlEncode(gameVersion) + "%22%2C%22versions%3A" + UrlEncode(shortV) + "%22%5D";
                else
                    facets += "%2C%5B%22versions%3A" + UrlEncode(gameVersion) + "%22%5D";
            }
            facets += "%5D";
            // Req3 中文译名：查询含汉字时先用 MC百科反查英文名，再按英文名（含候选）搜 Modrinth
            std::vector<std::pair<std::string, std::string>> searchTerms;  // {查询词, 中文译名}
            bool translated = false;
            if (QueryHasCJK(query)) {
                auto names = McModCnEnNames(query);
                for (const auto& p : names) {
                    if (p.second.empty()) continue;
                    searchTerms.push_back(p);  // first = 英文名, second = 中文名
                }
                translated = !searchTerms.empty();
            }
            if (searchTerms.empty()) searchTerms.push_back({ query, "" });
            // 单查询词（非中文）时保持原分页语义；多候选（译名）时合并去重后按 offset/limit 切片
            auto fetchHits = [&](const std::string& term, int lim) -> Json {
                std::string u = "https://api.modrinth.com/v2/search?query=" + UrlEncode(term)
                    + "&facets=" + facets
                    + "&limit=" + std::to_string(lim)
                    + "&offset=0"
                    + "&index=relevance";
                std::string t = HttpFetchText(lxe::Utf8ToWide(u));
                Json arr = Json::array();
                if (!t.empty()) {
                    try {
                        Json data = Json::parse(t);
                        if (data.contains("hits") && data.at("hits").isArray()) {
                            for (const auto& hit : data.at("hits").asArray()) {
                                if (!hit.isObject()) continue;
                                Json o = Json::object();
                                if (hit.contains("project_id") && hit.at("project_id").isString()) o["id"] = hit.at("project_id").asString();
                                if (hit.contains("slug") && hit.at("slug").isString()) o["slug"] = hit.at("slug").asString();
                                if (hit.contains("title") && hit.at("title").isString()) o["title"] = hit.at("title").asString();
                                if (hit.contains("description") && hit.at("description").isString()) o["description"] = hit.at("description").asString();
                                if (hit.contains("author") && hit.at("author").isString()) o["author"] = hit.at("author").asString();
                                if (hit.contains("downloads") && hit.at("downloads").isNumber()) o["downloads"] = (long long)hit.at("downloads").asNumber();
                                if (hit.contains("follows") && hit.at("follows").isNumber()) o["follows"] = (long long)hit.at("follows").asNumber();
                                if (hit.contains("date_created") && hit.at("date_created").isString()) o["published"] = hit.at("date_created").asString();
                                if (hit.contains("date_modified") && hit.at("date_modified").isString()) o["updated"] = hit.at("date_modified").asString();
                                if (hit.contains("icon_url") && hit.at("icon_url").isString()) o["icon"] = hit.at("icon_url").asString();
                                if (hit.contains("latest_version") && hit.at("latest_version").isString()) o["latestVersion"] = hit.at("latest_version").asString();
                                if (hit.contains("game_versions") && hit.at("game_versions").isArray()) {
                                    Json gv = Json::array();
                                    for (const auto& v : hit.at("game_versions").asArray())
                                        if (v.isString()) gv.asArray().push_back(v.asString());
                                    o["gameVersions"] = gv;
                                }
                                if (hit.contains("loaders") && hit.at("loaders").isArray()) {
                                    Json lds = Json::array();
                                    for (const auto& v : hit.at("loaders").asArray())
                                        if (v.isString()) lds.asArray().push_back(v.asString());
                                    o["loaders"] = lds;
                                }
                                if (hit.contains("categories") && hit.at("categories").isArray()) {
                                    Json cats = Json::array();
                                    for (const auto& c : hit.at("categories").asArray())
                                        if (c.isString()) cats.asArray().push_back(c.asString());
                                    o["categories"] = cats;
                                }
                                arr.asArray().push_back(o);
                            }
                        }
                    } catch (...) {}
                }
                return arr;
            };
            Json list = Json::array();
            long long total = 0;
            if (!translated) {
                // 普通（非中文）搜索：与原逻辑一致，offset/limit 直接作为 Modrinth 分页参数
                std::string url = "https://api.modrinth.com/v2/search?query=" + UrlEncode(query)
                    + "&facets=" + facets
                    + "&limit=" + std::to_string(limit)
                    + "&offset=" + std::to_string(offset)
                    + "&index=relevance";
                std::string text = HttpFetchText(lxe::Utf8ToWide(url));
                if (!text.empty()) {
                    try {
                        Json data = Json::parse(text);
                        if (data.contains("total_hits") && data.at("total_hits").isNumber())
                            total = (long long)data.at("total_hits").asNumber();
                        if (data.contains("hits") && data.at("hits").isArray()) {
                            for (const auto& hit : data.at("hits").asArray()) {
                                if (!hit.isObject()) continue;
                                Json o = Json::object();
                                if (hit.contains("project_id") && hit.at("project_id").isString()) o["id"] = hit.at("project_id").asString();
                                if (hit.contains("slug") && hit.at("slug").isString()) o["slug"] = hit.at("slug").asString();
                                if (hit.contains("title") && hit.at("title").isString()) o["title"] = hit.at("title").asString();
                                if (hit.contains("description") && hit.at("description").isString()) o["description"] = hit.at("description").asString();
                                if (hit.contains("author") && hit.at("author").isString()) o["author"] = hit.at("author").asString();
                                if (hit.contains("downloads") && hit.at("downloads").isNumber()) o["downloads"] = (long long)hit.at("downloads").asNumber();
                                if (hit.contains("follows") && hit.at("follows").isNumber()) o["follows"] = (long long)hit.at("follows").asNumber();
                                if (hit.contains("date_created") && hit.at("date_created").isString()) o["published"] = hit.at("date_created").asString();
                                if (hit.contains("date_modified") && hit.at("date_modified").isString()) o["updated"] = hit.at("date_modified").asString();
                                if (hit.contains("icon_url") && hit.at("icon_url").isString()) o["icon"] = hit.at("icon_url").asString();
                                if (hit.contains("latest_version") && hit.at("latest_version").isString()) o["latestVersion"] = hit.at("latest_version").asString();
                                if (hit.contains("game_versions") && hit.at("game_versions").isArray()) {
                                    Json gv = Json::array();
                                    for (const auto& v : hit.at("game_versions").asArray())
                                        if (v.isString()) gv.asArray().push_back(v.asString());
                                    o["gameVersions"] = gv;
                                }
                                if (hit.contains("loaders") && hit.at("loaders").isArray()) {
                                    Json lds = Json::array();
                                    for (const auto& v : hit.at("loaders").asArray())
                                        if (v.isString()) lds.asArray().push_back(v.asString());
                                    o["loaders"] = lds;
                                }
                                if (hit.contains("categories") && hit.at("categories").isArray()) {
                                    Json cats = Json::array();
                                    for (const auto& c : hit.at("categories").asArray())
                                        if (c.isString()) cats.asArray().push_back(c.asString());
                                    o["categories"] = cats;
                                }
                                list.asArray().push_back(o);
                            }
                        }
                    } catch (...) {}
                }
            } else {
                // 译名搜索：每个候选英文名各拉 limit+offset 条，按 project_id 去重合并，再切片
                int want = limit + offset;
                std::map<std::string, Json> merged;
                std::vector<std::string> order;
                for (const auto& term : searchTerms) {
                    Json arr = fetchHits(term.first, want);
                    for (const auto& o : arr.asArray()) {
                        if (!o.isObject()) continue;
                        std::string id = o.contains("id") && o.at("id").isString() ? o.at("id").asString() : "";
                        if (id.empty()) continue;
                        if (merged.count(id)) continue;
                        Json copy = o;
                        if (!term.second.empty()) copy["zhName"] = term.second;  // 中文译名附给结果
                        merged[id] = copy;
                        order.push_back(id);
                    }
                }
                total = (long long)order.size();
                for (int i = offset; i < (int)order.size() && (int)list.asArray().size() < limit; ++i)
                    list.asArray().push_back(merged[order[i]]);
            }
            // 为每个结果拉取最新版本的必需前置（列表展示"N 个前置模组"；失败则省略，不影响列表）
            if (!list.asArray().empty()) {
                std::map<std::string, Json> depsByProj;
                std::vector<std::string> projIds;
                for (auto& o : list.asArray()) {
                    if (!o.isObject()) continue;
                    std::string pid0 = o.contains("id") && o.at("id").isString() ? o.at("id").asString() : "";
                    std::string lv = o.contains("latestVersion") && o.at("latestVersion").isString() ? o.at("latestVersion").asString() : "";
                    Json dArr = Json::array();
                    if (!lv.empty()) {
                        std::string vtext = HttpFetchText(lxe::Utf8ToWide("https://api.modrinth.com/v2/version/" + UrlEncode(lv)));
                        if (!vtext.empty()) {
                            try {
                                Json vd = Json::parse(vtext);
                                if (vd.isObject() && vd.contains("dependencies") && vd.at("dependencies").isArray()) {
                                    for (const auto& d : vd.at("dependencies").asArray()) {
                                        if (!d.isObject()) continue;
                                        if (!(d.contains("dependency_type") && d.at("dependency_type").isString() && d.at("dependency_type").asString() == "required")) continue;
                                        if (d.contains("project_id") && d.at("project_id").isString()) {
                                            std::string dpid = d.at("project_id").asString();
                                            bool exists = false;
                                            for (const auto& dd : dArr.asArray()) if (dd.isString() && dd.asString() == dpid) { exists = true; break; }
                                            if (!exists) dArr.asArray().push_back(dpid);
                                            if (std::find(projIds.begin(), projIds.end(), dpid) == projIds.end()) projIds.push_back(dpid);
                                        }
                                    }
                                }
                            } catch (...) {}
                        }
                    }
                    depsByProj[pid0] = dArr;
                }
                std::map<std::string, Json> projMeta;
                if (!projIds.empty()) {
                    std::string idsJson = "[";
                    for (size_t i = 0; i < projIds.size(); ++i) { if (i) idsJson += ","; idsJson += "\"" + projIds[i] + "\""; }
                    idsJson += "]";
                    std::string ptext = HttpFetchText(lxe::Utf8ToWide("https://api.modrinth.com/v2/projects?ids=" + UrlEncode(idsJson)));
                    if (!ptext.empty()) {
                        try {
                            Json pd = Json::parse(ptext);
                            if (pd.isArray()) {
                                for (const auto& p : pd.asArray()) {
                                    if (!p.isObject()) continue;
                                    Json meta = Json::object();
                                    if (p.contains("id") && p.at("id").isString()) meta["id"] = p.at("id").asString();
                                    if (p.contains("slug") && p.at("slug").isString()) meta["slug"] = p.at("slug").asString();
                                    if (p.contains("title") && p.at("title").isString()) meta["title"] = p.at("title").asString();
                                    if (meta.contains("id")) projMeta[meta.at("id").asString()] = meta;
                                }
                            }
                        } catch (...) {}
                    }
                }
                for (auto& o : list.asArray()) {
                    if (!o.isObject()) continue;
                    std::string pid0 = o.contains("id") && o.at("id").isString() ? o.at("id").asString() : "";
                    auto it = depsByProj.find(pid0);
                    if (it == depsByProj.end()) continue;
                    o["depsCount"] = (int)it->second.asArray().size();
                    Json outDeps = Json::array();
                    for (const auto& dp : it->second.asArray()) {
                        if (!dp.isString()) continue;
                        auto mi = projMeta.find(dp.asString());
                        if (mi != projMeta.end()) outDeps.asArray().push_back(mi->second);
                    }
                    o["deps"] = outDeps;
                }
            }
            Json result = Json::object();
            result["projectType"] = projectType;
            result["list"] = list;
            result["total"] = total;
            result["offset"] = offset;
            done(Ok(result));
            } catch (const std::exception& e) {
                done(Err(-32053, std::string("搜索模组异常：") + e.what()));
            } catch (...) {
                done(Err(-32053, "搜索模组未知异常"));
            }
        }).detach();
    });

    // ===================== mc.curseSearch：CurseForge 搜索（模组 / 材质包 / 光影） =====================
    // 需要 x-api-key（在 CurseForge Console 申请，前端设置页填入后经 apiKey 参数传入）。
    // classId：mod=6（Minecraft Mods）、resourcepack=12（Resource Packs）、shader=6552（Shader Packs）、modpack=4471。
    bridge.RegisterAsync("mc.curseSearch", [](const Json& params, const std::function<void(HandlerResult)>& done) {
        std::string query, projectType, gameVersion, apiKey;
        int limit = 20, offset = 0;
        if (params.isObject() && params.contains("query") && params.at("query").isString())
            query = params.at("query").asString();
        if (params.isObject() && params.contains("projectType") && params.at("projectType").isString())
            projectType = params.at("projectType").asString();
        if (projectType.empty()) projectType = "mod";
        if (params.isObject() && params.contains("gameVersion") && params.at("gameVersion").isString())
            gameVersion = params.at("gameVersion").asString();
        if (params.isObject() && params.contains("apiKey") && params.at("apiKey").isString())
            apiKey = params.at("apiKey").asString();
        if (params.isObject() && params.contains("limit") && params.at("limit").isNumber()) {
            limit = (int)params.at("limit").asNumber();
            if (limit < 1) limit = 1;
            if (limit > 100) limit = 100;
        }
        if (params.isObject() && params.contains("offset") && params.at("offset").isNumber()) {
            offset = (int)params.at("offset").asNumber();
            if (offset < 0) offset = 0;
        }
        std::thread([query, projectType, gameVersion, apiKey, limit, offset, done]() {
            try {
                if (apiKey.empty()) { done(Err(-32060, "未配置 CurseForge API Key，请在 设置-下载 中填写")); return; }
                int classId = 6;
                if (projectType == "resourcepack") classId = 12;
                else if (projectType == "shader") classId = 6552;
                else if (projectType == "modpack") classId = 4471;
                std::string url = "https://api.curseforge.com/v1/mods/search?gameId=432"
                    + std::string("&classId=") + std::to_string(classId)
                    + "&pageSize=" + std::to_string(limit)
                    + "&index=" + std::to_string(offset)
                    + "&sortField=6&sortOrder=desc"; // 6 = 总下载量
                if (!query.empty()) url += "&searchFilter=" + UrlEncode(query);
                if (!gameVersion.empty()) url += "&gameVersion=" + UrlEncode(gameVersion);
                std::wstring header = L"x-api-key: " + lxe::Utf8ToWide(apiKey) + L"\r\nAccept: application/json\r\n";
                std::string text = HttpFetchTextWithHeader(lxe::Utf8ToWide(url), header);
                Json list = Json::array();
                long long total = 0;
                if (!text.empty()) {
                    try {
                        Json data = Json::parse(text);
                        if (data.contains("data") && data.at("data").isArray()) {
                            const auto& arr = data.at("data").asArray();
                            total = (long long)arr.size();
                            for (const auto& p : arr) {
                                if (!p.isObject()) continue;
                                Json o = Json::object();
                                if (p.contains("id") && p.at("id").isNumber()) o["id"] = std::to_string((long long)p.at("id").asNumber());
                                if (p.contains("slug") && p.at("slug").isString()) o["slug"] = p.at("slug").asString();
                                if (p.contains("name") && p.at("name").isString()) o["title"] = p.at("name").asString();
                                if (p.contains("summary") && p.at("summary").isString()) o["description"] = p.at("summary").asString();
                                if (p.contains("downloadCount") && p.at("downloadCount").isNumber()) o["downloads"] = (long long)p.at("downloadCount").asNumber();
                                if (p.contains("dateCreated") && p.at("dateCreated").isString()) o["published"] = p.at("dateCreated").asString();
                                if (p.contains("dateModified") && p.at("dateModified").isString()) o["updated"] = p.at("dateModified").asString();
                                if (p.contains("authors") && p.at("authors").isArray()) {
                                    for (const auto& a : p.at("authors").asArray()) {
                                        if (a.isObject() && a.contains("name") && a.at("name").isString()) { o["author"] = a.at("name").asString(); break; }
                                    }
                                }
                                if (p.contains("logo") && p.at("logo").isObject()) {
                                    const auto& lg = p.at("logo");
                                    if (lg.contains("thumbnailUrl") && lg.at("thumbnailUrl").isString()) o["icon"] = lg.at("thumbnailUrl").asString();
                                    else if (lg.contains("url") && lg.at("url").isString()) o["icon"] = lg.at("url").asString();
                                }
                                // 最新文件（下载用）：优先 downloadUrl，缺失时按 forgecdn 规则拼接
                                if (p.contains("latestFiles") && p.at("latestFiles").isArray()) {
                                    const auto& lf = p.at("latestFiles").asArray();
                                    for (const auto& f : lf) {
                                        if (!f.isObject()) continue;
                                        long long fileId = 0;
                                        std::string fileName, downloadUrl;
                                        if (f.contains("id") && f.at("id").isNumber()) fileId = (long long)f.at("id").asNumber();
                                        if (f.contains("fileName") && f.at("fileName").isString()) fileName = f.at("fileName").asString();
                                        if (f.contains("downloadUrl") && f.at("downloadUrl").isString()) downloadUrl = f.at("downloadUrl").asString();
                                        if (fileName.empty()) continue;
                                        if (downloadUrl.empty() && fileId > 0)
                                            downloadUrl = "https://edge.forgecdn.net/files/" + std::to_string(fileId / 1000) + "/" + std::to_string(fileId % 1000) + "/" + fileName;
                                        if (downloadUrl.empty()) continue;
                                        Json lfo = Json::object();
                                        lfo["url"] = downloadUrl;
                                        lfo["filename"] = fileName;
                                        lfo["id"] = std::to_string(fileId);
                                        if (f.contains("fileLength") && f.at("fileLength").isNumber()) lfo["size"] = (long long)f.at("fileLength").asNumber();
                                        o["latestFile"] = lfo;
                                        // 附带该文件的游戏版本列表
                                        if (f.contains("gameVersions") && f.at("gameVersions").isArray()) {
                                            Json gv = Json::array();
                                            for (const auto& v : f.at("gameVersions").asArray())
                                                if (v.isString()) gv.asArray().push_back(v.asString());
                                            o["gameVersions"] = gv;
                                        }
                                        break;
                                    }
                                }
                                if (o.contains("id")) list.asArray().push_back(o);
                            }
                        }
                        // CurseForge 不返回总数，用本次条数估算；不足一页说明到尾
                        if (total < limit) { /* 已到尾 */ }
                    } catch (...) {}
                }
                Json result = Json::object();
                result["projectType"] = projectType;
                result["source"] = "curseforge";
                result["list"] = list;
                result["total"] = total;
                result["offset"] = offset;
                done(Ok(result));
            } catch (const std::exception& e) {
                done(Err(-32061, std::string("CurseForge 搜索异常：") + e.what()));
            } catch (...) {
                done(Err(-32061, "CurseForge 搜索未知异常"));
            }
        }).detach();
    });

    // ===================== mc.gameVersions：Modrinth 游戏版本标签（用于模组/整合包筛选） =====================
    bridge.RegisterAsync("mc.gameVersions", [](const Json& params, const std::function<void(HandlerResult)>& done) {
        std::thread([done]() {
            std::string url = "https://api.modrinth.com/v2/tag/game_version";
            std::string text = HttpFetchText(lxe::Utf8ToWide(url));
            Json list = Json::array();
            if (!text.empty()) {
                try {
                    Json data = Json::parse(text);
                    if (data.isArray()) {
                        for (const auto& v : data.asArray()) {
                            if (v.isObject()) {
                                std::string gv;
                                if (v.contains("version") && v.at("version").isString()) gv = v.at("version").asString();
                                else if (v.contains("game_version") && v.at("game_version").isString()) gv = v.at("game_version").asString();
                                if (gv.empty()) continue;
                                Json o = Json::object();
                                o["version"] = gv;
                                if (v.contains("version_type") && v.at("version_type").isString()) o["type"] = v.at("version_type").asString();
                                if (v.contains("date") && v.at("date").isString()) o["date"] = v.at("date").asString();
                                if (v.contains("major") && v.at("major").isBool()) o["major"] = v.at("major").asBool();
                                list.asArray().push_back(o);
                            }
                        }
                    }
                } catch (...) {}
            }
            Json result = Json::object();
            result["list"] = list;
            done(Ok(result));
        }).detach();
    });

    // ===================== mc.modLatestFile：Modrinth 项目最新文件信息 =====================
    bridge.RegisterAsync("mc.modLatestFile", [](const Json& params, const std::function<void(HandlerResult)>& done) {
        std::string projectId;
        if (params.isObject() && params.contains("projectId") && params.at("projectId").isString())
            projectId = params.at("projectId").asString();
        if (projectId.empty()) { done(Err(-32602, "missing projectId")); return; }

        std::thread([projectId, done]() {
            // 项目版本列表（按发布日期新→旧）
            std::string url = "https://api.modrinth.com/v2/project/" + UrlEncode(projectId) + "/version";
            std::string text = HttpFetchText(lxe::Utf8ToWide(url));
            Json result = Json::object();
            if (text.empty()) { done(Err(-32000, "modrinth version fetch failed")); return; }
            try {
                Json data = Json::parse(text);
                if (!data.isArray() || data.asArray().empty()) { done(Err(-32000, "mod has no versions")); return; }
                const Json& first = data.asArray().front();
                if (first.contains("version_number") && first.at("version_number").isString())
                    result["versionNumber"] = first.at("version_number").asString();
                Json files = Json::array();
                if (first.contains("files") && first.at("files").isArray()) {
                    for (const auto& f : first.at("files").asArray()) {
                        if (!f.isObject()) continue;
                        Json fo = Json::object();
                        if (f.contains("url") && f.at("url").isString()) fo["url"] = f.at("url").asString();
                        if (f.contains("filename") && f.at("filename").isString()) fo["filename"] = f.at("filename").asString();
                        if (f.contains("size") && f.at("size").isNumber()) fo["size"] = (long long)f.at("size").asNumber();
                        if (f.contains("sha1") && f.at("sha1").isString()) fo["sha1"] = f.at("sha1").asString();
                        if (f.contains("primary") && f.at("primary").isBool()) fo["primary"] = f.at("primary").asBool();
                        files.asArray().push_back(fo);
                    }
                }
                result["files"] = files;
                if (files.asArray().empty()) { done(Err(-32000, "mod version has no files")); return; }
                result["file"] = files.asArray().front();
                if (first.contains("game_versions") && first.at("game_versions").isArray()) {
                    Json gv = Json::array();
                    for (const auto& v : first.at("game_versions").asArray())
                        if (v.isString()) gv.asArray().push_back(v.asString());
                    result["gameVersions"] = gv;
                }
                if (first.contains("loaders") && first.at("loaders").isArray()) {
                    Json lds = Json::array();
                    for (const auto& l : first.at("loaders").asArray())
                        if (l.isString()) lds.asArray().push_back(l.asString());
                    result["loaders"] = lds;
                }
                done(Ok(result));
            } catch (const std::exception& e) {
                done(Err(-32000, std::string("modrinth parse failed: ") + e.what()));
            }
        }).detach();
    });

    // ===================== mc.modDetail：Modrinth 项目详情（异步） =====================
    bridge.RegisterAsync("mc.modDetail", [](const Json& params, const std::function<void(HandlerResult)>& done) {
        std::string projectId;
        if (params.isObject() && params.contains("projectId") && params.at("projectId").isString())
            projectId = params.at("projectId").asString();
        if (projectId.empty()) { done(Err(-32602, "missing projectId")); return; }

        std::thread([projectId, done]() {
            std::string url = "https://api.modrinth.com/v2/project/" + UrlEncode(projectId);
            std::string text = HttpFetchText(lxe::Utf8ToWide(url));
            if (text.empty()) { done(Err(-32000, "modrinth project fetch failed")); return; }
            try {
                Json d = Json::parse(text);
                if (!d.isObject()) { done(Err(-32000, "modrinth project invalid")); return; }
                Json o = Json::object();
                if (d.contains("id") && d.at("id").isString()) o["id"] = d.at("id").asString();
                if (d.contains("slug") && d.at("slug").isString()) o["slug"] = d.at("slug").asString();
                if (d.contains("title") && d.at("title").isString()) o["title"] = d.at("title").asString();
                if (d.contains("description") && d.at("description").isString()) o["description"] = d.at("description").asString();
                if (d.contains("body") && d.at("body").isString()) o["body"] = d.at("body").asString();
                if (d.contains("icon_url") && d.at("icon_url").isString()) o["icon"] = d.at("icon_url").asString();
                if (d.contains("project_type") && d.at("project_type").isString()) o["projectType"] = d.at("project_type").asString();
                if (d.contains("downloads") && d.at("downloads").isNumber()) o["downloads"] = (long long)d.at("downloads").asNumber();
                if (d.contains("followers") && d.at("followers").isNumber()) o["follows"] = (long long)d.at("followers").asNumber();
                if (d.contains("published") && d.at("published").isString()) o["published"] = d.at("published").asString();
                else if (d.contains("date_created") && d.at("date_created").isString()) o["published"] = d.at("date_created").asString();
                if (d.contains("updated") && d.at("updated").isString()) o["updated"] = d.at("updated").asString();
                else if (d.contains("date_modified") && d.at("date_modified").isString()) o["updated"] = d.at("date_modified").asString();
                if (d.contains("categories") && d.at("categories").isArray()) {
                    Json cats = Json::array();
                    for (const auto& c : d.at("categories").asArray())
                        if (c.isString()) cats.asArray().push_back(c.asString());
                    o["categories"] = cats;
                }
                if (d.contains("game_versions") && d.at("game_versions").isArray()) {
                    Json gv = Json::array();
                    for (const auto& v : d.at("game_versions").asArray())
                        if (v.isString()) gv.asArray().push_back(v.asString());
                    o["gameVersions"] = gv;
                }
                if (d.contains("loaders") && d.at("loaders").isArray()) {
                    Json lds = Json::array();
                    for (const auto& l : d.at("loaders").asArray())
                        if (l.isString()) lds.asArray().push_back(l.asString());
                    o["loaders"] = lds;
                }
                if (d.contains("source_url") && d.at("source_url").isString()) o["sourceUrl"] = d.at("source_url").asString();
                if (d.contains("issues_url") && d.at("issues_url").isString()) o["issuesUrl"] = d.at("issues_url").asString();
                done(Ok(o));
            } catch (const std::exception& e) {
                done(Err(-32000, std::string("modrinth project parse failed: ") + e.what()));
            }
        }).detach();
    });

    // ===================== mc.modVersions：Modrinth 项目全部版本（异步） =====================
    bridge.RegisterAsync("mc.modVersions", [](const Json& params, const std::function<void(HandlerResult)>& done) {
        std::string projectId;
        if (params.isObject() && params.contains("projectId") && params.at("projectId").isString())
            projectId = params.at("projectId").asString();
        if (projectId.empty()) { done(Err(-32602, "missing projectId")); return; }

        std::thread([projectId, done]() {
            std::string url = "https://api.modrinth.com/v2/project/" + UrlEncode(projectId) + "/version";
            std::string text = HttpFetchText(lxe::Utf8ToWide(url));
            if (text.empty()) { done(Err(-32000, "modrinth version fetch failed")); return; }
            try {
                Json data = Json::parse(text);
                if (!data.isArray()) { done(Err(-32000, "modrinth versions invalid")); return; }
                Json list = Json::array();
                for (const auto& v : data.asArray()) {
                    if (!v.isObject()) continue;
                    Json o = Json::object();
                    if (v.contains("id") && v.at("id").isString()) o["id"] = v.at("id").asString();
                    if (v.contains("name") && v.at("name").isString()) o["name"] = v.at("name").asString();
                    if (v.contains("version_number") && v.at("version_number").isString()) o["versionNumber"] = v.at("version_number").asString();
                    if (v.contains("version_type") && v.at("version_type").isString()) o["versionType"] = v.at("version_type").asString();
                    if (v.contains("date_published") && v.at("date_published").isString()) o["datePublished"] = v.at("date_published").asString();
                    if (v.contains("downloads") && v.at("downloads").isNumber()) o["downloads"] = (long long)v.at("downloads").asNumber();
                    if (v.contains("game_versions") && v.at("game_versions").isArray()) {
                        Json gv = Json::array();
                        for (const auto& g : v.at("game_versions").asArray())
                            if (g.isString()) gv.asArray().push_back(g.asString());
                        o["gameVersions"] = gv;
                    }
                    if (v.contains("loaders") && v.at("loaders").isArray()) {
                        Json lds = Json::array();
                        for (const auto& l : v.at("loaders").asArray())
                            if (l.isString()) lds.asArray().push_back(l.asString());
                        o["loaders"] = lds;
                    }
                    // 依赖（前端用于下载前检查"必需前置"）
                    if (v.contains("dependencies") && v.at("dependencies").isArray()) {
                        Json deps = Json::array();
                        for (const auto& d : v.at("dependencies").asArray()) {
                            if (!d.isObject()) continue;
                            Json do_ = Json::object();
                            if (d.contains("project_id") && d.at("project_id").isString()) do_["projectId"] = d.at("project_id").asString();
                            if (d.contains("dependency_type") && d.at("dependency_type").isString()) do_["dependencyType"] = d.at("dependency_type").asString();
                            if (d.contains("version_id") && d.at("version_id").isString()) do_["versionId"] = d.at("version_id").asString();
                            if (d.contains("file_name") && d.at("file_name").isString()) do_["fileName"] = d.at("file_name").asString();
                            deps.asArray().push_back(do_);
                        }
                        o["dependencies"] = deps;
                    }
                    Json files = Json::array();
                    if (v.contains("files") && v.at("files").isArray()) {
                        for (const auto& f : v.at("files").asArray()) {
                            if (!f.isObject()) continue;
                            Json fo = Json::object();
                            if (f.contains("url") && f.at("url").isString()) fo["url"] = f.at("url").asString();
                            if (f.contains("filename") && f.at("filename").isString()) fo["filename"] = f.at("filename").asString();
                            if (f.contains("size") && f.at("size").isNumber()) fo["size"] = (long long)f.at("size").asNumber();
                            if (f.contains("sha1") && f.at("sha1").isString()) fo["sha1"] = f.at("sha1").asString();
                            if (f.contains("primary") && f.at("primary").isBool()) fo["primary"] = f.at("primary").asBool();
                            files.asArray().push_back(fo);
                        }
                    }
                    o["files"] = files;
                    // 主文件（下载用）
                    for (const auto& fo : files.asArray()) {
                        if (fo.isObject() && fo.contains("primary") && fo.at("primary").isBool() && fo.at("primary").asBool()) {
                            o["file"] = fo;
                            break;
                        }
                    }
                    if (!o.contains("file") && !files.asArray().empty()) o["file"] = files.asArray().front();
                    list.asArray().push_back(o);
                }
                Json result = Json::object();
                result["list"] = list;
                done(Ok(result));
            } catch (const std::exception& e) {
                done(Err(-32000, std::string("modrinth versions parse failed: ") + e.what()));
            }
        }).detach();
    });

    // ===================== mc.modDepsTree：模组必需前置链（异步） =====================
    // 返回给定版本的全部必需前置（project_id 链）。deep=true 时沿版本递归展开；输出按 id 去重
    bridge.RegisterAsync("mc.modDepsTree", [](const Json& params, const std::function<void(HandlerResult)>& done) {
        std::string projectId, versionId;
        bool deep = false;
        if (params.isObject() && params.contains("projectId") && params.at("projectId").isString())
            projectId = params.at("projectId").asString();
        if (params.isObject() && params.contains("versionId") && params.at("versionId").isString())
            versionId = params.at("versionId").asString();
        if (params.isObject() && params.contains("deep") && params.at("deep").isBool())
            deep = params.at("deep").asBool();
        if (projectId.empty()) { done(Err(-32602, "missing projectId")); return; }

        std::thread([projectId, versionId, deep, done]() {
            try {
            Json out = Json::array();
            std::vector<std::string> seen;
            auto push = [&](const Json& item) {
                std::string id = item.contains("id") && item.at("id").isString() ? item.at("id").asString() : "";
                if (!id.empty() && std::find(seen.begin(), seen.end(), id) != seen.end()) return;
                if (!id.empty()) seen.push_back(id);
                out.asArray().push_back(item);
            };
            std::function<void(const std::string&, const std::string&, int)> walk =
                [&](const std::string& pid, const std::string& vid, int depth) {
                    if (pid.empty() || depth > 6) return;
                    std::string vId = vid;
                    if (vId.empty()) {
                        std::string vlist = HttpFetchText(lxe::Utf8ToWide("https://api.modrinth.com/v2/project/" + UrlEncode(pid) + "/version"));
                        if (vlist.empty()) return;
                        try {
                            Json arr = Json::parse(vlist);
                            if (arr.isArray() && !arr.asArray().empty() && arr.asArray().front().isObject() &&
                                arr.asArray().front().contains("id") && arr.asArray().front().at("id").isString())
                                vId = arr.asArray().front().at("id").asString();
                        } catch (...) { return; }
                    }
                    if (vId.empty()) return;
                    std::vector<std::string> depIds;
                    std::vector<std::string> depVids;
                    std::string vtext = HttpFetchText(lxe::Utf8ToWide("https://api.modrinth.com/v2/version/" + UrlEncode(vId)));
                    if (!vtext.empty()) {
                        try {
                            Json vd = Json::parse(vtext);
                            if (vd.isObject() && vd.contains("dependencies") && vd.at("dependencies").isArray()) {
                                for (const auto& d : vd.at("dependencies").asArray()) {
                                    if (!d.isObject()) continue;
                                    if (!(d.contains("dependency_type") && d.at("dependency_type").isString() && d.at("dependency_type").asString() == "required")) continue;
                                    if (d.contains("project_id") && d.at("project_id").isString()) {
                                        std::string dp = d.at("project_id").asString();
                                        if (std::find(depIds.begin(), depIds.end(), dp) == depIds.end()) {
                                            depIds.push_back(dp);
                                            depVids.push_back(d.contains("version_id") && d.at("version_id").isString() ? d.at("version_id").asString() : "");
                                        }
                                    }
                                }
                            }
                        } catch (...) {}
                    }
                    std::map<std::string, Json> projMeta;
                    if (!depIds.empty()) {
                        std::string idsJson = "[";
                        for (size_t i = 0; i < depIds.size(); ++i) { if (i) idsJson += ","; idsJson += "\"" + depIds[i] + "\""; }
                        idsJson += "]";
                        std::string ptext = HttpFetchText(lxe::Utf8ToWide("https://api.modrinth.com/v2/projects?ids=" + UrlEncode(idsJson)));
                        if (!ptext.empty()) {
                            try {
                                Json pd = Json::parse(ptext);
                                if (pd.isArray()) {
                                    for (const auto& p : pd.asArray()) {
                                        if (!p.isObject()) continue;
                                        Json meta = Json::object();
                                        if (p.contains("id") && p.at("id").isString()) meta["id"] = p.at("id").asString();
                                        if (p.contains("slug") && p.at("slug").isString()) meta["slug"] = p.at("slug").asString();
                                        if (p.contains("title") && p.at("title").isString()) meta["title"] = p.at("title").asString();
                                        if (meta.contains("id")) projMeta[meta.at("id").asString()] = meta;
                                    }
                                }
                            } catch (...) {}
                        }
                    }
                    for (size_t i = 0; i < depIds.size(); ++i) {
                        Json item = Json::object();
                        auto mi = projMeta.find(depIds[i]);
                        if (mi != projMeta.end()) item = mi->second;
                        else item["id"] = depIds[i];
                        item["versionId"] = depVids[i];
                        push(item);
                        if (deep) walk(depIds[i], depVids[i], depth + 1);
                    }
                };
            walk(projectId, versionId, 0);
            Json result = Json::object();
            result["list"] = out;
            done(Ok(result));
            } catch (const std::exception& e) {
                done(Err(-32054, std::string("解析依赖树异常：") + e.what()));
            } catch (...) {
                done(Err(-32054, "解析依赖树未知异常"));
            }
        }).detach();
    });

    // ===================== mc.installModpack：整合包自动导入（异步） =====================
    // 下载 .mrpack（或本地拖入的 .mrpack/.zip，localPath 分支）→ tar 解压 → 解析
    // modrinth.index.json 等 → 安装对应 loader 版本(installName=整合包名) → 按 files[] 下载
    // mods/config 等到 mcRoot → 应用 overrides。全程 download.progress/state 事件。
    bridge.Register("mc.installModpack", [&bridge](const Json& params) {
        std::string name, url, localPath;
        if (params.isObject() && params.contains("name") && params.at("name").isString())
            name = params.at("name").asString();
        if (params.isObject() && params.contains("url") && params.at("url").isString())
            url = params.at("url").asString();
        if (params.isObject() && params.contains("localPath") && params.at("localPath").isString())
            localPath = params.at("localPath").asString();
        // name 必填；url（远程下载）与 localPath（本地拖入）二选一
        if (name.empty() || (url.empty() && localPath.empty()))
            return Err(-32602, "missing name/url or localPath");
        if (!localPath.empty()) {
            std::error_code lec;
            if (!std::filesystem::is_regular_file(lxe::Utf8ToWide(localPath), lec))
                return Err(-32002, "本地整合包文件不存在");
        }

        // 版本隔离（§29 四需求 Req1）：true 时 mods/config/overrides 铺到 versions/<safeName> 下，
        // 与启动时 versionIsolation 的 gameDir=versions/<version> 对齐；否则铺到全局 mcRoot。
        bool versionIsolation = false;
        if (params.isObject() && params.contains("versionIsolation") && params.at("versionIsolation").isBool())
            versionIsolation = params.at("versionIsolation").asBool();

        static std::atomic<int> packTaskSeq{30000};
        int taskId = ++packTaskSeq;
        int packThreads = 4;
        if (params.contains("threads") && params.at("threads").isNumber()) {
            packThreads = (int)params.at("threads").asNumber();
            if (packThreads < 1) packThreads = 1;
            if (packThreads > 16) packThreads = 16;
        }
        Json result = Json::object();
        result["taskId"] = std::to_string(taskId);
        result["started"] = true;
        result["name"] = name;

        std::thread([&bridge, taskId, name, url, localPath, packThreads, params, versionIsolation]() {
            auto progressMutex = std::make_shared<std::mutex>();
            auto filesState = std::make_shared<std::vector<Json>>();
            auto completedFiles = std::make_shared<std::atomic<int>>(0);
            auto postProgress = [&](int pct, const std::string& stage, const std::string& speed = "", const std::string& eta = "", const std::string& size = "", int doneFiles = -1) {
                Json prog = Json::object();
                prog["taskId"] = std::to_string(taskId);
                prog["percent"] = pct;
                prog["stage"] = stage;
                prog["name"] = "整合包 " + name;
                prog["speed"] = speed;
                prog["eta"] = eta;
                prog["size"] = size;
                int df = doneFiles < 0 ? (int)completedFiles->load() : doneFiles;
                prog["totalFiles"] = (int)filesState->size();
                prog["doneFiles"] = df;
                prog["remainingFiles"] = (int)filesState->size() - df;
                {
                    std::lock_guard<std::mutex> lock(*progressMutex);
                    Json fArr = Json::array();
                    for (const auto& fs : *filesState) fArr.asArray().push_back(fs);
                    prog["files"] = fArr;
                }
                bridge.PostEvent("download.progress", prog);
            };
            auto postState = [&](const std::string& state) {
                Json ev = Json::object();
                ev["taskId"] = std::to_string(taskId);
                ev["state"] = state;
                ev["name"] = "整合包 " + name;
                bridge.PostEvent("download.state", ev);
            };
            auto fail = [&](const std::string& stage) {
                postProgress(0, "错误：" + stage);
                postState("error");
            };

            try {
            postState("started");
            std::wstring mcRoot = GetMcRoot();
            std::wstring tmpDir = mcRoot + L"\\temp\\pack_import";
            // 版本名/文件夹名：统一用清洗后的安全名（InstallLoaderImpl 的 installName 用同一值）
            std::wstring safeNameW = lxe::Utf8ToWide(name);
            for (auto& c : safeNameW) if (c == L'\\' || c == L'/' || c == L':' || c == L'*' || c == L'?' || c == L'"' || c == L'<' || c == L'>' || c == L'|') c = L'_';
            std::string safeName = lxe::WideToUtf8(safeNameW);
            // 版本隔离：下载/覆盖目标根目录 = versions/<safeName>（与启动 gameDir 对齐）；否则全局 mcRoot
            std::wstring targetRoot = mcRoot;
            if (versionIsolation) {
                std::error_code tec;
                std::filesystem::create_directories(mcRoot + L"\\versions\\" + safeNameW, tec);
                targetRoot = mcRoot + L"\\versions\\" + safeNameW;
            }
            // §2.1 重复安装检测：若目标版本（versions/<safeName>）目录已存在，且前端未确认合并，则直接失败提示用户选择
            bool merge = false;
            if (params.isObject() && params.contains("merge") && params.at("merge").isBool())
                merge = params.at("merge").asBool();
            {
                std::error_code mec;
                std::wstring verDir = mcRoot + L"\\versions\\" + safeNameW;
                // 仅当 versions/<safeName>/<safeName>.json 已存在（该版本真实已安装）才要求合并确认；
                // 若只是残留目录（上次导入失败留下的空壳，无版本 JSON），版本列表并不显示它，
                // 这里也不视为“已存在”，直接继续覆盖安装，避免误报“目标版本已存在”。
                std::wstring verJson = verDir + L"\\" + safeNameW + L".json";
                if (!merge && std::filesystem::exists(verJson, mec)) {
                    // 后端兜底：拒绝自动覆盖；前端应改走“合并/取消”确认（§2.1）
                    postProgress(0, "目标版本已存在：请在界面上选择“合并”或“取消”");
                    postState("error");
                    Json errEv = Json::object();
                    errEv["taskId"] = std::to_string(taskId);
                    errEv["state"] = "error";
                    errEv["code"] = "version_exists";
                    errEv["name"] = "整合包 " + name;
                    bridge.PostEvent("download.state", errEv);
                    return;
                }
            }
            std::wstring archive = tmpDir + L"\\" + safeNameW + L".pack.zip";
            std::wstring extractDir = tmpDir + L"\\" + safeNameW + L"_extract";
            std::error_code ec;
            std::filesystem::remove_all(extractDir, ec);
            std::filesystem::create_directories(tmpDir, ec);

            // 阶段 1：获取整合包文件（localPath 为本地拖入文件 → 直接复制；否则统一走多线程下载器）
            postProgress(2, localPath.empty() ? "下载整合包文件" : "读取整合包文件");
            {
                if (!localPath.empty()) {
                    std::error_code lec;
                    std::filesystem::copy_file(lxe::Utf8ToWide(localPath), archive,
                                               std::filesystem::copy_options::overwrite_existing, lec);
                    if (lec) { fail("整合包文件读取失败"); return; }
                } else {
                    auto cb = [&](const Aria2Progress& p) -> bool {
                        int mapped = 2 + p.percent * 18 / 100;
                        std::ostringstream ss; ss << p.speed;
                        postProgress(mapped, "下载整合包文件", ss.str(), p.eta);
                        return true;
                    };
                    if (!DownloadFileSmart(lxe::Utf8ToWide(url), tmpDir, safeNameW + L".pack.zip", cb)) {
                        fail("整合包下载失败");
                        return;
                    }
                }
            }

            // .rar 检测（§2.1）：读取文件头 "Rar!" 即报错提示转 .zip
            {
                std::ifstream hdr(archive, std::ios::binary);
                char buf[4] = { 0, 0, 0, 0 };
                if (hdr.is_open()) { hdr.read(buf, 4); hdr.close(); }
                if (buf[0] == 'R' && buf[1] == 'a' && buf[2] == 'r' && buf[3] == '!') {
                    fail("检测到 RAR 压缩包：请先将整合包转换为 ZIP 格式后重试");
                    return;
                }
            }

            // 阶段 2：解压（tar 兼容 zip）
            postProgress(22, "解压整合包");
            {
                std::wstring cmd = L"cmd /c tar -xf \"" + archive + L"\" -C \"" + extractDir + L"\"";
                if (RunProcessSilent(cmd, tmpDir) != 0) {
                    std::filesystem::create_directories(extractDir, ec);
                    if (RunProcessSilent(cmd, tmpDir) != 0) { fail("整合包解压失败"); return; }
                }
            }

            // ============ 格式检测（§2.1：不依赖扩展名，按关键文件/目录定位，支持根目录与一层子目录） ============
            postProgress(26, "解析整合包信息");
            // 定位含关键文件的基准目录：先看根目录，再看一级子目录
            auto existsAt = [&](const std::wstring& base, const std::string& rel) {
                std::error_code e;
                return std::filesystem::exists(base + L"\\" + lxe::Utf8ToWide(rel), e);
            };
            auto findBase = [&](const std::vector<std::string>& keys, std::wstring& out) -> bool {
                if (existsAt(extractDir, keys.front())) { out = extractDir; return true; }
                std::error_code e;
                if (std::filesystem::is_directory(extractDir, e)) {
                    for (const auto& de : std::filesystem::directory_iterator(extractDir, e)) {
                        if (!de.is_directory(e)) continue;
                        std::wstring sub = de.path().wstring();
                        if (existsAt(sub, keys.front())) { out = sub; return true; }
                    }
                }
                return false;
            };
            std::wstring baseDir;
            std::string packKind; // modrinth | curseforge | mcbbs | multimc | hmcl | plain | launcher
            if (existsAt(extractDir, "modrinth.index.json")) { baseDir = extractDir; packKind = "modrinth"; }
            else if (existsAt(extractDir, "mcbbs.packmeta")) { baseDir = extractDir; packKind = "mcbbs"; }
            else if (existsAt(extractDir, "mmc-pack.json")) { baseDir = extractDir; packKind = "multimc"; }
            else if (existsAt(extractDir, "modpack.json")) { baseDir = extractDir; packKind = "hmcl"; }
            else if (findBase({ "manifest.json" }, baseDir)) {
                // manifest.json：有 addons → MCBBS（§2.6），否则 CurseForge（§2.2）
                auto [mt, mok] = ReadFileUtf8(baseDir + L"\\manifest.json");
                if (mok && mt.find("\"addons\"") != std::string::npos) packKind = "mcbbs";
                else packKind = "curseforge";
            } else if (existsAt(extractDir, "versions")) {
                // 普通压缩包：正则匹配 versions/<名称>/<名称>.json（§2.8）
                bool found = false;
                std::error_code e;
                for (const auto& de : std::filesystem::recursive_directory_iterator(extractDir, e)) {
                    if (!found && de.is_regular_file(e) && de.path().extension() == L".json") {
                        auto p = de.path().wstring();
                        size_t vs = p.find(L"versions");
                        if (vs != std::wstring::npos && p.find(L"\\") != std::wstring::npos) {
                            size_t after = p.find(L"\\", vs);
                            size_t slash2 = after == std::wstring::npos ? std::wstring::npos : p.find(L"\\", after + 1);
                            size_t endSlash = slash2 == std::wstring::npos ? std::wstring::npos : p.find(L"\\", slash2 + 1);
                            if (endSlash == std::wstring::npos && slash2 != std::wstring::npos) {
                                std::wstring vname = p.substr(after + 1, slash2 - after - 1);
                                if (!vname.empty() && p.size() >= slash2 + 1 + vname.size() &&
                                    p.compare(slash2 + 1, vname.size(), vname) == 0 &&
                                    p.substr(slash2 + 1 + vname.size()) == L".json") {
                                    found = true; break;
                                }
                            }
                        }
                    }
                }
                baseDir = extractDir; packKind = found ? "plain" : "";
            }
            // 带启动器的压缩包（§2.7）：包内存在 .exe / .bat 可执行文件
            if (packKind.empty()) {
                bool exeFound = false;
                std::error_code e;
                for (const auto& de : std::filesystem::recursive_directory_iterator(extractDir, e)) {
                    std::wstring ext = de.path().extension().wstring();
                    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
                    if (ext == L".exe" || ext == L".bat") { exeFound = true; break; }
                }
                if (exeFound) { baseDir = extractDir; packKind = "launcher"; }
            }
            if (packKind.empty()) {
                fail("无法识别的整合包格式：未找到 manifest.json / modrinth.index.json / mmc-pack.json / modpack.json / mcbbs.packmeta / versions/<名称>/<名称>.json");
                return;
            }

            // ============ 解析出 目标 MC 版本 + 加载器（各格式统一） ============
            std::string mcVersion;
            Json filesArr = Json::array();       // [{path,url}] 远程文件列表（Modrinth/CurseForge）
            bool copyWholeDir = false;           // true：把压缩包内容作为 assets/mods/... 直接铺到 mcRoot
            std::wstring contentRoot;            // copyWholeDir 时复制的源目录
            std::wstring loaderVersionNeeded;    // 需额外解析的加载器（见各格式分支）
            bool hasLoader = false; Json loaderInfo = Json::object();
            std::string loaderId, loaderVersion;
            std::error_code ecode;
            // MultiMC（§2.4）：instance.cfg 解析出的实例配置（键名转冒号形式），供 $INST_* 占位符替换
            std::map<std::wstring, std::wstring> instPlaceholders;
            std::wstring instJava;               // $INST_JAVA：Java 可执行文件路径
            std::string instName;                // $INST_NAME：实例名称（整合包名）

            if (packKind == "modrinth") {
                // —— Modrinth（§2.3）：modrinth.index.json ——
                if (existsAt(extractDir, "modrinth.index.json") == false) { fail("整合包缺少 modrinth.index.json"); return; }
                Json idx = Json();
                { auto [t, ok] = ReadFileUtf8(extractDir + L"\\modrinth.index.json");
                  if (!ok || t.empty()) { fail("整合包缺少 modrinth.index.json"); return; }
                  try { idx = Json::parse(t); } catch (...) { fail("modrinth.index.json 解析失败"); return; } }
                if (!idx.isObject()) { fail("modrinth.index.json 无效"); return; }
                if (idx.contains("dependencies") && idx.at("dependencies").isObject() &&
                    idx.at("dependencies").contains("minecraft") && idx.at("dependencies").at("minecraft").isString())
                    mcVersion = idx.at("dependencies").at("minecraft").asString();
                if (mcVersion.empty()) { fail("整合包未声明 Minecraft 版本"); return; }
                if (idx.contains("files") && idx.at("files").isArray()) filesArr = idx.at("files");
                // 识别加载器（fabric-loader/forge/neoforge/quilt-loader）
                static const std::map<std::string, std::string> modrinthLoaderKeys = {
                    { "fabric-loader", "fabric" }, { "forge", "forge" }, { "neoforge", "neoforge" }, { "quilt-loader", "quilt" } };
                if (idx.contains("dependencies") && idx.at("dependencies").isObject()) {
                    const auto& deps = idx.at("dependencies");
                    for (const auto& kv : modrinthLoaderKeys) {
                        if (deps.contains(kv.first) && deps.at(kv.first).isString()) {
                            loaderId = kv.second; loaderVersion = deps.at(kv.first).asString(); hasLoader = true; break;
                        }
                    }
                }
                copyWholeDir = false;
                contentRoot = extractDir + L"\\overrides";
            } else if (packKind == "curseforge") {
                // —— CurseForge（§2.2）：manifest.json，无 addons ——
                auto [mt, mok] = ReadFileUtf8(baseDir + L"\\manifest.json");
                if (!mok || mt.empty()) { fail("manifest.json 读取失败"); return; }
                Json mj = Json();
                try { mj = Json::parse(mt); } catch (...) { fail("manifest.json 解析失败"); return; }
                if (mj.contains("minecraft") && mj.at("minecraft").isObject() &&
                    mj.at("minecraft").contains("version") && mj.at("minecraft").at("version").isString())
                    mcVersion = mj.at("minecraft").at("version").asString();
                if (mcVersion.empty()) { fail("未找到 CurseForge 整合包的 MC 版本"); return; }
                // modLoaders 数组：前缀 forge- / neoforge- / fabric-
                if (mj.contains("minecraft") && mj.at("minecraft").isObject() &&
                    mj.at("minecraft").contains("modLoaders") && mj.at("minecraft").at("modLoaders").isArray()) {
                    for (const auto& ml : mj.at("minecraft").at("modLoaders").asArray()) {
                        if (!ml.isObject() || !ml.contains("id") || !ml.at("id").isString()) continue;
                        std::string id = ml.at("id").asString();
                        if (id.rfind("forge-", 0) == 0) { loaderId = "forge"; loaderVersion = id.substr(6); hasLoader = true; break; }
                        if (id.rfind("neoforge-", 0) == 0) { loaderId = "neoforge"; loaderVersion = id.substr(9); hasLoader = true; break; }
                        if (id.rfind("fabric-", 0) == 0) { loaderId = "fabric"; loaderVersion = id.substr(7); hasLoader = true; break; }
                    }
                }
                // files[]：fileId 批量查询真实下载链接（§2.2）
                if (mj.contains("files") && mj.at("files").isArray()) {
                    for (const auto& fentry : mj.at("files").asArray()) {
                        if (!fentry.isObject()) continue;
                        long long projectId = 0, fileId = 0;
                        if (fentry.contains("projectID") && fentry.at("projectID").isNumber()) projectId = (long long)fentry.at("projectID").asNumber();
                        if (fentry.contains("fileID") && fentry.at("fileID").isNumber()) fileId = (long long)fentry.at("fileID").asNumber();
                        if (projectId <= 0 || fileId <= 0) continue;
                        // 请求 CurseForge 文件详情（legacy 公开端点）取真实下载 URL
                        std::string apiUrl = "https://api.curseforge.com/v1/mods/" + std::to_string(projectId) +
                                             "/files/" + std::to_string(fileId);
                        std::string body = HttpFetchText(lxe::Utf8ToWide(apiUrl));
                        if (body.empty()) {
                            // 兜底：addons 公开镜像端点
                            body = HttpFetchText(lxe::Utf8ToWide(
                                "https://addons-ecs.forgesvc.net/api/v2/addon/" + std::to_string(projectId) +
                                "/file/" + std::to_string(fileId)));
                        }
                        if (body.empty()) continue;
                        try {
                            Json fj = Json::parse(body);
                            std::string dlUrl, fileName, relPath;
                            if (fj.isObject()) {
                                if (fj.contains("data") && fj.at("data").isObject()) fj = fj.at("data");
                                if (fj.contains("downloadUrl") && fj.at("downloadUrl").isString()) dlUrl = fj.at("downloadUrl").asString();
                                if (fj.contains("fileName") && fj.at("fileName").isString()) fileName = fj.at("fileName").asString();
                            }
                            if (dlUrl.empty()) continue;
                            Json fo = Json::object();
                            std::string path;
                            // 分类：检测 mcmod.info / fabric.mod.json → mods；pack.mcmeta → resourcepacks；其余 → 根目录（§2.2 文件分类规则由前端按内容判定）
                            if (fileName.rfind(".zip", fileName.size() - 4) == fileName.size() - 4) path = "resourcepacks/" + fileName;
                            else if (fileName.rfind(".jar", fileName.size() - 4) == fileName.size() - 4) path = "mods/" + fileName;
                            else path = fileName;
                            fo["path"] = path;
                            Json ds = Json::array(); ds.asArray().push_back(dlUrl);
                            fo["downloads"] = ds;
                            filesArr.asArray().push_back(fo);
                        } catch (...) {}
                    }
                }
                copyWholeDir = false;
                contentRoot = baseDir + L"\\overrides";
            } else if (packKind == "mcbbs") {
                // —— MCBBS（§2.6）：mcbbs.packmeta 或 manifest.json（含 addons） ——
                if (!existsAt(baseDir, "mcbbs.packmeta") && existsAt(baseDir, "manifest.json")) {
                    // manifest 含 addons → 判为 MCBBS；addons 提供 game/forge/neoforge/fabric/optifine
                    auto [mt, mok] = ReadFileUtf8(baseDir + L"\\manifest.json");
                    if (mok && mt.find("\"addons\"") != std::string::npos) {
                        Json mj2 = Json();
                        try { mj2 = Json::parse(mt); } catch (...) {}
                        if (mj2.isObject() && mj2.contains("addons") && mj2.at("addons").isObject()) {
                            const auto& addons = mj2.at("addons");
                            if (addons.contains("game") && addons.at("game").isString()) mcVersion = addons.at("game").asString();
                            if (addons.contains("forge") && addons.at("forge").isString()) { loaderId = "forge"; loaderVersion = addons.at("forge").asString(); hasLoader = true; }
                            else if (addons.contains("neoforge") && addons.at("neoforge").isString()) { loaderId = "neoforge"; loaderVersion = addons.at("neoforge").asString(); hasLoader = true; }
                            else if (addons.contains("fabric") && addons.at("fabric").isString()) { loaderId = "fabric"; loaderVersion = addons.at("fabric").asString(); hasLoader = true; }
                            else if (addons.contains("optifine") && addons.at("optifine").isString()) { loaderId = "optifine"; loaderVersion = addons.at("optifine").asString(); hasLoader = true; }
                            // Quilt 标记为不支持（§2.6）
                            if (addons.contains("quilt")) { fail("该整合包使用 Quilt 加载器，暂不支持"); return; }
                        }
                        if (mcVersion.empty()) { fail("MCBBS 整合包未声明游戏版本"); return; }
                        copyWholeDir = true;
                        contentRoot = baseDir;
                    } else {
                        fail("manifest.json 无法判定整合包类型");
                        return;
                    }
                } else {
                    auto [pt, pok] = ReadFileUtf8(baseDir + L"\\mcbbs.packmeta");
                    if (!pok || pt.empty()) { fail("mcbbs.packmeta 读取失败"); return; }
                    Json mj3 = Json();
                    try { mj3 = Json::parse(pt); } catch (...) { fail("mcbbs.packmeta 解析失败"); return; }
                    if (mj3.contains("addons") && mj3.at("addons").isObject()) {
                        const auto& addons = mj3.at("addons");
                        if (addons.contains("game") && addons.at("game").isString()) mcVersion = addons.at("game").asString();
                        if (addons.contains("forge") && addons.at("forge").isString()) { loaderId = "forge"; loaderVersion = addons.at("forge").asString(); hasLoader = true; }
                        else if (addons.contains("neoforge") && addons.at("neoforge").isString()) { loaderId = "neoforge"; loaderVersion = addons.at("neoforge").asString(); hasLoader = true; }
                        else if (addons.contains("fabric") && addons.at("fabric").isString()) { loaderId = "fabric"; loaderVersion = addons.at("fabric").asString(); hasLoader = true; }
                        else if (addons.contains("optifine") && addons.at("optifine").isString()) { loaderId = "optifine"; loaderVersion = addons.at("optifine").asString(); hasLoader = true; }
                        if (addons.contains("quilt")) { fail("该整合包使用 Quilt 加载器，暂不支持"); return; }
                    }
                    if (mcVersion.empty()) { fail("MCBBS 整合包未声明游戏版本"); return; }
                    copyWholeDir = true;
                    contentRoot = baseDir;
                }
            } else if (packKind == "multimc") {
                // —— MultiMC（§2.4）：mmc-pack.json components[] + instance.cfg ——
                auto [mt, mok] = ReadFileUtf8(baseDir + L"\\mmc-pack.json");
                if (!mok || mt.empty()) { fail("mmc-pack.json 读取失败"); return; }
                Json pk = Json();
                try { pk = Json::parse(mt); } catch (...) { fail("mmc-pack.json 解析失败"); return; }
                if (pk.contains("components") && pk.at("components").isArray()) {
                    for (const auto& comp : pk.at("components").asArray()) {
                        if (!comp.isObject() || !comp.contains("uid") || !comp.at("uid").isString()) continue;
                        std::string uid = comp.at("uid").asString();
                        if (uid == "net.minecraft" && comp.contains("version") && comp.at("version").isString()) mcVersion = comp.at("version").asString();
                        else if (uid == "forge" && comp.contains("version") && comp.at("version").isString()) { loaderId = "forge"; loaderVersion = comp.at("version").asString(); hasLoader = true; }
                        else if (uid == "neoforge" && comp.contains("version") && comp.at("version").isString()) { loaderId = "neoforge"; loaderVersion = comp.at("version").asString(); hasLoader = true; }
                        else if (uid == "fabric-loader" && comp.contains("version") && comp.at("version").isString()) { loaderId = "fabric"; loaderVersion = comp.at("version").asString(); hasLoader = true; }
                        else if (uid == "liteloader" && comp.contains("version") && comp.at("version").isString()) { loaderId = "liteloader"; loaderVersion = comp.at("version").asString(); hasLoader = true; }
                        // LWJGL 组件直接忽略（§2.4）
                    }
                }
                if (mcVersion.empty()) { fail("MultiMC 整合包未声明 MC 版本"); return; }
                // instance.cfg（§2.4）：键值对 "=" → ":"，解析实例配置构建占位符映射表
                instName = safeName;
                {
                    static const std::wstring kInstJavaKey = L"JavaPath"; // MultiMC 实例 Java 配置项键名
                    auto [it, iok] = ReadFileUtf8(baseDir + L"\\instance.cfg");
                    if (iok && !it.empty()) {
                        std::wstring wline;
                        std::wstringstream wss(lxe::Utf8ToWide(it));
                        auto trimWs = [](std::wstring s) {
                            size_t b = s.find_first_not_of(L" \t\r");
                            if (b == std::wstring::npos) return std::wstring();
                            size_t e = s.find_last_not_of(L" \t\r");
                            return s.substr(b, e - b + 1);
                        };
                        while (std::getline(wss, wline)) {
                             // MultiMC 的 instance.cfg：注释行为 #，小节标题为 [Section]（均跳过）
                             std::wstring t = trimWs(wline);
                             if (t.empty() || t[0] == L'#' || t[0] == L'[') continue;
                             size_t eq = t.find(L'=');
                             if (eq == std::wstring::npos) continue;
                            std::wstring key = trimWs(t.substr(0, eq));
                            std::wstring val = trimWs(t.substr(eq + 1));
                            if (key.empty()) continue;
                            // 键值对格式由 "=" 分隔统一转为冒号形式（§2.4）
                            std::wstring normKey = key;
                            std::transform(normKey.begin(), normKey.end(), normKey.begin(),
                                           [](wchar_t c) { return c == L'=' ? L':' : c; });
                            instPlaceholders[normKey] = val;
                            if (key == kInstJavaKey) instJava = val;
                        }
                    }
                    // 手工叠加核心占位符（§2.4：$INST_JAVA → Java 路径、$INST_MC_DIR → 游戏目录、$INST_NAME → 实例名）
                    instPlaceholders[L"INST_JAVA"] = instJava.empty() ? PickJavaForMajor(ScanInstalledJavas(), RecommendedJavaMajor(mcVersion)) : instJava;
                    instPlaceholders[L"INST_MC_DIR"] = mcRoot;
                    instPlaceholders[L"INST_NAME"] = lxe::Utf8ToWide(instName);
                }
                copyWholeDir = false; // MultiMC 的 .minecraft 目录内容铺到 mcRoot/.minecraft
                contentRoot = baseDir + L"\\.minecraft";
                if (!std::filesystem::is_directory(contentRoot, ecode)) contentRoot = baseDir;
            } else if (packKind == "hmcl") {
                // —— HMCL（§2.5）：modpack.json，覆写至实例目录下的 minecraft 子目录 ——
                auto [mt, mok] = ReadFileUtf8(baseDir + L"\\modpack.json");
                if (!mok || mt.empty()) { fail("modpack.json 读取失败"); return; }
                Json hj = Json();
                try { hj = Json::parse(mt); } catch (...) { fail("modpack.json 解析失败"); return; }
                if (hj.contains("launcherConfig") && hj.at("launcherConfig").isObject() &&
                    hj.at("launcherConfig").contains("minecraftVersion") && hj.at("launcherConfig").at("minecraftVersion").isString())
                    mcVersion = hj.at("launcherConfig").at("minecraftVersion").asString();
                if (mcVersion.empty() && hj.contains("minecraftVersion") && hj.at("minecraftVersion").isString())
                    mcVersion = hj.at("minecraftVersion").asString();
                if (mcVersion.empty()) { fail("HMCL 整合包未声明 MC 版本"); return; }
                copyWholeDir = true;
                contentRoot = baseDir + L"\\minecraft";
                if (!std::filesystem::is_directory(contentRoot, ecode)) contentRoot = baseDir;
            } else if (packKind == "plain") {
                // —— 普通压缩包（§2.8）：由 versions/<名称>/<名称>.json 反推 .minecraft 根目录，整体解压铺开 ——
                std::wstring foundRoot;
                std::error_code e;
                for (const auto& de : std::filesystem::recursive_directory_iterator(extractDir, e)) {
                    if (de.is_regular_file(e) && de.path().extension() == L".json") {
                        auto p = de.path().wstring();
                        size_t vs = p.find(L"versions");
                        if (vs == std::wstring::npos) continue;
                        size_t after = p.find(L"\\", vs);
                        if (after == std::wstring::npos) continue;
                        size_t slash2 = p.find(L"\\", after + 1);
                        if (slash2 == std::wstring::npos) continue;
                        size_t endSlash = p.find(L"\\", slash2 + 1);
                        if (endSlash != std::wstring::npos) continue;
                        std::wstring vname = p.substr(after + 1, slash2 - after - 1);
                        if (p.compare(slash2 + 1, vname.size(), vname) == 0 && p.substr(slash2 + 1 + vname.size()) == L".json") {
                            // versions 目录的上一级即为 .minecraft 根（versions/<名称>/<名称>.json）
                            foundRoot = p.substr(0, vs);
                            break;
                        }
                    }
                }
                contentRoot = foundRoot;
                copyWholeDir = true;
                // 从版本 json 反推 MC 版本
                for (const auto& de : std::filesystem::recursive_directory_iterator(extractDir, ecode)) {
                    if (de.is_regular_file(ecode) && de.path().extension() == L".json") {
                        auto [vt, vok] = ReadFileUtf8(de.path().wstring());
                        if (vok && !vt.empty()) {
                            try {
                                Json vj = Json::parse(vt);
                                std::string parentMC;
                                if (vj.contains("inheritsFrom") && vj.at("inheritsFrom").isString()) parentMC = vj.at("inheritsFrom").asString();
                                if (!parentMC.empty()) { mcVersion = parentMC; break; }
                            } catch (...) {}
                        }
                    }
                }
                if (mcVersion.empty()) { fail("普通整合包无法反推 MC 版本"); return; }
                if (contentRoot.empty()) { fail("普通整合包未找到 .minecraft 根目录"); return; }
            } else if (packKind == "launcher") {
                // —— 带启动器的压缩包（§2.7）：需用户确认改用本启动器管理 ——
                // 前端在调用前已询问；若仍带 force 标记则按 .minecraft 根目录注册，否则中止
                bool forceLauncher = false;
                if (params.isObject() && params.contains("forceLauncher") && params.at("forceLauncher").isBool())
                    forceLauncher = params.at("forceLauncher").asBool();
                if (!forceLauncher) {
                    fail("检测到带独立启动器的整合包：是否改为由本启动器管理？（如需继续请以确认模式重试）");
                    return;
                }
                std::wstring contentRootC = extractDir + L"\\.minecraft";
                if (!std::filesystem::is_directory(contentRootC, ecode)) contentRootC = extractDir;
                contentRoot = contentRootC;
                copyWholeDir = true;
                // 反推 MC 版本：读取 versions/ 下任意版本 json
                for (const auto& de : std::filesystem::recursive_directory_iterator(contentRoot, ecode)) {
                    if (de.is_regular_file(ecode) && de.path().extension() == L".json" &&
                        de.path().wstring().find(L"versions") != std::wstring::npos) {
                        auto [vt, vok] = ReadFileUtf8(de.path().wstring());
                        if (vok && !vt.empty()) {
                            try {
                                Json vj = Json::parse(vt);
                                if (vj.contains("inheritsFrom") && vj.at("inheritsFrom").isString()) { mcVersion = vj.at("inheritsFrom").asString(); break; }
                            } catch (...) {}
                        }
                    }
                }
                if (mcVersion.empty()) { fail("带启动器的整合包无法反推 MC 版本"); return; }
            }

            if (mcVersion.empty() && !packKind.empty()) { fail("整合包未声明 Minecraft 版本"); return; }

            // ============ 阶段 4：安装 loader（复用安装核心，installName=整合包名） ============
            if (!hasLoader) {
                // 无加载器：直接走原版安装
                postProgress(30, "安装原版 " + mcVersion);
                std::wstring verDir = mcRoot + L"\\versions\\" + safeNameW;
                Json lp = Json::object();
                lp["version"] = mcVersion;
                std::string vurl = FindVersionUrl(mcVersion);
                if (vurl.empty()) { fail("未找到原版版本 URL"); return; }
                std::string vtext = HttpFetchText(lxe::Utf8ToWide(vurl));
                if (vtext.empty()) { fail("原版版本信息下载失败"); return; }
                Json vj;
                try { vj = Json::parse(vtext); } catch (...) { fail("原版版本信息解析失败"); return; }
                vj["id"] = safeName;
                std::filesystem::create_directories(verDir, ec);
                std::string out = vj.dump();
                std::ofstream ofs(verDir + L"\\" + safeNameW + L".json", std::ios::binary);
                if (ofs.is_open()) { ofs.write(out.data(), out.size()); ofs.close(); }
                // 自动补下原版客户端 jar（修复：缺游戏本体时启动失败）
                if (!std::filesystem::exists(verDir + L"\\" + safeNameW + L".jar")) {
                    std::string jurl;
                    if (vj.contains("downloads") && vj.at("downloads").isObject() &&
                        vj.at("downloads").contains("client") && vj.at("downloads").at("client").isObject()) {
                        const auto& c = vj.at("downloads").at("client");
                        if (c.contains("url") && c.at("url").isString()) jurl = c.at("url").asString();
                    }
                    if (!jurl.empty()) {
                        postProgress(34, "下载原版客户端 " + mcVersion);
                        if (!DownloadFileSmart(lxe::Utf8ToWide(jurl), verDir, safeNameW + L".jar", nullptr)) {
                            fail("原版客户端下载失败");
                            return;
                        }
                    }
                }
                postProgress(35, "原版版本已创建");
            } else {
                Json ld = Json::object();
                // loader 阶段用独立 taskId：避免其 download.state=done 被前端误判为整合包整体完成
                int loaderTaskId = taskId + 100000;
                loaderInfo["loaderData"] = ld;
                g_installLoaderImpl(bridge, loaderTaskId, loaderId, mcVersion, loaderVersion, safeName, ld, false);
                if (!std::filesystem::exists(mcRoot + L"\\versions\\" + safeNameW + L"\\" + safeNameW + L".json")) {
                    fail("加载器版本安装失败");
                    return;
                }
            }

            int totalFiles = (int)filesArr.asArray().size();
            if (!copyWholeDir && totalFiles == 0) {
                // 两种格式都可能没有远程文件（纯 overrides 整合包）；走 overrides 拷贝即可
                std::wstring overridesDir = contentRoot;
                if (std::filesystem::is_directory(overridesDir, ec)) {
                    postProgress(96, "应用整合包配置");
                    if (packKind == "multimc" && !instPlaceholders.empty()) {
                        CopyDirWithPlaceholders(overridesDir, targetRoot, instPlaceholders, ec);
                    } else {
                        std::filesystem::copy(overridesDir, targetRoot, std::filesystem::copy_options::recursive |
                                                std::filesystem::copy_options::overwrite_existing, ec);
                    }
                }
                std::filesystem::remove_all(extractDir, ec);
                try { std::filesystem::remove(archive); } catch (...) {}
                postProgress(100, "导入完成");
                postState("done");
                return;
            }

            // 阶段 5：按 files[] 多线程下载 mods/config 等到 mcRoot（修复：原本串行；并带每文件进度与失败回退）。
            // copyWholeDir 格式（MCBBS/普通zip/HMCL/MultiMC/带启动器）无远程文件清单 → 直接整目录铺开，不进此分支。
            if (copyWholeDir) {
                if (std::filesystem::is_directory(contentRoot, ec)) {
                    postProgress(96, "应用整合包内容");
                    if (packKind == "multimc" && !instPlaceholders.empty()) {
                        CopyDirWithPlaceholders(contentRoot, targetRoot, instPlaceholders, ec);
                    } else {
                        std::filesystem::copy(contentRoot, targetRoot, std::filesystem::copy_options::recursive |
                                                std::filesystem::copy_options::overwrite_existing, ec);
                    }
                }
                std::filesystem::remove_all(extractDir, ec);
                try { std::filesystem::remove(archive); } catch (...) {}
                postProgress(100, "导入完成");
                postState("done");
                return;
            }
            struct PackFile { std::wstring outDir; std::wstring outName; std::vector<std::wstring> urls; std::string path; };
            auto packFiles = std::make_shared<std::vector<PackFile>>();
            packFiles->reserve(totalFiles);
            for (int i = 0; i < totalFiles; ++i) {
                const Json& f = filesArr.asArray()[i];
                if (!f.isObject()) continue;
                std::string path;
                std::vector<std::wstring> urls;
                if (f.contains("path") && f.at("path").isString()) path = f.at("path").asString();
                // 按顺序收集所有 download 镜像（Modrinth index 第一个是 CDN，之后是 CurseForge 等备用源），
                // 下载时逐个尝试：前一个失败（CDN 不可达）自动回退下一个，避免整个整合包导入失败。
                if (f.contains("downloads") && f.at("downloads").isArray()) {
                    for (const auto& d : f.at("downloads").asArray()) {
                        if (d.isString() && !d.asString().empty()) urls.push_back(lxe::Utf8ToWide(d.asString()));
                    }
                }
                if (path.empty() || urls.empty()) continue;
                // 路径安全：仅允许相对路径，不允许 .. 跳出 targetRoot（隔离时即 versions/<safeName>）
                std::filesystem::path rel(lxe::Utf8ToWide(path));
                if (rel.is_absolute()) continue;
                std::wstring targetRootCheck = targetRoot;
                while (targetRootCheck.size() > 1 && (targetRootCheck.back() == L'\\' || targetRootCheck.back() == L'/')) targetRootCheck.pop_back();
                std::wstring joined = (std::filesystem::path(targetRoot) / rel).wstring();
                if (joined.rfind(targetRootCheck, 0) != 0) continue;  // 越界跳过
                PackFile pf;
                pf.path = path;
                pf.urls = std::move(urls);
                pf.outDir = rel.parent_path().wstring().empty() ? targetRoot : (std::filesystem::path(targetRoot) / rel.parent_path()).wstring();
                pf.outName = rel.filename().wstring();
                std::filesystem::create_directories(pf.outDir, ec);
                packFiles->push_back(std::move(pf));
            }
            int nFiles = (int)packFiles->size();
            if (nFiles == 0) { fail("整合包没有可下载的文件"); return; }

            // 初始化每文件状态
            {
                std::lock_guard<std::mutex> lock(*progressMutex);
                filesState->clear();
                for (const auto& pf : *packFiles) {
                    Json fs = Json::object();
                    fs["name"] = pf.path;
                    fs["state"] = "pending";
                    filesState->push_back(fs);
                }
            }
            postProgress(36, "下载整合包文件（共 " + std::to_string(nFiles) + " 个）");

            int maxConcurrent = packThreads;
            auto errorFlag = std::make_shared<std::atomic<bool>>(false);
            auto nextIndex = std::make_shared<std::atomic<int>>(0);
            auto worker = [&, errorFlag, nextIndex](int /*slot*/) {
                while (!errorFlag->load()) {
                    int i = nextIndex->fetch_add(1);
                    if (i >= nFiles) break;
                    const PackFile& pf = packFiles->at(i);
                    {
                        std::lock_guard<std::mutex> lock(*progressMutex);
                        if (filesState->at(i).contains("state")) (*filesState)[i]["state"] = "downloading";
                    }
                    auto cb = [&, i](const Aria2Progress& p) -> bool {
                        if (errorFlag->load()) return false;
                        int mapped = 36 + (int)(59.0 * (completedFiles->load() + p.percent / 100.0) / nFiles);
                        if (mapped > 95) mapped = 95;
                        std::ostringstream ss; ss << p.speed;
                        postProgress(mapped, "下载整合包文件 " + pf.path, ss.str(), p.eta);
                        return true;
                    };
                    // 目标文件已完整存在（上次导入残留/曾下载过，无 .aria2 控制文件且大小>0）→ 直接复用，跳过下载
                    bool ok = false;
                    {
                        std::error_code xec;
                        std::wstring absPath = pf.outDir + L"\\" + pf.outName;
                        if (std::filesystem::exists(absPath, xec) &&
                            !std::filesystem::exists(absPath + L".aria2", xec) &&
                            std::filesystem::file_size(absPath, xec) > 0)
                            ok = true;
                    }
                    // 逐个尝试下载（优先直连整合包声明的原始 URL，失败自动重试并回退该 URL 的镜像/下一个 URL），全部失败才判该文件失败
                    for (const auto& u : pf.urls) {
                        if (errorFlag->load()) break;
                        if (DownloadFileSmartPreferOriginal(u, pf.outDir, pf.outName, cb)) { ok = true; break; }
                        std::error_code rce;
                        std::wstring absPath = pf.outDir + L"\\" + pf.outName;
                        std::filesystem::remove(absPath, rce);
                        std::filesystem::remove(absPath + L".aria2", rce);
                    }
                    if (ok) {
                        {
                            std::lock_guard<std::mutex> lock(*progressMutex);
                            if (filesState->at(i).contains("state")) (*filesState)[i]["state"] = "done";
                        }
                        ++(*completedFiles);
                    } else {
                        {
                            std::lock_guard<std::mutex> lock(*progressMutex);
                            if (filesState->at(i).contains("state")) (*filesState)[i]["state"] = "error";
                        }
                        errorFlag->store(true);
                    }
                }
            };
            std::vector<std::thread> workers;
            workers.reserve(maxConcurrent);
            for (int w = 0; w < maxConcurrent; ++w) workers.emplace_back(worker, w);
            for (auto& t : workers) if (t.joinable()) t.join();

            if (errorFlag->load()) {
                // 原子回退：删除本次未完成的整合包文件，避免残留半成品
                {
                    std::lock_guard<std::mutex> lock(*progressMutex);
                    for (int i = 0; i < nFiles; ++i) {
                        std::string st = filesState->at(i).contains("state") && filesState->at(i).at("state").isString()
                            ? filesState->at(i).at("state").asString() : "pending";
                        if (st == "done") continue;
                        const PackFile& pf = packFiles->at(i);
                        std::wstring absPath = pf.outDir + L"\\" + pf.outName;
                        std::filesystem::remove(absPath, ec);
                        std::filesystem::remove(absPath + L".aria2", ec);
                    }
                }
                fail("整合包文件下载失败");
                return;
            }

            // 阶段 6：应用 overrides/content（覆盖 targetRoot 下同名文件）
            if (std::filesystem::is_directory(contentRoot, ec)) {
                postProgress(96, "应用整合包配置");
                if (packKind == "multimc" && !instPlaceholders.empty()) {
                    CopyDirWithPlaceholders(contentRoot, targetRoot, instPlaceholders, ec);
                } else {
                    std::filesystem::copy(contentRoot, targetRoot, std::filesystem::copy_options::recursive |
                                            std::filesystem::copy_options::overwrite_existing, ec);
                }
            }

            std::filesystem::remove_all(extractDir, ec);
            try { std::filesystem::remove(archive); } catch (...) {}
            postProgress(100, "导入完成");
            postState("done");
            } catch (const std::exception& e) {
                fail(std::string("整合包安装异常：") + e.what());
            } catch (...) {
                fail("整合包安装未知异常");
            }
        }).detach();

        return Ok(result);
    });

    // ===================== mc.installLoader：官方源完整安装 =====================
    g_installLoaderImpl = [](Bridge& bridge, int taskId,
                             const std::string& loaderId, const std::string& mcVersion,
                             const std::string& loaderVersion, const std::string& installName,
                             const Json& loaderData, bool complete) {
        auto postProgress = [&](int pct, const std::string& stage, const std::string& speed = "", const std::string& eta = "", const std::string& size = "") {
                Json prog = Json::object();
                prog["taskId"] = std::to_string(taskId);
                prog["percent"] = pct;
                prog["stage"] = stage;
                prog["name"] = loaderId + " " + mcVersion + " " + loaderVersion;
                prog["speed"] = speed;
                prog["eta"] = eta;
                prog["size"] = size;
                bridge.PostEvent("download.progress", prog);
            };
            auto postState = [&](const std::string& state) {
                Json ev = Json::object();
                ev["taskId"] = std::to_string(taskId);
                ev["state"] = state;
                ev["name"] = loaderId + " " + mcVersion + " " + loaderVersion;
                bridge.PostEvent("download.state", ev);
            };
            auto fail = [&](const std::string& stage) {
                postProgress(0, "错误：" + stage);
                postState("error");
            };

            // complete=true：安装完成后在同一任务内补全 客户端JAR/依赖库/natives/全部资源（Forge 一次下完、启动前不再补）
            // 补全失败：CompleteVersionFilesWorker 已用同一 taskId 发送 error 状态与原子回退，此处直接退出
            auto finishLoader = [&]() -> bool {
                if (complete) {
                    postProgress(96, "补全版本全部文件（启动前无需再补）");
                    if (!CompleteVersionFilesWorker(bridge, taskId, installName, installName))
                        return false;
                }
                postProgress(100, "安装完成");
                postState("done");
                InvalidateLocalVersionsCache();
                return true;
            };

            try {
            postState("started");
            std::wstring mcRoot = GetMcRoot();
            std::wstring libDir = mcRoot + L"\\libraries";
            std::wstring verDir = mcRoot + L"\\versions\\" + lxe::Utf8ToWide(installName);

            // 读取原版 version.json：优先前端已写入的 versions/<installName>/<installName>.json，否则去官方清单取
            Json vanillaJson;
            bool haveVanilla = false;
            {
                std::wstring localJson = verDir + L"\\" + lxe::Utf8ToWide(installName) + L".json";
                auto [t, ok] = ReadFileUtf8(localJson);
                if (ok && !t.empty()) {
                    try { vanillaJson = Json::parse(t); if (vanillaJson.isObject()) haveVanilla = true; } catch (...) {}
                }
                if (!haveVanilla) {
                    std::string vurl = FindVersionUrl(mcVersion);
                    if (!vurl.empty()) {
                        std::string t = HttpFetchText(lxe::Utf8ToWide(vurl));
                        if (!t.empty()) {
                            try { vanillaJson = Json::parse(t); if (vanillaJson.isObject()) haveVanilla = true; } catch (...) {}
                        }
                    }
                }
            }
            if (!haveVanilla) { fail("未找到原版版本元数据"); return; }

            // 确保 versions/<mc>/<mc>.json 与 <mc>.jar 存在（Forge/OptiFine 安装器需要原版客户端）
            auto ensureVanillaBase = [&]() -> bool {
                std::wstring vdir = mcRoot + L"\\versions\\" + lxe::Utf8ToWide(mcVersion);
                std::wstring jp = vdir + L"\\" + lxe::Utf8ToWide(mcVersion) + L".json";
                std::wstring jarp = vdir + L"\\" + lxe::Utf8ToWide(mcVersion) + L".jar";
                if (std::filesystem::exists(jp) && std::filesystem::exists(jarp)) return true;
                std::string vurl = FindVersionUrl(mcVersion);
                if (vurl.empty()) return false;
                std::string text = HttpFetchText(lxe::Utf8ToWide(vurl));
                if (text.empty()) return false;
                Json vj;
                try { vj = Json::parse(text); } catch (...) { return false; }
                std::error_code ec;
                std::filesystem::create_directories(vdir, ec);
                if (!std::filesystem::exists(jp)) {
                    std::ofstream ofs(jp, std::ios::binary);
                    if (ofs.is_open()) { ofs.write(text.data(), text.size()); ofs.close(); }
                }
                if (!std::filesystem::exists(jarp)) {
                    std::string jurl;
                    if (vj.contains("downloads") && vj.at("downloads").isObject() &&
                        vj.at("downloads").contains("client") && vj.at("downloads").at("client").isObject()) {
                        const auto& c = vj.at("downloads").at("client");
                        if (c.contains("url") && c.at("url").isString()) jurl = c.at("url").asString();
                    }
                    if (!jurl.empty()) {
                        if (!DownloadFileSmart(lxe::Utf8ToWide(jurl), vdir, lxe::Utf8ToWide(mcVersion) + L".jar", nullptr))
                            return false;
                    }
                }
                return std::filesystem::exists(jp) && std::filesystem::exists(jarp);
            };

            // 为缺少 downloads.artifact 的库项补全（从 name 推导相对路径；url 为空表示已本地存在）
            auto ensureArtifact = [](Json lib) -> Json {
                if (!lib.isObject()) return lib;
                bool hasArt = lib.contains("downloads") && lib.at("downloads").isObject() &&
                              lib.at("downloads").contains("artifact") && lib.at("downloads").at("artifact").isObject();
                if (!hasArt) {
                    std::string name;
                    if (lib.contains("name") && lib.at("name").isString()) name = lib.at("name").asString();
                    std::string path = MavenCoordToPath(name);
                    if (!path.empty()) {
                        Json dl = Json::object();
                        Json art = Json::object();
                        art["path"] = path;
                        if (lib.contains("url") && lib.at("url").isString()) {
                            std::string u = lib.at("url").asString();
                            while (!u.empty() && u.back() == '/') u.pop_back();
                            art["url"] = u + "/" + path;
                        } else {
                            art["url"] = "";
                        }
                        if (lib.contains("size") && lib.at("size").isNumber()) art["size"] = lib.at("size");
                        if (lib.contains("sha1") && lib.at("sha1").isString()) art["sha1"] = lib.at("sha1").asString();
                        dl["artifact"] = art;
                        lib["downloads"] = dl;
                    }
                }
                return lib;
            };

            // 合并 libraries：原版 + 加载器（补全 artifact）
            auto mergeLibraries = [&](const Json& loaderJson) -> Json {
                Json libs = Json::array();
                if (vanillaJson.contains("libraries") && vanillaJson.at("libraries").isArray())
                    for (const auto& l : vanillaJson.at("libraries").asArray()) libs.asArray().push_back(l);
                if (loaderJson.contains("libraries") && loaderJson.at("libraries").isArray())
                    for (const auto& l : loaderJson.at("libraries").asArray())
                        libs.asArray().push_back(ensureArtifact(l));
                return libs;
            };

            // 合并 arguments：game / jvm（原版 + 加载器）
            auto appendArgKey = [](Json& args, const char* key, const Json& base, const Json& extra) {
                Json arr = Json::array();
                auto collect = [&](const Json& src) {
                    if (src.contains("arguments") && src.at("arguments").isObject() &&
                        src.at("arguments").contains(key) && src.at("arguments").at(key).isArray()) {
                        for (const auto& a : src.at("arguments").at(key).asArray()) arr.asArray().push_back(a);
                    }
                };
                collect(base);
                collect(extra);
                args[key] = arr;
            };

            // 写入合并后的自包含版本 json 到 versions/<installName>/<installName>.json
            auto writeMerged = [&](const Json& loaderJson, const std::string& mainClass, const std::string& stage) -> bool {
                Json merged = vanillaJson;
                if (merged.contains("inheritsFrom")) merged.asObject().erase("inheritsFrom");
                merged["id"] = installName;
                if (!mainClass.empty()) merged["mainClass"] = mainClass;
                Json args = Json::object();
                appendArgKey(args, "game", vanillaJson, loaderJson);
                appendArgKey(args, "jvm", vanillaJson, loaderJson);
                merged["arguments"] = args;
                merged["libraries"] = mergeLibraries(loaderJson);
                std::error_code ec;
                std::filesystem::create_directories(verDir, ec);
                std::wstring jsonPath = verDir + L"\\" + lxe::Utf8ToWide(installName) + L".json";
                std::string out = merged.dump();
                std::ofstream ofs(jsonPath, std::ios::binary);
                if (!ofs.is_open()) return false;
                ofs.write(out.data(), out.size());
                ofs.close();
                // 自动补下原版客户端 jar 到 versions/<installName>/<installName>.jar（修复：整合包/加载器版本缺游戏本体导致启动失败）
                std::wstring jarPath = verDir + L"\\" + lxe::Utf8ToWide(installName) + L".jar";
                if (!std::filesystem::exists(jarPath)) {
                    std::string jurl;
                    if (vanillaJson.contains("downloads") && vanillaJson.at("downloads").isObject() &&
                        vanillaJson.at("downloads").contains("client") && vanillaJson.at("downloads").at("client").isObject()) {
                        const auto& c = vanillaJson.at("downloads").at("client");
                        if (c.contains("url") && c.at("url").isString()) jurl = c.at("url").asString();
                    }
                    if (!jurl.empty()) {
                        postProgress(94, "下载原版客户端 " + mcVersion);
                        DownloadFileSmart(lxe::Utf8ToWide(jurl), verDir, lxe::Utf8ToWide(installName) + L".jar", nullptr);
                    }
                }
                postProgress(95, stage);
                return true;
            };

            if (loaderId == "fabric") {
                // ===== Fabric：官方 meta profile json + 下载库文件（无需 Java） =====
                postProgress(5, "获取 Fabric 元数据");
                std::string profileUrl = "https://meta.fabricmc.net/v2/versions/loader/" +
                    mcVersion + "/" + loaderVersion + "/profile/json";
                std::string ptext = HttpFetchText(lxe::Utf8ToWide(profileUrl));
                if (ptext.empty()) { fail("Fabric 元数据下载失败"); return; }
                Json profile;
                try { profile = Json::parse(ptext); } catch (...) { fail("Fabric 元数据解析失败"); return; }
                if (!profile.isObject()) { fail("Fabric 元数据无效"); return; }

                // 下载 Fabric 库文件（10%-85%）
                if (profile.contains("libraries") && profile.at("libraries").isArray()) {
                    const auto& libs = profile.at("libraries").asArray();
                    for (size_t i = 0; i < libs.size(); ++i) {
                        const auto& lib = libs[i];
                        if (!lib.isObject()) continue;
                        if (!(lib.contains("name") && lib.at("name").isString())) continue;
                        if (!(lib.contains("url") && lib.at("url").isString())) continue;
                        std::string name = lib.at("name").asString();
                        std::string path = MavenCoordToPath(name);
                        if (path.empty()) continue;
                        std::string base = lib.at("url").asString();
                        while (!base.empty() && base.back() == '/') base.pop_back();
                        std::string url = base + "/" + path;
                        int s = 10 + (int)(75.0 * i / libs.size());
                        int e = 10 + (int)(75.0 * (i + 1) / libs.size());
                        std::filesystem::path lp(lxe::Utf8ToWide(path));
                        std::wstring outDir = libDir + L"\\" + lp.parent_path().wstring();
                        std::wstring outName = lp.filename().wstring();
                        auto cb = [&](const Aria2Progress& p) -> bool {
                            int mapped = s + p.percent * (e - s) / 100;
                            std::ostringstream ss; ss << p.speed;
                            postProgress(mapped, "下载 Fabric 依赖库 " + path, ss.str(), p.eta);
                            return true;
                        };
                        if (!DownloadFileSmart(lxe::Utf8ToWide(url), outDir, outName, cb)) {
                            fail("Fabric 依赖库下载失败：" + path);
                            return;
                        }
                        postProgress(e, "下载 Fabric 依赖库 " + path);
                    }
                }

                std::string mainClass;
                if (profile.contains("mainClass") && profile.at("mainClass").isString())
                    mainClass = profile.at("mainClass").asString();
                if (!writeMerged(profile, mainClass, "写入 Fabric 加载器版本"))
                    { fail("写入版本 JSON 失败"); return; }
                if (!finishLoader()) return;
            } else if (loaderId == "forge") {
                // ===== Forge 安装：新版安装器（含 processors）按教程手动执行 processor 流程；旧版（universal jar）走官方 --installClient =====
                // artifact 按教程的版本规则计算（1.8.9 / 1.7.x 需带 -{mc} 后缀），否则安装路径/版本目录名对不上
                std::string artifact = ForgeArtifactVersion(mcVersion, loaderVersion);
                postProgress(3, "下载 Forge 安装器");
                std::string jarUrl = "https://maven.minecraftforge.net/net/minecraftforge/forge/" +
                    artifact + "/forge-" + artifact + "-installer.jar";
                // 官方源下载失败时回退 BMCLAPI 镜像（教程推荐使用镜像加速安装器/依赖的获取）
                std::string mirrorUrl = "https://bmclapi2.bangbang93.com/maven/net/minecraftforge/forge/" +
                    artifact + "/forge-" + artifact + "-installer.jar";
                std::wstring tmpDir = mcRoot + L"\\temp";
                std::wstring jarName = L"forge-" + lxe::Utf8ToWide(artifact) + L"-installer.jar";
                std::wstring installerJar = tmpDir + L"\\" + jarName;
                {
                    auto cb = [&](const Aria2Progress& p) -> bool {
                        int mapped = 3 + p.percent * 9 / 100;
                        std::ostringstream ss; ss << p.speed;
                        postProgress(mapped, "下载 Forge 安装器", ss.str(), p.eta);
                        return true;
                    };
                    if (!DownloadFileSmart(lxe::Utf8ToWide(jarUrl), tmpDir, jarName, cb)) {
                        if (!DownloadFileSmart(lxe::Utf8ToWide(mirrorUrl), tmpDir, jarName, cb)) {
                            fail("Forge 安装器下载失败");
                            return;
                        }
                    }
                }

                postProgress(12, "选择 Java 运行环境");
                int recMajor = RecommendedJavaMajor(mcVersion);
                auto javas0 = ScanInstalledJavas();
                bool recFound0 = false;
                for (const auto& j : javas0) if (j.major == recMajor) { recFound0 = true; break; }
                std::wstring java = PickJavaForMajor(javas0, recMajor);
                if (java.empty()) java = FindJavaPath();
                if (java.empty()) { fail("未检测到 Java：安装 Forge 需要 Java 运行环境，请先在下载中心安装 Java 或指定 Java 路径"); return; }
                if (!recFound0)
                    postProgress(12, "提示：未匹配到推荐 Java " + std::to_string(recMajor) + "，将使用现有 Java 继续（仅提示，非强制）");

                if (!ensureVanillaBase()) { fail("无法准备原版版本（安装器需要）"); return; }
                std::wstring vanillaJar = mcRoot + L"\\versions\\" + lxe::Utf8ToWide(mcVersion) +
                                          L"\\" + lxe::Utf8ToWide(mcVersion) + L".jar";

                // 读取安装器内 install_profile.json，据此判断新版（含 processors）/旧版（universal jar 型）安装器
                std::wstring extractDir = tmpDir + L"\\forge_" + lxe::Utf8ToWide(artifact);
                {
                    std::error_code ec;
                    std::filesystem::remove_all(extractDir, ec);
                    std::filesystem::create_directories(extractDir, ec);
                }
                Json installProfile;
                bool haveProfile = false;
                if (ExtractZipEntry(installerJar, "install_profile.json", extractDir)) {
                    auto [pt, pok] = ReadFileUtf8(extractDir + L"\\install_profile.json");
                    if (pok && !pt.empty()) { try { installProfile = Json::parse(pt); if (installProfile.isObject()) haveProfile = true; } catch (...) {} }
                }
                bool newInstaller = false;
                if (haveProfile && installProfile.contains("processors") && installProfile.at("processors").isArray() &&
                    !installProfile.at("processors").asArray().empty())
                    newInstaller = true;

                if (newInstaller) {
                    // ===== 新版安装器：下载依赖库 → 提取 client.lzma / mojmaps → 手动执行 processors（教程 Part 3） =====

                    // 1) 下载 install_profile.json 的 libraries（processor 依赖 + forge 运行库），缺 url 时推导标准 maven 路径
                    postProgress(14, "下载 Forge 依赖库");
                    if (installProfile.contains("libraries") && installProfile.at("libraries").isArray()) {
                        const auto& plibs = installProfile.at("libraries").asArray();
                        int n = 0;
                        for (const auto& lib : plibs) {
                            if (!lib.isObject() || !lib.contains("name") || !lib.at("name").isString()) continue;
                            // 跳过仅 server 侧需要的库
                            if (lib.contains("serverreq") && lib.at("serverreq").isBool() && lib.at("serverreq").asBool() &&
                                !(lib.contains("clientreq") && lib.at("clientreq").isBool() && lib.at("clientreq").asBool())) continue;
                            if (lib.contains("clientreq") && lib.at("clientreq").isBool() && !lib.at("clientreq").asBool()) continue;
                            std::string coord = lib.at("name").asString();
                            std::string path = MavenCoordToPath(coord);
                            if (path.empty()) continue;
                            std::wstring lp(lxe::Utf8ToWide(path));
                            std::wstring outDir = libDir + L"\\" + std::filesystem::path(lp).parent_path().wstring();
                            std::wstring outName = std::filesystem::path(lp).filename().wstring();
                            if (std::filesystem::exists(outDir + L"\\" + outName)) continue;
                            std::string url;
                            if (lib.contains("downloads") && lib.at("downloads").isObject() &&
                                lib.at("downloads").contains("artifact") && lib.at("downloads").at("artifact").isObject() &&
                                lib.at("downloads").at("artifact").contains("url") &&
                                lib.at("downloads").at("artifact").at("url").isString())
                                url = lib.at("downloads").at("artifact").at("url").asString();
                            else if (lib.contains("url") && lib.at("url").isString()) {
                                std::string base = lib.at("url").asString();
                                while (!base.empty() && base.back() == '/') base.pop_back();
                                url = base + "/" + path;
                            } else {
                                url = "https://files.minecraftforge.net/maven/" + path;
                            }
                            int mapped = 14 + 16 * (n++) / std::max<int>(1, (int)plibs.size());
                            auto cb = [&](const Aria2Progress& p) -> bool {
                                int m2 = mapped + p.percent * 3 / 100;
                                std::ostringstream ss; ss << p.speed;
                                postProgress(m2, "下载 Forge 依赖库 " + path, ss.str(), p.eta);
                                return true;
                            };
                            std::wstring embedRoot = tmpDir + L"\\forge_embed";
                            bool dlok = false;
                            if (url.empty()) {
                                // artifacts.url 为空（如 forge:{artifact}:universal）：直接走内嵌 maven / 镜像
                                dlok = CopyEmbeddedMavenLib(installerJar, path, libDir, embedRoot);
                                if (!dlok)
                                    dlok = DownloadFileSmart(lxe::Utf8ToWide(BmclMavenMirror("https://files.minecraftforge.net/maven/" + path)),
                                                             outDir, outName, cb);
                            } else {
                                dlok = DownloadFileSmart(lxe::Utf8ToWide(url), outDir, outName, cb);
                                if (!dlok) dlok = DownloadFileSmart(lxe::Utf8ToWide(BmclMavenMirror(url)), outDir, outName, cb);
                                if (!dlok) dlok = CopyEmbeddedMavenLib(installerJar, path, libDir, embedRoot);
                            }
                            if (!dlok) { fail("Forge 依赖库下载失败：" + path); return; }
                        }
                    }

                    // 1.5) 嵌入 maven 启动库：forge-{artifact}.jar（无分类器，version.json 引用且 url 为空）
                    CopyEmbeddedMavenLib(installerJar, "net/minecraftforge/forge/" + artifact + "/forge-" + artifact + ".jar",
                                         libDir, tmpDir + L"\\forge_embed");

                    // 1.6) 下载 version.json 的启动依赖库（modlauncher/eventbus/… 在版本 json 中 url 为空，须由安装器补下到 libraries/）
                    {
                        std::wstring vjPath = extractDir + L"\\version.json";
                        auto [vjt, vjok] = ReadFileUtf8(vjPath);
                        if (vjok && !vjt.empty()) {
                            Json vjLibs;
                            try { vjLibs = Json::parse(vjt); } catch (...) {}
                            if (vjLibs.isObject() && vjLibs.contains("libraries") && vjLibs.at("libraries").isArray()) {
                                std::wstring embedRoot2 = tmpDir + L"\\forge_embed2";
                                const auto& vls = vjLibs.at("libraries").asArray();
                                int k = 0;
                                for (const auto& lib : vls) {
                                    if (!lib.isObject() || !lib.contains("name") || !lib.at("name").isString()) continue;
                                    if (lib.contains("serverreq") && lib.at("serverreq").isBool() && lib.at("serverreq").asBool() &&
                                        !(lib.contains("clientreq") && lib.at("clientreq").isBool() && lib.at("clientreq").asBool())) continue;
                                    if (lib.contains("clientreq") && lib.at("clientreq").isBool() && !lib.at("clientreq").asBool()) continue;
                                    std::string coord2 = lib.at("name").asString();
                                    std::string path2 = MavenCoordToPath(coord2);
                                    if (path2.empty()) continue;
                                    // 已由原版 json 自带（log4j 等）→ 启动时按原版 URL 补下，跳过
                                    bool inVanilla = false;
                                    if (vanillaJson.contains("libraries") && vanillaJson.at("libraries").isArray()) {
                                        for (const auto& vll : vanillaJson.at("libraries").asArray()) {
                                            if (vll.isObject() && vll.contains("name") && vll.at("name").isString() &&
                                                vll.at("name").asString() == coord2) { inVanilla = true; break; }
                                        }
                                    }
                                    if (inVanilla) continue;
                                    std::wstring lp2(lxe::Utf8ToWide(path2));
                                    std::wstring outDir2 = libDir + L"\\" + std::filesystem::path(lp2).parent_path().wstring();
                                    std::wstring outName2 = std::filesystem::path(lp2).filename().wstring();
                                    if (std::filesystem::exists(outDir2 + L"\\" + outName2)) continue;
                                    std::string vurl2;
                                    if (lib.contains("downloads") && lib.at("downloads").isObject() &&
                                        lib.at("downloads").contains("artifact") && lib.at("downloads").at("artifact").isObject() &&
                                        lib.at("downloads").at("artifact").contains("url") &&
                                        lib.at("downloads").at("artifact").at("url").isString())
                                        vurl2 = lib.at("downloads").at("artifact").at("url").asString();
                                    // 版本 json 中 url 通常为空 → 用 maven 镜像补下
                                    std::string fullUrl2 = vurl2.empty() ? "https://maven.minecraftforge.net/" + path2 : vurl2;
                                    int m6 = 15 + 5 * (k++) / std::max<int>(1, (int)vls.size());
                                    auto cb6 = [&](const Aria2Progress& p) -> bool {
                                        int m7 = m6 + p.percent * 2 / 100;
                                        std::ostringstream ss; ss << p.speed;
                                        postProgress(m7, "下载 Forge 启动库 " + path2, ss.str(), p.eta);
                                        return true;
                                    };
                                    bool d6 = DownloadFileSmart(lxe::Utf8ToWide(fullUrl2), outDir2, outName2, cb6);
                                    if (!d6) d6 = DownloadFileSmart(lxe::Utf8ToWide(BmclMavenMirror(fullUrl2)), outDir2, outName2, cb6);
                                    if (!d6) d6 = CopyEmbeddedMavenLib(installerJar, path2, libDir, embedRoot2);
                                    if (!d6) { fail("Forge 启动库下载失败：" + path2); return; }
                                }
                            }
                        }
                    }

                    // 2) 若存在 DOWNLOAD_MOJMAPS processor：自行从原版 json 下载 client_mappings 到 data.MOJMAPS.client，随后跳过该 processor
                    if (installProfile.contains("processors") && installProfile.at("processors").isArray()) {
                        for (const auto& proc : installProfile.at("processors").asArray()) {
                            if (!proc.isObject() || !proc.contains("args") || !proc.at("args").isArray()) continue;
                            bool isMojmap = false;
                            for (const auto& a : proc.at("args").asArray())
                                if (a.isString() && a.asString() == "DOWNLOAD_MOJMAPS") { isMojmap = true; break; }
                            if (!isMojmap) continue;
                            std::string targetCoord;
                            if (installProfile.contains("data") && installProfile.at("data").isObject()) {
                                const auto& data = installProfile.at("data");
                                if (data.contains("MOJMAPS") && data.at("MOJMAPS").isObject()) {
                                    const auto& mo = data.at("MOJMAPS");
                                    if (mo.contains("client") && mo.at("client").isString()) targetCoord = mo.at("client").asString();
                                    if (targetCoord.size() >= 2 && targetCoord.front() == '[' && targetCoord.back() == ']')
                                        targetCoord = targetCoord.substr(1, targetCoord.size() - 2);
                                }
                            }
                            std::wstring targetPath = targetCoord.empty() ? L"" : ForgeLibFullPath(libDir, targetCoord);
                            std::string murl;
                            if (vanillaJson.contains("downloads") && vanillaJson.at("downloads").isObject() &&
                                vanillaJson.at("downloads").contains("client_mappings") &&
                                vanillaJson.at("downloads").at("client_mappings").isObject()) {
                                const auto& cm = vanillaJson.at("downloads").at("client_mappings");
                                if (cm.contains("url") && cm.at("url").isString()) murl = cm.at("url").asString();
                            }
                            if (!targetPath.empty() && !murl.empty()) {
                                std::error_code mec;
                                std::filesystem::create_directories(std::filesystem::path(targetPath).parent_path(), mec);
                                DownloadFileSmart(lxe::Utf8ToWide(murl), std::filesystem::path(targetPath).parent_path(),
                                                  std::filesystem::path(targetPath).filename(), nullptr);
                            }
                            break; // 只处理第一个 MOJMAPS processor
                        }
                    }

                    // 3) 提取 data/client.lzma 到 BINPATCH 对应库路径（net.minecraftforge:forge:{artifact}:clientdata@lzma）
                    std::wstring binpatchPath = ForgeLibFullPath(libDir, "net.minecraftforge:forge:" + artifact + ":clientdata@lzma");
                    if (!binpatchPath.empty()) {
                        std::wstring lzmaExtractDir = tmpDir + L"\\forge_lzma";
                        if (ExtractZipEntry(installerJar, "data/client.lzma", lzmaExtractDir)) {
                            std::wstring srcLzma = lzmaExtractDir + L"\\data\\client.lzma";
                            if (std::filesystem::exists(srcLzma)) {
                                std::error_code lec;
                                std::filesystem::create_directories(std::filesystem::path(binpatchPath).parent_path(), lec);
                                std::filesystem::copy_file(srcLzma, binpatchPath,
                                                           std::filesystem::copy_options::overwrite_existing, lec);
                            }
                        }
                    }

                    // 4) 依次执行 client 侧 processors；args 中 {KEY} / [coordinate] 按教程规则替换
                    postProgress(35, "运行 Forge 处理器（可能需要数分钟）");
                    auto resolveForgeArg = [&](const std::string& token) -> std::wstring {
                        std::string res = token;
                        for (;;) {
                            size_t b = res.find('{');
                            if (b == std::string::npos) break;
                            size_t e = res.find('}', b + 1);
                            if (e == std::string::npos) break;
                            std::string key = res.substr(b + 1, e - b - 1);
                            std::wstring val;
                            if (key == "MINECRAFT_JAR") val = vanillaJar;
                            else if (key == "INSTALLER") val = installerJar;
                            else if (key == "SIDE") val = L"client";
                            else if (key == "ROOT") val = mcRoot;
                            else if (key == "BINPATCH") val = binpatchPath;
                            else if (installProfile.contains("data") && installProfile.at("data").isObject()) {
                                const auto& data = installProfile.at("data");
                                if (data.contains(key) && data.at(key).isObject()) {
                                    const auto& dk = data.at(key);
                                    std::string v;
                                    if (dk.contains("client") && dk.at("client").isString()) v = dk.at("client").asString();
                                    bool bracket = v.size() >= 2 && v.front() == '[' && v.back() == ']';
                                    if (bracket) v = v.substr(1, v.size() - 2);
                                    if (bracket) val = ForgeLibFullPath(libDir, v);
                                    else val = lxe::Utf8ToWide(v);
                                }
                            }
                            res = res.substr(0, b) + (val.empty() ? "" : lxe::WideToUtf8(val)) + res.substr(e + 1);
                        }
                        if (!res.empty() && res.front() == '[' && res.back() == ']')
                            return ForgeLibFullPath(libDir, res.substr(1, res.size() - 2));
                        return lxe::Utf8ToWide(res);
                    };
                    int procCount = 0;
                    for (const auto& proc : installProfile.at("processors").asArray()) {
                        if (!proc.isObject()) continue;
                        bool doClient = true;
                        if (proc.contains("sides") && proc.at("sides").isArray()) {
                            doClient = false;
                            for (const auto& s : proc.at("sides").asArray())
                                if (s.isString() && s.asString() == "client") { doClient = true; break; }
                        }
                        if (!doClient) continue;
                        if (!proc.contains("jar") || !proc.at("jar").isString()) continue;
                        std::string pjar = proc.at("jar").asString();
                        bool skip = false;
                        if (proc.contains("args") && proc.at("args").isArray()) {
                            for (const auto& a : proc.at("args").asArray())
                                if (a.isString() && a.asString() == "DOWNLOAD_MOJMAPS") { skip = true; break; }
                        }
                        if (skip) continue;
                        std::wstring pjarPath = ForgeLibFullPath(libDir, pjar);
                        if (pjarPath.empty() || !std::filesystem::exists(pjarPath)) { fail("Forge 处理器缺失：" + pjar); return; }
                        std::wstring mainClass = lxe::Utf8ToWide(ReadJarMainClass(pjarPath));
                        if (mainClass.empty()) { fail("无法读取 Forge 处理器主类：" + pjar); return; }
                        std::vector<std::wstring> cps;
                        cps.push_back(pjarPath);
                        if (proc.contains("classpath") && proc.at("classpath").isArray()) {
                            for (const auto& c : proc.at("classpath").asArray()) {
                                if (!c.isString()) continue;
                                std::wstring cp = ForgeLibFullPath(libDir, c.asString());
                                if (!cp.empty()) cps.push_back(cp);
                            }
                        }
                        std::wstring cpAll;
                        for (size_t i = 0; i < cps.size(); ++i) { if (i) cpAll += L";"; cpAll += cps[i]; }
                        std::wstring argLine;
                        if (proc.contains("args") && proc.at("args").isArray()) {
                            for (const auto& a : proc.at("args").asArray()) {
                                if (!a.isString()) continue;
                                std::wstring av = resolveForgeArg(a.asString());
                                if (av.empty()) continue;
                                if (!argLine.empty()) argLine += L" ";
                                if (av.find(L' ') != std::wstring::npos || av.find(L'\t') != std::wstring::npos)
                                    argLine += L"\"" + av + L"\"";
                                else argLine += av;
                            }
                        }
                        std::wstring cmd = L"\"" + java + L"\" -cp \"" + cpAll + L"\" " + mainClass + L" " + argLine;
                        int rc = RunProcessSilent(cmd, mcRoot);
                        if (rc != 0) { fail("Forge 处理器执行失败：" + pjar + "（退出码 " + std::to_string(rc) + "）"); return; }
                        ++procCount;
                    }
                    if (procCount == 0) { fail("未找到需要执行的 Forge 处理器"); return; }

                    // 5) 读取安装器内 version.json（缺省时用 install_profile.versionInfo）并合并到 installName
                    postProgress(90, "读取 Forge 版本元数据");
                    Json forgeJson;
                    bool haveForgeJson = false;
                    if (ExtractZipEntry(installerJar, "version.json", extractDir)) {
                        auto [vt, vok] = ReadFileUtf8(extractDir + L"\\version.json");
                        if (vok && !vt.empty()) { try { forgeJson = Json::parse(vt); if (forgeJson.isObject()) haveForgeJson = true; } catch (...) {} }
                    }
                    if (!haveForgeJson && installProfile.contains("versionInfo") && installProfile.at("versionInfo").isObject())
                        { forgeJson = installProfile.at("versionInfo"); haveForgeJson = true; }
                    if (!haveForgeJson) { fail("未找到 Forge 版本元数据（version.json）"); return; }

                    std::string mainClass;
                    if (forgeJson.contains("mainClass") && forgeJson.at("mainClass").isString())
                        mainClass = forgeJson.at("mainClass").asString();
                    if (!writeMerged(forgeJson, mainClass, "写入 Forge 加载器版本"))
                        { fail("写入版本 JSON 失败"); return; }
                    {
                        std::wstring genDir2 = mcRoot + L"\\versions\\" + lxe::Utf8ToWide(artifact);
                        std::error_code ec2;
                        if (genDir2 != verDir) std::filesystem::remove_all(genDir2, ec2);
                        std::filesystem::remove_all(extractDir, ec2);
                    }
                    if (!finishLoader()) return;
                } else {
                    // ===== 旧版安装器（universal jar 型，无 processors）：官方 --installClient =====
                    postProgress(15, "运行 Forge 安装器（可能需要数分钟）");
                    std::wstring cmd = L"\"" + java + L"\" -jar \"" + installerJar +
                                       L"\" --installClient \"" + mcRoot + L"\"";
                    int rc = RunProcessSilent(cmd, mcRoot);
                    if (rc != 0) { fail("Forge 安装器执行失败（退出码 " + std::to_string(rc) + "）"); return; }

                    postProgress(55, "读取安装结果");
                    std::wstring genDir = mcRoot + L"\\versions\\" + lxe::Utf8ToWide(artifact);
                    std::wstring genJsonPath = genDir + L"\\" + lxe::Utf8ToWide(artifact) + L".json";
                    auto [gt, gok] = ReadFileUtf8(genJsonPath);
                    if (!gok || gt.empty()) { fail("未找到安装器生成的版本 JSON"); return; }
                    Json genJson;
                    try { genJson = Json::parse(gt); } catch (...) { fail("安装器生成 JSON 解析失败"); return; }

                    std::string mainClass;
                    if (genJson.contains("mainClass") && genJson.at("mainClass").isString())
                        mainClass = genJson.at("mainClass").asString();
                    if (!writeMerged(genJson, mainClass, "写入 Forge 加载器版本"))
                        { fail("写入版本 JSON 失败"); return; }
                    if (genDir != verDir) {
                        std::error_code ec;
                        std::filesystem::remove_all(genDir, ec);
                    }
                    if (!finishLoader()) return;
                }
} else if (loaderId == "optifine") {
                // ===== OptiFine：官方直链下载 + 官方安装器（需 Java）后合并到 installName =====
                std::string filename;
                if (loaderData.contains("filename") && loaderData.at("filename").isString())
                    filename = loaderData.at("filename").asString();
                std::string type, patch;
                if (loaderData.contains("type") && loaderData.at("type").isString()) type = loaderData.at("type").asString();
                if (loaderData.contains("patch") && loaderData.at("patch").isString()) patch = loaderData.at("patch").asString();
                if (filename.empty() || type.empty() || patch.empty()) { fail("缺少 OptiFine 版本信息"); return; }

                postProgress(3, "下载 OptiFine 安装包");
                // 回退下载方式：BMCLAPI https 直链下载（bmclapi2 优先，失败回退 bmclapi1），
                // 下载后校验内容确为 JAR（PK 头），避免镜像限流/宕机时返回 HTML 错误页却被当作有效安装器。
                std::wstring tmpDir = mcRoot + L"\\temp";
                std::wstring jarName = lxe::Utf8ToWide(filename);
                std::wstring fullPath = tmpDir + L"\\" + jarName;
                {
                    std::wstring rel = lxe::Utf8ToWide(BmclOptifineVersionPath(mcVersion) + "/" + type + "/" + patch);
                    auto cb = [&](const Aria2Progress& p) -> bool {
                        int mapped = 3 + p.percent * 9 / 100;
                        std::ostringstream ss; ss << p.speed;
                        postProgress(mapped, "下载 OptiFine 安装包", ss.str(), p.eta);
                        return true;
                    };
                    bool ok = DownloadFileSmart(L"https://bmclapi2.bangbang93.com/optifine/" + rel, tmpDir, jarName, cb);
                    if (!ok) {
                        std::error_code ec;
                        std::filesystem::remove(fullPath, ec);
                        std::filesystem::remove(fullPath + L".aria2", ec);
                        ok = DownloadFileSmart(L"https://bmclapi.bangbang93.com/optifine/" + rel, tmpDir, jarName, cb);
                    }
                    if (!ok) { fail("OptiFine 安装包下载失败（BMCLAPI 镜像不可用）"); return; }
                    if (!FileLooksLikeZip(fullPath)) {
                        std::error_code ec;
                        std::filesystem::remove(fullPath, ec);
                        std::filesystem::remove(fullPath + L".aria2", ec);
                        fail("OptiFine 安装包无效（镜像返回异常内容）"); return;
                    }
                }

                postProgress(12, "选择 Java 运行环境");
                // 优先按安装器入口类字节码版本反推最低 Java 大版本（§4.1.3），回退按 MC 版本推荐
                int recMajor = RecommendedJavaMajor(mcVersion);
                int reqMajor = recMajor;
                {
                    std::wstring clsDir = tmpDir + L"\\ofcls_" + lxe::Utf8ToWide(installName);
                    std::error_code cec;
                    int cm = ReadJarEntryClassMajor(fullPath, "optifine/Installer.class", clsDir);
                    std::filesystem::remove_all(clsDir, cec);
                    if (cm >= 52) {
                        int jm = cm - 44;
                        if (jm > reqMajor) reqMajor = jm;
                    }
                }
                auto javas0 = ScanInstalledJavas();
                bool recFound0 = false;
                for (const auto& j : javas0) if (j.major == reqMajor) { recFound0 = true; break; }
                std::wstring java = PickJavaForMajor(javas0, reqMajor);
                if (java.empty()) java = FindJavaPath();
                if (java.empty()) { fail("未检测到 Java：安装 OptiFine 需要 Java 运行环境，请先在下载中心安装 Java 或指定 Java 路径"); return; }
                if (!recFound0)
                    postProgress(12, "提示：未匹配到推荐 Java " + std::to_string(reqMajor) + "，将使用现有 Java 继续（仅提示，非强制）");

                if (!ensureVanillaBase()) { fail("无法准备原版版本（安装器需要）"); return; }

                if (IsOldOptifineMc(mcVersion)) {
                    // ===== 旧版（<1.13）：纯手工构造，不运行安装器（§4.2） =====
                    // 旧版 OptiFine 以“覆写原版 jar + launchwrapper tweak”方式工作：
                    // 复制原版 jar 为新版本 jar → OptiFine 主 jar 入库目录 → launchwrapper → 构造继承原版的版本描述
                    postProgress(14, "准备旧版 OptiFine 版本");
                    std::wstring mvW = lxe::Utf8ToWide(mcVersion);
                    std::error_code ec;
                    std::filesystem::create_directories(verDir, ec);
                    // 1) 复制原版 jar 为新版本 jar
                    std::wstring dstJar = verDir + L"\\" + lxe::Utf8ToWide(installName) + L".jar";
                    std::wstring srcJar = mcRoot + L"\\versions\\" + mvW + L"\\" + mvW + L".jar";
                    if (!std::filesystem::exists(srcJar)) { fail("原版客户端缺失：" + mcVersion); return; }
                    std::filesystem::copy_file(srcJar, dstJar, std::filesystem::copy_options::overwrite_existing, ec);
                    // 2) OptiFine 主 jar 放库目录（maven 坐标 optifine:OptiFine:<mc>_<type>_<patch>）
                    std::string ofCoord = mcVersion + "_" + type + "_" + patch;
                    std::string ofPath = "optifine/OptiFine/" + ofCoord + "/OptiFine-" + ofCoord + ".jar";
                    std::filesystem::path ofp(lxe::Utf8ToWide(ofPath));
                    std::wstring ofDir = libDir + L"\\" + ofp.parent_path().wstring();
                    std::wstring ofName = ofp.filename().wstring();
                    if (!std::filesystem::exists(ofDir + L"\\" + ofName)) {
                        postProgress(20, "放置 OptiFine 主 jar");
                        std::filesystem::create_directories(ofDir, ec);
                        std::filesystem::copy_file(fullPath, ofDir + L"\\" + ofName, std::filesystem::copy_options::overwrite_existing, ec);
                    }
                    // 3) launchwrapper（旧版 OptiFine 依赖，OptiFineTweaker 运行于其上）
                    std::string lwPath = "net/minecraft/launchwrapper/1.12/launchwrapper-1.12.jar";
                    std::filesystem::path lwp(lxe::Utf8ToWide(lwPath));
                    std::wstring lwDir = libDir + L"\\" + lwp.parent_path().wstring();
                    if (!std::filesystem::exists(lwDir + L"\\" + lwp.filename().wstring())) {
                        postProgress(30, "下载 launchwrapper");
                        std::filesystem::create_directories(lwDir, ec);
                        bool d = DownloadFileSmart(L"https://libraries.minecraft.net/" + lxe::Utf8ToWide(lwPath), lwDir, lwp.filename().wstring(), nullptr);
                        if (!d) d = DownloadFileSmart(L"https://bmclapi2.bangbang93.com/maven/" + lxe::Utf8ToWide(lwPath), lwDir, lwp.filename().wstring(), nullptr);
                        if (!d) { fail("launchwrapper 下载失败"); return; }
                    }
                    // 4) 构造版本描述：继承原版，仅附加 OptiFine 主 jar + launchwrapper 库，tweak 入口写入游戏参数
                    Json loaderJson = Json::object();
                    Json libs = Json::array();
                    Json ofLib = Json::object();
                    ofLib["name"] = "optifine:OptiFine:" + ofCoord;
                    Json ofDl = Json::object();
                    Json ofArt = Json::object();
                    ofArt["path"] = ofPath;
                    ofArt["url"] = "https://bmclapi2.bangbang93.com/optifine/" + BmclOptifineVersionPath(mcVersion) + "/" + type + "/" + patch;
                    ofDl["artifact"] = ofArt;
                    ofLib["downloads"] = ofDl;
                    libs.asArray().push_back(ofLib);
                    Json lwLib = Json::object();
                    lwLib["name"] = "net.minecraft:launchwrapper:1.12";
                    Json lwDl = Json::object();
                    Json lwArt = Json::object();
                    lwArt["path"] = lwPath;
                    lwArt["url"] = "https://libraries.minecraft.net/" + lwPath;
                    lwDl["artifact"] = lwArt;
                    lwLib["downloads"] = lwDl;
                    libs.asArray().push_back(lwLib);
                    loaderJson["libraries"] = libs;
                    Json args = Json::object();
                    Json game = Json::array();
                    game.asArray().push_back("--tweakClass");
                    game.asArray().push_back("optifine.OptiFineTweaker");
                    args["game"] = game;
                    loaderJson["arguments"] = args;
                    postProgress(40, "写入旧版 OptiFine 版本");
                    if (!writeMerged(loaderJson, "net.minecraft.launchwrapper.Launch", "写入 OptiFine 加载器版本"))
                        { fail("写入版本 JSON 失败"); return; }
                    std::error_code ec2;
                    std::filesystem::remove(fullPath, ec2);
                    std::filesystem::remove(fullPath + L".aria2", ec2);
                    if (!finishLoader()) return;
                } else {
                    // ===== 新版本（>=1.13）：隔离环境执行安装器（设计文档 §4.1） =====
                    // 让安装器以为自己装在一个全新的官方启动器环境：隔离根 + 空白 launcher_profiles + 复制原版 json/jar
                    std::wstring isoRoot = tmpDir + L"\\ofiso_" + lxe::Utf8ToWide(installName);
                    std::wstring mvW = lxe::Utf8ToWide(mcVersion);
                    {
                        std::error_code ec;
                        std::filesystem::remove_all(isoRoot, ec);
                        std::filesystem::create_directories(isoRoot + L"\\.minecraft\\versions\\" + mvW, ec);
                        std::wstring vsrc = mcRoot + L"\\versions\\" + mvW;
                        std::filesystem::copy_file(vsrc + L"\\" + mvW + L".json",
                            isoRoot + L"\\.minecraft\\versions\\" + mvW + L"\\" + mvW + L".json",
                            std::filesystem::copy_options::overwrite_existing, ec);
                        if (std::filesystem::exists(vsrc + L"\\" + mvW + L".jar"))
                            std::filesystem::copy_file(vsrc + L"\\" + mvW + L".jar",
                                isoRoot + L"\\.minecraft\\versions\\" + mvW + L"\\" + mvW + L".jar",
                                std::filesystem::copy_options::overwrite_existing, ec);
                        std::wstring lpf = isoRoot + L"\\.minecraft\\launcher_profiles.json";
                        std::string lpfContent = "{\"profiles\":{},\"selectedProfile\":\"(Default)\",\"clientToken\":\"00000000-0000-0000-0000-000000000000\"}";
                        std::ofstream ofs(lpf, std::ios::binary);
                        if (ofs.is_open()) { ofs.write(lpfContent.data(), (std::streamsize)lpfContent.size()); ofs.close(); }
                    }
                    postProgress(15, "运行 OptiFine 安装器（隔离环境，可能需要数分钟）");
                    // 重定向全部环境影响（§4.1.2）：user.home / APPDATA / USERPROFILE / HOME 指向隔离根
                    std::wstring cmd = L"\"" + java + L"\" -Duser.home=\"" + isoRoot +
                                       L"\" -cp \"" + tmpDir + L"\\" + jarName + L"\" optifine.Installer";
                    std::vector<std::pair<std::wstring, std::wstring>> envOverrides = {
                        { L"APPDATA", isoRoot }, { L"USERPROFILE", isoRoot }, { L"HOME", isoRoot }
                    };
                    // 副作用监控（§4.1.5）：实时消费输出流防止管道阻塞，并检查异常堆栈判断失败
                    std::string installerOut;
                    int rc = RunProcessCaptureEx(cmd, isoRoot, envOverrides, installerOut);
                    if (rc != 0) {
                        // 进程封装失败回退：直接 java -jar 启动安装器重试一次（§4.1.4）
                        std::wstring cmd2 = L"\"" + java + L"\" -jar \"" + tmpDir + L"\\" + jarName + L"\"";
                        std::string out2;
                        rc = RunProcessCaptureEx(cmd2, isoRoot, envOverrides, out2);
                        if (!out2.empty()) installerOut = out2;
                        if (rc != 0) { fail("OptiFine 安装器执行失败（退出码 " + std::to_string(rc) + "）"); return; }
                    }
                    if (!installerOut.empty() &&
                        (installerOut.find("Exception in thread") != std::string::npos ||
                         installerOut.find("java.lang.") != std::string::npos ||
                         installerOut.find("Caused by:") != std::string::npos)) {
                        fail("OptiFine 安装器运行出错（输出包含异常堆栈）"); return;
                    }
                    postProgress(55, "读取安装结果");
                    std::string genId = mcVersion + "-OptiFine_" + type + "_" + patch;
                    std::wstring isoGenDir = isoRoot + L"\\.minecraft\\versions\\" + lxe::Utf8ToWide(genId);
                    auto [gt, gok] = ReadFileUtf8(isoGenDir + L"\\" + lxe::Utf8ToWide(genId) + L".json");
                    if (!gok || gt.empty()) { fail("未找到 OptiFine 生成的版本 JSON"); return; }
                    Json genJson;
                    try { genJson = Json::parse(gt); } catch (...) { fail("OptiFine 生成 JSON 解析失败"); return; }
                    // 解析安装器生成的 OptiFine 主库坐标（optifine:OptiFine:<mc>_<type>_<patch>），用于校验产物真实生效
                    std::string ofCoord;
                    if (genJson.contains("libraries") && genJson.at("libraries").isArray()) {
                        for (const auto& l : genJson.at("libraries").asArray()) {
                            if (!l.isObject() || !l.contains("name") || !l.at("name").isString()) continue;
                            std::string nm = l.at("name").asString();
                            if (nm.rfind("optifine:OptiFine:", 0) == 0) { ofCoord = nm.substr(17); break; }
                        }
                    }
                    // 产物回收（§4.1.6）：把隔离目录生成的 OptiFine jar 并入真实 libraries（生成 json 引用的本地坐标库）
                    std::wstring isoLibs = isoRoot + L"\\.minecraft\\libraries";
                    if (std::filesystem::is_directory(isoLibs)) {
                        std::error_code ec;
                        std::filesystem::copy(isoLibs, libDir, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, ec);
                    }
                    // 校验：生成的版本必须携带 OptiFine 主库，且回收后真实库目录存在 OptiFine 主 jar，
                    // 否则视为安装器未真正完成安装，直接报错而不是“假成功”。
                    if (ofCoord.empty()) { fail("OptiFine 生成的版本 JSON 缺少 OptiFine 主库"); return; }
                    {
                        std::string rel = MavenCoordToPath("optifine:OptiFine:" + ofCoord);
                        std::wstring relW = lxe::Utf8ToWide(rel);
                        std::replace(relW.begin(), relW.end(), L'/', L'\\');
                        std::error_code ec;
                        if (rel.empty() || !std::filesystem::exists(libDir + L"\\" + relW, ec)) {
                            fail("OptiFine 安装未生效（缺少 OptiFine 主 jar，安装器可能未完成安装）"); return;
                        }
                    }
                    // 解析/补齐启动关键信息：OptiFine 以 launchwrapper tweak 方式启动，
                    // mainClass 缺失时默认 launchwrapper 入口，tweakClass 缺失时手工补齐，
                    // 避免合并后仍以原版 mainClass 启动（看起来“成功”但实际还是原版）。
                    std::string mainClass;
                    if (genJson.contains("mainClass") && genJson.at("mainClass").isString())
                        mainClass = genJson.at("mainClass").asString();
                    if (mainClass.empty()) mainClass = "net.minecraft.launchwrapper.Launch";
                    {
                        bool hasTweak = false;
                        if (genJson.contains("arguments") && genJson.at("arguments").isObject() &&
                            genJson.at("arguments").contains("game") && genJson.at("arguments").at("game").isArray()) {
                            const auto& garr = genJson.at("arguments").at("game").asArray();
                            for (size_t i = 0; i + 1 < garr.size(); ++i) {
                                if (garr[i].isString() && garr[i].asString() == "--tweakClass" &&
                                    garr[i + 1].isString() && garr[i + 1].asString() == "optifine.OptiFineTweaker") { hasTweak = true; break; }
                            }
                        }
                        if (!hasTweak) {
                            if (!genJson.contains("arguments") || !genJson.at("arguments").isObject()) genJson["arguments"] = Json::object();
                            if (!genJson.at("arguments").contains("game") || !genJson.at("arguments").at("game").isArray())
                                genJson["arguments"]["game"] = Json::array();
                            genJson["arguments"]["game"].asArray().push_back("--tweakClass");
                            genJson["arguments"]["game"].asArray().push_back("optifine.OptiFineTweaker");
                        }
                    }
                    if (!writeMerged(genJson, mainClass, "写入 OptiFine 加载器版本"))
                        { fail("写入版本 JSON 失败"); return; }
                    // 清理隔离目录与安装包
                    {
                        std::error_code ec;
                        std::filesystem::remove_all(isoRoot, ec);
                        std::filesystem::remove(fullPath, ec);
                        std::filesystem::remove(fullPath + L".aria2", ec);
                    }
                    if (!finishLoader()) return;
                }
            } else if (loaderId == "liteloader") {
                // ===== LiteLoader：元数据直写，无需运行安装器（文档 §3.3） =====
                // 手工构造继承自原版的版本 JSON，仅附加加载器类路径（launchwrapper + liteloader jar + tweakClass）
                postProgress(5, "准备 LiteLoader 元数据");
                std::string tweakClass = loaderData.contains("tweakClass") && loaderData.at("tweakClass").isString()
                    ? loaderData.at("tweakClass").asString() : "com.mumfrey.liteloader.launch.LiteLoaderTweaker";
                std::string file = loaderData.contains("file") && loaderData.at("file").isString()
                    ? loaderData.at("file").asString() : "";

                // 下载 liteloader jar 到 libraries/com/mumfrey/liteloader/liteloader/{ver}/liteloader-{ver}.jar
                // （版本 json 中以该 maven 坐标引用，classpath 可被 LibPathFromName 推导）
                std::string jarVersion = loaderVersion.empty() ? mcVersion : loaderVersion;
                std::string artPath = "com/mumfrey/liteloader/liteloader/" + jarVersion + "/liteloader-" + jarVersion + ".jar";
                std::filesystem::path lp(lxe::Utf8ToWide(artPath));
                std::wstring outDir = libDir + L"\\" + lp.parent_path().wstring();
                std::wstring outName = lp.filename().wstring();
                std::error_code ec;
                std::filesystem::create_directories(outDir, ec);
                if (!std::filesystem::exists(outDir + L"\\" + outName)) {
                    // 官方源：dl.liteloader.com/versions/files/{file}；镜像：BMCLAPI liteloader download
                    std::string url = "https://dl.liteloader.com/versions/files/" + file;
                    auto cb = [&](const Aria2Progress& p) -> bool {
                        int mapped = 5 + p.percent * 40 / 100;
                        std::ostringstream ss; ss << p.speed;
                        postProgress(mapped, "下载 LiteLoader 启动库", ss.str(), p.eta);
                        return true;
                    };
                    bool dlok = DownloadFileSmart(lxe::Utf8ToWide(url), outDir, outName, cb);
                    if (!dlok) {
                        // BMCLAPI：/maven/com/mumfrey/liteloader/{file} 需要真实文件名；按官方路径回退目录下载
                        std::wstring fnW = lxe::Utf8ToWide(file);
                        size_t slash = fnW.find_last_of(L'/');
                        std::wstring tail = (slash == std::wstring::npos) ? fnW : fnW.substr(slash + 1);
                        bool dlok2 = DownloadFileSmart(L"https://dl.liteloader.com/versions/files/" + lxe::Utf8ToWide(file),
                                                       outDir, tail, cb);
                        if (dlok2) {
                            // 重命名为坐标期望的文件名
                            std::filesystem::rename(outDir + L"\\" + tail, outDir + L"\\" + outName, ec);
                        } else {
                            fail("LiteLoader jar 下载失败");
                            return;
                        }
                    }
                }

                // 下载 launchwrapper（liteloader 在 1.5~1.12 依赖它）
                std::string lwPath = "net/minecraft/launchwrapper/1.12/launchwrapper-1.12.jar";
                std::filesystem::path lwp(lxe::Utf8ToWide(lwPath));
                std::wstring lwDir = libDir + L"\\" + lwp.parent_path().wstring();
                std::error_code lec;
                std::filesystem::create_directories(lwDir, lec);
                if (!std::filesystem::exists(lwDir + L"\\" + lwp.filename().wstring())) {
                    auto cb = [&](const Aria2Progress& p) -> bool {
                        int mapped = 45 + p.percent * 10 / 100;
                        std::ostringstream ss; ss << p.speed;
                        postProgress(mapped, "下载 launchwrapper", ss.str(), p.eta);
                        return true;
                    };
                    bool dlok = DownloadFileSmart(L"https://libraries.minecraft.net/" + lxe::Utf8ToWide(lwPath),
                                                  lwDir, lwp.filename().wstring(), cb);
                    if (!dlok)
                        dlok = DownloadFileSmart(L"https://bmclapi2.bangbang93.com/maven/" + lxe::Utf8ToWide(lwPath),
                                                 lwDir, lwp.filename().wstring(), cb);
                    if (!dlok) { fail("launchwrapper 下载失败"); return; }
                }

                // 构造加载器 json：仅附加类路径（libraries + tweakClass 游戏参数）
                Json loaderJson = Json::object();
                Json libs = Json::array();
                Json lwLib = Json::object();
                lwLib["name"] = "net.minecraft:launchwrapper:1.12";
                Json lwDl = Json::object();
                Json lwArt = Json::object();
                lwArt["path"] = lwPath;
                lwArt["url"] = "https://libraries.minecraft.net/" + lwPath;
                lwDl["artifact"] = lwArt;
                lwLib["downloads"] = lwDl;
                libs.asArray().push_back(lwLib);
                Json llLib = Json::object();
                llLib["name"] = "com.mumfrey.liteloader:liteloader:" + jarVersion;
                Json llDl = Json::object();
                Json llArt = Json::object();
                llArt["path"] = artPath;
                llArt["url"] = "https://dl.liteloader.com/versions/files/" + file;
                llDl["artifact"] = llArt;
                llLib["downloads"] = llDl;
                libs.asArray().push_back(llLib);
                loaderJson["libraries"] = libs;
                Json args = Json::object();
                Json game = Json::array();
                game.asArray().push_back("--tweakClass");
                game.asArray().push_back(tweakClass);
                args["game"] = game;
                loaderJson["arguments"] = args;
                postProgress(55, "写入 LiteLoader 加载器版本");
                if (!writeMerged(loaderJson, "net.minecraft.launchwrapper.Launch", "写入 LiteLoader 加载器版本"))
                    { fail("写入版本 JSON 失败"); return; }
                if (!finishLoader()) return;
            } else if (loaderId == "neoforge") {
                // ===== NeoForge：官方安装器（新版/旧版坐标）经 --installClient 安装，合并产物到 installName =====
                // 版本名规则：旧版坐标 net/neoforged/forge（1.20.1~1.20.3，如 1.20.1-47.1.106），
                // 新版坐标 net/neoforged/neoforge（20.4+，如 20.4.237-beta）
                std::string artifact = loaderVersion;
                bool oldCoord = artifact.find("1.") == 0 && artifact.find("-") != std::string::npos;
                std::string coordPath = oldCoord ? "net/neoforged/forge" : "net/neoforged/neoforge";
                std::string jarBase = oldCoord ? "forge" : "neoforge";
                std::string jarUrl = "https://maven.neoforged.net/releases/" + coordPath + "/" + artifact + "/" +
                                     jarBase + "-" + artifact + "-installer.jar";
                std::string mirrorUrl = "https://bmclapi2.bangbang93.com/maven/" + coordPath + "/" + artifact + "/" +
                                        jarBase + "-" + artifact + "-installer.jar";

                postProgress(3, "下载 NeoForge 安装器");
                std::wstring tmpDir = mcRoot + L"\\temp";
                std::wstring jarName = L"neoforge-" + lxe::Utf8ToWide(artifact) + L"-installer.jar";
                std::wstring installerJar = tmpDir + L"\\" + jarName;
                {
                    auto cb = [&](const Aria2Progress& p) -> bool {
                        int mapped = 3 + p.percent * 9 / 100;
                        std::ostringstream ss; ss << p.speed;
                        postProgress(mapped, "下载 NeoForge 安装器", ss.str(), p.eta);
                        return true;
                    };
                    if (!DownloadFileSmart(lxe::Utf8ToWide(jarUrl), tmpDir, jarName, cb)) {
                        if (!DownloadFileSmart(lxe::Utf8ToWide(mirrorUrl), tmpDir, jarName, cb)) {
                            fail("NeoForge 安装器下载失败");
                            return;
                        }
                    }
                }

                postProgress(12, "选择 Java 运行环境");
                int recMajor = RecommendedJavaMajor(mcVersion);
                auto javas0 = ScanInstalledJavas();
                bool recFound0 = false;
                for (const auto& j : javas0) if (j.major == recMajor) { recFound0 = true; break; }
                std::wstring java = PickJavaForMajor(javas0, recMajor);
                if (java.empty()) java = FindJavaPath();
                if (java.empty()) { fail("未检测到 Java：安装 NeoForge 需要 Java 运行环境，请先在下载中心安装 Java 或指定 Java 路径"); return; }
                if (!recFound0)
                    postProgress(12, "提示：未匹配到推荐 Java " + std::to_string(recMajor) + "，将使用现有 Java 继续（仅提示，非强制）");

                if (!ensureVanillaBase()) { fail("无法准备原版版本（安装器需要）"); return; }
                std::wstring vanillaJar = mcRoot + L"\\versions\\" + lxe::Utf8ToWide(mcVersion) +
                                          L"\\" + lxe::Utf8ToWide(mcVersion) + L".jar";

                // ===== 解包安装器 + 手动安装：install_profile.json → 依赖库 → client.lzma → installertools 执行 processors =====
                // 参照 temp/install_profile.json（NeoForge 26.2.0.0-beta）：
                //   data.BINPATCH.client = "/data/client.lzma"（安装器内 zip 条目，--apply-patches 输入）
                //   data.PATCHED.client  = "[net.neoforged:minecraft-client-patched:...]"（PROCESS_MINECRAFT_JAR 输出坐标）
                //   processors ① sides:[server] EXTRACT_FILES（客户端跳过）② PROCESS_MINECRAFT_JAR
                std::wstring extractDir = tmpDir + L"\\neoforge_" + lxe::Utf8ToWide(artifact);
                {
                    std::error_code ec;
                    std::filesystem::remove_all(extractDir, ec);
                    std::filesystem::create_directories(extractDir, ec);
                }
                Json installProfile;
                bool haveProfile = false;
                if (ExtractZipEntry(installerJar, "install_profile.json", extractDir)) {
                    auto [pt, pok] = ReadFileUtf8(extractDir + L"\\install_profile.json");
                    if (pok && !pt.empty()) { try { installProfile = Json::parse(pt); if (installProfile.isObject()) haveProfile = true; } catch (...) {} }
                }
                if (!haveProfile) { fail("未找到 NeoForge 安装器配置（install_profile.json）"); return; }

                // 1) 下载 install_profile.json 的 libraries（processor 依赖 + 运行库），url 缺省走镜像/内嵌 maven
                postProgress(14, "下载 NeoForge 依赖库");
                if (installProfile.contains("libraries") && installProfile.at("libraries").isArray()) {
                    const auto& plibs = installProfile.at("libraries").asArray();
                    int n = 0;
                    for (const auto& lib : plibs) {
                        if (!lib.isObject() || !lib.contains("name") || !lib.at("name").isString()) continue;
                        // 跳过仅 server 侧需要的库
                        if (lib.contains("serverreq") && lib.at("serverreq").isBool() && lib.at("serverreq").asBool() &&
                            !(lib.contains("clientreq") && lib.at("clientreq").isBool() && lib.at("clientreq").asBool())) continue;
                        if (lib.contains("clientreq") && lib.at("clientreq").isBool() && !lib.at("clientreq").asBool()) continue;
                        std::string coord = lib.at("name").asString();
                        std::string path = MavenCoordToPath(coord);
                        if (path.empty()) continue;
                        std::wstring lp(lxe::Utf8ToWide(path));
                        std::wstring outDir = libDir + L"\\" + std::filesystem::path(lp).parent_path().wstring();
                        std::wstring outName = std::filesystem::path(lp).filename().wstring();
                        if (std::filesystem::exists(outDir + L"\\" + outName)) continue;
                        std::string url;
                        if (lib.contains("downloads") && lib.at("downloads").isObject() &&
                            lib.at("downloads").contains("artifact") && lib.at("downloads").at("artifact").isObject() &&
                            lib.at("downloads").at("artifact").contains("url") &&
                            lib.at("downloads").at("artifact").at("url").isString())
                            url = lib.at("downloads").at("artifact").at("url").asString();
                        else if (lib.contains("url") && lib.at("url").isString()) {
                            std::string base = lib.at("url").asString();
                            while (!base.empty() && base.back() == '/') base.pop_back();
                            url = base + "/" + path;
                        } else {
                            url = "https://maven.neoforged.net/releases/" + path;
                        }
                        int mapped = 14 + 16 * (n++) / std::max<int>(1, (int)plibs.size());
                        auto cb = [&](const Aria2Progress& p) -> bool {
                            int m2 = mapped + p.percent * 3 / 100;
                            std::ostringstream ss; ss << p.speed;
                            postProgress(m2, "下载 NeoForge 依赖库 " + path, ss.str(), p.eta);
                            return true;
                        };
                        std::wstring embedRoot = tmpDir + L"\\neoforge_embed";
                        bool dlok = DownloadFileSmart(lxe::Utf8ToWide(url), outDir, outName, cb);
                        if (!dlok) dlok = DownloadFileSmart(lxe::Utf8ToWide(BmclMavenMirror(url)), outDir, outName, cb);
                        if (!dlok) dlok = CopyEmbeddedMavenLib(installerJar, path, libDir, embedRoot);
                        if (!dlok) { fail("NeoForge 依赖库下载失败：" + path); return; }
                    }
                }

                // 1.5) 嵌入 maven 启动库：{jarBase}-{artifact}.jar（无分类器，version.json 引用且 url 为空）
                CopyEmbeddedMavenLib(installerJar, coordPath + "/" + artifact + "/" + jarBase + "-" + artifact + ".jar",
                                     libDir, tmpDir + L"\\neoforge_embed");

                // 1.6) 解出 version.json 并补下启动依赖库（部分库在版本 json 中 url 为空）
                Json forgeJson;
                bool haveForgeJson = false;
                {
                    std::wstring vjPath = extractDir + L"\\version.json";
                    if (!std::filesystem::exists(vjPath))
                        ExtractZipEntry(installerJar, "version.json", extractDir);
                    auto [vjt, vjok] = ReadFileUtf8(vjPath);
                    if (vjok && !vjt.empty()) { try { forgeJson = Json::parse(vjt); if (forgeJson.isObject()) haveForgeJson = true; } catch (...) {} }
                    if (haveForgeJson && forgeJson.contains("libraries") && forgeJson.at("libraries").isArray()) {
                        std::wstring embedRoot2 = tmpDir + L"\\neoforge_embed2";
                        const auto& vls = forgeJson.at("libraries").asArray();
                        int k = 0;
                        for (const auto& lib : vls) {
                            if (!lib.isObject() || !lib.contains("name") || !lib.at("name").isString()) continue;
                            if (lib.contains("serverreq") && lib.at("serverreq").isBool() && lib.at("serverreq").asBool() &&
                                !(lib.contains("clientreq") && lib.at("clientreq").isBool() && lib.at("clientreq").asBool())) continue;
                            if (lib.contains("clientreq") && lib.at("clientreq").isBool() && !lib.at("clientreq").asBool()) continue;
                            std::string coord2 = lib.at("name").asString();
                            std::string path2 = MavenCoordToPath(coord2);
                            if (path2.empty()) continue;
                            // 已由原版 json 自带 → 启动时按原版 URL 补下，跳过
                            bool inVanilla = false;
                            if (vanillaJson.contains("libraries") && vanillaJson.at("libraries").isArray()) {
                                for (const auto& vll : vanillaJson.at("libraries").asArray()) {
                                    if (vll.isObject() && vll.contains("name") && vll.at("name").isString() &&
                                        vll.at("name").asString() == coord2) { inVanilla = true; break; }
                                }
                            }
                            if (inVanilla) continue;
                            std::wstring lp2(lxe::Utf8ToWide(path2));
                            std::wstring outDir2 = libDir + L"\\" + std::filesystem::path(lp2).parent_path().wstring();
                            std::wstring outName2 = std::filesystem::path(lp2).filename().wstring();
                            if (std::filesystem::exists(outDir2 + L"\\" + outName2)) continue;
                            std::string vurl2;
                            if (lib.contains("downloads") && lib.at("downloads").isObject() &&
                                lib.at("downloads").contains("artifact") && lib.at("downloads").at("artifact").isObject() &&
                                lib.at("downloads").at("artifact").contains("url") &&
                                lib.at("downloads").at("artifact").at("url").isString())
                                vurl2 = lib.at("downloads").at("artifact").at("url").asString();
                            std::string fullUrl2 = vurl2.empty() ? "https://maven.neoforged.net/releases/" + path2 : vurl2;
                            int m6 = 32 + 3 * (k++) / std::max<int>(1, (int)vls.size());
                            auto cb6 = [&](const Aria2Progress& p) -> bool {
                                int m7 = m6 + p.percent * 2 / 100;
                                std::ostringstream ss; ss << p.speed;
                                postProgress(m7, "下载 NeoForge 启动库 " + path2, ss.str(), p.eta);
                                return true;
                            };
                            bool d6 = DownloadFileSmart(lxe::Utf8ToWide(fullUrl2), outDir2, outName2, cb6);
                            if (!d6) d6 = DownloadFileSmart(lxe::Utf8ToWide(BmclMavenMirror(fullUrl2)), outDir2, outName2, cb6);
                            if (!d6) d6 = CopyEmbeddedMavenLib(installerJar, path2, libDir, embedRoot2);
                            if (!d6) { fail("NeoForge 启动库下载失败：" + path2); return; }
                        }
                    }
                }

                // 2) 提取 data/client.lzma 作为 BINPATCH（install_profile.data.BINPATCH.client = "/data/client.lzma"）
                postProgress(35, "提取 NeoForge 补丁文件");
                std::wstring lzmaDir = tmpDir + L"\\neoforge_lzma";
                std::wstring binpatchPath = lzmaDir + L"\\data\\client.lzma";
                {
                    std::error_code lec;
                    std::filesystem::remove_all(lzmaDir, lec);
                    if (!ExtractZipEntry(installerJar, "data/client.lzma", lzmaDir) || !std::filesystem::exists(binpatchPath)) {
                        fail("NeoForge 补丁文件提取失败"); return;
                    }
                }

                // 3) 依次执行 client 侧 processors（跳过 sides 仅含 server 的处理器，如 EXTRACT_FILES）
                postProgress(38, "运行 NeoForge 处理器（可能需要数分钟）");
                auto resolveForgeArg = [&](const std::string& token) -> std::wstring {
                    std::string res = token;
                    for (;;) {
                        size_t b = res.find('{');
                        if (b == std::string::npos) break;
                        size_t e = res.find('}', b + 1);
                        if (e == std::string::npos) break;
                        std::string key = res.substr(b + 1, e - b - 1);
                        std::wstring val;
                        if (key == "MINECRAFT_JAR") val = vanillaJar;
                        else if (key == "INSTALLER") val = installerJar;
                        else if (key == "SIDE") val = L"client";
                        else if (key == "ROOT") val = mcRoot;
                        else if (key == "BINPATCH") val = binpatchPath;
                        else if (installProfile.contains("data") && installProfile.at("data").isObject()) {
                            const auto& data = installProfile.at("data");
                            if (data.contains(key) && data.at(key).isObject()) {
                                const auto& dk = data.at(key);
                                std::string v;
                                if (dk.contains("client") && dk.at("client").isString()) v = dk.at("client").asString();
                                bool bracket = v.size() >= 2 && v.front() == '[' && v.back() == ']';
                                if (bracket) v = v.substr(1, v.size() - 2);
                                if (bracket) val = ForgeLibFullPath(libDir, v);
                                else val = lxe::Utf8ToWide(v);
                            }
                        }
                        res = res.substr(0, b) + (val.empty() ? "" : lxe::WideToUtf8(val)) + res.substr(e + 1);
                    }
                    if (!res.empty() && res.front() == '[' && res.back() == ']')
                        return ForgeLibFullPath(libDir, res.substr(1, res.size() - 2));
                    return lxe::Utf8ToWide(res);
                };
                int procCount = 0;
                if (installProfile.contains("processors") && installProfile.at("processors").isArray()) {
                    for (const auto& proc : installProfile.at("processors").asArray()) {
                        if (!proc.isObject()) continue;
                        bool doClient = true;
                        if (proc.contains("sides") && proc.at("sides").isArray()) {
                            doClient = false;
                            for (const auto& s : proc.at("sides").asArray())
                                if (s.isString() && s.asString() == "client") { doClient = true; break; }
                        }
                        if (!doClient) continue;
                        if (!proc.contains("jar") || !proc.at("jar").isString()) continue;
                        std::string pjar = proc.at("jar").asString();
                        std::wstring pjarPath = ForgeLibFullPath(libDir, pjar);
                        if (pjarPath.empty() || !std::filesystem::exists(pjarPath)) { fail("NeoForge 处理器缺失：" + pjar); return; }
                        // installertools fatjar 的 Manifest 无 Main-Class，固定为 ConsoleTool
                        std::wstring mainClass = lxe::Utf8ToWide(ReadJarMainClass(pjarPath));
                        if (mainClass.empty()) mainClass = L"net.neoforged.installertools.ConsoleTool";
                        std::vector<std::wstring> cps;
                        cps.push_back(pjarPath);
                        if (proc.contains("classpath") && proc.at("classpath").isArray()) {
                            for (const auto& c : proc.at("classpath").asArray()) {
                                if (!c.isString()) continue;
                                std::wstring cp = ForgeLibFullPath(libDir, c.asString());
                                if (!cp.empty()) cps.push_back(cp);
                            }
                        }
                        std::wstring cpAll;
                        for (size_t i = 0; i < cps.size(); ++i) { if (i) cpAll += L";"; cpAll += cps[i]; }
                        std::wstring argLine;
                        if (proc.contains("args") && proc.at("args").isArray()) {
                            for (const auto& a : proc.at("args").asArray()) {
                                if (!a.isString()) continue;
                                std::wstring av = resolveForgeArg(a.asString());
                                if (av.empty()) continue;
                                if (!argLine.empty()) argLine += L" ";
                                if (av.find(L' ') != std::wstring::npos || av.find(L'\t') != std::wstring::npos)
                                    argLine += L"\"" + av + L"\"";
                                else argLine += av;
                            }
                        }
                        std::wstring cmd = L"\"" + java + L"\" -cp \"" + cpAll + L"\" " + mainClass + L" " + argLine;
                        int rc = RunProcessSilent(cmd, mcRoot);
                        if (rc != 0) { fail("NeoForge 处理器执行失败：" + pjar + "（退出码 " + std::to_string(rc) + "）"); return; }
                        ++procCount;
                    }
                }
                if (procCount == 0) { fail("未找到需要执行的 NeoForge 处理器"); return; }

                // 4) 读取安装器内 version.json 并合并到 installName
                postProgress(90, "读取 NeoForge 版本元数据");
                if (!haveForgeJson && installProfile.contains("versionInfo") && installProfile.at("versionInfo").isObject())
                    { forgeJson = installProfile.at("versionInfo"); haveForgeJson = true; }
                if (!haveForgeJson) { fail("未找到 NeoForge 版本元数据（version.json）"); return; }

                std::string mainClass;
                if (forgeJson.contains("mainClass") && forgeJson.at("mainClass").isString())
                    mainClass = forgeJson.at("mainClass").asString();
                if (!writeMerged(forgeJson, mainClass, "写入 NeoForge 加载器版本"))
                    { fail("写入版本 JSON 失败"); return; }

                // 5) 清理临时文件
                {
                    std::error_code ec;
                    std::filesystem::remove_all(extractDir, ec);
                    std::filesystem::remove_all(lzmaDir, ec);
                    std::filesystem::remove_all(tmpDir + L"\\neoforge_embed", ec);
                    std::filesystem::remove_all(tmpDir + L"\\neoforge_embed2", ec);
                    std::filesystem::remove(installerJar, ec);
                }
                if (!finishLoader()) return;
            } else {
                fail("不支持的加载器：" + loaderId);
                return;
            }
            } catch (const std::exception& e) {
                fail(std::string("安装过程异常：") + e.what());
            } catch (...) {
                fail("安装过程未知异常");
            }
    };

    // mc.installLoader：后台线程执行加载器安装（保持原有异步/进度事件语义）
    bridge.Register("mc.installLoader", [&bridge](const Json& params) {
        std::string loaderId, mcVersion, loaderVersion, installName;
        if (params.isObject() && params.contains("loaderId") && params.at("loaderId").isString())
            loaderId = params.at("loaderId").asString();
        if (params.isObject() && params.contains("mcVersion") && params.at("mcVersion").isString())
            mcVersion = params.at("mcVersion").asString();
        if (params.isObject() && params.contains("loaderVersion") && params.at("loaderVersion").isString())
            loaderVersion = params.at("loaderVersion").asString();
        if (params.isObject() && params.contains("installName") && params.at("installName").isString())
            installName = params.at("installName").asString();
        if (loaderId.empty() || mcVersion.empty() || loaderVersion.empty() || installName.empty())
            return Err(-32602, "missing loaderId/mcVersion/loaderVersion/installName");

        bool complete = false;
        if (params.isObject() && params.contains("complete") && params.at("complete").isBool())
            complete = params.at("complete").asBool();

        Json loaderData = Json::object();
        if (params.isObject() && params.contains("loaderData") && params.at("loaderData").isObject())
            loaderData = params.at("loaderData");

        static std::atomic<int> loaderTaskSeq{20000};
        int taskId = ++loaderTaskSeq;
        Json result = Json::object();
        result["taskId"] = std::to_string(taskId);
        result["started"] = true;
        result["loaderId"] = loaderId;

        std::thread([&bridge, taskId, loaderId, mcVersion, loaderVersion, installName, loaderData, complete]() {
            g_installLoaderImpl(bridge, taskId, loaderId, mcVersion, loaderVersion, installName, loaderData, complete);
        }).detach();

        return Ok(result);
    });
}

// ============ 插件系统（前端契约 LXElauncher插件规范 V1.0）============
// 插件根目录：exe 同级 plugins/<插件目录>/manifest.json（懒创建）。
// apiVersion 为前端插件契约版本，须严格相等；kernelVersion 为后端 RPC 版本（semver 范围）。
// 后端只负责发现/状态/读 main 源码，插件代码由前端 webview 执行（mainSource 惰性下发）。
static const char* kPluginApiVersion = "5.0";
static const char* kPluginKernelVersion = "2.1.0";

// 标准插槽全集（与前端 PLUGIN_SLOTS 保持一致；LX.HOST.slots 下发）
static const char* kPluginSlots[] = {
    "navbar:left", "navbar:right", "navbar:center",
    "sidebar:top", "sidebar:bottom",
    "home:before", "home:after", "home:widgets",
    "downloads:toolbar", "downloads:list:before", "downloads:list:after",
    "settings:before", "settings:after", "settings:section:launch",
    "settings:section:graphics", "settings:section:controls", "settings:section:network",
    "settings:section:account",
    "dialog:crash:body", "dialog:crash:footer", "dialog:launch:body",
};

static std::wstring PluginsRootDir() {
    std::wstring root = ExeSiblingPath(L"plugins");
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    return root;
}

// plugins 启用状态持久化在 settings.json 的 pluginStates：{ key: bool }，缺省视为启用
static bool PluginEnabledState(const std::string& key) {
    Json s = LoadSettingsFile();
    if (s.isObject() && s.contains("pluginStates") && s.at("pluginStates").isObject()) {
        const Json& st = s.at("pluginStates");
        if (st.contains(key) && st.at(key).isBool()) return st.at(key).asBool();
    }
    return true;
}

static std::string ManifestField(const Json& m, const char* key) {
    if (m.contains(key) && m.at(key).isString()) return m.at(key).asString();
    return std::string();
}

static bool ResolvePluginMcPath(const std::string& input, std::filesystem::path& resolved) {
    if (input.empty() || input[0] != '/' || input.find('\\') != std::string::npos || input.find('\0') != std::string::npos)
        return false;
    if (input.size() > 1 && input.find("//") != std::string::npos) return false;
    std::wstring rel = lxe::Utf8ToWide(input);
    if (rel.empty() || rel.find(L':') != std::wstring::npos) return false;
    std::wstring relPart = rel.substr(1);
    size_t start = 0;
    while (start <= relPart.size()) {
        size_t end = relPart.find(L'/', start);
        std::wstring component = relPart.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
        if (component == L".." || component == L".") return false;
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    try {
        std::error_code ec;
        const std::filesystem::path root = std::filesystem::weakly_canonical(GetMcRoot(), ec);
        if (ec) return false;
        const std::filesystem::path candidate = root / std::filesystem::path(relPart);
        const std::filesystem::path canonical = std::filesystem::weakly_canonical(candidate, ec);
        if (ec) return false;
        const std::filesystem::path relative = std::filesystem::relative(canonical, root, ec);
        if (ec) return false;
        const std::wstring check = relative.wstring();
        if (check == L".." || check.rfind(L"..\\", 0) == 0 || check.rfind(L"../", 0) == 0) return false;
        resolved = canonical;
        return true;
    } catch (...) {
        return false;
    }
}

static std::string PluginBase64Encode(const std::string& data) {
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    for (size_t i = 0; i < data.size(); i += 3) {
        const unsigned int a = static_cast<unsigned char>(data[i]);
        const unsigned int b = i + 1 < data.size() ? static_cast<unsigned char>(data[i + 1]) : 0;
        const unsigned int c = i + 2 < data.size() ? static_cast<unsigned char>(data[i + 2]) : 0;
        out.push_back(table[(a >> 2) & 0x3f]);
        out.push_back(table[((a & 0x3) << 4) | ((b >> 4) & 0xf)]);
        out.push_back(i + 1 < data.size() ? table[((b & 0xf) << 2) | ((c >> 6) & 0x3)] : '=');
        out.push_back(i + 2 < data.size() ? table[c & 0x3f] : '=');
    }
    return out;
}

static bool IsEnvKey(const std::string& key) {
    if (key.empty()) return false;
    for (unsigned char c : key) {
        if (!(std::isalnum(c) || c == '_')) return false;
    }
    return true;
}

static std::wstring QuoteProcessArg(const std::wstring& arg) {
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') {
            ++slashes;
        } else if (c == L'\"') {
            out.append(slashes * 2 + 1, L'\\');
            out.push_back(L'\"');
            slashes = 0;
        } else {
            out.append(slashes, L'\\');
            out.push_back(c);
            slashes = 0;
        }
    }
    out.append(slashes * 2, L'\\');
    out.push_back(L'\"');
    return out;
}

void RegisterPlugins(Bridge& bridge) {
    // 扫描 plugins/ 下带 manifest.json 的插件目录；启用的插件附带 mainSource 供前端执行
    bridge.Register("plugin.list", [](const Json&) {
        Json r = Json::object();
        r["pluginsDir"] = lxe::WideToUtf8(PluginsRootDir());
        Json host = Json::object();
        Json settings = LoadAppSettings();
        host["id"] = "lxe-launcher";
        host["name"] = "LXElauncher";
        host["version"] = settings.isObject() && settings.contains("version") && settings.at("version").isString()
            ? settings.at("version").asString() : "0.1.0";
        host["apiVersion"] = kPluginApiVersion;
        host["kernelVersion"] = kPluginKernelVersion;
        host["layout"] = "top-nav";
        Json layouts = Json::array(); layouts.asArray().push_back("top-nav"); layouts.asArray().push_back("left-sidebar");
        host["supportedLayouts"] = layouts;
        Json features = Json::object();
        features["nativeMenu"] = false;
        features["acrylic"] = false;
        features["multiInstance"] = true;
        features["customCss"] = true;
        features["modalStack"] = true;
        host["features"] = features;
        Json uiDefaults = Json::object();
        uiDefaults["primaryColor"] = "#3b82f6";
        uiDefaults["borderRadius"] = "10px";
        uiDefaults["fontFamily"] = "";
        uiDefaults["isDark"] = true;
        host["uiDefaults"] = uiDefaults;
        Json slots = Json::array();
        for (const char* s : kPluginSlots) slots.asArray().push_back(s);
        host["slots"] = slots;
        r["host"] = host;
        Json arr = Json::array();
        std::error_code ec;
        const std::wstring root = PluginsRootDir();
        std::filesystem::directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, ec);
        for (; !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
            if (!it->is_directory(ec) || ec) continue;
            const std::wstring dir = it->path().wstring();
            std::error_code sec;
            if (!std::filesystem::is_regular_file(dir + L"\\manifest.json", sec)) continue;
            auto [raw, sok] = ReadFileUtf8(dir + L"\\manifest.json");
            if (!sok || raw.empty()) continue;
            Json m;
            try { m = Json::parse(raw); } catch (...) { continue; }
            if (!m.isObject()) continue;
            const std::string folder = lxe::WideToUtf8(it->path().filename().wstring());
            const std::string id = ManifestField(m, "id");
            const std::string key = id.empty() ? folder : id;
            Json p = Json::object();
            p["key"] = key;
            p["folder"] = folder;
            p["path"] = lxe::WideToUtf8(dir);
            p["id"] = id.empty() ? key : id;
            p["name"] = !ManifestField(m, "name").empty() ? ManifestField(m, "name") : folder;
            p["version"] = !ManifestField(m, "version").empty() ? ManifestField(m, "version") : "0.1";
            p["description"] = ManifestField(m, "description");
            p["author"] = ManifestField(m, "author");
            p["icon"] = ManifestField(m, "icon");
            p["main"] = !ManifestField(m, "main").empty() ? ManifestField(m, "main") : "main.js";
            p["manifestApi"] = ManifestField(m, "apiVersion");
            p["manifestKernel"] = ManifestField(m, "kernelVersion");
            p["enabled"] = PluginEnabledState(key);
            if (m.contains("permissions")) p["permissions"] = m.at("permissions");
            if (m.contains("hosts")) p["hosts"] = m.at("hosts");
            if (m.contains("layouts")) p["layouts"] = m.at("layouts");
            if (m.contains("hooks")) p["hooks"] = m.at("hooks");
            if (m.contains("injections")) p["injections"] = m.at("injections");
            if (m.contains("styles")) p["styles"] = m.at("styles");
            if (m.contains("dependencies")) p["dependencies"] = m.at("dependencies");
            // 只给已启用插件下发 main 源码（限制 256KB；main 路径含 ".." 视为越权跳过）
            if (p["enabled"].asBool()) {
                const std::string main = p["main"].asString();
                if (!main.empty() && main.find("..") == std::string::npos) {
                    std::wstring mainPath = dir + L"\\" + lxe::Utf8ToWide(main);
                    auto [src, mok] = ReadFileUtf8(mainPath);
                    if (mok && src.size() <= 256 * 1024) p["mainSource"] = src;
                }
            }
            arr.asArray().push_back(p);
        }
        r["list"] = arr;
        return Ok(r);
    });

    bridge.RegisterAsync("fs.readFile", [](const Json& params, const std::function<void(HandlerResult)>& done) {
        if (!params.isObject() || !params.contains("path") || !params.at("path").isString()) {
            done(Err(-32602, "缺少参数 path")); return;
        }
        const std::string encoding = params.contains("encoding") && params.at("encoding").isString()
            ? params.at("encoding").asString() : "utf-8";
        if (encoding != "utf-8" && encoding != "base64") { done(Err(-32602, "不支持的编码")); return; }
        // 可选参数 maxBytes：限制读取字节数（正整数）。文件更大时只返回末尾 maxBytes 字节并置 truncated=true，
        // 避免整读超大日志（latest.log/debug.log 可达数十 MB）阻塞桥消息线程导致 UI 卡死（§28 教训）。
        // 未传 maxBytes 时默认 1MB：插件侧同样受此上限保护，防止在崩溃分析等场景整读大文件卡死窗口。
        size_t maxBytes = 0;
        if (params.contains("maxBytes")) {
            if (!params.at("maxBytes").isNumber()) { done(Err(-32602, "maxBytes 必须为正整数")); return; }
            const double v = params.at("maxBytes").asNumber();
            if (v <= 0 || v > 2147483647.0) { done(Err(-32602, "maxBytes 必须为正整数")); return; }
            maxBytes = static_cast<size_t>(v);
        } else {
            maxBytes = 1 * 1024 * 1024;
        }
        // 路径在桥线程解析（ResolvePluginMcPath 依赖 GetMcRoot()，避免后台线程跨线程读 g_mcRoot）
        std::filesystem::path path;
        if (!ResolvePluginMcPath(params.at("path").asString(), path)) { done(Err(-32090, "路径不安全")); return; }
        const std::wstring abs = path.wstring();
        // 文件 I/O 放后台线程，不阻塞桥消息线程（此次白屏根因：插件崩溃分析整读大日志卡死 UI）
        std::thread([abs, encoding, maxBytes, done]() {
            std::string raw;
            bool truncated = false;
            try {
                std::ifstream file(abs, std::ios::binary);
                if (!file) { done(Err(-32000, "文件不存在或无法读取")); return; }
                std::error_code ec;
                const auto fileSize = std::filesystem::file_size(abs, ec);
                if (ec) { done(Err(-32000, "无法获取文件大小")); return; }
                truncated = fileSize > maxBytes;
                if (!truncated) {
                    // 文件不超限：复用既有读取路径（含 UTF-8 BOM 处理）
                    if (encoding == "utf-8") {
                        auto full = ReadFileUtf8(abs);
                        if (!full.second) { done(Err(-32000, "文件不存在或无法读取")); return; }
                        raw = std::move(full.first);
                    } else {
                        std::stringstream data;
                        data << file.rdbuf();
                        raw = data.str();
                    }
                } else {
                    // 超限：只读末尾 maxBytes 字节（日志错误信息通常在尾部）
                    if (!file.seekg(-static_cast<std::streamoff>(maxBytes), std::ios::end)) {
                        done(Err(-32000, "文件读取失败")); return;
                    }
                    std::string tail(maxBytes, '\0');
                    if (!file.read(&tail[0], static_cast<std::streamsize>(maxBytes))) {
                        done(Err(-32000, "文件读取失败")); return;
                    }
                    raw = std::move(tail);
                }
            } catch (const std::exception& e) {
                done(Err(-32000, std::string("读取异常：") + e.what())); return;
            } catch (...) {
                done(Err(-32000, "读取未知异常")); return;
            }
            Json out = Json::object();
            out["content"] = encoding == "base64" ? PluginBase64Encode(raw) : raw;
            if (truncated) out["truncated"] = true;
            done(Ok(out));
        }).detach();
    });

    bridge.RegisterAsync("fs.writeFile", [](const Json& params, const std::function<void(HandlerResult)>& done) {
        if (!params.isObject() || !params.contains("path") || !params.at("path").isString() ||
            !params.contains("content") || !params.at("content").isString()) {
            done(Err(-32602, "缺少参数 path/content")); return;
        }
        const std::string encoding = params.contains("encoding") && params.at("encoding").isString()
            ? params.at("encoding").asString() : "utf-8";
        if (encoding != "utf-8") { done(Err(-32602, "不支持的编码")); return; }
        std::filesystem::path path;
        if (!ResolvePluginMcPath(params.at("path").asString(), path)) { done(Err(-32090, "路径不安全")); return; }
        const std::wstring abs = path.wstring();
        const std::string content = params.at("content").asString();
        std::thread([abs, content, done]() {
            try {
                std::error_code ec;
                std::filesystem::create_directories(std::filesystem::path(abs).parent_path(), ec);
                if (ec) { done(Err(-32000, "无法创建父目录")); return; }
                std::ofstream file(abs, std::ios::binary | std::ios::trunc);
                if (!file) { done(Err(-32000, "文件无法写入")); return; }
                file.write(content.data(), static_cast<std::streamsize>(content.size()));
                if (!file) { done(Err(-32000, "文件写入失败")); return; }
                Json out = Json::object(); out["ok"] = true; done(Ok(out));
            } catch (const std::exception& e) {
                done(Err(-32000, std::string("写入异常：") + e.what()));
            } catch (...) {
                done(Err(-32000, "写入未知异常"));
            }
        }).detach();
    });

    bridge.RegisterAsync("fs.readDir", [](const Json& params, const std::function<void(HandlerResult)>& done) {
        if (!params.isObject() || !params.contains("path") || !params.at("path").isString()) {
            done(Err(-32602, "缺少参数 path")); return;
        }
        std::filesystem::path path;
        if (!ResolvePluginMcPath(params.at("path").asString(), path)) { done(Err(-32090, "路径不安全")); return; }
        const std::wstring abs = path.wstring();
        std::thread([abs, done]() {
            try {
                std::error_code ec;
                if (!std::filesystem::is_directory(abs, ec) || ec) { done(Err(-32000, "目录不存在或无法读取")); return; }
                Json out = Json::object();
                out["entries"] = Json::array();
                for (std::filesystem::directory_iterator it(abs, std::filesystem::directory_options::skip_permission_denied, ec);
                     !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
                    out["entries"].asArray().push_back(lxe::WideToUtf8(it->path().filename().wstring()));
                }
                if (ec) { done(Err(-32000, "目录读取失败")); return; }
                done(Ok(out));
            } catch (const std::exception& e) {
                done(Err(-32000, std::string("读取目录异常：") + e.what()));
            } catch (...) {
                done(Err(-32000, "读取目录未知异常"));
            }
        }).detach();
    });

    bridge.RegisterAsync("fs.exists", [](const Json& params, const std::function<void(HandlerResult)>& done) {
        if (!params.isObject() || !params.contains("path") || !params.at("path").isString()) {
            done(Err(-32602, "缺少参数 path")); return;
        }
        std::filesystem::path path;
        if (!ResolvePluginMcPath(params.at("path").asString(), path)) { done(Err(-32090, "路径不安全")); return; }
        const std::wstring abs = path.wstring();
        std::thread([abs, done]() {
            try {
                std::error_code ec;
                const bool exists = std::filesystem::exists(abs, ec);
                if (ec) { done(Err(-32000, "无法检查路径")); return; }
                Json out = Json::object(); out["exists"] = exists; done(Ok(out));
            } catch (const std::exception& e) {
                done(Err(-32000, std::string("检查路径异常：") + e.what()));
            } catch (...) {
                done(Err(-32000, "检查路径未知异常"));
            }
        }).detach();
    });

    bridge.RegisterAsync("fs.mkdir", [](const Json& params, const std::function<void(HandlerResult)>& done) {
        if (!params.isObject() || !params.contains("path") || !params.at("path").isString()) {
            done(Err(-32602, "缺少参数 path")); return;
        }
        std::filesystem::path path;
        if (!ResolvePluginMcPath(params.at("path").asString(), path)) { done(Err(-32090, "路径不安全")); return; }
        const std::wstring abs = path.wstring();
        std::thread([abs, done]() {
            try {
                std::error_code ec;
                std::filesystem::create_directories(abs, ec);
                if (ec) { done(Err(-32000, "目录创建失败")); return; }
                Json out = Json::object(); out["ok"] = true; done(Ok(out));
            } catch (const std::exception& e) {
                done(Err(-32000, std::string("创建目录异常：") + e.what()));
            } catch (...) {
                done(Err(-32000, "创建目录未知异常"));
            }
        }).detach();
    });

    bridge.RegisterAsync("fs.rm", [](const Json& params, const std::function<void(HandlerResult)>& done) {
        if (!params.isObject() || !params.contains("path") || !params.at("path").isString()) {
            done(Err(-32602, "缺少参数 path")); return;
        }
        const std::string& input = params.at("path").asString();
        if (input == "/") { done(Err(-32090, "禁止删除 Minecraft 根目录")); return; }
        const bool recursive = params.contains("recursive") && params.at("recursive").isBool()
            ? params.at("recursive").asBool() : false;
        std::filesystem::path path;
        if (!ResolvePluginMcPath(input, path)) { done(Err(-32090, "路径不安全")); return; }
        const std::wstring abs = path.wstring();
        std::thread([abs, recursive, done]() {
            try {
                std::error_code ec;
                if (recursive) std::filesystem::remove_all(abs, ec);
                else std::filesystem::remove(abs, ec);
                if (ec) { done(Err(-32000, "删除失败")); return; }
                Json out = Json::object(); out["ok"] = true; done(Ok(out));
            } catch (const std::exception& e) {
                done(Err(-32000, std::string("删除异常：") + e.what()));
            } catch (...) {
                done(Err(-32000, "删除未知异常"));
            }
        }).detach();
    });

    bridge.RegisterAsync("shell.openFile", [](const Json& params, const std::function<void(HandlerResult)>& done) {
        if (!params.isObject() || !params.contains("path") || !params.at("path").isString()) {
            done(Err(-32602, "缺少参数 path")); return;
        }
        std::filesystem::path path;
        if (!ResolvePluginMcPath(params.at("path").asString(), path)) { done(Err(-32090, "路径不安全")); return; }
        const std::wstring abs = path.wstring();
        std::thread([abs, done]() {
            try {
                std::error_code ec;
                if (!std::filesystem::is_regular_file(abs, ec) || ec) { done(Err(-32000, "文件不存在")); return; }
                HINSTANCE result = ShellExecuteW(nullptr, L"open", abs.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                if (reinterpret_cast<INT_PTR>(result) <= 32) { done(Err(-32000, "无法打开文件")); return; }
                Json out = Json::object(); out["ok"] = true; done(Ok(out));
            } catch (const std::exception& e) {
                done(Err(-32000, std::string("打开文件异常：") + e.what()));
            } catch (...) {
                done(Err(-32000, "打开文件未知异常"));
            }
        }).detach();
    });

    bridge.RegisterAsync("shell.getEnv", [](const Json& params, const std::function<void(HandlerResult)>& done) {
        if (!params.isObject() || !params.contains("key") || !params.at("key").isString()) {
            done(Err(-32602, "缺少参数 key")); return;
        }
        const std::string& key = params.at("key").asString();
        if (!IsEnvKey(key)) { done(Err(-32602, "非法环境变量名")); return; }
        const std::wstring wkey = lxe::Utf8ToWide(key);
        std::thread([wkey, done]() {
            try {
                std::vector<wchar_t> buffer(256);
                DWORD length = 0;
                for (;;) {
                    length = GetEnvironmentVariableW(wkey.c_str(), buffer.data(), static_cast<DWORD>(buffer.size()));
                    if (length == 0) {
                        if (GetLastError() == ERROR_ENVVAR_NOT_FOUND) {
                            Json out = Json::object(); out["value"] = ""; out["exists"] = false; done(Ok(out)); return;
                        }
                        done(Err(-32000, "无法读取环境变量")); return;
                    }
                    if (length < buffer.size()) break;
                    buffer.resize(length + 1);
                }
                Json out = Json::object();
                out["value"] = lxe::WideToUtf8(std::wstring(buffer.data(), length));
                out["exists"] = true;
                done(Ok(out));
            } catch (const std::exception& e) {
                done(Err(-32000, std::string("读取环境变量异常：") + e.what()));
            } catch (...) {
                done(Err(-32000, "读取环境变量未知异常"));
            }
        }).detach();
    });
    bridge.RegisterAsync("shell.execute", [](const Json& params, const std::function<void(HandlerResult)>& done) {
        if (!params.isObject() || !params.contains("command") || !params.at("command").isString()) {
            done(Err(-32602, "缺少参数 command")); return;
        }
        const std::string& command = params.at("command").asString();
        if (command.empty()) { done(Err(-32602, "command 不能为空")); return; }
        for (unsigned char c : command) if (c == 0 || c == '\r' || c == '\n' || c == '"') { done(Err(-32602, "非法 command")); return; }
        std::filesystem::path executable;
        if (command[0] == '/') {
            if (!ResolvePluginMcPath(command, executable)) { done(Err(-32090, "命令路径不安全")); return; }
            std::error_code ec;
            if (!std::filesystem::is_regular_file(executable, ec) || ec) { done(Err(-32000, "命令不存在")); return; }
        } else {
            for (unsigned char c : command) {
                if (!(std::isalnum(c) || c == '.' || c == '_' || c == '-')) { done(Err(-32602, "command 必须是基础命令名")); return; }
            }
        }
        const std::wstring commandLine = command[0] == '/'
            ? QuoteProcessArg(executable.wstring())
            : QuoteProcessArg(lxe::Utf8ToWide(command));
        if (params.contains("args")) {
            if (!params.at("args").isArray()) { done(Err(-32602, "args 必须是数组")); return; }
            for (const Json& arg : params.at("args").asArray()) {
                if (!arg.isString()) { done(Err(-32602, "args 必须为字符串")); return; }
                const std::wstring warg = lxe::Utf8ToWide(arg.asString());
                if (warg.find(L'\0') != std::wstring::npos || warg.find(L'\r') != std::wstring::npos || warg.find(L'\n') != std::wstring::npos) {
                    done(Err(-32602, "args 含非法字符")); return;
                }
            }
        }
        std::filesystem::path cwd;
        if (params.contains("cwd")) {
            if (!params.at("cwd").isString()) { done(Err(-32602, "cwd 必须是字符串")); return; }
            if (!ResolvePluginMcPath(params.at("cwd").asString(), cwd)) { done(Err(-32090, "工作目录不安全")); return; }
        } else {
            if (!ResolvePluginMcPath("/", cwd)) { done(Err(-32090, "Minecraft 根目录不可用")); return; }
        }
        std::error_code ec;
        if (!std::filesystem::is_directory(cwd, ec) || ec) { done(Err(-32000, "工作目录不存在")); return; }
        std::vector<std::wstring> args;
        if (params.contains("args") && params.at("args").isArray()) {
            for (const Json& arg : params.at("args").asArray()) args.push_back(lxe::Utf8ToWide(arg.asString()));
        }
        const std::wstring wcwd = cwd.wstring();
        std::thread([commandLine, args, wcwd, done]() {
            try {
                std::wstring full = commandLine;
                for (const std::wstring& a : args) { full += L" "; full += QuoteProcessArg(a); }
                std::vector<wchar_t> buffer(full.begin(), full.end());
                buffer.push_back(L'\0');
                STARTUPINFOW si{}; si.cb = sizeof(si);
                PROCESS_INFORMATION pi{};
                if (!CreateProcessW(nullptr, buffer.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                                    wcwd.c_str(), &si, &pi)) { done(Err(-32000, "命令启动失败")); return; }
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
                Json out = Json::object(); out["pid"] = static_cast<int>(pi.dwProcessId); done(Ok(out));
            } catch (const std::exception& e) {
                done(Err(-32000, std::string("执行命令异常：") + e.what()));
            } catch (...) {
                done(Err(-32000, "执行命令未知异常"));
            }
        }).detach();
    });

    bridge.Register("shell.openUrl", [](const Json& params) {
        if (!params.isObject() || !params.contains("url") || !params.at("url").isString())
            return Err(-32602, "缺少参数 url");
        const std::string& url = params.at("url").asString();
        if (url.empty() || (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0))
            return Err(-32602, "仅允许 http(s) 链接");
        HINSTANCE result = ShellExecuteW(nullptr, L"open", lxe::Utf8ToWide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(result) <= 32) return Err(-32000, "无法打开链接");
        Json out = Json::object(); out["ok"] = true; return Ok(out);
    });

    // 读取插件自身目录内的资源文件（styles/图标等），仅允许插件读取自己的目录，路径不得含 ".."
    bridge.Register("plugin.readAsset", [](const Json& params) {
        if (!params.isObject() || !params.contains("key") || !params.at("key").isString() ||
            !params.contains("file") || !params.at("file").isString())
            return Err(-32602, "缺少参数 key/file");
        const std::string& key = params.at("key").asString();
        const std::string& file = params.at("file").asString();
        if (file.empty() || file.find("..") != std::string::npos || file.find('\\') != std::string::npos)
            return Err(-32090, "资源路径不安全");
        std::wstring root = PluginsRootDir();
        std::error_code ec;
        std::wstring dir;
        for (std::filesystem::directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, ec);
             !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
            if (!it->is_directory(ec) || ec) continue;
            const std::wstring d = it->path().wstring();
            auto [raw, mok] = ReadFileUtf8(d + L"\\manifest.json");
            if (!mok || raw.empty()) continue;
            try {
                Json m = Json::parse(raw);
                if (!m.isObject()) continue;
                const std::string id = ManifestField(m, "id");
                const std::string folder = lxe::WideToUtf8(it->path().filename().wstring());
                if (id == key || folder == key) { dir = d; break; }
            } catch (...) { continue; }
        }
        if (dir.empty()) return Err(-32004, "未找到插件：" + key);
        const std::wstring full = dir + L"\\" + lxe::Utf8ToWide(file);
        if (!std::filesystem::is_regular_file(full, ec) || ec) return Err(-32000, "资源不存在");
        auto [content, ok] = ReadFileUtf8(full);
        if (!ok) return Err(-32000, "资源读取失败");
        Json out = Json::object(); out["content"] = content; return Ok(out);
    });

    // 启用/禁用插件（写 settings.json 的 pluginStates，并尝试卸载已加载代码）
    bridge.Register("plugin.setEnabled", [](const Json& params) {
        if (!params.isObject() || !params.contains("key") || !params.at("key").isString())
            return Err(-32602, "缺少参数 key");
        if (!params.contains("enabled") || !params.at("enabled").isBool())
            return Err(-32602, "缺少参数 enabled");
        const std::string key = params.at("key").asString();
        const bool enabled = params.at("enabled").asBool();
        Json s = LoadSettingsFile();
        Json st = Json::object();
        if (s.contains("pluginStates") && s.at("pluginStates").isObject()) st = s.at("pluginStates");
        st[key] = enabled;
        Json patch = Json::object();
        patch["pluginStates"] = st;
        SaveSettingsFile(patch);
        Json out = Json::object();
        out["ok"] = true;
        out["key"] = key;
        out["enabled"] = enabled;
        return Ok(out);
    });

    // 打开插件目录（仅放行白名单目录）
    bridge.Register("app.openFolder", [](const Json& params) {
        std::string folder;
        if (params.isObject() && params.contains("folder") && params.at("folder").isString())
            folder = params.at("folder").asString();
        std::wstring dir;
        if (folder == "plugins") dir = PluginsRootDir();
        else return Err(-32089, "未知的目录：" + folder);
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        Json out = Json::object();
        out["ok"] = true;
        out["path"] = lxe::WideToUtf8(dir);
        return Ok(out);
    });
}

void RegisterDemoServices(Bridge& bridge) {
    RegisterPing(bridge);
    RegisterAppConfig(bridge);
    RegisterSettings(bridge);
    RegisterSystemInfo(bridge);
    RegisterSubscribe(bridge);
    RegisterDownloadSimulate(bridge);
    RegisterBuildMeta(bridge);
    RegisterAria2(bridge);
    RegisterMinecraftVersions(bridge);
    RegisterMinecraftInstall(bridge);
    RegisterMinecraftLocalVersions(bridge);
    RegisterMinecraftImportFolder(bridge);
    RegisterMinecraftLaunch(bridge);
    RegisterLogExport(bridge);
    RegisterSubmitDownloadList(bridge);
    RegisterInstallStatus(bridge);
    RegisterAuthServices(bridge);
    RegisterMcFolders(bridge);
    RegisterLoaderServices(bridge);
    RegisterPlugins(bridge);
    StartGameMonitor(bridge);
    // §1 定期自动验证 Java 缓存（进程生命周期内后台线程，间隔 10 分钟）
    StartPeriodicJavaVerify();
}

void RegisterWindowServices(Bridge& bridge, HWND hwnd) {
    bridge.Register("window.minimize", [hwnd](const Json&) {
        ShowWindow(hwnd, SW_MINIMIZE);
        return Ok(Json::object());
    });

    bridge.Register("window.toggleMaximize", [hwnd](const Json&) {
        if (IsZoomed(hwnd)) {
            ShowWindow(hwnd, SW_RESTORE);
        } else {
            ShowWindow(hwnd, SW_MAXIMIZE);
        }
        return Ok(Json::object());
    });

    bridge.Register("window.close", [hwnd](const Json&) {
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
        return Ok(Json::object());
    });

    // 顶栏拖动：前端 mousedown 时调用
    bridge.Register("window.startDrag", [hwnd](const Json&) {
        ReleaseCapture();
        PostMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        return Ok(Json::object());
    });

    // 无边框窗口边缘缩放：前端检测到边缘 mousedown 时调用
    bridge.Register("window.startResize", [hwnd](const Json& params) {
        if (IsZoomed(hwnd)) return Ok(Json::object());

        std::string edge;
        if (params.isObject() && params.contains("edge")) {
            edge = params.at("edge").asString();
        }

        WPARAM hit = HTCLIENT;
        if (edge == "left")          hit = HTLEFT;
        else if (edge == "right")    hit = HTRIGHT;
        else if (edge == "top")      hit = HTTOP;
        else if (edge == "bottom")   hit = HTBOTTOM;
        else if (edge == "top-left")     hit = HTTOPLEFT;
        else if (edge == "top-right")    hit = HTTOPRIGHT;
        else if (edge == "bottom-left")  hit = HTBOTTOMLEFT;
        else if (edge == "bottom-right") hit = HTBOTTOMRIGHT;
        else return Err(-32602, "unknown edge");

        ReleaseCapture();
        PostMessageW(hwnd, WM_NCLBUTTONDOWN, hit, 0);
        return Ok(Json::object());
    });

    bridge.Register("window.getState", [hwnd](const Json&) {
        Json result = Json::object();
        result["maximized"] = IsZoomed(hwnd) ? true : false;
        result["minimized"] = IsIconic(hwnd) ? true : false;
        return Ok(result);
    });

    // 窗口圆角档位切换：设置 → 设置 { mode, radius? } → 立即应用 → 写入 settings.json
    bridge.Register("window.setCorner", [hwnd](const Json& params) {
        std::string mode;
        if (params.isObject() && params.contains("mode") && params.at("mode").isString()) {
            mode = params.at("mode").asString();
        } else if (params.isString()) {
            mode = params.asString();
        }
        if (mode != "none" && mode != "small" && mode != "medium" && mode != "large" && mode != "custom") {
            return Err(-32602, "unknown corner mode, expected: none|small|medium|large|custom");
        }
        SetWindowCornerMode(mode);
        // 自定义档可携带像素半径
        if (mode == "custom" && params.isObject() && params.contains("radius") && params.at("radius").isNumber()) {
            SetWindowCornerRadius(static_cast<int>(params.at("radius").asNumber()));
        }
        ApplyWindowCorner(hwnd);
        // 持久化：写入 settings.json
        Json patch = Json::object();
        patch["windowCorner"] = mode;
        if (mode == "custom") patch["windowCornerRadius"] = GetWindowCornerRadius();
        SaveSettingsFile(patch);
        Json r = Json::object();
        r["mode"] = mode;
        r["radius"] = GetWindowCornerRadius();
        return Ok(r);
    });

    // 获取当前圆角档位（前端下拉框回显）
    bridge.Register("window.getCorner", [hwnd](const Json&) {
        Json r = Json::object();
        r["mode"] = GetWindowCornerMode();
        r["radius"] = GetWindowCornerRadius();
        return Ok(r);
    });

    // 窗口亚克力毛玻璃开关（"开发者模式-亚克力"选项）：enabled=true 开启，false 关闭
    bridge.Register("window.setAcrylic", [hwnd](const Json& params) {
        bool enabled = true;
        if (params.isObject() && params.contains("enabled") && params.at("enabled").isBool()) {
            enabled = params.at("enabled").asBool();
        }
        SetAcrylicEnabled(enabled);
        ApplyWindowAcrylic(hwnd);
        // 持久化：写入 settings.json
        Json patch = Json::object();
        patch["acrylic"] = enabled;
        SaveSettingsFile(patch);
        Json r = Json::object();
        r["enabled"] = enabled;
        return Ok(r);
    });

    // 获取当前亚克力开关状态（前端回显）
    bridge.Register("window.getAcrylic", [](const Json&) {
        Json r = Json::object();
        r["enabled"] = GetAcrylicEnabled();
        return Ok(r);
    });
}

// ========== 窗口毛玻璃（Acrylic）实现 ==========
static bool g_acrylicEnabled = true;

void SetAcrylicEnabled(bool enabled) {
    g_acrylicEnabled = enabled;
}

bool GetAcrylicEnabled() {
    return g_acrylicEnabled;
}

void ApplyWindowAcrylic(HWND hwnd) {
    if (!hwnd) return;
    if (g_acrylicEnabled) EnableAcrylic(hwnd, 24, 32, 48, 128);
    else DisableAcrylic(hwnd);
}

// ========== 窗口圆角实现 ==========
// DWMWA_WINDOW_CORNER_PREFERENCE = 33
// DWMWCP_DEFAULT=0  DONOTROUND=1  ROUND=2  ROUNDSMALL=3
static void SetDwmCornerPreference(HWND hwnd, int value) {
    HMODULE dwm = GetModuleHandleW(L"dwmapi.dll");
    if (!dwm) return;
    using FnDwmSet = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    auto fn = reinterpret_cast<FnDwmSet>(GetProcAddress(dwm, "DwmSetWindowAttribute"));
    if (!fn) return;
    int attr = value;
    // Windows 10 及以下调用会失败，忽略返回值
    fn(hwnd, 33, &attr, sizeof(attr));
}

std::string GetWindowCornerMode() {
    std::lock_guard<std::mutex> lk(g_cornerMu);
    return g_cornerMode;
}

void SetWindowCornerMode(const std::string& mode) {
    std::lock_guard<std::mutex> lk(g_cornerMu);
    g_cornerMode = mode;
}

// 自定义圆角半径（px），默认 16；仅在档位为 custom 时使用
static int g_cornerRadius = 16;

void SetWindowCornerRadius(int radius) {
    std::lock_guard<std::mutex> lk(g_cornerMu);
    if (radius < 0) radius = 0;
    if (radius > 40) radius = 40;
    g_cornerRadius = radius;
}

int GetWindowCornerRadius() {
    std::lock_guard<std::mutex> lk(g_cornerMu);
    return g_cornerRadius;
}

void ApplyWindowCorner(HWND hwnd) {
    if (!hwnd) return;
    std::string mode;
    int radius = 16;
    {
        std::lock_guard<std::mutex> lk(g_cornerMu);
        mode = g_cornerMode;
        radius = g_cornerRadius;
    }

    // 自定义档：使用 SetWindowRgn 按像素半径裁剪出真实的窗口圆角。
    //   预设档（none/small/medium/large）仍走 DWM 档位（见下方注释）。
    if (mode == "custom") {
        SetDwmCornerPreference(hwnd, 1); // DWMWCP_DONOTROUND，避免与自绘圆角叠加
        if (radius <= 0) {
            SetWindowRgn(hwnd, NULL, TRUE);
            return;
        }
        RECT rc{};
        GetWindowRect(hwnd, &rc);
        int w = rc.right - rc.left;
        int h = rc.bottom - rc.top;
        if (w <= 0 || h <= 0) return;
        int r = radius;
        // 半径不得超过窗口短边的一半，否则 CreateRoundRectRgn 会失败
        int maxR = (w < h ? w : h) / 2;
        if (r > maxR) r = maxR;
        // SetWindowRgn 成功后会接管该区域的释放；失败则自行释放避免泄漏
        HRGN region = CreateRoundRectRgn(0, 0, w + 1, h + 1, r * 2, r * 2);
        if (region) {
            if (SetWindowRgn(hwnd, region, TRUE) == 0) {
                DeleteObject(region);
            }
        }
        return;
    }

    // 预设档统一使用 DWMWA_WINDOW_CORNER_PREFERENCE，不调用 SetWindowRgn。
    //   原因：SetWindowRgn 会裁剪掉窗口圆角区域外的像素，导致该区域内
    //   鼠标不再向窗口投递 WM_NCHITTEST / WM_SETCURSOR / 点击 等消息，
    //   表现为"窗口边角 resize 指针不显示"、"边角附近按钮点不动"。
    //   大圆角档（large）通过 DWM ROUND + 前端 CSS 超大 border-radius 视觉叠加来实现：
    //   窗口底层仍保留 DWM 的中等圆角（不会有直角），内容层再用 24px border-radius
    //   + ::before 四角遮罩形成"大圆角"视觉，窗口区域保持矩形，鼠标消息无损。
    int pref = 2; // DWMWCP_ROUND 默认
    if (mode == "none") pref = 1;       // DWMWCP_DONOTROUND
    else if (mode == "small") pref = 3; // DWMWCP_ROUNDSMALL
    else pref = 2;                      // medium / large → DWMWCP_ROUND

    SetDwmCornerPreference(hwnd, pref);
    SetWindowRgn(hwnd, NULL, TRUE); // 始终保持默认矩形区域，避免 SetWindowRgn 的副作用
}

} // namespace lxe
