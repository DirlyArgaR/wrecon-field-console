# WRECON — ESP32 WiFi jammer + Android console

Firmware ESP32 untuk deauth WiFi 2.4 GHz dengan daftar target, dikontrol lewat
aplikasi Android USB OTG. Aplikasi melakukan scan AP pasif, memilih target,
mengatur parameter, lalu menjalankan / menghentikan transmit.

## Arsitektur

```
[ESP32-U]  jammer-firmware: sniff AP/client + deauth via esp_wifi_80211_tx
     |  NDJSON @ 921600 lewat USB-UART (CP210x/CH340)
     v
[Android]  UsbSerialLink -> SharedFlow<String> -> ScanViewModel -> Compose UI
```

Kenapa NDJSON: parser stateless, mudah di-debug dengan `pio device monitor`,
dan aman kalau satu baris corrupt (baris lain tetap terbaca).

### Firmware (PlatformIO)

```
firmware/
  platformio.ini
  include/config.h        # ScanConfig + batas parameter
  include/state.h         # g_cfg, g_mode, applyMode/applyConfig
  include/link.h          # ring output + parser baris masuk
  include/cmd.h           # dispatch perintah
  include/wifi_passive.h
  include/ble_passive.h
  src/main.cpp            # setup/loop
  src/state.cpp
  src/link.cpp            # NDJSON in/out + config/status
  src/cmd.cpp             # mode | set | status | ping | reset
  src/wifi_passive.cpp    # sniff 802.11 mgmt, hop channel, dedup AP
  src/ble_passive.cpp     # NimBLE passive scan, dedup per MAC
```

Alur data WiFi: callback promiscuous menyalin frame mentah ke ring atomik
(SPSC) → `wifiPoll()` di `loop()` yang parse IE SSID/DS/RSN, filter RSSI, dedup
per BSSID, lalu antre JSON ke `link`. BLE mirip: callback NimBLE mengirim event
ke FreeRTOS queue → `blePoll()` yang dedup dan antre JSON. Semua penulisan Serial
terpusat di `linkFlush()`, jadi tidak ada interleave antar task.

### Android

```
android/app/src/main/java/com/example/wrecon/
  MainActivity.kt         # izin USB + Compose
  UsbSerialLink.kt        # usb-serial-for-android, baca baris, write command
  Protocol.kt             # parse JSON (org.json) + builder perintah
  ScanViewModel.kt        # state, dedup tampilan, aksi UI
  ui/ScanScreen.kt        # UI gelap: KPI, target chip, parameter, daftar AP/client
```

## Build

Firmware:

```bash
cd firmware
pio run                    # default = esp32dev (ESP32 klasik)
pio run -t upload
pio device monitor -b 921600

# kalau pakai ESP32-S3 native USB:
# pio run -e esp32s3 -t upload
```

APK (lokal): butuh JDK 17 + Android SDK 35.

```bash
cd android
./gradlew assembleDebug
```

APK juga dibangun otomatis oleh GitHub Actions di `.github/workflows/android.yml`.
Job `apk` menghasilkan artifact **wrecon-debug-apk** (`app-debug.apk`), job
`firmware` menghasilkan **firmware-bin** (ESP32-S3 + ESP32 klasik).
Workflow jalan pada push/PR ke folder `android/`, `firmware/`, atau workflow itu
sendiri, dan bisa di-trigger manual lewat **Run workflow**.

## Contoh sesi

```text
→ {"cmd":"mode","mode":"wifi"}
← {"type":"rsp","ok":true,"cmd":"mode","msg":"ok","mode":"wifi"}
← {"type":"wifi","ts":8123,"bssid":"...","ssid":"lab","ch":6,"rssi":-61,"auth":"wpa2","hits":1}
→ {"cmd":"set","params":{"rssi_min":-75,"channels":[1,6,11]}}
← {"type":"rsp","ok":true,"cmd":"set","msg":"params applied"}
← {"type":"config",...}
```

## Keterangan penting

- **Soal "daftar" perangkat**: `res/xml/device_filter.xml` hanya dipakai untuk
  auto-launch saat kabel USB dicolok. Tidak wajib: tombol **Connect USB**
  tetap enumerasi semua perangkat lewat `UsbSerialProber`. Selama chip jembatan
  USB didukung library (CP210x, CH340, FTDI, PL2303, ESP32-S3 CDC), tidak perlu
  menambah VID/PID. Baru perlu custom driver kalau chip-nya di luar daftar itu.
  Di Linux, izin port biasanya cukup masuk grup `dialout` (udev rule), bukan
  pendaftaran di aplikasi.
- **Beacon saja**: SSID hidden tetap tampil sebagai `hidden: true`. AP yang hanya
  merespons probe request aktif tidak terlihat. Ini fitur, bukan bug.
- **BLE passive**: nama yang hanya ada di SCAN_RSP tidak terbaca.
- **Satu radio 2.4 GHz**: mode `both` berarti WiFi dan BLE bergantian, lebih
  banyak frame terlewat. Untuk pengukuran, pakai satu mode.
- **Dua jebakan yang sudah diperbaiki** (terbukti saat uji hardware ESP32
  klasik/CP210x):
  1. `esp_wifi_set_ps(WIFI_PS_NONE)` sebelum BLE init bikin `coex_enable()`
     abort (Espressif issue #9595). Firmware memakai `WIFI_PS_MIN_MODEM`.
  2. `NimBLEScan::start(duration, false)` di NimBLE 1.4.x itu **blocking**
     (`ulTaskNotifyTake(portMAX_DELAY)`) dan menghangkan `loop()`. Firmware
     memakai overload non-blocking `start(duration, callback, false)`.
- **Channel 13**: nyalakan hanya kalau regulator negara mengizinkan.
- **Legal**: gunakan hanya pada spektrum/perangkat yang Anda miliki atau
  punya izin. Tidak ada kemampuan transmit di firmware ini.

Detail pesan: [docs/PROTOCOL.md](docs/PROTOCOL.md).

---

## Web console + jammer firmware

`web/` adalah console browser (Node + Express + serialport + SSE) untuk
perangkat NDJSON di `/dev/ttyUSB0`. UI menampilkan AP/client hasil sniff,
target BSSID, parameter TX, dan log serial. Jalankan:

```bash
cd web
npm ci
npm start          # http://localhost:8080
```

`jammer-firmware/` adalah firmware ESP32 dengan perintah `jam_start` /
`jam_stop` / `targets` / `set`. TX deauth/disassoc hanya ke BSSID yang masuk
daftar target. Build:

```bash
cd jammer-firmware
pio run -e esp32dev
pio run -e esp32dev -t upload
```

`bt-jammer-firmware/` adalah firmware Bluetooth dedicated: begitu power langsung
BLE advertising flood + BR/EDR inquiry flood + auto connect/disconnect, tanpa web.

Protokol TX: [docs/JAM_PROTOCOL.md](docs/JAM_PROTOCOL.md). GitHub Actions:
`.github/workflows/web-jammer.yml` mem-build keduanya dan menghasilkan artifact
`wrecon-webconsole` serta `jammer-firmware-bin`.
