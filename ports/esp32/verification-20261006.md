# ESP32-C6 compile/link-only 検証記録 (2026-10-06)

起動しても何も動かさない compile/link-only の記録。実機・実行時の動作は未検証。
Bazel の主線 (`//firmware:esp32c6`) は **未達** (下記 §1)。

## 1. Bazel 主線 (未達)
- `bazelisk --batch --output_user_root=/tmp/claude/bazel --host_jvm_args=-Djdk.http.auth.tunneling.disabledSchemes= build --config=esp32c6 //firmware:esp32c6`
  - exit 32。`Failed to fetch registry file https://bcr.bazel.build/modules/bazel_skylib/1.6.1/MODULE.bazel: Unable to tunnel through proxy. Proxy returns "HTTP/1.1 403 Forbidden"`。
  - サンドボックス違反は `deny network-outbound bcr.bazel.build:443 (user denied)`。環境 (権限) 起因で、コード起因ではない。
  - ログ: `/tmp/claude/bazel_main.log` (最後の実行分のみ)。
- サンドボックス外実行 (`dangerouslyDisableSandbox`) は `Run outside of the sandbox` で 2 回拒否された。迂回していない。
- `--output_user_root` を worktree 内 (`build/bazel`) にすると repo contents cache が workspace 内になりエラー。`/tmp/claude/bazel` を使う。
- Bazel 側のソース (`toolchains/rv32_esp/`、`//:shizuku_kernel`、`//firmware:esp32c6`、`.bazelrc` の `--config=esp32c6`) は **一度も解析されておらず unverified**。
- 再開: `bcr.bazel.build` (と GitHub のアーカイブ) の読取が実際にこのセッションで許可された状態で上のコマンドを再実行する。

## 2. 手動で生成した成果物 (Bazel 経由ではない)
- `.a`: `riscv32-esp-elf-g++` (esp-15.2.0_20251204) で 6 ソースを手動コンパイルし `ar rcs` (`/tmp/claude/rvprobe.sh`, `/tmp/claude/rvlib.sh`)。
  - `/tmp/claude/rv/libshizuku_kernel.a` SHA256 `1f19fceba7862a14137e85d59ab25f5e5a9a45b5c7b9568dcf87002c610b6745`
  - メンバー 6: c6_link_stubs.o / cpu_manager_init.o / kernel_dispatch.o / kernel_init.o / kernel_thread.o / memory_manager_freestanding.o
- IDF: ESP-IDF v6.1 + GCC 15.2.0 (esp-15.2.0_20251204) で `idf.py set-target esp32c6` / `build` が rc=0 (`/tmp/claude/idfbuild.sh`, ログ `/tmp/claude/idfbuild.log`)。
  - `.local/build/esp32c6/shizuku_esp32c6.bin` 160160 B SHA256 `adb249b462ff969abe82ada3fddf28f6822aed4ac4540cde66c04a00c73af88e`
  - `shizuku_esp32c6.elf` 3803848 B SHA256 `7e1ce06b9ff5371f0178ba436f7b812ef490dce22a41583f7b8c69ac76ee3f82`
- `firmware/esp32c6/build.sh` の単体実行 (`/tmp/claude/postest.sh`): exit 0、`/tmp/claude/shizuku_esp32c6.bin` 160160 B SHA256 `6216a8fb55cb0663a81648d1c581579905a1b670fabb576d99f422dc7a7e126e` (パス埋め込みで SHA が上と異なる)。Bazel の action からは未実行。
  - 異常系: `IDF_PATH` 不正と GCC 版不一致はいずれも理由を出して exit 1 (`/tmp/claude/negtest.sh`)。

## 3. テスト (直接実行。Bazel の test ではない)
- `firmware/esp32c6/test_image.sh` 21 assert (check 行 38-72) すべて PASS、`fails=0`、exit 0。
- 変異確認: `libmain.a` を渡すと A1-A5 の 5 件が FAIL、exit 1。
- 証明するのは「kernel の実コードが ELF にあり、`start_cpu0` が `ebreak` で、スケジューラ関連シンボルが ELF に無い」ことだけ。

## 4. 未実装・未検証
- `ports/esp32/c6/link_stubs.cpp` の 26 関数 (arch 13: 22-36 行、board 13: 44-56 行) はすべて即トラップ。カーネルは動かない。
- 実機での起動、PMP/trap、IDF の private ABI (`start_cpu0` 差し替え) の安定性は未検証。

## 5. `build.sh` の相対出力パスのバグ修正 (上層レビュー差戻し)
- バグ: Bazel の genrule は出力 `$@` を相対パスで渡す。旧 `build.sh` は `OUT=$2` / `ARCHIVE=$1` のまま、
  `mktemp -d "$OUT.idf..."` を元の cwd で作り、`cd "$SRC"` の後に `-B "$BUILD_DIR"` と `cp ... "$OUT"` を使っていたため、
  相対パスが別の場所を指した。絶対パスを渡す手動の `postest.sh` では露見しない。サンドボックス回避とは無関係の実バグ。
