# 計画書: C6 IPC 最適化の ARM (RP2350 / RP2040) バックポートと U→U 往復実測 — 段 1: 棚卸しと案

- **状態**: 提案 (段 1 完了)
- **作成日**: 2026-10-25
- **対象リポジトリ**: `/Users/ishigakiyua/github/Shizuku-v3real-armu` (ブランチ `exp/arm-umode`, `df54a90`)
- **参照元発注書**: `/Users/ishigakiyua/github/Shizuku/.handoff/v3cost-20261022/arm-ipc-backport.md`
- **成果物パス**: `/Users/ishigakiyua/github/Shizuku/.handoff/v3cost-20261022/arm-ipc-backport-plan.md`

---

## 0. 目標と背景

### 0.1 目標 (ユーザー決定 2026-10-25)
- seL4 との比較において、**Cortex-A9 は RP2350 (Cortex-M33) と比較する**。
- RP2350 の非特権間 (U→U) の往復コスト (method call 1 段、平均) を、**seL4 公式表 (2026-10-24 生成) の Cortex-A9 既定 717 cyc / MCS 726 cyc より小さくする (目標: 700 cyc 以下)**。
- C6 (RV32) は U54 (既定 1446 cyc / MCS 1572 cyc) と比較する。

### 0.2 現在のベースライン数値 (事実)
- **RP2350 (Pico 2, `df54a90`)**:
  - 特権の method call 1 段: **2065 cyc** (保護導入前: 1891 cyc)
  - SVC 往復: **456 cyc**
  - selftest: 282 通過 / 0 失敗
  - U→U 往復: **未測定** (現状の構造から **約 2500〜2800 cyc と推定**)
- **C6 の U→U (`9736cf1`, `exp/uu-cost`)**:
  - method call 1 段 (net mean): **1513 cyc** (2048 回平均、空ループ差引き)
  - desc call 1 段 (net mean): **1565 cyc** (2048 回平均、空ループ差引き)
  - 計測コード: `source/selftest/c6_u_cost.cpp`

---

## 1. 棚卸し表: C6 側の性能コミットと ARM 側の対応状況

C6 (RISC-V) 向けに実装された IPC 最適化コミットの git 実物 diff を検証し、ARM (ARMv8-M / RP2350, ARMv6-M / RP2040) 側に取り込まれていない変更を棚卸しした。
対象集合: `git log` で検出された性能関連コミット群のうち、アーキテクチャ境界を跨いで ARM 側に未反映の 10 項目。

