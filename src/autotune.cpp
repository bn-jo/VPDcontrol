#include "autotune.h"
#include "config.h"
#include <Arduino.h>

AutoTuner autoTuner;

// Relays tested, in order: TOP_FAN, BOTTOM_FAN, HUMIDIFIER, DEHUMIDIFIER, HEAT_MAT
const uint8_t AutoTuner::TEST_IDS[]      = { TOP_FAN, BOTTOM_FAN, HUMIDIFIER, DEHUMIDIFIER, HEAT_MAT };
const uint8_t AutoTuner::NUM_TEST_RELAYS = 5;

// Which environmental variable each relay primarily moves:
//   0 = VPD   (kPa)  — TOP_FAN, HUMIDIFIER
//   1 = hum   (%RH)  — DEHUMIDIFIER
//   2 = temp  (°C)   — BOTTOM_FAN, HEAT_MAT
static const uint8_t METRIC[AutoTuner::NUM_TEST_RELAYS] = { 0, 2, 0, 1, 2 };

// autoBuffer clamp range per metric type
static const float BUF_MAX[] = { 0.30f, 10.0f, 2.0f };  // [VPD, hum, temp]
static const float BUF_MIN[] = { 0.02f,  1.0f, 0.3f };

// ─── Public API ───────────────────────────────────────────────────────────────

void AutoTuner::begin() {
    _status           = {};
    _status.phase     = AT_IDLE;
    _status.stepTotal = NUM_TEST_RELAYS;
    _status.relayName = "";
    _status.relayId   = -1;
}

void AutoTuner::requestStart()  { _reqStart  = true; }
void AutoTuner::requestCancel() { _reqCancel = true; }

void AutoTuner::feed(float t, float h, float vpd) {
    _curT = t;
    _curH = h;
    _curV = vpd;
    // Accumulate samples during measurement phases
    if (_phase == AT_BASELINE || _phase == AT_ON) {
        _sum += (double)metricNow();
        _n++;
    }
}

void AutoTuner::tick() {
    // ── Handle cross-core requests ────────────────────────────────────────────
    if (_reqCancel) {
        _reqCancel = false;
        _reqStart  = false;
        if (_phase != AT_IDLE && _phase != AT_DONE && _phase != AT_ABORTED) {
            doAbort(false);
        }
        return;
    }

    if (_reqStart) {
        _reqStart = false;
        if (_phase == AT_IDLE || _phase == AT_DONE || _phase == AT_ABORTED) {
            _step               = 0;
            _status.resultCount = 0;
            _status.stepDone    = 0;
            _status.abortSafety = false;
            // Save all test-relay modes before touching anything
            for (int i = 0; i < NUM_TEST_RELAYS; i++) {
                RelayIndex idx  = (RelayIndex)TEST_IDS[i];
                _savedMode[i]   = relays.get(idx).mode;
                _savedManual[i] = relays.get(idx).manualOn;
            }
            enterBaseline();
        }
        return;
    }

    if (_phase == AT_IDLE || _phase == AT_DONE || _phase == AT_ABORTED) return;

    // ── Safety abort during ON phase ──────────────────────────────────────────
    if (_phase == AT_ON && !safetyOk()) {
        doAbort(true);
        return;
    }

    // ── Update remaining-time counter for UI ──────────────────────────────────
    uint32_t elapsed       = (uint32_t)(millis() - _phaseStartMs);
    uint32_t tot           = _status.phaseTotMs;
    _status.phaseRemMs     = (elapsed < tot) ? (tot - elapsed) : 0;

    // ── Phase transitions ─────────────────────────────────────────────────────
    switch (_phase) {
        case AT_BASELINE:
            if (elapsed >= AT_BASELINE_MS) {
                _baseVal = (_n > 0) ? (float)(_sum / _n) : metricNow();
                enterOn();
            }
            break;

        case AT_ON:
            if (elapsed >= AT_ON_MS) {
                finishStep();
                enterCooldown();
            }
            break;

        case AT_COOLDOWN:
            if (elapsed >= AT_COOLDOWN_MS) {
                advance();
            }
            break;

        default: break;
    }
}

// ─── Private helpers ──────────────────────────────────────────────────────────

float AutoTuner::metricNow() const {
    if (_step >= (int)NUM_TEST_RELAYS) return 0.0f;
    switch (METRIC[_step]) {
        case 0: return _curV;
        case 1: return _curH;
        case 2: return _curT;
    }
    return 0.0f;
}

float AutoTuner::maxBuf(uint8_t rid) const {
    for (int i = 0; i < (int)NUM_TEST_RELAYS; i++) {
        if (TEST_IDS[i] == rid) return BUF_MAX[METRIC[i]];
    }
    return 0.30f;
}

float AutoTuner::minBuf(uint8_t rid) const {
    for (int i = 0; i < (int)NUM_TEST_RELAYS; i++) {
        if (TEST_IDS[i] == rid) return BUF_MIN[METRIC[i]];
    }
    return 0.02f;
}

