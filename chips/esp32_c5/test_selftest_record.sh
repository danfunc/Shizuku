#!/bin/bash
set -euo pipefail

# C5 selftest RAM record unit test
clang++ -std=c++20 -Wall -Wextra -Werror -I. chips/esp32_c5/selftest_adapter.cpp chips/esp32_c5/test_selftest_record.cpp -o "${TMPDIR:-/private/tmp}/c5_test_selftest_record"
"${TMPDIR:-/private/tmp}/c5_test_selftest_record"
