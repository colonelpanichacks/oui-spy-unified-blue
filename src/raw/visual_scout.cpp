/*
 * Visual Scout — raw implementation (src/raw/visual_scout.cpp)
 *
 * Wrapped by src/mode_visual_scout.cpp in an anonymous namespace.
 * Entry points are setup() / loop() (renamed by the wrapper's #define trick).
 *
 * ─────────────────────────────────────────────────────────────────────────
 * FEATURES
 * ─────────────────────────────────────────────────────────────────────────
 * 1. Deflock camera DB sync
 *    - Primary:  Pi-local proxy  http://<pi_ip>:5000/flock_cameras.bin
 *    - Fallback: cdn.deflock.me  regional tile download (auto-filtered for
 *                Flock Safety brand in tile JSON)
 *    - DB stored as compact binary in LittleFS: uint32 count + N×{lat,lon}
 *
 * 2. GPS proximity alerting
 *    - TinyGPSPlus on UART2 (GPS_RX_PIN / GPS_TX_PIN)
 *    - Haversine check every PROXIMITY_CHECK_INTERVAL_MS
 *    - Buzzer + LED alert within ALERT_RADIUS_M_DEFAULT of any known camera
 *
 * 3. Camera MJPEG stream (port 81)
 *    - MJPEG stream at  http://192.168.4.1:81/stream
 *    - JPEG snapshot at http://192.168.4.1:81/snap
 *    - GPS metadata at  http://192.168.4.1:81/gps  (JSON)
 *    - Runs in a dedicated FreeRTOS task
 *
 * 4. Config AP + web UI (port 80)
 *    - SSID: ouispy-scout  (no password — or set one via web)
 *    - Configure Pi proxy IP, alert radius, WiFi creds for sync
 *    - Trigger manual sync, view GPS status, live DB stats
 *
 * ─────────────────────────────────────────────────────────────────────────
 * HARDWARE DEFAULTS  (ESP32-S3 WROOM + OV2640 breakout)
 * ─────────────────────────────────────────────────────────────────────────
 *  GPS:    RX←GPIO44  TX→GPIO43  9600 baud NMEA
 *  Camera: adjust CAM_PIN_* below for your PCB; defaults match a common
 *          ESP32-S3 WROOM + OV2640 FPC breakout
 *
 * ─────────────────────────────────────────────────────────────────────────
 * NVS namespace: "visual-scout"  (do not reuse in other modes)
 * ─────────────────────────────────────────────────────────────────────────
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <TinyGPSPlus.h>
#include "esp_camera.h"
#include "modes.h"

// ============================================================================
// Hardware
// ============================================================================

#ifndef BUZZER_PIN
#define BUZZER_PIN  3
#endif
#ifndef LED_PIN
#define LED_PIN     21
#endif

#define GPS_RX_PIN  44
#define GPS_TX_PIN  43
#define GPS_BAUD    9600

// OV2640 pin mapping — common ESP32-S3 WROOM breakout.
// Adjust for your specific PCB (see your schematic).
#define CAM_PIN_PWDN    (-1)
#define CAM_PIN_RESET   (-1)
#define CAM_PIN_XCLK     15
#define CAM_PIN_SIOD      4
#define CAM_PIN_SIOC      5
#define CAM_PIN_D7       16
#define CAM_PIN_D6       17
#define CAM_PIN_D5       18
#define CAM_PIN_D4       12
#define CAM_PIN_D3       10
#define CAM_PIN_D2        8
#define CAM_PIN_D1        9
#define CAM_PIN_D0       11
#define CAM_PIN_VSYNC     6
#define CAM_PIN_HREF      7
#define CAM_PIN_PCLK     13

// ============================================================================
// Deflock endpoints
// ============================================================================

// cdn.deflock.me tile API:
//   GET /regions/index.json            → manifest: { regions, tile_url, tile_size_degrees }
//   GET /regions/{lat20}/{lon20}.json  → array of camera nodes with lat/lon/tags
// Tile grid: snap lat/lon down to nearest 20° multiple.
// Each tile node has a "tags" object; check brand/surveillance:brand for "Flock Safety".
#define DEFLOCK_CDN_INDEX   "http://cdn.deflock.me/regions/index.json"
#define DEFLOCK_TILE_BASE   "http://cdn.deflock.me/regions"
#define DEFLOCK_MAX_CAMERAS  40000   // hard cap; 4 US tiles × ~10K Flock cameras

// ============================================================================
// Alert thresholds
// ============================================================================

#define ALERT_RADIUS_M_DEFAULT    150.0f
#define ALERT_COOLDOWN_MS         8000
#define PROXIMITY_CHECK_INTERVAL_MS 2000

// ============================================================================
// File paths
// ============================================================================

#define DEFLOCK_BIN_PATH   "/deflock.bin"
#define VS_CONFIG_PATH     "/vs_cfg.json"

// ============================================================================
// State
// ============================================================================

struct CameraPos { float lat; float lon; };

struct VSConfig {
    char  piProxyURL[128] = "";         // e.g. http://192.168.4.2:5000/flock_cameras.bin
    char  staSSID[64]     = "";         // WiFi STA for internet-direct sync
    char  staPass[64]     = "";
    float alertRadiusM    = ALERT_RADIUS_M_DEFAULT;
    bool  cameraEnabled   = true;
    bool  buzzerEnabled   = true;
};
static VSConfig cfg;

static TinyGPSPlus   gps;
static HardwareSerial gpsSerial(2);

static AsyncWebServer cfgServer(80);   // config UI

static CameraPos* deflockDB   = nullptr;
static uint32_t   deflockCount = 0;

static unsigned long lastProxCheck = 0;
static unsigned long lastAlertMs   = 0;
static int           lastAlertIdx  = -1;
static float         lastAlertDistM = 0.0f;

enum SyncState : uint8_t { SS_IDLE=0, SS_BUSY=1, SS_OK=2, SS_ERR=3 };
static SyncState syncState   = SS_IDLE;
static String    syncMsg     = "Idle";
static int       syncPct     = 0;

static bool cameraOK = false;

// MJPEG stream server — runs in its own task on port 81
static WiFiServer mjpegServer(81);
static TaskHandle_t mjpegTaskHandle = nullptr;

// ============================================================================
// Helpers
// ============================================================================

static void ledOn()  { digitalWrite(LED_PIN, LOW);  }
static void ledOff() { digitalWrite(LED_PIN, HIGH); }

static void beep(int hz, int ms) {
    if (!cfg.buzzerEnabled) return;
    ledcSetup(0, hz, 8);
    ledcAttachPin(BUZZER_PIN, 0);
    ledcWrite(0, 100);
    delay(ms);
    ledcWrite(0, 0);
}

// Haversine distance in metres
static float haversineM(float lat1, float lon1, float lat2, float lon2) {
    const float R = 6371000.0f;
    float dLat = (lat2 - lat1) * (float)DEG_TO_RAD;
    float dLon = (lon2 - lon1) * (float)DEG_TO_RAD;
    float a = sinf(dLat*0.5f)*sinf(dLat*0.5f)
            + cosf(lat1*(float)DEG_TO_RAD)*cosf(lat2*(float)DEG_TO_RAD)
            * sinf(dLon*0.5f)*sinf(dLon*0.5f);
    return R * 2.0f * atan2f(sqrtf(a), sqrtf(1.0f - a));
}

// Snap lat/lon to 20° grid (cdn.deflock.me tile coordinate)
static int tileCoord(float v) { return (int)floorf(v / 20.0f) * 20; }

// ============================================================================
// Config I/O
// ============================================================================

static void loadConfig() {
    if (!LittleFS.exists(VS_CONFIG_PATH)) return;
    File f = LittleFS.open(VS_CONFIG_PATH, "r");
    if (!f) return;
    JsonDocument doc;
    if (deserializeJson(doc, f) == DeserializationError::Ok) {
        strlcpy(cfg.piProxyURL, doc["pi_proxy"]    | "", sizeof(cfg.piProxyURL));
        strlcpy(cfg.staSSID,    doc["sta_ssid"]    | "", sizeof(cfg.staSSID));
        strlcpy(cfg.staPass,    doc["sta_pass"]    | "", sizeof(cfg.staPass));
        cfg.alertRadiusM  = doc["radius_m"]        | ALERT_RADIUS_M_DEFAULT;
        cfg.cameraEnabled = doc["cam_on"]          | true;
        cfg.buzzerEnabled = doc["buz_on"]          | true;
    }
    f.close();
}

static void saveConfig() {
    File f = LittleFS.open(VS_CONFIG_PATH, "w");
    if (!f) return;
    JsonDocument doc;
    doc["pi_proxy"] = cfg.piProxyURL;
    doc["sta_ssid"] = cfg.staSSID;
    doc["sta_pass"] = cfg.staPass;
    doc["radius_m"] = cfg.alertRadiusM;
    doc["cam_on"]   = cfg.cameraEnabled;
    doc["buz_on"]   = cfg.buzzerEnabled;
    serializeJson(doc, f);
    f.close();
}

// ============================================================================
// Deflock DB — load packed binary from LittleFS
// ============================================================================

static bool loadDeflockBin() {
    free(deflockDB); deflockDB = nullptr; deflockCount = 0;
    File f = LittleFS.open(DEFLOCK_BIN_PATH, "r");
    if (!f) { Serial.println("[VS] No deflock.bin"); return false; }
    uint32_t n = 0;
    if (f.read((uint8_t*)&n, 4) != 4 || n == 0 || n > DEFLOCK_MAX_CAMERAS) {
        f.close(); return false;
    }
    deflockDB = (CameraPos*)ps_malloc(n * sizeof(CameraPos));
    if (!deflockDB) deflockDB = (CameraPos*)malloc(n * sizeof(CameraPos));
    if (!deflockDB) { f.close(); return false; }
    size_t got = f.read((uint8_t*)deflockDB, n * sizeof(CameraPos));
    f.close();
    if (got != n * sizeof(CameraPos)) {
        free(deflockDB); deflockDB = nullptr; return false;
    }
    deflockCount = n;
    Serial.printf("[VS] Loaded %u Flock cameras from flash\n", deflockCount);
    return true;
}

// ============================================================================
// Deflock sync — FreeRTOS task
// ============================================================================

/*
 * Sync strategy (in priority order):
 *
 * A) Pi proxy (preferred):
 *    GET cfg.piProxyURL  → raw binary (uint32 count + N×{float lat, float lon})
 *    The Pi's deflock_downloader.py already filters for Flock Safety and
 *    outputs this format directly.
 *
 * B) cdn.deflock.me tiles (fallback, requires STA WiFi with internet):
 *    1. GET cdn.deflock.me/regions/index.json  → manifest
 *    2. For each tile in the US (all regions containing Flock Safety cameras),
 *       GET cdn.deflock.me/regions/{lat}/{lon}.json
 *       Filter nodes where tags.brand == "Flock Safety" OR
 *                         tags["surveillance:brand"] == "Flock Safety"
 *    3. Write packed binary to LittleFS
 *
 * B is slow (~10 tiles × up to 500 KB each) and may not fit in one session.
 * Highly recommend using the Pi proxy for regular updates.
 */

