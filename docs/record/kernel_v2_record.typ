#set document(title: "Shizuku 現行カーネル (V2) 記録", author: "")
#set page(paper: "a4", margin: 18mm)
#set text(lang: "ja", size: 9pt)
#set heading(numbering: "1.")

= Shizuku 現行カーネル (V2) 記録

本書は `perf/svc-in-ram` の現行カーネルを V2 と呼び、V3 設計と比較するための記録である。対象は HEAD `3239d37` と未コミット実験差分。計測対象は Pico 2 W / RP2350 / 150 MHz。

#outline()

= 構造と制御フロー

== カーネル、kobj、object、保護

Thread の syscall は共通 dispatcher に入り、API 番号・現在の object 種別により primitive 処理か object API の親 handler への転送を選ぶ。kobj (kernel object) は登録済み root handler を持つ特別な object 種別であり、ID 一致だけでなく kind と登録済み handler ID を照合して primitive の解釈権限を確認する (`source/kernel/dispatch.cpp:198–218, 298–343`)。

RP2350 の例外復帰前に glue が current context から MPU region 2 の base/limit を計算して設定する。有効時は RNR/RBAR/RLAR の3レジスタ、無効時は RNR/RLAR の2レジスタを更新する (`modules/pico_sdk_support/rp2350/arch_glue.cpp:42–100`)。context save、MPU hook、register restore は `modules/pico_sdk_support/rp2350/armv8m_ctx.S:13–16, 34–60, 71–117`。

== SVC から dispatch、kobj、return_stub、復帰

Thread は architecture `syscall` wrapper から SVC を発行する (`modules/pico_sdk_support/rp2350/internal_headers/shizuku/archs/armv8m.hpp:415–424`)。例外入口が context を保存し、glue から dispatcher を呼ぶ (`armv8m_ctx.S:34–60, 131–135`; `arch_glue.cpp:14–21`; `source/kernel/dispatch.cpp:198–199`)。dispatcher は primitive と object API を区別し、object API は `current_handler_object` に転送する (`dispatch.cpp:203–218, 298–343`)。handler return は共通 `return_stub` が RETURN/EXIT_METHOD SVC を発行し (`armv8m_ctx.S:167–181`)、dispatch の frame pop 後、architecture glue が context/MPU/register を復元して例外復帰する (`dispatch.cpp:242–265`; `armv8m_ctx.S:71–119`)。

== method call と parent handler routing

成功経路 (privileged leaf、専用 handler なし) は5 SVC、5例外復帰、3 push、3 frame pop (`ota-artifacts/cost3_report.md:45–57`)。

#table(columns: (0.45fr, 1.5fr, 2.1fr, 1.6fr, 0.8fr),
  table.header([SVC], [Thread側の発行元], [dispatch/handler], [復帰先], [frame]),
  [1], [caller CALL_METHOD], [`dispatch.cpp:300–343` で親kobjへ], [kobj CALL_METHOD], [push A],
  [2], [kobj primitive CALL], [`dispatch.cpp:219–236` → do_call], [leaf (LR=return_stub)], [push B],
  [3], [leaf return stub], [`dispatch.cpp:300–343` で親kobjへ], [kobj EXIT_METHOD], [push C],
  [4], [kobj primitive RETURN count=2], [`dispatch.cpp:242–264`], [call_method 継続], [pop C+B],
  [5], [kobj return RETURN count=1], [kobj本人がprimitive処理], [元caller], [pop A],
)

親 handler 自身の親は root kobj、root の親は自分に route する。root handler 未登録時は panic (`source/kernel/dispatch.cpp:298–343, 300–303`)。SVC単体の GET_CURRENT_OBJECT も caller SVC → handler → return_stub SVC → primitive RETURN となり、SVC/例外復帰2回、push/pop各1 (`cost3_report.md:57`)。

== kernel 所有後の stream

