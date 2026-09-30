# Mega_Maid Test Detections

Field-test captures for **Mode 6: Mega_Maid** on the Nesso N1.
Same export shape as the Flock-You reference file (`test/flockyou_test_detections.json`),
but structured as multiple runs because Mega_Maid collects **all** seen MACs rather than
matching a fixed watchlist.

## Test setup

| Item | Value |
|------|-------|
| Mode | 6 — Mega_Maid |
| Radio profile | Collect (50/50 BLE / WiFi promiscuous) |
| Export source | Dashboard `/api/detections` or TOOLS → Download JSON |
| Raw data | [`megamaid_test_detections.json`](megamaid_test_detections.json) |

## Expected behavior

- **BLE:** every advertisement recorded as `method: ble_adv`, `channel: 0`; local name in `name` when broadcast
- **WiFi MAC:** unicast globally-administered MACs as `wifi_addr1` or `wifi_addr2` with WiFi channel 1/6/11
- **WiFi SSID:** AP network names from beacons/probe responses in `ssid`; probe-request targets (familiar/saved networks) in `probed_ssids` with `method: wifi_probe`
- **WiFi methods:** `wifi_probe`, `wifi_beacon`, `wifi_probe_resp` when SSID IE is parsed from management frames
- **Dedup:** same MAC increments `count` and updates `last` / `rssi`; probe SSIDs merge into `probed_ssids` (up to 3 unique names)
- **Alerts:** buzzer on each **new unique** MAC (not on re-sights)
- **No filtering:** unlike Flock-You, no OUI/name/Raven gates — all traffic is stored

## Field-test checklist (SSID / probe collection)

After flashing with SSID support:

1. **Phone WiFi on** — walk collect mode for ~30s; expect client MACs with `wifi_probe`, non-empty `probed_ssids` listing home/work SSIDs the phone is searching for
2. **Nearby AP** — expect `wifi_beacon` or `wifi_probe_resp` on BSSID MAC with AP name in `ssid`
3. **Named BLE device** — e.g. `"ihoment_H5044_3963"` still appears in `name`, not `ssid`
4. **Export** — `/api/detections`, CSV, and session JSON include `ssid` and `probed_ssids` when present
5. **Privacy note:** probe SSIDs reveal saved network names; treat exports accordingly

## Run 1: Short collect session

Indoor smoke test after initial Mega_Maid flash. Collect mode, ~12s from first to last sighting.

| Metric | Value |
|--------|-------|
| Unique devices | 23 |
| Unique OUIs | 23 |
| Total hits (sum of counts) | 182 |
| Active window | 12.1s (`first`→`last` ms) |

**Methods**

- `ble_adv`: 6 unique MACs
- `wifi_addr1`: 2 unique MACs
- `wifi_addr2`: 15 unique MACs

**WiFi channels seen**

- Channel 1: 10 unique MACs
- Channel 6: 7 unique MACs

**Named BLE devices**

- `b0:78:39:c2:5c:7f` — net
- `98:17:3c:87:39:63` — ihoment_H5044_3963

**Top sightings by count**

| MAC | Method | Count | RSSI | Name |
|-----|--------|-------|------|------|
| `40:3f:8c:a8:49:c6` | `wifi_addr2` | 37 | -83 | — |
| `fc:12:63:27:7a:4e` | `wifi_addr2` | 22 | -83 | — |
| `18:d6:c7:f7:38:ef` | `wifi_addr2` | 18 | -88 | — |
| `f8:35:dd:67:56:af` | `wifi_addr2` | 18 | -93 | — |
| `bc:07:1d:28:a1:63` | `wifi_addr2` | 17 | -90 | — |
| `28:80:88:49:a4:7e` | `wifi_addr2` | 17 | -75 | — |
| `94:f7:be:d0:20:f2` | `wifi_addr2` | 15 | -91 | — |
| `14:59:c0:93:2a:68` | `wifi_addr2` | 10 | -94 | — |
| `f8:d0:0e:4c:32:40` | `wifi_addr2` | 5 | -88 | — |
| `fc:f5:c4:1f:ca:e8` | `wifi_addr2` | 4 | -86 | — |

## Run 2: Extended collect session

Longer residential RF environment. Validates BLE+WiFi time-slicing, dedup counts, and named BLE devices.

