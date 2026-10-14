#!/bin/bash
set -euo pipefail

fail() { echo "ERROR(idf-reference): $*" >&2; exit 1; }
abspath() { python3 -c 'import os,sys;print(os.path.realpath(sys.argv[1]))' "$1"; }

test -n "${IDF_PATH:-}" || fail "IDF_PATH is unset"
test -n "${IDF_TOOLS_PATH:-}" || fail "IDF_TOOLS_PATH is unset"
OUT=$(abspath "$1")
WANT_GCC=$2
IDF_PATH=$(abspath "$IDF_PATH")
IDF_TOOLS_PATH=$(abspath "$IDF_TOOLS_PATH")
export IDF_PATH IDF_TOOLS_PATH
test -f "$IDF_PATH/export.sh" || fail "IDF_PATH is invalid"
. "$IDF_PATH/export.sh" >/dev/null 2>&1 || fail "ESP-IDF export.sh failed"
command -v idf.py >/dev/null 2>&1 || fail "idf.py not found after export"
command -v riscv32-esp-elf-gcc >/dev/null 2>&1 || fail "RISC-V GCC not found after export"
GCC_BANNER=$(riscv32-esp-elf-gcc --version | head -1)
case "$GCC_BANNER" in *"$WANT_GCC"*) ;; *) fail "unexpected GCC: $GCC_BANNER" ;; esac
IDF_VER=$(idf.py --version 2>/dev/null | tail -1)
case "$IDF_VER" in "ESP-IDF v6.1"*) ;; *) fail "ESP-IDF v6.1 required, got $IDF_VER" ;; esac
IDF_PY_BUILD_JOBS=${IDF_PY_BUILD_JOBS:-2}
case "$IDF_PY_BUILD_JOBS" in
  ''|*[!0-9]*|0) fail "IDF_PY_BUILD_JOBS must be a positive integer" ;;
esac
export IDF_PY_BUILD_JOBS

SRC=$(cd -P "$(dirname "$0")" && pwd)
OUT_PREFIX=${OUT%.bin}
test "$OUT_PREFIX" != "$OUT" || fail "output name must end in .bin"
mkdir -p "$(dirname "$OUT")"
BUILD_DIR=$(mktemp -d "$OUT.idf.XXXXXX")
cd "$SRC"
idf.py -B "$BUILD_DIR" -DSDKCONFIG="$BUILD_DIR/sdkconfig" set-target esp32c6 \
  >"$BUILD_DIR.set-target.log" 2>&1 || fail "set-target failed ($BUILD_DIR.set-target.log)"
grep -qx 'CONFIG_BT_ENABLED=y' "$BUILD_DIR/sdkconfig" || fail "BLE is disabled"
grep -qx 'CONFIG_BT_NIMBLE_ENABLED=y' "$BUILD_DIR/sdkconfig" || fail "NimBLE is disabled"
idf.py -B "$BUILD_DIR" -DSDKCONFIG="$BUILD_DIR/sdkconfig" build || fail "IDF reference build failed"
test -s "$BUILD_DIR/shizuku_esp32c6_idf_ref.bin" || fail "application BIN missing"
test -s "$BUILD_DIR/shizuku_esp32c6_idf_ref.elf" || fail "application ELF missing"
test -s "$BUILD_DIR/shizuku_esp32c6_idf_ref.map" || fail "link map missing"
cp "$BUILD_DIR/shizuku_esp32c6_idf_ref.bin" "$OUT"
cp "$BUILD_DIR/shizuku_esp32c6_idf_ref.elf" "$OUT_PREFIX.elf"
cp "$BUILD_DIR/shizuku_esp32c6_idf_ref.map" "$OUT_PREFIX.map"
echo "OK(idf-reference): $IDF_VER / $GCC_BANNER -> $OUT"
