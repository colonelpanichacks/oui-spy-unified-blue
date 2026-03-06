#include "ws_broadcast.h"
#include <esp_timer.h>

// ============================================================================
// WebSocket Broadcast Infrastructure
//
// Ring buffer (16 x 512B slots) with spinlock protection.
// Producers call ws::enqueue() from any task (BLE callback, loop, httpd).
// A 50ms esp_timer queues httpd_queue_work to drain the buffer and
// broadcast to connected WS clients via httpd_ws_send_frame_async.
//
// portENTER_CRITICAL_SAFE used throughout — safe from both task and ISR
// contexts on ESP-IDF 4+ dual-core.
// ============================================================================

namespace ws {

// --- Ring buffer ---

static constexpr int SLOT_COUNT = 16;
static constexpr int SLOT_SIZE = 510;

struct WSSlot {
    uint16_t len;
    char payload[SLOT_SIZE];
};

static WSSlot _slots[SLOT_COUNT];
static volatile uint16_t _writeHead = 0;
static volatile uint16_t _readHead = 0;
static portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;

// --- Client tracking (protected by _mux) ---

static constexpr int MAX_CLIENTS = 3;
static int _clientFds[MAX_CLIENTS] = {-1, -1, -1};
static httpd_handle_t _server = nullptr;

// --- Forward declarations ---

static void drainTimerCb(void* arg);
static void drainWork(void* arg);
static esp_err_t wsHandler(httpd_req_t* req);

// --- JSON escape helper (fixed buffer, no heap) ---

int jsonEscape(char* dst, int dstSize, const char* src) {
    int j = 0;
    for (int i = 0; src[i] && j < dstSize - 1; i++) {
        char c = src[i];
        if (c == '"' || c == '\\') {
            if (j + 2 >= dstSize)
                break;
            dst[j++] = '\\';
            dst[j++] = c;
        } else if (c < 0x20) {
            // Skip control characters
        } else {
            dst[j++] = c;
        }
    }
    dst[j] = '\0';
    return j;
}

// ============================================================================
// Public API
// ============================================================================

void init(httpd_handle_t httpServer) {
    _server = httpServer;
    if (!_server)
        return;

    // Register WS endpoint on HTTP server only (no TLS overhead)
    static const httpd_uri_t wsUri = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = wsHandler,
        .user_ctx = nullptr,
        .is_websocket = true,
    };
    httpd_register_uri_handler(_server, &wsUri);

    // 50ms periodic drain timer
    esp_timer_create_args_t timerArgs = {};
    timerArgs.callback = drainTimerCb;
    timerArgs.name = "ws_drain";

    esp_timer_handle_t timer = nullptr;
    if (esp_timer_create(&timerArgs, &timer) == ESP_OK) {
        esp_timer_start_periodic(timer, 50000); // 50ms in microseconds
    }

    Serial.println("[WS] WebSocket broadcast initialized on /ws");
}

bool enqueue(const char* topic, const char* json) {
    // Format: {"t":"topic","d":json}
    char buf[SLOT_SIZE];
    int written = snprintf(buf, sizeof(buf), "{\"t\":\"%s\",\"d\":%s}", topic, json);
    if (written < 0 || written >= (int)sizeof(buf))
        return false;

    portENTER_CRITICAL_SAFE(&_mux);
    uint16_t head = _writeHead;
    _slots[head].len = (uint16_t)written;
    memcpy(_slots[head].payload, buf, written);
    _writeHead = (head + 1) % SLOT_COUNT;
    // Overflow: drop oldest by advancing read head
    if (_writeHead == _readHead)
        _readHead = (_readHead + 1) % SLOT_COUNT;
    portEXIT_CRITICAL_SAFE(&_mux);

    return true;
}

bool enqueueDoc(const char* topic, JsonDocument& doc) {
    char jsonBuf[SLOT_SIZE - 32]; // Leave room for wrapper
    size_t len = serializeJson(doc, jsonBuf, sizeof(jsonBuf));
    if (len == 0 || len >= sizeof(jsonBuf))
        return false;
    return enqueue(topic, jsonBuf);
}

int clientCount() {
    portENTER_CRITICAL_SAFE(&_mux);
    int count = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (_clientFds[i] >= 0)
            count++;
    }
    portEXIT_CRITICAL_SAFE(&_mux);
    return count;
}

// ============================================================================
// WS Handler — open/close/text frames
// ============================================================================

