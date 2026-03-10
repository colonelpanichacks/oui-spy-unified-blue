#include "server.h"
#include "../hal/buzzer.h"
#include "../hal/gps.h"
#include "../storage/nvs_store.h"
#include "certs/ca_cert_der.h"
#include "certs/ca_cert_pem.h"
#include "certs/dev_cert_pem.h"
#include "certs/dev_key_pem.h"
#include "dashboard_gz.h"
#include "http_helpers.h"
#include "welcome_html.h"
#include "ws_broadcast.h"
#include <ArduinoJson.h>
#include <esp_https_server.h>

namespace web {

static httpd_handle_t _httpsServer = nullptr;
static httpd_handle_t _httpServer = nullptr;
static IModule** _modules = nullptr;
static int _moduleCount = 0;

// ============================================================================
// Utility: register a handler on both servers
// ============================================================================

static void regBoth(const httpd_uri_t* uri) {
    registerOnBoth(_httpsServer, _httpServer, uri);
}

// ============================================================================
// Route Handlers
// ============================================================================

// HTTPS :443 → full Preact SPA dashboard (gzipped)
static esp_err_t handleDashboard(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    return httpd_resp_send(req, (const char*)DASHBOARD_HTML_GZ, DASHBOARD_HTML_GZ_LEN);
}

// HTTP :80 → captive portal welcome page
static esp_err_t handleWelcome(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_sendstr(req, WELCOME_HTML);
}

// Captive portal detection endpoints (also serve welcome page)
static esp_err_t handleCaptiveDetect(httpd_req_t* req) {
    return handleWelcome(req);
}

// CA certificate download (DER — for iOS profile install)
static esp_err_t handleCACertDER(httpd_req_t* req) {
    httpd_resp_set_type(req, "application/x-x509-ca-cert");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"ouispy-ca.cer\"");
    return httpd_resp_send(req, (const char*)CA_CERT_DER, CA_CERT_DER_LEN);
}

// CA certificate download (PEM)
static esp_err_t handleCACertPEM(httpd_req_t* req) {
    httpd_resp_set_type(req, "application/x-pem-file");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"ouispy-ca.pem\"");
    return httpd_resp_send(req, (const char*)CA_CERT_PEM, CA_CERT_PEM_LEN);
}

// System status
static esp_err_t handleStatus(httpd_req_t* req) {
    JsonDocument doc;
    doc["uptime"] = millis() / 1000;
    doc["heap"] = ESP.getFreeHeap();
    doc["psram"] = ESP.getFreePsram();
    doc["buzzer"] = hal::buzzerIsEnabled();
    String json;
    serializeJson(doc, json);
    return sendJSON(req, 200, json.c_str());
}

// Module list
static esp_err_t handleModules(httpd_req_t* req) {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (int i = 0; i < _moduleCount; i++) {
        JsonObject obj = arr.add<JsonObject>();
        obj["name"] = _modules[i]->name();
        obj["enabled"] = _modules[i]->isEnabled();
    }
    String json;
    serializeJson(doc, json);
    return sendJSON(req, 200, json.c_str());
}

// Toggle module
static esp_err_t handleModuleToggle(httpd_req_t* req) {
    char body[256];
    if (readFormBody(req, body, sizeof(body)) < 0)
        return sendError(req, 400, "bad request");

    String name, enabled;
    if (!getFormValue(body, "name", name) || !getFormValue(body, "enabled", enabled))
        return sendError(req, 400, "missing params");

    bool en = (enabled == "true");
    for (int i = 0; i < _moduleCount; i++) {
        if (name == _modules[i]->name()) {
            _modules[i]->setEnabled(en);
            storage::setModuleEnabled(name.c_str(), en);

            // Push module list update over WS
            ws::pushModuleList(_modules, _moduleCount);

            return sendJSON(req, 200, "{\"ok\":true}");
        }
    }
    return sendError(req, 404, "module not found");
}

