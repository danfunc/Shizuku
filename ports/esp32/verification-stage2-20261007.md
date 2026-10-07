# C6 段2: 上層受入用の証跡と実機依頼

2026-10-07。開始 HEAD `dc972cb`、開始 worktree は clean。今回は実装担当として直接編集した。Gemini callable tool はセッションになかった。Bazel/IDF 最終 build、esptool、serial、OpenOCD、commit/push は実行していない。

実装は M-only の段2 pilot。フレームは 144 bytes、caller-saved の既存オフセットを維持し、mcause=72、mtval=76、s0..s11=80..124、gp=128、tp=132 を追加した。SP は frame+144 として復帰する。オフセットを `c6/frame_layout.h` に集約し C++ は全フィールドの offsetof を assert する。V2 call-frame header+frame が16の倍数であることも RV32 コンパイル時に assert した。

`rv32_trap.S` は256-byte境界の32個の4-byteベクタ、全レジスタ保存、mepc/mstatus 復元と mret を実装する。例外フレームは V2 の幾何に合わせて interrupted stack に置く。外側では mscratch の専用 kernel stack top を取得し mscratch=0 にして C++ へ入る。内側は mscratch=0 を検出して interrupted kernel stack を使う。mscratch の元値は移動可能な thread frame とは別に16-byteの kernel-stack recordへ保存する。復帰直前に元値を戻す。MIE/MPIE は初回復帰で無効のまま。同期 fault を含む、frame 保存前の stack 自体の故障からの回復は今回の範囲外。

ECALL(8/11) のみ PC+4 と svc 経路へ分類する。kernel 内 nested ECALL は thread context を変更せず停止。hardware IRQ のネスト深さは interrupt bit のある trap だけを加算し、暫定公開 API は `rv32_c6::in_interrupt()`。単一 HP hart の変数で、LP CPU は対象外。IRQ acknowledge と timer/switch が未実装なので、本物の IRQ と同期 fault は記録して停止する。M-only のため U-mode 実復帰も拒否する。prepare_thread_entry の MPP 生成自体は privileged=true/false の双方を検査した。将来 U-mode を許すには、PMP 所有権と信用できる U-entry stack が必要。

`unprivileged_floor()` は pilot に限り linker `_bss_end` を返す。BSS 内の thread table/ledger を V2 へ貸せるようにするための値で、PMP 保護境界ではない。一般用途のセキュリティ保証は与えない。本番の fail-closed gate は SYSTIMER、interrupt matrix、PMP が残るため維持した。

thread 0 は V2 init → storage/handler 設定 → bootstrap → 初回 mret → object ECALL → diagnostic handler → RETURN ECALL → 元 thread へ戻る経路を通す。RAM マーカを残して active counter loop に入る。USB writer はこの pilot の起動経路から外した。IDF OS shim は追加していない。

## 実行済み検証

`/private/tmp/c6_stage2_check.sh` と `/private/tmp/c6_stage2_check.log` にコマンドと出力を保存。スクリプト終了コード0。host C++ tests 3個、fail-closed script、trap static Python、8個のRV32 C++ object と assembly object の単体コンパイル、relocatable link、shell syntax、git diff --check が通った。relocatable object の未解決参照は `_bss_end`, `malloc`, `free`, `memcpy`, `memset` の5個。最終 IDF link は未検証であり、これらの供給もそこで確認する。更新した early_diag.c も旧同定buildの compile_commands.json に記録された IDF include/flagsで、sourceとoutputだけを現workspaceと/private/tmpへ差し替え単体コンパイルした (exit 0)。これはfresh IDF buildの代わりではない。

上層が読むテスト本体:

| file:line | assert 内容 |
|---|---|
| c6/test_c6_concepts.cpp:18 | shared frame size=144 |
| c6/test_c6_concepts.cpp:25-57 | a0..a4 引数、a0/a1 結果、mepc/ra、frame+144幾何、単核、cause/mtval offset |
| c6/test_c6_concepts.cpp:67-74 | 16-byte境界、初期PC/RA/引数、MPPがprivilegedに一致、MIE/MPIE無効、他レジスタ/cause初期ゼロ |
| c6/test_trap_route.cpp:14-21 | ECALLだけPC+4、IRQだけdepth=1、faultはdepth=0、cause/mtval維持、分類件数2/2/1 |
| c6/test_trap_route.cpp:24-34 | nested IRQ depth 1→2→1→0、IRQ内ECALL/faultはdepthを増やさない |
| c6/test_trap_static.py:10-31 | 全オフセット、全レジスタsave/restore、4 CSR capture、mret、256-byte/32ベクタ、mscratch入れ子契約、入口マーカがring.magicを壊さない |
| c6/test_fail_closed.sh:7-24 | arch/boardの期待#error本文、明示pilot opt-inなら同じsourceがコンパイル成功 |
| c6/test_pmp.cpp:19,113 | NAPOT/TOR codecがLを暗黙付加しない (他の既存PMP assertionsも実行) |
| c6/trap.cpp:5 | V2 call-frame header+frameが16の倍数 (RV32コンパイル済み) |

