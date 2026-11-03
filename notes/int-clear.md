# 整数レジスタ (r4-r11) のカーネル消去に関する調査・設計報告

状態: 課題 4 (ARM r4-r11 のカーネル消去) および Argon レビュー指摘 (罠 A・罠 B・tail call) の反映完了。
C++ SVC ハンドラはベクタ直結のまま維持し、EXIT / GENERIC 専用の薄い asm 入口 (`shizuku_armv8m_exit_entry` / `shizuku_armv8m_forward_child_exit_entry` / `shizuku_armv8m_generic_entry`) を追加。C++ 側が「caller へ復帰したか (unwound)」を返し、asm 共通テールで復帰時のみ r4-r11 をゼロクリアし、callee 失敗時は値を温存する方式を採択・実装。

---

## 1. 例外フレームによる整数レジスタ消去 (確定事実)

- **対象**: `r0-r3`, `r12`, `lr`, `pc`, `xPSR`
- **事実**: 
  - ARMv8-M のハードウェア例外復帰機構により、これらのレジスタは例外フレーム (スタック上の 8 語) から自動的に復元される。
  - カーネルの `call_resume_at` (`arch/armv8m/armv8m.hpp:511-514, 799-801`) において、`zero_exception_frame(*next)` が `r0-r3`, `r12`, `lr` をゼロクリアした上で、`next->pc = pc`, `next->xPSR = 1u << 24` を設定している。
  - その直後、`dispatch.cpp:705` の `ARCH::set_result(*frame, error, value)` により `r0` (error) および `r1` (value) が上書きされる。
  - したがって、戻り値以外の `r2, r3, r12, lr` は**カーネルによって既に確実にゼロクリアされている**。

---

## 2. r4-r11 の扱いとハンドラのエピローグ (確定事実)

- **対象**: `r4-r11` (Callee-saved 汎用整数レジスタ 8 語)
- **事実**:
  - `r4-r11` は ARMv8-M の例外フレームに含まれないため、ハンドラが `bx lr` (例外復帰) を行う時点の物理レジスタ値がそのまま caller に露出する。
  - 実機ファームウェア (`shizuku.elf`) の逆アセンブル結果:
    1. **`shizuku_armv8m_svc_handler`** (`0x1000722c`):
       - `push {lr}` で番号判定し、`ldr.w lr, [sp], #4` で復元後、`b.w exit_method` (`0x10005488`) へ **tail call** している。`r4-r11` は push/pop していない。
    2. **`exit_method`** (`0x10005488`) / **`forward_child_exit`** (`0x10004e84`):
       - 普通の C++ 関数としてコンパイルされており、AAPCS 規約に従いプロローグで `stmdb sp!, {r4, r5, r6, r7, r8, r9, sl, fp, lr}` を実行し、`r4-r11` をスタックに退避している。
       - エピローグ (`0x10005666`, `0x10005044`) では `ldmia.w sp!, {r4, r5, r6, r7, r8, r9, sl, fp, pc}` を実行してスタックから値を復元し、`pc` に `EXC_RETURN` をポップして復帰している。
  - **結論**:
    - ハンドラ (C++ 関数) の実行開始時にスタックに保存されるのは、**Callee が SVC 発行時に残した `r4-r11` の値**である。
    - ハンドラ内部の C++ コードやインラインアセンブリでどれだけ `r4-r11` を 0 クリアしても、関数の最後でコンパイラが生成したエピローグ (`ldmia sp!, {r4-r11, pc}`) が走り、**スタックから Callee の値で上書き復元してしまう**。

---

## 3. レジスタ消去の担当箇所と改訂新契約

- **Callee への入口**:
  - `armv8m_abi.hpp`: Caller converter の `r4-r11, lr` ゼロ化 (旧 154-162 行) を削除。
  - 呼び出し元が入力レジスタとして活用可能とし、caller 自身のレジスタ保護は clobber 指定 (`"r4", "r5", "r6", "r8", "r9", "r10", "r11", "lr"`) によるコンパイラの退避に委ねる。
- **Callee からの出口 (カーネル保証)**:
  - `arch/armv8m/armv8m_ctx.S`: EXIT / GENERIC 専用薄 asm 入口 (`shizuku_armv8m_exit_entry` / `shizuku_armv8m_forward_child_exit_entry` / `shizuku_armv8m_generic_entry`) が C++ ハンドラのエピローグ後、**Caller へ復帰した (unwound == true) 場合のみ** `r4-r11` を 0 クリアして例外復帰する。
  - Callee が return stub を迂回して直接 `EXIT_METHOD` (2) や `primitive::RETURN` (1) を打った場合でも、カーネル側で漏洩が完全に遮断される。
  - Callee 自身に失敗が返る経路 (UNWIND_REJECTED / DEPTH_MISMATCH 等) では、Callee 自身のレジスタ値を破壊しないようゼロ化をスキップする (`cbz r0, 1f`)。
