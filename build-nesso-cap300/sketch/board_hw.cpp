#line 1 "/Users/nicholastenbrink/projects/nesso/oui_spy_nesso/board_hw.cpp"
#include "board_hw.h"

#ifdef NESSO_N1
#include "nesso_ui.h"
#include <Wire.h>
#endif

#ifdef NESSO_N1
static void boardEnsureWire() {
  Wire.begin(SDA, SCL);
}

static bool boardKeysInputReady = false;

static void boardEnsureKeysInput() {
  boardEnsureWire();
  if (boardKeysInputReady) {
    return;
  }
  pinMode(KEY1, INPUT_PULLUP);
  pinMode(KEY2, INPUT_PULLUP);
  boardKeysInputReady = true;
}

static bool boardKeyPressed(ExpanderPin key) {
  return digitalRead(key) == LOW;
}
#endif

void boardHwInit() {
#ifdef NESSO_N1
  boardEnsureWire();
  pinMode(LED_BUILTIN, OUTPUT);
  boardEnsureKeysInput();
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  boardLedOff();
  nessoUiInit();
#else
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  boardLedInit();
  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
#endif
}

void boardLedInit() {
  pinMode(BOARD_LED, OUTPUT);
  boardLedOff();
}

void boardLedOn() {
#ifdef NESSO_N1
  digitalWrite(BOARD_LED, LOW);
#else
  digitalWrite(BOARD_LED, LED_ACTIVE_LOW ? LOW : HIGH);
#endif
}

void boardLedOff() {
#ifdef NESSO_N1
  digitalWrite(BOARD_LED, HIGH);
#else
  digitalWrite(BOARD_LED, LED_ACTIVE_LOW ? HIGH : LOW);
#endif
}

void boardLedWrite(bool on) {
  if (on) {
    boardLedOn();
  } else {
    boardLedOff();
  }
}

bool boardMenuButtonPressed() {
#ifdef NESSO_N1
  boardEnsureKeysInput();
  return boardKeyPressed(MENU_BUTTON_PIN);
#else
  return digitalRead(MENU_BUTTON_PIN) == LOW;
#endif
}

bool boardBootButtonPressed() {
  return boardMenuButtonPressed();
}

bool boardSecondaryButtonPressed() {
#ifdef NESSO_N1
  boardEnsureKeysInput();
  return boardKeyPressed(KEY2);
#else
  return false;
#endif
}

void boardBuzzerAttach() {
  boardLedcConfigure(BUZZER_FREQ);
}

void boardBuzzerWrite(uint8_t duty) {
  boardLedcSetDuty(duty);
}

void boardAlertVisual(uint8_t r, uint8_t g, uint8_t b) {
  (void)g;
  (void)b;
#ifdef NESSO_N1
  if (r > 128) {
    boardLedOn();
    nessoUiFlashAlert();
  } else if (g > 128) {
    boardLedOn();
  } else {
    boardLedOff();
  }
#else
  if (r > 128 || g > 128) {
    boardLedOn();
  } else {
    boardLedOff();
  }
#endif
}
