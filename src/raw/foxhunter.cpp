#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
// NimBLE included by wrapper
#include <esp_wifi.h>

// Board-specific pin configuration
#if defined(ARDUINO_XIAO_ESP32C5)
  #define BUZZER_PIN 25         // D2 = GPIO25 on XIAO ESP32-C5
  #define LED_PIN 27            // LED_BUILTIN = GPIO27 (active HIGH)
  #define LED_INVERTED false
#else
  #define BUZZER_PIN 3          // D2 = GPIO3 on XIAO ESP32-S3
  #define LED_PIN 21            // Built-in LED (active LOW / inverted)
  #define LED_INVERTED true
#endif
#define BUZZER_FREQ 2000
#define BUZZER_DUTY 127

// Network configuration
const char* AP_SSID = "foxhunter";
const char* AP_PASSWORD = "foxhunter";
const unsigned long CONFIG_TIMEOUT = 20000; // 20 seconds

// Operating modes
enum OperatingMode {
    CONFIG_MODE,
    TRACKING_MODE
};

// Global variables
OperatingMode currentMode = CONFIG_MODE;
AsyncWebServer server(80);
DNSServer foxhunterDNS;
Preferences preferences;
NimBLEScan* pBLEScan;

String targetMAC = "";
unsigned long configStartTime = 0;
unsigned long lastConfigActivity = 0;
unsigned long modeSwitchScheduled = 0;
unsigned long deviceResetScheduled = 0;
unsigned long lastBeepTime = 0;
bool targetDetected = false;
int currentRSSI = -100;
unsigned long lastTargetSeen = 0;
bool firstDetection = true;
bool sessionFirstDetection = true; // Only beep once per hunting session

// Persistent settings
bool buzzerEnabled = true;
bool ledEnabled = true;

// Simple beep state
bool isBeeping = false;
unsigned long lastBeepStart = 0;
unsigned long beepDuration = 50;  // 50ms beep duration for fast response

// Serial output synchronization - avoid concurrent writes
volatile bool newTargetDetected = false;


int calculateBeepInterval(int rssi) {
    // REAL-TIME foxhunting intervals
    // RSSI ranges: -95 (very weak) to -30 (very strong)
    if (rssi >= -35) {
        return map(rssi, -35, -25, 25, 10); // 25ms to 10ms - INSANE SPEED
    } else if (rssi >= -45) {
        return map(rssi, -45, -35, 50, 25); // 50ms to 25ms - VERY FAST
    } else if (rssi >= -55) {
        return map(rssi, -55, -45, 100, 50); // 100ms to 50ms - FAST
    } else if (rssi >= -65) {
        return map(rssi, -65, -55, 200, 100); // 200ms to 100ms - MEDIUM
    } else if (rssi >= -75) {
        return map(rssi, -75, -65, 500, 200); // 500ms to 200ms - SLOW
    } else if (rssi >= -85) {
        return map(rssi, -85, -75, 1000, 500); // 1000ms to 500ms - VERY SLOW
    } else {
        return 3000; // 3000ms max for very weak signals
    }
}

// LED control functions (auto-adapts for S3 inverted vs C5 normal)
void ledOn() {
    if (ledEnabled) {
        #if LED_INVERTED
        digitalWrite(LED_PIN, LOW);
        #else
        digitalWrite(LED_PIN, HIGH);
        #endif
    }
}

void ledOff() {
    if (ledEnabled) {
        #if LED_INVERTED
        digitalWrite(LED_PIN, HIGH);
        #else
        digitalWrite(LED_PIN, LOW);
        #endif
    }
}

// Buzzer functions
void singleBeep() {
    if (buzzerEnabled) {
        ledcWrite(BUZZER_PIN, BUZZER_DUTY);
    }
    ledOn();
    delay(100);
    if (buzzerEnabled) {
        ledcWrite(BUZZER_PIN, 0);
    }
    ledOff();
}

void zeldaSecretBeep() {
    // Original Zelda "Secret Discovery" jingle (NES 1986)
    // Descending run then resolves upward -- the iconic "you found it!" sound
    if (!buzzerEnabled) return;

    struct { int freq; int ms; } notes[] = {
        {784, 80},   // G5
        {740, 80},   // F#5
        {622, 80},   // Eb5
        {440, 80},   // A4
        {415, 80},   // Ab4
        {659, 80},   // E5
        {831, 80},   // Ab5
        {1047, 220}, // C6 (held -- the payoff)
    };

    for (auto& n : notes) {
        ledcWriteTone(BUZZER_PIN, n.freq);
        ledcWrite(BUZZER_PIN, BUZZER_DUTY);
        ledOn();
        delay(n.ms);
        ledcWrite(BUZZER_PIN, 0);
        ledOff();
        delay(15);  // tiny gap between notes for articulation
    }

    // Reset to proximity frequency, buzzer OFF
    ledcWriteTone(BUZZER_PIN, 1000);
    ledcWrite(BUZZER_PIN, 0);
    delay(300);
}

void ascendingBeeps() {
    // Ready signal - 2 fast ascending beeps with close melodic notes
    if (buzzerEnabled) {
        ledcWriteTone(BUZZER_PIN, 1900);
        ledcWrite(BUZZER_PIN, BUZZER_DUTY);
    }
    ledOn();
    delay(150);
    if (buzzerEnabled) {
        ledcWrite(BUZZER_PIN, 0);
    }
    ledOff();
    delay(50);
    
    if (buzzerEnabled) {
        ledcWriteTone(BUZZER_PIN, 2200);
        ledcWrite(BUZZER_PIN, BUZZER_DUTY);
    }
    ledOn();
    delay(150);
    if (buzzerEnabled) {
        ledcWrite(BUZZER_PIN, 0);
    }
    ledOff();
    
    // Reset to proximity frequency and ENSURE buzzer is OFF
    if (buzzerEnabled) {
        ledcWriteTone(BUZZER_PIN, 1000);  // Set to 1kHz for consistency with proximity beeps
        ledcWrite(BUZZER_PIN, 0);  // Make sure buzzer is completely off
    }
    
    // Add delay to prevent interference with proximity beeps
    delay(500);
}

void handleProximityBeeping() {
    unsigned long currentTime = millis();
    int beepInterval = calculateBeepInterval(currentRSSI);
    
    // Ultra close - solid beep (continuous)
    if (currentRSSI >= -25) {
        if (buzzerEnabled) {
            ledcWriteTone(BUZZER_PIN, 1000);
            ledcWrite(BUZZER_PIN, BUZZER_DUTY);
        }
        ledOn();
        isBeeping = true;
        Serial.println("DEBUG: Solid beep mode");
        return;
    }
    
    // Regular proximity beeping with aggressive timing
    if (isBeeping) {
        // Check if beep duration is over (50ms)
        if (currentTime - lastBeepStart >= beepDuration) {
            // Turn off beep
            if (buzzerEnabled) {
                ledcWrite(BUZZER_PIN, 0);
            }
            ledOff();
            isBeeping = false;
            Serial.println("DEBUG: Beep OFF");
        }
    } else {
        // Check if it's time for next beep
        if (currentTime - lastBeepStart >= beepInterval) {
            // Start new beep
            if (buzzerEnabled) {
                ledcWriteTone(BUZZER_PIN, 1000);
                ledcWrite(BUZZER_PIN, BUZZER_DUTY);
            }
            ledOn();
            isBeeping = true;
            lastBeepStart = currentTime;
            Serial.print("DEBUG: Beep ON, RSSI: ");
            Serial.print(currentRSSI);
            Serial.print(", interval: ");
            Serial.println(beepInterval);
        }
    }
}

void threeSameToneBeeps() {
    // Three beeps at same tone for initial detection - using 1kHz for consistency
    for (int i = 0; i < 3; i++) {
        if (buzzerEnabled) {
            ledcWriteTone(BUZZER_PIN, 1000); // Same 1kHz tone as proximity beeps
            ledcWrite(BUZZER_PIN, BUZZER_DUTY);
        }
        ledOn();
        delay(100);
        if (buzzerEnabled) {
            ledcWrite(BUZZER_PIN, 0);
        }
        ledOff();
        delay(50);
    }
    
    // Ensure buzzer is OFF (frequency already at 1kHz)
    if (buzzerEnabled) {
        ledcWrite(BUZZER_PIN, 0);
    }
    
    // Add delay to prevent interference with proximity beeps
    delay(500);
}

// Configuration storage
void saveConfiguration() {
    preferences.begin("tracker", false);
    preferences.putString("targetMAC", targetMAC);
    preferences.putBool("buzzerEnabled", buzzerEnabled);
    preferences.putBool("ledEnabled", ledEnabled);
    preferences.end();
    Serial.println("Configuration saved to NVS");
}