// Buzzer toggle
static esp_err_t handleBuzzer(httpd_req_t* req) {
    char body[128];
    if (readFormBody(req, body, sizeof(body)) < 0)
        return sendError(req, 400, "bad request");

    String onVal;
    if (!getFormValue(body, "on", onVal))
        return sendError(req, 400, "missing on param");

    bool on = (onVal == "true");
    hal::buzzerSetEnabled(on);
    JsonDocument doc;
    doc["buzzer"] = on;
    String json;
    serializeJson(doc, json);
    return sendJSON(req, 200, json.c_str());
}

// GPS status
static esp_err_t handleGPS(httpd_req_t* req) {
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
    String json;
    serializeJson(doc, json);
    return sendJSON(req, 200, json.c_str());
}

// GPS set (from phone browser geolocation)
static esp_err_t handleGPSSet(httpd_req_t* req) {
    const hal::GPSData& g = hal::gpsGet();
    if (g.hwFix)
        return sendJSON(req, 200, "{\"status\":\"ignored\",\"reason\":\"hw_gps_active\"}");

    String lat, lon, acc;
    if (!getQueryParam(req, "lat", lat) || !getQueryParam(req, "lon", lon))
        return sendError(req, 400, "lat,lon required");

    float accVal = 0;
    if (getQueryParam(req, "acc", acc))
        accVal = acc.toFloat();

    hal::gpsSetFromPhone(lat.toDouble(), lon.toDouble(), accVal);
    return sendJSON(req, 200, "{\"status\":\"ok\"}");
}

// AP settings GET
static esp_err_t handleAPGet(httpd_req_t* req) {
    JsonDocument doc;
    doc["ssid"] = storage::getAPSSID();
    String json;
    serializeJson(doc, json);
    return sendJSON(req, 200, json.c_str());
}

// AP settings POST
static esp_err_t handleAPPost(httpd_req_t* req) {
    char body[256];
    if (readFormBody(req, body, sizeof(body)) < 0)
        return sendError(req, 400, "bad request");

    String ssid, pass;
    if (!getFormValue(body, "ssid", ssid))
        return sendError(req, 400, "missing ssid");

    getFormValue(body, "pass", pass); // optional

    if (ssid.length() < 1 || ssid.length() > 32)
        return sendError(req, 400, "SSID must be 1-32 characters");
    if (pass.length() > 0 && (pass.length() < 8 || pass.length() > 63))
        return sendError(req, 400, "Password must be 8-63 characters or empty for open");

    storage::setAPConfig(ssid, pass);
    sendJSON(req, 200, "{\"ok\":true,\"reboot\":true}");
    delay(500);
    ESP.restart();
    return ESP_OK;
}

// Device reset
static esp_err_t handleReset(httpd_req_t* req) {
    sendJSON(req, 200, "{\"ok\":true}");
    delay(500);
    ESP.restart();
    return ESP_OK;
}

// ============================================================================
// Server Initialization
// ============================================================================