| Metric | Value |
|--------|-------|
| Unique devices | 83 |
| Unique OUIs | 79 |
| Total hits (sum of counts) | 89370 |
| Active window | 2119.9s (`first`→`last` ms) |

**Methods**

- `ble_adv`: 59 unique MACs
- `wifi_addr1`: 8 unique MACs
- `wifi_addr2`: 16 unique MACs

**WiFi channels seen**

- Channel 1: 17 unique MACs
- Channel 6: 3 unique MACs
- Channel 11: 4 unique MACs

**Named BLE devices**

- `da:d2:d1:46:52:65` — Gladys Room
- `44:1b:f6:96:56:21` — Meshtastic_5620
- `64:db:a0:15:39:f6` — 64:db:a0:15:39:f6
- `80:e1:26:22:2d:fb` — Almamor
- `60:ab:d2:66:27:31` — LE-Bose Micro SoundLink
- `5d:7a:79:0e:34:68` — 4

**Top sightings by count**

| MAC | Method | Count | RSSI | Name |
|-----|--------|-------|------|------|
| `da:d2:d1:46:52:65` | `ble_adv` | 13352 | -89 | Gladys Room |
| `44:1b:f6:96:56:21` | `ble_adv` | 13014 | -60 | Meshtastic_5620 |
| `24:5a:4c:5d:5e:a4` | `wifi_addr2` | 5390 | -45 | — |
| `5c:86:c1:08:f7:86` | `wifi_addr2` | 4766 | -40 | — |
| `24:5a:4c:6b:7b:68` | `wifi_addr2` | 4427 | -68 | — |
| `6c:4a:85:53:16:95` | `ble_adv` | 3985 | -83 | — |
| `78:45:58:37:08:93` | `wifi_addr2` | 3295 | -64 | — |
| `70:2c:1f:5f:9a:52` | `wifi_addr2` | 3176 | -79 | — |
| `68:d7:9a:24:ac:2f` | `wifi_addr2` | 2748 | -42 | — |
| `53:07:6c:96:d1:6d` | `ble_adv` | 2376 | -54 | — |

## Run 3: SSID and probe-request collect session

Same residential site as Run 2, captured after SSID IE parsing shipped. Validates
`wifi_probe` / `wifi_probe_resp` methods plus `ssid` and `probed_ssids` export fields.

| Metric | Value |
|--------|-------|
| Unique devices | 48 |
| Unique OUIs | 45 |
| Total hits (sum of counts) | 40679 |
| Active window | 712.6s (`first`→`last` ms) |

**Methods**

- `ble_adv`: 29 unique MACs
- `wifi_probe`: 10 unique MACs
- `wifi_probe_resp`: 2 unique MACs
- `wifi_addr1`: 4 unique MACs
- `wifi_addr2`: 3 unique MACs

**WiFi channels seen**

- Channel 1: 13 unique MACs
- Channel 6: 3 unique MACs
- Channel 11: 3 unique MACs

**Named BLE devices**

- `da:d2:d1:46:52:65` — Gladys Room
- `44:1b:f6:96:56:21` — Meshtastic_5620
- `80:e1:26:22:2d:fb` — Almamor
- `60:ab:d2:66:27:31` — LE-Bose Micro SoundLink
- `64:db:a0:15:39:f6` — 64:db:a0:15:39:f6

**SSID / probe highlights**

| MAC | Method | SSID / probed | Notes |
|-----|--------|---------------|-------|
| `24:5a:4c:5d:5e:a4` | `wifi_probe_resp` | The Shack | AP on ch 11 (was `wifi_addr2` in Run 2) |
| `60:22:32:27:35:b6` | `wifi_probe_resp` | The Shack | Hidden/secondary BSSID for same AP |
| `24:5a:4c:6b:7b:68` | `wifi_probe` | The Shack | Client probing saved network |
| `78:45:58:37:08:93` | `wifi_probe` | The Shack | Second client on same SSID |
| `70:2c:1f:5f:9a:52` | `wifi_probe` | [range] Samsung | Samsung device probing saved SSID |
| `5c:86:c1:08:f7:86` | `wifi_probe` | SAMSUNGSMART | Samsung smart-home client |
| `84:0b:7c:7f:49:38` | `wifi_probe` | Moby_130 | Guest/mobile hotspot name |
| `84:0b:7c:e0:66:08` | `wifi_probe` | Unit-B | Saved network probe |
| `94:9f:3e:8b:0a:63` | `wifi_probe` | Sonos_H9HfbIogo38mkVvoFCdan9ikqj | Sonos speaker setup SSID |
| `d0:21:f9:f0:2a:e5` | `wifi_probe` | — | Probe frames without parsed SSID IE |
| `68:d7:9a:24:ac:2f` | `wifi_probe` | — | Active client, no SSID in export |

