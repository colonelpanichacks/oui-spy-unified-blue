/*
 * Mode 6: BLE SNIFF - Passive BLE Advertising Capture
 *
 * Merged from the standalone colonelpanichacks/ouispy-blesniff firmware:
 *   config.{h,cpp} + scan.{h,cpp} + nordic_pcap.{h,cpp} + pcap_stream.{h,cpp} +
 *   session_pcap.{h,cpp} + text_summary.{h,cpp} + web_dashboard.{h,cpp} +
 *   dashboard_html.h + main.cpp
 *
 * LINKTYPE_BLUETOOTH_LE_LL_WITH_PHDR (256) pcap over USB-CDC + text summary,
 * live dashboard on ouispy-blesniff / sniffuntothem, 2 MB in-PSRAM session
 * pcap available for browser download. Passive receive only - no scan
 * requests are ever transmitted.
 */

// Headers are included by the wrapper (mode_blesniff.cpp) outside the
// anonymous namespace so the symbols get external linkage. Re-including them
// here is a no-op thanks to header guards.
#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <driver/ledc.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <NimBLEDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <sys/time.h>
#include <ctype.h>
#include <string.h>
#include <stdio.h>

// ---------------------------------------------------------------------------
// Hardware - matches sibling modes (active-low LED on GPIO21).
// ---------------------------------------------------------------------------
#define BLESNIFF_BUZZER_PIN 3
#define BLESNIFF_LED_PIN    21

static const ledc_channel_t BLESNIFF_BUZZER_CH    = LEDC_CHANNEL_0;
static const ledc_timer_t   BLESNIFF_BUZZER_TIMER = LEDC_TIMER_0;

// ---------------------------------------------------------------------------
// config - persistent runtime settings (NVS namespace: "blesniff")
// ---------------------------------------------------------------------------
namespace config {

// Filter mask bits - matched against per-advert traits at capture time.
// Advert type bits use the wire-level LE LL PDU type numbering.
constexpr uint8_t FT_ADV_IND       = 0x01;
constexpr uint8_t FT_ADV_DIRECT    = 0x02;
constexpr uint8_t FT_ADV_NONCONN   = 0x04;
constexpr uint8_t FT_SCAN_RSP      = 0x08;
constexpr uint8_t FT_ADV_SCAN_IND  = 0x10;
constexpr uint8_t FT_ADDR_PUBLIC   = 0x20;
constexpr uint8_t FT_ADDR_RANDOM   = 0x40;
constexpr uint8_t FT_SPARE_7       = 0x80;

constexpr uint8_t FT_DEFAULT =
    FT_ADV_IND | FT_ADV_DIRECT | FT_ADV_NONCONN | FT_SCAN_RSP | FT_ADV_SCAN_IND |
    FT_ADDR_PUBLIC | FT_ADDR_RANDOM;

// USB output is text-only (line summaries + CMD replies). PCAP binary
// capture lives on the dashboard exclusively -- GET /api/session.pcap.
struct Config {
    uint16_t scan_window_ms;
    uint16_t scan_interval_ms;
    uint8_t  ft_mask;
    char     ap_ssid[33];
    char     ap_pass[64];
};

// unified-blue NVS convention: mode owns its own namespace.
constexpr const char* NS      = "blesniff";
constexpr const char* VERSION = "1.0.0";

Preferences prefs;
Config      cfg;

void apply_defaults() {
    // Leave ~70% of the 2.4 GHz radio for WiFi coexistence so the AP stays
    // reachable while we scan. window==interval starves SoftAP beacons.
    cfg.scan_window_ms   = 30;
    cfg.scan_interval_ms = 100;
    cfg.ft_mask          = FT_DEFAULT;
    strlcpy(cfg.ap_ssid, "ouispy-blesniff", sizeof(cfg.ap_ssid));
    strlcpy(cfg.ap_pass, "sniffuntothem",   sizeof(cfg.ap_pass));
}

void clamp() {
    if (cfg.scan_window_ms < 10)               cfg.scan_window_ms = 10;
    if (cfg.scan_window_ms > 2000)             cfg.scan_window_ms = 2000;
    if (cfg.scan_interval_ms < 20)             cfg.scan_interval_ms = 20;
    if (cfg.scan_interval_ms > 4000)           cfg.scan_interval_ms = 4000;
    if (cfg.scan_window_ms > cfg.scan_interval_ms)
        cfg.scan_window_ms = cfg.scan_interval_ms;
    if (cfg.ft_mask == 0)                      cfg.ft_mask = FT_DEFAULT;
    if (strlen(cfg.ap_ssid) == 0)              strlcpy(cfg.ap_ssid, "ouispy-blesniff", sizeof(cfg.ap_ssid));
    size_t pl = strlen(cfg.ap_pass);
    if (pl < 8 || pl > 63)                     strlcpy(cfg.ap_pass, "sniffuntothem", sizeof(cfg.ap_pass));
}

const char* FW_VERSION() { return VERSION; }
Config&     get()        { return cfg; }

void save() {
    clamp();
    prefs.begin(NS, false);
    prefs.putUShort("scan_win", cfg.scan_window_ms);
    prefs.putUShort("scan_int", cfg.scan_interval_ms);
    prefs.putUChar ("ftmask",   cfg.ft_mask);
    prefs.putString("ap_ssid",  cfg.ap_ssid);
    prefs.putString("ap_pass",  cfg.ap_pass);
    prefs.end();
}

void load() {
    apply_defaults();
    prefs.begin(NS, true);
    cfg.scan_window_ms   = prefs.getUShort("scan_win", cfg.scan_window_ms);
    cfg.scan_interval_ms = prefs.getUShort("scan_int", cfg.scan_interval_ms);
    cfg.ft_mask          = prefs.getUChar ("ftmask",   cfg.ft_mask);
    prefs.getString("ap_ssid", cfg.ap_ssid, sizeof(cfg.ap_ssid));
    prefs.getString("ap_pass", cfg.ap_pass, sizeof(cfg.ap_pass));
    prefs.end();
    clamp();
}

void reset_defaults() { apply_defaults(); save(); }

void set_scan_window(uint16_t ms)   { cfg.scan_window_ms = ms;   save(); }
void set_scan_interval(uint16_t ms) { cfg.scan_interval_ms = ms; save(); }
void set_ftmask(uint8_t m)          { cfg.ft_mask = m ? m : FT_DEFAULT; save(); }

void set_ap(const char* ssid, const char* pass) {
    if (ssid && *ssid) strlcpy(cfg.ap_ssid, ssid, sizeof(cfg.ap_ssid));
    if (pass) {
        size_t l = strlen(pass);
        if (l >= 8 && l <= 63) strlcpy(cfg.ap_pass, pass, sizeof(cfg.ap_pass));
    }
    save();
}

} // namespace config

// ---------------------------------------------------------------------------
// scan - passive NimBLE advert capture into two ring buffers
// ---------------------------------------------------------------------------
namespace scan {

// Legacy LE advertising PDUs are capped at 37 bytes (6 addr + 31 AdvData).
// Extended advertising can be larger; 256 covers both comfortably.
constexpr uint16_t MAX_PAYLOAD = 256;

// LE LL PDU type numbering (wire-level), used everywhere downstream.
// NimBLE's HCI advType enum is different - mapped below.
constexpr uint8_t LL_ADV_IND         = 0;
constexpr uint8_t LL_ADV_DIRECT_IND  = 1;
constexpr uint8_t LL_ADV_NONCONN_IND = 2;
constexpr uint8_t LL_SCAN_REQ        = 3;
constexpr uint8_t LL_SCAN_RSP        = 4;
constexpr uint8_t LL_CONNECT_IND     = 5;
constexpr uint8_t LL_ADV_SCAN_IND    = 6;

// Address type classification (refined for random subtypes).
constexpr uint8_t ADDR_PUBLIC        = 0;
constexpr uint8_t ADDR_RANDOM_STATIC = 1;
constexpr uint8_t ADDR_RANDOM_NRP    = 2;   // non-resolvable private
constexpr uint8_t ADDR_RANDOM_RPA    = 3;   // resolvable private
constexpr uint8_t ADDR_UNKNOWN       = 0xFF;

struct Frame {
    uint32_t idx;
    uint32_t ts_sec;
    uint32_t ts_usec;
    uint8_t  channel;        // 0xFF if unknown; NimBLE doesn't expose per-advert channel
    int8_t   rssi;
    int8_t   tx_power;       // INT8_MIN if not present in advert
    uint8_t  ll_pdu_type;    // LL_* above
    uint8_t  addr_type;      // ADDR_* above
    uint8_t  addr[6];        // advertising address, addr[5]=MSB
    uint16_t payload_len;
    uint8_t  payload[MAX_PAYLOAD];
};

struct Ring {
    Frame*        slots;
    size_t        capacity;
    volatile size_t head;
    volatile size_t tail;
    volatile uint32_t dropped;
    portMUX_TYPE  mux;
};

Ring ring_pcap = { nullptr, 0, 0, 0, 0, portMUX_INITIALIZER_UNLOCKED };
Ring ring_dash = { nullptr, 0, 0, 0, 0, portMUX_INITIALIZER_UNLOCKED };

volatile uint32_t g_total          = 0;
volatile uint32_t g_this_sec       = 0;
volatile uint32_t g_per_sec        = 0;
volatile uint32_t g_last_pps_ms    = 0;
volatile uint32_t g_frame_idx      = 0;

bool ring_alloc(Ring& r, size_t slot_count, bool prefer_psram) {
    r.capacity = slot_count;
    r.head = 0; r.tail = 0; r.dropped = 0;
    size_t bytes = slot_count * sizeof(Frame);
    if (prefer_psram && psramFound()) {
        r.slots = (Frame*)ps_malloc(bytes);
        if (r.slots) return true;
    }
    r.slots = (Frame*)malloc(bytes);
    return r.slots != nullptr;
}

inline size_t ring_next(const Ring& r, size_t i) {
    return (i + 1) % r.capacity;
}

void ring_push(Ring& r, const Frame& f) {
    portENTER_CRITICAL_ISR(&r.mux);
    size_t next_head = ring_next(r, r.head);
    if (next_head == r.tail) {
        r.tail = ring_next(r, r.tail);
        r.dropped++;
    }
    r.slots[r.head] = f;
    r.head = next_head;
    portEXIT_CRITICAL_ISR(&r.mux);
}

bool ring_pop(Ring& r, Frame* out) {
    bool got = false;
    portENTER_CRITICAL(&r.mux);
    if (r.tail != r.head) {
        *out = r.slots[r.tail];
        r.tail = ring_next(r, r.tail);
        got = true;
    }
    portEXIT_CRITICAL(&r.mux);
    return got;
}

// NimBLE HCI advType (from advertisement report) -> wire LL PDU type.
uint8_t map_hci_advtype_to_ll(uint8_t hci) {
    switch (hci) {
        case 0: return LL_ADV_IND;         // BLE_HCI_ADV_TYPE_ADV_IND
        case 1: return LL_ADV_DIRECT_IND;  // BLE_HCI_ADV_TYPE_ADV_DIRECT_IND_HD
        case 2: return LL_ADV_SCAN_IND;    // BLE_HCI_ADV_TYPE_ADV_SCAN_IND
        case 3: return LL_ADV_NONCONN_IND; // BLE_HCI_ADV_TYPE_ADV_NONCONN_IND
        case 4: return LL_SCAN_RSP;        // BLE_HCI_ADV_TYPE_SCAN_RSP
        default: return LL_ADV_IND;
    }
}

// Random address subtype is encoded in the top two bits of MSB (byte 5).
uint8_t classify_random_addr(const uint8_t addr[6]) {
    uint8_t hi2 = (addr[5] >> 6) & 0x03;
    switch (hi2) {
        case 0b00: return ADDR_RANDOM_NRP;
        case 0b01: return ADDR_RANDOM_RPA;
        case 0b11: return ADDR_RANDOM_STATIC;
        default:   return ADDR_RANDOM_STATIC; // 0b10 reserved; treat as static
    }
}

uint8_t ftbit_for_ll(uint8_t ll_pdu_type) {
    switch (ll_pdu_type) {
        case LL_ADV_IND:         return config::FT_ADV_IND;
        case LL_ADV_DIRECT_IND:  return config::FT_ADV_DIRECT;
        case LL_ADV_NONCONN_IND: return config::FT_ADV_NONCONN;
        case LL_SCAN_RSP:        return config::FT_SCAN_RSP;
        case LL_ADV_SCAN_IND:    return config::FT_ADV_SCAN_IND;
        default:                 return 0xFF;
    }
}

class Cb : public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice* dev) override {
        if (!dev) return;

        // Filter early - cheap byte checks. Advert-type gate.
        uint8_t ll = map_hci_advtype_to_ll(dev->getAdvType());
        uint8_t ft = config::get().ft_mask;
        uint8_t typebit = ftbit_for_ll(ll);
        if (typebit != 0xFF && (ft & typebit) == 0) return;

        NimBLEAddress a = dev->getAddress();
        uint8_t addr_type_raw = a.getType();

        // NimBLE stores m_address[5]=MSB, m_address[0]=LSB (matches its toString()).
        // Frame.addr keeps the same convention; nordic_pcap writes bytes on-wire LSB-first.
        const uint8_t* nat = a.getNative();
        uint8_t addr[6];
        memcpy(addr, nat, 6);

        uint8_t addr_type;
        if (addr_type_raw == 0 || addr_type_raw == 2) {
            addr_type = ADDR_PUBLIC;
            if ((ft & config::FT_ADDR_PUBLIC) == 0) return;
        } else if (addr_type_raw == 1 || addr_type_raw == 3) {
            addr_type = classify_random_addr(addr);
            if ((ft & config::FT_ADDR_RANDOM) == 0) return;
        } else {
            addr_type = ADDR_UNKNOWN;
        }

        Frame f;
        f.idx  = ++g_frame_idx;
        struct timeval tv;
        gettimeofday(&tv, nullptr);
        f.ts_sec  = (uint32_t)tv.tv_sec;
        f.ts_usec = (uint32_t)tv.tv_usec;
        f.channel = 0xFF;
        f.rssi    = (int8_t)dev->getRSSI();
        f.tx_power = dev->haveTXPower() ? (int8_t)dev->getTXPower() : INT8_MIN;
        f.ll_pdu_type = ll;
        f.addr_type   = addr_type;
        memcpy(f.addr, addr, 6);

        size_t plen = dev->getPayloadLength();
        if (plen > MAX_PAYLOAD) plen = MAX_PAYLOAD;
        f.payload_len = (uint16_t)plen;
        if (plen > 0) memcpy(f.payload, dev->getPayload(), plen);

        ring_push(ring_pcap, f);
        ring_push(ring_dash, f);

        g_total++;
        g_this_sec++;
    }
};

Cb g_cb;

void start_scan() {
    NimBLEScan* s = NimBLEDevice::getScan();
    s->stop();
    s->setActiveScan(false);           // passive - never emit SCAN_REQ
    s->setInterval(config::get().scan_interval_ms);
    s->setWindow(config::get().scan_window_ms);
    s->setDuplicateFilter(false);      // capture every advert, even repeats
    s->setAdvertisedDeviceCallbacks(&g_cb, /*wantDuplicates=*/true);
    s->start(0, nullptr, false);
}

