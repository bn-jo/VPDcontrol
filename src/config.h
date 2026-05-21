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
#define RELAY_EXTRA_PIN        13   // Relay 8: Spare / extra device


// ─── Intake air sensor (DHT11 — room outside the tent) ───────────────────────
// Wired to GPIO16 (free general-purpose IO; GPIO15 has pull-down that blocks DHT).
#define INTAKE_SENSOR_PIN          16
#define INTAKE_SENSOR_INTERVAL_MS  30000UL   // read every 30 s (DHT11 min ~1 s)

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
#define WIFI_ROAM_INTERVAL_MS   300000UL  // check for a better AP every 5 min
#define WIFI_ROAM_RSSI_MIN        -72     // dBm — only scan if signal is weaker than this
#define WIFI_ROAM_MIN_GAIN_DB       8     // only switch if new AP is ≥8 dBm stronger

// ─── WiFi sensor nodes ────────────────────────────────────────────────────────
// Managed at runtime via the Sensors tab in the web UI (stored in NVS).
// Constants are defined in wifisensors.h — nothing needed here.

// Built-in sensor: always registered, always active, cannot be removed via UI.
// Change the URL path if your sensor serves JSON on a different endpoint.
#define BUILTIN_SENSOR_NAME  "Sensor 2"
#define BUILTIN_SENSOR_URL   "http://192.168.1.204/data"

// Static IP — ESP32 will always appear at this address on your network.
// If you ever get a conflict, change STATIC_IP to another unused address.
#define STATIC_IP          192,168,1,200
#define STATIC_GATEWAY     192,168,1,1
#define STATIC_SUBNET      255,255,255,0

// ─── Remote access auth (HTTP Basic Auth for non-local connections) ───────────
// Requests from 192.168.x.x / 10.x.x.x / 172.16-31.x.x bypass auth.
// Change these before exposing the device to the internet.
#define WEB_AUTH_USER      "REDACTED"
#define WEB_AUTH_PASS      "REDACTED"
#define STATIC_DNS         8,8,8,8

// ─── Timing ──────────────────────────────────────────────────────────────────
#define SENSOR_INTERVAL_MS    10000UL   // Read sensor every 10 s
#define CONTROL_INTERVAL_MS   30000UL   // Run climate logic every 30 s
#define LOG_INTERVAL_MS      300000UL   // Log to flash every 5 min
#define WS_PUSH_INTERVAL_MS    2000UL   // Push WebSocket state every 2 s
#define MIN_RELAY_ON_MS             30000UL   // Minimum relay ON time
#define MIN_RELAY_OFF_MS            30000UL   // Minimum relay OFF time
#define LIGHTS_MANUAL_TIMEOUT_SEC    1200UL   // Auto-revert lights to AUTO after 20 min

// ─── Ceramic heater pulse (night auto mode) ───────────────────────────────────
// When lights are OFF and temp is low or humidity is high, the heater runs in
// short pulses instead of sustained ON to prevent overshoot.
// Tune these if the tent reacts too fast or too slow.
#define HEAT_PULSE_ON_MS    45000UL   //  45 s  ON  per pulse
#define HEAT_PULSE_REST_MS 420000UL   //   7 min OFF between pulses

// ─── Hysteresis ──────────────────────────────────────────────────────────────
#define TEMP_HYST       0.5f    // °C deadband around setpoints
#define HUMIDITY_HYST   3.0f    // %RH deadband
#define VPD_HYST        0.05f   // kPa deadband

// ─── A/C pre-shutdown humidifier guard ───────────────────────────────────────
// When A/C temperature drops within this margin of its shutoff point, the
// humidifier is suppressed — the A/C is about to stop dehumidifying and
// humidity will spike; adding moisture now makes the spike worse.
#define AC_PRESHUTDOWN_MARGIN  2.0f   // °C


// ─── Flowering sub-stages ─────────────────────────────────────────────────────
// Days 1-FLOWER_EARLY_DAYS = Early Flower; Day FLOWER_EARLY_DAYS+1 = auto-switch to Late Flower
#define FLOWER_EARLY_DAYS  21

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

// ─── Precision Irrigation ─────────────────────────────────────────────────────
#define IRRIG_LOG_FILE   "/irrig.csv"
#define IRRIG_LOG_MAX    200          // irrigation events kept in flash
#define IRRIG_LOG_TRIM   100          // trim target when full

#define MAX_PLANTS  16

