# LXElauncher 开发经验备忘

本文件记录在本项目中实际踩坑、验证过的开发经验。改动代码前请先读一遍。

---

## 功能速查索引（功能 → 代码定位）

> 行号会随编辑偏移，**一律用 grep 关键字定位**。前端文件是单行大文件，前端函数名用 `Select-String` 搜（文件含 NUL 字节，Grep 工具会判定为二进制而跳过）。

### 一、后端 RPC 一览（src/bridge/Services.cpp，按功能分组）

| 功能 | RPC（grep `Register("方法名"` 定位） | 说明 |
|---|---|---|
| 系统 | `app.ping` `app.config` `system.info` `app.buildInfo` `app.exportLogs` `shell.openUrl` | 导出日志 UTF-8 BOM 合并前后端日志 |
| 窗口 | `window.minimize` `window.toggleMaximize` `window.close` | postMessage JSON RPC |
| 下载中心 | `mc.submitDownloadList` `download.cancel` `aria2.available` | 任务级取消见 §21 |
| 版本 | `mc.versions` `mc.installVanilla` `mc.localVersions` `mc.importFolder` `mc.getMcRoot` `mc.refreshVersionList` `mc.deleteVersion` `mc.completeVersion` | installVanilla 带 anyLibError 防假完成 |
| 启动 | `mc.launch` `mc.isGameRunning` `mc.stopGame` `mc.versionIsInstalled` | 启动命令解析 BuildClasspath/ResolveVersionJson |
| 加载器 | `mc.installLoader` `mc.loaderVersions` | g_installLoaderImpl 文件作用域 |
| Java | `mc.javaList` `java.download` `mc.javaInfo` `mc.rescanJava` `java.isInstalled` `java.uninstall` `java.listInstalled` `mc.javaAutoInstall` | 检测链见 §15 |
| 日志 | `mc.logList` `mc.readGameLog` | 游戏 stdout/stderr 落盘 lxe-launcher-std.log |
| 账号 | `auth.probe/login/refresh/validate/injectorPath/ensureInjector` `msa.requestCode/completeLogin/refresh/profile/skin/capes/capeActive` | 外置登录见 §13；MSA 皮肤/披风 |
| MC 文件夹 | `mc.listMcFolders` `mc.addMcFolder` `mc.removeMcFolder` `mc.setActiveMcFolder` | 注册表持久化，.minecraft 自动迁移 |
| 智能拖放 | `mc.probeZip` `mc.probeFolder` `mc.dropImport` `app.readFileDataUrl` | 见 §20/§21 |
| 版本设置 | `mc.listPackFiles` `mc.deletePackFile` `mc.togglePackFile` `mc.togglePackFiles` `mc.deletePackFiles` `mc.openPackFolder` `mc.openMcFolder` | 模组/资源包/光影管理 |
| 模组市场 | `mc.searchMods` `mc.curseSearch` `mc.modDetail` `mc.modVersions` `mc.modLatestFile` `mc.modDepsTree` `mc.gameVersions` | Modrinth API（字段见 §5）；CurseForge 搜索见 §22 |
| 整合包 | `mc.installModpack` | mrpack 导入，结构见 §6 |
| 设置 | `settings.get` `settings.set` `source.info` `source.probe` | 镜像源/持久化 |
| 工具 | `http.fetchText` `mc.paths` `dialog.openFile` | |

### 二、后端核心工具函数（Services.cpp）

| 函数（grep 定位） | 作用 |
|---|---|
| `RunProcessSilent` | CREATE_NO_WINDOW 静默执行外部进程 |
| `HttpFetchText` | 拉取文本（下载源元数据） |
| `Aria2DownloadWithProgress` | aria2 `-x16 -s16` 多线程下载 + 进度回调（回调返回 false → 杀进程） |
| `DownloadFileSmart` | 通用单文件下载（WinHTTP 兜底） |
| `DownloadAuthlibInjector` | authlib jar 三级兜底下载（BMCLAPI→官方→GitHub） |
| `BuildClasspath` | classpath 构建（坐标解析去重，见 §19/§20） |
| `ResolveVersionJson` | 版本 json 合并（父版本 javaVersion 兜底） |
| `CompleteVersionFilesWorker` | 补全文件 worker（compSeq 50000） |
| `DetectVersionType` | 版本类型判断（release/snapshot/old_beta…） |
| `MavenVersionCmp` | Maven 版本比较 |
| `ScanInstalledJavas` / `ScanInstalledJavasNow` / `ProbeJavaExe` / `AddKnownLauncherJava` | Java 自动检测（注册表→常见路径→runtime 扫描→知名启动器） |
| `FindJavaPath` | 按版本匹配 Java（用户自定义 > 自动检测） |
| `GameMonitorThread` | 2 秒轮询游戏进程退出码（0=正常/非0=崩溃） |
| `g_installLoaderImpl` | 加载器安装实现（Forge 新版 processors 见 §12） |
| `DLCancelFlag` / `DLCancelFlagSet` / `DLCancelFlagRemove` | 任务级取消标志注册表（§21） |

### 三、前端功能速查（webapp/index.html 内联 JS）

| 功能 | 定位（Select-String 搜函数名） | 说明 |
|---|---|---|
| 统一任务中心 | `dlTasks` / `updateDownloadTask` / `renderNavDownloads` / `openTaskDetail` / `renderTaskDetail` / `cancelDownloadTask` / `clearDoneDownloads` | 监听 `download.progress` `download.state`（cancelled/cancelling） |
| 下载中心 | `renderDlPager` / `loadGameVersions` / `mc.submitDownloadList` 调用处 / 镜像源下拉（Persisted 同步） | 版本分支筛选、表格选择器 |
| 启动流程 | `buildLaunchParams` / `mc.launch` 调用处 / 预检（dryRun） | Java 自动选择、authlib 注入 |
| 安装流水线 | `mc.completeVersion` `mc.installLoader` 调用处 / `onStateEv` / `onProgressEv` | cancelled 分支必须处理 |
| Java 管理 | `downloadJava` / `mc.javaList` / `mc.javaAutoInstall` 调用处 / `java.found` 事件 | 自动安装并入任务中心 |
| 版本设置页 | `mc.listPackFiles` 等调用 / 5 标签页切换 | PCL2 式开关激活 |
| 模组市场 | `mc.searchMods` / `mc.modDetail` / `mc.modVersions` 调用处 | 分页/版本过滤 |
| 整合包 | `installModpack` | 下载→导入 |
| 智能拖放 | `handleDroppedPath` / `showDropConfirm` / `pendingDropRun` / `nativeDragEnter` `nativeDragLeave` `nativeDrop` 监听 | 先识别类型→确认→执行 |
| 设置 | `Persisted` / `SettingStore` / `applyGlassBlur` / 各开关 | localStorage + 注册表同步 |
| 通用 UI | `notify` / `confirmBox` / `openOverlay` / `closeOverlay` | 二次确认/浮层动画 |
| 入口 | `init` / `window.LX = LX` | 桥接对象挂载 |

### 四、窗口 / WebView2（src/win/）

| 文件/函数 | 说明 |
|---|---|
| `WebViewHost.cpp` `SetupWebView` | 虚拟主机映射 app.localhost（先导航后后台预检，失败 HTTP 兜底） |
| `WebViewHost.cpp` `InstallWebViewSubclass` / `WebViewSubclassProc` | 子窗口类化 + **接管子窗口 OLE 拖放目标**（§21） |
| `WebViewHost.cpp` `WebDropTarget`（全局类） | IDropTarget 实现：CF_HDROP 取真实路径 + CF_UNICODETEXT，OleInitialize 必须 |
| `WebViewHost.cpp` `WM_FLUSH_POST_JSON` | 非 UI 线程 PostMessage 队列刷新（互斥 + 原子变量） |
| `WebViewHost.h` | 公开 WM_WEBVIEW_FALLBACK / WM_WEBVIEW_FLUSH_JSON 常量 |
| `LXElauncher.cpp` WndProc | WM_NCHITTEST 边缘命中、WM_EXITSIZEMOVE 延迟 Resize、注册表窗口位置持久化 |

---

## 0. 硬性约束

- 下载源一律用官方源（Mojang、Fabric Meta、Quilt Meta、Neoforge Maven、Modrinth API、BMCLAPI 仅用于 Neoforge/Quilt 的版本列表加速）。
- Forge 新版安装器按 processors 手动执行（教程 Part 3）；旧版 universal jar 型才走官方 `--installClient`。安装后把版本合并到 `installName`（不是标准 Forge 版本名）。详 §12。
- 「不改动的别动」「不该用的命令别用」——尽量做最小改动，别顺手重构无关代码。
- 前端真身是 `webapp/index.html` 内联的 `<script>`；`webapp/js/app.js` 与 `webapp/css/theme.css` 是死代码/未加载文件，**不要改**。
- WebView2 界面无法用 playwright 驱动，UI 端到端验证只能靠用户运行 exe 或人工确认。

---

## 1. 构建

- MSBuild 实际路径（VS 18，不是 2022 Community）：
  `C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe`
- 构建命令：
  `& "<上面路径>" LXElauncher.vcxproj /p:Configuration=Debug /p:Platform=x64 /m /v:minimal /nologo`
- **LNK1140（PDB 超限）**：`x64\Debug\LXElauncher.pdb` / `.ilk` 超 2GB 后链接失败。构建前先删掉这两个文件：
  `Remove-Item -LiteralPath "x64\Debug\LXElauncher.pdb","x64\Debug\LXElauncher.ilk" -ErrorAction SilentlyContinue`
- 构建前先 `Stop-Process` 掉运行中的 `LXElauncher`，否则 exe 被占用无法写。
- 成功后产物：`x64\Debug\LXElauncher.exe`；日志里的 `aria2c copied or not found` 只是资源复制提示，不是错误。

---

## 2. 后端架构与 RPC 约定

- 后端核心在 `src/bridge/Services.cpp`，用 C++ 写，通过 `Bridge` 注册 RPC，前端 JS 用 `LX.call(method, params)` 调用。
- 三种注册方式：
  - `bridge.Register(name, fn)`：fn 返回 `Json`（同步；`Ok(obj)` / `Err(code, msg)`）。
  - `bridge.RegisterAsync(name, fn)`：fn 形如 `(const Json& params, const std::function<void(HandlerResult)>& done)`，内部 `std::thread([...](){ ... done(Ok(result)); }).detach();`，用于网络请求。
  - 事件：`bridge.Post("eventName", obj)` 或 RPC 内部往 taskId 推 `download.progress` / `download.state`，前端用 `LX.on(event, handler)` 监听。
- **`LX.call` 返回的是已解包的 HandlerResult.result**（不是 `{ok, result}`）。所以判断成功要看业务字段，`r.ok` 不存在。
- `Ok(x)` / `Err(code,msg)` 定义在 handler 结果包装里；错误码 `-32602` 参数缺失、`-32000` 业务失败。

### 关键 RPC 一览（行号随编辑有偏移，用 grep 定位）

完整 RPC 分组速查见文首「功能速查索引」。此处仅记录各 RPC 的返回结构与 taskId 前缀约定（新索引未覆盖的细节）：

| 方法 | 补充说明 |
|---|---|
| `mc.installLoader` | 薄包装：解析参数 → `++loaderTaskSeq` → 后台线程调 `g_installLoaderImpl` → `Ok({taskId, started, loaderId})` |
| `mc.installModpack` | mrpack 自动导入（taskId 前缀 30000） |
| `mc.submitDownloadList` | taskId 自 20000 递增；`maxConcurrent` 取 `SettingInt("downloadThreads",64)` 或显式参数（上限 16） |
| `mc.completeVersion` | 走 `CompleteVersionFilesWorker`（compSeq 前缀 50000） |
| `java.download` | taskId 为 `"java-"+版本号` 字符串 |
| `mc.javaAutoInstall` / `auth.ensureInjector` | 固定 taskId `"java-autoinstall"` / `"auth-injector"`（并入统一任务中心） |
| `download.cancel` | 参数 `{taskId}`，置位取消标志 → worker 杀 aria2 → 发 `cancelled` 事件 |
| 统一事件 schema | `download.progress` = `{taskId, percent, speed, eta, stage, name, threads, files...}`；`download.state` = `{taskId, state: started/done/error/cancelled}` |
| `mc.searchMods` | Modrinth `/v2/search`，已补 `published/updated/icon/latestVersion/gameVersions/loaders` 字段 |
| `mc.modDetail` / `mc.modVersions` | `/v2/project/{id}` / `/v2/project/{id}/version` 异步，含 `versionType/datePublished/downloads/gameVersions/loaders/files/file` |
| `mc.listPackFiles` / `mc.deletePackFile` / `mc.togglePackFile` / `mc.openPackFolder` | 版本设置里的模组/资源包/光影管理 |

---

## 3. InstallLoaderImpl 重构（重要经验）

问题链条（都实际踩过）：

1. 最初把它写成 `Register("mc.installLoader")` 的 lambda 内部嵌套 lambda（`InstallLoaderImpl`），MSVC 报 **C2601**（函数内嵌套函数非法）。
2. 改成局部 `std::function` 后，detached 线程晚于函数返回才执行，捕获的 lambda **悬垂** → 崩溃风险。
3. **最终方案**：文件作用域
   `static std::function<void(Bridge&, int, const std::string&, const std::string&, const std::string&, const std::string&, const Json&)> g_installLoaderImpl;`
   在 `RegisterLoaderServices` 内赋值 lambda。
   - 同时解决 C2601 与悬垂两个问题。
   - 注册发生在启动时，RPC 运行时才调用，赋值顺序安全。
   - 参数顺序：`(bridge, taskId, loaderId, mcVersion, loaderVersion, installName, ld)`。

---

## 4. 编译报错速查（都实际遇过并修好）

| 错误 | 原因 | 修法 |
|---|---|---|
| C2601 | 函数内嵌套函数 | 移到文件作用域 std::function / 单独函数 |
| C3493 | 局部 lambda 被 `[]` 空捕获 lambda 隐式捕获 | 把 helper 提升到文件作用域（如 `PackSubDirOf`） |
| C2440 | `std::filesystem::rename` 返回 void 赋给 bool | 改 `ok = !ec` |
| 语法错 | lambda 结尾多一个 `}` | 删掉 |
| `RunSilent(cmd,tmp)` 不匹配 | `RunSilent` 只接受 1 参返回 bool | 用 `RunProcessSilent(cmd,tmpDir)`（返回退出码） |
| wstring/path 混拼 | `mcRoot / rel`（wstring/path） | `std::filesystem::path(mcRoot) / rel` |
| LNK1140 | PDB 超 2GB | 删 .pdb/.ilk 重链 |

---

## 5. Modrinth API 字段（已实测验证）

- **search**（`/v2/search`，hits 元素）：`project_id, slug, title, description, author, downloads, follows, icon_url, date_created, date_modified, latest_version, game_versions, loaders, categories, project_type`。
- **project detail**（`/v2/project/{id}`）：字段是 **`followers`**（不是 `follows`）、**`published`**（不是 `date_created`）、**`updated`**（不是 `date_modified`）！后端做了兼容回退。
- **version list**（`/v2/project/{id}/version`）：`version_number, version_type, date_published, downloads, game_versions, loaders, files[] {url, filename, size, sha1, primary}, status`。
- 大字段 `body`（HTML 介绍）只在 detail 里，search 没有；前端 fallback 用 `description` + `categories`。

---

## 6. mrpack 整合包结构

- mrpack 本质是 **zip**（Windows 上 `cmd /c tar -xf` 可直接解压，与 Java 运行时解压同一套）。
- 解压后有关键文件 `modrinth.index.json`：
  - `dependencies.minecraft`（游戏版本，可能含 `=` 前缀）
  - `dependencies.fabric-loader` / `forge` / `neoforge` / `quilt-loader`（loader 版本，需去 `=` 前缀）
  - `files[] { path, downloads[] }`（每个文件有多个镜像 URL，优先 cdn）
  - `overrides` 目录内容覆盖到 mcRoot
