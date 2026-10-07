#set document(title: "Shizuku V3 カーネル設計", author: "Shizuku")
#set page(paper: "a4", margin: (x: 18mm, y: 18mm), numbering: "1")
#set text(font: ("Hiragino Sans", "Arial"), size: 9pt, lang: "ja")
#set par(leading: 0.65em)
#set heading(numbering: "1.1")
#show raw: set text(font: "Menlo", size: 7.5pt)
#let ref(s) = text(size: 7.8pt, fill: rgb("555555"), s)
#let tbl(..cells) = {
  let values = cells.pos()
  table(columns: (auto, 1fr, auto, auto, auto, 1fr), inset: 4pt,
    table.header(repeat: true, ..values.slice(0, 6)), ..values.slice(6))
}

= Shizuku V3 カーネル設計

状態: *設計案。製品コード未実装・性能未測定*。2026-10-07。
対象は `perf/svc-in-ram`、HEAD `3239d3761fd1898b78411434a31f91790251bd3b` と既存 dirty ファイル。
根拠は `ota-artifacts/cost3_report.md` / `cost2_report.md` と現行ソース。
本書だけを作成する。V2 記録 `docs/record/kernel_v2_record.typ`、製品・テスト・設定は編集しない。
以下のシンボル、構造体、フラグ、命令列は、現行コードの引用と明示したものを除き V3 の提案である。

== 固定した判断と設計の結論

ユーザー決定 1–11 を実装の前提にする。現行を V2 として残し、V3 でハンドラそのものを見直す。
カーネルとカーネルオブジェクトを一つのカーネルとして扱い、Thread mode のカーネル処理は、その世代のカーネル actor ID を持つ。
カーネル actor を挟んで primitive CALL/RETURN を再発行する V2 の二段構えは廃止する。
actor ID はホットスワップで変わる。ID 定数ではなく、世代付きの公開 descriptor から取得する。

syscall の実行モード・保存レベル・権限・ISR 可否は API ID ごとの静的属性で決め、template から入口、検査、分岐を生成する。
object のコード、親ハンドラ登録の有無、初回の成功／失敗で Handler/Thread を選び直さない。
GET、短い stream batch、atomic は Handler mode で完結する。待ち・譲渡・入れ子は Thread mode で行う。
D1 による分離は要求しない。MPU/PMP 保護は維持し、書込みの少なさだけを最適化の目的にしない。

専用 wait/wake 待ち行列は作らない。mutex は CAS と持ち主への宛先指定 YIELD、値待ちは再確認と YIELD、実行権の貸付けは RUN_FOR、時間待ちは SLEEP_US で組む。
atomic は公開 ABI に載せる。省電力用の待ち状態・WFI 連携は将来の別変更とする。
ESP-IDF の Wi-Fi OSI / BLE NPL shim は `esp-idf-support` 内の関数として閉じ、object にしない。
小さい `kernel_services` 契約の裏側を V2/V3 が実装し、C6 の当面の V2 移植を妨げない。

=== ID の二つの名前空間と現行との差

固定 object ID と syscall API ID は別の名前空間である。「40 ID + count」を syscall 40 個と読み替えない。
本 worktree は `BUILD.bazel:12–35` による object ID 生成を使い、`//abi` ディレクトリは未配置である。
公開 ABI の参照資料として、読取のみ行った別 worktree の
`/Users/ishigakiyua/github/Shizuku-ota-test/abi/include/shizuku/object_ids.hpp:8–48` は、40 個の名前付き object ID と `count=41` を定義する。
番号 1 はこの名前付き表にない。`abi/BUILD.bazel:3–17` はチップ非依存の公開ヘッダを持つ。
この資料は別 worktree のスナップショットであり、本ブランチへ適用済みとは扱わない。

V3 は、この 40 個の公開 object ID と count、公開 method 番号を変更しない。
syscall の現行語彙は `internal_headers/shizuku/object_api.hpp:15–139` の 1–31 であり、atomic の数値 ID は未定義。
V2 primitive の 1–4 はさらに別の内部語彙である (`kernel_abi.hpp:28–44`)。
移行前に上層が使用する `//abi` 正本の revision を確定し、現行 stream の ABI 変更を含めて同期する。
未定義 atomic 番号を空いているように見える番号へ勝手に割り当てない。

== V2 の何が遅いか

下表は cost3 の到着区間表の主要行を引用したもの。150 MHz RP2350 の既存 profile 条件で、値は hook を含む。
区間は「前 event → 今 event」であり関数の inclusive time ではない。

#table(columns: (1.1fr, auto, auto, 2.3fr), inset: 4pt,
  table.header(repeat: true, [到着区間], [合計 cyc / hits], [合計÷hits], [実コード／解釈]),
  [context_saved], [656 / 5], [131.2], [CTX_SAVE、context 取得、整数／FP callee-saved 退避。`cost3:29`],
  [dispatch], [328 / 5], [65.6], [glue と dispatch 入口まで。本体の時間ではない。`cost3:33`],
  [mpu_end], [643 / 5], [128.6], [lookup・領域計算・hook を含む wrapper。MPU body は別計測。`cost3:30`],
  [restore_end], [602 / 5], [120.4], [整数／PSP／CONTROL／PSPLIM／FP 復元。BX と unstack は含まない。`cost3:31`],
  [push_end], [1297 / 3], [432.3], [容量検査、header、120B context と exception frame 複写、幾何検算。`dispatch.cpp:74–144`、`cost3:27`],
  [mpu_begin], [1448 / 5], [289.6], [dispatch 後段／RETURN の pop／pending-step CAS。MPU store 前。`cost3:26`],
  [exception_entry], [1029 / 5], [205.8], [前の Thread handler の仕事も含む。純粋な hardware entry 費用ではない。`cost3:28`],
)
#ref("表の cost3 は ota-artifacts/cost3_report.md:24–37。全区間合計と hook の制約は同:39–41。")

save + dispatch 入口 + MPU wrapper + restore は 1 境界あたり約 446 traced cyc。
MPU body は call1 の 5 更新合計 80 cyc、16 cyc/更新であり主因ではない。
書込み省略版は body 135 cyc、untraced call1 は 5774 → 5830 cyc で、この条件では利益がなかった (`cost3:7`)。
16 cyc は RP2350 のこの測定であり、RP2040/C6 の保護更新コストではない。

V2 の GET_CURRENT_OBJECT は SVC 2 回、exception return 2 回、push/pop 各 1。
method 1 段は SVC/return 各 5 回、push 3 回、pop 3 frame。
caller → 親カーネル actor → primitive CALL → leaf → 親 EXIT_METHOD → primitive RETURN → カーネル handler return という中継が増やしている。
`cost3:47–57` に全 5 境界の対応がある。2 段の実測 hits は各 10 回、push 6 回 (`cost2_trace.log:71–74`)。

`cost2_trace.log:20–22,143` の untraced 表示は空枠を引いた svc 1985 / call1 5774 / call2 11642 cyc。
厳密な raw min は svc 2000 / call1 5789 cyc であり、引いた空枠は 15 cyc。
cost3 の “raw svc1985” は hook 無効値という意味で使われているため、本書では net と raw を区別する。
traced call1 は 6891 cyc。独立 min の和、traced/net の差を、新設計の改善量へそのまま換算しない。
CALL 開始／終了を各 1 回にする場合の 2200–2500 traced cyc は削除候補部品の予算であり、net 5774 から引ける実測差ではない (`cost3:61–78`)。

