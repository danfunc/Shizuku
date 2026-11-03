#!/bin/bash
# usage: build-diag.sh — wt-rpfp (cwd) を rbuild(worktree 名 rpfp) で SHIZUKU_DIAG_FP=1 の RP2350 診断ビルド。
set -u
cd /Users/ishigakiyua/github/Shizuku/.handoff/wt-rpfp
export PATH=$HOME/.local/bin:$PATH
OUT=/Users/ishigakiyua/github/Shizuku/.handoff/bench/rp-fp-ownership/diag
mkdir -p "$OUT"
R=(rbuild --config $HOME/.config/rbuild.toml --worker main-lxc --worktree rpfp)
git rev-parse --short HEAD > "$OUT/head"
"${R[@]}" build --jobs=2 --config=rp2350_pico2 --copt=-DSHIZUKU_DIAG_FP=1 //firmware:shizuku //firmware:shizuku_uf2 > "$OUT/build.log" 2>&1; echo "rp2350 diag build=$?"
cp -f .rbuild-out/firmware/shizuku "$OUT/shizuku.elf"
cp -f .rbuild-out/firmware/shizuku.uf2 "$OUT/"
cp /Users/ishigakiyua/github/Shizuku/.handoff/bench/fp-state-20261103/rp2350/run_one.sh "$OUT/run_one.sh"
( cd "$OUT" && shasum -a 256 shizuku.uf2 shizuku.elf )
