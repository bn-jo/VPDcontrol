#pragma once
#include <Arduino.h>
#include "sensors.h"

class DataLogger {
public:
    DataLogger();
    void begin();

    // Append one row to the CSV log (call every LOG_INTERVAL_MS)
    void log(const SensorData& sd);

    // Write a JSON array of the last `hours` hours of data into `buf`.
    // Returns number of characters written. buf is always null-terminated.
    int  getJsonLast(int hours, char* buf, size_t bufSize);

private:
    int _entryCount;
    void countEntries();
    void trimOldest();
};

extern DataLogger logger;
