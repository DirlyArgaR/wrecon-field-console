/*
 * Pemindaian WiFi PASIF.
 *
 * Cara kerja: promiscuous mode + filter frame management. Radio diam di
 * satu channel selama wifiDwellMs, mengumpulkan beacon (subtype 0x08) dan
 * probe response (0x05) yang sudah ada di udara. Tidak pernah mengirim
 * apa pun: tidak ada probe request, tidak ada association.
 *
 * Konsekuensi (penting):
 *  - SSID hidden tetap terbaca sebagai "hidden": true (SSID IE kosong di beacon).
 *  - AP yang hanya menjawab probe request aktif tidak akan terlihat.
 *  - Deauth/disassoc dihitung pasif; flood memicu alert NDJSON.
 *  - Channel 14 tidak disertakan (di luar region umum, butuh set country dulu).
 *
 * RSSI diambil dari rx_ctrl.rssi, bukan dari payload frame.
 */

#pragma once
#include "config.h"

void     wifiStart(const ScanConfig &cfg);
void     wifiStop();
void     wifiPoll();          // panggil dari loop(): hop channel + parse ring
uint32_t wifiFrames();        // total frame mgmt yang lolos filter
uint32_t wifiDeauthCount();   // total deauth lolos filter
uint32_t wifiDisassocCount(); // total disassoc lolos filter
uint32_t wifiAlertCount();    // total alert flood yang dikirim
uint32_t wifiUniqueCount();   // jumlah AP unik di cache
uint8_t  wifiCurrentChannel();
bool     wifiIsRunning();
