# VPD Controller — Web UI and API

> Sources: Ben Guetta, 2026-05-21 (updated 2026-05-21)

## Overview

The web interface is a ~4900-line single-page application embedded directly in the firmware. It uses vanilla HTML/CSS/JS with Chart.js for data visualization and native WebSocket for live updates. No build framework or bundler. Gzip-compressed size is ~54 KB.

## Design System

The UI uses a Zinc-950 dark theme with Emerald accent, defined as CSS custom properties:

- Background layers: `--bg` (#09090b) → `--surface` → `--surface2` → `--surface3`
- Accent: `--accent` (#10b981 emerald), `--accent-hi`, `--accent-lo`
- Status: `--green`, `--yellow`, `--red`, `--blue`
- Typography: Inter (Google Fonts CDN), modular scale `--text-xs` → `--text-2xl`
- Responsive breakpoints: 780px, 620px, 400px, 340px

## Embedding

`data/index.html` is processed by `scripts/build_html.py` which gzip-compresses it and writes `src/ui_html.h` as a C byte array. The firmware serves this compressed file in memory — no filesystem access needed for the UI.

## Page Structure

Six tabs, each with focused functionality:

| Tab | Contents |
|-----|----------|
| Dashboard | Metrics grid (T/H/VPD/Soil), grow progress strip, relay status bar, history chart with VPD band, VPD target control, relay grid, auto-tune |
| Irrigation | Master toggle, soil chart, profiles table, calibration, event log, plant setup |
| Sensors | WiFi sensor list, add/remove form, T/H readings, camera links |
| Syslog | Scrollable system log, crash banner |
| Camera | MJPEG stream viewer with auto-reconnect |
| Profiles | Grow mode editor — day/night targets for all 5 modes |

## Grow-Assist Features (Dashboard)

- **Grow progress strip:** Stage name + day counter, progress bar toward auto-transition day, "→ Next Stage in Nd" pill (warns when ≤3 days away)
- **Dry-back display:** Current dry-back % from peak soil moisture
- **Next-watering ETA:** Linear projection from soil drop rate since last water event
- **VPD time-in-range:** % of chart history where VPD was inside the profile target band
- **VPD band on chart:** Green shaded region on history chart showing profile min/max (or VPD target if enabled)

## History Chart

- Chart.js v4, 4 datasets: Temperature, Humidity, VPD, Soil
- Plugins: zoom (pinch/wheel), custom crosshair, custom VPD band (`_vpdBandPlugin`)
- VPD band source: profile `vpdMin`/`vpdMax`, overridden by VPD target when enabled
- Time-in-range badge updates whenever chart data changes

## Mock Data (dev preview)

`data/mock.js` (gitignored) injects realistic Early Flower state + 24h chart history when `index.html` is opened via `file://` or `localhost`. Never flashed to device — safe to run locally for UI development.

## WebSocket API

All live control and state sync happens over WebSocket at `ws://<device-ip>/ws`.

**Inbound (server → browser):** `state` message — full system state broadcast periodically.

**Outbound (browser → server):**

| Message type | Purpose |
|-------------|---------|
| `setMode` | Change grow mode |
| `setVpdTarget` | Set VPD target + enable/disable |
| `setAcTemps` | Set A/C temperature thresholds |
| `setLightStart` | Set light schedule start time |
| `setStageDay` | Set current grow day |
| `setDryingFast` | Toggle fast/slow drying mode |
| `relay` | Manual relay control |
| `restart` | Reboot ESP32 |

## HTTP Endpoints

| Method | Path | Description |
|--------|------|-------------|
| GET | `/api/logs?hours=N&step=N&since=epoch` | CSV rows (T, H, V, S, t) |
| GET | `/api/state` | Single JSON state snapshot |
| GET | `/api/sensors` | WiFi sensor node list |
| GET | `/api/syslog` | System log text |
| GET | `/api/irrigation` | Irrigation state, profiles, plant config |
| GET | `/update` | Serves firmware upload HTML page |
| POST | `/update` | Firmware OTA upload (multipart) — preferred over espota for reliability |
| POST | `/update/ui` | LittleFS filesystem upload — **NEVER USE, wipes logs.csv** |

## Key UI Behaviors

- **Guard window:** 30-second touch guard prevents re-rendering relay cards when user is actively editing
- **Echo suppression:** Relay mode/VPD target changes ignore server echo for a few seconds to prevent reverting user input
- **Instant feedback:** CSS class toggling for visual state before server confirmation (uses `_cache` object)
- **localStorage persistence:** Grow stage, relay modes, VPD target, disabled relays, UI preferences

## OTA Flash Reliability Note

`pio run --target upload --environment ota` (espota UDP) fails intermittently at ~70% on this device — the ESP32 watchdog fires during a slow flash sector erase. Use the HTTP endpoint instead:

```bash
curl -u ben:7777 -F "firmware=@.pio/build/ota/firmware.bin;type=application/octet-stream" \
  --max-time 300 http://192.168.1.200/update
```

Connection drop at the end is normal — it's the device rebooting after a successful flash.

## See Also

- [System Architecture](system-architecture.md)
- [Development and Flash Procedure](development-and-flash.md)
