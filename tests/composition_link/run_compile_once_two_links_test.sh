#!/usr/bin/env bash
set -euo pipefail

# compile-once, two-links host test script
# 同一 toolchain / chip (host) で sample_object.o を 1 回だけコンパイルし、
# 異なる 2 つの合成 ID 定義 (Alpha / Beta) とそれぞれリンクして、
# 期待通りの ID が得られることを検証する。

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

WORK_DIR="${TEST_TMPDIR:-$(mktemp -d /tmp/shizuku_comp_test.XXXXXX)}"
mkdir -p "${WORK_DIR}"
echo "[INFO] Working directory: ${WORK_DIR}"

CXX="${CXX:-clang++}"
CXXFLAGS="-std=c++20 -Wall -Wextra -I${REPO_ROOT}"

# 1. 合成 Alpha および Beta の ID 定義ソースを生成
echo "[STEP 1] Generating composition ID definitions via gen_object_ids.py"
python3 "${REPO_ROOT}/tools/gen_object_ids.py" \
  --mode extern \
  --source-template "${REPO_ROOT}/tools/object_ids.cpp.in" \
  --source-out "${WORK_DIR}/composition_alpha.cpp" \
  "${SCRIPT_DIR}/composition_alpha.list"

python3 "${REPO_ROOT}/tools/gen_object_ids.py" \
  --mode extern \
  --source-template "${REPO_ROOT}/tools/object_ids.cpp.in" \
  --source-out "${WORK_DIR}/composition_beta.cpp" \
  "${SCRIPT_DIR}/composition_beta.list"

# 2. sample_object.cpp を 1 度だけコンパイル
echo "[STEP 2] Compiling sample_object.cpp (EXACTLY ONCE)"
COMPILE_CMD="${CXX} ${CXXFLAGS} -c ${SCRIPT_DIR}/sample_object.cpp -o ${WORK_DIR}/sample_object.o"
echo "  Command: ${COMPILE_CMD}"
eval "${COMPILE_CMD}"

# sample_object.o の SHA256 ハッシュを計算・記録
HASH_ORIGINAL=$(shasum -a 256 "${WORK_DIR}/sample_object.o" | awk '{print $1}')
echo "  [HASH] sample_object.o sha256 = ${HASH_ORIGINAL}"

# nm で sample_object.o 内のシンボル参照を確認 (未定義 U シンボルとして参照されていること)
echo "[STEP 2.1] Inspecting symbols in sample_object.o with nm"
nm -g "${WORK_DIR}/sample_object.o" | tee "${WORK_DIR}/nm_sample_object.txt"

# 3. test_runner.cpp をコンパイル
echo "[STEP 3] Compiling test_runner.cpp"
eval "${CXX} ${CXXFLAGS} -c ${SCRIPT_DIR}/test_runner.cpp -o ${WORK_DIR}/test_runner.o"

# 4. 合成 Alpha 定義をコンパイル
echo "[STEP 4] Compiling composition_alpha.cpp"
eval "${CXX} ${CXXFLAGS} -c ${WORK_DIR}/composition_alpha.cpp -o ${WORK_DIR}/composition_alpha.o"

# nm で composition_alpha.o 内のシンボルを確認
echo "[STEP 4.1] Inspecting symbols in composition_alpha.o with nm"
nm -g "${WORK_DIR}/composition_alpha.o" | tee "${WORK_DIR}/nm_composition_alpha.txt"

# 5. リンク 1: sample_object.o + composition_alpha.o + test_runner.o -> test_alpha
echo "[STEP 5] Linking test_alpha (Link #1)"
LINK_CMD_ALPHA="${CXX} ${WORK_DIR}/sample_object.o ${WORK_DIR}/composition_alpha.o ${WORK_DIR}/test_runner.o -o ${WORK_DIR}/test_alpha"
echo "  Command: ${LINK_CMD_ALPHA}"
eval "${LINK_CMD_ALPHA}"

# 6. ハッシュ再確認 (sample_object.o が改変・再コンパイルされていないことの検証)
HASH_CHECK_1=$(shasum -a 256 "${WORK_DIR}/sample_object.o" | awk '{print $1}')
if [ "${HASH_ORIGINAL}" != "${HASH_CHECK_1}" ]; then
  echo "[ERROR] sample_object.o hash mismatch before link 2! Original: ${HASH_ORIGINAL}, Current: ${HASH_CHECK_1}" >&2
  exit 1
fi
echo "  [HASH VERIFIED] sample_object.o hash unchanged: ${HASH_CHECK_1}"

# 7. 合成 Beta 定義をコンパイル
echo "[STEP 6] Compiling composition_beta.cpp"
eval "${CXX} ${CXXFLAGS} -c ${WORK_DIR}/composition_beta.cpp -o ${WORK_DIR}/composition_beta.o"

# nm で composition_beta.o 内のシンボルを確認
echo "[STEP 6.1] Inspecting symbols in composition_beta.o with nm"
nm -g "${WORK_DIR}/composition_beta.o" | tee "${WORK_DIR}/nm_composition_beta.txt"

# 8. リンク 2: 同一の sample_object.o + composition_beta.o + test_runner.o -> test_beta
echo "[STEP 7] Linking test_beta (Link #2 with identical sample_object.o)"
LINK_CMD_BETA="${CXX} ${WORK_DIR}/sample_object.o ${WORK_DIR}/composition_beta.o ${WORK_DIR}/test_runner.o -o ${WORK_DIR}/test_beta"
echo "  Command: ${LINK_CMD_BETA}"
eval "${LINK_CMD_BETA}"

# ハッシュ最終確認
HASH_CHECK_2=$(shasum -a 256 "${WORK_DIR}/sample_object.o" | awk '{print $1}')
if [ "${HASH_ORIGINAL}" != "${HASH_CHECK_2}" ]; then
  echo "[ERROR] sample_object.o hash mismatch after link 2! Original: ${HASH_ORIGINAL}, Current: ${HASH_CHECK_2}" >&2
  exit 1
fi
echo "  [HASH VERIFIED] sample_object.o hash remained identical across both links: ${HASH_CHECK_2}"

# 9. 実行検証
echo "[STEP 8] Executing test_alpha (Expected: sample=2, target=3, count=5)"
"${WORK_DIR}/test_alpha" 2 3 5

echo "[STEP 9] Executing test_beta (Expected: sample=4, target=5, count=6)"
"${WORK_DIR}/test_beta" 4 5 6

echo "=========================================================="
echo "SUCCESS: Compile-once, two-links test verified completely!"
echo "Object hash: ${HASH_ORIGINAL}"
echo "=========================================================="
