#ifndef DASHBOARD_H
#define DASHBOARD_H

#include <Arduino.h>

// 0.96" 128x64 OLED on the Seeed Studio Expansion Base for XIAO.
// XIAO ESP32S3 I2C pins: SDA = GPIO5 (D4), SCL = GPIO6 (D5).
// WARNING: these are the same pins Sky Spy uses for its Serial1 mesh UART,
// so the dashboard is mutually exclusive with mesh forwarding on that mode.
#define DISPLAY_SDA_PIN 5
#define DISPLAY_SCL_PIN 6
#define DISPLAY_ADDR_0 0x3C
#define DISPLAY_ADDR_1 0x3D

// User button on the expansion board. It is wired to the D1 header position,
// which is GPIO2 on the XIAO ESP32S3. Active low with the internal pull-up.
#define DISPLAY_BUTTON_PIN 2

// Buzzer hardware routing.
// The oui-spy build drives an external piezo buzzer on GPIO3 (D2). The Seeed
// Studio XIAO Expansion Board carries its own passive buzzer on GPIO4 (D3/A3).
// When the expansion board is detected, buzzer output is routed to GPIO4
// instead, so GPIO4 must not be used for the optional NeoPixel.
#define BUZZER_PIN_EXTERNAL 3
#define BUZZER_PIN_EXPANSION 4

// Pin to drive the buzzer on for the currently attached hardware.
int dashboard_buzzer_pin();

// Probe the I2C bus and init the OLED when present. Returns true once a
// display is attached. When absent, every other dashboard_* call is a safe
// no-op, so firmware behavior is unchanged. Call this before any other
// peripheral claims DISPLAY_SDA_PIN/DISPLAY_SCL_PIN.
bool dashboard_init();

// True once a display has been detected and initialized.
bool dashboard_present();

// Clear the framebuffer (start of a frame).
void dashboard_clear();

// Push the framebuffer to the display (end of a frame).
void dashboard_flush();

// Select the U8g2 font used by subsequent text (see U8g2lib.h for symbols).
void dashboard_set_font(const uint8_t *font);

// Set the U8g2 draw color (1 = draw, 0 = erase). Lets callers invert text on a
// filled box to highlight menu selections.
void dashboard_set_draw_color(uint8_t color);

// Move the text cursor to pixel position (x, y).
void dashboard_set_cursor(uint16_t x, uint16_t y);

// Formatted text at the current cursor.
void dashboard_printf(const char *fmt, ...);

// Simple primitives (all in pixels).
void dashboard_draw_hline(uint16_t x, uint16_t y, uint16_t w);
void dashboard_draw_box(uint16_t x, uint16_t y, uint16_t w, uint16_t h);

// Generate a QR code for `text` and draw it centered on the display, scaled to
// fit. Returns false (and draws nothing) when the text is too long to encode in
// a QR that stays scannable on this 128x64 display.
bool dashboard_draw_qrcode(const char *text);

// Expansion board user button (active low, internal pull-up).
void dashboard_button_init();
// True once per fresh press (debounced, edge-triggered).
bool dashboard_button_pressed();

#endif // DASHBOARD_H
