// LXElauncher.cpp : 定义应用程序的入口点。
//

#include "framework.h"
#include "LXElauncher.h"

#include <windowsx.h>
#include <dwmapi.h>
#include <memory>
#include <string>

#include "src/bridge/Services.h"
#include "src/Json.h"
#include "src/win/WebViewHost.h"

#include <fstream>
#include <exception>
#include <cstdlib>

#pragma comment(lib, "dwmapi.lib")

#define MAX_LOADSTRING 100

// ========== 全局崩溃捕获：把未捕获异常 / 崩溃位置追加写入 logs\lxe-launcher-crash.log ==========
static void AppendCrashLog(const char* section, const char* detail) {
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring root(buf);
    auto pos = root.find_last_of(L"\\/");
    if (pos != std::wstring::npos) root = root.substr(0, pos);
    const std::wstring logPath = root + L"\\logs\\lxe-launcher-crash.log";
    HANDLE h = CreateFileW(logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        CreateDirectoryW((root + L"\\logs").c_str(), nullptr);
        h = CreateFileW(logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    if (h == INVALID_HANDLE_VALUE) return;
    std::string line = std::string("== ") + section + " ==";
    if (detail && *detail) { line += " "; line += detail; }
    line += "\r\n";
    DWORD written = 0;
    WriteFile(h, line.data(), (DWORD)line.size(), &written, nullptr);
    CloseHandle(h);
}

static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
    char msg[512];
    if (ep && ep->ExceptionRecord) {
        wsprintfA(msg, "0x%08X @ 0x%llX", ep->ExceptionRecord->ExceptionCode,
                  (unsigned long long)ep->ExceptionRecord->ExceptionAddress);
    } else {
        msg[0] = '\0';
    }
    AppendCrashLog("CRASH", msg);
    return EXCEPTION_CONTINUE_SEARCH;
}

static void TerminateHandler() {
    AppendCrashLog("TERMINATE", "未捕获 C++ 异常逃逸到线程边界（std::terminate -> abort）");
    abort();
}

// 无边框窗口缩放边框宽度（像素）
constexpr int kResizeBorder = 6;
// 窗口边角缩放判定范围（像素）：边角比普通边更大，便于鼠标抓取拖拽缩放
constexpr int kResizeCorner = 18;

// ========== 窗口位置持久化（HKCU\Software\LXElauncher\WindowPlacement） ==========
static const wchar_t kRegSubKey[] = L"Software\\LXElauncher";

static bool LoadWindowPlacementFromRegistry(HWND hWnd, int& outX, int& outY, int& outW, int& outH, bool& outMaximized) {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegSubKey, 0, KEY_READ, &hKey) != ERROR_SUCCESS) return false;
    struct Closer { HKEY k; ~Closer() { if (k) RegCloseKey(k); } } _{ hKey };
    DWORD type = 0, size = 0;
    WINDOWPLACEMENT wp{}; wp.length = sizeof(wp);
    size = sizeof(wp);
    if (RegQueryValueExW(hKey, L"WindowPlacement", nullptr, &type, reinterpret_cast<LPBYTE>(&wp), &size) != ERROR_SUCCESS) return false;
    if (type != REG_BINARY || size != sizeof(wp)) return false;
    if (wp.showCmd == SW_SHOWMAXIMIZED) outMaximized = true;
    RECT r = wp.rcNormalPosition;
    outX = r.left; outY = r.top; outW = r.right - r.left; outH = r.bottom - r.top;
    // 简单校验：确保不小于最小尺寸
    if (outW < 800) outW = 1024;
    if (outH < 560) outH = 720;
    return true;
}
static void SaveWindowPlacementToRegistry(HWND hWnd) {
    HKEY hKey = nullptr;
    DWORD disp = 0;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegSubKey, 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, &disp) != ERROR_SUCCESS) return;
    struct Closer { HKEY k; ~Closer() { if (k) RegCloseKey(k); } } _{ hKey };
    WINDOWPLACEMENT wp{}; wp.length = sizeof(wp);
    GetWindowPlacement(hWnd, &wp);
    RegSetValueExW(hKey, L"WindowPlacement", 0, REG_BINARY, reinterpret_cast<const BYTE*>(&wp), sizeof(wp));
}