- 修正 (`firmware/esp32c6/build.sh:16-22`): `cd` の前に `ARCHIVE` / `OUT` / `SRC` を絶対化 (`abspath` = `os.path.realpath`。
  存在しない出力にも対応)。出力ディレクトリは `mkdir -p` (同 `:42`、`cd` は `:44`)。
- 旧挙動の mutant (`/tmp/claude/build_mut.sh`: `OUT=$2`, `ARCHIVE=$1`) を `/tmp/claude/wt2` から相対パスで実行 → exit 1、`.bin` なし
  (`set-target.log` が見つからず失敗。`/tmp/claude/mut_run.sh`)。バグの再現と、テストが捕まえることを確認。
- 検査: `firmware/esp32c6/test_wrapper.sh` を `/tmp/claude/wrap_test.sh` から実行 → exit 0、`fails=0`、ログ `/tmp/claude/wrap_test.log`。
  - positive (行 58-66、P5/P6 は §6): P1 相対 ARCHIVE + 未作成ディレクトリ配下の相対 OUT で exit 0 / P2 cwd 相対の位置に .bin / P3 先頭 0xE9 / P4 スクリプトディレクトリに迷い込まない。
    生成物 `/tmp/claude/wt/out/rel.bin` 160160 B SHA256 `75656b961003231f7ae40a7f399196d88fb2fa4d9b677c6476d3093fe52056d4`。
  - negative (行 69-77): N1 `IDF_PATH` 欠落 exit 1 / N2 `IDF_TOOLS_PATH` 欠落 exit 1 / N3 GCC 版不一致 exit 1 + メッセージ確認 / N4 アーカイブ欠落 exit 1。
    いずれも `.bin` を作らない (N1b-N4b)。
  - 注: 欠落 env の失敗は `set -u` による bash のエラーで、専用メッセージではない (終了コードと .bin 非生成のみを確認)。
- これは Bazel の action 経由ではない。Bazel の一括ビルドは §1 のとおり未解析・未成功のまま。

## 6. `sdkconfig` / `sdkconfig.old` がソースツリーに出る副作用の修正
- 問題: `idf.py set-target` / `build` がプロジェクトディレクトリ (`firmware/esp32c6/`) に `sdkconfig` / `sdkconfig.old` を生成し、
  Bazel action の入力 source tree を汚していた。
- IDF の確認: `.local/esp-idf/tools/cmake/project.cmake:101-102` が外部指定の `SDKCONFIG` (絶対パスに解決) を使う。
- 修正 (`firmware/esp32c6/build.sh:46-47`): `set-target` と `build` の両方に `-DSDKCONFIG="$BUILD_DIR/sdkconfig"` を渡す
  (`.old` も同じ場所に出る)。コメントは `:43`。
- 既存ファイルの保護: 作業開始時点でソース側に `sdkconfig` は無かった (自分の過去の実行が作ったものだけを削除済み)。
  テストは実行前後の存在/SHA を比較するので、既存ファイルがあっても削除・上書きしない。
- 追加 assert (`firmware/esp32c6/test_wrapper.sh`): `:63` P5 ソース側の `sdkconfig` / `sdkconfig.old` が作られず変更もされない
  (`snap` は `:50-54`、実行前状態は `:55`) / `:64` P6 `sdkconfig` が `out/rel.bin.idf.*/` 配下に生成されている。
- 実 IDF で `/tmp/claude/wrap_test.sh` を実行 → exit 0、`fails=0` (P1-P6、N1-N4 と各 N*b、N3c すべて PASS)、ログ `/tmp/claude/wrap_test.log`。
  実行後の `firmware/esp32c6/` は `CMakeLists.txt build.sh main sdkconfig.defaults test_image.sh test_wrapper.sh` のみ。
  生成物 `/tmp/claude/wt/out/rel.bin` 160160 B SHA256 `c0a410cbbb222d270cea51c95765b930aeccae0dd24bc2e6f83439e56a313c6f`
  (ビルドディレクトリのパスが埋め込まれるため、実行ごとに SHA が変わる)。
- Bazel の一括ビルドは §1 のとおり未解析・未成功のまま。これも Bazel action 経由の実行ではない。

## 7. C6 pre-init USB Serial/JTAG entry (2026-10-06、診断candidate。未ビルド・未実機)

§1–6 は今回の編集前に記録された compile/link-only 段階の履歴。今回の source tree は `start_cpu0` から archive の
`shizuku_c6_early_boot` を呼ぶ形へ更新したが、Bazel / IDF build / ELF 検査 / 実機起動はまだ実行していない。

### 段階表 (V2 kernel / FreeRTOSなし)

| 段階 | 完了条件 | 現在の状態 |
|---|---|---|
| 1 | 診断候補のsource準備後、上層がreset cause・marker・USB安定性・post-gateを実機確認 | source候補は準備済み。fresh build / image assert と cause・marker・安定USB・post-gate実測は未完了。code準備とhardware受入を分けて扱う。 |
| 2 | V2 `KERNEL::init` 完了、最初のthread、および複数timer context switchの出力を実機確認 | 未着手。既存26 stubsが残る。 |
| 3 | C6 meaningful thread / call ladder等のselftestをnamed PASS、件数記録、failure 0で実機確認 | 未着手。 |

