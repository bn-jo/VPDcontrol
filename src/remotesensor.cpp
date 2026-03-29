#include "remotesensor.h"
#include "config.h"
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <WiFi.h>

RemoteSensor remoteSensor;

void RemoteSensor::update() {
    if (millis() - _lastMs < REMOTE_SENSOR_INTERVAL_MS) return;
    if (WiFi.status() != WL_CONNECTED) return;
    _lastMs = millis();

    HTTPClient http;
    http.begin(REMOTE_SENSOR_URL "/api/state");
    http.setTimeout(REMOTE_SENSOR_TIMEOUT_MS);
    int code = http.GET();

    if (code == HTTP_CODE_OK) {
        JsonDocument doc;
        if (deserializeJson(doc, http.getString()) == DeserializationError::Ok) {
            float t = doc["temp"] | -999.0f;
            float h = doc["hum"]  | -999.0f;
            if (t > -100.0f && h > 0.0f && h <= 100.0f) {
                _d.temperature = t;
                _d.humidity    = h;
                _d.valid       = true;
                Serial.printf("[REMOTE] T=%.1f°C  H=%.1f%%\n", t, h);
            } else {
                _d.valid = false;
            }
        } else {
            _d.valid = false;
        }
    } else {
        _d.valid = false;
        // Only log unexpected failures (not normal connection-refused when device is off)
        if (code > 0)
            Serial.printf("[REMOTE] Fetch failed, HTTP %d\n", code);
    }
    http.end();
}