`firmware/esp32c6/test_image.sh:73-88` は最終ELFの init/bootstrap 呼出し、thread0 symbol、mret、stage2 ELF検査へ更新した。`c6/test_stage2_elf.py:14-26` は fresh 最終ELFでリング/progressがnoinit内・BSS外、vector256-byte整列/32entry、dispatch/CSR復帰、JTAG symbol存在を assert する。両者は未実行。静的検査とhost分類テストは、実機でのtrap実行/IRQ nesting/PMP保護の証拠ではない。

## リングの調査結果

旧 DIAG_ADDR `0x4080af6c` と一致するローカル ELF は `bazel-bin/firmware/shizuku_esp32c6.bin.idf.eHmssi/shizuku_esp32c6.elf`。SHA256=`16f0c8ccb60bc9a774f2763c1c09cafeb031977b48658244d2b88858f54defc4`。この ELF と実機へ書き込まれた image の同一性は今回確認していない。

対応 map:19442-19447 は `.noinit`=0x4080af6c..0x4080b0e4 (376B)、BSS=0x4080b0e8..0x4080bbf8。IDF `cpu_start.c:986` は BSS clear、:1018 は SYS_STARTUP_FN。ELF の start_cpu0 0x42009538 は :0x4200953c で diag begin、:0x4200953e で early_boot を呼ぶ。diag begin は :0x42009898 で ROM reset-reason helper を呼び、それより前にリングを書いていない。このartifactは現HEADにある入口マーカ追加前のもの。ゼロクリア範囲へのリング混入はこのELFでは否定できるが、実機のmagic不一致原因は未確定。

現HEADには別の確定不具合があった: ROM前に ring.magic を ENTRY_MAGICへ上書きし、その後「magic != 正式MAGIC」で必ず initialize_log()へ進むため、retained history を消していた。今回、入口マーカを別noinit symbol `shizuku_c6_early_diag_progress` へ移した。progressはENTRY_MAGIC=0x5348454e (ROM前)、2 (ROM復帰後)、3 (WDT snapshot前)、4 (record確定)。リングの正式magic/versionは 0x53485a44/2。writerは今回呼ばないので writer_status=0xffffffff が正常。

magic不一致の原因確定には、実機に書いたimageの完全SHA256と一致するELF、progress/リングと既知RAMマーカの同時sysbus読取が足りない。旧dumpだけでROM停止、hook未到達、read-path不整合、retention問題のいずれとも決めない。

## 上層に依頼: fresh build と JTAG

cwd=`/Users/ishigakiyua/github/Shizuku-esp32`。起動失敗時は末尾「追記」のimage同定を先に行う。既存 `verification-20261006.md:129-280` のbuild directory同定、MAC gate、3領域flash、app run、JTAG identity/sysbus-only手順を使う。新しいコードなので ELF から symbol を毎回求める。古い DIAG_ADDR は使わない。Pico `/dev/cu.usbmodem101` と103は触らない。

```bash
IDF_PATH=.local/esp-idf IDF_TOOLS_PATH=.local/idf-tools bazelisk build --config=esp32c6 //firmware:esp32c6
# Expected: exit 0. Exactly one build directory must byte-match the Bazel .bin.
# Set BAZEL_BIN, BUILD_DIR and TC using the existing verification procedure.
bash firmware/esp32c6/test_image.sh "$BAZEL_BIN/libshizuku_kernel.a" \
  "$BUILD_DIR/shizuku_esp32c6.elf" "$BUILD_DIR/shizuku_esp32c6.map" \
  "$BUILD_DIR/shizuku_esp32c6.bin" "$TC"
# Expected: all assertions PASS, fails=0, exit 0. Failure stops flashing.
```

static ELF検査が通ったら、上層が直前MAC gate→同buildのflash_args→app runを実行する。シリアルをopenしない。完全ELF/bin hash、flash verify、app runのexit/timeを保存する。

