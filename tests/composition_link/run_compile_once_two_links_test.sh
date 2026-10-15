#!/usr/bin/env bash
set -euo pipefail

# compile-once, two-links host test script
# 同一 toolchain / chip (host) で sample_object.o を 1 回だけコンパイルし、
# 異なる 2 つの合成 ID 定義 (Alpha / Beta) とそれぞれリンクして、
# 期待通りの ID が得られることを検証する。

# REPO_ROOT の解決 (Bazel runfiles 環境と通常ローカル環境の両立)
if [ -n "${RUNFILES_DIR:-}" ] && [ -d "${RUNFILES_DIR}/_main" ]; then
  REPO_ROOT="${RUNFILES_DIR}/_main"
elif [ -n "${TEST_SRCDIR:-}" ] && [ -d "${TEST_SRCDIR}/_main" ]; then
  REPO_ROOT="${TEST_SRCDIR}/_main"
else
  SCRIPT_DIR_DETECTED="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
  REPO_ROOT="$(cd "${SCRIPT_DIR_DETECTED}/../.." && pwd)"
fi

SCRIPT_DIR="${REPO_ROOT}/tests/composition_link"

WORK_DIR="${TEST_TMPDIR:-$(mktemp -d /tmp/shizuku_comp_test.XXXXXX)}"
mkdir -p "${WORK_DIR}"
echo "[INFO] Working directory: ${WORK_DIR}"
echo "[INFO] Repository root: ${REPO_ROOT}"

CXX="${CXX:-clang++}"
# C++23 を標準採用 (constexpr 緩和、属性拡張、シンボル解決の最新規格)
CXXFLAGS=("-std=c++23" "-Wall" "-Wextra" "-I${REPO_ROOT}")

# 1. 合成 Alpha および Beta の ID 定義ソースを生成 (argv 配列実行)
echo "[STEP 1] Generating composition ID definitions via gen_object_ids.py"
GEN_CMD_ALPHA=(
  python3 "${REPO_ROOT}/tools/gen_object_ids.py"
  --mode extern
  --source-template "${REPO_ROOT}/tools/object_ids.cpp.in"
  --source-out "${WORK_DIR}/composition_alpha.cpp"
  "${SCRIPT_DIR}/composition_alpha.list"
)
"${GEN_CMD_ALPHA[@]}"

GEN_CMD_BETA=(
  python3 "${REPO_ROOT}/tools/gen_object_ids.py"
  --mode extern
  --source-template "${REPO_ROOT}/tools/object_ids.cpp.in"
  --source-out "${WORK_DIR}/composition_beta.cpp"
  "${SCRIPT_DIR}/composition_beta.list"
)
"${GEN_CMD_BETA[@]}"

# 2. sample_object.cpp を 1 度だけコンパイル (argv 配列実行)
echo "[STEP 2] Compiling sample_object.cpp (EXACTLY ONCE)"
COMPILE_CMD=(
  "${CXX}"
  "${CXXFLAGS[@]}"
  -c "${SCRIPT_DIR}/sample_object.cpp"
  -o "${WORK_DIR}/sample_object.o"
)
echo "  Command: ${COMPILE_CMD[*]}"
"${COMPILE_CMD[@]}"

# sample_object.o の SHA256 ハッシュを計算・記録
HASH_ORIGINAL=$(shasum -a 256 "${WORK_DIR}/sample_object.o" | awk '{print $1}')
echo "  [HASH] sample_object.o sha256 = ${HASH_ORIGINAL}"

# 2.1 nm で sample_object.o 内のシンボル参照を厳格に assert (U 参照のみであり、定義を持たないこと)
echo "[STEP 2.1] Asserting undefined symbols in sample_object.o via nm"
NM_SAMPLE=$(nm -g "${WORK_DIR}/sample_object.o")
echo "${NM_SAMPLE}"

# sample_service が未定義シンボル (U) であることを assert
if ! echo "${NM_SAMPLE}" | grep -E "[[:space:]]+U[[:space:]]+.*sample_service" > /dev/null; then
  echo "[ASSERT FAILED] sample_object.o does not have undefined symbol U for sample_service" >&2
  exit 1
