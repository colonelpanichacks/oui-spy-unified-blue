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
#define BUZZER_PIN 3  // GPIO3 (D2) - PWM capable pin on Xiao ESP32 S3

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

void callback(void *, wifi_promiscuous_pkt_type_t);
void send_json_fast(const id_data *UAV);
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

// Mutex guarding the uavs[] array. BLE and WiFi promiscuous callbacks both
// write here from different tasks; without this the large struct copies and
// strncpy calls can produce torn reads or stale trailing bytes.
static SemaphoreHandle_t uavsMutex = NULL;

static QueueHandle_t printQueue;

// Find an existing UAV slot by MAC, or allocate a free one.
// If the array is full, evicts the slot with the oldest last_seen (LRU) and
// zeroes it so strncpy writes don't leave stale trailing bytes.
// Caller must hold uavsMutex.
id_data* next_uav(uint8_t* mac) {
  static const uint8_t EMPTY_MAC[6] = {0, 0, 0, 0, 0, 0};
  // 1. Match existing slot by MAC
  for (int i = 0; i < MAX_UAVS; i++) {
    if (memcmp(uavs[i].mac, mac, 6) == 0 &&
        memcmp(uavs[i].mac, EMPTY_MAC, 6) != 0) {
      return &uavs[i];
    }
  }
  // 2. Take a free slot (all-zero MAC means empty)
  for (int i = 0; i < MAX_UAVS; i++) {
    if (memcmp(uavs[i].mac, EMPTY_MAC, 6) == 0) {
      return &uavs[i];
    }
  }
  // 3. Full -- evict the oldest (LRU) and zero it for clean reuse
  uint32_t oldest_seen = UINT32_MAX;
  int oldest_idx = 0;
  for (int i = 0; i < MAX_UAVS; i++) {
    if (uavs[i].last_seen < oldest_seen) {
      oldest_seen = uavs[i].last_seen;
      oldest_idx = i;
    }
  }
  memset(&uavs[oldest_idx], 0, sizeof(uavs[oldest_idx]));
  return &uavs[oldest_idx];
}

class MyAdvertisedDeviceCallbacks : public NimBLEAdvertisedDeviceCallbacks {
public:
  // Helper: decode and store a single ODID message into UAV. Returns true if
  // anything useful was extracted. Called from both the single-message path
  // and the Message Pack (0xF0) container path.
  static bool decodeOneODIDMessage(id_data* UAV, uint8_t* msg, size_t remaining) {
    if (remaining < ODID_MESSAGE_SIZE) return false;
    switch (msg[0] & 0xF0) {
      case 0x00: {
        ODID_BasicID_data basic;
        decodeBasicIDMessage(&basic, (ODID_BasicID_encoded*) msg);
        strncpy(UAV->uav_id, (char*) basic.UASID, ODID_ID_SIZE);
        UAV->uav_id[ODID_ID_SIZE] = '\0';
        return true;
      }
      case 0x10: {
        ODID_Location_data loc;
        decodeLocationMessage(&loc, (ODID_Location_encoded*) msg);
        UAV->lat_d = loc.Latitude;
        UAV->long_d = loc.Longitude;
        UAV->altitude_msl = (int) loc.AltitudeGeo;
        UAV->height_agl = (int) loc.Height;
        UAV->speed = (int) loc.SpeedHorizontal;
        UAV->heading = (int) loc.Direction;
        return true;
      }
      case 0x40: {
        ODID_System_data sys;
        decodeSystemMessage(&sys, (ODID_System_encoded*) msg);
        UAV->base_lat_d = sys.OperatorLatitude;
        UAV->base_long_d = sys.OperatorLongitude;
        return true;
      }
      case 0x50: {
        ODID_OperatorID_data op;
        decodeOperatorIDMessage(&op, (ODID_OperatorID_encoded*) msg);
        strncpy(UAV->op_id, (char*) op.OperatorId, ODID_ID_SIZE);
        UAV->op_id[ODID_ID_SIZE] = '\0';
        return true;
      }
      default:
        // 0x20 Auth, 0x30 Self-ID: not currently stored but not errors.
        return false;
    }
  }

