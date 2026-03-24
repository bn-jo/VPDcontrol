#include "webserver.h"
#include "config.h"
#include "sensors.h"
#include "soil.h"
#include "relays.h"
#include "climate.h"
#include "datalogger.h"

#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <LittleFS.h>

static AsyncWebServer server(80);
static AsyncWebSocket ws("/ws");

// ─── State JSON builder ────────────────────────────────────────────────────────
static void buildStateJson(String& out) {
    JsonDocument doc;
    doc["type"] = "state";

    // Soil moisture
    const SoilData& sd_soil = soil.data();
    doc["soil"]      = sd_soil.moisture;
    doc["soilValid"] = sd_soil.valid;

    const SensorData& sd = sensors.data();
    doc["temp"]  = sd.temperature;
    doc["hum"]   = sd.humidity;
    doc["vpd"]   = sd.vpd;
    doc["valid"] = sd.valid;

    doc["growMode"] = (int)climate.getMode();

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
    ls["isOn"]      = sched.isOn();
    ls["remSec"]    = sched.remainingSec();
    ls["durSec"]    = sched.phaseTotalSec();
    ls["onHours"]   = gp.lightOnHours;
    ls["offHours"]  = gp.lightOffHours;
    ls["ntpOk"]     = sched.ntpSynced();

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

    if (strcmp(msgType, "setMode") == 0) {
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