static esp_err_t wsHandler(httpd_req_t* req) {
    if (req->method == HTTP_GET) {
        // New WS connection — register client fd
        int fd = httpd_req_to_sockfd(req);
        portENTER_CRITICAL_SAFE(&_mux);
        // Find empty slot
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (_clientFds[i] < 0) {
                _clientFds[i] = fd;
                portEXIT_CRITICAL_SAFE(&_mux);
                Serial.printf("[WS] Client connected fd=%d\n", fd);
                return ESP_OK;
            }
        }
        // All slots full — evict oldest (slot 0), shift others down
        int evictFd = _clientFds[0];
        for (int i = 0; i < MAX_CLIENTS - 1; i++)
            _clientFds[i] = _clientFds[i + 1];
        _clientFds[MAX_CLIENTS - 1] = fd;
        portEXIT_CRITICAL_SAFE(&_mux);

        // Close evicted socket outside lock
        if (evictFd >= 0)
            httpd_sess_trigger_close(_server, evictFd);
        Serial.printf("[WS] Client connected fd=%d (evicted fd=%d)\n", fd, evictFd);
        return ESP_OK;
    }

    // Receive text frame (commands from client)
    httpd_ws_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.type = HTTPD_WS_TYPE_TEXT;

    // First call with len=0 gets the frame length
    esp_err_t ret = httpd_ws_recv_frame(req, &frame, 0);
    if (ret != ESP_OK)
        return ret;

    if (frame.len == 0)
        return ESP_OK;

    if (frame.len > 512)
        return ESP_OK; // Ignore oversized frames

    char buf[513];
    frame.payload = (uint8_t*)buf;
    ret = httpd_ws_recv_frame(req, &frame, frame.len);
    if (ret != ESP_OK)
        return ret;
    buf[frame.len] = '\0';

    // Parse command: {"c":"action","p":{...}}
    JsonDocument cmdDoc;
    if (deserializeJson(cmdDoc, buf) != DeserializationError::Ok)
        return ESP_OK;

    const char* action = cmdDoc["c"];
    if (!action)
        return ESP_OK;

    // Command dispatch — reuses existing module methods via HTTP handlers.
    // Future: add direct WS command dispatch here.
    // For now, acknowledge receipt.
    char esc[128];
    jsonEscape(esc, sizeof(esc), action);
    char ack[256];
    snprintf(ack, sizeof(ack), "{\"t\":\"cmd/ack\",\"d\":{\"action\":\"%s\",\"ok\":true}}", esc);

    httpd_ws_frame_t ackFrame;
    memset(&ackFrame, 0, sizeof(ackFrame));
    ackFrame.type = HTTPD_WS_TYPE_TEXT;
    ackFrame.payload = (uint8_t*)ack;
    ackFrame.len = strlen(ack);
    return httpd_ws_send_frame(req, &ackFrame);
}

// ============================================================================
// Drain Timer — broadcasts pending messages to all connected clients
// ============================================================================

static void drainTimerCb(void* arg) {
    if (!_server)
        return;

    // Check if there are pending messages and clients under single lock
    portENTER_CRITICAL_SAFE(&_mux);
    bool hasPending = (_readHead != _writeHead);
    bool hasClients = false;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (_clientFds[i] >= 0) {
            hasClients = true;
            break;
        }
    }
    if (!hasClients && hasPending) {
        // Discard all pending — no one listening
        _readHead = _writeHead;
    }
    portEXIT_CRITICAL_SAFE(&_mux);

    if (!hasPending || !hasClients)
        return;

    httpd_queue_work(_server, drainWork, nullptr);
}

static void drainWork(void* arg) {
    // Drain all pending slots and broadcast to clients
    while (true) {
        portENTER_CRITICAL_SAFE(&_mux);
        if (_readHead == _writeHead) {
            portEXIT_CRITICAL_SAFE(&_mux);
            break;
        }
        uint16_t idx = _readHead;
        uint16_t len = _slots[idx].len;
        // Copy payload out under lock (fixed-size, no heap)
        char payload[SLOT_SIZE];
        memcpy(payload, _slots[idx].payload, len);
        _readHead = (idx + 1) % SLOT_COUNT;
        portEXIT_CRITICAL_SAFE(&_mux);

        // Snapshot client fds under lock
        int fds[MAX_CLIENTS];
        portENTER_CRITICAL_SAFE(&_mux);
        for (int i = 0; i < MAX_CLIENTS; i++)
            fds[i] = _clientFds[i];
        portEXIT_CRITICAL_SAFE(&_mux);

        // Broadcast to all connected clients
        httpd_ws_frame_t frame;
        memset(&frame, 0, sizeof(frame));
        frame.type = HTTPD_WS_TYPE_TEXT;
        frame.payload = (uint8_t*)payload;
        frame.len = len;

        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (fds[i] < 0)
                continue;

            esp_err_t ret = httpd_ws_send_frame_async(_server, fds[i], &frame);
            if (ret != ESP_OK) {
                Serial.printf("[WS] Client fd=%d disconnected (err=%d)\n", fds[i], ret);
                portENTER_CRITICAL_SAFE(&_mux);
                // Only clear if it hasn't been reassigned
                for (int j = 0; j < MAX_CLIENTS; j++) {
                    if (_clientFds[j] == fds[i])
                        _clientFds[j] = -1;
                }
                portEXIT_CRITICAL_SAFE(&_mux);
            }
        }
    }
}

} // namespace ws