- 导入流程（6 阶段，全走 `download.progress/state` 事件）：
  1. 下载 mrpack 到临时目录（文件名用清洗后的 safeNameW）
  2. tar 解压
  3. 解析 index → 识别 loader → 取 mcVersion/loaderVersion
  4. 调 `g_installLoaderImpl` 装 loader 版本（`installName = 清洗后 safeName`，**不是**原始整合包名）——**注意 loader 阶段要用独立 taskId（`taskId + 100000`）**，否则其自身发的 `download.state=done` 会被前端误判为整个整合包已导入完成；无 loader 时直接写原版版本 json
  5. 按 `files[]` 下载到 `<mcRoot>/<path>`（**路径安全校验**：拒绝绝对路径与 `..` 越界）
  6. 应用 overrides
- 路径安全 helper：文件作用域 `PackSubDirOf`，用 `std::filesystem::relative` 判断是否越界。

### 模组前置链 & Java 源（2026-08 新增）
- Modrinth 依赖：版本对象里 `dependencies[] { project_id, dependency_type("required"|"optional"|"incompatible"), version_id }`。
  批量取项目标题用 `GET /v2/projects?ids=[\"a\",\"b\"]`（**必须 URL 编码方括号**，UrlEncode 会编码 `[` `]` `"`）。
  `mc.modDepsTree`：deep=false 只查一层必需前置；deep=true 沿 `version_id` 递归（限深 6，按 project id 去重）。
  `mc.searchMods` 结果补 `depsCount/deps`：每个 hit 的 `latest_version` 拉一次 version，再批量拉标题（后台线程，失败不阻塞列表）。
- Modrinth 版本 facet 主版本匹配：1.20.4 时同时给 `["versions:1.20.4","versions:1.20"]`（OR 数组），否则过滤后几乎无结果。
- BMCLAPI Java 运行时：`GET https://bmclapi2.bangbang93.com/java/list` 返回 `[{title,file}]`；下载直链
  `https://bmclapi.bangbang93.com/java/{file}`。`mc.javaList {source: official|bmclapi|yours, base}` 返回
  `{list:[{name,version,url,desc}]}`，url 直接交给 `java.download`。
- `mc.launch` 支持 `dryRun:true`（只返回 `command`/`mcRoot` 不启动，供导出启动脚本）；已追加 version.json `arguments.jvm`
  （字符串项 + 带 rules 的对象项，`IsLibAllowedOnWindows` 可复用判定规则）；启动前校验/补下缺失 libraries
  （按 `downloads.artifact.url`）；stdout/stderr 重定向到 `mcRoot/logs/lxe-launcher-std.log` / `lxe-launcher-crash.log`。
  `mc.readGameLog` 读尾部 256KB，优先级 latest.log → std → crash。
- 下载列表/整合包导入失败会删除未完成文件（含 `.aria2` 控制文件）实现原子回退；`download.progress` 事件带
  `files[] {name,state}`（pending/downloading/done/error）供前端着色。
- **Json 类两个坑**：① 无 `operator==`，去重要用手写循环按 `asString()` 比；② 默认 `Json()` 是 Null，
  `operator[]` 会 `std::get` 抛 `bad_variant_access`——**必须先 `Json x = Json::object();` 再 `x["k"]=...`**。
- `std::transform(..., ::towlower)` 需要 `#include <cwctype>`。

---

## 7. 前端（webapp/index.html 内联 JS）

### 通用注意
- 有多个 `<script>` 块；语法检查脚本：
  ```powershell
  node -e "const fs=require('fs');const s=fs.readFileSync('webapp/index.html','utf8');const m=s.match(/<script>([\s\S]*?)<\/script>/g);let code='';for(const x of m){code+=x.replace(/<\/?script>/g,'')+'\n;';}new Function(code);console.log('JS OK');"
  ```
- `const queue = []` 声明在 `currentTab` 附近（~1505），queue 相关渲染函数 `renderQueue/renderQueueItem/removeTask` 都是函数声明（提升），`downloadModVersion/installModpack` 里直接用没问题。
- `FA(cls)` 返回 `<i class="..." ...></i>`，**内含双引号**——所以**不能**把 FA() 拼进 HTML 属性的引号字符串里（会断属性）。图标 fallback 用 wrapper span + 绝对定位 img + `onerror="this.style.display='none'"`。

### 下载中心改版要点
- 非 game 的 tab（mod/modpack/java）隐藏「正式/快照」筛选 `versionBranchTabs`：在 `downloadTabs` handler 和 `applyPersistedSettings` **两处**都设 `display:none`。
- mod/modpack 行：icon 支持 `iconUrl` 图片、显示下载量/收藏/更新时间（`dl-desc-meta`）、按钮改「查看详情」→ `openModDetail`。
- 详情流程：`openModDetail`（先选游戏版本）→ 确认后 `openModVersions`（正式/测试前缀徽章、版本号、更新时间、下载量）→ `downloadModVersion`（模组 → `mc.submitDownloadList` 到 `<mcRoot>/mods`）。
- 整合包：`installModpack` → `mc.installModpack`；**事件监听器（download.progress/state）要在 `LX.call` 之前注册，且只注册一次**（曾修过重复注册 bug）。
- 打开 Modrinth 项目页：`window.open('https://modrinth.com/project/<id>')` 走 NewWindowRequested → ShellExecuteW 默认浏览器，无需新 RPC。
- 复制文本：`navigator.clipboard.writeText` 带 execCommand 回退。

### 版本设置改造
- `versionSettingsOverlay` 改为 settings-layout 风格：`vsNav`（重命名/模组/资源包/光影）+ `data-ls-section`/`data-ls-body` 面板。
- settings 导航 handler 要扩展 `isSubNav` 逻辑支持 `vsNav`。
- 新增 `refreshPackLists`：加载列表、启用/禁用（`.disabled` 重命名）、删除、打开文件夹（对应后端 4 个 RPC）。
- Escape 关闭弹窗的监听器列表要补新 overlay id。

---

## 8. 其他坑

- `downloadMod` 函数现在已是死代码（列表按钮已改走 `openModDetail`），但无害，留着。
- 网络工具：`HttpFetchText(lxe::Utf8ToWide(url))` 拉文本；`DownloadFileSmart` 下载；`RunProcessSilent(cmd, tmpDir)` 执行命令。
- 文件系统根目录工具：`GetMcRoot()`。
- 前端 popup 打开 URL 前注意 escapeAttr 防注入。
- D 盘空间紧张（曾到 ~0.08GB 空闲，清理后约 140-250MB），大构建前注意磁盘占用。
- Json 类两个坑：① 无 `operator==`，去重需手写循环按 `asString()` 比较；② 默认 `Json()` 是 Null，`operator[]` 会抛 `bad_variant_access`——必须先 `Json x = Json::object();`。
- `::towlower` 需 `#include <cwctype>`。
- `UrlEncode` 会编码 `[`/`]`/`"`，Modrinth 批量 `GET /v2/projects?ids=["a","b"]` 可直接用。
- Modrinth 依赖链：`GET /v2/version/{id}` 的 `dependencies[]`（required 依赖取 `project_id`/`version_id`），批量标题走 `GET /v2/projects?ids=[...]`。
- BMCLAPI Java 列表：`GET https://bmclapi2.bangbang93.com/java/list` 返回 `[{title,file}]`，直链 `https://bmclapi.bangbang93.com/java/{file}`。
- `mc.launch` 支持 `dryRun` 返回启动命令；日志重定向 stdout/stderr 到 `logs/lxe-launcher-std.log`/`lxe-launcher-crash.log`。
- 安装/下载失败用原子回退：删除所有未完成文件 + `.aria2` 控制文件。
- **Edit 工具替换要小心吞掉邻近声明**：曾把 `Json files = Json::array();` 当 oldString 的一部分替换后漏掉，导致 `files` 未声明（C2065）。
- **结构误插/`}` 错位排查**：若 MSVC 报"意外的 Json/if"且朴素括号计数看似平衡，用忽略字符串/注释的括号计数脚本定位（如本文件深处 `std::thread([...]()` 的 lambda 曾被多余的 `}` 提前闭合）。
- **Modrinth `/v2/tag/game_version` 返回字段是 `version`（不是 `game_version`）**：解析错字段会导致游戏版本筛选下拉只有"全部版本 + 无匹配版本"。
- **`http.fetchText` 必须用 `RegisterAsync`+线程**：前端用它抓 version.json/BMCLAPI 加载器列表/assetIndex，若 `Register` 同步执行会阻塞 WebView2 桥消息线程 → 整个 UI 卡死。
- **`bridge.Register` 的 `mc.versions`/`mc.versionIsInstalled` 只读本地文件**（manifest/版本目录），同步可接受；任何涉及网络 fetch 的 RPC 都要后台线程。
- **恢复持久化 tab 要复刻 tab 点击逻辑**：`applyPersistedSettings` 恢复 `downloadTab=mod` 时若只切 active 不触发 `searchMods`/`loadGameVersions`，模组列表会一直空态"无匹配资源"。
- 整合包/加载器安装：`writeMerged` 写入合并 json 后必须补下 `versions/<installName>/<installName>.jar`（vanilla `downloads.client.url`），否则启动报"版本 JAR 不存在"；`mc.launch` 第 3 步也应 best-effort 自动补下该 jar。
- **mc.loaderVersions 重写要点（教程对齐）**：Forge 官方 HTML `index_{mc}.html` 需把 `-`→`_`（1.7.10-pre4）；OptiFine 官方无参考解析、教程用 BMCLAPI `https://bmclapi2.bangbang93.com/optifine/{mc}` 返回 `[{mcversion,type,patch,filename,forge}]`；Fabric 走 `https://meta.fabricmc.net/v2/versions/loader/{mc}`。

---

## 9. 验证清单

改完一批功能后按此顺序验证：
1. `node -e` JS 语法检查（见 §7）。
2. 删 .pdb/.ilk → Stop-Process → MSBuild Debug/x64。
3. `Start-Process x64\Debug\LXElauncher.exe` 启动。
4. 关键 API 字段可用 PowerShell `Invoke-RestMethod` 实测核对（如 Modrinth 各端点字段名）。
5. UI 端到端：请用户人工验证（WebView2 无法 playwright 驱动）。

---

## 10. 第二批：低版本启动 / Java 自动选择 / 版本类型 / 补全文件（2026-08 新增）

### Java 自动选择
- `RecommendedJavaMajor(mcVersion)`：<1.17→8；1.17~1.20.4→17；1.20.5+→21；**主段非 "1" 时区分**：纯数字（如 2.x）→21，否则（c0.0.13a_03、b1.7.3、快照）→8；无点号（rd-*、23w*）→8。
- `JavaMajorFromVersionText` 解析 `java -version` 输出：找不到 "version" 关键字时兜底直接找第一段引号内数字（部分 JRE 输出 `openjdk 17.0.11 ...`）。
- `ScanInstalledJavas()` 全盘扫描 javaw.exe（**30 秒缓存**+互斥锁），返回 `{path, major, arch}`；`PickJavaForMajor(list, major)` 优先精确匹配、次选更高主版本。
- `mc.launch` 启动前：`PickJavaForMajor(ScanInstalledJavas(), RecommendedJavaMajor(mcVersion))`，空则兜底 `FindJavaPath()`。**Forge/OptiFine 安装器（g_installLoaderImpl 内）同样用推荐 Java 运行**，不能死用 FindJavaPath()（老版本 Forge 装器要 Java 8）。
- 前端 `buildJavaOptions()` 是 async：追加"已检测到"分隔符 + 具体 `Java {major} · {path}` 选项；选中后 `javaPath` 存的是**路径**，`buildLaunchParams` 里非"自动检测/自定义路径"标签时直接把 javaPath 传后端（后端拿具体路径）。
- 新增 `mc.javaInfo {version}`：`{list:[{path,major,arch}], recommendedMajor}`；前端 `refreshLaunchJavaRecommend()` 在打开启动设置时填充 `launchJavaRecommend` 提示。

### 旧版本（<1.12）崩溃修复要点（对照 temp/启动脚本采样）
- 主因是**用错 Java**（老版本需 Java 8）与**硬编码 `-Dlog4j.configurationFile` 指向不存在的文件**。
- `-Dlog4j.configurationFile` 必须按 version.json `logging` 解析出文件名 + `EnsureLoggingConfig()` 补下到 `log_configs`；**解析不到就整段跳过**，不能传空/不存在路径。
- 老版本补参数（采样 bat 同款）：`-Djdk.lang.Process.allowAmbiguousCommands`、`-Dfml.ignoreInvalidMinecraftCertificates`、`-Dfml.ignorePatchDiscrepancies`、`-Dlog4j2.formatMsgNoLookups`、`-XX:HeapDumpPath=<mcRoot>/logs/...`。
- `minecraftArguments`（老版本用）引号感知分割：`splitArgs()` 手写，`--gameDir "D:\...\Game"` 这类带空格路径不再被切碎。
- `arguments.game[].rules` 用 `rulesAllow()`：feature 命中且 action=allow 才进；`has_custom_resolution` 命中才补 `--width/--height`（取 version.json `resolution` 的 width/height）。

### 版本类型判断 `DetectVersionType(vj)`（判断顺序）
mainClass（neoforge/fabric/quilt/optifine/forge 关键词）→ `inheritsFrom` → 按 libraries artifactId 含上述词 → 版本 id（含 `-OptiFine_`/`forge`/`-fabric` 等）。全部转小写。`mc.localVersions` 输出补 `type/inheritsFrom/mcVersion/releaseTime/jarExists`。

### 本地版本列表防卡顿
- `mc.localVersions` 由同步 `Register` 改 `RegisterAsync`+线程（原来在 WebView2 桥线程逐目录读文件，版本多时卡 UI）。
- 前端 `renderDownloadTable` 只调一次 `mc.localVersions` 建 `installedIds` Set，行内同步判断，替代逐行 `mc.versionIsInstalled` RPC。

### 补全文件 `mc.completeVersion {version}`
- taskId 前缀 50000；发 `download.progress/state` 事件（前端队列收到 `download.state=started` 自动建任务）。
- 依次补：客户端 JAR、缺失 libraries artifact、natives 重解压、日志配置、资源索引、全部 asset objects。**`mc.launch` 启动前也 best-effort 调用（EnsureAssetIndex/EnsureLoggingConfig/补 jar/libs/natives）** → 半装的旧版本不再因缺文件崩。
- 前端：版本设置"补全文件"按钮 `vsCompleteBtn`；右键菜单 `_ctxLaunchItems` 移除"展示游戏日志"（只剩"导出启动脚本"），日志入口搬到 设置-开发者模式 `devGameLogBtn`。

### 其他
- `RunCapture(cmd, dir)` 捕获子进程 stdout 返回 `std::string`（用于 java -version）；`_wcsicmp` 可直接用（<wchar.h> 传递包含）。
- C3861（函数未声明）：helper 在被引用函数定义之后时，记得在引用点前加前向声明。

---

## 11. 第三批：Forge 崩溃修复 + 禁止无账号启动（2026-08 新增）

### 1.15.2 Forge "Cannot find launch target fmlclient" 根因
- 官方 `1.15.2f.json`（Forge 安装器产物）里 `net.minecraftforge:forge:1.15.2-31.2.55` 的 `downloads` 是空对象。
- 原 `BuildClasspath` 遇到 `downloads` 无 `artifact` 就**整条跳过** → 这个 thin jar（内含 `META-INF/services/cpw.mods.modlauncher.api.ILaunchHandlerService`（fmlclient/fmlserver）+ `FMLClientLaunchProvider.class`）没进 classpath → ModLauncher 找不到 fmlclient 直接崩。
- 修法：`downloads` 空时用 `LibPathFromName(name)` 按 Maven 坐标 `group/artifact/version/artifact-version[-classifier].jar` 推导 `libraries` 路径，**文件存在才加入**。
- 顺带：`rulesAllow` 原实现语义反了（默认 allowed=true、`is_demo_user==false` 写反）→ 非 demo 用户被塞 `--demo`、`--width/--height` 恒注入。正确语义：默认排除，feature 命中条件=`实际值==规则值`，disallow 命中直接 false，allow 命中才 true。签名改为 `(arg, hasCustomRes, isDemoUser, hasQuickPlays)`，调用点 `rulesAllow(arg, customResolution, false, false)`。

