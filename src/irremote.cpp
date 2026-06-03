#include "irremote.h"
#include "syslog.h"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

// IRremoteESP8266 — include protocol headers ONLY (no ac.begin/ac.send called here;
// objects are used for state encoding only, transmit goes through the shared IRsend).
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <ir_Tcl.h>
#include <ir_Midea.h>
#include <ir_Coolix.h>
#include <ir_Gree.h>

#include <Preferences.h>

#define IR_LED_PIN    23    // GPIO 23 — former IR TX pin (UNUSED since 2026-04-13)
#define IR_TASK_STACK 16384 // dedicated stack prevents the stack-overflow crash

IRRemote irRemote;

struct QueuedCmd { IRCommand cmd; IRProto proto; };
static QueueHandle_t _irQueue = nullptr;

// ─── IR task (Core 1, priority 1) ────────────────────────────────────────────
// Single IRsend owns the RMT channel.  AC protocol objects are created on the
// task stack purely for state encoding (getRaw()); their internal IRsend is
// never begin()-ed so no additional RMT channel is opened.
static void irTask(void*) {
    IRsend irsend(IR_LED_PIN);
    irsend.begin();
    rlog("[IR] Task started on Core %d — GPIO %d", xPortGetCoreID(), IR_LED_PIN);

    static const uint8_t TCL_MODES[] = { kTcl112AcCool, kTcl112AcHeat, kTcl112AcFan,
                                          kTcl112AcDry,  kTcl112AcAuto };
    static const uint8_t TCL_FANS[]  = { kTcl112AcFanAuto, kTcl112AcFanLow,
                                          kTcl112AcFanMed,  kTcl112AcFanHigh };

    static const uint8_t MID_MODES[] = { kMideaACCool, kMideaACHeat, kMideaACFan,
                                          kMideaACDry,  kMideaACAuto };
    static const uint8_t MID_FANS[]  = { kMideaACFanAuto, kMideaACFanLow,
                                          kMideaACFanMed,  kMideaACFanHigh };

    static const uint8_t COO_MODES[] = { kCoolixCool, kCoolixHeat, kCoolixFan,
                                          kCoolixDry,  kCoolixAuto };
    static const uint8_t COO_FANS[]  = { kCoolixFanAuto, kCoolixFanMin,
                                          kCoolixFanMed,  kCoolixFanMax };

    static const uint8_t GRE_MODES[] = { kGreeCool, kGreeHeat, kGreeFan,
                                          kGreeDry,  kGreeAuto };
    static const uint8_t GRE_FANS[]  = { kGreeFanAuto, kGreeFanMin,
                                          kGreeFanMed,  kGreeFanMax };

    QueuedCmd qc;
    for (;;) {
        if (xQueueReceive(_irQueue, &qc, portMAX_DELAY) != pdTRUE) continue;
        const IRCommand& c = qc.cmd;

        rlog("[IR] Send proto=%d pow=%d temp=%d mode=%d fan=%d",
             (int)qc.proto, (int)c.power, (int)c.temp, (int)c.mode, (int)c.fan);

        switch (qc.proto) {
            // ── TCL / Fuji / Tornado 112-bit ─────────────────────────────────
            case IRProto::Tcl112Ac: {
                IRTcl112Ac ac(IR_LED_PIN);   // encoding only — never begin()/send()
                ac.setPower(c.power);
                ac.setTemp(c.temp);
                ac.setMode(c.mode < 5 ? TCL_MODES[c.mode] : kTcl112AcCool);
                ac.setFan (c.fan  < 4 ? TCL_FANS [c.fan]  : kTcl112AcFanAuto);
                irsend.sendTcl112Ac(ac.getRaw(), kTcl112AcStateLength);
                break;
            }
            // ── Midea / Carrier / Electra / Kelvinator ────────────────────────
            case IRProto::MideaAC: {
                IRMideaAC ac(IR_LED_PIN);
                ac.setPower(c.power);
                ac.setTemp(c.temp);
                ac.setMode(c.mode < 5 ? MID_MODES[c.mode] : kMideaACCool);
                ac.setFan (c.fan  < 4 ? MID_FANS [c.fan]  : kMideaACFanAuto);
                irsend.sendMidea(ac.getRaw(), kMideaBits);
                break;
            }
            // ── Coolix / Beko / Hyundai / Innovair ───────────────────────────
            case IRProto::CoolixAC: {
                IRCoolixAC ac(IR_LED_PIN);
                ac.setPower(c.power);
                ac.setTemp(c.temp);
                ac.setMode(c.mode < 5 ? COO_MODES[c.mode] : kCoolixCool);
                ac.setFan (c.fan  < 4 ? COO_FANS [c.fan]  : kCoolixFanAuto);
                irsend.sendCoolix48(ac.getRaw(), kCoolix48Bits);
                break;
            }
            // ── Gree / Amcor / Cooper Hunter ─────────────────────────────────
            case IRProto::GreeAC: {
                IRGreeAC ac(IR_LED_PIN);
                ac.setPower(c.power);
                ac.setTemp(c.temp);
                ac.setMode(c.mode < 5 ? GRE_MODES[c.mode] : kGreeCool);
                ac.setFan (c.fan  < 4 ? GRE_FANS [c.fan]  : kGreeFanAuto);
                irsend.sendGree(ac.getRaw(), kGreeStateLength);
                break;
            }
            default: break;
        }
    }
}

// ─── Public API ───────────────────────────────────────────────────────────────
void IRRemote::begin() {
    loadPrefs();
    _irQueue = xQueueCreate(4, sizeof(QueuedCmd));
    xTaskCreatePinnedToCore(irTask, "irTask", IR_TASK_STACK, nullptr, 1, nullptr, 1);
}

bool IRRemote::sendCommand(const IRCommand& cmd) {
    if (!_irQueue) return false;
    _lastCmd    = cmd;
    _prefsDirty = true;
    QueuedCmd qc = { cmd, _proto };
    return xQueueSend(_irQueue, &qc, 0) == pdTRUE;  // non-blocking: drop if full
}

void IRRemote::setProtocol(IRProto p) {
    _proto      = p;
    _prefsDirty = true;
}

void IRRemote::flushPrefsIfDirty() {
    if (!_prefsDirty) return;
    _prefsDirty = false;
    savePrefs();
}

void IRRemote::loadPrefs() {
    Preferences prefs;
    prefs.begin("irremote", true);
    _proto        = (IRProto)prefs.getUChar("proto", 0);
    _lastCmd.power = prefs.getBool  ("power", false);
    _lastCmd.temp  = prefs.getUChar ("temp",  24);
    _lastCmd.mode  = prefs.getUChar ("mode",  0);
    _lastCmd.fan   = prefs.getUChar ("fan",   0);
    prefs.end();
    if ((uint8_t)_proto >= (uint8_t)IRProto::NUM_PROTO) _proto = IRProto::Tcl112Ac;
    _lastCmd.temp = constrain(_lastCmd.temp, 16, 30);
    _lastCmd.mode = constrain(_lastCmd.mode, 0,  4);
    _lastCmd.fan  = constrain(_lastCmd.fan,  0,  3);
}

void IRRemote::savePrefs() {
    Preferences prefs;
    prefs.begin("irremote", false);
    prefs.putUChar("proto", (uint8_t)_proto);
    prefs.putBool ("power", _lastCmd.power);
    prefs.putUChar("temp",  _lastCmd.temp);
    prefs.putUChar("mode",  _lastCmd.mode);
    prefs.putUChar("fan",   _lastCmd.fan);
    prefs.end();
}
