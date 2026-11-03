# EXIT の FP 戻り値印 (RETURN_STUB_MARKER_FP)

状態: 実装済み (host 試験のみ。bazel / rbuild / 実機は未検証)。対象: RP2350 (ARMv8-M)。
きっかけ: 実機 selftest 314/1 `FAIL leak direct EXIT bypass FP clean: got 16 want 0`
(got のビット4 = s0-s15 または FPSCR が非 0)。

## 1. 事実調査: marker の意味

確定事実 (ソースで確認):

- **marker は「return stub から来た」という識別**。ARM の通常 stub は r12 (= a4) に
  `RETURN_STUB_MARKER` (0xA55A) を載せて `svc` (r0 = 2) を打つ
  (`arch/armv8m/armv8m_ctx.S` の `.Lreturn_exit`)。r1 = 1 は legacy の「1 段だけ戻る」通知。
- **marker が付いたとき**: カーネルは a1 を無視し、戻り先の絶対深さを台帳の unwind floor
  (= 呼び先を積む前の深さ) に置き換える。
  - ARM の本番経路 (trap 内): `native::exit_in_place` (`internal_headers/shizuku/native_call.hpp`
    の `d = floor`)。
  - 汎用経路: `KERNEL::svc_exit` (`source/kernel/dispatch.cpp` `from_stub`)、primitive RETURN
    (`resolve_return_stub` で depth-1 の 1 段)、`svc_dispatch` が handler へ回すときの
    `args[1] = unwind_floor`。
- **marker が付かないとき**: a1 に呼び手が明示した絶対深さ d が必要。d は同じ
  `validate_return_depth` で検査される: `d < floor || d >= depth` なら DEPTH_MISMATCH、
  `count != depth - d` なら BAD_COUNT。弾かれたら 1 段も落とさず
  `UNWIND_REJECTED + kernel_error` を返す。
  つまり marker 無しでも、**自分の unwind floor より下 (親の側) へは戻れず**、
  現在の深さ以上にも戻れない。marker は検証を迂回する権限ではなく、「d を台帳から補え」という
  便宜でしかない。marker を偽っても戻り先は常に floor (自分の呼び手) で、安全性は変わらない。
- 例外: `d == 0 && call_count == 0` (スレッドの最外メソッドの終わり) は kobj の exit_thread へ
  FORWARD。これは marker と無関係。

セキュリティ上の問題の所在 (確定): RETURN 側の再開フレームを作る `call_resume_at`
(`arch/armv8m/armv8m.hpp`) は、拡張フレーム (FPCA=1) のとき FP 部 18 語の先頭 2 語
(s0, s1) を**無条件に**旧フレームからコピーしていた。float/double 戻り値は s0 / s0:s1 に
入るのでコピーが要るが、整数/void 戻りの callee や return stub を迂回して直接 EXIT した
callee の汚れた s0/s1 まで caller に見えていた。s2-s15 と FPSCR は従来から 0 クリア済み
(見立て。語単位の実機確認は command 88 の細分化で取る → 未検証)。

## 2. 方針と選んだ方式

方針 (上層): カーネルが s0/s1 をコピーするのは、EXIT が FP 戻り値を明示したときだけ。
それ以外は FP 部 18 語すべてゼロ。印を偽って渡せるのは callee 自身の s0/s1 だけで許容
(入口で caller の値が callee に見えるのと対称)。

**採用: (A) float/double stub 専用の別 marker 値 `RETURN_STUB_MARKER_FP` = 0xA55B。**

理由:
- 識別が r12 の 1 フィールドで完結する。(B) の a1/a3 に印を入れる案は、a1 が marker 時にカーネルで
  上書きされる (dispatch.cpp `args[1] = unwind_floor`、`d = floor`) ため読む順序に依存し、
  読み忘れで静かに壊れる。
- marker を比較する 4 箇所 (dispatch.cpp primitive RETURN / svc_exit / svc_dispatch、
  native_call.hpp exit_in_place) を `is_return_stub_marker()` に統一し、FP かどうかは
  `is_fp_result_marker()` の 1 箇所で決める。片方の値だけを比較して stub と認めない取り違えが起きない。
