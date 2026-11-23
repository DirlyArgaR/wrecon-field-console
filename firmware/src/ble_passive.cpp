#include "ble_passive.h"
#include "state.h"
#include "link.h"

#include <string.h>
#include <ArduinoJson.h>
#include <NimBLEDevice.h>

/*
 * NimBLE-Arduino 1.4.x.
 * Kalau naik ke 2.x: ganti NimBLEAdvertisedDeviceCallbacks ->
 * NimBLEScanCallbacks, dan onResult(NimBLEAdvertisedDevice*) ->
 * onResult(const NimBLEAdvertisedDevice*). Sisanya sama.
 */

#define BLE_Q_LEN 64

struct BleEvt {
  uint8_t addr[6];
  uint8_t addrType;
  int8_t  rssi;
  bool    connectable;
  bool    hasName;
  uint8_t advLen;
  char    name[33];
};

static QueueHandle_t s_q = nullptr;
static NimBLEScan  *s_scan = nullptr;
static const ScanConfig *s_cfg = nullptr;
static bool     s_running = false;
static uint32_t s_seen = 0;
static uint32_t s_queueDrops = 0;
static uint32_t s_lastStart = 0;

/* cache dedup per alamat (loop task saja) */
#define BCACHE_LEN 128
struct BCacheEntry {
  bool     used;
  uint8_t  addr[6];
  uint16_t count;
  int8_t   rssi;
  uint32_t lastEmit;
};
static BCacheEntry s_bcache[BCACHE_LEN];
static uint16_t    s_bevict = 0;
static uint32_t    s_bunique = 0;

/*
 * NimBLE 1.4.x memakai satuan 0.625 ms untuk interval/window.
 *   units = ms * 8 / 5     (ms 200 -> 320 units = 200 ms)
 * Window wajib <= interval dan <= 0x4000 units (~10.24 s).
 */
static uint16_t msToUnits(uint16_t ms) {
  uint32_t u = (uint32_t)ms * 8u / 5u;
  if (u < 0x0006) u = 0x0006;
  if (u > 0x4000) u = 0x4000;
  return (uint16_t)u;
}

static void macToStr(const uint8_t *m, char *out18) {
  snprintf(out18, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
           m[0], m[1], m[2], m[3], m[4], m[5]);
}

/* ---- callback: jalan di NimBLE host task ------------------------------- */

class ScanCb : public NimBLEAdvertisedDeviceCallbacks {
  void onResult(NimBLEAdvertisedDevice *dev) override {
    BleEvt e = {};
    const NimBLEAddress addr = dev->getAddress();
    // toString() stabil lintas versi NimBLE (Format "AA:BB:CC:DD:EE:FF").
    if (sscanf(addr.toString().c_str(), "%2hhx:%2hhx:%2hhx:%2hhx:%2hhx:%2hhx",
               (unsigned char *)&e.addr[0], (unsigned char *)&e.addr[1],
               (unsigned char *)&e.addr[2], (unsigned char *)&e.addr[3],
               (unsigned char *)&e.addr[4], (unsigned char *)&e.addr[5]) != 6) {
      return;
    }
    e.addrType    = addr.getType();
    e.rssi        = (int8_t)dev->getRSSI();
    e.connectable = dev->isConnectable();
    e.advLen      = dev->getPayloadLength();
    e.hasName     = dev->haveName();
    if (e.hasName) {
      const std::string &nm = dev->getName();
      size_t n = nm.size() > 32 ? 32 : nm.size();
      memcpy(e.name, nm.data(), n);
      e.name[n] = 0;
    }
    s_seen++;
    if (!s_q || xQueueSend(s_q, &e, 0) != pdTRUE) s_queueDrops++;
  }
};

/* ---- proses satu event (loop task) ------------------------------------ */

