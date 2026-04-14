# OUI-SPY Unified — GPS & Buzzer Fix Fork

Fork of [colonelpanichacks/oui-spy-unified-blue](https://github.com/colonelpanichacks/oui-spy-unified-blue) with fixes for **GPS lock loss during wardriving** and **LEDC buzzer initialization** in Flock-You mode (Mode 4).

These fixes improve GPS reliability for all Android users and are especially relevant for anyone running **GrapheneOS**, where the combination of device-only GPS, sandboxed Google Play Services, and HTTP Geolocation API restrictions created a perfect storm for GPS failures.

---

## What's Fixed

### GPS Auto-Recovery

The original `watchPosition` call would silently stop delivering updates when the phone screen locked, Chrome backgrounded the tab, or the OS throttled location services. Once GPS went stale, new detections stopped getting tagged with coordinates — with no indication anything was wrong.

**Changes to `src/raw/flockyou.cpp`:**

- **Health monitor** — checks every 10 seconds whether GPS is still alive. If the last successful fix is older than 20 seconds, the watch is torn down and restarted automatically, before the ESP32's 30-second stale threshold expires
- **Relaxed timing** — `maximumAge` increased from 5s to 15s, `timeout` from 15s to 30s. Device-only GPS (no network assist) needs more time to acquire satellites, especially on GrapheneOS
- **Manual restart** — tapping the GPS card now always restarts the watch, even if GPS was previously working. The old code blocked re-initialization once a fix succeeded
- **Send failure tracking** — counts consecutive failed `fetch` calls to `/api/gps`. After 3 failures, the indicator shows "SEND" (yellow) so you know the ESP32 isn't receiving coordinates
- **Smarter UI state** — the stats poll no longer overrides JavaScript-managed GPS state. Previously it would reset the indicator to "TAP" (red) whenever the ESP32 reported no GPS source, even with an active watch
- **Accuracy display** — shows accuracy in meters when >50m, so you can tell rough network fixes from proper GPS locks
- **Fixed alert text** — double-escaped `\n` characters that displayed as literal `\n\n` in alert dialogs now render as actual line breaks

### Buzzer LEDC Fix

The boot crow caw sounded muffled and threw `LEDC is not initialized` errors because `tone()` was called before the ESP32's LEDC peripheral was ready.

**Changes:**

- Explicit LEDC channel 0 initialization in `setup()` before any audio plays
- All `tone()`/`noTone()` calls replaced with direct `ledcSetup()`/`ledcWrite()` control, matching the approach used by Foxhunter and other modes
- Crow caw now plays cleanly on boot with no error messages

---

## GrapheneOS Setup Guide

GPS wardriving on Flock-You requires the browser Geolocation API over HTTP. GrapheneOS has additional privacy controls that need specific configuration. This guide was tested on a **Pixel 7a running GrapheneOS** with sandboxed Google Play Services.

### Step 1: Enable Location Rerouting

GrapheneOS sandboxes Google Play Services, which means Chrome's location requests go to Play Services — but Play Services doesn't have privileged location access like it does on stock Android. The requests silently fail.

**Settings → Apps → Sandboxed Google Play → Google Play Services → Permissions → Reroute location requests to OS → Enable**

This forwards location requests to GrapheneOS's own location provider (device-only GPS), which actually has hardware access.

### Step 2: Grant Chrome Location Permission

**Settings → Apps → Chrome → Permissions → Location → "Allow while using the app"**

### Step 3: Set Chrome Flag for Insecure Origins

The Geolocation API requires a secure context (HTTPS), but the ESP32 serves its dashboard over HTTP. Chrome has a flag to allow this for specific local IPs.

1. Open Chrome
2. Navigate to `chrome://flags`
3. Search for **"Insecure origins treated as secure"**
4. In the text field, enter: `http://192.168.4.1`
5. Set the dropdown to **Enabled**
6. Tap **Relaunch**

### Step 4: Configure Chrome Site Permissions

**Chrome → three dots → Settings → Site settings → Location:**

- Set to **"Sites can ask for your location"**
- Under "How to show requests," set to **"Expand all requests"** (prevents Chrome from silently collapsing the permission prompt for HTTP sites)

### Step 5: Use Chrome — Not the Captive Portal

This is the most common mistake. When you connect to the `flockyou` WiFi AP, Android shows a "Sign in to flockyou" captive portal popup. **This is a restricted WebView that does not support the Geolocation API at all.** GPS will always be denied in the captive portal browser.

Instead:

1. Connect to `flockyou` / `flockyou123`
2. When the captive portal appears, tap the three dots → **"Use this network as is"**
3. Open the **Chrome app** separately
4. Navigate to `http://192.168.4.1`
5. Tap the **GPS card** in the stats bar
6. Chrome should prompt for location permission → tap **Allow**
7. GPS indicator turns green with "OK" or accuracy in meters

### Troubleshooting

**GPS shows "DENIED" (red):**
Chrome cached a previous denial. Go to Chrome → Settings → Site settings → Location, find `192.168.4.1` in the Blocked list, and remove it. If no Blocked list is visible, try opening an Incognito tab to `192.168.4.1` — Incognito starts with clean permissions. To reset all site permissions: Chrome → Settings → Privacy and security → Clear browsing data → Advanced → check only "Site settings" → Clear data.

**GPS shows "..." (yellow) and never turns green:**
GPS is acquiring. Device-only GPS (no network assist) can take 30–60 seconds for a cold fix, especially indoors. Move near a window or outside. The patched firmware allows up to 30 seconds before timing out and will auto-retry.

**GPS shows "LOST" (yellow):**
The health monitor detected that GPS went stale and is auto-restarting the watch. This is normal when resuming after screen lock or tab backgrounding. It should recover within a few seconds.

**GPS shows "SEND" (yellow):**
Chrome has a GPS fix but the `fetch` calls to the ESP32 are failing. Check that you're still connected to the `flockyou` WiFi AP.

**`chrome://flags` setting disappeared after Chrome update:**
GrapheneOS Chrome updates can reset flags. Re-enter `http://192.168.4.1` in the "Insecure origins treated as secure" field and relaunch.

---

## Building & Flashing

Requires [PlatformIO](https://platformio.org/).

```bash
# Install PlatformIO
pip3 install platformio

# Build
cd oui-spy-unified-blue
pio run

# Flash (plug in XIAO ESP32-S3 via USB-C data cable)
pio run -t upload

# Monitor serial output (optional)
pio device monitor
```

---

## Verifying GPS

After flashing, connect your phone to the `flockyou` AP and open `http://192.168.4.1/api/stats` in Chrome. You should see:

```json
{
  "gps_valid": true,
  "gps_age": 1234,
  "gps_src": "phone",
  "gps_hw_detected": false
}
```

- `gps_valid: true` — GPS is active
- `gps_age` — milliseconds since last update (should be under 5000 if streaming)
- `gps_src: "phone"` — coordinates coming from browser Geolocation API

---

## Hardware

**Board:** Seeed Studio XIAO ESP32-S3

| Pin | Function |
|-----|----------|
| GPIO 3 | Piezo buzzer |
| GPIO 4 | NeoPixel LED |
| GPIO 44/43 | Hardware GPS UART (optional Seeed L76K module) |

---

## Upstream

All code changes are in `src/raw/flockyou.cpp`. See the original project for full documentation on all four firmware modes:

**[colonelpanichacks/oui-spy-unified-blue](https://github.com/colonelpanichacks/oui-spy-unified-blue)**

---

## Credits

- **colonelpanichacks** — original OUI-SPY Unified firmware
- **wgreenberg** — Flock Safety BLE manufacturer ID research ([flock-you](https://github.com/wgreenberg/flock-you))
