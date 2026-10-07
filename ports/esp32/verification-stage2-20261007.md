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

## 上層に依頼: fresh build、アプリ起動、JTAG観測

cwd=`/Users/ishigakiyua/github/Shizuku-esp32`。
Bazel/IDF 最終 build、esptool、serial、OpenOCD、commit/push、実機操作は下層では実行できないため上層に依頼する。
Pico `/dev/cu.usbmodem101` と 103 は触らない。対象は ESP32-C6 (MAC: `58:E6:C5:12:E3:98`、ポート: `/dev/cu.usbmodem1101`)。

### 1. Fresh build と static ELF 検査

```bash
IDF_PATH=.local/esp-idf IDF_TOOLS_PATH=.local/idf-tools bazelisk build --config=esp32c6 //firmware:esp32c6
# Expected: exit 0. Exactly one build directory must byte-match the Bazel .bin.
# Set BAZEL_BIN, BUILD_DIR and TC using the existing verification procedure.
bash firmware/esp32c6/test_image.sh "$BAZEL_BIN/libshizuku_kernel.a" \
  "$BUILD_DIR/shizuku_esp32c6.elf" "$BUILD_DIR/shizuku_esp32c6.map" \
  "$BUILD_DIR/shizuku_esp32c6.bin" "$TC"
# Expected: all assertions PASS (including test_stage2_elf.py), fails=0, exit 0. Failure stops flashing.
```

### 2. MAC gate と flash

直前 MAC gate を行い、同一 build directory の `flash_args` に従って bootloader / partition table / app の 3 領域を flash する。

```bash
esptool --chip esp32c6 --port /dev/cu.usbmodem1101 --before default-reset --after no-reset chip-id
# MAC 58:E6:C5:12:E3:98 一致を確認後:
cd "$BUILD_DIR"
esptool --chip esp32c6 --port /dev/cu.usbmodem1101 --before default-reset --after no-reset write-flash @flash_args
```

### 3. アプリ起動手順の正式化 (esptool run 不可、pyserial 起動)

`esptool run` (`--after hard-reset run`) は stub flasher が RAM 上に残るため**不可**。
正しい手順は「シリアルポートを `dtr=False`, `rts=False` で open → RTS を 0.1 秒 1 パルス (`True` → 0.1s → `False`) → close」により、ESP32-C6 ハードウェアリセット回路を作動させてブートローダから flash アプリを起動する。

最小の pyserial 起動スクリプト (`ports/esp32/tools/c6_reset_run.py` 相当):

```python
import time
import serial

PORT = "/dev/cu.usbmodem1101"

# dtr=False, rts=False で open して即時リセットやブートローダ突入を防ぐ
with serial.Serial(PORT, baudrate=115200, dtr=False, rts=False) as s:
    time.sleep(0.05)
    # RTS を True にして EN (CHIP_PU) を Low に落とす (0.1秒パルス)
    s.rts = True
    time.sleep(0.1)
    # RTS を False に戻して EN を High に引き上げ、通常ブート (DTR=FalseのためBOOTピンはHigh)
    s.rts = False
    time.sleep(0.05)
# close 完了でアプリ起動へ移行
```

起動実行例:
```bash
python3 -c "import serial, time; s = serial.Serial('/dev/cu.usbmodem1101', 115200, dtr=False, rts=False); time.sleep(0.05); s.rts = True; time.sleep(0.1); s.rts = False; time.sleep(0.05); s.close(); print('C6 hard reset pulsed via RTS')"
```

### 4. 観測手順: JTAG (OpenOCD, halt なし, sysbus)

シリアルポートを常時開くのではなく、JTAG (OpenOCD、halt なし、`sysbus`、`echo [mdw ADDR N]`) でターゲットを停止させずに RAM を直接読む。

ELF から各診断シンボルのアドレスを取得:
```bash
ELF="$BUILD_DIR/shizuku_esp32c6.elf"
for name in shizuku_c6_early_diag_log shizuku_c6_early_diag_progress \
            shizuku_c6_thread0_diag shizuku_c6_thread1_diag \
            shizuku_c6_switch_diag shizuku_c6_timer_diag \
            shizuku_c6_trap_diag shizuku_c6_irq_depth shizuku_c6_trap_depth; do
  "$TC/riscv32-esp-elf-nm" -n "$ELF" | awk -v name="$name" '$3 == name {print name, "0x"$1}'
done
```

