好的，已收到您的需求。以下是将 **LXElauncher 统一插件标准 V5.0** 与 **版本兼容性增强补充（V5.1）** 合并、补充完善后的 **统一标准文档 V1.0**。

---

# LXElauncher 统一插件标准 · 完整规范 V1.0

> **文档版本**：1.0  
> **适用声明**：本标准旨在让同一插件**无修改**运行于 LXElauncher、HMCL、PCL2、Baka‑Launcher、MultiMC 等不同启动器之上。启动器内核（后端）已有差异，本标准仅约定前端插件与宿主之间的**契约接口**，后端差异由启动器自身的适配层屏蔽。  
> **核心设计目标**：  
> - **契约稳定**：通过严格版本语义化，确保插件与宿主的安全兼容。  
> - **能力完备**：提供文件、进程、UI、事件等完整 API，足以支撑地图编辑器、崩溃分析、启动拦截等复杂插件。  
> - **生态统一**：一套代码，任意启动器运行。

---

## 1. 语言与运行环境

| 项目 | 规范 |
|------|------|
| **插件语言** | **JavaScript (ES2022+)** 或 **TypeScript（编译为 .js 后分发）** |
| **模块格式** | **ES Module**（使用 `export`/`import`） |
| **入口文件** | `main.js`，必须默认导出 `activate` 与 `deactivate` 函数 |
| **全局对象** | 宿主注入唯一的全局对象 **`LX`**，插件不得访问 `window` 或 `document` 的宿主私有属性（但可操作 DOM） |
| **宿主环境** | Chromium WebView2 / Electron / CEF，保证现代 DOM API 可用 |

---

## 2. 版本体系（三层独立语义）

为了安全兼容，启动器必须暴露三个独立版本号，插件在 `manifest.json` 中声明依赖范围。

| 版本字段 | 说明 | 示例 |
| :--- | :--- | :--- |
| **`apiVersion`** | **前端插件 API 规范版本**（本标准文档版本）。<br>变动表示 `LX.ui`、`LX.inject`、事件名称等**前端契约**发生破坏性变更。 | `"5.0"` |
| **`kernelVersion`** | **后端 RPC 内核版本**（启动器后端接口版本）。<br>变动表示 `LX.call` 中的底层 RPC 方法（如 `mc.launch` 参数结构）发生破坏性变更。 | `"2.1.0"` |
| **`version`** | **启动器整体发布版本**（仅供显示，不建议用于逻辑判断）。 | `"2.1.0-beta.3"` |

这些版本均在 `LX.HOST` 中提供，启动器加载插件时会依据 `manifest` 声明进行严格校验。

---

## 3. 宿主自描述（LX.HOST）

启动器启动时必须注入 `LX.HOST`，供插件识别环境与能力。

```typescript
interface LXHost {
  // ----- 基础标识 -----
  id: string;                     // 启动器唯一 ID（见注册表）
  name: string;                   // 显示名称
  version: string;                // 整体版本号（仅展示）
  apiVersion: string;             // 前端 API 规范版本，如 "5.0"
  kernelVersion: string;          // 后端 RPC 内核版本，如 "2.1.0"

  // ----- 布局 -----
  layout: LayoutType;             // 当前布局
  supportedLayouts: LayoutType[]; // 支持的所有布局

  // ----- 能力特性（供插件降级适配）-----
  features: {
    nativeMenu: boolean;          // 是否支持原生右键菜单
    acrylic: boolean;             // 是否支持毛玻璃
    multiInstance: boolean;       // 是否允许多开
    customCss: boolean;           // 是否允许注入全局样式（通常为 true）
    modalStack: boolean;          // 是否支持多模态框叠加
  };

  // ----- UI 默认风格参考（仅供插件“继承”或“混搭”，非强制）-----
  uiDefaults: {
    primaryColor: string;         // 主色，如 "#1976D2"
    borderRadius: string;         // 圆角，如 "16px"
    fontFamily: string;           // 字体
    isDark: boolean;              // 是否为深色模式
  };

  // ----- 可用插槽列表 -----
  slots: SlotId[];                // 所有可注入的锚点 ID（类型见第 6 节）
}
```

**已注册的启动器 ID（`LX.HOST.id`）**：

| ID | 对应启动器 | 备注 |
|----|-----------|------|
| `lxe-launcher` | LXElauncher | 本启动器 |
| `hmcl` | HMCL | 兼容模式 |
| `pcl2` | PCL2 | 兼容模式 |
| `baka-launcher` | Baka-Launcher | — |
| `multi-mc` | MultiMC | 兼容模式 |
| `prism-launcher` | Prism Launcher | — |
| `generic` | 其他未知启动器 | 通用回退 |