この作業ではV3 / BLE / OTAを扱わない。既存OTA planにあるFreeRTOS前提はV2の採用仕様として確定していない。この注記のためにOTA plan文書は変更しない。

- SDK一次確認: `.local/esp-idf/components/esp_hal_usb/esp32c6/include/hal/usb_serial_jtag_ll.h:126-184`
  は TX FIFO `write_txfifo`, `txfifo_writable`, `txfifo_flush` を定義する。`write_txfifo` は実書込数を返し、
  `txfifo_flush` は FIFO を host が取得可能にする。同 header の `SERIAL_IN_EMPTY` status は host が packet を取得した
  completion event として SDK driver (`esp_driver_usb_serial_jtag/src/usb_serial_jtag.c:61-70`) が扱う。実装はmarker全byteをFIFOに
  書いた後、event clear → fence → flush → fence → poll の順で処理する。poll上限は経過時間ではなくiteration数。USB clock/PHY/pins
  は再設定しない。実機出力は未検証。
- completionの境界: `SERIAL_IN_EMPTY` はUSB Serial/JTAG IN endpointが空になったhardware statusで、SDK driverのコメント (`components/esp_driver_usb_serial_jtag/src/usb_serial_jtag.c:61-70`) はhostがdataを取得したeventと説明する。
  これはUSB controller endpoint completionであり、host terminal/applicationがbyteをread/displayした証拠ではない。公式v6.1 docsはESP側のsmall bufferとhost memoryにdataが残り得る場合を記載する。
  [USB Serial/JTAG console / data buffering and sleep](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c6/api-guides/usb-serial-jtag-console.html)
- WFI仮説順位: 旧WFI imageのPC `0x4200e1a8` はhelper成功側だったとの上層観測。ただし報告SHAはprefixのみ。今回のnop candidateは完全SHAを確認済みで、PC `0x4200e1ae/0x4200e1b0` はそれぞれactive loopの`early_boot.cpp:24/23`へ対応する。
  NOPでも約1.9秒でUSB drop、marker 0という上層結果によりWFI単独説は弱まった。一方、その時のreopen後reset code `0x15` はUSB UART resetと定義され、serial reopenが原因を上書きした可能性があるためautonomous reset原因は未確定。
  ESP-IDF docsがUSB停止を明記するのは`esp_light_sleep_start()`等のLight-sleep / PM automatic Light-sleepで、生のRISC-V WFIをLight-sleepと同一視する根拠はない。IDF v6.1 `components/esp_pm/pm_impl.c:863-885` はFreeRTOS `vApplicationSleep`経由でLight-sleep APIを呼ぶ。
  **次に判定する候補**は、no-serial区間に増えるnoinit boot sequenceとraw reset reason、RWDT/MWDT/Super-WDT configのtimeout/stage設定。その後にUSB/host timingを分離する。
- USB init境界: IDF driver install (`components/esp_driver_usb_serial_jtag/src/usb_serial_jtag.c:200-211`) はbus clock enableとPHY setupを行う。このearly pathはLL FIFOのみを使い、その初期化を呼ばない。helperが今回一度host-side endpoint emptyまで到達した観測があるため、初期化追加を診断candidateと同時に行わない。
- 直近に実機試験された旧diagnostic候補 (noinit WDT record導入前) はbootloader WDT有効、timeout 9000 ms。ELF SHA-256は
  `d8435cb7f2d9d1ff8c7a0f737523ab1b3f744594a04ef172739cb1f536e4a197`、app bin SHA-256は
  `e435a0773a9f785ebc295f75749dbcdd4155e6ee8acb8fb09fa5a2fc7bc06716` と一致確認済み。対応sdkconfigは
  `CONFIG_BOOTLOADER_WDT_ENABLE=y`, `CONFIG_BOOTLOADER_WDT_TIME_MS=9000`, `CONFIG_PM_ENABLE` unset。
  上層reportのresetは約1.9秒周期、marker count 0。SavedPC `0x4200e1ae/0x4200e1b0`はこのELF内nop loopにsource/objdump照合済み。旧WFI imageの完全hashは不明。
