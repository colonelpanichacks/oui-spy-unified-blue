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
#include <Adafruit_NeoPixel.h>

// ================================
// Pin and Buzzer Definitions - Xiao ESP32 S3
// ================================
#define BUZZER_FREQ 2000  // Frequency in Hz
#define BUZZER_DUTY 127  // 50% duty cycle for good volume without excessive power draw
#define BEEP_DURATION 200  // Duration of each beep in ms
#define BEEP_PAUSE 50  // Pause between beeps in ms (faster sequence)
#define LED_PIN 21   // GPIO21 for onboard LED (inverted logic)

// Buzzer pin routing: GPIO3 (D2) for the external oui-spy piezo, or GPIO4
// (D3/A3) for the expansion board's passive buzzer when the chassis is present.
// Set in setup() after dashboard_init().
static int buzzerPin = BUZZER_PIN_EXTERNAL;
#define BUZZER_PIN buzzerPin

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
};

struct TargetFilter {
    String identifier;
    bool isFullMAC;
    String description;
};

struct DeviceAlias {
    String macAddress;
    String alias;
};

std::vector<DeviceInfo> devices;
std::vector<TargetFilter> targetFilters;
std::vector<DeviceAlias> deviceAliases;

// Forward declarations
void startScanningMode();
void startDetectionFlash();
class MyAdvertisedDeviceCallbacks;

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
        String keyId = "id_" + String(i);
        String keyMAC = "mac_" + String(i);
        String keyDesc = "desc_" + String(i);
        
        preferences.putString(keyId.c_str(), targetFilters[i].identifier);
        preferences.putBool(keyMAC.c_str(), targetFilters[i].isFullMAC);
        preferences.putString(keyDesc.c_str(), targetFilters[i].description);
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
            String keyId = "id_" + String(i);
            String keyMAC = "mac_" + String(i);
            String keyDesc = "desc_" + String(i);
            
            TargetFilter filter;
            filter.identifier = preferences.getString(keyId.c_str(), "");
            filter.isFullMAC = preferences.getBool(keyMAC.c_str(), false);
            filter.description = preferences.getString(keyDesc.c_str(), "");
            
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

