#set document(title: "Shizuku V3 カーネル解説: method call と svc の軌跡", author: "Shizuku")
#set page(paper: "a4", margin: (x: 18mm, y: 18mm), numbering: "1")
#set text(font: ("Hiragino Sans", "Arial"), size: 9pt, lang: "ja")
#set par(leading: 0.65em)
#set heading(numbering: "1.1")
#show raw: set text(font: "Menlo", size: 7.5pt)
#let ref(s) = text(size: 7.8pt, fill: rgb("555555"), s)

= Shizuku V3 カーネル解説: method call と svc の軌跡

状態: *解説記事 (一次資料に基づく技術解説)*。2026-10-07 (差し戻し R1 改訂)。 \
読者と目的: Shizuku の作者本人が、V3 カーネル設計書 (`docs/design/kernel_v3.typ`) を読み解くための手引き。 \
特に *method call と svc の実行軌跡を、V2 と V3 で、スタック (PSP/MSP、例外フレーム、callee-saved、call frame / ledger) とレジスタ (r0--r3/r12、LR/EXC_RETURN、CONTROL、PSPLIM、MPU) の変化を 1 ステップずつ追って* 比較・解説する。

本書で用いる厳格なステータスラベル:
- *[現行コード/下ごしらえ]*: 現行リポジトリのソースコード (`modules/pico_sdk_support/rp2350/`, `source/kernel/`, `internal_headers/` 等) に実在する V3 下ごしらえ (dirty worktree) またはベースライン実装。
- *[実測]*: `perf/svc-in-ram:docs/record/measurements/cost3_report.md`、`cost2_trace.log`、`perf/svc-in-ram:docs/record/kernel_v2_record.typ` 等で実機 (RP2350 150MHz) 採取された確定データ。
- *[設計]*: `docs/design/kernel_v3.typ` で策定された V3 の確定決定事項。
- *[未確定]*: 設計書内で未確定と明記されている事項 (数値 ID や引数 layout など)。
- *[棚上げ]*: 設計書内で棚上げと明記されている事項 (動的ホットスワップなど)。

---

== 前提: Cortex-M33 の例外動作と Shizuku のアーキテクチャ語彙

=== Cortex-M33 (ARMv8-M Mainline) の例外入口・復帰とレジスタ規約

Shizuku のカーネル・オブジェクト境界は、ARMv8-M の例外機構に全面的に依存している。公式資料 (#link("https://developer.arm.com/documentation/100230/0004/functional-description/programmers-model/processor-core-registers-summary")[Arm Cortex-M33 TRM 100230_0004 (Core Registers Summary)] および #link("https://developer.arm.com/-/media/Arm%20Developer%20Community/PDF/Armv8-M%20Architecture-Stack%20sealing%20and%20why%20it%20is%20needed%20in%20TrustZone%20for%20Armv8-M.pdf")[Armv8-M Architecture - Stack sealing and why it is needed in TrustZone for Armv8-M]) に基づき、ハードウェアの基本動作を整理する。

#table(columns: (1.2fr, 2.8fr), inset: 4pt,
  table.header(repeat: true, [ハードウェア要素], [動作・仕様・Shizuku における役割]),
  [ハードウェア Stacking], [Thread mode で `svc 0` を発行すると、ハードウェアが自動的に 8 語 (32B: `r0`, `r1`, `r2`, `r3`, `r12`, `LR`, `PC`, `xPSR`) をアクティブなスタック (通常は PSP) に push する。],
  [8B スタック整列パディング], [SVC 発行直前に SP が 8B 整列でない場合、ハードウェアは 4B パディングを挿入して 8B 整列にし、`xPSR` の bit 9 (SPREALIGN) を 1 に立てる (`armv8m.hpp:98–108`)。例外復帰時にハードウェアが自動で +4 補正する。],
  [Lazy FP Stacking], [FPU が活性化している場合 (`FPCCR.LSPACT=1` または `CONTROL.FPCA=1`)、ハードウェアは追加で 18 語 (72B: `s0`--`s15`, `FPSCR`, 予約語) の領域を予約し、計 26 語 (104B) の拡張フレームを形成する (`armv8m.hpp:86, 96`)。実体化は FP 命令実行時まで遅延される。],
  [`EXC_RETURN`], [例外突入時、`LR` レジスタには特殊な戻りトークンが装填される。通常は `0xFFFFFFFD` (bit 4=1: 標準フレーム、bit 3=1: Thread mode 復帰、bit 2=1: PSP スタック復帰)。Handler mode から `bx lr` することでハードウェア unstacking が走り例外復帰する (`armv8m_ctx.S:47–58, 105–106`)。],
  [MSP と PSP], [MSP (Main Stack Pointer) は Handler mode (割り込み・例外ハンドラ) で使用される。PSP (Process Stack Pointer) は Thread mode (ユーザーオブジェクトおよび Thread カーネル) で使用される。],
  [`CONTROL` レジスタ], [bit 0 (`nPRIV`: 0=特権, 1=非特権)、bit 1 (`SPSEL`: 0=MSP, 1=PSP)、bit 2 (`FPCA`: FPU 活性)。Shizuku は Thread mode 実行中常に `SPSEL=1` (PSP) を維持する (`armv8m_ctx.S:80–91`)。],
  [`PSPLIM` レジスタ], [ハードウェアスタック下限。PSP が `PSPLIM` を下回ると即座に UsageFault (設定により HardFault へエスカレーション) が発火する (`armv8m.hpp:76`, `dispatch.cpp:84–109`)。],
  [PMSAv8 MPU], [RP2350 の保護機構。RBAR (Base) と RLAR (Limit) で 32B 整列の保護区画を指定する。Shizuku では Region 2 を動的な `GRANT_REGION` 窓として使用 (`arch_glue.cpp:52–76`)。],
)

=== Shizuku のアーキテクチャ語彙

設計書とコードを読み解く上で必須となる Shizuku 固有の概念を定義する。

- *カーネル (kernel)*: `source/kernel/dispatch.cpp` に実装される最下層。機構 (mechanism) のみを提供し、コンテキスト切替、フレームの push/pop、MPU 窓の反映、スケジューリングの primitive を担う。V2 の設計原則 D1「カーネルはオブジェクトを知らない」は後に緩和され、V2 カーネルは現在実行中コンテキストの識別子 (`current_object()`)、種別 (`object_kind`: `PLAIN`, `HANDLER`, `KERNEL_OBJECT`)、特権台帳の呼出し履歴を保持・検査してルーティングを行う (`kernel_abi.hpp:47–67`, `templates/thread.hpp:58–68`, `dispatch.cpp:8–14, 232–260, 292–341`)。ただし、オブジェクトランドの方針、メソッド一覧、エクスポートテーブルそのものはカーネル内には持たない。
- *カーネルオブジェクト (kernel object / kobj)*: `source/kernel_object/handler.cpp` に実装される方針 (policy) の提供者。システムリソース (スレッド、オブジェクト、ストリーム、メモリ) を管理する。V2 では Thread mode 上の独立したハンドラ (`object_kind::HANDLER` / `KERNEL_OBJECT`) として実行される。
- *アクター (actor)*: 保護ドメインと実行権限の論理単位。V2 では各オブジェクトが固有の ID を持つ。
- *台帳 (call ledger)*: 特権メモリ (`g_bookkeeping_storage`) 上に確保された呼び出し履歴管理構造体 (`templates/thread.hpp:41–48`)。呼び出し先オブジェクト ID、種別 (`kind`)、経路 (`via`) 等を各フレーム段数ごとに記録し、非特権オブジェクトからは改ざんできない。呼び出し元情報は退避コンテキスト (`call_frame_header->saved`) や下位台帳に温存される。
- *コールフレーム (call frame)*: スレッドの PSP スタック上に積まれるフレーム。呼び出し元のコンテキスト退避域 (`call_frame_header`) と、ハードウェア例外フレームの作業コピー (`callee_frame`) から構成される (`templates/kernel.hpp:85–90`, `dispatch.cpp:68–142`)。
- *コンテキスト再利用 (context reuse)*: 別アクターを呼び出す際、新しいスレッドを割り当てる (`SPAWN`) のではなく、同一スレッドの同一 PSP スタック上でコンテキストを退避・積み替えて実行を遷移させる手法 (`dispatch.cpp:68–160`)。
- *オブジェクトランド (objectland)*: 非特権または特権で動作するオブジェクト群の実行環境。親ハンドラ (`parent_handler`) が未登録の raw SVC を捕捉・中継する階層構造を持つ (`dispatch.cpp:292–341`)。

