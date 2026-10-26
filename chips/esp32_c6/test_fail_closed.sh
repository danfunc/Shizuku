#!/usr/bin/env bash
set -euo pipefail

# コンパイラの解決 (worker では g++/c++, macOS では clang++)
CXX_BIN="${CXX:-}"
if [ -z "$CXX_BIN" ]; then
  for candidate in clang++ g++ c++; do
    if command -v "$candidate" >/dev/null 2>&1; then
      CXX_BIN="$candidate"
      break
    fi
  done
fi

if [ -z "$CXX_BIN" ]; then
  echo "ERROR: no C++ compiler found (checked clang++, g++, c++)" >&2
  exit 1
fi

scratch=$(mktemp -d "${TMPDIR:-/tmp}/shizuku-c6-gate.XXXXXX")
trap 'rm -rf "$scratch"' EXIT

for spec in arch board; do
  src=chips/esp32_c6/test_fail_closed.cpp
  diagnostic='ESP32-C6 arch backend is incomplete'
  if [ "$spec" = board ]; then
    src=chips/esp32_c6/test_fail_closed_board.cpp
    diagnostic='ESP32-C6 board backend is incomplete'
  fi
  if "$CXX_BIN" -std=c++20 -I. -Iinternal_headers -c "$src" -o "$scratch/$spec.o" 2>"$scratch/$spec.log"; then
    echo "ERROR: $spec production gate unexpectedly compiled" >&2
    exit 1
  fi
  # A missing include/compiler must not masquerade as a successful gate test.
  grep -q "$diagnostic" "$scratch/$spec.log"
  "$CXX_BIN" -std=c++20 -DSHIZUKU_C6_SKELETON_PERMITTED=1 -I. -Iinternal_headers \
    -c "$src" -o "$scratch/$spec.o"
done
echo "PASS: arch/board reject production and compile with explicit pilot opt-in (compiler: $CXX_BIN)"


