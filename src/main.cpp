#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <HTTPClient.h>
#include <esp_sntp.h>       // NTP
#include <esp_system.h>     // esp_reset_reason()
#include <esp_task_wdt.h>   // task watchdog
#include <nvs.h>            // nvs_get_stats()
#include <time.h>

#include "config.h"
#include "sensors.h"
#include "soil.h"
#include "relays.h"
#include "climate.h"
#include "datalogger.h"
#include "webserver.h"
#include "wifisensors.h"
#include "intakesensor.h"
#include "syslog.h"
#include "crashlog.h"
#include "eventlog.h"
#include "irremote.h"

static TaskHandle_t _controlTaskHandle = nullptr;

// ─── Core 0 → Core 1 pending writes ──────────────────────────────────────────
// All LittleFS and HTTP writes happen on Core 1 to keep the control loop pure.
// Each slot is single-producer (Core 0) / single-consumer (Core 1).
// Pattern: Core 0 writes payload then sets flag (with barrier); Core 1 clears
// flag first (with barrier), then reads payload — same as _irrigPushPending.

// Pending irrigation event (LittleFS log + HTTP remote push)
static volatile bool _irrigPushPending = false;
static IrrigEvent    _pendingIrrig;

// Pending sensor log (LittleFS write every LOG_INTERVAL_MS)
static volatile bool _sensorLogPending = false;
static SensorData    _pendingLogSd;
static float         _pendingLogSoil   = -1.0f;

// ─── FreeRTOS task: sensor reading + climate control ─────────────────────────
// Runs on Core 0 to leave Core 1 free for WiFi + web server
static void controlTask(void* pvParam) {
    // Register with the task watchdog — auto-restarts if this task hangs > 30 s
    esp_task_wdt_add(NULL);

    unsigned long lastSensorMs  = 0;
    unsigned long lastControlMs = 0;
    unsigned long lastLogMs     = 0;

    for (;;) {
        esp_task_wdt_reset();  // pet the watchdog every 100 ms tick
        unsigned long now = millis();

        // ── Sensor read ───────────────────────────────────────────────────────
        if (now - lastSensorMs >= SENSOR_INTERVAL_MS) {
            if (!sensors.read()) Serial.println("[SNS] Read failed — using stale data");
            lastSensorMs = now;
        }
        intakeSensor.update();  // Self-throttles at INTAKE_SENSOR_INTERVAL_MS
        soil.update();   // Self-throttles at SOIL_INTERVAL_MS
        relays.setSoilMoisture(soil.data().moisture, soil.data().valid);
        relays.setLightsOn(climate.isLightsOn());

        // ── Climate control ───────────────────────────────────────────────────
        // Blend remote WiFi sensor averages with local DHT22 for climate decisions.
        // wifiSensors.getAverage() only includes sensors with sensorActive=true.
        if (now - lastControlMs >= CONTROL_INTERVAL_MS) {
            SensorData blended = sensors.data();
            float remT, remH;
            if (wifiSensors.getAverage(remT, remH) && blended.valid) {
                blended.temperature = (blended.temperature + remT) / 2.0f;
                blended.humidity    = (blended.humidity    + remH) / 2.0f;
                blended.vpd         = SensorManager::calcVPD(blended.temperature, blended.humidity);
            }
            climate.update(blended);
            lastControlMs = now;
        }

        // ── Relay state machine (timers + hysteresis guards) ──────────────────
        relays.update();

        // ── Log completed irrigation events ───────────────────────────────────
        // LittleFS write and HTTP push both happen on Core 1 (loop()) to keep
        // Core 0 free of I/O. Payload is copied here; flag is set last (barrier).
        IrrigEvent iev;
        if (relays.popIrrigEvent(iev)) {
            _pendingIrrig = iev;
            __sync_synchronize();
            _irrigPushPending = true;
        }

        // ── Data logging ──────────────────────────────────────────────────────
        // Sensor data is copied to a pending slot; Core 1's loop() does the
        // actual LittleFS write so the control task stays off flash I/O entirely.
        if (now - lastLogMs >= LOG_INTERVAL_MS) {
            if (sensors.data().valid) {
                _pendingLogSoil = soil.data().valid ? soil.data().moisture : -1.0f;
                _pendingLogSd   = sensors.data();
                __sync_synchronize();
                _sensorLogPending = true;
            }
            lastLogMs = now;
        }

        vTaskDelay(pdMS_TO_TICKS(100));  // 100 ms tick
    }
}

