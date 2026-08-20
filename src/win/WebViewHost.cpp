#include "WebViewHost.h"

#include <commctrl.h>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <ole2.h>      // IDropTarget / IDataObject / FORMATETC
#include <objidl.h>    // CreateStreamOnHGlobal
#include <regex>
#include <shlobj.h>    // CF_HDROP
#include <sstream>
#include <shellapi.h>
#include <shlwapi.h>
#include <thread>
#include <windowsx.h>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "comctl32.lib")

#include "Utf.h"

// 虚拟域名（SetVirtualHostNameToFolderMapping）
// 注意：不要用 .local 结尾（如 app.local）——WebView2 会先走 DNS 解析该域名再应用映射，
// 解析超时会导致每次导航慢约 2 秒。.localhost 是 RFC 6761 保留名，解析到回环地址无需联网。
static constexpr const wchar_t* kVirtualHost = L"app.localhost";

// 顶栏高度（8px 顶部边距 + 56px navbar 高度），用于原生拖动命中检测
static constexpr int kTitleBarHeight = 64;

// 从 settings.json 读取 useVirtualHost 设置（默认 false = HTTP 模式，与老版本一致更流畅）
static bool ReadUseVirtualHostSetting() {
    wchar_t exePath[MAX_PATH]{};
    DWORD n = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return false; // 默认 HTTP 模式
    std::filesystem::path p(exePath);
    auto settingsPath = (p.parent_path() / L"settings.json").wstring();
    std::ifstream f(settingsPath, std::ios::binary);
    if (!f) return false; // 文件不存在，默认 HTTP 模式
    std::stringstream ss; ss << f.rdbuf();
    std::string s = ss.str();
    if (s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) {
        s = s.substr(3);
    }
    std::regex re("\"useVirtualHost\"\\s*:\\s*(true|false)");
    std::smatch m;
    if (std::regex_search(s, m, re) && m.size() >= 2) {
        return m[1].str() == "true";
    }
    return false; // 未找到键，默认 HTTP 模式
}
// 无边框缩放边框宽度（与 LXElauncher.cpp 中 kResizeBorder 保持一致）
static constexpr int kResizeBorder = 6;
static constexpr UINT_PTR kWebViewSubclassId = 12345;
// 自定义消息：由非 UI 线程 PostMessage 到主窗口，通知 UI 线程 flush 待发消息队列
// 对应 WebViewHost.h 中公开的 WM_WEBVIEW_FLUSH_JSON（WM_USER + 133）
static constexpr UINT WM_FLUSH_POST_JSON = WM_USER + 133;
static_assert(WM_FLUSH_POST_JSON == WM_WEBVIEW_FLUSH_JSON, "WM_WEBVIEW_FLUSH_JSON 必须与 WebViewHost.h 保持一致");

namespace {

using namespace Microsoft::WRL;

bool IsLocalhostUri(const std::wstring& uri) {
    if (uri.rfind(L"http://localhost:", 0) == 0) return true;
    if (uri.rfind(L"http://127.0.0.1:", 0) == 0) return true;
    return false;
}

std::wstring ResolveWebAppFolder() {
    wchar_t exePath[MAX_PATH]{};
    DWORD n = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L"";
    std::filesystem::path p(exePath);
    return (p.parent_path() / L"webapp").wstring();
}

// 拖放诊断日志：写入 <exe目录>\logs\lxe-drag.log（智能拖放排查用，失败不阻塞业务）
std::mutex g_dragLogMu;
void DragLog(const std::string& msg) {
    try {
        wchar_t exePath[MAX_PATH]{};
        DWORD n = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) return;
        std::filesystem::path p(exePath);
        std::error_code ec;
        std::filesystem::create_directories(p.parent_path() / L"logs", ec);
        std::ofstream f(p.parent_path() / L"logs" / L"lxe-drag.log", std::ios::app);
        if (!f) return;
        SYSTEMTIME st;
        GetLocalTime(&st);
        char ts[64];
        wsprintfA(ts, "%02u:%02u:%02u.%03u ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        std::lock_guard<std::mutex> lk(g_dragLogMu);
        f << ts << msg << "\n";
    } catch (...) {}
}
std::string Hex8(DWORD v) {
    char buf[16];
    wsprintfA(buf, "0x%08X", v);
    return buf;
}

} // namespace

