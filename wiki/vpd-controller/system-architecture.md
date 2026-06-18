# VPD Controller — System Architecture

> Sources: Ben Guetta, 2026-05-21
> Raw: [VPD Controller Project Overview](../../raw/vpd-controller/2026-05-21-vpd-controller-project.md)

## Overview

An ESP32-based grow room climate controller that automates VPD (Vapour Pressure Deficit), temperature, humidity, and soil moisture management via 8 relay outputs. The system runs a custom real-time firmware with a browser-based web UI accessible over WiFi.

## Dual-Core Architecture

The firmware exploits the ESP32's dual cores to isolate time-sensitive from network-sensitive work:

- **Core 0** — FreeRTOS task: sensor reads, VPD calculation, relay control logic (100ms tick, no `delay()`)
- **Core 1** — Arduino `loop()`: WiFi, AsyncWebServer, WebSocket broadcast

This prevents network jitter from affecting control timing and vice versa.

## Sensor Pipeline

1. AM2301 reads raw temperature and humidity every 100ms
2. 12-sample rolling average (~120s smoothing window) applied before any control decision
3. VPD calculated as: `SVP(leaf_temp) - actual_vapour_pressure` where `leaf_temp = air_temp - 2°C`
4. Smoothed values feed the climate control logic

## Control Hierarchy

Climate control applies three priority layers in order:

1. **VPD (primary)** — fans, humidifier adjusted to hit VPD target
2. **Temperature (secondary)** — heat mat, A/C relay as needed
3. **Humidity (tertiary)** — dehumidifier relay for high-humidity correction

## Relay System

Eight relays with asymmetric hysteresis (different turn-on vs turn-off thresholds) and a minimum 30-second ON/OFF time to prevent rapid cycling. All relays active LOW on a single 8-channel relay module.

Relay modes per relay: **Auto** (climate logic), **Manual** (user forced on/off), **Timer** (on for N minutes), **Schedule** (time-of-day on/off).

## Data Persistence

| Store | Contents |
|-------|----------|
| LittleFS `/logs.csv` | 7-day rolling sensor history (T, H, VPD, soil, timestamp) |
| LittleFS `/irrig.csv` | Irrigation event history |
| ESP32 Preferences (NVS) | Grow mode, relay settings, VPD target, irrigation profiles, WiFi sensors |
| Browser localStorage | Relay visibility, UI tab state, user preferences |

## Web UI

Single HTML file (`data/index.html`, ~4900 lines, ~54 KB gzip) embedded in firmware via `scripts/build_html.py` → `src/ui_html.h`. Zinc-950 dark theme, Emerald accent. Grow-assist features: stage progress strip, dry-back display, watering ETA, VPD time-in-range, VPD band overlay on history chart. Mock data file (`data/mock.js`, gitignored) enables full browser preview at localhost without a device.

## Key Invariants

- **Never `uploadfs` via OTA** — wipes all sensor history
- **NTP-backed light schedule** survives reboots; never auto-resets
- **Humidifier runs 24/7** — night VPD targets naturally reduce its duty cycle
- **IR blaster permanently removed** (2026-04-13) — caused stack overflow crashes; A/C controlled via physical relay only

## See Also

- [Hardware Reference](hardware-reference.md)
- [Climate and Control Logic](climate-and-control-logic.md)
- [Web UI and API](web-ui-and-api.md)
- [Development and Flash Procedure](development-and-flash.md)
