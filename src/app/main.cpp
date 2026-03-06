/*
 * OUI SPY - Unified Firmware Orchestrator
 * colonelpanichacks
 *
 * All detection modules run simultaneously with a single web dashboard.
 * No reboot needed to switch features — everything runs at once.
 * Connect to WiFi AP "oui-spy" and open https://ouispy.local
 *
 * SCAN MODE: One-shot reboot into STA-only WiFi promiscuous mode
 * for full drone Remote ID detection (NAN action frames on channel 6).
 * Triggered via web UI, returns to normal mode on next reboot.
 */

#include <Arduino.h>
#include <ArduinoJson.h>
#include <nvs_flash.h>

// HAL
#include "hal/ble_mgr.h"
#include "hal/buzzer.h"
#include "hal/dns_server.h"
#include "hal/gps.h"
#include "hal/led.h"
#include "hal/mac_util.h"
#include "hal/mdns_setup.h"
#include "hal/neopixel.h"
#include "hal/notify.h"
#include "hal/pins.h"
#include "hal/wifi_mgr.h"

// Modules
#include "modules/detector.h"
#include "modules/flockyou.h"
#include "modules/foxhunter.h"
#include "modules/module.h"
#include "modules/skyspy.h"
#include "modules/wardriver.h"

// Storage
#include "storage/nvs_store.h"

// Web
#include "web/server.h"
#include "web/ws_broadcast.h"

// ============================================================================
// Module Instances
// ============================================================================

static DetectorModule modDetector;
static FoxhunterModule modFoxhunter;
static FlockyouModule modFlockyou;
static SkySpyModule modSkyspy;
static WardriverModule modWardriver;

static IModule* modules[] = {
    &modDetector, &modFoxhunter, &modFlockyou, &modSkyspy, &modWardriver,
};
static const int MODULE_COUNT = sizeof(modules) / sizeof(modules[0]);

// ============================================================================
// Boot Mode
// ============================================================================

static bool g_scanMode = false;

// ============================================================================
// Boot Button (GPIO0) — hold to restart
// ============================================================================

#define BOOT_HOLD_TIME 1500

static unsigned long bootBtnStart = 0;
static bool bootBtnActive = false;

static void checkBootButton() {
    if (digitalRead(BOOT_BUTTON_PIN) == LOW) {
        if (!bootBtnActive) {
            bootBtnActive = true;
            bootBtnStart = millis();
        } else if (millis() - bootBtnStart >= BOOT_HOLD_TIME) {
            Serial.println("\n[OUI-SPY] Boot button held — restarting...");
            hal::notify(hal::NOTIFY_BOOT_HOLD);
            delay(500);
            ESP.restart();
        }
    } else {
        bootBtnActive = false;
    }
}

// ============================================================================
// Load Module Enabled States
// ============================================================================

static void loadModuleStates() {
    for (int i = 0; i < MODULE_COUNT; i++) {
        bool enabled = storage::getModuleEnabled(modules[i]->name());
        modules[i]->setEnabled(enabled);
        Serial.printf("[OUI-SPY] Module '%s': %s\n", modules[i]->name(),
                      enabled ? "ENABLED" : "DISABLED");
    }
}

// ============================================================================
// Scan Mode Setup & Loop
// ============================================================================