// ---- 内嵌 webapp 资源（WebApp.rc RCDATA，资源名 = webapp 内相对路径） ----
// 与 tools\pack_webapp.ps1 生成的 WebApp.rc 配合：编译时把 webapp/ 目录整体
// 编进 exe，运行时由 WebResourceRequested 从内存返回，发布无需携带 webapp 目录。
namespace {

// 读取内嵌文件字节；不存在返回 false（调用方放行到磁盘 webapp 文件夹）。
// RC.exe 把字符串资源名按大写 + 带引号的形式存入 PE（实测名称为 "INDEX.HTML"，
// 含外层双引号），FindResourceW 大小写不敏感，因此查询名必须用引号包住相对路径。
bool EmbeddedWebFileGet(const std::wstring& relPath, std::vector<char>& out) {
    out.clear();
    std::wstring quoted = L"\"" + relPath + L"\"";
    HRSRC h = FindResourceW(nullptr, quoted.c_str(), RT_RCDATA);
    if (!h) return false;
    HGLOBAL g = LoadResource(nullptr, h);
    if (!g) return false;
    void* p = LockResource(g);
    DWORD sz = SizeofResource(nullptr, h);
    if (!p || sz == 0) return false;
    const char* bytes = static_cast<const char*>(p);
    out.assign(bytes, bytes + sz);
    return true;
}

// 从 URI 提取 webapp 相对路径（去掉协议/主机/查询参数）；空路径默认 index.html。
// 例： https://app.localhost/index.html                      -> index.html
//      http://127.0.0.1:51837/vendor/fontawesome/all.min.css -> vendor/fontawesome/all.min.css
std::wstring WebRelPathFromUri(const std::wstring& uri) {
    size_t slash2 = uri.find(L"//");
    if (slash2 == std::wstring::npos) return L"index.html";
    std::wstring s = uri.substr(slash2 + 2);
    size_t q = s.find(L'?');
    if (q != std::wstring::npos) s = s.substr(0, q);
    size_t p1 = s.find(L'/');
    if (p1 == std::wstring::npos) return L"index.html";
    std::wstring rel = s.substr(p1 + 1);
    if (rel.empty()) return L"index.html";
    return rel;
}

// 简单的百分号解码（资源名只需处理 %xx 转义；不走 WebServer 的私有 UrlDecode）。
std::wstring WebPercentDecode(const std::wstring& in) {
    std::wstring out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        wchar_t c = in[i];
        if (c == L'%' && i + 2 < in.size()) {
            auto hx = [](wchar_t h) -> int {
                if (h >= L'0' && h <= L'9') return h - L'0';
                if (h >= L'a' && h <= L'f') return h - L'a' + 10;
                if (h >= L'A' && h <= L'F') return h - L'A' + 10;
                return -1;
            };
            int hi = hx(in[i + 1]), lo = hx(in[i + 2]);
            if (hi >= 0 && lo >= 0) { out.push_back(static_cast<wchar_t>((hi << 4) | lo)); i += 2; continue; }
        }
        out.push_back(c);
    }
    return out;
}

std::string WebMimeForPath(const std::wstring& path) {
    std::wstring lower = path;
    for (auto& c : lower) c = towlower(c);
    auto ends = [&](const wchar_t* suffix) {
        std::wstring s(suffix);
        if (lower.size() < s.size()) return false;
        return lower.compare(lower.size() - s.size(), s.size(), s) == 0;
    };
    if (ends(L".html")) return "text/html; charset=utf-8";
    if (ends(L".js")) return "text/javascript; charset=utf-8";
    if (ends(L".css")) return "text/css; charset=utf-8";
    if (ends(L".json")) return "application/json; charset=utf-8";
    if (ends(L".png")) return "image/png";
    if (ends(L".webp")) return "image/webp";
    if (ends(L".jpg") || ends(L".jpeg")) return "image/jpeg";
    if (ends(L".gif")) return "image/gif";
    if (ends(L".svg")) return "image/svg+xml";
    if (ends(L".ttf")) return "font/ttf";
    if (ends(L".woff")) return "font/woff";
    if (ends(L".woff2")) return "font/woff2";
    if (ends(L".ico")) return "image/x-icon";
    return "application/octet-stream";
}

} // namespace

// 智能拖放：接收系统拖放，提取拖入文件/文件夹的真实本地路径
// （页面 JS 因浏览器安全模型拿不到完整路径），通过 nativeDrop 事件转发给前端，
// 由前端按类型（authlib/模组/.minecraft/材质包/光影/整合包/背景图/java.exe/文字）自动应用。
// DragEnter/DragLeave 同步 nativeDragEnter/nativeDragLeave 事件，供前端显示拖放提示层。
// 定义在全局作用域（与 WebViewHost.h 中的前向声明一致）；Json 位于 lxe 命名空间。
class WebDropTarget : public IDropTarget {
public:
    explicit WebDropTarget(lxe::Bridge* bridge) : bridge_(bridge) {}