```bash
ELF="$BUILD_DIR/shizuku_esp32c6.elf"
export SHIZUKU_ELF="$ELF"
for name in shizuku_c6_early_diag_log shizuku_c6_early_diag_progress \
            shizuku_c6_thread0_diag shizuku_c6_trap_diag \
            shizuku_c6_irq_depth shizuku_c6_trap_depth; do
  "$TC/riscv32-esp-elf-nm" -n "$ELF" | awk -v name="$name" '$3 == name {print name, "0x"$1}'
done
```

各symbolが一意に得られたことを確認し、既存obs.tclのsysbus指定後に以下の形で読む。実ADDRは上記nm結果に置換する。mdwは必ずechoで出力する。2回のsnapshotは別invocationとし、間隔と時刻を記録する。

```tcl
checked mdw-progress {echo [mdw PROGRESS_ADDR 1]}
checked mdw-ring {echo [mdw RING_ADDR 94]}
checked mdw-thread0 {echo [mdw THREAD0_ADDR 5]}
checked mdw-trap {echo [mdw TRAP_ADDR 5]}
checked mdw-irq-depth {echo [mdw IRQ_DEPTH_ADDR 1]}
checked mdw-trap-depth {echo [mdw TRAP_DEPTH_ADDR 1]}
```

期待値:

| symbol | 判定 |
|---|---|
| early_diag_progress | 4ならrecord確定まで到達。ENTRY_MAGIC/2/3で止まればその区間を調査 |
| early_diag_log | magic=53485a44, version=2、少なくとも有効checksumのrecord。writer_status=ffffffff |
| thread0_diag[0..4] | c6000001, c6000002, **変化するcounter**, 00000000, c6000002 |
| trap_diag[0..4] | 0, 0000000b, **RETURN ECALL PC**, 0 (通常), 2。mtval非0ならrawを保存して調査 |
| irq_depth / trap_depth | idle時に0 / 0 |

thread0[0]だけなら初回mretまで、[1]とerror/value一致ならsvc/RETURN往復まで、counterが2回で変化すれば稼働継続まで確認できる。IRQ depth=0だけではIRQ経路/nestingの実機検証とはしない。trap.status e001=未対応IRQ、e002=同期fault、e003=nestedECALL、e004=U-mode拒否、e005=RETURN失敗、e006=初期stack不正、e007=core不正、e008=kernel panic。非0ならcause/PC/mtvalを同ELFに照合する。mdwやidentity gate失敗はunknownで停止し、halt/reset/progbufで救済しない。

## 追記: run 起動未到達の切り分け (2026-10-07)

上層報告: fresh build/test_image成功、flash3領域verify、run exit=0。8/11秒のrunning sysbusではprogress/ring=0、thread0/trap/depthは不定値で同値。次のrunの6秒後のhaltでPC=0x40800404。これらは「段2の到達マーカを観測できなかった」証拠。段1の「未openならUSB安定」はアプリ到達を確認した観測でないため、アプリの安定性の結論は保留。

**PCだけでは、実行中のイメージを同定できない。** installed esptool `loader.py:174-195` はstub2優先、`__init__.py:618-619` は`--no-stub`なしでstubをRAMへロードする。stub1/2のESP32-C6 JSONはいずれもtext_start=0x40800000。従って0x40800404はアプリとstubで別の命令になる。ローカルstub2では`addi sp,sp,32`、stub1では`lw ra,12(sp)`。アプリELFのaddr2line表示だけからcache関数の滞留とは断定しない。実際にstubが残っているかも、まだlive bytesを読んでいないため未確認。

完全SHAと一致する今回のflash用ELF/mapのパスは、今回の相談にはまだ提示されていない。ローカルに見つかったtgaq3YのELFはnm配置が上層報告と一致するが、SHA256は`3ffdde8aa679e431e08f38d29310ec04ab2de7f110e1f55a4cb87bc04674de8d`で、報告`117774df…16ee`と違う。これは**静的調査用の別artifact**であり、実機PCの確定解釈には使わない。このartifactの0x40800404は`lui a1,0x4080e`。直後0x4080040eで初めてspi_flash_disable_cacheを呼ぶ。PC観測位置自体は待機loopではなく、cache無効化後という証拠でもない。

