/*
 * Mode 4: PCAP — Passive WiFi Packet Capture
 *
 * Merged from the standalone colonelpanichacks/ouispy-pcap firmware:
 *   config.cpp + capture.cpp + pcap_stream.cpp + session_pcap.cpp +
 *   text_summary.cpp + web_dashboard.cpp + dashboard_html.h + main.cpp
 *
 * Wireshark-ready pcap over USB-CDC, live web dashboard, in-PSRAM session
 * pcap available for browser download, channel hop or lock across the full
 * 2.4 GHz band. Passive receive only — nothing is transmitted.
 */

// Headers are included by the wrapper (mode_pcap.cpp) outside the anonymous
// namespace so the symbols get external linkage. Re-including them here is
// a no-op thanks to header guards.
#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_wifi_types.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <driver/ledc.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <sys/time.h>
#include <ctype.h>
#include <string.h>
#include <stdio.h>

// ---------------------------------------------------------------------------
// Hardware — matches sibling modes (active-low LED on GPIO21).
// ---------------------------------------------------------------------------
#define PCAP_BUZZER_PIN 3
#define PCAP_LED_PIN    21

static const ledc_channel_t PCAP_BUZZER_CH    = LEDC_CHANNEL_0;
static const ledc_timer_t   PCAP_BUZZER_TIMER = LEDC_TIMER_0;

