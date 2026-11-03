# Stack 窓の設計 (thread 保持・積み上げ・使った分だけゼロ化)

状態: 設計 (2026-11-03)。ユーザー決定 5 件に従う。実装は段階ごとにコミット。
対象: C6 (U-mode, `SHIZUKU_C6_UMODE`)。共通部は thread 側、chip は PMP 形式への適用だけ。

## 0. 現状 (調べた事実)

- C6 の U stack は **object ごとに 1 領域**: `c6_stage1_object_bounds.stack_base/stack_limit` (`chips/esp32_c6/pmp_c6.hpp:77`、上端が `stack_limit`)。登録は `g_c6_stage1_u_bounds` と `g_c6_stage1_driver_bounds` の 2 つ。
- 入場のたびに callee SP を固定 top に戻す: `chips/esp32_c6/trap.cpp:1291` (`enter_u_object`)、`:559` (`context_to_frame` の SP 検査)。A→A / A→B→A で使用中 stack を上書きする。
- PMP: entry 0-3 固定 (code, 共有窓)、4-5 stack (TOR, RW)、6-7 data、8-9 mmio、10-15 未使用。粒度 4B (`pmp_c6.hpp:11`)。stack 窓は `precomputed.pmpaddr4/5` (`pmp_c6.hpp:23-24`) で object ごとに前計算。
- 台帳 (`thread.ledger[depth]`) はカーネル領域。1 段ごとに `ret_pc/ret_sp/prev_call/...` を持つ (`native_call.hpp:280-293`)。user stack にヘッダは書かない (`SHIZUKU_LEAN_LEDGER`)。
- U からの trap の frame はカーネル stack 上 (`arch/riscv/rv32_trap.S:40-50`)。U stack には触れない。
- 通常 return は `call_resume_at` (`arch/riscv/rv32.hpp:380-397`) が **caller の `ret_sp - sizeof(frame)` = user stack 上** に resume frame を作る。
- U 側の fault: `shizuku_c6_handle_fault` (`trap.cpp:894`) は selftest probe の PC 一致だけを扱い、それ以外は fatal。窓を伸ばす経路は無い。

## 1. データの持ち場所

thread (共通) が持つ:

| 項目 | 置き場 | 意味 |
|---|---|---|
| `stack_lo`, `stack_hi` | thread (context_t の `stack` 底/上限) | thread の U stack 全体 [lo, hi)。ここより下は本物の溢れ |
| `win_lo` | 台帳の「いまの段」= `thread.ledger[depth-1].win_lo`。最内段の値は thread にも写す (`thread.cur_win_lo`) | 現在の callee 窓の下限 (開いた分) |
| `win_hi` | 台帳の「いまの段」`.win_hi` (= 入場時の caller_sp、16B 切下げ) | 現在の callee 窓の上限。caller の積んだ枠は `>= win_hi` で窓の外 |
| `K` | 定数 `kStackWindowStep` (初期 256B。`internal_headers/shizuku/stack_window.hpp`) | 初期窓幅 = 伸ばす幅の最小単位 |

台帳 entry に `win_lo`, `win_hi` を足す (kernel 領域なので callee が書き換えられない)。ledger entry のサイズ制約が許さない場合は 2 本の `uint32_t` で別配列 (`thread.win[depth]`) にする。**object には stack を持たせない**。`c6_stage1_object_bounds.stack_base/limit` は廃止し、PMP 前計算から stack を外す (data/mmio の 4 本は残す)。

## 2. 各イベントでの窓・cursor の更新

窓の定義: callee が走っている間、U が触れる stack は `[win_lo, win_hi)` だけ。`win_hi` = 入場時の caller SP (16B 切下げ)、`win_lo` 初期値 = `max(win_hi - K, stack_lo)`。

- **call (CALL_METHOD / descriptor / U→U)**: `enter_in_place` で台帳段を積むとき `win_hi = ret_sp & ~15`, `win_lo = max(win_hi - K, stack_lo)` を記帳。`frame.reserved0 = win_hi` (callee SP)。固定 top に戻す処理 (`trap.cpp:1291`, `:559`) を消す。`win_hi - stack_lo < K_min` (余白不足) は `NO_STACK`。再入 (A→A, A→B→A) は同じ thread stack の下へ積むだけなので禁止しない。
- **return (通常)**: pop 前に「実際に開いた窓 `[win_lo, win_hi)`」だけを CPU でゼロ化 (`win_lo` から `win_hi` まで)。窓全体 ≠ stack 全体。その後 caller の窓 (一つ前の段の `win_lo/win_hi`) に戻す。DMA は使わない。
- **fault (access fault, mcause 5/7, U)**: `mtval` = a とする。`stack_lo <= a < win_lo` なら窓を伸ばす: `new_lo = max(stack_lo, min(win_lo - K, a & ~3))`、PMP 窓を張り直して **同じ命令を再実行** (mepc 不変)。O(1) (台帳の `win_lo` と PMP entry 2 本の書換えだけ)。`a < stack_lo` は本物の溢れで従来の fault 処理。`a >= win_hi` (caller 枠) は常に fault。
- **強制巻戻し (count>1 / forced_scrub)**: 各段を落とすたびに、その段の `[win_lo, win_hi)` だけをゼロ化する (全 stack の一括消去はしない)。段数に比例するが、各段は開いた分だけ。
- **thread 切替え**: PMP の stack 窓は「current thread の `[cur_win_lo, cur_win_hi)`」。切替え先で `cur_win_*` を読んで張る。切替えで窓は閉じない/ゼロ化しない (thread は stack 領域を共有しない)。

