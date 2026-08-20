#pragma once

#include <windows.h>

#include "../bridge/Bridge.h"

// 由 LXElauncher.cpp 实现（Win10+ SetWindowCompositionAttribute 亚克力毛玻璃）
void EnableAcrylic(HWND hwnd, BYTE r, BYTE g, BYTE b, BYTE alpha);
void DisableAcrylic(HWND hwnd);

namespace lxe {

void RegisterDemoServices(Bridge& bridge);
void RegisterWindowServices(Bridge& bridge, HWND hwnd);
// 由 LXElauncher.cpp 在启动时根据 temp 目录提供游戏版本清单路径
void SetManifestPath(const std::wstring& manifestJsonPath);
// 设置 exe 同级目录（用于查找 aria2c.exe 等可执行工具）
void SetExeDir(const std::wstring& exeDir);
// 关闭前调用：停止并回收后台线程（游戏进程监控等），必须早于 WebViewHost 析构
void ShutdownServices();

// ---------- 窗口圆角设置 ----------
// 圆角档位: "none" | "small" | "medium" | "large" | "custom"
//   none   = 无圆角(DWMWCP_DONOTROUND + SetWindowRgn=NULL)
//   small  = 小圆角(DWMWCP_ROUNDSMALL)
//   medium = 中等圆角/Win11 默认(DWMWCP_ROUND)
//   large  = 大圆角(DWMWCP_ROUND)
//   custom = 自定义圆角(SetWindowRgn 按 SetWindowCornerRadius 的像素半径裁剪)
std::string GetWindowCornerMode();
void SetWindowCornerMode(const std::string& mode);
// 自定义圆角半径（px）；仅档位为 "custom" 时生效
void SetWindowCornerRadius(int radius);
int GetWindowCornerRadius();
// 在 HWND 上根据当前档位实时应用圆角；WM_SIZE 和 InitInstance 以及 window.setCorner RPC 调用
void ApplyWindowCorner(HWND hwnd);

// ---------- 窗口毛玻璃（Acrylic）设置 ----------
// 由"开发者模式-亚克力"开关控制，默认开；保存到 settings.json 的 "acrylic" 键
void SetAcrylicEnabled(bool enabled);
bool GetAcrylicEnabled();
// 根据当前开关状态在 HWND 上应用/移除亚克力；启动、WM_EXITSIZEMOVE、window.setAcrylic RPC 调用
void ApplyWindowAcrylic(HWND hwnd);

} // namespace lxe