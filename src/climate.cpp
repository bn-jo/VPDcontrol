#include "climate.h"
#include "config.h"
#include "syslog.h"
#include "intakesensor.h"
#include "datalogger.h"
#include <Preferences.h>
#include <LittleFS.h>
#include <math.h>
#include <time.h>

ClimateController climate;

// ═══════════════════════════════════════════════════════════════════════════════
// LightSchedule
// ═══════════════════════════════════════════════════════════════════════════════

void LightSchedule::begin(GrowMode mode, const GrowProfile* profiles) {
    _isOn            = true;
    _phaseStartEpoch = 0;
    _onSec           = (uint32_t)profiles[mode].lightOnHours  * 3600U;
    _offSec          = (uint32_t)profiles[mode].lightOffHours * 3600U;
    _mode            = mode;
    _ntpSynced       = false;
    _alertFlags      = 0;

    load();            // Restore last saved state from NVS
    recoverFromNtp();  // If NTP already available, compute exact position now
}

// Option A: keep the running clock, apply new phase lengths for the new mode.
// If mid-phase elapsed time already exceeds the new phase length, tick() will
// immediately advance to the next phase on the next call — no sudden jumps.
void LightSchedule::onModeChange(GrowMode newMode, const GrowProfile* profiles) {
    _onSec  = (uint32_t)profiles[newMode].lightOnHours  * 3600U;
    _offSec = (uint32_t)profiles[newMode].lightOffHours * 3600U;
    _mode   = newMode;
    markDirty();
}

void LightSchedule::setDayStart(uint8_t hour, uint8_t min) {
    _dayStartHour = hour;
    _dayStartMin  = min;
    markDirty();
}

void LightSchedule::tick() {
    // Seedling: always ON — no schedule needed. With no time-based phases, neither
    // the overdue nor the NTP-missing alert is meaningful — clear both so the
    // "Light Schedule Alert" banner never pops on a 24/7 schedule.
    if (_offSec == 0) {
        _isOn = true;
        _alertFlags &= ~(ALERT_SCHED_OVERDUE | ALERT_NTP_MISSING);
        return;
    }
    // Drying: always OFF — lights never on. Same reasoning: no schedule, no alerts.
    if (_onSec == 0) {
        _isOn = false;
        _alertFlags &= ~(ALERT_SCHED_OVERDUE | ALERT_NTP_MISSING);
        return;
    }

    time_t now = time(nullptr);

    if (now < 1000000000L) {
        _alertFlags |= ALERT_NTP_MISSING;
        return;
    }

    if (!_ntpSynced) {
        recoverFromNtp();
        return;
    }

    _alertFlags &= ~ALERT_NTP_MISSING;

    // ── Fixed daily start time ────────────────────────────────────────────────
    if (_dayStartHour < 24) {
        struct tm t;
        localtime_r(&now, &t);
        int nowMin   = t.tm_hour * 60 + t.tm_min;
        int startMin = _dayStartHour * 60 + _dayStartMin;
        int endMin   = startMin + (int)(_onSec / 60);   // may exceed 1440

        bool shouldBeOn;
        if (endMin <= 1440) {
            shouldBeOn = (nowMin >= startMin && nowMin < endMin);
        } else {
            int wrapped = endMin - 1440;
            shouldBeOn  = (nowMin >= startMin || nowMin < wrapped);
        }

        if (_isOn != shouldBeOn) {
            _isOn            = shouldBeOn;
            _phaseStartEpoch = (int64_t)now;
            markDirty();
            Serial.printf("[LIGHT] Fixed-start → %s\n", _isOn ? "ON" : "OFF");
        }
        _alertFlags &= ~ALERT_SCHED_OVERDUE;
        return;
    }

    // ── Elapsed-time schedule (default) ──────────────────────────────────────
    int64_t elapsed = (int64_t)now - _phaseStartEpoch;
    if (elapsed < 0) elapsed = 0;

    uint32_t phaseDuration = _isOn ? _onSec : _offSec;

    if ((uint64_t)elapsed >= phaseDuration) {
        _isOn = !_isOn;
        _phaseStartEpoch = (int64_t)now;
        markDirty();
        Serial.printf("[LIGHT] Phase → %s\n", _isOn ? "ON" : "OFF");
    }

    if ((uint64_t)elapsed > (uint64_t)phaseDuration + 1800U) {
        _alertFlags |= ALERT_SCHED_OVERDUE;
    } else {
        _alertFlags &= ~ALERT_SCHED_OVERDUE;
    }
}

uint32_t LightSchedule::remainingSec() const {
    if (_offSec == 0) return UINT32_MAX;  // Always ON
    if (_onSec  == 0) return UINT32_MAX;  // Always OFF
    time_t now = time(nullptr);
    if (now < 1000000000L) return 0;

    // Fixed daily start: compute remaining from current time-of-day
    if (_dayStartHour < 24 && _ntpSynced) {
        struct tm t;
        localtime_r(&now, &t);
        int nowSec   = t.tm_hour * 3600 + t.tm_min * 60 + t.tm_sec;
        int startSec = _dayStartHour * 3600 + _dayStartMin * 60;
        int endSec   = startSec + (int)_onSec;           // may exceed 86400

        if (_isOn) {
            int endWrapped = endSec % 86400;
            if (endSec <= 86400) {
                return nowSec < endSec ? (uint32_t)(endSec - nowSec) : 0;
            } else {
                return (nowSec >= startSec)
                    ? (uint32_t)(86400 - nowSec + endWrapped)
                    : (nowSec < endWrapped ? (uint32_t)(endWrapped - nowSec) : 0);
            }
        } else {
            if (nowSec < startSec) return (uint32_t)(startSec - nowSec);
            else                   return (uint32_t)(86400 - nowSec + startSec);
        }
    }

    // Elapsed-time schedule
    if (_phaseStartEpoch == 0) return 0;
    int64_t elapsed = (int64_t)now - _phaseStartEpoch;
    if (elapsed < 0) elapsed = 0;
    uint32_t phaseDuration = _isOn ? _onSec : _offSec;
    if ((uint64_t)elapsed >= phaseDuration) return 0;
    return phaseDuration - (uint32_t)elapsed;
}

