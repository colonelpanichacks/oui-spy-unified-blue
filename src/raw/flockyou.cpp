// ============================================================================
// FLOCK-YOU: Surveillance Device Detector with Web Dashboard
// ============================================================================
// Detection methods (BLE only - WiFi radio used for AP):
//   1. BLE MAC prefix matching (known Flock Safety OUIs)
//   2. BLE device name pattern matching (case-insensitive substring)
//   3. BLE manufacturer company ID matching (0x09C8 XUNTONG) [from wgreenberg]
//   4. Raven gunshot detector service UUID matching
//   5. Raven firmware version estimation from service UUID patterns
//
// WiFi AP "flockyou" / "flockyou123" serves web dashboard at 192.168.4.1
// All detections stored in memory, exportable as JSON or CSV
// Optional WiFi STA connection for future features
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
#include "esp_wifi.h"
#include <TinyGPS++.h>
static void fyScanComplete(NimBLEScanResults results);

// ============================================================================
// CONFIGURATION
// ============================================================================

#define BUZZER_PIN 3

// Hardware GPS (Seeed L76K GNSS module)
#define GPS_RX_PIN 44      // D7 — ESP32 RX <- GPS TX
#define GPS_TX_PIN 43      // D6 — ESP32 TX -> GPS RX
#define GPS_BAUD   9600
#define GPS_HDOP_SCALE 5.0f  // HDOP * scale ≈ accuracy in meters

// Audio
#define LOW_FREQ 200
#define HIGH_FREQ 800
#define DETECT_FREQ 1000
#define HEARTBEAT_FREQ 600
#define BOOT_BEEP_DURATION 300
#define DETECT_BEEP_DURATION 150
#define HEARTBEAT_DURATION 100

// NeoPixel
#define FY_NEOPIXEL_PIN 4
#define FY_NEOPIXEL_BRIGHTNESS 50
#define FY_NEOPIXEL_DETECTION_BRIGHTNESS 200

// BLE scanning
#define BLE_SCAN_DURATION 2      // seconds per scan
#define BLE_SCAN_INTERVAL 3000   // ms between scans

// Detection storage
#define MAX_DETECTIONS 200

// WiFi AP credentials
#define FY_AP_SSID "flockyou"
#define FY_AP_PASS "flockyou123"

// ============================================================================
// DETECTION PATTERNS
// ============================================================================

// Known Flock Safety MAC address prefixes (OUIs)
static const char* mac_prefixes[] = {
    // FS Ext Battery devices
    "58:8e:81", "cc:cc:cc", "ec:1b:bd", "90:35:ea", "04:0d:84",
    "f0:82:c0", "1c:34:f1", "38:5b:44", "94:34:69", "b4:e3:f9",
    // Flock WiFi devices
    "70:c9:4e", "3c:91:80", "d8:f3:bc", "80:30:49", "14:5a:fc",
    "74:4c:a1", "08:3a:88", "9c:2f:9d", "94:08:53", "e4:aa:ea"
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
//
// Raven gunshot detectors advertise a mix of proprietary UUIDs (in the
// 0x3100-0x3500 range, reserved by the manufacturer) and standard Bluetooth
// SIG service UUIDs (0x180A DIS, 0x1809 Health Thermometer, 0x1819 LocNav).
//
// The SIG UUIDs are used by THOUSANDS of unrelated BLE devices (smart watches,
// earbuds, fitness trackers, etc.), so matching on them alone produces massive
// false positives. Only the proprietary UUIDs are reliable Raven markers.
//
// Detection rule:
//   - PRIMARY:   match at least one proprietary UUID (0x3100-0x3500)  -> is Raven
//   - SECONDARY: 0x1809 / 0x1819 / 0x180A are used only by estimateRavenFW()
//                to distinguish firmware versions AFTER a primary match.

#define RAVEN_DEVICE_INFO_SERVICE   "0000180a-0000-1000-8000-00805f9b34fb"
#define RAVEN_GPS_SERVICE           "00003100-0000-1000-8000-00805f9b34fb"
#define RAVEN_POWER_SERVICE         "00003200-0000-1000-8000-00805f9b34fb"
#define RAVEN_NETWORK_SERVICE       "00003300-0000-1000-8000-00805f9b34fb"
#define RAVEN_UPLOAD_SERVICE        "00003400-0000-1000-8000-00805f9b34fb"
#define RAVEN_ERROR_SERVICE         "00003500-0000-1000-8000-00805f9b34fb"
#define RAVEN_OLD_HEALTH_SERVICE    "00001809-0000-1000-8000-00805f9b34fb"
#define RAVEN_OLD_LOCATION_SERVICE  "00001819-0000-1000-8000-00805f9b34fb"

// Proprietary Raven UUIDs -- matching any of these is sufficient to flag as Raven.
static const char* raven_proprietary_uuids[] = {
    RAVEN_GPS_SERVICE,
    RAVEN_POWER_SERVICE,
    RAVEN_NETWORK_SERVICE,
    RAVEN_UPLOAD_SERVICE,
    RAVEN_ERROR_SERVICE
};

// Full UUID list (proprietary + SIG) exposed only via the /patterns endpoint
// so the web dashboard can show the complete Raven service fingerprint.
static const char* raven_service_uuids[] = {
    RAVEN_GPS_SERVICE,
    RAVEN_POWER_SERVICE,
    RAVEN_NETWORK_SERVICE,
    RAVEN_UPLOAD_SERVICE,
    RAVEN_ERROR_SERVICE,
    RAVEN_DEVICE_INFO_SERVICE,
    RAVEN_OLD_HEALTH_SERVICE,
    RAVEN_OLD_LOCATION_SERVICE
};

// ============================================================================
// DETECTION STORAGE
// ============================================================================

// Maximum (lat, lon, rssi-derived-distance) samples kept per MAC for triangulation.
// 12 samples * 200 MACs * 32 bytes/sample ~= 77 KB, allocated from PSRAM.
#define MAX_OBSERVATIONS_PER_DET 12

// Observations older than this are dropped from triangulation. Prevents a
// moving BLE emitter (e.g. vehicle) from being "triangulated" to a point
// that's actually the average of its route.
#define FY_OBS_MAX_AGE_MS (5UL * 60UL * 1000UL)

struct FYObservation {
    double lat;
    double lon;
    double distM;       // RSSI-derived estimate at the time of observation
    int    rssi;        // raw RSSI for reference
    unsigned long ts;   // millis() when sampled
};

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
    // GPS from phone (wardriving)
    double gpsLat;
    double gpsLon;
    float gpsAcc;
    bool hasGPS;
    // Distance estimate fields (meters, derived from RSSI via path-loss model)
    double lastDistM;
    double minDistM;
    double maxDistM;
    // Ring buffer of observations for triangulation. Lives in PSRAM so the
    // ~77 KB total doesn't blow out main SRAM. Pointer is allocated lazily
    // once at boot; see fySetupObservations().
    FYObservation* obs;
    int obsCount;       // number of valid entries (0..MAX_OBSERVATIONS_PER_DET)
    int obsHead;        // next write index (ring-buffer head)
};

static FYDetection fyDet[MAX_DETECTIONS];
static int fyDetCount = 0;
static SemaphoreHandle_t fyMutex = NULL;

// Path-loss model parameters, adjustable from the dashboard and persisted in NVS.
// Stored as int ×10 so we can keep NVS keys integer-typed (no float serialization).
// Default n=3.0 (urban/suburban), txPower=-59 dBm (common iBeacon reference).
static int   fyPathLossExp10 = 30;
static int   fyTxPowerDbm    = -59;
#define      FY_PATHLOSS_MIN_10 16   // n >= 1.6 (free space)
#define      FY_PATHLOSS_MAX_10 45   // n <= 4.5 (heavy obstruction)

// ============================================================================
// GLOBALS
// ============================================================================