- 今回のnoinit ring / WDT snapshotとRTC-WDT停止を含む候補はsource未buildであり、上記hashのartifactとは別物。候補では従来のsnapshot→RTC-WDT停止→after snapshot順を維持し、Super-WDT/TIMG WDTは停止・給餌・設定変更しない。
- 上流startupはcustom hookより先にwatchdog stateへ触る。`cpu_start.c:833` は`esp_clk_init()`を呼び、C6 `clk.c:61-91` はbootloader RTC WDTをfeedし、slow clock切替中のtimeoutを一時設定した後 `CONFIG_BOOTLOADER_WDT_TIME_MS` に再設定する。`cpu_start.c:983-1018` はその後`init_bss` / `sys_rtc_init` / `system_early_init`を経て`SYS_STARTUP_FN()`へ進む。noinit logはoverrideされた`start_cpu0` hook entry時点でのraw reasonとWDT registersであり、flash/ROM直後の初期configではない。Candidateはhook内でRWDT pre-stop snapshot後に従来のHAL stop手順を行い、post-stop config0を記録する。`startup_funcs.c:133-139` の通常secondary startup関数自体はこのoverride pathでは実行されない。
- Reset reason: custom hookは`esp_rom_get_reset_reason(0)`のraw codeを記録する。C6 `soc/reset_reasons.h:35-47` では `0x07/0x08`=MWDT0/1 system、`0x09/0x10/0x0D`=RTC WDT各reset level、`0x12`=Super WDT、`0x0F`=brownout、`0x15`=USB UART、`0x16`=USB JTAG。`0x15`はreopen由来とも整合し、no-serial ringで再判定する。
- Snapshot timing: IDF `cpu_start.c:833` はcustom hookより前に`esp_clk_init()`を呼び、C6 `clk.c:61-91` はRTC WDTをfeedしてslow-clock切替中のtimeoutを一時変更後、configured timeoutへ戻す。`call_start_cpu0` は`init_bss`/`sys_rtc_init`/`system_early_init`後に`SYS_STARTUP_FN()`を呼ぶ (`cpu_start.c:983-1018`)。noinit ringのWDT configはhook entry時の値で、ROM/bootloader直後の初期状態ではない。ROM raw reason取得時点までにhardware reasonが変更されていないかは実機記録と照合する。
- noinit record: candidateの`early_diag.h`はversion 2、4-entry ring、376 bytes。header offset 0/4、recordsはoffset 8から92-byte stride。record offsets: sequence 0、raw reason 4、RWDT config0-4 8–24、MWDT0 config0-4 28–44、MWDT1 config0-4 48–64、Super-WDT config 68、writer status 72、checksum 76、TIMG0 flags 80、TIMG1 flags 84、post-stop RWDT config0 88。`early_diag.c` のcompile-time assertionsでlayout/sizeを固定する。
  `esp_attr.h:152-153` の`__NOINIT_ATTR`で`.noinit`へ置き、linker `ld.dram.sections:71-89` はNOLOAD `.noinit`をBSS clear範囲と分けている。magic/version/checksumで未初期化/破損記録を捨てるが、full power loss/brownout後のDRAM保持は保証しない。
  Candidate checksumは`MAGIC ^ VERSION ^ 0xA71D39C5 ^ record.sequence ^ raw_reset_reason ^ 全WDT raw config words ^ super_wdt_config ^ writer_status ^ timg0_status ^ timg1_status ^ rwdt_config0_after_stop`。checksum field が0のslotはinvalid/in-progress。計算結果0は`0xFFFFFFFF`へ写像。ring slotは`sequence % 4`で選び、valid recordの最大sequence+1を使う。checksumはXORによるtorn/accidental corruption検出の目安で、暗号学的完全性ではない。partial writeで有効に見える可能性も排除しない。
  RWDT `config0`の`wdt_en` bit31/flashboot bit12、TIMG `wdtconfig0`のenable bit31/flashboot bit14、Super-WDT `swd_config`のdisable bit30/auto-feed bit18/reset flag bit0をraw wordから読む。Candidate flagsはbit0=APB enabled、bit1=reset released、bit2=WDT clock enabled、bit3=TIMG WDT registers actually read。PCR `tg*_rst_en` はactive-high reset (`0`=released) なのでflags bit1はそのbitが0の場合に立つ。APB clock / reset / WDT clockは読み取りのみ。いずれかgateがclosedならTIMG register群は読み込まず、5 wordsを`0xFFFFFFFF`にする。gate/statusがunknownなら原因分類もunknown。
- OpenOCD observer effects: local OpenOCD v0.12.0-esp32-20260703 `target/esp_common.cfg:239-252` のexamine-endはoption/semihost設定のみでreset/WDT writeをしない。`esp_common.cfg:367` はRISC-V memory access優先順を`sysbus progbuf abstract`にする。一方、同 `:279-285` の`halted` eventは`_ESP_WDT_DISABLE`を呼び、C6 target config `target/esp32c6.cfg:31-58` はTG0/TG1/RWDT/SWDをdisableしinterrupt状態をclearする。`esp32c6_soc_reset` `:61-90` はHPSYS/CPUをsoftware resetする。GDB attach `esp_common.cfg:307-325` はhaltし、memory protection判定次第で`reset halt`も行う。
  no-reset観測はbuiltin ESP USB-JTAGでGDB/TCL serverを無効、Telnetを別portにし、reset/GDB attachなしでstartする。最初に`mdw <DIAG_ADDR> 94`でrunning targetのnoinit logを読む。`mdw`がrunning stateで失敗したらhalt前の記録はunknownとして止める。RAM snapshot後にPC readのためhaltする場合、default halted callbackがWDTをdisableするため、それ以後のwatchdog挙動は観測値に含めない。
- 実装: `start_cpu0` はC側`shizuku_c6_early_diag_begin()`でreset/WDT recordを開始後、archive entryへ移る。`early_boot.cpp` はC ABIだけを使い、`usb_console.c`のLL writer結果をC diag ringへ記録する。USB routeのclock/PHY/pinsおよびTIMG/Super-WDTは変更しない。RWDTはpre-stop snapshot後にHALで停止する。
  `KERNEL::init` / `kernel_instance` / ctor / libc / heap / PMP / trap setup は呼ばない。FreeRTOS schedulerも始めない。RWDTだけ従来の起動前提どおりraw pre-stop snapshot後にHALで停止し、post-stop config0を別wordへ記録する。TIMG/Super-WDTをfeed/disable/clearしない。