// Called once when NTP first becomes available (or on begin() if already synced).
// Fast-forwards the saved phase start epoch through any cycles that passed while
// the device was off or before NTP was available.
void LightSchedule::recoverFromNtp() {
    time_t now = time(nullptr);
    if (now < 1000000000L) {
        _alertFlags |= ALERT_NTP_MISSING;
        return;
    }

    _ntpSynced   = true;
    _alertFlags &= ~ALERT_NTP_MISSING;

    // First boot ever — no saved epoch
    if (_phaseStartEpoch == 0) {
        _isOn            = true;
        _phaseStartEpoch = (int64_t)now;
        markDirty();
        Serial.println("[LIGHT] First boot — starting ON phase");
        return;
    }

    // Seedling: always ON, no recovery needed
    if (_offSec == 0) {
        _isOn = true;
        return;
    }
    // Drying: always OFF, no recovery needed
    if (_onSec == 0) {
        _isOn = false;
        return;
    }

    // Calculate where we are in the cycle, accounting for missed time
    uint32_t cycleSec   = _onSec + _offSec;
    int64_t  elapsed    = (int64_t)now - _phaseStartEpoch;
    if (elapsed <= 0) return;

    // Position within the current cycle
    uint32_t posInCycle = (uint32_t)((uint64_t)elapsed % cycleSec);

    bool shouldBeOn = (posInCycle < _onSec);

    if (shouldBeOn) {
        _isOn            = true;
        _phaseStartEpoch = (int64_t)now - (int64_t)posInCycle;
    } else {
        _isOn            = false;
        _phaseStartEpoch = (int64_t)now - (int64_t)(posInCycle - _onSec);
    }
    markDirty();
    Serial.printf("[LIGHT] NTP recovery — %s phase, %lu s remaining\n",
                  _isOn ? "ON" : "OFF", (unsigned long)remainingSec());
}

// markDirty() — safe to call from any core (sets a volatile flag only).
// writeNvs()  — the actual flash write; MUST be called only from Core 1.
// flushIfDirty() — called from ClimateController::flushPrefsIfDirty() on Core 1.
void LightSchedule::markDirty() { _dirty = true; }

void LightSchedule::writeNvs() {
    Preferences p;
    p.begin("lights", false);
    p.putLong64("epoch", _phaseStartEpoch);
    p.putBool  ("ison",  _isOn);
    p.putUChar ("mode",  (uint8_t)_mode);
    p.putUChar ("dsh",   _dayStartHour);
    p.putUChar ("dsm",   _dayStartMin);
    p.end();
}

void LightSchedule::flushIfDirty() {
    if (!_dirty) return;
    _dirty = false;
    writeNvs();
}

void LightSchedule::load() {
    Preferences p;
    p.begin("lights", true);
    _phaseStartEpoch = p.getLong64("epoch", 0LL);
    _isOn            = p.getBool  ("ison",  true);
    _dayStartHour    = p.getUChar ("dsh",   0xFF);
    _dayStartMin     = p.getUChar ("dsm",   0);
    p.end();
}

// ═══════════════════════════════════════════════════════════════════════════════
// ClimateController
// ═══════════════════════════════════════════════════════════════════════════════

ClimateController::ClimateController()
    : _mode(GROW_VEG), _dryingFast(false),
      _humidifierOn(false), _topFanOn(false), _bottomFanOn(false),
      _dehumidifierOn(false), _heatMatOn(false), _acOn(false),
      _stageStartEpoch(0)
{
    // ── Seedling ─────────────────────────────────────────────────────────────
    //                             tMin  tMax  hMin  hMax  vMin  vMax  vTgt
    _profiles[GROW_SEEDLING] = {
        "Seedling",
        { 22.0f, 28.0f, 65.0f, 80.0f, 0.40f, 0.80f, 0.60f },  // day  (24 h light)
        { 20.0f, 26.0f, 70.0f, 85.0f, 0.30f, 0.60f, 0.45f },  // night (unused: 24/7)
        24, 0   // 24 h on, 0 h off → always ON
    };

    // ── Vegetative ───────────────────────────────────────────────────────────
    _profiles[GROW_VEG] = {
        "Vegetative",
        { 20.0f, 26.0f, 50.0f, 70.0f, 0.80f, 1.20f, 1.00f },  // day  18 h
        { 18.0f, 22.0f, 50.0f, 68.0f, 0.50f, 0.90f, 0.70f },  // night 6 h  (hum max 75→68)
        18, 6
    };

    // ── Early Blooming (Days 1-21) ───────────────────────────────────────────
    // Stretch phase. Higher RH (45-55%) tolerated; VPD 1.0-1.4 kPa (Aroya/Pulse standard).
    // Auto-transitions to Late Bloom at Day BLOOM_EARLY_DAYS+1.
    _profiles[GROW_BLOOM] = {
        "Early Bloom",
        { 22.0f, 26.0f, 45.0f, 55.0f, 1.00f, 1.40f, 1.20f },  // day  12 h
        { 18.0f, 22.0f, 45.0f, 55.0f, 0.80f, 1.10f, 0.95f },  // night 12 h
        12, 12
    };

    // ── Late Blooming (Day 22+ — auto-set from Early Bloom) ────────────────
    // Bloom-fattening phase. Lower RH (35-45%) prevents mould; VPD 1.3-1.8 kPa
    // to maximise yield and water transport (Aroya/Pulse/AC Infinity guidelines).
    _profiles[GROW_LATE_BLOOM] = {
        "Late Bloom",
        { 20.0f, 25.0f, 35.0f, 45.0f, 1.30f, 1.80f, 1.55f },  // day  12 h
        { 16.0f, 21.0f, 35.0f, 45.0f, 1.00f, 1.40f, 1.20f },  // night 12 h
        12, 12
    };

    // ── Drying ───────────────────────────────────────────────────────────────
    // Default = Slow Dry: 15-18°C / 55-62% RH / VPD 0.50-0.80 kPa (~10-14 d)
    // Fast  Dry override: 20-22°C / 45-55% RH / VPD 0.90-1.20 kPa (~5-8 d)
    // setDryingFast() mutates this profile at runtime without resetting stageDay.
    // Lights OFF always (0 h on / 24 h off).
    //                          tMin  tMax  hMin  hMax  vMin  vMax  vTgt
    _profiles[GROW_DRYING] = {
        "Drying",
        { 15.0f, 18.0f, 55.0f, 62.0f, 0.50f, 0.80f, 0.65f },  // unused (always dark)
        { 15.0f, 18.0f, 55.0f, 62.0f, 0.50f, 0.80f, 0.65f },  // active profile (slow default)
        0, 24   // 0 h on → always OFF
    };
}

void ClimateController::begin() {
    loadPrefs();
    loadProfilePrefs();
    if (_dryingFast) setDryingFast(true);  // re-apply after profiles are loaded
    _sched.begin(_mode, _profiles);
    // In drying mode the watering relay must be off regardless of what mode/state
    // was persisted in NVS (e.g. MANUAL ON from a session before the stage change).
    // setMode(RELAY_AUTO) now calls applyPhysical(autoOn=false) immediately.
    if (_mode == GROW_DRYING) relays.setMode(WATERING, RELAY_AUTO);
}

