#line 1 "/Users/nicholastenbrink/projects/nesso/oui_spy_nesso/raw/skyspy.cpp"
#if !defined(ARDUINO_ARCH_ESP32)
  #error "This program requires an ESP32"
#endif

#include <Arduino.h>
#include <HardwareSerial.h>
// BLE headers provided by wrapper (NimBLE)
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <nvs_flash.h>
#include "opendroneid.h"
#include "odid_wifi.h"
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Audio Configuration
#define DETECT_FREQ 1000  // Detection alert - high pitch (faster beeps)
#define HEARTBEAT_FREQ 600 // Heartbeat pulse frequency
#define DETECT_BEEP_DURATION 150 // Detection beep duration (faster)
#define HEARTBEAT_DURATION 100   // Short heartbeat pulse

struct id_data {
  uint8_t  mac[6];
  int      rssi;
  uint32_t last_seen;
  char     op_id[ODID_ID_SIZE + 1];
  char     uav_id[ODID_ID_SIZE + 1];
  double   lat_d;
  double   long_d;
  double   base_lat_d;
  double   base_long_d;
  int      altitude_msl;
  int      height_agl;
  int      speed;
  int      heading;
  int      flag;
};

void callback(void *, wifi_promiscuous_pkt_type_t);
void send_json_fast(const id_data *UAV);
void buzzerTask(void *parameter);

#define MAX_UAVS 32
id_data uavs[MAX_UAVS] = {0};
NimBLEScan* pBLEScan = nullptr;
ODID_UAS_Data UAS_data;
unsigned long last_status = 0;
unsigned long last_heartbeat = 0;

// Buzzer toggle (shared via NVS from main selector menu)
static bool ssBuzzerOn = true;

// Thread-safe flags for buzzer (volatile for ISR access)
volatile bool device_in_range = false;
volatile bool trigger_detection_beep = false;
volatile bool trigger_heartbeat_beep = false;
static portMUX_TYPE buzzerMux = portMUX_INITIALIZER_UNLOCKED;

static QueueHandle_t printQueue;

id_data* next_uav(uint8_t* mac) {
  for (int i = 0; i < MAX_UAVS; i++) {
    if (memcmp(uavs[i].mac, mac, 6) == 0)
      return &uavs[i];
  }
  for (int i = 0; i < MAX_UAVS; i++) {
    if (uavs[i].mac[0] == 0)
      return &uavs[i];
  }
  return &uavs[0];
}

