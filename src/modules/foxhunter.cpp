#include "foxhunter.h"
#include "../hal/ble_mgr.h"
#include "../hal/buzzer.h"
#include "../hal/led.h"
#include "../hal/notify.h"
#include "../storage/nvs_store.h"
#include "../web/routes.h"
#include "../web/ws_broadcast.h"

// ============================================================================
// Foxhunter Module: Single-target RSSI proximity tracker
// ============================================================================

// ============================================================================
// NVS Persistence
// ============================================================================

void FoxhunterModule::loadConfig() {
    _targetMAC = storage::getFoxTargetMAC();
    if (_targetMAC.length() > 0)
        _targetMAC.toUpperCase();
    Serial.printf("[FOXHUNTER] Target: %s\n",
                  _targetMAC.length() > 0 ? _targetMAC.c_str() : "(none)");
}

void FoxhunterModule::saveConfig() {
    storage::setFoxTargetMAC(_targetMAC);
}

// ============================================================================
// Module Implementation
// ============================================================================

void FoxhunterModule::setup() {
    loadConfig();
    if (_targetMAC.length() > 0) {
        // Request aggressive scan for proximity tracking
        hal::bleRequestAggressiveScan(true);
    }
}

void FoxhunterModule::loop() {
    if (!_enabled)
        return;

    unsigned long now = millis();

    // Proximity beeping when target is in range
    if (_targetDetected && (now - _lastTargetSeen < 5000)) {
        hal::buzzerSetProximity(true, _currentRSSI);

        // RSSI serial + WS output every 100ms
        if (now - _lastRSSIPrint >= 100) {
            if (now - _lastRSSIPrint >= 2000) {
                Serial.printf("{\"module\":\"foxhunter\",\"rssi\":%d}\n", _currentRSSI);
            }
            // Push fox/status over WS at 100ms for real-time RSSI meter
            if (ws::hasClients()) {
                char json[128];
                snprintf(json, sizeof(json),
                         "{\"target\":\"%s\",\"detected\":true,\"rssi\":%d,\"lastSeen\":%lu}",
                         _targetMAC.c_str(), _currentRSSI, _lastTargetSeen);
                ws::enqueue(ws::topic::FOX_STATUS, json);
            }
            _lastRSSIPrint = now;
        }
    } else if (_targetDetected && (now - _lastTargetSeen >= 5000)) {
        // Target lost
        _targetDetected = false;
        hal::buzzerSetProximity(false);
        Serial.println("{\"module\":\"foxhunter\",\"status\":\"target_lost\"}");
        {
            char json[128];
            snprintf(json, sizeof(json),
                     "{\"target\":\"%s\",\"detected\":false,\"rssi\":%d,\"lastSeen\":%lu}",
                     _targetMAC.c_str(), _currentRSSI, _lastTargetSeen);
            ws::enqueue(ws::topic::FOX_STATUS, json);
        }
    }
}

void FoxhunterModule::onBLEAdvertisement(const NimBLEAdvertisedDevice* device) {
    if (!_enabled || _targetMAC.length() == 0)
        return;

    String mac = device->getAddress().toString().c_str();
    mac.toUpperCase();

    if (mac == _targetMAC) {
        _currentRSSI = device->getRSSI();
        _lastTargetSeen = millis();

        if (!_targetDetected) {
            _targetDetected = true;
            if (_sessionFirstDetection) {
                hal::notify(hal::NOTIFY_FOX_ACQUIRED);
                _sessionFirstDetection = false;
                Serial.println("{\"module\":\"foxhunter\",\"status\":\"target_acquired\"}");
            }
        }
    }
}

// ============================================================================
// Public Operations (used by route handlers)
// ============================================================================

void FoxhunterModule::setTarget(const String& mac) {
    _targetMAC = mac;
    _targetMAC.trim();
    _targetMAC.toUpperCase();
    saveConfig();
    _targetDetected = false;
    _sessionFirstDetection = true;
    _currentRSSI = -100;
    hal::bleRequestAggressiveScan(_targetMAC.length() > 0);
}

void FoxhunterModule::clearTarget() {
    _targetMAC = "";
    saveConfig();
    _targetDetected = false;
    hal::buzzerSetProximity(false);
    hal::bleRequestAggressiveScan(false);
}

// ============================================================================
// Web Routes
// ============================================================================

void FoxhunterModule::registerRoutes(httpd_handle_t https, httpd_handle_t http) {
    registerFoxhunterRoutes(https, http, *this);
}

bool FoxhunterModule::isEnabled() {
    return _enabled;
}
void FoxhunterModule::setEnabled(bool enabled) {
    _enabled = enabled;
    if (!enabled) {
        hal::buzzerSetProximity(false);
        hal::bleRequestAggressiveScan(false);
    }
}
