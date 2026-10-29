#set document(title: "Shizuku CPU 参照表", author: "Shizuku")
#set page(paper: "a4", margin: (x: 15mm, y: 15mm), numbering: "1")
#set text(font: ("Hiragino Sans", "Arial"), size: 8pt, lang: "ja")
#set par(leading: 0.55em, justify: true)
// 表題 (=) は番号なし、章 (==) から 1, 2, ... と数える。
#set heading(numbering: (..n) => if n.pos().len() > 1 { numbering("1.1", ..n.pos().slice(1)) })
#show raw: set text(font: "Menlo", size: 6.8pt)
#show table: set par(justify: false)
#let chk = text(fill: rgb("b03030"), weight: "bold", "[要確認]")
#let note(s) = block(fill: rgb("f8f8f8"), inset: 5pt, radius: 2pt, stroke: 0.5pt + rgb("dddddd"), width: 100%, text(size: 7.5pt, s))
#let src(path) = raw(read(path), block: true, lang: if path.ends-with(".S") { "asm" } else { "cpp" })

= Shizuku CPU 参照表 (RP2040 / RP2350 / ESP32-C6 / ESP32-C5)

状態: *初版*。2026-10-27。対象: `~/github/Shizuku` dev ブランチ (HEAD `1ee7b41` + 未コミット差分)。

== この文書の使い方

- 第 2〜5 章はチップごとの事実の表で、どのチップも同じ 7 項目を同じ順に並べる (コア / ISA と拡張 / 特権と例外 / 保護 / タイマとカウンタ / メモリ地図 / Shizuku での使用箇所)。
- 値の出どころは 2 種類ある。*repo* はリポジトリのコード (多くは pico-sdk / ESP-IDF の出典を注記済み) から取った値、*DS* は各社のデータシートと Arm / RISC-V の仕様書。初版の推定値は 2026-10-27 に Luna (gpt-6-luna) がデータシートと照合し、確認できたものは印を外した。#chk が残っているものは一次資料で確認できていない値で、設計判断の根拠にしない。
- 第 6 章は横並びの比較表。第 7 章は「cpu_manager にあるべき処理が各層に散っている」という現状の整理。第 8 章はそれを直した形のサンプルコード (`docs/design/arch/cpu_samples/`)。
- ISA ファミリーとしての設計論は `cortex_m.typ` / `riscv.typ` にある。この文書は個々のチップの事実だけを扱う。

== RP2040 (Cortex-M0+ × 2)

#table(columns: (1.1fr, 3fr, 1.2fr), inset: 3.5pt,
  table.header([項目], [内容], [出典]),
  [コア], [Cortex-M0+ × 2、対称。最大 133MHz (データシートには条件付きで 200MHz の記述もある)、pico-sdk の既定は 125MHz], [RP2040 DS §1.2, §2.15.3。コア数は repo: `chips/rp2040/chip.hpp:13`],
  [ISA と拡張], [ARMv6-M (Thumb-1 + BL/MRS/MSR/DMB 等の一部 Thumb-2)。*FPU なし。LDREX/STREX なし* — 原子操作は SIO のハードウェア spinlock (32 本) を IRQ 禁止と組み合わせて作る。除算器は SIO の周辺], [repo: `chip.hpp:52`、`arch/armv6m/armv6m.hpp:288-298`],
  [特権と例外], [Thread/Handler、CONTROL.nPRIV による特権/非特権。例外フレームは常に 32B (r0-r3, r12, lr, pc, xPSR)。*設定可能なフォールト例外が無い* (HardFault のみ、優先度 -1 固定)。DebugMonitor 例外は無い。NVIC 優先度は上位 2bit (4 段)。外部 IRQ 26 本。VTOR あり。*PSPLIM なし* — スタック下限はソフトで検査する], [repo: `chip.hpp:28-36`、`armv6m.hpp:43-68`。RP2040 DS §2.4.2],
  [保護], [PMSAv6 MPU、8 region。大きさは 2 の冪 (256B 以上) でその大きさに整列、8 分割の subregion を個別に無効化できる。PRIVDEFENA あり。任意の範囲は張れないので「張れなければ失敗を返す」], [repo: `chip.hpp:38-41`、`armv6m.hpp:129-135`],
  [タイマとカウンタ], [SysTick 24bit。*DWT CYCCNT なし* (サイクル計測不可)。SoC の TIMER は 64bit µs カウンタ], [repo: `armv6m.hpp:44,94-97`],
  [メモリ地図], [XIP `0x10000000`、SRAM `0x20000000`–`0x20042000` (256KiB ストライプ + 4KiB × 2)、APB `0x40000000`、SIO `0xD0000000`、PPB `0xE0000000`], [repo: `chip.hpp:15-25`],
  [Shizuku での使用], [`arch/armv6m/armv6m.hpp` (arch)、`arch/armv6m/armv6m_ctx.S` (入口)、`boards/rp2040_pico_w/board_rp2040.cpp:155-200` (結線・優先度・MPU)、`modules/pico_sdk_support/rp2040/arch_glue_v6m.cpp` (転送・窓)], [],
)
#note[*RP2040 固有の注意*: flash 書込み中はもう一方のコアを RAM 上で止める (multicore lockout)。その要求は SIO FIFO 割込みで届くので、これを SVC より上 (0x00) に置き、SVC を 0x40 に下げている (`board_rp2040.cpp:163-169`)。4 段しかないため、SVC / SysTick / PendSV の 3 段と SIO FIFO で全部埋まる。]

