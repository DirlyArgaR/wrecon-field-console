/*
 * ESP32 passive wireless signal analyzer — firmware entry point.
 *
 * Build: pio run -e esp32s3 -t upload
 * Monitor: pio device monitor -b 921600
 *
 * Semua komunikasi USB serial = NDJSON (satu objek JSON per baris).
 * Lihat docs/PROTOCOL.md untuk daftar lengkap pesan.
 */

#include <Arduino.h>
#include "config.h"
#include "link.h"
#include "state.h"
#include "wifi_passive.h"
#include "ble_passive.h"

static uint32_t s_lastStats = 0;

void setup() {
  Serial.begin(SERIAL_BAUD);
  linkInit();

  // USB CDC di S3 bisa belum siap tepat setelah boot. Tunggu maks 3 detik.
  uint32_t t0 = millis();
  while (!Serial && (millis() - t0) < 3000) delay(20);

  JsonDocument hello;
  hello["type"]    = "hello";
  hello["fw"]      = FW_VERSION;
  hello["chip"]    = ESP.getChipModel();
  hello["serial"]  = SERIAL_BAUD;
  hello["proto"]   = 1;
  linkEmitJson(hello);
  linkEmitConfig();
  linkFlush();

  s_lastStats = millis();
}

void loop() {
  linkPoll();      // terima perintah dari Android
  wifiPoll();      // hop channel + parse frame beacon yang antre
  blePoll();       // drain advertisement + restart window scanning

  if (g_cfg.statsMs && (uint32_t)(millis() - s_lastStats) >= g_cfg.statsMs) {
    s_lastStats = millis();
    linkEmitStatus();
  }

  linkFlush();     // kirim semua event yang antre
  delay(1);        // kasih waktu ke WiFi/BLE task
}
