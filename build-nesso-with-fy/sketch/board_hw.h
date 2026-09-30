#line 1 "/Users/nicholastenbrink/projects/nesso/oui_spy_nesso/board_hw.h"
#ifndef BOARD_HW_H
#define BOARD_HW_H

#include "board_pins.h"

void boardHwInit();
void boardLedInit();
void boardLedOn();
void boardLedOff();
void boardLedWrite(bool on);

// KEY1 (front): hold ~1.5s in firmware to open mode selector
bool boardMenuButtonPressed();
bool boardBootButtonPressed();  // alias for boardMenuButtonPressed()

// KEY2 (side): currently unused by firmware; initialized for future use
bool boardSecondaryButtonPressed();

void boardBuzzerAttach();
void boardBuzzerWrite(uint8_t duty);
void boardAlertVisual(uint8_t r, uint8_t g, uint8_t b);

#endif
