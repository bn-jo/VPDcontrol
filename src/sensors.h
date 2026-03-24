#pragma once
#include <Arduino.h>

// ─── Rolling average ──────────────────────────────────────────────────────────
// Keeps a circular buffer of N samples; returns the running mean.
template<int N>
struct RollingAvg {
    float buf[N];
    float _sum;
    int   _idx;
    int   _cnt;

    RollingAvg() : _sum(0), _idx(0), _cnt(0) {
        for (int i = 0; i < N; i++) buf[i] = 0.0f;
    }

    // Add a new sample and return the current average
    float add(float v) {
        _sum -= buf[_idx];
        buf[_idx] = v;
        _sum += v;
        _idx = (_idx + 1) % N;
        if (_cnt < N) _cnt++;
        return _sum / _cnt;
    }

    float value() const { return _cnt > 0 ? _sum / _cnt : 0.0f; }
    bool  ready() const { return _cnt >= N; }
    void  reset() { for (int i = 0; i < N; i++) buf[i] = 0.0f; _sum = 0; _idx = 0; _cnt = 0; }
};

// ─── Sensor reading ───────────────────────────────────────────────────────────
struct SensorData {
    float temperature;   // °C  (smoothed — 12-sample rolling average)
    float humidity;      // %RH (smoothed)
    float vpd;           // kPa (smoothed)
    bool  valid;         // true = fresh; false = stale (>60 s since last good read)
    time_t timestamp;    // Unix epoch of last successful raw read
};

class SensorManager {
public:
    SensorManager();
    void begin();

    // Attempt a DHT22 read. Returns true on success.
    // Smoothing is applied before storing; data() always returns averaged values.
    bool read();

    const SensorData& data() const { return _data; }

    static float calcVPD(float tempC, float humidityPct, float leafOffset = -2.0f);

private:
    SensorData    _data;
    SensorData    _lastValid;
    unsigned long _lastValidMs;

    RollingAvg<12> _avgTemp;
    RollingAvg<12> _avgHum;
    RollingAvg<12> _avgVpd;
};

extern SensorManager sensors;