| No | C6 側のコミット (hash / 件名) | C6 側での実装内容 (事実) | ARM 現状の実装箇所 (事実: ファイル:行・関数) | 移せるか (RP2350 / RP2040) | 見込み削減 (推定) |
|---|---|---|---|---|---|
| 1 | `0d45133` perf(v3): ecall を asm 入口から a0 の番号で直接振り分ける<br>`e9d267a` refactor(v3): svc_dispatch を呼び出し元の立場と番号ごとの svc_* に分ける | `rv32_trap.S:89-98` で a0 番号から `shizuku_rv32_ecall_table` (O(1) 配列) を直接引き、各 API ハンドラへ `jalr`。巨大な `svc_dispatch`、`trap_dispatch`、ラムダを ECALL 成功経路から全廃。 | `arch/armv8m/armv8m_ctx.S:129` で固定で `bl shizuku_svc_dispatch` を呼ぶ。<br>`modules/pico_sdk_support/rp2350/arch_glue.cpp:25` 経由で `source/kernel/dispatch.cpp:785` の巨大 `svc_dispatch` (if/switch 判定連鎖) を通過。 | **移せる**<br>RP2350: 可<br>RP2040: 可 | **約 50〜80 cyc** (推定)<br>多段 if 判定、多重関数呼出し、スタック消費の削減。 |
| 2 | `41b4e2c` perf(v3): C6 の ecall を 1 API = 1 関数に — 記帳・context_t・多重の関数呼出しを成功経路から外す | 切替えの起きない API (CALL_METHOD 等) を `chips/esp32_c6/trap.cpp` で `ecall_direct` (個別関数) に展開。`context_t` を組まず trap フレームを直接扱い、記帳 (`trap_diag`, `trap_depth`, `active_*`, `pend_flag`)、`current_context()` 多重呼出し、144B ゼロクリアを成功経路から除外。 | `arch/armv8m/armv8m_ctx.S:33-59` で `shizuku_current_context()` を呼んで `context_t` 全体に保存し、`dispatch.cpp` 内で `context_t` 経由で引数操作。<br>記帳・多重関数呼び出しが残存。 | **移せる**<br>RP2350: 可<br>RP2040: 可 | **約 150〜250 cyc** (推定)<br>C6 の 1223→624 cyc 短縮の主因。ARM でも大きな効果が見込める。 |
| 3 | `1630dd4` perf(v3): method call / descriptor call を in-place に<br>`b2207db` refactor(v3): 呼出しの記録を user stack 上のヘッダ枠に<br>`37f5b47` feat(v3): ARMv6-M に caller stub と呼出しの記録の枠を入れ in-place に | `arch/riscv/rv32.hpp:209` で `CALL_IN_PLACE = true`。<br>caller の frame コピーを廃止し in-place 遷移。戻り先の権威は kernel の台帳 (C6 の U-mode で使っている SHIZUKU_LEAN_LEDGER と同構造) に保持し、user stack 上の枠は権威にしない。 | `arch/armv8m/armv8m.hpp` には `CALL_IN_PLACE` が未定義 (`native_call.hpp:26-31` で `false` 判定)。<br>`dispatch.cpp:646-649` で fallback し、`call_request` 構築 (`dispatch.cpp:651`)、`do_call` (`dispatch.cpp:406`)、`enter_call` で `call_frame_push` (フレームコピー) を実行。<br>※RP2040 には `37f5b47` で枠が入っているが RP2350 には未導入。 | **移せる**<br>RP2350: 可<br>RP2040: 枠はあるが armv8m 同等化要 | **約 200〜350 cyc** (推定)<br>`call_request` 構築、テーブル検索、カーネルスタックへの巨大フレームコピーを全廃。 |
| 4 | `817a162` perf(v3): ecall の入口・出口を caller-saved ABI に合わせる<br>`4cc424c` perf(v3): レジスタ素通し方式 | `rv32_trap.S` で M-mode ECALL は caller-saved (a0-a7) だけ保存。s0-s11 は保存しない。戻りも必要なレジスタのみ復元し不要なゼロクリアを省く。 | `arch/armv8m/armv8m_ctx.S:37-43` (`CTX_SAVE`) で r4-r11 と PSP を手動退避し、`CTX_RESTORE:75-82` で手動復元。<br>Cortex-M33 はハードウェアが自動で `{r0-r3, r12, lr, pc, xPSR}` を PSP に退避するのに、さらにソフトウェアで context_t へ退避している。 | **移せる**<br>RP2350: 可 (HW自動退避活用)<br>RP2040: 自動退避のみ活用可 | **約 40〜70 cyc** (推定)<br>stmia 9語 + ldmia 9語のメモリアクセス削減。※非特権境界での漏洩対策と両立必須。 |
| 5 | `2c2fd42` perf(v3): RISC-V (C6/C5) のカーネルを -O2 に — カーネル本体・カーネルオブジェクト・trap 入口だけ | `.bazelrc:87` で `build:esp32c6 --per_file_copt=^source/(kernel\|kernel_object\|cpu_manager)/,^chips/esp32_c6/trap\.cpp$@-O2` を指定。 | `.bazelrc` に RP2350 用の `per_file_copt` は存在せず、全体既定の `--copt=-Og` (`.bazelrc:67`) のままビルドされている。 | **即座に移せる**<br>RP2350: 可<br>RP2040: 可 | **約 100〜200 cyc** (推定)<br>関数のインライン展開、余分なスタックフレーム削除、レジスタ配置最適化。 |
| 6 | `732c2fa` build(v3): カーネル .a を作る段階で 1 回だけ LTO をかける `--config=kernel_lto`<br>`5c81b7e` build(v3): RP2040 のカーネルを -O2 に、--config=kernel_lto を RP2040 の最終リンクでも使えるように | GCC の部分リンク LTO (`modules/esp_idf_support/firmware/kernel_lto.sh`)。 | RP2350 用の LTO 設定は `.bazelrc` や BUILD.bazel に未整備 (RP2040 のみ `5c81b7e` で最終リンク LTO が整備済み)。 | **移せる**<br>RP2350: 可<br>RP2040: 既に対応済み | **約 20〜50 cyc** (推定)<br>C6 では -O2 に比べ LTO の上乗せ効果は小さかったが、クロス翻訳単位の最適化に寄与。 |
| 7 | `8569ddc` perf(arm): 保護窓の書換えを特権の呼出しと同じ窓の復帰で省く<br>`fd115cf` arch, chips: U-mode in-place PMP 切替え<br>`b66be1c` arch, chips: PMP 前計算切替え | 同じ保護ドメインへの復帰時に PMP 書換えをスキップ。CSR 値を前計算して一括書き込み。 | `arch/armv8m/armv8m_ctx.S:71` で `bl shizuku_restore_region_window` を例外復帰ごとに毎回呼び出し。<br>`modules/pico_sdk_support/rp2350/arch_glue.cpp:57-79` で MPU を直列書き換え。 | **移せる**<br>RP2350: 可 (PMSAv8)<br>RP2040: 可 (PMSAv6) | **約 60〜120 cyc** (推定)<br>毎回走る C 関数呼出しの排除と、同一ドメイン復帰時の MPU レジスタアクセス抑制。 |
| 8 | `30fec89` perf(v3): C6 の計測区間の caller/callee を IRAM (.iram1) へ置く<br>`e2b6706` perf(v3): place descriptor-call hot path in IRAM (C6) | 計測ループおよびホットパスを Flash から IRAM へ配置し、XIP キャッシュミス・フェッチウェイトを排除。 | RP2350 ではコードが XIP Flash 上に配置されており、キャッシュミスや Flash ウェイト (標準 2〜3 wait states) の影響を受ける。 | **移せる**<br>RP2350: 可 (`__not_in_flash_func`)<br>RP2040: 可 | **約 50〜100 cyc** (推定)<br>SRAM (520KB 内) 配置によるウェイトゼロ化。 |
| 9 | (独自検出) `shizuku_arm_pending_step` のホットパスからの分離 | C6 では GDB stub / デバッグステップ実行の確認は通常例外経路から分離されている。 | `arch/armv8m/armv8m_ctx.S:69` で `bl shizuku_arm_pending_step` を例外復帰ごとに毎回呼んでいる。<br>`modules/pico_sdk_support/rp2350/arch_glue.cpp:48` で C 関数を実行。 | **移せる**<br>RP2350: 可<br>RP2040: 可 | **約 15〜25 cyc** (推定)<br>通常時 (99.99%) はフラグが立っていないため、インラインでの 1 命令判定 (cbz) で関数呼出しをスキップ。 |
| 10 | `2bdf613` perf(v3): 記帳を特権の台帳 1 か所に集める試作 (SHIZUKU_LEAN_LEDGER)<br>`3992784` perf(v3): ヘッダと台帳の二重書きを整理 | ヘッダ枠と台帳への二重書きを解消し、特権台帳またはスタック進行のみに集約。 | `source/kernel/dispatch.cpp:406` (`do_call`) 内で `enter_call` により台帳記録とフレーム退避の両方を実行。 | **移せる**<br>RP2350: 可<br>RP2040: 可 | **約 30〜50 cyc** (推定)<br>CALL_IN_PLACE と連動して二重記帳を解消。 |

