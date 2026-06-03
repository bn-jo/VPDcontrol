---
name: vpd-esp32-dev
description: "Use for any work on the VPD grow-tent ESP32 controller at /Users/liat/Documents/VPDcontrol. Triggers on: editing firmware (src/*.cpp, src/*.h), updating the web UI (data/index.html), flashing the device, debugging climate/relay/irrigation logic, or any question about the grow-tent system. Provides project-specific rules, pin assignments, gotchas, and workflow to avoid common mistakes like wiping sensor history or using GPIO 12."
---

# VPD Grow-Tent ESP32 Controller

This skill encodes hard-won project knowledge for working on /Users/liat/Documents/VPDcontrol — an ESP32 climate controller for a grow tent. Use it to avoid common mistakes and follow established patterns.

## Critical Rules (Never Break These)

1. **Never `uploadfs` via OTA.** It wipes `logs.csv` and `irrig.csv` — all sensor and irrigation history is permanently lost. `uploadfs` is only valid via cable on a blank device.

2. **Never use GPIO 12 for relays.** It is an ESP32 boot strapping pin. Lights are on GPIO 22.

3. **Never re-add IRremoteESP8266.** It was removed 2026-04-13 because it caused stack overflow PANIC-CRASH. A/C is now controlled exclusively via physical relay (GPIO 25).

4. **Never use `delay()` in firmware.** The entire control loop runs on a millis() state machine at 100ms ticks.

## Firmware Architecture

- **Core 0** — FreeRTOS control task: sensor reads, VPD calc, relay logic (100ms tick)
- **Core 1** — WiFi, AsyncWebServer, WebSocket broadcast
- Control logic files: `src/climate.cpp/h`, `src/relays.cpp/h`
- Sensor pipeline: DHT22 → 12-sample rolling average → VPD → control decisions

## Web UI Workflow

When editing `data/index.html`:
1. Make CSS/HTML changes
2. Open in browser via `file://` to preview (WebSocket won't connect but layout is visible)
3. Run `python3 scripts/build_html.py` to regenerate `src/ui_html.h`
4. Flash firmware OTA: `pio run --target upload --environment ota`

**JS safety:** All IDs, class names, and localStorage keys referenced by JavaScript must not be renamed. Only edit the `<style>` block and HTML structure — never touch the `<script>` block unless intentionally modifying behavior.

## GPIO Pin Map (Quick Reference)

| GPIO | Device |
|------|--------|
| 4 | DHT22 |
| 26 | Relay 1 — Top Fan |
| 27 | Relay 2 — Bottom Fan (exhausts TO room) |
| 14 | Relay 3 — Humidifier |
| 22 | Relay 4 — Lights |
| 25 | Relay 5 — A/C (physical relay) |
| 33 | Relay 6 — Heat Mat |
| 32 | Relay 7 — Watering |
| 13 | Relay 8 — Extra/Dehumidifier |
| 35 | Soil moisture (ADC1_CH7, input-only) |
| 23, 34 | UNUSED (former IR pins — can disconnect) |

## Relay Logic Summary

- All relays: active LOW, 30s minimum ON/OFF, asymmetric hysteresis
- Humidifier (3) and dehumidifier (8): hard interlock — dehumidifier wins
- Watering (7): AUTO = off; only triggered by precision irrigation
- A/C (5): ON when temp > tempMax

## Data Storage

- **NVS (Preferences):** grow mode, relay settings, VPD target, WiFi sensors, irrigation profiles
- **LittleFS:** logs.csv (7-day sensor history), irrig.csv (irrigation log)
- **localStorage:** UI preferences, relay visibility (client-side only)

## Common Debugging Paths

**Relay not responding in Auto:** Check `climate.cpp` for the relay's auto logic. Check hysteresis values in `config.h`. Check if minimum ON/OFF timer is still counting.

**UI state not updating:** WebSocket message type handling is in `webserver.cpp`. State broadcast is in the Core 1 loop in `main.cpp`.

**Sensor readings wrong:** Check rolling average window in `sensors.cpp`. Verify DHTesp library returns valid (non-NaN) readings before pushing to the average.

**Crash on boot:** Check if IRremoteESP8266 somehow re-entered platformio.ini. Also check GPIO 12 is not in use.

## Wiki

A full knowledge base is at `wiki/` in the project root. Start with `wiki/index.md`.

- System Architecture: `wiki/vpd-controller/system-architecture.md`
- Hardware Reference: `wiki/vpd-controller/hardware-reference.md`
- Climate Logic: `wiki/vpd-controller/climate-and-control-logic.md`
- Web UI & API: `wiki/vpd-controller/web-ui-and-api.md`
- Flash Procedure: `wiki/vpd-controller/development-and-flash.md`
