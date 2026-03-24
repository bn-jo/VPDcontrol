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

void LightSchedule::tick() {
    // Seedling: always ON — no schedule needed
    if (_offSec == 0) {
        _isOn = true;
        _alertFlags &= ~ALERT_SCHED_OVERDUE;
        return;
    }

    time_t now = time(nullptr);

    if (now < 1000000000L) {
        // NTP not yet synced
        _alertFlags |= ALERT_NTP_MISSING;
        // Hold last known state — do not advance
        return;
    }

    // First tick after NTP becomes available
    if (!_ntpSynced) {
        recoverFromNtp();
        return;
    }

    _alertFlags &= ~ALERT_NTP_MISSING;

    int64_t elapsed = (int64_t)now - _phaseStartEpoch;
    if (elapsed < 0) elapsed = 0;  // Guard against backward clock correction

    uint32_t phaseDuration = _isOn ? _onSec : _offSec;

    if ((uint64_t)elapsed >= phaseDuration) {
        // Phase complete — flip and record start of new phase
        _isOn = !_isOn;
        _phaseStartEpoch = (int64_t)now;
        save();
        Serial.printf("[LIGHT] Phase → %s\n", _isOn ? "ON" : "OFF");
    }

    // Alert if phase has overrun by more than 30 minutes (schedule frozen)
    if ((uint64_t)elapsed > (uint64_t)phaseDuration + 1800U) {
        _alertFlags |= ALERT_SCHED_OVERDUE;
    } else {
        _alertFlags &= ~ALERT_SCHED_OVERDUE;
    }
}

uint32_t LightSchedule::remainingSec() const {
    if (_offSec == 0) return UINT32_MAX;  // Always ON
    time_t now = time(nullptr);
    if (now < 1000000000L || _phaseStartEpoch == 0) return 0;

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
    p.end();
}

void LightSchedule::load() {
    Preferences p;
    p.begin("lights", true);
    _phaseStartEpoch = p.getLong64("epoch", 0LL);
    _isOn            = p.getBool  ("ison",  true);
    // Saved mode is informational only; current mode is set by ClimateController
    p.end();
}

// ═══════════════════════════════════════════════════════════════════════════════
// ClimateController
// ═══════════════════════════════════════════════════════════════════════════════

ClimateController::ClimateController()
    : _mode(GROW_VEG),
      _humidifierOn(false), _topFanOn(false), _bottomFanOn(false),
      _dehumidifierOn(false), _heatMatOn(false)
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
        { 16.0f, 20.0f, 60.0f, 75.0f, 0.50f, 0.90f, 0.70f },  // night 6 h
        18, 6
    };

    // ── Flowering ────────────────────────────────────────────────────────────
    _profiles[GROW_FLOWER] = {
        "Flowering",
        { 18.0f, 26.0f, 40.0f, 60.0f, 1.00f, 1.60f, 1.30f },  // day  12 h
        { 14.0f, 18.0f, 50.0f, 70.0f, 0.70f, 1.10f, 0.90f },  // night 12 h
        12, 12
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
    savePrefs();
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

    // ── Threshold flags ───────────────────────────────────────────────────────
    const bool tempHigh  = t   > (p.tempMax + TEMP_HYST);
    const bool tempLow   = t   < (p.tempMin - TEMP_HYST);
    const bool humHigh   = h   > (p.humMax  + HUMIDITY_HYST);
    // Asymmetric VPD: turn-ON uses outer band; turn-OFF uses inner band (see below)
    const bool vpdHigh   = vpd > (p.vpdMax  + VPD_HYST);          // clearly too dry
    const bool vpdLow    = vpd < (p.vpdMin  - VPD_HYST);          // clearly too humid
    const bool vpdCrisis = vpd > (p.vpdMax  + 2.0f * VPD_HYST);   // severe dryness

    // ── LIGHTS (relay 4) ─────────────────────────────────────────────────────
    relays.setAutoState(LIGHTS, lightsOn);

    // ── TOP FAN (exhaust → outside) ──────────────────────────────────────────
    // Turn ON  when vpd < vpdMin - HYST  OR  temp > tempMax + HYST
    // Turn OFF when vpd >= vpdMin (inner band) AND temp not high
    {
        bool want;
        if (_topFanOn) {
            want = (vpd < p.vpdMin) || tempHigh;     // OFF at inner boundary
        } else {
            want = vpdLow || tempHigh;               // ON only at outer boundary
        }
        _topFanOn = want;
        relays.setAutoState(TOP_FAN, want);
    }

    // ── BOTTOM FAN (light-heat extractor → into room) ────────────────────────
    //
    // LIGHTS ON  → aggressive extraction:
    //   1. VPD crisis          → ON  (maximum airflow, thermal priority)
    //   2. Temp too high       → ON  (primary: push light heat into room)
    //   3. Hum high + top ON   → ON  (secondary: assist drying)
    //   else                   → OFF (stable; passive negative-pressure sufficient)
    //
    // LIGHTS OFF → protect tent warmth:
    //   1. Temp too low        → OFF (hard stop — never pull cold room air in)
    //   2. Temp too high       → ON  (residual heat from lights still dissipating)
    //   3. Hum high + top ON   → ON  (urgent night drying)
    //   else                   → OFF (let tent cool passively)
    {
        bool want = false;

        if (lightsOn) {
            if (vpdCrisis) {
                want = true;
            } else if (tempHigh) {
                want = true;
            } else if (humHigh && _topFanOn) {
                want = true;
            }
        } else {
            if (tempLow) {
                want = false;                // HARD STOP
            } else if (tempHigh) {
                want = true;
            } else if (humHigh && _topFanOn) {
                want = true;
            }
        }

        _bottomFanOn = want;
        relays.setAutoState(BOTTOM_FAN, want);
    }

    // ── HUMIDIFIER ───────────────────────────────────────────────────────────
    // Runs 24 / 7 — night VPD targets are lower so it naturally activates less.
    // Asymmetric hysteresis:
    //   Turn ON  when vpd > vpdMax + HYST  (outer band — clearly too dry)
    //   Turn OFF when vpd <= vpdMax        (inner band — back to acceptable)
    {
        bool want;
        if (_humidifierOn) {
            want = (vpd > p.vpdMax) && !humHigh;    // OFF at inner boundary
        } else {
            want = vpdHigh && !humHigh;             // ON only at outer boundary
        }
        if (humHigh) want = false;  // Hard safety cutoff
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

// ─── Persistence ─────────────────────────────────────────────────────────────
void ClimateController::savePrefs() {
    Preferences p;
    p.begin("climate", false);
    p.putUChar("mode", (uint8_t)_mode);
    p.end();
}

void ClimateController::loadPrefs() {
    Preferences p;
    p.begin("climate", true);
    uint8_t m = p.getUChar("mode", (uint8_t)GROW_VEG);
    _mode = (m < NUM_GROW_MODES) ? (GrowMode)m : GROW_VEG;
    p.end();
}