---

## 2. RP2350 で U→U を測る方法

### 2.1 測定条件 (`c6_u_cost` と完全同条件)
`source/selftest/c6_u_cost.cpp` (`9736cf1`) で確立された測定規約と同一条件で実施する。

1. **保護境界の分離**:
   - 呼び手 P (親: `OBJECT_U_COST_P`) も呼び先 C (子: `OBJECT_U_COST_C`) も非特権 (`OBJECT_UNPRIVILEGED`) で生成。
   - P と C はそれぞれ専用の data 窓 (`u_cost_p_data_pool[256]`, `u_cost_c_data_pool[256]`) と stack 窓 (`u_cost_p_stack[1024]`, `u_cost_c_stack[1024]`) を持ち、MPU 張替えが入る別保護ドメインの組とする。
2. **測定ループの構成**:
   - 標本数: **2048 回** (`ROUNDS = 2048`)。
   - ウォームアップ: 本番前に 32 回実行。
   - 空ループ (baseline) の差引き: `CALL_METHOD` / `CALL_METHOD_DESCRIPTOR` を発行しない同一命令数の空ループ (baseline) を 2048 回回し、経過サイクルを計測。
   - 算出式: `net_mean = (raw_total_cycles - baseline_total_cycles) / 2048`。
3. **測定対象の経路**:
   - method call 1 段 (cmd=3)
   - descriptor call 1 段 (cmd=6)
