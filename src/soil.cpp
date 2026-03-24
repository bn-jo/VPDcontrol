#include "soil.h"
#include "config.h"

SoilSensor soil;

void SoilSensor::begin() {
    // GPIO 35 is input-only — ADC1_CH7, no WiFi conflict
    analogSetPinAttenuation(SOIL_PIN, ADC_11db);  // Full 0–3.3V range
    _d    = {};
    _lastMs = 0;
    Serial.printf("[SOIL] Sensor initialised on GPIO %d\n", (int)SOIL_PIN);
}

void SoilSensor::update() {
    unsigned long now = millis();
    if (now - _lastMs < SOIL_INTERVAL_MS) return;
    _lastMs = now;

    // Oversample — no delay() needed; multiple consecutive reads are fine here
    long sum = 0;
    for (int i = 0; i < SOIL_SAMPLES; i++) {
        sum += analogRead(SOIL_PIN);
    }
    int raw = (int)(sum / SOIL_SAMPLES);

    // Capacitive probe: dry (SOIL_ADC_DRY) → 0 %,  wet (SOIL_ADC_WET) → 100 %
    float pct = 100.0f * (float)(SOIL_ADC_DRY - raw)
                       / (float)(SOIL_ADC_DRY - SOIL_ADC_WET);
    _d.moisture = constrain(pct, 0.0f, 100.0f);
    _d.valid    = true;

    Serial.printf("[SOIL] raw=%d  moisture=%.1f%%\n", raw, _d.moisture);
}
