#include "syslog.h"
#include <Preferences.h>
#include <time.h>
#include <stdio.h>
#include <string.h>

// ─── Ring buffer ─────────────────────────────────────────────────────────────
static char  _lines[SYSLOG_LINES][SYSLOG_LINE_LEN];
static int   _head  = 0;   // next write position (oldest entry when buffer full)
static int   _count = 0;   // entries written so far (caps at SYSLOG_LINES)

CrashInfo lastCrash = {};

// ─── Init — load previous crash info from NVS ────────────────────────────────
void syslogBegin() {
    Preferences p;
    p.begin("syslog", true);
    lastCrash.valid     = p.getBool("valid", false);
    lastCrash.epoch     = p.getLong("epoch", 0L);
    lastCrash.uptimeSec = p.getUInt("up",    0U);
    String r = p.getString("reason", "");
    strncpy(lastCrash.reason, r.c_str(), sizeof(lastCrash.reason) - 1);
    lastCrash.reason[sizeof(lastCrash.reason) - 1] = '\0';
    p.end();
}

// ─── Save crash stamp to NVS ─────────────────────────────────────────────────
void syslogSaveCrashInfo(const char* reason) {
    Preferences p;
    p.begin("syslog", false);
    p.putBool  ("valid",  true);
    p.putString("reason", reason);
    p.putLong  ("epoch",  (long)time(nullptr));
    p.putUInt  ("up",     (uint32_t)(millis() / 1000UL));
    p.end();
}

// ─── Log a line (Serial + ring buffer) ───────────────────────────────────────
void rlog(const char* fmt, ...) {
    char msg[SYSLOG_LINE_LEN];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    Serial.println(msg);

    // Prepend timestamp if NTP is available, else uptime
    char line[SYSLOG_LINE_LEN];
    time_t now = time(nullptr);
    if (now > 1000000000L) {
        struct tm lt;
        localtime_r(&now, &lt);
        snprintf(line, sizeof(line), "[%02d:%02d:%02d] %.*s",
                 lt.tm_hour, lt.tm_min, lt.tm_sec,
                 (int)(SYSLOG_LINE_LEN - 12), msg);
    } else {
        uint32_t s = millis() / 1000UL;
        snprintf(line, sizeof(line), "[+%5us] %.*s",
                 s, (int)(SYSLOG_LINE_LEN - 10), msg);
    }

    strncpy(_lines[_head], line, SYSLOG_LINE_LEN - 1);
    _lines[_head][SYSLOG_LINE_LEN - 1] = '\0';
    _head = (_head + 1) % SYSLOG_LINES;
    if (_count < SYSLOG_LINES) _count++;
}

// ─── Serialize to JSON ────────────────────────────────────────────────────────
// Returns array: [ { "t": "...", "l": "line" }, ... ] oldest → newest
int syslogGetJson(char* buf, size_t bufSize) {
    if (bufSize < 8) return 0;
    size_t w = 0;

    // Previous crash block
    w += snprintf(buf + w, bufSize - w, "{\"crash\":");
    if (lastCrash.valid) {
        w += snprintf(buf + w, bufSize - w,
                      "{\"reason\":\"%s\",\"epoch\":%ld,\"uptime\":%u}",
                      lastCrash.reason, lastCrash.epoch, lastCrash.uptimeSec);
    } else {
        w += snprintf(buf + w, bufSize - w, "null");
    }

    w += snprintf(buf + w, bufSize - w, ",\"log\":[");

    // Iterate from oldest to newest
    int start = (_count < SYSLOG_LINES) ? 0 : _head;
    bool first = true;
    for (int i = 0; i < _count; i++) {
        int idx = (start + i) % SYSLOG_LINES;
        // Escape double quotes in the line
        char esc[SYSLOG_LINE_LEN * 2];
        int ep = 0;
        for (int c = 0; _lines[idx][c] && ep < (int)sizeof(esc) - 2; c++) {
            if (_lines[idx][c] == '"' || _lines[idx][c] == '\\') esc[ep++] = '\\';
            esc[ep++] = _lines[idx][c];
        }
        esc[ep] = '\0';

        int n = snprintf(buf + w, bufSize - w, "%s\"%s\"", first ? "" : ",", esc);
        if (w + n >= bufSize - 4) break;
        w += n;
        first = false;
    }

    w += snprintf(buf + w, bufSize - w, "]}");
    return (int)w;
}