void loadConfiguration() {
    preferences.begin("tracker", true);
    targetMAC = preferences.getString("targetMAC", "");
    buzzerEnabled = preferences.getBool("buzzerEnabled", true);
    ledEnabled = preferences.getBool("ledEnabled", true);
    preferences.end();
    
    if (targetMAC.length() > 0) {
        targetMAC.toUpperCase(); // Ensure consistent case for comparison
        Serial.println("Configuration loaded from NVS");
        Serial.println("Target MAC: " + targetMAC);
    }
    Serial.println("Buzzer enabled: " + String(buzzerEnabled ? "Yes" : "No"));
    Serial.println("LED enabled: " + String(ledEnabled ? "Yes" : "No"));
}

String getASCIIArt() {
    return R"(
                                                                                                                                                                                                                                                                                                                            
                                                                                                                                                                                                                                                                                                                            
                                                                                                                                                                                                                                                                                                                            
                                                                                                                                                                                                                                                                                                                            
                                                                                                                                                                                                                                                                                                                            
                                                                                                                                                                                                                                                                                                                            
                                                                                                                                                                                                                                                                                                                            
                                                                                                                                                                                                                                                                                                                            
                                                                                                                                                                                                                                                                                                                            
                                                                                                                                                                                                                                                                                                                            
                                                                                                                                                                                                                                                                                                                            
                                                                                                                                                                                                                                                                                                                            
                                                                                                                                                                                                                                                                                                                            
                                                                                                                                                                                                                                                                                                                            
                                                                                                                                                                                                                                                                                                                            
                                                                                                                                                                                                                                                                                                                            
                                                                                                                                                                                                                                                                                                                            
                                                                                                                                                                                                                                                                                                                            
                                                                                                                                                                                                                                                                                                                            
                                                                                                                                                                                                                                                                                                                            
                                                                                                                                                                                                                                                                                                                            
                                                                                                                                                                                                                                                                                                                            
                                                                                                                                                                                                                                                                                                                            
                                                                                                                                                                                                                                                                                                                            
                                                                                                                                                                                                                                                                                                                            
                                                                                                                                                                                                                                                                                                                            
                                                                                                                                                                                                                                                                                                                            
                                                                                                                                                                                                                                                                                                                            
                                                                                                                                                                                                                                                                                                                            
                                                                                                                                                                                                                                                                                                                            
                                                                                                                                                                                                                                                                                                                            
                                                                                                                                                                                                                                                                                                                            
                                                                                                                                                                                                                                                                                                                            
                                                                                                                                                                                           @@@@@@@@                                                         @@@@@@@@                                        
                                                                                                                                                                                       @@@ @@@@@@@@@@                                                    @@@@@@@@@@ @@@@                                    
                                              @@@@@                                                           @@@@@                                                                               @@@@ @ @ @@@@@@@@@@@@@                                               @@@@@@@@@@@@ @@@@@@@@                                
                                         @@@@ @@@@@@@@                                                     @@@@@@@@@@@@@                                                                     @@@@ @@@@@@@@@@@@@@@@@@@@@@@@                                          @@@@@@@@@@@@@@@@@@@ @@@@@@@@@                           
                                     @@@@@@@@ @@@@@@@@@@                                                 @@@@@@@@@@@@ @@ @@@@                                                            @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@                                    @@@@@@@@@@@@@@@@@@@@@@@@@@ @@@@@@@@@                       
                                @@@@@@@@@@@@@@@@@@@@@@@@@@@                                           @@@@@@@@@@@@@@@@@@@@@@@@@@@                                                        @@@@@@ @@@@@@@@@          @@@@@@@@@@@@                                @@@@@@@@@@@@@          @@@@@@@@@@@@@@@                       
                           @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@                                      @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@                                                   @@@@@@@@@ @@@               @@@@@@@@@@@@@                          @@@@@@@@@@@@@               @@@@@@@@@@@@@                       
                          @@@ @@@@@@@@@@@@@       @@@@@@@@@@@@@@                                 @@@@@@@@@@@@@@      @@@@@@@@@@@@@@@@@@                                                  @@ @@@@@@@@@                  @@@@@@@@@@@@@@                     @@@@@@@@ @@@@                   @@@@@  @@@@                       
                          @@@@ @@@@@@@@@              @@@@@@@@@@@@                            @@@@@@@@@@@@@              @@@@@@@@@ @@ @                                                  @@@@   @@@@                   @@@@@@@@@@@ @@                     @ @@@@@@@@@@@                    @@@@  @ @@                       
                          @@@@@@@ @@@                   @@@@@@@@@@@@@                       @@@@@@@@@@@@@                  @@@@ @@@@@@@                                                   @@@  @@@@                     @@ @@@@@@@@@@                     @@@@@@@@@ @@@                     @@@  @@ @                       
                          @@@@@  @ @@                   @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@ @@@@@@@@@@                   @@@@  @@@@                                                    @@@  @@@@                     @@@  @@ @                              @ @@@@@                      @@@@ @@@@                       
                           @@@   @@@                     @@@@@@@ @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@                    @@@@   @@@                                                    @@@@ @@@@                    @@@@  @@@@                              @@@@@@@@                    @@@@@@@@@@                       
                           @@@@ @@@@                     @@ @@@@ @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@   @@@                     @@@  @@@@                                                    @@@@ @@@@@                   @@@   @ @                                 @ @@@@@                  @@@@@@@@@@                        
                           @@@@ @@@@                     @@ @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@  @@@                     @@@@ @@@@                                                    @@@@@@@ @@@                @@@@@   @ @                                 @ @ @@@@                @@@@@@@@@ @                        
                           @@@@ @@@@@                   @@@ @ @                                @@@@  @@@@                   @@@@@ @@@@                                                     @@@@@@@@@@@@             @@@@@    @@@@                               @@@@  @@@@@            @@@@@@@  @  @                        
                           @@@@ @@ @@@                 @@@@ @ @                                 @ @   @@@@                 @@@ @@@@@@                                                      @@@ @@@ @@@@@@@@     @@@@@@@@     @@@@                               @@@@   @@@@@@@@    @@@@@@@@ @@ @@@@@                        
                            @@@@@@@@@@@@             @@@@@  @@@                                @@@@   @@@@@              @@@@@@@@@@@@                                                      @@@@@@@   @@@@@@@@@@@@@@@@@        @@@                               @@@      @@@@@@@@@@@@@@@@@  @@ @@@@@                        
                            @@@@ @@ @@@@@@         @@@@@@   @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@     @@@@@@        @@@@@@@ @@ @@ @                                                      @@@@@@@       @@@@@@@@@  @@@@@@@            @@@@@@@@          @@@     @@@@  @    @@@@@@@@@@      @@ @@@@@                        
                            @@ @@@@@ @@@@@@@@@@@@@@@@@@     @@@@@                             @@@@       @@@@@@@@@@@@@@@@@@   @@@@ @@                                                      @@@@@@@       @@@  @@   @@ @@@@@           @@@@  @ @          @ @     @@@@@@ @     @ @           @@ @ @@                         
                            @@ @ @@@  @@ @@@@@@@@@@@@@@@@@@   @@@@@@@ @@@@@@@@ @@@@@@@@@@@@@@@@@@@        @@ @@@@@@@@@@@@@@@ @@@@@@@@                                                      @@ @@@@      @@@@@@@@@@  @@@@@@ @@@        @@@@@@@@@          @@@@@   @@@@   @@@   @@@@@@@@      @@ @@@@                         
                            @@@@ @@@  @@@@     @@@@  @@@@@@     @ @@@@@   @@@@@@@@@        @@@@@@@@@@@@   @ @ @@@@@@@@@  @@@@@@@@@@@@                                                       @@@@@@@  @@@ @@  @@@@@@@@@    @@@@         @@@@@@@   @@@@@   @@@@@@ @@@  @ @@@@@@@@@@@@@@@      @@@@@ @                         
                            @@@@@@@@  @@@@  @@@@@@@@ @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@  @@@@@@@@@@ @@@@@@@@ @ @@@@@@@@@@@@@@@ @@@@                                                        @@@@@@@  @ @ @@  @@@@@@@@@@   @@@@          @@@@@@   @@@@@   @@@@@@ @ @   @@@@@@@@ @@  @@@      @@@@@@@                         
                             @@@@ @@  @ @ @@@@@@@@@@@@@@@@@@@@@@@ @@@@@@@@@@@ @@@@ @@@@@@@@@@@ @@@@@@@@@@@@   @  @@@@@@@ @@@@@@ @@@@                                                        @ @@ @@  @@@@ @  @@@@@@@@@@@@@@@            @@@@@@   @@@@@   @@@@@@ @@@@  @@@  @@@@@@@@@@@      @@@@@@@                         
                             @  @ @@  @@@@@@@@@@@@@@@ @@@@@@@ @@@@@@@@@@@@ @@@  @@@@@@@@@@@@@@@@@ @  @@ @@@@  @  @@@@@ @@@   @@ @@ @                                                        @ @@@@@  @@@ @@  @@@@@ @@ @ @@ @@           @@@@@@   @@@@@   @@@@@@@@@@           @@@@@@       @@@@  @                          
                             @@ @ @@  @@@@@@@@@@@@@@@ @@@@@@@@@@@   @@@@@@ @@@@ @@ @@@@@@@@@@@@@@ @@@@@@@@@@  @@@@@@@@@@     @@@@@ @                                                        @@ @@@@  @@@@    @@@@@@   @@@@@@@           @  @@@   @@@@@   @@@@@@@@@@           @@@@ @       @@@@@@@                          
                             @@@@@@@  @@@@@@    @@@ @ @@ @@@@@@@@@   @@@@@@@@@@ @@@@@@@@@@@ @@@@@ @ @@ @@@@@  @@@@@ @@       @@@@@@                                                          @@@@@@  @@@@    @@@@@@       @@@           @@@@@@   @@@@@   @@@@@@@@@@           @@@@@@       @@@@@@@                          
                             @@ @@@@  @@@@@@@@@@@@@@@ @@@@@@@    @@@@@@ @@@@@@@@@@@@@@ @@@@@@@@@@@@@@@@@@@    @@@@@@@@       @@@@@@                                                          @@@@@@  @@@      @ @ @@@@@@  @@@@ @        @@@@@@   @@@@@   @@@  @@@@@   @@@@@@  @@@@         @@@@@@                           
                              @  @@@  @@@@@@@@ @@@@@@ @@@@@@@    @@@@@@@@@@@@@@@@@@ @@@@@@@@@@@@@ @ @@@@@@    @@@@@@@@@      @@@@@@                                                          @@@@@@   @@@    @@@@ @ @@@@@@@@@@ @        @@@@@@   @@ @@      @ @@@@@@@@@@@@@@  @@@@ @       @@@@@@                           
                              @@ @@@   @@@@@@@@@@@@   @@@@@@@     @@@@@@ @@@@@@@@@@@@@@    @@@@@@@@@@@@@@@    @@@@@@@@@      @@@@@@                                                           @@@@@   @ @    @@@@ @@@@@@@@@@@ @@        @@@@@@   @@@@@      @@@@@@@ @ @@@@@@  @@@@         @@@  @                           
                              @@@@@@      @@@@ @@@       @@@@             @@@ @@@@@      @@@@   @   @@@       @@@  @@@@      @@@@@                                                            @@@@@   @@@     @@@     @@@@@@@           @@@                      @@@@@@@@@    @@@@         @@@@@@                           
                              @@@@@@@        @@       @@@@@   @@@@@@      @@@@@@@@@@@@@@@@   @    @@@@@@@@@@@@              @@ @@@                                                            @@@ @                                                                                        @@@@@@                           
                              @@@@@@@      @@@@@      @ @@@@@ @@@@@@@@@   @@@@@ @@@@ @@@@ @@@@   @@@@@@@@  @@@              @@@@@@                                                            @@@@@@             @@@@@@@@@    @@@   @@@    @@@@@@@@@    @@@@@@@@     @@@@@@@@@             @@@@@                            
                               @  @@@      @@@@       @@@@@ @ @@@@@@@ @   @@@@@@@@@@@@@@@        @@@@@@@@@@@@@              @@@@ @                                                            @@@@@@             @@    @@@    @ @   @ @@@  @@@    @@    @@ @@@@@     @@@    @@@            @ @@@                            
                               @@@@@@      @@@@       @@@@@@@ @@@@@@@@ @@@@@@@@      @@@@      @@@@@@@     @@ @@@@@         @@@@@@                                                            @@@@@@             @@@@@@@@@@@@ @@@   @@@@@  @@@@@@@ @@@@  @@@@@@@@@@@  @@@@@@ @@@@          @ @@@                            
                               @@@@@@     @@@@@      @@@@@@@@ @@@@@@  @@ @@@@@@      @@@@      @@@@@@@@      @@@@@@         @@@@@                                                              @@@@@           @@@@@   @@ @@@ @@@@  @@@@@@@@@@   @@@@@@@@@@   @@@@@@@@@@   @@@@@@         @@@@ @                            
                                 @@@@     @@@@@      @@@@@@@@ @@@@@@  @@@@@@@@@      @@@@      @@@@@@@@@@@@@@@@@@@@         @@@@@                                                              @@@ @           @@ @@@  @@@@@@ @@@@@ @@@ @@@@@@   @@@@@@@@@@   @@ @@@@@@@   @@@@@@         @@@@@@                            
                                @@@@@     @@@@@@@@@@@@@@@@@@@ @@@@@@     @@@@@@     @@@@@@@     @@@@@@@@@@@@@@ @@@@         @ @ @                                                              @@@@@           @@@@@@ @  @@@@ @@@@  @@@@@@@@@@   @@@@@@@@@@   @@@@@@@@@@   @@@@@@         @@@@@@                            
                                @@@ @     @@ @  @      @@@   @@@  @@      @@@@@     @@   @@         @@@@@@@@@  @@           @@ @@                                                              @@@@@              @@@  @  @@@ @@@  @@@@@@@@@@@   @@@ @@@@@@   @@ @@@@@@@   @@ @@@         @@@ @                             
                                @   @        @@@@@@@@@@@@@    @@@@@@    @ @@@@@@@@  @@@@@@@         @@@@@@@@@@@@            @@@@@                                                              @@@@                       @ @@@@@  @ @@ @ @@@@    @@ @@@@@@    @@@@@@@@@                    @@@                             
                                @@@@@      @@@@@@@@@@@@@@@@@@@  @@@@   @@@   @@@@@@  @@@@   @@@@@       @@@@@               @@@@@                                                               @@@                       @@@@@@@  @@@@@@@@@@@     @@@@@@@@    @@@@ @@@@                    @ @                             
                                @@@@@      @@ @@@  @@@ @  @@ @  @@@@   @ @@@@@ @@@@  @@@@   @ @@@@@@   @@@@@@                @@@                                                                @@@               @@@        @@@@   @@@  @@@@@   @@@  @@@@@        @@@@@                   @@@@                             
                                 @@@       @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@ @@@@@@@ @@@@@@@   @ @@@@@@@@@@@@@ @@               @@@                                                                @ @              @@@@@@@@@@@ @@@@   @@@@ @@@@@@@@@@@@@ @@@@  @@@@  @@@@@                   @@@@                             
                                 @@@              @@@@@     @@@@@@@@@@@@@@@@@  @@@@@@@@@@   @@@@@@@@@@@@@@@@@@@@             @ @                                                                @ @              @@@@@@@@@ @  @ @   @@@     @@@@@@@@ @  @ @     @    @ @                   @ @                              
                                 @ @              @@@@@     @@@@@@@@@@@@@@@@@  @@@@@@@@@@   @ @@@@@@@@@@ @@@@@ @             @ @                                                                @@@              @@@@@@@@@@@  @@@   @@@@    @@@@@@@@@@  @@@  @@@@    @@@                   @ @                              
                                 @@@@             @@@@@     @@@@  @@@@@@@ @@@@@@@ @@ @@@@   @ @@@@@@@@@@@@@ @  @@@          @@@@                                                                 @@@                                                                                       @@@                              
                                 @@@@            @@@@@@@      @@@@@@    @@@ @@@@@ @@@@@@@@@@@@@@@@@@@@@@ @@@@@@@@@          @@@                                                                  @ @  @@@    @@@   @@@@   @@@   @@@@@@@@@@@@ @@@@@@@@@@         @@@   @@@@   @@@@@@@@@     @@@                              
                                  @@@  @@@@@@    @@@@ @@      @@@@@@    @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@ @@@@@@@@@ @@@@@@@  @@@                                                                  @@@  @ @    @@@@  @@@@   @@@@  @@@@@@@@  @@ @@ @ @ @@@@        @ @   @@@@   @@  @ @@@@   @@@@                              
                                  @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@ @@    @@@@@@@@@@@ @ @@@@@@@ @@@@ @@@@@@ @@@@@@@@@@@@@@@@@@@@                                                                  @@@  @@@    @@@@  @@@@   @@@@  @@@@@@@@@@@@  @@@@@@@@@@        @@@   @@@@   @@@@@@@@@@   @@@@                              
                                  @ @@@@@  @@@@@@ @@@@@        @@ @@@@@@ @@@@@     @ @@@@@@@@@@ @@  @@@       @@@@@@@@  @@@@@ @                                                                  @ @ @@@     @@@@@@@@@@@  @@@@     @@@ @@   @@@@    @@@@        @@@   @@@@@@ @@    @@@@@@ @@@                               
                                  @@@@ @@@@@@ @@ @@@@@@@@@@@@@@ @@@@@@@@@@@@@@@@@@@@@@@@ @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@   @@@ @                                                                  @@@@@@@@    @@@@@@@@@@@  @@@@@@   @@@@@@   @@@@@   @@@@@@    @@@@@   @@@@@@ @@@@ @@@ @ @ @@@                               
                                  @@@@@@@@@@@@ @@ @          @@@@@@@@@@@                 @@@@@@@ @@@          @ @@@@@@@@@@@@@@                                                                    @@@@@@@@   @@@@@@@@@@@  @@@@ @   @@ @@@   @@@@@   @@@@@@    @@ @@   @@ @@@ @@@@ @ @  @@ @@@                               
                                  @@@@@@@@@@@@@@@@@        @@@@@@@@@                          @@@@@@@@        @ @@@@ @@@@@@@@@                                                                    @@@@@@@    @@@@@@@@@    @@@@@@   @@@@@@   @@@@@   @@@@@@    @@@@@   @@@@@@ @@@@ @@@@   @@@                                
                                   @@@@@@@@@@@@@ @@      @@@@@@@                                @@@@@@@@      @@@ @@@@@@@@@@@@                                                                    @@@@@@@     @ @ @@@@@   @@@ @@   @@@@@    @@@@@    @@@@@    @@@@@   @@@@@@       @@@@@@@@@                                
                                   @@@@@@@@@@@@@@@@@   @@@  @@@@                                 @@@@@@@@@    @@@@@@@@@@@@@@@@                                                                    @ @@@@@     @ @ @@@@@   @ @@ @    @ @@    @@@@@   @@@@ @    @@@@@   @@@  @  @@@  @ @@ @@ @                                
                                   @@@ @@@@@@@@@@@@@ @@@@@@@@@                                      @@@@@@@@ @@@@ @@@  @@@@@@                                                                      @ @@@@    @@@@ @@@@@   @ @@@@   @@@@     @@@@@   @ @@@@    @@@@@   @@@@@@  @ @  @ @@@ @@@                                
                                   @@@@@@@@@@@@@ @@@@@@  @@                                         @@@@@ @@@@@@   @@@@@@@@@@                                                                      @@@@@  @@@@@@@ @@@@    @ @      @ @      @@@ @@@@@@@       @@@@@@@@@ @  @  @@@@@@ @  @@@@                                
                                    @@@@@@@@@@@@ @@@@@@@@@@                                          @@ @@@ @@@@   @@@@@@@@@@                                                                      @@@    @@@ @ @ @@@@    @ @      @ @      @@@@@@@@@ @           @@@@@ @ @@  @@@@ @ @  @ @                                 
                                    @@@  @@@@@   @@@@@ @@@                                            @@ @@@@@@@   @@ @@@ @@@                                                                      @@@@@@ @@@ @@@ @@@@    @@@      @@@       @@@@@@@@@@           @@@@@@@     @@@@ @@@@@@@@                                 
                                    @@@@@@@ @@   @@@@ @@@@                                             @@@@ @@@@   @@@@@@@@@@                                                                       @@@@@   @@@                              @@@@                               @@@   @@@@@                                 
                                    @@@  @@@@@@@@    @@@@    @@@@@@@                       @@@@@@@@@@@  @ @     @@@@@@@@ @@@                                                                        @ @@@  @@@@                              @@@@                               @@@  @@ @@@                                 
                                      @@ @@@@@ @@    @@@@  @@@@@@@@@@                      @@@@@@@@@@@  @@@@    @@ @@@@@ @@@                                                                        @@ @@  @@@@        @@@@                  @@@@                   @@@@        @ @  @@@@@@                                 
                                     @@@ @@@@@ @@    @ @   @@@@   @@@@                     @@       @@   @@@   @@@ @@@@@ @ @                                                                        @@@@@@ @@@         @@@@@@                @@@@@                @@@@@@        @ @  @@@ @                                  
                                     @@@  @@@@ @@    @ @   @@      @@@                     @@       @@   @ @   @@@ @@@@  @ @                                                                        @@@@@@ @@@         @@@@@@@             @@@@@@@@             @@@@ @@@        @@@@ @@@ @                                  
                                     @@@@ @@@@@@@    @ @   @@@@  @@@@@                     @@       @@   @@@   @@@@@@@@  @@@                                                                        @@@@@@ @@@          @@@@@@@@@@@@@@@@@ @@@@@ @@@@@@@@@@@@@@@@@@ @@@@         @@@@@@@@ @                                  
                                     @@ @@@@@@@@     @@@@ @@@@@@@@@@@                      @@@@@@@@@@@@ @@@@     @@@@@@@@@@                                                                          @ @@@ @@@           @@@@@@@@@   @@@@@@@@@@@@@@@@@@@@@  @@@@@@@@@            @@@@@@@@@                                  
                                      @@@ @@ @@@     @@@@@  @@@@@@@@                       @@@@@@@@@@@@@@ @      @@@@@   @@                                                                          @@@@@@@@@             @@@@@@@@@@@@ @@@ @@@@@@@@@@@ @@@@@@@@@@@              @@@@@@@@@                                  
                                      @@@@@@@@@@      @@@@@@                                   @@@   @@@@@@      @@@@@@@@ @                                                                          @@@@@@@@@              @@@@@ @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@              @ @@@@@@@                                  
                                      @@@ @@@@@       @@@@@                                  @@@@@@@  @@@@@       @@@@ @@@@                                                                          @ @@@@@@@              @@ @@@@ @@@@@ @@@     @@ @@@@@@@@@@@@@               @ @@@@ @                                   
                                      @ @@@@@@@@@@    @@@@@                                  @@@ @@@ @@@@@@    @@@@@@@@@@@@                                                                          @@@ @@@@               @@@@@@@@ @@@@@@@@@@@@@@@@@@@@@@@@ @@@@               @ @@@@ @                                   
                                      @@@@@@@@@ @@   @@@@@@                                  @@@@@@@ @@@@@@@   @@ @@@@@@@@@                                                                           @@@@@@@                @@@@@@@@@@@@@@@@@@@@@@@@@@@@@ @@@@@@@               @@@@@@@@                                   
                                      @@@@@@@@@@@@   @@@@@ @@@                                 @@@   @@@ @ @   @@@@@@@@@@@                                                                            @@@@@@@                @@@@@@@@@   @@@@@@@@@@@@@  @@@@@@@@@@               @@@@ @@@                                   
                                       @ @@@@@@@@@   @@ @@@@@@                                        @ @@@ @@ @@@@@@@@@@@                                                                            @ @@@@@              @@@@@@@ @@       @@@@@@@@@@   @@@@@@ @@@              @@@@@@@@                                   
                                       @@@@@@@@@@@ @@ @@@@@@@@@@@@                             @@@    @@@@@@ @@@@@@@@ @@@@                                                                            @ @@@@@            @@@@@@@@@@@@       @@@@@@@@@    @@@@@@@@@@@@@            @@@@ @                                    
                                       @ @@@@@@@@@@@ @@ @ @@@ @@@@@@                        @@@@ @ @   @@@  @ @@@@@@@@@  @                                                                            @@@ @@@          @@@@ @@@@@@@@@       @@@@@@@@     @@@@@@@@@ @@@@@          @@@@@@                                    
                                       @@@@ @@@@ @@@@@@@@   @@@@@@@@ @@@@               @@@@@ @@@@     @@@  @@@@ @@@@@@@@@                                                                             @@@     @@@@@@@@@@@@@ @@@@@@@@    @@@@@@@@@@@@    @@@@@@@@@@@@@@@@@@@@@@      @@@                                    
                                       @@@@ @@@@ @ @ @@@@     @@@@@@@@@@@ @@@@@@@@@ @@@ @@@@@@@@       @@@@ @@@@ @@@  @@@                                                                              @ @     @@      @@@@@@@  @@@@@    @@  @@@@@@@@    @@@@@   @@@@@@@     @@      @ @                                    
                                        @ @ @@@@ @ @ @ @        @@@ @ @@@ @@@@@@ @@ @ @ @@@@  @@       @@@@ @@@@ @@@@@@@@                                                                              @@@     @@@@@@@@@@@@@@@@@@@@@@    @@@@@@@@@@@@    @@@@@@@@@@@@@@@@@@@@@@     @@@                                     
                                        @@@      @ @ @@@         @@@@@@@  @@@@@@@@@ @@@  @  @           @@@ @@@@     @@@@                                                                                               @@@@@ @@@@@@@        @@@@        @@@@@@@@ @@@@              @@@                                     
                                        @@@      @ @ @@@            @@ @@@                @@            @@@ @@@@     @@@                                                                               @ @                @@@@@ @@@@@       @@@@@@@      @@@@@@@@@@@                @ @                                     
                                        @@@      @ @ @ @             @@@ @                              @@@ @@@@     @@@                                                                               @ @                   @@@@@@@@       @@@@@@@      @@@@@@@@@                  @ @                                     
                                        @ @   @@@@ @ @@@               @@@                              @@@ @@@@@@   @@@                                                                               @@@                     @@@@@@     @@@@@@@@@@@    @@@@@@@                    @@@                                     
                                        @ @ @@@ @@ @                                                        @@@@ @@@@@@@                                                                               @@@                     @@@@@@     @@ @@@@  @@    @@@@@@@                    @@@@                                    
                                        @@@@@ @@@@@@@   @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@   @ @@@@@ @@@@                                                                               @@@                   @@@@@@@@     @@@@@@@@@@@    @@@@@@@@@                   @@@                                    
                                        @@@ @@    @ @@ @@@                                             @@  @@ @   @@@@@@                                                                              @@@@                @@@@@ @@@@@        @@@@        @@@@@@@@@@@                 @@@                                    
                                        @@@@@      @ @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@ @ @@      @@@@@                                                                             @ @               @@@@@ @@@@@@@      @@@@@@@@@     @@@@@@@@ @@@@@              @ @                                    
                                        @@@@@@@@@@@@@@@  @                                            @ @@ @@@@@@@@@@@@@@                                                                             @@@      @@@@@@@@@@@@@@@@@@@@@@      @ @@@@@ @     @@@@@@@ @@@@@@@@@@@@@@      @@@@                                   
                                       @@@@@@@@@@@@@@@   @                                            @ @@ @ @@@@@@@@ @@@                                                                             @@@      @@      @@@ @@@  @@@@@      @@@@@@@@@     @@@@@   @@@ @@@     @@      @@@@                                   
                                       @@@@@@@@   @@@@   @                                            @ @@ @ @@   @@@ @@@@                                                                            @ @      @@@@@@@@@@@@@@@@@@@@@@        @@@@@       @@@@@@@@@@@@@@@@@@@@@@       @@@                                   
                                       @@  @@@@@@@@@@@   @                                            @ @@ @ @@@@@@@@  @ @                                                                           @@@@              @@@@@@@@@@@@@@        @@@@        @@@@@@@@@@@@@@@              @@@                                   
                                       @@@ @@@@@@@@@ @   @                                            @ @@ @ @@@@@@@@  @@@                                                                           @@@                 @@@@@@@@@@@@        @@@@@@      @@@@@@@@@@@@                 @ @                                   
                                       @ @ @   @@@   @   @                                            @ @@ @   @@@     @@@                                                                           @@@                  @@ @@@@@ @@        @@@@        @@@@@@@@@@@                  @ @                                   
                                       @   @@@@@@@@@ @   @                                            @ @@ @ @@@@@@@   @ @                                                                           @ @                  @@@@@@@@ @@@       @@@@       @@@@@@@@@@@@                  @@@@                                  
                                      @@@@ @@@@@@@@@@@   @                                            @ @@ @@@@@@@@@@  @@@@                                                                          @@@                  @@@@ @@@@@@@@@@@@@@@@@@@@@@@@@@@ @@@@@@@@@@                 @@@@                                  
                                      @@@@ @@@@   @@@@   @                                            @ @@ @@@@   @@@   @@@                                                                          @@@                  @@@@@@@@@@@ @@@@   @@@@   @@@@@@@@@ @@@@@@@                  @ @                                  
                                      @@@@ @@@@@@@@@@@   @                                            @ @@ @@@@@@@@@@   @@@                                                                         @@@@                  @@@@@ @@@@@@@ @@@@@@@@@@@@@@  @@@@@@@@@@@@@                  @@@                                  
                                      @@@@ @@@@@@@@@ @   @                                            @ @@ @ @@@@@@@@   @ @                                                                         @@@                  @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@ @@@@@@@  @@@                 @ @                                  
                                      @ @@ @         @   @                                            @ @@ @            @@@                                                                         @@@                @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@                @@@@                                 
                                     @@@@@ @@@@@@@@@ @   @                                            @ @@ @ @@@@@@@@   @@@                                                                         @@@              @@@@ @@@@@@@@@@@   @@@@ @@@@@@@@@   @@@@@@@@@@@@@@@@              @@@@                                 
                                     @@@@@ @@@@@@@@@@@   @                                            @ @@ @@@@@@@@@@   @@@@                                                                        @ @              @@@@@@@              @@@@@@@@@@@             @@@@ @@               @ @                                 
                                     @ @ @ @@@    @@@@   @                                            @ @@ @@@@   @@@    @@@                                                                        @@@              @@@@@                 @@@@@@@@                 @@@@@@              @@@                                 
                                     @@@ @ @@@@@@@@@@@   @                                            @ @@ @@@@@@@@@@    @@@                                                                       @@@               @@@@                    @@@@@                    @@@               @@@                                 
                                     @@@ @ @@@@@@@@@ @   @                                            @ @@ @ @@@@@@@@    @@@                                                                       @ @                                       @@@@                                       @ @                                 
                                    @@@  @ @  @@@@   @   @                                            @ @@ @   @@@@      @ @                                                                       @@@                                       @@@@                                       @@@@                                
                                    @@@  @ @@@@@@@@@ @   @                                            @ @@ @ @@@@@@@@    @@@                                                                       @ @                   @@@ @@@             @@@@@@           @@@ @@@@                  @@@@                                
                                    @@@  @ @@@@@@@@@@@   @                                            @ @@ @@@@@@@@@@     @@@                                                                     @ @                    @ @@@ @@@          @@@@@@@          @@ @@@@@@                @@@@ @                                
                                    @@@  @ @@@@  @@@@@   @                                            @ @@ @@@@   @@@     @ @                                                                     @@@@@@@                @@@@@@@ @          @ @ @ @        @@@@@@@@@@@                @@@@@@                                
                                   @@@@@@@ @@@@@@@@@@@   @                                            @ @@ @@@@@@@@@@     @@@                                                                     @@@@@@@                   @ @@@@@@@       @ @ @ @       @@ @@@@@@@ @                @@@@@@                                
                                   @@@@@@@ @@@@@@@@@ @   @                                            @ @@@@@@@@@@@@   @@@@@@                                                                     @@    @                @  @@@ @ @ @@@     @ @ @ @     @@@@@ @@@@ @ @               @@@@@@@@                               
                                   @@@@@@@ @  @@@@   @   @                                            @ @ @@@@@@@@@    @@@@@@                                                                    @@@@   @                @    @@@@@@@ @     @ @ @ @   @@@ @  @@@@  @ @               @@@@@@@@                               
                                   @@@@@@  @@@@@@@@@ @   @                                            @ @  @@@@@@@@@@  @@@@@@@                                                                   @ @@   @                @ @    @@ @@@@@@@  @ @ @ @  @@ @@@@@@@    @ @               @@  @  @                               
                                   @ @@@@  @@@@@@@@@@@   @                                            @ @   @@@@@@@@@  @@@@@@@                                                                   @@@@@  @                @ @     @@@ @ @ @@ @ @ @ @@@@@@ @ @@      @ @               @@  @@@@@                              
                                  @@@@@@@  @@@@  @@@@@   @                                            @ @   @@@   @@@  @@@@@@@                                                                   @@@@@ @@@               @ @       @@@@@@@@@@ @ @ @@ @ @ @@@       @ @               @@  @@@@@                              
                                  @ @@@@@  @@@@@@@@@@@   @                                            @ @   @@@@@@@@@  @@@@@@@                                                                   @ @@@ @@@               @ @         @@ @@@ @ @ @ @@@@@@@@         @ @               @@  @@@@@                              
                                  @@@@@@@  @@@@@@@@@@ @@ @                                            @ @    @@@@@@@   @@@@@@ @                                                                 @@@@@@ @@@               @ @          @@@ @@@   @@@@@ @            @ @              @@@  @@@@@                              
                                  @@@@@ @  @  @@@@@@ @@@ @                                            @ @     @@@@@    @@ @@                                                                    @@@@@@ @@@               @ @            @@@@@@  @@@ @@@            @ @              @@@   @  @                              
                                  @ @@@ @  @@@@@@@@@@  @ @                                            @ @    @@@@@@@@  @@ @@@@@                                                                 @@ @@@ @@@@              @ @              @@ @  @@ @@              @ @              @@    @@@@                              
                                  @ @@@ @  @@@@@@@@@@  @ @                                            @ @   @@@@ @@@@  @@ @@@ @                                                                 @ @@ @ @@@@              @ @               @@@@ @@@                @ @             @@@    @@@@@                             
                                 @@@@@@ @  @@@@  @@@@  @ @                                            @ @   @@@@ @@@@  @@ @@ @@@                                                                @@@@ @ @ @@              @ @                @ @ @ @                @ @             @@@    @@@@@                             
                                 @@@@@  @  @@@@@@@@@   @ @                                            @ @   @@@@@@@@@  @@ @@@@@@                                                                @@@@ @ @ @@              @ @                @ @ @ @                @ @             @@@     @  @                             
                                 @@ @@  @  @ @@@@@@    @ @                                            @ @    @@@@@@@   @@  @@@@@                                                               @@@@  @ @ @@@                                @ @ @ @                @ @             @@      @@@@                             
                                 @ @@@  @  @ @@@@@@    @ @                                            @ @     @@@@@    @@  @@@ @                                                               @@@@  @ @ @@@             @@@@@@@@@@@@@@@@@@@@@@ @@@@@@@@@@@@@@@@@@@@@@             @@      @@@@                             
                                 @ @@@  @  @@@@@@@@@   @ @                                            @ @    @@@@@@@@  @@  @@@@@                                                               @@@@  @ @  @@   @@@@@@@@@@@@                 @@@ @@@                @@@ @@@@@@@@@  @@@       @@@@                            
                                @@@@@   @  @@@@@ @@@   @ @                                            @ @   @@@@ @@@@  @@  @@ @@                                                               @@@@  @ @  @@@@@@@@@ @@@  @@@@@@@@@@@@@@@@@@@@@ @@@@@@@@@@@@@@@@@@@@@@ @@  @@@ @   @@@       @@@@                            
                                @@@@@   @  @@@@  @@@   @ @                                            @ @   @@@@ @@@@  @@   @@@@@                                                              @@@   @ @  @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@ @@@@@@@@@@@@@@@@@@@@@@@@ @@@@@@@@@@@@@@ @@        @@@@                            
                                @ @@@   @  @@@@@@@@@   @@@                                            @@@    @@@@@@@@  @@   @@@@@                                                              @@@   @ @  @@@ @@@@ @@ @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@ @@        @@@@                            
                                @@@@@   @@@@ @@@@@@     @                                             @@@    @@@@@@@@@@@@   @@@@@                                                             @@@@   @ @  @@@@@@@@@@@@@@@@@@@@@@ @@@@@ @@@@@ @@@@ @@@@@@@ @  @ @@@@@@@@@@@@@@@@@@ @@         @@@                            
                                @@@@    @@@@@                                                                       @@@@@   @@@@@                                                             @@@@   @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@ @@@@@@@@@@@@@ @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@ @@@@@@     @@@                            
                                 @@@    @@@ @@@                                                                   @@@@@@@   @@@ @                                                             @@@@   @@@ @@@@@@@@@@@@@@@@@@@@@@@ @@        @@@@ @@@@       @@ @@@@@@@@@@@@@@@@@@@@@@@@@@@@   @@@@                           
                               @@@@@    @@@@@ @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@ @@@@@    @@@@                                                             @@@  @@@@@@@@@@@@@@@@@@@@@ @@@@    @@@@@@@@@@@@@@@@@@@@@@@@@@@@    @@@@  @@@@@@@@@@@@@@@@@@@@@ @@@@                           
                               @@@@   @@@@@@@@@@@@@@        @@@                                   @@@        @@@@@@@@@@@@@@  @@@@@                                                            @@@@@@@@@@@         @@@@ @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@  @@@@@@        @@@@@ @@  @@                           
                               @@@@ @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@  @@ @                                                           @@@@@@@@@              @@@@@@@@@@@@@@@@@@@@@@@@@@  @@@@@@@@@@@@@@@@@@@@@ @@@@              @@@@@@@@@                           
                              @@@@@@@@@@@@@       @@@@@@@@@@@@@ @                               @ @ @@@@@@@@@@@      @@@@@@@@@@@@@                                                           @@@@@@@@                 @@@@@@@@                                   @@@@@@@                  @@@@@@@                           
                              @ @@@@@@@@             @@@@ @@@ @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@  @  @@@@             @@@@@@@@@                                                           @@@ @@@                   @@@@@ @                                   @ @@@@                    @@@@ @@                          
                              @@@@@@@@                 @@@@@@@@@                                 @@  @ @@                  @@@@@@@                                                          @@@@@@@                     @@@@ @                                   @ @@@@                     @@ @@@                          
                              @@@ @@@                    @@@@ @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@ @@@@                    @@@@@@@                                                         @ @ @@                      @@@@ @                                   @ @@@                      @@ @@@                          
                              @ @@@@                     @@@@ @                                   @ @@@@                    @@@ @@@                                                         @@@ @@@                     @@ @ @                                   @ @@@@                    @@@ @@@                          
                             @@@@@@@                     @@ @ @                                   @ @@@@                     @@ @@@                                                         @@@@@@@@                   @@@ @ @                                   @ @@@@@                   @@@@@@@@                         
                             @@@@@@@                     @@ @ @                                   @ @@@@                     @@ @@@                                                         @ @@@@@@@                 @@@@@@ @                                   @ @@@@@@                @@@ @@@@ @                         
                             @@@@@@@@                   @@@@@ @                                   @ @@@@@                   @@@@ @ @                                                        @@@@@@@@@@@             @@@@@@@@ @                                   @ @@@@@@@@            @@@@@@@@@@@@                         
                             @@@@@@@@@                 @@@@@@ @                                   @ @@@@@@                 @@@@@  @                                                        @@@@@ @@@@@@@@@       @@@@@@@@@@@@@                                   @@@@ @@ @@@@@      @@@@@@@@@ @@@@@                         
                             @ @@@@@@@@               @@@@@@@ @                                   @ @@@@@@               @@@@@@@ @@@                                                       @ @@@  @@@@@@@@@@@@@@@@@@@@ @@@@@@@                                   @@@@@ @@@@@@@@@@@@@@@@@@@@@  @@@ @                         
                             @@@@@ @@@@@@@         @@@@@@@@@@ @                                   @ @@ @@@@@@@         @@@@ @@@@ @ @                                                       @ @@@     @@@@ @@@@@@@ @@@@@@@@@@@                                     @@@@@@@@@@@@@@@@@@@@@@@     @@@ @                         
                            @@@@@@  @@@@@@@@@@@@@@@@@@@@@@@@@@@                                   @@@@@@@@@@@@@@@@@@@@@@@@@@@ @@ @@@                                                       @ @@@@@@@     @@@@@@@@@@@@@@@@@                                          @@@@@@@ @@@@@@@@@     @@@@@@@@@                         
                            @@@@@@    @@@@@@@@@@@@@@@@@@@@@@@@@                                   @@@@@@@@@ @@@@@@@@@@@@@@@   @@  @@@                                                      @@@@@@@@@@@@@  @@@@@@ @@@@@@                                                @@@@@@@@@@@@@  @@@@@@@@@@@@@                         
                            @@@@@@@@@      @@@@@@@@@@@@@@@@@                                         @@@@@@@@@@@@@@@@@     @@@@@@@@ @                                                          @@@@@@@@@@@@@@ @@@@@@                                                      @@@@@@ @@@@@@@@@@@@@@@                            
                            @@@@@@@@@@@@@@   @@@@@ @@@@@@                                              @@@@@@@ @@@@@   @@@@@@@@@@@@@@                                                              @@@@@@@@@@@@@@                                                            @@@@@@@@@@@@@@@                                
                               @@@ @@@@@@@@@@@@@@ @@@@                                                     @@@@  @@@@@@@@@@@@@@@@@                                                                      @@@@@@                                                                  @@@@@@@                                     
                                   @@@ @@@@@@@@@@@@                                                           @@@@@@@@@@@@@@@@                                                                                                                                                                                              
                                       @@@@  @@@                                                                @@@@ @@@@@                                                                                                                                                                                                  
                                                                                                                    @                                                                                                                                                                                                       
)";
}

