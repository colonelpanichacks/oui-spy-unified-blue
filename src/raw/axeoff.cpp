/*
 * AXEOFF — headless Axon detector/tracker
 * ======================================
 * Dedicated detector for Axon surveillance gear (body cameras, Taser 7/10,
 * Signal sidearm, Axon Fleet). Five hardcoded signatures, zero configuration.
 *
 * There is NO user interface: no WiFi AP, no web server, no NVS config, no
 * MQTT, no LED. Power on -> boot jingle -> already scanning -> beep the
 * instant an Axon device is heard. Serial output at 115200 is debug only.
 *
 * The entire UX is four sounds on the GPIO3 buzzer:
 *   1. BOOT       4 ascending beeps             "awake and scanning"
 *   2. DETECTION  fast 8-bit ascending arpeggio  an Axon device just appeared
 *   3. HEARTBEAT  2-beep "lub-dub"               still present; beep rate
 *                                                encodes proximity
 *                                                (slow ~2 s = far, ~250 ms = close)
 *   4. LOST       descending 2-beep              gone quiet, back to IDLE
 *
 * The five hardcoded Axon signatures:
 *   1. WiFi probe request from OUI 00:25:DF  IEEE registry: "Axon Enterprise"
 *   2. WiFi beacon from OUI 00:25:DF         Axon body cam / Fleet in AP mode
 *   3. BT SIG manufacturer CID 0x034D        "TASER International" = Axon
 *   4. 16-bit service UUIDs 0xFC81 /         BT SIG member registry, all
 *      0xFE6B / 0xFE6C                        owned by Axon/TASER
 *   5. BLE MAC prefix 00:25:DF               same IEEE OUI on the BLE interface
 *
 * The detection core (promiscuous RX path, BLE matchers, time-sliced radio
 * scheduler) is ported from ouispy-detector (colonelpanichacks/ouispy-detector),
 * proven on this exact XIAO ESP32-S3 hardware.
 *
 * Audio is a non-blocking note sequencer serviced from loop(); BLE/WiFi
 * callbacks only enqueue detections — they never beep, log, or delay.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <NimBLEDevice.h>
#include <NimBLEScan.h>
#include <NimBLEAdvertisedDevice.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_wifi.h>

// ================================
// Axon signatures (hardcoded)
// ================================
#define AXON_CID 0x034D  // BT SIG company ID "TASER International" (= Axon), little-endian on air
static const uint8_t AXON_OUI[3] = { 0x00, 0x25, 0xDF };  // IEEE OUI "Axon Enterprise, Inc."

// 16-bit service UUIDs owned by Axon/TASER per the BT SIG member registry.
static const uint16_t AXON_SVCS[] = { 0xFC81, 0xFE6B, 0xFE6C };
#define AXON_SVC_COUNT (sizeof(AXON_SVCS) / sizeof(AXON_SVCS[0]))

// ================================
// Tuning knobs
// ================================
#define BUZZER_PIN             3      // GPIO3 (D2) on the XIAO ESP32-S3
#define LEDC_RES_BITS          8

#define LOST_TIMEOUT_MS        8000   // silence this long before "lost" (also the freshness window)
#define TRACK_TABLE_SIZE       8      // distinct Axon MACs tracked
#define RELOG_SUPPRESS_MS      30000  // per (MAC, signature) serial-log re-alert suppression

#define RSSI_FAR_DB            (-90)  // heartbeat map bounds...
#define RSSI_NEAR_DB           (-40)
#define HEARTBEAT_PERIOD_FAR_MS   2000  // ...weak/far target: lub-dub every 2 s
#define HEARTBEAT_PERIOD_NEAR_MS  250   // ...strong/close target: lub-dub every 250 ms

#define WIFI_SWEEP_MS          2200   // promiscuous sweep duration (11 -> 1 @ 200 ms dwell)
#define BLE_SCAN_MS            2200   // BLE phase duration (2 s NimBLE window + slack)
#define BLE_SCAN_SECONDS       2
#define WIFI_HOP_MS            200

#define DET_QUEUE_SIZE         16     // ISR/host-callback -> loop detection ring

// ================================
// 802.11 MAC header (not in public Arduino headers; same layout the
// ESP-IDF promiscuous path hands us — taken from the reference project)
// ================================
typedef struct __attribute__((packed)) {
    uint16_t frame_ctrl;
    uint16_t duration;
    uint8_t  addr1[6];
    uint8_t  addr2[6];
    uint8_t  addr3[6];
    uint16_t seq_ctrl;
} wifi_ieee80211_mac_hdr_t;

// ================================
// Detection report (queue payload)
// ================================
enum DetType : uint8_t {
    DET_BLE_CID = 0,   // manufacturer CID 0x034D
    DET_BLE_SVC,       // service UUID 0xFC81 / 0xFE6B / 0xFE6C
    DET_BLE_MAC,       // BLE MAC prefix 00:25:DF
    DET_WIFI_PROBE,    // 802.11 probe request from OUI 00:25:DF
    DET_WIFI_BEACON,   // 802.11 beacon from OUI 00:25:DF
    DET_TYPE_COUNT
};

struct DetEntry {
    char    mac[18];    // "aa:bb:cc:dd:ee:ff" (lowercase — NimBLE and WiFi paths format identically)
    DetType type;
    int8_t  rssi;
    uint16_t matchId;   // matched 16-bit service UUID when type == DET_BLE_SVC, else 0
};

static const char* detTypeCode(DetType t) {
    switch (t) {
        case DET_BLE_CID:      return "BLE_CID";
        case DET_BLE_SVC:      return "BLE_SVC";
        case DET_BLE_MAC:      return "BLE_MAC";
        case DET_WIFI_PROBE:   return "WIFI_PROBE";
        case DET_WIFI_BEACON:  return "WIFI_BEACON";
        default:               return "???";
    }
}

static const char* detMatchCode(DetType t) {
    switch (t) {
        case DET_BLE_CID:      return "034D";
        case DET_WIFI_PROBE:   return "0025DF";
        case DET_WIFI_BEACON:  return "0025DF";
        default:               return "??";  // DET_BLE_SVC is logged dynamically (which UUID hit)
    }
}

static const char* detHumanNote(DetType t) {
    switch (t) {
        case DET_BLE_CID:      return "Axon signature (TASER CID)";
        case DET_BLE_SVC:      return "Axon signature (svc UUID)";  // log appends the UUID
        case DET_BLE_MAC:      return "Axon signature (OUI 00:25:DF)";
        case DET_WIFI_PROBE:   return "Axon body cam (WiFi probe)";
        case DET_WIFI_BEACON:  return "Axon AP beacon (WiFi)";
        default:               return "Axon signature";
    }
}

// ================================
// Detection queue (ISR/host-callback producers, loop consumer)
// ================================
static DetEntry detQueue[DET_QUEUE_SIZE];
static portMUX_TYPE detQueueMux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint8_t detQueueHead = 0;
static volatile uint8_t detQueueTail = 0;
static volatile uint8_t detQueueCount = 0;

// Fixed stack buffers only; short critical section (two producers on
// different tasks: the NimBLE host task and the WiFi driver task).
static void detEnqueue(const char* mac, DetType type, int8_t rssi, uint16_t matchId) {
    DetEntry e;
    strncpy(e.mac, mac, sizeof(e.mac) - 1);
    e.mac[sizeof(e.mac) - 1] = '\0';
    e.type = type;
    e.rssi = rssi;
    e.matchId = matchId;

    portENTER_CRITICAL(&detQueueMux);
    if (detQueueCount < DET_QUEUE_SIZE) {
        detQueue[detQueueHead] = e;
        detQueueHead = (detQueueHead + 1) % DET_QUEUE_SIZE;
        detQueueCount++;
    }
    portEXIT_CRITICAL(&detQueueMux);
}

static bool detDequeue(DetEntry* out) {
    bool got = false;
    portENTER_CRITICAL(&detQueueMux);
    if (detQueueCount > 0) {
        *out = detQueue[detQueueTail];
        detQueueTail = (detQueueTail + 1) % DET_QUEUE_SIZE;
        detQueueCount--;
        got = true;
    }
    portEXIT_CRITICAL(&detQueueMux);
    return got;
}

// ================================
// Audio: non-blocking note sequencer (loop-context only)
// ================================
struct Note {
    uint16_t freq_hz;      // square wave; 0 = silence
    uint16_t duration_ms;  // how long it sounds
    uint16_t gap_after_ms; // silence before the next note
};

// 1. BOOT — 4 ascending beeps, short and bright: G4 C5 E5 G5
static const Note JINGLE_BOOT[] = {
    { 392, 70, 40 }, { 523, 70, 40 }, { 659, 70, 40 }, { 784, 85, 60 },
};
#define JINGLE_BOOT_LEN (sizeof(JINGLE_BOOT) / sizeof(JINGLE_BOOT[0]))

// 2. DETECTION — 8-bit ascending arpeggio: C5 E5 G5 C6 E6
static const Note JINGLE_DETECT[] = {
    { 523, 65, 30 }, { 659, 65, 30 }, { 784, 65, 30 }, { 1047, 65, 30 }, { 1319, 75, 60 },
};
#define JINGLE_DETECT_LEN (sizeof(JINGLE_DETECT) / sizeof(JINGLE_DETECT[0]))
#define JINGLE_DETECT_MS  515  // total length, used to delay the first heartbeat

// 3. HEARTBEAT — lub-dub: high/short then low/long (C5 then G4)
static const Note JINGLE_HEARTBEAT[] = {
    { 523, 60, 40 }, { 392, 110, 30 },
};
#define JINGLE_HEARTBEAT_LEN (sizeof(JINGLE_HEARTBEAT) / sizeof(JINGLE_HEARTBEAT[0]))

// 4. LOST — descending 2-beep, slightly longer: E5 then E4
static const Note JINGLE_LOST[] = {
    { 659, 150, 80 }, { 330, 260, 40 },
};
#define JINGLE_LOST_LEN (sizeof(JINGLE_LOST) / sizeof(JINGLE_LOST[0]))

#define AUDIO_QUEUE_LEN 16

static Note audioQueue[AUDIO_QUEUE_LEN];
static uint8_t audioQueueHead = 0;
static uint8_t audioQueueCount = 0;
static Note audioCur;                 // note currently sounding (or its gap)
static bool audioHasCur = false;
static bool audioSounding = false;
static unsigned long audioPhaseEnd = 0;

// LEDC tone API is channel-based on Arduino core 2.x (the reference's
// proven toolchain) and pin-based on core 3.x — support both.
#if defined(ESP_ARDUINO_VERSION) && (ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0))
#define AXEOFF_CORE_3_PLUS 1
#endif

#ifdef AXEOFF_CORE_3_PLUS
#define BUZZER_LEDC_TARGET   BUZZER_PIN
#else
#define BUZZER_LEDC_CH       0
#define BUZZER_LEDC_TARGET   BUZZER_LEDC_CH
#endif

static void audioInit() {
    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW);
#ifdef AXEOFF_CORE_3_PLUS
    ledcAttach(BUZZER_PIN, 2000, LEDC_RES_BITS);
#else
    ledcSetup(BUZZER_LEDC_CH, 2000, LEDC_RES_BITS);
    ledcAttachPin(BUZZER_PIN, BUZZER_LEDC_CH);
#endif
    ledcWriteTone(BUZZER_LEDC_TARGET, 0);
}

static void audioToneStart(uint16_t freqHz) {
    if (freqHz > 0) {
        ledcWriteTone(BUZZER_LEDC_TARGET, freqHz);
    }
}

static void audioToneStop() {
    ledcWriteTone(BUZZER_LEDC_TARGET, 0);
}

// A new jingle cancels anything playing.
static void audioPlay(const Note* notes, uint8_t count) {
    audioQueueHead = 0;
    audioQueueCount = 0;
    audioHasCur = false;
    audioSounding = false;
    audioToneStop();

    uint8_t n = (count > AUDIO_QUEUE_LEN) ? AUDIO_QUEUE_LEN : count;
    for (uint8_t i = 0; i < n; i++) {
        audioQueue[audioQueueHead] = notes[i];
        audioQueueHead = (audioQueueHead + 1) % AUDIO_QUEUE_LEN;
    }
    audioQueueCount = n;
}

// Serviced from loop() only — never from BLE/WiFi callback context.
static void audioTick(unsigned long now) {
    if (!audioHasCur) {
        if (audioQueueCount == 0) return;
        uint8_t tail = (audioQueueHead + AUDIO_QUEUE_LEN - audioQueueCount) % AUDIO_QUEUE_LEN;
        audioCur = audioQueue[tail];
        audioQueueCount--;
        audioHasCur = true;
        audioSounding = (audioCur.freq_hz > 0);
        audioToneStart(audioCur.freq_hz);
        audioPhaseEnd = now + audioCur.duration_ms;
        return;
    }

    if (now < audioPhaseEnd) return;

    if (audioSounding) {
        audioToneStop();
        audioSounding = false;
        audioPhaseEnd = now + audioCur.gap_after_ms;
        return;
    }

    audioHasCur = false;  // next tick picks up the following note
}

// ================================
// Target tracking table (loop context only)
// ================================
struct TargetRow {
    bool        used;
    char        mac[18];
    unsigned long lastSeenMs;
    int8_t      lastRssi;
    int8_t      bestRssi;
    DetType     src;
    unsigned long lastLogAt[DET_TYPE_COUNT];  // per (MAC, signature) log suppression
};

static TargetRow g_targets[TRACK_TABLE_SIZE];

// Insert-or-update. Presence (lastSeen) updates on EVERY matching frame; the
// caller decides separately whether to re-log (suppressed for RELOG_SUPPRESS_MS
// per MAC+signature). Evicts the least-recently-seen row when full.
static TargetRow* trackingUpdate(const char* mac, int8_t rssi, DetType type, unsigned long now) {
    TargetRow* row = nullptr;

    for (int i = 0; i < TRACK_TABLE_SIZE; i++) {
        if (g_targets[i].used && strcmp(g_targets[i].mac, mac) == 0) {
            row = &g_targets[i];
            break;
        }
    }

    if (!row) {
        int slot = -1;
        for (int i = 0; i < TRACK_TABLE_SIZE; i++) {
            if (!g_targets[i].used) { slot = i; break; }
        }
        if (slot < 0) {
            unsigned long oldest = ULONG_MAX;
            for (int i = 0; i < TRACK_TABLE_SIZE; i++) {
                if (g_targets[i].lastSeenMs < oldest) {
                    oldest = g_targets[i].lastSeenMs;
                    slot = i;
                }
            }
        }
        row = &g_targets[slot];
        memset(row, 0, sizeof(*row));
        row->used = true;
        strncpy(row->mac, mac, sizeof(row->mac) - 1);
        row->mac[sizeof(row->mac) - 1] = '\0';
    }

    row->lastSeenMs = now;
    row->lastRssi = rssi;
    if (rssi > row->bestRssi) row->bestRssi = rssi;
    row->src = type;
    return row;
}

// Freshest row still within the lost timeout, or nullptr.
static TargetRow* trackingFreshest(unsigned long now) {
    TargetRow* best = nullptr;
    for (int i = 0; i < TRACK_TABLE_SIZE; i++) {
        if (!g_targets[i].used) continue;
        if (now - g_targets[i].lastSeenMs > LOST_TIMEOUT_MS) continue;
        if (!best || g_targets[i].lastSeenMs > best->lastSeenMs) best = &g_targets[i];
    }
    return best;
}

// ================================
// Tracking state machine
// ================================
enum TrackState : uint8_t { STATE_IDLE, STATE_TRACKING };

static TrackState g_state = STATE_IDLE;
static unsigned long g_heartbeatAt = 0;

static void stateMachineTick(unsigned long now) {
    if (g_state != STATE_TRACKING) return;

    TargetRow* fresh = trackingFreshest(now);
    if (!fresh) {
        Serial.println("[state] TRACKING -> IDLE (lost)");
        audioPlay(JINGLE_LOST, JINGLE_LOST_LEN);
        g_state = STATE_IDLE;
        return;
    }

    if ((long)(now - g_heartbeatAt) >= 0) {
        // Heartbeat rate encodes proximity: weakest fresh signal -> 2 s,
        // strongest -> 250 ms, recomputed from the freshest RSSI every beat.
        long period = map(constrain((int)fresh->lastRssi, RSSI_FAR_DB, RSSI_NEAR_DB),
                          RSSI_FAR_DB, RSSI_NEAR_DB,
                          HEARTBEAT_PERIOD_FAR_MS, HEARTBEAT_PERIOD_NEAR_MS);
        audioPlay(JINGLE_HEARTBEAT, JINGLE_HEARTBEAT_LEN);
        g_heartbeatAt = now + (unsigned long)period;
    }
}

// ================================
// WiFi promiscuous detection (driver context — no Serial, no heap, no audio)
// ================================
static void promiscuousCallback(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT) return;

    wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
    if (pkt->rx_ctrl.sig_len < sizeof(wifi_ieee80211_mac_hdr_t)) return;

    wifi_ieee80211_mac_hdr_t* hdr = (wifi_ieee80211_mac_hdr_t*)pkt->payload;
    int8_t rssi = pkt->rx_ctrl.rssi;

    uint8_t fc0 = hdr->frame_ctrl & 0xFF;
    uint8_t ftype = (fc0 >> 2) & 0x03;
    uint8_t subtype = (fc0 >> 4) & 0x0F;

    // Management frames only; probe request (0x04) and beacon (0x08) only.
    if (ftype != 0) return;
    if (subtype != 0x04 && subtype != 0x08) return;

    // Fast path: 3-byte OUI pre-check on the source address (addr2).
    if (hdr->addr2[0] != AXON_OUI[0] ||
        hdr->addr2[1] != AXON_OUI[1] ||
        hdr->addr2[2] != AXON_OUI[2]) return;

    // Rare path: format the MAC and enqueue for loop().
    char macStr[18];
    int pos = 0;
    for (int i = 0; i < 6; i++) {
        if (i > 0) macStr[pos++] = ':';
        uint8_t hi = (hdr->addr2[i] >> 4) & 0x0F;
        uint8_t lo = hdr->addr2[i] & 0x0F;
        macStr[pos++] = hi > 9 ? 'a' + hi - 10 : '0' + hi;
        macStr[pos++] = lo > 9 ? 'a' + lo - 10 : '0' + lo;
    }
    macStr[pos] = '\0';

    detEnqueue(macStr, subtype == 0x04 ? DET_WIFI_PROBE : DET_WIFI_BEACON, rssi, 0);
}

// ================================
// BLE detection (NimBLE host-task context — match + enqueue only)
// ================================
// Table-driven: any of the three Axon-owned 16-bit service UUIDs, either as
// a bare 16-bit UUID or embedded at bytes 4-5 of a 128-bit UUID string.
static bool svcUuidIsAxon(NimBLEAdvertisedDevice* adv, uint16_t& matched) {
    for (uint8_t i = 0; i < adv->getServiceUUIDCount(); i++) {
        std::string s = adv->getServiceUUID(i).toString();
        for (size_t k = 0; k < s.length(); k++) {
            if (s[k] >= 'A' && s[k] <= 'F') s[k] += 'a' - 'A';
        }
        for (size_t u = 0; u < AXON_SVC_COUNT; u++) {
            char hex[5];
            snprintf(hex, sizeof(hex), "%04x", AXON_SVCS[u]);
            bool short16 = (s.length() == 4 && s == hex);
            bool in128   = (s.length() >= 8 && s.substr(4, 4) == hex);
            if (short16 || in128) {
                matched = AXON_SVCS[u];
                return true;
            }
        }
    }
    return false;
}

class AxonAdvertCallbacks : public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice* adv) override {
        DetType type;
        uint16_t matchId = 0;
        bool match = false;

        // 1) BT SIG manufacturer company ID 0x034D (TASER International = Axon),
        //    little-endian on air.
        if (adv->haveManufacturerData()) {
            std::string mfr = adv->getManufacturerData();
            if (mfr.length() >= 2 &&
                (uint8_t)mfr[0] == (AXON_CID & 0xFF) &&
                (uint8_t)mfr[1] == (AXON_CID >> 8)) {
                type = DET_BLE_CID;
                match = true;
            }
        }

        // 2) Any Axon-owned 16-bit service UUID (0xFC81 / 0xFE6B / 0xFE6C)
        //    in the advert.
        if (!match && adv->getServiceUUIDCount() > 0 && svcUuidIsAxon(adv, matchId)) {
            type = DET_BLE_SVC;
            match = true;
        }

        // 3) BLE MAC prefix 00:25:DF.
        std::string mac = adv->getAddress();
        if (!match && mac.compare(0, 8, "00:25:df") == 0) {
            type = DET_BLE_MAC;
            match = true;
        }

        if (match) {
            detEnqueue(mac.c_str(), type, (int8_t)adv->getRSSI(), matchId);
        }
    }
};

// ================================
// Time-sliced radio scheduler (BLE first so the first BLE hit lands fast)
// ================================
enum RadioPhase : uint8_t { PHASE_BLE, PHASE_WIFI };

static NimBLEScan* pBLEScan = nullptr;
static RadioPhase g_phase = PHASE_BLE;
static unsigned long g_phaseStartMs = 0;
static unsigned long g_lastHopMs = 0;
static uint8_t g_wifiChannel = 11;
static bool g_promiscArmed = false;

static void radioPromiscArm() {
    if (g_promiscArmed) return;
    wifi_promiscuous_filter_t filt = { .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT };
    esp_wifi_set_promiscuous_filter(&filt);
    esp_wifi_set_promiscuous_rx_cb(promiscuousCallback);
    g_promiscArmed = true;
}

static void radioToBleScan() {
    if (g_phase == PHASE_BLE) return;
    esp_wifi_set_promiscuous(false);
    pBLEScan->start(BLE_SCAN_SECONDS, nullptr, false);
    g_phase = PHASE_BLE;
}

static void radioToWifiSweep() {
    if (g_phase == PHASE_WIFI) return;
    if (pBLEScan->isScanning()) pBLEScan->stop();  // normally already expired at 2 s
    radioPromiscArm();
    esp_wifi_set_promiscuous(true);
    g_phase = PHASE_WIFI;
    g_wifiChannel = 11;
    esp_wifi_set_channel(g_wifiChannel, WIFI_SECOND_CHAN_NONE);
    g_lastHopMs = millis();
}

static void radioSchedulerTick(unsigned long now) {
    unsigned long phaseDur = (g_phase == PHASE_WIFI) ? WIFI_SWEEP_MS : BLE_SCAN_MS;
    if (now - g_phaseStartMs >= phaseDur) {
        if (g_phase == PHASE_WIFI) {
            radioToBleScan();
        } else {
            radioToWifiSweep();
        }
        g_phaseStartMs = now;
    }

    // Channel hop while sweeping: 11 -> 10 -> ... -> 1 -> 11.
    if (g_phase == PHASE_WIFI && now - g_lastHopMs >= WIFI_HOP_MS) {
        g_wifiChannel = (g_wifiChannel > 1) ? g_wifiChannel - 1 : 11;
        esp_wifi_set_channel(g_wifiChannel, WIFI_SECOND_CHAN_NONE);
        g_lastHopMs = now;
    }
}

// ================================
// Setup
// ================================
static void printMac(const char* label, const uint8_t* mac) {
    Serial.printf("%s %02x:%02x:%02x:%02x:%02x:%02x\n", label,
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void setup() {
    Serial.begin(115200);
    delay(300);

    Serial.println();
    Serial.println("=== AXEOFF — Axon detector ===");

    // MAC randomization on every boot (no MQTT here, so always on).
    // The BT DMAC follows from the same base MAC, so one esp_wifi_set_mac()
    // randomizes both radios' identities.
    uint8_t origMAC[6];
    esp_read_mac(origMAC, ESP_MAC_WIFI_STA);
    printMac("Original MAC:", origMAC);

    uint8_t newMAC[6];
    randomSeed(analogRead(0) + micros());
    for (int i = 0; i < 6; i++) {
        newMAC[i] = (uint8_t)random(0, 256);
    }
    newMAC[0] |= 0x02;  // locally administered
    newMAC[0] &= 0xFE;  // unicast
    WiFi.mode(WIFI_STA);
    esp_wifi_set_mac(WIFI_IF_STA, newMAC);
    printMac("Randomized MAC:", newMAC);
    WiFi.disconnect();  // never associate; the radio belongs to the sweeps

    esp_log_level_set("*", ESP_LOG_NONE);

    audioInit();

    // BLE before the first WiFi sweep; fully passive (never transmits a
    // SCAN_REQ) — all four BLE signatures live in ADV_IND payloads. Scan
    // results are consumed in the callback only (setMaxResults(0)).
    NimBLEDevice::init("");
    delay(100);  // one-time setup settle, before the loop starts
    pBLEScan = NimBLEDevice::getScan();
    pBLEScan->setAdvertisedDeviceCallbacks(new AxonAdvertCallbacks());
    pBLEScan->setActiveScan(false);  // passive scan type in NimBLE 1.4.x (no setScanType)
    pBLEScan->setInterval(300);
    pBLEScan->setWindow(200);
    pBLEScan->setMaxResults(0);

    // First phase after boot is BLE: first Axon advert lands within ~2 s.
    pBLEScan->start(BLE_SCAN_SECONDS, nullptr, false);
    g_phase = PHASE_BLE;
    g_phaseStartMs = millis();

    // Everything is initialized — play the boot jingle once. The sequencer
    // is serviced by audioTick() from the first loop() pass.
    audioPlay(JINGLE_BOOT, JINGLE_BOOT_LEN);

    Serial.println("[axeoff] armed — scanning for Axon signatures");
}

// ================================
// Main loop — audio, detection drain, tracking, radio time-slicing.
// No delay() anywhere on this path; vTaskDelay(1) just yields the tick.
// ================================
void loop() {
    unsigned long now = millis();

    audioTick(now);

    // Drain up to 4 queued detections per pass. Bookkeeping, logging,
    // state transitions and audio all happen here — never in callbacks.
    for (int i = 0; i < 4; i++) {
        DetEntry e;
        if (!detDequeue(&e)) break;

        TargetRow* row = trackingUpdate(e.mac, e.rssi, e.type, now);

        if (now - row->lastLogAt[e.type] >= RELOG_SUPPRESS_MS) {
            row->lastLogAt[e.type] = now;
            char matchBuf[5];
            const char* match = detMatchCode(e.type);
            char noteBuf[48];
            const char* note = detHumanNote(e.type);
            if (e.type == DET_BLE_SVC) {
                snprintf(matchBuf, sizeof(matchBuf), "%04X", e.matchId);
                match = matchBuf;
                snprintf(noteBuf, sizeof(noteBuf), "Axon signature (svc UUID %04X)", e.matchId);
                note = noteBuf;
            }
            Serial.printf("{\"mac\":\"%s\",\"rssi\":%d,\"type\":\"%s\",\"match\":\"%s\"}  %s\n",
                          e.mac, e.rssi, detTypeCode(e.type), match, note);
        }

        if (g_state == STATE_IDLE) {
            g_state = STATE_TRACKING;
            Serial.println("[state] IDLE -> TRACKING");
            audioPlay(JINGLE_DETECT, JINGLE_DETECT_LEN);
            // Let the arpeggio finish before the heartbeat takes over.
            g_heartbeatAt = now + JINGLE_DETECT_MS + 100;
        }
    }

    stateMachineTick(now);

    radioSchedulerTick(now);

    vTaskDelay(1);
}
