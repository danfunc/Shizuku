# 整数レジスタ (r4-r11) のカーネル消去に関する調査・設計報告

状態: 課題 4 (ARM r4-r11 のカーネル消去) 実装完了。
C++ SVC ハンドラはベクタ直結のまま維持し、EXIT 専用の薄い asm 入口 (`shizuku_armv8m_exit_entry` / `shizuku_armv8m_forward_child_exit_entry`) を追加して復帰直前に r4-r11 をゼロクリアする方式を採択・実装。

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
  - `arch/armv8m/armv8m_ctx.S`: EXIT 専用薄 asm 入口 (`shizuku_armv8m_exit_entry` / `shizuku_armv8m_forward_child_exit_entry`) が C++ ハンドラのエピローグ後に `r4-r11` を確実に 0 クリアして例外復帰する。
  - Callee が return stub を迂回して直接 `EXIT_METHOD` (2) を打った場合でも、カーネル側で漏洩が完全に遮断される。
- **重複消去の確認**:
  - `shizuku_armv8m_return_stub` は FP レジスタ (`s0-s15, fpscr`) のみ消去しており、`r4-r11` は消去していないため、重複による不要コードは存在しない。

---

## 4. ベクタ表の EXIT 経路・番号分岐の構造と性能影響

### (1) 現状のベクタ表と分岐の仕組み
- ARMv8-M (Cortex-M33) はハードウェア的に単一の SVCall 例外ベクタ (ベクタ 11, VTOR オフセット `0x2C`) を持つ。
- `armv8m_svc.hpp:287-290` において、Thumb エイリアスにより `isr_svcall` および `SVC_Handler` が C++ 関数 `shizuku_armv8m_svc_handler` に直結されている。
- SVC 番号は `svc 0` のため即値には乗らず、PSP 例外フレーム上の `r0` に格納されている。
- `shizuku_armv8m_svc_handler` は PSP から `r0` をロードし、C++ の `switch` 文によるジャンプテーブル (TBB/TBH) で各 API へ tail call する。

### (2) ベクタ入口で分岐判定する案 (全 SVC asm ラッパ) とその弊害
- もし SVC 番号の判定を asm ベクタ入口に配置した場合:
  1. ベクタ突入直後に PSP を読み、スタック上の `r0` を取り出す命令列が必要。
  2. `EXIT_METHOD` かどうかの比較 (`cmp`) と条件分岐 (`beq`) が全 SVC の入口で実行される。
  3. 最重要のレイテンシ臨界経路である `CALL_METHOD` や `CALL_DESCRIPTOR` を含むすべての SVC で、6〜8 サイクル程度の無用な遅延とパイプライン乱れが発生する。

### (3) 採用した方式 (命令数を増やさない EXIT 専用 asm 入口)
- C++ ハンドラ `shizuku_armv8m_svc_handler` はベクタ直結のまま完全に維持する。
- `switch` の分岐先として、`CALL_METHOD` 等は元の C++ 直結 tail call のまま 1 命令も増やさない。
- `EXIT_METHOD` および `FORWARD_CHILD_EXIT` のみ、薄い asm 入口 `shizuku_armv8m_exit_entry` / `shizuku_armv8m_forward_child_exit_entry` を呼ぶ。
- asm 入口の処理:
  ```assembly
  push {r0, lr}                      // exc_return と lr を退避 (MSP 8 バイト境界維持)
  bl   shizuku_armv8m_exit_method_body // 通常の C++ ハンドラ実行
  pop  {r12, lr}                     // r12 = exc_return
  movs r4, #0
  movs r5, #0
  movs r6, #0
  movs r7, #0
  mov  r8, r4
  mov  r9, r4
  mov  r10, r4
  mov  r11, r4
  bx   r12                           // 例外復帰
  ```
- **オーバーヘッド**:
  - `CALL_METHOD`, `CALL_DESCRIPTOR`, `GET_CURRENT`, `SET_BUDGET` 等: **0 命令 (影響ゼロ)**。
  - `EXIT_METHOD`: 退避・呼出し・復元・ゼロ化で計 12 命令 (約 12 サイクル)。

---

## 5. 戻り値拡張案 (案 2: 3〜4 語への拡張設計)

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

## 6. レジスタ消去の共通契約 (RV32 と ARMv8-M の対応表)

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
  - ARMv8-M: ハードウェアアンスタックによる `r2, r3, r12, lr` の消去 (`zero_exception_frame`) ＋ 薄 asm 入口 (`shizuku_armv8m_exit_entry`) による `r4-r11` の消去。

---

## 7. 整数版直接 EXIT 迂回漏洩試験 (`register_leak_test.cpp`)

- Callee 側 (`command == 89`):
  `r4-r11` を `0xdeadbeef` で汚し、return stub を通らず直接 `syscall(EXIT_METHOD, 0, 42, 0)` を発行。
- Caller 側 (`cmd == 8`):
  復帰直後の物理 `r4-r11` を `data[1..8]` に記録。
- アサーション (`leak direct EXIT bypass int clean`):
  - `error == 0`
  - `value == 42`
  - `r4-r11` の全 8 語が厳密に `0` であること (`direct_int_clean == true`) を検証。
