#include "link.h"
#include "state.h"
#include "cmd.h"
#include "wifi_passive.h"
#include "ble_passive.h"

#include <ArduinoJson.h>

/* ---- ring keluar (produsen: loop task saja) ---------------------------- */

#define OUT_RING_LEN 128

static EventMsg s_out[OUT_RING_LEN];
static volatile uint16_t s_head = 0;
static volatile uint16_t s_tail = 0;
static uint32_t s_dropped = 0;

static char s_rx[RX_LINE_MAX];
static size_t s_rxLen = 0;

void linkInit() {
  s_head = s_tail = 0;
  s_rxLen = 0;
}

bool linkHasRoom() {
  return (uint16_t)((s_head + 1) % OUT_RING_LEN) != s_tail;
}

uint32_t linkDropped() { return s_dropped; }

void linkEmitRaw(const char *s) {
  uint16_t next = (uint16_t)((s_head + 1) % OUT_RING_LEN);
  if (next == s_tail) { s_dropped++; return; }
  strncpy(s_out[s_head].json, s, EVENT_MAX_LEN - 1);
  s_out[s_head].json[EVENT_MAX_LEN - 1] = 0;
  s_head = next;
}

void linkEmitJson(JsonDocument &doc) {
  uint16_t next = (uint16_t)((s_head + 1) % OUT_RING_LEN);
  if (next == s_tail) { s_dropped++; return; }
  size_t n = serializeJson(doc, s_out[s_head].json, EVENT_MAX_LEN);
  if (n >= EVENT_MAX_LEN) s_out[s_head].json[EVENT_MAX_LEN - 1] = 0;
  s_head = next;
}

void linkEmitConfig() {
  JsonDocument d;
  d["type"] = "config";
  d["mode"] = modeStr(g_mode);
  JsonObject p = d["params"].to<JsonObject>();
  p["wifi_dwell_ms"]   = g_cfg.wifiDwellMs;
  p["ble_scan_ms"]     = g_cfg.bleScanMs;
  p["ble_interval_ms"] = g_cfg.bleIntervalMs;
  p["ble_window_ms"]   = g_cfg.bleWindowMs;
  p["repeat_ms"]       = g_cfg.repeatMs;
  p["dedup_ms"]        = g_cfg.dedupMs;
  p["stats_ms"]        = g_cfg.statsMs;
  p["rssi_min"]        = g_cfg.rssiMin;
  JsonArray ch = p["channels"].to<JsonArray>();
  for (uint8_t c = 1; c <= FW_CHANNEL_MAX; c++) if (g_cfg.channels[c]) ch.add(c);
  linkEmitJson(d);
}

void linkEmitStatus() {
  JsonDocument d;
  d["type"]    = "status";
  d["ts"]      = millis();
  d["mode"]    = modeStr(g_mode);
  d["running"] = g_running;

  JsonObject s = d["stats"].to<JsonObject>();
  s["wifi_frames"] = wifiFrames();
  s["wifi_unique"] = wifiUniqueCount();
  s["ble_events"]  = bleSeen();
  s["ble_unique"]  = bleUniqueCount();
  s["tx_dropped"]  = linkDropped();

  if (g_mode == MODE_WIFI || g_mode == MODE_BOTH) {
    d["wifi_channel"] = wifiCurrentChannel();
  }
  linkEmitJson(d);
}

/* ---- sisi terima ------------------------------------------------------- */

void linkPoll() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (s_rxLen) {
        s_rx[s_rxLen] = 0;
        cmdHandleLine(s_rx);
        s_rxLen = 0;
      }
    } else if (s_rxLen < (RX_LINE_MAX - 1)) {
      s_rx[s_rxLen++] = c;
    } else {
      s_rxLen = 0;    // baris kebanyakan: drop
      s_dropped++;
    }
  }
}

/* ---- sisi kirim -------------------------------------------------------- */

void linkFlush() {
  while (s_tail != s_head) {
    // Hindari menahan loop saat buffer USB penuh.
    if (Serial.availableForWrite() < 64) return;
    const char *s = s_out[s_tail].json;
    Serial.write((const uint8_t *)s, strlen(s));
    Serial.write('\n');
    s_tail = (uint16_t)((s_tail + 1) % OUT_RING_LEN);
  }
}
