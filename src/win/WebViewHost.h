#pragma once

// 必须在 <windows.h> 之前包含，以避免 winsock.h / winsock2.h 冲突
#include "WebServer.h"

#include <windows.h>
#include <wrl.h>
#include <WebView2.h>

#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "../bridge/Bridge.h"

// 自定义窗口消息：非 UI 线程 PostMessage 到主窗口，触发 WebView2 待发消息队列 flush
// 必须和 WebViewHost.cpp 内部使用的值保持一致；LXElauncher.cpp WndProc 处理此消息。
static constexpr UINT WM_WEBVIEW_FLUSH_JSON = WM_USER + 133;
// 后台预检线程发现虚拟域名不可用时，PostMessage 此消息到主窗口，
// 由 UI 线程执行 Navigate 回退到 HTTP（WebView2 不允许跨线程调用）。
static constexpr UINT WM_WEBVIEW_FALLBACK = WM_USER + 134;

class WebDropTarget;  // 系统拖放目标（接收拖入文件真实路径），实现位于 WebViewHost.cpp

class WebViewHost {
public:
    WebViewHost() = default;
    ~WebViewHost();

    WebViewHost(const WebViewHost&) = delete;
    WebViewHost& operator=(const WebViewHost&) = delete;

    bool Initialize(HWND parent);
    void Resize();
    lxe::Bridge& Bridge() { return bridge_; }

    // UI 线程调用：flush 跨线程入队的 WebMessage 并真实推送给 WebView2。
    // 在 WndProc 收到 WM_WEBVIEW_FLUSH_JSON 时调用。
    void DispatchPendingMessages();

    // UI 线程调用：后台预检线程发现虚拟域名不可用时通过 PostMessage 触发，
    // 由 UI 线程执行 Navigate 回退到 HTTP。在 WndProc 收到 WM_WEBVIEW_FALLBACK 时调用。
    void OnFallbackRequested();

    // 设置顶栏中不可拖动的区域（按钮等交互元素的 bounding rect，物理像素，客户区坐标）。
    // WM_NCHITTEST 中，顶栏区域内若命中这些 rect 则返回 HTCLIENT（让 WebView2 处理点击），
    // 否则返回 HTCAPTION（原生拖动，零延迟）。
    void SetNoDragRegions(const std::vector<RECT>& rects);
    bool HasNoDragRegions() const;

    // 注册/注销系统拖放目标（智能拖放：通过 IDataObject 读取拖入文件真实路径，
    // 以 nativeDrop / nativeDragEnter / nativeDragLeave 事件转发给前端）。
    void RegisterNativeDropTarget();

private:
    HRESULT SetupWebView();
    HRESULT RegisterMessageHandler();
    HRESULT RegisterNavigationHandlers();
    void RegisterNavCompletedHandler();
    // 内嵌 webapp 资源（WebApp.rc RCDATA）：注册 WebResourceRequested 从内存返回
    // 页面文件，使前端作为 exe 内嵌资源打包（无需 webapp 磁盘目录）。
    HRESULT RegisterEmbeddedResourceHandler();
    // 线程安全：非 UI 线程调用会 PostMessage 到主窗口排队，避免
    // "API cannot run on different threads" / RPC_E_WRONG_THREAD 错误。
    void PostToWeb(const std::string& utf8Json);

    // 返回当前内置服务器的基础 URL（形如 "http://127.0.0.1:51837/"），
    // 服务器未启动时返回空串。
    std::wstring HomeUrl() const;
    // 判断 URL 是否允许导航（仅允许内置服务器与已有的 localhost 白名单）
    bool IsWhitelistedUri(const std::wstring& uri) const;