bool AutoTuner::safetyOk() const {
    if (_curT > 38.0f || _curT < 8.0f)  return false;
    if (_curH > 93.0f || _curH < 15.0f) return false;
    if (_curV > 2.5f)                    return false;
    return true;
}

// ─── Phase entry ─────────────────────────────────────────────────────────────

void AutoTuner::enterBaseline() {
    _phase        = AT_BASELINE;
    _phaseStartMs = millis();
    _sum          = 0.0;
    _n            = 0;

    RelayIndex idx = (RelayIndex)TEST_IDS[_step];
    relays.setMode  (idx, RELAY_MANUAL);
    relays.setManual(idx, false);

    _status.phase      = AT_BASELINE;
    _status.relayId    = (int8_t)TEST_IDS[_step];
    _status.relayName  = relays.get(idx).name;
    _status.phaseTotMs = AT_BASELINE_MS;
    _status.phaseRemMs = AT_BASELINE_MS;

    Serial.printf("[AT] %d/%d  %s — BASELINE (%.0f s)\n",
                  _step + 1, (int)NUM_TEST_RELAYS,
                  relays.get(idx).name, AT_BASELINE_MS / 1000.0f);
}

void AutoTuner::enterOn() {
    _phase        = AT_ON;
    _phaseStartMs = millis();
    _sum          = 0.0;
    _n            = 0;

    RelayIndex idx = (RelayIndex)TEST_IDS[_step];
    relays.setManual(idx, true);

    _status.phase      = AT_ON;
    _status.phaseTotMs = AT_ON_MS;
    _status.phaseRemMs = AT_ON_MS;

    Serial.printf("[AT] %d/%d  %s — ON (%.0f s)\n",
                  _step + 1, (int)NUM_TEST_RELAYS,
                  relays.get(idx).name, AT_ON_MS / 1000.0f);
}

void AutoTuner::enterCooldown() {
    _phase        = AT_COOLDOWN;
    _phaseStartMs = millis();

    RelayIndex idx = (RelayIndex)TEST_IDS[_step];
    relays.setManual(idx, false);

    _status.phase      = AT_COOLDOWN;
    _status.phaseTotMs = AT_COOLDOWN_MS;
    _status.phaseRemMs = AT_COOLDOWN_MS;

    Serial.printf("[AT] %d/%d  %s — COOLDOWN (%.0f s)\n",
                  _step + 1, (int)NUM_TEST_RELAYS,
                  relays.get(idx).name, AT_COOLDOWN_MS / 1000.0f);
}

// ─── Step completion & advance ────────────────────────────────────────────────

void AutoTuner::finishStep() {
    float onVal = (_n > 0) ? (float)(_sum / _n) : metricNow();
    float delta = onVal - _baseVal;

    uint8_t rid = TEST_IDS[_step];
    // Buffer = 30% of observed effect, clamped to per-metric range
    float buf = fabsf(delta) * 0.3f;
    buf = constrain(buf, minBuf(rid), maxBuf(rid));

    relays.setBuffer((RelayIndex)rid, buf);

    if (_status.resultCount < NUM_TEST_RELAYS) {
        ATResult& r  = _status.results[_status.resultCount++];
        r.relayId    = rid;
        r.baseVal    = _baseVal;
        r.onVal      = onVal;
        r.delta      = delta;
        r.bufApplied = buf;
        r.valid      = true;
    }

    Serial.printf("[AT] %s  base=%.3f  on=%.3f  Δ=%.3f  buf→%.3f\n",
                  relays.get((RelayIndex)rid).name,
                  _baseVal, onVal, delta, buf);
}

void AutoTuner::advance() {
    _step++;
    _status.stepDone = (uint8_t)_step;

    if (_step >= (int)NUM_TEST_RELAYS) {
        restoreAll();
        _phase             = AT_DONE;
        _status.phase      = AT_DONE;
        _status.phaseRemMs = 0;
        _status.relayId    = -1;
        _status.relayName  = "";
        Serial.println("[AT] Complete — all buffers updated");
    } else {
        enterBaseline();
    }
}

void AutoTuner::doAbort(bool safety) {
    restoreAll();
    _phase              = AT_ABORTED;
    _status.phase       = AT_ABORTED;
    _status.phaseRemMs  = 0;
    _status.relayId     = -1;
    _status.relayName   = "";
    _status.abortSafety = safety;
    Serial.printf("[AT] Aborted%s\n", safety ? " — safety limit" : " — user cancel");
}

// ─── Restore saved relay modes ────────────────────────────────────────────────
// Turns every test relay off first, then restores its original mode.
// Called at the end of a successful run and on any abort.
void AutoTuner::restoreAll() {
    for (int i = 0; i < (int)NUM_TEST_RELAYS; i++) {
        RelayIndex idx = (RelayIndex)TEST_IDS[i];
        relays.setManual(idx, false);            // physically off while in MANUAL
        relays.setMode  (idx, _savedMode[i]);
        if (_savedMode[i] == RELAY_MANUAL) {
            relays.setManual(idx, _savedManual[i]);
        }
    }
}
