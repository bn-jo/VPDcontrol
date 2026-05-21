#pragma once
#include <Arduino.h>
#include "sensors.h"
#include "relays.h"

// ─── Day / night target range ─────────────────────────────────────────────────
struct DayNightRange {
    float tempMin, tempMax;   // °C
    float humMin,  humMax;    // %RH
    float vpdMin,  vpdMax;    // kPa
    float vpdTarget;          // kPa ideal
};

// ─── Grow profile ─────────────────────────────────────────────────────────────
struct GrowProfile {
    const char*   name;
    DayNightRange day;        // Targets when lights are ON
    DayNightRange night;      // Targets when lights are OFF
    uint8_t       lightOnHours;   // 24 = always on (seedling)
    uint8_t       lightOffHours;  // 0  = no off period
};

enum GrowMode : uint8_t { GROW_SEEDLING = 0, GROW_VEG, GROW_FLOWER, GROW_LATE_FLOWER, GROW_DRYING, NUM_GROW_MODES };

// ─── Manual VPD target (overrides grow-profile vpdMin/vpdMax when enabled) ───
struct VpdTargetCfg {
    bool  enabled = false;
    float kpa     = 1.0f;   // target kPa
    float buffer  = 0.1f;   // deadband each side of target (kPa)
};

// ─── Alert flags (bit mask) ───────────────────────────────────────────────────
#define ALERT_NTP_MISSING    0x01u  // NTP not synced — schedule position uncertain
#define ALERT_SCHED_OVERDUE  0x02u  // Phase ran past its duration (schedule may be frozen)

// ─── Light schedule ───────────────────────────────────────────────────────────
// Manages the photoperiod clock. Uses NTP-backed epoch timestamps so the
// schedule survives reboots. Never resets automatically — only on first boot.
class LightSchedule {
public:
    void begin(GrowMode mode, const GrowProfile* profiles);
    void tick();   // Advance; call once per control cycle

    // Option A: keep running clock, just update phase lengths for new mode
    void onModeChange(GrowMode newMode, const GrowProfile* profiles);

    // Fixed daily start time (hour < 24 = enabled; 255 = disabled, use elapsed clock)
    void setDayStart(uint8_t hour, uint8_t min);

    bool     isOn()           const { return _isOn; }
    uint32_t remainingSec()   const;
    uint32_t phaseTotalSec()  const { return _isOn ? _onSec : _offSec; }
    uint8_t  alerts()         const { return _alertFlags; }
    bool     ntpSynced()      const { return _ntpSynced; }
    uint8_t  dayStartHour()   const { return _dayStartHour; }
    uint8_t  dayStartMin()    const { return _dayStartMin;  }

private:
    bool     _isOn;
    int64_t  _phaseStartEpoch;  // Unix epoch when current phase started
    uint32_t _onSec;
    uint32_t _offSec;
    GrowMode _mode;
    bool     _ntpSynced;
    uint8_t  _alertFlags;
    uint8_t  _dayStartHour = 0xFF;   // 255 = disabled
    uint8_t  _dayStartMin  = 0;

    void save();
    void load();
    void recoverFromNtp();  // Called once NTP becomes available
};

// ─── Climate controller ───────────────────────────────────────────────────────
class ClimateController {
public:
    ClimateController();
    void begin();
    void update(const SensorData& sd);          // Call every CONTROL_INTERVAL_MS (Core 0)
    void checkAutoTransition();                  // Call from loop() on Core 1 — stage transitions

    GrowMode              getMode()        const { return _mode; }
    const GrowProfile&    getProfile()          const { return _profiles[_mode]; }
    const GrowProfile&    getProfileByMode(GrowMode m) const { return _profiles[m]; }
    void                  setMode(GrowMode m);
    void                  setProfile(GrowMode mode, const DayNightRange& day, const DayNightRange& night);
    void                  resetProfile(GrowMode mode);
    void                  setDryingFast(bool fast);  // switch slow↔fast without resetting day counter
    bool                  isDryingFast()   const { return _dryingFast;  }

    bool                  isLightsOn()     const { return _sched.isOn(); }
    const LightSchedule&  lightSchedule()  const { return _sched; }
    uint8_t               alertFlags()     const { return _sched.alerts(); }

    void                  setDayStart(uint8_t hour, uint8_t min);
    void                  setVpdTarget(bool enabled, float kpa, float buffer);
    const VpdTargetCfg&   vpdTarget()      const { return _vpdTarget; }

    // A/C temp overrides — 0 = use active profile value; separate day / night thresholds
    void                  setAcTemps(float low, float high, bool night = false);
    float                 acDayLow()       const { return _acDayLow; }
    float                 acDayHigh()      const { return _acDayHigh; }
    float                 acNightLow()     const { return _acNightLow; }
    float                 acNightHigh()    const { return _acNightHigh; }

    // Post-A/C stabilisation delay — humidifier and heat mat are suppressed for
    // this many seconds after the A/C turns off, letting the environment settle.
    void                  setAcHumDelay(uint32_t sec);
    uint32_t              acHumDelaySec()  const { return _acHumDelaySec; }
    // Days elapsed since current stage was set (0 if NTP not yet synced)
    uint32_t              stageDay()        const;
    void                  setStageDay(uint32_t day);  // manual correction

    void savePrefs();
    void loadPrefs();
    void saveProfilePrefs();
    void loadProfilePrefs();
    void flushPrefsIfDirty();         // call from Core 1 loop — deferred NVS write
    void flushProfilePrefsIfDirty();  // call from Core 1 loop — deferred NVS write

private:
    GrowMode      _mode;
    bool          _dryingFast;
    GrowProfile   _profiles[NUM_GROW_MODES];
    LightSchedule _sched;
    VpdTargetCfg    _vpdTarget;
    float           _acDayLow     = 0.0f;   // 0 = follow day profile tempMin
    float           _acDayHigh    = 0.0f;   // 0 = follow day profile tempMax
    float           _acNightLow   = 0.0f;   // 0 = follow night profile tempMin
    float           _acNightHigh  = 0.0f;   // 0 = follow night profile tempMax
    uint32_t        _acHumDelaySec = 600;   // seconds humidifier/heat-mat stay suppressed after A/C turns off
    unsigned long   _acLastOffMs   = 0;     // millis() when A/C last turned off (0 = never)
    int64_t         _stageStartEpoch;   // Unix epoch when current stage was last set
    bool            _userModeLocked     = false;  // set when user explicitly picks any non-flower stage
    volatile bool   _prefsDirty         = false;  // deferred climate NVS write
    volatile bool   _profilePrefsDirty = false;  // deferred profile NVS write — flushed from Core 1

    // Hysteresis state (persist between control ticks)
    bool _humidifierOn;
    bool _topFanOn;
    bool _bottomFanOn;
    bool _dehumidifierOn;
    bool _heatMatOn;
    bool _acOn;

    // Ceramic heater pulse state (night auto mode)
    bool          _heatPulseOn      = false;  // currently in ON phase
    unsigned long _heatPulseStartMs = 0;      // millis() when ON phase started
    unsigned long _heatPulseOffMs   = 0;      // millis() when OFF phase started (0 = never fired yet)

    void computeOutputs(const SensorData& sd);
};

extern ClimateController climate;