bool init() {
    bool have_psram = psramFound();
    size_t pcap_slots = have_psram ? 256 : 16;
    size_t dash_slots = have_psram ? 64  : 8;
    if (!ring_alloc(ring_pcap, pcap_slots, true)) return false;
    if (!ring_alloc(ring_dash, dash_slots, true)) return false;

    // Idempotent - other sibling modes may have already brought NimBLE up
    // (though the outer boot selector routes once, so we're usually first).
    if (!NimBLEDevice::getInitialized()) NimBLEDevice::init("");
    start_scan();
    return true;
}

void apply_scan_params() { start_scan(); }

bool pop_pcap(Frame* out)      { return ring_pop(ring_pcap, out); }
bool pop_dashboard(Frame* out) { return ring_pop(ring_dash, out); }

uint32_t total_adverts() { return g_total; }
uint32_t dropped_pcap()  { return ring_pcap.dropped; }
uint32_t dropped_dash()  { return ring_dash.dropped; }

uint32_t adverts_per_sec() {
    uint32_t now = millis();
    if (now - g_last_pps_ms >= 1000) {
        g_per_sec = g_this_sec;
        g_this_sec = 0;
        g_last_pps_ms = now;
    }
    return g_per_sec;
}

void clear_ring() {
    portENTER_CRITICAL(&ring_pcap.mux);
    ring_pcap.head = ring_pcap.tail = 0; ring_pcap.dropped = 0;
    portEXIT_CRITICAL(&ring_pcap.mux);
    portENTER_CRITICAL(&ring_dash.mux);
    ring_dash.head = ring_dash.tail = 0; ring_dash.dropped = 0;
    portEXIT_CRITICAL(&ring_dash.mux);
}

} // namespace scan

// ---------------------------------------------------------------------------
// nordic_pcap - builds LINKTYPE_BLUETOOTH_LE_LL_WITH_PHDR (256) records
// ---------------------------------------------------------------------------
namespace nordic_pcap {

constexpr size_t PHDR_LEN         = 10;
constexpr size_t ACCESS_ADDR_LEN  = 4;
constexpr size_t LL_HDR_LEN       = 2;
constexpr size_t ADV_ADDR_LEN     = 6;
constexpr size_t CRC_LEN          = 3;

constexpr size_t FRAME_OVERHEAD =
    PHDR_LEN + ACCESS_ADDR_LEN + LL_HDR_LEN + ADV_ADDR_LEN + CRC_LEN;

constexpr uint32_t PCAP_MAGIC        = 0xA1B2C3D4;
constexpr uint16_t PCAP_VER_MAJOR    = 2;
constexpr uint16_t PCAP_VER_MINOR    = 4;
constexpr uint32_t PCAP_LINKTYPE     = 256;  // LINKTYPE_BLUETOOTH_LE_LL_WITH_PHDR
constexpr uint32_t PCAP_SNAPLEN      = 512;

// Advertising channel access address (little-endian on wire: D6 BE 89 8E).
constexpr uint32_t ADV_ACCESS_ADDR   = 0x8E89BED6;

// Pseudo-header flags for advertising packets. Bit layout per Wireshark's
// btle_rf dissector (verified against tshark -G fields):
//   bit 0 (0x0001) dewhitened
//   bit 1 (0x0002) signal-power-valid
//   bit 4 (0x0010) reference-access-address-valid
//   bits 7-9 (0x0380) PDU Type: 0=Advertising, 4=CIS C->P, etc.
//   bit 10 (0x0400) CRC checked
//   bit 11 (0x0800) CRC valid
// PDU Type left at 0 (Advertising). CRC bits cleared because we synthesize
// zero CRC bytes -- claiming "checked" would make Wireshark flag CRC-bad.
constexpr uint16_t PHDR_FLAGS        = 0x0013;

size_t build_frame(const scan::Frame& f, uint8_t* out) {
    uint8_t* p = out;

    // -------- 10-byte LE-LL-WITH-PHDR pseudo-header --------
    // Byte 0: RF channel index (0..39). 0xFF means unknown; map to 39.
    uint8_t ch = (f.channel <= 39) ? f.channel : 39;
    *p++ = ch;
    *p++ = (uint8_t)f.rssi;   // signal power (int8 dBm)
    *p++ = 0;                 // noise power (unused)
    *p++ = 0;                 // access-address offenses
    *p++ = (uint8_t)(ADV_ACCESS_ADDR      );
    *p++ = (uint8_t)(ADV_ACCESS_ADDR >>  8);
    *p++ = (uint8_t)(ADV_ACCESS_ADDR >> 16);
    *p++ = (uint8_t)(ADV_ACCESS_ADDR >> 24);
    *p++ = (uint8_t)(PHDR_FLAGS      );
    *p++ = (uint8_t)(PHDR_FLAGS >>  8);

    // -------- 4-byte access address (on-air, little-endian) --------
    *p++ = (uint8_t)(ADV_ACCESS_ADDR      );
    *p++ = (uint8_t)(ADV_ACCESS_ADDR >>  8);
    *p++ = (uint8_t)(ADV_ACCESS_ADDR >> 16);
    *p++ = (uint8_t)(ADV_ACCESS_ADDR >> 24);

    // -------- 2-byte LL header --------
    uint8_t hdr0 = f.ll_pdu_type & 0x0F;
    bool tx_random = (f.addr_type != scan::ADDR_PUBLIC && f.addr_type != scan::ADDR_UNKNOWN);
    if (tx_random) hdr0 |= 0x40;
    if (f.ll_pdu_type == scan::LL_ADV_DIRECT_IND) hdr0 |= 0x80;
    uint8_t len_field = (uint8_t)((6 + f.payload_len) & 0x3F);
    *p++ = hdr0;
    *p++ = len_field;

    // -------- 6-byte advertising address (little-endian on wire) --------
    // Frame.addr[0] is LSB, addr[5] is MSB. Write in stored order.
    for (int i = 0; i < 6; ++i) *p++ = f.addr[i];

    // -------- AdvData --------
    if (f.payload_len) {
        memcpy(p, f.payload, f.payload_len);
        p += f.payload_len;
    }

    // -------- 3-byte CRC (synthesized zero) --------
    *p++ = 0;
    *p++ = 0;
    *p++ = 0;

    return (size_t)(p - out);
}

inline size_t frame_size(const scan::Frame& f) {
    return FRAME_OVERHEAD + f.payload_len;
}

} // namespace nordic_pcap

