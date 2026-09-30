#include "nesso_ui.h"

#ifdef NESSO_N1
#include <Arduino_Nesso_N1.h>
#include <string.h>

static NessoDisplay sDisplay;
static NessoBattery sBattery;
static bool sReady = false;
static char sModeName[24] = "OUI-SPY";
static char sProfileLine[32] = "";
static char sProfileBanner[16] = "";
static char sStatusLine[32] = "";
static char sInfoLine[32] = "";
static int sDetectionCount = 0;
static int sRssi = -100;
static unsigned long sAlertUntil = 0;
static unsigned long sLastBatteryDraw = 0;
static bool sSelectorActive = false;
static char sSelectorChoice[24] = "";
static int sSelectorIndex = 0;
static int sSelectorTotal = 0;
static NessoGpsIndicator sGpsIndicator = NESSO_GPS_HIDDEN;
static bool sBacklightOn = true;
static unsigned long sLastActivityMs = 0;

static constexpr unsigned long kBatteryRefreshMs = 5000;
static constexpr unsigned long kBacklightTimeoutMs = 30000;
static constexpr int kHeaderY = 4;
static constexpr int kGpsIconW = 8;
static constexpr int kGpsIconH = 8;
static constexpr int kGpsIconGap = 4;

static int uiW() { return sDisplay.width(); }
static int uiH() { return sDisplay.height(); }

static void nessoUiSetBacklight(bool on) {
  if (on == sBacklightOn) return;
  sBacklightOn = on;
  digitalWrite(LCD_BACKLIGHT, on ? HIGH : LOW);
}

static void nessoUiDrawAlertBar() {
  const int y = uiH() - 18;
  if (millis() < sAlertUntil) {
    sDisplay.fillRect(0, y, uiW(), 18, TFT_RED);
    sDisplay.setTextColor(TFT_WHITE);
    sDisplay.setTextSize(1);
    sDisplay.setCursor(8, y + 4);
    sDisplay.print("ALERT");
  } else {
    sDisplay.fillRect(0, y, uiW(), 18, TFT_BLACK);
  }
}

static void nessoUiDrawGpsIcon(int x, int y, uint16_t color) {
  sDisplay.drawRect(x + 2, y + 2, 4, 4, color);
  sDisplay.drawLine(x, y + 4, x + 2, y + 4, color);
  sDisplay.drawLine(x + 6, y + 4, x + kGpsIconW, y + 4, color);
  if (color == TFT_GREEN) {
    sDisplay.fillCircle(x + 4, y, 1, color);
  }
}

static void nessoUiDrawHeaderRow() {
  sDisplay.fillRect(0, 0, uiW(), 14, TFT_BLACK);
  sDisplay.setTextColor(TFT_GREEN);
  sDisplay.setTextSize(1);
  sDisplay.setCursor(4, kHeaderY);
  sDisplay.print(sModeName);

  float volts = 0.0f;
  uint16_t pct = 0;
  sBattery.getBatteryStatus(volts, pct);
  char bat[20];
  snprintf(bat, sizeof(bat), "%.2fV %u%%", volts, pct);
  int16_t batX = (int16_t)uiW() - sDisplay.textWidth(bat) - 2;
  if (batX < 4) {
    batX = 4;
  }
  if (sGpsIndicator != NESSO_GPS_HIDDEN) {
    uint16_t gpsColor =
        sGpsIndicator == NESSO_GPS_FIX ? TFT_GREEN : TFT_YELLOW;
    int16_t gpsX = batX - kGpsIconW - kGpsIconGap;
    if (gpsX < 4) {
      gpsX = 4;
    }
    nessoUiDrawGpsIcon(gpsX, kHeaderY, gpsColor);
  }
  if (pct < 5 || volts < 3.2f) {
    sDisplay.setTextColor(TFT_RED);
  } else if (pct < 15 || volts < 3.4f) {
    sDisplay.setTextColor(TFT_YELLOW);
  } else {
    sDisplay.setTextColor(TFT_GREEN);
  }
  sDisplay.setCursor(batX, kHeaderY);
  sDisplay.print(bat);
}

static void nessoUiDrawProfileBanner() {
  sDisplay.fillRect(0, 18, uiW(), 20, TFT_BLACK);
  if (sProfileBanner[0] && !sSelectorActive) {
    sDisplay.setTextColor(TFT_YELLOW);
    sDisplay.setTextSize(2);
    sDisplay.setCursor(4, 20);
    sDisplay.print(sProfileBanner);
  }
}

