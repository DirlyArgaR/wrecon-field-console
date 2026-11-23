#include "state.h"
#include "wifi_passive.h"
#include "ble_passive.h"

ScanConfig   g_cfg;
RunMode      g_mode = MODE_OFF;
volatile bool g_running = false;

const char *modeStr(RunMode m) {
  switch (m) {
    case MODE_WIFI: return "wifi";
    case MODE_BLE:  return "ble";
    case MODE_BOTH: return "both";
    default:        return "off";
  }
}

RunMode parseMode(const char *s) {
  if (!s) return MODE_OFF;
  if (!strcmp(s, "wifi")) return MODE_WIFI;
  if (!strcmp(s, "ble"))  return MODE_BLE;
  if (!strcmp(s, "both")) return MODE_BOTH;
  if (!strcmp(s, "off"))  return MODE_OFF;
  return MODE_OFF;
}

void applyMode(RunMode m) {
  if (g_mode == MODE_WIFI || g_mode == MODE_BOTH) wifiStop();
  if (g_mode == MODE_BLE  || g_mode == MODE_BOTH) bleStop();
  g_mode = m;
  g_running = (m != MODE_OFF);
  if (m == MODE_WIFI || m == MODE_BOTH) wifiStart(g_cfg);
  if (m == MODE_BLE  || m == MODE_BOTH) bleStart(g_cfg);
}

void applyConfig() {
  RunMode m = g_mode;
  applyMode(MODE_OFF);
  if (m != MODE_OFF) applyMode(m);
}