**布局类型（`LayoutType`）**：

```typescript
type LayoutType = 
  | 'top-nav'          // 顶栏导航
  | 'left-sidebar'     // 左侧栏
  | 'bottom-nav'       // 底部栏
  | 'fullscreen'       // 全屏覆盖
  | 'docking'          // 可停靠面板
  | 'minimal'          // 仅内容区
  | 'cli';             // 无图形界面
```

---

## 4. 清单文件（manifest.json）

插件根目录必须包含 `manifest.json`，格式如下（**新增版本依赖字段**）：

```json
{
  "$schema": "https://example.com/lxe-plugin-schema-v1.json",
  "id": "com.example.seedmap",
  "name": "Seedmap Generator",
  "version": "1.2.0",
  "description": "根据种子生成地图预览",
  "author": "Your Name",
  "icon": "assets/icon.png",
  "main": "main.js",

  // ----- 版本契约（必须声明）-----
  "apiVersion": "5.0",                // 必须与 LX.HOST.apiVersion 完全一致
  "kernelVersion": ">=2.0.0 <3.0.0", // semver 范围，支持 >=, <, ~, ^ 等

  // ----- 运行环境限制（可选）-----
  "hosts": ["lxe-launcher", "hmcl", "pcl2"],   // 允许运行的启动器，空数组 = 任意
  "layouts": ["top-nav", "left-sidebar"],       // 适配的布局，空数组 = 任意

  // ----- 权限声明（按需）-----
  "permissions": {
    "fs:read": ["/logs", "/saves"],            // 允许读取的目录（相对 MC 根目录）
    "fs:write": ["/plugins/data"],             // 允许写入的目录（仅限插件自身数据）
    "shell:execute": ["seedmap.exe"],          // 允许调用的可执行文件白名单
    "shell:open": true,                        // 是否允许打开外部 URL
    "ui:modal": true,                          // 是否允许弹窗
    "ui:toast": true,                          // 是否允许通知
    "storage": true                            // 是否允许持久化存储
  },

  // ----- 声明式注入（函数名对应 main.js 导出）-----
  "injections": {
    "home:widgets": "renderWidget",
    "dialog:crash:body": "renderCrashPanel"
  },

  // ----- 声明式事件钩子-----
  "hooks": {
    "launch:before": "onLaunchBefore",
    "launch:crash": "onCrash"
  },

  // ----- 样式注入（可选）-----
  "styles": "assets/plugin.css",

  // ----- 依赖其他插件（可选）-----
  "dependencies": {
    "com.example.lib": ">=1.0.0"
  }
}
```

**版本范围语法（遵循 semver）**：

| 表达式 | 含义 |
| :--- | :--- |
| `"2.1.0"` | 必须精确等于 `2.1.0` |
| `">=2.0.0"` | 大于等于 `2.0.0` |
| `">=2.0.0 <3.0.0"` | 2.x.x 系列 |
| `"~2.1.0"` | 兼容 `2.1.x`（补丁级） |
| `"^2.0.0"` | 兼容 `2.x.x`（次要级） |

**启动器加载行为**：  
- `apiVersion` 必须 **完全相等**，否则拒绝加载。  
- `kernelVersion` 必须满足 semver 范围，否则拒绝加载。  
- 拒绝时需输出明确错误日志并通知用户。

---

## 5. 全局 API 详细规范（LX 对象）

### 5.1 基础通信 —— `LX.call`

唯一与后端通信的通道。所有 RPC 方法名已标准化。

```typescript
function call<T = any>(method: string, params?: any): Promise<T>;
```

**标准化 RPC 方法列表**（插件应仅使用这些）：

