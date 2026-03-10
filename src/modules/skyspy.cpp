#include "skyspy.h"
#include "../hal/led.h"
#include "../hal/notify.h"
#include "../hal/wifi_mgr.h"
#include "../web/routes.h"
#include "../web/ws_broadcast.h"
#include "odid_wifi.h"
#include "opendroneid.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// Queued packet for deferred processing outside WiFi driver task
struct SSQueuedPkt {
    uint8_t data[SS_MAX_PKT_LEN];
    int length;
    int rssi;
};

// ============================================================================
// Sky Spy Module: Open Drone ID detection via BLE + WiFi promiscuous mode
// ============================================================================

// Pointer to the module instance for the static WiFi callback
static SkySpyModule* ssInstance = nullptr;

// ============================================================================
// Helpers
// ============================================================================

SSUavData* SkySpyModule::nextUav(const uint8_t* mac) {
    for (int i = 0; i < SS_MAX_UAVS; i++) {
        if (memcmp(_uavs[i].mac, mac, 6) == 0)
            return &_uavs[i];
    }
    for (int i = 0; i < SS_MAX_UAVS; i++) {
        if (_uavs[i].mac[0] == 0)
            return &_uavs[i];
    }
    return &_uavs[0];
}

void SkySpyModule::sendJSON(const SSUavData* uav) {
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02x:%02x:%02x:%02x:%02x:%02x", uav->mac[0], uav->mac[1],
             uav->mac[2], uav->mac[3], uav->mac[4], uav->mac[5]);
    Serial.printf("{\"module\":\"skyspy\",\"mac\":\"%s\",\"rssi\":%d,"
                  "\"drone_lat\":%.6f,\"drone_long\":%.6f,\"drone_altitude\":%d,"
                  "\"pilot_lat\":%.6f,\"pilot_long\":%.6f,\"basic_id\":\"%s\"}\n",
                  macStr, uav->rssi, uav->latD, uav->longD, uav->altitudeMsl, uav->baseLatD,
                  uav->baseLongD, uav->uavId);

    // Copy fields needed for WS push (will be sent outside mutex by caller)
    _pendingWsJson[0] = '\0';
    snprintf(_pendingWsJson, sizeof(_pendingWsJson),
             "{\"mac\":\"%s\",\"rssi\":%d,\"drone_lat\":%.6f,\"drone_long\":%.6f,"
             "\"altitude\":%d,\"height\":%d,\"speed\":%d,\"heading\":%d,"
             "\"pilot_lat\":%.6f,\"pilot_long\":%.6f,\"uav_id\":\"%s\",\"op_id\":\"%s\"}",
             macStr, uav->rssi, uav->latD, uav->longD, uav->altitudeMsl, uav->heightAgl,
             uav->speed, uav->heading, uav->baseLatD, uav->baseLongD, uav->uavId, uav->opId);
}

void SkySpyModule::flushPendingWs() {
    if (_pendingWsJson[0] != '\0') {
        ws::enqueue(ws::topic::SS_DRONE, _pendingWsJson);
        _pendingWsJson[0] = '\0';
    }
}

void SkySpyModule::extractFromODID(SSUavData* uav) {
    if (_uasData.BasicIDValid[0]) {
        strncpy(uav->uavId, (char*)_uasData.BasicID[0].UASID, ODID_ID_SIZE);
        uav->uavId[ODID_ID_SIZE] = '\0';
    }
    if (_uasData.LocationValid) {
        uav->latD = _uasData.Location.Latitude;
        uav->longD = _uasData.Location.Longitude;
        uav->altitudeMsl = (int)_uasData.Location.AltitudeGeo;
        uav->heightAgl = (int)_uasData.Location.Height;
        uav->speed = (int)_uasData.Location.SpeedHorizontal;
        uav->heading = (int)_uasData.Location.Direction;
    }
    if (_uasData.SystemValid) {
        uav->baseLatD = _uasData.System.OperatorLatitude;
        uav->baseLongD = _uasData.System.OperatorLongitude;
    }
    if (_uasData.OperatorIDValid) {
        strncpy(uav->opId, (char*)_uasData.OperatorID.OperatorId, ODID_ID_SIZE);
        uav->opId[ODID_ID_SIZE] = '\0';
    }
}

void SkySpyModule::triggerDetection() {
    if (!_deviceInRange) {
        _deviceInRange = true;
        _lastHeartbeat = millis();
        hal::notify(hal::NOTIFY_SS_DRONE);
        Serial.println("[SKYSPY] Drone detected!");
    }
}

// ============================================================================
// WiFi Promiscuous Callback (static, runs in WiFi task context)
// ============================================================================