// ---------------------------------------------------------------------------
// text_summary - one-line human-readable summary + AD-structure helpers
// ---------------------------------------------------------------------------
namespace text_summary {

namespace ad {
constexpr uint8_t FLAGS                    = 0x01;
constexpr uint8_t INCOMPLETE_16BIT_UUIDS   = 0x02;
constexpr uint8_t COMPLETE_16BIT_UUIDS     = 0x03;
constexpr uint8_t INCOMPLETE_32BIT_UUIDS   = 0x04;
constexpr uint8_t COMPLETE_32BIT_UUIDS     = 0x05;
constexpr uint8_t INCOMPLETE_128BIT_UUIDS  = 0x06;
constexpr uint8_t COMPLETE_128BIT_UUIDS    = 0x07;
constexpr uint8_t SHORTENED_LOCAL_NAME     = 0x08;
constexpr uint8_t COMPLETE_LOCAL_NAME      = 0x09;
constexpr uint8_t TX_POWER_LEVEL           = 0x0A;
constexpr uint8_t SERVICE_DATA_16          = 0x16;
constexpr uint8_t SERVICE_DATA_32          = 0x20;
constexpr uint8_t SERVICE_DATA_128         = 0x21;
constexpr uint8_t MANUFACTURER_SPECIFIC    = 0xFF;
} // namespace ad

// Traits bitfield surfaced to dashboard (bit-per-trait).
constexpr uint8_t TR_HAS_NAME     = 0x01;
constexpr uint8_t TR_HAS_MFR      = 0x02;
constexpr uint8_t TR_HAS_SVC_DATA = 0x04;
constexpr uint8_t TR_HAS_TXPOWER  = 0x08;
constexpr uint8_t TR_CONNECTABLE  = 0x10;

const char* ll_type_name(uint8_t t) {
    switch (t) {
        case scan::LL_ADV_IND:         return "ADV_IND";
        case scan::LL_ADV_DIRECT_IND:  return "ADV_DIRECT";
        case scan::LL_ADV_NONCONN_IND: return "ADV_NONCONN";
        case scan::LL_SCAN_REQ:        return "SCAN_REQ";
        case scan::LL_SCAN_RSP:        return "SCAN_RSP";
        case scan::LL_CONNECT_IND:     return "CONNECT_REQ";
        case scan::LL_ADV_SCAN_IND:    return "ADV_SCAN";
        default:                       return "ADV_?";
    }
}

const char* addr_type_short(uint8_t a) {
    switch (a) {
        case scan::ADDR_PUBLIC:        return "pub";
        case scan::ADDR_RANDOM_STATIC: return "rnd-s";
        case scan::ADDR_RANDOM_NRP:    return "rnd-nrp";
        case scan::ADDR_RANDOM_RPA:    return "rnd-rpa";
        default:                       return "unk";
    }
}

void format_addr(const uint8_t addr[6], char* out18) {
    snprintf(out18, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
             addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
}

// AD-structure iterator: for each (type, len, data), invoke visit(type, data, dlen).
template <typename V>
void for_each_ad(const scan::Frame& f, V visit) {
    size_t i = 0;
    while (i < f.payload_len) {
        uint8_t ad_len = f.payload[i];
        if (ad_len == 0) { i++; continue; }
        if (i + 1 + ad_len > f.payload_len) return;
        uint8_t ad_type = f.payload[i + 1];
        const uint8_t* d = f.payload + i + 2;
        uint8_t dlen = ad_len - 1;
        if (!visit(ad_type, d, dlen)) return;
        i += 1 + ad_len;
    }
}

void extract_name(const scan::Frame& f, char* out, size_t out_sz) {
    out[0] = 0;
    for_each_ad(f, [&](uint8_t type, const uint8_t* d, uint8_t dlen) -> bool {
        if (type == ad::COMPLETE_LOCAL_NAME || type == ad::SHORTENED_LOCAL_NAME) {
            size_t n = dlen;
            if (n >= out_sz) n = out_sz - 1;
            for (size_t k = 0; k < n; ++k) {
                uint8_t c = d[k];
                out[k] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
            }
            out[n] = 0;
            return false;
        }
        return true;
    });
}

uint16_t manufacturer_id(const scan::Frame& f) {
    uint16_t id = 0xFFFF;
    for_each_ad(f, [&](uint8_t type, const uint8_t* d, uint8_t dlen) -> bool {
        if (type == ad::MANUFACTURER_SPECIFIC && dlen >= 2) {
            id = (uint16_t)d[0] | ((uint16_t)d[1] << 8);
            return false;
        }
        return true;
    });
    return id;
}

const char* mfr_shortname(uint16_t id) {
    switch (id) {
        // Big consumer
        case 0x004C: return "Apple";
        case 0x0006: return "Microsoft";
        case 0x00E0: return "Google";
        case 0x0075: return "Samsung";
        case 0x0171: return "Amazon";
        case 0x038F: return "Xiaomi";
        case 0x0087: return "Garmin";
        case 0x00D2: return "Sonos";
        case 0x008A: return "Bose";
        case 0x2C00: return "GoPro";
        // Silicon / dev
        case 0x0059: return "Nordic";
        case 0x0131: return "Cypress";
        case 0x02E5: return "Espressif";
        case 0x000F: return "Broadcom";
        case 0x0002: return "Nokia";
        case 0x0157: return "Anhui Huami";
        case 0x0499: return "Ruuvi";
        // Surveillance / drones / smartglasses (from Detector OUI Database)
        case 0x034D: return "Axon/TASER";
        case 0x0D53: return "Luxottica (Meta/Ray-Ban)";
        case 0x0BF3: return "DJI";
        case 0x004D: return "Parrot";
        default:     return "?";
    }
}

size_t extract_service_uuids(const scan::Frame& f, char* out, size_t out_sz) {
    size_t written = 0;
    bool   first   = true;
    auto emit = [&](uint16_t u) {
        char buf[10];
        int n = snprintf(buf, sizeof(buf), first ? "0x%04X" : ",0x%04X", u);
        if (n > 0 && written + (size_t)n < out_sz) {
            memcpy(out + written, buf, n);
            written += n;
            first = false;
        }
    };
    for_each_ad(f, [&](uint8_t type, const uint8_t* d, uint8_t dlen) -> bool {
        if (type == ad::INCOMPLETE_16BIT_UUIDS || type == ad::COMPLETE_16BIT_UUIDS) {
            for (size_t k = 0; k + 1 < dlen; k += 2) {
                uint16_t u = (uint16_t)d[k] | ((uint16_t)d[k+1] << 8);
                emit(u);
            }
        }
        return true;
    });
    if (written < out_sz) out[written] = 0;
    return written;
}

uint8_t traits(const scan::Frame& f) {
    uint8_t t = 0;
    if (f.tx_power != INT8_MIN) t |= TR_HAS_TXPOWER;
    if (f.ll_pdu_type == scan::LL_ADV_IND || f.ll_pdu_type == scan::LL_ADV_DIRECT_IND) {
        t |= TR_CONNECTABLE;
    }
    for_each_ad(f, [&](uint8_t type, const uint8_t*, uint8_t) -> bool {
        if (type == ad::COMPLETE_LOCAL_NAME || type == ad::SHORTENED_LOCAL_NAME) t |= TR_HAS_NAME;
        if (type == ad::MANUFACTURER_SPECIFIC)                                    t |= TR_HAS_MFR;
        if (type == ad::SERVICE_DATA_16 || type == ad::SERVICE_DATA_32 ||
            type == ad::SERVICE_DATA_128)                                         t |= TR_HAS_SVC_DATA;
        return true;
    });
    return t;
}

size_t format_line(const scan::Frame& f, char* out, size_t out_sz) {
    char addr[18]; format_addr(f.addr, addr);
    char name[32] = {0};
    extract_name(f, name, sizeof(name));
    char svc[80]; svc[0] = 0;
    extract_service_uuids(f, svc, sizeof(svc));
    uint16_t mfr = manufacturer_id(f);

    char chbuf[4];
    if (f.channel <= 39) snprintf(chbuf, sizeof(chbuf), "%u", f.channel);
    else                 strlcpy(chbuf, "?", sizeof(chbuf));

    size_t off = snprintf(out, out_sz, "[Ch%s RSSI%ddBm] %s %s:%s",
                          chbuf, (int)f.rssi,
                          ll_type_name(f.ll_pdu_type),
                          addr_type_short(f.addr_type),
                          addr);
    if (off >= out_sz) return out_sz - 1;

    if (name[0]) {
        int n = snprintf(out + off, out_sz - off, " name=\"%s\"", name);
        if (n > 0) off += (size_t)n;
    }
    if (svc[0] && off < out_sz) {
        int n = snprintf(out + off, out_sz - off, " svc=%s", svc);
        if (n > 0) off += (size_t)n;
    }
    if (mfr != 0xFFFF && off < out_sz) {
        int n = snprintf(out + off, out_sz - off, " mfr=%04X(%s)", mfr, mfr_shortname(mfr));
        if (n > 0) off += (size_t)n;
    }
    if (off < out_sz) {
        out[off++] = '\n';
        if (off < out_sz) out[off] = 0;
    }
    return off;
}

} // namespace text_summary

// ---------------------------------------------------------------------------
// pcap_stream - USB-CDC text output only.
// PCAP binary capture lives on the dashboard exclusively -- GET
// /api/session.pcap in the web_dashboard namespace below. session_pcap
// still uses nordic_pcap::build_frame for its in-PSRAM buffer. The USB
// CDC layer on ESP32-S3 could not be made reliable for high-rate binary
// streaming (residual byte-boundary corruption under load).
// ---------------------------------------------------------------------------
namespace pcap_stream {

void write_frame_text(const scan::Frame& f) {
    char line[320];
    size_t n = text_summary::format_line(f, line, sizeof(line));
    if (n > 0) Serial.write((const uint8_t*)line, n);
}

} // namespace pcap_stream

// ---------------------------------------------------------------------------
// session_pcap - in-PSRAM rolling pcap buffer for browser download
// ---------------------------------------------------------------------------
namespace session_pcap {

// Tiered PSRAM allocation. First entry that succeeds wins; nothing falls
// through to DRAM (that path OOM-crashed the ESP32 previously).
constexpr size_t CAP_TIERS[]    = { 6 * 1024 * 1024, 4 * 1024 * 1024, 2 * 1024 * 1024 };
constexpr size_t GLOBAL_HDR_LEN = 24;

enum class State : uint8_t {
    IDLE      = 0,
    RECORDING = 1,
    PAUSED    = 2,
    STOPPED   = 3
};

struct __attribute__((packed)) PcapGlobal {
    uint32_t magic;
    uint16_t vmaj, vmin;
    int32_t  tz;
    uint32_t sig;
    uint32_t snaplen;
    uint32_t linktype;
};

struct __attribute__((packed)) PcapRec {
    uint32_t ts_sec, ts_usec;
    uint32_t incl_len, orig_len;
};

uint8_t*             g_buf     = nullptr;
size_t               g_cap     = 0;
size_t               g_used    = 0;
uint32_t             g_dropped = 0;
SemaphoreHandle_t    g_lock    = nullptr;

volatile State       g_state     = State::IDLE;
volatile uint32_t    g_downloads = 0;

void write_global_header_locked() {
    PcapGlobal g{};
    g.magic    = nordic_pcap::PCAP_MAGIC;
    g.vmaj     = nordic_pcap::PCAP_VER_MAJOR;
    g.vmin     = nordic_pcap::PCAP_VER_MINOR;
    g.snaplen  = nordic_pcap::PCAP_SNAPLEN;
    g.linktype = nordic_pcap::PCAP_LINKTYPE;
    memcpy(g_buf, &g, sizeof(g));
    g_used = sizeof(g);
}

// Walk record boundaries forward until we've skipped at least bytes_to_drop.
size_t next_boundary_after_locked(size_t bytes_to_drop) {
    size_t o = GLOBAL_HDR_LEN;
    size_t dropped = 0;
    while (o + sizeof(PcapRec) <= g_used) {
        PcapRec rec;
        memcpy(&rec, g_buf + o, sizeof(rec));
        size_t rec_total = sizeof(rec) + rec.incl_len;
        if (o + rec_total > g_used) break;
        dropped += rec_total;
        o += rec_total;
        if (dropped >= bytes_to_drop) return o;
    }
    return o;
}

void reset_ring_locked() {
    write_global_header_locked();
    g_dropped = 0;
}

bool init() {
    if (g_buf) return true;
    g_lock = xSemaphoreCreateMutex();
    if (!g_lock) return false;

    for (size_t i = 0; i < sizeof(CAP_TIERS) / sizeof(CAP_TIERS[0]); ++i) {
        uint8_t* p = (uint8_t*)heap_caps_malloc(CAP_TIERS[i],
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (p) {
            g_buf = p;
            g_cap = CAP_TIERS[i];
            Serial.printf("[session_pcap] tier %u ok: %u bytes in PSRAM\n",
                          (unsigned)i, (unsigned)g_cap);
            break;
        }
        Serial.printf("[session_pcap] tier %u FAILED (%u bytes)\n",
                      (unsigned)i, (unsigned)CAP_TIERS[i]);
    }

    if (!g_buf) {
        Serial.println("[session_pcap] disabled -- no PSRAM tier available");
        vSemaphoreDelete(g_lock);
        g_lock = nullptr;
        return false;
    }

    xSemaphoreTake(g_lock, portMAX_DELAY);
    reset_ring_locked();
    g_state = State::IDLE;
    xSemaphoreGive(g_lock);
    return true;
}

State       state()      { return g_state; }
const char* state_name() {
    switch (g_state) {
        case State::IDLE:      return "idle";
        case State::RECORDING: return "recording";
        case State::PAUSED:    return "paused";
        case State::STOPPED:   return "stopped";
    }
    return "?";
}

bool cmd_record() {
    if (!g_buf || !g_lock) return false;
    if (g_state == State::RECORDING) return false;
    xSemaphoreTake(g_lock, portMAX_DELAY);
    reset_ring_locked();
    g_state = State::RECORDING;
    xSemaphoreGive(g_lock);
    return true;
}
bool cmd_pause() {
    if (!g_buf || !g_lock) return false;
    if (g_state != State::RECORDING) return false;
    g_state = State::PAUSED;
    return true;
}
bool cmd_resume() {
    if (!g_buf || !g_lock) return false;
    if (g_state != State::PAUSED) return false;
    g_state = State::RECORDING;
    return true;
}
bool cmd_stop() {
    if (!g_buf || !g_lock) return false;
    if (g_state != State::RECORDING && g_state != State::PAUSED) return false;
    g_state = State::STOPPED;
    return true;
}

void append(const scan::Frame& f) {
    if (!g_buf || !g_lock) return;
    if (g_state != State::RECORDING) return;

    static uint8_t stage[nordic_pcap::FRAME_OVERHEAD + scan::MAX_PAYLOAD];
    size_t body = nordic_pcap::build_frame(f, stage);
    const size_t rec_len = sizeof(PcapRec) + body;

    if (rec_len > g_cap - GLOBAL_HDR_LEN) return;

    xSemaphoreTake(g_lock, portMAX_DELAY);

    if (g_state != State::RECORDING) {
        xSemaphoreGive(g_lock);
        return;
    }

    if (g_used + rec_len > g_cap) {
        const size_t want_free = g_cap / 2;
        const size_t drop_to = next_boundary_after_locked(want_free);
        if (drop_to > GLOBAL_HDR_LEN && drop_to <= g_used) {
            const size_t moved = g_used - drop_to;
            memmove(g_buf + GLOBAL_HDR_LEN, g_buf + drop_to, moved);
            g_used = GLOBAL_HDR_LEN + moved;
            g_dropped++;
        } else {
            write_global_header_locked();
        }
    }

    PcapRec rec{};
    rec.ts_sec   = f.ts_sec;
    rec.ts_usec  = f.ts_usec;
    rec.incl_len = (uint32_t)body;
    rec.orig_len = (uint32_t)body;
    memcpy(g_buf + g_used, &rec, sizeof(rec));
    memcpy(g_buf + g_used + sizeof(rec), stage, body);
    g_used += rec_len;

    xSemaphoreGive(g_lock);
}

size_t   size()      { return g_used; }
size_t   capacity()  { return g_cap; }
uint32_t dropped()   { return g_dropped; }

// Downloads only permitted from STOPPED. read_chunk() takes the session
// mutex per chunk and memcpys straight out of the live ring; the STOPPED
// guarantee is what makes this safe -- no double buffer. If the state ever
// leaves STOPPED mid-download (Record is the only such transition, and it
// wipes the ring), read_chunk() returns 0 for the remainder so the response
// truncates cleanly. Record wins over an in-flight download rather than
// being refused -- fresh capture intent beats a stale byte stream.
size_t read_chunk(size_t offset, uint8_t* out, size_t len) {
    if (!g_buf || !g_lock) return 0;
    if (g_state != State::STOPPED) return 0;
    xSemaphoreTake(g_lock, portMAX_DELAY);
    size_t copied = 0;
    if (g_state == State::STOPPED && offset < g_used) {
        const size_t remain = g_used - offset;
        const size_t n = (len < remain) ? len : remain;
        memcpy(out, g_buf + offset, n);
        copied = n;
    }
    xSemaphoreGive(g_lock);
    return copied;
}

void download_begin() { g_downloads++; }
void download_end()   { if (g_downloads) g_downloads--; }
uint32_t downloads_in_flight() { return g_downloads; }

} // namespace session_pcap

// ---------------------------------------------------------------------------
// Embedded dashboard HTML (was dashboard_html.h in the standalone)
// ---------------------------------------------------------------------------
static const char BLESNIFF_INDEX_HTML[] PROGMEM = R"HTML(<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<title>OUI-SPY BLESNIFF</title>
<style>
  :root {
    --bg:        #0a0f14;
    --panel:     #10161d;
    --panel-2:   #161d25;
    --border:    #232d38;
    --border-2:  #1a222b;
    --text:      #d5dee8;
    --muted:     #7d8896;
    --dim:       #56616f;
    --accent:    #58a6ff;
    --good:      #3fb950;
    --warn:      #d29922;
    --bad:       #f85149;
    --purple:    #d2a8ff;
    --teal:      #56d4dd;
    --pink:      #ff7b72;
    --f-adv-ind:      #58a6ff;
    --f-adv-direct:   #a5d6ff;
    --f-adv-nonconn:  #56d4dd;
    --f-adv-scan:     #d2a8ff;
    --f-scan-req:     #d29922;
    --f-scan-rsp:     #d2a8ff;
    --f-connect-req:  #7ee787;
    --f-extended:     #d29922;
    --f-addr-pub:     #58a6ff;
    --f-addr-rnd-s:   #56d4dd;
    --f-addr-rnd-nrp: #d29922;
    --f-addr-rnd-rpa: #d2a8ff;
    --v-ring:    #58a6ff;
    --v-axon:    #d2a8ff;
    --v-flock:   #ff7b72;
    --v-dji:     #3fb950;
    --v-parrot:  #7ee787;
    --v-skydio:  #56d4dd;
    --v-meta:    #d29922;
  }
  * { box-sizing: border-box; -webkit-tap-highlight-color: transparent; }
  html, body {
    margin: 0; padding: 0;
    background: var(--bg);
    color: var(--text);
    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Inter, system-ui, sans-serif;
    font-size: 13px;
    line-height: 1.4;
    height: 100dvh;
    overflow: hidden;
  }
  a { color: var(--accent); text-decoration: none; }
  b { color: var(--text); font-weight: 600; }
  input, select, button { font-family: inherit; }
  input, select, button { color: var(--text); background: var(--panel-2); border: 1px solid var(--border); }
  ::-webkit-scrollbar { width: 10px; height: 10px; }
  ::-webkit-scrollbar-track { background: var(--bg); }
  ::-webkit-scrollbar-thumb { background: var(--border); }
  ::-webkit-scrollbar-thumb:hover { background: var(--accent); }

  .app {
    display: grid;
    grid-template-columns: 300px 1fr;
    grid-template-rows: auto 1fr auto;
    grid-template-areas:
      "topbar topbar"
      "rail   main"
      "footer footer";
    height: 100dvh;
  }

  .topbar {
    grid-area: topbar;
    display: grid;
    grid-template-columns: minmax(0, 1fr) auto;
    gap: 20px;
    align-items: center;
    padding: 10px 14px;
    background: var(--panel);
    border-bottom: 1px solid var(--border);
    overflow: hidden;
    min-width: 0;
  }
  .banner-wrap { overflow: hidden; min-width: 0; }
  .banner-stack { display: flex; flex-wrap: wrap; gap: 8px 20px; align-items: center; }
  .banner-stack .banner { flex: 0 0 auto; }
  .banner {
    font-family: ui-monospace, Menlo, Consolas, monospace;
    color: var(--accent);
    font-size: 6px;
    line-height: 1.2;
    white-space: pre;
    margin: 0;
    letter-spacing: -0.3px;
    display: block;
    max-width: 100%;
    overflow: hidden;
  }
  .banner-compact { display: none; color: var(--accent); font-family: ui-monospace, Menlo, monospace; letter-spacing: 3px; }
  .status {
    display: grid;
    grid-template-columns: auto auto;
    gap: 2px 12px;
    font-size: 11px;
    color: var(--muted);
    text-transform: uppercase;
    letter-spacing: 1px;
  }
  .status .v {
    font-family: ui-monospace, Menlo, monospace;
    color: var(--text);
    text-transform: none;
    letter-spacing: 0;
    text-align: right;
    font-variant-numeric: tabular-nums;
  }
  .status .v.good { color: var(--good); }
  .status .v.bad  { color: var(--bad); }

  .rail {
    grid-area: rail;
    background: var(--panel);
    border-right: 1px solid var(--border);
    overflow-y: auto;
  }
  .rail section {
    padding: 12px 14px;
    border-bottom: 1px solid var(--border-2);
  }
  .rail h3 {
    margin: 0 0 8px;
    font-size: 10px;
    letter-spacing: 2px;
    text-transform: uppercase;
    color: var(--muted);
    font-weight: 500;
  }
  .rail label { display: block; margin: 6px 0 3px; font-size: 11px; color: var(--muted); }
  .rail input[type=text], .rail input[type=password], .rail select, .rail input[type=number] {
    width: 100%; padding: 6px 8px;
    font-family: ui-monospace, Menlo, monospace; font-size: 12px; outline: none;
  }
  .rail input:focus, .rail select:focus { border-color: var(--accent); }
  .radio-row, .check-row { display: flex; gap: 12px; margin: 4px 0; align-items: center; flex-wrap: wrap; }
  .radio-row label, .check-row label { margin: 0; color: var(--text); font-size: 12px; cursor: pointer; }
  input[type=checkbox], input[type=radio] { accent-color: var(--accent); margin: 0; }
  .warn-banner {
    background: rgba(210,153,34,0.08); border: 1px solid var(--warn);
    color: var(--warn); padding: 6px 8px; margin: 6px 0; font-size: 11px;
  }
  .slider-row { display: flex; align-items: center; gap: 8px; }
  .slider-row input[type=range] { flex: 1; accent-color: var(--accent); }
  .slider-row .val {
    font-family: ui-monospace, Menlo, monospace; color: var(--text);
    font-size: 11px; min-width: 62px; text-align: right;
  }

  .main { grid-area: main; display: grid; grid-template-rows: auto 1fr auto; overflow: hidden; min-width: 0; }

  .toolbar {
    display: flex; align-items: center; gap: 6px;
    padding: 8px 14px; background: var(--panel);
    border-bottom: 1px solid var(--border);
    flex-wrap: wrap;
  }
  .toolbar input[type=text] {
    flex: 1 1 240px; padding: 6px 10px;
    font-family: ui-monospace, Menlo, monospace; font-size: 12px; outline: none;
  }
  .toolbar input[type=text]:focus { border-color: var(--accent); }
  .toolbar input[type=text]::placeholder { color: var(--dim); }
  .btn {
    padding: 6px 10px; font-size: 12px; cursor: pointer; white-space: nowrap;
    letter-spacing: 1px;
  }
  .btn:hover { border-color: var(--accent); color: var(--accent); }
  .btn.active { border-color: var(--accent); color: var(--accent); background: rgba(88,166,255,0.06); }
  .btn.paused { border-color: var(--warn); color: var(--warn); background: rgba(210,153,34,0.06); }
  .btn.danger { color: var(--bad); border-color: var(--bad); }
  .btn.settings { display: none; }