IDFの起動順: `cpu_start.c:975-1018` のcall_start_cpu0はinit_cpu→reset reason→BSS clear(:986)→cache/MMU→RTC/MSPI→system_early_init→SYS_STARTUP_FN。`startup.c:75-83`のtableが強symbol start_cpu0へ渡し、`main/shizuku_boot.c:22-23`がdiag→early_bootへ渡す。start_cpu0差し替えはBSS clearより**後**なので、その差し替えでBSS clearが省略されることはない。BSS不定値の解釈には、wrong image、RAM reader/target、reset/上書きも残る。

アプリ側cache関数の呼出し元と依存:

- `spi_flash/flash_ops.c:84-91`: normal/no_osのguard tableのstart callback。
- `spi_flash/spi_flash_os_func_app.c:83-85,204-253`: SPI1 flash操作のstart/lock経路→cache_disable callback→当該関数。pre-init entryから直接呼んでいない。
- `spi_flash/esp_flash_spi_init.c:629-650`: CORE init_flashがdefault guardとOS functionsを設定する。`startup.c:168-170,200-204`のdo_core_initから実行されるが、今回のstrong start_cpu0はそこを飛ばす。
- `spi_flash/cache_utils.c:252-287`: CONFIG_FREERTOS_UNICORE時はother_cpu待ちがなく、spi_flash_op_lock→esp_intr_noniram_disable→CPU0 cache disable。ローカル調査artifactのsdkconfig.h:710はUNICORE=1。
- 同`:266`はvTaskSuspendAllを呼ぶ。将来Shizukuからdefault IDF flash OS pathを使えば、FreeRTOS依存の初期化欠落が問題になる。ただし今回の停止原因と結び付けるcaller/RAは未取得なので、shimやguard差し替えは追加しない。
- 同`:285`のlockはcacheを切る**前**、disable_cacheはIRAM→cache HAL経路。外側callerがflashにあり、cache offのままそこへ戻ればfaultになり得る。実際のcallerをRA/stackから同定して初めて判定する。
- IRQ/WDT停止・再起動を判断するにはrunning baselineが必要。halt後はtarget eventがWDTと割込み状態を変更するため、その後の無再起動やWDT状態を根拠にしない。