== 静的属性と template dispatcher

=== 属性の契約

各 API の traits は少なくとも `permission`, `from_isr`, `mode`, `save_level`, `permission_predicate`, `work_bound` を持つ。
permission は「非特権可」または「特権のみ」。さらに ownership、親子関係、特殊 flag に必要な authority を predicate で検査する。
特権であることと composition 権限は別である。Handler mode に入った事実を根拠に、元の非特権 caller を特権と判定してはならない。
caller の保護状態・actor は保存した origin と特権 ledger から取得する。
ISR の直呼びには、あらかじめ登録した信頼済み driver identity を使う。割込みで中断した object の identity を driver の権限へ流用しない。

以下は現行 API 31 個の V3 分類案である。H = Handler 内完結、T = Thread mode の kernel actor。
S0 = 最小入口＋C ABI 保存、S1 = 整数 continuation、S2 = S1＋活性 FP state (arch 能力に応じる)。
表の S1 は FP 有効 thread では S2 に拡張する。ISR ○ は、有界・非ブロック実装が受入試験を通った場合にだけ公開する。
ISR × は同じ ID を IRQ 内で呼んでも Thread 実行へ fallback せず、副作用なく拒否する。

#tbl(
  [API ID], [名前], [権限], [ISR], [mode/save], [追加条件・理由],
  [1], [CREATE_OBJECT], [非特権可], [×], [T/S1], [REPLACE は特権、HANDLER 宣言は composition authority。allocation／公開。],
  [2], [EXIT_METHOD], [非特権可], [×], [T/S1], [本人の call ledger、levels、return 権限を検算。primitive への読み替え禁止。],
  [3], [EXPORT_METHOD], [非特権可], [×], [T/S1], [owner=self。entry 検証、世代を公開。],
  [4], [CALL_METHOD], [非特権可], [×], [T/S1], [対象・method・領域・深さを検査。入れ子。],
  [5], [GET_CURRENT_OBJECT], [非特権可], [×], [H/S0], [Thread の caller identity を答える。ISR に object self は定義しない。],
  [6], [GET_CALLER_OBJECT], [非特権可], [×], [H/S0], [論理 call parent を答える。kernel 中継枠を caller に見せない。],
  [7], [SPAWN], [非特権可], [×], [T/S1], [method 検査、stack/ledger 確保。],
  [8], [YIELD], [非特権可], [×], [T/S1], [宛先 thread を検査。grant 借り手なら貸し手へ戻す。],
  [9], [RUN_FOR], [非特権可], [×], [T/S1], [cycles、残る grant 範囲、thread 所有を検査。],
  [10], [EXIT_THREAD], [非特権可], [×], [T/S1], [self の終了。実行中 stack はその場で解放しない。],
  [11], [SLEEP_US], [非特権可], [×], [T/S1], [時間待ち。deadline と overflow を検査。],
  [12], [SET_BUDGET], [非特権可], [×], [T/S1], [現行許可範囲を維持する案。特権限定化は別途判断。],
  [13], [MEMORY_ALLOCATE], [非特権可], [×], [T/S1], [self に allocation。走査・allocation。],
  [14], [MEMORY_RELEASE], [非特権可], [×], [T/S1], [owner を検査。allocation 表更新。],
  [15], [MEMORY_HAND_OVER], [非特権可], [×], [T/S1], [owner と受取先を検査。],
  [16], [MEMORY_OWNER], [非特権可], [×], [H/S0], [有界 snapshot と handle 検査へ置換。既存 table_lock を流用しない。],
  [17], [DECLARE_NAME], [非特権可], [×], [T/S1], [owner=self、未命名。書込み権限検査。],
  [18], [OBJECT_NAME], [非特権可], [○], [H/S0], [対象の有界 immutable snapshot。文字列を比較／copy しない。],
  [19], [STREAM_CREATE], [非特権可], [×], [T/S1], [形状・flags・積 overflow と allocation。],
  [20], [STREAM_OPEN], [非特権可], [○], [H/S0], [世代照合・有界 pin。席を変えない。],
  [21], [STREAM_BIND], [非特権可], [×], [T/S1], [self の SPSC 席。driver 内部席は別 authority。],
  [22], [STREAM_CONNECT], [非特権可], [×], [T/S1], [席・DMA 接続／資源確保。],
  [23], [GRANT_REGION], [特権のみ], [×], [T/S1], [有効 object、領域表現、descriptor 世代更新。],
  [24], [SET_OBJECT_AFFINITY], [非特権可], [×], [T/S1], [現行許可範囲維持案。次の SPAWN にのみ適用。],
  [25], [KILL_THREAD], [特権のみ], [×], [T/S1], [protected thread／貸し手／実行状態。self kill も T。],
  [26], [STREAM_DISCONNECT], [非特権可], [×], [T/S1], [要求は非同期。転送完了をこの API 内で待たない。],
  [27], [FORWARD_CHILD_EXIT], [特権のみ], [×], [T/S1], [信頼済み親 role と ledger の child 関係。特権だけでは許可しない。],
  [28], [STREAM_DESTROY], [非特権可], [×], [T/S1], [owner、DMA busy、users、寿命。],
  [29], [STREAM_PUSH_N], [非特権可], [○], [H/S0], [席、generation、範囲、最大 records/bytes、有界 pin。],
  [30], [STREAM_POP_N], [非特権可], [○], [H/S0], [同上、lost と copy 中追越し検査。],
  [31], [STREAM_AVAILABLE], [非特権可], [○], [H/S0], [有界 pin、capacity で飽和。],
  [未定], [ATOMIC (CAS / SWAP / FETCH_ADD)], [非特権可], [○], [H/S0], [32bit 整列、合法な共有 writable memory、ordering と有界排他。],
)

ISR stream 呼出しは driver 所有 buffer と driver 席であり、user svc の「origin thread stack 内」という copy 範囲をそのまま使わない。
表の permission は entry の許可であり、対象資源の権限検査を省く意味ではない。
現行 GRANT_REGION、KILL_THREAD、FORWARD_CHILD_EXIT の検査は `handler.cpp:507–509,961–966,628–633`。
CREATE_OBJECT の条件は同 `373–398`。FORWARD_CHILD_EXIT の特権条件は V3 の信頼済み親 role 契約として明示した案で、現行は interposed ledger 関係を主に検査している。
SET_BUDGET / AFFINITY 等を一律に特権限定へ変更する根拠は今回与えられていないので、表で黙って強化しない。
CREATE_OBJECT については、現行が非特権 creator の生成先 privilege を制限しているという証拠は `handler.cpp:368–435` にない。
V3 の permission predicate には「非特権 origin がより強い権限の actor を生成しない」条件も必要となる。
既定 flags の意味を変えずにこの条件を導入する場合の拒否範囲は、互換性／権限判断として移行前に確定する。

=== 生成する入口と分岐

以下は構造を示す擬似 C++。API traits の数は syscall enum の正本から生成し、固定 object ID の count を流用しない。
arch 入口にも同じ traits から最小 ID 分岐を生成し、T の original callee-saved はこの C++ dispatcher を BL する前に捕捉する。
以下の `EntryView` はその保存済み continuation を参照する。`prepare_kernel_landing` が C++ 内の live r4–r11 を caller の値と見なして保存する実装ではない。

