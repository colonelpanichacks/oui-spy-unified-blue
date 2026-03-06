#include "flockyou.h"
#include "../hal/gps.h"
#include "../hal/neopixel.h"
#include "../hal/notify.h"
#include "../web/http_helpers.h"
#include "../web/routes.h"
#include "../web/ws_broadcast.h"
#include "flockyou_logic.h"
#include <LittleFS.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <ctype.h>

// ============================================================================
// Flock-You Module: Surveillance device detector with all 5 detection methods
// ============================================================================

// --- Detection Patterns (compile-time data) ---

static const char* fyMACPrefixes[] = {"58:8e:81", "cc:cc:cc", "ec:1b:bd", "90:35:ea", "04:0d:84",
                                      "f0:82:c0", "1c:34:f1", "38:5b:44", "94:34:69", "b4:e3:f9",
                                      "70:c9:4e", "3c:91:80", "d8:f3:bc", "80:30:49", "14:5a:fc",
                                      "74:4c:a1", "08:3a:88", "9c:2f:9d", "94:08:53", "e4:aa:ea"};

static const char* fyNamePatterns[] = {"FS Ext Battery", "Penguin", "Flock", "Pigvision"};

static const uint16_t fyMfrIDs[] = {0x09C8};

#define RAVEN_DEVICE_INFO_SERVICE "0000180a-0000-1000-8000-00805f9b34fb"
#define RAVEN_GPS_SERVICE "00003100-0000-1000-8000-00805f9b34fb"
#define RAVEN_POWER_SERVICE "00003200-0000-1000-8000-00805f9b34fb"
#define RAVEN_NETWORK_SERVICE "00003300-0000-1000-8000-00805f9b34fb"
#define RAVEN_UPLOAD_SERVICE "00003400-0000-1000-8000-00805f9b34fb"
#define RAVEN_ERROR_SERVICE "00003500-0000-1000-8000-00805f9b34fb"
#define RAVEN_OLD_HEALTH_SERVICE "00001809-0000-1000-8000-00805f9b34fb"
#define RAVEN_OLD_LOCATION_SERVICE "00001819-0000-1000-8000-00805f9b34fb"

static const char* fyRavenUUIDs[] = {RAVEN_DEVICE_INFO_SERVICE, RAVEN_GPS_SERVICE,
                                     RAVEN_POWER_SERVICE,       RAVEN_NETWORK_SERVICE,
                                     RAVEN_UPLOAD_SERVICE,      RAVEN_ERROR_SERVICE,
                                     RAVEN_OLD_HEALTH_SERVICE,  RAVEN_OLD_LOCATION_SERVICE};

// Session file paths
#define FY_SESSION_FILE "/session.json"
#define FY_PREV_FILE "/prev_session.json"
#define FY_SAVE_INTERVAL 15000

// ============================================================================
// Detection Helpers
// ============================================================================

static bool fyCheckMAC(const uint8_t* mac) {
    return flockyou_logic::macMatchesPrefixes(mac, fyMACPrefixes,
                                              sizeof(fyMACPrefixes) / sizeof(fyMACPrefixes[0]));
}

static bool fyCheckName(const char* name) {
    return flockyou_logic::nameMatchesPatterns(name, fyNamePatterns,
                                               sizeof(fyNamePatterns) / sizeof(fyNamePatterns[0]));
}

static bool fyCheckMfr(uint16_t id) {
    return flockyou_logic::manufacturerMatches(id, fyMfrIDs,
                                               sizeof(fyMfrIDs) / sizeof(fyMfrIDs[0]));
}

static bool fyCheckRaven(const NimBLEAdvertisedDevice* dev) {
    if (!dev || !dev->haveServiceUUID())
        return false;
    int count = dev->getServiceUUIDCount();
    for (int i = 0; i < count; i++) {
        std::string str = dev->getServiceUUID(i).toString();
        if (flockyou_logic::uuidEqualsAny(str.c_str(), fyRavenUUIDs,
                                          sizeof(fyRavenUUIDs) / sizeof(fyRavenUUIDs[0])))
            return true;
    }
    return false;
}

