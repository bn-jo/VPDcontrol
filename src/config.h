#pragma once
#include <Arduino.h>

// ─── GPIO ────────────────────────────────────────────────────────────────────
#define DHT_PIN             4
#define DHT_TYPE            DHT22

// Relay pins — active LOW (relay board energises on LOW signal)
#define RELAY_ACTIVE_LOW    true

#define RELAY_TOP_FAN_PIN      26   // Relay 1: Top exhaust fan (main airflow)
#define RELAY_BOTTOM_FAN_PIN   27   // Relay 2: Bottom fan (heat extraction, NOT intake)
#define RELAY_HUMIDIFIER_PIN   14   // Relay 3: Humidifier / fogger
#define RELAY_LIGHTS_PIN       12   // Relay 4: Grow lights
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

// ─── Timing ──────────────────────────────────────────────────────────────────
#define SENSOR_INTERVAL_MS    10000UL   // Read sensor every 10 s
#define CONTROL_INTERVAL_MS   30000UL   // Run climate logic every 30 s
#define LOG_INTERVAL_MS      300000UL   // Log to flash every 5 min
#define WS_PUSH_INTERVAL_MS    2000UL   // Push WebSocket state every 2 s
#define MIN_RELAY_ON_MS       30000UL   // Minimum relay ON time
#define MIN_RELAY_OFF_MS      30000UL   // Minimum relay OFF time

// ─── Hysteresis ──────────────────────────────────────────────────────────────
#define TEMP_HYST       0.5f    // °C deadband around setpoints
#define HUMIDITY_HYST   3.0f    // %RH deadband
#define VPD_HYST        0.05f   // kPa deadband

// ─── Sensor ──────────────────────────────────────────────────────────────────
#define LEAF_TEMP_OFFSET    -2.0f   // Leaf is ~2°C cooler than air
#define SENSOR_STALE_MS    60000UL  // Use last reading for up to 60 s on failure

// ─── Logging ─────────────────────────────────────────────────────────────────
#define LOG_FILE_PATH       "/logs.csv"
#define LOG_MAX_ENTRIES     2016    // 7 days at 5-min intervals
#define LOG_TRIM_TARGET     1008    // Keep 3.5 days after trim
#define LOG_RESPONSE_BUF    40960   // 40 KB response buffer (covers ~24 h)

// ─── NTP ─────────────────────────────────────────────────────────────────────
#define NTP_SERVER          "pool.ntp.org"
#define NTP_GMT_OFFSET_SEC  0       // Adjust for your timezone (e.g. 7200 = UTC+2)
#define NTP_DST_OFFSET_SEC  0
