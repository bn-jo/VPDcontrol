#include "relays.h"
#include "config.h"
#include <Preferences.h>
#include <time.h>

static int _currentDOY() {
    time_t now = time(nullptr);
    if (now < 1000000000L) return -1;
    struct tm t; localtime_r(&now, &t);
    return t.tm_yday;
}

RelayManager relays;

// ─── Static tables ────────────────────────────────────────────────────────────
static const uint8_t    PINS[NUM_RELAYS]  = { RELAY_TOP_FAN_PIN, RELAY_BOTTOM_FAN_PIN,
                                               RELAY_HUMIDIFIER_PIN, RELAY_LIGHTS_PIN,
                                               RELAY_DEHUMIDIFIER_PIN, RELAY_HEAT_MAT_PIN,
                                               RELAY_WATERING_PIN, RELAY_EXTRA_PIN };
static const char* const NAMES[NUM_RELAYS] = { "Top Fan", "Bottom Fan",
                                                "Humidifier", "Lights",
                                                "A/C", "Heat",
                                                "Watering", "Dehumidifier" };

// Default autoBuffer per relay — matches the control variable for each relay:
//   VPD relays:      VPD_HYST      (kPa)
//   Humidity relay:  HUMIDITY_HYST (%RH)
//   Temp relay:      TEMP_HYST     (°C)
//   Schedule/spare:  0
static const float DEFAULT_BUFFER[NUM_RELAYS] = {
    VPD_HYST,       // TOP_FAN      — VPD low
    VPD_HYST,       // BOTTOM_FAN   — VPD crisis
    VPD_HYST,       // HUMIDIFIER   — VPD high
    0.0f,           // LIGHTS       — schedule-driven
    0.0f,           // AC_RELAY     — A/C power relay (temp-driven or IR-driven)
    TEMP_HYST,      // HEAT_MAT     — temp low
    0.0f,           // WATERING     — timer-driven
    HUMIDITY_HYST,  // DEHUMIDIFIER — humidity high (was EXTRA)
};

// ─── Construction ─────────────────────────────────────────────────────────────
RelayManager::RelayManager() {
    // Init irrigation profiles to defaults
    for (int s = 0; s < 4; s++) _irrigProfiles[s] = IRRIG_DEFAULTS[1][s];  // coco default until NVS loaded
    // Init plant config
    _plantCfg = {};
    _plantCfg.count             = 1;
    _plantCfg.plants[0].potVolumeL = 15;
    _plantCfg.substrateType     = 1;     // coco default
    _plantCfg.precisionEnabled  = false;

    for (int i = 0; i < NUM_RELAYS; i++) {
        _r[i] = {};
        _r[i].pin              = PINS[i];
        _r[i].name             = NAMES[i];
        _r[i].mode             = RELAY_AUTO;
        _r[i].manualOn         = false;
        _r[i].autoOn           = false;
        _r[i].physicalOn       = false;
        _r[i].lastOnMs         = 0;
        _r[i].lastOffMs        = 0;
        _r[i].timerPhaseStart  = 0;
        _r[i].timerInOnPhase   = false;
        _r[i].autoBuffer       = DEFAULT_BUFFER[i];
        _r[i].minOnSec         = (i == DEHUMIDIFIER) ? 180 : MIN_RELAY_ON_MS  / 1000;
        _r[i].minOffSec        = (i == WATERING) ? 1800 : (i == DEHUMIDIFIER) ? 300 : MIN_RELAY_OFF_MS / 1000;
        _r[i].maxOnSec         = 0;
        _r[i].maxOnRestSec     = 0;
        _r[i].lastMaxOnMs      = 0;
        _r[i].manualTimeoutSec = (i == LIGHTS) ? LIGHTS_MANUAL_TIMEOUT_SEC : 0;
        _r[i].manualStartMs    = 0;
        _r[i].onForSec         = 0;
        _r[i].onForStartMs     = 0;
        _r[i].soilThreshold    = 0;
        _r[i].waterDurationSec = 300;
        _r[i].waterFlowML      = (i == WATERING) ? 500 : 0;
        _r[i].fanIntake        = false;
        // Precision irrigation fields
        if (i == WATERING) {
            _r[i].irrigProfile     = IRRIG_DEFAULTS[1][1];  // Veg/coco default until mode is set
            _r[i].soilAtStart      = 0.0f;
            _r[i].soilAtStartPrev  = 0.0f;
            _r[i].peakSoilPct      = 0.0f;
            _r[i].dryBackPct       = 0.0f;
            _r[i].lastWaterTs      = 0;
            _r[i].lastWaterDurSec  = 0;
            _r[i].lastWaterML      = 0;
            _r[i].todayML          = 0;
            _r[i].todayDOY         = -1;
            _r[i].adaptiveDurSec   = 0;
            _r[i].fixedDurMode     = false;
            _r[i].fixedDurSec      = 60;
            _r[i].pulsePhase       = RelayState::PULSE_IDLE;
            _r[i].pulsePhaseStart  = 0;
            _r[i].totalPulseOnMs   = 0;
        }
    }
}