static bool syncViaPiProxy() {
    if (strlen(cfg.piProxyURL) == 0) return false;
    Serial.printf("[VS] Trying Pi proxy: %s\n", cfg.piProxyURL);
    syncMsg = "Connecting to Pi proxy…";
    syncPct = 10;

    HTTPClient http;
    http.setTimeout(15000);
    http.begin(cfg.piProxyURL);
    int code = http.GET();
    if (code != 200) {
        Serial.printf("[VS] Pi proxy HTTP %d\n", code);
        http.end(); return false;
    }

    File f = LittleFS.open(DEFLOCK_BIN_PATH ".tmp", "w");
    if (!f) { http.end(); return false; }

    WiFiClient* stream = http.getStreamPtr();
    uint8_t buf[512];
    int total = http.getSize(), received = 0;
    while (http.connected() && (total == -1 || received < total)) {
        size_t avail = stream->available();
        if (!avail) { delay(10); continue; }
        size_t rd = stream->readBytes(buf, min(avail, sizeof(buf)));
        f.write(buf, rd);
        received += (int)rd;
        if (total > 0) syncPct = 10 + (received * 80) / total;
    }
    f.close();
    http.end();

    if (received < 8) { LittleFS.remove(DEFLOCK_BIN_PATH ".tmp"); return false; }
    LittleFS.remove(DEFLOCK_BIN_PATH);
    LittleFS.rename(DEFLOCK_BIN_PATH ".tmp", DEFLOCK_BIN_PATH);
    return true;
}

