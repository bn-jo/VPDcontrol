# VPD Controller — Hardware Pins and Configuration

> Source: /Users/liat/Documents/VPDcontrol/src/config.h (codebase)
> Collected: 2026-05-21
> Published: Unknown

## GPIO Pin Assignments

| Pin | Component | Notes |
|-----|-----------|-------|
| GPIO 4 | DHT22 temperature/humidity sensor | Using DHTesp library |
| GPIO 26 | Relay 1 — Top Fan | |
| GPIO 27 | Relay 2 — Bottom Fan | Pushes air OUT of grow room (heat recycling) |
| GPIO 14 | Relay 3 — Humidifier | Runs 24/7; night VPD targets suppress it naturally |
| GPIO 22 | Relay 4 — Lights | Was GPIO 12 (boot strapping pin — changed) |
| GPIO 25 | Relay 5 — A/C | Physical relay for A/C unit; was "Dehumidifier" |
| GPIO 33 | Relay 6 — Heat Mat | ON when temp < tempMin - TEMP_HYST |
| GPIO 32 | Relay 7 — Watering | AUTO=off unless precision irrigation triggers |
| GPIO 13 | Relay 8 — Extra/Dehumidifier | Humidity control relay; visibility toggle in UI |
| GPIO 35 | Soil moisture sensor | ADC1_CH7, input-only, capacitive probe |
| GPIO 23 | UNUSED | Was HW-490 IR emitter — now disconnected |
| GPIO 34 | UNUSED | Was TSOP38238 IR receiver — now disconnected |

## Soil Moisture Calibration

- SOIL_ADC_DRY = 2800 (raw ADC value when fully dry)
- SOIL_ADC_WET = 800 (raw ADC value when fully wet)
- SOIL_SAMPLES = 8 (averaged readings)
- Output: percentage (0–100%)

## Relay Configuration

- All relays ACTIVE LOW (RELAY_ACTIVE_LOW = true)
- Single 8-channel relay module
- Minimum relay ON time: 30 seconds (configurable)
- Minimum relay OFF time: 30 seconds (configurable)

## Control Timing

- Main loop tick: 100ms (millis() state machine, no delay())
- Sensor smoothing: 12-sample rolling average ≈ 120 second window
- VPD calculation: SVP(leaf temp) − actual vapour pressure
- Leaf temp offset: air temp − 2°C

## Data Storage

- LittleFS partition: logs.csv (7-day rolling), irrig.csv (irrigation history)
- Preferences NVS: grow mode, relay settings, VPD target, irrigation profiles
- localStorage (client-side): relay visibility, UI preferences
