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
    : _mode(GROW_VEG),
      _humidifierOn(false), _topFanOn(false), _bottomFanOn(false),
      _dehumidifierOn(false), _heatMatOn(false),
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

    // ── Slow Dry ─────────────────────────────────────────────────────────────
    // 15-18°C / 55-62% RH / VPD 0.50-0.80 kPa — 10-14 day gentle drying.
    // Low VPD keeps moisture evaporating slowly so trichomes and terpenes stay
    // intact. Chlorophyll breaks down smoothly → smooth smoke.
    // Lights OFF (0 h on / 24 h off) — darkness protects cannabinoids.
    //                             tMin  tMax  hMin  hMax  vMin  vMax  vTgt
    _profiles[GROW_DRY_SLOW] = {
        "Slow Dry",
        { 15.0f, 18.0f, 55.0f, 62.0f, 0.50f, 0.80f, 0.65f },  // unused (always dark)
        { 15.0f, 18.0f, 55.0f, 62.0f, 0.50f, 0.80f, 0.65f },  // active profile
        0, 24   // 0 h on → always OFF
    };

    // ── Fast Dry ─────────────────────────────────────────────────────────────
    // 20-22°C / 45-55% RH / VPD 0.90-1.20 kPa — 5-8 day accelerated drying.
    // Higher temp and VPD pull moisture out faster. Stay within bounds to avoid
    // case-hardening (VPD > 1.2) or terpene loss (T > 24°C).
    // Lights OFF — same reason as slow dry.
    _profiles[GROW_DRY_FAST] = {
        "Fast Dry",
        { 20.0f, 22.0f, 45.0f, 55.0f, 0.90f, 1.20f, 1.05f },  // unused (always dark)
        { 20.0f, 22.0f, 45.0f, 55.0f, 0.90f, 1.20f, 1.05f },  // active profile
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
    // Record when this stage started (requires NTP; stored as 0 if not yet synced)
    time_t now = time(nullptr);
    _stageStartEpoch = (now > 1000000000L) ? (int64_t)now : 0;
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
    // Ignore zeroed-out boot state (no reading yet)
    if (sd.temperature == 0.0f && sd.humidity == 0.0f) return;

    _sched.tick();
    computeOutputs(sd);
}