void ClimateController::setMode(GrowMode m) {
    rlog("[CLIMATE] setMode %d→%d", (int)_mode, (int)m);
    // Option A: keep the clock, update phase lengths only
    _sched.onModeChange(m, _profiles);
    _mode = m;
    // Sync irrigation profile to new stage
    relays.setIrrigMode((uint8_t)m);
    // Drying: force watering relay out of any manual/timer/schedule mode so it
    // can't keep running after the stage switch.
    if (m == GROW_DRYING) relays.setMode(WATERING, RELAY_AUTO);
    // Lock the auto-transition as long as the user is NOT in Early Bloom.
    // checkAutoTransition() checks this flag first and returns early if set.
    _userModeLocked = (m != GROW_BLOOM);
    // Record when this stage started (requires NTP; stored as 0 if not yet synced)
    time_t now = time(nullptr);
    _stageStartEpoch = (now > 1000000000L) ? (int64_t)now : 0;
    // Defer both the main prefs and the "userMode" key to Core 1's flush.
    // Previously these were written immediately from the WS callback (async_tcp task),
    // which could preempt loop()'s flushPrefsIfDirty() mid-NVS-sequence.
    _prefsDirty       = true;
    _pendingUserMode  = (uint8_t)m;
    _userModeDirty    = true;
    rlog("[CLIMATE] setMode=%d (deferred NVS write)", (int)m);
}

void ClimateController::setDryingFast(bool fast) {
    _dryingFast = fast;
    // Mutate the drying profile in-place — stageDay counter is NOT reset.
    const DayNightRange slow = { 15.0f, 18.0f, 55.0f, 62.0f, 0.50f, 0.80f, 0.65f };
    const DayNightRange fast_ = { 20.0f, 22.0f, 45.0f, 55.0f, 0.90f, 1.20f, 1.05f };
    _profiles[GROW_DRYING].day   = fast ? fast_ : slow;
    _profiles[GROW_DRYING].night = fast ? fast_ : slow;
    _prefsDirty = true;
}

uint32_t ClimateController::stageDay() const {
    if (_stageStartEpoch <= 0) return 0;
    time_t now = time(nullptr);
    if (now < 1000000000L) return 0;
    int64_t elapsed = (int64_t)now - _stageStartEpoch;
    return (elapsed > 0) ? (uint32_t)(elapsed / 86400) + 1 : 1;
}

void ClimateController::setStageDay(uint32_t day) {
    time_t now = time(nullptr);
    if (now < 1000000000L || day < 1) return;
    _stageStartEpoch = (int64_t)now - (int64_t)(day - 1) * 86400LL;
    _prefsDirty = true;
}

void ClimateController::checkAutoTransition() {
    // Belt-and-suspenders: on the first call per boot, re-read NVS directly.
    // Guards the edge case where _userModeLocked was not set by loadPrefs() —
    // e.g. first boot on a firmware that added modeLocked/userMode, or any
    // scenario where the two keys diverged from in-memory state.
    // Only GROW_BLOOM and GROW_LATE_BLOOM are not "user-locked" — all other
    // stages (Seedling, Veg, Drying) represent an explicit user choice that must
    // survive reboots without the auto-transition overwriting them.
    static bool nvsVerified = false;
    if (!nvsVerified) {
        nvsVerified = true;
        Preferences pv;
        pv.begin("climate", true);
        uint8_t nvMode = pv.getUChar("mode",     0xFF);
        uint8_t umMode = pv.getUChar("userMode", 0xFF);
        pv.end();
        bool nvLock = nvMode < NUM_GROW_MODES
                   && nvMode != (uint8_t)GROW_BLOOM
                   && nvMode != (uint8_t)GROW_LATE_BLOOM;
        bool umLock = umMode < NUM_GROW_MODES
                   && umMode != (uint8_t)GROW_BLOOM
                   && umMode != (uint8_t)GROW_LATE_BLOOM;
        if (nvLock || umLock) {
            _userModeLocked = true;
            rlog("[CLIMATE] auto-transition locked by NVS: mode=%d userMode=%d",
                 (int)nvMode, (int)umMode);
            return;
        }
    }
    if (_userModeLocked) return;
    if (_mode == GROW_BLOOM && stageDay() > BLOOM_EARLY_DAYS) {
        rlog("[CLIMATE] Day %lu: auto-transition Early Bloom → Late Bloom",
             (unsigned long)stageDay());
        int64_t continuousStart = _stageStartEpoch + (int64_t)BLOOM_EARLY_DAYS * 86400LL;
        _sched.onModeChange(GROW_LATE_BLOOM, _profiles);
        _mode = GROW_LATE_BLOOM;
        relays.setIrrigMode((uint8_t)GROW_LATE_BLOOM);
        _stageStartEpoch = continuousStart;
        // Persist via the dirty flag so loop()'s flushPrefsIfDirty() writes the
        // new mode to NVS on the next tick. This prevents the same transition from
        // re-firing on every subsequent reboot when NVS still shows GROW_BLOOM.
        // Full savePrefs() is safe here: we only reach this path when _mode was
        // GROW_BLOOM AND _userModeLocked is false — the user never explicitly set
        // a protected stage, so overwriting "mode" is correct.
        _prefsDirty = true;
    }
}

void ClimateController::update(const SensorData& sd) {
    // Skip if no reading yet (boot state)
    if (sd.temperature == 0.0f && sd.humidity == 0.0f && sd.vpd == 0.0f) return;

    // On stale sensor data: allow fans/heat for safety, block humidifier (don't humidify blindly)
    if (!sd.valid) {
        relays.setAutoState(HUMIDIFIER, false);
        relays.setAutoState(HEAT_MAT, false);   // never heat blindly on stale sensor
        _humidifierOn = false;
        _heatMatOn    = false;
        _heatPulseOn  = false;
        _sched.tick();
        return;
    }

    _sched.tick();
    computeOutputs(sd);
}

