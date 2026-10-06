#!/usr/bin/env bash
set -euo pipefail

# test_fail_closed.cpp が #error でコンパイル失敗することを確認する (fail-closed 検証)
if clang++ -std=c++20 -I. -Iinternal_headers -c ports/esp32/c6/test_fail_closed.cpp 2>/dev/null; then
  echo "ERROR: test_fail_closed.cpp should have failed compilation, but succeeded!" >&2
  exit 1
fi

# test_fail_closed_board.cpp が #error でコンパイル失敗することを確認する (fail-closed 検証)
if clang++ -std=c++20 -I. -Iinternal_headers -c ports/esp32/c6/test_fail_closed_board.cpp 2>/dev/null; then
  echo "ERROR: test_fail_closed_board.cpp should have failed compilation, but succeeded!" >&2
  exit 1
fi

echo "All fail-closed compile refusal tests passed."
exit 0
