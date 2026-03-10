#pragma once

#include "pins.h"
#include <Arduino.h>
#include <TinyGPS++.h>

// ============================================================================
// Shared GPS Service (hardware L76K GNSS + phone browser geolocation)
// ============================================================================

namespace hal {

struct GPSData {
    double lat;
    double lon;
    float accuracy;
    bool valid;
    bool isHardware; // true = HW GPS, false = phone
    int satellites;
    bool hwDetected; // Module physically present
    bool hwFix;      // Has a valid position fix
    unsigned long lastUpdate;
};

void gpsInit();
void gpsUpdate(); // Call every loop iteration to process UART data

// Get current GPS state
const GPSData& gpsGet();

// Is GPS data fresh (< 30s old)?
bool gpsIsFresh();

// GPS source as a short string: "hw", "phone", or "none"
const char* gpsSourceStr();

// Get formatted GPS time for CSV timestamps (YYYY-MM-DD HH:MM:SS)
// Returns boot-relative timestamp if no GPS time available.
String gpsGetTime();

// Set GPS from phone browser (via web API)
void gpsSetFromPhone(double lat, double lon, float accuracy);

} // namespace hal