// ─── Public API ───────────────────────────────────────────────────────────────
void RelayManager::begin() {
    for (int i = 0; i < NUM_RELAYS; i++) {
        pinMode(_r[i].pin, OUTPUT);
        applyPhysical((RelayIndex)i, false);
    }
    loadPrefs();
    loadIrrigPrefs();
    // Apply the current irrigation profile (mode will be set by climate after begin)
    _r[WATERING].irrigProfile = _irrigProfiles[_currentMode];
}

void RelayManager::setAutoState(RelayIndex idx, bool on) {
    if (!_r[idx].installed) { _r[idx].autoOn = false; return; }
    _r[idx].autoOn = on;
    // Actual physical change happens in update()
}

void RelayManager::setMode(RelayIndex idx, RelayMode mode) {
    _r[idx].mode = mode;
    if (mode == RELAY_MANUAL) {
        _r[idx].manualStartMs = millis();   // start auto-revert countdown
    } else if (mode == RELAY_TIMER) {
        _r[idx].timerPhaseStart = millis();
        _r[idx].timerInOnPhase  = true;
        request(idx, true);
    }
    savePrefs();
}

void RelayManager::setManual(RelayIndex idx, bool on) {
    _r[idx].manualOn   = on;
    _r[idx].onForSec   = 0;   // cancel any one-shot timer
    if (_r[idx].mode == RELAY_MANUAL) {
        applyPhysical(idx, on);   // bypass canChange — manual override is immediate
    }
    savePrefs();
}

void RelayManager::setOnFor(RelayIndex idx, uint32_t seconds) {
    _r[idx].manualOn     = true;
    _r[idx].onForSec     = seconds;
    _r[idx].onForStartMs = millis();
    if (_r[idx].mode == RELAY_MANUAL) {
        applyPhysical(idx, true);
    }
    // no savePrefs — one-shot state is transient
}

void RelayManager::setSchedule(RelayIndex idx, const ScheduleCfg& cfg) {
    _r[idx].schedule = cfg;
    savePrefs();
}

void RelayManager::setTimer(RelayIndex idx, uint32_t onSec, uint32_t offSec) {
    _r[idx].timer.onSec        = onSec;
    _r[idx].timer.offSec       = offSec;
    _r[idx].timerPhaseStart    = millis();
    _r[idx].timerInOnPhase     = true;
    request(idx, true);
    savePrefs();
}