```typescript
interface StandardRPC {
  // ----- 游戏核心 -----
  'mc.launch': (params: LaunchParams) => Promise<{ pid: number; success: boolean }>;
  'mc.stop': () => Promise<{ pid?: number }>;
  'mc.status': () => Promise<{ running: boolean; pid?: number }>;
  'mc.versions': () => Promise<{ list: Version[] }>;          // Version: { id: string, type: string, releaseTime: string }
  'mc.localVersions': () => Promise<{ list: Version[] }>;
  'mc.install': (params: { version: string; loader?: string }) => Promise<{ taskId: string }>;

  // ----- 文件系统（经权限授权）-----
  // fs.readFile 的 maxBytes（可选，正整数）：限制读取字节数；文件更大时只返回末尾 maxBytes 字节，
  // 并在返回结果中附加 truncated: true（日志类文件错误信息通常在末尾，建议大日志传入以免整读阻塞界面）
  'fs.readFile': (params: { path: string; encoding?: 'utf-8' | 'base64'; maxBytes?: number }) => Promise<{ content: string; truncated?: boolean }>;
  'fs.writeFile': (params: { path: string; content: string; encoding?: 'utf-8' }) => Promise<{ ok: boolean }>;
  'fs.readDir': (params: { path: string }) => Promise<{ entries: string[] }>;
  'fs.exists': (params: { path: string }) => Promise<{ exists: boolean }>;
  'fs.mkdir': (params: { path: string }) => Promise<{ ok: boolean }>;
  'fs.rm': (params: { path: string; recursive?: boolean }) => Promise<{ ok: boolean }>;

  // ----- 下载 -----
  'download.start': (params: { url: string; path: string; name?: string }) => Promise<{ taskId: string }>;
  'download.status': (params: { taskId: string }) => Promise<{ percent: number; speed: string }>;
  'download.cancel': (params: { taskId: string }) => Promise<{ ok: boolean }>;

  // ----- 系统/Shell -----
  'shell.openUrl': (params: { url: string }) => Promise<void>;
  'shell.execute': (params: { command: string; args?: string[]; cwd?: string }) => Promise<{ pid: number }>;
}
```

**路径规范**：所有 `path` 参数均为 **相对于 Minecraft 根目录（即 `.minecraft` 文件夹）的绝对路径**，以 `/` 开头（如 `/logs/latest.log`）。跨平台时启动器适配层负责转换。

**`LaunchParams` 结构**（支持扩展）：

```typescript
interface LaunchParams {
  version: string;
  accountName: string;
  mem: number;                // MB
  javaPath?: string;
  jvmArgs?: string[];
  customResolution?: { width: number; height: number };
  [key: string]: any;         // 插件可附加自定义字段（启动器应透传，但不保证处理）
}
```

### 5.2 事件系统 —— `LX.on` / `LX.off` / `LX.emit`

```typescript
// 订阅宿主事件，返回取消函数
function on(event: HostEvent, handler: (data: any) => void): () => void;

// 取消订阅
function off(event: HostEvent, handler?: (data: any) => void): void;

// 插件间通信（自定义事件，建议使用 <插件ID>:<事件名> 命名）
function emit(event: string, data?: any): void;
```

**标准化宿主事件列表**（见第 8 节）。

### 5.3 存储 —— `LX.storage`

每个插件拥有独立的键值存储空间，持久化。

```typescript
interface Storage {
  get<T = any>(key: string): Promise<T | undefined>;
  set<T = any>(key: string, value: T): Promise<void>;
  remove(key: string): Promise<void>;
  clear(): Promise<void>;
  keys(): Promise<string[]>;
}
```

### 5.4 页面注入 —— `LX.inject`

向指定插槽（锚点）注入 DOM 内容。

```typescript
function inject(
  slotId: SlotId, 
  renderer: string | (() => HTMLElement | string | Promise<HTMLElement | string>)
): void;
```

- `slotId` 必须是 `LX.HOST.slots` 中存在的值。  
- `renderer` 可以是 HTML 字符串，或返回 DOM 元素/HTML 的函数（支持异步）。  
- 多次注入同一插槽，默认追加（启动器可提供清空方法，但本标准未强制）。

### 5.5 UI 工具包 —— `LX.ui`

提供模态框、通知等便利函数（可选实现，但推荐提供）。

```typescript
interface UI {
  modal: {
    open: (options: ModalOptions) => Promise<string>;  // 返回 modalId
    close: (id: string) => void;
    update: (id: string, content: string | HTMLElement) => void;
  };
  toast: {
    show: (options: ToastOptions) => void;
    hide: (id?: string) => void;
  };
  // 工具函数
  h: (tag: string, props?: any, ...children: any[]) => HTMLElement;
  render: (node: HTMLElement | string, container: HTMLElement) => void;
  createElement: (tag: string, attrs?: Record<string, any>, children?: any[]) => HTMLElement;
}
```

**`ModalOptions`**：

