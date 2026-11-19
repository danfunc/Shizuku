# 呼出しの記録を caller stack 枠へ一本化する設計 (固定台帳・窓別表の廃止)

状態: 本設計 (2026-11-04)。段 3。
更新 (2026-11-17): この方式 (STACK 方式) は C6 U-mode だけでなく in-place の ARCH (RP2350 / C6 U・M-mode / C5) 全部の唯一の方式になった。
旧 LEAN 方式 (特権の台帳に戻りの権威を置く) と旧 call-header 方式は廃止し、`SHIZUKU_LEAN_LEDGER` / `SHIZUKU_STACK_LEDGER` の define も無い。
方式は context_t の `CALL_IN_PLACE` (`thread.hpp` の `call_in_place_of`) で決まる。in-place でない ARCH (RP2040) だけが従来の特権配列を使う。
以下の本文は設計当時の記述で、行番号・名前は当時のもの。
対象: C6 (U-mode, `SHIZUKU_C6_UMODE`, RISC-V)。共通部の権威を stack 記録へ集約し、固定台帳・窓別表を全廃する。

---

## 1. 目的と背景

従来の C6 U-mode では、戻り先 PC や特権、窓情報などの呼出し記録を特権メモリ上の固定長配列 (`thread.ledger[34]`) および別表 (`thread.win_span[34]`) に保持していた (旧 LEAN 方式。define と `win_span` は廃止済み)。
しかしこの設計には以下の課題があった：
1. **固定資源の消費**: スレッドごとに 1KB 以上の固定台帳メモリと 68 バイトの窓別表を消費し、組み込み DRAM を圧迫していた。
2. **段数の固定上限**: `MAX_STACK_WINDOWS = 34` や `ledger_capacity` により、スタックに余裕があっても 34 段を超える呼出しができなかった。
3. **二重持ちの歪み**: caller 側 converter は ecall 前に stack 上に枠 (`RV32_CALL_HEADER_BYTES = 32`) を確保しているにもかかわらず、カーネルはそこに書かず特権台帳に書いており、資源が二重に予約されていた。

本設計 (段 3) では、呼出しの記録を caller が stack 上に確保した 32 バイトの枠 (`call_header_t`) へ完全に一本化する。
カーネル側が保持するのは「先頭記録へのポインタ (`thread.call_stack.top`) と深さ (`depth`, `call_count`)」のみとし、固定長配列 `thread.ledger` および `thread.win_span` を C6 から全廃する。

---

## 2. 安全性の根拠 (なぜ stack 上の記録が信頼できるか)

### (1) callee 窓の外側にあることの保証
- caller の converter は ecall の直前に `sp -= ECALL_RESERVE_BYTES` (48 バイト) を実行し、先頭 32 バイトに記録用の枠 `[sp, sp + 32)` を確保する。
- カーネルはこの枠に `call_header_t` (32 バイト) を書き込む。
- 呼び先 (callee) の PMP スタック窓は `[win_lo, win_hi)` であり、`win_hi = caller_sp & ~15` (枠の下端) に設定される。
- したがって、記録枠 `[caller_sp, caller_sp + 32)` は **callee の PMP スタック窓の厳格に外側 (アドレス上位)** に位置する。
- callee が実行中に記録枠を読み書きしようとすると、PMP ハードウェアにより即座に Load/Store Access Fault が発生し遮断される。

### (2) caller 停止中の不変性
- callee が実行されている間、caller は停止しており、自らのスタックに触れることはできない。
- caller が再開されるのは callee が return または fault で終了し、カーネルが記録を読み取って caller の再開フレームを復元した後である。

### (3) スレッド独立性と他コア干渉の不在
- スタック領域 `[stack_lo, stack_hi)` はスレッドごとに完全に分離されている。
- PMP 窓は現在アクティブなスレッドの窓のみが CPU に設定されるため、他スレッドが本スレッドのスタックを触ることはできない。
- ESP32-C6 はシングルコア RISC-V であり、他コアからの同時アクセスは存在しない。