#block(breakable: false)[
```cpp
template<Api Id, class Arch> struct svc_traits; // exhaustive; no default traits

template<Api Id, class Arch>
DispatchResult invoke(Origin const& origin, EntryView& entry) {
  using A = svc_traits<Id, Arch>;
  if constexpr (A::permission == privileged_only)
    if (!origin.is_privileged()) return reject(NOT_PRIVILEGED);
  if (origin.from_isr() && !A::from_isr) return reject_context();
  if (!A::permission_predicate(origin, entry.args)) return reject_permission();
  if constexpr (A::mode == handler) {
    static_assert(A::bounded && !A::may_yield && !A::may_block);
    return handler_service<Id>(origin, entry); // same exception, no registry callback
  } else {
    return prepare_kernel_landing<Id, A::save_level>(origin, entry);
  }
}

// generated exhaustive switch; ID read from argument register, never SVC immediate
switch (entry.api_id()) {
  case Api::GET_CURRENT_OBJECT: return invoke<Api::GET_CURRENT_OBJECT, Arch>(...);
  case Api::CALL_METHOD:        return invoke<Api::CALL_METHOD, Arch>(...);
  // ... one case per public API; unknown number -> UNKNOWN_API, no parent fallback
}
```
]

traits に未登録 ID があれば compile error にする。ID/引数／戻り値の公開ヘッダはチップ非依存の `//abi`、traits と実装はカーネル側とする。
arch 能力は `if constexpr` で必要命令だけを生成する。既存 `1c4fe7b` の HAS_CYCLE_COUNTER と同じ方式を、HW_STACKING、PSPLIM、FP、MPU/PMP、native AMO 等へ拡張する。
これら新しい能力名は提案であり現在定義済みとはしない。
許可 predicate も traits から呼び、各実装のあちこちで「今 privilege か」を再判定する形を廃止する。
対象存在や所有者の検査は失敗結果を変えるが、実行モードを変えない。

== SVC 入口で保存する最小集合

=== Cortex-M の同じ caller へ返す経路 (S0)

Thread/PSP からの SVC では hardware が r0–r3、r12、LR、PC、xPSR を積む。
入口は EXC_RETURN を BL 前に保持し、stack selector と FP frame 種別から core frame の位置を求める。
基本 frame は 32B、FP 拡張は 104B、整列 padding は別に扱う。FP 拡張時の core frame offset を arch の view に隠し、stack base を常に r0 の位置と仮定しない。
Security extension を使う構成は別の frame 規則となるため、当初の V3 受入対象を現在の同一 security state に限定する。

同一 Thread・同一 PSP/CONTROL/PSPLIM・同一保護領域へ同じ例外から返し、実装も上位 IRQ も AAPCS を守り、context switch／yield／入れ子 syscall を起こさないとき、r4–r11 を thread context へ常時丸ごと退避する必要はない。
C の prologue が使う callee-saved を MSP 上へ保存することは依然必要であり、「r4–r11 を一切保存しない」という意味ではない。
裸の入口／profile hook／途中の assembly も保存契約を守ることを disassembly で確認する。
上位 IRQ は現在の Handler レジスタを保全し、Thread 切替えを外側の例外終了まで延期する。PendSV の tail chaining 後にも元の Thread context を正しく捕捉できることを検証する。

S0 のソフト保存は EXC_RETURN、frame view、必要な origin/IRQ mask の最小 scratch に限定する。
PSP は参照するが変更しない。CONTROL/PSPLIM は origin の確認だけを行い、同一条件なら書戻さない。
同一 MPU/PMP の再計算／更新と pending-step の空 CAS は省く。debug step を予約している場合の処理は削除せず、予約 bit で囲む。
結果だけを元 core frame の r0/r1 に書き、保持した EXC_RETURN で *Handler mode から*復帰する。

=== Thread kernel actor に入る経路 (S1/S2)

static ID 分岐で T と分かった時点で、最初に r4–r11 を caller continuation に一度保存する。
capture 前に C 関数の作業で live registers を書き換えない。入口を caller-saved のみの assembly にするか、C の保存済み MSP slot から完全に復元して capture する必要がある。前者を推奨する。
PSP、stack bounds、EXC_RETURN、CONTROL の caller privilege/SPSEL/FP 状態、必要な PSPLIM と保護 descriptor 世代を保存する。
現行 restore のように「今の CONTROL の FPCA を流用」するだけで異なる FP thread へ移らない。

FP 有効時は EXC_RETURN FType、FPCCR.LSPACT 等を調べ、lazy frame がまだ実体化していないか区別する。
S0 の kernel/hook は FP 命令を出さず、lazy save を不要に発生させない。
異なる context に移る S2 は、arch 規約に従って lazy save を完了し、必要な S16–S31 を一度保存し、S0–S15/FPSCR の hardware 領域を保持する。
kernel landing の FPCA、callee の FP 状態、return 時の FPSCR/high registers を別々に管理する。
非活性 FP の 64B 空領域は毎回コピーしない。FP を使う kernel service は初期 V3 では禁止し、許可するなら専用 traits と保存量／WCET を追加する。

=== Handler 文脈からの呼出し

SVC/ECALL を IRQ 内から再発行することを ISR API としない。
Cortex-M は EXC_RETURN の stack selector に従い MSP の interrupted-handler frame も認識するが、通常の ISR 呼出しは特権 `kernel_services` の直呼びを使う。
ID の from_isr=true、permission、driver authority を同じ template 検査へ通し、H 本体だけを呼ぶ。T は拒否する。
直呼び wrapper は通常の C 引数／戻り値で H 本体と接続し、SVC frame を捏造せず、ISR 自身の復帰を途中で行わない。
SVC 優先度によっては IRQ 内 SVC が安全に入れないため、「SVC を撃てば context 検査できる」という契約にはしない。

== Handler mode の制約と上限

H service の本体、排他、copy、hook を含む最悪時間を `H_max` として board profile で決める。
提案初期値は RP2350 の *service 起因の低優先 IRQ 遅延 10 µs 以下*、うち global IRQ mask 区間 2 µs 以下。
これは未承認の設計予算であり実測上限ではない。高優先 IRQ の実行時間と測定 hook は別に記録する。
優先度を低くして preemptible にしても、同じ shared state を扱う IRQ の再入と lock 順序の証明が必要になる。

- block、yield/grant/sleep、nested SVC、unbounded allocation、user callback、printf、無制限 table scan を禁止する。
- copy は record 数に加えて bytes 上限を持つ。最大 record size が bytes 予算を超える既存 stream の扱いは仕様判断が必要。0 件を永続的に返すだけの設計は採用しない。
- CAS retry／pin／lock acquisition の回数にも上限を証明する。既存 `handler.cpp:1000–1005,1057–1082` の loop をそのまま H へ移すことはできない。
- Thread が持つ lock を同じ core の SVC/IRQ が待つと持ち主が再開できない。共有 short lock は全取得側で該当割込みを遮蔽し、持ったまま譲らず、他 core の保持時間も有限とする。これを証明できない lock は H から使用しない。
- ISR driver と user service の資源範囲を区別し、pointer、長さの overflow、alignment、アクセス権を検査する。pin した descriptor を copy 中に置換／解放しない。

user copy 中 fault は検証済み copy 命令範囲と recovery PC の fixup 表でのみ回復する。
部分 copy の公開／atomic の副作用は線形化点を基準に定義し、失敗時に成功済み操作を再実行しない。
それ以外の kernel fault、MSP overflow、frame 不整合は kernel fault として診断停止する。
SVC priority による fault escalation、M0+ の HardFault しか使えない経路も board ごとに設計・試験する。範囲検査をしただけで fault recovery 完成とはしない。

