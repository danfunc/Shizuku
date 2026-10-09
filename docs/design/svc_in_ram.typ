= svc 経路の RAM 配置 (設計調査)

状態: 調査のみ (ソース未変更)。2026-10-07、feat/kernel-stream 3239d37 時点。
「推測」と書いた箇所は未計測。確定事実は file:line 付き。

== 結論

- 現状、svc 経路は*全部 XIP flash 上*にある (`__not_in_flash_func` も
  `.time_critical` もカーネル側には 1 つも無い)。
- 推奨は*案 (b) ホットパスだけ RAM*。ただしその前に、設定だけで上限を測る
  *フェーズ 0 (全体 copy_to_ram の A/B 実験)* を挟む。
- 期待効果の見立て (推測、確度 低〜中):
  - `[COST] svc` の *min* (キャッシュ温かい値) は*数 % 〜 1 割程度*しか減らない。
  - 効くのは*平均と裾* (BLE/USB のコードに XIP キャッシュを追い出された直後)。
  - 「flash 書込み中に svc・割込みが動く」ことは*この案では起きない* (§3)。
    OTA / STRESS の十数秒 late の原因ではない。

== 1. svc 経路と配置

1 回の `push`/`pop` = 例外 2 回 + ハンドラ本体。
`syscall()` の `svc 0` (`armv8m.hpp:406-421`) → 入口 → `svc_dispatch` が
`call_frame_push` でトランポリンを積みハンドラへ (`dispatch.cpp:194,306`) →
`handler_entry_point` → `handle` → `stream_push_n` 等 → return stub の `svc 0`
(`armv8m_ctx.S:158-171`) → `svc_dispatch` RETURN → `call_frame_pop`。

#table(
  columns: (auto, auto, auto),
  align: left,
  [部品], [場所], [今のセクション],
  [`shizuku_armv8m_svc_entry` (CTX_SAVE/RESTORE マクロ)], [`armv8m_ctx.S:113-119`], [`.text` = flash],
  [`shizuku_current_context` / `shizuku_svc_dispatch`], [`arch_glue.cpp:13-21`], [flash],
  [`shizuku_arm_pending_step`, `shizuku_restore_region_window` (CTX_RESTORE が毎回 bl)], [`arch_glue.cpp:41-70`], [flash],
  [`KERNEL::svc_dispatch`, `do_call`, `call_frame_push/pop`], [`dispatch.cpp:72-330`], [flash],
  [`handler_entry_point`, `KERNEL_OBJECT::handle`], [`handler.cpp:121-126, 1577-`], [flash],
  [`table_lock/unlock`, `stream_lookup/pin/unpin/stack_range`, `stream_push_n/pop_n`], [`handler.cpp:994-1006, 1040-1080, 1199-1285`], [flash],
  [`ARCH::cas32` / `load_acquire32` / `store_release32`], [`armv8m.hpp:386-399` (static inline)], [呼び元に従う (inline)],
  [`shizuku_memcpy` (inline)], [`freestanding_mem.hpp:11-30`], [呼び元に従う (always_inline、既存の慣習)],
  [`call_frame_push` の `__builtin_memcpy` と `header->saved = *context`], [`dispatch.cpp:116,119`], [*libc の memcpy を呼ぶ* → flash 側 (RP2040 ELF では `__wrap_memcpy` = `0x1000be1c`)],
  [vector table], [`.ram_vector_table` (`memmap_default.ld:143`)], [テーブルは RAM、指す先 (入口関数) は flash],
)

既存の RAM 配置の用例は 3 件だけで、いずれも「flash が止まる間に動く」ための
もの: `flash_fs.cpp:23,31` (`__not_in_flash_func` と
`section(".time_critical.fs_fault_entry")`)、`ota.cpp:144`
(`__no_inline_not_in_flash_func(commit_blast)`)、`gdb_stub.cpp:514,1734`。
*速度目的の RAM 配置はまだ無い*。リンカは SDK 既定 (`.time_critical*` は
`.data` に入り起動時に RAM へコピー、`memmap_default.ld:152-156`)。
`.bazelrc` / `firmware/BUILD.bazel` に `copy_to_ram` / `no_flash` 系の設定は無く、
バイナリは XIP 実行。独自の `rp2350_flash_limit.ld` は flash 上限の ASSERT のみ。

== 2. 1562 cycle の内訳の見立て