OpenOCD observer スクリプト例:
```tcl
proc checked {label script} {
  if {[catch {uplevel #0 $script} result]} {
    echo "$label failed: $result"
    shutdown error
    error "$label failed"
  }
}
checked init {init}
checked target-select [list targets $_TARGETNAME_0]
checked sysbus [list $_TARGETNAME_0 riscv set_mem_access sysbus]

# 診断シンボル読み取り (アドレスは nm 結果を使用)
checked mdw-progress     {echo [mdw PROGRESS_ADDR 1]}
checked mdw-ring         {echo [mdw RING_ADDR 94]}
checked mdw-thread0      {echo [mdw THREAD0_ADDR 5]}
checked mdw-thread1      {echo [mdw THREAD1_ADDR 5]}
checked mdw-switch       {echo [mdw SWITCH_ADDR 4]}
checked mdw-timer        {echo [mdw TIMER_ADDR 7]}
checked mdw-trap         {echo [mdw TRAP_ADDR 5]}
checked mdw-irq-depth    {echo [mdw IRQ_DEPTH_ADDR 1]}
checked mdw-trap-depth   {echo [mdw TRAP_DEPTH_ADDR 1]}
shutdown
```

間隔をあけて 2 回以上 snapshot を取得し、カウンタの増加を確認する。

### 5. 新しい診断印のシンボルと期待値の表

| シンボル | 要素 / 期待値 | 判定基準 |
|---|---|---|
| `shizuku_c6_thread0_diag` | `[0]` = `0xc6000001`<br>`[1]` = `0xc6000002`<br>`[2]` = **増加カウンタ**<br>`[3]` = `0x00000000` (初回ECALL error)<br>`[4]` = `0xc6000002` (初回ECALL value) | `[0]`でthread0起動、`[1]`で初期ECALL往復成立、`[2]`増加でthread0が生存ループ実行中。 |
| `shizuku_c6_thread1_diag` | `[0]` = `0xc6100001`<br>`[1]` = **増加カウンタ**<br>`[2]` = `0x00000000` (spawn error OK)<br>`[3]` = `2` (spawned thread ID)<br>`[4]` = `0` (reserved) | `[0]`でthread1起動到達、`[1]`増加でthread1が生存ループ実行中、`[2]=0`でspawn成功。 |
| `shizuku_c6_switch_diag` | `[0]` = EXPIRED カウンタ (増加)<br>`[1]` = YIELDED カウンタ<br>`[2]` = error カウンタ (`0`)<br>`[3]` = last raw reply | GRANT プリミティブからの復帰カウンタ。タイムスライス満了による復帰 (`[0]`) が記録される。 |
| `shizuku_c6_timer_diag` | `[0]` = SYSTIMER IRQ count (**増加**)<br>`[1]` = max IRQ depth (`1`)<br>`[2]` = switch count (**増加**)<br>`[3]` = last oneshot ticks (`160000`)<br>`[4]` = last IRQ mcause (`0x80000011` / slot 17)<br>`[5]` = init時 MIE readback (`0x00020000`)<br>`[6]` = pend_context_switch count (**増加**) | `[0]`のIRQ増加でSYSTIMER割込み発生、`[1]=1`でネスト不正なし、`[2]`増加でタイマ契機コンテキストスイッチ成立。 |
| `shizuku_c6_trap_diag` | `[0]` = `0` (status)<br>`[1]` = `0x0000000b` (last ECALL cause)<br>`[2]` = RETURN ECALL PC<br>`[3]` = `0` (mtval)<br>`[4]` = ECALL count (増加) | `[0]=0` でトラップ異常停止なし。 |
| `shizuku_c6_irq_depth`<br>`shizuku_c6_trap_depth` | idle時ともに `0` | トラップハンドラ外での正常値。 |

**判定結論**: `shizuku_c6_thread0_diag[2]` と `shizuku_c6_thread1_diag[1]` の**両スレッドのカウンタがともに増加**していることをもって、「段2後半: SYSTIMER 割込みおよびプリエンプティブなタイマ切替成立」と判定する。

---

## 過去の観測事実への訂正注記

