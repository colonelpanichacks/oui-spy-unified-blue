#line 1 "/Users/nicholastenbrink/projects/nesso/oui_spy_nesso/board_gps.cpp"
#include "board_gps.h"

#ifdef NESSO_N1
#include <Arduino_Nesso_N1.h>
#include <Wire.h>
#endif

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

const char* boardGpsModuleName() {
#ifdef NESSO_N1
  return "M5 Unit GPS v1.1 (Grove UART)";
#else
  return "Seeed L76K (D6/D7)";
#endif
}
