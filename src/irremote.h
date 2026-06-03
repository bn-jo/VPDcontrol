#pragma once
#include <Arduino.h>

// IR protocols to try — in order of likelihood:
// 0 = Tcl112Ac (default), 1 = MideaAC, 2 = CoolixAC, 3 = GreeAC
enum class IRProto : uint8_t { Tcl112Ac = 0, MideaAC, CoolixAC, GreeAC, NUM_PROTO };

struct IRCommand {
    bool    power = false;
    uint8_t temp  = 24;   // Celsius, 16-30
    uint8_t mode  = 0;    // 0=Cool 1=Heat 2=Fan 3=Dry 4=Auto
    uint8_t fan   = 0;    // 0=Auto 1=Low  2=Med 3=High
};

class IRRemote {
public:
    // Call once from setup() — starts the IR FreeRTOS task on Core 1
    void begin();

    // Queue an IR command (thread-safe, drops if queue full)
    bool sendCommand(const IRCommand& cmd);

    void    setProtocol(IRProto p);
    IRProto protocol()        const { return _proto; }
    const IRCommand& lastCmd() const { return _lastCmd; }

    // Call from Core 1 loop() — deferred NVS write
    void flushPrefsIfDirty();

private:
    IRProto   _proto      = IRProto::Tcl112Ac;
    IRCommand _lastCmd    = {};
    bool      _prefsDirty = false;

    void loadPrefs();
    void savePrefs();
};

extern IRRemote irRemote;