- **重複消去の確認**:
  - `shizuku_armv8m_return_stub` は FP レジスタ (`s0-s15, fpscr`) のみ消去しており、`r4-r11` は消去していないため、重複による不要コードは存在しない。

---

## 4. 罠 A: Caller へ pop 復帰する全経路と r4-r11 消去の完全性

### (1) Caller へ pop 復帰する全経路の列挙
Shizuku の IPC において、スタック上のフレームを巻き戻して caller へ復帰する全経路は以下の 4 経路に集約される：

1. **直接 `EXIT_METHOD` (2) 経路**:
   - `internal_headers/shizuku/armv8m_svc.hpp:156`: `direct<svc_direct_kind::EXIT_METHOD>`
   - `internal_headers/shizuku/native_call.hpp:709`: `native::exit_in_place<KERNEL, KERNEL_OBJECT, false>`
   - 巻き戻し実行: `unwind_in_place` (`native_call.hpp:348`)
2. **直接 `FORWARD_CHILD_EXIT` (27) 経路**:
   - `internal_headers/shizuku/armv8m_svc.hpp:159`: `direct<svc_direct_kind::FORWARD_CHILD_EXIT>`
   - `internal_headers/shizuku/native_call.hpp:709`: `native::exit_in_place<KERNEL, KERNEL_OBJECT, true>`
   - 巻き戻し実行: `unwind_in_place` (`native_call.hpp:348`)
3. **generic 経由 `primitive::RETURN` (1) 経路 (`return_to_depth` / `reply`)**:
   - `internal_headers/shizuku/armv8m_svc.hpp:74`: `generic` -> `svc_dispatch`
   - `source/kernel/dispatch.cpp:550`: `case primitive::RETURN:`
   - 巻き戻し実行: `call_unwind` (`source/kernel/dispatch.cpp:579`)
4. **generic 経由の `EXIT_METHOD` / `FORWARD_CHILD_EXIT` フォールバック経路**:
   - `internal_headers/shizuku/armv8m_svc.hpp:134, 204`: direct が decline した際の generic フォールバック
   - `source/kernel/dispatch.cpp:656`: `KERNEL::svc_exit` -> `native::exit_in_place`

### (2) 復帰先に応じた r4-r11 消去の動作
- **Caller への正常復帰時**:
  - `direct` および `generic` は、コンテキストが巻き戻されたこと (`thread.call_stack.depth != depth || (uintptr_t)context->sp != psp`) を検知し、`return_resumed` を実行して `true` (unwound = true) を返す。
  - asm 共通テール `.Lshizuku_armv8m_exit_clear_and_return` において `cbz r0, 1f` を通過し、`r4-r11` の全 8 語を `0` に消去して `bx r12` で例外復帰する。
  - これにより、全復帰経路で Callee の機密値が Caller へ漏洩することが完全に防止される。
- **Callee へのエラー返却時**:
  - 引数不正 (UNWIND_REJECTED / DEPTH_MISMATCH) や権限違反 (NOT_PRIVILEGED) の場合、深さ・SP・PC は変化しないため `return false` (unwound = false) が返る。
  - asm 共通テールで `cbz r0, 1f` により分岐し、`r4-r11` のゼロ化をスキップしてそのまま例外復帰する。
  - Callee 自身にエラーが返る経路で Callee の生きたレジスタが破壊される不具合を回避する。
- **スレッド切替 (`return_pended`) 時**:
  - `PendSV` による遅延切替の場合も `false` を返し、SVC 出口でのゼロ化をスキップ。PendSV の `CTX_SAVE` が現在スレッドの正しいレジスタ値を退避する。

### (3) テストで届かない経路とその理由
- **`exit_thread` (最外メソッド終了 `d == 0 && call_count == 0`)**:
  - スレッドそのものが終了 (`thread_state::TERMINATED`) するため、pop して復帰すべき caller フレームが存在しない。
  - スレッドスケジューラが次の実行可能スレッドを選択し、`do_switch` / `PendSV` 経由で切替が行われる。
- **`FORWARD_CHILD_EXIT` の親ハンドラ経路**:
  - 専用の親ハンドラ (HANDLER オブジェクト) からのみ発行可能であり、通常オブジェクトから呼ぶと `NOT_PRIVILEGED` で即座に callee へエラー返却される。
  - 親ハンドラによる正常 forward 巻き戻しは `exit_in_place<..., true>` を共有しており、本質的に `EXIT_METHOD` と同一の消去ゲートを通過する。

---

## 5. 罠 B: Caller Converter の作業レジスタと CONTROL レジスタの検証

