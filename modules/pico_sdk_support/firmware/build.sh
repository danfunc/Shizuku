#!/bin/bash
# RP2040 Pico SDK CMake final-link script.
# Links SDK-free Shizuku archive to produce a bootable UF2 binary.
# Usage: build.sh <SHIZUKU_ARCHIVE> <OUT_UF2> <OUT_ELF> <OUT_MAP> <PICO_SDK_PATH> <PICO_PLATFORM> <PICO_BOARD> <BUILD_DIR> <SHIZUKU_CONFIG_HEADER>
set -euo pipefail

fail() {
  echo "ERROR(rp2040_firmware): $*" >&2
  exit 1
}

abspath() {
  python3 -c 'import os,sys;print(os.path.realpath(sys.argv[1]))' "$1"
}

command -v python3 >/dev/null 2>&1 || fail "python3 is required to resolve paths."
command -v cmake >/dev/null 2>&1 || fail "cmake is not found on PATH."
command -v arm-none-eabi-gcc >/dev/null 2>&1 || fail "arm-none-eabi-gcc is not found on PATH."

if [ $# -lt 4 ]; then
  fail "Usage: $0 <SHIZUKU_ARCHIVE> <OUT_UF2> <OUT_ELF> <OUT_MAP> <PICO_SDK_PATH> <PICO_PLATFORM> <PICO_BOARD> <BUILD_DIR> <SHIZUKU_CONFIG_HEADER>"
fi

ARCHIVE=$(abspath "$1")
OUT_UF2=$(abspath "$2")
OUT_ELF=$(abspath "$3")
OUT_MAP=$(abspath "$4")

PICO_SDK_PATH=${5:-${PICO_SDK_PATH:-}}
[ -n "$PICO_SDK_PATH" ] || fail "PICO_SDK_PATH must be provided by the caller or environment."
PICO_SDK_PATH=$(abspath "$PICO_SDK_PATH")
PICO_PLATFORM=${6:-rp2040}
PICO_BOARD=${7:-pico}
BUILD_DIR_ARG=${8:-}
SHIZUKU_CONFIG_HEADER=${9:-}
[ -f "$SHIZUKU_CONFIG_HEADER" ] || fail "SHIZUKU_CONFIG_HEADER must be provided as a generated config.hpp file."
SHIZUKU_CONFIG_HEADER=$(abspath "$SHIZUKU_CONFIG_HEADER")

# Constraint: RP2040 only. RP2350 must not be declared supported.
if [ "$PICO_PLATFORM" != "rp2040" ]; then
  fail "Unsupported PICO_PLATFORM '$PICO_PLATFORM'. Only rp2040 is currently supported."
fi

# MVP Constraint: board pico only. Fail closed on pico_w or other boards.
if [ "$PICO_BOARD" != "pico" ]; then
  fail "Unsupported PICO_BOARD '$PICO_BOARD'. Only pico is currently supported (pico_w is not yet supported for standalone composition)."
fi

test -s "$ARCHIVE" || fail "Shizuku archive does not exist or is empty: '$ARCHIVE'"
test -f "$PICO_SDK_PATH/pico_sdk_init.cmake" || fail "PICO_SDK_PATH does not point to a valid Pico SDK: '$PICO_SDK_PATH'"

SRC=$(cd -P "$(dirname "$0")" && pwd)
if [ ! -f "$SRC/CMakeLists.txt" ]; then
  if [ -f "$SRC/modules/pico_sdk_support/firmware/CMakeLists.txt" ]; then
    SRC="$SRC/modules/pico_sdk_support/firmware"
  elif [ -f "modules/pico_sdk_support/firmware/CMakeLists.txt" ]; then
    SRC=$(abspath "modules/pico_sdk_support/firmware")
  elif [ -n "${BUILD_WORKSPACE_DIRECTORY:-}" ] && [ -f "$BUILD_WORKSPACE_DIRECTORY/modules/pico_sdk_support/firmware/CMakeLists.txt" ]; then
    SRC="$BUILD_WORKSPACE_DIRECTORY/modules/pico_sdk_support/firmware"
  fi
fi

# Deterministic target-scoped build directory
mkdir -p "$(dirname "$OUT_UF2")"
mkdir -p "$(dirname "$OUT_ELF")"
mkdir -p "$(dirname "$OUT_MAP")"
if [ -n "$BUILD_DIR_ARG" ]; then
  BUILD_DIR=$(abspath "$BUILD_DIR_ARG")
  mkdir -p "$BUILD_DIR"
elif [ -n "${SHIZUKU_RP_BUILD_DIR:-}" ]; then
  BUILD_DIR=$(abspath "$SHIZUKU_RP_BUILD_DIR")
  mkdir -p "$BUILD_DIR"
else
  BUILD_DIR="${OUT_UF2}.pico_build"
  mkdir -p "$BUILD_DIR"
fi

# Safe target identity check before reuse
if [ -f "$BUILD_DIR/CMakeCache.txt" ]; then
  CACHED_PLATFORM=$(grep -E "^PICO_PLATFORM:STRING=" "$BUILD_DIR/CMakeCache.txt" | cut -d= -f2 || true)
  CACHED_BOARD=$(grep -E "^PICO_BOARD:STRING=" "$BUILD_DIR/CMakeCache.txt" | cut -d= -f2 || true)
  if [ -n "$CACHED_PLATFORM" ] && [ "$CACHED_PLATFORM" != "$PICO_PLATFORM" ]; then
    echo "Notice: Target platform mismatch ($CACHED_PLATFORM != $PICO_PLATFORM). Reconfiguring cache safely."
    rm -f "$BUILD_DIR/CMakeCache.txt"
  elif [ -n "$CACHED_BOARD" ] && [ "$CACHED_BOARD" != "$PICO_BOARD" ]; then
    echo "Notice: Board mismatch ($CACHED_BOARD != $PICO_BOARD). Reconfiguring cache safely."
    rm -f "$BUILD_DIR/CMakeCache.txt"
  fi
fi

PICOTOOL_OPT=""
if [ -d "/opt/picotool/lib/cmake/picotool" ]; then
  PICOTOOL_OPT="-Dpicotool_DIR=/opt/picotool/lib/cmake/picotool"
fi

echo "=== Configuring RP2040 Pico SDK build ==="
cmake -B "$BUILD_DIR" -S "$SRC" \
  -DPICO_SDK_PATH="$PICO_SDK_PATH" \
  -DPICO_PLATFORM="$PICO_PLATFORM" \
  -DPICO_BOARD="$PICO_BOARD" \
  -DSHIZUKU_ARCHIVE="$ARCHIVE" \
  -DSHIZUKU_CONFIG_HEADER="$SHIZUKU_CONFIG_HEADER" \
  $PICOTOOL_OPT \
  -DCMAKE_BUILD_TYPE=Debug

echo "=== Building target shizuku_rp2040 ==="
cmake --build "$BUILD_DIR" --target shizuku_rp2040

if [ ! -f "$BUILD_DIR/shizuku_rp2040.uf2" ]; then
  fail "Build finished but '$BUILD_DIR/shizuku_rp2040.uf2' was not generated."
fi
if [ ! -f "$BUILD_DIR/shizuku_rp2040.elf" ]; then
  fail "Build finished but '$BUILD_DIR/shizuku_rp2040.elf' was not generated."
fi
if [ ! -f "$BUILD_DIR/shizuku_rp2040.map" ]; then
  fail "Build finished but '$BUILD_DIR/shizuku_rp2040.map' was not generated."
fi

cp "$BUILD_DIR/shizuku_rp2040.uf2" "$OUT_UF2"
cp "$BUILD_DIR/shizuku_rp2040.elf" "$OUT_ELF"
cp "$BUILD_DIR/shizuku_rp2040.map" "$OUT_MAP"
echo "OK(rp2040): Successfully generated UF2 -> $OUT_UF2"
echo "OK(rp2040): Successfully generated ELF -> $OUT_ELF"
echo "OK(rp2040): Successfully generated MAP -> $OUT_MAP"
