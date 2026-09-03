/*
 * Mode 7: Visual Scout — GPS proximity + Deflock DB + Camera stream
 *
 * Wraps src/raw/visual_scout.cpp in an anonymous namespace so its internal
 * symbols don't collide with other modes.
 *
 * Setup:
 *   1. Wire a GPS module to GPIO 44 (RX) / 43 (TX)
 *   2. Wire OV2640 camera per CAM_PIN_* constants in visual_scout.cpp
 *   3. Flash and connect to WiFi AP "ouispy-scout" (open)
 *   4. Open http://192.168.4.1 — enter Pi proxy URL or WiFi STA creds,
 *      then press "SYNC DB" to download Flock camera locations
 *   5. The Raspberry Pi stream consumer reads http://192.168.4.1:81/stream
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <TinyGPSPlus.h>
#include "esp_camera.h"
#include "modes.h"

#define setup  visual_scout_ns_setup
#define loop   visual_scout_ns_loop
#define stop   visual_scout_ns_stop

namespace {
#include "raw/visual_scout.cpp"
} // anonymous namespace

#undef setup
#undef loop
#undef stop

void visual_scout_setup() {
    ouispy_mode_preamble("MODE 7 VISUAL SCOUT");
    visual_scout_ns_setup();
    ouispy_log_ap_state("MODE 7 VISUAL SCOUT", /*expectAP=*/true);
}
void visual_scout_loop() { visual_scout_ns_loop(); }
void visual_scout_stop() { visual_scout_ns_stop(); }