void serverInit() {
    // --- HTTPS Server on :443 ---
    httpd_ssl_config_t httpsConfig = HTTPD_SSL_CONFIG_DEFAULT();
    httpsConfig.servercert = DEV_CERT_PEM;
    httpsConfig.servercert_len = DEV_CERT_PEM_LEN;
    httpsConfig.prvtkey_pem = DEV_KEY_PEM;
    httpsConfig.prvtkey_len = DEV_KEY_PEM_LEN;
    httpsConfig.httpd.max_uri_handlers = 55;
    httpsConfig.httpd.max_open_sockets = 2; // Keep low — each TLS session holds ~16-30KB
    httpsConfig.httpd.stack_size = 10240;   // TLS handshake needs more stack than plain HTTP
    httpsConfig.httpd.lru_purge_enable = true;

    esp_err_t ret = httpd_ssl_start(&_httpsServer, &httpsConfig);
    if (ret == ESP_OK) {
        Serial.println("[WEB] HTTPS server started on port 443");
    } else {
        Serial.printf("[WEB] HTTPS start failed: %s\n", esp_err_to_name(ret));
    }

    // --- HTTP Server on :80 (also hosts WebSocket endpoint) ---
    httpd_config_t httpConfig = HTTPD_DEFAULT_CONFIG();
    httpConfig.ctrl_port = 32769; // Must differ from HTTPS server's ctrl_port (32768)
    httpConfig.max_uri_handlers = 55;
    httpConfig.stack_size = 8192;
    httpConfig.lru_purge_enable = true;
    httpConfig.global_transport_ctx = nullptr;

    ret = httpd_start(&_httpServer, &httpConfig);
    if (ret == ESP_OK) {
        Serial.println("[WEB] HTTP server started on port 80");
    } else {
        Serial.printf("[WEB] HTTP start failed: %s\n", esp_err_to_name(ret));
    }

    // --- Register CA cert download routes on BOTH ---
    static const httpd_uri_t caCerUri = {"/ca.cer", HTTP_GET, handleCACertDER, nullptr};
    static const httpd_uri_t caPemUri = {"/ca.pem", HTTP_GET, handleCACertPEM, nullptr};
    regBoth(&caCerUri);
    regBoth(&caPemUri);

    // --- Dashboard on BOTH servers ---
    // HTTP: fast, no TLS overhead — default for most use
    // HTTPS: slower but enables browser geolocation API (secure context)
    static const httpd_uri_t dashUri = {"/", HTTP_GET, handleDashboard, nullptr};
    regBoth(&dashUri);

    // --- HTTP: captive portal detection URLs (welcome page for mini-browsers) ---
    if (_httpServer) {
        static const httpd_uri_t gen204 = {"/generate_204", HTTP_GET, handleCaptiveDetect, nullptr};
        static const httpd_uri_t hotspot = {"/hotspot-detect.html", HTTP_GET, handleCaptiveDetect,
                                            nullptr};
        static const httpd_uri_t ncsi = {"/connecttest.txt", HTTP_GET, handleCaptiveDetect,
                                         nullptr};
        static const httpd_uri_t success = {"/success.txt", HTTP_GET, handleCaptiveDetect, nullptr};
        httpd_register_uri_handler(_httpServer, &gen204);
        httpd_register_uri_handler(_httpServer, &hotspot);
        httpd_register_uri_handler(_httpServer, &ncsi);
        httpd_register_uri_handler(_httpServer, &success);
    }
}

httpd_handle_t getHTTPSServer() {
    return _httpsServer;
}

httpd_handle_t getHTTPServer() {
    return _httpServer;
}

void registerSystemRoutes(IModule** modules, int count) {
    _modules = modules;
    _moduleCount = count;

    static const httpd_uri_t statusUri = {"/api/status", HTTP_GET, handleStatus, nullptr};
    static const httpd_uri_t modulesUri = {"/api/modules", HTTP_GET, handleModules, nullptr};
    static const httpd_uri_t toggleUri = {"/api/modules/toggle", HTTP_POST, handleModuleToggle,
                                          nullptr};
    static const httpd_uri_t buzzerUri = {"/api/buzzer", HTTP_POST, handleBuzzer, nullptr};
    static const httpd_uri_t gpsUri = {"/api/gps", HTTP_GET, handleGPS, nullptr};
    static const httpd_uri_t gpsSetUri = {"/api/gps/set", HTTP_GET, handleGPSSet, nullptr};
    static const httpd_uri_t apGetUri = {"/api/ap", HTTP_GET, handleAPGet, nullptr};
    static const httpd_uri_t apPostUri = {"/api/ap", HTTP_POST, handleAPPost, nullptr};
    static const httpd_uri_t resetUri = {"/api/reset", HTTP_POST, handleReset, nullptr};

    regBoth(&statusUri);
    regBoth(&modulesUri);
    regBoth(&toggleUri);
    regBoth(&buzzerUri);
    regBoth(&gpsUri);
    regBoth(&gpsSetUri);
    regBoth(&apGetUri);
    regBoth(&apPostUri);
    regBoth(&resetUri);
}

void serverBegin() {
    // Initialize WebSocket broadcast on HTTP server
    ws::init(_httpServer);

    // Servers already started in serverInit()
    Serial.println("[WEB] All routes registered");
    Serial.println("[WEB] HTTPS: https://192.168.4.1 | HTTP: http://192.168.4.1");
}

} // namespace web
