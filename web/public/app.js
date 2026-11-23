'use strict';

const $ = (id) => document.getElementById(id);

let snap = {
  connected: false,
  aps: [],
  stas: [],
  targets: [],
  clientTargets: [],
  btTargets: [],
  btDevices: [],
  log: [],
};

const selectedAps = new Set();
const selectedStas = new Set();
let lastConfigJson = '';

function esc(v) {
  return String(v == null ? '' : v)
    .replace(/&/g, '&amp;').replace(/</g, '&lt;')
    .replace(/>/g, '&gt;').replace(/"/g, '&quot;');
}

function pushLocalLog(dir, text) {
  if (!Array.isArray(snap.log)) snap.log = [];
  snap.log.push({ ts: new Date().toISOString().slice(11, 23), dir, text });
  if (snap.log.length > 200) snap.log.shift();
  renderLog();
}

async function api(url, options = {}) {
  const res = await fetch(url, {
    headers: { 'Content-Type': 'application/json' },
    ...options,
  });
  const data = await res.json().catch(() => ({}));
  if (!res.ok) throw new Error(data.error || `HTTP ${res.status}`);
  return data;
}

async function send(cmd) {
  try {
    await api('/api/command', { method: 'POST', body: JSON.stringify(cmd) });
    pushLocalLog('out', JSON.stringify(cmd));
  } catch (err) {
    pushLocalLog('err', err.message);
  }
}

async function refreshPorts() {
  try {
    const ports = await api('/api/ports');
    const sel = $('port');
    const previous = sel.value;
    sel.innerHTML = '';
    for (const p of ports) {
      const opt = document.createElement('option');
      opt.value = p.path;
      opt.textContent = `${p.path}${p.vendorId ? ` [${p.vendorId}:${p.productId}]` : ''}`;
      sel.appendChild(opt);
    }
    if ([...sel.options].some((o) => o.value === previous)) sel.value = previous;
    else if (ports.some((p) => p.path === '/dev/ttyUSB0')) sel.value = '/dev/ttyUSB0';
  } catch (err) {
    pushLocalLog('err', `ports: ${err.message}`);
  }
}

async function doConnect() {
  const portPath = $('port').value || '/dev/ttyUSB0';
  const baudRate = Number($('baud').value || 921600);
  try {
    snap = await api('/api/connect', {
      method: 'POST',
      body: JSON.stringify({ portPath, baudRate }),
    });
    maybeRenderConfig();
    renderAll();
  } catch (err) {
    pushLocalLog('err', `connect: ${err.message}`);
  }
}

async function doDisconnect() {
  try {
    await api('/api/disconnect', { method: 'POST', body: '{}' });
    snap.connected = false;
    renderAll();
  } catch (err) {
    pushLocalLog('err', `disconnect: ${err.message}`);
  }
}

function upsert(list, key, value) {
  if (!key) return;
  const i = list.findIndex((x) => (x.bssid || x.addr || x.a || x.b) === key);
  if (i >= 0) list[i] = { ...list[i], ...value };
  else list.push(value);
  if (list.length > 240) list.shift();
}

function handleMsg(msg) {
  switch (msg.type) {
    case 'hello':
      snap.info = msg;
      snap.aps = [];
      snap.stas = [];
      selectedAps.clear();
      selectedStas.clear();
      break;
    case 'config':
      snap.config = msg;
      // Firmware pasif tidak mengirim field ini — jangan timpa target lokal.
      if (Array.isArray(msg.targets)) snap.targets = msg.targets;
      if (Array.isArray(msg.client_targets)) snap.clientTargets = msg.client_targets;
      if (Array.isArray(msg.bt_targets)) snap.btTargets = msg.bt_targets;
      break;
    case 'status':
      snap.status = msg;
      break;
    case 'wifi':
    case 'ap':
      upsert(snap.aps, msg.bssid || msg.b, msg);
      break;
    case 'client':
    case 'sta':
      upsert(snap.stas, msg.addr || msg.a, msg);
      break;
    case 'bt_device':
      upsert(snap.btDevices, msg.addr, msg);
      break;
    case 'done':
      if (msg.kind === 'bt_scan') pushLocalLog('in', `bt_scan selesai: ${msg.count} perangkat`);
      return;
    case 'rsp':
      pushLocalLog(msg.ok ? 'in' : 'err', `${msg.cmd || '?'}: ${msg.msg || ''}`);
      return;
    default:
      return;
  }
  maybeRenderConfig();
  renderAll();
}

function maybeRenderConfig() {
  const json = JSON.stringify(snap.config || null);
  if (json === lastConfigJson) return;
  lastConfigJson = json;
  renderConfigFromSnap();
}

function renderConfigFromSnap() {
  const p = (snap.config && snap.config.params) || {};
  const setVal = (id, value) => {
    if (value !== undefined && value !== null) $(id).value = value;
  };
  setVal('cfg-channel', p.channel);
  setVal('cfg-interval', p.interval_ms ?? p.wifi_dwell_ms);
  setVal('cfg-burst', p.burst);
  setVal('cfg-gap', p.burst_gap_ms);
  setVal('cfg-hop-ms', p.hop_ms);
  setVal('cfg-rssi', p.rssi_min);
  if (p.reason !== undefined) $('cfg-reason').value = String(p.reason);
  if (p.hop !== undefined) $('cfg-hop').checked = !!p.hop;
  if (p.broadcast !== undefined) $('cfg-broadcast').checked = !!p.broadcast;
  if (p.disassoc !== undefined) $('cfg-disassoc').checked = !!p.disassoc;

  setVal('ble-mode', p.ble_mode);
  setVal('ble-adv-ms', p.ble_adv_ms);
  setVal('ble-gap-ms', p.ble_burst_gap_ms);
  setVal('ble-power', p.ble_power);
  setVal('bt-mode', p.bt_mode);
  setVal('bt-hold-ms', p.bt_hold_ms);
  setVal('bt-gap-ms', p.bt_gap_ms);
  setVal('bt-scan-ms', p.bt_scan_ms);
  if (p.bt_auto_add !== undefined) $('bt-auto-add').checked = !!p.bt_auto_add;
}

function isJammerFirmware() {
  const params = (snap.config && snap.config.params) || {};
  return (snap.info && Number(snap.info.proto) >= 2) ||
    Object.prototype.hasOwnProperty.call(params, 'interval_ms');
}

function renderConn() {
  $('conn-dot').classList.toggle('on', !!snap.connected);
  $('conn-label').textContent = snap.connected ? 'CONNECTED' : 'OFFLINE';

  const jammer = isJammerFirmware();
  const note = $('fw-note');
  note.textContent = jammer
    ? 'JAMMER FIRMWARE AKTIF — TX siap'
    : 'FIRMWARE PASIF TERDETEKSI — flash jammer-firmware untuk deauth';
  note.classList.toggle('warn', !jammer);
  $('btn-start').disabled = !jammer;
  $('btn-stop').disabled = !jammer;
  $('btn-apply').disabled = !jammer;
  $('btn-ble-start').disabled = !jammer;
  $('btn-ble-stop').disabled = !jammer;
  $('btn-bt-start').disabled = !jammer;
  $('btn-bt-stop').disabled = !jammer;
}

function renderTables() {
  const apBody = $('ap-table').querySelector('tbody');
  apBody.innerHTML = snap.aps.map((a) => {
    const key = a.bssid || a.b;
    if (!key) return '';
    return `<tr>
      <td><input type="checkbox" data-ap="${esc(key)}" ${selectedAps.has(key) ? 'checked' : ''}></td>
      <td title="${esc(key)}">${esc(key)}</td>
      <td title="${esc(a.ssid || (a.hidden ? '<hidden>' : ''))}">${esc(a.ssid || (a.hidden ? '<hidden>' : ''))}</td>
      <td>${esc(a.ch)}</td><td>${esc(a.rssi)}</td><td>${esc(a.auth || a.sec || '')}</td>
    </tr>`;
  }).join('');
  apBody.querySelectorAll('input[data-ap]').forEach((box) => {
    box.addEventListener('change', () => {
      if (box.checked) selectedAps.add(box.dataset.ap);
      else selectedAps.delete(box.dataset.ap);
    });
  });

  const staBody = $('sta-table').querySelector('tbody');
  staBody.innerHTML = snap.stas.map((s) => {
    const key = s.addr || s.a;
    if (!key) return '';
    return `<tr>
      <td><input type="checkbox" data-sta="${esc(key)}" ${selectedStas.has(key) ? 'checked' : ''}></td>
      <td title="${esc(key)}">${esc(key)}</td>
      <td title="${esc(s.bssid || s.b)}">${esc(s.bssid || s.b)}</td>
      <td>${esc(s.ch)}</td><td>${esc(s.rssi)}</td>
    </tr>`;
  }).join('');
  staBody.querySelectorAll('input[data-sta]').forEach((box) => {
    box.addEventListener('change', () => {
      if (box.checked) selectedStas.add(box.dataset.sta);
      else selectedStas.delete(box.dataset.sta);
    });
  });

  $('ap-count').textContent = snap.aps.length;
  $('sta-count').textContent = snap.stas.length;
}

function renderTargets() {
  const targets = Array.isArray(snap.targets) ? snap.targets : [];
  $('target-count').textContent = targets.length;
  $('target-chips').innerHTML = targets.length
    ? targets.map((t) => `<span class="chip">${esc(t)}<button data-remove="${esc(t)}" title="remove">&times;</button></span>`).join('')
    : '<span class="muted">no targets</span>';
  $('target-chips').querySelectorAll('button[data-remove]').forEach((btn) => {
    btn.addEventListener('click', () => {
      const next = targets.filter((t) => t !== btn.dataset.remove);
      snap.targets = next;
      send({ cmd: 'targets', targets: next });
      renderTargets();
    });
  });
}

function renderStats() {
  const st = snap.status || {};
  const s = st.stats || {};
  const jam = st.jamming === true;
  $('st-jam').textContent = jam ? 'ACTIVE' : (st.running ? String(st.mode || 'RUN').toUpperCase() : 'IDLE');
  $('st-jam').classList.toggle('active', jam);
  $('st-ch').textContent = st.channel ?? st.wifi_channel ?? '-';
  $('st-frames').textContent = s.frames ?? s.wifi_frames ?? 0;
  $('st-unique').textContent = `${s.aps ?? s.wifi_unique ?? 0} / ${s.clients ?? s.ble_unique ?? 0}`;
  $('st-tx').textContent = s.tx ?? 0;
  $('st-err').textContent = s.tx_errors ?? 0;
  $('st-drop').textContent = s.rx_dropped ?? s.dropped ?? 0;
}

function renderLog() {
  $('log').innerHTML = (snap.log || []).map((l) =>
    `<span class="${esc(l.dir)}">${esc(l.ts)} ${esc(l.dir)}</span> ${esc(l.text)}`
  ).join('\n');
  $('log').scrollTop = $('log').scrollHeight;
}

function renderExtra() {
  const ble = (snap.status && snap.status.ble) || {};
  const bt = (snap.status && snap.status.bt) || {};
  $('ble-state').textContent = ble.running ? 'ACTIVE' : 'IDLE';
  $('ble-state').classList.toggle('active', !!ble.running);
  $('ble-sent').textContent = ble.sent || 0;
  $('ble-err').textContent = ble.errors || 0;

  $('bt-state').textContent = bt.running ? 'ACTIVE' : 'IDLE';
  $('bt-state').classList.toggle('active', !!bt.running);
  $('bt-sent').textContent = bt.sent || 0;
  $('bt-err').textContent = bt.errors || 0;
  $('bt-scan-state').textContent = bt.scanning ? 'SCANNING' : 'IDLE';
  $('bt-scan-state').classList.toggle('active', !!bt.scanning);
  $('bt-found').textContent = bt.devices || (snap.btDevices || []).length || 0;
  $('kpi-bt').textContent = `${ble.sent || 0} / ${bt.sent || 0}`;

  const btTargets = Array.isArray(snap.btTargets) ? snap.btTargets : [];
  $('bt-target-count').textContent = btTargets.length;
  $('bt-target-chips').innerHTML = btTargets.length
    ? btTargets.map((m) => `<span class="chip">${esc(m)}<button data-bt-remove="${esc(m)}" title="remove">&times;</button></span>`).join('')
    : '<span class="muted">belum ada target</span>';
  $('bt-target-chips').querySelectorAll('button[data-bt-remove]').forEach((btn) => {
    btn.addEventListener('click', () => {
      const next = btTargets.filter((m) => m !== btn.dataset.btRemove);
      snap.btTargets = next;
      send({ cmd: 'bt_targets', targets: next });
      renderExtra();
    });
  });

  const devices = Array.isArray(snap.btDevices) ? snap.btDevices : [];
  $('bt-device-count').textContent = devices.length;
  $('bt-devices').querySelector('tbody').innerHTML = devices.slice().reverse().map((d) => `
    <tr>
      <td title="${esc(d.name || '')}">${esc(d.name || '<unknown>')}</td>
      <td>${esc(d.addr)}</td>
      <td>${esc(d.rssi)}</td>
      <td><button class="small ghost" data-bt-device="${esc(d.addr)}">ADD</button></td>
    </tr>`).join('');
  $('bt-devices').querySelectorAll('button[data-bt-device]').forEach((btn) => {
    btn.addEventListener('click', () => {
      const set = new Set(Array.isArray(snap.btTargets) ? snap.btTargets : []);
      set.add(btn.dataset.btDevice);
      snap.btTargets = [...set];
      send({ cmd: 'bt_targets', targets: snap.btTargets });
      renderExtra();
      pushLocalLog('in', `target BT ditambah: ${btn.dataset.btDevice}`);
    });
  });
}

function renderAll() {
  renderConn();
  renderTables();
  renderTargets();
  renderStats();
  renderExtra();
  renderLog();
}

/* ---- actions ---------------------------------------------------------- */

function applyParams() {
  const params = {
    channel: Number($('cfg-channel').value),
    interval_ms: Number($('cfg-interval').value),
    burst: Number($('cfg-burst').value),
    burst_gap_ms: Number($('cfg-gap').value),
    hop_ms: Number($('cfg-hop-ms').value),
    reason: Number($('cfg-reason').value),
    rssi_min: Number($('cfg-rssi').value),
    hop: $('cfg-hop').checked,
    broadcast: $('cfg-broadcast').checked,
    disassoc: $('cfg-disassoc').checked,
  };
  send({ cmd: 'set', params });
}

function addSelected() {
  const merged = new Set(Array.isArray(snap.targets) ? snap.targets : []);
  selectedAps.forEach((mac) => merged.add(mac));
  snap.targets = [...merged];
  send({ cmd: 'targets', targets: snap.targets });
  selectedAps.clear();
  renderTargets();
  renderTables();
}

function addManual() {
  const mac = $('manual-mac').value.trim().toUpperCase();
  if (!/^([0-9A-F]{2}:){5}[0-9A-F]{2}$/.test(mac)) {
    pushLocalLog('err', 'invalid MAC');
    return;
  }
  const merged = new Set(Array.isArray(snap.targets) ? snap.targets : []);
  merged.add(mac);
  snap.targets = [...merged];
  $('manual-mac').value = '';
  send({ cmd: 'targets', targets: snap.targets });
  renderTargets();
}

function startJam() {
  send({
    cmd: 'jam_start',
    targets: Array.isArray(snap.targets) ? snap.targets : [],
    client_targets: [...selectedStas],
  });
}

function bleStart() {
  const params = {
    ble_mode: Number($('ble-mode').value),
    ble_adv_ms: Number($('ble-adv-ms').value),
    ble_burst_gap_ms: Number($('ble-gap-ms').value),
    ble_power: Number($('ble-power').value),
  };
  const hex = $('ble-custom').value.trim();
  send(hex ? { cmd: 'ble_start', params, payload: hex } : { cmd: 'ble_start', params });
}

function btAdd() {
  const mac = $('bt-mac').value.trim().toUpperCase();
  if (!/^([0-9A-F]{2}:){5}[0-9A-F]{2}$/.test(mac)) {
    pushLocalLog('err', 'invalid BT MAC');
    return;
  }
  const set = new Set(Array.isArray(snap.btTargets) ? snap.btTargets : []);
  set.add(mac);
  snap.btTargets = [...set];
  $('bt-mac').value = '';
  send({ cmd: 'bt_targets', targets: snap.btTargets });
  renderExtra();
}

function btScanStart() {
  send({
    cmd: 'bt_scan_start',
    params: {
      bt_scan_ms: Number($('bt-scan-ms').value),
      bt_auto_add: $('bt-auto-add').checked,
    },
  });
}

function btStart() {
  send({
    cmd: 'bt_start',
    params: {
      bt_mode: Number($('bt-mode').value),
      bt_hold_ms: Number($('bt-hold-ms').value),
      bt_gap_ms: Number($('bt-gap-ms').value),
    },
    targets: Array.isArray(snap.btTargets) ? snap.btTargets : [],
  });
}

/* ---- wiring ----------------------------------------------------------- */

$('btn-refresh-ports').addEventListener('click', refreshPorts);
$('btn-connect').addEventListener('click', doConnect);
$('btn-disconnect').addEventListener('click', doDisconnect);
$('btn-apply').addEventListener('click', applyParams);
$('btn-add-selected').addEventListener('click', addSelected);
$('btn-select-all').addEventListener('click', () => {
  selectedAps.clear();
  snap.aps.forEach((a) => { if (a.bssid || a.b) selectedAps.add(a.bssid || a.b); });
  renderTables();
});
$('btn-add-manual').addEventListener('click', addManual);
$('manual-mac').addEventListener('keydown', (e) => {
  if (e.key === 'Enter') addManual();
});
$('btn-start').addEventListener('click', startJam);
$('btn-stop').addEventListener('click', () => send({ cmd: 'jam_stop' }));
$('btn-ble-start').addEventListener('click', bleStart);
$('btn-ble-stop').addEventListener('click', () => send({ cmd: 'ble_stop' }));
$('btn-bt-scan').addEventListener('click', btScanStart);
$('btn-bt-scan-stop').addEventListener('click', () => send({ cmd: 'bt_scan_stop' }));
$('btn-bt-add').addEventListener('click', btAdd);
$('bt-mac').addEventListener('keydown', (e) => { if (e.key === 'Enter') btAdd(); });
$('btn-bt-start').addEventListener('click', btStart);
$('btn-bt-stop').addEventListener('click', () => send({ cmd: 'bt_stop' }));
$('btn-clear').addEventListener('click', () => {
  snap.aps = [];
  snap.stas = [];
  selectedAps.clear();
  selectedStas.clear();
  send({ cmd: 'clear' });
  renderAll();
});

function initTabs() {
  document.querySelectorAll('.tab').forEach((btn) => {
    btn.addEventListener('click', () => {
      document.querySelectorAll('.tab').forEach((b) => b.classList.remove('active'));
      document.querySelectorAll('.tab-panel').forEach((p) => p.classList.remove('active'));
      btn.classList.add('active');
      const panel = document.querySelector(`.tab-panel[data-panel="${btn.dataset.tab}"]`);
      if (panel) panel.classList.add('active');
    });
  });
}

function startStream() {
  const es = new EventSource('/api/events');
  es.addEventListener('snapshot', (e) => {
    snap = JSON.parse(e.data);
    maybeRenderConfig();
    renderAll();
  });
  es.addEventListener('msg', (e) => handleMsg(JSON.parse(e.data)));
  es.addEventListener('raw', (e) => pushLocalLog('in', JSON.parse(e.data).line));
  es.addEventListener('error', (e) => {
    if (e.data) pushLocalLog('err', JSON.parse(e.data).message);
  });
}

(async function boot() {
  initTabs();
  renderAll();
  startStream();
  await refreshPorts();
  await doConnect();
})();
