# LXElauncher UI 设计文档（草稿 v2）

> 基于 **sungbly 玻璃拟态主题（LINTUI2-theme-SKILL.md）** 为 MC 启动器生成前端 UI。
> 主题规则（`:root` + 6 主题类、玻璃配方、组件类名、JS 切换逻辑）全部原样复用。
> v2 变更：无边框窗口、顶栏重设计、主页中央大按钮布局、调色盘改到设置页并支持拖动。

---

## 1. 窗口与整体布局（无边框）

- **取消原生标题栏**：窗口样式去掉 `WS_CAPTION`（保留 `WS_THICKFRAME|WS_MINIMIZEBOX|WS_MAXIMIZEBOX` 保证可缩放），标题栏改为网页内实现。
- **拖动区域**：顶栏 `.navbar` 即拖动区。JS 在 mousedown 时通知后端 `window.startDrag`（后端 `ReleaseCapture` + `WM_NCLBUTTONDOWN/HTCAPTION`），实现网页内拖拽移动。
- **窗口控制按钮**（右上角）：最小化 / 最大化切换 / 关闭，调用后端 `window.minimize / window.toggleMaximize / window.close`。
- 窗口默认 1024×720（后端已设），背景为页面 `linear-gradient(135deg, var(--bg-gradient-start), var(--bg-gradient-end))`。

```
┌──────────────────────────────────────────────────────────────────┐
│ .navbar (玻璃圆角16px 高56px top/left/right 20px，整条为拖动区)      │
│  [账号▾] [自定义按钮1][自定义按钮2]…   [组件管理][设置]  [_][□][X]   │
├──────────────────────────────────────────────────────────────────┤
│ .content-area                                                    │
│  ┌────────────────────────────────────────────────────────────┐  │
│  │ .content-panel（页面随导航切换，shrink/grow 动画）           │  │
│  └────────────────────────────────────────────────────────────┘  │
└──────────────────────────────────────────────────────────────────┘
```

- 导航即顶栏按钮，**取消侧栏**；页面切换沿用 `shrink-down/grow-from-up` 动画。

---

## 2. 顶栏明细（左 → 右）

### 2.1 账号下拉（`.account-dropdown`）
- **收起态**：显示当前账号圆形头像 `.account-switcher-icon` + 当前玩家名 + 下拉箭头。
- **展开态**（玻璃浮层，`transform` 展开动画）逐行展示：
  - 账号头像、账号名、玩家名
  - 玩家名颜色语义：**绿色=正版**、**灰色=离线**、**蓝色=第三方认证库**
  - 底部 **「修改账户设置」按钮** → 点击跳转「设置 → 玩家」
- 支持多账号，可点击切换（复用 `.account-switcher-item` 结构）。
- 数据来自后端 `auth.list`（仅脱敏：头像、账号名、玩家名、类型）。

### 2.2 组件自定义按钮条（`.custom-bar`）
- 账号下拉右侧的一行**用户可自定义按钮**（一条、可增删排序，设置里管理）。
- 每个按钮 = 图标 + 可选文字，点击执行对应动作（跳页/打开浮窗/运行命令）。
- 默认内置：下载、模组、整合包、资源包（对应页面跳转）。

### 2.3 组件管理按钮
- 打开「组件管理」页：管理已安装版本 / 模组 / 整合包 / 组件（见 §4.3）。

### 2.4 设置按钮
- 打开「设置」页（见 §4.4），内含玩家/启动/下载/外观/关于 分栏。

### 2.5 窗口控制（右上角，原色板位置）
- `_` 最小化 ｜ `□` 最大化/还原 ｜ `✕` 关闭；hover 主色描边，关闭 hover 红底。
- 调用后端 `window.*` 方法（见 §5 新增 WindowService）。

> 颜色切换组件已移除：6 主题切换 + 调色盘移入「设置 → 外观」。

---

## 3. 主页（启动台）

### 3.1 中央三按钮
```
        [启动设置 ⚙]          [版本选择]          [版本设置 📦]
         (图标按钮)      (中央，最大，圆形/大卡)      (图标按钮)
```
- **中央 · 版本选择按钮（最大）**：
  - 显示当前版本图标（按版本类型映射，见下表），图标下方显示版本名称。
  - 点击弹出版本选择悬浮窗（`.form-select-wrapper` 大浮层）：已安装版本 + 官方版本列表，选中即切换。
- **左侧 · 启动设置（图标按钮）**：悬浮按钮，**hover 显示背景 + 上方弹出按钮名**；点击打开「启动设置悬浮窗」。
- **右侧 · 版本设置（图标按钮）**：同交互；点击打开「版本设置悬浮窗」。
- 按钮名是否悬浮显示可在「设置 → 外观」中开关（`.toggle-switch` 项）。

### 3.2 版本图标映射表（`.version-icon`）
| 版本类型 | 图标 |
|---|---|
| 原版 Vanilla | 草方块 |
| Forge | 铁砧 |
| Fabric | 布/织线 |
| NeoForge | 狐狸 |
| Quilt | 羽毛 |
| OptiFine | 蜡烛 |
| 整合包 | 箱子 |
| 其他 | 通用方块图标（fallback） |

### 3.3 右下角启动按钮
- `.primary-btn` **长方形**，位于内容区右下角：`启动游戏`。
- 点击 → 后端 `launch.prepare` + `launch.start`；运行中按钮变 `停止游戏`（调用 `launch.kill`）。
- 启动前若未选中版本/未登录，弹 `.notification.error` 或 `.confirm-dialog`。