測定は `call_cost.cpp:115` の `GET_CURRENT_OBJECT` 1 回 (= 上の往復から
stream 本体を除いたもの)、*min* 採用 (`call_cost.cpp:20-22`)。*以下は全て推測*で、
区間計測 (§5) で確定させる。

#table(
  columns: (auto, auto, auto),
  align: left,
  [要素], [見立て], [RAM 化で],
  [例外入退出 ×4 (svc 入口・復帰 ×2)。ハード自動 stacking/unstacking は各 ~12 cyc、FP 文脈が生きていれば遅延退避が上乗せ], [数百 cyc], [*減らない* (命令の置き場所と無関係)],
  [CTX_SAVE/RESTORE (`bl` ×4 / 例外あたり、MPU region 書換、`msr`/`isb`、CONTROL 書換え)], [数百 cyc], [flash 待ちぶんだけ減る。`isb` / MPU / sysreg は*減らない*],
  [`call_frame_push/pop` (frame 32〜104B + context 120B の複写、検算、ledger)], [数百 cyc], [複写が libc memcpy なら veneer + flash 経由。ここは減る余地が最大],
  [`handle` の switch、`current_object` 等、ledger 読み], [〜百 cyc], [flash 待ちぶん減る],
  [`table_lock` (cas32 = LDREX/STREX) ほか atomics], [数十 cyc/回], [*減らない* (SRAM 側の排他モニタ)。2 コア競合時のみ伸びる],
  [XIP キャッシュミス], [min では ≈0 (推測: 経路合計 ~4.5KB < 16KB)。*mean・裾では支配的になり得る* — 1 line fill が QSPI 往復で数十 cyc 級 (推測)], [RAM 化で*ほぼ全部消える*],
)

根拠: 経路の関数サイズ合計。現存する ELF は RP2040 (armv6m, -Og) のもの
(RP2350 ELF はこの環境に無い。Thumb-2 なので数 10% 小さいはず) で、
ホットパス (`svc_dispatch` 572B, `handle` 500B, `stream_pop_n` 412B,
`stream_push_n` 344B, `call_method` 312B, `schedule` 600B ほか) の合計は
KERN 1.1KB + KOBJ 3.4KB ≒ 4.5KB。XIP キャッシュ (RP2350: 16KB) に*単独なら収まる*ので、
min 値にミスは乗りにくい。逆に BLE (cyw43/btstack)・USB が走る STRESS では
追い出されるので、平均と tail に出る。*「min=1562 のうち XIP 由来は小さい」*が
最有力の仮説で、「RAM 化で svc が数倍速くなる」ことは期待しない。

== 3. 第二の軸: flash 書込み中に動けるか

*結論: 今の構成では動けない。RAM 化しても変わらない。*

- `flash_safe_execute` は呼んだコアで `save_and_disable_interrupts()` する
  (pico-sdk `pico_flash/flash.c:203`)。他コアは `multicore_lockout` の victim
  ハンドラ (RAM 上) で割込み禁止のまま spin させられる。両コアとも止まるので、
  svc 入口がどこにあっても走る機会が無い (PRIMASK 下の SVC は HardFault 昇格し得る。
  推測、未確認)。
- 実害の本質は XIP 停止ではなく*割込み禁止の長さ*。`ota.cpp:403-409` は 64KB erase
  が約 108ms、`ota.cpp:457-462` は 4KB 書込みが 8ms で CYW43 の SDIO バスが壊れる
  (ble_uart.cpp:1335-1338 の記述)。このため OTA は 4KB erase (~40ms) / 256B
  page 単位に刻み、間で yield している。RAM 化はこの時間を 1 µs も縮めない。
- STRESS で BLE 有効時に late 十数秒・`led_write` ~1s は、stream 変更前
  (bdba79e) でも出る別課題 (依頼文)。svc の 10µs 級の差では説明できず、
  RAM 化の対象外。
- 将来像 (本案の範囲外): victim コアを「RAM のみで動く」状態にし、割込み禁止を
  やめて*その間も svc・タイマ・DMA を処理する*設計にすれば意味が出る。要件は
  コード + rodata + vtable + スタック + ハンドラ表が全て SRAM、`memcpy` 等の
  libc も RAM、MPU 設定も RAM 経由、CYW43 の SDIO バスを止めない仕組み。
  案 (b) の RAM 化はその*前提の一部*にはなるが、単体では効果が出ない。

