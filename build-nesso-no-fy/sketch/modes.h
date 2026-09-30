#line 1 "/Users/nicholastenbrink/projects/nesso/oui_spy_nesso/modes.h"
#ifndef MODES_H
#define MODES_H

// Mode 1: OUI Spy Detector
void detector_setup();
void detector_loop();

// Mode 2: Foxhunter
void foxhunter_setup();
void foxhunter_loop();

// Mode 4: Flock-You
void flockyou_setup();
void flockyou_loop();

// Mode 6: Mega_Maid
void megamaid_setup();
void megamaid_loop();

// Mode 5: Sky Spy
void skyspy_setup();
void skyspy_loop();

#endif // MODES_H
