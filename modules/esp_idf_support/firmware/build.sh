#!/bin/bash
# Bazel (//modules/esp_idf_support/firmware:esp32c6) から呼ぶ薄いラッパ: idf.py build を走らせて .bin を出す。
#   使い方: build.sh <libshizuku_kernel.a> <出力 .bin> <期待する GCC 版文字列> [target] [profile] [ELF] [map] [--ota-source path | --ota-sources paths...] [--ota-include-dir path]...
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
command -v python3 >/dev/null 2>&1 || fail "python3 が無く、パスを絶対化できない"
test -n "${IDF_PATH:-}" || fail "IDF_PATH が未設定 (--action_env=IDF_PATH が要る)"
test -n "${IDF_TOOLS_PATH:-}" || fail "IDF_TOOLS_PATH が未設定 (--action_env=IDF_TOOLS_PATH が要る)"
ARCHIVE=$(abspath "$1")
OUT=$(abspath "$2")
IDF_PATH=$(abspath "$IDF_PATH")
IDF_TOOLS_PATH=$(abspath "$IDF_TOOLS_PATH")
export IDF_PATH IDF_TOOLS_PATH
WANT_GCC=$3
TARGET=esp32c6
PROFILE=""
ELF_OUT=""
MAP_OUT=""
BLE_PREFER_1M_PHY=${SHIZUKU_ESP_BLE_PREFER_1M_PHY:-${SHIZUKU_C5_BLE_PREFER_1M_PHY:-0}}
OTA_SOURCES=()
OTA_INCLUDE_DIRS=()
shift 3
OPTIONAL_POSITIONALS=0
while (($#)) && [[ "$1" != --* ]] && ((OPTIONAL_POSITIONALS < 4)); do
  case "$OPTIONAL_POSITIONALS" in
    0) TARGET=$1 ;;
    1) PROFILE=$1 ;;
    2) if test -n "$1"; then ELF_OUT=$(abspath "$1"); fi ;;
    3) if test -n "$1"; then MAP_OUT=$(abspath "$1"); fi ;;
  esac
  shift
  OPTIONAL_POSITIONALS=$((OPTIONAL_POSITIONALS + 1))
done
while (($#)); do
  case "$1" in
    --ota-source)
      test $# -ge 2 || fail "--ota-source には path が必要"
      OTA_SOURCES+=("$(abspath "$2")")
      shift 2
      ;;
    --ota-sources)
      shift
      SOURCE_COUNT=0
      while (($#)) && [[ "$1" != --* ]]; do
        OTA_SOURCES+=("$(abspath "$1")")
        SOURCE_COUNT=$((SOURCE_COUNT + 1))
        shift
      done
      test "$SOURCE_COUNT" -gt 0 || fail "--ota-sources には 1 つ以上の path が必要"
      ;;
    --ota-include-dir)
      test $# -ge 2 || fail "--ota-include-dir には path が必要"
      OTA_INCLUDE_DIRS+=("$(abspath "$2")")
      shift 2
      ;;
    *) fail "未対応の追加引数: $1" ;;
  esac
done
if ((${#OTA_SOURCES[@]} || ${#OTA_INCLUDE_DIRS[@]})) && test "$PROFILE" != ble; then
  fail "外部 OTA sources/include dirs は ble profile でのみ指定できる"
fi
for source in "${OTA_SOURCES[@]}"; do
  test -f "$source" || fail "外部 OTA source が無い: $source"
done
for include_dir in "${OTA_INCLUDE_DIRS[@]}"; do
  test -d "$include_dir" || fail "外部 OTA include dir が無い: $include_dir"
done
case "$TARGET" in esp32c6|esp32c5) ;; *) fail "未対応の IDF target: $TARGET" ;; esac
# C6 U-mode + PMP (既定 on)。C6 以外 (C5) では off に倒す。
C6_UMODE=${SHIZUKU_C6_UMODE:-on}
case "$C6_UMODE" in off|on) ;; *) fail "SHIZUKU_C6_UMODE は off|on のどちらか: $C6_UMODE" ;; esac
if [ "$C6_UMODE" = on ] && [ "$TARGET" != esp32c6 ]; then
  C6_UMODE=off
