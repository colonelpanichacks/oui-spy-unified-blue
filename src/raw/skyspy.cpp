#if !defined(ARDUINO_ARCH_ESP32)
  #error "This program requires an ESP32S3"
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

// Buzzer configuration
// Buzzer pin routing: GPIO3 (D2) for the external oui-spy piezo, or GPIO4
// (D3/A3) for the expansion board's passive buzzer when the chassis is present.
// Set in initializeBuzzer() after initializeSerial() has probed for the display.
static int buzzerPin = BUZZER_PIN_EXTERNAL;
#define BUZZER_PIN buzzerPin

// LED configuration
#define LED_PIN 21    // GPIO21 - Built-in orange LED on Xiao ESP32 S3 (inverted logic)

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

// Mesh UART on pins D4 (TX) and D5 (RX) for Heltec LoRa gateway
const int SERIAL1_RX_PIN = 6;
const int SERIAL1_TX_PIN = 5;

void callback(void *, wifi_promiscuous_pkt_type_t);
void send_json_fast(const id_data *UAV);
void send_mesh_message(const id_data *UAV);
void buzzerTask(void *parameter);

#define MAX_UAVS 8
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
  void onResult(NimBLEAdvertisedDevice* device) override {
    int len = device->getPayloadLength();
    if (len <= 0) return;
      
    uint8_t* payload = device->getPayload();
    if (len > 5 && payload[1] == 0x16 && payload[2] == 0xFA && 
        payload[3] == 0xFF && payload[4] == 0x0D) {
      uint8_t* mac = (uint8_t*) device->getAddress().getNative();
      id_data* UAV = next_uav(mac);
      UAV->last_seen = millis();
      UAV->rssi = device->getRSSI();
      memcpy(UAV->mac, mac, 6);
      
      uint8_t* odid = &payload[6];
      switch (odid[0] & 0xF0) {
        case 0x00: {
          ODID_BasicID_data basic;
          decodeBasicIDMessage(&basic, (ODID_BasicID_encoded*) odid);
          strncpy(UAV->uav_id, (char*) basic.UASID, ODID_ID_SIZE);
          break;
        }
        case 0x10: {
          ODID_Location_data loc;
          decodeLocationMessage(&loc, (ODID_Location_encoded*) odid);
          UAV->lat_d = loc.Latitude;
          UAV->long_d = loc.Longitude;
          UAV->altitude_msl = (int) loc.AltitudeGeo;
          UAV->height_agl = (int) loc.Height;
          UAV->speed = (int) loc.SpeedHorizontal;
          UAV->heading = (int) loc.Direction;
          break;
        }
        case 0x40: {
          ODID_System_data sys;
          decodeSystemMessage(&sys, (ODID_System_encoded*) odid);
          UAV->base_lat_d = sys.OperatorLatitude;
          UAV->base_long_d = sys.OperatorLongitude;
          break;
        }
        case 0x50: {
          ODID_OperatorID_data op;
          decodeOperatorIDMessage(&op, (ODID_OperatorID_encoded*) odid);
          strncpy(UAV->op_id, (char*) op.OperatorId, ODID_ID_SIZE);
          break;
        }
      }
      UAV->flag = 1;
      
      // Trigger buzzer alert (thread-safe, non-blocking)
      portENTER_CRITICAL_ISR(&buzzerMux);
      if (!device_in_range) {
        trigger_detection_beep = true;
        device_in_range = true;
        last_heartbeat = millis();
      }
      portEXIT_CRITICAL_ISR(&buzzerMux);
      
      {
        id_data tmp = *UAV;
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        xQueueSendFromISR(printQueue, &tmp, &xHigherPriorityTaskWoken);
        if (xHigherPriorityTaskWoken) portYIELD_FROM_ISR();
      }
    }
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
        digitalWrite(LED_PIN, LOW);  // Turn on LED (inverted logic)
        vTaskDelay(pdMS_TO_TICKS(150)); // LED on during beep
        digitalWrite(LED_PIN, HIGH); // Turn off LED (inverted logic)
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
      digitalWrite(LED_PIN, LOW);  // Turn on LED (inverted logic)
      vTaskDelay(pdMS_TO_TICKS(100));
      digitalWrite(LED_PIN, HIGH); // Turn off LED (inverted logic)
      vTaskDelay(pdMS_TO_TICKS(50));
      if (ssBuzzerOn) tone(BUZZER_PIN, HEARTBEAT_FREQ, HEARTBEAT_DURATION);
      digitalWrite(LED_PIN, LOW);  // Turn on LED (inverted logic)
      vTaskDelay(pdMS_TO_TICKS(100));
      digitalWrite(LED_PIN, HIGH); // Turn off LED (inverted logic)
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
}