fi
# target_service が未定義シンボル (U) であることを assert
if ! echo "${NM_SAMPLE}" | grep -E "[[:space:]]+U[[:space:]]+.*target_service" > /dev/null; then
  echo "[ASSERT FAILED] sample_object.o does not have undefined symbol U for target_service" >&2
  exit 1
fi
# sample_object.o 内に ID の実体定義シンボル (T, D, R, S) が存在しないことを assert
if echo "${NM_SAMPLE}" | grep -E "[[:space:]]+[TDRS][[:space:]]+.*(sample_service|target_service)" > /dev/null; then
  echo "[ASSERT FAILED] sample_object.o unexpectedly defines sample_service or target_service directly!" >&2
  exit 1
fi
echo "  [ASSERT PASSED] sample_object.o references symbols strictly as undefined (U)"

# 3. test_runner.cpp をコンパイル
echo "[STEP 3] Compiling test_runner.cpp"
COMPILE_RUNNER_CMD=(
  "${CXX}"
  "${CXXFLAGS[@]}"
  -c "${SCRIPT_DIR}/test_runner.cpp"
  -o "${WORK_DIR}/test_runner.o"
)
"${COMPILE_RUNNER_CMD[@]}"

# 4. 合成 Alpha 定義をコンパイル
echo "[STEP 4] Compiling composition_alpha.cpp"
COMPILE_ALPHA_CMD=(
  "${CXX}"
  "${CXXFLAGS[@]}"
  -c "${WORK_DIR}/composition_alpha.cpp"
  -o "${WORK_DIR}/composition_alpha.o"
)
"${COMPILE_ALPHA_CMD[@]}"

# 4.1 nm & section で composition_alpha.o を厳格に assert (定義シンボル保有 + 動的初期化不在)
echo "[STEP 4.1] Asserting defined symbols and absence of dynamic constructors in composition_alpha.o"
NM_ALPHA=$(nm -g "${WORK_DIR}/composition_alpha.o")
echo "${NM_ALPHA}"

# Alpha に未定義シンボル (U) が残っていないことを assert
if echo "${NM_ALPHA}" | grep -E "[[:space:]]+U[[:space:]]+" > /dev/null; then
  echo "[ASSERT FAILED] composition_alpha.o unexpectedly contains undefined symbols" >&2
  exit 1
fi
# Alpha が sample_service / target_service の定義シンボルを持つことを assert
if ! echo "${NM_ALPHA}" | grep -E ".*sample_service" > /dev/null; then
  echo "[ASSERT FAILED] composition_alpha.o missing sample_service definition" >&2
  exit 1
fi

# 動的初期化関数の不在を assert (Constant Initialization の証明)
if echo "${NM_ALPHA}" | grep -E "(_GLOBAL__sub_I|__static_initialization)" > /dev/null; then
  echo "[ASSERT FAILED] composition_alpha.o contains dynamic initialization functions!" >&2
  exit 1
fi
# Mach-O の場合: __mod_init_func セクション不在を assert
if command -v otool >/dev/null 2>&1; then
  if otool -l "${WORK_DIR}/composition_alpha.o" | grep -i "__mod_init_func" > /dev/null; then
    echo "[ASSERT FAILED] composition_alpha.o contains __mod_init_func section!" >&2
    exit 1
  fi
fi
echo "  [ASSERT PASSED] composition_alpha.o defines symbols statically with zero runtime constructors"

# 5. リンク 1: sample_object.o + composition_alpha.o + test_runner.o -> test_alpha
echo "[STEP 5] Linking test_alpha (Link #1)"
LINK_CMD_ALPHA=(
  "${CXX}"
  "${WORK_DIR}/sample_object.o"
  "${WORK_DIR}/composition_alpha.o"
  "${WORK_DIR}/test_runner.o"
  -o "${WORK_DIR}/test_alpha"
)
echo "  Command: ${LINK_CMD_ALPHA[*]}"
"${LINK_CMD_ALPHA[@]}"

# 6. ハッシュ再確認 (sample_object.o が改変・再コンパイルされていないことの検証)
HASH_CHECK_1=$(shasum -a 256 "${WORK_DIR}/sample_object.o" | awk '{print $1}')
if [ "${HASH_ORIGINAL}" != "${HASH_CHECK_1}" ]; then
  echo "[ERROR] sample_object.o hash mismatch before link 2! Original: ${HASH_ORIGINAL}, Current: ${HASH_CHECK_1}" >&2
  exit 1
