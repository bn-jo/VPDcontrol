#include "climate.h"
#include "config.h"
#include <Preferences.h>
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
    save();
}

void LightSchedule::setDayStart(uint8_t hour, uint8_t min) {
    _dayStartHour = hour;
    _dayStartMin  = min;
    save();
}

void LightSchedule::tick() {
    // Seedling: always ON — no schedule needed
    if (_offSec == 0) {
        _isOn = true;
        _alertFlags &= ~ALERT_SCHED_OVERDUE;
        return;
    }
    // Drying: always OFF — lights never on
    if (_onSec == 0) {
        _isOn = false;
        _alertFlags &= ~ALERT_SCHED_OVERDUE;
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
            save();
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
        save();
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
        save();
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
    save();
    Serial.printf("[LIGHT] NTP recovery — %s phase, %lu s remaining\n",
                  _isOn ? "ON" : "OFF", (unsigned long)remainingSec());
}

void LightSchedule::save() {
    Preferences p;
    p.begin("lights", false);
    p.putLong64("epoch", _phaseStartEpoch);
    p.putBool  ("ison",  _isOn);
    p.putUChar ("mode",  (uint8_t)_mode);
    p.putUChar ("dsh",   _dayStartHour);
    p.putUChar ("dsm",   _dayStartMin);
    p.end();
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

    // ── Flowering ────────────────────────────────────────────────────────────
    _profiles[GROW_FLOWER] = {
        "Flowering",
        { 20.0f, 26.0f, 40.0f, 55.0f, 1.00f, 1.60f, 1.30f },  // day  12 h  (hum max 60→55)
        { 16.0f, 22.0f, 40.0f, 55.0f, 0.80f, 1.30f, 1.05f },  // night 12 h (hum 50-70→40-55, vpd 0.70-1.10→0.80-1.30)
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
    _sched.begin(_mode, _profiles);
}

void ClimateController::setMode(GrowMode m) {
    // Option A: keep the clock, update phase lengths only
    _sched.onModeChange(m, _profiles);
    _mode = m;
    // Sync irrigation profile to new stage
    relays.setIrrigMode((uint8_t)m);
    // Record when this stage started (requires NTP; stored as 0 if not yet synced)
    time_t now = time(nullptr);
    _stageStartEpoch = (now > 1000000000L) ? (int64_t)now : 0;
    savePrefs();
}

void ClimateController::setDryingFast(bool fast) {
    _dryingFast = fast;
    // Mutate the drying profile in-place — stageDay counter is NOT reset.
    const DayNightRange slow = { 15.0f, 18.0f, 55.0f, 62.0f, 0.50f, 0.80f, 0.65f };
    const DayNightRange fast_ = { 20.0f, 22.0f, 45.0f, 55.0f, 0.90f, 1.20f, 1.05f };
    _profiles[GROW_DRYING].day   = fast ? fast_ : slow;
    _profiles[GROW_DRYING].night = fast ? fast_ : slow;
    savePrefs();
}

uint32_t ClimateController::stageDay() const {
    if (_stageStartEpoch <= 0) return 0;
    time_t now = time(nullptr);
    if (now < 1000000000L) return 0;
    int64_t elapsed = (int64_t)now - _stageStartEpoch;
    return (elapsed > 0) ? (uint32_t)(elapsed / 86400) + 1 : 1;
}

void ClimateController::update(const SensorData& sd) {
    // Skip if no reading yet (boot state)
    if (sd.temperature == 0.0f && sd.humidity == 0.0f && sd.vpd == 0.0f) return;
    // On stale sensor data: allow fans/heat for safety, block humidifier (don't humidify blindly)
    if (!sd.valid) {
        relays.setAutoState(HUMIDIFIER, false);
        _humidifierOn = false;
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
    const float dehBuf  = relays.get(DEHUMIDIFIER).autoBuffer;
    const float heatBuf = relays.get(HEAT_MAT).autoBuffer;

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
    // Temperature
    const bool tempHigh = t > (p.tempMax + TEMP_HYST);
    const bool tempLow  = t < (p.tempMin - heatBuf);

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

    // ── LIGHTS ───────────────────────────────────────────────────────────────
    relays.setAutoState(LIGHTS, lightsOn);

    // ── TOP FAN ──────────────────────────────────────────────────────────────
    // Priority: temperature > VPD > baseline off
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
            } else if (tempLow) {
                want = false;                       // too cold — keep heat in
            } else if (fanShouldStop && !humHigh) {
                // Air is already dry (VPD above max) and no mold risk:
                // stop exhausting so the humidifier can build moisture.
                // humHigh exception: mold risk overrides VPD (humidity must come down).
                want = false;
            } else if (_topFanOn) {
                // Was ON: keep running until VPD clearly recovers or humidity ok
                want = fanShouldRun || humHigh;
            } else {
                // Was OFF: only restart when clearly too humid (outer band)
                want = vpdWet || humHigh;
            }
        }
        _topFanOn = want;
        relays.setAutoState(TOP_FAN, want);
    }

    // ── BOTTOM FAN ───────────────────────────────────────────────────────────
    // Secondary fan — responds primarily to temperature, not VPD.
    // Exhaust (default): assists when tent is hot or when humidity is dangerously
    //   high AND top fan is already running (cascade assist).
    // Intake: brings cooler room air in; hard stop when cold.
    {
        bool want = false;
        const bool tempHighBF = t > (p.tempMax + botBuf);

        if (bottomIntake) {
            if (tempLow)  want = false;          // hard stop — don't chill tent
            else          want = tempHighBF;      // thermal regulation only
        } else {
            // Exhaust
            if (tempLow)                    want = false;   // hard stop
            else if (tempHighBF)            want = true;    // temp crisis
            else if (humHigh && _topFanOn)  want = true;    // cascade: assist top fan
            // Not triggered by VPD alone — VPD is the top fan's responsibility
        }
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
        _humidifierOn = want;
        relays.setAutoState(HUMIDIFIER, want);
    }

    // ── DEHUMIDIFIER ─────────────────────────────────────────────────────────
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
        _dehumidifierOn = want;
        relays.setAutoState(DEHUMIDIFIER, want);
    }

    // ── HEAT MAT ─────────────────────────────────────────────────────────────
    // Root-zone warming: ON when temp too low.
    // Cold+humid assist: also runs when humidity is very high AND temp has room
    // to rise — warming air reduces RH passively without running the dehumidifier.
    // Skip humColdAssist if dehumidifier already running (avoid fighting each other).
    // Hard interlocks:
    //   • tempHigh → off (never overheat)
    //   • lightsOn → off (lights already provide heat)
    //   • lights turning on within 30 min → off (pre-heat lockout)
    {
        bool want;
        const bool humColdAssist = humHigh && t < p.tempMax && !_dehumidifierOn;
        if (_heatMatOn) {
            want = (t < p.tempMin) || humColdAssist;
        } else {
            want = tempLow || humColdAssist;
        }
        if (tempHigh) want = false;
        if (lightsOn) want = false;
        if (!lightsOn && _sched.remainingSec() < 1800U) want = false;
        _heatMatOn = want;
        relays.setAutoState(HEAT_MAT, want);
    }

    // ── WATERING ─────────────────────────────────────────────────────────────
    // Precision irrigation is handled in RelayManager::update() (soil-driven).
    relays.setAutoState(WATERING, false);

    // ── A/C ───────────────────────────────────────────────────────────────────
    // Active cooling: ON when temperature clearly exceeds the profile maximum.
    // Asymmetric hysteresis: turn ON at tempMax+acBuf, turn OFF when temp ≤ tempMax.
    // Hard interlock: never runs simultaneously with heat mat.
    {
        const float acBuf = relays.get(AC).autoBuffer;
        bool want;
        if (_acOn) {
            want = t > p.tempMax;             // stay ON until temp drops to max
        } else {
            want = t > (p.tempMax + acBuf);   // turn ON only past outer band
        }
        if (_heatMatOn) want = false;
        _acOn = want;
        relays.setAutoState(AC, want);
    }

    // ── FINAL INTERLOCK ───────────────────────────────────────────────────────
    // Humidifier and dehumidifier must never run simultaneously.
    // Dehumidifier wins (high humidity = mold risk, more urgent).
    if (_humidifierOn && _dehumidifierOn) {
        relays.setAutoState(HUMIDIFIER, false);
        _humidifierOn = false;
    }
}