== 4. 方式案

#table(
  columns: (auto, auto, auto, auto),
  align: left,
  [案], [内容], [RAM 増 (RP2040 ELF 実測ベース)], [所見],
  [(a) svc 入口〜dispatch〜全ハンドラ], [`.S` の入口/復帰、`arch_glue`、`dispatch.cpp` 全体、`handler.cpp` 全体 (`.time_critical.*`)], [KERN ≈ 4.1KB + KOBJ ≈ 9.5KB + glue ≈ 0.3KB ≒ *14KB*], [`init`・`create_object`・`pump` など冷たい処理も入る。RP2040 (264KB) でも許容だが効きが薄い部分に払う],
  [(b) ホットパスのみ], [入口 `.S`、`current_context` / `restore_region_window` / `arm_pending_step`、`svc_dispatch` / `do_call` / `call_frame_push/pop`、`handler_entry_point` / `handle` / `call_method` / `exit_method` / `yield_to` / `sleep_us` / `schedule` / `stream_*` 5 種 / `table_lock` / `current_object`], [≒ *4.5〜5KB*], [cyc に効くものだけ。dispatch の `switch` が呼ぶ冷たいハンドラ (create_object 等) は flash に残り、呼ぶたびにミス/往復が起きるが頻度は低い],
  [(c) カーネル全体 / バイナリ全体 copy_to_ram], [(c1) カーネル TU 全部 ≒ 14KB、(c2) SDK の `copy_to_ram` ビルド], [(c1) ≒ 14KB、(c2) 本実機の `.bin` は 378〜435KB (`ota-artifacts/*.bin`) で RP2350 の 520KB SRAM に *実質載らない* (bss/heap/stack/BLE バッファ込みで超過)、RP2040 は問題外], [(c2) は*計測専用の上限実験*以外には使えない。(c1) は (a) とほぼ同義],
)

*余裕*: RP2350 SRAM 520KB (本ビルドの `.data` 16KB + `.bss` 89KB は RP2040 ELF 値で参考) に
対し 5KB は 1%。RP2040 264KB でも 2% 弱。ESP32-C6 は IRAM が小さく
(HP SRAM 512KB のうち命令用は設定依存で、IRAM に載せたぶんだけ cache との取合い)、
*ホットパスだけ*の (b) が筋。

*arch 能力フラグとの整合 (1c4fe7b の流儀)*: `HAS_CYCLE_COUNTER` と同様に、
arch/board ヘッダへ静的な性質を足し、マクロ (例: `SHIZUKU_HOT`) の定義を
モジュール側に置く。

- armv8m / armv6m: `SHIZUKU_HOT` = `__attribute__((section(".time_critical.shizuku_hot")))`
  (pico の `__not_in_flash_func` と同じ配置、`-ffunction-sections` と競合しない名前)
- ESP32-C6: `IRAM_ATTR`
- dummy / ホストテスト: 空
- カーネル本体 (ISA 非依存) は `SHIZUKU_HOT` を付けるだけで、配置先を知らない。
  XIP を持たない (または全 SRAM 実行の) 構成では空に倒せる。

*リスクと落とし穴*

- *RAM から flash 関数を呼ぶと意味が薄い*。コードは RAM でも (1) 呼び先が flash なら
  フェッチは XIP に戻る、(2) `bl` は ±16MB なので RAM→flash は 256MB 離れ、リンカが
  veneer (間接分岐) を入れる (RP2040 ELF の `____wrap_memset_veneer` が実例)。
  *経路が閉じていること* — 呼び先を全て同じ `SHIZUKU_HOT` にする — が要件。
- `call_frame_push` の `__builtin_memcpy` / 構造体代入 (`dispatch.cpp:116,119`) は
  libc の memcpy を呼ぶ。RP2350 の `memcpy` の配置は*未確認*。`shizuku_memcpy`
  (always_inline、`freestanding_mem.hpp:11-30`) に置換するか、RAM の memcpy を使う。
  ここが (b) の効きを左右する。
- const データ・vtable・switch の jump table (Thumb-2 では関数内埋込だが
  GCC の最適化次第で `.rodata`) は flash に残る。ホットパスの
  `.rodata` 参照は 0 に近い (表は `m_streams` 等の RAM 実体) が、`nm`/`objdump` で確認する。
