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

enum GrowMode : uint8_t { GROW_SEEDLING = 0, GROW_VEG, GROW_FLOWER, NUM_GROW_MODES };

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

    bool     isOn()           const { return _isOn; }
    uint32_t remainingSec()   const;
    uint32_t phaseTotalSec()  const { return _isOn ? _onSec : _offSec; }
    uint8_t  alerts()         const { return _alertFlags; }
    bool     ntpSynced()      const { return _ntpSynced; }

private:
    bool     _isOn;
    int64_t  _phaseStartEpoch;  // Unix epoch when current phase started
    uint32_t _onSec;
    uint32_t _offSec;
    GrowMode _mode;
    bool     _ntpSynced;
    uint8_t  _alertFlags;

    void save();
    void load();
    void recoverFromNtp();  // Called once NTP becomes available
};

// ─── Climate controller ───────────────────────────────────────────────────────
class ClimateController {
public:
    ClimateController();
    void begin();
    void update(const SensorData& sd);  // Call every CONTROL_INTERVAL_MS

    GrowMode              getMode()        const { return _mode; }
    const GrowProfile&    getProfile()     const { return _profiles[_mode]; }
    void                  setMode(GrowMode m);

    bool                  isLightsOn()     const { return _sched.isOn(); }
    const LightSchedule&  lightSchedule()  const { return _sched; }
    uint8_t               alertFlags()     const { return _sched.alerts(); }

    void savePrefs();
    void loadPrefs();

private:
    GrowMode      _mode;
    GrowProfile   _profiles[NUM_GROW_MODES];
    LightSchedule _sched;

    // Hysteresis state (persist between control ticks)
    bool _humidifierOn;
    bool _topFanOn;
    bool _bottomFanOn;
    bool _dehumidifierOn;
    bool _heatMatOn;

    void computeOutputs(const SensorData& sd);
};

extern ClimateController climate;
