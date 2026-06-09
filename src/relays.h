#pragma once
#include <Arduino.h>
#include "config.h"

// ─── Irrigation event — emitted when any watering cycle completes ─────────────
struct IrrigEvent {
    time_t   ts;
    float    soilBefore;   // soil % at session start (-1 if no sensor)
    float    soilAfter;    // soil % at session end   (-1 if no sensor)
    uint32_t durationSec;  // total valve-on time (seconds)
    uint32_t volumeML;     // estimated volume delivered (ml)
    uint8_t  src;          // 0=auto/precision  1=manual  2=timer  3=schedule
};

enum RelayMode  : uint8_t { RELAY_AUTO = 0, RELAY_MANUAL, RELAY_TIMER, RELAY_SCHEDULE };
enum RelayIndex : uint8_t { TOP_FAN = 0, BOTTOM_FAN, HUMIDIFIER, LIGHTS,
                            DEHUMIDIFIER, HEAT_MAT, WATERING, EXTRA, NUM_RELAYS };

struct TimerCfg {
    uint32_t onSec  = 3600;   // default 1 h ON
    uint32_t offSec = 1800;   // default 30 min OFF
};

#define MAX_SCHED_SLOTS 4

struct ScheduleSlot {
    uint8_t startHour = 8;
    uint8_t startMin  = 0;
    uint8_t endHour   = 9;
    uint8_t endMin    = 0;
};

struct ScheduleCfg {
    uint8_t      slotCount = 1;          // how many slots are active (1–MAX_SCHED_SLOTS)
    ScheduleSlot slots[MAX_SCHED_SLOTS]; // time windows
    uint8_t      daysMask  = 0x7F;       // bit0=Mon … bit6=Sun; 0x7F = every day
};

struct RelayState {
    uint8_t     pin;
    const char* name;
    RelayMode   mode;

    // Manual state (used when mode == RELAY_MANUAL)
    bool manualOn;

    // Timer bookkeeping
    TimerCfg      timer;
    unsigned long timerPhaseStart;
    bool          timerInOnPhase;

    // Schedule config
    ScheduleCfg   schedule;

    // Auto state written by ClimateController
    bool autoOn;

    // Physical output
    bool physicalOn;

    // Timing guards (per-relay, replaces global MIN_RELAY_ON/OFF_MS)
    unsigned long lastOnMs;
    unsigned long lastOffMs;

    // Manual override timeout — auto-revert to AUTO after this many seconds (0 = disabled)
    uint32_t      manualTimeoutSec;
    unsigned long manualStartMs;

    // One-shot timed ON — turns OFF automatically after onForSec seconds (0 = disabled)
    uint32_t      onForSec;
    unsigned long onForStartMs;

    // Per-relay AUTO mode tuning
    float    autoBuffer;   // deadband for this relay's control variable
                           //   VPD relays (fan/humidifier):  kPa
                           //   Humidity relays (dehumidifier): %RH
                           //   Temperature relays (heat mat):  °C
    uint32_t minOnSec;       // minimum ON  duration before state may change
    uint32_t minOffSec;      // minimum OFF duration before state may change
    uint32_t maxOnSec;       // maximum ON duration in AUTO (0 = unlimited)
    uint32_t maxOnRestSec;   // forced rest after Max ON fires (0 = use minOffSec only)
    unsigned long lastMaxOnMs; // millis() when Max ON last triggered
    uint32_t maxOffSec;      // max OFF duration in AUTO — fan forced back ON after this (0 = disabled)
                             // Use for exhaust fan to maintain negative grow room pressure

    // Soil-triggered auto watering (WATERING relay only)
    uint8_t  soilThreshold;    // % — legacy: open valve when soil drops below this (0 = disabled)
    uint32_t waterDurationSec; // legacy fixed cycle duration (seconds)
    uint32_t waterFlowML;      // dripper flow rate in ml/min (used for volume estimate, 0 = unknown)

    // Precision irrigation (WATERING relay only) ─────────────────────────────
    IrrigationProfile irrigProfile;   // active profile for the current grow stage
    float    soilAtStart;             // soil % when current watering cycle began
    float    soilAtStartPrev;         // soilAtStart from the PREVIOUS cycle (for adaptive calc)
    float    peakSoilPct;             // highest soil % recorded after last watering
    float    dryBackPct;              // peakSoilPct − current soil (dry-back tracking)
    time_t   lastWaterTs;             // epoch of last completed cycle
    uint32_t lastWaterDurSec;         // duration of last cycle (seconds)
    uint32_t lastWaterML;             // estimated volume of last cycle (ml)
    uint32_t todayML;                 // cumulative today in ml (resets at midnight)
    int      todayDOY;                // day-of-year for midnight rollover detection

    // Adaptive / fixed duration (WATERING relay only) ─────────────────────────
    uint32_t adaptiveDurSec;  // auto-calibrated total on-time estimate (0 = no history yet)
    bool     fixedDurMode;    // true = ignore soil sensor, run for fixedDurSec total on-time
    uint32_t fixedDurSec;     // user-set fixed total on-time (seconds, delivered as pulses)