**Top sightings by count**

| MAC | Method | Count | RSSI | Name / SSID |
|-----|--------|-------|------|-------------|
| `da:d2:d1:46:52:65` | `ble_adv` | 4637 | -96 | Gladys Room |
| `44:1b:f6:96:56:21` | `ble_adv` | 4279 | -64 | Meshtastic_5620 |
| `24:5a:4c:5d:5e:a4` | `wifi_probe_resp` | 3292 | -41 | The Shack |
| `5c:86:c1:08:f7:86` | `wifi_probe` | 2414 | -73 | SAMSUNGSMART |
| `24:5a:4c:6b:7b:68` | `wifi_probe` | 2379 | -64 | The Shack |
| `78:45:58:37:08:93` | `wifi_probe` | 2069 | -64 | The Shack |
| `70:2c:1f:5f:9a:52` | `wifi_probe` | 2062 | -77 | [range] Samsung |
| `68:d7:9a:24:ac:2f` | `wifi_probe` | 1932 | -66 | — |
| `60:22:32:27:35:b6` | `wifi_probe_resp` | 1919 | -90 | The Shack |
| `70:7d:c7:a7:b5:90` | `ble_adv` | 1715 | -44 | — |

## Field captures: Runs 4–16

Thirteen additional exports from Runestone/iCloud (`mega2.json`–`mega13.json`, `megamaid.json`).
Most source files were **truncated** (~7–15 KB) when copied; salvaged by closing at the last complete JSON object.
Run 11 and Run 12 (`mega9` / `mega10`) are **byte-identical** duplicate exports.

| Run | Source | Label | Devices | Hits | Window | Truncated |
|-----|--------|-------|---------|------|--------|-----------|
| 4 | `mega2.json` | Brief mixed probe snapshot | 20 | 145 | 3.6s | no |
| 5 | `mega3.json` | Multi-unit apartment complex | 41 | 2571 | 169.6s | yes |
| 6 | `mega4.json` | Retail and hospitality corridor | 41 | 1904 | 116.3s | yes |
| 7 | `mega5.json` | Residential mixed WiFi | 43 | 2186 | 142.8s | yes |
| 8 | `mega6.json` | Extended mixed environment | 47 | 20132 | 590.2s | yes |
| 9 | `mega7.json` | Parking lot and service area | 44 | 956 | 40.5s | no |
| 10 | `mega8.json` | Trade show and demo floor | 44 | 3010 | 280.5s | yes |
| 11 | `mega9.json` | Conference corp venue (BLE-heavy) | 48 | 7858 | 487.2s | yes |
| 12 | `mega10.json` | Conference corp venue (duplicate export) | 48 | 7858 | 487.2s | yes |
| 13 | `mega11.json` | Retail shop walk | 46 | 2788 | 103.1s | yes |
| 14 | `mega12.json` | Home and appliance walk | 44 | 3775 | 242.6s | yes |
| 15 | `mega13.json` | Mobile and travel mix | 47 | 3223 | 416.1s | yes |
| 16 | `megamaid.json` | Pre-SSID long addr-only collect | 104 | 1891 | 686.5s | yes |

### Method mix (unique MACs per run)

| Run | ble_adv | wifi_probe | wifi_probe_resp | wifi_addr1 | wifi_addr2 |
|-----|---------|------------|-----------------|------------|------------|
| 4 | 3 | 9 | 4 | 3 | 1 |
| 5 | 10 | 21 | 2 | 4 | 4 |
| 6 | 7 | 24 | 1 | 9 | 0 |
| 7 | 13 | 18 | 1 | 7 | 4 |
| 8 | 24 | 12 | 3 | 7 | 1 |
| 9 | 21 | 19 | 2 | 0 | 2 |
| 10 | 18 | 12 | 7 | 2 | 5 |
| 11 | 37 | 11 | 0 | 0 | 0 |
| 12 | 37 | 11 | 0 | 0 | 0 |
| 13 | 20 | 21 | 1 | 2 | 2 |
| 14 | 16 | 21 | 3 | 3 | 1 |
| 15 | 18 | 26 | 0 | 2 | 1 |
| 16 | 27 | 0 | 0 | 13 | 64 |

