# OUI SPY

Multi-mode surveillance detection and BLE intelligence firmware for the **Seeed Studio XIAO ESP32-S3**.

One device. Seven firmware modes. Select from a boot menu, reboot, and go.

---

## Web Flasher

Flash straight from the browser — no Python, no PlatformIO, no drivers to think about:

**https://colonelpanichacks.github.io/oui-spy-unified-blue/**

Chrome, Edge, or Opera on desktop (Web Serial API). Plug in the XIAO ESP32-S3 with a USB-C data cable, click **Connect & Flash**, pick the serial port. The page always serves the latest firmware committed to `master`.

---

## Quick Connect

On first boot, connect to the selector AP to pick a firmware mode:

> **SSID:** `oui-spy` | **Password:** `ouispy123` | **Dashboard:** `192.168.4.1`

---

## Modes

### Mode 1: Detector

BLE alert tool that continuously scans for specific target devices by OUI prefix, MAC address, and device name patterns. When a match is found, the device triggers audible and visual alerts. Configurable target lists via web interface.

- AP: `snoopuntothem` / `astheysnoopuntous`
- Scans BLE advertisements against user-configured watchlists
- NeoPixel + buzzer feedback on detection
- Web dashboard for managing targets and viewing scan results

**Five signature classes.** OUI prefix and full MAC are typed into the config
boxes. Company ID, 16-bit service UUID, and device-name substring cannot be
expressed as text and are installed from the OUI Database.

**OUI Database.** A browsable list of known surveillance hardware — RING,
AXON, FLOCK SAFETY, DJI, PARROT, SKYDIO — each with prefixes, category, and
typical devices. AXON carries more than OUIs, so its button reads **+ Add
all signatures**:

| Vendor | Signatures |
|---|---|
| **AXON** | OUI `00:25:DF`, company ID `0x034D` (TASER International), service UUID `0xFC81` |

OUIs alone are the weakest signal — Axon hardware may never expose its OUI
in an advertisement, so the company ID and service UUID do most of the work.
Each added vendor renders one colour-coded line under the OUI box with an
`x` to remove. Manual OUI entry is unaffected.

**Meta / Ray-Ban detection.** No OUI-Database preset. The glasses use RPA
(rotating random MAC per BT spec), so OUI-based matching is pure noise, and
CID-only or svc-UUID-only auto-installers were false-positive magnets.
Detection is instead handled by a hardcoded composite matcher that runs on
every advert regardless of user filter config and fires only when either:
mfr company ID `0x0D53` (Luxottica) AND service UUID `0xFD5F` (Meta) are
present in the same advert, or the complete local name contains `Ray-Ban`,
`Wayfarer`, or `Oakley Meta`. Hits render with a red-pink `META` badge and
are logged with `match_method: "meta_composite"`. Manually adding `0x0D53`,
`0xFD5F`, or a Luxottica MAC via the target config UI still triggers via the
normal filter path with its normal badge.

**Burn-in is reversible.** Locking the config disables the AP permanently, but
holding BOOT during power-on clears the lock and restores config mode. Older
firmware claimed a reflash would unlock it — it does not, NVS survives a
reflash.

### Mode 2: Foxhunter

RSSI-based proximity tracker for hunting down a specific BLE device. Lock onto a target MAC address, then follow the signal strength. The buzzer cadence increases as you get closer — like a Geiger counter for Bluetooth.

- AP: `foxhunter`
- Select target from live BLE scan or enter MAC manually
- Audio feedback rate scales inversely with distance
- Web interface for target selection and RSSI monitoring

### Mode 3: Flock-You — Promiscuous WiFi Edition