static bool fyBuzzerOn = true;
static Adafruit_NeoPixel fyPixel(1, FY_NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);
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

// Phone GPS state (updated via browser Geolocation API -> /api/gps)
static double fyGPSLat = 0;
static double fyGPSLon = 0;
static float  fyGPSAcc = 0;
static bool   fyGPSValid = false;
static unsigned long fyGPSLastUpdate = 0;
#define GPS_STALE_MS 30000  // GPS considered stale after 30s without update

// Mutex guarding GPS state. Writes happen from loop() (fyProcessHardwareGPS)
// and from the AsyncTCP task (/api/gps). Reads happen from the BLE callback
// task. On 32-bit Xtensa a `double` write is two instructions, so without
// this a concurrent read can tear a coordinate into two unrelated halves.
static SemaphoreHandle_t fyGPSMutex = NULL;

// Hardware GPS state (Seeed L76K GNSS module on UART1)
static TinyGPSPlus fyGPS;
static HardwareSerial fyGPSSerial(1);
static bool fyHWGPSDetected = false;     // Any NMEA received = module present
static bool fyHWGPSFix = false;          // Valid position fix
static int  fyHWGPSSats = 0;             // Satellite count
static unsigned long fyHWGPSLastChar = 0;
static bool fyGPSIsHardware = false;     // Current GPS source is hardware
#define GPS_HW_TIMEOUT_MS 5000

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

static bool checkRavenUUID(NimBLEAdvertisedDevice* device, char* out_uuid = nullptr) {
    if (!device || !device->haveServiceUUID()) return false;
    int count = device->getServiceUUIDCount();
    if (count == 0) return false;
    // Only match the proprietary 0x3100-0x3500 range. SIG UUIDs (0x180A, 0x1809,
    // 0x1819) are intentionally excluded here -- they appear on countless
    // unrelated consumer BLE devices and would produce false positives.
    for (int i = 0; i < count; i++) {
        NimBLEUUID svc = device->getServiceUUID(i);
        std::string str = svc.toString();
        for (size_t j = 0; j < sizeof(raven_proprietary_uuids)/sizeof(raven_proprietary_uuids[0]); j++) {
            if (strcasecmp(str.c_str(), raven_proprietary_uuids[j]) == 0) {
                if (out_uuid) strncpy(out_uuid, str.c_str(), 40);
                return true;
            }
        }
    }
    return false;
}

