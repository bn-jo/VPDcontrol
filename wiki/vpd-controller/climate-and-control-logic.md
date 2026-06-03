# VPD Controller — Climate and Control Logic

> Sources: Ben Guetta, 2026-05-21
> Raw: [VPD Controller Project Overview](../../raw/vpd-controller/2026-05-21-vpd-controller-project.md)

## Overview

Climate control follows a three-tier priority hierarchy: VPD is primary, temperature is secondary, and humidity is tertiary. Each relay runs with hysteresis to prevent rapid cycling, and relay conflicts are resolved by explicit interlock rules.

## VPD Calculation

```
leaf_temp   = air_temp - 2.0°C
SVP(T)      = 0.6108 * exp(17.27 * T / (T + 237.3))  [Magnus formula, kPa]
actual_VP   = SVP(air_temp) * (RH / 100)
VPD         = SVP(leaf_temp) - actual_VP
```

A 12-sample rolling average (~120 seconds) is applied to raw sensor readings before VPD is computed, preventing transient spikes from triggering relay changes.

## Grow Profiles

Five grow modes, each with separate day/night target ranges (`DayNightRange` structs):

| Mode | Lights | VPD Target | Notes |
|------|--------|------------|-------|
| Seedling | 24/7 | Low | No light schedule; constant VPD |
| Veg | 18h/6h | Medium | |
| Bloom | 12h/12h | Higher | |
| Flush | 12h/12h | Similar to Bloom | |
| Drying | User-set | Humidity focus | Fast/Slow drying mode toggle |

Mode and day-count persist in NVS; light schedule uses NTP epoch timestamps (never auto-resets on reboot).

## Relay Auto Logic

### Fans (relays 1–2)
Adjusted to modulate VPD toward target by controlling air exchange rate.

### Humidifier (relay 3)
Runs continuously in Auto mode; VPD target naturally constrains it — lower night targets mean it runs less at night without explicit night suppression.

### A/C (relay 5, GPIO 25)
ON when temperature exceeds `tempMax`. Replaces former IR blaster approach (removed 2026-04-13 due to stack overflow crashes).

### Heat Mat (relay 6)
ON when `temp < tempMin - TEMP_HYST`. Hard cutoff if temperature is too high.

### Watering (relay 7)
Always OFF in Auto unless precision irrigation system triggers it based on soil moisture readings.

### Dehumidifier (relay 8)
ON when `humidity > humMax + HUMIDITY_HYST`.

## Interlock Rules

1. Dehumidifier (relay 8) and humidifier (relay 3) never run simultaneously
2. **Dehumidifier wins** — it takes hard priority over humidifier when both conditions are met
3. A/C relay has no direct interlock with other relays

## Light Schedule

- Seedling: constant ON (no schedule)
- Veg/Bloom/Flush/Drying: NTP epoch timestamps for on/off boundaries
- Schedule survives reboots (stored in NVS as epoch timestamps)
- Alert `ALERT_SCHED_OVERDUE` fires if schedule is more than 1 hour past expected state
- Alert `ALERT_NTP_MISSING` fires if NTP has never synced

## See Also

- [System Architecture](system-architecture.md)
- [Hardware Reference](hardware-reference.md)
- [Web UI and API](web-ui-and-api.md)