// ─── Core control logic ───────────────────────────────────────────────────────
//
// DESIGN PHILOSOPHY (based on commercial grow-room practice):
//
//   Priority order: Temperature safety → VPD → Humidity absolute limits
//
//   TOP FAN (exhaust) is the PRIMARY VPD control element:
//     • Too humid (VPD low)  → fan ON  → removes humid air → VPD rises
//     • Too dry   (VPD high) → fan OFF → humid air stays in → VPD falls
//     • Temperature emergency always overrides VPD logic
//     • Negative-pressure guard (smell control) is handled separately in
//       RelayManager via maxOffSec — forces a brief exhaust burst every 60 s
//       regardless of this logic. No need to run the fan constantly.
//
//   HUMIDIFIER cooperates WITH the fan, not against it:
//     • When VPD is high (dry), fan stops AND humidifier runs — both work together
//     • When VPD is fine, neither runs → no wasteful conflict
//     • Old "Rule 2" (both fans kill humidifier) is removed — it was backwards
//
//   BOTTOM FAN is secondary, responds mainly to temperature.
//
void ClimateController::computeOutputs(const SensorData& sd) {
    const bool lightsOn = _sched.isOn();

    // Active profile: day targets when lights ON, night targets when lights OFF
    const DayNightRange& p = lightsOn
                           ? _profiles[_mode].day
                           : _profiles[_mode].night;

    const float t   = sd.temperature;
    const float h   = sd.humidity;
    const float vpd = sd.vpd;

    // ── Predictive VPD ────────────────────────────────────────────────────────
    float trend = sd.vpdTrend;
    if (fabsf(trend) < VPD_TREND_MIN) trend = 0.0f;
    const float vpdP = vpd + trend * VPD_LOOKAHEAD_MIN;

    // ── Fan direction flags ───────────────────────────────────────────────────
    const bool topIntake    = relays.get(TOP_FAN).fanIntake;
    const bool bottomIntake = relays.get(BOTTOM_FAN).fanIntake;

    // ── Per-relay autoBuffers (configurable in UI, set by auto-tune) ──────────
    const float humBuf  = relays.get(HUMIDIFIER).autoBuffer;
    const float fanBuf  = relays.get(TOP_FAN).autoBuffer;
    const float botBuf  = relays.get(BOTTOM_FAN).autoBuffer;
    const float dehBuf  = relays.get(EXTRA).autoBuffer;
    // Clamp to ≥0: a negative buffer would raise the trigger above tempMin,
    // causing the heater to fire at warm temperatures (e.g. 24.5 °C).
    const float heatBuf = fmaxf(relays.get(HEAT_MAT).autoBuffer, 0.0f);

    // ── VPD operating range ───────────────────────────────────────────────────
    // VPD target override replaces profile vpdMin/vpdMax with a single target ± buffer.
    // Both fan and humidifier use the same range → they coordinate automatically.
    // Changing the target immediately shifts where the fan turns on/off and where
    // the humidifier fires — no other code paths need to know about VPD target.
    float vpdMin = p.vpdMin;
    float vpdMax = p.vpdMax;
    if (_vpdTarget.enabled) {
        vpdMin = _vpdTarget.kpa - _vpdTarget.buffer;
        vpdMax = _vpdTarget.kpa + _vpdTarget.buffer;
    }

    // ── Threshold flags ───────────────────────────────────────────────────────
    // Temperature — heater target overrides profile tempMin when set
    const float heatMin = (_heatTarget > 0.0f) ? _heatTarget : p.tempMin;
    const bool tempHigh = t > (p.tempMax + TEMP_HYST);
    const bool tempLow  = t < (heatMin - heatBuf);

    // Humidity — hard profile limits (mold / plant stress)
    const bool humHigh  = h > (p.humMax + dehBuf);
    const bool humLow   = h < (p.humMin - humBuf);

    // VPD — two thresholds per direction:
    //   fanShouldStop / fanShouldRun: raw vpdMax/vpdMin — fan reacts to profile edge
    //   vpdDry / vpdWet: add relay buffers — humidifier/fan only fire past outer band
    // "Dry" = VPD too high → air needs MORE moisture (humidifier ON, exhaust OFF)
    // "Wet" = VPD too low  → air needs LESS moisture (exhaust ON, humidifier OFF)
    const bool fanShouldStop = vpdP > vpdMax;              // air is dry → stop exhausting
    const bool fanShouldRun  = vpdP < vpdMin;              // air is humid → exhaust to remove
    const bool vpdDry        = vpdP > (vpdMax + humBuf);   // outer band → humidifier ON
    const bool vpdWet        = vpdP < (vpdMin - fanBuf);   // outer band → exhaust fan ON

    // ── A/C thresholds + predictive hot-hours window ──────────────────────────
    // Computed up here (before the humidifier) so the pre-humidify boost can see
    // whether the A/C is about to start. Inside the learned hot window the A/C
    // engages as soon as temp reaches its floor (acLow) and rides through the heat
    // continuously; outside it kicks in only at the ceiling (acHigh) as before.
    const float acLow  = lightsOn ?
        ((_acDayLow    > 0.0f) ? _acDayLow    : p.tempMin) :
        ((_acNightLow  > 0.0f) ? _acNightLow  : p.tempMin);
    const float acHigh = lightsOn ?
        ((_acDayHigh   > 0.0f) ? _acDayHigh   : p.tempMax) :
        ((_acNightHigh > 0.0f) ? _acNightHigh : p.tempMax);

    bool inAcWindow = false;
    if (acWindowValid()) {
        time_t nowW = time(nullptr);
        if (nowW > 1000000000L) {
            struct tm lt;
            localtime_r(&nowW, &lt);
            int nowMin = lt.tm_hour * 60 + lt.tm_min;
            int s = _acHotStartMin, e = _acHotEndMin;
            inAcWindow = (s <= e) ? (nowMin >= s && nowMin <= e)
                                  : (nowMin >= s || nowMin <= e);
        }
    }
    const float acTurnOn      = inAcWindow ? acLow : acHigh;
    const bool  acAboutToStart = !_acOn && (t >= acTurnOn - AC_PRESTART_MARGIN);
    const bool  bloomStage     = (_mode == GROW_BLOOM || _mode == GROW_LATE_BLOOM);

    // ── LIGHTS ───────────────────────────────────────────────────────────────
    relays.setAutoState(LIGHTS, lightsOn);

    // ── TOP FAN ──────────────────────────────────────────────────────────────
    // Priority: temperature > VPD > baseline off
    // Skip entirely if relay is not installed.
    //
    // Exhaust mode (default):
    //   tempHigh         → always on  (temperature emergency overrides everything)
    //   tempLow          → always off (stop venting heat out)
    //   VPD too high     → off unless humidity is dangerously high
    //                      (stop exhausting so humid air stays; humidifier runs)
    //   VPD too low / ok → asymmetric hysteresis (harder to stop than start)
    //
    // Negative-pressure guard (smell): maxOffSec=60s in RelayManager forces
    // brief exhaust bursts even when this logic says OFF. Handled automatically.
    {
        bool want;
        if (topIntake) {
            // Intake: ON when air needs refreshing (too dry or too hot)
            if (_topFanOn) {
                want = (vpdP > vpdMax) || tempHigh;
            } else {
                want = vpdDry || tempHigh;
            }
        } else {
            // Exhaust
            if (tempHigh) {
                want = true;                        // temp emergency — always exhaust
            } else if (tempLow && !humHigh) {
                want = false;                       // too cold — keep heat in; critical humidity overrides
            } else if (!lightsOn && fanShouldStop && !humHigh) {
                // Lights OFF + air dry + no mold risk:
                // stop exhausting so the humidifier can build moisture.
                // When lights are ON we never stop the fan for VPD — lights heat
                // the grow room and airflow is needed; the humidifier compensates instead.
                want = false;
            } else if (_topFanOn) {
                // Was ON: keep running when humid, mold risk, or lights are on (heat exhaust).
                // Lights ON + dry: stay on — humidifier handles moisture, fan handles heat.
                want = fanShouldRun || humHigh || lightsOn;
            } else {
                // Was OFF: only restart when clearly too humid (outer band)
                want = vpdWet || humHigh;
            }
        }
        // A/C TEMPERATURE TRIM: the A/C is kept running as continuously as possible
        // (see the A/C block — cycling it off is costly). Instead of cutting the A/C,
        // the TOP FAN trims the temperature: empirically the fan LOWERS grow room temp
        // when the A/C runs (it distributes the chilled air rather than dumping it out;
        // the old "idle the fan to save A/C cooling" assumption was backwards here).
        //   • temp above the A/C low target → fan ON  (adds cooling, pulls temp down)
        //   • temp at/below the low target  → fan OFF (less cooling, lets temp drift UP)
        // Mold safety (humHigh) still forces the fan on, and the relay's min ON/OFF
        // timers prevent chatter at the boundary.
        // (DEHUMIDIFIER index == the A/C relay, historical naming swap.)
        if (relays.get(DEHUMIDIFIER).physicalOn && !humHigh) want = (t > acLow);
        if (!relays.get(TOP_FAN).installed) want = false;
        _topFanOn = want;
        relays.setAutoState(TOP_FAN, want);
    }

    // ── BOTTOM FAN ───────────────────────────────────────────────────────────
    // Secondary fan — responds primarily to temperature, not VPD.
    // Exhaust (default): assists when grow room is hot or when humidity is dangerously
    //   high AND top fan is already running (cascade assist).
    // Intake: brings cooler room air in; hard stop when cold.
    {
        bool want = false;
        const bool tempHighBF = t > (p.tempMax + botBuf);

        if (bottomIntake) {
            if (tempLow)  want = false;          // hard stop — don't chill grow room
            else          want = tempHighBF;      // thermal regulation only
        } else {
            // Exhaust
            if (tempLow)                    want = false;   // hard stop
            else if (tempHighBF)            want = true;    // temp crisis
            else if (humHigh && _topFanOn)  want = true;    // cascade: assist top fan
            // Not triggered by VPD alone — VPD is the top fan's responsibility
        }
        // A/C interlock: A/C already supplies cold intake air — bottom fan intake is redundant
        if (bottomIntake && relays.get(DEHUMIDIFIER).physicalOn) want = false;
        if (!relays.get(BOTTOM_FAN).installed) want = false;
        _bottomFanOn = want;
        relays.setAutoState(BOTTOM_FAN, want);
    }

    // ── HUMIDIFIER ───────────────────────────────────────────────────────────
    // Dual trigger: VPD too high (too dry) OR humidity directly below minimum.
    //
    // Coordination with top fan: when VPD is too high, the top fan ALSO turns
    // off (see above). Fan off + humidifier on = maximum moisture retention.
    // No conflict suppression needed — the fan logic already does the right thing.
    //
    // Asymmetric hysteresis:
    //   Turn ON  when vpdDry OR humLow          (outer band, past relay buffer)
    //   Turn OFF when vpdP ≤ vpdMax AND h ≥ humMin  (recovered to inner edge)
    // Hard cutoff: never run when humidity is already above max (mold guard).
    {
        bool want;
        if (_humidifierOn) {
            want = (vpdP > vpdMax || h < p.humMin) && !humHigh;
        } else {
            want = (vpdDry || humLow) && !humHigh;
        }
        // Pre-humidify before the A/C kicks in (all stages except Bloom / Late
        // Bloom). The A/C dries the grow room hard the instant it starts, so add
        // moisture first to soften the RH drop. Never override the mold cutoff.
        if (acAboutToStart && !bloomStage && !humHigh) want = true;
        if (!relays.get(HUMIDIFIER).installed) want = false;
        _humidifierOn = want;
        relays.setAutoState(HUMIDIFIER, want);
    }

    // ── DEHUMIDIFIER (relay 8 / EXTRA index) ─────────────────────────────────
    // ON when humidity clearly too high. Asymmetric hysteresis:
    //   Turn ON  when h > humMax + dehBuf  (outer band)
    //   Turn OFF when h ≤ humMax           (inner band)
    // Hard interlock: never run simultaneously with humidifier.
    {
        bool want;
        if (_dehumidifierOn) {
            want = h > p.humMax;
        } else {
            want = humHigh;
        }
        if (_humidifierOn) want = false;
        if (!relays.get(EXTRA).installed) want = false;
        _dehumidifierOn = want;
        relays.setAutoState(EXTRA, want);
    }

    // ── CERAMIC HEATER (Heat Mat relay) ──────────────────────────────────────
    // Powerful ceramic heater inside the grow room — sustained ON risks overshoot.
    // Strategy: PULSED heating at night.
    //   • Trigger: temp below tempMin OR humidity high while below tempMin
    //   • humColdAssist uses tempMin (not tempMax) — heater only helps when
    //     it is actually cold; never runs warm just because humidity is high
    //   • Hard off checked FIRST: t ≥ 24 °C, lights+temp≥min, <30 min to lights-on,
    //     A/C running/settling, outside air warmer, relay not installed
    //   • Pulse: HEAT_PULSE_ON_MS on → HEAT_PULSE_REST_MS off → repeat
    {
        bool want = false;
        unsigned long now = millis();

        // ── Hard cutoffs — evaluated before any pulse logic ───────────────────
        bool acCooldown = _acLastOffMs > 0 &&
                          (now - _acLastOffMs) < (unsigned long)_acHumDelaySec * 1000UL;
        bool hardOff = (t >= 24.0f)                   // user ceiling — never heat at/above 24 °C
                    || tempHigh
                    || (lightsOn && t >= heatMin)     // lights + adequate temp → no heat; cold + lights → allow
                    || (_sched.remainingSec() < 1800U) // <30 min to lights-on
                    || !relays.get(HEAT_MAT).installed
                    || (intakeSensor.data().valid && intakeSensor.data().temperature >= t)
                    || _acOn || acCooldown;           // never heat while A/C is running or cooling down

        if (hardOff) {
            // Reset pulse state cleanly — no log spam while blocked
            if (_heatPulseOn || _heatPulseOffMs != 0) {
                rlog("[HEAT] Pulse reset — hard cutoff (t=%.1f°C)", t);
            }
            _heatPulseOn      = false;
            _heatPulseStartMs = 0;
            _heatPulseOffMs   = 0;
        } else {
            // humColdAssist: only when temp is genuinely low (below tempMin),
            // not just below tempMax — avoids heating a warm grow room to fight humidity
            const bool humColdAssist = humHigh && t < p.tempMin;
            const bool needHeat = tempLow || humColdAssist;

            if (needHeat) {
                if (_heatPulseOn) {
                    // Currently in ON phase — keep until HEAT_PULSE_ON_MS elapsed
                    if (now - _heatPulseStartMs >= (unsigned long)_heatPulseOnSec * 1000UL) {
                        _heatPulseOn    = false;
                        _heatPulseOffMs = now;
                        want = false;
                        rlog("[HEAT] Pulse OFF — resting %lu s", HEAT_PULSE_REST_MS / 1000UL);
                    } else {
                        want = true;
                    }
                } else {
                    // In rest (OFF) phase — fire when rest expires or on first trigger
                    if (_heatPulseOffMs == 0 || (now - _heatPulseOffMs >= (unsigned long)_heatPulseRestSec * 1000UL)) {
                        _heatPulseOn      = true;
                        _heatPulseStartMs = now;
                        want = true;
                        rlog("[HEAT] Pulse ON — t=%.1f°C h=%.0f%%", t, h);
                    }
                }
            } else {
                // Conditions cleared — reset pulse state so next trigger starts fresh
                if (_heatPulseOn || _heatPulseOffMs != 0) {
                    rlog("[HEAT] Pulse reset — conditions cleared");
                }
                _heatPulseOn      = false;
                _heatPulseStartMs = 0;
                _heatPulseOffMs   = 0;
            }
        }

        _heatMatOn = want;
        relays.setAutoState(HEAT_MAT, want);
    }

    // ── WATERING ─────────────────────────────────────────────────────────────
    // Precision irrigation is handled in RelayManager::update() (soil-driven).
    relays.setAutoState(WATERING, false);

    // ── A/C (relay 5 / DEHUMIDIFIER index) ───────────────────────────────────
    // Physical relay cuts/restores A/C mains power. Thresholds (acLow/acHigh) and
    // the predictive hot-hours window were computed above.
    //   Turn ON  when temp ≥ acTurnOn  (acHigh normally; acLow inside the window,
    //            so it pre-cools ahead of the heat and runs continuously through it)
    //   Turn OFF when temp ≤ acLow      (floor — protects against over-cooling)
    // Hard interlock: never runs simultaneously with heat mat.
    {
        bool want;
        if (_acOn) {
            // Keep the A/C running as continuously as possible — cutting power is
            // costly (slow to cool back to full power after an off cycle). The top
            // fan trims temperature above acLow, so only cut the A/C if the room
            // actually over-cools past the safety floor despite the fan being idled.
            want = t > (acLow - AC_CONTINUOUS_FLOOR);
        } else {
            want = t >= acTurnOn;   // idle: kick in at ceiling, or at floor inside hot window
        }
        if (_heatMatOn) want = false;
        if (!relays.get(DEHUMIDIFIER).installed) want = false;
        bool prevAcOn = _acOn;
        _acOn = want;
        if (prevAcOn && !_acOn) _acLastOffMs = millis();  // record when A/C turns off
        relays.setAutoState(DEHUMIDIFIER, _acOn);
    }

    // ── FINAL INTERLOCK ───────────────────────────────────────────────────────
    // Humidifier and dehumidifier must never run simultaneously.
    // Dehumidifier wins (high humidity = mold risk, more urgent).
    if (_humidifierOn && _dehumidifierOn) {
        relays.setAutoState(HUMIDIFIER, false);
        _humidifierOn = false;
    }

    // A/C ↔ humidifier interlock
    // Bloom / Late Bloom: block humidifier while A/C is ON or cooling down —
    // running both causes large RH swings and wastes energy.
    // Seedling, Veg, Drying: high humidity is a priority; humidifier may run
    // alongside A/C — but suppress it when A/C is within AC_PRESHUTDOWN_MARGIN of
    // its shutoff point (humidity spikes as soon as A/C stops dehumidifying) and
    // keep the post-off cooldown guard so the environment settles first.
    // Stage-dependent behaviour:
    //   Seedling / Veg: humidity is the top priority — the A/C interlock NEVER
    //     blocks the humidifier. The main humidifier logic (humLow / vpdDry, with
    //     the humHigh mold cutoff) is the sole authority, so it runs whenever RH
    //     is low, even with the A/C running or cooling down.
    //   Drying: moisture still matters, but respect the post-A/C RH spike — suppress
    //     near the A/C shutoff / cooldown unless RH is below the profile minimum.
    //   Bloom / Late Bloom: want LOW humidity and RH spikes the moment the A/C
    //     stops dehumidifying, so hold the humidifier off near A/C events and only
    //     release it on a true dry-out (hard 35 % floor).
    bool acExempt = (_mode == GROW_SEEDLING || _mode == GROW_VEG);
    if (!acExempt) {
        bool acCooldown = _acLastOffMs > 0 &&
                          (millis() - _acLastOffMs) < (unsigned long)_acHumDelaySec * 1000UL;
        bool  suppress;
        float critLow;
        if (_mode == GROW_DRYING) {
            bool acNearOff = _acOn && (t <= acLow + AC_PRESHUTDOWN_MARGIN);
            suppress = acNearOff || acCooldown;
            critLow  = p.humMin;     // humidify to target
        } else {
            suppress = _acOn || acCooldown;
            critLow  = 35.0f;        // bloom: only a true dry-out releases the block
        }
        if (suppress && _humidifierOn && h >= critLow) {
            relays.setAutoState(HUMIDIFIER, false);
            _humidifierOn = false;
        }
    }
}

