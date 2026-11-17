#!/bin/bash
# usage: build.sh [xip|sram] [builddir]
# Builds the Zephyr userspace IPC benchmark for Pico 2 (rp2350a/m33) on the Mac with west.
# xip  : default (code runs from flash)
# sram : kernel library relocated to SRAM (-DBENCH_SRAM=ON)
set -e
VAR=${1:-xip}
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${2:-$HERE/../../../.zbuild/zephyr_ipc_$VAR}
source "$HOME/github/zephyrproject/.venv/bin/activate"
export ZEPHYR_BASE=$HOME/github/zephyrproject/zephyr
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
export ZEPHYR_SDK_INSTALL_DIR=$HOME/.local/zephyr-sdk-1.0.1
EXTRA=()
[ "$VAR" = sram ] && EXTRA=(-DBENCH_SRAM=ON -DCONFIG_CODE_DATA_RELOCATION_SRAM=y)
# USER_CACHE_DIR keeps the toolchain-capability cache inside the build directory
# (the default is under $ZEPHYR_BASE, which a sandboxed shell cannot write).
mkdir -p "$OUT/.usercache" "$OUT/.ccache"
export CCACHE_DIR="$OUT/.ccache"   # default ~/.ccache is outside the sandbox's write set
west build -p auto -b rpi_pico2/rp2350a/m33 -d "$OUT" "$HERE" -- -DUSER_CACHE_DIR="$OUT/.usercache" "${EXTRA[@]}"
ls -l "$OUT/zephyr/zephyr.uf2" "$OUT/zephyr/zephyr.elf"