// ─── WiFi: scan all APs matching our SSID, return channel of the strongest ────
// Fills bssid[6] with the AP MAC. Returns 0 if none found.
static int scanBestBSSID(uint8_t* bssid) {
    Serial.printf("[WIFI] Scanning for best AP on \"%s\"...\n", WIFI_SSID);
    int n = WiFi.scanNetworks(/*async*/false, /*hidden*/false, /*passive*/false, /*ms_per_ch*/200);
    if (n <= 0) { Serial.println("[WIFI] No networks found"); return 0; }

    int bestRssi = -999, bestIdx = -1;
    for (int i = 0; i < n; i++) {
        if (WiFi.SSID(i).equals(WIFI_SSID) && WiFi.RSSI(i) > bestRssi) {
            bestRssi = WiFi.RSSI(i);
            bestIdx  = i;
        }
    }
    if (bestIdx < 0) { WiFi.scanDelete(); Serial.println("[WIFI] SSID not found"); return 0; }

    memcpy(bssid, WiFi.BSSID(bestIdx), 6);
    int ch = WiFi.channel(bestIdx);
    Serial.printf("[WIFI] Best AP: %02X:%02X:%02X:%02X:%02X:%02X  ch=%d  %d dBm\n",
                  bssid[0],bssid[1],bssid[2],bssid[3],bssid[4],bssid[5], ch, bestRssi);
    WiFi.scanDelete();
    return ch;
}

// ─── WiFi connect (blocking until connected or timeout) ───────────────────────
static bool wifiConnect() {
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);   // handles brief dropouts automatically
    WiFi.persistent(false);

#ifdef STATIC_IP
    WiFi.config(
        IPAddress(STATIC_IP),
        IPAddress(STATIC_GATEWAY),
        IPAddress(STATIC_SUBNET),
        IPAddress(STATIC_DNS)
    );
#endif

    uint8_t bssid[6] = {};
    int ch = scanBestBSSID(bssid);
    if (ch > 0) {
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD, ch, bssid);
    } else {
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);  // fallback: let driver pick
    }

    Serial.printf("[WIFI] Connecting");
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - start > WIFI_TIMEOUT_MS) {
            Serial.println("\n[WIFI] Timeout!");
            return false;
        }
        esp_task_wdt_reset();  // wifiConnect() blocks up to WIFI_TIMEOUT_MS — pet WDT
        delay(500);
        Serial.print('.');
    }
    Serial.printf("\n[WIFI] Connected — IP: %s  RSSI: %d dBm\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
    return true;
}

