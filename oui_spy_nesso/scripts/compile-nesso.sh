#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

"$ROOT/scripts/setup-arduino-user.sh"

# Build outside the sketch dir so arduino-cli never tries to treat build
# artifacts as sketch sources, and so the ELF path is deterministic.
BUILD_PATH="$ROOT/../build-nesso"

EXTRA_FLAGS="-DNESSO_N1=1 -DNESSO_DISPLAY_UI=1 -DNESSO_NO_FLOCKYOU=1 -DCONFIG_BT_NIMBLE_ENABLED=1 -DCONFIG_LWIP_TCPIP_CORE_LOCKING=1 -Iraw"

# Find the connected ESP32 board's serial port. Prefer the Espressif USB
# vendor id (0x303A) reported by arduino-cli; fall back to any port it has
# matched to the esp32 core. Returns empty if nothing is connected.
detect_port() {
  local p
  p="$(arduino-cli board list --format json 2>/dev/null \
      | jq -r '.detected_ports[]
               | select(.port.properties.vid? == "0x303A")
               | .port.address' 2>/dev/null \
      | head -1)"
  if [[ -z "$p" ]]; then
    p="$(arduino-cli board list 2>/dev/null | awk '/esp32:esp32/ {print $1; exit}')"
  fi
  printf '%s' "$p"
}

# Libraries (including the vendored async stack) resolve from the project-local
# user dir set up by setup-arduino-user.sh; no --libraries override needed.
# partitions.csv (copied into the sketch by setup-arduino-user.sh) defines a spiffs
# region. The stock "default" scheme only has FAT, so SPIFFS.begin() fails.
PARTITION_PROPS=(
  --build-property "upload.maximum_size=7340032"
)

ARGS=(compile --clean --config-file arduino-cli-nesso.yaml
      --fqbn esp32:esp32:arduino_nesso_n1
      --build-path "$BUILD_PATH"
      --build-property "build.extra_flags=$EXTRA_FLAGS"
      "${PARTITION_PROPS[@]}")

if [[ "${1:-}" == "-u" || "${1:-}" == "--upload" ]]; then
  PORT="${2:-}"
  if [[ -z "$PORT" ]]; then
    PORT="$(detect_port)"
    if [[ -z "$PORT" ]]; then
      echo "ERROR: no ESP32 board detected. Plug the Nesso N1 in, or pass a port:" >&2
      echo "       $0 --upload /dev/cu.usbmodemXXXX" >&2
      exit 1
    fi
    echo "Auto-detected board on $PORT"
  fi
  ARGS+=(-u -p "$PORT")
fi

arduino-cli "${ARGS[@]}" .

ELF="$BUILD_PATH/oui_spy_nesso.ino.elf"
if [[ ! -f "$ELF" ]]; then
  echo "Could not locate built ELF for link verification at $ELF." >&2
  exit 1
fi

# Capture once into a variable. Reading from a here-string keeps grep -q from
# closing a pipe mid-stream, which under `pipefail` would surface as SIGPIPE
# (141) on `strings` and turn a real match into a false failure.
ASYNCTCP_PATHS="$(strings "$ELF" | grep -E 'AsyncTCP' || true)"

if grep -qE 'Documents/Arduino/libraries/AsyncTCP' <<<"$ASYNCTCP_PATHS"; then
  echo "ERROR: firmware still links global AsyncTCP@1.1.4 from Documents." >&2
  grep -E 'AsyncTCP.cpp' <<<"$ASYNCTCP_PATHS" | head -5 >&2
  exit 1
fi

# The patched AsyncTCP lives at lib/async_web/AsyncTCP, reached through the
# .arduino-cli-user symlink. The toolchain may record either the symlink path
# or the resolved realpath, so accept any in-repo AsyncTCP under projects/nesso.
if ! grep -qE 'projects/nesso/.*AsyncTCP' <<<"$ASYNCTCP_PATHS"; then
  echo "ERROR: patched vendored AsyncTCP was not linked." >&2
  echo "$ASYNCTCP_PATHS" | head -8 >&2
  exit 1
fi

if ! grep -qiE '(^|[[:space:]]*)spiffs[[:space:]]*,' "$BUILD_PATH/partitions.csv" 2>/dev/null; then
  echo "ERROR: built partition table has no spiffs region — mode sessions will not persist." >&2
  echo "       Expected oui_spy_nesso/partitions.csv from setup-arduino-user.sh." >&2
  exit 1
fi

echo "OK: partition table includes spiffs ($(grep -i spiffs "$BUILD_PATH/partitions.csv" | head -1 | tr -d ' '))"
