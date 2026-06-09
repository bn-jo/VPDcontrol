#pragma once
#include <Arduino.h>
#include <stdarg.h>

// ─── Remote system log ────────────────────────────────────────────────────────
// Circular in-RAM buffer: stores the last SYSLOG_LINES entries.
// Persists as long as the device is running; lost on reboot.
// Also saves crash info (reset reason + uptime) to NVS so the PREVIOUS crash
// is visible after the device reboots and reconnects to WiFi.
//
// Usage:
//   rlog("[TAG] message %d", value);   — like Serial.printf but goes to buffer too
//   syslogGetJson(buf, size)           — serialize buffer to JSON for /api/syslog
//   syslogSaveCrashInfo()              — call before a planned restart to stamp reason

#define SYSLOG_LINES    60
#define SYSLOG_LINE_LEN 128

void syslogBegin();                              // load previous crash info from NVS
void rlog(const char* fmt, ...);                 // log a line (Serial + ring buffer)
void syslogClear();                              // wipe the in-RAM ring buffer
int  syslogGetJson(char* buf, size_t bufSize);   // serialize ring buffer → JSON
void syslogSaveCrashInfo(const char* reason);    // persist crash stamp to NVS
void syslogFixEpoch();                           // call once after NTP sync — fixes epoch in NVS + RAM

// Previous crash info (loaded at boot from NVS)
struct CrashInfo {
    char     reason[32];   // reset reason string
    long     epoch;        // Unix timestamp of that boot
    uint32_t uptimeSec;    // uptime at crash (seconds)
    bool     valid;        // false = no previous crash info saved
};
extern CrashInfo lastCrash;