class MyAdvertisedDeviceCallbacks : public NimBLEAdvertisedDeviceCallbacks {
public:
  void onResult(const NimBLEAdvertisedDevice* device) override {
    size_t len = 0;
    const uint8_t* payload = nimbleAdvPayload(device, &len);
    if (!payload || len < 6 + (int)sizeof(ODID_BasicID_encoded)) return;

    if (payload[1] != 0x16 || payload[2] != 0xFA ||
        payload[3] != 0xFF || payload[4] != 0x0D) return;

    const uint8_t* odid = &payload[6];
    int odid_len = len - 6;

    memset(&UAS_data, 0, sizeof(UAS_data));
    if ((odid[0] & 0xF0) == (ODID_MESSAGETYPE_PACKED << 4)) {
      odid_message_process_pack(&UAS_data, const_cast<uint8_t*>(odid), odid_len);
    } else {
      decodeOpenDroneID(&UAS_data, const_cast<uint8_t*>(odid));
    }

    bool useful = UAS_data.BasicIDValid[0] || UAS_data.LocationValid ||
                  UAS_data.SystemValid     || UAS_data.OperatorIDValid;
    if (!useful) return;

    const uint8_t* mac = nimbleMacBytes(device);
    if (!mac) return;

    id_data* UAV = next_uav(const_cast<uint8_t*>(mac));
    if (memcmp(UAV->mac, mac, 6) != 0) {
      memset(UAV, 0, sizeof(*UAV));
      memcpy(UAV->mac, mac, 6);
    }
    UAV->last_seen = millis();
    UAV->rssi = device->getRSSI();

    if (UAS_data.BasicIDValid[0])
      strncpy(UAV->uav_id, (char*)UAS_data.BasicID[0].UASID, ODID_ID_SIZE);
    if (UAS_data.LocationValid) {
      UAV->lat_d        = UAS_data.Location.Latitude;
      UAV->long_d       = UAS_data.Location.Longitude;
      UAV->altitude_msl = (int)UAS_data.Location.AltitudeGeo;
      UAV->height_agl   = (int)UAS_data.Location.Height;
      UAV->speed        = (int)UAS_data.Location.SpeedHorizontal;
      UAV->heading      = (int)UAS_data.Location.Direction;
    }
    if (UAS_data.SystemValid) {
      UAV->base_lat_d  = UAS_data.System.OperatorLatitude;
      UAV->base_long_d = UAS_data.System.OperatorLongitude;
    }
    if (UAS_data.OperatorIDValid)
      strncpy(UAV->op_id, (char*)UAS_data.OperatorID.OperatorId, ODID_ID_SIZE);
    UAV->flag = 1;

    portENTER_CRITICAL_ISR(&buzzerMux);
    if (!device_in_range) {
      trigger_detection_beep = true;
      device_in_range = true;
      last_heartbeat = millis();
    }
    portEXIT_CRITICAL_ISR(&buzzerMux);

    id_data tmp = *UAV;
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xQueueSendFromISR(printQueue, &tmp, &xHigherPriorityTaskWoken);
    if (xHigherPriorityTaskWoken) portYIELD_FROM_ISR();
  }
};

