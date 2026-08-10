/*
 * Shared dashboard module for the Seeed Studio Expansion Base for XIAO.
 *
 * The expansion board carries a 0.96" 128x64 SSD1306-family OLED (SSD1315)
 * wired to the XIAO header I2C pins, plus a user button on the D1 position.
 * On the XIAO ESP32S3 the I2C pins are GPIO5 (SDA / D4) and GPIO6 (SCL / D5),
 * and the button is GPIO2.
 *
 * Every function is a safe no-op until a display is detected, so modes keep
 * working exactly as before when no expansion board is attached. Detection is
 * done by probing the I2C bus at the SSD1306/SSD1315 addresses; when nothing
 * answers, the bus is released so other peripherals (e.g. Sky Spy's Serial1
 * mesh UART on the same two pins) can claim them.
 */

#include "dashboard.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <Wire.h>
#include <U8g2lib.h>
#include "qrcode.h"

static U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(
    U8G2_R0, U8X8_PIN_NONE, DISPLAY_SCL_PIN, DISPLAY_SDA_PIN);

static bool displayPresent = false;

bool dashboard_init() {
    static bool alreadyTried = false;
    if (alreadyTried) return displayPresent;
    alreadyTried = true;

    // Probe the bus before committing, so the I2C pins are released back to
    // GPIO when no display is attached. On Sky Spy those pins double as the
    // Serial1 mesh UART, so dashboard_init() must run before Serial1.begin().
    Wire.begin(DISPLAY_SDA_PIN, DISPLAY_SCL_PIN);
    bool found = false;
    uint8_t addrs[] = { DISPLAY_ADDR_0, DISPLAY_ADDR_1 };
    for (size_t i = 0; i < sizeof(addrs); i++) {
        Wire.beginTransmission(addrs[i]);
        if (Wire.endTransmission() == 0) { found = true; break; }
    }
    Wire.end();

    dashboard_button_init();

    if (!found) {
        Serial.println("[DASH] No OLED display detected - running headless");
        return false;
    }

    Serial.println("[DASH] SSD1306/SSD1315 OLED display detected");
    displayPresent = true;
    u8g2.begin();
    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.setDrawColor(1);
    u8g2.clearBuffer();
    u8g2.sendBuffer();
    return true;
}

bool dashboard_present() { return displayPresent; }

int dashboard_buzzer_pin() {
    return displayPresent ? BUZZER_PIN_EXPANSION : BUZZER_PIN_EXTERNAL;
}

void dashboard_clear() {
    if (!displayPresent) return;
    u8g2.clearBuffer();
}

void dashboard_flush() {
    if (!displayPresent) return;
    u8g2.sendBuffer();
}

void dashboard_set_font(const uint8_t *font) {
    if (!displayPresent) return;
    u8g2.setFont(font);
}

void dashboard_set_draw_color(uint8_t color) {
    if (!displayPresent) return;
    u8g2.setDrawColor(color);
}

void dashboard_set_cursor(uint16_t x, uint16_t y) {
    if (!displayPresent) return;
    u8g2.setCursor(x, y);
}

void dashboard_printf(const char *fmt, ...) {
    if (!displayPresent) return;
    char buf[64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    u8g2.print(buf);
}

void dashboard_draw_hline(uint16_t x, uint16_t y, uint16_t w) {
    if (!displayPresent) return;
    u8g2.drawHLine(x, y, w);
}

void dashboard_draw_box(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    if (!displayPresent) return;
    u8g2.drawBox(x, y, w, h);
}

// QR version -> byte-mode data capacity at ECC_LOW (versions 1-3 are the ones
// that render at a scannable 2 px/module on a 128x64 display).
static const uint16_t QR_BYTE_CAPACITY[3] = { 17, 32, 53 };

bool dashboard_draw_qrcode(const char *text) {
    if (!displayPresent) return true;
    if (!text) return false;

    size_t len = strlen(text);
    uint8_t version = 0;
    uint16_t bufferSize = 0;
    for (uint8_t v = 1; v <= 3; v++) {
        if (len <= QR_BYTE_CAPACITY[v - 1]) {
            version = v;
            bufferSize = qrcode_getBufferSize(v);
            break;
        }
    }
    if (version == 0) return false;  // too long to stay scannable here

    uint8_t qrcodeData[bufferSize];
    QRCode qr;
    qrcode_initText(&qr, qrcodeData, version, ECC_LOW, text);

    int size = qr.size;          // modules per side (4*version + 17)
    int scale = 2;               // px per module
    int px = size * scale;
    int x0 = (128 - px) / 2;
    int y0 = (64 - px) / 2;

    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            if (qrcode_getModule(&qr, (uint8_t)x, (uint8_t)y)) {
                u8g2.drawBox(x0 + x * scale, y0 + y * scale, scale, scale);
            }
        }
    }
    return true;
}

void dashboard_button_init() {
    pinMode(DISPLAY_BUTTON_PIN, INPUT_PULLUP);
}

bool dashboard_button_pressed() {
    static uint32_t lastChange = 0;
    static bool lastRaw = HIGH;
    static bool stable = HIGH;
    static bool prevStable = HIGH;

    bool raw = digitalRead(DISPLAY_BUTTON_PIN);
    uint32_t now = millis();
    if (raw != lastRaw) {
        lastRaw = raw;
        lastChange = now;
    }
    if (now - lastChange >= 40) {
        stable = raw;
    }

    bool fresh = (prevStable == HIGH && stable == LOW);  // falling edge
    prevStable = stable;
    return fresh;
}
