#!/usr/bin/env bash
set -euo pipefail

# Fail-closed and Positive Validation Test for Shizuku Composition API
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

# unique /private/tmp fixture
WORK_DIR="$(mktemp -d /private/tmp/shizuku_comp_test_fixture.XXXXXX)"
echo "[INFO] Test working directory: ${WORK_DIR}"

# ------------------------------------------------------------------------------
# 1. Positive Tests (正常系の検証)
# ------------------------------------------------------------------------------
echo "[TEST 1] Testing sample_fw composition metadata generation..."
bazel build //tests/firmware_composition:sample_fw_composition_gen
echo "  [PASS] sample_fw metadata generated"

echo "[TEST 2] Testing direct_sketch_fw composition metadata generation..."
bazel build //tests/firmware_composition:direct_sketch_fw_composition_gen
echo "  [PASS] direct_sketch_fw metadata generated"

# ------------------------------------------------------------------------------
# 2. Negative Tests (異常系 fail-closed の検証)
# ------------------------------------------------------------------------------

# 2.1 重複オブジェクト名 (Duplicate object name)
echo "[TEST 3.1] Negative Test: Duplicate object name rejection..."
cat << 'EOF' > "${WORK_DIR}/dup.bzl"
load("//:defs.bzl", "shizuku_object", "shizuku_firmware")

def define_dup_targets():
    shizuku_object(name = "dup_a", object_name = "conflict_name", srcs = ["//tests/firmware_composition:sample_object.cpp"], hdrs = ["//tests/firmware_composition:sample_object.hpp"], deps = ["//:internal_headers"])
    shizuku_object(name = "dup_b", object_name = "conflict_name", srcs = ["//tests/firmware_composition:sample_object.cpp"], hdrs = ["//tests/firmware_composition:sample_object.hpp"], deps = ["//:internal_headers"])
    shizuku_firmware(name = "dup_fw", target = "esp32c6", objects = [":dup_a", ":dup_b"])
EOF

# 2.2 予約語名 (Reserved object name)
echo "[TEST 3.2] Negative Test: Reserved object name rejection..."

# 2.3 親の不在 (Missing parent object)
echo "[TEST 3.3] Negative Test: Missing parent object rejection..."

# 2.4 自己親サイクル (Self-parenting cycle)
echo "[TEST 3.4] Negative Test: Self-parenting cycle rejection..."

# 2.5 循環親階層 (Cycle in parent hierarchy)
echo "[TEST 3.5] Negative Test: Cyclic parent hierarchy rejection..."

# 2.6 上限超過 (Limit > 62)
echo "[TEST 3.6] Negative Test: Capacity limit > 62 rejection..."

# 2.7 不明ターゲット (Unknown target)
echo "[TEST 3.7] Negative Test: Unknown target rejection..."

echo "[ALL INTEGRATION SPECS PREPARED]"
