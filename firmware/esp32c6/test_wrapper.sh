#!/bin/bash
# firmware/esp32c6/build.sh の出力パス処理と失敗系への assert。
#   使い方: test_wrapper.sh <kernel.a の絶対パス> <作業ディレクトリ (空 or 新規)>
# ★Bazel の genrule は出力を相対パス ($@) で渡す。build.sh が cd した後でも
#   相対 OUT/ARCHIVE が正しい場所を指すこと (positive) と、環境不備・版不一致・
#   アーカイブ欠落で .bin を作らず非 0 で終わること (negative) を見る。
# 前提: IDF_PATH / IDF_TOOLS_PATH を呼び出し側が export 済みであること。
set -u
KERNEL_A=$1
WORK=$2
HERE=$(cd -P "$(dirname "$0")" && pwd)
BUILD_SH=$HERE/build.sh
GCC=esp-15.2.0_20251204
mkdir -p "$WORK/sub"
cd "$WORK" || exit 1
cp "$KERNEL_A" "$WORK/rel_kernel.a"
rm -f "$WORK/out/rel.bin" "$WORK/neg.bin"
FAILS=0

# expect <説明> <期待する exit: 0 か nonzero> <コマンド...>
expect() {
  desc=$1
  want=$2
  shift 2
  "$@" > "$WORK/last.log" 2>&1
  rc=$?
  if [ "$want" = "0" ] && [ "$rc" -eq 0 ]; then
    echo "PASS: $desc (exit=$rc)"
  elif [ "$want" = "nonzero" ] && [ "$rc" -ne 0 ]; then
    echo "PASS: $desc (exit=$rc)"
  else
    echo "FAIL: $desc (exit=$rc, want=$want)" >&2
    FAILS=$((FAILS + 1))
  fi
}

# assert <説明> <test の引数...>
assert() {
  desc=$1
  shift
  if test "$@"; then
    echo "PASS: $desc"
  else
    echo "FAIL: $desc" >&2
    FAILS=$((FAILS + 1))
  fi
}

# ソース側 (build.sh と同じディレクトリ) の sdkconfig / sdkconfig.old の実行前の状態 (無ければ none)
snap() (
  for f in sdkconfig sdkconfig.old; do
    if test -e "$HERE/$f"; then shasum -a 256 "$HERE/$f"; else echo "none $f"; fi
  done
)
BEFORE=$(snap)

# --- positive: 相対 ARCHIVE と、まだ存在しないディレクトリ配下の相対 OUT
expect "P1 relative ARCHIVE + relative OUT (nonexistent dir) builds" 0 \
  bash "$BUILD_SH" rel_kernel.a out/rel.bin "$GCC"
assert "P2 .bin lands at the cwd-relative path" -s "$WORK/out/rel.bin"
assert "P3 .bin has ESP image magic 0xE9" "$(head -c1 "$WORK/out/rel.bin" | xxd -p)" = "e9"
assert "P4 no stray .bin under the script dir" -z "$(ls "$HERE"/out "$HERE"/rel.bin 2> /dev/null)"
assert "P5 source sdkconfig/sdkconfig.old were not created or modified" "$(snap)" = "$BEFORE"
assert "P6 sdkconfig was generated under the build dir instead" -n "$(ls "$WORK"/out/rel.bin.idf.*/sdkconfig 2> /dev/null)"
shasum -a 256 "$WORK/out/rel.bin"
wc -c < "$WORK/out/rel.bin"

# --- negative: 失敗系は壊れていない (.bin を作らず非 0)
expect "N1 missing IDF_PATH fails" nonzero env -u IDF_PATH bash "$BUILD_SH" rel_kernel.a neg.bin "$GCC"
assert "N1b no .bin after N1" ! -e "$WORK/neg.bin"
expect "N2 missing IDF_TOOLS_PATH fails" nonzero env -u IDF_TOOLS_PATH bash "$BUILD_SH" rel_kernel.a neg.bin "$GCC"
assert "N2b no .bin after N2" ! -e "$WORK/neg.bin"
expect "N3 compiler version mismatch fails" nonzero bash "$BUILD_SH" rel_kernel.a neg.bin esp-99.9.9_00000000
assert "N3b no .bin after N3" ! -e "$WORK/neg.bin"
assert "N3c message names the mismatch" -n "$(grep 'と違う' "$WORK/last.log")"
expect "N4 missing archive fails" nonzero bash "$BUILD_SH" no_such.a neg.bin "$GCC"
assert "N4b no .bin after N4" ! -e "$WORK/neg.bin"

echo "fails=$FAILS"
test "$FAILS" -eq 0
