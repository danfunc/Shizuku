#!/bin/bash
# usage: measure.sh <uf2> <outdir>
# Writes the Zephyr benchmark with picotool (-f: reboot the running Shizuku firmware into
# BOOTSEL first), finds the CDC device by its USB product name ("Zephyr IPC bench", via
# ioreg; the /dev/cu.usbmodem* name is NOT used to decide, it can be the same as Shizuku's),
# starts capturing as soon as it is enumerated, and prints the table. The firmware returns
# to BOOTSEL by itself (30 s after a host has read a full dump, or 180 s after finishing),
# after which the board is ready for `picotool load` of the Shizuku firmware.
UF2=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
OUT=$2
mkdir -p "$OUT"
HERE=$(cd "$(dirname "$0")" && pwd)
PICOTOOL=$HOME/.pico-sdk/picotool/2.2.0-a4/picotool/picotool
PRODUCT="Zephyr IPC bench"

find_tty() {  # prints the IOCalloutDevice of the device whose product name is $PRODUCT
  ioreg -r -c IOUSBHostDevice -l -w0 2>/dev/null | awk -v p="$PRODUCT" '
    /^\+-o / { cur = index($0, p) > 0 }
    cur && /IOCalloutDevice/ { gsub(/.*= "/, ""); gsub(/".*/, ""); print; exit }'
}

"$PICOTOOL" load -x -v -f "$UF2" > "$OUT/load.log" 2>&1; echo "load=$?"
TTY=
for i in $(seq 1 240); do   # up to 120 s, 0.5 s steps
  TTY=$(find_tty)
  [ -n "$TTY" ] && break
  sleep 0.5
done
echo "tty=$TTY"
ioreg -r -c IOUSBHostDevice -l -w0 2>/dev/null | grep -E '^\+-o |IOCalloutDevice' > "$OUT/usb-devices.txt"
test -n "$TTY" || { echo "no '$PRODUCT' device enumerated (see $OUT/usb-devices.txt)"; exit 1; }
: > "$OUT/capture.txt"
python3 -I "$HERE/capture.py" "$TTY" "$OUT/capture.txt" 900; echo "capture=$?"
python3 -I "$HERE/read_results.py" "$OUT/capture.txt" | tee "$OUT/results.txt"
exit ${PIPESTATUS[0]}
