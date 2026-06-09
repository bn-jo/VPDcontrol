#pragma once
#include <Arduino.h>
#include <esp_system.h>

// ─── Persistent daily crash counter (7-day ring buffer) ──────────────────────
// Counts PANIC, BROWNOUT, and Watchdog resets per calendar day.
// Stored in NVS; survives reboots and OTA updates.
// The counter for the current boot is incremented after NTP sync so it can be
// attributed to the correct calendar day.

#define CRASHLOG_DAYS 7

struct CrashDay {
    uint32_t date;      // YYYYMMDD, 0 = empty slot
    uint8_t  panic;
    uint8_t  brownout;
    uint8_t  watchdog;
};

void crashlogBegin(esp_reset_reason_t reason);  // call at boot with esp_reset_reason()
void crashlogOnNtpSync();                        // call once after NTP becomes ready
int  crashlogGetJson(char* buf, size_t bufSize); // serialize history → JSON array
void crashlogClear();                            // wipe all counters (NVS + RAM); Core 1 only

extern CrashDay crashDays[CRASHLOG_DAYS]; // exposed for direct read if needed
extern uint8_t  crashSlot;               // current day's slot index