stream descriptor と ring state を共有公開せず、kernel object 表へ所有権を移し、利用側は世代付き ID と batch SVC API を使う。DMA pump は kernel 保管の形状だけを読み、owner/seat/generation を検査する (`docs/design/kernel_stream.typ:3–28, 42–53`)。object destroy の契機が未定で自動片付けは未実装。1 object に複数 thread があり得るため thread exit で object 所有 stream を破棄しない (`kernel_stream.typ:28–33, 73–77`)。

= 性能計測

== 初回計測と比較条件

初回 stream 版・計測なしの記録値: direct=10, svc=1562, call1=4409, call2=9085 cyc。cost 系実機ログとは別の測定値である。以下の比較はログ表示の harness 控除 min/mean (cyc)。raw min はログ内に別記され、trace 有効時は hook overhead/IRQ を含む。

== cost / cost2 実測値

#table(columns: (1.45fr, 1fr, 1fr, 1fr, 0.8fr, 2.2fr),
  table.header([版], [svc min/mean], [call1 min/mean], [call2 min/mean], [direct min], [条件・ログ]),
  [初回 stream], [1562 / —], [4409 / —], [9085 / —], [10], [計測なし。依頼記録値],
  [cost_base], [1840 / 1858], [6012 / 6822], [別形式のみ], [9], [`cost_base.log:40–54`],
  [cost_ram], [2103 / 2112], [6251 / 6268], [別形式のみ], [9], [`cost_ram.log:40–56`],
  [cost_ram_os], [2072 / 2081], [5928 / 5944], [別形式のみ], [3], [`cost_ram_os.log:36–50`],
  [cost_ram_o2], [平均なし], [平均なし], [平均なし], [—], [`cost_ram_o2.log`; compact status only],
  [cost2_trace], [1985 / 1993], [5774 / 5790], [11642 / 11675], [9], [`cost2_trace.log:18–25`],
  [cost2_mpu_skip_final], [2028 / 2036], [5830 / 5846], [11755 / 11788], [9], [`cost2_mpu_skip_final.log:36–44`],
  [cost2_os_final], [1882 / 1892], [5314 / 5330], [10751 / 10781], [3], [`cost2_os_final.log:38–44`],
  [cost2_o2_diag_nofp], [未完走], [未完走], [未完走], [—], [`cost2_o2_diag_nofp.log:81–145`; stage 808],
)

cost2_trace から cost2_os_final の call1 min は5774→5314 cyc、約8.0%改善。-O2 は stack exhaustion (stage=808) で停止。停止原因はそこまで絞れるが LR 破壊は未確定 (`cost3_report.md:82–94`)。

旧 cost ログの call2 には min/mean 行がなく、繰返し出る compact status 値だけがある。これらを min/mean に読み替えない: cost_base 12537, cost_ram 12599, cost_ram_os 11982, cost_ram_o2 12599 cyc (`cost_*.log` の `[COST] direct=...` 行)。cost_ram_o2 は compact status の cycle 値は出るが min/mean および区間表がなく、別の `cost2_o2_diag_nofp` は stage 808 で watchdog reset。

== 区間の定義と実コード対応

区間は「直前 event → 到着 event」の timestamp 差であり関数 inclusive time ではない。hook overhead と IRQ を含む。合計/hits は異なる invocation の平均で、独立した min の和は比較指標にならない (`cost3_report.md:14–16, 39–41`)。値は `cost2_trace.log:42–57` と cost3 区間対応表。