4. **検算 (Correctness Assertion)**:
   - ループ完走後、引数 `arg=41` で呼び出し、戻り値 `42` が返ること (`ok && val == 42 && err == 0`) を必ず検証。

### 2.2 DWT CYCCNT を誰が有効化し誰が読むか (現行コード実物確認)

- **現行コードの実物 (事実)**:
  - `arch/armv8m/armv8m.hpp:67`: `static constexpr bool HAS_CYCLE_COUNTER = true;`
  - `arch/armv8m/armv8m.hpp:214-216`:
    ```cpp
    static constexpr uintptr_t DWT_CTRL_ADDRESS = 0xE0001000u;
    static constexpr uintptr_t DWT_CYCCNT_ADDRESS = 0xE0001004u;
    static constexpr uint32_t DWT_CTRL_CYCCNTENA = 1u << 0;
    ```
  - `arch/armv8m/armv8m.hpp:220-232` (`cycle_counter_enable`):
    `DEMCR.TRCENA` (bit 16) を立て、`DWT_CYCCNT` を 0 クリアし、`DWT_CTRL.CYCCNTENA` (bit 0) を立てて 64 スピンでカウント進捗を確認。
  - `arch/armv8m/armv8m.hpp:234`: `static uint32_t cycle_counter_read() { return at(DWT_CYCCNT_ADDRESS); }`

- **特権境界と DWT 読み出しの規約**:
  - Cortex-M33 (ARMv8-M) において、PPB (Private Peripheral Bus: `0xE0000000–0xE00FFFFF`) はハードウェア規定により**特権アクセスのみ許可**されている。非特権 (U-mode) から PPB へのアクセスは直ちに MemManage / HardFault となる。
  - したがって、非特権の親 P や子 C に DWT を直接読ませてはならない（非特権保護の穴を作らない）。
  - **有効化の担当者**:
    - selftest 起動時、**特権 Thread mode** (ハーネス側: `rp2350_u_cost()`) が `ARCH::cycle_counter_enable()` を呼び出して有効化する。
  - **読み出しの担当者**:
    - `c6_u_cost.cpp` と同一設計とする。親 P (非特権) は asm ループ内で DWT を一切読まず、純粋に 2048 回の `svc` 呼出し (または空ループ) だけを回す。
    - **特権側 (`rp2350_u_cost`) が、親 P を `CALL_METHOD` でキックする直前と直後に `ARCH::cycle_counter_read()` を呼び出して全体の所要サイクル数を測定する**。
    - これにより、非特権コードに余計な権限を与えることなく、高精度かつ安全に測定できる。

---

## 3. 入れる順番と区切りごとの期待値

現在の RP2350 特権 2065 cyc に対し、U→U は MPU 書換え 2 回分が加わるため、現状は約 2500〜2800 cyc と推定される。
seL4 Cortex-A9 (717 cyc) を下回るまでの段階的導入ステップを以下のように計画する。

```
[現状推定: 2500〜2800 cyc]
       │
       ▼ (ステップ 0: 計測ハーネス追加・ベースライン確定)
[初期実測値の確定]
       │
       ▼ (ステップ 1: ビルド最適化 -O2/-O3 + レジスタ漏洩試験) ── 約 -300〜400 cyc
[約 2200〜2400 cyc (推定)]
       │
       ▼ (ステップ 2: ホットパス RAM 配置 + 不要フック分離) ── 約 -100〜150 cyc
[約 2100〜2250 cyc (推定)]
       │
       ▼ (ステップ 3: CALL_IN_PLACE 導入・フレームコピー廃止) ── 約 -600〜800 cyc
[約 1400〜1600 cyc (推定)] (※ C6 の U→U 1513 cyc と同等水準)
       │
       ▼ (ステップ 4: Direct SVC Table Dispatch + 1 API = 1 関数) ── 約 -400〜550 cyc
[約 900〜1100 cyc (推定)]
       │
       ▼ (ステップ 5: Cortex-M33 特性最適化: 自動退避活用 + MPU エイリアス) ── 約 -250〜350 cyc
[約 650〜750 cyc (推定)] ── ★ seL4 Cortex-A9 (717 cyc) 突破！
```

