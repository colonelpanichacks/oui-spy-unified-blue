#include "wardriver.h"
#include "../hal/gps.h"
#include "../hal/notify.h"
#include "../hal/wifi_mgr.h"
#include "../storage/nvs_store.h"
#include "../web/routes.h"
#include "../web/ws_broadcast.h"
#include "wardriver_logic.h"
#include <LittleFS.h>
#include <WiFi.h>
#include <algorithm>
#include <memory>

// ============================================================================
// Wardriver Module: WiFi + BLE wardriving with WiGLE CSV export
// ============================================================================

#define WD_SCAN_INTERVAL_MS 10000
#define WD_SAVE_INTERVAL_MS 10000

// ============================================================================
// NVS Persistence
// ============================================================================

void WardriverModule::loadFilters() {
    int count = storage::getWdFilterCount();
    _filters.clear();
    for (int i = 0; i < count; i++) {
        WDTargetFilter f;
        storage::getWdFilter(i, f.identifier, f.isFullMAC, f.description);
        if (f.identifier.length() > 0)
            _filters.push_back(f);
    }
}

void WardriverModule::saveFilters() {
    int count = (int)_filters.size();
    std::vector<String> ids(count), descs(count);
    std::unique_ptr<bool[]> fullMACs(new bool[count]);
    for (int i = 0; i < count; i++) {
        ids[i] = _filters[i].identifier;
        fullMACs[i] = _filters[i].isFullMAC;
        descs[i] = _filters[i].description;
    }
    storage::saveWdFilters(count, ids.data(), fullMACs.get(), descs.data());
}

void WardriverModule::loadAliases() {
    int count = storage::getWdAliasCount();
    _aliases.clear();
    for (int i = 0; i < count; i++) {
        WDDeviceAlias a;
        storage::getWdAlias(i, a.macAddress, a.alias);
        if (a.macAddress.length() > 0 && a.alias.length() > 0)
            _aliases.push_back(a);
    }
}

void WardriverModule::saveAliases() {
    int count = (int)_aliases.size();
    std::vector<String> macs(count), names(count);
    for (int i = 0; i < count; i++) {
        macs[i] = _aliases[i].macAddress;
        names[i] = _aliases[i].alias;
    }
    storage::saveWdAliases(count, macs.data(), names.data());
}

void WardriverModule::saveDevices() {
    int count = min((int)_wlDevices.size(), 100);
    std::vector<String> macs(count), filters(count);
    std::vector<int> rssis(count);
    std::vector<unsigned long> lastSeens(count);
    for (int i = 0; i < count; i++) {
        macs[i] = _wlDevices[i].macAddress;
        rssis[i] = _wlDevices[i].rssi;
        lastSeens[i] = _wlDevices[i].lastSeen;
        filters[i] = _wlDevices[i].filterDescription;
    }
    storage::saveWdDevices(count, macs.data(), rssis.data(), lastSeens.data(), filters.data());
}

void WardriverModule::loadDevices() {
    int count = storage::getWdDeviceCount();
    _wlDevices.clear();
    for (int i = 0; i < count; i++) {
        WDDeviceInfo d;
        storage::getWdDevice(i, d.macAddress, d.rssi, d.lastSeen, d.filterDescription);
        d.firstSeen = d.lastSeen;
        d.inCooldown = false;
        d.cooldownUntil = 0;
        if (d.macAddress.length() > 0)
            _wlDevices.push_back(d);
    }
}

void WardriverModule::loadWigleConfig() {
    _wigleApiName = storage::getWdApiName();
    _wigleApiToken = storage::getWdApiToken();
}

// ============================================================================
// Module Lifecycle
// ============================================================================

void WardriverModule::setup() {
    _mutex = xSemaphoreCreateMutex();
    if (!LittleFS.begin(true)) {
        Serial.println("[WARDRIVER] LittleFS mount failed");
    }
    loadFilters();
    loadAliases();
    loadDevices();
    loadWigleConfig();
    Serial.printf("[WARDRIVER] Loaded %d filters, %d aliases, %d watchlist devices\n",
                  (int)_filters.size(), (int)_aliases.size(), (int)_wlDevices.size());
}

