"""RP2350 のカーネルだけにかける LTO の flag。

カーネル (カーネル本体・カーネルオブジェクト・SVC 経路の arch_glue) の .o だけを
`-flto -ffat-lto-objects` でコンパイルし、最終リンクの `-flto` で IR を持つ .o (= カーネルだけ) を
1 つにまとめて最適化する。selftest・board・pico-sdk は IR を持たない普通の .o なので LTO の対象外
(計測コードに LTO をかけない。C6 の kernel_lto と同じ比較条件)。
切るとき: `--//configs:rp2350_kernel_lto=false` (RP2350 のカーネルは従来どおり LTO なし)。
"""

RP2350_KERNEL_LTO_COPTS = select({
    "//configs:rp2350_kernel_lto_enabled": ["-flto", "-ffat-lto-objects"],
    "//conditions:default": [],
})

# リンク時のコード生成 option はリンクのコマンドラインのものが効く。最適化レベルは関数ごとに
# IR へ残るので -O は渡さない (default の -O2 と bench_opt_o3 の -O3 がそのまま効く)。
# -flto-partition=none: 1 つの翻訳単位にまとめる (SVC 経路の跨ぎ inline を最大にし、結果を決定的にする)。
RP2350_KERNEL_LTO_LINKOPTS = select({
    "//configs:rp2350_kernel_lto_enabled": [
        "-flto",
        "-flto-partition=none",
        "-ffunction-sections",
        "-fdata-sections",
    ],
    "//conditions:default": [],
})
