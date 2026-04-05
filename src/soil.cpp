#include "soil.h"
#include "config.h"
#include <Preferences.h>

SoilSensor soil;

void SoilSensor::begin() {
    // GPIO 35 is input-only — ADC1_CH7, no WiFi conflict
    analogSetPinAttenuation(SOIL_PIN, ADC_11db);  // Full 0–3.3V range
    _adcDry = SOIL_ADC_DRY;  // compile-time defaults
    _adcWet = SOIL_ADC_WET;
    _d      = {};
    _lastMs = 0;
    loadCalibPrefs();         // override with saved calibration if present
    Serial.printf("[SOIL] Sensor initialised on GPIO %d  dry=%d wet=%d\n",
                  (int)SOIL_PIN, _adcDry, _adcWet);
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
    _rawAdc = (int)(sum / SOIL_SAMPLES);

    // Capacitive probe: dry (_adcDry) → 0 %,  wet (_adcWet) → 100 %
    float pct = 100.0f * (float)(_adcDry - _rawAdc)
                       / (float)(_adcDry  - _adcWet);
    _d.moisture = constrain(pct, 0.0f, 100.0f);
    _d.valid    = true;

    Serial.printf("[SOIL] raw=%d  moisture=%.1f%%\n", _rawAdc, _d.moisture);
}

void SoilSensor::setCalib(int dry, int wet) {
    if (dry <= wet || dry < 100 || wet < 100) return;   // sanity check
    _adcDry = dry;
    _adcWet = wet;
    saveCalibPrefs();
    Serial.printf("[SOIL] Calibration saved: dry=%d wet=%d\n", dry, wet);
}

void SoilSensor::loadCalibPrefs() {
    Preferences prefs;
    prefs.begin("soil", true);
    _adcDry = prefs.getInt("adcDry", _adcDry);
    _adcWet = prefs.getInt("adcWet", _adcWet);
    prefs.end();
}

void SoilSensor::saveCalibPrefs() {
    Preferences prefs;
    prefs.begin("soil", false);
    prefs.putInt("adcDry", _adcDry);
    prefs.putInt("adcWet", _adcWet);
    prefs.end();
}