void WardriverModule::loop() {
    if (!_enabled)
        return;
    if (!_sessionActive)
        return;

    unsigned long now = millis();

    // WiFi scan cycle
    if (!_scanPending && (now - _lastScanStart >= WD_SCAN_INTERVAL_MS)) {
        if (hal::wifiStartNetworkScan()) {
            _scanPending = true;
            _lastScanStart = now;
        }
    }

    if (_scanPending) {
        int result = hal::wifiGetScanComplete();
        if (result >= 0) {
            // Process scan results
            bool gpsFresh = hal::gpsIsFresh();
            const hal::GPSData& gps = hal::gpsGet();
            String timeStr = hal::gpsGetTime();

            for (int i = 0; i < result; i++) {
                String bssid = WiFi.BSSIDstr(i);
                String ssid = WiFi.SSID(i);
                int rssi = WiFi.RSSI(i);
                int channel = WiFi.channel(i);
                int authMode = (int)WiFi.encryptionType(i);

                if (!xSemaphoreTake(_mutex, pdMS_TO_TICKS(100)))
                    continue;

                // Dedup
                uint32_t hash = wardriver_logic::hashMAC(bssid.c_str());
                bool isNew = wardriver_logic::dedupInsert(_dedupSet, WD_DEDUP_CAPACITY, hash);

                _wifiCount++;

                // Write CSV row (use GPS coords if fresh, otherwise 0,0)
                {
                    std::string row = wardriver_logic::formatWigleRow(
                        bssid.c_str(), ssid.c_str(), wardriver_logic::authModeToString(authMode),
                        "WIFI", channel, rssi, gpsFresh ? gps.lat : 0.0, gpsFresh ? gps.lon : 0.0,
                        gpsFresh ? gps.accuracy : 0.0f, timeStr.c_str());
                    appendCsvRow(String(row.c_str()));
                }

                // Add to recent ring
                addRecent(bssid, ssid, "WIFI", rssi, channel);

                // Check watchlist (accesses _filters and _wlDevices)
                if (isNew)
                    checkWatchlist(bssid, rssi, "WIFI");

                xSemaphoreGive(_mutex);
            }

            hal::wifiScanCleanup();
            _scanPending = false;
        }
    }

    // Push wd/status over WS every 2s when session active
    static unsigned long lastWsPush = 0;
    if (now - lastWsPush >= 2000) {
        bool gpsFresh = hal::gpsIsFresh();
        int unique = 0;
        if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            unique = wardriver_logic::dedupCount(_dedupSet, WD_DEDUP_CAPACITY);
            xSemaphoreGive(_mutex);
        }
        char json[256];
        snprintf(json, sizeof(json),
                 "{\"active\":true,\"wifi\":%d,\"ble\":%d,\"unique\":%d,"
                 "\"gps_fresh\":%s,\"has_csv\":%s,\"csv_size\":%u}",
                 _wifiCount, _bleCount, unique, gpsFresh ? "true" : "false",
                 hasCsvFile() ? "true" : "false", (unsigned)csvFileSize());
        ws::enqueue(ws::topic::WD_STATUS, json);
        lastWsPush = now;
    }

    // Auto-save watchlist devices
    if (now - _lastSave >= WD_SAVE_INTERVAL_MS) {
        saveDevices();
        _lastSave = now;
    }
}