// Check if a CDN tile node's tags indicate Flock Safety
static bool isFlockSafety(JsonObject tags) {
    const char* fields[] = { "brand", "surveillance:brand",
                              "surveillance:manufacturer", "operator" };
    for (const char* f : fields) {
        const char* v = tags[f] | "";
        if (strstr(v, "Flock") || strstr(v, "flock")) return true;
    }
    return false;
}

static bool syncViaCDN() {
    Serial.println("[VS] Trying cdn.deflock.me tile sync");
    syncMsg = "Fetching CDN index…";
    syncPct = 5;

    // Connect STA
    WiFi.mode(WIFI_AP_STA);
    WiFi.begin(cfg.staSSID, cfg.staPass);
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) delay(500);
    if (WiFi.status() != WL_CONNECTED) {
        syncMsg = "STA connect failed";
        WiFi.disconnect(); WiFi.mode(WIFI_AP);
        return false;
    }

    // Fetch index
    HTTPClient http;
    http.setTimeout(20000);
    http.begin(DEFLOCK_CDN_INDEX);
    int code = http.GET();
    if (code != 200) {
        http.end(); WiFi.disconnect(); WiFi.mode(WIFI_AP);
        syncMsg = "CDN index HTTP " + String(code);
        return false;
    }
    JsonDocument idxDoc;
    deserializeJson(idxDoc, *http.getStreamPtr());
    http.end();

    JsonArray regions = idxDoc["regions"].as<JsonArray>();
    int nRegions = regions.size();
    if (nRegions == 0) {
        WiFi.disconnect(); WiFi.mode(WIFI_AP);
        syncMsg = "CDN index empty";
        return false;
    }

    File binFile = LittleFS.open(DEFLOCK_BIN_PATH ".tmp", "w");
    if (!binFile) { WiFi.disconnect(); WiFi.mode(WIFI_AP); return false; }
    uint32_t count = 0;
    binFile.write((uint8_t*)&count, 4);   // placeholder for count

    int ri = 0;
    for (JsonVariant region : regions) {
        const char* reg = region.as<const char*>();   // e.g. "40/-100"
        String tileURL = String(DEFLOCK_TILE_BASE) + "/" + reg + ".json";

        syncMsg  = "Tile " + String(++ri) + "/" + String(nRegions) + "…";
        syncPct  = 10 + (ri * 80) / nRegions;
        Serial.printf("[VS] Fetching tile %s\n", reg);

        HTTPClient th;
        th.setTimeout(30000);
        th.begin(tileURL);
        int tc = th.GET();
        if (tc != 200) { th.end(); continue; }

        // Stream-parse the array; each element is {id, lat, lon, tags:{...}}
        WiFiClient* ts = th.getStreamPtr();
        JsonDocument tile;
        DeserializationError err = deserializeJson(tile, *ts);
        th.end();
        if (err) continue;

        for (JsonVariant node : tile.as<JsonArray>()) {
            if (count >= DEFLOCK_MAX_CAMERAS) break;
            JsonObject tags = node["tags"].as<JsonObject>();
            if (!isFlockSafety(tags)) continue;
            CameraPos pos;
            pos.lat = node["lat"].as<float>();
            pos.lon = node["lon"].as<float>();
            if (pos.lat == 0.0f && pos.lon == 0.0f) continue;
            binFile.write((uint8_t*)&pos, sizeof(pos));
            count++;
        }
        if (count >= DEFLOCK_MAX_CAMERAS) break;
        delay(200);   // be polite to cdn.deflock.me
    }

    binFile.seek(0);
    binFile.write((uint8_t*)&count, 4);
    binFile.close();
    WiFi.disconnect(); WiFi.mode(WIFI_AP);

    if (count == 0) {
        LittleFS.remove(DEFLOCK_BIN_PATH ".tmp");
        syncMsg = "No Flock cameras found in tiles";
        return false;
    }
    LittleFS.remove(DEFLOCK_BIN_PATH);
    LittleFS.rename(DEFLOCK_BIN_PATH ".tmp", DEFLOCK_BIN_PATH);
    Serial.printf("[VS] CDN sync: %u Flock cameras saved\n", count);
    return true;
}