void ClimateController::setDayStart(uint8_t hour, uint8_t min) {
    _sched.setDayStart(hour, min);
}

void ClimateController::setVpdTarget(bool enabled, float kpa, float buffer) {
    _vpdTarget.enabled = enabled;
    _vpdTarget.kpa     = kpa;
    _vpdTarget.buffer  = buffer;
    _prefsDirty = true;
}

void ClimateController::setAcTemps(float low, float high, bool night) {
    if (night) {
        _acNightLow  = low;
        _acNightHigh = high;
    } else {
        _acDayLow  = low;
        _acDayHigh = high;
    }
    _prefsDirty = true;
}

void ClimateController::setAcHumDelay(uint32_t sec) {
    _acHumDelaySec = sec;
    _prefsDirty = true;
}

// ─── Predictive A/C hot-hours window ──────────────────────────────────────────
// Learn the daily hot window from logged temperature history and store it as a
// minute-of-day range. Called from Core 1 (loop) — does a full logs.csv read.
// Outside the window the A/C uses normal hysteresis; inside it runs continuously.
void ClimateController::recomputeAcWindow() {
    float avg[24];
    int filled = logger.getHourlyTempAvg(avg);
    if (filled < AC_WINDOW_MIN_HOURS) {
        rlog("[AC-WIN] Not enough history (%d/24 h) — hysteresis only", filled);
        _acHotStartMin = -1;
        _acHotEndMin   = -1;
        return;
    }

    // Daily peak among hours that have data
    int   peakH = -1;
    float peakT = -1000.0f;
    for (int h = 0; h < 24; h++) {
        if (!isnan(avg[h]) && avg[h] > peakT) { peakT = avg[h]; peakH = h; }
    }
    if (peakH < 0) { _acHotStartMin = -1; _acHotEndMin = -1; return; }

    const float thresh = peakT - AC_HOT_BAND;

    // Grow the window outward from the peak hour while still "hot" (wraps midnight).
    int startH = peakH, endH = peakH;
    for (int i = 1; i < 24; i++) {
        int h = (peakH - i + 24) % 24;
        if (!isnan(avg[h]) && avg[h] >= thresh) startH = h; else break;
    }
    for (int i = 1; i < 24; i++) {
        int h = (peakH + i) % 24;
        if (!isnan(avg[h]) && avg[h] >= thresh) endH = h; else break;
    }

    // Hour bins [startH:00 .. endH:59], padded each side by the margin.
    int startMin = startH * 60 - AC_WINDOW_MARGIN_MIN;
    int endMin   = endH * 60 + 60 + AC_WINDOW_MARGIN_MIN;
    startMin = (startMin % 1440 + 1440) % 1440;
    endMin   = (endMin   % 1440 + 1440) % 1440;

    _acHotStartMin = startMin;
    _acHotEndMin   = endMin;
    rlog("[AC-WIN] Hot window %02d:%02d-%02d:%02d (peak %.1f C @ %02d:00, %d/24 h)",
         startMin / 60, startMin % 60, endMin / 60, endMin % 60,
         (double)peakT, peakH, filled);
}

