#include "wifi_passive.h"
#include "state.h"
#include "link.h"

#include <atomic>
#include <string.h>
#include <ArduinoJson.h>
#include <esp_wifi.h>

/* ---- ring frame mentah: WiFi sniff task -> loop task -------------------- */

#define FRAME_MAX 320
#define RING_LEN  24

struct RawFrame {
  uint16_t len;
  int8_t   rssi;
  uint8_t  ch;
  uint8_t  data[FRAME_MAX];
};

static RawFrame s_ring[RING_LEN];
static std::atomic<uint16_t> s_head{0};
static std::atomic<uint16_t> s_tail{0};
static std::atomic<uint32_t> s_frames{0};
static std::atomic<uint32_t> s_ringDrops{0};

/* ---- cache AP unik (dipakai hanya oleh loop task) ----------------------- */

#define CACHE_LEN 96

struct CacheEntry {
  bool     used;
  uint8_t  bssid[6];
  char     ssid[33];
  uint8_t  ch;
  int8_t   rssi;
  uint8_t  auth;      // 0 open, 1 wep, 2 wpa, 3 wpa2
  bool     hidden;
  uint16_t count;
  uint32_t lastSeen;
  uint32_t lastEmit;
};

static CacheEntry s_cache[CACHE_LEN];
static uint16_t   s_evict = 0;
static uint32_t   s_unique = 0;

static const ScanConfig *s_cfg = nullptr;
static bool     s_inited = false;
static bool     s_running = false;
static uint8_t  s_curCh = 1;
static uint8_t  s_hopCursor = 0;
static uint32_t s_lastHop = 0;

/* ---- helper ------------------------------------------------------------ */

static bool macEq(const uint8_t *a, const uint8_t *b) { return memcmp(a, b, 6) == 0; }

static void macToStr(const uint8_t *m, char *out18) {
  snprintf(out18, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
           m[0], m[1], m[2], m[3], m[4], m[5]);
}

static const char *authStr(uint8_t a) {
  switch (a) {
    case 1: return "wep";
    case 2: return "wpa";
    case 3: return "wpa2";
    default: return "open";
  }
}

/* ---- callback promiscuous (konteks WiFi task, bukan ISR) ---------------- */

