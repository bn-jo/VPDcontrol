#pragma once

// Initialise AsyncWebServer + WebSocket
void webBegin();

// Push current state to all connected WebSocket clients
// Call from main loop every WS_PUSH_INTERVAL_MS
void webBroadcast();
