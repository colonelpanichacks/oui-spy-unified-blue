#ifndef MODES_H
#define MODES_H

// Shared preamble used by every mode's exported setup() to reset the radio
// state (WiFi.persistent(false) + WIFI_OFF + esp_wifi_restore). Prevents the
// Arduino WiFi wrapper from writing per-mode SSIDs back to ESP32-native NVS,
// which is the root cause of "wrong SSID after switching modes" bugs.
void ouispy_mode_preamble(const char* modeName);

// Log the actual softAP state after a mode's setup runs. If expectAP is true
// and no AP came up, starts a fallback "oui-spy-recovery" AP so the board is
// still reachable. Returns true if an AP is live at the end of the call.
bool ouispy_log_ap_state(const char* modeName, bool expectAP);

// Mode 1: OUI Spy Detector
void detector_setup();
void detector_loop();
void detector_stop();

// Mode 2: Foxhunter
void foxhunter_setup();
void foxhunter_loop();
void foxhunter_stop();

// Mode 3: Flock-You — Promiscuous WiFi Edition
void flockyou_promiscious_setup();
void flockyou_promiscious_loop();
void flockyou_promiscious_stop();

// Mode 4: PCAP — Passive WiFi Packet Capture
void pcap_setup();
void pcap_loop();
void pcap_stop();

// Mode 5: Sky Spy
void skyspy_setup();
void skyspy_loop();
void skyspy_stop();

// Mode 6: BLE Sniff — Passive BLE advertising capture
void blesniff_setup();
void blesniff_loop();
void blesniff_stop();

// Mode 7: Visual Scout — GPS + Deflock DB proximity + Camera stream for Pi/Coral
void visual_scout_setup();
void visual_scout_loop();
void visual_scout_stop();

#endif // MODES_H
