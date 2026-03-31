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
    if (millis() - _lastMs < INTAKE_SENSOR_INTERVAL_MS) return;
    _lastMs = millis();

    TempAndHumidity th = dht11.getTempAndHumidity();

    if (dht11.getStatus() != DHTesp::ERROR_NONE || isnan(th.temperature) || isnan(th.humidity)) {
        _d.valid = false;
        Serial.printf("[INTAKE] Read failed (GPIO%d)\n", INTAKE_SENSOR_PIN);
        return;
    }

    _d.temperature = th.temperature;
    _d.humidity    = th.humidity;
    _d.valid       = true;
    Serial.printf("[INTAKE] T=%.1f°C  H=%.1f%%\n", _d.temperature, _d.humidity);
}