static void syncTask(void* /*param*/) {
    syncState = SS_BUSY;
    syncPct   = 0;

    bool ok = syncViaPiProxy();
    if (!ok) ok = syncViaCDN();

    if (ok) {
        loadDeflockBin();
        syncMsg   = "Sync complete — " + String(deflockCount) + " cameras";
        syncState = SS_OK;
        syncPct   = 100;
        beep(2000, 80); delay(40); beep(2800, 120);
    } else {
        syncMsg   = syncMsg.length() ? syncMsg : "Sync failed";
        syncState = SS_ERR;
        syncPct   = 0;
    }
    vTaskDelete(nullptr);
}

static void startSync() {
    if (syncState == SS_BUSY) return;
    if (strlen(cfg.piProxyURL) == 0 && strlen(cfg.staSSID) == 0) {
        syncMsg = "Configure Pi proxy URL or WiFi STA credentials first";
        syncState = SS_ERR;
        return;
    }
    xTaskCreatePinnedToCore(syncTask, "vs_sync", 20480, nullptr, 1, nullptr, 1);
}

// ============================================================================
// Camera init
// ============================================================================

static bool initCamera() {
    camera_config_t c = {};
    c.ledc_channel  = LEDC_CHANNEL_1;
    c.ledc_timer    = LEDC_TIMER_1;
    c.pin_d0        = CAM_PIN_D0;   c.pin_d1    = CAM_PIN_D1;
    c.pin_d2        = CAM_PIN_D2;   c.pin_d3    = CAM_PIN_D3;
    c.pin_d4        = CAM_PIN_D4;   c.pin_d5    = CAM_PIN_D5;
    c.pin_d6        = CAM_PIN_D6;   c.pin_d7    = CAM_PIN_D7;
    c.pin_xclk      = CAM_PIN_XCLK; c.pin_pclk  = CAM_PIN_PCLK;
    c.pin_vsync     = CAM_PIN_VSYNC; c.pin_href  = CAM_PIN_HREF;
    c.pin_sccb_sda  = CAM_PIN_SIOD; c.pin_sccb_scl = CAM_PIN_SIOC;
    c.pin_pwdn      = CAM_PIN_PWDN; c.pin_reset = CAM_PIN_RESET;
    c.xclk_freq_hz  = 20000000;
    c.pixel_format  = PIXFORMAT_JPEG;
    if (psramFound()) {
        c.frame_size   = FRAMESIZE_VGA;
        c.jpeg_quality = 12;
        c.fb_count     = 2;
        c.fb_location  = CAMERA_FB_IN_PSRAM;
        c.grab_mode    = CAMERA_GRAB_LATEST;
    } else {
        c.frame_size   = FRAMESIZE_QVGA;
        c.jpeg_quality = 20;
        c.fb_count     = 1;
        c.fb_location  = CAMERA_FB_IN_DRAM;
        c.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;
    }
    esp_err_t err = esp_camera_init(&c);
    if (err != ESP_OK) { Serial.printf("[VS] Camera init err: 0x%x\n", err); return false; }
    Serial.println("[VS] Camera OK");
    return true;
}