### (4) M↔U 境界の防護
- M caller が U callee を呼ぶ場合、caller の SP はカーネルスタック上にある。
- `enter_in_place` において `ret_sp < thread.stk.stack_lo || ret_sp > thread.stk.stack_hi` を検査し、U stack 範囲外であれば `NO_STACK` で拒否する。これによりカーネルスタックが U callee に公開されることはない。
- M caller から M callee への呼出しは両者が特権モード (信頼側) であり問題ない。

---

## 3. 記録の改ざん・破損に対する O(1) 検証

スタックの溢れ、メモリ破壊、不正なポインタ書き換えなどにより、巻き戻し時に不正なヘッダアドレスを参照してカーネルがクラッシュすることを防ぐため、`thread.call_stack.top` および `header->prev` を参照する直前に以下の **O(1) 検証** を必ず実行する。

```cpp
bool is_valid_call_header_ptr(const thread &t, uintptr_t ptr, uintptr_t current_top) {
  // 0 は最外段 (base) を表す正当な終端
  if (ptr == 0) return true;
  // 1. 16B アライメント検査 (32B ヘッダは 16B 境界)
  if ((ptr & 0xFu) != 0u) return false;
  // 2. スタック範囲検査 (thread の U stack 全域 [stack_lo, stack_hi) 内)
  if (ptr < t.stk.stack_lo || ptr + sizeof(call_header_t) > t.stk.stack_hi) return false;
  // 3. 単調増加検査 (下向きスタックのため、呼び元のフレームは厳格に上位アドレス)
  if (current_top != 0 && ptr <= current_top) return false;
  return true;
}
```

この検証が偽を返した場合、カーネルは台帳破損 (CORRUPT / fail-closed) として即座に停止し、不正アドレスの参照を防止する。

---

## 4. 消去される固定資源の一覧と削減バイト数 (sizeof 根拠)

C6 U-mode において全廃・削減される固定資源の一覧：

| 資源名 | 定義箇所 | 従来のサイズ計算 | 削減バイト数 (sizeof 根拠) |
|---|---|---|---|
| `thread.ledger` 配列 | `internal_headers/shizuku/templates/thread.hpp:320`<br>`source/kernel/thread.cpp:97` | `call_ledger_entry[34]`<br>1 エントリ = 32 バイト | **1,088 バイト / スレッド**<br>(34 × 32B) |
| `thread.win_span` 配列 | `internal_headers/shizuku/templates/thread.hpp:243` | `uint16_t[34]` | **68 バイト / スレッド**<br>(34 × 2B) |
| `thread.ledger_capacity` | `internal_headers/shizuku/templates/thread.hpp:321` | `uint32_t` (4B) | **4 バイト / スレッド** |
| `early_boot` の静的台帳 | `modules/esp_idf_support/c6/early_boot.cpp:87` | `call_ledger_entry[4]` (4 × 32B) | **128 バイト** (.bss) |
| `s_thread_returns` (参考) | `source/kernel/thread.cpp:29` | `caller_return_record[32][34]` (32 × 34 × 16B) | **17,408 バイト** (in-place でない ARCH = RP2040 だけが使う。`source/kernel/thread.cpp` の `thread_return_storage<false>` に移り、in-place の構成では確保されない) |

**合計削減量 (C6 実行時)**:
- スレッドあたり: **1,160 バイト** (`1088 + 68 + 4`)
- ブート・システム静的領域: **128 バイト**
- スレッド 4 本構成の場合: **4,768 バイト (約 4.7 KB)** の貴重な内部 SRAM/DRAM を節約。

---

## 5. 記録枠 `call_header_t` の構造 (32 バイト)

`arch/riscv/rv32.hpp` に定義される `call_header_t`:

```cpp
struct call_header_t {
  uintptr_t prev;              // [0..3]   一つ外側のヘッダのアドレス (0 = 最外)
  uintptr_t pc;                // [4..7]   caller の再開 PC
  uintptr_t caller_sp;         // [8..11]  caller の再開 SP
  uint32_t caller_object;      // [12..15] caller の object ID
  uint32_t caller_generation;  // [16..19] caller の object 世代
  uint32_t win_lo;             // [20..23] この段が開いた stack 窓下限
  uint32_t win_hi;             // [24..27] この段が開いた stack 窓上限 (0 = 窓なし/M callee)
  uint16_t unwind_floor;       // [28..29] unwind floor (深さ)
  uint8_t via;                 // [30]     call 種別 (LEDGER_CALL 等)
  uint8_t privileged;          // [31]     caller の特権 (1 = M, 0 = U)
};
static_assert(sizeof(call_header_t) == 32);
```

