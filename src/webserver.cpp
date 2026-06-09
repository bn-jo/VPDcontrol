#include "webserver.h"
#include "ui_html.h"
#include "config.h"
#include "sensors.h"
#include "soil.h"
#include "relays.h"
#include "climate.h"
#include "datalogger.h"
#include "wifisensors.h"
#include "intakesensor.h"
#include "syslog.h"
#include "crashlog.h"
#include "eventlog.h"
#include "irremote.h"

#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <Update.h>

static AsyncWebServer server(80);
static AsyncWebSocket ws("/ws");

// ─── Shared state-JSON resources ──────────────────────────────────────────────
// Single static output buffer + mutex prevents two concurrent callers (loop() vs
// async_tcp task) from racing on heap String allocations or the former static
// sensBuf inside buildStateJson(), both of which caused PANIC-CRASH.
static SemaphoreHandle_t _jsonMux      = nullptr;
static char              _stateJsonBuf[8192];
static char              _wsSensBuf[WIFI_SENSOR_MAX * 220 + 8];

// Set from the WS handler (async_tcp); the actual wipe runs in webBroadcast()
// on Core 1 (loop) so it never preempts an in-progress eventlog()/NVS flash write.
// Bitmask: which logs to clear on the next loop tick.
#define CLEAR_SYS    0x01
#define CLEAR_EVENT  0x02
#define CLEAR_CRASH  0x04
static volatile uint8_t  _clearPending = 0;

// ─── Auth helper ──────────────────────────────────────────────────────────────
// Always require HTTP Basic Auth — credentials defined in config.h.
static bool checkAuth(AsyncWebServerRequest* req) {
    if (req->authenticate(WEB_AUTH_USER, WEB_AUTH_PASS)) return true;
    req->requestAuthentication();
    return false;
}