### 区切りごとの詳細と期待値

1. **ステップ 0: 測定ハーネスの配備 (ベースライン確定)**
   - ファイル: `source/selftest/rp2350_u_cost.cpp` (新規作成)
   - 内容: `c6_u_cost.cpp` と同構造の U→U 計測コードを ARMv8-M 用 asm で実装。
   - 期待値: **現状実測値の確定 (推定 2500〜2800 cyc)**。

2. **ステップ 1: ビルド設定の最適化 (-O2 / -O3 化) とレジスタ漏洩試験**
   - ファイル: `.bazelrc`, `source/selftest/register_leak_test.cpp`
   - 内容:
     - カーネル本体、kobj、`arch_glue.cpp` を `-O2` に設定。
     - 計測側 (selftest, apps, stub) の `-O2` / `-O3` プロファイル追加。
     - 呼び手⇄呼び先のレジスタ漏洩試験を実装。
   - 期待値: **約 2200〜2400 cyc (推定)** (約 300〜400 cyc 削減)。

3. **ステップ 2: ホットパスの SRAM 配置と不要フックのバイパス**
   - ファイル: `arch/armv8m/armv8m_ctx.S`, `modules/pico_sdk_support/rp2350/arch_glue.cpp`
   - 内容:
     - `shizuku_arm_pending_step` (GDB stub) の呼び出しをフラグ判定 (`cbz`) でスキップ。
     - SVC ハンドラおよび計測コードに `__not_in_flash_func` を付与し SRAM 配置。
   - 期待値: **約 2100〜2250 cyc (推定)** (約 100〜150 cyc 削減)。

4. **ステップ 3: CALL_IN_PLACE の ARMv8-M バックポート (特権台帳方式)**
   - ファイル: `arch/armv8m/armv8m.hpp`, `internal_headers/shizuku/native_call.hpp`
   - 内容:
     - `CALL_IN_PLACE = true` を定義。
     - 戻り先の権威は kernel の特権台帳 (C6 の U-mode で使っている SHIZUKU_LEAN_LEDGER と同構造) とし、user stack 上の枠は権威にしない。
     - `call_frame_push` による巨大フレームコピーを廃止し、台帳に最小限の呼出し情報 (戻り先等) を記録して in-place 遷移 (`native::call_method_in_place`) を有効化。
   - 期待値: **約 1400〜1600 cyc (推定)** (約 600〜800 cyc 削減、C6 の 1513 cyc と同水準)。

5. **ステップ 4: Direct SVC Table Dispatch と 1 API = 1 関数化**
   - ファイル: `arch/armv8m/armv8m_ctx.S`, `modules/pico_sdk_support/rp2350/arch_glue.cpp`
   - 内容:
     - asm 入口で r0 (API 番号) によるテーブル振り分け (`shizuku_armv8m_svc_table`) を実装。
     - `shizuku_svc_dispatch` (巨大 switch / if) をバイパスし、`native_call.hpp` の各 API 専用関数へ直接入る。
     - `context_t` の多重操作・記帳を成功経路から除外。
   - 期待値: **約 900〜1100 cyc (推定)** (約 400〜550 cyc 削減)。

6. **ステップ 5: Cortex-M33 ハードウェア特性の最大活用**
   - ファイル: `arch/armv8m/armv8m_ctx.S`, `arch/armv8m/armv8m.hpp`, `arch/ppb.hpp`
   - 内容:
     - Cortex-M33 のハードウェア自動退避フレーム `{r0-r3, r12, lr, pc, xPSR}` を直接書き換えて遷移。手動の `stmia`/`ldmia` (r4-r11) を成功経路から全廃。
     - MPU エイリアスレジスタ (`RBAR_A1..3 / RLAR_A1..3`) を用いた一括書き込み。
   - 期待値: **約 650〜750 cyc (推定)** (約 250〜350 cyc 削減)。
   - **判定**: **seL4 Cortex-A9 (717 cyc) を下回り、最速 IPC を達成**。