    // ---- IUnknown ----
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDropTarget) {
            *ppv = this;
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --ref_;
        if (r == 0) delete this;
        return r;
    }

    // ---- IDropTarget ----
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject*, DWORD, POINTL, DWORD* pdwEffect) override {
        if (pdwEffect) *pdwEffect = DROPEFFECT_COPY;
        DragLog("[DropTarget] DragEnter -> nativeDragEnter");
        if (bridge_) bridge_->PostEvent("nativeDragEnter", lxe::Json::object());
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD* pdwEffect) override {
        if (pdwEffect) *pdwEffect = DROPEFFECT_COPY;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override {
        DragLog("[DropTarget] DragLeave -> nativeDragLeave");
        if (bridge_) bridge_->PostEvent("nativeDragLeave", lxe::Json::object());
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* pDataObj, DWORD, POINTL, DWORD* pdwEffect) override {
        if (pdwEffect) *pdwEffect = DROPEFFECT_COPY;
        if (!bridge_ || !pDataObj) return S_OK;

        lxe::Json ev = lxe::Json::object();
        lxe::Json arr = lxe::Json::array();
        lxe::Json dirs = lxe::Json::array();

        // 文件/文件夹（从资源管理器拖入）
        FORMATETC fmt{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        STGMEDIUM med{};
        if (SUCCEEDED(pDataObj->GetData(&fmt, &med)) && med.hGlobal) {
            HDROP hDrop = static_cast<HDROP>(GlobalLock(med.hGlobal));
            if (hDrop) {
                UINT n = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
                for (UINT i = 0; i < n; i++) {
                    UINT len = DragQueryFileW(hDrop, i, nullptr, 0);
                    std::wstring p(len, L'\0');
                    DragQueryFileW(hDrop, i, &p[0], len + 1);
                    arr.asArray().push_back(lxe::WideToUtf8(p));
                    bool isDir = false;
                    std::error_code dec;
                    if (std::filesystem::is_directory(p, dec)) isDir = true;
                    dirs.asArray().push_back(isDir);
                }
                GlobalUnlock(med.hGlobal);
            }
            ReleaseStgMedium(&med);
        }
        ev["paths"] = arr;
        ev["dirs"] = dirs;

        // 文本（浏览器选中文字、authlib-injector:yggdrasil-server:… 链接等）
        std::wstring text;
        FORMATETC fmtText{CF_UNICODETEXT, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        STGMEDIUM medText{};
        if (SUCCEEDED(pDataObj->GetData(&fmtText, &medText)) && medText.hGlobal) {
            const wchar_t* ptr = static_cast<const wchar_t*>(GlobalLock(medText.hGlobal));
            if (ptr) text = ptr;
            GlobalUnlock(medText.hGlobal);
            ReleaseStgMedium(&medText);
        }
        ev["text"] = lxe::WideToUtf8(text);

        DragLog("[DropTarget] Drop: files=" + std::to_string(arr.asArray().size()) +
                " textLen=" + std::to_string(text.size()) + " -> nativeDrop");
        bridge_->PostEvent("nativeDrop", ev);
        return S_OK;
    }

private:
    lxe::Bridge* bridge_;      // 指向 WebViewHost::bridge_，析构在 WebViewHost 析构前完成
    std::atomic<ULONG> ref_{1};
};

bool WebViewHost::Initialize(HWND parent) {
    hwnd_ = parent;
    ui_thread_id_ = GetCurrentThreadId();
    bridge_.SetPoster([this](const std::string& json) { PostToWeb(json); });

    // 读取 settings.json 判断是否使用虚拟域名模式
    use_virtual_host_ = ReadUseVirtualHostSetting();
    std::wstring webappPath = ResolveWebAppFolder();
    webapp_path_ = webappPath;
    // webapp 是否已作为 RCDATA 资源编进 exe（WebApp.rc 由 tools\pack_webapp.ps1 生成）
    bool webEmbedded = (FindResourceW(nullptr, L"\"index.html\"", RT_RCDATA) != nullptr);
    // 始终启动 HTTP 服务器作为后备（虚拟域名模式下也启动，映射失败时可无缝切换）。
    // 内嵌模式即使磁盘没有 webapp 目录，也需要服务器提供 HomeUrl 供 HTTP 模式导航。
    if (!webappPath.empty() || webEmbedded) {
        web_server_.Start(webappPath);
    }

    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, nullptr,
        nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this](HRESULT envResult, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(envResult)) return envResult;
                if (env) env_ = env; // 保留环境，供内存返回 webapp 资源时 CreateWebResourceResponse
                HRESULT hr = env->CreateCoreWebView2Controller(
                    hwnd_,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [this](HRESULT ctrlResult, ICoreWebView2Controller* controller) -> HRESULT {
                            if (FAILED(ctrlResult)) return ctrlResult;
                            controller_ = controller;
                            if (FAILED(controller_->get_CoreWebView2(&webview_))) {
                                return E_FAIL;
                            }
                            return SetupWebView();
                        })
                        .Get());
                return hr;
            })
            .Get());

    return SUCCEEDED(hr);
}

