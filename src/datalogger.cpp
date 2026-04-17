#include "datalogger.h"
#include "config.h"
#include <LittleFS.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

DataLogger logger;

DataLogger::DataLogger() : _entryCount(0), _irrigCount(0) {}

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
    countIrrigEntries();
    Serial.printf("[LOG] Ready. Env entries: %d  Irrig entries: %d\n",
                  _entryCount, _irrigCount);
}

// ─── Append one row ───────────────────────────────────────────────────────────
void DataLogger::log(const SensorData& sd, float soilPct) {
    if (_entryCount >= LOG_MAX_ENTRIES) trimOldest();

    File f = LittleFS.open(LOG_FILE_PATH, "a");
    if (!f) { Serial.println("[LOG] Failed to open log file"); return; }

    char row[80];
    int  n = snprintf(row, sizeof(row), "%ld,%.1f,%.1f,%.3f,%.1f\n",
                      (long)sd.timestamp, sd.temperature, sd.humidity, sd.vpd,
                      soilPct);
    f.write((const uint8_t*)row, n);
    f.close();
    _entryCount++;
}

// ─── Build JSON response ───────────────────────────────────────────────────────
int DataLogger::getJsonLast(int hours, char* buf, size_t bufSize, long since) {
    if (bufSize < 4) return 0;

    // Cutoff epoch: anything older than the requested window is skipped
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

    // Buffered line read — readBytesUntil reads a whole line at once,
    // far faster than the previous one-byte-at-a-time f.read() loop.
    char line[96];
    while (f.available()) {
        int n = f.readBytesUntil('\n', line, sizeof(line) - 1);
        if (n <= 0) continue;
        line[n] = '\0';

        long  ts;
        float T, H, V, S = -1.0f;
        // Accept both old 4-field rows (no soil) and new 5-field rows
        int fields = sscanf(line, "%ld,%f,%f,%f,%f", &ts, &T, &H, &V, &S);
        if (fields < 4) continue;
        if ((time_t)ts < cutoff) continue;        // outside requested window
        if (since > 0 && ts <= since) continue;   // already seen by client

        if (w > bufSize - 96) break;              // guard against overflow

        char entry[96];
        int  en;
        if (S >= 0.0f) {
            en = snprintf(entry, sizeof(entry),
                          "%s{\"t\":%ld,\"T\":%.1f,\"H\":%.1f,\"V\":%.3f,\"S\":%.1f}",
                          first ? "" : ",", ts, T, H, V, S);
        } else {
            en = snprintf(entry, sizeof(entry),
                          "%s{\"t\":%ld,\"T\":%.1f,\"H\":%.1f,\"V\":%.3f}",
                          first ? "" : ",", ts, T, H, V);
        }
        if (w + en < bufSize - 4) {
            memcpy(buf + w, entry, en);
            w += en;
            first = false;
        }
    }
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

// ─── Irrigation event log ─────────────────────────────────────────────────────
void DataLogger::logIrrigation(time_t ts, float before, float after,
                                uint32_t durSec, uint32_t ml, uint8_t src) {
    if (_irrigCount >= IRRIG_LOG_MAX) trimIrrigation();

    File f = LittleFS.open(IRRIG_LOG_FILE, "a");
    if (!f) { Serial.println("[LOG] Failed to open irrig log"); return; }

    char row[72];
    int n = snprintf(row, sizeof(row), "%ld,%.1f,%.1f,%lu,%lu,%u\n",
                     (long)ts, before, after, (unsigned long)durSec, (unsigned long)ml, (unsigned)src);
    f.write((const uint8_t*)row, n);
    f.close();
    _irrigCount++;
    static const char* srcNames[] = { "auto", "manual", "timer", "sched" };
    Serial.printf("[LOG] Irrigation [%s]: before=%.0f%% after=%.0f%% dur=%lus vol=%lml\n",
                  srcNames[src < 4 ? src : 0], before, after, (unsigned long)durSec, (unsigned long)ml);
}

int DataLogger::getIrrigationJson(char* buf, size_t bufSize) {
    if (bufSize < 4) return 0;

    File f = LittleFS.open(IRRIG_LOG_FILE, "r");
    if (!f) {
        buf[0] = '['; buf[1] = ']'; buf[2] = '\0';
        return 2;
    }

    size_t w = 0;
    bool first = true;
    buf[w++] = '[';

    char line[80];
    int lp = 0;

    auto flush = [&]() {
        if (lp == 0) return;
        line[lp] = '\0'; lp = 0;
        long ts; float bef, aft; unsigned long dur, vol; unsigned src = 0;
        int fields = sscanf(line, "%ld,%f,%f,%lu,%lu,%u", &ts, &bef, &aft, &dur, &vol, &src);
        if (fields < 4) return;
        if (w > bufSize - 96) return;
        char entry[96];
        int n = snprintf(entry, sizeof(entry),
                         "%s{\"t\":%ld,\"b\":%.1f,\"a\":%.1f,\"d\":%lu,\"ml\":%lu,\"src\":%u}",
                         first ? "" : ",", ts, bef, aft, dur, vol, src);
        if (w + n < bufSize - 4) { memcpy(buf + w, entry, n); w += n; first = false; }
    };

    while (f.available()) {
        char c = (char)f.read();
        if (c == '\n') flush();
        else if (lp < (int)sizeof(line) - 2) line[lp++] = c;
    }
    flush();
    f.close();

    buf[w++] = ']';
    buf[w]   = '\0';
    return (int)w;
}

void DataLogger::countIrrigEntries() {
    File f = LittleFS.open(IRRIG_LOG_FILE, "r");
    if (!f) { _irrigCount = 0; return; }
    _irrigCount = 0;
    while (f.available()) { if (f.read() == '\n') _irrigCount++; }
    f.close();
}

void DataLogger::trimIrrigation() {
    int skip = _irrigCount - IRRIG_LOG_TRIM;
    if (skip <= 0) return;
    File src = LittleFS.open(IRRIG_LOG_FILE, "r");
    File dst = LittleFS.open("/irrig_tmp.csv", "w");
    if (!src || !dst) { if (src) src.close(); if (dst) dst.close(); return; }
    int skipped = 0;
    int byteCnt = 0;
    while (src.available()) {
        char c = (char)src.read();
        byteCnt++;
        if (skipped < skip) { if (c == '\n') skipped++; }
        else { dst.write((uint8_t)c); }
        if (byteCnt >= 4096) { byteCnt = 0; vTaskDelay(1); }
    }
    src.close(); dst.close();
    LittleFS.remove(IRRIG_LOG_FILE);
    LittleFS.rename("/irrig_tmp.csv", IRRIG_LOG_FILE);
    _irrigCount = IRRIG_LOG_TRIM;
    Serial.printf("[LOG] Irrig log trimmed to %d\n", _irrigCount);
}

// ─────────────────────────────────────────────────────────────────────────────
void DataLogger::trimOldest() {
    // Keep the newest LOG_TRIM_TARGET entries by copying them to a temp file
    int skip = _entryCount - LOG_TRIM_TARGET;
    if (skip <= 0) return;

    File src = LittleFS.open(LOG_FILE_PATH, "r");
    File dst = LittleFS.open("/logs_tmp.csv", "w");
    if (!src || !dst) { if (src) src.close(); if (dst) dst.close(); return; }

    // Read in line-sized chunks and yield periodically to prevent Task Watchdog
    // from firing. The byte-by-byte copy of ~50 KB could block Core 0 for several
    // seconds, triggering a WDT reset — this was the most likely cause of the
    // nightly crash around 01:30 AM when the 7-day trim happened to land there.
    char line[96];
    int  lp       = 0;
    int  skipped  = 0;
    int  byteCnt  = 0;

    while (src.available()) {
        char c = (char)src.read();
        byteCnt++;

        if (skipped < skip) {
            if (c == '\n') skipped++;
        } else {
            line[lp++] = c;
            if (c == '\n' || lp >= (int)sizeof(line) - 1) {
                dst.write((const uint8_t*)line, lp);
                lp = 0;
            }
        }

        // Yield to RTOS every 4 KB so the IDLE task (and WDT) can run
        if (byteCnt >= 4096) {
            byteCnt = 0;
            vTaskDelay(1);
        }
    }
    if (lp > 0) dst.write((const uint8_t*)line, lp);  // flush remainder

    src.close();
    dst.close();

    LittleFS.remove(LOG_FILE_PATH);
    LittleFS.rename("/logs_tmp.csv", LOG_FILE_PATH);
    _entryCount = LOG_TRIM_TARGET;
    Serial.printf("[LOG] Trimmed to %d entries\n", _entryCount);
}
