#!/bin/bash
# Bazel (//firmware:esp32c6) から呼ぶ薄いラッパ: idf.py build を走らせて .bin を出す。
#   使い方: build.sh <libshizuku_kernel.a> <出力 .bin> <期待する GCC 版文字列>
# ★環境が違えば黙って別物を作らず、理由を出して失敗する (IDF 版 / コンパイラ版の食い違い)。
# fail は subshell で exit 1 する。`|| fail` は最後のコマンドなので set -e が効いて止まる。
set -euo pipefail

fail() (
  echo "ERROR(esp32c6): $*" >&2
  exit 1
)

# ★パスは cd する前にすべて絶対化する。Bazel の genrule は $@ を execroot 相対で渡し、
#   このスクリプトは後で SRC へ cd するので、相対のままだと mktemp / -B / cp が別の場所を指す。
#   realpath は存在しないパス (まだ無い出力 .bin) でも解決できる。
abspath() (
  python3 -c 'import os,sys;print(os.path.realpath(sys.argv[1]))' "$1"
)
ARCHIVE=$(abspath "$1")
OUT=$(abspath "$2")
WANT_GCC=$3
SRC=$(cd -P "$(dirname "$0")" && pwd)

test -s "$ARCHIVE" || fail "カーネルのアーカイブが無い/空: $ARCHIVE"
test -f "$IDF_PATH/export.sh" || fail "IDF_PATH が ESP-IDF を指していない (--action_env=IDF_PATH が要る)"
test -d "$IDF_TOOLS_PATH" || fail "IDF_TOOLS_PATH が未設定 (--action_env=IDF_TOOLS_PATH が要る)"

. "$IDF_PATH/export.sh" > /dev/null 2>&1 || fail "ESP-IDF の export.sh が失敗"

IDF_VER=$(idf.py --version 2>/dev/null | tail -1)
case "$IDF_VER" in
  "ESP-IDF v6.1"*) ;;
  *) fail "ESP-IDF v6.1 が必要。検出: $IDF_VER" ;;
esac
GCC_BANNER=$(riscv32-esp-elf-gcc --version | head -1)
case "$GCC_BANNER" in
  *"$WANT_GCC"*) ;;
  *) fail "IDF のコンパイラ ($GCC_BANNER) が Bazel のツールチェーン ($WANT_GCC) と違う" ;;
esac

# 作業ディレクトリは出力の隣に作る (mktemp の既定の置き場は環境によって書けない)。
mkdir -p "$(dirname "$OUT")"
BUILD_DIR=$(mktemp -d "$OUT.idf.XXXXXX")
# ★sdkconfig / sdkconfig.old は BUILD_DIR 配下へ出す (SRC は Bazel の入力ツリーなので汚さない)。
cd "$SRC"
idf.py -B "$BUILD_DIR" -DSDKCONFIG="$BUILD_DIR/sdkconfig" -DSHIZUKU_KERNEL_ARCHIVE="$ARCHIVE" set-target esp32c6 > "$BUILD_DIR.set-target.log" 2>&1 || fail "idf.py set-target 失敗 ($BUILD_DIR.set-target.log)"
idf.py -B "$BUILD_DIR" -DSDKCONFIG="$BUILD_DIR/sdkconfig" -DSHIZUKU_KERNEL_ARCHIVE="$ARCHIVE" build || fail "idf.py build 失敗"
cp "$BUILD_DIR/shizuku_esp32c6.bin" "$OUT"
echo "OK(esp32c6): $IDF_VER / $GCC_BANNER -> $OUT"
