#!/usr/bin/env bash
set -euo pipefail

# Fail-closed validation test for Shizuku composition API (defs.bzl)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

WORK_DIR="${TEST_TMPDIR:-$(mktemp -d /tmp/shizuku_fail_closed_test.XXXXXX)}"
mkdir -p "${WORK_DIR}"
echo "[INFO] Working directory: ${WORK_DIR}"

# 1. 正常系ターゲットの検証 (archive 生成が成功すること)
echo "[TEST 1] Verifying valid firmware archive build..."
bazel build //tests/composition_link:test_c6_firmware_archive
echo "  [PASS] test_c6_firmware_archive built successfully"

# 2. 正常系スケッチターゲットの検証 (sketch archive 生成が成功すること)
echo "[TEST 2] Verifying valid sketch archive build..."
bazel build //tests/composition_link:test_c6_sketch_archive
echo "  [PASS] test_c6_sketch_archive built successfully"

# 3. 異常系テスト用の一時パッケージ作成
TMP_PKG_DIR="${REPO_ROOT}/tests/composition_link/fail_closed_cases"
mkdir -p "${TMP_PKG_DIR}"
trap "rm -rf '${TMP_PKG_DIR}'" EXIT

# 3.1 重複名 (Duplicate object name)
echo "[TEST 3.1] Testing duplicate object name rejection..."
cat << 'EOF' > "${TMP_PKG_DIR}/BUILD.bazel"
load("//:defs.bzl", "shizuku_object", "shizuku_firmware")

shizuku_object(
    name = "dup_obj_a",
    srcs = ["//tests/composition_link:sample_object.cpp"],
    hdrs = ["//tests/composition_link:sample_object.hpp"],
    deps = ["//:internal_headers"],
)

shizuku_object(
    name = "dup_obj_b",
    object_name = "dup_obj_a", # 意図的な重複名
    srcs = ["//tests/composition_link:sample_object.cpp"],
    hdrs = ["//tests/composition_link:sample_object.hpp"],
    deps = ["//:internal_headers"],
)

shizuku_firmware(
    name = "dup_fw",
    target = "esp32c6",
    objects = [":dup_obj_a", ":dup_obj_b"],
)
EOF

if bazel build //tests/composition_link/fail_closed_cases:dup_fw_composition_gen > "${WORK_DIR}/dup.log" 2>&1; then
    echo "  [FAIL] Duplicate object name should have been rejected!" >&2
    exit 1
else
    if grep -q "Duplicate object name" "${WORK_DIR}/dup.log"; then
        echo "  [PASS] Duplicate object correctly rejected with error message"
    else
        echo "  [FAIL] Unexpected error message:" >&2
        cat "${WORK_DIR}/dup.log" >&2
        exit 1
    fi
fi

# 3.2 予約語名 (Reserved object name)
echo "[TEST 3.2] Testing reserved object name rejection..."
cat << 'EOF' > "${TMP_PKG_DIR}/BUILD.bazel"
load("//:defs.bzl", "shizuku_object", "shizuku_firmware")

shizuku_object(
    name = "root", # 予約語
    srcs = ["//tests/composition_link:sample_object.cpp"],
    hdrs = ["//tests/composition_link:sample_object.hpp"],
    deps = ["//:internal_headers"],
)

shizuku_firmware(
    name = "reserved_fw",
    target = "esp32c6",
    objects = [":root"],
)
EOF

if bazel build //tests/composition_link/fail_closed_cases:reserved_fw_composition_gen > "${WORK_DIR}/reserved.log" 2>&1; then
    echo "  [FAIL] Reserved object name should have been rejected!" >&2
    exit 1
else
    if grep -q "Reserved object name" "${WORK_DIR}/reserved.log"; then
        echo "  [PASS] Reserved object correctly rejected with error message"
    else
        echo "  [FAIL] Unexpected error message:" >&2
        cat "${WORK_DIR}/reserved.log" >&2
        exit 1
    fi
fi

# 3.3 親の不在 (Missing parent object)
echo "[TEST 3.3] Testing missing parent object rejection..."
cat << 'EOF' > "${TMP_PKG_DIR}/BUILD.bazel"
load("//:defs.bzl", "shizuku_object", "shizuku_firmware")

shizuku_object(
    name = "child_obj",
    parent = "non_existent_parent",
    srcs = ["//tests/composition_link:sample_object.cpp"],
    hdrs = ["//tests/composition_link:sample_object.hpp"],
    deps = ["//:internal_headers"],
)

shizuku_firmware(
    name = "missing_parent_fw",
    target = "esp32c6",
    objects = [":child_obj"],
)
EOF

if bazel build //tests/composition_link/fail_closed_cases:missing_parent_fw_composition_gen > "${WORK_DIR}/missing_parent.log" 2>&1; then
    echo "  [FAIL] Missing parent should have been rejected!" >&2
    exit 1
else
    if grep -q "Missing parent object" "${WORK_DIR}/missing_parent.log"; then
        echo "  [PASS] Missing parent correctly rejected with error message"
    else
        echo "  [FAIL] Unexpected error message:" >&2
        cat "${WORK_DIR}/missing_parent.log" >&2
        exit 1
    fi
fi

echo "[ALL TESTS PASSED] Fail-closed validation complete."
