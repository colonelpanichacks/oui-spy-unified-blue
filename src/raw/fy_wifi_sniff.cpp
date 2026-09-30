// WiFi promiscuous Flock detector — ported from colonelpanichacks/flock-you.
// Included from mode_flockyou.cpp (anonymous namespace). Alert processing
// happens in flockyou.cpp via fyWiFiSniffPopAlert().

#include <Arduino.h>
#include <WiFi.h>
#include "esp_wifi.h"
#include <ctype.h>
#include <string.h>
#include <stdio.h>

#define FYWS_RSSI_MIN           -95
#define FYWS_CHANNEL_DWELL_MS   150   // fits 1/6/11 in 500 ms collect WiFi slice
#define FYWS_ALERT_QUEUE_SIZE   32

#define FYWS_CHECK_ADDR1        1
#define FYWS_CHECK_ADDR3        0
#define FYWS_ENABLE_SSID_MATCH  0
#define FYWS_PROCESS_MGMT       1
#define FYWS_PROCESS_DATA       1

static const uint8_t fywsCustomChannels[] = {1, 6, 11};
static const size_t fywsCustomChannelCount =
    sizeof(fywsCustomChannels) / sizeof(fywsCustomChannels[0]);

// WiFi OUI list (@NitekryDPaul promiscuous set + DeFlockJoplin 82:6b:f2).
// Excludes April 2026 BLE-only additions (e.g. f0:82:c0).
static const char* fywsTargetOuis[] = {
    "70:c9:4e", "3c:91:80", "d8:f3:bc", "80:30:49", "b8:35:32",
    "14:5a:fc", "74:4c:a1", "08:3a:88", "9c:2f:9d", "c0:35:32",
    "94:08:53", "e4:aa:ea", "f4:6a:dd", "24:b2:b9", "00:f4:8d",
    "d0:39:57", "e8:d0:fc", "e0:4f:43", "b8:1e:a4", "70:08:94",
    "58:8e:81", "ec:1b:bd", "3c:71:bf", "58:00:e3", "90:35:ea",
    "5c:93:a2", "64:6e:69", "48:27:ea", "a4:cf:12",
    "82:6b:f2",
};
static const size_t FYWS_OUI_COUNT =
    sizeof(fywsTargetOuis) / sizeof(fywsTargetOuis[0]);

static uint8_t fywsOuiBytes[FYWS_OUI_COUNT][3];

enum FyWiFiAlertType : uint8_t {
    FYWS_ALERT_OUI_ADDR2 = 0,
    FYWS_ALERT_OUI_ADDR1 = 1,
    FYWS_ALERT_OUI_ADDR3 = 2,
    FYWS_ALERT_WILDCARD_PROBE = 4,
};

struct FyWiFiAlert {
    FyWiFiAlertType type;
    uint8_t mac[6];
    int8_t rssi;
    uint8_t channel;
    char frameKind[12];
};

typedef struct __attribute__((packed)) {
    uint16_t frame_ctrl;
    uint16_t duration;
    uint8_t addr1[6];
    uint8_t addr2[6];
    uint8_t addr3[6];
    uint16_t seq_ctrl;
} fyws_mac_hdr_t;

typedef struct {
    FyWiFiAlertType type;
    uint8_t mac[6];
    int8_t rssi;
    uint8_t channel;
    char frameKind[12];
} FyWiFiAlertEntry;

static volatile FyWiFiAlertEntry fywsAlertQueue[FYWS_ALERT_QUEUE_SIZE];
static volatile size_t fywsAlertHead = 0;
static volatile size_t fywsAlertTail = 0;
static portMUX_TYPE fywsQueueMux = portMUX_INITIALIZER_UNLOCKED;

static uint8_t fywsCurrentChannel = 1;
static size_t fywsChannelIndex = 0;
static unsigned long fywsLastHop = 0;
static bool fywsStackStarted = false;
static bool fywsPromiscActive = false;
static bool fywsOuisReady = false;
static bool fywsApCoexist = false;  // dashboard: sniff on AP channel, no STA takeover

static void IRAM_ATTR fywsEnqueueAlert(FyWiFiAlertType type, const uint8_t* mac,
                                       int8_t rssi, uint8_t ch, const char* kind) {
    portENTER_CRITICAL_ISR(&fywsQueueMux);
    size_t next = (fywsAlertHead + 1) % FYWS_ALERT_QUEUE_SIZE;
    if (next == fywsAlertTail) {
        portEXIT_CRITICAL_ISR(&fywsQueueMux);
        return;
    }
    FyWiFiAlertEntry* e = (FyWiFiAlertEntry*)&fywsAlertQueue[fywsAlertHead];
    e->type = type;
    e->rssi = rssi;
    e->channel = ch;
    memcpy((void*)e->mac, mac, 6);
    if (kind) {
        strncpy((char*)e->frameKind, kind, 11);
        ((char*)e->frameKind)[11] = '\0';
    } else {
        ((char*)e->frameKind)[0] = '\0';
    }
    fywsAlertHead = next;
    portEXIT_CRITICAL_ISR(&fywsQueueMux);
}