MSP 上限は入口 scratch + compiler stack-usage + 許可した最大 IRQ nesting + fault 診断余白から算出し、linker 予約と照合する。
全 assembly→C 呼出しで 8B 整列を維持する。現行 `armv8m_ctx.S:35–36` の `push {lr}; bl ...` は一般 C ABI の整列上の検討対象であり、V3 では 8B 単位の scratch を使う。
固定の MSP 1 KiB 等を根拠なく安全とせず、最大 chain を静的集計し、canary/high-water と nesting stress で確認する。

== Thread kernel actor と CALL/RETURN プロトコル

=== 入口、実行、復帰

T syscall の入口は origin continuation を保持し、その SVC の exception return 先を *特権 Thread mode の kernel landing* にする。
landing は専用の kernel stack segment、公開世代の actor ID、サービス ID と検証済み arguments を受け取る。
この時点の current actor は kernel ID、logical caller は origin actor。user arguments を指す pointer は caller の bounds/permissions を用いて検査する。
H syscall は actor を kernel に積み替えず、origin に対する operation として実行する。

kernel service は内部関数として scheduler/call/return 機構を直接呼ぶ。V2 primitive を再 SVC する経路を設けない。
しかし Cortex-M の Thread mode は、PSP/privilege を準備しただけで EXC_RETURN を実行できない。
*普通の C return を例外復帰と見なす実装は禁止する*。
V3 は二つの復帰実装を区別し、arch 能力でビルド時に選ぶ。object 登録状態による runtime fallback はしない。

#table(columns: (auto, 2.2fr, 1.6fr), inset: 4pt,
  table.header(repeat: true, [案], [処理], [境界数への影響]),
  [A: Thread tail transfer], [kernel landing が caller/callee の通常 Thread 状態を専用 assembly で復元し、CONTROL/PSP/MPU を確定して branch。低 privilege へ移る最後の命令列は許可された固定 gate で実行。], [kernel の再 SVC が不要。1 段 CALL の開始／終了を各 1 回にできる候補。],
  [B: 固定 completion trap], [kernel landing から保護された completion portal を呼び、Handler が保存 continuation へ exception return。既存 primitive と別で、ID の意味を actor により再解釈しない。], [単純で検証しやすいが、返却または callee 起動のたびに追加 trap。2 回目標は達成しない。],
)

A が第一候補である。ただし r0/r1 以外の hardware saved registers、LR、APSR flags、FP、整列 padding、kernel register 値の漏洩を含む全契約を満たす必要がある。
B の completion は固定の内部 selector と静的 traits を持つ特権 portal として別に定義する必要がある。
公開 API の ID=2 を actor により内部 RETURN へ読み替えたり、C6 に存在しない ECALL 即値へ selector を埋めたりしない。
現行 inline SVC は r2/r3/r12 を input-only、LR/cc を clobber していない (`armv8m.hpp:415–423`)。
API の数値 ABI を維持しても、これらを壊す tail transfer へ無断で変えられない。
まず既存保存契約を維持する復元 gate を証明する。clobber 契約変更を選ぶ場合は公開 wrapper の変更と全利用者の rebuild を別の受入条件とする。
RISC-V の M-mode では MRET により M/U と interrupt state を復元できるが、mepc/mstatus を保存して任意の trap nest と区別する必要がある。
Arm の Thread return の制約を C6 に機械的に移さない。

=== CALL 開始

`CALL_METHOD=4` は常に T/S1 (必要なら S2)。API ID・origin 検査後、kernel landing が object/method の immutable descriptor を取得・pin し、entry、権限、親 binding、保護領域、最大深さを検査する。
logical method frame を一枚 commit し、callee actor と caller actor を台帳へ記録する。
callee が kernel actor を名乗る request、任意 PC、任意 privilege を user 引数だけで受理しない。
失敗は frame/台帳/参照数を commit 前へ戻し、元 caller に object error を返す。
callees の途中 fault と終了用の stack reserve も含め、CALL 成功直後に RETURN を発行できる余白を確保する。

tail transfer gate が callee stack segment と method 引数、normal LR=return_stub、保護属性を適用して callee に移る。
root カーネル用 dispatch frame と primitive CALL frame を別々に積まない。
callee 起動後に用いない kernel landing の一時 C frame は continuation として保持しない。
kernel code と caller/return gate が保護変更後にも到達可能であること、低 privilege 移行後に特権 descriptor を読まないことを arch 実装で保証する。
actor、PSP、CONTROL/FP 状態、保護 domain を変更する最後の gate は、scheduler が途中状態を保存しない有限の transition 区間とする。
mask 可能な IRQ に対して状態公開を整合させ、NMI/fault の診断は transition record を識別する。Thread 復帰直前の IRQ と PendSV も受入試験に含める。

=== CALL 終了

method の通常 return は共通 stub から `EXIT_METHOD=2` を一回発行する。
発行 actor、method frame 世代、thread 世代、depth、親 continuation、許可した levels を特権 ledger で検算する。
ID=2 は kernel actor から出ても「primitive RETURN」には化けない。internal 完了は protected continuation の別の状態遷移で表す。
kernel landing が result/error と必要な復元情報を保持し、対象 logical frames を閉じ、A の復元 gate で元 caller の SVC 直後へ戻す。
pins の最後の解放は、その世代のコード／descriptor を参照し終えた後に行う。最後の pin を落としながら旧世代内の C epilogue を実行することは許さない。
不正 levels、偽造 claim、既に終了した token は台帳を変更せず拒否する。
複数段 RETURN も logical frames を単位に検査し、V2 の物理 frame 数 `2 * levels` をそのまま用いない。
Thread entry 自身の return は EXIT_THREAD 相当とし、存在しない caller に pop しない。

=== frame に積むものと copy の廃止

#table(columns: (1fr, 2.7fr), inset: 4pt,
  table.header(repeat: true, [領域], [内容]),
  [特権 call descriptor], [prev、thread generation、logical call depth、caller/callee actor と generation、origin privilege、parent binding/policy continuation、kind、phase、result/error、kernel generation pin、object/region descriptor pin。user 書換え不可。],
  [arch continuation], [元 HW frame への参照、PSP/stack bounds、EXC_RETURN、r4–r11、必要な CONTROL/PSPLIM、活性 FP high registers の別 block。C6 はソフト trap frame。],
  [callee 初期状態], [entry、通常 return stub、引数、callee stack segment と保護 descriptor。caller HW frame の全複製ではなく必要な state を構成する。],
)

基本整数 continuation は 32B の callee-saved と数語の metadata が中心となる。
FP block は活性時だけ用意する。120B `context_t` 全体を push/pop ごとに memcpy する構造をやめ、保護領域／stack bounds は immutable descriptor への参照にする。
ただし descriptor と台帳の全体が 32B になるという見積りではない。構造体の実サイズは実装時に size/assert で確定する。

元 HW frame を参照する間、その stack 領域を再利用しない。
kernel/callee の downward-growing segment を元 frame より下へ確保し、境界／guard を明示する。
同一 stack の別 segment は性能上の候補だが、callee から caller frame が writable な場合は PC/privilege 復帰情報として信用できない。
MPU/PMP が frame を隔離できなければ、制御用情報を特権側へ必要分だけ copy し検算する。安全に参照できない場合の必要 copy までゼロにはしない。
IRQ/他 core の thread migration、kill、hot swap 中にも stack/descriptor lifetime を保持する。