#table(columns: (1.25fr, 0.7fr, 0.5fr, 0.7fr, 3fr),
  table.header([到着区間], [合計cyc], [hits], [平均], [含む実コード/処理]),
  [mpu_begin], [1448], [5], [289.6], [`dispatch.cpp:184–195, 324–343, 242–265, 148–163`; pending-step CAS; MPU store前],
  [push_end], [1297], [3], [432.3], [`dispatch.cpp:74–144`; reserve/limit, header, 120B context copy, exception frame copy],
  [exception_entry], [1029], [5], [205.8], [hardware entryだけでなく直前handler仕事も含む],
  [context_saved], [656], [5], [131.2], [`armv8m_ctx.S:34–60`; context/callee-saved保存],
  [mpu_end], [643], [5], [128.6], [`arch_glue.cpp:52–100`; lookup, region計算, 2/3 store, hook],
  [restore_end], [602], [5], [120.4], [`armv8m_ctx.S:77–117`; restore。BX/unstackを含まない],
  [push_begin], [396], [3], [132.0], [`dispatch.cpp:203–218, 300–312, 223–230, 170–183`; 分岐とrequest copy],
  [dispatch], [328], [5], [65.6], [`armv8m_ctx.S:134`, glueとdispatch入口],
  [handler_begin], [164], [2], [82.0], [hardware unstack後のhandler trampoline],
  [return_stub], [141], [2], [70.5], [`armv8m_ctx.S:167–172`; leaf処理も含む],
  [handler_end], [106], [1], [106.0], [primitive RETURN後のCALL_METHOD後半],
  [caller_resume], [81], [1], [81.0], [復帰とsyscall result],
)

`ota-artifacts/cost3_report.md:18–41` が区間対応の正。call1 は5 SVC/復帰、push3/pop3。GET_CURRENT_OBJECT は2 SVC/復帰、push/pop各1 (`cost3_report.md:45–57`)。

cost2 各版の主な区間 min/mean (cyc) は次の通り。旧 cost_base/cost_ram/cost_ram_os のログには profile call1 が exception_entry/context_saved/dispatch の3区間しか残っておらず、他区間の min/mean は不明。cost_ram_o2 は区間出力前に reset する。

#table(columns: (1.45fr, 1fr, 1fr, 1fr),
  table.header([区間], [cost2_trace], [mpu_skip_final], [os_final]),
  [exception_entry], [1029 / 1034], [1028 / 1028], [1049 / 1049],
  [context_saved], [656 / 656], [654 / 654], [640 / 640],
  [dispatch], [328 / 328], [327 / 327], [320 / 320],
  [push_begin], [396 / 396], [395 / 395], [358 / 358],
  [push_end], [1297 / 1297], [1253 / 1253], [915 / 915],
  [handler_begin], [164 / 172], [166 / 169], [174 / 184],
  [handler_end], [106 / 122], [104 / 112], [107 / 107],
  [return_stub], [141 / 145], [141 / 158], [146 / 150],
  [caller_resume], [81 / 85], [80 / 111], [81 / 95],
  [mpu_begin], [1448 / 1448], [1474 / 1474], [1105 / 1105],
  [mpu_end], [643 / 643], [732 / 732], [630 / 630],
  [restore_end], [602 / 602], [603 / 603], [580 / 580],
)

出典: `cost2_trace.log:42–57`, `cost2_mpu_skip_final.log:41–56`, `cost2_os_final.log:42–57`。区間表の単一値も同じ log の min/mean が同値だった区間を示す。

== MPU body と性能結論

call1 MPU body は合計80 cyc / 5 updates = 16 cyc/update (`cost2_trace.log:56–57`)。skip版はMMIO write 0でもbody 135 cyc、untraced call1は5774→5830 (+56) (`cost2_mpu_skip_final.log:110–112`)。この条件でMPU書込みは主因ではない。

境界1回あたり save/dispatch/MPU-wrapper/restore は約446 traced cyc。pushは約432 cyc/hitで120B context複写を含む。境界越え回数とcontext copyが有力な改善対象だが、これはtraceからの部品値であり新ABIの実測改善量ではない (`cost3_report.md:7, 61–78`)。

= RAM 化実験

== RAM 配置、残存 flash 呼出し、サイズ

RAMへ移動したもの: RP2350/RP2040 architecture header、kernel dispatcher、kobj handler、memory helper、SVC entry/return stub、profile hook (`cost_report.md:5–19`)。RP2350 audit の残存 flash edge は cost_ram 33、cost_ram_o2 38、cost_ram_os 37、cost_ram_noprofile 33 (`cost_report.md:48–66`)。

