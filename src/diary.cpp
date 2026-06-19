#include "diary.h"
#include <LittleFS.h>
#include <time.h>
#include <stdio.h>
#include <string.h>
#include "syslog.h"

static int _diaryCount = 0;

// ─── Count existing entries ───────────────────────────────────────────────────
void diaryBegin() {
    File f = LittleFS.open(DIARY_FILE, "r");
    if (!f) { _diaryCount = 0; return; }
    _diaryCount = 0;
    while (f.available()) { if (f.read() == '\n') _diaryCount++; }
    f.close();
    rlog("[DIARY] %d entries in log", _diaryCount);
}

// ─── Trim oldest entries ──────────────────────────────────────────────────────
static void _trim() {
    int skip = _diaryCount - DIARY_TRIM;
    if (skip <= 0) return;
    File src = LittleFS.open(DIARY_FILE, "r");
    File dst = LittleFS.open("/diary_tmp.csv", "w");
    if (!src || !dst) { if (src) src.close(); if (dst) dst.close(); return; }
    int skipped = 0, byteCnt = 0;
    while (src.available()) {
        char c = (char)src.read();
        if (skipped < skip) { if (c == '\n') skipped++; }
        else dst.write((uint8_t)c);
        if (++byteCnt >= 4096) { byteCnt = 0; vTaskDelay(1); }
    }
    src.close(); dst.close();
    LittleFS.remove(DIARY_FILE);
    LittleFS.rename("/diary_tmp.csv", DIARY_FILE);
    _diaryCount = DIARY_TRIM;
}

// ─── Sanitise a field: strip commas/newlines so CSV stays intact ─────────────
static void _sanitise(const char* in, char* out, int outSize) {
    int sp = 0;
    for (int i = 0; in && in[i] && sp < outSize - 1; i++) {
        char c = in[i];
        out[sp++] = (c == ',' || c == '\n' || c == '\r') ? ' ' : c;
    }
    out[sp] = '\0';
}

// ─── Append one diary entry ───────────────────────────────────────────────────
void diaryAdd(char kind, uint32_t growDay, const char* stage, const char* note) {
    if (kind != 'S' && kind != 'N') kind = 'N';
    if (_diaryCount >= DIARY_MAX) _trim();
    time_t now = time(nullptr);
    File f = LittleFS.open(DIARY_FILE, "a");
    if (!f) return;
    char safeStage[32], safeNote[128];
    _sanitise(stage, safeStage, sizeof(safeStage));
    _sanitise(note,  safeNote,  sizeof(safeNote));
    char row[200];
    int n = snprintf(row, sizeof(row), "%ld,%c,%lu,%s,%s\n",
                     (long)now, kind, (unsigned long)growDay, safeStage, safeNote);
    f.write((const uint8_t*)row, n);
    f.close();
    _diaryCount++;
    rlog("[DIARY] %c day%lu %s: %s", kind, (unsigned long)growDay, safeStage, safeNote);
}

// ─── Clear the diary ──────────────────────────────────────────────────────────
// Core 1 only (loop), same as the writers — guarantees it never preempts an
// in-progress diaryAdd() append.
void diaryClear() {
    LittleFS.remove(DIARY_FILE);
    _diaryCount = 0;
}