- 通常 stub (整数/void) は変更なし (0xA55A のまま)。r12 は ABI 上 EXIT で a4 として使われ、
  RISC-V (`rv32_trap.S:425`) と ARMv6-M (`armv6m_ctx.S:203`) の stub は変更しない
  (FP 部が無い)。

## 3. 実装の要点

- `kernel_abi.hpp`: `RETURN_STUB_MARKER_FP`、`is_return_stub_marker`、`is_fp_result_marker`。
- `armv8m_ctx.S`: `.Lreturn_exit` は r1 に下位バイト 0x5A を置いて `.Lreturn_exit_marker` へ。
  float/double stub は `.Lreturn_fp_exit` で r1 = 0x5B にして `.Lreturn_exit_marker` へ合流
  (r3 = 0xA500 + r1 を r12 へ)。一度だけ組み立てるので stub の語数はほぼ増えない。
- `native_call.hpp`: `exit_in_place` が印を読んで `fp_result` を決め
  (再開フレームを作る前に読む)、`unwind_in_place` → `pop_in_place` → `A::call_resume_at(…, scrub, fp_result, core)`。
  末尾に `bool fp_result = false` の既定引数を足しただけで、既存呼び出しは変えない。
  この形を持たない ARCH (RV32、ARMv6-M) は `requires` で従来の呼び出しに落ちる
  (`A` に依存した requires なので hard error にならない)。
- `dispatch.cpp` / `kernel.hpp`: `call_frame_pop` / `call_unwind` に同じ既定引数。primitive RETURN と
  `svc_exit` の汎用経路から渡す。
  - 注意 (確定): `svc_dispatch` が handler (kobj) へ回す経路では、a4 に `unwind_floor` を入れるため
    FP 印は kobj へ渡らない。この経路で FP 戻り値が必要になる設計が出たら、別途運ぶこと。
    現状 ARM の float/double stub は trap 内の `exit_in_place` で完結する
    (FORWARD になるのはスレッドの終わりだけ)。
- `armv8m.hpp`: private `call_resume_at` に `bool fp_result` を追加。s0/s1 を
  `s[i] & (0u - fp_result)` でマスクして d[0], d[1] へ書く (分岐なし。d と s は重なりうるので読んでから書く)。
  `exit_method` の関数サイズへの影響は bool 1 引数と AND 2 回だけ。ELF での確認は未実施 (上層)。
  `caller_fpca` 引数、`clear_upper_fp_registers` (naked out-of-line)、always_inline、オーバーロード構成は維持。
  公開 `(…, scrub, core_num)` 版は `fp_result = false` で転送 (既存呼び出しは安全側に倒れる)。

## 4. 試験

- host `armv8m_native_geometry_test`: `fp_result` の true/false × 基本/拡張フレーム × 重なり有無で
  FP 部 18 語を検証。印ヘルパの static_assert (0xA55A / 0xA55B / 近傍値)。
- host `arm_inplace_fixture.hpp::run_inplace_exit_fp_marker` (armv8m / armv6m、LEAN 台帳):
  EXIT を 0xA55A / 0xA55B / 印なし / 未知の印 (0xA55C) で打ち、mock の `call_resume_at` に
  渡された `fp_result` を検証 (ARMv6-M は FP 部が無いので渡らないことを検証)。
- 実機 selftest `register_leak_test.cpp` command 88: got のビットを語単位に細分化
  (bit4 = s0/s1、bit5 = s16-s31、bit6 = s2-s15、bit7 = FPSCR)。期待は全て 0。
  float/double 戻り値の試験 (`FP float return` / `FP double return`) は変更なし。
  → 実機は未検証。
- 任意項目 (command 89: FP 印ありの直接 EXIT で自分の s0 を渡すと caller に見える) は
  **足さなかった**。caller worker 側のコマンド配線 (`run_fresh` → command 88 の配線、データ領域の割り当て)
  を増やす必要があり、実機で検証できないまま asm を増やすのは割に合わないため。
  仕様の裏取りは host 試験 (`fp_result=true` で s0/s1 が保たれる) で行った。