String generateConfigHTML() {
    // Generate random MAC for placeholder
    String randomMAC = "";
    randomSeed(analogRead(0) + micros());
    for (int i = 0; i < 6; i++) {
        if (i > 0) randomMAC += ":";
        byte randByte = random(0, 256);
        if (randByte < 16) randomMAC += "0";
        randomMAC += String(randByte, HEX);
    }
    randomMAC.toLowerCase();

    String html = R"html(
<!DOCTYPE html>
<html>
<head>
    <title>OUI-SPY FOXHUNT Configuration</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        * { box-sizing: border-box; }
        body {
            font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
            margin: 0; 
            padding: 20px;
            background: #0f0f23;
            color: #ffffff;
            position: relative;
            overflow-x: hidden;
        }
        .ascii-background {
            position: fixed;
            top: 0;
            left: 0;
            width: 100%;
            height: 100%;
            z-index: -1;
            opacity: 0.6;
            color: #00ff00;
            font-family: 'Courier New', monospace;
            font-size: 8px;
            line-height: 8px;
            white-space: pre;
            pointer-events: none;
            overflow: hidden;
        }
        .container {
            max-width: 700px; 
            margin: 0 auto; 
            background: rgba(255, 255, 255, 0.02);
            padding: 40px; 
            border-radius: 16px;
            box-shadow: 0 8px 32px rgba(0, 0, 0, 0.2); 
            backdrop-filter: blur(5px);
            border: 1px solid rgba(255, 255, 255, 0.05);
            position: relative;
            z-index: 1;
        }
        h1 {
            text-align: center;
            margin-bottom: 20px;
            margin-top: 0px;
            font-size: 48px;
            font-weight: 700;
            color: #8a2be2;
            background: -webkit-linear-gradient(45deg, #8a2be2, #4169e1);
            background: -moz-linear-gradient(45deg, #8a2be2, #4169e1);
            background: linear-gradient(45deg, #8a2be2, #4169e1);
            -webkit-background-clip: text;
            -moz-background-clip: text;
            background-clip: text;
            -webkit-text-fill-color: transparent;
            -moz-text-fill-color: transparent;
            letter-spacing: 3px;
        }
        @media (max-width: 768px) {
            h1 {
                font-size: clamp(32px, 8vw, 48px);
                letter-spacing: 2px;
                margin-bottom: 15px;
                text-align: center;
                display: block;
                width: 100%;
            }
            .container {
                padding: 20px;
                margin: 10px;
            }
        }
        .section { 
            margin-bottom: 30px; 
            padding: 25px; 
            border: 1px solid rgba(255, 255, 255, 0.1); 
            border-radius: 12px; 
            background: rgba(255, 255, 255, 0.01); 
            backdrop-filter: blur(3px);
        }
        .section h3 { 
            margin-top: 0; 
            color: #ffffff; 
            font-size: 18px;
            font-weight: 600;
            margin-bottom: 15px;
        }
        textarea { 
            width: 100%; 
            min-height: 120px;
            padding: 15px; 
            border: 1px solid rgba(255, 255, 255, 0.2); 
            border-radius: 8px; 
            background: rgba(255, 255, 255, 0.02);
            color: #ffffff;
            font-family: 'Courier New', monospace;
            font-size: 14px;
            resize: vertical;
        }
        textarea:focus {
            outline: none;
            border-color: #4ecdc4;
            box-shadow: 0 0 0 3px rgba(78, 205, 196, 0.2);
        }
        .toggle-container {
            display: flex;
            flex-direction: column;
            gap: 15px;
        }
        .toggle-item {
            display: flex;
            align-items: center;
            gap: 15px;
            padding: 15px;
            border: 1px solid rgba(255, 255, 255, 0.1);
            border-radius: 8px;
            background: rgba(255, 255, 255, 0.02);
        }
        .toggle-item input[type="checkbox"] {
            width: 20px;
            height: 20px;
            accent-color: #4ecdc4;
            cursor: pointer;
        }
        .toggle-label {
            font-weight: 500;
            color: #ffffff;
            cursor: pointer;
            user-select: none;
        }
        .help-text { 
            font-size: 13px; 
            color: #a0a0a0; 
            margin-top: 8px; 
            line-height: 1.4;
        }
        button { 
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%); 
            color: #ffffff; 
            padding: 14px 28px; 
            border: none; 
            border-radius: 8px; 
            cursor: pointer; 
            font-size: 16px; 
            font-weight: 500;
            margin: 10px 5px; 
            transition: all 0.3s;
        }
        button:hover { 
            transform: translateY(-2px);
            box-shadow: 0 8px 25px rgba(102, 126, 234, 0.4);
        }
        .button-container {
            text-align: center;
            margin-top: 40px;
            padding-top: 30px;
            border-top: 1px solid #404040;
        }
        .status { 
            padding: 15px; 
            border-radius: 8px; 
            margin-bottom: 30px; 
            margin-top: 10px;
            border-left: 4px solid #ff1493;
            background: rgba(255, 20, 147, 0.05);
            color: #ffffff;
            border: 1px solid rgba(255, 20, 147, 0.2);
        }
    </style>
</head>
<body>
    <div class="ascii-background">)html" + getASCIIArt() + R"html(</div>
    <div class="container">
        <h1>OUI-SPY FOXHUNT</h1>
        
            <div class="status">
            Enter the target MAC address for foxhunt tracking. Beep speed indicates proximity: LIGHTNING FAST when close, PAINFULLY SLOW when far.
        </div>
        
        <form method="POST" action="/save">
            <div class="section">
                <h3>Target MAC Address</h3>
                <textarea name="targetMAC" placeholder="Enter target MAC address:
)html" + randomMAC + R"html(">)html" + targetMAC + R"html(</textarea>
                <div class="help-text">
                    Single MAC address for directional antenna tracking.<br>
                    Format: XX:XX:XX:XX:XX:XX (17 characters with colons)<br>
                    Beep intervals: 50ms (LIGHTNING) to 10s (PAINFULLY SLOW)
                </div>
            </div>
            
            <div class="section">
                <h3>Audio & Visual Settings</h3>
                <div class="toggle-container">
                    <div class="toggle-item">
                        <input type="checkbox" id="buzzerEnabled" name="buzzerEnabled" )html" + String(buzzerEnabled ? "checked" : "") + R"html(>
                        <label class="toggle-label" for="buzzerEnabled">Enable Buzzer</label>
                        <div class="help-text" style="margin-top: 0;">Audio feedback for target proximity</div>
                    </div>
                    <div class="toggle-item">
                        <input type="checkbox" id="ledEnabled" name="ledEnabled" )html" + String(ledEnabled ? "checked" : "") + R"html(>
                        <label class="toggle-label" for="ledEnabled">Enable LED Blinking</label>
                        <div class="help-text" style="margin-top: 0;">Orange LED blinks with same cadence as buzzer</div>
                    </div>
                </div>
            </div>
            
            <div class="button-container">
                <button type="submit">Save Configuration & Start Scanning</button>
                <button type="button" onclick="clearConfig()" style="background: #8b0000; margin-left: 20px;">Clear All Filters</button>
                <button type="button" onclick="deviceReset()" style="background: #4a0000; margin-left: 20px; font-size: 12px;">Device Reset</button>
            </div>
            
            <script>
            function clearConfig() {
                if (confirm('Are you sure you want to clear the target MAC? This action cannot be undone.')) {
                    document.querySelector('textarea[name="targetMAC"]').value = '';
                    fetch('/clear', { method: 'POST' })
                        .then(response => response.text())
                        .then(data => {
                            alert('Target MAC cleared!');
                            location.reload();
                        })
                        .catch(error => {
                            console.error('Error:', error);
                            alert('Error clearing target. Check console.');
                        });
                }
            }
            
            function deviceReset() {
                if (confirm('DEVICE RESET: This will completely wipe all saved data and restart the device. Are you absolutely sure?')) {
                    if (confirm('This action cannot be undone. The device will restart and behave like first boot. Continue?')) {
                        fetch('/device-reset', { method: 'POST' })
                            .then(response => response.text())
                            .then(data => {
                                alert('Device reset initiated! Device restarting...');
                                setTimeout(function() {
                                    window.location.href = '/';
                                }, 5000);
                            })
                            .catch(error => {
                                console.error('Error:', error);
                                alert('Error during device reset. Check console.');
                            });
                    }
            }
        }
    </script>
        </form>
    </div>
</body>
</html>
)html";
    
    return html;
}

// Web server handlers
void startConfigMode() {
    currentMode = CONFIG_MODE;
    Serial.println("\n=== STARTING FOXHUNT CONFIG MODE ===");
    Serial.println("SSID: " + String(AP_SSID));
    Serial.println("Password: " + String(AP_PASSWORD));
    Serial.println("Initializing WiFi AP...");
    
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASSWORD);
    delay(2000); // Allow AP to fully initialize
    
    // Set timing AFTER AP initialization
    configStartTime = millis();
    lastConfigActivity = millis();
    
    Serial.println("✓ Access Point created successfully!");
    Serial.println("AP IP address: " + WiFi.softAPIP().toString());
    Serial.println("Config portal: http://" + WiFi.softAPIP().toString());

    // Captive portal DNS - redirect all DNS queries to our AP IP
    foxhunterDNS.start(53, "*", WiFi.softAPIP());
    Serial.println("Captive portal DNS started");
    Serial.println("==============================\n");
    
    // Web server routes
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
        lastConfigActivity = millis();
        request->send(200, "text/html", generateConfigHTML());
    });
    
    server.on("/save", HTTP_POST, [](AsyncWebServerRequest *request){
        lastConfigActivity = millis();
        
        if (request->hasParam("targetMAC", true)) {
            targetMAC = request->getParam("targetMAC", true)->value();
            targetMAC.trim();
            targetMAC.toUpperCase(); // Ensure consistent case for comparison
            
            // Process buzzer and LED toggles
            buzzerEnabled = request->hasParam("buzzerEnabled", true);
            ledEnabled = request->hasParam("ledEnabled", true);
            
            Serial.println("Received target MAC: " + targetMAC);
            Serial.println("Buzzer enabled: " + String(buzzerEnabled ? "Yes" : "No"));
            Serial.println("LED enabled: " + String(ledEnabled ? "Yes" : "No"));
            saveConfiguration();
            
            String responseHTML = R"html(
<!DOCTYPE html>
<html>
<head>
    <title>Configuration Saved</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        body { 
            font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; 
            margin: 0; 
            padding: 20px;
            background: #1a1a1a; 
            color: #e0e0e0;
            text-align: center; 
        }
        .container { 
            max-width: 600px; 
            margin: 0 auto; 
            background: #2d2d2d; 
            padding: 40px; 
            border-radius: 12px; 
            box-shadow: 0 4px 20px rgba(0,0,0,0.3); 
        }
        h1 { 
            color: #ffffff; 
            margin-bottom: 30px; 
            font-weight: 300;
        }
        .success { 
            background: #1a4a3a; 
            color: #4ade80; 
            border: 1px solid #166534; 
            padding: 20px; 
            border-radius: 8px; 
            margin: 30px 0; 
        }
        p { 
            line-height: 1.6; 
            margin: 15px 0;
        }
    </style>
    <script>
        setTimeout(function() {
            document.getElementById('countdown').innerHTML = 'Switching to tracking mode now...';
        }, 5000);
    </script>
</head>
<body>
    <div class="container">
        <h1>Configuration Saved</h1>
        <div class="success">
            <p><strong>Target MAC configured successfully!</strong></p>
            <p id="countdown">Switching to tracking mode in 5 seconds...</p>
        </div>
        <p>The device will now start tracking your target device.</p>
        <p>When the target is found, you'll hear proximity beeps!</p>
    </div>
</body>
</html>
)html";
            
            request->send(200, "text/html", responseHTML);
            
            // Schedule mode switch for 5 seconds from now
            modeSwitchScheduled = millis() + 5000;
            
            Serial.println("Mode switch scheduled for 5 seconds from now");
            Serial.println("==============================\n");
        } else {
            request->send(400, "text/plain", "Missing target MAC");
        }
    });
    
    server.on("/clear", HTTP_POST, [](AsyncWebServerRequest *request){
        lastConfigActivity = millis();
        
        targetMAC = "";
        saveConfiguration();
        Serial.println("Target MAC cleared");
        
        request->send(200, "text/plain", "Target cleared");
    });
    
    server.on("/device-reset", HTTP_POST, [](AsyncWebServerRequest *request){
        lastConfigActivity = millis();
        request->send(200, "text/plain", "Device reset initiated");
        
        // Schedule device reset (non-blocking)
        deviceResetScheduled = millis() + 1000; // 1 second delay
    });
    
    // Captive portal catch-all: redirect any unknown URL to root
    server.onNotFound([](AsyncWebServerRequest *request) {
        request->redirect("http://192.168.4.1/");
    });

    server.begin();
    Serial.println("Web server started!");
}

