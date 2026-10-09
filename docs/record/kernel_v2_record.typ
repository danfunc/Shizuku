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
