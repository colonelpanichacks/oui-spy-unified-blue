#pragma once

#include <ArduinoJson.h>
#include <esp_http_server.h>
#include <freertos/portmacro.h>

class IModule;

// ============================================================================
// WebSocket Broadcast — ring buffer + drain timer for real-time event streaming
// ============================================================================

namespace ws {

// --- Topic name constants ---
namespace topic {
constexpr const char* SYS_STATUS = "sys/status";
constexpr const char* SYS_GPS = "sys/gps";
constexpr const char* SYS_MODULES = "sys/modules";
constexpr const char* DET_MATCH = "det/match";
constexpr const char* FOX_STATUS = "fox/status";
constexpr const char* FY_DETECTION = "fy/detection";
constexpr const char* FY_STATS = "fy/stats";
constexpr const char* SS_DRONE = "ss/drone";
constexpr const char* SS_STATUS = "ss/status";
constexpr const char* WD_SIGHTING = "wd/sighting";
constexpr const char* WD_STATUS = "wd/status";
} // namespace topic

void init(httpd_handle_t httpServer);
bool enqueue(const char* topic, const char* json);
bool enqueueDoc(const char* topic, JsonDocument& doc);
int clientCount();
bool hasClients();

// Serialize a module list as JSON array and enqueue as sys/modules.
// Used by both heartbeat and module-toggle to avoid duplication.
void pushModuleList(IModule** modules, int count);

// Escape a string for safe JSON embedding (quotes, backslashes, control chars).
// Returns number of bytes written (excluding null terminator).
int jsonEscape(char* dst, int dstSize, const char* src);

} // namespace ws
