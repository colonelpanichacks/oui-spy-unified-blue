// ============================================================================
// FLOCK-YOU: Surveillance Device Detector with Web Dashboard
// ============================================================================
// Detection methods:
//   BLE: MAC prefix, device name, manufacturer ID, Raven UUID
//   WiFi promiscuous: OUI match (addr1/addr2), DeFlockJoplin wildcard probe
//
// Default: COLLECT mode — 50% BLE / 50% WiFi promiscuous (time-sliced).
// Double-click KEY1 toggles DASHBOARD mode: softAP always on; BLE/WiFi sniff
// alternate on the remaining radio time (50/50). WiFi sniff stays on AP channel.
// Hold KEY1 ~1.5s (handled in main.cpp) still returns to the mode selector.
// AP "flockyou" / "flockyou123" — only active in dashboard mode.
// All detections stored in memory, exportable as JSON or CSV
// ============================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <NimBLEDevice.h>
#include <NimBLEScan.h>
#include <NimBLEAdvertisedDevice.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <SPIFFS.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include "esp_wifi.h"

// ============================================================================
// CONFIGURATION
// ============================================================================
#define FY_NEOPIXEL_BRIGHTNESS 50
#define FY_NEOPIXEL_DETECTION_BRIGHTNESS 200

// Audio
#define LOW_FREQ 200
#define HIGH_FREQ 800
#define DETECT_FREQ 1000
#define HEARTBEAT_FREQ 600
#define BOOT_BEEP_DURATION 300
#define DETECT_BEEP_DURATION 150
#define HEARTBEAT_DURATION 100

// BLE scanning
#define BLE_SCAN_DURATION 2      // seconds per scan
#define BLE_SCAN_INTERVAL 3000   // ms between clearing cached scan results

// ESP32-C6 radio sharing: explicit time slices (one consumer at a time).
#define FY_BLE_SLOT_MS              100
#define FY_CYCLE_MS                 1000
#define FY_COLLECT_BLE_MS           500
#define FY_COLLECT_WIFI_MS          500
#define FY_DASH_BLE_MS              500   // dashboard: alternate with WiFi sniff
#define FY_DASH_WIFI_MS             500
#define FY_BLE_DASHBOARD_WINDOW_MS   70   // BLE scan window while softAP is up
#define FY_AP_SETTLE_MS             1500
#define FY_KEY1_DCLICK_MS           450

// Detection storage
#define MAX_DETECTIONS 200

// WiFi AP credentials
#define FY_AP_SSID "flockyou"
#define FY_AP_PASS "flockyou123"

// ============================================================================
// DETECTION PATTERNS
// ============================================================================

// Known Flock Safety MAC address prefixes (OUIs).
//
// Research credit: OrdoOuroborous / @NitekryDPaul
//   GitHub: https://github.com/nitekry
//   Source of truth: https://github.com/nitekry/nite-oui-collection
//   Status tracker:  groups/flockers/my_tested_flock.md in that repo
//
// Plus 82:6b:f2 — contributed by Michael / DeFlockJoplin during a
// drive-test in Joplin; the 12th camera in the field run used this
// OUI and wasn't in @NitekryDPaul's original 30.
//
// Notes on demoted entries (intentionally absent below):
//   - f8:a2:d6 → hit on a Sony Media Player, marked Removed
//   - cc:cc:cc → no observed hits, marked Removed
//   - 00:0c:e7 → possible MediaTek false positive, marked Removed
// Notes on flagged entries (still tracked):
//   - 08:3a:88 has a known BLE Ring conflict; expect occasional
//     Ring-doorbell false positives in BLE mode.
static const char* mac_prefixes[] = {
    // @NitekryDPaul original promiscuous-mode set (29 OUIs)
    "70:c9:4e", "3c:91:80", "d8:f3:bc", "80:30:49", "b8:35:32",
    "14:5a:fc", "74:4c:a1", "08:3a:88", "9c:2f:9d", "c0:35:32",
    "94:08:53", "e4:aa:ea", "f4:6a:dd", "24:b2:b9", "00:f4:8d",
    "d0:39:57", "e8:d0:fc", "e0:4f:43", "b8:1e:a4", "70:08:94",
    "58:8e:81", "ec:1b:bd", "3c:71:bf", "58:00:e3", "90:35:ea",
    "5c:93:a2", "64:6e:69", "48:27:ea", "a4:cf:12",
    // @NitekryDPaul April 2026 additions (nite-oui-collection)
    "04:0d:84", "f0:82:c0", "1c:34:f1", "38:5b:44", "94:34:69",
    "b4:e3:f9", "b4:1e:52", "14:b5:cd", "94:2a:6f", "f4:e2:c6",
    "d4:11:d6", "e0:0a:f6",
    // Michael / DeFlockJoplin — drive-test in Joplin
    "82:6b:f2"
};

// BLE device name patterns (matched case-insensitive substring)
static const char* device_name_patterns[] = {
    "FS Ext Battery",
    "Penguin",
    "Flock",
    "Pigvision"
};

// BLE Manufacturer Company IDs
// Source: wgreenberg/flock-you - XUNTONG ID associated with Flock Safety devices
static const uint16_t ble_manufacturer_ids[] = {
    0x09C8   // XUNTONG
};

// ============================================================================
// RAVEN SURVEILLANCE DEVICE UUID PATTERNS
// ============================================================================

#define RAVEN_DEVICE_INFO_SERVICE   "0000180a-0000-1000-8000-00805f9b34fb"
#define RAVEN_GPS_SERVICE           "00003100-0000-1000-8000-00805f9b34fb"
#define RAVEN_POWER_SERVICE         "00003200-0000-1000-8000-00805f9b34fb"
#define RAVEN_NETWORK_SERVICE       "00003300-0000-1000-8000-00805f9b34fb"
#define RAVEN_UPLOAD_SERVICE        "00003400-0000-1000-8000-00805f9b34fb"
#define RAVEN_ERROR_SERVICE         "00003500-0000-1000-8000-00805f9b34fb"
#define RAVEN_OLD_HEALTH_SERVICE    "00001809-0000-1000-8000-00805f9b34fb"
#define RAVEN_OLD_LOCATION_SERVICE  "00001819-0000-1000-8000-00805f9b34fb"

// Raven-specific custom service UUIDs only. Do NOT include standard GATT
// services (0x180A Device Info, 0x1809 Health, 0x1819 Location) — they
// appear on phones, earbuds, watches, etc. and cause false positives.
static const char* raven_service_uuids[] = {
    RAVEN_GPS_SERVICE,
    RAVEN_POWER_SERVICE,
    RAVEN_NETWORK_SERVICE,
    RAVEN_UPLOAD_SERVICE,
    RAVEN_ERROR_SERVICE
};

// ============================================================================
// DETECTION STORAGE
// ============================================================================

struct FYDetection {
    char mac[18];
    char name[48];
    int rssi;
    char method[24];
    unsigned long firstSeen;
    unsigned long lastSeen;
    int count;
    bool isRaven;
    char ravenFW[16];
    uint8_t channel;   // WiFi channel (0 = BLE / N/A)
    // GPS from phone (wardriving)
    double gpsLat;
    double gpsLon;
    float gpsAcc;
    bool hasGPS;
};

static FYDetection fyDet[MAX_DETECTIONS];
static int fyDetCount = 0;
static SemaphoreHandle_t fyMutex = NULL;      // guards fyDet[] + fyDetCount
static SemaphoreHandle_t fyGPSMutex = NULL;   // guards fyGPS* globals

// ============================================================================
// GLOBALS
// ============================================================================

static bool fyBuzzerOn = true;
static Adafruit_NeoPixel fyPixel(1, BOARD_NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);
static bool fyPixelAlertMode = false;
static unsigned long fyPixelAlertStart = 0;
static unsigned long fyLastBleScan = 0;
static bool fyTriggered = false;
static bool fyDeviceInRange = false;
static unsigned long fyLastDetTime = 0;
static unsigned long fyLastHB = 0;
static NimBLEScan* fyBLEScan = NULL;
static AsyncWebServer fyServer(80);
static DNSServer flockyouDNS;
static bool fyRoutesRegistered = false;
static bool fyServerStarted = false;
static bool fyDnsStarted = false;
static bool fyDashboardApUp = false;
static unsigned long fyApStartMs = 0;

enum FyRadioProfile : uint8_t {
    FY_RADIO_COLLECT = 0,    // 50/50 BLE / WiFi sniff
    FY_RADIO_DASHBOARD = 1,  // AP always on; 50/50 BLE / WiFi sniff
};

enum FyRadioSlice : uint8_t {
    FY_SLICE_BLE = 0,
    FY_SLICE_WIFI_SNIFF = 1,
    FY_SLICE_AP = 2,
};

static FyRadioProfile fyRadioProfile = FY_RADIO_COLLECT;
static FyRadioSlice fyCurrentSlice = FY_SLICE_BLE;
static unsigned long fySliceStartMs = 0;
static unsigned long fyLastSliceTickMs = 0;
static bool fyKey1WasDown = false;
static uint8_t fyKey1ClickCount = 0;
static unsigned long fyKey1LastReleaseMs = 0;

// Phone GPS state (updated via browser Geolocation API -> /api/gps)
static double fyGPSLat = 0;
static double fyGPSLon = 0;
static float  fyGPSAcc = 0;
static bool   fyGPSValid = false;
static unsigned long fyGPSLastUpdate = 0;
static bool fyGPSIsHardware = false;
#define GPS_STALE_MS 30000

// Hardware GPS (UART1; board pins/baud in board_gps.h)
static TinyGPSPlus fyGPS;
static HardwareSerial fyGPSSerial(1);
static bool fyHWGPSDetected = false;
static bool fyHWGPSFix = false;
static int  fyHWGPSSats = 0;
static unsigned long fyHWGPSLastChar = 0;
#define GPS_HW_TIMEOUT_MS 5000
static unsigned long fyLastDashboardGpsPollMs = 0;

// Session persistence (SPIFFS)
#define FY_SESSION_FILE  "/session.json"
#define FY_PREV_FILE     "/prev_session.json"
#define FY_SAVE_INTERVAL 15000  // Auto-save every 15 seconds (prevent data loss on quick power-cycle)
static unsigned long fyLastSave = 0;
static int fyLastSaveCount = 0;  // Track changes to avoid unnecessary writes
static bool fySpiffsReady = false;

// ============================================================================
// AUDIO SYSTEM
// ============================================================================

static void fyBeep(int freq, int dur) {
    if (!fyBuzzerOn) return;
    tone(BUZZER_PIN, freq, dur);
    delay(dur + 50);
}

