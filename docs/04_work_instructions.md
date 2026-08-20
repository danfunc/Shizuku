# 04. 指示書 — フェーズ別の実装手順

前提: [03_porting_policy.md](03_porting_policy.md) の決定 D1-D17 に従う。
各フェーズは「受け入れ条件」を満たしてから次へ進む。**梯子を飛ばさない**こと
(参照実装が 6 段ネストを一気に試して長時間溶かした反省 = DESIGN §16 / PORT §7)。

実装順序の背骨は 2 本:
- 機構: PORT §6 (arch 抽象 → オブジェクト概念の外出し → 2nd ISA → RISC-V → ボード)
- アクセス制御: DESIGN §17 (identity → 親子 → md → arena)。**順序厳守・飛ばすと無意味**

---

## Phase 0 — リポジトリ衛生 + Bazel bring-up

目的: 「壊れていない土台」を作る。カーネル設計には触らない。

1. 01 §4 の不具合を潰す:
   - ディレクトリ名を小文字 snake_case に統一 (`source/object_system`,
     `source/kernel_object`) し、CMakeLists の参照を一致させる (git mv で)
   - include guard 衝突 2 組を解消。`kernel_object_abi/rp2040_abi.hpp` は削除
     (abis 側と統合)。`dummy.hpp` は 1 本化
   - `modules/pico_sdk_support/main.cpp` の asm オペランド方向バグ修正
     (入力は `"r"`)、svc ハンドラ二重登録の解消 (cpu_driver.cpp 側に一本化)
   - buildUtilities.cmake の残骸 `setting()` 削除
2. **Bazel bring-up (D15)**:
   - `MODULE.bazel` 新設: `bazel_dep` で rules_cc + pico-sdk (2.2.0, local_path_override
     で `~/.pico-sdk/sdk/2.2.0` を指すか registry 版)。
     `register_toolchains("@pico-sdk//bazel/toolchain:mac-aarch64-rp2350")` ほか
   - `//bazel/config` パッケージ: `SHIZUKU_ARCH` / `SHIZUKU_BOARD` / `SHIZUKU_CPU_COUNT`
     等を build setting 化し、`expand_template` で `shizuku/config.hpp` を生成
     (configs/config_template.hpp.in と同内容。CMake と同じ config.hpp が出ることを diff で確認)
   - `cc_binary(name="shizuku")` + UF2 aspect でイメージ生成:
     `bazel build --platforms=@pico-sdk//bazel/platform:rp2350 \
        --@pico-sdk//bazel/config:PICO_BOARD=pico2 //...`
   - CMake は削除せず並存 (同等性確認が終わる Phase 2 末まで)
3. 実機 (pico2) で現状の svc 往復デモが Bazel ビルドでも動くことを確認

**受け入れ条件**: Bazel と CMake の両方で configure/build が通り、UF2 が出て、
実機で `svc handled num:1` 相当の出力が出る。Linux (大文字小文字区別) でも
configure が通る構成になっている (CI が無いうちは手元で `git ls-files` の
大文字小文字を目視確認でよい)。

### Phase 0 完了記録 (2026-08-19 実施・**実機確認済み**)

実施済み (すべて未コミット):
- ディレクトリ正規化: `source/Object_System` → `source/object_system`、
  `source/kernel_Object` → `source/kernel_object`。source/CMakeLists.txt 修正済み
- guard 衝突解消: `kernel_object_abi/` 削除 (svc_number enum は abis/rp2040_abi.hpp へ
  統合)、dummy.hpp 2 本のガード分離
- main.cpp 書き直し (asm オペランド方向修正・ハンドラ登録を cpu_driver に一本化)、
  cpu_driver.cpp の nodiscard 警告修正、buildUtilities.cmake 整理
- Bazel bring-up: MODULE.bazel (pico-sdk 2.2.0 を local_path_override) / .bazelrc /
  .bazelversion (**8.3.1**。7.x は SDK の REPO.bazel の `ignore_directories` で不可) /
  BUILD.bazel / configs/BUILD.bazel (expand_template で config.hpp 生成) /
  modules/pico_sdk_support/BUILD.bazel (headers と実装を分離、main は alwayslink)