// ============================================================================
// MJPEG server task  (port 81, plain TCP — more reliable than AsyncWebServer
//                     for continuous streaming connections)
// ============================================================================

#define MJPEG_BOUNDARY "flockhunter"

static void sendGpsJson(WiFiClient& client) {
    String body = "{\"valid\":";
    body += gps.location.isValid() ? "true" : "false";
    body += ",\"lat\":" + String(gps.location.isValid() ? gps.location.lat() : 0.0, 6);
    body += ",\"lon\":" + String(gps.location.isValid() ? gps.location.lng() : 0.0, 6);
    body += ",\"sats\":" + String(gps.satellites.isValid() ? (int)gps.satellites.value() : 0);
    body += ",\"ts_ms\":" + String(millis()) + "}";
    client.print("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                 "Access-Control-Allow-Origin: *\r\n"
                 "Content-Length: " + String(body.length()) + "\r\n\r\n");
    client.print(body);
}

static void sendSnapshot(WiFiClient& client) {
    if (!cameraOK) { client.print("HTTP/1.1 503 Service Unavailable\r\n\r\n"); return; }
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb)       { client.print("HTTP/1.1 500 Internal Server Error\r\n\r\n"); return; }
    client.printf("HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\n"
                  "Content-Length: %u\r\nAccess-Control-Allow-Origin: *\r\n\r\n", fb->len);
    client.write(fb->buf, fb->len);
    esp_camera_fb_return(fb);
}

