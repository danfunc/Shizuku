# cost2 途中成果・実機確認待ち

commit/push/実機操作・他 worktree 操作なし。既存 cost_* の UF2/ELF/実機ログと設計文書は保持。

## 1. 区間欠落と MPU

- `modules/pico_sdk_support/objects/usb_cdc.cpp:239-266`: CDC FIFO に行全体が入らないと無言で捨てる。旧ログは profile の前半 3 行程度だけ残り、他の SELFTEST 行も抜けている。hook が無いという説明より **出力欠落が有力**。新しい hits を実機で確認するまでは確定しない。
- `source/selftest/call_cost.cpp:164,220,228,231,237,240`: 計測外で 10ms 間隔を空け、全 stage の平均記録回数 (hits)、event 件数 (coverage)、全 timestamp 差 (trace_total)、MPU body の直接 DWT 差を表示。
- `modules/pico_sdk_support/rp2350/arch_glue.cpp:52-98`: 更新対象は region 2 の **1 領域**。有効時 3 レジスタ (RNR/RBAR/RLAR)、無効時 2 (RNR/RLAR)。MPU body 内の DSB/ISB は 0。`armv8m_ctx.S:106` の CONTROL 復元後に ISB 1 回。**多領域を毎回張り直している実装ではない**。
- body 計測は region window の計算・書込みの発行を含み、current_context 取得は含まない。CYCCNT の読み出し費用を含む。書込みの完了待ちを単独で証明する計測ではない。`to=mpu_end` は hook 費用も含む別の区間。
- stage 0 は最初の timestamp なので svc/call1 では「そこへ至る区間」は存在しない。hits には stage 0 も出る。EXIT_METHOD / reply の成功は non-returning なので、そのハンドラの handler_end は通らない (`handler.cpp:361,1598-1615`)。privileged cost leaf / 通常 GET では return_stub hook を通る。非特権 return_stub hook は意図的に飛ばす (`armv8m_ctx.S:168-175`)。

**最初に cost2_trace.uf2 と cost2_mpu_skip_final.uf2 を比較**。同じ現行ソース・-Og・profile 有り、後者だけ `SHIZUKU_MPU_SKIP_SAME=1`。`arch_glue.cpp:61-92` の per-core cache が同一 region_base/limit の書込みを省く (実験フラグ、既定 off)。現行 runtime region-2 writer はこの hook のみ。board 初期化以外に writer を増やす場合は cache invalidation が必要。実機の保護検証は未実施。

確認行:

```text
[COST] profile call1 to=mpu_end min=N mean=M cyc samples=128 overflow=0
[COST] profile call1 to=restore_end ...
[COST] hits call1 stages0..12=...
[COST] coverage call1 events_min=N events_max=N overflow=0
[COST] trace_total call1 min=N mean=M cyc (enabled hook overhead included)
[COST] mpu_body call1 min=N mean=M cyc updates_last=U mmio_writes_last=W dsb=0 isb=0
```

stage 順: caller_pre, exception_entry, context_saved, dispatch, push_begin, push_end, handler_begin, handler_end, return_stub, caller_resume, mpu_begin, mpu_end, restore_end。

raw cost は hook 無効で測り、trace は hook 有効で別に採取する。trace_total と既存 raw min は同じ物差しではない。区間の **mean** の和は trace の時間を説明するが、独立 min の和は不可。hits も欠けていれば記録側、hits は揃っているのに行が無ければ出力側を切り分ける。

削減案:

1. 同一ドメイン省略: 今回の実験版。速度・保護回帰を上層で確認。
2. RBAR/RLAR の事前計算: base の mask と limit の上丸めを context 作成時へ移す案。未実装。context の公開/内部 ABI と flash extent 粒度の維持が必要。
3. 変化したレジスタだけ更新: enable/disable 遷移や RBAR の再利用を条件付きにできるが、古い領域が一時的に有効になる更新順の確認が必要。未実装。
4. RBAR_A1.. の連続書込み: 主に複数領域更新向け。ここでは 1 領域なので大幅削減とは判断しない。RNR の選択グループを固定できれば RNR 1 書込みを省く余地。未実装。
5. ISB 除去: 今回は行わない。CONTROL / MPU の同期保証を確認してから判断する。Arm の [Cortex-M33 Generic User Guide](https://documentation-service.arm.com/static/66be2153882fec713ef49e49) は MPU 反映の同期として DSB と ISB または exception return を説明する。単純に不要と断定できない。

## 2. RAM min 悪化

- 旧 base は call_frame_push/pop に libc memcpy。旧 RAM ELF は context 120B を 1 語ずつ 30 回の load/store/index/compare/branch loop にしていた。GET の push (context + frame) / pop で複写が繰り返される。**配置だけでなくコピー実装も変わっており、SRAM の遅さだけには帰せない**。
- `internal_headers/shizuku/freestanding_mem.hpp:29-39`: 4 語単位に変更した比較版を作成。-Og の ELF は追加 register spill もあるため、速くなったとは断定しない。旧方式は cost2_profile、4 語化は cost2_copy (その後の cache/clock 変更前)。この 2 版で copy 変更を切り分けられる。
- `tools/test_hot_copy.cpp:21`: 0..256B、src/dst offset 0..7、前後 sentinel を全 byte assert。16,448 ケース × host -Og/-O2/-Os = 49,344 ケース、compile/run 全 exit 0 (`cost2_hot_copy.tests.log`)。ARM 実機証拠ではない。
- `rp2350/internal_headers/shizuku/boards/rp2350_pico2.hpp:45`: SDK timer_time_us_64 と同じ high/low/high coherency read を inline 化。schedule / sleep_us の flash call 3 箇所を除去。通常 GET/CALL の残存表は panic/error/cold handler の枝が中心で、33 件全てが benchmark の実行枝ではない。
- 現行 -Og ELF の検査は 33 → **30 flash edge**。全 svc 閉包の RAM 化は未完了 (panic / diag / DMA / boot 等)。`*.audit.log` を参照。
- SDK addressmap.h:34-38 が SRAM_STRIPED_BASE=0x20000000, END=0x20080000 と定義。現 ELF の hot code はこの範囲。code/data/stack 競合・非 striped 配置の効果は未測定。原因確定には copy 比較後に配置 A/B または競合負荷別の実機値が必要。veneer の位置・分岐は検査済みだが、待ち cycle は未測定。

## 3. -O2 / -Os

- -Os 旧ログは 4000 件送受信・loss=0・順序違反=0 と back-pressure=0 が同居。**LOSSLESS の破れを示すログではなく、満杯試験のタイミング依存が有力**。
- `source/selftest/stream_ladder.cpp:108-112`: consumer を producer の最初の full 拒否まで YIELD させてから drain。有限 guard と producer_done で打切り、実際の full hit を捏造しない。満杯を観測しない場合は元の assert が失敗する。修正版の実機 PASS は未確認。
- -O2 旧ログ最後の PASS の直後は depth restored の check、その後 identity checks、unknown API、stack exhaustion。旧ログは USB 行落ちがあるため停止 PC は分からない。
- `source/selftest/call_ladder.cpp:64-85,259-322`: [CALLDBG] stage を 10ms 間隔で表示し、既存 SELFTEST_PROGRESS watchdog/fault/tick 診断も有効化。
- **cost2_o2_diag_nofp.uf2 を推奨**。既存 DEEP_TRACE の __builtin_frame_address(0) が frame pointer を強制して不具合を隠す恐れがあるため、`call_ladder.cpp:44-49` のフラグで deep trace のみ無効化。元 O2 と新 nofp ELF の deep() はともに `push {r4}` (4B) の prologue を保つ (`cost_ram_o2.deep.dis`, `cost2_o2_diag_nofp.deep.dis`)。root caller の stack / 診断コード自体の摂動は残る。
- **-O2 原因は未確定・未修正**。必要な追加証拠は最後の [CALLDBG] stage、再起動後の [SELFTEST_PROG] tick sample / fault_pre の PC・LR・CFSR・PSP・PSPLIM。同梱 ELF に照合すること。deep trace を無効化したので deep-read 出力は原因判断に使わない。

stage: 800/801 = 6 段後の levels check、802/803 = identity/depth check、804 = 異常系前、805 = unknown API 前、806 = absent object 前、807/808 = stack exhaustion 前、809 = stack exhaustion から戻った後、810 = name 試験前。

## ビルドと成果物

全コマンドは `bazel --output_user_root=/private/tmp/shizuku-stream-bazel build ... //firmware:shizuku_uf2`。RP2350 は `--config=rp2350_pico2_w --config=svc_profile` を基本に使用。

- cost2_trace: -Og、cache off。現行計測版。
- cost2_mpu_skip_final: 上記 + `--copt=-DSHIZUKU_MPU_SKIP_SAME=1`。
- cost2_os_final: 上記 + `--config=opt_os`。決定的 full 試験。
- cost2_o2_diag_nofp: 上記 + `--config=opt_o2 --copt=-DSHIZUKU_SELFTEST_PROGRESS=1 --copt=-DSHIZUKU_CALL_DIAG=1 --copt=-DSHIZUKU_DEEP_TRACE_DISABLE=1`。
- cost2_rp2040: `--config=rp2040_xiao --config=svc_profile`。profile 無効。build exit 0。

全 build exit 0。既定 -Og / MPU cache off を維持。途中段階も cost2_*.uf2 / .elf / .build.log に保持。推奨版以外を混ぜて比較しないこと。

|UF2|SHA256|
|---|---|
|cost2_control.uf2|`40e33535a31fdb7fd660c4965e028ba1856005d85f7af0b036c9699bc4dedb6b`|
|cost2_copy.uf2|`177a6a691d37013256ade1a6a888e63bcdf0e9238fa672073da4c8cf647c4324`|
|cost2_hot.uf2|`0429759d9f64c055658f0817e711f9c5afe9f234836e2a03988e98c5f0d6d678`|
|cost2_mpu_skip.uf2|`2dde09ea6145eef79009b28176aae06630852c89837c4dbe8bc12c4acb6d889e`|
|cost2_mpu_skip_final.uf2|`64f5b2df7728bcdc5ea6afab46a259efdcb61ba4fd1d432dee7668a072d2abef`|
|cost2_o2_diag.uf2|`43073b1c19d13ddf2b6cb1d0a4459a0a5df657886bd10fadfeeb20baa04119a0`|
|cost2_os.uf2|`8416aea069944fe2ee9aeb5ee0a9e48a141ec8f9410030c3b76c5e3ff8f9ed2e`|
|cost2_os_final.uf2|`cc8ff2e9452f0215d8e75d49cc0a58239c05f27292a02668c97155a0b0993992`|
|cost2_profile.uf2|`b699723a7723443d50c332c78ff96248439209f4c07d08539ce7e11672abcf68`|
|cost2_rp2040.uf2|`d6e47b6e25a9375523db573bcac91095830347385a93c0c75dde8191227b3fcf`|
|cost2_trace.uf2|`c307d292783831b07f040f8da70f0e96479fc3ec1f83d57660de7e535aa76b7b`|
|cost2_o2_diag_nofp.uf2|`b047c2d35eded6bda18a578883a4626f0321a72ec35f819a9275d9e54b002297`|

`cost2_manifest.json` に各 ELF の arm-none-eabi-size、audit exit / 件数。`cost2_sha256.txt` に全 hash。検査スクリプトは 5 tests exit 0 (`cost2_audit.tests.log`)。上層が読む assert は `tools/test_check_svc_ram.py:23,30,36,45,51` と `tools/test_hot_copy.cpp:21`。stream 試験の受入は `source/selftest/stream_ladder.cpp:276` 付近の g_full_hits>0 と送受信・loss・順序の check を全て確認。

実機測定を行っていないため、新版の cycle / 全区間出力 / selftest 成功、MPU skip の保護安全性、-O2 の復旧は断定しない。上記ログが返れば同じ ELF で次の切り分けを継続できる。
