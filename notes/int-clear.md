# 整数レジスタ (r4-r11) のカーネル消去に関する調査報告

状態: 課題 4 調査完了。ハンドラの形 (ベクタ直結の C++ 関数) を変える方式は実装せず、上層の判断を仰ぐ。

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

## 3. レジスタ消去の担当箇所と迂回可能性

- **Callee への入口**:
  - `armv8m_abi.hpp:149-157`: Caller converter が `svc 0` の直前に `r4-r11, lr` を明示的に 0 クリアしており、Caller の機密は Callee へ漏洩しない。
- **Callee からの出口**:
  - `wrapper<F>` (`armv8m_abi.hpp:62-72`): 通常の C++ 関数呼出し。`r4-r11` の消去は行わない。
  - `fp_entry<F>` (`armv8m_abi.hpp:90-103`): `shizuku_armv8m_return_float_stub` / `double_stub` へジャンプ。
  - `shizuku_armv8m_return_stub` (`arch/armv8m/armv8m_ctx.S:195-233`):
    - FP レジスタ (`s0-s15, fpscr`) は消去するが、**`r4-r11` は消去していない**。
- **迂回可能性**:
  - Callee が return stub や converter を介さず、インラインアセンブリ等で `r4-r11` に機密値を残したまま直接 `svc 0` (EXIT_METHOD, a0=2) を呼出した場合、ハンドラのエピローグによって Callee の値がそのまま Caller へ復元・漏洩する。

---

## 4. 方式検討表

| 方式 | 概要 | コスト (命令数 / サイクル) | 「ベクタ直結 C++ 関数」決定との衝突 |
| :--- | :--- | :--- | :--- |
| **① 全 SVC asm ラッパ** | ベクタを asm ラッパとし、C++ ハンドラ呼出し後、`bx lr` の直前に `r4-r11` をゼロクリア (または EXIT 時のみ分岐) | 全 SVC に 8〜12 命令 (+8〜12 cyc)、または分岐判定数命令 | **衝突あり** (ベクタ直結 C++ 関数でなくなる) |
| **② EXIT 専用 asm 入口** | EXIT_METHOD (2) 専用の薄い asm 入口を設け、復帰直前に `r4-r11` をゼロクリア | EXIT 経路のみ約 10 命令。他 API へのオーバーヘッドなし | **衝突あり** (ベクタ直結 C++ 関数でなくなる) |
| **③ ハンドラ形状維持** | ベクタ直結 C++ 関数のまま内部で消去を試みる | 実現不可 (コンパイラのエピローグ `ldmia` が上書きするため、不正なスタック書き換えハックを行わない限り不可能) | 衝突なし (ただし消去不能) |

**判断**:
ユーザー決定・上層指示に基づき、ハンドラの形状を変更する方式 (①, ②) は下層では実装せず、現状のまま止めて上層へ報告・エスカレーションする。

---

## 5. 整数版直接 EXIT 迂回漏洩試験の設計案

将来カーネル側で消去方式が採択された際に導入するテストの設計案:

```cpp
// callee 側: r4-r11 を 0xdeadbeef で汚して return stub を迂回し EXIT_METHOD (2) を直接打つ
if (command == 89) {
  asm volatile(
      "movw r4, #0xbeef\n movt r4, #0xdead\n"
      "mov r5, r4\n mov r6, r4\n mov r7, r4\n"
      "mov r8, r4\n mov r9, r4\n mov r10, r4\n mov r11, r4\n"
      : : : "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11", "memory");
  (void)shizuku::armv8m_abi::syscall(static_cast<uintptr_t>(shizuku::object_api::EXIT_METHOD), 1, 42, 0);
  return 99;
}

// caller 側: 復帰後の r4-r11 を配列に記録し、callee の値 (0xdeadbeef) が残っていないことを検証
// (caller FPCA=0 始まりなら 0 であること、あるいは少なくとも 0xdeadbeef でないこと)
const auto direct_int_exit = run_fresh(8);
bool direct_int_clean = true;
for (unsigned i = 0; i < 8; ++i) {
  direct_int_clean &= (c[i + 1] != 0xdeadbeefu); // または c[i + 1] == 0
}
record_check("leak direct EXIT bypass int clean",
             direct_int_exit.error == 0 && direct_int_exit.value == (uintptr_t)grant_end::YIELDED &&
             c[9] == 0 && c[10] == 42 && direct_int_clean,
             direct_int_clean ? 0 : 1, 0);
```
