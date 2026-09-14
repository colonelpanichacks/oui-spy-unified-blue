#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include <NimBLEDevice.h>
#include <NimBLEUtils.h>
#include <NimBLEScan.h>
#include <NimBLEAdvertisedDevice.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <nvs_flash.h>
#include <vector>
#include <algorithm>
#include <FS.h>
#include <SPIFFS.h>
#include <Adafruit_NeoPixel.h>

// ================================
// Pin and Buzzer Definitions - Xiao ESP32 S3
// ================================
#define BUZZER_PIN 3   // GPIO3 (D2) for buzzer - good PWM pin on Xiao ESP32 S3
#define BUZZER_FREQ 2000  // Frequency in Hz
#define BUZZER_DUTY 127  // 50% duty cycle for good volume without excessive power draw
#define BEEP_DURATION 200  // Duration of each beep in ms
#define BEEP_PAUSE 50  // Pause between beeps in ms (faster sequence)
#define LED_PIN 21   // GPIO21 for onboard LED (inverted logic)

// ================================
// NeoPixel Definitions - Xiao ESP32 S3
// ================================
#define NEOPIXEL_PIN 4   // GPIO4 (D3) for NeoPixel - confirmed safe pin on Xiao ESP32 S3
#define NEOPIXEL_COUNT 1 // Number of NeoPixels (1 for single pixel)
#define NEOPIXEL_BRIGHTNESS 50 // Brightness (0-255)
#define NEOPIXEL_DETECTION_BRIGHTNESS 200 // Brightness during detection (0-255)

// NeoPixel object
Adafruit_NeoPixel strip(NEOPIXEL_COUNT, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);

// NeoPixel state variables
bool detectionMode = false;
unsigned long detectionStartTime = 0;
int detectionFlashCount = 0;

// ================================
// WiFi AP Configuration
// ================================
String AP_SSID = "snoopuntothem";
String AP_PASSWORD = "astheysnoopuntous";
#define CONFIG_TIMEOUT 20000   // 20 seconds timeout for config mode

// ================================
// Operating Modes
// ================================
enum OperatingMode {
    CONFIG_MODE,
    SCANNING_MODE
};

// ================================
// Global Variables
// ================================
OperatingMode currentMode = CONFIG_MODE;
AsyncWebServer server(80);
DNSServer detectorDNS;
Preferences preferences;
NimBLEScan* pBLEScan;
unsigned long configStartTime = 0;
unsigned long lastConfigActivity = 0;
unsigned long modeSwitchScheduled = 0; // When to switch modes (0 = not scheduled)
unsigned long deviceResetScheduled = 0; // When to reset device (0 = not scheduled)
unsigned long normalRestartScheduled = 0; // When to do normal restart (0 = not scheduled)

// Serial output synchronization - avoid concurrent writes
volatile bool newMatchFound = false;
String detectedMAC = "";
int detectedRSSI = 0;
String matchedFilter = "";
String matchType = "";  // "NEW", "RE-3s", "RE-30s"

// Persistent settings
bool buzzerEnabled = true;
bool ledEnabled = true;

// Filter classification — determines which BLE advert field the matcher
// checks against `identifier`. Values are persisted to NVS; do NOT
// renumber existing entries or old configs will break.
enum FilterType : uint8_t {
    FT_MAC_PREFIX      = 0,  // identifier = 6-char OUI (e.g. "985949")
    FT_FULL_MAC        = 1,  // identifier = 12-char MAC
    FT_COMPANY_ID      = 2,  // identifier = 4-char hex "0D53" (BT SIG mfr CID)
    FT_SERVICE_UUID_16 = 3,  // identifier = 4-char hex "FD5F" (BT SIG 16-bit svc UUID)
    FT_NAME_SUBSTRING  = 4,  // identifier = case-insensitive substring
    // Meta/Ray-Ban composite (mfr CID 0x0D53 + svc UUID 0xFD5F in the SAME
    // advert, or a name-substring hit). User-installable via the META preset
    // in the OUI Database and persisted like any other filter — no filter
    // installed means no Meta detection. Kept at the end so existing NVS
    // values 0-4 stay stable.
    FT_META_COMPOSITE  = 5,
};

// Short code shown on the dashboard match-type badge. Kept in sync with
// the standalone ouispy-detector so the palette is identical.
static const char* filterTypeCode(FilterType t) {
    switch (t) {
        case FT_MAC_PREFIX:      return "OUI";
        case FT_FULL_MAC:        return "MAC";
        case FT_COMPANY_ID:      return "CID";
        case FT_SERVICE_UUID_16: return "SVC";
        case FT_NAME_SUBSTRING:  return "NAME";
        case FT_META_COMPOSITE:  return "META";
    }
    return "OUI";
}

// Device tracking
struct DeviceInfo {
    String macAddress;
    int rssi;
    unsigned long firstSeen;
    unsigned long lastSeen;
    bool inCooldown;
    unsigned long cooldownUntil;
    const char* matchedFilter;
    String filterDescription;  // Store filter description for persistence
    String matchedIdentifier;  // Raw identifier that triggered (e.g. "985949")
    FilterType matchedType = FT_MAC_PREFIX;  // Which filter class hit
};

struct TargetFilter {
    String identifier;
    bool isFullMAC;      // kept for NVS backwards-compat with pre-typed configs
    String description;
    FilterType type = FT_MAC_PREFIX;  // set from isFullMAC on legacy load
};

struct DeviceAlias {
    String macAddress;
    String alias;
};

std::vector<DeviceInfo> devices;
std::vector<TargetFilter> targetFilters;
std::vector<DeviceAlias> deviceAliases;

// ================================
// Firmware version (reported over CMD:VERSION and /api/session)
// ================================
#ifndef DETECTOR_FW_VERSION
#define DETECTOR_FW_VERSION "1.1.0"
#endif

// ================================
// BLE detection session — flock-you dashboard companion.
//
// Mirrors the Mode 3 (flock-you WiFi) persistence + protocol so the same Flask
// dashboard can pull BLE detections. The subsystem is fully self-contained:
// everything below is prefixed `ble` / `BLE_SESSION_*` so it can be lifted
// wholesale without disturbing Mode 1's target-filter web UI, buzzer, LED,
// NeoPixel, alias, or NVS code paths.
//
// Wire format (SPIFFS envelope):
//   Line 1:  {"v":1,"count":N,"bytes":B,"crc":"0xXXXXXXXX"}\n
//   Line 2+: [{"mac":...},...]      (exactly B bytes, CRC32 == X)
//
// Files live at /ble_session.* — DIFFERENT from Mode 3's /session.* so a
// device flashed between modes can't cross-contaminate the payload stream.
// ================================
#define BLE_SESSION_FILE     "/ble_session.json"
#define BLE_SESSION_TMP      "/ble_session.tmp"
#define BLE_PREV_FILE        "/ble_prev_session.json"
#define BLE_MAX_DETECTIONS   256
#define BLE_AUTOSAVE_MS      60000
#define BLE_CMD_BUF_LEN      160

// Match-method labels (also the JSON `match_method` field value).
#define BLE_MM_OUI_PREFIX     "oui_prefix"
#define BLE_MM_FULL_MAC       "full_mac"
#define BLE_MM_COMPANY_ID     "company_id"
#define BLE_MM_SERVICE_UUID   "service_uuid"
#define BLE_MM_NAME_SUBSTRING "name_substring"
#define BLE_MM_META_COMPOSITE "meta_composite"
#define BLE_MM_UNKNOWN        "unknown"

// Address-type labels — mapped from NimBLE getAddressType() plus the top-2-bit
// check on the first octet for random addresses (RFC-standard RPA/NRPA/static).
#define BLE_ADDR_LABEL_PUBLIC "public"
#define BLE_ADDR_LABEL_RANDOM "random_static"
#define BLE_ADDR_LABEL_RPA    "rpa"
#define BLE_ADDR_LABEL_NRPA   "nrpa"

// Per-detection record. Kept flat + fixed-size so the whole table can be
// mem-serialized without touching the heap during autosave.
typedef struct {
    char     mac[18];              // "aa:bb:cc:dd:ee:ff\0"
    char     addrType[16];         // "public" / "random_static" / "rpa" / "nrpa"
    char     matchMethod[20];      // oui_prefix / full_mac / company_id / ...
    char     matchedSig[64];       // human-readable label from OUI Database entry
    char     localName[32];        // truncated device name (may be empty)
    uint16_t companyId;            // 0xFFFF when N/A
    uint16_t serviceUuid;          // 0x0000 when N/A
    int8_t   rssiMin;              // weakest RSSI seen
    int8_t   rssiMax;              // strongest RSSI seen
    uint32_t firstSeen;            // millis() at first hit
    uint32_t lastSeen;             // millis() at latest hit
    uint16_t hitCount;
} BLEDetection;

static BLEDetection  bleDet[BLE_MAX_DETECTIONS];
static int           bleDetCount     = 0;
static bool          bleSpiffsReady  = false;
static bool          bleSessionDirty = false;
static unsigned long bleLastSaveAt   = 0;
static int           bleLastSaveCount = 0;
static bool          blePrevExists   = false;
static size_t        blePrevBytes    = 0;
static char          bleCmdBuf[BLE_CMD_BUF_LEN];
static size_t        bleCmdLen       = 0;

// Forward declarations
void startScanningMode();
void startDetectionFlash();
class MyAdvertisedDeviceCallbacks;
static void bleSessionSetup();
static void blePollSerialCmd();
static void bleAutosaveTick();
static void bleRegisterWebEndpoints();
static void bleNoteDetection(NimBLEAdvertisedDevice* dev, const String& mac,
                             int rssi, const String& matchedSig);

// ================================
// Serial Configuration
// ================================
void initializeSerial() {
    Serial.begin(115200);
    delay(100);
}

bool isSerialConnected() {
    return Serial;
}

// ================================
// LED Control Functions (inverted logic for Xiao ESP32-S3)
// ================================
void ledOn() {
    if (ledEnabled) {
        digitalWrite(LED_PIN, LOW);  // LOW = LED ON for Xiao ESP32-S3
    }
}

void ledOff() {
    if (ledEnabled) {
        digitalWrite(LED_PIN, HIGH); // HIGH = LED OFF for Xiao ESP32-S3
    }
}

// ================================
// Buzzer Functions
// ================================
void initializeBuzzer() {
    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW);
    ledcSetup(0, BUZZER_FREQ, 8);
    ledcAttachPin(BUZZER_PIN, 0);
    
    // Setup LED (inverted logic - HIGH = OFF for Xiao ESP32-S3)
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, HIGH);
}

void digitalBeep(int duration) {
    unsigned long startTime = millis();
    while (millis() - startTime < duration) {
        digitalWrite(BUZZER_PIN, HIGH);
        delayMicroseconds(250);
        digitalWrite(BUZZER_PIN, LOW);
        delayMicroseconds(250);
    }
}

void singleBeep() {
    if (buzzerEnabled) {
        ledcWrite(0, BUZZER_DUTY);
    }
    ledOn();
    delay(BEEP_DURATION);
    if (buzzerEnabled) {
        ledcWrite(0, 0);
        digitalBeep(BEEP_DURATION);
    }
    ledOff();
}

void threeBeeps() {
    // Start detection flash animation
    startDetectionFlash();
    
    for(int i = 0; i < 3; i++) {
        singleBeep();
        if (i < 2) delay(BEEP_PAUSE);
    }
}

// ================================
// NeoPixel Functions
// ================================
void initializeNeoPixel() {
    strip.begin();
    strip.setBrightness(NEOPIXEL_BRIGHTNESS);
    strip.clear();
    strip.show();
}

// Convert HSV to RGB
uint32_t hsvToRgb(uint16_t h, uint8_t s, uint8_t v) {
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
    
    return strip.Color(r, g, b);
}

// Normal pink breathing animation
void normalBreathingAnimation() {
    static unsigned long lastUpdate = 0;
    static float brightness = 0.0;
    static bool increasing = true;
    
    unsigned long currentTime = millis();
    
    // Update every 20ms for smooth animation
    if (currentTime - lastUpdate >= 20) {
        lastUpdate = currentTime;
        
        // Update brightness (breathing effect)
        if (increasing) {
            brightness += 0.02;
            if (brightness >= 1.0) {
                brightness = 1.0;
                increasing = false;
            }
        } else {
            brightness -= 0.02;
            if (brightness <= 0.1) {
                brightness = 0.1;
                increasing = true;
            }
        }
        
        // Pink color (hue 300) with breathing brightness
        uint32_t color = hsvToRgb(300, 255, (uint8_t)(NEOPIXEL_BRIGHTNESS * brightness));
        strip.setPixelColor(0, color);
        strip.show();
    }
}

// Detection flash animation synchronized with beeps
void detectionFlashAnimation() {
    unsigned long currentTime = millis();
    unsigned long elapsed = currentTime - detectionStartTime;
    
    // Calculate which flash we're on based on elapsed time
    int currentFlash = (elapsed / (BEEP_DURATION + BEEP_PAUSE)) % 3;
    unsigned long flashProgress = elapsed % (BEEP_DURATION + BEEP_PAUSE);
    
    // Determine color based on flash number
    uint16_t hue;
    if (currentFlash == 0) {
        hue = 240; // Blue
    } else if (currentFlash == 1) {
        hue = 300; // Pink
    } else {
        hue = 270; // Purple
    }
    
    // Flash brightness - bright during beep, dim during pause
    uint8_t brightness;
    if (flashProgress < BEEP_DURATION) {
        // During beep - bright flash
        brightness = NEOPIXEL_DETECTION_BRIGHTNESS;
    } else {
        // During pause - dim
        brightness = NEOPIXEL_BRIGHTNESS / 4;
    }
    
    // Set color
    uint32_t color = hsvToRgb(hue, 255, brightness);
    strip.setPixelColor(0, color);
    strip.show();
    
    // End detection mode after 3 flashes (same as threeBeeps)
    if (elapsed >= (BEEP_DURATION + BEEP_PAUSE) * 3) {
        detectionMode = false;
    }
}

// Main animation function
void updateNeoPixelAnimation() {
    if (detectionMode) {
        detectionFlashAnimation();
    } else {
        normalBreathingAnimation();
    }
}

// Set NeoPixel to a specific color
void setNeoPixelColor(uint8_t r, uint8_t g, uint8_t b) {
    strip.setPixelColor(0, strip.Color(r, g, b));
    strip.show();
}

// Turn off NeoPixel
void turnOffNeoPixel() {
    strip.clear();
    strip.show();
}

// Start detection flash animation
void startDetectionFlash() {
    detectionMode = true;
    detectionStartTime = millis();
}

void twoBeeps() {
    for(int i = 0; i < 2; i++) {
        singleBeep();
        if (i < 1) delay(BEEP_PAUSE);
    }
}

void ascendingBeeps() {
    // Two fast ascending beeps to indicate "ready to scan"
    int frequencies[] = {1900, 2200}; // Close melodic interval, not octave
    int fastPause = 100; // Faster than normal beeps
    
    for (int i = 0; i < 2; i++) {
        if (buzzerEnabled) {
            ledcSetup(0, frequencies[i], 8);
            ledcWrite(0, BUZZER_DUTY);
        }
        ledOn();
        delay(BEEP_DURATION);
        if (buzzerEnabled) {
            ledcWrite(0, 0);
        }
        ledOff();
        if (i < 1) delay(fastPause);
    }
    
    // Reset to original frequency for future beeps
    if (buzzerEnabled) {
        ledcSetup(0, BUZZER_FREQ, 8);
    }
}

// ================================
// Configuration Storage Functions
// ================================
void saveConfiguration() {
    preferences.begin("ouispy", false);
    preferences.putInt("filterCount", targetFilters.size());
    preferences.putBool("buzzerEnabled", buzzerEnabled);
    preferences.putBool("ledEnabled", ledEnabled);
    
    for (int i = 0; i < targetFilters.size(); i++) {
        String keyId   = "id_"   + String(i);
        String keyMAC  = "mac_"  + String(i);
        String keyDesc = "desc_" + String(i);
        String keyType = "type_" + String(i);

        preferences.putString(keyId.c_str(),   targetFilters[i].identifier);
        preferences.putBool  (keyMAC.c_str(),  targetFilters[i].isFullMAC);
        preferences.putString(keyDesc.c_str(), targetFilters[i].description);
        preferences.putUChar (keyType.c_str(), (uint8_t)targetFilters[i].type);
    }
    
    preferences.end();
    
    if (isSerialConnected()) {
        Serial.println("Configuration saved to NVS");
    }
}

void loadConfiguration() {
    preferences.begin("ouispy", true);
    int filterCount = preferences.getInt("filterCount", 0);
    buzzerEnabled = preferences.getBool("buzzerEnabled", true);
    ledEnabled = preferences.getBool("ledEnabled", true);
    
    targetFilters.clear();
    
    // Load saved filters (no defaults - start empty)
    if (filterCount > 0) {
        for (int i = 0; i < filterCount; i++) {
            String keyId   = "id_"   + String(i);
            String keyMAC  = "mac_"  + String(i);
            String keyDesc = "desc_" + String(i);
            String keyType = "type_" + String(i);

            TargetFilter filter;
            filter.identifier  = preferences.getString(keyId.c_str(), "");
            filter.isFullMAC   = preferences.getBool  (keyMAC.c_str(), false);
            filter.description = preferences.getString(keyDesc.c_str(), "");

            // Legacy compat: if `type_i` is missing (255 sentinel), derive
            // from the old boolean — old configs are all MAC-based.
            uint8_t rawType = preferences.getUChar(keyType.c_str(), 0xFF);
            if (rawType == 0xFF) {
                filter.type = filter.isFullMAC ? FT_FULL_MAC : FT_MAC_PREFIX;
            } else if (rawType <= FT_META_COMPOSITE) {
                filter.type = (FilterType)rawType;
            } else {
                filter.type = FT_MAC_PREFIX;  // corrupt value, fail safe
            }

            if (filter.identifier.length() > 0) {
                targetFilters.push_back(filter);
            }
        }
    }
    // No default values - form starts empty (placeholder examples remain in HTML)
    
    preferences.end();
}

void loadWiFiCredentials() {
    preferences.begin("ouispy", true);
    AP_SSID = preferences.getString("ap_ssid", "snoopuntothem");
    AP_PASSWORD = preferences.getString("ap_password", "astheysnoopuntous");
    preferences.end();
}

void saveWiFiCredentials() {
    preferences.begin("ouispy", false);
    preferences.putString("ap_ssid", AP_SSID);
    preferences.putString("ap_password", AP_PASSWORD);
    preferences.end();
}

// ================================
// MAC Address Utility Functions
// ================================
void normalizeMACAddress(String& mac) {
    mac.toLowerCase();
    mac.replace("-", ":");
    mac.replace(" ", "");
}

bool isValidMAC(const String& mac) {
    String normalized = mac;
    normalizeMACAddress(normalized);
    
    // Check for valid OUI (8 chars) or full MAC (17 chars)
    if (normalized.length() != 8 && normalized.length() != 17) {
        return false;
    }
    
    // Basic format validation
    for (int i = 0; i < normalized.length(); i++) {
        char c = normalized.charAt(i);
        if (i % 3 == 2) {
            if (c != ':') return false;
        } else {
            if (!isxdigit(c)) return false;
        }
    }
    
    return true;
}

// Case-insensitive substring for FT_NAME_SUBSTRING.
static bool nameContains(const String& haystack, const String& needle) {
    if (needle.length() == 0 || haystack.length() < needle.length()) return false;
    String h = haystack; h.toLowerCase();
    String n = needle;   n.toLowerCase();
    return h.indexOf(n) >= 0;
}

// Normalise a hex-string identifier ("0x0D53", "0d53", "0D 53") to a
// bare lowercase hex string so string compares work.
static String normalizeHexId(const String& in) {
    String s = in;
    s.toLowerCase();
    if (s.startsWith("0x")) s = s.substring(2);
    String out;
    out.reserve(s.length());
    for (size_t i = 0; i < s.length(); i++) {
        char c = s[i];
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) out += c;
    }
    return out;
}

// Forward declaration — defined below; FT_META_COMPOSITE filters defer to it.
bool matchesMetaComposite(NimBLEAdvertisedDevice* dev, const char*& outLabel);

// Second-pass classifier for the DeviceInfo row / dashboard badge. We keep
// matchesTargetFilter()'s signature untouched (upstream code depends on it),
// so this helper replays the same rules to recover which filter class hit
// and the raw identifier. Called only on NEW devices in the BLE callback,
// so the redundant pass is not on the hot per-advertisement path.
static bool resolveMatchedFilterMeta(NimBLEAdvertisedDevice* dev,
                                     const String& deviceMAC,
                                     FilterType& outType,
                                     String& outIdent) {
    String norm = deviceMAC; normalizeMACAddress(norm);
    for (const TargetFilter& f : targetFilters) {
        switch (f.type) {
            case FT_MAC_PREFIX: {
                String id = f.identifier; normalizeMACAddress(id);
                if (norm.startsWith(id)) { outType = f.type; outIdent = f.identifier; return true; }
                break;
            }
            case FT_FULL_MAC: {
                String id = f.identifier; normalizeMACAddress(id);
                if (norm.equals(id)) { outType = f.type; outIdent = f.identifier; return true; }
                break;
            }
            case FT_COMPANY_ID: {
                if (!dev || !dev->haveManufacturerData()) break;
                std::string mfr = dev->getManufacturerData();
                if (mfr.length() < 2) break;
                uint16_t cid = (uint8_t)mfr[0] | ((uint8_t)mfr[1] << 8);
                char cidHex[5]; snprintf(cidHex, sizeof(cidHex), "%04x", cid);
                if (normalizeHexId(f.identifier).equals(cidHex)) {
                    outType = f.type; outIdent = f.identifier; return true;
                }
                break;
            }
            case FT_SERVICE_UUID_16: {
                if (!dev) break;
                String target = normalizeHexId(f.identifier);
                for (int i = 0; i < dev->getServiceUUIDCount(); i++) {
                    NimBLEUUID uuid = dev->getServiceUUID(i);
                    String s = uuid.toString().c_str(); s.toLowerCase();
                    if ((s.length() == 4 && s.equals(target)) ||
                        (s.length() >= 8 && s.substring(4, 8).equals(target))) {
                        outType = f.type; outIdent = f.identifier; return true;
                    }
                }
                break;
            }
            case FT_NAME_SUBSTRING: {
                if (!dev || !dev->haveName()) break;
                String name = dev->getName().c_str();
                if (nameContains(name, f.identifier)) {
                    outType = f.type; outIdent = f.identifier; return true;
                }
                break;
            }
            case FT_META_COMPOSITE: {
                const char* metaLabel = nullptr;
                if (matchesMetaComposite(dev, metaLabel)) {
                    outType = f.type; outIdent = f.identifier; return true;
                }
                break;
            }
        }
    }
    return false;
}