// ─── Setup ────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n=== VPD Control System ===");

    // ── Task watchdog ─────────────────────────────────────────────────────────
    // 30 s timeout — if controlTask or loop() freezes, auto-restart instead of
    // hanging indefinitely. controlTask registers itself; loop() is registered below.
    esp_task_wdt_init(30, true);   // 30 s, panic (= restart) on timeout
    esp_task_wdt_add(NULL);        // add setup()/loop() (Core 1) to WDT now

    // ── Remote log + crash diagnostics ───────────────────────────────────────
    syslogBegin();   // load previous crash info from NVS

    // Capture reset reason early (before LittleFS) so it can be logged later
    static const char* _bootReason = "UNKNOWN";
    {
        esp_reset_reason_t rr = esp_reset_reason();
        switch (rr) {
            case ESP_RST_POWERON:   _bootReason = "POWER-ON";          break;
            case ESP_RST_SW:        _bootReason = "SOFTWARE-RESTART";  break;
            case ESP_RST_PANIC:     _bootReason = "PANIC-CRASH";       break;
            case ESP_RST_INT_WDT:   _bootReason = "INT-WATCHDOG";      break;
            case ESP_RST_TASK_WDT:  _bootReason = "TASK-WATCHDOG";     break;
            case ESP_RST_WDT:       _bootReason = "WDT-OTHER";         break;
            case ESP_RST_BROWNOUT:  _bootReason = "BROWNOUT";          break;
            default: break;
        }
        rlog("[BOOT] Reset reason: %s", _bootReason);
        syslogSaveCrashInfo(_bootReason);
        crashlogBegin(rr);
    }

    // Hardware init
    sensors.begin();
    intakeSensor.begin();
    soil.begin();
    relays.begin();
    // Log NVS usage after relay init (relay migration clears old keys — this shows the result)
    {
        nvs_stats_t ns;
        if (nvs_get_stats(NULL, &ns) == ESP_OK)
            rlog("[NVS] used=%u free=%u total=%u namespaces=%u",
                 ns.used_entries, ns.free_entries, ns.total_entries, ns.namespace_count);
    }
    logger.begin();   // mounts LittleFS — must come before climate.begin()
    eventlogBegin();  // count existing events (LittleFS already mounted)
    eventlog("BOOT", _bootReason);  // persist this boot's reason across future reboots
    climate.begin();
    relays.setIrrigMode((uint8_t)climate.getMode());  // sync initial stage profile
    wifiSensors.begin();  // load NVS now — WiFi not required for NVS read
    irRemote.begin();     // start IR task on Core 1 (dedicated 16 KB stack)

    // WiFi
    if (wifiConnect()) {
        rlog("[WIFI] Connected — IP: %s", WiFi.localIP().toString().c_str());
        // NTP time sync (non-blocking after initial handshake)
        configTime(NTP_GMT_OFFSET_SEC, NTP_DST_OFFSET_SEC, NTP_SERVER);
        rlog("[NTP] Sync requested");

        // Start web server only when WiFi is up
        webBegin();

        // OTA firmware update
        ArduinoOTA.setHostname("vpd-control");
        ArduinoOTA.setPassword(OTA_PASSWORD);
        ArduinoOTA.onStart([]() {
            // Flush any pending NVS writes before the reboot so stage/prefs survive
            climate.flushPrefsIfDirty();
            climate.flushProfilePrefsIfDirty();
            relays.flushPrefsIfDirty();
            wifiSensors.flushPrefsIfDirty();
            soil.flushCalibIfDirty();
            irRemote.flushPrefsIfDirty();
            rlog("[OTA] Start — prefs flushed");
        });
        ArduinoOTA.onEnd([]()    { rlog("[OTA] Done — rebooting"); });
        ArduinoOTA.onError([](ota_error_t e) { rlog("[OTA] Error %u", e); });
        ArduinoOTA.begin();
        rlog("[OTA] Ready on port 3232");
    } else {
        rlog("[WARN] Running without WiFi — web UI unavailable");
    }

    // Start control task on Core 0
    xTaskCreatePinnedToCore(
        controlTask,
        "ControlTask",
        16384,      // Stack size (bytes) — was 8192; enlarged after NVS calls were moved to update()
        nullptr,
        3,          // Priority (higher = more urgent)
        &_controlTaskHandle,
        0           // Core 0
    );

    Serial.println("[MAIN] Setup complete");
}

