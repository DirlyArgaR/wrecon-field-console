# BT JAMMER — dedicated firmware

Firmware ESP32 khusus Bluetooth. Begitu diberi power, langsung jalan tanpa
perintah, tanpa web, tanpa WiFi.

## Yang berjalan

- **BLE advertising flood** — payload vendor + random bergantian, MAC acak setiap burst.
- **BR/EDR inquiry flood** — general inquiry berulang, menemukan perangkat discoverable.
- **Auto connect/disconnect** — perangkat hasil inquiry disambung sebentar lalu diputus, bergilir.
- Status NDJSON tiap 2 detik di `921600` untuk debugging.

## Build / flash

```bash
cd bt-jammer-firmware
pio run -e esp32dev
pio run -e esp32dev -t upload --upload-port /dev/ttyUSB0
```

## Tuning

Ubah konstanta di puncak `src/main.cpp`:

| Konstanta | Default | Fungsi |
|---|---|---|
| `BLE_ADV_MS` | 25 | durasi tiap advertising burst |
| `BLE_GAP_MS` | 10 | jeda antar burst |
| `BLE_POWER` | 9 | daya TX BLE |
| `BT_INQUIRY_MS` | 6000 | durasi inquiry sebelum pindah ke connect |
| `BT_CONNECT_ENABLE` | true | aktifkan connect/disconnect |
| `BT_CONNECT_HOLD_MS` | 500 | lama koneksi sebelum diputus |
| `BT_CONNECT_GAP_MS` | 200 | jeda antar percobaan |

Partisi `no_ota.csv` dipakai karena flash 4 MB. Ukuran firmware ~1,12 MB.