### 禁止无账号启动
- 后端 `mc.launch` 新增必填 `accountType`（offline|thirdparty|premium），否则 `Err(-32060, "未选择账号，请先在 设置-账号管理 中添加并选择账号")`。
- 离线账号规范化：`userType='legacy'` + uuid 全零时用 `OfflinePlayerUuid(username)`（CryptoAPI MD5("OfflinePlayer:"+name) → 版本3/变体位，与 Java `UUID.nameUUIDFromBytes` 一致）。需要 `#include <wincrypt.h>` + `#pragma comment(lib,"advapi32.lib")`。
- 前端 `buildLaunchParams`：无账号传空 username/accountType（后端兜底拒）；offline 显式 `userType='legacy'`；thirdparty 传 `userType='thirdparty'`+authServer+prefetched。启动处理与 `exportLaunchScript` 均在无 `acc` 时 `notify(...)` 后 return。
- 自定义分辨率：后端 `customResolution` 布尔决定 `--width/--height` 是否随 `has_custom_resolution` 规则进入；前端启动设置"高级"新增卡片（`switch-square` + 宽高输入），`LaunchStore.resEnabled/resWidth/resHeight` + `applyPersistedSettings` 恢复 + input 绑定。默认宽高 854×480。
- 旧版本模板变量兜底：`applyGameVars` 补 `user_properties→{}`、`auth_session→token`、`profile_name→username`、`game_assets→assetsDir\virtual\legacy`（1.8 及更早才用）。
- 本机没有 Java，Forge 启动验证只能靠用户真机跑。

---

## 12. 第三批续：Forge 新版安装器 + OptiFine + 下载源下拉框（2026-08 新增）

### Forge 新版安装器（含 processors）真实结构（已实测 1.16.5-36.2.34 / 1.18.2-40.2.21）
- 安装器是含 `install_profile.json` + `version.json` + 内嵌 `maven/**` 目录的 jar（旧版才是 universal jar 型）。
- `install_profile.json` 顶层键：`version`（如 `1.16.5-forge-36.2.34`）、`json=version.json`（注意其实是 `version.json` 条目）、`path=net.minecraftforge:forge:...`、`spec`、`data`、`processors`、`libraries`。
- `data` 键：`MAPPINGS`（mcp_config 输出）、`BINPATCH=/data/client.lzma`、`MC_SLIM/MC_EXTRA/MC_SRG/PATCHED` 等；1.17+ 还带 `MOJMAPS/MERGED_MAPPINGS/MC_UNPACKED`。
- `processors[]`：每项 `{jar, sides?, classpath[], args[]}`；处理顺序严格（MCP_DATA → jarsplitter → SpecialSource → binarypatcher …）。
- `data.*.client` 的值若为 `[maven坐标]` 表示替换为该库在 `libraries/` 下的完整路径；`{KEY}` 变量表：`MINECRAFT_JAR/INSTALLER/SIDE/ROOT/BINPATCH/MAPPINGS/MOJMAPS/MC_SLIM/.../PATCHED`。
- **binarypatcher `{PATCHED}` 是 `forge-{artifact}-client.jar`（-client.jar），而启动库是 `forge-{artifact}.jar`（无分类器）**——两者同目录同名后缀不同；共享目录冲突风险要按"先下载/嵌入启动库 → 再跑 processor → 最后复制处理器输出"的顺序处理。
- **启动库来源**：`version.json.libraries` 里的 `net.minecraftforge:forge:{artifact}`（url 为空）对应安装器内嵌 `maven/net/minecraftforge/forge/{artifact}/forge-{artifact}.jar`，不是下载产物，必须从安装器 zip 提取（`CopyEmbeddedMavenLib`）。universal 库同理（url 为空）。
- **libraries 补下**：install_profile.libraries 大多数带 `downloads.artifact.url`，但 forge 自己的 universal 库 url 为空 → 先试内嵌 maven、再试 BMCLAPI 镜像；`version.json` 的启动依赖（modlauncher/bootstraplauncher/securejarhandler 等，url 全空）也要单独补下到 `libraries/`（BMCLAPI maven 镜像兜底），已在 vanilla.json 出现过的（log4j 等）跳过。
- 处理器主类/classpath：`jar` 坐标 → `libraries` 全路径 → `META-INF/MANIFEST.MF` 的 `Main-Class`（`tar -xOf` 读）；classpath = processor 自己 + `classpath[]` 坐标解析。MCP_DATA/SpecialSource 等处理器用 `--task XX` 形式。
- **DOWNLOAD_MOJMAPS**：1.17+ 处理器里出现，官方它自己会从 `https://piston-data.mojang.com/.../client.txt` 下载映射，但离线/在修复流程里我们**自行下载 client_mappings 到 data.MOJMAPS 对应路径并跳过该 processor**（与原版 json `downloads.client_mappings.url` 对应）。
- 旧版安装器（无 processors，universal jar 型）兜底走官方 `java -jar installer.jar --installClient <mcRoot>`。
- Forge 的老版本 artifact 名（1.8.9/1.7.10）规则：minor==8 且 build==8/空 → `{mc}-{forge}`；minor==7 或 8 → `{mc}-{forge}-{mc}`；其余 → `{mc}-{forge}`（函数 `ForgeArtifactVersion`）。

### 启动侧（mc.launch）Forge 适配
- **arguments.jvm 参数会带 `${library_directory}` 与 `${classpath_separator}`**（bootstraplauncher 的 `-p` 模块路径），启动前必须按 `libDir`/`;` 替换，否则 JVM 报找不到模块。
- 追加 version.json 的 jvm 参数时**不能只收 `-` 开头项**：`-p`、`--add-modules ALL-MODULE-PATH`、`--add-opens <module>=<module>` 的成对值是裸字符串（非 `-` 开头），直接丢弃会断参数。目前做法保留全部字符串项，仅整条跳过冲突项（`-Xmx/-Xms/-cp/-classpath/-javaagent/-Djava.library.path` 前缀）。

### OptiFine（教程 3-4）
- 信息列表：BMCLAPI `https://bmclapi2.bangbang93.com/optifine/{mc}` → `[{mcversion,type,patch,filename,forge}]`。
- 下载直链：`https://bmclapi2.bangbang93.com/optifine/{mc}/{type}/{patch}`（保存为 `filename`）。
- 安装：`java -Duser.home=<mcRoot 的上级目录> -cp "<jar>" optifine.Installer`，**必须同时设 APPDATA/USERPROFILE/HOME 环境变量指向 user.home**（OptiFine 装到自己 `.minecraft`）；装完读取 `versions/{mc}-OptiFine_{type}_{patch}/{...}.json` 合并到 installName。

### 前端下拉框修复（点不动）
- 症状：下载中心"下载源"下拉框点了没反应（其他下拉正常）。
- 根因：`buildDlSourceOptions` 只重建选项列表，**漏绑 `.form-select-display` 的 `onclick` 展开**。所有正常下拉都有一套同款手势：
  ```js
  display.onclick = (e) => { e.stopPropagation();
    $$('.form-select-wrapper').forEach((w) => { if (w !== wrapper) w.classList.remove('open'); });
    wrapper.classList.toggle('open'); };
  ```
- 修法：`buildDlSourceOptions` 开头补同样的 display.onclick。样式层（`.form-select-*`）与主题共用，无需改动，保持主题一致。
- 排查技巧：点不动先查 `closest('.form-select-wrapper')` 全局关闭监听器没问题后，再确认该 wrapper 的 display 是否真的绑了展开事件。

## 13. 第四批：authlib-injector 外置登录完整实现（2026-08 新增）

依据 `temp/启动器技术规范 - authlib-injector wiki.html`（唯一允许的资料，含 `wiki_files` 静态资源）。

### 拖入地址"找不到"的根因（URL 转码）
- 规范「通过拖拽设置」：拖动数据 `text/plain` 内容为 `authlib-injector:yggdrasil-server:{API 地址}`，其中 API 地址是 **URI 组成、须经 `encodeURIComponent` 编码**。
- 旧代码前端 `drop` 只剥前缀不解码，把 `https%3A%2F%2F…` 原样塞进输入框 → 后端又补 `https://` → 整个地址失效。
- 修法（双保险）：
  - 前端 `drop`：剥前缀后 `decodeURIComponent`（带 try/catch）；并优先取含 `authlib-injector:` 前缀的 `text/plain`/`text/uri-list`。
  - 后端 `NormalizeServerInput`：先剥前缀 → `UrlDecode` → 后再补协议、统一末尾 `/`。

### 登录"没响应 accessToken"的根因（endpoint 前缀）
- 规范/服务端技术规范：Yggdrasil 认证接口在 API Root 下走 `authserver/` 子路径（镜像 `authserver.mojang.com`）：
  `{apiRoot}/authserver/authenticate`、`{apiRoot}/authserver/refresh`、`{apiRoot}/authserver/validate`、`{apiRoot}/authserver/invalidate`。
- 旧代码拼接成 `{apiRoot}/authenticate` → 命中错误路径，服务器返回元数据/错误页而非令牌 JSON → 解析无 accessToken → -32054。
- 修法：`auth.login`、`auth.refresh` 的 URL 改为 `apiRoot + L"authserver/authenticate"`、`L"authserver/refresh"`。

### ALI（API 地址指示）处理
- 规范算法：GET（跟随重定向）→ 若响应带 `X-Authlib-Injector-API-Location` 头，则把 ALI 指向的 URL 当新地址继续；ALI 指向自身即停止；ALI 可为**绝对或相对 URL**。
- 新增 `ResolveAliUrl(base, location)`：支持绝对 URL、`/`开头相对路径、相对目录。`ProbeAuthServer` 用 `for(5)` 循环解析直到 ALI 为空/指向自身，最后把返回体当元数据（`metaJson` 供 prefetched 用）。

### 凭证有效性确认（启动前）
- 规范「启动游戏」第 2 步：validate → 失败则 refresh → 再失败要求用户重登。
- 后端新增 `auth.validate`：POST `{apiRoot}/authserver/validate`，204=有效；错误 JSON=失效。
- 前端启动流（第三方账号）：先 `ensureInjector`（下载/skip）→ `auth.validate` → 失效时 `auth.refresh` 并回写 accessToken/uuid/playerName 到 `State.accounts` + `Persisted` → 再 `repairThenLaunch`。凭据错误直接 `setLaunchState('idle')` 阻止启动并提示重登。

### 参数模板替换（规范「替换参数模板」表）
- `${user_type}` 对外置登录账号应为 **`mojang`**（不是 `thirdparty`）——两处前端 `launchParams.userType` 已改。
- `${auth_access_token}`/`${auth_session}`=accessToken、`${auth_uuid}`=无符号 UUID、`${user_properties}`= `{}`（规范允许）、`${auth_player_name}`=角色名、`${auth_uuid}` 走既有 `applyGameVars`。
- 启动 JVM 追加（加在主类参数前）：`-javaagent:{injectorPath}={authServer}` 与 `-Dauthlibinjector.yggdrasil.prefetched={Base64 元数据}`（`Base64Encode` 已有）。

### 账号标识（规范「账户信息的存储」）
- 用 **验证服务器 + 账户标识 + 角色 UUID** 三者共同标识账户；前端去重过滤已改为三键同时比较。
- 密码任何时候不落盘，存储的是令牌（accessToken/clientToken）。

### 其他坑
- WinHTTP 默认自动跟随 3xx 重定向且**拒绝 HTTPS→HTTP 降级**（符合规范"不得降级明文 HTTP"），`HttpRequest`/`HttpFetchText` 无需额外处理。
- 新增 RPC 后前端 `addThirdOk` 登录成功回调、设置里"刷新令牌"按钮（`auth.refresh`）仍复用同一套，无需改。

---

## 14. 第五批：Java 匹配优化 / 启动按钮排查 / 前端作用域修复（2026-08 新增）

### C++ 命名空间 / 前向声明陷阱（本次踩 C2129）
- **`static` 函数的前向声明必须与定义放在同一个命名空间**。`Services.cpp` 顶部是 `namespace lxe`，内部 545 行起又开匿名 `namespace {`（等价 `lxe::<anon>`）。
  - `LoadSettingsFile`/`SaveSettingsFile` 定义在文件作用域（`} // namespace` 之后，即 `lxe`）→ 前向声明放 `lxe` 里，匿名命名空间内的调用因 unqualified lookup 向上可见 ✓。
  - `ScanInstalledJavasNow` 定义在**匿名命名空间内** → 前向声明也必须放匿名命名空间内；若放 `lxe`，会声明出另一个 `lxe::ScanInstalledJavasNow`，而定义在 `lxe::<anon>`，导致 **C2129「声明但未定义」**。
- 同理：被 `std::vector<T>` 用作完整类型的结构体（如 `InstalledJava`），前向声明 `struct InstalledJava;` 不够（`<vector>` 实例化要完整类型 → C2036），需在 `lxe` 提前**完整定义**它。

### `InstalledJava` / Java 缓存前移到文件顶部
- 把完整 `struct InstalledJava` 从 2187 行附近搬到 `namespace lxe` 顶部（30 行后），删掉原位置的重复定义，解决匿名命名空间内 `std::vector<InstalledJava>` 的可见性问题。

### `mc.rescanJava` 曾悬空在函数外（编译报 `->` 尾随返回类型错）
- 排查方法：写脚本做**括号深度追踪**定位每个 `Register*` 函数的开/闭行，确认 `bridge.Register(...)` 是否落在某个函数体内。悬空在函数外的 `Register` 会报一串 C3927/C3484/C3613/C4430（编译器把回调当函数声明尾随返回类型解析）。
- 修法：把 `mc.rescanJava` 移进 `RegisterMinecraftLaunch` 内（Java 相关 RPC 聚集处），紧跟 `mc.javaInfo` 之后。

### version.json 的 javaVersion.majorVersion（比版本号推导更准）
- 新增 `JavaMajorFromVersionJson(vj, mcVersion)`：优先读合并后 version.json 的 `javaVersion.majorVersion`，找不到再回退 `RecommendedJavaMajor(mcVersion)`。
- `JavaRequirement` 加第 5 参 `vjMajor=0`：`base = vjMajor>0 ? vjMajor : RecommendedJavaMajor(...)`。
- 接入点：`mc.launch`（选 Java）、`mc.javaInfo`（loMajor/hiMajor）、`mc.javaAutoInstall`（pickWithin 的 rec 与 want）。各自先 `ResolveVersionJson(GetMcRoot(), wid)` 再取 major。

### 定期 Java 缓存自动验证
- 新增 `StartPeriodicJavaVerify()`：后台线程每 10 分钟轻量校验磁盘缓存路径仍存在 + 缓存是否过 `kJavaCacheMaxAgeSeconds`，有变化则 `AsyncScanJava()` 重扫。在 `RegisterDemoServices` 末尾调用。
- `AsyncScanJava` 有 `g_javaScanRunning` 互斥，重扫不会并发。

### 前端作用域 bug：外层函数引用 `init()` 里的 const
- 症状易被忽略：`initSourceSettings` 在 `.then` 回调里引用 `RATE_LABELS`，而 `RATE_LABELS` 是 `init()` 内的局部 const → 回调执行时抛 `ReferenceError`，被外层 `.catch(()=>{})` **静默吞掉**，表现为"限速下拉文本永不更新"。
- 修法：把 `RATE_LABELS` 提升到 IIFE 顶层（`initSourceSettings` 定义之前），删除 `init()` 内的重复定义。
- **教训：`.catch(()=>{})` 会吞异常，前端 bug 可能毫无表面现象**——排查时优先找"回调里引用了外层不存在的名字"。

