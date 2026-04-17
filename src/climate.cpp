#include "climate.h"
#include "config.h"
#include "syslog.h"
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

    // ── Early Flowering (Days 1-21) ───────────────────────────────────────────
    // Stretch phase. Higher RH (45-55%) tolerated; VPD 1.0-1.4 kPa (Aroya/Pulse standard).
    // Auto-transitions to Late Flower at Day FLOWER_EARLY_DAYS+1.
    _profiles[GROW_FLOWER] = {
        "Early Flower",
        { 22.0f, 26.0f, 45.0f, 55.0f, 1.00f, 1.40f, 1.20f },  // day  12 h
        { 18.0f, 22.0f, 45.0f, 55.0f, 0.80f, 1.10f, 0.95f },  // night 12 h
        12, 12
    };

    // ── Late Flowering (Day 22+ — auto-set from Early Flower) ────────────────
    // Bud-fattening phase. Lower RH (35-45%) prevents botrytis; VPD 1.3-1.8 kPa
    // to maximise resin and water transport (Aroya/Pulse/AC Infinity guidelines).
    _profiles[GROW_LATE_FLOWER] = {
        "Late Flower",
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

    // Auto-transition: Early Flower → Late Flower after FLOWER_EARLY_DAYS
    if (_mode == GROW_FLOWER && stageDay() > FLOWER_EARLY_DAYS) {
        rlog("[CLIMATE] Day %lu: auto-transition Early Flower → Late Flower", (unsigned long)stageDay());
        setMode(GROW_LATE_FLOWER);
    }

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
            } else if (tempLow) {
                want = false;                       // too cold — keep heat in
            } else if (!lightsOn && fanShouldStop && !humHigh) {
                // Lights OFF + air dry + no mold risk:
                // stop exhausting so the humidifier can build moisture.
                // When lights are ON we never stop the fan for VPD — lights heat
                // the tent and airflow is needed; the humidifier compensates instead.
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
        if (!relays.get(TOP_FAN).installed) want = false;
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
    // Powerful ceramic heater inside the tent — sustained ON risks overshoot.
    // Strategy: PULSED heating at night.
    //   • Trigger: temp too low OR humidity too high (warming air reduces RH)
    //   • Dehumidifier interlock REMOVED — they work together (dehumid removes
    //     moisture, heater warms air = both lower RH from different angles)
    //   • Pulse: HEAT_PULSE_ON_MS on → HEAT_PULSE_REST_MS off → repeat
    //   • Hard off: tempHigh, lightsOn, or pre-lights lockout (<30 min to lights-on)
    {
        bool want = false;

        const bool humColdAssist = humHigh && t < p.tempMax;
        const bool needHeat = (tempLow || humColdAssist) && !lightsOn && !tempHigh
                              && (_sched.remainingSec() >= 1800U);

        if (needHeat) {
            unsigned long now = millis();
            if (_heatPulseOn) {
                // Currently in ON phase — keep until HEAT_PULSE_ON_MS elapsed
                if (now - _heatPulseStartMs >= HEAT_PULSE_ON_MS) {
                    _heatPulseOn    = false;
                    _heatPulseOffMs = now;
                    want = false;
                    rlog("[HEAT] Pulse OFF — resting %lu s", HEAT_PULSE_REST_MS / 1000UL);
                } else {
                    want = true;
                }
            } else {
                // In rest (OFF) phase — fire when rest expires or on first trigger
                if (_heatPulseOffMs == 0 || (now - _heatPulseOffMs >= HEAT_PULSE_REST_MS)) {
                    _heatPulseOn      = true;
                    _heatPulseStartMs = now;
                    want = true;
                    rlog("[HEAT] Pulse ON — t=%.1f°C h=%.0f%%", t, h);
                } else {
                    want = false;
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

        // Absolute safety cutoffs (belt-and-suspenders)
        if (tempHigh) want = false;
        if (lightsOn) want = false;
        if (!relays.get(HEAT_MAT).installed) want = false;

        // A/C interlock: never heat while A/C is running or still settling.
        // A/C overshoot pushes temp below tempMin, which would otherwise trigger
        // the heater — blocking it here avoids heating against a running A/C cycle.
        {
            bool acCooldown = _acLastOffMs > 0 &&
                              (millis() - _acLastOffMs) < (unsigned long)_acHumDelaySec * 1000UL;
            if (_acOn || acCooldown) want = false;
        }

        _heatMatOn = want;
        relays.setAutoState(HEAT_MAT, want);
    }

    // ── WATERING ─────────────────────────────────────────────────────────────
    // Precision irrigation is handled in RelayManager::update() (soil-driven).
    relays.setAutoState(WATERING, false);

    // ── A/C (relay 5 / DEHUMIDIFIER index) ───────────────────────────────────
    // Physical relay cuts/restores A/C mains power.
    // Wide hysteresis using full profile range — A/C takes time to start cooling:
    //   Turn ON  when temp ≥ acHigh (default = profile tempMax)
    //   Turn OFF when temp ≤ acLow  (default = profile tempMin)
    // Both thresholds can be overridden via UI (0 = use profile value).
    // Hard interlock: never runs simultaneously with heat mat.
    {
        const float acLow  = (_acTempLow  > 0.0f) ? _acTempLow  : p.tempMin;
        const float acHigh = (_acTempHigh > 0.0f) ? _acTempHigh : p.tempMax;
        bool want;
        if (_acOn) {
            want = t > acLow;   // running: keep going until floor is reached
        } else {
            want = t >= acHigh; // idle: kick in as soon as ceiling is hit
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

    // A/C ↔ humidifier interlock: A/C naturally dehumidifies — running the
    // humidifier against it causes large humidity swings and wastes energy.
    // Block humidifier while A/C is ON and for _acHumDelaySec afterwards so
    // the environment can thermally stabilise before we add moisture again.
    {
        bool acCooldown = _acLastOffMs > 0 &&
                          (millis() - _acLastOffMs) < (unsigned long)_acHumDelaySec * 1000UL;
        if ((_acOn || acCooldown) && _humidifierOn) {
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
    savePrefs();
}

void ClimateController::setAcTemps(float low, float high) {
    _acTempLow  = low;
    _acTempHigh = high;
    savePrefs();
}

void ClimateController::setAcHumDelay(uint32_t sec) {
    _acHumDelaySec = sec;
    savePrefs();
}

// ─── Persistence ─────────────────────────────────────────────────────────────
void ClimateController::savePrefs() {
    Preferences p;
    p.begin("climate", false);
    p.putUChar  ("mode",    (uint8_t)_mode);
    p.putBool   ("dryFast", _dryingFast);
    p.putBool   ("vtEn",      _vpdTarget.enabled);
    p.putFloat  ("vtKpa",     _vpdTarget.kpa);
    p.putFloat  ("vtBuf",     _vpdTarget.buffer);
    p.putFloat  ("acLow",     _acTempLow);
    p.putFloat  ("acHigh",    _acTempHigh);
    p.putUInt   ("acDelay",   _acHumDelaySec);
    p.putLong64 ("stEpoch",   _stageStartEpoch);
    p.end();
}

void ClimateController::loadPrefs() {
    Preferences p;
    p.begin("climate", true);
    uint8_t m = p.getUChar("mode", (uint8_t)GROW_VEG);
    _mode = (m < NUM_GROW_MODES) ? (GrowMode)m : GROW_VEG;
    _dryingFast        = p.getBool   ("dryFast", false);
    _vpdTarget.enabled = p.getBool   ("vtEn",      false);
    _vpdTarget.kpa     = p.getFloat  ("vtKpa",     1.0f);
    _vpdTarget.buffer  = p.getFloat  ("vtBuf",     0.1f);
    _acTempLow         = p.getFloat  ("acLow",     0.0f);
    _acTempHigh        = p.getFloat  ("acHigh",    0.0f);
    _acHumDelaySec     = p.getUInt   ("acDelay",   600);
    _stageStartEpoch   = p.getLong64 ("stEpoch",   0LL);
    p.end();
    // Re-apply drying speed after loading (profile starts as slow default)
    if (_mode == GROW_DRYING && _dryingFast) setDryingFast(true);
}
