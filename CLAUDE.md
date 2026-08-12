# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Hard Rules — DO NOT VIOLATE

### Never modify global PlatformIO packages
- **NEVER** modify, rename, delete, or overwrite files in `~/.platformio/packages/`. This is a shared global directory that affects ALL PlatformIO projects on the system.
- If a build needs a different framework version, use `platform_packages` overrides in `platformio.ini` — never manually swap framework directories.
- If two targets need incompatible frameworks, use **separate project directories**, not the same shared package directory.

### Never make system-wide changes for project-local problems
- Scope all changes to the project directory. If a workaround requires touching files outside the project, stop and find a project-local solution instead.
- Before modifying anything in `~/.platformio/`, `~/.config/`, or any global path, ask the user for explicit permission and explain the blast radius.

### Always clean build after framework changes
- If the PlatformIO framework package was modified or reinstalled for any reason, always run `pio run -t clean` before building. Stale bootloader binaries from a wrong framework cause unrecoverable boot loops (`ets_loader.c 78`).

### Verify hardware claims with manufacturer documentation
- Do not assume GPIO mappings, onboard peripherals (LEDs, NeoPixels), or antenna configurations based on similar boards. Always verify against the manufacturer's official documentation (e.g., Seeed Wiki) before writing code that depends on specific hardware features.

## Project Overview

OUI-SPY Unified Blue is a multi-mode firmware for the **Seeed Studio XIAO ESP32-S3**. It provides a unified bootloader with 4 selectable firmware modes for surveillance device detection and drone monitoring, all running on a single device with a WiFi-based boot selector.

**Modes:** Detector (BLE scanner with OUI/MAC watchlists), Foxhunter (RSSI proximity tracker), Flock-You (Flock Safety / Raven surveillance device detector), Sky Spy (FAA Remote ID / Open Drone ID drone detector).

## Build & Flash Commands

```bash
pio run                     # Build firmware
pio run -t upload           # Build and flash via USB
pio run -t clean            # Clean build artifacts
pio device monitor          # Serial monitor (115200 baud)
python flash.py             # Flash pre-compiled bin from firmware/ folder
python flash.py --erase     # Full erase before flashing
```

Build output: `.pio/build/seeed_xiao_esp32s3/firmware.bin`

There are no automated tests. Validation is done by flashing to hardware and testing each mode.

## Architecture

### Unified Bootloader (`src/main.cpp`)

The entry point. On boot it checks the BOOT button (GPIO 0), reads the stored mode from NVS, and dispatches to the selected mode's `setup()`/`loop()` functions. Mode 0 (Selector) serves a WiFi AP with a web UI for choosing modes and configuring AP credentials/buzzer. Holding BOOT for 2s from any mode returns to the selector.

### Mode Wrapper Pattern

Each mode's original standalone firmware lives unmodified in `src/raw/`. Wrapper files (`src/mode_*.cpp`) encapsulate them using this pattern to avoid linker symbol collisions:

```cpp
#define setup modename_ns_setup
#define loop  modename_ns_loop
namespace {
    #include "raw/modename.cpp"    // Original firmware included verbatim
}
#undef setup
#undef loop
void modename_setup() { modename_ns_setup(); }
void modename_loop()  { modename_ns_loop(); }
```

The `#define` renames Arduino's `setup`/`loop`, the anonymous namespace gives all symbols internal linkage, and the exported `modename_setup()`/`modename_loop()` functions are declared in `src/modes.h` and called from `main.cpp`.

**Critical:** Files in `src/raw/` are excluded from direct compilation via `src_filter = +<*> -<raw/>` in `platformio.ini`. They are only compiled through `#include` in the wrapper files. Do not add them to the build filter.

### Mode-to-File Mapping

| Mode | ID | Wrapper | Implementation | AP |
|------|----|---------|----------------|----|
| Selector | 0 | `main.cpp` | `main.cpp` | `oui-spy` / `ouispy123` |
| Detector | 1 | `mode_detector.cpp` | `raw/detector.cpp` | `snoopuntothem` / `astheysnoopuntous` |
| Foxhunter | 2 | `mode_foxhunter.cpp` | `raw/foxhunter.cpp` | `foxhunter` / `foxhunter` |
| Flock-You | 4 | `mode_flockyou.cpp` | `raw/flockyou.cpp` | `flockyou` / `flockyou123` |
| Sky Spy | 5 | `mode_skyspy.cpp` | `raw/skyspy.cpp` | No AP (passive WiFi scanner) |