static inline bool IRAM_ATTR fywsIsMulticast(const uint8_t* mac) {
    return mac[0] & 0x01;
}

static bool IRAM_ATTR fywsMatchOuiRaw(const uint8_t* mac) {
    if (mac[0] & 0x02) return false;
    for (size_t i = 0; i < FYWS_OUI_COUNT; i++) {
        if (mac[0] == fywsOuiBytes[i][0] &&
            mac[1] == fywsOuiBytes[i][1] &&
            mac[2] == fywsOuiBytes[i][2]) {
            return true;
        }
    }
    return false;
}

static int IRAM_ATTR fywsIsWildcardProbeIE(const uint8_t* body, int len) {
    if (!body || len < 2) return -1;
    while (len >= 2) {
        uint8_t id = body[0];
        uint8_t elen = body[1];
        if ((int)elen + 2 > len) break;
        if (id == 0) return (elen == 0) ? 1 : 0;
        body += elen + 2;
        len -= elen + 2;
    }
    return -1;
}

static void IRAM_ATTR fywsWifiSniffer(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (!buf || !fywsPromiscActive) return;

#if FYWS_PROCESS_MGMT && FYWS_PROCESS_DATA
    if (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA) return;
#elif FYWS_PROCESS_MGMT
    if (type != WIFI_PKT_MGMT) return;
#elif FYWS_PROCESS_DATA
    if (type != WIFI_PKT_DATA) return;
#else
    return;
#endif

    wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
    if (pkt->rx_ctrl.sig_len < sizeof(fyws_mac_hdr_t)) return;
    fyws_mac_hdr_t* hdr = (fyws_mac_hdr_t*)pkt->payload;
    int8_t rssi = pkt->rx_ctrl.rssi;
    if (rssi < FYWS_RSSI_MIN) return;

    uint8_t ch = (uint8_t)pkt->rx_ctrl.channel;

    if (fywsMatchOuiRaw(hdr->addr2)) {
        bool emitted = false;
        if (type == WIFI_PKT_MGMT) {
            uint8_t fc0 = hdr->frame_ctrl & 0xFF;
            uint8_t ftype = (fc0 >> 2) & 0x03;
            uint8_t subtype = (fc0 >> 4) & 0x0F;
            if (ftype == 0 && subtype == 4) {
                int sigLen = (int)pkt->rx_ctrl.sig_len;
                int bodyLen = sigLen - (int)sizeof(fyws_mac_hdr_t);
                const uint8_t* body = pkt->payload + sizeof(fyws_mac_hdr_t);
                int r = (bodyLen > 0) ? fywsIsWildcardProbeIE(body, bodyLen) : -1;
                if (r == -1 && bodyLen > 4) {
                    r = fywsIsWildcardProbeIE(body, bodyLen - 4);
                }
                if (r == 1) {
                    fywsEnqueueAlert(FYWS_ALERT_WILDCARD_PROBE, hdr->addr2, rssi, ch,
                                     "probe_req");
                    emitted = true;
                }
            }
        }
        if (!emitted) {
            fywsEnqueueAlert(FYWS_ALERT_OUI_ADDR2, hdr->addr2, rssi, ch, "addr2");
        }
    }

#if FYWS_CHECK_ADDR1
    if (!fywsIsMulticast(hdr->addr1) && fywsMatchOuiRaw(hdr->addr1)) {
        fywsEnqueueAlert(FYWS_ALERT_OUI_ADDR1, hdr->addr1, rssi, ch, "addr1");
    }
#endif

#if FYWS_CHECK_ADDR3
    if (type == WIFI_PKT_MGMT && fywsMatchOuiRaw(hdr->addr3)) {
        fywsEnqueueAlert(FYWS_ALERT_OUI_ADDR3, hdr->addr3, rssi, ch, "addr3");
    }
#endif

#if FYWS_ENABLE_SSID_MATCH
    (void)type;
#endif
}

static void fywsPrecompileOuis() {
    for (size_t i = 0; i < FYWS_OUI_COUNT; i++) {
        const char* o = fywsTargetOuis[i];
        fywsOuiBytes[i][0] = (uint8_t)strtol(o, nullptr, 16);
        fywsOuiBytes[i][1] = (uint8_t)strtol(o + 3, nullptr, 16);
        fywsOuiBytes[i][2] = (uint8_t)strtol(o + 6, nullptr, 16);
    }
    fywsOuisReady = true;
}