// Dedicated non-blocking buzzer task - never delays detection
void buzzerTask(void *parameter) {
  for (;;) {
    // Check for detection beep trigger
    portENTER_CRITICAL(&buzzerMux);
    bool do_detection = trigger_detection_beep;
    if (do_detection) trigger_detection_beep = false;
    portEXIT_CRITICAL(&buzzerMux);
    
    if (do_detection) {
      Serial.println("DRONE DETECTED! Playing alert sequence");
      for (int i = 0; i < 3; i++) {
        if (ssBuzzerOn) tone(BUZZER_PIN, DETECT_FREQ, DETECT_BEEP_DURATION);
        boardLedOn();
        vTaskDelay(pdMS_TO_TICKS(150));
        boardLedOff();
        vTaskDelay(pdMS_TO_TICKS(50)); // Short pause between beeps
      }
      Serial.println("Detection complete - drone identified!");
    }
    
    // Check for heartbeat beep trigger
    portENTER_CRITICAL(&buzzerMux);
    bool do_heartbeat = trigger_heartbeat_beep;
    if (do_heartbeat) trigger_heartbeat_beep = false;
    portEXIT_CRITICAL(&buzzerMux);
    
    if (do_heartbeat) {
      Serial.println("Heartbeat: Drone still in range");
      if (ssBuzzerOn) tone(BUZZER_PIN, HEARTBEAT_FREQ, HEARTBEAT_DURATION);
      boardLedOn();
      vTaskDelay(pdMS_TO_TICKS(100));
      boardLedOff();
      vTaskDelay(pdMS_TO_TICKS(50));
      if (ssBuzzerOn) tone(BUZZER_PIN, HEARTBEAT_FREQ, HEARTBEAT_DURATION);
      boardLedOn();
      vTaskDelay(pdMS_TO_TICKS(100));
      boardLedOff();
    }
    
    // Check for new beep triggers every 50ms
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

void send_json_fast(const id_data *UAV) {
  char mac_str[18];
  snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
           UAV->mac[0], UAV->mac[1], UAV->mac[2],
           UAV->mac[3], UAV->mac[4], UAV->mac[5]);
  char json_msg[256];
  snprintf(json_msg, sizeof(json_msg),
    "{\"mac\":\"%s\",\"rssi\":%d,\"drone_lat\":%.6f,\"drone_long\":%.6f,\"drone_altitude\":%d,\"pilot_lat\":%.6f,\"pilot_long\":%.6f,\"basic_id\":\"%s\"}",
    mac_str, UAV->rssi, UAV->lat_d, UAV->long_d, UAV->altitude_msl,
    UAV->base_lat_d, UAV->base_long_d, UAV->uav_id);
  Serial.println(json_msg);
  nessoUiFlashAlert();
  nessoUiSetStatus("Drone detected");
}

// Mesh functionality removed - this is now a pure USB serial drone scanner

void bleScanTask(void *parameter) {
  // NimBLE-Arduino 2.x start() is in milliseconds (0 = continuous). The
  // original start(1) scanned for 1 ms per loop and missed nearly everything.
  pBLEScan->start(0, false);
  for (;;) {
    if (!pBLEScan->isScanning()) {
      pBLEScan->start(0, false);
    }
    pBLEScan->clearResults();
    // No flag checking needed - BLE callback handles buzzer triggering
    delay(1000);
  }
}

void wifiProcessTask(void *parameter) {
  for (;;) {
    // No-op: callback sets uavs[].flag and data, so nothing needed here
    delay(10);
  }
}

void callback(void *buffer, wifi_promiscuous_pkt_type_t type) {
  if (type != WIFI_PKT_MGMT) return;
  
  wifi_promiscuous_pkt_t *packet = (wifi_promiscuous_pkt_t *)buffer;
  uint8_t *payload = packet->payload;
  int length = packet->rx_ctrl.sig_len;
  
  static const uint8_t nan_dest[6] = {0x51, 0x6f, 0x9a, 0x01, 0x00, 0x00};
  if (memcmp(nan_dest, &payload[4], 6) == 0) {
    char nan_mac[6] = {0};  // receive buffer for source MAC (library writes to this)
    if (odid_wifi_receive_message_pack_nan_action_frame(&UAS_data, nan_mac, payload, length) == 0) {
      id_data UAV;
      memset(&UAV, 0, sizeof(UAV));
      memcpy(UAV.mac, &payload[10], 6);
      UAV.rssi = packet->rx_ctrl.rssi;
      UAV.last_seen = millis();
      
      if (UAS_data.BasicIDValid[0]) {
        strncpy(UAV.uav_id, (char *)UAS_data.BasicID[0].UASID, ODID_ID_SIZE);
      }
      if (UAS_data.LocationValid) {
        UAV.lat_d = UAS_data.Location.Latitude;
        UAV.long_d = UAS_data.Location.Longitude;
        UAV.altitude_msl = (int)UAS_data.Location.AltitudeGeo;
        UAV.height_agl = (int)UAS_data.Location.Height;
        UAV.speed = (int)UAS_data.Location.SpeedHorizontal;
        UAV.heading = (int)UAS_data.Location.Direction;
      }
      if (UAS_data.SystemValid) {
        UAV.base_lat_d = UAS_data.System.OperatorLatitude;
        UAV.base_long_d = UAS_data.System.OperatorLongitude;
      }
      if (UAS_data.OperatorIDValid) {
        strncpy(UAV.op_id, (char *)UAS_data.OperatorID.OperatorId, ODID_ID_SIZE);
      }
      
      id_data* storedUAV = next_uav(UAV.mac);
      *storedUAV = UAV;
      storedUAV->flag = 1;
      
      // Trigger buzzer alert (thread-safe, non-blocking)
      portENTER_CRITICAL_ISR(&buzzerMux);
      if (!device_in_range) {
        trigger_detection_beep = true;
        device_in_range = true;
        last_heartbeat = millis();
      }
      portEXIT_CRITICAL_ISR(&buzzerMux);
      
      {
        id_data tmp = *storedUAV;
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        xQueueSendFromISR(printQueue, &tmp, &xHigherPriorityTaskWoken);
        if (xHigherPriorityTaskWoken) portYIELD_FROM_ISR();
      }
    }
  }
  else if (payload[0] == 0x80) {
    int offset = 36;
    while (offset < length) {
      int typ = payload[offset];
      int len = payload[offset + 1];
      if ((typ == 0xdd) &&
          (((payload[offset + 2] == 0x90 && payload[offset + 3] == 0x3a && payload[offset + 4] == 0xe6)) ||
           ((payload[offset + 2] == 0xfa && payload[offset + 3] == 0x0b && payload[offset + 4] == 0xbc)))) {
        int j = offset + 7;
        if (j < length) {
          memset(&UAS_data, 0, sizeof(UAS_data));
          odid_message_process_pack(&UAS_data, &payload[j], length - j);
          
          id_data UAV;
          memset(&UAV, 0, sizeof(UAV));
          memcpy(UAV.mac, &payload[10], 6);
          UAV.rssi = packet->rx_ctrl.rssi;
          UAV.last_seen = millis();
          
          if (UAS_data.BasicIDValid[0]) {
            strncpy(UAV.uav_id, (char *)UAS_data.BasicID[0].UASID, ODID_ID_SIZE);
          }
          if (UAS_data.LocationValid) {
            UAV.lat_d = UAS_data.Location.Latitude;
            UAV.long_d = UAS_data.Location.Longitude;
            UAV.altitude_msl = (int)UAS_data.Location.AltitudeGeo;
            UAV.height_agl = (int)UAS_data.Location.Height;
            UAV.speed = (int)UAS_data.Location.SpeedHorizontal;
            UAV.heading = (int)UAS_data.Location.Direction;
          }
          if (UAS_data.SystemValid) {
            UAV.base_lat_d = UAS_data.System.OperatorLatitude;
            UAV.base_long_d = UAS_data.System.OperatorLongitude;
          }
          if (UAS_data.OperatorIDValid) {
            strncpy(UAV.op_id, (char *)UAS_data.OperatorID.OperatorId, ODID_ID_SIZE);
          }
          
          id_data* storedUAV = next_uav(UAV.mac);
          *storedUAV = UAV;
          storedUAV->flag = 1;
          
          // Trigger buzzer alert (thread-safe, non-blocking)
          portENTER_CRITICAL_ISR(&buzzerMux);
          if (!device_in_range) {
            trigger_detection_beep = true;
            device_in_range = true;
            last_heartbeat = millis();
          }
          portEXIT_CRITICAL_ISR(&buzzerMux);
          
          {
            id_data tmp = *storedUAV;
            BaseType_t xHigherPriorityTaskWoken = pdFALSE;
            xQueueSendFromISR(printQueue, &tmp, &xHigherPriorityTaskWoken);
            if (xHigherPriorityTaskWoken) portYIELD_FROM_ISR();
          }
        }
      }
      offset += len + 2;
    }
  }
}

void printerTask(void *param) {
  id_data UAV;
  for (;;) {
    if (xQueueReceive(printQueue, &UAV, portMAX_DELAY)) {
      send_json_fast(&UAV);
      // Mesh functionality removed - only JSON output over USB serial
    }
  }
}

void initializeSerial() {
  Serial.begin(115200);
  // Serial1 removed - no mesh functionality
}

void initializeBuzzer() {
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  // Read buzzer toggle from shared NVS
  Preferences bzP;
  bzP.begin("ouispy-bz", true);
  ssBuzzerOn = bzP.getBool("on", true);
  bzP.end();

  Serial.printf("Buzzer initialized on GPIO3 (%s)\n", ssBuzzerOn ? "ON" : "OFF");
}

// Close Encounters of the Third Kind - iconic 5-note motif
// D5, E5, C5, C4, G4 — played fast and punchy
void playCloseEncounters() {
  if (!ssBuzzerOn) return;

  // The five notes with duration in ms
  struct { int freq; int dur; int gap; } notes[] = {
    { 587, 120,  30 },  // D5
    { 659, 120,  30 },  // E5
    { 523, 120,  30 },  // C5
    { 262, 120,  30 },  // C4 (octave down)
    { 392, 200,   0 },  // G4 (held slightly longer)
  };

  for (int i = 0; i < 5; i++) {
    tone(BUZZER_PIN, notes[i].freq, notes[i].dur);
    boardLedOn();
    delay(notes[i].dur);
    boardLedOff();
    noTone(BUZZER_PIN);
    if (notes[i].gap > 0) delay(notes[i].gap);
  }

  Serial.println("[SKY-SPY] *close encounters theme*");
}

void initializeLED() {
  boardLedInit();
  Serial.println("Status LED initialized");
}

void setup() {
  setCpuFrequencyMhz(160);
  initializeSerial();
  initializeBuzzer();
  initializeLED();
  
  // Close Encounters boot melody
  playCloseEncounters();
  
  nvs_flash_init();
  
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_promiscuous_rx_cb(&callback);
  esp_wifi_set_channel(6, WIFI_SECOND_CHAN_NONE);
  
  // Mute benign parse-advertisement noise from the underlying BLE lib when
  // peripherals send zero-length ADV packets. Our callback already bails on
  // those; this just keeps Serial clean for JSON consumers.
  esp_log_level_set("BLEAdvertisedDevice", ESP_LOG_NONE);
  esp_log_level_set("NimBLEAdvertisedDevice", ESP_LOG_NONE);

  NimBLEDevice::init("DroneID");
  pBLEScan = NimBLEDevice::getScan();
  nimbleSetAdvertisedCallbacks(pBLEScan, new MyAdvertisedDeviceCallbacks());
  pBLEScan->setActiveScan(true);

  printQueue = xQueueCreate(MAX_UAVS, sizeof(id_data));
  
  TASK_CREATE_PINNED(bleScanTask, "BLEScanTask", 8192, NULL, 1, NULL, 1);
  TASK_CREATE_PINNED(wifiProcessTask, "WiFiProcessTask", 8192, NULL, 1, NULL, 0);
  TASK_CREATE_PINNED(printerTask, "PrinterTask", 8192, NULL, 1, NULL, 1);
  TASK_CREATE_PINNED(buzzerTask, "BuzzerTask", 4096, NULL, 1, NULL, 1);
  
  memset(uavs, 0, sizeof(uavs));
}

void loop() {
  unsigned long current_millis = millis();
  
  // Status message every 60 seconds
  if ((current_millis - last_status) > 60000UL) {
    Serial.println("{\"status\":\"scanning\"}");
    last_status = current_millis;
  }
  
  // Handle heartbeat pulse if drone is in range (thread-safe)
  portENTER_CRITICAL(&buzzerMux);
  bool in_range = device_in_range;
  portEXIT_CRITICAL(&buzzerMux);
  
  if (in_range) {
    // Check if 5 seconds have passed since last heartbeat
    if (current_millis - last_heartbeat >= 5000) {
      portENTER_CRITICAL(&buzzerMux);
      trigger_heartbeat_beep = true;
      portEXIT_CRITICAL(&buzzerMux);
      last_heartbeat = current_millis;
    }
    
    // Check if drone has gone out of range (no detection for 7 seconds)
    bool drone_still_detected = false;
    for (int i = 0; i < MAX_UAVS; i++) {
      if (uavs[i].mac[0] != 0 && (current_millis - uavs[i].last_seen) < 7000) {
        drone_still_detected = true;
        break;
      }
    }
    
    if (!drone_still_detected) {
      Serial.println("Drone out of range - stopping heartbeat");
      portENTER_CRITICAL(&buzzerMux);
      device_in_range = false;
      portEXIT_CRITICAL(&buzzerMux);
    }
  }
}