// BLE callback for device detection
class MyAdvertisedDeviceCallbacks: public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice* advertisedDevice) override {
        if (currentMode != TRACKING_MODE) return;
        
        String deviceMAC = advertisedDevice->getAddress().toString().c_str();
        deviceMAC.toUpperCase();
        
        // Check if this is our target
        if (deviceMAC == targetMAC) {
            currentRSSI = advertisedDevice->getRSSI();
            lastTargetSeen = millis();
            
            // Set flags for main loop to handle
            targetDetected = true;
            newTargetDetected = true;
            Serial.print("DEBUG: Target detected, RSSI: ");
            Serial.println(currentRSSI);
        }
    }
};

void startTrackingMode() {
    if (targetMAC.length() == 0) {
        Serial.println("No target MAC configured, staying in config mode");
        return;
    }
    
    currentMode = TRACKING_MODE;
    
    // Reset session detection flag for new hunting session
    sessionFirstDetection = true;
    firstDetection = true;
    
    // Stop the web server and captive portal DNS
    foxhunterDNS.stop();
    server.end();
    
    Serial.println("\n==============================");
    Serial.println("=== STARTING FOXHUNT TRACKING MODE ===");
    Serial.print("Target MAC: ");
    Serial.println(targetMAC);
    Serial.println("==============================\n");
    
    // Initialize BLE
    NimBLEDevice::init("");
    NimBLEDevice::setPower(9);
    
    pBLEScan = NimBLEDevice::getScan();
    pBLEScan->setScanCallbacks(new MyAdvertisedDeviceCallbacks());
    pBLEScan->setInterval(16);    // 16ms intervals (maximum speed)
    pBLEScan->setWindow(15);      // 15ms scan window (95% duty cycle)
    pBLEScan->setActiveScan(true);
    pBLEScan->setDuplicateFilter(false);
    
    // Start continuous scanning
    pBLEScan->start(0);
    
    Serial.println("FOXHUNT REALTIME tracking started!");
    
    // Play startup ready signal
    ascendingBeeps();
}

