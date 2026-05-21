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
#include "autotune.h"
#include "wifisensors.h"
#include "intakesensor.h"
#include "syslog.h"

static TaskHandle_t _controlTaskHandle = nullptr;

// ─── Remote log push — shared between controlTask (writer) and loop() (sender)
// Single-slot queue: controlTask fills _pendingIrrig and sets the flag;
// loop() reads it, clears the flag, then fires the HTTP POST on Core 1.
static volatile bool _irrigPushPending = false;
static IrrigEvent    _pendingIrrig;

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
            bool ok = sensors.read();
            if (!ok) Serial.println("[SNS] Read failed — using stale data");
            if (ok) {
                const SensorData& sd = sensors.data();
                autoTuner.feed(sd.temperature, sd.humidity, sd.vpd);
            }
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
        autoTuner.tick();

        // ── Log completed irrigation events ───────────────────────────────────
        IrrigEvent iev;
        if (relays.popIrrigEvent(iev)) {
            logger.logIrrigation(iev.ts, iev.soilBefore, iev.soilAfter,
                                 iev.durationSec, iev.volumeML, iev.src);
            // Queue for remote push (consumed by loop() on Core 1)
            _pendingIrrig     = iev;
            _irrigPushPending = true;
        }

        // ── Data logging ──────────────────────────────────────────────────────
        if (now - lastLogMs >= LOG_INTERVAL_MS) {
            if (sensors.data().valid) {
                float soilPct = soil.data().valid ? soil.data().moisture : -1.0f;
                logger.log(sensors.data(), soilPct);
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

    {
        esp_reset_reason_t rr = esp_reset_reason();
        const char* rrStr = "UNKNOWN";
        switch (rr) {
            case ESP_RST_POWERON:   rrStr = "POWER-ON";          break;
            case ESP_RST_SW:        rrStr = "SOFTWARE-RESTART";  break;
            case ESP_RST_PANIC:     rrStr = "PANIC-CRASH";       break;
            case ESP_RST_INT_WDT:   rrStr = "INT-WATCHDOG";      break;
            case ESP_RST_TASK_WDT:  rrStr = "TASK-WATCHDOG";     break;
            case ESP_RST_WDT:       rrStr = "WDT-OTHER";         break;
            case ESP_RST_BROWNOUT:  rrStr = "BROWNOUT";          break;
            default: break;
        }
        rlog("[BOOT] Reset reason: %s", rrStr);
        // Stamp this boot's reason into NVS so it survives the NEXT reboot
        syslogSaveCrashInfo(rrStr);
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
    climate.begin();
    relays.setIrrigMode((uint8_t)climate.getMode());  // sync initial stage profile
    autoTuner.begin();
    wifiSensors.begin();  // load NVS now — WiFi not required for NVS read

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
        ArduinoOTA.setPassword("REDACTED");
        ArduinoOTA.onStart([]() {
            // Flush any pending NVS writes before the reboot so stage/prefs survive
            climate.flushPrefsIfDirty();
            climate.flushProfilePrefsIfDirty();
            relays.flushPrefsIfDirty();
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

    // ── Deferred NVS flush — runs first, before any early return or restart ──────
    relays.flushPrefsIfDirty();
    climate.flushPrefsIfDirty();
    climate.flushProfilePrefsIfDirty();

    // ── Stage auto-transition (Core 1) ────────────────────────────────────────
    // Runs here, NOT in controlTask, so it is serialised with setMode() on Core 1
    // and cannot race with user-initiated stage changes.
    climate.checkAutoTransition();

    // ── WiFi reconnect guard ──────────────────────────────────────────────────
    // setAutoReconnect handles brief dropouts; this catches longer outages where
    // the driver gives up (e.g. router reboot at night).
    if (WiFi.status() != WL_CONNECTED) {
        roamScanActive = false;   // cancel any pending roam scan
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

    wifiSensors.update();    // Non-blocking; polls each due sensor with exponential backoff

    if (millis() - lastWsPushMs >= WS_PUSH_INTERVAL_MS) {
        webBroadcast();
        lastWsPushMs = millis();
    }

    // ── Fix crash epoch once NTP syncs ───────────────────────────────────────
    // At boot, syslogSaveCrashInfo runs before NTP so the epoch is 0.
    // Update it to the correct wall-clock time as soon as NTP is ready.
    {
        static bool epochFixed = false;
        if (!epochFixed && time(nullptr) > 1000000000L) {
            epochFixed = true;
            syslogFixEpoch();
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
            if (inNightWindow && !dailyRestartDone) {
                dailyRestartDone = true;
                climate.flushPrefsIfDirty();
                climate.flushProfilePrefsIfDirty();
                relays.flushPrefsIfDirty();
                rlog("[MAIN] Safe restart at 01:20");
                syslogSaveCrashInfo("SOFTWARE-RESTART");
                delay(200);
                ESP.restart();
            }
            if (inNoonWindow && !noonRestartDone) {
                noonRestartDone = true;
                climate.flushPrefsIfDirty();
                climate.flushProfilePrefsIfDirty();
                relays.flushPrefsIfDirty();
                rlog("[MAIN] Safe restart at 13:20");
                syslogSaveCrashInfo("SOFTWARE-RESTART");
                delay(200);
                ESP.restart();
            }
            if (!inNightWindow) dailyRestartDone = false;
            if (!inNoonWindow)  noonRestartDone  = false;
        }
    }

    // ── Remote data push ──────────────────────────────────────────────────────
    // Mirror every sensor log point and irrigation event to the remote server.
    // Runs on Core 1 alongside WiFi — HTTPClient is not safe on Core 0.
    {
        static unsigned long lastRemoteLogMs = 0;
        if (millis() - lastRemoteLogMs >= LOG_INTERVAL_MS && sensors.data().valid) {
            lastRemoteLogMs = millis();
            const SensorData& sd = sensors.data();
            char body[100];
            float soilPct = soil.data().valid ? soil.data().moisture : -1.0f;
            int n;
            if (soilPct >= 0.0f)
                n = snprintf(body, sizeof(body),
                    "{\"t\":%ld,\"T\":%.1f,\"H\":%.1f,\"V\":%.3f,\"S\":%.1f}",
                    (long)sd.timestamp, sd.temperature, sd.humidity, sd.vpd, soilPct);
            else
                n = snprintf(body, sizeof(body),
                    "{\"t\":%ld,\"T\":%.1f,\"H\":%.1f,\"V\":%.3f}",
                    (long)sd.timestamp, sd.temperature, sd.humidity, sd.vpd);
            (void)n;
            HTTPClient http;
            http.begin(REMOTE_LOG_URL "/vpd/log");
            http.setTimeout(REMOTE_LOG_TIMEOUT);
            http.addHeader("Content-Type", "application/json");
            http.POST(body);
            http.end();
        }

        if (_irrigPushPending) {
            _irrigPushPending = false;
            IrrigEvent ie = _pendingIrrig;
            char body[128];
            snprintf(body, sizeof(body),
                "{\"t\":%ld,\"b\":%.1f,\"a\":%.1f,\"d\":%lu,\"ml\":%lu,\"src\":%u}",
                (long)ie.ts, ie.soilBefore, ie.soilAfter,
                (unsigned long)ie.durationSec, (unsigned long)ie.volumeML, (unsigned)ie.src);
            HTTPClient http;
            http.begin(REMOTE_LOG_URL "/vpd/irrig");
            http.setTimeout(REMOTE_LOG_TIMEOUT);
            http.addHeader("Content-Type", "application/json");
            http.POST(body);
            http.end();
        }
    }

    // ── Heap + stack watchdog ─────────────────────────────────────────────────
    if (millis() - lastHeapLogMs >= 120000UL) {   // every 2 min (was 5 min)
        uint32_t freeHeap   = ESP.getFreeHeap();
        uint32_t c1Stack    = uxTaskGetStackHighWaterMark(nullptr);
        uint32_t c0Stack    = _controlTaskHandle
                              ? uxTaskGetStackHighWaterMark(_controlTaskHandle) : 0;
        rlog("[HEAP] Free: %u  min-ever: %u  C0-stack: %u  C1-stack: %u",
             freeHeap, ESP.getMinFreeHeap(), c0Stack, c1Stack);
        if (freeHeap < 20000) {   // raised from 10 KB — restart before allocations start failing
            climate.flushPrefsIfDirty();
            climate.flushProfilePrefsIfDirty();
            relays.flushPrefsIfDirty();
            rlog("[HEAP] Low — restarting");
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