// ─── State JSON builder ────────────────────────────────────────────────────────
// Caller MUST hold _jsonMux before calling and release it after consuming buf.
// buf == _stateJsonBuf; protected by _jsonMux to prevent re-entrancy between
// loop() (webBroadcast) and async_tcp task (onWsEvent / /api/state).
static void buildStateJson(char* buf, size_t bufSize) {
    static JsonDocument doc;  // static: allocated once, no heap churn every 2 s
    doc.clear();
    doc["type"] = "state";
    doc["rssi"] = WiFi.RSSI();

    // Soil moisture
    const SoilData& sd_soil = soil.data();
    doc["soil"]       = sd_soil.moisture;
    doc["soilValid"]  = sd_soil.valid;
    doc["soilRawAdc"] = soil.rawAdc();
    doc["soilAdcDry"] = soil.adcDry();
    doc["soilAdcWet"] = soil.adcWet();

    const SensorData& sd = sensors.data();

    // Average local sensor with any reachable WiFi sensors
    float dispTemp, dispHum, dispVpd;
    float remoteT, remoteH;
    bool  remoteValid = wifiSensors.getAverage(remoteT, remoteH);
    if (remoteValid) {
        dispTemp = (sd.temperature + remoteT) / 2.0f;
        dispHum  = (sd.humidity    + remoteH) / 2.0f;
        dispVpd  = SensorManager::calcVPD(dispTemp, dispHum);
    } else {
        dispTemp = sd.temperature;
        dispHum  = sd.humidity;
        dispVpd  = sd.vpd;
    }

    doc["temp"]            = dispTemp;
    doc["hum"]             = dispHum;
    doc["vpd"]             = dispVpd;
    doc["vpdTrend"]        = sd.vpdTrend;
    doc["valid"]           = sd.valid;
    doc["remoteConnected"] = remoteValid;

    // WiFi sensor array (name, T/H, valid)
    // _wsSensBuf is at file scope, protected by _jsonMux held by the caller.
    wifiSensors.getJson(_wsSensBuf, sizeof(_wsSensBuf));
    doc["wifiSensors"] = serialized(_wsSensBuf);

    doc["growMode"]   = (int)climate.getMode();
    doc["dryingFast"] = climate.isDryingFast();
    doc["stageDay"] = climate.stageDay();   // Day 1 = first day of current stage

    const VpdTargetCfg& vt = climate.vpdTarget();
    JsonObject vtObj        = doc["vpdTarget"].to<JsonObject>();
    vtObj["enabled"]        = vt.enabled;
    vtObj["kpa"]            = vt.kpa;
    vtObj["buffer"]         = vt.buffer;

    JsonObject acObj        = doc["acTemps"].to<JsonObject>();
    acObj["dayLow"]         = climate.acDayLow();
    acObj["dayHigh"]        = climate.acDayHigh();
    acObj["nightLow"]       = climate.acNightLow();
    acObj["nightHigh"]      = climate.acNightHigh();

    JsonObject hpObj        = doc["heatPulse"].to<JsonObject>();
    hpObj["onSec"]          = climate.heatPulseOnSec();
    hpObj["restSec"]        = climate.heatPulseRestSec();
    hpObj["target"]         = climate.heatTarget();
    acObj["delay"]          = climate.acHumDelaySec();

    // Intake air sensor (DHT11 outside grow tent)
    const IntakeData& isd = intakeSensor.data();
    JsonObject intake      = doc["intake"].to<JsonObject>();
    intake["temp"]         = isd.temperature;
    intake["hum"]          = isd.humidity;
    intake["valid"]        = isd.valid;

    // Active profile targets — correct day/night set
    const bool         lightsOn = climate.isLightsOn();
    const GrowProfile& gp       = climate.getProfile();
    const DayNightRange& p      = lightsOn ? gp.day : gp.night;

    JsonObject prof  = doc["profile"].to<JsonObject>();
    prof["name"]         = gp.name;
    prof["isDay"]        = lightsOn;
    prof["tempMin"]      = p.tempMin;
    prof["tempMax"]      = p.tempMax;
    prof["humMin"]       = p.humMin;
    prof["humMax"]       = p.humMax;
    prof["vpdMin"]       = p.vpdMin;
    prof["vpdMax"]       = p.vpdMax;
    prof["dayTempMin"]   = gp.day.tempMin;
    prof["dayTempMax"]   = gp.day.tempMax;
    prof["nightTempMin"] = gp.night.tempMin;
    prof["nightTempMax"] = gp.night.tempMax;

    // Light schedule state
    const LightSchedule& sched = climate.lightSchedule();
    JsonObject ls   = doc["lightSched"].to<JsonObject>();
    ls["isOn"]         = sched.isOn();
    ls["remSec"]       = sched.remainingSec();
    ls["durSec"]       = sched.phaseTotalSec();
    ls["onHours"]      = gp.lightOnHours;
    ls["offHours"]     = gp.lightOffHours;
    ls["ntpOk"]        = sched.ntpSynced();
    ls["dayStartHour"] = sched.dayStartHour();
    ls["dayStartMin"]  = sched.dayStartMin();

    // Alerts
    uint8_t flags         = climate.alertFlags();
    JsonArray alertArr    = doc["alerts"].to<JsonArray>();
    if (flags & ALERT_NTP_MISSING) {
        JsonObject a = alertArr.add<JsonObject>();
        a["id"]  = ALERT_NTP_MISSING;
        a["msg"] = "NTP not synced — light schedule position uncertain";
    }
    if (flags & ALERT_SCHED_OVERDUE) {
        JsonObject a = alertArr.add<JsonObject>();
        a["id"]  = ALERT_SCHED_OVERDUE;
        a["msg"] = "Light phase overdue — check timer or relay";
    }

    // Relays
    JsonArray arr = doc["relays"].to<JsonArray>();
    for (int i = 0; i < NUM_RELAYS; i++) {
        const RelayState& r = relays.get((RelayIndex)i);
        JsonObject rel      = arr.add<JsonObject>();
        rel["id"]     = i;
        rel["name"]   = r.name;
        rel["mode"]   = (int)r.mode;
        rel["state"]  = r.physicalOn;
        rel["manual"] = r.manualOn;
        rel["buffer"]    = r.autoBuffer;
        rel["minOn"]     = r.minOnSec;
        rel["maxOn"]     = r.maxOnSec;
        rel["maxOnRest"] = r.maxOnRestSec;
        rel["maxOff"]    = r.maxOffSec;
        rel["soilThresh"]= r.soilThreshold;
        rel["waterDur"]  = r.waterDurationSec;
        rel["waterFlow"] = (unsigned int)r.waterFlowML;
        rel["fanIntake"] = r.fanIntake;
        rel["installed"] = r.installed;
        // Timer countdown: remaining seconds in current phase + which phase we are in
        if (r.mode == RELAY_TIMER && r.timerPhaseStart > 0) {
            unsigned long elapsed    = millis() - r.timerPhaseStart;
            unsigned long phaseDurMs = r.timerInOnPhase
                ? (unsigned long)r.timer.onSec  * 1000UL
                : (unsigned long)r.timer.offSec * 1000UL;
            rel["timerRemSec"]  = (elapsed < phaseDurMs)
                ? (uint32_t)((phaseDurMs - elapsed) / 1000UL) : (uint32_t)0;
            rel["timerPhaseOn"] = r.timerInOnPhase;
        } else {
            rel["timerRemSec"]  = 0;
            rel["timerPhaseOn"] = false;
        }
        // Seconds remaining before auto-revert to AUTO (0 if not in manual or no timeout)
        uint32_t manualRemain = 0;
        if (r.mode == RELAY_MANUAL && r.manualTimeoutSec > 0 && r.manualStartMs > 0) {
            unsigned long elapsed   = millis() - r.manualStartMs;
            unsigned long timeoutMs = (unsigned long)r.manualTimeoutSec * 1000UL;
            manualRemain = (elapsed < timeoutMs) ? (uint32_t)((timeoutMs - elapsed) / 1000UL) : 0;
        }
        rel["manualRemain"] = manualRemain;
        // One-shot countdown: seconds remaining before auto-OFF (0 if not active)
        uint32_t onForRemSec = 0;
        if (r.mode == RELAY_MANUAL && r.onForSec > 0 && r.manualOn && r.onForStartMs > 0) {
            unsigned long elapsed    = millis() - r.onForStartMs;
            unsigned long durationMs = (unsigned long)r.onForSec * 1000UL;
            onForRemSec = (elapsed < durationMs) ? (uint32_t)((durationMs - elapsed) / 1000UL) : 0;
        }
        rel["onForRemSec"]   = onForRemSec;
        rel["onForTotalSec"] = (r.mode == RELAY_MANUAL && r.onForSec > 0) ? r.onForSec : (uint32_t)0;
        JsonObject tmr = rel["timer"].to<JsonObject>();
        tmr["on"]  = r.timer.onSec;
        tmr["off"] = r.timer.offSec;
        JsonObject sc      = rel["sched"].to<JsonObject>();
        sc["days"]         = r.schedule.daysMask;
        JsonArray slotArr  = sc["slots"].to<JsonArray>();
        for (int s = 0; s < r.schedule.slotCount && s < MAX_SCHED_SLOTS; s++) {
            JsonObject sl = slotArr.add<JsonObject>();
            sl["sh"] = r.schedule.slots[s].startHour;
            sl["sm"] = r.schedule.slots[s].startMin;
            sl["eh"] = r.schedule.slots[s].endHour;
            sl["em"] = r.schedule.slots[s].endMin;
        }
    }

    // ── Precision Irrigation state ────────────────────────────────────────────
    {
        JsonObject irr = doc["irrigation"].to<JsonObject>();
        const PlantConfig& pc = relays.getPlantConfig();
        irr["precisionEnabled"] = pc.precisionEnabled;
        irr["activeStage"]      = (int)climate.getMode();

        // Active (current stage) profile
        const RelayState& wr = relays.get(WATERING);
        JsonObject ap        = irr["activeProfile"].to<JsonObject>();
        ap["enabled"]        = wr.irrigProfile.enabled;
        ap["trigger"]        = wr.irrigProfile.soilTriggerPct;
        ap["target"]         = wr.irrigProfile.soilTargetPct;
        ap["maxSec"]         = wr.irrigProfile.maxWaterSec;
        ap["dayRest"]        = wr.irrigProfile.minRestDaySec;
        ap["nightRest"]      = wr.irrigProfile.minRestNightSec;
        ap["pulseOn"]        = wr.irrigProfile.pulseOnSec;
        ap["pauseSec"]       = wr.irrigProfile.pauseSec;
        ap["pulsePhase"]     = (uint8_t)wr.pulsePhase;

        // All 4 stored profiles (user edits these)
        JsonArray profiles = irr["profiles"].to<JsonArray>();
        for (int s = 0; s < 4; s++) {
            const IrrigationProfile& p = relays.getIrrigProfile(s);
            JsonObject po = profiles.add<JsonObject>();
            po["stage"]     = s;
            po["enabled"]   = p.enabled;
            po["trigger"]   = p.soilTriggerPct;
            po["target"]    = p.soilTargetPct;
            po["maxSec"]    = p.maxWaterSec;
            po["dayRest"]   = p.minRestDaySec;
            po["nightRest"] = p.minRestNightSec;
            po["pulseOn"]   = p.pulseOnSec;
            po["pauseSec"]  = p.pauseSec;
        }

        // Plant config
        JsonObject plants = irr["plants"].to<JsonObject>();
        plants["count"]     = pc.count;
        plants["substrate"] = pc.substrateType;
        float totalVol = 0.0f;
        JsonArray pots = plants["pots"].to<JsonArray>();
        for (int i = 0; i < pc.count && i < MAX_PLANTS; i++) {
            pots.add(pc.plants[i].potVolumeL);
            totalVol += pc.plants[i].potVolumeL;
        }
        plants["totalVolL"] = totalVol;
        plants["holdCap"]   = SUBSTRATE_HOLD_CAP[pc.substrateType];

        // Probe placement mode
        irr["probeMode"] = relays.probePlacementMode();

        // Runtime stats
        const RelayState& wrSt = relays.get(WATERING);
        JsonObject stats    = irr["stats"].to<JsonObject>();
        stats["peakSoil"]   = relays.getPeakSoilPct();
        stats["dryBack"]    = relays.getDryBackPct();
        stats["lastWaterTs"]  = (long)relays.getLastWaterTs();
        stats["lastWaterDur"] = relays.getLastWaterDur();
        stats["lastWaterML"]  = relays.getLastWaterML();
        stats["todayML"]      = relays.getTodayML();
        stats["adaptiveDurSec"] = wrSt.adaptiveDurSec;
        stats["fixedDurMode"]   = wrSt.fixedDurMode;
        stats["fixedDurSec"]    = wrSt.fixedDurSec;
        stats["stopCount"]      = logger.irrigCount();
    }

    // All grow profiles (for profile editor tab)
    JsonArray allProf = doc["allProfiles"].to<JsonArray>();
    for (int i = 0; i < NUM_GROW_MODES; i++) {
        const GrowProfile& gp = climate.getProfileByMode((GrowMode)i);
        JsonObject po = allProf.add<JsonObject>();
        po["dtMin"] = gp.day.tempMin;    po["dtMax"] = gp.day.tempMax;
        po["dhMin"] = gp.day.humMin;     po["dhMax"] = gp.day.humMax;
        po["dvMin"] = gp.day.vpdMin;     po["dvMax"] = gp.day.vpdMax;
        po["ntMin"] = gp.night.tempMin;  po["ntMax"] = gp.night.tempMax;
        po["nhMin"] = gp.night.humMin;   po["nhMax"] = gp.night.humMax;
        po["nvMin"] = gp.night.vpdMin;   po["nvMax"] = gp.night.vpdMax;
    }

    // IR remote state
    {
        const IRCommand& ir = irRemote.lastCmd();
        JsonObject irObj    = doc["ir"].to<JsonObject>();
        irObj["proto"]  = (uint8_t)irRemote.protocol();
        irObj["power"]  = ir.power;
        irObj["temp"]   = ir.temp;
        irObj["mode"]   = ir.mode;
        irObj["fan"]    = ir.fan;
    }

    serializeJson(doc, buf, bufSize);
}