static void sniffCb(void *buf, wifi_promiscuous_pkt_type_t type) {
  if (type != WIFI_PKT_MGMT) return;
  const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
  const uint8_t *f = pkt->payload;
  if (pkt->rx_ctrl.sig_len < 36) return;

  uint8_t subtype = (f[0] >> 4) & 0x0F;
  if (subtype != 0x08 && subtype != 0x05) return;   // beacon atau probe response

  uint16_t h = s_head.load(std::memory_order_relaxed);
  uint16_t next = (uint16_t)((h + 1) % RING_LEN);
  if (next == s_tail.load(std::memory_order_acquire)) {
    s_ringDrops.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  RawFrame &r = s_ring[h];
  uint16_t n = pkt->rx_ctrl.sig_len;
  if (n > FRAME_MAX) n = FRAME_MAX;
  r.len  = n;
  r.rssi = pkt->rx_ctrl.rssi;
  r.ch   = pkt->rx_ctrl.channel ? pkt->rx_ctrl.channel : s_curCh;
  memcpy(r.data, f, n);

  s_head.store(next, std::memory_order_release);
  s_frames.fetch_add(1, std::memory_order_relaxed);
}

/* ---- parsing beacon ---------------------------------------------------- */

static void processFrame(const RawFrame &r) {
  if (r.len < 36) return;
  const uint8_t *f = r.data;
  const uint8_t *bssid = f + 16;                     // addr3
  uint16_t capab = (uint16_t)f[34] | ((uint16_t)f[35] << 8);
  bool privacy = (capab & 0x0010) != 0;

  char    ssid[33] = {0};
  uint8_t ssidLen  = 0;
  uint8_t ch       = r.ch;
  bool    rsn = false, wpa = false;

  uint16_t i = 36;                                   // offset awal elemen IE
  while ((uint16_t)(i + 2) <= r.len) {
    uint8_t id = f[i];
    uint8_t l  = f[i + 1];
    if ((uint16_t)(i + 2 + l) > r.len) break;
    const uint8_t *v = f + i + 2;
    switch (id) {
      case 0:   if (l <= 32) { ssidLen = l; memcpy(ssid, v, l); ssid[l] = 0; } break;
      case 3:   if (l >= 1) ch = v[0]; break;
      case 48:  rsn = true; break;
      case 221: if (l >= 4 && v[0] == 0x00 && v[1] == 0x50 && v[2] == 0xF2 && v[3] == 0x01) wpa = true; break;
      default: break;
    }
    i += (uint16_t)(2 + l);
  }

  if (r.rssi < s_cfg->rssiMin) return;

  uint8_t auth = privacy ? (uint8_t)(rsn ? 3 : (wpa ? 2 : 1)) : 0;

  CacheEntry *e = nullptr;
  CacheEntry *freeSlot = nullptr;
  for (uint16_t k = 0; k < CACHE_LEN; k++) {
    if (!s_cache[k].used) { if (!freeSlot) freeSlot = &s_cache[k]; continue; }
    if (macEq(s_cache[k].bssid, bssid)) { e = &s_cache[k]; break; }
  }

  bool isNew = false;
  if (!e) {
    e = freeSlot ? freeSlot : &s_cache[s_evict++ % CACHE_LEN];
    e->used = true;
    memcpy(e->bssid, bssid, 6);
    e->count    = 0;
    e->lastEmit = 0;
    e->rssi     = -127;
    isNew = true;
    s_unique++;
  }

  uint32_t now = millis();
  e->count++;
  e->lastSeen = now;
  e->ch       = ch;
  e->hidden   = (ssidLen == 0);
  if (ssidLen) memcpy(e->ssid, ssid, ssidLen + 1); else e->ssid[0] = 0;

  int8_t oldRssi = e->rssi;
  e->rssi = r.rssi;
  e->auth = auth;

  bool due   = s_cfg->repeatMs && (uint32_t)(now - e->lastEmit) >= s_cfg->repeatMs;
  bool moved = abs((int)r.rssi - (int)oldRssi) >= 6;   // perubahan sinyal nyata
  if (!isNew && !due && !moved) return;
  e->lastEmit = now;

  char mac[18];
  macToStr(e->bssid, mac);

  JsonDocument d;
  d["type"]   = "wifi";
  d["ts"]     = now;
  d["bssid"]  = mac;
  if (!e->hidden) d["ssid"] = e->ssid;
  d["hidden"] = e->hidden;
  d["ch"]     = e->ch;
  d["rssi"]   = e->rssi;
  d["auth"]   = authStr(e->auth);
  d["hits"]   = e->count;
  linkEmitJson(d);
}

/* ---- channel hop ------------------------------------------------------- */

static uint8_t pickNextChannel() {
  for (uint8_t k = 0; k < FW_CHANNEL_MAX; k++) {
    uint8_t c = (uint8_t)((s_hopCursor + k) % FW_CHANNEL_MAX) + 1;   // 1..13
    if (s_cfg->channels[c]) { s_hopCursor = (uint8_t)(c % FW_CHANNEL_MAX); return c; }
  }
  return 1;
}

static void hopTick() {
  uint32_t now = millis();
  uint16_t dwell = s_cfg->wifiDwellMs ? s_cfg->wifiDwellMs : 50;
  if (s_lastHop && (uint32_t)(now - s_lastHop) < dwell) return;
  s_lastHop = now;
  uint8_t c = pickNextChannel();
  if (c == s_curCh) return;
  if (esp_wifi_set_channel(c, WIFI_SECOND_CHAN_NONE) == ESP_OK) s_curCh = c;
}

/* ---- API publik -------------------------------------------------------- */

void wifiStart(const ScanConfig &cfg) {
  s_cfg = &cfg;

  if (!s_inited) {
    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&ic);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();
    // Jangan pakai WIFI_PS_NONE di sini. Espressif issue #9595:
    // mematikan modem sleep WiFi sebelum BT/NimBLE init bikin coex_enable() abort.
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

    wifi_promiscuous_filter_t flt = {};
    flt.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
    esp_wifi_set_promiscuous_filter(&flt);
    esp_wifi_set_promiscuous_rx_cb(&sniffCb);
    s_inited = true;
  }

  s_head.store(0);
  s_tail.store(0);
  s_hopCursor = 0;
  s_lastHop  = 0;
  s_running  = true;
  esp_wifi_set_promiscuous(true);
}

void wifiStop() {
  if (!s_running) return;
  esp_wifi_set_promiscuous(false);
  s_running = false;
}

void wifiPoll() {
  if (!s_running || !s_cfg) return;
  hopTick();

  for (;;) {
    uint16_t t = s_tail.load(std::memory_order_relaxed);
    if (t == s_head.load(std::memory_order_acquire)) break;
    processFrame(s_ring[t]);
    s_tail.store((uint16_t)((t + 1) % RING_LEN), std::memory_order_release);
  }
}

uint32_t wifiFrames()      { return s_frames.load(std::memory_order_relaxed); }
uint32_t wifiUniqueCount() { return s_unique; }
uint8_t  wifiCurrentChannel() { return s_curCh; }
bool     wifiIsRunning()   { return s_running; }
