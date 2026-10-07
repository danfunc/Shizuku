#!/usr/bin/env bash
set -euo pipefail
scratch=$(mktemp -d "${TMPDIR:-/private/tmp}/shizuku-c6-gate.XXXXXX")
trap 'rm -rf "$scratch"' EXIT
for spec in arch board; do
  src=ports/esp32/c6/test_fail_closed.cpp
  diagnostic='ESP32-C6 arch backend is incomplete'
  if [ "$spec" = board ]; then
    src=ports/esp32/c6/test_fail_closed_board.cpp
    diagnostic='ESP32-C6 board backend is incomplete'
  fi
  if clang++ -std=c++20 -I. -Iinternal_headers -c "$src" -o "$scratch/$spec.o" 2>"$scratch/$spec.log"; then
    echo "ERROR: $spec production gate unexpectedly compiled" >&2
    exit 1
  fi
  # A missing include/compiler must not masquerade as a successful gate test.
  rg -q "$diagnostic" "$scratch/$spec.log"
  clang++ -std=c++20 -DSHIZUKU_C6_SKELETON_PERMITTED=1 -I. -Iinternal_headers \
    -c "$src" -o "$scratch/$spec.o"
done
echo "PASS: arch/board reject production and compile with explicit pilot opt-in"
