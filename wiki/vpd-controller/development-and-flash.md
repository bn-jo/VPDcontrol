# VPD Controller — Development and Flash Procedure

> Sources: Ben Guetta, 2026-05-21
> Raw: [VPD Controller Project Overview](../../raw/vpd-controller/2026-05-21-vpd-controller-project.md)

## Overview

The project supports two build paths: PlatformIO (recommended) and Arduino IDE. OTA flashing is the standard update path for a deployed device.

## PlatformIO (Recommended)

### Firmware Update (OTA)

**Preferred method — HTTP upload (reliable):**
```bash
curl -u ben:7777 \
  -F "firmware=@.pio/build/ota/firmware.bin;type=application/octet-stream" \
  --max-time 300 http://192.168.1.200/update
```
The connection drops at the end — that's the device rebooting. Verify with:
```bash
curl -s -u ben:7777 http://192.168.1.200/ -H "Accept-Encoding: gzip" -D - -o /dev/null | grep content-length
```

**Alternative — espota (sometimes fails at ~70%):**
```bash
~/.platformio/penv/bin/pio run --target upload --environment ota
```
`pio` must be run via its full path (`~/.platformio/penv/bin/pio`) — it is not on PATH by default. The espota method fails intermittently because the device watchdog fires during a slow flash sector erase. If it fails, use the HTTP method above.

### CRITICAL: Never OTA-flash the filesystem
`pio run --target uploadfs` wipes `logs.csv` and `irrig.csv` — all sensor and irrigation history is permanently lost. Only run `uploadfs` on a blank device via cable for the initial setup.

### UI Update
1. Edit `data/index.html`
2. Run `python3 scripts/build_html.py` — regenerates `src/ui_html.h`
3. Flash firmware OTA (the compressed UI is embedded in the firmware binary)

### Environment config (`platformio.ini`)
- Target board: ESP32
- Partition: LittleFS
- OTA environment: includes upload_port and auth settings

## Arduino IDE

- Entry point: `VPDcontrol.ino` (project root) — Arduino IDE 2.x auto-compiles `src/*.cpp`
- Board: **ESP32 Dev Module** | Partition: **Default 4MB with spiffs** | Upload speed: 921600
- Required libraries:
  - DHT sensor library
  - Adafruit Unified Sensor
  - ESP Async WebServer
  - AsyncTCP
  - ArduinoJson
- Web UI upload: Tools → ESP32 LittleFS Data Upload (requires `arduino-esp32fs-plugin`)

## Local UI Development

Open `data/index.html` via a local HTTP server for full preview (mock data requires same-origin script loading):

```bash
python3 -m http.server 8080 --directory data
# then open http://localhost:8080/index.html
```

`data/mock.js` (gitignored) auto-injects realistic state and 24h chart history on `localhost` or `file://`. Opening via `file://` in Chrome will suppress mock.js due to CORS — use localhost instead.

After editing the HTML, run `scripts/build_html.py` is invoked automatically as a PlatformIO pre-build step — no need to run it manually.

## Removed Library: IRremoteESP8266

Removed 2026-04-13. Was causing repeated PANIC-CRASH (stack overflow). Do not re-add. A/C is controlled via physical relay (GPIO 25) only.

## WiFi Credentials

Stored in `src/secrets.h` (gitignored — copy from `src/secrets.h.example`):
- `WIFI_SSID` / `WIFI_PASSWORD`

## See Also

- [System Architecture](system-architecture.md)
- [Web UI and API](web-ui-and-api.md)
