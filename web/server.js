'use strict';

const express = require('express');
const path = require('path');
const { SerialPort, ReadlineParser } = require('serialport');

const HOST = process.env.HOST || '0.0.0.0';
const PORT = Number(process.env.PORT || 8080);
const DEFAULT_PORT = process.env.SERIAL_PORT || '/dev/ttyUSB0';
const DEFAULT_BAUD = Number(process.env.BAUD || 921600);

const app = express();
app.use(express.json({ limit: '256kb' }));
app.use(express.static(path.join(__dirname, 'public')));

const state = {
  connected: false,
  portPath: DEFAULT_PORT,
  baudRate: DEFAULT_BAUD,
  info: null,
  config: null,
  status: null,
  aps: new Map(),
  stas: new Map(),
  targets: [],
  clientTargets: [],
  log: [],
};

let serial = null;
let parser = null;
const sseClients = new Set();

const KNOWN = new Set([
  'ping', 'status', 'config', 'set', 'targets', 'clear',
  'jam_start', 'jam_stop', 'reset',
  'mode', 'get_status', 'get_config', 'set_config',
  'scan_wifi', 'scan_ble', 'scan_all', 'stop', 'get_results',
]);

function stamp() {
  return new Date().toISOString().slice(11, 23);
}

function pushLog(dir, text) {
  state.log.push({ ts: stamp(), dir, text });
  if (state.log.length > 200) state.log.shift();
}

function sse(event, payload) {
  const chunk = `event: ${event}\ndata: ${JSON.stringify(payload)}\n\n`;
  for (const res of sseClients) {
    try { res.write(chunk); } catch { sseClients.delete(res); }
  }
}

function snapshot() {
  return {
    connected: state.connected,
    portPath: state.portPath,
    baudRate: state.baudRate,
    info: state.info,
    config: state.config,
    status: state.status,
    aps: [...state.aps.values()],
    stas: [...state.stas.values()],
    targets: state.targets,
    clientTargets: state.clientTargets,
    log: state.log.slice(-80),
  };
}

function emitSnapshot() {
  sse('snapshot', snapshot());
}

function onLine(line) {
  const text = String(line).trim();
  if (!text) return;
  pushLog('in', text);

  let msg;
  try {
    msg = JSON.parse(text);
  } catch {
    sse('raw', { line: text });
    return;
  }

  switch (msg.type) {
    case 'hello':
      state.info = msg;
      state.aps.clear();
      state.stas.clear();
      break;
    case 'config':
      state.config = msg;
      if (Array.isArray(msg.targets)) state.targets = msg.targets;
      if (Array.isArray(msg.client_targets)) state.clientTargets = msg.client_targets;
      break;
    case 'status':
      state.status = msg;
      break;
    case 'wifi':
    case 'ap':
      state.aps.set(msg.bssid || msg.b, msg);
      break;
    case 'client':
    case 'sta':
      state.stas.set(msg.addr || msg.a, msg);
      break;
    case 'rsp':
      break;
    default:
      break;
  }

  sse('msg', msg);
  if (msg.type === 'hello' || msg.type === 'config' || msg.type === 'status') {
    emitSnapshot();
  }
}

function writeCommand(obj) {
  if (!serial || !serial.isOpen) throw new Error('serial port not open');
  const line = JSON.stringify(obj) + '\n';
  pushLog('out', line.trim());
  serial.write(line);
}

async function disconnect() {
  if (!serial) return { connected: false };
  const port = serial;
  serial = null;
  parser = null;
  if (port.isOpen) {
    await new Promise((resolve) => port.close(() => resolve()));
  }
  state.connected = false;
  state.info = null;
  state.status = null;
  state.config = null;
  state.aps.clear();
  state.stas.clear();
  pushLog('sys', 'disconnected');
  emitSnapshot();
  return { connected: false };
}

