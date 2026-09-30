# M5Stack Unit GPS v1.1 (SKU U032-V11)

Official product page: [M5 Unit GPS v1.1](https://docs.m5stack.com/en/unit/Unit-GPS%20v1.1)

GNSS module based on **ATGM336H-6N** (AT6668), multi-constellation (GPS, QZSS, BDS, GLONASS, Galileo). Outputs **NMEA 0183** over UART.

## Specifications (summary)

| Item | Value |
|------|--------|
| UART | **115200 baud**, 8N1 (default) |
| Protocol | NMEA 0183 4.1 |
| Accuracy | &lt;1.5 m CEP50 |
| Update rate | Up to 10 Hz |
| Power | 5 V, ~32 mA |
| Cable | HY2.0-4P Grove, 20 cm |

## Unit connector (PORT.C)

| HY2.0-4P | Black | Red | Yellow | White |
|----------|-------|-----|--------|-------|
| PORT.C | GND | 5V | UART_RX | UART_TX |

On the **unit**, Yellow is RX (host TX) and White is TX (host RX). Cross-connect per normal UART practice when using a straight Grove cable host-to-unit.

## Nesso N1 wiring

1. Plug the Unit GPS into the Nesso **HY2.0-4P Grove** port with the included cable.
2. Firmware enables the Grove **5 V** rail via `GROVE_POWER_EN` before UART starts.
3. ESP32-C6 UART1 mapping (M5 PORT.C host convention):
   - **GPIO 5** (`GROVE_IO_0`) → host TX → unit Yellow (RX)
   - **GPIO 4** (`GROVE_IO_1`) → host RX ← unit White (TX)
4. Used in **Flock-You** and **Mega_Maid** for wardriving geotags; phone browser GPS remains a fallback if the module is absent or has no fix.

## Firmware fix rates (Mega_Maid / Flock-You)

Firmware sets the ATGM336H fix interval with **PMTK220** when switching radio profiles (`board_gps.cpp`):

| Profile | Fix interval | UART poll (dashboard only) | When |
|---------|--------------|----------------------------|------|
| **Collect** | 200 ms (5 Hz) | every main loop (~20 ms) | Wardriving / active scan |
| **Dashboard** | 1000 ms (1 Hz) | at most once per second | AP export / review |

Collect mode restores 5 Hz before scanning resumes. Dashboard mode keeps the module powered (no cold start) but reduces GNSS activity and CPU wakeups to save power on marginal USB supplies.

If no NMEA appears in serial logs, try swapping RX/TX in [`src/board_gps.h`](../../src/board_gps.h).

## References

- [Schematics PDF](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/849/U032-V11-UNIT_GPS_SCHE.pdf)
- [ATGM336H-6N datasheet](https://m5stack.oss-cn-shenzhen.aliyuncs.com/resource/docs/products/unit/Unit-GPS%20v1.1/ATGM336H-6N.pdf)
- Arduino parsing: [TinyGPS++](https://github.com/mikalhart/TinyGPSPlus) (same library as M5’s Arduino examples)

## XIAO ESP32-S3 (non-Nesso)

Optional **Seeed L76K** on GPIO 43/44 at **9600** baud is still supported; see `board_gps.h`.
