#line 1 "/Users/nicholastenbrink/projects/nesso/oui_spy_nesso/board_gps.h"
#ifndef BOARD_GPS_H
#define BOARD_GPS_H

#include <Arduino.h>

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

void boardGpsPowerOn();
void boardGpsUartBegin(HardwareSerial& ser);

// Short label for boot logs.
const char* boardGpsModuleName();

#endif