// ─── Main update (call every loop) ───────────────────────────────────────────
void RelayManager::update() {
    // ── Deferred NVS save (avoids deep call stack from applyPhysical) ─────────
    if (_irrigPrefsDirty) {
        saveIrrigPrefs();
        _irrigPrefsDirty = false;
    }

    // ── Midnight rollover for daily water total (all modes) ───────────────────
    {
        int doy = _currentDOY();
        if (doy >= 0) {
            if (_r[WATERING].todayDOY < 0) {
                // First NTP sync — record today without resetting accumulated ml
                _r[WATERING].todayDOY = doy;
            } else if (doy != _r[WATERING].todayDOY) {
                // Actual midnight rollover — reset daily total
                _r[WATERING].todayML  = 0;
                _r[WATERING].todayDOY = doy;
            }
        }
    }

    for (int i = 0; i < NUM_RELAYS; i++) {
        RelayIndex idx = (RelayIndex)i;
        switch (_r[i].mode) {
            case RELAY_AUTO:
                if (idx == WATERING) {
                    const IrrigationProfile& ip = _r[i].irrigProfile;
                    bool usePrecision = _plantCfg.precisionEnabled && ip.enabled && _soilValid && !_probeMode;

                    if (usePrecision) {
                        // ── Precision mode: pulse-soak cycling ───────────────────────────
                        // Valve opens for ip.pulseOnSec, then closes for ip.pauseSec (soak).
                        // Soil target is checked only at the end of each soak — not during
                        // the pulse itself, where the sensor reads falsely high as water
                        // flows past it before the medium has absorbed it.
                        // Total valve-on time (sum of all pulses) is capped by maxWaterSec.

                        if (_r[i].pulsePhase == RelayState::PULSE_ON) {
                            // ── Currently pulsing — wait for pulse duration ───────────────
                            unsigned long pulseSoFarMs = millis() - _r[i].pulsePhaseStart;
                            unsigned long ceilMs = (unsigned long)(_r[i].fixedDurMode
                                ? _r[i].fixedDurSec : ip.maxWaterSec) * 1000UL;
                            if (_r[i].adaptiveDurSec > 0 && !_r[i].fixedDurMode) {
                                unsigned long adaptMs = (unsigned long)_r[i].adaptiveDurSec * 1000UL;
                                if (adaptMs < ceilMs) ceilMs = adaptMs;
                            }
                            bool pulseTimedOut = (pulseSoFarMs >= (unsigned long)ip.pulseOnSec * 1000UL);
                            bool maxReached    = (_r[i].totalPulseOnMs + pulseSoFarMs >= ceilMs);

                            if (pulseTimedOut || maxReached) {
                                // End this pulse — enter soak
                                _r[i].totalPulseOnMs += pulseSoFarMs;
                                _r[i].pulsePhase      = RelayState::PULSE_SOAK;
                                _r[i].pulsePhaseStart = millis();
                                applyPhysical(idx, false);
                            }

                        } else if (_r[i].pulsePhase == RelayState::PULSE_SOAK) {
                            // ── Soak phase — wait, then check sensor ──────────────────────
                            // During soak track soil peak (water distributes and rises)
                            if (_soilPct > _r[i].peakSoilPct)
                                _r[i].peakSoilPct = _soilPct;

                            unsigned long soakMs = millis() - _r[i].pulsePhaseStart;
                            if (soakMs < (unsigned long)ip.pauseSec * 1000UL) break; // still soaking

                            // Soak complete — evaluate
                            unsigned long ceilMs = (unsigned long)(_r[i].fixedDurMode
                                ? _r[i].fixedDurSec : ip.maxWaterSec) * 1000UL;
                            if (_r[i].adaptiveDurSec > 0 && !_r[i].fixedDurMode) {
                                unsigned long adaptMs = (unsigned long)_r[i].adaptiveDurSec * 1000UL;
                                if (adaptMs < ceilMs) ceilMs = adaptMs;
                            }
                            bool targetReached = !_r[i].fixedDurMode
                                                 && (_soilPct >= (float)ip.soilTargetPct);
                            bool maxReached    = (_r[i].totalPulseOnMs >= ceilMs);

                            if (targetReached || maxReached) {
                                // ── Session complete — record event ───────────────────────
                                uint32_t durSec = (uint32_t)(_r[i].totalPulseOnMs / 1000UL);
                                uint32_t mlDel  = (_r[i].waterFlowML > 0)
                                    ? (uint32_t)((float)_r[i].waterFlowML * durSec / 60.0f) : 0;
                                _r[i].dryBackPct      = 0.0f;
                                _r[i].lastWaterDurSec = durSec;
                                _r[i].lastWaterML     = mlDel;
                                { time_t _t = time(nullptr); if (_t > 1000000000L) _r[i].lastWaterTs = _t; }
                                _r[i].todayML        += mlDel;
                                _lastIrrigEvent = { _r[i].lastWaterTs, _r[i].soilAtStart,
                                                    _soilPct, durSec, mlDel, 0 }; // src=0 auto/precision
                                _irrigEventReady = true;
                                _irrigPrefsDirty = true;
                                _r[i].pulsePhase     = RelayState::PULSE_IDLE;
                                _r[i].totalPulseOnMs = 0;
                                // relay is already OFF from the last PULSE_ON→PULSE_SOAK transition
                            } else {
                                // Soil still below target — another pulse
                                _r[i].pulsePhase      = RelayState::PULSE_ON;
                                _r[i].pulsePhaseStart = millis();
                                applyPhysical(idx, true);
                            }

                        } else {
                            // ── PULSE_IDLE: not in a session ─────────────────────────────
                            // Update dry-back tracking
                            if (_r[i].peakSoilPct > 0.0f) {
                                float db = _r[i].peakSoilPct - _soilPct;
                                _r[i].dryBackPct = db > 0.0f ? db : 0.0f;
                            }
                            // Day/night rest selection
                            uint32_t rest = _lightsOn ? ip.minRestDaySec : ip.minRestNightSec;
                            bool rested = (_r[i].lastOffMs == 0 ||
                                (millis() - _r[i].lastOffMs) >= (unsigned long)rest * 1000UL);
                            if (rested && _soilPct < (float)ip.soilTriggerPct) {
                                // ── Adaptive duration: recalculate before starting session ─
                                if (!_r[i].fixedDurMode &&
                                    _r[i].lastWaterDurSec > 0 &&
                                    _r[i].peakSoilPct > _r[i].soilAtStartPrev + 0.5f)
                                {
                                    float actualRise  = _r[i].peakSoilPct - _r[i].soilAtStartPrev;
                                    float desiredRise = (float)ip.soilTargetPct - _soilPct;
                                    if (desiredRise > 0.5f) {
                                        uint32_t est = (uint32_t)((float)_r[i].lastWaterDurSec
                                                                  * (desiredRise / actualRise));
                                        est = max(3U, min(ip.maxWaterSec, est));
                                        if (_r[i].adaptiveDurSec > 0) {
                                            est = (uint32_t)(est * 0.7f
                                                            + _r[i].adaptiveDurSec * 0.3f);
                                            est = max(3U, min(ip.maxWaterSec, est));
                                        }
                                        _r[i].adaptiveDurSec = est;
                                    }
                                }
                                _r[i].soilAtStartPrev = _r[i].soilAtStart;
                                _r[i].soilAtStart     = _soilPct;
                                _r[i].peakSoilPct     = _soilPct;  // reset peak for this session
                                _r[i].totalPulseOnMs  = 0;
                                _r[i].pulsePhase      = RelayState::PULSE_ON;
                                _r[i].pulsePhaseStart = millis();
                                applyPhysical(idx, true);
                            }
                        }
                    } else if (_r[i].soilThreshold > 0 && _soilValid && !_probeMode) {
                        // ── Legacy threshold mode ─────────────────────────────
                        if (_r[i].physicalOn) {
                            if ((millis() - _r[i].lastOnMs) >=
                                (unsigned long)_r[i].waterDurationSec * 1000UL) {
                                applyPhysical(idx, false);
                            }
                        } else if (_soilPct < (float)_r[i].soilThreshold) {
                            bool rested = (_r[i].lastOffMs == 0 ||
                                (millis() - _r[i].lastOffMs) >=
                                (unsigned long)_r[i].minOffSec * 1000UL);
                            if (rested) applyPhysical(idx, true);
                        }
                    }
                    // else: relay stays OFF (no irrigation configured)
                } else {
                    // Standard AUTO: climate controller drives via setAutoState()
                    if (_r[i].physicalOn && _r[i].maxOnSec > 0 &&
                        (millis() - _r[i].lastOnMs) >= (unsigned long)_r[i].maxOnSec * 1000UL) {
                        _r[i].lastMaxOnMs = millis();
                        applyPhysical(idx, false);
                    } else {
                        // Max-off guard: force ON if off too long (e.g. exhaust fan for tent pressure)
                        bool forceOn = !_r[i].physicalOn
                                    && _r[i].maxOffSec > 0
                                    && _r[i].lastOffMs > 0
                                    && (millis() - _r[i].lastOffMs) >= (unsigned long)_r[i].maxOffSec * 1000UL;
                        request(idx, _r[i].autoOn || forceOn);
                    }
                }
                break;
            case RELAY_MANUAL:
                // Auto-revert to AUTO after timeout (lights = 20 min default)
                if (_r[i].manualTimeoutSec > 0 && _r[i].manualStartMs > 0 &&
                    (millis() - _r[i].manualStartMs) >= (unsigned long)_r[i].manualTimeoutSec * 1000UL) {
                    _r[i].mode          = RELAY_AUTO;
                    _r[i].manualStartMs = 0;
                    _r[i].onForSec      = 0;
                    savePrefs();
                } else {
                    // One-shot: turn OFF after onForSec seconds
                    if (_r[i].onForSec > 0 && _r[i].manualOn && _r[i].onForStartMs > 0 &&
                        (millis() - _r[i].onForStartMs) >= (unsigned long)_r[i].onForSec * 1000UL) {
                        _r[i].manualOn = false;
                        _r[i].onForSec = 0;
                    }
                    applyPhysical(idx, _r[i].manualOn);
                }
                break;
            case RELAY_TIMER:
                tickTimer(idx);
                break;
            case RELAY_SCHEDULE:
                tickSchedule(idx);
                break;
        }
    }
}