void WardriverModule::onBLEAdvertisement(const NimBLEAdvertisedDevice* device) {
    if (!_enabled || !_sessionActive)
        return;

    String mac = device->getAddress().toString().c_str();
    int rssi = device->getRSSI();
    String deviceName = device->getName().c_str();

    // All shared state access under single mutex hold to avoid races
    // with loop() WiFi path and web route handlers
    if (!xSemaphoreTake(_mutex, pdMS_TO_TICKS(100)))
        return;

    // Dedup
    uint32_t hash = wardriver_logic::hashMAC(mac.c_str());
    bool isNew = wardriver_logic::dedupInsert(_dedupSet, WD_DEDUP_CAPACITY, hash);

    _bleCount++;

    // Write CSV row (use GPS coords if fresh, otherwise 0,0)
    {
        bool gpsFresh = hal::gpsIsFresh();
        const hal::GPSData& gps = hal::gpsGet();
        String timeStr = hal::gpsGetTime();
        std::string row = wardriver_logic::formatWigleRow(
            mac.c_str(), deviceName.c_str(), "[BLE]", "BLE", 0, rssi, gpsFresh ? gps.lat : 0.0,
            gpsFresh ? gps.lon : 0.0, gpsFresh ? gps.accuracy : 0.0f, timeStr.c_str());
        appendCsvRow(String(row.c_str()));
    }

    // Add to recent ring
    addRecent(mac, deviceName, "BLE", rssi, 0);

    // Check watchlist (accesses _filters and _wlDevices)
    if (isNew)
        checkWatchlist(mac, rssi, "BLE");

    xSemaphoreGive(_mutex);
}

// ============================================================================
// Session Management
// ============================================================================

void WardriverModule::startSession() {
    if (_sessionActive)
        return;

    // Reset counters and dedup
    _wifiCount = 0;
    _bleCount = 0;
    memset(_dedupSet, 0, sizeof(_dedupSet));
    _recentHead = 0;
    _recentCount = 0;

    // Write CSV header
    writeCsvHeader();

    _sessionActive = true;
    _lastScanStart = 0;
    _lastSave = millis();
    hal::notify(hal::NOTIFY_WD_SESSION_START);
    Serial.println("[WARDRIVER] Session started");
}

void WardriverModule::stopSession() {
    if (!_sessionActive)
        return;
    _sessionActive = false;

    // Cleanup any pending scan
    if (_scanPending) {
        hal::wifiScanCleanup();
        _scanPending = false;
    }

    saveDevices();
    Serial.printf("[WARDRIVER] Session stopped. WiFi:%d BLE:%d Unique:%d\n", _wifiCount, _bleCount,
                  uniqueCount());
}

void WardriverModule::clearData() {
    stopSession();
    LittleFS.remove(WD_CSV_PATH);
    _wifiCount = 0;
    _bleCount = 0;
    memset(_dedupSet, 0, sizeof(_dedupSet));
    _recentHead = 0;
    _recentCount = 0;
    Serial.println("[WARDRIVER] Data cleared");
}

// ============================================================================
// CSV File Operations
// ============================================================================

void WardriverModule::writeCsvHeader() {
    File f = LittleFS.open(WD_CSV_PATH, "w");
    if (!f)
        return;
    // WiGLE pre-header
    f.println(
        "WigleWifi-1.4,appRelease=OUI-SPY,model=ESP32-S3,release=3.0,device=XIAO,display=SPA,board="
        "seeed_xiao_esp32s3,brand=OUI-SPY");
    // Column header
    f.println("MAC,SSID,AuthMode,FirstSeen,Channel,RSSI,CurrentLatitude,CurrentLongitude,"
              "AltitudeMeters,AccuracyMeters,Type");
    f.close();
}

void WardriverModule::appendCsvRow(const String& row) {
    File f = LittleFS.open(WD_CSV_PATH, "a");
    if (!f)
        return;
    f.println(row);
    f.close();
}

size_t WardriverModule::csvFileSize() const {
    File f = LittleFS.open(WD_CSV_PATH, "r");
    if (!f)
        return 0;
    size_t sz = f.size();
    f.close();
    return sz;
}

bool WardriverModule::hasCsvFile() const {
    return LittleFS.exists(WD_CSV_PATH);
}

// ============================================================================
// Recent Sightings Ring Buffer
// ============================================================================

