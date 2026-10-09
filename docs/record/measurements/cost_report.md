# svc RAM 化・計測: 途中報告

HEAD: `3239d3761fd1898b78411434a31f91790251bd3b`、branch `perf/svc-in-ram`。commit/push/実機操作なし。他 worktree 操作なし。既存未追跡設計文書・OTA 成果物は保持。

**受入未達: RAM → flash 呼出し 0 件ではない。下表 UF2 は部分 RAM 化版であり、完了版と扱わない。**

## 実装位置

- `modules/pico_sdk_support/rp2350/internal_headers/shizuku/archs/armv8m.hpp:8` / `rp2040/internal_headers/shizuku/archs/armv6m.hpp:8`: SHIZUKU_HOT、arch ヘッダ関数 always_inline。dummy は空定義。C6 は実装用 arch ヘッダがこの checkout に無いので未追加。
- `source/kernel/dispatch.cpp:56,70,119,122,148,159,198,229`: RAM 配置・文脈と call_request の libc-free copy。`source/kernel_object/handler.cpp:29,122,562`、`memory.cpp:38`: 全ハンドラ・メモリヘルパと request 初期化。
- `modules/pico_sdk_support/rp2350/armv8m_ctx.S:126,161`、`rp2040/armv6m_ctx.S:79,163`: SVC 入口と return stub。`rp2350/board.cpp:120,142`: 非特権 return stub 用の限定 RX region 3 (256B)。RP2040 は既存 data の実行許可を使用。
- `internal_headers/shizuku/freestanding_mem.hpp:24`: word copy の may_alias 型。-O2 の strict aliasing に備える。
- `internal_headers/shizuku/svc_profile.hpp:5`、`rp2350/arch_glue.cpp:176`、`source/selftest/call_cost.cpp:163`: per-core 256-event trace、128 回採取。記録の更新中のみ PRIMASK を保存・禁止・復元。全体コストは既存 2048 回 min/mean。
- `.bazelrc:65`: `opt_o2`、`opt_os`、`svc_profile` config。既定 -Og を維持。

## ビルド・検査

すべて `bazel --output_user_root=/private/tmp/shizuku-stream-bazel build`。RP2350: `--config=rp2350_pico2_w --config=svc_profile [--config=opt_o2|opt_os] //firmware:shizuku_uf2`。RP2040: `--config=rp2040_xiao --config=svc_profile //firmware:shizuku_uf2`。全ビルド終了 0。個別 `*.build.log` を参照。

baseline は HEAD に計測変更だけを一時適用してビルドし、RAM 化ソースを復元した。`cost_base.source.patch` が差分証跡 (新規共通ヘッダ svc_profile.hpp は現行ファイルを共有)。同じ timestamp hook・計測ループで比較。

リンカ map 取得のため RP2350 3 版を relink。既存 UF2 と SHA256 が同一と確認 (`*.map-build.log`)。

|UF2|build exit|SHA256|
|---|---:|---|
|cost_base.uf2|0|`2433cd2fd08e1ad525835db810bed28bbd3ea64b0669b4b0848c135f0917d7de`|
|cost_ram.uf2|0|`fda16f2009384053c431515e5a36d20233674a1aef949fa7789f704d18996df9`|
|cost_ram_o2.uf2|0|`6a06eee2c546ffa8f1ae48e97bd4c4f90d67e7420aa3b472e359ea423f7f19db`|
|cost_ram_os.uf2|0|`9e28c0dc50b96c5e9d487d54bec650bb394905b5c9498177e468d12f7ae2f133`|
|cost_ram_rp2040.uf2|0|`9a27b637fdb52636c9cb1a118f6a8768a837a5e241f6c336764c491876393b68`|

`python3 tools/check_svc_ram.py --require-entry ELF` は各 RAM ELF で exit 1 (未達)。

|ELF|監査 RAM 関数|flash edge|未分類 BLX|
|---|---:|---:|---:|
|cost_ram|98|33|0|
|cost_ram_o2|95|38|0|
|cost_ram_os|97|37|0|
|cost_ram_rp2040|97|34|0|
|cost_ram_noprofile|97|33|0|

残存先は `BOARD::panic` / `diag_printf`、`time_us_64`、DMA claim/copy/busy/release、boot/start-secondary の SDK 処理など。例外・初期化系も一部配置したので検査数には svc 本体以外を含む。ただし svc_dispatch の panic や stream_connect の DMA claim は実際の svc 経路に残る。詳細は `*.audit.log`。