bool matchesTargetFilter(NimBLEAdvertisedDevice* dev, const String& deviceMAC,
                          String& matchedDescription) {
    String normalizedDeviceMAC = deviceMAC;
    normalizeMACAddress(normalizedDeviceMAC);

    for (const TargetFilter& filter : targetFilters) {
        switch (filter.type) {
            case FT_MAC_PREFIX: {
                String filterID = filter.identifier;
                normalizeMACAddress(filterID);
                if (normalizedDeviceMAC.startsWith(filterID)) {
                    matchedDescription = filter.description;
                    return true;
                }
                break;
            }
            case FT_FULL_MAC: {
                String filterID = filter.identifier;
                normalizeMACAddress(filterID);
                if (normalizedDeviceMAC.equals(filterID)) {
                    matchedDescription = filter.description;
                    return true;
                }
                break;
            }
            case FT_COMPANY_ID: {
                if (!dev || !dev->haveManufacturerData()) break;
                std::string mfr = dev->getManufacturerData();
                if (mfr.length() < 2) break;
                // BLE mfr data prefixes CID little-endian: byte0=LSB, byte1=MSB.
                uint16_t cid = (uint8_t)mfr[0] | ((uint8_t)mfr[1] << 8);
                char cidHex[5];
                snprintf(cidHex, sizeof(cidHex), "%04x", cid);
                if (normalizeHexId(filter.identifier).equals(cidHex)) {
                    matchedDescription = filter.description;
                    return true;
                }
                break;
            }
            case FT_SERVICE_UUID_16: {
                if (!dev) break;
                String target = normalizeHexId(filter.identifier);
                for (int i = 0; i < dev->getServiceUUIDCount(); i++) {
                    NimBLEUUID uuid = dev->getServiceUUID(i);
                    // 16-bit UUIDs come back as short strings like "fd5f".
                    // 128-bit UUIDs come back as "0000fd5f-0000-1000-...":
                    // BT SIG 16-bit UUIDs live in bytes 2-3 of the base UUID.
                    String s = uuid.toString().c_str();
                    s.toLowerCase();
                    if (s.length() == 4 && s.equals(target)) {
                        matchedDescription = filter.description;
                        return true;
                    }
                    if (s.length() >= 8 && s.substring(4, 8).equals(target)) {
                        matchedDescription = filter.description;
                        return true;
                    }
                }
                break;
            }
            case FT_NAME_SUBSTRING: {
                if (!dev || !dev->haveName()) break;
                String name = dev->getName().c_str();
                if (nameContains(name, filter.identifier)) {
                    matchedDescription = filter.description;
                    return true;
                }
                break;
            }
            case FT_META_COMPOSITE: {
                // Installed via the META preset; defer to the composite
                // matcher. The label it returns ("META-RAYBAN (mfr+svc)" /
                // "META-RAYBAN (name)") is more informative than the filter
                // description, so it wins.
                const char* metaLabel = nullptr;
                if (matchesMetaComposite(dev, metaLabel)) {
                    matchedDescription = metaLabel;
                    return true;
                }
                break;
            }
        }
    }
    return false;
}

// Meta / Ray-Ban composite matcher.
//
// Only runs while a FT_META_COMPOSITE filter is installed (via the META
// preset in the OUI Database) — matchesTargetFilter calls it from that
// case. Does NOT use OUI signals: Meta glasses use RPA (rotating random
// MACs per BT spec), so OUI-based detection is pure noise, and CID-alone
// or UUID-alone filters were false-positive magnets (0xFD5F is advertised
// by phones running Meta apps). This matcher fires when BOTH conditions in
// condition A are present in the same advert, OR when condition B fires:
//   A. mfr data starts with company ID 0x0D53 (Luxottica, little-endian
//      0x53 0x0D) AND service UUID list contains 0xFD5F (Meta).
//   B. complete local name contains "Ray-Ban" / "Wayfarer" / "Oakley Meta"
//      (case-insensitive substring).
//
// A user manually installing 0x0D53, 0xFD5F, or a Luxottica MAC via the
// target config UI still triggers via the normal single-signature cases in
// matchesTargetFilter, with that filter's badge.
bool matchesMetaComposite(NimBLEAdvertisedDevice* dev, const char*& outLabel) {
    outLabel = nullptr;
    if (!dev) return false;

    bool haveLuxottica = false;
    if (dev->haveManufacturerData()) {
        std::string mfr = dev->getManufacturerData();
        if (mfr.length() >= 2 && (uint8_t)mfr[0] == 0x53 && (uint8_t)mfr[1] == 0x0D) {
            haveLuxottica = true;
        }
    }
    if (haveLuxottica) {
        for (int i = 0; i < dev->getServiceUUIDCount(); i++) {
            NimBLEUUID uuid = dev->getServiceUUID(i);
            String s = uuid.toString().c_str();
            s.toLowerCase();
            bool short16 = (s.length() == 4 && s.equals("fd5f"));
            bool long128 = (s.length() >= 8 && s.substring(4, 8).equals("fd5f"));
            if (short16 || long128) {
                outLabel = "META-RAYBAN (mfr+svc)";
                return true;
            }
        }
    }

    if (dev->haveName()) {
        String name = dev->getName().c_str();
        if (nameContains(name, "Ray-Ban") ||
            nameContains(name, "Wayfarer") ||
            nameContains(name, "Oakley Meta")) {
            outLabel = "META-RAYBAN (name)";
            return true;
        }
    }

    return false;
}

// ================================================================
// BLE detection session subsystem
// ================================================================
//
// Mirrors Mode 3 (flock-you WiFi)'s persistence + protocol shape so the same
// Flask dashboard can pull BLE detections. Kept below matchesTargetFilter()
// so the classifier here can re-use its normalisation helpers.
//
// Threading model: everything below is called from loop() only. onResult()
// (BLE scan-task callback) delegates to bleNoteDetection() via the atomic
// `newMatchFound` flag path that already exists — so we never touch this
// table from an ISR.
// ================================================================

// Re-classify a MAC/device that matchesTargetFilter() has already accepted.
// We can't get the method from matchesTargetFilter() (task says preserve it
// verbatim), so we replay the same rules and record which one hit. Returns
// the method label string; also fills in the company_id / service_uuid it
// carried, when available.
static const char* bleClassifyMatch(NimBLEAdvertisedDevice* dev,
                                    const String& deviceMAC,
                                    uint16_t& outCompanyId,
                                    uint16_t& outServiceUuid) {
    outCompanyId   = 0xFFFF;
    outServiceUuid = 0x0000;

    String normDev = deviceMAC;
    normalizeMACAddress(normDev);

    for (const TargetFilter& filter : targetFilters) {
        switch (filter.type) {
            case FT_FULL_MAC: {
                String id = filter.identifier;
                normalizeMACAddress(id);
                if (normDev.equals(id)) return BLE_MM_FULL_MAC;
                break;
            }
            case FT_MAC_PREFIX: {
                String id = filter.identifier;
                normalizeMACAddress(id);
                if (normDev.startsWith(id)) return BLE_MM_OUI_PREFIX;
                break;
            }
            case FT_COMPANY_ID: {
                if (!dev || !dev->haveManufacturerData()) break;
                std::string mfr = dev->getManufacturerData();
                if (mfr.length() < 2) break;
                uint16_t cid = (uint8_t)mfr[0] | ((uint8_t)mfr[1] << 8);
                char cidHex[5];
                snprintf(cidHex, sizeof(cidHex), "%04x", cid);
                if (normalizeHexId(filter.identifier).equals(cidHex)) {
                    outCompanyId = cid;
                    return BLE_MM_COMPANY_ID;
                }
                break;
            }
            case FT_SERVICE_UUID_16: {
                if (!dev) break;
                String target = normalizeHexId(filter.identifier);
                for (int i = 0; i < dev->getServiceUUIDCount(); i++) {
                    NimBLEUUID uuid = dev->getServiceUUID(i);
                    String s = uuid.toString().c_str();
                    s.toLowerCase();
                    if ((s.length() == 4 && s.equals(target)) ||
                        (s.length() >= 8 && s.substring(4, 8).equals(target))) {
                        outServiceUuid = (uint16_t)strtoul(target.c_str(), nullptr, 16);
                        return BLE_MM_SERVICE_UUID;
                    }
                }
                break;
            }
            case FT_NAME_SUBSTRING: {
                if (!dev || !dev->haveName()) break;
                String name = dev->getName().c_str();
                if (nameContains(name, filter.identifier)) return BLE_MM_NAME_SUBSTRING;
                break;
            }
            case FT_META_COMPOSITE: {
                const char* metaLabel = nullptr;
                if (matchesMetaComposite(dev, metaLabel)) return BLE_MM_META_COMPOSITE;
                break;
            }
        }
    }
    return BLE_MM_UNKNOWN;
}

// Return the "public"/"random_static"/"rpa"/"nrpa" label for a NimBLE addr.
// For random addresses the top two bits of the first octet distinguish
// static-random (11), resolvable private (10), and non-resolvable private (00).
static const char* bleAddrTypeLabel(uint8_t nimbleType, const String& mac) {
    if (nimbleType == 0 /*BLE_ADDR_PUBLIC*/) return BLE_ADDR_LABEL_PUBLIC;
    // Random of any flavour — decode top 2 bits of MSB octet.
    int colon = mac.indexOf(':');
    if (colon <= 0) return BLE_ADDR_LABEL_RANDOM;
    String hex = mac.substring(0, colon);
    if (hex.length() < 2) return BLE_ADDR_LABEL_RANDOM;
    uint8_t b0 = (uint8_t)strtoul(hex.c_str(), nullptr, 16);
    uint8_t top = (b0 >> 6) & 0x03;
    if (top == 0b11) return BLE_ADDR_LABEL_RANDOM;  // static random
    if (top == 0b10) return BLE_ADDR_LABEL_RPA;     // resolvable
    if (top == 0b00) return BLE_ADDR_LABEL_NRPA;    // non-resolvable
    return BLE_ADDR_LABEL_RANDOM;                    // reserved encoding
}

// ---------- JSON escape (SSID / device name is user-controlled) ----------

static size_t bleJsonEscape(char* dst, size_t cap, const char* src) {
    size_t o = 0;
    if (cap == 0 || !src) return 0;
    for (size_t i = 0; src[i]; i++) {
        char c = src[i];
        if (c == '"' || c == '\\') {
            if (o + 2 >= cap) break;
            dst[o++] = '\\'; dst[o++] = c;
        } else if ((unsigned char)c < 0x20) {
            if (o + 6 >= cap) break;
            int n = snprintf(dst + o, cap - o, "\\u%04x", (unsigned)(unsigned char)c);
            if (n <= 0 || (size_t)n >= cap - o) break;
            o += (size_t)n;
        } else {
            if (o + 1 >= cap) break;
            dst[o++] = c;
        }
    }
    dst[o] = '\0';
    return o;
}

// ---------- CRC32 (zlib polynomial, matches Mode 3's fyCRC32) ----------

static uint32_t bleCRC32Update(uint32_t crc, const uint8_t* buf, size_t len) {
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & -(int32_t)(crc & 1));
        }
    }
    return ~crc;
}

// ---------- Detection serialisation ----------
//
// Line schema matches the WiFi-side "detection" line the flock-you dashboard
// already ingests, minus WiFi-specific fields (channel/frequency/tier) and
// plus BLE-specific ones (addr_type, company_id, service_uuid, local_name,
// match_method, matched_signature). `protocol` is "ble" and `event` is
// "detection" so the same JSON-per-line parser picks it up.