== RP2350 (Cortex-M33 × 2)

#table(columns: (1.1fr, 3fr, 1.2fr), inset: 3.5pt,
  table.header([項目], [内容], [出典]),
  [コア], [Cortex-M33 × 2 (起動時に Hazard3 RISC-V × 2 と選択可。Shizuku は ARM を使う)。150MHz], [計測ログの `clk 150 MHz`。コア数は repo: `chips/rp2350/chip.hpp:11`],
  [ISA と拡張], [ARMv8-M Mainline + DSP + FPv5 単精度 FPU (s0-s31) + Security Extension (TrustZone-M)。LDREX/STREX あり。SIO spinlock には erratum (RP2350-E2) があり、pico-sdk は既定でソフトの spinlock を使う], [repo: `arch/armv8m/armv8m_ctx.S:6-8`。pico-sdk `spin_lock.h`],
  [特権と例外], [CONTROL.nPRIV。MemManage / BusFault / UsageFault / SecureFault / DebugMonitor を個別に持ち、SHCSR で有効化すれば HardFault へ昇格しない。NVIC 優先度は上位 4bit (16 段)。外部 IRQ 52 本。*PSPLIM/MSPLIM あり* (下回ると UsageFault)。例外フレームは 32B、FP を使った文脈では 104B (s0-s15, FPSCR, 予約を追加)。*lazy FP stacking*: FPCCR.ASPEN と LSPEN の両方が 1 のとき、例外入口は FP 領域だけ確保し、ハンドラが FP 命令を使ったときに初めて書く (ASPEN=0 なら自動保存そのものをしない)。CONTROL.FPCA = このスレッドが FP を触ったか (EXC_RETURN bit4 = 0 と対応)], [repo: `chip.hpp:24-35`、`armv8m.hpp:90,102-115`。RP2350 DS §3.7.2、Armv8-M ARM B3],
  [保護], [PMSAv8 MPU、セキュリティ状態ごとに 8 region。base/limit とも 32B 粒度、limit は含む側のブロック。*region の重なり不可* (広い窓に穴を開けられない)。属性は MAIR0/1 の索引。PRIVDEFENA あり。SAU 8 region], [repo: `chip.hpp:37-39`、`armv8m.hpp:317-337,404-423`。RP2350 DS §3.7.2],
  [タイマとカウンタ], [SysTick 24bit。DWT CYCCNT 32bit (DEMCR.TRCENA → DWT_CTRL.CYCCNTENA)。SoC の TIMER0/1 は 64bit µs], [repo: `armv8m.hpp:181-188,216-230`],
  [メモリ地図], [XIP `0x10000000` (XIP キャッシュ 16KiB 2-way)、SRAM `0x20000000`–`0x20082000` (520KiB)、SIO `0xD0000000`、PPB `0xE0000000`], [repo: `chip.hpp:14-20`],
  [Shizuku での使用], [`arch/armv8m/armv8m.hpp` (arch)、`armv8m_ctx.S` (入口)、`boards/rp2350_pico2/board.cpp:47-140` (結線・優先度・MPU)、`modules/pico_sdk_support/rp2350/arch_glue.cpp` (転送・窓)], [],
)
#note[*O3 の測定値について*: 3A の測定で O3 だけ O2 の 2 倍前後の値が出た。flash 上のコード配置が XIP キャッシュに収まらない仮説はあるが未検証。ホットパスを SRAM に置く (`docs/design/svc_in_ram.typ`) まで O3 の数字は比較に使わない。]

== ESP32-C6 (RV32IMAC × 1)

