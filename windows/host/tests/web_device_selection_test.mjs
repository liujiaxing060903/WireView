// Exercise the page's state and command boundaries. Hardware is checked separately.
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import vm from 'node:vm';

const nodes = new Map();
const listeners = new Map();
const commands = [];
let receive;
class Element {
  constructor() {
    this.dataset = {};
    this.style = {};
    this.classList = { toggle() {}, add() {}, remove() {} };
  }
  addEventListener() {}
  setAttribute() {}
  removeAttribute() {}
  contains() { return false; }
  closest(selector) { return get(selector); }
  querySelector(selector) { return get(selector); }
  querySelectorAll() { return []; }
  insertAdjacentHTML(_, value) { this.innerHTML += value; }
  append() {}
}
function get(selector) {
  if (!nodes.has(selector)) nodes.set(selector, new Element());
  return nodes.get(selector);
}
const document = {
  body: new Element(), querySelector: get, createElement: () => new Element(),
  querySelectorAll: () => [],
  addEventListener(name, fn) {
    if (!listeners.has(name)) listeners.set(name, []);
    listeners.get(name).push(fn);
  }
};
const window = {
  chrome: { webview: { postMessage: value => commands.push(value), addEventListener: (_, fn) => { receive = fn; } } }
};
vm.runInNewContext(readFileSync(process.argv[2], 'utf8'), {
  document, window, localStorage: { getItem: () => null }
});
const first = {
  serial: 'first', name: 'REDMI', availability: 'device',
  state: { connected: true, running: true, streaming: true, canStopSession: true, mode: 'extend', cursorSizePercent: 150,
    hasWindowsTarget: true, canSwitchInWindows: true }
};
const second = {
  serial: 'second', name: '<平板>', availability: 'device',
  state: { connected: true, canStartSession: true, cursorSizePercent: 100, canPreview: true, canCreateVdd: true,
    copySource: 'screen-a', copySources: [{ id: 'screen-a', label: '电脑主屏' }] }
};
const state = (selectedDevice, devices, availableDevices = [], extensionCount = 1) => receive({
  data: { dashboard: true, selectedDevice, devices, availableDevices, extensionCount }
});
const click = dataset => {
  const node = { dataset, disabled: false };
  for (const fn of listeners.get('click') ?? []) fn({ target: { closest: () => node } });
};

state('second', [first, { ...second, commandPending: true }]);
assert.match(get('#device-list').innerHTML ?? '', /REDMI.*正在扩展.*&lt;平板&gt;.*正在发送操作/s,
  'a queued command is visible for its own device while another stream stays live');
assert.match(get('#device-overview').innerHTML, /data-command="mode-copy" disabled/,
  'the pending start must not be submitted twice');
state('second', [first, second]);
assert.match(get('#device-list').innerHTML, /&lt;平板&gt;.*待投屏/s, 'acknowledgement clears the pending label');
const beforeRapidStart = commands.length;
click({ command: 'mode-copy' });
click({ command: 'mode-copy' });
assert.equal(commands.length, beforeRapidStart + 1, 'rapid clicks before a host reply must send only one start');
assert.match(get('#device-overview').innerHTML, /data-command="mode-copy" disabled/,
  'the first click must disable starts immediately');
state('second', [first, second]);
click({ command: 'mode-copy' });
assert.equal(commands.length, beforeRapidStart + 1, 'an older ready snapshot must not unlock the local pending start');
state('second', [first, { ...second, commandPending: true }]);
state('second', [first, second]);
click({ command: 'mode-extend' });
assert.equal(commands.length, beforeRapidStart + 2, 'confirmed completion permits the next action');
state('second', [first, { ...second, commandPending: true }]);
state('second', [first, second]);
state('second', [first, { ...second, commandError: '操作未能确认，请刷新后重试。' }]);
assert.match(get('#device-list').innerHTML, /&lt;平板&gt;.*操作未确认/s,
  'delivery failure must be visible for the affected device');
assert.match(get('#status-bar').innerHTML, /操作未能确认，请刷新后重试。/,
  'the stale ready snapshot must not hide the delivery error');
state('first', [{ ...first, commandError: '操作未能确认，请刷新后重试。' }, second]);
assert.match(get('#device-list').innerHTML, /REDMI.*正在扩展/s,
  'a settings-command failure does not mean an active stream stopped');
assert.match(get('#status-bar').innerHTML, /操作未能确认，请刷新后重试。/,
  'an active stream must still surface a failed control command');

state('first', [first], [second]);
assert.match(get('#device-list').innerHTML ?? '', /REDMI.*正在扩展/s, 'show the active device and its real mode');
assert.match(get('#device-overview').innerHTML, /ui\/copy-screen\.png/);
assert.match(get('#device-overview').innerHTML, /ui\/extend-screen\.png/);
assert.match(get('#device-overview').innerHTML, /data-command="display-settings">在 Windows 切换/);
const mi8 = { ...first, name: '小米 8', state: { ...first.state,
  deviceResolution: '2248×1080', deviceRefresh: '60', resolution: '2772x1280', vddMode: '240 Hz' } };
