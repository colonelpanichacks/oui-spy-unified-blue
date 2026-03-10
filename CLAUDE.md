# OUI SPY Unified Blue

ESP32-S3 XIAO multi-module surveillance detection firmware. All 5 modules (detector, foxhunter, flockyou, skyspy, wardriver) run in parallel — no mode switching. Preact SPA dashboard served from embedded flash.

## Build / Test / Lint

```bash
# Firmware
just build                # pio run (-Werror, must be clean)
just test-cpp             # Unity native tests (native + native_parser envs)
just coverage-cpp         # native tests + gcovr (35% line threshold)
just analyze-cpp          # cppcheck static analysis

# Python
just test                 # uv run pytest -q
just lint                 # pre-commit run --all-files

# Web dashboard (web/)
just web-build            # Vite production build (100KB bundle gate)
just web-lint             # Biome linter
just web-test             # Vitest + Testing Library
just web-typecheck        # TypeScript type checks
just generate-types       # openapi.yaml -> TypeScript types

# All quality checks
just quality

# Docker equivalents (no local toolchain needed, just Docker + just)
just docker-build         # firmware build in devcontainer image
just docker-quality       # all checks in devcontainer image
```

## Code Architecture

- **`src/modules/`** — Detection engines implementing `IModule` interface (`module.h`). BLE callbacks via `hal::BLEListener` mixin.
- **`src/modules/*_logic.h/cpp`** — Pure logic (no Arduino deps), testable on native.
- **`src/hal/`** — Hardware abstraction: BLE scan management, WiFi AP/promiscuous, buzzer, neopixel, GPS, notifications.
- **`src/web/`** — HTTP server + route handlers. Routes in `*_routes.cpp` files.
- **`src/storage/nvs_store.cpp`** — Typed NVS accessors. 6 namespaces (ouispy-mod, ouispy-ap, ouispy-bz, ouispy, ouispy-dev, tracker). Plus per-module namespaces for wardriver (ouispy-wd, ouispy-wdd, ouispy-wdc).
- **`src/protocols/`** — ASTM F3411 Open Drone ID codec (C library) + WiFi frame parser.
- **`web/`** — Preact SPA. OpenAPI schema at `web/api/openapi.yaml`.

## Test Structure

- **C++ native tests:** Unity framework, `test/test_native_smoke/` and `test/test_native_parser_edge/`. `_under_test.cpp` wrappers include `_logic.cpp` directly.
- **Python tests:** `tests/test_flash.py` — pytest, exercises `scripts/flash.py`.
- **Web tests:** `web/src/**/*.test.{ts,tsx}` — Vitest + jsdom + Testing Library.
- **Coverage:** `scripts/native_coverage.py` adds gcov flags (Linux only, Apple Clang lacks libgcov).

## Debugging

`scripts/monitor.py` — lightweight serial monitor that works without a TTY (unlike `pio device monitor`). Works in headless environments and Claude Code sessions.

```bash
just monitor                  # auto-detect port, stream forever
just monitor --timeout 10     # capture 10 seconds
just monitor --port /dev/...  # explicit port
```

## Key Conventions

- Modules default to DISABLED on first boot (`getModuleEnabled` default=false).
- ArduinoJson v7 for all JSON API responses.
- `-Werror` enforced; firmware binary must stay under 2MB (CI gate).
- Scan mode: one-shot NVS flag (`ouispy-boot/scan`) for WiFi promiscuous reboot. Self-clearing.
- Flash script (`scripts/flash.py`): use `-y`/`--yes` for non-interactive mode. `just flash-build` and `just flash-release` pass `-y` automatically.
- All `just` commands in Justfile; `docker-*` variants run in devcontainer image.

## Deeper Docs

- `README.md` — Full project docs, API reference, hardware pinout, architecture diagram.
- `docs/adr/` — Architecture Decision Records (USB dashboard access, ESP-IDF 5.x upgrade).
- `.devcontainer/README.md` — Devcontainer profiles, Docker target docs.
- `web/api/openapi.yaml` — API schema (source of truth for TypeScript types).