Note: Mode IDs 3 is skipped intentionally.

### Key Patterns

- **NVS persistence:** `Preferences` library stores mode selection (`"unified-mode"` namespace), AP credentials (`"ouispy-ap"`), and buzzer state (`"ouispy-bz"`). Always call `prefs.end()` after use.
- **WiFi factory reset on boot:** `esp_wifi_restore()` clears stale NVS WiFi config every boot before mode init.
- **MAC randomization:** Random locally-administered MAC generated on each boot via `esp_random()`.
- **Async web servers:** All modes with dashboards use `ESPAsyncWebServer` on port 80 at `192.168.4.1`.
- **BLE scanning:** NimBLE library with `NimBLEAdvertisedDeviceCallbacks` for advertisement processing.
- **Buzzer audio:** LEDC PWM on GPIO 3 (inverted logic). Frequencies range from 600 Hz (heartbeat) to 3000 Hz (confirmation).
- **LED:** GPIO 21 has inverted logic (LOW = ON). Optional NeoPixel on GPIO 4.
- **Embedded HTML:** Stored as `PROGMEM` raw string literals with `%PLACEHOLDER%` template substitution.
- **Device cooldowns:** Detection modes use timed cooldowns (3s or 30s) to prevent alert spam on the same device.
- **Sky Spy differs:** Uses WiFi promiscuous mode (+ BLE passive scan) to capture ASTM F3411 Open Drone ID frames. The OpenDroneID parser is in `src/opendroneid.h/c` and `src/wifi.c`. Outputs full JSON on USB Serial (every detection) AND a Serial1 line per detection whose format is gated by expansion board presence: headless builds send compact human-readable messages on pins 5/6 for the Heltec LoRa/Meshtastic gateway (unchanged legacy behavior); when the expansion board is detected the full JSON line is sent on the Grove UART (GPIO43 TX / GPIO44 RX) for a second XIAO ESP32-S3 running the `sky-spy-relay` firmware, which publishes it to MQTT.
- **Expansion board dashboard:** Shared `src/dashboard.h` / `src/dashboard.cpp` module (U8g2) that auto-detects the expansion board OLED on I2C and no-ops when absent. Modes call `dashboard_init()` early, then draw with `dashboard_*()` helpers; the USER button advances multi-page dashboards.

### Hardware

**Board:** Seeed Studio XIAO ESP32-S3 with PSRAM. Custom partition table: ~6MB app + ~2MB LittleFS (`partitions.csv`).

| GPIO | Function |
|------|----------|
| 0 | BOOT button (hold 2s → return to selector) |
| 2 / D1 | Expansion board USER button (active low, internal pull-up) |
| 3 | Buzzer (external oui-spy piezo, PWM; see note below) |
| 4 / D3 | Expansion board buzzer (A3) when chassis present; else optional NeoPixel LED |
| 5 / D4 | OLED I2C SDA (expansion board); Serial1 TX — mesh UART to Heltec LoRa gateway (Sky Spy, headless) |
| 6 / D5 | OLED I2C SCL (expansion board); Serial1 RX — mesh UART from Heltec LoRa gateway (Sky Spy, headless) |
| 43 / D6 | Serial1 TX — relay UART to sky-spy-relay (Sky Spy, expansion board present); Grove UART TX |
| 44 / D7 | Serial1 RX — relay UART from sky-spy-relay (Sky Spy, expansion board present); Grove UART RX |
| 21 | Onboard LED (inverted logic) |

**Buzzer routing:** the oui-spy build drives an external piezo on GPIO3 (D2). The **Seeed Studio XIAO Expansion Board** carries its own passive buzzer on **GPIO4 (D3/A3)**. When the expansion board is detected (`dashboard_present()`), `dashboard_buzzer_pin()` returns GPIO4 and every mode routes its `BUZZER_PIN` there instead; GPIO4 then cannot drive the optional NeoPixel, so Detector and Flock-You skip NeoPixel init/animations while the chassis is present.

### Expansion Board OLED Dashboard