- 今回のdiagnostic candidateはWFIを使わず両writer結果をactive `nop` loopへ集約する。ringはresetごとにsequence付きのraw reasonとWDT raw config、writer statusを4件保持する。checksum不正/古いmagic/versionは破棄する。RAM記録はTTY表示証明ではなく、通常BSSとは別でも全reset種別に対する保持保証はない。
  2026-10-07 の実機dumpは `0x4080af6c` 先頭が `0xb09b4201`、残り93 wordsが0でmagic不一致だった。指定fresh ELFの `start_cpu0` は `shizuku_c6_early_diag_begin` を呼んでいたが、旧関数の最初の処理は `esp_rom_get_reset_reason(0)` であり、そのROM呼出しが返るまでringには書かなかった。従ってこのdumpだけではhook未到達、ROM helper内停止/例外、RAM read/retention問題を区別できない。候補修正ではROM call前に `SHIZUKU_C6_EARLY_DIAG_ENTRY_MAGIC=0x5348454e` をvolatile noinit先頭へ書き、`fence rw,rw` の後にROM helperを呼ぶ。次の実機dumpでこの値ならhook到達後ROM call前後の停止を疑う材料になるが、read-path sanityを既知RAM wordと合わせて確認する。
  予測:最新有効recordの`writer_status=1`はLL endpoint eventまで戻ったこと、marker表示はTTY側の別証拠。nop candidateでもno-serial resetが続けばWFI単独原因を弱める。`writer_status=2`ならhelper falseでidle原因は判定不能。sequence増加はhookを複数回通った証拠だが、ring欠落はhook前reset/noinit retention lossもあり得るためreset不在とはしない。OpenOCD halt後やintentional RTS pulse中のresetは別扱い。
- kernel archive の `early_boot.o` からIDF main componentへの参照はarchive公開シンボル一覧には現れないため、
  `firmware/esp32c6/main/CMakeLists.txt` は `--undefined=shizuku_c6_usb_console_write_and_flush` を追加link rootにし、
  link順で先に来る `libmain.a` から `usb_console.c.obj` を抽出してGCから保持する。
- 未実装 stub: `ports/esp32/c6/link_stubs.cpp` の既存 26 関数 (arch 13、board 13) は維持し、USB 出力のための
  no-op stub は追加しない。実際にその未実装経路が呼ばれた場合は引き続き trap する。
- `firmware/esp32c6/test_image.sh` の D2-D17 はstart_cpu0の診断記録順、raw-reason captureのELF参照、early diagnosticのELF/map provenance、USB writer/route、noinit log、active nop、early entry内WFI不在、marker、
  `g_startup_fn[0]` を検査する。C4 は archive member ごとに strong symbol を map と ELF の双方で照合する。
  host fixture は重複 `init.o` の正負例に加え、空白を含むC++ template specializationのdemangle名を確認する。
  `test_build_env_fixture.py` と `test_archive_members.py` は host synthetic fixture、Bazel / IDF build / `test_image.sh` は未実行。

### 上層で行うビルド・実機確認

この手順は未実行。Sonnet は各書込みの直前に gate を必ず再実行し、chip type と MAC が一致しない場合は停止する。
USB Serial/JTAG は port `/dev/cu.usbmodem1101` のみ対象。Pico 101 / 103 は開かない。erase-all と eFuse は行わない。
この実機手順内ではcommitしない。受入後のcommitは上層が別途行い、push/PRは行わない。

build wrapper が `IDF_PATH` と `IDF_TOOLS_PATH` を `cd` より前に絶対化し、自身で `$IDF_PATH/export.sh` を source する。
最初のbuildはsourceしていない新しいshellで相対値をinline指定し、build action自身の環境修復を独立に確認する。

```bash
set -euo pipefail
cd /Users/ishigakiyua/github/Shizuku-esp32
if IDF_PATH=.local/esp-idf IDF_TOOLS_PATH=.local/idf-tools bazelisk build --config=esp32c6 //firmware:esp32c6; then
  BUILD_RC=0
else
  BUILD_RC=$?
fi
printf 'bazel build exit=%s\n' "$BUILD_RC"
if [ "$BUILD_RC" -ne 0 ]; then exit "$BUILD_RC"; fi

# 以降のinfo / test_image / esptool手順が使う親shell環境を設定する。
export IDF_PATH=/Users/ishigakiyua/github/Shizuku-esp32/.local/esp-idf
export IDF_TOOLS_PATH=/Users/ishigakiyua/github/Shizuku-esp32/.local/idf-tools
. "$IDF_PATH/export.sh"
```

相対値で失敗した場合は新しいshellで同じbuildを絶対値でも実行し、そのexit codeを別記する。最初の相対buildが失敗したら
そのfailureを記録し、`BUILD_RC`が非0のためこのblockは後段へ進まない。host fixture は相対値 + cwd 変更と
post-export Python 欠落を確認するが、実 SDK build の代わりにはならない。

