#pragma once
#include <Arduino.h>

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
    uint32_t minOnSec;     // minimum ON  duration before state may change
    uint32_t minOffSec;    // minimum OFF duration before state may change
    uint32_t maxOnSec;     // maximum ON duration in AUTO (0 = unlimited)

    // Soil-triggered auto watering (WATERING relay only)
    uint8_t  soilThreshold;    // % — open valve when soil drops below this (0 = disabled)
    uint32_t waterDurationSec; // how long to run each watering cycle (seconds)
    uint32_t waterFlowML;      // dripper flow rate in ml/min (used for volume estimate, 0 = unknown)

    // Fan direction — TOP_FAN and BOTTOM_FAN only
    // false = exhaust (removes air from tent); true = intake (brings air in)
    bool fanIntake;
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
    void setDuration(RelayIndex idx, uint32_t minOnSec, uint32_t maxOnSec);
    void setSoilWater(RelayIndex idx, uint8_t threshold, uint32_t durationSec);
    void setWaterFlow(RelayIndex idx, uint32_t mlPerMin);
    void setFanIntake(RelayIndex idx, bool intake);
    void setOnFor(RelayIndex idx, uint32_t seconds);  // turn ON for N seconds then OFF
    void setSoilMoisture(float pct, bool valid);
    void resetAllBuffers();   // Restore every relay's autoBuffer to factory default

    const RelayState& get(RelayIndex idx) const { return _r[idx]; }

    void savePrefs();
    void loadPrefs();

private:
    RelayState _r[NUM_RELAYS];
    float      _soilPct   = 0.0f;
    bool       _soilValid = false;

    void     applyPhysical(RelayIndex idx, bool on);
    bool     canChange(RelayIndex idx, bool newOn) const;
    bool     request(RelayIndex idx, bool on);
    void     tickTimer(RelayIndex idx);
    void     tickSchedule(RelayIndex idx);
};

extern RelayManager relays;
