#include "detector.h"
#include "../hal/led.h"
#include "../hal/notify.h"
#include "../storage/nvs_store.h"
#include "../web/routes.h"
#include "../web/ws_broadcast.h"
#include "detector_logic.h"
#include <algorithm>

// ============================================================================
// Detector Module: BLE device scanning with OUI/MAC watchlists
// ============================================================================

// ============================================================================
// MAC Utility Functions
// ============================================================================

void DetectorModule::normMAC(String& mac) {
    mac = detector_logic::normalizeMac(mac.c_str()).c_str();
}

bool DetectorModule::isValidMAC(const String& mac) {
    return detector_logic::isValidMac(mac.c_str());
}

bool DetectorModule::matchesFilter(const String& deviceMAC, String& matchedDesc) {
    for (const auto& f : _filters) {
        if (detector_logic::matchesFilter(deviceMAC.c_str(), f.identifier.c_str(), f.isFullMAC)) {
            matchedDesc = f.description;
            return true;
        }
    }
    return false;
}

// ============================================================================
// NVS Persistence
// ============================================================================

void DetectorModule::saveFilters() {
    int count = (int)_filters.size();
    std::vector<String> ids(count), descs(count);
    bool* fullMACs = new bool[count];
    for (int i = 0; i < count; i++) {
        ids[i] = _filters[i].identifier;
        fullMACs[i] = _filters[i].isFullMAC;
        descs[i] = _filters[i].description;
    }
    storage::saveDetFilters(count, ids.data(), fullMACs, descs.data(), _buzzerEnabled, _ledEnabled);
    delete[] fullMACs;
}

void DetectorModule::loadFilters() {
    int count = storage::getDetFilterCount();
    _buzzerEnabled = storage::getDetBuzzerEnabled();
    _ledEnabled = storage::getDetLedEnabled();
    _filters.clear();
    for (int i = 0; i < count; i++) {
        DetTargetFilter f;
        storage::getDetFilter(i, f.identifier, f.isFullMAC, f.description);
        if (f.identifier.length() > 0)
            _filters.push_back(f);
    }
}

void DetectorModule::saveAliases() {
    int count = (int)_aliases.size();
    std::vector<String> macs(count), names(count);
    for (int i = 0; i < count; i++) {
        macs[i] = _aliases[i].macAddress;
        names[i] = _aliases[i].alias;
    }
    storage::saveDetAliases(count, macs.data(), names.data());
}

void DetectorModule::loadAliases() {
    int count = storage::getDetAliasCount();
    _aliases.clear();
    for (int i = 0; i < count; i++) {
        DetDeviceAlias a;
        storage::getDetAlias(i, a.macAddress, a.alias);
        if (a.macAddress.length() > 0 && a.alias.length() > 0)
            _aliases.push_back(a);
    }
}

String DetectorModule::getAlias(const String& mac) {
    String norm = mac;
    normMAC(norm);
    for (const auto& a : _aliases) {
        String n = a.macAddress;
        normMAC(n);
        if (n.equals(norm))
            return a.alias;
    }
    return "";
}

void DetectorModule::setAlias(const String& mac, const String& alias) {
    String norm = mac;
    normMAC(norm);
    if (alias.length() == 0) {
        _aliases.erase(std::remove_if(_aliases.begin(), _aliases.end(),
                                      [&](const DetDeviceAlias& da) {
                                          String nm = da.macAddress;
                                          normMAC(nm);
                                          return nm.equals(norm);
                                      }),
                       _aliases.end());
        return;
    }

    for (auto& a : _aliases) {
        String n = a.macAddress;
        normMAC(n);
        if (n.equals(norm)) {
            a.alias = alias;
            return;
        }
    }

    _aliases.push_back({norm, alias});
}

void DetectorModule::saveDevices() {
    int count = min((int)_devices.size(), 100);
    std::vector<String> macs(count), filters(count);
    std::vector<int> rssis(count);
    std::vector<unsigned long> lastSeens(count);
    for (int i = 0; i < count; i++) {
        macs[i] = _devices[i].macAddress;
        rssis[i] = _devices[i].rssi;
        lastSeens[i] = _devices[i].lastSeen;
        filters[i] = _devices[i].filterDescription;
    }
    storage::saveDetDevices(count, macs.data(), rssis.data(), lastSeens.data(), filters.data());
}

void DetectorModule::loadDevices() {
    int count = storage::getDetDeviceCount();
    _devices.clear();
    for (int i = 0; i < count; i++) {
        DetDeviceInfo d;
        storage::getDetDevice(i, d.macAddress, d.rssi, d.lastSeen, d.filterDescription);
        d.firstSeen = d.lastSeen;
        d.inCooldown = false;
        d.cooldownUntil = 0;
        if (d.macAddress.length() > 0)
            _devices.push_back(d);
    }
}

// ============================================================================
// Module Implementation
// ============================================================================

void DetectorModule::setup() {
    loadFilters();
    loadAliases();
    loadDevices();
    Serial.printf("[DETECTOR] Loaded %d filters, %d aliases, %d devices\n", (int)_filters.size(),
                  (int)_aliases.size(), (int)_devices.size());
}

