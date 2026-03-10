#pragma once

#include "../hal/ble_mgr.h"
#include "module.h"
#include "opendroneid.h"
#include <cstdint>
#include <esp_http_server.h>
#include <esp_wifi_types.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#define SS_MAX_UAVS 8
#define SS_MAX_PKT_LEN 400
#define SS_PKT_QUEUE_LEN 4

struct SSUavData {
    uint8_t mac[6];
    int rssi;
    uint32_t lastSeen;
    char opId[ODID_ID_SIZE + 1];
    char uavId[ODID_ID_SIZE + 1];
    double latD;
    double longD;
    double baseLatD;
    double baseLongD;
    int altitudeMsl;
    int heightAgl;
    int speed;
    int heading;
    int flag;
};

class SkySpyModule : public IModule, public hal::BLEListener {
  public:
    const char* name() override {
        return "skyspy";
    }
    void setup() override;
    void loop() override;
    void registerRoutes(httpd_handle_t https, httpd_handle_t http) override;
    bool isEnabled() override;
    void setEnabled(bool enabled) override;

    // hal::BLEListener
    void onBLEAdvertisement(const NimBLEAdvertisedDevice* device) override;

    // Process queued WiFi frames (called from loop)
    void processWiFiQueue();
    void handleWiFiFrame(const uint8_t* payload, int length, int rssi);

    QueueHandle_t pktQueue() const {
        return _pktQueue;
    }

    // Public accessors for route handlers
    SemaphoreHandle_t uavMutex() const {
        return _mutex;
    }

    const SSUavData& uavAt(int index) const {
        return _uavs[index];
    }
    bool deviceInRange() const {
        return _deviceInRange;
    }

  private:
    bool _enabled = false;
    SSUavData _uavs[SS_MAX_UAVS] = {};
    ODID_UAS_Data _uasData = {};
    SemaphoreHandle_t _mutex = nullptr;
    QueueHandle_t _pktQueue = nullptr;
    bool _deviceInRange = false;
    unsigned long _lastHeartbeat = 0;
    unsigned long _lastStatus = 0;

    char _pendingWsJson[384] = {};

    SSUavData* nextUav(const uint8_t* mac);
    void sendJSON(const SSUavData* uav);
    void flushPendingWs();
    void extractFromODID(SSUavData* uav);
    void triggerDetection();
};

// Static WiFi promiscuous callback (cannot be a member function)
void skyspyWiFiCallback(void* buf, wifi_promiscuous_pkt_type_t type);
