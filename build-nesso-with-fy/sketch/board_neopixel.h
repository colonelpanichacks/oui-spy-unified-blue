#line 1 "/Users/nicholastenbrink/projects/nesso/oui_spy_nesso/board_neopixel.h"
#ifndef BOARD_NEOPIXEL_H
#define BOARD_NEOPIXEL_H

#include <Arduino.h>
#include "board_hw.h"

#ifdef NESSO_N1
#include "nesso_ui.h"

#define NEO_GRB 0
#define NEO_KHZ800 0

class Adafruit_NeoPixel {
public:
  Adafruit_NeoPixel(uint16_t, uint8_t, uint8_t) {}

  static uint32_t Color(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
  }

  void begin() {}
  void setBrightness(uint8_t) {}
  void setPixelColor(uint16_t, uint32_t color) { _color = color; }
  void show() {
    uint8_t r = (_color >> 16) & 0xFF;
    uint8_t g = (_color >> 8) & 0xFF;
    uint8_t b = _color & 0xFF;
    boardAlertVisual(r, g, b);
  }

  void clear() {
    _color = 0;
    boardLedOff();
  }

private:
  uint32_t _color = 0;
};

typedef int neoPixelType;

#else
#include <Adafruit_NeoPixel.h>
#endif

#endif