static const char* fyEstimateRavenFW(const NimBLEAdvertisedDevice* dev) {
    if (!dev || !dev->haveServiceUUID())
        return "?";
    bool hasNewGPS = false, hasOldLoc = false, hasPower = false;
    int count = dev->getServiceUUIDCount();
    for (int i = 0; i < count; i++) {
        std::string u = dev->getServiceUUID(i).toString();
        if (strcasecmp(u.c_str(), RAVEN_GPS_SERVICE) == 0)
            hasNewGPS = true;
        if (strcasecmp(u.c_str(), RAVEN_OLD_LOCATION_SERVICE) == 0)
            hasOldLoc = true;
        if (strcasecmp(u.c_str(), RAVEN_POWER_SERVICE) == 0)
            hasPower = true;
    }
    return flockyou_logic::estimateRavenFirmware(hasNewGPS, hasOldLoc, hasPower);
}

void FlockyouModule::attachGPS(FYDetection& d) {
    if (hal::gpsIsFresh()) {
        const hal::GPSData& g = hal::gpsGet();
        d.hasGPS = true;
        d.gpsLat = g.lat;
        d.gpsLon = g.lon;
        d.gpsAcc = g.accuracy;
    }
}

int FlockyouModule::addDetection(const char* mac, const char* detName, int rssi, const char* method,
                                 bool isRaven, const char* ravenFW) {
    if (!_mutex || xSemaphoreTake(_mutex, pdMS_TO_TICKS(100)) != pdTRUE)
        return -1;

    // Update existing
    for (int i = 0; i < _detCount; i++) {
        if (strcasecmp(_det[i].mac, mac) == 0) {
            _det[i].count++;
            _det[i].lastSeen = millis();
            _det[i].rssi = rssi;
            if (detName && detName[0])
                strncpy(_det[i].name, detName, sizeof(_det[i].name) - 1);
            attachGPS(_det[i]);
            xSemaphoreGive(_mutex);
            return i;
        }
    }

    // Add new
    if (_detCount < FY_MAX_DETECTIONS) {
        FYDetection& d = _det[_detCount];
        memset(&d, 0, sizeof(d));
        strncpy(d.mac, mac, sizeof(d.mac) - 1);
        if (detName) {
            for (int j = 0; j < (int)sizeof(d.name) - 1 && detName[j]; j++)
                d.name[j] = (detName[j] == '"' || detName[j] == '\\') ? '_' : detName[j];
        }
        d.rssi = rssi;
        strncpy(d.method, method, sizeof(d.method) - 1);
        d.firstSeen = millis();
        d.lastSeen = millis();
        d.count = 1;
        d.isRaven = isRaven;
        strncpy(d.ravenFW, ravenFW ? ravenFW : "", sizeof(d.ravenFW) - 1);
        attachGPS(d);
        // Copy name out before releasing mutex (d.name was sanitized above)
        char safeName[sizeof(d.name)];
        strlcpy(safeName, d.name, sizeof(safeName));
        int idx = _detCount++;
        xSemaphoreGive(_mutex);

        // Push new detection over WS (outside mutex — enqueue uses spinlock)
        {
            char json[256];
            snprintf(json, sizeof(json),
                     "{\"mac\":\"%s\",\"name\":\"%s\",\"rssi\":%d,\"method\":\"%s\","
                     "\"count\":1,\"raven\":%s,\"fw\":\"%s\"}",
                     mac, safeName, rssi, method, isRaven ? "true" : "false",
                     ravenFW ? ravenFW : "");
            ws::enqueue("fy/detection", json);
        }

        return idx;
    }

    xSemaphoreGive(_mutex);
    return -1;
}

// ============================================================================
// Session Persistence (LittleFS)
// ============================================================================

void FlockyouModule::saveSession() {
    if (!_fsReady || !_mutex)
        return;
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(300)) != pdTRUE)
        return;
    File f = LittleFS.open(FY_SESSION_FILE, "w");
    if (!f) {
        xSemaphoreGive(_mutex);
        return;
    }
    f.print("[");
    for (int i = 0; i < _detCount; i++) {
        if (i > 0)
            f.print(",");
        FYDetection& d = _det[i];
        f.printf("{\"mac\":\"%s\",\"name\":\"%s\",\"rssi\":%d,\"method\":\"%s\","
                 "\"first\":%lu,\"last\":%lu,\"count\":%d,\"raven\":%s,\"fw\":\"%s\"",
                 d.mac, d.name, d.rssi, d.method, d.firstSeen, d.lastSeen, d.count,
                 d.isRaven ? "true" : "false", d.ravenFW);
        if (d.hasGPS)
            f.printf(",\"gps\":{\"lat\":%.8f,\"lon\":%.8f,\"acc\":%.1f}", d.gpsLat, d.gpsLon,
                     d.gpsAcc);
        f.print("}");
    }
    f.print("]");
    f.close();
    _lastSaveCount = _detCount;
    xSemaphoreGive(_mutex);
}