    // 在主窗口下寻找 WebView2 创建的浏览器子 HWND，并对其安装子类化。
    // 子类化后，子 HWND 的 WM_NCHITTEST 在窗口边缘返回 HTLEFT/HTTOP 等（允许缩放），
    // 顶栏区域返回 HTCLIENT（拖动由前端 JS mousedown → window.startDrag RPC 处理）；
    // WM_LBUTTONDBLCLK 触发顶栏双击最大化切换。
    // forceDropReapply=true 时，无论是否已注册都重新撤销并注册子窗口的 OLE 拖放目标
    // （导航完成后调用，防止 WebView2 重新注册内置目标覆盖我们的接管）。
    void InstallWebViewSubclass(bool forceDropReapply = false);
    void UninstallWebViewSubclass();
    // 把 OLE 拖放目标注册到光标实际命中的最顶层窗口上。
    // 关键：OLE 拖放命中测试用 WindowFromPoint 找光标下的窗口且不向父链查找注册目标；
    // WebView2 的浏览器进程（msedgewebview2）会创建覆盖整个客户区的顶层可见窗口
    // （Chrome_WidgetWin_1 / Chrome_RenderWidgetHostHWND），若只注册到我们进程的
    // 主窗口/宿主窗口，拖入会被浏览器进程窗口拦截，表现为"禁止光标 + 无反应"。
    // RegisterDragDrop 的注册表是全局窗口属性，因此可以跨进程注册到浏览器进程窗口上。
    void EnsureTopmostDropTarget();
    static LRESULT CALLBACK WebViewSubclassProc(
        HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
        UINT_PTR uIdSubclass, DWORD_PTR dwRefData);

    HWND hwnd_ = nullptr;
    HWND webview_child_hwnd_ = nullptr;  // WebView2 内部子窗口句柄
    DWORD ui_thread_id_ = 0;
    std::atomic<bool> use_virtual_host_{false};  // true: 虚拟域名映射; false: 内置 HTTP 服务器
    std::atomic<bool> nav_fallback_fired_{false}; // 虚拟域名→HTTP 回退是否已触发（仅首次有效，双线程原子）
    std::wstring webapp_path_;           // webapp 根目录（虚拟域名映射时使用）
    Microsoft::WRL::ComPtr<ICoreWebView2Environment> env_; // 环境（构造内存返回响应）
    std::mutex queue_mu_;
    std::deque<std::string> pending_queue_;
    std::atomic<bool> flush_pending_{false};
    mutable std::mutex drag_mu_;
    std::vector<RECT> no_drag_regions_;  // 顶栏不可拖动区域（物理像素，客户区坐标）
    // 成员按声明逆序析构：web_server_ 声明在前 ⇒ 最后析构，
    // 这样 WebView2 控制器/视图释放期间服务器仍可用。
    lxe::WebServer web_server_;
    Microsoft::WRL::ComPtr<ICoreWebView2Controller> controller_;
    Microsoft::WRL::ComPtr<ICoreWebView2> webview_;
    lxe::Bridge bridge_;
    Microsoft::WRL::ComPtr<ICoreWebView2WebMessageReceivedEventHandler> messageHandler_;
    Microsoft::WRL::ComPtr<ICoreWebView2NavigationStartingEventHandler> navHandler_;
    Microsoft::WRL::ComPtr<ICoreWebView2NavigationCompletedEventHandler> navCompletedHandler_;
    Microsoft::WRL::ComPtr<ICoreWebView2NewWindowRequestedEventHandler> windowHandler_;
    Microsoft::WRL::ComPtr<ICoreWebView2WebResourceRequestedEventHandler> resHandler_; // 内嵌 webapp 资源
    WebDropTarget* drop_target_ = nullptr;    // 系统拖放目标（生命周期由本类管理）
    bool drop_registered_ = false;            // 是否已 RegisterDragDrop（主窗口）
    bool child_drop_registered_ = false;      // 是否已接管 WebView2 子窗口的 OLE 拖放目标
    HWND drop_top_hwnd_ = nullptr;            // 光标实际命中的最顶层窗口（浏览器进程），拖放目标注册于此
    bool initialized_ = false;
    bool web_embedded_ = false;            // 内嵌 webapp 资源是否可用（WebApp.rc）
};