struct IrrigationProfile {
    bool     enabled;           // false = no auto-watering (e.g. Drying stage)
    uint8_t  soilTriggerPct;    // start cycle when soil drops below this %
    uint8_t  soilTargetPct;     // stop when soil reaches this % — checked only after each soak pause
    uint32_t maxWaterSec;       // safety cutoff: total valve-on time across all pulses
    uint32_t minRestDaySec;     // minimum rest between full sessions (lights ON)
    uint32_t minRestNightSec;   // minimum rest between full sessions (lights OFF)
    uint32_t pulseOnSec;        // valve-open time per pulse (0 = continuous legacy mode)
    uint32_t pauseSec;          // soak pause between pulses (soil sensor checked at end of soak)
};

struct PlantEntry  { uint16_t potVolumeL; };   // 1 US gal ≈ 3.785 L

struct PlantConfig {
    uint8_t    count;                  // 1–MAX_PLANTS
    PlantEntry plants[MAX_PLANTS];
    uint8_t    substrateType;          // 0=soil  1=coco/perlite  2=perlite
    bool       precisionEnabled;       // master switch (false = legacy threshold mode)
};

// Substrate water-holding fraction (fraction of pot volume that actually holds water)
static const float SUBSTRATE_HOLD_CAP[3] = { 0.40f, 0.30f, 0.20f };

// Substrate-aware defaults — IRRIG_DEFAULTS[substrate][stage]
//   substrate: 0=Soil  1=Coco/Perlite  2=Perlite
//   stage:     0=Seedling  1=Veg  2=Flower  3=Drying
// Night rest = 86400 s (24 h) → effectively no irrigation after lights-off.
// Pulse/soak: valve opens for pulseOnSec, closes for pauseSec, repeats until
//   soil hits target (checked only at end of each soak) or maxWaterSec on-time is used.
//   { enabled, triggerPct, targetPct, maxSec, dayRestSec, nightRestSec, pulseOnSec, pauseSec }
static const IrrigationProfile IRRIG_DEFAULTS[3][4] = {
  // ── Soil ──────────────────────────────────────────────────────────────────
  // Soil soaks slowly — 40 s pulses / 60 s soak gives water time to distribute.
  {
    { true,  35, 48,  60,  5400, 86400, 40, 60 },  // Seedling — gentle, ~2-3 shots/day
    { true,  30, 45, 120,  3600, 86400, 40, 60 },  // Veg      — moderate dry-back
    { true,  28, 42, 180,  3600, 86400, 40, 60 },  // Flower   — deeper dry-back (generative)
    { false,  0,  0,   0,     0,     0,  0,  0 },  // Drying   — off
  },
  // ── Coco / Perlite mix ────────────────────────────────────────────────────
  // Coco absorbs quickly — 20 s pulses / 45 s soak.
  {
    { true,  48, 62,  60,  3600, 86400, 20, 45 },  // Seedling — small shots, 1/h max
    { true,  50, 65, 120,  1800, 86400, 20, 45 },  // Veg      — frequent shots (6-8/day)
    { true,  44, 62, 180,  1800, 86400, 20, 45 },  // Flower   — deeper dry-back for resin
    { false,  0,  0,   0,     0,     0,  0,  0 },  // Drying   — off
  },
  // ── Pure Perlite ──────────────────────────────────────────────────────────
  // Drains very fast — 15 s pulses / 30 s soak.
  {
    { true,  55, 70,  60,  2700, 86400, 15, 30 },  // Seedling
    { true,  52, 68,  90,  1800, 86400, 15, 30 },  // Veg
    { true,  48, 65, 120,  1800, 86400, 15, 30 },  // Flower
    { false,  0,  0,   0,     0,     0,  0,  0 },  // Drying   — off
  },
};

// ─── Remote data collector ────────────────────────────────────────────────────
// Server that stores unlimited sensor history. Push happens every LOG_INTERVAL_MS.
// Set to "" to disable remote logging.
#define REMOTE_LOG_URL      "http://178.104.6.12"
#define REMOTE_LOG_TIMEOUT  2000   // ms — HTTP timeout for push (fire-and-forget)

// ─── NTP ─────────────────────────────────────────────────────────────────────
#define NTP_SERVER          "pool.ntp.org"
#define NTP_GMT_OFFSET_SEC  7200    // UTC+2 (Israel/Eastern Europe standard time)
#define NTP_DST_OFFSET_SEC  3600    // +1 h daylight saving (summer → UTC+3)
