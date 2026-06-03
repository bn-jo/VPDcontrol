#include "wifisensors.h"
#include "config.h"
#include "syslog.h"
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_task_wdt.h>

WifiSensorManager wifiSensors;

// ─── Init ─────────────────────────────────────────────────────────────────────
void WifiSensorManager::begin() {
    loadPrefs();
    int n = 0;
    for (int i = 0; i < WIFI_SENSOR_MAX; i++) if (_s[i].enabled) n++;
    rlog("[WSENS] Loaded %d sensor(s)", n);
}

// ─── Main poll loop ───────────────────────────────────────────────────────────
void WifiSensorManager::update() {
    if (WiFi.status() != WL_CONNECTED) return;
    unsigned long now = millis();
    for (int i = 0; i < WIFI_SENSOR_MAX; i++) {
        if (!_s[i].enabled || _s[i].sensorUrl[0] == '\0') continue;
        if (now < _s[i].nextPollMs) continue;
        _poll(i);
    }
}

void WifiSensorManager::_poll(int id) {
    WifiSensor& s = _s[id];
    unsigned long now = millis();

    HTTPClient http;
    http.setReuse(false);                   // don't reuse — avoids stale-socket hangs
    http.setConnectTimeout(WIFI_SENSOR_TIMEOUT);
    http.setTimeout(WIFI_SENSOR_TIMEOUT);
    http.begin(s.sensorUrl);
    int code = http.GET();
    esp_task_wdt_reset();   // pet watchdog — GET() can block up to WIFI_SENSOR_TIMEOUT

    if (code == HTTP_CODE_OK) {
        static JsonDocument doc;  // static: no heap churn on repeated polls
        doc.clear();
        if (deserializeJson(doc, http.getStream()) == DeserializationError::Ok) {
            // Accept "temp"/"hum", "temperature"/"humidity", or "t"/"h"
            float t = doc["temp"]        | doc["temperature"] | doc["t"] | -999.0f;
            float h = doc["hum"]         | doc["humidity"]    | doc["h"] | -999.0f;
            if (t > -100.0f && h > 0.0f && h <= 100.0f) {
                s.temperature = t;
                s.humidity    = h;
                s.valid       = true;
                s.failCount   = 0;
                s.nextPollMs  = now + WIFI_SENSOR_POLL_MS;
                http.end();
                return;
            }
        }
    }

    // Failure path
    http.end();
    s.failCount++;
    // Grace period: keep last valid reading for the first WIFI_SENSOR_GRACE_FAILS failures.
    // This prevents a single timeout from immediately showing the sensor as disconnected.
    if (s.failCount > WIFI_SENSOR_GRACE_FAILS) {
        s.valid = false;
    }
    // Back-off: 30 s → 60 s, then stay at 60 s (not 5 min — sensor is usually live)
    unsigned long backoff = min(WIFI_SENSOR_POLL_MS * s.failCount, (unsigned long)WIFI_SENSOR_BACKOFF);
    s.nextPollMs = now + backoff;

    rlog("[WSENS] %s fail#%u (HTTP %d) — retry in %lus%s",
         s.name, s.failCount, code, backoff / 1000UL,
         s.valid ? " [keeping last reading]" : " [marked invalid]");
}

// ─── Average across all valid+enabled+sensorActive sensors ───────────────────
bool WifiSensorManager::getAverage(float& t, float& h) const {
    float sumT = 0, sumH = 0;
    int   cnt  = 0;
    for (int i = 0; i < WIFI_SENSOR_MAX; i++) {
        if (_s[i].enabled && _s[i].sensorActive && _s[i].valid) {
            sumT += _s[i].temperature;
            sumH += _s[i].humidity;
            cnt++;
        }
    }
    if (!cnt) return false;
    t = sumT / cnt;
    h = sumH / cnt;
    return true;
}

// ─── Management ──────────────────────────────────────────────────────────────
bool WifiSensorManager::add(const char* name, const char* sensorUrl) {
    // Update existing entry with same name (any state), else find empty slot
    int slot = -1;
    for (int i = 0; i < WIFI_SENSOR_MAX; i++) {
        if (_s[i].name[0] && strncmp(_s[i].name, name, WIFI_SENSOR_NAME_LEN) == 0) {
            slot = i; break;
        }
    }
    if (slot < 0) {
        for (int i = 0; i < WIFI_SENSOR_MAX; i++) {
            if (!_s[i].name[0]) { slot = i; break; }
        }
    }
    if (slot < 0) return false;  // all 4 slots occupied

    strncpy(_s[slot].name,      name,                       WIFI_SENSOR_NAME_LEN - 1);
    strncpy(_s[slot].sensorUrl, sensorUrl ? sensorUrl : "", WIFI_SENSOR_URL_LEN  - 1);
    _s[slot].name[WIFI_SENSOR_NAME_LEN-1]     = '\0';
    _s[slot].sensorUrl[WIFI_SENSOR_URL_LEN-1] = '\0';
    _s[slot].enabled      = true;
    _s[slot].sensorActive = true;
    _s[slot].valid        = false;
    _s[slot].failCount    = 0;
    _s[slot].nextPollMs   = 0;
    _prefsDirty = true;
    rlog("[WSENS] Added '%s' sensor=%s", name,
         sensorUrl && sensorUrl[0] ? sensorUrl : "(none)");
    return true;
}

