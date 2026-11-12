(() => {
  const state = {
    connected: false, running: false, canStartSession: true, inputEnabled: false, mode: 'copy',
    deviceName: 'Android 设备', deviceCategory: 'tablet', deviceRefresh: '--', status: '正在准备控制中心…',
    resolution: '原生自动', vddMode: '--', frameRate: '60 FPS',
    displays: [], resolutions: [], vddModes: [], selectedDisplay: 0,
    selectedResolution: 0, selectedVddMode: 0, parsecAvailable: false,
    autoResume: false, autoResumeMode: 'copy', lowLatencyDecode: false,
    outputFps: '', bitrateMbps: '', latencyMs: '', latencyP95Ms: '',
    parsecVersion: 0, parsecDisplayCount: 0, diagnostics: ''
  };
  const send = command => window.chrome?.webview?.postMessage(command);
  const lockedCommands = new Set(['input-off', 'input-on', 'create-vdd', 'remove-vdd', 'recover-vdd', 'preview']);
  const lockedMessage = '先停止当前 WireView 视频会话';
  let transientStatus = '';
  let transientStatusTimer = 0;
  let settingsOpen = false;
  let darkMode = localStorage.getItem('wireview-theme') !== 'light';
  const escape = value => String(value ?? '').replace(/[&<>"']/g, char => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[char]);
  const optionList = (items, selected) => items.map((item, index) => `<option value="${index}" ${index === selected ? 'selected' : ''}>${escape(item)}</option>`).join('');
  const panelTitle = (number, title) => `<div class="card-title"><span><i class="number">${number}</i>${title}</span></div>`;
  const deviceCard = () => `<article class="device-card ${state.connected ? 'is-connected' : 'is-disconnected'}"><div class="device-art"></div><div class="device-copy"><span class="connected ${state.connected ? 'is-connected' : 'is-disconnected'}">${state.connected ? '● 设备已连接' : '○ 等待设备连接'}</span><strong class="device-name">${escape(state.deviceName)}</strong><span class="device-meta">${state.connected ? `USB 已连接 · ${escape(state.resolution)} · ${escape(state.deviceRefresh)} Hz` : '连接 USB 数据线并允许 USB 调试'}</span></div><div class="device-actions"><button class="device-stop" data-command="stop" ${state.running ? '' : 'disabled'}>■　停止 WireView</button><button class="device-settings" data-command="display-settings">▣　Windows 显示设置</button></div></article>`;
  const simple = () => {
    const modeDisabled = state.connected && state.canStartSession ? '' : 'disabled';
    document.querySelector('#simple-page').innerHTML = `<div class="simple-layout">${deviceCard()}<article class="panel simple-configuration"><section class="simple-mode-section"><div class="section-heading"><h2>显示方式</h2><p>选择本次启动的显示方式（启动后生效）</p></div><div class="mode-cards"><button class="mode-card copy ${state.running && state.mode === 'copy' ? 'is-selected' : ''}" data-command="mode-copy" ${modeDisabled}><span class="diagram-stage" aria-hidden="true"><span class="screen-diagram copy"><i class="screen primary"></i><i class="screen secondary"></i></span></span><span class="mode-copy"><span class="mode-title">复制屏幕</span><span class="mode-description">副屏显示与主屏相同的内容</span><span class="hint">推荐用于演示与分享</span></span></button><button class="mode-card extend ${state.running && state.mode === 'extend' ? 'is-selected' : ''}" data-command="mode-extend" ${modeDisabled}><span class="diagram-stage" aria-hidden="true"><span class="screen-diagram extend"><i class="screen primary"></i><i class="screen secondary"></i></span></span><span class="mode-copy"><span class="mode-title">扩展屏幕</span><span class="mode-description">将副屏作为扩展显示区域</span><span class="hint">推荐用于效率与创作</span></span></button></div></section><section class="simple-input-section"><div class="section-heading"><h2>本次会话输入</h2><p>启动 WireView 时应用此设置</p></div><div class="segmented"><button class="segment ${!state.inputEnabled ? 'is-selected' : ''}" data-command="input-off"><span class="radio"></span><span><strong>仅显示</strong><small>副屏仅显示画面，无法触控</small></span></button><button class="segment ${state.inputEnabled ? 'is-selected' : ''}" data-command="input-on"><span class="radio"></span><span><strong>触控与手写笔</strong><small>启用触控与手写笔输入</small></span></button></div></section></article></div>`;
  };
  const full = () => {
    const startLabel = state.running ? 'WireView 正在运行' : '▶　启动 WireView';
    const sessionTitle = `${state.running ? '会话摘要 / 正在运行' : '会话摘要 / 即将启动'}<small class="session-title-note">点击“启动 WireView”后，将使用以上配置运行。</small>`;
    const vddDisplayImage = state.parsecDisplayCount > 0
      ? 'ui/vdd-display-1.png'
      : 'ui/vdd-display-0.png';
    document.querySelector('#full-page').innerHTML = `<div class="full-layout"><article class="panel compact-card">${panelTitle('1', '设备概览')}<div class="device-overview ${state.connected ? 'is-connected' : 'is-disconnected'}"><div class="device-art"></div><div><div class="connected ${state.connected ? 'is-connected' : 'is-disconnected'}">${state.connected ? '● 已连接' : '○ 未连接'}</div><div class="device-name" style="font-size:23px">${escape(state.deviceName)}</div><div class="device-meta">USB ${state.connected ? '已连接' : '未连接'} · ${escape(state.resolution)} · ${escape(state.deviceRefresh)} Hz</div></div></div></article><article class="panel compact-card">${panelTitle('2', '显示配置')}<div class="config-values"><div class="config-value"><label>目标显示器</label><select data-select="display">${optionList(state.displays, state.selectedDisplay)}</select></div><div class="config-value"><label>分辨率</label><select data-select="resolution">${optionList(state.resolutions, state.selectedResolution)}</select></div><div class="config-value"><label>刷新率</label><select data-select="vdd">${optionList(state.vddModes, state.selectedVddMode)}</select></div><div class="config-value"><label>视频帧率</label><select data-select="fps"><option value="60" ${state.frameRate.startsWith('60') ? 'selected' : ''}>60 FPS</option><option value="120" ${state.frameRate.startsWith('120') ? 'selected' : ''}>120 FPS（实验）</option></select></div></div></article><article class="panel compact-card vdd-card">${panelTitle('3', 'WireView 副屏 / Parsec VDD')}<div class="vdd-summary"><img class="vdd-display-icon" src="${vddDisplayImage}" alt="WireView 副屏数量 ${state.parsecDisplayCount}"><div class="vdd-metric"><small>当前 WireView 副屏数量</small><b>${state.parsecDisplayCount} 个</b><small>启动方式：${state.mode === 'extend' ? '扩展屏幕' : '复制屏幕'}</small></div><div class="vdd-side"><strong>◇　Parsec VDD</strong><span class="connected">${state.parsecAvailable ? '● 可用' : '○ 不可用'}</span><span class="device-meta">版本：${state.parsecVersion || '--'}</span></div></div><div class="vdd-actions"><button data-command="create-vdd">＋ 创建副屏</button><button data-command="remove-vdd">− 移除副屏</button><button data-command="recover-vdd">♜ 清理残留显示</button></div></article><article class="panel compact-card input-card">${panelTitle('4', '本次会话输入（启动时应用）')}<div class="input-compact"><button class="segment ${!state.inputEnabled ? 'is-selected' : ''}" data-command="input-off"><span class="radio"></span><span><strong>仅显示</strong><small>不启用触控与手写笔</small></span></button><button class="segment ${state.inputEnabled ? 'is-selected' : ''}" data-command="input-on"><span class="radio"></span><span><strong>触控与手写笔</strong><small>启用触控与手写笔输入</small></span></button></div></article><article class="panel session-action-card wide">${panelTitle('5', sessionTitle)}<div class="session-action-content"><div class="session-details"><div class="session-summary"><div class="session-item"><label>启动方式</label><strong>${state.mode === 'extend' ? '扩展屏幕' : '复制屏幕'}</strong></div><div class="session-item"><label>副屏数量</label><strong>${state.parsecDisplayCount} 个</strong></div><div class="session-item"><label>目标显示器</label><strong>${escape(state.displayName || '--')}</strong></div><div class="session-item"><label>输入模式</label><strong>${state.inputEnabled ? '触控与手写笔' : '仅显示'}</strong></div><div class="session-item"><label>帧率</label><strong>${escape(state.frameRate)}</strong></div></div></div><div class="session-actions"><button class="primary-button" data-command="start" ${state.running || !state.connected || !state.canStartSession ? 'disabled' : ''}>${startLabel}</button><button class="stop-button" data-command="stop" ${state.running ? '' : 'disabled'}>■　停止 WireView</button></div></div></article><div class="advanced-grid wide"><article class="panel advanced-card">${panelTitle('6', '高级工具')}<div class="tool-row"><button data-command="preview">◉　预览所选画面范围<small>预览并选择显示区域</small></button><button data-command="custom-modes">⌘　编辑自定义分辨率<small>创建或编辑分辨率</small></button><button data-command="display-settings">▣　Windows 显示设置<small>打开系统显示设置</small></button></div></article><article class="panel advanced-card">${panelTitle('7', '诊断与日志')}<button class="diagnostic-link" data-command="diagnostics"><span>▤　连接诊断、日志查看与导出<small>查看连接状态、系统信息与运行日志</small></span><b>›</b></button></article></div></div>`;
  };
  const addDisplayRefreshButton = () => {
    const overview = document.querySelector('#full-page .device-overview');
    if (!overview || overview.querySelector('[data-command="refresh-displays"]')) return;
    const button = document.createElement('button');
    button.className = 'display-refresh';
    button.dataset.command = 'refresh-displays';
    button.title = '重新检测显示器、Parsec VDD 副屏与 Android 设备连接状态';
    button.textContent = '↻　刷新';
    overview.append(button);
  };
  const renderStatus = () => {
    const metrics = [
      `副屏分辨率：${escape(state.resolution)}`,
      `刷新率：${escape(state.vddMode)}`,
      `Parsec VDD ${state.parsecAvailable ? '可用' : '不可用'}`
    ];
    if (state.outputFps) metrics.push(`${escape(state.outputFps)} FPS`);
    if (state.bitrateMbps) metrics.push(`${escape(state.bitrateMbps)} Mbps`);
    if (state.latencyMs) {
      metrics.push(`延迟 ${escape(state.latencyMs)} ms${state.latencyP95Ms ? `（P95 ${escape(state.latencyP95Ms)}）` : ''}`);
    }
    document.querySelector('#status-bar').innerHTML =
      `<span class="status-dot" style="background:${state.connected ? 'var(--green)' : 'var(--subtle)'}"></span>` +
      `<span class="status-main status-message">${escape(transientStatus || state.status || '准备就绪。')}</span>` +
      `<span class="status-divider"></span>` +
      `<span class="status-main status-metrics">${metrics.join('　·　')}</span>`;
  };
  const showTransient = text => {
    transientStatus = text;
    window.clearTimeout(transientStatusTimer);
    renderStatus();
    transientStatusTimer = window.setTimeout(() => { transientStatus = ''; renderStatus(); }, 2500);
  };
  const showLockedMessage = () => showTransient(lockedMessage);
  let pendingSwitchMode = null;
  const closeModeSwitchDialog = () => {
    pendingSwitchMode = null;
    const dialog = document.querySelector('#mode-switch-dialog');
    if (!dialog) return;
    dialog.hidden = true;
    dialog.classList.remove('is-open');
  };
  const openModeSwitchDialog = target => {
    pendingSwitchMode = target;
    const dialog = document.querySelector('#mode-switch-dialog');
    const text = document.querySelector('#mode-switch-text');
    if (!dialog) return;
    const current = state.mode === 'extend' ? '扩展屏幕' : '复制屏幕';
    const next = target === 'extend' ? '扩展屏幕' : '复制屏幕';
    if (text) {
      text.textContent = `当前正在使用${current}。切换到${next}会先停止当前投屏，再按新方式重新开始。`;
    }
    dialog.hidden = false;
    dialog.classList.add('is-open');
    document.querySelector('#mode-switch-confirm')?.focus();
  };
  const updateSettingsPanel = () => {
    const panel = document.querySelector('#settings-panel');
    const toggle = document.querySelector('#settings-toggle');
    panel.classList.toggle('is-open', settingsOpen);
    panel.setAttribute('aria-hidden', settingsOpen ? 'false' : 'true');
    toggle.setAttribute('aria-expanded', settingsOpen ? 'true' : 'false');
    document.body.classList.toggle('theme-light', !darkMode);
    const autoResumeToggle = document.querySelector('#auto-resume-toggle');
    if (autoResumeToggle) {
      autoResumeToggle.classList.toggle('is-on', !!state.autoResume);
      autoResumeToggle.setAttribute('aria-checked', state.autoResume ? 'true' : 'false');
      const autoResumeHint = document.querySelector('#auto-resume-state');
      if (autoResumeHint) {
        autoResumeHint.textContent = state.autoResume
          ? (state.autoResumeMode === 'extend' ? '已开启 · 恢复扩展屏幕' : '已开启 · 恢复复制屏幕')
          : '已关闭';
      }
    }
    const lowLatencyToggle = document.querySelector('#low-latency-toggle');
    if (lowLatencyToggle) {
      lowLatencyToggle.classList.toggle('is-on', !!state.lowLatencyDecode);
      lowLatencyToggle.setAttribute('aria-checked', state.lowLatencyDecode ? 'true' : 'false');
      const lowLatencyHint = document.querySelector('#low-latency-state');
      if (lowLatencyHint) lowLatencyHint.textContent = state.lowLatencyDecode ? '已开启 · 需重新开始投屏' : '已关闭';
    }
    const themeToggle = document.querySelector('#theme-toggle');
    themeToggle.classList.toggle('is-on', darkMode);
    themeToggle.setAttribute('aria-checked', darkMode ? 'true' : 'false');
    themeToggle.closest('.settings-toggle-row').querySelector('small').textContent = darkMode ? '当前已开启' : '当前已关闭';
  };
  const wireSettingsEvents = () => {
    const panel = document.querySelector('#settings-panel');
    const toggle = document.querySelector('#settings-toggle');
    toggle.addEventListener('click', event => {
      event.stopPropagation();
      settingsOpen = !settingsOpen;
      updateSettingsPanel();
    });
    document.querySelector('#auto-resume-toggle').addEventListener('click', event => {
      event.stopPropagation();
      state.autoResume = !state.autoResume;
      updateSettingsPanel();
      send(state.autoResume ? 'auto-resume-on' : 'auto-resume-off');
    });
    document.querySelector('#low-latency-toggle').addEventListener('click', event => {
      event.stopPropagation();
      state.lowLatencyDecode = !state.lowLatencyDecode;
      updateSettingsPanel();
      send(state.lowLatencyDecode ? 'low-latency-on' : 'low-latency-off');
    });
    document.querySelector('#theme-toggle').addEventListener('click', event => {
      event.stopPropagation();
      darkMode = !darkMode;
      localStorage.setItem('wireview-theme', darkMode ? 'dark' : 'light');
      updateSettingsPanel();
      send(darkMode ? 'theme-dark' : 'theme-light');
    });
    panel.addEventListener('click', event => {
      if (event.target === panel) {
        updateSettingsPanel();
      }
    });
    document.addEventListener('click', event => {
      if (!settingsOpen || panel.contains(event.target) || toggle.contains(event.target)) return;
      settingsOpen = false;
      updateSettingsPanel();
    });
  };
  const applyRunLocks = () => {
    document.body.classList.toggle('is-session-running', state.running);
    document.querySelectorAll('[data-command]').forEach(node => {
      const locked = state.running && lockedCommands.has(node.dataset.command);
      node.classList.toggle('is-run-locked', locked);
      node.setAttribute('aria-disabled', locked ? 'true' : 'false');
      if (locked) node.title = lockedMessage;
      else node.removeAttribute('title');
    });
    document.querySelectorAll('[data-select]').forEach(node => {
      const locked = state.running;
      const unavailable = !state.connected;
      node.classList.toggle('is-run-locked', locked);
      node.classList.toggle('is-device-disconnected', unavailable);
      node.disabled = unavailable;
      node.setAttribute('aria-disabled', locked || unavailable ? 'true' : 'false');
      node.dataset.lockedValue = node.value;
      if (locked) node.title = lockedMessage;
      else node.removeAttribute('title');
    });
    document.querySelectorAll('#full-page [data-command="preview"]').forEach(node => {
      const unavailable = !state.connected;
      node.disabled = unavailable;
      node.classList.toggle('is-device-disconnected', unavailable);
      node.setAttribute('aria-disabled', unavailable ? 'true' : 'false');
    });
  };
  const render = () => { simple(); full(); addDisplayRefreshButton(); renderStatus(); applyRunLocks(); wireEvents(); };
  const layoutSnapshot = () => JSON.stringify({
    connected: state.connected,
    running: state.running,
    canStartSession: state.canStartSession,
    inputEnabled: state.inputEnabled,
    mode: state.mode,
    deviceName: state.deviceName,
    deviceCategory: state.deviceCategory,
    deviceRefresh: state.deviceRefresh,
    resolution: state.resolution,
    vddMode: state.vddMode,
    frameRate: state.frameRate,
    displays: state.displays,
    resolutions: state.resolutions,
    vddModes: state.vddModes,
    selectedDisplay: state.selectedDisplay,
    selectedResolution: state.selectedResolution,
    selectedVddMode: state.selectedVddMode,
    parsecAvailable: state.parsecAvailable,
    parsecVersion: state.parsecVersion,
    parsecDisplayCount: state.parsecDisplayCount,
    displayName: state.displayName,
    autoResume: state.autoResume,
    autoResumeMode: state.autoResumeMode
  });
  const wireEvents = () => {
    document.querySelectorAll('[data-command]').forEach(node => node.addEventListener('click', () => {
      const command = node.dataset.command;
      if (command === 'mode-copy' || command === 'mode-extend') {
        const target = command === 'mode-copy' ? 'copy' : 'extend';
        if (!state.running) { send(command); return; }
        if (state.mode === target) { showTransient(target === 'extend' ? '已经在使用扩展屏幕。' : '已经在使用复制屏幕。'); return; }
        openModeSwitchDialog(target);
        return;
      }
      if (state.running && lockedCommands.has(command)) { showLockedMessage(); return; }
      send(command);
    }));
    document.querySelectorAll('[data-select]').forEach(node => {
      const blockIfRunning = event => {
        if (!state.running) return;
        event.preventDefault();
        showLockedMessage();
      };
      node.addEventListener('pointerdown', blockIfRunning);
      node.addEventListener('keydown', blockIfRunning);
      node.addEventListener('change', () => {
        if (state.running) { node.value = node.dataset.lockedValue; showLockedMessage(); return; }
        send(`select-${node.dataset.select}|${node.value}`);
      });
    });
  };
  document.querySelectorAll('.tab').forEach(tab => tab.addEventListener('click', () => {
    const simpleMode = tab.dataset.mode === 'simple';
    document.querySelectorAll('.tab').forEach(item => item.classList.toggle('is-active', item === tab));
    document.querySelector('#simple-page').classList.toggle('is-hidden', !simpleMode);
    document.querySelector('#full-page').classList.toggle('is-hidden', simpleMode);
  }));
  window.chrome?.webview?.addEventListener('message', event => {
    const beforeLayout = layoutSnapshot();
    const beforeStatus = state.status;
    const beforeMetrics = `${state.outputFps}|${state.bitrateMbps}|${state.latencyMs}|${state.latencyP95Ms}|${state.resolution}|${state.vddMode}|${state.parsecAvailable}`;
    Object.assign(state, event.data);
    updateSettingsPanel();
    if (beforeLayout !== layoutSnapshot()) render();
    else if (beforeStatus !== state.status ||
      beforeMetrics !== `${state.outputFps}|${state.bitrateMbps}|${state.latencyMs}|${state.latencyP95Ms}|${state.resolution}|${state.vddMode}|${state.parsecAvailable}`) renderStatus();
  });
  document.querySelector('#mode-switch-cancel')?.addEventListener('click', closeModeSwitchDialog);
  document.querySelector('#mode-switch-confirm')?.addEventListener('click', () => {
    const target = pendingSwitchMode;
    closeModeSwitchDialog();
    if (target) send(target === 'extend' ? 'mode-extend' : 'mode-copy');
  });
  document.querySelector('#mode-switch-dialog')?.addEventListener('click', event => {
    if (event.target === event.currentTarget) closeModeSwitchDialog();
  });
  document.addEventListener('keydown', event => {
    if (event.key === 'Escape' && pendingSwitchMode) closeModeSwitchDialog();
  });
  wireSettingsEvents();
  updateSettingsPanel();
  send(darkMode ? 'theme-dark' : 'theme-light');
  render();
  send('page-ready');
})();