// ─── Private helpers ──────────────────────────────────────────────────────────
void RelayManager::applyPhysical(RelayIndex idx, bool on) {
    if (!_r[idx].installed) { on = false; }   // never energise a missing relay
    bool pinLevel = RELAY_ACTIVE_LOW ? !on : on;
    digitalWrite(_r[idx].pin, pinLevel);

    if (on && !_r[idx].physicalOn) {
        _r[idx].lastOnMs = millis();
        // Record soil at session start for non-precision modes (PULSE_IDLE means
        // the pulse-soak machine is not active — manual, timer, sched, or legacy).
        if (idx == WATERING && _r[idx].pulsePhase == RelayState::PULSE_IDLE) {
            _r[idx].soilAtStart = _soilValid ? _soilPct : -1.0f;
        }
    } else if (!on && _r[idx].physicalOn) {
        _r[idx].lastOffMs = millis();
        // Non-precision watering session complete: log event + update daily stats.
        // PULSE_IDLE = not a mid-session pulse→soak boundary (which uses PULSE_SOAK).
        if (idx == WATERING && _r[idx].pulsePhase == RelayState::PULSE_IDLE) {
            unsigned long durMs = _r[idx].lastOffMs - _r[idx].lastOnMs;
            uint32_t durSec = (uint32_t)(durMs / 1000UL);
            if (durSec > 0) {
                uint32_t ml = (_r[idx].waterFlowML > 0)
                    ? (uint32_t)((float)_r[idx].waterFlowML * durSec / 60.0f) : 0;
                _r[idx].todayML          += ml;
                _r[idx].lastWaterDurSec   = durSec;
                _r[idx].lastWaterML       = ml;
                { time_t _t = time(nullptr); if (_t > 1000000000L) _r[idx].lastWaterTs = _t; }
                _lastIrrigEvent = { _r[idx].lastWaterTs,
                                    _r[idx].soilAtStart,
                                    _soilValid ? _soilPct : -1.0f,
                                    durSec, ml, (uint8_t)_r[idx].mode };
                _irrigEventReady = true;
                _irrigPrefsDirty = true;
            }
        }
    }
    _r[idx].physicalOn = on;
}

