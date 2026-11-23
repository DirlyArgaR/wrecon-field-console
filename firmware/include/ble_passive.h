/*
 * Pemindaian BLE PASIF (NimBLE-Arduino 1.4.x).
 *
 * setActiveScan(false) => scanner tidak pernah kirim SCAN_REQ.
 * Konsekuensi: nama perangkat hanya terbaca kalau ada di payload ADV_IND
 * itu sendiri. Nama yang cuma ada di SCAN_RSP tidak akan terlihat.
 *
 * Alamat random (RPA) berputar tiap ~15 menit, jadi device yang sama bisa
 * muncul dengan MAC berbeda. Cache dedup di sini hanya meredam duplikat
 * dalam rentang pendek.
 *
 * Catatan hardware: ESP32 punya satu radio 2.4 GHz. Mode both = WiFi dan
 * BLE bergantian pakai radio, jadi sensitivitas keduanya turun. Untuk
 * pengukuran serius pakai satu mode saja.
 */

#pragma once
#include "config.h"

void     bleStart(const ScanConfig &cfg);
void     bleStop();
void     blePoll();           // panggil dari loop(): drain queue + restart window
uint32_t bleSeen();           // total advertisement yang diterima
uint32_t bleUniqueCount();    // jumlah alamat unik di cache
bool     bleIsRunning();
