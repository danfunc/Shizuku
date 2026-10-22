#!/usr/bin/env bash
# カーネルの .a を作る段階で 1 回だけ LTO をかける (GCC の部分リンク)。
#   使い方: kernel_lto.sh <CC> <AR> <NM> <入力 .a> <出力 .a>   (objdump は NM と同じ場所のもの)
#
# 入力の .a のうち、`-flto -ffat-lto-objects` でコンパイルされたメンバー (= カーネル。.gnu.lto_* を
# 持つ) と、カーネルの asm (`*_vectors.o`) を `gcc -flto -r -flinker-output=nolto-rel` で 1 個の
# 普通の .o (最適化済み、LTO の中間表現は残さない) にまとめ、残りのメンバー (selftest・board・
# ESP-IDF との接着部) はそのまま新しい .a へ入れる。ESP-IDF の CMake の最終リンクは LTO を知らなくてよい。
#   - 外から呼ばれる C ABI の入口 (asm・ESP-IDF 側が参照する大域シンボル) が消えていないことを、
#     部分リンクの前後で大域シンボル (弱くないもの) を突き合わせて確かめる。消えていたら失敗。
#   - `.iram1` などの section 指定が残っていること、関数が個別の section に分かれていること
#     (-ffunction-sections。最終リンクの gc が効く) を確かめる。
set -euo pipefail
abs() { case "$1" in /*) printf '%s' "$1" ;; *) printf '%s/%s' "$PWD" "$1" ;; esac; }
CC=$(abs "$1"); AR=$(abs "$2"); NM=$(abs "$3")
OBJDUMP="$(dirname "$NM")/objdump"
IN=$(abs "$4"); OUT=$(abs "$5")
fail() { echo "kernel_lto: $*" >&2; exit 1; }
test -s "$IN" || fail "入力の .a が無い/空: $IN"

ORIG=$PWD
WORK=$(mktemp -d "${OUT}.work.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
mkdir "$WORK/m" && cd "$WORK/m"
# ツールチェーンのラッパ (toolchains/rv32_esp/bin/*) は cwd 基準の相対パス (external/...) で本体を exec する。
# ar x は展開先を指定できず cwd に出すので、作業ディレクトリへ移っても相対パスが引けるようにする。
ln -s "$ORIG/external" external

# 1. 全メンバーを取り出す。同名のメンバー (init.o が 2 つ) があるので、n 番目を `xN` で取り出して
#    連番付きの名前にする。
MEMBERS=$("$AR" t "$IN") || fail "ar t 失敗: $IN"
declare -A seen=()
LTO=(); VEC=(); REST=()
idx=0
while IFS= read -r name; do
  seen[$name]=$(( ${seen[$name]:-0} + 1 ))
  idx=$((idx + 1))
  file=$(printf '%03d_%s' "$idx" "$name")
  "$AR" xN "${seen[$name]}" "$IN" "$name" || fail "ar xN 失敗: $name (${seen[$name]})"
  mv "$name" "$file"
  if grep -qa '\.gnu\.lto_' "$file"; then
    LTO+=("$file")
  elif [[ "$name" == *_vectors.o ]]; then
    VEC+=("$file")
  else
    REST+=("$file")
  fi
done <<<"$MEMBERS"
((${#LTO[@]})) || fail "LTO のメンバーが 1 つも無い (-flto -ffat-lto-objects でコンパイルされていない)"
echo "kernel_lto: LTO ${#LTO[@]} 個 (${LTO[*]}) + asm ${#VEC[@]} 個 (${VEC[*]}) / そのまま ${#REST[@]} 個"

# 2. 部分リンク前の大域シンボル (弱くない定義) を控える。fat なので普通のコードも持っている。
globals() { "$NM" -g --defined-only "$@" 2>/dev/null | awk 'NF==3 && $2 !~ /^[WwVvu]$/ {print $3}' | sort -u; }
globals "${LTO[@]}" "${VEC[@]}" > "$WORK/before.txt"

# 3. 部分リンク。-flinker-output=nolto-rel: LTO で最適化した普通のオブジェクトを出す。
#    -flto-partition=none: 1 つの翻訳単位にまとめる (関数の跨ぎ inline を最大にし、結果を決定的にする)。
#    -ffunction-sections / -fdata-sections / -fno-common: コード生成の option は LTO では**リンクの
#    コマンドラインのものが効く**ので、コンパイル時と同じものをここにも渡す (ツールチェーンの既定)。
"$CC" -flto -flto-partition=none -r -flinker-output=nolto-rel -nostdlib \
    -march=rv32imac_zicsr_zifencei -mabi=ilp32 -O2 -ffunction-sections -fdata-sections -fno-common \
    -o "$WORK/kernel_lto.o" "${LTO[@]}" "${VEC[@]}" || fail "部分リンク失敗"

# 4. 検査。
globals "$WORK/kernel_lto.o" > "$WORK/after.txt"
comm -23 "$WORK/before.txt" "$WORK/after.txt" > "$WORK/lost.txt"
if test -s "$WORK/lost.txt"; then
  echo "kernel_lto: 部分リンクで消えた大域シンボル:" >&2
  cat "$WORK/lost.txt" >&2
  fail "外から呼ばれうる大域シンボルが消えた"
fi
SECTIONS=$("$OBJDUMP" -h "$WORK/kernel_lto.o")
grep -q '\.iram1' <<<"$SECTIONS" || fail ".iram1 の section 指定が残っていない"
FN_SECTIONS=$(grep -c ' \.text\.' <<<"$SECTIONS" || true)
test "$FN_SECTIONS" -gt 10 || fail "関数ごとの section が少ない ($FN_SECTIONS): -ffunction-sections が効いていない"
grep -qa '\.gnu\.lto_' "$WORK/kernel_lto.o" && fail "出力に LTO の中間表現が残っている"
echo "kernel_lto: 大域シンボル $(wc -l < "$WORK/before.txt") 個すべて保持、.iram1 あり、.text.* section $FN_SECTIONS 個"

# 5. 新しい .a。LTO にしなかったメンバーはそのまま。
rm -f "$OUT"
"$AR" rcs "$OUT" "$WORK/kernel_lto.o" "${REST[@]}" || fail "ar 失敗"
echo "kernel_lto: OK -> $OUT"