bool RelayManager::canChange(RelayIndex idx, bool newOn) const {
    const RelayState& r = _r[idx];
    unsigned long now = millis();

    if (newOn && !r.physicalOn) {
        // Want to turn ON — check per-relay minimum OFF time has elapsed
        if (r.lastOffMs > 0 && (now - r.lastOffMs) < (unsigned long)r.minOffSec * 1000UL) return false;
        // Extra forced rest after Max ON fires
        if (r.maxOnRestSec > 0 && r.lastMaxOnMs > 0 &&
            (now - r.lastMaxOnMs) < (unsigned long)r.maxOnRestSec * 1000UL) return false;
    } else if (!newOn && r.physicalOn) {
        // Want to turn OFF — check per-relay minimum ON time has elapsed
        if (r.lastOnMs > 0 && (now - r.lastOnMs) < (unsigned long)r.minOnSec * 1000UL) return false;
    }
    return true;
}

bool RelayManager::request(RelayIndex idx, bool on) {
    if (_r[idx].physicalOn == on) return true;   // Already in desired state
    if (!canChange(idx, on))      return false;  // Too soon to switch
    applyPhysical(idx, on);
    return true;
}

void RelayManager::tickTimer(RelayIndex idx) {
    RelayState& r   = _r[idx];
    unsigned long   elapsed = millis() - r.timerPhaseStart;

    if (r.timerInOnPhase) {
        if (elapsed >= (unsigned long)r.timer.onSec * 1000UL) {
            r.timerPhaseStart  = millis();
            r.timerInOnPhase   = false;
            request(idx, false);
        }
    } else {
        if (elapsed >= (unsigned long)r.timer.offSec * 1000UL) {
            r.timerPhaseStart  = millis();
            r.timerInOnPhase   = true;
            request(idx, true);
        }
    }
}

void RelayManager::tickSchedule(RelayIndex idx) {
    time_t t = time(nullptr);
    if (t < 1000000000L) {          // NTP not yet synced
        request(idx, false);
        return;
    }
    struct tm now;
    localtime_r(&t, &now);

    // tm_wday: 0=Sun…6=Sat → bit 0=Mon…bit 6=Sun
    uint8_t dayBit = (now.tm_wday == 0) ? 6 : (uint8_t)(now.tm_wday - 1);
    if (!(_r[idx].schedule.daysMask & (1u << dayBit))) {
        request(idx, false);
        return;
    }

    int nowSec = now.tm_hour * 3600 + now.tm_min * 60 + now.tm_sec;
    bool inAny = false;

    for (int s = 0; s < _r[idx].schedule.slotCount && s < MAX_SCHED_SLOTS; s++) {
        const ScheduleSlot& sl = _r[idx].schedule.slots[s];
        int startSec = sl.startHour * 3600 + sl.startMin * 60;
        int endSec   = sl.endHour   * 3600 + sl.endMin   * 60;
        bool inWindow = (endSec <= startSec)            // same time or crosses midnight
                      ? (nowSec >= startSec || nowSec < endSec)
                      : (nowSec >= startSec && nowSec < endSec);
        if (inWindow) { inAny = true; break; }
    }

    request(idx, inAny);
}

void RelayManager::setBuffer(RelayIndex idx, float buf) {
    _r[idx].autoBuffer = buf;
    savePrefs();
}

void RelayManager::setDuration(RelayIndex idx, uint32_t minOnSec, uint32_t maxOnSec, uint32_t maxOnRestSec) {
    _r[idx].minOnSec     = minOnSec;
    _r[idx].maxOnSec     = maxOnSec;
    _r[idx].maxOnRestSec = maxOnRestSec;
    savePrefs();
}

void RelayManager::setMaxOff(RelayIndex idx, uint32_t maxOffSec) {
    _r[idx].maxOffSec = maxOffSec;
    savePrefs();
}

void RelayManager::setSoilWater(RelayIndex idx, uint8_t threshold, uint32_t durationSec) {
    _r[idx].soilThreshold    = threshold;
    _r[idx].waterDurationSec = durationSec;
    savePrefs();
}

void RelayManager::setWaterFlow(RelayIndex idx, uint32_t mlPerMin) {
    _r[idx].waterFlowML = mlPerMin;
    savePrefs();
}

void RelayManager::setFanIntake(RelayIndex idx, bool intake) {
    _r[idx].fanIntake = intake;
    savePrefs();
}

void RelayManager::setInstalled(RelayIndex idx, bool installed) {
    _r[idx].installed = installed;
    if (!installed) {
        // Turn off immediately — don't leave the relay energised
        applyPhysical(idx, false);
        _r[idx].autoOn     = false;
        _r[idx].physicalOn = false;
    }
    savePrefs();
}

void RelayManager::setProbePlacement(bool m) {
    _probeMode = m;
    // If enabling probe mode while a session is in progress, abort it cleanly
    if (m && _r[WATERING].pulsePhase != RelayState::PULSE_IDLE) {
        applyPhysical(WATERING, false);
        _r[WATERING].pulsePhase     = RelayState::PULSE_IDLE;
        _r[WATERING].totalPulseOnMs = 0;
    }
}

void RelayManager::setWaterDurMode(bool fixed, uint32_t fixedSec) {
    _r[WATERING].fixedDurMode = fixed;
    _r[WATERING].fixedDurSec  = (fixedSec > 0) ? fixedSec : 60;
    saveIrrigPrefs();
}

void RelayManager::resetAllBuffers() {
    for (int i = 0; i < NUM_RELAYS; i++) {
        _r[i].autoBuffer = DEFAULT_BUFFER[i];
    }
    savePrefs();
}

