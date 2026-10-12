# cost3 区間対応・境界回数・O2 候補 (報告のみ)

2026-10-07。製品コード修正・ビルド・commit/push・実機操作なし。既存 dirty state と成果物を保持。

## 結論

- MPU body は call1 合計 80 cyc / 5 更新 = **16 cyc/更新**。mpu_skip は書込み 0 でも body 135 cyc、untraced call1 は 5774 → 5830 (+56)。この構成・測定条件ではレジスタ書込み省略は利益なし。
- **to=mpu_begin は MPU 時間ではない**。dispatch 後半・RETURN の context 複写・pending-step の CAS 等が主な対象。exception_entry もハード例外入口だけではなく、直前の handler の仕事を含む。
- call1 は **SVC 5 回、例外復帰 5 回、push 3 回、pop 3 frame**。GET_CURRENT_OBJECT は **SVC 2 回、例外復帰 2 回、push/pop 各1 frame**。回数削減と context copy 削減が MPU 書込みより有力な設計対象。
- O2 の停止はログから **stack exhaustion (stage=808)** まで絞れる。しかし fs_fault の PC/LR は valid な fault 記録と認められず、**LR 破壊は未確定**。

## 1. 区間の定義と実コード対応

計測実装 `source/selftest/call_cost.cpp:193-207` は「直前 event → 今の event」の差を到着 stage ごとに加算する。関数の inclusive time ではない。すべて hook の前回後半・今回前半を含み、IRQ も入り得る。call1 の hit と success 分岐から以下の predecessor を復元した (event ごとの raw dump は未取得)。

以下の略号:
- A = `modules/pico_sdk_support/rp2350/armv8m_ctx.S`
- G = `modules/pico_sdk_support/rp2350/arch_glue.cpp`
- D = `source/kernel/dispatch.cpp`
- H = `source/kernel_object/handler.cpp`
- C = `source/selftest/call_cost.cpp`
- T = `source/kernel/thread.cpp`

|到着区間|call1 min 合計 cyc|hits|合計/hits cyc|含まれるコード範囲 (現行)|
|---|---:|---:|---:|---|
|mpu_begin (10)|1448|5|289.6|3 回は push_end(5) → do_call/dispatch 後半: D:184-195 または D:324-343。2 回は dispatch(3) → RETURN: D:242-265、D:148-163 の pop (2 frame / 1 frame)。全5回で G:42-44 → T:463-466 pending-step の CAS、A:71-73 の hook 開始まで。MPU store はまだ実行していない。|
|push_end (5)|1297|3|432.3|push_begin(4) → D:74-144。サイズ・reserve/limit 検査、header メタデータ、120B context 複写 (D:119)、例外 frame 複写 (D:122)、normalize/幾何検査、top/depth 更新。copy 本体は freestanding_mem.hpp:20-45。|
|exception_entry (1)|1029|5|205.8|最初は C:59-60 → arch header:415-423 の register setup/SVC → hardware stacking → A:131 の hook。primitive CALL の回は H:127 から H:handle/CALL_METHOD → H:call_method の validation/ledger scan/request 作成 (H:534-576) を含む。primitive RETURN の回は H:127 → exit_method (H:591-618) を含む。残り2回は return_stub(8) → A:175-181 → stacking → hook。よって205.8は純粋な例外エントリ費用ではない。|
|context_saved (2)|656|5|131.2|entry(1) → A:34-60 の CTX_SAVE、G:14-16 → D:56-58 current_context、EXC_RETURN と整数/FPU callee-saved 退避 → A:133。|
|mpu_end (11)|643|5|128.6|mpu_begin(10) → A:74、G:52-100 (current_context、profile 判定、region 2 計算/2 or 3 store、mpu_body 集計) → A:75。実 store 部を含む body の合計は別計測80 cyc。|
|restore_end (12)|602|5|120.4|mpu_end(11) → A:77-117。current_context 再取得、r4-r11/PSP/EXC_RETURN/PSPLIM/CONTROL 復元、ISB、必要ならFPU復元。**BX LR と hardware unstack はこの区間に入らない**。|
|push_begin (4)|396|3|132.0|dispatch(3) → D:203-218 の thread/kind/number 分岐、その後 object dispatch は D:300-312、primitive CALL は D:223-230 + D:170-183 の request 複写/検査 → D:73。|
|dispatch (3)|328|5|65.6|context_saved(2) → A:134 の BL、G:19-21 の glue、D:198-199 の hook。dispatch 本体はその後。|
|handler_begin (6)|164|2|82.0|restore_end(12) → A:118-119 の EXC_RETURN/BX、hardware unstack、H:122-127 の trampoline prologue/hook。CALL_METHOD と EXIT_METHOD の2回。|
|return_stub (8)|141|2|70.5|1回は restore_end(12) → leaf 実行/return → A:167-172、もう1回は handler_end(7) → H:130 return → A:167-172。leaf 自身の仕事も混ざる。|
|handler_end (7)|106|1|106.0|primitive RETURN 後の restore_end(12) → H:579-585 call_method 後半 → H:handle の switch/return → H:129。EXIT_METHOD は正常時 non-returning なのでこの marker を通らない。handler 全体時間ではない。|
|caller_resume (9)|81|1|81.0|最終 restore_end(12) → A:118-119 / hardware unstack → arch syscall_result (arch header:424) → C:61。|

