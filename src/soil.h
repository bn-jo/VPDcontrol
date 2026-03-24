#pragma once
#include <Arduino.h>

// ─── Capacitive soil moisture sensor ─────────────────────────────────────────
// Reads an analog capacitive probe (e.g. v1.2 / v2.0).
// Higher ADC count = drier soil.
// Calibrate SOIL_ADC_DRY and SOIL_ADC_WET in config.h for your specific probe.

struct SoilData {
    float moisture;   // 0.0 – 100.0 %  (0 = dry, 100 = saturated)
    bool  valid;      // false until first reading completes
};

class SoilSensor {
public:
    void begin();
    void update();    // Self-throttles; call every loop tick

    const SoilData& data() const { return _d; }

private:
    SoilData      _d      = {};
    unsigned long _lastMs = 0;
};

extern SoilSensor soil;
