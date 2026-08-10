/*
 * OUI SPY - Unified Firmware Boot Selector
 * colonelpanichacks
 *
 * On boot: creates AP "ouispy" with web UI to select firmware mode 1-5.
 * After selection, stores mode in NVS and reboots into that firmware.
 * To return to selector: double-tap the reset button quickly.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <nvs_flash.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include "modes.h"
#include "dashboard.h"

// Hardware pins (shared across all modes)
// Buzzer is routed to the expansion board's buzzer (GPIO4) when the chassis is
// attached, otherwise the external oui-spy piezo (GPIO3). Set in setup() after
// dashboard_init().
static int buzzerPin = BUZZER_PIN_EXTERNAL;
#define BUZZER_PIN buzzerPin
#define LED_PIN 21

// Boot button (GPIO0) - held during boot to return to selector menu
#define BOOT_BUTTON_PIN 0
#define BOOT_HOLD_TIME 1500  // ms - hold boot button this long to force selector

static Preferences prefs;
static AsyncWebServer selectorServer(80);
static int currentMode = 0;

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
<div class="i" onclick="go(4)"><div class="n">FLOCK-YOU</div><div class="d">Surveillance Detector &bull; AP: flockyou</div></div>
<div class="i" onclick="go(5)"><div class="n">SKY SPY</div><div class="d">Drone Remote ID Monitor</div></div>
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
var info={1:{t:'DETECTOR',s:'Scans for BLE devices and alerts when specific targets are detected. Configure OUI prefixes and MAC addresses to monitor.'},2:{t:'FOXHUNTER',s:'Track down a specific device using RSSI signal strength. Beeps get faster as you get closer to your target.'},4:{t:'FLOCK-YOU',s:'Detects Flock Safety surveillance cameras via BLE. Serves web dashboard on AP flockyou with live detections, pattern DB, and JSON/CSV export.'},5:{t:'SKY SPY',s:'Monitors for FAA Remote ID broadcasts from drones. Detects Open Drone ID signals over WiFi and BLE.'}};
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

// ============================================================================
// Boot Jingle for Selector - Zelda "Secret Discovered" Style
// ============================================================================
static void playNote(int freq, int duration) {
    ledcSetup(0, freq, 8);
    ledcAttachPin(BUZZER_PIN, 0);
    ledcWrite(0, 100);
    delay(duration);
    ledcWrite(0, 0);
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
    pinMode(LED_PIN, OUTPUT);
    for (int i = 0; i < 3; i++) {
        digitalWrite(LED_PIN, LOW);   // On
        delay(50);
        digitalWrite(LED_PIN, HIGH);  // Off
        delay(50);
    }
}

// ============================================================================
// OLED Mode Selector Menu (expansion board display)
// ============================================================================
// When the expansion board OLED is attached, the selector offers an on-screen
// menu as an alternative to the web UI:
//   - TAP the USER button  -> advance the highlighted mode
//   - HOLD the USER button -> boot the highlighted mode
// The WiFi AP / web UI keeps working alongside it. If the user does not
// interact for MENU_TIMEOUT_MS, the menu resumes the last active mode.
#define MENU_MODES_COUNT 4
#define MENU_HOLD_BOOT_MS 1500
#define MENU_TIMEOUT_MS 30000

static const uint8_t MENU_MODES[MENU_MODES_COUNT] = { 1, 2, 4, 5 };
static const char* MENU_LABELS[MENU_MODES_COUNT] = {
    "DETECTOR", "FOXHUNTER", "FLOCK-YOU", "SKY SPY"
};

#define MENU_BASELINE 6
#define MENU_ROW_STEP 8

static uint8_t menuIndex = 0;
static unsigned long lastMenuRender = 0;
static bool menuDirty = true;
static unsigned long menuBtnDown = 0;
static bool menuBtnWasDown = false;
static bool menuBtnLongDone = false;
static unsigned long lastMenuActivity = 0;

static void renderSelectorMenu(unsigned long now) {
    if (!dashboard_present()) return;
    if (!menuDirty && now - lastMenuRender < 1000) return;
    menuDirty = false;
    lastMenuRender = now;

    dashboard_clear();
    dashboard_set_cursor(0, MENU_BASELINE + 0 * MENU_ROW_STEP);
    dashboard_printf("OUI-SPY");
    dashboard_set_cursor(0, MENU_BASELINE + 1 * MENU_ROW_STEP);
    dashboard_printf("FIRMWARE SELECTOR");
    dashboard_draw_hline(0, 15, 128);

    for (uint8_t i = 0; i < MENU_MODES_COUNT; i++) {
        int yBase = MENU_BASELINE + (2 + i) * MENU_ROW_STEP;
        if (i == menuIndex) {
            dashboard_draw_box(0, yBase - 6, 128, 7);
            dashboard_set_draw_color(0);
            dashboard_set_cursor(0, yBase);
            dashboard_printf("%s", MENU_LABELS[i]);
            dashboard_set_draw_color(1);
        } else {
            dashboard_set_cursor(0, yBase);
            dashboard_printf("  %s", MENU_LABELS[i]);
        }
    }

    dashboard_set_cursor(0, MENU_BASELINE + 6 * MENU_ROW_STEP);
    dashboard_printf("TAP NEXT  HOLD BOOT");
    dashboard_set_cursor(0, MENU_BASELINE + 7 * MENU_ROW_STEP);
    dashboard_printf("WEB 192.168.4.1");
    dashboard_flush();
}

static void bootSelectedMode() {
    int mode = MENU_MODES[menuIndex];
    Serial.printf("[SELECTOR] OLED menu: booting mode %d (%s)\n",
                  mode, MENU_LABELS[menuIndex]);
    Serial.flush();

    // Write mode to NVS (same path as the web UI /select handler)
    prefs.begin("unified-mode", false);
    prefs.putInt("mode", mode);
    prefs.end();

    // One-shot flag: the next boot goes straight into this mode, not the menu.
    Preferences menuPrefs;
    menuPrefs.begin("ouispy-menu", false);
    menuPrefs.putBool("boot", true);
    menuPrefs.end();

    // Confirmation feedback
    for (int i = 0; i < 2; i++) {
        playNote(1000 + i * 200, 120);
        delay(30);
    }

    if (dashboard_present()) {
        dashboard_clear();
        dashboard_set_cursor(0, MENU_BASELINE + 0 * MENU_ROW_STEP);
        dashboard_printf("BOOTING MODE %d", mode);
        dashboard_set_cursor(0, MENU_BASELINE + 2 * MENU_ROW_STEP);
        dashboard_printf("%s", MENU_LABELS[menuIndex]);
        dashboard_flush();
        delay(1200);
    }

    ESP.restart();
}

// Boots the last active mode (stored in NVS) when the OLED menu times out.
// Returns true when it actually reboots, false if there is no last mode to
// resume (in which case the menu keeps waiting).
static bool resumeLastMode() {
    prefs.begin("unified-mode", true);
    int mode = prefs.getInt("mode", 0);
    prefs.end();

    // Valid modes are 1, 2, 4, 5 (mode 3 is intentionally skipped).
    if (mode != 1 && mode != 2 && mode != 4 && mode != 5) {
        Serial.println("[SELECTOR] Menu timeout - no last mode to resume, staying in menu");
        Serial.flush();
        return false;
    }

    // One-shot flag: the next boot goes straight into this mode, not the menu.
    Preferences menuPrefs;
    menuPrefs.begin("ouispy-menu", false);
    menuPrefs.putBool("boot", true);
    menuPrefs.end();

    Serial.printf("[SELECTOR] Menu timeout - resuming last mode %d\n", mode);
    Serial.flush();

    // Confirmation feedback
    for (int i = 0; i < 2; i++) {
        playNote(800 + i * 150, 100);
        delay(30);
    }

    if (dashboard_present()) {
        dashboard_clear();
        dashboard_set_cursor(0, MENU_BASELINE + 0 * MENU_ROW_STEP);
        dashboard_printf("RESUMING MODE %d", mode);
        dashboard_set_cursor(0, MENU_BASELINE + 2 * MENU_ROW_STEP);
        dashboard_printf("LAST ACTIVE MODE");
        dashboard_flush();
        delay(1000);
    }

    ESP.restart();
    return true;  // never reached
}

// Handles the USER button: tap advances, hold boots. Call every loop tick.
static void handleSelectorMenu(unsigned long now) {
    if (!dashboard_present()) return;

    bool down = digitalRead(DISPLAY_BUTTON_PIN) == LOW;

    if (down && !menuBtnWasDown) {
        menuBtnWasDown = true;
        menuBtnDown = now;
        menuBtnLongDone = false;
    }

    if (down && menuBtnWasDown && !menuBtnLongDone &&
        now - menuBtnDown >= MENU_HOLD_BOOT_MS) {
        menuBtnLongDone = true;
        bootSelectedMode();
        return;
    }

    if (!down && menuBtnWasDown) {
        menuBtnWasDown = false;
        // Debounce release, then treat as a tap
        if (!menuBtnLongDone && now - menuBtnDown >= 30) {
            menuIndex = (menuIndex + 1) % MENU_MODES_COUNT;
            menuDirty = true;
            lastMenuActivity = now;
            Serial.printf("[SELECTOR] OLED menu highlight: %s (mode %d)\n",
                          MENU_LABELS[menuIndex], MENU_MODES[menuIndex]);
            Serial.flush();
        }
    }
}

// ============================================================================
// Boot Button Detection (GPIO0)
// ============================================================================
// Hold the BOOT button during startup to return to selector menu.
// Beeps while waiting so you know it's detecting the hold.
static bool checkBootButton() {
    pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
    
    // Quick check - is button even pressed?
    if (digitalRead(BOOT_BUTTON_PIN) == HIGH) {
        Serial.println("[OUI-SPY] Boot button not pressed");
        return false;  // Not pressed, skip
    }
    
    Serial.println("[OUI-SPY] Boot button PRESSED - hold to return to menu...");
    Serial.flush();
    
    // Button is pressed - wait for hold duration with beep feedback
    unsigned long start = millis();
    while (millis() - start < BOOT_HOLD_TIME) {
        if (digitalRead(BOOT_BUTTON_PIN) == HIGH) {
            Serial.println("[OUI-SPY] Boot button released too early");
            return false;  // Released before threshold
        }
        // Quick beep feedback every 300ms so user knows it's working
        if ((millis() - start) % 300 < 50) {
            ledcSetup(0, 2000, 8);
            ledcAttachPin(BUZZER_PIN, 0);
            ledcWrite(0, 80);
        } else {
            ledcWrite(0, 0);
        }
        delay(10);
    }
    ledcWrite(0, 0);  // Stop beeping
    
    Serial.println("[OUI-SPY] *** BOOT BUTTON HELD *** -> FORCING SELECTOR");
    Serial.flush();
    
    // Clear the stored mode
    prefs.begin("unified-mode", false);
    prefs.putInt("mode", 0);
    prefs.end();
    return true;
}

// ============================================================================
// Selector Mode - AP + Web UI
// ============================================================================
static void startSelector() {
    // Load user-configured AP credentials and buzzer setting from NVS
    loadAPConfig();
    loadBuzzerConfig();
    
    // Probe for the expansion board OLED. Pins 5/6 are free in this mode, so
    // this is safe and a no-op when no display is attached. When present, an
    // on-screen mode menu is offered alongside the web UI.
    dashboard_init();
    if (dashboard_present()) {
        Serial.println("[SELECTOR] OLED detected - on-screen mode menu enabled");
        menuDirty = true;
        // Position the highlight on the last active mode so the user can see
        // what the menu will auto-boot into on timeout.
        prefs.begin("unified-mode", true);
        int lastMode = prefs.getInt("mode", 0);
        prefs.end();
        for (uint8_t i = 0; i < MENU_MODES_COUNT; i++) {
            if (MENU_MODES[i] == lastMode) {
                menuIndex = i;
                break;
            }
        }
        lastMenuActivity = millis();
        renderSelectorMenu(millis());
    }
    
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
    
    // Selector page - inject current AP config into template
    selectorServer.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        prefs.begin("unified-mode", false);
        prefs.putInt("mode", 0);
        prefs.end();
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
            if (mode >= 1 && mode <= 5) {
                Serial.printf("[OUI-SPY] USER SELECTED MODE %d - Storing and rebooting\n", mode);
                
                // Clear reset flag so double-reset detection doesn't override on next boot
                Preferences resetPrefs;
                resetPrefs.begin("ouispy-rst", false);
                resetPrefs.putBool("flag", false);
                resetPrefs.end();
                
                // Write mode to NVS
                prefs.begin("unified-mode", false);
                prefs.putInt("mode", mode);
                prefs.end();

                // One-shot flag: next boot lands in this mode, not the menu.
                Preferences menuPrefs;
                menuPrefs.begin("ouispy-menu", false);
                menuPrefs.putBool("boot", true);
                menuPrefs.end();
                
                // Verify the write by reading it back
                prefs.begin("unified-mode", true);
                int verify = prefs.getInt("mode", -1);
                prefs.end();
                Serial.printf("[OUI-SPY] NVS VERIFY: wrote %d, read back %d - %s\n", 
                    mode, verify, (verify == mode) ? "OK" : "MISMATCH!");
                Serial.flush();
                
                request->send(200, "text/plain", "OK");
                delay(1500);  // Extra time for NVS to settle
                Serial.printf("[OUI-SPY] REBOOTING INTO MODE %d NOW\n", mode);
                Serial.flush();
                ESP.restart();
                return;
            }
        }
        Serial.println("[OUI-SPY] Invalid mode selection rejected");
        request->send(400, "text/plain", "Invalid mode (1-5)");
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
        prefs.begin("unified-mode", false);
        prefs.putInt("mode", 0);
        prefs.end();
        request->send(200, "text/plain", "Returning to menu...");
        delay(500);
        ESP.restart();
    });
    
    Serial.println("[SELECTOR] Starting web server...");
    Serial.flush();
    selectorServer.begin();
    Serial.println("[SELECTOR] Web server started!");
    Serial.flush();
    
    // Visual indicator - breathe LED
    Serial.println("[SELECTOR] Setting up LED...");
    Serial.flush();
    pinMode(LED_PIN, OUTPUT);
    
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
    
    // Detect the expansion board early so the buzzer is routed to the chassis
    // buzzer (GPIO4) instead of the external piezo (GPIO3) from the first beep.
    // This also decides whether the selector boots into its OLED mode menu.
    dashboard_init();
    buzzerPin = dashboard_buzzer_pin();
    
    // FIRST THING: Check if BOOT button (GPIO0) is being held
    // Hold BOOT for 1.5 seconds during startup to force selector menu.
    bool forceSelector = checkBootButton();
    
    // Initialize shared hardware
    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW);
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, HIGH);  // LED off (inverted logic on XIAO)
    
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
        Serial.println("[OUI-SPY] Boot button override -> SELECTOR MODE");
        Serial.flush();
        currentMode = 0;
    } else {
        // Step 2: Read stored mode from NVS
        prefs.begin("unified-mode", true);  // read-only
        currentMode = prefs.getInt("mode", 0);
        prefs.end();
        Serial.printf("[OUI-SPY] Stored mode from NVS: %d\n", currentMode);
        Serial.flush();
        
        // Validate mode range
        if (currentMode < 0 || currentMode > 5) {
            Serial.printf("[OUI-SPY] Invalid stored mode %d, defaulting to selector\n", currentMode);
            currentMode = 0;
        }
        
        // If we're booting into a firmware mode (not selector), log it clearly
        if (currentMode != 0) {
            Serial.println("========================================");
            Serial.printf("[OUI-SPY] *** BOOTING INTO FIRMWARE MODE %d ***\n", currentMode);
            Serial.println("========================================");
            Serial.flush();
        }
    }
    
    Serial.printf("[OUI-SPY] FINAL BOOT MODE: %d\n", currentMode);
    Serial.println("========================================");
    Serial.flush();
    
    // One-shot flag set by the OLED menu (or web UI) when the user picks a mode:
    // the next boot must go straight into that mode instead of the selector.
    bool launchFromMenu = false;
    {
        Preferences menuPrefs;
        menuPrefs.begin("ouispy-menu", true);
        launchFromMenu = menuPrefs.getBool("boot", false);
        menuPrefs.end();
        if (launchFromMenu) {
            menuPrefs.begin("ouispy-menu", false);
            menuPrefs.putBool("boot", false);
            menuPrefs.end();
        }
    }

    // If the expansion board OLED is present, the on-screen menu is the default
    // boot interface: always boot into the selector so the user can pick a mode
    // with the USER button (unless this boot is the result of a menu selection,
    // which must land in the chosen mode). This is also the reliable way to reach
    // the selector, because holding the BOOT button across a reset makes the ROM
    // enter download mode instead of running the firmware.
    if (dashboard_init() && currentMode != 0 && !launchFromMenu) {
        Serial.println("[OUI-SPY] OLED present - booting into selector menu");
        Serial.flush();
        currentMode = 0;
    }
    
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
        startSelector();
        Serial.println("[OUI-SPY] startSelector() returned");
        Serial.flush();
    } else if (currentMode == 1) {
        Serial.println("[OUI-SPY] >>> STARTING DETECTOR (mode 1) <<<");
        Serial.println("[OUI-SPY] AP will be: snoopuntothem");
        Serial.flush();
        detector_setup();
    } else if (currentMode == 2) {
        Serial.println("[OUI-SPY] >>> STARTING FOXHUNTER (mode 2) <<<");
        Serial.println("[OUI-SPY] AP will be: foxhunter");
        Serial.flush();
        foxhunter_setup();
    } else if (currentMode == 4) {
        Serial.println("[OUI-SPY] >>> STARTING FLOCK-YOU (mode 4) <<<");
        Serial.println("[OUI-SPY] No WiFi AP (BLE only)");
        Serial.flush();
        flockyou_setup();
    } else if (currentMode == 5) {
        Serial.println("[OUI-SPY] >>> STARTING SKY SPY (mode 5) <<<");
        Serial.println("[OUI-SPY] No WiFi AP (BLE only)");
        Serial.flush();
        skyspy_setup();
    } else {
        Serial.printf("[OUI-SPY] ERROR: Unknown mode %d, defaulting to selector\n", currentMode);
        Serial.flush();
        startSelector();
    }
    
    Serial.println("[OUI-SPY] ========== MODE STARTED ==========\n");
    Serial.flush();
}

// ============================================================================
// Boot Button -> Menu (runs every loop, works from ANY mode)
// ============================================================================
static unsigned long bootBtnStart = 0;
static bool bootBtnActive = false;

static void checkBootButtonLoop() {
    if (digitalRead(BOOT_BUTTON_PIN) == LOW) {
        if (!bootBtnActive) {
            // Button just pressed - start timing
            bootBtnActive = true;
            bootBtnStart = millis();
        } else if (millis() - bootBtnStart >= BOOT_HOLD_TIME) {
            // Held long enough - triple beep confirmation then reboot to menu
            Serial.println("\n[OUI-SPY] *** BOOT BUTTON HELD -> RETURNING TO MENU ***");
            Serial.flush();
            for (int i = 0; i < 3; i++) {
                ledcSetup(0, 3000, 8);
                ledcAttachPin(BUZZER_PIN, 0);
                ledcWrite(0, 100);
                delay(80);
                ledcWrite(0, 0);
                delay(60);
            }
            // Clear mode and reboot
            prefs.begin("unified-mode", false);
            prefs.putInt("mode", 0);
            prefs.end();
            delay(200);
            ESP.restart();
        }
    } else {
        bootBtnActive = false;
    }
}

void loop() {
    // ALWAYS check boot button - hold 2s from ANY mode to return to menu
    checkBootButtonLoop();
    
    // Route to active mode's loop
    switch (currentMode) {
        case 1: detector_loop(); break;
        case 2: foxhunter_loop(); break;

        case 4: flockyou_loop(); break;
        case 5: skyspy_loop(); break;
        default:
            // Selector mode - web server handles everything
            // OLED mode menu (when expansion board display present)
            handleSelectorMenu(millis());
            renderSelectorMenu(millis());
            // Menu timeout: after 30s without interaction, resume the last mode
            {
                unsigned long now = millis();
                if (dashboard_present() && now - lastMenuActivity >= MENU_TIMEOUT_MS) {
                    if (!resumeLastMode()) {
                        lastMenuActivity = now;  // no last mode - keep waiting
                    }
                }
            }
            // LED breathing animation
            {
                static unsigned long lastLed = 0;
                static bool ledState = false;
                if (millis() - lastLed > 1000) {
                    ledState = !ledState;
                    digitalWrite(LED_PIN, ledState ? LOW : HIGH);
                    lastLed = millis();
                }
            }
            delay(10);
            break;
    }
}