fi
IDF_PROFILE_ARGS=("-DSHIZUKU_C6_UMODE=$C6_UMODE")
case "$PROFILE" in
  "") ;;
  ble)
    case "$BLE_PREFER_1M_PHY" in 0|1) ;; *) fail "SHIZUKU_C5_BLE_PREFER_1M_PHY は 0 または 1" ;; esac
    export SHIZUKU_ESP_BLE=1
    IDF_PROFILE_ARGS+=("-DSHIZUKU_ESP_BLE=ON"
                       "-DSHIZUKU_ESP_BLE_PREFER_1M_PHY=$BLE_PREFER_1M_PHY"
                       "-DSHIZUKU_ESP_BLE_IRQ_LATENCY_DIAG=1"
                       "-DSHIZUKU_ESP_BLE_ENABLE_SCHED_DIAG_IRQ_MASK=1")
    if test "$TARGET" = esp32c6; then
      BLE_COMPAT_GRANT_CYCLES=${SHIZUKU_BLE_COMPAT_GRANT_CYCLES:-1600}
      if [[ ! "$BLE_COMPAT_GRANT_CYCLES" =~ ^[1-9][0-9]*$ ]] ||
         (( ${#BLE_COMPAT_GRANT_CYCLES} > 10 )); then
        fail "SHIZUKU_BLE_COMPAT_GRANT_CYCLES must be a positive uint32_t"
      fi
      if (( BLE_COMPAT_GRANT_CYCLES > 4294967295 )); then
        fail "SHIZUKU_BLE_COMPAT_GRANT_CYCLES must be a positive uint32_t"
      fi
      IDF_PROFILE_ARGS+=("-DSHIZUKU_BLE_COMPAT_GRANT_CYCLES=$BLE_COMPAT_GRANT_CYCLES")
    fi
    ;;
  *) fail "未対応の firmware profile: $PROFILE" ;;
esac
if ((${#OTA_SOURCES[@]})); then
  OTA_SOURCE_LIST=$(IFS=';'; printf '%s' "${OTA_SOURCES[*]}")
  IDF_PROFILE_ARGS+=("-DSHIZUKU_ESP_OTA_SOURCES=$OTA_SOURCE_LIST")
fi
if ((${#OTA_INCLUDE_DIRS[@]})); then
  OTA_INCLUDE_LIST=$(IFS=';'; printf '%s' "${OTA_INCLUDE_DIRS[*]}")
  IDF_PROFILE_ARGS+=("-DSHIZUKU_ESP_OTA_INCLUDE_DIRS=$OTA_INCLUDE_LIST")
fi
SRC=$(cd -P "$(dirname "$0")" && pwd)

test -s "$ARCHIVE" || fail "カーネルのアーカイブが無い/空: $ARCHIVE"
test -f "$IDF_PATH/export.sh" || fail "IDF_PATH が ESP-IDF を指していない (--action_env=IDF_PATH が要る)"
test -d "$IDF_TOOLS_PATH" || fail "IDF_TOOLS_PATH が未設定 (--action_env=IDF_TOOLS_PATH が要る)"

. "$IDF_PATH/export.sh" > /dev/null 2>&1 || fail "ESP-IDF の export.sh が失敗 (IDF_PATH / IDF_TOOLS_PATH / Python 環境を確認)"

command -v python >/dev/null 2>&1 || fail "export.sh 後に python が PATH 上に無い"
command -v idf.py >/dev/null 2>&1 || fail "export.sh 後に idf.py が PATH 上に無い"
command -v riscv32-esp-elf-gcc >/dev/null 2>&1 || fail "export.sh 後に riscv32-esp-elf-gcc が PATH 上に無い"
test -n "${IDF_PYTHON_ENV_PATH:-}" && test -x "$IDF_PYTHON_ENV_PATH/bin/python" || fail "ESP-IDF Python 環境が無い/実行不可"

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
idf.py -B "$BUILD_DIR" -DSDKCONFIG="$BUILD_DIR/sdkconfig" -DSHIZUKU_KERNEL_ARCHIVE="$ARCHIVE" "${IDF_PROFILE_ARGS[@]}" set-target "$TARGET" > "$BUILD_DIR.set-target.log" 2>&1 || fail "idf.py set-target 失敗 ($BUILD_DIR.set-target.log)"
if test "$PROFILE" = ble; then
  grep -qx 'CONFIG_BT_ENABLED=y' "$BUILD_DIR/sdkconfig" || fail "BLE profile sdkconfig で CONFIG_BT_ENABLED が有効でない ($BUILD_DIR/sdkconfig)"
  grep -qx 'CONFIG_BT_NIMBLE_ENABLED=y' "$BUILD_DIR/sdkconfig" || fail "BLE profile sdkconfig で CONFIG_BT_NIMBLE_ENABLED が有効でない ($BUILD_DIR/sdkconfig)"
fi
idf.py -B "$BUILD_DIR" -DSDKCONFIG="$BUILD_DIR/sdkconfig" -DSHIZUKU_KERNEL_ARCHIVE="$ARCHIVE" -DIDF_TARGET="$TARGET" "${IDF_PROFILE_ARGS[@]}" build || fail "idf.py build 失敗"
if test "${SHIZUKU_ESP_OTA:-}" = 1; then
  PARTITION_BIN="$BUILD_DIR/partition_table/partition-table.bin"
  PARTITION_CSV="$BUILD_DIR/partition_table/partition-table.csv"
  test -s "$PARTITION_BIN" || fail "OTA build の partition-table.bin が無い/空: $PARTITION_BIN"
  OTA_FLASH_SIZE=$(sed -n 's/^CONFIG_ESPTOOLPY_FLASHSIZE="\([^"]*\)"/\1/p' "$BUILD_DIR/sdkconfig" | tail -1)
  test -n "$OTA_FLASH_SIZE" || fail "OTA sdkconfig に CONFIG_ESPTOOLPY_FLASHSIZE が無い: $BUILD_DIR/sdkconfig"
  PARTITION_DECODER="$IDF_PATH/components/partition_table/gen_esp32part.py"
  test -f "$PARTITION_DECODER" || fail "ESP-IDF partition decoder が無い: $PARTITION_DECODER"
  "$IDF_PYTHON_ENV_PATH/bin/python" "$PARTITION_DECODER" \
      "$PARTITION_BIN" "$PARTITION_CSV" "--flash-size=$OTA_FLASH_SIZE" || \
      fail "OTA partition-table.bin の CSV decode に失敗"
  test -s "$PARTITION_CSV" || fail "OTA partition-table.csv が無い/空: $PARTITION_CSV"
fi
cp "$BUILD_DIR/shizuku_$TARGET.bin" "$OUT"
if test -n "$ELF_OUT"; then cp "$BUILD_DIR/shizuku_$TARGET.elf" "$ELF_OUT"; fi
if test -n "$MAP_OUT"; then cp "$BUILD_DIR/shizuku_$TARGET.map" "$MAP_OUT"; fi

if [ "$TARGET" = esp32c6 ] && [ "$C6_UMODE" = on ]; then
  echo "--- [CHECK] Verifying C6 Stage1 PMA Wrap & U-mode symbols ---"
  echo "1. Bootloader ELF wrap check:"
  BOOTLOADER_ELF="$BUILD_DIR/bootloader/bootloader.elf"
  test -f "$BOOTLOADER_ELF" || fail "bootloader.elf が無い: $BOOTLOADER_ELF"
  riscv32-esp-elf-nm "$BOOTLOADER_ELF" | grep -E "wrap_esp_cpu_configure_region_protection|shizuku_c6_bootloader_branch" || fail "bootloader.elf に wrap シンボルが無い"

  echo "2. App ELF wrap check:"
  APP_ELF="$BUILD_DIR/shizuku_$TARGET.elf"
  test -f "$APP_ELF" || fail "app ELF が無い: $APP_ELF"
  riscv32-esp-elf-nm "$APP_ELF" | grep -E "wrap_esp_cpu_configure_region_protection|shizuku_c6_app_branch|shizuku_c6_stage1_pma_ready" || fail "app ELF に wrap シンボルが無い"

  echo "3. U-mode entry IRAM check:"
  riscv32-esp-elf-nm "$APP_ELF" | grep -E "shizuku_rv32_trap_entry|shizuku_c6_vectors" || fail "IRAM entry シンボルが無い"
  echo "--- [CHECK] Verification PASSED ---"
fi

echo "OK($TARGET): $IDF_VER / $GCC_BANNER -> $OUT"
