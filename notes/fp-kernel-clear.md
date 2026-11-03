# FP レジスタ カーネル消去および移動オーバーヘッド削減設計書

## 1. 72B 移動 (memmove 388 cyc) の理由と削減設計 (課題 A)

### なぜ 72B (18 語) を callee フレームから新フレームへ移すのか
`call_resume_at` (`arch/armv8m/armv8m.hpp`) は、例外復帰先のスレッド SP 直下 (`sp - bytes`) に新しい例外復帰フレームを構築する。ARMv8-M では、ハードウェアが例外復帰 (Exception Return) 時に PSP からレジスタを自動的に unstack するが、FP 拡張フレーム (104 バイト: 基本 32B + FP 部 72B [s0-s15, FPSCR, 予約 1 語]) の場合、遅延スタッキング (Lazy Stacking) を `FloatingPointState::materialize()` (`vmrs`) で強制実体化させた後、ハードウェアが unstack 参照する位置は新スタックポインタ直下の FP 部となる。
ここで、CALL 側と RETURN 側で意図が根本的に異なる:
- **CALL 側 (call_descriptor 経由等)**: 呼び出し元 (caller) の FP レジスタ値を呼び出し先 (callee) に見せる契約 (試験 `leak A callee entry` で `d[16+i] == 0x3f800100 + i` が要求される) であるため、18 語全ての完全なコピーが必要となる。
- **RETURN 側 (exit_method, forward_child_exit 等)**: 「戻りでカーネルが返り値以外を消す」という基本決定に従い、新フレームの FP 部は **s0, s1 (FP 戻り値 float/double の格納スロット) の 2 語のみを旧フレームからコピーし、残り 16 語 (s2-s15, FPSCR, 予約 1 語) にはゼロを直接書き込む**。

この分離により、RETURN 側のホットパスでのコピー量は 72B (18 語) から 8B (2 語) へと激減し、388 サイクルを消費していた `memmove` の呼出しが完全に消滅する。さらに、return stub を迂回して `EXIT_METHOD` を直接打つ不正な callee に対しても、カーネルがハードウェア例外フレーム上で s2-s15 および FPSCR を確実にゼロクリアするため、漏洩防止がカーネルレベルで保証される。

### 配置崩れ防止と ELF 確認コマンド
段階 2a では `copy_fp_frame_72b` の強制インライン化により、`call_resume_at` が GCC によって非インライン化され Flash XIP 遠距離呼出し (+600 cyc) を引き起こした。これを防ぐため:
1. `call_resume_at` (public/private 両方) に必ず `[[gnu::always_inline]] static inline` を付与する。
2. テンプレート引数 `bool IsReturn = true` を導入し、RETURN 側では 18 語コピー関数を展開せず、コンパイル時に 2 語コピー + 16 語ゼロ直接ストアのみに絞る。

上層がビルド結果の ELF (`shizuku.elf`) で配置崩れが発生していないか確認するためのコマンド:
```bash
# 1. call_resume_at が単独シンボルとしてアウトライン化されていないこと (出力が 0 行であること)
arm-none-eabi-nm bazel-bin/firmware/shizuku.elf | grep -i "call_resume_at"

# 2. 各 SVC ハンドラの関数サイズが急激に肥大化・縮小 (剥離) していないこと
# 基準値 (2a revert 後): exit_method ~1480B, forward_child_exit ~1492B, call_method ~1576B
arm-none-eabi-nm -S --size-sort bazel-bin/firmware/shizuku.elf | grep -E "exit_method|forward_child_exit|call_method|call_descriptor"

# 3. exit_method や forward_child_exit 内に memmove への bl 呼出しが存在しないこと
arm-none-eabi-objdump -d bazel-bin/firmware/shizuku.elf | grep -A 50 "<_ZN7shizuku17armv8m_svc_detailL11exit_method" | grep "bl.*memmove"
```

---

## 2. 漏洩修正と caller FPCA 制御の分析 (課題 B)

### B1. caller の FPCA 状態の保持 (O(1))
- **保存場所**: `thread.ledger[depth]` (`compact_call_ledger_entry` / `standard_call_ledger_entry`)
- **更新箇所**: `internal_headers/shizuku/native_call.hpp` の `enter_in_place` (line 349 付近)
- **構造体設計**:
  `compact_call_ledger_entry` において `ret_privileged : 2` であったビットフィールドを、`ret_privileged : 1` および `ret_fpca : 1` に分割。全体の 32 ビット幅を完全に維持し、メモリ増加ゼロで caller の `CONTROL.FPCA` を保持する。
- **参照箇所**: `native_call.hpp` の `pop_in_place` (line 218 付近) で `entry.ret_fpca` を O(1) で参照し、復帰処理へ渡す。

### B2. caller FPCA=0 時の s16-s31 カーネル消去
`exit_method` / `forward_child_exit` から呼ばれる RETURN 側復帰において、callee が FP を使用しており (拡張フレーム `exc_return & 0x10 == 0`)、かつ caller が CALL 時に FPCA=0 (`entry.ret_fpca == 0`) であった場合に限り、`FloatingPointState::materialize()` の直後に `vmov s16..s31, ゼロ` (16 命令) を実行して消去する。通常経路 (`caller_fpca == 1`) ではこの消去は実行されず、オーバーヘッドはゼロである。

### B3. EXC_RETURN を変えずに caller の FPCA を 0 へ戻すことの可否と結論
**結論**: **EXC_RETURN を変更せずに、非特権 caller の CONTROL.FPCA を 0 へ戻すことは、ARMv8-M ハードウェア仕様上不可能である。**