void FlockyouModule::promotePrevSession() {
    if (!_fsReady || !LittleFS.exists(FY_SESSION_FILE))
        return;
    File src = LittleFS.open(FY_SESSION_FILE, "r");
    if (!src)
        return;
    String data = src.readString();
    src.close();
    if (data.length() == 0) {
        LittleFS.remove(FY_SESSION_FILE);
        return;
    }
    File dst = LittleFS.open(FY_PREV_FILE, "w");
    if (!dst)
        return;
    dst.print(data);
    dst.close();
    LittleFS.remove(FY_SESSION_FILE);
    Serial.printf("[FLOCKYOU] Prior session promoted: %d bytes\n", data.length());
}

// ============================================================================
// JSON/KML Writers
// ============================================================================

void FlockyouModule::writeJSON(Print& out) {
    out.print("[");
    if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        for (int i = 0; i < _detCount; i++) {
            if (i > 0)
                out.print(",");
            out.printf("{\"mac\":\"%s\",\"name\":\"%s\",\"rssi\":%d,\"method\":\"%s\","
                       "\"first\":%lu,\"last\":%lu,\"count\":%d,\"raven\":%s,\"fw\":\"%s\"",
                       _det[i].mac, _det[i].name, _det[i].rssi, _det[i].method, _det[i].firstSeen,
                       _det[i].lastSeen, _det[i].count, _det[i].isRaven ? "true" : "false",
                       _det[i].ravenFW);
            if (_det[i].hasGPS)
                out.printf(",\"gps\":{\"lat\":%.8f,\"lon\":%.8f,\"acc\":%.1f}", _det[i].gpsLat,
                           _det[i].gpsLon, _det[i].gpsAcc);
            out.print("}");
        }
        xSemaphoreGive(_mutex);
    }
    out.print("]");
}

void FlockyouModule::writeKML(Print& out) {
    out.print(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<kml "
        "xmlns=\"http://www.opengis.net/kml/2.2\">\n<Document>\n"
        "<name>Flock-You Detections</name>\n"
        "<Style "
        "id=\"det\"><IconStyle><color>ff4489ec</color><scale>1.0</scale></IconStyle></Style>\n"
        "<Style "
        "id=\"raven\"><IconStyle><color>ff4444ef</color><scale>1.2</scale></IconStyle></Style>\n");
    if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(300)) == pdTRUE) {
        for (int i = 0; i < _detCount; i++) {
            FYDetection& d = _det[i];
            if (!d.hasGPS)
                continue;
            out.printf("<Placemark><name>%s</name><styleUrl>#%s</styleUrl>"
                       "<description><![CDATA[",
                       d.mac, d.isRaven ? "raven" : "det");
            if (d.name[0])
                out.printf("<b>Name:</b> %s<br/>", d.name);
            out.printf("<b>Method:</b> %s<br/><b>RSSI:</b> %d<br/><b>Count:</b> %d", d.method,
                       d.rssi, d.count);
            if (d.isRaven)
                out.printf("<br/><b>FW:</b> %s", d.ravenFW);
            out.printf("]]></description><Point><coordinates>%.8f,%.8f,0</coordinates></Point></"
                       "Placemark>\n",
                       d.gpsLon, d.gpsLat);
        }
        xSemaphoreGive(_mutex);
    }
    out.print("</Document>\n</kml>");
}

// ============================================================================
// Module Implementation
// ============================================================================

void FlockyouModule::setup() {
    _mutex = xSemaphoreCreateMutex();

    if (LittleFS.begin(true)) {
        _fsReady = true;
        promotePrevSession();
        Serial.println("[FLOCKYOU] LittleFS ready");
    }

    hal::neopixelSetIdleHue(270); // Purple for flockyou
    Serial.println("[FLOCKYOU] Module initialized");
}