- **背景**:
  - `internal_headers/shizuku/armv8m_abi.hpp:137-150` において、Caller converter は `r4=sp`, `r5=control`, `r6=r7(fp)`, `lr=aligned_sp` を作業用に使用している。
  - caller converter からゼロ化を外したため、callee の入口でこれら caller の生レジスタが見える。
  - ユーザー方針: 「入口で caller 自身の値が見えるのは caller の自己責任」として漏洩扱いしない。
- **カーネル由来情報の混入確認 (検証結果)**:
  - `mrs r5, control` は `svc 0` の**前**（caller の非特権 Thread モード空間）で実行されている。
  - CONTROL レジスタのビット構成:
    - bit 0 (`nPRIV`): ユーザーモード実行時 `1` (非特権)。
    - bit 1 (`SPSEL`): ユーザースレッド実行時 `1` (PSP 使用)。
    - bit 2 (`FPCA`): caller の浮動小数点演算使用有無フラグ (ハードウェア管理)。
  - カーネルに突入する前の純粋な caller 実行時状態でサンプリングされているため、**カーネルの特権状態 (nPRIV=0) やカーネル内部の状態は一切混入していない**。
  - したがって、callee に見える CONTROL の値は caller 自身の状態そのものであり、カーネル由来の機密漏洩は存在しない。

---

## 6. ベクタ表の EXIT 経路・番号分岐の構造と性能影響

### (1) 現状のベクタ表と分岐の仕組み
- ARMv8-M (Cortex-M33) はハードウェア的に単一の SVCall 例外ベクタ (ベクタ 11, VTOR オフセット `0x2C`) を持つ。
- `armv8m_svc.hpp` において、Thumb エイリアスにより `isr_svcall` および `SVC_Handler` が C++ 関数 `shizuku_armv8m_svc_handler` に直結されている。
- SVC 番号は `svc 0` のため即値には乗らず、PSP 例外フレーム上の `r0` に格納されている。
- `shizuku_armv8m_svc_handler` は PSP から `r0` をロードし、C++ の `switch` 文によるジャンプテーブル (TBB/TBH) で各 API へ tail call する。

### (2) 採用した方式 (命令数を増やさない EXIT/GENERIC 専用 asm 入口)
- C++ ハンドラ `shizuku_armv8m_svc_handler` はベクタ直結のまま完全に維持する。
- `switch` の分岐先として、`CALL_METHOD` 等は元の C++ 直結 tail call のまま 1 命令も増やさない。
- `EXIT_METHOD`, `FORWARD_CHILD_EXIT`, および `default (GENERIC)` のみ、薄い asm 入口 `shizuku_armv8m_exit_entry` / `shizuku_armv8m_forward_child_exit_entry` / `shizuku_armv8m_generic_entry` を呼ぶ。
- asm 入口の処理:
  ```assembly
  push {r0, lr}                        // exc_return と lr を退避 (MSP 8 バイト境界維持)
  bl   shizuku_armv8m_exit_method_body   // 通常の C++ ハンドラ実行 (bool unwound を返却)
  // (forward_child_exit_entry, generic_entry も同型)
  
  .Lshizuku_armv8m_exit_clear_and_return:
  pop  {r12, lr}                       // r12 = exc_return
  cbz  r0, 1f                          // caller へ復帰していない場合はゼロ化をスキップ
  movs r4, #0
  movs r5, #0
  movs r6, #0
  movs r7, #0
  mov  r8, r4
  mov  r9, r4
  mov  r10, r4
  mov  r11, r4
  1:
  bx   r12                             // 例外復帰
  ```
- **オーバーヘッド**:
  - `CALL_METHOD`, `CALL_DESCRIPTOR`, `GET_CURRENT`, `SET_BUDGET` 等: **0 命令 (影響ゼロ)**。
  - `EXIT_METHOD` (正常復帰): 約 13 サイクル。
  - `EXIT_METHOD` (エラー返却): 約 5 サイクル (ゼロ化スキップ)。

---

## 7. tail call (`b`) の確認方法

`shizuku_armv8m_svc_handler` から `shizuku_armv8m_exit_entry` / `shizuku_armv8m_forward_child_exit_entry` / `shizuku_armv8m_generic_entry` への分岐が、スタック枠を積まない tail call (`b` / `b.w`) になっていることの確認手順：

- **確認対象シンボル**: `shizuku_armv8m_svc_handler`
- **確認コマンド**:
  ```bash
  arm-none-eabi-objdump -d --demangle <firmware_elf> | grep -A 50 "<shizuku_armv8m_svc_handler>:"
  ```
- **確認基準**:
  - `switch` のジャンプテーブル出口において、`shizuku_armv8m_exit_entry` 等への呼び出しが `bl` ではなく `b` または `b.w` であること。
  - これにより、`shizuku_armv8m_svc_handler` の MSP スタックフレームが残らず、asm 入口の `bx r12` で正しく例外復帰が行われる。