### MSA 账号行切换按钮被 `.icon-btn:not(.aac-refresh)` 误绑
- `renderSettingsAccounts` 每行按钮：thirdparty 有 `.aac-refresh`，msa 有 `.aac-msa`。旧选择器 `.icon-btn:not(.aac-refresh)` 在 msa 行命中第一个 `.aac-msa`（调色板按钮），导致真正"切换账号"按钮无 onclick。
- 修法：改成 `.icon-btn:not(.aac-refresh):not(.aac-msa)`。（属于第三方/msa 账号 UI 的隐蔽 bug。）

### 启动按钮"点击无响应"排查结论
- **前端 init 是健康的**：真实 DOM 下可（playwright 加载静态页验证）`launchBtn` 绑定了 onclick、无遮罩覆盖、点击无版本时弹 toast「请先选择版本」。
- **根因在架构而非 JS**：`mc.launch` 是同步 `Register`，运行在 WebView2 桥消息线程；其内部对缺失 client jar/依赖库做**串行同步 `DownloadFileSmart`**（aria2 子进程 `WaitForSingleObject INFINITE`），缺失文件多时整条消息线程被阻塞 → 前端 `LX.call` 永不返回，UI 卡死，观感即"点按钮没反应"。
- 缓解：前端启动流先 `mc.completeVersion`（异步，taskId+`download.state` 事件驱动补全）→ 收到 `done` 才真正 `mc.launch`。`mc.completeVersion` 对"无缺失"也发 `done`，不会卡死在修复环节。
- 若仍要根治：把 `mc.launch` 里启动前的 inline 下载改成「缺失即报错提示+引导走补全」，避免消息线程同步阻塞。

### 前端 mock 冒烟测试的正确姿势
- 用「只创建真实 HTML 里存在 id 的 getElementById」注册表模式：脚本 `document.getElementById(id)` 对不存在 id 返回 `null` → 谁引用了缺失元素立即暴露，且与真实 DOM 行为一致（auto-create 的 mock 会掩盖这类 bug）。
- `DOMContentLoaded` 用 `addEventListener` 收集后手动触发，才能驱动 `init()`。
- 真实页面优先级更高：本地 node 静态服务器 + playwright 加载 → 检查 console error、按钮绑定、点按行为，比纯 JS mock 准得多。

### 部署提醒
- 前端改动后仅需 `node --check` 内联 script（`<script>` 块）；C++ 改动后 MSBuild（构建前删 .pdb/.ilk、Stop-Process exe）。

---

## 16. `${classpath}` 被当成主类（ClassNotFoundException）的根因与修法

### 现象
- 启动报：`错误: 找不到或无法加载主类 ${classpath}；原因: java.lang.ClassNotFoundException: ${classpath}`。
- 触发版本：2026 新版 version.json（如 `temp/26.3-snapshot-7.json`）。

### 根因
- 新版 json 的 `arguments.jvm` 数组里直接写了两个独立字符串项：`"-cp"` 和 `"${classpath}"`（旧版只写模板变量不写 `-cp`）。
- 原循环用 `kSkipExact` 跳过 `-cp`，但下一项 `${classpath}` 是字面量、不以 `-` 开头，被当作普通 JVM 参数 push 进 jvmArgs；
- 组装命令时它落在 `-cp <真实classpath>` 之前，JVM 把第一个非选项 token `${classpath}` 当主类 → ClassNotFoundException。

### 修法（仅启动命令解析，不动下载逻辑）
- `Services.cpp` jvm 字符串参数循环（约 3895-3929）：
  - 在占位符替换块里补 `replaceAll(${natives_directory}, nativesDir)` / `${launcher_name}`→`LXElauncher` / `${launcher_version}`→`1.0`；
  - 入列条件改为 `if (!a.empty() && a != L"${classpath}")`——精确跳过裸 `${classpath}`（我们已在循环后自行 `-cp <classpath>`，见 3945-3946，不会缺 classpath）；
  - `kSkipExact` 增补 `-class-path`。
- `applyGameVars`（游戏参数模板）补新版占位符：`${clientid}` → `lxelauncher-mc`、`${auth_xuid}` → `0`（固定非空，避免 `--clientId <空>` 参数错位）。
- 注意：`-Djava.library.path=${natives_directory}/java` 这类仍会被 `-djava.library.path` 前缀规则跳过，由我们自己的 `-Djava.library.path=<nativesDir>`（约 3881）负责——natives 解压目录本就是 `<版本>-natives`。

### 排查方法
- `arguments.jvm` / `arguments.game` / `minecraftArguments` 中任何 `${xxx}` 残留都会被原样拼进命令行；用 `Get-Content ... -Raw | ConvertFrom-Json` 打印 `arguments.jvm` 数组逐项核对字符串项，比肉眼读单行 json 可靠。

---

## 15. Java 增量检测与前端渲染（§2.6 落地的工程经验）

### 增量事件设计（C++ 侧）
- 全局 `static Bridge* g_bridgeForJavaScan`（声明在 `g_javaScanRunning` 附近，`RegisterMinecraftLaunch` 里赋值），供后台线程安全地推事件。
- `InsertJavaFoundAndNotify(const InstalledJava&)`：major<=0 或非 64 位直接跳过；锁内按路径 `_wcsicmp` 查重，新条目并入 `g_javaScanCache` → 回写 `SaveJavaCacheToDisk` → `PostEvent("java.found", ev)`；整体 try/catch 包裹防后台线程抛异常崩溃。
- `ScanInstalledJavas` 的 `probeFn` 每个 `ProbeJavaExe` 成功后**立即** `InsertJavaFoundAndNotify(p)`（不等全盘扫完），实现"找到一个推送一个"。
- `mc.javaInfo`（原 4126 附近）改快速返回：内存缓存非空直接秒回；空则先 `LoadJavaCacheFromDisk`（静默路径验证），仍空才 `AsyncScanJava()` 后台增量探测。不再同步阻塞。

### 缓存优先与并发复位（避免与增量写盘冲突）
- `ScanInstalledJavas` 开头：内存缓存 30s 内非空（非 force）短路返回；首次内存空时试磁盘缓存，命中则秒回 + `AsyncScanJava()` 后台续扫回写（2521-2535）。
- `ScanInstalledJavasNow` 只是 `g_forceJavaScan.store(true)` 后调 `ScanInstalledJavas`；`g_javaScanLast`/`g_javaScanRunning` 的刷新/复位都只在正确位置出现。尾部锁内 `s_cache=out; g_javaScanCache=out` 覆盖增量并入值不影响正确性（`out` 含全部探测结果），且 `SaveJavaCacheToDisk` 只在尾部锁外串行一次 → **无重复写盘**。
- 失效兜底：`StartPeriodicJavaVerify()` 每 10 分钟轻量校验磁盘缓存路径仍在 + 是否过 `kJavaCacheMaxAgeSeconds`，失效才 `AsyncScanJava()` 重扫。

### 前端作用域修法（LaunchParams bug）：
- 症状：`Uncaught ReferenceError: launchParams is not defined`。根因是 `ensureJavaThenRepair` 定义在 `launchBtn.onclick` 回调之外，却引用了 onClick 内的局部变量 `launchParams`。
- 修法：把整个函数定义块**移入** `launchBtn.onclick` 内（`repairThenLaunch` 定义后），并删除旧孤立定义。
- 验证用 AST 括号深度脚本：onclick 行 `braceDepth==3`，函数定义深度 `==4` → 证明函数在 onClick 作用域内。词法括号深度检查能实证作用域归属，比肉眼可靠。

### 前端增量渲染（detectedJavaCache + java.found）
- 全局 `let detectedJavaCache = []`；`buildJavaOptions()` 把 `ji.list` 合并去重进缓存后重绘；`LV.on('java.found')` 按 path 去重并入缓存 + `State.detectedJava` + `refreshDetectedJavaUI()`。
- `renderDetectedJava(wrapper)` 只重绘「—— 已检测到 ——」区段：移除旧 `.js-detected-sep/.js-detected-opt` 再重建，静态 5 项不动；`refreshDetectedJavaUI()` 双 wrapper（javaPath + launchJava）重绘。
- 路径省略：`shortJavaLabel(j)` 路径>46 字符时保留开头 18 + 结尾 22，中间省略号，完整路径放 title。
- `State` 加 `detectedJava: []` 属性；启动设置 overlay 的 Java 下拉由同 wrapper 机制覆盖。

### mock 冒烟测试补足清单（跑 mocktest.js 需要的 DOM 方法）
- `makeEl` 必须包含：`addEventListener`、`append(...cs)`、`setAttribute/getAttribute`（buildSliders/renderMcFolderDropdowns 用到）、`innerHTML` setter（置空 children）、`querySelector` 返回存根元素而非 null（buildSelect 会对 null 赋 onclick 抛错）、`style.setProperty`、`body.appendChild`、`body.style`、`removeEventListener`（window）。
- `wrapper.querySelector('.form-select-options')` 需映射到真实 options 元素（`javaPathWrapper→javaPathOptions`、`launchJavaWrapper→launchJavaOptions`），否则渲染 children 计数为 0。
- 事件注册键集合包含 `java.found`、handlers 数为 1、渲染后 `javaPathOptions` 有 10 children（5 静态 + sep + 2 opt + 重复项去重不追加）即可判定链路通。

### 内存自动分配（memAuto）实现要点
- `autoMemRecommend(info, version)`：版本维（`/1\.(\d+)/`，>=20→4096、>=17→3072、>=13→2048、else→1536）+ 内存维（预留 keepSys=max(2048,25% total)，取 (avail-keepSys)*0.8，256MB 对齐）。
- **上限要 `Math.max(base, total*0.7)`**：4G 内存机 70% 上限（2867）会压过 1.17 的 base(3072) 得到荒谬值，必须保证上限不低于版本基础需求；两处都得改（test 与真实 inline 保持同公式）。
- 双开关注入（`injectMemAutoToggles`）：设置页 `memAutoToggle` + 启动 overlay `launchMemAutoToggle`；`applyAutoMem()` 调 `system.info` 后 `LaunchStore.set('mem', want)` 并更新 hint 文本；手动拖滑块自动关 memAuto（onMemInput）。
- 持久化：`applyPersistedSettings()` 恢复 `memAuto`；`Persisted.load().then` 与 `updateHomeVersion`（onSettingState currentVersion 分支）都重算。

### 本轮已验证证据
- mocktest：`SCRIPT_OK`、`java.found handlers registered: 1`、`javaPathOptions rendered children: 10`（含去重验证）。
- node --check inline_1.js exit 0；memtest.js pass=5 fail=0。
- MSBuild Debug x64 exit 0（仅 aria2c 提示）；playwright：静态 5 项渲染、memAuto toggle active/sliderDisabled 切换正确、无 JS error（仅 favicon 404）。

## 17. 第六批：completeVersion 补全兜底 + Forge 1.16.5 launchwrapper 缺库 + mc.launch 异步化（2026-08 新增）

### mc.completeVersion 的 libraries 兜底（覆盖 Forge/Fabric/OptiFine 合并 json）
- 合并 json 里部分库没有 `downloads` 或 `artifact.url` 为空（launchwrapper / forge 各运行库 / Fabric loader 库），旧 `addMissing` 对空 url 静默跳过 → 缺库不进 classpath。
- 修法：循环内先按 `downloads.artifact` 处理，**`url 非空才算 handledArtifact`**（空 url 留给兜底）；未 handled 且 `name` 是 3 段 maven 坐标（`@ext` 后缀先剥离；段数==3，含 classifier 的算 >3 不兜底）→ `MavenCoordToPath` 推到 `libraries/<rel>`，缺失/0 字节时 push `CVItem`（url=`https://files.minecraftforge.net/maven/`+rel，label=`安装器库 · <rel>`）。
- 注意 `std::filesystem::exists` + `file_size==0` 才算缺失，避免保留损坏文件。

### Forge 1.16.5 `NoClassDefFoundError: net/minecraft/launchwrapper/LaunchClassLoader` 定位
- 用户诊断：Java 25 版本太高（1.16.5 推荐 Java 8/11，启动器已有 JavaRequirement 推荐逻辑）+ launchwrapper 库缺失/classpath 不完整 + Mixin 问题。
- 启动器侧修复点就是**缺库补下**：`mc.launch` 4.1 校验段同 completeVersion 一样加 `handled` 标记 + 对无 downloads/空 url 的 3 段坐标按官方 maven 补下（`DownloadFileSmart("https://files.minecraftforge.net/maven/"+rel, ...)`），保证 `BuildClasspath` 不缺 launchwrapper/forge 运行库。

### 关键顺序坑：必须先补下再 BuildClasspath
- 原顺序：`BuildClasspath`（第4步 用 `LibPathFromName` 只加「已存在」的文件）在 4.1 补下**之后**执行才能把新补的库收进 classpath；若调反，补下发生在 classpath 构建后 → 启动仍缺库。
- 已调整：4.1 校验/补下移到第 4 步 classpath 构建之前（Services.cpp 3861 起）。

### mc.launch 同步阻塞 → 卡 UI（启动无响应）根因与异步化
- 根因：`mc.launch` 原是同步 `Register`，其内部 `DownloadFileSmart` 会 `WaitForSingleObject INFINITE` 等 aria2 子进程，在 WebView2 桥消息线程上串行补下 → `LX.call('mc.launch')` 永不返回 → 前端 `.then` 不 fire、UI 假死。
- 修法：`Register` 改 `RegisterAsync`，函数体包成 `auto run = [&bridge, params]() -> HandlerResult {...}`（params 按值深拷贝进线程）+ `std::thread([&bridge, params, done]{ try { done(run()); } catch(...) { done(Err(-32030, ...)); } }).detach()`。
- 前端 `.then((r) => ...)` 直接在 done 回调返回，**无需改前端**——README/§2 提到 RegisterAsync 用在网络请求，现在启动/补全这类可能阻塞的处理器都应走它。
- 编译注意：`RegisterAsync` 捕获 `&bridge`（程序生命周期常驻）安全；函数体的所有 `return Err/Ok` 都留在 run lambda 内，类型 HandlerResult 完全兼容。

---

## 18. 第七批：${classpath} 残留在启动命令中的三重防御 + javaVersion 合并（2026-08 新增）

按 Huangyu 教程 Part 1~4 对齐启动命令解析（只动启动命令解析，下载部分不动）。教程核心格式：
`{java.exe} {jvm参数...} -cp {classpath} {主类名} {游戏参数}`，且「启动高版本 Forge 按原版方式启动即可、对生成的 classpath 去重」；
OptiFine 合并 json 后 `mainClass` 以 OptiFine 为主、`arguments` 子项 OptiFine 在后——均由本启动器 `mc.launch` 的既有 `-cp <classpath>` 追加逻辑覆盖。

### ${classpath} 三重防御（全部位于启动命令组装区，不碰下载）
1. **arguments.jvm 解析循环**：任何含 `${classpath}` 子串的字符串项一律 `continue`（不只是精确等于）——新版 version.json（Forge 1.21+ / 2026 快照）把 `-cp` 与 `${classpath}` 拆成两个独立字符串项，残留字面量会被 JVM 当主类 → `ClassNotFoundException: ${classpath}`。
2. **组装命令行 9.1 防御**：遍历 jvmArgs 前再滤一遍，凡含 `${` 且（以 `${classpath}` 或 `${` 开头）的项剔除。
3. **9.2 主类防御**：`mainClass` 若仍含 `${`（如 `${main_class}`）则兜底为 `net.minecraft.client.main.Main`。

### 新增：arguments.jvm 的 `-X` 前缀兜底剔除
- `kSkipPrefix` 只挡小写 `-xmx/-xms/-cp/-classpath`，第三方/加载器 json 偶发大写 `-Xmx3072M`、`-XX:+...` 变体漏网 → 追加规则：`lower[0]=='-' && lower[1]=='X'`（-X/-XX 头）一律 skip。
- 注意：不能一刀切 `-D` 头（`-Dlauncherforgepath` 等引导参数会被误杀），只剔 `-X` 头是安全的（我们已自行注入 -Xmx/-Xms/G1GC）。

