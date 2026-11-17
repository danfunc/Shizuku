#!/bin/bash
set -euo pipefail

fail() { echo "ERROR(frtos-ipc): $*" >&2; exit 1; }
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
SC="$BUILD_DIR/sdkconfig"
for want in CONFIG_COMPILER_OPTIMIZATION_PERF=y CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_DISABLE=y; do
  grep -qx "$want" "$SC" || fail "sdkconfig lacks $want"
done
# Names not confirmed against this IDF: warn only, do not fail the build.
for want in CONFIG_FREERTOS_HZ=1000 CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=160; do
  grep -qx "$want" "$SC" || echo "WARNING(frtos-ipc): sdkconfig lacks $want" >&2
done
for off in CONFIG_BT_ENABLED CONFIG_FREERTOS_USE_TRACE_FACILITY CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS; do
  ! grep -qx "$off=y" "$SC" || fail "$off must be disabled"
done
idf.py -B "$BUILD_DIR" -DSDKCONFIG="$SC" build || fail "FreeRTOS IPC bench build failed"
for f in bin elf map; do
  test -s "$BUILD_DIR/shizuku_esp32c6_frtos_ipc.$f" || fail "$f missing"
done
cp "$BUILD_DIR/shizuku_esp32c6_frtos_ipc.bin" "$OUT"
cp "$BUILD_DIR/shizuku_esp32c6_frtos_ipc.elf" "$OUT_PREFIX.elf"
cp "$BUILD_DIR/shizuku_esp32c6_frtos_ipc.map" "$OUT_PREFIX.map"
cp "$SC" "$OUT_PREFIX.sdkconfig"
echo "OK(frtos-ipc): $IDF_VER / $GCC_BANNER -> $OUT"