static void streamMjpeg(WiFiClient& client) {
    if (!cameraOK) { client.print("HTTP/1.1 503 Service Unavailable\r\n\r\n"); return; }
    client.print("HTTP/1.1 200 OK\r\nContent-Type: multipart/x-mixed-replace;"
                 "boundary=" MJPEG_BOUNDARY "\r\n"
                 "Access-Control-Allow-Origin: *\r\n\r\n");
    while (client.connected()) {
        camera_fb_t* fb = esp_camera_fb_get();
        if (!fb) { delay(30); continue; }
        client.printf("--" MJPEG_BOUNDARY "\r\n"
                      "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", fb->len);
        client.write(fb->buf, fb->len);
        client.print("\r\n");
        esp_camera_fb_return(fb);
        delay(30);   // ~33 fps cap; reduce if CPU-bound
    }
}

static void mjpegTask(void* /*param*/) {
    mjpegServer.begin();
    Serial.println("[VS] MJPEG server listening on :81");
    for (;;) {
        WiFiClient client = mjpegServer.accept();
        if (!client) { delay(10); continue; }

        // Read the request line
        String req = client.readStringUntil('\n');
        // Drain headers
        while (client.connected()) {
            String line = client.readStringUntil('\n');
            if (line == "\r" || line.length() == 0) break;
        }

        if      (req.indexOf("/stream") >= 0) streamMjpeg(client);
        else if (req.indexOf("/snap")   >= 0) sendSnapshot(client);
        else if (req.indexOf("/gps")    >= 0) sendGpsJson(client);
        else { client.print("HTTP/1.1 404 Not Found\r\n\r\n"); }
        client.stop();
    }
}

// ============================================================================
// Config web UI HTML  (port 80)
// ============================================================================