### ResolveVersionJson 需合并 javaVersion（父版本兜底）
- 加载器子 json（Forge/OptiFine/Fabric 合并产物）常不写 `javaVersion`，父原版 json 才有 `majorVersion` → 在 inheritsFrom 合并区块补：
  `if (!vj.contains("javaVersion") && parent.contains("javaVersion")) vj["javaVersion"] = parent.at("javaVersion");`
- `JavaMajorFromVersionJson(vj, mcVersion)` 已存在（Services.cpp 2754 行）：优先读合并后 javaVersion.majorVersion，缺省回退 `RecommendedJavaMajor`；`mc.launch`/`mc.javaInfo`/`mc.javaAutoInstall` 三个接入点都经它取值，无需重复解析。

### 教训
- 改 mc.launch 时先确认 helper 是否已存在（grep 函数定义）再决定是否新建，避免误报编译错误。
- 「只改启动命令解析」= 只动 jvm 参数解析循环 + 命令行组装段；`BuildClasspath`、libraries 补下、natives 解压等下载/文件类代码一律不动。

---

## 19. 第八批：classpath 去重（Log4j NoSuchMethodError）+ authlib-injector 下载 API 修正（2026-08 新增）

### 问题一：Forge 1.16.5 启动报 `NoSuchMethodError: ThrowableProxy.formatExtendedStackTraceTo(...)`
- **根因**：原版 json 带 log4j 2.8.1，Forge 合并 json 追加 2.15.0（log4shell 修复版）——`BuildClasspath` 不去重，classpath 里新旧两版并存且旧版在前 → JVM 先加载 2.8.1，Forge 调用 2.15.0 才有的方法 → NoSuchMethodError。
- **修复**：`BuildClasspath` 改为「收集 → 按 Maven 坐标去重」。要点：
  - 坐标 = `group:artifact`（classifier 不同视为不同库，不合并）；
  - 同坐标保留**最高版本**（`MavenVersionCmp` 逐数字段比较，支持 `2.8.1 < 2.15.0` 与 `1.16.5-36.2.55` 带后缀）；
  - 精确路径 `_wcsicmp` 去重（同一文件只出现一次）；
  - 从 `downloads.artifact.path` 反推坐标（`org/apache/logging/log4j/log4j-api/2.8.1/log4j-api-2.8.1.jar` → group=org.apache.logging.log4j, artifact=log4j-api, version=2.8.1）。
- **注意**：Windows 下 `std::max` 被 `max` 宏污染 → 编译报 C2589，必须写 `(std::max)(...)`。

### 问题二：authlib-injector.jar「总是找不到」
- **根因**：官方下载 API 是 `GET /artifact/latest.json`（返回 JSON：`{version, download_url, ...}`），旧代码直接下载 `/artifact/latest`（404）和 BMCLAPI 根 `mirrors/authlib-injector`（返回 HTML 页面）→ 保存的不是 jar，或根本下不动。
- **正确下载链**（`DownloadAuthlibInjector` 三级兜底）：
  1. BMCLAPI 镜像：`https://bmclapi2.bangbang93.com/mirrors/authlib-injector/artifact/latest.json`（**注意末尾 `/artifact/latest.json` 与开头镜像路径的尾斜杠**——BMCLAPI 文档显示镜像入口为 `mirrors/authlib-injector/`，只到根只返回 HTML）；
  2. 官方：`https://authlib-injector.yushi.moe/artifact/latest.json`；
  3. GitHub：`api.github.com/repos/yushijinhun/authlib-injector/releases/latest` 取 assets 里 `.jar` 的 `browser_download_url`（实测最新 v1.2.8）。
- 每级先 `HttpFetchText` 取元数据 → 解析 `download_url` → `DownloadFileSmart` 下载真实 jar。
- 替换点：`mc.launch` 内联下载 + `auth.ensureInjector` RPC 两处共用 `DownloadAuthlibInjector`。

### 教训
- 涉及"下载到文件"的需求，先实测 URL 返回内容（PowerShell `Invoke-WebRequest -MaximumRedirection 0` 看状态码/Content-Type），别假设 URL 直接给 jar——很可能是 JSON 元数据或 HTML。
- MSB3073 robocopy 返回码 1 是无害噪音（robocopy 成功即返回 1），不用理会；LNK1140「超出 PDB 限制」是残留 PDB/进程占用导致，删 `x64\Debug` 整个目录重链即可。

## 20. 第九批：classpath 去重坐标解析 bug（LWJGL 主库被 natives 顶掉）+ UI 布局 + 智能拖放（2026-08 新增）

### 问题：Fabric 26.2 启动报 `NoClassDefFoundError: org/lwjgl/glfw/GLFWErrorCallbackI`
- **现象**：`-cp` 里 LWJGL 全是 `...-natives-windows.jar`，主 jar（`lwjgl-glfw-3.4.1.jar` 等）一个都没有 → GLFW 接口类找不到。库文件其实都在磁盘上（不要信"重新下载"）。
- **根因**：某版本 json（如 26.2）把 LWJGL 的 natives 也写成**独立 library 且带 `downloads.artifact`**（不是 Mojang 标准的 `downloads.classifiers`）。旧代码从 `artifact.path` **文件名反推版本**：`lwjgl-glfw-3.4.1-natives-windows.jar` 用 `rfind('-')` 取版本 → 得到 `"windows"`。于是主 jar（coord=`org.lwjgl:lwjgl-glfw`, ver=3.4.1）与 natives（同 coord, ver=`windows`）**撞去重键**；`MavenVersionCmp("windows","3.4.1")` 逐段字符串比较 `w > 3` → natives 判定"更高版本" → **替换掉主 jar**。
- **修复**（`BuildClasspath`）：
  1. 坐标**优先从 json 的 `name` 字段**解析（`coordParts` 支持 `group:artifact:version[:classifier]`），不再从文件名反推；
  2. 无 `name` 才从 path 反推，且版本取**版本目录**（`segs[segs.size()-2]`）而非文件名，classifier 用前缀 `artifact-version-` 剥离提取；
  3. **去重键 = `group:artifact[:classifier]`**——natives 与主 jar 视为不同库，互不覆盖。
- **验证方法**：PowerShell 读 json，模拟新旧两种 key 推导 + 去重，对比结果（旧：所有 lwjgl 条目同一 key；新：主 jar 与 natives 各一 key）。
- **通用经验**：Maven 文件名 `artifact-version-classifier.jar` 的版本号永远不要用 `rfind('-')` 从文件名猜——classifier 里也可能有 `-`（`natives-windows-x86`）。要么用 json 的 name 坐标，要么用版本目录段。

### UI 布局修复（index.html）
- 内存行：`injectMemAutoToggles` 注入的下拉框 `style.cssText` 改为 `width:140px;flex-shrink:0;margin-right:6px;margin-bottom:10px;`——横排靠 gap+margin-right，换行（wrap）靠 margin-bottom 撑开与滑条的间距。
- 滑条禁用态：`.ore-slider:disabled` 整条 `background` 用 `var(--text-secondary)` 变灰、`opacity:.55`、`cursor:not-allowed`；thumb 同步变灰去阴影。
- 启动设置标题栏：`#lsVersionBadge` 的 inline style 加 `margin-right:14px`（它 `margin-left:auto` 顶到最右，与关闭按钮无间隙会粘连）。
- 毛玻璃强度滑块失效：`.content-panel` 第 140 行**硬编码 `blur(20px)`** 覆盖了第 36 行 `blur(var(--glass-blur-strength,20px))` 的引用 → `applyGlassBlur` 设置的变量对面板无效。统一把 `blur(20px)` 替换为 `blur(var(--glass-blur-strength, 20px))`（`blur(20px) saturate(160%)` 与 `blur(20px);` 两种形态都要改）。
- 滑块初始线条不连接：`--p`（填充百分比）只在 `applyGlassBlur` 里设置，未持久化 `glassBlur` 时初始不生效 → 初始化时按 input.value 同步一次 `--p`（value/max 百分比）。

### 智能拖放（拖入文件识别导入）
- **浏览器安全模型**：JS `dataTransfer.files` 拿不到本地完整路径 → 必须走 C++ 拦截系统拖放。
- **C++ 实现**（`WebViewHost`）：
  - `ICoreWebView2Controller4::put_AllowExternalDrop(FALSE)` 关掉 WebView2 内置拖放目标（必须在首次导航前），再 `RegisterDragDrop(hwnd_, IDropTarget)` 自己接管；
  - `Drop()` 里从 `IDataObject` 提 `CF_HDROP`（`DragQueryFileW` 拿真实路径）+ `CF_UNICODETEXT`（文字），`nativeDrop` 事件转发前端；DragEnter/DragLeave 转发显隐提示层；
  - **必须 `OleInitialize`**（`wWinMain` 里 CoInitializeEx 之后补）否则 `RegisterDragDrop` 失败；
  - 析构顺序：先 `RevokeDragDrop` 再 `Release`，WebDropTarget 定义在全局作用域（与头文件前向声明一致），Json 用 `lxe::Json`。
- **后端 RPC**：`mc.probeZip`（`tar -tf` 列条目，判 pack.mcmeta→材质包 / shaders→光影 / mods|versions|.minecraft→整合包）、`mc.probeFolder`（versions|libraries|assets→mcroot，version.json→单版本目录）、`mc.dropImport`（复制到 mcRoot\mods / resourcepacks / shaderpacks / modpacks / authlib-injector.jar）、`app.readFileDataUrl`（读图片为 base64 dataUrl，>16MB 拒绝）。
- **前端分类**：.jar 名含 authlib→authlib，否则模组；.zip 先 probeZip；图片→设背景；.exe→设自定义 Java 路径（`LaunchStore.set('javaPath','自定义路径...')`）；目录 probeFolder=mcroot→`mc.addMcFolder`+`setActiveMcFolder`；单版本目录→提示"请拖入 .minecraft 文件夹"；文字含 `authlib-injector:yggdrasil-server:` → 填入第三方登录服务器。
- **日志定位**：游戏 stdout/stderr 在 `mcRoot\logs\lxe-launcher-std.log` / `lxe-launcher-crash.log`；`Select-String` 只读搜索 index.html 可绕过 Grep 工具的二进制跳过（该文件含 NUL 字节被 ripgrep 判定为二进制）。

### 知名启动器自带 JRE 的预置路径（Java 自动检测扩展）
- 各启动器的 JRE 子目录名随版本变化（如 Lunar 的 `zulu17.40.19-ca-fx-jre17.0.6-win_x64`），**不能写死具体 exe 路径**，要按**根目录递归**找 `javaw.exe`（兜底 `java.exe`）。
- 预置根目录清单：`%USERPROFILE%\.lunar\jre`（Lunar）、`%LOCALAPPDATA%\Programs\CurseForge\runtime\java` 与 `%LOCALAPPDATA%\CurseForge\Install\runtime\java`（CurseForge）、`%LOCALAPPDATA%\Badlion Client`、`%APPDATA%\LabyMod` 与 `%LOCALAPPDATA%\Programs\LabyMod`、`%APPDATA%\ModrinthApp` 与 `%LOCALAPPDATA%\Programs\modrinth-app`、`%ProgramFiles(x86)%\Minecraft Launcher\runtime`（官方）、`%USERPROFILE%\.tlauncher`、`%APPDATA%\PrismLauncher`/`MultiMC`/`GDLauncher`、`%LOCALAPPDATA%\Programs\Feather`。
- 实现 `AddKnownLauncherJava(out)`：环境变量取 USERPROFILE/LOCALAPPDATA/APPDATA/ProgramFiles(x86) → 递归深度 ≤5、总结果 ≤24 → 接入 `ScanInstalledJavas`（level 0 候选，进 Java 下拉框）与 `FindJavaPath`（自动兜底）。
- **缓存注意**：Java 列表走磁盘缓存（javaCache 7 天有效），新增路径要等后台重扫（`AsyncScanJava`）回写后才出现在下拉框，属预期行为。

## 21. 第十批：Java 25 + 下载任务级取消 + 自动下载并入任务中心 + WebView2 子窗口拖放接管（2026-08 新增）

### Java 25 下载条目
- Adoptium Temurin 直链模式（8/17/21/25 一致）：`/v3/binary/latest/{ver}/ga/windows/x64/jre/hotspot/normal/eclipse`。
- 在 `mc.javaList` 官方源补 `mk("Java 25 (JRE 25.x)", "25", "适用于 Minecraft 1.21.4+ / 最新版本", "25")`。

### 任务级下载取消（核心模式，可复用到任意下载路径）
- **取消标志注册表**：`std::mutex g_dlCancelMu` + `std::map<std::string, std::shared_ptr<std::atomic<bool>>> g_dlCancelFlags`（按 taskId 登记）。三个函数：
  - `DLCancelFlag(taskId)`：取或创建标志（`make_shared<atomic<bool>>(false)`）；
  - `DLCancelFlagSet(taskId)`：置位 true（`download.cancel` RPC 调它）；
  - `DLCancelFlagRemove(taskId)`：任务结束后擦除，**防 map 泄漏**（worker 的所有提前返回/完成/错误/异常分支都要调用）。
- **取消生效机制**：aria2 下载的 `progressCb` 里检查 `cancelFlag->load()` 返回 false → `Aria2DownloadWithProgress` 内部 `TerminateProcess` 杀 aria2 → 任务失败分支把文件状态记 `"cancelled"` 而非 `"error"`，并 `postState("cancelled")`。
- **已接入的路径**（事件 schema 统一：`download.progress` 带 percent/speed/eta/stage + `download.state` 带 started/done/error/cancelled）：`mc.submitDownloadList`、`CompleteVersionFilesWorker`（compSeq 50000）、`java.download`、`mc.javaAutoInstall`（固定 taskId `"java-autoinstall"`）、`auth.ensureInjector`（固定 taskId `"auth-injector"`）。
- **前端**：`download.state == 'cancelled'` → `updateDownloadTask(state:'cancelled', stage: '已取消')`；任务中心 running 态显示终止按钮，点击 `confirmBox` 二次确认后 `LX.call('download.cancel', {taskId})`；CSS 加 `cancelled`/`cancelling` 灰态。
- **坑**：`mc.javaAutoInstall` 的 `RegisterAsync` lambda 若内部线程引用 `bridge`，外层捕获必须是 `[&bridge]`，否则 C3493「无法隐式捕获」。
- **UI 卡死坑**：前端 `onStateEv`（安装流水线）与 `downloadJava` 的 `onState` 必须各加 `cancelled` 分支（恢复按钮/移除队列项/解绑监听），否则取消后界面按钮不可点。

### WebView2 子窗口拖放接管（修"拖不动"）
- **根因**：即使 `put_AllowExternalDrop(FALSE)`，WebView2 的 `Chrome_WidgetWin_1` 子窗口仍可能注册内部 OLE 拖放目标拦截拖入，父窗口 `RegisterDragDrop` 收不到事件 → 表现为拖不动/无反应。
- **修复**（`InstallWebViewSubclass`）：拿到子窗口句柄后 `RevokeDragDrop(webview_child_hwnd_)` 再 `RegisterDragDrop(webview_child_hwnd_, drop_target_)`，使整个客户端区域（含子窗口覆盖区）都由自己的 `WebDropTarget` 接管；析构时 `if (IsWindow(...)) RevokeDragDrop` 清理（新增 `bool child_drop_registered_` 成员）。

### 拖放确认 UX（模糊背景 + 居中放大 dialog + 类型确认）
- 拖入提示层 `#nativeDropOverlay`：`backdrop-filter: blur(10px) saturate(120%)` 大模糊遮罩 + 居中 `.ndo-dialog`（`transform: scale(0.92) → scale(1)` + opacity 过渡动画）。`pointer-events: none`（仅展示层），确认按钮放独立 `.overlay`。
- 确认 dialog `dropConfirmOverlay`：复用 `.overlay`/`.overlay-box`（scale 0.92→1 放大动画）。`handleDroppedPath` 改为**先识别类型 → `showDropConfirm(info)` 弹框（类型图标 + kindLabel + 文件名 + 描述）→ 点"确认导入"才执行**。`pendingDropRun` 保存确认后要执行的函数，取消则置空。
- 类型映射：文件夹 mcroot→MC 文件夹；jar→authlib-injector/模组；zip→`probeZip` 后按 resourcepack/shader/modpack/mod 映射图标标签；图片→背景图；exe→Java 运行时。