// ─── Core control logic ───────────────────────────────────────────────────────
//
// SYSTEM OVERVIEW:
//   • Grow light = primary heat source inside the tent
//   • Top fan    = main exhaust; creates negative pressure (vents to outside)
//   • Bottom fan = extracts hot air from tent INTO THE ROOM (energy recycling)
//                  NOT an intake, NOT vented to outside
//   • Humidifier = adds moisture
//   • Lights     = driven by photo-period schedule
//
// BOTTOM FAN DESIGN INTENT:
//   When lights are ON and tent is warm, bottom fan pushes that light-heated
//   air into the room — this is the desired use of grow-light energy.
//   It does NOT cool the tent by bringing in cold air; it redistributes heat.
//
void ClimateController::computeOutputs(const SensorData& sd) {
    const bool lightsOn = _sched.isOn();

    // Active target profile depends on light state
    const DayNightRange& p = lightsOn
                           ? _profiles[_mode].day
                           : _profiles[_mode].night;

    const float t   = sd.temperature;
    const float h   = sd.humidity;
    const float vpd = sd.vpd;

    // ── Predictive VPD: project trend VPD_LOOKAHEAD_MIN minutes ahead ─────────
    // Suppresses small sensor noise via VPD_TREND_MIN floor.
    // Temperature and humidity flags still use current measured values.
    float trend = sd.vpdTrend;
    if (fabsf(trend) < VPD_TREND_MIN) trend = 0.0f;
    const float vpdP = vpd + trend * VPD_LOOKAHEAD_MIN;  // projected VPD

    // ── Fan direction flags ───────────────────────────────────────────────────
    const bool topIntake    = relays.get(TOP_FAN).fanIntake;
    const bool bottomIntake = relays.get(BOTTOM_FAN).fanIntake;

    // ── Per-relay buffers (configurable in UI, persisted in NVS) ─────────────
    const float humBuf       = relays.get(HUMIDIFIER).autoBuffer;
    const float fanBuf       = relays.get(TOP_FAN).autoBuffer;
    const float bottomFanBuf = relays.get(BOTTOM_FAN).autoBuffer;
    const float dehBuf       = relays.get(DEHUMIDIFIER).autoBuffer;
    const float heatBuf      = relays.get(HEAT_MAT).autoBuffer;

    // ── VPD thresholds — profile range, or overridden by manual VPD target ───
    float vpdMin = p.vpdMin;
    float vpdMax = p.vpdMax;
    if (_vpdTarget.enabled) {
        vpdMin = _vpdTarget.kpa - _vpdTarget.buffer;
        vpdMax = _vpdTarget.kpa + _vpdTarget.buffer;
    }

    // ── Threshold flags ───────────────────────────────────────────────────────
    const bool tempHigh  = t    > (p.tempMax + TEMP_HYST);
    const bool tempLow   = t    < (p.tempMin - heatBuf);
    const bool humHigh   = h    > (p.humMax  + dehBuf);             // too humid → dehumidifier
    const bool humLow    = h    < (p.humMin  - humBuf);             // too dry   → humidifier
    // VPD flags use projected value — devices activate before the spike arrives
    const bool vpdHigh   = vpdP > (vpdMax + humBuf);                // heading too dry
    const bool vpdLow    = vpdP < (vpdMin - fanBuf);                // heading too humid
    const bool vpdCrisis = vpdP > (vpdMax + 2.0f * humBuf);         // severe dryness

    // ── LIGHTS (relay 4) ─────────────────────────────────────────────────────
    relays.setAutoState(LIGHTS, lightsOn);

    // ── TOP FAN ───────────────────────────────────────────────────────────────
    // Exhaust: removes humid/hot air → ON when VPD too low (too humid) or temp too high
    // Intake:  brings fresh air in   → ON when VPD too high (too dry)  or temp too high
    {
        bool want;
        if (topIntake) {
            // Intake top fan: brings potentially cooler/drier room air in.
            // Helps when tent is too dry (vpdHigh) or too hot.
            if (_topFanOn) {
                want = (vpdP > vpdMax) || tempHigh;  // OFF once VPD back in range
            } else {
                want = vpdHigh || tempHigh;
            }
        } else {
            // Exhaust (default): removes humid/hot air
            if (_topFanOn) {
                want = (vpd < vpdMin) || tempHigh;   // OFF at inner boundary
            } else {
                want = vpdLow || tempHigh;           // ON only at outer boundary
            }
        }
        _topFanOn = want;
        relays.setAutoState(TOP_FAN, want);
    }

    // ── BOTTOM FAN ────────────────────────────────────────────────────────────
    // Exhaust (default — light-heat extractor → into room):
    //   LIGHTS ON : ON for VPD crisis, temp high, or assist drying
    //   LIGHTS OFF: hard stop when cold; otherwise ON for temp/humidity issues
    //
    // Intake (inline duct booster → brings room air in):
    //   Paired with exhaust top fan for balanced air exchange.
    //   Primary role: thermal regulation (bring in cooler air when tent is hot).
    //   Hard stop when cold (same reason: don't pull cold air in).
    //   bottomFanBuf (set by auto-tune) is the temperature hysteresis for this fan.
    {
        bool want = false;
        // Per-relay temp threshold uses bottomFanBuf as hysteresis (auto-tune calibrated)
        const bool tempHighBF = t > (p.tempMax + bottomFanBuf);

        if (bottomIntake) {
            // Intake mode: mirrors top-fan triggers so they work as a pair
            if (lightsOn) {
                want = tempHighBF || vpdCrisis;
            } else {
                if (tempLow) want = false;   // HARD STOP — don't pull cold air in
                else         want = tempHighBF;
            }
        } else {
            // Exhaust mode (default)
            if (lightsOn) {
                if (vpdCrisis)              want = true;
                else if (tempHighBF)        want = true;
                else if (humHigh && _topFanOn) want = true;
            } else {
                if (tempLow)                want = false;  // HARD STOP
                else if (tempHighBF)        want = true;
                else if (humHigh && _topFanOn) want = true;
            }
        }

        _bottomFanOn = want;
        relays.setAutoState(BOTTOM_FAN, want);
    }

    // ── HUMIDIFIER ───────────────────────────────────────────────────────────
    // Dual trigger: VPD too high (too dry) OR humidity directly below minimum.
    // Asymmetric hysteresis:
    //   Turn ON  when (vpdHigh OR humLow)  — outer band
    //   Turn OFF when vpd <= vpdMax AND h >= humMin  — back in range
    // Hard cutoff: never run when humidity is already too high.
    {
        bool want;
        if (_humidifierOn) {
            want = (vpdP > vpdMax || h < p.humMin) && !humHigh;
        } else {
            want = (vpdHigh || humLow) && !humHigh;
        }
        if (humHigh) want = false;
        _humidifierOn = want;
        relays.setAutoState(HUMIDIFIER, want);
    }

    // ── DEHUMIDIFIER (relay 5) ────────────────────────────────────────────────
    // Opposite of humidifier: ON when humidity clearly too high.
    // Asymmetric hysteresis:
    //   Turn ON  when h > humMax + HUMIDITY_HYST  (outer band)
    //   Turn OFF when h <= humMax                 (inner band)
    // Hard interlock: never run at the same time as the humidifier.
    {
        bool want;
        if (_dehumidifierOn) {
            want = (h > p.humMax);                    // OFF at inner boundary
        } else {
            want = humHigh;                           // ON only at outer boundary
        }
        if (_humidifierOn) want = false;              // Interlock
        _dehumidifierOn = want;
        relays.setAutoState(DEHUMIDIFIER, want);
    }

    // ── HEAT MAT (relay 6) ────────────────────────────────────────────────────
    // Root-zone heating: ON when temp is too low.
    // Asymmetric hysteresis:
    //   Turn ON  when t < tempMin - TEMP_HYST  (outer band)
    //   Turn OFF when t >= tempMin             (inner band)
    // Hard cutoff: never run when temp is high.
    {
        bool want;
        if (_heatMatOn) {
            want = (t < p.tempMin);                   // OFF at inner boundary
        } else {
            want = tempLow;                           // ON only at outer boundary
        }
        if (tempHigh) want = false;                   // Hard cutoff
        _heatMatOn = want;
        relays.setAutoState(HEAT_MAT, want);
    }

    // ── WATERING (relay 7) ────────────────────────────────────────────────────
    // No moisture sensor — AUTO keeps relay OFF; user should use TIMER mode.
    relays.setAutoState(WATERING, false);

    // ── EXTRA (relay 8) ───────────────────────────────────────────────────────
    // Spare output — AUTO keeps it OFF; user controls via MANUAL or TIMER.
    relays.setAutoState(EXTRA, false);

    // ── CONFLICT RESOLUTION ───────────────────────────────────────────────────
    // Rule 1: VPD crisis → suppress humidifier, enable dehumidifier if humidity high
    if (vpdCrisis && _humidifierOn) {
        relays.setAutoState(HUMIDIFIER, false);
        _humidifierOn = false;
    }
    // Rule 2: Both fans running for non-thermal reasons + humidifier = wasteful
    if (_topFanOn && _bottomFanOn && _humidifierOn && !tempHigh && !vpdCrisis) {
        relays.setAutoState(HUMIDIFIER, false);
        _humidifierOn = false;
    }
    // Rule 3: Hard interlock — humidifier and dehumidifier never both ON
    if (_humidifierOn && _dehumidifierOn) {
        // Dehumidifier wins (humidity high takes priority for plant safety)
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
    _vpdTarget.enabled   = p.getBool   ("vtEn",    false);
    _vpdTarget.kpa       = p.getFloat  ("vtKpa",   1.0f);
    _vpdTarget.buffer    = p.getFloat  ("vtBuf",   0.1f);
    _stageStartEpoch     = p.getLong64 ("stEpoch", 0LL);
    p.end();
}