// ─── WebSocket event handler ───────────────────────────────────────────────────
static void onWsEvent(AsyncWebSocket*       server,
                      AsyncWebSocketClient* client,
                      AwsEventType          type,
                      void*                 arg,
                      uint8_t*              data,
                      size_t                len)
{
    if (type == WS_EVT_CONNECT) {
        // Push full state immediately so new browsers don't wait for the next broadcast tick
        if (xSemaphoreTake(_jsonMux, pdMS_TO_TICKS(200)) == pdTRUE) {
            buildStateJson(_stateJsonBuf, sizeof(_stateJsonBuf));
            client->text(_stateJsonBuf, strlen(_stateJsonBuf));
            xSemaphoreGive(_jsonMux);
        }
        return;
    }
    if (type != WS_EVT_DATA) return;

    AwsFrameInfo* info = (AwsFrameInfo*)arg;
    // Only process complete, unfragmented text frames; reject oversized payloads
    if (!info->final || info->index != 0 || info->len != len) return;
    if (info->opcode != WS_TEXT) return;
    if (len > 4096) { client->close(); return; }

    // Parse without modifying the library-owned buffer (avoids out-of-bounds write).
    // static: allocated once to avoid heap churn on every WS message.
    // deserializeJson() clears doc before parsing, so stale content is never an issue.
    static JsonDocument doc;
    if (deserializeJson(doc, (const char*)data, len) != DeserializationError::Ok) return;

    const char* msgType = doc["type"] | "";

    if (strcmp(msgType, "setVpdTarget") == 0) {
        bool  en  = doc["enabled"] | false;
        float kpa = doc["kpa"]     | 1.0f;
        float buf = doc["buffer"]  | 0.1f;
        climate.setVpdTarget(en, kpa, buf);
    } else if (strcmp(msgType, "setAcTemps") == 0) {
        float    low   = doc["low"]   | 0.0f;
        float    high  = doc["high"]  | 0.0f;
        bool     night = doc["night"] | false;
        climate.setAcTemps(low, high, night);
        if (doc.containsKey("delay")) {
            uint32_t delay = doc["delay"] | (uint32_t)600;
            climate.setAcHumDelay(delay);
        }
    } else if (strcmp(msgType, "setProfile") == 0) {
        uint8_t mode = (uint8_t)(doc["mode"] | 0xFF);
        if (mode < NUM_GROW_MODES) {
            const GrowProfile& cur = climate.getProfileByMode((GrowMode)mode);
            DayNightRange day   = cur.day;
            DayNightRange night = cur.night;
            day.tempMin  = doc["dtMin"] | cur.day.tempMin;
            day.tempMax  = doc["dtMax"] | cur.day.tempMax;
            day.humMin   = doc["dhMin"] | cur.day.humMin;
            day.humMax   = doc["dhMax"] | cur.day.humMax;
            day.vpdMin   = doc["dvMin"] | cur.day.vpdMin;
            day.vpdMax   = doc["dvMax"] | cur.day.vpdMax;
            night.tempMin = doc["ntMin"] | cur.night.tempMin;
            night.tempMax = doc["ntMax"] | cur.night.tempMax;
            night.humMin  = doc["nhMin"] | cur.night.humMin;
            night.humMax  = doc["nhMax"] | cur.night.humMax;
            night.vpdMin  = doc["nvMin"] | cur.night.vpdMin;
            night.vpdMax  = doc["nvMax"] | cur.night.vpdMax;
            climate.setProfile((GrowMode)mode, day, night);
        }
    } else if (strcmp(msgType, "resetProfile") == 0) {
        uint8_t mode = (uint8_t)(doc["mode"] | 0xFF);
        if (mode < NUM_GROW_MODES) {
            climate.resetProfile((GrowMode)mode);
        }
    } else if (strcmp(msgType, "setProbePlacement") == 0) {
        relays.setProbePlacement(doc["active"] | false);
    } else if (strcmp(msgType, "setMode") == 0) {
        int m = doc["mode"] | -1;
        if (m >= 0 && m < NUM_GROW_MODES) {
            climate.setMode((GrowMode)m);
            static const char* stageNames[] = {
                "Seedling","Veg","Early Bloom","Late Bloom","Drying"
            };
            char evtBuf[48];
            snprintf(evtBuf, sizeof(evtBuf), "%s (user)", stageNames[m]);
            eventlog("STAGE", evtBuf);
        }
    } else if (strcmp(msgType, "setStageDay") == 0) {
        int d = doc["day"] | 0;
        if (d >= 1) { climate.setStageDay((uint32_t)d); }
        char evtBuf[32]; snprintf(evtBuf, sizeof(evtBuf), "day corrected to %d", d);
        eventlog("STAGE-DAY", evtBuf);
    } else if (strcmp(msgType, "relay") == 0) {
        int         id     = doc["id"]     | -1;
        const char* action = doc["action"] | "";
        if (id < 0 || id >= NUM_RELAYS) return;

        RelayIndex idx = (RelayIndex)id;

        if (strcmp(action, "mode") == 0) {
            int v = doc["value"] | 0;
            relays.setMode(idx, (RelayMode)v);
        } else if (strcmp(action, "manual") == 0) {
            bool v = doc["value"] | false;
            relays.setManual(idx, v);
        } else if (strcmp(action, "timer") == 0) {
            uint32_t onSec  = doc["on"]  | 3600;
            uint32_t offSec = doc["off"] | 1800;
            relays.setTimer(idx, onSec, offSec);
        } else if (strcmp(action, "buffer") == 0) {
            float buf = doc["value"] | 0.05f;
            relays.setBuffer(idx, buf);
        } else if (strcmp(action, "duration") == 0) {
            uint32_t minOn      = doc["minOn"]     | (uint32_t)30;
            uint32_t maxOn      = doc["maxOn"]     | (uint32_t)0;
            uint32_t maxOnRest  = doc["maxOnRest"] | (uint32_t)0;
            relays.setDuration(idx, minOn, maxOn, maxOnRest);
        } else if (strcmp(action, "maxOff") == 0) {
            uint32_t sec = doc["value"] | (uint32_t)0;
            relays.setMaxOff(idx, sec);
        } else if (strcmp(action, "fanIntake") == 0) {
            bool v = doc["value"] | false;
            relays.setFanIntake(idx, v);
        } else if (strcmp(action, "onFor") == 0) {
            uint32_t seconds = doc["value"] | (uint32_t)0;
            if (seconds > 0) relays.setOnFor(idx, seconds);
        } else if (strcmp(action, "soilWater") == 0) {
            uint8_t  threshold   = (uint8_t)(doc["threshold"] | 0);
            uint32_t durationSec = doc["duration"]  | (uint32_t)300;
            relays.setSoilWater(idx, threshold, durationSec);
        } else if (strcmp(action, "waterFlow") == 0) {
            uint32_t mlPerMin = doc["value"] | (uint32_t)500;
            relays.setWaterFlow(idx, mlPerMin);
        } else if (strcmp(action, "installed") == 0) {
            bool v = doc["value"] | true;
            relays.setInstalled(idx, v);
        } else if (strcmp(action, "waterDurMode") == 0) {
            bool     fixed = doc["fixed"] | false;
            uint32_t sec   = doc["sec"]   | (uint32_t)60;
            relays.setWaterDurMode(fixed, sec);
        } else if (strcmp(action, "schedule") == 0) {
            ScheduleCfg cfg;
            cfg.daysMask  = doc["days"] | 0x7F;
            cfg.slotCount = 0;
            for (JsonObjectConst sl : doc["slots"].as<JsonArrayConst>()) {
                if (cfg.slotCount >= MAX_SCHED_SLOTS) break;
                int s = cfg.slotCount;
                cfg.slots[s].startHour = sl["sh"] | 8;
                cfg.slots[s].startMin  = sl["sm"] | 0;
                cfg.slots[s].endHour   = sl["eh"] | 9;
                cfg.slots[s].endMin    = sl["em"] | 0;
                cfg.slotCount++;
            }
            if (cfg.slotCount == 0) cfg.slotCount = 1;
            relays.setSchedule(idx, cfg);
        }
    } else if (strcmp(msgType, "setLightStart") == 0) {
        uint8_t hour = (uint8_t)(doc["hour"] | 0xFF);
        uint8_t min  = (uint8_t)(doc["min"]  | 0);
        climate.setDayStart(hour, min);
    } else if (strcmp(msgType, "setDryingFast") == 0) {
        climate.setDryingFast(doc["fast"] | false);
    } else if (strcmp(msgType, "setIrrigProfile") == 0) {
        uint8_t stage = (uint8_t)(doc["stage"] | 0);
        if (stage < 4) {
            IrrigationProfile p;
            p.enabled           = doc["enabled"]   | true;
            p.soilTriggerPct    = (uint8_t)(doc["trigger"]   | 50);
            p.soilTargetPct     = (uint8_t)(doc["target"]    | 70);
            p.maxWaterSec       = doc["maxSec"]    | (uint32_t)180;
            p.minRestDaySec     = doc["dayRest"]   | (uint32_t)1800;
            p.minRestNightSec   = doc["nightRest"] | (uint32_t)3600;
            p.pulseOnSec        = doc["pulseOn"]   | (uint32_t)20;
            p.pauseSec          = doc["pauseSec"]  | (uint32_t)45;
            relays.setIrrigProfile(stage, p);
        }
    } else if (strcmp(msgType, "resetIrrigDefaults") == 0) {
        relays.resetIrrigDefaults();
    } else if (strcmp(msgType, "setHeatPulse") == 0) {
        uint32_t onSec   = doc["onSec"]   | (uint32_t)45;
        uint32_t restSec = doc["restSec"] | (uint32_t)420;
        float    target  = doc["target"]  | 0.0f;
        climate.setHeatPulse(onSec, restSec, target);
    } else if (strcmp(msgType, "irSend") == 0) {
        IRCommand cmd;
        cmd.power = doc["power"] | false;
        cmd.temp  = (uint8_t)constrain(doc["temp"] | 24, 16, 30);
        cmd.mode  = (uint8_t)constrain(doc["mode"] | 0,  0,  4);
        cmd.fan   = (uint8_t)constrain(doc["fan"]  | 0,  0,  3);
        irRemote.sendCommand(cmd);
    } else if (strcmp(msgType, "irSetProto") == 0) {
        uint8_t p = (uint8_t)(doc["proto"] | 0);
        if (p < (uint8_t)IRProto::NUM_PROTO) irRemote.setProtocol((IRProto)p);
    } else if (strcmp(msgType, "restart") == 0) {
        climate.flushPrefsIfDirty();
        climate.flushProfilePrefsIfDirty();
        relays.flushPrefsIfDirty();
        wifiSensors.flushPrefsIfDirty();
        soil.flushCalibIfDirty();
        irRemote.flushPrefsIfDirty();
        delay(200);
        ESP.restart();
    } else if (strcmp(msgType, "soilCalib") == 0) {
        const char* point = doc["point"] | "";
        int raw = soil.rawAdc();
        if (raw < 100) {
            // No valid reading yet — ignore
        } else if (strcmp(point, "dry") == 0) {
            soil.setCalib(raw, soil.adcWet());
        } else if (strcmp(point, "wet") == 0) {
            soil.setCalib(soil.adcDry(), raw);
        }
    } else if (strcmp(msgType, "setPlantConfig") == 0) {
        PlantConfig pc;
        pc.precisionEnabled = doc["enabled"]   | false;
        pc.count            = (uint8_t)(doc["count"] | 1);
        pc.substrateType    = (uint8_t)(doc["substrate"] | 1);
        if (pc.count < 1 || pc.count > MAX_PLANTS) pc.count = 1;
        for (uint8_t i = 0; i < pc.count && i < MAX_PLANTS; i++) {
            pc.plants[i].potVolumeL = doc["pots"][i] | (uint16_t)15;
        }
        relays.setPlantConfig(pc);
    } else if (strcmp(msgType, "addSensor") == 0) {
        const char* name      = doc["name"]      | "";
        const char* sensorUrl = doc["sensorUrl"] | "";
        bool urlOk = sensorUrl[0] && strncmp(sensorUrl, "http://", 7) == 0;
        if (name[0] && urlOk)
            wifiSensors.add(name, sensorUrl);
    } else if (strcmp(msgType, "removeSensor") == 0) {
        int id = doc["id"] | -1;
        if (id >= 0) wifiSensors.remove(id);
    } else if (strcmp(msgType, "toggleSensor") == 0) {
        int  id = doc["id"]      | -1;
        bool en = doc["enabled"] | false;
        if (id >= 0) wifiSensors.setEnabled(id, en);
    } else if (strcmp(msgType, "setSensorActive") == 0) {
        int  id     = doc["id"]     | -1;
        bool active = doc["active"] | true;
        if (id >= 0) wifiSensors.setSensorActive(id, active);
    } else if (strcmp(msgType, "clearLogs") == 0) {
        // Deferred to Core 1 (webBroadcast) — these touch flash/NVS. `what`
        // selects the target box; omitted/"all" clears everything.
        const char* what = doc["what"] | "all";
        if      (strcmp(what, "sys")   == 0) _clearPending |= CLEAR_SYS;
        else if (strcmp(what, "event") == 0) _clearPending |= CLEAR_EVENT;
        else if (strcmp(what, "crash") == 0) _clearPending |= CLEAR_CRASH;
        else                                 _clearPending |= (CLEAR_SYS | CLEAR_EVENT | CLEAR_CRASH);
    }

    // Echo updated state back to the requesting client only.
    // The 2-second webBroadcast() loop notifies all other clients — calling
    // textAll() here from the async-TCP callback context races with that loop
    // and can corrupt the WebSocket queue → PANIC-CRASH.
    if (xSemaphoreTake(_jsonMux, pdMS_TO_TICKS(200)) == pdTRUE) {
        buildStateJson(_stateJsonBuf, sizeof(_stateJsonBuf));
        client->text(_stateJsonBuf, strlen(_stateJsonBuf));
        xSemaphoreGive(_jsonMux);
    }
}

