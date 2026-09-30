#!/usr/bin/env bash
# Prepare the project-local Arduino environment for the Nesso N1 sketch.
#
#  1. Shadow conflicting global libraries with project-controlled copies.
#  2. Vendor the patched ESP32Async stack from ../lib/async_web (shared with
#     the PlatformIO build) instead of the global AsyncTCP@1.1.4.
#  3. Mirror the canonical sources from ../src into this sketch directory.
#
# The mirrored *.cpp/*.c/*.h here and the raw/ subdir are GENERATED — they are
# git-ignored. Edit ../src (the single source of truth), never the copies.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PROJECT="$(cd "$ROOT/.." && pwd)"
USER_DIR="$ROOT/.arduino-cli-user/libraries"
GLOBAL="${HOME}/Documents/Arduino/libraries"
ASYNC="$PROJECT/lib/async_web"
SRC="$PROJECT/src"

mkdir -p "$USER_DIR"

# Real copies, not symlinks: .arduino-cli-user lives inside the sketch dir and
# arduino-cli copies it into build-nesso/sketch/; symlinks to ~/Documents break.
stage_lib() {
  local src="$1"
  local name="$2"
  if [[ ! -d "$src" ]]; then
    echo "Missing library: $src" >&2
    exit 1
  fi
  rm -rf "$USER_DIR/$name"
  cp -R "$src" "$USER_DIR/$name"
}

stage_global() {
  stage_lib "$GLOBAL/$1" "$1"
}

TINYGPS="$PROJECT/lib/TinyGPSPlus"
NESSO_BOARD="$PROJECT/lib/Arduino_Nesso_N1"

# Board + deps from the normal sketchbook (not vendored in-repo).
for lib in M5GFX Arduino_BMI270_BMM150 NimBLE-Arduino ArduinoJson; do
  stage_global "$lib"
done

if [[ -d "$NESSO_BOARD" ]]; then
  stage_lib "$NESSO_BOARD" "Arduino_Nesso_N1"
else
  stage_global "Arduino_Nesso_N1"
fi

stage_lib "$TINYGPS" "TinyGPSPlus"

# Patched ESP32Async stack — single copy lives in ../lib/async_web, shared with
# the PlatformIO env. AsyncTCP carries the lwIP TCPIP-core-lock fix; must NOT
# fall back to the global AsyncTCP@1.1.4.
stage_lib "$ASYNC/AsyncTCP" "AsyncTCP"
stage_lib "$ASYNC/ESP_Async_WebServer" "ESP_Async_WebServer"

# Mirror canonical sources into the sketch dir (generated, git-ignored).
# Warn loudly if a generated copy was hand-edited instead of ../src so the
# edit is not silently overwritten by this sync.
warned=0
warn_if_diverged() {
  local gen="$1" canon="$2"
  if [[ -f "$gen" && -f "$canon" ]] && ! diff -q "$gen" "$canon" >/dev/null 2>&1; then
    if [[ "$warned" == 0 ]]; then
      echo "WARNING: generated sketch sources differ from ../src and will be overwritten:" >&2
      warned=1
    fi
    echo "  - ${gen#"$ROOT"/}  (edit ../src/${canon#"$SRC"/} instead)" >&2
  fi
}

shopt -s nullglob
for f in "$SRC"/*.cpp "$SRC"/*.c "$SRC"/*.h; do
  base="$(basename "$f")"
  if [[ "$base" == "mode_flockyou.cpp" ]]; then
    continue
  fi
  warn_if_diverged "$ROOT/$base" "$f"
  cp "$f" "$ROOT/"
done
rm -f "$ROOT/mode_flockyou.cpp"
for f in "$SRC"/raw/*; do
  warn_if_diverged "$ROOT/raw/$(basename "$f")" "$f"
done
rm -rf "$ROOT/raw"
mkdir -p "$ROOT/raw"
cp "$SRC"/raw/* "$ROOT/raw/"

# Partition table with a spiffs region (required for Mega_Maid session persistence).
# Must be named partitions.csv — Arduino prebuild copies sketch/partitions.csv first.
cp "$PROJECT/partitions_nesso.csv" "$ROOT/partitions.csv"

echo "Arduino user libraries ready at $USER_DIR"
echo "  TinyGPSPlus         -> $TINYGPS"
echo "  AsyncTCP            -> $ASYNC/AsyncTCP (patched 3.4.10, QUERY_HOLDER core guard)"
echo "  ESP_Async_WebServer -> $ASYNC/ESP_Async_WebServer (3.11.1)"
echo "Sketch sources mirrored from $SRC (generated, git-ignored)"