ビルドコマンド (検証済み):
```
# Bazel (既定 = rp2350/pico2。.bazelrc 参照)
bazelisk build //:shizuku        # ELF → bazel-bin/shizuku
bazelisk build //:shizuku_uf2    # UF2 → bazel-bin/shizuku.uf2 (ローカル picotool 使用)

# CMake (並存中)
export PICO_SDK_PATH=$HOME/.pico-sdk/sdk/2.2.0 \
       PICO_TOOLCHAIN_PATH=$HOME/.pico-sdk/toolchain/14_2_Rel1
export PATH=$PICO_TOOLCHAIN_PATH/bin:$PATH
cmake -S . -B build -G Ninja \
  -DPRELOAD_TOOLCHAIN_FILE=$PWD/cmake/pico_sdk/pico_sdk_import.cmake \
  -DPOSTLOAD_TOOLCHAIN_FILE=$PWD/cmake/pico_sdk/pico_sdk_setup.cmake \
  -DPICO_BOARD=pico2 -DPICO_PLATFORM=rp2350 \
  -DSHIZUKU_CPU_DRIVER=rp2040 -DSHIZUKU_CPU_COUNT=1 \
  -DSHIZUKU_MEMORY_MANAGER=shizuku::memory_managers::pico_sdk \
  -DSHIZUKU_OBJECT_USE_THREAD_TABLE="shizuku::templates::static_table<shizuku::THREAD,128>" \
  -DSHIZUKU_OBJECT_USE_METHOD_TABLE="shizuku::templates::static_table<shizuku::METHOD,128>" \
  -DSHIZUKU_OBJECT_USE_MEMORY_TABLE="shizuku::templates::static_table<shizuku::MEMORY_MAP,128>"
cmake --build build -j8          # → build/Shizuku.elf / Shizuku.uf2
```

同等性の確認結果: 生成 config.hpp は空白差のみ。ELF は両者とも stdio_usb 込みで
text 41.9k (Bazel) vs 42.2k (CMake)、bss ほぼ同一。

既知の注意点:
- **UF2 aspect は使えない** (Apple Silicon ホストで picotool のソースビルドが
  pico-sdk 2.2.0 の `src/host/hardware_sync` の clang builtin 衝突で失敗)。
  `//:shizuku_uf2` genrule が `/opt/picotool/bin/picotool` を直接叩く (非 hermetic)。
  SDK 更新で直ったら .bazelrc のコメントアウトを戻す
- **-Og を全体に付与** (.bazelrc)。fastbuild の -O0 だと svc ラッパの inline asm が
  「入力 5 + 出力 2 + r0-r3/ip/lr clobber」でレジスタ割付不能になるため。
  最適化構成を分けるときは svc ラッパ側の register 指定 (`register uint32_t r0 asm("r0")`
  方式への書き換え) で根治するのが筋 (Phase 2 の ABI 書き直しで対応)
- CMake は環境変数の PICO_SDK_PATH でなく `~/github/pico-sdk` (git clone) を掴んで
  いた形跡がある (configure ログ)。Bazel 側は 2.2.0 に固定済み。CMake 削除まで
  の間に SDK バージョン差異が出たら疑うこと

## Phase 1 — arch 抽象の定義 + ARMv8-M バックエンド

目的: PORT §5.1/§5.2 の arch 層を concept として確定し、参照実装の検証済み機構を
armv8m バックエンドとして移植する。**この段階ではまだ現行と同等の動きでよい** (退行なし確認)。

1. `internal_headers/shizuku/concepts/arch.hpp` を新規作成。最小要件 (PORT §5.1):
   - `context_t` 型 / 文脈の退避・復帰 (asm 側との契約)
   - `exc_frame_bytes(ctx)` — 例外フレーム実サイズ (FP 拡張・パディング込み)
   - `psp_after_return(ctx)` — **復帰後の SP** (xPSR bit9 の +4 を吸収する唯一の点。
     PORT §5.2「移植で最初に壊れるところ」)
   - 呼び出しフレーム push/pop に必要な幾何計算 (退避域サイズ、書き換え用フレーム位置)
   - `set_priv` / 現在特権の取得 (自己申告テスト用)
   - `stack_limit_set` / 検査 (PSPLIM。無い ISA はソフト縮退 = Q3)
   - `cas32(ptr, expected, desired)` + release store / acquire load (D9)
   - 時限割り込みの装填/解除 (systick 相当)、強制切替の起票 (pendsv 相当)
   - **要件コメントとして明記**: 「スレッド切替は最低優先度の遅延例外でのみ起こす」
     (D10。破ると §14.5.1 の無償相互排除が静かに消える)
   - 返す値の意味は doc コメントで縛る (「関数を用意し忘れる」でなく「意味が違う」
     形で壊れるため。PORT §5.4)