The **Seeed Studio Expansion Base for XIAO** carries a 0.96" 128x64 SSD1306-family OLED (SSD1315) plus a USER button. Shared driver: `src/dashboard.h` / `src/dashboard.cpp` (uses U8g2, detected by I2C probe at `0x3C`/`0x3D`). On the XIAO ESP32S3 the OLED is on **GPIO5 (SDA) / GPIO6 (SCL)** and the button on **GPIO2**.

- **Auto-detect, never required:** `dashboard_init()` probes the I2C bus; if nothing answers it releases the pins and every other `dashboard_*` call becomes a no-op, so modes behave exactly as before with no display.
- **Sky Spy Serial1 output is gated by expansion board presence:** headless builds send compact mesh messages to the Heltec LoRa/Meshtastic gateway on GPIO5/6 (unchanged legacy behavior). When the OLED is present, GPIO5/6 belong to the display and Serial1 moves to the Grove UART pins GPIO43/44 (`dashboard_present()` selects the pins in `initializeSerial()`), carrying the full JSON detection stream for the `sky-spy-relay` board. `dashboard_init()` runs before `Serial1.begin()` to probe the OLED and pick the correct pins.
- **Multi-page dashboards:** the USER button (GPIO2) advances pages. Sky Spy implements 4 pages (Summary, Latest Drone, Position, Fleet) via the `dashPage*()` render functions in `src/raw/skyspy.cpp`, redrawn at 1 Hz.
- `dashboard_printf` uses U8g2's `setCursor(x, y)` where **y is the text baseline** (font `u8g2_font_5x7_tr` ascent = 6), so row `i` lives at `y = 6 + i*8`.
- All draw/press calls are safe no-ops until `dashboard_init()` returns true. Call `dashboard_init()` before any other peripheral claims GPIO5/6.

### Relay UART (Sky Spy)

The Sky Spy relay UART (`Serial1`, 115200 8N1) lives on the expansion board's **Grove UART** connector, which is wired to **GPIO43 (TX / D6) and GPIO44 (RX / D7)** on the XIAO ESP32-S3. It carries one full JSON detection line per detection. A second XIAO ESP32-S3 running `sky-spy-relay` receives this stream and publishes it to MQTT (`skyspy/<topic>/raw` and `skyspy/<topic>/detections`). The Grove UART pins do NOT overlap the OLED on GPIO5/6, so the OLED dashboard and the relay stream coexist on the same board. This relay path is only active when the expansion board is detected; headless builds keep the legacy compact mesh messages on GPIO5/6.

Note: GPIO43/44 are the same pins Flock-You uses for its hardware GPS (Seeed L76K GNSS). This is fine because Flock-You and Sky Spy are never active at the same time (mode selection reboots the board).

Pins claimed by other hardware: GPIO0 (BOOT button), GPIO2 (USER button), GPIO3 (external buzzer), GPIO4 (expansion buzzer / optional NeoPixel), GPIO5/6 (OLED I2C), GPIO21 (onboard LED), GPIO43/44 (Sky Spy relay UART / Flock-You GPS). Free XIAO header pins: **D0 (GPIO1), D8 (GPIO7), D9 (GPIO8), D10 (GPIO9), D11 (GPIO42), D12 (GPIO41)**.

### Dependencies (managed by PlatformIO)

- `NimBLE-Arduino` ^1.4.0 — BLE scanning
- `ESP Async WebServer` ^3.0.6 — Web interfaces
- `ArduinoJson` ^7.0.4 — JSON serialization
- `Adafruit NeoPixel` ^1.12.0 — LED control
- `U8g2` ^2.36.18 — expansion board OLED driver

## ESP32-C6 Experimental Branch

An experimental standalone Flock-You build for the XIAO ESP32-C6 exists on the `esp32c6-flockyou` branch (release: `esp32c6-v1.0`). It is NOT on master. If working on C6 support in the future, use a **separate project directory** to avoid framework conflicts with the S3 build.

## Git Identity (this repo)

- user.name: suteny0r
- user.email: suteny0r@gmail.com
- Fork remote: https://github.com/suteny0r/oui-spy-unified-blue

### Never push to upstream origin
- This is a fork. `origin` points to the upstream repo (`colonelpanichacks/oui-spy-unified-blue`) which we do **not** have write access to.
- **ALWAYS** push to `fork` (e.g., `git push fork master`), **NEVER** to `origin`.
- When creating PRs, push the branch to `fork` first, then open the PR against the upstream.