```typescript
interface ModalOptions {
  id?: string;                     // 可选，自动生成
  title: string;
  content: string | HTMLElement | (() => HTMLElement);
  size?: 'sm' | 'md' | 'lg' | 'xl' | 'full';
  closable?: boolean;
  onClose?: () => void;
  actions?: ModalAction[];
}
```

**`ModalAction`**：

```typescript
interface ModalAction {
  label: string;
  variant?: 'primary' | 'secondary' | 'danger' | 'text';
  onClick: (close: () => void) => void | Promise<void>;
}
```

**`ToastOptions`**：

```typescript
interface ToastOptions {
  id?: string;
  message: string;
  type?: 'info' | 'success' | 'warning' | 'error';
  duration?: number;               // 毫秒，0=永久
  action?: { label: string; onClick: () => void };
}
```

### 5.6 文件系统快捷 —— `LX.fs`

语义化封装 `fs.*` RPC，自动处理权限检查。

```typescript
interface FS {
  readFile(path: string, encoding?: 'utf-8' | 'base64', maxBytes?: number): Promise<string>;
  writeFile(path: string, content: string, encoding?: 'utf-8'): Promise<void>;
  readDir(path: string): Promise<string[]>;
  exists(path: string): Promise<boolean>;
  mkdir(path: string): Promise<void>;
  rm(path: string, recursive?: boolean): Promise<void>;
}
```

**`readFile` 的 `maxBytes` 参数**（可选，正整数）：限制读取字节数，文件超过该值时**只返回末尾 `maxBytes` 字节**（日志类文件错误信息通常在末尾）。是否发生截断可通过底层 `LX.call('fs.readFile', ...)` 的返回字段 `truncated` 判断。读取超大日志（如 `latest.log` / `debug.log` 可达数十 MB）时建议传入，避免整读同步阻塞启动器界面。

### 5.7 Shell/系统快捷 —— `LX.shell`

```typescript
interface Shell {
  openUrl(url: string): Promise<void>;
  execute(command: string, args?: string[], cwd?: string): Promise<{ pid: number }>;
  openFile(path: string): Promise<void>;  // 用默认程序打开
  getEnv(key: string): Promise<string | undefined>;
}
```

### 5.8 样式注入 —— `LX.injectStyles`

```typescript
function injectStyles(css: string): void;      // 注入 CSS 文本
function injectStyles(url: string): void;      // 或加载外部 CSS（URL）
```

---

## 6. 标准化插槽（注入点）完整列表

插槽 ID 类型定义为 `SlotId`，取值为以下字符串字面量联合：

```typescript
type SlotId = 
  // 导航
  | 'navbar:left' | 'navbar:right' | 'navbar:center'
  | 'sidebar:top' | 'sidebar:bottom'
  // 主页
  | 'home:before' | 'home:after' | 'home:widgets'
  // 下载页
  | 'downloads:toolbar' | 'downloads:list:before' | 'downloads:list:after'
  // 设置页（可动态拼接 section）
  | 'settings:before' | 'settings:after'
  | `settings:section:${string}`   // 例如 'settings:section:launch'
  // 系统对话框
  | 'dialog:crash:body' | 'dialog:crash:footer'
  | 'dialog:launch:body'
  | `dialog:custom:${string}`;
```

**`settings:section` 的常用 ID 建议**（由启动器自行定义，但推荐统一）：
- `launch`（启动设置）
- `graphics`（图形）
- `controls`（控制）
- `network`（网络）
- `account`（账户）

---

## 7. 生命周期与入口函数

`main.js` 必须导出以下函数：

```typescript
// 插件激活时调用
export async function activate(context: PluginContext): Promise<void> {
  // context: { id: string, storage: Storage, host: LXHost }
}

// 插件停用时调用
export async function deactivate(): Promise<void> {
  // 清理资源
}

// 可选：声明式钩子函数（若 manifest 中引用）
export function onLaunchBefore(params: LaunchParams) { /* ... */ }
export function renderCrashPanel() { /* ... */ }

// 可选：暴露公共 API 供其他插件调用
export const api = { /* ... */ };
```

---

## 8. 标准化事件与钩子（完整列表）

所有事件均可通过 `LX.on` 订阅，部分支持阻断或修改数据。