// ─── Log API handler ──────────────────────────────────────────────────────────
static char logBuf[LOG_RESPONSE_BUF];

// Stream a NUL-terminated static buffer to the client in small chunks instead of
// copying it into a heap String. The String copy of a full 24 KB /api/logs reply
// is a large, variable-sized, contiguous allocation — fired on every UI open — and
// after a day of uptime the fragmented heap can't satisfy it (or AsyncTCP's own
// buffers right after), which is the PANIC-on-open. The filler copies ≤maxLen
// bytes per TCP ack, so no large allocation is ever needed.
//
// IMPORTANT: `src` must stay valid and unchanged for the whole async send. Each
// streamed endpoint therefore owns a dedicated static buffer that no other handler
// touches; the only residual race is two concurrent requests to the *same*
// endpoint, which at worst garbles one reply (the client refetches) — never a crash.
static AsyncWebServerResponse* streamStatic(AsyncWebServerRequest* req,
                                            const char* src, size_t len) {
    return req->beginResponse("application/json", len,
        [src, len](uint8_t* buffer, size_t maxLen, size_t index) -> size_t {
            size_t remaining = len - index;
            size_t chunk     = remaining < maxLen ? remaining : maxLen;
            if (chunk) memcpy(buffer, src + index, chunk);
            return chunk;
        });
}

