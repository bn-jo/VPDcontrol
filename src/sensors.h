#pragma once
#include <Arduino.h>
#include <math.h>

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

// ─── Rolling slope (least-squares linear regression) ─────────────────────────
// Keeps a circular buffer of N samples; returns the slope in units/sample.
// Convert to physical units: multiply by (60000.0f / SENSOR_INTERVAL_MS) → /min.
template<int N>
struct RollingSlope {
    float buf[N];
    int   _idx;
    int   _cnt;

    RollingSlope() : _idx(0), _cnt(0) {
        for (int i = 0; i < N; i++) buf[i] = 0.0f;
    }

    // Add a sample and return the current slope (units per sample interval)
    float add(float v) {
        buf[_idx] = v;
        _idx = (_idx + 1) % N;
        if (_cnt < N) _cnt++;
        return slope();
    }

    // Least-squares slope over all buffered samples (units per sample interval).
    // i=0 is the oldest sample, i=_cnt-1 is the newest.
    float slope() const {
        if (_cnt < 2) return 0.0f;
        float sumX = 0, sumY = 0, sumXY = 0, sumX2 = 0;
        for (int i = 0; i < _cnt; i++) {
            float x = (float)i;
            float y = buf[(_idx - _cnt + i + N) % N];
            sumX  += x;
            sumY  += y;
            sumXY += x * y;
            sumX2 += x * x;
        }
        float denom = (float)_cnt * sumX2 - sumX * sumX;
        if (fabsf(denom) < 1e-6f) return 0.0f;
        return ((float)_cnt * sumXY - sumX * sumY) / denom;
    }
};

// ─── Sensor reading ───────────────────────────────────────────────────────────
struct SensorData {
    float temperature;   // °C  (smoothed — 12-sample rolling average)
    float humidity;      // %RH (smoothed)
    float vpd;           // kPa (smoothed)
    float vpdTrend;      // kPa/min — positive = VPD rising (air drying out)
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

    RollingAvg<12>  _avgTemp;
    RollingAvg<12>  _avgHum;
    RollingAvg<12>  _avgVpd;
    RollingSlope<6> _slopeVpd;  // 6 samples × 10 s = 60 s trend window
};

extern SensorManager sensors;