### Environment highlights

- **Run 4:** 3.6s snapshot — Apt 212, QuantumFiber4865, Labowski, ARRIS-9615 probe SSIDs.
- **Run 5:** Apartment block — 12+ `Apt ###` SSIDs from `e4:6c:d1` clients; Tcore Tag 59.
- **Run 6:** Retail/hospitality — Midco, ThirstMissions, Marine Bank; ALAM BLE tags; 1× flockyou probe.
- **Run 7:** Residential — HP DeskJet DIRECT, TMOBILE/Verizon clients, TUYA_/Samsung TV BLE.
- **Run 8:** Long mixed walk — omada24g, ATT-HOMEBASE, NETGEAR37; S39 hearing-aid LE names.
- **Run 9:** Parking/service — HASI_JIFFYLUBE, myChevrolet6966; 19thP# parking BLE beacons.
- **Run 10:** Trade show — PRODUCT_DEMO_NETWORK, MGM-Midco, Cub_Guest, Smart_Choice_Data_Transfer.
- **Run 11:** Corp conference — XXZKN/XXZUN industrial BLE IDs; Edge-WP, CorpNet, Vendor SSIDs.
- **Run 12:** Duplicate of Run 11 — discard one export when counting unique field sites.
- **Run 13:** Retail shop — The Batcave, AmericanImportsShop, Pretty Fly For a Wifi; 2× flockyou.
- **Run 14:** Home/appliances — Samsung Dryer, CenturyLink, Mario ARRIS; GBK LED strip BLE.
- **Run 15:** Mobile/travel — KeepTruckin hotspot, Mars Rover, Toyota RAV4 WiFi; 5× flockyou.
- **Run 16:** Pre-SSID firmware — `wifi_addr1/2` only (no `ssid` fields); 104 salvaged devices in ~11 min.

### Cross-run anchors and flockyou probes

- **Almamor** (`80:e1:26:22:2d:fb`) appears in Runs 4–10 and 13–16 — portable BLE anchor across most field sites.
- **Tcore Tag 59** (`80:6f:b0:15:22:87`) in Runs 5, 14, 15, 16.
- **flockyou** probe SSID hits: Run 6 (1), Run 8 (1), Run 13 (2), Run 15 (5) — validates Flock-You SSID string appears in Mega_Maid probe exports.
- **544 unique MACs** and **168 unique SSID strings** across Runs 4–16 combined.

## Comparison to Flock-You test file

| | Flock-You (`flockyou_test_detections.json`) | Mega_Maid (this file) |
|---|---|---|
| Purpose | Confirm known Flock/Raven OUIs hit | Confirm promiscuous collect-all works |
| Methods | `mac_prefix`, `wifi_addr1/2`, … | `ble_adv`, `wifi_addr1/2`, `wifi_probe`, `wifi_beacon`, `wifi_probe_resp` |
| Run 1 devices | — | 23 |
| Run 2 devices | — | 83 |
| Run 3 devices | — | 48 |
| Runs 4–16 devices | — | 48–104 per run (544 unique MACs combined) |

## Notes

- Runs 4–16: twelve of thirteen source JSON files were truncated mid-export (Run 4 and Run 9 intact). Re-export full `/api/detections` arrays if complete baselines are needed.
- Run 12 duplicates Run 11 exactly (`mega10.json` = `mega9.json`); treat as one field site.
- Run 16 (`megamaid.json`) predates SSID parsing — useful baseline for addr-only WiFi classification vs Runs 4–15.
- Run 3 overlaps 23 MACs with Run 2 but reclassifies 12 WiFi entries from `wifi_addr2` to `wifi_probe` or `wifi_probe_resp` once SSID IEs are parsed; 25 MACs are new to this shorter (~12 min) window.
- Run 3 export was truncated after `e3:48:69:43:69:23`; any additional trailing devices from the live session were not included.
- Run 2 includes `48:f6:ee:c4:d1:29` on WiFi (`wifi_addr1`, RSSI -31) — likely the Nesso/host radio itself.
- Many BLE MACs use locally administered/random prefixes (`xx:xx:xx` with second nibble 2/6/a/e); Mega_Maid still records BLE addresses as seen over the air.
- WiFi collector skips locally administered and multicast MACs by design.

