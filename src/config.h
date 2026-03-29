#pragma once
#include <Arduino.h>

// ─── GPIO ────────────────────────────────────────────────────────────────────
#define DHT_PIN              4   // GPIO4 — reliable general-purpose IO, not used by any peripheral
#define DHT_TYPE            DHT22

// Relay pins — active LOW (relay board energises on LOW signal)
#define RELAY_ACTIVE_LOW    false   // NO (Normally Open) wiring — HIGH = relay energised

#define RELAY_TOP_FAN_PIN      26   // Relay 1: Top exhaust fan (main airflow)
#define RELAY_BOTTOM_FAN_PIN   27   // Relay 2: Bottom fan (heat extraction, NOT intake)
#define RELAY_HUMIDIFIER_PIN   14   // Relay 3: Humidifier / fogger
#define RELAY_LIGHTS_PIN       22   // Relay 4: Grow lights  (was 12 — GPIO 12 is a boot strapping pin)
#define RELAY_DEHUMIDIFIER_PIN 25   // Relay 5: Dehumidifier
#define RELAY_HEAT_MAT_PIN     33   // Relay 6: Heat mat (root zone heating)
#define RELAY_WATERING_PIN     32   // Relay 7: Watering / irrigation
#define RELAY_EXTRA_PIN        13   // Relay 8: Spare / future device

// ─── Soil moisture sensor (capacitive, analog) ────────────────────────────────
// GPIO 35 = ADC1_CH7 — input-only, no WiFi conflict
#define SOIL_PIN               35
#define SOIL_ADC_DRY         2800   // Raw ADC when probe is in dry air (~0 %)
#define SOIL_ADC_WET          800   // Raw ADC when probe is submerged in water (~100 %)
#define SOIL_SAMPLES            8   // Reads averaged per measurement
#define SOIL_INTERVAL_MS     5000UL // Measure every 5 s

// ─── WiFi ────────────────────────────────────────────────────────────────────
#define WIFI_SSID           "REDACTED"
#define WIFI_PASSWORD       "REDACTED"
#define WIFI_TIMEOUT_MS     20000UL

// ─── Remote sensor node ───────────────────────────────────────────────────────
// Second ESP32 sensor at this address. If unreachable, main sensor is used alone.
#define REMOTE_SENSOR_URL          "http://192.168.1.204"
#define REMOTE_SENSOR_INTERVAL_MS  30000UL  // fetch every 30 s
#define REMOTE_SENSOR_TIMEOUT_MS   2000     // 2 s HTTP timeout

// Static IP — ESP32 will always appear at this address on your network.
// If you ever get a conflict, change STATIC_IP to another unused address.
#define STATIC_IP          192,168,1,200
#define STATIC_GATEWAY     192,168,1,1
#define STATIC_SUBNET      255,255,255,0
#define STATIC_DNS         8,8,8,8

// ─── Timing ──────────────────────────────────────────────────────────────────
#define SENSOR_INTERVAL_MS    10000UL   // Read sensor every 10 s
#define CONTROL_INTERVAL_MS   30000UL   // Run climate logic every 30 s
#define LOG_INTERVAL_MS      300000UL   // Log to flash every 5 min
#define WS_PUSH_INTERVAL_MS    2000UL   // Push WebSocket state every 2 s
#define MIN_RELAY_ON_MS             30000UL   // Minimum relay ON time
#define MIN_RELAY_OFF_MS            30000UL   // Minimum relay OFF time
#define LIGHTS_MANUAL_TIMEOUT_SEC    1200UL   // Auto-revert lights to AUTO after 20 min

// ─── Hysteresis ──────────────────────────────────────────────────────────────
#define TEMP_HYST       0.5f    // °C deadband around setpoints
#define HUMIDITY_HYST   3.0f    // %RH deadband
#define VPD_HYST        0.05f   // kPa deadband

// ─── Predictive VPD control ───────────────────────────────────────────────────
// dVPD/dt is computed over a 60 s window (6 × SENSOR_INTERVAL_MS).
// The projected VPD used for control = current VPD + trend × lookahead.
#define VPD_LOOKAHEAD_MIN   2.0f    // minutes to project ahead
#define VPD_TREND_MIN       0.03f   // kPa/min — ignore trend below this (noise floor)

// ─── Sensor ──────────────────────────────────────────────────────────────────
#define LEAF_TEMP_OFFSET    -2.0f   // Leaf is ~2°C cooler than air
#define TEMP_CAL_OFFSET      0.0f   // °C  — adjust if sensor reads high/low
#define HUM_CAL_OFFSET       0.0f   // %RH — adjust if sensor reads consistently high/low
#define SENSOR_STALE_MS    60000UL  // Use last reading for up to 60 s on failure

// ─── Logging ─────────────────────────────────────────────────────────────────
#define LOG_FILE_PATH       "/logs.csv"
#define LOG_MAX_ENTRIES     2016    // 7 days at 5-min intervals
#define LOG_TRIM_TARGET     1008    // Keep 3.5 days after trim
#define LOG_RESPONSE_BUF    40960   // 40 KB response buffer (covers ~24 h)

// ─── Auto-Tune ───────────────────────────────────────────────────────────────
// Step-response characterisation: BASELINE (relay OFF) → ON → COOLDOWN per relay.
// Total time per relay ≈ 7 min; full run (5 relays) ≈ 35 min.
#define AT_BASELINE_MS  120000UL   // 2 min — measure ambient before turning ON
#define AT_ON_MS        180000UL   // 3 min — relay ON, measure effect
#define AT_COOLDOWN_MS  120000UL   // 2 min — relay OFF, environment recovers

// ─── NTP ─────────────────────────────────────────────────────────────────────
#define NTP_SERVER          "pool.ntp.org"
#define NTP_GMT_OFFSET_SEC  7200    // UTC+2 (Israel/Eastern Europe standard time)
#define NTP_DST_OFFSET_SEC  3600    // +1 h daylight saving (summer → UTC+3)
