# VPD Controller — Hardware Reference

> Sources: Ben Guetta, 2026-05-21
> Raw: [Hardware Config](../../raw/vpd-controller/2026-05-21-hardware-config.md); [VPD Controller Project Overview](../../raw/vpd-controller/2026-05-21-vpd-controller-project.md)

## Overview

The controller runs on an ESP32 Dev Module connected to a single 8-channel relay module, a DHT22 sensor, and a capacitive soil moisture probe. Two previously-used IR pins are now unused after IR blaster removal.

## GPIO Pin Map

| GPIO | Component | Notes |
|------|-----------|-------|
| 4 | DHT22 (temp/humidity) | DHTesp library |
| 26 | Relay 1 — Top Fan | |
| 27 | Relay 2 — Bottom Fan | Exhausts TO THE ROOM (not intake); recycles grow-light heat |
| 14 | Relay 3 — Humidifier | Active 24/7; night VPD targets throttle duty naturally |
| 22 | Relay 4 — Lights | Previously GPIO 12 (ESP32 boot pin — do not use) |
| 25 | Relay 5 — A/C | Physical relay for A/C unit |
| 33 | Relay 6 — Heat Mat | |
| 32 | Relay 7 — Watering | Controlled by precision irrigation logic |
| 13 | Relay 8 — Extra/Dehumidifier | High-humidity correction; visibility toggleable in UI |
| 35 | Soil moisture (ADC1_CH7) | Input-only pin; capacitive probe |
| 23 | **UNUSED** | Was IR emitter HW-490 — can disconnect |
| 34 | **UNUSED** | Was IR receiver TSOP38238 — can disconnect |

## Soil Moisture Calibration

The ADC reads a raw value that maps linearly to moisture percentage:

- `SOIL_ADC_DRY = 2800` → 0%
- `SOIL_ADC_WET = 800` → 100%
- `SOIL_SAMPLES = 8` readings averaged per measurement

Recalibrate by pressing Dry/Wet buttons in the UI's sensor calibration panel while the probe is in known-dry or fully-saturated substrate.

## Relay Behavior

- All relays **active LOW** (`RELAY_ACTIVE_LOW = true`)
- Minimum ON time: 30 seconds
- Minimum OFF time: 30 seconds
- Hysteresis: asymmetric — separate thresholds for turn-on vs turn-off

## Important Pin Gotcha

**GPIO 12 is an ESP32 boot strapping pin.** Lights were previously on GPIO 12 and caused intermittent boot failures. Always use GPIO 22 for the lights relay.

## See Also

- [System Architecture](system-architecture.md)
- [Climate and Control Logic](climate-and-control-logic.md)