bool WifiSensorManager::remove(int id) {
    if (id < 0 || id >= WIFI_SENSOR_MAX) return false;
    rlog("[WSENS] Removed '%s'", _s[id].name);
    memset(&_s[id], 0, sizeof(WifiSensor));
    _prefsDirty = true;
    return true;
}

bool WifiSensorManager::setEnabled(int id, bool en) {
    if (id < 0 || id >= WIFI_SENSOR_MAX) return false;
    _s[id].enabled = en;
    if (!en) { _s[id].valid = false; _s[id].failCount = 0; }
    else      { _s[id].nextPollMs = 0; }  // poll immediately when re-enabled
    _prefsDirty = true;
    return true;
}

bool WifiSensorManager::setSensorActive(int id, bool active) {
    if (id < 0 || id >= WIFI_SENSOR_MAX) return false;
    _s[id].sensorActive = active;
    _prefsDirty = true;
    rlog("[WSENS] '%s' sensor data %s", _s[id].name, active ? "enabled" : "disabled");
    return true;
}

// ─── JSON serializer ─────────────────────────────────────────────────────────
int WifiSensorManager::getJson(char* buf, size_t bufSize) const {
    size_t w = 0;
    w += snprintf(buf + w, bufSize - w, "[");
    bool first = true;
    for (int i = 0; i < WIFI_SENSOR_MAX; i++) {
        if (_s[i].name[0] == '\0') continue;  // skip truly empty slots
        if (w > bufSize - 200) break;
        char tempStr[12] = "null", humStr[12] = "null";
        if (_s[i].valid) {
            snprintf(tempStr, sizeof(tempStr), "%.1f", _s[i].temperature);
            snprintf(humStr,  sizeof(humStr),  "%.1f", _s[i].humidity);
        }
        w += snprintf(buf + w, bufSize - w,
            "%s{\"id\":%d,\"name\":\"%s\",\"sensorUrl\":\"%s\","
            "\"temp\":%s,\"hum\":%s,"
            "\"valid\":%s,\"enabled\":%s,\"sensorActive\":%s}",
            first ? "" : ",", i,
            _s[i].name, _s[i].sensorUrl,
            tempStr, humStr,
            _s[i].valid        ? "true" : "false",
            _s[i].enabled      ? "true" : "false",
            _s[i].sensorActive ? "true" : "false");
        first = false;
    }
    w += snprintf(buf + w, bufSize - w, "]");
    return (int)w;
}

// ─── NVS persistence ─────────────────────────────────────────────────────────
void WifiSensorManager::savePrefs() {
    Preferences p;
    p.begin("wsens", false);
    for (int i = 0; i < WIFI_SENSOR_MAX; i++) {
        char k[8];
        snprintf(k, sizeof(k), "n%d",  i); p.putString(k, _s[i].name);
        snprintf(k, sizeof(k), "u%d",  i); p.putString(k, _s[i].sensorUrl);
        snprintf(k, sizeof(k), "e%d",  i); p.putBool  (k, _s[i].enabled);
        snprintf(k, sizeof(k), "sa%d", i); p.putBool  (k, _s[i].sensorActive);
    }
    p.end();
}

void WifiSensorManager::flushPrefsIfDirty() {
    if (!_prefsDirty) return;
    _prefsDirty = false;
    savePrefs();
}

void WifiSensorManager::loadPrefs() {
    Preferences p;
    p.begin("wsens", true);
    for (int i = 0; i < WIFI_SENSOR_MAX; i++) {
        char k[8];
        snprintf(k, sizeof(k), "n%d", i); {
            String v = p.getString(k, "");
            strncpy(_s[i].name, v.c_str(), WIFI_SENSOR_NAME_LEN - 1);
        }
        snprintf(k, sizeof(k), "u%d", i); {
            String v = p.getString(k, "");
            strncpy(_s[i].sensorUrl, v.c_str(), WIFI_SENSOR_URL_LEN - 1);
        }
        snprintf(k, sizeof(k), "e%d",  i); _s[i].enabled      = p.getBool(k, false);
        snprintf(k, sizeof(k), "sa%d", i); _s[i].sensorActive  = p.getBool(k, true);
    }
    p.end();
}