## 3. PMP エントリ割当て

- entry 4/5 を stack 窓 (TOR, RW, NX) のまま使う。`pmpaddr4 = win_lo >> 2`, `pmpaddr5 = win_hi >> 2`。粒度 4B なので `win_lo/win_hi` は 4B 整列で足りる。
- 現状は object 固有の 8 本 (4..9) を `switch_to_precomputed` で一括書換え。stack 窓は thread/段の状態で変わるので、前計算 (`precomputed`) から stack 2 本を外し、`switch_stack_window(win_lo, win_hi)` (CSR 2 本) を別に持つ。object 切替えと stack 窓更新は `context_to_frame` の最後の 1 か所で書く (現状の「1 往復 1 回」規律を維持)。shadow に `win_lo/win_hi` を持たせ、同じ値なら書かない。
- 層境界: PMP 形式への変換 (`pmpaddr`/cfg) は `chips/esp32_c6/pmp_c6.hpp`、窓の値 (win_lo/win_hi) の持ち主と更新規則は thread 側 (共通ヘッダ `internal_headers/shizuku/stack_window.hpp` に純関数: `open_window`, `extend_window`, `window_zero_range`)。chip は `apply_stack_window(lo, hi)` だけ。object policy や modules を trap.cpp に入れない。

## 4. ゼロ化範囲の記録

「開いた分」= 台帳段の `[win_lo, win_hi)`。伸ばすたびに `win_lo` が下がるので、これが唯一の記録 (別の watermark は要らない)。return/巻戻しはこの範囲を `win_lo` から昇順に 4B ずつ 0 を書く (M-mode は PMP 非適用のため窓外でも書ける)。回数は使用量に比例し、未使用 stack は触らない。

## 5. 隙間 (決定が触れていない点) と解決案

1. **resume frame が user stack 上にある** (`call_resume_at`, `rv32.hpp:384`)。窓を閉じてゼロ化した直後に caller の `ret_sp - 128` へ frame (mepc・a0/a1・mstatus…) を書くと、次の呼出しの callee 窓 `[caller_sp-K, caller_sp)` の最上位に前回 return の値が残り、callee に見える (試験 (c) に反する)。**解決案**: U caller への通常 return では resume frame を **カーネル stack 上の現 trap frame を再利用** して作り (U からの trap の frame は常に kernel stack 上 = `rv32_trap.S:40-50`)、`reserved0 = ret_sp` で SP を戻す。user stack には何も書かない。実装時に `call_resume_at` の U 分岐と `context.sp` の扱いを確認し、不可能なら「出口 asm が frame 復元後に frame 領域をゼロ化する」を代案とする (32 語ぶんの追加コスト)。
2. **窓の外の free stack は常にゼロ**という不変条件が必要 (窓を伸ばしたとき callee に未初期化値が見えないように)。thread stack を作るとき全体をゼロにし、以後は「開いた窓は閉じるときゼロ化」で保たれる。ゼロ化漏れ経路 (fault での thread 殺害、dispatch の途中失敗) は実装時に列挙して試験する。
3. **窓の外から callee が持つ 2 種の stack**: 特権 (M) の stack は従来どおり kernel 管理で、この窓の対象外。M→M は変更なし。

## 6. 試験 (要求 (a)-(e))

- (a) 再入: A→A, A→B→A で各段の stack の既知値が壊れない (selftest U→U 実経路 + host)。
- (b) 窓が下へ伸びて正常に戻る。`stack_lo` を超えると fault。
- (c) callee が既知パターンを窓に書いて戻った後、caller が同アドレスを読むと 0 (残り値なし)。caller 側の読取りは「次の呼出しの callee 内」または M 側 probe で確認。
- (d) 強制巻戻し後も (c) と同じ。
- (e) callee から `>= win_hi` (caller 枠) の読み書きは fault。
- 既存試験は弱めない。[UCOST] 計測行はそのまま。

## 7. ARM への将来の写像

ARMv8-M は MPU の region (base/limit) 1 組を stack 窓に当てられ、`win_lo/win_hi` を `RBAR/RLAR` に書く形で同じ。MemManage fault (MMFAR に fault address) で窓を伸ばして再実行、return で開いた分だけゼロ化、台帳に窓を持つ点は共通。Cortex-M の MPU region 数が少ない構成では stack 窓に 1 region を専有する。共通の純関数 (`stack_window.hpp`) は arch 非依存に保ち、`apply_stack_window` だけ arch/chip が持つ。