void ClimateController::setDayStart(uint8_t hour, uint8_t min) {
    _sched.setDayStart(hour, min);
}

void ClimateController::setVpdTarget(bool enabled, float kpa, float buffer) {
    _vpdTarget.enabled = enabled;
    _vpdTarget.kpa     = kpa;
    _vpdTarget.buffer  = buffer;
    savePrefs();
}

// ─── Persistence ─────────────────────────────────────────────────────────────
void ClimateController::savePrefs() {
    Preferences p;
    p.begin("climate", false);
    p.putUChar  ("mode",    (uint8_t)_mode);
    p.putBool   ("dryFast", _dryingFast);
    p.putBool   ("vtEn",    _vpdTarget.enabled);
    p.putFloat  ("vtKpa",   _vpdTarget.kpa);
    p.putFloat  ("vtBuf",   _vpdTarget.buffer);
    p.putLong64 ("stEpoch", _stageStartEpoch);
    p.end();
}

void ClimateController::loadPrefs() {
    Preferences p;
    p.begin("climate", true);
    uint8_t m = p.getUChar("mode", (uint8_t)GROW_VEG);
    _mode = (m < NUM_GROW_MODES) ? (GrowMode)m : GROW_VEG;
    _dryingFast        = p.getBool   ("dryFast", false);
    _vpdTarget.enabled = p.getBool   ("vtEn",    false);
    _vpdTarget.kpa     = p.getFloat  ("vtKpa",   1.0f);
    _vpdTarget.buffer  = p.getFloat  ("vtBuf",   0.1f);
    _stageStartEpoch   = p.getLong64 ("stEpoch", 0LL);
    p.end();
    // Re-apply drying speed after loading (profile starts as slow default)
    if (_mode == GROW_DRYING && _dryingFast) setDryingFast(true);
}
