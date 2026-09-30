#line 1 "/Users/nicholastenbrink/projects/nesso/oui_spy_nesso/board_pins.h"
#ifndef BOARD_PINS_H
#define BOARD_PINS_H

#ifdef NESSO_N1
#include <Arduino_Nesso_N1.h>
#include <freertos/task.h>

// Nesso N1 buttons (I2C IO expander, active-low when pressed):
//   KEY1 — front face button (near the display)
//   KEY2 — side edge button
// Power/reset is a separate hardware button; it is NOT KEY1 or KEY2.
#define MENU_BUTTON_PIN KEY1
#define BOOT_BUTTON_PIN MENU_BUTTON_PIN  // legacy alias used by mode sources

#define BUZZER_PIN BEEP_PIN
#define BOARD_LED LED_BUILTIN
#define LED_ACTIVE_LOW 0
#define BUZZER_FREQ 4000
#define BUZZER_DUTY 127
#define HAS_NEOPIXEL 0
// No physical NeoPixel on the N1; the Adafruit_NeoPixel shim ignores the pin
// and routes colors to the display/LED. Value is a harmless placeholder.
#define BOARD_NEOPIXEL_PIN 0

inline void boardLedcConfigure(uint32_t freq) {
  ledcAttach(BUZZER_PIN, freq, 8);
}

inline void boardLedcSetDuty(uint32_t duty) {
  ledcWrite(BUZZER_PIN, duty);
}

inline void boardLedcTone(uint32_t freq) {
  ledcWriteTone(BUZZER_PIN, freq);
}

#define TASK_CREATE_PINNED(fn, name, stack, param, prio, handle, core) \
  xTaskCreate(fn, name, stack, param, prio, handle)

#else

#define LED_PIN 21
#define BUZZER_PIN 3
#define BOOT_BUTTON_PIN 0
#define MENU_BUTTON_PIN BOOT_BUTTON_PIN
#define BOARD_LED LED_PIN
#define LED_ACTIVE_LOW 1
#define BUZZER_FREQ 2000
#define BUZZER_DUTY 127
#define HAS_NEOPIXEL 1
// Physical NeoPixel on the XIAO ESP32-S3 (GPIO4 / D3).
#define BOARD_NEOPIXEL_PIN 4

inline void boardLedcConfigure(uint32_t freq) {
  ledcSetup(0, freq, 8);
  ledcAttachPin(BUZZER_PIN, 0);
}

inline void boardLedcSetDuty(uint32_t duty) {
  ledcWrite(0, duty);
}

inline void boardLedcTone(uint32_t freq) {
  ledcWriteTone(0, freq);
}

#define TASK_CREATE_PINNED(fn, name, stack, param, prio, handle, core) \
  xTaskCreatePinnedToCore(fn, name, stack, param, prio, handle, core)

#endif

#define BOOT_HOLD_TIME 1500
#define MENU_HOLD_TIME BOOT_HOLD_TIME

#ifndef BEEP_DURATION
#define BEEP_DURATION 200
#define BEEP_PAUSE 150
#endif

#endif
