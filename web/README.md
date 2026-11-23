# WRECON Web Console

Browser console untuk perangkat NDJSON ESP32 lewat USB-UART (CP210x / CH340 /
FTDI). Dipakai untuk firmware passive `firmware/` dan firmware TX `jammer-firmware/`.

## Jalankan

```bash
cd web
npm ci
npm start
```

Buka `http://localhost:8080`. Port dan baud bisa diubah di header sebelum
CONNECT. Default: `/dev/ttyUSB0`, `921600`.

Kalau port tidak terdeteksi:

```bash
sudo usermod -aG uucp "$USER"   # Arch: uucp, Debian/Ubuntu: dialout
# logout/login, lalu cek: ls -l /dev/ttyUSB0
```

## Alur

1. CONNECT — server membuka serial, mengirim `status` + `config`.
2. Tunggu AP/client muncul dari promiscuous sniff.
3. Centang AP → **ADD SELECTED → TARGETS**, atau **ADD MAC** manual.
4. Atur channel / interval / burst / reason / broadcast.
5. **APPLY PARAMS**, lalu **START JAM**. **STOP JAM** menghentikan TX.

Endpoint:

| Method | Path | Fungsi |
|---|---|---|
| GET | `/api/state` | snapshot lengkap |
| GET | `/api/ports` | daftar port serial |
| POST | `/api/connect` | `{portPath, baudRate}` |
| POST | `/api/disconnect` | — |
| POST | `/api/command` | objek command NDJSON |
| GET | `/api/events` | SSE (`snapshot`, `msg`, `raw`, `error`) |

## Build GitHub

`.github/workflows/web-jammer.yml` menjalankan `npm ci` + build firmware dan
menghasilkan artifact `wrecon-webconsole` serta `jammer-firmware-bin`.