#table(columns: (1.1fr, 3fr, 1.2fr), inset: 3.5pt,
  table.header([項目], [内容], [出典]),
  [コア], [HP コア 1 個 (最大 160MHz) + LP コア (RV32IMAC、最大 20MHz)。Shizuku は HP コアだけ使う], [C6 DS (CPU and Memory)。コア数は repo: `chips/esp32_c6/chip.hpp:69`],
  [ISA と拡張], [RV32IMAC + Zicsr。*FPU なし*。A 拡張 (LR/SC、AMO) あり], [repo: `chips/esp32_c6/rv32_c6.hpp:5`],
  [特権と例外], [M と U の 2 モード (S なし)。ECALL は mcause 8 (U) / 11 (M)。*trap でレジスタは自動退避されない* (ARM のような例外フレームが無い) — 退避は全部ソフト。割込みは割込み行列で周辺 source を CPU 割込み 1..31 へ写し、PLIC 相当のレジスタで優先度・閾値・許可を付ける。mtvec は vectored (256B 整列の 32 スロット)。trap 中は MIE=0 なので、ECALL の処理中にタイマは入らない (ARM の「SVC 最優先」が構造で成り立つ)], [repo: `chips/esp32_c6/trap.cpp:377-407`、`systimer.hpp:7-17`],
  [保護], [PMP 16 entry、粒度 4B (TOR/NAPOT)。M モードは L ビットを立てない限り PMP を受けない。U モードは一致 entry が無ければ全部拒否。加えて Espressif 独自の PMA 16 entry (属性・M モードも縛れる)], [repo: `chip.hpp:70-73`、`pmp_c6.hpp`、`trap.cpp:56,428-433`],
  [タイマとカウンタ], [SYSTIMER 52bit (16MHz) の比較器を oneshot に使う。サイクルカウンタは標準の mcycle ではなく独自 CSR: `mpcer` (0x7E0) / `mpcmr` (0x7E1) / `mpccr` (0x7E2)。U モードからの読出しは未検証], [repo: `trap.cpp:1148-1160,1252-1260`、`chip.hpp:32-38`、`systimer.hpp:67-68`],
  [メモリ地図], [HP SRAM `0x40800000`–`0x40880000` (512KiB)、LP SRAM `0x50000000` 16KiB、flash の命令写像 `0x42000000`、周辺 `0x60000000`– (INTMTX `0x60010000`、SYSTIMER `0x6000A000`)], [repo: `systimer.hpp:7-10`],
  [Shizuku での使用], [`arch/riscv/rv32.hpp` + `rv32_trap.S` (arch)、`chips/esp32_c6/chip.hpp` の `plic_vector_mode`、`chips/esp32_c6/trap.cpp` (1287 行: trap 配送、ecall 表、PMP 切替え、kstack、診断)], [],
)

== ESP32-C5 (RV32IMAC × 1)

#table(columns: (1.1fr, 3fr, 1.2fr), inset: 3.5pt,
  table.header([項目], [内容], [出典]),
  [コア], [HP コア 1 個 (最大 240MHz) + LP コア (最大 48MHz)], [C5 DS (CPU and Memory)。コア数は repo: `chips/esp32_c5/chip.hpp:39`],
  [ISA と拡張], [RV32IMAC。データシートが明記するのはここまで。Zc 系などの追加拡張の有無は一次資料で確認できていない #chk], [C5 DS],
  [特権と例外], [M / U。割込みは *CLIC* (mtvec mode=3 の直接エントリ + mtvt の関数ポインタ表)。Shizuku は全割込みを non-vectored (clicintattr.shv=0) にして 1 入口へ集める。CLIC は mcause に mpp/mpie/mpil を載せるので、例外コードは下位 12bit で比べる], [repo: `chip.hpp:13-17`、`arch/riscv/rv32_trap.S` の mcause マスク],
  [保護], [PMP 粒度 128B (ESP-IDF の `SOC_CPU_PMP_REGION_GRANULARITY`)。entry 数は 16 と推定 (未確認)。M-only pilot では PMP を張っていない], [repo: `chip.hpp:41-47`],
  [タイマとカウンタ], [SYSTIMER。サイクルカウンタは未使用 (`HAS_CYCLE_COUNTER = false`)], [repo: `chip.hpp:19-21`],
  [メモリ地図], [HP SRAM 384KiB、LP SRAM 16KiB], [C5 DS],
  [Shizuku での使用], [`chips/esp32_c5/` (C6 と同じ形の `clic_direct_mode`)。fail-closed で、`SHIZUKU_C5_SKELETON_PERMITTED` が無いとビルドを拒否する], [repo: `rv32_c5.hpp:14-16`],
)

== 横並び比較

