#include "sensors.h"
#include "config.h"
#include <DHT.h>
#include <math.h>
#include <time.h>

SensorManager sensors;

static DHT dht(DHT_PIN, DHT_TYPE);

SensorManager::SensorManager()
    : _lastValidMs(0)
{
    _data      = {0, 0, 0, 0.0f, false, 0};
    _lastValid = _data;
}

void SensorManager::begin() {
    dht.begin();
}

// Magnus formula: saturation vapour pressure (kPa)
static float svp(float tempC) {
    return 0.6108f * expf(17.27f * tempC / (tempC + 237.3f));
}

float SensorManager::calcVPD(float tempC, float humidityPct, float leafOffset) {
    float svpAir  = svp(tempC);
    float svpLeaf = svp(tempC + leafOffset);
    float avp     = svpAir * (humidityPct / 100.0f);
    return fmaxf(0.0f, svpLeaf - avp);
}

bool SensorManager::read() {
    float rawH = dht.readHumidity();
    float rawT = dht.readTemperature();

    if (isnan(rawH) || isnan(rawT)) {
        Serial.printf("[SENSOR] Read failed — T=%.1f H=%.1f (check wiring on GPIO %d)\n",
                      rawT, rawH, DHT_PIN);
        // Use stale smoothed data for up to SENSOR_STALE_MS
        if (millis() - _lastValidMs < SENSOR_STALE_MS) {
            _data       = _lastValid;
            _data.valid = false;
        } else {
            _data.valid = false;
        }
        return false;
    }

    // Apply rolling average — smooths out sensor noise over ~120 s
    float rawVpd      = calcVPD(rawT, rawH);
    _data.temperature = _avgTemp.add(rawT);
    _data.humidity    = _avgHum.add(rawH);
    _data.vpd         = _avgVpd.add(rawVpd);
    // Trend: slope over last 6 raw VPD samples, converted to kPa/min
    _data.vpdTrend    = _slopeVpd.add(rawVpd) * (60000.0f / SENSOR_INTERVAL_MS);
    _data.valid       = true;
    _data.timestamp   = time(nullptr);

    _lastValid   = _data;
    _lastValidMs = millis();
    return true;
}
