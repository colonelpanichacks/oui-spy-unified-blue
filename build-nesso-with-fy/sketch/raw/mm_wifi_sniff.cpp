#line 1 "/Users/nicholastenbrink/projects/nesso/oui_spy_nesso/raw/mm_wifi_sniff.cpp"
// WiFi promiscuous MAC collector for Mega_Maid mode.
// Included from mode_megamaid.cpp (anonymous namespace).

#include <Arduino.h>
#include <WiFi.h>
#include "esp_wifi.h"
#include <string.h>
#include <stdio.h>

#define MMWS_RSSI_MIN           -95
#define MMWS_CHANNEL_DWELL_MS   150
#define MMWS_ALERT_QUEUE_SIZE   32

#define MMWS_CHECK_ADDR1        1
#define MMWS_CHECK_ADDR3        0
#define MMWS_PROCESS_MGMT       1
#define MMWS_PROCESS_DATA       1

// Full 2.4 GHz band (ESP32-C6 promiscuous). Ch 12–14 rarely used in US (FCC 1–11);
// ch 14 is Japan-only in practice — harmless to scan, may see no traffic.
static const uint8_t mmwsCustomChannels[] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14
};
static const size_t mmwsCustomChannelCount =
    sizeof(mmwsCustomChannels) / sizeof(mmwsCustomChannels[0]);

enum MmWiFiAlertType : uint8_t {
    MMWS_ALERT_ADDR2 = 0,
    MMWS_ALERT_ADDR1 = 1,
    MMWS_ALERT_ADDR3 = 2,
    MMWS_ALERT_PROBE = 3,
    MMWS_ALERT_BEACON = 4,
    MMWS_ALERT_PROBE_RESP = 5,
};

struct MmWiFiAlert {
    MmWiFiAlertType type;
    uint8_t mac[6];
    int8_t rssi;
    uint8_t channel;
    char frameKind[12];
    char ssid[33];
    bool hasSsid;
};

typedef struct __attribute__((packed)) {
    uint16_t frame_ctrl;
    uint16_t duration;
    uint8_t addr1[6];
    uint8_t addr2[6];
    uint8_t addr3[6];
    uint16_t seq_ctrl;
} mmws_mac_hdr_t;

typedef struct {
    MmWiFiAlertType type;
    uint8_t mac[6];
    int8_t rssi;
    uint8_t channel;
    char frameKind[12];
    char ssid[33];
    bool hasSsid;
} MmWiFiAlertEntry;

static volatile MmWiFiAlertEntry mmwsAlertQueue[MMWS_ALERT_QUEUE_SIZE];
static volatile size_t mmwsAlertHead = 0;
static volatile size_t mmwsAlertTail = 0;
static portMUX_TYPE mmwsQueueMux = portMUX_INITIALIZER_UNLOCKED;

static uint8_t mmwsCurrentChannel = 1;
static size_t mmwsChannelIndex = 0;
static unsigned long mmwsLastHop = 0;
static bool mmwsStackStarted = false;
static bool mmwsPromiscActive = false;
static bool mmwsApCoexist = false;

static void IRAM_ATTR mmwsEnqueueAlertEx(MmWiFiAlertType type, const uint8_t* mac,
                                           int8_t rssi, uint8_t ch, const char* kind,
                                           const char* ssid, bool hasSsid) {
    portENTER_CRITICAL_ISR(&mmwsQueueMux);
    size_t next = (mmwsAlertHead + 1) % MMWS_ALERT_QUEUE_SIZE;
    if (next == mmwsAlertTail) {
        portEXIT_CRITICAL_ISR(&mmwsQueueMux);
        return;
    }
    MmWiFiAlertEntry* e = (MmWiFiAlertEntry*)&mmwsAlertQueue[mmwsAlertHead];
    e->type = type;
    e->rssi = rssi;
    e->channel = ch;
    e->hasSsid = hasSsid;
    memcpy((void*)e->mac, mac, 6);
    if (kind) {
        strncpy((char*)e->frameKind, kind, 11);
        ((char*)e->frameKind)[11] = '\0';
    } else {
        ((char*)e->frameKind)[0] = '\0';
    }
    if (hasSsid && ssid) {
        strncpy((char*)e->ssid, ssid, 32);
        ((char*)e->ssid)[32] = '\0';
    } else {
        ((char*)e->ssid)[0] = '\0';
    }
    mmwsAlertHead = next;
    portEXIT_CRITICAL_ISR(&mmwsQueueMux);
}

