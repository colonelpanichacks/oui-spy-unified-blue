/*
 * Mode 6: Mega_Maid
 * BLE + WiFi MAC/OUI collector with web dashboard.
 * Collects all seen BLE advertisements and WiFi unicast MACs (no watchlist).
 * Dashboard mode serves detection export via WiFi AP "megamaid" on boot.
 * Triple-click KEY1 to enter collection mode (50/50 BLE/WiFi sniff).
 * Double-click KEY1 to pause/resume scanning while in collection mode.
 */

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

#define setup megamaid_ns_setup
#define loop  megamaid_ns_loop

volatile bool g_megamaid_suppress_key1_menu_hold = false;

bool megamaidKey1MenuHoldBlocked(void) {
    return g_megamaid_suppress_key1_menu_hold;
}

#define MM_KEY1_MENU_HOLD_SET(BLOCK) do {            \
    g_megamaid_suppress_key1_menu_hold = (BLOCK);    \
    if (BLOCK) {                                     \
        nessoResetBootButtonHold();                  \
    }                                                \
} while (0)

namespace {
#include "raw/mm_wifi_sniff.cpp"
#include "raw/megamaid.cpp"
}

#undef MM_KEY1_MENU_HOLD_SET
#undef setup
#undef loop

void megamaid_setup() { megamaid_ns_setup(); }
void megamaid_loop()  { megamaid_ns_loop(); }