static void handleEvt(const BleEvt &e) {
  if (!s_cfg) return;

  BCacheEntry *c = nullptr;
  BCacheEntry *freeSlot = nullptr;
  for (uint16_t k = 0; k < BCACHE_LEN; k++) {
    if (!s_bcache[k].used) { if (!freeSlot) freeSlot = &s_bcache[k]; continue; }
    if (!memcmp(s_bcache[k].addr, e.addr, 6)) { c = &s_bcache[k]; break; }
  }

  bool isNew = false;
  if (!c) {
    c = freeSlot ? freeSlot : &s_bcache[s_bevict++ % BCACHE_LEN];
    c->used = true;
    memcpy(c->addr, e.addr, 6);
    c->count    = 0;
    c->lastEmit = 0;
    c->rssi     = -127;
    isNew = true;
    s_bunique++;
  }

  uint32_t now = millis();
  c->count++;
  int8_t oldRssi = c->rssi;
  c->rssi = e.rssi;

  bool due   = s_cfg->repeatMs && (uint32_t)(now - c->lastEmit) >= s_cfg->repeatMs;
  bool moved = abs((int)e.rssi - (int)oldRssi) >= 6;
  if (!isNew && !due && !moved) return;
  c->lastEmit = now;

  char mac[18];
  macToStr(c->addr, mac);

  JsonDocument d;
  d["type"] = "ble";
  d["ts"]   = now;
  d["addr"] = mac;
  d["addr_type"] = e.addrType ? "random" : "public";
  d["rssi"] = e.rssi;
  d["conn"] = e.connectable;
  d["adv_len"] = e.advLen;
  if (e.hasName) d["name"] = e.name;
  d["hits"] = c->count;
  linkEmitJson(d);
}

static void drainQueue() {
  BleEvt e;
  while (s_q && xQueueReceive(s_q, &e, 0) == pdTRUE) handleEvt(e);
}

/*
 * start(duration, bool) di NimBLE 1.4.x adalah BLOCKING: ia menunggu
 * ulTaskNotifyTake(portMAX_DELAY) sampai scan selesai. Dipanggil dari
 * loop() ini membuat seluruh firmware hang. Pakai overload non-blocking
 * yang menerima callback; restart window dilakukan dari loop().
 */
static void scanEndCb(NimBLEScanResults results) {
  (void)results;
}

static void startWindow() {
  if (!s_scan || !s_cfg) return;
  uint16_t itv = s_cfg->bleIntervalMs;
  uint16_t win = s_cfg->bleWindowMs > itv ? itv : s_cfg->bleWindowMs;
  s_scan->setInterval(msToUnits(itv));
  s_scan->setWindow(msToUnits(win));
  s_lastStart = millis();
  s_scan->start(s_cfg->bleScanMs, scanEndCb, false);   // non-blocking
}

/* ---- API publik ------------------------------------------------------- */

void bleStart(const ScanConfig &cfg) {
  s_cfg = &cfg;

  if (!s_q) s_q = xQueueCreate(BLE_Q_LEN, sizeof(BleEvt));

  if (!s_scan) {
    NimBLEDevice::init("");                 // tanpa nama, kita hanya mau menerima
    NimBLEDevice::setPower(ESP_PWR_LVL_P9); // sensitivitas maksimum
    s_scan = NimBLEDevice::getScan();
    s_scan->setAdvertisedDeviceCallbacks(new ScanCb(), true);  // true = teruskan duplikat
    s_scan->setActiveScan(false);           // <-- PASIF: tidak kirim SCAN_REQ
    s_scan->setDuplicateFilter(false);
    s_scan->setMaxResults(0);               // tanpa batas cache internal
  }

  s_running = true;
  startWindow();
}

void bleStop() {
  if (!s_running) return;
  if (s_scan) s_scan->stop();
  s_running = false;
}

void blePoll() {
  if (!s_running || !s_scan) return;

  if (!s_scan->isScanning() && (uint32_t)(millis() - s_lastStart) >= s_cfg->bleScanMs) {
    drainQueue();
    startWindow();          // window berikutnya, param terbaru otomatis dipakai
    return;
  }

  drainQueue();
}

uint32_t bleSeen()        { return s_seen + s_queueDrops; }
uint32_t bleUniqueCount() { return s_bunique; }
bool     bleIsRunning()   { return s_running; }