void WardriverModule::addRecent(const String& mac, const String& ssid, const String& type, int rssi,
                                int channel) {
    _recent[_recentHead] = {mac, ssid, type, rssi, channel, millis()};
    _recentHead = (_recentHead + 1) % WD_RECENT_SIZE;
    if (_recentCount < WD_RECENT_SIZE)
        _recentCount++;

    // Push sighting over WS (escape user-supplied SSID)
    {
        char safeSsid[72];
        ws::jsonEscape(safeSsid, sizeof(safeSsid), ssid.c_str());
        char json[256];
        snprintf(json, sizeof(json),
                 "{\"mac\":\"%s\",\"ssid\":\"%s\",\"type\":\"%s\",\"rssi\":%d,\"channel\":%d}",
                 mac.c_str(), safeSsid, type.c_str(), rssi, channel);
        ws::enqueue(ws::topic::WD_SIGHTING, json);
    }
}

void WardriverModule::getRecentSightings(std::vector<WDRecentSighting>& out) const {
    out.clear();
    if (!_mutex)
        return;
    if (!xSemaphoreTake(_mutex, pdMS_TO_TICKS(100)))
        return;
    out.reserve(_recentCount);
    // Newest first
    for (int i = 0; i < _recentCount; i++) {
        int idx = (_recentHead - 1 - i + WD_RECENT_SIZE) % WD_RECENT_SIZE;
        out.push_back(_recent[idx]);
    }
    xSemaphoreGive(_mutex);
}

int WardriverModule::uniqueCount() const {
    if (!_mutex)
        return 0;
    if (!xSemaphoreTake(_mutex, pdMS_TO_TICKS(100)))
        return 0;
    int count = wardriver_logic::dedupCount(_dedupSet, WD_DEDUP_CAPACITY);
    xSemaphoreGive(_mutex);
    return count;
}

void WardriverModule::getFilters(std::vector<WDTargetFilter>& out) const {
    out.clear();
    if (!_mutex)
        return;
    if (!xSemaphoreTake(_mutex, pdMS_TO_TICKS(100)))
        return;
    out = _filters;
    xSemaphoreGive(_mutex);
}

void WardriverModule::getWatchlistDevices(std::vector<WDDeviceInfo>& out) const {
    out.clear();
    if (!_mutex)
        return;
    if (!xSemaphoreTake(_mutex, pdMS_TO_TICKS(100)))
        return;
    out = _wlDevices;
    xSemaphoreGive(_mutex);
}

int WardriverModule::filterCount() const {
    if (!_mutex)
        return 0;
    if (!xSemaphoreTake(_mutex, pdMS_TO_TICKS(100)))
        return 0;
    int count = (int)_filters.size();
    xSemaphoreGive(_mutex);
    return count;
}

// ============================================================================
// Watchlist
// ============================================================================

bool WardriverModule::matchesFilter(const String& deviceMAC, String& matchedDesc) {
    String normDevice = deviceMAC;
    normDevice.toLowerCase();
    for (const auto& f : _filters) {
        String normFilter = f.identifier;
        normFilter.toLowerCase();
        normFilter.replace("-", ":");
        if (f.isFullMAC) {
            if (normDevice == normFilter) {
                matchedDesc = f.description;
                return true;
            }
        } else {
            if (normDevice.startsWith(normFilter)) {
                matchedDesc = f.description;
                return true;
            }
        }
    }
    return false;
}

void WardriverModule::checkWatchlist(const String& mac, int rssi, const String& type) {
    if (_filters.empty())
        return;

    String matchedDesc;
    if (!matchesFilter(mac, matchedDesc))
        return;

    unsigned long now = millis();

    // Check existing watchlist devices
    for (auto& dev : _wlDevices) {
        if (dev.macAddress == mac) {
            if (dev.inCooldown && now < dev.cooldownUntil)
                return;
            if (dev.inCooldown && now >= dev.cooldownUntil)
                dev.inCooldown = false;

            unsigned long sinceLast = now - dev.lastSeen;
            if (sinceLast >= 30000) {
                hal::notify(hal::NOTIFY_WD_RE_30S);
                dev.inCooldown = true;
                dev.cooldownUntil = now + 10000;
            } else if (sinceLast >= 3000) {
                hal::notify(hal::NOTIFY_WD_RE_3S);
                dev.inCooldown = true;
                dev.cooldownUntil = now + 3000;
            }
            dev.lastSeen = now;
            dev.rssi = rssi;
            return;
        }
    }

    // New watchlist device
    WDDeviceInfo newDev;
    newDev.macAddress = mac;
    newDev.rssi = rssi;
    newDev.firstSeen = now;
    newDev.lastSeen = now;
    newDev.inCooldown = true;
    newDev.cooldownUntil = now + 3000;
    newDev.filterDescription = matchedDesc;
    _wlDevices.push_back(newDev);

    hal::notify(hal::NOTIFY_WD_NEW_DEVICE);
    Serial.printf("[WARDRIVER] Watchlist match: %s (%s) type=%s\n", mac.c_str(),
                  matchedDesc.c_str(), type.c_str());
}