全 hook は G:208-222 (enabled 確認、PRIMASK 保存/禁止、CYCCNT 読み、配列保存、PRIMASK 復元)。asm 側は A:13-16 の24Bレジスタ保存/復元も加わる。

上表の「合計/hits」は **混合した invocation の算術平均**であり、各 invocation の min を実測した値ではない。今回の区間 min の和は **6891** で trace_total min と一致する。mean の和は **6928**、trace_total mean は6929 (各区間の整数切捨てによる差1)。この測定では時間が全部区分されており「行が欠けた」の問題は解消している。ただし一般に独立minの和が全体minと一致する保証はなく、見積りにはこの制約が残る。trace_total 6891 と untraced 5774 の差1117も、別採取の min 差であり hook 単体の厳密な費用ではない。

## 2. 境界越えの内訳

call1 の成功経路 (leaf は privileged、専用 handler なし):

|SVC番号|Thread 側の発行元|dispatch の仕事|例外復帰先|push/pop|
|---|---|---|---|---|
|1|caller: CALL_METHOD (C:60)|D:300-343 親kobjへ転送|kobj handler CALL_METHOD (H:127)|push A (dispatch frame)|
|2|kobj call_method: primitive CALL (H:575-576)|D:219-236 → do_call|leaf (LR=return_stub)|push B (call frame)|
|3|leaf が return、共通 return_stub: RETURN/EXIT_METHOD (A:175-181)|leaf は非kobjなので D:300-343 親kobjへ転送|kobj handler EXIT_METHOD|push C (dispatch frame)|
|4|exit_method: primitive RETURN count=2 (H:606-618)|D:242-264、pop C+B|#2 の SVC 直後、call_method 継続|pop 2|
|5|call_method handler が return、return_stub: RETURN (A:175-181)|kobj 本人なので直接 primitive RETURN count=1|#1 の SVC 直後、元caller|pop A|

hits = entry/saved/dispatch/mpu_begin/end/restore_end が各5、push 各3、handler_begin 2、handler_end 1、return_stub 2、caller_resume 1。caller_pre 1を含め43 events と一致 (`cost2_trace.log:55-57`)。

GET_CURRENT_OBJECT の svc 単体: caller SVC → push A → kobj GET handler → normal C return → return_stub の SVC → kernel primitive RETURN/pop A → caller。従って entry2 / push1 / MPU更新2。**svc 往復の return_stub SVC が2回目の境界**。MPU更新はレジスタ条件にかかわらず全例外復帰で hook が走るため2回で、GET自身が保護ドメインを要求したという意味ではない。

## 3. 回数削減案と数字の扱い

見積りは既存 trace の当該部品が消えると仮定した **hook込み設計予算**。新設計の untraced 改善量を測った値ではなく、ledger検査・コピー・エラー処理の移動先の費用も別途必要。独立minの混合による誤差を含む。

1. **同期 GET などを SVC handler 内で処理し、その同じ例外から callerへ返す (2→1 SVC、push/pop1→0)**。
   - push417 + push前91 + handler-entry82 + handler-end130 + stub57 + 復帰2回目の save/dispatch/MPU-wrapper/restore 1組 (130.5+65.5+128.5+120.5=445) = **約1222 traced cyc の部品予算**。RETURN側popを含むmpu_beginのうち消える量、hardware-entry/unstackの変化は未分離なので上乗せを断定しない。重複して数えないこと。
   - raw svc1985から1222を引いて「763になる」とは予測しない。hook除去や新inline dispatchの費用を含む実機比較が必要。
   - kernel機構/kobj方針の分離を維持するなら登録した同期 handler callback を Handler mode 内で呼ぶ設計、またはkobj/ARCH側のstatic fast pathという別案。blocking/yield/nestを許すAPIと分け、current identity・error・IRQ priority・stack limitを再定義する必要。
2. **CALL_METHOD の入口でkobjのcall requestを確定し、そのSVCから直接leafへ (5→4、push3→2)**。
   - kobj→kernel primitive CALL の #2 を省く案。既存1入口の save+dispatch+mpu-wrapper+restore 約446 traced cyc と push約432、push前約132、dispatch後段の一部が対象: **概ね878～1010 traced cyc の部品予算** (exception_entry / mpu_beginは異種の仕事が混じるため加算しない)。
   - validation/ledger/request構築は消えず移る。leafから直接callerに返せる設計まで畳むと元kobj continuation不要となり、さらにframe Aも減らせる。
3. **leafのreturn stub SVCをkernelが直接扱い、EXIT_METHODへ転送してからprimitive RETURNする #3+#4 を1回にする (5→4、push3→2)**。
   - ledgerのvia/kind/claimで本人性を確認し、returnの意味をkernelが確定できるプロトコルが必要。既存1入口と1dispatch pushを除く **約878 traced cyc + EXIT_METHOD準備/後段の未分離分** が部品予算。
   - 親handling objectがEXIT_METHODを処理/forwardする意味論を維持する。root-only fast pathを用意し、専用handlerは従来経路へ戻す案もある。
