# Protokol Serial NDJSON

Satu baris = satu objek JSON, diakhiri `\n`. Baud **921600**, 8N1, tanpa flow control.
Semua field opsional kecuali `type`/`cmd`. Field tak dikenal diabaikan.

## App → Device

```jsonc
{"cmd":"mode","mode":"wifi"}      // off | wifi | ble | both
{"cmd":"status"}                   // minta status + config
{"cmd":"ping"}
{"cmd":"reset"}                    // set parameter default + mode off
{"cmd":"set","params":{
  "wifi_dwell_ms":400,             // 10..5000
  "ble_scan_ms":1000,              // 50..30000
  "ble_interval_ms":200,           // 20..4000
  "ble_window_ms":60,              // 20..4000
  "repeat_ms":2000,                // 0 = kirim hanya saat perubahan RSSI
  "dedup_ms":60000,                // umur cache dedup
  "stats_ms":5000,                 // 0 = matikan status periodik
  "rssi_min":-95,                  // -120..0
  "channels":[1,6,11,13]           // kosong = semua channel
}}
```

Balasan setiap perintah:

```json
{"type":"rsp","ok":true,"cmd":"mode","msg":"ok","mode":"wifi"}
```

## Device → App

```jsonc
{"type":"hello","fw":"1.0.0","chip":"ESP32-S3","serial":921600,"proto":1}

{"type":"config","mode":"wifi","params":{ ...sama seperti di atas... }}

{"type":"status","ts":12345,"mode":"wifi","running":true,
 "stats":{"wifi_frames":420,"wifi_unique":38,"ble_events":90,"ble_unique":12,"tx_dropped":0},
 "wifi_channel":6}

{"type":"wifi","ts":12345,"bssid":"AA:BB:CC:DD:EE:FF","ssid":"lab-ap",
 "hidden":false,"ch":6,"rssi":-67,"auth":"wpa2","hits":3}

{"type":"ble","ts":12345,"addr":"11:22:33:44:55:66","addr_type":"random",
 "rssi":-72,"conn":true,"adv_len":31,"name":"Sensor-01","hits":2}
```

`hits` = jumlah kemunculan sejak scanner aktif. `name` tidak dikirim kalau tidak
ada di payload advertisement (karena passive scan tidak mengirim SCAN_REQ).

## Status filter RSSI

Filter `rssi_min` diterapkan di firmware sebelum JSON dibentuk, supaya traffic
USB tetap rendah. App hanya perlu menampilkan yang diterima.

## Rekomendasi alur

1. Buka port USB 921600.
2. Tunggu `hello` + `config`.
3. Kirim `status` untuk sinkron (boot message bisa terlewat sebelum host siap).
4. Kirim `set` bila perlu, lalu `mode`.
5. Tampilkan `wifi`/`ble` secara real-time; `status` untuk counter.