static void IRAM_ATTR mmwsEnqueueAlert(MmWiFiAlertType type, const uint8_t* mac,
                                       int8_t rssi, uint8_t ch, const char* kind) {
    mmwsEnqueueAlertEx(type, mac, rssi, ch, kind, nullptr, false);
}

static inline bool IRAM_ATTR mmwsIsMulticast(const uint8_t* mac) {
    return mac[0] & 0x01;
}

static inline bool IRAM_ATTR mmwsIsLocallyAdministered(const uint8_t* mac) {
    return mac[0] & 0x02;
}

static inline bool IRAM_ATTR mmwsIsCollectableMac(const uint8_t* mac) {
    return !mmwsIsMulticast(mac) && !mmwsIsLocallyAdministered(mac);
}

// Returns 1 if SSID IE found (out filled, may be empty), 0 if not found, -1 on error.
static int IRAM_ATTR mmwsParseSsidIE(const uint8_t* body, int len, char* out, size_t outCap) {
    if (!body || len < 2 || !out || outCap < 1) return -1;
    while (len >= 2) {
        uint8_t id = body[0];
        uint8_t elen = body[1];
        if ((int)elen + 2 > len) break;
        if (id == 0) {
            out[0] = '\0';
            if (elen > 0) {
                size_t copyLen = (size_t)elen;
                if (copyLen >= outCap) copyLen = outCap - 1;
                memcpy(out, body + 2, copyLen);
                out[copyLen] = '\0';
            }
            return 1;
        }
        body += elen + 2;
        len -= elen + 2;
    }
    return 0;
}

static bool IRAM_ATTR mmwsMgmtTaggedParams(const uint8_t* payload, int sigLen, uint8_t subtype,
                                           const uint8_t** body, int* bodyLen) {
    if (!payload || !body || !bodyLen || sigLen < (int)sizeof(mmws_mac_hdr_t)) return false;
    const uint8_t* p = payload + sizeof(mmws_mac_hdr_t);
    int remain = sigLen - (int)sizeof(mmws_mac_hdr_t);
    if (subtype == 8 || subtype == 5) {
        if (remain < 12) return false;
        p += 12;
        remain -= 12;
    }
    *body = p;
    *bodyLen = remain;
    return true;
}

static void IRAM_ATTR mmwsProcessMgmtSsid(const mmws_mac_hdr_t* hdr, const uint8_t* payload,
                                          int sigLen, int8_t rssi, uint8_t ch) {
    uint8_t fc0 = hdr->frame_ctrl & 0xFF;
    uint8_t ftype = (fc0 >> 2) & 0x03;
    uint8_t subtype = (fc0 >> 4) & 0x0F;
    if (ftype != 0) return;

    const uint8_t* body = nullptr;
    int bodyLen = 0;
    if (!mmwsMgmtTaggedParams(payload, sigLen, subtype, &body, &bodyLen)) return;

    char ssid[33];
    int ssidResult = mmwsParseSsidIE(body, bodyLen, ssid, sizeof(ssid));
    if (ssidResult == 0 && bodyLen > 4) {
        ssidResult = mmwsParseSsidIE(body, bodyLen - 4, ssid, sizeof(ssid));
    }
    if (ssidResult != 1) return;

    if (subtype == 4 && !mmwsIsMulticast(hdr->addr2)) {
        mmwsEnqueueAlertEx(MMWS_ALERT_PROBE, hdr->addr2, rssi, ch, "probe_req", ssid, true);
    } else if (subtype == 8 && mmwsIsCollectableMac(hdr->addr2)) {
        mmwsEnqueueAlertEx(MMWS_ALERT_PROBE, hdr->addr2, rssi, ch, "probe_req", ssid, true);
    } else if (subtype == 8 && mmwsIsCollectableMac(hdr->addr2)) {
        mmwsEnqueueAlertEx(MMWS_ALERT_BEACON, hdr->addr2, rssi, ch, "beacon", ssid, true);
    } else if (subtype == 5 && mmwsIsCollectableMac(hdr->addr2)) {
        mmwsEnqueueAlertEx(MMWS_ALERT_PROBE_RESP, hdr->addr2, rssi, ch, "probe_resp", ssid, true);
    }
}