SDKも [USB-Serial/JTAGを手動download modeへ入れた場合、既定resetで抜けられない場合](https://docs.espressif.com/projects/esptool/en/latest/esp32c6/troubleshooting.html) を説明する。今回の該当有無はBOOT操作履歴とrunの生ログが必要。`RUN_EXIT=0`や`Hard resetting via RTS pin...`はhost command成功を示すが、実際のnormal bootを保証しない。

### 上層への依頼順 (今回はkernel sourceを変更しない)

1. 今回flashした完全ELF SHA256とELF/map/bootloaderの絶対パス、run stdout/stderr、BOOT/EN操作履歴、OpenOCD target名を保存する。同じELFとの一致が取れるまでPCのsymbol名を確定しない。
2. 今のrun経路のまま、running/sysbus-onlyで下記のcode windowsを追加読取する。halt前baselineの別invocation。mdw失敗はunknownで停止する。
3. fingerprintをoffline比較する。stub一致なら「アプリのcache関数」での停止という仮説を撤回し、normal bootへのreset経路を切り分ける。app一致なら次のhalt観測へ。unknown/混在ならlive codeの出所が未確定として停止し、appのsymbolだけで解釈しない。
4. USB/RTSでアプリへ移れない場合、**BOOTを離した状態で物理EN/RESETを一度押す** (上層がboardのボタンを識別した場合のみ)。serial portは開かずJTAG baselineを採る。代替の試験コマンドは `esptool --chip esp32c6 --port /dev/cu.usbmodem1101 --no-stub --before no-reset --after hard-reset run`。これはstubを再ロードしない試験であり、成功を前提にしない。ROM syncできなければそこで停止する。既存run経路、no-stub経路、物理resetは別runにしT0と結果を分ける。再flashする場合は既存MAC gateを必ず直前に行う。
5. app bytesと到達マーカが一致すればthread0期待値を再検査する。まだ到達しなければ次のhalt→resume→haltを採る。PCが進む場合はcaller chain、同じ場合はRA/CSRと命令bytesを合わせて調査する。同PCの3標本だけでは永久停止/loopを断定しない。

fingerprint toolはdeviceを開かず、指定ELFの完全SHAを検証し、PT_LOADのfile-backed bytesとstub JSONを比較する。

```bash
# Set these to the EXACT flashed artifacts; do not select by mtime or prefix SHA.
ELF=/absolute/path/to/flashed/shizuku_esp32c6.elf
BOOT_ELF=/absolute/path/to/same-build/bootloader/bootloader.elf
ELF_SHA256=full_64_lowercase_hex_digits
STUB_DIR="$IDF_TOOLS_PATH/python_env/idf6.1_py3.14_env/lib/python3.14/site-packages/esptool/targets/stub_flasher"
python3 ports/esp32/tools/boot_fingerprint.py \
  --elf "$ELF" --expected-sha256 "$ELF_SHA256" --bootloader "$BOOT_ELF" \
  --stub-json "$STUB_DIR/2/esp32c6.json" --stub-json "$STUB_DIR/1/esp32c6.json" \
  > /private/tmp/c6-boot-fingerprint-expected.txt
```

既存running observerのsysbus指定後、shutdown前へ追加する:

```tcl
checked code-0 {echo [mdw 0x40800000 8]}
checked code-1 {echo [mdw 0x408003f0 8]}
```

stdout/stderrのmdw出力を1 invocation分だけ保存してtoolへ`--dump /absolute/path/to/code-dump.log`を追加する。exit0でCODE_IDENTITYがapp/stubの**一意一致**、exit1はunknown/混在/複数一致、exit2はSHA/入力/read不足。二つのwindowがapp一致してもimage全体の完全一致の証明ではない。少なくとも今回の0x40800404と初期vectorを解釈するための局所的同定となる。bootloaderが二windowをfile-backedで持たない場合はnot file-backedと出る。PCがbootloader領域なら`--address`へそのPC周辺の32B境界を指定し、そこで比較する。

### 正式な halt 観測 (baseline後、別invocation)

`ports/esp32/tools/boot_pc_observer.tcl`を追加した。HP target選択とsysbus指定、halt前のcode windows、3回のhalt1000→force read→resume (間隔200ms)を行い、最後は明示resumeする。失敗はshutdown errorで止まり、targetがhaltedかrunningかはunknownとして報告する。reset、step、reg書込み、progbufへのfallbackはしない。CSR名がinstalled OpenOCDで使えなければ、reg inventoryをread-onlyで採って名前を確認し、未取得の値を補わない。

```bash
# Same identity/location gate and OpenOCD path as the baseline procedure.
"$OPENOCD_BASE/bin/openocd" -s "$OPENOCD_BASE/share/openocd/scripts" \
  -f interface/esp_usb_jtag.cfg -f target/esp32c6.cfg \
  -c "adapter serial 58:E6:C5:12:E3:98" -c "adapter usb location $USB_LOCATION" \
  -c "gdb_port disabled" -c "tcl_port disabled" -c "telnet_port disabled" \
  -f ports/esp32/tools/boot_pc_observer.tcl \
  > /private/tmp/c6-boot-pc.stdout.log 2> /private/tmp/c6-boot-pc.stderr.log
# Save exit code/time. Failure does not justify reset or alternative mem access.
```

CSR取得の本体は `[get_reg -force {pc ra sp mepc mcause mtval mstatus mtvec}]`。forceでOpenOCDのregister cacheを使わない。PC/RA/SPを毎sampleの命令bytesと組に保存する。mepc/mcause/mtvalは最新trapの値で、haltした時点のactive faultを必ず意味するわけではない。SPが同ELFのRAM範囲内と確認できれば別のchecked mdwで8wordsを読み、候補return addressを同定する。raw RAにはRISC-Vの2-byte/4-byte命令があり、RA-4を無条件にcaller PCとしない。nm/objdumpでRA直前のcall命令と照合する。

[OpenOCD reg/get_reg/halt/resume仕様](https://www.openocd.org/doc/html/General-Commands.html) と installed `esp_common.cfg:279-285`/`esp32c6.cfg:31-58`を確認した。halt eventはTG0/TG1/RWDT/SWDを止め、interrupt stateをclearする。**最初のhalt以降の全sampleはWDTに関して観測で変化した系**として扱い、無再起動・WDT stateを元のbaselineにしない。最後にchip-id gateをする場合も、それはもう一つのserial/reset介入であり、PC sample前のrunと混ぜない。

今回の追加host検証: `c6/test_boot_fingerprint.py:16-29`のassertは同PCで別bytes、未mapped/部分window拒否、mdw parser、矛盾する複数snapshot拒否を検証しexit0。ローカルの別artifactとinstalled stub JSONからfingerprint生成もexit0。新Tclはmock commandによる構文/順序検査のみ実施する。実機・新buildは上層依頼、startup/cacheの原因修正はlive code identityとcallerが未確定のため実施していない。
