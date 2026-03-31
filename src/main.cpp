#include <Arduino.h>
#include <WiFi.h>
#include <esp_sntp.h>       // NTP
#include <time.h>

#include "config.h"
#include "sensors.h"
#include "soil.h"
#include "relays.h"
#include "climate.h"
#include "datalogger.h"
#include "webserver.h"
#include "autotune.h"
#include "remotesensor.h"
#include "intakesensor.h"

// ─── FreeRTOS task: sensor reading + climate control ─────────────────────────
// Runs on Core 0 to leave Core 1 free for WiFi + web server
static void controlTask(void* pvParam) {
    unsigned long lastSensorMs  = 0;
    unsigned long lastControlMs = 0;
    unsigned long lastLogMs     = 0;

    for (;;) {
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

        // ── Climate control ───────────────────────────────────────────────────
        if (now - lastControlMs >= CONTROL_INTERVAL_MS) {
            climate.update(sensors.data());
            lastControlMs = now;
        }

        // ── Relay state machine (timers + hysteresis guards) ──────────────────
        relays.update();
        autoTuner.tick();

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

// ─── WiFi connect (blocking until connected or timeout) ───────────────────────
static bool wifiConnect() {
    Serial.printf("[WIFI] Connecting to %s", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);   // let the driver handle brief dropouts
    WiFi.persistent(false);        // don't wear out flash with credential writes

#ifdef STATIC_IP
    WiFi.config(
        IPAddress(STATIC_IP),
        IPAddress(STATIC_GATEWAY),
        IPAddress(STATIC_SUBNET),
        IPAddress(STATIC_DNS)
    );
    Serial.printf(" (static IP %d.%d.%d.%d)", STATIC_IP);
#endif

    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - start > WIFI_TIMEOUT_MS) {
            Serial.println("\n[WIFI] Timeout!");
            return false;
        }
        delay(500);
        Serial.print('.');
    }
    Serial.printf("\n[WIFI] Connected — IP: %s\n", WiFi.localIP().toString().c_str());
    return true;
}

// ─── Setup ────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n=== VPD Control System ===");

    // Hardware init
    sensors.begin();
    intakeSensor.begin();
    soil.begin();
    relays.begin();
    climate.begin();
    logger.begin();
    autoTuner.begin();

    // WiFi
    if (wifiConnect()) {
        // NTP time sync (non-blocking after initial handshake)
        configTime(NTP_GMT_OFFSET_SEC, NTP_DST_OFFSET_SEC, NTP_SERVER);
        Serial.println("[NTP] Sync requested");

        // Start web server only when WiFi is up
        webBegin();
    } else {
        Serial.println("[WARN] Running without WiFi — web UI unavailable");
    }

    // Start control task on Core 0
    xTaskCreatePinnedToCore(
        controlTask,
        "ControlTask",
        8192,       // Stack size (bytes)
        nullptr,
        3,          // Priority (higher = more urgent)
        nullptr,
        0           // Core 0
    );

    Serial.println("[MAIN] Setup complete");
}

// ─── Main loop (Core 1) ───────────────────────────────────────────────────────
// Handles WebSocket broadcast + keeps async server running
void loop() {
    static unsigned long lastWsPushMs  = 0;
    static unsigned long lastWifiRetry = 0;
    static unsigned long lastHeapLogMs = 0;

    // ── WiFi reconnect guard ──────────────────────────────────────────────────
    // setAutoReconnect handles brief dropouts; this catches longer outages where
    // the driver gives up (e.g. router reboot at night).
    if (WiFi.status() != WL_CONNECTED) {
        unsigned long now = millis();
        if (now - lastWifiRetry >= 30000UL) {
            Serial.println("[WIFI] Lost — reconnecting...");
            WiFi.disconnect(false, false);
            WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
            lastWifiRetry = now;
        }
        yield();
        return;   // skip WS push and remote fetch while offline
    }

    remoteSensor.update();   // Non-blocking; self-throttles to REMOTE_SENSOR_INTERVAL_MS

    if (millis() - lastWsPushMs >= WS_PUSH_INTERVAL_MS) {
        webBroadcast();
        lastWsPushMs = millis();
    }

    // ── Heap watchdog ─────────────────────────────────────────────────────────
    // Log free heap every 5 min. Restart cleanly if critically low — prevents
    // silent hangs caused by heap exhaustion after many hours of operation.
    if (millis() - lastHeapLogMs >= 300000UL) {
        uint32_t freeHeap = ESP.getFreeHeap();
        Serial.printf("[HEAP] Free: %u bytes  (min ever: %u)\n",
                      freeHeap, ESP.getMinFreeHeap());
        if (freeHeap < 10000) {
            Serial.println("[HEAP] Critical — restarting");
            delay(200);
            ESP.restart();
        }
        lastHeapLogMs = millis();
    }

    // Give RTOS a chance to process other Core-1 tasks
    yield();
}
