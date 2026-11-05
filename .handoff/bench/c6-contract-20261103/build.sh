#!/bin/bash
# c6-contract-20261103 ビルド用スクリプト (wt-c6c 側から実行)
# host 試験 + RP2040 build + RP2350 build + C6 build
# 出力先: 本体の /Users/ishigakiyua/github/Shizuku/.handoff/bench/c6-contract-20261103/{c6,rp2350}/
set -u
cd /Users/ishigakiyua/github/Shizuku/.handoff/wt-c6c
export PATH=$HOME/.local/bin:$PATH
OUT=/Users/ishigakiyua/github/Shizuku/.handoff/bench/c6-contract-20261103
mkdir -p "$OUT/rp2350" "$OUT/c6"

R=(rbuild --config "$HOME/.config/rbuild.toml" --worker main-lxc --worktree c6c)

git rev-parse --short HEAD > "$OUT/head"
git status --short | grep -v '^??' > "$OUT/dirty" || true

echo "=== 1. Host Tests ==="
"${R[@]}" test --cache_test_results=no --test_output=errors \
  --//configs:board=esp32_c6 --//configs:c6_umode=disabled \
  //:dispatcher_v1_test //:dispatcher_v1_lean_ledger_test \
  //:dispatcher_v1_3a_real_kobj_lean_test //tests/arm_inplace/... \
  //:test_u_cost_invariants //:three_a_return_window_test //:stack_window_test \
  //arch/armv8m:object_window_test //arch/armv6m:object_window_test \
  //:armv8m_legacy_frame_return_test \
  //tests/esp32_c6/... //chips/esp32_c6/... > "$OUT/host.log" 2>&1
HOST_RC=$?
echo "host=$HOST_RC $(grep -o 'Executed.*' "$OUT/host.log" 2>/dev/null || true)"

echo "=== 2. RP2040 Build ==="
"${R[@]}" build --jobs=2 --config=rp2040_pico_w //firmware:shizuku > "$OUT/rp2040.log" 2>&1
RP2040_RC=$?
echo "rp2040 build=$RP2040_RC"

echo "=== 3. RP2350 Build ==="
"${R[@]}" build --jobs=2 --config=rp2350_pico2 //firmware:shizuku //firmware:shizuku_uf2 > "$OUT/rp2350/build.log" 2>&1
RP2350_RC=$?
echo "rp2350 build=$RP2350_RC"
if [ $RP2350_RC -eq 0 ]; then
  cp -f .rbuild-out/firmware/shizuku "$OUT/rp2350/shizuku.elf"
  cp -f .rbuild-out/firmware/shizuku.uf2 "$OUT/rp2350/"
fi

echo "=== 4. C6 Build ==="
"${R[@]}" build --config=esp32c6 --jobs=auto --action_env=CMAKE_BUILD_PARALLEL_LEVEL=24 \
  --action_env=IDF_PATH=/home/ishigakiyua/.local/esp-idf-c5-v6.1 \
  --action_env=IDF_TOOLS_PATH=/home/ishigakiyua/.local/idf-tools-c5-v6.1 \
  //modules/esp_idf_support/firmware:esp32c6 > "$OUT/c6/build.log" 2>&1
C6_RC=$?
echo "c6 build=$C6_RC"
if [ $C6_RC -eq 0 ]; then
  cp -f .rbuild-out/modules/esp_idf_support/firmware/shizuku_esp32c6.bin "$OUT/c6/"
  cp -f .rbuild-out/modules/esp_idf_support/firmware/shizuku_esp32c6.elf "$OUT/c6/"
fi

echo "=== SHA-256 Checksums ==="
SHACMD="sha256sum"
if ! command -v sha256sum >/dev/null 2>&1; then
  SHACMD="shasum -a 256"
fi

for f in "$OUT/c6/shizuku_esp32c6.bin" "$OUT/c6/shizuku_esp32c6.elf" "$OUT/rp2350/shizuku.elf" "$OUT/rp2350/shizuku.uf2"; do
  if [ -f "$f" ]; then
    $SHACMD "$f"
  else
    echo "MISSING: $f"
  fi
done
