/*
 * State aplikasi global. Semua modul baca/tulis lewat sini.
 */

#pragma once
#include <Arduino.h>
#include "config.h"

extern ScanConfig g_cfg;      // parameter aktif
extern RunMode    g_mode;     // mode aktif
extern volatile bool g_running;

const char *modeStr(RunMode m);
RunMode     parseMode(const char *s);   // MODE_OFF kalau tak dikenal

void applyMode(RunMode m);    // start/stop scanner sesuai mode
void applyConfig();           // restart scanner aktif agar param baru berlaku