void DetectorModule::loop() {
    if (!_enabled)
        return;

    // Output JSON for new matches — copy under lock then print outside
    if (_newMatch) {
        char mac[18], type[8];
        int rssi;
        portENTER_CRITICAL(&_matchMux);
        strlcpy(mac, _matchMAC, sizeof(mac));
        rssi = _matchRSSI;
        strlcpy(type, _matchType, sizeof(type));
        _newMatch = false;
        portEXIT_CRITICAL(&_matchMux);
        String alias = getAlias(mac);
        Serial.printf("{\"module\":\"detector\",\"mac\":\"%s\",\"alias\":\"%s\",\"rssi\":%d,"
                      "\"type\":\"%s\"}\n",
                      mac, alias.c_str(), rssi, type);

        // Push detection event over WS (escape user-supplied alias)
        {
            char safeAlias[72];
            ws::jsonEscape(safeAlias, sizeof(safeAlias), alias.c_str());
            char json[256];
            snprintf(json, sizeof(json),
                     "{\"mac\":\"%s\",\"alias\":\"%s\",\"rssi\":%d,\"type\":\"%s\"}", mac,
                     safeAlias, rssi, type);
            ws::enqueue("det/match", json);
        }
    }

    // Auto-save every 10s
    if (millis() - _lastSave >= 10000) {
        saveDevices();
        _lastSave = millis();
    }
}

void DetectorModule::onBLEAdvertisement(const NimBLEAdvertisedDevice* device) {
    if (!_enabled)
        return;
    if (_filters.empty())
        return;

    String mac = device->getAddress().toString().c_str();
    int rssi = device->getRSSI();
    unsigned long now = millis();

    String matchedDesc;
    if (!matchesFilter(mac, matchedDesc))
        return;

    // Check existing devices
    for (auto& dev : _devices) {
        if (dev.macAddress == mac) {
            if (dev.inCooldown && now < dev.cooldownUntil)
                return;
            if (dev.inCooldown && now >= dev.cooldownUntil)
                dev.inCooldown = false;

            unsigned long sinceLast = now - dev.lastSeen;
            if (sinceLast >= 30000) {
                portENTER_CRITICAL(&_matchMux);
                strlcpy(_matchMAC, mac.c_str(), sizeof(_matchMAC));
                _matchRSSI = rssi;
                strlcpy(_matchFilter, matchedDesc.c_str(), sizeof(_matchFilter));
                strlcpy(_matchType, "RE-30s", sizeof(_matchType));
                _newMatch = true;
                portEXIT_CRITICAL(&_matchMux);
                hal::notify(hal::NOTIFY_DET_RE_30S);
                dev.inCooldown = true;
                dev.cooldownUntil = now + 10000;
            } else if (sinceLast >= 3000) {
                portENTER_CRITICAL(&_matchMux);
                strlcpy(_matchMAC, mac.c_str(), sizeof(_matchMAC));
                _matchRSSI = rssi;
                strlcpy(_matchFilter, matchedDesc.c_str(), sizeof(_matchFilter));
                strlcpy(_matchType, "RE-3s", sizeof(_matchType));
                _newMatch = true;
                portEXIT_CRITICAL(&_matchMux);
                hal::notify(hal::NOTIFY_DET_RE_3S);
                dev.inCooldown = true;
                dev.cooldownUntil = now + 3000;
            }
            dev.lastSeen = now;
            dev.rssi = rssi;
            return;
        }
    }

    // New device
    DetDeviceInfo newDev;
    newDev.macAddress = mac;
    newDev.rssi = rssi;
    newDev.firstSeen = now;
    newDev.lastSeen = now;
    newDev.inCooldown = true;
    newDev.cooldownUntil = now + 3000;
    newDev.filterDescription = matchedDesc;
    _devices.push_back(newDev);

    portENTER_CRITICAL(&_matchMux);
    strlcpy(_matchMAC, mac.c_str(), sizeof(_matchMAC));
    _matchRSSI = rssi;
    strlcpy(_matchFilter, matchedDesc.c_str(), sizeof(_matchFilter));
    strlcpy(_matchType, "NEW", sizeof(_matchType));
    _newMatch = true;
    portEXIT_CRITICAL(&_matchMux);
    hal::notify(hal::NOTIFY_DET_NEW_DEVICE);
}

// ============================================================================
// Public Operations (used by route handlers)
// ============================================================================

void DetectorModule::clearDevices() {
    _devices.clear();
    storage::clearDetDevices();
}

void DetectorModule::clearFilters() {
    _filters.clear();
    saveFilters();
}

void DetectorModule::parseFilters(const String& ouis, const String& macs) {
    _filters.clear();
    // OUI entries
    {
        String data = ouis;
        data.trim();
        if (data.length() > 0) {
            int start = 0, end = data.indexOf('\n');
            while (start < (int)data.length()) {
                String line;
                if (end == -1) {
                    line = data.substring(start);
                    start = data.length();
                } else {
                    line = data.substring(start, end);
                    start = end + 1;
                    end = data.indexOf('\n', start);
                }
                line.trim();
                line.replace("\r", "");
                if (line.length() > 0 && isValidMAC(line)) {
                    _filters.push_back({line, false, "OUI: " + line});
                }
            }
        }
    }
    // MAC entries
    {
        String data = macs;
        data.trim();
        if (data.length() > 0) {
            int start = 0, end = data.indexOf('\n');
            while (start < (int)data.length()) {
                String line;
                if (end == -1) {
                    line = data.substring(start);
                    start = data.length();
                } else {
                    line = data.substring(start, end);
                    start = end + 1;
                    end = data.indexOf('\n', start);
                }
                line.trim();
                line.replace("\r", "");
                if (line.length() > 0 && isValidMAC(line)) {
                    _filters.push_back({line, true, "MAC: " + line});
                }
            }
        }
    }
}

void DetectorModule::registerRoutes(httpd_handle_t https, httpd_handle_t http) {
    registerDetectorRoutes(https, http, *this);
}

bool DetectorModule::isEnabled() {
    return _enabled;
}
void DetectorModule::setEnabled(bool enabled) {
    _enabled = enabled;
}