2. `modules/pico_sdk_support/internal_headers/shizuku/archs/armv8m.hpp` + `armv8m_ctx.S`:
   - 参照実装の `context_t` (オフセット 32/36/40/104/108) と CTX_SAVE/CTX_RESTORE を
     ほぼそのまま移植。`.equ` と `static_assert` の両縛りも移植する
   - 幾何関数は参照 `call_frame_push` の定数 (fs=32/104, has_fp 64B, −8 余裕) を
     arch 関数に括り出す
   - claim CAS (`__atomic_compare_exchange_n`) を `cas32` に実装
3. board 概念の最小版 `concepts/board.hpp` + `boards/rp2350_pico2.hpp`:
   時刻 (`time_us_64` 相当)、コア番号、コア起動、例外ハンドラ登録、noinit RAM 提供。
   クロック API (D11) は骨だけ置く
4. dummy arch/board (ホストコンパイル検査用) を concept に適合させて維持

**受け入れ条件**: `static_assert(concepts::arch_requires<archs::armv8m>)` が通る。
svc 往復デモが arch 経由の実装で動く (機能は Phase 0 と同等)。
concept を満たさないダミー型で「どの要件が欠けたか」が 1 行診断で出ることを確認。

### Phase 1 完了記録 (2026-08-19 実施・**実機確認済み**)

実施済み (未コミット):
- `concepts/arch.hpp` / `concepts/board.hpp` 新設。意味の契約 (psp_after_return の
  +4 吸収、優先度規約、atomic 継ぎ目) を doc コメントで縛った。timer 系
  (timer_oneshot / pend_context_switch) は **GRANT 実装時に requires へ追加する**
  と明記して未要件化
- `modules/pico_sdk_support/internal_headers/shizuku/archs/armv8m.hpp` +
  `armv8m_ctx.S` 新設: 参照実装の context_t (オフセット 32/36/40/104/108) と
  CTX_SAVE/CTX_RESTORE を移植。asm フックは `shizuku_current_context` /
  `shizuku_svc_dispatch` / `shizuku_pendsv_dispatch` の 3 本 (extern "C")
- `boards/rp2350_pico2.hpp` + `board.cpp` 新設: 例外結線 (登録は core0 のみ、
  優先度は per-core) と core_num/time_us。**Phase 1 の暫定として現在文脈フック
  (per-core の g_current_context) も board.cpp に置いた — Phase 2 でカーネル側へ移す**
- `templates/cpu_manager.hpp` を `cpu_manager<ARCH, BOARD, CORE_COUNT, THREAD>` に
  再定義 (concept で制約)。`init()` は「呼び出したコアのみ」初期化する意味論に変更
- `configs/config_template.hpp.in` を ARCH/BOARD 注入形に変更
  (`SHIZUKU_CPU_DRIVER` → `SHIZUKU_ARCH` + `SHIZUKU_BOARD`)。kit / Bazel 側も更新
- 旧 `cpu_drivers/rp2040.hpp` / `cpu_driver.cpp` / `concepts/cpu_driver.hpp` /
  `concepts/context.hpp` を削除。`archs/dummy.hpp` (concept 適合のダミー、ホストで
  syntax check 済み) を新設
- `source/kernel.cpp` のデッドコード (旧オブジェクト生成スケッチ) を削除
- 検証: Bazel / CMake 両方でビルド成功。ELF に `shizuku_armv8m_svc_entry` 等を確認。
  svc デモ (`svc 1` 往復 → success 印字) は**実機確認済み** (2026-08-19、
  pico2 で success の周期出力を確認 = CTX_SAVE/RESTORE 経路が実機で通っている)

注意 (Phase 2 への引き継ぎ):
- `abis/rp2040_abi.hpp` (svc ラッパ) は名前も ABI も暫定のまま。Q1 (レジスタ渡し統一)
  の決定と合わせて Phase 2 で書き直す
- `templates/kernel.hpp` はまだ OBJECT パラメータと object_table を持つ (D1 未適用)。
  `templates/object.hpp` の特権 enum (D6) も未処理。どちらも Phase 2/3 の対象
- main.cpp の PSP 移行 + svc ループは実験コードのまま。Phase 2 で thread0
  ブートストラップ (参照 shizu.hpp の set_current_context_as_kernel_init 相当) に置換する

## Phase 2 — カーネルテンプレート (オブジェクト概念なし) + プリミティブ