void ClimateController::setHeatPulse(uint32_t onSec, uint32_t restSec, float target) {
    if (onSec   < 5)    onSec   = 5;
    if (onSec   > 3600) onSec   = 3600;
    if (restSec < 30)   restSec = 30;
    if (restSec > 7200) restSec = 7200;
    _heatPulseOnSec   = onSec;
    _heatPulseRestSec = restSec;
    _heatTarget       = (target > 0.0f) ? target : 0.0f;
    _prefsDirty = true;
}

// ─── Persistence ─────────────────────────────────────────────────────────────
void ClimateController::savePrefs() {
    Preferences p;
    p.begin("climate", false);
    p.putUChar  ("mode",    (uint8_t)_mode);
    rlog("[CLIMATE] savePrefs mode=%d", (int)_mode);
    p.putBool   ("dryFast", _dryingFast);
    p.putBool   ("vtEn",      _vpdTarget.enabled);
    p.putFloat  ("vtKpa",     _vpdTarget.kpa);
    p.putFloat  ("vtBuf",     _vpdTarget.buffer);
    p.putFloat  ("acLow",     _acDayLow);
    p.putFloat  ("acHigh",    _acDayHigh);
    p.putFloat  ("acNLow",    _acNightLow);
    p.putFloat  ("acNHigh",   _acNightHigh);
    p.putUInt   ("acDelay",   _acHumDelaySec);
    p.putUInt   ("hpOnSec",  _heatPulseOnSec);
    p.putUInt   ("hpRestSec",_heatPulseRestSec);
    p.putFloat  ("hpTarget", _heatTarget);
    p.putLong64 ("stEpoch",   _stageStartEpoch);
    p.putBool   ("modeLocked", _userModeLocked);
    p.end();
}