state('first', [mi8]);
assert.match(get('#device-overview').innerHTML, /2248×1080 · 60 Hz/);
assert.doesNotMatch(get('#device-overview').innerHTML, /<p>[^<]*(2772|240 Hz)/);
state('first', [{ ...mi8, state: { ...mi8.state, deviceResolution: '1080×2248' } }]);
assert.match(get('#device-overview').innerHTML, /1080×2248 · 60 Hz/);
click({ command: 'display-settings' });
assert.equal(commands.at(-1), 'display-settings', 'the running mode opens Windows display settings');
state('first', [{ ...first, state: { ...first.state, mode: 'copy' } }]);
assert.match(get('#device-list').innerHTML, /REDMI.*正在复制/s, 'track a clone selected by Windows');
state('first', [{ ...first, state: { ...first.state, mode: 'copy', hasWindowsTarget: false, canSwitchInWindows: false } }]);
assert.match(get('#device-overview').innerHTML, /直接投送/);
assert.doesNotMatch(get('#device-overview').innerHTML, /在 Windows 切换/);
state('first', [{ ...first, state: { ...first.state, streaming: false } }], [second]);
assert.match(get('#device-list').innerHTML, /REDMI.*等待出画/s, 'show that the device has yet to display the stream');
assert.match(get('#device-overview').innerHTML, /data-command="stop"/, 'starting a session can still be cancelled');
assert.doesNotMatch(get('#status-bar').innerHTML, /dashboard-live-dot/);
click({ command: 'toggle-device-picker' });
assert.match(get('#device-picker').innerHTML, /&lt;平板&gt;/, 'show escaped available devices only after opening the picker');
assert.equal(commands.at(-1), 'refresh-devices');
click({ addDevice: 'second' });
assert.equal(commands.at(-1), 'add-device|second', 'adding does not start a session');
state('second', [first, second]);
assert.match(get('#device-list').innerHTML, /REDMI.*正在扩展.*&lt;平板&gt;.*待投屏/s);
assert.match(get('#device-overview').innerHTML, /data-select="cursor"/, 'cursor size is available without opening advanced settings');
assert.match(get('#device-overview').innerHTML, /data-select="copy-source"/, 'the copy source is visible before starting');
assert.match(get('#device-overview').innerHTML, /data-select="resolution"/, 'extension resolution is visible before starting');
assert.doesNotMatch(get('#advanced-content').innerHTML, /data-command="start"|启动所选显示器/, 'there is no second start action');
assert.match(get('#advanced-content').innerHTML, /data-select="copy-resolution".*data-select="copy-fps"/s,
  'copy video limits have separate controls');
assert.match(get('#advanced-content').innerHTML, /Parsec VDD/);
assert.doesNotMatch(get('#device-overview').innerHTML, /data-command="preview-copy" disabled/);
click({ command: 'preview-copy' });
assert.equal(commands.at(-1), 'device|second|preview-copy');
state('second', [first, { ...second, commandPending: true }]);
click({ device: 'first' });
assert.equal(commands.at(-1), 'select-device|first', 'a running stream must allow sidebar switching');
state('first', [first, second]);
click({ command: 'stop' });
assert.equal(commands.at(-1), 'device|first|stop', 'stop is addressed to the displayed device');
state('second', [first, second], [], 2);
assert.match(get('#device-overview').innerHTML, /data-command="mode-extend" disabled/, 'the extension limit only disables extension');
assert.doesNotMatch(get('#device-overview').innerHTML, /data-command="mode-copy" disabled/);
assert.match(get('#status-bar').innerHTML, /独立扩展 2 \/ 2/);
state('second', [first, { ...second, state: { ...second.state, hasOwnedVdd: true, canRemoveVdd: true } }], [], 2);
assert.doesNotMatch(get('#device-overview').innerHTML, /data-command="mode-extend" disabled/, 'reuse the selected device’s reserved extension');
state('second', [first, { ...second, availability: 'offline', state: null }]);
assert.match(get('#device-list').innerHTML, /is-offline[^>]+data-device="second"[^>]*>.*&lt;平板&gt;.*离线/s,
  'remember and visually dim an unplugged device');
assert.match(get('#device-overview').innerHTML, /data-command="forget-device"/);
state('second', [first, { ...second, state: { canStartSession: false, adbProblem: 'app_missing' } }]);
assert.match(get('#device-overview').innerHTML, /未安装 WireView/);
assert.match(get('#device-overview').innerHTML, /data-command="mode-copy" disabled/);
assert.doesNotMatch(get('#status-bar').innerHTML, /dashboard-live-dot/);
state('second', [{ ...second, state: { ...second.state, status: '状态：已连接，正在准备视频' } }]);
assert.doesNotMatch(get('#status-bar').innerHTML, /正在准备视频/, 'idle state must not reuse the previous transport status');
state('second', [{ ...second, state: { ...second.state, running: true, canStopSession: true,
  sessionPhase: 'paused', adbProblem: 'app_not_running' } }]);