HRESULT WebViewHost::SetupWebView() {
    HRESULT hr = S_OK;

    // 透明背景（让下层 Acrylic 毛玻璃透出）
    ComPtr<ICoreWebView2Controller2> controller2;
    if (controller_.As(&controller2) == S_OK) {
        COREWEBVIEW2_COLOR color{0, 0, 0, 0}; // A=R=G=B=0 透明
        controller2->put_DefaultBackgroundColor(color);
    }

    // 智能拖放：禁用 WebView2 默认外部拖放（页面 JS 拿不到真实路径），
    // 改由主窗口注册的 IDropTarget 接管，通过 IDataObject 提取拖入文件路径。
    // 必须在首次导航前设置，随后 RegisterDragDrop 才不会与 WebView2 内置目标冲突。
    {
        ComPtr<ICoreWebView2Controller4> controller4;
        if (controller_.As(&controller4) == S_OK) {
            BOOL allow = TRUE;
            HRESULT hrGet = controller4->get_AllowExternalDrop(&allow);
            HRESULT hrSet = S_OK;
            if (SUCCEEDED(hrGet) && allow) hrSet = controller4->put_AllowExternalDrop(FALSE);
            DragLog("[SetupWebView] AllowExternalDrop get=" + std::string(allow ? "TRUE" : "FALSE") +
                    " (" + Hex8(hrGet) + ") set(FALSE)=" + Hex8(hrSet));
        } else {
            DragLog("[SetupWebView] 无 ICoreWebView2Controller4，跳过 AllowExternalDrop");
        }
        RegisterNativeDropTarget();
    }

    RECT rc{};
    GetClientRect(hwnd_, &rc);
    controller_->put_Bounds(rc);

    hr = RegisterMessageHandler();
    if (FAILED(hr)) return hr;
    hr = RegisterNavigationHandlers();
    if (FAILED(hr)) return hr;

    ComPtr<ICoreWebView2Settings> settings;
    hr = webview_->get_Settings(&settings);
    if (SUCCEEDED(hr)) {
        settings->put_AreDefaultScriptDialogsEnabled(FALSE);
        settings->put_IsWebMessageEnabled(TRUE);
        settings->put_AreHostObjectsAllowed(TRUE);
        // 禁用 Ctrl+滚轮 / Ctrl++- 缩放
        settings->put_IsZoomControlEnabled(FALSE);
        settings->put_AreDevToolsEnabled(TRUE);
    }

    ComPtr<ICoreWebView2Settings2> settings2;
    if (settings.As(&settings2) == S_OK) {
        settings2->put_IsStatusBarEnabled(TRUE);
    }

    // 禁用触控板 pinch 缩放
    ComPtr<ICoreWebView2Settings5> settings5;
    if (settings.As(&settings5) == S_OK) {
        settings5->put_IsPinchZoomEnabled(FALSE);
    }

    // 内嵌 webapp 资源：webapp 已编进 exe 时，页面全部从内存返回（无需磁盘目录）
    RegisterEmbeddedResourceHandler();

    // 虚拟域名模式：先映射+导航（零延迟），后台线程再预检文件可读性
    if (use_virtual_host_ && !webapp_path_.empty()) {
        ComPtr<ICoreWebView2_3> webview3;
        HRESULT hrQi = webview_.As(&webview3);
        HRESULT hrMap = E_FAIL;
        if (SUCCEEDED(hrQi) && webview3) {
            hrMap = webview3->SetVirtualHostNameToFolderMapping(
                kVirtualHost,
                webapp_path_.c_str(),
                COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
        }
        if (FAILED(hrMap)) {
            // 映射 API 本身失败 → 立即回退，无需后台预检
            use_virtual_host_ = false;
        } else {
            // 映射成功 → 启动后台预检线程（复制路径，不捕获 this 的 webapp_path_ 引用）
            std::wstring pathCopy = webapp_path_;
            HWND hwndCopy = hwnd_;
            std::thread([pathCopy, hwndCopy]() {
                // 短暂延迟，让 NavigationCompleted 有机会先返回成功（比文件检查更权威）
                Sleep(50);
                try {
                    std::filesystem::path idx = std::filesystem::path(pathCopy) / L"index.html";
                    std::ifstream test(idx, std::ios::binary | std::ios::ate);
                    if (!test || test.tellg() <= 0) {
                        // 文件不可读 → PostMessage 回 UI 线程执行回退
                        PostMessage(hwndCopy, WM_WEBVIEW_FALLBACK, 0, 0);
                    }
                } catch (...) {
                    PostMessage(hwndCopy, WM_WEBVIEW_FALLBACK, 0, 0);
                }
            }).detach();
        }
    }

    // 导航到首页（虚拟域名 https://app.localhost/index.html 或 HTTP http://127.0.0.1:<port>/）
    std::wstring home = HomeUrl();
    if (!home.empty()) {
        InstallWebViewSubclass();
        RegisterNavCompletedHandler(); // 虚拟域名导航失败时自动回退 HTTP
        webview_->Navigate(home.c_str());
    }
    initialized_ = true;
    return S_OK;
}

WebViewHost::~WebViewHost() {
    // 先注销拖放目标再释放对象，避免窗口销毁后仍有 OLE 回调
    if (drop_registered_) {
        RevokeDragDrop(hwnd_);
        drop_registered_ = false;
    }
    if (child_drop_registered_) {
        if (IsWindow(webview_child_hwnd_)) RevokeDragDrop(webview_child_hwnd_);
        child_drop_registered_ = false;
    }
    if (drop_top_hwnd_) {
        if (IsWindow(drop_top_hwnd_)) RevokeDragDrop(drop_top_hwnd_);
        drop_top_hwnd_ = nullptr;
    }
    if (drop_target_) {
        drop_target_->Release();
        drop_target_ = nullptr;
    }
    UninstallWebViewSubclass();
}

void WebViewHost::EnsureTopmostDropTarget() {
    if (!drop_target_ || !hwnd_ || !drop_registered_) return;
    if (!IsWindow(hwnd_)) return;

    // 取客户端中心对应的最顶层窗口。OLE 拖放命中测试与 WindowFromPoint 一致：
    // 光标下的窗口必须是已注册拖放目标的窗口，不向父链跨窗口查找。
    RECT cr{};
    GetClientRect(hwnd_, &cr);
    if (cr.right <= cr.left || cr.bottom <= cr.top) return;
    POINT pt{ (cr.right - cr.left) / 2, (cr.bottom - cr.top) / 2 };
    ClientToScreen(hwnd_, &pt);
    HWND top = WindowFromPoint(pt);
    if (!top || top == hwnd_ || top == webview_child_hwnd_) return;  // 已由其他注册覆盖

    // 校验该窗口属于本窗口树（顶层祖先为主窗口），防止误注册悬浮在窗口上方的其他程序窗口
    HWND root = top;
    while (HWND p = GetParent(root)) root = p;
    if (root != hwnd_) return;

    if (top == drop_top_hwnd_) return;  // 已注册过同一个窗口

    // 撤销 WebView2 浏览器进程窗口上的内置拖放目标，注册我们的接管目标。
    // RegisterDragDrop 的注册表是全局窗口属性，跨进程（浏览器进程窗口）同样生效。
    HRESULT hrRevoke = RevokeDragDrop(top);
    HRESULT hrReg = RegisterDragDrop(top, drop_target_);
    drop_top_hwnd_ = SUCCEEDED(hrReg) ? top : nullptr;
    DragLog("[EnsureTopmostDropTarget] 命中窗口 HWND=" + Hex8((DWORD)(uintptr_t)top) +
            " Revoke=" + Hex8(hrRevoke) + " Register=" + Hex8(hrReg) +
            (drop_top_hwnd_ ? " 成功" : " 失败"));
}

void WebViewHost::RegisterNativeDropTarget() {
    if (drop_target_ || drop_registered_) return;
    // 生命周期由本类管理：new 时不 AddRef（ref_=1），析构时 Release 一次
    drop_target_ = new WebDropTarget(&bridge_);
    HRESULT hr = RegisterDragDrop(hwnd_, drop_target_);
    drop_registered_ = SUCCEEDED(hr);
    DragLog("[RegisterNativeDropTarget] 主窗口 RegisterDragDrop=" + Hex8(hr) +
            (drop_registered_ ? " 成功" : " 失败（OLE 未初始化或窗口已注册）"));
    if (!drop_registered_) {
        drop_target_->Release();
        drop_target_ = nullptr;
    }
}

void WebViewHost::InstallWebViewSubclass(bool forceDropReapply) {
    // 只枚举主窗口的直接子 HWND。Chrome_WidgetWin_1 是 WebView2 顶层控件。
    // 注意：不能要求子窗口可见（IsWindowVisible）——SetupWebView 早期子窗口可能
    // 尚未显示，若因不可见而跳过，WebView2 内置拖放目标会拦截所有拖入（表现为"拖不动"）。
    HWND child = nullptr;
    HWND cur = FindWindowExW(hwnd_, nullptr, nullptr, nullptr);
    while (cur) {
        wchar_t cls[128]{};
        if (GetClassNameW(cur, cls, _countof(cls))) {
            std::wstring c(cls);
            if (c == L"Chrome_WidgetWin_1") { child = cur; break; }
            if (!child && c.find(L"Chrome_WidgetWin") == 0) child = cur;
        }
        cur = FindWindowExW(hwnd_, cur, nullptr, nullptr);
    }
    if (!child) {
        DragLog("[InstallWebViewSubclass] 未找到 WebView2 子窗口（稍后由导航完成/Resize 重试）");
        return;
    }

    if (webview_child_hwnd_ && webview_child_hwnd_ != child) {
        // 子窗口句柄变化（WebView2 重建窗口）→ 卸载旧子类化并重置拖放注册状态
        DragLog("[InstallWebViewSubclass] 子窗口句柄变化 " + Hex8((DWORD)(uintptr_t)webview_child_hwnd_) +
                " -> " + Hex8((DWORD)(uintptr_t)child));
        child_drop_registered_ = false;
        UninstallWebViewSubclass();
    }
    if (!webview_child_hwnd_) {
        webview_child_hwnd_ = child;
        BOOL ok = SetWindowSubclass(child, WebViewSubclassProc,
                                    kWebViewSubclassId, reinterpret_cast<DWORD_PTR>(this));
        DragLog("[InstallWebViewSubclass] 子类化 HWND=" + Hex8((DWORD)(uintptr_t)child) +
                (ok ? " 成功" : " 失败"));
    }

    // 智能拖放：WebView2 子窗口自身也会注册 OLE 拖放目标（即使 AllowExternalDrop=FALSE
    // 仍可能拦截拖入事件，导致父窗口的 IDropTarget 收不到 DragEnter/Drop，表现即"拖不动"）。
    // 这里撤销其内置目标并改注册我们自己的 WebDropTarget，让整块客户端区域（子窗口覆盖
    // 全客户区）都能接收拖入文件，任何页面下拖放均生效。导航完成后 forceDropReapply=true
    // 重新执行一次，防止 WebView2 在导航期间重新注册内置目标覆盖我们的接管。
    if (drop_target_ && (!child_drop_registered_ || forceDropReapply)) {
        HRESULT hrRevoke = RevokeDragDrop(child);
        HRESULT hrReg = RegisterDragDrop(child, drop_target_);
        child_drop_registered_ = SUCCEEDED(hrReg);
        DragLog("[InstallWebViewSubclass] 子窗口拖放 Revoke=" + Hex8(hrRevoke) +
                " Register=" + Hex8(hrReg) + (child_drop_registered_ ? " 成功" : " 失败"));
    }
}

void WebViewHost::UninstallWebViewSubclass() {
    if (!webview_child_hwnd_) return;
    if (IsWindow(webview_child_hwnd_)) {
        RemoveWindowSubclass(webview_child_hwnd_, WebViewSubclassProc, kWebViewSubclassId);
    }
    webview_child_hwnd_ = nullptr;
}

LRESULT CALLBACK WebViewHost::WebViewSubclassProc(
    HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
    UINT_PTR uIdSubclass, DWORD_PTR dwRefData) {

    auto* self = reinterpret_cast<WebViewHost*>(dwRefData);
    if (!self) return DefSubclassProc(hWnd, uMsg, wParam, lParam);
    HWND parent = self->hwnd_;

    switch (uMsg) {
        case WM_NCDESTROY:
            // 子窗口销毁前先移除子类化
            self->UninstallWebViewSubclass();
            break;

        case WM_LBUTTONDBLCLK: {
            // 顶栏双击 = 最大化/还原切换（回退路径：no-drag 区域未注册时由 JS mousedown 触发）
            // 注意：WM_LBUTTONDBLCLK 的 lParam 是**本窗口（WebView 子 HWND）**客户区坐标
            int y = GET_Y_LPARAM(lParam);
            const UINT dpi = parent ? GetDpiForWindow(parent) : 96;
            const int topH = MulDiv(kTitleBarHeight, dpi, 96);
            if (y >= 0 && y <= topH && parent) {
                if (IsZoomed(parent))
                    SendMessageW(parent, WM_SYSCOMMAND, SC_RESTORE, 0);
                else
                    SendMessageW(parent, WM_SYSCOMMAND, SC_MAXIMIZE, 0);
                return 0;
            }
            break;
        }

        case WM_NCLBUTTONDOWN: {
            // WebView2 子窗口是 WS_CHILD，自身无法处理拖动/缩放。
            // 当 WM_NCHITTEST 返回 HTCAPTION/HTLEFT 等 时，Windows 将 WM_NCLBUTTONDOWN
            // 发给子窗口；此处转发给父窗口，由父窗口 DefWindowProc 执行原生拖动/缩放。
            if (parent) {
                SendMessageW(parent, WM_NCLBUTTONDOWN, wParam, lParam);
                return 0;
            }
            break;
        }

        case WM_NCLBUTTONDBLCLK: {
            // 顶栏双击最大化/还原（原生路径：HTCAPTION 区域双击时由 Windows 发送）
            if (parent && wParam == HTCAPTION) {
                if (IsZoomed(parent))
                    SendMessageW(parent, WM_SYSCOMMAND, SC_RESTORE, 0);
                else
                    SendMessageW(parent, WM_SYSCOMMAND, SC_MAXIMIZE, 0);
                return 0;
            }
            break;
        }

        case WM_NCHITTEST: {
            // 边缘缩放优先：先做边缘检测，避免被 WebView2 默认返回值（如 HTTRANSPARENT）
            // 跳过自定义命中，导致缩放失效。修复"上一版支持缩放、现在不支持"的回归。
            // WM_NCHITTEST 的 lParam 是屏幕坐标
            POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ScreenToClient(parent, &pt);
            RECT rc{};
            GetClientRect(parent, &rc);

            const UINT dpi = parent ? GetDpiForWindow(parent) : 96;
            const int b = MulDiv(kResizeBorder, dpi, 96);
            const int topH = MulDiv(kTitleBarHeight, dpi, 96);
            const bool zoomed = parent ? !!IsZoomed(parent) : false;
            if (!zoomed) {
                bool left   = pt.x < b;
                bool right  = pt.x >= rc.right - b;
                bool top    = pt.y < b;
                bool bottom = pt.y >= rc.bottom - b;

                if (top && left)     return HTTOPLEFT;
                if (top && right)    return HTTOPRIGHT;
                if (bottom && left)  return HTBOTTOMLEFT;
                if (bottom && right) return HTBOTTOMRIGHT;
                if (left)            return HTLEFT;
                if (right)           return HTRIGHT;
                if (top)             return HTTOP;
                if (bottom)          return HTBOTTOM;
            }

            // 顶栏区域：原生拖动（HTCAPTION），零延迟，无需 JS RPC 往返。
            // 前端通过 window.setDragRegions RPC 注册按钮等交互元素的 bounding rect，
            // 命中这些区域时返回 HTCLIENT 让 WebView2 处理点击，其余区域返回 HTCAPTION。
            // 仅在 no_drag_regions_ 已注册后启用原生拖动，避免初始加载时按钮不可点击。
            if (pt.y >= 0 && pt.y <= topH) {
                bool hasRegions = false;
                bool inNoDrag = false;
                {
                    std::lock_guard<std::mutex> lk(self->drag_mu_);
                    hasRegions = !self->no_drag_regions_.empty();
                    if (hasRegions) {
                        for (const auto& r : self->no_drag_regions_) {
                            if (pt.x >= r.left && pt.x < r.right &&
                                pt.y >= r.top && pt.y < r.bottom) {
                                inNoDrag = true;
                                break;
                            }
                        }
                    }
                }
                if (hasRegions && !inNoDrag) return HTCAPTION;
            }

            // 非顶栏区域、命中不可拖动区域、或 no_drag_regions_ 尚未注册：
            // 交由 WebView2 默认处理（前端 JS mousedown → window.startDrag 作为回退）
            return DefSubclassProc(hWnd, uMsg, wParam, lParam);
        }

        case WM_SETCURSOR: {
            // WM_SETCURSOR 的 lParam 低字 = WM_NCHITTEST 返回的命中码。
            // WebView2 子窗口 (Chrome_WidgetWin_1) 的类光标为 IDC_ARROW，
            // 会覆盖系统默认的 resize 光标。此处根据命中码显式设置对应系统光标，
            // 确保鼠标移到窗口边框时立即显示 SizeWE/SizeNS/SizeNWSE/SizeNESW。
            WORD ht = LOWORD(lParam);
            HCURSOR cur = nullptr;
            switch (ht) {
                case HTLEFT: case HTRIGHT:
                    cur = LoadCursorW(nullptr, IDC_SIZEWE); break;
                case HTTOP: case HTBOTTOM:
                    cur = LoadCursorW(nullptr, IDC_SIZENS); break;
                case HTTOPLEFT: case HTBOTTOMRIGHT:
                    cur = LoadCursorW(nullptr, IDC_SIZENWSE); break;
                case HTTOPRIGHT: case HTBOTTOMLEFT:
                    cur = LoadCursorW(nullptr, IDC_SIZENESW); break;
            }
            if (cur) {
                SetCursor(cur);
                return TRUE; // 已处理，阻止 WebView2 覆盖
            }
            return DefSubclassProc(hWnd, uMsg, wParam, lParam);
        }
    }
    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

std::wstring WebViewHost::HomeUrl() const {
    if (use_virtual_host_) {
        return L"https://" + std::wstring(kVirtualHost) + L"/index.html";
    }
    return web_server_.BaseUrl();
}

bool WebViewHost::IsWhitelistedUri(const std::wstring& uri) const {
    if (web_server_.IsOwnUri(uri)) return true;
    // 虚拟域名白名单
    if (uri.rfind(L"https://app.localhost/", 0) == 0) return true;
    // 保留对 localhost 的兼容白名单（开发期可手动访问调试）
    if (IsLocalhostUri(uri)) return true;
    return false;
}

HRESULT WebViewHost::RegisterEmbeddedResourceHandler() {
    web_embedded_ = (FindResourceW(nullptr, L"\"index.html\"", RT_RCDATA) != nullptr);
    if (!web_embedded_ || !env_) return S_OK; // 未嵌入（旧构建/开发期），回退磁盘 webapp 文件夹
    DragLog("[WebViewHost] 内嵌 webapp 资源可用，注册 WebResourceRequested");
    resHandler_ = Callback<ICoreWebView2WebResourceRequestedEventHandler>(
        [this](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
            ComPtr<ICoreWebView2WebResourceRequest> req;
            if (FAILED(args->get_Request(&req)) || !req) return S_OK;
            LPWSTR uriPtr = nullptr;
            if (FAILED(req->get_Uri(&uriPtr))) return S_OK;
            std::wstring uri = uriPtr ? uriPtr : L"";
            CoTaskMemFree(uriPtr);
            std::wstring raw = WebRelPathFromUri(uri);
            std::wstring rel = WebPercentDecode(raw);
            // 路径遍历防护：拒绝 .. 段（资源名不允许这类路径）
            if (rel.find(L"..") != std::wstring::npos) return S_OK;
            std::vector<char> data;
            if (!EmbeddedWebFileGet(rel, data)) return S_OK; // 未嵌入此文件 → 放行（磁盘 webapp）
            HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, data.empty() ? 1 : data.size());
            if (!hg) return S_OK;
            {
                void* dst = GlobalLock(hg);
                if (dst) {
                    if (!data.empty()) memcpy(dst, data.data(), data.size());
                    GlobalUnlock(hg);
                }
            }
            ComPtr<IStream> stream;
            if (FAILED(CreateStreamOnHGlobal(hg, TRUE, &stream))) { GlobalFree(hg); return S_OK; }
            std::wstring mime = lxe::Utf8ToWide(WebMimeForPath(rel));
            std::wstring headers = L"Content-Type: " + mime + L"\r\nCache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\n";
            ComPtr<ICoreWebView2WebResourceResponse> resp;
            if (FAILED(env_->CreateWebResourceResponse(stream.Get(), 200, L"OK", headers.c_str(), &resp)))
                return S_OK;
            return args->put_Response(resp.Get());
        });
    HRESULT hr = webview_->add_WebResourceRequested(resHandler_.Get(), nullptr);
    if (FAILED(hr)) return hr;
    // 覆盖两种加载模式：内置 HTTP 服务器（随机端口）与虚拟域名 app.localhost
    std::wstring base = web_server_.BaseUrl();
    if (!base.empty()) webview_->AddWebResourceRequestedFilter((base + L"*").c_str(), COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
    webview_->AddWebResourceRequestedFilter((L"https://" + std::wstring(kVirtualHost) + L"/*").c_str(), COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
    return S_OK;
}

HRESULT WebViewHost::RegisterMessageHandler() {
    messageHandler_ = Callback<ICoreWebView2WebMessageReceivedEventHandler>(
        [this](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
            // 前端使用 chrome.webview.postMessage(JSON.stringify(...)) 发送字符串，
            // 必须用 TryGetWebMessageAsString 取得原字符串（ICoreWebView2WebMessageReceivedEventArgs2）；
            // 若用 get_WebMessageAsJson 会被二次 JSON 编码为字符串字面量，
            // 导致 Bridge::HandleMessage 解析得到 JSON 字符串而非对象，所有 RPC 失效。
            std::wstring w;
            ComPtr<ICoreWebView2WebMessageReceivedEventArgs2> args2;
            LPWSTR ptr = nullptr;
            if (SUCCEEDED(args->QueryInterface(IID_PPV_ARGS(&args2))) &&
                SUCCEEDED(args2->TryGetWebMessageAsString(&ptr)) && ptr) {
                w = ptr;
                CoTaskMemFree(ptr);
            } else {
                // 回退：JSON 编码的字符串（前后带引号），需要去掉外层引号并反转义
                LPWSTR jsonPtr = nullptr;
                if (FAILED(args->get_WebMessageAsJson(&jsonPtr))) return E_FAIL;
                std::wstring raw = jsonPtr ? jsonPtr : L"";
                CoTaskMemFree(jsonPtr);
                // 简单处理：若是 "..." 形式，剥掉外层引号并反转义常见字符
                if (raw.size() >= 2 && raw.front() == L'"' && raw.back() == L'"') {
                    std::wstring s;
                    s.reserve(raw.size());
                    for (size_t i = 1; i + 1 < raw.size(); ++i) {
                        wchar_t c = raw[i];
                        if (c == L'\\' && i + 1 < raw.size() - 1) {
                            wchar_t e = raw[++i];
                            switch (e) {
                                case L'"': s.push_back(L'"'); break;
                                case L'\\': s.push_back(L'\\'); break;
                                case L'/': s.push_back(L'/'); break;
                                case L'b': s.push_back(L'\b'); break;
                                case L'f': s.push_back(L'\f'); break;
                                case L'n': s.push_back(L'\n'); break;
                                case L'r': s.push_back(L'\r'); break;
                                case L't': s.push_back(L'\t'); break;
                                case L'u': {
                                    if (i + 4 < raw.size() - 1) {
                                        int v = 0;
                                        for (int k = 1; k <= 4; ++k) {
                                            wchar_t h = raw[i + k];
                                            v <<= 4;
                                            if (h >= L'0' && h <= L'9') v |= h - L'0';
                                            else if (h >= L'a' && h <= L'f') v |= h - L'a' + 10;
                                            else if (h >= L'A' && h <= L'F') v |= h - L'A' + 10;
                                        }
                                        i += 4;
                                        if (v < 0x10000) s.push_back(static_cast<wchar_t>(v));
                                    }
                                    break;
                                }
                                default: s.push_back(e);
                            }
                        } else {
                            s.push_back(c);
                        }
                    }
                    w = s;
                } else {
                    w = raw;
                }
            }
            bridge_.HandleMessage(lxe::WideToUtf8(w));
            return S_OK;
        });
    return webview_->add_WebMessageReceived(messageHandler_.Get(), nullptr);
}

HRESULT WebViewHost::RegisterNavigationHandlers() {
    navHandler_ = Callback<ICoreWebView2NavigationStartingEventHandler>(
        [this](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
            LPWSTR uriPtr = nullptr;
            args->get_Uri(&uriPtr);
            std::wstring uri = uriPtr ? uriPtr : L"";
            CoTaskMemFree(uriPtr);
            if (!IsWhitelistedUri(uri)) {
                args->put_Cancel(TRUE);
            }
            return S_OK;
        });
    HRESULT hr = webview_->add_NavigationStarting(navHandler_.Get(), nullptr);
    if (FAILED(hr)) return hr;

    windowHandler_ = Callback<ICoreWebView2NewWindowRequestedEventHandler>(
        [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
            args->put_Handled(TRUE);
            LPWSTR uriPtr = nullptr;
            args->get_Uri(&uriPtr);
            if (uriPtr) {
                ShellExecuteW(nullptr, L"open", uriPtr, nullptr, nullptr, SW_SHOWNORMAL);
                CoTaskMemFree(uriPtr);
            }
            return S_OK;
        });
    return webview_->add_NewWindowRequested(windowHandler_.Get(), nullptr);
}

void WebViewHost::RegisterNavCompletedHandler() {
    // 导航完成后（无论虚拟域名/HTTP 模式）：
    // 1) 虚拟域名模式首次导航失败时自动回退到 HTTP；
    // 2) 导航成功时补装 WebView2 子窗口子类化 + 重新注册子窗口拖放目标。
    //    这是拖放可靠性的关键：SetupWebView 时子窗口可能尚未就绪，若跳过子窗口的
    //    RevokeDragDrop/RegisterDragDrop，WebView2 内置拖放目标会拦截所有拖入。
    navCompletedHandler_ = Callback<ICoreWebView2NavigationCompletedEventHandler>(
        [this](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
            BOOL success = FALSE;
            args->get_IsSuccess(&success);
            if (success) {
                DragLog("[NavCompleted] 导航成功，补装子类化 + 重新注册子窗口拖放目标");
                InstallWebViewSubclass(true);
                if (!drop_registered_) RegisterNativeDropTarget();
                // 浏览器进程窗口此时已存在：把拖放目标注册到光标实际命中的最顶层窗口
                EnsureTopmostDropTarget();
            } else if (use_virtual_host_ && !nav_fallback_fired_) {
                // 虚拟域名导航失败，回退到 HTTP（仅触发一次）
                nav_fallback_fired_ = true;
                use_virtual_host_ = false;
                std::wstring home = HomeUrl();
                if (!home.empty()) {
                    // 附加 #fallback=vh 标记，前端检测到后显示提示
                    home += L"#fallback=vh";
                    webview_->Navigate(home.c_str());
                }
            }
            return S_OK;
        });
    webview_->add_NavigationCompleted(navCompletedHandler_.Get(), nullptr);
}

void WebViewHost::OnFallbackRequested() {
    // 后台预检线程发现文件不可读 → UI 线程执行回退
    // 如果 NavigationCompleted 已经成功回退或导航本身已成功，则跳过
    if (!use_virtual_host_ || nav_fallback_fired_) return;
    nav_fallback_fired_ = true;
    use_virtual_host_ = false;
    std::wstring home = HomeUrl();
    if (!home.empty()) {
        home += L"#fallback=vh";
        webview_->Navigate(home.c_str());
    }
}

void WebViewHost::Resize() {
    if (!controller_) return;
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    controller_->put_Bounds(rc);

    // 每次 Resize（窗口尺寸变化/缩放结束）都重新核对子窗口子类化 + 拖放目标注册。
    // 幂等：子窗口句柄未变时直接复用，句柄变化（WebView2 重建窗口）时自动迁移，
    // 确保任何时刻拖放目标都落在 WebView2 子窗口上。
    if (initialized_) {
        InstallWebViewSubclass();
        // 光标命中窗口变化时（浏览器进程窗口重建/层级调整）重新注册拖放目标
        EnsureTopmostDropTarget();
    }
}

void WebViewHost::DispatchPendingMessages() {
    if (!webview_ || !initialized_) return;
    // 取出队首批处理（期间新入队的在下一轮 flush），避免长时间持有锁阻塞后台线程
    for (;;) {
        std::string msg;
        {
            std::lock_guard<std::mutex> lk(queue_mu_);
            if (pending_queue_.empty()) break;
            msg = std::move(pending_queue_.front());
            pending_queue_.pop_front();
        }
        std::wstring w = lxe::Utf8ToWide(msg);
        // 忽略单条失败，继续推送后续消息
        HRESULT hr = webview_->PostWebMessageAsJson(w.c_str());
        if (FAILED(hr)) {
            // 记录错误但不阻塞：WebView2 可能在导航/初始化期间临时不可用
        }
    }
    flush_pending_.store(false, std::memory_order_release);
}

void WebViewHost::SetNoDragRegions(const std::vector<RECT>& rects) {
    std::lock_guard<std::mutex> lk(drag_mu_);
    no_drag_regions_ = rects;
}

bool WebViewHost::HasNoDragRegions() const {
    std::lock_guard<std::mutex> lk(drag_mu_);
    return !no_drag_regions_.empty();
}

void WebViewHost::PostToWeb(const std::string& utf8Json) {
    if (!webview_ || !initialized_) return;
    if (ui_thread_id_ != 0 && GetCurrentThreadId() == ui_thread_id_) {
        // 已经在 UI 线程：直接投递（同时 flush 可能已入队的残留）
        {
            std::lock_guard<std::mutex> lk(queue_mu_);
            pending_queue_.push_back(utf8Json);
        }
        DispatchPendingMessages();
        return;
    }
    // 跨线程：入队 + PostMessage 通知 UI 线程 flush
    {
        std::lock_guard<std::mutex> lk(queue_mu_);
        pending_queue_.push_back(utf8Json);
    }
    bool expected = false;
    if (flush_pending_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        // 只有没挂起时才 Post，避免消息泵被 WM_FLUSH_POST_JSON 刷屏
        if (hwnd_) PostMessageW(hwnd_, WM_FLUSH_POST_JSON, 0, 0);
    }
}