void FlockyouModule::loop() {
    if (!_enabled)
        return;

    // Heartbeat tracking
    if (_deviceInRange) {
        if (millis() - _lastHB >= 10000) {
            hal::notify(hal::NOTIFY_FY_HEARTBEAT);
            _lastHB = millis();
        }
        if (millis() - _lastDetTime >= 30000) {
            _deviceInRange = false;
            _triggered = false;
            hal::notify(hal::NOTIFY_FY_IDLE);
        }
    }

    // Push fy/stats over WS every 2.5s
    static unsigned long lastStatsPush = 0;
    if (millis() - lastStatsPush >= 2500) {
        const hal::GPSData& g = hal::gpsGet();
        char json[128];
        const char* gpsSrc = g.hwFix ? "hw" : (hal::gpsIsFresh() ? "phone" : "none");
        snprintf(json, sizeof(json),
                 "{\"count\":%d,\"gps_src\":\"%s\",\"gps_sats\":%d,\"gps_hw_detected\":%s}",
                 _detCount, gpsSrc, g.satellites, g.hwDetected ? "true" : "false");
        ws::enqueue("fy/stats", json);
        lastStatsPush = millis();
    }

    // Auto-save
    if (_fsReady && millis() - _lastSave >= FY_SAVE_INTERVAL) {
        if (_detCount > 0 && _detCount != _lastSaveCount)
            saveSession();
        _lastSave = millis();
    } else if (_fsReady && _detCount > 0 && _lastSaveCount == 0 && millis() - _lastSave >= 5000) {
        saveSession();
        _lastSave = millis();
    }
}

void FlockyouModule::onBLEAdvertisement(const NimBLEAdvertisedDevice* dev) {
    if (!_enabled)
        return;

    NimBLEAddress addr = dev->getAddress();
    const uint8_t* mac = addr.getVal();
    std::string addrStr = addr.toString();

    int rssi = dev->getRSSI();
    std::string devName = dev->haveName() ? dev->getName() : "";

    bool detected = false;
    const char* method = "";
    bool isRaven = false;
    const char* ravenFW = "";

    // 1. MAC prefix
    if (fyCheckMAC(mac)) {
        detected = true;
        method = "mac_prefix";
    }

    // 2. Device name
    if (!detected && !devName.empty() && fyCheckName(devName.c_str())) {
        detected = true;
        method = "device_name";
    }

    // 3. Manufacturer ID
    if (!detected) {
        for (uint8_t i = 0; i < dev->getManufacturerDataCount(); i++) {
            std::string data = dev->getManufacturerData(i);
            if (data.size() >= 2) {
                uint16_t code = ((uint16_t)(uint8_t)data[1] << 8) | (uint16_t)(uint8_t)data[0];
                if (fyCheckMfr(code)) {
                    detected = true;
                    method = "ble_mfr_id";
                    break;
                }
            }
        }
    }

    // 4. Raven UUID
    if (!detected && fyCheckRaven(dev)) {
        detected = true;
        method = "raven_uuid";
        isRaven = true;
        ravenFW = fyEstimateRavenFW(dev);
    }

    if (!detected)
        return;

    addDetection(addrStr.c_str(), devName.c_str(), rssi, method, isRaven, ravenFW);

    // Serial JSON output
    char gpsBuf[80] = "";
    if (hal::gpsIsFresh()) {
        const hal::GPSData& g = hal::gpsGet();
        snprintf(gpsBuf, sizeof(gpsBuf),
                 ",\"gps\":{\"latitude\":%.8f,\"longitude\":%.8f,\"accuracy\":%.1f}", g.lat, g.lon,
                 g.accuracy);
    }
    if (isRaven) {
        Serial.printf(
            "{\"module\":\"flockyou\",\"detection_method\":\"%s\",\"mac_address\":\"%s\","
            "\"device_name\":\"%s\",\"rssi\":%d,\"is_raven\":true,\"raven_fw\":\"%s\"%s}\n",
            method, addrStr.c_str(), devName.c_str(), rssi, ravenFW, gpsBuf);
    } else {
        Serial.printf("{\"module\":\"flockyou\",\"detection_method\":\"%s\",\"mac_address\":\"%s\","
                      "\"device_name\":\"%s\",\"rssi\":%d%s}\n",
                      method, addrStr.c_str(), devName.c_str(), rssi, gpsBuf);
    }

    if (!_triggered) {
        _triggered = true;
        hal::notify(hal::NOTIFY_FY_ALERT);
    }
    _deviceInRange = true;
    _lastDetTime = millis();
    _lastHB = millis();
}

