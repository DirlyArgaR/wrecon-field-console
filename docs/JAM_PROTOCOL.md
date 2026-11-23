# Protokol Serial — Jammer Firmware

NDJSON, 921600 8N1, satu objek per baris. Sama seperti `PROTOCOL.md`, dengan
perintah TX tambahan. Firmware: `jammer-firmware/`.

## App → Device

```jsonc
{"cmd":"ping"}
{"cmd":"status"}                 // rsp + event status
{"cmd":"config"}                 // rsp + event config
{"cmd":"reset"}                  // stop TX, config default, target dikosongkan
{"cmd":"clear"}                  // kosongkan tabel AP/client (bukan target)

{"cmd":"set","params":{
  "interval_ms":5,               // 1..1000, jeda antar frame
  "burst":8,                     // 1..100 frame per burst
  "burst_gap_ms":30,             // 0..5000
  "channel":6,                   // 1..13, dipakai sniff + TX
  "hop":false,                   // sweep 1..13
  "hop_ms":200,                  // 10..5000
  "reason":7,                    // 1..25, deauth reason code
  "broadcast":true,              // true = deauth ke broadcast per BSSID
  "disassoc":false,              // true = subtype disassoc (0xA0)
  "stats_ms":2000,               // 0 = matikan status periodik
  "repeat_ms":1500,              // 0 = kirim hanya saat ada perubahan
  "rssi_min":-95                 // -120..0
}}

{"cmd":"targets","targets":["AA:BB:CC:DD:EE:FF"]}
{"cmd":"jam_start","targets":["AA:BB:CC:DD:EE:FF"],"client_targets":["11:22:33:44:55:66"]}
{"cmd":"jam_stop"}
```

`jam_start` mengganti target bila `targets` dikirim. `client_targets` hanya
dipakai kalau `broadcast:false`; MAC client harus sudah pernah terlihat supaya
BSSID-nya diketahui.

## Device → App

```jsonc
{"type":"hello","fw":"1.0.0-jam","chip":"ESP32","serial":921600,"proto":2}

{"type":"config","params":{...sama seperti di atas...},
 "targets":["AA:BB:CC:DD:EE:FF"],"client_targets":[],"jamming":false}

{"type":"status","ts":12345,"jamming":true,"channel":6,
 "targets":1,
 "stats":{"frames":420,"aps":38,"clients":12,"tx":8000,"tx_errors":0,
          "rx_dropped":0,"ring_dropped":0}}

{"type":"wifi","ts":12345,"bssid":"AA:BB:CC:DD:EE:FF","ssid":"lab",
 "hidden":false,"ch":6,"rssi":-61,"auth":"wpa2","hits":3}
{"type":"client","ts":12345,"addr":"11:22:33:44:55:66",
 "bssid":"AA:BB:CC:DD:EE:FF","ch":6,"rssi":-67,"hits":2}
{"type":"rsp","ok":true,"cmd":"jam_start","msg":"jamming"}
```

## Frame yang dikirim

802.11 Management Deauthentication (0xC0) atau Disassociation (0xA0), 26 byte:

| Offset | Isi |
|---|---|
| 0 | `0xC0` / `0xA0` |
| 4..9 | receiver (`FF:FF:...` untuk broadcast, atau MAC client) |
| 10..15 | transmitter = BSSID target (spoof AP) |
| 16..21 | BSSID target |
| 24..25 | reason code |

TX hanya berjalan untuk BSSID yang ada di `targets`. Daftar itu bisa dikirim
langsung lewat web, tapi tetap berada di firmware — bukan sekadar filter UI.

Firmware memakai `WIFI_IF_STA` tanpa asosiasi dan `en_sys_seq=false`. SoftAP
sengaja tidak dinyalakan: dengan AP aktif, driver menolak frame yang `addr2`-nya
bukan MAC AP interface (`ESP_ERR_INVALID_ARG` 258).

Pada IDF 4.4 / Arduino core 2.x, driver juga memvalidasi `addr2` untuk frame
management. `src/bypass.S` masuk ke `esp_wifi_80211_tx` setelah validasi itu
(offset `+0x19`) supaya BSSID spoof benar-benar dikirim. Tanpa wrapper ini,
status menampilkan `tx_errors` dan `tx_last_err: 258`.
