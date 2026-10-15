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

CC="${CC:-clang}"
CXX="${CXX:-clang++}"
# C17 および C++23 を標準採用
CFLAGS=("-std=c17" "-Wall" "-Wextra" "-I${REPO_ROOT}")
CXXFLAGS=("-std=c++23" "-Wall" "-Wextra" "-I${REPO_ROOT}")

# ヘルパー関数: fail-closed な動的初期化セクション不在検査
assert_zero_dynamic_init() {
  local obj_file="$1"
  local label="$2"

  echo "  [INSPECT] Checking dynamic initialization in ${label} (${obj_file})"

  if [ ! -f "${obj_file}" ]; then
    echo "[ASSERT FAILED] Target object file does not exist: ${obj_file}" >&2
    return 1
  fi

  # 1. nm によるコンストラクタマーカーシンボルの検査
  local nm_symbols
  if ! nm_symbols=$(nm "${obj_file}" 2>/dev/null); then
    echo "[ASSERT FAILED] nm command failed on ${obj_file}" >&2
    return 1
  fi

  if echo "${nm_symbols}" | grep -E "(_GLOBAL__sub_I|__static_initialization|___cxx_global_var_init)" > /dev/null; then
    echo "[ASSERT FAILED] ${label} contains dynamic initialization functions in nm symbols!" >&2
    return 1
  fi

  # 2. オブジェクトファイル形式の判定とセクション検査
  local file_info
  if ! file_info=$(file -b "${obj_file}" 2>/dev/null); then
    echo "[ASSERT FAILED] file command failed on ${obj_file}" >&2
    return 1
  fi

  local inspected=0

  if echo "${file_info}" | grep -E -i "Mach-O" > /dev/null; then
    # Mach-O 形式: otool -l を使用
    if ! command -v otool >/dev/null 2>&1; then
      echo "[ASSERT FAILED] Object is Mach-O but otool is not available!" >&2
      return 1
    fi

    local otool_out
    if ! otool_out=$(otool -l "${obj_file}" 2>/dev/null); then
      echo "[ASSERT FAILED] otool -l failed on ${obj_file}" >&2
      return 1
    fi

    # セクション情報が正常に取得できていることを確認 (Section / sectname の存在)
    if ! echo "${otool_out}" | grep -E "sectname" > /dev/null; then
      echo "[ASSERT FAILED] otool -l output contains no sections for ${obj_file}" >&2
      return 1
    fi

    if echo "${otool_out}" | grep -E -i "sectname[[:space:]]+(__mod_init_func|__mod_term_func|__StaticInit)" > /dev/null; then
      echo "[ASSERT FAILED] ${label} contains Mach-O init section (__mod_init_func/__StaticInit)!" >&2
      return 1
    fi

    inspected=1

  elif echo "${file_info}" | grep -E -i "ELF" > /dev/null; then
    # ELF 形式: readelf または llvm-readelf を使用
    local elf_tool=""
    if command -v readelf >/dev/null 2>&1; then
      elf_tool="readelf"
    elif command -v llvm-readelf >/dev/null 2>&1; then
      elf_tool="llvm-readelf"
    fi

    if [ -z "${elf_tool}" ]; then
      echo "[ASSERT FAILED] Object is ELF but neither readelf nor llvm-readelf is available!" >&2
      return 1
    fi

    local readelf_out
    if ! readelf_out=$("${elf_tool}" -S "${obj_file}" 2>/dev/null); then
      echo "[ASSERT FAILED] ${elf_tool} -S failed on ${obj_file}" >&2
      return 1
    fi

    # セクションヘッダが正常に取得できていることを確認
    if ! echo "${readelf_out}" | grep -E "Section Headers:" > /dev/null; then
      echo "[ASSERT FAILED] ${elf_tool} output contains no Section Headers for ${obj_file}" >&2
      return 1
    fi

    if echo "${readelf_out}" | grep -E "(\.init_array|\.ctors)" > /dev/null; then
      echo "[ASSERT FAILED] ${label} contains ELF init section (.init_array/.ctors)!" >&2
      return 1
    fi

    inspected=1

  else
    echo "[ASSERT FAILED] Unknown or unsupported object file format: ${file_info}" >&2
    return 1
  fi

  if [ "${inspected}" -ne 1 ]; then
    echo "[ASSERT FAILED] Section inspection was not completed for ${label} (inspected=${inspected})!" >&2
    return 1
  fi

  echo "  [ASSERT PASSED] ${label} has zero dynamic initialization functions and zero ctor sections (inspected via ${file_info})"
  return 0
}

# 1. 合成 Alpha および Beta の ID 定義ソース (.c) とヘッダー (.hpp) を生成 (純粋 C template 使用)
echo "[STEP 1] Generating composition ID definitions via gen_object_ids.py"
GEN_CMD_ALPHA=(
  python3 "${REPO_ROOT}/tools/gen_object_ids.py"
  --mode extern
  --template "${REPO_ROOT}/tools/object_ids.hpp.in"
  --out "${WORK_DIR}/composition_alpha.hpp"
  --source-template "${REPO_ROOT}/tools/object_ids.c.in"
  --source-out "${WORK_DIR}/composition_alpha.c"
  "${SCRIPT_DIR}/composition_alpha.list"
)
"${GEN_CMD_ALPHA[@]}"