static void IRAM_ATTR mmwsWifiSniffer(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (!buf || !mmwsPromiscActive) return;

#if MMWS_PROCESS_MGMT && MMWS_PROCESS_DATA
    if (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA) return;
#elif MMWS_PROCESS_MGMT
    if (type != WIFI_PKT_MGMT) return;
#elif MMWS_PROCESS_DATA
    if (type != WIFI_PKT_DATA) return;
#else
    return;
#endif

    wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
    if (pkt->rx_ctrl.sig_len < sizeof(mmws_mac_hdr_t)) return;
    mmws_mac_hdr_t* hdr = (mmws_mac_hdr_t*)pkt->payload;
    int8_t rssi = pkt->rx_ctrl.rssi;
    if (rssi < MMWS_RSSI_MIN) return;

    uint8_t ch = (uint8_t)pkt->rx_ctrl.channel;
    int sigLen = (int)pkt->rx_ctrl.sig_len;

    if (type == WIFI_PKT_MGMT) {
        mmwsProcessMgmtSsid(hdr, pkt->payload, sigLen, rssi, ch);
    }

    if (mmwsIsCollectableMac(hdr->addr2)) {
        mmwsEnqueueAlert(MMWS_ALERT_ADDR2, hdr->addr2, rssi, ch, "addr2");
    }

#if MMWS_CHECK_ADDR1
    if (mmwsIsCollectableMac(hdr->addr1)) {
        mmwsEnqueueAlert(MMWS_ALERT_ADDR1, hdr->addr1, rssi, ch, "addr1");
    }
#endif

#if MMWS_CHECK_ADDR3
    if (type == WIFI_PKT_MGMT && mmwsIsCollectableMac(hdr->addr3)) {
        mmwsEnqueueAlert(MMWS_ALERT_ADDR3, hdr->addr3, rssi, ch, "addr3");
    }
#endif
}

static void mmwsApplyChannel(uint8_t ch) {
    mmwsCurrentChannel = ch;
    esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
    mmwsLastHop = millis();
}

static void mmwsInstallPromiscFilter() {
    wifi_promiscuous_filter_t filt = {
        .filter_mask = 0
#if MMWS_PROCESS_MGMT
        | WIFI_PROMIS_FILTER_MASK_MGMT
#endif
#if MMWS_PROCESS_DATA
        | WIFI_PROMIS_FILTER_MASK_DATA
#endif
    };
    esp_wifi_set_promiscuous_filter(&filt);
    esp_wifi_set_promiscuous_rx_cb(&mmwsWifiSniffer);
}

void mmWiFiSniffInit() {
    mmwsAlertHead = 0;
    mmwsAlertTail = 0;
    mmwsChannelIndex = 0;
    mmwsCurrentChannel = mmwsCustomChannels[0];
}

void mmWiFiSniffSetApCoexist(bool enable) {
    mmwsApCoexist = enable;
}

