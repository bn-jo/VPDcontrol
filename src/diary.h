#pragma once
#include <Arduino.h>

// ─── Grow diary (LittleFS) ───────────────────────────────────────────────────
// User-facing journal of the grow: auto entries on stage changes plus free-text
// notes the grower adds from the UI. Separate from the technical event log so it
// stays clean (no boots/crashes/wifi noise).
// CSV: epoch,kind,growDay,stage,note\n  — kept in /diary.csv.
//   kind = 'S' (stage change) | 'N' (user note)
// Only call diaryAdd()/diaryClear() from Core 1 (loop / async_tcp); LittleFS is
// mutex-protected but the entry count is not, so writes must be single-threaded.

#define DIARY_FILE  "/diary.csv"
#define DIARY_MAX   400   // entries before trim
#define DIARY_TRIM  300   // target after trim

void diaryBegin();                                                        // call after LittleFS mounted
void diaryAdd(char kind, uint32_t growDay, const char* stage, const char* note);
void diaryClear();                                                        // delete the diary file (Core 1 only)
// The diary is served as raw CSV streamed straight from LittleFS (see /api/diary);
// the browser parses it, so no on-device JSON serialiser is needed.
