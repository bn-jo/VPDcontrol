#include "crashlog.h"
#include <Preferences.h>
#include <time.h>
#include <stdio.h>

CrashDay crashDays[CRASHLOG_DAYS];
uint8_t  crashSlot = 0;

// 0=none, 1=panic, 2=brownout, 3=watchdog
static uint8_t _pendingType = 0;
static bool    _incremented = false;

static uint32_t _dateInt(const struct tm* lt) {
    return (uint32_t)((1900 + lt->tm_year) * 10000 + (lt->tm_mon + 1) * 100 + lt->tm_mday);
}

// ─── Load from NVS, note pending crash type ──────────────────────────────────
void crashlogBegin(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_PANIC:    _pendingType = 1; break;
        case ESP_RST_BROWNOUT: _pendingType = 2; break;
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
        case ESP_RST_WDT:      _pendingType = 3; break;
        default:               _pendingType = 0; break;
    }

    Preferences p;
    p.begin("crashlog", true);
    crashSlot = p.getUChar("slot", 0);
    if (crashSlot >= CRASHLOG_DAYS) crashSlot = 0;
    for (uint8_t i = 0; i < CRASHLOG_DAYS; i++) {
        char key[5];
        snprintf(key, sizeof(key), "d%u", i);
        crashDays[i].date     = p.getUInt(key, 0);
        snprintf(key, sizeof(key), "p%u", i);
        crashDays[i].panic    = p.getUChar(key, 0);
        snprintf(key, sizeof(key), "b%u", i);
        crashDays[i].brownout = p.getUChar(key, 0);
        snprintf(key, sizeof(key), "w%u", i);
        crashDays[i].watchdog = p.getUChar(key, 0);
    }
    p.end();
}

// ─── Increment counter once NTP date is known ────────────────────────────────
void crashlogOnNtpSync() {
    if (_incremented) return;
    _incremented = true;

    if (_pendingType == 0) return;  // clean boot — nothing to count

    time_t now = time(nullptr);
    if (now <= 1000000000L) return;  // NTP not ready

    struct tm lt;
    localtime_r(&now, &lt);
    uint32_t today = _dateInt(&lt);

    // Advance to a new slot if this is a new calendar day
    if (crashDays[crashSlot].date != today) {
        crashSlot = (crashSlot + 1) % CRASHLOG_DAYS;
        crashDays[crashSlot].date     = today;
        crashDays[crashSlot].panic    = 0;
        crashDays[crashSlot].brownout = 0;
        crashDays[crashSlot].watchdog = 0;
    }

    if (_pendingType == 1 && crashDays[crashSlot].panic    < 255) crashDays[crashSlot].panic++;
    if (_pendingType == 2 && crashDays[crashSlot].brownout < 255) crashDays[crashSlot].brownout++;
    if (_pendingType == 3 && crashDays[crashSlot].watchdog < 255) crashDays[crashSlot].watchdog++;

    // Persist the updated slot
    Preferences p;
    p.begin("crashlog", false);
    p.putUChar("slot", crashSlot);
    char key[5];
    snprintf(key, sizeof(key), "d%u", crashSlot);
    p.putUInt(key, crashDays[crashSlot].date);
    snprintf(key, sizeof(key), "p%u", crashSlot);
    p.putUChar(key, crashDays[crashSlot].panic);
    snprintf(key, sizeof(key), "b%u", crashSlot);
    p.putUChar(key, crashDays[crashSlot].brownout);
    snprintf(key, sizeof(key), "w%u", crashSlot);
    p.putUChar(key, crashDays[crashSlot].watchdog);
    p.end();
}

// ─── Clear all counters (RAM + NVS) ──────────────────────────────────────────
// Core 1 only (called from webBroadcast via the deferred flag) — wipes the NVS
// namespace so flash isn't written from the async_tcp WS callback.
void crashlogClear() {
    for (uint8_t i = 0; i < CRASHLOG_DAYS; i++) {
        crashDays[i].date     = 0;
        crashDays[i].panic    = 0;
        crashDays[i].brownout = 0;
        crashDays[i].watchdog = 0;
    }
    crashSlot = 0;
    Preferences p;
    p.begin("crashlog", false);
    p.clear();
    p.end();
}

// ─── Serialize oldest → newest (skips empty slots) ──────────────────────────
int crashlogGetJson(char* buf, size_t bufSize) {
    size_t w = 0;
    w += snprintf(buf + w, bufSize - w, "[");
    bool first = true;
    // oldest slot = one past current (ring buffer)
    for (int i = 0; i < CRASHLOG_DAYS; i++) {
        int idx = (crashSlot + 1 + i) % CRASHLOG_DAYS;
        if (crashDays[idx].date == 0) continue;
        int n = snprintf(buf + w, bufSize - w,
                         "%s{\"date\":%lu,\"panic\":%u,\"brownout\":%u,\"watchdog\":%u}",
                         first ? "" : ",",
                         (unsigned long)crashDays[idx].date,
                         crashDays[idx].panic,
                         crashDays[idx].brownout,
                         crashDays[idx].watchdog);
        if (w + (size_t)n < bufSize - 4) { w += n; first = false; }
    }
    w += snprintf(buf + w, bufSize - w, "]");
    buf[w] = '\0';
    return (int)w;
}
