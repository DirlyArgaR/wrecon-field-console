/*
 * Handler perintah masuk dari aplikasi Android.
 *
 * Setiap perintah = satu baris JSON + '\n'. Lihat docs/PROTOCOL.md.
 * Tiap perintah dibalas {"type":"rsp", ...} supaya app punya konfirmasi.
 */

#pragma once

void cmdHandleLine(const char *line);
