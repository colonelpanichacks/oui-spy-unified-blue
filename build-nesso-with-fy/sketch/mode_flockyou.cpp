#line 1 "/Users/nicholastenbrink/projects/nesso/oui_spy_nesso/mode_flockyou.cpp"
/*
 * Mode 4: Flock-You
 * Surveillance device detector with web dashboard.
 * Scans BLE and WiFi promiscuous for Flock Safety, Raven, and surveillance patterns.
 * Dashboard mode serves detection export via time-sliced WiFi AP "flockyou".
 * Detections stored in memory; exportable as JSON or CSV.
 */

// All includes from flock-you (outside namespace for proper linkage)
#include <Arduino.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <NimBLEDevice.h>
#include <NimBLEScan.h>
#include <NimBLEAdvertisedDevice.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <stdint.h>
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <SPIFFS.h>
#include <TinyGPS++.h>
#include "board_pins.h"
#include "board_hw.h"
#include "board_gps.h"
#include "board_neopixel.h"
#include "nesso_ui.h"
#include "nimble_compat.h"
#include "modes.h"

// Rename setup/loop
#define setup flockyou_ns_setup
#define loop  flockyou_ns_loop

namespace {
#include "raw/fy_wifi_sniff.cpp"
#include "raw/flockyou.cpp"
} // anonymous namespace

#undef setup
#undef loop

void flockyou_setup() { flockyou_ns_setup(); }
void flockyou_loop()  { flockyou_ns_loop(); }