=== 境界回数の目標と成立条件

「境界」は SVC/ECALL 発行 1 回とその trap からの復帰 1 回の組で数える。
Thread mode の tail branch による actor／保護ドメイン移行は別に数える。

#table(columns: (1.5fr, auto, auto, 2fr), inset: 4pt,
  table.header(repeat: true, [仕事], [V2 trap 回数], [V3 目標], [条件]),
  [同期 H syscall 単体], [2], [1], [同例外内完結。push/pop=0。],
  [一般 T service 単体], [少なくとも 2], [入口 1], [A による Thread 復元。B は入口＋completion の 2 回以上。内部 YIELD 等は別途。],
  [method call 1 段], [5], [2], [開始 1、終了 1。A が成功し、追加 parent/user syscall を含まない。],
  [method call 2 段], [10], [4], [CALL/EXIT 2 組。追加 service の trap は別。],
)

1 段 CALL の trap 2 組でも、caller→kernel→callee→kernel→caller の actor/domain 移行は最大 4 回ある。
MPU 更新が必ず 2 回になるとは限らない。保護 domain が実際に変わるたびに必要な更新・barrier を数える。
B を選ぶ board は「2/4 回達成」と報告しない。Thread service の waits/scheduler/timer trap も数に含めて別途表示する。

== parent handler routing と静的経路の両立

V2 は `dispatch.cpp:298–343` で全 object syscall を登録 parent handler に配送し、未登録なら panic。
専用 handler の親は root 一段に固定されている (`同:329–334`)。
`handler_binding.cpp:60–78,102–145,213–239` は、同じ番号 0x10 を親 A では +100、親 B では +200 と解釈し、child の ID=2 を return 要求以外の独自要求としても使う。
0x10 は標準 API の MEMORY_OWNER=16 と衝突する。この生 syscall 再解釈をそのまま残すと、ID ごとの固定意味／Handler 経路と両立しない。

V3 で保持する意味論は、親の自動導出、親ごとの method policy、親自身の再入、child 終了の本人性、拒否時の frame 不変である。
標準 syscall を横取りできることは固定決定 5 により維持しない。
第一案は、専用 parent の操作を既存 CALL_METHOD の *明示的な method 呼出し*へ移すこと。raw CMD は request データに載せ、標準 API ID と衝突させない。
trusted composition が parent endpoint を定め、protected ledger の binding と整合することを検査する。
呼出し対象が parent object でも、CALL_METHOD の静的 T 経路と kernel actor を必ず通る。

child 終了の parent policy が必要な構成では、論理 frame にその continuation をあらかじめ記録し、EXIT_METHOD の固定 T 経路で処理する。
親が object code を実行する区間は「kernel service」ではなく通常の object method 区間で、その actor は親 ID。
kernel の Thread 処理へ戻った区間では kernel actor ID に戻る。
root は同じ論理 protocol の既定 policy をカーネル内部関数で処理し、root の登録有無で H/T を変えない。
専用親が追加 method/API を発行した分の trap は加算するため、任意 parent code を含む CALL 全体に 2 回上限は約束しない。

別案は、parent policy を kernel が検証できるデータとして表現し object callback をなくす方式である。これは表現可能な policy が狭まる。
どちらも「登録 callback があれば遅い T、なければ速い H」という方式とは異なる。
*未決*: V2 の raw syscall 再解釈を必要とする利用者を explicit method に移行する採否、EXIT 通知を同期 parent method とするか検証可能な policy データにするか。
固定決定 5 を緩めて legacy fallback を足す案は本書の採用案に含めない。

== MPU/PMP 領域の事前計算

object/stack/kernel の descriptor 作成・GRANT_REGION・hot swap の公開時に、arch ごとの RBAR/RLAR または PMP address/config、alignment、enable/disable、権限を検査・計算する。
descriptor に region version と arch/ABI version を持たせ、CALL 時は descriptor を pin する。
restore へ current context pointer と次の protection descriptor を渡し、context の再 lookup と region 2 の丸め計算を繰り返さない。
同一 domain の H syscall は MPU を触らない。domain が変わる T/CALL は、現在の version と比較して正しい更新を行う。

per-core cache は唯一の writer の場合だけ使える。board/driver/デバッガの別 writer、GRANT_REGION、thread 移動、hot swap、失敗 rollback で invalidation する。
旧 descriptor を pin した in-flight call は旧権限 snapshot を保持するのを初期案とする。即時 revoke が必要なら stop/drain 後に更新する別 protocol が必要。
書込み途中で一時的に余計な権限が開かない順序にし、必要な DSB/ISB・PMP 同期を arch backend が担う。
16 cyc の測定を理由に barrier を削除しない。常設 kernel／return gate の到達性を region 切替え前に検証する。

== ホットスワップと actor ID の変化

公開単位は immutable `KernelGeneration {generation, actor_id, entry_table, attributes_hash, abi_version, protection, state_schema}`。
API の traits はビルドに静的であり、hot swap 先も同じ API/属性契約を満たすことを publish 前に検査する。
mode を動的 entry table から読む設計に戻さない。属性変更は互換 hot swap ではなく、全 thread を quiesce する別の更新とする。
固定 object ID 40 個は維持するが、kernel role の runtime ID をその表の定数 1 等へ焼き込まない。
kernel ID の割当ては、公開 ID と衝突しない runtime slot または generation-tagged identity のどちらにするか、ABI 正本の ID 幅・容量と照合して確定する。

=== 公開と参照の取得

publish pointer を release store し、SVC 入口は acquire load した一つの generation を使用する。
ID、entry、region を別々の global field から取らない。entry を BL する前に old generation の寿命を確保する。
単純な「pointer load → refcount++」では、その間に reclaim されるため不十分である。
初期案は per-core hazard/entry slot と writer の grace period、または既知の最大時間で取得できる short publication lock。
IRQ masking だけでは別 core からの reclaim を防げない。ISR nesting の hazard slot も用意する。
H の pin acquisition は H_max に含め、無制限 seqlock retry を導入しない。

T 入口で取得した generation pin は service continuation／method frame の終了まで保持する。
kernel actor 内で再入する API と復帰操作は、その actor/continuation が pin した世代を継続使用する。
通常 object の新しい独立 syscall は公開中の世代を取得できるが、EXIT は終了対象 frame の kernel 世代を使う。
この選択規則は protected phase/ledger に基づき、ID と entry の対応を整える。標準 API の実行モードはどの世代でも同じ。

=== 旧 ID と進行中の thread

publish 後の旧 ledger の actor ID/generation、parent binding、return PC を新 ID に書き換えない。
古い kernel actor の本人性は旧 descriptor と pin で検査する。「現行 global kernel ID と違う」という理由だけで拒否しない。
生の ID 一致だけでも authority を与えない。role、generation、protected continuation を併用する。
sleep、grant、callee 実行中、他 core の suspended thread の continuation も世代を保持する。
終了/kill は二重解放を防ぎ、実行中 stack と参照を、どの core も使っていないことを確認してから回収する。