| 事件名 | 触发时机 | 参数类型 | 是否可阻断/修改 | 说明 |
|--------|----------|----------|----------------|------|
| `app:ready` | 启动器加载完成 | `{}` | ❌ | 初始化注入 |
| `app:before-close` | 窗口关闭前 | `{ hasDownloads: boolean }` | ✅ 返回 `false` 阻止关闭 | |
| `panel:before-show` | 页面切换前 | `{ name: string; previous: string }` | ✅ 返回 `false` 阻止切换 | |
| `panel:after-show` | 页面切换后 | `{ name: string }` | ❌ | |
| **`launch:before`** | **游戏启动前** | `LaunchParams`（引用传递） | ✅ **可修改参数** | 添加 JVM 参数、调整内存等 |
| `launch:after` | 游戏启动后 | `{ pid: number; version: string; success: boolean }` | ❌ | |
| **`launch:crash`** | **游戏崩溃时** | `CrashData` | ❌ | 注入分析面板 |
| `launch:stop` | 游戏被停止 | `{ pid: number }` | ❌ | |
| `download:start` | 下载开始 | `{ taskId: string; name: string; url: string }` | ❌ | |
| `download:progress` | 下载进度 | `{ taskId: string; percent: number; speed: string }` | ❌ | |
| `download:done` | 下载完成 | `{ taskId: string; path: string }` | ❌ | |
| `download:error` | 下载失败 | `{ taskId: string; error: string }` | ❌ | |
| `settings:change` | 设置变更 | `{ key: string; value: any; oldValue: any }` | ✅ 返回 `false` 阻止变更 | |
| `account:switch` | 账号切换 | `{ account: Account }` | ❌ | |
| `ui:modal-open` | 模态框打开 | `{ id: string; title: string; content: any }` | ✅ **可替换 content**（通过返回新 content） | 插件可拦截并修改内容 |
| `ui:modal-close` | 模态框关闭 | `{ id: string }` | ❌ | |

**重要数据结构**：

```typescript
interface CrashData {
  exitCode: number;
  logPath: string;            // 绝对路径
  crashReport?: string;       // 如果是 Java 崩溃，包含摘要
  isNative: boolean;          // 是否为本机崩溃（非 Java）
  version: string;
  time: string;               // ISO 时间
}

interface Account {
  id: string;
  name: string;
  type: 'offline' | 'premium' | 'thirdparty';
  uuid?: string;
}
```

---

## 9. 权限模型

插件在 `manifest.json` 中声明所需权限，启动器在加载时检查并拦截未授权调用。

| 权限 | 说明 | 默认行为（未声明） |
|------|------|-------------------|
| `storage` | 允许使用 `LX.storage` | 存储操作静默失败 |
| `fs:read` | 允许读取指定路径（支持 `*` 通配符，如 `"/logs/*"`） | `LX.fs.readFile` 抛出异常 |
| `fs:write` | 允许写入指定路径（支持通配符） | `LX.fs.writeFile` 抛出异常 |
| `shell:execute` | 允许执行指定程序（支持通配符，如 `"*.exe"`） | `LX.shell.execute` 抛出异常 |
| `shell:open` | 允许打开 URL 或文件 | `LX.shell.openUrl` 静默失败 |
| `ui:modal` | 允许弹出模态框 | `LX.ui.modal.open` 抛出异常 |
| `ui:toast` | 允许显示通知 | `LX.ui.toast.show` 静默失败 |
| `inject:styles` | 允许注入全局样式 | 始终允许（插件自由） |
| `inject:dom` | 允许向插槽注入 DOM | 始终允许 |

**通配符规则**：  
- `*` 匹配任意路径片段（如 `/logs/*.log` 匹配 `/logs/a.log`，不匹配 `/logs/sub/b.log`）。  
- `**` 匹配任意深度的目录（如 `/logs/**` 匹配 `/logs/a.log` 和 `/logs/sub/b.log`）。  
- 路径始终以 `/` 开头，相对 MC 根目录。

---

## 10. 跨启动器兼容性保证

为确保插件在不同启动器上通用，遵循以下原则：

1. **仅使用标准 RPC**：只调用 `LX.call` 中列出的方法名。  
2. **检测特性**：通过 `LX.HOST.features` 判断能力，优雅降级。  
3. **不依赖特定 DOM 结构**：通过插槽注入，不直接操作宿主私有类名。  
4. **样式隔离**：使用独特前缀（如 `my-plugin-`）或 Shadow DOM（若支持）。  
5. **布局适配**：依据 `LX.HOST.layout` 决定注入位置。

**版本兼容策略**（详见第 4 节）：  
- 启动器强制校验 `apiVersion` 与 `kernelVersion`，不满足则拒绝加载。  
- 插件如需支持多版本，应在 `manifest` 中声明宽泛范围，并在 `activate` 中动态开关功能。