`//firmware:esp32c6` の declared output は `bazel-bin/firmware/shizuku_esp32c6.bin`。
Build 成功後、出力と byte-for-byte 一致する IDF build directory だけを候補にする。mtime 最新だけで選ばない。
Bazel cache hit などで一致する IDF build directory が無い、または複数一致する場合は出所不明として停止する。
古い手動 image / 別 build の ELF・map・flash args を代用しない。`build.sh` は Bazel `.bin` の隣に `.bin.idf.*` directory を作る。

```bash
set -euo pipefail
BAZEL_BIN="$(bazelisk info --config=esp32c6 bazel-bin)"
BIN="$BAZEL_BIN/firmware/shizuku_esp32c6.bin"
MATCHES=()
for d in "$BAZEL_BIN"/firmware/shizuku_esp32c6.bin.idf.*; do
  [ -d "$d" ] || continue
  if cmp -s "$d/shizuku_esp32c6.bin" "$BIN"; then MATCHES+=("$d"); fi
done
[ "${#MATCHES[@]}" -eq 1 ] || { echo "Expected exactly one IDF build matching Bazel output; stopping" >&2; exit 1; }
BUILD_DIR="${MATCHES[0]}"
test -s "$BAZEL_BIN/libshizuku_kernel.a"
test -s "$BUILD_DIR/bootloader/bootloader.bin"
test -s "$BUILD_DIR/partition_table/partition-table.bin"
test -s "$BUILD_DIR/shizuku_esp32c6.elf" && test -s "$BUILD_DIR/shizuku_esp32c6.map" && test -s "$BUILD_DIR/shizuku_esp32c6.bin"
test -s "$BUILD_DIR/flash_args" && test -s "$BUILD_DIR/flasher_args.json"
cmp -s "$BUILD_DIR/shizuku_esp32c6.bin" "$BIN" || { echo "Bazel / IDF app binary mismatch; stopping" >&2; exit 1; }
shasum -a 256 "$BUILD_DIR/shizuku_esp32c6.elf" "$BUILD_DIR/shizuku_esp32c6.bin" "$BIN"

TC="$IDF_TOOLS_PATH/tools/riscv32-esp-elf/esp-15.2.0_20251204/riscv32-esp-elf/bin"
firmware/esp32c6/test_image.sh \
  "$BAZEL_BIN/libshizuku_kernel.a" \
  "$BUILD_DIR/shizuku_esp32c6.elf" \
  "$BUILD_DIR/shizuku_esp32c6.map" \
  "$BUILD_DIR/shizuku_esp32c6.bin" \
  "$TC" || { echo "test_image.sh failed; stopping" >&2; exit 1; }
```

`test_image.sh` の実行は上層の fresh Bazel build 後に行う。この手順・assert は今回まだ実行していない。
保存PCを調べる場合はraw PCと完全な ELF SHA-256 を対で保存し、同じbuild directoryのELFに対して
`"$TC/riscv32-esp-elf-addr2line" -e "$BUILD_DIR/shizuku_esp32c6.elf" -f -C "$SAVED_PC"` を実行する。
報告済み `0x4200e1a8` は以前のWFI imageのPCであり、今回のnop candidateへ流用しない。以前のimageのSHAはprefixしか報告されていないため、完全値は補わない。

各書込みの直前に次を実行し、出力の chip が ESP32-C6、MAC が `58:E6:C5:12:E3:98` と完全一致した場合のみ続行する。

```sh
esptool --chip esp32c6 --port /dev/cu.usbmodem1101 --before default-reset --after no-reset chip-id
```

一致しない、応答しない、port が再列挙されない場合は即停止。合致時のみ、同一 IDF build directory の `flash_args` に
従って bootloader / partition table / app を書く。IDF の生成した `flash_args` は同 build directory が cwd の状態で使う。
`flash_args` が存在し内容を確認した場合のコマンド形は次のとおり。erase-all と eFuse 操作は含めず、書込みのたびに
上記 chip-id gate をやり直す。同一 `flash_args` に記載されたbootloader・partition table・applicationの3領域について、
書込み先offsetと各領域の書込み/verify結果をログで照合し、3領域すべての成功を記録する。

```sh
cd "$BUILD_DIR"
esptool --chip esp32c6 --port /dev/cu.usbmodem1101 --before default-reset --after no-reset write-flash @flash_args
```

### A. Serial observerなしのreset原因観測

fresh build後、各flashの直前に前記MAC gateを実施し、同一`flash_args`のbootloader・partition table・application 3領域の書込み/verify結果を保存する。`write-flash --after no-reset` の後はESPがROM bootloader待機状態なので、これを安定稼働とみなさない。対象アプリを一度だけ起動する。