void skyspyWiFiCallback(void* buffer, wifi_promiscuous_pkt_type_t type) {
    if (!ssInstance || type != WIFI_PKT_MGMT)
        return;

    wifi_promiscuous_pkt_t* packet = (wifi_promiscuous_pkt_t*)buffer;
    int length = packet->rx_ctrl.sig_len;
    if (length > 4)
        length -= 4; // Strip FCS

    if (length <= 0 || length > SS_MAX_PKT_LEN)
        return;

    // Quick-filter: only action frames (0xd0) or beacons (0x80)
    uint8_t subtype = packet->payload[0];
    if (subtype != 0xd0 && subtype != 0x80)
        return;

    // Static buffer — callback runs in WiFi driver task, avoid stack pressure
    static SSQueuedPkt pkt;
    memcpy(pkt.data, packet->payload, length);
    pkt.length = length;
    pkt.rssi = packet->rx_ctrl.rssi;

    // Non-blocking enqueue — drop if full
    xQueueSend(ssInstance->pktQueue(), &pkt, 0);
}

void SkySpyModule::processWiFiQueue() {
    static SSQueuedPkt pkt;
    while (xQueueReceive(_pktQueue, &pkt, 0) == pdTRUE) {
        handleWiFiFrame(pkt.data, pkt.length, pkt.rssi);
    }
}

void SkySpyModule::handleWiFiFrame(const uint8_t* payload, int length, int rssi) {
    if (!_enabled || !payload || length < 1)
        return;

    // NAN Action Frames (drone Remote ID)
    // Need at least 16 bytes: 4 (frame ctrl) + 6 (NAN dest) + 6 (source MAC)
    static const uint8_t nan_dest[6] = {0x51, 0x6f, 0x9a, 0x01, 0x00, 0x00};
    if (length >= 16 && memcmp(nan_dest, &payload[4], 6) == 0) {
        char nan_mac[6] = {0};
        if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            memset(&_uasData, 0, sizeof(_uasData));
            if (odid_wifi_receive_message_pack_nan_action_frame(&_uasData, nan_mac,
                                                                (uint8_t*)payload, length) == 0) {
                SSUavData uav;
                memset(&uav, 0, sizeof(uav));
                memcpy(uav.mac, &payload[10], 6);
                uav.rssi = rssi;
                uav.lastSeen = millis();
                extractFromODID(&uav);
                SSUavData* stored = nextUav(uav.mac);
                *stored = uav;
                stored->flag = 1;
                triggerDetection();
                sendJSON(stored);
            }
            xSemaphoreGive(_mutex);
            flushPendingWs();
        }
    }
    // Beacon frames with vendor-specific ODID IEs
    // Need at least 37 bytes: 36 (beacon header) + 1 (first IE byte)
    else if (length >= 37 && payload[0] == 0x80) {
        int offset = 36;
        while (offset + 2 <= length) {
            int typ = payload[offset];
            int len = payload[offset + 1];
            if (offset + 2 + len > length)
                break;
            if ((typ == 0xdd) && (len >= 5) &&
                (((payload[offset + 2] == 0x90 && payload[offset + 3] == 0x3a &&
                   payload[offset + 4] == 0xe6)) ||
                 ((payload[offset + 2] == 0xfa && payload[offset + 3] == 0x0b &&
                   payload[offset + 4] == 0xbc)))) {
                int j = offset + 7;
                if (j < length && length >= 16) {
                    if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                        memset(&_uasData, 0, sizeof(_uasData));
                        odid_message_process_pack(&_uasData, (uint8_t*)&payload[j], length - j);
                        SSUavData uav;
                        memset(&uav, 0, sizeof(uav));
                        memcpy(uav.mac, &payload[10], 6);
                        uav.rssi = rssi;
                        uav.lastSeen = millis();
                        extractFromODID(&uav);
                        SSUavData* stored = nextUav(uav.mac);
                        *stored = uav;
                        stored->flag = 1;
                        triggerDetection();
                        sendJSON(stored);
                        xSemaphoreGive(_mutex);
                        flushPendingWs();
                    }
                }
            }
            offset += len + 2;
        }
    }
}

// ============================================================================
// Module Implementation
// ============================================================================

void SkySpyModule::setup() {
    _mutex = xSemaphoreCreateMutex();
    _pktQueue = xQueueCreate(SS_PKT_QUEUE_LEN, sizeof(SSQueuedPkt));
    memset(_uavs, 0, sizeof(_uavs));
    ssInstance = this;
    Serial.println("[SKYSPY] Module initialized (BLE active, WiFi scan via reboot)");
}