static size_t bleSerializeDet(const BLEDetection& d, char* dst, size_t cap,
                              const char* replay_source) {
    char nameEsc[sizeof(d.localName) * 6 + 1];
    char sigEsc[sizeof(d.matchedSig) * 6 + 1];
    bleJsonEscape(nameEsc, sizeof(nameEsc), d.localName);
    bleJsonEscape(sigEsc,  sizeof(sigEsc),  d.matchedSig);

    // Company ID / service UUID are omitted as fields (rendered as null) when
    // sentinel — dashboards can distinguish "not-applicable" from a real 0.
    char cidStr[8]; char svcStr[8];
    if (d.companyId  == 0xFFFF) snprintf(cidStr, sizeof(cidStr), "null");
    else                        snprintf(cidStr, sizeof(cidStr), "%u", (unsigned)d.companyId);
    if (d.serviceUuid == 0x0000) snprintf(svcStr, sizeof(svcStr), "null");
    else                         snprintf(svcStr, sizeof(svcStr), "%u", (unsigned)d.serviceUuid);

    int n = snprintf(dst, cap,
        "{\"event\":\"detection\","
        "\"protocol\":\"ble\","
        "\"detection_method\":\"ble_%s\","
        "\"mac_address\":\"%s\","
        "\"addr_type\":\"%s\","
        "\"rssi\":%d,"
        "\"rssi_min\":%d,"
        "\"rssi_max\":%d,"
        "\"company_id\":%s,"
        "\"service_uuid\":%s,"
        "\"local_name\":\"%s\","
        "\"device_name\":\"%s\","
        "\"match_method\":\"%s\","
        "\"matched_signature\":\"%s\","
        "\"first_seen_ms\":%lu,"
        "\"last_seen_ms\":%lu,"
        "\"hit_count\":%u%s%s%s}",
        d.matchMethod,
        d.mac,
        d.addrType,
        (int)d.rssiMax,
        (int)d.rssiMin,
        (int)d.rssiMax,
        cidStr,
        svcStr,
        nameEsc,
        nameEsc,
        d.matchMethod,
        sigEsc,
        (unsigned long)d.firstSeen,
        (unsigned long)d.lastSeen,
        (unsigned)d.hitCount,
        replay_source ? ",\"replay_source\":\"" : "",
        replay_source ? replay_source           : "",
        replay_source ? "\""                    : "");
    return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

// ---------- Detection table update ----------
//
// Called from bleNoteDetection() with a MAC that has already matched the
// active target filters. Returns true iff this call created a NEW row (used
// to gate the live JSON emission — mirrors Mode 3's chirp-worthy dedup so
// the RE-3s / RE-30s re-hit alerts don't spam the dashboard).

static bool bleTableUpsert(const char* mac, const char* addrType,
                           const char* method, const char* matchedSig,
                           const char* localName, uint16_t cid, uint16_t svc,
                           int8_t rssi, BLEDetection** outRow) {
    uint32_t now = millis();
    for (int i = 0; i < bleDetCount; i++) {
        if (strcasecmp(bleDet[i].mac, mac) == 0) {
            BLEDetection& d = bleDet[i];
            if (d.hitCount < 0xFFFF) d.hitCount++;
            d.lastSeen = now;
            if (rssi < d.rssiMin) d.rssiMin = rssi;
            if (rssi > d.rssiMax) d.rssiMax = rssi;
            if (localName && localName[0] && !d.localName[0]) {
                strlcpy(d.localName, localName, sizeof(d.localName));
            }
            if (cid != 0xFFFF && d.companyId == 0xFFFF)  d.companyId  = cid;
            if (svc != 0x0000 && d.serviceUuid == 0x0000) d.serviceUuid = svc;
            bleSessionDirty = true;
            if (outRow) *outRow = &d;
            return false;
        }
    }
    if (bleDetCount >= BLE_MAX_DETECTIONS) {
        // LRU-drop the oldest row (smallest lastSeen).
        int oldest = 0;
        for (int i = 1; i < bleDetCount; i++) {
            if (bleDet[i].lastSeen < bleDet[oldest].lastSeen) oldest = i;
        }
        // Shift down the tail to keep the array packed.
        for (int i = oldest; i < bleDetCount - 1; i++) bleDet[i] = bleDet[i + 1];
        bleDetCount--;
    }
    BLEDetection& d = bleDet[bleDetCount];
    memset(&d, 0, sizeof(d));
    strlcpy(d.mac,          mac,        sizeof(d.mac));
    strlcpy(d.addrType,     addrType,   sizeof(d.addrType));
    strlcpy(d.matchMethod,  method,     sizeof(d.matchMethod));
    strlcpy(d.matchedSig,   matchedSig ? matchedSig : "", sizeof(d.matchedSig));
    if (localName) strlcpy(d.localName, localName, sizeof(d.localName));
    d.companyId   = cid;
    d.serviceUuid = svc;
    d.rssiMin     = rssi;
    d.rssiMax     = rssi;
    d.firstSeen   = now;
    d.lastSeen    = now;
    d.hitCount    = 1;
    bleDetCount++;
    bleSessionDirty = true;
    if (outRow) *outRow = &d;
    return true;
}

// ---------- Envelope pass 1: compute payload bytes + CRC ----------

static uint32_t bleComputePayloadCRC(size_t& outBytes) {
    char line[512];
    uint32_t crc = 0;
    outBytes = 0;
    crc = bleCRC32Update(crc, (const uint8_t*)"[", 1); outBytes += 1;
    for (int i = 0; i < bleDetCount; i++) {
        if (i > 0) { crc = bleCRC32Update(crc, (const uint8_t*)",", 1); outBytes += 1; }
        size_t n = bleSerializeDet(bleDet[i], line, sizeof(line), nullptr);
        if (n == 0) continue;
        crc = bleCRC32Update(crc, (const uint8_t*)line, n);
        outBytes += n;
    }
    crc = bleCRC32Update(crc, (const uint8_t*)"]", 1); outBytes += 1;
    return crc;
}

// ---------- Envelope parse / validate / promote ----------

static bool bleParseEnvelope(const char* hdr, size_t& outBytes, uint32_t& outCrc) {
    const char* b = strstr(hdr, "\"bytes\":");
    const char* c = strstr(hdr, "\"crc\":\"0x");
    if (!b || !c) return false;
    long long bv = 0;
    if (sscanf(b + 8, "%lld", &bv) != 1 || bv < 0) return false;
    unsigned cv = 0;
    if (sscanf(c + 9, "%x", &cv) != 1) return false;
    outBytes = (size_t)bv;
    outCrc   = (uint32_t)cv;
    return true;
}

static bool bleValidateSessionFile(const char* path) {
    if (!SPIFFS.exists(path)) return false;
    File f = SPIFFS.open(path, "r");
    if (!f) return false;
    String hdr = f.readStringUntil('\n');
    if (hdr.length() < 10 || hdr[0] != '{') { f.close(); return false; }
    size_t   expBytes = 0;
    uint32_t expCRC   = 0;
    if (!bleParseEnvelope(hdr.c_str(), expBytes, expCRC)) { f.close(); return false; }
    size_t bodyOff = hdr.length() + 1;
    size_t fSize   = f.size();
    if (fSize < bodyOff + expBytes || (fSize - bodyOff) != expBytes) {
        f.close(); return false;
    }
    uint8_t buf[256];
    uint32_t crc = 0;
    size_t remaining = expBytes;
    while (remaining > 0) {
        int n = f.read(buf, remaining < sizeof(buf) ? remaining : sizeof(buf));
        if (n <= 0) break;
        crc = bleCRC32Update(crc, buf, (size_t)n);
        remaining -= (size_t)n;
    }
    f.close();
    return (remaining == 0 && crc == expCRC);
}

static bool bleSpiffsCopy(const char* src, const char* dst) {
    File s = SPIFFS.open(src, "r");
    if (!s) return false;
    File d = SPIFFS.open(dst, "w");
    if (!d) { s.close(); return false; }
    uint8_t buf[256];
    int n; bool ok = true;
    while ((n = s.read(buf, sizeof(buf))) > 0) {
        if (d.write(buf, (size_t)n) != (size_t)n) { ok = false; break; }
    }
    s.close(); d.close();
    return ok;
}

static bool bleAtomicPromote(const char* src, const char* dst) {
    if (SPIFFS.rename(src, dst)) return true;
    if (!bleSpiffsCopy(src, dst)) return false;
    SPIFFS.remove(src);
    return true;
}

static void bleRefreshPrevMeta() {
    blePrevExists = SPIFFS.exists(BLE_PREV_FILE);
    if (blePrevExists) {
        File f = SPIFFS.open(BLE_PREV_FILE, "r");
        blePrevBytes = f ? f.size() : 0;
        if (f) f.close();
    } else {
        blePrevBytes = 0;
    }
}

// ---------- Save the current in-RAM table to SPIFFS ----------

static void bleSaveSession() {
    if (!bleSpiffsReady) return;
    if (!bleSessionDirty && bleDetCount == bleLastSaveCount) return;

    size_t   payloadBytes = 0;
    uint32_t crc          = bleComputePayloadCRC(payloadBytes);
    int      savedCount   = bleDetCount;

    File f = SPIFFS.open(BLE_SESSION_TMP, "w");
    if (!f) {
        Serial.printf("[ble_session] save failed: cannot open %s\n", BLE_SESSION_TMP);
        return;
    }
    f.printf("{\"v\":1,\"count\":%d,\"bytes\":%u,\"crc\":\"0x%08lX\"}\n",
             savedCount, (unsigned)payloadBytes, (unsigned long)crc);

    char line[512];
    size_t wrote = 0;
    f.write((uint8_t*)"[", 1); wrote++;
    for (int i = 0; i < bleDetCount; i++) {
        if (i > 0) { f.write((uint8_t*)",", 1); wrote++; }
        size_t n = bleSerializeDet(bleDet[i], line, sizeof(line), nullptr);
        if (n == 0) continue;
        f.write((uint8_t*)line, n);
        wrote += n;
    }
    f.write((uint8_t*)"]", 1); wrote++;
    f.close();

    if (wrote != payloadBytes) {
        Serial.printf("[ble_session] save WARN: wrote %u expected %u — aborting\n",
                      (unsigned)wrote, (unsigned)payloadBytes);
        return;
    }
    if (!bleValidateSessionFile(BLE_SESSION_TMP)) {
        Serial.println("[ble_session] save verify FAILED — old session preserved");
        return;
    }

    SPIFFS.remove(BLE_SESSION_FILE);
    if (!bleAtomicPromote(BLE_SESSION_TMP, BLE_SESSION_FILE)) {
        Serial.printf("[ble_session] promote FAILED — data in %s for recovery\n",
                      BLE_SESSION_TMP);
        return;
    }

    bleLastSaveAt    = millis();
    bleLastSaveCount = savedCount;
    bleSessionDirty  = false;
}

// ---------- Boot: promote any prior session into /ble_prev_session.json ----------

static void blePromotePrevSession() {
    if (!bleSpiffsReady) return;
    const char* source = nullptr;
    if      (bleValidateSessionFile(BLE_SESSION_FILE)) source = BLE_SESSION_FILE;
    else if (bleValidateSessionFile(BLE_SESSION_TMP))  source = BLE_SESSION_TMP;

    if (!source) {
        if (SPIFFS.exists(BLE_SESSION_FILE)) SPIFFS.remove(BLE_SESSION_FILE);
        if (SPIFFS.exists(BLE_SESSION_TMP))  SPIFFS.remove(BLE_SESSION_TMP);
        bleRefreshPrevMeta();
        return;
    }
    if (!bleSpiffsCopy(source, BLE_PREV_FILE)) {
        Serial.printf("[ble_session] promote failed: %s -> %s\n", source, BLE_PREV_FILE);
        return;
    }
    if (SPIFFS.exists(BLE_SESSION_FILE)) SPIFFS.remove(BLE_SESSION_FILE);
    if (SPIFFS.exists(BLE_SESSION_TMP))  SPIFFS.remove(BLE_SESSION_TMP);
    bleRefreshPrevMeta();
}

static void bleSessionSetup() {
    if (SPIFFS.begin(true)) {
        bleSpiffsReady = true;
        Serial.println("[ble_session] SPIFFS ready");
        blePromotePrevSession();
    } else {
        Serial.println("[ble_session] SPIFFS init FAILED — running without persistence");
        bleSpiffsReady = false;
    }
}

static void bleAutosaveTick() {
    if (!bleSpiffsReady || !bleSessionDirty) return;
    if (millis() - bleLastSaveAt < BLE_AUTOSAVE_MS) return;
    bleSaveSession();
}

// ---------- Live-detection hook (called from onResult path) ----------
//
// The scan callback still runs its beep/flash/on-device-table logic verbatim.
// This function is called in ADDITION to that path for every hit; internally
// it dedupes new-row vs re-hit to gate the live JSON emit so that repeat
// sightings don't spam the dashboard (mirrors Mode 3's chirp-worthy filter).

static void bleNoteDetection(NimBLEAdvertisedDevice* dev, const String& mac,
                             int rssi, const String& matchedSig) {
    if (!dev) return;

    uint16_t cid = 0xFFFF, svc = 0x0000;
    const char* method = bleClassifyMatch(dev, mac, cid, svc);
    const char* addrTy = bleAddrTypeLabel(dev->getAddressType(), mac);

    String nameStr;
    if (dev->haveName()) nameStr = dev->getName().c_str();
    char nameBuf[32];
    strlcpy(nameBuf, nameStr.c_str(), sizeof(nameBuf));

    char macBuf[18];
    strlcpy(macBuf, mac.c_str(), sizeof(macBuf));
    // Force lowercase for a canonical key.
    for (char* p = macBuf; *p; p++) *p = tolower(*p);

    BLEDetection* row = nullptr;
    bool created = bleTableUpsert(macBuf, addrTy, method, matchedSig.c_str(),
                                  nameBuf, cid, svc, (int8_t)rssi, &row);

    // Emit the live JSON line only on the FIRST sighting so the dashboard
    // doesn't get spammed by re-hits — matches Mode 3's dedup contract.
    if (created && row && isSerialConnected()) {
        char line[600];
        size_t n = bleSerializeDet(*row, line, sizeof(line), "live");
        if (n > 0) {
            Serial.write((const uint8_t*)line, n);
            Serial.print('\n');
        }
    }
}

// ---------- CMD: serial protocol (mirrors Mode 3 line-wrap contract) ----------
//
// Streams are wrapped BEGIN/END so the Flask dashboard's line reader can
// tell exactly where a dump starts and ends. Payload lines carry the exact
// same JSON schema the live path emits (with `replay_source` set to
// "flash" or "ram" instead of "live").

static void bleReplyOk()             { Serial.println(F("OK")); }
static void bleReplyErr(const char* m){ Serial.print(F("ERR ")); Serial.println(m); }

static void bleDumpPrev() {
    if (!bleSpiffsReady) { bleReplyErr("spiffs not ready"); return; }
    if (!SPIFFS.exists(BLE_PREV_FILE)) {
        Serial.println(F("BEGIN_DUMP prev bytes=0 count=0"));
        Serial.println(F("END_DUMP prev count=0"));
        return;
    }
    File f = SPIFFS.open(BLE_PREV_FILE, "r");
    if (!f) { bleReplyErr("open prev failed"); return; }
    String hdr = f.readStringUntil('\n');
    size_t bytes = 0; uint32_t crc = 0;
    bleParseEnvelope(hdr.c_str(), bytes, crc);
    // Count entries = number of top-level '{' in the payload.
    // We stream the payload as-is (verbatim), but split into one JSON object
    // per line so the dashboard's line reader can ingest without a stream
    // parser. Each object gets `"replay_source":"flash"` injected before
    // the closing brace.
    String body;
    body.reserve(bytes + 16);
    while (f.available()) body += (char)f.read();
    f.close();

    // Strip surrounding [ ]
    int start = body.indexOf('[');
    int end   = body.lastIndexOf(']');
    if (start < 0 || end < 0 || end <= start) {
        bleReplyErr("prev payload malformed");
        return;
    }
    body = body.substring(start + 1, end);

    // Split by top-level commas (assume no ',' inside strings for simplicity —
    // our serializer never emits one; user-controlled fields are JSON-escaped
    // so ',' can appear but only inside a string. We track brace depth to be
    // safe on both counts.)
    std::vector<String> objs;
    int depth = 0; int objStart = -1;
    for (int i = 0; i < (int)body.length(); i++) {
        char c = body[i];
        if (c == '{') { if (depth == 0) objStart = i; depth++; }
        else if (c == '}') {
            depth--;
            if (depth == 0 && objStart >= 0) {
                objs.push_back(body.substring(objStart, i + 1));
                objStart = -1;
            }
        }
    }

    Serial.printf("BEGIN_DUMP prev bytes=%u count=%u\n",
                  (unsigned)bytes, (unsigned)objs.size());
    for (size_t i = 0; i < objs.size(); i++) {
        String s = objs[i];
        // Inject `,"replay_source":"flash"` before the closing '}'
        int close = s.lastIndexOf('}');
        if (close > 0) {
            s = s.substring(0, close) + ",\"replay_source\":\"flash\"}";
        }
        Serial.println(s);
    }
    Serial.printf("END_DUMP prev count=%u\n", (unsigned)objs.size());
}

static void bleDumpLive() {
    Serial.printf("BEGIN_DUMP live bytes=0 count=%u\n", (unsigned)bleDetCount);
    char line[600];
    for (int i = 0; i < bleDetCount; i++) {
        size_t n = bleSerializeDet(bleDet[i], line, sizeof(line), "ram");
        if (n > 0) {
            Serial.write((const uint8_t*)line, n);
            Serial.print('\n');
        }
    }
    Serial.printf("END_DUMP live count=%u\n", (unsigned)bleDetCount);
}

static void bleCmdStatus() {
    Serial.printf(
        "{\"mode\":\"ble_detector\",\"fw\":\"%s\",\"uptime_s\":%lu,"
        "\"live_count\":%u,\"prev_exists\":%s,\"prev_bytes\":%u,"
        "\"heap\":%u}\n",
        DETECTOR_FW_VERSION,
        (unsigned long)(millis() / 1000UL),
        (unsigned)bleDetCount,
        blePrevExists ? "true" : "false",
        (unsigned)blePrevBytes,
        (unsigned)ESP.getFreeHeap());
}

static void bleCmdVersion() {
    Serial.printf("OUI-SPY BLE DETECTOR %s built %s %s\n",
                  DETECTOR_FW_VERSION, __DATE__, __TIME__);
}

static void bleCmdClearPrev() {
    if (!bleSpiffsReady) { bleReplyErr("spiffs not ready"); return; }
    if (SPIFFS.exists(BLE_PREV_FILE)) SPIFFS.remove(BLE_PREV_FILE);
    bleRefreshPrevMeta();
    bleReplyOk();
}

static void bleCmdClearLive() {
    bleDetCount     = 0;
    bleSessionDirty = true;
    bleReplyOk();
}

static void bleHandleCmdLine(const String& raw) {
    String line = raw; line.trim();
    if (!line.startsWith("CMD:") && !line.startsWith("cmd:")) return;
    String body = line.substring(4); body.trim(); body.toUpperCase();

    if      (body == "DUMP_PREV")  { bleDumpPrev(); }
    else if (body == "DUMP_LIVE")  { bleDumpLive(); }
    else if (body == "CLEAR_PREV") { bleCmdClearPrev(); }
    else if (body == "CLEAR_LIVE") { bleCmdClearLive(); }
    else if (body == "STATUS")     { bleCmdStatus(); }
    else if (body == "VERSION")    { bleCmdVersion(); }
    else                           { bleReplyErr("unknown"); }
}

static void blePollSerialCmd() {
    while (Serial.available() > 0) {
        int c = Serial.read();
        if (c < 0) break;
        if (c == '\n' || c == '\r') {
            if (bleCmdLen > 0) {
                bleCmdBuf[bleCmdLen] = '\0';
                bleHandleCmdLine(String(bleCmdBuf));
                bleCmdLen = 0;
            }
            continue;
        }
        if (bleCmdLen < BLE_CMD_BUF_LEN - 1) {
            bleCmdBuf[bleCmdLen++] = (char)c;
        } else {
            bleCmdLen = 0;   // overflow — drop the whole line rather than truncate
        }
    }
}

// ---------- On-device web endpoints for the same operations ----------

static void bleRegisterWebEndpoints() {
    server.on("/api/session", HTTP_GET, [](AsyncWebServerRequest *request) {
        char body[192];
        snprintf(body, sizeof(body),
                 "{\"live_count\":%u,\"prev_exists\":%s,\"prev_bytes\":%u,\"heap_free\":%u}",
                 (unsigned)bleDetCount,
                 blePrevExists ? "true" : "false",
                 (unsigned)blePrevBytes,
                 (unsigned)ESP.getFreeHeap());
        request->send(200, "application/json", body);
    });

    server.on("/api/session/clear_prev", HTTP_POST, [](AsyncWebServerRequest *request) {
        if (bleSpiffsReady && SPIFFS.exists(BLE_PREV_FILE)) SPIFFS.remove(BLE_PREV_FILE);
        bleRefreshPrevMeta();
        request->send(200, "application/json", "{\"ok\":true}");
    });

    // Previous-session panel data source. Strips the envelope header line
    // from /ble_prev_session.json and returns the raw JSON payload array,
    // giving the dashboard the same shape as the standalone detector's
    // /api/session/previous. Empty array on any error / missing file.
    server.on("/api/session/previous", HTTP_GET, [](AsyncWebServerRequest *request) {
        if (!bleSpiffsReady || !SPIFFS.exists(BLE_PREV_FILE)) {
            request->send(200, "application/json", "[]");
            return;
        }
        File f = SPIFFS.open(BLE_PREV_FILE, "r");
        if (!f) { request->send(200, "application/json", "[]"); return; }
        f.readStringUntil('\n');  // discard envelope header
        String body;
        body.reserve(f.size());
        while (f.available()) body += (char)f.read();
        f.close();
        int lo = body.indexOf('[');
        int hi = body.lastIndexOf(']');
        if (lo < 0 || hi < 0 || hi <= lo) {
            request->send(200, "application/json", "[]");
            return;
        }
        request->send(200, "application/json", body.substring(lo, hi + 1));
    });

    // Symmetric alias for the standalone detector's endpoint name — the
    // same operation as /api/session/clear_prev, kept so a shared front-end
    // codebase can hit either firmware.
    server.on("/api/session/clear_previous", HTTP_POST, [](AsyncWebServerRequest *request) {
        if (bleSpiffsReady && SPIFFS.exists(BLE_PREV_FILE)) SPIFFS.remove(BLE_PREV_FILE);
        bleRefreshPrevMeta();
        request->send(200, "application/json", "{\"ok\":true}");
    });

    server.on("/api/session/clear_live", HTTP_POST, [](AsyncWebServerRequest *request) {
        bleDetCount     = 0;
        bleSessionDirty = true;
        request->send(200, "application/json", "{\"ok\":true}");
    });
}

// ================================
// Detection Presets
// ================================
//
// One-click "add every known signature for a device family" bundles.
// Signatures triple-sourced: Bluetooth SIG assigned-numbers registry
// (company IDs + 16-bit service UUIDs), IEEE OUI registry (MAC prefixes),
// and cross-referenced against the community friendorfoe project
// (lnxgod/friendorfoe/esp32/scanner/main/detection/ble_fingerprint.c)
// which ships the same discrimination logic on ESP32-S3 hardware.
//
// Adding a new preset: define the array, add a case to applyPreset().

struct PresetEntry {
    FilterType type;
    const char* identifier;
    const char* description;
};

// Meta / Ray-Ban smart glasses (Wayfarer, Headliner, Skyler, Oakley Meta).
// One synthetic composite signature rather than separate CID/UUID entries:
// the glasses use RPA (rotating random MAC per BT spec), so OUI matching is
// pure noise, and CID-alone or UUID-alone filters were false-positive
// magnets (0xFD5F is advertised by phones running Meta apps). The composite
// requires mfr CID 0x0D53 (Luxottica) AND svc UUID 0xFD5F (Meta) in the
// same advert, or a name-substring hit — see matchesMetaComposite().
static const PresetEntry PRESET_META[] = {
    { FT_META_COMPOSITE, "0x0D53+0xFD5F", "Composite: Luxottica CID + Meta svc UUID, or Ray-Ban/Wayfarer/Oakley Meta name" },
};
static const size_t PRESET_META_COUNT = sizeof(PRESET_META) / sizeof(PRESET_META[0]);

// Axon body cameras (Body 3/4, Fleet dash, Taser 7/10).
// Uses all three signal types: dedicated IEEE OUI 00:25:DF ("Axon
// Enterprise, Inc."), Bluetooth SIG company ID 0x034D ("TASER
// International, Inc." — Axon's earlier registered name), and service
// UUID 0xFC81 ("Axon Enterprise, Inc."). All three uniquely attributable.
static const PresetEntry PRESET_AXON[] = {
    { FT_MAC_PREFIX,      "0025DF", "Axon Enterprise OUI (IEEE)" },
    { FT_COMPANY_ID,      "034D",   "TASER International CID (Axon body cams)" },
    { FT_SERVICE_UUID_16, "FC81",   "Axon Enterprise service UUID" },
};
static const size_t PRESET_AXON_COUNT = sizeof(PRESET_AXON) / sizeof(PRESET_AXON[0]);

// Returns count added. Skips entries whose (type, identifier) already
// exists so repeated clicks don't duplicate rows.
int applyPreset(const PresetEntry* preset, size_t count, const char* labelPrefix) {
    int added = 0;
    for (size_t i = 0; i < count; i++) {
        const PresetEntry& p = preset[i];
        bool exists = false;
        for (const TargetFilter& f : targetFilters) {
            if (f.type == p.type && f.identifier.equalsIgnoreCase(p.identifier)) {
                exists = true;
                break;
            }
        }
        if (exists) continue;

        TargetFilter f;
        f.type        = p.type;
        f.identifier  = p.identifier;
        f.isFullMAC   = (p.type == FT_FULL_MAC);
        f.description = String(labelPrefix) + ": " + p.description;
        targetFilters.push_back(f);
        added++;
    }
    if (added > 0) saveConfiguration();
    return added;
}

// Remove the non-MAC signatures a preset installed. MAC prefixes are left
// alone — those live in the OUI textarea and are the user's to manage.
int removePreset(const PresetEntry* preset, size_t count) {
    int removed = 0;
    for (size_t i = 0; i < count; i++) {
        const PresetEntry& p = preset[i];
        if (p.type == FT_MAC_PREFIX || p.type == FT_FULL_MAC) continue;
        for (size_t j = 0; j < targetFilters.size(); ) {
            if (targetFilters[j].type == p.type &&
                targetFilters[j].identifier.equalsIgnoreCase(p.identifier)) {
                targetFilters.erase(targetFilters.begin() + j);
                removed++;
            } else {
                j++;
            }
        }
    }
    if (removed > 0) saveConfiguration();
    return removed;
}

// True if any of the preset's non-MAC signatures are currently installed.
// MAC prefixes are excluded on purpose: those live in the OUI textarea and
// may have been added manually, so they don't indicate preset state.
bool presetInstalled(const PresetEntry* preset, size_t count) {
    for (size_t i = 0; i < count; i++) {
        const PresetEntry& p = preset[i];
        if (p.type == FT_MAC_PREFIX || p.type == FT_FULL_MAC) continue;
        for (const TargetFilter& f : targetFilters) {
            if (f.type == p.type && f.identifier.equalsIgnoreCase(p.identifier)) return true;
        }
    }
    return false;
}

// ================================
// Device Alias Functions
// ================================
void saveDeviceAliases() {
    preferences.begin("ouispy", false);
    preferences.putInt("aliasCount", deviceAliases.size());
    
    for (int i = 0; i < deviceAliases.size(); i++) {
        String keyMac = "alias_mac_" + String(i);
        String keyName = "alias_name_" + String(i);
        
        preferences.putString(keyMac.c_str(), deviceAliases[i].macAddress);
        preferences.putString(keyName.c_str(), deviceAliases[i].alias);
    }
    
    preferences.end();
    
    if (isSerialConnected()) {
        Serial.println("Device aliases saved to NVS (" + String(deviceAliases.size()) + " aliases)");
    }
}

void loadDeviceAliases() {
    preferences.begin("ouispy", true);
    int aliasCount = preferences.getInt("aliasCount", 0);
    
    deviceAliases.clear();
    
    for (int i = 0; i < aliasCount; i++) {
        String keyMac = "alias_mac_" + String(i);
        String keyName = "alias_name_" + String(i);
        
        DeviceAlias alias;
        alias.macAddress = preferences.getString(keyMac.c_str(), "");
        alias.alias = preferences.getString(keyName.c_str(), "");
        
        if (alias.macAddress.length() > 0 && alias.alias.length() > 0) {
            deviceAliases.push_back(alias);
        }
    }
    
    preferences.end();
    
    if (isSerialConnected()) {
        Serial.println("Device aliases loaded from NVS (" + String(deviceAliases.size()) + " aliases)");
    }
}

String getDeviceAlias(const String& macAddress) {
    String normalizedMAC = macAddress;
    normalizeMACAddress(normalizedMAC);
    
    for (const DeviceAlias& alias : deviceAliases) {
        String normalizedAliasMAC = alias.macAddress;
        normalizeMACAddress(normalizedAliasMAC);
        
        if (normalizedAliasMAC.equals(normalizedMAC)) {
            return alias.alias;
        }
    }
    
    return ""; // No alias found
}

void setDeviceAlias(const String& macAddress, const String& alias) {
    String normalizedMAC = macAddress;
    normalizeMACAddress(normalizedMAC);
    
    // Check if alias already exists, update it
    for (auto& deviceAlias : deviceAliases) {
        String normalizedAliasMAC = deviceAlias.macAddress;
        normalizeMACAddress(normalizedAliasMAC);
        
        if (normalizedAliasMAC.equals(normalizedMAC)) {
            if (alias.length() > 0) {
                deviceAlias.alias = alias;
            } else {
                // Remove alias if empty - find and remove the entry
                for (size_t i = 0; i < deviceAliases.size(); i++) {
                    String mac = deviceAliases[i].macAddress;
                    normalizeMACAddress(mac);
                    if (mac.equals(normalizedMAC)) {
                        deviceAliases.erase(deviceAliases.begin() + i);
                        break;
                    }
                }
            }
            return;
        }
    }
    
    // Add new alias if not empty
    if (alias.length() > 0) {
        DeviceAlias newAlias;
        newAlias.macAddress = normalizedMAC;
        newAlias.alias = alias;
        deviceAliases.push_back(newAlias);
    }
}

// ================================
// Persistent Device Storage Functions
// ================================
void saveDetectedDevices() {
    preferences.begin("ouispy", false);
    
    // Limit to 100 most recent devices to avoid NVS overflow
    int deviceCount = min((int)devices.size(), 100);
    preferences.putInt("deviceCount", deviceCount);
    
    for (int i = 0; i < deviceCount; i++) {
        String keyMac = "dev_mac_" + String(i);
        String keyRssi = "dev_rssi_" + String(i);
        String keyTime = "dev_time_" + String(i);
        String keyFilt = "dev_filt_" + String(i);
        
        preferences.putString(keyMac.c_str(), devices[i].macAddress);
        preferences.putInt(keyRssi.c_str(), devices[i].rssi);
        preferences.putULong(keyTime.c_str(), devices[i].lastSeen);
        preferences.putString(keyFilt.c_str(), devices[i].filterDescription);
    }
    
    preferences.end();
}

void loadDetectedDevices() {
    preferences.begin("ouispy", true);
    int deviceCount = preferences.getInt("deviceCount", 0);
    
    devices.clear();
    
    for (int i = 0; i < deviceCount; i++) {
        String keyMac = "dev_mac_" + String(i);
        String keyRssi = "dev_rssi_" + String(i);
        String keyTime = "dev_time_" + String(i);
        String keyFilt = "dev_filt_" + String(i);
        
        DeviceInfo device;
        device.macAddress = preferences.getString(keyMac.c_str(), "");
        device.rssi = preferences.getInt(keyRssi.c_str(), 0);
        device.lastSeen = preferences.getULong(keyTime.c_str(), 0);
        device.filterDescription = preferences.getString(keyFilt.c_str(), "");
        device.firstSeen = device.lastSeen;
        device.inCooldown = false;
        device.cooldownUntil = 0;
        device.matchedFilter = nullptr;
        
        if (device.macAddress.length() > 0) {
            devices.push_back(device);
        }
    }
    
    preferences.end();
    
    if (isSerialConnected()) {
        Serial.println("Detected devices loaded from NVS (" + String(deviceCount) + " devices)");
    }
}

void clearDetectedDevices() {
    devices.clear();
    
    preferences.begin("ouispy", false);
    preferences.putInt("deviceCount", 0);
    preferences.end();
    
    if (isSerialConnected()) {
        Serial.println("All detected devices cleared from memory and NVS");
    }
}

// ================================
// Web Server HTML
// ================================
const char* getASCIIArt() {
    return R"(
                                                                                                                                                                                                                                                                                                                            
                                                                                                                                                                                                                                                                                                                            
                                                                                                                                                                                                                                                                                                                            
                                                                                                                                                                                                                                                                                                                            
                                                                                                                                                                                                                                                                                                                            
                                                                                                                                                                                                                                                                                                                            
                                                                                                                                                                                                                                                                                                                            
                                                                                                                                                                                                                                                                                                                            
                                                                                                                                                                                                                                                                                                                            
                                                                                                                                                                                                                                                                                                                            
                                                                                                                                                                                                                                                                                                                            
                                                                                                                                                                                                                                                                                                                            
                                                                                                                                                                                                                                                                                                                            
                                                                                                                                                                                                                                                                                                                            
                                                                                                                                                                                                           @@@@@@@@                                                         @@@@@@@@                                        
                                                                                                                                                                                                       @@@ @@@@@@@@@@                                                    @@@@@@@@@@ @@@@                                    
                                              @@@@@                                                           @@@@@                                                                               @@@@ @ @ @@@@@@@@@@@@@                                               @@@@@@@@@@@@ @@@@@@@@                                
                                         @@@@ @@@@@@@@                                                     @@@@@@@@@@@@@                                                                     @@@@ @@@@@@@@@@@@@@@@@@@@@@@@                                          @@@@@@@@@@@@@@@@@@@ @@@@@@@@@                           
                                     @@@@@@@@ @@@@@@@@@@                                                 @@@@@@@@@@@@ @@ @@@@                                                            @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@                                    @@@@@@@@@@@@@@@@@@@@@@@@@@ @@@@@@@@@                       
                                @@@@@@@@@@@@@@@@@@@@@@@@@@@                                           @@@@@@@@@@@@@@@@@@@@@@@@@@@                                                        @@@@@@ @@@@@@@@@          @@@@@@@@@@@@                                @@@@@@@@@@@@@          @@@@@@@@@@@@@@@                       
                           @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@                                      @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@                                                   @@@@@@@@@ @@@               @@@@@@@@@@@@@                          @@@@@@@@@@@@@               @@@@@@@@@@@@@                       
                          @@@ @@@@@@@@@@@@@       @@@@@@@@@@@@@@                                 @@@@@@@@@@@@@@      @@@@@@@@@@@@@@@@@@                                                  @@ @@@@@@@@@                  @@@@@@@@@@@@@@                     @@@@@@@@ @@@@                   @@@@@  @@@@                       
                          @@@@ @@@@@@@@@              @@@@@@@@@@@@                            @@@@@@@@@@@@@              @@@@@@@@@ @@ @                                                  @@@@   @@@@                   @@@@@@@@@@@ @@                     @ @@@@@@@@@@@                    @@@@  @ @@                       
                          @@@@@@@ @@@                   @@@@@@@@@@@@@                       @@@@@@@@@@@@@                  @@@@ @@@@@@@                                                   @@@  @@@@                     @@ @@@@@@@@@@                     @@@@@@@@@ @@@                     @@@  @@ @                       
                          @@@@@  @ @@                   @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@ @@@@@@@@@@                   @@@@  @@@@                                                    @@@  @@@@                     @@@  @@ @                              @ @@@@@                      @@@@ @@@@                       
                           @@@   @@@                     @@@@@@@ @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@                    @@@@   @@@                                                    @@@@ @@@@                    @@@@  @@@@                              @@@@@@@@                    @@@@@@@@@@                       
                           @@@@ @@@@                     @@ @@@@ @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@   @@@                     @@@  @@@@                                                    @@@@ @@@@@                   @@@   @ @                                 @ @@@@@                  @@@@@@@@@@                        
                           @@@@ @@@@                     @@ @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@  @@@                     @@@@ @@@@                                                    @@@@@@@ @@@                @@@@@   @ @                                 @ @ @@@@                @@@@@@@@@ @                        
                           @@@@ @@@@@                   @@@ @ @                                @@@@  @@@@                   @@@@@ @@@@                                                     @@@@@@@@@@@@             @@@@@    @@@@                               @@@@  @@@@@            @@@@@@@  @  @                        
                           @@@@ @@ @@@                 @@@@ @ @                                 @ @   @@@@                 @@@ @@@@@@                                                      @@@ @@@ @@@@@@@@     @@@@@@@@     @@@@                               @@@@   @@@@@@@@    @@@@@@@@ @@ @@@@@                        
                            @@@@@@@@@@@@             @@@@@  @@@                                @@@@   @@@@@              @@@@@@@@@@@@                                                      @@@@@@@   @@@@@@@@@@@@@@@@@        @@@                               @@@      @@@@@@@@@@@@@@@@@  @@ @@@@@                        
                            @@@@ @@ @@@@@@         @@@@@@   @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@     @@@@@@        @@@@@@@ @@ @@ @                                                      @@@@@@@       @@@@@@@@@  @@@@@@@            @@@@@@@@          @@@     @@@@  @    @@@@@@@@@@      @@ @@@@@                        
                            @@ @@@@@ @@@@@@@@@@@@@@@@@@     @@@@@                             @@@@       @@@@@@@@@@@@@@@@@@   @@@@ @@                                                      @@@@@@@       @@@  @@   @@ @@@@@           @@@@  @ @          @ @     @@@@@@ @     @ @           @@ @ @@                         
                            @@ @ @@@  @@ @@@@@@@@@@@@@@@@@@   @@@@@@@ @@@@@@@@ @@@@@@@@@@@@@@@@@@@        @@ @@@@@@@@@@@@@@@ @@@@@@@@                                                      @@ @@@@      @@@@@@@@@@  @@@@@@ @@@        @@@@@@@@@          @@@@@   @@@@   @@@   @@@@@@@@      @@ @@@@                         
                            @@@@ @@@  @@@@     @@@@  @@@@@@     @ @@@@@   @@@@@@@@@        @@@@@@@@@@@@   @ @ @@@@@@@@@  @@@@@@@@@@@@                                                       @@@@@@@  @@@ @@  @@@@@@@@@    @@@@         @@@@@@@   @@@@@   @@@@@@ @@@  @ @@@@@@@@@@@@@@@      @@@@@ @                         
                            @@@@@@@@  @@@@  @@@@@@@@ @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@  @@@@@@@@@@ @@@@@@@@ @ @@@@@@@@@@@@@@@ @@@@                                                        @@@@@@@  @ @ @@  @@@@@@@@@@   @@@@          @@@@@@   @@@@@   @@@@@@ @ @   @@@@@@@@ @@  @@@      @@@@@@@                         
                             @@@@ @@  @ @ @@@@@@@@@@@@@@@@@@@@@@@ @@@@@@@@@@@ @@@@ @@@@@@@@@@@ @@@@@@@@@@@@   @  @@@@@@@ @@@@@@ @@@@                                                        @ @@ @@  @@@@ @  @@@@@@@@@@@@@@@            @@@@@@   @@@@@   @@@@@@ @@@@  @@@  @@@@@@@@@@@      @@@@@@@                         
                             @  @ @@  @@@@@@@@@@@@@@@ @@@@@@@ @@@@@@@@@@@@ @@@  @@@@@@@@@@@@@@@@@ @  @@ @@@@  @  @@@@@ @@@   @@ @@ @                                                        @ @@@@@  @@@ @@  @@@@@ @@ @ @@ @@           @@@@@@   @@@@@   @@@@@@@@@@           @@@@@@       @@@@  @                          
                             @@ @ @@  @@@@@@@@@@@@@@@ @@@@@@@@@@@   @@@@@@ @@@@ @@ @@@@@@@@@@@@@@ @@@@@@@@@@  @@@@@@@@@@     @@@@@ @                                                        @@ @@@@  @@@@    @@@@@@   @@@@@@@           @  @@@   @@@@@   @@@@@@@@@@           @@@@ @       @@@@@@@                          
                             @@@@@@@  @@@@@@    @@@ @ @@ @@@@@@@@@   @@@@@@@@@@ @@@@@@@@@@@ @@@@@ @ @@ @@@@@  @@@@@ @@       @@@@@@                                                          @@@@@@  @@@@    @@@@@@       @@@           @@@@@@   @@@@@   @@@@@@@@@@           @@@@@@       @@@@@@@                          
                             @@ @@@@  @@@@@@@@@@@@@@@ @@@@@@@    @@@@@@ @@@@@@@@@@@@@@ @@@@@@@@@@@@@@@@@@@    @@@@@@@@       @@@@@@                                                          @@@@@@  @@@      @ @ @@@@@@  @@@@ @        @@@@@@   @@@@@   @@@  @@@@@   @@@@@@  @@@@         @@@@@@                           
                              @  @@@  @@@@@@@@ @@@@@@ @@@@@@@    @@@@@@@@@@@@@@@@@@ @@@@@@@@@@@@@ @ @@@@@@    @@@@@@@@@      @@@@@@                                                          @@@@@@   @@@    @@@@ @ @@@@@@@@@@ @        @@@@@@   @@ @@      @ @@@@@@@@@@@@@@  @@@@ @       @@@@@@                           
                              @@ @@@   @@@@@@@@@@@@   @@@@@@@     @@@@@@ @@@@@@@@@@@@@@    @@@@@@@@@@@@@@@    @@@@@@@@@      @@@@@@                                                           @@@@@   @ @    @@@@ @@@@@@@@@@@ @@        @@@@@@   @@@@@      @@@@@@@ @ @@@@@@  @@@@         @@@  @                           
                              @@@@@@      @@@@ @@@       @@@@             @@@ @@@@@      @@@@   @   @@@       @@@  @@@@      @@@@@                                                            @@@@@   @@@     @@@     @@@@@@@           @@@                      @@@@@@@@@    @@@@         @@@@@@                           
                              @@@@@@@        @@       @@@@@   @@@@@@      @@@@@@@@@@@@@@@@   @    @@@@@@@@@@@@              @@ @@@                                                            @@@ @                                                                                        @@@@@@                           
                              @@@@@@@      @@@@@      @ @@@@@ @@@@@@@@@   @@@@@ @@@@ @@@@ @@@@   @@@@@@@@  @@@              @@@@@@                                                            @@@@@@             @@@@@@@@@    @@@   @@@    @@@@@@@@@    @@@@@@@@     @@@@@@@@@             @@@@@                            
                               @  @@@      @@@@       @@@@@ @ @@@@@@@ @   @@@@@@@@@@@@@@@        @@@@@@@@@@@@@              @@@@ @                                                            @@@@@@             @@    @@@    @ @   @ @@@  @@@    @@    @@ @@@@@     @@@    @@@            @ @@@                            
                               @@@@@@      @@@@       @@@@@@@ @@@@@@@@ @@@@@@@@      @@@@      @@@@@@@     @@ @@@@@         @@@@@@                                                            @@@@@@             @@@@@@@@@@@@ @@@   @@@@@  @@@@@@@ @@@@  @@@@@@@@@@@  @@@@@@ @@@@          @ @@@                            
                               @@@@@@     @@@@@      @@@@@@@@ @@@@@@  @@ @@@@@@      @@@@      @@@@@@@@      @@@@@@         @@@@@                                                              @@@@@           @@@@@   @@ @@@ @@@@  @@@@@@@@@@   @@@@@@@@@@   @@@@@@@@@@   @@@@@@         @@@@ @                            
                                 @@@@     @@@@@      @@@@@@@@ @@@@@@  @@@@@@@@@      @@@@      @@@@@@@@@@@@@@@@@@@@         @@@@@                                                              @@@ @           @@ @@@  @@@@@@ @@@@@ @@@ @@@@@@   @@@@@@@@@@   @@ @@@@@@@   @@@@@@         @@@@@@                            
                                @@@@@     @@@@@@@@@@@@@@@@@@@ @@@@@@     @@@@@@     @@@@@@@     @@@@@@@@@@@@@@ @@@@         @ @ @                                                              @@@@@           @@@@@@ @  @@@@ @@@@  @@@@@@@@@@   @@@@@@@@@@   @@@@@@@@@@   @@@@@@         @@@@@@                            
                                @@@ @     @@ @  @      @@@   @@@  @@      @@@@@     @@   @@         @@@@@@@@@  @@           @@ @@                                                              @@@@@              @@@  @  @@@ @@@  @@@@@@@@@@@   @@@ @@@@@@   @@ @@@@@@@   @@ @@@         @@@ @                             
                                @   @        @@@@@@@@@@@@@    @@@@@@    @ @@@@@@@@  @@@@@@@         @@@@@@@@@@@@            @@@@@                                                              @@@@                       @ @@@@@  @ @@ @ @@@@    @@ @@@@@@    @@@@@@@@@                    @@@                             
                                @@@@@      @@@@@@@@@@@@@@@@@@@  @@@@   @@@   @@@@@@  @@@@   @@@@@       @@@@@               @@@@@                                                               @@@                       @@@@@@@  @@@@@@@@@@@     @@@@@@@@    @@@@ @@@@                    @ @                             
                                @@@@@      @@ @@@  @@@ @  @@ @  @@@@   @ @@@@@ @@@@  @@@@   @ @@@@@@   @@@@@@                @@@                                                                @@@               @@@        @@@@   @@@  @@@@@   @@@  @@@@@        @@@@@                   @@@@                             
                                 @@@       @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@ @@@@@@@ @@@@@@@   @ @@@@@@@@@@@@@ @@               @@@                                                                @ @              @@@@@@@@@@@ @@@@   @@@@ @@@@@@@@@@@@@ @@@@  @@@@  @@@@@                   @@@@                             
                                 @@@              @@@@@     @@@@@@@@@@@@@@@@@  @@@@@@@@@@   @@@@@@@@@@@@@@@@@@@@             @ @                                                                @ @              @@@@@@@@@ @  @ @   @@@     @@@@@@@@ @  @ @     @    @ @                   @ @                              
                                 @ @              @@@@@     @@@@@@@@@@@@@@@@@  @@@@@@@@@@   @ @@@@@@@@@@ @@@@@ @             @ @                                                                @@@              @@@@@@@@@@@  @@@   @@@@    @@@@@@@@@@  @@@  @@@@    @@@                   @ @                              
                                 @@@@             @@@@@     @@@@  @@@@@@@ @@@@@@@ @@ @@@@   @ @@@@@@@@@@@@@ @  @@@          @@@@                                                                 @@@                                                                                       @@@                              
                                 @@@@            @@@@@@@      @@@@@@    @@@ @@@@@ @@@@@@@@@@@@@@@@@@@@@@ @@@@@@@@@          @@@                                                                  @ @  @@@    @@@   @@@@   @@@   @@@@@@@@@@@@ @@@@@@@@@@         @@@   @@@@   @@@@@@@@@     @@@                              
                                  @@@  @@@@@@    @@@@ @@      @@@@@@    @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@ @@@@@@@@@ @@@@@@@  @@@                                                                  @@@  @ @    @@@@  @@@@   @@@@  @@@@@@@@  @@ @@ @ @ @@@@        @ @   @@@@   @@  @ @@@@   @@@@                              
                                  @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@ @@    @@@@@@@@@@@ @ @@@@@@@ @@@@ @@@@@@ @@@@@@@@@@@@@@@@@@@@                                                                  @@@  @@@    @@@@  @@@@   @@@@  @@@@@@@@@@@@  @@@@@@@@@@        @@@   @@@@   @@@@@@@@@@   @@@@                              
                                  @ @@@@@  @@@@@@ @@@@@        @@ @@@@@@ @@@@@     @ @@@@@@@@@@ @@  @@@       @@@@@@@@  @@@@@ @                                                                  @ @ @@@     @@@@@@@@@@@  @@@@     @@@ @@   @@@@    @@@@        @@@   @@@@@@ @@    @@@@@@ @@@                               
                                  @@@@ @@@@@@ @@ @@@@@@@@@@@@@@ @@@@@@@@@@@@@@@@@@@@@@@@ @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@   @@@ @                                                                  @@@@@@@@    @@@@@@@@@@@  @@@@@@   @@@@@@   @@@@@   @@@@@@    @@@@@   @@@@@@ @@@@ @@@ @ @ @@@                               
                                  @@@@@@@@@@@@ @@ @          @@@@@@@@@@@                 @@@@@@@ @@@          @ @@@@@@@@@@@@@@                                                                    @@@@@@@@   @@@@@@@@@@@  @@@@ @   @@ @@@   @@@@@   @@@@@@    @@ @@   @@ @@@ @@@@ @ @  @@ @@@                               
                                  @@@@@@@@@@@@@@@@@        @@@@@@@@@                          @@@@@@@@        @ @@@@ @@@@@@@@@                                                                    @@@@@@@    @@@@@@@@@    @@@@@@   @@@@@@   @@@@@   @@@@@@    @@@@@   @@@@@@ @@@@ @@@@   @@@                                
                                   @@@@@@@@@@@@@ @@      @@@@@@@                                @@@@@@@@      @@@ @@@@@@@@@@@@                                                                    @@@@@@@     @ @ @@@@@   @@@ @@   @@@@@    @@@@@    @@@@@    @@@@@   @@@@@@       @@@@@@@@@                                
                                   @@@@@@@@@@@@@@@@@   @@@  @@@@                                 @@@@@@@@@    @@@@@@@@@@@@@@@@                                                                    @ @@@@@     @ @ @@@@@   @ @@ @    @ @@    @@@@@   @@@@ @    @@@@@   @@@  @  @@@  @ @@ @@ @                                
                                   @@@ @@@@@@@@@@@@@ @@@@@@@@@                                      @@@@@@@@ @@@@ @@@  @@@@@@                                                                      @ @@@@    @@@@ @@@@@   @ @@@@   @@@@     @@@@@   @ @@@@    @@@@@   @@@@@@  @ @  @ @@@ @@@                                
                                   @@@@@@@@@@@@@ @@@@@@  @@                                         @@@@@ @@@@@@   @@@@@@@@@@                                                                      @@@@@  @@@@@@@ @@@@    @ @      @ @      @@@ @@@@@@@       @@@@@@@@@ @  @  @@@@@@ @  @@@@                                
                                    @@@@@@@@@@@@ @@@@@@@@@@                                          @@ @@@ @@@@   @@@@@@@@@@                                                                      @@@    @@@ @ @ @@@@    @ @      @ @      @@@@@@@@@ @           @@@@@ @ @@  @@@@ @ @  @ @                                 
                                    @@@  @@@@@   @@@@@ @@@                                            @@ @@@@@@@   @@ @@@ @@@                                                                      @@@@@@ @@@ @@@ @@@@    @@@      @@@       @@@@@@@@@@           @@@@@@@     @@@@ @@@@@@@@                                 
                                    @@@@@@@ @@   @@@@ @@@@                                             @@@@ @@@@   @@@@@@@@@@                                                                       @@@@@   @@@                              @@@@                               @@@   @@@@@                                 
                                    @@@  @@@@@@@@    @@@@    @@@@@@@                       @@@@@@@@@@@  @ @     @@@@@@@@ @@@                                                                        @ @@@  @@@@                              @@@@                               @@@  @@ @@@                                 
                                      @@ @@@@@ @@    @@@@  @@@@@@@@@@                      @@@@@@@@@@@  @@@@    @@ @@@@@ @@@                                                                        @@ @@  @@@@        @@@@                  @@@@                   @@@@        @ @  @@@@@@                                 
                                     @@@ @@@@@ @@    @ @   @@@@   @@@@                     @@       @@   @@@   @@@ @@@@@ @ @                                                                        @@@@@@ @@@         @@@@@@                @@@@@                @@@@@@        @ @  @@@ @                                  
                                     @@@  @@@@ @@    @ @   @@      @@@                     @@       @@   @ @   @@@ @@@@  @ @                                                                        @@@@@@ @@@         @@@@@@@             @@@@@@@@             @@@@ @@@        @@@@ @@@ @                                  
                                     @@@@ @@@@@@@    @ @   @@@@  @@@@@                     @@       @@   @@@   @@@@@@@@  @@@                                                                        @@@@@@ @@@          @@@@@@@@@@@@@@@@@ @@@@@ @@@@@@@@@@@@@@@@@@ @@@@         @@@@@@@@ @                                  
                                     @@ @@@@@@@@     @@@@ @@@@@@@@@@@                      @@@@@@@@@@@@ @@@@     @@@@@@@@@@                                                                          @ @@@ @@@           @@@@@@@@@   @@@@@@@@@@@@@@@@@@@@@  @@@@@@@@@            @@@@@@@@@                                  
                                      @@@ @@ @@@     @@@@@  @@@@@@@@                       @@@@@@@@@@@@@@ @      @@@@@   @@                                                                          @@@@@@@@@             @@@@@@@@@@@@ @@@ @@@@@@@@@@@ @@@@@@@@@@@              @@@@@@@@@                                  
                                      @@@@@@@@@@      @@@@@@                                   @@@   @@@@@@      @@@@@@@@ @                                                                          @@@@@@@@@              @@@@@ @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@              @ @@@@@@@                                  
                                      @@@ @@@@@       @@@@@                                  @@@@@@@  @@@@@       @@@@ @@@@                                                                          @ @@@@@@@              @@ @@@@ @@@@@ @@@     @@ @@@@@@@@@@@@@               @ @@@@ @                                   
                                      @ @@@@@@@@@@    @@@@@                                  @@@ @@@ @@@@@@    @@@@@@@@@@@@                                                                          @@@ @@@@               @@@@@@@@ @@@@@@@@@@@@@@@@@@@@@@@@ @@@@               @ @@@@ @                                   
                                      @@@@@@@@@ @@   @@@@@@                                  @@@@@@@ @@@@@@@   @@ @@@@@@@@@                                                                           @@@@@@@                @@@@@@@@@@@@@@@@@@@@@@@@@@@@@ @@@@@@@               @@@@@@@@                                   
                                      @@@@@@@@@@@@   @@@@@ @@@                                 @@@   @@@ @ @   @@@@@@@@@@@                                                                            @@@@@@@                @@@@@@@@@   @@@@@@@@@@@@@  @@@@@@@@@@               @@@@ @@@                                   
                                       @ @@@@@@@@@   @@ @@@@@@                                        @ @@@ @@ @@@@@@@@@@@                                                                            @ @@@@@              @@@@@@@ @@       @@@@@@@@@@   @@@@@@ @@@              @@@@@@@@                                   
                                       @@@@@@@@@@@ @@ @@@@@@@@@@@@                             @@@    @@@@@@ @@@@@@@@ @@@@                                                                            @ @@@@@            @@@@@@@@@@@@       @@@@@@@@@    @@@@@@@@@@@@@            @@@@ @                                    
                                       @ @@@@@@@@@@@ @@ @ @@@ @@@@@@                        @@@@ @ @   @@@  @ @@@@@@@@@  @                                                                            @@@ @@@          @@@@ @@@@@@@@@       @@@@@@@@     @@@@@@@@@ @@@@@          @@@@@@                                    
                                       @@@@ @@@@ @@@@@@@@   @@@@@@@@ @@@@               @@@@@ @@@@     @@@  @@@@ @@@@@@@@@                                                                             @@@     @@@@@@@@@@@@@ @@@@@@@@    @@@@@@@@@@@@    @@@@@@@@@@@@@@@@@@@@@@      @@@                                    
                                       @@@@ @@@@ @ @ @@@@     @@@@@@@@@@@ @@@@@@@@@ @@@ @@@@@@@@       @@@@ @@@@ @@@  @@@                                                                              @ @     @@      @@@@@@@  @@@@@    @@  @@@@@@@@    @@@@@   @@@@@@@     @@      @ @                                    
                                        @ @ @@@@ @ @ @ @        @@@ @ @@@ @@@@@@ @@ @ @ @@@@  @@       @@@@ @@@@ @@@@@@@@                                                                              @@@     @@@@@@@@@@@@@@@@@@@@@@    @@@@@@@@@@@@    @@@@@@@@@@@@@@@@@@@@@@     @@@                                     
                                        @@@      @ @ @@@         @@@@@@@  @@@@@@@@@ @@@  @  @           @@@ @@@@     @@@@                                                                                               @@@@@ @@@@@@@        @@@@        @@@@@@@@ @@@@              @@@                                     
                                        @@@      @ @ @@@            @@ @@@                @@            @@@ @@@@     @@@                                                                               @ @                @@@@@ @@@@@       @@@@@@@      @@@@@@@@@@@                @ @                                     
                                        @@@      @ @ @ @             @@@ @                              @@@ @@@@     @@@                                                                               @ @                   @@@@@@@@       @@@@@@@      @@@@@@@@@                  @ @                                     
                                        @ @   @@@@ @ @@@               @@@                              @@@ @@@@@@   @@@                                                                               @@@                     @@@@@@     @@@@@@@@@@@    @@@@@@@                    @@@                                     
                                        @ @ @@@ @@ @                                                        @@@@ @@@@@@@                                                                               @@@                     @@@@@@     @@ @@@@  @@    @@@@@@@                    @@@@                                    
                                        @@@@@ @@@@@@@   @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@   @ @@@@@ @@@@                                                                               @@@                   @@@@@@@@     @@@@@@@@@@@    @@@@@@@@@                   @@@                                    
                                        @@@ @@    @ @@ @@@                                             @@  @@ @   @@@@@@                                                                              @@@@                @@@@@ @@@@@        @@@@        @@@@@@@@@@@                 @@@                                    
                                        @@@@@      @ @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@ @ @@      @@@@@                                                                             @ @               @@@@@ @@@@@@@      @@@@@@@@@     @@@@@@@@ @@@@@              @ @                                    
                                        @@@@@@@@@@@@@@@  @                                            @ @@ @@@@@@@@@@@@@@                                                                             @@@      @@@@@@@@@@@@@@@@@@@@@@      @ @@@@@ @     @@@@@@@ @@@@@@@@@@@@@@      @@@@                                   
                                       @@@@@@@@@@@@@@@   @                                            @ @@ @ @@@@@@@@ @@@                                                                             @@@      @@      @@@ @@@  @@@@@      @@@@@@@@@     @@@@@   @@@ @@@     @@      @@@@                                   
                                       @@@@@@@@   @@@@   @                                            @ @@ @ @@   @@@ @@@@                                                                            @ @      @@@@@@@@@@@@@@@@@@@@@@        @@@@@       @@@@@@@@@@@@@@@@@@@@@@       @@@                                   
                                       @@  @@@@@@@@@@@   @                                            @ @@ @ @@@@@@@@  @ @                                                                           @@@@              @@@@@@@@@@@@@@        @@@@        @@@@@@@@@@@@@@@              @@@                                   
                                       @@@ @@@@@@@@@ @   @                                            @ @@ @ @@@@@@@@  @@@                                                                           @@@                 @@@@@@@@@@@@        @@@@@@      @@@@@@@@@@@@                 @ @                                   
                                       @ @ @   @@@   @   @                                            @ @@ @   @@@     @@@                                                                           @@@                  @@ @@@@@ @@        @@@@        @@@@@@@@@@@                  @ @                                   
                                       @   @@@@@@@@@ @   @                                            @ @@ @ @@@@@@@   @ @                                                                           @ @                  @@@@@@@@ @@@       @@@@       @@@@@@@@@@@@                  @@@@                                  
                                      @@@@ @@@@@@@@@@@   @                                            @ @@ @@@@@@@@@@  @@@@                                                                          @@@                  @@@@ @@@@@@@@@@@@@@@@@@@@@@@@@@@ @@@@@@@@@@                 @@@@                                  
                                      @@@@ @@@@   @@@@   @                                            @ @@ @@@@   @@@   @@@                                                                          @@@                  @@@@@@@@@@@ @@@@   @@@@   @@@@@@@@@ @@@@@@@                  @ @                                  
                                      @@@@ @@@@@@@@@@@   @                                            @ @@ @@@@@@@@@@   @@@                                                                         @@@@                  @@@@@ @@@@@@@ @@@@@@@@@@@@@@  @@@@@@@@@@@@@                  @@@                                  
                                      @@@@ @@@@@@@@@ @   @                                            @ @@ @ @@@@@@@@   @ @                                                                         @@@                  @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@ @@@@@@@  @@@                 @ @                                  
                                      @ @@ @         @   @                                            @ @@ @            @@@                                                                         @@@                @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@                @@@@                                 
                                     @@@@@ @@@@@@@@@ @   @                                            @ @@ @ @@@@@@@@   @@@                                                                         @@@              @@@@ @@@@@@@@@@@   @@@@ @@@@@@@@@   @@@@@@@@@@@@@@@@              @@@@                                 
                                     @@@@@ @@@@@@@@@@@   @                                            @ @@ @@@@@@@@@@   @@@@                                                                        @ @              @@@@@@@              @@@@@@@@@@@             @@@@ @@               @ @                                 
                                     @ @ @ @@@    @@@@   @                                            @ @@ @@@@   @@@    @@@                                                                        @@@              @@@@@                 @@@@@@@@                 @@@@@@              @@@                                 
                                     @@@ @ @@@@@@@@@@@   @                                            @ @@ @@@@@@@@@@    @@@                                                                       @@@               @@@@                    @@@@@                    @@@               @@@                                 
                                     @@@ @ @@@@@@@@@ @   @                                            @ @@ @ @@@@@@@@    @@@                                                                       @ @                                       @@@@                                       @ @                                 
                                    @@@  @ @  @@@@   @   @                                            @ @@ @   @@@@      @ @                                                                       @@@                                       @@@@                                       @@@@                                
                                    @@@  @ @@@@@@@@@ @   @                                            @ @@ @ @@@@@@@@    @@@                                                                       @ @                   @@@ @@@             @@@@@@           @@@ @@@@                  @@@@                                
                                    @@@  @ @@@@@@@@@@@   @                                            @ @@ @@@@@@@@@@     @@@                                                                     @ @                    @ @@@ @@@          @@@@@@@          @@ @@@@@@                @@@@ @                                
                                    @@@  @ @@@@  @@@@@   @                                            @ @@ @@@@   @@@     @ @                                                                     @@@@@@@                @@@@@@@ @          @ @ @ @        @@@@@@@@@@@                @@@@@@                                
                                   @@@@@@@ @@@@@@@@@@@   @                                            @ @@ @@@@@@@@@@     @@@                                                                     @@@@@@@                   @ @@@@@@@       @ @ @ @       @@ @@@@@@@ @                @@@@@@                                
                                   @@@@@@@ @@@@@@@@@ @   @                                            @ @@@@@@@@@@@@   @@@@@@                                                                     @@    @                @  @@@ @ @ @@@     @ @ @ @     @@@@@ @@@@ @ @               @@@@@@@@                               
                                   @@@@@@@ @  @@@@   @   @                                            @ @ @@@@@@@@@    @@@@@@                                                                    @@@@   @                @    @@@@@@@ @     @ @ @ @   @@@ @  @@@@  @ @               @@@@@@@@                               
                                   @@@@@@  @@@@@@@@@ @   @                                            @ @  @@@@@@@@@@  @@@@@@@                                                                   @ @@   @                @ @    @@ @@@@@@@  @ @ @ @  @@ @@@@@@@    @ @               @@  @  @                               
                                   @ @@@@  @@@@@@@@@@@   @                                            @ @   @@@@@@@@@  @@@@@@@                                                                   @@@@@  @                @ @     @@@ @ @ @@ @ @ @ @@@@@@ @ @@      @ @               @@  @@@@@                              
                                  @@@@@@@  @@@@  @@@@@   @                                            @ @   @@@   @@@  @@@@@@@                                                                   @@@@@ @@@               @ @       @@@@@@@@@@ @ @ @@ @ @ @@@       @ @               @@  @@@@@                              
                                  @@@ @@@  @@@@@@@@@@@   @                                            @ @   @@@@@@@@@  @@@@@@@                                                                   @ @@@ @@@               @ @         @@ @@@ @ @ @ @@@@@@@@         @ @               @@  @@@@@                              
                                  @@@@@@@  @@@@@@@@@@ @@ @                                            @ @    @@@@@@@   @@@@@@ @                                                                 @@@@@@ @@@               @ @          @@@ @@@   @@@@@ @            @ @              @@@  @@@@@                              
                                  @@@@@ @  @  @@@@@@ @@@ @                                            @ @     @@@@@    @@ @@                                                                    @@@@@@ @@@               @ @            @@@@@@  @@@ @@@            @ @              @@@   @  @                              
                                  @ @@@ @  @@@@@@@@@@  @ @                                            @ @    @@@@@@@@  @@ @@@@@                                                                 @@ @@@ @@@@              @ @              @@ @  @@ @@              @ @              @@    @@@@                              
                                  @ @@@ @  @@@@@@@@@@  @ @                                            @ @   @@@@ @@@@  @@ @@@ @                                                                 @ @@ @ @@@@              @ @               @@@@ @@@                @ @             @@@    @@@@@                             
                                 @@@@@@ @  @@@@  @@@@  @ @                                            @ @   @@@@ @@@@  @@ @@ @@@                                                                @@@@ @ @ @@              @ @                @ @ @ @                @ @             @@@    @@@@@                             
                                 @@@@@  @  @@@@@@@@@   @ @                                            @ @   @@@@@@@@@  @@ @@@@@@                                                                @@@@ @ @ @@              @ @                @ @ @ @                @ @             @@@     @  @                             
                                 @@@@@  @  @ @@@@@@    @ @                                            @ @   @@@@@@@   @@  @@@@@                                                               @@@@  @ @ @@@                                @ @ @ @                @ @             @@      @@@@                             
                                 @@ @@  @  @ @@@@@@    @ @                                            @ @   @@@@@    @@  @@@ @                                                               @@@@  @ @ @@@             @@@@@@@@@@@@@@@@@@@@@@ @@@@@@@@@@@@@@@@@@@@@@             @@      @@@@                             
                                 @ @@@  @  @@@@@@@@@   @ @                                            @ @   @@@@@@@@  @@  @@@@@                                                               @@@@  @ @  @@   @@@@@@@@@@@@                 @@@ @@@                @@@ @@@@@@@@@  @@@       @@@@                            
                                @@@@@   @  @@@@@ @@@   @ @                                            @ @   @@@@ @@@@  @@  @@ @@                                                               @@@@  @ @  @@@@@@@@@ @@@  @@@@@@@@@@@@@@@@@@@@@ @@@@@@@@@@@@@@@@@@@@@@ @@  @@@ @   @@@       @@@@                            
                                @@@@@   @  @@@@  @@@   @ @                                            @ @   @@@@ @@@@  @@   @@@@@                                                              @@@   @ @  @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@ @@@@@@@@@@@@@@@@@@@@@@@@ @@@@@@@@@@@@@@ @@        @@@@                            
                                @ @@@   @  @@@@@@@@@   @@@                                            @@@    @@@@@@@@  @@   @@@@@                                                              @@@   @ @  @@@ @@@@ @@ @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@ @@        @@@@                            
                                @@@@@   @@@@ @@@@@@     @                                             @@@    @@@@@@@@@@@@   @@@@@                                                             @@@@   @ @  @@@@@@@@@@@@@@@@@@@@@@ @@@@@ @@@@@ @@@@ @@@@@@@ @  @ @@@@@@@@@@@@@@@@@@ @@         @@@                            
                                @@@@    @@@@@                                                                       @@@@@   @@@@@                                                             @@@@   @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@ @@@@@@@@@@@@@ @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@ @@@@@@     @@@                            
                                 @@@    @@@ @@@                                                                   @@@@@@@   @@@ @                                                             @@@@   @@@ @@@@@@@@@@@@@@@@@@@@@@@ @@        @@@@ @@@@       @@ @@@@@@@@@@@@@@@@@@@@@@@@@@@@   @@@@                           
                               @@@@@    @@@@@ @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@ @@@@@    @@@@                                                             @@@  @@@@@@@@@@@@@@@@@@@@@ @@@@    @@@@@@@@@@@@@@@@@@@@@@@@@@@@    @@@@  @@@@@@@@@@@@@@@@@@@@@ @@@@                           
                               @@@@   @@@@@@@@@@@@@@        @@@                                   @@@        @@@@@@@@@@@@@@  @@@@@                                                            @@@@@@@@@@@         @@@@ @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@  @@@@@@        @@@@@ @@  @@                           
                               @@@@ @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@  @@ @                                                           @@@@@@@@@              @@@@@@@@@@@@@@@@@@@@@@@@@@  @@@@@@@@@@@@@@@@@@@@@ @@@@              @@@@@@@@@                           
                              @@@@@@@@@@@@@       @@@@@@@@@@@@@ @                               @ @ @@@@@@@@@@@      @@@@@@@@@@@@@                                                           @@@@@@@@                 @@@@@@@@                                   @@@@@@@                  @@@@@@@                           
                              @ @@@@@@@@             @@@@ @@@ @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@  @  @@@@             @@@@@@@@@                                                           @@@ @@@                   @@@@@ @                                   @ @@@@                    @@@@ @@                          
                              @@@@@@@@                 @@@@@@@@@                                 @@  @ @@                  @@@@@@@                                                          @@@@@@@                     @@@@ @                                   @ @@@@                     @@ @@@                          
                              @@@ @@@                    @@@@ @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@ @@@@                    @@@@@@@                                                         @ @ @@                      @@@@ @                                   @ @@@                      @@ @@@                          
                              @ @@@@                     @@@@ @                                   @ @@@@                    @@@ @@@                                                         @@@ @@@                     @@ @ @                                   @ @@@@                    @@@ @@@                          
                             @@@@@@@                     @@ @ @                                   @ @@@@                     @@ @@@                                                         @@@@@@@@                   @@@ @ @                                   @ @@@@@                   @@@@@@@@                         
                             @@@@@@@                     @@ @ @                                   @ @@@@                     @@ @@@                                                         @ @@@@@@@                 @@@@@@ @                                   @ @@@@@@                @@@ @@@@ @                         
                             @@@@@@@@                   @@@@@ @                                   @ @@@@@                   @@@@ @ @                                                        @@@@@@@@@@@             @@@@@@@@ @                                   @ @@@@@@@@            @@@@@@@@@@@@                         
                             @@@@@@@@@                 @@@@@@ @                                   @ @@@@@@                 @@@@@  @                                                        @@@@@ @@@@@@@@@       @@@@@@@@@@@@@                                   @@@@ @@ @@@@@      @@@@@@@@@ @@@@@                         
                             @ @@@@@@@@               @@@@@@@ @                                   @ @@@@@@               @@@@@@@ @@@                                                       @ @@@  @@@@@@@@@@@@@@@@@@@@ @@@@@@@                                   @@@@@ @@@@@@@@@@@@@@@@@@@@@  @@@ @                         
                             @@@@@ @@@@@@@         @@@@@@@@@@ @                                   @ @@ @@@@@@@         @@@@ @@@@ @ @                                                       @ @@@     @@@@ @@@@@@@ @@@@@@@@@@@                                     @@@@@@@@@@@@@@@@@@@@@@@     @@@ @                         
                            @@@@@@  @@@@@@@@@@@@@@@@@@@@@@@@@@@                                   @@@@@@@@@@@@@@@@@@@@@@@@@@@ @@ @@@                                                       @ @@@@@@@     @@@@@@@@@@@@@@@@@                                          @@@@@@@ @@@@@@@@@     @@@@@@@@@                         
                            @@@@@@    @@@@@@@@@@@@@@@@@@@@@@@@@                                   @@@@@@@@@ @@@@@@@@@@@@@@@   @@  @@@                                                      @@@@@@@@@@@@@  @@@@@@ @@@@@@                                                @@@@@@@@@@@@@  @@@@@@@@@@@@@                         
                            @@@@@@@@@      @@@@@@@@@@@@@@@@@                                         @@@@@@@@@@@@@@@@@     @@@@@@@@ @                                                          @@@@@@@@@@@@@@ @@@@@@                                                      @@@@@@ @@@@@@@@@@@@@@@                            
                            @@@@@@@@@@@@@@   @@@@@ @@@@@@                                              @@@@@@@ @@@@@   @@@@@@@@@@@@@@                                                              @@@@@@@@@@@@@@                                                            @@@@@@@@@@@@@@@                                
                               @@@ @@@@@@@@@@@@@@ @@@@                                                     @@@@  @@@@@@@@@@@@@@@@@                                                                      @@@@@@                                                                  @@@@@@@                                     
                                   @@@ @@@@@@@@@@@@                                                           @@@@@@@@@@@@@@@@                                                                                                                                                                                              
                                       @@@@  @@@                                                                @@@@ @@@@@                                                                                                                                                                                                  
                                                                                                                    @                                                                                                                                                                                                       
                                                                                                                                                                                                                                                                                                                            
                                                                                                                                                                                                                                                                                                                            
                                                                                                                                                                                                                                                                                                                            
                                                                                                                                                                                                                                                                                                                            
                                                                                                                                                                                                                                                                                                                            
                                                                                                                                                                                                                                                                                                                            
)";
}