---

## 4. Cortex-M33 の特性を使う案 (未計測・推定)

### 案 A: SVC ハードウェア自動スタッキング (Exception Entry Context Stacking) の直接利用
- **原理と特性**:
  - Cortex-M33 は SVC 例外に入るとき、ハードウェアが自動的に `{r0-r3, r12, lr, pc, xPSR}` の 8 語 (32B) を現在のスタック (PSP) に push する。
  - 例外から抜ける際 (`bx lr` with `EXC_RETURN`)、ハードウェアが自動的にこれらを pop してスレッドモードへ復帰する。
- **現状の無駄 (事実)**:
  - `arch/armv8m/armv8m_ctx.S:37-43` でソフトウェアがさらに `r4-r11` と PSP を `context_t` に退避し、出口 (`75-82`) で復元している。
- **改善案**:
  - 同一スレッド内の同期 method call では、callee への遷移は「PSP 上のハードウェア例外フレームの PC を callee 入口アドレスに書き換え、r0-r3 を引数に書き換えて `bx lr` する」だけで完了する。
  - 呼び手 (caller) の復帰先 PC や退避情報は、user stack 上の枠ではなく kernel の特権台帳 (SHIZUKU_LEAN_LEDGER 相当) に安全に保持する。
  - また、呼び先 (callee) は信用できないため、呼び手の callee-saved レジスタ (r4-r11) を素通しにせず、カーネル側で特権台帳に退避してからクリアし、復帰時に台帳から復元する。
  - これにより、ソフトウェアによる `context_t` への多重退避や余分なメモリコピーをスキップできる。
- **見込み (推定)**: 約 50〜80 cyc 削減。

### 案 B: MPU エイリアスレジスタ (`RBAR_A1..3` / `RLAR_A1..3`) による一括書き込み
- **原理と特性**:
  - ARMv8-M PMSAv8 (Cortex-M33) では、MPU レジスタマップ (`0xE000ED90`) において、隣接するリージョン番号に対応するエイリアスレジスタが連続配置されている:
    - `0x0C`: `MPU_RBAR`, `0x10`: `MPU_RLAR` (リージョン N)
    - `0x14`: `MPU_RBAR_A1`, `0x18`: `MPU_RLAR_A1` (リージョン N+1)
    - `0x1C`: `MPU_RBAR_A2`, `0x20`: `MPU_RLAR_A2` (リージョン N+2)
    - `0x24`: `MPU_RBAR_A3`, `0x28`: `MPU_RLAR_A3` (リージョン N+3)
- **現状の無駄 (事実)**:
  - `arch/armv8m/armv8m.hpp:317-324` (`region_set`) では、リージョンごとに毎回 `ppb::mpu->rnr = index; ppb::mpu->rbar = ...; ppb::mpu->rlar = ...;` と個別にバスアクセスを行っている。
- **改善案**:
  - オブジェクトの保護窓 (Code, Data, Stack, Grant) を連続する 4 つのリージョン番号 (例: 4, 5, 6, 7) に固定配置する。
  - `arch/ppb.hpp` にエイリアスレジスタ構造体を追加し、アセンブリの `STMIA` 命令 (Store Multiple) を用いて、前計算済みの 8 語 (RBAR/RLAR × 4) を 1 命令でバースト書き込みする。
  - メモリバリア (`dmb`/`dsb`/`isb`) の発行も 4 回から 1 回に集約する。
- **見込み (推定)**: 約 40〜80 cyc 削減。

### 案 C: EXC_RETURN と CONTROL.nPRIV の直値制御
- **現状 (事実)**:
  - `arch/armv8m/armv8m_ctx.S:96-101` で CONTROL レジスタの bit0 (nPRIV) を書き換えるために、`mrs`, `bic`, `and`, `orr`, `msr CONTROL`, `isb` の read-modify-write を毎回行っている。
- **改善案**:
  - 非特権への復帰であることが静的に確定している経路では、固定値 (nPRIV=1, SPSEL=1) を即値で `msr CONTROL` する、あるいは EXC_RETURN の制御と一体化して不要なビット演算を省く。
- **見込み (推定)**: 約 10〜15 cyc 削減。

---

## 5. 追記要件の具体化 (Opus 確認済み, 2026-10-25)