```bash
RUN_STDOUT="$PWD/esptool-run.stdout.log"
RUN_STDERR="$PWD/esptool-run.stderr.log"
if esptool --chip esp32c6 --port /dev/cu.usbmodem1101 --before no-reset --after hard-reset run >"$RUN_STDOUT" 2>"$RUN_STDERR"; then RUN_RC=0; else RUN_RC=$?; fi
T0="$(date '+%Y-%m-%dT%H:%M:%S%z')"
printf 'RUN_EXIT=%s\nT0_HARD_RESET_COMPLETE=%s\nstdout=%s\nstderr=%s\n' "$RUN_RC" "$T0" "$RUN_STDOUT" "$RUN_STDERR"
cat "$RUN_STDOUT"
cat "$RUN_STDERR" >&2
if [ "$RUN_RC" -ne 0 ]; then echo "app launch failed; stop with reset cause unknown" >&2; exit "$RUN_RC"; fi
```

ここでT0は、esptool `run` がhard-resetとport closeを終えて終了した直後にhost側で記録する。SDK実装上 `--after hard-reset` はRTS control lineによるhard reset、`run` はflash上のappを実行する操作で、teardownで `--after` のreset処理を行う (`site-packages/esptool/__init__.py:417-429,656-682,867-873`; `cmds.py:2776-2793`; `loader.py:2145-2155`)。flash直後からこのrunによる意図したresetまでに得た初回raw reset reason `0x15` はobserver-triggered resetとして原因集計から除外し、その後の再起動recordと区別する。

T0から5–10秒間はpyserialもOpenOCDも起動せず、RTS等を操作せずUSB disconnect/resetと再列挙を記録する。これはpyserial/OpenOCDを開かない区間だがhost USB busは接続されたままで、host独立試験ではない。続いてnoinit snapshotを取得する。

同一build ELFからnoinit log addressを求める。

```bash
DIAG_ADDR=$("$TC/riscv32-esp-elf-nm" -n "$BUILD_DIR/shizuku_esp32c6.elf" | awk '$3 == "shizuku_c6_early_diag_log" {print "0x"$1}')
test -n "$DIAG_ADDR"
```