static void fywsApplyChannel(uint8_t ch) {
    fywsCurrentChannel = ch;
    esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
    fywsLastHop = millis();
}

static void fywsInstallPromiscFilter() {
    wifi_promiscuous_filter_t filt = {
        .filter_mask = 0
#if FYWS_PROCESS_MGMT
        | WIFI_PROMIS_FILTER_MASK_MGMT
#endif
#if FYWS_PROCESS_DATA
        | WIFI_PROMIS_FILTER_MASK_DATA
#endif
    };
    esp_wifi_set_promiscuous_filter(&filt);
    esp_wifi_set_promiscuous_rx_cb(&fywsWifiSniffer);
}

void fyWiFiSniffInit() {
    if (!fywsOuisReady) {
        fywsPrecompileOuis();
    }
    fywsAlertHead = 0;
    fywsAlertTail = 0;
    fywsChannelIndex = 0;
    fywsCurrentChannel = fywsCustomChannels[0];
}

void fyWiFiSniffSetApCoexist(bool enable) {
    fywsApCoexist = enable;
}

void fyWiFiSniffStart() {
    if (!fywsOuisReady) {
        fyWiFiSniffInit();
    }
    if (!fywsApCoexist) {
        WiFi.mode(WIFI_STA);
        WiFi.disconnect();
        delay(20);
        fywsStackStarted = true;
    }
    fywsInstallPromiscFilter();
    if (fywsApCoexist) {
        uint8_t ch = WiFi.channel();
        if (ch < 1 || ch > 14) {
            ch = fywsCustomChannels[fywsChannelIndex];
        }
        fywsApplyChannel(ch);
    } else {
        fywsApplyChannel(fywsCustomChannels[fywsChannelIndex]);
    }
    esp_wifi_set_promiscuous(true);
    fywsStackStarted = true;
    fywsPromiscActive = true;
}

void fyWiFiSniffStop() {
    fywsPromiscActive = false;
    if (fywsStackStarted) {
        esp_wifi_set_promiscuous(false);
    }
}

bool fyWiFiSniffIsActive() {
    return fywsPromiscActive;
}

uint8_t fyWiFiSniffCurrentChannel() {
    return fywsCurrentChannel;
}

void fyWiFiSniffTick() {
    if (!fywsPromiscActive || fywsApCoexist) return;
    if (millis() - fywsLastHop < FYWS_CHANNEL_DWELL_MS) return;
    fywsChannelIndex = (fywsChannelIndex + 1) % fywsCustomChannelCount;
    fywsApplyChannel(fywsCustomChannels[fywsChannelIndex]);
    printf("[FLOCK-YOU] wifi hop ch=%u\n", fywsCurrentChannel);
}

const char* fyWiFiAlertMethod(FyWiFiAlertType type) {
    switch (type) {
        case FYWS_ALERT_WILDCARD_PROBE: return "wifi_probe";
        case FYWS_ALERT_OUI_ADDR2:      return "wifi_addr2";
        case FYWS_ALERT_OUI_ADDR1:      return "wifi_addr1";
        case FYWS_ALERT_OUI_ADDR3:      return "wifi_addr3";
        default:                        return "wifi_unknown";
    }
}

bool fyWiFiSniffPopAlert(FyWiFiAlert* out) {
    if (!out) return false;
    portENTER_CRITICAL(&fywsQueueMux);
    if (fywsAlertTail == fywsAlertHead) {
        portEXIT_CRITICAL(&fywsQueueMux);
        return false;
    }
    FyWiFiAlertEntry e;
    memcpy(&e, (const void*)&fywsAlertQueue[fywsAlertTail], sizeof(e));
    fywsAlertTail = (fywsAlertTail + 1) % FYWS_ALERT_QUEUE_SIZE;
    portEXIT_CRITICAL(&fywsQueueMux);

    out->type = e.type;
    out->rssi = e.rssi;
    out->channel = e.channel;
    memcpy(out->mac, e.mac, 6);
    strncpy(out->frameKind, e.frameKind, sizeof(out->frameKind) - 1);
    out->frameKind[sizeof(out->frameKind) - 1] = '\0';
    return true;
}

size_t fyWiFiSniffPendingCount() {
    portENTER_CRITICAL(&fywsQueueMux);
    size_t n = (fywsAlertHead + FYWS_ALERT_QUEUE_SIZE - fywsAlertTail) % FYWS_ALERT_QUEUE_SIZE;
    portEXIT_CRITICAL(&fywsQueueMux);
    return n;
}
