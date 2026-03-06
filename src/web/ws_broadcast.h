#pragma once

#include <ArduinoJson.h>
#include <esp_http_server.h>
#include <freertos/portmacro.h>

// ============================================================================
// WebSocket Broadcast — ring buffer + drain timer for real-time event streaming
// ============================================================================

namespace ws {

void init(httpd_handle_t httpServer);
bool enqueue(const char* topic, const char* json);
bool enqueueDoc(const char* topic, JsonDocument& doc);
int clientCount();

// Escape a string for safe JSON embedding (quotes, backslashes, control chars).
// Returns number of bytes written (excluding null terminator).
int jsonEscape(char* dst, int dstSize, const char* src);

} // namespace ws
