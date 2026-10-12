#!/bin/bash
set -euo pipefail

# C6 selftest RAM record unit test
clang++ -std=c++20 -Wall -Wextra -Werror -I. chips/esp32_c6/selftest_adapter.cpp chips/esp32_c6/test_selftest_record.cpp -o /private/tmp/c6_test_selftest_record
/private/tmp/c6_test_selftest_record