---

## 11. 插件功能完整性验证（以地图编辑器为例）

**问**：能否基于本标准开发一个完整的 **Minecraft 地图编辑器**？  
**答**：**完全可以**。所需能力全部具备：

| 功能需求 | 对应 API / 机制 |
|---------|----------------|
| 读取/写入地图数据（如 `region` 文件） | `LX.fs.readFile` / `writeFile` 访问 `/saves/世界名/region/*.mca` |
| 浏览世界列表 | `LX.fs.readDir('/saves')` |
| 显示地图预览（渲染 2D/3D） | 通过 `LX.inject` 注入 Canvas 或 WebGL 容器，自由使用 DOM API |
| 调用外部地图渲染工具（如 `seedmap.exe`） | `LX.shell.execute`（需在 manifest 中声明白名单） |
| 导出地图为图片 | 使用 Canvas 的 `toDataURL`，或通过 `LX.fs.writeFile` 保存 |
| 用户交互（选择种子、调整参数） | 在注入的 DOM 中添加表单，绑定事件 |
| 持久化用户设置（如最近打开的世界） | `LX.storage` |
| 通知用户渲染完成 | `LX.ui.toast` |
| 崩溃时自动分析地图相关错误 | 订阅 `launch:crash`，读取日志，给出建议 |

**示例代码片段**（简易地图列表）：

```javascript
export function renderWidget() {
  const container = document.createElement('div');
  container.innerHTML = `<h3>🌍 地图列表</h3><ul id="world-list"></ul>`;
  (async () => {
    const worlds = await LX.fs.readDir('/saves');
    const list = container.querySelector('#world-list');
    worlds.forEach(name => {
      const li = document.createElement('li');
      li.textContent = name;
      li.onclick = () => loadWorld(name);
      list.appendChild(li);
    });
  })();
  return container;
}
```

因此，本标准的功能覆盖度足以实现地图编辑器乃至更复杂的插件。

---

## 12. 插件间通信规范

建议使用 **`<插件ID>:<事件名>`** 命名空间，避免冲突。  
示例：插件 `com.example.seedmap` 触发 `LX.emit('com.example.seedmap:generate', { seed: 123 })`，其他插件通过 `LX.on('com.example.seedmap:generate', ...)` 监听。

---

## 13. 启动器适配层实现要点（给启动器开发者的指南）

1. **注入 `LX` 全局对象**，实现所有 API（`call`, `on`, `inject`, `storage`, `ui`, `fs`, `shell`, `injectStyles`）。  
2. **实现标准 RPC 映射**：将 `mc.launch` 等映射到自己的后端实现。  
3. **提供插槽容器**：在界面中预留所有标准插槽的 DOM 元素，并实现 `LX.inject` 的插入逻辑。  
4. **触发标准事件**：在游戏启动、崩溃、下载等时机，调用 `LX.emit` 或直接触发监听器。  
5. **解析 `manifest.json`**：加载时验证版本、权限，自动注入样式和钩子，执行 `activate`。  
6. **版本校验逻辑**：必须使用语义化版本比较库（如 `semver`）检查 `kernelVersion` 范围。

---

## 14. 安全与错误处理

- **权限拦截**：对未授权的 `fs`、`shell` 调用抛出明确的 `SecurityError`。  
- **超时保护**：建议对 RPC 调用设置超时（如 30 秒），防止死等待。  
- **插件崩溃隔离**：单个插件的未捕获异常不应导致整个启动器崩溃，宿主应捕获并记录。  
- **存储限额**：建议限制单个插件存储数据大小（如 5MB），避免滥用。

---

## 15. 总结

本标准通过以下设计，实现了 **“插件零修改跨启动器运行”** 与 **“功能深度可扩展”** 的双重目标：

- **契约版本化**：`apiVersion` + `kernelVersion` 双锁，确保安全升级。  
- **完整 API 覆盖**：文件、进程、UI、事件、存储一应俱全。  
- **布局无关注入**：通过插槽机制，适应不同启动器界面。  
- **声明式配置**：清单文件驱动样式、钩子、权限，减少样板代码。  
- **生态协作**：插件间通信与公共 API 暴露，支持复杂协作。

任何启动器只需实现本标准定义的 `LX` 接口，即可无缝接入整个插件生态。

---

**文档版本**：1.0  
**对应 API 规范版本**：5.0  
**最后更新**：2026-08-18  
**维护者**：LXElauncher 团队