  .qf {
    background: var(--panel);
    border-top: 1px solid var(--border);
    padding: 4px 14px 4px;
    max-height: 150px;
    overflow-y: auto;
    scrollbar-gutter: stable;
  }
  .qf::-webkit-scrollbar { width: 8px; }
  .qf::-webkit-scrollbar-track { background: var(--panel); }
  .qf::-webkit-scrollbar-thumb { background: var(--border); }
  .qf-row {
    padding: 4px 0; border-top: 1px solid var(--border-2);
  }
  .qf-row:first-child { border-top: none; }
  .qf-row-toggle {
    display: grid; grid-template-columns: 14px 1fr auto;
    gap: 8px; align-items: center;
    width: 100%; padding: 4px 2px;
    background: transparent; border: none;
    cursor: pointer; text-align: left;
    color: var(--text); font: inherit;
  }
  .qf-row-toggle:hover .lbl { color: #fff; }
  .qf-row-toggle .caret {
    color: var(--muted); font-family: ui-monospace, monospace;
    font-size: 10px; line-height: 1;
    transition: transform 120ms ease;
    display: inline-block; width: 12px; text-align: center;
  }
  .qf-row.collapsed .qf-row-toggle .caret { transform: rotate(-90deg); color: var(--text); }
  .qf-row .lbl {
    font-size: 11px; letter-spacing: 2px; text-transform: uppercase;
    color: var(--text); font-weight: 600;
  }
  .qf-row-toggle .badge {
    color: var(--accent); background: rgba(88,166,255,0.10);
    border: 1px solid var(--accent);
    font-family: ui-monospace, Menlo, monospace;
    font-size: 10px; padding: 1px 7px;
    min-width: 20px; text-align: center;
    font-weight: 600;
    display: none;
  }
  .qf-row-toggle .badge.on { display: inline-block; }
  .qf-row-body {
    display: grid; grid-template-columns: 72px 1fr auto;
    gap: 12px; align-items: center;
    padding-top: 3px;
    max-height: 300px; overflow: hidden;
    transition: max-height 140ms ease, padding-top 140ms ease, opacity 100ms ease;
  }
  .qf-row.collapsed .qf-row-body {
    max-height: 0; padding-top: 0; opacity: 0; pointer-events: none;
  }
  .qf-row .chips {
    display: flex; flex-wrap: wrap; gap: 4px; align-items: center;
    grid-column: 2;
  }
  .qf-row .trail { display: flex; align-items: center; gap: 8px; color: var(--text); font-size: 11px; white-space: nowrap; grid-column: 3; }
  .chip {
    --c: var(--accent);
    background: transparent; border: 1px solid var(--muted);
    color: var(--text);
    padding: 4px 10px 4px 8px;
    font-size: 11px; font-weight: 500;
    font-family: ui-monospace, Menlo, monospace;
    letter-spacing: 1px; text-transform: uppercase;
    cursor: pointer;
    display: inline-flex; align-items: center; gap: 8px;
    transition: background 80ms ease, color 80ms ease, border-color 80ms ease;
  }
  .chip .ind {
    width: 8px; height: 8px; border: 1px solid var(--muted);
    background: transparent; display: inline-block; flex-shrink: 0;
    transition: background 80ms ease, border-color 80ms ease;
  }
  .chip .count {
    color: var(--muted); font-size: 10px;
    padding-left: 6px; border-left: 1px solid var(--border); margin-left: 2px;
    min-width: 24px; text-align: right;
    font-variant-numeric: tabular-nums;
  }
  .chip:hover { border-color: var(--text); color: #fff; }
  .chip:hover .ind { border-color: var(--text); }
  .chip:active { transform: translateY(1px); }
  .chip.on { background: rgba(88,166,255,0.10); border-color: var(--c); color: var(--c); }
  .chip.on .ind { background: var(--c); border-color: var(--c); }
  .chip.on .count { color: var(--c); border-left-color: var(--c); }
  .chip.danger { --c: var(--bad); }
  .chip.danger.on { background: rgba(248,81,73,0.10); }
  .chip.warn { --c: var(--warn); }
  .chip.warn.on { background: rgba(210,153,34,0.10); }
  .chip.good { --c: var(--good); }
  .chip.good.on { background: rgba(63,185,80,0.10); }
  .chip.clear { color: var(--dim); font-size: 10px; padding: 3px 8px; }
  .chip.clear .ind { display: none; }
  .chip.clear:hover { color: var(--bad); border-color: var(--bad); }

  .tablewrap {
    overflow-x: auto; overflow-y: auto;
    background: var(--bg);
    -webkit-overflow-scrolling: touch;
    min-width: 0;
  }
  table { min-width: 1000px; }
  table { width: 100%; border-collapse: collapse;
          font-family: ui-monospace, Menlo, Consolas, monospace; font-size: 12px; }
  thead th {
    position: sticky; top: 0;
    background: var(--panel); color: var(--muted);
    text-transform: uppercase; letter-spacing: 1px;
    font-weight: 500; font-size: 10px;
    text-align: left; padding: 6px 8px;
    border-bottom: 1px solid var(--border);
    white-space: nowrap;
  }
  tbody td {
    padding: 3px 8px; border-bottom: 1px solid var(--border-2);
    white-space: nowrap;
  }
  tbody tr { border-left: 2px solid transparent; }
  tbody tr:hover { background: var(--panel-2); }
  tbody tr.hit { background: rgba(88,166,255,0.03); border-left-color: var(--accent); }
  tbody td.n     { color: var(--dim); text-align: right; }
  tbody td.right { text-align: right; }
  tbody td.mac   { color: var(--muted); }
  tbody td.info  { color: var(--text); overflow: hidden; text-overflow: ellipsis; max-width: 260px; }
  tbody td.rssi.strong { color: var(--good); }
  tbody td.rssi.mid    { color: var(--warn); }
  tbody td.rssi.weak   { color: var(--bad); }
  tr.t-adv-ind      { color: var(--f-adv-ind); }
  tr.t-adv-direct   { color: var(--f-adv-direct); }
  tr.t-adv-nonconn  { color: var(--f-adv-nonconn); }
  tr.t-adv-scan     { color: var(--f-adv-scan); }
  tr.t-scan-req     { color: var(--f-scan-req); }
  tr.t-scan-rsp     { color: var(--f-scan-rsp); }
  tr.t-connect-req  { color: var(--f-connect-req); font-weight: 500; }
  tr.t-extended     { color: var(--f-extended); }
  tbody tr td.type  { font-weight: 500; }
  tbody td.atype {
    color: var(--muted); font-size: 10px;
    text-transform: uppercase; letter-spacing: 1px;
  }
  tbody td.atype.pub     { color: var(--f-addr-pub); }
  tbody td.atype.rnd-s   { color: var(--f-addr-rnd-s); }
  tbody td.atype.rnd-nrp { color: var(--f-addr-rnd-nrp); }
  tbody td.atype.rnd-rpa { color: var(--f-addr-rnd-rpa); }
  .tag {
    display: inline-block; padding: 1px 6px;
    border: 1px solid; font-size: 10px; letter-spacing: 1px;
    margin-right: 6px; text-transform: uppercase;
    color: var(--warn); border-color: var(--warn);
  }
  .tag.vendor { color: var(--accent); border-color: var(--accent); }

  .footer {
    grid-area: footer;
    display: flex; justify-content: space-between; align-items: center;
    padding: 5px 12px; background: var(--panel);
    border-top: 1px solid var(--border);
    font-family: ui-monospace, Menlo, monospace;
    font-size: 11px; color: var(--muted);
    gap: 12px;
  }
  .footer .right { display: flex; gap: 14px; flex-wrap: wrap; justify-content: flex-end; }
  .footer .v { color: var(--text); }
  .footer .v.good { color: var(--good); }
  .footer .v.bad  { color: var(--bad); }

  .scrim {
    position: fixed; inset: 0; background: rgba(0,0,0,0.55);
    opacity: 0; pointer-events: none;
    transition: opacity 0.15s ease; z-index: 40;
  }
  .scrim.open { opacity: 1; pointer-events: auto; }

  #save-status { font-family: ui-monospace, Menlo, monospace; font-size: 11px; color: var(--muted); }
  #save-status.ok { color: var(--good); }
  #save-status.err { color: var(--bad); }

  /* -- session control strip ---------------------------------------- */
  .sess {
    display: grid;
    grid-template-columns: auto 1fr auto;
    gap: 14px;
    align-items: center;
    padding: 8px 14px;
    background: var(--panel);
    border-bottom: 1px solid var(--border);
    min-width: 0;
  }
  .sess .info { display: flex; flex-direction: column; gap: 4px; min-width: 0; }
  .sess .badge {
    display: inline-flex; align-items: center; gap: 6px;
    padding: 4px 10px;
    font-family: ui-monospace, Menlo, monospace;
    font-size: 11px; font-weight: 700; letter-spacing: 2px;
    text-transform: uppercase;
    border: 1px solid;
    background: rgba(255,255,255,0.02);
    white-space: nowrap;
  }
  .sess .badge .dot { width: 8px; height: 8px; border-radius: 50%; display: inline-block; background: currentColor; }
  .sess .badge.idle      { color: #8892a0; border-color: #4a5568; }
  .sess .badge.recording {
    color: #ff2b3b; border-color: #ff2b3b;
    background: rgba(255,43,59,0.10);
    animation: pulse-rec 1.2s ease-in-out infinite;
    box-shadow: 0 0 12px rgba(255,43,59,0.35);
  }
  .sess .badge.paused    { color: #f6c05a; border-color: #f6c05a; background: rgba(246,192,90,0.10); }
  .sess .badge.stopped   { color: #4ecca3; border-color: #4ecca3; background: rgba(78,204,163,0.10);
                            box-shadow: 0 0 10px rgba(78,204,163,0.35); }
  @keyframes pulse-rec { 0% { opacity: 0.6; } 50% { opacity: 1.0; } 100% { opacity: 0.6; } }

  .sess .bar-wrap { display: flex; flex-direction: column; gap: 3px; min-width: 0; }
  .sess .bar { height: 10px; width: 100%; background: var(--panel-2);
               border: 1px solid var(--border); position: relative; overflow: hidden; }
  .sess .bar .fill { height: 100%; width: 0%; background: #4a5568;
                     transition: width 200ms ease, background 120ms ease; }
  .sess.state-recording .bar .fill { background: #ff2b3b; box-shadow: 0 0 10px rgba(255,43,59,0.55); }
  .sess.state-paused    .bar .fill { background: #f6c05a; }
  .sess.state-stopped   .bar .fill { background: #4ecca3; box-shadow: 0 0 10px rgba(78,204,163,0.45); }
  .sess.state-idle      .bar .fill { background: #4a5568; }
  .sess .readout {
    display: flex; gap: 14px; align-items: center;
    font-family: ui-monospace, Menlo, monospace;
    font-size: 11px; color: var(--muted);
    font-variant-numeric: tabular-nums;
  }
  .sess .readout .fill-txt { color: var(--text); }
  .sess .readout .drops    { color: var(--warn); }
  .sess .readout .drops.zero { color: var(--muted); }
  .sess .readout .mem      { color: var(--dim); }

  .sess .btns { display: flex; gap: 6px; flex-wrap: wrap; }
  .sess .sbtn {
    padding: 7px 12px;
    font-family: ui-monospace, Menlo, monospace;
    font-size: 12px; font-weight: 600; letter-spacing: 1px;
    background: var(--panel-2);
    border: 1px solid var(--border);
    color: var(--text);
    cursor: pointer;
    text-transform: uppercase;
    white-space: nowrap;
    transition: color 100ms ease, border-color 100ms ease, background 100ms ease;
  }
  .sess .sbtn:hover:not(:disabled) { border-color: var(--accent); color: var(--accent); }
  .sess .sbtn:disabled { opacity: 0.35; cursor: not-allowed; }
  .sess .sbtn.rec         { color: #ff2b3b; border-color: #ff2b3b; }
  .sess .sbtn.rec:hover:not(:disabled)   { background: rgba(255,43,59,0.10); color: #ff2b3b; }
  .sess .sbtn.pause       { color: #f6c05a; border-color: #f6c05a; }
  .sess .sbtn.pause:hover:not(:disabled) { background: rgba(246,192,90,0.10); color: #f6c05a; }
  .sess .sbtn.stop        { color: #d5dee8; border-color: #7d8896; }
  .sess .sbtn.stop:hover:not(:disabled)  { background: rgba(213,222,232,0.06); color: #fff; border-color: #d5dee8; }
  .sess .sbtn.save        { color: #4ecca3; border-color: #4ecca3; }
  .sess .sbtn.save:hover:not(:disabled)  { background: rgba(78,204,163,0.10); color: #4ecca3; }

  @media (max-width: 720px) {
    .sess { grid-template-columns: 1fr; }
    .sess .btns { justify-content: flex-start; }
  }

  @media (max-width: 900px) {
    .app { grid-template-columns: 240px 1fr; }
    .banner { font-size: 6px; }
  }
  @media (max-width: 720px) {
    .app {
      grid-template-columns: 1fr;
      grid-template-areas: "topbar" "main" "footer";
    }
    .banner-stack { flex-direction: column; align-items: flex-start; gap: 4px; }
    .banner { font-size: 6px; display: block; overflow: hidden; }
    .banner-compact { display: none; }
    .status { grid-template-columns: repeat(3, auto); font-size: 10px; gap: 2px 10px; }
    .topbar { padding: 8px 10px; }
    .toolbar { padding: 6px 10px; gap: 4px; }
    .btn.settings { display: inline-flex; }
    .qf-row-body { grid-template-columns: 1fr; padding-left: 22px; }
    .qf-row .chips { grid-column: 1; flex-wrap: wrap; gap: 3px; }
    .qf-row .trail { grid-column: 1; padding-left: 0; justify-content: flex-start; }
    .chip {
      padding: 3px 7px 3px 6px; font-size: 10px;
      gap: 5px; letter-spacing: 0.5px;
    }
    .chip .ind { width: 6px; height: 6px; }
    .chip .count { padding-left: 4px; min-width: 18px; font-size: 9px; }
    .rail {
      position: fixed; top: 0; left: 0; bottom: 0;
      width: 88vw; max-width: 340px;
      transform: translateX(-100%);
      transition: transform 0.18s ease;
      z-index: 50; border-right: 1px solid var(--border);
    }
    .rail.open { transform: translateX(0); }
    .footer { flex-wrap: wrap; font-size: 10px; padding: 4px 8px; }
    .btn { padding: 8px 12px; }
  }
  @media (max-width: 420px) {
    .status { grid-template-columns: repeat(2, auto); }
    .qf { padding: 4px 8px; max-height: 130px; }
    .qf-row .lbl { font-size: 10px; padding-left: 2px; }
    tbody td.info { max-width: 140px; }
    .banner { font-size: 4.5px; }
  }
</style>

<div class="app">

  <div class="topbar">
    <div class="banner-wrap">
      <div class="banner-stack">
        <pre class="banner banner-1">  .oooooo.   ooooo     ooo ooooo          .oooooo..o ooooooooo.   oooooo   oooo
 d8P'  `Y8b  `888'     `8' `888'         d8P'    `Y8 `888   `Y88.  `888.   .8'
888      888  888       8   888          Y88bo.       888   .d88'   `888. .8'
888      888  888       8   888           `"Y8888o.   888ooo88P'     `888.8'
888      888  888       8   888  8888888      `"Y88b  888             `888'
`88b    d88'  `88.    .8'   888          oo     .d8P  888              888
 `Y8bood8P'     `YbodP'    o888o         8""88888P'  o888o            o888o</pre>
        <pre class="banner banner-2">oooooooooo.  ooooo        oooooooooooo  .oooooo..o ooooo      ooo ooooo oooooooooooo oooooooooooo
`888'   `Y8b `888'        `888'     `8 d8P'    `Y8 `888b.     `8' `888' `888'     `8 `888'     `8
 888     888  888          888         Y88bo.       8 `88b.    8   888   888          888
 888oooo888'  888          888oooo8     `"Y8888o.   8   `88b.  8   888   888oooo8     888oooo8
 888    `88b  888          888    "         `"Y88b  8     `88b.8   888   888    "     888    "
 888    .88P  888       o  888       o oo     .d8P  8       `888   888   888          888
o888bood8P'  o888ooooood8 o888ooooood8 8""88888P'  o8o        `8  o888o o888o        o888o</pre>
      </div>
      <span class="banner-compact">OUI-SPY // BLESNIFF</span>
    </div>
    <div class="status">
      <span>Out</span><span class="v good" id="statOut">--</span>
      <span>Win</span><span class="v" id="statWin">--</span>
      <span>Int</span><span class="v" id="statInt">--</span>
      <span>Up</span><span class="v" id="statUp">--</span>
      <span>Pps</span><span class="v" id="statPps">0</span>
      <span>Hits</span><span class="v good" id="statHits">0</span>
      <span>Drop</span><span class="v" id="statDrop">0</span>
    </div>
  </div>

  <div class="scrim" id="scrim"></div>

  <aside class="rail" id="rail">

    <section>
      <h3>Output</h3>
      <div class="radio-row">
        <input type="radio" name="out" id="outPcap"><label for="outPcap">PCAP</label>
        <input type="radio" name="out" id="outText"><label for="outText">Text</label>
      </div>
    </section>

    <section>
      <h3>Scan</h3>
      <label>Window (ms) &mdash; radio-on time per interval</label>
      <div class="slider-row">
        <input type="range" min="10" max="2000" step="10" value="100" id="scanWin"/>
        <span class="val" id="scanWinVal">100 ms</span>
      </div>
      <label>Interval (ms) &mdash; period between windows</label>
      <div class="slider-row">
        <input type="range" min="20" max="4000" step="10" value="100" id="scanInt"/>
        <span class="val" id="scanIntVal">100 ms</span>
      </div>
      <div class="warn-banner" id="winWarn" style="display:none">Window must be &le; interval.</div>
    </section>

    <section>
      <h3>Advert types</h3>
      <div class="check-row">
        <input type="checkbox" id="ftAdvInd"><label for="ftAdvInd">ADV_IND</label>
        <input type="checkbox" id="ftAdvDirect"><label for="ftAdvDirect">DIRECT</label>
        <input type="checkbox" id="ftAdvNonconn"><label for="ftAdvNonconn">NONCONN</label>
        <input type="checkbox" id="ftScanRsp"><label for="ftScanRsp">SCAN_RSP</label>
        <input type="checkbox" id="ftAdvScan"><label for="ftAdvScan">ADV_SCAN</label>
      </div>
      <h3 style="margin-top:10px">Address types</h3>
      <div class="check-row">
        <input type="checkbox" id="ftAddrPub"><label for="ftAddrPub">Public</label>
        <input type="checkbox" id="ftAddrRnd"><label for="ftAddrRnd">Random</label>
      </div>
    </section>

    <section>
      <h3>Access Point</h3>
      <label>SSID</label>
      <input type="text" id="apSsid" maxlength="32" />
      <label>Password (8-63 chars)</label>
      <input type="text" id="apPass" maxlength="63" />
      <div style="margin-top:8px">
        <button class="btn" id="apSave">Save AP &amp; Reboot</button>
      </div>
    </section>

    <section>
      <div style="display:grid;grid-template-columns:1fr 1fr;gap:6px">
        <button class="btn" id="btnReboot">Reboot</button>
        <button class="btn danger" id="btnReset">Factory</button>
        <button class="btn" id="btnDiscard">Discard</button>
        <button class="btn active" id="btnSave">Apply</button>
      </div>
      <div style="margin-top:6px"><span id="save-status">--</span></div>
    </section>

  </aside>

  <div class="main">
    <div class="sess state-idle" id="sess">
      <div class="info">
        <span class="badge idle" id="sessBadge"><span class="dot"></span><span id="sessBadgeTxt">IDLE</span></span>
      </div>
      <div class="bar-wrap">
        <div class="bar"><div class="fill" id="sessFill"></div></div>
        <div class="readout">
          <span class="fill-txt"><b id="sessBytesTxt">0 B</b> / <b id="sessCapTxt">--</b> &mdash; <b id="sessPct">0%</b></span>
          <span class="drops zero">dropped: <b id="sessDrop">0</b></span>
          <span class="mem">psram <b id="sessPsram">--</b> &middot; heap <b id="sessHeap">--</b></span>
        </div>
      </div>
      <div class="btns">
        <button class="sbtn rec"   id="btnRecord"   title="Start recording">&#9679; RECORD</button>
        <button class="sbtn pause" id="btnPause"    title="Pause recording">&#9208; PAUSE</button>
        <button class="sbtn stop"  id="btnStop"     title="Stop and finalize">&#9209; STOP</button>
        <button class="sbtn save"  id="btnSavePcap" title="Download session PCAP">&#8681; SAVE PCAP</button>
      </div>
    </div>
    <div class="toolbar">
      <input type="text" id="filter" placeholder="filter -- rssi>-60 | addr:aa:bb | name:airtag | mfr:apple | free text" />
      <button class="btn settings" id="btnSettings">Settings</button>
      <button class="btn active" id="followBtn">Follow</button>
      <button class="btn active" id="pauseBtn">Running</button>
      <button class="btn" id="clearViewBtn">Clear</button>
      <button class="btn" id="snapBtn">CSV</button>
      <button class="btn danger" id="clearRingBtn">Clear ring</button>
    </div>

    <div class="tablewrap" id="tablewrap">
      <table>
        <thead>
          <tr>
            <th class="hide-sm" style="width:54px">#</th>
            <th class="hide-sm" style="width:88px">Time</th>
            <th style="width:32px">Ch</th>
            <th style="width:52px">RSSI</th>
            <th style="width:130px">Adv Type</th>
            <th class="hide-sm" style="width:82px">Addr Type</th>
            <th style="width:140px">Address</th>
            <th style="width:150px">Name</th>
            <th class="hide-sm" style="width:120px">Svc</th>
            <th class="hide-sm" style="width:110px">Mfr</th>
            <th style="width:52px">Len</th>
          </tr>
        </thead>
        <tbody id="rows"></tbody>
      </table>
    </div>

    <div class="qf" id="qf">
      <div class="qf-row" data-group="type">
        <button class="qf-row-toggle" type="button">
          <span class="caret">&#9662;</span>
          <span class="lbl">Type</span>
          <span class="badge" data-badge="type">0</span>
        </button>
        <div class="qf-row-body">
          <div class="chips">
            <button class="chip" data-key="adv_ind" data-group="type"><span class="ind"></span>ADV_IND<span class="count" id="c-adv_ind">0</span></button>
            <button class="chip" data-key="adv_direct" data-group="type"><span class="ind"></span>DIRECT<span class="count" id="c-adv_direct">0</span></button>
            <button class="chip" data-key="adv_nonconn" data-group="type"><span class="ind"></span>NONCONN<span class="count" id="c-adv_nonconn">0</span></button>
            <button class="chip" data-key="adv_scan" data-group="type"><span class="ind"></span>ADV_SCAN<span class="count" id="c-adv_scan">0</span></button>
            <button class="chip warn" data-key="scan_req" data-group="type"><span class="ind"></span>SCAN_REQ<span class="count" id="c-scan_req">0</span></button>
            <button class="chip" data-key="scan_rsp" data-group="type"><span class="ind"></span>SCAN_RSP<span class="count" id="c-scan_rsp">0</span></button>
            <button class="chip good" data-key="connect_req" data-group="type"><span class="ind"></span>CONNECT<span class="count" id="c-connect_req">0</span></button>
            <button class="chip warn" data-key="extended" data-group="type"><span class="ind"></span>EXTENDED<span class="count" id="c-extended">0</span></button>
          </div>
          <div class="trail"></div>
        </div>
      </div>
      <div class="qf-row" data-group="addr">
        <button class="qf-row-toggle" type="button">
          <span class="caret">&#9662;</span>
          <span class="lbl">Address</span>
          <span class="badge" data-badge="addr">0</span>
        </button>
        <div class="qf-row-body">
          <div class="chips">
            <button class="chip" data-key="a_pub" data-group="addr"><span class="ind"></span>Public<span class="count" id="c-a_pub">0</span></button>
            <button class="chip" data-key="a_rnd_s" data-group="addr"><span class="ind"></span>Random-Static<span class="count" id="c-a_rnd_s">0</span></button>
            <button class="chip warn" data-key="a_rnd_nrp" data-group="addr"><span class="ind"></span>Random-NRP<span class="count" id="c-a_rnd_nrp">0</span></button>
            <button class="chip" data-key="a_rnd_rpa" data-group="addr"><span class="ind"></span>Random-RPA<span class="count" id="c-a_rnd_rpa">0</span></button>
          </div>
          <div class="trail"></div>
        </div>
      </div>
      <div class="qf-row" data-group="traits">
        <button class="qf-row-toggle" type="button">
          <span class="caret">&#9662;</span>
          <span class="lbl">Traits</span>
          <span class="badge" data-badge="traits">0</span>
        </button>
        <div class="qf-row-body">
          <div class="chips">
            <button class="chip" data-key="has_name" data-group="traits"><span class="ind"></span>Has-name<span class="count" id="c-has_name">0</span></button>
            <button class="chip" data-key="has_mfr" data-group="traits"><span class="ind"></span>Has-mfr<span class="count" id="c-has_mfr">0</span></button>
            <button class="chip" data-key="has_svc" data-group="traits"><span class="ind"></span>Has-svc<span class="count" id="c-has_svc">0</span></button>
            <button class="chip" data-key="has_tx" data-group="traits"><span class="ind"></span>Has-tx-pwr<span class="count" id="c-has_tx">0</span></button>
            <button class="chip good" data-key="connectable" data-group="traits"><span class="ind"></span>Connectable<span class="count" id="c-connectable">0</span></button>
          </div>
          <div class="trail"></div>
        </div>
      </div>
      <div class="qf-row" data-group="vendor">
        <button class="qf-row-toggle" type="button">
          <span class="caret">&#9662;</span>
          <span class="lbl">Vendor</span>
          <span class="badge" data-badge="vendor">0</span>
        </button>
        <div class="qf-row-body">
          <div class="chips" id="vendorChips"></div>
          <div class="trail">
            <input type="checkbox" id="hitsOnly" />
            <label for="hitsOnly">Hits only</label>
            <button class="chip clear" id="clearChipsBtn">Clear all</button>
          </div>
        </div>
      </div>
    </div>
  </div>

  <div class="footer">
    <span>ouispy-blesniff <b id="fwVer">--</b></span>
    <span class="right">
      <span>ws <b class="v" id="wsState">--</b></span>
      <span>total <b id="totalPkts">0</b></span>
      <span>shown <b id="rowCount">0</b></span>
      <span>session <b id="sessBytes">0</b></span>
      <span>flt <b id="fltState">off</b></span>
    </span>
  </div>

</div>

<script>
(function(){
  const $ = (id) => document.getElementById(id);
  const rows = $('rows');
  const wrap = $('tablewrap');
  const MAX_ROWS = 500;

  // --- Rail drawer (mobile) --------------------------------------------
  function toggleRail(force) {
    const rail = $('rail'); const scrim = $('scrim');
    const on = force !== undefined ? force : !rail.classList.contains('open');
    rail.classList.toggle('open', on);
    scrim.classList.toggle('open', on);
  }
  $('btnSettings').onclick = () => toggleRail(true);
  $('scrim').onclick = () => toggleRail(false);

  // --- Vendor DB -------------------------------------------------------
  // Same OUI list as ouispy-pcap. Matched against the first 3 bytes of the
  // advertising address (the high three bytes when printed MSB-first).
  // Match on any of: MAC OUI, Bluetooth SIG company ID (in mfr data),
  // 16-bit service UUID, or a case-insensitive substring of the local name.
  // BLE MACs randomize often -- CID / UUID / name are the reliable signals.
  const VENDORS = [
    { id:'ring',   name:'RING',   color:'var(--v-ring)',
      ouis:['00:0d:c5','14:cc:20','a4:77:33','b0:09:da','7c:8c:6c'],
      cids:['0171'], svcs:[], names:['Ring'] },
    { id:'axon',   name:'AXON',   color:'var(--v-axon)',
      ouis:['00:25:df'],
      cids:['034d'], svcs:['fc81','fe6b','fe6c'], names:[] },
    { id:'flock',  name:'FLOCK',  color:'var(--v-flock)',
      ouis:['a4:cf:12','24:6f:28','3c:71:bf','48:e7:29','98:cd:ac'],
      cids:[], svcs:[], names:['Flock','Falcon','Raven'] },
    { id:'dji',    name:'DJI',    color:'var(--v-dji)',
      ouis:['0c:9a:e6','8c:58:23','04:a8:5a','58:b8:58','e4:7a:2c','60:60:1f','48:1c:b9','34:d2:62'],
      cids:['0bf3'], svcs:[], names:['DJI','Mavic','Phantom','Inspire'] },
    { id:'parrot', name:'PARROT', color:'var(--v-parrot)',
      ouis:['00:12:1c','00:26:7e','90:03:b7','90:3a:e6','a0:14:3d'],
      cids:['004d'], svcs:[], names:['Parrot','Anafi','Bebop'] },
    { id:'skydio', name:'SKYDIO', color:'var(--v-skydio)',
      ouis:['38:1d:14','24:69:8e'],
      cids:[], svcs:[], names:['Skydio'] },
    { id:'meta',   name:'META',   color:'var(--v-meta)',
      ouis:['7c:2a:9e','cc:66:0a','f4:03:43','5c:e9:1e','98:59:49','80:aa:1c','38:47:12','a4:c1:38','58:d5:6e','2c:41:a1','44:d9:e7','9c:d9:17'],
      cids:['0d53'], svcs:['fd5f'], names:['Ray-Ban','Wayfarer','Oakley Meta'] },
  ];
  const vendorEnabled = new Set(VENDORS.map(v => v.id));
  const vendorHitCounts = {};
  VENDORS.forEach(v => {
    vendorHitCounts[v.id] = 0;
    $('vendorChips').insertAdjacentHTML('beforeend',
      '<button class="chip on" data-vendor="'+v.id+'" style="border-color:'+v.color+';color:'+v.color+'">'+
        '<span class="ind" style="background:'+v.color+';border-color:'+v.color+'"></span>'+v.name+
        '<span class="count" id="count-'+v.id+'">0</span>'+
      '</button>');
  });
  $('vendorChips').querySelectorAll('.chip').forEach(chip => {
    chip.addEventListener('click', () => {
      const id = chip.dataset.vendor;
      if (vendorEnabled.has(id)) vendorEnabled.delete(id); else vendorEnabled.add(id);
      chip.classList.toggle('on', vendorEnabled.has(id));
      const ind = chip.querySelector('.ind');
      ind.style.background = vendorEnabled.has(id) ? chip.style.color : 'transparent';
      if (typeof updateGroupBadges === 'function') updateGroupBadges();
      applyFilter();
    });
  });
  // Any of: MAC OUI, Bluetooth SIG company ID, 16-bit service UUID, or
  // case-insensitive name substring. BLE MACs are usually randomized so
  // CID/UUID/name are what actually catch Meta glasses, Axon body cams, etc.
  function vendorFor(mac, cid, svcStr, name) {
    const prefix   = mac ? mac.slice(0, 8).toLowerCase() : '';
    const cidLower = cid ? cid.toLowerCase() : '';
    const svcs     = svcStr ? svcStr.toLowerCase().split(',').map(s => s.replace(/^0x/, '').trim()) : [];
    const nameL    = name ? name.toLowerCase() : '';
    for (const v of VENDORS) {
      if (!vendorEnabled.has(v.id)) continue;
      if (prefix && v.ouis.includes(prefix)) return v;
      if (cidLower && v.cids && v.cids.includes(cidLower)) return v;
      if (svcs.length && v.svcs && v.svcs.some(u => svcs.includes(u))) return v;
      if (nameL && v.names && v.names.some(n => nameL.includes(n.toLowerCase()))) return v;
    }
    return null;
  }

  // --- Advert type mapping ----------------------------------------------
  function classifyType(y) {
    y = (y || '').toUpperCase();
    const keys = [];
    let cls = '';
    if (y === 'ADV_IND')            { keys.push('adv_ind');     cls = 't-adv-ind'; }
    else if (y === 'ADV_DIRECT')    { keys.push('adv_direct');  cls = 't-adv-direct'; }
    else if (y === 'ADV_NONCONN')   { keys.push('adv_nonconn'); cls = 't-adv-nonconn'; }
    else if (y === 'ADV_SCAN')      { keys.push('adv_scan');    cls = 't-adv-scan'; }
    else if (y === 'SCAN_REQ')      { keys.push('scan_req');    cls = 't-scan-req'; }
    else if (y === 'SCAN_RSP')      { keys.push('scan_rsp');    cls = 't-scan-rsp'; }
    else if (y === 'CONNECT_REQ')   { keys.push('connect_req'); cls = 't-connect-req'; }
    else                            { keys.push('extended');    cls = 't-extended'; }
    return { keys, cls };
  }

  function classifyAddrType(a) {
    a = (a || '').toLowerCase();
    if (a === 'pub')     return 'a_pub';
    if (a === 'rnd-s')   return 'a_rnd_s';
    if (a === 'rnd-nrp') return 'a_rnd_nrp';
    if (a === 'rnd-rpa') return 'a_rnd_rpa';
    return null;
  }
  function addrTypeCls(a) {
    a = (a || '').toLowerCase();
    if (a === 'pub')     return 'pub';
    if (a === 'rnd-s')   return 'rnd-s';
    if (a === 'rnd-nrp') return 'rnd-nrp';
    if (a === 'rnd-rpa') return 'rnd-rpa';
    return '';
  }

  // Trait bits from firmware match text_summary::traits(): 1=name, 2=mfr, 4=svc, 8=tx, 16=connectable.
  const TR_HAS_NAME=1, TR_HAS_MFR=2, TR_HAS_SVC=4, TR_HAS_TX=8, TR_CONNECTABLE=16;

  // --- Rendering: streaming append, cap at MAX_ROWS -------------------
  let paused = false;
  let n = 0;
  let hits = 0;
  const chipCounts = {};
  function bumpChip(k) {
    chipCounts[k] = (chipCounts[k] || 0) + 1;
    const el = $('c-' + k);
    if (el) el.textContent = chipCounts[k];
  }

  function escapeHtml(s) {
    return String(s).replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
  }
  function rssiCls(r) {
    return r > -55 ? 'strong' : r > -75 ? 'mid' : 'weak';
  }
  function fmtTime(ms) { return (ms/1000).toFixed(3); }
  function fmtUptime(sec) {
    const d = Math.floor(sec/86400);
    const h = Math.floor((sec%86400)/3600);
    const m = Math.floor((sec%3600)/60);
    const s = sec%60;
    if (d) return d+'d '+h+'h';
    if (h) return String(h).padStart(2,'0')+':'+String(m).padStart(2,'0');
    return String(m).padStart(2,'0')+':'+String(s).padStart(2,'0');
  }
  function fmtBytes(b) {
    if (b < 1024) return b+' B';
    if (b < 1024*1024) return (b/1024).toFixed(1)+' KB';
    return (b/1024/1024).toFixed(2)+' MB';
  }

  function pushPacket(p) {
    const y    = p.y || '';
    const a    = p.a || '';
    const addr = p.m || '';
    const name = p.n || '';
    const svc  = p.s || '';
    const mfr  = p.v || '';
    const mfrHex = p.u || '';
    const tr   = p.f || 0;
    const ch   = (p.c == null || p.c < 0) ? '?' : p.c;

    const cls = classifyType(y);
    const keys = new Set(cls.keys);
    const ak = classifyAddrType(a); if (ak) keys.add(ak);
    if (tr & TR_HAS_NAME)     keys.add('has_name');
    if (tr & TR_HAS_MFR)      keys.add('has_mfr');
    if (tr & TR_HAS_SVC)      keys.add('has_svc');
    if (tr & TR_HAS_TX)       keys.add('has_tx');
    if (tr & TR_CONNECTABLE)  keys.add('connectable');
    keys.forEach(bumpChip);

    const vend = vendorFor(addr, mfrHex, svc, name);
    if (vend) {
      hits++;
      vendorHitCounts[vend.id]++;
      $('count-'+vend.id).textContent = vendorHitCounts[vend.id];
      $('statHits').textContent = hits;
    }

    n++;
    const rc = rssiCls(p.r);
    const vendorTag = vend
      ? '<span class="tag vendor" style="color:'+vend.color+';border-color:'+vend.color+'">'+vend.name+'</span>'
      : '';

    let mfrDisplay = '';
    if (mfrHex) {
      mfrDisplay = mfrHex + (mfr && mfr !== '?' ? ' ' + mfr : '');
    }

    const tr_el = document.createElement('tr');
    tr_el.className = cls.cls;
    if (vend) tr_el.classList.add('hit');
    tr_el.dataset.keys = [...keys].join(' ');
    tr_el.dataset.hit  = vend ? '1' : '0';
    tr_el.dataset.addr = addr.toLowerCase();
    tr_el.dataset.name = name.toLowerCase();
    tr_el.dataset.svc  = svc.toLowerCase();
    tr_el.dataset.mfr  = (mfrHex + ' ' + (mfr||'')).toLowerCase();
    tr_el.dataset.type = y.toLowerCase();
    tr_el.dataset.atype = a.toLowerCase();
    tr_el.dataset.rssi = String(p.r);
    tr_el.dataset.ch   = String(ch);
    tr_el.innerHTML =
      '<td class="n hide-sm">'+(p.i != null ? p.i : n)+'</td>'+
      '<td class="hide-sm">'+fmtTime(p.t || 0)+'</td>'+
      '<td>'+escapeHtml(String(ch))+'</td>'+
      '<td class="rssi '+rc+' right">'+p.r+'</td>'+
      '<td class="type">'+escapeHtml(y)+'</td>'+
      '<td class="atype '+addrTypeCls(a)+' hide-sm">'+escapeHtml(a)+'</td>'+
      '<td class="mac">'+vendorTag+escapeHtml(addr)+'</td>'+
      '<td class="info">'+escapeHtml(name)+'</td>'+
      '<td class="mac hide-sm">'+escapeHtml(svc)+'</td>'+
      '<td class="mac hide-sm">'+escapeHtml(mfrDisplay)+'</td>'+
      '<td class="right">'+p.l+'</td>';
    rows.appendChild(tr_el);
    while (rows.childElementCount > MAX_ROWS) rows.removeChild(rows.firstChild);
    applyRowFilter(tr_el);
  }

  // --- Follow (auto-scroll) --------------------------------------------
  let follow = true;
  let scrollByCode = false;
  function setFollow(on) {
    follow = on;
    const b = $('followBtn');
    b.textContent = on ? 'Follow' : 'Scroll to bottom to follow';
    b.classList.toggle('active', on);
    b.classList.toggle('paused', !on);
    if (on) {
      scrollByCode = true;
      requestAnimationFrame(() => { wrap.scrollTop = wrap.scrollHeight; });
    }
  }
  $('followBtn').onclick = () => setFollow(!follow);
  wrap.addEventListener('scroll', () => {
    if (scrollByCode) { scrollByCode = false; return; }
    const atBottom = (wrap.scrollHeight - wrap.scrollTop - wrap.clientHeight) <= 4;
    if (atBottom && !follow) setFollow(true);
    else if (!atBottom && follow) setFollow(false);
  }, { passive: true });

  const stickBottom = () => {
    if (!follow) return;
    scrollByCode = true;
    wrap.scrollTop = wrap.scrollHeight;
  };

  // --- Pause / Clear / Snapshot / Clear ring --------------------------
  $('pauseBtn').onclick = () => {
    paused = !paused;
    const b = $('pauseBtn');
    b.textContent = paused ? 'Paused' : 'Running';
    b.classList.toggle('active', !paused);
    b.classList.toggle('paused', paused);
  };
  $('clearViewBtn').onclick = () => {
    rows.innerHTML = ''; n = 0; hits = 0;
    $('statHits').textContent = '0';
    Object.keys(chipCounts).forEach(k => chipCounts[k] = 0);
    document.querySelectorAll('#qf .chip[data-key] .count').forEach(c => c.textContent = '0');
    VENDORS.forEach(v => { vendorHitCounts[v.id]=0; $('count-'+v.id).textContent='0'; });
    $('rowCount').textContent = '0';
  };
  $('clearRingBtn').onclick = async () => {
    try { await fetch('/api/clear', {method:'POST'}); } catch(e){}
  };

  // --- Session state machine (Record / Pause / Stop / Save) -----------
  let sessState = 'idle';
  let sessCap   = 0;
  function pretty(state) {
    if (state === 'recording') return 'RECORDING';
    if (state === 'paused')    return 'PAUSED';
    if (state === 'stopped')   return 'STOPPED';
    return 'IDLE';
  }
  function applySessState(state) {
    const s = state || 'idle';
    sessState = s;
    const sess = $('sess');
    sess.classList.remove('state-idle','state-recording','state-paused','state-stopped');
    sess.classList.add('state-' + s);
    const badge = $('sessBadge');
    badge.classList.remove('idle','recording','paused','stopped');
    badge.classList.add(s);
    $('sessBadgeTxt').textContent = pretty(s);
    const rec = $('btnRecord'), pause = $('btnPause'), stop = $('btnStop'), save = $('btnSavePcap');
    if (s === 'idle') {
      rec.innerHTML = '&#9679; RECORD';   rec.disabled = false;
      pause.disabled = true; stop.disabled = true; save.disabled = true;
    } else if (s === 'recording') {
      rec.innerHTML = '&#9679; RECORD';   rec.disabled = true;
      pause.disabled = false; stop.disabled = false; save.disabled = true;
    } else if (s === 'paused') {
      rec.innerHTML = '&#9654; RESUME';   rec.disabled = false;
      pause.disabled = true; stop.disabled = false; save.disabled = true;
    } else if (s === 'stopped') {
      rec.innerHTML = '&#9679; RE-RECORD'; rec.disabled = false;
      pause.disabled = true; stop.disabled = true; save.disabled = false;
    }
  }
  applySessState('idle');

  async function sessPost(path) {
    try { await fetch(path, {method:'POST'}); } catch(e){}
  }
  $('btnRecord').onclick = () => { applySessState('recording'); sessPost('/api/session/record'); };
  $('btnPause').onclick  = () => { applySessState('paused');    sessPost('/api/session/pause');  };
  $('btnStop').onclick   = () => { applySessState('stopped');   sessPost('/api/session/stop');   };
  $('btnSavePcap').onclick = () => {
    if (sessState !== 'stopped') return;
    const stamp = new Date().toISOString().replace(/[:.]/g,'-').slice(0,19);
    const a = document.createElement('a');
    a.href = '/api/session.pcap?ts=' + Date.now();
    a.download = 'ouispy-blesniff-' + stamp + '.pcap';
    document.body.appendChild(a);
    a.click();
    a.remove();
  };
  $('snapBtn').onclick = () => {
    const cols = ['idx','t_ms','ch','rssi','type','addr_type','address','name','svc','mfr','len'];
    const lines = [cols.join(',')];
    rows.querySelectorAll('tr').forEach(tr => {
      const c = tr.children;
      const q = (s) => '"'+String(s||'').replace(/"/g,'""')+'"';
      lines.push([c[0].textContent, c[1].textContent, c[2].textContent, c[3].textContent,
                  c[4].textContent, c[5].textContent, c[6].textContent,
                  q(c[7].textContent), q(c[8].textContent), q(c[9].textContent),
                  c[10].textContent].join(','));
    });
    const blob = new Blob([lines.join('\n')], {type:'text/csv'});
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = 'ouispy-blesniff-' + new Date().toISOString().replace(/[:.]/g,'-') + '.csv';
    a.click();
  };

  // --- Chip filter -----------------------------------------------------
  const activeChips = new Set();
  document.querySelectorAll('#qf .chip[data-key]').forEach(chip => {
    chip.addEventListener('click', () => {
      const key = chip.dataset.key;
      if (activeChips.has(key)) activeChips.delete(key);
      else activeChips.add(key);
      chip.classList.toggle('on', activeChips.has(key));
      updateGroupBadges();
      applyFilter();
    });
  });
  $('clearChipsBtn').onclick = () => {
    activeChips.clear();
    document.querySelectorAll('#qf .chip[data-key]').forEach(c => c.classList.remove('on'));
    if (typeof vendorEnabled !== 'undefined') {
      vendorEnabled.clear();
      VENDORS.forEach(v => vendorEnabled.add(v.id));
      document.querySelectorAll('#vendorChips .chip').forEach(c => {
        c.classList.add('on');
        const ind = c.querySelector('.ind');
        if (ind) ind.style.background = c.style.color;
      });
    }
    updateGroupBadges();
    applyFilter();
  };

  // --- Collapsible groups ---------------------------------------------
  document.querySelectorAll('.qf-row-toggle').forEach(tog => {
    tog.addEventListener('click', () => {
      tog.closest('.qf-row').classList.toggle('collapsed');
    });
  });
  function updateGroupBadges() {
    document.querySelectorAll('.qf-row').forEach(row => {
      const g = row.dataset.group;
      let count = 0;
      if (g === 'vendor') {
        count = (typeof vendorEnabled !== 'undefined') ? (VENDORS.length - vendorEnabled.size) : 0;
      } else {
        count = row.querySelectorAll('.chip[data-key].on').length;
      }
      const badge = row.querySelector('[data-badge]');
      if (badge) {
        badge.textContent = count;
        badge.classList.toggle('on', count > 0);
      }
    });
  }
  updateGroupBadges();
  $('hitsOnly').onchange = () => applyFilter();
  $('filter').oninput = () => applyFilter();

  function parseTextFilter() {
    const q = $('filter').value.trim().toLowerCase();
    return {
      q,
      rssi:  q.match(/rssi\s*([<>=])\s*(-?\d+)/),
      type:  q.match(/type:(\S+)/),
      addr:  q.match(/addr:([0-9a-f:]+)/),
      name:  q.match(/name:(\S+)/),
      svc:   q.match(/svc:(\S+)/),
      mfr:   q.match(/mfr:(\S+)/),
      ch:    q.match(/ch:(\d+)/),
      free:  q.replace(/rssi\s*[<>=]\s*-?\d+/g,'')
              .replace(/(type|addr|name|svc|mfr|ch):\S+/g,'').trim()
    };
  }
  function rowMatch(tr, f, hitsOnly) {
    if (activeChips.size > 0) {
      const rowKeys = (tr.dataset.keys || '').split(' ');
      if (!rowKeys.some(k => activeChips.has(k))) return false;
    }
    if (hitsOnly && tr.dataset.hit !== '1') return false;
    if (f.rssi) {
      const r = parseInt(tr.dataset.rssi, 10);
      const op = f.rssi[1], v = +f.rssi[2];
      if (op === '>' && !(r > v)) return false;
      if (op === '<' && !(r < v)) return false;
      if (op === '=' && r !== v) return false;
    }
    if (f.type && !tr.dataset.type.includes(f.type[1]))   return false;
    if (f.addr && !tr.dataset.addr.includes(f.addr[1]))   return false;
    if (f.name && !tr.dataset.name.includes(f.name[1]))   return false;
    if (f.svc  && !tr.dataset.svc.includes(f.svc[1]))     return false;
    if (f.mfr  && !tr.dataset.mfr.includes(f.mfr[1]))     return false;
    if (f.ch   && tr.dataset.ch !== f.ch[1])              return false;
    if (f.free) {
      const hay = tr.dataset.type+' '+tr.dataset.atype+' '+tr.dataset.addr+' '+
                  tr.dataset.name+' '+tr.dataset.svc+' '+tr.dataset.mfr+
                  ' ch'+tr.dataset.ch+' rssi'+tr.dataset.rssi;
      if (!hay.includes(f.free)) return false;
    }
    return true;
  }
  function applyRowFilter(tr) {
    const f = parseTextFilter();
    const hitsOnly = $('hitsOnly').checked;
    tr.style.display = rowMatch(tr, f, hitsOnly) ? '' : 'none';
  }
  function applyFilter() {
    const f = parseTextFilter();
    const hitsOnly = $('hitsOnly').checked;
    const any = activeChips.size || hitsOnly || f.rssi || f.type || f.addr ||
                f.name || f.svc || f.mfr || f.ch || f.free;
    $('fltState').textContent = any ? 'on' : 'off';
    let shown = 0;
    rows.querySelectorAll('tr').forEach(tr => {
      const on = rowMatch(tr, f, hitsOnly);
      tr.style.display = on ? '' : 'none';
      if (on) shown++;
    });
    $('rowCount').textContent = shown;
  }

  // --- Batched render: keep DOM writes cheap under load ---------------
  let pending = [];
  let flushScheduled = false;
  function scheduleFlush() {
    if (flushScheduled) return;
    flushScheduled = true;
    requestAnimationFrame(() => {
      flushScheduled = false;
      const batch = pending;
      pending = [];
      for (const p of batch) pushPacket(p);
      let shown = 0;
      rows.querySelectorAll('tr').forEach(tr => { if (tr.style.display !== 'none') shown++; });
      $('rowCount').textContent = shown;
      stickBottom();
    });
  }
  function ingest(p) {
    if (paused) return;
    pending.push(p);
    if (pending.length > 400) pending.splice(0, pending.length - 400);
    scheduleFlush();
  }

  // --- WebSocket -------------------------------------------------------
  let ws = null;
  function connectWS() {
    const url = (location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + '/ws';
    ws = new WebSocket(url);
    ws.onopen  = () => { $('wsState').textContent = 'connected'; $('wsState').className = 'v good'; };
    ws.onclose = () => { $('wsState').textContent = 'disconnected'; $('wsState').className = 'v bad';
                          setTimeout(connectWS, 2000); };
    ws.onerror = () => { $('wsState').textContent = 'error'; $('wsState').className = 'v bad'; };
    ws.onmessage = (ev) => {
      let msg; try { msg = JSON.parse(ev.data); } catch(e) { return; }
      if (msg.type === 'status') {
        $('statOut').textContent = msg.out || '--';
        $('statUp').textContent = fmtUptime(msg.uptime || 0);
        $('statPps').textContent = msg.pps || 0;
        const drop = (msg.dropped_pcap || 0) + (msg.dropped_dash || 0);
        $('statDrop').textContent = drop;
        $('statDrop').className = 'v' + (drop > 0 ? ' bad' : '');
        $('totalPkts').textContent = msg.total || 0;
        $('sessBytes').textContent = fmtBytes(msg.session_bytes || 0);
        $('fwVer').textContent = msg.fw || '--';

        applySessState(msg.state || 'idle');
        sessCap = msg.session_cap || 0;
        const sbytes = msg.session_bytes || 0;
        $('sessBytesTxt').textContent = fmtBytes(sbytes);
        $('sessCapTxt').textContent   = sessCap ? fmtBytes(sessCap) : '--';
        const pct = sessCap ? Math.min(100, (sbytes * 100 / sessCap)) : 0;
        $('sessPct').textContent = pct.toFixed(pct < 10 ? 1 : 0) + '%';
        $('sessFill').style.width = pct.toFixed(2) + '%';
        const sd = msg.session_drop || 0;
        $('sessDrop').textContent = sd;
        $('sessDrop').parentElement.classList.toggle('zero', sd === 0);
        if (msg.psram_free != null) $('sessPsram').textContent = fmtBytes(msg.psram_free);
        if (msg.heap_free  != null) $('sessHeap').textContent  = fmtBytes(msg.heap_free);
        return;
      }
      if (msg.type === 'pkts' && Array.isArray(msg.p)) {
        for (const p of msg.p) ingest(p);
      } else if (msg.type === 'pkt') {
        ingest(msg);
      }
    };
  }

  // --- Config load / save ---------------------------------------------
  function markDirty() {
    $('save-status').textContent = 'unsaved';
    $('save-status').className = '';
  }
  document.querySelectorAll('.rail input, .rail select').forEach(el => {
    el.addEventListener('change', markDirty);
    el.addEventListener('input',  markDirty);
  });

  function updateScanLabels() {
    $('scanWinVal').textContent = $('scanWin').value + ' ms';
    $('scanIntVal').textContent = $('scanInt').value + ' ms';
    const win = +$('scanWin').value;
    const intv = +$('scanInt').value;
    $('winWarn').style.display = (win > intv) ? '' : 'none';
  }
  $('scanWin').oninput = updateScanLabels;
  $('scanInt').oninput = updateScanLabels;

  async function loadConfig() {
    try {
      const r = await fetch('/api/config');
      const c = await r.json();
      $('outPcap').checked = c.out === 0;
      $('outText').checked = c.out === 1;
      $('scanWin').value = c.scan_win;
      $('scanInt').value = c.scan_int;
      updateScanLabels();
      $('ftAdvInd').checked      = (c.ftmask & 0x01) !== 0;
      $('ftAdvDirect').checked   = (c.ftmask & 0x02) !== 0;
      $('ftAdvNonconn').checked  = (c.ftmask & 0x04) !== 0;
      $('ftScanRsp').checked     = (c.ftmask & 0x08) !== 0;
      $('ftAdvScan').checked     = (c.ftmask & 0x10) !== 0;
      $('ftAddrPub').checked     = (c.ftmask & 0x20) !== 0;
      $('ftAddrRnd').checked     = (c.ftmask & 0x40) !== 0;
      $('apSsid').value = c.ap_ssid || '';
      $('apPass').value = c.ap_pass || '';
      $('statWin').textContent = c.scan_win + 'ms';
      $('statInt').textContent = c.scan_int + 'ms';
      $('save-status').textContent = 'saved';
      $('save-status').className = 'ok';
    } catch (e) {
      $('save-status').textContent = 'load failed';
      $('save-status').className = 'err';
    }
  }

  $('btnDiscard').onclick = () => loadConfig();

  $('btnSave').onclick = async () => {
    let ftmask = 0;
    if ($('ftAdvInd').checked)     ftmask |= 0x01;
    if ($('ftAdvDirect').checked)  ftmask |= 0x02;
    if ($('ftAdvNonconn').checked) ftmask |= 0x04;
    if ($('ftScanRsp').checked)    ftmask |= 0x08;
    if ($('ftAdvScan').checked)    ftmask |= 0x10;
    if ($('ftAddrPub').checked)    ftmask |= 0x20;
    if ($('ftAddrRnd').checked)    ftmask |= 0x40;
    const body = {
      out:      $('outPcap').checked ? 0 : 1,
      scan_win: parseInt($('scanWin').value, 10),
      scan_int: parseInt($('scanInt').value, 10),
      ftmask:   ftmask
    };
    $('save-status').textContent = 'applying...';
    $('save-status').className = '';
    try {
      const r = await fetch('/api/config', {
        method: 'POST',
        headers: {'content-type':'application/json'},
        body: JSON.stringify(body)
      });
      if (r.ok) {
        $('save-status').textContent = 'applied';
        $('save-status').className = 'ok';
        $('statWin').textContent = body.scan_win + 'ms';
        $('statInt').textContent = body.scan_int + 'ms';
      } else {
        $('save-status').textContent = 'error';
        $('save-status').className = 'err';
      }
    } catch(e) {
      $('save-status').textContent = 'error';
      $('save-status').className = 'err';
    }
  };

  $('apSave').onclick = async () => {
    const body = { ssid: $('apSsid').value, pass: $('apPass').value };
    try {
      const r = await fetch('/api/ap', {
        method: 'POST',
        headers: {'content-type':'application/json'},
        body: JSON.stringify(body)
      });
      if (r.ok) setTimeout(() => fetch('/api/reboot', {method:'POST'}), 300);
    } catch(e){}
  };
  $('btnReboot').onclick = async () => {
    try { await fetch('/api/reboot', {method:'POST'}); } catch(e){}
  };
  $('btnReset').onclick = async () => {
    if (!confirm('Factory reset all settings and reboot?')) return;
    try { await fetch('/api/reset', {method:'POST'}); } catch(e){}
  };

  loadConfig();
  connectWS();
})();
</script>
)HTML";

// ---------------------------------------------------------------------------
// web_dashboard - AsyncWebServer + WebSocket streamer + REST config
// ---------------------------------------------------------------------------
namespace web_dashboard {

AsyncWebServer   server(80);
AsyncWebSocket   ws("/ws");
TaskHandle_t     dash_task_h = nullptr;
uint32_t         boot_ms = 0;

size_t append_pkt_json(const scan::Frame& f, char* out, size_t cap) {
    char addr[18];
    text_summary::format_addr(f.addr, addr);
    char name[32] = {0};
    text_summary::extract_name(f, name, sizeof(name));
    char svc[80]; svc[0] = 0;
    text_summary::extract_service_uuids(f, svc, sizeof(svc));
    uint16_t mfr = text_summary::manufacturer_id(f);
    uint8_t  tr  = text_summary::traits(f);

    StaticJsonDocument<512> doc;
    doc["i"] = f.idx;
    doc["t"] = (uint32_t)(millis() - boot_ms);
    doc["c"] = (int)(f.channel <= 39 ? f.channel : -1);
    doc["r"] = (int)f.rssi;
    if (f.tx_power != INT8_MIN) doc["x"] = (int)f.tx_power;
    doc["y"] = text_summary::ll_type_name(f.ll_pdu_type);
    doc["a"] = text_summary::addr_type_short(f.addr_type);
    doc["m"] = addr;
    doc["l"] = f.payload_len;
    doc["f"] = tr;
    if (name[0]) doc["n"] = name;
    if (svc[0])  doc["s"] = svc;
    if (mfr != 0xFFFF) {
        char mbuf[16];
        snprintf(mbuf, sizeof(mbuf), "%04X", mfr);
        doc["u"] = mbuf;
        doc["v"] = text_summary::mfr_shortname(mfr);
    }

    size_t n = measureJson(doc);
    if (n + 2 > cap) return 0;
    return serializeJson(doc, out, cap);
}

void send_status() {
    if (ws.count() == 0) return;
    StaticJsonDocument<512> doc;
    doc["type"] = "status";
    doc["uptime"] = (uint32_t)((millis() - boot_ms) / 1000);
    doc["pps"]    = scan::adverts_per_sec();
    doc["total"]  = scan::total_adverts();
    doc["dropped_pcap"] = scan::dropped_pcap();
    doc["dropped_dash"] = scan::dropped_dash();
    doc["session_bytes"] = (uint32_t)session_pcap::size();
    doc["session_cap"]   = (uint32_t)session_pcap::capacity();
    doc["session_drop"]  = (uint32_t)session_pcap::dropped();
    doc["state"]         = session_pcap::state_name();
    doc["psram_free"]    = (uint32_t)ESP.getFreePsram();
    doc["heap_free"]     = (uint32_t)ESP.getFreeHeap();
    doc["fw"] = config::FW_VERSION();

    char buf[512];
    size_t n = serializeJson(doc, buf, sizeof(buf));
    ws.textAll(buf, n);
}

static constexpr size_t BATCH_CAP        = 8192;
static constexpr size_t BATCH_FLUSH_WATER = 6144;
static constexpr uint32_t BATCH_TICK_MS  = 30;
static constexpr int MAX_DRAIN_PER_TICK  = 120;

void flush_batch(char* buf, size_t& pos, uint16_t& count) {
    if (count == 0) return;
    buf[pos++] = ']';
    buf[pos++] = '}';
    if (ws.count() > 0 && ws.availableForWriteAll()) {
        ws.textAll(buf, pos);
    }
    pos = 0;
    count = 0;
}

void begin_batch(char* buf, size_t& pos) {
    memcpy(buf, "{\"type\":\"pkts\",\"p\":[", 20);
    pos = 20;
}

void dashboard_task(void*) {
    uint32_t last_status = 0;
    uint32_t last_flush  = 0;
    static char batch[BATCH_CAP];
    size_t pos = 0;
    uint16_t count = 0;
    begin_batch(batch, pos);

    for (;;) {
        scan::Frame f;
        int drained = 0;
        while (drained < MAX_DRAIN_PER_TICK && scan::pop_dashboard(&f)) {
            if (count > 0) {
                if (pos + 1 >= BATCH_CAP) break;
                batch[pos++] = ',';
            }
            size_t n = append_pkt_json(f, batch + pos, BATCH_CAP - pos - 2);
            if (n == 0) {
                if (count > 0) pos--;
                break;
            }
            pos += n;
            count++;
            drained++;
            if (pos >= BATCH_FLUSH_WATER) break;
        }

        uint32_t now = millis();
        bool tick_expired = (now - last_flush) >= BATCH_TICK_MS;
        if (count > 0 && (tick_expired || pos >= BATCH_FLUSH_WATER)) {
            flush_batch(batch, pos, count);
            begin_batch(batch, pos);
            last_flush = now;
        }

        if (now - last_status > 1000) {
            send_status();
            ws.cleanupClients();
            last_status = now;
        }
        vTaskDelay(pdMS_TO_TICKS(drained >= MAX_DRAIN_PER_TICK ? 2 : 20));
    }
}

void handle_get_config(AsyncWebServerRequest* req) {
    StaticJsonDocument<512> doc;
    const auto& c = config::get();
    doc["scan_win"] = c.scan_window_ms;
    doc["scan_int"] = c.scan_interval_ms;
    doc["ftmask"]   = c.ft_mask;
    doc["ap_ssid"]  = c.ap_ssid;
    doc["ap_pass"]  = c.ap_pass;
    String body;
    serializeJson(doc, body);
    req->send(200, "application/json", body);
}

bool accumulate_body(AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
    if (total == 0 || total > 8192) return false;
    if (index == 0 && req->_tempObject == nullptr) {
        req->_tempObject = malloc(total);
    }
    if (req->_tempObject == nullptr) return false;
    memcpy((uint8_t*)req->_tempObject + index, data, len);
    return (index + len) >= total;
}

void handle_post_config(AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
    if (!accumulate_body(req, data, len, index, total)) return;
    StaticJsonDocument<640> doc;
    DeserializationError err = deserializeJson(doc, (const uint8_t*)req->_tempObject, total);
    if (err) { req->send(400, "application/json", "{\"error\":\"json\"}"); return; }

    bool need_apply_scan = false;

    if (doc.containsKey("scan_win")) {
        uint16_t v = doc["scan_win"];
        if (v != config::get().scan_window_ms) { config::set_scan_window(v); need_apply_scan = true; }
    }
    if (doc.containsKey("scan_int")) {
        uint16_t v = doc["scan_int"];
        if (v != config::get().scan_interval_ms) { config::set_scan_interval(v); need_apply_scan = true; }
    }
    if (doc.containsKey("ftmask")) {
        uint8_t m = doc["ftmask"];
        if (m != config::get().ft_mask) { config::set_ftmask(m); }
    }

    if (need_apply_scan) scan::apply_scan_params();

    req->send(200, "application/json", "{\"ok\":true}");
}

void handle_post_ap(AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
    if (!accumulate_body(req, data, len, index, total)) return;
    StaticJsonDocument<256> doc;
    if (deserializeJson(doc, (const uint8_t*)req->_tempObject, total)) { req->send(400); return; }
    const char* ssid = doc["ssid"] | "";
    const char* pass = doc["pass"] | "";
    config::set_ap(ssid, pass);
    req->send(200, "application/json", "{\"ok\":true}");
}

void handle_reboot(AsyncWebServerRequest* req) {
    req->send(200, "application/json", "{\"ok\":true}");
    delay(200);
    ESP.restart();
}

void handle_reset(AsyncWebServerRequest* req) {
    config::reset_defaults();
    req->send(200, "application/json", "{\"ok\":true}");
    delay(200);
    ESP.restart();
}

void handle_clear(AsyncWebServerRequest* req) {
    scan::clear_ring();
    req->send(200, "application/json", "{\"ok\":true}");
}

// -- Session state machine endpoints -----------------------------------------
// Legal transitions:
//   IDLE|PAUSED|STOPPED -> RECORDING   (via /api/session/record; clears ring)
//   RECORDING           -> PAUSED      (via /api/session/pause)
//   PAUSED              -> RECORDING   (via /api/session/resume)
//   RECORDING|PAUSED    -> STOPPED     (via /api/session/stop)
// Illegal transitions return HTTP 409.

const char* target_name_for(const char* which) {
    if (!strcmp(which, "record") || !strcmp(which, "resume")) return "recording";
    if (!strcmp(which, "pause"))  return "paused";
    if (!strcmp(which, "stop"))   return "stopped";
    return "?";
}

void reply_transition(AsyncWebServerRequest* req, bool ok, const char* which, const char* from) {
    if (ok) {
        char body[80];
        snprintf(body, sizeof(body), "{\"ok\":true,\"state\":\"%s\"}",
                 session_pcap::state_name());
        req->send(200, "application/json", body);
    } else {
        char body[160];
        snprintf(body, sizeof(body),
                 "{\"error\":\"invalid transition\",\"from\":\"%s\",\"to\":\"%s\"}",
                 from, target_name_for(which));
        req->send(409, "application/json", body);
    }
}

void handle_session_record(AsyncWebServerRequest* req) {
    const char* from = session_pcap::state_name();
    reply_transition(req, session_pcap::cmd_record(), "record", from);
}
void handle_session_pause(AsyncWebServerRequest* req) {
    const char* from = session_pcap::state_name();
    reply_transition(req, session_pcap::cmd_pause(),  "pause",  from);
}
void handle_session_resume(AsyncWebServerRequest* req) {
    const char* from = session_pcap::state_name();
    reply_transition(req, session_pcap::cmd_resume(), "resume", from);
}
void handle_session_stop(AsyncWebServerRequest* req) {
    const char* from = session_pcap::state_name();
    reply_transition(req, session_pcap::cmd_stop(),   "stop",   from);
}

void handle_session_pcap(AsyncWebServerRequest* req) {
    // Download only permitted from STOPPED. No snapshot buffer -- the STOPPED
    // guarantee is what makes reading straight out of the live ring safe.
    // If state slips out of STOPPED mid-download, read_chunk() returns 0 and
    // the chunked response truncates cleanly.
    if (session_pcap::state() != session_pcap::State::STOPPED) {
        req->send(409, "application/json",
            "{\"error\":\"invalid transition\",\"detail\":\"download only allowed from stopped\"}");
        return;
    }
    if (session_pcap::size() <= session_pcap::GLOBAL_HDR_LEN) {
        req->send(204, "application/vnd.tcpdump.pcap", "");
        return;
    }

    session_pcap::download_begin();
    AsyncWebServerResponse* r = req->beginChunkedResponse(
        "application/vnd.tcpdump.pcap",
        [](uint8_t* buf, size_t maxLen, size_t index) -> size_t {
            constexpr size_t CHUNK = 4096;
            size_t want = maxLen < CHUNK ? maxLen : CHUNK;
            size_t got = session_pcap::read_chunk(index, buf, want);
            if (got == 0) session_pcap::download_end();
            return got;
        });
    char filename[64];
    snprintf(filename, sizeof(filename), "attachment; filename=\"ouispy-blesniff-%lu.pcap\"",
             (unsigned long)(millis() / 1000));
    r->addHeader("Content-Disposition", filename);
    r->addHeader("Cache-Control", "no-store");
    req->send(r);
}

uint32_t connected_clients() { return ws.count(); }

bool init() {
    boot_ms = millis();

    server.on("/", HTTP_GET, [](AsyncWebServerRequest* req){
        AsyncWebServerResponse* r = req->beginResponse_P(200, "text/html",
            (const uint8_t*)BLESNIFF_INDEX_HTML, strlen_P(BLESNIFF_INDEX_HTML));
        r->addHeader("Cache-Control", "no-store");
        req->send(r);
    });
    server.on("/api/config", HTTP_GET, handle_get_config);
    server.on("/api/config", HTTP_POST,
        [](AsyncWebServerRequest* req){}, nullptr, handle_post_config);
    server.on("/api/ap", HTTP_POST,
        [](AsyncWebServerRequest* req){}, nullptr, handle_post_ap);
    server.on("/api/reboot", HTTP_POST, handle_reboot);
    server.on("/api/reset", HTTP_POST, handle_reset);
    server.on("/api/clear", HTTP_POST, handle_clear);
    server.on("/api/session.pcap", HTTP_GET, handle_session_pcap);
    server.on("/api/session/record", HTTP_POST, handle_session_record);
    server.on("/api/session/pause",  HTTP_POST, handle_session_pause);
    server.on("/api/session/resume", HTTP_POST, handle_session_resume);
    server.on("/api/session/stop",   HTTP_POST, handle_session_stop);

    server.onNotFound([](AsyncWebServerRequest* req){ req->send(404, "text/plain", "not found"); });

    server.addHandler(&ws);
    server.begin();

    xTaskCreatePinnedToCore(&dashboard_task, "bls_dash", 10240, nullptr, 3, &dash_task_h, 1);
    return true;
}

void tick() {
    ws.cleanupClients();
}

} // namespace web_dashboard

// ---------------------------------------------------------------------------
// Buzzer + LED + PCAP writer task (was main.cpp in the standalone)
// ---------------------------------------------------------------------------
enum class Fault { None, Wifi, Scan };
volatile Fault   g_fault = Fault::None;
TaskHandle_t     pcap_task_h = nullptr;
TaskHandle_t     led_task_h  = nullptr;
volatile uint32_t last_advert_ms = 0;

void blesniff_buzzer_setup() {
    ledc_timer_config_t t = {};
    t.speed_mode      = LEDC_LOW_SPEED_MODE;
    t.duty_resolution = LEDC_TIMER_10_BIT;
    t.timer_num       = BLESNIFF_BUZZER_TIMER;
    t.freq_hz         = 1500;
    t.clk_cfg         = LEDC_AUTO_CLK;
    ledc_timer_config(&t);

    ledc_channel_config_t c = {};
    c.gpio_num   = BLESNIFF_BUZZER_PIN;
    c.speed_mode = LEDC_LOW_SPEED_MODE;
    c.channel    = BLESNIFF_BUZZER_CH;
    c.timer_sel  = BLESNIFF_BUZZER_TIMER;
    c.duty       = 0;
    c.hpoint     = 0;
    ledc_channel_config(&c);
}

void blesniff_buzzer_chirp(uint16_t freq, uint16_t ms) {
    ledc_set_freq(LEDC_LOW_SPEED_MODE, BLESNIFF_BUZZER_TIMER, freq);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BLESNIFF_BUZZER_CH, 512);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BLESNIFF_BUZZER_CH);
    delay(ms);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BLESNIFF_BUZZER_CH, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BLESNIFF_BUZZER_CH);
}

// Active-low GPIO21 discrete LED (matches sibling modes). Solid on for fault;
// otherwise brief pulse on each advert plus a slow heartbeat when idle.
void led_task(void*) {
    uint32_t last_pkt_seen = 0;
    uint32_t on_until = 0;
    for (;;) {
        uint32_t now = millis();
        bool on = false;
        if (g_fault != Fault::None) {
            on = true;
        } else {
            uint32_t pkt_ms = last_advert_ms;
            if (pkt_ms != last_pkt_seen) {
                last_pkt_seen = pkt_ms;
                on_until = now + 30;
            }
            if (now < on_until) on = true;
            else if ((now / 1000) % 3 == 0 && (now % 1000) < 60) on = true;
        }
        digitalWrite(BLESNIFF_LED_PIN, on ? LOW : HIGH);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void pcap_writer_task(void*) {
    // USB output is text only. Full PCAP capture lives on the dashboard
    // (GET /api/session.pcap). session_pcap::append fills the download
    // buffer for every frame regardless of USB text emission.
    for (;;) {
        scan::Frame f;
        int drained = 0;
        while (drained < 16 && scan::pop_pcap(&f)) {
            last_advert_ms = millis();
            pcap_stream::write_frame_text(f);
            session_pcap::append(f);
            drained++;
        }
        vTaskDelay(pdMS_TO_TICKS(drained ? 2 : 10));
    }
}

// USB-CDC command protocol - same shape as the standalone.
String upper_s(const String& s) { String o = s; o.toUpperCase(); return o; }
void   reply_ok()               { Serial.println(F("OK")); }
void   reply_err(const char* m) { Serial.print(F("ERR ")); Serial.println(m); }

void handle_serial_cmd(const String& raw) {
    String line = raw; line.trim();
    if (!line.startsWith("CMD:") && !line.startsWith("cmd:")) return;
    String body = line.substring(4);
    body.trim();
    String U = upper_s(body);

    if (U == "STATUS") {
        IPAddress ip = WiFi.softAPIP();
        String apmac = WiFi.softAPmacAddress();
        Serial.printf("{\"scan_win\":%u,\"scan_int\":%u,\"ftmask\":\"0x%02x\","
            "\"total\":%u,\"pps\":%u,\"drop_pcap\":%u,\"drop_dash\":%u,\"fw\":\"%s\","
            "\"ap_ssid\":\"%s\",\"ap_ip\":\"%s\",\"ap_mac\":\"%s\",\"ap_stations\":%u,"
            "\"session_bytes\":%u,\"session_cap\":%u,\"session_drop\":%u,"
            "\"state\":\"%s\",\"psram_free\":%u,\"heap_free\":%u}\n",
            (unsigned)config::get().scan_window_ms,
            (unsigned)config::get().scan_interval_ms,
            (unsigned)config::get().ft_mask,
            (unsigned)scan::total_adverts(),
            (unsigned)scan::adverts_per_sec(),
            (unsigned)scan::dropped_pcap(),
            (unsigned)scan::dropped_dash(),
            config::FW_VERSION(),
            config::get().ap_ssid, ip.toString().c_str(), apmac.c_str(),
            (unsigned)WiFi.softAPgetStationNum(),
            (unsigned)session_pcap::size(),
            (unsigned)session_pcap::capacity(),
            (unsigned)session_pcap::dropped(),
            session_pcap::state_name(),
            (unsigned)ESP.getFreePsram(),
            (unsigned)ESP.getFreeHeap());
        return;
    }
    if (U == "VERSION") {
        Serial.printf("OUI-SPY BLESNIFF %s built %s %s\n", config::FW_VERSION(), __DATE__, __TIME__);
        return;
    }
    if (U.startsWith("MODE ")) {
        // Kept as a compatibility no-op. USB output is text only; PCAP
        // binary capture lives on the dashboard at /api/session.pcap.
        reply_ok();
        return;
    }
    if (U.startsWith("WINDOW ")) {
        int v = U.substring(7).toInt();
        if (v < 10 || v > 2000) { reply_err("bad window"); return; }
        config::set_scan_window((uint16_t)v);
        scan::apply_scan_params();
        reply_ok(); return;
    }
    if (U.startsWith("INTERVAL ")) {
        int v = U.substring(9).toInt();
        if (v < 20 || v > 4000) { reply_err("bad interval"); return; }
        config::set_scan_interval((uint16_t)v);
        scan::apply_scan_params();
        reply_ok(); return;
    }
    reply_err("unknown");
}

void serial_pump() {
    static String line;
    while (Serial.available()) {
        int c = Serial.read();
        if (c < 0) break;
        if (c == '\n' || c == '\r') {
            if (line.length()) { handle_serial_cmd(line); line = ""; }
        } else {
            if (line.length() < 200) line += (char)c;
        }
    }
}

bool wifi_ap_start() {
    WiFi.mode(WIFI_AP);
    // BLE + Wi-Fi coexistence: NimBLE brings up the BT controller which shares
    // the 2.4 GHz radio. AsyncWebServer + softAP work fine alongside NimBLE
    // scan on ESP32-S3 as long as we don't pin either to an aggressive
    // channel. Channel 1 default is a reasonable choice.
    return WiFi.softAP(config::get().ap_ssid, config::get().ap_pass, 1, 0, 4);
}

// ---------------------------------------------------------------------------
// Mode entry points (wrapped by mode_blesniff.cpp as blesniff_ns_setup /
// blesniff_ns_loop)
// ---------------------------------------------------------------------------
void setup() {
    pinMode(BLESNIFF_LED_PIN, OUTPUT);
    digitalWrite(BLESNIFF_LED_PIN, HIGH);   // LED off (inverted)

    blesniff_buzzer_setup();

    config::load();

    if (!wifi_ap_start()) {
        g_fault = Fault::Wifi;
    }

    session_pcap::init();
    web_dashboard::init();

    if (!scan::init()) {
        g_fault = Fault::Scan;
    }

    // pcap_stream no longer needs init -- USB output is text-only, session
    // buffer for the dashboard is initialized separately.

    // pcap_writer_task copies scan::Frame (~280B) into a stack local per pop;
    // 8KB is comfortable headroom.
    xTaskCreatePinnedToCore(&pcap_writer_task, "bls_wr",  8192, nullptr, 5, &pcap_task_h, 0);
    xTaskCreatePinnedToCore(&led_task,         "bls_led", 2048, nullptr, 1, &led_task_h,  0);

    if (g_fault == Fault::None) blesniff_buzzer_chirp(1500, 40);
}

void loop() {
    serial_pump();
    web_dashboard::tick();
    delay(20);
}