void RelayManager::setSoilMoisture(float pct, bool valid) {
    _soilPct   = pct;
    _soilValid = valid;
}

void RelayManager::setLightsOn(bool on) {
    _lightsOn = on;
}

void RelayManager::setIrrigMode(uint8_t stage) {
    // Map GrowMode enum → irrigation slot (4 buckets: 0=Seedling,1=Veg,2=Flower,3=Drying)
    // GROW_LATE_FLOWER (3) shares the Flower irrigation profile.
    // GROW_DRYING is now enum value 4 → slot 3.
    uint8_t irrigStage = (stage == 3) ? 2   // Late Flower → Flower irrigation
                       : (stage == 4) ? 3   // Drying → Drying irrigation
                       : stage;             // 0,1,2 direct
    if (irrigStage >= 4) return;
    _currentMode = irrigStage;
    _r[WATERING].irrigProfile = _irrigProfiles[irrigStage];
}

void RelayManager::setIrrigProfile(uint8_t stage, const IrrigationProfile& p) {
    if (stage >= 4) return;
    _irrigProfiles[stage] = p;
    // If this is the active stage, apply immediately
    if (stage == _currentMode) {
        _r[WATERING].irrigProfile = p;
    }
    saveIrrigPrefs();
}

void RelayManager::resetIrrigDefaults() {
    uint8_t sub = _plantCfg.substrateType < 3 ? _plantCfg.substrateType : 1;
    for (int s = 0; s < 4; s++) {
        _irrigProfiles[s] = IRRIG_DEFAULTS[sub][s];
    }
    _r[WATERING].irrigProfile = _irrigProfiles[_currentMode];
    saveIrrigPrefs();
}

const IrrigationProfile& RelayManager::getIrrigProfile(uint8_t stage) const {
    static const IrrigationProfile fallback = {};
    return (stage < 4) ? _irrigProfiles[stage] : fallback;
}

void RelayManager::setPlantConfig(const PlantConfig& cfg) {
    _plantCfg = cfg;
    saveIrrigPrefs();
}

bool RelayManager::popIrrigEvent(IrrigEvent& ev) {
    if (!_irrigEventReady) return false;
    ev = _lastIrrigEvent;
    _irrigEventReady = false;
    return true;
}

// ─── Irrigation persistence ───────────────────────────────────────────────────
void RelayManager::saveIrrigPrefs() {
    Preferences p;
    p.begin("irrig", false);
    p.putBool  ("en",  _plantCfg.precisionEnabled);
    p.putUChar ("cnt", _plantCfg.count);
    p.putUChar ("sub",      _plantCfg.substrateType);
    p.putUChar ("prof_sub", _plantCfg.substrateType);  // track which substrate profiles were saved for
    for (int i = 0; i < MAX_PLANTS; i++) {
        char k[8]; snprintf(k, sizeof(k), "p%d", i);
        p.putUShort(k, _plantCfg.plants[i].potVolumeL);
    }
    for (int s = 0; s < 4; s++) {
        const IrrigationProfile& ip = _irrigProfiles[s];
        char k[12];
        snprintf(k, sizeof(k), "s%d_en",  s); p.putBool  (k, ip.enabled);
        snprintf(k, sizeof(k), "s%d_tr",  s); p.putUChar (k, ip.soilTriggerPct);
        snprintf(k, sizeof(k), "s%d_tg",  s); p.putUChar (k, ip.soilTargetPct);
        snprintf(k, sizeof(k), "s%d_mx",  s); p.putUInt  (k, ip.maxWaterSec);
        snprintf(k, sizeof(k), "s%d_dr",  s); p.putUInt  (k, ip.minRestDaySec);
        snprintf(k, sizeof(k), "s%d_nr",  s); p.putUInt  (k, ip.minRestNightSec);
        snprintf(k, sizeof(k), "s%d_po",  s); p.putUInt  (k, ip.pulseOnSec);
        snprintf(k, sizeof(k), "s%d_ps",  s); p.putUInt  (k, ip.pauseSec);
    }
    // ── Watering runtime stats (persist across reboots) ──────────────────────
    p.putUInt  ("lastTs",   (uint32_t)_r[WATERING].lastWaterTs);
    p.putUInt  ("lastDur",  _r[WATERING].lastWaterDurSec);
    p.putUInt  ("lastML",   _r[WATERING].lastWaterML);
    p.putUInt  ("todayML",  _r[WATERING].todayML);
    p.putInt   ("todayDOY", _r[WATERING].todayDOY);
    p.putFloat ("peak",     _r[WATERING].peakSoilPct);
    p.putFloat ("dryBack",  _r[WATERING].dryBackPct);
    p.putUInt  ("adapDur",  _r[WATERING].adaptiveDurSec);
    p.putBool  ("fixMode",  _r[WATERING].fixedDurMode);
    p.putUInt  ("fixDur",   _r[WATERING].fixedDurSec);
    p.end();
}