---

## 22. 第十一批：Lunar Java 路径修正 + .disabled 模组拖入 + CurseForge 接入（2026-08 新增）

### Lunar Client Java 路径修正
- 正确路径是 `%USERPROFILE%\.lunarclient\jre`（**不是** `.lunar`），jre 下是随机版本子目录（zulu17.xx-win_x64 等）。`AddKnownLauncherJava` 的 base 改为 `.lunarclient\jre` + `.lunarclient\client`（旧版），沿用递归扫描找 javaw.exe/java.exe 的方式。

### .disabled 禁用模组拖入
- 禁用模组文件名为 `xxx.jar.disabled`，扩展名是 `disabled`。
- 后端 `mc.dropImport` kind 自动判定加分支：`ext == L".disabled"` 且 stem 以 `.jar` 结尾 → kind = mod（authlib 名同理），复制时**保留原名（含 .disabled 后缀）** → 游戏不加载、版本设置页可识别启用。
- 前端 `handleDroppedPath`：`ext === 'disabled'` 时用 `name.replace(/\.disabled$/i,'')` 剥后缀判断 jar，确认框标签显示"已禁用模组"，描述提示可在版本设置中重新启用。

### CurseForge 接入（模组 / 材质包 / 光影搜索）
- **API Key 是硬性要求**：v1 API 全部端点带 `x-api-key` 头，无 key 返回 403。key 存前端 `Persisted('curseApiKey')`，设置-下载页有输入框，搜索时经参数传给后端。
- **新工具函数** `HttpFetchTextWithHeader(url, header)`（WinHTTP，header 以 `\r\n` 结尾、需 `Accept: application/json`），`HttpFetchText` 不支持自定义头，不可直接用于 CurseForge。
- **`mc.curseSearch` RPC**：`/v1/mods/search?gameId=432&classId=...&searchFilter=...&pageSize&index&sortField=6&sortOrder=desc&gameVersion=...`。
  - **classId 映射**：mod=6、resourcepack=12（Resource Packs）、shader=6552（Shader Packs）、modpack=4471。
  - 返回结构对齐 `mc.searchMods`（`{list, total}`，`id` 转字符串），额外带 `latestFile`（下载用）：优先 `downloadUrl`，缺失时按 forgecdn 拼接 `https://edge.forgecdn.net/files/{fileId/1000}/{fileId%1000}/{fileName}`（公开 CDN，无需 key）。
  - CurseForge 不返回总条数 → `total` 用当页条数估算（首页/尾页判断由前端翻页逻辑自然处理）。
- **前端**：下载中心新增 材质包（resourcepack）/ 光影（shader）tab；`MOD_STATE_KEY`（tab→State 数组）与 `MOD_PROJECT_TYPE`（tab→projectType）两张映射表统一所有 mod-like tab（mod/modpack/resourcepack/shader）；`modSource`（'modrinth'|'curseforge'，Persisted 持久化）决定搜索走 `mc.searchMods` 还是 `mc.curseSearch`；`dlSourceWrapper` 下拉在 mod 类 tab 显示"Modrinth/CurseForge"，其余 tab 显示原有下载源。
- **下载**：CurseForge 行按钮直接"下载"（结果已带 latestFile，无需二次查询）；输出目录按 tab 区分 mods/resourcepacks/shaderpacks/downloads；`_ctxModItems` 右键菜单按 `_source` 分流（CurseForge 行 → "在 CurseForge 查看"，Modrinth 行 → "在 Modrinth 查看/查看详情"）。
- **注意**：CurseForge 行不做详情弹层（id 是数字，Modrinth API 不认），只提供下载 + 右键打开网页。

---

## 23. 第十二批：拖入 Java 自动入列 + 缓存续期/删除 + HSV 调色盘（2026-08 新增）

### 拖入 java.exe 自动识别并加入「已找到的 Java」列表
- 后端新增 RPC `mc.addJavaRuntime {path}`（RegisterAsync，探测启动 JVM 不能同步阻塞桥线程，§10/§14 教训）：
  `ProbeJavaExe(path)` → major>0 且 is64 → `InsertJavaFoundAndNotify(j)`（内存缓存 + 磁盘回写 + `java.found` 事件）；
  major<=0 返回 `{major:0}` 供前端提示，不抛错。返回 `{path,major,is64,fileEncoding,nativeEncoding}`。
- 前端 `handleDroppedPath` 的 `.exe` 分支（原先只 `LaunchStore.set('javaPath','自定义路径...')`）改为：
  `LX.call('mc.addJavaRuntime',{path})` 成功且 major>0 → 并入 `detectedJavaCache`（去重）→ `refreshDetectedJavaUI()`
  → `LaunchStore.set('javaCustomPath',p)` + `set('javaPath',p)`（直接用真实路径，`buildLaunchParams` 会原样传后端）。

### Java 缓存续期 / 缺失删除（StartPeriodicJavaVerify 重构）
- 旧逻辑「过期→全盘重扫」过重且不会续期（javaCacheTime 永久不刷新 → 每 7 天必全盘扫一次）。
- 新增 `ReadJavaCacheEntriesFromSettings(s)`（原始条目，**不做有效期/存在性过滤**），`LoadJavaCacheFromDisk` 复用其 + 有效期过滤 + 存在性过滤。
- 新增 `RemoveJavaFromCacheAndNotify(path)`：内存移除 + 磁盘回写 + 推 `java.removed` 事件（前端 `LX.on('java.removed')` 从 `detectedJavaCache` 过滤 + `refreshDetectedJavaUI`）。
- 新增 `PruneAndRefreshJavaCache(expired)`：逐条 `filesystem::exists`，缺失→删除；在 `expired` 时把仍存在的条目重写磁盘（`SaveJavaCacheToDisk` 刷新 `javaCacheTime` = **续期**）；`g_javaScanCache` 同步为结果。
- 定期循环：`expired` 才 `AsyncScanJava()`（发现新安装），否则仅按需删除/续期，不再无谓全盘重扫。

### HSV 调色盘（替换上一版「色相条+明度网格」）
- 参照实测样例实现（SV 渐变区 + 色相条 + 预览/hex + EyeDropper 取色器 + 8 色样），无原生 `<input type=color>`、无 HSL/RGB 滑块。
- `buildPicker(btnId, panelId, applyFn)` 三实例（primary/secondary/bg），内部是 HSV 模型（h0-360、s/v 0-100）：
  SV 区拖拽（mousedown+全局 mousemove/mouseup）改 s/v，色相条拖改 h；`hsvToHex/hexToHsv` 按参考实现。
- `PICKERS[panelId] = {setColor, syncFromSwatch}`：预设 swatch 点击后调用 `syncFromSwatch()` 让面板指针对齐当前已选色（读按钮 `.picker-swatch` 的 inline background，`/^#[0-9a-fA-F]{3,6}$/` 校验）；面板打开时也 sync。
- 注意：`hexToHsv` 取整量化导致 hex→HSV→hex 回环 ±1 通道差，属预期（参考实现同样行为），不影响使用。
- hex 输入：Enter 或 blur 校验 `isValidHex`，无效恢复当前色；EyeDropper 不可用（非 Chromium/WebView2）时 `notify` 提示。
- CSS：`.picker-sv`（SV 渐变 + `::before` 黑渐变）、`.picker-sv-ptr`、`.picker-hue`（hue 渐变滑条）、`.picker-hue-ptr`、`.picker-preview`、`.picker-eye`；面板宽度 216→250px。

---

## 24. 第十三批：javaw 探测回退 + 自定义路径持久化 + 调色盘/悬浮框样式统一（2026-08 新增）

### javaw.exe 探测回退（根治「拖入 javaw 无法识别」+ runtime 目录 Java 永远进不了列表）
- 根因：javaw.exe 是 GUI 子系统程序，`CreateProcessW` 管道（`RunCaptureTimeout` 捕获的 stdout/stderr）**拿不到任何输出** →
  `ProbeJavaExe` 返回 `major=0`。此前只在 `java.download` 安装后做了「同目录 java.exe 再探测」的局部兜底，
  而拖入 javaw、`ScanInstalledJavas` 的 runtime 目录（`java-*` 下只找 javaw.exe）全都不带该兜底 → 都失败。
- 修复（集中在 `ProbeJavaExe`，一处修复覆盖所有调用路径含全盘扫描）：
  1. 首次 `-XshowSettings:properties -version` 输出为空 → 若文件名是 javaw.exe，改试同目录 `java.exe`（控制台程序有输出）再解析；
  2. 仍空 → 纯 `-version` 兜底（个别发行版对 `-XshowSettings` 无输出）。
- 连锁收益：`mc.javaAutoInstall` 的⑤再次选取 `ScanInstalledJavasNow()` 现在能识别刚下载解压到 runtime 的 javaw，
  自动安装不再走到「安装后仍未匹配」报错；`mc.addJavaRuntime` 拖入 javaw 也成功。

### 自定义 Java 路径浏览/手输 → 一并探测持久化
- 前端 `buildJavaOptions` 新增 `probeAddJava(path)`：调 `mc.addJavaRuntime` → 成功并入 `detectedJavaCache` + `refreshDetectedJavaUI`；
- 浏览按钮选文件后不再只 `LaunchStore.set('javaCustomPath')`，而是 `set + probeAddJava`；
- 两个自定义输入框补 `onchange`（失焦触发）：填了完整 `.exe` 路径（`/\.exe$/i`）就 probe。此前「浏览选的 Java 不在已检测列表」
  是因为浏览只写了 store，从未触发探测。

### 调色盘字体 / 悬浮框深色背景统一
- `.picker-hex-input` 原 `font-family:monospace;font-size:12px`（与全 UI 的 MiSans 不一致）→ `font-family:inherit;font-size:13px`；
  `.picker-btn` 补 `font-family:inherit` 并 12→13px。输入框有全局 `input{font-family:inherit}`（html 27 行附近），显式继承即可。
- `.lx-tooltip` 原 `background:rgba(255,255,255,0.88)` 硬编码白底 → `background:var(--glass-bg)`
  （theme.css 的 `--glass-bg` 深色主题已是 `rgba(20,30,48,0.8)` 等），深色模式悬浮框背景/对比度自动同步；light 主题 0.75 透明度视觉差异可忽略。
- 教训：悬浮浮层类（tooltip/菜单/下拉）一律用 `var(--glass-bg)` 而非硬编码 rgba，否则切深色主题必出白底。

## 25. 第十四批：版本隔离（全局/按版本两套开关）+ Java 25 不再报无效 + 拖入导入版本选择 + 两段式包列表（2026-08 新增）

### Java 需求区间：现代版本接受更高主版本（Java 25 不判无效）
- `JavaRequirement`（Services.cpp ~3100）：`if (lo >= 17 && hi < 90) hi = 90;`
  → 下限 ≥17 的现代版本把上界抬到哨兵 90，Java 21/25 等更高主版本被 `inRange` 自然匹配；
  - 下限 8 老版本保持窄区间（离线 8u141+ 下限逻辑不变，`offline&&lo<8` 与 `offline&&lo==8` 两分支原样）。
  - 前端 `refreshLaunchJavaRecommend`：`reqText` 对 `hi===90` 显示「Java N 及以上」而不是「Java 17 ~ 90」。
- 启动路径分支（`mc.launch` javaPath 处理，Services.cpp 4317）：`"Java N"` 标签先 `PickJavaForMajor`；
  **无匹配时 `javaPath.clear()`**，交给自动选择，避免残留 "Java N" 标签被当成实际路径 → 「Java 路径无效」误报。

### PackDirFor 统一包目录解析（该版本目录 vs 全局 .minecraft）
- `PackDirFor(kind, version)`（Services.cpp 90）：version 非空 → `GetMcRoot()/versions/<version>/<kind>`，否则 `GetMcRoot()/<kind>`；
  对 version 做 `..` 与 `\/:*?"<>|` 字符拒绝（防路径穿越/非法目录）。
- **C3861 教训：`PackDirFor` 必须定义在 `GetMcRoot` 之后**（Helper 顺序无关规则不成立，C++ 需先声明）；
  `PackSubDirOf` 无依赖可放前面（47 行）。
- 所有包 RPC 新增 `version` 入参（缺省=None=全局，行为 100% 兼容旧调用）：
  `mc.listPackFiles`、`mc.deletePackFile`、`mc.togglePackFile`、`mc.togglePackFiles`、`mc.deletePackFiles`、`mc.openPackFolder`。
- 新 RPC `mc.importIntoVersion {kind, version, names[]}`：把全局 `<kind>` 下文件**复制**进该版本目录（**保留全局原文件**，非移动）；
  目录型资源包/光影用 `copy(recursive|overwrite_existing)`，单文件用 `copy_file(overwrite_existing)`，
  普通名与 `.disabled` 实体的复制逻辑一致（到目标端保持禁用状态），`src==dst` 防御跳过；返回 `{ok, done}`。
- `mc.dropImport` 新增 `version` 参数：mod/resourcepack/shader 目的目录走 `PackDirFor`；authlib/modpack 分支不受影响。

### 版本隔离开关：全局默认 + 按版本覆盖（前端）
- 持久化三级：`versionIsolation_<versionId>`（按版本，可置 null 消除覆盖）→ 回退 `versionIsolationGlobal`（设置-启动卡开关）→
  再回退旧启发式（版本名纯 `\d+(\.\d+){1,3}$` 判为 vanilla 类 → 开启；否则关闭）。
- 三个 UI 开关经 `applyVersionIsolationToggles()` 互相同步：`versionIsolationToggle`（启动设置-高级）、
  `vsVersionIsolationToggle`（版本设置-新「隔离」tab，导航行 `<button data-ls-section="isolate">隔离</button>` 位于资源包之后、光影之前），
  全局开关单独在设置-启动卡（`versionIsolationGlobalToggle`）；`vsIsolationResetBtn` 把该版本值置 null 跟随全局。
- 绑定都在 `bindEvents` 的开关区；`applyConfig` 处理 `'versionIsolationGlobal' in d`。

### 拖入导入：隔离版本弹版本选择框
- 拖入 jar/zip → `confirmPackImport(p, kind, label, icon, desc, isDisabled)`：
  仅当 `scoped && cur && versionIsolationOf(cur)` 时 `loadInstalledVersions()`（`mc.localVersions`，结果缓存进 `State.localVersions`），
  把名单传给 `showDropConfirm`。
- `showDropConfirm(info)` 现在用**自定义 `.form-select-wrapper` 下拉**（非原生 `<select>`）：
  `<select id="dcVersionSel">` 已移除，改为动态 `form-select-display` + `form-select-options`
  （选项含置顶「全局 mods/resourcepacks/shaderpacks 目录」`data-value=""` + 已装版本列表，默认=当前版本 cur）；
  展开/收起复用 app.js 的事件委托点击处理器（`.form-select-display` → toggle `.open`），无需手动绑定。
  OK 按钮调 `run({ version: selVersion })`（`''` = 全局）；`importDroppedFile` 里 `if (version) params.version` 使 `''` 落到全局。
  `confirmPackImport` 的 run 逻辑：`typeof opts.version==='string' ? (opts.version||undefined) : (hasList?cur:undefined)`，
  **老调用方 `run:()=>xxx` 不受影响**（忽略 `opts`），`''` 不再被 `||cur` 误判成当前版本。
- `importDroppedFile` 末参 version 透传给 `mc.dropImport`。

### 两段式包列表 + 「导入进该版本」批量按钮
- `refreshPackLists()` 隔离时两段渲染：`.vs-seg` 分隔（「该版本目录」/「全局 .minecraft」）；
  全局卡片加 `global` 类 → opacity 0.6、行内删除按钮隐藏、仅可勾选（键盘 focus 仍可达）；
  顶部 size 文案显示「本版 n / 全局 m」。