#table(columns: (1.4fr, 1fr, 1fr, 1fr, 1fr), inset: 3.5pt,
  table.header([項目], [RP2040], [RP2350], [ESP32-C6], [ESP32-C5]),
  [コア数 (カーネルが使う)], [2], [2], [1], [1],
  [最大クロック], [133MHz], [150MHz], [160MHz], [240MHz],
  [FPU], [なし], [単精度 (lazy)], [なし], [なし],
  [原子命令], [なし (SIO spinlock)], [LDREX/STREX], [LR/SC, AMO], [LR/SC, AMO],
  [syscall 命令], [SVC], [SVC], [ECALL], [ECALL],
  [HW が積むもの], [32B 固定], [32B / 104B], [なし], [なし],
  [遅延切替え], [PendSV], [PendSV], [ソフトの旗 (trap 出口)], [同左],
  [優先度], [2bit], [4bit], [PLIC 1–15 #chk], [CLIC],
  [個別のフォールト例外], [なし], [あり], [(例外は全部 mtvec)], [同左],
  [スタック下限], [ソフト], [PSPLIM], [ソフト], [ソフト],
  [保護機構], [PMSAv6 × 8], [PMSAv8 × 8], [PMP × 16 + PMA], [PMP × 16? (128B)],
  [保護の粒度], [2 の冪 (≥256B)], [32B], [4B], [128B],
  [特権外の既定], [PRIVDEFENA], [PRIVDEFENA], [M は PMP を受けない], [同左],
  [サイクルカウンタ], [なし], [DWT CYCCNT], [独自 CSR 0x7E2], [未使用],
)

== 現状: cpu_manager にあるべき処理がどこにあるか

今の `source/cpu_manager/init.cpp` は `BOARD::init(BOARD::core_num())` を呼ぶだけで、中身は次に散っている。どれも SDK にも基板にも依存しないので、cpu_manager (または arch) へ寄せられる。

#table(columns: (1.6fr, 2.2fr, 1.4fr), inset: 3.5pt,
  table.header([処理], [今ある場所], [あるべき場所]),
  [例外ベクタの登録、SVC > SysTick > PendSV の優先度、SHCSR のフォールト有効化、DebugMon の SHPR 直書き (`0xE000ED18`)], [`boards/rp2350_pico2/board.cpp:47-124`、`boards/rp2040_pico_w/board_rp2040.cpp:155-174` (同じことを 2 回)], [cpu_manager (順序の規約) + arch (レジスタ)。board は SoC 割込み (USB、SIO FIFO) の優先度だけ],
  [保護の土台 (region0 = コード、残り無効、有効化)], [両 board の `protection_init` (ほぼ同文)], [cpu_manager。board は範囲を渡すだけ],
  [asm からカーネルへの転送 (`shizuku_svc_dispatch` 等 5 本) と `shizuku_restore_region_window`], [`modules/pico_sdk_support/rp2350/arch_glue.cpp:22-94`、`rp2040/arch_glue_v6m.cpp:108-171` (同文 2 本)], [カーネル共通 (ARCH で template)。SDK 依存は無い],
  [`unprivileged_floor` / `cycles_per_us` / `time_us`], [両 board に同文], [board 共通の基底 (pico-sdk 系で 1 本)],
  [「起こされたコアは自分で init を呼ぶ」規約], [`rp2350_pico2.hpp` のコメントだけ], [cpu_manager の `init_core` / コア起動手順],
  [特権切替え・PMP 切替え・初回起動・タイマ・pend (`plic_vector_mode`)], [`chips/esp32_c6/trap.cpp` (1287 行)], [特権・PMP・初回起動は arch/riscv + cpu_manager、割込み行列と SYSTIMER は chip],
  [arch 層がチップのマクロを見る (`SHIZUKU_C6_UMODE` 12 か所)], [`arch/riscv/rv32.hpp`], [chip が型で渡す特性値 + `if constexpr`],
  [カーネル本体がチップ名で分岐 (`SHIZUKU_RP2040`、`__arm__`、`__riscv`)], [`source/kernel/dispatch.cpp`、`thread.cpp`], [ARCH/CHIP の `constexpr` 特性値],
  [診断の `#if` (BOOT_STAGE_TRACE / SELFTEST_PROGRESS / CRASH_RECORD / BOOT_LOG_WAIT)], [`dispatch.cpp` 十数か所、`board_rp2040.cpp` 19、`arch_glue_v6m.cpp` 21], [構成が選ぶ DIAG 型 (無効は空 inline)],
)

== サンプル: 直した形

=== 方針

