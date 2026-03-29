#include "webserver.h"
#include "config.h"
#include "sensors.h"
#include "soil.h"
#include "relays.h"
#include "climate.h"
#include "datalogger.h"
#include "autotune.h"
#include "remotesensor.h"

#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <WiFi.h>

static AsyncWebServer server(80);
static AsyncWebSocket ws("/ws");

// ─── State JSON builder ────────────────────────────────────────────────────────
static void buildStateJson(String& out) {
    JsonDocument doc;
    doc["type"] = "state";
    doc["rssi"] = WiFi.RSSI();

    // Soil moisture
    const SoilData& sd_soil = soil.data();
    doc["soil"]      = sd_soil.moisture;
    doc["soilValid"] = sd_soil.valid;

    const SensorData&       sd  = sensors.data();
    const RemoteSensorData& rsd = remoteSensor.data();

    // If the remote node is reachable, average its readings with the local sensor
    float dispTemp, dispHum, dispVpd;
    if (rsd.valid) {
        dispTemp = (sd.temperature + rsd.temperature) / 2.0f;
        dispHum  = (sd.humidity    + rsd.humidity)    / 2.0f;
        dispVpd  = SensorManager::calcVPD(dispTemp, dispHum);
    } else {
        dispTemp = sd.temperature;
        dispHum  = sd.humidity;
        dispVpd  = sd.vpd;
    }

    doc["temp"]            = dispTemp;
    doc["hum"]             = dispHum;
    doc["vpd"]             = dispVpd;
    doc["vpdTrend"]        = sd.vpdTrend;   // kPa/min — positive = drying, negative = humidifying
    doc["valid"]           = sd.valid;
    doc["remoteConnected"] = rsd.valid;

    doc["growMode"] = (int)climate.getMode();
    doc["stageDay"] = climate.stageDay();   // Day 1 = first day of current stage

    const VpdTargetCfg& vt = climate.vpdTarget();
    JsonObject vtObj        = doc["vpdTarget"].to<JsonObject>();
    vtObj["enabled"]        = vt.enabled;
    vtObj["kpa"]            = vt.kpa;
    vtObj["buffer"]         = vt.buffer;

    // Active profile targets — correct day/night set
    const bool         lightsOn = climate.isLightsOn();
    const GrowProfile& gp       = climate.getProfile();
    const DayNightRange& p      = lightsOn ? gp.day : gp.night;

    JsonObject prof  = doc["profile"].to<JsonObject>();
    prof["name"]     = gp.name;
    prof["isDay"]    = lightsOn;
    prof["tempMin"]  = p.tempMin;
    prof["tempMax"]  = p.tempMax;
    prof["humMin"]   = p.humMin;
    prof["humMax"]   = p.humMax;
    prof["vpdMin"]   = p.vpdMin;
    prof["vpdMax"]   = p.vpdMax;

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
        rel["soilThresh"]= r.soilThreshold;
        rel["waterDur"]  = r.waterDurationSec;
        rel["fanIntake"] = r.fanIntake;
        // Seconds remaining before auto-revert to AUTO (0 if not in manual or no timeout)
        uint32_t manualRemain = 0;
        if (r.mode == RELAY_MANUAL && r.manualTimeoutSec > 0 && r.manualStartMs > 0) {
            unsigned long elapsed   = millis() - r.manualStartMs;
            unsigned long timeoutMs = (unsigned long)r.manualTimeoutSec * 1000UL;
            manualRemain = (elapsed < timeoutMs) ? (uint32_t)((timeoutMs - elapsed) / 1000UL) : 0;
        }
        rel["manualRemain"] = manualRemain;
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

    // Auto-Tune status
    const ATStatus& at = autoTuner.status();
    JsonObject atObj        = doc["autoTune"].to<JsonObject>();
    atObj["phase"]          = (int)at.phase;
    atObj["relayId"]        = at.relayId;
    atObj["relayName"]      = at.relayName ? at.relayName : "";
    atObj["stepDone"]       = at.stepDone;
    atObj["stepTotal"]      = at.stepTotal;
    atObj["phaseRemMs"]     = at.phaseRemMs;
    atObj["phaseTotMs"]     = at.phaseTotMs;
    atObj["abortSafety"]    = at.abortSafety;
    JsonArray atResults     = atObj["results"].to<JsonArray>();
    for (int i = 0; i < at.resultCount; i++) {
        const ATResult& r = at.results[i];
        JsonObject ro     = atResults.add<JsonObject>();
        ro["id"]          = r.relayId;
        ro["name"]        = relays.get((RelayIndex)r.relayId).name;
        ro["base"]        = r.baseVal;
        ro["on"]          = r.onVal;
        ro["delta"]       = r.delta;
        ro["buf"]         = r.bufApplied;
    }

    serializeJson(doc, out);
}