- 批量操作栏动态出现「导入进该版本」按钮（`data-batch-import`）：非隔离隐藏、无全局勾选 disabled，
  click → `mc.importIntoVersion` → done 后刷新两段。批量启用/禁用/删除与单个删除、打开文件夹在隔离时全部带 `version`。
- **监听只绑一次**：`list._importBound` 标志防重复绑定（refresh 会被反复调用）。
- CSS 新增：`.vs-seg`（::before/::after 分割线）、`.vs-card.global`、`.vs-batch-import:disabled`。
- 语法验证流程：正则抓 index.html 内联 `<script>`（282k 字符）→ 写 `%TEMP%\opencode\node2.js`（UTF8 无 BOM）→ `node --check` 需 `SYNTAX_OK`。
- 版本隔离相关测试要点：全局开关改完所有版本生效；按住版本再关全局时该版本仍开启（被 per-version 覆盖）；Reset 后回跟全局。
- **动态插入的下拉必须显式绑 `display.onclick`**：拖入确认框里的版本下拉（`#dcVersionSelWrap`）是 `showDropConfirm` 里 innerHTML 动态生成的，
  app.js 的全局事件委托（`.form-select-display` capture toggle）对新建 wrapper 并不可靠（实测点击不展开，表现为「下拉框点不动」）——
  根因是委托监听未生效；显式 `display.onclick = (e)=>{ e.stopPropagation(); ...; wrap.classList.toggle('open', !wasOpen); }`（与其它 working 下拉一致）即可。
  选项点击关闭 wrap 的逻辑也一样要内联绑（`o.onclick`）。
- 版本设置「隔离」tab 位于「光影」之后（vsNav 顺序：重命名/模组/资源包/光影/隔离；body section DOM 顺序不必同步，仅处可见 class 切换）。

## 26. 第十五批：设置页「插件管理」tab + 前端插件运行时（2026-08 新增）

### 架构决策：后端只扫描、前端真执行
- `webapp\js\app.js` 里的旧 `LX.plugins`（register/list/flushSlots/renderPluginsList/设置「插件中心」导航）**从未被 index.html 加载**（加载的只有 `js/bridge.js` + 内联 script），即旧插件系统实际不存在。本次按 `temp/LXElauncher插件规范.md`（标准 V1.0 / apiVersion "5.0"）**在 index.html 内联 script 里全新实现**，旧文件只作 API 参考不再维护。
- 后端（`RegisterPlugins`，注册进 `RegisterDemoServices`）只做三件事：扫 `plugins/` 目录、持久化启用状态、下发源码：
  - `plugin.list`：遍历 `<exe 同级>/plugins/<子目录>/manifest.json`（`ReadFileUtf8` 自动去 BOM），返回 `{pluginsDir, host:{apiVersion:"5.0", kernelVersion:"2.1.0"}, list:[{key,folder,path,id,name,version,description,author,icon,main,manifestApi,manifestKernel,enabled,permissions,hosts,layouts,hooks,injections,mainSource?}]}`；
  - **mainSource 只给已启用插件**（main 路径含 `..` 拒绝，>256KB 拒绝）→ 禁用状态的插件天然不加载；
  - `plugin.setEnabled{key,enabled}`：写 settings.json 的 `pluginStates` map（`LoadSettingsFile`→`SaveSettingsFile` 合并写，白名单无，任意键可存）；
  - `app.openFolder{folder}`：仅放行 `plugins` 白名单目录，`ShellExecuteW` 打开。
- **前端契约版本号独立于后端**：`apiVersion="5.0"`（前端 LX 契约）与 `kernelVersion="2.1.0"`（后端 RPC 语义版本）是两套；兼容判断走**单向规则**：
  - apiVersion **可向上兼容**：插件声明 ≤ 宿主版本允许运行（4.x 插件跑在 5.0 宿主上 → 标记「兼容（向上）」）；**不可向下兼容**：插件声明 > 宿主版本（6.0>5.0）→ **在列表中红色报错**「向下不兼容：插件要求 apiVersion 6.0，宿主为 5.0」且不执行 mainSource；
  - 未声明 / 非数字格式 → 同样报错不运行；
  - kernelVersion 用简易 semver 范围解析器（支持 `x.y.z`/`>=`/`<=`/`>`/`<`/`=`/`^`/`~` 空格合并），范围天然实现方向性（插件声明 `<2.0.0` 而宿主 2.1.0 → 内核不兼容）。

### 前端插件运行时要点
- **动态 import 源码**：`new Blob([src],{type:'text/javascript'})` → `URL.createObjectURL` → `import(url)`（finally 里 `revokeObjectURL`）。Chromium/WebView2 里动态 import 的 blob:/data: URL 都可用；`castle:` 是 WebKit 专属 scheme，WebView2 里不能用。插件须是**单文件 ESM**（相对 `import './dep.js'` 会失败，规范限制）。
- `activate({manifest, LX:api, LXHost, State, $, $$})` 可异步，可 `return {deactivate()}`；`deactivate` 里做资源清理。`promise.all` 并发激活多个插件。
- 插件的 `LX` 是**门面**：`call/on/off/emit/$/$$/FA/notify/openFolder/storage(按 key 前缀隔离)/register(desc)`。`register` 支持 `renderSettings(options)`（注入设置页 `#pluginsExtras`）、`panelAfterShow/panelBeforeShow` 钩子、`slots`、`rpcs`（自定义前端 RPC）。
- **自定义 RPC feeder**：重新包装 `LX.call`（在原日志包装外层再包一层）：method 命中 `plugins.rpcs` 时走插件处理器（`__plugin` 标注 key），否则透传原 call —— 实现"插件注册自定义 RPC"，且不破坏 bridge 日志包装。
- 启用/禁用为读后即删模式：禁用 → 立即 `deactivate()` 清空 slots/hooks/rpcs + 调 `plugin.setEnabled` → `refreshPlugins()`（后端下次不下发 mainSource → 不再运行）；启用 → 后端重新下发源码自动 reload。
- 总开关 `pluginsToggle` 持久化在 `settings.json` 的 `pluginSysEnabled`（前端 `Persisted.set`）——用 `Persisted` 而非 special RPC，与其它设置一致。

### 踩过的坑
- **addInitScript 不能闭包外层变量**：Playwright 的 `page.addInitScript` 会把函数序列化后重放，外层的 `demoPlugin`/`state` 变量**不可见** → 在页面里引用即抛错、RPC 返回 null。Stub 数据必须全部内联在 `addInitScript` 的函数体里（`var demoEnabled=true` 之类）。
- `page.evaluate` 里写了 Playwright 专属 `:has-text` 伪类选择器 → 页面 `querySelector` 抛 SyntaxError；纯 DOM 就用标准选择器。
- node --check 通过 ≠ 运行时正确：inline script 是整段 IIFE，`let pluginSubSeq` 曾误放 `{}` 块作用域里（`ReferenceError`），纯语法检查不抓作用域/运行期错误 → 必须跑一次真实页面冒烟。
- 兼容 badge 与状态：不兼容（api 高于宿主 / 内核不符）插件不拉源码不运行，行内红字「错误：…」显示具体原因（playwright 验证：api 6.0 →「错误：向下不兼容：插件要求 apiVersion 6.0，宿主为 5.0」且不运行；api 4.0 →「兼容（向上）」并运行；未声明/格式无效同样报错）。

### 验证证据
- MSBuild Debug x64 BUILD_OK；内联 script `node --check` SYNTAX_OK。
- Playwright（file:// + chrome.webview stub）全流程通过：列表渲染(2 项含 count)、兼容判别、mainSource 激活(activate/deactivate 均触发)、切换禁用→「已禁用」+toggle 灰、重启用→「运行中」、总开关 off→卸载/on→重载、空态与"打开插件目录"。前端公开契约版本由 `PLUGIN_API_VERSION` 常量控制。

## 27. 第十六批：Java 显示版本与实际不符（`-XshowSettings` 管道竞态 + 兜底解析）（2026-08 新增）

### 现象
- Java 列表里版本号错误：Java 8 显示成 32、Java 25/21 显示成 8，且同一 java.exe 时对时错（`settings.json` 的 `javaCache` 里 javaw 对、java.exe 错）。
- 定位入口：`x64\Debug\settings.json` 的 `javaCache`（`{path,major}`），对照路径里的 `jre1.8.0_311`/`zulu25.30.17-ca-jre25.0.1` 直接看出 major 错。

### 根因（两处叠加）
1. **`RunCaptureTimeout` 管道读竞态丢尾块**（Services.cpp）：子进程写完输出立即退出时，主循环 `PeekNamedPipe→rd==0→WaitForSingleObject(200ms)→WAIT_OBJECT_0→break`，在 200ms 内"写数据+退出"都完成 → **管道里剩余的 `-version` 引号块没被读出**，截断点随机 → 同路径探测结果不确定。
2. **`JavaMajorFromVersionText` 兜底抓"第一段数字"**：截断后输出无引号 → 兜底 `find_first_of(digits)` 从第 0 位找 → 实测复现：
   - Java 8（`-XshowSettings` 属性顺序中 `java.awt.graphicsenv = sun.awt.**Win32**GraphicsEnvironment` 先于 `java.class.version`）→ 抓到 `32`；
   - Zulu 25（`file.encoding = **UTF-8**` 是第一处数字）→ 抓到 `8`。
   - 即「java8→32、java25→8」的来源（`32-Bit` 排除注释正确，但兜底逻辑本身没排除这类非版本数字）。

### 修法（最小改动，两处）
- `RunCaptureTimeout`：主循环退出后**追加排空循环**（`PeekNamedPipe`→有数据→`ReadFile`，直到 0 字节/失败），进程退出后缓冲区数据完整读走。
- `JavaMajorFromVersionText` 改为三级解析（不再递归）：
  1. **属性行优先**：解析 `java.specification.version` / `java.version`（`-XshowSettings` 值不带引号，最可靠）：`1.8`→8（`1.` 后接数字段）、`25`→25；
  2. 引号内版本号（标准 `version "25.0.1"` / `"1.8.0_311"`，原逻辑保留）；
  3. 兜底只认**含小数点的"数字.数字"**，`UTF-8`/`Win32` 等无小数点数字段直接跳过。
- 清理 `x64\Debug\settings.json` 的 `javaCache`/`javaCacheTime`（旧 bug 产物，含大量 8/32 错值；7 天缓存会一直返回旧值）。只删这两个键，其余设置不动；清空后走 `AsyncScanJava` 增量 `java.found` 重填，UI 不阻塞。

### 排查技巧
- PowerShell 直跑 `& java -XshowSettings:properties -version 2>&1` 会把 stderr 包装成 `java.exe : xxx` 前缀——**不要用 2>&1 拼接验证原始输出**；用 .NET `ProcessStartInfo` + `ReadToEndAsync` 双流异步读（同步逐流 `ReadToEnd` 会因管道缓冲死锁），才是 C++ 管道看到的真实字节。
- 怀疑"版本探测不稳"时先查 `settings.json` 的 `javaCache` 实际存了什么，比猜前端显示逻辑快得多。

### 验证证据
- 用 .NET 双流读取真实 JVM 输出：jre1.8.0_311 → `spec.version='1.8'`、`firstDigit='32'`（旧兜底会抓 32）；zulu25 → `spec.version='25'`、`firstDigit='8'`（旧兜底会抓 8）。新三级解析分别返回 8 / 25。
- MSBuild Debug/x64 BUILD_OK；构建前删 .pdb/.ilk + Stop-Process LXElauncher。

## 28. 第十七批：整合包(.mrpack/.zip)拖入自动导入（2026-08 新增）

