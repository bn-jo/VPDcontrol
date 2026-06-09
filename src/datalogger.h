#pragma once
#include <Arduino.h>
#include "sensors.h"

class DataLogger {
public:
    DataLogger();
    void begin();

    // Append one row to the CSV log (call every LOG_INTERVAL_MS).
    // soilPct < 0 means no soil reading (stored as -1, omitted from JSON).
    void log(const SensorData& sd, float soilPct = -1.0f);

    // Write a JSON array of the last `hours` hours of data into `buf`.
    // If since > 0, only rows with timestamp > since are included (incremental fetch).
    // step > 1 emits every Nth qualifying row (decimation for large time windows).
    // Returns number of characters written. buf is always null-terminated.
    int  getJsonLast(int hours, char* buf, size_t bufSize, long since = 0, int step = 1);

    // Build a 24-bin average-temperature-by-hour-of-day profile from logs.csv.
    // avg[h] = mean temperature recorded during local hour h (NAN if no samples).
    // Returns the number of hours (0–24) that have at least one sample.
    // Core-1 only: does a full-file LittleFS read and yields/pets the WDT.
    int  getHourlyTempAvg(float avg[24]);

    // ── Irrigation event log (/irrig.csv) ─────────────────────────────────────
    void logIrrigation(time_t ts, float before, float after, uint32_t durSec, uint32_t ml, uint8_t src = 0);
    int  getIrrigationJson(char* buf, size_t bufSize);
    int  irrigCount() const { return _irrigCount; }

private:
    int _entryCount;
    int _irrigCount;
    void countEntries();
    void trimOldest();
    void countIrrigEntries();
    void trimIrrigation();
};

extern DataLogger logger;