#### (i) 不可能である理由
1. **例外復帰時のハードウェアによる強制上書き**:
   ARMv8-M Architecture Reference Manual (DDI 0553) の規定により、例外復帰処理 (Exception Return) 実行時、ハードウェアは `EXC_RETURN` の bit[4] を評価する。
   - `EXC_RETURN[4] == 0` (拡張フレーム復帰) の場合、ハードウェアはスタックから s0-s15/FPSCR を unstack し、例外復帰の完了時に **`CONTROL.FPCA` を強制的に 1 にセット**する。
   - ハンドラモード内で特権命令 `MSR CONTROL, ...` を実行して事前に `CONTROL.FPCA = 0` に書き換えていたとしても、例外復帰命令 (`bx lr` / `pop {..., pc}`) が実行された瞬間に、ハードウェアが `EXC_RETURN[4] == 0` に基づいて `CONTROL.FPCA = 1` で上書きする。
2. **非特権 (U-mode) からの変更不可**:
   caller が動作する非特権 Thread mode において、`CONTROL` レジスタへの書き込み (`MSR CONTROL, Rn`) はアーキテクチャ上無視される (RAZ/WI)。したがって caller 自身が後から FPCA をクリアすることはできない。
3. **C++ 例外ハンドラ境界**:
   現在の SVC ハンドラ `shizuku_armv8m_svc_handler` は通常の C++ 例外関数であり、復帰時の `EXC_RETURN` は関数エントリ時の `LR` (`__builtin_return_address(0)`) に拘束されている。callee が FPCA=1 で SVC を打った場合、入口の `LR` は拡張フレーム復帰 (`0xFFFFFFED` 等、bit 4 == 0) となるため、C++ 関数の通常 return では必ず `CONTROL.FPCA = 1` で復帰する。

#### (ii) 上層への選択肢
- **選択肢 a (FPCA 昇格の許容とデータ機密性保証)**:
  callee が FP を使った場合、caller の `CONTROL.FPCA` が 1 に昇格することをアーキテクチャの正規動作として許容する。
  機密性に関しては、課題 A (s0-s15/FPSCR のカーネルゼロクリア) および課題 B2 (caller FPCA=0 時の s16-s31 カーネルゼロクリア) により、callee の FP 値は全レジスタで完全に抹消されるため、情報漏洩は完全に遮断される。ただし、selftest の `(c[12] & 4u) == 0` という「CONTROL レジスタのビット自体の不変性」を要求する assert を改訂する必要がある。
- **選択肢 b (EXC_RETURN を切り替える専用の薄い asm トランポリン導入)**:
  caller が FPCA=0 の場合、復帰直前の `EXC_RETURN` の bit 4 を 1 (`0xFFFFFFFD` 等) に書き換えて例外復帰させる薄いアセンブリ復帰機構を設ける。これによりハードウェアが basic frame として unstack し、`CONTROL.FPCA` は 0 にクリアされる。ただし「ARM SVC ハンドラは普通の C++ 関数のまま」という既存設計規律に手を加える必要がある。
*(規律に従い、EXC_RETURN の変更および selftest assert の勝手な変更は行わず、上層の判断を仰ぐためここで停止する)*

### B5. 整数レジスタ (r4-r11, lr) 消去の所在
- **所在**: `internal_headers/shizuku/armv8m_abi.hpp:131-197` (`syscall` インラインアセンブリ / caller converter)
- **実態**:
  ARM では D1 caller clobber 設計を採用しており、caller の `syscall` converter 内 (line 149-157) で `r4-r11, lr` をゼロクリアしてから SVC を発行している。復帰時、カーネル (`zero_exception_frame`) は `r0-r3, r12, lr` のみ消去し、`r4-r11` は消去しない。caller 側のコンパイラが clobber リストに基づき自身のスタックから `r4-r11` を復元する規約である。
- **迂回可能性**:
  callee が return stub を通らず直接 EXIT した場合、callee の `r4-r11` の値はカーネルでは消去されずそのまま Thread mode に残るが、caller 側のコンパイラが復帰直後に自身の退避領域から `r4-r11` を上書き復元するため、U-mode アプリケーションレベルでの漏洩は防がれている。

---

## 3. 診断版 monotonic FAIL の特定 (課題 D)

- **一言**: 親ワーカーへの `CALL_METHOD` 1 回と子ワーカーの計測 `ROUNDS` (2048) 回により、計測区間内で `commit_round()` が合計 2049 回 (`ROUNDS + 1`) 実行されるため、判定式 `sample_count == ROUNDS` が必ず不一致となっていた。
- **根拠**:
  1. `rp2350_u_cost.cpp` では `shizuku::diag_fp::reset()` から `shizuku::diag_fp::stop()` の間で親ワーカー (`cmd_base + 3` / `cmd_base + 6`) を呼び出している。
  2. 親ワーカーは子ワーカーを 2048 回呼び出し (`EXIT_METHOD` で 2048 回 commit)、さらに親ワーカー自身が終了する際の `EXIT_METHOD` で 1 回 commit されるため、合計コミット回数は **2049 回** (`N=2049`) となる。
  3. 実機ログ (`cu.usbmodem1101.log`) にも `method fpca=1 ... (mean, N=2049)` および `desc fpca=1 ... (mean, N=2049)` と明記されており、計測自体と単調増加性 (`order_valid`) は完全に正常である。
  4. 判定式 (`rp2350_u_cost.cpp:1176-1177`) が `sample_count == ROUNDS` (2048 との完全一致) を要求していたため、2049 != 2048 により FAIL していた。
  5. 判定式を `sample_count >= ROUNDS` に修正することで解消する。