---

== V2 の SVC 1 回 (GET_CURRENT_OBJECT) の詳細追跡

=== 2 回の境界越えの全貌

V2 において、最も単純な同期システムコールである `GET_CURRENT_OBJECT` (API ID=5) を発行した場合の実行軌跡を追う。
この呼び出しは、*SVC 2 回、例外復帰 2 回、push 1 回、pop 1 回、MPU 更新 2 回* を消費する (`perf/svc-in-ram:docs/record/measurements/cost3_report.md:45–57` [実測])。

なぜ 1 回の API 呼び出しに 2 回の SVC が必要なのか？
その根本原因は、*「カーネル (Handler mode)」と「カーネルオブジェクト (Thread mode)」が分離しており、カーネルオブジェクトが通常の C 関数として return したあと、カーネルの巻き戻しプリミティブ (`primitive::RETURN`) を呼ぶためにもう一度 SVC を発行する構造* にある。

#table(columns: (0.8fr, 1.3fr, 2.2fr, 1.7fr), inset: 4pt,
  table.header(repeat: true, [境界], [実行モード / 主体], [処理内容], [コード位置 (V2 ベースライン)]),
  [SVC #1], [Thread (非特権) → Handler], [Caller が `GET_CURRENT_OBJECT` を発行。ハードウェア stacking 後、`CTX_SAVE`。親 kobj へのトランポリン作成 (`call_frame_push`)。], [`modules/pico_sdk_support/rp2350/internal_headers/shizuku/archs/armv8m.hpp:406–419` \ `modules/pico_sdk_support/rp2350/armv8m_ctx.S:112–119` \ `source/kernel/dispatch.cpp:292–341`],
  [復帰 #1], [Handler → Thread (特権)], [`CTX_RESTORE` 後、`bx lr` で例外復帰。復帰先は kobj の `handler_entry_point`。], [`modules/pico_sdk_support/rp2350/armv8m_ctx.S:58–107` \ `source/kernel_object/handler.cpp:121–127`],
  [処理], [Thread (特権)], [kobj が `handle()` を実行。`current_object()` を取得して通常の C return。戻り先は `return_stub`。], [`handler.cpp:1606–1608` \ `handler.cpp:1693–1694`],
  [SVC #2], [Thread (特権) → Handler], [`return_stub` が `primitive::RETURN` (count=1) を発行。`CTX_SAVE` 後、`call_frame_pop` で caller の元コンテキストを復元。], [`armv8m_ctx.S:142–154` \ `dispatch.cpp:232–260`],
  [復帰 #2], [Handler → Thread (非特権)], [`CTX_RESTORE` 後、`bx lr` で例外復帰。復帰先は caller の SVC 直後。], [`armv8m_ctx.S:58–107` \ `armv8m.hpp:418`],
)

注: V2 のコード位置は記録スナップショット (`git show 3239d3761fd1898b78411434a31f91790251bd3b:<path>`) に基づく。現行の作業ツリーコードは V3 下ごしらえによる変更を含んでいる。

=== 各時点のスタック断面とレジスタ推移

*前提条件 (代表ケース)*:
- Caller 突入時の $P_0$ は 8B 整列。
- 基本例外フレーム 32B (`exc_frame_bytes = 32`)。
- FPU 非活性 (`CONTROL.FPCA = 0`)。
- 割り込み・遅延切替の割り込みなし。
可変ケース (非整列時の 4B ハードウェアパディング、FPU 活性時の 104B 拡張フレーム) は本代表ケースの幾何計算とは別に扱う。

==== [時点 1] Caller が SVC を発行する直前
- *モード / 特権*: Thread mode、非特権 (`CONTROL.nPRIV=1`, `CONTROL.SPSEL=1`, `CONTROL.FPCA=0`)
- *スタックポインタ*:
  - `PSP`: $P_0$ (Caller のスタックトップ。8B 整列)
  - `MSP`: $M_0$ (カーネルの予約 MSP)
- *HW Frame*: 未作成 (Caller の C スタックフレームが $P_0$ 以上に存在)
- *Callee-saved*: `r4`--`r11` は Caller のローカル変数を保持
- *Call Frame / Ledger*: `depth = 0`, `call_stack.top = 0`
- *汎用レジスタ*:
  - `r0`: 5 (`object_api::GET_CURRENT_OBJECT`)
  - `r1`--`r3`, `r12`: 0 (未使用引数)
- *`LR` / `PC`*: `LR` = Caller の呼出し元リターンアドレス, `PC` = `svc 0` 命令
- *`PSPLIM`*: Caller のスタック下限 $L_"caller"$
- *MPU*: Caller ドメイン (Region 2 `GRANT_REGION` は Caller のデータ領域または窓なし)

==== [時点 2] SVC #1 発行直後 (ハードウェア Stacking 完了、Handler mode 突入)
- *ハードウェア動作*:
  - PSP 上に 8 語 (32B: `r0`, `r1`, `r2`, `r3`, `r12`, `LR`, `PC`, `xPSR`) の HW Frame A を自動 push。
  - $P_1 = P_0 - 32$ (8B 整列前提のため、ハードウェアパディングなし、`xPSR.SPREALIGN` bit 9 = 0)。
- *モード / 特権*: Handler mode、特権 (`CONTROL.SPSEL` は Handler mode 中ハードウェアにより無視され常に MSP が動作スタックとなる)
- *スタックポインタ*: `PSP` = $P_1$ ($P_0 - 32$), `MSP` = $M_0$
- *HW Frame*: HW Frame A ($P_1$ .. $P_0$): `r0=5`, `r1=0`, `r2=0`, `r3=0`, `r12=0`, `LR=Caller LR`, `PC=Caller SVC次命令`, `xPSR`
- *Callee-saved*: `r4`--`r11` はレジスタに保持
- *Call Frame / Ledger*: 変更なし (`depth = 0`)
- *`LR` (`EXC_RETURN`)*: `0xFFFFFFFD` (Return to Thread mode using PSP, basic frame)
- *`CONTROL` / `PSPLIM` / MPU*: 変更なし

==== [時点 3] `CTX_SAVE` 完了直後 (`armv8m_ctx.S:25–52`)
- *モード / 特権*: Handler mode、特権
- *MSP の厳密な動作*:
  `CTX_SAVE` マクロの先頭 (`armv8m_ctx.S:26`) で `push {lr}` を行い、MSP は一時的に $M_1 = M_0 - 4$ となる。しかし、`bl shizuku_current_context` でコンテキスト先頭を取得し、レジスタ群を退避した後、マクロ内 line 39 で `pop {r1}` を実行して `EXC_RETURN` をスタックから回収する。*したがって、マクロ完了時点で MSP は直ちに $M_0$ に復帰している*。その後の C 関数 `shizuku_svc_dispatch` 実行中およびディスパッチ処理中の基準 MSP は常に $M_0$ である。
- *`context_t` (120B) への退避*:
  現在スレッドの `context_t` 構造体に以下を退避 (`armv8m.hpp:68–82` [現行コード/下ごしらえ]):
  - `r4`--`r11` (32B: offset 0..28)
  - `sp` = $P_1$ (4B: offset 32, HW Frame A の先頭アドレス)
  - `exc_return` = `0xFFFFFFFD` (4B: offset 36)
  - `fp[16]` (64B: offset 40..103, S16--S31, FP 非活性時は保存スキップ)
  - `psplim` = $L_"caller"$ (4B: offset 104)
  - `control` = `CONTROL_UNPRIV_PSP` (4B: offset 108)
  - `region_base`, `region_limit` (8B: offset 112..116)
  計 120 バイトのコンテキストペイロードが退避される。
- *スタックポインタ*: `PSP` = $P_1$ ($P_0 - 32$), `MSP` = $M_0$
- *MPU*: 変更なし

==== [時点 4] `shizuku_svc_dispatch` による `call_frame_push` 完了直後 (`dispatch.cpp:68–142, 292–341`)
- *モード / 特権*: Handler mode、特権
- Caller は `object_kind::PLAIN` (非 KERNEL_OBJECT) であるため、登録済み親 kobj (root 構成時は ID 0) の `handler_entry_point` へルーティングされる (`dispatch.cpp:292–341`)。
- *スタックの幾何計算* (`templates/kernel.hpp:85–90`, `dispatch.cpp:68–142`):
  - `frame_bytes = 32` (基本例外フレーム)
  - `sizeof(call_frame_header)` = 132B (`prev`: 4B + `total_bytes`: 4B + `frame_bytes`: 4B + `saved`: 120B `context_t` = 132B)
  - `total = (sizeof(call_frame_header) + frame_bytes + 7) & ~7 = (132 + 32 + 7) & ~7 = 168B` (8B 境界整列)
  - `caller_stack = context->sp + frame_bytes = P_1 + 32 = P_0`
  - `snapshot = caller_stack - total = P_0 - 168` (ここに `call_frame_header` を配置)
  - `callee_frame = snapshot - frame_bytes = (P_0 - 168) - 32 = P_0 - 200` (呼び出し先の作業用例外フレーム)
  - 呼び出し元スタック $P_0$ から `callee_frame` $P_0 - 200$ までの総変位は *200B*。
  - ヘッダ配置スパン ($P_0 - 168$ .. $P_0 - 32$) の 136B には、132B の `call_frame_header` 構造体と、ソフトウェア round-up による 4B の空きスペースが収まる。
- *フレーム操作*:
  - `header->saved = *context`: Caller の 120B `context_t` (元の $P_1$ を含む) をヘッダへ完全コピー。
  - `memcpy(callee_frame, context->sp, 32)`: 元の HW Frame A を `callee_frame` へ複製。
  - `callee_frame->PC` = `kobj.parent_handler_entry` (`handler_entry_point`)
  - `callee_frame->LR` = `ARCH::return_stub()` (`shizuku_armv8m_return_stub`)
  - `callee_frame->r0` = 5 (`GET_CURRENT_OBJECT`)
  - `context->control`: `nPRIV` を 0 (特権) に設定
  - `context->sp` = `callee_frame` ($P_0 - 200$)
- *台帳 / コールスタック*:
  - `thread.call_stack.top = snapshot` ($P_0 - 168$)、`depth = 1`
  - 特権台帳 `ledger[0]` (`dispatch.cpp:318–322`) には、呼び出し先である登録済み親 kobj の `target_object`、`kind` (to_root なら `KERNEL_OBJECT`、専用ハンドラなら `HANDLER`)、`via = LEDGER_DISPATCH` を記録。Caller のオブジェクト情報は `thread.base_object` および退避された `header->saved` 内に温存される。
- *MPU*: kobj 実行用に更新 (特権実行のため全メモリ許可)

#table(columns: (1.5fr, 2.5fr, 4fr), inset: 4pt,
  table.header([アドレス], [スタック構造], [内容]),
  [$P_0$ (高位)], [Caller ローカル領域], [呼び出し元のローカル変数・C スタックフレーム],
  [$P_0 - 32$ .. $P_0$], [元の HW Frame A (32B)], [Caller の例外フレーム (温存される。P0 整列のため SPREALIGN パディングなし)],
  [$P_0 - 36$ .. $P_0 - 32$], [ソフトウェア整列空き (4B)], [call_frame_push の 8B round-up (total=168B) による空隙。ハードウェアパディングではない。],
  [$P_0 - 168$ .. $P_0 - 36$], [*call_frame_header (132B)*], [`prev`(=0, 4B), `total_bytes`(=168, 4B), `frame_bytes`(=32, 4B), `saved` (*Caller の 120B context_t*)],
  [$P_0 - 200$ .. $P_0 - 168$], [*callee_frame (32B)*], [`r0`(=5), `r1`..`r3`(=0), `r12`(=0), `LR`(=*return_stub*), `PC`(=*handler_entry_point*), `xPSR`],
)

==== [時点 5] `CTX_RESTORE 1` と例外復帰 #1 直後 (`armv8m_ctx.S:58–107`)
- *モード / 特権*: Thread mode、*特権* (`CONTROL.nPRIV=0`, `CONTROL.SPSEL=1`)
- *ハードウェア動作*:
  `bx lr` によりハードウェアが PSP ($P_0 - 200$) から `callee_frame` (32B) を unstack！
- *スタックポインタ*:
  - `PSP`: $P_0 - 200 + 32 = P_0 - 168$ (`snapshot` の位置！)
  - `MSP`: $M_0$
- *レジスタ*:
  - `PC`: `handler_entry_point`
  - `LR`: `shizuku_armv8m_return_stub`
  - `r0`: 5 (`GET_CURRENT_OBJECT`)
  - `r1`--`r3`, `r12`: 0
- *`PSPLIM`*: kobj のスタック下限 (ヘッドルーム検査済み)
- *MPU*: 特権アクセス

==== [時点 6] kobj のハンドラ実行と return (`handler.cpp:121–127, 1606–1608, 1693–1694`)
- *モード / 特権*: Thread mode、特権
- `handler_entry_point(5, 0, 0, 0)` が `kernel_object_instance.handle(5, 0, 0, 0)` を呼び出す。
- `case object_api::GET_CURRENT_OBJECT:` により `value = current_object(...)` を取得。
- `return value;` (通常の C return)。
- `handler_entry_point` のエピローグで `bx lr`。`LR` に仕込まれていた `shizuku_armv8m_return_stub` へジャンプする！
- ハンドラ内の C スタックは巻き戻り、`PSP` は再び $P_0 - 168$ を指す。

==== [時点 7] `return_stub` による SVC #2 発行 (`armv8m_ctx.S:142–154`)
- *モード / 特権*: Thread mode、特権
- レジスタ準備:
  - `r0`: 2 (`primitive::RETURN`)
  - `r1`: 1 (落とす段数: `count=1`)
  - `r2`: `value` (戻り値)
  - `r3`: 0 (`error=0`)
  - `r12`: 0
- `svc 0` を発行！
- ハードウェアが PSP ($P_0 - 168$) に HW Frame B (32B) を push。$P_2 = (P_0 - 168) - 32 = P_0 - 200$。
- Handler mode へ突入 (`LR = 0xFFFFFFFD`, MSP = $M_0$)。

==== [時点 8] SVC #2 の `shizuku_svc_dispatch` による `call_frame_pop` (`dispatch.cpp:144–160, 232–260`)
- *モード / 特権*: Handler mode、特権
- `CTX_SAVE` を経て MSP は $M_0$。
- 発行元スレッドは `object_kind::KERNEL_OBJECT` 本人であるため、`primitive::RETURN` の分岐 (`dispatch.cpp:232–260`) へ入る。
- `call_frame_pop`:
  - `header = (call_frame_header *)thread.call_stack.top` ($P_0 - 168$)
  - `*context = header->saved`: *Caller の 120B `context_t` を完全復元！*
  - この復元により、`context->sp` は元の HW Frame A ($P_1 = P_0 - 32$) を再び指す！
  - `thread.call_stack.top = header->prev` (=0)、`depth = 0`。
- 結果書き込み (`ARCH::set_result(*frame, error, value)`):
  - 復元された HW Frame A の `r0` に 0 (`kernel_error::OK`)、`r1` に `value` を書き込む。
- MPU: Caller ドメインへ復元

==== [時点 9] `CTX_RESTORE 1` と例外復帰 #2 直後 (`armv8m_ctx.S:58–107`)
- *モード / 特権*: Thread mode、*非特権* (`CONTROL.nPRIV=1`, `CONTROL.SPSEL=1`)
- `context_t` から Caller の元のレジスタ (`r4`--`r11`, `CONTROL.nPRIV=1`, `PSPLIM`=$L_"caller"$) を復元。
- `bx lr` によりハードウェアが HW Frame A (32B) を unstack！
- *復帰後の状態*:
  - `PSP`: $P_0$ (Caller のスタックトップに完全復帰)
  - `MSP`: $M_0$
  - `PC`: Caller の SVC 0 の次の命令 (`armv8m.hpp:418`)
  - `r0`: 0 (成功)
  - `r1`: `value` (現在のオブジェクト ID)
  - `r4`--`r11`: Caller の元の値

=== 発生するオーバーヘッドと実測値 (1985 cyc の正体)

V2 の `GET_CURRENT_OBJECT` の実測値は *untraced net min 1985 cyc* (空枠控除前 raw min 2000 cyc) である (`perf/svc-in-ram:docs/record/kernel_v2_record.typ:50–70`, `perf/svc-in-ram:docs/record/measurements/cost3_report.md:45–57, 81–83` [実測])。
この 1985 cyc の背景には以下の要因が存在する:
1. *2 回の境界越え*: 各境界で `CTX_SAVE` (約131.2 cyc)、dispatch (約65.6 cyc)、MPU wrapper (約128.6 cyc)、`CTX_RESTORE` (約120.4 cyc) を含む。これらの区間平均の合算として約 446 traced cyc/境界が観測される (446 cyc は区間平均バケットの合算値であり、排他的な確定測定値ではない)。
2. *1 回の `call_frame_push`*: 120B `context_t` 複写、例外フレーム複写、幾何検査 (区間平均 432.3 traced cyc)。
3. *Thread mode での kobj トランポリン実行*: `handler_begin`, `handle`, `current_object`, `return_stub`。
4. *1 回の `call_frame_pop`*: 120B `context_t` 読み戻し。

本来ならレジスタ 1 本を読んで返すだけの操作に、*特権 Thread モードへのコンテキスト積み替えと復帰を往復させたこと* が、2000 サイクル近くを費やす主因であった。

---

== V2 の Method Call 1 段 (CALL_METHOD) の詳細追跡

=== 5 回の SVC の大局観

V2 において、あるオブジェクトが別のオブジェクトのメソッドを呼ぶ `CALL_METHOD` (API ID=4) は、成功時に *SVC 5 回、例外復帰 5 回、push 3 回、pop 3 frame* を発生させる (`perf/svc-in-ram:docs/record/measurements/cost3_report.md:45–57` [実測])。

登場する実行主体:
1. *Caller*: メソッドを呼び出す側 (Thread mode, 非特権)
2. *カーネル*: 機構を提供する Handler mode
3. *親 kobj*: 方針・台帳を管理する特権ハンドラ (Thread mode, 特権)
4. *Leaf*: 呼び出される末端メソッド (Thread mode, 特権または非特権)

#table(columns: (0.6fr, 1.3fr, 1.8fr, 1.6fr, 0.9fr), inset: 4pt,
  table.header(repeat: true, [SVC], [発行元 (Thread)], [カーネルの処理 (Handler)], [例外復帰先 (Thread)], [フレーム操作]),
  [#1], [Caller: `CALL_METHOD`], [`dispatch.cpp:292–341` 親 kobj へ転送], [kobj `call_method` (`handler.cpp:532–581`)], [*push A* (dispatch frame)],
  [#2], [kobj: `primitive::CALL`], [`dispatch.cpp:214–230` \ `dispatch.cpp:162–192` `do_call`], [Leaf 本体 (`LR=return_stub`)], [*push B* (call frame)],
  [#3], [Leaf 戻り: `return_stub`], [`dispatch.cpp:292–341` 親 kobj へ転送], [kobj `exit_method` (`handler.cpp:584–617`)], [*push C* (dispatch frame)],
  [#4], [kobj: `primitive::RETURN` (count=2)], [`dispatch.cpp:232–260` C と B を pop], [kobj `call_method` 継続部 (`handler.cpp:572`)], [*pop C + B* (2 frames)],
  [#5], [kobj 戻り: `return_stub`], [`dispatch.cpp:232–260` A を pop], [元 Caller (`armv8m.hpp:418`)], [*pop A* (1 frame)],
)

=== 5 つの境界のステップバイステップ追跡

*前提条件 (代表ケース)*:
- Caller 突入時の $P_0$ は 8B 整列。
- 基本例外フレーム 32B (`exc_frame_bytes = 32`)。
- FPU 非活性 (`CONTROL.FPCA = 0`)。
- 割り込み・遅延切替の割り込みなし。
- Leaf は非特権オブジェクト (`PROTECTION_UNPRIVILEGED`) として呼び出されると仮定 (特権 Leaf の場合は CONTROL.nPRIV が 0 のまま維持される)。

==== [境界 1] Caller → 親 kobj (push A)
- *発行元 (Thread mode, 非特権)*:
  Caller が `CALL_METHOD` を SVC 発行 (`armv8m.hpp:406–419`):
  - レジスタ (固定 ABI 形式): `r0 = 4` (`CALL_METHOD`), `r1 = target_object_id`, `r2 = method_id`, `r3 = arg`, `r12 = 0`.
  - スタック: PSP = $P_0$, MSP = $M_0$, `PSPLIM` = $L_"caller"$, `CONTROL` = `CONTROL_UNPRIV_PSP` (`nPRIV=1`, `SPSEL=1`).
  - MPU: Caller ドメイン。
- *SVC #1 発行直後 (ハードウェア)*:
  PSP に 32B HW Frame A が push され、PSP = $P_0 - 32$。Handler mode へ突入 (`LR = 0xFFFFFFFD`, MSP = $M_0$)。
- *Handler mode (カーネル)*:
  - `CTX_SAVE` (`armv8m_ctx.S:25–52`): 一時的に MSP=$M_0-4$ となるが直後に pop され MSP は $M_0$ に復帰。Caller の 120B `context_t` (元の `sp` = $P_0 - 32$, `r4`--`r11`, `CONTROL`, `PSPLIM`) を退避。
  - ルーティング: Caller は `object_kind::PLAIN` のため `dispatch.cpp:292–341` により登録済み親 kobj (root 構成時 ID 0) へ転送。
  - `call_frame_push` (`dispatch.cpp:68–142`):
    - `total = round_up(132 + 32, 8) = 168B`
    - `snapshot_A = P_0 - 168`, `callee_frame_A = P_0 - 200` (変位 200B)。
    - $P_0 - 36$ .. $P_0 - 32$ の 4B はソフトウェア round-up 空隙 (ハードウェア SPREALIGN ではない)。
    - `header_A->saved = *context` (Caller の 120B `context_t` を退避)。
    - `callee_frame_A` の設定: `PC = handler_entry_point` (`handler.cpp:121–127`), `LR = return_stub` (`armv8m_ctx.S:142–154`), `r0=4`, `r1=target_object_id`, `r2=method_id`, `r3=arg`.
    - `context->control = CONTROL_PRIV_PSP` (`nPRIV=0`), `context->sp = P_0 - 200`.
    - 特権台帳 `ledger[0]` (`dispatch.cpp:318–322`) には、呼び出し先である登録済み親 kobj の `target_object`、`kind` (to_root なら `KERNEL_OBJECT`、専用ハンドラなら `HANDLER`)、`via = LEDGER_DISPATCH` を記録。Caller (呼び出し元) の情報は `thread.base_object` / `base_kind` および `header_A->saved` に温存される。`call_stack.top = snapshot_A` ($P_0 - 168$), `depth = 1`.
- *例外復帰 #1*:
  `CTX_RESTORE` (`armv8m_ctx.S:58–107`) 経由で `bx lr`。`callee_frame_A` (32B) が unstack され、復帰先は kobj の `handler_entry_point`。
- *復帰後の状態 (Thread mode, 特権)*:
  - PSP = $P_0 - 168$ (`snapshot_A`), MSP = $M_0$, `CONTROL.nPRIV=0`, `CONTROL.SPSEL=1`.
  - LR = `shizuku_armv8m_return_stub`, PC = `handler_entry_point` → `handle()` → `call_method()` (`handler.cpp:532–581`).

==== [境界 2] 親 kobj → Leaf (push B)
- *発行元 (Thread mode, 特権)*:
  kobj の `call_method()` が対象オブジェクト・メソッド・深さを検査し、`call_request` を構築して `ARCH::syscall(primitive::CALL, &request)` を発行 (`handler.cpp:570–571`)。
  - レジスタ: `r0 = 1` (`primitive::CALL`), `r1 = &request` (kobj スタック上のポインタ)。
  - スタック: PSP = $P_0 - 168$, MSP = $M_0$, `CONTROL.nPRIV=0`.
- *SVC #2 発行直後 (ハードウェア)*:
  PSP に 32B HW Frame B が push され、PSP = $(P_0 - 168) - 32 = P_0 - 200$。Handler mode へ突入 (`LR = 0xFFFFFFFD`, MSP = $M_0$)。
- *Handler mode (カーネル)*:
  - `CTX_SAVE`: kobj の 120B `context_t` (元の `sp` = $P_0 - 200$, 特権 CONTROL, PSPLIM) を退避。MSP は $M_0$。
  - ルーティング: 発行元は KERNEL_OBJECT 本人のため `dispatch.cpp:214–230` の `primitive::CALL` 分岐 → `dispatch.cpp:162–192` (`do_call`)。
  - `call_frame_push` (`dispatch.cpp:68–142`):
    - `caller_stack = (P_0 - 200) + 32 = P_0 - 168`
    - `total = 168B`
    - `snapshot_B = (P_0 - 168) - 168 = P_0 - 336`, `callee_frame_B = P_0 - 368` (変位 200B、累計 368B)。
    - $P_0 - 204$ .. $P_0 - 200$ の 4B はソフトウェア round-up 空隙。
    - `header_B->saved = *context` (kobj の 120B `context_t` を退避)。
    - `callee_frame_B` の設定: `PC = leaf_entry` (`cost_leaf`), `LR = return_stub`, `r0..r3, r12` = 実引数。
    - 権限・MPU の設定: `call_request.protection` に従い、非特権指定なら `context->control = CONTROL_UNPRIV_PSP` (`nPRIV=1`), Region 2 窓を設定。特権指定なら `nPRIV=0`。
    - 特権台帳 `ledger[1]` に Callee の object, kind, via=`LEDGER_CALL` を記録。`call_stack.top = snapshot_B` ($P_0 - 336$), `depth = 2`.
- *例外復帰 #2*:
  `CTX_RESTORE` 経由で `bx lr`。`callee_frame_B` (32B) が unstack され、復帰先は Leaf 関数本体 (`cost_leaf`)。
- *復帰後の状態 (Thread mode, Callee 権限)*:
  - PSP = $P_0 - 336$ (`snapshot_B`), MSP = $M_0$, `CONTROL.nPRIV=1` (非特権時), `CONTROL.SPSEL=1`.
  - LR = `shizuku_armv8m_return_stub`, PC = Leaf 関数先頭。

==== [境界 3] Leaf 戻り → 親 kobj EXIT_METHOD (push C)
- *発行元 (Thread mode, Callee 権限)*:
  Leaf が処理を終え通常の C return を実行すると、`LR` (`shizuku_armv8m_return_stub`, `armv8m_ctx.S:142–154`) に落ちる。
  - `return_stub` がレジスタを設定:
    `r0 = 2` (`primitive::RETURN`), `r1 = 1` (`count=1`), `r2 = value` (Leaf 戻り値), `r3 = 0` (`error=0`), `r12 = 0`.
  - `svc 0` を発行。
  - スタック: PSP = $P_0 - 336$, MSP = $M_0$.
- *SVC #3 発行直後 (ハードウェア)*:
  PSP に 32B HW Frame C が push され、PSP = $(P_0 - 336) - 32 = P_0 - 368$。Handler mode へ突入 (`LR = 0xFFFFFFFD`, MSP = $M_0$)。
- *Handler mode (カーネル)*:
  - `CTX_SAVE`: Leaf の 120B `context_t` (元の `sp` = $P_0 - 368$) を退避。MSP は $M_0$。
  - ルーティング: Leaf は `object_kind::PLAIN` (非 KERNEL_OBJECT) であるため、`primitive::RETURN` としては解釈されず `dispatch.cpp:292–341` の親ハンドラ転送へ送られる。
    `r0 = 2` は `object_api::EXIT_METHOD` の番号と一致するため、親 kobj の `exit_method` 呼び出しとなる。
  - `call_frame_push` (`dispatch.cpp:68–142`):
    - `caller_stack = (P_0 - 368) + 32 = P_0 - 336`
    - `total = 168B`
    - `snapshot_C = (P_0 - 336) - 168 = P_0 - 504`, `callee_frame_C = P_0 - 536` (変位 200B、累計 536B)。
    - $P_0 - 372$ .. $P_0 - 368$ の 4B はソフトウェア round-up 空隙。
    - `header_C->saved = *context` (Leaf の 120B `context_t` を退避)。
    - `callee_frame_C` の設定: `PC = handler_entry_point`, `LR = return_stub`, `r0=2` (`EXIT_METHOD`), `r1=1`, `r2=value`, `r3=0`.
    - `context->control = CONTROL_PRIV_PSP` (`nPRIV=0`), 特権台帳 `ledger[2]` に記録。`call_stack.top = snapshot_C` ($P_0 - 504$), `depth = 3`.
- *例外復帰 #3*:
  `CTX_RESTORE` 経由で `bx lr`。`callee_frame_C` (32B) が unstack され、復帰先は kobj の `exit_method` (`handler.cpp:584–617`)。
- *復帰後の状態 (Thread mode, 特権)*:
  - PSP = $P_0 - 504$ (`snapshot_C`), MSP = $M_0$, `CONTROL.nPRIV=0`, `CONTROL.SPSEL=1`.
  - LR = `shizuku_armv8m_return_stub`, PC = `exit_method`。

==== [境界 4] 親 kobj EXIT_METHOD → kobj call_method 継続部 (pop C + B)
- *発行元 (Thread mode, 特権)*:
  kobj の `exit_method` (`handler.cpp:584–617`) が台帳から落とすべき段数 `count = 2` (Leaf の Frame B と自身の Frame C の計 2 枚) を算出。
  - `ARCH::syscall(primitive::RETURN, count=2, value, error, depth)` を発行 (`handler.cpp:613`)。
  - レジスタ: `r0 = 2` (`primitive::RETURN`), `r1 = 2` (`count=2`), `r2 = value`, `r3 = error(=0)`, `r12 = depth(=3)` (claim 検査用)。
  - スタック: PSP = $P_0 - 504$, MSP = $M_0$.
- *SVC #4 発行直後 (ハードウェア)*:
  PSP に 32B HW Frame が push され、PSP = $(P_0 - 504) - 32 = P_0 - 536$。Handler mode へ突入 (`LR = 0xFFFFFFFD`, MSP = $M_0$)。
- *Handler mode (カーネル)*:
  - `CTX_SAVE`: MSP は $M_0$。発行元は KERNEL_OBJECT 本人のため `dispatch.cpp:232–260` の `primitive::RETURN` 分岐へ入る。
  - 連続 pop 処理: `depth=3`, `claim=3` (一致確認), `count=2` のため、`call_frame_pop` (`dispatch.cpp:144–160`) を 2 回連続実行:
    - 1 回目 pop: Frame C を pop。`thread.call_stack.top` = $P_0 - 336$ (`snapshot_B`), `depth = 2`.
    - 2 回目 pop: Frame B を pop。`thread.call_stack.top` = $P_0 - 168$ (`snapshot_A`), `depth = 1`.
    - 2 回目の pop により、`*context = header_B->saved` が実行され、kobj が SVC #2 を発行した直後の文脈 (`sp = P_0 - 200`, 特権 CONTROL, PSPLIM) が完全復元される！
  - 結果書き込み: 復元された Frame B の HW frame ($P_0 - 200$) の `r0` に `error=0`, `r1` に `value` を書き込む。
- *例外復帰 #4*:
  `CTX_RESTORE` 経由で `bx lr`。復帰対象の HW frame ($P_0 - 200$, 32B) が unstack され、復帰先は kobj の `call_method()` 内 `ARCH::syscall` の直後 (`handler.cpp:572`)。
- *復帰後の状態 (Thread mode, 特権)*:
  - PSP = $P_0 - 168$ (`snapshot_A`), MSP = $M_0$, `CONTROL.nPRIV=0`, `CONTROL.SPSEL=1`.
  - PC = `call_method` 継続部。Leaf の戻り値を受け取って通常の C return を行い、エピローグで `bx lr` (境界 1 で仕込まれていた `return_stub` へジャンプ)。

==== [境界 5] 親 kobj 戻り → 元 Caller (pop A)
- *発行元 (Thread mode, 特権)*:
  `return_stub` (`armv8m_ctx.S:142–154`) が `r0=2` (`primitive::RETURN`), `r1=1` (`count=1`), `r2=value`, `r3=0`, `r12=0` をセットして `svc 0` を発行。
  - スタック: PSP = $P_0 - 168$, MSP = $M_0$.
- *SVC #5 発行直後 (ハードウェア)*:
  PSP に 32B HW Frame が push され、PSP = $(P_0 - 168) - 32 = P_0 - 200$。Handler mode へ突入 (`LR = 0xFFFFFFFD`, MSP = $M_0$)。
- *Handler mode (カーネル)*:
  - `CTX_SAVE`: MSP は $M_0$。発行元は KERNEL_OBJECT 本人。
  - `dispatch.cpp:232–260` (`primitive::RETURN`, `count=1`):
    - `call_frame_pop` を 1 回実行: Frame A を pop。
    - `*context = header_A->saved`: Caller が SVC #1 を発行した直後の文脈 (`sp = P_0 - 32`, 非特権 `CONTROL`, $L_"caller"$) を完全復元！
    - `thread.call_stack.top = 0`, `depth = 0`.
  - 結果書き込み: 復元された Frame A の HW frame ($P_0 - 32$) の `r0` に 0, `r1` に `value` を書き込む。
- *例外復帰 #5*:
  `CTX_RESTORE` により MPU Region 2 を Caller ドメインへ復元、`CONTROL` を非特権 (`nPRIV=1`) へ復元。
  `bx lr` で HW Frame A ($P_0 - 32$, 32B) を unstack。
- *復帰後の状態 (Thread mode, 非特権)*:
  - PSP = $P_0$ (Caller のスタックトップに完全復帰), MSP = $M_0$.
  - `CONTROL.nPRIV=1`, `CONTROL.SPSEL=1`, `PSPLIM` = $L_"caller"$.
  - PC = Caller の SVC #1 直後 (`armv8m.hpp:418`), `r0=0`, `r1=value`.

=== 120B context_t 複写の位置

この 1 段の Method Call の間に、PSP スタック上には 3 つのフレームが順次 push される。
各 push において、`call_frame_push` (`dispatch.cpp:68–142`) は以下のコピーを行う:
1. `header->saved = *context`: 120 バイトの `context_t` 全体を構造体代入 (`dispatch.cpp:119`)。
2. `memcpy(callee_frame, context->sp, frame_bytes)`: 32 バイトの例外フレームを複製 (`dispatch.cpp:122`)。

3 回の push (Frame A, Frame B, Frame C) で、*120B コンテキストのメモリコピーが計 3 回 (360B)* 行われ、3 回の pop でそれが逆順に読み戻される。

#table(columns: (1fr, 1.8fr, 3fr), inset: 4pt,
  table.header([フレーム], [push のタイミング], [複写内容と目的]),
  [Frame A], [SVC #1 (Caller → kobj)], [Caller の 120B context_t (および 32B HW frame) を退避。kobj 実行中の破壊から保護。],
  [Frame B], [SVC #2 (kobj → Leaf)], [kobj の 120B context_t (および 32B HW frame) を退避。Leaf 実行中の破壊から保護。],
  [Frame C], [SVC #3 (Leaf → kobj EXIT)], [Leaf の 120B context_t (および 32B HW frame) を退避。EXIT_METHOD 実行用の枠。],
)

=== cost3 実測サイクル数と区間分析

`perf/svc-in-ram:docs/record/measurements/cost3_report.md` における 150 MHz RP2350 での実機計測結果 (`cost2_trace.log` [実測]) は、この構造的ボトルネックを完全に数値化している。
ここで留意すべきは、*cost3 の各区間数値は「直前 event → 今回 event」の到着タイムスタンプ差の加算バケットであり、関数の排他的・網羅的な実行コスト (inclusive cost) ではない* という点である (`cost3_report.md:12–41`)。各区間にはフック自身の処理時間や、稀に発生する割り込み (IRQ) の影響が含まれる。

#table(columns: (1.2fr, 0.8fr, 0.6fr, 0.8fr, 3fr), inset: 4pt,
  table.header(repeat: true, [到着区間], [合計 cyc], [hits], [平均 cyc], [実コード対応と内容 (`cost3_report.md:24–37`)]),
  [`mpu_begin`], [1448], [5], [289.6], [push_end → dispatch 後半 (3回) または dispatch → RETURN pop (2回)。pending-step CAS を含む。MPU store 前。],
  [`push_end`], [1297], [3], [432.3], [`dispatch.cpp:68–142`。サイズ検査、header、*120B context 複写*、例外 frame 複写、幾何検算。],
  [`exception_entry`], [1029], [5], [205.8], [hardware entry だけでなく直前 Thread handler (validation、ledger scan、request 作成) の仕事を含む。],
  [`context_saved`], [656], [5], [131.2], [`armv8m_ctx.S:25–52`。`CTX_SAVE`、`current_context` 取得、整数・FP callee-saved 退避。],
  [`mpu_end`], [643], [5], [128.6], [`arch_glue.cpp:52–100`。lookup、profile 判定、Region 2 計算、2/3 store、hook。],
  [`restore_end`], [602], [5], [120.4], [`armv8m_ctx.S:58–107`。コンテキスト復元、PSPLIM、CONTROL、ISB。`bx lr` は含まない。],
  [`push_begin`], [396], [3], [132.0], [`dispatch.cpp:203–218`。分岐、request 複写・検査。],
  [`dispatch`], [328], [5], [65.6], [`armv8m_ctx.S:118`。glue と dispatch 入口。],
  [`handler_begin`], [164], [2], [82.0], [hardware unstack 後の handler trampoline prologue/hook。],
  [`return_stub`], [141], [2], [70.5], [`armv8m_ctx.S:142–154`。Leaf 処理・return を含む。],
  [`handler_end`], [106], [1], [106.0], [primitive RETURN 後の `call_method` 後半。],
  [`caller_resume`], [81], [1], [81.0], [最終 restore_end → unstack → Caller 復帰。],
)

重要な実測の結論:
1. *1 境界あたりの基本オーバーヘッド*: `context_saved` (131.2) + `dispatch` (65.6) + `mpu_end` (128.6) + `restore_end` (120.4) = *約 446 traced cyc/境界*。5 回の境界越えだけで $446 times 5 approx 2230$ cyc を占める (446 cyc は区間平均バケットの合算値)。
2. *push の重さ*: `push_end` は 1 hit あたり *432.3 cyc*。120B のメモリコピーとスタック幾何検算が大きな負荷となっている。
3. *MPU 書込みは主因ではない*: MPU body (MMIO への実際の書込み) は 5 回合計で 80 cyc、すなわち *16 cyc/更新* に過ぎない (`cost3_report.md:72`)。MPU 書込みをスキップする実験を行っても、untraced call1 は 5774 → 5830 cyc とむしろ悪化した (`cost3_report.md:73`)。

結論として、V2 のボトルネックは「MPU 設定の重さ」ではなく、*「中継による 5 回の境界越え」と「3 回の 120B context 複写」* であった。

---

== V3 における SVC と Method Call の再設計

=== V3 の基本哲学

V3 設計書 (`docs/design/kernel_v3.typ` [設計]) は、V2 の計測結果を踏まえ、ハンドラ構造を根本から再構築する。
- *カーネルとカーネルオブジェクトの統合*: カーネルと kobj を一体のカーネルとして扱い、Thread mode のカーネル処理はその世代のカーネル actor ID を持つ (決定 1--11)。
- *二段構えの全廃*: Caller → kobj → primitive CALL という二段構えを廃止する。
- *静的属性による経路分離*: システムコールの実行モードを、実行時の動的探索ではなく、API ID ごとの静的 traits で決定する。

=== V3 の同期 SVC: Handler mode 完結経路 (S0)

`GET_CURRENT_OBJECT` や短い stream batch、CAS などの同期システムコールは、Thread mode のカーネルハンドラを起こさず、*Handler mode の同一例外内で完結して即座に呼び出し元へ復帰する* (S0 経路)。

#table(columns: (1fr, 1.8fr, 1.8fr), inset: 4pt,
  table.header(repeat: true, [項目], [V2 の実装], [V3 の設計 (S0)]),
  [境界越え回数], [2 回 (SVC 2回、例外復帰 2回)], [*1 回* (SVC 1回、例外復帰 1回)],
  [スタック操作], [push 1 回、pop 1 回 (120B copy)], [*push / pop ゼロ* (スタック変更なし)],
  [レジスタ保存], [Caller の全 context 退避 (`r4`--`r11`, PSP, FP)], [*最小限の C ABI 保存のみ* (MSP 上)],
  [`CONTROL` / `PSPLIM`], [特権への切替えと復元], [*変更なし* (Caller の状態を維持)],
  [MPU 窓], [再計算・2 回の更新], [*更新なし* (同一保護領域を維持)],
  [復帰方法], [kobj が `return_stub` から再 SVC], [*Handler mode から直接 `bx lr`*],
)

==== S0 のステップバイステップ推移
1. *Caller (Thread mode, 非特権)*:
   - `r0=5` (`GET_CURRENT_OBJECT`), `r1`--`r3`=0, `r12`=0 で `svc 0` を発行。
   - ハードウェアが PSP に 8 語 (HW Frame: 32B) を push ($P_1 = P_0 - 32$)。
2. *Handler mode 入口*:
   - モード: Handler mode (特権)。動作スタック: MSP ($M_0$)。
   - `LR` (`EXC_RETURN`) を保持。
   - PSP から HW Frame のアドレスを取得。
   - `r4`--`r11` をスレッドの `context_t` に丸ごと退避することは *しない* (C 関数の prologue が MSP 上に退避する通常の AAPCS 保存のみ)。
3. *ハンドラ本体の実行*:
   - 静的ディスパッチャが `handler_service<GET_CURRENT_OBJECT>` を直接呼び出す。
   - 保存された origin 情報と特権台帳から Caller の actor ID を読み出し、HW Frame の `r0` (エラー=0) と `r1` (オブジェクト ID) に直接書き込む。
4. *例外復帰*:
   - PSP、`CONTROL`、`PSPLIM`、MPU の書き戻しは行わない (同一条件のため)。
   - 保持していた `EXC_RETURN` で直ちに `bx lr` を実行！
5. *Caller (Thread mode, 非特権)*:
   - ハードウェアが HW Frame を unstack して PSP は $P_0$ に復帰。
   - 呼び出し直後の命令へ 1 回の往復で復帰。

*[設計目標]*: net min 1985 cyc → *500 cyc 以下* (`kernel_v3.typ:600` [設計目標])。
*[Handler mode の制約]*: Handler mode で完結させるため、最悪実行時間 $H_"max"$ を 10 µs 以下、割り込み禁止区間を 2 µs 以下に抑える (`kernel_v3.typ:235` [設計])。ブロッキング、yield、動的メモリ確保、再帰的 SVC は厳格に禁止される。

=== V3 の Method Call: 開始 1 回・終了 1 回 (S1/S2)

V3 における `CALL_METHOD` は、中継アクターを挟まず、*開始 1 回・終了 1 回の計 2 回の境界越え (SVC/例外復帰の組)* に統合される。

#table(columns: (1fr, 1.8fr, 1.8fr), inset: 4pt,
  table.header(repeat: true, [項目], [V2 の実装], [V3 の設計 (S1/S2)]),
  [境界越え回数], [5 回 (SVC 5回、例外復帰 5回)], [*2 回* (開始 1回、終了 1回)],
  [スタックフレーム], [push 3 回、pop 3 回 (物理 3 枚)], [*logical method frame 1 枚* (台帳上)],
  [120B context 複写], [3 回 memcpy (計 360B)], [*廃止* (immutable descriptor 参照化)],
  [中継 actor], [親 kobj が Thread mode で中継], [*全廃* (kernel landing が直接 tail transfer)],
  [復帰プロトコル], [Leaf return_stub → EXIT → RETURN], [*context reuse 専用復元経路* (`reused_context`)],
)

==== [開始] CALL 開始のステップバイステップ推移
1. *Caller (Thread mode, 非特権)*: `CALL_METHOD` を SVC 発行 (SVC #1)。
   - ハードウェアが PSP に HW Frame (32B) を push。
2. *Handler mode 入口 (S1)*:
   - モード: Handler mode (特権)。動作スタック: MSP ($M_0$)。
   - Caller の `r4`--`r11`、PSP、`EXC_RETURN` を Caller の continuation に 1 回保存。
   - 静的ディスパッチャが `prepare_kernel_landing` を呼び出す。
   - *例外復帰先を特権 Thread mode の kernel landing に設定*。
3. *例外復帰 → Kernel Landing (Thread mode, 特権)*:
   - `bx lr` により特権 Thread mode の kernel landing へ遷移。
   - kernel landing は専用の kernel stack segment で動作。
   - 対象オブジェクト/メソッドの immutable descriptor を pin し、権限・最大深さを検査。
   - 特権台帳に logical method frame を 1 枚 commit。
   - *Tail Transfer Gate*:
     - Callee の stack segment、メソッド引数 (`r0`--`r3`, `r12`)、`LR = return_stub`、MPU 保護属性を適用。
     - Callee へ直接ジャンプ (tail branch)！

==== [終了] CALL 終了のステップバイステップ推移
1. *Leaf (Thread mode)*: 処理を終えて C return すると、共通 `return_stub` から `EXIT_METHOD` を SVC 発行 (SVC #2)。
   - ハードウェアが PSP に HW Frame (32B) を push。
2. *Handler mode 入口 (S2)*:
   - モード: Handler mode (特権)。動作スタック: MSP ($M_0$)。
   - 発行 actor、method frame 世代、depth を特権台帳で検算。
   - *context reuse 専用の復元経路 (`resume_reused_context`)*:
     - 開始時に保持された Caller の continuation から、元 PSP、callee-saved (`r4`--`r11`)、元 HW frame、`CONTROL`、`PSPLIM`、MPU descriptor を復元。
     - 元 Caller の HW frame の `r0`/`r1` に戻り値を設定。
3. *例外復帰*:
   - Caller の元 HW frame に対して Handler mode から直接例外復帰 (`bx lr`)！
4. *Caller (Thread mode, 非特権)*:
   - SVC #1 の直後へ復帰。

==== なぜ「普通の C return による例外復帰」を禁止するのか？
設計書で強調される重要なハードウェア制約 (`kernel_v3.typ:264–278` 決定 3 [設計]):
*Cortex-M の Thread mode は、単に PSP や特権レジスタを準備しただけでは `EXC_RETURN` に基づく例外復帰として動作しない*。
したがって、Thread mode のカーネルが普通の C return で例外復帰することは不可能であり、*「context reuse で入った呼出しは、context reuse 専用の復元経路 (Handler mode の復帰ゲートまたは専用トラップ) で戻す」* 規約が不可欠となる。
また、トラップ境界回数の目標 (1 段 call で 2 回) は、*この context reuse 専用復元が追加のトラップを要求しないこと* を前提条件としている。Thread mode 間での actor/保護ドメイン遷移 (Caller → Kernel → Callee → Kernel → Caller) は最大 4 回発生し得るため、トラップ境界回数とドメイン遷移回数は峻別して数える必要がある (`kernel_v3.typ:334–347`)。

==== 120B context_t 複写の廃止
V2 では push/pop ごとに 120B の `context_t` 全体をスタック上に memcpy していたが、V3 では保護領域やスタック境界を *immutable descriptor への参照* とする。これにより、保存が必要なデータは 32B の callee-saved 整数レジスタと数語のメタデータのみに圧縮される (`kernel_v3.typ:321–325` [設計])。

*[設計目標]*: call1 net min 5774 cyc → *3000 cyc 以下*、call2 11642 cyc → *6000 cyc 以下* (`kernel_v3.typ:601` [設計目標])。

=== 設計書における確定事項・未確定事項・棚上げ事項の境界

V3 設計書を正確に読むために、何が決定済みで何が未確定かを峻別する。

#table(columns: (1fr, 1.2fr, 2.8fr), inset: 4pt,
  table.header(repeat: true, [区分], [項目], [設計書上の扱い・根拠]),
  [*確定* \ (決定 1--4, 6--7, 9--11)], [Handler mode 完結], [GET, stream 一部は H/S0 完結。IRQ 予算は 10 µs / mask 2 µs (`kernel_v3.typ:235`)。],
  [*確定*], [専用戻り経路], [context reuse で入った呼出しは専用の復元経路で戻す (決定 3, `kernel_v3.typ:264–278`)。],
  [*確定*], [レジスタ保存契約], [現行の保存契約 (r4--r11, flags, scratch) は維持する (決定 4, `kernel_v3.typ:280–283`)。],
  [*確定*], [Atomic 範囲], [1 ID + op。カーネル Handler mode は CAS と load/store のみ。FETCH_ADD やキュー等は objectland で処理 (決定 6, `kernel_v3.typ:459–464`)。],
  [*確定*], [Wait queue 不採用], [専用 wait/wake キューは作らない。mutex は CAS と宛先 YIELD、値待ちは再確認 (決定 9, `kernel_v3.typ:484–500`)。],
  [*確定*], [lane の両立], [native は静的 traits、objectland は raw SVC の親ハンドラインターセプトを完全維持 (決定 2, `kernel_v3.typ:349–371`)。],
  [*未確定* \ (決定 8)], [宛先 YIELD の世代], [スレッド枠の再利用 (世代) の扱い。上層案は「YIELD はヒント扱い、期待世代は ABI に入れない」だが、決定ではなく未確定として扱う (`kernel_v3.typ:484–488, 677`)。],
  [*未確定*], [Atomic API 番号], [番号割当てと引数 layout は `//abi` 正本に合わせて別途確定。現行コードの `object_api` には未定義 (`kernel_v3.typ:49–51, 459–465`)。],
  [*棚上げ* \ (決定 5)], [動的ホットスワップ], [呼出し完了待ちが他システムへ波及するため棚上げ。actor ID を定数に焼かない制約のみ残す (`kernel_v3.typ:386–388, 674`)。],
)

---

== 静的属性表と template dispatcher

=== 5 つの静的属性

現行の V3 下ごしらえコードにおいて、システムコールの振る舞いは静的属性構造体 `svc_attribute` によって定義される (`internal_headers/shizuku/svc_attributes.hpp:11–17` [現行コード/下ごしらえ]):
1. `permission`: `unprivileged_allowed` (非特権可) または `privileged_only` (特権のみ)。
2. `from_isr`: ISR (割り込みハンドラ) から直接呼出し可能か。
3. `mode`: `handler` (Handler mode 内完結) または `thread` (Thread mode の kernel actor)。
4. `save_level`: `minimal` (S0: 最小入口) または `continuation` (S1/S2: コンテキスト保存)。
5. `authority`: `none`、`create_flags`、`parent_ledger` などの特殊権限述語。

*重要な注意点*:
- 構造体のフィールドは上記 *5 つのみ* であり、実行時間上限を表す `work_bound` フィールドは構造体内には実在しない ($H_"max" <= 10$ µs は設計上の不変条件・契約である)。
- `svc_attributes.hpp:18–19` の注記にある通り、「V2 は `permission` と `authority` のみを消費し、`mode` / `save_level` / `from_isr` は V3 を記述するものであって、V2 の raw 親ハンドラ中継経路を変更してはならない」。

現行コードにおける API の分類 (`svc_attributes.hpp:20–36` [現行コード/下ごしらえ]):
- *Handler mode 完結 (`mode = handler`, `save_level = minimal`)*: `GET_CURRENT_OBJECT` (5), `GET_CALLER_OBJECT` (6), `MEMORY_OWNER` (16), `OBJECT_NAME` (18), `STREAM_OPEN` (20), `STREAM_PUSH_N` (29), `STREAM_POP_N` (30), `STREAM_AVAILABLE` (31)。
- *ISR から呼出し可能 (`from_isr = true`)*: 18, 20, 29, 30, 31。
- *特権限定 (`privileged_only`)*: `GRANT_REGION` (23), `KILL_THREAD` (25)。
- *特殊権限述語*: `CREATE_OBJECT` (1: `create_flags`), `FORWARD_CHILD_EXIT` (27: `parent_ledger`)。

なお、V3 設計で Handler mode かつ ISR 呼出し可能として提案されている `ATOMIC` サービスは、現行ヘッダ (`object_api.hpp`, `svc_attributes.hpp`) には未定義であり、数値 ID および引数配置は `//abi` 正本 (`abi/include/shizuku/object_ids.hpp`) の確定待ちである (`kernel_v3.typ:49–51, 459–464` [未確定])。

=== template dispatcher の仕組み

ディスパッチャは、C++ の `if constexpr` を用いたテンプレート関数として生成される (`kernel_v3.typ:158–184` [設計]):

```cpp
template<Api Id, class Arch>
DispatchResult invoke(Origin const& origin, EntryView& entry) {
  using A = svc_traits<Id, Arch>;
  if constexpr (A::permission == privileged_only)
    if (!origin.is_privileged()) return reject(NOT_PRIVILEGED);
  if (origin.from_isr() && !A::from_isr) return reject_context();
  if (!A::permission_predicate(origin, entry.args)) return reject_permission();
  if constexpr (A::mode == handler) {
    static_assert(A::bounded && !A::may_yield && !A::may_block);
    return handler_service<Id>(origin, entry); // 同じ例外内で完結
  } else {
    return prepare_kernel_landing<Id, A::save_level>(origin, entry); // Thread landing へ
  }
}
```

この設計により、実行時の関数ポインタ配列走査や権限の動的再判定が排除され、コンパイル時に最適化された命令列が展開される。

=== native lane と objectland lane の分離

V3 における極めて重要な設計判断は、*native レーンと objectland レーンの分離* である (決定 2, `kernel_v3.typ:349–371` [設計])。

#table(columns: (1fr, 2fr, 2fr), inset: 4pt,
  table.header(repeat: true, [項目], [native lane], [objectland lane]),
  [所属ドメイン], [`svc_domain::native`], [`svc_domain::objectland`],
  [確定契機], [生成時 (コンポジション時) に固定], [生成時 (コンポジション時) に固定],
  [ディスパッチ方式], [上記の `svc_traits` による template 分岐], [固定親ハンドラ (`parent_handler`) への一律 Thread ディスパッチ],
  [raw 番号の扱い], [未知の番号は即座に `UNKNOWN_API` で拒否], [raw 番号を解釈せず、親ハンドラへそのまま透過],
  [親の独自意味論], [なし (explicit method を推奨)], [親による番号の独自解釈、`FORWARD_CHILD_EXIT` の親 ledger 検査を完全維持],
)

*注意すべき原則*: native の高速ディスパッチ表を objectland の raw SVC へ適用してはならない (`kernel_v3.typ:30, 349–371`)。
objectland では、親オブジェクトが子の raw SVC をインターセプトして独自のエミュレーションやポリモーフィズムを行う意味論 (`source/selftest/handler_binding.cpp` の親 A/B による同一番号の独自解釈など) が存在しており、これを native 表で先に弾いてしまうと既存の互換性が崩壊するためである。

---

== まとめと読み解きの注意点

=== 境界越え回数と性能目標の総括

#table(columns: (1.5fr, 1.2fr, 1.2fr, 1.5fr, 1.6fr), inset: 4pt,
  table.header(repeat: true, [処理内容], [V2 境界回数], [V3 目標回数], [V2 実測 cyc (net min)], [V3 目標 cyc (net min)]),
  [同期 SVC (GET 等)], [2 回], [*1 回*], [1985 cyc [実測]], [*500 cyc 以下* [設計目標]],
  [Method call 1 段], [5 回], [*2 回*], [5774 cyc [実測]], [*3000 cyc 以下* [設計目標]],
  [Method call 2 段], [10 回], [*4 回*], [11642 cyc [実測]], [*6000 cyc 以下* [設計目標]],
  [共通境界コスト], [約 446 cyc/境界], [*200--250 cyc*], [区間平均 [実測]], [部品目標 [設計目標]],
  [フレーム準備], [約 432 cyc/frame], [*100--150 cyc*], [区間平均 [実測]], [部品目標 [設計目標]],
)

注意:
- V2 の数値 (1985 cyc, 5774 cyc, 11642 cyc) は RP2350 150MHz -Og 条件における確定した *[実測]* である (`perf/svc-in-ram:docs/record/kernel_v2_record.typ:50–70`, `perf/svc-in-ram:docs/record/measurements/cost3_report.md`)。raw min は GET で 2000 cyc (空枠控除前)。
- 約 446 cyc/境界 および 約 432 cyc/frame は、cost3 到着区間表から導出された区間平均バケットの合算・平均値であり、排他的な inclusive cost ではない。
- V3 の 500 cyc, 3000 cyc, 6000 cyc および部品目標 (200--250 cyc, 100--150 cyc) は *[設計目標 (未測定・未承認の予算)]* であり (`kernel_v3.typ:599–608`)、部品目標の差分を単純加算して total を予言してはならない。

=== 作者本人が設計書を読み解く上での 5 つの急所

1. *「Handler mode 完結」の境界条件を誤解しないこと*:
   - H 完結は、Thread mode の仕事を Handler に無理やり詰め込むことではない。有界 ($H_"max" <= 10$ µs)、非ブロック、非割り込み禁止 ($"PRIMASK" <= 2$ µs)、メモリコピー上限が証明できる操作に限定される。
2. *C return を例外復帰と見なす実装は絶対に作らないこと*:
   - Cortex-M の Thread mode は、単に PSP や特権レジスタを準備しただけでは `EXC_RETURN` に基づく例外復帰として動作しない (`docs/design/kernel_v3.typ:264–278`)。context reuse 専用の復帰ゲート (`reused_context`) が不可欠である。
3. *レジスタ保存契約の緩和を安易な最適化と過信しないこと*:
   - `r4`--`r11` の保存契約や flags の保存を削っても、H 経路ではハードウェアが既に保全しているため 0--4 cyc (0--0.8%) 程度しか変わらない (`kernel_v3.typ:560` [設計])。契約を崩して安全性を犠牲にする利益はない。
   - 現行 inline SVC は `r2`/`r3`/`r12` を input-only、`LR`/cc を clobber しておらず (`armv8m.hpp:415–423`)、これを壊す変更は行わない。
4. *native と objectland を混同しないこと*:
   - native の最適化 (template dispatch) を急ぐあまり、objectland の親ハンドラ機構を破壊してはならない。生成時固定の `svc_domain` で明確に分離する。
5. *確定事実と設計予算・見積もりを混同しないこと*:
   - cost3 の数値 (1985 cyc, 5774 cyc, 446 cyc/境界) は 150 MHz RP2350 で採取された確定事実 (*[実測]*) である。
   - 一方、V3 の 500 cyc や 3000 cyc は *[設計目標 (予算)]* であり、今後の実装と実機検証を経て初めて確定する。