GEN_CMD_BETA=(
  python3 "${REPO_ROOT}/tools/gen_object_ids.py"
  --mode extern
  --template "${REPO_ROOT}/tools/object_ids.hpp.in"
  --out "${WORK_DIR}/composition_beta.hpp"
  --source-template "${REPO_ROOT}/tools/object_ids.c.in"
  --source-out "${WORK_DIR}/composition_beta.c"
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
if echo "${NM_SAMPLE}" | grep -E "[[:space:]]+[TDRS][[:space:]]+.*(shizuku_object_id_sample_service|shizuku_object_id_target_service)" > /dev/null; then
  echo "[ASSERT FAILED] sample_object.o unexpectedly defines sample_service or target_service directly!" >&2
  exit 1
fi
echo "  [ASSERT PASSED] sample_object.o references symbols strictly as undefined (U)"

# 2.2 consumer 側 (sample_object.o) の動的初期化 (ctor / init_array / __mod_init_func) 不在を機械 assert
echo "[STEP 2.2] Asserting zero runtime dynamic constructors in consumer sample_object.o"
assert_zero_dynamic_init "${WORK_DIR}/sample_object.o" "consumer sample_object.o"

# 2.3 Negative fixture の検証 (旧値コピー global consumer で動的初期化が発生し、checker が検出・失敗することの実証)
echo "[STEP 2.3] Negative fixture: verifying that value-copy global consumer produces dynamic init and checker fails closed"
cat << 'EOF' > "${WORK_DIR}/test_negative_value_copy.cpp"
#include <cstdint>
extern "C" const uintptr_t shizuku_object_id_sample_service;
namespace shizuku::object_id {
  inline constexpr const uintptr_t &sample_service = shizuku_object_id_sample_service;
}

// 旧値コピー global consumer: extern const シンボルの値でグローバル変数を初期化するため動的初期化ルーチンが必須となる
const uintptr_t GLOBAL_VALUE_COPY = shizuku::object_id::sample_service;

uintptr_t get_negative_val() {
  return GLOBAL_VALUE_COPY;
}
EOF

COMPILE_NEGATIVE_CMD=(
  "${CXX}"
  "${CXXFLAGS[@]}"
  -c "${WORK_DIR}/test_negative_value_copy.cpp"
  -o "${WORK_DIR}/test_negative_value_copy.o"
)
"${COMPILE_NEGATIVE_CMD[@]}"

# negative fixture の object に ctor/init section が実際に存在することを確認
NM_NEG=$(nm "${WORK_DIR}/test_negative_value_copy.o" 2>/dev/null || true)
if ! echo "${NM_NEG}" | grep -E "(_GLOBAL__sub_I|___cxx_global_var_init)" > /dev/null; then
  echo "[ASSERT FAILED] Negative fixture object unexpectedly did NOT generate dynamic init symbols!" >&2
  exit 1
fi

# assert_zero_dynamic_init をサブシェルで実行し、必ず失敗 (非ゼロ終了) することを assert
set +e
( assert_zero_dynamic_init "${WORK_DIR}/test_negative_value_copy.o" "negative fixture object" ) > "${WORK_DIR}/negative_check.log" 2>&1
CHECK_STATUS=$?
set -e

if [ "${CHECK_STATUS}" -eq 0 ]; then
  echo "[ASSERT FAILED] assert_zero_dynamic_init unexpectedly succeeded on negative fixture containing dynamic init!" >&2
  cat "${WORK_DIR}/negative_check.log" >&2
  exit 1
fi
echo "  [ASSERT PASSED] Negative fixture correctly failed dynamic init check (exit code ${CHECK_STATUS})"

# 3. test_runner.cpp をコンパイル
echo "[STEP 3] Compiling test_runner.cpp"
COMPILE_RUNNER_CMD=(
  "${CXX}"
  "${CXXFLAGS[@]}"
  -c "${SCRIPT_DIR}/test_runner.cpp"
  -o "${WORK_DIR}/test_runner.o"
)
"${COMPILE_RUNNER_CMD[@]}"

# 4. 合成 Alpha 定義を「純粋 C コンパイラ」でコンパイル
echo "[STEP 4] Compiling composition_alpha.c via C compiler (${CC})"
COMPILE_ALPHA_CMD=(
  "${CC}"
  "${CFLAGS[@]}"
  -c "${WORK_DIR}/composition_alpha.c"
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

assert_zero_dynamic_init "${WORK_DIR}/composition_alpha.o" "definition composition_alpha.o"

# 4.2 C 言語 consumer からの header include / compile & link 実証
echo "[STEP 4.2] Verifying pure C compilation and linkage with object_ids header"
mkdir -p "${WORK_DIR}/shizuku"
cp "${WORK_DIR}/composition_alpha.hpp" "${WORK_DIR}/shizuku/object_ids.hpp"
cat << 'EOF' > "${WORK_DIR}/test_c_consumer.c"
#include <assert.h>
#include <stdint.h>
#include "shizuku/object_ids.hpp"

int main(void) {
  assert(shizuku_object_id_root == 0);
  assert(shizuku_object_id_sample_service == 2);
  assert(shizuku_object_id_target_service == 3);
  assert(shizuku_object_id_count == 5);
  return 0;
}
EOF

COMPILE_C_CONSUMER_CMD=(
  "${CC}"
  "${CFLAGS[@]}"
  -I"${WORK_DIR}"
  "${WORK_DIR}/test_c_consumer.c"
  "${WORK_DIR}/composition_alpha.o"
  -o "${WORK_DIR}/test_c_consumer_bin"
)
"${COMPILE_C_CONSUMER_CMD[@]}"
"${WORK_DIR}/test_c_consumer_bin"
echo "  [ASSERT PASSED] Pure C consumer successfully compiled and linked with C ABI definitions"

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

# 7. 合成 Beta 定義を「純粋 C コンパイラ」でコンパイル
echo "[STEP 6] Compiling composition_beta.c via C compiler (${CC})"
COMPILE_BETA_CMD=(
  "${CC}"
  "${CFLAGS[@]}"
  -c "${WORK_DIR}/composition_beta.c"
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
// 生成されたヘッダーのみをインクルード (ソース定義 .c/.cpp のリンクなし)
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
