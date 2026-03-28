#include "relays.h"
#include "config.h"
#include <Preferences.h>
#include <time.h>

RelayManager relays;

// ─── Static tables ────────────────────────────────────────────────────────────
static const uint8_t    PINS[NUM_RELAYS]  = { RELAY_TOP_FAN_PIN, RELAY_BOTTOM_FAN_PIN,
                                               RELAY_HUMIDIFIER_PIN, RELAY_LIGHTS_PIN,
                                               RELAY_DEHUMIDIFIER_PIN, RELAY_HEAT_MAT_PIN,
                                               RELAY_WATERING_PIN, RELAY_EXTRA_PIN };
static const char* const NAMES[NUM_RELAYS] = { "Top Fan", "Bottom Fan",
                                                "Humidifier", "Lights",
                                                "Dehumidifier", "Heat",
                                                "Watering", "Extra" };

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
    HUMIDITY_HYST,  // DEHUMIDIFIER — humidity high
    TEMP_HYST,      // HEAT_MAT     — temp low
    0.0f,           // WATERING     — timer-driven
    0.0f,           // EXTRA        — spare
};

// ─── Construction ─────────────────────────────────────────────────────────────
RelayManager::RelayManager() {
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
        _r[i].minOnSec         = MIN_RELAY_ON_MS  / 1000;
        _r[i].minOffSec        = MIN_RELAY_OFF_MS / 1000;
        _r[i].maxOnSec         = 0;
        _r[i].manualTimeoutSec = (i == LIGHTS) ? LIGHTS_MANUAL_TIMEOUT_SEC : 0;
        _r[i].manualStartMs    = 0;
    }
    // Lights default autoOn = false; ClimateController drives them via schedule
}

// ─── Public API ───────────────────────────────────────────────────────────────
void RelayManager::begin() {
    for (int i = 0; i < NUM_RELAYS; i++) {
        pinMode(_r[i].pin, OUTPUT);
        applyPhysical((RelayIndex)i, false);
    }
    loadPrefs();
}

void RelayManager::setAutoState(RelayIndex idx, bool on) {
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
    _r[idx].manualOn = on;
    if (_r[idx].mode == RELAY_MANUAL) {
        request(idx, on);
    }
    savePrefs();
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
    for (int i = 0; i < NUM_RELAYS; i++) {
        RelayIndex idx = (RelayIndex)i;
        switch (_r[i].mode) {
            case RELAY_AUTO:
                // Enforce MAX ON duration — hard-cut if relay has been on too long
                if (_r[i].physicalOn && _r[i].maxOnSec > 0 &&
                    (millis() - _r[i].lastOnMs) >= (unsigned long)_r[i].maxOnSec * 1000UL) {
                    applyPhysical(idx, false);
                } else {
                    request(idx, _r[i].autoOn);
                }
                break;
            case RELAY_MANUAL:
                // Auto-revert to AUTO after timeout (lights = 20 min default)
                if (_r[i].manualTimeoutSec > 0 && _r[i].manualStartMs > 0 &&
                    (millis() - _r[i].manualStartMs) >= (unsigned long)_r[i].manualTimeoutSec * 1000UL) {
                    _r[i].mode         = RELAY_AUTO;
                    _r[i].manualStartMs = 0;
                    savePrefs();
                } else {
                    request(idx, _r[i].manualOn);
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
    bool pinLevel = RELAY_ACTIVE_LOW ? !on : on;
    digitalWrite(_r[idx].pin, pinLevel);

    if (on && !_r[idx].physicalOn) {
        _r[idx].lastOnMs = millis();
    } else if (!on && _r[idx].physicalOn) {
        _r[idx].lastOffMs = millis();
    }
    _r[idx].physicalOn = on;
}

bool RelayManager::canChange(RelayIndex idx, bool newOn) const {
    const RelayState& r = _r[idx];
    unsigned long now = millis();

    if (newOn && !r.physicalOn) {
        // Want to turn ON — check per-relay minimum OFF time has elapsed
        if (r.lastOffMs > 0 && (now - r.lastOffMs) < (unsigned long)r.minOffSec * 1000UL) return false;
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

void RelayManager::setDuration(RelayIndex idx, uint32_t minOnSec, uint32_t maxOnSec) {
    _r[idx].minOnSec = minOnSec;
    _r[idx].maxOnSec = maxOnSec;
    savePrefs();
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
        snprintf(k, sizeof(k), "r%d_buf",  i);   _r[i].autoBuffer = p.getFloat(k, DEFAULT_BUFFER[i]);
        snprintf(k, sizeof(k), "r%d_mion", i);   _r[i].minOnSec   = p.getUInt (k, MIN_RELAY_ON_MS  / 1000);
        snprintf(k, sizeof(k), "r%d_miof", i);   _r[i].minOffSec  = p.getUInt (k, MIN_RELAY_OFF_MS / 1000);
        snprintf(k, sizeof(k), "r%d_mxon", i);   _r[i].maxOnSec   = p.getUInt (k, 0);
    }
    p.end();
}