// Crow caw: harsh descending sweep with warble texture
static void fyCaw(int startFreq, int endFreq, int durationMs, int warbleHz) {
    if (!fyBuzzerOn) return;
    int steps = durationMs / 8;  // 8ms per step
    float fStep = (float)(endFreq - startFreq) / steps;
    for (int i = 0; i < steps; i++) {
        int f = startFreq + (int)(fStep * i);
        // Add warble: oscillate frequency +/- for raspy texture
        if (warbleHz > 0 && (i % 3 == 0)) {
            f += ((i % 6 < 3) ? warbleHz : -warbleHz);
        }
        if (f < 100) f = 100;
        tone(BUZZER_PIN, f, 10);
        delay(8);
    }
    noTone(BUZZER_PIN);
}

static void fyBootBeep() {
    printf("[FLOCK-YOU] Boot sound (buzzer %s)\n", fyBuzzerOn ? "ON" : "OFF");
    if (!fyBuzzerOn) return;

    // === CROW CALL SEQUENCE ===
    // Caw 1: sharp descending caw
    fyCaw(850, 380, 180, 40);
    delay(100);

    // Caw 2: slightly lower, shorter
    fyCaw(780, 350, 150, 50);
    delay(100);

    // Caw 3: longer trailing caw with more rasp
    fyCaw(820, 280, 220, 60);
    delay(80);

    // Quick staccato ending "kk-kk"
    tone(BUZZER_PIN, 600, 25); delay(40);
    tone(BUZZER_PIN, 550, 25); delay(40);
    noTone(BUZZER_PIN);

    printf("[FLOCK-YOU] *caw caw caw*\n");
}

static void fyDetectBeep() {
    printf("[FLOCK-YOU] Detection alert!\n");
    fyPixelAlertMode = true;
    fyPixelAlertStart = millis();
    if (!fyBuzzerOn) return;
    // Alarm crow: two sharp ascending chirps then a caw
    fyCaw(400, 900, 100, 30);   // rising alarm chirp
    delay(60);
    fyCaw(450, 950, 100, 30);   // second chirp, higher
    delay(60);
    fyCaw(900, 350, 200, 50);   // descending caw
}

static void fyHeartbeat() {
    if (!fyBuzzerOn) return;
    // Soft double coo - like a distant crow
    fyCaw(500, 400, 80, 20);
    delay(120);
    fyCaw(480, 380, 80, 20);
}

// ============================================================================
// NEOPIXEL FUNCTIONS
// ============================================================================

static uint32_t fyHsvToRgb(uint16_t h, uint8_t s, uint8_t v) {
    uint8_t r, g, b;
    if (s == 0) {
        r = g = b = v;
    } else {
        uint8_t region = h / 43;
        uint8_t remainder = (h - (region * 43)) * 6;
        uint8_t p = (v * (255 - s)) >> 8;
        uint8_t q = (v * (255 - ((s * remainder) >> 8))) >> 8;
        uint8_t t = (v * (255 - ((s * (255 - remainder)) >> 8))) >> 8;
        switch (region) {
            case 0: r = v; g = t; b = p; break;
            case 1: r = q; g = v; b = p; break;
            case 2: r = p; g = v; b = t; break;
            case 3: r = p; g = q; b = v; break;
            case 4: r = t; g = p; b = v; break;
            default: r = v; g = p; b = q; break;
        }
    }
    return fyPixel.Color(r, g, b);
}

// Idle: slow purple breathing (hue 270)
static void fyPixelBreathing() {
    static unsigned long lastUpdate = 0;
    static float brightness = 0.0;
    static bool increasing = true;
    if (millis() - lastUpdate < 20) return;
    lastUpdate = millis();
    if (increasing) {
        brightness += 0.02;
        if (brightness >= 1.0) { brightness = 1.0; increasing = false; }
    } else {
        brightness -= 0.02;
        if (brightness <= 0.1) { brightness = 0.1; increasing = true; }
    }
    uint32_t color = fyHsvToRgb(270, 255, (uint8_t)(FY_NEOPIXEL_BRIGHTNESS * brightness));
    fyPixel.setPixelColor(0, color);
    fyPixel.show();
}

// Detection: 3 rapid flashes red->pink->red (~750ms total)
static void fyPixelDetection() {
    unsigned long elapsed = millis() - fyPixelAlertStart;
    int flashIdx = elapsed / 250;
    if (flashIdx >= 3) {
        fyPixelAlertMode = false;
        return;
    }
    uint16_t hue = (flashIdx == 1) ? 300 : 0; // pink middle, red bookends
    bool bright = ((elapsed % 250) < 150);
    uint8_t val = bright ? FY_NEOPIXEL_DETECTION_BRIGHTNESS : (FY_NEOPIXEL_BRIGHTNESS / 4);
    fyPixel.setPixelColor(0, fyHsvToRgb(hue, 255, val));
    fyPixel.show();
}

// Device in range: dim steady pink glow (hue 300)
static void fyPixelHeartbeat() {
    fyPixel.setPixelColor(0, fyHsvToRgb(300, 255, 30));
    fyPixel.show();
}

// Dispatcher: called each loop iteration
static void fyUpdatePixel() {
    if (fyPixelAlertMode) {
        fyPixelDetection();
    } else if (fyDeviceInRange) {
        fyPixelHeartbeat();
    } else {
        fyPixelBreathing();
    }
}

// ============================================================================
// DETECTION HELPERS
// ============================================================================

static bool checkMACPrefix(const uint8_t* mac) {
    char mac_str[9];
    snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x", mac[0], mac[1], mac[2]);
    for (size_t i = 0; i < sizeof(mac_prefixes)/sizeof(mac_prefixes[0]); i++) {
        if (strncasecmp(mac_str, mac_prefixes[i], 8) == 0) return true;
    }
    return false;
}

static bool checkDeviceName(const char* name) {
    if (!name || !name[0]) return false;
    for (size_t i = 0; i < sizeof(device_name_patterns)/sizeof(device_name_patterns[0]); i++) {
        if (strcasestr(name, device_name_patterns[i])) return true;
    }
    return false;
}

static bool checkManufacturerID(uint16_t id) {
    for (size_t i = 0; i < sizeof(ble_manufacturer_ids)/sizeof(ble_manufacturer_ids[0]); i++) {
        if (ble_manufacturer_ids[i] == id) return true;
    }
    return false;
}

// ============================================================================
// RAVEN UUID DETECTION
// ============================================================================

static bool checkRavenUUID(const NimBLEAdvertisedDevice* device, char* out_uuid = nullptr) {
    if (!device || !device->haveServiceUUID()) return false;
    int count = device->getServiceUUIDCount();
    if (count == 0) return false;
    for (int i = 0; i < count; i++) {
        NimBLEUUID svc = device->getServiceUUID(i);
        std::string str = svc.toString();
        for (size_t j = 0; j < sizeof(raven_service_uuids)/sizeof(raven_service_uuids[0]); j++) {
            if (strcasecmp(str.c_str(), raven_service_uuids[j]) == 0) {
                if (out_uuid) strncpy(out_uuid, str.c_str(), 40);
                return true;
            }
        }
    }
    return false;
}

static const char* estimateRavenFW(const NimBLEAdvertisedDevice* device) {
    if (!device || !device->haveServiceUUID()) return "?";
    bool has_new_gps = false, has_old_loc = false, has_power = false;
    int count = device->getServiceUUIDCount();
    for (int i = 0; i < count; i++) {
        std::string u = device->getServiceUUID(i).toString();
        if (strcasecmp(u.c_str(), RAVEN_GPS_SERVICE) == 0)          has_new_gps = true;
        if (strcasecmp(u.c_str(), RAVEN_OLD_LOCATION_SERVICE) == 0) has_old_loc = true;
        if (strcasecmp(u.c_str(), RAVEN_POWER_SERVICE) == 0)        has_power = true;
    }
    if (has_old_loc && !has_new_gps) return "1.1.x";
    if (has_new_gps && !has_power)   return "1.2.x";
    if (has_new_gps && has_power)    return "1.3.x";
    return "?";
}

// ============================================================================
// GPS HELPERS (mutex-protected snapshot pattern)
// ============================================================================

// Fast advisory check — safe lock-free (for UI/stats only, don't trust for writes)
static bool fyGPSIsFresh() {
    return fyGPSValid && (millis() - fyGPSLastUpdate < GPS_STALE_MS);
}

static void fyUpdateGpsIndicator() {
    nessoUiSetGpsIndicator(
        fyGPSIsFresh() ? NESSO_GPS_FIX : NESSO_GPS_SEARCHING);
}

// Atomic snapshot: returns true and fills out-params if GPS is fresh & valid.
// Safe to call from BLE callback context — never races with producer.
static bool fyGPSSnapshot(double& lat, double& lon, float& acc) {
    if (!fyGPSMutex) return false;
    if (xSemaphoreTake(fyGPSMutex, pdMS_TO_TICKS(20)) != pdTRUE) return false;
    bool fresh = fyGPSValid && (millis() - fyGPSLastUpdate < GPS_STALE_MS);
    if (fresh) { lat = fyGPSLat; lon = fyGPSLon; acc = fyGPSAcc; }
    xSemaphoreGive(fyGPSMutex);
    return fresh;
}

// Atomic GPS publish (hardware OR phone). fromHardware=true sticky-locks out phone GPS.
static void fyGPSUpdate(double lat, double lon, float acc, bool fromHardware) {
    if (!fyGPSMutex) return;
    if (xSemaphoreTake(fyGPSMutex, pdMS_TO_TICKS(20)) != pdTRUE) return;
    fyGPSLat = lat;
    fyGPSLon = lon;
    fyGPSAcc = acc;
    fyGPSValid = true;
    fyGPSLastUpdate = millis();
    fyGPSIsHardware = fromHardware;
    xSemaphoreGive(fyGPSMutex);
    fyUpdateGpsIndicator();
}

// Stamp a detection with current GPS if available (used at first-sight and re-sight)
static void fyAttachGPS(FYDetection& d) {
    double lat, lon;
    float acc;
    if (fyGPSSnapshot(lat, lon, acc)) {
        d.hasGPS = true;
        d.gpsLat = lat;
        d.gpsLon = lon;
        d.gpsAcc = acc;
    }
}