void mmWiFiSniffStart() {
    if (!mmwsApCoexist) {
        WiFi.mode(WIFI_STA);
        WiFi.disconnect();
        delay(100);
        mmwsStackStarted = true;
    }
    mmwsInstallPromiscFilter();
    if (mmwsApCoexist) {
        uint8_t ch = WiFi.channel();
        if (ch < 1 || ch > 14) {
            ch = mmwsCustomChannels[mmwsChannelIndex];
        }
        mmwsApplyChannel(ch);
    } else {
        mmwsApplyChannel(mmwsCustomChannels[mmwsChannelIndex]);
    }
    esp_wifi_set_promiscuous(true);
    mmwsStackStarted = true;
    mmwsPromiscActive = true;
}

void mmWiFiSniffStop() {
    mmwsPromiscActive = false;
    if (mmwsStackStarted) {
        esp_wifi_set_promiscuous(false);
    }
}

bool mmWiFiSniffIsActive() {
    return mmwsPromiscActive;
}

uint8_t mmWiFiSniffCurrentChannel() {
    return mmwsCurrentChannel;
}

void mmWiFiSniffTick() {
    if (!mmwsPromiscActive || mmwsApCoexist) return;
    wifi_mode_t mode = WiFi.getMode();
    if (mode == WIFI_MODE_NULL || mode == WIFI_MODE_AP) return;
    if (millis() - mmwsLastHop < MMWS_CHANNEL_DWELL_MS) return;
    mmwsChannelIndex = (mmwsChannelIndex + 1) % mmwsCustomChannelCount;
    mmwsApplyChannel(mmwsCustomChannels[mmwsChannelIndex]);
    printf("[MEGA-MAID] wifi hop ch=%u\n", mmwsCurrentChannel);
}

const char* mmWiFiAlertMethod(MmWiFiAlertType type) {
    switch (type) {
        case MMWS_ALERT_PROBE:       return "wifi_probe";
        case MMWS_ALERT_BEACON:      return "wifi_beacon";
        case MMWS_ALERT_PROBE_RESP:  return "wifi_probe_resp";
        case MMWS_ALERT_ADDR2:       return "wifi_addr2";
        case MMWS_ALERT_ADDR1:       return "wifi_addr1";
        case MMWS_ALERT_ADDR3:       return "wifi_addr3";
        default:                     return "wifi_unknown";
    }
}

bool mmWiFiSniffPopAlert(MmWiFiAlert* out) {
    if (!out) return false;
    portENTER_CRITICAL(&mmwsQueueMux);
    if (mmwsAlertTail == mmwsAlertHead) {
        portEXIT_CRITICAL(&mmwsQueueMux);
        return false;
    }
    MmWiFiAlertEntry e;
    memcpy(&e, (const void*)&mmwsAlertQueue[mmwsAlertTail], sizeof(e));
    mmwsAlertTail = (mmwsAlertTail + 1) % MMWS_ALERT_QUEUE_SIZE;
    portEXIT_CRITICAL(&mmwsQueueMux);

    out->type = e.type;
    out->rssi = e.rssi;
    out->channel = e.channel;
    out->hasSsid = e.hasSsid;
    memcpy(out->mac, e.mac, 6);
    strncpy(out->frameKind, e.frameKind, sizeof(out->frameKind) - 1);
    out->frameKind[sizeof(out->frameKind) - 1] = '\0';
    strncpy(out->ssid, e.ssid, sizeof(out->ssid) - 1);
    out->ssid[sizeof(out->ssid) - 1] = '\0';
    return true;
}

size_t mmWiFiSniffPendingCount() {
    portENTER_CRITICAL(&mmwsQueueMux);
    size_t n = (mmwsAlertHead + MMWS_ALERT_QUEUE_SIZE - mmwsAlertTail) % MMWS_ALERT_QUEUE_SIZE;
    portEXIT_CRITICAL(&mmwsQueueMux);
    return n;
}
