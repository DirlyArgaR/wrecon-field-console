#include "cmd.h"
#include "state.h"
#include "link.h"

#include <ArduinoJson.h>
#include <string.h>

static JsonDocument s_rxDoc;

/* ---- ack ke app -------------------------------------------------------- */

static void ack(bool ok, const char *msg, const char *srcCmd) {
  JsonDocument d;
  d["type"] = "rsp";
  d["ok"]   = ok;
  d["cmd"]  = srcCmd;
  d["msg"]  = msg ? msg : (ok ? "ok" : "invalid");
  if (ok) d["mode"] = modeStr(g_mode);
  linkEmitJson(d);
}

/* ---- parsers ------------------------------------------------------------ */

static bool getBounded(JsonObject params, const char *key, long lo, long hi, long *out) {
  JsonVariant v = params[key];
  if (!v.is<long>()) return false;
  long x = v.as<long>();
  if (x < lo || x > hi) return false;
  *out = x;
  return true;
}

static void setParams(JsonObject params) {
  long x;
  bool changed = false;

  if (getBounded(params, "wifi_dwell_ms", 10, 5000, &x)) {
    if (g_cfg.wifiDwellMs != x) { g_cfg.wifiDwellMs = x; changed = true; }
  }
  if (getBounded(params, "ble_scan_ms", 50, 30000, &x)) {
    if (g_cfg.bleScanMs != x) { g_cfg.bleScanMs = x; changed = true; }
  }
  if (getBounded(params, "ble_interval_ms", 20, 4000, &x)) {
    if (g_cfg.bleIntervalMs != x) { g_cfg.bleIntervalMs = x; changed = true; }
  }
  if (getBounded(params, "ble_window_ms", 20, 4000, &x)) {
    if (g_cfg.bleWindowMs != x) { g_cfg.bleWindowMs = x; changed = true; }
  }
  if (getBounded(params, "repeat_ms", 0, 60000, &x)) {
    if (g_cfg.repeatMs != x) { g_cfg.repeatMs = x; changed = true; }
  }
  if (getBounded(params, "dedup_ms", 0, 600000, &x)) {
    if (g_cfg.dedupMs != x) { g_cfg.dedupMs = x; changed = true; }
  }
  if (getBounded(params, "stats_ms", 0, 60000, &x)) {
    if (g_cfg.statsMs != x) { g_cfg.statsMs = x; changed = true; }
  }
  if (getBounded(params, "rssi_min", -120, 0, &x)) {
    if (g_cfg.rssiMin != x) { g_cfg.rssiMin = x; changed = true; }
  }
  if (params["channels"].is<JsonArray>()) {
    // Semantik: daftar channel yang AKTIF. Kosong = semua channel aktif.
    JsonArray arr = params["channels"].as<JsonArray>();
    bool want[FW_CHANNEL_MAX + 1] = {};
    for (size_t k = 0; k < arr.size(); k++) {
      int c = arr[k].as<int>();
      if (c >= 1 && c <= FW_CHANNEL_MAX) want[c] = true;
    }
    bool allOff = true;
    for (int c = 1; c <= FW_CHANNEL_MAX; c++) if (want[c]) allOff = false;
    if (allOff) { for (int c = 1; c <= FW_CHANNEL_MAX; c++) want[c] = true; }

    for (int c = 1; c <= FW_CHANNEL_MAX; c++) {
      if (g_cfg.channels[c] != want[c]) { g_cfg.channels[c] = want[c]; changed = true; }
    }
  }

  if (changed) applyConfig();

  ack(true, "params applied", "set");
  linkEmitConfig();
}

/* ---- dispatch ----------------------------------------------------------- */

void cmdHandleLine(const char *line) {
  DeserializationError err = deserializeJson(s_rxDoc, line);
  if (err) {
    ack(false, "bad json", "");
    return;
  }

  const char *cmd = s_rxDoc["cmd"] | "";

  if (!strcmp(cmd, "ping")) {
    ack(true, "pong", cmd);
    return;
  }
  if (!strcmp(cmd, "reset")) {
    applyMode(MODE_OFF);
    g_cfg = ScanConfig();
    ack(true, "reset+off", cmd);
    linkEmitConfig();
    return;
  }
  if (!strcmp(cmd, "status")) {
    ack(true, "ok", cmd);
    return;
  }
  if (!strcmp(cmd, "mode")) {
    RunMode m = parseMode(s_rxDoc["mode"] | "");
    applyMode(m);
    ack(true, "ok", cmd);
    linkEmitConfig();
    return;
  }
  if (!strcmp(cmd, "set")) {
    setParams(s_rxDoc["params"].as<JsonObject>());
    return;
  }

  ack(false, "unknown cmd", cmd);
}