void SkySpyModule::loop() {
    if (!_enabled)
        return;

    // Process any WiFi frames queued by the promiscuous callback
    // (queue is only populated in scan mode; no-op in normal mode)
    processWiFiQueue();

    unsigned long now = millis();

    // Status heartbeat every 60s (serial) / 5s (WS)
    if (now - _lastStatus > 60000UL) {
        Serial.println("{\"module\":\"skyspy\",\"status\":\"scanning\"}");
        _lastStatus = now;
    }

    // Push ss/status over WS every 5s
    static unsigned long lastWsPush = 0;
    if (now - lastWsPush >= 5000) {
        int droneCount = 0;
        bool inRange = _deviceInRange;
        if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            for (int i = 0; i < SS_MAX_UAVS; i++) {
                if (_uavs[i].mac[0] != 0 && (now - _uavs[i].lastSeen) < 7000)
                    droneCount++;
            }
            xSemaphoreGive(_mutex);
        }
        char json[96];
        snprintf(json, sizeof(json),
                 "{\"in_range\":%s,\"active_drones\":%d,\"wifi_scanning\":false}",
                 inRange ? "true" : "false", droneCount);
        ws::enqueue(ws::topic::SS_STATUS, json);
        lastWsPush = now;
    }

    // Heartbeat pulse if drone in range
    if (_deviceInRange) {
        if (now - _lastHeartbeat >= 5000) {
            hal::notify(hal::NOTIFY_SS_HEARTBEAT);
            _lastHeartbeat = now;
        }
        // Check if all drones out of range (7s timeout)
        bool anyDetected = false;
        if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            for (int i = 0; i < SS_MAX_UAVS; i++) {
                if (_uavs[i].mac[0] != 0 && (now - _uavs[i].lastSeen) < 7000) {
                    anyDetected = true;
                    break;
                }
            }
            xSemaphoreGive(_mutex);
        }
        if (!anyDetected) {
            Serial.println("{\"module\":\"skyspy\",\"status\":\"drone_out_of_range\"}");
            _deviceInRange = false;
        }
    }
}

void SkySpyModule::onBLEAdvertisement(const NimBLEAdvertisedDevice* device) {
    if (!_enabled)
        return;

    // Check for ODID BLE service data (UUID 0xFFFA)
    static const NimBLEUUID ODID_SVC_UUID(static_cast<uint16_t>(0xFFFA));
    std::string svcData = device->getServiceData(ODID_SVC_UUID);
    if (svcData.size() < 25) // ODID single messages are 25 bytes
        return;

    NimBLEAddress addr = device->getAddress();
    const uint8_t* mac = addr.getVal();
    if (!_mutex || xSemaphoreTake(_mutex, pdMS_TO_TICKS(50)) != pdTRUE)
        return;

    SSUavData* uav = nextUav(mac);
    uav->lastSeen = millis();
    uav->rssi = device->getRSSI();
    memcpy(uav->mac, mac, 6);

    // Service data starts after the UUID, so first byte is the ODID message type
    const uint8_t* odid = reinterpret_cast<const uint8_t*>(svcData.data());
    switch (odid[0] & 0xF0) {
    case 0x00: {
        ODID_BasicID_data basic;
        decodeBasicIDMessage(&basic, (ODID_BasicID_encoded*)odid);
        strncpy(uav->uavId, (char*)basic.UASID, ODID_ID_SIZE);
        uav->uavId[ODID_ID_SIZE] = '\0';
        break;
    }
    case 0x10: {
        ODID_Location_data loc;
        decodeLocationMessage(&loc, (ODID_Location_encoded*)odid);
        uav->latD = loc.Latitude;
        uav->longD = loc.Longitude;
        uav->altitudeMsl = (int)loc.AltitudeGeo;
        uav->heightAgl = (int)loc.Height;
        uav->speed = (int)loc.SpeedHorizontal;
        uav->heading = (int)loc.Direction;
        break;
    }
    case 0x40: {
        ODID_System_data sys;
        decodeSystemMessage(&sys, (ODID_System_encoded*)odid);
        uav->baseLatD = sys.OperatorLatitude;
        uav->baseLongD = sys.OperatorLongitude;
        break;
    }
    case 0x50: {
        ODID_OperatorID_data op;
        decodeOperatorIDMessage(&op, (ODID_OperatorID_encoded*)odid);
        strncpy(uav->opId, (char*)op.OperatorId, ODID_ID_SIZE);
        uav->opId[ODID_ID_SIZE] = '\0';
        break;
    }
    }
    uav->flag = 1;
    triggerDetection();
    sendJSON(uav);
    xSemaphoreGive(_mutex);
    flushPendingWs();
}

// ============================================================================
// Web Routes
// ============================================================================

void SkySpyModule::registerRoutes(httpd_handle_t https, httpd_handle_t http) {
    registerSkySpyRoutes(https, http, *this);
}

bool SkySpyModule::isEnabled() {
    return _enabled;
}
void SkySpyModule::setEnabled(bool enabled) {
    _enabled = enabled;
    if (!enabled) {
        _deviceInRange = false;
    }
}