static const char* estimateRavenFW(NimBLEAdvertisedDevice* device) {
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
// DISTANCE ESTIMATION & TRIANGULATION
// ============================================================================
// Log-distance path-loss model:
//   RSSI(d) = TxPower - 10 * n * log10(d)
// Solving for d:
//   d = 10 ^ ((TxPower - RSSI) / (10 * n))
//
// Defaults are intentionally conservative. Field calibration for a specific
// Flock/Raven install would improve accuracy; we expose both parameters via
// the dashboard settings panel.
//
// Absolute distance from any single RSSI reading is ±50% to ±300% in practice
// due to antenna orientation, obstructions, and multipath. Triangulation
// from multiple readings at different positions is where the real value is.
// ============================================================================

#include <math.h>

static double fyRssiToDistance(int rssi) {
    double n = (double)fyPathLossExp10 / 10.0;
    if (n < 1.0) n = 1.0;   // guard against bad NVS values
    double exponent = (double)(fyTxPowerDbm - rssi) / (10.0 * n);
    double d = pow(10.0, exponent);
    if (d < 0.1)     d = 0.1;
    if (d > 10000.0) d = 10000.0;
    return d;
}

// Haversine distance between two GPS points, in meters. Used by the
// triangulation solver as the "observed distance from pinhole to camera"
// ground truth.
static double fyHaversineMeters(double lat1, double lon1, double lat2, double lon2) {
    static const double R = 6371000.0;  // Earth radius (m)
    double dLat = (lat2 - lat1) * M_PI / 180.0;
    double dLon = (lon2 - lon1) * M_PI / 180.0;
    double a = sin(dLat/2)*sin(dLat/2) +
               cos(lat1*M_PI/180.0)*cos(lat2*M_PI/180.0) *
               sin(dLon/2)*sin(dLon/2);
    double c = 2 * atan2(sqrt(a), sqrt(1-a));
    return R * c;
}

// Gauss-Newton least-squares triangulation. Given N observations (lat_i, lon_i,
// d_i) representing "the transmitter was approximately d_i meters from (lat_i,lon_i)",
// find the (lat, lon) that minimizes sum of squared distance residuals.
// Returns true if converged with a sensible residual, false otherwise.
// outLat/outLon are initialized to the RSSI-weighted centroid as starting guess.
static bool fyTriangulate(const FYObservation* obs, int n,
                          double& outLat, double& outLon, double& outRmsErrM) {
    if (n < 3) return false;

    // Drop stale observations
    unsigned long now = millis();
    int used[MAX_OBSERVATIONS_PER_DET];
    int m = 0;
    for (int i = 0; i < n; i++) {
        if (now - obs[i].ts <= FY_OBS_MAX_AGE_MS) used[m++] = i;
    }
    if (m < 3) return false;

    // Initial guess: weighted centroid (weights = 1 / distM^2 so close-in
    // readings dominate). Clamp divisor to avoid infinite weight from d=0.1.
    double sumW = 0, sumWLat = 0, sumWLon = 0;
    for (int k = 0; k < m; k++) {
        const FYObservation& o = obs[used[k]];
        double w = 1.0 / (o.distM * o.distM + 1.0);
        sumW += w;
        sumWLat += w * o.lat;
        sumWLon += w * o.lon;
    }
    if (sumW <= 0) return false;
    double lat = sumWLat / sumW;
    double lon = sumWLon / sumW;

    // Gauss-Newton iterations. Work in a local tangent plane (meters east/north
    // from the centroid) so we can treat lat/lon as Cartesian for the solver.
    // Convert back to lat/lon at the end.
    double metersPerDegLat = 111320.0;
    double metersPerDegLon = 111320.0 * cos(lat * M_PI / 180.0);
    if (metersPerDegLon < 1.0) metersPerDegLon = 1.0;  // avoid singularity at poles

    // State vector: (x, y) meters offset from initial guess
    double x = 0, y = 0;

    for (int iter = 0; iter < 10; iter++) {
        // Build normal equations: J^T J * [dx dy] = J^T r
        double a00=0, a01=0, a11=0, b0=0, b1=0;
        for (int k = 0; k < m; k++) {
            const FYObservation& o = obs[used[k]];
            double ox = (o.lon - lon) * metersPerDegLon;
            double oy = (o.lat - lat) * metersPerDegLat;
            double dx = x - ox;
            double dy = y - oy;
            double d  = sqrt(dx*dx + dy*dy);
            if (d < 0.5) d = 0.5;
            double res = d - o.distM;
            // Jacobian: d/dx = dx/d, d/dy = dy/d
            double jx = dx / d;
            double jy = dy / d;
            a00 += jx*jx;  a01 += jx*jy;  a11 += jy*jy;
            b0  += jx*res; b1  += jy*res;
        }
        // Solve 2x2 normal equations for [dx dy]. Negate because we go opposite residual.
        double det = a00 * a11 - a01 * a01;
        if (fabs(det) < 1e-9) break;  // singular, can't improve
        double sx = -(a11 * b0 - a01 * b1) / det;
        double sy = -(a00 * b1 - a01 * b0) / det;
        // Damp huge steps (prevents divergence when one observation is a massive outlier)
        double stepMag = sqrt(sx*sx + sy*sy);
        if (stepMag > 500.0) {
            sx *= 500.0 / stepMag;
            sy *= 500.0 / stepMag;
        }
        x += sx;
        y += sy;
        if (stepMag < 0.5) break;  // converged to sub-meter step
    }

    // Compute final RMS residual and convert solution back to lat/lon.
    double totalRes2 = 0;
    for (int k = 0; k < m; k++) {
        const FYObservation& o = obs[used[k]];
        double ox = (o.lon - lon) * metersPerDegLon;
        double oy = (o.lat - lat) * metersPerDegLat;
        double dx = x - ox;
        double dy = y - oy;
        double d  = sqrt(dx*dx + dy*dy);
        double res = d - o.distM;
        totalRes2 += res * res;
    }
    outRmsErrM = sqrt(totalRes2 / m);
    outLat = lat + y / metersPerDegLat;
    outLon = lon + x / metersPerDegLon;
    return true;
}

// Allocate per-detection observation buffers in PSRAM. Called once during
// setup(). We allocate the whole pool as one block and hand out slices so
// we fail loudly if PSRAM is absent, rather than partially-succeeding later.
static bool fySetupObservations() {
    size_t bytes = (size_t)MAX_DETECTIONS * MAX_OBSERVATIONS_PER_DET * sizeof(FYObservation);
    FYObservation* pool = (FYObservation*) ps_malloc(bytes);
    if (!pool) {
        // Fall back to regular heap -- loses triangulation on boards without PSRAM.
        pool = (FYObservation*) malloc(bytes);
        if (!pool) {
            printf("[FLOCK-YOU] WARNING: observation buffer allocation FAILED (%u bytes). "
                   "Triangulation disabled.\n", (unsigned)bytes);
            return false;
        }
        printf("[FLOCK-YOU] observation buffer in heap (%u KB). PSRAM preferred but unavailable.\n",
               (unsigned)(bytes / 1024));
    } else {
        printf("[FLOCK-YOU] observation buffer in PSRAM (%u KB, %d MACs x %d samples)\n",
               (unsigned)(bytes / 1024), MAX_DETECTIONS, MAX_OBSERVATIONS_PER_DET);
    }
    memset(pool, 0, bytes);
    for (int i = 0; i < MAX_DETECTIONS; i++) {
        fyDet[i].obs = pool + (size_t)i * MAX_OBSERVATIONS_PER_DET;
        fyDet[i].obsCount = 0;
        fyDet[i].obsHead  = 0;
    }
    return true;
}

// Add one observation to a detection's ring buffer. Caller must hold fyMutex.
static void fyAddObservation(FYDetection& d, double lat, double lon, double distM, int rssi) {
    if (!d.obs) return;
    FYObservation& o = d.obs[d.obsHead];
    o.lat = lat;
    o.lon = lon;
    o.distM = distM;
    o.rssi  = rssi;
    o.ts    = millis();
    d.obsHead = (d.obsHead + 1) % MAX_OBSERVATIONS_PER_DET;
    if (d.obsCount < MAX_OBSERVATIONS_PER_DET) d.obsCount++;
}

// ============================================================================
// PATH-LOSS CONFIG PERSISTENCE
// ============================================================================

static void fyLoadPathLoss() {
    Preferences p;
    p.begin("fy-radio", true);
    fyPathLossExp10 = p.getInt("n10",  30);
    fyTxPowerDbm    = p.getInt("txpw", -59);
    p.end();
    if (fyPathLossExp10 < FY_PATHLOSS_MIN_10) fyPathLossExp10 = FY_PATHLOSS_MIN_10;
    if (fyPathLossExp10 > FY_PATHLOSS_MAX_10) fyPathLossExp10 = FY_PATHLOSS_MAX_10;
    if (fyTxPowerDbm < -100) fyTxPowerDbm = -100;
    if (fyTxPowerDbm > -20)  fyTxPowerDbm = -20;
    printf("[FLOCK-YOU] Path-loss: n=%.1f, txPower=%d dBm\n",
           fyPathLossExp10 / 10.0, fyTxPowerDbm);
}

static void fySavePathLoss(int n10, int txpw) {
    if (n10 < FY_PATHLOSS_MIN_10) n10 = FY_PATHLOSS_MIN_10;
    if (n10 > FY_PATHLOSS_MAX_10) n10 = FY_PATHLOSS_MAX_10;
    if (txpw < -100) txpw = -100;
    if (txpw > -20)  txpw = -20;
    Preferences p;
    p.begin("fy-radio", false);
    p.putInt("n10",  n10);
    p.putInt("txpw", txpw);
    p.end();
    fyPathLossExp10 = n10;
    fyTxPowerDbm    = txpw;
    printf("[FLOCK-YOU] Path-loss saved: n=%.1f, txPower=%d dBm\n", n10 / 10.0, txpw);
}



static bool fyGPSIsFresh() {
    return fyGPSValid && (millis() - fyGPSLastUpdate < GPS_STALE_MS);
}

// Atomic snapshot of GPS state. Returns true with fresh (lat,lon,acc) filled
// in, false if GPS is stale/invalid or the mutex could not be taken.
static bool fyGPSSnapshot(double& lat, double& lon, float& acc) {
    if (!fyGPSMutex || xSemaphoreTake(fyGPSMutex, pdMS_TO_TICKS(20)) != pdTRUE) return false;
    bool fresh = fyGPSValid && (millis() - fyGPSLastUpdate < GPS_STALE_MS);
    if (fresh) {
        lat = fyGPSLat;
        lon = fyGPSLon;
        acc = fyGPSAcc;
    }
    xSemaphoreGive(fyGPSMutex);
    return fresh;
}

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

    // Update position when valid fix is available
    if (fyGPS.location.isUpdated() && fyGPS.location.isValid()) {
        if (!fyHWGPSFix) {
            printf("[FLOCK-YOU] First GPS fix acquired! Sats:%d Lat:%.6f Lon:%.6f\n",
                   fyHWGPSSats, fyGPS.location.lat(), fyGPS.location.lng());
        }
        fyHWGPSFix = true;
        fyGPSIsHardware = true;
        if (fyGPSMutex && xSemaphoreTake(fyGPSMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            fyGPSLat = fyGPS.location.lat();
            fyGPSLon = fyGPS.location.lng();
            fyGPSAcc = fyGPS.hdop.isValid() ? (float)(fyGPS.hdop.hdop() * GPS_HDOP_SCALE) : 10.0f;
            fyGPSValid = true;
            fyGPSLastUpdate = millis();
            xSemaphoreGive(fyGPSMutex);
        }
    } else if (fyHWGPSFix && fyGPS.location.isValid()) {
        // Keep updating timestamp while fix is held
        if (fyGPSMutex && xSemaphoreTake(fyGPSMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            fyGPSLastUpdate = millis();
            xSemaphoreGive(fyGPSMutex);
        }
    }
}

// ============================================================================
// DETECTION MANAGEMENT
// ============================================================================

static int fyAddDetection(const char* mac, const char* name, int rssi,
                          const char* method, bool isRaven = false,
                          const char* ravenFW = "") {
    if (!fyMutex || xSemaphoreTake(fyMutex, pdMS_TO_TICKS(100)) != pdTRUE) return -1;

    // Path-loss distance estimate for this sighting.
    double distM = fyRssiToDistance(rssi);

    // Snapshot GPS outside the detection mutex's inner loop (fyGPSSnapshot
    // takes fyGPSMutex internally). Needed below both for update and insert
    // paths; capture now so we hold the GPS mutex for as short as possible.
    double gLat = 0, gLon = 0;
    float  gAcc = 0;
    bool   haveGPS = fyGPSSnapshot(gLat, gLon, gAcc);

    // Update existing by MAC. Name is stored raw -- output-side JSON/CSV
    // escapers (fyJsonEscape, fyCsvEscape) handle quoting and control chars.
    for (int i = 0; i < fyDetCount; i++) {
        if (strcasecmp(fyDet[i].mac, mac) == 0) {
            fyDet[i].count++;
            fyDet[i].lastSeen = millis();
            fyDet[i].rssi = rssi;
            fyDet[i].lastDistM = distM;
            if (distM < fyDet[i].minDistM || fyDet[i].minDistM == 0) fyDet[i].minDistM = distM;
            if (distM > fyDet[i].maxDistM) fyDet[i].maxDistM = distM;
            if (name && name[0]) {
                strncpy(fyDet[i].name, name, sizeof(fyDet[i].name) - 1);
                fyDet[i].name[sizeof(fyDet[i].name) - 1] = '\0';
            }
            // Update GPS on every re-sighting (captures movement)
            if (haveGPS) {
                fyDet[i].hasGPS = true;
                fyDet[i].gpsLat = gLat;
                fyDet[i].gpsLon = gLon;
                fyDet[i].gpsAcc = gAcc;
                fyAddObservation(fyDet[i], gLat, gLon, distM, rssi);
            }
            xSemaphoreGive(fyMutex);
            return i;
        }
    }

    // Add new
    if (fyDetCount < MAX_DETECTIONS) {
        FYDetection& d = fyDet[fyDetCount];
        // Preserve the obs pointer across the zero-init -- fySetupObservations()
        // allocated it once and we don't want to lose it.
        FYObservation* savedObs = d.obs;
        memset(&d, 0, sizeof(d));
        d.obs = savedObs;
        strncpy(d.mac, mac, sizeof(d.mac) - 1);
        if (name) {
            strncpy(d.name, name, sizeof(d.name) - 1);
            // memset zeroed the buffer, strncpy's n-1 cap leaves the final byte NUL
        }
        d.rssi = rssi;
        strncpy(d.method, method, sizeof(d.method) - 1);
        d.firstSeen = millis();
        d.lastSeen = millis();
        d.count = 1;
        d.isRaven = isRaven;
        strncpy(d.ravenFW, ravenFW ? ravenFW : "", sizeof(d.ravenFW) - 1);
        d.lastDistM = distM;
        d.minDistM  = distM;
        d.maxDistM  = distM;
        // Attach GPS from phone
        if (haveGPS) {
            d.hasGPS = true;
            d.gpsLat = gLat;
            d.gpsLon = gLon;
            d.gpsAcc = gAcc;
            fyAddObservation(d, gLat, gLon, distM, rssi);
        }
        int idx = fyDetCount++;
        xSemaphoreGive(fyMutex);
        return idx;
    }

    xSemaphoreGive(fyMutex);
    return -1;
}

// ============================================================================
// BLE SCANNING
// ============================================================================

class FYBLECallbacks : public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice* dev) override {
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
                                     method, isRaven, ravenFW);

            // Human-readable log
            printf("[FLOCK-YOU] DETECTED: %s %s RSSI:%d [%s] count:%d\n",
                   addrStr.c_str(), name.c_str(), rssi, method,
                   idx >= 0 ? fyDet[idx].count : 0);

            // JSON serial output (Flask-compatible format for live ingestion)
            // Build GPS fragment if available (atomic snapshot -- no torn reads)
            char gpsBuf[80] = "";
            double gLat, gLon;
            float gAcc;
            if (fyGPSSnapshot(gLat, gLon, gAcc)) {
                snprintf(gpsBuf, sizeof(gpsBuf),
                    ",\"gps\":{\"latitude\":%.8f,\"longitude\":%.8f,\"accuracy\":%.1f}",
                    gLat, gLon, gAcc);
            }
            if (isRaven) {
                printf("{\"detection_method\":\"%s\",\"protocol\":\"bluetooth_le\","
                       "\"mac_address\":\"%s\",\"device_name\":\"%s\","
                       "\"rssi\":%d,\"is_raven\":true,\"raven_fw\":\"%s\"%s}\n",
                       method, addrStr.c_str(), name.c_str(), rssi, ravenFW, gpsBuf);
            } else {
                printf("{\"detection_method\":\"%s\",\"protocol\":\"bluetooth_le\","
                       "\"mac_address\":\"%s\",\"device_name\":\"%s\","
                       "\"rssi\":%d%s}\n",
                       method, addrStr.c_str(), name.c_str(), rssi, gpsBuf);
            }

            if (!fyTriggered) {
                fyTriggered = true;
                fyDetectBeep();
            }
            fyDeviceInRange = true;
            fyLastDetTime = millis();
            fyLastHB = millis();
        }
    }
};

