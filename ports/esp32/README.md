# ESP32 port exploration

This directory holds isolated ESP32 migration experiments. It is not wired into
the repository build and does not claim a functional port.

## Local state at the time of the exploration

- `~/esp` and `~/.espressif` were absent.
- `idf.py`, `xtensa-esp32s3-elf-gcc`, and `riscv32-esp-elf-gcc` were not on `PATH`.
- Apple clang 21 was present, but its cross-target compile failed because its LLVM
  option parser rejected `-riscv-add-build-attributes`. The installed `llvm-mc`
  successfully assembled the RV32 probes instead.
- No probe was executed on a device.

## RV32 probe

`c6/rv32_isa_probe.S` is an assembler-only check for a few RV32IMAC CSR, atomic,
and trap mnemonics. It is not an interrupt entry or an ARCH implementation.
Compile from this directory with:

```sh
llvm-mc -triple=riscv32-unknown-elf -mattr=+m,+a,+c -filetype=obj \
  c6/rv32_isa_probe.S -o build/rv32_isa_probe.o
llvm-mc -triple=riscv32-unknown-elf -mattr=+m,+a,+c -filetype=obj \
  c6/pmp_arch_probe.S -o build/pmp_arch_probe.o
```

The output belongs under `build/`, which is ignored. An ESP-IDF compile must be
done after installing/exporting the matching ESP-IDF toolchain; no package was
downloaded as part of this exploration.

`tools/install_esp_idf.sh` pins the current stable ESP-IDF bugfix release and is
intentionally not run in this workspace. Validate it with `bash -n` and run it
from an Apple Silicon macOS terminal when installation is wanted.

## Implementation & Milestone (2026-10-06)

### 1. PMP Codec & Host Verification (`c6/pmp.hpp`, `c6/test_pmp.cpp`)
- Implemented pure arithmetic & bitwise encoding/decoding for ESP32-C6 PMP (the TRM cites RISC-V Privileged Architecture v1.10):
  - NAPOT (Naturally Aligned Power-of-Two, >= 8B): `(base >> 2) | ((size >> 3) - 1)`
  - TOR (Top of Range)
  - Validation: rejects unaligned bases, non-power-of-two sizes, sizes < 8B, and invalid permission bits (fail-closed).
  - Packed CSR configuration: `pack_pmpcfg` / `unpack_pmpcfg`.
- Host test with full assertions passes via Bazel or host clang++:
  ```sh
  bazel test --platforms=@platforms//host //ports/esp32/c6:all
  ```

### 2. C6 Arch & Board Skeletons (`rv32_c6.hpp`, `esp32_c6.hpp`)
- `shizuku::archs::rv32_c6` and `shizuku::boards::esp32_c6` strictly satisfy `concepts::arch_requires` and `concepts::board_requires`.
- Single-core constraint: `CORE_COUNT = 1`. Multicore functions (`launch_core`, `park_other_cores`) explicitly declare panic rather than deceptive no-op success.
- Fail-closed guard: guarded with `#if !defined(SHIZUKU_C6_SKELETON_PERMITTED) #error ... #endif`. Unintentional linkage into production builds is rejected at compile time.

### 3. 現状の観測 (compile/link-only マイルストーン)

観測済みの事実だけを書く。これは起動・実行時の動作の証明ではない。

- **ビルド経路 (Bazel は未達)**:
  - Bazel の主線 (`bazelisk build --config=esp32c6 //firmware:esp32c6`) は **解析・ビルドとも未達**。
    `bcr.bazel.build` への接続が拒否され、MODULE の依存解決で止まった。
    `toolchains/rv32_esp/`、`//:shizuku_kernel`、`//firmware:esp32c6`、`.bazelrc` の
    `--config=esp32c6` は書いてあるが **一度も実行しておらず unverified**。
    回復には `bcr.bazel.build` の読取許可が必要。
  - `libshizuku_kernel.a` は Bazel ではなく、手動の RV32 コンパイル (`riscv32-esp-elf-g++`) + `ar rcs` で作った。
    メンバーは `c6_link_stubs.o` / `cpu_manager_init.o` / `kernel_dispatch.o` / `kernel_init.o` /
    `kernel_thread.o` / `memory_manager_freestanding.o` の 6 個。
  - IDF v6.1 の `.bin` は、その手動 `.a` を使った手動の `idf.py build` (`firmware/esp32c6/build.sh` 経由を含む) で生成した。
    `build.sh` を Bazel の action から呼んだ実績は無い。
- **イメージの中身**: `.a` は実オブジェクトを含み、最終 ELF/map は 6 メンバーすべてを取り込む。
  ただし起動は動かない。強シンボル `start_cpu0` (`firmware/esp32c6/main/shizuku_boot.c`) は `ebreak` するだけで、
  カーネルを呼ばない。`ports/esp32/c6/link_stubs.cpp` の 26 関数 (arch 13: 行22-36、board 13: 行44-56) は
  すべて呼ばれたら即トラップするリンク専用スタブで、成功を返す no-op は無い。
- **FreeRTOS**: スケジューラは起動しない (ユーザー確定条件)。この ELF に `vTaskStartScheduler` /
  `esp_startup_start_app` / `start_cpu0_default` のシンボルが無いことを確認した。
  FreeRTOS ライブラリのリンクとスケジューラ起動は区別する。
- **IDF startup の使い方 (未解決)**: IDF v6.1 の `start_cpu0` 差し替えは private ABI で、
  `do_core_init` / コンストラクタ / `do_secondary_init` を飛ばす。ヒープ・libc・ウォッチドッグ・PMP の
  初期化責任の持ち方は決まっておらず、現状は意図的に何も動かさないことで回避しているだけである。

### 4. 将来の設計目標と未確認事項 (実証していない)

以下は設計上の目標・仮説であり、今回の実装では **何も検証していない**。保証や必須の構成として読まないこと。

- **目標**: Shizuku が M-mode の trap vector / SYSTIMER / PMP を持つ最小起動を目指す。
  例外優先度 (`ecall > timer > deferred switch`) と U-mode オブジェクトの隔離を狙う。
  trap・timer・context switch・`current_priv` は未実装で、PMP の host unit test は
  ハードウェア上の隔離の証明にならない。
- **トレードオフ候補**: 最小起動は ESP-IDF のドライバ群 (Wi-Fi/BLE/USB 等、FreeRTOS に依存するもの) を使えなくする可能性がある。
- **FreeRTOS 上の service task 案**: FreeRTOS が trap/PMP を持つ場合に Shizuku がどこまで強制できるかは未調査。
  「できない」とも「できる」とも結論していない。
- **無線**: 別サブシステム / RPC 境界に分ける案があるが、評価していない。
- **未確認**: 実機での起動、PMP/trap 所有の可否、IDF private ABI の安定性 (v6.1 固定でも保証なし)。
