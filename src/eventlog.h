#pragma once
#include <Arduino.h>

// ─── Persistent event log (LittleFS) ─────────────────────────────────────────
// Stores important state-change events across reboots.
// CSV: epoch,TYPE,detail\n — kept in /events.csv.
// Only call eventlog() from Core 1 (loop / async_tcp); LittleFS is mutex-protected
// but the entry count is not, so writes must be single-threaded.

#define EVENTLOG_FILE  "/events.csv"
#define EVENTLOG_MAX   200   // entries before trim
#define EVENTLOG_TRIM  100   // target after trim

void eventlogBegin();                              // call after LittleFS is mounted
void eventlog(const char* type, const char* detail); // append one entry
int  eventlogGetJson(char* buf, size_t bufSize);   // last 60 events → JSON array