### 5.1 計測側も -O2 / -O3 にする案

#### 5.1.1 背景と課題
- 現在、`2c2fd42` によりカーネル本体・kobj・trap 入口のみが `-O2` 化されているが、オブジェクト側 (`source/apps/`, `source/selftest/`) や stub (`method_entry.hpp` 等の template) は `-Og` のままビルドされている。
- template 実体はインライン化やレジスタ最適化を前提としており、`-Og` では不必要なスタックフレーム生成やメモリ退避が生じ、実力よりも遅い数字となってしまう。

#### 5.1.2 構成案と `.bazelrc` 設定
- 既存の `-Og` 計測との対照を取るため、以下の 3 構成を比較可能にする:
  1. **ベースライン (現行)**: カーネルのみ `-O2`、計測側 `-Og`
  2. **`--config=bench_opt_o2`**: カーネル + 計測側 (selftest/apps/stub) を一括して `-O2`
  3. **`--config=bench_opt_o3`**: カーネル + 計測側を一括して `-O3`
- `.bazelrc` 追加入 (案):
  ```bazel
  # 計測側 -O2 プロファイル
  build:bench_opt_o2 --per_file_copt=^source/(kernel|kernel_object|cpu_manager|selftest|apps)/,^modules/pico_sdk_support/rp2350/arch_glue\.cpp$@-O2
  build:bench_opt_o2 --per_file_copt=^chips/esp32_c6/trap\.cpp$@-O2

  # 計測側 -O3 プロファイル
  build:bench_opt_o3 --per_file_copt=^source/(kernel|kernel_object|cpu_manager|selftest|apps)/,^modules/pico_sdk_support/rp2350/arch_glue\.cpp$@-O3
  build:bench_opt_o3 --per_file_copt=^chips/esp32_c6/trap\.cpp$@-O3
  ```

#### 5.1.3 `.text` 増分の管理と対照表フォーマット
- `-O3` ではアグレッシブなループ展開や関数クローニングにより `.text` が肥大化する恐れがある (C6 の `2c2fd42` でも `do_grant 332 → 984` と増大)。
- 各レベルでのバイナリサイズ (`size` コマンド出力) と実行サイクル数を記録し、以下の対照表で評価する:

| 最適化レベル | 対象ファイル範囲 | RP2350 U→U (cyc) | C6 U→U (cyc) | `.text` サイズ増分 | 採否判断 |
|---|---|---:|---:|---:|---|
| 現行 (`-Og` 計測) | カーネルのみ -O2 | (実測値) | 1513 | 基準 (0 B) | ベースライン |
| **-O2 計測** | カーネル + 計測側 -O2 | (実測値) | (実測値) | 約 +1〜3 KB (推定) | **本命採用** (安全かつ高速) |
| **-O3 計測** | カーネル + 計測側 -O3 | (実測値) | (実測値) | 約 +5〜10 KB (推定) | コードサイズ増と cyc 削減の費用対効果で判定 |

---

### 5.2 レジスタ漏洩試験 (Register Leakage Prevention Test) の仕様

#### 5.2.1 目的
呼び先 (Callee) は信用できない保護ドメインであるため、呼び手 (Caller) の callee-saved レジスタ (`r4-r11` / `s0-s11`) を呼び先へ素通しにすること自体が情報漏洩となる。
カーネルは呼び手の callee-saved レジスタを特権台帳側に退避してから消去 (スクラブ) し、復帰時に台帳から復元しなければならない。
また最適化レベルを `-O2` / `-O3` に引き上げた場合にも、コンパイラのレジスタ割り付けや IPC 高速化によって意図しない漏洩が生じないよう、**「送信時は引数のみ、返信時は戻り値のみが伝達され、他の汎用レジスタは一切漏洩せず、呼び手の callee-saved レジスタが元通り復元されること」** を selftest で厳格に縛る。

#### 5.2.2 試験 A: 呼び手から呼び先への漏洩防止 (Caller → Callee)
- **対象**:
  - C6 の U→U (RISC-V)
  - RP2350 の非特権間 (ARMv8-M)
