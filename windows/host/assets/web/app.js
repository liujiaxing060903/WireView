(() => {
  const emptyDevice = {
    connected: false, running: false, canStartSession: false, canStopSession: false,
    cursorSizePercent: 100, inputEnabled: false, mode: 'copy', resolution: '原生自动',
    vddMode: '--', frameRate: '60 FPS', deviceResolution: '', deviceRefresh: '--', displays: [], resolutions: [],
    vddModes: [], selectedDisplay: 0, selectedResolution: 0, selectedVddMode: 0,
    copySources: [], copySource: '', copyResolution: 'auto', copyFrameRate: 60
  };
  const send = command => window.chrome?.webview?.postMessage(command);
  let updateChecking = false;
  let updateVersion = '';
  let promptedUpdate = '';
  const showUpdateDialog = () => {
    if (!updateVersion) return;
    const dialog = document.querySelector('#update-dialog');
    document.querySelector('#update-dialog-title').textContent = `发现新版本 ${updateVersion}`;
    if (!dialog.open) dialog.showModal();
  };
  const checkUpdates = async (manual = false) => {
    const button = document.querySelector('#check-update');
    if (updateChecking || !window.WireViewUpdate || !button) return;
    updateChecking = true;
    button.disabled = true;
    if (manual) button.textContent = '检查中…';
    try {
      const current = document.querySelector('.brand-version').textContent.match(/\d+\.\d+\.\d+/)[0];
      const result = await window.WireViewUpdate.check(current);
      const download = document.querySelector('#update-download');
      download.hidden = !result.newer;
      download.textContent = `发现新版本 ${result.version} · 前往下载`;
      updateVersion = result.newer ? result.version : '';
      button.textContent = manual && !result.newer ? '已是最新' : '检查更新';
      button.title = result.newer ? `最新版本 ${result.version}` : '当前已是最新版本';
      if (result.newer && (manual || promptedUpdate !== result.version)) {
        promptedUpdate = result.version;
        showUpdateDialog();
      }
    } catch {
      if (manual) button.textContent = '检查失败';
      button.title = '暂时无法检查更新，请稍后重试';
    } finally {
      updateChecking = false;
      button.disabled = false;
      if (manual) window.setTimeout(() => { if (!updateChecking) button.textContent = '检查更新'; }, 2200);
    }
  };
  let dashboard = { dashboard: true, devices: [], availableDevices: [], selectedDevice: '', extensionCount: 0 };
  let pickerOpen = false;
  let focusDevice = '';
  const rendered = new Map();
  let settingsOpen = false;
  let supportVisible = false;
  let supportCodeEnlarged = false;
  let darkMode = localStorage.getItem('wireview-theme') !== 'light';
  const supportConfettiTrajectories = Array.from({ length: 54 }, (_, index) => {
    const angle = index * 2.399963229728653;
    const speed = 120 + (index % 8) * 19;
    return {
      velocityX: Math.cos(angle) * speed,
      velocityY: Math.sin(angle) * speed,
      gravity: 320 + (index % 6) * 19,
      duration: 1.75 + (index % 7) * .075,
      delay: (index * 29) % 85,
      spin: (index % 2 ? -1 : 1) * (430 + (index % 9) * 72),
      flips: 3 + (index % 4),
      width: 5 + (index % 6),
      height: 9 + (index % 7) * 2
    };
  });
  const escape = value => String(value ?? '').replace(/[&<>"']/g, char => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[char]);
  const optionList = (items, selected) => items.map((item, index) => `<option value="${index}" ${index === selected ? 'selected' : ''}>${escape(item)}</option>`).join('');
  const selectedDevice = () => dashboard.devices.find(device => device.serial === dashboard.selectedDevice);
  // Keep an immediate start lock until the host has reported its queue and then
  // completed it. An older ready snapshot must not unlock a rapid second click.
  const localStarts = new Map();
  const localOperations = new Map();
  const commandPending = device => device?.commandPending || localStarts.has(device?.serial);
  const stateFor = device => ({ ...emptyDevice, ...device?.state });
  const operationMode = device => localOperations.has(device?.serial) ? localOperations.get(device.serial)
    : device?.operationMode ?? device?.state?.operationMode
      ?? (device?.state?.running && !device.state.previewing ? device.state.mode : '');
  const isStreaming = device => device?.availability === 'device'
    && device.state?.running === true && device.state?.streaming === true;
  const statusTone = device => {
    if (!device || device.availability === 'offline') return 'offline';
    if (device.availability !== 'device') return 'warning';
    if (commandPending(device) && !isStreaming(device)) return 'busy';
    if (device.commandError) return 'error';
    const state = device.state;
    if (state?.sessionPhase === 'paused' && state?.adbProblem !== 'app_missing') return 'ready';
    if (state?.sessionPhase === 'error' || state?.sessionPhase === 'video_error') return 'error';
    if (state?.adbProblem || state?.sessionPhase === 'app_missing') return 'warning';
    if (isStreaming(device)) return 'live';
    if (state?.identifying || state?.running || state?.deviceSelectionLocked) return 'busy';
    return 'ready';
  };
  const connectionInfo = device => {
    const { availability, state } = device;
    if (availability === 'unauthorized') return ['未授权', '请解锁手机并允许 USB 调试'];
    if (availability === 'unknown') return ['USB 检查失败', '暂时无法确认设备状态，请点刷新重试'];
    if (availability === 'offline') return ['离线', '请检查 USB 数据线和手机的 USB 调试'];
    if (availability !== 'device') return ['设备不可用', '请确认 Android 已正常启动'];
    if (commandPending(device) && !isStreaming(device)) return ['正在发送操作', '设备正在处理操作，请稍候。'];
    if (device.commandError) return [isStreaming(device)
      ? (state.mode === 'extend' ? '正在扩展' : '正在复制') : '操作未确认', device.commandError];
    if (state?.sessionPhase === 'paused' && state?.adbProblem !== 'app_missing') {
      return ['已暂停', '请回到设备上的 WireView，将自动恢复原来的投屏；点击停止可取消。'];
    }
    if (state?.adbProblem === 'app_missing' || state?.sessionPhase === 'app_missing') {
      return ['未安装 WireView', 'USB 已接入，请在手机上安装 WireView'];
    }
    if (state?.adbProblem === 'app_not_running') return ['请打开 WireView', 'USB 已接入，请在手机上打开 WireView'];
    if (state?.adbProblem === 'probe_error') return ['应用检查失败', '暂时无法检查手机上的 WireView，请点刷新重试'];
    if (state?.sessionPhase === 'error') return ['连接失败', '请查看下方状态或连接诊断'];
    if (state?.sessionPhase === 'video_error') return ['画面异常', '手机端解码失败，请停止后重试'];
    if (state?.sessionPhase === 'reconnecting' || state?.sessionPhase === 'recovering') {
      return ['正在重连', '投屏连接已中断，正在尝试恢复'];
    }
    const resolution = state?.deviceResolution
      ? state.deviceResolution + (state.deviceRefresh === '--' ? '' : ` · ${state.deviceRefresh} Hz`) : '';
    if (isStreaming(device)) return [state.mode === 'extend' ? '正在扩展' : '正在复制', resolution || '手机正在显示电脑画面'];
    if (state?.identifying) return ['正在识别', '正在读取设备分辨率和刷新率'];
    if (state?.previewing) return ['画面预览', '手机上显示画面范围'];
    if (state?.running) return ['等待出画', '正在连接手机或等待首帧'];
    if (state?.deviceSelectionLocked && state?.canStopSession) return ['正在准备', '正在准备本次投屏，请稍候。'];
    if (state?.connected) return ['待投屏', ['USB 已接入', resolution].filter(Boolean).join(' · ')];
    return ['USB 已接入', '等待检查手机上的 WireView'];
  };
  const setMarkup = (selector, markup) => {
    if (rendered.get(selector) === markup) return;
    document.querySelector(selector).innerHTML = markup;
    rendered.set(selector, markup);
  };
  const sendToDevice = command => {
    const device = selectedDevice();
    if (!device) return;
    if (dashboard.dashboard && ['mode-copy', 'mode-extend', 'start', 'preview', 'preview-copy', 'preview-extend'].includes(command)) {
      if (commandPending(device)) return;
      if (command === 'mode-copy' || command === 'mode-extend') {
        localOperations.set(device.serial, command === 'mode-copy' ? 'copy' : 'extend');
      }
      localStarts.set(device.serial, false);
      render();
    }
    if (command === 'stop') { localOperations.set(device.serial, ''); render(); }
    send(dashboard.dashboard ? `device|${device.serial}|${command}` : command);
  };
  const refresh = () => send(dashboard.dashboard ? 'refresh-devices' : 'refresh-displays');

  const renderSidebar = () => {
    setMarkup('#device-list', dashboard.devices.map(device => `
      <button class="dashboard-device ${device.availability === 'offline' ? 'is-offline' : ''}" type="button" data-device="${escape(device.serial)}"
          aria-pressed="${device.serial === dashboard.selectedDevice}">
        <span class="dashboard-phone" aria-hidden="true"></span>
        <span class="dashboard-device-label">
          <strong title="${escape(device.name)}">${escape(device.name)}</strong>
          <small data-status="${statusTone(device)}" class="${isStreaming(device) ? 'is-online' : ''}">${connectionInfo(device)[0]}</small>
        </span>
      </button>`).join(''));
    const add = document.querySelector('#device-add');
    add.hidden = !dashboard.dashboard;
    add.setAttribute('aria-expanded', String(pickerOpen));
    document.querySelector('#device-picker').hidden = !pickerOpen;
    if (pickerOpen) {
      const options = dashboard.availableDevices.map(device => `
        <button type="button" data-add-device="${escape(device.serial)}" ${device.availability === 'device' ? '' : 'disabled'}>
          <strong>${escape(device.name)}</strong><small>${connectionInfo(device)[0]}</small>
        </button>`).join('');
      setMarkup('#device-picker', `<strong>选择投屏设备</strong>${options || '<small>暂无其他设备。接入 USB 后可点刷新。</small>'}`);
    }
    if (focusDevice) {
      const button = [...document.querySelectorAll('[data-device]')].find(node => node.dataset.device === focusDevice);
      if (button) { button.focus(); focusDevice = ''; }
    }
  };

  const stopHint = (control, running) => running
    ? `<span class="dashboard-stop-hint" data-stop-hint="更改此项需要先停止投屏">${control}</span>`
    : control;
  const modeCard = (mode, title, description, suggestion, badge, disabled, running) => stopHint(`
    <button class="dashboard-mode ${badge ? 'is-selected' : ''}" type="button"
        data-command="mode-${mode}" ${disabled ? 'disabled' : ''}>
      <svg class="dashboard-mode-art" aria-hidden="true" focusable="false"
          viewBox="${mode === 'copy' ? '85 210 1095 845' : '83 215 1608 447'}">
        <image href="ui/${mode}-screen.png" width="${mode === 'copy' ? 1254 : 1774}"
            height="${mode === 'copy' ? 1254 : 887}"></image>
      </svg>
      <span class="dashboard-mode-label"><strong>${title}</strong><small>${description}</small>
        <span class="dashboard-mode-tag ${badge ? 'dashboard-in-use' : ''}">${badge || suggestion}</span></span>
    </button>`, running);

  const screenLayout = (device, state, requestedMode) => {
    const screens = (dashboard.displayLayout ?? state.displayLayout ?? []).filter(screen =>
      ['x', 'y', 'width', 'height'].every(key => Number.isFinite(screen[key])) && screen.width > 0 && screen.height > 0);
    const mode = state.running ? state.mode : requestedMode;
    const directCopy = mode === 'copy' && !state.hasWindowsTarget;
    const target = directCopy ? state.copySource : state.layoutTarget;
    const current = screens.find(screen => !directCopy && state.layoutMonitor
      ? screen.path?.toLowerCase() === state.layoutMonitor.toLowerCase() : screen.source === target);
    const primary = screens.find(screen => screen.primary);
    const groups = [];
    for (const screen of screens) {
      let group = groups.find(item => ['x', 'y', 'width', 'height'].every(key => item[key] === screen[key]));
      if (!group) { group = { ...screen, numbers: [], selected: false }; groups.push(group); }
      group.numbers.push(screen.number);
      group.selected ||= screen === current;
    }
    let graphic = '<p class="dashboard-map-empty">暂时无法读取屏幕位置，请打开 Windows 显示设置查看。</p>';
    if (groups.length) {
      const left = Math.min(...groups.map(item => item.x)), top = Math.min(...groups.map(item => item.y));
      const width = Math.max(...groups.map(item => item.x + item.width)) - left + 240;
      const height = Math.max(...groups.map(item => item.y + item.height)) - top + 240;
      graphic = `<svg class="dashboard-map" viewBox="0 0 ${width} ${height}" role="img" aria-label="Windows 屏幕排布">${groups.map(item => {
        const x = item.x - left + 120, y = item.y - top + 120;
        return `<g data-screen="${escape(item.numbers.join('|'))}" class="${item.selected ? 'is-current' : ''}">
          <rect x="${x + 8}" y="${y + 8}" width="${Math.max(1, item.width - 16)}" height="${Math.max(1, item.height - 16)}" rx="48" />
          <text x="${x + item.width / 2}" y="${y + item.height / 2}" font-size="${Math.min(400, item.height * .32)}">${escape(item.numbers.join(' | '))}</text>
          <text class="dashboard-map-caption" x="${x + item.width / 2}" y="${y + item.height * .78}" font-size="${Math.min(160, item.height * .14)}">${item.selected ? (mode === 'copy' ? '复制来源' : '当前设备') : item.primary ? '电脑主屏' : '屏幕'}</text>
        </g>`;
      }).join('')}</svg>`;
    }
    const selected = groups.find(group => group.selected);
    let hint = '设备屏幕尚未就绪，准备完成或重新连接后，这里会显示它的位置。';
    if (selected && mode === 'copy') {
      hint = `复制来源：${selected.numbers.join(' | ')} 号屏幕，设备显示相同画面。`;
    } else if (current && primary) {
      const dx = current.x + current.width / 2 - primary.x - primary.width / 2;
      const dy = current.y + current.height / 2 - primary.y - primary.height / 2;
      const direction = Math.abs(dx) >= Math.abs(dy) ? (dx >= 0 ? '右侧' : '左侧') : (dy >= 0 ? '下方' : '上方');
      hint = current.source === primary.source ? '当前设备对应主屏，鼠标可以直接在这个桌面上操作。'
        : `当前设备位于电脑主屏${direction}，沿排布方向移动鼠标即可进入。`;
    }
    return `<section class="dashboard-screen-layout" aria-label="屏幕位置">
      <div class="dashboard-mode-heading"><div class="dashboard-section-title"><h2>屏幕位置</h2>
        <span class="dashboard-layout-mode">${mode === 'copy' ? '复制屏幕' : '扩展屏幕'}</span></div>
        <div class="dashboard-layout-actions"><button type="button" data-command="identify-screens" ${screens.length ? '' : 'disabled'}>标识屏幕</button>
        <button type="button" data-command="display-settings">调整屏幕位置</button></div></div>
      <div class="dashboard-map-stage">${graphic}</div><p class="dashboard-map-hint">${escape(hint)}</p>
      <div class="dashboard-layout-footer"><span>${escape(device.commandError || connectionInfo(device)[1])}</span>
        <button type="button" data-command="diagnostics">连接诊断</button></div>
    </section>`;
  };

  const renderDetail = () => {
    const device = selectedDevice();
    const advanced = document.querySelector('#advanced-settings');
    advanced.hidden = !device;
    if (!device) {
      setMarkup('#device-overview', `<section class="dashboard-empty"><h2>添加一台设备</h2>
        <p>用 USB 数据线连接 Android 设备并允许 USB 调试，再从左侧选择。</p></section>`);
      return;
    }

    const state = stateFor(device);
    const requestedMode = operationMode(device);
    const showingLayout = Boolean(requestedMode);
    advanced.hidden = showingLayout;
    const online = device.availability === 'device';
    const ready = online && state.connected;
    const locked = !ready || showingLayout || commandPending(device) || state.running || state.deviceSelectionLocked;
    const canStart = ready && !showingLayout && !commandPending(device) && state.canStartSession && !state.running;
    const copyInUse = state.copySource && dashboard.devices.find(other => other.serial !== device.serial
      && other.state?.running && other.state?.mode === 'copy' && other.state?.copySource === state.copySource);
    const limitReached = dashboard.extensionCount >= 2;
    const extensionBlocked = limitReached && !state.hasOwnedVdd;
    const stopLabel = state.previewing ? '结束预览' : state.running ? '停止投屏' : '取消操作';
    const action = showingLayout || state.canStopSession && (state.running || state.deviceSelectionLocked)
      ? `<button class="dashboard-stop" type="button" data-command="${state.previewing ? 'preview' : 'stop'}"
          aria-label="${stopLabel} · ${escape(device.name)}"><span aria-hidden="true">■</span> ${stopLabel}</button>`
      : !online && device.availability === 'offline' && dashboard.dashboard
        ? '<button class="dashboard-quiet" type="button" data-command="forget-device">从列表移除</button>' : '';
    const modeNote = state.sessionPhase === 'paused' ? '回到设备上的 WireView 后自动恢复，可停止取消'
      : state.running ? (state.canSwitchInWindows ? '当前模式跟随 Windows 显示设置'
      : state.hasWindowsTarget ? '当前 Windows 显示布局暂不可切换'
        : '直接投送现有屏幕。切换扩展请先停止本设备。')
      : extensionBlocked && online ? '两块独立扩展已在使用，请先停止一块'
        : ready && !state.copySource ? '复制来源已不可用，请在下方重新选择复制来源'
        : '点击上方复制或扩展开始；需要调整时，先修改下方对应设置';
    const windowsSwitch = state.canSwitchInWindows
      ? '<button class="dashboard-quiet" type="button" data-command="display-settings">在 Windows 切换</button>' : '';
    const badge = online && state.running ? (state.sessionPhase === 'paused' ? '已暂停' : state.streaming ? '使用中' : '连接中') : '';
    const disabled = locked ? 'disabled' : '';
    const settings = (label, key, options) => `<label><span>${label}</span>
      <select data-select="${key}" ${disabled}>${options}</select></label>`;
    const fpsOptions = selected => ['60', '120'].map(value => `<option value="${value}" ${value === String(selected) ? 'selected' : ''}>
      ${value} FPS${value === '120' ? '（实验）' : ''}</option>`).join('');
    const copySources = (state.copySource ? '' : '<option value="" disabled selected>请选择复制来源</option>')
      + state.copySources.map(source => `<option value="${escape(source.id)}" ${source.id === state.copySource ? 'selected' : ''}>${escape(source.label)}</option>`).join('');
    const preview = mode => `<button type="button" data-command="preview-${mode}" ${online && !commandPending(device) && state.canPreview ? '' : 'disabled'}>${state.previewing ? '结束画面预览' : '预览画面范围'}</button>`;
    const [label, detail] = connectionInfo(device);
    setMarkup('#device-overview', `
      <div class="dashboard-overview ${online ? 'is-online' : ''}">
        <span class="dashboard-device-art ${state.deviceCategory === 'phone' ? 'is-phone' : ''}" aria-hidden="true"></span>
        <div class="dashboard-device-info">
          <span data-status="${statusTone(device)}" class="dashboard-state ${isStreaming(device) ? 'is-online' : ''}">${label}</span>
          <h1>${escape(device.name)}</h1>
          <p>${escape(detail)}</p>
        </div>
        ${action}
      </div>
      <section class="dashboard-common">
        ${showingLayout ? screenLayout(device, state, requestedMode) : ''}
        <div class="dashboard-configuration" ${showingLayout ? 'hidden' : ''}>
        <div class="dashboard-mode-heading">
          <div class="dashboard-section-title"><h2>显示方式</h2><small>${modeNote}</small></div>
          ${windowsSwitch}
        </div>
        <div class="dashboard-mode-grid">
          ${modeCard('copy', '复制屏幕', '显示所选屏幕的画面', '适合演示与分享', state.mode === 'copy' ? badge : '', !canStart || !state.copySource || Boolean(copyInUse), state.running)}
          ${modeCard('extend', '扩展屏幕', '作为独立的 Windows 桌面', '适合多任务与创作', state.mode === 'extend' ? badge : '', !canStart || extensionBlocked, state.running)}
        </div>
        <div class="dashboard-mode-settings">
          <section aria-label="复制设置"><h3>复制设置</h3>
            <div class="dashboard-fields">${settings('复制来源', 'copy-source', copySources)}</div>
            <div class="dashboard-tools">${preview('copy')}</div>
            ${copyInUse ? `<p class="dashboard-tool-note">${escape(copyInUse.name)}正在复制这个来源，请换一个来源。</p>` : ''}
          </section>
          <section aria-label="扩展设置"><h3>扩展设置</h3>
            <div class="dashboard-fields">
              ${settings('桌面分辨率', 'resolution', optionList(state.resolutions.map(label => label
                .replace(/（使用\s*\d+\s*次）/g, '').replace(/（自定义槽\s*\d+）/g, '（自定义）')), state.selectedResolution))}
              ${settings('发送帧率', 'fps', fpsOptions(state.frameRate.startsWith('120') ? '120' : '60'))}
            </div>
            <div class="dashboard-tools">${preview('extend')}<button type="button" data-command="custom-resolution" ${disabled}>自定义分辨率</button></div>
          </section>
        </div>
        <div class="dashboard-input">
          <div class="dashboard-section-title"><h2>输入方式</h2><small>启动时应用</small></div>
          <div class="dashboard-input-options" aria-label="输入方式">
            ${stopHint(`<button type="button" data-command="input-off" aria-pressed="${!state.inputEnabled}" ${locked ? 'disabled' : ''}>仅显示</button>`, state.running)}
            ${stopHint(`<button type="button" data-command="input-on" aria-pressed="${state.inputEnabled}" ${locked ? 'disabled' : ''}>触控与手写笔</button>`, state.running)}
          </div>
        </div>
        <div class="dashboard-fields dashboard-public-settings"><label><span>鼠标指针大小</span><span class="dashboard-cursor-size">
          <input type="number" data-select="cursor" min="50" max="300" step="1" required
              aria-label="鼠标指针大小，百分比" value="${Number(state.cursorSizePercent)}" ${disabled}>
          <span>%</span></span><small>按设备记住，启动投屏时应用</small></label></div>
        </div>
      </section>`);

    const copySizes = [...new Set([...state.resolutions.map(label => label.match(/^(\d+x\d+)/)?.[1]),
      state.copyResolution === 'auto' ? null : state.copyResolution].filter(Boolean))];
    const copySizeOptions = `<option value="auto" ${state.copyResolution === 'auto' ? 'selected' : ''}>跟随来源尺寸</option>`
      + copySizes.map(size => `<option value="${size}" ${size === state.copyResolution ? 'selected' : ''}>${size}</option>`).join('');
    const vddStatus = state.parsecAvailable ? '驱动可用'
      : device.state ? '驱动未就绪' : '等待检查驱动';
    setMarkup('#advanced-content', `
      <div class="dashboard-fields">
        ${settings('复制 · 传输画面上限', 'copy-resolution', copySizeOptions)}
        ${settings('复制 · 发送帧率', 'copy-fps', fpsOptions(state.copyFrameRate))}
        ${settings('扩展 · Windows副屏刷新率', 'vdd', optionList(state.vddModes, state.selectedVddMode))}
      </div>
      <div class="dashboard-tools">
        <button type="button" data-command="custom-modes" ${disabled}>管理自定义分辨率</button>
        <button type="button" data-command="diagnostics" ${device.state ? '' : 'disabled'}>连接诊断与日志</button>
      </div>
      <p class="dashboard-tool-note">复制的传输设置不改变来源屏幕的设置。预览会在设备上显示等比画框。</p>
      <section class="dashboard-vdd" aria-label="Parsec VDD 驱动信息">
        <div class="dashboard-section-title"><h3>Parsec VDD</h3>
          <small>${vddStatus}${state.parsecVersion ? ` · 版本 ${escape(state.parsecVersion)}` : ''}</small></div>
      </section>`);
  };

  const render = () => {
    renderSidebar();
    renderDetail();
    const device = selectedDevice();
    const [label, detail] = device ? connectionInfo(device) : ['暂无投屏', dashboard.status];
    let sessionStatus = commandPending(device) ? '' : device?.commandError ||
      (device?.availability === 'device' && !device.state?.adbProblem
        && device.state?.sessionPhase !== 'app_missing' ? device.state?.status : '');
    if (!device?.state?.running && !device?.state?.identifying && !device?.state?.previewing
        && sessionStatus?.startsWith('状态：')) sessionStatus = '';
    const status = sessionStatus || detail || label;
    const summary = sessionStatus && !sessionStatus.startsWith('状态：') ? sessionStatus.split(/ · |。/, 1)[0] : label;
    setMarkup('#status-bar', `<span data-status="${statusTone(device)}" class="status-dot ${isStreaming(device) ? 'dashboard-live-dot' : ''}"></span>
      <span class="status-main" title="${escape(status)}">${escape(summary)}</span>
      <span class="dashboard-extension-count">独立扩展 ${dashboard.extensionCount} / 2</span>`);
  };

  const updateSettingsPanel = () => {
    const panel = document.querySelector('#settings-panel');
    const toggle = document.querySelector('#settings-toggle');
    const support = document.querySelector('#support-content');
    const supportQr = document.querySelector('#support-qr');
    const scanHelp = document.querySelector('#support-scan-help');
    const scanTip = document.querySelector('#support-scan-tip');
    panel.classList.toggle('is-open', settingsOpen);
    panel.setAttribute('aria-hidden', settingsOpen ? 'false' : 'true');
    toggle.setAttribute('aria-expanded', settingsOpen ? 'true' : 'false');
    support.hidden = !supportVisible;
    supportQr.src = supportCodeEnlarged ? 'ui/developer-support-enlarged.png' : 'ui/developer-support.png';
    scanTip.hidden = !supportCodeEnlarged;
    scanHelp.setAttribute('aria-pressed', supportCodeEnlarged ? 'true' : 'false');
    scanHelp.textContent = supportCodeEnlarged ? '已放大赞赏码' : '扫码有问题？';
    document.body.classList.toggle('theme-light', !darkMode);
    const themeToggle = document.querySelector('#theme-toggle');
    themeToggle.classList.toggle('is-on', darkMode);
    themeToggle.setAttribute('aria-checked', darkMode ? 'true' : 'false');
    themeToggle.closest('.settings-toggle-row').querySelector('small').textContent = darkMode ? '当前已开启' : '当前已关闭';
  };
  const copyDeveloperEmail = async () => {
    const email = 'S_Znnn@163.com';
    const button = document.querySelector('#copy-developer-email');
    try {
      if (!navigator.clipboard?.writeText) throw new Error('clipboard unavailable');
      await navigator.clipboard.writeText(email);
    } catch {
      const field = document.createElement('textarea');
      field.value = email;
      field.style.position = 'fixed';
      field.style.opacity = '0';
      document.body.append(field);
      field.select();
      document.execCommand('copy');
      field.remove();
    }
    button.textContent = '已复制';
    window.setTimeout(() => { button.textContent = '复制邮箱'; }, 1800);
  };
  const launchSupportConfetti = () => {
    const container = document.querySelector('.support-confetti');
    const colors = ['#ff6f91', '#ffcf4b', '#70ddff', '#9d7bff', '#66df84', '#f2f5ff'];
    container.replaceChildren();
    if (window.matchMedia('(prefers-reduced-motion: reduce)').matches) return;
    supportConfettiTrajectories.forEach((trajectory, index) => {
      const piece = document.createElement('i');
      piece.style.width = `${trajectory.width}px`;
      piece.style.height = `${trajectory.height}px`;
      piece.style.background = colors[Math.floor(Math.random() * colors.length)];
      container.append(piece);
      const frames = [];
      for (let frame = 0; frame <= 48; frame += 1) {
        const progress = frame / 48;
        const time = trajectory.duration * progress;
        const x = trajectory.velocityX * time;
        const y = trajectory.velocityY * time + .5 * trajectory.gravity * time * time;
        const opacity = progress < .06 ? progress / .06 : progress > .8 ? (1 - progress) / .2 : 1;
        const flip = Math.cos(progress * Math.PI * trajectory.flips);
        frames.push({
          offset: progress,
          opacity: Math.max(0, opacity),
          transform: `translate3d(calc(-50% + ${x.toFixed(1)}px), calc(-50% + ${y.toFixed(1)}px), 0) rotate(${(trajectory.spin * progress).toFixed(1)}deg) scaleX(${flip.toFixed(3)})`
        });
      }
      piece.animate(frames, { duration: trajectory.duration * 1000, delay: trajectory.delay, easing: 'linear', fill: 'both' });
    });
  };
  const wireSettingsEvents = () => {
    const panel = document.querySelector('#settings-panel');
    const toggle = document.querySelector('#settings-toggle');
    toggle.addEventListener('click', event => {
      event.stopPropagation();
      settingsOpen = !settingsOpen;
      if (!settingsOpen) {
        supportVisible = false;
        supportCodeEnlarged = false;
      }
      updateSettingsPanel();
    });
    document.querySelector('#support-developer').addEventListener('click', event => {
      event.stopPropagation();
      supportVisible = !supportVisible;
      if (!supportVisible) supportCodeEnlarged = false;
      updateSettingsPanel();
      if (supportVisible) launchSupportConfetti();
    });
    document.querySelector('#support-scan-help').addEventListener('click', event => {
      event.stopPropagation();
      supportCodeEnlarged = true;
      updateSettingsPanel();
    });
    document.querySelector('#copy-developer-email').addEventListener('click', event => {
      event.stopPropagation();
      void copyDeveloperEmail();
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
        supportVisible = false;
        supportCodeEnlarged = false;
        updateSettingsPanel();
      }
    });
    document.addEventListener('click', event => {
      if (!settingsOpen || panel.contains(event.target) || toggle.contains(event.target)) return;
      settingsOpen = false;
      supportVisible = false;
      supportCodeEnlarged = false;
      updateSettingsPanel();
    });
  };
  document.addEventListener('click', event => {
    const node = event.target.closest('[data-command], [data-device], [data-add-device]');
    if (!node || node.disabled) return;
    const { command, device, addDevice } = node.dataset;
    if (device || addDevice) {
      focusDevice = device || addDevice;
      if (addDevice) { pickerOpen = false; renderSidebar(); }
      send(addDevice ? `add-device|${addDevice}` : `select-device|${device}`);
    } else if (command === 'toggle-device-picker') {
      pickerOpen = !pickerOpen;
      renderSidebar();
      if (pickerOpen) refresh();
    } else if (command === 'refresh-devices') {
      refresh();
    } else if (command === 'display-settings' || command === 'identify-screens') {
      send(command);
    } else if (command === 'forget-device') {
      send(`forget-device|${dashboard.selectedDevice}`);
    } else if (command === 'custom-resolution') {
      const state = stateFor(selectedDevice());
      const size = state.resolutions[state.selectedResolution]?.match(/^(\d+)x(\d+)/);
      document.querySelector('#custom-width').value = size?.[1] || '2560';
      document.querySelector('#custom-height').value = size?.[2] || '1600';
      document.querySelector('#custom-hz').value = state.vddModes[state.selectedVddMode]?.match(/^\d+/)?.[0] || '60';
      document.querySelector('#custom-resolution-dialog').showModal();
    } else if (command) {
      sendToDevice(command);
    }
  });
  document.addEventListener('change', event => {
    const node = event.target;
    if (node.dataset.select === 'cursor' && !node.checkValidity()) {
      node.reportValidity();
      return;
    }
    const value = node.dataset.select === 'cursor' ? String(node.valueAsNumber) : node.value;
    if (node.dataset.select && !node.disabled) sendToDevice(`select-${node.dataset.select}|${value}`);
  });
  window.chrome?.webview?.addEventListener('message', event => {
    const previous = dashboard.selectedDevice;
    const message = event.data;
    if (message.dashboard) {
      dashboard = message;
    } else {
      // An explicitly opened per-device window uses the same device page.
      dashboard = {
        dashboard: false, selectedDevice: message.selectedDevice, availableDevices: [],
        extensionCount: Number(message.running && message.mode === 'extend'),
        devices: message.selectedDevice ? [{
          serial: message.selectedDevice, name: message.deviceName,
          availability: message.connected || message.adbProblem || message.sessionPhase === 'app_missing'
            ? 'device' : 'offline', state: message
        }] : []
      };
    }
    for (const [serial, mode] of localOperations) {
      const device = dashboard.devices.find(entry => entry.serial === serial);
      const reported = device?.operationMode ?? device?.state?.operationMode;
      if (!device || reported === mode) localOperations.delete(serial);
    }
    for (const [serial, seenByHost] of localStarts) {
      const device = dashboard.devices.find(entry => entry.serial === serial);
      if (!device || device.availability !== 'device' || device.commandError
          || device.state?.running || device.state?.deviceSelectionLocked) {
        localStarts.delete(serial);
      } else if (device.commandPending) {
        localStarts.set(serial, true);
      } else if (seenByHost) {
        localStarts.delete(serial);
      }
    }
    if (previous !== dashboard.selectedDevice) {
      document.querySelector('#advanced-settings').open = false;
      document.querySelector('#device-page').scrollTop = 0;
    }
    render();
  });
  document.querySelector('#custom-resolution-cancel').addEventListener('click', () => document.querySelector('#custom-resolution-dialog').close());
  document.querySelector('#custom-resolution-form').addEventListener('submit', event => {
    event.preventDefault();
    if (!event.target.reportValidity()) return;
    const values = ['#custom-width', '#custom-height', '#custom-hz'].map(id => document.querySelector(id).valueAsNumber);
    sendToDevice(`custom-resolution|${values[0]}x${values[1]}@${values[2]}`);
    document.querySelector('#custom-resolution-dialog').close();
  });
  wireSettingsEvents();
  document.querySelector('#check-update')?.addEventListener('click', () => { void checkUpdates(true); });
  document.querySelector('#update-download')?.addEventListener('click', showUpdateDialog);
  document.querySelector('#update-close')?.addEventListener('click', () => document.querySelector('#update-dialog').close());
  document.querySelector('#update-github')?.addEventListener('click', () => send('open-release-page'));
  document.querySelector('#update-mirror')?.addEventListener('click', () => send('open-mirror-page'));
  void checkUpdates();
  window.setInterval?.(() => { void checkUpdates(); }, 6 * 60 * 60 * 1000);
  updateSettingsPanel();
  send(darkMode ? 'theme-dark' : 'theme-light');
  render();
  send('page-ready');
})();