// ─── WebSocket event handler ───────────────────────────────────────────────────
static void onWsEvent(AsyncWebSocket*       server,
                      AsyncWebSocketClient* client,
                      AwsEventType          type,
                      void*                 arg,
                      uint8_t*              data,
                      size_t                len)
{
    if (type != WS_EVT_DATA) return;

    AwsFrameInfo* info = (AwsFrameInfo*)arg;
    // Only process complete, unfragmented text frames
    if (!info->final || info->index != 0 || info->len != len) return;
    if (info->opcode != WS_TEXT) return;

    // Parse without modifying the library-owned buffer (avoids out-of-bounds write)
    JsonDocument doc;
    if (deserializeJson(doc, (const char*)data, len) != DeserializationError::Ok) return;

    const char* msgType = doc["type"] | "";

    if (strcmp(msgType, "setVpdTarget") == 0) {
        bool  en  = doc["enabled"] | false;
        float kpa = doc["kpa"]     | 1.0f;
        float buf = doc["buffer"]  | 0.1f;
        climate.setVpdTarget(en, kpa, buf);
    } else if (strcmp(msgType, "setMode") == 0) {
        int m = doc["mode"] | -1;
        if (m >= 0 && m < NUM_GROW_MODES) {
            climate.setMode((GrowMode)m);
        }
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
            uint32_t minOn = doc["minOn"] | (uint32_t)30;
            uint32_t maxOn = doc["maxOn"] | (uint32_t)0;
            relays.setDuration(idx, minOn, maxOn);
        } else if (strcmp(action, "fanIntake") == 0) {
            bool v = doc["value"] | false;
            relays.setFanIntake(idx, v);
        } else if (strcmp(action, "soilWater") == 0) {
            uint8_t  threshold   = (uint8_t)(doc["threshold"] | 0);
            uint32_t durationSec = doc["duration"]  | (uint32_t)300;
            relays.setSoilWater(idx, threshold, durationSec);
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
    } else if (strcmp(msgType, "autoTune") == 0) {
        const char* action = doc["action"] | "";
        if (strcmp(action, "start") == 0) {
            autoTuner.requestStart();
        } else if (strcmp(action, "cancel") == 0) {
            autoTuner.requestCancel();
        }
    }

    // Echo updated state back to all clients
    String out;
    buildStateJson(out);
    server->textAll(out);
}

// ─── Log API handler ──────────────────────────────────────────────────────────
static char logBuf[LOG_RESPONSE_BUF];

static void handleLogRequest(AsyncWebServerRequest* req) {
    int hours = 24;
    if (req->hasParam("hours")) {
        hours = req->getParam("hours")->value().toInt();
        hours = constrain(hours, 1, 168);
    }
    logger.getJsonLast(hours, logBuf, sizeof(logBuf));

    AsyncWebServerResponse* resp = req->beginResponse(200, "application/json", logBuf);
    resp->addHeader("Cache-Control", "no-cache");
    req->send(resp);
}

// ─── Public init ──────────────────────────────────────────────────────────────
void webBegin() {
    ws.onEvent(onWsEvent);
    server.addHandler(&ws);

    // API routes (must be registered before static file handler)
    server.on("/api/logs",  HTTP_GET,  handleLogRequest);

    server.on("/api/state", HTTP_GET, [](AsyncWebServerRequest* req) {
        String out;
        buildStateJson(out);
        req->send(200, "application/json", out);
    });

    // Serve everything else from LittleFS (index.html as default)
    server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

    server.onNotFound([](AsyncWebServerRequest* req) {
        req->send(404, "text/plain", "Not found");
    });

    server.begin();
    Serial.println("[WEB] Server started on port 80");
}

// ─── Broadcast (called from main loop) ────────────────────────────────────────
void webBroadcast() {
    if (!ws.count()) return;
    ws.cleanupClients();

    String out;
    buildStateJson(out);
    ws.textAll(out);
}
