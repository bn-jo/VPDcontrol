#pragma once
#include <Arduino.h>

struct RemoteSensorData {
    float temperature = 0.0f;
    float humidity    = 0.0f;
    bool  valid       = false;
};

// Fetches temp/hum from a second sensor node at REMOTE_SENSOR_URL (config.h).
// Call update() from the main loop (Core 1 / WiFi side).
// If unreachable, valid stays false and the main sensor is used alone.
class RemoteSensor {
public:
    void update();
    const RemoteSensorData& data() const { return _d; }

private:
    RemoteSensorData _d;
    unsigned long    _lastMs = 0;
};

extern RemoteSensor remoteSensor;