static const char VS_HTML[] PROGMEM = R"html(<!DOCTYPE html>
<html><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Visual Scout</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font:12px monospace;background:#000;color:#0f0;padding:10px}
h1{font-size:16px;letter-spacing:2px;margin-bottom:10px;border-bottom:1px solid #0f0;padding-bottom:6px}
h2{font-size:11px;margin:10px 0 4px;text-transform:uppercase;opacity:.7}
input[type=text],input[type=password],input[type=number]{
  width:100%;padding:5px;background:#000;color:#0f0;border:1px solid #0f0;
  font:12px monospace;margin-bottom:6px}
button{padding:5px 12px;background:#0f0;color:#000;border:none;font:bold 11px monospace;
  cursor:pointer;margin:2px 4px 2px 0}
button:active{background:#fff}
.ok{color:#0f0}.warn{color:#ff0}.err{color:#f00}
.card{border:1px solid #0f0;padding:8px;margin-bottom:8px;font-size:11px;line-height:1.8}
.sm{font-size:10px;opacity:.7}
</style></head><body>
<h1>&#x1F4F9; VISUAL SCOUT</h1>

<div class="card" id="gpsCard">GPS: loading…</div>
<div class="card" id="dbCard">DB: loading…</div>
<div class="card" id="camCard">Camera: loading…</div>

<h2>Pi Proxy URL <span class="sm">(fastest sync — run pi/deflock_downloader.py first)</span></h2>
<input type="text" id="pi_proxy" placeholder="http://192.168.0.X:5000/flock_cameras.bin">

<h2>WiFi for direct CDN sync <span class="sm">(fallback if no Pi)</span></h2>
<input type="text" id="sta_ssid" placeholder="Network SSID">
<input type="password" id="sta_pass" placeholder="Password">

<h2>Alert Radius (metres)</h2>
<input type="number" id="radius_m" min="10" max="5000" step="10" value="150">

<div>
  <button onclick="save()">SAVE</button>
  <button onclick="syncNow()">SYNC DB</button>
  <button onclick="location.href='/menu'" style="background:#f00;color:#fff">MENU</button>
</div>
<div id="msg" class="sm" style="margin-top:6px"></div>

<script>
function poll(){
  fetch('/status').then(r=>r.json()).then(d=>{
    var gv=d.gps_valid;
    document.getElementById('gpsCard').innerHTML=
      '<span class="'+(gv?'ok':'warn')+'">'
      +(gv?'&#x1F6F0; '+d.gps_lat.toFixed(6)+', '+d.gps_lon.toFixed(6)+
            ' &bull; '+d.gps_sats+' sats &bull; HDOP '+d.gps_hdop.toFixed(1)
          :'No GPS fix — check GPIO '+d.gps_rx+'/'+d.gps_tx+' wiring')
      +'</span>'+(d.last_alert_idx>=0?'<br><span class="warn">&#x26A0; Last alert: camera #'+d.last_alert_idx+' @ '+d.last_alert_dist_m.toFixed(0)+' m</span>':'');
    var dbOk=d.db_cameras>0;
    var ss=['Idle','Syncing…','&#x2705; Done','&#x274C; Error'];
    document.getElementById('dbCard').innerHTML=
      '<span class="'+(dbOk?'ok':'warn')+'">'+d.db_cameras+' Flock cameras</span>'
      +(d.sync_state!==0?' &bull; '+ss[d.sync_state]+': '+d.sync_msg+' ('+d.sync_pct+'%)':'');
    document.getElementById('camCard').innerHTML=
      d.camera_ok
        ? '&#x2705; Stream: <a href="http://'+location.hostname+':81/stream" target="_blank" style="color:#0f0">:81/stream</a>'
          +' &bull; <a href="http://'+location.hostname+':81/snap" target="_blank" style="color:#0f0">snapshot</a>'
          +' &bull; GPS JSON: <a href="http://'+location.hostname+':81/gps" target="_blank" style="color:#0f0">:81/gps</a>'
        : '<span class="err">Camera not detected — check CAM_PIN_* wiring</span>';
  }).catch(()=>{});
}
function save(){
  var b=new URLSearchParams();
  ['pi_proxy','sta_ssid','sta_pass','radius_m'].forEach(k=>b.append(k,document.getElementById(k).value));
  fetch('/saveconfig',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b})
    .then(r=>r.json()).then(d=>document.getElementById('msg').textContent=d.ok?'Saved!':'Error');
}
function syncNow(){
  document.getElementById('msg').textContent='Sync started…';
  fetch('/sync').then(r=>r.text()).then(t=>document.getElementById('msg').textContent=t);
}
fetch('/cfgvals').then(r=>r.json()).then(d=>{
  document.getElementById('pi_proxy').value=d.pi_proxy||'';
  document.getElementById('sta_ssid').value=d.sta_ssid||'';
  document.getElementById('radius_m').value=d.radius_m||150;
});
setInterval(poll,3000); poll();
</script></body></html>)html";

// ============================================================================
// Web server endpoints
// ============================================================================

static void setupWebServer() {
    cfgServer.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send_P(200, "text/html", VS_HTML);
    });

    cfgServer.on("/status", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument doc;
        doc["gps_valid"]         = gps.location.isValid();
        doc["gps_lat"]           = gps.location.isValid() ? gps.location.lat() : 0.0;
        doc["gps_lon"]           = gps.location.isValid() ? gps.location.lng() : 0.0;
        doc["gps_sats"]          = gps.satellites.isValid() ? (int)gps.satellites.value() : 0;
        doc["gps_hdop"]          = gps.hdop.isValid() ? gps.hdop.hdop() : 0.0;
        doc["gps_rx"]            = GPS_RX_PIN;
        doc["gps_tx"]            = GPS_TX_PIN;
        doc["db_cameras"]        = (int)deflockCount;
        doc["sync_state"]        = (int)syncState;
        doc["sync_msg"]          = syncMsg;
        doc["sync_pct"]          = syncPct;
        doc["alert_radius_m"]    = cfg.alertRadiusM;
        doc["last_alert_dist_m"] = lastAlertDistM;
        doc["last_alert_idx"]    = lastAlertIdx;
        doc["camera_ok"]         = cameraOK;
        String out; serializeJson(doc, out);
        req->send(200, "application/json", out);
    });

    cfgServer.on("/cfgvals", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument doc;
        doc["pi_proxy"] = cfg.piProxyURL;
        doc["sta_ssid"] = cfg.staSSID;
        doc["radius_m"] = cfg.alertRadiusM;
        String out; serializeJson(doc, out);
        req->send(200, "application/json", out);
    });

    cfgServer.on("/saveconfig", HTTP_POST, [](AsyncWebServerRequest* req) {
        if (req->hasParam("pi_proxy", true))
            strlcpy(cfg.piProxyURL, req->getParam("pi_proxy", true)->value().c_str(), sizeof(cfg.piProxyURL));
        if (req->hasParam("sta_ssid", true))
            strlcpy(cfg.staSSID, req->getParam("sta_ssid", true)->value().c_str(), sizeof(cfg.staSSID));
        if (req->hasParam("sta_pass", true))
            strlcpy(cfg.staPass, req->getParam("sta_pass", true)->value().c_str(), sizeof(cfg.staPass));
        if (req->hasParam("radius_m", true))
            cfg.alertRadiusM = req->getParam("radius_m", true)->value().toFloat();
        saveConfig();
        req->send(200, "application/json", "{\"ok\":true}");
    });

    cfgServer.on("/sync", HTTP_GET, [](AsyncWebServerRequest* req) {
        startSync();
        req->send(200, "text/plain", "Sync started");
    });

    cfgServer.on("/menu", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(200, "text/plain", "Returning to menu…");
        Preferences p; p.begin("unified-mode", false); p.putInt("mode", 0); p.end();
        delay(500); ESP.restart();
    });

    cfgServer.onNotFound([](AsyncWebServerRequest* req) {
        req->redirect("http://192.168.4.1/");
    });

    cfgServer.begin();
    Serial.println("[VS] Config UI on :80  http://192.168.4.1/");
}