void RelayManager::loadIrrigPrefs() {
    Preferences p;
    p.begin("irrig", true);
    _plantCfg.precisionEnabled  = p.getBool  ("en",  false);
    _plantCfg.count             = p.getUChar ("cnt", 1);
    _plantCfg.substrateType     = p.getUChar ("sub", 1);
    if (_plantCfg.count < 1 || _plantCfg.count > MAX_PLANTS) _plantCfg.count = 1;
    for (int i = 0; i < MAX_PLANTS; i++) {
        char k[8]; snprintf(k, sizeof(k), "p%d", i);
        _plantCfg.plants[i].potVolumeL = p.getUShort(k, (i == 0) ? 15 : 0);
    }
    // If substrate changed since last save (0xFF = never saved), reset all stage
    // profiles to the new substrate's research-backed defaults instead of loading
    // stale NVS values tuned for a different medium.
    uint8_t sub = _plantCfg.substrateType < 3 ? _plantCfg.substrateType : 1;
    bool useDefaults = (p.getUChar("prof_sub", 0xFF) != sub);
    for (int s = 0; s < 4; s++) {
        const IrrigationProfile& def = IRRIG_DEFAULTS[sub][s];
        if (useDefaults) {
            _irrigProfiles[s] = def;
        } else {
            char k[12];
            snprintf(k, sizeof(k), "s%d_en", s); _irrigProfiles[s].enabled         = p.getBool  (k, def.enabled);
            snprintf(k, sizeof(k), "s%d_tr", s); _irrigProfiles[s].soilTriggerPct  = p.getUChar (k, def.soilTriggerPct);
            snprintf(k, sizeof(k), "s%d_tg", s); _irrigProfiles[s].soilTargetPct   = p.getUChar (k, def.soilTargetPct);
            snprintf(k, sizeof(k), "s%d_mx", s); _irrigProfiles[s].maxWaterSec     = p.getUInt  (k, def.maxWaterSec);
            snprintf(k, sizeof(k), "s%d_dr", s); _irrigProfiles[s].minRestDaySec   = p.getUInt  (k, def.minRestDaySec);
            snprintf(k, sizeof(k), "s%d_nr", s); _irrigProfiles[s].minRestNightSec = p.getUInt  (k, def.minRestNightSec);
            snprintf(k, sizeof(k), "s%d_po", s); _irrigProfiles[s].pulseOnSec      = p.getUInt  (k, def.pulseOnSec);
            snprintf(k, sizeof(k), "s%d_ps", s); _irrigProfiles[s].pauseSec        = p.getUInt  (k, def.pauseSec);
        }
    }
    // ── Watering runtime stats ────────────────────────────────────────────────
    _r[WATERING].lastWaterTs      = (time_t)p.getUInt ("lastTs",   0);
    _r[WATERING].lastWaterDurSec  = p.getUInt ("lastDur",  0);
    _r[WATERING].lastWaterML      = p.getUInt ("lastML",   0);
    _r[WATERING].todayML          = p.getUInt ("todayML",  0);
    _r[WATERING].todayDOY         = p.getInt  ("todayDOY", -1);
    _r[WATERING].peakSoilPct      = p.getFloat("peak",     0.0f);
    _r[WATERING].dryBackPct       = p.getFloat("dryBack",  0.0f);
    _r[WATERING].adaptiveDurSec   = p.getUInt ("adapDur",  0);
    _r[WATERING].fixedDurMode     = p.getBool ("fixMode",  false);
    _r[WATERING].fixedDurSec      = p.getUInt ("fixDur",   60);
    p.end();
}

// ─── Persistence ──────────────────────────────────────────────────────────────
void RelayManager::savePrefs() {
    Preferences p;
    p.begin("relays", false);
    for (int i = 0; i < NUM_RELAYS; i++) {
        char k[16];
        snprintf(k, sizeof(k), "r%d_mode", i);   p.putUChar(k, (uint8_t)_r[i].mode);
        snprintf(k, sizeof(k), "r%d_man",  i);   p.putBool (k, _r[i].manualOn);
        snprintf(k, sizeof(k), "r%d_ton",  i);   p.putUInt (k, _r[i].timer.onSec);
        snprintf(k, sizeof(k), "r%d_toff", i);   p.putUInt (k, _r[i].timer.offSec);
        snprintf(k, sizeof(k), "r%d_sc",   i);   p.putUChar(k, _r[i].schedule.slotCount);
        snprintf(k, sizeof(k), "r%d_sday", i);   p.putUChar(k, _r[i].schedule.daysMask);
        for (int s = 0; s < MAX_SCHED_SLOTS; s++) {
            char sk[16];
            snprintf(sk, sizeof(sk), "r%d_%d_sh", i, s); p.putUChar(sk, _r[i].schedule.slots[s].startHour);
            snprintf(sk, sizeof(sk), "r%d_%d_sm", i, s); p.putUChar(sk, _r[i].schedule.slots[s].startMin);
            snprintf(sk, sizeof(sk), "r%d_%d_eh", i, s); p.putUChar(sk, _r[i].schedule.slots[s].endHour);
            snprintf(sk, sizeof(sk), "r%d_%d_em", i, s); p.putUChar(sk, _r[i].schedule.slots[s].endMin);
        }
        snprintf(k, sizeof(k), "r%d_buf",  i);   p.putFloat(k, _r[i].autoBuffer);
        snprintf(k, sizeof(k), "r%d_mion", i);   p.putUInt (k, _r[i].minOnSec);
        snprintf(k, sizeof(k), "r%d_miof", i);   p.putUInt (k, _r[i].minOffSec);
        snprintf(k, sizeof(k), "r%d_mxon", i);   p.putUInt (k, _r[i].maxOnSec);
        snprintf(k, sizeof(k), "r%d_mxrs", i);   p.putUInt (k, _r[i].maxOnRestSec);
        snprintf(k, sizeof(k), "r%d_mxof", i);   p.putUInt (k, _r[i].maxOffSec);
        snprintf(k, sizeof(k), "r%d_sthr", i);   p.putUChar(k, _r[i].soilThreshold);
        snprintf(k, sizeof(k), "r%d_wdur", i);   p.putUInt (k, _r[i].waterDurationSec);
        snprintf(k, sizeof(k), "r%d_fi",   i);   p.putBool (k, _r[i].fanIntake);
        snprintf(k, sizeof(k), "r%d_wfl",  i);   p.putUInt (k, _r[i].waterFlowML);
        snprintf(k, sizeof(k), "r%d_inst", i);   p.putBool (k, _r[i].installed);
    }
    p.end();
}

