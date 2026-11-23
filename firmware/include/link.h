/*
 * Serial link: pembungkus baris JSON (NDJSON) dua arah.
 *
 * Hanya SATU task (loop) yang menulis ke Serial. Produsen lain
 * (WiFi sniff task, BLE NimBLE host task, handler perintah) hanya
 * push ke ring di bawah. Jadi jalur panas bebas mutex.
 */

#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include "config.h"

struct EventMsg {
  char json[EVENT_MAX_LEN];
};

void linkInit();
void linkPoll();                       // baca byte masuk + dispatch perintah
void linkFlush();                      // tulis ring keluar ke Serial
void linkEmitJson(JsonDocument &doc);  // antre satu objek JSON
void linkEmitRaw(const char *s);       // antre string JSON siap pakai
uint32_t linkDropped();
bool linkHasRoom();
void linkEmitConfig();
void linkEmitStatus();