local OpenOCD v0.12.0-esp32-20260703のESP USB-JTAGを使う。GDB/TCL/Telnet serverをdisableし、既定port競合を避ける。`esp_usb_jtag.cfg:6-9` はdriver `esp_usb_jtag` と VID/PID `0x303a:0x1001` を指定する。upstream `esp_usb_jtag_init` は共通 `jtag_libusb_open` に選択を委譲し (`esp_usb_jtag.c:562-584`)、共通 `libusb_helper.c` は `adapter_get_required_serial()` を読み、要求serialと一致しないdeviceを候補から除外する (`libusb_helper.c:237,263-267`) ([driver source](https://github.com/espressif/openocd-esp32/blob/master/src/jtag/drivers/esp_usb_jtag.c#L562-L584), [common USB helper](https://github.com/espressif/openocd-esp32/blob/master/src/jtag/drivers/libusb_helper.c#L237-L267))。これはmasterのsource確認であり、installed v0.12.0-esp32-20260703との同一性は未確認。保守的なidentity gateとして、init前にOSのread-only USB inventoryで対象serial `58:E6:C5:12:E3:98`、VID/PID `303a:1001` と一意なbus/port locationの対応を確認し、そのlocationも指定する。OS側でserialとlocationの一意対応を確認できなければOpenOCDを起動しない。候補inventory command `ioreg -p IOUSB -l -w0` はこのhostで未実行・未検証であり、serial/location両方が出ない場合は別のread-only inventoryを使う。

最初のinvocationではreset/halt/GDB attachをせず、`init` 後にtarget configが定めるHP target `$_TARGETNAME_0` を選び、そのtargetへ `riscv set_mem_access sysbus` を指定してからrunning targetのsystem-bus readを行う。ESP common configのdefaultは `sysbus progbuf abstract` (`esp_common.cfg:367`) なので、この上書きに失敗した場合やcontrollerがsysbus readを受け付けない場合は状態unknownとして止める。progbuf/abstractへのfallback、halt、resetによる救済はしない。

```bash
OPENOCD_BASE="$IDF_TOOLS_PATH/tools/openocd-esp32/v0.12.0-esp32-20260703/openocd-esp32"
USB_LOCATION="${USB_LOCATION:?set to the uniquely verified USB bus-port[.port] location for serial 58:E6:C5:12:E3:98}"
[[ "$USB_LOCATION" =~ ^[0-9]+-[0-9]+(\.[0-9]+)*$ ]] || { echo "Invalid USB_LOCATION; stopping" >&2; exit 1; }
JTAG_START="$(date '+%Y-%m-%dT%H:%M:%S%z')"
OPENOCD_STDOUT="$PWD/openocd-noinit.stdout.log"
OPENOCD_STDERR="$PWD/openocd-noinit.stderr.log"
OBSERVER_TCL="/private/tmp/shizuku-c6-noinit-observer-$$.tcl"
cat >"$OBSERVER_TCL" <<'OPENOCD_TCL'
proc checked {label script} {
  if {[catch {uplevel #0 $script} result]} {
    echo "$label failed: $result"
    shutdown error
    error "$label failed"
  }
}
checked init {init}
if {![info exists _TARGETNAME_0]} {
  echo "ESP target name missing; stopping"
  shutdown error
  error "ESP target name missing"
}
checked target-select [list targets $_TARGETNAME_0]
checked sysbus [list $_TARGETNAME_0 riscv set_mem_access sysbus]
checked mdw {echo [mdw $::env(SHIZUKU_DIAG_ADDR) 94]}
shutdown
OPENOCD_TCL
export SHIZUKU_DIAG_ADDR="$DIAG_ADDR"
printf 'JTAG_START=%s\nUSB_LOCATION=%s\n' "$JTAG_START" "$USB_LOCATION"
if "$OPENOCD_BASE/bin/openocd" -s "$OPENOCD_BASE/share/openocd/scripts" \
  -f interface/esp_usb_jtag.cfg -f target/esp32c6.cfg \
  -c "adapter serial 58:E6:C5:12:E3:98" -c "adapter usb location $USB_LOCATION" \
  -c "gdb_port disabled" -c "tcl_port disabled" -c "telnet_port disabled" \
  -f "$OBSERVER_TCL" \
  >"$OPENOCD_STDOUT" 2>"$OPENOCD_STDERR"; then OPENOCD_RC=0; else OPENOCD_RC=$?; fi
printf 'OPENOCD_EXIT=%s\nstdout=%s\nstderr=%s\n' "$OPENOCD_RC" "$OPENOCD_STDOUT" "$OPENOCD_STDERR"
cat "$OPENOCD_STDOUT"
cat "$OPENOCD_STDERR" >&2
if [ "$OPENOCD_RC" -ne 0 ]; then echo "RAM snapshot unknown; do not halt/reset for recovery" >&2; exit "$OPENOCD_RC"; fi
```

OpenOCD User's Guideは `shutdown error` がnon-zero exit statusを返すと記載する ([General Commands: shutdown](https://www.openocd.org/doc/html/General-Commands.html#shutdown)). stdout / stderrを別々に保存し、JTAG開始時刻とOpenOCD exit codeを記録する。exit non-zero、serial/location mismatch、sysbus選択失敗、または94 wordsの読取失敗はRAM snapshot unknownで停止し、halt/resetして救済しない。`esp_common.cfg:367`のdefault fallbackを有効なまま使わない。OpenOCD `examine-end` (`esp_common.cfg:239-252`)はoption/semihost設定のみでreset/WDT writeをしない。

PCが必要なら、noinit dump完了後の別invocationで同じinterface/targetに`-c init -c halt -c "reg pc" -c shutdown`を行う。これはreset commandではないが、C6 targetの`halted` event (`esp_common.cfg:279-285`)はWDT disable callbackを呼ぶ。C6 handler (`target/esp32c6.cfg:31-58`)はTG0/TG1/RWDT/SWDをdisableしinterrupt stateをclearするため、halt後のWDT statusや自律reset有無はbaselineに使わない。GDB attach (`esp_common.cfg:307-325`)はhaltし、memory protection次第で`reset halt`を行うため使わない。`esp32c6_soc_reset` (`target/esp32c6.cfg:61-90`)は明示的HPSYS/CPU software reset手順なので呼ばない。

ring dumpのmagic/version/checksum、sequence、raw reset reason、全WDT config、writer status、PCと同じbuild ELF hashを保存する。`0x15`はC6 USB UART reset、`0x12`はSuper WDT、`0x07/0x08/0x0B/0x11`はMWDT、`0x09/0x0D/0x10`はRTC WDT、`0x0F`はbrownout。Sequenceが増えてWDT reset reasonが反復すればwatchdog仮説を支持する。invalid/no entryはhook前resetやDRAM retention lossも残るためresetなしの証拠にしない。OpenOCD終了後、esptool post-gateでESP32-C6とMAC `58:E6:C5:12:E3:98` を確認する。OpenOCD/JTAG接続時刻も記録する。

### B. Serial captureは別run

no-serial runのcounterと混ぜず、別flash runとして各flash前MAC gateをやり直し、3領域のwrite/verifyを保存する。OpenOCDを起動せず、pyserialを115200 baud、`dtr=False`, `rts=False`でopenする。open時刻とopen直後reset/re-enumerationを別記し、RTS等を触らず5秒のactive-nop idle区間でmarker数、disconnect/reset数、最終port状態を記録する。次にRTSを一度だけ0.1秒pulseし、USB再列挙後15–30秒以内に同portを自動reopenして同じ値を記録する。open/RTS起因resetはpulse前区間と分ける。serial run最後もchip-id post-gateを実行する。

判定: no-serial ringでsequence増加とraw WDT reset codeが対応すればwatchdog resetを支持する。no-serial区間が安定しserial open/reopen/RTS後だけresetするならhost/open起因を支持する。writer status `2`ならhelper falseでありidle原因は判定不能。writer status `1`はUSB IN endpoint completionで、TTYの受信/displayではない。Serial marker count 0単独ではendpoint completion失敗とhost buffer遅延を区別できない。原因判断にはfull-hash一致ELF、raw ring records、observer開始/停止時刻を揃える。

上層は下層の静的検査とは別に、Bazel build exit code、test_image.sh 各 assert と exit code、ELF/hash、gate の生出力、
serial log を確認する。実機ログが得られても、それは USB Serial/JTAG marker 到達の証拠であり、`KERNEL::init` / PMP / trap / scheduler
が初期化された証拠ではない。
