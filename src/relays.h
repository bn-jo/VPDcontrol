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

    // Timing guards (hysteresis / min on-off)
    unsigned long lastOnMs;
    unsigned long lastOffMs;
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

    const RelayState& get(RelayIndex idx) const { return _r[idx]; }

    void savePrefs();
    void loadPrefs();

private:
    RelayState _r[NUM_RELAYS];

    void     applyPhysical(RelayIndex idx, bool on);
    bool     canChange(RelayIndex idx, bool newOn) const;
    bool     request(RelayIndex idx, bool on);
    void     tickTimer(RelayIndex idx);
    void     tickSchedule(RelayIndex idx);
};

extern RelayManager relays;