目的: DESIGN §7 の ABI を `templates::kernel<ARCH, BOARD, …>` として実装する。
**カーネルからオブジェクト概念を外す (D1) のはここ。**

1. `templates::kernel` を再定義: thread_table (疎, D8) / per-core current_thread /
   grant_stacks / 登録ハンドラ (entry + 信頼フラグ)。`OBJECT_T` パラメータ削除
2. スレッド: 参照実装 `thread_t` の状態機械 (UNINITIALIZED/READY/RUNNING/SUSPENDED/
   WAIT_GRANT/RESERVED) + RESERVED CAS 予約 + READY release 公開を移植。
   ただし `object_id` は持たない → **`cookie` (uintptr_t, カーネルは解釈しない)**
3. 呼び出しフレーム: 参照 `call_frame_push/pop` を移植し、
   `caller_object_id` → `caller_cookie` に置換。幾何は arch 関数経由に (I-3/I-4/I-5)
4. プリミティブ実装 (D3):
   - `CALL(entry_pc, callee_cookie, caller_cookie, protection, args…)` — 信頼活性化のみ (I-2)
   - `RETURN(n, value, err, depth_claim)` — 両側チェック (§9.3)。失敗時 1 段も落とさない
   - `SET_HANDLER(entry)` — 信頼活性化のみ
   - `SWITCH(tid)` / `GRANT(tid, us)` — 参照実装の claim/grant/PendSV 経路を移植 (I-7)
   - ディスパッチは「信頼された活性化か」の 1 ビットのみ (I-1)。svc 番号は
     経路決定に使わない。**未知番号は必ずエラー返却** (信頼側からの未知番号のみ panic 可)
5. エラー: すべて I-9 準拠 (NO_STACK / DEPTH_MISMATCH / BAD_COUNT / NOT_READY /
   BAD_AFFINITY / UNKNOWN_API…)。ワイヤは 0=成功 enum (`wire_result.hpp`)
6. 診断: panic ring / svc_trace / 「固まっても出る」経路 (タイマからの同期排出) を
   この段階で入れる (PORT §7。後回しにすると移植中の無言ハングで詰む)

**受け入れ条件** (最小プローブの梯子。DESIGN §16):
- 1 段 CALL/RETURN 往復 (FP 非活性) → FP 活性 → 2 段 → N 段、の順で全部 PASS
- 不正な depth_claim を申告して**系が生存**し、エラーが返る (I-9 テスト)
- スタック不足で NO_STACK が返る (PSPLIM 手前拒否)
- 2 コアで SWITCH/GRANT のストレス (参照 smp_stress の縮小版) が ADVANCING

### Phase 2 進捗記録 (2026-08-19 実装。**実機未確認**)

実装済み (CALL / RETURN / SET_HANDLER。SWITCH / GRANT は Phase 2b へ):
- `kernel_abi.hpp`: プリミティブ 5 種と `call_request` / `kernel_error`。
  **カーネルの語彙に「未知の番号」「権限がない」は無い** (§3.6.1)
- `templates/kernel.hpp` を D1 の形へ再定義: `kernel<CPU_MANAGER, MEMORY_MANAGER,
  THREAD_COUNT>`。OBJECT パラメータと object_table を削除し、現在オブジェクトは
  不透明な cookie に。`templates/object.hpp` (特権 enum を含む旧型) は削除 (D6)
- `templates/thread.hpp`: 状態機械 + 活性化状態 (cookie / caller_cookie / trusted) +
  call_stack (top/depth)
- `source/kernel/dispatch.cpp`: 呼び出しフレームの push/pop、do_call、svc_dispatch。
  トランポリンと CALL は **do_call 1 本を共有**する (別機構にしない)
- `source/kernel/init.cpp`: init + bootstrap (今の実行をスレッド 0 として採用)
- arch 拡張: 幾何 (normalize_frame / psp_after_return)、ABI スロット (arg/set_args/
  set_result/set_entry)、活性化情報、return stub、enter_thread_mode、syscall ラッパ
  (レジスタ明示束縛で最適化レベル非依存に)
- `source/selftest/call_ladder.cpp`: 梯子 (1 段 → FP 活性 → 6 段 → 段数申告ミス →
  スタック枯渇)。identity は呼ばれた側に申告させて突き合わせる

**参照実装からの意図的な差分** (どれも DESIGN 側の要求):
- フレームヘッダは文脈を**丸ごと**退避する (参照は r4-r11/control/exc_return/fp を
  個別に退避)。ISA 非依存にでき、pop が `*context = header->saved` の 1 行になる。
  代償は FP 非活性時も fp[16] を運ぶこと (64B/段)
