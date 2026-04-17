# OUI SPY — Audited Fork

Multi-mode surveillance detection and BLE intelligence firmware for the **Seeed Studio XIAO ESP32-S3**.

One device. Four firmware modes. Select from a boot menu, reboot, and go.

This is a friendly fork of [colonelpanichacks/oui-spy-unified-blue](https://github.com/colonelpanichacks/oui-spy-unified-blue) with a focused audit pass applied. Full list of fork-specific changes is at the bottom of this document.

> **⚠️ TESTING STATUS — PLEASE READ**
>
> The audit fixes and new features in this fork (distance estimation, triangulation, heatmap viewer, GPS mutex, XSS hardening, etc.) have been code-reviewed and compile-tested but **have not yet been thoroughly field-tested** against real Flock Safety, Raven, or other surveillance hardware in production environments. Distance estimates rely on an uncalibrated path-loss model that may be significantly off without per-device tuning. Triangulation requires ≥3 GPS-tagged observations and accuracy depends heavily on GPS quality and environmental conditions.
>
> **Use at your own discretion.** I will update this notice once field testing is complete. If you encounter issues, please [open an issue](https://github.com/churchja/oui-spy-unified-blue/issues) with details about your hardware, environment, and what you observed.

---

## Quick Connect

On first boot, connect to the selector AP to pick a firmware mode:

> **SSID:** `oui-spy` | **Password:** `ouispy123` | **Dashboard:** `192.168.4.1`

---

## Modes

### Mode 1: Detector

BLE alert tool that continuously scans for specific target devices by OUI prefix, MAC address, and device name patterns. When a match is found, the device triggers audible and visual alerts. Configurable target lists via web interface.

- AP: `snoopuntothem`
- Scans BLE advertisements against user-configured MAC/OUI watchlists
- NeoPixel + buzzer feedback on detection
- Web dashboard for managing targets and viewing scan results

### Mode 2: Foxhunter

RSSI-based proximity tracker for hunting down a specific BLE device. Lock onto a target MAC address, then follow the signal strength. The buzzer cadence increases as you get closer — like a Geiger counter for Bluetooth.

- AP: `foxhunter`
- Select target from live BLE scan or enter MAC manually
- Audio feedback rate scales inversely with distance
- Web interface for target selection and RSSI monitoring

### Mode 3: Flock-You

Detects Flock Safety surveillance cameras, Raven gunshot detectors, and related monitoring hardware using BLE-only heuristics. All detections are stored in memory and can be exported as JSON, CSV, or KML for later analysis.

**Detection methods:**

- **MAC prefix matching** — 20 known Flock Safety OUI prefixes (FS Ext Battery, Flock WiFi modules)
- **BLE device name patterns** — case-insensitive substring matching for `FS Ext Battery`, `Penguin`, `Flock`, `Pigvision`
- **BLE manufacturer company ID** — `0x09C8` (XUNTONG), associated with Flock Safety hardware. Catches devices even when no name is broadcast. *Sourced from [wgreenberg/flock-you](https://github.com/wgreenberg/flock-you).*
- **Raven service UUID matching** — identifies Raven gunshot detection units by their proprietary BLE GATT service UUIDs (GPS, power, network, upload, error)
- **Raven firmware version estimation** — determines approximate firmware version (1.1.x / 1.2.x / 1.3.x) based on which service UUIDs are advertised

**Features:**

- AP: `flockyou` / password: `flockyou123`
- Web dashboard at `192.168.4.1` with live detection feed, full pattern database browser, and export tools
- **Hardware GPS** (Seeed L76K on D6/D7) auto-detected; falls back to phone GPS if no NMEA data arrives
- **Phone GPS wardriving** — uses your phone's GPS via the browser Geolocation API to tag every detection with coordinates
- JSON, CSV, and KML export of all detections (MAC, name, RSSI, detection method, timestamps, count, Raven status, firmware version, GPS coordinates)
- JSON-formatted serial output (with GPS) for live ingestion by the companion Flask dashboard
- Thread-safe detection storage (up to 200 unique devices) with FreeRTOS mutex
- Atomic session persistence to SPIFFS with prev-session replay after reboot

**Enabling GPS (Android Chrome):**

The phone's GPS is used to geotag detections when no hardware GPS is attached. Because the dashboard is served over HTTP, Chrome requires a one-time flag change to allow location access:

1. Open a new Chrome tab and go to `chrome://flags`
2. Search for **"Insecure origins treated as secure"**
3. Add `http://192.168.4.1` to the text field
4. Set the flag to **Enabled**
5. Tap **Relaunch**

After relaunching, connect to the `flockyou` AP, open `192.168.4.1`, and tap the **GPS** card in the stats bar to grant location permission. Detections will be tagged with coordinates automatically.

> **Note:** iOS Safari does not support Geolocation over HTTP. GPS wardriving requires Android with Chrome, or the optional hardware GPS module.

### Mode 4: Sky Spy

Passive drone detection via FAA Remote ID (Open Drone ID) WiFi beacon monitoring. Listens in promiscuous mode for ASTM F3411 compliant broadcasts and extracts drone telemetry.

- Captures drone serial numbers, operator/UAV IDs
- Tracks location (lat/lon), altitude, ground speed, heading
- Parses ODID message types: Basic ID, Location, System, Operator ID, and Message Pack (0xF0) containers
- Real-time logging of all detected drones
- Dedicated FreeRTOS buzzer task for non-blocking audio alerts
- Thread-safe UAV array with LRU slot eviction under load

---

## WiFi Access Points

Each mode creates its own AP. When switching modes, **your phone/laptop will auto-reconnect to the last saved network**, which may be the wrong mode's AP. To avoid confusion:

- **Forget the previous mode's network** before switching, or
- **Disable auto-connect/auto-reconnect** for all OUI-SPY networks in your WiFi settings

| Mode | SSID | Password | Dashboard | Notes |
|------|------|----------|-----------|-------|
| **Boot Selector** | `oui-spy` | `ouispy123` | `192.168.4.1` | Configurable from selector UI, saved to NVS |
| **Detector** | `snoopuntothem` | `astheysnoopuntous` | `192.168.4.1` | Configurable from web dashboard, saved to NVS |
| **Foxhunter** | `foxhunter` | `foxhunter` | `192.168.4.1` | Fixed credentials |
| **Flock-You** | `flockyou` | `flockyou123` | `192.168.4.1` | Fixed credentials |
| **Sky Spy** | *none* | — | — | No AP — passive scanner, serial JSON output only |

> **Tip:** If you can't reach the dashboard after a mode switch, check which WiFi network you're connected to. Your device may have auto-joined a previously saved OUI-SPY AP from a different mode.

---

## Hardware

**Board:** Seeed Studio XIAO ESP32-S3

| Pin | Function |
|-----|----------|
| GPIO 3 | Piezo buzzer |
| GPIO 4 | NeoPixel (Flock-You detection indicator) |
| GPIO 21 | Onboard LED (inverted logic) |
| GPIO 43 (D6) | Optional hardware GPS TX (to L76K RX) |
| GPIO 44 (D7) | Optional hardware GPS RX (from L76K TX) |
| GPIO 0 | BOOT button (hold 1.5s to return to mode selector) |

---

## Boot Selector

On power-up, the device starts a WiFi access point (`oui-spy` / `ouispy123` by default) and serves a firmware selector at `192.168.4.1`. Pick a mode, the device stores the selection in NVS, and reboots into it.

- **Return to menu:** Hold the BOOT button for 1.5 seconds at any time
- **AP credentials:** Configurable SSID and password from the selector page, stored in NVS
- **Buzzer toggle:** Enable/disable the boot buzzer globally from the selector menu
- **MAC randomization:** Device MAC is randomized on every boot
- **Boot sounds:** Each mode plays its own distinct tone sequence on startup — modulated sweeps, retro melodies, and other piezo-buzzer tributes to let you know which firmware you're in before the screen is even up

---

## Flashing

Everything you need to flash a board is included in the repo. No PlatformIO or build tools required -- just Python and a USB cable.

### What You Need

- **Python 3.8 or newer** -- [download here](https://www.python.org/downloads/) if you don't have it
  - Windows: check **"Add Python to PATH"** during install
  - macOS: `brew install python3` or use the installer from python.org
  - Linux: `sudo apt install python3 python3-pip`
- **USB-C data cable** -- must be a data cable, not a charge-only cable
- **USB drivers** (usually not needed — XIAO ESP32-S3 uses native USB, VID 303A):
  - CH340/CH341: https://www.wch-ic.com/downloads/CH341SER_ZIP.html
  - CP210x: https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers

### Step 1: Install Dependencies

```bash
pip install -r requirements.txt
```

Or manually:

```bash
pip install "esptool>=5.0" pyserial
```

> **Note:** On some systems you may need to use `pip3` instead of `pip`, and `python3` instead of `python`. Also note that this fork requires esptool 5.0 or newer — the flasher uses v5 dashed-subcommand syntax.

### Step 2: Flash a Single Board

1. Plug in your XIAO ESP32-S3 via USB-C
2. Run:

```bash
python3 flash.py
```

3. The script auto-detects your board and the firmware
4. Type `y` and press Enter to confirm
5. Wait for "Done!" -- the board reboots automatically and plays a boot melody

### Step 3: Verify It Worked

After a successful flash, the board reboots automatically. Here's how to confirm it's working:

1. **Listen for 4 ascending beeps** -- this is the boot confirmation sound. If you hear it, the firmware is running.
2. On your phone or laptop, look for the WiFi network **`oui-spy`**
3. Connect with password **`ouispy123`**
4. Open **http://192.168.4.1** in your browser
5. You should see the mode selector dashboard

> **No beeps?** The board may not have flashed correctly. Try flashing again with `python3 flash.py --erase` to do a full erase first.

### Batch Mode (Multiple Boards)

For flashing many boards in a row. Fully hands-free -- no button presses or typing between boards.

```bash
python3 flash.py --batch
```

**How it works:**

1. The script starts and waits for a board
2. Plug in a board -- it is detected and flashed automatically
3. Wait for the board to reboot -- **listen for 4 ascending beeps**. That's your confirmation the flash was successful and the firmware is running.
4. Unplug the board and plug in the next one -- flashing starts automatically
5. Repeat until all boards are done
6. Press **Ctrl+C** to stop -- a summary of flashed/failed counts prints on exit

To erase flash completely before writing (clean slate, recommended when upgrading from older firmware):

```bash
python3 flash.py --batch --erase
```

### What Gets Flashed

The flasher writes all three binary files from the `firmware/` folder in one shot:

| File | Offset | Purpose |
|------|--------|---------|
| `bootloader.bin` | `0x0000` | ESP32-S3 bootloader |
| `partitions.bin` | `0x8000` | Partition table |
| `oui-spy-unified-blue.bin` | `0x10000` | Application firmware |

All three files must be present in the `firmware/` folder. The script will warn you if any are missing.

This is a single-app (factory) flash layout -- no OTA. If you flashed an older version of this firmware that used a 4-file layout with `boot_app0.bin`, run `python3 flash.py --erase` once to wipe the old partition table cleanly.

### All Options

```bash
python3 flash.py                        # flash one board (interactive)
python3 flash.py --erase                # full erase before flashing
python3 flash.py --batch                # batch mode: hands-free, auto-detect
python3 flash.py --batch --erase        # batch + erase (production runs)
python3 flash.py my_firmware.bin        # flash a specific .bin file
python3 flash.py --help                 # show help
```

### Troubleshooting

| Problem | Fix |
|---------|-----|
| `python: command not found` | Use `python3` instead of `python` |
| `esptool not found` or `unrecognized arguments: write-flash` | Run `pip install --upgrade -r requirements.txt`. This fork needs esptool 5.0+. |
| No port detected | Check USB cable is a data cable (not charge-only). Install drivers if needed. Try a different USB port. |
| Board doesn't boot after flash | Make sure all 3 `.bin` files are in `firmware/`. Try `python3 flash.py --erase` to do a full erase first. |
| Multiple serial devices detected | In single mode, the script lets you pick. In batch mode, it auto-selects the first native USB device and warns if multiple are found. Unplug other boards to avoid ambiguity. |
| Permission denied on serial port | Linux: `sudo usermod -a -G dialout $USER` then log out and back in. macOS: should work out of the box. |

### Building from Source

Only needed if you want to modify the firmware. Requires [PlatformIO](https://platformio.org/).

```bash
pio run                     # build
pio run -t upload           # flash directly
pio device monitor          # serial output (115200 baud)
```

The build output lands in `.pio/build/seeed_xiao_esp32s3/firmware.bin`. To use the flasher script instead, copy the build artifacts into `firmware/`:

```bash
mkdir -p firmware
cp .pio/build/seeed_xiao_esp32s3/bootloader.bin firmware/
cp .pio/build/seeed_xiao_esp32s3/partitions.bin firmware/
cp .pio/build/seeed_xiao_esp32s3/firmware.bin firmware/oui-spy-unified-blue.bin
```

**Build dependencies** (managed by PlatformIO):

- `NimBLE-Arduino` -- BLE scanning
- `ESP Async WebServer` + `AsyncTCP` -- web interfaces
- `ArduinoJson` v7 -- JSON serialization
- `Adafruit NeoPixel` -- LED control
- `TinyGPSPlus` -- hardware GPS NMEA parsing

**Flash layout:** Single-app (factory) partition table with ~6MB app + ~2MB SPIFFS data. See `partitions.csv`.

---

## Changes in this fork

This fork started from a straight clone of [colonelpanichacks/oui-spy-unified-blue](https://github.com/colonelpanichacks/oui-spy-unified-blue). Every change below came from walking the source file by file looking for race conditions, unchecked buffer reads, output-encoding gaps, and build-configuration drift.

### Correctness & safety

- **Flock-You Raven false-positive fix.** Detection no longer triggers on the standard Bluetooth SIG UUIDs (`0x180A` Device Information, `0x1809` Health Thermometer, `0x1819` Location & Navigation) — these appear on thousands of unrelated consumer BLE devices (watches, earbuds, fitness trackers). Only the proprietary Raven UUIDs in the `0x3100–0x3500` range trigger a Raven classification. The SIG UUIDs are still used as secondary evidence for firmware-version fingerprinting once a primary match is confirmed.
- **Sky-Spy bounds-safe WiFi callback.** The promiscuous-mode packet handler now length-guards every dereference of the radio payload. Malformed beacons or NAN frames from a hostile RF environment can no longer walk off the end of the payload buffer.
- **Sky-Spy Message Pack (0xF0) support.** Modern drones (most DJI) broadcast their Basic ID, Location, System, and Operator ID inside a single packed advertisement. Without pack handling, everything after the first message was silently dropped. Now decoded via `odid_message_process_pack()`.
- **Sky-Spy UAV array mutex.** BLE and WiFi callbacks both write to the same `uavs[]` array from different tasks; the wide struct copies and strncpy calls are now serialized by `uavsMutex`. LRU slot eviction replaces a bug where new drones silently clobbered slot 0 once the 8-slot array filled.
- **Detector dangling-pointer fix.** `DeviceInfo::matchedFilter` was a `const char*` pointing at a local `String`'s internal buffer — the pointer dangled the moment the caller returned. Changed to an owned `String`.
- **Detector devices-vector mutex.** BLE callback did `push_back` (which can reallocate and invalidate iterators) while loop-side code iterated the vector concurrently. Now serialized by `devicesMutex`, with the 600 ms beep moved outside the critical section so the lock stays short.
- **Detector double-beep fix.** `singleBeep()` was playing each beep twice — once as a PWM tone, once as a bit-banged square wave. Alerts took twice as long as intended and the second half was harsher. Removed the redundant second emission.
- **Flock-You GPS state mutex.** `fyGPSLat/Lon/Acc/Valid/LastUpdate` are written from the main loop and from the `/api/gps` handler (AsyncTCP task), read from the BLE callback task. On 32-bit Xtensa, a `double` is two write instructions — a concurrent read could tear a coordinate into two unrelated halves. `fyGPSMutex` + the new `fyGPSSnapshot()` atomic-read helper close that hole.
- **Flock-You atomic session save.** Session persistence now writes to `/session.tmp`, validates every write, and swaps into `/session.json` only on full success. Power loss mid-save no longer propagates corrupted JSON into `prev_session.json` on the next boot.

### Hardening

- **JSON output escaping (RFC 8259).** A new `fyJsonEscape()` helper handles `"`, `\`, `\b`, `\f`, `\n`, `\r`, `\t`, and emits `\u00XX` for other control characters. Applied on every path that emits a BLE name, method, MAC, or Raven firmware string to JSON — `/api/detections`, session save, and serial output.
- **CSV output escaping (RFC 4180).** New `fyCsvEscape()` quote-wraps fields and doubles embedded quotes. Applied to `/api/export/csv`.
- **Dashboard XSS escape.** A hostile BLE beacon advertising a name like `<script>alert(1)</script>` could previously execute in the browser of anyone loading the dashboard. The embedded JS now escapes `&<>"'` on every server-supplied string before injecting into `innerHTML`, and coerces numeric fields to numbers defensively.
- **Null-terminated `strncpy` throughout.** Every `strncpy` to a fixed-size buffer (`uav_id`, `op_id`, detection-record names) now explicitly null-terminates after the copy.

### Performance

- **Flock-You non-blocking BLE scan.** `fyBLEScan->start(duration, false)` was blocking the main loop for the full scan window (2 seconds), during which captive DNS stalled, GPS NMEA parsing backed up on UART1, and the NeoPixel animation stuttered. Switched to `start(duration, fyScanComplete, false)` with a completion callback — returns immediately.

### Build & infrastructure

- **Partition layout fix.** Original `partitions.csv` had an `otadata` partition but no second app slot, so OTA couldn't actually work. Switched to a single-app `factory` layout. Dropped `boot_app0.bin` from the flasher and instructions.
- **`platformio.ini` cleanup.** Removed `-mfix-esp32-psram-cache-issue` (a workaround for original-ESP32 silicon, no-op on S3) and `-DCONFIG_BT_NIMBLE_ENABLED=1` (Kconfig symbol, not a cppflag). Changed `board_build.filesystem = littlefs` → `spiffs` to match what the code actually calls at runtime.
- **`requirements.txt` bumped to esptool 5.0+.** The flasher has always used v5 dashed-subcommand syntax; the old `>=4.0` floor would silently install a 4.x version that failed with "unrecognized arguments".
- **ArduinoJson v7 API.** Replaced `containsKey()` (removed in v7) with `.isNull()` checks to match the pinned `@^7.0.4`.
- **`flash.py` fixes.** Added a `try/except KeyboardInterrupt` inside `batch_mode()` so the "flashed/failed" summary actually prints on Ctrl+C (previously unreachable). Added a warning when multiple native USB boards are detected in batch mode. Removed the `boot_app0.bin` write path.
- **HTML selector footer.** Said "Hold BOOT 2s" but `BOOT_HOLD_TIME = 1500`. Aligned to "1.5s" in the HTML, pin table, and documentation.

### New features

- **RSSI distance estimation.** Every BLE detection now carries an estimated distance in meters, computed via a log-distance path-loss model. Three fields per detection: `lastDistM` (current sighting), `minDistM` (closest approach this session), `maxDistM` (farthest observation). The dashboard shows `~Xm` and `min:Xm` on every detection card. Distance accuracy is ±50% to ±300% from a single reading — multiple readings from different positions are where triangulation adds real value.
- **Configurable path-loss model.** The path-loss exponent (`n`) and TX power reference (`txPower`) are adjustable from the dashboard's Tools → Radio Settings panel and persist in NVS across reboots. Defaults are `n=3.0` (urban/suburban) and `txPower=-59 dBm` (common iBeacon reference). Adjust `n` lower (toward 1.6) for open-air line-of-sight, higher (toward 4.5) for dense urban with obstructions.
- **Gauss-Newton triangulation.** When a MAC accumulates ≥3 GPS-tagged observations, the firmware estimates where the transmitter actually is using non-linear least-squares. Initial guess is the RSSI-weighted centroid; solver iterates in a local tangent plane with step damping. Observations older than 5 minutes are dropped to prevent moving emitters from averaging to a nonsensical midpoint. Results are available via `GET /api/locations` and in the dashboard's Tools → Triangulation panel.
- **Observation ring buffer.** Each of the 200 detection slots gets a 12-sample ring buffer of `(lat, lon, distance, rssi, timestamp)` tuples, allocated from PSRAM at boot (~77 KB). These feed the triangulation solver.
- **Standalone heatmap viewer (`heatmap.html`).** A single-file Leaflet-based map viewer in the repo root. Open it in any browser with internet, click Load JSON, pick your exported `flockyou_detections.json`. Renders RSSI-colored detection dots (green=close → red=far), dashed path lines connecting same-MAC detections in time order, and triangulated source pins. Gracefully degrades to a coordinate table when offline.
- **Updated exports.** JSON includes `dist_m`, `min_dist_m`, `max_dist_m`, and `obs` count per detection. CSV adds three distance columns. KML placemarks show distance info and a new "Estimated Source Locations" folder with distinct target-icon pins for triangulated positions.

---

## Acknowledgments

**colonelpanichacks** — author of the upstream [oui-spy-unified-blue](https://github.com/colonelpanichacks/oui-spy-unified-blue) and the broader OUI-SPY firmware ecosystem. This fork exists only because the upstream is solid enough to audit productively.

**Will Greenberg** ([@wgreenberg](https://github.com/wgreenberg)) — His [flock-you](https://github.com/wgreenberg/flock-you) fork contributed the BLE manufacturer company ID detection method (`0x09C8` XUNTONG) and structured pattern management approaches that inform the upstream detection architecture.

---

## OUI-SPY Firmware Ecosystem (upstream)

Each firmware is available as a standalone project:

| Firmware | Description | Board |
|----------|-------------|-------|
| **[OUI-SPY Unified](https://github.com/colonelpanichacks/oui-spy-unified-blue)** | Multi-mode BLE + WiFi detector (upstream of this fork) | ESP32-S3 / ESP32-C5 |
| **[OUI-SPY Detector](https://github.com/colonelpanichacks/ouispy-detector)** | Targeted BLE scanner with OUI filtering | ESP32-S3 |
| **[OUI-SPY Foxhunter](https://github.com/colonelpanichacks/ouispy-foxhunter)** | RSSI-based proximity tracker | ESP32-S3 |
| **[Flock You](https://github.com/colonelpanichacks/flock-you)** | Flock Safety / Raven surveillance detection | ESP32-S3 |
| **[Sky-Spy](https://github.com/colonelpanichacks/Sky-Spy)** | Drone Remote ID detection | ESP32-S3 / ESP32-C5 |
| **[Remote-ID-Spoofer](https://github.com/colonelpanichacks/Remote-ID-Spoofer)** | WiFi Remote ID spoofer & simulator with swarm mode | ESP32-S3 |
| **[OUI-SPY UniPwn](https://github.com/colonelpanichacks/Oui-Spy-UniPwn)** | Unitree robot exploitation system | ESP32-S3 |

---

## Disclaimer

This tool is intended for security research, privacy auditing, and educational purposes. Detecting the presence of surveillance hardware in public spaces is legal in most jurisdictions. Always comply with local laws regarding wireless scanning and signal interception. The authors are not responsible for misuse.