static void setupScanMode() {
    Serial.println("\n========================================");
    Serial.println("  OUI SPY — SCAN MODE");
    Serial.println("  WiFi promiscuous ch6 + BLE");
    Serial.println("  Hold BOOT 1.5s to return to normal");
    Serial.println("========================================\n");

    // HAL init
    hal::ledInit();
    hal::buzzerInit();
    hal::neopixelInit();

    // Visual: purple flash
    hal::neopixelSetColor(128, 0, 255);
    delay(300);

    hal::gpsInit();

    // WiFi: STA-only (no AP)
    hal::wifiInitScanMode();

    // BLE init — aggressive scanning (no WiFi AP to share radio with)
    hal::bleInit();
    hal::bleRequestAggressiveScan(true);

    // Only Sky Spy module in scan mode
    modSkyspy.setEnabled(true);
    modSkyspy.setup();
    hal::bleAddListener(&modSkyspy);

    // Enable promiscuous on channel 6 (safe in STA-only mode)
    hal::wifiEnablePromiscuousScanMode(skyspyWiFiCallback, 6);

    // BLE kick off
    hal::bleUpdate();

    // Scan mode notification (audio + blue/cyan animation)
    hal::notify(hal::NOTIFY_SCAN_MODE_BOOT);

    Serial.println("\n========================================");
    Serial.println("  SCAN MODE ACTIVATED");
    Serial.println("  Channel 6 promiscuous + BLE");
    Serial.println("  No web server — hold BOOT to exit");
    Serial.println("========================================\n");
}

static void loopScanMode() {
    checkBootButton();
    hal::gpsUpdate();
    hal::bleUpdate();
    modSkyspy.loop();
    hal::buzzerUpdate();
    hal::neopixelUpdate();

    // Heartbeat
    static unsigned long lastHB = 0;
    if (millis() - lastHB > 5000) {
        Serial.printf("[SCAN-HB] uptime=%lums heap=%u\n", millis(), ESP.getFreeHeap());
        Serial.flush();
        lastHB = millis();
    }

    delay(10);
}

// ============================================================================
// Normal Mode Setup & Loop
// ============================================================================

static void setupNormalMode() {
    Serial.println("\n========================================");
    Serial.println("  OUI SPY UNIFIED FIRMWARE v3.0");
    Serial.println("  All modules running simultaneously");
    Serial.println("========================================\n");

    // HAL init
    hal::ledInit();
    hal::buzzerInit();
    hal::neopixelInit();

    // Test NeoPixel: pink -> purple flash
    hal::neopixelSetColor(255, 0, 255);
    delay(300);
    hal::neopixelSetColor(128, 0, 255);
    delay(300);

    hal::gpsInit();

    // WiFi init (AP+STA)
    String ssid = storage::getAPSSID();
    String pass = storage::getAPPass();
    hal::wifiInit(ssid, pass);

    // DNS Captive Portal + mDNS
    hal::dnsServerStart();
    hal::mdnsInit();

    // BLE init
    hal::bleInit();

    // Load module states
    loadModuleStates();

    // Module setup
    for (int i = 0; i < MODULE_COUNT; i++) {
        if (modules[i]->isEnabled()) {
            Serial.printf("[OUI-SPY] Setting up module: %s\n", modules[i]->name());
            modules[i]->setup();
        }
    }

    // Register BLE listeners
    hal::bleAddListener(&modDetector);
    hal::bleAddListener(&modFoxhunter);
    hal::bleAddListener(&modFlockyou);
    hal::bleAddListener(&modSkyspy);
    hal::bleAddListener(&modWardriver);

    // Web server setup
    web::serverInit();
    web::registerSystemRoutes(modules, MODULE_COUNT);

    // Register each module's web routes on both HTTPS + HTTP servers
    httpd_handle_t https = web::getHTTPSServer();
    httpd_handle_t http = web::getHTTPServer();
    for (int i = 0; i < MODULE_COUNT; i++) {
        modules[i]->registerRoutes(https, http);
    }

    web::serverBegin();

    // BLE kick off
    hal::bleUpdate();

    // Boot jingle
    hal::notify(hal::NOTIFY_BOOT_READY);

    Serial.println("\n========================================");
    Serial.printf("  WiFi AP: %s\n", ssid.c_str());
    Serial.printf("  Dashboard: https://%s\n", hal::wifiGetAPIP().toString().c_str());
    Serial.println("  mDNS: https://ouispy.local");
    Serial.println("  All modules active!");
    Serial.println("========================================\n");
}

