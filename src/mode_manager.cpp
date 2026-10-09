/*
 * OUI SPY - Mode manager
 *
 * Owns the mode registry and the exclusive-activation state machine that the
 * pager UI (and, on the XIAO, the loop) drive. Registry order is pager order.
 */
#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <NimBLEDevice.h>

#include "mode_iface.h"
#include "modes.h"

namespace {

const ModeDef kModes[] = {
    { "detector",  "DETECTOR",       "BLE alert tool for specific devices",
      detector_setup,             detector_loop,             detector_stop },
    { "foxhunter", "FOXHUNTER",      "RSSI proximity tracker",
      foxhunter_setup,            foxhunter_loop,            foxhunter_stop },
    { "flock-wifi","FLOCK-YOU WIFI", "Promiscuous 2.4GHz surveillance sniffer",
      flockyou_promiscious_setup, flockyou_promiscious_loop, flockyou_promiscious_stop },
    { "pcap",      "PCAP",           "Passive WiFi packet capture (Wireshark-ready)",
      pcap_setup,                 pcap_loop,                 pcap_stop },
    { "skyspy",    "SKY SPY",        "Drone Remote ID monitor",
      skyspy_setup,               skyspy_loop,               skyspy_stop },
    { "blesniff",  "BLE SNIFF",      "Passive BLE advertising capture (Wireshark-ready)",
      blesniff_setup,             blesniff_loop,             blesniff_stop },
    { "axeoff",    "AXEOFF",         "Headless Axon detector (beep-only)",
      axeoff_setup,               axeoff_loop,               axeoff_stop },
};
constexpr int kModeCount = sizeof(kModes) / sizeof(kModes[0]);

int gActive = -1;  // -1 == IDLE

} // namespace

namespace ModeManager {

void begin() {
    gActive = -1;
    Serial.println("[MODEMGR] ready (IDLE)");
}

int count() { return kModeCount; }

const ModeDef& at(int i) { return kModes[i]; }

int activeIndex() { return gActive; }

void releaseRadios() {
    // BLE: only deinit if a mode actually brought the stack up.
    if (NimBLEDevice::getInitialized()) {
        NimBLEDevice::deinit(true);
    }
    // WiFi: drop promiscuous, AP and STA, then power the radio down so the
    // next mode's setup() starts from a known-clean state.
    esp_wifi_set_promiscuous(false);
    WiFi.softAPdisconnect(true);
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);
    delay(100);
    Serial.println("[MODEMGR] radios released");
}

void deactivate() {
    if (gActive < 0) return;
    const ModeDef& m = kModes[gActive];
    Serial.printf("[MODEMGR] stopping '%s'\n", m.id);
    if (m.stop) m.stop();
    releaseRadios();
    gActive = -1;
}

void activate(int i) {
    if (i < 0 || i >= kModeCount) {
        Serial.printf("[MODEMGR] activate: bad index %d\n", i);
        return;
    }
    if (i == gActive) return;  // already running
    deactivate();              // stop current + release radios
    Serial.printf("[MODEMGR] starting '%s'\n", kModes[i].id);
    gActive = i;
    kModes[i].setup();
}

void tick() {
    if (gActive >= 0) {
        kModes[gActive].loop();
    }
}

} // namespace ModeManager
