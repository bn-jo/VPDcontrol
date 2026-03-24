#include "datalogger.h"
#include "config.h"
#include <LittleFS.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

DataLogger logger;

DataLogger::DataLogger() : _entryCount(0) {}

void DataLogger::begin() {
    if (!LittleFS.begin(true)) {
        Serial.println("[LOG] LittleFS mount failed — reformatting");
        LittleFS.format();
        if (!LittleFS.begin(true)) {
            Serial.println("[LOG] LittleFS still failed — logging disabled");
            return;
        }
    }
    countEntries();
    Serial.printf("[LOG] Ready. Existing entries: %d\n", _entryCount);
}

// ─── Append one row ───────────────────────────────────────────────────────────
void DataLogger::log(const SensorData& sd) {
    if (_entryCount >= LOG_MAX_ENTRIES) trimOldest();

    File f = LittleFS.open(LOG_FILE_PATH, "a");
    if (!f) { Serial.println("[LOG] Failed to open log file"); return; }

    char row[64];
    int  n = snprintf(row, sizeof(row), "%ld,%.1f,%.1f,%.3f\n",
                      (long)sd.timestamp, sd.temperature, sd.humidity, sd.vpd);
    f.write((const uint8_t*)row, n);
    f.close();
    _entryCount++;
}

// ─── Build JSON response ───────────────────────────────────────────────────────
int DataLogger::getJsonLast(int hours, char* buf, size_t bufSize) {
    if (bufSize < 4) return 0;

    // Cutoff epoch: anything older is skipped
    time_t now    = time(nullptr);
    time_t cutoff = (hours > 0 && now > (time_t)(hours * 3600))
                  ? now - (time_t)(hours * 3600)
                  : 0;

    File f = LittleFS.open(LOG_FILE_PATH, "r");
    if (!f) {
        buf[0] = '['; buf[1] = ']'; buf[2] = '\0';
        return 2;
    }

    size_t w     = 0;
    bool   first = true;
    buf[w++] = '[';

    char line[80];
    int  lp = 0;

    auto flush = [&]() {
        if (lp == 0) return;
        line[lp] = '\0';
        lp = 0;

        long  ts;
        float T, H, V;
        if (sscanf(line, "%ld,%f,%f,%f", &ts, &T, &H, &V) != 4) return;
        if ((time_t)ts < cutoff) return;
        if (w > bufSize - 80) return; // Guard against overflow

        char entry[80];
        int  n = snprintf(entry, sizeof(entry),
                          "%s{\"t\":%ld,\"T\":%.1f,\"H\":%.1f,\"V\":%.3f}",
                          first ? "" : ",", ts, T, H, V);
        if (w + n < bufSize - 4) {
            memcpy(buf + w, entry, n);
            w += n;
            first = false;
        }
    };

    while (f.available()) {
        char c = (char)f.read();
        if (c == '\n') {
            flush();
        } else if (lp < (int)sizeof(line) - 2) {
            line[lp++] = c;
        }
    }
    flush(); // Handle file without trailing newline
    f.close();

    buf[w++] = ']';
    buf[w]   = '\0';
    return (int)w;
}

// ─── Private helpers ──────────────────────────────────────────────────────────
void DataLogger::countEntries() {
    File f = LittleFS.open(LOG_FILE_PATH, "r");
    if (!f) { _entryCount = 0; return; }
    _entryCount = 0;
    while (f.available()) {
        if (f.read() == '\n') _entryCount++;
    }
    f.close();
}

void DataLogger::trimOldest() {
    // Keep the newest LOG_TRIM_TARGET entries by copying them to a temp file
    int skip = _entryCount - LOG_TRIM_TARGET;
    if (skip <= 0) return;

    File src = LittleFS.open(LOG_FILE_PATH, "r");
    File dst = LittleFS.open("/logs_tmp.csv", "w");
    if (!src || !dst) { if (src) src.close(); if (dst) dst.close(); return; }

    int skipped = 0;
    while (src.available()) {
        char c = (char)src.read();
        if (skipped < skip) {
            if (c == '\n') skipped++;
        } else {
            dst.write((uint8_t)c);
        }
    }
    src.close();
    dst.close();

    LittleFS.remove(LOG_FILE_PATH);
    LittleFS.rename("/logs_tmp.csv", LOG_FILE_PATH);
    _entryCount = LOG_TRIM_TARGET;
    Serial.printf("[LOG] Trimmed to %d entries\n", _entryCount);
}
