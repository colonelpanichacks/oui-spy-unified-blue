#ifndef BOARD_GPS_H
#define BOARD_GPS_H

#include <Arduino.h>
#include <stdint.h>

// Accuracy estimate from HDOP when NMEA does not include a literal accuracy field.
#define GPS_HDOP_SCALE 5.0f

#ifdef NESSO_N1
// M5 Unit GPS v1.1 on HY2.0-4P Grove (PORT.C): Yellow=host TX, White=host RX.
#define GPS_TX_PIN GROVE_IO_0
#define GPS_RX_PIN GROVE_IO_1
#define GPS_BAUD   115200
#else
// Seeed L76K GNSS on XIAO ESP32-S3 (D6/D7).
#define GPS_RX_PIN 44
#define GPS_TX_PIN 43
#define GPS_BAUD   9600
#endif

class TinyGPSPlus;

enum BoardGpsProfile : uint8_t {
  GPS_PROFILE_COLLECT = 0,
  GPS_PROFILE_DASHBOARD = 1,
};

#define GPS_FIX_INTERVAL_COLLECT_MS    200
#define GPS_FIX_INTERVAL_DASHBOARD_MS  1000
#define GPS_DASHBOARD_POLL_MS          1000

void boardGpsPowerOn();
void boardGpsUartBegin(HardwareSerial& ser);

// ATGM336H fix interval via PMTK220 (Nesso N1 Grove GPS).
void boardGpsSetFixIntervalMs(HardwareSerial& ser, uint16_t ms);
void boardGpsApplyProfile(HardwareSerial& ser, BoardGpsProfile profile);
const char* boardGpsProfileName(BoardGpsProfile profile);

// Short label for boot logs.
const char* boardGpsModuleName();

// --- GPS UTC clock (anchor: uptime ms -> epoch ms) ---

// Start UART1 NMEA reader + background task (modes without their own GPS loop).
void boardGpsClockBegin();

// Update anchor from a TinyGPSPlus instance (Mega_Maid / Flock-You hardware path).
void boardGpsClockSyncFromParser(TinyGPSPlus& gps);

bool boardGpsTimeValid();
bool boardGpsClockModuleDetected();

// Convert boot-uptime ms to UTC epoch ms; returns 0 when not synced.
uint64_t boardGpsUptimeToEpochMs(unsigned long uptimeMs);
uint64_t boardGpsNowEpochMs();

// UTC epoch ms from calendar components (testable, UTC-safe).
uint64_t boardGpsUtcEpochMsFromComponents(int year, int month, int day,
                                          int hour, int minute, int second,
                                          int centisecond);

#endif