bool matchesTargetFilter(const String& deviceMAC, String& matchedDescription) {
    String normalizedDeviceMAC = deviceMAC;
    normalizeMACAddress(normalizedDeviceMAC);
    
    for (const TargetFilter& filter : targetFilters) {
        String filterID = filter.identifier;
        normalizeMACAddress(filterID);
        
        if (filter.isFullMAC) {
            if (normalizedDeviceMAC.equals(filterID)) {
                matchedDescription = filter.description;
                return true;
            }
        } else {
            if (normalizedDeviceMAC.startsWith(filterID)) {
                matchedDescription = filter.description;
                return true;
            }
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
                        <strong>Unlock:</strong> USB connection, flash erase, then firmware reflash required
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
                        Cannot be undone without flash erase + reflash
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
            </style>
            
            <script>
            // Load detected devices on page load
            window.addEventListener('DOMContentLoaded', function() {
                loadDetectedDevices();
                
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
                if (!confirm('PERMANENT CONFIGURATION LOCK\n\nThis will PERMANENTLY lock all settings (OUI/MAC filters, aliases, buzzer/LED preferences).\n\nAfter activation:\n- WiFi AP and config window disabled on boot\n- Device boots directly to scanning mode\n- Unlock requires: flash erase + firmware reflash via USB\n\nClick OK to proceed with permanent lock.')) {
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
        
        targetFilters.clear();
        
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
        targetFilters.clear();
        
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
            Serial.println("Reflash firmware to unlock");
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
            <p style="margin-bottom: 0;">USB connection required. Must erase flash storage, then reflash firmware to restore configuration access</p>
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
        bool matchFound = matchesTargetFilter(mac, matchedDescription);
        
        if (matchFound) {
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
    
    // Stop web server and WiFi
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
// Expansion Board OLED Dashboard
// ================================
#define DASH_REFRESH_MS 1000
#define DASH_BASELINE 6
#define DASH_ROW_STEP 8

static unsigned long lastDashboardRefresh = 0;

enum DashPage {
    DASH_PAGE_SUMMARY = 0,
    DASH_PAGE_LATEST,
    DASH_PAGE_FLEET,
    DASH_PAGE_FILTERS,
    DASH_PAGE_QR,
    DASH_PAGE_COUNT
};
static uint8_t dashPage = DASH_PAGE_SUMMARY;

static void dashRow(uint8_t row) {
    dashboard_set_cursor(0, DASH_BASELINE + row * DASH_ROW_STEP);
}

static void dashFooter(unsigned long now) {
    dashRow(7);
    dashboard_printf("P%u/%u UP %02lu:%02lu:%02lu",
                     (unsigned)(dashPage + 1), (unsigned)DASH_PAGE_COUNT,
                     (unsigned long)((now / 3600000UL) % 100UL),
                     (unsigned long)((now / 60000UL) % 60UL),
                     (unsigned long)((now / 1000UL) % 60UL));
}

static void dashPageSummary(unsigned long now) {
    dashRow(0);
    dashboard_printf("DETECTOR FLT:%u DEV:%u",
                     (unsigned)targetFilters.size(), (unsigned)devices.size());
    dashboard_draw_hline(0, 7, 128);

    if (currentMode == SCANNING_MODE) {
        dashRow(2);
        dashboard_printf("MODE SCANNING");
        dashRow(3);
        dashboard_printf("WATCHLIST ACTIVE");
    } else {
        dashRow(2);
        dashboard_printf("MODE CONFIG");
        dashRow(3);
        dashboard_printf("WAITING FOR CONFIG");
    }
    dashRow(4);
    dashboard_printf("BZ:%s LED:%s", buzzerEnabled ? "ON" : "OFF",
                     ledEnabled ? "ON" : "OFF");
    dashRow(5);
    dashboard_printf(currentMode == CONFIG_MODE ? "AP %s" : "WIFI OFF",
                     currentMode == CONFIG_MODE ? AP_SSID.c_str() : "");
    if (currentMode == CONFIG_MODE) {
        dashRow(6);
        dashboard_printf("WEB 192.168.4.1");
    }
    dashFooter(now);
}

static void dashPageLatest(unsigned long now) {
    dashRow(0);
    dashboard_printf("LATEST DETECTION");
    dashboard_draw_hline(0, 7, 128);

    if (devices.size() == 0) {
        dashRow(3);
        dashboard_printf("NO DEVICES YET");
        dashRow(4);
        dashboard_printf("CONFIG VIA WEB UI");
        dashFooter(now);
        return;
    }

    const DeviceInfo* latest = &devices[0];
    unsigned long newest = devices[0].lastSeen;
    for (size_t i = 1; i < devices.size(); i++) {
        if (devices[i].lastSeen > newest) {
            newest = devices[i].lastSeen;
            latest = &devices[i];
        }
    }

    dashRow(2);
    dashboard_printf("%s", latest->macAddress.c_str());
    String alias = getDeviceAlias(latest->macAddress);
    if (alias.length() > 0) {
        dashRow(3);
        dashboard_printf("ALIAS %s", alias.c_str());
    }
    dashRow(4);
    dashboard_printf("RSSI %d dBm", latest->rssi);
    dashRow(5);
    dashboard_printf("%.14s", latest->filterDescription.c_str());
    dashRow(6);
    dashboard_printf("SEEN %lus AGO",
                     (unsigned long)((now - latest->lastSeen) / 1000UL));
    dashFooter(now);
}

static void dashPageFleet(unsigned long now) {
    dashRow(0);
    dashboard_printf("DEVICES RSSI TOT:%u", (unsigned)devices.size());
    dashboard_draw_hline(0, 7, 128);

    if (devices.size() == 0) {
        dashRow(3);
        dashboard_printf("NO DETECTIONS YET");
        dashFooter(now);
        return;
    }

    std::vector<const DeviceInfo*> sorted;
    sorted.reserve(devices.size());
    for (const auto& d : devices) sorted.push_back(&d);
    std::sort(sorted.begin(), sorted.end(),
              [](const DeviceInfo* a, const DeviceInfo* b) { return a->rssi > b->rssi; });

    size_t shown = 0;
    for (const DeviceInfo* d : sorted) {
        if (shown >= 5) break;
        dashRow(2 + shown);
        dashboard_printf("%d %s %d", (int)shown + 1,
                         d->macAddress.c_str(), d->rssi);
        shown++;
    }
    if (devices.size() > 5) {
        dashRow(6);
        dashboard_printf("...and %u more", (unsigned)(devices.size() - 5));
    }
    dashFooter(now);
}

static void dashPageFilters(unsigned long now) {
    dashRow(0);
    dashboard_printf("WATCHLIST FLT:%u", (unsigned)targetFilters.size());
    dashboard_draw_hline(0, 7, 128);

    if (targetFilters.size() == 0) {
        dashRow(3);
        dashboard_printf("NO FILTERS");
        dashRow(4);
        dashboard_printf("CONFIGURE VIA WEB UI");
        dashFooter(now);
        return;
    }

    size_t shown = 0;
    for (const auto& f : targetFilters) {
        if (shown >= 5) break;
        const char* type = f.isFullMAC ? "M" : "O";
        dashRow(2 + shown);
        dashboard_printf("%d %s %.12s", (int)shown + 1, type,
                         f.identifier.c_str());
        shown++;
    }
    if (targetFilters.size() > 5) {
        dashRow(6);
        dashboard_printf("...and %u more", (unsigned)(targetFilters.size() - 5));
    }
    dashFooter(now);
}

static void dashPageQR(unsigned long now) {
    char wifiStr[96];
    snprintf(wifiStr, sizeof(wifiStr), "WIFI:T:WPA;S:%s;P:%s;;",
             AP_SSID.c_str(), AP_PASSWORD.c_str());
    if (!dashboard_draw_qrcode(wifiStr)) {
        dashRow(2);
        dashboard_printf("SSID/PASS TOO LONG");
        dashRow(3);
        dashboard_printf("FOR QR CODE");
        dashFooter(now);
    }
}

void renderDashboard() {
    if (!dashboard_present()) return;

    unsigned long now = millis();
    if (now - lastDashboardRefresh < DASH_REFRESH_MS) return;
    lastDashboardRefresh = now;

    dashboard_clear();
    switch (dashPage) {
        case DASH_PAGE_SUMMARY: dashPageSummary(now); break;
        case DASH_PAGE_LATEST:  dashPageLatest(now); break;
        case DASH_PAGE_FLEET:   dashPageFleet(now); break;
        case DASH_PAGE_FILTERS: dashPageFilters(now); break;
        case DASH_PAGE_QR:      dashPageQR(now); break;
        default:                dashPage = DASH_PAGE_SUMMARY; break;
    }
    dashboard_flush();
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
    
    // Probe for the expansion board OLED. Pins 5/6 are free in this mode, so
    // this is safe and a no-op when no display is attached. Also route the
    // buzzer to the chassis buzzer (GPIO4) when the expansion board is present.
    dashboard_init();
    buzzerPin = dashboard_buzzer_pin();
    if (dashboard_present()) {
        dashboard_clear();
        dashRow(0);
        dashboard_printf("DETECTOR");
        dashRow(1);
        dashboard_printf("BLE WATCHLIST ALERT");
        dashRow(2);
        dashboard_printf("MODE 1");
        dashRow(5);
        dashboard_printf("BOOTING...");
        dashboard_flush();
        delay(1200);
    }
    
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
    
    // NeoPixel shares GPIO4 with the expansion board's buzzer, so it is skipped
    // when the chassis (and its buzzer) is present.
    if (!dashboard_present()) {
        initializeNeoPixel();
        
        // Test NeoPixel
        setNeoPixelColor(255, 0, 255); // Bright pink
        delay(1000);
        setNeoPixelColor(128, 0, 255); // Purple
        delay(1000);
    }
    
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
    
    if (configLocked) {
        Serial.println("======================================");
        Serial.println("CONFIGURATION LOCKED (BURNED IN)");
        Serial.println("Skipping config mode - going straight to scanning");
        Serial.println("To enable config mode: reflash firmware");
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
    
    // Expansion board button advances the dashboard page (display only)
    if (dashboard_present() && dashboard_button_pressed()) {
        dashPage++;
        if (dashPage >= DASH_PAGE_COUNT) dashPage = 0;
        lastDashboardRefresh = 0;  // redraw immediately on page change
    }
    renderDashboard();
    
    if (currentMode == CONFIG_MODE) {
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
    
    // Update NeoPixel animation (skipped when the expansion board buzzer owns GPIO4)
    if (!dashboard_present()) updateNeoPixelAnimation();
    
    delay(100);
} 