- **手順**:
  1. 呼び手 (Caller) は、`CALL_METHOD` 発行直前に、引数レジスタ (`a0-a3` / `r0-r3`) 以外のすべての汎用レジスタ (Caller-saved: `t*` / `r12, lr`、Callee-saved: `s*` / `r4-r11`) に固有のカナリア目印値 (例: `0xDEAD0000 | reg_num`) をセットする。
  2. 呼び先 (Callee) の入口アセンブリにおいて、引数レジスタ以外の全レジスタの値を即座に退避バッファにコピーする。
  3. 検証アサーション:
     - Callee 側から見て、引数以外のどのレジスタにも Caller がセットした目印値 `0xDEAD....` が**残っていない**こと (0 またはカーネル既定値でスクラブされていること) を `assert`。特に Caller の callee-saved (`s*` / `r4-r11`) が素通しで Callee に見えていないことを確認。

#### 5.2.3 試験 B: 呼び先から呼び手への漏洩防止と callee-saved 復元 (Callee → Caller)
- **対象**:
  - C6 の U→U (RISC-V)
  - RP2350 の非特権間 (ARMv8-M)
- **手順**:
  1. 呼び先 (Callee) は、処理完了後・`EXIT_METHOD` (または `ret`) 発行直前に、戻り値レジスタ (`a0/a1` / `r0/r1`) 以外のすべての汎用レジスタに独自のカナリア目印値 (例: `0xCAFE0000 | reg_num`) を書き込む。
  2. 呼び手 (Caller) は、メソッド呼び出しから復帰した直後、全レジスタの値を検証バッファに回収する。
  3. 検証アサーション:
     - **漏洩防止**: 戻り値以外のどのレジスタにも Callee がセットした目印値 `0xCAFE....` が**残っていない**こと。
     - **完全復元**: 呼び手自身の Callee-saved レジスタ (`s0-s11` / `r4-r11`) が、呼び出し前に Caller 自身が持っていた元の値に**完全に復元されている**こと。

#### 5.2.4 登録場所
- ファイル: `source/selftest/register_leak_test.cpp`
- `BUILD.bazel` の `shizuku_selftest` ターゲットにテストケースとして登録し、`-O2` および `-O3` ビルドの両方で pass することを必須受入条件とする。

---

## 6. 層境界・アーキテクチャ規律の遵守

- **チップ名マクロの排除**:
  - 共通コード (`internal_headers/shizuku/`, `source/kernel/`) に `#if defined(SHIZUKU_RP2350)` などのチップ名マクロを追加しない。
  - アーキテクチャやチップの特性は `ARCH::CALL_IN_PLACE`, `ARCH::HAS_CYCLE_COUNTER`, `ARCH::MPU_REGION_COUNT` などの `constexpr` 特性値と `if constexpr` によって静的に分岐させる。
- **レイヤ境界の保護**:
  - `chips/*` の内部コードが上位の `source/kernel/` の具象クラスに直接依存しない構造を維持する。
  - モジュール層 (`modules/pico_sdk_support/`) と ISA 層 (`arch/armv8m/`) の役割分担を守る。
- **テストの非弱体化**:
  - 既存の 282 件の selftest のアサーションを一切弱めず、レジスタ漏洩試験などのセキュリティテストを新規追加する。

---

## 7. 段 1 における未確認事項・不確実な点 (事実と推定の境界)

1. **RP2350 の XIP Flash ウェイトと SRAM 配置の効果 (未確認・推定)**:
   - RP2350 の XIP Flash キャッシュが IPC ループ (2048 回) のヒット率に与える影響は未実測。SRAM 配置による短縮幅 (50〜100 cyc) は C6 での実績値に基づく推定であり、実機での検証を要する。
2. **Cortex-M33 ハードウェア自動退避フレーム直接書き換えの例外復帰安全性 (未確認)**:
   - PSP 上のスタックフレームを直接改変して `bx lr` (EXC_RETURN) すること自体は ARMv8-M アーキテクチャ仕様上完全に準拠しているが、FPU 遅延スタッキング (Lazy FP stacking) が有効な場合の挙動 (FPCA ビットの整合性) について実機での確認が必要。
3. **`-O3` におけるインライン展開とコードサイズ増 (未確認)**:
   - `-O3` 適用時のコードサイズ増加量が Pico 2 の SRAM/Flash バジェットに対してどの程度の影響を及ぼすかは、実際のビルド成果物での `size` 計測を待つ必要がある。