void RelayManager::loadPrefs() {
    Preferences p;
    p.begin("relays", true);
    for (int i = 0; i < NUM_RELAYS; i++) {
        char k[16];
        snprintf(k, sizeof(k), "r%d_mode", i);   _r[i].mode                    = (RelayMode)p.getUChar(k, RELAY_AUTO);
        snprintf(k, sizeof(k), "r%d_man",  i);   _r[i].manualOn                = p.getBool (k, false);
        snprintf(k, sizeof(k), "r%d_ton",  i);   _r[i].timer.onSec             = p.getUInt (k, 3600);
        snprintf(k, sizeof(k), "r%d_toff", i);   _r[i].timer.offSec            = p.getUInt (k, 1800);
        snprintf(k, sizeof(k), "r%d_sc",   i);   _r[i].schedule.slotCount  = p.getUChar(k, 1);
        snprintf(k, sizeof(k), "r%d_sday", i);   _r[i].schedule.daysMask   = p.getUChar(k, 0x7F);
        if (_r[i].schedule.slotCount < 1 || _r[i].schedule.slotCount > MAX_SCHED_SLOTS)
            _r[i].schedule.slotCount = 1;
        for (int s = 0; s < MAX_SCHED_SLOTS; s++) {
            char sk[16];
            snprintf(sk, sizeof(sk), "r%d_%d_sh", i, s); _r[i].schedule.slots[s].startHour = p.getUChar(sk, 8);
            snprintf(sk, sizeof(sk), "r%d_%d_sm", i, s); _r[i].schedule.slots[s].startMin  = p.getUChar(sk, 0);
            snprintf(sk, sizeof(sk), "r%d_%d_eh", i, s); _r[i].schedule.slots[s].endHour   = p.getUChar(sk, 9);
            snprintf(sk, sizeof(sk), "r%d_%d_em", i, s); _r[i].schedule.slots[s].endMin    = p.getUChar(sk, 0);
        }
        snprintf(k, sizeof(k), "r%d_buf",  i);   _r[i].autoBuffer       = p.getFloat(k, DEFAULT_BUFFER[i]);
        snprintf(k, sizeof(k), "r%d_mion", i);   _r[i].minOnSec         = p.getUInt (k, (i == DEHUMIDIFIER) ? 180 : MIN_RELAY_ON_MS  / 1000);
        snprintf(k, sizeof(k), "r%d_miof", i);   _r[i].minOffSec        = p.getUInt (k, (i == WATERING) ? 1800 : (i == DEHUMIDIFIER) ? 300 : MIN_RELAY_OFF_MS / 1000);
        snprintf(k, sizeof(k), "r%d_mxon", i);   _r[i].maxOnSec         = p.getUInt (k, 0);
        snprintf(k, sizeof(k), "r%d_mxrs", i);   _r[i].maxOnRestSec     = p.getUInt (k, 0);
        // TOP_FAN defaults to 60 s max-off (negative pressure guard); all others default disabled
        snprintf(k, sizeof(k), "r%d_mxof", i);   _r[i].maxOffSec        = p.getUInt (k, (i == TOP_FAN) ? 60 : 0);
        snprintf(k, sizeof(k), "r%d_sthr", i);   _r[i].soilThreshold    = p.getUChar(k, 0);
        snprintf(k, sizeof(k), "r%d_wdur", i);   _r[i].waterDurationSec = p.getUInt (k, 300);
        snprintf(k, sizeof(k), "r%d_fi",   i);   _r[i].fanIntake        = p.getBool (k, false);
        snprintf(k, sizeof(k), "r%d_wfl",  i);   _r[i].waterFlowML      = p.getUInt (k, (i == WATERING) ? 500 : 0);
        snprintf(k, sizeof(k), "r%d_inst", i);   _r[i].installed        = p.getBool (k, true);
    }
    p.end();
}
