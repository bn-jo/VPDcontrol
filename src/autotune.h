#pragma once
#include <Arduino.h>
#include "relays.h"

// ─── Phases of a single Auto-Tune run ────────────────────────────────────────
enum ATPhase : uint8_t {
    AT_IDLE     = 0,   // doing nothing
    AT_BASELINE,       // relay forced OFF — measuring ambient
    AT_ON,             // relay forced ON  — measuring effect
    AT_COOLDOWN,       // relay forced OFF — environment recovering
    AT_DONE,           // all relays tested, buffers applied
    AT_ABORTED         // cancelled or safety limit hit
};

// ─── Per-relay result ─────────────────────────────────────────────────────────
struct ATResult {
    uint8_t  relayId;
    float    baseVal;     // average of target metric while relay was OFF
    float    onVal;       // average of target metric while relay was ON
    float    delta;       // onVal - baseVal
    float    bufApplied;  // autoBuffer value written to NVS
    bool     valid;
};

// ─── Snapshot visible to webserver (Core 1) ───────────────────────────────────
struct ATStatus {
    ATPhase     phase;
    int8_t      relayId;       // -1 when idle / done / aborted
    uint8_t     stepDone;      // relays fully tested so far
    uint8_t     stepTotal;     // always NUM_TEST_RELAYS
    uint32_t    phaseRemMs;    // ms left in current phase
    uint32_t    phaseTotMs;    // total ms for current phase
    const char* relayName;     // name of relay currently under test
    ATResult    results[5];
    uint8_t     resultCount;
    bool        abortSafety;   // true = stopped due to out-of-range environment
};

// ─── Auto-Tuner ───────────────────────────────────────────────────────────────
// Runs a step-response test on each controllable relay:
//   BASELINE (relay OFF) → ON (relay ON) → COOLDOWN (relay OFF) → next relay
// Measures how much the relay moves its target variable (VPD / humidity / temp)
// and sets autoBuffer proportional to the observed effect.
//
// All heavy lifting happens in tick() on Core 0 (controlTask).
// requestStart() / requestCancel() are called from Core 1 (WS handler) via
// single-byte volatile flags — no mutex needed.
class AutoTuner {
public:
    static const uint8_t TEST_IDS[];       // all tunable relays (TOP_FAN … HEAT_MAT)
    static const uint8_t NUM_TEST_RELAYS;  // = 5 (max)

    void begin();

    // Core 0 — call whenever a fresh sensor reading is available
    void feed(float t, float h, float vpd);

    // Core 0 — call every 100 ms (same cadence as relays.update())
    void tick();

    // Core 1 — from WebSocket handler; single-byte flags, safe across cores
    // relayMask: bitmask of relay IDs to test (bit N = relay N); 0 = test all
    void requestStart(uint8_t relayMask = 0xFF);
    void requestCancel();
    void requestReset();   // Cancel any active run and restore all relay buffers to factory defaults

    const ATStatus& status() const { return _status; }

private:
    // Cross-core flags (written by Core 1, read by Core 0)
    volatile bool    _reqStart  = false;
    volatile bool    _reqCancel = false;
    volatile bool    _reqReset  = false;
    volatile uint8_t _reqMask   = 0xFF;  // set before _reqStart

    // State — only accessed from Core 0
    ATPhase       _phase        = AT_IDLE;
    int           _step         = 0;        // index into _activeIds[]
    unsigned long _phaseStartMs = 0;

    uint8_t _activeIds  [5];   // relay IDs selected for this run
    uint8_t _activeCount = 0;  // how many relays in this run

    float _curT = 0.0f, _curH = 0.0f, _curV = 0.0f;

    double _sum  = 0.0;
    int    _n    = 0;
    float  _baseVal = 0.0f;

    RelayMode _savedMode  [5];
    bool      _savedManual[5];

    ATStatus _status = {};

    // Helpers
    float metricNow()         const;
    float maxBuf(uint8_t rid) const;
    float minBuf(uint8_t rid) const;
    bool  safetyOk()          const;

    void enterBaseline();
    void enterOn();
    void enterCooldown();
    void finishStep();
    void advance();
    void doAbort(bool safety);
    void restoreAll();
};

extern AutoTuner autoTuner;