static void nessoUiDrawDetectionCount() {
  const int y = sProfileBanner[0] ? 42 : 20;
  sDisplay.fillRect(0, y, uiW(), 14, TFT_BLACK);
  if (sDetectionCount > 0) {
    sDisplay.setTextColor(TFT_GREEN);
    sDisplay.setTextSize(1);
    sDisplay.setCursor(4, y + 2);
    sDisplay.printf("Hits: %d", sDetectionCount);
  }
}

static const char *nessoUiActiveStatusLine() {
  if (sStatusLine[0]) {
    return sStatusLine;
  }
  if (sProfileLine[0]) {
    return sProfileLine;
  }
  return nullptr;
}

static void nessoUiDrawInfoLines() {
  if (!sReady || sSelectorActive) return;

  const int line1Y = sProfileBanner[0] ? 58 : 36;
  const int line2Y = line1Y + 14;
  sDisplay.fillRect(0, line1Y, uiW(), 28, TFT_BLACK);

  const char *status = nessoUiActiveStatusLine();
  if (status) {
    sDisplay.setTextColor(TFT_GREEN);
    sDisplay.setTextSize(1);
    sDisplay.setCursor(4, line1Y + 2);
    sDisplay.print(status);
  }

  if (sInfoLine[0]) {
    sDisplay.setTextColor(TFT_DARKGREY);
    sDisplay.setTextSize(1);
    sDisplay.setCursor(4, line2Y + 2);
    sDisplay.print(sInfoLine);
  }

  if (sRssi > -95) {
    const int rssiY = line2Y + (sInfoLine[0] ? 14 : 0);
    sDisplay.fillRect(0, rssiY, uiW(), 20, TFT_BLACK);
    sDisplay.setTextColor(TFT_GREEN);
    sDisplay.setTextSize(1);
    sDisplay.setCursor(4, rssiY + 2);
    sDisplay.printf("RSSI: %d dBm", sRssi);
    int bar = map(constrain(sRssi, -100, -30), -100, -30, 0, 100);
    sDisplay.fillRect(4, rssiY + 12, map(bar, 0, 100, 0, uiW() - 8), 5, TFT_GREEN);
  }
}

static void nessoUiDraw() {
  if (!sReady) return;

  sDisplay.fillScreen(TFT_BLACK);
  nessoUiDrawHeaderRow();
  sLastBatteryDraw = millis();

  nessoUiDrawProfileBanner();
  nessoUiDrawDetectionCount();
  nessoUiDrawInfoLines();

  if (sSelectorActive && sSelectorChoice[0]) {
    sDisplay.fillRect(0, 30, uiW(), 56, TFT_BLACK);
    sDisplay.setTextColor(TFT_YELLOW);
    sDisplay.setTextSize(2);
    sDisplay.setCursor(4, 34);
    sDisplay.print('>');
    sDisplay.print(sSelectorChoice);
    sDisplay.setTextSize(1);
    sDisplay.setTextColor(TFT_GREEN);
    sDisplay.setCursor(4, 58);
    sDisplay.printf("%d / %d", sSelectorIndex + 1, sSelectorTotal);
  }

  nessoUiDrawAlertBar();

  if (sSelectorActive) {
    sDisplay.setTextColor(TFT_DARKGREY);
    sDisplay.setCursor(4, uiH() - 22);
    sDisplay.print("KEY2 next KEY1 go");
  }
}

void nessoUiInit() {
  if (sDisplay.begin()) {
    sDisplay.setRotation(1);
    sReady = true;
    sLastActivityMs = millis();
    sBattery.begin();
    nessoUiDraw();
  }
}

void nessoUiSetMode(const char *modeName) {
  sSelectorActive = false;
  sSelectorChoice[0] = '\0';
  if (modeName) {
    strncpy(sModeName, modeName, sizeof(sModeName) - 1);
    sModeName[sizeof(sModeName) - 1] = '\0';
  }
  nessoUiDraw();
}

void nessoUiSetStatus(const char *line) {
  if (line) {
    strncpy(sStatusLine, line, sizeof(sStatusLine) - 1);
    sStatusLine[sizeof(sStatusLine) - 1] = '\0';
  } else {
    sStatusLine[0] = '\0';
  }
  nessoUiDraw();
}

void nessoUiPinProfile(const char *line) {
  if (line) {
    strncpy(sProfileLine, line, sizeof(sProfileLine) - 1);
    sProfileLine[sizeof(sProfileLine) - 1] = '\0';
  } else {
    sProfileLine[0] = '\0';
  }
  if (!sStatusLine[0]) {
    nessoUiDrawInfoLines();
  }
}

