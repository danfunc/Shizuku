#!/bin/bash
# Pure static source regression test runner for RP2040 Pico SDK CMake firmware integration.
# Runs without Bazel/CMake/build.sh/compiler/device operations.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PYTHON_TEST="${SCRIPT_DIR}/test_rp2040_firmware_source.py"

echo "=== Running RP2040 Static Source Regression Test Suite ==="
PYTHONDONTWRITEBYTECODE=1 python3 "$PYTHON_TEST"
echo "=== All RP2040 static source regression tests completed successfully ==="
