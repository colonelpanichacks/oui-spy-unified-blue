# OUI SPY

Multi-mode surveillance detection and BLE intelligence firmware for the **Seeed Studio XIAO ESP32-S3**.

One device. Four firmware modes. Select from a boot menu, reboot, and go.

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

Detects Flock Safety surveillance cameras, Raven gunshot detectors, and related monitoring hardware using BLE-only heuristics. All detections are stored in memory and can be exported as JSON or CSV for later analysis.

**Detection methods:**

- **MAC prefix matching** — 20 known Flock Safety OUI prefixes (FS Ext Battery, Flock WiFi modules)
- **BLE device name patterns** — case-insensitive substring matching for `FS Ext Battery`, `Penguin`, `Flock`, `Pigvision`
- **BLE manufacturer company ID** — `0x09C8` (XUNTONG), associated with Flock Safety hardware. Catches devices even when no name is broadcast. *Sourced from [wgreenberg/flock-you](https://github.com/wgreenberg/flock-you).*
- **Raven service UUID matching** — identifies Raven gunshot detection units by their BLE GATT service UUIDs (device info, GPS, power, network, upload, error, legacy health/location services)
- **Raven firmware version estimation** — determines approximate firmware version (1.1.x / 1.2.x / 1.3.x) based on which service UUIDs are advertised

**Features:**

- AP: `flockyou` / password: `flockyou123`
- Web dashboard at `192.168.4.1` with live detection feed, full pattern database browser, and export tools
- **GPS wardriving** — uses your phone's GPS via the browser Geolocation API to tag every detection with coordinates
- JSON and CSV export of all detections (MAC, name, RSSI, detection method, timestamps, count, Raven status, firmware version, GPS coordinates)
- JSON-formatted serial output (with GPS) for live ingestion by the companion Flask dashboard
- Thread-safe detection storage (up to 200 unique devices) with FreeRTOS mutex

**Enabling GPS (Android Chrome):**

The phone's GPS is used to geotag detections. Because the dashboard is served over HTTP, Chrome requires a one-time flag change to allow location access:

1. Open a new Chrome tab and go to `chrome://flags`
2. Search for **"Insecure origins treated as secure"**
3. Add `http://192.168.4.1` to the text field
4. Set the flag to **Enabled**
5. Tap **Relaunch**

After relaunching, connect to the `flockyou` AP, open `192.168.4.1`, and tap the **GPS** card in the stats bar to grant location permission. Detections will be tagged with coordinates automatically.

> **Note:** iOS Safari does not support Geolocation over HTTP. GPS wardriving requires Android with Chrome.

### Mode 5: Sky Spy

Passive drone detection via FAA Remote ID (Open Drone ID). Listens in promiscuous mode for ASTM F3411 compliant broadcasts over WiFi and BLE, extracting drone telemetry in real time.

- **Dual-band support:** ESP32-C5 scans both 2.4GHz and 5GHz WiFi channels with fast channel hopping (~30ms dwell), plus BLE. ESP32-S3 scans 2.4GHz + BLE (single-band)
- Captures drone serial numbers, operator/UAV IDs
- Tracks location (lat/lon), altitude, ground speed, heading
- Parses all ODID message types: Basic ID, Location, Authentication, Self-ID, System, Operator ID
- JSON serial output includes detection band (`2.4GHz`, `5GHz`, `BLE`) and WiFi channel
- Board-auto-detect: pin mapping and LED logic adapt automatically for C5 vs S3
- Real-time logging of all detected drones
- Dedicated FreeRTOS buzzer task for non-blocking audio alerts

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

**Primary board:** Seeed Studio XIAO ESP32-S3 (2.4GHz WiFi + BLE)
**Dual-band board:** Seeed Studio XIAO ESP32-C5 (2.4GHz + 5GHz WiFi 6 + BLE)

| Pin | ESP32-S3 | ESP32-C5 | Function |
|-----|----------|----------|----------|
| D2 | GPIO 3 | GPIO 25 | Piezo buzzer |
| LED | GPIO 21 (inverted) | GPIO 27 | Status LED |
| BOOT | GPIO 0 | GPIO 28 | Hold 2s to return to mode selector |

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

### Quick Flash (no PlatformIO needed)

Just Python, a USB cable, and a `.bin` file.

```bash
pip install esptool pyserial
```

Drop your compiled firmware into the `firmware/` folder, plug in the XIAO ESP32-S3, and run:

```bash
python flash.py
```

The script auto-detects your board and flashes it. Done.

**Options:**

```bash
python flash.py                        # auto-detect bin from firmware/ folder
python flash.py my_firmware.bin        # flash a specific file
python flash.py --erase                # full erase before flashing
python flash.py my_firmware.bin --erase
```

### Building from Source

Requires [PlatformIO](https://platformio.org/).

```bash
pio run                     # build
pio run -t upload           # flash directly
pio device monitor          # serial output (115200 baud)
```

The build output lands in `.pio/build/seeed_xiao_esp32s3/firmware.bin` — copy that into `firmware/` if you want to use the flasher script instead.

**Dependencies** (managed by PlatformIO):

- `NimBLE-Arduino` — BLE scanning
- `ESP Async WebServer` + `AsyncTCP` — web interfaces
- `ArduinoJson` — JSON serialization
- `Adafruit NeoPixel` — LED control

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