#table(columns: (1.5fr, 0.8fr, 1fr, 1fr, 1.4fr),
  table.header([版], [flash edge], [.data B], [.data差 B], [time_critical input B]),
  [cost_base], [—], [7872], [—], [2784],
  [cost_ram], [33], [21232], [+13360], [15996],
  [cost_ram_o2], [38], [22928], [+15056], [17568],
  [cost_ram_os], [37], [19536], [+11664], [14140],
  [cost_ram_noprofile], [33], [21224], [+13352], [15796],
)

baseline→cost_ram は `.data` +13360 B、time_critical input +13212 B (`cost_report.md:56–74`)。残存呼出しは panic/diag、time、DMA、SDK boot/start-secondary 等。checker は動的 callback・flash rodata を完全証明せず、ゼロ件受入は未達。追加stackとMPU regionの実機確認も残る (`cost_report.md:48–80`)。

== 未完了点

RAM閉包0件は未達。profileなしの BSS は profile有りより4112 B小さい (event buffer 4096 B + state 16 B)。`.data` はRAM使用全体を示さず、map input section集計とは区別する (`cost_report.md:33–45, 56–80`)。cost_ram_o2 はビルド成果物があってもその性能ログはresetで完走していない。

= BLE と既知の問題・未決事項

BLE有効 stress では late window の遅れが十数秒続く既知事象で、`bdba79e` でも同様と報告され、別課題扱い (`docs/design/svc_in_ram.typ:91–92`)。BLE OTA は busy-poll 修正後、device reports done: OK、9.84 s (`ota-artifacts/fix_ota_send.log:52`)。

object destroy の契機と stream 自動片付けは未決 (`docs/design/kernel_stream.typ:28–33, 73–77`)。atomic / wait-wake は将来項目で未実装 (`kernel_stream.typ:82–89`)。ESP32-C6 は現 worktree で未起動、V3動作証拠なし (`docs/design/kernel_v3.typ:525–527`)。-O2 の停止は stack exhaustion まで絞れるが、LR破壊は未確定 (`cost3_report.md:82–94`)。

= V3 設計に効く事実

#enum[
  [method call は5 SVC/5例外復帰、3 push/3 pop。kobj中継を含む境界数をV3で比較する。],
  [1境界の save/dispatch/MPU-wrapper/restore は約446 traced cyc。],
  [push は約432 cycで120B context copyを含み、MPU bodyは5更新80 cyc。MPU write削減単独は主因でない。],
  [区間値はevent間差でhook/IRQ込み。区間minの和を速度予測に使わない。],
  [RAM実験はflash edge 33–38件を残し、.dataを約11.7–15.1 KiB増加。RAM閉包・stack・MPU受入を別に測る。],
]

== 03 から移した現行決定

`docs/03_porting_policy.md` の決定のうち、今も有効でコードや他の正本に要点が無かったものを移す。コードのコメントは D 番号で参照しているため、番号は残す。廃止・置換された決定は移さない (経緯は `docs/record/legacy_docs_summary.typ` を参照)。根拠欄は 2026-10-21 時点の worktree のコードと文書で確認した位置である。動作は実機で確かめ直していない。

