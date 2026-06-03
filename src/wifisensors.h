#pragma once
#include <Arduino.h>

// ─── WiFi Sensor Manager ──────────────────────────────────────────────────────
// Polls up to WIFI_SENSOR_MAX remote ESP nodes for temperature + humidity via HTTP.
// Each node is identified by a sensor data URL.
// Settings persist in NVS ("wsens" namespace). Update() must be called from loop()
// on Core 1 — HTTPClient is not safe on Core 0.

#define WIFI_SENSOR_MAX       4
#define WIFI_SENSOR_NAME_LEN  24
#define WIFI_SENSOR_URL_LEN   80

#define WIFI_SENSOR_POLL_MS       30000UL   // normal poll interval (30 s)
#define WIFI_SENSOR_TIMEOUT        800      // HTTP connect + read timeout (ms) — LAN sensors respond in <100 ms
#define WIFI_SENSOR_BACKOFF       60000UL   // max back-off on repeated failure (60 s)
#define WIFI_SENSOR_GRACE_FAILS   2         // failures before marking invalid (grace period)

struct WifiSensor {
    char  name[WIFI_SENSOR_NAME_LEN] = {};
    char  sensorUrl[WIFI_SENSOR_URL_LEN] = {};  // URL that returns {"temp":X,"hum":Y}
    bool  enabled      = false;  // entry is active (shown in list, sensor polled if sensorUrl set)
    bool  sensorActive = true;   // include T/H readings in climate calculations

    // runtime (not persisted)
    float    temperature = 0.0f;
    float    humidity    = 0.0f;
    bool     valid       = false;
    uint32_t failCount   = 0;
    unsigned long nextPollMs = 0;
};

class WifiSensorManager {
public:
    void begin();   // load NVS
    void update();  // call from loop() — polls due sensors

    // Returns true if at least one enabled sensor is valid.
    // t and h are the average across all valid+enabled sensors.
    bool getAverage(float& t, float& h) const;

    // Management — all persist to NVS immediately.
    // add(): updates existing entry if name matches, else finds empty slot.
    bool add(const char* name, const char* sensorUrl);
    bool remove(int id);
    bool setEnabled(int id, bool en);
    bool setSensorActive(int id, bool active);  // toggle T/H inclusion in climate calculations

    int count() const { return WIFI_SENSOR_MAX; }
    const WifiSensor& get(int id) const { return _s[id]; }

    // Serialize all slots to JSON object array (into pre-allocated buffer).
    int getJson(char* buf, size_t bufSize) const;

    void savePrefs();
    void loadPrefs();
    void flushPrefsIfDirty();  // call from loop() on Core 1 — deferred NVS write

private:
    WifiSensor _s[WIFI_SENSOR_MAX];
    bool _prefsDirty = false;  // set by WS callbacks; flushed from loop()
    void _poll(int id);
};

extern WifiSensorManager wifiSensors;