void send_mesh_message(const id_data *UAV) {
  // Mesh UART is disabled while the expansion board OLED uses pins 5/6.
  if (dashboard_present()) return;

  static unsigned long lastSendTime = 0;
  const unsigned long sendInterval = 5000;
  const int MAX_MESH_SIZE = 230;

  if (millis() - lastSendTime < sendInterval) return;
  lastSendTime = millis();

  char mac_str[18];
  snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
           UAV->mac[0], UAV->mac[1], UAV->mac[2],
           UAV->mac[3], UAV->mac[4], UAV->mac[5]);

  char mesh_msg[MAX_MESH_SIZE];
  int msg_len = 0;
  msg_len += snprintf(mesh_msg + msg_len, sizeof(mesh_msg) - msg_len,
                      "Drone: %s RSSI:%d", mac_str, UAV->rssi);
  if (msg_len < MAX_MESH_SIZE && UAV->lat_d != 0.0 && UAV->long_d != 0.0) {
    msg_len += snprintf(mesh_msg + msg_len, sizeof(mesh_msg) - msg_len,
                        " https://maps.google.com/?q=%.6f,%.6f",
                        UAV->lat_d, UAV->long_d);
  }
  if (Serial1.availableForWrite() >= msg_len) {
    Serial1.println(mesh_msg);
  }

  vTaskDelay(pdMS_TO_TICKS(1000));
  if (UAV->base_lat_d != 0.0 && UAV->base_long_d != 0.0) {
    char pilot_msg[MAX_MESH_SIZE];
    int pilot_len = snprintf(pilot_msg, sizeof(pilot_msg),
                             "Pilot: https://maps.google.com/?q=%.6f,%.6f",
                             UAV->base_lat_d, UAV->base_long_d);
    if (Serial1.availableForWrite() >= pilot_len) {
      Serial1.println(pilot_msg);
    }
  }
}

void bleScanTask(void *parameter) {
  for (;;) {
    // Short BLE scan with long gap to minimize WiFi promiscuous interference.
    // ESP32 shares one 2.4GHz radio for BLE and WiFi; aggressive BLE scanning
    // starves WiFi promiscuous mode, causing missed drone detections.
    NimBLEScanResults foundDevices = pBLEScan->start(1, false);
    pBLEScan->clearResults();
    delay(10000);  // 10s gap between scans — gives WiFi promiscuous priority
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
      send_mesh_message(&UAV);
    }
  }
}

void initializeSerial() {
  Serial.begin(115200);

  // Probe for the expansion board OLED before touching Serial1. The display
  // lives on the same two pins (GPIO5/6) as the mesh UART, so when it is
  // present those pins belong to the I2C bus and mesh forwarding is disabled.
  dashboard_init();

  if (dashboard_present()) {
    Serial.println("[SKY-SPY] OLED detected - Serial1 mesh UART disabled (pins 5/6 shared with display)");
    return;
  }
  Serial1.begin(115200, SERIAL_8N1, SERIAL1_RX_PIN, SERIAL1_TX_PIN);
}

void initializeBuzzer() {
  // Route to the chassis buzzer (GPIO4) when the expansion board is present;
  // initializeSerial() already probed for the display before this runs.
  buzzerPin = dashboard_buzzer_pin();

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  // Read buzzer toggle from shared NVS
  Preferences bzP;
  bzP.begin("ouispy-bz", true);
  ssBuzzerOn = bzP.getBool("on", true);
  bzP.end();

  Serial.printf("Buzzer initialized on GPIO%d (%s)\n", buzzerPin, ssBuzzerOn ? "ON" : "OFF");
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
    digitalWrite(LED_PIN, LOW);   // LED flash with each note
    delay(notes[i].dur);
    digitalWrite(LED_PIN, HIGH);
    noTone(BUZZER_PIN);
    if (notes[i].gap > 0) delay(notes[i].gap);
  }

  Serial.println("[SKY-SPY] *close encounters theme*");
}

void initializeLED() {
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH); // Turn off LED initially (inverted logic)
  Serial.println("Orange LED initialized on GPIO21 (inverted logic)");
}

// ============================================================================
// Expansion board OLED dashboard (SSD1306/SSD1315 128x64)
// The 5x7 font uses y as the text baseline, so row i sits at 6 + i*8 pixels.
// ============================================================================
#define DASH_REFRESH_MS 1000
#define DASH_BASELINE 6
#define DASH_ROW_STEP 8