4. **CALL開始/終了を各1SVCに統合 (5→2)**。
   - 1回あたり約446 traced cycのsave/dispatch/MPU-wrapper/restoreを3回、dispatch用pushを2回程度除ければ **約2200～2500 traced cyc の部品予算**。現行の5回全ての平均205.8entryや289.6mpu_beginをさらに丸ごと足さない (移す仕事と重複する)。新ABI/方針分離/identity/NO_STACK/専用handler/return検算の大きい設計変更。
5. **同じprivilege/regionなら restoreのCONTROL/MPU/step検査を軽くする**。
   - register-only省略は既に効果なし。remainingはcurrent_context再取得、pending-step CAS、context restore、profile更新など。debug_stepを予約フラグで守る、restoreへcontext pointerを渡す、region計算の事前化が候補。CAS/lookup自身の個別測定なしなので削減cycは未算定。

「returnを例外復帰で直接行う」は、普通のThread mode C returnをそのまま EXC_RETURN に変えることではない。現在のEXC_RETURN処理はHandler modeのA:118-119。Thread modeで単にBX EXC_RETURNする案は成立を示せない。同期SVC処理をHandler modeに閉じる、または1つのRETURN SVC内で元caller frameを選んで例外復帰する構成を検討する。

## 4. O2 の確定範囲と候補

実ログ `cost2_o2_diag_nofp.log:26-44` は6段成功、depth/identity成功、unknown API成功、absent object成功、watchdog reset **stage=808**。停止は `source/selftest/call_ladder.cpp:310-311` の deep 呼出しの内側 (stack exhaustion) に絞れる。808の後の809を通った証拠は無い。

**fs_fault はLR破壊の証拠にしない。**
- `firmware/main.cpp:339-361` はscratch[4..7]を読み、magic検証なしでfs_fault/pc/lr/xpsrと印字する。
- 対応するwriterは `flash_fs.cpp:17-29` の **RP2040限定**で、magicはFA17FA17。Pico2W/RP2350のログの6ab73121は一致しない。過去値/別用途/未初期化値の可能性を分離できない。
- validなM33 faultは G の shizuku_selftest_fault_pre/read2 と firmware/main.cpp:345-347,367-380 の fault_pre / tick sample。これがログに無いので、破壊したLR/PC値は未取得。watchdog stageは別のSPRG magicで検証される (main.cpp:333-338) ため808は使える。

候補 (未確定、検証順):

1. **stack exhaustion境界・alignment・復帰frame幾何**: deep()はO2 nofpでpush {r4} (4B、同梱 .deep.dis)、SVC時に4Bずれる。ARCH:100-115 はframe32/104BとxPSR bit9 paddingを別扱い。D:74-112 のreserve検査 / caller_stack計算、D:119-131 のcopy/normalize/検算、D:148-162 のpopを、失敗境界のPSP/PSPLIM/frame/LR/xPSRで照合する。4B prologue自体を直ちにABI違反と断定しない (通常BLしないleaf)。
2. **asm→CのMSP alignment**: A:35-36はpush {lr} (4B)の直後にC関数をBLする。MSPが入口で8B整列ならC呼出し時4Bずれる。現current_context ELFがstack不使用でも、一般のC ABIを満たさない構造の候補。SVC最高優先度だからそれだけで破壊原因とは言えない。
3. **syscall inline asmの保存契約**: arch header:415-423 はr0/r1のみ+readwrite、r2/r3/r12はinput、lr/cc clobberなし。設計どおり元のhardware frameとxPSR/LRを完全復元するなら成立するが、RETURN/NO_STACKのどれかでscratch/LR/flagsが保持されないならO2がその保存を仮定して壊れる。**clobber不足と確定せず**、deepのSVC前後の原frame/LR/r2/r3/r12とD:264のset_resultを確認。
4. **tail call**: archived O2 nofp deep()はSVC後にerror判定、r4復元、BX LR。deep自身が単純なtail recursionに変わった証拠は無い。kobj handle/call_method/replyのtail分岐、saved continuation LRの対応を同じELFで調べる余地あり。
5. **naked/hook**: A:13-16はr0-r3/r12/lrを保存してhookを呼び、hookはC ABI上callee-savedを保持する構成。A:117-119はr3のEXC_RETURNをPROFILE後に使うので実ASMで保存を再点検。RP2040-onlyのfs_fault naked関数をRP2350故障の実行コードとして扱わない。RP2350 fault入口/診断のframe解析が正しいかはvalidなEXC_RETURNとfaultレコードが必要。

次に必要な証拠: stage808の最後の正常deep depth・PSP/PSPLIM、NO_STACK拒否前後のframe LR/xPSRとcontext sp/EXC_RETURN、valid magic付きM33 CFSR/HFSR/PC/LR。観測のためframe pointerを強制しないSP採取を使い、正しいcost2_o2_diag_nofp.elfにaddr2line/objdumpで照合する。修正・再ビルドは今回行っていない。