- 合計 32 バイトで、`RV32_CALL_HEADER_BYTES = 32` にジャストフィット。
- パディングなし、全フィールドが自然境界に整列。

---

## 6. 各イベントでの詳細動作

### 6.1 Call (入場: `enter_in_place`)
1. **余白検査**:
   - `ret_sp = A::call_return_sp(context)` (U caller の場合、ecall 前に確保された枠のアドレス)。
   - `ret_sp - sizeof(call_header_t) < thread.stk.stack_lo + kRequired` なら `kernel_error::NO_STACK` で即時拒否。
2. **窓の計算**:
   - U callee の場合: `win = open_window(ret_sp, thread.stk.stack_lo)`。
   - M callee の場合: `win = {0, 0}`。
3. **記録の書き込み**:
   - `header = (call_header_t*)ret_sp`。
   - `header->prev = thread.call_stack.top`。
   - `header->pc = frame.mepc`。
   - `header->caller_sp = ret_sp`。
   - `header->caller_object = current_object`。
   - `header->caller_generation = current_generation`。
   - `header->win_lo = win.lo`。
   - `header->win_hi = win.hi`。
   - `header->unwind_floor = thread.call_stack.depth`。
   - `header->via = via`。
   - `header->privileged = caller_privileged`。
4. **カーネル状態の更新**:
   - `thread.call_stack.top = (uintptr_t)header`。
   - `thread.call_stack.depth++`。
   - `thread.call_stack.call_count++`。
   - U callee なら `apply_stack_window(win.lo, win.hi)` で PMP を適用。

### 6.2 Return (通常 1 段復帰: `pop_in_place`)
1. `header = (call_header_t*)thread.call_stack.top`。
2. `is_valid_call_header_ptr` で `header` を検証。不正なら `CORRUPT`。
3. **ゼロ化**:
   - `header->win_hi != 0` (U callee) なら、`[header->win_lo, header->win_hi)` を 32B 単位展開でゼロクリア。
4. **外側の窓の解決**:
   - `prev = header->prev`。
   - `is_valid_call_header_ptr` で `prev` を検証。
   - `prev != 0` かつ `((call_header_t*)prev)->win_hi != 0` ならその窓、なければ `[stack_lo, stack_hi)`。
   - `cur_win_*` を更新し、U caller なら PMP 適用。
5. **Caller 再開フレームの構築**:
   - `call_resume_at` でカーネル stack 上の trap frame を再開フレームとして再利用。
   - `reserved0 = header->caller_sp`。
6. **スタックの pop**:
   - `thread.call_stack.top = header->prev`。
   - `thread.call_stack.depth--`。
   - `thread.call_stack.call_count--`。

### 6.3 Forced Unwind (強制巻き戻し: `unwind_in_place`)
- `count` 段を順次 pop。
- 各段で `[header->win_lo, header->win_hi)` をゼロ化。
- `top` を `header->prev` で辿り、各ステップで O(1) 検証。
- 途中で `prev` が不正であれば直ちに巻き戻しを中止しエラーを返す。

### 6.4 Fault による窓伸長 (`shizuku_c6_handle_fault`)
- mcause 5/7 (Load/Store Access Fault) 発生時:
  - `header = (call_header_t*)thread.call_stack.top`。
  - `header` の O(1) 検証。
  - `fault_addr >= stack_lo && fault_addr < header->win_lo` ならば:
    - `new_win = extend_window(header->win_lo, header->win_hi, fault_addr, stack_lo)`。
    - `header->win_lo = new_win.lo` (stack 記録の win_lo を更新！)。
    - `thread.stk.cur_win_lo = new_win.lo`。
    - PMP レジスタを更新し、同一命令を再実行 (`mepc` 不変)。
