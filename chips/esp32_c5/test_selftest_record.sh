#!/bin/bash
set -euo pipefail

# C5 selftest RAM record unit test
CXX="${CXX:-$(command -v clang++ 2>/dev/null || command -v g++ 2>/dev/null || echo c++)}"
OUT_BIN="${TMPDIR:-/tmp}/c5_test_selftest_record_$$"
"$CXX" -std=c++20 -Wall -Wextra -Werror -I. chips/esp32_c5/selftest_adapter.cpp chips/esp32_c5/test_selftest_record.cpp -o "$OUT_BIN"
trap 'rm -f "$OUT_BIN"' EXIT
"$OUT_BIN"