---

## 8. 戻り値拡張案 (案 2: 3〜4 語への拡張設計)

現状の Shizuku IPC 戻り値は 2 語 (`r0` = error, `r1` = value) に固定されている。
将来的に 3 語または 4 語 (最大 128 ビット) の戻り値を受け渡す拡張口の設計案:

1. **AAPCS 例外フレームの活用**:
   - ARMv8-M のハードウェア例外フレームには `r0, r1, r2, r3` が含まれる。
   - 現在の `call_resume_at` は `zero_exception_frame` で `r2, r3` を 0 クリアしている。
   - `ARCH::set_result(frame, error, value0, value1, value2)` のように拡張し、`frame.r2`, `frame.r3` に追加の戻り値を設定する。
2. **callee-saved レジスタ (r4-r11) との干渉なし**:
   - `r2, r3` は caller-saved かつ例外フレーム経由でアンスタックされるため、asm exit 入口による `r4-r11` のゼロクリアと一切干渉しない。
   - カーネル消去の安全性を損なうことなく、戻り値レジスタを 4 語まで自然に拡張できる。

---

## 9. レジスタ消去の共通契約 (RV32 と ARMv8-M の対応表)

共通ヘッダ `internal_headers/shizuku/register_isolation_contract.hpp` にて、両アーキテクチャの復帰時レジスタ契約を constexpr 配列で定義した。

| 役割 (`reg_role`) | RV32 (ESP32-C6) | ARMv8-M (RP2350) | 備考 |
| :--- | :--- | :--- | :--- |
| **RETURN_ERROR** | `a0` (x10) | `r0` | 呼出し結果エラーコード (成功時 0) |
| **RETURN_VALUE** | `a1` (x11) | `r1` | 主戻り値 |
| **PRESERVED_SP** | `sp` (x2) | `sp` (r13 / PSP) | Caller スタックポインタに復帰 |
| **PRESERVED_SYS**| `gp` (x3), `tp` (x4) | なし | システム管理ポインタ (カーネルは触らない) |
| **RESTORED_PC**  | `sepc`/`mepc` | `pc` (r15 / stacked) | Caller 再開アドレス |
| **SCRUBBED_ZERO**| `ra`, `t0-t6`, `a2-a7`, `s0-s11`, `x0` (計 27 語) | `r2`, `r3`, `r4-r11`, `r12`, `lr` (計 12 語) | **カーネルが完全に 0 クリアを保証** |

- **消去の実現方式**:
  - RV32: M-mode `ecall` exit のアセンブリ (`rv32_trap.S:380-425`) にて、`mret` 直前に全対象レジスタを `mv reg, zero` で消去。
  - ARMv8-M: ハードウェアアンスタックによる `r2, r3, r12, lr` の消去 (`zero_exception_frame`) ＋ 薄 asm 入口 (`shizuku_armv8m_exit_entry` / `shizuku_armv8m_generic_entry`) による `r4-r11` の消去。

---

## 10. 迂回漏洩試験 (`register_leak_test.cpp`)

本改訂において、以下の 4 つの直交する漏洩・保持試験を実装：

1. **直接 EXIT 迂回正常復帰 (cmd=8, callee command=89)**:
   - Callee が return stub を通らず直接 `EXIT_METHOD` (2) を打つ (`r4-r11` 汚染)。
   - Caller 復帰直後の `r4-r11` が全 8 語厳密に `0` であることを検証 (`leak direct EXIT bypass int clean`)。
2. **直接 EXIT callee 失敗温存 (cmd=9, callee command=90)**:
   - Callee が不正な target depth (99) で `EXIT_METHOD` を打ち、`UNWIND_REJECTED` を受け取る。
   - Callee 自身への復帰であるため、Callee の `r4-r11` が `0xdeadbeef` のまま温存されていることを検証 (`leak direct EXIT callee failure int preserved`)。
3. **generic RETURN 迂回正常復帰 (cmd=10, callee command=91)**:
   - Callee が return stub を通らず generic 経由の `primitive::RETURN` (1) を直接打つ (`r4-r11` 汚染)。
   - Caller 復帰直後の `r4-r11` が全 8 語厳密に `0` であることを検証 (`leak generic RETURN bypass int clean`)。
4. **generic RETURN callee 失敗温存 (cmd=11, callee command=92)**:
   - Callee が不正な target depth (99) で `primitive::RETURN` (1) を打ち、`DEPTH_MISMATCH` を受け取る。
   - Callee 自身への復帰であるため、Callee の `r4-r11` が `0xdeadbeef` のまま温存されていることを検証 (`leak generic RETURN callee failure int preserved`)。
