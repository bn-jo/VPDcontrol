# VPD Grow-Tent Controller — Project Overview

> Source: /Users/liat/Documents/VPDcontrol (codebase + memory/project_vpd_control.md)
> Collected: 2026-05-21
> Published: Unknown

Full ESP32 grow-tent climate control system built in /Users/liat/Documents/VPDcontrol.

## Purpose

Automated VPD (Vapour Pressure Deficit), temperature, and humidity management for a grow tent with 8 relay outputs and soil moisture monitoring.

## File Layout

```
platformio.ini              PlatformIO build config (ESP32, LittleFS partition)
src/config.h                All pins, WiFi credentials, timing constants
src/sensors.h/cpp           DHT22 reader + VPD calculation (Magnus formula)
src/soil.h/cpp              Capacitive soil moisture sensor (ADC, averaged, % output)
src/relays.h/cpp            8-relay manager: AUTO / MANUAL / TIMER modes, hysteresis, min on/off
src/climate.h/cpp           Climate logic: VPD primary, temp secondary, humidity tertiary
src/datalogger.h/cpp        LittleFS CSV log (/logs.csv), 7-day rolling, JSON API
src/webserver.h/cpp         AsyncWebServer + WebSocket, /api/logs, /api/state
src/main.cpp                FreeRTOS task (Core 0): sensors + control; loop (Core 1): WS broadcast
data/index.html             Single-page dark-theme UI, Chart.js from CDN, WebSocket live updates
wiring_diagram.svg          Full wiring diagram — single 8-channel relay module
```

## Key Design Decisions

- No delay() anywhere — millis() state machine in FreeRTOS task (100ms tick)
- Control task on Core 0; WiFi/web server on Core 1
- Relay hysteresis: asymmetric thresholds (different turn-on vs turn-off bands)
- Minimum relay ON/OFF time = 30 seconds (configurable in config.h)
- Bottom fan pushes tent air INTO THE ROOM (energy recycling of grow-light heat); NOT an intake
- VPD = SVP(leaf temp) − actual vapour pressure; leaf temp = air − 2°C offset
- 12-sample rolling average on temp/hum/VPD (≈120 s smoothing window) before any control logic
- GrowProfile has separate day/night target ranges (DayNightRange structs)
- LightSchedule: NTP-backed epoch timestamps, survives reboots, NEVER resets automatically
- Seedling = 24/7 lights (no schedule); Veg = 18h/6h; Flower = 12h/12h
- Mode change (Option A): keeps running clock, just applies new phase lengths
- Alert system: ALERT_NTP_MISSING and ALERT_SCHED_OVERDUE shown as banner in UI
- Humidifier runs 24/7 (no night suppression) — night VPD targets are lower so it naturally activates less
- Grow modes and relay settings persist via Preferences NVS
- All 3 relay control sections (Auto, Manual, Timer) always visible in UI — no dimming
- Extra relay (8) visibility toggle stored in localStorage (client-side, no firmware change)

## WiFi / Credentials

- SSID: REDACTED  Password: REDACTED (in config.h)
- Chart.js loaded from CDN (jsDelivr) — requires internet access on the WiFi network

## A/C Control History

- IR blaster REMOVED (2026-04-13) — was causing PANIC-CRASH (IRremoteESP8266 stack overflow)
- A/C is controlled exclusively via physical relay (relay 5 / DEHUMIDIFIER index, GPIO 25)
- irblaster.cpp/h deleted; IRremoteESP8266 library removed from platformio.ini
- GPIO 23 (HW-490) and GPIO 34 (TSOP38238) are now unused — hardware can be disconnected

## WiFi Sensors (added 2026-04-14)

- remotesensor.h/cpp replaced by wifisensors.h/cpp — dynamic list, up to 4 nodes, NVS-stored
- WiFi sensor polling: exponential backoff (30s→5min), accepts temp/hum/temperature/humidity/t/h JSON fields
- New "Sensors" tab in UI — add/remove/toggle sensors, T/H readings, camera-launch button per sensor
- Camera tab: auto-reconnect on stream drop (8s retry), watchdog for frozen streams

## NTP / Timezone

- UTC+2 / DST +1h (Israel)
- NTP_GMT_OFFSET_SEC=7200, NTP_DST_OFFSET_SEC=3600

## Auto Logic for Relays 5–8

- A/C relay (relay 5, index 4): ON/OFF based on tempMax threshold
- Heat Mat: ON when t < tempMin - TEMP_HYST; cutoff if tempHigh
- Watering: always OFF in AUTO unless precision irrigation triggers it
- Dehumidifier (relay 8, index 7): ON when h > humMax + HUMIDITY_HYST; interlocked (never runs with humidifier)
- Conflict rule 3: dehumidifier wins hard interlock over humidifier

## Flash Procedure (PlatformIO)

1. `pio run --target upload --environment ota` — firmware (OTA)
- NEVER run `uploadfs` via OTA — it wipes logs.csv and irrig.csv (all sensor history lost)
- index.html is gzip-embedded in firmware via scripts/build_html.py → src/ui_html.h
- LittleFS partition is used ONLY for logs.csv and irrig.csv (sensor + irrigation history)
- `uploadfs` is only valid on first cable flash of a blank device

## Flash Procedure (Arduino IDE)

- Entry point: VPDcontrol.ino (project root)
- Libraries: DHT sensor library, Adafruit Unified Sensor, ESP Async WebServer, AsyncTCP, ArduinoJson
- Board: ESP32 Dev Module | Partition: Default 4MB with spiffs | Upload: 921600
- Web UI upload: Tools → ESP32 LittleFS Data Upload (requires arduino-esp32fs-plugin)
