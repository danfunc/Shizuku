#!/bin/bash
set -euo pipefail
if [[ $# -ne 11 ]]; then
  echo "usage: build_ota_profile.sh <build.sh> <collector.py> <archive> <bin> <elf> <map> <sdkconfig> <bootloader> <partition-bin> <partition-csv> <manifest>" >&2
  exit 2
fi
BUILD_SH=$1
COLLECTOR=$2
ARCHIVE=$3
BIN=$4
ELF=$5
MAP=$6
SDKCONFIG=$7
BOOTLOADER=$8
PARTITION_BIN=$9
PARTITION_CSV=${10}
MANIFEST=${11}
export SHIZUKU_ESP_OTA=1
"$BUILD_SH" "$ARCHIVE" "$BIN" esp-15.2.0_20251204 esp32c6 ble "$ELF" "$MAP"
python3 "$COLLECTOR" "$BIN" "$SDKCONFIG" "$BOOTLOADER" "$PARTITION_BIN" "$PARTITION_CSV" "$MANIFEST"