旧世代は new admission を停止し、全 core の hazard slot、ISR、service pin、call frame、deferred task、driver callback が消えてから reclaim する。
旧 text/stack/MPU descriptor はこの時点まで存続する。OTA が旧 XIP 領域を erase しないことも受入条件である。
世代カウンタ wrap と slot 再利用による ABA は許さず、使い切った identity は再利用しない。
SVC vector の最小入口、publication 管理、最終 tail/exception-return gate は初期 V3 では交換しない常駐 nucleus に置く。
generation pin/hazard の解除は、旧 text から常駐 gate へ移り、復元情報を常駐管理領域へ引き継いだ後に行う。
常駐 nucleus 自体のオンライン置換はこの protocol の対象外で、停止更新の別設計とする。

=== 共有状態、drain、失敗

当初は旧／新が共通の versioned object/thread/stream state を参照し、schema が一致する更新だけを並行公開する。
schema migration は新規受付停止→全 in-flight drain→移行→公開の stop/drain 方式とする。
一部の thread の PC や C stack を移し替える migration は行わない。
無期限 method があれば旧世代は drain できない。timeout 時は旧を保持して更新延期とする案を第一候補とし、強制終了の採否はユーザー判断に残す。
publish 前の失敗は旧に戻す。publish 後の rollback は新世代の状態変更と参照を含めた再公開 protocol で行い、pointer だけ旧へ戻せば全副作用が消えると考えない。

== stream と atomic、行列を作らない待ち

=== stream の現状と H 実装

`kernel_stream.typ:12–38,42–57` の kernel-owned stream は現行で create/bind/connect/destroy と push_n/pop_n 等を持つ。
`handler.cpp:1205–1285` は capacity で batch を制限し、copy 中追越しと lost を扱う。
これだけでは H_max を保証しない。capacity と record size は可変で、pin/unpin は CAS loop を持つためである。
V3 の H API は静的な最大 bytes/records と有限 pin protocol を持ち、件数が大きくても T へ切り替えない。
成功した件数だけを返し、残りは利用者が再発行する。最大 record 自体が bytes 上限を超える場合の API 契約は移行前に決める。
満杯 lossless は 0 件／短い結果を返し、kernel 内で consumer を待たない。
ring descriptor と wr/rd/席は kernel 所有。DMA はその形状だけを参照し、generation stale、DMA busy、owner/席拒否を維持する。
batch copy の release publish / acquire consume、再読検査、lost の更新順を保持する。

`kernel_stream.typ:28–36,72–88` の未決／将来項目は次のように整理する。

#table(columns: (1.2fr, 2.7fr), inset: 4pt,
  table.header(repeat: true, [項目], [V3 への配置]),
  [Atomic svc], [新しい公開 API ID の traits は H/S0、非特権可、ISR 可。番号と引数 layout は未確定。既存案の r0=op をそのまま使うと r0=API ID と衝突するため、op は a1、operand は a2…または request に置く。],
  [Wait/wake], [専用 API／kernel wait queue は実装しない。決定 9 が旧 futex queue 案を置き換える。YIELD=8、RUN_FOR=9、SLEEP_US=11 は常に T/ISR 不可。],
  [大 buffer の ownership 移転], [新 ID／既存 MEMORY_HAND_OVER の拡張契約を確定してから T として設計。今回実装済みとはしない。],
  [object 終了時の stream 回収], [単一 thread exit で object 所有 stream を壊さない。object 寿命と置換の drain policy を決めてから T の cleanup に接続。],
  [extent の consumer access], [data copy と XIP access grant は別。MPU/PMP で consumer の access を確定するまで安全と断定しない。],
)

=== Atomic の線形化と範囲

最初は 32bit CAS/SWAP/FETCH_ADD と load/store の ordering 契約を定義する。
mutex 取得／値待ちの read は acquire、unlock／signal の write は release を最低条件にする。
ABI として seq_cst を初期標準とするか、op ごとに明示 ordering を持たせるかは未決で、ISA が強いという理由で省略しない。
atomic target は caller に writable な共有 normal memory、自然整列、存続する領域に限定する。kernel ledger、別 object の protected memory、任意 MMIO を特権代理書込みしない。
ISR caller は driver authority と登録済み共有範囲を使う。

RP2040 の `armv6m.hpp:137–140` は Pico の atomic hardware spinlock を使う。
これをそのまま H の無制限 blocking lock として受け入れない。全 holder の IRQ/scheduling 契約、他 core の保持上限、lock 順序を証明する。
M33 の LDREX/STREX retry も無制限なら H 不可。短い専用 kernel 排他、有限試行と既存契約で表現できる失敗結果、または arch の直接 AMO を選ぶ。
失敗を新 error で返す場合は ABI 追加を承認してから行い、CAS の「比較不一致」と「競合上限超過」を混同しない。
CAS の mutex 自体が論理的に失敗して所有者へ譲ることと、atomic syscall の内部が blocking することは別である。

=== 宛先 YIELD による mutex／値待ち

mutex の owner は runtime thread ID *と generation* を持ち、CAS 成功で取得する。
失敗した Thread は owner を再読・検証して宛先指定 YIELD(owner) し、戻ったら CAS を再試行する。
owner が終了／再利用された場合、NOT_RUNNABLE、self owner、再帰 mutex、owner death を取り扱う。生の slot 番号への譲渡で別 thread を起こさない。
caller 側の generation 比較と raw thread ID の YIELD の間にも再利用の窓があるため、比較だけで安全としない。
kernel の宛先選択と generation 検証を同じ不可分の操作にするか、選択中の slot を pin する。公開 YIELD へ期待 generation をどう渡すか／既存引数と共存させるかは ABI 判断として残す。
RUN_FOR は既存の貸付け上限、借り手 YIELD は貸し手へ戻る意味を保つ (`handler.cpp:884–919`)。
借り手が owner と別の第三者へ YIELD できるとは仮定しない。貸付け中は貸し手へ返して外側の policy に再試行させる。

値待ちは load-acquire→条件不成立なら YIELD(既知 producer または 0)→再確認を繰り返す。
更新側は store-release または atomic で値／sequence を公開する。edge 通知だけを使うと見逃せるため、待つ側の predicate は持続する状態か monotonic sequence にする。
queue に登録して眠ることがないので futex の compare/register lost-wakeup は起きないが、スケジューラの公平性と producer が再び runnable になることが進行条件である。
yield が返っただけで値変更を保証しない。必要なら caller が期限を確認し、短い SLEEP_US と再確認を組む。
SLEEP_US による polling の最大待ち時間は sleep 間隔＋scheduler 遅延であり、即時 wake と同等ではない。
mutex owner を長く待つ場合に睡眠すれば省電力は改善し得るが、unlock が即時起床させる契約は今回作らない。
IRQ 内では失敗を返すだけで、YIELD/RUN_FOR/SLEEP を実行しない。ISR からの completion は値を公開し、次の合法な scheduling point で Thread が再確認する。

== ESP-IDF / blob 用 kernel_services

`esp-idf-support` が Wi-Fi OSI と BLE NPL の関数表を所有し、その関数から kernel service を直接呼ぶ。
shim を object 化せず、CALL_METHOD/SVC の dispatcher に blob の OS 依存を混ぜない。
NPL の event queue／buffer queue はデータ構造として shim に置けるが、kernel の wait/wake 専用待ち行列は作らない。
ISR に必要な有界 enqueue/atomic は直接行い、sleep/yield を含む処理は driver Thread の再確認に任せる。