// ─── Main loop (Core 1) ───────────────────────────────────────────────────────
// Handles WebSocket broadcast + keeps async server running
void loop() {
    static unsigned long lastWsPushMs   = 0;
    static unsigned long lastWifiRetry  = 0;
    static unsigned long lastHeapLogMs  = 0;
    static unsigned long lastRoamMs     = 0;
    static bool          roamScanActive = false;
    static bool          dailyRestartDone  = false;  // one restart per 01:20 window
    static bool          noonRestartDone   = false;  // one restart per 13:20 window
    static bool          wifiWasUp         = false;  // track WiFi for event logging
    static bool          lightsWasOn       = false;  // track light state for event logging
    static GrowMode      lastGrowMode      = (GrowMode)255; // track stage for event logging

    // ── Deferred NVS flush — runs first, before any early return or restart ──────
    relays.flushPrefsIfDirty();
    climate.flushPrefsIfDirty();
    climate.flushProfilePrefsIfDirty();
    wifiSensors.flushPrefsIfDirty();
    soil.flushCalibIfDirty();
    irRemote.flushPrefsIfDirty();

    // ── Stage auto-transition (Core 1) ────────────────────────────────────────
    // Runs here, NOT in controlTask, so it is serialised with setMode() on Core 1
    // and cannot race with user-initiated stage changes.
    climate.checkAutoTransition();

    // ── WiFi reconnect guard ──────────────────────────────────────────────────
    // setAutoReconnect handles brief dropouts; this catches longer outages where
    // the driver gives up (e.g. router reboot at night).
    if (WiFi.status() != WL_CONNECTED) {
        roamScanActive = false;   // cancel any pending roam scan
        if (wifiWasUp) {
            wifiWasUp = false;
            eventlog("WIFI", "disconnected");
        }
        unsigned long now = millis();
        if (now - lastWifiRetry >= 30000UL) {
            rlog("[WIFI] Lost — reconnecting...");
            WiFi.disconnect(false, false);
            uint8_t bssid[6] = {};
            int ch = scanBestBSSID(bssid);
            if (ch > 0) WiFi.begin(WIFI_SSID, WIFI_PASSWORD, ch, bssid);
            else        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
            lastWifiRetry = now;
        }
        esp_task_wdt_reset();  // pet WDT — WiFi loss must never trigger a crash loop
        yield();
        return;   // skip WS push and remote fetch while offline
    }
    if (!wifiWasUp) {
        wifiWasUp = true;
        char wfBuf[40];
        snprintf(wfBuf, sizeof(wfBuf), "connected %s", WiFi.localIP().toString().c_str());
        eventlog("WIFI", wfBuf);
    }

    // ── Roaming: periodically check for a stronger AP ─────────────────────────
    {
        unsigned long now = millis();
        if (!roamScanActive && now - lastRoamMs >= WIFI_ROAM_INTERVAL_MS) {
            lastRoamMs = now;
            if (WiFi.RSSI() < WIFI_ROAM_RSSI_MIN) {
                rlog("[WIFI] Weak signal (%d dBm) — scanning for better AP", WiFi.RSSI());
                WiFi.scanNetworks(/*async*/true);
                roamScanActive = true;
            }
        }
        if (roamScanActive) {
            int n = WiFi.scanComplete();
            if (n >= 0) {
                roamScanActive = false;
                int bestRssi = -999, bestIdx = -1;
                for (int i = 0; i < n; i++) {
                    if (WiFi.SSID(i).equals(WIFI_SSID) && WiFi.RSSI(i) > bestRssi) {
                        bestRssi = WiFi.RSSI(i);
                        bestIdx  = i;
                    }
                }
                if (bestIdx >= 0 && bestRssi >= WiFi.RSSI() + WIFI_ROAM_MIN_GAIN_DB) {
                    uint8_t bssid[6];
                    memcpy(bssid, WiFi.BSSID(bestIdx), 6);
                    int ch = WiFi.channel(bestIdx);
                    WiFi.scanDelete();
                    rlog("[WIFI] Roaming to better AP: %d dBm  ch=%d", bestRssi, ch);
                    WiFi.disconnect(false, false);
                    WiFi.begin(WIFI_SSID, WIFI_PASSWORD, ch, bssid);
                } else {
                    WiFi.scanDelete();
                }
            }
        }
    }

    ArduinoOTA.handle();

    // ── WebSocket broadcast ───────────────────────────────────────────────────
    // Runs BEFORE wifiSensors.update() so HTTP polling never delays the broadcast.
    if (millis() - lastWsPushMs >= WS_PUSH_INTERVAL_MS) {
        webBroadcast();
        lastWsPushMs = millis();
    }

    // ── WiFi sensor polling (blocking HTTP GET — up to WIFI_SENSOR_TIMEOUT each)
    wifiSensors.update();

    // ── Fix crash epoch once NTP syncs ───────────────────────────────────────
    {
        static bool epochFixed = false;
        if (!epochFixed && time(nullptr) > 1000000000L) {
            epochFixed = true;
            syslogFixEpoch();
            crashlogOnNtpSync();
        }
    }

    // ── Predictive A/C hot-hours window (Core 1: reads logs.csv) ──────────────
    // Recompute shortly after NTP sync, then every AC_WINDOW_RECALC_MS. Kept on
    // Core 1 so the full-file scan never touches the control loop on Core 0.
    {
        static unsigned long lastAcWinMs = 0;
        if (time(nullptr) > 1000000000L &&
            (lastAcWinMs == 0 || millis() - lastAcWinMs >= AC_WINDOW_RECALC_MS)) {
            lastAcWinMs = millis();
            climate.recomputeAcWindow();
        }
    }

    // ── Track light state + grow stage changes → event log ────────────────────
    {
        bool lightsNow = climate.isLightsOn();
        if (lightsNow != lightsWasOn) {
            lightsWasOn = lightsNow;
            eventlog("LIGHT", lightsNow ? "ON" : "OFF");
        }
        GrowMode modeNow = climate.getMode();
        if (modeNow != lastGrowMode) {
            lastGrowMode = modeNow;
            if (lastGrowMode != (GrowMode)255) {  // skip the initial sentinel value
                static const char* stageNames[] = {
                    "Seedling","Veg","Early Bloom","Late Bloom","Drying"
                };
                char detail[48];
                snprintf(detail, sizeof(detail), "%s day=%lu",
                         stageNames[(int)modeNow < 5 ? (int)modeNow : 0],
                         (unsigned long)climate.stageDay());
                eventlog("STAGE", detail);
            }
        }
    }

    // ── Proactive safe restarts (01:20 AM and 13:20 PM) ─────────────────────
    // Two restarts per day caps the maximum uptime at ~12 h, keeping heap
    // fragmentation and memory drift from accumulating into a panic.
    {
        time_t nowT = time(nullptr);
        if (nowT > 1000000000L) {
            struct tm lt;
            localtime_r(&nowT, &lt);
            bool inNightWindow = (lt.tm_hour == 1  && lt.tm_min == 20);
            bool inNoonWindow  = (lt.tm_hour == 13 && lt.tm_min == 20);
            // Don't restart while the A/C is running (a reboot would cut cooling),
            // and not until the device has been up long enough — otherwise a reboot
            // that lands back inside the same minute window restarts again, looping.
            bool acRunning = relays.get(DEHUMIDIFIER).physicalOn;
            bool uptimeOk  = millis() > PROACTIVE_RESTART_MIN_UPTIME_MS;
            bool mayRestart = uptimeOk && !acRunning;
            if (inNightWindow && !dailyRestartDone && mayRestart) {
                dailyRestartDone = true;
                climate.flushPrefsIfDirty();
                climate.flushProfilePrefsIfDirty();
                relays.flushPrefsIfDirty();
                wifiSensors.flushPrefsIfDirty();
                soil.flushCalibIfDirty();
                irRemote.flushPrefsIfDirty();
                rlog("[MAIN] Safe restart at 01:20");
                eventlog("RST", "safe-01:20");
                syslogSaveCrashInfo("SOFTWARE-RESTART");
                delay(200);
                ESP.restart();
            }
            if (inNoonWindow && !noonRestartDone && mayRestart) {
                noonRestartDone = true;
                climate.flushPrefsIfDirty();
                climate.flushProfilePrefsIfDirty();
                relays.flushPrefsIfDirty();
                wifiSensors.flushPrefsIfDirty();
                soil.flushCalibIfDirty();
                irRemote.flushPrefsIfDirty();
                rlog("[MAIN] Safe restart at 13:20");
                eventlog("RST", "safe-13:20");
                syslogSaveCrashInfo("SOFTWARE-RESTART");
                delay(200);
                ESP.restart();
            }
            if (!inNightWindow) dailyRestartDone = false;
            if (!inNoonWindow)  noonRestartDone  = false;
        }
    }

    // ── Deferred LittleFS writes + remote push (all Core 1) ──────────────────
    // Core 0 copies data into pending slots and sets flags; Core 1 drains them
    // here. This keeps the control task (Core 0) completely off flash I/O.
    {
        // ── Sensor log ────────────────────────────────────────────────────────
        if (_sensorLogPending) {
            // Copy payload before clearing the flag so Core 0 can re-arm immediately.
            SensorData logSd   = _pendingLogSd;
            float      logSoil = _pendingLogSoil;
            __sync_synchronize();
            _sensorLogPending = false;

            // LittleFS write — safe on Core 1 (HTTP handlers also Core 1)
            if (logSd.valid) logger.log(logSd, logSoil);

            // Remote push — same snapshot as LittleFS entry so they always match
            if (REMOTE_LOG_URL[0]) {
                char body[100];
                int n;
                if (logSoil >= 0.0f)
                    n = snprintf(body, sizeof(body),
                        "{\"t\":%ld,\"T\":%.1f,\"H\":%.1f,\"V\":%.3f,\"S\":%.1f}",
                        (long)logSd.timestamp, logSd.temperature, logSd.humidity,
                        logSd.vpd, logSoil);
                else
                    n = snprintf(body, sizeof(body),
                        "{\"t\":%ld,\"T\":%.1f,\"H\":%.1f,\"V\":%.3f}",
                        (long)logSd.timestamp, logSd.temperature, logSd.humidity,
                        logSd.vpd);
                (void)n;
                esp_task_wdt_reset();
                HTTPClient http;
                http.begin(REMOTE_LOG_URL "/vpd/log");
                http.setConnectTimeout(REMOTE_LOG_TIMEOUT);
                http.setTimeout(REMOTE_LOG_TIMEOUT);
                http.addHeader("Content-Type", "application/json");
                http.POST(body);
                http.end();
            }
        }

        // ── Irrigation event ──────────────────────────────────────────────────
        // Clear flag FIRST so Core 0 can arm a new event while we process this one.
        if (_irrigPushPending) {
            IrrigEvent ie = _pendingIrrig;
            __sync_synchronize();
            _irrigPushPending = false;

            // LittleFS write — safe on Core 1
            logger.logIrrigation(ie.ts, ie.soilBefore, ie.soilAfter,
                                  ie.durationSec, ie.volumeML, ie.src);

            // Remote push
            if (REMOTE_LOG_URL[0]) {
                char body[128];
                snprintf(body, sizeof(body),
                    "{\"t\":%ld,\"b\":%.1f,\"a\":%.1f,\"d\":%lu,\"ml\":%lu,\"src\":%u}",
                    (long)ie.ts, ie.soilBefore, ie.soilAfter,
                    (unsigned long)ie.durationSec, (unsigned long)ie.volumeML,
                    (unsigned)ie.src);
                esp_task_wdt_reset();
                HTTPClient http;
                http.begin(REMOTE_LOG_URL "/vpd/irrig");
                http.setConnectTimeout(REMOTE_LOG_TIMEOUT);
                http.setTimeout(REMOTE_LOG_TIMEOUT);
                http.addHeader("Content-Type", "application/json");
                http.POST(body);
                http.end();
            }
        }
    }

    // ── DuckDNS update — keeps remote access hostname current ────────────────
    if (DUCKDNS_DOMAIN[0] && DUCKDNS_TOKEN[0]) {
        static unsigned long lastDnsMs = 0;
        if (millis() - lastDnsMs >= DUCKDNS_INTERVAL_MS) {
            lastDnsMs = millis();
            char url[220];
            snprintf(url, sizeof(url),
                "http://www.duckdns.org/update?domains=%s&token=%s&ip=",
                DUCKDNS_DOMAIN, DUCKDNS_TOKEN);
            esp_task_wdt_reset();  // GET may block up to 4 s
            HTTPClient http;
            http.begin(url);
            http.setTimeout(4000);
            int code = http.GET();
            if (code == 200) {
                char resp[8] = {};
                WiFiClient* stream = http.getStreamPtr();
                if (stream) stream->readBytes(resp, sizeof(resp) - 1);
                resp[strcspn(resp, "\r\n ")] = '\0';
                rlog("[DDNS] %s", resp);   // "OK" or "KO"
            } else {
                rlog("[DDNS] Update failed: %d", code);
            }
            http.end();
        }
    }

    // ── Heap + stack watchdog ─────────────────────────────────────────────────
    if (millis() - lastHeapLogMs >= 120000UL) {   // every 2 min (was 5 min)
        uint32_t freeHeap   = ESP.getFreeHeap();
        uint32_t c1Stack    = uxTaskGetStackHighWaterMark(nullptr);
        uint32_t c0Stack    = _controlTaskHandle
                              ? uxTaskGetStackHighWaterMark(_controlTaskHandle) : 0;
        // async_tcp and irTask are library-created tasks; look them up by name once.
        static TaskHandle_t _asyncHandle = nullptr;
        static TaskHandle_t _irHandle    = nullptr;
        if (!_asyncHandle) _asyncHandle  = xTaskGetHandle("async_tcp");
        if (!_irHandle)    _irHandle     = xTaskGetHandle("irTask");
        uint32_t asyncStack = _asyncHandle ? uxTaskGetStackHighWaterMark(_asyncHandle) : 0;
        uint32_t irStack    = _irHandle    ? uxTaskGetStackHighWaterMark(_irHandle)    : 0;
        rlog("[HEAP] Free: %u  min: %u  C0: %u  C1: %u  async: %u  ir: %u",
             freeHeap, ESP.getMinFreeHeap(), c0Stack, c1Stack, asyncStack, irStack);
        if (freeHeap < 20000) {   // raised from 10 KB — restart before allocations start failing
            climate.flushPrefsIfDirty();
            climate.flushProfilePrefsIfDirty();
            relays.flushPrefsIfDirty();
            wifiSensors.flushPrefsIfDirty();
            soil.flushCalibIfDirty();
            irRemote.flushPrefsIfDirty();
            rlog("[HEAP] Low — restarting");
            char hpBuf[32];
            snprintf(hpBuf, sizeof(hpBuf), "free=%lu", (unsigned long)freeHeap);
            eventlog("HEAP-RST", hpBuf);
            syslogSaveCrashInfo("HEAP-LOW");
            delay(200);
            ESP.restart();
        }
        lastHeapLogMs = millis();
    }

    // ── Core 1 task watchdog reset ────────────────────────────────────────────
    esp_task_wdt_reset();

    // Give RTOS a chance to process other Core-1 tasks
    yield();
}