// ---------------------------------------------------------------------------
// config — persistent runtime settings (NVS namespace: "pcap-mode")
// ---------------------------------------------------------------------------
namespace config {

constexpr uint8_t MODE_LOCKED = 0;
constexpr uint8_t MODE_HOP    = 1;

constexpr uint8_t FT_MGMT = 0x01;
constexpr uint8_t FT_CTRL = 0x02;
constexpr uint8_t FT_DATA = 0x04;

// USB output is text-only (line summaries + CMD replies). PCAP binary
// capture lives on the dashboard exclusively -- GET /api/session.pcap.
struct Config {
    uint8_t  mode;
    uint8_t  chan;
    uint16_t hopmask;
    uint16_t dwell_ms;
    uint8_t  ft_mask;
    char     ap_ssid[33];
    char     ap_pass[64];
    char     bssids[257];
    char     ouis[257];
};

// unified-blue NVS convention: mode owns its own namespace.
constexpr const char* NS      = "pcap-mode";
constexpr const char* VERSION = "1.0.0";

Preferences prefs;
Config      cfg;

void apply_defaults() {
    cfg.mode      = MODE_LOCKED;
    cfg.chan      = 6;
    cfg.hopmask   = 0x0422;
    cfg.dwell_ms  = 300;
    cfg.ft_mask   = FT_MGMT | FT_CTRL | FT_DATA;
    strlcpy(cfg.ap_ssid, "ouispy-pcap", sizeof(cfg.ap_ssid));
    strlcpy(cfg.ap_pass, "packetsniffer", sizeof(cfg.ap_pass));
    cfg.bssids[0] = 0;
    cfg.ouis[0]   = 0;
}

bool valid_channel(uint8_t c) { return c >= 1 && c <= 14; }

bool parse_mac(const char* s, uint8_t out[6]) {
    int v[6];
    if (sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return false;
    for (int i = 0; i < 6; ++i) {
        if (v[i] < 0 || v[i] > 0xFF) return false;
        out[i] = (uint8_t)v[i];
    }
    return true;
}

bool parse_oui(const char* s, uint8_t out[3]) {
    int v[3];
    if (sscanf(s, "%x:%x:%x", &v[0], &v[1], &v[2]) == 3) {
        for (int i = 0; i < 3; ++i) { if (v[i] < 0 || v[i] > 0xFF) return false; out[i] = (uint8_t)v[i]; }
        return true;
    }
    if (strlen(s) >= 6) {
        char buf[7]; strlcpy(buf, s, 7);
        unsigned x;
        if (sscanf(buf, "%6x", &x) == 1) {
            out[0] = (x >> 16) & 0xFF; out[1] = (x >> 8) & 0xFF; out[2] = x & 0xFF;
            return true;
        }
    }
    return false;
}

const char* FW_VERSION() { return VERSION; }
Config&     get()        { return cfg; }

void save() {
    prefs.begin(NS, false);
    prefs.putUChar("mode",    cfg.mode);
    prefs.putUChar("chan",    cfg.chan);
    prefs.putUShort("hopmask", cfg.hopmask);
    prefs.putUShort("dwell",  cfg.dwell_ms);
    prefs.putUChar("ftmask",  cfg.ft_mask);
    prefs.putString("apssid", cfg.ap_ssid);
    prefs.putString("appass", cfg.ap_pass);
    prefs.putString("bssids", cfg.bssids);
    prefs.putString("ouis",   cfg.ouis);
    prefs.end();
}

void load() {
    apply_defaults();
    prefs.begin(NS, true);
    cfg.mode     = prefs.getUChar("mode", cfg.mode);
    cfg.chan     = prefs.getUChar("chan", cfg.chan);
    cfg.hopmask  = prefs.getUShort("hopmask", cfg.hopmask);
    cfg.dwell_ms = prefs.getUShort("dwell", cfg.dwell_ms);
    cfg.ft_mask  = prefs.getUChar("ftmask", cfg.ft_mask);
    prefs.getString("apssid", cfg.ap_ssid, sizeof(cfg.ap_ssid));
    prefs.getString("appass", cfg.ap_pass, sizeof(cfg.ap_pass));
    prefs.getString("bssids", cfg.bssids, sizeof(cfg.bssids));
    prefs.getString("ouis",   cfg.ouis,   sizeof(cfg.ouis));
    prefs.end();

    if (cfg.mode > MODE_HOP)          cfg.mode = MODE_LOCKED;
    if (!valid_channel(cfg.chan))     cfg.chan = 6;
    if ((cfg.hopmask & 0x3FFF) == 0)  cfg.hopmask = 0x0422;
    cfg.hopmask &= 0x3FFF;
    if (cfg.dwell_ms < 100)           cfg.dwell_ms = 100;
    if (cfg.dwell_ms > 2000)          cfg.dwell_ms = 2000;
    if ((cfg.ft_mask & 0x07) == 0)    cfg.ft_mask = FT_MGMT | FT_CTRL | FT_DATA;
    if (strlen(cfg.ap_ssid) == 0)     strlcpy(cfg.ap_ssid, "ouispy-pcap", sizeof(cfg.ap_ssid));
    size_t pl = strlen(cfg.ap_pass);
    if (pl < 8 || pl > 63)            strlcpy(cfg.ap_pass, "packetsniffer", sizeof(cfg.ap_pass));
}

void reset_defaults() { apply_defaults(); save(); }

void set_mode(uint8_t m)      { cfg.mode = (m > MODE_HOP) ? MODE_LOCKED : m; save(); }
void set_channel(uint8_t ch)  { if (valid_channel(ch)) { cfg.chan = ch; save(); } }
void set_hopmask(uint16_t m)  { m &= 0x3FFF; if (m == 0) return; cfg.hopmask = m; save(); }
void set_dwell(uint16_t ms)   { if (ms < 100) ms = 100; if (ms > 2000) ms = 2000; cfg.dwell_ms = ms; save(); }
void set_ftmask(uint8_t m)    { m &= 0x07; if (m == 0) m = FT_MGMT | FT_CTRL | FT_DATA; cfg.ft_mask = m; save(); }

void set_ap(const char* ssid, const char* pass) {
    if (ssid && *ssid) strlcpy(cfg.ap_ssid, ssid, sizeof(cfg.ap_ssid));
    if (pass) {
        size_t l = strlen(pass);
        if (l >= 8 && l <= 63) strlcpy(cfg.ap_pass, pass, sizeof(cfg.ap_pass));
    }
    save();
}

void set_bssids(const char* list) { strlcpy(cfg.bssids, list ? list : "", sizeof(cfg.bssids)); save(); }
void set_ouis  (const char* list) { strlcpy(cfg.ouis,   list ? list : "", sizeof(cfg.ouis));   save(); }

bool bssid_allowed(const uint8_t mac[6]) {
    if (cfg.bssids[0] == 0) return true;
    char buf[257]; strlcpy(buf, cfg.bssids, sizeof(buf));
    char* saveptr = nullptr;
    for (char* tok = strtok_r(buf, ", ", &saveptr); tok; tok = strtok_r(nullptr, ", ", &saveptr)) {
        uint8_t m[6];
        if (parse_mac(tok, m) && memcmp(m, mac, 6) == 0) return true;
    }
    return false;
}

bool oui_allowed(const uint8_t mac[6]) {
    if (cfg.ouis[0] == 0) return true;
    char buf[257]; strlcpy(buf, cfg.ouis, sizeof(buf));
    char* saveptr = nullptr;
    for (char* tok = strtok_r(buf, ", ", &saveptr); tok; tok = strtok_r(nullptr, ", ", &saveptr)) {
        uint8_t o[3];
        if (parse_oui(tok, o) && memcmp(o, mac, 3) == 0) return true;
    }
    return false;
}

} // namespace config

// ---------------------------------------------------------------------------
// capture — promiscuous WiFi RX + two ring buffers (PCAP writer + dashboard)
// ---------------------------------------------------------------------------
namespace capture {

constexpr uint16_t MAX_FRAME_LEN = 2500;
constexpr uint8_t  RADIOTAP_LEN  = 23;

struct Frame {
    uint32_t idx;
    uint32_t ts_sec;
    uint32_t ts_usec;
    uint8_t  channel;
    int8_t   rssi;
    uint8_t  rate_500k;
    uint16_t len;
    uint8_t  data[MAX_FRAME_LEN];
};

constexpr uint32_t RADIOTAP_PRESENT_BITS =
    (1u << 0) | (1u << 1) | (1u << 2) | (1u << 3) | (1u << 5);

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

volatile uint32_t g_total_packets = 0;
volatile uint32_t g_pkts_this_sec = 0;
volatile uint32_t g_pkts_per_sec  = 0;
volatile uint32_t g_last_pps_ms   = 0;
volatile uint8_t  g_active_channel = 6;
volatile uint32_t g_frame_idx = 0;

TaskHandle_t  hop_task_handle = nullptr;
volatile bool hop_task_run    = false;

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

inline size_t ring_next(const Ring& r, size_t i) { return (i + 1) % r.capacity; }

void ring_push_bytes(Ring& r, const Frame& meta, const uint8_t* payload, uint16_t payload_len) {
    portENTER_CRITICAL_ISR(&r.mux);
    size_t next_head = ring_next(r, r.head);
    if (next_head == r.tail) {
        r.tail = ring_next(r, r.tail);
        r.dropped++;
    }
    Frame* slot = &r.slots[r.head];
    slot->idx       = meta.idx;
    slot->ts_sec    = meta.ts_sec;
    slot->ts_usec   = meta.ts_usec;
    slot->channel   = meta.channel;
    slot->rssi      = meta.rssi;
    slot->rate_500k = meta.rate_500k;
    slot->len       = payload_len;
    memcpy(slot->data, payload, payload_len);
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

void promisc_cb(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (buf == nullptr) return;

    uint8_t ft_bit = 0;
    switch (type) {
        case WIFI_PKT_MGMT: ft_bit = config::FT_MGMT; break;
        case WIFI_PKT_CTRL: ft_bit = config::FT_CTRL; break;
        case WIFI_PKT_DATA: ft_bit = config::FT_DATA; break;
        default: return;
    }
    if ((config::get().ft_mask & ft_bit) == 0) return;

    const wifi_promiscuous_pkt_t* pkt = (const wifi_promiscuous_pkt_t*)buf;
    uint16_t len = pkt->rx_ctrl.sig_len;
    if (len < 10 || len > MAX_FRAME_LEN) return;

    // addr2 at 10..15, addr3 (bssid) at 16..21
    if (len >= 22) {
        const uint8_t* frame = pkt->payload;
        if (!config::oui_allowed(frame + 10)) return;
        if (!config::bssid_allowed(frame + 16)) return;
    }

    Frame meta;
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    meta.idx       = ++g_frame_idx;
    meta.ts_sec    = (uint32_t)tv.tv_sec;
    meta.ts_usec   = (uint32_t)tv.tv_usec;
    meta.channel   = pkt->rx_ctrl.channel;
    meta.rssi      = pkt->rx_ctrl.rssi;
    meta.rate_500k = (uint8_t)(pkt->rx_ctrl.rate & 0x7F);
    meta.len       = 0;

    ring_push_bytes(ring_pcap, meta, pkt->payload, len);
    ring_push_bytes(ring_dash, meta, pkt->payload, len);
    g_total_packets++;
    g_pkts_this_sec++;
}

void hop_task(void* /*arg*/) {
    for (;;) {
        if (!hop_task_run) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
        uint16_t mask  = config::get().hopmask & 0x3FFF;
        uint16_t dwell = config::get().dwell_ms;
        if (mask == 0) { vTaskDelay(pdMS_TO_TICKS(200)); continue; }
        for (uint8_t ch = 1; ch <= 14 && hop_task_run; ++ch) {
            if ((mask & (1u << (ch - 1))) == 0) continue;
            esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
            g_active_channel = ch;
            vTaskDelay(pdMS_TO_TICKS(dwell));
            mask  = config::get().hopmask & 0x3FFF;
            dwell = config::get().dwell_ms;
        }
    }
}

void apply_filter_mask() {
    wifi_promiscuous_filter_t filter = {0};
    uint32_t m = 0;
    uint8_t ft = config::get().ft_mask;
    if (ft & config::FT_MGMT) m |= WIFI_PROMIS_FILTER_MASK_MGMT;
    if (ft & config::FT_CTRL) m |= WIFI_PROMIS_FILTER_MASK_CTRL;
    if (ft & config::FT_DATA) m |= WIFI_PROMIS_FILTER_MASK_DATA;
    if (m == 0) m = WIFI_PROMIS_FILTER_MASK_ALL;
    filter.filter_mask = m;
    esp_wifi_set_promiscuous_filter(&filter);
}

void teardown_wifi() {
    esp_wifi_set_promiscuous(false);
    WiFi.mode(WIFI_OFF);
    delay(50);
}

void start_ap_locked() {
    teardown_wifi();
    WiFi.mode(WIFI_AP);
    WiFi.softAP(config::get().ap_ssid, config::get().ap_pass, config::get().chan, 0, 4);
    apply_filter_mask();
    esp_wifi_set_promiscuous_rx_cb(&promisc_cb);
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(config::get().chan, WIFI_SECOND_CHAN_NONE);
    g_active_channel = config::get().chan;
}

void start_sta_hop() {
    teardown_wifi();
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(true, true);
    apply_filter_mask();
    esp_wifi_set_promiscuous_rx_cb(&promisc_cb);
    esp_wifi_set_promiscuous(true);
}

bool build_radiotap(uint8_t* out, uint8_t channel, int8_t rssi, uint8_t rate_500k) {
    memset(out, 0, RADIOTAP_LEN);
    uint16_t it_len = RADIOTAP_LEN;
    memcpy(out + 2, &it_len, 2);
    uint32_t pres = RADIOTAP_PRESENT_BITS;
    memcpy(out + 4, &pres, 4);
    uint64_t tsft = (uint64_t)esp_timer_get_time();
    memcpy(out + 8, &tsft, 8);
    out[17] = rate_500k ? rate_500k : 2;
    uint16_t freq = 2407 + (uint16_t)channel * 5;
    if (channel == 14) freq = 2484;
    memcpy(out + 18, &freq, 2);
    uint16_t ch_flags = 0x00A0;
    memcpy(out + 20, &ch_flags, 2);
    out[22] = (uint8_t)rssi;
    return true;
}

void apply_mode() {
    hop_task_run = false;
    vTaskDelay(pdMS_TO_TICKS(50));
    if (config::get().mode == config::MODE_HOP) {
        start_sta_hop();
        hop_task_run = true;
    } else {
        start_ap_locked();
    }
}

bool init() {
    bool have_psram = psramFound();
    size_t pcap_slots = have_psram ? 96 : 12;
    size_t dash_slots = have_psram ? 24 : 4;
    if (!ring_alloc(ring_pcap, pcap_slots, true)) return false;
    if (!ring_alloc(ring_dash, dash_slots, true)) return false;

    apply_mode();
    xTaskCreatePinnedToCore(&hop_task, "pcap_hop", 3072, nullptr, 4, &hop_task_handle, 0);
    return true;
}

bool pop_pcap(Frame* out)      { return ring_pop(ring_pcap, out); }
bool pop_dashboard(Frame* out) { return ring_pop(ring_dash, out); }

uint32_t total_packets()   { return g_total_packets; }
uint32_t dropped_pcap()    { return ring_pcap.dropped; }
uint32_t dropped_dash()    { return ring_dash.dropped; }
uint8_t  current_channel() { return g_active_channel; }

uint32_t packets_per_sec() {
    uint32_t now = millis();
    if (now - g_last_pps_ms >= 1000) {
        g_pkts_per_sec = g_pkts_this_sec;
        g_pkts_this_sec = 0;
        g_last_pps_ms = now;
    }
    return g_pkts_per_sec;
}

void clear_ring() {
    portENTER_CRITICAL(&ring_pcap.mux);
    ring_pcap.head = ring_pcap.tail = 0; ring_pcap.dropped = 0;
    portEXIT_CRITICAL(&ring_pcap.mux);
    portENTER_CRITICAL(&ring_dash.mux);
    ring_dash.head = ring_dash.tail = 0; ring_dash.dropped = 0;
    portEXIT_CRITICAL(&ring_dash.mux);
}

} // namespace capture

// ---------------------------------------------------------------------------
// pcap_stream — pcap global header + record framing over USB-CDC
// ---------------------------------------------------------------------------
// USB output is text-only (one-line human-readable per frame). PCAP binary
// capture lives on the dashboard exclusively -- GET /api/session.pcap on
// the AP. The USB CDC layer on ESP32-S3 could not be made reliable for
// high-rate binary streaming (residual byte-boundary corruption under load).
namespace pcap_stream {

// PCAP framing constants -- session_pcap uses these for the dashboard buffer.
constexpr uint32_t PCAP_MAGIC     = 0xA1B2C3D4;
constexpr uint16_t PCAP_VER_MAJOR = 2;
constexpr uint16_t PCAP_VER_MINOR = 4;
constexpr uint32_t PCAP_LINKTYPE  = 127; // IEEE802_11_RADIOTAP
constexpr uint32_t PCAP_SNAPLEN   = 2500;

} // namespace pcap_stream

// ---------------------------------------------------------------------------
// text_summary — one-line human-readable summary of an 802.11 frame
// ---------------------------------------------------------------------------
namespace text_summary {

const char* mgmt_subtype(uint8_t sub) {
    switch (sub) {
        case 0:  return "ASSOC-REQ";
        case 1:  return "ASSOC-RESP";
        case 2:  return "REASSOC-REQ";
        case 3:  return "REASSOC-RESP";
        case 4:  return "PROBE-REQ";
        case 5:  return "PROBE-RESP";
        case 8:  return "BEACON";
        case 9:  return "ATIM";
        case 10: return "DISASSOC";
        case 11: return "AUTH";
        case 12: return "DEAUTH";
        case 13: return "ACTION";
        default: return "MGMT";
    }
}
const char* ctrl_subtype(uint8_t sub) {
    switch (sub) {
        case 4:  return "BEAMFORM";
        case 7:  return "WRAPPER";
        case 8:  return "BAR";
        case 9:  return "BA";
        case 10: return "PS-POLL";
        case 11: return "RTS";
        case 12: return "CTS";
        case 13: return "ACK";
        case 14: return "CF-END";
        case 15: return "CF-END-ACK";
        default: return "CTRL";
    }
}
const char* data_subtype(uint8_t sub) {
    switch (sub) {
        case 0:  return "DATA";
        case 4:  return "NULL";
        case 8:  return "QOS-DATA";
        case 12: return "QOS-NULL";
        default: return "DATA-SUB";
    }
}

const char* type_name(uint8_t fc0) {
    uint8_t type = (fc0 >> 2) & 0x03;
    uint8_t sub  = (fc0 >> 4) & 0x0F;
    static char scratch[24];
    const char* type_str = "?";
    const char* sub_str  = "?";
    switch (type) {
        case 0: type_str = "MGMT"; sub_str = mgmt_subtype(sub); break;
        case 1: type_str = "CTRL"; sub_str = ctrl_subtype(sub); break;
        case 2: type_str = "DATA"; sub_str = data_subtype(sub); break;
        default: type_str = "EXT"; sub_str = "?"; break;
    }
    snprintf(scratch, sizeof(scratch), "%s/%s", type_str, sub_str);
    return scratch;
}

void format_mac(const uint8_t mac[6], char* out17) {
    snprintf(out17, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void extract_ssid(const capture::Frame& f, char* out, size_t out_sz) {
    out[0] = 0;
    if (f.len < 24) return;
    uint8_t fc0 = f.data[0];
    uint8_t type = (fc0 >> 2) & 0x03;
    uint8_t sub  = (fc0 >> 4) & 0x0F;
    if (type != 0) return;

    size_t tagged_offset = 0;
    if (sub == 8 || sub == 5) {
        if (f.len < 24 + 12 + 2) return;
        tagged_offset = 24 + 12;
    } else if (sub == 4) {
        if (f.len < 24 + 2) return;
        tagged_offset = 24;
    } else {
        return;
    }
    while (tagged_offset + 2 <= f.len) {
        uint8_t tag = f.data[tagged_offset];
        uint8_t tlen = f.data[tagged_offset + 1];
        if (tagged_offset + 2 + tlen > f.len) return;
        if (tag == 0) {
            size_t n = tlen;
            if (n >= out_sz) n = out_sz - 1;
            for (size_t i = 0; i < n; ++i) {
                uint8_t c = f.data[tagged_offset + 2 + i];
                out[i] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
            }
            out[n] = 0;
            return;
        }
        tagged_offset += 2 + tlen;
    }
}

size_t format_line(const capture::Frame& f, char* out, size_t out_sz) {
    if (f.len < 24) {
        return snprintf(out, out_sz, "[Ch%u RSSI%ddBm] SHORT len=%u\n",
                        f.channel, (int)f.rssi, f.len);
    }
    const uint8_t* p = f.data;
    char addr1[18], addr2[18], addr3[18];
    format_mac(p + 4,  addr1);
    format_mac(p + 10, addr2);
    format_mac(p + 16, addr3);
    char ssid[64] = {0};
    extract_ssid(f, ssid, sizeof(ssid));

    if (ssid[0]) {
        return snprintf(out, out_sz,
            "[Ch%u RSSI%ddBm] %s src=%s dst=%s bssid=%s len=%u ssid=%s\n",
            f.channel, (int)f.rssi, type_name(p[0]),
            addr2, addr1, addr3, f.len, ssid);
    }
    return snprintf(out, out_sz,
        "[Ch%u RSSI%ddBm] %s src=%s dst=%s bssid=%s len=%u\n",
        f.channel, (int)f.rssi, type_name(p[0]),
        addr2, addr1, addr3, f.len);
}

} // namespace text_summary

// ---------------------------------------------------------------------------
// pcap_stream (text output tail — depends on text_summary)
// ---------------------------------------------------------------------------
namespace pcap_stream {

void write_frame_text(const capture::Frame& f) {
    char line[320];
    size_t n = text_summary::format_line(f, line, sizeof(line));
    if (n > 0) Serial.write((const uint8_t*)line, n);
}

} // namespace pcap_stream

// ---------------------------------------------------------------------------
// session_pcap — in-PSRAM circular pcap buffer for browser download
// ---------------------------------------------------------------------------
namespace session_pcap {

// Tiered PSRAM allocation. First entry that succeeds wins; nothing falls
// through to DRAM (that path OOM-crashed the ESP32 previously).
constexpr size_t CAP_TIERS[] = { 6 * 1024 * 1024, 4 * 1024 * 1024, 2 * 1024 * 1024 };
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

uint8_t*          g_buf     = nullptr;
size_t            g_cap     = 0;
size_t            g_used    = 0;
uint32_t          g_dropped = 0;
SemaphoreHandle_t g_lock    = nullptr;

volatile State    g_state       = State::IDLE;
volatile uint32_t g_downloads   = 0;

void write_global_header_locked() {
    PcapGlobal g{};
    g.magic    = pcap_stream::PCAP_MAGIC;
    g.vmaj     = pcap_stream::PCAP_VER_MAJOR;
    g.vmin     = pcap_stream::PCAP_VER_MINOR;
    g.snaplen  = pcap_stream::PCAP_SNAPLEN;
    g.linktype = pcap_stream::PCAP_LINKTYPE;
    memcpy(g_buf, &g, sizeof(g));
    g_used = sizeof(g);
}

// Walk record boundaries forward until we've skipped at least `bytes_to_drop`.
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

void append(const capture::Frame& f) {
    if (!g_buf || !g_lock) return;
    if (g_state != State::RECORDING) return;

    uint8_t rt[capture::RADIOTAP_LEN];
    capture::build_radiotap(rt, f.channel, f.rssi, f.rate_500k);
    const uint32_t total = capture::RADIOTAP_LEN + f.len;
    const size_t rec_len = sizeof(PcapRec) + total;

    if (rec_len > g_cap - GLOBAL_HDR_LEN) return;

    xSemaphoreTake(g_lock, portMAX_DELAY);

    // Re-check state after taking the lock -- a stop() could have raced in.
    if (g_state != State::RECORDING) {
        xSemaphoreGive(g_lock);
        return;
    }

    if (g_used + rec_len > g_cap) {
        // Reclaim at least half the buffer per memmove to amortize the cost.
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
    rec.incl_len = total;
    rec.orig_len = total;
    memcpy(g_buf + g_used, &rec, sizeof(rec));
    memcpy(g_buf + g_used + sizeof(rec), rt, capture::RADIOTAP_LEN);
    memcpy(g_buf + g_used + sizeof(rec) + capture::RADIOTAP_LEN, f.data, f.len);
    g_used += rec_len;

    xSemaphoreGive(g_lock);
}

size_t   size()      { return g_used; }
size_t   capacity()  { return g_cap; }
uint32_t dropped()   { return g_dropped; }

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
static const char PCAP_INDEX_HTML[] PROGMEM = R"HTML(<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<title>OUI-SPY PCAP</title>
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
    --f-beacon:   #58a6ff;
    --f-probe:    #d2a8ff;
    --f-assoc:    #56d4dd;
    --f-auth:     #56d4dd;
    --f-action:   #a5d6ff;
    --f-deauth:   #f85149;
    --f-disassoc: #ff7b72;
    --f-data:     #3fb950;
    --f-eapol:    #7ee787;
    --f-ctrl:     #d29922;
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
  .rail input[type=text], .rail input[type=password], .rail select {
    width: 100%; padding: 6px 8px;
    font-family: ui-monospace, Menlo, monospace; font-size: 12px; outline: none;
  }
  .rail input:focus, .rail select:focus { border-color: var(--accent); }
  .radio-row, .check-row { display: flex; gap: 12px; margin: 4px 0; align-items: center; flex-wrap: wrap; }
  .radio-row label, .check-row label { margin: 0; color: var(--text); font-size: 12px; cursor: pointer; }
  input[type=checkbox], input[type=radio] { accent-color: var(--accent); margin: 0; }
  .ch-grid {
    display: grid; grid-template-columns: repeat(7, 1fr); gap: 3px; margin: 6px 0;
  }
  .ch-cell {
    display: flex; align-items: center; gap: 4px; padding: 3px 5px;
    background: var(--bg); border: 1px solid var(--border);
    font-family: ui-monospace, Menlo, monospace; font-size: 10px;
    color: var(--text); cursor: pointer;
  }
  .ch-cell.checked { border-color: var(--accent); color: var(--accent); }
  .preset-row { display: flex; flex-wrap: wrap; gap: 3px; margin: 6px 0; }
  .preset-row .btn { padding: 3px 8px; font-size: 10px; }
  .warn-banner {
    background: rgba(210,153,34,0.08); border: 1px solid var(--warn);
    color: var(--warn); padding: 6px 8px; margin: 6px 0; font-size: 11px;
  }
  .slider-row { display: flex; align-items: center; gap: 8px; }
  .slider-row input[type=range] { flex: 1; accent-color: var(--accent); }
  .slider-row .val {
    font-family: ui-monospace, Menlo, monospace; color: var(--text);
    font-size: 11px; min-width: 54px; text-align: right;
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
    max-height: 130px;
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
  table { min-width: 900px; }
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
  tbody td.info  { color: var(--text); overflow: hidden; text-overflow: ellipsis; max-width: 340px; }
  tbody td.rssi.strong { color: var(--good); }
  tbody td.rssi.mid    { color: var(--warn); }
  tbody td.rssi.weak   { color: var(--bad); }
  tr.t-beacon   { color: var(--f-beacon); }
  tr.t-probe    { color: var(--f-probe); }
  tr.t-assoc    { color: var(--f-assoc); }
  tr.t-auth     { color: var(--f-auth); }
  tr.t-action   { color: var(--f-action); }
  tr.t-deauth   { color: var(--f-deauth); font-weight: 500; }
  tr.t-disassoc { color: var(--f-disassoc); font-weight: 500; }
  tr.t-data     { color: var(--f-data); }
  tr.t-eapol    { color: var(--f-eapol); font-weight: 500; background: rgba(126,231,135,0.04); }
  tr.t-ctrl     { color: var(--f-ctrl); }
  tbody tr td.type { font-weight: 500; }
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
  .sess .badge .dot {
    width: 8px; height: 8px; border-radius: 50%;
    display: inline-block; background: currentColor;
  }
  .sess .badge.idle      { color: #8892a0; border-color: #4a5568; }
  .sess .badge.recording {
    color: #ff2b3b; border-color: #ff2b3b;
    background: rgba(255,43,59,0.10);
    animation: pulse-rec 1.2s ease-in-out infinite;
    box-shadow: 0 0 12px rgba(255,43,59,0.35);
  }
  .sess .badge.paused    {
    color: #f6c05a; border-color: #f6c05a;
    background: rgba(246,192,90,0.10);
  }
  .sess .badge.stopped   {
    color: #4ecca3; border-color: #4ecca3;
    background: rgba(78,204,163,0.10);
    box-shadow: 0 0 10px rgba(78,204,163,0.35);
  }
  @keyframes pulse-rec {
    0%   { opacity: 0.6; }
    50%  { opacity: 1.0; }
    100% { opacity: 0.6; }
  }

  .sess .bar-wrap { display: flex; flex-direction: column; gap: 3px; min-width: 0; }
  .sess .bar {
    height: 10px; width: 100%;
    background: var(--panel-2);
    border: 1px solid var(--border);
    position: relative; overflow: hidden;
  }
  .sess .bar .fill {
    height: 100%;
    width: 0%;
    background: #4a5568;
    transition: width 200ms ease, background 120ms ease;
    box-shadow: none;
  }
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
  .sess .sbtn:disabled {
    opacity: 0.35; cursor: not-allowed;
  }
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
    .banner { font-size: 4.5px; display: block; overflow: hidden; }
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
    .qf { padding: 4px 8px; max-height: 110px; }
    .qf-row .lbl { font-size: 10px; padding-left: 2px; }
    tbody td.info { max-width: 160px; }
    .banner { font-size: 3.6px; }
  }
</style>

<div class="app">

  <div class="topbar">
    <div class="banner-wrap">
      <pre class="banner">  .oooooo.   ooooo     ooo ooooo          .oooooo..o ooooooooo.   oooooo   oooo       ooooooooo.     .oooooo.         .o.       ooooooooo.
 d8P'  `Y8b  `888'     `8' `888'         d8P'    `Y8 `888   `Y88.  `888.   .8'        `888   `Y88.  d8P'  `Y8b       .888.      `888   `Y88.
888      888  888       8   888          Y88bo.       888   .d88'   `888. .8'          888   .d88' 888              .8"888.      888   .d88'
888      888  888       8   888           `"Y8888o.   888ooo88P'     `888.8'           888ooo88P'  888             .8' `888.     888ooo88P'
888      888  888       8   888  8888888      `"Y88b  888             `888'            888         888            .88ooo8888.    888
`88b    d88'  `88.    .8'   888          oo     .d8P  888              888             888         `88b    ooo   .8'     `888.   888
 `Y8bood8P'     `YbodP'    o888o         8""88888P'  o888o            o888o           o888o         `Y8bood8P'  o88o     o8888o o888o</pre>
      <span class="banner-compact">OUI-SPY // PCAP</span>
    </div>
    <div class="status">
      <span>Mode</span><span class="v good" id="statMode">--</span>
      <span>Ch</span><span class="v" id="statChan">--</span>
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
      <h3>Channel</h3>
      <div class="radio-row">
        <input type="radio" name="chmode" id="chLock">
        <label for="chLock">Locked</label>
        <input type="radio" name="chmode" id="chHop">
        <label for="chHop">Hop</label>
      </div>
      <div id="lockPanel">
        <select id="lockCh">
          <option>1</option><option>2</option><option>3</option><option>4</option><option>5</option>
          <option>6</option><option>7</option><option>8</option><option>9</option>
          <option>10</option><option>11</option><option>12</option><option>13</option><option>14</option>
        </select>
      </div>
      <div id="hopPanel" style="display:none">
        <div class="warn-banner">Hop disables the AP. Dashboard will drop. Use USB output.</div>
        <div class="ch-grid" id="chGrid"></div>
        <div class="preset-row">
          <button class="btn" data-preset="quick">Quick</button>
          <button class="btn" data-preset="us">US</button>
          <button class="btn" data-preset="world">World</button>
          <button class="btn" data-preset="jp">JP</button>
          <button class="btn" data-preset="clear">Clear</button>
        </div>
        <label>Dwell</label>
        <div class="slider-row">
          <input type="range" min="100" max="2000" step="10" value="300" id="dwell"/>
          <span class="val" id="dwellVal">300 ms</span>
        </div>
      </div>
    </section>

    <section>
      <h3>Frame types</h3>
      <div class="check-row">
        <input type="checkbox" id="ftMgmt"><label for="ftMgmt">Mgmt</label>
        <input type="checkbox" id="ftCtrl"><label for="ftCtrl">Ctrl</label>
        <input type="checkbox" id="ftData"><label for="ftData">Data</label>
      </div>
    </section>

    <section>
      <h3>Custom lists</h3>
      <label>BSSID allow (blank = all)</label>
      <input type="text" id="bssids" placeholder="aa:bb:cc:dd:ee:ff, ..." />
      <label>OUI allow (first 3 bytes)</label>
      <input type="text" id="ouis" placeholder="aa:bb:cc, ..." />
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
        <button class="sbtn rec"   id="btnRecord" title="Start recording">&#9679; RECORD</button>
        <button class="sbtn pause" id="btnPause"  title="Pause recording">&#9208; PAUSE</button>
        <button class="sbtn stop"  id="btnStop"   title="Stop and finalize">&#9209; STOP</button>
        <button class="sbtn save"  id="btnSavePcap" title="Download session PCAP">&#8681; SAVE PCAP</button>
      </div>
    </div>
    <div class="toolbar">
      <input type="text" id="filter" placeholder="filter -- rssi>-60 | src:aa:bb | ssid:xfini | free text" />
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
            <th style="width:130px">Type</th>
            <th class="hide-sm" style="width:140px">Src</th>
            <th class="hide-sm" style="width:140px">Dst</th>
            <th class="hide-sm" style="width:140px">BSSID</th>
            <th class="hide-sm" style="width:52px">Len</th>
            <th>Info</th>
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
            <button class="chip" data-key="beacon" data-group="type"><span class="ind"></span>Beacons<span class="count" id="c-beacon">0</span></button>
            <button class="chip" data-key="probereq" data-group="type"><span class="ind"></span>Probe Req<span class="count" id="c-probereq">0</span></button>
            <button class="chip" data-key="proberesp" data-group="type"><span class="ind"></span>Probe Resp<span class="count" id="c-proberesp">0</span></button>
            <button class="chip" data-key="assoc" data-group="type"><span class="ind"></span>Assoc<span class="count" id="c-assoc">0</span></button>
            <button class="chip" data-key="auth" data-group="type"><span class="ind"></span>Auth<span class="count" id="c-auth">0</span></button>
            <button class="chip" data-key="action" data-group="type"><span class="ind"></span>Action<span class="count" id="c-action">0</span></button>
          </div>
          <div class="trail"></div>
        </div>
      </div>
      <div class="qf-row" data-group="attack">
        <button class="qf-row-toggle" type="button">
          <span class="caret">&#9662;</span>
          <span class="lbl">Attack</span>
          <span class="badge" data-badge="attack">0</span>
        </button>
        <div class="qf-row-body">
          <div class="chips">
            <button class="chip danger" data-key="deauth" data-group="attack"><span class="ind"></span>Deauth<span class="count" id="c-deauth">0</span></button>
            <button class="chip danger" data-key="disassoc" data-group="attack"><span class="ind"></span>Disassoc<span class="count" id="c-disassoc">0</span></button>
            <button class="chip good" data-key="eapol" data-group="attack"><span class="ind"></span>EAPOL<span class="count" id="c-eapol">0</span></button>
          </div>
          <div class="trail"></div>
        </div>
      </div>
      <div class="qf-row" data-group="data">
        <button class="qf-row-toggle" type="button">
          <span class="caret">&#9662;</span>
          <span class="lbl">Data</span>
          <span class="badge" data-badge="data">0</span>
        </button>
        <div class="qf-row-body">
          <div class="chips">
            <button class="chip" data-key="data" data-group="data"><span class="ind"></span>Data<span class="count" id="c-data">0</span></button>
            <button class="chip" data-key="qosdata" data-group="data"><span class="ind"></span>QoS<span class="count" id="c-qosdata">0</span></button>
            <button class="chip" data-key="nulldata" data-group="data"><span class="ind"></span>Null<span class="count" id="c-nulldata">0</span></button>
            <button class="chip" data-key="ctrl" data-group="data"><span class="ind"></span>Ctrl<span class="count" id="c-ctrl">0</span></button>
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
            <button class="chip warn" data-key="broadcast" data-group="traits"><span class="ind"></span>Bcast<span class="count" id="c-broadcast">0</span></button>
            <button class="chip warn" data-key="retry" data-group="traits"><span class="ind"></span>Retry<span class="count" id="c-retry">0</span></button>
            <button class="chip" data-key="unencrypted" data-group="traits"><span class="ind"></span>Open<span class="count" id="c-unencrypted">0</span></button>
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
    <span>ouispy-pcap <b id="fwVer">--</b></span>
    <span class="right">
      <span>ws <b class="v" id="wsState">--</b></span>
      <span>total <b id="totalPkts">0</b></span>
      <span>shown <b id="rowCount">0</b></span>
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

  function toggleRail(force) {
    const rail = $('rail'); const scrim = $('scrim');
    const on = force !== undefined ? force : !rail.classList.contains('open');
    rail.classList.toggle('open', on);
    scrim.classList.toggle('open', on);
  }
  $('btnSettings').onclick = () => toggleRail(true);
  $('scrim').onclick = () => toggleRail(false);

  // Each OUI and company ID here is registered to that very company (IEEE MA-L, Bluetooth SIG; checked
  // 2026-11-25). A radio maker's (Espressif, Liteon, Telink) or a parent's (Amazon) would tag every
  // device built on it as the brand.
  const VENDORS = [
    { id:'ring',   name:'RING',   color:'var(--v-ring)',   ouis:['00:b4:63','18:7f:88','24:2b:d6','34:3e:a4','50:e4:67','54:e0:19','5c:47:5e','64:9a:63','90:48:6c','9c:76:13','ac:9f:c3','c4:db:ad','cc:3b:fb'] },
    { id:'axon',   name:'AXON',   color:'var(--v-axon)',   ouis:['00:25:df'] },
    { id:'flock',  name:'FLOCK',  color:'var(--v-flock)',  ouis:['b4:1e:52'] },
    { id:'dji',    name:'DJI',    color:'var(--v-dji)',    ouis:['0c:9a:e6','8c:58:23','04:a8:5a','58:b8:58','e4:7a:2c','60:60:1f','48:1c:b9','34:d2:62','4c:43:f6','88:29:85'] },
    { id:'parrot', name:'PARROT', color:'var(--v-parrot)', ouis:['00:12:1c','00:26:7e','90:03:b7','90:3a:e6','a0:14:3d'] },
    { id:'skydio', name:'SKYDIO', color:'var(--v-skydio)', ouis:['38:1d:14'] },
    { id:'meta',   name:'META',   color:'var(--v-meta)',   ouis:['2c:26:17','48:05:60','48:57:dd','50:99:03','78:c4:fa','80:f3:ef','84:57:f7','88:25:08','94:f9:29','a4:0e:2b','b4:17:a8','c0:dd:8a','cc:a1:74','d0:b3:c2','d4:d6:59','f4:4e:35','38:47:12','80:aa:1c','98:59:49'] },
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
  function vendorFor(mac) {
    if (!mac) return null;
    const prefix = mac.slice(0, 8).toLowerCase();
    for (const v of VENDORS) {
      if (!vendorEnabled.has(v.id)) continue;
      if (v.ouis.includes(prefix)) return v;
    }
    return null;
  }

  const grid = $('chGrid');
  for (let c = 1; c <= 14; c++) {
    grid.insertAdjacentHTML('beforeend',
      '<label class="ch-cell"><input type="checkbox" data-ch="'+c+'"/>'+c+'</label>');
  }
  grid.querySelectorAll('input').forEach(cb => {
    cb.onchange = () => cb.parentElement.classList.toggle('checked', cb.checked);
  });
  function preset(list) {
    grid.querySelectorAll('input[type=checkbox]').forEach(cb => {
      const on = list.includes(+cb.dataset.ch);
      cb.checked = on;
      cb.parentElement.classList.toggle('checked', on);
    });
  }
  const PRESETS = {
    quick:[1,6,11],
    us:[1,2,3,4,5,6,7,8,9,10,11],
    world:[1,2,3,4,5,6,7,8,9,10,11,12,13],
    jp:[1,2,3,4,5,6,7,8,9,10,11,12,13,14],
    clear:[]
  };
  document.querySelectorAll('.preset-row .btn').forEach(b => {
    b.onclick = () => preset(PRESETS[b.dataset.preset] || []);
  });
  function setChMode(m) {
    $('lockPanel').style.display = m === 'lock' ? '' : 'none';
    $('hopPanel').style.display  = m === 'hop'  ? '' : 'none';
  }
  $('chLock').onchange = () => setChMode('lock');
  $('chHop').onchange  = () => setChMode('hop');
  $('dwell').oninput = () => { $('dwellVal').textContent = $('dwell').value + ' ms'; };

  function classifyType(y) {
    y = (y || '').toUpperCase();
    const keys = [];
    let cls = '';
    if (y === 'MGMT/BEACON')          { keys.push('beacon');            cls = 't-beacon'; }
    else if (y === 'MGMT/PROBE-REQ')  { keys.push('probe','probereq');  cls = 't-probe'; }
    else if (y === 'MGMT/PROBE-RSP')  { keys.push('probe','proberesp'); cls = 't-probe'; }
    else if (y === 'MGMT/ASSOC-REQ' || y === 'MGMT/ASSOC-RESP' ||
             y === 'MGMT/REASSOC-REQ' || y === 'MGMT/REASSOC-RESP') {
      keys.push('assoc');             cls = 't-assoc';
    }
    else if (y === 'MGMT/AUTH')       { keys.push('auth');              cls = 't-auth'; }
    else if (y === 'MGMT/ACTION')     { keys.push('action');            cls = 't-action'; }
    else if (y === 'MGMT/DEAUTH')     { keys.push('deauth');            cls = 't-deauth'; }
    else if (y === 'MGMT/DISASSOC')   { keys.push('disassoc');          cls = 't-disassoc'; }
    else if (y === 'DATA/QOS-DATA')   { keys.push('data','qosdata');    cls = 't-data'; }
    else if (y === 'DATA/NULL' || y === 'DATA/QOS-NULL') {
      keys.push('data','nulldata');   cls = 't-data';
    }
    else if (y.startsWith('DATA/'))   { keys.push('data');              cls = 't-data'; }
    else if (y.startsWith('CTRL/'))   { keys.push('ctrl');              cls = 't-ctrl'; }
    return { keys, cls };
  }

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

  function pushPacket(p) {
    const y   = p.y || '';
    const src = p.s || '';
    const dst = p.d || '';
    const bss = p.b || '';
    const info = p.n || '';
    const cls = classifyType(y);
    const keys = new Set(cls.keys);
    if (dst === 'ff:ff:ff:ff:ff:ff') keys.add('broadcast');
    keys.forEach(bumpChip);

    const vend = vendorFor(src) || vendorFor(dst) || vendorFor(bss);
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

    const tr = document.createElement('tr');
    tr.className = cls.cls;
    if (vend) tr.classList.add('hit');
    tr.dataset.keys = [...keys].join(' ');
    tr.dataset.hit  = vend ? '1' : '0';
    tr.dataset.src  = src.toLowerCase();
    tr.dataset.dst  = dst.toLowerCase();
    tr.dataset.bss  = bss.toLowerCase();
    tr.dataset.type = y.toLowerCase();
    tr.dataset.rssi = String(p.r);
    tr.dataset.ch   = String(p.c);
    tr.dataset.info = info.toLowerCase();
    tr.innerHTML =
      '<td class="n hide-sm">'+(p.i != null ? p.i : n)+'</td>'+
      '<td class="hide-sm">'+fmtTime(p.t || 0)+'</td>'+
      '<td>'+p.c+'</td>'+
      '<td class="rssi '+rc+' right">'+p.r+'</td>'+
      '<td class="type">'+escapeHtml(y)+'</td>'+
      '<td class="mac hide-sm">'+escapeHtml(src)+'</td>'+
      '<td class="mac hide-sm">'+escapeHtml(dst)+'</td>'+
      '<td class="mac hide-sm">'+escapeHtml(bss)+'</td>'+
      '<td class="right hide-sm">'+p.l+'</td>'+
      '<td class="info">'+vendorTag+escapeHtml(info)+'</td>';
    rows.appendChild(tr);
    while (rows.childElementCount > MAX_ROWS) rows.removeChild(rows.firstChild);
    applyRowFilter(tr);
  }

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
  function fmtBytes(n) {
    if (n == null) return '--';
    if (n < 1024) return n + ' B';
    if (n < 1024*1024) return (n/1024).toFixed(1) + ' KB';
    if (n < 1024*1024*1024) return (n/(1024*1024)).toFixed(1) + ' MB';
    return (n/(1024*1024*1024)).toFixed(2) + ' GB';
  }
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

    const rec   = $('btnRecord');
    const pause = $('btnPause');
    const stop  = $('btnStop');
    const save  = $('btnSavePcap');
    if (s === 'idle') {
      rec.innerHTML   = '&#9679; RECORD';
      rec.disabled    = false;
      pause.disabled  = true;
      stop.disabled   = true;
      save.disabled   = true;
    } else if (s === 'recording') {
      rec.innerHTML   = '&#9679; RECORD';
      rec.disabled    = true;
      pause.disabled  = false;
      stop.disabled   = false;
      save.disabled   = true;
    } else if (s === 'paused') {
      rec.innerHTML   = '&#9654; RESUME';
      rec.disabled    = false;
      pause.disabled  = true;
      stop.disabled   = false;
      save.disabled   = true;
    } else if (s === 'stopped') {
      rec.innerHTML   = '&#9679; RE-RECORD';
      rec.disabled    = false;
      pause.disabled  = true;
      stop.disabled   = true;
      save.disabled   = false;
    }
  }
  applySessState('idle');

  async function sessPost(path) {
    try { await fetch(path, {method:'POST'}); } catch(e){}
  }
  $('btnRecord').onclick = () => {
    applySessState('recording');
    sessPost('/api/session/record');
  };
  $('btnPause').onclick = () => {
    applySessState('paused');
    sessPost('/api/session/pause');
  };
  $('btnStop').onclick = () => {
    applySessState('stopped');
    sessPost('/api/session/stop');
  };
  $('btnSavePcap').onclick = () => {
    if (sessState !== 'stopped') return;
    const stamp = new Date().toISOString().replace(/[:.]/g,'-').slice(0,19);
    const a = document.createElement('a');
    a.href = '/api/session.pcap?ts=' + Date.now();
    a.download = 'ouispy-pcap-' + stamp + '.pcap';
    document.body.appendChild(a);
    a.click();
    a.remove();
  };
  $('snapBtn').onclick = () => {
    const cols = ['idx','t_ms','ch','rssi','type','src','dst','bssid','len','info'];
    const lines = [cols.join(',')];
    rows.querySelectorAll('tr').forEach(tr => {
      const cells = tr.children;
      const info = tr.dataset.info || '';
      const idx = cells[0].textContent;
      const t = cells[1].textContent;
      const ch = cells[2].textContent;
      const r  = cells[3].textContent;
      const y  = cells[4].textContent;
      const s  = cells[5].textContent;
      const d  = cells[6].textContent;
      const b  = cells[7].textContent;
      const l  = cells[8].textContent;
      lines.push([idx, t, ch, r, y, s, d, b, l, '"'+info.replace(/"/g,'""')+'"'].join(','));
    });
    const blob = new Blob([lines.join('\n')], {type:'text/csv'});
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = 'ouispy-pcap-' + new Date().toISOString().replace(/[:.]/g,'-') + '.csv';
    a.click();
  };

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
      src:   q.match(/src:([0-9a-f:]+)/),
      dst:   q.match(/dst:([0-9a-f:]+)/),
      bssid: q.match(/bssid:([0-9a-f:]+)/),
      ssid:  q.match(/ssid:(\S+)/),
      ch:    q.match(/ch:(\d+)/),
      free:  q.replace(/rssi\s*[<>=]\s*-?\d+/g,'')
              .replace(/(type|src|dst|bssid|ssid|ch):\S+/g,'').trim()
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
    if (f.type  && !tr.dataset.type.includes(f.type[1]))   return false;
    if (f.src   && !tr.dataset.src.includes(f.src[1]))     return false;
    if (f.dst   && !tr.dataset.dst.includes(f.dst[1]))     return false;
    if (f.bssid && !tr.dataset.bss.includes(f.bssid[1]))   return false;
    if (f.ssid  && !tr.dataset.info.includes(f.ssid[1]))   return false;
    if (f.ch    && tr.dataset.ch !== f.ch[1])              return false;
    if (f.free) {
      const hay = tr.dataset.type+' '+tr.dataset.src+' '+tr.dataset.dst+' '+
                  tr.dataset.bss+' '+tr.dataset.info+' ch'+tr.dataset.ch+' rssi'+tr.dataset.rssi;
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
    const any = activeChips.size || hitsOnly || f.rssi || f.type || f.src ||
                f.dst || f.bssid || f.ssid || f.ch || f.free;
    $('fltState').textContent = any ? 'on' : 'off';
    let shown = 0;
    rows.querySelectorAll('tr').forEach(tr => {
      const on = rowMatch(tr, f, hitsOnly);
      tr.style.display = on ? '' : 'none';
      if (on) shown++;
    });
    $('rowCount').textContent = shown;
  }

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
        const modeStr = msg.mode_str || '--';
        $('statMode').textContent = modeStr;
        $('statMode').className = 'v ' + (modeStr === 'LOCKED' ? 'good' : 'good');
        $('statChan').textContent = msg.chan_str || '--';
        $('statUp').textContent = fmtUptime(msg.uptime || 0);
        $('statPps').textContent = msg.pps || 0;
        const drop = (msg.dropped_pcap || 0) + (msg.dropped_dash || 0);
        $('statDrop').textContent = drop;
        $('statDrop').className = 'v' + (drop > 0 ? ' bad' : '');
        $('totalPkts').textContent = msg.total || 0;
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

  function markDirty() {
    $('save-status').textContent = 'unsaved';
    $('save-status').className = '';
  }
  document.querySelectorAll('.rail input, .rail select').forEach(el => {
    el.addEventListener('change', markDirty);
    el.addEventListener('input',  markDirty);
  });

  async function loadConfig() {
    try {
      const r = await fetch('/api/config');
      const c = await r.json();
      $('outPcap').checked = c.out === 0;
      $('outText').checked = c.out === 1;
      $('chLock').checked  = c.mode === 0;
      $('chHop').checked   = c.mode === 1;
      setChMode(c.mode === 1 ? 'hop' : 'lock');
      $('lockCh').value = c.chan;
      grid.querySelectorAll('input').forEach(cb => {
        const ch = +cb.dataset.ch;
        const on = ((c.hopmask || 0) & (1 << (ch - 1))) !== 0;
        cb.checked = on;
        cb.parentElement.classList.toggle('checked', on);
      });
      $('dwell').value = c.dwell;
      $('dwellVal').textContent = c.dwell + ' ms';
      $('ftMgmt').checked = (c.ftmask & 1) !== 0;
      $('ftCtrl').checked = (c.ftmask & 2) !== 0;
      $('ftData').checked = (c.ftmask & 4) !== 0;
      $('bssids').value = c.bssids || '';
      $('ouis').value   = c.ouis   || '';
      $('apSsid').value = c.ap_ssid || '';
      $('apPass').value = c.ap_pass || '';
      $('save-status').textContent = 'saved';
      $('save-status').className = 'ok';
    } catch (e) {
      $('save-status').textContent = 'load failed';
      $('save-status').className = 'err';
    }
  }

  $('btnDiscard').onclick = () => loadConfig();

  $('btnSave').onclick = async () => {
    let hopmask = 0;
    grid.querySelectorAll('input').forEach(cb => {
      if (cb.checked) hopmask |= (1 << (+cb.dataset.ch - 1));
    });
    let ftmask = 0;
    if ($('ftMgmt').checked) ftmask |= 1;
    if ($('ftCtrl').checked) ftmask |= 2;
    if ($('ftData').checked) ftmask |= 4;
    const body = {
      out:     $('outPcap').checked ? 0 : 1,
      mode:    $('chLock').checked  ? 0 : 1,
      chan:    parseInt($('lockCh').value, 10),
      hopmask: hopmask,
      dwell:   parseInt($('dwell').value, 10),
      ftmask:  ftmask,
      bssids:  $('bssids').value.trim(),
      ouis:    $('ouis').value.trim()
    };
    $('save-status').textContent = 'applying...';
    $('save-status').className = '';
    try {
      const r = await fetch('/api/config', {
        method: 'POST',
        headers: {'content-type':'application/json'},
        body: JSON.stringify(body)
      });
      if (r.ok) { $('save-status').textContent = 'applied'; $('save-status').className = 'ok'; }
      else      { $('save-status').textContent = 'error';   $('save-status').className = 'err'; }
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
// web_dashboard — AsyncWebServer + WebSocket batching
// ---------------------------------------------------------------------------
namespace web_dashboard {

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
TaskHandle_t   dash_task_h = nullptr;
uint32_t       boot_ms = 0;

size_t append_pkt_json(const capture::Frame& f, char* out, size_t cap) {
    char src[18], dst[18], bssid[18], ssid[64];
    if (f.len >= 22) {
        text_summary::format_mac(f.data + 10, src);
        text_summary::format_mac(f.data + 4,  dst);
        text_summary::format_mac(f.data + 16, bssid);
    } else { src[0] = dst[0] = bssid[0] = 0; }
    text_summary::extract_ssid(f, ssid, sizeof(ssid));

    StaticJsonDocument<512> doc;
    doc["i"] = f.idx;
    doc["t"] = (uint32_t)(millis() - boot_ms);
    doc["c"] = f.channel;
    doc["r"] = (int)f.rssi;
    doc["y"] = text_summary::type_name(f.data[0]);
    doc["s"] = src;
    doc["d"] = dst;
    doc["b"] = bssid;
    doc["l"] = f.len;
    if (ssid[0]) doc["n"] = ssid;

    size_t n = measureJson(doc);
    if (n + 2 > cap) return 0;
    return serializeJson(doc, out, cap);
}

void send_status() {
    if (ws.count() == 0) return;
    StaticJsonDocument<640> doc;
    doc["type"] = "status";
    doc["uptime"] = (uint32_t)((millis() - boot_ms) / 1000);
    doc["pps"]    = capture::packets_per_sec();
    doc["total"]  = capture::total_packets();
    doc["dropped_pcap"] = capture::dropped_pcap();
    doc["dropped_dash"] = capture::dropped_dash();
    doc["session_bytes"] = (uint32_t)session_pcap::size();
    doc["session_cap"]   = (uint32_t)session_pcap::capacity();
    doc["session_drop"]  = (uint32_t)session_pcap::dropped();
    doc["state"]         = session_pcap::state_name();
    doc["psram_free"]    = (uint32_t)ESP.getFreePsram();
    doc["heap_free"]     = (uint32_t)ESP.getFreeHeap();
    doc["fw"] = config::FW_VERSION();

    if (config::get().mode == config::MODE_LOCKED) {
        doc["mode_str"] = "LOCKED";
        char cbuf[16]; snprintf(cbuf, sizeof(cbuf), "%u", capture::current_channel());
        doc["chan_str"] = cbuf;
    } else {
        doc["mode_str"] = "HOP";
        char cbuf[24]; snprintf(cbuf, sizeof(cbuf), "%u (0x%04x)",
            capture::current_channel(), config::get().hopmask);
        doc["chan_str"] = cbuf;
    }

    char buf[640];
    size_t n = serializeJson(doc, buf, sizeof(buf));
    ws.textAll(buf, n);
}

// Batch WS writes: one frame per tick (~33 fps) or when we approach the cap.
// Prevents SoftAP back-pressure under load; still perceptually realtime.
constexpr size_t   BATCH_CAP           = 8192;
constexpr size_t   BATCH_FLUSH_WATER   = 6144;
constexpr uint32_t BATCH_TICK_MS       = 30;
constexpr int      MAX_DRAIN_PER_TICK  = 120;

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
        capture::Frame f;
        int drained = 0;
        while (drained < MAX_DRAIN_PER_TICK && capture::pop_dashboard(&f)) {
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
    doc["mode"]    = c.mode;
    doc["chan"]    = c.chan;
    doc["hopmask"] = c.hopmask;
    doc["dwell"]   = c.dwell_ms;
    doc["ftmask"]  = c.ft_mask;
    doc["ap_ssid"] = c.ap_ssid;
    doc["ap_pass"] = c.ap_pass;
    doc["bssids"]  = c.bssids;
    doc["ouis"]    = c.ouis;
    String body;
    serializeJson(doc, body);
    req->send(200, "application/json", body);
}

// AsyncWebServer frees req->_tempObject with free(), so we accumulate with malloc.
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
    StaticJsonDocument<768> doc;
    DeserializationError err = deserializeJson(doc, (const uint8_t*)req->_tempObject, total);
    if (err) { req->send(400, "application/json", "{\"error\":\"json\"}"); return; }

    bool need_apply_mode   = false;
    bool need_apply_filter = false;

    if (doc.containsKey("chan")) {
        uint8_t ch = doc["chan"];
        if (ch != config::get().chan) { config::set_channel(ch); need_apply_mode = true; }
    }
    if (doc.containsKey("hopmask")) {
        uint16_t hm = doc["hopmask"];
        if (hm != config::get().hopmask) { config::set_hopmask(hm); }
    }
    if (doc.containsKey("dwell")) {
        uint16_t d = doc["dwell"];
        if (d != config::get().dwell_ms) { config::set_dwell(d); }
    }
    if (doc.containsKey("ftmask")) {
        uint8_t m = doc["ftmask"];
        if (m != config::get().ft_mask) { config::set_ftmask(m); need_apply_filter = true; }
    }
    if (doc.containsKey("mode")) {
        uint8_t m = doc["mode"];
        if (m != config::get().mode) { config::set_mode(m); need_apply_mode = true; }
    }
    if (doc.containsKey("bssids")) config::set_bssids(doc["bssids"] | "");
    if (doc.containsKey("ouis"))   config::set_ouis(doc["ouis"]     | "");

    if (need_apply_mode) capture::apply_mode();
    else if (need_apply_filter) capture::apply_filter_mask();

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
    capture::clear_ring();
    req->send(200, "application/json", "{\"ok\":true}");
}

// -- Session state machine endpoints -----------------------------------------
// Legal transitions:
//   IDLE|PAUSED|STOPPED -> RECORDING   (via /api/session/record; clears ring)
//   RECORDING           -> PAUSED      (via /api/session/pause)
//   PAUSED              -> RECORDING   (via /api/session/resume)
//   RECORDING|PAUSED    -> STOPPED     (via /api/session/stop)
// Illegal transitions return HTTP 409 with the current + attempted state.

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
    // Download only permitted from STOPPED. Chunks are read straight from the
    // live ring under the session mutex -- the STOPPED guarantee is what makes
    // this safe. No snapshot buffer. If state leaves STOPPED mid-download
    // (Record wipes the ring) read_chunk() returns 0 and the response truncates.
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
    snprintf(filename, sizeof(filename), "attachment; filename=\"ouispy-pcap-%lu.pcap\"",
             (unsigned long)(millis() / 1000));
    r->addHeader("Content-Disposition", filename);
    r->addHeader("Cache-Control", "no-store");
    req->send(r);
}

bool init() {
    boot_ms = millis();

    server.on("/", HTTP_GET, [](AsyncWebServerRequest* req){
        AsyncWebServerResponse* r = req->beginResponse_P(200, "text/html",
            (const uint8_t*)PCAP_INDEX_HTML, strlen_P(PCAP_INDEX_HTML));
        r->addHeader("Cache-Control", "no-store");
        req->send(r);
    });
    server.on("/api/config",       HTTP_GET,  handle_get_config);
    server.on("/api/config",       HTTP_POST, [](AsyncWebServerRequest* req){}, nullptr, handle_post_config);
    server.on("/api/ap",           HTTP_POST, [](AsyncWebServerRequest* req){}, nullptr, handle_post_ap);
    server.on("/api/reboot",       HTTP_POST, handle_reboot);
    server.on("/api/reset",        HTTP_POST, handle_reset);
    server.on("/api/clear",        HTTP_POST, handle_clear);
    server.on("/api/session.pcap",   HTTP_GET,  handle_session_pcap);
    server.on("/api/session/record", HTTP_POST, handle_session_record);
    server.on("/api/session/pause",  HTTP_POST, handle_session_pause);
    server.on("/api/session/resume", HTTP_POST, handle_session_resume);
    server.on("/api/session/stop",   HTTP_POST, handle_session_stop);

    server.onNotFound([](AsyncWebServerRequest* req){ req->send(404, "text/plain", "not found"); });

    server.addHandler(&ws);
    server.begin();

    xTaskCreatePinnedToCore(&dashboard_task, "pcap_dash", 10240, nullptr, 3, &dash_task_h, 1);
    return true;
}

void tick() { ws.cleanupClients(); }

} // namespace web_dashboard

// ---------------------------------------------------------------------------
// Buzzer + LED + PCAP writer task (was main.cpp in the standalone)
// ---------------------------------------------------------------------------
enum class Fault { None, Wifi };
volatile Fault  g_fault = Fault::None;
TaskHandle_t    pcap_task_h = nullptr;
TaskHandle_t    led_task_h  = nullptr;
volatile uint32_t last_packet_ms = 0;

void pcap_buzzer_setup() {
    ledc_timer_config_t t = {};
    t.speed_mode      = LEDC_LOW_SPEED_MODE;
    t.duty_resolution = LEDC_TIMER_10_BIT;
    t.timer_num       = PCAP_BUZZER_TIMER;
    t.freq_hz         = 1500;
    t.clk_cfg         = LEDC_AUTO_CLK;
    ledc_timer_config(&t);

    ledc_channel_config_t c = {};
    c.gpio_num   = PCAP_BUZZER_PIN;
    c.speed_mode = LEDC_LOW_SPEED_MODE;
    c.channel    = PCAP_BUZZER_CH;
    c.timer_sel  = PCAP_BUZZER_TIMER;
    c.duty       = 0;
    c.hpoint     = 0;
    ledc_channel_config(&c);
}

void pcap_buzzer_chirp(uint16_t freq, uint16_t ms) {
    ledc_set_freq(LEDC_LOW_SPEED_MODE, PCAP_BUZZER_TIMER, freq);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, PCAP_BUZZER_CH, 512);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, PCAP_BUZZER_CH);
    delay(ms);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, PCAP_BUZZER_CH, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, PCAP_BUZZER_CH);
}

// Active-low GPIO21 discrete LED (matches sibling modes). Blink briefly on RX;
// slow heartbeat when idle; solid-on when a fault is set.
void led_task(void*) {
    uint32_t last_pkt_seen = 0;
    uint32_t on_until = 0;
    for (;;) {
        uint32_t now = millis();
        bool on = false;
        if (g_fault != Fault::None) {
            on = true;
        } else {
            uint32_t pkt_ms = last_packet_ms;
            if (pkt_ms != last_pkt_seen) {
                last_pkt_seen = pkt_ms;
                on_until = now + 40;
            }
            if (now < on_until) on = true;
            else if ((now / 1000) % 3 == 0 && (now % 1000) < 60) on = true;
        }
        digitalWrite(PCAP_LED_PIN, on ? LOW : HIGH);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void pcap_writer_task(void*) {
    // USB output is text only. Full PCAP capture lives on the dashboard
    // (GET /api/session.pcap). session_pcap::append fills the download
    // buffer for every frame regardless of USB text emission.
    for (;;) {
        capture::Frame f;
        int drained = 0;
        while (drained < 16 && capture::pop_pcap(&f)) {
            last_packet_ms = millis();
            pcap_stream::write_frame_text(f);
            session_pcap::append(f);
            drained++;
        }
        vTaskDelay(pdMS_TO_TICKS(drained ? 2 : 10));
    }
}

// USB-CDC command protocol — same as the standalone.
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
        wifi_mode_t wm = WIFI_MODE_NULL;
        esp_wifi_get_mode(&wm);
        const char* wm_s = wm == WIFI_MODE_AP ? "AP" : wm == WIFI_MODE_STA ? "STA"
                         : wm == WIFI_MODE_APSTA ? "APSTA" : "NULL";
        IPAddress ip = WiFi.softAPIP();
        String apmac = WiFi.softAPmacAddress();
        Serial.printf("{\"mode\":\"%s\",\"chan\":%u,\"hopmask\":\"0x%04x\",\"dwell\":%u,"
            "\"total\":%u,\"pps\":%u,\"drop_pcap\":%u,\"drop_dash\":%u,\"fw\":\"%s\","
            "\"wifi\":\"%s\",\"ap_ssid\":\"%s\",\"ap_ip\":\"%s\",\"ap_mac\":\"%s\",\"ap_stations\":%u}\n",
            config::get().mode == config::MODE_HOP ? "HOP" : "LOCKED",
            (unsigned)capture::current_channel(),
            (unsigned)config::get().hopmask,
            (unsigned)config::get().dwell_ms,
            (unsigned)capture::total_packets(),
            (unsigned)capture::packets_per_sec(),
            (unsigned)capture::dropped_pcap(),
            (unsigned)capture::dropped_dash(),
            config::FW_VERSION(),
            wm_s, config::get().ap_ssid, ip.toString().c_str(), apmac.c_str(),
            (unsigned)WiFi.softAPgetStationNum());
        return;
    }
    if (U == "VERSION") {
        Serial.printf("OUI-SPY PCAP %s built %s %s\n", config::FW_VERSION(), __DATE__, __TIME__);
        return;
    }
    if (U.startsWith("MODE ")) {
        // Kept as a compatibility no-op. USB output is text only; PCAP
        // binary capture lives on the dashboard at /api/session.pcap.
        reply_ok();
        return;
    }
    if (U.startsWith("CHAN ")) {
        int ch = U.substring(5).toInt();
        if (ch < 1 || ch > 14) { reply_err("bad channel"); return; }
        config::set_mode(config::MODE_LOCKED);
        config::set_channel((uint8_t)ch);
        capture::apply_mode();
        reply_ok(); return;
    }
    if (U.startsWith("HOP ")) {
        String v = U.substring(4); v.trim();
        uint32_t mask = 0;
        if (v.startsWith("0X")) v = v.substring(2);
        mask = strtoul(v.c_str(), nullptr, 16);
        mask &= 0x3FFF;
        if (mask == 0) { reply_err("empty mask"); return; }
        config::set_hopmask((uint16_t)mask);
        config::set_mode(config::MODE_HOP);
        capture::apply_mode();
        reply_ok(); return;
    }
    if (U.startsWith("DWELL ")) {
        int d = U.substring(6).toInt();
        if (d < 100 || d > 2000) { reply_err("bad dwell"); return; }
        config::set_dwell((uint16_t)d);
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

// ---------------------------------------------------------------------------
// Mode entry points (wrapped by mode_pcap.cpp as pcap_ns_setup / pcap_ns_loop)
// ---------------------------------------------------------------------------
void setup() {
    pinMode(PCAP_LED_PIN, OUTPUT);
    digitalWrite(PCAP_LED_PIN, HIGH);   // LED off (inverted)

    pcap_buzzer_setup();

    config::load();

    if (!capture::init()) {
        g_fault = Fault::Wifi;
    }

    session_pcap::init();
    web_dashboard::init();
    // pcap_stream no longer needs init -- USB output is text-only, session
    // buffer for the dashboard is initialized separately.

    // Frame struct embeds a 2500-byte inline buffer, so each pop copies ~2.5KB
    // onto the task stack. These sizes need real headroom on top of that.
    xTaskCreatePinnedToCore(&pcap_writer_task, "pcap_wr",  8192, nullptr, 5, &pcap_task_h, 0);
    xTaskCreatePinnedToCore(&led_task,         "pcap_led", 2048, nullptr, 1, &led_task_h,  0);

    if (g_fault == Fault::None) pcap_buzzer_chirp(1500, 40);
}

void loop() {
    serial_pump();
    web_dashboard::tick();
    delay(20);
}