初期 `kernel_services` ヘッダの契約は、宛先 yield、grant(cycles)、単調時刻、sleep(µs)、cas/atomic、in_isr/trap_depth、IRQ save/restore の小さい組に絞る。
shim が実際に必要とする task 起動、allocation、timer callback 等がこの組だけで表現できるかは adapter の依存監査で確認する。
不足時に object API や FreeRTOS 内部全体へ依存を広げず、追加 service を理由・ISR 条件付きで上層へ提示する。
V2/V3 backend は共通の error、時刻単位、atomic ordering、ISR rejection を満たす。
直呼びが trap を省略できるのは現在の特権と安全な scheduling point が保証される場合だけで、V2 backend は必要なら内部 trap を使ってよい。

=== 同じ NPL 関数の task/ISR 両用

上層提示の `npl_os_freertos.c:194–197` は in_isr 分岐を使うという確定前提とするが、対象 IDF revision は今回提示されていない。
公開 `espressif/esp-nimble` の参照版でも eventq/sem の同じ関数が ISR を判定して非待機の実装を選ぶ。ただし参照版の行番号は上層提示と一致しない。URL は資料節に示す。
V3 の NPL も同じ公開関数を持ち、まず実行文脈を判定する。
これは OS adapter の文脈による契約であり、同一 syscall API ID の H/T を runtime に変更する許可ではない。
Thread 版は値確認と yield/grant/sleep の直接 service、ISR 版は from_isr=true の有界 primitive のみを使う。
ISR timeout=0、blocking mutex 不可等の NPL 契約を保ち、ISR が「たぶん task」と扱われて譲る経路を作らない。

=== ISR 判定と trap 深さ

`in_isr()` は *現在*の割込み／trap 実行中であるかを答える。
call depth や kernel actor であることは ISR 判定に使わない。Thread kernel landing は kernel actor でも in_isr=false。
一方、dispatcher の権限検査は現在の Handler 状態ではなく、保存した origin が ISR であったかを使う。
Thread 由来 SVC の中では current trap_depth>0、origin.from_isr=false となる。

Arm は IPSR 等の現在の例外番号から判定し、全入口／出口を通る per-core nesting depth も diagnostic として公開する。
C6 は ISA に Handler mode がないため、trap vector の software depth を保存し、割込みを再許可する前に加算、帰還直前に復元する。
ecall、外部 IRQ、timer、nested trap、fault の全入口を数え、cause 分類と origin を別に保存する。
Thread landing は outer trap を完了してから実行するので depth=0。他 core の depth を読まない。
IRQ save は以前の mask/token を返し restore はその状態だけを戻す。単純 enable による nested critical section の破壊を防ぐ。
これら内部サービスは privileged kernel/driver 用で、非特権 object に IRQ 制御を公開する新 syscall は追加しない。

== RP2040 と ESP32-C6 への載せ方

#table(columns: (1fr, 1.5fr, 1.5fr, 1.7fr), inset: 4pt,
  table.header(repeat: true, [差分], [RP2350 / M33], [RP2040 / M0+], [C6 / RV32IMAC]),
  [入口保存], [HW core frame＋必要なら FP。S0/S1/S2。], [HW core frame、FP/PSPLIM 命令なし。r8–r11 の保存命令差は backend 内。], [GPR 自動 stacking なし。ecall 用 caller-saved と CSR を software 保存、切替え時に s0–s11 等を追加。],
  [復帰], [Handler EXC_RETURN。Thread tail gate は別途証明。], [同じモード区別。Thumb-1 で完全復元 gate を個別設計。], [mepc/mstatus と MRET。ECALL は 32bit なので再開 PC は +4。一般 fault の PC に一律 +4 しない。],
  [保護], [MPU/PSPLIM。], [現行は software stack limit、MPU guard 未実装。M33 同等の保護を主張しない。], [PMP 等を能力／IDF 所有権と合わせて実装。MPU の register 形状を持ち込まない。],
  [atomic], [exclusive 命令と排他上限。], [hardware spinlock と IRQ 規約の裏取り。], [A extension の AMO/CAS backend、ordering。],
  [計測], [既存 DWT CYCCNT。], [HAS_CYCLE_COUNTER=false。timer による多数反復または GPIO/外部計測。単発 cyc は未確定。], [cycle CSR の実装／アクセス権／clock 条件を確定し新 backend。数値未測定。],
)

根拠: `armv6m.hpp:36–49,132–140`、`armv8m.hpp:75–109`、`concepts/arch.hpp:82–86`、`1c4fe7b`。
C6 の現 worktree は ISA probe と移植資料の段階であり、V3 の動作証拠はない。
当面の C6 V2 移植は別 worktree で進み、shim 契約を共通にして backend だけ後で V3 へ置換する。
“Handler service” は C6 では trap 内完結を指す設計用語で、RISC-V に Arm の Handler mode が存在するとは書かない。
ABI はレジスタに API 番号と引数を渡す。SVC 即値に番号を埋めず、ECALL の即値なしに対応する。

== 性能目標と測定

次は *未測定・未承認の設計予算*。RP2350 150 MHz、hot path、同じ -Og 条件を最初の比較基準とする。
- H の GET svc net min: 1985 cyc → *500 cyc 以下*を初期目標とする。
- direct method 1 段 net min: 5774 cyc → *3000 cyc 以下*、2 段: 11642 cyc → *6000 cyc 以下*。
- traced 共通 save/dispatch/MPU-wrapper/restore: 約 446 → *200–250 cyc/境界*、整数 frame 準備: 約 432 → *100–150 cyc/frame*を部品目標とする。

total と部品目標は独立の予算であり、上の部品差を足して total を予言しない。
tail gate、policy、descriptor pin、fault fixup、保護変更回数、FP の費用が入る。
H の WCET 10 µs 予算は GET min 500 cyc 目標と別で、最大 stream batch の競合／copy／割込みを含めて確認する。
RP2040/C6 は別の測定基準を取得してから数値目標を置き、M33 の cyc を移植しない。

`call_cost.cpp:57–79,193–241,245–255` の測定枠と event 区間を流用する。
raw=hook 無効の外枠計測、net=空枠を引いた値、trace=hook 有効の全 event と定義する。
各採取で ELF hash、source revision/dirty diff hash、compiler/flags、board/clock、cache/RAM placement、FP、IRQ 負荷、core、stream count/record size を保存する。
同じ label でも異なる ELF の PC や計測値を混ぜない。

V3 は entry、minimal capture、permission check、kernel landing、logical frame prepare、domain apply、tail transfer、return validation、caller resume の marker を加え、元の番号を流用した場合も stage 定義を version で区別する。
S0 の context_saved/push が 0 hit になること、method 1/2 段の entry hit 2/4、logical frame push/pop 1/2、domain change 回数を検査する。
event raw dump と hits/coverage/overflow を保持し、mean の和と trace_total の整合を取る。
独立 min の和を全体 min と断定せず、hook の費用は別採取の差から厳密に逆算しない。
USB FIFO の行落ちを防ぎ、全 stage が記録されても出力欠落なら失敗と区別する (`cost2_report.md:7–11`)。
batch は 1/4/16 records と最大 bytes、満杯／空、最大 IRQ nesting、他 core 競合、FP 活性／非活性、hot swap 中で比較する。

== V2/V3 並存と移行の完了条件

