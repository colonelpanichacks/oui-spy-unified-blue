#line 1 "/Users/nicholastenbrink/projects/nesso/oui_spy_nesso/raw/megamaid.cpp"
// ============================================================================
// MEGA-MAID: BLE + WiFi MAC/OUI Collector with Web Dashboard
// ============================================================================
// Collects every BLE advertisement and WiFi unicast MAC (no watchlist).
//
// Default: DASHBOARD mode — softAP "megamaid" + web export on boot.
// Triple-click KEY1 toggles COLLECTION mode (50% BLE / 50% WiFi ch 1–14; AP off).
// Double-click KEY1 pauses/resumes scanning while in COLLECTION mode (AP stays off).
// Hold KEY1 ~1.5s (handled in main.cpp) still returns to the mode selector.
// AP "megamaid" / "megamaid123"
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
#include "esp_wifi.h"
#include "esp_heap_caps.h"

// ============================================================================
// CONFIGURATION
// ============================================================================
#define MM_NEOPIXEL_BRIGHTNESS 50
#define MM_NEOPIXEL_DETECTION_BRIGHTNESS 200

// Audio
#define LOW_FREQ 200
#define HIGH_FREQ 800
#define DETECT_FREQ 1000
#define HEARTBEAT_FREQ 600
#define BOOT_BEEP_DURATION 300
#define DETECT_BEEP_DURATION 150
#define HEARTBEAT_DURATION 100

// BLE scanning
#define BLE_SCAN_DURATION 2      // seconds per scan burst
#define BLE_SCAN_INTERVAL 3000   // ms between clearing cached scan results
#define MM_BLE_ADV_QUEUE_SIZE 32
#define MM_BLE_ADVS_PER_LOOP 8

// ESP32-C6 radio sharing: explicit time slices (one consumer at a time).
#define MM_BLE_SLOT_MS              100
#define MM_CYCLE_MS                 1000
#define MM_COLLECT_BLE_MS           500
#define MM_COLLECT_WIFI_MS          500
#define MM_DASH_BLE_MS              500   // dashboard: alternate with WiFi sniff
#define MM_DASH_WIFI_MS             500
#define MM_BLE_DASHBOARD_WINDOW_MS   30   // C6: AP needs airtime (see foxhunter.cpp)
#define MM_BLE_DASHBOARD_INTERVAL_MS 100
#define MM_AP_SETTLE_MS             2000
#define MM_KEY1_DCLICK_MS           800
#define MM_KEY1_DCLICK_CAP_MS       1200
#define MM_LOOP_INTERVAL_MS          20
#define MM_WIFI_ALERTS_PER_LOOP       8
#define MM_BACKFILL_BATCH            25
#define MM_SAVE_CRC_BATCH            40
#define MM_SAVE_WRITE_BATCH          20
#ifdef NESSO_N1
#define MAX_DETECTIONS 300
#else
#define MAX_DETECTIONS 500
#endif
#define MM_NEAR_CAP_THRESHOLD       (MAX_DETECTIONS - 10)
#define MM_HTTP_MIN_HEAP            24576
#define MM_HTTP_FALLBACK_MS         30000
#define MM_DASH_SAVE_TIMEOUT_MS     60000
#define MM_HEAP_RECOVERY_INTERVAL_MS 5000
#define MM_HTTP_WATCHDOG_IDLE_MS    15000
#define MM_HTTP_WATCHDOG_RESTART_MS 10000
#define MM_HTTP_WATCHDOG_MAX_FAILS  3

// Detection storage
#define MM_MAC_HASH_SLOTS 512

// WiFi AP credentials
#define MM_AP_SSID "megamaid"
#define MM_AP_PASS "megamaid123"
#define MM_BUILD_TAG "2026-06-24m"

// ============================================================================
// DETECTION STORAGE
// ============================================================================

struct MMDetection {
    char mac[18];
    char name[48];           // BLE local name
    char ssid[33];           // last seen WiFi SSID (beacon/probe/probe_resp)
    char probedSsids[3][33]; // unique SSIDs from probe requests (familiar networks)
    uint8_t probedSsidCount;
    int rssi;
    char method[24];
    unsigned long firstSeen;
    unsigned long lastSeen;
    int count;
    uint8_t channel;   // WiFi channel (0 = BLE / N/A)
    // GPS from phone (wardriving)
    double gpsLat;
    double gpsLon;
    float gpsAcc;
    bool hasGPS;
};

static MMDetection mmDet[MAX_DETECTIONS];
static int mmDetCount = 0;
static int16_t mmMacHash[MM_MAC_HASH_SLOTS];
static SemaphoreHandle_t mmMutex = NULL;      // guards mmDet[] + mmDetCount
static SemaphoreHandle_t mmGPSMutex = NULL;   // guards mmGPS* globals

// OUI aggregation (static — avoid large stack in AsyncTCP handlers)
static char mmOuiBuf[MAX_DETECTIONS][9];
static int mmOuiCounts[MAX_DETECTIONS];

// ============================================================================
// GLOBALS
// ============================================================================

static bool mmBuzzerOn = true;
static Adafruit_NeoPixel mmPixel(1, BOARD_NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);
static bool mmPixelAlertMode = false;
static unsigned long mmPixelAlertStart = 0;
static unsigned long mmLastBleScan = 0;
static bool mmTriggered = false;
static bool mmDeviceInRange = false;
static unsigned long mmLastDetTime = 0;
static unsigned long mmLastHB = 0;
static NimBLEScan* mmBLEScan = NULL;
class MM_BLECallbacks;
static MM_BLECallbacks* mmBleCallbacks = nullptr;
static AsyncWebServer mmServer(80);
static DNSServer megamaidDNS;
static bool mmRoutesRegistered = false;
static bool mmServerStarted = false;
static bool mmDnsStarted = false;
static bool mmDashboardApUp = false;
static unsigned long mmApStartMs = 0;

enum MmRadioProfile : uint8_t {
    MM_RADIO_COLLECT = 0,    // 50/50 BLE / WiFi sniff
    MM_RADIO_DASHBOARD = 1,  // AP always on; 50/50 BLE / WiFi sniff
};

enum MmRadioSlice : uint8_t {
    MM_SLICE_BLE = 0,
    MM_SLICE_WIFI_SNIFF = 1,
    MM_SLICE_AP = 2,
};

static MmRadioProfile mmRadioProfile = MM_RADIO_DASHBOARD;
static MmRadioSlice mmCurrentSlice = MM_SLICE_BLE;
static unsigned long mmSliceStartMs = 0;
static unsigned long mmLastSliceTickMs = 0;
static bool mmKey1WasDown = false;
static uint8_t mmKey1ClickCount = 0;
static unsigned long mmKey1LastReleaseMs = 0;
static bool mmPendingDashboardTransition = false;
static bool mmDashboardEntryInProgress = false;
static bool mmBleReleasedForDashboard = false;

enum MmDashTransitionPhase : uint8_t {
    MM_DASH_X_IDLE = 0,
    MM_DASH_X_STOP_SCAN,
    MM_DASH_X_SAVE,
    MM_DASH_X_FREE_HEAP,
    MM_DASH_X_START_AP,
    MM_DASH_X_WAIT_HTTP,
};

static MmDashTransitionPhase mmDashTransPhase = MM_DASH_X_IDLE;
static unsigned long mmDashTransPhaseStart = 0;
static unsigned long mmDashHttpWaitStart = 0;
static bool mmDashHttpFallback = false;
static bool mmDashTransSkipSave = false;
static unsigned long mmHeapRecoveryLastMs = 0;
static bool mmApRecycleAttempted = false;
static bool mmDashHttpRestartPending = false;
static unsigned long mmServerStartMs = 0;
static unsigned long mmServerLastRequestMs = 0;
static unsigned long mmHttpWatchdogLastRestartMs = 0;
static uint8_t mmHttpWatchdogFailCount = 0;

static unsigned long mmCollectWifiReadyMs = 0;
static unsigned long mmLastBleBurst = 0;
static bool mmSaveRadioPaused = false;
static bool mmCollectScanPaused = false;

struct MmBleAdvItem {
    char mac[18];
    char name[48];
    int rssi;
};
static MmBleAdvItem mmBleAdvQueue[MM_BLE_ADV_QUEUE_SIZE];
static uint8_t mmBleAdvHead = 0;
static uint8_t mmBleAdvTail = 0;
static uint8_t mmBleAdvCount = 0;

// Phone GPS state (updated via browser Geolocation API -> /api/gps)
static double mmGPSLat = 0;
static double mmGPSLon = 0;
static float  mmGPSAcc = 0;
static bool   mmGPSValid = false;
static unsigned long mmGPSLastUpdate = 0;
static bool mmGPSIsHardware = false;
#define GPS_STALE_MS 30000

// Hardware GPS (UART1; board pins/baud in board_gps.h)
static TinyGPSPlus mmGPS;
static HardwareSerial mmGPSSerial(1);
static bool mmHWGPSDetected = false;
static bool mmHWGPSFix = false;
static int  mmHWGPSSats = 0;
static unsigned long mmHWGPSLastChar = 0;
#define GPS_HW_TIMEOUT_MS 5000

// Session persistence (SPIFFS)
#define MM_SESSION_FILE  "/megamaid_session.json"
#define MM_PREV_FILE     "/megamaid_prev_session.json"
#define MM_EXPORT_BODY_TMP "/megamaid_export.json.tmp"
#define MM_SAVE_INTERVAL 15000  // Auto-save every 15 seconds (prevent data loss on quick power-cycle)
static unsigned long mmLastSave = 0;
static int mmLastSaveCount = 0;  // Track changes to avoid unnecessary writes
static bool mmSpiffsReady = false;
static bool mmCapDropLogged = false;
static bool mmNearCapShown = false;
static int mmBackfillCursor = 0;

enum MmSavePhase : uint8_t {
    MM_SAVE_IDLE = 0,
    MM_SAVE_CRC,
    MM_SAVE_WRITE,
    MM_SAVE_VERIFY,
    MM_SAVE_PROMOTE,
};
static MmSavePhase mmSavePhase = MM_SAVE_IDLE;
static bool mmSavePending = false;
static bool mmSaveSyncWait = false;
static int mmSaveSnapCount = 0;
static int mmSaveProgressIdx = 0;
static uint32_t mmSaveCrc = 0;
static size_t mmSavePayloadBytes = 0;
static File mmSaveFile;

// Streaming export state (single in-flight export per device)
static struct {
    File f;
    bool active;
} mmBodyStream;

static struct {
    File f;
    bool active;
    int offset;
    int limit;
    int skipped;
    int emitted;
    uint8_t phase;
} mmDetPageStream;

static struct {
    bool active;
    int detIdx;
    int total;
} mmRamJsonExport;

static struct {
    bool active;
    int detIdx;
    int total;
    bool headerSent;
} mmRamCsvExport;

static struct {
    bool active;
    int detIdx;
    int total;
    uint8_t phase;  // 0=header, 1=body, 2=footer
} mmRamKmlExport;

static struct {
    File f;
    char objBuf[768];
    bool active;
    bool headerSent;
    bool done;
} mmHistCsvExport;

static struct {
    File f;
    char objBuf[768];
    bool active;
    uint8_t phase;  // 0=kml header, 1=placemarks, 2=footer
    bool done;
} mmHistKmlExport;

struct MmBeepState {
    bool active;
    uint8_t type;   // 1=detect, 2=heartbeat
    uint8_t phase;
    unsigned long nextAt;
};
static MmBeepState mmBeepSt = {false, 0, 0, 0};
static unsigned long mmLastLoopEnd = 0;

static void mmRequestSave();
static void mmSaveTick();
static void mmSaveSyncWaitDone();
static bool mmSaveBusy();
static void mmEnterBleSlice();
static void mmEnterSlice(MmRadioSlice slice);
static void mmBeepTick();
static void mmQueueDetectBeep();
static void mmQueueHeartbeatBeep();
static void mmMacHashClear();
static int mmMacHashFind(const char* mac);
static void mmMacHashInsert(const char* mac, int idx);
static void mmYieldMs(unsigned long ms);
static bool mmDashboardTransitionActive();
static void mmDashboardTransitionTick();
static void mmBeginDashboardTransition(bool skipSave);
static void mmPrepareHeapForHttp();
static void mmRecoverDashboardRadioForHttp();
static void mmCancelDashboardTransition(const char* reason);
static void mmPollKey1DoubleClick();
static unsigned long mmKey1DclickWindowMs();
static bool mmHttpHeapReady();
static void mmPollButtonsDuringSave();

// ============================================================================
// AUDIO SYSTEM
// ============================================================================

static void mmBeep(int freq, int dur) {
    if (!mmBuzzerOn) return;
    tone(BUZZER_PIN, freq, dur);
    delay(dur + 50);
}