// ============ Acrylic / 毛玻璃 ============
enum ACCENT_STATE {
    ACCENT_DISABLED = 0,
    ACCENT_ENABLE_GRADIENT = 1,
    ACCENT_ENABLE_TRANSPARENTGRADIENT = 2,
    ACCENT_ENABLE_BLURBEHIND = 3,
    ACCENT_ENABLE_ACRYLICBLURBEHIND = 4,
    ACCENT_INVALID_STATE = 5
};

struct ACCENTPOLICY {
    int nAccentState;
    int nFlags;
    int nColor; // 0xAABBGGRR
    int nAnimationId;
};

struct WINCOMPATTRDATA {
    int nAttribute;
    PVOID pData;
    ULONG ulDataSize;
};

// SetWindowCompositionAttribute (user32.dll, 未公开但 Win10+)
using FnSetWindowCompositionAttribute = BOOL(WINAPI*)(HWND, WINCOMPATTRDATA*);
static FnSetWindowCompositionAttribute g_pSetWindowCompositionAttribute = nullptr;

void EnableAcrylic(HWND hwnd, BYTE r, BYTE g, BYTE b, BYTE alpha) {
    if (!g_pSetWindowCompositionAttribute) {
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        if (user32) {
            g_pSetWindowCompositionAttribute =
                reinterpret_cast<FnSetWindowCompositionAttribute>(
                    GetProcAddress(user32, "SetWindowCompositionAttribute"));
        }
    }
    if (!g_pSetWindowCompositionAttribute) return;

    ACCENTPOLICY policy{};
    policy.nAccentState = ACCENT_ENABLE_ACRYLICBLURBEHIND;
    // 0x20=Left,0x40=Right,0x80=Invalid | 全窗口亚克力统一值 0xE0
    policy.nFlags = 0xE0;
    policy.nColor = (alpha << 24) | (b << 16) | (g << 8) | r; // 0xAABBGGRR
    policy.nAnimationId = 0;

    WINCOMPATTRDATA data{};
    data.nAttribute = 19; // WCA_ACCENT_POLICY
    data.pData = &policy;
    data.ulDataSize = sizeof(policy);
    g_pSetWindowCompositionAttribute(hwnd, &data);
}

// 拖动/缩放期间临时关闭毛玻璃：DWM 不再实时抓取并模糊背景，CPU/GPU 占用骤降。
// 释放鼠标后由 WM_EXITSIZEMOVE 调用 EnableAcrylic 恢复。
void DisableAcrylic(HWND hwnd) {
    if (!g_pSetWindowCompositionAttribute) {
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        if (user32) {
            g_pSetWindowCompositionAttribute =
                reinterpret_cast<FnSetWindowCompositionAttribute>(
                    GetProcAddress(user32, "SetWindowCompositionAttribute"));
        }
    }
    if (!g_pSetWindowCompositionAttribute) return;

    ACCENTPOLICY policy{};
    policy.nAccentState = ACCENT_DISABLED; // 完全移除毛玻璃
    WINCOMPATTRDATA data{};
    data.nAttribute = 19; // WCA_ACCENT_POLICY
    data.pData = &policy;
    data.ulDataSize = sizeof(policy);
    g_pSetWindowCompositionAttribute(hwnd, &data);
}

// 全局变量:
HINSTANCE hInst;                                // 当前实例
WCHAR szTitle[MAX_LOADSTRING];                  // 标题栏文本
WCHAR szWindowClass[MAX_LOADSTRING];            // 主窗口类名

std::unique_ptr<WebViewHost> g_webviewHost;
static bool g_isResizing = false;

// 此代码模块中包含的函数的前向声明:
ATOM                MyRegisterClass(HINSTANCE hInstance);
BOOL                InitInstance(HINSTANCE, int);
LRESULT CALLBACK    WndProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK    About(HWND, UINT, WPARAM, LPARAM);

// 窗口置顶开关状态（默认关闭）。RPC 写入，启动时从 settings.json 读取。
static bool g_topmost = false;

