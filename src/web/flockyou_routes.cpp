#include "../hal/gps.h"
#include "../modules/flockyou.h"
#include "http_helpers.h"
#include "routes.h"
#include <ArduinoJson.h>
#include <LittleFS.h>

static FlockyouModule* _fyMod = nullptr;

// Detections JSON (chunked streaming)
static esp_err_t handleFYDetections(httpd_req_t* req) {
    web::ChunkedPrint print(req, "application/json");
    _fyMod->writeJSON(print);
    return print.end();
}

// Stats
static esp_err_t handleFYStats(httpd_req_t* req) {
    int raven = 0, withGPS = 0;
    SemaphoreHandle_t mtx = _fyMod->mutex();
    if (mtx && xSemaphoreTake(mtx, pdMS_TO_TICKS(100)) == pdTRUE) {
        const FYDetection* dets = _fyMod->detections();
        int count = _fyMod->detectionCount();
        for (int i = 0; i < count; i++) {
            if (dets[i].isRaven)
                raven++;
            if (dets[i].hasGPS)
                withGPS++;
        }
        xSemaphoreGive(mtx);
    }
    const hal::GPSData& g = hal::gpsGet();
    const char* gpsSrc = hal::gpsSourceStr();
    JsonDocument doc;
    doc["total"] = _fyMod->detectionCount();
    doc["raven"] = raven;
    doc["ble"] = "active";
    doc["gps_valid"] = hal::gpsIsFresh();
    doc["gps_tagged"] = withGPS;
    doc["gps_src"] = gpsSrc;
    doc["gps_sats"] = g.satellites;
    doc["gps_hw_detected"] = g.hwDetected;
    String json;
    serializeJson(doc, json);
    return web::sendJSON(req, 200, json.c_str());
}

// Patterns (chunked streaming)
static esp_err_t handleFYPatterns(httpd_req_t* req) {
    web::ChunkedPrint print(req, "application/json");
    _fyMod->writePatterns(print);
    return print.end();
}

// Export JSON
static esp_err_t handleFYExportJSON(httpd_req_t* req) {
    web::ChunkedPrint print(req, "application/json");
    httpd_resp_set_hdr(req, "Content-Disposition",
                       "attachment; filename=\"flockyou_detections.json\"");
    _fyMod->writeJSON(print);
    return print.end();
}

// Export CSV
static esp_err_t handleFYExportCSV(httpd_req_t* req) {
    web::ChunkedPrint print(req, "text/csv");
    httpd_resp_set_hdr(req, "Content-Disposition",
                       "attachment; filename=\"flockyou_detections.csv\"");
    _fyMod->writeCSV(print);
    return print.end();
}

// Export KML
static esp_err_t handleFYExportKML(httpd_req_t* req) {
    web::ChunkedPrint print(req, "application/vnd.google-earth.kml+xml");
    httpd_resp_set_hdr(req, "Content-Disposition",
                       "attachment; filename=\"flockyou_detections.kml\"");
    _fyMod->writeKML(print);
    return print.end();
}

// History (serve file)
static esp_err_t handleFYHistory(httpd_req_t* req) {
    return _fyMod->serveHistory(req);
}

// History download
static esp_err_t handleFYHistoryDownload(httpd_req_t* req) {
    return _fyMod->serveHistoryDownload(req);
}

// Clear detections
static esp_err_t handleFYClear(httpd_req_t* req) {
    _fyMod->clearDetections();
    return web::sendJSON(req, 200, "{\"status\":\"cleared\"}");
}

void registerFlockyouRoutes(httpd_handle_t https, httpd_handle_t http, FlockyouModule& mod) {
    _fyMod = &mod;

    static const httpd_uri_t detectionsUri = {"/api/flockyou/detections", HTTP_GET,
                                              handleFYDetections, nullptr};
    static const httpd_uri_t statsUri = {"/api/flockyou/stats", HTTP_GET, handleFYStats, nullptr};
    static const httpd_uri_t patternsUri = {"/api/flockyou/patterns", HTTP_GET, handleFYPatterns,
                                            nullptr};
    static const httpd_uri_t exportJsonUri = {"/api/flockyou/export/json", HTTP_GET,
                                              handleFYExportJSON, nullptr};
    static const httpd_uri_t exportCsvUri = {"/api/flockyou/export/csv", HTTP_GET,
                                             handleFYExportCSV, nullptr};
    static const httpd_uri_t exportKmlUri = {"/api/flockyou/export/kml", HTTP_GET,
                                             handleFYExportKML, nullptr};
    static const httpd_uri_t historyUri = {"/api/flockyou/history", HTTP_GET, handleFYHistory,
                                           nullptr};
    static const httpd_uri_t historyDlUri = {"/api/flockyou/history/json", HTTP_GET,
                                             handleFYHistoryDownload, nullptr};
    static const httpd_uri_t clearUri = {"/api/flockyou/clear", HTTP_GET, handleFYClear, nullptr};

    web::registerOnBoth(https, http, &detectionsUri);
    web::registerOnBoth(https, http, &statsUri);
    web::registerOnBoth(https, http, &patternsUri);
    web::registerOnBoth(https, http, &exportJsonUri);
    web::registerOnBoth(https, http, &exportCsvUri);
    web::registerOnBoth(https, http, &exportKmlUri);
    web::registerOnBoth(https, http, &historyUri);
    web::registerOnBoth(https, http, &historyDlUri);
    web::registerOnBoth(https, http, &clearUri);

    Serial.println("[FLOCKYOU] Web routes registered");
}