// Crow caw: harsh descending sweep with warble texture
static void mmCaw(int startFreq, int endFreq, int durationMs, int warbleHz) {
    if (!mmBuzzerOn) return;
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

static void mmBootBeep() {
    printf("[MEGA-MAID] Boot sound (buzzer %s)\n", mmBuzzerOn ? "ON" : "OFF");
    if (!mmBuzzerOn) return;

    // === CROW CALL SEQUENCE ===
    // Caw 1: sharp descending caw
    mmCaw(850, 380, 180, 40);
    delay(100);

    // Caw 2: slightly lower, shorter
    mmCaw(780, 350, 150, 50);
    delay(100);

    // Caw 3: longer trailing caw with more rasp
    mmCaw(820, 280, 220, 60);
    delay(80);

    // Quick staccato ending "kk-kk"
    tone(BUZZER_PIN, 600, 25); delay(40);
    tone(BUZZER_PIN, 550, 25); delay(40);
    noTone(BUZZER_PIN);

    printf("[MEGA-MAID] *caw caw caw*\n");
}

static void mmBeepTick() {
    if (!mmBeepSt.active) return;
    unsigned long now = millis();
    if (now < mmBeepSt.nextAt) return;

    if (!mmBuzzerOn) {
        noTone(BUZZER_PIN);
        mmBeepSt.active = false;
        return;
    }

    if (mmBeepSt.type == 1) {
        switch (mmBeepSt.phase) {
            case 0: tone(BUZZER_PIN, 600, 80); mmBeepSt.nextAt = now + 100; mmBeepSt.phase = 1; break;
            case 1: tone(BUZZER_PIN, 900, 80); mmBeepSt.nextAt = now + 100; mmBeepSt.phase = 2; break;
            case 2: tone(BUZZER_PIN, 400, 150); mmBeepSt.nextAt = now + 180; mmBeepSt.phase = 3; break;
            default: noTone(BUZZER_PIN); mmBeepSt.active = false; break;
        }
    } else if (mmBeepSt.type == 2) {
        switch (mmBeepSt.phase) {
            case 0: tone(BUZZER_PIN, 500, 80); mmBeepSt.nextAt = now + 120; mmBeepSt.phase = 1; break;
            case 1: tone(BUZZER_PIN, 480, 80); mmBeepSt.nextAt = now + 120; mmBeepSt.phase = 2; break;
            default: noTone(BUZZER_PIN); mmBeepSt.active = false; break;
        }
    }
}

static void mmQueueDetectBeep() {
    printf("[MEGA-MAID] Detection alert!\n");
    mmPixelAlertMode = true;
    mmPixelAlertStart = millis();
    if (!mmBuzzerOn) return;
    if (mmBeepSt.active) return;
    mmBeepSt.active = true;
    mmBeepSt.type = 1;
    mmBeepSt.phase = 0;
    mmBeepSt.nextAt = millis();
}

static void mmQueueHeartbeatBeep() {
    if (!mmBuzzerOn || mmDetCount >= 400) return;
    if (mmBeepSt.active) return;
    mmBeepSt.active = true;
    mmBeepSt.type = 2;
    mmBeepSt.phase = 0;
    mmBeepSt.nextAt = millis();
}

// Legacy names used by side-effect path
static void mmDetectBeep() { mmQueueDetectBeep(); }
static void mmHeartbeat() { mmQueueHeartbeatBeep(); }

// ============================================================================
// NEOPIXEL FUNCTIONS
// ============================================================================

static uint32_t mmHsvToRgb(uint16_t h, uint8_t s, uint8_t v) {
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
    return mmPixel.Color(r, g, b);
}

// Idle: slow purple breathing (hue 270)
static void mmPixelBreathing() {
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
    uint32_t color = mmHsvToRgb(270, 255, (uint8_t)(MM_NEOPIXEL_BRIGHTNESS * brightness));
    mmPixel.setPixelColor(0, color);
    mmPixel.show();
}

// Detection: 3 rapid flashes red->pink->red (~750ms total)
static void mmPixelDetection() {
    unsigned long elapsed = millis() - mmPixelAlertStart;
    int flashIdx = elapsed / 250;
    if (flashIdx >= 3) {
        mmPixelAlertMode = false;
        return;
    }
    uint16_t hue = (flashIdx == 1) ? 300 : 0; // pink middle, red bookends
    bool bright = ((elapsed % 250) < 150);
    uint8_t val = bright ? MM_NEOPIXEL_DETECTION_BRIGHTNESS : (MM_NEOPIXEL_BRIGHTNESS / 4);
    mmPixel.setPixelColor(0, mmHsvToRgb(hue, 255, val));
    mmPixel.show();
}

// Device in range: dim steady pink glow (hue 300)
static void mmPixelHeartbeat() {
    mmPixel.setPixelColor(0, mmHsvToRgb(300, 255, 30));
    mmPixel.show();
}

// Dispatcher: called each loop iteration
static void mmUpdatePixel() {
    if (mmPixelAlertMode) {
        mmPixelDetection();
    } else if (mmDeviceInRange) {
        mmPixelHeartbeat();
    } else {
        mmPixelBreathing();
    }
}

// ============================================================================
// GPS HELPERS (mutex-protected snapshot pattern)
// ============================================================================

// Fast advisory check — safe lock-free (for UI/stats only, don't trust for writes)
static bool mmGPSIsFresh() {
    return mmGPSValid && (millis() - mmGPSLastUpdate < GPS_STALE_MS);
}

static void mmUpdateGpsIndicator() {
    nessoUiSetGpsIndicator(
        mmGPSIsFresh() ? NESSO_GPS_FIX : NESSO_GPS_SEARCHING);
}

// Atomic snapshot: returns true and fills out-params if GPS is fresh & valid.
// Safe to call from BLE callback context — never races with producer.
static bool mmGPSSnapshot(double& lat, double& lon, float& acc) {
    if (!mmGPSMutex) return false;
    if (xSemaphoreTake(mmGPSMutex, pdMS_TO_TICKS(20)) != pdTRUE) return false;
    bool fresh = mmGPSValid && (millis() - mmGPSLastUpdate < GPS_STALE_MS);
    if (fresh) { lat = mmGPSLat; lon = mmGPSLon; acc = mmGPSAcc; }
    xSemaphoreGive(mmGPSMutex);
    return fresh;
}

// Atomic GPS publish (hardware OR phone). fromHardware=true sticky-locks out phone GPS.
static void mmGPSUpdate(double lat, double lon, float acc, bool fromHardware) {
    if (!mmGPSMutex) return;
    if (xSemaphoreTake(mmGPSMutex, pdMS_TO_TICKS(20)) != pdTRUE) return;
    mmGPSLat = lat;
    mmGPSLon = lon;
    mmGPSAcc = acc;
    mmGPSValid = true;
    mmGPSLastUpdate = millis();
    mmGPSIsHardware = fromHardware;
    xSemaphoreGive(mmGPSMutex);
    mmUpdateGpsIndicator();
}

// Stamp a detection with current GPS if available (used at first-sight and re-sight)
static void mmAttachGPS(MMDetection& d) {
    double lat, lon;
    float acc;
    if (mmGPSSnapshot(lat, lon, acc)) {
        d.hasGPS = true;
        d.gpsLat = lat;
        d.gpsLon = lon;
        d.gpsAcc = acc;
    }
}

// Periodic: back-fill GPS incrementally (few entries per tick).
static void mmBackfillGPS() {
    double lat, lon;
    float acc;
    if (!mmGPSSnapshot(lat, lon, acc)) return;
    if (!mmMutex || xSemaphoreTake(mmMutex, pdMS_TO_TICKS(20)) != pdTRUE) return;
    int filled = 0;
    int checked = 0;
    while (checked < MM_BACKFILL_BATCH && mmDetCount > 0) {
        if (mmBackfillCursor >= mmDetCount) mmBackfillCursor = 0;
        MMDetection& d = mmDet[mmBackfillCursor++];
        checked++;
        if (!d.hasGPS) {
            d.hasGPS = true;
            d.gpsLat = lat;
            d.gpsLon = lon;
            d.gpsAcc = acc;
            filled++;
        }
    }
    xSemaphoreGive(mmMutex);
    if (filled) printf("[MEGA-MAID] GPS backfilled %d detection(s)\n", filled);
}

// ============================================================================
// CRC32 (IEEE 802.3) — for session file integrity
// ============================================================================

static uint32_t mmCRC32Update(uint32_t crc, const uint8_t* data, size_t len) {
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

static void mmProcessHardwareGPS() {
    // Read all available UART bytes into TinyGPSPlus parser
    while (mmGPSSerial.available()) {
        char c = mmGPSSerial.read();
        mmGPS.encode(c);
        mmHWGPSLastChar = millis();
        if (!mmHWGPSDetected) {
            mmHWGPSDetected = true;
            printf("[MEGA-MAID] Hardware GPS module detected (NMEA data received)\n");
        }
    }

    // Timeout: no NMEA data for 5s → module disconnected or absent
    if (mmHWGPSDetected && (millis() - mmHWGPSLastChar > GPS_HW_TIMEOUT_MS)) {
        if (mmGPSIsHardware) {
            printf("[MEGA-MAID] Hardware GPS timeout — falling back to phone GPS\n");
        }
        mmHWGPSDetected = false;
        mmHWGPSFix = false;
        mmHWGPSSats = 0;
        mmGPSIsHardware = false;
    }

    // Update satellite count whenever available
    if (mmGPS.satellites.isUpdated()) {
        mmHWGPSSats = mmGPS.satellites.value();
    }

    // Update position when valid fix is available (atomic publish under mmGPSMutex)
    if (mmGPS.location.isUpdated() && mmGPS.location.isValid()) {
        if (!mmHWGPSFix) {
            printf("[MEGA-MAID] First GPS fix acquired! Sats:%d Lat:%.6f Lon:%.6f\n",
                   mmHWGPSSats, mmGPS.location.lat(), mmGPS.location.lng());
        }
        mmHWGPSFix = true;
        float acc = mmGPS.hdop.isValid()
            ? (float)(mmGPS.hdop.hdop() * GPS_HDOP_SCALE) : 10.0f;
        mmGPSUpdate(mmGPS.location.lat(), mmGPS.location.lng(), acc, /*fromHardware=*/true);
    } else if (mmHWGPSFix && mmGPS.location.isValid()) {
        // Keep timestamp fresh while fix held (bump under lock via mmGPSUpdate)
        if (mmGPSMutex && xSemaphoreTake(mmGPSMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            mmGPSLastUpdate = millis();
            xSemaphoreGive(mmGPSMutex);
        }
    }
    mmUpdateGpsIndicator();
}

// ============================================================================
// DETECTION MANAGEMENT
// ============================================================================

static uint32_t mmMacHashKey(const char* mac) {
    uint32_t h = 2166136261u;
    for (const char* p = mac; *p; p++) {
        h ^= (uint8_t)tolower((unsigned char)*p);
        h *= 16777619u;
    }
    return h;
}

static void mmMacHashClear() {
    for (int i = 0; i < MM_MAC_HASH_SLOTS; i++) {
        mmMacHash[i] = -1;
    }
}

static void mmMacHashInsert(const char* mac, int idx) {
    uint32_t h = mmMacHashKey(mac);
    for (int probe = 0; probe < MM_MAC_HASH_SLOTS; probe++) {
        int slot = (int)((h + (uint32_t)probe) % MM_MAC_HASH_SLOTS);
        if (mmMacHash[slot] < 0) {
            mmMacHash[slot] = (int16_t)idx;
            return;
        }
    }
}

static int mmMacHashFind(const char* mac) {
    uint32_t h = mmMacHashKey(mac);
    for (int probe = 0; probe < MM_MAC_HASH_SLOTS; probe++) {
        int slot = (int)((h + (uint32_t)probe) % MM_MAC_HASH_SLOTS);
        if (mmMacHash[slot] < 0) return -1;
        int idx = mmMacHash[slot];
        if (idx >= 0 && idx < mmDetCount && strcasecmp(mmDet[idx].mac, mac) == 0) {
            return idx;
        }
    }
    return -1;
}

static void mmUpdateCapStatusUnlocked() {
    if (mmDetCount >= MAX_DETECTIONS) {
        if (!mmCapDropLogged) {
            mmCapDropLogged = true;
            printf("[MEGA-MAID] CAP FULL (%d) — new unique MACs dropped\n", MAX_DETECTIONS);
            char capMsg[24];
            snprintf(capMsg, sizeof(capMsg), "Cap full (%d)", MAX_DETECTIONS);
            nessoUiSetStatus(capMsg);
            mmRequestSave();
        }
    } else if (mmDetCount >= MM_NEAR_CAP_THRESHOLD) {
        if (!mmNearCapShown) {
            mmNearCapShown = true;
            nessoUiSetStatus("Near cap");
        }
    } else {
        mmCapDropLogged = false;
        mmNearCapShown = false;
        nessoUiRestoreProfile();
    }
}

static void mmSanitizeCopy(char* dst, size_t dstCap, const char* src) {
    if (!dst || dstCap == 0) return;
    dst[0] = '\0';
    if (!src) return;
    for (size_t j = 0; j < dstCap - 1 && src[j]; j++) {
        dst[j] = (src[j] == '"' || src[j] == '\\') ? '_' : src[j];
    }
    dst[dstCap - 1] = '\0';
}

// Append a JSON string literal (including quotes) to dst. Returns false on overflow.
static bool mmJsonAppendQuoted(char* dst, size_t cap, size_t* off, const char* src) {
    if (!dst || !off || !src || *off >= cap) return false;
    if (*off + 1 >= cap) return false;
    dst[(*off)++] = '"';
    for (const char* p = src; *p; p++) {
        unsigned char c = (unsigned char)*p;
        char chunk[7];
        size_t n = 0;
        if (c == '"' || c == '\\') {
            chunk[0] = '\\';
            chunk[1] = (char)c;
            n = 2;
        } else if (c < 0x20) {
            snprintf(chunk, sizeof(chunk), "\\u%04x", c);
            n = strlen(chunk);
        } else {
            chunk[0] = (char)c;
            n = 1;
        }
        if (*off + n >= cap) return false;
        memcpy(dst + *off, chunk, n);
        *off += n;
    }
    if (*off + 1 >= cap) return false;
    dst[(*off)++] = '"';
    dst[*off] = '\0';
    return true;
}

static void mmPrintJsonString(Print* out, const char* src) {
    if (!out || !src) return;
    out->print('"');
    for (const char* p = src; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') {
            out->print('\\');
            out->print((char)c);
        } else if (c < 0x20) {
            out->printf("\\u%04x", c);
        } else {
            out->print((char)c);
        }
    }
    out->print('"');
}

static void mmPrintHtmlEscaped(Print* out, const char* src) {
    if (!out || !src) return;
    for (const char* p = src; *p; p++) {
        switch (*p) {
            case '&': out->print("&amp;"); break;
            case '<': out->print("&lt;"); break;
            case '>': out->print("&gt;"); break;
            case '"': out->print("&quot;"); break;
            default: out->print(*p); break;
        }
    }
}

static size_t mmHtmlEscapeToBuf(const char* src, char* dst, size_t cap) {
    if (!dst || cap == 0) return 0;
    size_t off = 0;
    if (!src) {
        dst[0] = '\0';
        return 0;
    }
    for (const char* p = src; *p; p++) {
        const char* rep = nullptr;
        switch (*p) {
            case '&': rep = "&amp;"; break;
            case '<': rep = "&lt;"; break;
            case '>': rep = "&gt;"; break;
            case '"': rep = "&quot;"; break;
            default: break;
        }
        if (rep) {
            size_t n = strlen(rep);
            if (off + n >= cap) return 0;
            memcpy(dst + off, rep, n);
            off += n;
        } else {
            if (off + 1 >= cap) return 0;
            dst[off++] = *p;
        }
    }
    if (off >= cap) return 0;
    dst[off] = '\0';
    return off;
}

static bool mmJsonAppendField(char* dst, size_t cap, size_t* off, const char* key, const char* value) {
    if (!value || !value[0]) return true;
    int n = snprintf(dst + *off, cap - *off, ",\"%s\":", key);
    if (n <= 0 || (size_t)n >= cap - *off) return false;
    *off += (size_t)n;
    return mmJsonAppendQuoted(dst, cap, off, value);
}

static void mmAddProbedSsid(MMDetection& d, const char* ssid) {
    if (!ssid || !ssid[0]) return;
    for (int i = 0; i < (int)d.probedSsidCount; i++) {
        if (strcmp(d.probedSsids[i], ssid) == 0) return;
    }
    if (d.probedSsidCount < 3) {
        mmSanitizeCopy(d.probedSsids[d.probedSsidCount], sizeof(d.probedSsids[0]), ssid);
        d.probedSsidCount++;
    }
}

static void mmApplyWifiSsid(MMDetection& d, const char* method,
                            const char* wifiSsid, bool hasWifiSsid) {
    if (!hasWifiSsid) return;
    mmSanitizeCopy(d.ssid, sizeof(d.ssid), wifiSsid ? wifiSsid : "");
    if (method && strcmp(method, "wifi_probe") == 0 && wifiSsid && wifiSsid[0]) {
        mmAddProbedSsid(d, wifiSsid);
    }
}

static int mmAddDetection(const char* mac, const char* name, int rssi,
                          const char* method, uint8_t channel = 0,
                          const char* wifiSsid = nullptr, bool hasWifiSsid = false,
                          bool* outIsNew = nullptr) {
    if (!mmMutex || xSemaphoreTake(mmMutex, pdMS_TO_TICKS(20)) != pdTRUE) return -1;

    int existing = mmMacHashFind(mac);
    if (existing >= 0) {
        MMDetection& d = mmDet[existing];
        d.count++;
        d.lastSeen = millis();
        d.rssi = rssi;
        if (channel > 0) d.channel = channel;
        if (name && name[0]) {
            mmSanitizeCopy(d.name, sizeof(d.name), name);
        }
        mmApplyWifiSsid(d, method, wifiSsid, hasWifiSsid);
        mmAttachGPS(d);
        if (outIsNew) *outIsNew = false;
        xSemaphoreGive(mmMutex);
        return existing;
    }

    if (mmDetCount < MAX_DETECTIONS) {
        MMDetection& d = mmDet[mmDetCount];
        memset(&d, 0, sizeof(d));
        strncpy(d.mac, mac, sizeof(d.mac) - 1);
        d.mac[sizeof(d.mac) - 1] = '\0';
        mmSanitizeCopy(d.name, sizeof(d.name), name);
        mmApplyWifiSsid(d, method, wifiSsid, hasWifiSsid);
        d.rssi = rssi;
        d.channel = channel;
        strncpy(d.method, method, sizeof(d.method) - 1);
        d.method[sizeof(d.method) - 1] = '\0';
        d.firstSeen = millis();
        d.lastSeen = d.firstSeen;
        d.count = 1;
        mmAttachGPS(d);
        int idx = mmDetCount++;
        mmMacHashInsert(mac, idx);
        if (outIsNew) *outIsNew = true;
        xSemaphoreGive(mmMutex);
        nessoUiSetDetectionCount(mmDetCount);
        mmUpdateCapStatusUnlocked();
        return idx;
    }

    if (outIsNew) *outIsNew = false;
    xSemaphoreGive(mmMutex);
    mmUpdateCapStatusUnlocked();
    return -1;
}

static void mmAppendProbedSsidsJson(char* buf, size_t cap, const MMDetection& d) {
    if (!buf || cap == 0 || d.probedSsidCount == 0) return;
    size_t off = strlen(buf);
    if (off >= cap) return;
    int n = snprintf(buf + off, cap - off, ",\"probed_ssids\":[");
    if (n <= 0 || (size_t)n >= cap - off) return;
    off += (size_t)n;
    for (int i = 0; i < (int)d.probedSsidCount; i++) {
        if (i > 0) {
            if (off + 1 >= cap) return;
            buf[off++] = ',';
            buf[off] = '\0';
        }
        if (!mmJsonAppendQuoted(buf, cap, &off, d.probedSsids[i])) return;
    }
    if (off + 2 >= cap) return;
    buf[off++] = ']';
    buf[off] = '\0';
}

static void mmEmitDetectionSideEffects(int idx, const char* addrStr,
                                       const char* name, int rssi,
                                       const char* method, uint8_t channel) {
    MMDetection snap = {};
    if (idx >= 0 && idx < mmDetCount) {
        snap = mmDet[idx];
    }
    printf("[MEGA-MAID] DETECTED: %s %s RSSI:%d [%s] count:%d\n",
           addrStr, name ? name : "", rssi, method, snap.count);

    char gpsBuf[80] = "";
    {
        double sLat, sLon; float sAcc;
        if (mmGPSSnapshot(sLat, sLon, sAcc)) {
            snprintf(gpsBuf, sizeof(gpsBuf),
                ",\"gps\":{\"latitude\":%.8f,\"longitude\":%.8f,\"accuracy\":%.1f}",
                sLat, sLon, sAcc);
        }
    }
    char wifiExtra[320] = "";
    if (snap.ssid[0]) {
        size_t woff = 0;
        if (!mmJsonAppendField(wifiExtra, sizeof(wifiExtra), &woff, "wifi_ssid", snap.ssid)) {
            wifiExtra[0] = '\0';
        }
    }
    mmAppendProbedSsidsJson(wifiExtra, sizeof(wifiExtra), snap);

    if (channel > 0) {
        uint16_t freq = (channel >= 1 && channel <= 14) ? (uint16_t)(2407 + 5 * channel) : 0;
        char nameEsc[128] = "";
        size_t nameOff = 0;
        mmJsonAppendQuoted(nameEsc, sizeof(nameEsc), &nameOff, name ? name : "");
        printf("{\"detection_method\":\"%s\",\"protocol\":\"wifi_2_4ghz\","
               "\"mac_address\":\"%s\",\"device_name\":%s"
               "%s,\"rssi\":%d,\"channel\":%u,\"frequency\":%u%s}\n",
               method, addrStr, nameEsc,
               wifiExtra, rssi, (unsigned)channel, (unsigned)freq, gpsBuf);
    } else {
        char nameEsc[128] = "";
        size_t nameOff = 0;
        mmJsonAppendQuoted(nameEsc, sizeof(nameEsc), &nameOff, name ? name : "");
        printf("{\"detection_method\":\"%s\",\"protocol\":\"bluetooth_le\","
               "\"mac_address\":\"%s\",\"device_name\":%s,"
               "\"rssi\":%d%s}\n",
               method, addrStr, nameEsc, rssi, gpsBuf);
    }

    if (!mmTriggered) {
        mmTriggered = true;
    }
    if (!mmSaveBusy()) {
        mmDetectBeep();
    }
    mmDeviceInRange = true;
    mmLastDetTime = millis();
    mmLastHB = millis();
}

static void mmProcessWiFiAlerts() {
    MmWiFiAlert alert;
    int processed = 0;
    while (processed < MM_WIFI_ALERTS_PER_LOOP && mmWiFiSniffPopAlert(&alert)) {
        processed++;
        char macStr[18];
        snprintf(macStr, sizeof(macStr), "%02x:%02x:%02x:%02x:%02x:%02x",
                 alert.mac[0], alert.mac[1], alert.mac[2],
                 alert.mac[3], alert.mac[4], alert.mac[5]);
        const char* method = mmWiFiAlertMethod(alert.type);
        bool isNew = false;
        const char* wifiSsid = alert.hasSsid ? alert.ssid : nullptr;
        int idx = mmAddDetection(macStr, "", alert.rssi, method,
                                 alert.channel, wifiSsid, alert.hasSsid, &isNew);
        if (idx >= 0 && isNew) {
            mmEmitDetectionSideEffects(idx, macStr, "", alert.rssi, method,
                                       alert.channel);
        }
    }
}

// ============================================================================
// BLE SCANNING
// ============================================================================

static void mmBleAdvEnqueue(const char* mac, const char* name, int rssi) {
    if (!mac || mmBleAdvCount >= MM_BLE_ADV_QUEUE_SIZE) {
        return;
    }
    MmBleAdvItem& item = mmBleAdvQueue[mmBleAdvTail];
    strncpy(item.mac, mac, sizeof(item.mac) - 1);
    item.mac[sizeof(item.mac) - 1] = '\0';
    strncpy(item.name, name ? name : "", sizeof(item.name) - 1);
    item.name[sizeof(item.name) - 1] = '\0';
    item.rssi = rssi;
    mmBleAdvTail = (mmBleAdvTail + 1) % MM_BLE_ADV_QUEUE_SIZE;
    mmBleAdvCount++;
}

static void mmProcessBleAdverts() {
    int processed = 0;
    while (processed < MM_BLE_ADVS_PER_LOOP && mmBleAdvCount > 0) {
        MmBleAdvItem item = mmBleAdvQueue[mmBleAdvHead];
        mmBleAdvHead = (mmBleAdvHead + 1) % MM_BLE_ADV_QUEUE_SIZE;
        mmBleAdvCount--;
        processed++;
        bool isNew = false;
        int idx = mmAddDetection(item.mac, item.name, item.rssi, "ble_adv",
                                 0, nullptr, false, &isNew);
        if (idx >= 0 && isNew) {
            mmEmitDetectionSideEffects(idx, item.mac, item.name, item.rssi,
                                       "ble_adv", 0);
        }
    }
}

class MM_BLECallbacks : public NimBLEAdvertisedDeviceCallbacks {
    void onResult(const NimBLEAdvertisedDevice* dev) override {
        NimBLEAddress addr = dev->getAddress();
        std::string addrStr = addr.toString();
        int rssi = dev->getRSSI();
        const char* name = "";
        if (dev->haveName()) {
            std::string n = dev->getName();
            mmBleAdvEnqueue(addrStr.c_str(), n.c_str(), rssi);
        } else {
            mmBleAdvEnqueue(addrStr.c_str(), "", rssi);
        }
    }
};

#ifndef MM_DIAG_DISABLE_BLE
static void mmBleScanStopWait() {
    if (!mmBLEScan) {
        return;
    }
    if (mmBLEScan->isScanning()) {
        mmBLEScan->stop();
        unsigned long deadline = millis() + 500;
        while (mmBLEScan->isScanning() && (long)(millis() - deadline) < 0) {
            yield();
            delay(10);
        }
    }
}
static void mmPauseBleForDashboard() {
    mmBleScanStopWait();
    if (mmBLEScan) {
        mmBLEScan->clearResults();
    }
    printf("[MEGA-MAID] BLE scan paused (stack kept alive for AP)\n");
}

static void mmStopBleStack() {
#ifndef MM_DIAG_DISABLE_BLE
    if (!mmBLEScan && !NimBLEDevice::isInitialized()) {
        return;
    }
    mmBleScanStopWait();
    if (mmBLEScan) {
        mmBLEScan->clearResults();
        mmBLEScan = nullptr;
    }
    if (mmBleCallbacks) {
        delete mmBleCallbacks;
        mmBleCallbacks = nullptr;
    }
    if (NimBLEDevice::isInitialized()) {
        NimBLEDevice::deinit(true);
        mmYieldMs(300);
    }
    printf("[MEGA-MAID] BLE stack stopped (dashboard AP needs full radio)\n");
#endif
}

// When AP is up, mmEnterBleSlice skips BLE start (single-radio constraint).
// Loading NimBLE there wastes ~54KB and breaks Collect→Dashboard HTTP on C6.
static bool mmShouldInitBleStack() {
#ifndef MM_DIAG_DISABLE_BLE
    if (mmDashboardApUp) {
        return false;
    }
    return true;
#else
    return false;
#endif
}

static bool mmStartBleStack() {
    if (mmBLEScan) {
        return true;
    }
    if (!mmShouldInitBleStack()) {
        return false;
    }
    if (ESP.getFreeHeap() < 16384) {
        printf("[MEGA-MAID] BLE init skipped: low heap (%u)\n",
               (unsigned)ESP.getFreeHeap());
        return false;
    }
    if (!NimBLEDevice::isInitialized()) {
        NimBLEDevice::init("");
    }
    if (!NimBLEDevice::isInitialized()) {
        printf("[MEGA-MAID] BLE init failed\n");
        return false;
    }
    mmBLEScan = NimBLEDevice::getScan();
    if (!mmBLEScan) {
        printf("[MEGA-MAID] BLE scan unavailable\n");
        return false;
    }
    if (!mmBleCallbacks) {
        mmBleCallbacks = new MM_BLECallbacks();
        mmBLEScan->setScanCallbacks(mmBleCallbacks, true);
    }
    mmBLEScan->setActiveScan(true);
    mmBLEScan->setInterval(MM_BLE_SLOT_MS);
    mmBLEScan->setWindow(MM_BLE_SLOT_MS);
    printf("[MEGA-MAID] BLE stack ready (heap=%u)\n", (unsigned)ESP.getFreeHeap());
    return true;
}
#endif

// ============================================================================
// JSON HELPER
// ============================================================================

static void mmWriteDetWifiFields(Print* out, const MMDetection& d) {
    if (d.ssid[0]) {
        out->print(",\"ssid\":");
        mmPrintJsonString(out, d.ssid);
    }
    if (d.probedSsidCount > 0) {
        out->print(",\"probed_ssids\":[");
        for (int j = 0; j < (int)d.probedSsidCount; j++) {
            if (j > 0) out->print(",");
            mmPrintJsonString(out, d.probedSsids[j]);
        }
        out->print("]");
    }
}

static void writeDetectionsJSON(AsyncResponseStream *resp, int offset = 0, int limit = -1) {
    if (offset < 0) offset = 0;
    if (limit <= 0 || limit > MAX_DETECTIONS) limit = MAX_DETECTIONS;
    resp->print("[");
    if (mmMutex && xSemaphoreTake(mmMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        int end = mmDetCount;
        if (offset > end) offset = end;
        if (offset + limit < end) end = offset + limit;
        bool first = true;
        for (int i = offset; i < end; i++) {
            if (!first) resp->print(",");
            first = false;
            resp->print("{\"mac\":");
            mmPrintJsonString(resp, mmDet[i].mac);
            resp->print(",\"name\":");
            mmPrintJsonString(resp, mmDet[i].name);
            resp->printf(",\"rssi\":%d,\"method\":", mmDet[i].rssi);
            mmPrintJsonString(resp, mmDet[i].method);
            resp->printf(",\"first\":%lu,\"last\":%lu,\"count\":%d,\"channel\":%u",
                mmDet[i].firstSeen, mmDet[i].lastSeen, mmDet[i].count,
                (unsigned)mmDet[i].channel);
            mmWriteDetWifiFields(resp, mmDet[i]);
            if (mmDet[i].hasGPS) {
                resp->printf(",\"gps\":{\"lat\":%.8f,\"lon\":%.8f,\"acc\":%.1f}",
                    mmDet[i].gpsLat, mmDet[i].gpsLon, mmDet[i].gpsAcc);
            }
            resp->print("}");
        }
        xSemaphoreGive(mmMutex);
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
//   1. Compute size+CRC over the detections payload (pass 1, under mmMutex)
//   2. Write envelope header + payload to /session.tmp (pass 2, under same lock)
//   3. Remove /session.json
//   4. Rename /session.tmp → /session.json (with copy+delete fallback)
//
// Recovery: if /session.json is missing or CRC-invalid, fall back to /session.tmp.
// If power fails between steps 3 and 4, mmPromotePrevSession() recovers from tmp.

#define MM_SESSION_TMP "/megamaid_session.tmp"
#define MM_SESSION_HDR_BYTES 56

// Serialize a single detection to `dst`. Returns bytes written (0 on overflow).
static size_t mmSerializeDet(const MMDetection& d, char* dst, size_t cap) {
    char wifiBuf[320] = "";
    size_t woff = 0;
    if (!mmJsonAppendField(wifiBuf, sizeof(wifiBuf), &woff, "ssid", d.ssid)) return 0;
    mmAppendProbedSsidsJson(wifiBuf, sizeof(wifiBuf), d);

    char macEsc[24] = "";
    char nameEsc[96] = "";
    char methodEsc[32] = "";
    size_t macOff = 0, nameOff = 0, methodOff = 0;
    if (!mmJsonAppendQuoted(macEsc, sizeof(macEsc), &macOff, d.mac)) return 0;
    if (!mmJsonAppendQuoted(nameEsc, sizeof(nameEsc), &nameOff, d.name)) return 0;
    if (!mmJsonAppendQuoted(methodEsc, sizeof(methodEsc), &methodOff, d.method)) return 0;

    char line[768];
    int n;
    if (d.hasGPS) {
        n = snprintf(line, sizeof(line),
            "{\"mac\":%s,\"name\":%s,\"rssi\":%d,\"method\":%s,"
            "\"first\":%lu,\"last\":%lu,\"count\":%d,"
            "\"channel\":%u"
            "%s,\"gps\":{\"lat\":%.8f,\"lon\":%.8f,\"acc\":%.1f}}",
            macEsc, nameEsc, d.rssi, methodEsc,
            d.firstSeen, d.lastSeen, d.count,
            (unsigned)d.channel,
            wifiBuf,
            d.gpsLat, d.gpsLon, d.gpsAcc);
    } else {
        n = snprintf(line, sizeof(line),
            "{\"mac\":%s,\"name\":%s,\"rssi\":%d,\"method\":%s,"
            "\"first\":%lu,\"last\":%lu,\"count\":%d,"
            "\"channel\":%u%s}",
            macEsc, nameEsc, d.rssi, methodEsc,
            d.firstSeen, d.lastSeen, d.count,
            (unsigned)d.channel,
            wifiBuf);
    }
    if (n <= 0 || (size_t)n >= cap) return 0;
    memcpy(dst, line, (size_t)n + 1);
    return (size_t)n;
}

// Pass 1: compute exact payload size + CRC32 from snapshot (no mutex).
static uint32_t mmComputeSnapCRC(const MMDetection* snap, int count, size_t& outBytes) {
    char line[768];
    uint32_t crc = 0;
    outBytes = 0;
    crc = mmCRC32Update(crc, (const uint8_t*)"[", 1); outBytes += 1;
    for (int i = 0; i < count; i++) {
        if (i > 0) { crc = mmCRC32Update(crc, (const uint8_t*)",", 1); outBytes += 1; }
        size_t n = mmSerializeDet(snap[i], line, sizeof(line));
        if (n == 0) continue;
        crc = mmCRC32Update(crc, (const uint8_t*)line, n);
        outBytes += n;
    }
    crc = mmCRC32Update(crc, (const uint8_t*)"]", 1); outBytes += 1;
    return crc;
}

// Legacy helper for callers that still hold mmMutex on live mmDet[].
static uint32_t mmComputePayloadCRC(size_t& outBytes) {
    return mmComputeSnapCRC(mmDet, mmDetCount, outBytes);
}

// Validate a session file envelope and its payload CRC. Returns true if intact.
// Optionally outputs payload byte-offset and length.
static bool mmValidateSessionFile(const char* path,
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
        crc = mmCRC32Update(crc, buf, (size_t)n);
        remaining -= (size_t)n;
    }
    f.close();

    if (remaining != 0 || crc != expectedCRC) return false;
    if (outBodyOffset) *outBodyOffset = bodyOffset;
    if (outBodyBytes)  *outBodyBytes  = expectedBytes;
    return true;
}

// Copy src→dst in chunks. Returns true on success (both files existed & read/write ok).
static bool mmSpiffsCopy(const char* src, const char* dst) {
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
static bool mmAtomicPromote(const char* tmp, const char* final) {
    if (SPIFFS.rename(tmp, final)) return true;
    if (!mmSpiffsCopy(tmp, final)) return false;
    SPIFFS.remove(tmp);
    return true;
}

static void mmRequestSave() {
    if (!mmSpiffsReady) return;
    if (mmRadioProfile == MM_RADIO_DASHBOARD && !mmSaveSyncWait) return;
    mmSavePending = true;
}

static void mmWriteSessionHeader(File& f, int count, size_t bytes, uint32_t crc) {
    char hdr[MM_SESSION_HDR_BYTES + 1];
    memset(hdr, ' ', MM_SESSION_HDR_BYTES);
    hdr[MM_SESSION_HDR_BYTES] = '\0';
    char body[MM_SESSION_HDR_BYTES];
    int n = snprintf(body, sizeof(body),
                     "{\"v\":1,\"count\":%d,\"bytes\":%u,\"crc\":\"0x%08lX\"}",
                     count, (unsigned)bytes, (unsigned long)crc);
    if (n > 0 && n <= MM_SESSION_HDR_BYTES) {
        memcpy(hdr, body, (size_t)n);
    }
    f.write((const uint8_t*)hdr, MM_SESSION_HDR_BYTES);
    f.write((const uint8_t*)"\n", 1);
}

static bool mmSaveCopyDet(int idx, MMDetection& out) {
    if (!mmMutex || xSemaphoreTake(mmMutex, pdMS_TO_TICKS(30)) != pdTRUE) return false;
    if (idx < 0 || idx >= mmDetCount) {
        xSemaphoreGive(mmMutex);
        return false;
    }
    out = mmDet[idx];
    xSemaphoreGive(mmMutex);
    return true;
}

static bool mmSaveBusy() {
    return mmSavePhase != MM_SAVE_IDLE || mmSavePending;
}

#ifndef MM_DIAG_DISABLE_BLE
static void mmSavePauseRadio();
static void mmSaveResumeRadio();
static void mmEnterSlice(MmRadioSlice slice);
#endif

static void mmSaveEnterIdle() {
    mmSavePhase = MM_SAVE_IDLE;
#ifndef MM_DIAG_DISABLE_BLE
    mmSaveResumeRadio();
#endif
}

#ifndef MM_DIAG_DISABLE_BLE
static void mmSavePauseRadio() {
    if (mmSaveRadioPaused) {
        return;
    }
    mmSaveRadioPaused = true;
    mmWiFiSniffStop();
    mmBleScanStopWait();
}

static void mmSaveResumeRadio() {
    if (!mmSaveRadioPaused) {
        return;
    }
    mmSaveRadioPaused = false;
    // Dashboard transition may still be in COLLECT while sync save finishes.
    if (mmSaveSyncWait || mmRadioProfile != MM_RADIO_COLLECT || mmDashboardEntryInProgress
        || mmCollectScanPaused) {
        return;
    }
    mmEnterSlice(mmCurrentSlice);
}
#endif

static unsigned long mmKey1DclickWindowMs() {
    return (mmDetCount >= MM_NEAR_CAP_THRESHOLD) ? MM_KEY1_DCLICK_CAP_MS : MM_KEY1_DCLICK_MS;
}

static bool mmHttpHeapReady() {
    size_t freeHeap = ESP.getFreeHeap();
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    return freeHeap >= MM_HTTP_MIN_HEAP && largest >= MM_HTTP_MIN_HEAP;
}

static void mmPollButtonsDuringSave() {
    nessoPollInput();
    mmPollKey1DoubleClick();
}

static void mmSaveTick() {
    if (mmSavePhase == MM_SAVE_IDLE) {
        if (mmRadioProfile == MM_RADIO_DASHBOARD && !mmSaveSyncWait) return;
        if (!mmSavePending || !mmSpiffsReady) return;
        mmSavePending = false;
        if (!mmMutex || xSemaphoreTake(mmMutex, pdMS_TO_TICKS(30)) != pdTRUE) {
            mmSavePending = true;
            return;
        }
        mmSaveSnapCount = mmDetCount;
        xSemaphoreGive(mmMutex);
        mmSaveProgressIdx = 0;
        mmSaveCrc = 0;
        mmSavePayloadBytes = 0;
        mmSaveFile = SPIFFS.open(MM_SESSION_TMP, "w");
        if (!mmSaveFile) {
            printf("[MEGA-MAID] Save failed: cannot open %s\n", MM_SESSION_TMP);
            mmSaveEnterIdle();
            return;
        }
        mmWriteSessionHeader(mmSaveFile, 0, 0, 0);
        mmSaveFile.write((const uint8_t*)"[", 1);
        mmSaveCrc = mmCRC32Update(0, (const uint8_t*)"[", 1);
        mmSavePayloadBytes = 1;
        mmSavePhase = MM_SAVE_WRITE;
#ifndef MM_DIAG_DISABLE_BLE
        mmSavePauseRadio();
#endif
        return;
    }

    char line[768];

    if (mmSavePhase == MM_SAVE_WRITE) {
        int batchEnd = mmSaveProgressIdx + MM_SAVE_WRITE_BATCH;
        if (batchEnd > mmSaveSnapCount) batchEnd = mmSaveSnapCount;
        for (int i = mmSaveProgressIdx; i < batchEnd; i++) {
            if (i > 0) {
                mmSaveFile.write((const uint8_t*)",", 1);
                mmSaveCrc = mmCRC32Update(mmSaveCrc, (const uint8_t*)",", 1);
                mmSavePayloadBytes += 1;
            }
            MMDetection d;
            if (!mmSaveCopyDet(i, d)) {
                mmSaveFile.close();
                mmSavePending = true;
                mmSaveEnterIdle();
                return;
            }
            size_t n = mmSerializeDet(d, line, sizeof(line));
            if (n == 0) continue;
            mmSaveFile.write((const uint8_t*)line, n);
            mmSaveCrc = mmCRC32Update(mmSaveCrc, (const uint8_t*)line, n);
            mmSavePayloadBytes += n;
        }
        mmSaveProgressIdx = batchEnd;
        if (mmSaveProgressIdx >= mmSaveSnapCount) {
            mmSaveFile.write((const uint8_t*)"]", 1);
            mmSaveCrc = mmCRC32Update(mmSaveCrc, (const uint8_t*)"]", 1);
            mmSavePayloadBytes += 1;
            mmSaveFile.close();
            mmSaveFile = SPIFFS.open(MM_SESSION_TMP, "r+");
            if (mmSaveFile) {
                mmSaveFile.seek(0);
                mmWriteSessionHeader(mmSaveFile, mmSaveSnapCount,
                                     mmSavePayloadBytes, mmSaveCrc);
                mmSaveFile.close();
            }
            mmSavePhase = MM_SAVE_VERIFY;
        }
        mmPollButtonsDuringSave();
        return;
    }

    if (mmSavePhase == MM_SAVE_VERIFY) {
        if (!mmValidateSessionFile(MM_SESSION_TMP)) {
            printf("[MEGA-MAID] Save verify FAILED — aborting promote\n");
            mmSaveEnterIdle();
            return;
        }
        mmSavePhase = MM_SAVE_PROMOTE;
        return;
    }

    if (mmSavePhase == MM_SAVE_PROMOTE) {
        SPIFFS.remove(MM_SESSION_FILE);
        if (!mmAtomicPromote(MM_SESSION_TMP, MM_SESSION_FILE)) {
            printf("[MEGA-MAID] Promote FAILED — data in %s for recovery\n", MM_SESSION_TMP);
            mmSaveEnterIdle();
            return;
        }
        mmLastSaveCount = mmSaveSnapCount;
        printf("[MEGA-MAID] Session saved: %d det, %u bytes, crc=0x%08lX\n",
               mmSaveSnapCount, (unsigned)mmSavePayloadBytes, (unsigned long)mmSaveCrc);
        mmSaveEnterIdle();
    }
}

static void mmSaveSyncWaitDone() {
    mmSaveSyncWait = true;
    mmRequestSave();
    unsigned long start = millis();
    while (mmSaveBusy()) {
        mmSaveTick();
        mmPollButtonsDuringSave();
        nessoResetBootButtonHold();
        yield();
        delay(1);
        if (millis() - start > MM_DASH_SAVE_TIMEOUT_MS) {
            printf("[MEGA-MAID] Save sync timeout\n");
            break;
        }
    }
    mmSaveSyncWait = false;
}

static void mmSaveSession() {
    mmSaveSyncWaitDone();
}

static void mmPromotePrevSession() {
    if (!mmSpiffsReady) return;

    const char* source = nullptr;
    if (mmValidateSessionFile(MM_SESSION_FILE)) {
        source = MM_SESSION_FILE;
    } else if (mmValidateSessionFile(MM_SESSION_TMP)) {
        printf("[MEGA-MAID] Main session corrupt/missing — recovering from tmp\n");
        source = MM_SESSION_TMP;
    } else {
        // Legacy fallback: old format (no envelope, raw array)
        if (SPIFFS.exists(MM_SESSION_FILE)) {
            File f = SPIFFS.open(MM_SESSION_FILE, "r");
            if (f && f.size() > 2) {
                int first = f.peek();
                f.close();
                if (first == '[') {
                    source = MM_SESSION_FILE;
                    printf("[MEGA-MAID] Legacy-format session detected — promoting\n");
                }
            } else if (f) { f.close(); }
        }
    }

    if (!source) {
        if (SPIFFS.exists(MM_SESSION_FILE)) SPIFFS.remove(MM_SESSION_FILE);
        if (SPIFFS.exists(MM_SESSION_TMP))  SPIFFS.remove(MM_SESSION_TMP);
        printf("[MEGA-MAID] No valid prior session to promote\n");
        return;
    }

    // Copy source → prev_session.json (same format as stored — envelope or legacy)
    if (!mmSpiffsCopy(source, MM_PREV_FILE)) {
        printf("[MEGA-MAID] Failed to promote %s → %s\n", source, MM_PREV_FILE);
        return;
    }

    // Clean up: clear session files so they don't get re-promoted next boot
    if (SPIFFS.exists(MM_SESSION_FILE)) SPIFFS.remove(MM_SESSION_FILE);
    if (SPIFFS.exists(MM_SESSION_TMP))  SPIFFS.remove(MM_SESSION_TMP);

    File v = SPIFFS.open(MM_PREV_FILE, "r");
    size_t sz = v ? v.size() : 0;
    if (v) v.close();
    printf("[MEGA-MAID] Prior session promoted from %s (%u bytes)\n", source, (unsigned)sz);
}

// Read prev_session as a raw detection JSON array (strips envelope header if present).
// Streams into the response to avoid buffering the whole file in RAM.
static int mmPrevSessionCount() {
    if (!mmSpiffsReady || !SPIFFS.exists(MM_PREV_FILE)) {
        return 0;
    }
    File f = SPIFFS.open(MM_PREV_FILE, "r");
    if (!f) {
        return 0;
    }
    if (f.peek() != '{') {
        f.close();
        return 0;
    }
    String hdr = f.readStringUntil('\n');
    f.close();
    JsonDocument doc;
    if (deserializeJson(doc, hdr) != DeserializationError::Ok) {
        return 0;
    }
    return doc["count"] | 0;
}

// ============================================================================
// EXPORT — stream from SPIFFS / RAM without buffering entire payload in heap
// ============================================================================

static bool mmJsonArraySkipWsAndComma(File& f) {
    while (f.available()) {
        int ch = f.peek();
        if (ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t') {
            f.read();
            continue;
        }
        if (ch == ',') {
            f.read();
            continue;
        }
        break;
    }
    return f.available() > 0;
}

// Read the next {...} object from a JSON array file into `out`.
static bool mmJsonArrayReadNextObject(File& f, char* out, size_t outCap) {
    if (!mmJsonArraySkipWsAndComma(f)) {
        return false;
    }
    int ch = f.peek();
    if (ch == ']') {
        f.read();
        return false;
    }
    if (ch != '{') {
        return false;
    }

    size_t len = 0;
    int depth = 0;
    while (f.available()) {
        ch = f.read();
        if (ch < 0) {
            return false;
        }
        if (len + 1 >= outCap) {
            return false;
        }
        out[len++] = (char)ch;
        if (ch == '{') {
            depth++;
        } else if (ch == '}') {
            depth--;
            if (depth == 0) {
                out[len] = '\0';
                return true;
            }
        }
    }
    return false;
}

static bool mmCopySessionBodyToExportTmp(const char* srcPath) {
    if (!mmSpiffsReady || !SPIFFS.exists(srcPath)) {
        return false;
    }

    File src = SPIFFS.open(srcPath, "r");
    if (!src) {
        return false;
    }

    size_t bodyOffset = 0;
    size_t bodyBytes = src.size();
    int first = src.peek();

    if (first == '{') {
        if (!mmValidateSessionFile(srcPath, &bodyOffset, &bodyBytes)) {
            String hdr = src.readStringUntil('\n');
            bodyOffset = hdr.length() + 1;
            bodyBytes = src.size() > bodyOffset ? src.size() - bodyOffset : 0;
        }
        if (bodyBytes == 0 || !src.seek(bodyOffset)) {
            src.close();
            return false;
        }
    }

    File dst = SPIFFS.open(MM_EXPORT_BODY_TMP, "w");
    if (!dst) {
        src.close();
        return false;
    }

    uint8_t buf[512];
    size_t remaining = bodyBytes;
    bool ok = true;
    while (remaining > 0) {
        size_t chunk = remaining < sizeof(buf) ? remaining : sizeof(buf);
        int n = src.read(buf, chunk);
        if (n <= 0) {
            ok = false;
            break;
        }
        if (dst.write(buf, (size_t)n) != (size_t)n) {
            ok = false;
            break;
        }
        remaining -= (size_t)n;
    }
    src.close();
    dst.close();

    if (!ok || remaining != 0) {
        SPIFFS.remove(MM_EXPORT_BODY_TMP);
        return false;
    }
    return true;
}

static bool mmSendJsonFileDownload(AsyncWebServerRequest* r, const char* downloadName) {
    if (!SPIFFS.exists(MM_EXPORT_BODY_TMP)) {
        return false;
    }
    AsyncWebServerResponse* resp = r->beginResponse(SPIFFS, MM_EXPORT_BODY_TMP, "application/json", true);
    if (!resp) {
        printf("[MEGA-MAID] export file response failed (heap=%u)\n",
               (unsigned)ESP.getFreeHeap());
        return false;
    }
    resp->removeHeader("Content-Disposition");
    resp->addHeader("Content-Disposition",
                    String("attachment; filename=\"") + downloadName + "\"", true);
    File vf = SPIFFS.open(MM_EXPORT_BODY_TMP, "r");
    size_t sz = vf ? vf.size() : 0;
    if (vf) {
        vf.close();
    }
    printf("[MEGA-MAID] export download %s (%u bytes, heap=%u)\n",
           downloadName, (unsigned)sz, (unsigned)ESP.getFreeHeap());
    r->send(resp);
    return true;
}

static bool mmExportSessionJsonDownload(AsyncWebServerRequest* r, const char* srcPath,
                                       const char* downloadName) {
    if (!mmSpiffsReady || !SPIFFS.exists(srcPath)) {
        return false;
    }
    if (!mmCopySessionBodyToExportTmp(srcPath)) {
        printf("[MEGA-MAID] export body copy failed for %s\n", srcPath);
        return false;
    }
    return mmSendJsonFileDownload(r, downloadName);
}

static size_t mmSessionBodyStreamFiller(uint8_t* buf, size_t maxLen, size_t index) {
    (void)index;
    if (!mmBodyStream.active || !mmBodyStream.f) {
        return 0;
    }
    size_t n = mmBodyStream.f.read(buf, maxLen);
    if (n == 0) {
        mmBodyStream.f.close();
        mmBodyStream.active = false;
    }
    return n;
}

static bool mmStartSessionBodyChunked(AsyncWebServerRequest* r, const char* path,
                                      const char* contentType,
                                      const char* disposition = nullptr) {
    if (!mmSpiffsReady || !SPIFFS.exists(path)) {
        return false;
    }
    mmBodyStream.f = SPIFFS.open(path, "r");
    if (!mmBodyStream.f) {
        return false;
    }
    int first = mmBodyStream.f.peek();
    if (first == '{') {
        mmBodyStream.f.readStringUntil('\n');
    } else if (first != '[') {
        mmBodyStream.f.close();
        return false;
    }
    mmBodyStream.active = true;
    AsyncWebServerResponse* resp =
        r->beginChunkedResponse(contentType, mmSessionBodyStreamFiller);
    if (disposition) {
        resp->addHeader("Content-Disposition", disposition, false);
    }
    r->send(resp);
    return true;
}

static size_t mmRamJsonExportFiller(uint8_t* buf, size_t maxLen, size_t index) {
    (void)index;
    if (!mmRamJsonExport.active) {
        return 0;
    }

    size_t off = 0;
    char line[768];

    while (off < maxLen && mmRamJsonExport.detIdx < mmRamJsonExport.total) {
        if (mmRamJsonExport.detIdx == 0) {
            if (off + 1 > maxLen) {
                break;
            }
            buf[off++] = '[';
        } else {
            if (off + 1 > maxLen) {
                break;
            }
            buf[off++] = ',';
        }

        MMDetection d;
        if (!mmSaveCopyDet(mmRamJsonExport.detIdx, d)) {
            break;
        }
        size_t n = mmSerializeDet(d, line, sizeof(line));
        if (n == 0) {
            mmRamJsonExport.detIdx++;
            continue;
        }
        if (off + n > maxLen) {
            break;
        }
        memcpy(buf + off, line, n);
        off += n;
        mmRamJsonExport.detIdx++;
    }

    if (mmRamJsonExport.detIdx >= mmRamJsonExport.total) {
        if (off < maxLen) {
            buf[off++] = ']';
        }
        mmRamJsonExport.active = false;
        printf("[MEGA-MAID] RAM JSON export complete (%d det)\n", mmRamJsonExport.total);
    }
    return off;
}

static void mmStartRamJsonExport(AsyncWebServerRequest* r) {
    if (!mmMutex || xSemaphoreTake(mmMutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        r->send(500, "application/json", "{\"error\":\"export busy\"}");
        return;
    }
    mmRamJsonExport.total = mmDetCount;
    mmRamJsonExport.detIdx = 0;
    mmRamJsonExport.active = mmRamJsonExport.total > 0;
    xSemaphoreGive(mmMutex);

    if (!mmRamJsonExport.active) {
        AsyncWebServerResponse* resp = r->beginResponse(200, "application/json", "[]");
        resp->addHeader("Content-Disposition",
                        "attachment; filename=\"megamaid_detections.json\"", false);
        r->send(resp);
        return;
    }

    printf("[MEGA-MAID] RAM JSON export fallback (%d det, heap=%u)\n",
           mmRamJsonExport.total, (unsigned)ESP.getFreeHeap());
    AsyncWebServerResponse* resp =
        r->beginChunkedResponse("application/json", mmRamJsonExportFiller);
    resp->addHeader("Content-Disposition",
                    "attachment; filename=\"megamaid_detections.json\"", false);
    r->send(resp);
}

static size_t mmRamCsvExportFiller(uint8_t* buf, size_t maxLen, size_t index) {
    (void)index;
    if (!mmRamCsvExport.active) {
        return 0;
    }

    size_t off = 0;
    if (!mmRamCsvExport.headerSent) {
        const char* hdr =
            "mac,name,ssid,probed_ssids,rssi,method,channel,first_seen_ms,last_seen_ms,"
            "count,latitude,longitude,gps_accuracy\n";
        size_t hdrLen = strlen(hdr);
        if (hdrLen > maxLen) {
            return 0;
        }
        memcpy(buf, hdr, hdrLen);
        mmRamCsvExport.headerSent = true;
        off = hdrLen;
    }

    char line[512];
    while (off < maxLen && mmRamCsvExport.detIdx < mmRamCsvExport.total) {
        if (!mmMutex || xSemaphoreTake(mmMutex, pdMS_TO_TICKS(20)) != pdTRUE) {
            break;
        }
        MMDetection& d = mmDet[mmRamCsvExport.detIdx];
        char probes[128] = "";
        for (int j = 0; j < (int)d.probedSsidCount; j++) {
            if (j > 0) {
                strncat(probes, ";", sizeof(probes) - strlen(probes) - 1);
            }
            strncat(probes, d.probedSsids[j], sizeof(probes) - strlen(probes) - 1);
        }
        int n;
        if (d.hasGPS) {
            n = snprintf(line, sizeof(line),
                "\"%s\",\"%s\",\"%s\",\"%s\",%d,\"%s\",%u,%lu,%lu,%d,%.8f,%.8f,%.1f\n",
                d.mac, d.name, d.ssid, probes, d.rssi, d.method, (unsigned)d.channel,
                d.firstSeen, d.lastSeen, d.count,
                d.gpsLat, d.gpsLon, d.gpsAcc);
        } else {
            n = snprintf(line, sizeof(line),
                "\"%s\",\"%s\",\"%s\",\"%s\",%d,\"%s\",%u,%lu,%lu,%d,,,\n",
                d.mac, d.name, d.ssid, probes, d.rssi, d.method, (unsigned)d.channel,
                d.firstSeen, d.lastSeen, d.count);
        }
        mmRamCsvExport.detIdx++;
        xSemaphoreGive(mmMutex);
        if (n <= 0) {
            continue;
        }
        if (off + (size_t)n > maxLen) {
            break;
        }
        memcpy(buf + off, line, (size_t)n);
        off += (size_t)n;
    }

    if (mmRamCsvExport.detIdx >= mmRamCsvExport.total) {
        mmRamCsvExport.active = false;
    }
    return off;
}

static void mmStartRamCsvExport(AsyncWebServerRequest* r, const char* filename) {
    int total = 0;
    if (mmMutex && xSemaphoreTake(mmMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        total = mmDetCount;
        xSemaphoreGive(mmMutex);
    }
    mmRamCsvExport.detIdx = 0;
    mmRamCsvExport.total = total;
    mmRamCsvExport.headerSent = false;
    mmRamCsvExport.active = true;
    AsyncWebServerResponse* resp =
        r->beginChunkedResponse("text/csv", mmRamCsvExportFiller);
    resp->addHeader("Content-Disposition",
                    String("attachment; filename=\"") + filename + "\"", false);
    r->send(resp);
}

static size_t mmRamKmlExportFiller(uint8_t* buf, size_t maxLen, size_t index) {
    (void)index;
    if (!mmRamKmlExport.active) {
        return 0;
    }

    size_t off = 0;
    if (mmRamKmlExport.phase == 0) {
        const char* hdr =
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<kml xmlns=\"http://www.opengis.net/kml/2.2\">\n<Document>\n"
            "<name>Mega Maid Detections</name>\n"
            "<description>BLE + WiFi MAC detections with GPS</description>\n"
            "<Style id=\"det\"><IconStyle><color>ff4489ec</color>"
            "<scale>1.0</scale></IconStyle></Style>\n";
        size_t hdrLen = strlen(hdr);
        if (hdrLen > maxLen) {
            return 0;
        }
        memcpy(buf, hdr, hdrLen);
        off = hdrLen;
        mmRamKmlExport.phase = 1;
    }

    char chunk[640];
    while (off < maxLen && mmRamKmlExport.phase == 1) {
        if (!mmMutex || xSemaphoreTake(mmMutex, pdMS_TO_TICKS(20)) != pdTRUE) {
            break;
        }
        if (mmRamKmlExport.detIdx >= mmRamKmlExport.total) {
            mmRamKmlExport.phase = 2;
            xSemaphoreGive(mmMutex);
            break;
        }
        MMDetection& d = mmDet[mmRamKmlExport.detIdx++];
        if (!d.hasGPS) {
            xSemaphoreGive(mmMutex);
            continue;
        }
        int n = snprintf(chunk, sizeof(chunk),
            "<Placemark>\n<name>%s</name>\n<styleUrl>#det</styleUrl>\n"
            "<description><![CDATA[",
            d.mac);
        if (n > 0 && d.name[0]) {
            char escName[96];
            if (mmHtmlEscapeToBuf(d.name, escName, sizeof(escName)) > 0) {
                n += snprintf(chunk + n, sizeof(chunk) - (size_t)n,
                              "<b>Name:</b> %s<br/>", escName);
            }
        }
        if (d.ssid[0]) {
            char escSsid[96];
            if (mmHtmlEscapeToBuf(d.ssid, escSsid, sizeof(escSsid)) > 0) {
                n += snprintf(chunk + n, sizeof(chunk) - (size_t)n,
                              "<b>WiFi SSID:</b> %s<br/>", escSsid);
            }
        }
        n += snprintf(chunk + n, sizeof(chunk) - (size_t)n,
            "<b>Method:</b> %s<br/><b>RSSI:</b> %d dBm<br/><b>Count:</b> %d<br/>",
            d.method, d.rssi, d.count);
        n += snprintf(chunk + n, sizeof(chunk) - (size_t)n,
            "<b>Accuracy:</b> %.1f m", d.gpsAcc);
        n += snprintf(chunk + n, sizeof(chunk) - (size_t)n,
            "]]></description>\n<Point><coordinates>%.8f,%.8f,0</coordinates></Point>\n"
            "</Placemark>\n",
            d.gpsLon, d.gpsLat);
        xSemaphoreGive(mmMutex);
        if (n <= 0) {
            continue;
        }
        if (off + (size_t)n > maxLen) {
            break;
        }
        memcpy(buf + off, chunk, (size_t)n);
        off += (size_t)n;
    }

    if (mmRamKmlExport.phase == 2) {
        const char* foot = "</Document>\n</kml>";
        size_t footLen = strlen(foot);
        if (off + footLen <= maxLen) {
            memcpy(buf + off, foot, footLen);
            off += footLen;
            mmRamKmlExport.active = false;
        }
    }
    return off;
}

static void mmStartRamKmlExport(AsyncWebServerRequest* r, const char* filename) {
    int total = 0;
    if (mmMutex && xSemaphoreTake(mmMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        total = mmDetCount;
        xSemaphoreGive(mmMutex);
    }
    mmRamKmlExport.detIdx = 0;
    mmRamKmlExport.total = total;
    mmRamKmlExport.phase = 0;
    mmRamKmlExport.active = true;
    AsyncWebServerResponse* resp =
        r->beginChunkedResponse("application/vnd.google-earth.kml+xml", mmRamKmlExportFiller);
    resp->addHeader("Content-Disposition",
                    String("attachment; filename=\"") + filename + "\"", false);
    r->send(resp);
}

static int mmFormatJsonDetCsvRow(JsonObject d, char* line, size_t cap) {
    char probes[128] = "";
    if (d["probed_ssids"].is<JsonArray>()) {
        for (JsonVariant v : d["probed_ssids"].as<JsonArray>()) {
            const char* s = v.as<const char*>();
            if (!s) {
                continue;
            }
            if (probes[0]) {
                strncat(probes, ";", sizeof(probes) - strlen(probes) - 1);
            }
            strncat(probes, s, sizeof(probes) - strlen(probes) - 1);
        }
    }
    const char* mac = d["mac"] | "";
    const char* name = d["name"] | "";
    const char* ssid = d["ssid"] | "";
    int rssi = d["rssi"] | 0;
    const char* method = d["method"] | "";
    unsigned ch = (unsigned)(d["channel"] | 0);
    unsigned long first = d["first"] | 0UL;
    unsigned long last = d["last"] | 0UL;
    int count = d["count"] | 1;
    JsonObject gps = d["gps"];
    if (!gps.isNull() && gps.containsKey("lat")) {
        return snprintf(line, cap,
            "\"%s\",\"%s\",\"%s\",\"%s\",%d,\"%s\",%u,%lu,%lu,%d,%.8f,%.8f,%.1f\n",
            mac, name, ssid, probes, rssi, method, ch, first, last, count,
            (double)(gps["lat"] | 0.0), (double)(gps["lon"] | 0.0),
            (double)(gps["acc"] | 0.0));
    }
    return snprintf(line, cap,
        "\"%s\",\"%s\",\"%s\",\"%s\",%d,\"%s\",%u,%lu,%lu,%d,,,\n",
        mac, name, ssid, probes, rssi, method, ch, first, last, count);
}

static size_t mmHistCsvExportFiller(uint8_t* buf, size_t maxLen, size_t index) {
    (void)index;
    if (!mmHistCsvExport.active || mmHistCsvExport.done) {
        return 0;
    }

    size_t off = 0;
    if (!mmHistCsvExport.headerSent) {
        const char* hdr =
            "mac,name,ssid,probed_ssids,rssi,method,channel,first_seen_ms,last_seen_ms,"
            "count,latitude,longitude,gps_accuracy\n";
        size_t hdrLen = strlen(hdr);
        if (hdrLen > maxLen) {
            return 0;
        }
        memcpy(buf, hdr, hdrLen);
        mmHistCsvExport.headerSent = true;
        off = hdrLen;
    }

    char line[512];
    JsonDocument doc;
    while (off < maxLen) {
        if (!mmJsonArrayReadNextObject(mmHistCsvExport.f, mmHistCsvExport.objBuf,
                                       sizeof(mmHistCsvExport.objBuf))) {
            mmHistCsvExport.f.close();
            mmHistCsvExport.active = false;
            mmHistCsvExport.done = true;
            break;
        }
        doc.clear();
        if (deserializeJson(doc, mmHistCsvExport.objBuf) != DeserializationError::Ok) {
            continue;
        }
        int n = mmFormatJsonDetCsvRow(doc.as<JsonObject>(), line, sizeof(line));
        if (n <= 0) {
            continue;
        }
        if (off + (size_t)n > maxLen) {
            break;
        }
        memcpy(buf + off, line, (size_t)n);
        off += (size_t)n;
    }
    return off;
}

static bool mmStartHistCsvExport(AsyncWebServerRequest* r, const char* path,
                                 const char* filename) {
    mmHistCsvExport.f = SPIFFS.open(path, "r");
    if (!mmHistCsvExport.f) {
        return false;
    }
    if (mmHistCsvExport.f.peek() == '{') {
        mmHistCsvExport.f.readStringUntil('\n');
    }
    mmHistCsvExport.active = true;
    mmHistCsvExport.headerSent = false;
    mmHistCsvExport.done = false;
    AsyncWebServerResponse* resp =
        r->beginChunkedResponse("text/csv", mmHistCsvExportFiller);
    resp->addHeader("Content-Disposition",
                    String("attachment; filename=\"") + filename + "\"", false);
    r->send(resp);
    return true;
}

static int mmFormatJsonDetKmlPlacemark(JsonObject d, char* chunk, size_t cap) {
    JsonObject gps = d["gps"];
    if (!gps || !gps.containsKey("lat")) {
        return 0;
    }
    int n = snprintf(chunk, cap,
        "<Placemark><name>%s</name>\n<styleUrl>#det</styleUrl>\n"
        "<description><![CDATA[",
        d["mac"] | "?");
    if (n < 0) {
        return 0;
    }
    if (d["name"].is<const char*>() && strlen(d["name"] | "") > 0) {
        n += snprintf(chunk + n, cap - (size_t)n, "<b>Name:</b> %s<br/>", d["name"] | "");
    }
    n += snprintf(chunk + n, cap - (size_t)n,
        "<b>Method:</b> %s<br/><b>RSSI:</b> %d<br/><b>Count:</b> %d",
        d["method"] | "?", d["rssi"] | 0, d["count"] | 1);
    n += snprintf(chunk + n, cap - (size_t)n,
        "]]></description>\n<Point><coordinates>%.8f,%.8f,0</coordinates></Point>\n</Placemark>\n",
        (double)(gps["lon"] | 0.0), (double)(gps["lat"] | 0.0));
    return n;
}

static size_t mmHistKmlExportFiller(uint8_t* buf, size_t maxLen, size_t index) {
    (void)index;
    if (!mmHistKmlExport.active || mmHistKmlExport.done) {
        return 0;
    }

    size_t off = 0;
    if (mmHistKmlExport.phase == 0) {
        const char* hdr =
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<kml xmlns=\"http://www.opengis.net/kml/2.2\">\n<Document>\n"
            "<name>Mega Maid Prior Session</name>\n"
            "<description>Surveillance device detections from prior session</description>\n"
            "<Style id=\"det\"><IconStyle><color>ff4489ec</color>"
            "<scale>1.0</scale></IconStyle></Style>\n";
        size_t hdrLen = strlen(hdr);
        if (hdrLen > maxLen) {
            return 0;
        }
        memcpy(buf, hdr, hdrLen);
        off = hdrLen;
        mmHistKmlExport.phase = 1;
    }

    char chunk[640];
    JsonDocument doc;
    while (off < maxLen && mmHistKmlExport.phase == 1) {
        if (!mmJsonArrayReadNextObject(mmHistKmlExport.f, mmHistKmlExport.objBuf,
                                       sizeof(mmHistKmlExport.objBuf))) {
            mmHistKmlExport.f.close();
            mmHistKmlExport.phase = 2;
            break;
        }
        doc.clear();
        if (deserializeJson(doc, mmHistKmlExport.objBuf) != DeserializationError::Ok) {
            continue;
        }
        int n = mmFormatJsonDetKmlPlacemark(doc.as<JsonObject>(), chunk, sizeof(chunk));
        if (n <= 0) {
            continue;
        }
        if (off + (size_t)n > maxLen) {
            break;
        }
        memcpy(buf + off, chunk, (size_t)n);
        off += (size_t)n;
    }

    if (mmHistKmlExport.phase == 2) {
        const char* foot = "</Document>\n</kml>";
        size_t footLen = strlen(foot);
        if (off + footLen <= maxLen) {
            memcpy(buf + off, foot, footLen);
            off += footLen;
            mmHistKmlExport.active = false;
            mmHistKmlExport.done = true;
        }
    }
    return off;
}

static bool mmStartHistKmlExport(AsyncWebServerRequest* r, const char* path,
                                 const char* filename) {
    mmHistKmlExport.f = SPIFFS.open(path, "r");
    if (!mmHistKmlExport.f) {
        return false;
    }
    if (mmHistKmlExport.f.peek() == '{') {
        mmHistKmlExport.f.readStringUntil('\n');
    }
    mmHistKmlExport.active = true;
    mmHistKmlExport.phase = 0;
    mmHistKmlExport.done = false;
    AsyncWebServerResponse* resp =
        r->beginChunkedResponse("application/vnd.google-earth.kml+xml", mmHistKmlExportFiller);
    resp->addHeader("Content-Disposition",
                    String("attachment; filename=\"") + filename + "\"", false);
    r->send(resp);
    return true;
}

// ============================================================================
// KML EXPORT
// ============================================================================

static void writeDetectionsKML(AsyncResponseStream *resp) {
    resp->print("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                "<kml xmlns=\"http://www.opengis.net/kml/2.2\">\n<Document>\n"
                "<name>Mega Maid Detections</name>\n"
                "<description>BLE + WiFi MAC detections with GPS</description>\n");

    // Detection pin style
    resp->print("<Style id=\"det\"><IconStyle><color>ff4489ec</color>"
                "<scale>1.0</scale></IconStyle></Style>\n");

    if (mmMutex && xSemaphoreTake(mmMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        for (int i = 0; i < mmDetCount; i++) {
            MMDetection& d = mmDet[i];
            if (!d.hasGPS) continue;  // Skip detections without GPS
            resp->print("<Placemark>\n");
            resp->printf("<name>%s</name>\n", d.mac);
            resp->printf("<styleUrl>#det</styleUrl>\n");
            resp->print("<description><![CDATA[");
            if (d.name[0]) {
                resp->print("<b>Name:</b> ");
                mmPrintHtmlEscaped(resp, d.name);
                resp->print("<br/>");
            }
            if (d.ssid[0]) {
                resp->print("<b>WiFi SSID:</b> ");
                mmPrintHtmlEscaped(resp, d.ssid);
                resp->print("<br/>");
            }
            if (d.probedSsidCount > 0) {
                resp->print("<b>Probed SSIDs:</b> ");
                for (int j = 0; j < (int)d.probedSsidCount; j++) {
                    if (j > 0) resp->print(", ");
                    mmPrintHtmlEscaped(resp, d.probedSsids[j]);
                }
                resp->print("<br/>");
            }
            resp->printf("<b>Method:</b> %s<br/>"
                         "<b>RSSI:</b> %d dBm<br/>"
                         "<b>Count:</b> %d<br/>",
                         d.method, d.rssi, d.count);
            resp->printf("<b>Accuracy:</b> %.1f m", d.gpsAcc);
            resp->print("]]></description>\n");
            resp->printf("<Point><coordinates>%.8f,%.8f,0</coordinates></Point>\n",
                         d.gpsLon, d.gpsLat);
            resp->print("</Placemark>\n");
        }
        xSemaphoreGive(mmMutex);
    }
    resp->print("</Document>\n</kml>");
}

// ============================================================================
// OUI AGGREGATION
// ============================================================================

static int mmCountUniqueOuis() {
    if (!mmMutex || xSemaphoreTake(mmMutex, pdMS_TO_TICKS(100)) != pdTRUE) return 0;
    int ouiCount = 0;
    for (int i = 0; i < mmDetCount; i++) {
        char prefix[9];
        snprintf(prefix, sizeof(prefix), "%.8s", mmDet[i].mac);
        bool found = false;
        for (int j = 0; j < ouiCount; j++) {
            if (strcasecmp(mmOuiBuf[j], prefix) == 0) {
                found = true;
                break;
            }
        }
        if (!found && ouiCount < MAX_DETECTIONS) {
            strncpy(mmOuiBuf[ouiCount], prefix, sizeof(mmOuiBuf[ouiCount]) - 1);
            mmOuiBuf[ouiCount][sizeof(mmOuiBuf[ouiCount]) - 1] = '\0';
            ouiCount++;
        }
    }
    xSemaphoreGive(mmMutex);
    return ouiCount;
}

static void mmWriteOuisJSON(AsyncResponseStream* resp) {
    resp->print("[");
    if (mmMutex && xSemaphoreTake(mmMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        int ouiCount = 0;
        for (int i = 0; i < mmDetCount; i++) {
            char prefix[9];
            snprintf(prefix, sizeof(prefix), "%.8s", mmDet[i].mac);
            int idx = -1;
            for (int j = 0; j < ouiCount; j++) {
                if (strcasecmp(mmOuiBuf[j], prefix) == 0) {
                    idx = j;
                    break;
                }
            }
            if (idx >= 0) {
                mmOuiCounts[idx]++;
            } else if (ouiCount < MAX_DETECTIONS) {
                strncpy(mmOuiBuf[ouiCount], prefix, sizeof(mmOuiBuf[ouiCount]) - 1);
                mmOuiBuf[ouiCount][sizeof(mmOuiBuf[ouiCount]) - 1] = '\0';
                mmOuiCounts[ouiCount] = 1;
                ouiCount++;
            }
        }
        for (int i = 0; i < ouiCount; i++) {
            if (i > 0) resp->print(",");
            resp->printf("{\"oui\":\"%s\",\"count\":%d}", mmOuiBuf[i], mmOuiCounts[i]);
        }
        xSemaphoreGive(mmMutex);
    }
    resp->print("]");
}

static void mmPrintJsonDetCsvRow(Print* out, JsonObject d) {
    char probes[128] = "";
    if (d["probed_ssids"].is<JsonArray>()) {
        for (JsonVariant v : d["probed_ssids"].as<JsonArray>()) {
            const char* s = v.as<const char*>();
            if (!s) {
                continue;
            }
            if (probes[0]) {
                strncat(probes, ";", sizeof(probes) - strlen(probes) - 1);
            }
            strncat(probes, s, sizeof(probes) - strlen(probes) - 1);
        }
    }
    const char* mac = d["mac"] | "";
    const char* name = d["name"] | "";
    const char* ssid = d["ssid"] | "";
    int rssi = d["rssi"] | 0;
    const char* method = d["method"] | "";
    unsigned ch = (unsigned)(d["channel"] | 0);
    unsigned long first = d["first"] | 0;
    unsigned long last = d["last"] | 0;
    int count = d["count"] | 1;
    JsonObject gps = d["gps"];
    if (!gps.isNull() && gps.containsKey("lat")) {
        out->printf("\"%s\",\"%s\",\"%s\",\"%s\",%d,\"%s\",%u,%lu,%lu,%d,%.8f,%.8f,%.1f\n",
                    mac, name, ssid, probes, rssi, method, ch, first, last, count,
                    (double)(gps["lat"] | 0.0), (double)(gps["lon"] | 0.0),
                    (double)(gps["acc"] | 0.0));
    } else {
        out->printf("\"%s\",\"%s\",\"%s\",\"%s\",%d,\"%s\",%u,%lu,%lu,%d,,,\n",
                    mac, name, ssid, probes, rssi, method, ch, first, last, count);
    }
}

// ============================================================================
// DASHBOARD HTML
// ============================================================================

static const char MM_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no">
<meta http-equiv="Cache-Control" content="no-store, no-cache, must-revalidate">
<title>MEGA-MAID</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
html,body{height:100%;overflow:hidden;overflow-x:hidden;max-width:100vw}
body{font-family:'Courier New',monospace;background:#0a0012;color:#e0e0e0;display:flex;flex-direction:column;overflow-x:hidden}
.hd{background:#1a0033;padding:10px 14px;border-bottom:2px solid #ec4899;flex-shrink:0}
.hd h1{font-size:22px;color:#ec4899;letter-spacing:3px}
.hd .sub{font-size:11px;color:#8b5cf6;margin-top:2px}
.st{display:flex;gap:8px;padding:8px 12px;background:rgba(139,92,246,.08);border-bottom:1px solid rgba(139,92,246,.19);flex-shrink:0}
.sc{flex:1;text-align:center;padding:6px;border:1px solid rgba(139,92,246,.25);border-radius:5px}
.sc .n{font-size:22px;font-weight:bold;color:#ec4899}
.sc .l{font-size:10px;color:#8b5cf6;margin-top:2px}
.tb{display:flex;border-bottom:1px solid #8b5cf6;flex-shrink:0}
.tb button{flex:1;padding:12px 8px;text-align:center;cursor:pointer;color:#8b5cf6;border:none;background:none;font-family:inherit;font-size:13px;font-weight:bold;letter-spacing:1px;-webkit-tap-highlight-color:rgba(236,72,153,.3);touch-action:manipulation}
.tb button.a{color:#ec4899;border-bottom:2px solid #ec4899;background:rgba(236,72,153,.08)}
.cn{flex:1;overflow-y:auto;overflow-x:hidden;padding:10px;-webkit-overflow-scrolling:touch;max-width:100%}
.pn{display:none;overflow-x:hidden;max-width:100%}.pn.a{display:block}
.det{background:rgba(45,27,105,.4);border:1px solid rgba(139,92,246,.25);border-radius:7px;padding:10px;margin-bottom:8px;overflow-x:hidden;max-width:100%}
.det .mac{color:#ec4899;font-weight:bold;font-size:14px;word-break:break-all;overflow-wrap:anywhere}
.det .nm{color:#c084fc;font-size:13px;margin-left:4px;word-break:break-word}
.det .inf{display:flex;flex-wrap:wrap;gap:5px;margin-top:5px;font-size:12px;max-width:100%}
.det .inf span{background:rgba(139,92,246,.15);padding:3px 6px;border-radius:4px;word-break:break-word;overflow-wrap:anywhere;max-width:100%}
.pg{margin-bottom:12px}
.pg h3{color:#ec4899;font-size:14px;margin-bottom:4px;border-bottom:1px solid rgba(139,92,246,.19);padding-bottom:4px}
.pg .it{display:flex;flex-wrap:wrap;gap:4px;font-size:12px}
.pg .it span{background:rgba(139,92,246,.15);padding:3px 6px;border-radius:4px;border:1px solid rgba(139,92,246,.12)}
.btn{display:block;width:100%;padding:10px;margin-bottom:8px;background:#8b5cf6;color:#fff;border:none;border-radius:5px;cursor:pointer;font-family:'Courier New',monospace;font-size:13px;font-weight:bold;text-decoration:none;text-align:center;box-sizing:border-box;line-height:1.3}
.btn:active{background:#ec4899}
.btn.alt{background:#6366f1}
.btn.kml{background:#22c55e}
.btn.dng{background:#ef4444}
.badge{display:inline-block;padding:2px 8px;border-radius:4px;font-size:10px;font-weight:bold;letter-spacing:1px;margin-left:8px;vertical-align:middle}
.badge.dash{background:#ec4899;color:#fff}
.badge.coll{background:#6366f1;color:#fff}
.empty{text-align:center;color:rgba(139,92,246,.5);padding:28px;font-size:14px}
.sep{border:none;border-top:1px solid rgba(139,92,246,.12);margin:14px 0}
h4{color:#ec4899;font-size:14px;margin-bottom:6px}
#conn{font-size:10px;color:#8b5cf6;padding:4px 12px;flex-shrink:0}
.tools .summary{font-size:11px;color:#8b5cf6;margin-bottom:10px}
.tools .section{margin-bottom:4px}
.tools .btn-stack{display:flex;flex-direction:column;gap:8px}
.tools .btn-stack .btn{margin-bottom:0}
</style></head><body>
<div class="hd"><h1>MEGA MAID<span class="badge dash" id="modeBadge">DASHBOARD</span></h1><div class="sub">WiFi MAC collector &bull; AP always on</div></div>
<div class="st">
<div class="sc"><div class="n" id="sT">0</div><div class="l">DETECTED</div></div>
<div class="sc"><div class="n" id="sR">0</div><div class="l">OUIs</div></div>
<div class="sc"><div class="n" id="sB">—</div><div class="l" id="sBL">SCAN</div></div>
<div class="sc" onclick="reqGPS()" style="cursor:pointer"><div class="n" id="sG" style="font-size:14px">TAP</div><div class="l" id="sGL">GPS</div></div>
</div>
<div id="conn">connecting...</div>
<div class="tb">
<button class="a" onclick="tab(0,this)">LIVE</button>
<button onclick="tab(1,this)">PREV</button>
<button onclick="tab(2,this)">OUIs</button>
<button onclick="tab(3,this)">TOOLS</button>
</div>
<div class="cn">
<div class="pn a" id="p0"><div id="dL"><div class="empty">Waiting for detections...</div></div></div>
<div class="pn" id="p1"><div id="hL"><div class="empty">Open tab to load prior session</div></div></div>
<div class="pn" id="p2"><div id="pC"><div class="empty">Open tab to load OUIs</div></div></div>
<div class="pn" id="p3"><div class="tools">
<div class="section">
<h4>CURRENT SESSION</h4>
<p class="summary" id="exportSummary">Ready to export 0 detections</p>
<div class="btn-stack">
<a class="btn alt" href="/api/export/json">DOWNLOAD JSON</a>
<button type="button" class="btn alt" onclick="copyText('/api/export/json','JSON')">COPY JSON</button>
<a class="btn" href="/api/export/csv">DOWNLOAD CSV</a>
<a class="btn kml" href="/api/export/kml">DOWNLOAD KML</a>
</div>
</div>
<hr class="sep">
<div class="section">
<h4>PRIOR SESSION</h4>
<p class="summary" id="prevSummary">Prior session on SPIFFS</p>
<div class="btn-stack">
<a class="btn alt" href="/api/history/json">DOWNLOAD JSON</a>
<button type="button" class="btn alt" onclick="copyText('/api/history/json','PREV JSON')">COPY JSON</button>
<a class="btn" href="/api/history/csv">DOWNLOAD CSV</a>
<a class="btn kml" href="/api/history/kml">DOWNLOAD KML</a>
</div>
</div>
<hr class="sep">
<div class="section">
<div class="btn-stack">
<button type="button" class="btn dng" onclick="clearAll()">CLEAR ALL DETECTIONS</button>
</div>
</div>
</div></div>
</div>
<script>
var D=[],H=[],_prevCount=0;
function fetchT(url,ms){
  return new Promise(function(ok,no){
    var done=false,t=setTimeout(function(){if(!done){done=true;no(new Error('timeout'));}},ms);
    fetch(url,{cache:'no-store'}).then(function(r){if(!done){done=true;clearTimeout(t);ok(r);}}).catch(function(e){if(!done){done=true;clearTimeout(t);no(e);}});
  });
}
function tab(i,el){
  document.querySelectorAll('.tb button').forEach(function(b){b.classList.remove('a');});
  document.querySelectorAll('.pn').forEach(function(p){p.classList.remove('a');});
  if(el) el.classList.add('a');
  document.getElementById('p'+i).classList.add('a');
  if(i===1&&!window._hL) loadHistory();
  if(i===2&&!window._pL) loadPat();
}
function setConn(msg,ok){
  var c=document.getElementById('conn');if(!c)return;
  c.textContent=msg;c.style.color=ok?'#22c55e':'#facc15';
}
function refresh(){
  fetchT('/api/ping',3000).then(function(r){
    if(!r.ok) throw new Error('ping');
    setConn('connected',true);
    return fetchT('/api/detections?limit=100',8000);
  }).then(function(r){return r.json();}).then(function(d){D=d;render();return stats();})
  .catch(function(){setConn('reconnecting...',false);});
}
function render(){
  var el=document.getElementById('dL');if(!el)return;
  if(!D.length){el.innerHTML='<div class="empty">No detections yet — collect mode scans WiFi ch 1–14</div>';return;}
  D.sort(function(a,b){return b.last-a.last;});
  el.innerHTML=D.map(card).join('');
}
function stats(){
  return fetchT('/api/stats',5000).then(function(r){return r.json();}).then(function(s){
    var sT=document.getElementById('sT');if(sT)sT.textContent=s.total;
    var sR=document.getElementById('sR');if(sR)sR.textContent=s.unique_ouis;
    var b=document.getElementById('sB'),bl=document.getElementById('sBL');
    if(b&&bl){
      var w=s.wifi_sniff==='active',ble=s.ble==='active';
      if(w){b.textContent='WiFi';b.style.color='#22c55e';bl.textContent='ACTIVE';}
      else if(ble){b.textContent='BLE';b.style.color='#22c55e';bl.textContent='ACTIVE';}
      else{b.textContent='OFF';b.style.color='#666';bl.textContent='SCAN';}
    }
    var mb=document.getElementById('modeBadge');
    if(mb){if(s.radio_profile==='collection'){mb.textContent='COLLECTION';mb.className='badge coll';}else{mb.textContent='DASHBOARD';mb.className='badge dash';}}
    var es=document.getElementById('exportSummary');if(es)es.textContent='Ready to export '+s.total+' detection'+(s.total===1?'':'s');
    var ps=document.getElementById('prevSummary');
    if(ps)ps.textContent=(s.prev_session_count>0)?('Prior session: '+s.prev_session_count+' detections'):'No prior session on SPIFFS';
    _prevCount=s.prev_session_count||0;
    var g=document.getElementById('sG'),gl=document.getElementById('sGL');
    if(g&&gl){
      if(s.gps_src==='hw'){g.textContent=s.gps_sats+'sat';g.style.color='#22c55e';gl.textContent='HW GPS';}
      else if(s.gps_src==='phone'){g.textContent=s.gps_tagged+'/'+s.total;g.style.color='#22c55e';gl.textContent='PHONE';}
      else if(s.gps_hw_detected){g.textContent=s.gps_sats+'sat';g.style.color='#facc15';gl.textContent='NO FIX';}
      else{g.textContent='TAP';g.style.color='#ef4444';gl.textContent='GPS';}
    }
  });
}
function copyText(url,label){
  return fetchT(url,15000).then(function(r){if(!r.ok)throw new Error();return r.text();}).then(function(t){
    if(navigator.clipboard&&navigator.clipboard.writeText)return navigator.clipboard.writeText(t);
    var ta=document.createElement('textarea');ta.value=t;document.body.appendChild(ta);ta.select();document.execCommand('copy');document.body.removeChild(ta);
    return t;
  }).then(function(t){alert(label+' copied ('+t.length+' chars)');});
}
function clearAll(){if(!confirm('Clear all detections?'))return;fetchT('/api/clear',5000).then(function(){refresh();}).catch(function(){alert('Clear failed');});}
function escHtml(s){return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;');}
function card(d){
  return '<div class="det"><div class="mac">'+escHtml(d.mac)+(d.name?'<span class="nm">'+escHtml(d.name)+'</span>':'')+(d.ssid?'<span class="nm">'+escHtml(d.ssid)+'</span>':'')+'</div><div class="inf"><span>RSSI: '+d.rssi+'</span><span>'+escHtml(d.method)+'</span><span style="color:#ec4899;font-weight:bold">&times;'+d.count+'</span>'+(d.probed_ssids&&d.probed_ssids.length?'<span style="color:#8b5cf6">probes: '+d.probed_ssids.map(escHtml).join(', ')+'</span>':'')+(d.gps?'<span style="color:#22c55e">&#9673; '+d.gps.lat.toFixed(5)+','+d.gps.lon.toFixed(5)+'</span>':'<span style="color:#666">no gps</span>')+'</div></div>';
}
function loadHistory(){
  var el=document.getElementById('hL');if(!el)return;
  if(_prevCount<=0){el.innerHTML='<div class="empty">No prior session on SPIFFS</div>';window._hL=1;return;}
  el.innerHTML='<div class="empty">Loading prior session...</div>';
  fetchT('/api/history',15000).then(function(r){return r.json();}).then(function(d){
    H=d;if(!H.length){el.innerHTML='<div class="empty">No prior session data</div>';return;}
    H.sort(function(a,b){return b.last-a.last;});
    el.innerHTML='<div style="font-size:11px;color:#8b5cf6;margin-bottom:8px">'+H.length+' detections from prior session</div>'+H.map(card).join('');
    window._hL=1;
  }).catch(function(){el.innerHTML='<div class="empty">Load failed — try TOOLS download</div>';});
}
function loadPat(){
  var el=document.getElementById('pC');if(!el)return;
  el.innerHTML='<div class="empty">Loading OUIs...</div>';
  fetchT('/api/ouis',10000).then(function(r){return r.json();}).then(function(p){
    var h='<div class="pg"><h3>Unique OUIs ('+p.length+')</h3><div class="it">';
    if(!p.length)h+='<span>none yet</span>';
    else h+=p.map(function(o){return '<span>'+o.oui+' ('+o.count+')</span>';}).join('');
    h+='</div></div>';el.innerHTML=h;window._pL=1;
  }).catch(function(){el.innerHTML='<div class="empty">OUIs unavailable</div>';});
}
var _gW=null,_gOk=false;
function sendGPS(p){_gOk=true;var g=document.getElementById('sG');if(g){g.textContent='OK';g.style.color='#22c55e';}
fetch('/api/gps?lat='+p.coords.latitude+'&lon='+p.coords.longitude+'&acc='+(p.coords.accuracy||0)).catch(function(){});}
function gpsErr(e){_gOk=false;var g=document.getElementById('sG');if(!g)return;
var msg='ERR';if(e.code===1){msg='DENIED';g.style.color='#ef4444';}else if(e.code===2){msg='N/A';g.style.color='#ef4444';}else if(e.code===3){msg='WAIT';g.style.color='#facc15';}
g.textContent=msg;}
function startGPS(){if(!navigator.geolocation)return false;
if(_gW!==null){navigator.geolocation.clearWatch(_gW);_gW=null;}
var g=document.getElementById('sG');if(g){g.textContent='...';g.style.color='#facc15';}
_gW=navigator.geolocation.watchPosition(sendGPS,gpsErr,{enableHighAccuracy:true,maximumAge:5000,timeout:15000});return true;}
function reqGPS(){if(!navigator.geolocation){alert('GPS not available');return;}if(_gOk)return;startGPS();}
setTimeout(refresh,300);setInterval(refresh,5000);
</script></body></html>
)rawliteral";

// ============================================================================
// RADIO PROFILE (collect vs dashboard) + time-sliced scheduler
// ============================================================================

static void mmRegisterRoutes();
static void mmTryStartServer();
static void mmStopDashboardServices();
static void mmPrepareCollectApOff();
static void mmStopCollectSniff();
static void mmApplyProfileUi();
static const char* mmSliceName(MmRadioSlice slice);
static void mmRevertToCollectMode(const char* transientMsg);
static void mmRadioSchedulerReset();

static void mmUpdateOledStatusLines() {
    char line1[32];
    char line2[32];
    if (mmRadioProfile == MM_RADIO_DASHBOARD) {
        if (mmDashHttpFallback) {
            snprintf(line1, sizeof(line1), "Saved to flash");
            snprintf(line2, sizeof(line2), "Reboot to export");
        } else {
            snprintf(line1, sizeof(line1), "%s",
                     mmServerStarted ? "Phone export ready" : "Starting export...");
            snprintf(line2, sizeof(line2), "http://192.168.4.1");
        }
    } else if (mmCollectScanPaused) {
        snprintf(line1, sizeof(line1), "Scan paused");
        snprintf(line2, sizeof(line2), "KEY1 x2 to resume");
    } else if (mmServerStarted) {
        snprintf(line1, sizeof(line1), "Collecting + export ready");
        snprintf(line2, sizeof(line2), "http://192.168.4.1");
    } else {
        snprintf(line1, sizeof(line1), "Collecting WiFi MACs");
        snprintf(line2, sizeof(line2), "KEY1 x3 when done to export");
    }
    nessoUiPinProfile(line1);
    nessoUiSetInfoLine(line2);
}

static void mmApplyProfileUi() {
    if (mmRadioProfile == MM_RADIO_DASHBOARD) {
        nessoUiSetMode("Mega Maid");
        nessoUiSetProfileBanner("DASHBOARD");
    } else {
        nessoUiSetMode("Mega Maid");
        nessoUiSetProfileBanner("COLLECT");
    }
    mmUpdateOledStatusLines();
    nessoUiRestoreProfile();
}

static void mmRefreshOledStatusTick() {
    static unsigned long last = 0;
    if (millis() - last < 2500) {
        return;
    }
    last = millis();
    if (mmDashboardTransitionActive()
        && mmDashTransPhase != MM_DASH_X_WAIT_HTTP) {
        return;
    }
    mmUpdateOledStatusLines();
}

static void mmRevertToCollectMode(const char* transientMsg) {
    mmDashboardEntryInProgress = false;
    mmPendingDashboardTransition = false;
    mmBleReleasedForDashboard = false;
    mmDashTransPhase = MM_DASH_X_IDLE;
    mmDashHttpFallback = false;
    mmDashTransSkipSave = false;
    mmHeapRecoveryLastMs = 0;
    mmApRecycleAttempted = false;
    mmDashHttpRestartPending = false;
    mmHttpWatchdogFailCount = 0;
    mmHttpWatchdogLastRestartMs = 0;
    mmSaveSyncWait = false;
    mmCollectScanPaused = false;
    mmPrepareCollectApOff();
#ifndef MM_DIAG_DISABLE_BLE
    if (!mmBLEScan) {
        mmStartBleStack();
    }
#endif
    mmRadioSchedulerReset();
    mmRadioProfile = MM_RADIO_COLLECT;
    mmApplyProfileUi();
    if (transientMsg) {
        nessoUiSetStatus(transientMsg);
    }
}

static const char* mmSliceName(MmRadioSlice slice) {
    switch (slice) {
        case MM_SLICE_BLE: return "BLE";
        case MM_SLICE_WIFI_SNIFF: return "WIFI";
        case MM_SLICE_AP: return "AP";
        default: return "?";
    }
}

static uint16_t mmSliceDurationMs(MmRadioSlice slice) {
    if (mmRadioProfile == MM_RADIO_COLLECT) {
        if (slice == MM_SLICE_BLE) return MM_COLLECT_BLE_MS;
        if (slice == MM_SLICE_WIFI_SNIFF) return MM_COLLECT_WIFI_MS;
        return 0;
    }
    if (slice == MM_SLICE_BLE) return MM_DASH_BLE_MS;
    if (slice == MM_SLICE_WIFI_SNIFF) return MM_DASH_WIFI_MS;
    return 0;
}

static MmRadioSlice mmNextSlice(MmRadioSlice slice) {
    if (mmRadioProfile == MM_RADIO_COLLECT) {
        return (slice == MM_SLICE_BLE) ? MM_SLICE_WIFI_SNIFF : MM_SLICE_BLE;
    }
    return (slice == MM_SLICE_BLE) ? MM_SLICE_WIFI_SNIFF : MM_SLICE_BLE;
}

static void mmTryStartServer();
static void mmRestartDashboardServices();
static void mmMaintainDashboardHttp();
static void mmMaintainDashboardAp();

// Yield without letting KEY1 hold-for-menu accumulate (main loop is blocked).
static void mmYieldMs(unsigned long ms) {
    unsigned long end = millis() + ms;
    while ((long)(millis() - end) < 0) {
        nessoResetBootButtonHold();
        nessoPollInput();
        yield();
        delay(1);
    }
}

static void mmRestartDashboardServices();
static void mmEnsureWifiApServing();

static void mmRegisterApEvents() {
    static bool registered = false;
    if (registered) {
        return;
    }
    WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t) {
        if (event == ARDUINO_EVENT_WIFI_AP_STACONNECTED) {
            printf("[MEGA-MAID] AP client connected (profile=%s server=%s dns=%s)\n",
                   mmRadioProfile == MM_RADIO_DASHBOARD ? "dashboard" : "collect",
                   mmServerStarted ? "up" : "pending",
                   mmDnsStarted ? "up" : "off");
        } else if (event == ARDUINO_EVENT_WIFI_AP_STADISCONNECTED) {
            printf("[MEGA-MAID] AP client disconnected\n");
        }
    });
    registered = true;
}

// After promisc/BLE collect, restore clean AP-only WiFi for lwIP + AsyncTCP.
static void mmEnsureWifiApServing() {
    if (!mmDashboardApUp) {
        return;
    }
    mmWiFiSniffStop();
    mmWiFiSniffSetApCoexist(false);
    wifi_mode_t mode = WiFi.getMode();
    if (mode != WIFI_MODE_AP) {
        printf("[MEGA-MAID] Restoring WIFI_AP (was mode %d)\n", (int)mode);
        WiFi.mode(WIFI_AP);
        mmYieldMs(100);
    }
    esp_wifi_set_ps(WIFI_PS_NONE);
}

static bool mmStartDashboardAp() {
    if (mmDashboardApUp) {
        return true;
    }
    mmWiFiSniffStop();
    mmWiFiSniffSetApCoexist(false);
    // Caller stops BLE before AP start when needed; do not deinit NimBLE here
    // (double deinit crashes C6; collect mode re-inits BLE after AP is up).
    // Exit STA/promisc before AP — do NOT use WIFI_OFF (detector.cpp / C6 lwIP).
    WiFi.disconnect(true, true);
    mmYieldMs(100);
    WiFi.persistent(false);
    mmRegisterApEvents();
    WiFi.mode(WIFI_AP);
    esp_wifi_set_ps(WIFI_PS_NONE);
    mmYieldMs(200);
    if (!WiFi.softAP(MM_AP_SSID, MM_AP_PASS, 1, 0, 4)) {
        printf("[MEGA-MAID] softAP start FAILED\n");
        return false;
    }
    mmYieldMs(2000);
    IPAddress ip = WiFi.softAPIP();
    if (ip == IPAddress(0, 0, 0, 0)) {
        printf("[MEGA-MAID] softAP has no IP\n");
        WiFi.softAPdisconnect(true);
        return false;
    }
    mmApStartMs = millis();
    mmDashboardApUp = true;
    printf("[MEGA-MAID] softAP up %s\n", ip.toString().c_str());
    return true;
}

static void mmStopDashboardAp() {
    mmStopDashboardServices();
    if (mmDashboardApUp) {
        WiFi.softAPdisconnect(true);
        mmDashboardApUp = false;
    }
    // Do NOT WiFi.mode(WIFI_OFF) here — tears down lwIP; Collect→Dashboard AP fails.
    mmWiFiSniffSetApCoexist(false);
}

// Prepare collect with AP off so WiFi can hop channels 1–14 (see mmWiFiSniffTick).
static void mmPrepareCollectApOff() {
    mmWiFiSniffStop();
#ifndef MM_DIAG_DISABLE_BLE
    mmBleScanStopWait();
#endif
    mmStopDashboardAp();
    printf("[MEGA-MAID] Collect prep: AP off, WiFi ch 1-14 hop + BLE\n");
}

static void mmStopCollectSniff() {
    mmWiFiSniffStop();
#ifndef MM_DIAG_DISABLE_BLE
    mmBleScanStopWait();
#endif
}

static void mmEnterBleSlice() {
    mmWiFiSniffStop();
    mmWiFiSniffSetApCoexist(false);
#ifndef MM_DIAG_DISABLE_BLE
    bool apUp = mmDashboardApUp;
    if (mmRadioProfile == MM_RADIO_COLLECT && !apUp) {
        wifi_mode_t mode = WiFi.getMode();
        if (mode != WIFI_MODE_NULL) {
            WiFi.softAPdisconnect(true);
            WiFi.mode(WIFI_OFF);
            delay(20);
        }
    }
    if (mmBLEScan) {
        mmBleScanStopWait();
        if (apUp || mmRadioProfile == MM_RADIO_DASHBOARD) {
            mmBLEScan->setInterval(MM_BLE_DASHBOARD_INTERVAL_MS);
            mmBLEScan->setWindow(MM_BLE_DASHBOARD_WINDOW_MS);
        } else {
            mmBLEScan->setInterval(MM_BLE_SLOT_MS);
            mmBLEScan->setWindow(MM_BLE_SLOT_MS);
        }
        if (!apUp) {
            mmBLEScan->start(0, false);
            mmLastBleScan = millis();
            mmLastBleBurst = millis();
        }
    }
#endif
    printf("[MEGA-MAID] slice=BLE%s\n",
           mmDashboardApUp ? " (AP up)" : "");
}

static void mmEnterWifiSniffSlice() {
#ifndef MM_DIAG_DISABLE_BLE
    if (mmBLEScan && mmBLEScan->isScanning()) {
        mmBLEScan->stop();
    }
#endif
    if (mmDashboardApUp) {
        mmWiFiSniffSetApCoexist(true);
        mmWiFiSniffStart();
        printf("[MEGA-MAID] slice=WIFI ch=%u (AP up)\n", mmWiFiSniffCurrentChannel());
    } else if (mmRadioProfile == MM_RADIO_COLLECT) {
        mmStopDashboardServices();
        WiFi.softAPdisconnect(true);
        delay(50);
        mmWiFiSniffSetApCoexist(false);
        mmWiFiSniffStart();
        printf("[MEGA-MAID] slice=WIFI ch=%u\n", mmWiFiSniffCurrentChannel());
    }
}

static void mmEnterSlice(MmRadioSlice slice) {
    switch (slice) {
        case MM_SLICE_BLE: mmEnterBleSlice(); break;
        case MM_SLICE_WIFI_SNIFF: mmEnterWifiSniffSlice(); break;
        case MM_SLICE_AP: break;  // unused — AP stays up in dashboard
    }
}

static void mmRadioSchedulerReset() {
    mmSliceStartMs = millis();
    mmLastSliceTickMs = mmSliceStartMs;
    mmCurrentSlice = MM_SLICE_BLE;
    mmEnterSlice(mmCurrentSlice);
}

static void mmRadioSchedulerTick() {
    if (mmRadioProfile == MM_RADIO_DASHBOARD) {
        // AP-only: no BLE/WiFi sniff — C6 single radio cannot serve HTTP otherwise.
        return;
    }
    if (mmCollectScanPaused) {
        return;
    }

    unsigned long now = millis();

    if (mmRadioProfile == MM_RADIO_COLLECT && now < mmCollectWifiReadyMs) {
        if (mmCurrentSlice != MM_SLICE_BLE) {
            mmCurrentSlice = MM_SLICE_BLE;
            mmSliceStartMs = now;
            mmEnterBleSlice();
        }
        return;
    }

    if (mmCurrentSlice == MM_SLICE_WIFI_SNIFF) {
        mmWiFiSniffTick();
    }

    if (now - mmSliceStartMs < mmSliceDurationMs(mmCurrentSlice)) {
        return;
    }

    mmCurrentSlice = mmNextSlice(mmCurrentSlice);
    mmSliceStartMs = now;
    mmEnterSlice(mmCurrentSlice);
}

static void mmModeConfirmBeep() {
    if (!mmBuzzerOn) return;
    tone(BUZZER_PIN, BUZZER_FREQ, 80);
    mmYieldMs(100);
    noTone(BUZZER_PIN);
}

static void mmStopDashboardServices() {
    if (mmServerStarted) {
        mmServer.end();
        mmServerStarted = false;
        printf("[MEGA-MAID] Web server stopped\n");
    }
    if (mmDnsStarted) {
        megamaidDNS.stop();
        mmDnsStarted = false;
        printf("[MEGA-MAID] Captive DNS stopped\n");
    }
}

static void mmPrepareHeapForHttp() {
    unsigned before = ESP.getFreeHeap();
    mmStopDashboardServices();
    mmWiFiSniffStop();
#ifndef MM_DIAG_DISABLE_BLE
    mmBleScanStopWait();
    if (ESP.getFreeHeap() < MM_HTTP_MIN_HEAP && NimBLEDevice::isInitialized()) {
        printf("[MEGA-MAID] Low heap — stopping BLE stack (heap=%u)\n", before);
        mmStopBleStack();
    }
#endif
    unsigned after = ESP.getFreeHeap();
    if (after != before) {
        printf("[MEGA-MAID] Heap prep: %u -> %u\n", before, after);
    }
}

static bool mmDashboardTransitionActive() {
    return mmDashTransPhase != MM_DASH_X_IDLE;
}

static void mmSetTransitionStatus(const char* line1, const char* line2) {
    nessoUiPinProfile(line1);
    nessoUiSetInfoLine(line2);
}

static void mmCancelDashboardTransition(const char* reason) {
    mmDashTransPhase = MM_DASH_X_IDLE;
    mmSaveSyncWait = false;
    mmDashboardEntryInProgress = false;
    mmPendingDashboardTransition = false;
    mmDashHttpFallback = false;
    mmDashTransSkipSave = false;
    mmHeapRecoveryLastMs = 0;
    mmApRecycleAttempted = false;
    if (mmRadioProfile == MM_RADIO_DASHBOARD) {
        mmRevertToCollectMode(reason);
    } else {
#ifndef MM_DIAG_DISABLE_BLE
        mmSaveEnterIdle();
#endif
        if (reason) {
            nessoUiSetStatus(reason);
        }
        mmApplyProfileUi();
    }
    printf("[MEGA-MAID] Dashboard transition cancelled: %s\n", reason ? reason : "?");
}

static void mmShowHttpFallbackUi() {
    mmDashHttpFallback = true;
    mmBleReleasedForDashboard = true;
    nessoUiPinProfile("Saved to flash");
    nessoUiSetInfoLine("Reboot to export");
    printf("[MEGA-MAID] HTTP start timeout — session on SPIFFS, reboot for HISTORY export (heap=%u)\n",
           (unsigned)ESP.getFreeHeap());
    if (mmBuzzerOn) {
        tone(BUZZER_PIN, 440, 100);
        mmYieldMs(120);
        tone(BUZZER_PIN, 330, 150);
        mmYieldMs(170);
        noTone(BUZZER_PIN);
    }
}

static void mmBeginDashboardTransition(bool skipSave) {
    if (mmDashTransPhase != MM_DASH_X_IDLE) {
        return;
    }
    nessoResetBootButtonHold();
    mmDashTransSkipSave = skipSave;
    mmDashHttpFallback = false;
    mmBleReleasedForDashboard = false;
    mmHeapRecoveryLastMs = 0;
    mmApRecycleAttempted = false;
    mmDashHttpRestartPending = false;
    mmHttpWatchdogFailCount = 0;
    mmHttpWatchdogLastRestartMs = 0;
    mmDashboardEntryInProgress = true;
    mmDashTransPhase = MM_DASH_X_STOP_SCAN;
    mmDashTransPhaseStart = millis();
    printf("[MEGA-MAID] Dashboard transition started (skipSave=%d)\n", skipSave ? 1 : 0);
}

static void mmDashboardTransitionTick() {
    if (mmDashTransPhase == MM_DASH_X_IDLE) {
        return;
    }

    if (mmSaveBusy()) {
        mmSaveTick();
    }

    switch (mmDashTransPhase) {
        case MM_DASH_X_STOP_SCAN:
            mmStopCollectSniff();
            mmWiFiSniffSetApCoexist(false);
            mmSetTransitionStatus("Preparing export...", "Stopping scan");
            if (!mmDashTransSkipSave && mmDetCount > 0
                && (mmSaveBusy() || mmDetCount != mmLastSaveCount || mmSavePending)) {
                mmSaveSyncWait = true;
                if (!mmSaveBusy()) {
                    mmRequestSave();
                }
                mmDashTransPhase = MM_DASH_X_SAVE;
                printf("[MEGA-MAID] Saving %d detections before dashboard...\n", mmDetCount);
            } else if (mmSaveBusy()) {
                mmSaveSyncWait = true;
                mmDashTransPhase = MM_DASH_X_SAVE;
            } else {
                mmDashTransPhase = MM_DASH_X_FREE_HEAP;
            }
            mmDashTransPhaseStart = millis();
            break;

        case MM_DASH_X_SAVE: {
            char buf[32];
            snprintf(buf, sizeof(buf), "Saving %d...", mmDetCount);
            mmSetTransitionStatus(buf, "KEY1 x3 to cancel");
            mmSaveTick();
            if (!mmSaveBusy()) {
                mmSaveSyncWait = false;
                mmDashTransPhase = MM_DASH_X_FREE_HEAP;
                mmDashTransPhaseStart = millis();
                break;
            }
            if (millis() - mmDashTransPhaseStart > MM_DASH_SAVE_TIMEOUT_MS) {
                mmSaveSyncWait = false;
                mmCancelDashboardTransition("Save failed — retry KEY1 x3");
            }
            break;
        }

        case MM_DASH_X_FREE_HEAP:
            mmSetTransitionStatus("Freeing memory...", "Please wait");
            mmPrepareHeapForHttp();
#ifndef MM_DIAG_DISABLE_BLE
            mmPauseBleForDashboard();
#endif
            mmDashTransPhase = MM_DASH_X_START_AP;
            mmDashTransPhaseStart = millis();
            break;

        case MM_DASH_X_START_AP:
            mmSetTransitionStatus("Starting AP...", "http://192.168.4.1");
            mmRadioProfile = MM_RADIO_DASHBOARD;
            mmEnsureWifiApServing();
            if (!mmRoutesRegistered) {
                mmRegisterRoutes();
            }
            mmStopDashboardServices();
            mmDashboardApUp = false;
            WiFi.softAPdisconnect(true);
            mmYieldMs(100);
            if (!mmStartDashboardAp()) {
                mmCancelDashboardTransition("AP failed");
                break;
            }
            mmApplyProfileUi();
            mmDashboardEntryInProgress = false;
            mmDashHttpWaitStart = millis();
            mmDashHttpRestartPending = true;
            mmHttpWatchdogFailCount = 0;
            mmHttpWatchdogLastRestartMs = 0;
            mmDashTransPhase = MM_DASH_X_WAIT_HTTP;
            printf("[MEGA-MAID] Dashboard AP recycled — HTTP binds after %ums settle (heap=%u)\n",
                   (unsigned)MM_AP_SETTLE_MS, (unsigned)ESP.getFreeHeap());
            break;

        case MM_DASH_X_WAIT_HTTP:
            if (mmDashboardApUp) {
                mmEnsureWifiApServing();
                mmRecoverDashboardRadioForHttp();
                if (mmDashHttpRestartPending
                    && millis() - mmApStartMs >= MM_AP_SETTLE_MS
                    && mmHttpHeapReady()) {
                    mmDashHttpRestartPending = false;
                    mmHttpWatchdogLastRestartMs = millis();
                    mmRestartDashboardServices();
                } else {
                    mmMaintainDashboardHttp();
                }
                if (mmDnsStarted) {
                    megamaidDNS.processNextRequest();
                }
                mmMaintainDashboardAp();
            }
            if (mmServerStarted) {
                mmDashTransPhase = MM_DASH_X_IDLE;
                mmModeConfirmBeep();
                mmUpdateOledStatusLines();
                printf("[MEGA-MAID] Dashboard ready heap=%u\n", (unsigned)ESP.getFreeHeap());
                break;
            }
            if (!mmDashHttpFallback && millis() - mmDashHttpWaitStart > MM_HTTP_FALLBACK_MS) {
                mmShowHttpFallbackUi();
                mmDashTransPhase = MM_DASH_X_IDLE;
            }
            break;

        default:
            break;
    }
}

// Retry heap recovery for HTTP until heap is sufficient or fallback UI is shown.
static void mmRecoverDashboardRadioForHttp() {
    if (mmDashboardEntryInProgress) {
        return;
    }
    if (mmRadioProfile != MM_RADIO_DASHBOARD || !mmDashboardApUp) {
        return;
    }
    if (mmDashHttpFallback) {
        return;
    }
    if (millis() - mmApStartMs < MM_AP_SETTLE_MS) {
        return;
    }
    if (mmHttpHeapReady()) {
        mmBleReleasedForDashboard = true;
        return;
    }

    if (mmHeapRecoveryLastMs != 0
        && (millis() - mmHeapRecoveryLastMs < MM_HEAP_RECOVERY_INTERVAL_MS)) {
        return;
    }
    mmHeapRecoveryLastMs = millis();

    size_t freeHeap = ESP.getFreeHeap();
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    printf("[MEGA-MAID] Low heap for HTTP — recovery attempt (free=%u largest=%u need=%u)\n",
           (unsigned)freeHeap, (unsigned)largest, (unsigned)MM_HTTP_MIN_HEAP);

    mmPrepareHeapForHttp();

    if (mmHttpHeapReady()) {
        mmBleReleasedForDashboard = true;
        printf("[MEGA-MAID] Heap recovered: free=%u largest=%u\n",
               (unsigned)ESP.getFreeHeap(),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        return;
    }

    if (mmApRecycleAttempted) {
        return;
    }
    mmApRecycleAttempted = true;

    mmEnsureWifiApServing();
    mmStopDashboardServices();
    mmDashboardApUp = false;
    if (!mmStartDashboardAp()) {
        printf("[MEGA-MAID] AP recycle failed (heap=%u)\n", (unsigned)ESP.getFreeHeap());
        return;
    }
    printf("[MEGA-MAID] AP recycled, heap=%u largest=%u\n",
           (unsigned)ESP.getFreeHeap(),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    if (mmHttpHeapReady()) {
        mmBleReleasedForDashboard = true;
    }
}

// Always stop then start — AsyncTCP wedged state cannot be fixed by begin() alone.
static void mmRestartDashboardServices() {
    mmStopDashboardServices();
    mmYieldMs(50);
    mmTryStartServer();
    printf("[MEGA-MAID] HTTP restart heap=%u largest=%u det=%d\n",
           (unsigned)ESP.getFreeHeap(),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
           mmDetCount);
}

static void mmMaintainDashboardHttp() {
    if (mmRadioProfile != MM_RADIO_DASHBOARD || mmDashHttpFallback) {
        return;
    }
    if (!mmDashboardApUp || millis() - mmApStartMs < MM_AP_SETTLE_MS) {
        return;
    }

    if (!mmServerStarted) {
        if (!mmHttpHeapReady()) {
            return;
        }
        if (millis() - mmHttpWatchdogLastRestartMs < MM_HTTP_WATCHDOG_RESTART_MS) {
            return;
        }
        mmHttpWatchdogLastRestartMs = millis();
        mmRestartDashboardServices();
        if (!mmServerStarted) {
            mmHttpWatchdogFailCount++;
            if (mmHttpWatchdogFailCount >= MM_HTTP_WATCHDOG_MAX_FAILS) {
                mmShowHttpFallbackUi();
            }
        }
        return;
    }

    bool heapLow = !mmHttpHeapReady();
    bool idleTooLong = (millis() - mmServerStartMs > MM_HTTP_WATCHDOG_IDLE_MS)
                       && (millis() - mmServerLastRequestMs > MM_HTTP_WATCHDOG_IDLE_MS);
    if (!heapLow && !idleTooLong) {
        return;
    }
    if (millis() - mmHttpWatchdogLastRestartMs < MM_HTTP_WATCHDOG_RESTART_MS) {
        return;
    }

    printf("[MEGA-MAID] HTTP watchdog restart (heapLow=%d idle=%d)\n",
           heapLow ? 1 : 0, idleTooLong ? 1 : 0);
    mmHttpWatchdogLastRestartMs = millis();
    mmRestartDashboardServices();
    if (!mmServerStarted) {
        mmHttpWatchdogFailCount++;
        if (mmHttpWatchdogFailCount >= MM_HTTP_WATCHDOG_MAX_FAILS) {
            mmShowHttpFallbackUi();
        }
    } else {
        mmHttpWatchdogFailCount = 0;
    }
}

static void mmEnterCollectMode() {
    if (mmRadioProfile == MM_RADIO_COLLECT && !mmDashboardTransitionActive()) {
        return;
    }
    mmDashTransPhase = MM_DASH_X_IDLE;
    mmDashHttpFallback = false;
    mmDashTransSkipSave = false;
    mmHeapRecoveryLastMs = 0;
    mmApRecycleAttempted = false;
    mmDashHttpRestartPending = false;
    mmHttpWatchdogFailCount = 0;
    mmHttpWatchdogLastRestartMs = 0;
    nessoResetBootButtonHold();
    mmCollectScanPaused = false;
    printf("[MEGA-MAID] COLLECTION mode: AP off, WiFi ch 1-14 + BLE (50/50)\n");

    mmPrepareCollectApOff();

#ifndef MM_DIAG_DISABLE_BLE
    if (!mmBLEScan) {
        mmStartBleStack();
    }
#endif
    mmRadioProfile = MM_RADIO_COLLECT;
    mmBleReleasedForDashboard = false;
    mmCollectWifiReadyMs = millis() + 2000;
    mmRadioSchedulerReset();
    mmApplyProfileUi();
    printf("[MEGA-MAID] COLLECT ready heap=%u\n", (unsigned)ESP.getFreeHeap());
    mmModeConfirmBeep();
}

static void mmToggleCollectScanPause() {
    if (mmRadioProfile != MM_RADIO_COLLECT
        || mmDashboardTransitionActive()
        || mmPendingDashboardTransition
        || mmDashboardEntryInProgress
        || mmSaveBusy()) {
        return;
    }
    mmCollectScanPaused = !mmCollectScanPaused;
    if (mmCollectScanPaused) {
        mmStopCollectSniff();
        printf("[MEGA-MAID] Scan paused (AP stays off)\n");
    } else {
        mmEnterSlice(mmCurrentSlice);
        printf("[MEGA-MAID] Scan resumed\n");
    }
    mmUpdateOledStatusLines();
}

static void mmToggleRadioProfile() {
    nessoResetBootButtonHold();
    if (mmDashboardEntryInProgress && !mmDashboardTransitionActive()) {
        mmDashboardEntryInProgress = false;
    }
    if (mmDashboardTransitionActive()) {
        mmCancelDashboardTransition("Cancelled");
        return;
    }
    if (mmRadioProfile == MM_RADIO_COLLECT) {
        if (!mmPendingDashboardTransition && !mmDashboardEntryInProgress) {
            mmPendingDashboardTransition = true;
            printf("[MEGA-MAID] KEY1 tclick -> queue dashboard\n");
            printf("[MEGA-MAID] Collect→Dashboard transition queued\n");
        }
    } else {
        mmPendingDashboardTransition = false;
        mmEnterCollectMode();
    }
}

static void mmPollKey1DoubleClick() {
    bool down = boardMenuButtonPressed();
    unsigned long now = millis();
    unsigned long dclickMs = mmKey1DclickWindowMs();

    if (down && !mmKey1WasDown) {
        mmKey1WasDown = true;
    } else if (!down && mmKey1WasDown) {
        mmKey1WasDown = false;
        if (now - mmKey1LastReleaseMs <= dclickMs) {
            mmKey1ClickCount++;
        } else {
            mmKey1ClickCount = 1;
        }
        mmKey1LastReleaseMs = now;
        if (mmKey1ClickCount == 1
            && mmDetCount >= MM_NEAR_CAP_THRESHOLD
            && mmRadioProfile == MM_RADIO_COLLECT
            && !mmDashboardTransitionActive()) {
            nessoUiSetInfoLine("Tap KEY1 x3 to export");
        }
        if (mmKey1ClickCount >= 3) {
            mmKey1ClickCount = 0;
            mmToggleRadioProfile();
        }
    } else if (mmKey1ClickCount > 0 && (now - mmKey1LastReleaseMs) > dclickMs) {
        uint8_t clicks = mmKey1ClickCount;
        mmKey1ClickCount = 0;
        if (clicks == 2
            && mmRadioProfile == MM_RADIO_COLLECT
            && !mmDashboardTransitionActive()
            && !mmPendingDashboardTransition
            && !mmDashboardEntryInProgress) {
            mmToggleCollectScanPause();
        }
    }
}

// ============================================================================
// WEB SERVER SETUP
// ============================================================================

static bool mmReadSessionJsonObject(File& f, char* buf, size_t cap, size_t& outLen) {
    outLen = 0;
    int depth = 0;
    bool started = false;
    while (outLen < cap) {
        int c = f.read();
        if (c < 0) {
            return false;
        }
        buf[outLen++] = (char)c;
        if (c == '{') {
            depth++;
            started = true;
        } else if (c == '}' && started) {
            depth--;
            if (depth == 0) {
                return true;
            }
        }
    }
    return false;
}

static bool mmSkipSessionWhitespaceAndCommas(File& f) {
    while (f.available()) {
        int p = f.peek();
        if (p == ' ' || p == '\n' || p == '\r' || p == '\t' || p == ',') {
            f.read();
            continue;
        }
        return true;
    }
    return false;
}

static size_t mmDetPageStreamFiller(uint8_t* buf, size_t maxLen, size_t index) {
    (void)index;
    if (!mmDetPageStream.active || !mmDetPageStream.f) {
        return 0;
    }

    size_t off = 0;
    char obj[768];

    if (mmDetPageStream.phase == 0) {
        if (maxLen < 1) {
            return 0;
        }
        buf[off++] = '[';
        mmDetPageStream.phase = 1;
        if (mmDetPageStream.f.peek() == '[') {
            mmDetPageStream.f.read();
        }
    }

    while (mmDetPageStream.phase == 1 && off < maxLen) {
        if (!mmSkipSessionWhitespaceAndCommas(mmDetPageStream.f)) {
            mmDetPageStream.phase = 2;
            break;
        }
        if (mmDetPageStream.f.peek() == ']') {
            mmDetPageStream.phase = 2;
            break;
        }

        if (mmDetPageStream.skipped < mmDetPageStream.offset) {
            size_t n = 0;
            if (!mmReadSessionJsonObject(mmDetPageStream.f, obj, sizeof(obj), n)) {
                mmDetPageStream.phase = 2;
                break;
            }
            mmDetPageStream.skipped++;
            continue;
        }

        if (mmDetPageStream.emitted >= mmDetPageStream.limit) {
            mmDetPageStream.phase = 2;
            break;
        }

        if (mmDetPageStream.emitted > 0) {
            if (off + 1 > maxLen) {
                return off;
            }
            buf[off++] = ',';
        }

        size_t n = 0;
        if (!mmReadSessionJsonObject(mmDetPageStream.f, obj, sizeof(obj), n)) {
            mmDetPageStream.phase = 2;
            break;
        }
        if (off + n > maxLen) {
            return off;
        }
        memcpy(buf + off, obj, n);
        off += n;
        mmDetPageStream.emitted++;
    }

    if (mmDetPageStream.phase == 2) {
        if (off < maxLen) {
            buf[off++] = ']';
            mmDetPageStream.f.close();
            mmDetPageStream.active = false;
        }
    }
    return off;
}

static bool mmServeDetectionsFromSession(AsyncWebServerRequest* r, int offset, int limit) {
    if (!mmSpiffsReady || !mmValidateSessionFile(MM_SESSION_FILE)) {
        return false;
    }
    if (mmDetPageStream.active || mmBodyStream.active) {
        r->send(503, "application/json", "{\"error\":\"busy\"}");
        return true;
    }

    size_t bodyOffset = 0;
    if (!mmValidateSessionFile(MM_SESSION_FILE, &bodyOffset, nullptr)) {
        return false;
    }

    mmDetPageStream.f = SPIFFS.open(MM_SESSION_FILE, "r");
    if (!mmDetPageStream.f) {
        return false;
    }
    mmDetPageStream.f.seek(bodyOffset, SeekSet);
    mmDetPageStream.offset = offset;
    mmDetPageStream.limit = limit;
    mmDetPageStream.skipped = 0;
    mmDetPageStream.emitted = 0;
    mmDetPageStream.phase = 0;
    mmDetPageStream.active = true;

    AsyncWebServerResponse* resp =
        r->beginChunkedResponse("application/json", mmDetPageStreamFiller);
    resp->addHeader("Cache-Control", "no-store, no-cache, must-revalidate");
    r->send(resp);
    return true;
}

static void mmRegisterRoutes() {
    if (mmRoutesRegistered) {
        return;
    }

    // Dashboard
    mmServer.on("/", HTTP_GET, [](AsyncWebServerRequest *r) {
        mmServerLastRequestMs = millis();
        AsyncWebServerResponse *resp = r->beginResponse_P(200, "text/html", MM_HTML);
        resp->addHeader("Cache-Control", "no-store, no-cache, must-revalidate");
        resp->addHeader("Pragma", "no-cache");
        r->send(resp);
    });

    mmServer.on("/api/ping", HTTP_GET, [](AsyncWebServerRequest *r) {
        mmServerLastRequestMs = millis();
        r->send(200, "text/plain", "ok");
    });

    // API: Detection list (paginated for dashboard)
    mmServer.on("/api/detections", HTTP_GET, [](AsyncWebServerRequest *r) {
        mmServerLastRequestMs = millis();
        int offset = 0;
        int limit = 100;
        if (r->hasParam("offset")) offset = r->getParam("offset")->value().toInt();
        if (r->hasParam("limit")) limit = r->getParam("limit")->value().toInt();
        if (offset < 0) offset = 0;
        if (limit <= 0 || limit > MAX_DETECTIONS) limit = MAX_DETECTIONS;

        if (mmDetCount >= MM_NEAR_CAP_THRESHOLD
            && mmServeDetectionsFromSession(r, offset, limit)) {
            return;
        }

        AsyncResponseStream *resp = r->beginResponseStream("application/json");
        writeDetectionsJSON(resp, offset, limit);
        r->send(resp);
    });

    // API: Stats (includes GPS status)
    mmServer.on("/api/stats", HTTP_GET, [](AsyncWebServerRequest *r) {
        int uniqueOuis = 0, withGPS = 0;
        if (mmMutex && xSemaphoreTake(mmMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            for (int i = 0; i < mmDetCount; i++) {
                if (mmDet[i].hasGPS) withGPS++;
            }
            xSemaphoreGive(mmMutex);
        }
        uniqueOuis = mmCountUniqueOuis();
        const char* gpsSrc = "none";
        if (mmGPSIsHardware && mmHWGPSFix) gpsSrc = "hw";
        else if (mmGPSIsFresh()) gpsSrc = "phone";
        char buf[420];
        snprintf(buf, sizeof(buf),
            "{\"total\":%d,\"unique_ouis\":%d,"
            "\"ble\":\"%s\",\"wifi_sniff\":\"%s\","
            "\"radio_profile\":\"%s\",\"radio_slice\":\"%s\","
            "\"gps_valid\":%s,\"gps_age\":%lu,\"gps_tagged\":%d,"
            "\"gps_src\":\"%s\",\"gps_sats\":%d,\"gps_hw_detected\":%s,"
            "\"prev_session_count\":%d}",
            mmDetCount, uniqueOuis,
            mmCurrentSlice == MM_SLICE_BLE ? "active" : "idle",
            (mmCurrentSlice == MM_SLICE_WIFI_SNIFF && mmWiFiSniffIsActive()) ? "active" : "idle",
            mmRadioProfile == MM_RADIO_COLLECT ? "collection" : "dashboard",
            mmSliceName(mmCurrentSlice),
            mmGPSIsFresh() ? "true" : "false",
            mmGPSValid ? (millis() - mmGPSLastUpdate) : 0UL,
            withGPS,
            gpsSrc, mmHWGPSSats,
            mmHWGPSDetected ? "true" : "false",
            mmPrevSessionCount());
        r->send(200, "application/json", buf);
    });

    // API: Receive GPS from phone browser (ignored when hardware GPS has fix)
    mmServer.on("/api/gps", HTTP_GET, [](AsyncWebServerRequest *r) {
        if (mmHWGPSFix) {
            r->send(200, "application/json",
                "{\"status\":\"ignored\",\"reason\":\"hw_gps_active\"}");
            return;
        }
        if (r->hasParam("lat") && r->hasParam("lon")) {
            double lat = r->getParam("lat")->value().toDouble();
            double lon = r->getParam("lon")->value().toDouble();
            float  acc = r->hasParam("acc") ? r->getParam("acc")->value().toFloat() : 0;
            mmGPSUpdate(lat, lon, acc, /*fromHardware=*/false);
            r->send(200, "application/json", "{\"status\":\"ok\"}");
        } else {
            r->send(400, "application/json", "{\"error\":\"lat,lon required\"}");
        }
    });

    // API: Unique OUI summary from collected MACs
    mmServer.on("/api/ouis", HTTP_GET, [](AsyncWebServerRequest *r) {
        AsyncResponseStream *resp = r->beginResponseStream("application/json");
        mmWriteOuisJSON(resp);
        r->send(resp);
    });

    // API: Export JSON (downloadable file) — SPIFFS file download, not RAM buffer
    mmServer.on("/api/export/json", HTTP_GET, [](AsyncWebServerRequest *r) {
        const char* src = MM_SESSION_FILE;
        if (!SPIFFS.exists(src) && SPIFFS.exists(MM_SESSION_TMP)) {
            src = MM_SESSION_TMP;
        }
        if (mmExportSessionJsonDownload(r, src, "megamaid_detections.json")) {
            return;
        }
        mmStartRamJsonExport(r);
    });

    // API: Export CSV (downloadable file, includes GPS)
    mmServer.on("/api/export/csv", HTTP_GET, [](AsyncWebServerRequest *r) {
        mmStartRamCsvExport(r, "megamaid_detections.csv");
    });

    // API: Export KML (GPS-tagged detections for Google Earth)
    mmServer.on("/api/export/kml", HTTP_GET, [](AsyncWebServerRequest *r) {
        mmStartRamKmlExport(r, "megamaid_detections.kml");
    });

    // API: Prior session history (JSON) — stream body from SPIFFS
    mmServer.on("/api/history", HTTP_GET, [](AsyncWebServerRequest *r) {
        if (!mmSpiffsReady || !SPIFFS.exists(MM_PREV_FILE)) {
            r->send(200, "application/json", "[]");
            return;
        }
        if (!mmStartSessionBodyChunked(r, MM_PREV_FILE, "application/json")) {
            r->send(500, "application/json", "{\"error\":\"history read failed\"}");
        }
    });

    // API: Download prior session as JSON file (body-only, envelope stripped)
    mmServer.on("/api/history/json", HTTP_GET, [](AsyncWebServerRequest *r) {
        if (!mmSpiffsReady || !SPIFFS.exists(MM_PREV_FILE)) {
            r->send(404, "application/json", "{\"error\":\"no prior session\"}");
            return;
        }
        if (!mmExportSessionJsonDownload(r, MM_PREV_FILE, "megamaid_prev_session.json")) {
            r->send(500, "application/json", "{\"error\":\"export failed\"}");
        }
    });

    // API: Download prior session as CSV
    mmServer.on("/api/history/csv", HTTP_GET, [](AsyncWebServerRequest *r) {
        if (!mmSpiffsReady || !SPIFFS.exists(MM_PREV_FILE)) {
            r->send(404, "application/json", "{\"error\":\"no prior session\"}");
            return;
        }
        if (!mmStartHistCsvExport(r, MM_PREV_FILE, "megamaid_prev_session.csv")) {
            r->send(500, "text/plain", "export failed");
        }
    });

    // API: Download prior session as KML
    mmServer.on("/api/history/kml", HTTP_GET, [](AsyncWebServerRequest *r) {
        if (!mmSpiffsReady || !SPIFFS.exists(MM_PREV_FILE)) {
            r->send(404, "application/json", "{\"error\":\"no prior session\"}");
            return;
        }
        if (!mmStartHistKmlExport(r, MM_PREV_FILE, "megamaid_prev_session.kml")) {
            r->send(500, "text/plain", "export failed");
        }
    });

    // API: Clear all detections (saves current session first)
    mmServer.on("/api/clear", HTTP_GET, [](AsyncWebServerRequest *r) {
        mmSaveSession();
        if (mmMutex && xSemaphoreTake(mmMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
            mmDetCount = 0;
            memset(mmDet, 0, sizeof(mmDet));
            mmMacHashClear();
            mmTriggered = false;
            mmDeviceInRange = false;
            mmCapDropLogged = false;
            mmNearCapShown = false;
            mmBackfillCursor = 0;
            nessoUiSetDetectionCount(0);
            xSemaphoreGive(mmMutex);
        }
        r->send(200, "application/json", "{\"status\":\"cleared\"}");
        printf("[MEGA-MAID] All detections cleared (session saved)\n");
    });

    // Captive portal catch-all: redirect any unknown URL to root
    mmServer.onNotFound([](AsyncWebServerRequest *r) {
        r->redirect("http://192.168.4.1/");
    });

    mmRoutesRegistered = true;
}

static void mmTryStartServer() {
    if (mmServerStarted) {
        return;
    }

    if (!mmDashboardApUp) {
        return;
    }

    IPAddress ip = WiFi.softAPIP();
    if (ip == IPAddress(0, 0, 0, 0)) {
        static unsigned long lastLog = 0;
        if (millis() - lastLog >= 5000) {
            printf("[MEGA-MAID] Web server waiting for AP IP...\n");
            lastLog = millis();
        }
        return;
    }

    unsigned long sinceAp = millis() - mmApStartMs;
    if (sinceAp < MM_AP_SETTLE_MS) {
        static unsigned long lastSettleLog = 0;
        if (millis() - lastSettleLog >= 5000) {
            printf("[MEGA-MAID] Web server settle %lums remaining\n",
                   (unsigned long)(MM_AP_SETTLE_MS - sinceAp));
            lastSettleLog = millis();
        }
        return;
    }

    if (!mmHttpHeapReady()) {
        static unsigned long lastHeapLog = 0;
        if (millis() - lastHeapLog >= 5000) {
            printf("[MEGA-MAID] Web server waiting for heap (free=%u largest=%u need=%u)\n",
                   (unsigned)ESP.getFreeHeap(),
                   (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                   (unsigned)MM_HTTP_MIN_HEAP);
            lastHeapLog = millis();
        }
        return;
    }

    mmServer.begin();
    mmServerStarted = true;
    mmServerStartMs = millis();
    mmServerLastRequestMs = millis();
    printf("[MEGA-MAID] Web server started on port 80 (%s) heap=%u largest=%u\n",
           ip.toString().c_str(), (unsigned)ESP.getFreeHeap(),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

    if (!mmDnsStarted) {
        if (megamaidDNS.start(53, "*", ip)) {
            mmDnsStarted = true;
            printf("[MEGA-MAID] Captive portal DNS started\n");
        } else {
            printf("[MEGA-MAID] Captive portal DNS FAILED (HTTP still at http://192.168.4.1)\n");
        }
    }
}

// Restart softAP only in dashboard mode — collect WiFi coexist glitches softAPIP().
static void mmMaintainDashboardAp() {
    if (!mmDashboardApUp || mmRadioProfile != MM_RADIO_DASHBOARD) {
        return;
    }

    IPAddress ip = WiFi.softAPIP();
    static unsigned long apIpMissingSince = 0;

    if (ip != IPAddress(0, 0, 0, 0)) {
        apIpMissingSince = 0;
        return;
    }

    if (apIpMissingSince == 0) {
        apIpMissingSince = millis();
        return;
    }

    if (millis() - apIpMissingSince < 15000) {
        return;
    }

    printf("[MEGA-MAID] softAP IP missing 15s — restarting AP\n");
    mmStopDashboardServices();
    mmDashboardApUp = false;
    apIpMissingSince = 0;
    if (!mmStartDashboardAp()) {
        printf("[MEGA-MAID] softAP restart failed\n");
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
    mmBuzzerOn = bzP.getBool("on", true);
    bzP.end();

    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW);

    // Init NeoPixel
    mmPixel.begin();
    mmPixel.setBrightness(MM_NEOPIXEL_BRIGHTNESS);
    mmPixel.clear();
    mmPixel.show();
    // Test flash: pink -> purple
    mmPixel.setPixelColor(0, mmPixel.Color(236, 72, 153));  // pink #ec4899
    mmPixel.show();
    delay(500);
    mmPixel.setPixelColor(0, mmPixel.Color(139, 92, 246));  // purple #8b5cf6
    mmPixel.show();
    delay(500);
    mmPixel.clear();
    mmPixel.show();

    mmMutex    = xSemaphoreCreateMutex();
    mmGPSMutex = xSemaphoreCreateMutex();
    mmMacHashClear();

    ::boardGpsUartBegin(mmGPSSerial);
    nessoUiSetGpsIndicator(NESSO_GPS_SEARCHING);

    // Init SPIFFS for session persistence
    if (SPIFFS.begin(true)) {
        mmSpiffsReady = true;
        printf("[MEGA-MAID] SPIFFS ready\n");
        // Promote last session to prev_session before we start a new one
        mmPromotePrevSession();
        if (SPIFFS.exists(MM_PREV_FILE)) {
            File pf = SPIFFS.open(MM_PREV_FILE, "r");
            size_t psz = pf ? pf.size() : 0;
            if (pf) pf.close();
            printf("[MEGA-MAID] Prior session on SPIFFS (%u bytes) — export via dashboard HISTORY\n",
                   (unsigned)psz);
        }
    } else {
        printf("[MEGA-MAID] SPIFFS init failed - no persistence\n");
    }

    printf("\n========================================\n");
    printf("  MEGA MAID BLE + WiFi MAC Collector\n");
    printf("  Build: %s\n", MM_BUILD_TAG);
    printf("  Buzzer: %s\n", mmBuzzerOn ? "ON" : "OFF");
    printf("  GPS: auto-detect (%s)\n", ::boardGpsModuleName());
    printf("========================================\n");

    // Init WiFi sniffer; BLE starts when entering collect mode.
    mmWiFiSniffInit();
    mmApplyProfileUi();
    mmBootBeep();

    if (!mmRoutesRegistered) {
        mmRegisterRoutes();
    }
    mmBeginDashboardTransition(true);
    while (mmDashboardTransitionActive()) {
        mmDashboardTransitionTick();
        mmBeepTick();
        delay(10);
        yield();
    }
    if (!mmServerStarted && mmDashboardApUp && !mmDashHttpFallback) {
        mmDashHttpRestartPending = true;
        unsigned long retryEnd = millis() + 10000;
        while (!mmServerStarted && millis() < retryEnd) {
            if (mmDashHttpRestartPending
                && millis() - mmApStartMs >= MM_AP_SETTLE_MS
                && mmHttpHeapReady()) {
                mmDashHttpRestartPending = false;
                mmHttpWatchdogLastRestartMs = millis();
                mmRestartDashboardServices();
            } else {
                mmMaintainDashboardHttp();
            }
            if (mmDnsStarted) {
                megamaidDNS.processNextRequest();
            }
            delay(50);
            yield();
        }
    }
    if (mmRadioProfile != MM_RADIO_DASHBOARD) {
#ifndef MM_DIAG_DISABLE_BLE
    if (!mmBLEScan && mmShouldInitBleStack()) {
        mmStartBleStack();
    }
#endif
        mmRadioSchedulerReset();
        mmApplyProfileUi();
        if (mmServerStarted) {
            printf("[MEGA-MAID] Server up — collect mode (triple-click KEY1 for dashboard)\n");
        } else {
            printf("[MEGA-MAID] WARN: AP up but server down — open http://192.168.4.1 manually\n");
        }
    } else {
        printf("[MEGA-MAID] Dashboard: triple-click KEY1 for collection mode\n");
        printf("[MEGA-MAID] Collect: double-click KEY1 to pause/resume scan\n");
    }
    printf("[MEGA-MAID] Hold KEY1 ~1.5s for mode selector\n");
    printf("[MEGA-MAID] BLE: all advertisements | WiFi: MACs + SSIDs/probes\n");
    printf("[MEGA-MAID] Ready\n\n");
}

void loop() {
    mmPollKey1DoubleClick();
    mmBeepTick();

    if (mmPendingDashboardTransition) {
        mmPendingDashboardTransition = false;
        mmBeginDashboardTransition(false);
    }

    if (mmDashboardTransitionActive()) {
        mmDashboardTransitionTick();
        mmProcessHardwareGPS();
        mmUpdatePixel();
        mmRefreshOledStatusTick();
        delay(20);
        yield();
        return;
    }

    if (mmRadioProfile == MM_RADIO_DASHBOARD) {
        if (mmDashboardApUp) {
            mmEnsureWifiApServing();
            mmRecoverDashboardRadioForHttp();
            mmMaintainDashboardHttp();
            if (mmDnsStarted) {
                megamaidDNS.processNextRequest();
            }
            mmMaintainDashboardAp();
        }
        if (mmSaveSyncWait || mmSaveBusy()) {
            mmSaveTick();
        }
        mmProcessHardwareGPS();
        mmUpdatePixel();
        mmRefreshOledStatusTick();
        static unsigned long lastStatusLog = 0;
        if (millis() - lastStatusLog >= 30000) {
            lastStatusLog = millis();
            printf("[MEGA-MAID] status: hits=%d profile=dashboard server=%s heap=%u\n",
                   mmDetCount,
                   mmServerStarted ? "up" : "off",
                   (unsigned)ESP.getFreeHeap());
        }
        delay(50);
        yield();
        return;
    }

    mmRadioSchedulerTick();
    if (mmRadioProfile == MM_RADIO_COLLECT) {
        mmProcessWiFiAlerts();
    }
#ifndef MM_DIAG_DISABLE_BLE
    mmProcessBleAdverts();
#endif
    mmSaveTick();

    if (mmDashboardApUp) {
        mmMaintainDashboardAp();
        if (mmDetCount < MM_NEAR_CAP_THRESHOLD) {
            mmTryStartServer();
            if (mmDnsStarted) {
                megamaidDNS.processNextRequest();
            }
        } else {
            mmStopDashboardServices();
        }
    }

    mmProcessHardwareGPS();
    mmUpdateGpsIndicator();
    mmUpdatePixel();
    mmRefreshOledStatusTick();

    static unsigned long lastStatusLog = 0;
    if (millis() - lastStatusLog >= 30000) {
        lastStatusLog = millis();
        printf("[MEGA-MAID] status: hits=%d in_range=%s profile=%s slice=%s server=%s save=%s\n",
               mmDetCount,
               mmDeviceInRange ? "yes" : "no",
               mmRadioProfile == MM_RADIO_COLLECT ? "collection" : "dashboard",
               mmSliceName(mmCurrentSlice),
               mmServerStarted ? "up" : "off",
               mmSaveBusy() ? "busy" : "idle");
    }

#ifndef MM_DIAG_DISABLE_BLE
    if (mmRadioProfile == MM_RADIO_COLLECT
        && mmCurrentSlice == MM_SLICE_BLE
        && !mmSaveRadioPaused
        && !mmCollectScanPaused
        && millis() >= mmCollectWifiReadyMs) {
        if (mmBLEScan && !mmBLEScan->isScanning()) {
            unsigned long now = millis();
            unsigned long burstGap = mmDashboardApUp ? MM_BLE_DASHBOARD_INTERVAL_MS : MM_BLE_SLOT_MS;
            if (now - mmLastBleScan >= BLE_SCAN_INTERVAL) {
                mmBLEScan->clearResults();
                mmLastBleScan = now;
            }
            if (now - mmLastBleBurst >= burstGap) {
                mmBLEScan->start(BLE_SCAN_DURATION, false);
                mmLastBleBurst = now;
            }
        }
    }
#endif

    if (mmDeviceInRange) {
        if (millis() - mmLastHB >= 10000) {
            mmQueueHeartbeatBeep();
            mmLastHB = millis();
        }
        if (millis() - mmLastDetTime >= 30000) {
            printf("[MEGA-MAID] Device out of range - stopping heartbeat\n");
            mmDeviceInRange = false;
            mmTriggered = false;
        }
    }

    static unsigned long lastBackfill = 0;
    if (millis() - lastBackfill >= 2000) {
        mmBackfillGPS();
        lastBackfill = millis();
    }

    if (mmSpiffsReady && mmDetCount > 0 && !mmSaveSyncWait
        && mmRadioProfile != MM_RADIO_DASHBOARD
        && millis() >= mmCollectWifiReadyMs) {
        unsigned long now = millis();
        bool countChanged = (mmDetCount != mmLastSaveCount);
        bool minGap       = (now - mmLastSave >= 3000);
        bool firstSave    = (mmLastSaveCount == 0 && now - mmLastSave >= 5000);
        unsigned long saveInterval = (mmDetCount >= MM_NEAR_CAP_THRESHOLD) ? 10000UL : MM_SAVE_INTERVAL;
        bool periodic = (now - mmLastSave >= saveInterval);
        bool shouldSave = firstSave || (countChanged && minGap);
        if (!shouldSave && periodic && mmDetCount != mmLastSaveCount
            && mmDetCount < MAX_DETECTIONS) {
            shouldSave = true;
        }

        if (!mmSaveBusy() && shouldSave) {
            mmRequestSave();
            mmLastSave = now;
        }
    }

    unsigned long now = millis();
    if (now - mmLastLoopEnd < MM_LOOP_INTERVAL_MS) {
        delay(1);
    }
    mmLastLoopEnd = millis();
}
