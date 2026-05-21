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
    unsigned long _lastMs      = 0;
    unsigned long _lastValidMs = 0;  // millis() of last successful read
    uint8_t       _failCount   = 0;  // consecutive failures (drives faster retry)
};

extern IntakeSensor intakeSensor;