static void handleLogRequest(AsyncWebServerRequest* req) {
    int hours = 24;
    if (req->hasParam("hours")) {
        hours = req->getParam("hours")->value().toInt();
        hours = constrain(hours, 1, 168);
    }
    long since = 0;
    if (req->hasParam("since")) {
        since = req->getParam("since")->value().toInt();
    }
    int step = 1;
    if (req->hasParam("step")) {
        step = req->getParam("step")->value().toInt();
        step = constrain(step, 1, 60);
    }
    int len = logger.getJsonLast(hours, logBuf, sizeof(logBuf), since, step);

    AsyncWebServerResponse* resp = streamStatic(req, logBuf, (size_t)len);
    resp->addHeader("Cache-Control", "no-cache");
    req->send(resp);
}

// ─── Public init ──────────────────────────────────────────────────────────────
void webBegin() {
    _jsonMux = xSemaphoreCreateMutex();  // serialises buildStateJson() across tasks
    ws.setAuthentication(WEB_AUTH_USER, WEB_AUTH_PASS);
    ws.onEvent(onWsEvent);
    server.addHandler(&ws);

    // API routes (must be registered before static file handler)
    server.on("/api/logs",  HTTP_GET,  [](AsyncWebServerRequest* req) {
        if (!checkAuth(req)) return;
        handleLogRequest(req);
    });

    server.on("/api/state", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (!checkAuth(req)) return;
        if (xSemaphoreTake(_jsonMux, pdMS_TO_TICKS(300)) == pdTRUE) {
            buildStateJson(_stateJsonBuf, sizeof(_stateJsonBuf));
            req->send(200, "application/json", _stateJsonBuf);
            xSemaphoreGive(_jsonMux);
        } else {
            req->send(503, "text/plain", "Busy");
        }
    });

    // ── OTA update page (firmware + UI filesystem) ───────────────────────────
    static const char OTA_PAGE[] =
        "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Guetos Systems \xe2\x80\x94 OTA Update</title>"
        "<style>"
        "body{margin:0;font-family:system-ui;background:#111827;color:#f1f5f9;"
        "display:flex;align-items:center;justify-content:center;min-height:100vh}"
        ".box{background:#1e293b;border-radius:16px;padding:2rem;width:340px;"
        "box-shadow:0 8px 32px rgba(0,0,0,.5)}"
        "h2{margin:0 0 .4rem;font-size:1.15rem;color:#94a3b8;text-align:center}"
        "h2 span{color:#4ade80}"
        ".card{background:#0f172a;border-radius:10px;padding:1.1rem;margin-top:1.2rem}"
        ".card-title{font-size:.78rem;font-weight:700;color:#94a3b8;text-transform:uppercase;"
        "letter-spacing:.07em;margin-bottom:.75rem}"
        "input[type=file]{width:100%;padding:.5rem;background:#1e293b;border:1px solid #334155;"
        "border-radius:7px;color:#f1f5f9;margin-bottom:.75rem;box-sizing:border-box;font-size:.85rem}"
        "button{width:100%;padding:.65rem;border:none;border-radius:7px;font-size:.95rem;"
        "font-weight:600;cursor:pointer;color:#fff}"
        ".btn-fw{background:#16a34a}.btn-fw:hover{background:#15803d}"
        ".btn-ui{background:#0369a1}.btn-ui:hover{background:#0284c7}"
        ".progress{width:100%;background:#1e293b;border-radius:8px;height:7px;margin-top:.7rem;display:none}"
        ".progress-bar{height:7px;border-radius:8px;width:0%;transition:width .25s}"
        ".bar-fw{background:#4ade80}.bar-ui{background:#38bdf8}"
        ".status{margin-top:.6rem;font-size:.82rem;color:#94a3b8;min-height:1.1em;text-align:center}"
        "</style></head><body>"
        "<div class='box'>"
        "<h2>\xf0\x9f\x8c\xbf <span>Guetos</span> Systems</h2>"
        // ── Firmware card ──
        "<div class='card'>"
        "<div class='card-title'>\xf0\x9f\x94\xa7 Firmware (.bin)</div>"
        "<form id='fwForm'>"
        "<input type='file' id='fwFile' accept='.bin' required>"
        "<button type='submit' class='btn-fw'>Flash Firmware</button>"
        "</form>"
        "<div class='progress' id='fwProg'><div class='progress-bar bar-fw' id='fwBar'></div></div>"
        "<div class='status' id='fwStatus'></div>"
        "</div>"
        // ── UI (filesystem) card ──
        "<div class='card'>"
        "<div class='card-title'>\xf0\x9f\x96\xa5 UI Files (.bin)</div>"
        "<form id='uiForm'>"
        "<input type='file' id='uiFile' accept='.bin' required>"
        "<button type='submit' class='btn-ui'>Flash UI</button>"
        "</form>"
        "<div class='progress' id='uiProg'><div class='progress-bar bar-ui' id='uiBar'></div></div>"
        "<div class='status' id='uiStatus'></div>"
        "</div>"
        "</div>"
        "<script>"
        "function upload(formId,fileId,url,progId,barId,statusId){"
        "document.getElementById(formId).onsubmit=function(e){"
        "e.preventDefault();"
        "const f=document.getElementById(fileId).files[0];if(!f)return;"
        "const fd=new FormData();fd.append('file',f);"
        "const prog=document.getElementById(progId);"
        "const bar=document.getElementById(barId);"
        "const st=document.getElementById(statusId);"
        "prog.style.display='block';st.textContent='Uploading\xe2\x80\xa6';"
        "const xhr=new XMLHttpRequest();"
        "xhr.upload.onprogress=function(e){if(e.lengthComputable){"
        "const p=Math.round(e.loaded/e.total*100);"
        "bar.style.width=p+'%';st.textContent='Uploading '+p+'%';}};"
        "xhr.onload=function(){if(xhr.status===200&&xhr.responseText==='OK'){"
        "bar.style.width='100%';st.textContent='Done! Rebooting\xe2\x80\xa6';"
        "}else{st.textContent='FAILED: '+(xhr.responseText||xhr.status);bar.style.background='#ef4444';}};"
        "xhr.onerror=function(){st.textContent='Upload failed.';};"
        "xhr.open('POST',url);xhr.send(fd);}}"
        "upload('fwForm','fwFile','/update','fwProg','fwBar','fwStatus');"
        // UI uses raw binary POST so onBody handler knows exact size
        "document.getElementById('uiForm').onsubmit=function(e){"
        "e.preventDefault();"
        "const f=document.getElementById('uiFile').files[0];if(!f)return;"
        "const prog=document.getElementById('uiProg');"
        "const bar=document.getElementById('uiBar');"
        "const st=document.getElementById('uiStatus');"
        "prog.style.display='block';st.textContent='Uploading\xe2\x80\xa6';"
        "const xhr=new XMLHttpRequest();"
        "xhr.upload.onprogress=function(e){if(e.lengthComputable){"
        "const p=Math.round(e.loaded/e.total*100);"
        "bar.style.width=p+'%';st.textContent='Uploading '+p+'%';}};"
        "xhr.onload=function(){if(xhr.status===200&&xhr.responseText==='OK'){"
        "bar.style.width='100%';st.textContent='Done! Rebooting\xe2\x80\xa6';"
        "}else{st.textContent='FAILED: '+(xhr.responseText||xhr.status);bar.style.background='#ef4444';}};"
        "xhr.onerror=function(){st.textContent='Upload failed.';};"
        "xhr.open('POST','/update/ui');"
        "xhr.setRequestHeader('Content-Type','application/octet-stream');"
        "xhr.send(f);};"
        "</script></body></html>";

    server.on("/update", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (!checkAuth(req)) return;
        req->send(200, "text/html", OTA_PAGE);
    });

    // ── Firmware OTA (U_FLASH) ────────────────────────────────────────────────
    server.on("/update", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            if (!checkAuth(req)) return;
            bool ok = !Update.hasError();
            AsyncWebServerResponse* resp = req->beginResponse(200, "text/plain", ok ? "OK" : "FAIL");
            resp->addHeader("Connection", "close");
            req->send(resp);
            if (ok) { delay(300); ESP.restart(); }
        },
        [](AsyncWebServerRequest* req, String filename, size_t index, uint8_t* data, size_t len, bool final) {
            if (index == 0 && !checkAuth(req)) return;
            if (!index) {
                Serial.printf("[OTA-FW] Start: %s\n", filename.c_str());
                if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH))
                    Update.printError(Serial);
            }
            if (Update.write(data, len) != len) Update.printError(Serial);
            if (final) {
                if (Update.end(true)) Serial.printf("[OTA-FW] OK: %u bytes\n", index + len);
                else                  Update.printError(Serial);
            }
        }
    );

    // ── WiFi sensor list ──────────────────────────────────────────────────────
    server.on("/api/sensors", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (!checkAuth(req)) return;
        static char sensBuf[WIFI_SENSOR_MAX * 220 + 8];
        wifiSensors.getJson(sensBuf, sizeof(sensBuf));
        AsyncWebServerResponse* resp = req->beginResponse(200, "application/json", sensBuf);
        resp->addHeader("Cache-Control", "no-cache");
        req->send(resp);
    });

    // ── Remote system log ─────────────────────────────────────────────────────
    server.on("/api/syslog", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (!checkAuth(req)) return;
        static char syslogBuf[SYSLOG_LINES * (SYSLOG_LINE_LEN + 4) + 256];
        int len = syslogGetJson(syslogBuf, sizeof(syslogBuf));
        AsyncWebServerResponse* resp = streamStatic(req, syslogBuf, (size_t)len);
        resp->addHeader("Cache-Control", "no-cache");
        req->send(resp);
    });

    // ── Crash history (7-day ring buffer) ────────────────────────────────────
    server.on("/api/crashes", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (!checkAuth(req)) return;
        static char crashBuf[512];
        crashlogGetJson(crashBuf, sizeof(crashBuf));
        AsyncWebServerResponse* resp = req->beginResponse(200, "application/json", crashBuf);
        resp->addHeader("Cache-Control", "no-cache");
        req->send(resp);
    });

    // ── Persistent event log ─────────────────────────────────────────────────
    server.on("/api/events", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (!checkAuth(req)) return;
        // Dedicated buffer (not logBuf): the streamed response keeps reading its
        // source after the handler returns, so it must not share with /api/logs.
        static char eventBuf[10000];   // 60 events × ~130 B JSON + margin
        int len = eventlogGetJson(eventBuf, sizeof(eventBuf));
        AsyncWebServerResponse* resp = streamStatic(req, eventBuf, (size_t)len);
        resp->addHeader("Cache-Control", "no-cache");
        req->send(resp);
    });

    // ── Settings backup ───────────────────────────────────────────────────────
    server.on("/api/backup", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (!checkAuth(req)) return;
        static JsonDocument bdoc;
        bdoc.clear();
        bdoc["version"] = 1;
        bdoc["ts"]      = (long)time(nullptr);
        bdoc["growMode"]   = (int)climate.getMode();
        bdoc["stageDay"]   = climate.stageDay();
        bdoc["dryingFast"] = climate.isDryingFast();

        const VpdTargetCfg& vtc = climate.vpdTarget();
        JsonObject vt   = bdoc["vpdTarget"].to<JsonObject>();
        vt["enabled"]   = vtc.enabled; vt["kpa"] = vtc.kpa; vt["buffer"] = vtc.buffer;

        JsonObject ac   = bdoc["acTemps"].to<JsonObject>();
        ac["dayLow"]    = climate.acDayLow();    ac["dayHigh"]   = climate.acDayHigh();
        ac["nightLow"]  = climate.acNightLow();  ac["nightHigh"] = climate.acNightHigh();
        ac["delay"]     = climate.acHumDelaySec();

        JsonObject hp   = bdoc["heatPulse"].to<JsonObject>();
        hp["onSec"]     = climate.heatPulseOnSec(); hp["restSec"] = climate.heatPulseRestSec();
        hp["target"]    = climate.heatTarget();

        const LightSchedule& sched = climate.lightSchedule();
        JsonObject ls   = bdoc["lightStart"].to<JsonObject>();
        ls["hour"]      = sched.dayStartHour(); ls["min"] = sched.dayStartMin();

        JsonArray profiles = bdoc["profiles"].to<JsonArray>();
        for (int i = 0; i < NUM_GROW_MODES; i++) {
            const GrowProfile& gp = climate.getProfileByMode((GrowMode)i);
            JsonObject p = profiles.add<JsonObject>();
            p["dtMin"] = gp.day.tempMin;   p["dtMax"] = gp.day.tempMax;
            p["dhMin"] = gp.day.humMin;    p["dhMax"] = gp.day.humMax;
            p["dvMin"] = gp.day.vpdMin;    p["dvMax"] = gp.day.vpdMax;
            p["ntMin"] = gp.night.tempMin; p["ntMax"] = gp.night.tempMax;
            p["nhMin"] = gp.night.humMin;  p["nhMax"] = gp.night.humMax;
            p["nvMin"] = gp.night.vpdMin;  p["nvMax"] = gp.night.vpdMax;
            p["lOn"]   = gp.lightOnHours;  p["lOff"]  = gp.lightOffHours;
        }

        JsonArray relayArr = bdoc["relays"].to<JsonArray>();
        for (int i = 0; i < NUM_RELAYS; i++) {
            const RelayState& r = relays.get((RelayIndex)i);
            JsonObject rel = relayArr.add<JsonObject>();
            rel["id"]      = i;
            rel["mode"]    = (int)r.mode;
            rel["minOn"]   = r.minOnSec;    rel["maxOn"]      = r.maxOnSec;
            rel["maxOnRest"]= r.maxOnRestSec; rel["maxOff"]   = r.maxOffSec;
            rel["buffer"]  = r.autoBuffer;  rel["installed"]  = r.installed;
            rel["fanIntake"]= r.fanIntake;
            rel["waterFlow"]= (unsigned int)r.waterFlowML;
            JsonObject tmr = rel["timer"].to<JsonObject>();
            tmr["on"] = r.timer.onSec; tmr["off"] = r.timer.offSec;
            JsonObject sc  = rel["sched"].to<JsonObject>();
            sc["days"]     = r.schedule.daysMask;
            JsonArray slots = sc["slots"].to<JsonArray>();
            for (int s = 0; s < r.schedule.slotCount && s < MAX_SCHED_SLOTS; s++) {
                JsonObject sl = slots.add<JsonObject>();
                sl["sh"] = r.schedule.slots[s].startHour; sl["sm"] = r.schedule.slots[s].startMin;
                sl["eh"] = r.schedule.slots[s].endHour;   sl["em"] = r.schedule.slots[s].endMin;
            }
        }

        JsonArray irrigArr = bdoc["irrigProfiles"].to<JsonArray>();
        for (int i = 0; i < 4; i++) {
            const IrrigationProfile& ip = relays.getIrrigProfile(i);
            JsonObject po = irrigArr.add<JsonObject>();
            po["stage"]   = i; po["enabled"]    = ip.enabled;
            po["trigger"] = ip.soilTriggerPct;  po["target"] = ip.soilTargetPct;
            po["maxSec"]  = ip.maxWaterSec;     po["dayRest"]    = ip.minRestDaySec;
            po["nightRest"]= ip.minRestNightSec; po["pulseOn"]  = ip.pulseOnSec;
            po["pauseSec"]= ip.pauseSec;
        }

        const PlantConfig& pc = relays.getPlantConfig();
        JsonObject plants  = bdoc["plantConfig"].to<JsonObject>();
        plants["count"]    = pc.count; plants["substrate"] = pc.substrateType;
        plants["precision"]= pc.precisionEnabled;
        JsonArray pots     = plants["pots"].to<JsonArray>();
        for (int i = 0; i < pc.count && i < MAX_PLANTS; i++) pots.add(pc.plants[i].potVolumeL);

        JsonObject soilCal = bdoc["soilCalib"].to<JsonObject>();
        soilCal["dry"]     = soil.adcDry();
        soilCal["wet"]     = soil.adcWet();

        // WiFi sensors — reuse existing _wsSensBuf (already BSS, same file scope)
        wifiSensors.getJson(_wsSensBuf, sizeof(_wsSensBuf));
        bdoc["wifiSensors"] = serialized(_wsSensBuf);

        // Serialize into logBuf — async_tcp is single-threaded, safe to reuse
        serializeJson(bdoc, logBuf, sizeof(logBuf));
        AsyncWebServerResponse* resp = req->beginResponse(200, "application/json", logBuf);
        resp->addHeader("Content-Disposition", "attachment; filename=\"vpd-backup.json\"");
        resp->addHeader("Cache-Control", "no-cache");
        req->send(resp);
    });

    // ── Settings restore (upload backup JSON) ────────────────────────────────
    // Accepts the backup JSON as POST body; applies all settings and flushes NVS.
    server.on("/api/restore", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            if (!checkAuth(req)) return;
            req->send(200, "text/plain", "OK — rebooting to apply");
            delay(300);
            ESP.restart();
        },
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
            if (!checkAuth(req)) return;
            // Use logBuf to accumulate body (backup JSON ~4-6KB, logBuf is 24KB)
            static size_t restoreLen = 0;
            if (index == 0) restoreLen = 0;
            if (restoreLen + len < sizeof(logBuf) - 1) {
                memcpy(logBuf + restoreLen, data, len);
                restoreLen += len;
            }
            if (index + len < total) return;  // not done yet
            logBuf[restoreLen] = '\0';

            static JsonDocument rdoc;
            if (deserializeJson(rdoc, logBuf, restoreLen) != DeserializationError::Ok) {
                req->send(400, "text/plain", "Invalid JSON");
                return;
            }

            // Apply profiles
            if (rdoc["profiles"].is<JsonArray>()) {
                for (int i = 0; i < NUM_GROW_MODES && i < (int)rdoc["profiles"].size(); i++) {
                    JsonObjectConst p = rdoc["profiles"][i];
                    const GrowProfile& cur = climate.getProfileByMode((GrowMode)i);
                    DayNightRange day = cur.day, night = cur.night;
                    day.tempMin = p["dtMin"] | cur.day.tempMin;  day.tempMax = p["dtMax"] | cur.day.tempMax;
                    day.humMin  = p["dhMin"] | cur.day.humMin;   day.humMax  = p["dhMax"] | cur.day.humMax;
                    day.vpdMin  = p["dvMin"] | cur.day.vpdMin;   day.vpdMax  = p["dvMax"] | cur.day.vpdMax;
                    night.tempMin = p["ntMin"] | cur.night.tempMin; night.tempMax = p["ntMax"] | cur.night.tempMax;
                    night.humMin  = p["nhMin"] | cur.night.humMin;  night.humMax  = p["nhMax"] | cur.night.humMax;
                    night.vpdMin  = p["nvMin"] | cur.night.vpdMin;  night.vpdMax  = p["nvMax"] | cur.night.vpdMax;
                    climate.setProfile((GrowMode)i, day, night);
                }
            }

            // Apply VPD target
            if (rdoc["vpdTarget"].is<JsonObject>())
                climate.setVpdTarget(rdoc["vpdTarget"]["enabled"] | false,
                                     rdoc["vpdTarget"]["kpa"]     | 1.0f,
                                     rdoc["vpdTarget"]["buffer"]  | 0.1f);

            // Apply A/C temps
            if (rdoc["acTemps"].is<JsonObject>()) {
                climate.setAcTemps(rdoc["acTemps"]["dayLow"]   | 0.0f,
                                   rdoc["acTemps"]["dayHigh"]  | 0.0f, false);
                climate.setAcTemps(rdoc["acTemps"]["nightLow"] | 0.0f,
                                   rdoc["acTemps"]["nightHigh"]| 0.0f, true);
                climate.setAcHumDelay(rdoc["acTemps"]["delay"] | (uint32_t)600);
            }

            // Apply heat pulse
            if (rdoc["heatPulse"].is<JsonObject>())
                climate.setHeatPulse(rdoc["heatPulse"]["onSec"]   | (uint32_t)45,
                                     rdoc["heatPulse"]["restSec"] | (uint32_t)420,
                                     rdoc["heatPulse"]["target"]  | 0.0f);

            // Apply light schedule start
            if (rdoc["lightStart"].is<JsonObject>())
                climate.setDayStart(rdoc["lightStart"]["hour"] | (uint8_t)255,
                                    rdoc["lightStart"]["min"]  | (uint8_t)0);

            // Apply irrigation profiles
            if (rdoc["irrigProfiles"].is<JsonArray>()) {
                for (JsonObjectConst ip : rdoc["irrigProfiles"].as<JsonArrayConst>()) {
                    uint8_t stage = ip["stage"] | (uint8_t)0;
                    if (stage >= 4) continue;
                    IrrigationProfile p;
                    p.enabled          = ip["enabled"]   | true;
                    p.soilTriggerPct   = ip["trigger"]   | (uint8_t)50;
                    p.soilTargetPct    = ip["target"]    | (uint8_t)70;
                    p.maxWaterSec      = ip["maxSec"]    | (uint32_t)180;
                    p.minRestDaySec    = ip["dayRest"]   | (uint32_t)1800;
                    p.minRestNightSec  = ip["nightRest"] | (uint32_t)3600;
                    p.pulseOnSec       = ip["pulseOn"]   | (uint32_t)20;
                    p.pauseSec         = ip["pauseSec"]  | (uint32_t)45;
                    relays.setIrrigProfile(stage, p);
                }
            }

            // Apply plant config
            if (rdoc["plantConfig"].is<JsonObject>()) {
                PlantConfig pc;
                pc.precisionEnabled = rdoc["plantConfig"]["precision"] | false;
                pc.count            = rdoc["plantConfig"]["count"]     | (uint8_t)1;
                pc.substrateType    = rdoc["plantConfig"]["substrate"] | (uint8_t)1;
                if (pc.count < 1 || pc.count > MAX_PLANTS) pc.count = 1;
                for (uint8_t i = 0; i < pc.count && i < MAX_PLANTS; i++)
                    pc.plants[i].potVolumeL = rdoc["plantConfig"]["pots"][i] | (uint16_t)15;
                relays.setPlantConfig(pc);
            }

            // Apply soil calibration
            if (rdoc["soilCalib"].is<JsonObject>())
                soil.setCalib(rdoc["soilCalib"]["dry"] | soil.adcDry(),
                              rdoc["soilCalib"]["wet"] | soil.adcWet());

            // Flush all to NVS
            climate.flushPrefsIfDirty();
            climate.flushProfilePrefsIfDirty();
            relays.flushPrefsIfDirty();
            soil.flushCalibIfDirty();
            eventlog("RESTORE", "settings restored from backup");
        }
    );

    // ── Irrigation event history ──────────────────────────────────────────────
    server.on("/api/irrigation", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (!checkAuth(req)) return;
        static char irrigBuf[2500];  // 25 entries * ~90 bytes — ring-buffered in getIrrigationJson
        logger.getIrrigationJson(irrigBuf, sizeof(irrigBuf));
        AsyncWebServerResponse* resp = req->beginResponse(200, "application/json", irrigBuf);
        resp->addHeader("Cache-Control", "no-cache");
        req->send(resp);
    });

    // Serve index.html (embedded gzip) for all non-API paths
    auto serveUI = [](AsyncWebServerRequest* req) {
        if (!checkAuth(req)) return;
        AsyncWebServerResponse* resp = req->beginResponse(
            200, "text/html", UI_HTML_GZ, UI_HTML_GZ_LEN);
        resp->addHeader("Content-Encoding", "gzip");
        resp->addHeader("Cache-Control", "no-cache");
        req->send(resp);
    };
    server.on("/",           HTTP_GET, serveUI);
    server.on("/index.html", HTTP_GET, serveUI);

    server.onNotFound([](AsyncWebServerRequest* req) {
        if (!checkAuth(req)) return;
        req->send(404, "text/plain", "Not found");
    });

    server.begin();
    Serial.println("[WEB] Server started on port 80");
}