    // Pulse-soak state (precision irrigation, WATERING relay only) ────────────
    enum PulsePhase : uint8_t { PULSE_IDLE=0, PULSE_ON=1, PULSE_SOAK=2 };
    PulsePhase    pulsePhase;       // current phase of the pulse-soak cycle
    unsigned long pulsePhaseStart;  // millis() when current pulse or soak started
    unsigned long totalPulseOnMs;   // cumulative valve-open ms this session

    // Fan direction — TOP_FAN and BOTTOM_FAN only
    // false = exhaust (removes air from grow room); true = intake (brings air in)
    bool fanIntake;

    // Physically connected to the grow room — if false, AUTO logic skips this relay
    // and the GPIO is never toggled. Set via the UI checkbox "Installed".
    bool installed = true;
};

class RelayManager {
public:
    RelayManager();
    void begin();

    // Call every loop iteration — runs timers and applies pending states
    void update();

    // Climate controller calls this; physical change happens in update()
    void setAutoState(RelayIndex idx, bool on);

    void setMode(RelayIndex idx, RelayMode mode);
    void setManual(RelayIndex idx, bool on);
    void setTimer(RelayIndex idx, uint32_t onSec, uint32_t offSec);
    void setSchedule(RelayIndex idx, const ScheduleCfg& cfg);
    void setBuffer(RelayIndex idx, float buf);
    void setDuration(RelayIndex idx, uint32_t minOnSec, uint32_t maxOnSec, uint32_t maxOnRestSec = 0);
    void setMaxOff(RelayIndex idx, uint32_t maxOffSec);  // 0 = disable; >0 = max seconds OFF (pressure guard)
    void setSoilWater(RelayIndex idx, uint8_t threshold, uint32_t durationSec);
    void setWaterFlow(RelayIndex idx, uint32_t mlPerMin);
    void setFanIntake(RelayIndex idx, bool intake);
    void setOnFor(RelayIndex idx, uint32_t seconds);  // turn ON for N seconds then OFF
    void setInstalled(RelayIndex idx, bool installed); // mark relay as connected/missing
    void setSoilMoisture(float pct, bool valid);
    void setWaterDurMode(bool fixed, uint32_t fixedSec);  // set adaptive vs fixed duration
    void setProbePlacement(bool m);  // suppress soil-triggered watering while moving probe
    bool probePlacementMode() const { return _probeMode; }

    // ── Precision irrigation ─────────────────────────────────────────────────
    void setIrrigMode(uint8_t stage);                                        // call on grow-mode change
    void setIrrigProfile(uint8_t stage, const IrrigationProfile& p);        // UI edits a stage profile
    void resetIrrigDefaults();                                               // reset all stages to substrate defaults
    const IrrigationProfile& getIrrigProfile(uint8_t stage) const;          // read by webserver
    void setLightsOn(bool on);                                               // called each control tick
    void setPlantConfig(const PlantConfig& cfg);
    const PlantConfig& getPlantConfig() const { return _plantCfg; }
    bool popIrrigEvent(IrrigEvent& ev);                                      // drain event → main loop

    // Irrigation runtime stats (WATERING relay)
    float    getDryBackPct()   const { return _r[WATERING].dryBackPct; }
    float    getPeakSoilPct()  const { return _r[WATERING].peakSoilPct; }
    time_t   getLastWaterTs()  const { return _r[WATERING].lastWaterTs; }
    uint32_t getLastWaterDur() const { return _r[WATERING].lastWaterDurSec; }
    uint32_t getLastWaterML()  const { return _r[WATERING].lastWaterML; }
    uint32_t getTodayML()      const { return _r[WATERING].todayML; }

    void saveIrrigPrefs();
    void loadIrrigPrefs();

    const RelayState& get(RelayIndex idx) const { return _r[idx]; }

    void savePrefs();
    void loadPrefs();
    void saveInstalledFlags();
    void loadInstalledFlags();
    void flushPrefsIfDirty();   // call from Core 1 loop — writes deferred NVS saves

private:
    RelayState _r[NUM_RELAYS];
    float      _soilPct   = 0.0f;
    bool       _soilValid = false;

    // Precision irrigation state
    IrrigationProfile _irrigProfiles[4];   // stored profiles, one per grow stage
    PlantConfig       _plantCfg;
    bool              _lightsOn    = false;
    uint8_t           _currentMode = 1;    // default Veg
    IrrigEvent        _lastIrrigEvent;
    bool              _irrigEventReady = false;
    bool              _irrigPrefsDirty   = false;  // deferred irrig NVS write — flushed from Core 1
    bool              _installedDirty   = false;  // deferred installed-flags NVS write
    bool              _prefsDirty       = false;  // deferred relay NVS write — flushed from Core 1
    bool              _probeMode        = false;  // probe placement: suppress soil-triggered watering

    void     applyPhysical(RelayIndex idx, bool on);
    bool     canChange(RelayIndex idx, bool newOn) const;
    bool     request(RelayIndex idx, bool on);
    void     tickTimer(RelayIndex idx);
    void     tickSchedule(RelayIndex idx);
};

extern RelayManager relays;