const char* getConfigHTML() {
    return R"html(
<!DOCTYPE html>
<html>
<head>
    <title>OUI-SPY Detector</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        * { box-sizing: border-box; }
        body { 
            font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; 
            margin: 0; 
            padding: 20px;
            background: #0f0f23; 
            color: #ffffff;
            position: relative;
            overflow-x: hidden;
        }
        .ascii-background {
            position: fixed;
            top: 0;
            left: 0;
            width: 100%;
            height: 100%;
            z-index: -1;
            opacity: 0.6;
            color: #ff1493;
            font-family: 'Courier New', monospace;
            font-size: 8px;
            line-height: 8px;
            white-space: pre;
            pointer-events: none;
            overflow: hidden;
        }
        .container { 
            max-width: 700px; 
            margin: 0 auto; 
            background: rgba(255, 255, 255, 0.02); 
            padding: 40px; 
            border-radius: 16px; 
            box-shadow: 0 8px 32px rgba(0, 0, 0, 0.2); 
            backdrop-filter: blur(5px);
            border: 1px solid rgba(255, 255, 255, 0.05);
            position: relative;
            z-index: 1;
        }
        h1 {
            text-align: center;
            margin-bottom: 20px;
            margin-top: 0px;
            font-size: 48px;
            font-weight: 700;
            color: #8a2be2;
            background: -webkit-linear-gradient(45deg, #8a2be2, #4169e1);
            background: -moz-linear-gradient(45deg, #8a2be2, #4169e1);
            background: linear-gradient(45deg, #8a2be2, #4169e1);
            -webkit-background-clip: text;
            -moz-background-clip: text;
            background-clip: text;
            -webkit-text-fill-color: transparent;
            -moz-text-fill-color: transparent;
            letter-spacing: 3px;
        }
        @media (max-width: 768px) {
            h1 {
                font-size: clamp(32px, 8vw, 48px);
                letter-spacing: 2px;
                margin-bottom: 15px;
                text-align: center;
                display: block;
                width: 100%;
            }
            .container {
                padding: 20px;
                margin: 10px;
            }
        }
        .section { 
            margin-bottom: 30px; 
            padding: 25px; 
            border: 1px solid rgba(255, 255, 255, 0.1); 
            border-radius: 12px; 
            background: rgba(255, 255, 255, 0.01); 
            backdrop-filter: blur(3px);
        }
        .section h3 { 
            margin-top: 0; 
            color: #ffffff; 
            font-size: 18px;
            font-weight: 600;
            margin-bottom: 15px;
        }
        textarea { 
            width: 100%; 
            min-height: 120px;
            padding: 15px; 
            border: 1px solid rgba(255, 255, 255, 0.2); 
            border-radius: 8px; 
            background: rgba(255, 255, 255, 0.02);
            color: #ffffff;
            font-family: 'Courier New', monospace;
            font-size: 14px;
            resize: vertical;
        }
        textarea:focus {
            outline: none;
            border-color: #4ecdc4;
            box-shadow: 0 0 0 3px rgba(78, 205, 196, 0.2);
        }
        .help-text { 
            font-size: 13px; 
            color: #a0a0a0; 
            margin-top: 8px; 
            line-height: 1.4;
        }
        .toggle-container {
            display: flex;
            flex-direction: column;
            gap: 15px;
        }
        .toggle-item {
            display: flex;
            align-items: center;
            gap: 15px;
            padding: 15px;
            border: 1px solid rgba(255, 255, 255, 0.1);
            border-radius: 8px;
            background: rgba(255, 255, 255, 0.02);
        }
        .toggle-item input[type="checkbox"] {
            width: 20px;
            height: 20px;
            accent-color: #4ecdc4;
            cursor: pointer;
        }
        .toggle-label {
            font-weight: 500;
            color: #ffffff;
            cursor: pointer;
            user-select: none;
        }
        button { 
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%); 
            color: #ffffff; 
            padding: 14px 28px; 
            border: none; 
            border-radius: 8px; 
            cursor: pointer; 
            font-size: 16px; 
            font-weight: 500;
            margin: 10px 5px; 
            transition: all 0.3s;
        }
        button:hover { 
            transform: translateY(-2px);
            box-shadow: 0 8px 25px rgba(102, 126, 234, 0.4);
        }
        .button-container {
            text-align: center;
            margin-top: 40px;
            padding-top: 30px;
            border-top: 1px solid #404040;
        }
        .status { 
            padding: 15px; 
            border-radius: 8px; 
            margin-bottom: 30px; 
            margin-top: 10px;
            border-left: 4px solid #ff1493;
            background: rgba(255, 20, 147, 0.05);
            color: #ffffff;
            border: 1px solid rgba(255, 20, 147, 0.2);
        }
            .oui-add-btn { background: linear-gradient(135deg, #10b981 0%%, #059669 100%%) !important; font-size: 13px !important; padding: 8px 16px !important; margin: 8px 0 !important; width: 100%%; }
        .sig-lines { margin-top: 12px; }
        .sig-line {
            display: flex; align-items: center; flex-wrap: wrap; gap: 6px;
            padding: 8px 10px; margin-bottom: 6px;
            background: rgba(255,255,255,0.03);
            border: 1px solid rgba(255,255,255,0.10);
            border-radius: 8px; font-size: 12px;
            font-family: 'SFMono-Regular', Consolas, monospace;
        }
        .sig-vendor { font-weight: 700; color: #ffffff; margin-right: 4px;
                      font-family: 'Segoe UI', sans-serif; }
        .sig-mac  { color: #4dd0e1; }
        .sig-cid  { color: #ffb74d; }
        .sig-uuid { color: #81c784; }
        .sig-name { color: #ba9ffb; }
        .sig-meta { color: #e94560; }
        .sig-sep  { color: #6b6b7d; }
        .sig-rm {
            margin-left: auto; background: none; border: none;
            color: #ff6b6b; cursor: pointer; font-size: 14px;
            padding: 0 4px; line-height: 1;
        }
        </style>
</head>
<body>
    <div class="ascii-background">%ASCII_ART%</div>
    <div class="container">
        <h1>OUI-SPY Detector</h1>
        
        <div class="status">
            Enter MAC addresses and/or OUI prefixes below. You must provide at least one entry in either field.
        </div>

        <form id="configForm" method="POST" action="/save">
            <div class="section">
                <h3>OUI Prefixes</h3>
                <textarea id="ouis" name="ouis" placeholder="Enter OUI prefixes, one per line:
AA:BB:CC
DD:EE:FF
11:22:33">%OUI_VALUES%</textarea>
                <div class="help-text">
                    OUI prefixes (first 3 bytes) match all devices from a manufacturer.<br>
                    Format: XX:XX:XX (8 characters with colons)
                </div>
                <div id="sigLines" class="sig-lines"></div>
            </div>
            
            <div class="section">
                <h3>OUI Database</h3>
                <div class="help-text" style="margin-bottom: 15px;">
                    Browse known surveillance device OUI prefixes by manufacturer. Click <strong>"+ Add"</strong> to append them to your filter list above.
                </div>
                <div class="oui-db">
                    <!-- OUI_DB_START -->
                    <details>
                    <summary><b>RING</b> <code>11 OUIs</code></summary>
                    <div class="oui-entries"><code>18:7F:88</code> <code>24:2B:D6</code> <code>34:3E:A4</code> <code>54:E0:19</code> <code>5C:47:5E</code> <code>64:9A:63</code> <code>90:48:6C</code> <code>9C:76:13</code> <code>AC:9F:C3</code> <code>C4:DB:AD</code> <code>CC:3B:FB</code></div>
                    <button type="button" class="oui-add-btn" onclick="appendOUIs('18:7F:88,24:2B:D6,34:3E:A4,54:E0:19,5C:47:5E,64:9A:63,90:48:6C,9C:76:13,AC:9F:C3,C4:DB:AD,CC:3B:FB')">+ Add to filter list</button>
                    <div class="oui-meta"><strong>Category:</strong> Doorbell/Security Camera</div>
                    <div class="oui-meta"><strong>Detection Range:</strong> Typical WiFi/BLE range</div>
                    <div class="oui-meta"><strong>Common Devices:</strong> Ring Doorbell, Ring Camera, Ring Chime</div>
                    </details>
                    <details>
                    <summary><b>AXON</b> <code>1 OUI</code></summary>
                    <div class="oui-entries"><code>00:25:DF</code> <code>CID 0x034D</code> <code>UUID 0xFC81</code></div>
                    <button type="button" class="oui-add-btn" onclick="addVendor('axon','AXON','00:25:DF')">+ Add all signatures</button>
                    <div class="oui-meta"><strong>Category:</strong> Body Camera / Law Enforcement</div>
                    <div class="oui-meta"><strong>Detection Range:</strong> Short-range BLE/WiFi</div>
                    <div class="oui-meta"><strong>Common Devices:</strong> Axon Body Camera, Axon Fleet</div>
                    </details>
                    <details>
                    <summary><b>META / RAY-BAN</b> <code>composite</code></summary>
                    <div class="oui-entries"><code>CID 0x0D53 + UUID 0xFD5F</code> <code>name: Ray-Ban / Wayfarer / Oakley Meta</code></div>
                    <button type="button" class="oui-add-btn" onclick="addVendor('meta','META / RAY-BAN', null)">+ Add composite signature</button>
                    <div class="oui-meta"><strong>Category:</strong> Smart Glasses</div>
                    <div class="oui-meta"><strong>Detection Range:</strong> BLE range (~10-30 m)</div>
                    <div class="oui-meta"><strong>Common Devices:</strong> Ray-Ban Meta (Wayfarer, Headliner, Skyler), Oakley Meta HSTN</div>
                    <div class="oui-note">No OUI: the glasses rotate random MACs (RPA per BT spec). The composite requires Luxottica company ID 0x0D53 AND Meta service UUID 0xFD5F in the same advert, or the advertised name — single-signature CID/UUID filters false-positive on phones running Meta apps.</div>
                    </details>
                    <details>
                    <summary><b>FLOCK SAFETY</b> <code>1 OUI</code></summary>
                    <div class="oui-entries"><code>B4:1E:52</code></div>
                    <button type="button" class="oui-add-btn" onclick="appendOUIs('B4:1E:52')">+ Add to filter list</button>
                    <div class="oui-meta"><strong>Category:</strong> Automated License Plate Reader (ALPR) / Security Camera</div>
                    <div class="oui-meta"><strong>Detection Range:</strong> WiFi/Cellular</div>
                    <div class="oui-meta"><strong>Common Devices:</strong> Flock Safety Camera, Falcon Camera, Raven Camera</div>
                    </details>
                    <details>
                    <summary><b>DJI</b> <code>8 OUIs</code></summary>
                    <div class="oui-entries"><code>0C:9A:E6</code> <code>8C:58:23</code> <code>04:A8:5A</code> <code>58:B8:58</code> <code>E4:7A:2C</code> <code>60:60:1F</code> <code>48:1C:B9</code> <code>34:D2:62</code></div>
                    <button type="button" class="oui-add-btn" onclick="appendOUIs('0C:9A:E6,8C:58:23,04:A8:5A,58:B8:58,E4:7A:2C,60:60:1F,48:1C:B9,34:D2:62')">+ Add to filter list</button>
                    <div class="oui-meta"><strong>Category:</strong> Consumer & Commercial Drones</div>
                    <div class="oui-meta"><strong>Detection Range:</strong> WiFi/OcuSync up to several km</div>
                    <div class="oui-meta"><strong>Common Devices:</strong> Mavic, Phantom, Inspire, Mini series</div>
                    </details>
                    <details>
                    <summary><b>PARROT</b> <code>5 OUIs</code></summary>
                    <div class="oui-entries"><code>00:12:1C</code> <code>00:26:7E</code> <code>90:03:B7</code> <code>90:3A:E6</code> <code>A0:14:3D</code></div>
                    <button type="button" class="oui-add-btn" onclick="appendOUIs('00:12:1C,00:26:7E,90:03:B7,90:3A:E6,A0:14:3D')">+ Add to filter list</button>
                    <div class="oui-meta"><strong>Category:</strong> Consumer & Commercial Drones</div>
                    <div class="oui-meta"><strong>Detection Range:</strong> WiFi/BLE range</div>
                    <div class="oui-meta"><strong>Common Devices:</strong> Parrot Anafi, Parrot Bebop, Parrot AR.Drone</div>
                    </details>
                    <details>
                    <summary><b>SKYDIO</b> <code>1 OUI</code></summary>
                    <div class="oui-entries"><code>38:1D:14</code></div>
                    <button type="button" class="oui-add-btn" onclick="appendOUIs('38:1D:14')">+ Add to filter list</button>
                    <div class="oui-meta"><strong>Category:</strong> Commercial & Enterprise Drones</div>
                    <div class="oui-meta"><strong>Detection Range:</strong> WiFi range</div>
                    <div class="oui-meta"><strong>Common Devices:</strong> Skydio 2, Skydio X2, Skydio 3</div>
                    </details>
                    <!-- OUI_DB_END -->
                </div>
            </div>

            <div class="section">
                <h3>MAC Addresses</h3>
                <textarea id="macs" name="macs" placeholder="Enter full MAC addresses, one per line:
AA:BB:CC:12:34:56
DD:EE:FF:ab:cd:ef
11:22:33:44:55:66">%MAC_VALUES%</textarea>
                <div class="help-text">
                    Full MAC addresses match specific devices only.<br>
                    Format: XX:XX:XX:XX:XX:XX (17 characters with colons)
                </div>
            </div>
            
            <div class="section">
                <h3>Audio & Visual Settings</h3>
                <div class="toggle-container">
                    <div class="toggle-item">
                        <input type="checkbox" id="buzzerEnabled" name="buzzerEnabled" %BUZZER_CHECKED%>
                        <label class="toggle-label" for="buzzerEnabled">Enable Buzzer</label>
                        <div class="help-text" style="margin-top: 0;">Audio feedback for target detection</div>
                    </div>
                    <div class="toggle-item">
                        <input type="checkbox" id="ledEnabled" name="ledEnabled" %LED_CHECKED%>
                        <label class="toggle-label" for="ledEnabled">Enable LED Blinking</label>
                        <div class="help-text" style="margin-top: 0;">Orange LED blinks with same pattern as buzzer</div>
                    </div>
                </div>
            </div>
            
            <div class="section">
                <h3>WiFi Access Point Settings</h3>
                <div class="help-text" style="margin-bottom: 15px;">
                    Customize the WiFi network name and password for the configuration portal.<br>
                    <strong>Changes take effect on next device boot.</strong>
                </div>
                <div style="margin-bottom: 15px;">
                    <label for="ap_ssid" style="display: block; margin-bottom: 8px; font-weight: 500; color: #ffffff;">Network Name (SSID)</label>
                    <input type="text" id="ap_ssid" name="ap_ssid" value="%AP_SSID%" maxlength="32" style="width: 100%; padding: 12px; border: 1px solid rgba(255, 255, 255, 0.2); border-radius: 8px; background: rgba(255, 255, 255, 0.02); color: #ffffff; font-size: 14px;">
                    <div class="help-text" style="margin-top: 5px;">1-32 characters</div>
                </div>
                <div>
                    <label for="ap_password" style="display: block; margin-bottom: 8px; font-weight: 500; color: #ffffff;">Password</label>
                    <input type="text" id="ap_password" name="ap_password" value="%AP_PASSWORD%" minlength="8" maxlength="63" style="width: 100%; padding: 12px; border: 1px solid rgba(255, 255, 255, 0.2); border-radius: 8px; background: rgba(255, 255, 255, 0.02); color: #ffffff; font-size: 14px;">
                    <div class="help-text" style="margin-top: 5px;">8-63 characters (leave empty for open network)</div>
                </div>
            </div>
            
            <!-- Detected Devices Section -->
            <div class="section" id="detectedDevicesSection">
                <h3>Device Alias Management</h3>
                <div class="help-text" style="margin-bottom: 15px;">
                    Assign identification labels to detected MAC addresses for serial output tracking.<br>
                    <strong>Device history and aliases persist in non-volatile storage.</strong>
                </div>
                <div id="clearDeviceBtn" style="margin-bottom: 10px; text-align: right; display: none;">
                    <button type="button" onclick="clearDeviceHistory()" style="background: #8b0000; padding: 8px 16px; font-size: 13px; margin: 0;">Clear Device History</button>
                </div>
                <div id="previousSessionPanel" style="display: none; margin-bottom: 15px; border: 1px solid rgba(255,255,255,0.12); border-radius: 8px; overflow: hidden; background: rgba(255,255,255,0.015);">
                    <div style="display:flex; align-items:center; justify-content:space-between; padding: 10px 14px; background: rgba(255,255,255,0.04); cursor: pointer;" onclick="togglePrevSession()">
                        <span id="previousSessionTitle" style="font-family:'Courier New',monospace; font-size:12px; letter-spacing:1px; color:#4ecdc4;">PREVIOUS SESSION (0)</span>
                        <span style="display:flex; align-items:center; gap:10px;">
                            <button type="button" onclick="event.stopPropagation(); clearPreviousSession();" style="background:#4a0000; padding:4px 10px; font-size:11px; margin:0;">Clear</button>
                            <span id="previousSessionCaret" style="color:#888; font-size:11px;">[-]</span>
                        </span>
                    </div>
                    <div id="previousSessionList" class="device-list" style="opacity: 0.65; padding: 10px 12px; max-height: 300px;"></div>
                </div>
                <div id="deviceList" class="device-list">
                    <div style="text-align: center; padding: 30px; color: #888888;">
                        <p style="font-size: 14px;">No device records in storage.</p>
                        <p style="font-size: 12px; margin-top: 10px;">Detected devices during scanning operations will persist to this list.</p>
                    </div>
                </div>
            </div>

            <div class="button-container">
                <button type="submit">Save Configuration & Start Scanning</button>
                <button type="button" onclick="clearConfig()" style="background: #8b0000; margin-left: 20px;">Clear All Filters</button>
                <button type="button" onclick="deviceReset()" style="background: #4a0000; margin-left: 20px; font-size: 12px;">Device Reset</button>
            </div>
            
            <!-- Burn In Configuration Section -->
            <div class="section" style="border: 2px solid #8b0000; background: linear-gradient(135deg, rgba(139, 0, 0, 0.03) 0%, rgba(139, 0, 0, 0.08) 100%); margin-top: 40px;">
                <h3 style="color: #ff6b6b; margin-top: 0; font-size: 18px; letter-spacing: 1px; text-transform: uppercase; border-bottom: 2px solid rgba(255, 107, 107, 0.3); padding-bottom: 12px; margin-bottom: 20px; text-align: center;">
                    Burn In Settings
                </h3>
                
                <div style="background: linear-gradient(135deg, #1a0a0a 0%, #2d0a0a 100%); color: #ff9999; padding: 18px; border-radius: 8px; margin: 15px 0; border: 2px solid #8b0000; box-shadow: 0 4px 15px rgba(139, 0, 0, 0.3);">
                    <p style="font-weight: 600; font-size: 13px; margin: 0 0 10px 0; color: #ff6b6b; text-transform: uppercase; letter-spacing: 0.5px;">
                        Warning - Requires Flash Erase to Unlock
                    </p>
                    <p style="line-height: 1.5; margin: 0 0 12px 0; color: #ffcccc; font-size: 13px;">
                        Permanently locks all current settings: <strong>OUI/MAC filters, device aliases, buzzer/LED preferences</strong>
                    </p>
                    <p style="line-height: 1.4; margin: 0 0 8px 0; color: #e0e0e0; font-weight: 500; font-size: 12px;">
                        Effects after activation:
                    </p>
                    <ul style="text-align: left; line-height: 1.6; margin: 0 0 12px 0; padding-left: 20px; color: #e0e0e0; font-size: 12px;">
                        <li>Disables WiFi AP and 20-second config window</li>
                        <li>Boots directly to scanning mode (~2 seconds)</li>
                        <li>Removes web interface access</li>
                    </ul>
                    <p style="line-height: 1.4; margin: 0; color: #ffcccc; font-size: 12px;">
                        <strong>Unlock:</strong> hold the BOOT button during power-on (or erase flash + reflash)
                    </p>
                </div>
                
                <div style="background: linear-gradient(135deg, #0a1a0a 0%, #0a2d0a 100%); color: #99ff99; padding: 18px; border-radius: 8px; margin: 15px 0; border: 1px solid #166534; box-shadow: 0 2px 10px rgba(22, 101, 52, 0.2);">
                    <p style="font-weight: 600; margin: 0 0 8px 0; color: #4ade80; font-size: 13px; text-transform: uppercase; letter-spacing: 0.5px;">
                        Use Cases:
                    </p>
                    <ul style="text-align: left; line-height: 1.6; margin: 0; padding-left: 20px; color: #ccffcc; font-size: 12px;">
                        <li>Production deployments</li>
                        <li>Fixed installations</li>
                        <li>Security-sensitive environments</li>
                        <li>Battery-powered optimization</li>
                    </ul>
                </div>
                
                <div style="text-align: center; margin-top: 25px; padding-top: 20px; border-top: 1px solid rgba(255, 107, 107, 0.2);">
                    <button type="button" onclick="burnInConfig()" style="background: linear-gradient(135deg, #8b0000 0%, #6b0000 100%); color: #ffffff; font-size: 15px; padding: 15px 35px; font-weight: 600; border: 2px solid #ff0000; border-radius: 8px; cursor: pointer; text-transform: uppercase; letter-spacing: 1px; box-shadow: 0 4px 15px rgba(139, 0, 0, 0.4); transition: all 0.3s;">
                        Lock Configuration Permanently
                    </button>
                    <p style="font-size: 11px; color: #888888; margin-top: 12px; font-style: italic;">
                        Undo by holding BOOT during power-on
                    </p>
                </div>
            </div>
            
            <style>
                .device-list {
                    display: flex;
                    flex-direction: column;
                    gap: 10px;
                    max-height: 400px;
                    overflow-y: auto;
                }
                .device-item {
                    display: flex;
                    flex-direction: column;
                    gap: 10px;
                    padding: 12px;
                    border: 1px solid rgba(255, 255, 255, 0.1);
                    border-radius: 8px;
                    background: rgba(255, 255, 255, 0.02);
                }
                .device-info-row {
                    display: flex;
                    align-items: center;
                    gap: 12px;
                    flex-wrap: wrap;
                }
                .device-alias-row {
                    display: flex;
                    align-items: center;
                    gap: 10px;
                    width: 100%;
                }
                .device-mac {
                    font-family: 'Courier New', monospace;
                    font-weight: 500;
                    color: #4ecdc4;
                    font-size: 13px;
                }
                .device-rssi {
                    color: #a0a0a0;
                    font-size: 12px;
                }
                .device-time {
                    color: #888888;
                    font-size: 11px;
                    font-style: italic;
                }
                .device-time.recent {
                    color: #4ade80;
                }
                .alias-input {
                    flex: 1;
                    padding: 8px 12px;
                    border: 1px solid rgba(255, 255, 255, 0.2);
                    border-radius: 6px;
                    background: rgba(255, 255, 255, 0.05);
                    color: #ffffff;
                    font-size: 14px;
                    min-width: 0;
                }
                .alias-input:focus {
                    outline: none;
                    border-color: #4ecdc4;
                    box-shadow: 0 0 0 2px rgba(78, 205, 196, 0.2);
                }
                .save-alias-btn {
                    padding: 8px 16px;
                    font-size: 13px;
                    margin: 0;
                    white-space: nowrap;
                }
                .device-filter {
                    color: #a0a0a0;
                    font-size: 11px;
                    font-style: italic;
                }
                .match-badge {
                    display: inline-block;
                    padding: 2px 7px;
                    border-radius: 10px;
                    font-family: 'Courier New', monospace;
                    font-size: 10px;
                    font-weight: 700;
                    letter-spacing: 0.5px;
                    background: rgba(0,0,0,0.35);
                    border: 1px solid currentColor;
                    text-shadow: 0 0 4px currentColor;
                }
                .match-badge.type-OUI  { color: #00d4ff; }
                .match-badge.type-MAC  { color: #ff2ee0; }
                .match-badge.type-CID  { color: #ffb020; }
                .match-badge.type-SVC  { color: #a3ff2e; }
                .match-badge.type-NAME { color: #ff6b9d; }
                .match-badge.type-META { color: #e94560; }
                .prev-tag {
                    display: inline-block;
                    margin-left: 6px;
                    padding: 1px 6px;
                    border-radius: 4px;
                    font-family: 'Courier New', monospace;
                    font-size: 9px;
                    letter-spacing: 0.5px;
                    color: #888;
                    border: 1px solid rgba(255,255,255,0.15);
                    background: rgba(255,255,255,0.03);
                }
            </style>
            
            <script>
            // Load detected devices on page load
            window.addEventListener('DOMContentLoaded', function() {
                loadDetectedDevices();
                loadPreviousSession();

                // Ensure form submits on first click (mobile fix)
                const configForm = document.getElementById('configForm');
                if (configForm) {
                    const submitBtn = configForm.querySelector('button[type="submit"]');
                    if (submitBtn) {
                        submitBtn.addEventListener('touchstart', function(e) {
                            // Blur any focused inputs to ensure submit works on first tap
                            if (document.activeElement) {
                                document.activeElement.blur();
                            }
                        }, { passive: true });
                        
                        submitBtn.addEventListener('click', function(e) {
                            // Ensure any focused element is blurred before submit
                            if (document.activeElement && document.activeElement !== submitBtn) {
                                document.activeElement.blur();
                            }
                        });
                    }
                }
            });
            
            function formatTimeSince(milliseconds) {
                const seconds = Math.floor(milliseconds / 1000);
                const minutes = Math.floor(seconds / 60);
                const hours = Math.floor(minutes / 60);
                const days = Math.floor(hours / 24);
                
                if (seconds < 60) return 'Just now';
                if (minutes < 60) return minutes + ' min ago';
                if (hours < 24) return hours + ' hour' + (hours > 1 ? 's' : '') + ' ago';
                return days + ' day' + (days > 1 ? 's' : '') + ' ago';
            }
            
            function loadDetectedDevices() {
                fetch('/api/devices')
                    .then(response => response.json())
                    .then(data => {
                        const deviceList = document.getElementById('deviceList');
                        const clearBtn = document.getElementById('clearDeviceBtn');
                        
                        if (data.devices && data.devices.length > 0) {
                            clearBtn.style.display = 'block';
                            deviceList.innerHTML = '';
                            
                            data.devices.forEach(device => {
                                const deviceItem = document.createElement('div');
                                deviceItem.className = 'device-item';
                                
                                // First row: device info
                                const infoRow = document.createElement('div');
                                infoRow.className = 'device-info-row';
                                
                                const macSpan = document.createElement('span');
                                macSpan.className = 'device-mac';
                                macSpan.textContent = device.mac;
                                
                                const rssiSpan = document.createElement('span');
                                rssiSpan.className = 'device-rssi';
                                rssiSpan.textContent = device.rssi + ' dBm';
                                
                                const timeSpan = document.createElement('span');
                                timeSpan.className = 'device-time';
                                const timeSince = device.timeSince || 0;
                                timeSpan.textContent = formatTimeSince(timeSince);
                                if (timeSince < 60000) { // Less than 1 minute
                                    timeSpan.classList.add('recent');
                                }
                                
                                infoRow.appendChild(macSpan);
                                if (device.type) {
                                    infoRow.appendChild(makeMatchBadge(device.type, device.filter, device.match));
                                }
                                infoRow.appendChild(rssiSpan);
                                infoRow.appendChild(timeSpan);

                                if (device.filter) {
                                    const filterSpan = document.createElement('span');
                                    filterSpan.className = 'device-filter';
                                    filterSpan.textContent = device.filter;
                                    filterSpan.title = device.filter;
                                    infoRow.appendChild(filterSpan);
                                }
                                
                                // Second row: alias input and button
                                const aliasRow = document.createElement('div');
                                aliasRow.className = 'device-alias-row';
                                
                                const aliasInput = document.createElement('input');
                                aliasInput.type = 'text';
                                aliasInput.className = 'alias-input';
                                aliasInput.placeholder = 'Device identification label';
                                aliasInput.value = device.alias || '';
                                aliasInput.maxLength = 32;
                                
                                const saveBtn = document.createElement('button');
                                saveBtn.type = 'button';
                                saveBtn.className = 'save-alias-btn';
                                saveBtn.textContent = 'Save';
                                saveBtn.onclick = function() {
                                    saveAlias(device.mac, aliasInput.value, saveBtn);
                                };
                                
                                aliasRow.appendChild(aliasInput);
                                aliasRow.appendChild(saveBtn);
                                
                                deviceItem.appendChild(infoRow);
                                deviceItem.appendChild(aliasRow);
                                
                                deviceList.appendChild(deviceItem);
                            });
                        }
                    })
                    .catch(error => {
                        console.error('Error loading devices:', error);
                    });
            }
            
            // Mode 1's persisted schema stores match_method as a long-form
            // label ("oui_prefix" / "full_mac" / etc). The badge palette is
            // keyed off the short codes the standalone firmware uses; map
            // between the two so both live and previous rows render.
            function mapMatchMethod(mm) {
                if (!mm) return null;
                var s = String(mm).toLowerCase();
                // Check meta_composite BEFORE the generic 'name' probe so a
                // Meta hit that fell through the name branch doesn't render
                // as a plain NAME badge.
                if (s.indexOf('meta') >= 0) return 'META';
                if (s.indexOf('oui') >= 0) return 'OUI';
                if (s.indexOf('full_mac') >= 0 || s === 'mac') return 'MAC';
                if (s.indexOf('company') >= 0 || s === 'cid') return 'CID';
                if (s.indexOf('service') >= 0 || s === 'svc') return 'SVC';
                if (s.indexOf('name') >= 0) return 'NAME';
                var allowed = ['OUI','MAC','CID','SVC','NAME','META'];
                var up = String(mm).toUpperCase();
                return allowed.indexOf(up) >= 0 ? up : null;
            }

            function makeMatchBadge(type, description, matchIdent) {
                var badge = document.createElement('span');
                var t = mapMatchMethod(type) || 'OUI';
                badge.className = 'match-badge type-' + t;
                badge.textContent = t;
                var titleParts = [];
                if (matchIdent) titleParts.push(matchIdent);
                if (description) titleParts.push(description);
                badge.title = titleParts.join(' - ');
                return badge;
            }

            function togglePrevSession() {
                var list = document.getElementById('previousSessionList');
                var caret = document.getElementById('previousSessionCaret');
                if (!list) return;
                if (list.style.display === 'none') {
                    list.style.display = '';
                    caret.textContent = '[-]';
                } else {
                    list.style.display = 'none';
                    caret.textContent = '[+]';
                }
            }

            function loadPreviousSession() {
                fetch('/api/session/previous')
                    .then(function(r) { return r.json(); })
                    .then(function(arr) {
                        var panel = document.getElementById('previousSessionPanel');
                        var list  = document.getElementById('previousSessionList');
                        var title = document.getElementById('previousSessionTitle');
                        if (!Array.isArray(arr) || arr.length === 0) {
                            if (panel) panel.style.display = 'none';
                            return;
                        }
                        panel.style.display = '';
                        title.textContent = 'PREVIOUS SESSION (' + arr.length + ')';
                        list.innerHTML = '';
                        arr.forEach(function(entry) {
                            var item = document.createElement('div');
                            item.className = 'device-item';
                            var row = document.createElement('div');
                            row.className = 'device-info-row';

                            var macSpan = document.createElement('span');
                            macSpan.className = 'device-mac';
                            macSpan.textContent = entry.mac_address || entry.mac || '?';
                            row.appendChild(macSpan);

                            var mm = entry.match_method || entry.type;
                            var sig = entry.matched_signature || entry.desc || '';
                            row.appendChild(makeMatchBadge(mm, sig, entry.matched_signature || entry.match));

                            var rssi = (typeof entry.rssi_max === 'number') ? entry.rssi_max
                                     : (typeof entry.rssi === 'number' ? entry.rssi : null);
                            if (rssi !== null) {
                                var rs = document.createElement('span');
                                rs.className = 'device-rssi';
                                rs.textContent = rssi + ' dBm';
                                row.appendChild(rs);
                            }

                            var prevTag = document.createElement('span');
                            prevTag.className = 'prev-tag';
                            prevTag.textContent = 'PREV';
                            row.appendChild(prevTag);

                            if (sig) {
                                var descSpan = document.createElement('span');
                                descSpan.className = 'device-filter';
                                descSpan.textContent = sig;
                                descSpan.title = sig;
                                row.appendChild(descSpan);
                            }
                            item.appendChild(row);
                            list.appendChild(item);
                        });
                    })
                    .catch(function(err) {
                        console.error('prev session load failed', err);
                    });
            }

            function clearPreviousSession() {
                fetch('/api/session/clear_previous', { method: 'POST' })
                    .then(function() {
                        var panel = document.getElementById('previousSessionPanel');
                        if (panel) panel.style.display = 'none';
                    })
                    .catch(function(err) { console.error(err); });
            }

            function saveAlias(mac, alias, button) {
                const originalText = button.textContent;
                const originalBg = button.style.background;
                button.textContent = 'Saving...';
                button.disabled = true;
                button.style.opacity = '0.6';
                
                fetch('/api/alias', {
                    method: 'POST',
                    headers: {
                        'Content-Type': 'application/x-www-form-urlencoded',
                    },
                    body: 'mac=' + encodeURIComponent(mac) + '&alias=' + encodeURIComponent(alias)
                })
                .then(response => response.json())
                .then(data => {
                    button.textContent = 'Saved!';
                    button.style.background = 'linear-gradient(135deg, #10b981 0%, #059669 100%)';
                    button.style.opacity = '1';
                    setTimeout(() => {
                        button.textContent = originalText;
                        button.style.background = originalBg;
                        button.disabled = false;
                    }, 2000);
                })
                .catch(error => {
                    console.error('Error saving alias:', error);
                    button.textContent = 'Error';
                    button.style.background = 'linear-gradient(135deg, #ef4444 0%, #dc2626 100%)';
                    button.style.opacity = '1';
                    setTimeout(() => {
                        button.textContent = originalText;
                        button.style.background = originalBg;
                        button.disabled = false;
                    }, 2000);
                });
            }
            
            function clearDeviceHistory() {
                if (confirm('CLEAR DEVICE HISTORY\n\nThis will remove all detected device records from non-volatile storage.\n\nAliases and filter configurations will be preserved.\n\nProceed with clearing device history?')) {
                    fetch('/api/clear-devices', { method: 'POST' })
                        .then(response => response.json())
                        .then(data => {
                            alert('Device history cleared from storage.');
                            location.reload();
                        })
                        .catch(error => {
                            console.error('Error:', error);
                            alert('Error clearing device history.');
                        });
                }
            }
            
            function applyPreset(name, label) {
                var statusEl = document.getElementById('presetStatus');
                statusEl.textContent = 'Adding ' + label + '…';
                statusEl.style.color = '#aaddff';
                var body = 'name=' + encodeURIComponent(name);
                fetch('/api/presets/apply', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
                    body: body
                })
                .then(function(r) { return r.json(); })
                .then(function(data) {
                    if (data.ok) {
                        statusEl.style.color = '#88ee88';
                        if (data.added > 0) {
                            statusEl.textContent = 'Added ' + data.added + ' new filter(s) for ' + label + '. Total filters: ' + data.total_filters + '.';
                        } else {
                            statusEl.textContent = label + ' preset already installed — no changes.';
                        }
                    } else {
                        statusEl.style.color = '#ff7777';
                        statusEl.textContent = 'Error: ' + (data.error || 'unknown');
                    }
                })
                .catch(function(err) {
                    statusEl.style.color = '#ff7777';
                    statusEl.textContent = 'Preset request failed: ' + err;
                });
            }

            var VENDOR_OUIS = {
                axon:   '00:25:DF'
            };
            var VENDOR_LABELS = { axon: 'AXON', meta: 'META / RAY-BAN' };

            // Repopulate the signature lines on page load. Without this the
            // filters stay installed in NVS but the UI looks empty after a
            // refresh, which reads as "my presets vanished".
            function loadSigLines() {
                fetch('/api/presets/status')
                    .then(function(r){ return r.json(); })
                    .then(function(d){
                        Object.keys(VENDOR_LABELS).forEach(function(k){
                            if (d && d[k]) renderSigLine(k, VENDOR_LABELS[k], VENDOR_OUIS[k], null);
                        });
                    })
                    .catch(function(){ /* device offline: leave lines empty */ });
            }

            var VENDOR_SIGS = {
                axon:   [ {t:'cid',  v:'0x034D', l:'CID'},
                          {t:'uuid', v:'0xFC81', l:'UUID'} ],
                meta:   [ {t:'meta', v:'0x0D53+0xFD5F', l:'COMPOSITE'} ]
            };

            function addVendor(preset, label, ouiStr) {
                if (ouiStr) appendOUIs(ouiStr);          // manual box still authoritative for MACs
                fetch('/api/presets/apply', {
                    method: 'POST',
                    headers: {'Content-Type':'application/x-www-form-urlencoded'},
                    body: 'name=' + encodeURIComponent(preset)
                })
                .then(function(r){ return r.json(); })
                .then(function(d){
                    renderSigLine(preset, label, ouiStr, d && d.ok ? null : (d && d.error) || 'failed');
                })
                .catch(function(e){ renderSigLine(preset, label, ouiStr, String(e)); });
            }

            function renderSigLine(preset, label, ouiStr, err) {
                var box = document.getElementById('sigLines');
                if (!box) return;
                var existing = document.getElementById('sig-' + preset);
                if (existing) existing.remove();

                var parts = [];
                (ouiStr ? ouiStr.split(',') : []).forEach(function(o){
                    parts.push('<span class="sig-mac">' + o.trim() + '</span>');
                });
                (VENDOR_SIGS[preset] || []).forEach(function(sig){
                    parts.push('<span class="sig-' + sig.t + '">' + sig.l + ' ' + sig.v + '</span>');
                });

                var row = document.createElement('div');
                row.className = 'sig-line';
                row.id = 'sig-' + preset;
                row.innerHTML =
                    '<span class="sig-vendor">' + label + '</span>' +
                    parts.join('<span class="sig-sep">,</span> ') +
                    (err ? ' <span style="color:#ff6b6b">(' + err + ')</span>' : '') +
                    '<button type="button" class="sig-rm" title="Remove signatures" ' +
                        'onclick="removeVendor(\'' + preset + '\')">&times;</button>';
                box.appendChild(row);
            }

            function removeVendor(preset) {
                fetch('/api/presets/remove', {
                    method: 'POST',
                    headers: {'Content-Type':'application/x-www-form-urlencoded'},
                    body: 'name=' + encodeURIComponent(preset)
                }).then(function(){
                    var el = document.getElementById('sig-' + preset);
                    if (el) el.remove();
                });
            }

            function appendOUIs(ouiStr) {
                var ta = document.getElementById('ouis');
                var current = ta.value.trim();
                var existing = current ? current.split('\n').map(function(s){return s.trim();}).filter(Boolean) : [];
                var toAdd = ouiStr.split(',').map(function(s){return s.trim();}).filter(Boolean);
                var added = 0;
                toAdd.forEach(function(oui) {
                    if (existing.indexOf(oui) === -1) { existing.push(oui); added++; }
                });
                ta.value = existing.join('\n');
                ta.style.borderColor = '#10b981';
                ta.style.boxShadow = '0 0 0 3px rgba(16,185,129,0.3)';
                setTimeout(function(){ ta.style.borderColor = ''; ta.style.boxShadow = ''; }, 1500);
                ta.scrollIntoView({behavior:'smooth',block:'center'});
            }

            function clearConfig() {
                if (confirm('Are you sure you want to clear all filters? This action cannot be undone.')) {
                    document.getElementById('ouis').value = '';
                    document.getElementById('macs').value = '';
                    fetch('/clear', { method: 'POST' })
                        .then(response => response.text())
                        .then(data => {
                            alert('All filters cleared!');
                            location.reload();
                        })
                        .catch(error => {
                            console.error('Error:', error);
                            alert('Error clearing filters. Check console.');
                        });
                }
            }
            
            function deviceReset() {
                if (confirm('DEVICE RESET: This will completely wipe all saved data and restart the device. Are you absolutely sure?')) {
                    if (confirm('This action cannot be undone. The device will restart and behave like first boot. Continue?')) {
                        fetch('/device-reset', { method: 'POST' })
                            .then(response => response.text())
                            .then(data => {
                                alert('Device reset initiated! Device restarting...');
                                setTimeout(function() {
                                    window.location.href = '/';
                                }, 5000);
                            })
                            .catch(error => {
                                console.error('Error:', error);
                                alert('Error during device reset. Check console.');
                            });
                    }
                }
            }
            
            function burnInConfig() {
                if (!confirm('PERMANENT CONFIGURATION LOCK\n\nThis will PERMANENTLY lock all settings (OUI/MAC filters, aliases, buzzer/LED preferences).\n\nAfter activation:\n- WiFi AP and config window disabled on boot\n- Device boots directly to scanning mode\n- Unlock: hold BOOT during power-on (or erase flash + reflash)\n\nClick OK to proceed with permanent lock.')) {
                    return;
                }
                
                // Collect current form values
                const formData = new URLSearchParams();
                const ouisElement = document.getElementById('ouis');
                const macsElement = document.getElementById('macs');
                const ouis = ouisElement ? ouisElement.value.trim() : '';
                const macs = macsElement ? macsElement.value.trim() : '';
                const buzzerEnabled = document.getElementById('buzzerEnabled') ? document.getElementById('buzzerEnabled').checked : true;
                const ledEnabled = document.getElementById('ledEnabled') ? document.getElementById('ledEnabled').checked : true;
                const apSSID = document.getElementById('ap_ssid') ? document.getElementById('ap_ssid').value : '';
                const apPassword = document.getElementById('ap_password') ? document.getElementById('ap_password').value : '';
                
                // Debug logging
                console.log('Burn-in: OUI values:', ouis);
                console.log('Burn-in: MAC values:', macs);
                
                formData.append('ouis', ouis);
                formData.append('macs', macs);
                if (buzzerEnabled) formData.append('buzzerEnabled', 'on');
                if (ledEnabled) formData.append('ledEnabled', 'on');
                formData.append('ap_ssid', apSSID);
                formData.append('ap_password', apPassword);
                
                // User confirmed, proceed with burn-in - send current form values
                fetch('/api/lock-config', { 
                    method: 'POST',
                    headers: {
                        'Content-Type': 'application/x-www-form-urlencoded',
                    },
                    body: formData.toString()
                })
                    .then(response => response.text())
                    .then(data => {
                        // Response is HTML that shows the success page
                        document.open();
                        document.write(data);
                        document.close();
                    })
                    .catch(error => {
                        console.error('Error:', error);
                        alert('Error locking configuration. Check console.');
                    });
            }
                        // populate preset signature lines on load
            loadSigLines();
        </script>
        </form>
    </div>
</body>
</html>
)html";
}

String generateRandomOUI() {
    String oui = "";
    for (int i = 0; i < 3; i++) {
        if (i > 0) oui += ":";
        int val = random(0, 256);
        if (val < 16) oui += "0";
        oui += String(val, HEX);
    }
    oui.toLowerCase();
    return oui;
}

String generateRandomMAC() {
    String mac = "";
    for (int i = 0; i < 6; i++) {
        if (i > 0) mac += ":";
        int val = random(0, 256);
        if (val < 16) mac += "0";
        mac += String(val, HEX);
    }
    mac.toLowerCase();
    return mac;
}

String generateConfigHTML() {
    String html = getConfigHTML();
    String ouiValues = "";
    String macValues = "";
    
    // Populate existing saved values (if any)
    for (const TargetFilter& filter : targetFilters) {
        if (filter.isFullMAC) {
            if (macValues.length() > 0) macValues += "\n";
            macValues += filter.identifier;
        } else {
            if (ouiValues.length() > 0) ouiValues += "\n";
            ouiValues += filter.identifier;
        }
    }
    
    // Generate random examples for placeholders
    String randomOUIExamples = generateRandomOUI() + "\n" + generateRandomOUI() + "\n" + generateRandomOUI();
    String randomMACExamples = generateRandomMAC() + "\n" + generateRandomMAC() + "\n" + generateRandomMAC();
    
    // Replace static placeholders with random examples
    html.replace("AA:BB:CC\nDD:EE:FF\n11:22:33", randomOUIExamples);
    html.replace("AA:BB:CC:12:34:56\nDD:EE:FF:ab:cd:ef\n11:22:33:44:55:66", randomMACExamples);
    
    // Remove ASCII art - causes memory exhaustion on ESP32
    html.replace("%ASCII_ART%", "");
    
    html.replace("%OUI_VALUES%", ouiValues);
    html.replace("%MAC_VALUES%", macValues);
    
    // Replace toggle states
    html.replace("%BUZZER_CHECKED%", buzzerEnabled ? "checked" : "");
    html.replace("%LED_CHECKED%", ledEnabled ? "checked" : "");
    
    // Replace WiFi credentials
    html.replace("%AP_SSID%", AP_SSID);
    html.replace("%AP_PASSWORD%", AP_PASSWORD);
    
    return html;
}

// ================================
// WiFi and Web Server Functions
// ================================
void startConfigMode() {
    currentMode = CONFIG_MODE;
    // configStartTime will be set AFTER AP is fully ready
    
    Serial.println("\n=== STARTING CONFIG MODE ===");
    Serial.println("SSID: " + AP_SSID);
    Serial.println("Password: " + AP_PASSWORD);
    Serial.println("Initializing WiFi AP...");
    
    // Ensure WiFi is off first
    WiFi.mode(WIFI_OFF);
    delay(1000);
    
    // Start WiFi AP
    Serial.println("Setting WiFi mode to AP...");
    WiFi.mode(WIFI_AP);
    delay(500);
    
    Serial.println("Creating access point...");
    bool apStarted = WiFi.softAP(AP_SSID.c_str(), AP_PASSWORD.c_str());
    
    if (apStarted) {
        Serial.println("✓ Access Point created successfully!");
    } else {
        Serial.println("✗ Failed to create Access Point!");
        return;
    }
    
    delay(2000); // Give AP time to fully initialize
    
    IPAddress IP = WiFi.softAPIP();
    Serial.println("AP IP address: " + IP.toString());
    Serial.println("Config portal: http://" + IP.toString());

    // Captive portal DNS - redirect all DNS queries to our AP IP
    detectorDNS.start(53, "*", IP);
    Serial.println("Captive portal DNS started");
    Serial.println("==============================\n");
    
    // NOW start the countdown - AP is fully ready and visible
    configStartTime = millis();
    lastConfigActivity = millis();
    
    // Setup web server routes
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        lastConfigActivity = millis();
        request->send(200, "text/html", generateConfigHTML());
    });
    
    server.on("/save", HTTP_POST, [](AsyncWebServerRequest *request) {
        lastConfigActivity = millis();
        
        if (isSerialConnected()) {
            Serial.println("\n=== WEB CONFIG SUBMISSION ===");
        }

        // The textareas only speak MAC/OUI. Preserve any preset-installed
        // filters (company IDs, service UUIDs, name substrings) so the
        // /save round-trip doesn't nuke them.
        targetFilters.erase(
            std::remove_if(targetFilters.begin(), targetFilters.end(),
                [](const TargetFilter& f) {
                    return f.type == FT_MAC_PREFIX || f.type == FT_FULL_MAC;
                }),
            targetFilters.end());

        // Process OUI entries
        if (request->hasParam("ouis", true)) {
            String ouiData = request->getParam("ouis", true)->value();
            ouiData.trim();
            
            if (ouiData.length() > 0) {
                // Split by newlines and process each OUI
                int start = 0;
                int end = ouiData.indexOf('\n');
                
                while (start < ouiData.length()) {
                    String oui;
                    if (end == -1) {
                        oui = ouiData.substring(start);
                        start = ouiData.length();
                    } else {
                        oui = ouiData.substring(start, end);
                        start = end + 1;
                        end = ouiData.indexOf('\n', start);
                    }
                    
                    oui.trim();
                    oui.replace("\r", ""); // Remove carriage returns
                    
                    if (oui.length() > 0 && isValidMAC(oui)) {
                        TargetFilter filter;
                        filter.identifier = oui;
                        filter.description = "OUI: " + oui;
                        filter.isFullMAC = false;
                        filter.type = FT_MAC_PREFIX;
                        targetFilters.push_back(filter);
                    }
                }
            }
        }
        
        // Process MAC address entries
        if (request->hasParam("macs", true)) {
            String macData = request->getParam("macs", true)->value();
            macData.trim();
            
            if (macData.length() > 0) {
                // Split by newlines and process each MAC
                int start = 0;
                int end = macData.indexOf('\n');
                
                while (start < macData.length()) {
                    String mac;
                    if (end == -1) {
                        mac = macData.substring(start);
                        start = macData.length();
                    } else {
                        mac = macData.substring(start, end);
                        start = end + 1;
                        end = macData.indexOf('\n', start);
                    }
                    
                    mac.trim();
                    mac.replace("\r", ""); // Remove carriage returns
                    
                    if (mac.length() > 0 && isValidMAC(mac)) {
                        TargetFilter filter;
                        filter.identifier = mac;
                        filter.description = "MAC: " + mac;
                        filter.isFullMAC = true;
                        filter.type = FT_FULL_MAC;
                        targetFilters.push_back(filter);
                    }
                }
            }
        }
        
        // Process buzzer and LED toggles
        buzzerEnabled = request->hasParam("buzzerEnabled", true);
        ledEnabled = request->hasParam("ledEnabled", true);
        
        // Process WiFi credentials
        if (request->hasParam("ap_ssid", true)) {
            String newSSID = request->getParam("ap_ssid", true)->value();
            newSSID.trim();
            if (newSSID.length() > 0 && newSSID.length() <= 32) {
                AP_SSID = newSSID;
            }
        }
        
        if (request->hasParam("ap_password", true)) {
            String newPassword = request->getParam("ap_password", true)->value();
            newPassword.trim();
            // Allow empty password for open network, or 8-63 chars
            if (newPassword.length() == 0 || (newPassword.length() >= 8 && newPassword.length() <= 63)) {
                AP_PASSWORD = newPassword;
            }
        }
        
        // Save WiFi credentials
        saveWiFiCredentials();
        
        if (isSerialConnected()) {
            Serial.println("Buzzer enabled: " + String(buzzerEnabled ? "Yes" : "No"));
            Serial.println("LED enabled: " + String(ledEnabled ? "Yes" : "No"));
            Serial.println("WiFi SSID: " + AP_SSID);
            Serial.println("WiFi Password: " + String(AP_PASSWORD.length() > 0 ? "********" : "(Open Network)"));
        }
        
        if (targetFilters.size() > 0) {
            saveConfiguration();
            
            if (isSerialConnected()) {
                Serial.println("Saved " + String(targetFilters.size()) + " filters:");
                for (const TargetFilter& filter : targetFilters) {
                    String type = filter.isFullMAC ? "Full MAC" : "OUI";
                    Serial.println("  - " + filter.identifier + " (" + type + ")");
                }
            }
            
            String responseHTML = R"html(
<!DOCTYPE html>
<html>
<head>
    <title>Configuration Saved</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        body { 
            font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; 
            margin: 0; 
            padding: 20px;
            background: #1a1a1a; 
            color: #e0e0e0;
            text-align: center; 
        }
        .container { 
            max-width: 600px; 
            margin: 0 auto; 
            background: #2d2d2d; 
            padding: 40px; 
            border-radius: 12px; 
            box-shadow: 0 4px 20px rgba(0,0,0,0.3); 
        }
        h1 { 
            color: #ffffff; 
            margin-bottom: 30px; 
            font-weight: 300;
        }
        .success { 
            background: #1a4a3a; 
            color: #4ade80; 
            border: 1px solid #166534; 
            padding: 20px; 
            border-radius: 8px; 
            margin: 30px 0; 
        }
        p { 
            line-height: 1.6; 
            margin: 15px 0;
        }
    </style>
    <script>
        setTimeout(function() {
            document.getElementById('countdown').innerHTML = 'Switching to scanning mode now...';
        }, 5000);
    </script>
</head>
<body>
    <div class="container">
        <h1>Configuration Saved</h1>
        <div class="success">
            <p><strong>Saved )html" + String(targetFilters.size()) + R"html( filters successfully!</strong></p>
            <p id="countdown">Switching to scanning mode in 5 seconds...</p>
        </div>
        <p>The device will now start scanning for your configured devices.</p>
        <p>When a match is found, you'll hear the buzzer alerts!</p>
    </div>
</body>
</html>
)html";
            
            request->send(200, "text/html", responseHTML);
            
            // Schedule mode switch for 5 seconds from now
            modeSwitchScheduled = millis() + 5000;
            
            if (isSerialConnected()) {
                Serial.println("Mode switch scheduled for 5 seconds from now");
                Serial.println("==============================\n");
            }
        } else {
            request->send(400, "text/html", "<h1>Error: No valid filters provided</h1>");
        }
    });
    
    server.on("/clear", HTTP_POST, [](AsyncWebServerRequest *request) {
        lastConfigActivity = millis();
        
        // Clear all filters
        targetFilters.clear();
        saveConfiguration();
        
        if (isSerialConnected()) {
            Serial.println("All filters cleared via web interface");
        }
        
        request->send(200, "text/plain", "Filters cleared successfully");
    });
    
    // Device reset - completely wipe saved config and restart
    server.on("/device-reset", HTTP_POST, [](AsyncWebServerRequest *request) {
        lastConfigActivity = millis();
        
        if (isSerialConnected()) {
            Serial.println("DEVICE RESET - Request received, scheduling reset...");
        }
        
        request->send(200, "text/html", 
            "<html><body style='background:#1a1a1a;color:#e0e0e0;font-family:Arial;text-align:center;padding:50px;'>"
            "<h1>Device Reset Complete</h1>"
            "<p>Device restarting in 3 seconds...</p>"
            "<script>setTimeout(function(){window.location.href='/';}, 5000);</script>"
            "</body></html>");
        
        // Just schedule device reset - do all clearing in main loop
        deviceResetScheduled = millis() + 3000;
    });
    
    // API endpoint to get detected devices
    server.on("/api/devices", HTTP_GET, [](AsyncWebServerRequest *request) {
        lastConfigActivity = millis();
        
        String json = "{\"devices\":[";
        
        unsigned long currentTime = millis();
        
        for (size_t i = 0; i < devices.size(); i++) {
            if (i > 0) json += ",";
            
            String alias = getDeviceAlias(devices[i].macAddress);
            String filterDesc = devices[i].filterDescription;
            if (filterDesc.length() == 0 && devices[i].matchedFilter) {
                filterDesc = String(devices[i].matchedFilter);
            }
            
            // Calculate time since last seen
            unsigned long timeSince = (currentTime >= devices[i].lastSeen) ? 
                                     (currentTime - devices[i].lastSeen) : 0;
            
            json += "{";
            json += "\"mac\":\"" + devices[i].macAddress + "\",";
            json += "\"rssi\":" + String(devices[i].rssi) + ",";
            json += "\"filter\":\"" + filterDesc + "\",";
            json += "\"type\":\"" + String(filterTypeCode(devices[i].matchedType)) + "\",";
            json += "\"match\":\"" + devices[i].matchedIdentifier + "\",";
            json += "\"alias\":\"" + alias + "\",";
            json += "\"lastSeen\":" + String(devices[i].lastSeen) + ",";
            json += "\"timeSince\":" + String(timeSince);
            json += "}";
        }
        
        json += "],";
        json += "\"currentTime\":" + String(currentTime);
        json += "}";
        
        request->send(200, "application/json", json);
    });
    
    // API endpoint to save device alias
    server.on("/api/alias", HTTP_POST, [](AsyncWebServerRequest *request) {
        lastConfigActivity = millis();
        
        if (request->hasParam("mac", true) && request->hasParam("alias", true)) {
            String mac = request->getParam("mac", true)->value();
            String alias = request->getParam("alias", true)->value();
            
            setDeviceAlias(mac, alias);
            saveDeviceAliases();
            
            if (isSerialConnected()) {
                if (alias.length() > 0) {
                    Serial.println("Alias saved: " + mac + " -> \"" + alias + "\"");
                } else {
                    Serial.println("Alias removed: " + mac);
                }
            }
            
            request->send(200, "application/json", "{\"success\":true}");
        } else {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"Missing parameters\"}");
        }
    });
    
    // API endpoint to clear device history
    server.on("/api/clear-devices", HTTP_POST, [](AsyncWebServerRequest *request) {
        lastConfigActivity = millis();
        
        clearDetectedDevices();
        
        if (isSerialConnected()) {
            Serial.println("Device history cleared via web interface");
        }
        
        request->send(200, "application/json", "{\"success\":true}");
    });
    
    // API endpoint to lock/burn-in configuration
    server.on("/api/lock-config", HTTP_POST, [](AsyncWebServerRequest *request) {
        lastConfigActivity = millis();
        
        if (isSerialConnected()) {
            Serial.println("======================================");
            Serial.println("CONFIGURATION LOCK REQUESTED");
            Serial.println("Saving current form values before locking...");
            Serial.println("======================================");
        }
        
        // Process and save current form values (same logic as /save endpoint)
        // Only drop the MAC/OUI-backed filters — those are the ones the
        // textareas own. Preset-installed filters (company ID, service UUID,
        // name substring) have no textarea representation, so clearing the
        // whole vector here would silently delete them on burn-in.
        targetFilters.erase(
            std::remove_if(targetFilters.begin(), targetFilters.end(),
                [](const TargetFilter& f) {
                    return f.type == FT_MAC_PREFIX || f.type == FT_FULL_MAC;
                }),
            targetFilters.end());
        
        // Process OUI entries
        if (request->hasParam("ouis", true)) {
            String ouiData = request->getParam("ouis", true)->value();
            ouiData.trim();
            
            if (isSerialConnected()) {
                Serial.println("Received OUI data length: " + String(ouiData.length()));
                Serial.println("OUI data: [" + ouiData + "]");
            }
            
            if (ouiData.length() > 0) {
                // Split by newlines and process each OUI
                int start = 0;
                int end = ouiData.indexOf('\n');
                
                while (start < ouiData.length()) {
                    String oui;
                    if (end == -1) {
                        oui = ouiData.substring(start);
                        start = ouiData.length();
                    } else {
                        oui = ouiData.substring(start, end);
                        start = end + 1;
                        end = ouiData.indexOf('\n', start);
                    }
                    
                    oui.trim();
                    oui.replace("\r", ""); // Remove carriage returns
                    
                    if (oui.length() > 0 && isValidMAC(oui)) {
                        TargetFilter filter;
                        filter.identifier = oui;
                        filter.description = "OUI: " + oui;
                        filter.isFullMAC = false;
                        filter.type = FT_MAC_PREFIX;
                        targetFilters.push_back(filter);
                    }
                }
            }
        }
        
        // Process MAC address entries
        if (request->hasParam("macs", true)) {
            String macData = request->getParam("macs", true)->value();
            macData.trim();
            
            if (isSerialConnected()) {
                Serial.println("Received MAC data length: " + String(macData.length()));
                Serial.println("MAC data: [" + macData + "]");
            }
            
            if (macData.length() > 0) {
                // Split by newlines and process each MAC
                int start = 0;
                int end = macData.indexOf('\n');
                
                while (start < macData.length()) {
                    String mac;
                    if (end == -1) {
                        mac = macData.substring(start);
                        start = macData.length();
                    } else {
                        mac = macData.substring(start, end);
                        start = end + 1;
                        end = macData.indexOf('\n', start);
                    }
                    
                    mac.trim();
                    mac.replace("\r", ""); // Remove carriage returns
                    
                    if (mac.length() > 0 && isValidMAC(mac)) {
                        TargetFilter filter;
                        filter.identifier = mac;
                        filter.description = "MAC: " + mac;
                        filter.isFullMAC = true;
                        filter.type = FT_FULL_MAC;
                        targetFilters.push_back(filter);
                    }
                }
            }
        }
        
        // Process buzzer and LED toggles
        buzzerEnabled = request->hasParam("buzzerEnabled", true);
        ledEnabled = request->hasParam("ledEnabled", true);
        
        // Process WiFi credentials
        if (request->hasParam("ap_ssid", true)) {
            String newSSID = request->getParam("ap_ssid", true)->value();
            newSSID.trim();
            if (newSSID.length() > 0 && newSSID.length() <= 32) {
                AP_SSID = newSSID;
            }
        }
        
        if (request->hasParam("ap_password", true)) {
            String newPassword = request->getParam("ap_password", true)->value();
            newPassword.trim();
            // Allow empty password for open network, or 8-63 chars
            if (newPassword.length() == 0 || (newPassword.length() >= 8 && newPassword.length() <= 63)) {
                AP_PASSWORD = newPassword;
            }
        }
        
        // Save WiFi credentials
        saveWiFiCredentials();
        
        // Save configuration (even if empty - that's what user wants)
        saveConfiguration();
        
        if (isSerialConnected()) {
            Serial.println("Buzzer enabled: " + String(buzzerEnabled ? "Yes" : "No"));
            Serial.println("LED enabled: " + String(ledEnabled ? "Yes" : "No"));
            Serial.println("WiFi SSID: " + AP_SSID);
            Serial.println("WiFi Password: " + String(AP_PASSWORD.length() > 0 ? "********" : "(Open Network)"));
            Serial.println("Saved " + String(targetFilters.size()) + " filters before locking:");
            for (const TargetFilter& filter : targetFilters) {
                String type = filter.isFullMAC ? "Full MAC" : "OUI";
                Serial.println("  - " + filter.identifier + " (" + type + ")");
            }
        }
        
        // Set the lock flag
        preferences.begin("ouispy", false);
        preferences.putBool("configLocked", true);
        preferences.end();
        
        if (isSerialConnected()) {
            Serial.println("Configuration locked successfully!");
            Serial.println("Device will skip config mode on next boot");
            Serial.println("Unlock: hold BOOT at power-on, or erase flash + reflash");
        }
        
        String responseHTML = R"html(
<!DOCTYPE html>
<html>
<head>
    <title>Configuration Locked</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        body { 
            font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; 
            margin: 0; 
            padding: 20px;
            background: linear-gradient(135deg, #1a1a1a 0%, #0a0a0a 100%); 
            color: #e0e0e0;
            text-align: center;
            min-height: 100vh;
            display: flex;
            align-items: center;
            justify-content: center;
        }
        .container { 
            max-width: 750px; 
            margin: 0 auto; 
            background: linear-gradient(135deg, #2d2d2d 0%, #1a1a1a 100%);
            padding: 50px; 
            border-radius: 16px; 
            box-shadow: 0 8px 32px rgba(0,0,0,0.5); 
            border: 2px solid rgba(139, 0, 0, 0.3);
        }
        h1 { 
            color: #ff6b6b; 
            margin-bottom: 30px;
            font-size: 32px;
            font-weight: 600;
            letter-spacing: 1px;
            text-transform: uppercase;
        }
        .warning { 
            background: linear-gradient(135deg, #1a0a0a 0%, #2d0a0a 100%);
            color: #ffcccc; 
            border: 2px solid #8b0000; 
            padding: 25px; 
            border-radius: 10px; 
            margin: 25px 0; 
            font-weight: 500;
            box-shadow: 0 4px 15px rgba(139, 0, 0, 0.3);
        }
        .info {
            background: linear-gradient(135deg, #0a1a0a 0%, #0a2d0a 100%);
            color: #ccffcc; 
            border: 1px solid #166534; 
            padding: 25px; 
            border-radius: 10px; 
            margin: 25px 0;
            box-shadow: 0 2px 10px rgba(22, 101, 52, 0.2);
        }
        p { 
            line-height: 1.8; 
            margin: 15px 0; 
            font-size: 15px;
        }
        .status-item {
            text-align: left;
            padding: 10px 0;
            border-bottom: 1px solid rgba(255, 255, 255, 0.05);
        }
        .status-item:last-child {
            border-bottom: none;
        }
        .countdown {
            font-size: 16px;
            color: #888888;
            margin-top: 30px;
            font-style: italic;
        }
    </style>
</head>
<body>
    <div class="container">
        <h1>Configuration Locked</h1>
        <div class="warning">
            <p style="font-size: 18px; margin-top: 0;"><strong>CONFIGURATION HAS BEEN PERMANENTLY LOCKED</strong></p>
            <p style="margin-bottom: 0;">20-second configuration window has been disabled for all future boots</p>
        </div>
        <div class="info">
            <p style="font-weight: 600; margin-top: 0; color: #4ade80; font-size: 16px; text-transform: uppercase; letter-spacing: 0.5px;">Active Configuration:</p>
            <div class="status-item">Device transitions directly to scanning mode on boot</div>
            <div class="status-item">Current OUI/MAC filters permanently saved to memory</div>
            <div class="status-item">WiFi access point disabled</div>
            <div class="status-item">Web configuration interface disabled</div>
            <div class="status-item">Reduced boot time (approximately 2 seconds)</div>
            <div class="status-item">Optimized power consumption</div>
        </div>
        <div class="warning">
            <p style="font-weight: 600; margin-top: 0; font-size: 16px; text-transform: uppercase;">Unlock Procedure:</p>
            <p style="margin-bottom: 0;">To restore configuration access: power-cycle the device holding the BOOT button for 1.5s. Erasing flash also works.</p>
        </div>
        <p class="countdown">Device will restart and begin scanning in 3 seconds...</p>
        <script>
            setTimeout(function() {
                window.location.href = 'about:blank';
            }, 3000);
        </script>
    </div>
</body>
</html>
)html";
        
        request->send(200, "text/html", responseHTML);
        
        // Schedule normal restart after 3 seconds (NOT factory reset)
        normalRestartScheduled = millis() + 3000;
    });
    
    // One-click add all known signatures for a device family.
    // POST body/query: name=axon | meta
    server.on("/api/presets/apply", HTTP_POST, [](AsyncWebServerRequest *request) {
        lastConfigActivity = millis();

        String presetName;
        if (request->hasParam("name", true))       presetName = request->getParam("name", true)->value();
        else if (request->hasParam("name", false)) presetName = request->getParam("name", false)->value();
        presetName.toLowerCase();

        int added = 0;
        String label;
        if (presetName == "axon") {
            label = "Axon body cam";
            added = applyPreset(PRESET_AXON, PRESET_AXON_COUNT, "Axon body cam");
        } else if (presetName == "meta") {
            label = "Meta glasses";
            added = applyPreset(PRESET_META, PRESET_META_COUNT, "Meta glasses");
        } else {
            request->send(400, "application/json",
                "{\"ok\":false,\"error\":\"unknown preset\"}");
            return;
        }

        String body = "{\"ok\":true,\"preset\":\"" + presetName + "\",\"label\":\"" + label +
                      "\",\"added\":" + String(added) +
                      ",\"total_filters\":" + String(targetFilters.size()) + "}";
        request->send(200, "application/json", body);

        if (isSerialConnected()) {
            Serial.printf("Preset %s applied — %d new filters (total: %u)\n",
                          presetName.c_str(), added, (unsigned)targetFilters.size());
        }
    });

    server.on("/api/presets/remove", HTTP_POST, [](AsyncWebServerRequest *request) {
        lastConfigActivity = millis();
        String n;
        if (request->hasParam("name", true))       n = request->getParam("name", true)->value();
        else if (request->hasParam("name", false)) n = request->getParam("name", false)->value();
        n.toLowerCase();

        int removed = 0;
        if (n == "axon")           removed = removePreset(PRESET_AXON, PRESET_AXON_COUNT);
        else if (n == "meta")      removed = removePreset(PRESET_META, PRESET_META_COUNT);
        else { request->send(400, "application/json", "{\"ok\":false,\"error\":\"unknown preset\"}"); return; }

        request->send(200, "application/json",
            "{\"ok\":true,\"removed\":" + String(removed) +
            ",\"total_filters\":" + String(targetFilters.size()) + "}");
    });

    server.on("/api/presets/status", HTTP_GET, [](AsyncWebServerRequest *request) {
        String body = "{\"axon\":";
        body += presetInstalled(PRESET_AXON, PRESET_AXON_COUNT) ? "true" : "false";
        body += ",\"meta\":";
        body += presetInstalled(PRESET_META, PRESET_META_COUNT) ? "true" : "false";
        body += "}";
        request->send(200, "application/json", body);
    });

    // BLE session subsystem endpoints — same operations as the CMD: serial
    // protocol, surfaced to the on-device dashboard. Read-mostly, no burn-in
    // gate: the underlying data is purely observational.
    bleRegisterWebEndpoints();

    // Captive portal catch-all: redirect any unknown URL to root
    server.onNotFound([](AsyncWebServerRequest *request) {
        request->redirect("http://192.168.4.1/");
    });

    server.begin();

    if (isSerialConnected()) {
        Serial.println("Web server started!");
    }
}

// ================================
// BLE Advertised Device Callback Class
// ================================
class MyAdvertisedDeviceCallbacks: public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice* advertisedDevice) {
        if (currentMode != SCANNING_MODE) return;
        
        String mac = advertisedDevice->getAddress().toString().c_str();
        int rssi = advertisedDevice->getRSSI();
        unsigned long currentMillis = millis();

        String matchedDescription;
        bool matchFound = matchesTargetFilter(advertisedDevice, mac, matchedDescription);

        // Meta / Ray-Ban composite detection runs inside matchesTargetFilter
        // when (and only when) the META preset is installed — no filter, no
        // trigger.

        if (matchFound) {
            // Feed the BLE session subsystem BEFORE the existing beep/flash
            // path so a first-sight JSON line lands on the wire promptly.
            // bleNoteDetection dedups internally so re-hits don't spam.
            bleNoteDetection(advertisedDevice, mac, rssi, matchedDescription);

            bool known = false;
            for (auto& dev : devices) {
                if (dev.macAddress == mac) {
                    known = true;

                    if (dev.inCooldown && currentMillis < dev.cooldownUntil) {
                        return;
                    }

                    if (dev.inCooldown && currentMillis >= dev.cooldownUntil) {
                        dev.inCooldown = false;
                    }

                    unsigned long timeSinceLastSeen = currentMillis - dev.lastSeen;

                    if (timeSinceLastSeen >= 30000) {
                        // Store data for main loop to process
                        detectedMAC = mac;
                        detectedRSSI = rssi;
                        matchedFilter = matchedDescription;
                        matchType = "RE-30s";
                        newMatchFound = true;
                        
                        threeBeeps();
                        dev.inCooldown = true;
                        dev.cooldownUntil = currentMillis + 10000;
                    } else if (timeSinceLastSeen >= 3000) {
                        // Store data for main loop to process
                        detectedMAC = mac;
                        detectedRSSI = rssi;
                        matchedFilter = matchedDescription;
                        matchType = "RE-3s";
                        newMatchFound = true;
                        
                        twoBeeps();
                        dev.inCooldown = true;
                        dev.cooldownUntil = currentMillis + 3000;
                    }

                    dev.lastSeen = currentMillis;
                    break;
                }
            }

            if (!known) {
                DeviceInfo newDev;
                newDev.macAddress = mac;
                newDev.rssi = rssi;
                newDev.firstSeen = currentMillis;
                newDev.lastSeen = currentMillis;
                newDev.inCooldown = false;
                newDev.cooldownUntil = 0;
                newDev.matchedFilter = matchedDescription.c_str();
                newDev.filterDescription = matchedDescription;
                // Second pass to recover the specific filter class + raw
                // identifier for the dashboard match-type badge. Fills a
                // sane default if the resolver can't reproduce the hit.
                {
                    FilterType mt = FT_MAC_PREFIX;
                    String mid;
                    if (resolveMatchedFilterMeta(advertisedDevice, mac, mt, mid)) {
                        newDev.matchedType = mt;
                        newDev.matchedIdentifier = mid;
                    }
                }
                devices.push_back(newDev);

                // Store data for main loop to process
                detectedMAC = mac;
                detectedRSSI = rssi;
                matchedFilter = matchedDescription;
                matchType = "NEW";
                newMatchFound = true;
                
                threeBeeps();
                
                auto& dev = devices.back();
                dev.inCooldown = true;
                dev.cooldownUntil = currentMillis + 3000;
            }
        }
    }
};

void startScanningMode() {
    currentMode = SCANNING_MODE;
    
    // Stop web server, captive portal DNS, and WiFi
    detectorDNS.stop();
    server.end();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    
    if (isSerialConnected()) {
        Serial.println("\n=== STARTING SCANNING MODE ===");
        Serial.println("Configured Filters:");
        for (const TargetFilter& filter : targetFilters) {
            String type = filter.isFullMAC ? "Full MAC" : "OUI";
            Serial.println("- " + filter.identifier + " (" + type + "): " + filter.description);
        }
        Serial.println("==============================\n");
    }
    
    // Initialize BLE (but don't start scanning yet)
    NimBLEDevice::init("");
    delay(1000);
    
    // Setup BLE scanning (but don't start)
    pBLEScan = NimBLEDevice::getScan();
    if (pBLEScan != nullptr) {
        pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
        pBLEScan->setActiveScan(true);
        pBLEScan->setInterval(300);
        pBLEScan->setWindow(200);
    }
    
    // Ready to scan - ascending beeps (no interference possible)
    delay(500);
    ascendingBeeps();
    
    // 2-second pause after ready signal
    delay(2000);
    
    // NOW start BLE scanning - after ready signal is complete
    if (pBLEScan != nullptr) {
        pBLEScan->start(3, nullptr, false);
        
        if (isSerialConnected()) {
            Serial.println("BLE scanning started!");
        }
    }
}



// ================================
// Setup Function
// ================================
void setup() {
    delay(2000);
    
    // Initialize Serial first
    Serial.begin(115200);
    delay(1000);
    
    // Print ASCII art banner
    Serial.println("\n\n");
    Serial.println("        _________        .__                       .__    __________               .__              ");
    Serial.println("        \\_   ___ \\  ____ |  |   ____   ____   ____ |  |   \\______   \\_____    ____ |__| ____        ");
    Serial.println("        /    \\  \\/ /  _ \\|  |  /  _ \\ /    \\_/ __ \\|  |    |     ___/\\__  \\  /    \\|  |/ ___\\       ");
    Serial.println("        \\     \\___(  <_> )  |_(  <_> )   |  \\  ___/|  |__  |    |     / __ \\|   |  \\  \\  \\___       ");
    Serial.println("         \\______  /\\____/|____/\\____/|___|  /\\___  >____/  |____|    (____  /___|  /__/\\___  >      ");
    Serial.println("                \\/                        \\/     \\/                       \\/     \\/        \\/       ");
    Serial.println("             .__                                     .___      __                 __                ");
    Serial.println("  ____  __ __|__|           ____________ ___.__.   __| _/_____/  |_  ____   _____/  |_  ___________ ");
    Serial.println(" /  _ \\|  |  \\  |  ______  /  ___/\\____ <   |  |  / __ |/ __ \\   __\\/ __ \\_/ ___\\   __\\/  _ \\_  __ \\");
    Serial.println("(  <_> )  |  /  | /_____/  \\___ \\ |  |_> >___  | / /_/ \\  ___/|  | \\  ___/\\  \\___|  | (  <_> )  | \\/");
    Serial.println(" \\____/|____/|__|         /____  >|   __// ____| \\____ |\\___  >__|  \\___  >\\___  >__|  \\____/|__|   ");
    Serial.println("                               \\/ |__|   \\/           \\/    \\/          \\/     \\/                   ");
    Serial.println("\n");
    
    // Randomize MAC address on each boot
    uint8_t newMAC[6];
    WiFi.macAddress(newMAC);
    
    Serial.print("Original MAC: ");
    for (int i = 0; i < 6; i++) {
        if (newMAC[i] < 16) Serial.print("0");
        Serial.print(newMAC[i], HEX);
        if (i < 5) Serial.print(":");
    }
    Serial.println();
    
    // STEALTH MODE: Randomize ALL 6 bytes for maximum anonymity
    randomSeed(analogRead(0) + micros()); // Better randomization
    for (int i = 0; i < 6; i++) {
        newMAC[i] = random(0, 256);
    }
    // Ensure it's a valid locally administered address
    newMAC[0] |= 0x02; // Set locally administered bit
    newMAC[0] &= 0xFE; // Clear multicast bit
    
    // Set the randomized MAC for both STA and AP modes
    WiFi.mode(WIFI_STA);
    esp_wifi_set_mac(WIFI_IF_STA, newMAC);
    
    Serial.print("Randomized MAC: ");
    for (int i = 0; i < 6; i++) {
        if (newMAC[i] < 16) Serial.print("0");
        Serial.print(newMAC[i], HEX);
        if (i < 5) Serial.print(":");
    }
    Serial.println();
    
    // Silence ESP-IDF logs
    esp_log_level_set("*", ESP_LOG_NONE);
    
    initializeBuzzer();
    
    // Test buzzer
    singleBeep();
    delay(500);
    
    initializeNeoPixel();
    
    // Test NeoPixel
    setNeoPixelColor(255, 0, 255); // Bright pink
    delay(1000);
    setNeoPixelColor(128, 0, 255); // Purple
    delay(1000);
    
    // Check for factory reset flag first
    preferences.begin("ouispy", true); // read-only
    bool factoryReset = preferences.getBool("factoryReset", false);
    preferences.end();
    
    if (factoryReset) {
        Serial.println("FACTORY RESET FLAG DETECTED - Clearing all data...");
        
        // Clear the factory reset flag and all data
        preferences.begin("ouispy", false);
        preferences.clear(); // Wipe everything
        preferences.end();
        
        // Clear in-memory data
        targetFilters.clear();
        deviceAliases.clear();
        devices.clear();
        
        Serial.println("Factory reset complete - starting with clean state");
    } else {
        // Load configuration from NVS
        loadConfiguration();
        loadWiFiCredentials();
        loadDeviceAliases();
        loadDetectedDevices();
    }
    
    // Check if configuration is locked/burned in
    preferences.begin("ouispy", true);
    bool configLocked = preferences.getBool("configLocked", false);
    preferences.end();

    // BOOT-button escape hatch. Burn-in is otherwise irreversible: config
    // mode never comes back, so there is no way to reach the dashboard and
    // no way to undo it short of erasing flash. Holding BOOT (GPIO0) for
    // 1.5s during power-on clears the lock. Beeps while counting so the
    // hold is obviously registering; releasing early aborts.
    if (configLocked) {
        pinMode(0, INPUT_PULLUP);
        delay(50);
        if (digitalRead(0) == LOW) {
            Serial.println("BOOT held - keep holding 1.5s to clear config lock...");
            unsigned long t0 = millis();
            bool held = true;
            while (millis() - t0 < 1500) {
                if (digitalRead(0) == HIGH) { held = false; break; }
                if ((millis() - t0) % 300 < 40) {
                    ledcSetup(0, 2000, 8);
                    ledcAttachPin(BUZZER_PIN, 0);
                    ledcWrite(0, 90);
                    delay(30);
                    ledcWrite(0, 0);
                }
                delay(10);
            }
            if (held) {
                preferences.begin("ouispy", false);
                preferences.remove("configLocked");
                preferences.end();
                configLocked = false;
                Serial.println("*** CONFIG LOCK CLEARED - entering config mode ***");
                for (int i = 0; i < 3; i++) {   // triple beep = unlocked
                    ledcSetup(0, 3000, 8);
                    ledcAttachPin(BUZZER_PIN, 0);
                    ledcWrite(0, 110); delay(80);
                    ledcWrite(0, 0);   delay(60);
                }
            } else {
                Serial.println("BOOT released early - lock unchanged");
            }
        }
    }
    
    // BLE session subsystem — init AFTER the factory-reset NVS wipe (they
    // live in different partitions but we want SPIFFS-init failures reported
    // in the same block as everything else). Non-fatal if SPIFFS is missing.
    bleSessionSetup();

    if (configLocked) {
        Serial.println("======================================");
        Serial.println("CONFIGURATION LOCKED (BURNED IN)");
        Serial.println("Skipping config mode - going straight to scanning");
        Serial.println("To unlock: hold BOOT during power-on (or erase flash)");
        Serial.println("======================================");

        // Start scanning immediately
        startScanningMode();
    } else {
        // Start in configuration mode
        Serial.println("Starting configuration mode...");
        startConfigMode();
    }
}

// ================================
// Loop Function
// ================================
void loop() {
    static unsigned long lastScanTime = 0;
    static unsigned long lastCleanupTime = 0;
    static unsigned long lastStatusTime = 0;
    unsigned long currentMillis = millis();

    // BLE session subsystem — always pump the serial CMD: protocol and the
    // autosave tick regardless of config/scanning state so the dashboard can
    // still pull the prior session (or clear it) while the device is sitting
    // in the config-mode captive portal.
    blePollSerialCmd();
    bleAutosaveTick();

    if (currentMode == CONFIG_MODE) {
        detectorDNS.processNextRequest();  // Captive portal DNS
        // Check for scheduled normal restart (from burn-in config)
        if (normalRestartScheduled > 0 && currentMillis >= normalRestartScheduled) {
            if (isSerialConnected()) {
                Serial.println("Scheduled normal restart - rebooting with locked configuration...");
            }
            
            delay(500); // Give time for any pending operations
            ESP.restart(); // Simple restart - settings preserved
        }
        
        // Check for scheduled device reset (from web device reset)
        if (deviceResetScheduled > 0 && currentMillis >= deviceResetScheduled) {
            if (isSerialConnected()) {
                Serial.println("Scheduled device reset - setting factory reset flag and restarting...");
            }
            
            // Just set a factory reset flag - much safer than complex NVS operations
            preferences.begin("ouispy", false);
            preferences.putBool("factoryReset", true);
            preferences.end();
            
            delay(500); // Give time for NVS write
            ESP.restart(); // Restart - clearing will happen safely on boot
        }
        
        // Check for scheduled mode switch (from web config save)
        if (modeSwitchScheduled > 0 && currentMillis >= modeSwitchScheduled) {
            if (isSerialConnected()) {
                Serial.println("Scheduled mode switch - switching to scanning mode");
            }
            modeSwitchScheduled = 0; // Reset
            startScanningMode();
            return;
        }
        
        // Check for config timeout 
        if (targetFilters.size() == 0) {
            // No saved filters - stay in config mode indefinitely
            if (currentMillis - configStartTime > CONFIG_TIMEOUT && lastConfigActivity == configStartTime) {
                if (isSerialConnected()) {
                    Serial.println("No one connected and no saved filters - staying in config mode");
                    Serial.println("Connect to '" + AP_SSID + "' AP to configure your first filters!");
                }
            }
        } else if (targetFilters.size() > 0) {
            // Have saved filters - timeout only if no one connected
            if (currentMillis - configStartTime > CONFIG_TIMEOUT && lastConfigActivity == configStartTime) {
                if (isSerialConnected()) {
                    Serial.println("No one connected within 20s - using saved filters, switching to scanning mode");
                }
                startScanningMode();
            } else if (lastConfigActivity > configStartTime) {
                // Someone connected - wait for them to submit (no timeout)
                if (isSerialConnected() && currentMillis - configStartTime > CONFIG_TIMEOUT) {
                    static unsigned long lastConnectedMsg = 0;
                    if (currentMillis - lastConnectedMsg > 30000) { // Print every 30s
                        Serial.println("Web interface connected - waiting for configuration submission...");
                        lastConnectedMsg = currentMillis;
                    }
                }
            }
        }
        
        // Handle web server
        delay(100);
        return;
    }
    
    // Scanning mode loop
    if (currentMode == SCANNING_MODE) {
        // Handle match detection messages (JSON output for API)
        if (newMatchFound) {
            if (isSerialConnected()) {
                String alias = getDeviceAlias(detectedMAC);
                
                // Output clean JSON
                Serial.print("{\"mac\":\"");
                Serial.print(detectedMAC);
                Serial.print("\",\"alias\":\"");
                Serial.print(alias);
                Serial.print("\",\"rssi\":");
                Serial.print(detectedRSSI);
                Serial.println("}");
            }
            newMatchFound = false;
        }
        
        // Restart BLE scan every 3 seconds
        if (currentMillis - lastScanTime >= 3000) {
            pBLEScan->stop();
            delay(10);
            pBLEScan->start(2, nullptr, false);
            lastScanTime = currentMillis;
        }

        // Auto-save detected devices to NVS every 10 seconds
        if (currentMillis - lastCleanupTime >= 10000) {
            saveDetectedDevices();
            lastCleanupTime = currentMillis;
        }

        // Status report disabled - using JSON output only
        if (currentMillis - lastStatusTime >= 30000) {
            lastStatusTime = currentMillis;
        }
    }
    
    // Update NeoPixel animation
    updateNeoPixelAnimation();
    
    delay(100);
} 