// ============================================================================
// Proximity check
// ============================================================================

static void checkProximity() {
    if (!gps.location.isValid() || deflockCount == 0) return;
    float myLat = (float)gps.location.lat();
    float myLon = (float)gps.location.lng();
    float closest = 1e9f; int ci = -1;
    for (uint32_t i = 0; i < deflockCount; i++) {
        float d = haversineM(myLat, myLon, deflockDB[i].lat, deflockDB[i].lon);
        if (d < closest) { closest = d; ci = (int)i; }
    }
    if (ci < 0 || closest > cfg.alertRadiusM) return;
    unsigned long now = millis();
    if (ci == lastAlertIdx && (now - lastAlertMs) < ALERT_COOLDOWN_MS) return;
    lastAlertIdx   = ci;
    lastAlertDistM = closest;
    lastAlertMs    = now;
    Serial.printf("[VS] *** FLOCK CAMERA NEARBY: #%d at %.0f m (%.6f, %.6f) ***\n",
                  ci, closest, deflockDB[ci].lat, deflockDB[ci].lon);
    ledOn();
    beep(1500, 80); delay(40); beep(2000, 80); delay(40); beep(2800, 120);
    ledOff();
}

// ============================================================================
// Arduino entry points  (renamed to setup_ns / loop_ns by wrapper)
// ============================================================================

void setup() {
    Serial.println("[VS] ===== VISUAL SCOUT =====");

    if (!LittleFS.begin(true)) Serial.println("[VS] LittleFS failed");

    loadConfig();

    gpsSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
    Serial.printf("[VS] GPS on RX=%d TX=%d @%d\n", GPS_RX_PIN, GPS_TX_PIN, GPS_BAUD);

    if (cfg.cameraEnabled) cameraOK = initCamera();

    loadDeflockBin();

    WiFi.persistent(false);
    WiFi.mode(WIFI_AP);
    WiFi.softAP("ouispy-scout", "");       // open AP; set password via web UI if desired
    Serial.printf("[VS] AP: ouispy-scout  http://%s\n", WiFi.softAPIP().toString().c_str());

    setupWebServer();

    // Start MJPEG server in a separate task pinned to core 0
    xTaskCreatePinnedToCore(mjpegTask, "mjpeg", 8192, nullptr, 5, &mjpegTaskHandle, 0);

    beep(2000, 60); delay(40); beep(2500, 60); delay(40); beep(3000, 100);

    Serial.println("[VS] Stream: http://192.168.4.1:81/stream");
    Serial.println("[VS] Config: http://192.168.4.1/");
    if (deflockCount == 0)
        Serial.println("[VS] No DB — open config UI and press SYNC DB");
}

void loop() {
    while (gpsSerial.available()) gps.encode(gpsSerial.read());
    unsigned long now = millis();
    if (now - lastProxCheck >= PROXIMITY_CHECK_INTERVAL_MS) {
        lastProxCheck = now;
        checkProximity();
    }
}

void stop() {
    if (mjpegTaskHandle) { vTaskDelete(mjpegTaskHandle); mjpegTaskHandle = nullptr; }
    esp_camera_deinit();
    gpsSerial.end();
}
