#pragma once
#include <Arduino.h>

// ─── Capacitive soil moisture sensor ─────────────────────────────────────────
// Reads an analog capacitive probe (e.g. v1.2 / v2.0).
// Higher ADC count = drier soil.
// Calibration is saved in NVS ("soil" namespace) and adjustable from the web UI.

struct SoilData {
    float moisture;   // 0.0 – 100.0 %  (0 = dry, 100 = saturated)
    bool  valid;      // false until first reading completes
};

class SoilSensor {
public:
    void begin();
    void update();    // Self-throttles; call every loop tick

    const SoilData& data() const { return _d; }

    // ── Calibration ───────────────────────────────────────────────────────────
    int  rawAdc()  const { return _rawAdc; }   // last averaged ADC reading
    int  adcDry()  const { return _adcDry; }   // calibrated dry endpoint
    int  adcWet()  const { return _adcWet; }   // calibrated wet endpoint
    void setCalib(int dry, int wet);            // update calibration (deferred NVS write)
    void flushCalibIfDirty();                   // call from loop() on Core 1

private:
    SoilData      _d         = {};
    unsigned long _lastMs    = 0;
    int           _rawAdc    = 0;
    int           _adcDry    = 2800;
    int           _adcWet    = 800;
    bool          _calibDirty = false;
    void loadCalibPrefs();
    void saveCalibPrefs();
};

extern SoilSensor soil;