// ============================================================================
// Filter / Alias / Config Operations
// ============================================================================

void WardriverModule::parseFilters(const String& ouis, const String& macs) {
    _filters.clear();
    // OUI entries
    String ouiData = ouis;
    ouiData.trim();
    if (ouiData.length() > 0) {
        int start = 0, end = ouiData.indexOf('\n');
        while (start < (int)ouiData.length()) {
            String line;
            if (end == -1) {
                line = ouiData.substring(start);
                start = ouiData.length();
            } else {
                line = ouiData.substring(start, end);
                start = end + 1;
                end = ouiData.indexOf('\n', start);
            }
            line.trim();
            line.replace("\r", "");
            if (line.length() >= 8) {
                _filters.push_back({line, false, "OUI: " + line});
            }
        }
    }
    // MAC entries
    String macData = macs;
    macData.trim();
    if (macData.length() > 0) {
        int start = 0, end = macData.indexOf('\n');
        while (start < (int)macData.length()) {
            String line;
            if (end == -1) {
                line = macData.substring(start);
                start = macData.length();
            } else {
                line = macData.substring(start, end);
                start = end + 1;
                end = macData.indexOf('\n', start);
            }
            line.trim();
            line.replace("\r", "");
            if (line.length() == 17) {
                _filters.push_back({line, true, "MAC: " + line});
            }
        }
    }
}

void WardriverModule::clearFilters() {
    _filters.clear();
    saveFilters();
}

String WardriverModule::getAlias(const String& mac) {
    String norm = mac;
    norm.toLowerCase();
    for (const auto& a : _aliases) {
        String n = a.macAddress;
        n.toLowerCase();
        if (n.equals(norm))
            return a.alias;
    }
    return "";
}

void WardriverModule::setAlias(const String& mac, const String& alias) {
    String norm = mac;
    norm.toLowerCase();
    if (alias.length() == 0) {
        _aliases.erase(std::remove_if(_aliases.begin(), _aliases.end(),
                                      [&](const WDDeviceAlias& da) {
                                          String nm = da.macAddress;
                                          nm.toLowerCase();
                                          return nm.equals(norm);
                                      }),
                       _aliases.end());
        return;
    }
    for (auto& a : _aliases) {
        String n = a.macAddress;
        n.toLowerCase();
        if (n.equals(norm)) {
            a.alias = alias;
            return;
        }
    }
    _aliases.push_back({norm, alias});
}

void WardriverModule::clearWatchlistDevices() {
    _wlDevices.clear();
    storage::clearWdDevices();
}

void WardriverModule::setWigleConfig(const String& apiName, const String& apiToken) {
    _wigleApiName = apiName;
    _wigleApiToken = apiToken;
    storage::setWdApiConfig(apiName, apiToken);
}

// ============================================================================
// IModule Interface
// ============================================================================

void WardriverModule::registerRoutes(httpd_handle_t https, httpd_handle_t http) {
    registerWardriverRoutes(https, http, *this);
}

bool WardriverModule::isEnabled() {
    return _enabled;
}

void WardriverModule::setEnabled(bool enabled) {
    _enabled = enabled;
    if (!enabled && _sessionActive)
        stopSession();
}
