#include "eventlog.h"
#include <LittleFS.h>
#include <time.h>
#include <stdio.h>
#include <string.h>
#include "syslog.h"

static int _evtCount = 0;

// ─── Count existing entries ───────────────────────────────────────────────────
void eventlogBegin() {
    File f = LittleFS.open(EVENTLOG_FILE, "r");
    if (!f) { _evtCount = 0; return; }
    _evtCount = 0;
    while (f.available()) { if (f.read() == '\n') _evtCount++; }
    f.close();
    rlog("[EVT] %d events in log", _evtCount);
}

// ─── Trim oldest entries ──────────────────────────────────────────────────────
static void _trim() {
    int skip = _evtCount - EVENTLOG_TRIM;
    if (skip <= 0) return;
    File src = LittleFS.open(EVENTLOG_FILE, "r");
    File dst = LittleFS.open("/evts_tmp.csv", "w");
    if (!src || !dst) { if (src) src.close(); if (dst) dst.close(); return; }
    int skipped = 0, byteCnt = 0;
    while (src.available()) {
        char c = (char)src.read();
        if (skipped < skip) { if (c == '\n') skipped++; }
        else dst.write((uint8_t)c);
        if (++byteCnt >= 4096) { byteCnt = 0; vTaskDelay(1); }
    }
    src.close(); dst.close();
    LittleFS.remove(EVENTLOG_FILE);
    LittleFS.rename("/evts_tmp.csv", EVENTLOG_FILE);
    _evtCount = EVENTLOG_TRIM;
}

// ─── Append one event ─────────────────────────────────────────────────────────
void eventlog(const char* type, const char* detail) {
    if (_evtCount >= EVENTLOG_MAX) _trim();
    time_t now = time(nullptr);
    File f = LittleFS.open(EVENTLOG_FILE, "a");
    if (!f) return;
    char row[140];
    // Sanitise detail: replace commas and newlines with space so CSV stays intact
    char safe[96];
    int sp = 0;
    for (int i = 0; detail[i] && sp < (int)sizeof(safe) - 1; i++) {
        char c = detail[i];
        safe[sp++] = (c == ',' || c == '\n' || c == '\r') ? ' ' : c;
    }
    safe[sp] = '\0';
    int n = snprintf(row, sizeof(row), "%ld,%s,%s\n", (long)now, type, safe);
    f.write((const uint8_t*)row, n);
    f.close();
    _evtCount++;
    rlog("[EVT] %s: %s", type, safe);
}

// ─── Clear the persistent event log ───────────────────────────────────────────
// Removes /events.csv and resets the counter. Core 1 only (loop), same as the
// writers — this guarantees it never preempts an in-progress eventlog() append.
void eventlogClear() {
    LittleFS.remove(EVENTLOG_FILE);
    _evtCount = 0;
}

// ─── Serialize last 60 entries → JSON — reads from file tail, no static ring ──
int eventlogGetJson(char* buf, size_t bufSize) {
    File f = LittleFS.open(EVENTLOG_FILE, "r");
    if (!f) { buf[0]='['; buf[1]=']'; buf[2]='\0'; return 2; }

    // Seek to approximately the last 60 entries from the end of the file
    size_t fileSize = f.size();
    const size_t bytesNeeded = 60 * 120;
    if (bytesNeeded < fileSize) {
        f.seek(fileSize - bytesNeeded);
        while (f.available() && f.read() != '\n') {}  // align to entry boundary
    }

    size_t w = 0;
    bool first = true;
    buf[w++] = '[';

    char line[140];
    int lp = 0;
    auto parseLine = [&]() {
        if (lp <= 4) { lp = 0; return; }
        line[lp] = '\0'; lp = 0;
        long ts = 0; char type[24] = {}, detail[100] = {};
        if (sscanf(line, "%ld,%23[^,],%99[^\n]", &ts, type, detail) < 2) return;
        char esc[128]; int ep = 0;
        for (int i = 0; detail[i] && ep < (int)sizeof(esc) - 3; i++) {
            if (detail[i] == '"' || detail[i] == '\\') esc[ep++] = '\\';
            esc[ep++] = detail[i];
        }
        esc[ep] = '\0';
        int n = snprintf(buf + w, bufSize - w,
                         "%s{\"ts\":%ld,\"type\":\"%s\",\"detail\":\"%s\"}",
                         first ? "" : ",", ts, type, esc);
        if (w + (size_t)n < bufSize - 4) { w += n; first = false; }
    };
    while (f.available()) {
        char c = (char)f.read();
        if (c == '\n') parseLine();
        else if (lp < (int)sizeof(line) - 2) line[lp++] = c;
    }
    parseLine();  // flush last line if no trailing newline
    f.close();

    buf[w++] = ']';
    buf[w]   = '\0';
    return (int)w;
}