// ============================================================================
// JSON HELPER
// ============================================================================

// RFC 8259-compliant JSON string escaper. Writes `src` to `out` as a JSON
// string body (no surrounding quotes). Bails cleanly if out_sz is too small,
// always null-terminating. Characters that must be escaped per RFC:
//   "  \\  and all control chars U+0000..U+001F.
// Non-ASCII bytes (UTF-8 continuation) pass through unchanged -- modern JSON
// parsers handle UTF-8 natively. Anything that can't be escaped and doesn't
// fit gets truncated at the last safe boundary.
static void fyJsonEscape(char* out, size_t out_sz, const char* src) {
    if (!out || out_sz == 0) return;
    size_t o = 0;
    if (!src) { out[0] = '\0'; return; }
    while (*src && o + 7 < out_sz) {  // 7 = worst case "\u00XX" + NUL
        unsigned char c = (unsigned char)*src++;
        switch (c) {
            case '"':  out[o++] = '\\'; out[o++] = '"';  break;
            case '\\': out[o++] = '\\'; out[o++] = '\\'; break;
            case '\b': out[o++] = '\\'; out[o++] = 'b';  break;
            case '\f': out[o++] = '\\'; out[o++] = 'f';  break;
            case '\n': out[o++] = '\\'; out[o++] = 'n';  break;
            case '\r': out[o++] = '\\'; out[o++] = 'r';  break;
            case '\t': out[o++] = '\\'; out[o++] = 't';  break;
            default:
                if (c < 0x20) {
                    // Remaining control chars -> \u00XX
                    static const char hex[] = "0123456789abcdef";
                    out[o++] = '\\'; out[o++] = 'u';
                    out[o++] = '0';  out[o++] = '0';
                    out[o++] = hex[(c >> 4) & 0xF];
                    out[o++] = hex[c & 0xF];
                } else {
                    out[o++] = (char)c;
                }
        }
    }
    out[o] = '\0';
}

// RFC 4180 CSV string escaper. Wraps fields containing commas, quotes, CR,
// or LF in surrounding quotes and doubles any embedded quotes. Writes a
// complete quoted field (including the outer quotes) into `out`.
static void fyCsvEscape(char* out, size_t out_sz, const char* src) {
    if (!out || out_sz < 3) { if (out && out_sz) out[0] = '\0'; return; }
    size_t o = 0;
    out[o++] = '"';
    if (src) {
        while (*src && o + 2 < out_sz) {  // leave room for closing quote + NUL
            char c = *src++;
            if (c == '"') {
                if (o + 3 >= out_sz) break;  // need room for "" + closing + NUL
                out[o++] = '"';
                out[o++] = '"';
            } else {
                out[o++] = c;
            }
        }
    }
    out[o++] = '"';
    out[o] = '\0';
}

