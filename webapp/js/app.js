/* ============================================================
   LXElauncher 前端逻辑 — LINTUI2 主题 + WebView2 JSON-RPC + Font Awesome
   - 顶栏标题 LXElauncher + 动态版本号（app.config 读 appsettings.json）
   - 插件系统：LX.plugins.register(plugin)，可注入自定义按钮/面板内容
   - 下载中心：游戏版本 / 模组 / 整合包 三分类
   - 已删除：模组管理页、整合包管理页（功能合并入下载中心）
   ============================================================ */
(function (LX, document, window) {
  'use strict';

  const $ = (id) => document.getElementById(id);
  const $$ = (sel) => Array.prototype.slice.call(document.querySelectorAll(sel));
  const FA = { cls: (c) => `<i class="${c}" aria-hidden="true"></i>` };

  /* ---------- 全局状态 ---------- */
  const State = {
    theme: 'blue',
    currentPanel: 'home',
    currentVersion: null,
    selectedAccount: null,
    mem: 4096,
    customPrimary: null,
    version: '0.1.0',
    currentVersionBranch: 'release',
    gameRunning: false,
    gamePids: [],
    allowMultiInstance: false,
    maxConcurrentDownloads: 3,
    mcFolders: [],
    activeMcFolder: null,
    accounts: [
      { id: 'a1', avatar: 'S', accountName: 'Steve@123', playerName: 'Steve', type: 'premium' },
      { id: 'a2', avatar: 'A', accountName: 'Alex_offline', playerName: 'Alex', type: 'offline' },
      { id: 'a3', avatar: 'T', accountName: 'third@auth.cn', playerName: 'ThirdPlayer', type: 'thirdparty' },
    ],
    versions: [
      { id: '1.21.4', type: 'vanilla', name: '1.21.4', sub: '最新正式版' },
      { id: '1.21.4-forge', type: 'forge', name: '1.21.4-Forge', sub: 'Forge 51.0.35' },
      { id: '1.21.4-fabric', type: 'fabric', name: '1.21.4-Fabric', sub: 'Fabric Loader 0.16.10' },
      { id: '1.21.4-neoforge', type: 'neoforge', name: '1.21.4-NeoForge', sub: 'NeoForge 21.4.140' },
      { id: '1.20.1', type: 'vanilla', name: '1.20.1', sub: '经典版本' },
      { id: '1.20.1-forge', type: 'forge', name: '1.20.1-Forge', sub: 'Forge 47.3.40' },
      { id: '1.20.4-fabric', type: 'fabric', name: '1.20.4-Fabric', sub: 'Fabric Loader 0.16.10' },
      { id: '1.19.4-quilt', type: 'quilt', name: '1.19.4-Quilt', sub: 'Quilt Loader 0.27.3' },
    ],
    downloads: {
      game: [
        { name: '1.21.4', desc: '最新正式版 · 官方', kind: 'vanilla', icon: 'fa-solid fa-gamepad' },
        { name: '1.21.3', desc: '正式版 · 官方',   kind: 'vanilla', icon: 'fa-solid fa-gamepad' },
        { name: '1.20.1', desc: '经典正式版 · 官方', kind: 'vanilla', icon: 'fa-solid fa-gamepad' },
        { name: '1.21.4 Fabric',    desc: 'Fabric Loader 0.16.10', kind: 'fabric',    icon: 'fa-solid fa-layer-group' },
        { name: '1.21.4 Forge',     desc: 'Forge 51.0.35',         kind: 'forge',     icon: 'fa-solid fa-fire' },
        { name: '1.21.4 NeoForge',  desc: 'NeoForge 21.4.140',     kind: 'neoforge',  icon: 'fa-solid fa-feather-pointed' },
        { name: '1.20.1 Fabric',    desc: 'Fabric Loader 0.15.11', kind: 'fabric',    icon: 'fa-solid fa-layer-group' },
        { name: '1.20.1 Forge',     desc: 'Forge 47.3.40',         kind: 'forge',     icon: 'fa-solid fa-fire' },
      ],
      mod: [
        { name: 'Sodium', desc: 'v0.6.6 · 性能渲染优化 · MC 1.21.4',   author: 'FlashyReese', kind: 'mod', icon: 'fa-solid fa-gauge-high' },
        { name: 'Lithium', desc: 'v0.14.2 · 服务端性能优化 · MC 1.21.4', author: 'jellysquid3',  kind: 'mod', icon: 'fa-solid fa-bolt' },
        { name: 'Iris', desc: 'v1.8.0 · 光影加载 · MC 1.21.4',         author: 'coderbot16',   kind: 'mod', icon: 'fa-solid fa-eye' },
        { name: 'Fabric API', desc: 'v0.115.0 · Fabric 必备模组',       author: 'FabricMC',     kind: 'mod', icon: 'fa-solid fa-puzzle-piece' },
        { name: 'JEI', desc: 'v18.3.0.334 · 物品合成查看',              author: 'mezz',         kind: 'mod', icon: 'fa-solid fa-book' },
      ],
      modpack: [
        { name: 'Better MC [FABRIC] 1.21.4', desc: 'v23 · 大型科技/冒险整合包 · 300+模组', author: 'bettermc', kind: 'modpack', icon: 'fa-solid fa-box-archive' },
        { name: 'SkyFactory One', desc: 'v1.0.6 · 空岛整合包 · MC 1.20.1',                 author: 'Darkosto',  kind: 'modpack', icon: 'fa-solid fa-cloud' },
        { name: 'All the Mods 10', desc: 'v1.1.0 · 大型科技整合包 · 1.20.1',                author: 'ATMTeam',   kind: 'modpack', icon: 'fa-solid fa-cubes' },
      ],
      java: [],      // BMCLAPI /java/list 返回数据填充
      loaders: [],   // BMCLAPI Forge/NeoForge/OptiFine 填充
    },
    // BMCLAPI 相关
    bmcl: {
      mirrorSource: 'official',   // 'official' | 'bmclapi'
      baseUrl: 'https://bmclapi2.bangbang93.com',
      javaList: null,             // 缓存 BMCLAPI /java/list
      forgeMcVersions: null,      // 缓存 Forge 支持的 MC 版本
      forgeCache: new Map(),      // mcVersion -> forge version list
      neoforgeCache: new Map(),   // mcVersion -> neoforge version list
      optifineList: null,         // 全量 optifine 列表
      liteloaderList: null,
      loaderSubtab: 'forge',      // 当前加载器子标签
      loaderMcVersion: '1.21.4',  // 当前加载器筛选的 MC 版本
      loadersLoading: false,
      javaLoading: false,
    },
  };

  /* 版本类型 → FA 图标 / 标签 */
  const TYPE_ICON = {
    vanilla: 'fa-solid fa-cubes', forge: 'fa-solid fa-fire',
    fabric: 'fa-solid fa-layer-group', neoforge: 'fa-solid fa-feather-pointed',
    quilt: 'fa-solid fa-feather', optifine: 'fa-solid fa-candle', modpack: 'fa-solid fa-box-archive',
  };
  const TYPE_LABEL = {
    vanilla: '原版', forge: 'Forge', fabric: 'Fabric',
    neoforge: 'NeoForge', quilt: 'Quilt', optifine: 'OptiFine', modpack: '整合包',
  };
  const TYPE_CLASS = { premium: 'pname-premium', offline: 'pname-offline', thirdparty: 'pname-thirdparty' };

  /* ============================================================
     BMCLAPI 接口封装（列出/下载）
     基础文档：BangBang93 BMCLAPI
     主要分组：Forge / NeoForge / OptiFine / Java / LiteLoader / Version 本体
     ============================================================ */
  const BMCLAPI = {
    // 镜像源切换：official 用官方源直连，bmclapi 走 State.bmcl.baseUrl
    get base() {
      return State.bmcl.mirrorSource === 'bmclapi' ? State.bmcl.baseUrl : '';
    },

    // 通用 fetch 包装：失败返回 null，不抛异常（UI 有占位兜底）
    async _fetch(path, params) {
      const url = (this.base || State.bmcl.baseUrl) + path;
      try {
        const ctrl = new AbortController();
        const t = setTimeout(() => ctrl.abort(), 15000);
        const r = await fetch(url, Object.assign({ signal: ctrl.signal }, params || {}));
        clearTimeout(t);
        if (!r.ok) return null;
        return await r.json();
      } catch (e) { return null; }
    },

    /* ---------- Forge ---------- */
    // Forge 支持的 Minecraft 版本列表：GET /forge/minecraft
    async forgeMcVersions(force) {
      if (!force && State.bmcl.forgeMcVersions) return State.bmcl.forgeMcVersions;
      const list = await this._fetch('/forge/minecraft');
      if (Array.isArray(list)) State.bmcl.forgeMcVersions = list;
      return list || [];
    },
    // 根据 MC 版本获取 Forge 版本列表：GET /forge/minecraft/:id
    async forgeByMc(mcVersion, force) {
      const key = String(mcVersion);
      if (!force && State.bmcl.forgeCache.has(key)) return State.bmcl.forgeCache.get(key);
      const list = await this._fetch('/forge/minecraft/' + encodeURIComponent(key));
      if (Array.isArray(list)) State.bmcl.forgeCache.set(key, list);
      return list || [];
    },
    // 推荐版本：GET /forge/promos → { mcVersion: { latest, recommended } }
    async forgePromos() {
      const r = await this._fetch('/forge/promos');
      return r && typeof r === 'object' ? r : {};
    },
    // 下载 Forge：/forge/download 或 /forge/download/:build（构造 URL 由后端下载）
    forgeDownloadUrl(build) {
      const base = State.bmcl.mirrorSource === 'bmclapi' ? State.bmcl.baseUrl : 'https://bmclapi2.bangbang93.com';
      return build ? `${base}/forge/download/${encodeURIComponent(build)}` : `${base}/forge/download`;
    },

    /* ---------- NeoForge ---------- */
    // NeoForge 列表（按 MC 版本）：GET /neoforge/list/:mcversion
    async neoforgeByMc(mcVersion, force) {
      const key = String(mcVersion);
      if (!force && State.bmcl.neoforgeCache.has(key)) return State.bmcl.neoforgeCache.get(key);
      const list = await this._fetch('/neoforge/list/' + encodeURIComponent(key));
      if (Array.isArray(list)) State.bmcl.neoforgeCache.set(key, list);
      return list || [];
    },
    // NeoForge 版本详情：GET /neoforge/version/:version
    async neoforgeVersion(version) {
      return await this._fetch('/neoforge/version/' + encodeURIComponent(version));
    },
    // 下载 NeoForge 文件：/neoforge/version/:version/download/:file (file=installer 等)
    neoforgeDownloadUrl(version, file) {
      const base = State.bmcl.mirrorSource === 'bmclapi' ? State.bmcl.baseUrl : 'https://bmclapi2.bangbang93.com';
      return `${base}/neoforge/version/${encodeURIComponent(version)}/download/${encodeURIComponent(file || 'installer')}`;
    },

    /* ---------- OptiFine ---------- */
    // 全部 OptiFine：GET /optifine/versionList
    async optifineAll(force) {
      if (!force && Array.isArray(State.bmcl.optifineList)) return State.bmcl.optifineList;
      const list = await this._fetch('/optifine/versionList');
      if (Array.isArray(list)) State.bmcl.optifineList = list;
      return list || [];
    },
    // 按 MC 版本筛选 OptiFine：GET /optifine/:mcversion
    async optifineByMc(mcVersion) {
      const list = await this._fetch('/optifine/' + encodeURIComponent(String(mcVersion)));
      return Array.isArray(list) ? list : [];
    },
    // 下载 OptiFine：/optifine/:mcversion/:type/:patch
    optifineDownloadUrl(mcVersion, type, patch) {
      const base = State.bmcl.mirrorSource === 'bmclapi' ? State.bmcl.baseUrl : 'https://bmclapi2.bangbang93.com';
      return `${base}/optifine/${encodeURIComponent(mcVersion)}/${encodeURIComponent(type)}/${encodeURIComponent(patch)}`;
    },

    /* ---------- Java 运行时 ---------- */
    // Java 列表：GET /java/list
    async javaList(force) {
      if (!force && Array.isArray(State.bmcl.javaList)) return State.bmcl.javaList;
      const list = await this._fetch('/java/list');
      if (Array.isArray(list)) {
        State.bmcl.javaList = list;
        // 同步到 State.downloads.java 供搜索/筛选统一使用
        State.downloads.java = list.map((j) => ({
          id: j.id || j.filename || j.name,
          name: j.name || j.filename || 'Java',
          desc: (j.os ? j.os + ' · ' : '') + (j.arch ? j.arch + ' · ' : '') + (j.version || '') + (j.size ? ' · ' + humanSize(j.size) : ''),
          kind: 'java',
          icon: 'fa-brands fa-java',
          author: j.vendor || '',
          size: j.size,
          url: j.url || (j.download && j.download.url) || '',
          raw: j,
        }));
      }
      return State.bmcl.javaList || [];
    },

    /* ---------- LiteLoader ---------- */
    async liteloaderList(force) {
      if (!force && Array.isArray(State.bmcl.liteloaderList)) return State.bmcl.liteloaderList;
      const list = await this._fetch('/liteloader/list');
      if (Array.isArray(list)) State.bmcl.liteloaderList = list;
      return list || [];
    },

    /* ---------- MC 本体下载（Version） ---------- */
    // /version/:version/:category   category: client / server / client_mappings / server_mappings
    versionDownloadUrl(version, category) {
      const base = State.bmcl.mirrorSource === 'bmclapi' ? State.bmcl.baseUrl : 'https://bmclapi2.bangbang93.com';
      return `${base}/version/${encodeURIComponent(version)}/${encodeURIComponent(category || 'client')}`;
    },
  };

  // 字节数 → 可读
  function humanSize(n) {
    if (!n || isNaN(+n)) return '—';
    const v = +n;
    const units = ['B', 'KB', 'MB', 'GB', 'TB'];
    let i = 0, x = v;
    while (x >= 1024 && i < units.length - 1) { x /= 1024; i++; }
    return x.toFixed(x < 10 && i > 0 ? 2 : 1) + ' ' + units[i];
  }

  /* ============================================================
     插件系统（LX.plugins）
     插件可：注册自定义按钮、把 HTML/元素注入 data-plugin-slot 槽、
     注册面板切换钩子、订阅/发布事件，或注入自定义 RPC。
     ============================================================ */
  const pluginsStore = new Map();  // id -> plugin
  const slotRenderers = new Map(); // slotName -> fn(hostEl, LX, State)[]
  const hooks = { panelAfterShow: [], panelBeforeShow: [] };
  LX.plugins = {
    register(plugin) {
      if (!plugin || typeof plugin.id !== 'string') throw new Error('插件缺少 id');
      if (pluginsStore.has(plugin.id)) throw new Error('插件 id 重复：' + plugin.id);
      pluginsStore.set(plugin.id, plugin);
      if (typeof plugin.slotRenderers === 'object') {
        Object.keys(plugin.slotRenderers).forEach((slot) => {
          if (!slotRenderers.has(slot)) slotRenderers.set(slot, []);
          slotRenderers.get(slot).push(plugin.slotRenderers[slot]);
        });
      }
      if (typeof plugin.onPanelShow === 'function') hooks.panelAfterShow.push(plugin.onPanelShow);
      if (typeof plugin.init === 'function') {
        try { plugin.init({ LX: LX, State: State, $: $, $$: $$ }); }
        catch (e) { console.error('[plugin] init fail', plugin.id, e); }
      }
      renderPluginsList();
    },
    unregister(id) { pluginsStore.delete(id); renderPluginsList(); },
    list() { return Array.from(pluginsStore.values()); },
    // 主渲染流程里会调用，公开给插件/测试直接使用
    flushSlots() {
      $$('[data-plugin-slot]').forEach((host) => {
        const name = host.dataset.pluginSlot;
        host.innerHTML = '';
        const fns = slotRenderers.get(name) || [];
        fns.forEach((fn) => {
          try { fn(host, LX, State); } catch (e) { console.error('[plugin] slot err', name, e); }
        });
      });
    },
  };

  function renderPluginsList() {
    const host = $('pluginsList');
    if (!host) return;
    const list = LX.plugins.list();
    if (!list.length) {
      host.innerHTML = ''
        + '<div class="empty-state">'
        +   '<div class="empty-state-icon">' + FA.cls('fa-regular fa-folder-open') + '</div>'
        +   '<div>plugins/ 目录为空，<a href="javascript:void(0)" id="openPluginsFolder">打开 plugins 目录</a></div>'
        + '</div>';
      const a = $('openPluginsFolder');
      if (a) a.onclick = () => LX.call?.('app.openFolder', { folder: 'plugins' }).catch(() => notify('当前未接入 openFolder 后端接口', 'error'));
      return;
    }
    host.innerHTML = '';
    list.forEach((p) => {
      const row = document.createElement('div');
      row.className = 'plugins-card-item';
      row.innerHTML = ''
        + '<div class="pi-icon">' + FA.cls(p.icon || 'fa-solid fa-plug') + '</div>'
        + '<div class="pi-info">'
        +   '<div class="pi-name">' + (p.name || p.id) + ' <span class="uid-badge" style="margin-left:6px;">v' + (p.version || '0.1') + '</span></div>'
        +   '<div class="pi-desc">' + (p.description || '') + '</div>'
        + '</div>';
      host.appendChild(row);
    });
  }

  /* ============================================================
     主题切换（LINTUI2 规范）
     ============================================================ */
  let currentTheme = 'blue';
  function setTheme(theme) {
    document.body.className = 'theme-' + theme;
    currentTheme = theme;
    State.theme = theme;
    if (State.customPrimary) applyCustomPrimary(State.customPrimary);
    saveThemeToCookie(theme);
    updateThemeButtons(theme);
  }
  function updateThemeButtons(activeTheme) {
    document.querySelectorAll('.color-btn').forEach((b) => b.classList.remove('selected'));
    const btn = document.querySelector('.color-btn.' + activeTheme);
    if (btn) btn.classList.add('selected');
  }
  function saveThemeToCookie(theme) {
    const d = new Date();
    d.setTime(d.getTime() + (365 * 24 * 60 * 60 * 1000));
    document.cookie = 'sungbly_theme=' + theme + ';expires=' + d.toUTCString() + ';path=/';
  }
  function getThemeFromCookie() {
    const m = decodeURIComponent(document.cookie).match(/(?:^|;\s*)sungbly_theme=([^;]*)/);
    return m ? m[1] : '';
  }
  function loadSavedTheme() {
    const saved = getThemeFromCookie();
    if (saved) { document.body.className = 'theme-' + saved; currentTheme = saved; State.theme = saved; }
    updateThemeButtons(currentTheme);
  }

  /* ============================================================
     通知 / 确认框
     ============================================================ */
  let notifTimer = null;
  function notify(msg, type) {
    const el = $('notification');
    if (!el) return;
    el.textContent = msg;
    el.className = 'notification show ' + (type || 'info');
    clearTimeout(notifTimer);
    notifTimer = setTimeout(() => { el.classList.remove('show'); }, 2600);
  }
  function confirmBox(title, message, onOk) {
    $('confirmTitle').textContent = ' ' + title;
    $('confirmMessage').textContent = message;
    $('confirmOverlay').classList.add('open');
    const ok = $('confirmOk');
    const cancel = $('confirmCancel');
    const close = () => { $('confirmOverlay').classList.remove('open'); ok.onclick = null; cancel.onclick = null; };
    cancel.onclick = close;
    ok.onclick = () => { close(); if (onOk) onOk(); };
  }

  /* ============================================================
     面板导航
     ============================================================ */
  function showPanel(name) {
    hooks.panelBeforeShow.forEach((h) => { try { h(name, State); } catch (e) {} });
    State.currentPanel = name;
    $$('.content-panel').forEach((p) => p.classList.remove('active'));
    const panel = document.querySelector('.content-panel[data-panel="' + name + '"]');
    if (panel) panel.classList.add('active');
    $$('.nav-btn').forEach((b) => b.classList.remove('active'));
    const nav = $('nav' + name.charAt(0).toUpperCase() + name.slice(1));
    if (nav) nav.classList.add('active');
    updateCustomBarActive();
    // 插件插槽重新跑一次
    LX.plugins.flushSlots();
    hooks.panelAfterShow.forEach((h) => { try { h(name, State); } catch (e) {} });
  }

  /* ============================================================
     顶栏版本号
     ============================================================ */
  function applyVersionToTitle(ver) {
    State.version = ver || '0.1.0';
    const t = $('titleVersion');
    if (t) t.textContent = 'v' + State.version;
    const about = $('aboutVersion');
    if (about) about.textContent = State.version;
  }

  /* ============================================================
     账号下拉
     ============================================================ */
  function accountName(a) {
    if (a.type === 'premium') return '正版';
    if (a.type === 'offline') return '离线';
    return '第三方认证';
  }
  function renderAccountMenu() {
    const list = $('accountMenuList');
    list.innerHTML = '';
    State.accounts.forEach((a) => {
      const item = document.createElement('div');
      item.className = 'account-menu-item' + (State.selectedAccount === a.id ? ' selected' : '');
      const icon = document.createElement('div');
      icon.className = 'account-switcher-icon';
      icon.textContent = a.avatar;
      const info = document.createElement('div');
      info.className = 'am-info';
      const acc = document.createElement('div');
      acc.className = 'am-accname';
      acc.textContent = a.accountName;
      const pn = document.createElement('div');
      pn.className = 'am-playername ' + (TYPE_CLASS[a.type] || '');
      pn.textContent = a.playerName;
      const ty = document.createElement('div');
      ty.className = 'am-type';
      ty.textContent = accountName(a);
      info.append(acc, pn, ty);
      item.append(icon, info);
      item.onclick = () => selectAccount(a.id);
      list.appendChild(item);
    });
  }
  function selectAccount(id) {
    const acc = State.accounts.find((a) => a.id === id);
    if (!acc) return;
    State.selectedAccount = id;
    $('triggerAvatar').textContent = acc.avatar;
    $('triggerName').textContent = acc.playerName;
    closeAccountMenu();
    renderAccountMenu();
    notify('已切换账号：' + acc.playerName, 'success');
  }
  function openAccountMenu() { $('accountDropdown').classList.add('open'); }
  function closeAccountMenu() { $('accountDropdown').classList.remove('open'); }

  /* ============================================================
     组件自定义按钮条（可拖动排序，启动器插件系统可注入）
     ============================================================ */
  const DEFAULT_CUSTOM_BTNS = [
    { id: 'home',      label: '启动',      icon: 'fa-solid fa-play',         action: 'panel:home' },
    { id: 'downloads', label: '下载',      icon: 'fa-solid fa-download',     action: 'panel:downloads' },
    { id: 'settings',  label: '设置',      icon: 'fa-solid fa-gear',         action: 'panel:settings' },
    { id: 'plugins',   label: '插件中心', icon: 'fa-solid fa-plug',         action: 'panel:settings&section=plugins' },
  ];
  function loadCustomBar() {
    try {
      const raw = localStorage.getItem('lxe_custombar');
      if (raw) { const arr = JSON.parse(raw); if (Array.isArray(arr) && arr.length) return arr; }
    } catch (e) {}
    return DEFAULT_CUSTOM_BTNS.slice();
  }
  function saveCustomBar(list) { try { localStorage.setItem('lxe_custombar', JSON.stringify(list)); } catch (e) {} }
  function renderCustomBar() {
    const bar = $('customBar');
    bar.innerHTML = '';
    const items = loadCustomBar();
    items.forEach((btn, idx) => {
      const el = document.createElement('button');
      el.className = 'custom-bar-item';
      el.draggable = true;
      el.dataset.idx = idx;
      el.innerHTML = (btn.icon ? FA.cls(btn.icon) : '') + ' ' + (btn.label || '');
      el.title = btn.label || btn.id || '';
      el.onclick = () => {
        const [kind, target] = (btn.action || '').split(':');
        if (kind === 'panel') {
          const [pname, section] = (target || '').split('&');
          showPanel(pname);
          if (section && section.startsWith('section=')) {
            setTimeout(() => {
              const sec = section.split('=')[1];
              const nav = document.querySelector('.settings-nav-item[data-section="' + sec + '"]');
              if (nav) nav.click();
            }, 20);
          }
        }
        if (kind === 'plugin' && target) {
          hooks.panelAfterShow.forEach((h) => { try { h('__plugin__:' + target, State); } catch (e) {} });
        }
      };
      el.addEventListener('dragstart', (e) => {
        e.dataTransfer.effectAllowed = 'move';
        el.classList.add('dragging');
        e.dataTransfer.setData('text/plain', String(idx));
      });
      el.addEventListener('dragend', () => el.classList.remove('dragging'));
      el.addEventListener('dragover', (e) => { e.preventDefault(); el.classList.add('ghost'); });
      el.addEventListener('dragleave', () => el.classList.remove('ghost'));
      el.addEventListener('drop', (e) => {
        e.preventDefault();
        el.classList.remove('ghost');
        const from = parseInt(e.dataTransfer.getData('text/plain'), 10);
        if (isNaN(from)) return;
        const list = loadCustomBar();
        const [moved] = list.splice(from, 1);
        list.splice(idx, 0, moved);
        saveCustomBar(list);
        renderCustomBar();
      });
      bar.appendChild(el);
    });
    updateCustomBarActive();
  }
  function updateCustomBarActive() {
    $$('.custom-bar-item').forEach((el) => {
      const item = loadCustomBar()[el.dataset.idx];
      if (!item) return;
      const [kind, target] = (item.action || '').split(':');
      if (kind === 'panel') {
        const [pname] = target.split('&');
        el.classList.toggle('active', pname === State.currentPanel);
      }
    });
  }

  /* ============================================================
     版本选择
     ============================================================ */
  function renderVersionList() {
    const list = $('versionList');
    list.innerHTML = '';
    State.versions.forEach((v) => {
      const card = document.createElement('div');
      card.className = 'feature-card';
      const cover = document.createElement('div');
      cover.className = 'feature-card-cover';
      cover.innerHTML = FA.cls(TYPE_ICON[v.type] || 'fa-solid fa-gamepad');
      const title = document.createElement('div');
      title.className = 'feature-card-title';
      title.textContent = v.name;
      const sub = document.createElement('div');
      sub.className = 'feature-card-sub';
      sub.textContent = TYPE_LABEL[v.type] + ' · ' + v.sub;
      const foot = document.createElement('div');
      foot.className = 'feature-card-foot';
      const badge = document.createElement('span');
      badge.className = 'uid-badge';
      badge.textContent = State.currentVersion === v.id ? '已选择' : TYPE_LABEL[v.type];
      foot.appendChild(badge);
      card.append(cover, title, sub, foot);
      card.onclick = () => {
        State.currentVersion = v;
        updateHomeVersion();
        closeVersionOverlay();
        notify('已选择版本：' + v.name, 'success');
      };
      list.appendChild(card);
    });
  }
  function updateHomeVersion() {
    const v = State.currentVersion;
    if (!v) return;
    $('vsIcon').innerHTML = FA.cls(TYPE_ICON[v.type] || 'fa-solid fa-gamepad');
    $('vsName').textContent = v.name;
    $('vsSub').textContent = TYPE_LABEL[v.type] + ' · ' + v.sub;
    $('homeVersionBadge').textContent = v.name;
  }
  function openVersionOverlay() { $('versionOverlay').classList.add('open'); }
  function closeVersionOverlay() { $('versionOverlay').classList.remove('open'); }

  /* ============================================================
     启动设置滑块联动 / 调色盘
     ============================================================ */
  function bindMemSlider(sliderId, valId, stateKey) {
    const s = $(sliderId); const v = $(valId);
    s.addEventListener('input', () => {
      const mb = parseInt(s.value, 10);
      v.textContent = mb + ' MB';
      State[stateKey] = mb;
    });
  }
  let currentHSL = { h: 210, s: 75, l: 46 };
  function hslToRgb(h, s, l) {
    s /= 100; l /= 100;
    const c = (1 - Math.abs(2 * l - 1)) * s;
    const x = c * (1 - Math.abs(((h / 60) % 2) - 1));
    const m = l - c / 2;
    let r = 0, g = 0, b = 0;
    if (h < 60) { r = c; g = x; } else if (h < 120) { r = x; g = c; }
    else if (h < 180) { g = c; b = x; } else if (h < 240) { g = x; b = c; }
    else if (h < 300) { r = x; b = c; } else { r = c; b = x; }
    return {
      r: Math.round((r + m) * 255),
      g: Math.round((g + m) * 255),
      b: Math.round((b + m) * 255),
    };
  }
  function rgbToHsl(r, g, b) {
    r /= 255; g /= 255; b /= 255;
    const max = Math.max(r, g, b), min = Math.min(r, g, b);
    let h = 0, s = 0;
    const l = (max + min) / 2;
    if (max !== min) {
      const d = max - min;
      s = l > 0.5 ? d / (2 - max - min) : d / (max + min);
      switch (max) {
        case r: h = (g - b) / d + (g < b ? 6 : 0); break;
        case g: h = (b - r) / d + 2; break;
        default: h = (r - g) / d + 4; break;
      }
      h /= 6;
    }
    return { h: Math.round(h * 360), s: Math.round(s * 100), l: Math.round(l * 100) };
  }
  function rgbToHex(r, g, b) { return '#' + [r, g, b].map((x) => x.toString(16).padStart(2, '0')).join(''); }
  function hexToRgb(hex) {
    const m = /^#?([a-f\d]{2})([a-f\d]{2})([a-f\d]{2})$/i.exec(hex);
    return m ? { r: parseInt(m[1], 16), g: parseInt(m[2], 16), b: parseInt(m[3], 16) } : { r: 25, g: 118, b: 210 };
  }
  function darken(hex, f) { const { r, g, b } = hexToRgb(hex); return rgbToHex(Math.round(r * f), Math.round(g * f), Math.round(b * f)); }
  function applyCustomPrimary(hex) {
    State.customPrimary = hex;
    const style = document.body.style;
    style.setProperty('--primary-color', hex);
    style.setProperty('--primary-hover', darken(hex, 0.85));
    const { r, g, b } = hexToRgb(hex);
    style.setProperty('--primary-light', 'rgba(' + r + ',' + g + ',' + b + ',0.15)');
    style.setProperty('--scrollbar-thumb', 'rgba(' + r + ',' + g + ',' + b + ',0.3)');
    style.setProperty('--scrollbar-thumb-hover', 'rgba(' + r + ',' + g + ',' + b + ',0.5)');
    $('colorPreview').style.background = hex;
    $('paletteValue').textContent = hex;
    $('paletteCustom').value = hex;
    try { localStorage.setItem('lxe_custom_primary', hex); } catch (e) {}
  }
  function buildSliders() {
    const hslBox = $('hslSliders'); const rgbBox = $('rgbSliders');
    hslBox.innerHTML = ''; rgbBox.innerHTML = '';
    const hslDefs = [
      { key: 'h', label: '色相 H', max: 360 },
      { key: 's', label: '饱和度 S', max: 100 },
      { key: 'l', label: '亮度 L', max: 100 },
    ];
    const rgbDefs = [
      { key: 'r', label: '红 R', max: 255 },
      { key: 'g', label: '绿 G', max: 255 },
      { key: 'b', label: '蓝 B', max: 255 },
    ];
    hslDefs.forEach((def) => {
      const row = document.createElement('div'); row.className = 'slider-row';
      const lab = document.createElement('span'); lab.className = 'slider-label'; lab.textContent = def.label;
      const input = document.createElement('input'); input.type = 'range'; input.className = 'slider';
      input.min = 0; input.max = def.max; input.value = currentHSL[def.key]; input.dataset.hslKey = def.key;
      const val = document.createElement('span'); val.className = 'slider-val'; val.textContent = input.value;
      input.addEventListener('input', () => {
        currentHSL[def.key] = parseInt(input.value, 10); val.textContent = input.value; syncFromHSL();
      });
      row.append(lab, input, val); hslBox.appendChild(row);
    });
    let rgb = hslToRgb(currentHSL.h, currentHSL.s, currentHSL.l);
    rgbDefs.forEach((def) => {
      const row = document.createElement('div'); row.className = 'slider-row';
      const lab = document.createElement('span'); lab.className = 'slider-label'; lab.textContent = def.label;
      const input = document.createElement('input'); input.type = 'range'; input.className = 'slider';
      input.min = 0; input.max = def.max; input.value = rgb[def.key]; input.dataset.rgbKey = def.key;
      const val = document.createElement('span'); val.className = 'slider-val'; val.textContent = input.value;
      input.addEventListener('input', () => {
        rgb[def.key] = parseInt(input.value, 10); val.textContent = input.value;
        const hsl = rgbToHsl(rgb.r, rgb.g, rgb.b);
        currentHSL = hsl; syncSliders();
        applyCustomPrimary(rgbToHex(rgb.r, rgb.g, rgb.b));
      });
      row.append(lab, input, val); rgbBox.appendChild(row);
    });
  }
  function syncSliders() {
    const hsl = currentHSL;
    $$('#hslSliders .slider').forEach((s) => { s.value = hsl[s.dataset.hslKey]; s.nextElementSibling.textContent = s.value; });
    const rgb = hslToRgb(hsl.h, hsl.s, hsl.l);
    $$('#rgbSliders .slider').forEach((s) => { s.value = rgb[s.dataset.rgbKey]; s.nextElementSibling.textContent = s.value; });
  }
  function syncFromHSL() {
    const rgb = hslToRgb(currentHSL.h, currentHSL.s, currentHSL.l);
    applyCustomPrimary(rgbToHex(rgb.r, rgb.g, rgb.b));
    syncSliders();
  }
  function loadSavedPrimary() {
    try {
      const hex = localStorage.getItem('lxe_custom_primary');
      if (hex && /^#[0-9a-f]{6}$/i.test(hex)) {
        applyCustomPrimary(hex);
        const { r, g, b } = hexToRgb(hex);
        currentHSL = rgbToHsl(r, g, b); syncSliders();
        document.querySelectorAll('.palette-swatch').forEach((s) => {
          s.classList.toggle('selected', s.dataset.color.toLowerCase() === hex.toLowerCase());
        });
      }
    } catch (e) {}
  }

  /* ============================================================
     下载中心（游戏版本 / 模组 / 整合包 三分类）
     ============================================================ */
  let currentDownloadTab = 'game';
  const queue = [];
  function renderDownloadList() {
    const list = $('downloadList');
    list.innerHTML = '';
    const items = State.downloads[currentDownloadTab] || [];
    const kw = ($('downloadSearch').value || '').toLowerCase();
    items
      .filter((it) => it.name.toLowerCase().includes(kw) ||
                      ((it.desc || '').toLowerCase().includes(kw)) ||
                      ((it.author || '').toLowerCase().includes(kw)))
      .forEach((it) => {
        const card = document.createElement('div');
        card.className = 'feature-card';
        const cover = document.createElement('div');
        cover.className = 'feature-card-cover';
        cover.innerHTML = FA.cls(it.icon || 'fa-solid fa-folder-open');
        const title = document.createElement('div');
        title.className = 'feature-card-title';
        title.textContent = it.name;
        const sub = document.createElement('div');
        sub.className = 'feature-card-sub';
        sub.textContent = it.desc || '';
        if (it.author) {
          const by = document.createElement('div');
          by.className = 'feature-card-sub';
          by.textContent = '作者：' + it.author;
          card.append(cover, title, sub, by);
        } else {
          card.append(cover, title, sub);
        }
        const foot = document.createElement('div');
        foot.className = 'feature-card-foot';
        const badge = document.createElement('span');
        badge.className = 'uid-badge';
        badge.textContent = ({
          game: '版本', mod: '模组', modpack: '整合包'
        })[currentDownloadTab] || '资源';
        const btn = document.createElement('button');
        btn.className = 'primary-btn';
        btn.innerHTML = FA.cls('fa-solid fa-download') + ' 安装';
        btn.onclick = (e) => { e.stopPropagation(); startDownload(it.name); };
        foot.append(badge, btn);
        card.appendChild(foot);
        list.appendChild(card);
      });
    if (!list.children.length) {
      list.innerHTML = ''
        + '<div class="empty-state" style="padding:40px 0;">'
        +   '<div class="empty-state-icon">' + FA.cls('fa-solid fa-magnifying-glass') + '</div>'
        +   '<div>没有找到匹配的资源</div>'
        + '</div>';
    }
  }
  function startDownload(name) {
    const id = 'dl-' + Date.now();
    queue.push({ id, name, pct: 0 });
    renderQueue();
    if (LX) {
      LX.call('download.simulate', {}).then((r) => {
        notify('已提交下载：' + name, 'info');
      }).catch(() => localSimulate(id));
      if (!startDownload._bound) {
        startDownload._bound = true;
        LX.on('download.progress', (d) => {
          const q = queue.find((x) => x.id === 'dl-backend');
          if (q) { q.pct = d.percent || 0; renderQueue(); }
        });
        LX.on('download.state', (d) => {
          const q = queue.find((x) => x.id === 'dl-backend');
          if (q && d.state === 'done') { q.pct = 100; renderQueue(); notify('下载完成', 'success'); }
        });
        queue.push({ id: 'dl-backend', name: name, pct: 0 });
        renderQueue();
      }
    } else {
      localSimulate(id);
    }
  }
  function localSimulate(id) {
    let pct = 0;
    const timer = setInterval(() => {
      const q = queue.find((x) => x.id === id);
      if (!q) { clearInterval(timer); return; }
      pct = Math.min(100, pct + 10);
      q.pct = pct;
      renderQueue();
      if (pct >= 100) { clearInterval(timer); notify('下载完成：' + q.name, 'success'); }
    }, 300);
  }
  function renderQueue() {
    const box = $('downloadQueue');
    box.innerHTML = '';
    $('queueCount').textContent = queue.length + ' 项';
    queue.forEach((q) => {
      const item = document.createElement('div');
      item.className = 'upload-item';
      const info = document.createElement('div');
      info.className = 'upload-info';
      const name = document.createElement('span');
      name.className = 'upload-name';
      name.textContent = q.name;
      const size = document.createElement('em');
      size.className = 'upload-size';
      size.textContent = (q.pct * 0.24).toFixed(1) + ' MB';
      info.append(name, size);
      const prog = document.createElement('div');
      prog.className = 'upload-progress';
      const bar = document.createElement('div');
      bar.className = 'upload-progress-bar';
      bar.style.width = q.pct + '%';
      prog.appendChild(bar);
      const meta = document.createElement('div');
      meta.className = 'upload-meta';
      const pct = document.createElement('span');
      pct.textContent = q.pct + '%';
      const rm = document.createElement('button');
      rm.className = 'upload-remove';
      rm.innerHTML = FA.cls('fa-solid fa-xmark') + ' 取消';
      rm.onclick = () => {
        const i = queue.findIndex((x) => x.id === q.id);
        if (i >= 0) queue.splice(i, 1);
        renderQueue();
      };
      meta.append(pct, rm);
      item.append(info, prog, meta);
      box.appendChild(item);
    });
  }

  /* ============================================================
     设置：账号 / Java 路径
     ============================================================ */
  function renderSettingsAccounts() {
    const list = $('settingsAccountList');
    list.innerHTML = '';
    State.accounts.forEach((a) => {
      const item = document.createElement('div');
      item.className = 'app-item';
      const icon = document.createElement('div');
      icon.className = 'account-switcher-icon';
      icon.textContent = a.avatar;
      const info = document.createElement('div');
      info.className = 'app-item-info';
      const name = document.createElement('div');
      name.className = 'app-item-name';
      name.textContent = a.playerName;
      const sub = document.createElement('div');
      sub.className = 'app-item-sub';
      sub.textContent = a.accountName + ' · ' + accountName(a);
      info.append(name, sub);
      const actions = document.createElement('div');
      actions.className = 'app-item-actions';
      const use = document.createElement('button');
      use.className = 'icon-btn';
      use.innerHTML = (State.selectedAccount === a.id
        ? (FA.cls('fa-solid fa-check') + ' 当前')
        : (FA.cls('fa-solid fa-rotate') + ' 切换'));
      use.onclick = () => { selectAccount(a.id); renderSettingsAccounts(); };
      const del = document.createElement('button');
      del.className = 'icon-btn';
      del.innerHTML = FA.cls('fa-solid fa-trash') + ' 移除';
      del.style.color = '#ef4444';
      del.onclick = () => confirmBox('移除账号', '确定移除 ' + a.playerName + ' 吗？', () => {
        State.accounts = State.accounts.filter((x) => x.id !== a.id);
        renderSettingsAccounts(); renderAccountMenu();
        notify('已移除账号', 'success');
      });
      actions.append(use, del);
      item.append(icon, info, actions);
      list.appendChild(item);
    });
  }

  function buildJavaOptions() {
    ['自动检测 (推荐)', 'Java 8', 'Java 17', 'Java 21', '自定义路径...'].forEach((name, i) => {
      const opt = document.createElement('div');
      opt.className = 'form-select-option';
      opt.textContent = name;
      opt.onclick = () => {
        if (name === '自定义路径...') notify('请在后端接入文件选择对话框', 'info');
        $('javaPathText').textContent = name;
        $('launchJavaText').textContent = name;
        $$('.form-select-wrapper').forEach((w) => w.classList.remove('open'));
      };
      $('javaPathOptions').appendChild(opt);
      const opt2 = opt.cloneNode(true);
      opt2.onclick = opt.onclick;
      $('launchJavaOptions').appendChild(opt2);
    });
  }

  function bindToggle(sel) {
    document.querySelectorAll(sel).forEach((el) => {
      el.onclick = () => { el.classList.toggle('active'); };
    });
  }

  /* ============================================================
     后端桥接 & 初始化
     ============================================================ */
  function hasActiveDownloads() {
    return queue.some((q) => q.pct != null && q.pct < 100);
  }
  function bindBackend() {
    if (!LX) return;
    $('winMin').onclick = () => LX.call('window.minimize').catch((e) => notify('窗口方法不可用：' + e.message, 'error'));
    $('winMax').onclick = () => LX.call('window.toggleMaximize').catch((e) => notify('窗口方法不可用：' + e.message, 'error'));
    $('winClose').onclick = () => {
      if (hasActiveDownloads()) {
        confirmBox('关闭确认', '当前有下载任务正在进行，确定要关闭启动器吗？', () => LX.call('window.close').catch(() => window.close()));
      } else {
        LX.call('window.close').catch(() => window.close());
      }
    };
    if (LX.on) {
      LX.on('mc.launched', (d) => setGameRunningUI(true, d && d.pids ? d.pids : [d && d.pid].filter(Boolean)));
      LX.on('mc.stopped', () => setGameRunningUI(false, []));
      LX.on('mc.crashed', () => { setGameRunningUI(false, []); notify('游戏崩溃', 'error'); });
      // 拖动/缩放窗口期间：C++ 后端发送 window.dragging 事件，
      // 为 <body> 添加/移除 .dragging 类，CSS 临时移除 backdrop-filter 减负
      LX.on('window.dragging', (d) => {
        if (d && d.dragging) document.body.classList.add('dragging');
        else document.body.classList.remove('dragging');
      });
    }

    // 顶栏 JS 侧拖动（作为 C++ 子类化 hit-test 的兜底；按钮元素不触发）
    $('navbar').addEventListener('mousedown', (e) => {
      const t = e.target;
      if (t.closest('.win-btn') || t.closest('.nav-btn') || t.closest('.custom-bar-item') ||
          t.closest('.account-trigger') || t.closest('.account-menu') || t.closest('input') ||
          t.closest('textarea') || t.closest('button')) return;
      LX.call('window.startDrag').catch(() => {});
    });

    // 边缘缩放（JS 兜底：捕获阶段）
    const RESIZE_BORDER = 6;
    const CURSOR_MAP = {
      'left': 'ew-resize', 'right': 'ew-resize',
      'top': 'ns-resize', 'bottom': 'ns-resize',
      'top-left': 'nwse-resize', 'bottom-right': 'nwse-resize',
      'top-right': 'nesw-resize', 'bottom-left': 'nesw-resize',
    };
    let windowMaximized = false;
    const refreshMaxState = () => LX.call('window.getState').then((s) => {
      windowMaximized = !!s.maximized;
      $('winMax').innerHTML = windowMaximized
        ? FA.cls('fa-regular fa-clone')
        : FA.cls('fa-regular fa-square');
    }).catch(() => {});
    refreshMaxState();
    $('winMax').addEventListener('click', () => setTimeout(refreshMaxState, 50));

    function detectEdge(x, y) {
      const w = window.innerWidth; const h = window.innerHeight;
      const left = x < RESIZE_BORDER;
      const right = x > w - RESIZE_BORDER;
      const top = y < RESIZE_BORDER;
      const bottom = y > h - RESIZE_BORDER;
      if (top && left) return 'top-left';
      if (top && right) return 'top-right';
      if (bottom && left) return 'bottom-left';
      if (bottom && right) return 'bottom-right';
      if (left) return 'left';
      if (right) return 'right';
      if (top) return 'top';
      if (bottom) return 'bottom';
      return null;
    }
    document.addEventListener('mousedown', (e) => {
      if (e.button !== 0 || windowMaximized) return;
      const edge = detectEdge(e.clientX, e.clientY);
      if (edge) {
        e.preventDefault(); e.stopPropagation();
        LX.call('window.startResize', { edge: edge }).catch(() => {});
      }
    }, true);
    document.addEventListener('mousemove', (e) => {
      if (windowMaximized) { document.body.style.cursor = ''; return; }
      const edge = detectEdge(e.clientX, e.clientY);
      document.body.style.cursor = edge ? (CURSOR_MAP[edge] || '') : '';
    });

    // 版本号 + 配置
    const pCfg = LX.call ? LX.call('app.config').catch(() => null) : Promise.resolve(null);
    pCfg.then((cfg) => {
      let ver = cfg && cfg.version;
      if (ver) applyVersionToTitle(ver);
      const sysP = LX.call ? LX.call('system.info').catch(() => null) : Promise.resolve(null);
      sysP.then((info) => {
        if (!info) return;
        $('aboutArch').textContent = (info.arch || '?') + ' / ' + (info.build || '?');
        if (!ver && info.version) applyVersionToTitle(info.version);
      });
    });
  }

  /* ============================================================
     bootstrap
     ============================================================ */
  function detectVersionBranch(name) {
    if (!name) return 'release';
    const n = String(name).toLowerCase();
    if (n.includes('pre') || n.includes('rc') || n.includes('beta')) return 'old_beta';
    if (n.includes('snapshot') || /\d{2}w\d{2}[a-z]/.test(n) || n.includes('-snapshot')) return 'snapshot';
    return 'release';
  }
  function filterByBranch(items, branch) {
    if (!branch || branch === 'all') return items;
    return (items || []).filter((it) => {
      const n = it.name || it.id || it.version || '';
      return detectVersionBranch(n) === branch;
    });
  }
  function loadRemoteVersions(force) {
    if (!LX || !LX.call) return Promise.resolve();
    return LX.call('mc.remoteVersions', { force: !!force }).then((list) => {
      if (Array.isArray(list) && list.length) {
        State.downloads.game = list.map((v) => ({
          name: v.name || v.id || v.version,
          desc: (v.type ? TYPE_LABEL[v.type] || '原版' : '原版') + (v.releaseTime ? ' · ' + v.releaseTime : ''),
          kind: v.kind || v.loader || 'vanilla',
          icon: TYPE_ICON[v.kind || v.loader] || 'fa-solid fa-gamepad',
          branch: detectVersionBranch(v.name || v.id || v.version),
          raw: v,
        }));
        renderDownloadTable();
      }
    }).catch(() => {});
  }
  function setGameRunningUI(running, pids) {
    State.gameRunning = !!running;
    State.gamePids = Array.isArray(pids) ? pids : [];
    const btn = $('launchBtn');
    if (btn) {
      if (State.allowMultiInstance) {
        btn.classList.remove('running');
        btn.innerHTML = FA.cls('fa-solid fa-rocket') + ' <span>启动游戏</span>';
      } else {
        if (running) {
          btn.classList.add('running');
          btn.innerHTML = FA.cls('fa-solid fa-stop') + ' <span>停止游戏</span>';
        } else {
          btn.classList.remove('running');
          btn.innerHTML = FA.cls('fa-solid fa-rocket') + ' <span>启动游戏</span>';
        }
      }
    }
    updateLeftStopButton();
  }
  function updateLeftStopButton() {
    const btn = $('floatingStopBtn');
    if (!btn) return;
    if (State.gameRunning && State.allowMultiInstance) {
      btn.style.display = 'inline-flex';
    } else {
      btn.style.display = 'none';
    }
  }
  function buildConcurrentOptions() {
    const opts = $('concurrentOptions');
    const text = $('concurrentText');
    if (!opts) return;
    opts.innerHTML = '';
    for (let i = 1; i <= 16; i++) {
      const d = document.createElement('div');
      d.className = 'form-select-option';
      d.textContent = i;
      d.onclick = () => {
        State.maxConcurrentDownloads = i;
        text.textContent = i;
        try { localStorage.setItem('lxe_max_concurrent', String(i)); } catch (e) {}
        opts.parentElement.classList.remove('open');
      };
      opts.appendChild(d);
    }
    try {
      const saved = parseInt(localStorage.getItem('lxe_max_concurrent'), 10);
      if (saved >= 1 && saved <= 16) {
        State.maxConcurrentDownloads = saved;
        text.textContent = saved;
      }
    } catch (e) {}
  }
  const CORNER_OPTIONS = [
    { mode: 'none',   label: '无圆角' },
    { mode: 'small',  label: '小圆角' },
    { mode: 'medium', label: '中等圆角（Win11 默认）' },
    { mode: 'large',  label: '大圆角（页面圆角）' },
  ];
  function applyCornerClass(mode) {
    if (!document.body) return;
    document.body.classList.remove('corner-none', 'corner-small', 'corner-medium', 'corner-large');
    document.body.classList.add('corner-' + (mode || 'medium'));
  }
  function buildWindowCornerOptions() {
    const wrap = $('windowCornerWrapper');
    const opts = $('windowCornerOptions');
    const text = $('windowCornerText');
    if (!wrap || !opts || !text) return;
    opts.innerHTML = '';
    CORNER_OPTIONS.forEach((opt) => {
      const d = document.createElement('div');
      d.className = 'form-select-option';
      d.textContent = opt.label;
      d.dataset.mode = opt.mode;
      d.onclick = () => {
        text.textContent = opt.label;
        wrap.classList.remove('open');
        // 立即应用到 body，DWM 档位切换由后端异步执行
        applyCornerClass(opt.mode);
        if (LX && LX.call) {
          LX.call('window.setCorner', { mode: opt.mode })
            .then(() => notify('窗口圆角已切换：' + opt.label, 'success'))
            .catch((e) => notify('切换失败：' + (e.message || e), 'error'));
        }
      };
      opts.appendChild(d);
    });
    // 初始回显：后端 RPC → 失败时显示默认"中等圆角"
    const apply = (m) => {
      const hit = CORNER_OPTIONS.find((o) => o.mode === m);
      if (hit) text.textContent = hit.label;
      applyCornerClass(m || 'medium');
    };
    if (LX && LX.call) {
      LX.call('window.getCorner').then((r) => apply(r && r.mode)).catch(() => apply('medium'));
    } else {
      apply('medium');
    }
  }
  function loadMcFolders() {
    if (LX && LX.call) {
      LX.call('mc.listFolders').then((list) => {
        State.mcFolders = Array.isArray(list) ? list : [];
        if (State.mcFolders.length && !State.activeMcFolder) State.activeMcFolder = State.mcFolders[0].id || State.mcFolders[0].path;
        renderMcFolderList();
        renderMcFolderDropdowns();
      }).catch(() => {});
    }
  }
  function renderMcFolderList() {
    const containers = [$('mcFolderList'), $('settingsMcFolderList')].filter(Boolean);
    containers.forEach((list) => {
      list.innerHTML = '';
      if (!State.mcFolders.length) {
        list.innerHTML = '<div class="empty-state">尚未导入 MC 文件夹</div>';
        return;
      }
      State.mcFolders.forEach((f) => {
        const item = document.createElement('div');
        item.className = 'mc-folder-item' + (State.activeMcFolder === (f.id || f.path) ? ' active' : '');
        const icon = document.createElement('div');
        icon.className = 'mcfi-icon';
        icon.innerHTML = FA.cls('fa-solid fa-folder-open');
        const info = document.createElement('div');
        info.className = 'mcfi-info';
        const name = document.createElement('div');
        name.className = 'mcfi-name';
        name.textContent = f.name || f.label || 'MC 文件夹';
        const path = document.createElement('div');
        path.className = 'mcfi-path';
        path.textContent = f.path || '';
        info.append(name, path);
        item.append(icon, info);
        item.onclick = () => {
          State.activeMcFolder = f.id || f.path;
          renderMcFolderList();
          renderMcFolderDropdowns();
        };
        list.appendChild(item);
      });
    });
  }
  function renderMcFolderDropdowns() {
    const dropdowns = [
      { wrapper: 'homeMcFolderWrapper', text: 'homeMcFolderText', options: 'homeMcFolderOptions' },
      { wrapper: 'downloadsMcFolderWrapper', text: 'downloadsMcFolderText', options: 'downloadsMcFolderOptions' },
      { wrapper: 'mcFolderWrapper', text: 'mcFolderText', options: 'mcFolderOptions' },
    ];
    dropdowns.forEach((cfg) => {
      const wrap = $(cfg.wrapper); const txt = $(cfg.text); const opts = $(cfg.options);
      if (!wrap || !txt || !opts) return;
      const active = State.mcFolders.find((f) => (f.id || f.path) === State.activeMcFolder);
      txt.innerHTML = FA.cls('fa-solid fa-folder-open') + (active ? ' ' + (active.name || active.label) : ' 默认');
      opts.innerHTML = '';
      State.mcFolders.forEach((f) => {
        const d = document.createElement('div');
        d.className = 'form-select-option' + ((f.id || f.path) === State.activeMcFolder ? ' selected' : '');
        d.textContent = f.name || f.label || f.path;
        d.onclick = () => {
          State.activeMcFolder = f.id || f.path;
          renderMcFolderList();
          renderMcFolderDropdowns();
          wrap.classList.remove('open');
        };
        opts.appendChild(d);
      });
    });
  }
  function refreshInstalledStatus() {
    renderDownloadTable();
  }
  function _applyInstallButtonState(btn, it, installed) {
    if (installed) {
      btn.className = 'cancel-btn';
      btn.innerHTML = FA.cls('fa-solid fa-trash') + ' 卸载';
      btn.onclick = (e) => { e.stopPropagation(); confirmBox('卸载版本', '确定卸载 ' + it.name + ' 吗？', () => notify('已提交卸载请求', 'info')); };
    } else {
      btn.className = 'primary-btn';
      btn.innerHTML = FA.cls('fa-solid fa-download') + ' 安装';
      btn.onclick = (e) => { e.stopPropagation(); startDownload(it.name); };
    }
  }
  function renderDownloadTable() {
    const list = $('downloadList');
    if (!list) return;
    list.innerHTML = '';
    const items = filterByBranch(State.downloads[currentDownloadTab] || [], State.currentVersionBranch);
    const kw = ($('downloadSearch').value || '').toLowerCase();
    items
      .filter((it) => it.name.toLowerCase().includes(kw) ||
                      ((it.desc || '').toLowerCase().includes(kw)) ||
                      ((it.author || '').toLowerCase().includes(kw)))
      .forEach((it) => {
        const card = document.createElement('div');
        card.className = 'feature-card';
        const cover = document.createElement('div');
        cover.className = 'feature-card-cover';
        cover.innerHTML = FA.cls(it.icon || 'fa-solid fa-folder-open');
        const title = document.createElement('div');
        title.className = 'feature-card-title';
        title.textContent = it.name;
        const sub = document.createElement('div');
        sub.className = 'feature-card-sub';
        sub.textContent = it.desc || '';
        if (it.author) {
          const by = document.createElement('div');
          by.className = 'feature-card-sub';
          by.textContent = '作者：' + it.author;
          card.append(cover, title, sub, by);
        } else {
          card.append(cover, title, sub);
        }
        const foot = document.createElement('div');
        foot.className = 'feature-card-foot';
        const badge = document.createElement('span');
        badge.className = 'uid-badge';
        badge.textContent = ({
          game: '版本', mod: '模组', modpack: '整合包'
        })[currentDownloadTab] || '资源';
        const btn = document.createElement('button');
        _applyInstallButtonState(btn, it, false);
        if (currentDownloadTab === 'game') {
          const mlBtn = document.createElement('button');
          mlBtn.className = 'icon-btn';
          mlBtn.title = '加载器';
          mlBtn.innerHTML = FA.cls('fa-solid fa-layer-group');
          mlBtn.onclick = (e) => { e.stopPropagation(); openModloaderOverlay(it); };
          foot.append(badge, mlBtn, btn);
        } else {
          foot.append(badge, btn);
        }
        card.appendChild(foot);
        list.appendChild(card);
      });
    if (!list.children.length) {
      list.innerHTML = ''
        + '<div class="empty-state" style="padding:40px 0;">'
        +   '<div class="empty-state-icon">' + FA.cls('fa-solid fa-magnifying-glass') + '</div>'
        +   '<div>没有找到匹配的资源</div>'
        + '</div>';
    }
  }

  /* ============================================================
     BMCLAPI：tab 切换 + Java/加载器 表格渲染
     ============================================================ */
  // tab 容器切换：卡片式(downloadList) vs 表格式(dlTableWrap)，版本分支栏仅游戏版本可见
  function switchDownloadTab(tab) {
    const cardTabs = ['game', 'mod', 'modpack'];
    const tableTabs = ['java', 'loaders'];
    const cards = $('downloadList');
    const table = $('dlTableWrap');
    const branchTabs = $('versionBranchTabs');
    if (!cards || !table) return;
    if (cardTabs.indexOf(tab) >= 0) {
      cards.style.display = 'grid';
      table.style.display = 'none';
    } else if (tableTabs.indexOf(tab) >= 0) {
      cards.style.display = 'none';
      table.style.display = 'block';
    }
    if (branchTabs) branchTabs.style.display = (tab === 'game') ? 'flex' : 'none';
    // 加载对应数据
    if (tab === 'java') {
      if (!State.bmcl.javaList || !State.bmcl.javaList.length) {
        showDlLoading(true);
        BMCLAPI.javaList(false).then(() => { showDlLoading(false); renderBmclJavaTable(); });
      } else {
        renderBmclJavaTable();
      }
    } else if (tab === 'loaders') {
      renderBmclLoaderTable();
      // 默认拉取 forge 的当前 loaderMcVersion 列表
      preloadCurrentLoaderData();
    } else {
      showDlLoading(false);
      renderDownloadTable();
    }
    // 更新源标签
    updateSourceTag();
  }
  function updateSourceTag() {
    const el = $('dlSourceTag');
    if (el) el.textContent = '源: ' + (State.bmcl.mirrorSource === 'bmclapi' ? 'BMCLAPI (国内)' : '官方');
  }
  function showDlLoading(show) {
    const wrap = $('dlTableWrap');
    if (show) {
      if (wrap && wrap.style.display === 'block') {
        const tb = wrap.querySelector('tbody');
        const th = wrap.querySelector('thead');
        if (tb) tb.innerHTML = '';
        if (th) th.innerHTML = '';
        let cell = $('dlLoadingCell');
        if (cell) cell.remove();
        cell = document.createElement('div');
        cell.className = 'dl-loading-cell';
        cell.id = 'dlLoadingCell';
        cell.innerHTML = '<div class="dl-loading-box">' + FA.cls('fa-solid fa-circle-notch fa-spin') + ' <span id="dlLoadingText">正在从 BMCLAPI 加载…</span></div>';
        wrap.parentNode.insertBefore(cell, wrap.nextSibling);
      }
    } else {
      const c = $('dlLoadingCell');
      if (c) c.remove();
    }
  }

  // Java 表格渲染
  function renderBmclJavaTable() {
    const table = $('dlTable');
    const thead = table && table.querySelector('thead');
    const tbody = $('dlTableBody');
    if (!tbody) return;
    thead.innerHTML = '';
    tbody.innerHTML = '';
    const trHead = document.createElement('tr');
    ['名称', '厂商 · 版本', '平台 · 架构', '大小', '操作'].forEach((t) => {
      const th = document.createElement('th'); th.textContent = t; trHead.appendChild(th);
    });
    thead.appendChild(trHead);
    const kw = ($('downloadSearch') && $('downloadSearch').value || '').toLowerCase();
    const list = (State.downloads.java || []).filter((j) => {
      if (!kw) return true;
      return (j.name || '').toLowerCase().includes(kw) ||
             (j.desc || '').toLowerCase().includes(kw) ||
             ((j.author || '').toLowerCase().includes(kw));
    });
    if (!list.length) {
      const tr = document.createElement('tr');
      const td = document.createElement('td'); td.colSpan = 5;
      td.innerHTML = '<div class="empty-state" style="padding:30px 0;">'
        + '<div class="empty-state-icon">' + FA.cls('fa-brands fa-java') + '</div>'
        + '<div>' + (State.bmcl.javaLoading ? '加载中…' : '没有匹配的 Java 运行时') + '</div></div>';
      tr.appendChild(td); tbody.appendChild(tr);
      return;
    }
    list.forEach((j) => {
      const tr = document.createElement('tr');
      const td1 = document.createElement('td');
      td1.innerHTML = '<div class="dl-name-cell"><div class="dl-icon">' + FA.cls(j.icon || 'fa-brands fa-java') + '</div>'
        + '<div><div class="dl-name-text">' + escapeHtml(j.name) + '</div>'
        + '<div class="dl-desc-text">' + escapeHtml(j.desc || '') + '</div></div></div>';
      const td2 = document.createElement('td');
      td2.textContent = (j.raw && j.raw.vendor ? j.raw.vendor + ' · ' : '') + (j.raw && j.raw.version ? j.raw.version : '');
      const td3 = document.createElement('td');
      td3.textContent = (j.raw && j.raw.os ? j.raw.os : '') + ' · ' + (j.raw && j.raw.arch ? j.raw.arch : '');
      const td4 = document.createElement('td');
      td4.textContent = humanSize(j.size || (j.raw && j.raw.size));
      const td5 = document.createElement('td');
      const btn = document.createElement('button');
      btn.className = 'primary-btn';
      btn.style.padding = '6px 12px';
      btn.style.fontSize = '12px';
      btn.innerHTML = FA.cls('fa-solid fa-download') + ' 下载';
      btn.onclick = (e) => { e.stopPropagation(); submitBmclDownload('java', j); };
      td5.appendChild(btn);
      tr.append(td1, td2, td3, td4, td5);
      tbody.appendChild(tr);
    });
  }

  // 加载器（Forge / NeoForge / OptiFine）子标签 + 表格渲染
  function renderBmclLoaderTable() {
    const wrap = $('dlTableWrap');
    if (!wrap) return;
    // 在 wrap 前面插入子标签和筛选器（首次插入，之后更新选择）
    let subtabs = $('bmclLoaderSubtabs');
    if (!subtabs) {
      subtabs = document.createElement('div');
      subtabs.className = 'bmcl-subtabs';
      subtabs.id = 'bmclLoaderSubtabs';
      [
        { k: 'forge',     n: 'Forge',     i: 'fa-solid fa-fire' },
        { k: 'neoforge',  n: 'NeoForge',  i: 'fa-solid fa-feather-pointed' },
        { k: 'optifine',  n: 'OptiFine',  i: 'fa-solid fa-candle' },
      ].forEach((s) => {
        const btn = document.createElement('button');
        btn.className = 'bmcl-subtab' + (State.bmcl.loaderSubtab === s.k ? ' active' : '');
        btn.dataset.loader = s.k;
        btn.innerHTML = FA.cls(s.i) + ' ' + s.n;
        btn.onclick = () => {
          State.bmcl.loaderSubtab = s.k;
          document.querySelectorAll('#bmclLoaderSubtabs .bmcl-subtab').forEach((x) => x.classList.toggle('active', x.dataset.loader === s.k));
          preloadCurrentLoaderData();
          renderBmclLoaderTableContent();
        };
        subtabs.appendChild(btn);
      });
      // MC 版本下拉选择
      const mcvWrap = document.createElement('div');
      mcvWrap.style.cssText = 'display:inline-flex;align-items:center;gap:8px;margin-left:8px;';
      mcvWrap.innerHTML = '<span style="font-size:12px;color:var(--text-secondary);">MC 版本:</span>';
      const sel = document.createElement('select');
      sel.id = 'bmclLoaderMcSelect';
      sel.className = 'form-input';
      sel.style.cssText = 'width:auto;padding:5px 10px;font-size:12px;min-width:110px;';
      sel.onchange = () => {
        State.bmcl.loaderMcVersion = sel.value;
        preloadCurrentLoaderData();
        renderBmclLoaderTableContent();
      };
      mcvWrap.appendChild(sel);
      subtabs.appendChild(mcvWrap);
      wrap.parentNode.insertBefore(subtabs, wrap);
    }
    // 填充 MC 版本下拉框（优先取 forgeMcVersions）
    const sel = $('bmclLoaderMcSelect');
    if (sel && !sel.options.length) {
      const defaults = ['1.21.4', '1.21.3', '1.21.1', '1.21', '1.20.6', '1.20.4', '1.20.1', '1.19.4', '1.18.2', '1.17.1', '1.16.5'];
      defaults.forEach((v) => {
        const o = document.createElement('option'); o.value = v; o.textContent = v;
        if (v === State.bmcl.loaderMcVersion) o.selected = true;
        sel.appendChild(o);
      });
      // 异步用 BMCLAPI 的真实列表替换
      BMCLAPI.forgeMcVersions(false).then((arr) => {
        if (Array.isArray(arr) && arr.length) {
          const cur = State.bmcl.loaderMcVersion;
          sel.innerHTML = '';
          arr.forEach((v) => {
            const o = document.createElement('option');
            const val = (typeof v === 'object' ? (v.version || v.id || v) : v);
            o.value = val; o.textContent = val;
            if (val === cur) o.selected = true;
            sel.appendChild(o);
          });
        }
      });
    }
    renderBmclLoaderTableContent();
  }

  // 预取当前选择的加载器 + MC 版本数据
  function preloadCurrentLoaderData() {
    const k = State.bmcl.loaderSubtab;
    const mc = State.bmcl.loaderMcVersion;
    const text = $('dlLoadingText');
    if (k === 'forge') {
      if (text) text.textContent = '正在加载 Forge 版本列表…';
      showDlLoading(true);
      BMCLAPI.forgeByMc(mc).then(() => { showDlLoading(false); renderBmclLoaderTableContent(); });
    } else if (k === 'neoforge') {
      if (text) text.textContent = '正在加载 NeoForge 版本列表…';
      showDlLoading(true);
      BMCLAPI.neoforgeByMc(mc).then(() => { showDlLoading(false); renderBmclLoaderTableContent(); });
    } else if (k === 'optifine') {
      if (text) text.textContent = '正在加载 OptiFine 列表…';
      showDlLoading(true);
      BMCLAPI.optifineAll(false).then(() => { showDlLoading(false); renderBmclLoaderTableContent(); });
    }
  }

  function renderBmclLoaderTableContent() {
    const table = $('dlTable');
    const thead = table && table.querySelector('thead');
    const tbody = $('dlTableBody');
    if (!tbody) return;
    thead.innerHTML = ''; tbody.innerHTML = '';
    const k = State.bmcl.loaderSubtab;
    const mc = State.bmcl.loaderMcVersion;
    const kw = ($('downloadSearch') && $('downloadSearch').value || '').toLowerCase();
    const trHead = document.createElement('tr');
    const cols = k === 'optifine'
      ? ['版本 (MC/Type/Patch)', '文件名', '大小', '操作']
      : ['Build / 版本', '修改时间', '标签', '操作'];
    cols.forEach((c) => { const th = document.createElement('th'); th.textContent = c; trHead.appendChild(th); });
    thead.appendChild(trHead);
    let rows = [];
    if (k === 'forge') {
      const list = State.bmcl.forgeCache.get(mc) || [];
      rows = list.map((f) => {
        const build = f.build != null ? f.build : (f.version || f);
        const time = f.modified || f.time || '';
        const tag = (f.latest ? '最新 ' : '') + (f.recommended ? '推荐' : '');
        return [
          `<b>${escapeHtml(String(build))}</b>`,
          escapeHtml(typeof time === 'string' ? time : (time ? new Date(time).toLocaleString() : '')),
          tag ? `<span class="tag ${f.recommended ? 'tag-release' : 'tag-latest'}">${escapeHtml(tag.trim())}</span>` : '<span class="dl-type-tag">常规</span>',
          makeLoaderActionBtn('forge', { mcVersion: mc, build: build }),
        ];
      });
    } else if (k === 'neoforge') {
      const list = State.bmcl.neoforgeCache.get(mc) || [];
      rows = list.map((n, idx) => {
        const ver = n.version || n;
        const isLatest = idx === 0;
        return [
          `<b>${escapeHtml(String(ver))}</b>`,
          '',
          isLatest ? `<span class="tag tag-latest">最新版</span>` : '<span class="dl-type-tag">常规</span>',
          makeLoaderActionBtn('neoforge', { version: ver }),
        ];
      });
    } else if (k === 'optifine') {
      const all = State.bmcl.optifineList || [];
      const filtered = all.filter((o) => {
        if (o.mcversion && String(o.mcversion) !== String(mc)) return false;
        if (kw) {
          const blob = [o.mcversion, o.type, o.patch, o.filename, o.sha256].join(' ').toLowerCase();
          if (!blob.includes(kw)) return false;
        }
        return true;
      });
      rows = filtered.slice(0, 200).map((o) => [
        `<b>${escapeHtml(String(o.mcversion || ''))}</b> · ${escapeHtml(String(o.type || ''))} · <span style="color:var(--text-secondary)">Patch ${escapeHtml(String(o.patch || ''))}</span>`,
        `<code>${escapeHtml(String(o.filename || ''))}</code>`,
        humanSize(o.size),
        makeOptifineInstallBtn(o),
      ]);
      if (!kw && State.bmcl.optifineList && !filtered.length) {
        // 当前 MC 版本没有 optifine：fallback 用全量前 50 条
        rows = (State.bmcl.optifineList || []).slice(0, 50).map((o) => [
          `<b>${escapeHtml(String(o.mcversion || ''))}</b> · ${escapeHtml(String(o.type || ''))} · <span style="color:var(--text-secondary)">Patch ${escapeHtml(String(o.patch || ''))}</span>`,
          `<code>${escapeHtml(String(o.filename || ''))}</code>`,
          humanSize(o.size),
          makeOptifineInstallBtn(o),
        ]);
      }
    }
    // 关键词过滤
    if (kw) {
      rows = rows.filter((r) => r.slice(0, 3).join(' ').toLowerCase().includes(kw));
    }
    if (!rows.length) {
      const tr = document.createElement('tr');
      const td = document.createElement('td'); td.colSpan = cols.length;
      td.innerHTML = '<div class="empty-state" style="padding:30px 0;">'
        + '<div class="empty-state-icon">' + FA.cls('fa-solid fa-layer-group') + '</div>'
        + '<div>当前筛选条件下没有数据（尝试切换 MC 版本或清空关键词）</div></div>';
      tr.appendChild(td); tbody.appendChild(tr);
      return;
    }
    rows.forEach((cells) => {
      const tr = document.createElement('tr');
      cells.forEach((c, i) => {
        const td = document.createElement('td');
        if (i === cells.length - 1 && typeof c !== 'string') td.appendChild(c);
        else td.innerHTML = c;
        tr.appendChild(td);
      });
      tbody.appendChild(tr);
    });
  }

  function makeLoaderActionBtn(kind, data) {
    const btn = document.createElement('button');
    btn.className = 'primary-btn';
    btn.style.padding = '6px 12px';
    btn.style.fontSize = '12px';
    btn.innerHTML = FA.cls('fa-solid fa-download') + ' 下载';
    btn.onclick = (e) => { e.stopPropagation(); submitBmclDownload(kind, data); };
    return btn;
  }

  // 通用 HTML 转义
  function escapeHtml(s) {
    if (s == null) return '';
    return String(s).replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
  }

  /* ---------- 提交 BMCLAPI 下载到后端 ---------- */
  function submitBmclDownload(kind, data) {
    let taskName = '';
    let url = '';
    if (kind === 'java') {
      taskName = 'Java · ' + (data.name || data.id || 'runtime');
      url = data.url || (data.raw && (data.raw.url || (data.raw.download && data.raw.download.url)));
      // 没 url 则走后端 java.download(id) RPC
      if (!url) {
        downloadJava(data.id || data.name, true);
        return;
      }
    } else if (kind === 'forge') {
      taskName = 'Forge · MC ' + data.mcVersion + ' Build ' + data.build;
      url = BMCLAPI.forgeDownloadUrl(data.build);
    } else if (kind === 'neoforge') {
      taskName = 'NeoForge · ' + data.version;
      url = BMCLAPI.neoforgeDownloadUrl(data.version, 'installer');
    } else if (kind === 'optifine') {
      taskName = 'OptiFine · ' + data.mcversion + ' ' + data.type + ' ' + data.patch;
      url = BMCLAPI.optifineDownloadUrl(data.mcversion, data.type, data.patch);
    }
    // 先加入本地前端队列占位，再提交后端
    const id = 'dl-bmcl-' + Date.now();
    queue.push({ id, name: taskName, pct: 0 });
    renderQueue();
    if (LX && LX.call) {
      // mc.submitDownloadList: [{ name, url, path? }]
      const item = { name: taskName, url: url, category: kind };
      LX.call('mc.submitDownloadList', { list: [item], folder: State.activeMcFolder })
        .then(() => notify('已提交下载：' + taskName, 'info'))
        .catch((e) => {
          // 如果后端没有 submitDownloadList RPC，回退到 download.simulate + 本地模拟
          if (e && /RPC|not.*found|not support|未知|invalid/i.test(String(e.message || e))) {
            localSimulate(id);
            notify('后端未接入提交接口，使用前端模拟：' + taskName, 'info');
          } else {
            notify('提交下载失败：' + (e.message || e), 'error');
            // 移除占位
            const i = queue.findIndex((q) => q.id === id);
            if (i >= 0) queue.splice(i, 1);
            renderQueue();
          }
        });
    } else {
      localSimulate(id);
    }
  }

  /* ---------- OptiFine 安装：mc.installLoader（下载安装器 → 运行 → 合并 JSON） ---------- */
  // 后端在安装过程中通过 download.progress / download.state 上报 taskId，据此回填队列进度
  const installTasks = new Map(); // taskId -> queue item id
  function bindInstallTaskEvents() {
    if (bindInstallTaskEvents._bound || !LX) return;
    bindInstallTaskEvents._bound = true;
    LX.on('download.progress', (d) => {
      if (!d || d.taskId == null) return;
      const qid = installTasks.get(String(d.taskId));
      if (!qid) return;
      const q = queue.find((x) => x.id === qid);
      if (q) { q.pct = Math.max(0, Math.min(100, Math.round(d.percent || 0))); q.stage = d.stage || ''; renderQueue(); }
    });
    LX.on('download.state', (d) => {
      if (!d || d.taskId == null) return;
      const qid = installTasks.get(String(d.taskId));
      if (!qid) return;
      installTasks.delete(String(d.taskId));
      const q = queue.find((x) => x.id === qid);
      if (!q) return;
      if (d.state === 'done') { q.pct = 100; renderQueue(); notify('OptiFine 安装完成', 'success'); }
      else if (d.state === 'error' || d.state === 'cancelled') { q.pct = -1; renderQueue(); notify('OptiFine 安装失败：' + (d.stage || d.state), 'error'); }
    });
  }
  function installOptifine(o) {
    bindInstallTaskEvents();
    const installName = String(o.mcversion) + '-OptiFine_' + String(o.type) + '_' + String(o.patch);
    const loaderVersion = String(o.type) + '_' + String(o.patch);
    const name = 'OptiFine · ' + String(o.mcversion) + ' ' + String(o.type) + ' ' + String(o.patch);
    const qid = 'dl-of-' + Date.now();
    queue.push({ id: qid, name: name, pct: 0 });
    renderQueue();
    if (LX && LX.call) {
      LX.call('mc.installLoader', {
        loaderId: 'optifine',
        mcVersion: String(o.mcversion),
        loaderVersion: loaderVersion,
        installName: installName,
        loaderData: { type: String(o.type), patch: String(o.patch), filename: String(o.filename || '') },
        complete: true,
      }).then((r) => {
        if (r && r.taskId != null) installTasks.set(String(r.taskId), qid);
        notify('已提交 OptiFine 安装', 'info');
      }).catch((e) => notify('OptiFine 安装提交失败：' + (e.message || e), 'error'));
    } else {
      localSimulate(qid);
      notify('后端未接入安装接口，使用前端模拟', 'info');
    }
  }
  function makeOptifineInstallBtn(o) {
    const btn = document.createElement('button');
    btn.className = 'primary-btn';
    btn.style.padding = '6px 12px';
    btn.style.fontSize = '12px';
    btn.innerHTML = FA.cls('fa-solid fa-download') + ' 安装';
    btn.onclick = (e) => { e.stopPropagation(); installOptifine(o); };
    return btn;
  }

  let _modloaderCtx = null;
  let _selectedLoader = null;
  function openModloaderOverlay(versionItem) {
    _modloaderCtx = versionItem || null;
    _selectedLoader = null;
    const overlay = $('modloaderOverlay');
    if (overlay) overlay.classList.add('open');
    const list = $('modloaderList');
    if (list) {
      list.innerHTML = '';
      const loaders = [
        { id: 'forge', name: 'Forge', desc: '老牌模组加载器', icon: 'fa-solid fa-fire' },
        { id: 'fabric', name: 'Fabric', desc: '轻量级模组加载器', icon: 'fa-solid fa-layer-group' },
        { id: 'neoforge', name: 'NeoForge', desc: 'Forge 分支 · 高版本', icon: 'fa-solid fa-feather-pointed' },
        { id: 'quilt', name: 'Quilt', desc: 'Fabric 分支', icon: 'fa-solid fa-feather' },
      ];
      loaders.forEach((l) => {
        const card = document.createElement('div');
        card.className = 'installer-card' + (_selectedLoader === l.id ? ' selected' : '');
        card.onclick = () => setCardSelected(l.id);
        const icon = document.createElement('div');
        icon.className = 'installer-icon';
        icon.innerHTML = FA.cls(l.icon);
        const info = document.createElement('div');
        info.className = 'installer-info';
        const nm = document.createElement('div');
        nm.className = 'installer-name';
        nm.textContent = l.name;
        const ds = document.createElement('div');
        ds.className = 'installer-desc';
        ds.textContent = l.desc;
        info.append(nm, ds);
        card.append(icon, info);
        list.appendChild(card);
      });
    }
  }
  function setCardSelected(id) {
    _selectedLoader = id;
    const overlay = $('modloaderOverlay');
    if (overlay) {
      overlay.querySelectorAll('.installer-card').forEach((c, idx) => {
        const loaders = ['forge', 'fabric', 'neoforge', 'quilt'];
        c.classList.toggle('selected', loaders[idx] === id);
      });
    }
    const next = $('openLoaderBtn');
    if (next) next.classList.toggle('disabled', !id);
  }
  function openLoaderVersionOverlay() {
    if (!_selectedLoader) { notify('请先选择加载器', 'error'); return; }
    const overlay1 = $('modloaderOverlay');
    const overlay2 = $('loaderVersionOverlay');
    if (overlay1) overlay1.classList.remove('open');
    if (overlay2) overlay2.classList.add('open');
  }
  function downloadJava(javaId, install) {
    if (!LX || !LX.call) return;
    if (install) {
      LX.call('java.download', { id: javaId }).then(() => notify('已提交 Java 下载', 'info')).catch((e) => notify('Java 下载失败：' + e.message, 'error'));
    } else {
      LX.call('java.uninstall', { id: javaId }).then(() => notify('已提交 Java 卸载', 'info')).catch((e) => notify('Java 卸载失败：' + e.message, 'error'));
    }
  }
  let cbOrder = ['launcher', 'downloads', 'settings'];
  let cbOverflow = [];
  function renderCustomBar() {
    const bar = $('customBar');
    if (!bar) return;
    const labels = { launcher: '启动台', downloads: '下载中心', settings: '设置中心' };
    const icons = { launcher: 'fa-solid fa-rocket', downloads: 'fa-solid fa-download', settings: 'fa-solid fa-gear' };
    const panelMap = { launcher: 'home', downloads: 'downloads', settings: 'settings' };
    bar.innerHTML = '';
    cbOrder.forEach((key) => {
      const btn = document.createElement('button');
      btn.className = 'custom-bar-item' + (State.currentPanel === panelMap[key] ? ' active' : '');
      btn.dataset.cbKey = key;
      btn.innerHTML = FA.cls(icons[key]) + ' <span>' + labels[key] + '</span>';
      btn.onclick = () => showPanel(panelMap[key]);
      bar.appendChild(btn);
    });
    const ov = document.createElement('div');
    ov.className = 'custom-bar-overflow';
    ov.id = 'customBarOverflow';
    const ovBtn = document.createElement('button');
    ovBtn.className = 'custom-bar-overflow-btn';
    ovBtn.innerHTML = FA.cls('fa-solid fa-ellipsis-h');
    ovBtn.onclick = () => ov.classList.toggle('open');
    const menu = document.createElement('div');
    menu.className = 'custom-bar-overflow-menu';
    menu.id = 'customBarOverflowMenu';
    ov.append(ovBtn, menu);
    bar.appendChild(ov);
    setTimeout(updateCustomBarOverflow, 0);
  }
  function updateCustomBarOverflow() {
    const bar = $('customBar'); const ov = $('customBarOverflow'); const menu = $('customBarOverflowMenu');
    if (!bar || !ov || !menu) return;
    menu.innerHTML = '';
    const labels = { launcher: '启动台', downloads: '下载中心', settings: '设置中心' };
    const icons = { launcher: 'fa-solid fa-rocket', downloads: 'fa-solid fa-download', settings: 'fa-solid fa-gear' };
    const panelMap = { launcher: 'home', downloads: 'downloads', settings: 'settings' };
    const items = bar.querySelectorAll('.custom-bar-item');
    let hideIdx = -1;
    if (items.length && bar.scrollWidth > bar.clientWidth + 2) {
      ov.classList.add('visible');
      const totalW = bar.clientWidth - ov.offsetWidth - 12;
      let acc = 0;
      items.forEach((it, idx) => {
        acc += it.offsetWidth + 4;
        if (hideIdx < 0 && acc > totalW) hideIdx = idx;
      });
      if (hideIdx < 0) hideIdx = items.length - 1;
    } else {
      ov.classList.remove('visible');
    }
    items.forEach((it, idx) => {
      if (hideIdx >= 0 && idx >= hideIdx) {
        it.style.display = 'none';
        const key = cbOrder[idx]; if (!key) return;
        const mBtn = document.createElement('button');
        mBtn.className = 'custom-bar-item' + (State.currentPanel === panelMap[key] ? ' active' : '');
        mBtn.innerHTML = FA.cls(icons[key]) + ' <span>' + labels[key] + '</span>';
        mBtn.onclick = () => { ov.classList.remove('open'); showPanel(panelMap[key]); };
        menu.appendChild(mBtn);
      } else {
        it.style.display = '';
      }
    });
  }
  function loadLocalVersions() {
    if (!LX || !LX.call) return;
    LX.call('mc.localVersions').then((list) => {
      if (Array.isArray(list)) {
        State.versions = list.map((v) => ({
          id: v.id || v.name,
          type: v.loader || v.type || 'vanilla',
          name: v.name || v.id,
          sub: v.loader ? (TYPE_LABEL[v.loader] || '') + ' · 本地' : '原版 · 本地',
          raw: v,
        }));
        renderVersionList();
      }
    }).catch(() => {});
  }
  function init() {
    loadSavedTheme();
    loadSavedPrimary();
    buildSliders();
    buildJavaOptions();
    buildConcurrentOptions();
    buildWindowCornerOptions(); // 窗口圆角下拉框（4档）
    renderCustomBar();
    renderAccountMenu();
    renderVersionList();
    renderDownloadTable();
    renderSettingsAccounts();
    renderPluginsList();
    bindBackend();
    bindMemSlider('memSlider', 'memVal', 'mem');
    bindMemSlider('launchMemSlider', 'launchMemVal', 'launchMem');
    bindToggle('.toggle-switch');
    try {
      const mi = localStorage.getItem('lxe_allow_multi_instance');
      State.allowMultiInstance = mi === '1';
      const t = $('multiInstanceToggle');
      if (t && State.allowMultiInstance) t.classList.add('active');
    } catch (e) {}
    loadMcFolders();
    loadLocalVersions();
    loadRemoteVersions(false);

    // 默认版本号（覆盖后端失败时）
    applyVersionToTitle(State.version);

    const origShowPanel = window.LX && LX.showPanel ? LX.showPanel : null;
    const _showPanel = (p) => {
      State.currentPanel = p;
      document.querySelectorAll('.nav-btn').forEach((b) => b.classList.toggle('active', b.dataset.nav === p));
      document.querySelectorAll('.content-panel').forEach((pp) => pp.classList.remove('active'));
      const panel = document.querySelector('.content-panel[data-panel="' + p + '"]');
      if (panel) {
        document.body.classList.add('animating');
        panel.classList.add('active');
        setTimeout(() => document.body.classList.remove('animating'), 320);
      }
      renderCustomBar();
    };
    window.showPanel = _showPanel;

    // 动画期间临时隐藏滚动条：入场动画（fade-in/fade-slide-in/grow-from-down）或
    // 滚动容器自身的过渡（浮层/下拉/队列展开等）播放时给 <body> 加 .animating，动画结束后移除
    const _scrollCtrSel = '.content-panel,.settings-sections,.overlay-content,.overlay-box,.dl-table-wrap,.queue-items-wrap,.ndp-list,.log-viewer,.form-select-options,.download-queue';
    const _entranceAnims = ['fade-in', 'fade-slide-in', 'grow-from-down'];
    let _scrollbarTimer = null;
    function _hideScrollbarDuringAnim() {
      document.body.classList.add('animating');
      clearTimeout(_scrollbarTimer);
      _scrollbarTimer = setTimeout(() => document.body.classList.remove('animating'), 450);
    }
    document.addEventListener('animationstart', (e) => {
      if (_entranceAnims.indexOf(e.animationName) !== -1) _hideScrollbarDuringAnim();
    });
    document.addEventListener('transitionstart', (e) => {
      if (e.target && e.target.matches && e.target.matches(_scrollCtrSel)) _hideScrollbarDuringAnim();
    });

    // 面板导航
    $('navDownloads').onclick = () => showPanel('downloads');
    $('navSettings').onclick = () => showPanel('settings');
    if ($('navHome')) $('navHome').onclick = () => showPanel('home');

    // 设置内部导航（区分主settings和launchSettingsOverlay内的lsNav）
    document.querySelectorAll('.settings-nav-item').forEach((el) => {
      el.onclick = () => {
        const parentNav = el.closest('.settings-nav');
        if (!parentNav) return;
        if (parentNav.id === 'lsNav') {
          parentNav.querySelectorAll('.settings-nav-item').forEach((x) => x.classList.remove('active'));
          el.classList.add('active');
          const section = el.dataset.lsSection;
          document.querySelectorAll('[data-ls-body]').forEach((s) => s.classList.remove('active'));
          const body = document.querySelector('[data-ls-body="' + section + '"]');
          if (body) body.classList.add('active');
        } else {
          parentNav.querySelectorAll('.settings-nav-item').forEach((x) => x.classList.remove('active'));
          el.classList.add('active');
          const section = el.dataset.section;
          const host = parentNav.closest('.settings-layout');
          if (host) {
            host.querySelectorAll('.settings-section').forEach((x) => x.classList.remove('active'));
            const body = host.querySelector('.settings-section[data-section-body="' + section + '"]');
            if (body) body.classList.add('active');
          }
        }
      };
    });

    // 账号下拉
    $('accountTrigger').onclick = () => $('accountDropdown').classList.toggle('open');
    document.addEventListener('click', (e) => {
      if (!e.target.closest('.account-dropdown')) closeAccountMenu();
      if (!e.target.closest('.form-select-wrapper')) {
        document.querySelectorAll('.form-select-wrapper.open').forEach((w) => w.classList.remove('open'));
      }
      if (!e.target.closest('.custom-bar-overflow')) {
        const ov = $('customBarOverflow');
        if (ov) ov.classList.remove('open');
      }
    });

    // form-select-wrapper 展开/收起（事件委托，兼容后插入 DOM 的下拉框）
    // 原先 querySelectorAll 一次性绑定会漏掉动态生成的 wrapper（如版本下载、
    // launch 动态加载版本列表后插入的下拉），表现为"下拉框点不动"。
    // 改为点击事件委托到 document：命中 .form-select-display 时执行展开逻辑。
    document.addEventListener('click', (e) => {
      const display = e.target.closest('.form-select-display');
      if (!display) return;
      const wrap = display.closest('.form-select-wrapper');
      if (!wrap) return;
      e.stopPropagation();
      const wasOpen = wrap.classList.contains('open');
      document.querySelectorAll('.form-select-wrapper.open').forEach((w) => w.classList.remove('open'));
      if (!wasOpen) wrap.classList.add('open');
    }, true);

    $('accountManageBtn').onclick = () => {
      closeAccountMenu();
      showPanel('settings');
      setTimeout(() => {
        const nav = document.querySelector('.settings-nav-item[data-section="players"]');
        if (nav) nav.click();
      }, 20);
    };

    // 多开开关
    const multiToggle = $('multiInstanceToggle');
    if (multiToggle) {
      multiToggle.addEventListener('click', () => {
        State.allowMultiInstance = multiToggle.classList.toggle('active');
        try { localStorage.setItem('lxe_allow_multi_instance', State.allowMultiInstance ? '1' : '0'); } catch (e) {}
        setGameRunningUI(State.gameRunning, State.gamePids);
      });
    }

    // 浮动停止按钮
    const fsb = $('floatingStopBtn');
    if (fsb) {
      fsb.onclick = () => {
        if (LX && LX.call) {
          LX.call('mc.stop').then(() => notify('已提交停止游戏请求', 'info')).catch(() => {});
        } else {
          setGameRunningUI(false, []);
        }
      };
    }

    // MC 文件夹导入按钮
    const addMcBtns = [$('addMcFolderBtn'), $('addMcFolderBtnFolders')].filter(Boolean);
    addMcBtns.forEach((b) => {
      b.onclick = () => {
        if (LX && LX.call) {
          LX.call('mc.importFolder').then((f) => {
            if (f) {
              notify('已导入 MC 文件夹', 'success');
              loadMcFolders();
            }
          }).catch((e) => notify('导入失败：' + e.message, 'error'));
        } else {
          notify('后端未连接', 'error');
        }
      };
    });

    // 版本分支过滤 tabs
    document.querySelectorAll('#versionBranchTabs .seg-tab').forEach((tab) => {
      tab.onclick = () => {
        document.querySelectorAll('#versionBranchTabs .seg-tab').forEach((x) => x.classList.remove('active'));
        tab.classList.add('active');
        State.currentVersionBranch = tab.dataset.branch;
        renderDownloadTable();
      };
    });

    // 主页
    $('openVersionSelect').onclick = openVersionOverlay;
    const voc = $('versionOverlayClose');
    if (voc) voc.onclick = closeVersionOverlay;
    $('openLaunchSettings').onclick = () => {
      const badge = $('lsVersionBadge');
      if (badge) badge.textContent = State.currentVersion ? State.currentVersion.name : '未选择版本';
      $('launchSettingsOverlay').classList.add('open');
    };
    $('openVersionSettings').onclick = () => {
      renderVersionComponentsList();
      $('versionSettingsOverlay').classList.add('open');
    };
    $('launchBtn').onclick = () => {
      if (!State.currentVersion) { notify('请先选择版本', 'error'); return; }
      const btn = $('launchBtn');
      if (State.allowMultiInstance) {
        appendLog('正在启动 ' + State.currentVersion.name + '（内存 ' + State.mem + ' MB，多开模式）...');
        notify('已提交启动请求：' + State.currentVersion.name, 'success');
        if (LX && LX.call) {
          LX.call('mc.launch', {
            version: State.currentVersion.id, folder: State.activeMcFolder,
            mem: State.mem, jvm: $('jvmArgsInput') && $('jvmArgsInput').value || null,
          }).catch((e) => notify('启动失败：' + e.message, 'error'));
        }
      } else {
        if (btn.classList.contains('running')) {
          if (LX && LX.call) {
            LX.call('mc.stop').then(() => notify('已提交停止请求', 'info')).catch(() => {});
          } else {
            setGameRunningUI(false, []);
            appendLog('游戏已停止');
            notify('已停止游戏', 'info');
          }
        } else {
          appendLog('正在启动 ' + State.currentVersion.name + '（内存 ' + State.mem + ' MB）...');
          notify('已提交启动请求：' + State.currentVersion.name, 'success');
          if (LX && LX.call) {
            LX.call('mc.launch', {
              version: State.currentVersion.id, folder: State.activeMcFolder,
              mem: State.mem, jvm: $('jvmArgsInput') && $('jvmArgsInput').value || null,
            }).catch((e) => notify('启动失败：' + e.message, 'error'));
          } else {
            setGameRunningUI(true, [Math.floor(Math.random() * 9000) + 1000]);
          }
        }
      }
    };
    function appendLog(text) {
      const el = $('logConsole');
      if (!el) return;
      el.textContent += (el.textContent ? '\n' : '') + '> ' + text;
      el.scrollTop = el.scrollHeight;
    }

    // 下载中心 tab 切换（卡片式 game/mod/modpack；表格式 java/loaders）
    document.querySelectorAll('#downloadTabs .seg-tab').forEach((tab) => {
      tab.onclick = () => {
        document.querySelectorAll('#downloadTabs .seg-tab').forEach((x) => x.classList.remove('active'));
        tab.classList.add('active');
        currentDownloadTab = tab.dataset.tab;
        State.currentDownloadTab = tab.dataset.tab;
        switchDownloadTab(tab.dataset.tab);
      };
    });
    $('downloadSearch').addEventListener('input', () => {
      const t = State.currentDownloadTab || currentDownloadTab;
      if (t === 'java') renderBmclJavaTable();
      else if (t === 'loaders') renderBmclLoaderTableContent();
      else renderDownloadTable();
    });
    // 刷新按钮：强制重新拉取当前 tab 的数据
    const bmclRefreshBtn = $('bmclRefreshBtn');
    if (bmclRefreshBtn) {
      bmclRefreshBtn.onclick = () => {
        const t = State.currentDownloadTab || currentDownloadTab;
        notify('正在重新拉取 BMCLAPI 数据…', 'info');
        if (t === 'java') {
          State.bmcl.javaList = null; State.downloads.java = [];
          showDlLoading(true);
          BMCLAPI.javaList(true).then(() => { showDlLoading(false); renderBmclJavaTable(); });
        } else if (t === 'loaders') {
          const k = State.bmcl.loaderSubtab; const mc = State.bmcl.loaderMcVersion;
          if (k === 'forge') State.bmcl.forgeCache.delete(mc);
          if (k === 'neoforge') State.bmcl.neoforgeCache.delete(mc);
          if (k === 'optifine') State.bmcl.optifineList = null;
          preloadCurrentLoaderData();
        } else {
          renderDownloadTable();
        }
      };
    }
    // 初始化镜像源：从 localStorage 恢复
    try {
      const saved = localStorage.getItem('lxe_bmcl_mirror');
      if (saved === 'bmclapi') { State.bmcl.mirrorSource = 'bmclapi'; }
      else { State.bmcl.mirrorSource = 'official'; }
    } catch (e) {}
    // 设置页面镜像源下拉框（自定义 form-select-options）
    const mirrorOptions = $('mirrorOptions');
    const mirrorText = $('mirrorText');
    if (mirrorOptions) {
      mirrorOptions.innerHTML = '';
      const opts = [
        { v: 'official', n: '官方源 (全球)' },
        { v: 'bmclapi', n: 'BMCLAPI (国内镜像 · bangbang93)' },
      ];
      // 同步当前显示文字
      if (mirrorText) {
        const cur = opts.find((o) => o.v === State.bmcl.mirrorSource);
        mirrorText.textContent = cur ? cur.n : '官方源';
      }
      opts.forEach((o) => {
        const d = document.createElement('div');
        d.className = 'form-select-option' + (o.v === State.bmcl.mirrorSource ? ' selected' : '');
        d.dataset.value = o.v;
        d.textContent = o.n;
        d.onclick = () => {
          State.bmcl.mirrorSource = o.v;
          try { localStorage.setItem('lxe_bmcl_mirror', State.bmcl.mirrorSource); } catch (e) {}
          if (mirrorText) mirrorText.textContent = o.n;
          // 高亮选中
          mirrorOptions.querySelectorAll('.form-select-option').forEach((x) => x.classList.toggle('selected', x === d));
          // 关闭下拉框
          const wrapper = mirrorOptions.closest('.form-select-wrapper');
          if (wrapper) wrapper.classList.remove('open');
          // 清空缓存
          State.bmcl.javaList = null; State.downloads.java = [];
          State.bmcl.forgeMcVersions = null; State.bmcl.forgeCache.clear();
          State.bmcl.neoforgeCache.clear(); State.bmcl.optifineList = null;
          updateSourceTag();
          notify('已切换镜像源：' + o.n, 'success');
        };
        mirrorOptions.appendChild(d);
      });
    }
    // 默认：初始化当前选中的 tab（game），source tag
    updateSourceTag();
    // 确保 State.currentDownloadTab 与当前 DOM 激活 tab 同步
    const activeTab = document.querySelector('#downloadTabs .seg-tab.active');
    if (activeTab) {
      State.currentDownloadTab = activeTab.dataset.tab;
      currentDownloadTab = activeTab.dataset.tab;
    }
    // 如果启动时当前面板就是 downloads，直接初始化一次视图
    if (State.currentPanel === 'downloads') switchDownloadTab(State.currentDownloadTab || 'game');

    // 关闭浮层
    document.querySelectorAll('[data-close]').forEach((btn) => {
      btn.onclick = () => $(btn.dataset.close).classList.remove('open');
    });
    $('deleteVersionBtn').onclick = () => confirmBox('删除版本', '确定删除当前版本吗？（后端执行文件删除）', () => {
      notify('版本删除请求已提交', 'success');
      $('versionSettingsOverlay').classList.remove('open');
    });
    $('renameBtn').onclick = () => {
      const v = $('renameInput').value.trim();
      if (v) { notify('版本重命名为：' + v, 'success'); $('renameInput').value = ''; }
      else notify('请输入新版本名', 'error');
    };
    $('addAccountBtn').onclick = () => notify('添加账号：后端 MSA 登录待接入', 'info');

    // Modloader 浮层按钮绑定
    const openLoaderBtn = $('openLoaderBtn');
    if (openLoaderBtn) openLoaderBtn.onclick = openLoaderVersionOverlay;

    // 调色盘
    document.querySelectorAll('.palette-swatch').forEach((s) => {
      s.onclick = () => {
        document.querySelectorAll('.palette-swatch').forEach((x) => x.classList.remove('selected'));
        s.classList.add('selected');
        applyCustomPrimary(s.dataset.color);
        const { r, g, b } = hexToRgb(s.dataset.color);
        currentHSL = rgbToHsl(r, g, b); syncSliders();
        const pv = $('paletteValue'); if (pv) pv.textContent = s.dataset.color;
        const cp = $('colorPreview'); if (cp) cp.style.background = s.dataset.color;
      };
    });
    const paletteCustom = $('paletteCustom');
    if (paletteCustom) {
      paletteCustom.addEventListener('input', (e) => {
        applyCustomPrimary(e.target.value);
        const { r, g, b } = hexToRgb(e.target.value);
        currentHSL = rgbToHsl(r, g, b); syncSliders();
        const pv = $('paletteValue'); if (pv) pv.textContent = e.target.value;
        const cp = $('colorPreview'); if (cp) cp.style.background = e.target.value;
      });
    }
    document.querySelectorAll('.color-btn').forEach((b) => {
      b.onclick = () => {
        setTheme(b.dataset.theme);
        State.customPrimary = null;
        try { localStorage.removeItem('lxe_custom_primary'); } catch (e) {}
        ['--primary-color', '--primary-hover', '--primary-light', '--scrollbar-thumb', '--scrollbar-thumb-hover']
          .forEach((p) => document.body.style.removeProperty(p));
        notify('已切换主题：' + b.title, 'success');
      };
    });
    const btnNamesToggle = $('btnNamesToggle');
    if (btnNamesToggle) {
      btnNamesToggle.onclick = () => {
        btnNamesToggle.classList.toggle('active');
        document.body.classList.toggle('no-btn-names', !btnNamesToggle.classList.contains('active'));
        try { localStorage.setItem('lxe_btn_names', btnNamesToggle.classList.contains('active') ? '1' : '0'); } catch (e) {}
      };
      try {
        if (localStorage.getItem('lxe_btn_names') === '0') {
          btnNamesToggle.classList.remove('active');
          document.body.classList.add('no-btn-names');
        }
      } catch (e) {}
    }

    // 插件开关
    const pluginsToggle = $('pluginsToggle');
    if (pluginsToggle) {
      pluginsToggle.onclick = () => {
        pluginsToggle.classList.toggle('active');
        notify(pluginsToggle.classList.contains('active') ? '已启用插件系统' : '插件已禁用', 'info');
      };
    }

    // 窗口关闭前二次确认（beforeunload 兜底）
    window.addEventListener('beforeunload', (e) => {
      if (hasActiveDownloads()) {
        e.preventDefault();
        e.returnValue = '您有进行中的下载任务，确定要关闭启动器吗？';
        return e.returnValue;
      }
    });
    window.addEventListener('resize', () => {
      updateCustomBarOverflow();
    });

    // 插件系统：自动发现 plugins/ 目录下的文件靠后端；前端这里暴露 window.LX 给插件脚本引用。
    window.LX = LX;
    window.LXState = State;

    // 快捷键
    document.addEventListener('keydown', (e) => {
      if (e.key === 'Escape') {
        closeVersionOverlay();
        document.querySelectorAll('.overlay.open').forEach((o) => o.classList.remove('open'));
      }
    });

    // 插件插槽首次渲染
    LX.plugins.flushSlots();

    // 默认选第一个账号
    selectAccount(State.accounts[0] && State.accounts[0].id || null);
  }

  // 版本设置里的"组件列表"（原模组列表现改组件信息）
  function renderVersionComponentsList() {
    const list = $('versionModList');
    list.innerHTML = '';
    // 读取版本绑定的"组件"，这里用静态示例（插件也可注入）
    const demo = [
      { name: 'Java 17', sub: '随版本绑定 · 自动检测' },
      { name: 'JVM 参数', sub: '-Xmx4G -XX:+UseG1GC' },
      { name: '启动图标', sub: '当前：assets/covers/ 目录未设置封面' },
    ];
    demo.forEach((m) => {
      const item = document.createElement('div');
      item.className = 'app-item';
      const info = document.createElement('div');
      info.className = 'app-item-info';
      const name = document.createElement('div');
      name.className = 'app-item-name';
      name.textContent = m.name;
      const sub = document.createElement('div');
      sub.className = 'app-item-sub';
      sub.textContent = m.sub;
      info.append(name, sub);
      const sw = document.createElement('button');
      sw.className = 'toggle-switch active';
      sw.innerHTML = '';
      sw.onclick = () => sw.classList.toggle('active');
      item.append(info, sw);
      list.appendChild(item);
    });
  }

  document.addEventListener('DOMContentLoaded', init);
})(window.LX, document, window);
