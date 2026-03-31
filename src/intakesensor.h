#pragma once
#include <Arduino.h>

struct IntakeData {
    float temperature = 0.0f;   // °C
    float humidity    = 0.0f;   // %RH
    bool  valid       = false;
};

// Reads a DHT11 wired to INTAKE_SENSOR_PIN.
// Call update() every loop — self-throttles to INTAKE_SENSOR_INTERVAL_MS.
class IntakeSensor {
public:
    void begin();
    void update();
    const IntakeData& data() const { return _d; }

private:
    IntakeData    _d;
    unsigned long _lastMs = 0;
};

extern IntakeSensor intakeSensor;