void setup() {
    Serial.begin(115200);
    Serial.println("\n=== OUI-SPY FOXHUNT MODE ===");
    #if defined(ARDUINO_XIAO_ESP32C5)
    Serial.println("Hardware: XIAO ESP32-C5");
    Serial.printf("Buzzer: GPIO%d\n", BUZZER_PIN);
    #else
    Serial.println("Hardware: XIAO ESP32-S3");
    Serial.printf("Buzzer: GPIO%d\n", BUZZER_PIN);
    #endif
    Serial.println("Target: Single MAC address");
    Serial.println("Mode: REALTIME RSSI-based proximity beeping");
    Serial.println("Range: 5s (WEAK) to 100ms (STRONG)");
    Serial.println("Initializing...\n");
    
    // Setup buzzer - initialize to 1kHz for proximity beeps
    ledcAttach(BUZZER_PIN, 1000, 8);  // 1kHz default frequency
    
    // Setup LED
    pinMode(LED_PIN, OUTPUT);
    #if LED_INVERTED
    digitalWrite(LED_PIN, HIGH);  // OFF (inverted)
    #else
    digitalWrite(LED_PIN, LOW);   // OFF (normal)
    #endif
    
    zeldaSecretBeep(); // Zelda secret discovery jingle on boot
    
    // STEALTH MODE: Full MAC randomization
    uint8_t newMAC[6];
    WiFi.macAddress(newMAC);
    
    Serial.print("Original MAC: ");
    for (int i = 0; i < 6; i++) {
        Serial.printf("%02x", newMAC[i]);
        if (i < 5) Serial.print(":");
    }
    Serial.println();
    
    // STEALTH MODE: Randomize ALL 6 bytes for maximum anonymity
    randomSeed(analogRead(0) + micros());
    for (int i = 0; i < 6; i++) {
        newMAC[i] = random(0, 256);
    }
    // Ensure it's a valid locally administered address
    newMAC[0] |= 0x02; // Set locally administered bit
    newMAC[0] &= 0xFE; // Clear multicast bit
    
    // Set the randomized MAC for both STA and AP modes
    esp_wifi_set_mac(WIFI_IF_STA, newMAC);
    esp_wifi_set_mac(WIFI_IF_AP, newMAC);
    
    Serial.print("Randomized MAC: ");
    for (int i = 0; i < 6; i++) {
        Serial.printf("%02x", newMAC[i]);
        if (i < 5) Serial.print(":");
    }
    Serial.println();
    
    // Load configuration
    loadConfiguration();
    
    // Start in configuration mode
    startConfigMode();
}

