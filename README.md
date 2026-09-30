# OUI SPY

Multi-mode surveillance detection and BLE intelligence firmware for the **Nesso N1** (primary target). Legacy **Seeed Studio XIAO ESP32-S3** support remains in the repo but is not built by default.

One device. Four firmware modes on Nesso (Detector, Foxhunter, Sky Spy, Mega_Maid). Select from a boot menu, reboot, and go.

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

### Mode 4: Flock-You (legacy — not in Nesso build)

> **Note:** Flock-You is retained in the repo for reference and XIAO builds, but is **excluded from the Nesso N1 firmware**. Use **Mega_Maid** (mode 6) for wardriving and MAC collection on Nesso.

Detects Flock Safety surveillance cameras, Raven gunshot detectors, and related monitoring hardware using BLE and WiFi promiscuous sniffing. All detections are stored in memory and can be exported as JSON or CSV for later analysis.

**Detection methods:**

- **MAC prefix matching** — 42 known Flock Safety OUI prefixes sourced from **OrdoOuroborous / [@NitekryDPaul](https://github.com/nitekry)**'s [nite-oui-collection](https://github.com/nitekry/nite-oui-collection) (41) plus Michael / DeFlockJoplin's drive-test addition `82:6b:f2` (1). All research credit to @NitekryDPaul for the underlying promiscuous-mode work; this firmware just consumes his list.
- **BLE device name patterns** — case-insensitive substring matching for `FS Ext Battery`, `Penguin`, `Flock`, `Pigvision`
- **BLE manufacturer company ID** — `0x09C8` (XUNTONG), associated with Flock Safety hardware. Catches devices even when no name is broadcast. *Sourced from [wgreenberg/flock-you](https://github.com/wgreenberg/flock-you).*
- **Raven service UUID matching** — identifies Raven gunshot detection units by their BLE GATT service UUIDs (device info, GPS, power, network, upload, error, legacy health/location services)
- **Raven firmware version estimation** — determines approximate firmware version (1.1.x / 1.2.x / 1.3.x) based on which service UUIDs are advertised

**Features:**

- Collect mode: 50/50 BLE + WiFi promiscuous time-sliced scan (no AP)
- Dashboard mode: double-click KEY1 for time-sliced AP + web export
- Web dashboard at `192.168.4.1` with live detection feed, full pattern database browser, and export tools
- **GPS wardriving** — on Nesso with an [M5 Unit GPS v1.1](docs/hardware/m5stack-unit-gps-v1.1.md) on the Grove port, detections use the module fix automatically; otherwise uses your phone's GPS via the browser Geolocation API
- JSON and CSV export of all detections (MAC, name, RSSI, detection method, timestamps, count, Raven status, firmware version, GPS coordinates)
- Detection `first` / `last` (and CSV `first_seen_ms` / `last_seen_ms`) are **UTC epoch milliseconds** when the GPS module has synced time this session; `0` before the first GPS time fix. Internal capture uses boot uptime and converts retroactively once GPS UTC is available.
- JSON-formatted serial output (with GPS) for ingestion by external post-capture analysis tools
- Thread-safe detection storage (up to 200 unique devices) with FreeRTOS mutex

**Enabling GPS (Android Chrome):**

The phone's GPS is used to geotag detections. Because the dashboard is served over HTTP, Chrome requires a one-time flag change to allow location access:

1. Open a new Chrome tab and go to `chrome://flags`
2. Search for **"Insecure origins treated as secure"**
3. Add `http://192.168.4.1` to the text field
4. Set the flag to **Enabled**
5. Tap **Relaunch**

After relaunching, connect to the mode AP (`megamaid` on Nesso), open `192.168.4.1`, and tap the **GPS** card in the stats bar to grant location permission. Detections will be tagged with coordinates automatically.

> **Note:** iOS Safari does not support Geolocation over HTTP. GPS wardriving requires Android with Chrome.

### Mode 5: Sky Spy

Passive drone detection via FAA Remote ID (Open Drone ID) WiFi beacon monitoring. Listens in promiscuous mode for ASTM F3411 compliant broadcasts and extracts drone telemetry.

- Captures drone serial numbers, operator/UAV IDs
- Tracks location (lat/lon), altitude, ground speed, heading
- Parses all ODID message types: Basic ID, Location, Authentication, Self-ID, System, Operator ID
- Real-time logging of all detected drones
- Dedicated FreeRTOS buzzer task for non-blocking audio alerts

### Mode 6: Mega_Maid

Passive BLE + WiFi MAC/OUI collector — records every seen BLE advertisement and WiFi unicast MAC address with no watchlist filtering.

**Features:**

- **Dashboard mode** (default on boot): softAP `megamaid` / `megamaid123` at `192.168.4.1` for web export only — no active scanning (ESP32-C6 single radio)
- **Collect mode** (double-click KEY1 from dashboard): AP off, 50/50 BLE + WiFi promiscuous time-sliced scan across **WiFi channels 1–14**
- Double-click KEY1 again when done collecting to return to dashboard, save session, and export JSON/CSV/KML
- Web dashboard with live feed, unique OUI summary, and export tools
- GPS wardriving — hardware GNSS on Nesso (Grove) when fitted; otherwise phone browser Geolocation API (Chrome flag workaround below)
- Timestamps (`first` / `last`) use GPS UTC epoch ms when synced; see Flock-You notes above
- Beeps on each new unique MAC address
- Up to 250 unique devices in RAM on Nesso N1 (500 on other boards)
- **Export:** current session downloads from live RAM; prior session downloads from SPIFFS (PREV tab / history endpoints)
- **Multi-location GPS:** moving APs/clients can record **`gps_sights`** (array of `{ lat, lon, acc, t }`) alongside the latest `gps` fix; import JSON into the detections platform for map movement tracks

**Workflow:** boot → dashboard → KEY1 x2 → collect (walk/drive) → KEY1 x2 → dashboard → connect phone → export

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
| **Flock-You** | `flockyou` | `flockyou123` | `192.168.4.1` | Legacy / XIAO only — not in Nesso build |
| **Sky Spy** | *none* | — | — | No AP — passive scanner, serial JSON output only |
| **Mega_Maid** | `megamaid` | `megamaid123` | `192.168.4.1` | Dashboard for export; Collect (KEY1 x2) turns AP off |

> **Tip:** If you can't reach the dashboard after a mode switch, check which WiFi network you're connected to. Your device may have auto-joined a previously saved OUI-SPY AP from a different mode.

---

## Hardware

### Seeed Studio XIAO ESP32-S3 (original target)

| Pin | Function |
|-----|----------|
| GPIO 3 | Piezo buzzer |
| GPIO 21 | NeoPixel LED |
| GPIO 0 | BOOT button (hold 2s to return to mode selector) |

### Arduino Nesso N1 (ESP32-C6 port)

Build from `oui_spy_nesso/` with `arduino-cli`. **Use the project build script** so the patched vendored `AsyncTCP` is linked (required for the Flock-You web dashboard on ESP32-C6):

```bash
cd oui_spy_nesso
./scripts/compile-nesso.sh            # compile only
./scripts/compile-nesso.sh --upload   # compile + flash (port auto-detected)
```

The board's serial port is detected automatically (Espressif USB VID `0x303A`); pass one explicitly only if you have several ESP32s attached: `--upload /dev/cu.usbmodemXXXX`.

The script creates `.arduino-cli-user/` (symlinks to your global libs) and links the patched `AsyncTCP` + `ESP_Async_WebServer` from `lib/async_web/` — the same copies the PlatformIO build uses. Plain `arduino-cli compile` may pick up the old `~/Documents/Arduino/libraries/AsyncTCP@1.1.4` and crash when starting the web server.

**Single source of truth:** the firmware lives in `src/` only. `compile-nesso.sh` mirrors `src/` into the sketch dir at build time, so `oui_spy_nesso/*.cpp` and `oui_spy_nesso/raw/` are *generated and git-ignored* — **edit `src/`, never the sketch copies.** After a build, the verifier confirms the patched AsyncTCP was actually linked. See also `oui_spy_nesso/arduino-cli-nesso.yaml`.

| Control | Location | Function |
|---------|----------|----------|
| **KEY1** | **Front** (face, near display) | Hold ~1.5s → mode selector |
| **KEY2** | **Side** (edge) | Reserved (not used by firmware yet) |
| **Power/reset** | Side (separate from KEY1/KEY2) | Single press = reboot; long press = bootloader |
| Display | Front | Mode name, battery, hits/RSSI, alerts |
| Buzzer | Internal | Alert tones (mode-specific) |

**Optional GNSS (M5 Unit GPS v1.1):** Connect to the **HY2.0-4P Grove** port with the unit’s included cable. Firmware powers the Grove 5 V rail and reads NMEA at **115200** on UART1 (see [docs/hardware/m5stack-unit-gps-v1.1.md](docs/hardware/m5stack-unit-gps-v1.1.md)). **Flock-You** and **Mega_Maid** prefer the module’s fix for geotagging; without a fix, Android Chrome phone GPS still works as before.

Default boot mode on Nesso: **Flock-You**. Hold **KEY1 (front)** during power-on to force the web mode selector instead.

---

## Boot Selector

On power-up, the device starts a WiFi access point (`oui-spy` / `ouispy123` by default) and serves a firmware selector at `192.168.4.1`. Pick a mode, the device stores the selection in NVS, and reboots into it.

- **Return to menu:** Hold the BOOT button for 2 seconds at any time
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
- **USB drivers** (if your OS doesn't auto-detect the board):
  - CH340/CH341: https://www.wch-ic.com/downloads/CH341SER_ZIP.html
  - CP210x: https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers

### Step 1: Install Dependencies

```bash
pip install -r requirements.txt
```

Or manually:

```bash
pip install esptool pyserial
```

> **Note:** On some systems you may need to use `pip3` instead of `pip`, and `python3` instead of `python`.

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
6. Press **Ctrl+C** to stop

The script never times out. It will wait as long as needed for the next board. It also tracks how many boards were flashed successfully vs. failed.

> **Quick test cycle:** Plug in -> auto-flash -> hear 4 beeps -> unplug -> next board. That's it.

To erase flash completely before writing (clean slate, recommended for first-time flash):

```bash
python3 flash.py --batch --erase
```

### What Gets Flashed

The flasher writes all four binary files from the `firmware/` folder in one shot:

| File | Offset | Purpose |
|------|--------|---------|
| `bootloader.bin` | `0x0000` | ESP32-S3 bootloader |
| `partitions.bin` | `0x8000` | Partition table |
| `boot_app0.bin` | `0xe000` | OTA data partition |
| `oui-spy-unified-blue.bin` | `0x10000` | Application firmware |

All four files must be present in the `firmware/` folder. The script will warn you if any are missing.

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
| `esptool not found` | Run `pip install esptool pyserial` (or `pip3`) |
| No port detected | Check USB cable is a data cable (not charge-only). Install CH340/CP210x drivers. Try a different USB port. |
| Board doesn't boot after flash | Make sure all 4 `.bin` files are in `firmware/`. Try `python3 flash.py --erase` to do a full erase first. |
| Multiple serial devices detected | In single mode, the script lets you pick. In batch mode, it auto-selects. Unplug other USB serial devices if you get unexpected behavior. |
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
cp .pio/build/seeed_xiao_esp32s3/bootloader.bin firmware/
cp .pio/build/seeed_xiao_esp32s3/partitions.bin firmware/
cp ~/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin firmware/
cp .pio/build/seeed_xiao_esp32s3/firmware.bin firmware/oui-spy-unified-blue.bin
```

**Build dependencies** (managed by PlatformIO):

- `NimBLE-Arduino` -- BLE scanning
- `ESP Async WebServer` + `AsyncTCP` -- web interfaces
- `ArduinoJson` -- JSON serialization
- `Adafruit NeoPixel` -- LED control

**Flash layout:** Custom partition table with ~6MB app + ~2MB LittleFS data. See `partitions.csv`.

---

## Acknowledgments

**Will Greenberg** ([@wgreenberg](https://github.com/wgreenberg)) — His [flock-you](https://github.com/wgreenberg/flock-you) fork was instrumental in improving the Flock Safety detection heuristics. The BLE manufacturer company ID detection method (`0x09C8` XUNTONG) was sourced directly from his work, along with structured pattern management approaches that informed the detection architecture. Thank you for the research and for making it open.

---

## OUI-SPY Firmware Ecosystem

Each firmware is available as a standalone project:

| Firmware | Description | Board |
|----------|-------------|-------|
| **[OUI-SPY Unified](https://github.com/colonelpanichacks/oui-spy-unified-blue)** | Multi-mode BLE + WiFi detector (this project) | ESP32-S3 / ESP32-C5 |
| **[OUI-SPY Detector](https://github.com/colonelpanichacks/ouispy-detector)** | Targeted BLE scanner with OUI filtering | ESP32-S3 |
| **[OUI-SPY Foxhunter](https://github.com/colonelpanichacks/ouispy-foxhunter)** | RSSI-based proximity tracker | ESP32-S3 |
| **[Flock You](https://github.com/colonelpanichacks/flock-you)** | Flock Safety / Raven surveillance detection | ESP32-S3 |
| **[Sky-Spy](https://github.com/colonelpanichacks/Sky-Spy)** | Drone Remote ID detection | ESP32-S3 / ESP32-C5 |
| **[Remote-ID-Spoofer](https://github.com/colonelpanichacks/Remote-ID-Spoofer)** | WiFi Remote ID spoofer & simulator with swarm mode | ESP32-S3 |
| **[OUI-SPY UniPwn](https://github.com/colonelpanichacks/Oui-Spy-UniPwn)** | Unitree robot exploitation system | ESP32-S3 |

---

## Author

**colonelpanichacks**

---

## Disclaimer

This tool is intended for security research, privacy auditing, and educational purposes. Detecting the presence of surveillance hardware in public spaces is legal in most jurisdictions. Always comply with local laws regarding wireless scanning and signal interception. The authors are not responsible for misuse.