fi
echo "  [HASH VERIFIED] sample_object.o hash unchanged: ${HASH_CHECK_1}"

# 7. 合成 Beta 定義をコンパイル
echo "[STEP 6] Compiling composition_beta.cpp"
COMPILE_BETA_CMD=(
  "${CXX}"
  "${CXXFLAGS[@]}"
  -c "${WORK_DIR}/composition_beta.cpp"
  -o "${WORK_DIR}/composition_beta.o"
)
"${COMPILE_BETA_CMD[@]}"

# 8. リンク 2: 同一の sample_object.o + composition_beta.o + test_runner.o -> test_beta
echo "[STEP 7] Linking test_beta (Link #2 with identical sample_object.o)"
LINK_CMD_BETA=(
  "${CXX}"
  "${WORK_DIR}/sample_object.o"
  "${WORK_DIR}/composition_beta.o"
  "${WORK_DIR}/test_runner.o"
  -o "${WORK_DIR}/test_beta"
)
echo "  Command: ${LINK_CMD_BETA[*]}"
"${LINK_CMD_BETA[@]}"

# ハッシュ最終確認
HASH_CHECK_2=$(shasum -a 256 "${WORK_DIR}/sample_object.o" | awk '{print $1}')
if [ "${HASH_ORIGINAL}" != "${HASH_CHECK_2}" ]; then
  echo "[ERROR] sample_object.o hash mismatch after link 2! Original: ${HASH_ORIGINAL}, Current: ${HASH_CHECK_2}" >&2
  exit 1
fi
echo "  [HASH VERIFIED] sample_object.o hash remained identical across both links: ${HASH_CHECK_2}"

# 9. 実行検証 (test_runner.cpp の 6 つの assert × 2 合成)
echo "[STEP 8] Executing test_alpha (Expected: sample=2, target=3, count=5)"
"${WORK_DIR}/test_alpha" 2 3 5

echo "[STEP 9] Executing test_beta (Expected: sample=4, target=5, count=6)"
"${WORK_DIR}/test_beta" 4 5 6

# 10. --mode=constexpr 契約の実証テスト (--source-out なしでもリンク成立することの証明)
echo "[STEP 10] Testing --mode=constexpr header-only contract without --source-out"
GEN_CMD_CONSTEXPR=(
  python3 "${REPO_ROOT}/tools/gen_object_ids.py"
  --mode constexpr
  --template "${REPO_ROOT}/tools/object_ids.hpp.in"
  --out "${WORK_DIR}/constexpr_object_ids.hpp"
  --names obj_first obj_second
)
"${GEN_CMD_CONSTEXPR[@]}"

cat << 'EOF' > "${WORK_DIR}/test_constexpr_consumer.cpp"
#include <cassert>
#include <cstdint>
// 生成されたヘッダーのみをインクルード (ソース定義 .cpp のリンクなし)
#include "constexpr_object_ids.hpp"

int main() {
  static_assert(shizuku::object_id::obj_first == 2, "constexpr id must match 2");
  static_assert(shizuku::object_id::obj_second == 3, "constexpr id must match 3");
  assert(shizuku::object_id::obj_first == 2);
  assert(shizuku::object_id::obj_second == 3);
  return 0;
}
EOF

COMPILE_CONSTEXPR_CMD=(
  "${CXX}"
  "${CXXFLAGS[@]}"
  -I"${WORK_DIR}"
  "${WORK_DIR}/test_constexpr_consumer.cpp"
  -o "${WORK_DIR}/test_constexpr_bin"
)
"${COMPILE_CONSTEXPR_CMD[@]}"
"${WORK_DIR}/test_constexpr_bin"
echo "  [ASSERT PASSED] --mode=constexpr builds and passes with header-only (zero source definitions needed)"

echo "=========================================================="
echo "SUCCESS: Compile-once, two-links & constexpr tests verified completely!"
echo "Object hash: ${HASH_ORIGINAL}"
echo "=========================================================="
