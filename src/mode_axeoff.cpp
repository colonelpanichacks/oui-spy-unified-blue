/*
 * Mode 7: AXEOFF — headless Axon detector
 *
 * Passive, beep-only detector for Axon surveillance gear (body cameras,
 * Taser 7/10, Signal sidearm, Axon Fleet). Wraps the standalone firmware
 * from colonelpanichacks/axeoff in an anonymous namespace. No AP, no web
 * server, no NVS config — the buzzer is the entire UX. Time-sliced BLE-first
 * radio schedule; both radios fully passive (BLE passive scan + WiFi
 * promiscuous listen only).
 */

// All includes from the original firmware must be OUTSIDE the namespace
// so they get external linkage. Re-inclusions inside the namespace are
// no-ops thanks to header guards.
#include <Arduino.h>
#include <WiFi.h>
#include "esp_wifi.h"
#include <esp_log.h>
#include <esp_mac.h>
#include <NimBLEDevice.h>
#include <NimBLEScan.h>
#include <NimBLEAdvertisedDevice.h>
#include "modes.h"

// Rename setup/loop so they don't collide with the unified main.cpp's
// Arduino entry points (and the other modes' wrapped setup/loop).
#define setup axeoff_ns_setup
#define loop  axeoff_ns_loop

namespace {
#include "raw/axeoff.cpp"
} // anonymous namespace

#undef setup
#undef loop

void axeoff_setup() {
    // Mode 7 has NO AP (passive sniffing only), but still touches the radio.
    // The preamble matters so a prior mode's leftover softAP state can't
    // reappear on this boot.
    ouispy_mode_preamble("MODE 7 AXEOFF");
    axeoff_ns_setup();
    ouispy_log_ap_state("MODE 7 AXEOFF", /*expectAP=*/false);
}
void axeoff_loop()  { axeoff_ns_loop(); }
void axeoff_stop()  {
    // The mode's globals live in this file's anonymous namespace, so they are
    // reachable unqualified here. Stop mode-specific resources; the manager's
    // releaseRadios() then deinits NimBLE and powers WiFi down after we return.
    if (pBLEScan && pBLEScan->isScanning()) {
        pBLEScan->stop();
    }
    audioToneStop();
}
