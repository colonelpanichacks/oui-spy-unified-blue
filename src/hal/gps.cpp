#include "gps.h"

namespace hal {

#define GPS_BAUD 9600
#define GPS_STALE_MS 30000
#define GPS_HW_TIMEOUT 5000
#define GPS_HDOP_SCALE 5.0f

static TinyGPSPlus _parser;
static HardwareSerial _serial(1);
static GPSData _data = {};
static unsigned long _lastChar = 0;

void gpsInit() {
    _serial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
    memset(&_data, 0, sizeof(_data));
    Serial.println("[HAL] GPS initialized (L76K on D6/D7)");
}

void gpsUpdate() {
    // Read UART bytes into TinyGPSPlus parser
    while (_serial.available()) {
        char c = _serial.read();
        _parser.encode(c);
        _lastChar = millis();
        if (!_data.hwDetected) {
            _data.hwDetected = true;
            Serial.println("[HAL] Hardware GPS module detected");
        }
    }

    // Timeout: no NMEA for 5s -> module absent
    if (_data.hwDetected && (millis() - _lastChar > GPS_HW_TIMEOUT)) {
        if (_data.isHardware) {
            Serial.println("[HAL] Hardware GPS timeout - falling back to phone GPS");
        }
        _data.hwDetected = false;
        _data.hwFix = false;
        _data.satellites = 0;
        _data.isHardware = false;
    }

    // Update satellite count
    if (_parser.satellites.isUpdated()) {
        _data.satellites = _parser.satellites.value();
    }

    // Update position on valid fix
    if (_parser.location.isUpdated() && _parser.location.isValid()) {
        if (!_data.hwFix) {
            Serial.printf("[HAL] GPS fix acquired! Sats:%d Lat:%.6f Lon:%.6f\n", _data.satellites,
                          _parser.location.lat(), _parser.location.lng());
        }
        _data.hwFix = true;
        _data.isHardware = true;
        _data.lat = _parser.location.lat();
        _data.lon = _parser.location.lng();
        _data.accuracy =
            _parser.hdop.isValid() ? (float)(_parser.hdop.hdop() * GPS_HDOP_SCALE) : 10.0f;
        _data.valid = true;
        _data.lastUpdate = millis();
    } else if (_data.hwFix && _parser.location.isValid()) {
        _data.lastUpdate = millis();
    }
}

const GPSData& gpsGet() {
    return _data;
}

bool gpsIsFresh() {
    return _data.valid && (millis() - _data.lastUpdate < GPS_STALE_MS);
}

const char* gpsSourceStr() {
    if (_data.hwFix)
        return "hw";
    if (gpsIsFresh())
        return "phone";
    return "none";
}

String gpsGetTime() {
    // Use GPS time if date and time are valid
    if (_parser.date.isValid() && _parser.time.isValid() && _parser.date.year() >= 2020) {
        char buf[24];
        snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d", _parser.date.year(),
                 _parser.date.month(), _parser.date.day(), _parser.time.hour(),
                 _parser.time.minute(), _parser.time.second());
        return String(buf);
    }

    // Fallback: boot-relative timestamp
    unsigned long s = millis() / 1000;
    char buf[24];
    snprintf(buf, sizeof(buf), "0000-00-00 %02lu:%02lu:%02lu", s / 3600, (s % 3600) / 60, s % 60);
    return String(buf);
}

void gpsSetFromPhone(double lat, double lon, float accuracy) {
    // Ignore phone GPS when hardware has a fix
    if (_data.hwFix)
        return;
    _data.lat = lat;
    _data.lon = lon;
    _data.accuracy = accuracy;
    _data.valid = true;
    _data.isHardware = false;
    _data.lastUpdate = millis();
}

} // namespace hal