// 从 exe 同级的 settings.json 读取布尔键（用于启动早期、窗口创建前的判断，如自适应 DPI）
static bool ReadSettingsBool(const char* key, bool def) {
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buf, ARRAYSIZE(buf));
    std::wstring exe = buf;
    auto pos = exe.find_last_of(L"\\/");
    std::wstring root = (pos != std::wstring::npos) ? exe.substr(0, pos) : exe;
    std::ifstream sf(root + L"\\settings.json", std::ios::binary);
    if (sf) {
        try {
            std::string content((std::istreambuf_iterator<char>(sf)), std::istreambuf_iterator<char>());
            if (!content.empty()) {
                lxe::Json j = lxe::Json::parse(content);
                if (j.isObject() && j.contains(key) && j.at(key).isBool()) {
                    return j.at(key).asBool();
                }
            }
        } catch (...) {}
    }
    return def;
}

// 窗口置顶：top=true 置顶（HWND_TOPMOST），false 取消置顶。SWP_NOACTIVATE 避免抢焦点。
static void SetWindowTopmost(HWND hwnd, bool top) {
    if (!hwnd) return;
    SetWindowPos(hwnd, top ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
                     _In_opt_ HINSTANCE hPrevInstance,
                     _In_ LPWSTR    lpCmdLine,
                     _In_ int       nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

    // 全局崩溃捕获：未捕获异常 / 原生崩溃写入 logs\lxe-launcher-crash.log
    SetUnhandledExceptionFilter(CrashFilter);
    std::set_terminate(TerminateHandler);

    // 自适应 DPI 开关：只决定是否启用下方第 157 行的 Per-Monitor DPI Awareness V2。
    // ON = 调用 SetThreadDpiAwarenessContext(-4)（原生分辨率渲染，避免高分屏模糊）；
    // OFF = 不调用（系统按默认 DPI 缩放界面，文字可能模糊但更大）。
    // 必须在创建窗口之前调用，故这里提前读取 settings.json；运行期切换仅持久化、下次启动生效。
    const bool adaptiveDpi = ReadSettingsBool("adaptiveDpi", true);

    // Per-Monitor DPI Awareness V2 — 修复 WebView2 在高分屏下渲染模糊
    // 详见 win32webview模糊解决方案.md 方案一
    // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 的原始值为 -4
    {
        auto pSetThreadDpiAwarenessContext = reinterpret_cast<INT_PTR(WINAPI*)(INT_PTR)>(
            GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext"));
        if (adaptiveDpi && pSetThreadDpiAwarenessContext) pSetThreadDpiAwarenessContext(-4);
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    // OLE 初始化：主窗口需注册 IDropTarget 以接收系统拖放（智能拖放功能）
    OleInitialize(nullptr);

    // 初始化全局字符串
    LoadStringW(hInstance, IDS_APP_TITLE, szTitle, MAX_LOADSTRING);
    LoadStringW(hInstance, IDC_LXELAUNCHER, szWindowClass, MAX_LOADSTRING);
    MyRegisterClass(hInstance);

    // 执行应用程序初始化:
    if (!InitInstance (hInstance, nCmdShow))
    {
        return FALSE;
    }

    HACCEL hAccelTable = LoadAccelerators(hInstance, MAKEINTRESOURCE(IDC_LXELAUNCHER));

    MSG msg;

    // 主消息循环:
    while (GetMessage(&msg, nullptr, 0, 0))
    {
        if (!TranslateAccelerator(msg.hwnd, hAccelTable, &msg))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    // 关闭序列：先停止后台服务线程（游戏监控等），再关闭 Bridge 事件推送，
    // 最后销毁 WebViewHost。若跳过该步骤，退出时 joinable 线程或残留线程
    // 会继续调用 Bridge::PostEvent，导致 use-after-free / "abort has been called"。
    lxe::ShutdownServices();
    if (g_webviewHost) g_webviewHost->Bridge().Close();
    g_webviewHost.reset();
    OleUninitialize();
    CoUninitialize();
    return (int) msg.wParam;
}



//
//  函数: MyRegisterClass()
//
//  目标: 注册窗口类。
//
ATOM MyRegisterClass(HINSTANCE hInstance)
{
    WNDCLASSEXW wcex;

    wcex.cbSize = sizeof(WNDCLASSEX);

    wcex.style          = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc    = WndProc;
    wcex.cbClsExtra     = 0;
    wcex.cbWndExtra     = 0;
    wcex.hInstance      = hInstance;
    wcex.hIcon          = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_LXELAUNCHER));
    wcex.hCursor        = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground  = (HBRUSH)(COLOR_WINDOW+1);
    wcex.lpszMenuName   = MAKEINTRESOURCEW(IDC_LXELAUNCHER);
    wcex.lpszClassName  = szWindowClass;
    wcex.hIconSm        = LoadIcon(wcex.hInstance, MAKEINTRESOURCE(IDI_SMALL));

    return RegisterClassExW(&wcex);
}

//
//   函数: InitInstance(HINSTANCE, int)
//
//   目标: 保存实例句柄并创建主窗口
//
//   注释:
//
//        在此函数中，我们在全局变量中保存实例句柄并
//        创建和显示主程序窗口。
//
BOOL InitInstance(HINSTANCE hInstance, int nCmdShow)
{
   hInst = hInstance; // 将实例句柄存储在全局变量中

   // 先尝试从注册表恢复上次窗口位置/大小；失败时使用默认值
   int px = CW_USEDEFAULT, py = 0, pw = 1024, ph = 720;
   bool maximized = false;
   LoadWindowPlacementFromRegistry(nullptr, px, py, pw, ph, maximized);

   HWND hWnd = CreateWindowW(szWindowClass, szTitle,
      WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU |
      WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
      px, py, pw, ph, nullptr, nullptr, hInstance, nullptr);

   if (!hWnd)
   {
      return FALSE; 
   }

   // 如果上次是最大化，ShowWindow 用 SW_SHOWMAXIMIZED
   int showCmd = nCmdShow;
   if (maximized) showCmd = SW_SHOWMAXIMIZED;

   // 为无边框窗口保留 DWM 阴影
   MARGINS margins{ 1, 1, 1, 1 };
   DwmExtendFrameIntoClientArea(hWnd, &margins);

   // 毛玻璃亚克力（Win10+），色调取半透明深蓝：让 CSS 渐变/背景图盖在上面形成磨砂叠加。
   // 由"开发者模式-亚克力"开关控制（settings.json 的 "acrylic" 键），默认开，随后会从 settings.json 覆盖。
   lxe::SetAcrylicEnabled(true);
   lxe::ApplyWindowAcrylic(hWnd);

   g_webviewHost = std::make_unique<WebViewHost>();
   // 设置游戏版本清单路径（temp\version_manifest.json）+ exe 同级目录
   {
       wchar_t buf[MAX_PATH]{};
       GetModuleFileNameW(nullptr, buf, ARRAYSIZE(buf));
       std::wstring exe = buf;
       auto pos = exe.find_last_of(L"\\/");
       std::wstring root = (pos != std::wstring::npos) ? exe.substr(0, pos) : exe;
       lxe::SetExeDir(root);
       // 从 exe 目录向上定位到项目根，再拼接 temp\version_manifest.json
       std::wstring manifest = root + L"\\..\\..\\temp\\version_manifest.json";
       DWORD attr = GetFileAttributesW(manifest.c_str());
       if (attr == INVALID_FILE_ATTRIBUTES) manifest = root + L"\\temp\\version_manifest.json";
       attr = GetFileAttributesW(manifest.c_str());
       if (attr == INVALID_FILE_ATTRIBUTES) {
           // 再尝试项目工作目录
           manifest = L"d:\\vsc项目\\LXElauncher\\LXElauncher\\temp\\version_manifest.json";
       }
       lxe::SetManifestPath(manifest);
   }
   lxe::RegisterDemoServices(g_webviewHost->Bridge());
   lxe::RegisterWindowServices(g_webviewHost->Bridge(), hWnd);

   // 窗口圆角：从 settings.json 读取上次的档位（默认 medium），并立即应用到 HWND
   {
       wchar_t buf[MAX_PATH]{};
       GetModuleFileNameW(nullptr, buf, ARRAYSIZE(buf));
       std::wstring exe = buf;
       auto pos = exe.find_last_of(L"\\/");
       std::wstring root = (pos != std::wstring::npos) ? exe.substr(0, pos) : exe;
       std::wstring setPath = root + L"\\settings.json";
       std::ifstream sf(setPath, std::ios::binary);
       if (sf) {
           try {
               std::string content((std::istreambuf_iterator<char>(sf)),
                                    std::istreambuf_iterator<char>());
                 if (!content.empty()) {
                    lxe::Json j = lxe::Json::parse(content);
                    if (j.isObject() && j.contains("windowCorner") && j.at("windowCorner").isString()) {
                        std::string m = j.at("windowCorner").asString();
                        if (m == "none" || m == "small" || m == "medium" || m == "large" || m == "custom") {
                            lxe::SetWindowCornerMode(m);
                        }
                    }
                    // 自定义圆角半径（px），仅当档位为 custom 时生效
                    if (j.isObject() && j.contains("windowCornerRadius") && j.at("windowCornerRadius").isNumber()) {
                        int r = static_cast<int>(j.at("windowCornerRadius").asNumber());
                        lxe::SetWindowCornerRadius(r);
                    }
                    // 亚克力毛玻璃开关（默认开）；用户可在"开发者模式"中关闭
                    if (j.isObject() && j.contains("acrylic") && j.at("acrylic").isBool()) {
                        lxe::SetAcrylicEnabled(j.at("acrylic").asBool());
                    }
                    // 窗口置顶开关（默认关闭）：启动时读取并应用 HWND_TOPMOST
                    if (j.isObject() && j.contains("alwaysOnTop") && j.at("alwaysOnTop").isBool()) {
                        g_topmost = j.at("alwaysOnTop").asBool();
                    }
                }
            } catch (...) {}
        }
        // 应用圆角（4 档：DWM 档位 + large 档额外 SetWindowRgn 裁剪）
        lxe::ApplyWindowCorner(hWnd);
        // 应用亚克力（依据上面读出的开关状态）
        lxe::ApplyWindowAcrylic(hWnd);
        // 应用窗口置顶
        SetWindowTopmost(hWnd, g_topmost);
    }

   // 原生拖动支持：前端通过此 RPC 注册顶栏中按钮等交互元素的 bounding rect，
   // WM_NCHITTEST 命中这些区域时返回 HTCLIENT（允许点击），其余顶栏区域返回 HTCAPTION（原生拖动）。
   {
       WebViewHost* host = g_webviewHost.get();
       g_webviewHost->Bridge().Register("window.setDragRegions", [host](const lxe::Json& params) {
           lxe::Bridge::HandlerResult r;
           r.ok = true;
           r.result = lxe::Json::object();
           if (params.isObject() && params.contains("regions") && params.at("regions").isArray()) {
               std::vector<RECT> regions;
               const auto& arr = params.at("regions").asArray();
               regions.reserve(arr.size());
               for (const auto& item : arr) {
                   if (!item.isObject()) continue;
                   RECT rc{};
                   if (item.contains("x")) rc.left = static_cast<LONG>(item.at("x").asNumber());
                   if (item.contains("y")) rc.top = static_cast<LONG>(item.at("y").asNumber());
                   if (item.contains("w")) rc.right = rc.left + static_cast<LONG>(item.at("w").asNumber());
                   if (item.contains("h")) rc.bottom = rc.top + static_cast<LONG>(item.at("h").asNumber());
                   regions.push_back(rc);
               }
                host->SetNoDragRegions(regions);
            }
            return r;
        });
    }

   // 窗口置顶开关：true = HWND_TOPMOST，false = 取消置顶。默认关闭。
   // 状态持久化由前端 Persisted（settings.set）负责，下次启动时读取并应用。
   {
       g_webviewHost->Bridge().Register("window.setTopmost", [hWnd](const lxe::Json& params) {
           lxe::Bridge::HandlerResult r;
           r.ok = true;
           r.result = lxe::Json::object();
           bool top = false;
           if (params.isObject() && params.contains("top") && params.at("top").isBool()) {
               top = params.at("top").asBool();
           } else if (params.isBool()) {
               top = params.asBool();
           }
           g_topmost = top;
           SetWindowTopmost(hWnd, top);
           r.result["top"] = top;
           return r;
       });
       g_webviewHost->Bridge().Register("window.getTopmost", [hWnd](const lxe::Json&) {
           lxe::Bridge::HandlerResult r;
           r.ok = true;
           r.result = lxe::Json::object();
           r.result["top"] = g_topmost;
           return r;
       });
   }

    g_webviewHost->Initialize(hWnd);

   ShowWindow(hWnd, showCmd);
   UpdateWindow(hWnd);

   return TRUE;
}

//
//  函数: WndProc(HWND, UINT, WPARAM, LPARAM)
//
//  目标: 处理主窗口的消息。
//
//  WM_COMMAND  - 处理应用程序菜单
//  WM_DESTROY  - 发送退出消息并返回
//
//
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_COMMAND:
        {
            int wmId = LOWORD(wParam);
            // 分析菜单选择:
            switch (wmId)
            {
            case IDM_ABOUT:
                DialogBox(hInst, MAKEINTRESOURCE(IDD_ABOUTBOX), hWnd, About);
                break;
            case IDM_EXIT:
                DestroyWindow(hWnd);
                break;
            default:
                return DefWindowProc(hWnd, message, wParam, lParam);
            }
        }
        break;
    case WM_NCCALCSIZE:
        // 无边框窗口：把整个窗口区域都当作客户区
        if (wParam == TRUE) {
            NCCALCSIZE_PARAMS* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
            // 最大化时需要避开任务栏
            if (IsZoomed(hWnd)) {
                HMONITOR hMon = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
                MONITORINFO mi;
                mi.cbSize = sizeof(mi);
                if (GetMonitorInfoW(hMon, &mi)) {
                    params->rgrc[0] = mi.rcWork;
                }
            }
            return 0;
        }
        break;
    case WM_NCHITTEST:
        {
            // 无边框窗口边缘缩放检测
            if (IsZoomed(hWnd)) return HTCLIENT;

            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ScreenToClient(hWnd, &pt);
            RECT rc;
            GetClientRect(hWnd, &rc);
            // DPI 感知下按物理像素缩放边框宽度
            const UINT dpi = GetDpiForWindow(hWnd);
            const int b = MulDiv(kResizeBorder, dpi, 96);
            const int c = MulDiv(kResizeCorner, dpi, 96);

            // 边角判定范围比普通边更大（kResizeCorner），角落更易被抓取缩放
            bool cornerL = pt.x < c;
            bool cornerR = pt.x >= rc.right - c;
            bool cornerT = pt.y < c;
            bool cornerB = pt.y >= rc.bottom - c;

            if (cornerT && cornerL)     return HTTOPLEFT;
            if (cornerT && cornerR)    return HTTOPRIGHT;
            if (cornerB && cornerL)    return HTBOTTOMLEFT;
            if (cornerB && cornerR)    return HTBOTTOMRIGHT;

            bool left   = pt.x < b;
            bool right  = pt.x >= rc.right - b;
            bool top    = pt.y < b;
            bool bottom = pt.y >= rc.bottom - b;

            if (left)            return HTLEFT;
            if (right)           return HTRIGHT;
            if (top)             return HTTOP;
            if (bottom)          return HTBOTTOM;
            return HTCLIENT;
        }
        break;
    case WM_GETMINMAXINFO:
        {
            LPMINMAXINFO lpMMI = reinterpret_cast<LPMINMAXINFO>(lParam);
            // 最小窗口尺寸按 DPI 缩放
            const UINT dpi = GetDpiForWindow(hWnd);
            lpMMI->ptMinTrackSize.x = MulDiv(800, dpi, 96);
            lpMMI->ptMinTrackSize.y = MulDiv(560, dpi, 96);
        }
        break;
    case WM_DPICHANGED:
        // 跨显示器拖拽时按系统建议矩形调整窗口大小，保持 WebView2 清晰
        {
            LPRECT lprcRect = reinterpret_cast<LPRECT>(lParam);
            SetWindowPos(hWnd, nullptr, lprcRect->left, lprcRect->top,
                         lprcRect->right - lprcRect->left,
                         lprcRect->bottom - lprcRect->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        break;
    case WM_ENTERSIZEMOVE:
        g_isResizing = true;
        // 立即关闭毛玻璃：拖动/缩放期间 DWM 不再实时抓取并模糊背景画面，
        // CPU/GPU 占用从 20%+ 骤降至 5% 以内。释放鼠标后由 WM_EXITSIZEMOVE 恢复。
        DisableAcrylic(hWnd);
        // 通知前端进入拖动态：CSS 移除 backdrop-filter，进一步减轻合成器负载
        if (g_webviewHost) {
            lxe::Json d = lxe::Json::object();
            d["dragging"] = true;
            g_webviewHost->Bridge().PostEvent("window.dragging", d);
        }
        return 0;

    case WM_EXITSIZEMOVE:
        g_isResizing = false;
        // 恢复毛玻璃亚克力（仅当"开发者模式-亚克力"开关开启，色调与 InitInstance 中一致）
        lxe::ApplyWindowAcrylic(hWnd);
        if (g_webviewHost) {
            g_webviewHost->Resize();
            InvalidateRect(hWnd, nullptr, TRUE);
            // 通知前端退出拖动态：恢复 backdrop-filter
            lxe::Json d = lxe::Json::object();
            d["dragging"] = false;
            g_webviewHost->Bridge().PostEvent("window.dragging", d);
            // 窗口状态可能已改变（如拖到屏幕边缘吸附），推送最新状态
            lxe::Json state = lxe::Json::object();
            state["maximized"] = IsZoomed(hWnd) ? true : false;
            state["minimized"] = IsIconic(hWnd) ? true : false;
            g_webviewHost->Bridge().PostEvent("window.state", state);
        }
        return 0;

    case WM_SIZE:
        if (g_webviewHost) {
            // 调整中仅记录状态，不触发 WebView2 重绘；结束时统一在 WM_EXITSIZEMOVE 处理
            if (!g_isResizing) {
                g_webviewHost->Resize();
            }
            // 仅在窗口状态真正变化时推送事件，避免拖动期间（跨 DPI）无脑构造 JSON
            static bool lastMaximized = false;
            static bool lastMinimized = false;
            bool curMaximized = IsZoomed(hWnd) ? true : false;
            bool curMinimized = IsIconic(hWnd) ? true : false;
            if (wParam == SIZE_MAXIMIZED || wParam == SIZE_MINIMIZED || wParam == SIZE_RESTORED) {
                if (curMaximized != lastMaximized || curMinimized != lastMinimized) {
                    lastMaximized = curMaximized;
                    lastMinimized = curMinimized;
                    lxe::Json state = lxe::Json::object();
                    state["maximized"] = curMaximized;
                    state["minimized"] = curMinimized;
                    g_webviewHost->Bridge().PostEvent("window.state", state);
                }
            }
        }
        // 尺寸变化时重新应用圆角：
        //  - large 档：SetWindowRgn 需要按新尺寸重建圆角区域，避免圆角拉伸变形
        //  - 最大化/还原：切换是否裁剪圆角，防止最大化时圆角遮挡任务栏
        lxe::ApplyWindowCorner(hWnd);
        break;
    case WM_WEBVIEW_FLUSH_JSON:
        // 跨线程入队的 WebView2 消息需要在 UI 线程 flush
        if (g_webviewHost) g_webviewHost->DispatchPendingMessages();
        return 0;
    case WM_WEBVIEW_FALLBACK:
        // 后台预检线程发现虚拟域名不可用，由 UI 线程执行 Navigate 回退到 HTTP
        if (g_webviewHost) g_webviewHost->OnFallbackRequested();
        return 0;
    case WM_DESTROY:
        // 关闭前保存当前窗口位置/大小（含正常/最大化状态）
        SaveWindowPlacementToRegistry(hWnd);
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

// “关于”框的消息处理程序。
INT_PTR CALLBACK About(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(lParam);
    switch (message)
    {
    case WM_INITDIALOG:
        return (INT_PTR)TRUE;

    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)
        {
            EndDialog(hDlg, LOWORD(wParam));
            return (INT_PTR)TRUE;
        }
        break;
    }
    return (INT_PTR)FALSE;
}