static void loopNormalMode() {
    checkBootButton();
    hal::gpsUpdate();
    hal::bleUpdate();

    for (int i = 0; i < MODULE_COUNT; i++) {
        if (modules[i]->isEnabled()) {
            modules[i]->loop();
        }
    }

    hal::buzzerUpdate();
    hal::neopixelUpdate();
    hal::wifiUpdate();

    // Heartbeat (5s interval)
    static unsigned long lastHB = 0;
    if (millis() - lastHB > 5000) {
        Serial.printf("[HEARTBEAT] uptime=%lums heap=%u clients=%d ws=%d\n", millis(),
                      ESP.getFreeHeap(), WiFi.softAPgetStationNum(), ws::clientCount());
        Serial.flush();

        // Push sys/status over WS
        {
            JsonDocument doc;
            doc["uptime"] = millis() / 1000;
            doc["heap"] = ESP.getFreeHeap();
            doc["psram"] = ESP.getFreePsram();
            doc["buzzer"] = hal::buzzerIsEnabled();
            ws::enqueueDoc("sys/status", doc);
        }

        // Push sys/gps over WS
        {
            const hal::GPSData& g = hal::gpsGet();
            JsonDocument doc;
            doc["valid"] = g.valid;
            doc["lat"] = serialized(String(g.lat, 8));
            doc["lon"] = serialized(String(g.lon, 8));
            doc["acc"] = serialized(String(g.accuracy, 1));
            doc["hardware"] = g.isHardware;
            doc["hw_detected"] = g.hwDetected;
            doc["hw_fix"] = g.hwFix;
            doc["sats"] = g.satellites;
            doc["fresh"] = hal::gpsIsFresh();
            ws::enqueueDoc("sys/gps", doc);
        }

        // Push sys/modules over WS
        {
            JsonDocument doc;
            JsonArray arr = doc.to<JsonArray>();
            for (int i = 0; i < MODULE_COUNT; i++) {
                JsonObject obj = arr.add<JsonObject>();
                obj["name"] = modules[i]->name();
                obj["enabled"] = modules[i]->isEnabled();
            }
            ws::enqueueDoc("sys/modules", doc);
        }

        lastHB = millis();
    }

    // DNS captive portal (non-blocking)
    hal::dnsServerLoop();

    // Small yield to prevent WDT
    delay(10);
}

// ============================================================================
// Arduino Entry Points
// ============================================================================

void setup() {
    Serial.begin(115200);
    delay(200);

    // Log reset reason
    esp_reset_reason_t resetReason = esp_reset_reason();
    const char* reasonStr = "UNKNOWN";
    switch (resetReason) {
    case ESP_RST_POWERON:
        reasonStr = "POWER_ON";
        break;
    case ESP_RST_SW:
        reasonStr = "SOFTWARE";
        break;
    case ESP_RST_PANIC:
        reasonStr = "PANIC (crash)";
        break;
    case ESP_RST_INT_WDT:
        reasonStr = "INTERRUPT_WDT";
        break;
    case ESP_RST_TASK_WDT:
        reasonStr = "TASK_WDT";
        break;
    case ESP_RST_WDT:
        reasonStr = "OTHER_WDT";
        break;
    case ESP_RST_BROWNOUT:
        reasonStr = "BROWNOUT";
        break;
    default:
        break;
    }
    Serial.printf("[OUI-SPY] Reset reason: %s (%d)\n", reasonStr, (int)resetReason);

    // Boot button
    pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);

    // Check scan mode flag (one-shot: clear immediately)
    if (storage::getSkyScanRequested()) {
        storage::clearSkyScanRequested();
        g_scanMode = true;
        Serial.println("[OUI-SPY] Scan mode flag detected — entering scan mode");
        setupScanMode();
    } else {
        g_scanMode = false;
        setupNormalMode();
    }
}

void loop() {
    if (g_scanMode) {
        loopScanMode();
    } else {
        loopNormalMode();
    }
}
