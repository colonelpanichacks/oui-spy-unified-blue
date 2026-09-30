/*
 * OUI SPY - Unified Firmware Boot Selector
 * colonelpanichacks
 *
 * On boot: creates AP "ouispy" with web UI to select firmware mode 1-5.
 * After selection, stores mode in NVS and reboots into that firmware.
 * To return to selector: hold KEY1 (front button) ~1.5 seconds.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <nvs_flash.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include "modes.h"
#include "board_pins.h"
#include "board_hw.h"
#include "board_gps.h"
#include "nesso_ui.h"

#define BOOT_HOLD_TIME 1500  // ms - hold boot button this long to force selector
#define SELECTOR_CONFIRM_MAX_MS 1200  // KEY1 tap (shorter than hold-for-menu)
#ifdef NESSO_NO_FLOCKYOU
#define DEFAULT_BOOT_MODE 6  // Mega_Maid
#else
#define DEFAULT_BOOT_MODE 4  // Flock-You
#endif

static Preferences prefs;
static AsyncWebServer selectorServer(80);
static DNSServer selectorDNS;
static int currentMode = 0;

#ifdef NESSO_NO_FLOCKYOU
static const int kSelectorModes[] = {1, 2, 5, 6};
static const char* kSelectorNames[] = {"DETECTOR", "FOXHUNTER", "SKY SPY", "MEGA MAID"};
static const int kSelectorModeCount = 4;
#else
static const int kSelectorModes[] = {1, 2, 4, 5, 6};
static const char* kSelectorNames[] = {"DETECTOR", "FOXHUNTER", "FLOCK-YOU", "SKY SPY", "MEGA MAID"};
static const int kSelectorModeCount = 5;
#endif
static int selectorHighlight = 0;

static void selectorApplyMode(int mode) {
#ifdef NESSO_NO_FLOCKYOU
    if (mode != 1 && mode != 2 && mode != 5 && mode != 6) {
#else
    if (mode != 1 && mode != 2 && mode != 4 && mode != 5 && mode != 6) {
#endif
        return;
    }
    Serial.printf("[OUI-SPY] SELECT MODE %d - storing and rebooting\n", mode);

    Preferences resetPrefs;
    resetPrefs.begin("ouispy-rst", false);
    resetPrefs.putBool("flag", false);
    resetPrefs.end();

    prefs.begin("unified-mode", false);
    prefs.putInt("mode", mode);
    prefs.putBool("selector", false);
    prefs.end();

    prefs.begin("unified-mode", true);
    int verify = prefs.getInt("mode", -1);
    prefs.end();
    Serial.printf("[OUI-SPY] NVS VERIFY: wrote %d, read back %d - %s\n",
                  mode, verify, (verify == mode) ? "OK" : "MISMATCH!");
    Serial.flush();

    nessoUiSetStatus("Rebooting...");
    delay(500);
    ESP.restart();
}

static void selectorUpdateDisplay() {
    nessoUiSetSelectorChoice(kSelectorNames[selectorHighlight],
                             selectorHighlight, kSelectorModeCount);
}

static void selectorNavBeep() {
    boardLedcConfigure(BUZZER_FREQ);
    boardBuzzerAttach();
    boardLedcSetDuty(50);
    delay(30);
    boardLedcSetDuty(0);
}

static void checkSelectorButtons() {
    if (currentMode != 0) {
        return;
    }

    static bool key2WasDown = false;
    bool key2Down = boardSecondaryButtonPressed();
    if (key2Down && !key2WasDown) {
        selectorHighlight = (selectorHighlight + 1) % kSelectorModeCount;
        selectorUpdateDisplay();
        selectorNavBeep();
        Serial.printf("[OUI-SPY] Selector highlight: %s (mode %d)\n",
                      kSelectorNames[selectorHighlight],
                      kSelectorModes[selectorHighlight]);
    }
    key2WasDown = key2Down;

    static bool key1WasDown = false;
    static unsigned long key1DownMs = 0;
    bool key1Down = boardMenuButtonPressed();
    if (key1Down && !key1WasDown) {
        key1WasDown = true;
        key1DownMs = millis();
    } else if (!key1Down && key1WasDown) {
        key1WasDown = false;
        unsigned long held = millis() - key1DownMs;
        if (held > 50 && held < SELECTOR_CONFIRM_MAX_MS) {
            int mode = kSelectorModes[selectorHighlight];
            Serial.printf("[OUI-SPY] KEY1 confirm -> mode %d (%s)\n",
                          mode, kSelectorNames[selectorHighlight]);
            selectorNavBeep();
            delay(80);
            selectorNavBeep();
            selectorApplyMode(mode);
        }
    }
}

static void requestSelectorOnReboot() {
    prefs.begin("unified-mode", false);
    prefs.putBool("selector", true);
    prefs.end();
}

static void clearSelectorRequest() {
    prefs.begin("unified-mode", false);
    prefs.putBool("selector", false);
    prefs.end();
}

static int resolveBootMode(bool forceSelector) {
    if (forceSelector) {
        return 0;
    }

    prefs.begin("unified-mode", true);
    int stored = prefs.getInt("mode", DEFAULT_BOOT_MODE);
    bool wantSelector = prefs.getBool("selector", false);
    prefs.end();

    if (wantSelector) {
        clearSelectorRequest();
        return 0;
    }

#ifdef NESSO_NO_FLOCKYOU
    if (stored == 4) {
        return 6;  // Legacy Flock-You NVS -> Mega_Maid
    }
#endif
    if (stored == 1 || stored == 2 || stored == 5 || stored == 6) {
        return stored;
    }

    return DEFAULT_BOOT_MODE;
}

// AP configuration (loaded from NVS, user-configurable via web UI)
static String apSSID = "oui-spy";
static String apPassword = "ouispy123";

// Buzzer configuration (shared across all modes via NVS)
static bool buzzerEnabled = true;

// ============================================================================
// AP Config Storage (NVS)
// ============================================================================
static void loadAPConfig() {
    Preferences apPrefs;
    apPrefs.begin("ouispy-ap", true);  // read-only
    apSSID = apPrefs.getString("ssid", "oui-spy");
    apPassword = apPrefs.getString("pass", "ouispy123");
    apPrefs.end();
    Serial.printf("[OUI-SPY] Loaded AP config: SSID='%s' PASS='%s'\n", apSSID.c_str(), apPassword.c_str());
}

static void saveAPConfig(const String& ssid, const String& pass) {
    Preferences apPrefs;
    apPrefs.begin("ouispy-ap", false);
    apPrefs.putString("ssid", ssid);
    apPrefs.putString("pass", pass);
    apPrefs.end();
    Serial.printf("[OUI-SPY] Saved AP config: SSID='%s' PASS='%s'\n", ssid.c_str(), pass.c_str());
}

// ============================================================================
// Buzzer Config Storage (NVS) — shared across all modes
// ============================================================================
static void loadBuzzerConfig() {
    Preferences bzPrefs;
    bzPrefs.begin("ouispy-bz", true);
    buzzerEnabled = bzPrefs.getBool("on", true);
    bzPrefs.end();
    Serial.printf("[OUI-SPY] Buzzer: %s\n", buzzerEnabled ? "ON" : "OFF");
}

static void saveBuzzerConfig(bool enabled) {
    Preferences bzPrefs;
    bzPrefs.begin("ouispy-bz", false);
    bzPrefs.putBool("on", enabled);
    bzPrefs.end();
    buzzerEnabled = enabled;
    Serial.printf("[OUI-SPY] Buzzer saved: %s\n", enabled ? "ON" : "OFF");
}

// ============================================================================
// MAC Address Randomization
// ============================================================================
static void randomizeMAC() {
    uint8_t mac[6];
    // Generate random bytes using ESP32 hardware RNG
    uint32_t r1 = esp_random();
    uint32_t r2 = esp_random();
    mac[0] = (r1 >> 0) & 0xFF;
    mac[1] = (r1 >> 8) & 0xFF;
    mac[2] = (r1 >> 16) & 0xFF;
    mac[3] = (r1 >> 24) & 0xFF;
    mac[4] = (r2 >> 0) & 0xFF;
    mac[5] = (r2 >> 8) & 0xFF;
    // Set locally administered bit (bit 1 of first byte) and clear multicast bit (bit 0)
    mac[0] = (mac[0] | 0x02) & 0xFE;
    
    esp_wifi_set_mac(WIFI_IF_AP, mac);
    Serial.printf("[OUI-SPY] Randomized MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// ============================================================================
// Selector Web UI HTML
// ============================================================================
#ifdef NESSO_NO_FLOCKYOU
static const char SELECTOR_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no">
<title>OUI SPY</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
html{height:100%;height:-webkit-fill-available;overflow:hidden}
body{margin:0;height:100vh;height:-webkit-fill-available;font-family:monospace;background:#000;color:#0f0;display:flex;flex-direction:column;padding:4px;overflow:hidden}
.t{flex:1;display:flex;flex-direction:column;border:2px solid #0f0;padding:6px;overflow:hidden;min-height:0}
.h{text-align:center;padding-bottom:3px;margin-bottom:3px;border-bottom:1px solid #0f0;flex-shrink:0}
.ti{font-size:24px;font-weight:bold;letter-spacing:2px}
.s{font-size:8px;margin-top:1px;opacity:.7}
#x{flex:1;display:flex;flex-direction:column;min-height:0;overflow:hidden}
.m{flex:1;display:flex;flex-direction:column;min-height:0;overflow:hidden}
.i{flex:1;display:flex;flex-direction:column;justify-content:center;align-items:center;border:2px solid #0f0;border-bottom:0;cursor:pointer;background:#000;text-align:center;min-height:0;overflow:hidden}
.i:last-child{border-bottom:2px solid #0f0}
.i:active{background:#0f0;color:#000}
.n{font-size:18px;font-weight:bold;letter-spacing:1px}
.d{font-size:9px;opacity:.7;margin-top:1px}
.ap{display:flex;gap:3px;align-items:center;margin-top:4px;border-top:1px solid #0f0;padding-top:4px;flex-shrink:0}
.ap input{flex:1;padding:4px;background:#000;color:#0f0;border:1px solid #0f0;font-family:monospace;font-size:11px;min-width:0}
.ap input:focus{outline:none;border-color:#fff;color:#fff}
.ap .sb{padding:4px 7px;background:#0f0;color:#000;border:none;font-family:monospace;font-size:10px;font-weight:bold;cursor:pointer;white-space:nowrap}
.ap .sb:active{background:#fff}
.bz{display:flex;align-items:center;white-space:nowrap;cursor:pointer;font-size:9px;gap:2px;opacity:.7}
.bz:hover{opacity:1}
.bz input{margin:0;cursor:pointer}
.f{padding-top:2px;margin-top:3px;font-size:7px;text-align:center;opacity:.5;flex-shrink:0}
.boot{flex:1;display:flex;flex-direction:column;justify-content:center;align-items:center;text-align:center;padding:20px}
.bt{font-size:28px;font-weight:bold;margin-bottom:16px;letter-spacing:2px}
.bs{font-size:12px;line-height:1.5;margin-bottom:16px;opacity:.9;max-width:500px}
.br{font-size:13px}
@keyframes b{0%,50%{opacity:1}51%,100%{opacity:0}}
.blink{animation:b 1s infinite}
</style></head><body>
<div class="t">
<div class="h"><div class="ti">OUI SPY</div><div class="s">FIRMWARE SELECTOR</div></div>
<div id="x">
<div class="m">
<div class="i" onclick="go(1)"><div class="n">DETECTOR</div><div class="d">BLE Alert Tool for Specific Devices</div></div>
<div class="i" onclick="go(2)"><div class="n">FOXHUNTER</div><div class="d">RSSI Proximity Tracker</div></div>
<div class="i" onclick="go(5)"><div class="n">SKY SPY</div><div class="d">Drone Remote ID Monitor</div></div>
<div class="i" onclick="go(6)"><div class="n">MEGA MAID</div><div class="d">BLE + WiFi MAC/OUI collector &bull; time-sliced AP</div></div>
</div>
<div class="ap">
<input type="text" id="ap_ssid" placeholder="SSID" maxlength="32" value="%SSID%">
<input type="text" id="ap_pass" placeholder="PASSWORD" maxlength="63" value="%PASS%">
<button class="sb" onclick="saveAP()">SET</button>
<label class="bz"><input type="checkbox" id="bz" onchange="saveBZ(this.checked)" %BUZZER%>BZR</label>
</div>
<div class="f" id="ft">Hold BOOT 2s for menu &bull; MAC randomized</div>
</div>
<div id="y" class="boot" style="display:none">
<div class="bt" id="yt"></div>
<div class="bs" id="ys"></div>
<div class="br">REBOOTING<span class="blink">_</span></div>
</div>
</div>
<script>
var info={1:{t:'DETECTOR',s:'Scans for BLE devices and alerts when specific targets are detected. Configure OUI prefixes and MAC addresses to monitor.'},2:{t:'FOXHUNTER',s:'Track down a specific device using RSSI signal strength. Beeps get faster as you get closer to your target.'},5:{t:'SKY SPY',s:'Monitors for FAA Remote ID broadcasts from drones. Detects Open Drone ID signals over WiFi and BLE.'},6:{t:'MEGA MAID',s:'Collects all BLE and WiFi MAC addresses (no watchlist). Boots into dashboard mode (megamaid AP + web export). Double-click KEY1 to start collection mode (50/50 BLE/WiFi).'}};
function go(m){var d=info[m];document.getElementById('yt').textContent=d.t;document.getElementById('ys').textContent=d.s;document.getElementById('x').style.display='none';document.getElementById('y').style.display='flex';fetch('/select?mode='+m)}
function saveAP(){
var s=document.getElementById('ap_ssid').value.trim();
var p=document.getElementById('ap_pass').value.trim();
var ft=document.getElementById('ft');
if(s.length<1||s.length>32){ft.textContent='SSID must be 1-32 chars';return}
if(p.length>0&&p.length<8){ft.textContent='Password must be 8+ chars or empty';return}
ft.textContent='SAVING...';
fetch('/saveap?ssid='+encodeURIComponent(s)+'&pass='+encodeURIComponent(p)).then(function(r){
if(r.ok){ft.textContent='SAVED! REBOOTING...'}else{ft.textContent='ERROR'}
}).catch(function(){ft.textContent='ERROR'})}
function saveBZ(on){fetch('/buzzer?on='+(on?'1':'0'))}
</script></body></html>
)rawliteral";
#else
static const char SELECTOR_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no">
<title>OUI SPY</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
html{height:100%;height:-webkit-fill-available;overflow:hidden}
body{margin:0;height:100vh;height:-webkit-fill-available;font-family:monospace;background:#000;color:#0f0;display:flex;flex-direction:column;padding:4px;overflow:hidden}
.t{flex:1;display:flex;flex-direction:column;border:2px solid #0f0;padding:6px;overflow:hidden;min-height:0}
.h{text-align:center;padding-bottom:3px;margin-bottom:3px;border-bottom:1px solid #0f0;flex-shrink:0}
.ti{font-size:24px;font-weight:bold;letter-spacing:2px}
.s{font-size:8px;margin-top:1px;opacity:.7}
#x{flex:1;display:flex;flex-direction:column;min-height:0;overflow:hidden}
.m{flex:1;display:flex;flex-direction:column;min-height:0;overflow:hidden}
.i{flex:1;display:flex;flex-direction:column;justify-content:center;align-items:center;border:2px solid #0f0;border-bottom:0;cursor:pointer;background:#000;text-align:center;min-height:0;overflow:hidden}
.i:last-child{border-bottom:2px solid #0f0}
.i:active{background:#0f0;color:#000}
.n{font-size:18px;font-weight:bold;letter-spacing:1px}
.d{font-size:9px;opacity:.7;margin-top:1px}
.ap{display:flex;gap:3px;align-items:center;margin-top:4px;border-top:1px solid #0f0;padding-top:4px;flex-shrink:0}
.ap input{flex:1;padding:4px;background:#000;color:#0f0;border:1px solid #0f0;font-family:monospace;font-size:11px;min-width:0}
.ap input:focus{outline:none;border-color:#fff;color:#fff}
.ap .sb{padding:4px 7px;background:#0f0;color:#000;border:none;font-family:monospace;font-size:10px;font-weight:bold;cursor:pointer;white-space:nowrap}
.ap .sb:active{background:#fff}
.bz{display:flex;align-items:center;white-space:nowrap;cursor:pointer;font-size:9px;gap:2px;opacity:.7}
.bz:hover{opacity:1}
.bz input{margin:0;cursor:pointer}
.f{padding-top:2px;margin-top:3px;font-size:7px;text-align:center;opacity:.5;flex-shrink:0}
.boot{flex:1;display:flex;flex-direction:column;justify-content:center;align-items:center;text-align:center;padding:20px}
.bt{font-size:28px;font-weight:bold;margin-bottom:16px;letter-spacing:2px}
.bs{font-size:12px;line-height:1.5;margin-bottom:16px;opacity:.9;max-width:500px}
.br{font-size:13px}
@keyframes b{0%,50%{opacity:1}51%,100%{opacity:0}}
.blink{animation:b 1s infinite}
</style></head><body>
<div class="t">
<div class="h"><div class="ti">OUI SPY</div><div class="s">FIRMWARE SELECTOR</div></div>
<div id="x">
<div class="m">
<div class="i" onclick="go(1)"><div class="n">DETECTOR</div><div class="d">BLE Alert Tool for Specific Devices</div></div>
<div class="i" onclick="go(2)"><div class="n">FOXHUNTER</div><div class="d">RSSI Proximity Tracker</div></div>
<div class="i" onclick="go(4)"><div class="n">FLOCK-YOU</div><div class="d">BLE + WiFi Flock detect &bull; time-sliced AP</div></div>
<div class="i" onclick="go(5)"><div class="n">SKY SPY</div><div class="d">Drone Remote ID Monitor</div></div>
<div class="i" onclick="go(6)"><div class="n">MEGA MAID</div><div class="d">BLE + WiFi MAC/OUI collector &bull; time-sliced AP</div></div>
</div>
<div class="ap">
<input type="text" id="ap_ssid" placeholder="SSID" maxlength="32" value="%SSID%">
<input type="text" id="ap_pass" placeholder="PASSWORD" maxlength="63" value="%PASS%">
<button class="sb" onclick="saveAP()">SET</button>
<label class="bz"><input type="checkbox" id="bz" onchange="saveBZ(this.checked)" %BUZZER%>BZR</label>
</div>
<div class="f" id="ft">Hold BOOT 2s for menu &bull; MAC randomized</div>
</div>
<div id="y" class="boot" style="display:none">
<div class="bt" id="yt"></div>
<div class="bs" id="ys"></div>
<div class="br">REBOOTING<span class="blink">_</span></div>
</div>
</div>
<script>
var info={1:{t:'DETECTOR',s:'Scans for BLE devices and alerts when specific targets are detected. Configure OUI prefixes and MAC addresses to monitor.'},2:{t:'FOXHUNTER',s:'Track down a specific device using RSSI signal strength. Beeps get faster as you get closer to your target.'},4:{t:'FLOCK-YOU',s:'Detects Flock Safety cameras via BLE and WiFi promiscuous sniffing. Collect mode: 50/50 BLE/WiFi. Double-click KEY1 for dashboard mode with time-sliced AP (flockyou) and web export.'},5:{t:'SKY SPY',s:'Monitors for FAA Remote ID broadcasts from drones. Detects Open Drone ID signals over WiFi and BLE.'},6:{t:'MEGA MAID',s:'Collects all BLE and WiFi MAC addresses (no watchlist). Boots into dashboard mode (megamaid AP + web export). Double-click KEY1 to start collection mode (50/50 BLE/WiFi).'}};
function go(m){var d=info[m];document.getElementById('yt').textContent=d.t;document.getElementById('ys').textContent=d.s;document.getElementById('x').style.display='none';document.getElementById('y').style.display='flex';fetch('/select?mode='+m)}
function saveAP(){
var s=document.getElementById('ap_ssid').value.trim();
var p=document.getElementById('ap_pass').value.trim();
var ft=document.getElementById('ft');
if(s.length<1||s.length>32){ft.textContent='SSID must be 1-32 chars';return}
if(p.length>0&&p.length<8){ft.textContent='Password must be 8+ chars or empty';return}
ft.textContent='SAVING...';
fetch('/saveap?ssid='+encodeURIComponent(s)+'&pass='+encodeURIComponent(p)).then(function(r){
if(r.ok){ft.textContent='SAVED! REBOOTING...'}else{ft.textContent='ERROR'}
}).catch(function(){ft.textContent='ERROR'})}
function saveBZ(on){fetch('/buzzer?on='+(on?'1':'0'))}
</script></body></html>
)rawliteral";
#endif

// ============================================================================
// Boot Jingle for Selector - Zelda "Secret Discovered" Style
// ============================================================================
static void playNote(int freq, int duration) {
    boardLedcConfigure(freq);
    boardBuzzerAttach();
    boardLedcSetDuty(100);
    delay(duration);
    boardLedcSetDuty(0);
}

static void selectorBeep() {
    // "Secret discovered" ascending jingle
    playNote(784, 150);   // G5
    delay(20);
    playNote(988, 150);   // B5
    delay(20);
    playNote(1175, 150);  // D6
    delay(20);
    playNote(1568, 400);  // G6 (hold)
    delay(100);
    
    // LED flash sync
    boardLedInit();
    for (int i = 0; i < 3; i++) {
        boardLedOn();
        delay(50);
        boardLedOff();
        delay(50);
    }
}

// ============================================================================
// Menu button (KEY1 front) -> mode selector
// Hold KEY1 on the front face ~1.5s from any mode, or during boot, to open
// the web mode selector. KEY2 (side) is not used for menu navigation.
// Power/reset is a separate button on the side edge.
// ============================================================================
static bool checkBootButton() {
    if (!boardBootButtonPressed()) {
        Serial.println("[OUI-SPY] KEY1 (front) not pressed");
        return false;
    }
    
    Serial.println("[OUI-SPY] KEY1 (front) pressed - hold for mode selector...");
    boardLedOn();
    nessoUiSetStatus("Hold for menu...");
    Serial.flush();
    
    unsigned long start = millis();
    while (millis() - start < BOOT_HOLD_TIME) {
        if (!boardBootButtonPressed()) {
            Serial.println("[OUI-SPY] KEY1 released too early");
            boardLedOff();
            nessoUiSetStatus("");
            return false;
        }
        if ((millis() - start) % 300 < 50) {
            boardLedcConfigure(BUZZER_FREQ);
            boardBuzzerAttach();
            boardLedcSetDuty(80);
        } else {
            boardLedcSetDuty(0);
        }
        delay(10);
    }
    boardLedcSetDuty(0);
    boardLedOff();
    
    Serial.println("[OUI-SPY] *** KEY1 HELD *** -> FORCING SELECTOR");
    Serial.flush();
    
    requestSelectorOnReboot();
    return true;
}

// ============================================================================
// Selector Mode - AP + Web UI
// ============================================================================
static void startSelector() {
    // Load user-configured AP credentials and buzzer setting from NVS
    loadAPConfig();
    loadBuzzerConfig();
    
    Serial.println("\n========================================");
    Serial.println("  OUI SPY - Firmware Selector");
    Serial.printf("  Connect to WiFi: %s\n", apSSID.c_str());
    Serial.printf("  Password: %s\n", apPassword.c_str());
    Serial.println("  Open: http://192.168.4.1");
    Serial.println("========================================\n");
    Serial.flush();
    
    // Clean WiFi init from OFF state (setup() already nuked everything)
    Serial.println("[SELECTOR] Initializing WiFi AP...");
    Serial.flush();
    WiFi.persistent(false);       // Don't save this config back to NVS
    WiFi.mode(WIFI_AP);
    delay(200);
    
    // Randomize MAC address every boot for privacy
    randomizeMAC();
    
    Serial.printf("[SELECTOR] Starting AP: %s...\n", apSSID.c_str());
    Serial.flush();
    bool apStarted;
    if (apPassword.length() >= 8) {
        apStarted = WiFi.softAP(apSSID.c_str(), apPassword.c_str());
    } else {
        // Open network (no password or too short for WPA2)
        apStarted = WiFi.softAP(apSSID.c_str());
    }
    Serial.printf("[SELECTOR] AP started: %s\n", apStarted ? "SUCCESS" : "FAILED");
    Serial.print("[SELECTOR] AP IP: ");
    Serial.println(WiFi.softAPIP());
    Serial.flush();

    // Captive portal DNS - redirect all DNS queries to our AP IP
    selectorDNS.start(53, "*", WiFi.softAPIP());
    Serial.println("[SELECTOR] Captive portal DNS started");

    // Selector page - inject current AP config into template
    selectorServer.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        // Build HTML with current AP values injected
        String html = FPSTR(SELECTOR_HTML);
        html.replace("%SSID%", apSSID);
        html.replace("%PASS%", apPassword);
        html.replace("%BUZZER%", buzzerEnabled ? "checked" : "");
        request->send(200, "text/html", html);
    });
    
    // Mode selection endpoint - ONLY place that should trigger reboot
    selectorServer.on("/select", HTTP_GET, [](AsyncWebServerRequest *request) {
        if (request->hasParam("mode")) {
            int mode = request->getParam("mode")->value().toInt();
#ifdef NESSO_NO_FLOCKYOU
            if (mode == 1 || mode == 2 || mode == 5 || mode == 6) {
#else
            if (mode == 1 || mode == 2 || mode == 4 || mode == 5 || mode == 6) {
#endif
                request->send(200, "text/plain", "OK");
                delay(1500);
                selectorApplyMode(mode);
                return;
            }
        }
        Serial.println("[OUI-SPY] Invalid mode selection rejected");
#ifdef NESSO_NO_FLOCKYOU
        request->send(400, "text/plain", "Invalid mode (1, 2, 5, 6)");
#else
        request->send(400, "text/plain", "Invalid mode (1, 2, 4, 5, 6)");
#endif
    });
    
    // Save AP settings endpoint
    selectorServer.on("/saveap", HTTP_GET, [](AsyncWebServerRequest *request) {
        if (request->hasParam("ssid")) {
            String newSSID = request->getParam("ssid")->value();
            String newPass = request->hasParam("pass") ? request->getParam("pass")->value() : "";
            
            // Validate
            if (newSSID.length() < 1 || newSSID.length() > 32) {
                request->send(400, "text/plain", "SSID must be 1-32 chars");
                return;
            }
            if (newPass.length() > 0 && newPass.length() < 8) {
                request->send(400, "text/plain", "Password must be 8+ chars or empty");
                return;
            }
            
            Serial.printf("[OUI-SPY] Saving new AP config: SSID='%s'\n", newSSID.c_str());
            saveAPConfig(newSSID, newPass);
            
            request->send(200, "text/plain", "OK");
            delay(1000);
            ESP.restart();
            return;
        }
        request->send(400, "text/plain", "Missing SSID parameter");
    });
    
    // Buzzer toggle endpoint
    selectorServer.on("/buzzer", HTTP_GET, [](AsyncWebServerRequest *request) {
        if (request->hasParam("on")) {
            bool enabled = request->getParam("on")->value() == "1";
            saveBuzzerConfig(enabled);
            request->send(200, "text/plain", "OK");
        } else {
            request->send(400, "text/plain", "Missing 'on' parameter");
        }
    });
    
    // Reset to selector (callable from any mode's web interface)
    selectorServer.on("/menu", HTTP_GET, [](AsyncWebServerRequest *request) {
        requestSelectorOnReboot();
        request->send(200, "text/plain", "Returning to menu...");
        delay(500);
        ESP.restart();
    });
    
    // Captive portal catch-all: redirect any unknown URL to root
    selectorServer.onNotFound([](AsyncWebServerRequest *request) {
        request->redirect("http://192.168.4.1/");
    });

    Serial.println("[SELECTOR] Starting web server...");
    Serial.flush();
    selectorServer.begin();
    Serial.println("[SELECTOR] Web server started!");
    Serial.flush();
    
    // Visual indicator - breathe LED
    Serial.println("[SELECTOR] Setting up LED...");
    Serial.flush();
    boardLedInit();
    nessoUiSetMode("Selector");
    nessoUiSetGpsIndicator(NESSO_GPS_HIDDEN);
    
    Serial.println("[SELECTOR] Playing startup jingle...");
    Serial.flush();
    selectorBeep();
    
    Serial.println("[SELECTOR] *** SELECTOR FULLY INITIALIZED ***");
    Serial.printf("[SELECTOR] WiFi AP: '%s'\n", apSSID.c_str());
    Serial.flush();
}

// ============================================================================
// Arduino Entry Points
// ============================================================================
static unsigned long bootTime = 0;

void setup() {
    Serial.begin(115200);
    delay(200);  // Give serial time to initialize
    
    Serial.println("\n\n========================================");
    Serial.println("OUI SPY UNIFIED FIRMWARE v2.0");
    Serial.println("========================================");
    Serial.flush();
    
    // Initialize expander/display before reading KEY1
    boardHwInit();

    // Hold KEY1 (front) during startup to force selector menu.
    bool forceSelector = checkBootButton();
    
    // CRITICAL: Nuke ALL stored WiFi config from NVS.
    // The ESP32 persists AP SSID/password in flash and auto-restores it,
    // causing stale APs from previous firmware to appear on every boot.
    // We must: init WiFi -> restore factory defaults -> shut it down.
    WiFi.mode(WIFI_AP_STA);       // Init the WiFi stack so IDF calls work
    delay(100);
    esp_wifi_restore();           // Erase ALL stored WiFi config from NVS
    WiFi.softAPdisconnect(true);  // Kill any auto-restored AP
    WiFi.disconnect(true, true);  // Kill STA + erase stored creds
    WiFi.mode(WIFI_OFF);          // Shut it all down
    delay(100);
    Serial.println("[OUI-SPY] WiFi factory-reset complete - all stale config erased");
    Serial.flush();
    
    bootTime = millis();
    
    if (forceSelector) {
        Serial.println("[OUI-SPY] KEY1 override -> SELECTOR MODE");
        Serial.flush();
    }

    currentMode = resolveBootMode(forceSelector);
    Serial.printf("[OUI-SPY] Resolved boot mode: %d\n", currentMode);
    Serial.flush();

    if (currentMode != 0) {
        Serial.println("========================================");
        Serial.printf("[OUI-SPY] *** BOOTING INTO FIRMWARE MODE %d ***\n", currentMode);
        Serial.println("========================================");
        Serial.flush();
    }
    
    Serial.printf("[OUI-SPY] FINAL BOOT MODE: %d\n", currentMode);
    Serial.println("========================================");
    Serial.flush();
    
    // Route to selected mode
    Serial.println("\n[OUI-SPY] ========== ROUTING TO MODE ==========");
    Serial.printf("[OUI-SPY] About to switch on currentMode = %d\n", currentMode);
    Serial.flush();
    delay(100);
    
    if (currentMode == 0) {
        Serial.println("[OUI-SPY] >>> STARTING SELECTOR (mode 0) <<<");
        Serial.println("[OUI-SPY] AP will be configured from NVS");
        Serial.println("[OUI-SPY] Calling startSelector()...");
        Serial.flush();
        delay(100);
        nessoUiSetMode("Selector");
        nessoUiSetGpsIndicator(NESSO_GPS_HIDDEN);
        nessoUiSetSelectorActive(true);
        selectorHighlight = 0;
        selectorUpdateDisplay();
        startSelector();
        Serial.println("[OUI-SPY] startSelector() returned");
        Serial.flush();
    } else if (currentMode == 1) {
        Serial.println("[OUI-SPY] >>> STARTING DETECTOR (mode 1) <<<");
        Serial.println("[OUI-SPY] AP will be: snoopuntothem");
        Serial.flush();
        nessoUiSetMode("Detector");
        nessoUiSetGpsIndicator(NESSO_GPS_SEARCHING);
        boardGpsClockBegin();
        detector_setup();
    } else if (currentMode == 2) {
        Serial.println("[OUI-SPY] >>> STARTING FOXHUNTER (mode 2) <<<");
        Serial.println("[OUI-SPY] AP will be: foxhunter");
        Serial.flush();
        nessoUiSetMode("Foxhunter");
        nessoUiSetGpsIndicator(NESSO_GPS_SEARCHING);
        boardGpsClockBegin();
        foxhunter_setup();
#ifndef NESSO_NO_FLOCKYOU
    } else if (currentMode == 4) {
        Serial.println("[OUI-SPY] >>> STARTING FLOCK-YOU (mode 4) <<<");
        Serial.println("[OUI-SPY] Collect: 50/50 BLE+WiFi | Dashboard: time-sliced AP flockyou");
        Serial.flush();
        nessoUiSetMode("Flock-You");
        flockyou_setup();
#endif
    } else if (currentMode == 5) {
        Serial.println("[OUI-SPY] >>> STARTING SKY SPY (mode 5) <<<");
        Serial.println("[OUI-SPY] No WiFi AP (BLE only)");
        Serial.flush();
        nessoUiSetMode("Sky Spy");
        nessoUiSetGpsIndicator(NESSO_GPS_SEARCHING);
        boardGpsClockBegin();
        skyspy_setup();
    } else if (currentMode == 6) {
        Serial.println("[OUI-SPY] >>> STARTING MEGA MAID (mode 6) <<<");
        Serial.println("[OUI-SPY] Boot: dashboard AP megamaid — triple-click KEY1 for collection");
        Serial.flush();
        nessoUiSetMode("Mega Maid");
        megamaid_setup();
    } else {
#ifdef NESSO_NO_FLOCKYOU
        Serial.printf("[OUI-SPY] ERROR: Unknown mode %d, defaulting to Mega_Maid\n", currentMode);
        Serial.flush();
        nessoUiSetMode("Mega Maid");
        megamaid_setup();
#else
        Serial.printf("[OUI-SPY] ERROR: Unknown mode %d, defaulting to Flock-You\n", currentMode);
        Serial.flush();
        nessoUiSetMode("Flock-You");
        flockyou_setup();
#endif
    }
    
    Serial.println("[OUI-SPY] ========== MODE STARTED ==========\n");
    Serial.flush();
}

// ============================================================================
// KEY1 (front) hold -> mode selector (runs every loop, works from ANY mode)
// ============================================================================
static unsigned long bootBtnStart = 0;
static bool bootBtnActive = false;
static bool bootBtnMidBeep = false;

static void checkBootButtonLoop() {
    // Mega_Maid uses KEY1 for export gestures (double/triple-click). Never treat hold as
    // "return to selector" — that looked like an unexpected reboot at cap-full export.
    if (currentMode == 6) {
        nessoResetBootButtonHold();
        return;
    }
    if (boardBootButtonPressed()) {
        if (!bootBtnActive) {
            bootBtnActive = true;
            bootBtnMidBeep = false;
            bootBtnStart = millis();
            boardLedOn();
            nessoUiSetStatus("Hold for menu...");
            Serial.println("[OUI-SPY] KEY1 (front) down");
        } else {
            unsigned long held = millis() - bootBtnStart;
            if (!bootBtnMidBeep && held >= 500) {
                bootBtnMidBeep = true;
                boardLedcConfigure(BUZZER_FREQ);
                boardBuzzerAttach();
                boardLedcSetDuty(60);
                delay(40);
                boardLedcSetDuty(0);
            }
            if (held >= BOOT_HOLD_TIME) {
            Serial.println("\n[OUI-SPY] *** KEY1 HELD -> RETURNING TO SELECTOR ***");
            Serial.flush();
            nessoUiSetStatus("Restarting...");
            for (int i = 0; i < 3; i++) {
                boardLedcConfigure(BUZZER_FREQ);
                boardBuzzerAttach();
                boardLedcSetDuty(100);
                delay(80);
                boardLedcSetDuty(0);
                delay(60);
            }
            requestSelectorOnReboot();
            delay(200);
            ESP.restart();
            }
        }
    } else if (bootBtnActive) {
        bootBtnActive = false;
        bootBtnMidBeep = false;
        boardLedOff();
        nessoUiClearHoldPrompt();
    }
}

void nessoResetBootButtonHold() {
    bootBtnActive = false;
    bootBtnMidBeep = false;
    bootBtnStart = 0;
    boardLedOff();
}

void nessoPollInput() {
    if (boardBootButtonPressed() || boardSecondaryButtonPressed()) {
        nessoUiNoteActivity();
    }
    checkBootButtonLoop();
    nessoUiTick();
}

void loop() {
    // Mega_Maid: run mode loop before KEY1 hold-for-menu so triple-click export is not
    // mistaken for a 1.5s hold reboot (especially at cap-full when clicks are slower).
    if (currentMode == 6) {
        megamaid_loop();
    }

    nessoPollInput();

    if (currentMode == 1 || currentMode == 2 || currentMode == 5) {
        nessoUiSetGpsIndicator(boardGpsTimeValid() ? NESSO_GPS_FIX : NESSO_GPS_SEARCHING);
    }
    
    // Route to active mode's loop
    switch (currentMode) {
        case 1: detector_loop(); break;
        case 2: foxhunter_loop(); break;

#ifndef NESSO_NO_FLOCKYOU
        case 4: flockyou_loop(); break;
#endif
        case 5: skyspy_loop(); break;
        case 6: break;
        default:
            // Selector mode - web UI + KEY2 cycle / KEY1 confirm
            checkSelectorButtons();
            selectorDNS.processNextRequest();
            // LED breathing animation
            {
                static unsigned long lastLed = 0;
                static bool ledState = false;
                if (millis() - lastLed > 1000) {
                    ledState = !ledState;
                    boardLedWrite(ledState);
                    lastLed = millis();
                }
            }
            delay(10);
            break;
    }
}