### 3.4 底部日志与状态
- 游戏运行日志：`.detail-code` 等宽卡片，滚动，接收 `launch.log` 事件实时追加（可折叠）。
- 加载/空态复用 `.loading-spinner` / `.empty-state`。

---

## 4. 其他页面

### 4.1 下载中心
- 分类 Tabs（官方 / Fabric / Forge / NeoForge / 材质 / 资源包）。
- 搜索 `.form-input` + 筛选 `.form-select-wrapper`。
- 版本卡片 `.feature-card` 网格，右下「安装」。
- 下载队列复用「上传框」：`.upload-box/.upload-item/.upload-progress-bar/.upload-remove`，监听 `download.progress / download.state`。
- 后端：`download.*`（列表在前端组织，写盘在后端）。

### 4.2 模组 / 整合包（合并入组件管理，也可由自定义按钮直达）
- 安装：`.drag-zone` + `.file-list`（`.jar/.zip`，后端校验+解压）。
- 列表：`.app-item`（名称/版本/状态点）+ `.toggle-switch` 启停 + `.danger-btn` 删除（`.confirm-dialog` 二次确认）。
- 详情：`.dialog-overlay/.dialog-box` 展示描述/依赖。

### 4.3 组件管理（组件管理按钮打开）
- 分栏 Tabs：版本 / 模组 / 整合包 / 组件。
- 版本：列表（图标+名称+类型+「重命名」「删除」），重命名用 `.dialog-input`。
- 后端：`version.list / rename / remove`、`mods.*`、`modpack.*`。

### 4.4 设置
分栏（左侧小 Tabs 或右侧分段）：
- **玩家**：账号列表管理（添加微软/离线/第三方认证、删除），「账户设置」入口目标页。
- **启动**：Java 路径（`system.java.detect` 下拉）、内存分配（滑块+`.form-input`）、JVM 参数（`.form-textarea`）、分辨率 `.form-select-wrapper`、游戏目录。
- **下载**：线程数、并发、镜像源下拉、断点续传/校验开关。
- **外观**：
  - 6 主题切换按钮（`.color-btn`，移至此）
  - **调色盘 `.palette`（支持拖动）**：预设色块 + 原生取色器 + **R/G/B 或 H/S/L 拖动滑块**，拖动实时更新 `--primary-color`（写 cookie，与主题并存）
  - 「悬浮显示按钮名」开关（`.toggle-switch`）
- **关于**：版本、仓库链接、`detail-item`。

### 4.5 账号
- 登录入口：设置 → 玩家；主页顶部只有账号下拉。
- 微软登录设备流：后端返回 URL/二维码（`.detail-code`）→ 轮询 `auth.poll` → 成功 `notification.success`。

---

## 5. 前后端接口（新增 WindowService）

```
UI → bridge.js LX.call() → 后端 Bridge(白名单校验) → Service → result
后端事件 → PostWebMessageAsJson → 前端 LX.on() 更新界面
```

| 方法 | 说明 |
|---|---|
| `window.minimize` / `window.toggleMaximize` / `window.close` | 无边框窗口控制（C++ `ShowWindow`/`SendMessage`/`DestroyWindow`） |
| `window.startDrag` | 顶栏 mousedown 触发：`ReleaseCapture`+`WM_NCLBUTTONDOWN/HTCAPTION` |
| `window.getState` | 返回当前最大化/最小化状态（前端同步按钮图标） |
| `auth.list` | 账号脱敏列表（头像/账号名/玩家名/类型：正版/离线/第三方） |
| `auth.poll` / `auth.logout` | 登录轮询 / 退出 |
| `settings.get / settings.set` | 白名单键读写（含自定义按钮条、按钮名开关） |
| `system.info` / `system.java.detect` | 内存磁盘 / Java 探测 |
| `version.*` / `download.*` / `mods.*` / `modpack.*` | 版本/下载/模组/整合包（现有设计） |

---

## 6. 文件落地计划

| 文件 | 职责 |
|---|---|
| `webapp/index.html` | 骨架：无边框 navbar（账号下拉/自定义条/组件管理/设置/窗口控制）+ 各 `.content-panel` |
| `webapp/css/theme.css` | 主题规范逐字引入 + 新增 `.account-dropdown/.custom-bar/.window-controls/.version-icon/.palette-sliders` 等扩展样式 |
| `webapp/js/bridge.js` | 已有，不动 |
| `webapp/js/app.js` | 导航、账号下拉、窗口控制、页面渲染、事件绑定（可拆 `pages/`） |
| `webapp/js/demo.js` | 演示脚本，UI 完成后删除 |

> 后端需同步：无边框窗口改造（去 `WS_CAPTION`）+ 新增 WindowService + 设置白名单键。

---

## 7. 待确认问题

1. 无边框窗口的**缩放边缘**：保留系统缩放边框，还是也做成网页内缩放手柄？
2. 「组件自定义按钮条」默认放哪些按钮？支持排序还是仅开关？
3. 主页中央三按钮的尺寸比例：版本选择按钮建议直径 ~140px，两侧 56px 图标按钮，可否？
4. 调色盘拖动改色：优先 **HSL 三滑块** 还是 **R/G/B 三滑块**，或两者都要？
5. 官方/其他版本（如 OptiFine、Quilt）图标没有素材时是否用 fallback 图标？
6. 版本选择悬浮窗是**全屏大浮层**还是**居中玻璃卡**？
