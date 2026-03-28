// ============================================================
//  VPD Grow-Tent Controller — Arduino IDE entry point
//
//  Board      : ESP32 Dev Module
//  Partition  : Default 4MB with spiffs
//  Upload spd : 921600
//
//  All source code is in the src/ folder.
//  Arduino IDE 2.x compiles src/ automatically — nothing
//  needs to be changed here.
//
//  LIBRARIES to install via Library Manager (Tools → Manage Libraries):
//    • DHT sensor library        by Adafruit        (≥ 1.4.6)
//    • Adafruit Unified Sensor   by Adafruit        (≥ 1.1.14)
//    • ESP Async WebServer       by me-no-dev       (≥ 1.2.3)
//    • AsyncTCP                  by me-no-dev       (≥ 1.1.1)
//    • ArduinoJson               by Benoit Blanchon (≥ 7.1.0)
//
//  WEB UI (index.html) upload — one-time, or after changing data/:
//    1. Install plugin: https://github.com/lorol/arduino-esp32fs-plugin
//       Place the .jar in ~/Documents/Arduino/tools/ESP32FS/tool/
//       Restart Arduino IDE.
//    2. Tools → ESP32 LittleFS Data Upload
//       (uploads the data/ folder to the ESP32 flash)
// ============================================================