#table(
  columns: (0.8fr, 3.7fr, 1.9fr),
  table.header([番号], [決定の要点], [根拠]),
  [D2], [arch と board を分ける。arch は ISA 依存 (例外入口、文脈退避、特権遷移、atomic)、board は SoC 依存 (時刻、コア起動、ペリフェラル、クロック)。`cpu_driver` の名は廃止した。], [`internal_headers/shizuku/concepts/arch.hpp`、`concepts/board.hpp`、`arch/`、`boards/`],
  [D3, D5], [実行プリミティブは CALL / RETURN / SWITCH / GRANT の 4 種。REDISPATCH は削除済み。経路は今走っているオブジェクトの種別 (`object_kind`) だけで決め、svc 番号・ID・段数のパリティでは決めない。種別は呼び出しフレームに退避し、書けるのはカーネルだけ。exit は RETURN 1 本に統一し、オブジェクトが申告した段数 (`depth_claim`) をカーネルが実際の深さと突き合わせる。], [`internal_headers/shizuku/kernel_abi.hpp`、`object_api.hpp:152–163` の注釈],
  [D12 (I-9)], [一般オブジェクト起因の失敗は必ずエラーで返す。panic はカーネル自身の不変条件が破れたときだけ。共通の原則は「黙って捨てない」で、返す先が無いときだけ panic する。], [`kernel_abi.hpp` の `kernel_error`、`templates::result<T>`],
  [D15, D38], [ビルドは Bazel のみ。CMake は 2026-08-21 に削除した。構成のつまみは `.bazelrc` と `configs/BUILD.bazel`、オブジェクト番号は `objects.list`。CYW43 の SCK は `configs/cyw43_clock.bzl` から導出し、`PICO_CONFIG_EXTRA_HEADER` 経由で SDK 内部まで届ける (`--copt` では届かない)。], [`BUILD.bazel`、`MODULE.bazel`、`configs/cyw43_clock.bzl`],
  [D17], [XNO 分離。Shizuku 本体には kernel、kobj、arch、board、カーネル自己テスト用オブジェクトだけを残し、小さく保つ。XNO との境界は svc とメソッド呼び出しの安定 ABI。公開ヘッダの区分 (`public_headers/` 相当) は未実装。], [`public_headers/` が無いことを確認],
  [D18, D19], [スレッドの表とスタックはオブジェクト側から貸す (`set_thread_storage`、`spawn_request` の `stack_base`)。所有 (誰が出すか) と保護 (どこに置くか) は別問題で、置き場所はカーネルが検査する。`base >= unprivileged_floor()` を満たさなければ panic する。簿記は特権のみの領域、作業領域は非特権から届く領域から出す。], [`source/kernel/`、`boards/rp2350_pico2/board.cpp:149`],
  [D20], [メモリの授受は持ち主を記録する。`MEMORY_ALLOCATE` / `RELEASE` / `HAND_OVER` / `OWNER`。持ち主でないものは返せず (`NOT_OWNER`)、`HAND_OVER` で渡した側は以後返せない。持ち主は発行元から導出し、名乗らせない。], [`internal_headers/shizuku/object_api.hpp:76, 200`],
  [D23], [オーバークロックは今は入れない。理由は「SPI が上げられないから」ではない。SCK を半分にしても LED 書き込みの最小値は +52 µs しか変わらず、費用の大半は ioctl 往復とスケジューリング方針で、clk_sys に比例しない。分周比は自由なつまみではなく、PIO プログラムの選択 (応答のサンプル位置) と対で決まる。上げるときは XIP のスイープを先に回す。], [`configs/cyw43_clock.bzl`。実測値は消した文書の要約に残す],
  [D24, D25], [実行権の貸し借りの単位はクロックにする (残量は「クロック数」で持つ)。仕事量はクロック数で、µs で持つと周波数が変わったとき同じ数字が別の仕事量を意味する。SLEEP は壁時計なので µs のまま、単位を揃えない。タイマの装填 (`arm_timer`) とサイクル計数 (`grant_charge`) は別の関数で、arch ごとに変わるのは装填だけ。残量が装填の下限より小さいときは貸さずに断る (`GRANT_TOO_SMALL`)。下限へ切り上げて貸すと、借り手は貸された以上に走る。], [`templates/kernel.hpp`、`source/kernel/thread.cpp:232–264`、`kernel_abi.hpp:133`],
  [D26], [オブジェクトシステムは定数時間で抜ける。名前による解決や文字列比較は入れない。`DECLARE_NAME` の名前は保持するだけで比較しない。例外はファイルシステムの `lookup` で、返すのはアドレスなので引いた後の読み出しは定数時間。メモリ割当は階級別の空きリストで O(1)。2026-08-21 の監査でオブジェクトシステム本体に文字列処理が無いことを確認した。], [`object_api.hpp`、`source/kernel_object/memory.cpp`、`objects/flash_fs.cpp`],
  [D27, D28], [オブジェクトは自分で名乗る (`DECLARE_NAME`)。名前は発行元から導出し、付け直しは断る。`OBJECT_NAME` は名前を返すだけで比較しない。カーネルは `resolve` を持たない。名前解決が要る系 (後から積むドライバ、FS、セッション) は System Object の仕事。番号はソースに書かず、ビルドが振る。各モジュールが `objects.list` (1 行 1 名) を置き、`tools/gen_object_ids.py` が読む。二重宣言と上限超過は生成時に落とす。`create_object` の衝突は拒否し、鳴らす。], [`modules/pico_sdk_support/objects.list`、`tools/gen_object_ids.py`、`object_api.hpp`],
  [D29, D30, D32], [2 コア化後、kobj の表、arena、名前、スレッドスタックの控えは `table_lock` で守る。錠を握ったまま実行権を手放さない (スケジューラを呼ぶ前に放す)。他コアを止める操作は入れ子で数え、実際に止めるのは外側の 1 回だけにする。2 コアが互いを止め合うと即デッドロックするので、止める側は錠で 1 コアに絞る。], [`table_lock`、`park_other_cores` の呼び出し元],
  [D31, D34], [flash は 1 から 0 にしか書けない前提で、消去は本当に 0xFF へ戻すときだけにする。目録は追記式で、削除はビットを落とすだけ。締切のある系では消去を書き込み経路に置かない (1 セクタの消去約 33 ms は媒体の性質で縮まない)。空きは `PREPARE` で事前に払い、走行中の書き込みは 0xFF の場所へ書くだけにする。まとめて消すほど 1 バイトあたりは安いが、64KB のブロック消去は 64KB 境界に揃えないと効かない。], [`objects/internal_headers/shizuku/objects/flash_fs.hpp:53`、`objects/flash_fs.cpp`],
  [D39], [デバッグ UI は VS Code 標準の Run and Debug に寄せる。launch.json は固定名 `/tmp/shizuku-gdb-port` だけを指し、焼く前にポートを聞かない。焼いた直後はデバイスノードが消えて戻るので、焼く側は消えるのを待ってから戻るのを待つ。焼く前にポートを掴んだ gdb だけを SIGKILL する (SIGTERM では死なない)。], [`.vscode/launch.json`、`.vscode/tasks.json`、`tools/flash_and_wait.sh`],
  [D40, D41, D43], [デバッグは自己ホスト型。Cortex-M33 の DebugMonitor (例外 12) と FPB、DWT で、外付けプローブは要らない。halting debugger が繋がっている間は DEMCR の MON_EN が効かないので併用しない。MON_STEP は arm した瞬間の実行中の文脈の次の 1 命令に効くので、対象が次に復帰する直前 (CTX_RESTORE の直前) で arm する。DEMCR に書いたら dsb と isb を入れ、対になる clear にも入れる。], [`modules/pico_sdk_support/rp2350/objects/gdb_stub.cpp`、`consume_pending_step`、`source/selftest/debug_ladder.cpp`],
  [D42, D45, D47], [USB CDC は 2 本。若い番号が診断、大きい番号が GDB。診断は溢れたら捨てて本業を止めない。GDB の返事は 1 バイトも落とせないので、FIFO が空くまで上限付きで待つ。記述子で名乗ったインターフェースには必ずドライバが要る。panic は診断を 1 行ごとに flush して出し切り、もう一方のコアを止め、USB に要る IRQ だけを残して SysTick を止める。macOS は CDC のインターフェース名を読まないので、名前では見分けられない。見分けは番号の規則による。], [`objects/usb_cdc.cpp` (`usb_cdc_isolate_for_panic`)、`shizuku_panic_dump`],
  [D44, Q8], [GRANT_REGION: 窓の開示は特権オブジェクトだけができる (誰に何を見せるかを決める権限)。開示された窓は対象オブジェクトの性質として保存され、対象の CALL / SPAWN のたびに効く。呼び出し元が渡す値ではない。region0 は XIP 全域ではなく、ファーム本体の終端までに絞る。PMSAv8 は 32B 粒度で、要求範囲の末尾はそのブロックの末尾まで読めてしまう (HW の限界)。窓の取り消し (CLOSE 時の解除) は未実装。], [`object_api.hpp` (`GRANT_REGION`)、`boards/rp2350_pico2/board.cpp`、`source/selftest/flash_stream.cpp`],
  [D48, D51, D52, D53], [GDB stub は走行中も応答する。走らせたら返事をせず主ループへ戻り、止まったとき、または 0x03 を受けたときだけ停止応答を送る。スレッド ID は 16 進 (10 進で書くと 10 本目から静かにずれる)。止めたものは自分が止めたものだけをビットマスクで持ち、detach で戻す。all-stop は運用上使えない (止めると系が終わるスレッドがある) ので non-stop を使う。停止通知は握手が済むまで送らない。], [`modules/pico_sdk_support/rp2350/objects/gdb_stub.cpp`],
  [D49, D55], [GDB stub は 3 役に分ける。server 1 本が CDC と RSP の解釈を担い、コアごとの agent 2 本が自コアの FPB と DEMCR を仕掛ける。FPB とデバッグ機構はコアごとに独立なので、agent は両コアに置く。止めてよい相手は絞る。デバッガ自身 (server と agent) と転送スレッドは GDB の一覧に出さず、止められない。], [`gdb_stub.cpp` (`g_protected_threads`)],
  [D54, D57], [止めたスレッドが走り続けて見えた原因は、切り替え経路と grant の巻き戻し経路が状態を無条件に上書きしていたことだった。両方に状態ガードを入れた。寝ているスレッドは `sleep_us` の抜け条件に状態を入れ、締切で勝手に起きないようにした。例外復帰の全経路 (CTX_RESTORE) に検査を入れる案は測って効果が無かったので採らなかった。syscall を撃たない純粋な計算ループは量が切れるまで止まらないので、「全部止められる」とは書かない。], [`templates/kernel.hpp` (`do_switch`、`grant_unwind`)、`kernel_object.hpp` (`sleep_us`)],
  [D58], [D1 (カーネルはオブジェクトを知らない) は 2026-09-28 に廃止した。現行の必須要件は、システムコールハンドラとカーネルオブジェクトを再登録できること (ホットスワップ) で、両者の密結合は禁じない。MMU のある構成では重くても保護してよい。オブジェクトは保護付きオブジェクトとカーネル空間オブジェクトの 2 種別を持つ。kobj の差し替え想定と、ID 0 のハードコード禁止は維持する。], [`docs/06_esp32_port_manifest.typ` (ホットスワップの手順)、`kernel_abi.hpp:68` (`KERNEL_OBJECT`)],
)

未決として残るもの:

- Q7 (idle スレッド): 未着手。起こし方として、board のアラームを使うか、grant のタイマの意味を広げるかを決める。
- D26 残: `schedule()` は O(THREAD_COUNT) の巡回のまま。ready のビットマップと ctz で定数にできるが未着手。
- D42 残: CDC は今も直接呼び出しで、ストリームとして扱っていない。
- GDB の書き込み系 (メモリ書き込み `M`/`X`、レジスタ書き込み `G`/`P`) は未実装。空の返事のまま GDB に黙って無視される。明示的にエラーを返すか、実装するかを決める。
- D53 の限界: カーネルが持つデバッグ記録は直近 1 件だけなので、2 本がほぼ同時に止まると古い方を見落とす窓がある。