- *例外ベクタは RAM テーブルだが、入口関数は flash* (`exception_set_exclusive_handler`、
  `board.cpp:44`、`board_rp2040.cpp:110`)。入口を RAM に移すには `.S` 側の
  `.section .time_critical...,"ax"` が要る。
- デバッグ: GDB stub は RAM のブレークポイントを別扱いする (flash は FPB)。
  RAM 内のカーネルコードに止める経路は要確認。`addr2line` は ELF に LMA/VMA の
  両方が出るので問題なし。
- RAM 命令フェッチは同バンクのデータ・DMA・他コアと競合し得る (SRAM は 4KB 単位の
  ストライプ。*推測*)。flash より遅くなることは通常無いが、*ゼロ競合でもない*。
- `-Og` ビルド (`.bazelrc`) でのサイズ。最適化が上がると縮む。

== 5. 推奨案と測定手順

*推奨: (b)。ただし実装前にフェーズ 0 で上限を測る。*

=== 測定手順

+ *フェーズ 0 (設定のみ、コード変更なし)*: RP2350 W のビルドを
  `--@pico-sdk//bazel/config:PICO_NO_FLASH`/copy_to_ram 相当で作る (載らなければ
  カーネル TU だけの (c1) を `-Wl,--...` で `.data` 側へ)。`[COST] svc/call1/call2`
  と STRESS の `late`・`led_write`・BLE 有り/無しの差を取り、*RAM 化の上限*を知る。
  svc が 10% 未満しか減らなければ、速度目的の RAM 化は*やらない*判断もあり得る。
+ *区間計測 (§2 の推測を確定)*: `CYCCNT` を CTX_SAVE 直後・dispatch 前後・
  CTX_RESTORE 直前で `volatile` 配列へ記録し、例外入退出、保存復元、dispatch、
  ハンドラ本体の各 cyc を出す (診断マクロの裏、本番に残さない)。
+ *(b) 実装後*: `call_cost` の `svc`/`call1`/`call2` の min *と mean* を before/after で
  比較 (`call_cost.cpp:165` が両方出す)。min が動かず mean が下がるのが想定どおりの姿。
+ *STRESS*: BLE 有効/無効それぞれで 60 秒、`late`・`led_write` を比較。
  差が出ても svc 起因かの切り分けのため、(b) 無し/有りを同一条件で交互に。
+ *BLE OTA*: flash 書込み中の挙動 (割込み禁止時間、`took=`) が変わらないこと、
  転送が完走すること。ここは*改善を期待せず、回帰が無いこと*の確認。

=== 実装の段取り (案 (b))

+ `modules/.../rp2350/armv8m_ctx.S`、`rp2040/armv6m_ctx.S`: 入口と復帰マクロを
  含むセクションを `.time_critical.*` に (CTX_SAVE/RESTORE は macro でそのまま移る)。
+ `arch_glue.cpp` / `arch_glue_v6m.cpp`: 4 関数に `SHIZUKU_HOT`。
+ arch ヘッダに `SHIZUKU_HOT` の定義 (§4)。concepts にフラグは不要
  (マクロなので)。
+ `source/kernel/dispatch.cpp`: `svc_dispatch`、`do_call`、`call_frame_push/pop`、
  `current_context` に付与。`__builtin_memcpy` / 構造体代入を `shizuku_memcpy`
  か手書き ldm に。
+ `source/kernel_object/handler.cpp`: 宣言 (`:28-111` の特殊化宣言) にも
  属性を付ける (定義だけだと一致しない)。対象は §4 (b) の一覧。
+ `nm` で全ホット関数が `.data` 側 (0x2000_0000 台) にあり、`objdump -d` に
  flash (0x1000_0000 台) への `bl`/veneer が*無い*ことを確認 (これが完了条件)。

=== 未決事項

- RP2350 ELF での実サイズと `memcpy` 配置 (この環境の ELF は RP2040 のみ)。
- XIP キャッシュの正確な構成と miss コスト (データシート確認)。
- GDB stub が RAM 上のカーネルコードで止められるか。
- ESP32-C6 側の IRAM 予算と、`SHIZUKU_HOT` を port 側のどこで定義するか。
- (b) の効きが*平均・裾だけ*だった場合に、複雑さ (リンク配置の保守) に見合うか
  — フェーズ 0 の結果で判断。
- §3 の将来像 (flash 書込み中も動く) をやるなら別設計文書とする。