提案は V2 の現行ブランチを保存し、V3 実装を専用 branch と独立した build config `kernel_v3` に置く方式。
既存 dirty を勝手に commit/stash/checkout しない。今回 branch/config は作らない。
一つの firmware image が途中で V2/V3 ABI の解釈を切り替える構成は初期対象外とする。
公開 `//abi` と `kernel_services` は共通、V2/V3 backend は別に選択する。

#table(columns: (auto, 1.7fr, 2.6fr), inset: 4pt,
  table.header(repeat: true, [段], [変更単位], [完了条件]),
  [0], [正本と比較基準], [40 named object ID/count・method ID 正本、31 API と atomic の追加番号を確認。ELF/flags/log 対応を固定。O2 の既知停止を記録。],
  [1], [traits、権限、ISR 判定、shim 境界], [全 API に traits。未知 ID 拒否。origin privilege と driver identity、task/ISR と nested depth、V2/V3 services の契約試験。未実装 shim を実動と報告しない。],
  [2], [H GET と最小入口], [r4–r11/flags/volatile regs 保持、同 PSP/CONTROL、IRQ nesting、MSP alignment、保護不変。GET entry=1/push=0。],
  [3], [Thread landing と CALL/EXIT], [A または B の arch ごとの採否を固定。identity/親/return/NO_STACK/fault を検証。A なら 1/2 段 2/4 trap、全 copy byte 数を記録。],
  [4], [stream H と atomic／yield 待ち], [bytes/retry/WCET 上限、lost/order/席、CAS linearization、owner generation、値再確認、deadline、ISR 拒否。専用 wait queue を導入しない。],
  [5], [generation hot swap], [全 core/IRQ/in-flight/old ledger/rollback/OTA text lifetime の証跡。旧 actor ID 継続と reclaim 条件を検査。],
  [6], [最適化と board 受入], [-Og/-O2/-Os の同じ correctness matrix、RP2350/RP2040 と C6 backend の個別証跡。速度と保護を別に受入。],
)

=== 上層が読むべき現行 selftest と不足分

これは既存テストの検査内容を示す一覧で、今回の実行結果ではない。
- `source/selftest/call_ladder.cpp:260–264`: 6 段の戻り値 21 と depth=0。
- 同 `275–288`: 各層の self/caller identity と、V2 の内部深さが 2×(level+1) であること。
  V3 の logical depth と物理 continuation 数を分離し、後者の V2 数値をそのまま受入 assert にしない。
- 同 `295–304`: UNKNOWN_API と BAD_OBJECT。`311–317` は deep が帰還し depth=0 を確認し、`172–178` の deep 本体が NO_STACK を検出して上へ返す。ラベルだけで NO_STACK の assert 内容を判断しない。
- `source/selftest/handler_binding.cpp:201–220,230–239`: 親の自動導出、親 A/B の独立解釈、replace 後 binding、child の primitive 偽装が parent 要求扱いであること。
  後二つの raw SVC 解釈は V3 の explicit method へ移行して、同じ上位の意味を確認する新テストが必要。
- `source/selftest/stream_ladder.cpp:268–297`: 全件受信、lost=0、順序違反=0、full_hits>0、席拒否、他 owner destroy 拒否、stale ID 拒否。

V3 に新しく必要な試験は、各 traits の privileged/unprivileged×task/ISR matrix、IRQ 中の T 拒否と無副作用、driver authority、r0/r1 以外の完全保存、MSP 8B、FP lazy、4B stack padding、最大 copy、CAS 競合、Thread lock を preempt する IRQ、hot swap 全 phase の強制 interleave と old PC lifetime である。
テストは期待値を持つ本番経路を通し、exit 0 や build 成功だけで実機動作を証明しない。
ホスト試験、cross build、disassembly、実機 selftest、実機 cycle を別の証跡として報告する。

=== -O2 / -Os の扱い

V2 O2 は stack exhaustion の stage=808 で停止しており、原因は未確定 (`cost3:82–97`)。
valid magic のない fs_fault PC/LR を破壊証拠にしない。V3 で原因が自然に直ると仮定しない。
入口整列、stack 幾何、inline asm 保存契約、tail transfer を各最適化で点検する。
frame pointer を強制する診断で不具合が隠れないよう、nofp と exact ELF の fault 記録を組にする。
Os の lossless 試験は consumer 開始を full 拒否まで遅らせた現行修正を基準とし、full_hits>0 を実際に確認する (`cost2:49–54`)。
既定最適化の変更は全 correctness matrix と cycle/stack/size 比較の後に判断する。今回は -Og を変更しない。

== ユーザーに決めてほしい点

固定決定 1–11 は再確認しない。以下は未決である。
#block(breakable: false)[
1. Handler の IRQ 遅延予算 (初期案 10 µs / mask 2 µs)、最大 bytes/records、上限より大きい record の互換性。
2. parent policy を explicit method に移す採否と、終了通知を同期 method にするかデータ policy にするか。raw 標準 syscall の再解釈は廃止する。
3. Arm Thread tail transfer A を必須にするか、先に completion trap B で安全性を受け入れてから最適化するか。B の追加回数は許容値として別記する。
4. syscall の既存 register/flags 保存契約を維持するか、wrapper clobber を変更して利用者を rebuild するか。
5. hot swap actor ID の runtime slot/世代表現、drain timeout、無期限 call を強制終了するか更新延期するか。初期案は延期・旧世代保持。
6. atomic の公開番号、CAS 等の引数／戻り、ordering、競合上限を超えた場合の error。ID 番号と object ID 表を混同しない。
7. `//abi` 正本の revision と stream ABI 同期、SET_BUDGET/AFFINITY の権限、CREATE_OBJECT の非特権→特権生成を拒否する互換性条件。
8. 宛先 YIELD の thread 再利用対策。期待 generation の ABI 表現または lifetime pin による選択の契約。
]

今回の文書ではこれらを選択済み／実装済みとはしない。wait queue 不採用、shim 直接呼び、静的権限／ISR 属性は決定済みである。

== 一次資料と検証の境界

architecture の制約を確認するために以下の一次資料を参照した。
- #link("https://documentation-service.arm.com/static/66be2153882fec713ef49e49")[Arm Cortex-M33 Generic User Guide, 100235_0100_06] の §2.3.7 (exception entry/return)、§4.6 (FP context)、§4.5.19–20 (MPU 更新／同期)。
- #link("https://github.com/ARM-software/abi-aa/blob/main/aapcs32/aapcs32.rst")[Arm AAPCS32] の §6.1.1 / §6.2.1 (callee-saved と public interface の SP 8B 整列)。
- #link("https://docs.riscv.org/reference/isa/v20260120/priv/machine.html")[RISC-V Privileged ISA v1.13, Machine-Level ISA] の trap CSR、§2.1.3.1–2 (ECALL の epc と MRET)。
- #link("https://documentation.espressif.com/esp32-c6_datasheet_en.html")[Espressif ESP32-C6 Datasheet]。C6 の正確な PMP/interrupt 統合は採用 IDF/TRM revision と別 worktree 実装で再確認する。
- #link("https://raw.githubusercontent.com/espressif/esp-nimble/master/porting/npl/freertos/src/npl_os_freertos.c")[Espressif esp-nimble NPL FreeRTOS 参照実装]。task/ISR 分岐の存在を確認した参照版であり、上層が使う IDF の exact revision とは未照合。

本書の作成では製品 build、selftest 実行、commit/push、実機／USB 操作は行わない。
Typst の構文・組版を compile で確認するだけで、V3 の実行経路や cycle を検証済みとは扱わない。