void ClimateController::loadPrefs() {
    Preferences p;
    p.begin("climate", true);
    uint8_t m  = p.getUChar("mode",     (uint8_t)GROW_VEG);
    uint8_t um = p.getUChar("userMode", 0xFF);   // 0xFF = key not written yet
    // If the user explicitly set a non-bloom mode, trust that over "mode"
    // which may have been overwritten by the auto-transition on a previous boot.
    if (um < NUM_GROW_MODES && (GrowMode)um != GROW_BLOOM) {
        m = um;
        rlog("[CLIMATE] loadPrefs userMode override=%d", (int)um);
    }
    _mode = (m < NUM_GROW_MODES) ? (GrowMode)m : GROW_VEG;
    rlog("[CLIMATE] loadPrefs raw=%d userMode=%d final=%d", (int)p.getUChar("mode",(uint8_t)GROW_VEG), (int)um, (int)_mode);
    _dryingFast        = p.getBool   ("dryFast", false);
    _vpdTarget.enabled = p.getBool   ("vtEn",      false);
    _vpdTarget.kpa     = p.getFloat  ("vtKpa",     1.0f);
    _vpdTarget.buffer  = p.getFloat  ("vtBuf",     0.1f);
    _acDayLow          = p.getFloat  ("acLow",     0.0f);
    _acDayHigh         = p.getFloat  ("acHigh",    0.0f);
    _acNightLow        = p.getFloat  ("acNLow",    0.0f);
    _acNightHigh       = p.getFloat  ("acNHigh",   0.0f);
    _acHumDelaySec     = p.getUInt   ("acDelay",   600);
    _heatPulseOnSec    = p.getUInt   ("hpOnSec",  45);
    _heatPulseRestSec  = p.getUInt   ("hpRestSec",420);
    _heatTarget        = p.getFloat  ("hpTarget", 0.0f);
    _stageStartEpoch   = p.getLong64 ("stEpoch",   0LL);
    // If modeLocked key was never written (first boot on this firmware), infer it
    // from userMode: if the user previously set a non-bloom stage, treat it as
    // locked so checkAutoTransition() cannot override it on this boot.
    bool inferLocked = (um < NUM_GROW_MODES &&
                        (GrowMode)um != GROW_BLOOM &&
                        (GrowMode)um != GROW_LATE_BLOOM);
    _userModeLocked    = p.getBool   ("modeLocked", inferLocked);
    p.end();
}

// ─── Profile persistence ──────────────────────────────────────────────────────