assert.match(get('#device-overview').innerHTML, /已暂停.*自动恢复/s, 'a background interruption remains a cancellable session');
assert.doesNotMatch(get('#device-overview').innerHTML, /data-command="stop" disabled/);
const copying = { ...first, state: { ...first.state, mode: 'copy', copySource: 'screen-a' } };
state('second', [copying, { ...second, state: { ...second.state, copySource: 'screen-a' } }]);
assert.match(get('#device-overview').innerHTML, /data-command="mode-copy" disabled/,
  'two devices cannot start copying the same source');
state('second', [copying, { ...second, state: { ...second.state, copySource: 'screen-b' } }]);
assert.doesNotMatch(get('#device-overview').innerHTML, /data-command="mode-copy" disabled/,
  'copying different sources remains available');
state('second', [{ ...second, state: { ...second.state, copySource: '' } }]);
assert.match(get('#device-overview').innerHTML, /请选择复制来源/);
assert.match(get('#device-overview').innerHTML, /复制来源已不可用/,
  'an unavailable copy source must explain why copying is disabled');
assert.match(get('#device-overview').innerHTML, /data-command="mode-copy" disabled/,
  'an unavailable source must require selection before copying');
const screens = [
  { number: 1, source: 'screen-a', path: 'panel', x: 0, y: 0, width: 2560, height: 1600, primary: true },
  { number: 2, source: 'hdmi', path: 'external', x: -1920, y: 300, width: 1920, height: 1080 },
  { number: 3, source: 'virtual', path: 'tablet', x: 2560, y: 0, width: 2560, height: 1600 }
];
const layoutDevice = { ...second, serial: 'layout', operationMode: 'extend',
  state: { ...second.state, layoutTarget: 'virtual', layoutMonitor: 'tablet' } };
const layoutState = (device, layout = screens) => receive({ data: {
  dashboard: true, selectedDevice: device.serial, devices: [device], availableDevices: [],
  displayLayout: layout, extensionCount: 0
} });
layoutState({ ...layoutDevice, availability: 'offline' });
assert.match(get('#device-overview').innerHTML, /dashboard-screen-layout/,
  'an unplugged but uncancelled operation keeps its screen layout');
assert.match(get('#device-overview').innerHTML, /dashboard-configuration" hidden/);
assert.match(get('#device-overview').innerHTML, /data-command="stop"/,
  'an operation without an active worker must remain cancellable');
assert.match(get('#device-overview').innerHTML, /data-screen="3"[^>]*is-current/);
assert.doesNotMatch(get('#device-overview').innerHTML, /NaN|Infinity/);
assert.equal(get('#advanced-settings').hidden, true);
layoutState(layoutDevice);
assert.match(get('#device-overview').innerHTML, /右侧/);
assert.match(get('#device-overview').innerHTML, /&lt;平板&gt;/);
click({ command: 'identify-screens' });
assert.equal(commands.at(-1), 'identify-screens', 'identify addresses real Windows screens globally');
click({ command: 'stop' });
assert.doesNotMatch(get('#device-overview').innerHTML, /dashboard-screen-layout/,
  'cancel immediately restores settings before the backend reply');
layoutState({ ...layoutDevice, operationMode: '' });
click({ command: 'mode-extend' });
assert.match(get('#device-overview').innerHTML, /dashboard-screen-layout/,
  'starting immediately replaces the settings');
layoutState({ ...layoutDevice, operationMode: '', commandPending: true });
assert.match(get('#device-overview').innerHTML, /dashboard-screen-layout/,
  'an older pending snapshot must not erase the requested view');
layoutState(layoutDevice);
state('second', [layoutDevice, second]);
assert.doesNotMatch(get('#device-overview').innerHTML, /dashboard-screen-layout/,
  'another idle device keeps its own settings');
layoutState({ ...layoutDevice, operationMode: 'copy', state: { ...layoutDevice.state,
  mode: 'copy', copySource: 'screen-a' } });
assert.match(get('#device-overview').innerHTML, /复制来源：1/);
layoutState(layoutDevice, [screens[0], { ...screens[2], source: 'screen-a', x: 0 }]);
assert.match(get('#device-overview').innerHTML, /data-screen="1\|3"/,
  'Windows duplicate targets share one tile rather than inventing a separate desktop');
layoutState({ ...layoutDevice, availability: 'offline', state: {} }, []);
assert.match(get('#device-overview').innerHTML, /暂时无法读取屏幕位置/);
assert.match(get('#device-overview').innerHTML, /data-command="stop"/);
console.log('Unified device page contracts passed');
