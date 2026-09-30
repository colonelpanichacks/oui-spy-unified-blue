#include "board_gps.h"

#include <TinyGPSPlus.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#ifdef NESSO_N1
#include <Arduino_Nesso_N1.h>
#include <Wire.h>
#endif

static SemaphoreHandle_t gpsClockMutex = nullptr;
static uint64_t anchorEpochMs = 0;
static unsigned long anchorUptimeMs = 0;
static bool gpsTimeValid = false;
static bool gpsModuleDetected = false;
static bool gpsClockTaskRunning = false;

static TinyGPSPlus clockGps;
static HardwareSerial clockSerial(1);

static bool isLeapYear(int year) {
  return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

uint64_t boardGpsUtcEpochMsFromComponents(int year, int month, int day,
                                          int hour, int minute, int second,
                                          int centisecond) {
  if (year < 2000 || year > 2100 || month < 1 || month > 12 ||
      day < 1 || day > 31 || hour < 0 || hour > 23 || minute < 0 ||
      minute > 59 || second < 0 || second > 60 || centisecond < 0 ||
      centisecond > 99) {
    return 0;
  }

  static const int daysInMonth[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  int64_t days = 0;
  for (int y = 1970; y < year; y++) {
    days += isLeapYear(y) ? 366 : 365;
  }
  for (int m = 1; m < month; m++) {
    days += daysInMonth[m - 1];
    if (m == 2 && isLeapYear(year)) {
      days += 1;
    }
  }
  days += day - 1;

  int64_t sec = days * 86400LL + hour * 3600LL + minute * 60LL + second;
  if (sec < 0) {
    return 0;
  }
  return (uint64_t)sec * 1000ULL + (uint64_t)centisecond * 10ULL;
}

static void boardGpsClockEnsureMutex() {
  if (!gpsClockMutex) {
    gpsClockMutex = xSemaphoreCreateMutex();
  }
}

static void boardGpsClockSetAnchor(uint64_t epochMs, unsigned long uptimeMs,
                                   int year, int month, int day,
                                   int hour, int minute, int second) {
  boardGpsClockEnsureMutex();
  if (!gpsClockMutex ||
      xSemaphoreTake(gpsClockMutex, pdMS_TO_TICKS(50)) != pdTRUE) {
    return;
  }

  const bool firstSync = !gpsTimeValid;
  anchorEpochMs = epochMs;
  anchorUptimeMs = uptimeMs;
  gpsTimeValid = (epochMs > 0);

  xSemaphoreGive(gpsClockMutex);

  if (firstSync && gpsTimeValid) {
    printf("[GPS] Time sync: %04d-%02d-%02dT%02d:%02d:%02dZ\n",
           year, month, day, hour, minute, second);
  }
}

void boardGpsClockSyncFromParser(TinyGPSPlus& gps) {
  if (!gps.time.isValid() || !gps.date.isValid()) {
    return;
  }
  if (!gps.time.isUpdated() && !gps.date.isUpdated()) {
    return;
  }

  const int year = gps.date.year();
  const int month = gps.date.month();
  const int day = gps.date.day();
  const int hour = gps.time.hour();
  const int minute = gps.time.minute();
  const int second = gps.time.second();
  const int centisecond = gps.time.centisecond();

  const uint64_t epochMs = boardGpsUtcEpochMsFromComponents(
      year, month, day, hour, minute, second, centisecond);
  if (epochMs == 0) {
    return;
  }

  boardGpsClockSetAnchor(epochMs, millis(), year, month, day, hour, minute, second);
}

bool boardGpsTimeValid() {
  boardGpsClockEnsureMutex();
  if (!gpsClockMutex ||
      xSemaphoreTake(gpsClockMutex, pdMS_TO_TICKS(20)) != pdTRUE) {
    return false;
  }
  const bool valid = gpsTimeValid;
  xSemaphoreGive(gpsClockMutex);
  return valid;
}

bool boardGpsClockModuleDetected() {
  return gpsModuleDetected;
}

uint64_t boardGpsUptimeToEpochMs(unsigned long uptimeMs) {
  boardGpsClockEnsureMutex();
  if (!gpsClockMutex ||
      xSemaphoreTake(gpsClockMutex, pdMS_TO_TICKS(20)) != pdTRUE) {
    return 0;
  }

  uint64_t result = 0;
  if (gpsTimeValid) {
    const int64_t delta =
        (int64_t)uptimeMs - (int64_t)anchorUptimeMs;
    result = (uint64_t)((int64_t)anchorEpochMs + delta);
  }

  xSemaphoreGive(gpsClockMutex);
  return result;
}

uint64_t boardGpsNowEpochMs() {
  return boardGpsUptimeToEpochMs(millis());
}

static void gpsClockTask(void* /*param*/) {
  for (;;) {
    while (clockSerial.available()) {
      const char c = (char)clockSerial.read();
      clockGps.encode(c);
      if (!gpsModuleDetected) {
        gpsModuleDetected = true;
        printf("[GPS] Hardware module detected (%s)\n", boardGpsModuleName());
      }
    }
    boardGpsClockSyncFromParser(clockGps);
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

void boardGpsClockBegin() {
  if (gpsClockTaskRunning) {
    return;
  }
  boardGpsClockEnsureMutex();
  boardGpsUartBegin(clockSerial);
  gpsClockTaskRunning = true;
  xTaskCreate(gpsClockTask, "gpsClock", 4096, nullptr, 1, nullptr);
  printf("[GPS] Clock task started (%s)\n", boardGpsModuleName());
}

void boardGpsPowerOn() {
#ifdef NESSO_N1
  Wire.begin(SDA, SCL);
  pinMode(GROVE_POWER_EN, OUTPUT);
  digitalWrite(GROVE_POWER_EN, HIGH);
#endif
}

void boardGpsUartBegin(HardwareSerial& ser) {
  boardGpsPowerOn();
  ser.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
}

static void boardGpsSendPmtk(HardwareSerial& ser, const char* body) {
  uint8_t checksum = 0;
  for (const char* p = body; *p; p++) {
    checksum ^= (uint8_t)*p;
  }

  char line[48];
  snprintf(line, sizeof(line), "$%s*%02X\r\n", body, checksum);
  ser.print(line);
}

void boardGpsSetFixIntervalMs(HardwareSerial& ser, uint16_t ms) {
#ifdef NESSO_N1
  char body[24];
  snprintf(body, sizeof(body), "PMTK220,%u", (unsigned)ms);
  boardGpsSendPmtk(ser, body);
  delay(100);
#else
  (void)ser;
  (void)ms;
#endif
}

const char* boardGpsProfileName(BoardGpsProfile profile) {
  switch (profile) {
    case GPS_PROFILE_COLLECT:
      return "collect";
    case GPS_PROFILE_DASHBOARD:
      return "dashboard";
    default:
      return "unknown";
  }
}

void boardGpsApplyProfile(HardwareSerial& ser, BoardGpsProfile profile) {
  uint16_t intervalMs = GPS_FIX_INTERVAL_COLLECT_MS;
  if (profile == GPS_PROFILE_DASHBOARD) {
    intervalMs = GPS_FIX_INTERVAL_DASHBOARD_MS;
  }
  boardGpsSetFixIntervalMs(ser, intervalMs);
  printf("[GPS] Profile %s (%u ms fix interval)\n",
         boardGpsProfileName(profile), (unsigned)intervalMs);
}

const char* boardGpsModuleName() {
#ifdef NESSO_N1
  return "M5 Unit GPS v1.1 (Grove UART)";
#else
  return "Seeed L76K (D6/D7)";
#endif
}