// ─── Broadcast (called from main loop) ────────────────────────────────────────
void webBroadcast() {
    // Process pending log-clear requests here (Core 1 / loop context) — the WS
    // handler only sets the bits so the flash/NVS writes never race the writers.
    if (_clearPending) {
        uint8_t what = _clearPending;
        _clearPending = 0;
        if (what & CLEAR_EVENT) eventlogClear();
        if (what & CLEAR_CRASH) crashlogClear();
        // Clear the in-RAM syslog last so the confirmation line below survives.
        if (what & CLEAR_SYS)   syslogClear();
        rlog("[LOG] Cleared by user:%s%s%s",
             (what & CLEAR_SYS)   ? " sys"   : "",
             (what & CLEAR_EVENT) ? " event" : "",
             (what & CLEAR_CRASH) ? " crash" : "");
    }

    if (!ws.count()) return;
    ws.cleanupClients();

    // Short timeout: if the async_tcp task is mid-build, skip this tick.
    // The next broadcast 2 s later will succeed — no point blocking loop().
    if (xSemaphoreTake(_jsonMux, pdMS_TO_TICKS(50)) != pdTRUE) return;
    buildStateJson(_stateJsonBuf, sizeof(_stateJsonBuf));
    ws.textAll(_stateJsonBuf, strlen(_stateJsonBuf));
    xSemaphoreGive(_jsonMux);
}