  void onResult(NimBLEAdvertisedDevice* device) override {
    int len = device->getPayloadLength();
    if (len <= 0) return;

    uint8_t* payload = device->getPayload();
    // Need at least: length(1) + AD-type(1) + UUID(2) + AD-type(1) + first
    // ODID byte(1) + header byte of first message = 6 bytes minimum before
    // we dereference payload[6].
    if (len < 7) return;
    // Match ODID service-data advertisement: AD-type 0x16, UUID 0xFFFA,
    // ODID app code 0x0D.
    if (!(payload[1] == 0x16 && payload[2] == 0xFA &&
          payload[3] == 0xFF && payload[4] == 0x0D)) return;

    uint8_t* mac = (uint8_t*) device->getAddress().getNative();
    uint8_t* odid = &payload[6];
    size_t odid_len = (size_t)(len - 6);

    // Dispatch under the uavs mutex -- both the slot lookup and the write
    // need to be atomic relative to the WiFi promiscuous callback.
    if (!uavsMutex || xSemaphoreTake(uavsMutex, pdMS_TO_TICKS(20)) != pdTRUE) return;

    id_data* UAV = next_uav(mac);
    UAV->last_seen = millis();
    UAV->rssi = device->getRSSI();
    memcpy(UAV->mac, mac, 6);

    bool got_data = false;
    if ((odid[0] & 0xF0) == 0xF0) {
      // Message Pack container -- walks the pack and dispatches each message.
      // Uses the library's pack-processor which populates the global UAS_data,
      // then we snapshot the interesting fields.
      ODID_UAS_Data local;
      memset(&local, 0, sizeof(local));
      if (odid_message_process_pack(&local, odid, odid_len) == 0) {
        if (local.BasicIDValid[0]) {
          strncpy(UAV->uav_id, (char*)local.BasicID[0].UASID, ODID_ID_SIZE);
          UAV->uav_id[ODID_ID_SIZE] = '\0';
          got_data = true;
        }
        if (local.LocationValid) {
          UAV->lat_d = local.Location.Latitude;
          UAV->long_d = local.Location.Longitude;
          UAV->altitude_msl = (int) local.Location.AltitudeGeo;
          UAV->height_agl = (int) local.Location.Height;
          UAV->speed = (int) local.Location.SpeedHorizontal;
          UAV->heading = (int) local.Location.Direction;
          got_data = true;
        }
        if (local.SystemValid) {
          UAV->base_lat_d = local.System.OperatorLatitude;
          UAV->base_long_d = local.System.OperatorLongitude;
          got_data = true;
        }
        if (local.OperatorIDValid) {
          strncpy(UAV->op_id, (char*)local.OperatorID.OperatorId, ODID_ID_SIZE);
          UAV->op_id[ODID_ID_SIZE] = '\0';
          got_data = true;
        }
      }
    } else {
      got_data = decodeOneODIDMessage(UAV, odid, odid_len);
    }

    if (got_data) UAV->flag = 1;

    id_data tmp = *UAV;  // snapshot for the print queue
    xSemaphoreGive(uavsMutex);

    if (!got_data) return;  // no useful payload, don't beep or enqueue

    // Trigger buzzer alert (thread-safe, non-blocking)
    portENTER_CRITICAL(&buzzerMux);
    if (!device_in_range) {
      trigger_detection_beep = true;
      device_in_range = true;
      last_heartbeat = millis();
    }
    portEXIT_CRITICAL(&buzzerMux);

    // BLE callback runs on the NimBLE host task (not a hardware ISR) so the
    // non-ISR variant of xQueueSend is correct here.
    xQueueSend(printQueue, &tmp, 0);
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

// Mesh functionality removed - this is now a pure USB serial drone scanner

void bleScanTask(void *parameter) {
  for (;;) {
    NimBLEScanResults foundDevices = pBLEScan->start(1, false);
    pBLEScan->clearResults();
    // No flag checking needed - BLE callback handles buzzer triggering
    delay(100);
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
  // Minimum 802.11 mgmt frame header we care about reads payload[0..9] (frame
  // control + duration + DA). Anything shorter is malformed -- drop it before
  // touching memory.
  if (length < 10 || !payload) return;

  static const uint8_t nan_dest[6] = {0x51, 0x6f, 0x9a, 0x01, 0x00, 0x00};
  if (memcmp(nan_dest, &payload[4], 6) == 0) {
    // NAN action frame -- need at least up to the source MAC (bytes 10..15).
    if (length < 16) return;
    char nan_mac[6] = {0};  // receive buffer for source MAC (library writes to this)
    if (odid_wifi_receive_message_pack_nan_action_frame(&UAS_data, nan_mac, payload, length) == 0) {
      id_data UAV;
      memset(&UAV, 0, sizeof(UAV));
      memcpy(UAV.mac, &payload[10], 6);
      UAV.rssi = packet->rx_ctrl.rssi;
      UAV.last_seen = millis();

      if (UAS_data.BasicIDValid[0]) {
        strncpy(UAV.uav_id, (char *)UAS_data.BasicID[0].UASID, ODID_ID_SIZE);
        UAV.uav_id[ODID_ID_SIZE] = '\0';
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
        UAV.op_id[ODID_ID_SIZE] = '\0';
      }

      if (!uavsMutex || xSemaphoreTake(uavsMutex, pdMS_TO_TICKS(20)) != pdTRUE) return;
      id_data* storedUAV = next_uav(UAV.mac);
      *storedUAV = UAV;
      storedUAV->flag = 1;
      id_data tmp = *storedUAV;
      xSemaphoreGive(uavsMutex);

      portENTER_CRITICAL(&buzzerMux);
      if (!device_in_range) {
        trigger_detection_beep = true;
        device_in_range = true;
        last_heartbeat = millis();
      }
      portEXIT_CRITICAL(&buzzerMux);

      xQueueSend(printQueue, &tmp, 0);
    }
  }
  else if (payload[0] == 0x80) {
    // Beacon frame. Tagged parameters start after the fixed 36-byte header
    // (MAC hdr 24 + timestamp 8 + beacon interval 2 + capability 2).
    // Need at least the first 5 bytes of the first vendor tag we test.
    if (length < 41) return;
    int offset = 36;
    while (offset + 2 <= length) {            // need at least typ+len bytes
      int typ = payload[offset];
      int len = payload[offset + 1];
      if (offset + 2 + len > length) break;   // tag claims bytes past frame end
      if (typ == 0xdd && len >= 3 && (offset + 7) <= length &&
          ((payload[offset + 2] == 0x90 && payload[offset + 3] == 0x3a && payload[offset + 4] == 0xe6) ||
           (payload[offset + 2] == 0xfa && payload[offset + 3] == 0x0b && payload[offset + 4] == 0xbc))) {
        int j = offset + 7;
        int available = length - j;
        if (available > 0) {
          memset(&UAS_data, 0, sizeof(UAS_data));
          odid_message_process_pack(&UAS_data, &payload[j], (size_t)available);

          id_data UAV;
          memset(&UAV, 0, sizeof(UAV));
          memcpy(UAV.mac, &payload[10], 6);
          UAV.rssi = packet->rx_ctrl.rssi;
          UAV.last_seen = millis();

          if (UAS_data.BasicIDValid[0]) {
            strncpy(UAV.uav_id, (char *)UAS_data.BasicID[0].UASID, ODID_ID_SIZE);
            UAV.uav_id[ODID_ID_SIZE] = '\0';
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
            UAV.op_id[ODID_ID_SIZE] = '\0';
          }

          if (!uavsMutex || xSemaphoreTake(uavsMutex, pdMS_TO_TICKS(20)) != pdTRUE) return;
          id_data* storedUAV = next_uav(UAV.mac);
          *storedUAV = UAV;
          storedUAV->flag = 1;
          id_data tmp = *storedUAV;
          xSemaphoreGive(uavsMutex);

          portENTER_CRITICAL(&buzzerMux);
          if (!device_in_range) {
            trigger_detection_beep = true;
            device_in_range = true;
            last_heartbeat = millis();
          }
          portEXIT_CRITICAL(&buzzerMux);

          xQueueSend(printQueue, &tmp, 0);
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

void setup() {
  setCpuFrequencyMhz(160);
  initializeSerial();
  initializeBuzzer();
  initializeLED();
  
  // Close Encounters boot melody
  playCloseEncounters();
  
  nvs_flash_init();

  // Create the uavs[] array mutex BEFORE any callbacks get installed.
  uavsMutex = xSemaphoreCreateMutex();

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_promiscuous_rx_cb(&callback);
  esp_wifi_set_channel(6, WIFI_SECOND_CHAN_NONE);
  
  NimBLEDevice::init("DroneID");
  pBLEScan = NimBLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setActiveScan(true);

  printQueue = xQueueCreate(MAX_UAVS, sizeof(id_data));
  
  xTaskCreatePinnedToCore(bleScanTask, "BLEScanTask", 10000, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(wifiProcessTask, "WiFiProcessTask", 10000, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(printerTask, "PrinterTask", 10000, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(buzzerTask, "BuzzerTask", 4096, NULL, 1, NULL, 1);
  
  memset(uavs, 0, sizeof(uavs));
}

void loop() {
  unsigned long current_millis = millis();
  
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