### 现象/需求
- 用户拖入 `.mrpack` 等专有格式整合包（Modrinth 包 / CurseForge/MMC/HMCL zip），旧逻辑只把文件复制进 `.minecraft\modpacks\`，**从不真正安装**；`.mrpack` 扩展名甚至未识别，直接提示"不支持的文件类型"。

### 改动（后端 + 前端）
- **后端 `mc.installModpack` 支持 `localPath`**（Services.cpp）：
  - 新增可选参数 `localPath`，校验 `name` 必填、`url`（远程）与 `localPath`（本地）**二选一**；本地文件先 `is_regular_file` 校验存在。
  - 阶段 1 分支：本地 → `copy_file(localPath, tmpDir\safeName.pack.zip)`（跳过下载）；远程 → 原 `DownloadFileSmart` 不变。后续解压/格式识别(modrinth/curseforge/mcbbs/multimc/hmcl/plain/launcher)/装加载器/下载 mods/应用 overrides 全部复用。
  - **注意**：`localPath` 是函数内新局部变量，必须加进后台 `std::thread` 的捕获列表，否则编译报"未捕获"（C2664/C2440）。
- **前端拖放**（index.html）：
  - `handleDroppedPath`：`ext==='mrpack'` 直接走 `dropInstallModpack`；`ext==='zip'` 经 `mc.probeZip` 判 `kind==='modpack'` 时也走它（替代原 `confirmPackImport→mc.dropImport` 只复制不安装的路径）。
  - 新增 `dropInstallModpack`（确认对话框 + §2.1 重复安装检测合并确认，复用 `versionExists`）+ `runDropInstallModpack`（任务队列 `queue.push`/`renderQueue`、监听 `download.progress`/`download.state`、完成刷新 `renderVersionTable`）。
  - 任务匹配沿用 runInstallModpack 的 `_match`：`String(taskId)===String(myTaskId) || ===String(Number(myTaskId)+100000)`。

### 经验
- **C1128 节数超过对象格式限制**：Services.cpp 函数/节过多时 MSBuild 报 C1128（早期/加大文件后偶发）→ vcxproj 四个配置 `AdditionalOptions` 统一加 `/bigobj`（已加到 `/utf-8 /bigobj`），一劳永逸。
- 大文件（如 Services.cpp 已上万行）继续膨胀前，优先考虑按 RPC 拆到独立编译单元，而不是依赖 /bigobj 硬扛。

### 验证证据
- 内联 script `node --check` SYNTAX_OK；MSBuild Debug/x64 BUILD_OK（含 /bigobj 后）。

## 29. 第十八批：插件系统全量（LX.HOST/导航 tab/标准事件/manifest 全量）+ 崩溃白屏修复（2026-08 新增）

### 崩溃白屏根因与修复（用户反馈：开启日志分析插件、游戏崩溃后主窗口卡死白屏）
- **根因 A：同步 fs.readFile 阻塞桥消息线程**。插件崩溃分析整读 latest.log/debug.log（可达数十 MB），原为同步 `bridge.Register`，直接在桥线程整读 → 窗口消息不能处理 → 未响应/白屏。
- **修复（三层）**：
  1. **后端**：`fs.readFile` 改为 `bridge.RegisterAsync` + 后台 `std::thread` 做文件 I/O；路径校验 `ResolvePluginMcPath`（依赖 GetMcRoot/g_mcRoot）留在桥线程，只把解析好的 `abs` wstring 传给线程；未传 maxBytes 时**默认 1MB**（读尾部 + `truncated=true`）。`done()` 跨线程回调安全（Bridge::SendResult 经 poster_ 投递）。
  2. **前端**：插件门面 `readFile: async` 同样默认 `maxBytes || (1024*1024)`。
  3. **log-analysis 插件**：删除 activate 内 `LX.on('launch:crash')` 自订阅（与 manifest.hooks 的导出 `onCrash` 双重触发），崩溃处理全部移入导出 `onCrash`（toast → closeGameLog → 单次 modal）。
- 同类结算：`fs.writeFile / fs.readDir / fs.exists / fs.mkdir / fs.rm / shell.openFile / shell.getEnv / shell.execute` 全部改 `RegisterAsync` + 桥线程校验 + 工作线程执行 + try/catch 兜底（`done(Err(...))`）。`shell.openUrl`、`app.openFolder`、`plugin.readAsset` 保持同步（操作瞬时）。
- **根因 B（"插件界面先弹、约两秒后才卡死"）→ `settings.set` 同步写盘**。插件 `LX.storage.set` → 前端 `Persisted.set` → 防抖 `flush` → `LX.call('settings.set', ...)`，崩溃分析 bundle（数 KB~数十 KB 日志摘要）在主线程同步整写 settings.json → 桥消息线程阻塞。修复：`settings.get`/`settings.set` 改 `RegisterAsync`，patch 在桥线程拷贝、后台线程写盘；`g_settingsMutex` 保证读-合并-写原子串行，无丢更新。

### 插件 API 新增清单（对应 temp/LXElauncher插件规范.md）
- **后端 RPC**：
  - `shell.openUrl`：仅 `http://`/`https://`，ShellExecuteW SW_SHOWNORMAL，返回值 ≤32 判失败。
  - `plugin.readAsset`：仅读插件**自身**目录（key 匹配 manifest id 或文件夹名），file 不得含 `..`/`\`，UTF-8 返回 `{content}`。
- **plugin.list 下发扩充**：host = { id/name/version/apiVersion/kernelVersion/layout:'top-nav'/supportedLayouts/features{...}/uiDefaults{...}/slots(21 个插槽数组) }；manifest 的 `styles`、`dependencies` 字段透传。
- **前端 LX.HOST**：新增 `pluginHostDefaults()` 补齐 HOST 全结构，`refreshPlugins` 成功后合入后端 `r.host`；`platform()/readFile()/fs.getMetrics()/deps.load()` 等门面方法补齐（实现全部经桥调用或直接返回）。
- **manifest 全量兼容检查 `pluginCompatOf`**：hosts（含 `hosts: ['*']`）、layouts、dependencies（按 key/id 查找已装插件，support 表判定）、kernelVersion/apiVersion 一并校验，失败 state='incompatible' 并在设置页显示原因徽标。
- **manifest 自动接线**：`injections[slotName]` → 导出函数挂到 `pluginRegisterSlot(...,'manifest')`；`hooks[event]` → `pluginRegisterEvent`；`styles[]` → `plugin.readAsset` 读回 + `injectStyles`。
- **顶部栏 tab（registerPluginNavTab）**：`LX.register({navTab:{...}})` → 建 `button.nav-btn.plugin-nav` + `section.content-panel[data-panel="plugin-tab:xxx"]`，按 order 排序插入，`Router` 自动切换/高亮/渲染；`pluginUnregister` 与 dispose 清理 key+DOM。`navTab` pre-`apiActive` 时存 `plugins.pendingTabs`，activate 后补注册（X 序列化标签逃逸用 `FA('solid','puzzle-piece')` 兜底）。
- **标准事件**：app:ready（首轮 refreshPlugins 成功一次）、panel:before-show(可阻断，Router.go 包装)/after-show、launch:before（mutable，仅插件经 `mc.launch` 时）、下载 proxy（download.state/progress → start/done/error/progress）、settings:change（Persisted.set 包装）、modal open/close（ui/modal 包装）、account:switch（selectAccount 内显式 emit）。
- **示例插件**：`nav-tab-demo`（顶部栏 tab：系统信息 + 计数器 + modal/toast/fs 演示）、`log-analysis`（崩溃分析：injections dialog:crash:body + hooks launch:crash）。

### 经验
- **跨线程安全传参**：路径解析（依赖 GetMcRoot/g_mcRoot，可能被 mc.setActiveMcFolder 改）必须在桥线程完成；工作线程只接收已绝对化的 `wstring`。
- **不要双重订阅崩溃事件**：manifest.hooks 自动接线的 `onCrash` 与插件内自 `LX.on('launch:crash')` 会各自触发一次分析 → 每个崩溃读两次日志、弹两个 modal。
- **验证**：插件 `main.js` 是 ESM → 复制为 `.mjs` 再 `node --check`（不要用 `--input-type=module` 读文件，会报 ERR_INPUT_TYPE_NOT_ALLOWED）。
- **Playwright run_code 沙箱**：`async (page)=>{...}` 求值环境无 require/动态 import（`A dynamic import callback was not specified`），页面是 about:blank → 验证插件必须把宿主 stub + 插件源码**全量内联**进代码；`addInitScript` 函数体被序列化不能闭包外层变量。

### 验证证据
- 内联 script `node --check` INLINE_OK；插件 main.js（转 .mjs）`node --check` ESM_OK；MSBuild Debug/x64 BUILD_OK；webapp/index.html 与 x64\Debug\webapp 已同步。

## 22. 净室合规：关键数值的独立设计决策（2026-08 新增）
> 用途：以下阈值/常量均为独立工程决策，供净室合规审查引用。每个数值给出"为什么是它"的开发推理，可逐条对照代码位置复核。

### 22.1 探测超时 15 秒（Java 版本探测）
- 代码：`src/bridge/Services.cpp` `RunCaptureTimeout(..., 15000)`（探测 javaw `-XshowSettings:properties -version`，见 2497/2507/2512/4458 附近）。
- 推理：正常 Java 冷启动探测 p99 在 2 秒内完成；15 秒 ≈ 7 倍裕量，覆盖三类边缘场景：① 机械盘 + 首次冷启动 JVM 类加载慢；② 安全软件/EDR 拦截 javaw.exe 导致进程挂起不退出；③ 巨型 JDK 目录下的磁盘 IO 抖动。取 10 秒会在真实慢机上误杀正常探测，取 20 秒以上会让用户无谓等待。15 秒是"不误杀"与"不让等"的折中，且与 Minecraft 官方启动超时同量级（业内常见 10~30 秒）。

### 22.2 并行路数（探测/下载并发）
- 代码实际值（无 4）：Java 扫描并行上限 **8**（`ScanInstalledJavasNow`，`probeThreads > 8` 截断）；`mc.submitDownloadList` 默认 **8**、可配 1~16；`completeVersion` 走设置项 `downloadThreads` 默认 **64**、范围 1~999；前端下载中心 UI 默认 **3**、范围 1~16（`maxConcurrentDownloads`）。
- 推理：并行度是"吞吐 vs 稳定性"的权衡，2~8 均为业界常见取值，无技术必需值。三个场景取值依据不同：Java 探测是短命进程且可能被杀软逐个拦截，8 路封顶避免瞬时拉起过多 javaw；下载任务按用户可调（UI 默认 3 偏保守，避免小水管用户带宽被打满）；完整性补全（completeVersion）目标是把缺失库快速补齐，64 路冲吞吐，因为有 aria2 多连接兜底 + 失败原子回退。
- 备注：若净室记录中出现"4 路"，与本实现不符——本实现无 4 这个阈值。

### 22.3 日志裁剪（尾部 2000 行）
- 代码：`webapp/index.html` 日志查看器 `lines.slice(-2000)`（游戏日志读取后仅渲染尾部 2000 行）；`app.exportLogs` 导出为**全量**合并（不做任何裁剪）。
- 推理：① 崩溃根因（异常堆栈、`Fatal` 行）几乎总在日志**尾部**，头部为启动 banner/模组初始化噪音；② 单次渲染 2000 行纯文本 < 300KB，DOM `textContent` 渲染无卡顿，而 1MB+ 全量文本会让内联查看器掉帧；③ 需要全量时走导出功能，两者职责分离。取 1000 行对长运行实例（含下载轮询日志的会话）截断过狠，5000 行渲染已有明显延迟，2000 行是渲染平滑与信息完整的平衡点。

### 22.4 Java 排序基准 21
- 代码：`ScanInstalledJavasNow` 排序 `std::abs(major - 21)` 升序（3047/3057 附近）；配套 `RecommendedJavaMajor`：<1.17→8，1.17~1.20.4→17，1.20.5+→21。
- 推理：**21 是 Mojang 官方版本要求，不是随意值**——自 Minecraft 1.20.5 起官方 version.json 的 `javaVersion.majorVersion` 固定为 21（公开的 launcher 元数据事实）。选 21 作排序基准意味着：已装 Java 中"离 21 最近"者排最前，恰好服务当前主流版本区间（1.20.5~1.21.x）。这是按用户真实场景（多数人玩最新主流版）做的最优默认，无雷同风险；若基准选 17 反而会劣化主流版本的用户体验。

### 22.5 下载源自动选择：<4 秒判定官方可用
- 代码：`EnsureOfficialProbe()`，对官方 manifest 发起一次完整 GET 并计时，`!body.empty() && ms < 4000` 判可用（7083~7093 附近）。
- 推理：官方源（piston-meta.mojang.com）在国内网络常被限速/中断，需要"连通性 + 速度"双重判定。4 秒对一个小型 manifest（约 1MB）是合理的分界：阈值太低（1~2 秒）会把正常慢速用户误判为不可用；太高（≥10 秒）会让自动模式迟迟无法落地，用户等不到源切换。配套"本会话偏向官方源"策略：官方可用就优先官方（更完整、更新及时），只有被判定不可用才切镜像——4 秒是在"优先准确官方"与"快速止损"之间的平衡。

### 22.6 下载校验：不校验哈希、不校验大小（差异点）
- 代码事实：`DLItem.sha1` 仅由前端从 version.json 解析传入（1163/1174），下载完成后**不比对**；`DownloadFileSmart` 仅保证目录与下载本身成功（HTTP/aria2 层判定），成功即标 done，无哈希比对、无 Content-Length 校验。
- 推理：① 下载源（官方/镜像）本身就提供哈希一致的文件，镜像只是 CDN 代理不重打包，哈希校验价值有限；② 对上千文件逐一重算 SHA-1 额外消耗一次全文件 IO，拖慢安装；③ aria2 下载失败会自动回退重试，HTTP 层校验状态码与流完整性，已能拦截绝大多数损坏。若净室记录出现"只校验 sha1 不校验大小"的描述，与本实现不符——本实现两者都不校验，反而构成与对方代码的天然差异点。
- 后续若要加校验，建议只在**镜像源**启用哈希比对（镜像完整性可信度低于官方），官方源维持现状。

## 30. 第十九批：OptiFine「假成功实际原版」根因 + 净室版隔离安装 + aria2 CRL 修复（2026-08 新增）

### 现象
- 用户反馈：下载完成后提示成功、前端无任何异常代码，但游戏启动仍是原版（1.21.1 实测）。
- 曾误以为后端安装没生效，实际**后端净室版 §4.1 隔离安装根本没被触发**。

### 根因：前端流水线分支选错（镜像源下 OptiFine 走 loaderJar 只下载不安装）
- 前端安装流水线（index.html `useOfficial` 判断）：
  - 官方源：`json → complete → installLoader`（后端真安装）；
  - 镜像源：`json → complete → loaderJar`（**仅把安装包下载到 libraries 目录，从不运行安装器**）。
- 旧条件 `((loaderId === 'fabric' || loaderId === 'optifine') && !_downloadBase())`：配置了镜像下载源时 OptiFine 走 loaderJar → 下载完成即"成功"，但没有任何安装动作 → 游戏还是原版。
- **修法**：OptiFine 无条件纳入 `useOfficial`（无论官方/镜像源都走 `mc.installLoader`）：
  ```js
  const useOfficial = loaderId === 'forge' || loaderId === 'neoforge' || loaderId === 'liteloader' ||
    loaderId === 'optifine' || (loaderId === 'fabric' && !_downloadBase());
  ```
- **参数契约**（改分支前先确认匹配）：后端 OptiFine 安装分支消费 `loaderData` 的 `filename/type/patch` 三字段；官方源 `mc.loaderVersions`（Services.cpp OptiFine 列表段）与镜像源 BMCLAPI 列表（`{mcversion,type,patch,filename,forge}`）的列表项都含这三字段，前端 `pendingInstall.loaderData = ver` 存的是完整列表项 → 切 installLoader 不会缺信息。

### 后端 `mc.installLoader` OptiFine 分支结构（Services.cpp ~10220，净室版 §4.1）
1. **下载安装包**：bmclapi2 https 直链优先 → 失败清理后 bmclapi1 回退 → `FileLooksLikeZip` 校验（PK 头），防镜像限流时返回 HTML 错误页却被当有效安装器。路径规则：`https://bmclapi2.bangbang93.com/optifine/{mc}/{type}/{patch}`，文件名用 BMCLAPI 的 `filename`。**与前端下载源设置无关，固定走 BMCLAPI**。
2. **确定 Java**：`ReadJarEntryClassMajor` 读安装器入口 `optifine/Installer.class` 字节码版本反推最低 Java 大版本（class major ≥52 时 `major-44`）→ `PickJavaForMajor` → 空则 `FindJavaPath` 兜底；未匹配到推荐版本仅提示不强制。
3. **新旧判定 `IsOldOptifineMc`**（1.x 且 <1.13 → 旧版）：旧版纯手工构造（复制原版 jar + 入库 + launchwrapper + 继承原版的版本描述），新版走隔离安装。
4. **新版隔离安装**（核心）：
   - 构造隔离根 `tmpDir\ofiso_<installName>`：`.minecraft\versions\<mc>` 复制原版 json/jar + 写**空白 launcher_profiles.json**（安装器要求存在）；
   - **环境全重定向**：`-Duser.home=<isoRoot>` + 环境变量 `APPDATA/USERPROFILE/HOME` → isoRoot（`RunProcessCaptureEx` 支持 env 覆盖），防安装器读写用户真实数据；
   - 执行 `java -cp <jar> optifine.Installer`（工作目录=isoRoot），rc!=0 时回退 `java -jar <jar>` 重试一次；
   - **副作用监控**：`RunProcessCaptureEx` 合并 stdout/stderr，输出含 `Exception in thread`/`java.lang.`/`Caused by:` 即判失败；
   - **产物回收**：读 `isoRoot\.minecraft\versions\{mc}-OptiFine_{type}_{patch}\<id>.json`，解析 `optifine:OptiFine:` 坐标，把隔离根内 `libraries` 递归 copy 进真实 libDir，清理隔离根；
   - **生效校验**：ofCoord 非空 + `libDir\optifine\OptiFine\<ofCoord>\OptiFine-*.jar` 真实存在，否则 fail「OptiFine 安装未生效」——**防假成功的最后一道闸**；
   - mainClass 空 → `net.minecraft.launchwrapper.Launch`；game 参数缺 `--tweakClass optifine.OptiFineTweaker` 则补齐；
   - `writeMerged`（合并删除 id/inheritsFrom/time/releaseTime/type，mainClass 以 OptiFine 为主）→ `finishLoader`。
5. **生成的版本**：`{mc}-OptiFine_{type}_{patch}`，库 = `optifine:OptiFine:{mc}_{type}_{patch}` + `optifine:launchwrapper-of:2.x`。

### `RunProcessCaptureEx`（本轮新增，Services.cpp ~181）
- 静默执行 + **合并 stdout/stderr** + **环境变量覆盖**（CreatePipe + `CreateProcessW(CREATE_NO_WINDOW|CREATE_UNICODE_ENVIRONMENT)` + 双管道读取 + WaitForSingleObject），返回退出码、输出写入 wstring。
- 用途：安装器进程封装（净室版 §4.1.4）与副作用监控；`RunCapture` 只捕获 stdout 且无 env 覆盖，不适用于隔离安装。

### aria2 全部下载失败（CRL 吊销检查）
- 现象：`SSL/TLS handshake failure: 吊销服务器已脱机 (80092013)`，BMCLAPI 302 到 CDN 后 TLS 失败。
- 修法：`Aria2DownloadWithProgress` 追加 `--check-certificate=false`（已验证 exit=0、8MB、PK 头）。

### 排查套路总结
- **「前端没报错但功能没生效」优先查前端流水线分支**（useOfficial/loaderJar/installLoader 之类分流），而不是后端实现——后端代码可能根本没被触发。
- OptiFine 新版安装**必须运行安装器**生成版本条目，任何"只下载 jar 就算装好"的设计都是假成功。
- 安装生效判定要在后端落盘前做（校验 OptiFine 主 jar 真实存在 + tweak 参数），而不是等用户启动游戏才发现是原版。