static void writeDetectionsJSON(AsyncResponseStream *resp) {
    resp->print("[");
    if (fyMutex && xSemaphoreTake(fyMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        // Escape buffers -- sized for worst-case expansion (6x for \u00XX).
        char macEsc[18 * 6 + 1];
        char nameEsc[48 * 6 + 1];
        char methodEsc[24 * 6 + 1];
        char fwEsc[16 * 6 + 1];
        for (int i = 0; i < fyDetCount; i++) {
            if (i > 0) resp->print(",");
            fyJsonEscape(macEsc,    sizeof(macEsc),    fyDet[i].mac);
            fyJsonEscape(nameEsc,   sizeof(nameEsc),   fyDet[i].name);
            fyJsonEscape(methodEsc, sizeof(methodEsc), fyDet[i].method);
            fyJsonEscape(fwEsc,     sizeof(fwEsc),     fyDet[i].ravenFW);
            resp->printf(
                "{\"mac\":\"%s\",\"name\":\"%s\",\"rssi\":%d,\"method\":\"%s\","
                "\"first\":%lu,\"last\":%lu,\"count\":%d,"
                "\"raven\":%s,\"fw\":\"%s\","
                "\"dist_m\":%.1f,\"min_dist_m\":%.1f,\"max_dist_m\":%.1f,"
                "\"obs\":%d",
                macEsc, nameEsc, fyDet[i].rssi, methodEsc,
                fyDet[i].firstSeen, fyDet[i].lastSeen, fyDet[i].count,
                fyDet[i].isRaven ? "true" : "false", fwEsc,
                fyDet[i].lastDistM, fyDet[i].minDistM, fyDet[i].maxDistM,
                fyDet[i].obsCount);
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
// SESSION PERSISTENCE (SPIFFS)
// ============================================================================

#define FY_SESSION_TMP "/session.tmp"

// Atomic session save: writes to a temp file, closes it, then swaps it into
// place only after a full successful write. Prevents corrupt half-written
// session.json files on power loss (which would otherwise get promoted to
// prev_session.json on next boot and propagate corruption).
// Also JSON-escapes every string field so BLE names with control chars or
// quotes can't break the output.
static void fySaveSession() {
    if (!fySpiffsReady || !fyMutex) return;
    if (xSemaphoreTake(fyMutex, pdMS_TO_TICKS(300)) != pdTRUE) return;

    // Stale tmp from a prior crashed save? Clear it first.
    if (SPIFFS.exists(FY_SESSION_TMP)) SPIFFS.remove(FY_SESSION_TMP);

    File f = SPIFFS.open(FY_SESSION_TMP, "w");
    if (!f) { xSemaphoreGive(fyMutex); return; }

    char macEsc[18 * 6 + 1];
    char nameEsc[48 * 6 + 1];
    char methodEsc[24 * 6 + 1];
    char fwEsc[16 * 6 + 1];

    bool ok = true;
    size_t written = 1;  // account for opening '['
    if (f.print("[") != 1) ok = false;
    for (int i = 0; ok && i < fyDetCount; i++) {
        if (i > 0 && f.print(",") != 1) { ok = false; break; }
        FYDetection& d = fyDet[i];
        fyJsonEscape(macEsc,    sizeof(macEsc),    d.mac);
        fyJsonEscape(nameEsc,   sizeof(nameEsc),   d.name);
        fyJsonEscape(methodEsc, sizeof(methodEsc), d.method);
        fyJsonEscape(fwEsc,     sizeof(fwEsc),     d.ravenFW);
        int n = f.printf("{\"mac\":\"%s\",\"name\":\"%s\",\"rssi\":%d,\"method\":\"%s\","
                         "\"first\":%lu,\"last\":%lu,\"count\":%d,"
                         "\"raven\":%s,\"fw\":\"%s\"",
                         macEsc, nameEsc, d.rssi, methodEsc,
                         d.firstSeen, d.lastSeen, d.count,
                         d.isRaven ? "true" : "false", fwEsc);
        if (n <= 0) { ok = false; break; }
        written += n;
        if (d.hasGPS) {
            n = f.printf(",\"gps\":{\"lat\":%.8f,\"lon\":%.8f,\"acc\":%.1f}",
                         d.gpsLat, d.gpsLon, d.gpsAcc);
            if (n <= 0) { ok = false; break; }
            written += n;
        }
        if (f.print("}") != 1) { ok = false; break; }
        written += 1;
    }
    if (ok && f.print("]") != 1) ok = false;
    f.close();

    if (!ok) {
        printf("[FLOCK-YOU] Session save FAILED (write error) - tmp file discarded\n");
        SPIFFS.remove(FY_SESSION_TMP);
        xSemaphoreGive(fyMutex);
        return;
    }

    // Atomic swap: remove old, rename tmp. SPIFFS.rename() is noted as
    // unreliable elsewhere in this file, so do it as delete+copy+delete.
    if (SPIFFS.exists(FY_SESSION_FILE)) SPIFFS.remove(FY_SESSION_FILE);

    File src = SPIFFS.open(FY_SESSION_TMP, "r");
    File dst = SPIFFS.open(FY_SESSION_FILE, "w");
    if (!src || !dst) {
        printf("[FLOCK-YOU] Session save FAILED (swap error)\n");
        if (src) src.close();
        if (dst) dst.close();
        SPIFFS.remove(FY_SESSION_TMP);
        xSemaphoreGive(fyMutex);
        return;
    }
    uint8_t buf[256];
    while (src.available()) {
        size_t r = src.read(buf, sizeof(buf));
        if (r == 0) break;
        dst.write(buf, r);
    }
    src.close();
    dst.close();
    SPIFFS.remove(FY_SESSION_TMP);

    fyLastSaveCount = fyDetCount;
    printf("[FLOCK-YOU] Session saved: %d detections (%u bytes)\n",
           fyDetCount, (unsigned)written);
    xSemaphoreGive(fyMutex);
}

static void fyPromotePrevSession() {
    // Copy current session to prev_session on boot, then delete original
    // NOTE: SPIFFS.rename() is unreliable on ESP32 — use copy+delete instead
    if (!fySpiffsReady) return;
    if (!SPIFFS.exists(FY_SESSION_FILE)) {
        printf("[FLOCK-YOU] No prior session file to promote\n");
        return;
    }

    File src = SPIFFS.open(FY_SESSION_FILE, "r");
    if (!src) {
        printf("[FLOCK-YOU] Failed to open session file for promotion\n");
        return;
    }
    String data = src.readString();
    src.close();

    if (data.length() == 0) {
        printf("[FLOCK-YOU] Session file empty, skipping promotion\n");
        SPIFFS.remove(FY_SESSION_FILE);
        return;
    }

    // Write to prev_session (overwrite any existing)
    File dst = SPIFFS.open(FY_PREV_FILE, "w");
    if (!dst) {
        printf("[FLOCK-YOU] Failed to create prev_session file\n");
        return;
    }
    dst.print(data);
    dst.close();

    // Delete the old session file so it doesn't get re-promoted next boot
    SPIFFS.remove(FY_SESSION_FILE);
    printf("[FLOCK-YOU] Prior session promoted: %d bytes\n", data.length());
}

// ============================================================================
// KML EXPORT
// ============================================================================

static void writeDetectionsKML(AsyncResponseStream *resp) {
    resp->print("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                "<kml xmlns=\"http://www.opengis.net/kml/2.2\">\n<Document>\n"
                "<name>Flock-You Detections</name>\n"
                "<description>Surveillance device detections with GPS + distance</description>\n");

    // Pin styles
    resp->print("<Style id=\"det\"><IconStyle><color>ff4489ec</color>"
                "<scale>1.0</scale></IconStyle></Style>\n"
                "<Style id=\"raven\"><IconStyle><color>ff4444ef</color>"
                "<scale>1.2</scale></IconStyle></Style>\n"
                "<Style id=\"src\"><IconStyle><color>ff0000ff</color>"
                "<scale>1.4</scale><Icon><href>http://maps.google.com/mapfiles/kml/shapes/target.png</href></Icon>"
                "</IconStyle></Style>\n");

    if (fyMutex && xSemaphoreTake(fyMutex, pdMS_TO_TICKS(300)) == pdTRUE) {
        // Folder 1: raw detection sightings
        resp->print("<Folder><name>Detections</name>\n");
        for (int i = 0; i < fyDetCount; i++) {
            FYDetection& d = fyDet[i];
            if (!d.hasGPS) continue;
            resp->print("<Placemark>\n");
            resp->printf("<name>%s</name>\n", d.mac);
            resp->printf("<styleUrl>#%s</styleUrl>\n", d.isRaven ? "raven" : "det");
            resp->print("<description><![CDATA[");
            if (d.name[0]) resp->printf("<b>Name:</b> %s<br/>", d.name);
            resp->printf("<b>Method:</b> %s<br/>"
                         "<b>RSSI:</b> %d dBm<br/>"
                         "<b>Distance:</b> ~%.0f m (closest %.0f m)<br/>"
                         "<b>Count:</b> %d<br/>",
                         d.method, d.rssi,
                         d.lastDistM, d.minDistM,
                         d.count);
            if (d.isRaven) resp->printf("<b>Raven FW:</b> %s<br/>", d.ravenFW);
            resp->printf("<b>GPS accuracy:</b> %.1f m<br/>"
                         "<b>Observations:</b> %d",
                         d.gpsAcc, d.obsCount);
            resp->print("]]></description>\n");
            resp->printf("<Point><coordinates>%.8f,%.8f,0</coordinates></Point>\n",
                         d.gpsLon, d.gpsLat);
            resp->print("</Placemark>\n");
        }
        resp->print("</Folder>\n");

        // Folder 2: triangulated source positions (MACs with >=3 observations)
        resp->print("<Folder><name>Estimated Source Locations</name>\n"
                    "<description>Triangulated transmitter positions (Gauss-Newton, >=3 observations)</description>\n");
        for (int i = 0; i < fyDetCount; i++) {
            FYDetection& d = fyDet[i];
            if (d.obsCount < 3 || !d.obs) continue;
            double srcLat, srcLon, rmsErr;
            if (!fyTriangulate(d.obs, d.obsCount, srcLat, srcLon, rmsErr)) continue;
            resp->print("<Placemark>\n");
            resp->printf("<name>SRC %s</name>\n", d.mac);
            resp->print("<styleUrl>#src</styleUrl>\n");
            resp->print("<description><![CDATA[");
            resp->printf("<b>Estimated transmitter location</b><br/>"
                         "<b>MAC:</b> %s<br/>", d.mac);
            if (d.name[0]) resp->printf("<b>Name:</b> %s<br/>", d.name);
            resp->printf("<b>RMS error:</b> %.1f m<br/>"
                         "<b>Observations:</b> %d<br/>"
                         "<b>Type:</b> %s",
                         rmsErr, d.obsCount,
                         d.isRaven ? "Raven" : "Flock/Surveillance");
            resp->print("]]></description>\n");
            resp->printf("<Point><coordinates>%.8f,%.8f,0</coordinates></Point>\n",
                         srcLon, srcLat);
            resp->print("</Placemark>\n");
        }
        resp->print("</Folder>\n");

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
<hr class="sep">
<h4>RADIO SETTINGS</h4>
<p style="font-size:10px;color:#8b5cf6;margin-bottom:6px">Path-loss model for RSSI-to-distance estimation</p>
<div style="margin-bottom:8px">
<label style="font-size:12px;display:block;margin-bottom:4px">Path-loss exponent (n): <span id="nV">3.0</span></label>
<input type="range" id="nR" min="16" max="45" step="1" value="30" style="width:100%" oninput="document.getElementById('nV').textContent=(this.value/10).toFixed(1)">
<div style="display:flex;justify-content:space-between;font-size:9px;color:#8b5cf6"><span>1.6 open</span><span>3.0 urban</span><span>4.5 dense</span></div>
</div>
<div style="margin-bottom:8px">
<label style="font-size:12px;display:block;margin-bottom:4px">TX power ref (dBm): <span id="txV">-59</span></label>
<input type="range" id="txR" min="-100" max="-20" step="1" value="-59" style="width:100%" oninput="document.getElementById('txV').textContent=this.value">
</div>
<button class="btn" onclick="saveSettings()" style="background:#22c55e">SAVE RADIO SETTINGS</button>
<hr class="sep">
<h4>TRIANGULATION</h4>
<div id="triInfo" style="font-size:11px;color:#8b5cf6;margin-bottom:8px">Loading...</div>
<button class="btn" onclick="loadLocations()">REFRESH LOCATIONS</button>
<hr class="sep">
<button class="btn dng" onclick="if(confirm('Clear all detections?'))fetch('/api/clear').then(()=>refresh())">CLEAR ALL DETECTIONS</button>
</div>
</div>
<script>
let D=[],H=[];
// HTML-escape server-supplied strings before injecting into innerHTML.
// Fields like d.name, d.method, d.mac, d.fw, and the pattern lists come
// from BLE advertisements or pattern DBs -- a hostile beacon could inject
// <script> or event-handler payloads otherwise.
function esc(s){return String(s==null?'':s).replace(/[&<>"']/g,function(c){return{'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c];});}
function tab(i,el){document.querySelectorAll('.tb button').forEach(b=>b.classList.remove('a'));document.querySelectorAll('.pn').forEach(p=>p.classList.remove('a'));el.classList.add('a');document.getElementById('p'+i).classList.add('a');if(i===1&&!window._hL)loadHistory();if(i===2&&!window._pL)loadPat();}
function refresh(){fetch('/api/detections').then(r=>r.json()).then(d=>{D=d;render();stats();}).catch(()=>{});}
function render(){const el=document.getElementById('dL');if(!D.length){el.innerHTML='<div class="empty">Scanning for surveillance devices...<br>BLE active on all channels</div>';return;}
D.sort((a,b)=>b.last-a.last);el.innerHTML=D.map(card).join('');}
function stats(){document.getElementById('sT').textContent=D.length;document.getElementById('sR').textContent=D.filter(d=>d.raven).length;
fetch('/api/stats').then(r=>r.json()).then(s=>{let g=document.getElementById('sG'),gl=document.getElementById('sGL');if(s.gps_src==='hw'){g.textContent=s.gps_sats+'sat';g.style.color='#22c55e';gl.textContent='HW GPS';}else if(s.gps_src==='phone'){g.textContent=s.gps_tagged+'/'+s.total;g.style.color='#22c55e';gl.textContent='PHONE';}else if(s.gps_hw_detected){g.textContent=s.gps_sats+'sat';g.style.color='#facc15';gl.textContent='NO FIX';}else{g.textContent='TAP';g.style.color='#ef4444';gl.textContent='GPS';}}).catch(()=>{});}
function card(d){return '<div class="det"><div class="mac">'+esc(d.mac)+(d.name?'<span class="nm">'+esc(d.name)+'</span>':'')+'</div><div class="inf"><span>RSSI: '+(+d.rssi|0)+'</span><span>'+esc(d.method)+'</span><span style="color:#ec4899;font-weight:bold">&times;'+(+d.count|0)+'</span>'+(d.raven?'<span class="rv">RAVEN '+esc(d.fw)+'</span>':'')+(d.gps?'<span style="color:#22c55e">&#9673; '+(+d.gps.lat).toFixed(5)+','+(+d.gps.lon).toFixed(5)+'</span>':'<span style="color:#666">no gps</span>')+'</div></div>';}
function loadHistory(){fetch('/api/history').then(r=>r.json()).then(d=>{H=d;let el=document.getElementById('hL');if(!H.length){el.innerHTML='<div class="empty">No prior session data</div>';return;}
H.sort((a,b)=>b.last-a.last);el.innerHTML='<div style="font-size:11px;color:#8b5cf6;margin-bottom:8px">'+H.length+' detections from prior session</div>'+H.map(card).join('');window._hL=1;}).catch(()=>{document.getElementById('hL').innerHTML='<div class="empty">No prior session data</div>';});}
function loadPat(){fetch('/api/patterns').then(r=>r.json()).then(p=>{let h='';
h+='<div class="pg"><h3>MAC Prefixes ('+p.macs.length+')</h3><div class="it">'+p.macs.map(m=>'<span>'+esc(m)+'</span>').join('')+'</div></div>';
h+='<div class="pg"><h3>BLE Device Names ('+p.names.length+')</h3><div class="it">'+p.names.map(n=>'<span>'+esc(n)+'</span>').join('')+'</div></div>';
h+='<div class="pg"><h3>BLE Manufacturer IDs ('+p.mfr.length+')</h3><div class="it">'+p.mfr.map(m=>'<span>0x'+m.toString(16).toUpperCase().padStart(4,'0')+'</span>').join('')+'</div></div>';
h+='<div class="pg"><h3>Raven UUIDs ('+p.raven.length+')</h3><div class="it">'+p.raven.map(u=>'<span style="font-size:8px">'+esc(u)+'</span>').join('')+'</div></div>';
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
function loadSettings(){fetch('/api/settings').then(r=>r.json()).then(s=>{document.getElementById('nR').value=s.pathloss_n10;document.getElementById('nV').textContent=s.pathloss_n.toFixed(1);document.getElementById('txR').value=s.txpower;document.getElementById('txV').textContent=s.txpower;}).catch(()=>{});}
function saveSettings(){var fd=new FormData();fd.append('n10',document.getElementById('nR').value);fd.append('txpower',document.getElementById('txR').value);fetch('/api/settings',{method:'POST',body:fd}).then(r=>r.json()).then(()=>{alert('Radio settings saved. New detections will use updated model.');}).catch(()=>{alert('Save failed');});}
function loadLocations(){var el=document.getElementById('triInfo');el.textContent='Calculating...';fetch('/api/locations').then(r=>r.json()).then(s=>{if(s.count===0){el.innerHTML='No triangulated positions yet. Need 3+ GPS-tagged sightings per MAC.';return;}var h='<b>'+s.count+' source(s)</b> (n='+s.pathloss_n.toFixed(1)+', tx='+s.txpower+'dBm)<br/>';s.sources.forEach(function(src){h+='<div style="margin:4px 0;padding:4px;border:1px solid rgba(139,92,246,.25);border-radius:4px"><b>'+esc(src.mac)+'</b> '+src.lat.toFixed(6)+','+src.lon.toFixed(6)+' (RMS:'+src.rms_err_m.toFixed(0)+'m, '+src.obs+'obs)'+(src.is_raven?' <span style="color:#ef4444;font-weight:bold">RAVEN</span>':'')+'</div>';});el.innerHTML=h;}).catch(()=>{el.textContent='Error';});}
loadSettings();loadLocations();
</script></body></html>
)rawliteral";

// ============================================================================
// WEB SERVER SETUP
// ============================================================================

static void fySetupServer() {
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
        char buf[320];
        snprintf(buf, sizeof(buf),
            "{\"total\":%d,\"raven\":%d,\"ble\":\"active\","
            "\"gps_valid\":%s,\"gps_age\":%lu,\"gps_tagged\":%d,"
            "\"gps_src\":\"%s\",\"gps_sats\":%d,\"gps_hw_detected\":%s}",
            fyDetCount, raven,
            fyGPSIsFresh() ? "true" : "false",
            fyGPSValid ? (millis() - fyGPSLastUpdate) : 0UL,
            withGPS,
            gpsSrc, fyHWGPSSats,
            fyHWGPSDetected ? "true" : "false");
        r->send(200, "application/json", buf);
    });

    // API: Receive GPS from phone browser (ignored when hardware GPS has fix)
    fyServer.on("/api/gps", HTTP_GET, [](AsyncWebServerRequest *r) {
        if (fyHWGPSFix) {
            r->send(200, "application/json", "{\"status\":\"ignored\",\"reason\":\"hw_gps_active\"}");
            return;
        }
        if (r->hasParam("lat") && r->hasParam("lon")) {
            double lat = r->getParam("lat")->value().toDouble();
            double lon = r->getParam("lon")->value().toDouble();
            float  acc = r->hasParam("acc") ? r->getParam("acc")->value().toFloat() : 0;
            if (fyGPSMutex && xSemaphoreTake(fyGPSMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                fyGPSLat = lat;
                fyGPSLon = lon;
                fyGPSAcc = acc;
                fyGPSValid = true;
                fyGPSLastUpdate = millis();
                fyGPSIsHardware = false;
                xSemaphoreGive(fyGPSMutex);
                r->send(200, "application/json", "{\"status\":\"ok\"}");
            } else {
                r->send(503, "application/json", "{\"error\":\"busy\"}");
            }
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

    // API: Export CSV (downloadable file, includes GPS). Fields containing
    // quotes, commas, or newlines are wrapped/escaped per RFC 4180.
    fyServer.on("/api/export/csv", HTTP_GET, [](AsyncWebServerRequest *r) {
        AsyncResponseStream *resp = r->beginResponseStream("text/csv");
        resp->addHeader("Content-Disposition", "attachment; filename=\"flockyou_detections.csv\"");
        resp->println("mac,name,rssi,method,first_seen_ms,last_seen_ms,count,is_raven,raven_fw,latitude,longitude,gps_accuracy,distance_m,min_distance_m,max_distance_m");
        if (fyMutex && xSemaphoreTake(fyMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
            // Worst case: every byte becomes "" (2x) plus surrounding quotes
            char macEsc[18 * 2 + 3];
            char nameEsc[48 * 2 + 3];
            char methodEsc[24 * 2 + 3];
            char fwEsc[16 * 2 + 3];
            for (int i = 0; i < fyDetCount; i++) {
                FYDetection& d = fyDet[i];
                fyCsvEscape(macEsc,    sizeof(macEsc),    d.mac);
                fyCsvEscape(nameEsc,   sizeof(nameEsc),   d.name);
                fyCsvEscape(methodEsc, sizeof(methodEsc), d.method);
                fyCsvEscape(fwEsc,     sizeof(fwEsc),     d.ravenFW);
                if (d.hasGPS) {
                    resp->printf("%s,%s,%d,%s,%lu,%lu,%d,%s,%s,%.8f,%.8f,%.1f,%.1f,%.1f,%.1f\n",
                        macEsc, nameEsc, d.rssi, methodEsc,
                        d.firstSeen, d.lastSeen, d.count,
                        d.isRaven ? "true" : "false", fwEsc,
                        d.gpsLat, d.gpsLon, d.gpsAcc,
                        d.lastDistM, d.minDistM, d.maxDistM);
                } else {
                    resp->printf("%s,%s,%d,%s,%lu,%lu,%d,%s,%s,,,,%.1f,%.1f,%.1f\n",
                        macEsc, nameEsc, d.rssi, methodEsc,
                        d.firstSeen, d.lastSeen, d.count,
                        d.isRaven ? "true" : "false", fwEsc,
                        d.lastDistM, d.minDistM, d.maxDistM);
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

    // API: Prior session history (JSON)
    fyServer.on("/api/history", HTTP_GET, [](AsyncWebServerRequest *r) {
        if (fySpiffsReady && SPIFFS.exists(FY_PREV_FILE)) {
            r->send(SPIFFS, FY_PREV_FILE, "application/json");
        } else {
            r->send(200, "application/json", "[]");
        }
    });

    // API: Download prior session as JSON file
    fyServer.on("/api/history/json", HTTP_GET, [](AsyncWebServerRequest *r) {
        if (fySpiffsReady && SPIFFS.exists(FY_PREV_FILE)) {
            AsyncWebServerResponse *resp = r->beginResponse(SPIFFS, FY_PREV_FILE, "application/json");
            resp->addHeader("Content-Disposition", "attachment; filename=\"flockyou_prev_session.json\"");
            r->send(resp);
        } else {
            r->send(404, "application/json", "{\"error\":\"no prior session\"}");
        }
    });

    // API: Download prior session as KML (reads JSON from SPIFFS, converts)
    fyServer.on("/api/history/kml", HTTP_GET, [](AsyncWebServerRequest *r) {
        if (!fySpiffsReady || !SPIFFS.exists(FY_PREV_FILE)) {
            r->send(404, "application/json", "{\"error\":\"no prior session\"}");
            return;
        }
        File f = SPIFFS.open(FY_PREV_FILE, "r");
        if (!f) { r->send(500, "text/plain", "read error"); return; }
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
		if (!gps || gps["lat"].isNull()) continue;
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

    // API: Triangulated source locations. For every MAC with >=3 GPS-tagged
    // observations, run Gauss-Newton least-squares to estimate where the
    // transmitter actually is. Returns a JSON array of source estimates.
    fyServer.on("/api/locations", HTTP_GET, [](AsyncWebServerRequest *r) {
        AsyncResponseStream *resp = r->beginResponseStream("application/json");
        resp->print("{\"sources\":[");
        int emitted = 0;
        if (fyMutex && xSemaphoreTake(fyMutex, pdMS_TO_TICKS(300)) == pdTRUE) {
            char macEsc[18 * 6 + 1];
            for (int i = 0; i < fyDetCount; i++) {
                FYDetection& d = fyDet[i];
                if (d.obsCount < 3 || !d.obs) continue;
                double srcLat, srcLon, rmsErr;
                if (fyTriangulate(d.obs, d.obsCount, srcLat, srcLon, rmsErr)) {
                    if (emitted > 0) resp->print(",");
                    fyJsonEscape(macEsc, sizeof(macEsc), d.mac);
                    resp->printf(
                        "{\"mac\":\"%s\",\"lat\":%.8f,\"lon\":%.8f,"
                        "\"rms_err_m\":%.1f,\"obs\":%d,"
                        "\"is_raven\":%s,\"last_rssi\":%d,\"last_dist_m\":%.1f}",
                        macEsc, srcLat, srcLon, rmsErr, d.obsCount,
                        d.isRaven ? "true" : "false", d.rssi, d.lastDistM);
                    emitted++;
                }
            }
            xSemaphoreGive(fyMutex);
        }
        resp->printf("],\"count\":%d,\"pathloss_n\":%.1f,\"txpower\":%d}",
                     emitted, fyPathLossExp10 / 10.0, fyTxPowerDbm);
        r->send(resp);
    });

    // API: Path-loss model settings (GET = read, POST = write).
    fyServer.on("/api/settings", HTTP_GET, [](AsyncWebServerRequest *r) {
        char buf[128];
        snprintf(buf, sizeof(buf),
            "{\"pathloss_n10\":%d,\"pathloss_n\":%.1f,\"txpower\":%d}",
            fyPathLossExp10, fyPathLossExp10 / 10.0, fyTxPowerDbm);
        r->send(200, "application/json", buf);
    });
    fyServer.on("/api/settings", HTTP_POST, [](AsyncWebServerRequest *r) {
        bool changed = false;
        int n10 = fyPathLossExp10;
        int txpw = fyTxPowerDbm;
        if (r->hasParam("n10", true)) {
            n10 = r->getParam("n10", true)->value().toInt();
            changed = true;
        }
        if (r->hasParam("txpower", true)) {
            txpw = r->getParam("txpower", true)->value().toInt();
            changed = true;
        }
        if (changed) {
            fySavePathLoss(n10, txpw);
            r->send(200, "application/json", "{\"status\":\"saved\"}");
        } else {
            r->send(400, "application/json", "{\"error\":\"provide n10 and/or txpower\"}");
        }
    });

    // API: Clear all detections (saves current session first)
    fyServer.on("/api/clear", HTTP_GET, [](AsyncWebServerRequest *r) {
        fySaveSession();  // Persist before clearing
        if (fyMutex && xSemaphoreTake(fyMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
            // Snapshot obs pointers (allocated once from PSRAM in fySetupObservations)
            // so the memset below doesn't strand the backing buffers.
            FYObservation* savedObs[MAX_DETECTIONS];
            for (int i = 0; i < MAX_DETECTIONS; i++) savedObs[i] = fyDet[i].obs;
            fyDetCount = 0;
            memset(fyDet, 0, sizeof(fyDet));
            for (int i = 0; i < MAX_DETECTIONS; i++) fyDet[i].obs = savedObs[i];
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

    fyServer.begin();
    printf("[FLOCK-YOU] Web server started on port 80\n");
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

    fyMutex = xSemaphoreCreateMutex();
    fyGPSMutex = xSemaphoreCreateMutex();

    // Allocate per-MAC observation ring buffers (PSRAM preferred, ~77 KB).
    fySetupObservations();

    // Load path-loss model parameters from NVS.
    fyLoadPathLoss();

    // Init hardware GPS UART (Seeed L76K on D6/D7)
    fyGPSSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);

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
    printf("  GPS: auto-detect (L76K on D6/D7)\n");
    printf("========================================\n");

    // Init BLE scanner FIRST -- start scanning immediately
    NimBLEDevice::init("");
    fyBLEScan = NimBLEDevice::getScan();
    fyBLEScan->setAdvertisedDeviceCallbacks(new FYBLECallbacks());
    fyBLEScan->setActiveScan(true);
    fyBLEScan->setInterval(100);
    fyBLEScan->setWindow(99);

    // Kick off the first scan right away (non-blocking)
    fyBLEScan->start(BLE_SCAN_DURATION, fyScanComplete, false);
    fyLastBleScan = millis();
    printf("[FLOCK-YOU] BLE scanning ACTIVE\n");

    // Crow calls play WHILE BLE is already scanning
    fyBootBeep();

    // Start WiFi AP (no need to connect to anything -- AP only)
    WiFi.mode(WIFI_AP);
    delay(100);
    WiFi.softAP(FY_AP_SSID, FY_AP_PASS);
    printf("[FLOCK-YOU] AP: %s / %s\n", FY_AP_SSID, FY_AP_PASS);
    printf("[FLOCK-YOU] IP: %s\n", WiFi.softAPIP().toString().c_str());

    // Captive portal DNS - redirect all DNS queries to our AP IP
    flockyouDNS.start(53, "*", WiFi.softAPIP());
    printf("[FLOCK-YOU] Captive portal DNS started\n");

    // Start web dashboard
    fySetupServer();

    printf("[FLOCK-YOU] Detection methods: MAC prefix, device name, manufacturer ID, Raven UUID\n");
    printf("[FLOCK-YOU] Dashboard: http://192.168.4.1\n");
    printf("[FLOCK-YOU] Ready - no WiFi connection needed, BLE + AP only\n\n");
}

// BLE scan completion callback -- fires when a non-blocking scan finishes.
// Clears results so we don't keep advertisement memory between cycles.
// Runs on the NimBLE host task, not loop(), so it must not do heavy work.
static void fyScanComplete(NimBLEScanResults /*results*/) {
    if (fyBLEScan) fyBLEScan->clearResults();
}

void loop() {
    flockyouDNS.processNextRequest();  // Captive portal DNS
    fyProcessHardwareGPS();
    fyUpdatePixel();

    // BLE scanning cycle -- non-blocking form with a completion callback so
    // GPS NMEA parsing, DNS, and NeoPixel animation keep running during the
    // scan window. start(duration, cb, continue) returns immediately.
    if (millis() - fyLastBleScan >= BLE_SCAN_INTERVAL && !fyBLEScan->isScanning()) {
        fyBLEScan->start(BLE_SCAN_DURATION, fyScanComplete, false);
        fyLastBleScan = millis();
    }

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

    // Auto-save session to SPIFFS every 15s if detections changed
    // Also triggers an early save 5s after first detection to minimize loss on power-cycle
    if (fySpiffsReady && millis() - fyLastSave >= FY_SAVE_INTERVAL) {
        if (fyDetCount > 0 && fyDetCount != fyLastSaveCount) {
            fySaveSession();
        }
        fyLastSave = millis();
    } else if (fySpiffsReady && fyDetCount > 0 && fyLastSaveCount == 0 &&
               millis() - fyLastSave >= 5000) {
        // Quick first-save: persist within 5s of first detection
        fySaveSession();
        fyLastSave = millis();
    }

    delay(100);
}
