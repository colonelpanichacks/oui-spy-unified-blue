#ifndef NESSO_UI_H
#define NESSO_UI_H

#include <Arduino.h>

enum NessoGpsIndicator : uint8_t {
  NESSO_GPS_HIDDEN = 0,
  NESSO_GPS_SEARCHING,
  NESSO_GPS_FIX,
};

void nessoUiInit();
void nessoUiSetMode(const char *modeName);
void nessoUiSetStatus(const char *line);
void nessoUiPinProfile(const char *line);
void nessoUiSetInfoLine(const char *line);
void nessoUiSetProfileBanner(const char *banner);
void nessoUiRestoreProfile();
void nessoUiClearHoldPrompt();
void nessoResetBootButtonHold();
void nessoPollInput();
void nessoUiSetDetectionCount(int count);
void nessoUiUpdateRssi(int rssi);
void nessoUiFlashAlert();
void nessoUiTick();
void nessoUiSetSelectorActive(bool active);
void nessoUiSetSelectorChoice(const char *modeName, int index, int total);
void nessoUiSetGpsIndicator(NessoGpsIndicator state);
void nessoUiNoteActivity();

#endif