void loop() {
    unsigned long currentTime = millis();
    
    // Handle scheduled mode switch
    if (modeSwitchScheduled > 0 && currentTime >= modeSwitchScheduled) {
        modeSwitchScheduled = 0;
        startTrackingMode();
        return;
    }
    
    // Handle scheduled device reset
    if (deviceResetScheduled > 0 && currentTime >= deviceResetScheduled) {
        deviceResetScheduled = 0;
        Serial.println("Device reset triggered");
        
        // Clear NVS and restart
        preferences.begin("tracker", false);
        preferences.clear();
        preferences.end();
        
        delay(1000);
        ESP.restart();
        return;
    }
    
    if (currentMode == CONFIG_MODE) {
        foxhunterDNS.processNextRequest();  // Captive portal DNS
        // Check for config timeout only if no recent activity AND no connected clients
        int connectedClients = WiFi.softAPgetStationNum();
        if (currentTime - lastConfigActivity > CONFIG_TIMEOUT && connectedClients == 0) {
            Serial.println("Configuration timeout - switching to tracking mode with saved config");
            startTrackingMode();
        }
    } 
    else if (currentMode == TRACKING_MODE) {
        unsigned long currentTime = millis();
        
        // Handle target detection messages (safe serial output)
        if (newTargetDetected) {
            newTargetDetected = false;
            
            // Only play three same-tone beeps on FIRST detection of hunting session
            if (sessionFirstDetection) {
                threeSameToneBeeps();
                sessionFirstDetection = false;
                Serial.println("TARGET ACQUIRED!");
            } else if (firstDetection) {
                // Silent reacquisition after loss
                firstDetection = false;
                Serial.println("TARGET REACQUIRED!");
            }
        }
        
        // Handle proximity beeping
        if (targetDetected && (currentTime - lastTargetSeen < 5000)) { // Target seen within last 5 seconds
            handleProximityBeeping();
            
            // Print RSSI for visual fox hunting feedback (reduced frequency for real-time performance)
            static unsigned long lastRSSIPrint = 0;
            int printInterval = 2000; // Fixed 2-second intervals - less serial spam
            
            if (currentTime - lastRSSIPrint >= printInterval) {
                Serial.print("RSSI: ");
                Serial.print(currentRSSI);
                Serial.println(" dBm");
                lastRSSIPrint = currentTime;
            }
        } else if (currentTime - lastTargetSeen >= 5000) {
            // Target lost - INSTANT LED OFF for maximum reactivity
            targetDetected = false;
            firstDetection = true; // Reset for next detection
            
            // Turn off beep and LED immediately
            if (buzzerEnabled) {
                ledcWrite(BUZZER_PIN, 0);
            }
            ledOff();
            isBeeping = false;
            
            Serial.println("TARGET LOST - Searching...");
        }
        
        return;
    }
} 