+ *カーネルへの入口は 4 本だけ* (`shizuku_svc` / `shizuku_switch` / `shizuku_tick` / `shizuku_fault`)。asm はどれか 1 本を 1 回呼ぶだけにする。今の SVC は 1 回で C を 4 回呼ぶ (`current_context`、`arm_pending_step`、`restore_region_window`、`current_context`)。
+ *SVC/ECALL は切り替えない*。結果を frame に書き (in-place call なら pc / sp / 特権 / 窓も書き換え)、同じスレッドへ戻る。切替えが要れば pend を立てるだけで、PendSV (RISC-V は trap 出口) の 1 か所で切り替える。
+ そのため *SVC 入口は r4-r11 を退避しない*。C が callee-saved を守る。呼び手の callee-saved を消すのは converter の責任 (D1 (c)、2026-10-27 決定) なので、カーネルが消す必要もない。SVC 入口は ARMv6-M でも 4 命令になる。
+ *EXC_RETURN はスレッドのスタックに置かない*。r4-r11 は PendSV がスレッドのスタックへ積むが、非特権スレッドが書き換えられる場所に EXC_RETURN を置くと、ハンドラモードへの復帰を偽造できる。C へ渡し、カーネルのメモリに持つ。
+ 次に走るスレッドの特権・スタック下限・MPU/PMP の窓は、`shizuku_switch` の最後で `cpu_manager::prepare_resume` が 1 回で書く。
+ 優先度の規約 (SVC > タイマ > 切替え) は `static_assert` で縛り、自己テストでレジスタから読み戻して検算する。
+ ISA 差は `HAS_*` の `constexpr` と `if constexpr`、診断は DIAG 型で選ぶ。`#ifdef` を使わない。

#note[*未検討の点*: (1) in-place call で呼び手と呼び先が別のスタック窓を持つとき、`shizuku_svc` の中で PSP と MPU を書き換える順序。(2) RP2040 の lockout が SVC より上の優先度で入ること (SVC の中で止められても、SVC が文脈を切り替えないので壊れない、と見ているが未確認)。(3) RISC-V の速い ECALL 経路で、呼び手の ra / t\* / a2-a7 を消さずに返している点 (converter が clobber 宣言する前提)。サンプルは方向を示すもので、製品コードへの適用は S2 の設計として別に詰める。]

サンプルは `docs/design/arch/cpu_samples/` にある。ARMv8-M / ARMv6-M / RV32 の 3 ターゲットで C++ のコンパイル (clang 23、`-std=c++20 -O2 -c`) と asm の組立て (`arm-none-eabi-as`、`clang --target=riscv32`) が通ることだけを確かめた。実機では動かしていない。

=== 構成

#table(columns: (1.6fr, 3fr), inset: 3.5pt,
  table.header([ファイル], [役割]),
  [`common/kernel_entry.hpp`], [カーネルの入口 4 本と規約],
  [`common/cpu_manager.hpp`], [1 コアの一生: 起動、優先度、保護の土台、切替え直前の準備],
  [`common/diag.hpp`], [診断を型で選ぶ],
  [`arch/armv8m_entry.S`, `armv6m_entry.S`, `rv32_entry.S`], [例外入口 (SVC / 切替え / フォールト / 初回起動)],
  [`arch/armv8m.hpp`, `armv6m.hpp`, `rv32.hpp`], [ISA の操作 (同じ名前で揃える)],
  [`chips/rp2350.hpp`, `rp2040.hpp`, `esp32_c6.hpp`], [SoC の数 (コア数、優先度、region 数、メモリ地図)],
  [`boards/pico2.hpp`], [基板の配線と起動コードとの継ぎ目],
)

=== ARMv8-M の入口
#src("cpu_samples/arch/armv8m_entry.S")

=== ARMv6-M の入口
#src("cpu_samples/arch/armv6m_entry.S")

=== RV32 の入口
#src("cpu_samples/arch/rv32_entry.S")

=== カーネルの入口
#src("cpu_samples/common/kernel_entry.hpp")

=== cpu_manager
#src("cpu_samples/common/cpu_manager.hpp")

=== 診断
#src("cpu_samples/common/diag.hpp")

=== arch: ARMv8-M
#src("cpu_samples/arch/armv8m.hpp")

=== arch: ARMv6-M
#src("cpu_samples/arch/armv6m.hpp")

=== arch: RV32
#src("cpu_samples/arch/rv32.hpp")

=== chip と board
#src("cpu_samples/chips/rp2350.hpp")
#src("cpu_samples/chips/rp2040.hpp")
#src("cpu_samples/chips/esp32_c6.hpp")
#src("cpu_samples/boards/pico2.hpp")
