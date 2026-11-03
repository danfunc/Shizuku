#!/bin/bash
# usage: build.sh  — wt-rpfp (cwd) を rbuild(worktree 名 rpfp) で host 試験 + RP2040 + RP2350 ビルド。
set -u
cd /Users/ishigakiyua/github/Shizuku/.handoff/wt-rpfp
export PATH=$HOME/.local/bin:$PATH
OUT=/Users/ishigakiyua/github/Shizuku/.handoff/bench/rp-fp-ownership
R=(rbuild --config $HOME/.config/rbuild.toml --worker main-lxc --worktree rpfp)
git rev-parse --short HEAD > $OUT/head
"${R[@]}" test --cache_test_results=no --test_output=errors --//configs:board=esp32_c6 --//configs:c6_umode=disabled //:dispatcher_v1_test //:dispatcher_v1_lean_ledger_test //:dispatcher_v1_3a_real_kobj_lean_test //tests/arm_inplace/... //:test_u_cost_invariants //:three_a_return_window_test //arch/armv8m:object_window_test //arch/armv6m:object_window_test //:armv8m_legacy_frame_return_test > $OUT/host.log 2>&1; echo "host=$? $(grep -o 'Executed.*' $OUT/host.log)"
"${R[@]}" build --jobs=2 --config=rp2040_pico_w //firmware:shizuku > $OUT/rp2040.log 2>&1; echo "rp2040 build=$?"
"${R[@]}" build --jobs=2 --config=rp2350_pico2 //firmware:shizuku //firmware:shizuku_uf2 > $OUT/rp2350/build.log 2>&1; echo "rp2350 build=$?"
cp -f .rbuild-out/firmware/shizuku $OUT/rp2350/shizuku.elf; cp -f .rbuild-out/firmware/shizuku.uf2 $OUT/rp2350/
cp /Users/ishigakiyua/github/Shizuku/.handoff/bench/fp-state-20261103/rp2350/run_one.sh $OUT/rp2350/run_one.sh
( cd $OUT/rp2350 && shasum -a 256 shizuku.uf2 shizuku.elf )