// Periodic: back-fill GPS on detections recorded before a fix was available.
// Runs in main loop — MUST NOT be called from BLE callback (takes fyMutex).
static void fyBackfillGPS() {
    double lat, lon;
    float acc;
    if (!fyGPSSnapshot(lat, lon, acc)) return;
    if (!fyMutex || xSemaphoreTake(fyMutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
    int filled = 0;
    for (int i = 0; i < fyDetCount; i++) {
        if (!fyDet[i].hasGPS) {
            fyDet[i].hasGPS = true;
            fyDet[i].gpsLat = lat;
            fyDet[i].gpsLon = lon;
            fyDet[i].gpsAcc = acc;
            filled++;
        }
    }
    xSemaphoreGive(fyMutex);
    if (filled) printf("[FLOCK-YOU] GPS backfilled %d detection(s)\n", filled);
}

// ============================================================================
// CRC32 (IEEE 802.3) — for session file integrity
// ============================================================================

static uint32_t fyCRC32Update(uint32_t crc, const uint8_t* data, size_t len) {
    crc = ~crc;
    while (len--) {
        crc ^= *data++;
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320UL & -(crc & 1));
    }
    return ~crc;
}

// ============================================================================
// HARDWARE GPS PROCESSING
// ============================================================================

static void fyProcessHardwareGPS() {
    // Read all available UART bytes into TinyGPSPlus parser
    while (fyGPSSerial.available()) {
        char c = fyGPSSerial.read();
        fyGPS.encode(c);
        fyHWGPSLastChar = millis();
        if (!fyHWGPSDetected) {
            fyHWGPSDetected = true;
            printf("[FLOCK-YOU] Hardware GPS module detected (NMEA data received)\n");
        }
    }

    // Timeout: no NMEA data for 5s → module disconnected or absent
    if (fyHWGPSDetected && (millis() - fyHWGPSLastChar > GPS_HW_TIMEOUT_MS)) {
        if (fyGPSIsHardware) {
            printf("[FLOCK-YOU] Hardware GPS timeout — falling back to phone GPS\n");
        }
        fyHWGPSDetected = false;
        fyHWGPSFix = false;
        fyHWGPSSats = 0;
        fyGPSIsHardware = false;
    }

    // Update satellite count whenever available
    if (fyGPS.satellites.isUpdated()) {
        fyHWGPSSats = fyGPS.satellites.value();
    }

    // Update position when valid fix is available (atomic publish under fyGPSMutex)
    if (fyGPS.location.isUpdated() && fyGPS.location.isValid()) {
        if (!fyHWGPSFix) {
            printf("[FLOCK-YOU] First GPS fix acquired! Sats:%d Lat:%.6f Lon:%.6f\n",
                   fyHWGPSSats, fyGPS.location.lat(), fyGPS.location.lng());
        }
        fyHWGPSFix = true;
        float acc = fyGPS.hdop.isValid()
            ? (float)(fyGPS.hdop.hdop() * GPS_HDOP_SCALE) : 10.0f;
        fyGPSUpdate(fyGPS.location.lat(), fyGPS.location.lng(), acc, /*fromHardware=*/true);
    } else if (fyHWGPSFix && fyGPS.location.isValid()) {
        // Keep timestamp fresh while fix held (bump under lock via fyGPSUpdate)
        if (fyGPSMutex && xSemaphoreTake(fyGPSMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            fyGPSLastUpdate = millis();
            xSemaphoreGive(fyGPSMutex);
        }
    }

    boardGpsClockSyncFromParser(fyGPS);
    fyUpdateGpsIndicator();
}

static void fyProcessDashboardHardwareGPS() {
    unsigned long now = millis();
    if (now - fyLastDashboardGpsPollMs < GPS_DASHBOARD_POLL_MS) {
        return;
    }
    fyLastDashboardGpsPollMs = now;
    fyProcessHardwareGPS();
}

static uint64_t fyExportMs(unsigned long uptimeMs) {
    return boardGpsUptimeToEpochMs(uptimeMs);
}

// ============================================================================
// DETECTION MANAGEMENT
// ============================================================================

static int fyAddDetection(const char* mac, const char* name, int rssi,
                          const char* method, bool isRaven = false,
                          const char* ravenFW = "", uint8_t channel = 0) {
    if (!fyMutex || xSemaphoreTake(fyMutex, pdMS_TO_TICKS(100)) != pdTRUE) return -1;

    // Update existing by MAC
    for (int i = 0; i < fyDetCount; i++) {
        if (strcasecmp(fyDet[i].mac, mac) == 0) {
            fyDet[i].count++;
            fyDet[i].lastSeen = millis();
            fyDet[i].rssi = rssi;
            if (channel > 0) {
                fyDet[i].channel = channel;
            }
            if (name && name[0]) {
                strncpy(fyDet[i].name, name, sizeof(fyDet[i].name) - 1);
            }
            // Update GPS on every re-sighting (captures movement)
            fyAttachGPS(fyDet[i]);
            xSemaphoreGive(fyMutex);
            return i;
        }
    }

    // Add new
    if (fyDetCount < MAX_DETECTIONS) {
        FYDetection& d = fyDet[fyDetCount];
        memset(&d, 0, sizeof(d));
        strncpy(d.mac, mac, sizeof(d.mac) - 1);
        // Sanitize name for JSON safety
        if (name) {
            for (int j = 0; j < (int)sizeof(d.name) - 1 && name[j]; j++) {
                d.name[j] = (name[j] == '"' || name[j] == '\\') ? '_' : name[j];
            }
        }
        d.rssi = rssi;
        d.channel = channel;
        strncpy(d.method, method, sizeof(d.method) - 1);
        d.firstSeen = millis();
        d.lastSeen = millis();
        d.count = 1;
        d.isRaven = isRaven;
        strncpy(d.ravenFW, ravenFW ? ravenFW : "", sizeof(d.ravenFW) - 1);
        // Attach GPS from phone
        fyAttachGPS(d);
        int idx = fyDetCount++;
        nessoUiSetDetectionCount(fyDetCount);
        xSemaphoreGive(fyMutex);
        return idx;
    }

    xSemaphoreGive(fyMutex);
    return -1;
}

static void fyEmitDetectionSideEffects(int idx, const char* addrStr,
                                       const char* name, int rssi,
                                       const char* method, bool isRaven,
                                       const char* ravenFW, uint8_t channel) {
    printf("[FLOCK-YOU] DETECTED: %s %s RSSI:%d [%s] count:%d\n",
           addrStr, name ? name : "", rssi, method,
           idx >= 0 ? fyDet[idx].count : 0);

    char gpsBuf[80] = "";
    {
        double sLat, sLon; float sAcc;
        if (fyGPSSnapshot(sLat, sLon, sAcc)) {
            snprintf(gpsBuf, sizeof(gpsBuf),
                ",\"gps\":{\"latitude\":%.8f,\"longitude\":%.8f,\"accuracy\":%.1f}",
                sLat, sLon, sAcc);
        }
    }
    if (channel > 0) {
        uint16_t freq = (channel >= 1 && channel <= 14) ? (uint16_t)(2407 + 5 * channel) : 0;
        printf("{\"detection_method\":\"%s\",\"protocol\":\"wifi_2_4ghz\","
               "\"mac_address\":\"%s\",\"device_name\":\"%s\","
               "\"rssi\":%d,\"channel\":%u,\"frequency\":%u%s}\n",
               method, addrStr, name ? name : "", rssi,
               (unsigned)channel, (unsigned)freq, gpsBuf);
    } else if (isRaven) {
        printf("{\"detection_method\":\"%s\",\"protocol\":\"bluetooth_le\","
               "\"mac_address\":\"%s\",\"device_name\":\"%s\","
               "\"rssi\":%d,\"is_raven\":true,\"raven_fw\":\"%s\"%s}\n",
               method, addrStr, name ? name : "", rssi, ravenFW, gpsBuf);
    } else {
        printf("{\"detection_method\":\"%s\",\"protocol\":\"bluetooth_le\","
               "\"mac_address\":\"%s\",\"device_name\":\"%s\","
               "\"rssi\":%d%s}\n",
               method, addrStr, name ? name : "", rssi, gpsBuf);
    }

    if (!fyTriggered) {
        fyTriggered = true;
        fyDetectBeep();
    }
    fyDeviceInRange = true;
    fyLastDetTime = millis();
    fyLastHB = millis();
}

static void fyProcessWiFiAlerts() {
    FyWiFiAlert alert;
    while (fyWiFiSniffPopAlert(&alert)) {
        char macStr[18];
        snprintf(macStr, sizeof(macStr), "%02x:%02x:%02x:%02x:%02x:%02x",
                 alert.mac[0], alert.mac[1], alert.mac[2],
                 alert.mac[3], alert.mac[4], alert.mac[5]);
        const char* method = fyWiFiAlertMethod(alert.type);
        int idx = fyAddDetection(macStr, "", alert.rssi, method, false, "",
                                 alert.channel);
        fyEmitDetectionSideEffects(idx, macStr, "", alert.rssi, method,
                                   false, "", alert.channel);
    }
}

// ============================================================================
// BLE SCANNING
// ============================================================================

class FYBLECallbacks : public NimBLEAdvertisedDeviceCallbacks {
    void onResult(const NimBLEAdvertisedDevice* dev) override {
        NimBLEAddress addr = dev->getAddress();
        std::string addrStr = addr.toString();

        // Safe MAC byte extraction
        unsigned int m[6];
        sscanf(addrStr.c_str(), "%02x:%02x:%02x:%02x:%02x:%02x",
               &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]);
        uint8_t mac[6] = {(uint8_t)m[0], (uint8_t)m[1], (uint8_t)m[2],
                          (uint8_t)m[3], (uint8_t)m[4], (uint8_t)m[5]};

        int rssi = dev->getRSSI();
        std::string name = dev->haveName() ? dev->getName() : "";

        bool detected = false;
        const char* method = "";
        bool isRaven = false;
        const char* ravenFW = "";

        // 1. Check MAC prefix against known Flock Safety OUIs
        if (checkMACPrefix(mac)) {
            detected = true;
            method = "mac_prefix";
        }

        // 2. Check BLE device name patterns
        if (!detected && !name.empty() && checkDeviceName(name.c_str())) {
            detected = true;
            method = "device_name";
        }

        // 3. Check BLE manufacturer company IDs (from wgreenberg/flock-you)
        if (!detected) {
            for (int i = 0; i < (int)dev->getManufacturerDataCount(); i++) {
                std::string data = dev->getManufacturerData(i);
                if (data.size() >= 2) {
                    uint16_t code = ((uint16_t)(uint8_t)data[1] << 8) |
                                     (uint16_t)(uint8_t)data[0];
                    if (checkManufacturerID(code)) {
                        detected = true;
                        method = "ble_mfr_id";
                        break;
                    }
                }
            }
        }

        // 4. Check Raven gunshot detector service UUIDs
        if (!detected) {
            char detUUID[41] = {0};
            if (checkRavenUUID(dev, detUUID)) {
                detected = true;
                method = "raven_uuid";
                isRaven = true;
                ravenFW = estimateRavenFW(dev);
            }
        }

        if (detected) {
            int idx = fyAddDetection(addrStr.c_str(), name.c_str(), rssi,
                                     method, isRaven, ravenFW, 0);
            fyEmitDetectionSideEffects(idx, addrStr.c_str(), name.c_str(), rssi,
                                       method, isRaven, ravenFW, 0);
        }
    }
};

// ============================================================================
// JSON HELPER
// ============================================================================

static void writeDetectionsJSON(AsyncResponseStream *resp) {
    resp->print("[");
    if (fyMutex && xSemaphoreTake(fyMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        for (int i = 0; i < fyDetCount; i++) {
            if (i > 0) resp->print(",");
            resp->printf(
                "{\"mac\":\"%s\",\"name\":\"%s\",\"rssi\":%d,\"method\":\"%s\","
                "\"first\":%" PRIu64 ",\"last\":%" PRIu64 ",\"count\":%d,"
                "\"raven\":%s,\"fw\":\"%s\",\"channel\":%u",
                fyDet[i].mac, fyDet[i].name, fyDet[i].rssi, fyDet[i].method,
                fyExportMs(fyDet[i].firstSeen), fyExportMs(fyDet[i].lastSeen), fyDet[i].count,
                fyDet[i].isRaven ? "true" : "false", fyDet[i].ravenFW,
                (unsigned)fyDet[i].channel);
            // Append GPS if present
            if (fyDet[i].hasGPS) {
                resp->printf(",\"gps\":{\"lat\":%.8f,\"lon\":%.8f,\"acc\":%.1f}",
                    fyDet[i].gpsLat, fyDet[i].gpsLon, fyDet[i].gpsAcc);
            }
            resp->print("}");
        }
        xSemaphoreGive(fyMutex);
    }
    resp->print("]");
}

// ============================================================================
// SESSION PERSISTENCE (SPIFFS) — bulletproof envelope format
// ============================================================================
//
// Wire format on disk:
//   Line 1: {"v":1,"count":N,"bytes":B,"crc":"0xXXXXXXXX"}\n
//   Line 2+: [{"mac":...},{"mac":...},...]    (exactly B bytes, CRC32 == X)
//
// Atomic write procedure:
//   1. Compute size+CRC over the detections payload (pass 1, under fyMutex)
//   2. Write envelope header + payload to /session.tmp (pass 2, under same lock)
//   3. Remove /session.json
//   4. Rename /session.tmp → /session.json (with copy+delete fallback)
//
// Recovery: if /session.json is missing or CRC-invalid, fall back to /session.tmp.
// If power fails between steps 3 and 4, fyPromotePrevSession() recovers from tmp.

#define FY_SESSION_TMP "/session.tmp"

// Serialize a single detection to `dst`. Returns bytes written (0 on overflow).
static size_t fySerializeDet(const FYDetection& d, char* dst, size_t cap) {
    int n;
    if (d.hasGPS) {
        n = snprintf(dst, cap,
            "{\"mac\":\"%s\",\"name\":\"%s\",\"rssi\":%d,\"method\":\"%s\","
            "\"first\":%" PRIu64 ",\"last\":%" PRIu64 ",\"count\":%d,"
            "\"raven\":%s,\"fw\":\"%s\",\"channel\":%u,"
            "\"gps\":{\"lat\":%.8f,\"lon\":%.8f,\"acc\":%.1f}}",
            d.mac, d.name, d.rssi, d.method,
            fyExportMs(d.firstSeen), fyExportMs(d.lastSeen), d.count,
            d.isRaven ? "true" : "false", d.ravenFW,
            (unsigned)d.channel,
            d.gpsLat, d.gpsLon, d.gpsAcc);
    } else {
        n = snprintf(dst, cap,
            "{\"mac\":\"%s\",\"name\":\"%s\",\"rssi\":%d,\"method\":\"%s\","
            "\"first\":%" PRIu64 ",\"last\":%" PRIu64 ",\"count\":%d,"
            "\"raven\":%s,\"fw\":\"%s\",\"channel\":%u}",
            d.mac, d.name, d.rssi, d.method,
            fyExportMs(d.firstSeen), fyExportMs(d.lastSeen), d.count,
            d.isRaven ? "true" : "false", d.ravenFW,
            (unsigned)d.channel);
    }
    return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

// Pass 1: compute exact payload size + CRC32 without allocating.
// Caller MUST hold fyMutex.
static uint32_t fyComputePayloadCRC(size_t& outBytes) {
    char line[512];
    uint32_t crc = 0;  // seed for fyCRC32Update
    outBytes = 0;
    crc = fyCRC32Update(crc, (const uint8_t*)"[", 1); outBytes += 1;
    for (int i = 0; i < fyDetCount; i++) {
        if (i > 0) { crc = fyCRC32Update(crc, (const uint8_t*)",", 1); outBytes += 1; }
        size_t n = fySerializeDet(fyDet[i], line, sizeof(line));
        if (n == 0) continue;  // overflow — skip (truncation is safer than partial)
        crc = fyCRC32Update(crc, (const uint8_t*)line, n);
        outBytes += n;
    }
    crc = fyCRC32Update(crc, (const uint8_t*)"]", 1); outBytes += 1;
    return crc;
}

// Validate a session file envelope and its payload CRC. Returns true if intact.
// Optionally outputs payload byte-offset and length.
static bool fyValidateSessionFile(const char* path,
                                   size_t* outBodyOffset = nullptr,
                                   size_t* outBodyBytes = nullptr) {
    if (!SPIFFS.exists(path)) return false;
    File f = SPIFFS.open(path, "r");
    if (!f) return false;

    String hdr = f.readStringUntil('\n');
    if (hdr.length() < 10 || hdr[0] != '{') { f.close(); return false; }

    JsonDocument doc;
    if (deserializeJson(doc, hdr) != DeserializationError::Ok) { f.close(); return false; }
    if ((int)(doc["v"] | 0) != 1) { f.close(); return false; }
    size_t expectedBytes = (size_t)(doc["bytes"] | 0);
    uint32_t expectedCRC = 0;
    const char* crcStr = doc["crc"] | "";
    if (sscanf(crcStr, "%x", &expectedCRC) != 1) { f.close(); return false; }

    size_t bodyOffset = hdr.length() + 1;  // + '\n'
    size_t fileSize = f.size();
    if (fileSize < bodyOffset + expectedBytes) { f.close(); return false; }
    size_t actualBytes = fileSize - bodyOffset;
    if (actualBytes != expectedBytes) { f.close(); return false; }

    uint8_t buf[256];
    uint32_t crc = 0;
    size_t remaining = expectedBytes;
    while (remaining > 0) {
        int n = f.read(buf, remaining < sizeof(buf) ? remaining : sizeof(buf));
        if (n <= 0) break;
        crc = fyCRC32Update(crc, buf, (size_t)n);
        remaining -= (size_t)n;
    }
    f.close();

    if (remaining != 0 || crc != expectedCRC) return false;
    if (outBodyOffset) *outBodyOffset = bodyOffset;
    if (outBodyBytes)  *outBodyBytes  = expectedBytes;
    return true;
}

// Copy src→dst in chunks. Returns true on success (both files existed & read/write ok).
static bool fySpiffsCopy(const char* src, const char* dst) {
    File s = SPIFFS.open(src, "r");
    if (!s) return false;
    File d = SPIFFS.open(dst, "w");
    if (!d) { s.close(); return false; }
    uint8_t buf[256];
    int n;
    bool ok = true;
    while ((n = s.read(buf, sizeof(buf))) > 0) {
        if (d.write(buf, (size_t)n) != (size_t)n) { ok = false; break; }
    }
    s.close();
    d.close();
    return ok;
}

// Atomic rename: try SPIFFS rename first, fall back to copy+delete if rename fails.
static bool fyAtomicPromote(const char* tmp, const char* final) {
    if (SPIFFS.rename(tmp, final)) return true;
    if (!fySpiffsCopy(tmp, final)) return false;
    SPIFFS.remove(tmp);
    return true;
}

static void fySaveSession() {
    if (!fySpiffsReady || !fyMutex) return;
    if (xSemaphoreTake(fyMutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        printf("[FLOCK-YOU] Save skipped: fyMutex busy\n");
        return;
    }

    // Pass 1: compute CRC + byte count (no allocation)
    size_t payloadBytes = 0;
    uint32_t crc = fyComputePayloadCRC(payloadBytes);
    int savedCount = fyDetCount;

    // Pass 2: write envelope + payload to tmp file
    File f = SPIFFS.open(FY_SESSION_TMP, "w");
    if (!f) {
        xSemaphoreGive(fyMutex);
        printf("[FLOCK-YOU] Save failed: cannot open %s\n", FY_SESSION_TMP);
        return;
    }
    f.printf("{\"v\":1,\"count\":%d,\"bytes\":%u,\"crc\":\"0x%08lX\"}\n",
             savedCount, (unsigned)payloadBytes, (unsigned long)crc);

    char line[512];
    size_t wrote = 0;
    f.write((uint8_t*)"[", 1); wrote++;
    for (int i = 0; i < fyDetCount; i++) {
        if (i > 0) { f.write((uint8_t*)",", 1); wrote++; }
        size_t n = fySerializeDet(fyDet[i], line, sizeof(line));
        if (n == 0) continue;
        f.write((uint8_t*)line, n);
        wrote += n;
    }
    f.write((uint8_t*)"]", 1); wrote++;
    f.close();
    xSemaphoreGive(fyMutex);

    if (wrote != payloadBytes) {
        printf("[FLOCK-YOU] Save WARNING: wrote %u expected %u — aborting promote\n",
               (unsigned)wrote, (unsigned)payloadBytes);
        return;  // keep old session.json intact
    }

    // Verify the tmp file is intact before promoting
    if (!fyValidateSessionFile(FY_SESSION_TMP)) {
        printf("[FLOCK-YOU] Save verify FAILED — aborting promote (old session preserved)\n");
        return;
    }

    // Atomic promote: remove old, then rename tmp → session.json
    SPIFFS.remove(FY_SESSION_FILE);
    if (!fyAtomicPromote(FY_SESSION_TMP, FY_SESSION_FILE)) {
        printf("[FLOCK-YOU] Promote FAILED — data in %s for recovery\n", FY_SESSION_TMP);
        return;
    }

    fyLastSaveCount = savedCount;
    printf("[FLOCK-YOU] Session saved: %d det, %u bytes, crc=0x%08lX\n",
           savedCount, (unsigned)payloadBytes, (unsigned long)crc);
}

static void fyPromotePrevSession() {
    if (!fySpiffsReady) return;

    const char* source = nullptr;
    if (fyValidateSessionFile(FY_SESSION_FILE)) {
        source = FY_SESSION_FILE;
    } else if (fyValidateSessionFile(FY_SESSION_TMP)) {
        printf("[FLOCK-YOU] Main session corrupt/missing — recovering from tmp\n");
        source = FY_SESSION_TMP;
    } else {
        // Legacy fallback: old format (no envelope, raw array)
        if (SPIFFS.exists(FY_SESSION_FILE)) {
            File f = SPIFFS.open(FY_SESSION_FILE, "r");
            if (f && f.size() > 2) {
                int first = f.peek();
                f.close();
                if (first == '[') {
                    source = FY_SESSION_FILE;
                    printf("[FLOCK-YOU] Legacy-format session detected — promoting\n");
                }
            } else if (f) { f.close(); }
        }
    }

    if (!source) {
        if (SPIFFS.exists(FY_SESSION_FILE)) SPIFFS.remove(FY_SESSION_FILE);
        if (SPIFFS.exists(FY_SESSION_TMP))  SPIFFS.remove(FY_SESSION_TMP);
        printf("[FLOCK-YOU] No valid prior session to promote\n");
        return;
    }

    // Copy source → prev_session.json (same format as stored — envelope or legacy)
    if (!fySpiffsCopy(source, FY_PREV_FILE)) {
        printf("[FLOCK-YOU] Failed to promote %s → %s\n", source, FY_PREV_FILE);
        return;
    }

    // Clean up: clear session files so they don't get re-promoted next boot
    if (SPIFFS.exists(FY_SESSION_FILE)) SPIFFS.remove(FY_SESSION_FILE);
    if (SPIFFS.exists(FY_SESSION_TMP))  SPIFFS.remove(FY_SESSION_TMP);

    File v = SPIFFS.open(FY_PREV_FILE, "r");
    size_t sz = v ? v.size() : 0;
    if (v) v.close();
    printf("[FLOCK-YOU] Prior session promoted from %s (%u bytes)\n", source, (unsigned)sz);
}

// Read prev_session as a raw detection JSON array (strips envelope header if present).
// Streams into the response to avoid buffering the whole file in RAM.
static void fyStreamPrevSessionBody(AsyncResponseStream* resp) {
    if (!fySpiffsReady || !SPIFFS.exists(FY_PREV_FILE)) { resp->print("[]"); return; }
    File f = SPIFFS.open(FY_PREV_FILE, "r");
    if (!f) { resp->print("[]"); return; }

    // Detect envelope vs legacy raw-array
    int first = f.peek();
    if (first == '{') {
        // Envelope: skip header line
        f.readStringUntil('\n');
    }

    uint8_t buf[256];
    int n;
    size_t streamed = 0;
    while ((n = f.read(buf, sizeof(buf))) > 0) {
        resp->write(buf, (size_t)n);
        streamed += (size_t)n;
    }
    f.close();
    if (streamed == 0) resp->print("[]");
}

// ============================================================================
// KML EXPORT
// ============================================================================

static void writeDetectionsKML(AsyncResponseStream *resp) {
    resp->print("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                "<kml xmlns=\"http://www.opengis.net/kml/2.2\">\n<Document>\n"
                "<name>Flock-You Detections</name>\n"
                "<description>Surveillance device detections with GPS</description>\n");

    // Detection pin style
    resp->print("<Style id=\"det\"><IconStyle><color>ff4489ec</color>"
                "<scale>1.0</scale></IconStyle></Style>\n"
                "<Style id=\"raven\"><IconStyle><color>ff4444ef</color>"
                "<scale>1.2</scale></IconStyle></Style>\n");

    if (fyMutex && xSemaphoreTake(fyMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        for (int i = 0; i < fyDetCount; i++) {
            FYDetection& d = fyDet[i];
            if (!d.hasGPS) continue;  // Skip detections without GPS
            resp->print("<Placemark>\n");
            resp->printf("<name>%s</name>\n", d.mac);
            resp->printf("<styleUrl>#%s</styleUrl>\n", d.isRaven ? "raven" : "det");
            resp->print("<description><![CDATA[");
            if (d.name[0]) resp->printf("<b>Name:</b> %s<br/>", d.name);
            resp->printf("<b>Method:</b> %s<br/>"
                         "<b>RSSI:</b> %d dBm<br/>"
                         "<b>Count:</b> %d<br/>",
                         d.method, d.rssi, d.count);
            if (d.isRaven) resp->printf("<b>Raven FW:</b> %s<br/>", d.ravenFW);
            resp->printf("<b>Accuracy:</b> %.1f m", d.gpsAcc);
            resp->print("]]></description>\n");
            resp->printf("<Point><coordinates>%.8f,%.8f,0</coordinates></Point>\n",
                         d.gpsLon, d.gpsLat);
            resp->print("</Placemark>\n");
        }
        xSemaphoreGive(fyMutex);
    }
    resp->print("</Document>\n</kml>");
}

// ============================================================================
// DASHBOARD HTML
// ============================================================================

static const char FY_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no">
<title>FLOCK-YOU</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
html,body{height:100%;overflow:hidden}
body{font-family:'Courier New',monospace;background:#0a0012;color:#e0e0e0;display:flex;flex-direction:column}
.hd{background:#1a0033;padding:10px 14px;border-bottom:2px solid #ec4899;flex-shrink:0}
.hd h1{font-size:22px;color:#ec4899;letter-spacing:3px}
.hd .sub{font-size:11px;color:#8b5cf6;margin-top:2px}
.st{display:flex;gap:8px;padding:8px 12px;background:rgba(139,92,246,.08);border-bottom:1px solid rgba(139,92,246,.19);flex-shrink:0}
.sc{flex:1;text-align:center;padding:6px;border:1px solid rgba(139,92,246,.25);border-radius:5px}
.sc .n{font-size:22px;font-weight:bold;color:#ec4899}
.sc .l{font-size:10px;color:#8b5cf6;margin-top:2px}
.tb{display:flex;border-bottom:1px solid #8b5cf6;flex-shrink:0}
.tb button{flex:1;padding:9px;text-align:center;cursor:pointer;color:#8b5cf6;border:none;background:none;font-family:inherit;font-size:13px;font-weight:bold;letter-spacing:1px}
.tb button.a{color:#ec4899;border-bottom:2px solid #ec4899;background:rgba(236,72,153,.08)}
.cn{flex:1;overflow-y:auto;padding:10px}
.pn{display:none}.pn.a{display:block}
.det{background:rgba(45,27,105,.4);border:1px solid rgba(139,92,246,.25);border-radius:7px;padding:10px;margin-bottom:8px}
.det .mac{color:#ec4899;font-weight:bold;font-size:14px}
.det .nm{color:#c084fc;font-size:13px;margin-left:4px}
.det .inf{display:flex;flex-wrap:wrap;gap:5px;margin-top:5px;font-size:12px}
.det .inf span{background:rgba(139,92,246,.15);padding:3px 6px;border-radius:4px}
.det .rv{background:rgba(239,68,68,.15)!important;color:#ef4444;font-weight:bold}
.pg{margin-bottom:12px}
.pg h3{color:#ec4899;font-size:14px;margin-bottom:4px;border-bottom:1px solid rgba(139,92,246,.19);padding-bottom:4px}
.pg .it{display:flex;flex-wrap:wrap;gap:4px;font-size:12px}
.pg .it span{background:rgba(139,92,246,.15);padding:3px 6px;border-radius:4px;border:1px solid rgba(139,92,246,.12)}
.btn{display:block;width:100%;padding:10px;margin-bottom:8px;background:#8b5cf6;color:#fff;border:none;border-radius:5px;cursor:pointer;font-family:inherit;font-size:14px;font-weight:bold}
.btn:active{background:#ec4899}
.btn.dng{background:#ef4444}
.empty{text-align:center;color:rgba(139,92,246,.5);padding:28px;font-size:14px}
.sep{border:none;border-top:1px solid rgba(139,92,246,.12);margin:12px 0}
h4{color:#ec4899;font-size:14px;margin-bottom:8px}
</style></head><body>
<div class="hd"><h1>FLOCK-YOU</h1><div class="sub">Surveillance Device Detector &bull; Wardriving + GPS</div></div>
<div class="st">
<div class="sc"><div class="n" id="sT">0</div><div class="l">DETECTED</div></div>
<div class="sc"><div class="n" id="sR">0</div><div class="l">RAVEN</div></div>
<div class="sc"><div class="n" id="sB">ON</div><div class="l">BLE</div></div>
<div class="sc" onclick="reqGPS()" style="cursor:pointer"><div class="n" id="sG" style="font-size:14px">TAP</div><div class="l" id="sGL">GPS</div></div>
</div>
<div class="tb">
<button class="a" onclick="tab(0,this)">LIVE</button>
<button onclick="tab(1,this)">PREV</button>
<button onclick="tab(2,this)">DB</button>
<button onclick="tab(3,this)">TOOLS</button>
</div>
<div class="cn">
<div class="pn a" id="p0">
<div id="dL"><div class="empty">Scanning for surveillance devices...<br>BLE active on all channels</div></div>
</div>
<div class="pn" id="p1"><div id="hL"><div class="empty">Loading prior session...</div></div></div>
<div class="pn" id="p2"><div id="pC">Loading patterns...</div></div>
<div class="pn" id="p3">
<h4>EXPORT DETECTIONS</h4>
<p style="font-size:10px;color:#8b5cf6;margin-bottom:8px">Download current session to import into Flask dashboard</p>
<button class="btn" onclick="location.href='/api/export/json'">DOWNLOAD JSON</button>
<button class="btn" onclick="location.href='/api/export/csv'">DOWNLOAD CSV</button>
<button class="btn" onclick="location.href='/api/export/kml'" style="background:#22c55e">DOWNLOAD KML (GPS MAP)</button>
<hr class="sep">
<h4>PRIOR SESSION</h4>
<button class="btn" onclick="location.href='/api/history/json'" style="background:#6366f1">DOWNLOAD PREV JSON</button>
<button class="btn" onclick="location.href='/api/history/kml'" style="background:#22c55e">DOWNLOAD PREV KML</button>
<hr class="sep">
<button class="btn dng" onclick="if(confirm('Clear all detections?'))fetch('/api/clear').then(()=>refresh())">CLEAR ALL DETECTIONS</button>
</div>
</div>
<script>
let D=[],H=[];
function tab(i,el){document.querySelectorAll('.tb button').forEach(b=>b.classList.remove('a'));document.querySelectorAll('.pn').forEach(p=>p.classList.remove('a'));el.classList.add('a');document.getElementById('p'+i).classList.add('a');if(i===1&&!window._hL)loadHistory();if(i===2&&!window._pL)loadPat();}
function refresh(){fetch('/api/detections').then(r=>r.json()).then(d=>{D=d;render();stats();}).catch(()=>{});}
function render(){const el=document.getElementById('dL');if(!D.length){el.innerHTML='<div class="empty">Scanning for surveillance devices...<br>BLE active on all channels</div>';return;}
D.sort((a,b)=>b.last-a.last);el.innerHTML=D.map(card).join('');}
function stats(){document.getElementById('sT').textContent=D.length;document.getElementById('sR').textContent=D.filter(d=>d.raven).length;
fetch('/api/stats').then(r=>r.json()).then(s=>{let g=document.getElementById('sG'),gl=document.getElementById('sGL');if(s.gps_src==='hw'){g.textContent=s.gps_sats+'sat';g.style.color='#22c55e';gl.textContent='HW GPS';}else if(s.gps_src==='phone'){g.textContent=s.gps_tagged+'/'+s.total;g.style.color='#22c55e';gl.textContent='PHONE';}else if(s.gps_hw_detected){g.textContent=s.gps_sats+'sat';g.style.color='#facc15';gl.textContent='NO FIX';}else{g.textContent='TAP';g.style.color='#ef4444';gl.textContent='GPS';}}).catch(()=>{});}
function card(d){return '<div class="det"><div class="mac">'+d.mac+(d.name?'<span class="nm">'+d.name+'</span>':'')+'</div><div class="inf"><span>RSSI: '+d.rssi+'</span><span>'+d.method+'</span><span style="color:#ec4899;font-weight:bold">&times;'+d.count+'</span>'+(d.raven?'<span class="rv">RAVEN '+d.fw+'</span>':'')+(d.gps?'<span style="color:#22c55e">&#9673; '+d.gps.lat.toFixed(5)+','+d.gps.lon.toFixed(5)+'</span>':'<span style="color:#666">no gps</span>')+'</div></div>';}
function loadHistory(){fetch('/api/history').then(r=>r.json()).then(d=>{H=d;let el=document.getElementById('hL');if(!H.length){el.innerHTML='<div class="empty">No prior session data</div>';return;}
H.sort((a,b)=>b.last-a.last);el.innerHTML='<div style="font-size:11px;color:#8b5cf6;margin-bottom:8px">'+H.length+' detections from prior session</div>'+H.map(card).join('');window._hL=1;}).catch(()=>{document.getElementById('hL').innerHTML='<div class="empty">No prior session data</div>';});}
function loadPat(){fetch('/api/patterns').then(r=>r.json()).then(p=>{let h='';
h+='<div class="pg"><h3>MAC Prefixes ('+p.macs.length+')</h3><div class="it">'+p.macs.map(m=>'<span>'+m+'</span>').join('')+'</div></div>';
h+='<div class="pg"><h3>BLE Device Names ('+p.names.length+')</h3><div class="it">'+p.names.map(n=>'<span>'+n+'</span>').join('')+'</div></div>';
h+='<div class="pg"><h3>BLE Manufacturer IDs ('+p.mfr.length+')</h3><div class="it">'+p.mfr.map(m=>'<span>0x'+m.toString(16).toUpperCase().padStart(4,'0')+'</span>').join('')+'</div></div>';
h+='<div class="pg"><h3>Raven UUIDs ('+p.raven.length+')</h3><div class="it">'+p.raven.map(u=>'<span style="font-size:8px">'+u+'</span>').join('')+'</div></div>';
document.getElementById('pC').innerHTML=h;window._pL=1;}).catch(()=>{});}
// GPS from phone -> ESP32 (wardriving)
// NOTE: Geolocation API needs secure context (HTTPS) on most browsers.
// HTTP works on: Android Chrome (local IPs), some Android browsers.
// Won't work on: iOS Safari (needs HTTPS always).
// We only request on user tap (gesture) for best permission prompt chance.
let _gW=null,_gOk=false,_gTried=false;
function sendGPS(p){_gOk=true;let g=document.getElementById('sG');g.textContent='OK';g.style.color='#22c55e';
fetch('/api/gps?lat='+p.coords.latitude+'&lon='+p.coords.longitude+'&acc='+(p.coords.accuracy||0)).catch(()=>{});}
function gpsErr(e){_gOk=false;let g=document.getElementById('sG');
var msg='ERR';if(e.code===1){msg='DENIED';g.style.color='#ef4444';alert('GPS permission denied. On iPhone, GPS requires HTTPS which this device cannot provide. On Android Chrome, tap the lock/info icon in the address bar and allow Location.');}
else if(e.code===2){msg='N/A';g.style.color='#ef4444';}
else if(e.code===3){msg='WAIT';g.style.color='#facc15';}
g.textContent=msg;}
function startGPS(){if(!navigator.geolocation){return false;}
if(_gW!==null){navigator.geolocation.clearWatch(_gW);_gW=null;}
let g=document.getElementById('sG');g.textContent='...';g.style.color='#facc15';
_gW=navigator.geolocation.watchPosition(sendGPS,gpsErr,{enableHighAccuracy:true,maximumAge:5000,timeout:15000});return true;}
function reqGPS(){if(!navigator.geolocation){alert('GPS not available in this browser.');return;}
if(_gOk){return;}
if(!window.isSecureContext){alert('GPS requires a secure context (HTTPS). This HTTP page may not get GPS permission.\\n\\nAndroid Chrome: try chrome://flags and enable "Insecure origins treated as secure", add http://192.168.4.1\\n\\niPhone: GPS will not work over HTTP.');}
startGPS();_gTried=true;}
refresh();setInterval(refresh,2500);
</script></body></html>
)rawliteral";

// ============================================================================
// RADIO PROFILE (collect vs dashboard) + time-sliced scheduler
// ============================================================================

static void fyRegisterRoutes();
static void fyTryStartServer();
static void fyStopDashboardServices();

static const char* fySliceName(FyRadioSlice slice) {
    switch (slice) {
        case FY_SLICE_BLE: return "BLE";
        case FY_SLICE_WIFI_SNIFF: return "WIFI";
        case FY_SLICE_AP: return "AP";
        default: return "?";
    }
}

static uint16_t fySliceDurationMs(FyRadioSlice slice) {
    if (fyRadioProfile == FY_RADIO_COLLECT) {
        if (slice == FY_SLICE_BLE) return FY_COLLECT_BLE_MS;
        if (slice == FY_SLICE_WIFI_SNIFF) return FY_COLLECT_WIFI_MS;
        return 0;
    }
    if (slice == FY_SLICE_BLE) return FY_DASH_BLE_MS;
    if (slice == FY_SLICE_WIFI_SNIFF) return FY_DASH_WIFI_MS;
    return 0;
}

static FyRadioSlice fyNextSlice(FyRadioSlice slice) {
    if (fyRadioProfile == FY_RADIO_COLLECT) {
        return (slice == FY_SLICE_BLE) ? FY_SLICE_WIFI_SNIFF : FY_SLICE_BLE;
    }
    return (slice == FY_SLICE_BLE) ? FY_SLICE_WIFI_SNIFF : FY_SLICE_BLE;
}

static bool fyStartDashboardAp() {
    if (fyDashboardApUp) {
        return true;
    }
    fyWiFiSniffSetApCoexist(false);
    fyWiFiSniffStop();
#ifndef FY_DIAG_DISABLE_BLE
    if (fyBLEScan && fyBLEScan->isScanning()) {
        fyBLEScan->stop();
    }
#endif
    WiFi.mode(WIFI_AP);
    delay(100);
    if (!WiFi.softAP(FY_AP_SSID, FY_AP_PASS)) {
        printf("[FLOCK-YOU] softAP start FAILED\n");
        return false;
    }
    fyApStartMs = millis();
    fyDashboardApUp = true;
    printf("[FLOCK-YOU] softAP up (continuous) %s\n",
           WiFi.softAPIP().toString().c_str());
    return true;
}

static void fyStopDashboardAp() {
    fyStopDashboardServices();
    if (fyDashboardApUp) {
        WiFi.softAPdisconnect(true);
        fyDashboardApUp = false;
    }
    WiFi.mode(WIFI_OFF);
    delay(20);
    fyWiFiSniffSetApCoexist(false);
}

static void fyEnterBleSlice() {
    fyWiFiSniffStop();
    fyWiFiSniffSetApCoexist(false);
#ifndef FY_DIAG_DISABLE_BLE
    if (fyRadioProfile == FY_RADIO_COLLECT) {
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_OFF);
        delay(20);
    }
    if (fyBLEScan) {
        if (fyBLEScan->isScanning()) {
            fyBLEScan->stop();
        }
        fyBLEScan->setInterval(FY_BLE_SLOT_MS);
        if (fyRadioProfile == FY_RADIO_DASHBOARD) {
            fyBLEScan->setWindow(FY_BLE_DASHBOARD_WINDOW_MS);
        } else {
            fyBLEScan->setWindow(FY_BLE_SLOT_MS);
        }
        fyBLEScan->start(0, false);
        fyLastBleScan = millis();
    }
#endif
    printf("[FLOCK-YOU] slice=BLE%s\n",
           fyRadioProfile == FY_RADIO_DASHBOARD ? " (AP up)" : "");
}

static void fyEnterWifiSniffSlice() {
#ifndef FY_DIAG_DISABLE_BLE
    if (fyBLEScan && fyBLEScan->isScanning()) {
        fyBLEScan->stop();
    }
#endif
    if (fyRadioProfile == FY_RADIO_COLLECT) {
        fyStopDashboardServices();
        WiFi.softAPdisconnect(true);
        delay(20);
        fyWiFiSniffSetApCoexist(false);
    } else {
        fyWiFiSniffSetApCoexist(true);
    }
    fyWiFiSniffStart();
    printf("[FLOCK-YOU] slice=WIFI ch=%u%s\n",
           fyWiFiSniffCurrentChannel(),
           fyRadioProfile == FY_RADIO_DASHBOARD ? " (AP up)" : "");
}

static void fyEnterSlice(FyRadioSlice slice) {
    switch (slice) {
        case FY_SLICE_BLE: fyEnterBleSlice(); break;
        case FY_SLICE_WIFI_SNIFF: fyEnterWifiSniffSlice(); break;
        case FY_SLICE_AP: break;  // unused — AP stays up in dashboard
    }
}

static void fyRadioSchedulerReset() {
    fySliceStartMs = millis();
    fyLastSliceTickMs = fySliceStartMs;
    fyCurrentSlice = FY_SLICE_BLE;
    fyEnterSlice(fyCurrentSlice);
}

static void fyRadioSchedulerTick() {
    unsigned long now = millis();

    if (fyCurrentSlice == FY_SLICE_WIFI_SNIFF) {
        fyWiFiSniffTick();
    }

    if (now - fySliceStartMs < fySliceDurationMs(fyCurrentSlice)) {
        return;
    }

    fyCurrentSlice = fyNextSlice(fyCurrentSlice);
    fySliceStartMs = now;
    fyEnterSlice(fyCurrentSlice);
}

static void fyModeConfirmBeep() {
    boardLedcConfigure(BUZZER_FREQ);
    boardBuzzerAttach();
    boardLedcSetDuty(40);
    delay(60);
    boardLedcSetDuty(0);
}

static void fyStopDashboardServices() {
    if (fyServerStarted) {
        fyServer.end();
        fyServerStarted = false;
        printf("[FLOCK-YOU] Web server stopped\n");
    }
    if (fyDnsStarted) {
        flockyouDNS.stop();
        fyDnsStarted = false;
        printf("[FLOCK-YOU] Captive DNS stopped\n");
    }
}

static void fyEnterCollectMode() {
    if (fyRadioProfile == FY_RADIO_COLLECT) {
        return;
    }
    printf("[FLOCK-YOU] COLLECT mode: 50%% BLE / 50%% WiFi sniff\n");
    boardGpsApplyProfile(fyGPSSerial, GPS_PROFILE_COLLECT);
    fyStopDashboardAp();
    fyWiFiSniffStop();
    fyRadioProfile = FY_RADIO_COLLECT;
    fyRadioSchedulerReset();
    nessoUiSetStatus("Collect mode");
    fyModeConfirmBeep();
}

static void fyEnterDashboardMode() {
    if (fyRadioProfile == FY_RADIO_DASHBOARD) {
        return;
    }
    printf("[FLOCK-YOU] DASHBOARD mode: AP always on, 50/50 BLE/WiFi sniff\n");
    boardGpsApplyProfile(fyGPSSerial, GPS_PROFILE_DASHBOARD);
    fyLastDashboardGpsPollMs = millis();
    fyRadioProfile = FY_RADIO_DASHBOARD;
    if (!fyRoutesRegistered) {
        fyRegisterRoutes();
    }
    if (!fyStartDashboardAp()) {
        fyRadioProfile = FY_RADIO_COLLECT;
        nessoUiSetStatus("AP failed");
        return;
    }
    fyRadioSchedulerReset();
    nessoUiSetStatus("Dashboard: flockyou");
    printf("[FLOCK-YOU] AP: %s / %s  http://192.168.4.1\n",
           FY_AP_SSID, FY_AP_PASS);
    fyModeConfirmBeep();
}

static void fyToggleRadioProfile() {
    if (fyRadioProfile == FY_RADIO_COLLECT) {
        fyEnterDashboardMode();
    } else {
        fyEnterCollectMode();
    }
}

static void fyPollKey1DoubleClick() {
    bool down = boardMenuButtonPressed();
    unsigned long now = millis();

    if (down && !fyKey1WasDown) {
        fyKey1WasDown = true;
    } else if (!down && fyKey1WasDown) {
        fyKey1WasDown = false;
        if (now - fyKey1LastReleaseMs <= FY_KEY1_DCLICK_MS) {
            fyKey1ClickCount++;
        } else {
            fyKey1ClickCount = 1;
        }
        fyKey1LastReleaseMs = now;
        if (fyKey1ClickCount >= 2) {
            fyKey1ClickCount = 0;
            fyToggleRadioProfile();
        }
    } else if (fyKey1ClickCount > 0 && (now - fyKey1LastReleaseMs) > FY_KEY1_DCLICK_MS) {
        fyKey1ClickCount = 0;
    }
}

// ============================================================================
// WEB SERVER SETUP
// ============================================================================

static void fyRegisterRoutes() {
    if (fyRoutesRegistered) {
        return;
    }

    // Dashboard
    fyServer.on("/", HTTP_GET, [](AsyncWebServerRequest *r) {
        r->send(200, "text/html", FY_HTML);
    });

    // API: Detection list
    fyServer.on("/api/detections", HTTP_GET, [](AsyncWebServerRequest *r) {
        AsyncResponseStream *resp = r->beginResponseStream("application/json");
        writeDetectionsJSON(resp);
        r->send(resp);
    });

    // API: Stats (includes GPS status)
    fyServer.on("/api/stats", HTTP_GET, [](AsyncWebServerRequest *r) {
        int raven = 0, withGPS = 0;
        if (fyMutex && xSemaphoreTake(fyMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            for (int i = 0; i < fyDetCount; i++) {
                if (fyDet[i].isRaven) raven++;
                if (fyDet[i].hasGPS) withGPS++;
            }
            xSemaphoreGive(fyMutex);
        }
        const char* gpsSrc = "none";
        if (fyGPSIsHardware && fyHWGPSFix) gpsSrc = "hw";
        else if (fyGPSIsFresh()) gpsSrc = "phone";
        char buf[480];
        snprintf(buf, sizeof(buf),
            "{\"total\":%d,\"raven\":%d,"
            "\"ble\":\"%s\",\"wifi_sniff\":\"%s\","
            "\"radio_profile\":\"%s\",\"radio_slice\":\"%s\","
            "\"gps_valid\":%s,\"gps_age\":%lu,\"gps_tagged\":%d,"
            "\"gps_src\":\"%s\",\"gps_sats\":%d,\"gps_hw_detected\":%s,"
            "\"gps_time_valid\":%s,\"gps_time_ms\":%" PRIu64 "}",
            fyDetCount, raven,
            fyCurrentSlice == FY_SLICE_BLE ? "active" : "idle",
            (fyCurrentSlice == FY_SLICE_WIFI_SNIFF && fyWiFiSniffIsActive()) ? "active" : "idle",
            fyRadioProfile == FY_RADIO_COLLECT ? "collect" : "dashboard",
            fySliceName(fyCurrentSlice),
            fyGPSIsFresh() ? "true" : "false",
            fyGPSValid ? (millis() - fyGPSLastUpdate) : 0UL,
            withGPS,
            gpsSrc, fyHWGPSSats,
            fyHWGPSDetected ? "true" : "false",
            boardGpsTimeValid() ? "true" : "false",
            boardGpsNowEpochMs());
        r->send(200, "application/json", buf);
    });

    // API: Receive GPS from phone browser (ignored when hardware GPS has fix)
    fyServer.on("/api/gps", HTTP_GET, [](AsyncWebServerRequest *r) {
        if (fyHWGPSFix) {
            r->send(200, "application/json",
                "{\"status\":\"ignored\",\"reason\":\"hw_gps_active\"}");
            return;
        }
        if (r->hasParam("lat") && r->hasParam("lon")) {
            double lat = r->getParam("lat")->value().toDouble();
            double lon = r->getParam("lon")->value().toDouble();
            float  acc = r->hasParam("acc") ? r->getParam("acc")->value().toFloat() : 0;
            fyGPSUpdate(lat, lon, acc, /*fromHardware=*/false);
            r->send(200, "application/json", "{\"status\":\"ok\"}");
        } else {
            r->send(400, "application/json", "{\"error\":\"lat,lon required\"}");
        }
    });

    // API: Pattern database
    fyServer.on("/api/patterns", HTTP_GET, [](AsyncWebServerRequest *r) {
        AsyncResponseStream *resp = r->beginResponseStream("application/json");
        resp->print("{\"macs\":[");
        for (size_t i = 0; i < sizeof(mac_prefixes)/sizeof(mac_prefixes[0]); i++) {
            if (i > 0) resp->print(",");
            resp->printf("\"%s\"", mac_prefixes[i]);
        }
        resp->print("],\"names\":[");
        for (size_t i = 0; i < sizeof(device_name_patterns)/sizeof(device_name_patterns[0]); i++) {
            if (i > 0) resp->print(",");
            resp->printf("\"%s\"", device_name_patterns[i]);
        }
        resp->print("],\"mfr\":[");
        for (size_t i = 0; i < sizeof(ble_manufacturer_ids)/sizeof(ble_manufacturer_ids[0]); i++) {
            if (i > 0) resp->print(",");
            resp->printf("%u", ble_manufacturer_ids[i]);
        }
        resp->print("],\"raven\":[");
        for (size_t i = 0; i < sizeof(raven_service_uuids)/sizeof(raven_service_uuids[0]); i++) {
            if (i > 0) resp->print(",");
            resp->printf("\"%s\"", raven_service_uuids[i]);
        }
        resp->print("]}");
        r->send(resp);
    });

    // API: Export JSON (downloadable file)
    fyServer.on("/api/export/json", HTTP_GET, [](AsyncWebServerRequest *r) {
        AsyncResponseStream *resp = r->beginResponseStream("application/json");
        resp->addHeader("Content-Disposition", "attachment; filename=\"flockyou_detections.json\"");
        writeDetectionsJSON(resp);
        r->send(resp);
    });

    // API: Export CSV (downloadable file, includes GPS)
    fyServer.on("/api/export/csv", HTTP_GET, [](AsyncWebServerRequest *r) {
        AsyncResponseStream *resp = r->beginResponseStream("text/csv");
        resp->addHeader("Content-Disposition", "attachment; filename=\"flockyou_detections.csv\"");
        resp->println("mac,name,rssi,method,channel,first_seen_ms,last_seen_ms,count,is_raven,raven_fw,latitude,longitude,gps_accuracy");
        if (fyMutex && xSemaphoreTake(fyMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
            for (int i = 0; i < fyDetCount; i++) {
                FYDetection& d = fyDet[i];
                if (d.hasGPS) {
                    resp->printf("\"%s\",\"%s\",%d,\"%s\",%u,%" PRIu64 ",%" PRIu64 ",%d,%s,\"%s\",%.8f,%.8f,%.1f\n",
                        d.mac, d.name, d.rssi, d.method, (unsigned)d.channel,
                        fyExportMs(d.firstSeen), fyExportMs(d.lastSeen), d.count,
                        d.isRaven ? "true" : "false", d.ravenFW,
                        d.gpsLat, d.gpsLon, d.gpsAcc);
                } else {
                    resp->printf("\"%s\",\"%s\",%d,\"%s\",%u,%" PRIu64 ",%" PRIu64 ",%d,%s,\"%s\",,,\n",
                        d.mac, d.name, d.rssi, d.method, (unsigned)d.channel,
                        fyExportMs(d.firstSeen), fyExportMs(d.lastSeen), d.count,
                        d.isRaven ? "true" : "false", d.ravenFW);
                }
            }
            xSemaphoreGive(fyMutex);
        }
        r->send(resp);
    });

    // API: Export KML (GPS-tagged detections for Google Earth)
    fyServer.on("/api/export/kml", HTTP_GET, [](AsyncWebServerRequest *r) {
        AsyncResponseStream *resp = r->beginResponseStream("application/vnd.google-earth.kml+xml");
        resp->addHeader("Content-Disposition", "attachment; filename=\"flockyou_detections.kml\"");
        writeDetectionsKML(resp);
        r->send(resp);
    });

    // API: Prior session history (JSON) — strips envelope header if present
    fyServer.on("/api/history", HTTP_GET, [](AsyncWebServerRequest *r) {
        AsyncResponseStream *resp = r->beginResponseStream("application/json");
        fyStreamPrevSessionBody(resp);
        r->send(resp);
    });

    // API: Download prior session as JSON file (body-only, envelope stripped)
    fyServer.on("/api/history/json", HTTP_GET, [](AsyncWebServerRequest *r) {
        if (!fySpiffsReady || !SPIFFS.exists(FY_PREV_FILE)) {
            r->send(404, "application/json", "{\"error\":\"no prior session\"}");
            return;
        }
        AsyncResponseStream *resp = r->beginResponseStream("application/json");
        resp->addHeader("Content-Disposition",
            "attachment; filename=\"flockyou_prev_session.json\"");
        fyStreamPrevSessionBody(resp);
        r->send(resp);
    });

    // API: Download prior session as KML (reads JSON body from SPIFFS, converts)
    fyServer.on("/api/history/kml", HTTP_GET, [](AsyncWebServerRequest *r) {
        if (!fySpiffsReady || !SPIFFS.exists(FY_PREV_FILE)) {
            r->send(404, "application/json", "{\"error\":\"no prior session\"}");
            return;
        }
        File f = SPIFFS.open(FY_PREV_FILE, "r");
        if (!f) { r->send(500, "text/plain", "read error"); return; }
        // Strip envelope header if present, read body
        if (f.peek() == '{') f.readStringUntil('\n');
        String content = f.readString();
        f.close();
        if (content.length() == 0) {
            r->send(404, "application/json", "{\"error\":\"prior session empty\"}");
            return;
        }
        AsyncResponseStream *resp = r->beginResponseStream("application/vnd.google-earth.kml+xml");
        resp->addHeader("Content-Disposition", "attachment; filename=\"flockyou_prev_session.kml\"");
        resp->print("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                    "<kml xmlns=\"http://www.opengis.net/kml/2.2\">\n<Document>\n"
                    "<name>Flock-You Prior Session</name>\n"
                    "<description>Surveillance device detections from prior session</description>\n"
                    "<Style id=\"det\"><IconStyle><color>ff4489ec</color>"
                    "<scale>1.0</scale></IconStyle></Style>\n"
                    "<Style id=\"raven\"><IconStyle><color>ff4444ef</color>"
                    "<scale>1.2</scale></IconStyle></Style>\n");
        // Parse JSON array and emit placemarks
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, content);
        if (!err && doc.is<JsonArray>()) {
            int placed = 0;
            for (JsonObject d : doc.as<JsonArray>()) {
                JsonObject gps = d["gps"];
                if (!gps || !gps.containsKey("lat")) continue;
                bool isRaven = d["raven"] | false;
                resp->printf("<Placemark><name>%s</name>\n", d["mac"] | "?");
                resp->printf("<styleUrl>#%s</styleUrl>\n", isRaven ? "raven" : "det");
                resp->print("<description><![CDATA[");
                if (d["name"].is<const char*>() && strlen(d["name"] | "") > 0)
                    resp->printf("<b>Name:</b> %s<br/>", d["name"] | "");
                resp->printf("<b>Method:</b> %s<br/><b>RSSI:</b> %d<br/><b>Count:</b> %d",
                    d["method"] | "?", d["rssi"] | 0, d["count"] | 1);
                if (isRaven && d["fw"].is<const char*>())
                    resp->printf("<br/><b>Raven FW:</b> %s", d["fw"] | "");
                resp->print("]]></description>\n");
                resp->printf("<Point><coordinates>%.8f,%.8f,0</coordinates></Point>\n",
                    (double)(gps["lon"] | 0.0), (double)(gps["lat"] | 0.0));
                resp->print("</Placemark>\n");
                placed++;
            }
            printf("[FLOCK-YOU] Prior session KML: %d placemarks\n", placed);
        } else {
            printf("[FLOCK-YOU] Prior session KML: JSON parse failed\n");
        }
        resp->print("</Document>\n</kml>");
        r->send(resp);
    });

    // API: Clear all detections (saves current session first)
    fyServer.on("/api/clear", HTTP_GET, [](AsyncWebServerRequest *r) {
        fySaveSession();  // Persist before clearing
        if (fyMutex && xSemaphoreTake(fyMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
            fyDetCount = 0;
            memset(fyDet, 0, sizeof(fyDet));
            fyTriggered = false;
            fyDeviceInRange = false;
            xSemaphoreGive(fyMutex);
        }
        r->send(200, "application/json", "{\"status\":\"cleared\"}");
        printf("[FLOCK-YOU] All detections cleared (session saved)\n");
    });

    // Captive portal catch-all: redirect any unknown URL to root
    fyServer.onNotFound([](AsyncWebServerRequest *r) {
        r->redirect("http://192.168.4.1/");
    });

    fyRoutesRegistered = true;
}

static void fyTryStartServer() {
    if (fyServerStarted) {
        return;
    }

    if (WiFi.softAPIP() == IPAddress(0, 0, 0, 0)) {
        return;
    }

    // Cumulative AP time must reach FY_AP_SETTLE_MS (time-sliced dashboard).
    if (!fyDashboardApUp || millis() - fyApStartMs < FY_AP_SETTLE_MS) {
        return;
    }

    fyServer.begin();
    fyServerStarted = true;
    printf("[FLOCK-YOU] Web server started on port 80\n");

    if (!fyDnsStarted) {
        flockyouDNS.start(53, "*", WiFi.softAPIP());
        fyDnsStarted = true;
        printf("[FLOCK-YOU] Captive portal DNS started\n");
    }
}

// ============================================================================
// MAIN FUNCTIONS
// ============================================================================

void setup() {
    Serial.begin(115200);
    delay(500);

    // Read buzzer setting from OUI-SPY NVS
    Preferences bzP;
    bzP.begin("ouispy-bz", true);
    fyBuzzerOn = bzP.getBool("on", true);
    bzP.end();

    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW);

    // Init NeoPixel
    fyPixel.begin();
    fyPixel.setBrightness(FY_NEOPIXEL_BRIGHTNESS);
    fyPixel.clear();
    fyPixel.show();
    // Test flash: pink -> purple
    fyPixel.setPixelColor(0, fyPixel.Color(236, 72, 153));  // pink #ec4899
    fyPixel.show();
    delay(500);
    fyPixel.setPixelColor(0, fyPixel.Color(139, 92, 246));  // purple #8b5cf6
    fyPixel.show();
    delay(500);
    fyPixel.clear();
    fyPixel.show();

    fyMutex    = xSemaphoreCreateMutex();
    fyGPSMutex = xSemaphoreCreateMutex();

    ::boardGpsUartBegin(fyGPSSerial);
    boardGpsApplyProfile(fyGPSSerial, GPS_PROFILE_COLLECT);
    nessoUiSetGpsIndicator(NESSO_GPS_SEARCHING);

    // Init SPIFFS for session persistence
    if (SPIFFS.begin(true)) {
        fySpiffsReady = true;
        printf("[FLOCK-YOU] SPIFFS ready\n");
        // Promote last session to prev_session before we start a new one
        fyPromotePrevSession();
    } else {
        printf("[FLOCK-YOU] SPIFFS init failed - no persistence\n");
    }

    printf("\n========================================\n");
    printf("  FLOCK-YOU Surveillance Detector\n");
    printf("  Buzzer: %s\n", fyBuzzerOn ? "ON" : "OFF");
    printf("  GPS: auto-detect (%s)\n", ::boardGpsModuleName());
    printf("========================================\n");

    // Init BLE scanner and WiFi sniffer; radio scheduler starts first slice.
    fyWiFiSniffInit();
    NimBLEDevice::init("");
    fyBLEScan = NimBLEDevice::getScan();
    fyBLEScan->setScanCallbacks(new FYBLECallbacks(), true);
    fyBLEScan->setActiveScan(true);
    fyBLEScan->setInterval(FY_BLE_SLOT_MS);
    fyBLEScan->setWindow(FY_BLE_SLOT_MS);

#ifdef FY_DIAG_DISABLE_BLE
    printf("[FLOCK-YOU] DIAG BUILD: BLE scanning DISABLED\n");
#endif

    fyBootBeep();

    fyRadioProfile = FY_RADIO_COLLECT;
    fyRadioSchedulerReset();
    nessoUiSetStatus("Collect mode");
    printf("[FLOCK-YOU] Collect: 50/50 BLE+WiFi — double-click KEY1 for dashboard\n");
    printf("[FLOCK-YOU] Hold KEY1 ~1.5s for mode selector\n");
    printf("[FLOCK-YOU] BLE: MAC/name/mfr/Raven | WiFi: OUI/probe sniff\n");
    printf("[FLOCK-YOU] Ready\n\n");
}

void loop() {
    fyPollKey1DoubleClick();
    fyRadioSchedulerTick();
    fyProcessWiFiAlerts();

    if (fyRadioProfile == FY_RADIO_DASHBOARD) {
        fyTryStartServer();
        if (fyDnsStarted) {
            flockyouDNS.processNextRequest();
        }
        fyProcessDashboardHardwareGPS();
    } else {
        fyProcessHardwareGPS();
    }

    fyUpdateGpsIndicator();
    fyUpdatePixel();

    static unsigned long lastStatusLog = 0;
    if (millis() - lastStatusLog >= 30000) {
        lastStatusLog = millis();
        printf("[FLOCK-YOU] status: hits=%d in_range=%s profile=%s slice=%s server=%s\n",
               fyDetCount,
               fyDeviceInRange ? "yes" : "no",
               fyRadioProfile == FY_RADIO_COLLECT ? "collect" : "dashboard",
               fySliceName(fyCurrentSlice),
               (fyRadioProfile == FY_RADIO_DASHBOARD && fyServerStarted) ? "up" : "off");
    }

#ifndef FY_DIAG_DISABLE_BLE
    if (fyCurrentSlice == FY_SLICE_BLE) {
        if (fyBLEScan && !fyBLEScan->isScanning()) {
            fyBLEScan->start(0, false);
        }
        if (fyBLEScan && millis() - fyLastBleScan >= BLE_SCAN_INTERVAL) {
            fyBLEScan->clearResults();
            fyLastBleScan = millis();
        }
    }
#endif

    // Heartbeat tracking
    if (fyDeviceInRange) {
        if (millis() - fyLastHB >= 10000) {
            fyHeartbeat();
            fyLastHB = millis();
        }
        if (millis() - fyLastDetTime >= 30000) {
            printf("[FLOCK-YOU] Device out of range - stopping heartbeat\n");
            fyDeviceInRange = false;
            fyTriggered = false;
        }
    }

    // Back-fill GPS on any detections captured before the first fix (every 2s)
    static unsigned long lastBackfill = 0;
    if (millis() - lastBackfill >= 2000) {
        fyBackfillGPS();
        lastBackfill = millis();
    }

    // Bulletproof save cadence:
    //  - within 5s of first detection (quick first-save)
    //  - any time fyDetCount increases (new unique device), throttled to 3s minimum
    //  - every FY_SAVE_INTERVAL (15s) as a safety net
    if (fySpiffsReady && fyDetCount > 0) {
        unsigned long now = millis();
        bool countChanged = (fyDetCount != fyLastSaveCount);
        bool minGap       = (now - fyLastSave >= 3000);
        bool firstSave    = (fyLastSaveCount == 0 && now - fyLastSave >= 5000);
        bool periodic     = (now - fyLastSave >= FY_SAVE_INTERVAL);

        if (firstSave || (countChanged && minGap) || periodic) {
            fySaveSession();
            fyLastSave = millis();
        }
    }

    delay(100);
}