static unsigned long lastDashboardRefresh = 0;

enum DashPage {
  DASH_PAGE_SUMMARY = 0,
  DASH_PAGE_LATEST,
  DASH_PAGE_POSITION,
  DASH_PAGE_FLEET,
  DASH_PAGE_COUNT
};
static uint8_t dashPage = DASH_PAGE_SUMMARY;

static void dashRow(uint8_t row) {
  dashboard_set_cursor(0, DASH_BASELINE + row * DASH_ROW_STEP);
}

static void dashFormatMac(const uint8_t *mac, char *out, size_t len) {
  snprintf(out, len, "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static const char *dashDroneId(const id_data *uav) {
  if (uav->op_id[0] != '\0') return uav->op_id;
  if (uav->uav_id[0] != '\0') return uav->uav_id;
  return "-";
}

static void dashFooter(unsigned long now) {
  dashRow(7);
  dashboard_printf("P%u/%u UP %02lu:%02lu:%02lu",
                   (unsigned)(dashPage + 1), (unsigned)DASH_PAGE_COUNT,
                   (unsigned long)((now / 3600000UL) % 100UL),
                   (unsigned long)((now / 60000UL) % 60UL),
                   (unsigned long)((now / 1000UL) % 60UL));
}

static void dashPageSummary(const id_data *best, int activeCount, int totalCount, unsigned long now) {
  char mac[18];
  dashRow(0);
  dashboard_printf("SKYSPY ACT:%d TOT:%d", activeCount, totalCount);
  dashboard_draw_hline(0, 7, 128);

  if (activeCount > 0) {
    dashRow(2);
    dashboard_printf("DRONE IN RANGE");
    if (best != NULL) {
      dashRow(3);
      dashboard_printf("RSSI %d dBm", best->rssi);
      dashFormatMac(best->mac, mac, sizeof(mac));
      dashRow(4);
      dashboard_printf("%s", mac);
    }
  } else {
    dashRow(2);
    dashboard_printf("SCANNING FOR DRONES");
    dashRow(3);
    dashboard_printf("NO REMOTE ID SIGNAL");
  }
  dashRow(5);
  dashboard_printf("WIFI+BLE PASSIVE");
  dashFooter(now);
}

static void dashPageLatest(const id_data *best, unsigned long now) {
  char mac[18];
  dashRow(0);
  dashboard_printf("LATEST DRONE");
  dashboard_draw_hline(0, 7, 128);

  if (best == NULL) {
    dashRow(3);
    dashboard_printf("NO DRONE DETECTED");
    dashFooter(now);
    return;
  }

  dashFormatMac(best->mac, mac, sizeof(mac));
  dashRow(2);
  dashboard_printf("MAC %s", mac);
  dashRow(3);
  dashboard_printf("RSSI %d dBm", best->rssi);
  dashRow(4);
  dashboard_printf("ALT %dm SPD %dm/s", best->altitude_msl, best->speed);
  dashRow(5);
  dashboard_printf("HDG %d AGL %dm", best->heading, best->height_agl);
  dashRow(6);
  dashboard_printf("ID %s", dashDroneId(best));
  dashFooter(now);
}

static void dashPagePosition(const id_data *best, unsigned long now) {
  dashRow(0);
  dashboard_printf("DRONE POSITION");
  dashboard_draw_hline(0, 7, 128);

  if (best == NULL) {
    dashRow(3);
    dashboard_printf("NO DRONE DETECTED");
    dashFooter(now);
    return;
  }

  if (best->lat_d != 0.0 && best->long_d != 0.0) {
    dashRow(2);
    dashboard_printf("LAT %.5f", best->lat_d);
    dashRow(3);
    dashboard_printf("LON %.5f", best->long_d);
  } else {
    dashRow(2);
    dashboard_printf("POSITION NOT RX'D");
    dashRow(3);
    dashboard_printf("LAT/LON UNAVAILABLE");
  }
  dashRow(4);
  dashboard_printf("ALT %dm AGL %dm", best->altitude_msl, best->height_agl);

  if (best->base_lat_d != 0.0 && best->base_long_d != 0.0) {
    dashRow(5);
    dashboard_printf("PILOT LAT %.5f", best->base_lat_d);
    dashRow(6);
    dashboard_printf("PILOT LON %.5f", best->base_long_d);
  } else {
    dashRow(5);
    dashboard_printf("PILOT POS N/A");
  }
  dashFooter(now);
}

static void dashPageFleet(int totalCount, unsigned long now) {
  id_data *list[MAX_UAVS];
  int n = 0;
  for (int i = 0; i < MAX_UAVS; i++) {
    if (uavs[i].mac[0] != 0) list[n++] = &uavs[i];
  }
  // Insertion sort, strongest signal first
  for (int i = 1; i < n; i++) {
    id_data *key = list[i];
    int j = i - 1;
    while (j >= 0 && list[j]->rssi < key->rssi) {
      list[j + 1] = list[j];
      j--;
    }
    list[j + 1] = key;
  }

  dashRow(0);
  dashboard_printf("FLEET TOT:%d", totalCount);
  dashboard_draw_hline(0, 7, 128);

  int shown = 0;
  for (int i = 0; i < n && shown < 5; i++) {
    char mac[18];
    dashFormatMac(list[i]->mac, mac, sizeof(mac));
    dashRow(2 + shown);
    dashboard_printf("%d %s %d", i + 1, mac, list[i]->rssi);
    shown++;
  }
  if (n > 5) {
    dashRow(6);
    dashboard_printf("...and %d more", n - 5);
  }
  dashFooter(now);
}

void renderDashboard() {
  if (!dashboard_present()) return;

  unsigned long now = millis();
  if (now - lastDashboardRefresh < DASH_REFRESH_MS) return;
  lastDashboardRefresh = now;

  int activeCount = 0, totalCount = 0;
  id_data *best = NULL;
  uint32_t bestLast = 0;
  for (int i = 0; i < MAX_UAVS; i++) {
    if (uavs[i].mac[0] == 0) continue;
    totalCount++;
    if (now - uavs[i].last_seen < 10000) activeCount++;
    if (uavs[i].last_seen >= bestLast) {
      bestLast = uavs[i].last_seen;
      best = &uavs[i];
    }
  }

  dashboard_clear();
  switch (dashPage) {
    case DASH_PAGE_SUMMARY:  dashPageSummary(best, activeCount, totalCount, now); break;
    case DASH_PAGE_LATEST:   dashPageLatest(best, now); break;
    case DASH_PAGE_POSITION: dashPagePosition(best, now); break;
    case DASH_PAGE_FLEET:    dashPageFleet(totalCount, now); break;
    default:                 dashPage = DASH_PAGE_SUMMARY; break;
  }
  dashboard_flush();
}

void setup() {
  setCpuFrequencyMhz(160);
  initializeSerial();
  initializeBuzzer();
  initializeLED();
  
  // Close Encounters boot melody
  playCloseEncounters();

  // Brief boot splash on the expansion board OLED
  if (dashboard_present()) {
    dashboard_clear();
    dashboard_set_cursor(0, DASH_BASELINE + 0 * DASH_ROW_STEP);
    dashboard_printf("SKY SPY");
    dashboard_set_cursor(0, DASH_BASELINE + 1 * DASH_ROW_STEP);
    dashboard_printf("DRONE REMOTE ID MONITOR");
    dashboard_set_cursor(0, DASH_BASELINE + 2 * DASH_ROW_STEP);
    dashboard_printf("MODE 5 WIFI+BLE");
    dashboard_set_cursor(0, DASH_BASELINE + 5 * DASH_ROW_STEP);
    dashboard_printf("BOOTING...");
    dashboard_flush();
    delay(1500);
  }

  nvs_flash_init();
  
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_promiscuous_rx_cb(&callback);
  esp_wifi_set_channel(6, WIFI_SECOND_CHAN_NONE);
  
  NimBLEDevice::init("DroneID");
  pBLEScan = NimBLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setActiveScan(false);  // Passive scan — less radio contention with WiFi promisc

  printQueue = xQueueCreate(MAX_UAVS, sizeof(id_data));
  
  xTaskCreatePinnedToCore(bleScanTask, "BLEScanTask", 10000, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(wifiProcessTask, "WiFiProcessTask", 10000, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(printerTask, "PrinterTask", 10000, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(buzzerTask, "BuzzerTask", 4096, NULL, 1, NULL, 1);
  
  memset(uavs, 0, sizeof(uavs));
}

void loop() {
  unsigned long current_millis = millis();

  // Expansion board button advances the dashboard page (display only)
  if (dashboard_present() && dashboard_button_pressed()) {
    dashPage++;
    if (dashPage >= DASH_PAGE_COUNT) dashPage = 0;
    lastDashboardRefresh = 0;  // redraw immediately on page change
  }
  renderDashboard();

  // Status message every 60 seconds
  if ((current_millis - last_status) > 60000UL) {
    Serial.println("{\"   [+] Device is active and scanning...\"}");
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