async function connect(portPath, baudRate) {
  await disconnect();

  const port = new SerialPort({
    path: portPath,
    baudRate,
    autoOpen: false,
  });

  const lineParser = new ReadlineParser({ delimiter: '\n' });
  lineParser.on('data', onLine);
  port.pipe(lineParser);

  await new Promise((resolve, reject) => {
    port.open((err) => (err ? reject(err) : resolve()));
  });

  port.on('error', (err) => {
    pushLog('err', err.message);
    sse('error', { message: err.message });
  });
  port.on('close', () => {
    if (serial === port) {
      serial = null;
      parser = null;
      state.connected = false;
      pushLog('sys', 'port closed');
      emitSnapshot();
    }
  });

  serial = port;
  parser = lineParser;
  state.connected = true;
  state.portPath = portPath;
  state.baudRate = baudRate;
  state.info = null;
  state.status = null;
  state.config = null;
  state.aps.clear();
  state.stas.clear();
  pushLog('sys', `connected ${portPath} @ ${baudRate}`);

  writeCommand({ cmd: 'status' });
  writeCommand({ cmd: 'config' });
  emitSnapshot();
  return snapshot();
}

/* ---- routes ----------------------------------------------------------- */

app.get('/api/state', (_req, res) => res.json(snapshot()));

app.get('/api/ports', async (_req, res, next) => {
  try {
    const ports = await SerialPort.list();
    res.json(ports.map((p) => ({
      path: p.path,
      manufacturer: p.manufacturer || '',
      serialNumber: p.serialNumber || '',
      vendorId: p.vendorId || '',
      productId: p.productId || '',
    })));
  } catch (err) {
    next(err);
  }
});

app.post('/api/connect', async (req, res, next) => {
  try {
    const portPath = String(req.body.portPath || DEFAULT_PORT);
    const baudRate = Number(req.body.baudRate || DEFAULT_BAUD);
    if (!portPath || !Number.isFinite(baudRate)) {
      return res.status(400).json({ error: 'portPath and baudRate required' });
    }
    res.json(await connect(portPath, baudRate));
  } catch (err) {
    next(err);
  }
});

app.post('/api/disconnect', async (_req, res, next) => {
  try {
    res.json(await disconnect());
  } catch (err) {
    next(err);
  }
});

app.post('/api/command', (req, res, next) => {
  try {
    const body = req.body || {};
    if (typeof body.cmd !== 'string') {
      return res.status(400).json({ error: 'cmd must be a string' });
    }
    if (!KNOWN.has(body.cmd)) {
      return res.status(400).json({ error: `unknown command: ${body.cmd}` });
    }
    writeCommand(body);
    res.json({ ok: true, sent: body });
  } catch (err) {
    next(err);
  }
});

app.get('/api/events', (req, res) => {
  res.set({
    'Content-Type': 'text/event-stream',
    'Cache-Control': 'no-cache, no-transform',
    Connection: 'keep-alive',
    'X-Accel-Buffering': 'no',
  });
  res.flushHeaders();
  sseClients.add(res);
  res.write(`event: snapshot\ndata: ${JSON.stringify(snapshot())}\n\n`);

  req.on('close', () => sseClients.delete(res));
});

app.use((err, _req, res, _next) => {
  pushLog('err', err.message);
  sse('error', { message: err.message });
  res.status(500).json({ error: err.message });
});

const server = app.listen(PORT, HOST, () => {
  pushLog('sys', `console listening http://${HOST}:${PORT}`);
  console.log(`WRECON console: http://${HOST}:${PORT}`);
});

const heartbeat = setInterval(() => {
  for (const res of sseClients) {
    try { res.write(': keepalive\n\n'); } catch { sseClients.delete(res); }
  }
}, 15000);
heartbeat.unref();

function shutdown() {
  clearInterval(heartbeat);
  server.close(() => process.exit(0));
}
process.on('SIGINT', shutdown);
process.on('SIGTERM', shutdown);
