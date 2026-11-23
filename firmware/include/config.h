#pragma once
#include <Arduino.h>

#define FW_VERSION      "1.0.0"
#define SERIAL_BAUD     921600
#define FW_CHANNEL_MAX  13

/* Panjang baris perintah masuk (byte). Buffer tetap, hindari malloc. */
#define RX_LINE_MAX     768
/* Panjang satu baris event keluar. Cukup untuk SSID 32 char + MAC + JSON. */
#define EVENT_MAX_LEN   320

enum RunMode : uint8_t {
  MODE_OFF  = 0,
  MODE_WIFI = 1,
  MODE_BLE  = 2,
  MODE_BOTH = 3,
};

/*
 * Parameter yang bisa diubah aplikasi mobile lewat perintah {"cmd":"set",...}
 * Semua nilai punya default sane supaya alat langsung jalan tanpa konfigurasi.
 */
struct ScanConfig {
  uint16_t wifiDwellMs   = 400;   // waktu diam per channel (beacon ~100 ms)
  uint16_t bleScanMs     = 1000;  // panjang satu window scanning BLE
  uint16_t bleIntervalMs = 200;   // interval scan BLE
  uint16_t bleWindowMs   = 60;    // scan window BLE (duty = window/interval)
  uint16_t repeatMs      = 2000;  // kirim ulang AP yang sama tiap N ms
  uint16_t dedupMs       = 60000; // umur entri di cache dedup
  uint16_t statsMs       = 5000;  // periodik stats (0 = matikan)
  uint16_t floodWindowMs = 1000;  // jendela deteksi flood deauth/disassoc
  uint8_t  floodThreshold = 20;   // frame per jendela sebelum dianggap flood
  int8_t   rssiMin       = -95;   // buang frame dengan RSSI di bawah ini
  bool     channels[FW_CHANNEL_MAX + 1] = {
    false,                                              // index 0 tak dipakai
    true, true, true, true, true, true, true,           // 1..7
    true, true, true, true, true, true,                 // 8..13
  };
};
