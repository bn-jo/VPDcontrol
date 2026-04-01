#include "webserver.h"
#include "ui_html.h"
#include "config.h"
#include "sensors.h"
#include "soil.h"
#include "relays.h"
#include "climate.h"
#include "datalogger.h"
#include "autotune.h"
#include "remotesensor.h"
#include "intakesensor.h"

#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <Update.h>

static AsyncWebServer server(80);
static AsyncWebSocket ws("/ws");

// ─── Auth helper ──────────────────────────────────────────────────────────────
// Always require HTTP Basic Auth — credentials defined in config.h.
static bool checkAuth(AsyncWebServerRequest* req) {
    if (req->authenticate(WEB_AUTH_USER, WEB_AUTH_PASS)) return true;
    req->requestAuthentication();
    return false;
}

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

    doc["growMode"]   = (int)climate.getMode();
    doc["dryingFast"] = climate.isDryingFast();
    doc["stageDay"] = climate.stageDay();   // Day 1 = first day of current stage

    const VpdTargetCfg& vt = climate.vpdTarget();
    JsonObject vtObj        = doc["vpdTarget"].to<JsonObject>();
    vtObj["enabled"]        = vt.enabled;
    vtObj["kpa"]            = vt.kpa;
    vtObj["buffer"]         = vt.buffer;

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
        rel["maxOnRest"] = r.maxOnRestSec;
        rel["soilThresh"]= r.soilThreshold;
        rel["waterDur"]  = r.waterDurationSec;
        rel["waterFlow"] = (unsigned int)r.waterFlowML;
        rel["fanIntake"] = r.fanIntake;
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
            uint32_t minOn      = doc["minOn"]     | (uint32_t)30;
            uint32_t maxOn      = doc["maxOn"]     | (uint32_t)0;
            uint32_t maxOnRest  = doc["maxOnRest"] | (uint32_t)0;
            relays.setDuration(idx, minOn, maxOn, maxOnRest);
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
    } else if (strcmp(msgType, "autoTune") == 0) {
        const char* action = doc["action"] | "";
        if (strcmp(action, "start") == 0) {
            uint8_t mask = 0;
            for (JsonVariantConst v : doc["relays"].as<JsonArrayConst>()) {
                uint8_t rid = v.as<uint8_t>();
                if (rid < 8) mask |= (1u << rid);
            }
            if (mask == 0) mask = 0xFF;  // fallback: test all
            autoTuner.requestStart(mask);
        } else if (strcmp(action, "cancel") == 0) {
            autoTuner.requestCancel();
        } else if (strcmp(action, "reset") == 0) {
            autoTuner.requestReset();
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
    server.on("/api/logs",  HTTP_GET,  [](AsyncWebServerRequest* req) {
        if (!checkAuth(req)) return;
        handleLogRequest(req);
    });

    server.on("/api/state", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (!checkAuth(req)) return;
        String out;
        buildStateJson(out);
        req->send(200, "application/json", out);
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

    // ── UI filesystem OTA (raw binary POST — avoids multipart size ambiguity) ──
    server.on("/update/ui", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            if (!checkAuth(req)) return;
            bool ok = !Update.hasError();
            AsyncWebServerResponse* resp = req->beginResponse(200, "text/plain", ok ? "OK" : "FAIL");
            resp->addHeader("Connection", "close");
            req->send(resp);
            if (ok) { delay(300); ESP.restart(); }
        },
        nullptr,  // no multipart upload handler
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
            if (index == 0) {
                Serial.printf("[OTA-UI] Start: %u bytes\n", (unsigned)total);
                LittleFS.end();
                if (!Update.begin(total > 0 ? total : UPDATE_SIZE_UNKNOWN, U_SPIFFS))
                    Update.printError(Serial);
            }
            if (!Update.hasError()) {
                if (Update.write(data, len) != len) Update.printError(Serial);
            }
            if (index + len >= total) {
                if (!Update.hasError()) {
                    if (Update.end(true)) Serial.printf("[OTA-UI] OK: %u bytes\n", (unsigned)total);
                    else                  Update.printError(Serial);
                }
            }
        }
    );

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
