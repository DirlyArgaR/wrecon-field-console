'use strict';

const $ = (id) => document.getElementById(id);

let snap = {
  connected: false,
  aps: [],
  stas: [],
  targets: [],
  clientTargets: [],
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

function renderAll() {
  renderConn();
  renderTables();
  renderTargets();
  renderStats();
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
$('btn-clear').addEventListener('click', () => {
  snap.aps = [];
  snap.stas = [];
  selectedAps.clear();
  selectedStas.clear();
  send({ cmd: 'clear' });
  renderAll();
});

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
  renderAll();
  startStream();
  await refreshPorts();
  await doConnect();
})();
