#!/usr/bin/env bash
set -euo pipefail

# Fail-closed and Positive Validation Test for Shizuku Composition API
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

# unique /private/tmp fixture and output root
WORK_DIR="$(mktemp -d /private/tmp/shizuku_fc_fixture.XXXXXX)"
BAZEL_OUT_ROOT="$(mktemp -d /private/tmp/shizuku_fc_bazel_root.XXXXXX)"
echo "[INFO] Test working directory: ${WORK_DIR}"
echo "[INFO] Isolated Bazel output root: ${BAZEL_OUT_ROOT}"

# ------------------------------------------------------------------------------
# 1. Positive Tests (正常系の検証)
# ------------------------------------------------------------------------------
echo "[TEST 1] Testing sample_fw composition metadata generation..."
bazel --output_user_root="${BAZEL_OUT_ROOT}" build //tests/firmware_composition:sample_fw_composition_gen
echo "  [PASS] sample_fw metadata generated"

echo "[TEST 2] Testing direct_sketch_fw composition metadata generation..."
bazel --output_user_root="${BAZEL_OUT_ROOT}" build //tests/firmware_composition:direct_sketch_fw_composition_gen
echo "  [PASS] direct_sketch_fw metadata generated"

# ------------------------------------------------------------------------------
# 2. Negative Tests (異常系 fail-closed の検証)
# ------------------------------------------------------------------------------
# Set up independent external scratch workspace in WORK_DIR
cat << EOF > "${WORK_DIR}/MODULE.bazel"
module(name = "shizuku_fail_closed_test")
bazel_dep(name = "shizuku")
local_path_override(
    module_name = "shizuku",
    path = "${REPO_ROOT}",
)
EOF

echo "8.3.1" > "${WORK_DIR}/.bazelversion"
echo "int dummy_val = 0;" > "${WORK_DIR}/dummy.cpp"

cat << 'EOF' > "${WORK_DIR}/BUILD.bazel"
load("@shizuku//:defs.bzl", "shizuku_object", "shizuku_firmware")

# Case 1: Duplicate object name
shizuku_object(name = "dup_a", object_name = "conflict_name", srcs = ["dummy.cpp"])
shizuku_object(name = "dup_b", object_name = "conflict_name", srcs = ["dummy.cpp"])
shizuku_firmware(name = "dup_fw", target = "esp32c6", objects = [":dup_a", ":dup_b"])

# Case 2: Reserved object name
shizuku_object(name = "res_obj", object_name = "kernel", srcs = ["dummy.cpp"])
shizuku_firmware(name = "reserved_fw", target = "esp32c6", objects = [":res_obj"])

# Case 3: Missing parent object
shizuku_object(name = "orphan_obj", object_name = "child_obj", srcs = ["dummy.cpp"])
shizuku_firmware(name = "missing_parent_fw", target = "esp32c6", objects = [":orphan_obj"], parents = {"child_obj": "no_such_parent"})

# Case 4: Self-parenting cycle
shizuku_object(name = "self_obj", object_name = "self_obj", srcs = ["dummy.cpp"])
shizuku_firmware(name = "self_parent_fw", target = "esp32c6", objects = [":self_obj"], parents = {"self_obj": "self_obj"})

# Case 5: Cyclic parent hierarchy
shizuku_object(name = "cyc_a", object_name = "cyc_a", srcs = ["dummy.cpp"])
shizuku_object(name = "cyc_b", object_name = "cyc_b", srcs = ["dummy.cpp"])
shizuku_firmware(name = "cyclic_parent_fw", target = "esp32c6", objects = [":cyc_a", ":cyc_b"], parents = {"cyc_a": "cyc_b", "cyc_b": "cyc_a"})

# Case 6: Capacity limit > 62
[
    shizuku_object(name = "over_obj_%d" % i, object_name = "obj_%d" % i, srcs = ["dummy.cpp"])
    for i in range(63)
]
shizuku_firmware(name = "over_limit_fw", target = "esp32c6", objects = [":over_obj_%d" % i for i in range(63)])

# Case 7: Unknown target
shizuku_object(name = "unk_obj", object_name = "valid_name", srcs = ["dummy.cpp"])
shizuku_firmware(name = "unknown_target_fw", target = "esp32_unknown_chip", objects = [":unk_obj"])
EOF

# Helper function to run assertion
assert_fail_closed() {
    local test_name="$1"
    local target="$2"
    local expected_pattern="$3"
    local log_file="${WORK_DIR}/${target}.log"

    echo "Running ${test_name} (target: //:${target})..."
    if (cd "${WORK_DIR}" && bazel --output_user_root="${BAZEL_OUT_ROOT}" build "//:${target}" > "${log_file}" 2>&1); then
        echo "  [FAIL] Expected target //:${target} to fail, but it succeeded!" >&2
        cat "${log_file}" >&2
        exit 1
    fi

    if grep -E "${expected_pattern}" "${log_file}" > /dev/null; then
        echo "  [PASS] Correctly rejected: found pattern '${expected_pattern}'"
    else
        echo "  [FAIL] Expected pattern '${expected_pattern}' not found in log:" >&2
        cat "${log_file}" >&2
        exit 1
    fi
}

# 2.1 重複オブジェクト名 (Duplicate object name)
assert_fail_closed "[TEST 3.1] Duplicate object name rejection" "dup_fw" "Duplicate object name: 'conflict_name'"

# 2.2 予約語名 (Reserved object name)
assert_fail_closed "[TEST 3.2] Reserved object name rejection" "reserved_fw" "Reserved object name: 'kernel' is not allowed"

# 2.3 親の不在 (Missing parent object)
assert_fail_closed "[TEST 3.3] Missing parent object rejection" "missing_parent_fw" "Missing parent object 'no_such_parent' for object 'child_obj'"

# 2.4 自己親サイクル (Self-parenting cycle)
assert_fail_closed "[TEST 3.4] Self-parenting cycle rejection" "self_parent_fw" "Self-parenting cycle detected for object 'self_obj'"

# 2.5 循環親階層 (Cycle in parent hierarchy)
assert_fail_closed "[TEST 3.5] Cyclic parent hierarchy rejection" "cyclic_parent_fw" "Cycle detected in parent hierarchy for object"

# 2.6 上限超過 (Limit > 62)
assert_fail_closed "[TEST 3.6] Capacity limit > 62 rejection" "over_limit_fw" "Object count exceeds capacity limit \(62\): got 63"

# 2.7 不明ターゲット (Unknown target)
assert_fail_closed "[TEST 3.7] Unknown target rejection" "unknown_target_fw_composition_gen" "Unknown target: 'esp32_unknown_chip'"

echo "[ALL ASSERTIONS PASSED: 2 POSITIVE, 7 NEGATIVE FAIL-CLOSED CASES VERIFIED]"