- 参照の「-8 の余裕」を廃し、作業コピー側の整列パディング指定を消して
  (`normalize_frame`) 幾何を厳密一致させ、push のたびに `psp_after_return` で検算する
- 戻り口 (return stub) はネスト数を申告しない (0)。発行者がカーネル自身で、
  落とす枚数もカーネルが決めているため。申告が意味を持つのはオブジェクト側が
  段数を指定して巻き戻すとき (exit の 2 枚 pop など)

**★2026-08-20 の作り直し (ユーザー指摘 3 連発による)**:
1. 「活性化 (activation)」の概念を廃止。参照実装の `in_handler` は「走り方に権限が
   付く」形で、ハンドラから呼ばれた先まで特権化する穴だった
2. cookie / identity / 信頼ビットをカーネルから全廃。**経路は呼び出しフレームの
   段数のパリティだけ**で決まる (偶数 = オブジェクト、奇数 = ハンドラ。積むのは
   トランポリンと CALL の 2 つだけなので必ず交互になる)
3. `call_request.return_pc` も廃止。戻り口はカーネルの 1 本で、同じ RETURN が
   誰から出たかで意味が変わる (プリミティブ / exit API)
4. **カーネルオブジェクトを先に実装**し (`source/kernel_object/handler.cpp`)、
   そこから必要なものだけをカーネルに残す順序へ修正。自己テストが偽 kobj を
   演じる形はやめ、オブジェクトランドの API だけを使う実経路の検査にした
5. ペリフェラルオブジェクト (GPIO / SPI) を board モジュールへ追加 (D18)

**次にやること**: 実機で自己テストを流す (下の受け入れ条件)。LED が点滅すれば
オブジェクト経由の呼び出しが回っている目視証拠になる。その後 Phase 2b
(SWITCH / GRANT / スレッド生成) → Phase 4 (親子関係と md)。

## Phase 3 — カーネルオブジェクト (kobj) + 委譲ファミリ

目的: オブジェクトモデル・表・方針をすべて kobj 側に実装 (PORT §3)。

1. `templates::kernel_object` (ISA 非依存): 疎な object_table / method_table /
   svc ルート表 / (後の) md 表。コア間共有表は ktab_lock (短スピン, D9) で保護 (§14.5.2)
2. トランポリン受け皿: SET_HANDLER で登録するハンドラ + arch の handler_shim。
   ネスト数 (r7 相当) の申告返しを共通ヘッダ化 (参照 svc_handler.hpp の教訓:
   サブシステムに自作させると必ず間違える)
3. 委譲 2 形態 (D4): **CALL_STRICT (既定)** / CALL。caller_cookie は kobj が自分の
   帳簿から埋める。svc 範囲のサブカーネル委譲も CALL_STRICT (svc 番号を arg0 に転送)
4. exit は kobj からの `RETURN(n=2, depth_claim)` で実装 (D5)。+1 の知識は kobj の
   1 ヶ所のみ。exit 要求は全オブジェクトに開放 (self-limited なので昇格でない)
5. obj_api 相当の再構成 (create_object/async_call/export/yield/sleep/run_for/
   set_affinity/set_budget…)。**Q1 の決定に従い番号はレジスタ渡し ABI で新規採番**。
   r4==100/255 の臨時ハックは移植しない
6. スケジューラ (sched_pick_next / scheduler_idle_loop) を kobj の方針として移植

**受け入れ条件**:
- **identity e2e テスト**: ネスト各層で「誰に呼ばれたか」を突き合わせ
  (参照 svc_delegate_test の caller(a=28 b=27) 相当。**これが無いと §12.1 型の
  「黙って化ける」を検出できない** — PORT §4.6 ★注意)
- 委譲サブハンドラが**非特権のまま**走ることを CONTROL 自己申告で確認 (I-8)
- 1 段プローブ → 多段ネスト委譲の梯子が PASS
- 参照実装と同等のアプリなしベンチ (svc 往復 / CALL 往復のサイクル数) を記録

## Phase 4 — アクセス制御の復活 (DESIGN §17 の順序で)

1. caller_cookie による identity 伝播の常時化 (Phase 3 で完了しているはずの確認)
2. **親子関係**: 表に親を持たせ、既定を「親子のメソッドのみ呼べる」に。
   **拒否のテスト** (親子でない相手を呼んで**拒否されること**) を必ず書く —
   許可のテストだけでは無意味 (DESIGN §16)