void nessoUiSetInfoLine(const char *line) {
  if (line) {
    strncpy(sInfoLine, line, sizeof(sInfoLine) - 1);
    sInfoLine[sizeof(sInfoLine) - 1] = '\0';
  } else {
    sInfoLine[0] = '\0';
  }
  nessoUiDrawInfoLines();
}

void nessoUiSetProfileBanner(const char *banner) {
  if (banner) {
    strncpy(sProfileBanner, banner, sizeof(sProfileBanner) - 1);
    sProfileBanner[sizeof(sProfileBanner) - 1] = '\0';
  } else {
    sProfileBanner[0] = '\0';
  }
  if (sReady) {
    nessoUiDrawProfileBanner();
    nessoUiDrawDetectionCount();
    nessoUiDrawInfoLines();
  }
}

void nessoUiRestoreProfile() {
  sStatusLine[0] = '\0';
  nessoUiDrawInfoLines();
}

void nessoUiClearHoldPrompt() {
  if (strcmp(sStatusLine, "Hold for menu...") == 0 ||
      strcmp(sStatusLine, "KEY1 again: Dashboard") == 0 ||
      strcmp(sStatusLine, "KEY1 again: Collection") == 0) {
    nessoUiRestoreProfile();
  }
}

void nessoUiSetDetectionCount(int count) {
  if (count == sDetectionCount) {
    return;
  }
  sDetectionCount = count;
  if (sSelectorActive) {
    nessoUiDraw();
  } else {
    nessoUiDrawDetectionCount();
  }
}

void nessoUiUpdateRssi(int rssi) {
  if (rssi == sRssi) {
    return;
  }
  sRssi = rssi;
  nessoUiDrawInfoLines();
}

void nessoUiFlashAlert() {
  sAlertUntil = millis() + 500;
  if (!sReady) return;
  nessoUiDrawAlertBar();
}

void nessoUiTick() {
  if (!sReady) return;

  unsigned long now = millis();

  if (now - sLastBatteryDraw >= kBatteryRefreshMs) {
    nessoUiDrawHeaderRow();
    sLastBatteryDraw = now;
  }

  if (sAlertUntil != 0 && now > sAlertUntil) {
    sAlertUntil = 0;
    nessoUiDrawAlertBar();
  }

  if (sBacklightOn && (now - sLastActivityMs >= kBacklightTimeoutMs)) {
    nessoUiSetBacklight(false);
  }
}

void nessoUiNoteActivity() {
  sLastActivityMs = millis();
  nessoUiSetBacklight(true);
}

void nessoUiSetSelectorActive(bool active) {
  sSelectorActive = active;
  if (!active) {
    sSelectorChoice[0] = '\0';
    sSelectorIndex = 0;
    sSelectorTotal = 0;
  }
  nessoUiDraw();
}

void nessoUiSetSelectorChoice(const char *modeName, int index, int total) {
  sSelectorActive = true;
  sSelectorIndex = index;
  sSelectorTotal = total;
  if (modeName) {
    strncpy(sSelectorChoice, modeName, sizeof(sSelectorChoice) - 1);
    sSelectorChoice[sizeof(sSelectorChoice) - 1] = '\0';
  }
  nessoUiDraw();
}

void nessoUiSetGpsIndicator(NessoGpsIndicator state) {
  if (state == sGpsIndicator) {
    return;
  }
  sGpsIndicator = state;
  if (!sReady) {
    return;
  }
  nessoUiDrawHeaderRow();
}

#else

void nessoUiInit() {}
void nessoUiSetMode(const char *) {}
void nessoUiSetStatus(const char *) {}
void nessoUiPinProfile(const char *) {}
void nessoUiSetInfoLine(const char *) {}
void nessoUiSetProfileBanner(const char *) {}
void nessoUiRestoreProfile() {}
void nessoUiClearHoldPrompt() {}
void nessoResetBootButtonHold() {}
void nessoUiSetDetectionCount(int) {}
void nessoUiUpdateRssi(int) {}
void nessoUiFlashAlert() {}
void nessoUiTick() {}
void nessoUiSetSelectorActive(bool) {}
void nessoUiSetSelectorChoice(const char *, int, int) {}
void nessoUiSetGpsIndicator(NessoGpsIndicator) {}
void nessoUiNoteActivity() {}

#endif