// Hardcoded defaults — single source of truth for resetProfile()
static const struct { DayNightRange day, night; } PROFILE_DEFAULTS[NUM_GROW_MODES] = {
    // GROW_SEEDLING
    { { 22.0f, 28.0f, 65.0f, 80.0f, 0.40f, 0.80f, 0.60f },
      { 20.0f, 26.0f, 70.0f, 85.0f, 0.30f, 0.60f, 0.45f } },
    // GROW_VEG
    { { 20.0f, 26.0f, 50.0f, 70.0f, 0.80f, 1.20f, 1.00f },
      { 18.0f, 22.0f, 50.0f, 68.0f, 0.50f, 0.90f, 0.70f } },
    // GROW_BLOOM (Early)
    { { 22.0f, 26.0f, 45.0f, 55.0f, 1.00f, 1.40f, 1.20f },
      { 18.0f, 22.0f, 45.0f, 55.0f, 0.80f, 1.10f, 0.95f } },
    // GROW_LATE_BLOOM
    { { 20.0f, 25.0f, 35.0f, 45.0f, 1.30f, 1.80f, 1.55f },
      { 16.0f, 21.0f, 35.0f, 45.0f, 1.00f, 1.40f, 1.20f } },
    // GROW_DRYING (slow — fast mode overrides at runtime)
    { { 15.0f, 18.0f, 55.0f, 62.0f, 0.50f, 0.80f, 0.65f },
      { 15.0f, 18.0f, 55.0f, 62.0f, 0.50f, 0.80f, 0.65f } },
};

struct __attribute__((packed)) ProfileBlob {
    float dtMin, dtMax, dhMin, dhMax, dvMin, dvMax;
    float ntMin, ntMax, nhMin, nhMax, nvMin, nvMax;
};

void ClimateController::setProfile(GrowMode mode, const DayNightRange& day, const DayNightRange& night) {
    _profiles[mode].day   = day;
    _profiles[mode].night = night;
    _profilePrefsDirty = true;  // flushed from Core 1 in loop()
}

void ClimateController::resetProfile(GrowMode mode) {
    _profiles[mode].day   = PROFILE_DEFAULTS[mode].day;
    _profiles[mode].night = PROFILE_DEFAULTS[mode].night;
    _profilePrefsDirty = true;
    // Re-apply fast-dry override if resetting the drying profile while fast mode is active
    if (mode == GROW_DRYING && _dryingFast) {
        const DayNightRange fast_ = { 20.0f, 22.0f, 45.0f, 55.0f, 0.90f, 1.20f, 1.05f };
        _profiles[GROW_DRYING].day   = fast_;
        _profiles[GROW_DRYING].night = fast_;
    }
}

void ClimateController::saveProfilePrefs() {
    ProfileBlob blobs[NUM_GROW_MODES];
    for (int i = 0; i < NUM_GROW_MODES; i++) {
        blobs[i].dtMin = _profiles[i].day.tempMin;    blobs[i].dtMax = _profiles[i].day.tempMax;
        blobs[i].dhMin = _profiles[i].day.humMin;     blobs[i].dhMax = _profiles[i].day.humMax;
        blobs[i].dvMin = _profiles[i].day.vpdMin;     blobs[i].dvMax = _profiles[i].day.vpdMax;
        blobs[i].ntMin = _profiles[i].night.tempMin;  blobs[i].ntMax = _profiles[i].night.tempMax;
        blobs[i].nhMin = _profiles[i].night.humMin;   blobs[i].nhMax = _profiles[i].night.humMax;
        blobs[i].nvMin = _profiles[i].night.vpdMin;   blobs[i].nvMax = _profiles[i].night.vpdMax;
    }
    File f = LittleFS.open("/profiles.bin", "w");
    if (!f) { rlog("[PROFCFG] ERROR: cannot open /profiles.bin for write"); return; }
    size_t written = f.write((const uint8_t*)blobs, sizeof(blobs));
    f.close();
    if (written == sizeof(blobs))
        rlog("[PROFCFG] saved profiles (%u bytes) to LittleFS", (unsigned)written);
    else
        rlog("[PROFCFG] ERROR: wrote %u/%u bytes", (unsigned)written, (unsigned)sizeof(blobs));
}

void ClimateController::flushPrefsIfDirty() {
    // "userMode" key is written separately from savePrefs() so auto-transitions
    // can never overwrite it (savePrefs() writes "mode"; "userMode" is only set
    // when the user explicitly picks a stage).
    if (_userModeDirty) {
        _userModeDirty = false;
        Preferences pu;
        pu.begin("climate", false);
        pu.putUChar("userMode", _pendingUserMode);
        pu.end();
        rlog("[CLIMATE] userMode flushed=%d", (int)_pendingUserMode);
    }
    if (_prefsDirty) {
        _prefsDirty = false;
        savePrefs();
    }
    _sched.flushIfDirty();  // light schedule NVS — must only write from Core 1
}

void ClimateController::flushProfilePrefsIfDirty() {
    if (!_profilePrefsDirty) return;
    _profilePrefsDirty = false;
    saveProfilePrefs();
}

void ClimateController::loadProfilePrefs() {
    File f = LittleFS.open("/profiles.bin", "r");
    if (!f) { rlog("[PROFCFG] No /profiles.bin — using defaults"); return; }
    ProfileBlob blobs[NUM_GROW_MODES];
    size_t readBytes = f.read((uint8_t*)blobs, sizeof(blobs));
    f.close();
    if (readBytes != sizeof(blobs)) {
        rlog("[PROFCFG] profiles.bin size mismatch (%u/%u) — using defaults",
             (unsigned)readBytes, (unsigned)sizeof(blobs));
        return;
    }
    for (int i = 0; i < NUM_GROW_MODES; i++) {
        _profiles[i].day.tempMin   = blobs[i].dtMin;  _profiles[i].day.tempMax   = blobs[i].dtMax;
        _profiles[i].day.humMin    = blobs[i].dhMin;  _profiles[i].day.humMax    = blobs[i].dhMax;
        _profiles[i].day.vpdMin    = blobs[i].dvMin;  _profiles[i].day.vpdMax    = blobs[i].dvMax;
        _profiles[i].night.tempMin = blobs[i].ntMin;  _profiles[i].night.tempMax = blobs[i].ntMax;
        _profiles[i].night.humMin  = blobs[i].nhMin;  _profiles[i].night.humMax  = blobs[i].nhMax;
        _profiles[i].night.vpdMin  = blobs[i].nvMin;  _profiles[i].night.vpdMax  = blobs[i].nvMax;
    }
    rlog("[PROFCFG] loaded all profiles from /profiles.bin");
}