Passive 2.4 GHz promiscuous-mode detector for Flock Safety surveillance infrastructure. No AP, no transmit — the radio stays dedicated to sniffing while the device hops channels 1 / 6 / 11 at 350 ms dwell. Detections beep, flash, persist to SPIFFS with a CRC envelope, and stream over USB-CDC for live ingestion by the Flask dashboard at [colonelpanichacks/flock-you](https://github.com/colonelpanichacks/flock-you).

This is a port of the `promiscious` branch of `flock-you` — see that repo for the full research write-up and the Flask side.

**Detection methods (WiFi only):**

- **addr2 OUI match** — transmitter-side match against the 39-OUI Flock Safety list (29 from @NitekryDPaul's original promiscuous-mode set, 10 from his April 2026 additions — two of the original 12 April adds, `94:2a:6f` and `f4:e2:c6`, were demoted as Ubiquiti false positives per his June 2026 update). All work of **OrdoOuroborous / [@NitekryDPaul](https://github.com/nitekry)**.
- **addr1 OUI match** — the receiver-side technique: catches Flock STAs that appear only as the destination of probe responses or data frames during their burst-sleep windows. Mandatory multicast + locally-administered guards before the match. @NitekryDPaul's discovery.
- **Wildcard probe signature** — Probe Request (type=0 subtype=4) + zero-length SSID IE + known-OUI addr2. The DeFlockJoplin high-precision signature (Joplin drive-test: 11/12 cameras caught with 2 false positives). Suppresses the broad addr2 alert on the same frame to avoid double-counting.

**Features:**

- No AP — radio is dedicated to promiscuous sniffing
- Audible alerts: two-chirp on new MAC, monotone heartbeat while target stays in range
- SPIFFS-persisted session with atomic CRC envelope; previous-boot data is preserved
- USB-CDC command protocol so the host can pull stored detections without re-flashing:
  - `CMD:DUMP_PREV` — streams the previous session from `/prev_session.json` as replay-flagged JSON
  - `CMD:DUMP_LIVE` — streams the in-RAM detection table
  - `CMD:STATUS` / `CMD:VERSION` — device telemetry and firmware identifier
  - `CMD:CLEAR_PREV` / `CMD:CLEAR_LIVE` — wipe persisted or in-memory state
- Flask-compatible JSON line per detection on Serial (same schema as the BLE companion)

**Companion dashboard:** `api/flockyou.py` in [colonelpanichacks/flock-you](https://github.com/colonelpanichacks/flock-you) exposes the command protocol as REST endpoints — `/api/flock/{status,version,dump_prev,dump_live,clear_prev,clear_live}` — and surfaces them in the web UI as a five-button Sniffer command bar (**Pull Prev**, **Pull Live**, **Status**, **Clear Prev**, **Clear Live**) that appears once the device is connected.

Replayed detections show up in the detection list with a purple **FLASH** badge (from SPIFFS) or blue **RAM** badge (from the in-memory table), a tinted left border, and `timestamp_source: device_replay`. They don't get GPS temporal matching (the device's stored entries only have monotonic millis, not wall-clock) and never overwrite a fresher live entry for the same MAC. Every command response surfaces as a coloured top-right toast.

The canonical "plug device back in after wardriving" workflow from a terminal:

```bash
curl -X POST http://localhost:5000/api/flock/dump_prev
curl -X POST http://localhost:5000/api/flock/clear_prev
```

Full dashboard docs (endpoints, socket events, JSON wire formats, GPS setup, persistence layout, troubleshooting): [colonelpanichacks/flock-you/api/README.md](https://github.com/colonelpanichacks/flock-you/blob/promiscious/api/README.md).

### Mode 4: PCAP

Passive WiFi packet capture. Fills the slot vacated by the retired Flock-You BLE mode. Hosts a live web dashboard on `ouispy-pcap` / `packetsniffer` and keeps a rolling 2 MB in-PSRAM session pcap (linktype 127, IEEE 802.11 with radiotap) that the browser can download at any time via **Save PCAP**.

- AP: `ouispy-pcap` / `packetsniffer` (configurable from the dashboard, stored in the mode's own NVS namespace `pcap-mode`)
- Dashboard at `http://192.168.4.1` — live packet table, vendor colouring (RING / AXON / FLOCK / DJI / PARROT / SKYDIO / META), chip filters, per-frame CSV snapshot, and one-click session PCAP download
- Two channel modes: **locked** (AP + one channel) or **hop** (STA, no AP, cycles a user-selected 2.4 GHz channel mask with configurable dwell)
- USB-CDC output: human-readable one-line summaries per frame (scriptable)
- USB-CDC command protocol: `CMD:STATUS`, `CMD:VERSION`, `CMD:CHAN <n>`, `CMD:HOP 0x0422`, `CMD:DWELL <ms>`
- The USB PCAP binary streaming path (previously extcap / pipe helpers) has been removed — ESP32-S3 Arduino USB CDC is not reliable for high-rate binary streaming. Use the dashboard **Save PCAP** button instead; the download parses cleanly in Wireshark regardless of capture rate.

### Mode 5: Sky Spy

> Numbering note: Sky Spy keeps its original number 5 (retained after the
> Flock-You BLE mode was retired) so devices with a mode already saved to
> NVS keep booting into the mode they were set to. Mode 4 is now PCAP.

Passive drone detection via FAA Remote ID (Open Drone ID) WiFi beacon monitoring. Listens in promiscuous mode for ASTM F3411 compliant broadcasts and extracts drone telemetry.

- Captures drone serial numbers, operator/UAV IDs
- Tracks location (lat/lon), altitude, ground speed, heading
- Parses all ODID message types: Basic ID, Location, Authentication, Self-ID, System, Operator ID
- Real-time logging of all detected drones
- Dedicated FreeRTOS buzzer task for non-blocking audio alerts

### Mode 6: BLE Sniff

Passive BLE advertising capture. Listens on the three BLE advertising channels (37 / 38 / 39) via NimBLE, hosts a live web dashboard on `ouispy-blesniff` / `sniffuntothem`, and keeps a rolling 2 MB in-PSRAM session pcap (`LINKTYPE_BLUETOOTH_LE_LL_WITH_PHDR`, linktype 256) that the browser can download at any time via **Save PCAP**.

**Scope:** advertisements only. This mode does not capture connected-device (LL data channel) traffic — it observes the same broadcasts a phone sees while scanning, no more.

- AP: `ouispy-blesniff` / `sniffuntothem` (configurable from the dashboard, stored in the mode's own NVS namespace `blesniff`)
- Dashboard at `http://192.168.4.1` — live packet table, vendor chips (RING / AXON / FLOCK / DJI / PARROT / SKYDIO / META), advert-type + address-type + trait chip filters, per-frame CSV snapshot, and one-click session PCAP download
- USB-CDC output: human-readable one-line summaries per advert (scriptable)
- Passive receive only — `setActiveScan(false)` means the radio never transmits a `SCAN_REQ`
- Scan defaults: `window=30ms`, `interval=100ms` (leaves ~70% of the radio for WiFi coexistence so the AP stays reachable while scanning)
- USB-CDC command protocol: `CMD:STATUS`, `CMD:VERSION`, `CMD:WINDOW <ms>`, `CMD:INTERVAL <ms>`
- Vendor identify against the OUI Database (same list surfaced by PCAP and Detector)
- The USB PCAP binary streaming path (previously extcap / pipe helpers) has been removed — ESP32-S3 Arduino USB CDC is not reliable for high-rate binary streaming. Use the dashboard **Save PCAP** button instead; the download parses cleanly in Wireshark regardless of capture rate.

### Mode 7: Visual Scout

Multi-sensor Flock camera detection that catches both RF-transmitting cameras (via GPS + DeFlock database) and RF-silent cameras (via MJPEG stream + Raspberry Pi 5 + Google Coral TPU visual inference). A bridge between passive RF detection and active computer vision.

**Hardware requirements:**
- GPS module (9600 baud NMEA) on GPIO 44 (RX) / 43 (TX)
- OV2640 camera (via standard ESP32-S3 WROOM FPC breakout — adjust `CAM_PIN_*` in firmware for your PCB)
- Raspberry Pi 5 (optional for visual detection; operates autonomously without Pi if only GPS proximity alerts are desired)
- Google Coral Edge TPU (USB or M.2 via Pi HAT — optional, falls back to CPU YOLOv8)

**Detection methods:**

1. **GPS proximity alerting** — Haversine distance check every 2 seconds against downloaded Flock camera locations from the DeFlock database (~20–30K cameras filtered from ~117K total ALPR nodes). Buzzer + LED alert within configurable radius (default 150 m).

2. **Visual detection** — MJPEG stream from ESP32 OV2640 flows to a Raspberry Pi running YOLOv8 (CPU or Coral TPU accelerated). Every detection is saved with GPS metadata and frame-level annotations to the Pi's NVMe SSD as training data. Supports crowd-source uploads to a central server for collaborative model retraining.

**Deflock database sync:**
- **Preferred path:** Pi proxy (`pi/deflock_downloader.py`) — downloads full camera list from `data.dontgetflocked.com`, filters to Flock Safety only, serves compact binary at `http://<pi>:5000/flock_cameras.bin` for the ESP32 to fetch (fastest, ~30 KB payload)
- **Fallback:** ESP32 fetches CDN tiles directly from `cdn.deflock.me` (slower, on-device filtering to Flock Safety brand)

**Features:**

- AP: `ouispy-scout` (open by default; set password via web UI if desired)
- Dashboard at `http://192.168.4.1` — configure Pi proxy URL, alert radius, WiFi STA credentials for direct CDN sync, view GPS status, trigger manual DB sync
- Camera lens profile toggle in dashboard: **IR-cut** (default daylight color) vs **No IR-cut** (night/NIR flash visibility profile)
- MJPEG stream on port 81 (`http://192.168.4.1:81/stream`) — real-time camera feed for the Pi consumer via `cv2.VideoCapture` or `requests` streaming
- GPS endpoint at `http://192.168.4.1:81/gps` — JSON current position for synchronization with visual detections
- Compact binary DB in LittleFS: `uint32 camera count` + `N × {float32 lat, float32 lon}`
- Dedicated FreeRTOS task for MJPEG streaming (core 0), GPS parse on loop (core 1)

**Raspberry Pi pipeline (`pi/`):**

- **`pi/deflock_downloader.py`** — HTTP service on port 5000. Downloads from `data.dontgetflocked.com/cameras-us.json` (primary, fastest), falls back to `data.dontgetflocked.com/cameras.geojson.gz` or `cdn.deflock.me` tiles. Re-syncs every 6 hours. Serves `/flock_cameras.bin` (compact binary), `/flock_cameras.geojson` (for QGIS), `/status` (JSON), and accepts `POST /sync` for manual trigger.

- **`pi/flock_detector.py`** — MJPEG stream consumer. Runs YOLOv8 inference (CPU baseline) or Coral Edge TPU compiled `.tflite` model via `pycoral`. Saves annotated frames + JSON metadata sidecars to NVMe SSD (`/mnt/ssd/training_data/YYYYMMDD/`). Optional background upload thread for crowd-source training data servers.

- **`pi/SETUP.md`** — Full guide: NVMe mount, Coral Edge TPU runtime installation, `edgetpu_compiler` model conversion, systemd service setup.

**Workflow:**
```
Driving
  ├─ ESP32 GPS alert on known Flock cameras (RF-silent or RF-on)
  └─ ESP32 MJPEG → Pi → Coral TPU detection of new/unknown cameras
                        ├─ Save frames + GPS to SSD
                        ├─ Optional: upload to crowd-source server
                        └─ Retrain shared model
```

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
| **Flock-You WiFi** | *none* | — | — | No AP — radio is dedicated to promiscuous sniffing; talk to it via USB-CDC commands and the Flask dashboard |
| **PCAP** | `ouispy-pcap` | `packetsniffer` | `192.168.4.1` | Configurable from mode dashboard, saved to NVS. Hop mode disables the AP — radio is dedicated to sniffing; use USB-CDC then |
| **Sky Spy** | *none* | — | — | No AP — passive scanner, serial JSON output only |
| **BLE Sniff** | `ouispy-blesniff` | `sniffuntothem` | `192.168.4.1` | Configurable from mode dashboard, saved to NVS |
| **Visual Scout** | `ouispy-scout` | *(open)* | `192.168.4.1` | GPS + Deflock DB proximity alerts + MJPEG camera stream for Pi/Coral; set password & configure Pi proxy URL from dashboard |

> **Tip:** If you can't reach the dashboard after a mode switch, check which WiFi network you're connected to. Your device may have auto-joined a previously saved OUI-SPY AP from a different mode.

---

## Hardware

**Board:** Seeed Studio XIAO ESP32-S3

| Pin | Function |
|-----|----------|
| GPIO 3 | Piezo buzzer |
| GPIO 21 | NeoPixel LED |
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

**Will Greenberg** ([@wgreenberg](https://github.com/wgreenberg)) — His [flock-you](https://github.com/wgreenberg/flock-you) fork was instrumental in improving the Flock Safety detection heuristics. The BLE manufacturer company ID detection method (`0x09C8` XUNTONG) was sourced directly from his work and shipped in the Flock-You BLE mode; that mode has since been retired in favour of the promiscuous WiFi edition, but the structured pattern-management approach it introduced still informs the detection architecture. Thank you for the research and for making it open.

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