checker は直接 branch と RAM veneer の flash literal を検出し、未知 BLX と監査 0 関数を成功にしない。動的 callback と flash rodata を完全証明するものではない。`--all-ram` も実行: RAM 関数 202 / flash edge 52 / 未分類 BLX 28、exit 1 (`cost_ram.all-ram.audit.log`)。SDK/OTA の既存 RAM コードも含む。

`python3 tools/test_check_svc_ram.py`: 5 tests、exit 0。上層の監査箇所: `tools/test_check_svc_ram.py:23,30,36,45,51` (空入力拒否、RAM call 成功、flash call 拒否、veneer 拒否、未知 BLX 拒否)。

RP2350 profile 無しおよび RP2040 profile flag 有り ELF の `arm-none-eabi-nm ... | rg svc_profile` は 0 件 (rg exit 1)。`*.profile-symbols.log`。RP2350 profile 無し BSS は有りから 4112B 減少 = event buffer 4096B + count/enabled 16B。

## サイズ (byte)

GNU size は RAM .data が CODE 属性なので text に含める。data=0 は RAM 使用量 0 の意味ではない。`.time_critical*` は SDK の出力 .data に統合されるため map の生存 input section の合計を集計 (整列 padding / veneer は別、.data はそれらを含む)。

|版|size text|size data|size bss|.data|.bss|time_critical input|
|---|---:|---:|---:|---:|---:|---:|
|cost_base|387660|0|106944|7872|104624|2784|
|cost_ram|388572|0|106944|21232|104624|15996|
|cost_ram_o2|392292|0|106932|22928|104612|17568|
|cost_ram_os|372820|0|106904|19536|104584|14140|
|cost_ram_rp2040|100800|0|91128|33784|88888|未取得|
|cost_ram_noprofile|387540|0|102832|21224|100512|15796|

RP2350 baseline → RAM (-Og): .data +13360B、time_critical input +13212B。RAM の固定予約 (.data + .bss + vector + heap + 両 core stack) は 118912 → 132272B。アプリ側動的利用量を実測した値ではない。`cost_sizes.json` と `*.size.log`。

-O2 は build 可、RAM -Og 比 size text +3720B / .data +1696B。-Os は build 可、同 text -15752B / .data -1696B。速度・selftest 意味論の実機成立は未確認。inline asm は明示レジスタ + volatile + memory clobber、word copy は may_alias 化したが、全ソースの UB 監査は未完了。カーネルだけ -O2 の config は未追加。既定変更はしない。

## 上層の実機確認

既存: `[COST] svc round trip ... min N cyc (...) mean M cyc [raw min R]`、`method call (1 hop)` / `(2 hops)`。

追加: `[COST] profile svc|call1|call2 to=LABEL min=N mean=M cyc samples=128 overflow=0`。

`LABEL`: caller_pre, exception_entry, context_saved, dispatch, push_begin, push_end, handler_begin, handler_end, return_stub, caller_resume, mpu_begin, mpu_end, restore_end。連続する timestamp の差を「到着先 label」で区分し、ネストを含め 1 operation 内で合計する。各区間には hook overhead / IRQ が含まれ、各区間の独立 min は足し合わせ不可。return_stub の hook は非特権時に実行しない。caller timestamp は benchmark API のみ。

上層は `source/selftest/call_cost.cpp:284` の error==OK && value==42 に対応する `[COST] PASS method call still returns 42` と全 selftest 成否、非特権メソッドの return、call_ladder の stack limit 回帰を確認すること。ここでは一切実行していない。

## 再開点

ゼロ件達成には時刻/DMA wrapper・SDK helper・診断/panic の関数閉包を RAM に置き、ELF 検査を再実行する必要がある。診断経路は USB polling callback を含むので、関数ポインタ先も個別に確認する。RAM 化による追加 stack 消費と MPU region の実機確認も未完。`cost_*` は計測用途中成果であり受入済みではない。

別案 (未実装・未検証): SDK を含む executable .text だけを RAM にコピーし、大きい .rodata は flash に残すリンカ構成。設計文書の全バイナリ copy_to_ram とは RAM 消費が異なる。crt0/初期コピーコードの flash 残置、MPU、間接関数先と RAM 使用量の検証が必要。