3. **md を権限付与機構として新規実装**: 呼び出し元索引 (`md_table[caller][slot]`) /
   払い出しは特権オブジェクト (軸 C) のみ / O(1) 呼び出し。現行 md は移植しない
4. オブジェクト arena (メモリの保有と解放, §10.1/§12.5)。非特権化 (§11.2.2) の前提

**受け入れ条件**: 上記それぞれの許可 + 拒否テスト。arena 上のオブジェクトが
private global なしで動く最小例。

## Phase 5 — 保護 (MPU) と非特権実行

参照実装の Step0 (W^X) を board/arch に移植 → 非特権最小プローブ (unpriv_probe 同型,
**MRS CONTROL 自己申告必須** — DESIGN §11.2.0 の測定事故を繰り返さない) →
MemManage ハンドラ (違反しても系が止まらない, §11.2.4) → per-object region (§11.3,
CALL の protection 引数に region set を載せる)。

## Phase 6 以降 (順不同・必要に応じて)

- ストリーム (制御プレーンのみ svc。connect + DMA ポンプは board 依存)
- XNO リポジトリ作成 (D17) と公開ヘッダの切り出し
- 2nd ISA: ARMv7-M (PSPLIM 無し / FP 差分のみ) → RISC-V (PORT §6)
- クロック変更 API の本実装 (D11, XIP_FLASH_CLOCK.md)

---

## 地雷一覧 (全フェーズ共通。参照実装が実際に踏んだもの)

| 地雷 | 対策 |
|---|---|
| xPSR bit9 パディングで復帰後 SP が +4 → 退避域を踏んで無言ロックアップ | `psp_after_return` を arch に一元化。幾何自己テストを FP 有無×整列有無で回す |
| 元の例外フレームを動かすと FP 拡張 S0-S15 と乖離 | I-3。フレームは複製のみ、原本は動かさない |
| pop 時の幾何再計算 | I-5。push 時の記録値を読み戻す |
| ハンドラ shim の引数位置 (第 8 引数は r7、r12 ではない) | shim は共通ヘッダ 1 ヶ所のみ。自作禁止 |
| 委譲時に元 syscall の引数を転送し忘れ/取り違え → 引数が obj ID に化けて無限再帰 (参照実装が REDISPATCH で実際に踏んだ。kernel.cpp:999) | 委譲は kobj の CALL_STRICT に一本化し (D4/§3.6)、identity e2e テストで検出 |
| 「見てから作る」の TOCTOU (2 コアが同じスレッド枠を掴み panic) | RESERVED への CAS 予約 (arch_claim_thread_slot 同型) |
| READY を初期化冒頭で公開 → 途中初期化のまま他コアが claim | READY は release store で最後に公開。affinity/budget は公開前に確定 |
| 未知の svc 番号を default で黙殺 → 状態設定が無音で消え「動いた」と誤認 | 未知番号は必ずエラー返却。状態の主張は対象自身に申告させる (MRS CONTROL 等) |
| 生存カウンタを機能の証拠にする (poll は前進、実イベントは停止) | カウンタは期待値と突き合わせる。「出るべきものが出ている」を見る |
| printf が系を固める (pico_stdio の print_mutex 1s WFE) | カーネル自己テスト段階から出力はリング + 同期排出経路。診断経路を最初に作る |
| exclusive handler の二重登録 (pico-sdk は panic) | 例外ハンドラ登録は board init の 1 ヶ所に集約。RAM ベクタは両コア共有な点に注意 (PendSV/SVC の登録は core0 の 1 回だけ、優先度・MPU・SysTick は per-core banked なので各コアで) |
| 固まったファームは書き込みツールが「成功」のまま書かない | 書き込み完了は出力行で判定。焼く前にシリアル待ち受けを起動 |
| ~~PICO_FLASH_SPI_CLKDIV で分周調整~~ 死んだつまみ | フラッシュクロックは flash_clock.cpp 方式 (目標周波数から導出) を将来移植 |

## 進め方の作法

- 各フェーズの完了時に本 docs (特にこのファイルと README の「現在地」) を更新する
- 設計判断で DESIGN/PORT と矛盾しそうになったら、コードを曲げる前に 03 の
  D/Q リストに追記してユーザーに確認する
- コミットは機能単位で小さく。ブランチは `0.1` (PR 先 `stable`)