> **訂正注記 (2026-10-07)**:
> 過去の観測において、`esptool run` 直後の halt 観測で `PC=0x40800404` が得られ、アプリの SPI flash cache 無効化関数 (`spi_flash_disable_cache`) 付近での停止と疑われた経緯がある。
> しかしその後の調査により、`esptool` がダウンロード時に RAM に配置する stub flasher (`esp32c6.json`) も同じ `0x40800000` 番地 (IRAM) を `text_start` として使用しており、`0x40800404` は stub 内の有効命令 (`addi sp, sp, 32` 等) に一致することが判明した。
> すなわち、`esptool run` (`--after hard-reset`) では ESP32-C6 の内蔵 USB-Serial-JTAG 経由のリセット制御が正常にアプリへハンドオーバされず、**RAM 上の stub flasher がそのまま残存・実行されていた可能性が極めて高い**。
> アプリケーションの cache 処理自体が壊れていたわけではなく、ブートモード脱出・アプリ起動手段の不備であったと訂正・記録する。そのため、以後は `esptool run` を使用せず、上記の pyserial による RTS パルス起動を正式手順とする。

---

## シリアルを開くと約 1.9 秒で USB が切れる問題の仮説と切り分け調査方法

シリアルポートをオープンした後に約 1.9 秒で USB 切断が発生する現象について、以下の仮説と切り分け方法を定義する。

### 1. 考えられる仮説

1. **RTS/DTR アサーションによるリセット発生**:
   - ホスト側の端末ソフトやドライバがポートオープン時に DTR/RTS をアサートし、USB-Serial-JTAG 回路を介して EN ピンまたは BOOT ピンを駆動して ESP32-C6 をリセットまたはダウンロードモードへ落としている。
2. **USB_SERIAL_JTAG FIFO / 割込み未処理によるホストタイムアウト**:
   - ESP32-C6 内蔵 USB-Serial-JTAG はハードウェア FIFO を持つ。ホストがシリアルポートを開いて受信待機またはデータ送信した際、ターゲット側で USB-Serial-JTAG のエンドポイント/FIFO 読み書きやステータスクリアが一切処理されない場合、ホスト側 USB コントローラ (macOS IOUSBFamily / AppleUSBHostPlatform) が応答途絶やエンドポイントストールを検出し、USB デバイスをタイムアウト切断・リセットする。
3. **MWDT (Main Watchdog Timer) / RTC WDT の未餌付け (Time-out Reset)**:
   - IDF ブートローダまたはハードウェアデフォルトにより有効化されたウォッチドッグタイマ (MWDT0 または RWDT) のタイムアウト周期が約 1.9 秒前後であり、Shizuku 側で WDT の給餌 (feed / reset) や停止 (disable) を行っていないため、WDT が発火してチップリセットがかかり USB が切断・再列挙される。

### 2. 切り分け調査方法

1. **DTR/RTS 線の分離テスト**:
   - ポートオープン時に `dtr=False, rts=False` を明示的に指定したテストスクリプトを実行し、1.9 秒後の切断が発生するか確認する。
   ```python
   import serial, time
   s = serial.Serial('/dev/cu.usbmodem1101', 115200, dtr=False, rts=False)
   t0 = time.time()
   try:
       while time.time() - t0 < 5.0:
           time.sleep(0.5)
           print(f"Alive at {time.time() - t0:.1f}s")
   except Exception as e:
       print(f"Disconnected at {time.time() - t0:.1f}s: {e}")
   finally:
       s.close()
   ```
2. **JTAG 経由での WDT レジスタ状態の読取 (非破壊 sysbus read)**:
   - 切断直前の稼働中、または起動直後に OpenOCD sysbus read で MWDT / RTC WDT の設定・タイマ値・リセット要因レジスタを確認する:
     - `TIMG0_WDTCONFIG0_REG` (`0x60008048`)
     - `RTC_CNTL_WDTCONFIG0_REG` (`0x6000B090`)
     - `LP_CLKRST_FOSC_CNTL_REG` / reset reason
   - WDT がカウントアップしているか、あるいは stage 動作でリセットがかかっているかを同定する。
3. **USB CDC / JTAG エンドポイントのハンドリング確認**:
   - シリアルを一切オープンせず JTAG のみで 10 秒以上安定稼働するか確認 (JTAG のみで安定し、シリアルを開いた瞬間だけ 1.9 秒で切れる場合は仮説 1 または 2 が濃厚)。
