#include "intakesensor.h"
#include "config.h"
#include <DHTesp.h>

IntakeSensor intakeSensor;

static DHTesp dht11;

void IntakeSensor::begin() {
    dht11.setup(INTAKE_SENSOR_PIN, DHTesp::DHT11);
    Serial.printf("[INTAKE] DHT11 on GPIO%d\n", INTAKE_SENSOR_PIN);
}

void IntakeSensor::update() {
    // After a failure, retry every 5 s instead of waiting the full 30 s.
    // First read: wait 3 s after boot so the sensor stabilises.
    unsigned long interval;
    if (_lastMs == 0)        interval = 3000UL;
    else if (_failCount > 0) interval = 5000UL;
    else                     interval = INTAKE_SENSOR_INTERVAL_MS;

    if (millis() - _lastMs < interval) return;
    _lastMs = millis();

    TempAndHumidity th = dht11.getTempAndHumidity();

    if (dht11.getStatus() != DHTesp::ERROR_NONE || isnan(th.temperature) || isnan(th.humidity)) {
        _failCount = (_failCount < 255) ? _failCount + 1 : 255;
        // Keep last reading valid for up to 2 minutes — DHT11 has a ~5-10% per-read
        // failure rate; a single bad read should not blank out the display or affect
        // climate decisions until the sensor is genuinely absent.
        const unsigned long staleMs = 120000UL;  // 2 min
        if (_lastValidMs > 0 && (millis() - _lastValidMs) < staleMs) {
            // Data stays valid; nothing changes
        } else {
            _d.valid = false;
        }
        if (_failCount <= 3 || _failCount % 12 == 0) {  // log first 3, then every 60 s
            Serial.printf("[INTAKE] Read failed #%u (GPIO%d)%s\n",
                          _failCount, INTAKE_SENSOR_PIN,
                          _d.valid ? " — using stale data" : " — no data");
        }
        return;
    }

    _d.temperature = th.temperature;
    _d.humidity    = th.humidity;
    _d.valid       = true;
    _lastValidMs   = millis();
    _failCount     = 0;
    Serial.printf("[INTAKE] T=%.1f°C  H=%.1f%%\n", _d.temperature, _d.humidity);
}