// ============================================================================
// Public Operations (used by route handlers)
// ============================================================================

void FlockyouModule::writeCSV(Print& out) {
    out.println("mac,name,rssi,method,first_seen_ms,last_seen_ms,count,is_raven,raven_fw,"
                "latitude,longitude,gps_accuracy");
    if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        for (int i = 0; i < _detCount; i++) {
            FYDetection& d = _det[i];
            if (d.hasGPS) {
                out.printf("\"%s\",\"%s\",%d,\"%s\",%lu,%lu,%d,%s,\"%s\",%.8f,%.8f,%.1f\n", d.mac,
                           d.name, d.rssi, d.method, d.firstSeen, d.lastSeen, d.count,
                           d.isRaven ? "true" : "false", d.ravenFW, d.gpsLat, d.gpsLon, d.gpsAcc);
            } else {
                out.printf("\"%s\",\"%s\",%d,\"%s\",%lu,%lu,%d,%s,\"%s\",,,\n", d.mac, d.name,
                           d.rssi, d.method, d.firstSeen, d.lastSeen, d.count,
                           d.isRaven ? "true" : "false", d.ravenFW);
            }
        }
        xSemaphoreGive(_mutex);
    }
}

void FlockyouModule::writePatterns(Print& out) {
    out.print("{\"macs\":[");
    for (size_t i = 0; i < sizeof(fyMACPrefixes) / sizeof(fyMACPrefixes[0]); i++) {
        if (i > 0)
            out.print(",");
        out.printf("\"%s\"", fyMACPrefixes[i]);
    }
    out.print("],\"names\":[");
    for (size_t i = 0; i < sizeof(fyNamePatterns) / sizeof(fyNamePatterns[0]); i++) {
        if (i > 0)
            out.print(",");
        out.printf("\"%s\"", fyNamePatterns[i]);
    }
    out.print("],\"mfr\":[");
    for (size_t i = 0; i < sizeof(fyMfrIDs) / sizeof(fyMfrIDs[0]); i++) {
        if (i > 0)
            out.print(",");
        out.printf("%u", fyMfrIDs[i]);
    }
    out.print("],\"raven\":[");
    for (size_t i = 0; i < sizeof(fyRavenUUIDs) / sizeof(fyRavenUUIDs[0]); i++) {
        if (i > 0)
            out.print(",");
        out.printf("\"%s\"", fyRavenUUIDs[i]);
    }
    out.print("]}");
}

esp_err_t FlockyouModule::serveHistory(httpd_req_t* req) {
    if (_fsReady && LittleFS.exists(FY_PREV_FILE)) {
        return web::sendFile(req, LittleFS, FY_PREV_FILE, "application/json", nullptr);
    }
    return web::sendJSON(req, 200, "[]");
}

esp_err_t FlockyouModule::serveHistoryDownload(httpd_req_t* req) {
    if (_fsReady && LittleFS.exists(FY_PREV_FILE)) {
        return web::sendFile(req, LittleFS, FY_PREV_FILE, "application/json",
                             "attachment; filename=\"flockyou_prev_session.json\"");
    }
    return web::sendError(req, 404, "no prior session");
}

void FlockyouModule::clearDetections() {
    saveSession();
    if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        _detCount = 0;
        memset(_det, 0, sizeof(_det));
        _triggered = false;
        _deviceInRange = false;
        xSemaphoreGive(_mutex);
    }
}

// ============================================================================
// Web Routes
// ============================================================================

void FlockyouModule::registerRoutes(httpd_handle_t https, httpd_handle_t http) {
    registerFlockyouRoutes(https, http, *this);
}

bool FlockyouModule::isEnabled() {
    return _enabled;
}
void FlockyouModule::setEnabled(bool enabled) {
    _enabled = enabled;
}
