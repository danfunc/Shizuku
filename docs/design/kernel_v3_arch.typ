#set document(title: "Shizuku V3 カーネル アーキテクチャ補完設計書", author: "Shizuku Architecture Team")
#set page(paper: "a4", margin: (x: 15mm, y: 15mm), numbering: "1")
#set text(font: ("Hiragino Sans", "Arial"), size: 8pt, lang: "ja")
#set par(leading: 0.55em, justify: true)
#set heading(numbering: "1.1")
#show raw: set text(font: "Menlo", size: 6.8pt)
#let ref(s) = text(size: 7.2pt, fill: rgb("555555"), s)
#let note(s) = block(fill: rgb("f8f8f8"), inset: 5pt, radius: 2pt, stroke: 0.5pt + rgb("dddddd"), width: 100%, text(size: 7.5pt, s))

= Shizuku V3 カーネル アーキテクチャ補完設計書 (ISA ファミリー展開と非依存核モデル)

状態: *最終確定設計書 (kernel_v3.typ 補完・独立文書、受入監査指摘反映版)*。2026-10-07。
対象リポジトリ: `Shizuku-stream` (Git HEAD: `3239d3761fd1898b78411434a31f91790251bd3b`)。
参照元設計書: `docs/design/kernel_v3.typ` (スナップショット詳細は第1章参照)。

== 範囲・スナップショットと用語定義

=== 本文書の目的と位置付け

本書は、`docs/design/kernel_v3.typ` (以下、元文書) が Cortex-M (特に ARMv8-M Mainline / RP2350 および ARMv6-M / RP2040) の実装文脈を前提に策定した V3 カーネル核設計を、複数のプロセッサ・アーキテクチャ (ISA) ファミリーを視野に入れて補完・汎用化する独立したアーキテクチャ設計書である。

元文書における決定 1–11 を不変条件として遵守し、決定内容そのものの変更や再判断は行わない。チップ固有の呼称や Cortex-M 専用の機構用語を排し、ハードウェア機能の有無を「機能変数」として直交化・抽象化することで、各 ISA ファミリーにおける成立性、費用差、代替・劣化モード (Degradation Modes) を客観的に評価・定義する。

=== 参照元スナップショットの確定事実

元文書 `docs/design/kernel_v3.typ` の参照スナップショット情報は以下の通りである。

- ファイル絶対パス: `/Users/ishigakiyua/github/Shizuku-stream/docs/design/kernel_v3.typ`
- SHA-256 チェックサム: `625956303e0b7d7f9049a3bcc42f5fb565a1104f5e6afdf5026e6e3af97f97d0`
- 最終更新日時 (mtime): `2026-10-07 14:24:03 +0900`
- 行数: `694` 行 (`wc -l` 測定値。末尾空行を含む論理行数は 695 行)
- 並行変更の有無: 作業着手時、初回執筆時、第2次改訂時、および受入監査対応時において内容の一致を厳密に確認済み (並行変更なし)。
- リポジトリ状態: `Shizuku-stream`、コミット `3239d3761fd1898b78411434a31f91790251bd3b` (`feat/v3-groundwork`)。

=== 用語定義と Cortex-M 依存用語の対照

元文書において Cortex-M のハードウェア機能に強く依存していた用語を、全 ISA で共通に通用する中立なアーキテクチャ概念へ再定義する。

#table(columns: (1.2fr, 1.4fr, 2.4fr), inset: 3.5pt,
  table.header(repeat: true, [Cortex-M 用語], [ISA 非依存用語], [概念の定義と境界]),
  [Handler mode], [同期トラップ文脈 / 例外文脈 (Inline Synchronous Trap Context)], [割込み (IRQ) または同期例外ハンドラ内で直接動作する実行状態。元の実行コンテキストを直接中断している。],
  [Thread mode], [通常実行文脈 / スレッド文脈 (Ordinary / Deferred Task Context)], [スケジューラによって管理される通常タスク・スレッドの実行状態。非特権または特権で動作する。],
  [SVC (Supervisor Call)], [同期システムコールトラップ (Synchronous Syscall Trap)], [ユーザー空間からカーネル特権を要求する明示的な同期例外命令 (RISC-V: `ecall`, x64: `syscall`, Xtensa: `syscall`)。],
  [PendSV], [非同期遅延切替割込み (Deferred Context Switch)], [割込みネストを抜け出した最外周でコンテキスト切替えを安全に行うための最低優先度ソフトウェア割込み。],
  [MSP (Main Stack)], [特権スタック / 例外スタック (Privileged / Trap Stack)], [カーネル核および例外・割込み処理が使用する、ユーザー空間から不可侵な隔離スタック。],
  [PSP (Process Stack)], [スレッドスタック / ユーザースタック (Thread / User Stack)], [特定のスレッドまたはオブジェクト実行インスタンスに割り当てられた実行スタック。],
  [EXC_RETURN], [ハードウェア例外復帰トークン (Exception Return Token)], [例外ハンドラから元のモード・スタック・セキュリティドメインへ正しく復帰するための予約値またはハードウェア状態。],
  [CONTROL (nPRIV/SPSEL)], [実行特権・スタック選択状態 (Privilege & Stack State)], [現在の実行モード (特権/非特権) および使用スタックポインタを規定するステータスレジスタ (RISC-V: `mstatus`, x64: `CS/RFLAGS`)。],
  [PSPLIM / MSPLIM], [ハードウェアスタック境界検査 (Hardware Stack Limit Register)], [スタックポインタのアンダーフロー/オーバーフローをハードウェアが自律検出する専用レジスタ。],
  [MPU / PMP], [メモリ保護機構 (Memory Protection Unit)], [ページテーブル (MMU) を介さずに物理アドレス空間の領域ごとのアクセス権 (R/W/X/特権) を制御するハードウェア機構。],
  [PRIMASK], [大域割込みマスク (Global Interrupt Mask)], [プロセッサの可変優先度割込み受付を一括して禁止・許可するフラグ。NMI および HardFault はマスクされない (RISC-V: `mstatus.MIE`, x64: `RFLAGS.IF`, Xtensa: `PS.INTLEVEL`)。],
)

== ISA 非依存のカーネル核モデル

=== 静的属性契約 `{permission, from_isr, mode, save_level}`

すべてのカーネルサービスおよびシステムコール (API) は、実行時ではなくビルド時に確定する以下の静的属性タプルによって厳密に定義される。

$ "API Traits" = chevron.l "permission", "from_isr", "mode", "save_level", "permission_predicate", "work_bound" chevron.r $

+ *`permission` (呼出し権限)*:
  - `unprivileged_allowed` (非特権可): ユーザーランド / 非特権オブジェクトからの発行が許可される。
  - `privileged_only` (特権のみ): 特権コンテキスト (特権スレッドまたは信頼済みドライバ) からのみ発行可能。
  - さらに、資源所有権、親子関係、委譲権限を検証する `permission_predicate` を備える。トラップ文脈に入ったという事実だけで非特権呼出し元を特権昇格させてはならない。
+ *`from_isr` (割込み内呼出し可否)*:
  - `false` (禁止): 割込みハンドラ (ISR) からの呼出しを即座に拒否し、副作用なくエラーを返す。通常実行文脈への暗黙のフォールバックは禁止する。
  - `true` (許可): 割込みハンドラからの直接呼出しを許可する。ただし、有界時間 (Bounded Execution Time) かつ非ブロック (Non-blocking) であることが形式的に検証されている API に限定される。
+ *`mode` (実行コンテキストモデル)*:
  - `inline_synchronous_trap` (同期トラップ内即時完結): システムコールを発行した例外ハンドラ (同期トラップ) の文脈内で処理を完結させ、同一例外から呼出し元スレッドへ即座に復帰する (元文書の `handler` mode に対応)。
  - `deferred_kernel_task` (通常実行カーネルタスク): 例外復帰先を特権通常実行文脈の「カーネル landing (カーネル actor)」に設定し、カーネル専用スタックとカーネル actor ID を用いて通常スレッド文脈で実行する (元文書の `thread` mode に対応)。ターゲット port はこれらを従来の H / T ラベルへマッピングしてよい。
+ *`save_level` (文脈保存レベル)*:
  - `S0` (最小保存): 同一スレッド・同一保護領域への復帰を前提とし、呼出し規約スクラッチレジスタのみを退避・復元する。
  - `S1` (整数継続保存): 異なるスレッドやカーネル actor へ実行を移すため、全 callee-saved 整数レジスタを含む継続 (continuation) を保存する。
  - `S2` (拡張状態保存): S1 に加え、活性化している浮動小数点 (FPU) やベクトルレジスタ等の拡張状態を明示的に退避する。

=== 実行権移譲とスケジューリング契約: GRANT と YIELD

カーネル V3 における実行権管理は、専用の wait/wake 待ち行列 (futex 型キュー等) を一切排除し、厳密な *GRANT 貸付け* と *無引数 YIELD 返却* の二項関係によって成立する (元文書 決定 8, 9)。

+ *GRANT 貸付け (Execution Right Lending)*:
  - システムにおける実行権の根本的な貸し手 (Root Lender) はスケジューラである。
  - 実行権の自発的な移動は `GRANT(target, cycles)` (API ID: 9 `RUN_FOR` 相当) のみによって行われる。
  - 貸付けの流れは入れ子構造 (A $arrow$ B $arrow$ C) を許容する。内側の貸付け予算は、外側の残余貸付け予算によって厳密にクランプ (飽和制限) され、親の割り当て時間を超過して実行されることはない。
  - 貸付け深さには静的上限 `GRANT_DEPTH` が設けられ、スタック枯渇を防ぐ。
+ *YIELD 返却 (Unilateral Yield to Parent)*:
  - `YIELD` (API ID: 8) は*引数を持たない* (元文書 L680: 宛先指定 YIELD は完全廃止)。
  - YIELD を呼んだスレッドが借り手 (Borrower) である場合、残余の貸付け実行権を直前の貸し手 (親) へ無条件に返却する。返却手数料 (ペナルティ) は存在しない。
  - 借り手でない根スレッド (Root Thread) が YIELD を呼んだ場合、実行権はスケジューラへ返却され、次の適格な根スレッドがスケジュールされる。
+ *祖先スレッドへの GRANT の返却扱い (Ancestor Grant as Return)*:
  - スレッド祖先に対する貸付け (例: A $arrow$ B $arrow$ A) が試みられた場合、新たな貸付けフレームをスタックに積むことは禁止される。
  - この操作は「祖先 A までの返却」として解釈され、中間フレームを解消して `grant_end=YIELDED` 相当として処理される。
+ *同期プリミティブの構築契約*:
  - *Mutex*: Mutex は所有者スレッド ID を保持する共有メモリワードで表現される。取得側は CAS (Compare-And-Swap) を試み、失敗した場合は判明している所有者に対して `GRANT(owner, 予算)` を発行して所有者の処理を進めさせる。GRANT から復帰した後、再び CAS を再試行する。
  - *値待ち (Condition Waiting)*: 待機スレッドは `load-acquire` で条件を検査し、不成立なら producer が判明していれば `GRANT(producer, 予算)`、不明なら無引数 `YIELD` を行い、再開後に条件を再確認する。
  - *時間待ち (Timed Waiting)*: `SLEEP_US` (API ID: 11) による時限スリープと再確認ループを組み合わせる。

=== context reuse と専用戻り経路

元文書 決定 3 に基づき、*context reuse (同一スレッド文脈の再利用) によって入った呼出しは、必ず context reuse 専用の復元経路で戻す* ことを絶対契約とする。

- V2 における多段呼出し (CALL/RETURN) では、同一 THREAD の同一 context 領域上に作業フレームを積み上げ (`call_frame_push`)、復帰時に元のフレームポインタを復元する (`call_frame_pop`) という context reuse が行われていた。
- V3 においても、この呼び出し幾何と保護記述子ピン留めの対応を維持する。
- *禁止事項*: 普通の C 言語の `return` 文を例外復帰 (Hardware Exception Return) と同一視してはならない。通常実行文脈で動作するカーネル actor からユーザーランドへ復帰する際は、必ず専用の復元ゲート (Arch Gate) を通過し、元例外フレーム、スタックポインタ、保護ドメイン、実行特権をアトミックに同期・復元しなければならない。

=== 保存契約 (Saved Registers / Continuation)

元文書 決定 4 に基づき、レジスタおよびステータスフラグの保存契約は安易に緩和せず、各 ISA/ABI ごとに厳格に維持する。

- *S0 (同期短経路完結)*:
  - ハードウェアが例外発生時に自動スタッキングを行うアーキテクチャ (ARMv7-M/v8-M 等) では、同例外内から同一スレッドへ復帰する場合、追加のコンテキスト退避を行わず、ハードウェアが積んだフレーム内の戻り値レジスタのみを更新して復帰する。
  - 自動スタッキングのないアーキテクチャ (RISC-V, Xtensa, x64) では、同期システムコール (`ecall`/`syscall`) において呼出し規約で規定された caller-saved スクラッチレジスタのみを例外スタックへ最小限退避し、復帰時に復元する。全レジスタを一律退避する非同期割込みフレームとは明確に区別する。
- *S1 (通常実行カーネル actor)*:
  - 呼出し元スレッドの全 callee-saved 整数レジスタを、呼出し元の継続構造体 (Continuation) へ確実に退避する。
  - C 言語コンパイラが生成する関数プロローグによって特権スタック上に退避されたレジスタ値を、スレッド継続の正本と誤認してはならない。
- *S2 (拡張レジスタ)*:
  - 浮動小数点 (FPU) やベクトル演算が活性化しているスレッドのみ、拡張レジスタ退避領域を割り当てて保存する。非活性スレッドに対して無駄な固定領域コピーを行ってはならない。

=== Atomic 操作の線形化と役割分担

元文書 決定 6 に基づき、アトミック操作の役割分担を明確に分離する。

- *システムコール表現*: 単一の API ID と `op` 引数によって表現する。引数の物理配置は各 ISA の ABI 規約に従う (Arm: `r0`=ID, `r1`=op; RISC-V: `a0`=ID, `a1`=op; x64: `rdi`=ID, `rsi`=op; Xtensa Call0: `a2`=ID, `a3`=op)。全 ISA 共通で特定レジスタに固定されると仮定してはならない。
- *同期短経路の責務*:
  - カーネルが同期トラップ内で完結させるのは、*32ビット CAS (Compare-And-Swap)* および *単一ワード load-acquire / store-release* のみに限定する。
  - `FETCH_ADD`、`SWAP`、キューのエンキュー/デキュー等の複合操作はカーネルに載せず、*objectland (スレッドモード)* で処理する。
- *禁止事項*: ユーザーランド内にライブラリ形式の CAS 再試行ループ (FETCH_ADD のエミュレーション等) を無制限に構築してはならない。
- *直接ハードウェア命令の許可と制約*:
  - ネイティブ CAS 命令 (x64 の `LOCK CMPXCHG`、RISC-V `Zacas` の `amocas.w`) が利用可能な環境では、ユーザーコードが直接ハードウェア命令を実行してよい。
  - Load-Linked / Store-Conditional (ARMv7-M/v8-M の `LDREX`/`STREX`、RISC-V `Zalrsc` の `lr.w`/`sc.w`) は排他モニタ付き命令列であり、CAS ループを構成する設計選択肢となるが、競合時の再試行が非有界 (unbounded retry) になりうるため、厳格な時間予算が課される ISR や確定的短経路での利用には上限管理または代替手段の検討が必要である。
  - カーネルの atomic システムコールは、主にハードウェア排他命令を欠くアーキテクチャ (ARMv6-M / RP2040 等) や、ISR との保護領域競合を安全に解決するための口として機能する。

=== ISR 判定とトラップ深さの契約

システム内の文脈識別において、以下の不変条件を確立する。

- *`in_isr()` の定義*: 「現在プロセッサがハードウェア割込みまたは非同期例外ハンドラを実行中であるか」のみを判定する。
- *除外条件*: 呼び出し深さ (Call Depth) や、カーネル actor ID を持っているという事実は、ISR 判定に一切流用してはならない。通常実行文脈で動作するカーネル actor landing は、特権カーネルコードを実行中であっても `in_isr() == false` である。
- *origin 属性との分離*: システムコール・ディスパッチャの権限判定には、現在の実行状態ではなく、トラップ入口で捕捉・保存した「呼出し元 origin が ISR であったか (`origin.from_isr()`)」を用いる。通常実行文脈から発行されたシステムコールの中では、`current trap_depth > 0` であっても `origin.from_isr() == false` となる。

=== `kernel_services` 内部契約と実装差分

OS アダプタ (ESP-IDF Wi-Fi / BLE NPL shim 等) およびカーネル内部処理が直接呼ぶ特権サービス層 `kernel_services` は、以下の最小限の関数群に限定する (元文書 決定 10, 11)。

```cpp
namespace shizuku::kernel_services {
  enum class status { ok, not_privileged, bad_context, operation_failed };
  struct result { status code; uintptr_t value; uintptr_t detail; };
  struct irq_state { uint32_t mask; uint32_t depth; };

  result   yield();                             // 宛先引数なし。親 (根ならスケジューラ) へ返却
  result   grant(uint32_t target, uint32_t cycles);
  result   sleep_us(uintptr_t microseconds);
  uint64_t time_us();
  bool     cas(volatile uint32_t *address, uint32_t expected, uint32_t desired);
  bool     in_isr();
  irq_state irq_save();
  void     irq_restore(irq_state state);
}
```

#note[
*既存実装コードとの差分に関する注記 (実装との不一致の記録)*:
元文書の確定設計 (決定 8, L680) では「宛先指定 YIELD は廃止。YIELD は引数を持たず、常に親へ返却」と定められており、上記の通り `result yield();` (無引数) が正本である。
しかしながら、現行の未コミット作業コード `internal_headers/shizuku/kernel_services.hpp:9` では `result yield(uint32_t target = 0);`、`source/kernel/services.cpp:34` では `result yield(uint32_t target)` と暫定引数が残存し、`source/selftest/kernel_services.cpp:30` でも `yield(0)` が呼ばれている。
本書は確定設計を優先して `result yield();` を正規契約として定義し、既存実装ファイルの引数残存は将来クリーンアップすべき実装乖離として記録する。
]

== 機能変数と劣化モード (Feature Variables and Fallback Modes)

プロセッサの具体的製品名ではなく、アーキテクチャが備えるハードウェア機構を有無の「機能変数 (Boolean Flags)」として直交化して定義し、欠落時におけるカーネルの代替動作および劣化モードを規定する。

特に、以下の設計直交化を厳格に確立する:
+ *特権分離* (`HAS_PRIVILEGE_SEPARATION`) と *メモリ保護* (`HAS_MEMORY_PROTECTION`) を完全に分離。
+ *ネイティブ CAS 能力* (`HAS_NATIVE_ATOMIC_CAS`)、*ネイティブ不可分 RMW 演算* (`HAS_NATIVE_ATOMIC_RMW`)、および *アドレス監視型排他モニタ* (`HAS_EXCLUSIVE_MONITOR`) を明確に区別・直交化。
  - `Zaamo` AMO は swap, add 等の不可分演算であり、CAS ではない。
  - `Zalrsc` や `LDREX`/`STREX` は排他モニタによる LL/SC 命令列であり、単体 CAS 命令ではない。
  - 代替選択肢として、これらを総称する上位フラグ `HAS_NATIVE_ATOMIC` を設ける設計も許容するが、内部成立要件は直交して評価する。
+ *ハードウェア自動スタッキング* (`HAS_GPR_HARDWARE_STACKING`):
  - ARM Cortex-M のように例外突入時に基本 GPR (r0–r3, r12, LR, PC, xPSR) を自動スタックする機構と、x86-64 のように最小復帰フレーム (RIP, CS, RFLAGS 等) のみをハードウェアが積み GPR は退避しない機構、および RISC-V/Xtensa のようにハードウェアスタッキングを一切行わない機構を厳密に区別する。

#table(columns: (1.5fr, 1.2fr, 2.5fr), inset: 3.5pt,
  table.header(repeat: true, [機能変数], [代表的充足 / 欠落例], [欠落時の代替動作・劣化モード (Degradation Mode)]),
  [`HAS_PRIVILEGE_SEPARATION`], [充足: ARMv6-M (CONTROL.nPRIV), ARMv7-M, ARMv8-M, RV32 (M/U-mode 実装コア), x64 (Ring 0/3)\ 欠落: 特権モード未実装の単一モード実行コア], [特権モードが未実装または無効化されている場合、全コードが同一特権で動作する。カーネル保護境界はソフトウェアの型安全性および呼出し規約のみに依存し、ハードウェア耐タンパー性は消失する (信頼済みフラット実行モード)。],
  [`HAS_MEMORY_PROTECTION`], [充足: ARMv7-M (MPU opt), ARMv8-M (MPU opt), RV32 (PMP opt), x64 (Page Tables)\ 欠落: ARMv6-M (RP2040: MPUなし), PMP未実装RV32コア], [ハードウェアメモリ保護機構 (MPU/PMP/MMU) が欠落している場合、物理メモリ空間は全スレッドから共有・アクセス可能となる。領域分離は静的リンカ配置と共有バッファの規約に委ねられ、スタック溢れや不正ポインタ破壊のハードウェア遮断は不能となる。],
  [`HAS_GPR_HARDWARE_STACKING`], [充足: ARMv6-M/v7-M/v8-M\ 部分充足 (Return Frameのみ): x64\ 欠落: RISC-V, Xtensa], [
    + *ARM*: ハードウェアが例外突入時に基本 GPR を自動スタックする。
    + *x86-64*: IDT 割込み/例外では最小 Return Frame (RIP, CS, RFLAGS 等) のみがハードウェア退避され、GPR/FP 退避はソフトウェア責任となる。`SYSCALL` 命令ではスタック変更を行わず GPR スタック退避は完全欠落。
    + *RISC-V / Xtensa*: ハードウェアによるスタック退避は一切行われず、例外ベクタ入口のアセンブリコードで scratch / caller-saved GPR を例外スタックへソフトウェア退避する (S0 で数命令のオーバーヘッド)。
  ],
  [`HAS_DEFERRED_TRAP`], [充足: ARM (PendSV), RV (MSIP / ソフトウェア割込み)\ 欠落: 一部単純マイコン], [最低優先度の遅延切替割込みが存在しない場合、同期トラップ内で即座にコンテキスト切替えを完結させるか、外部タイマ割込みの保留フラグを手動セットして代用する。],
  [`HAS_HARDWARE_STACK_LIMIT`], [充足: ARMv8-M Mainline (PSPLIM/MSPLIM標準装備; BaselineはTZ opt)\ 欠落: ARMv6-M, ARMv7-M, RISC-V, Xtensa, x64], [ハードウェア境界検査レジスタが存在しない場合、各関数プロローグにおけるソフトウェア検査、またはスタック末尾に MPU/PMP ガード領域 (No-Access 領域) を配置してフォールトで捕捉する。],
  [`HAS_NATIVE_ATOMIC_CAS`], [充足: x64 (LOCK cmpxchg), RV32 (Zacas opt: amocas.w)\ 欠落: ARMv6-M, ARMv7-M, ARMv8-M, RV32 (Zacasなし)], [単一命令によるネイティブ CAS がない場合、`HAS_EXCLUSIVE_MONITOR` (LDREX/STREX, lr/sc) による CAS 再試行ループで構成する。ただし競合時の retry が非有界になりうるため、厳格な時間予算 (work-bound) や ISR 適合性を満たすには上限管理または代替設計が必要となる。],
  [`HAS_NATIVE_ATOMIC_RMW`], [充足: x64 (LOCK xadd等), RV32 (Zaamo opt: amoadd等)\ 欠落: ARMv6-M, ARMv7-M, ARMv8-M, RV32I], [不可分演算命令 (AMO) がない場合、排他モニタによる LL/SC ループで代替する。なお `Zaamo` AMO は CAS 機能を含まないことに留意する。],
  [`HAS_EXCLUSIVE_MONITOR`], [充足: ARMv7-M/v8-M (LDREX/STREX), RV32A (Zalrsc opt: lr/sc)\ 欠落: ARMv6-M, x64, RV32I, Xtensa (S32C1I未実装時)], [排他モニタ (LL/SC) が存在しない場合、ネイティブ CAS 命令 (`HAS_NATIVE_ATOMIC_CAS`) を用いるか、以下のフォールバック設計へ移行する:
  + *単一ハート所有 (Delegation / Actor モデル)*: 共有変更可能状態 (shared mutable state) を単一のコア/ハートに排他所有させ、他コアからはメッセージパッシングで更新依頼する。
  + *操作のアーキテクチャ制限*: 複数コアからの直接アトミック共有アクセスをアーキテクチャ境界として不許可とする。
  + *SoC プラットフォーム排他機構*: RP2040 SIO スピンロック等の外付けハードウェアレジスタを利用する (参加者全員がプロトコルに従う場合のみ有効。DMA/NMI/非参加エージェントには無効)。
  + *単一コア割込みマスク*: 単一コア内での割込みマスク (`PRIMASK` / `MIE=0`) は同一コア内の maskable IRQ preemption のみを遮蔽し、外部バスマスタや NMI に対する atomicity は成立しない。],
  [`HAS_CYCLE_COUNTER`], [充足: ARMv7-M/v8-M (DWT CYCCNT opt), RV32 (rdcycle/mcycle opt), x64 (RDTSC/RDTSCP)\ 欠落: ARMv6-M, カウンタ未実装コア], [CPU サイクルカウンタがない場合、マイクロ秒精度の高分解能ハードウェアタイマ (SysTick, Systimer) による反復測定、または GPIO トグルとオシロスコープ等による外部測定で代替する。RDTSC はタイムスタンプカウンタであり、不変性 (Invariant TSC) やシリアライズ条件 (`LFENCE; RDTSC` 等) が満たされない限り単純なサイクル数とは扱えない。],
  [`HAS_EXTENDED_REGISTERS`], [充足: FPU/SIMD 実装構成\ 欠落: 整数専用構成 (ARMv6-M, RV32IM)], [浮動小数点ユニット等が存在しない場合、S2 レジスタ保存は S1 と同一になり、コンテキスト退避・復元コストは最小の整数セットのみで固定される。],
  [`HAS_MULTICORE`], [充足: RP2040, RP2350, ESP32\ 欠落: 単一コア SoC], [単一コア環境であっても、外部 DMA バスマスタや NMI との並行性 (concurrency) は別個に存在し、単なるローカル割込み禁止だけで完全排他が保証されるわけではない。],
)

== ABI / ISA / SoC 境界の分離

移植性および安全性を確保するため、設計要素を「ISA」「ABI」「SoC/ボード」の 3 階層に厳格に分離する。単一の共通 C++ ヘッダの存在をもって「完全な可搬性」を主張することは誤りであり、*各ターゲットの port 層および ABI バックエンドが規約を満たすことを前提とした「スコープ限定されたインターフェース契約 (scope-limited interface contract)」* として位置付ける。

各アーキテクチャ port の実装責務は以下の通りである:
+ ABI 規定の引数・戻り値レジスタの厳密な配置
+ スタック境界整列 (8B または 16B) の維持
+ 呼出し元 callee-saved 整数レジスタの完全保存と復元
+ 例外復帰命令および特権遷移の正規実行

```
+------------------------------------------------------------------------+
| 1. ISA 階層 (CPU コア・命令セット仕様)                                  |
|    - レジスタセット (GPR, PC, CSR/System Regs, Status)                 |
|    - 命令実行モデル (Trap 命令: SVC/ECALL/SYSCALL, 復帰命令: MRET/SYSRET)|
|    - 特権レベル定義 (User, Machine, Supervisor, Handler, Thread)       |
+------------------------------------------------------------------------+
                                    |
+------------------------------------------------------------------------+
| 2. ABI 階層 (呼び出し規約・データ表現)                                  |
|    - 引数および戻り値のレジスタ配置 (各 ISA 固有: r0..r3, a0..a7, rdi 等)|
|    - レジスタ保存責任 (Caller-saved vs Callee-saved)                   |
|    - スタックアライメント (8B / 16B 境界整列契約)                      |
|    - C++ 例外・アンワインド境界、構造体パディング規約                  |
+------------------------------------------------------------------------+
                                    |
+------------------------------------------------------------------------+
| 3. SoC / ボード階層 (周辺回路・チップ統合仕様)                         |
|    - メモリマップ (SRAM, XIP Flash, ROM, MMIO アドレス割り付け)        |
|    - 割込みコントローラ (NVIC, PLIC, CLIC, ESP INTC) の優先度・ルーティング|
|    - システムタイマ (SysTick, Systimer, RISC-V mtime, HPET)            |
|    - マルチコア相互排除機構 (SIO Spinlock, Mailbox, Hardware Mutex)    |
|    - キャッシュ制御・バス調停・DMA コヒーレンシ                        |
+------------------------------------------------------------------------+
```

== ISA / SoC 別アーキテクチャ評価

代表的なターゲット環境について、11 の評価軸に基づき詳細な比較・検証を行う。各主張について、確認済み事実 (confirmed)、設計上の推論 (inference)、未確認事項 (unknown) を明確に区別する。

=== 11 軸比較一覧表

#table(columns: (0.8fr, 1.1fr, 1.1fr, 1.1fr, 1.2fr, 1.2fr, 1.4fr), inset: 3pt,
  table.header(repeat: true, [比較軸], [ARMv6-M (M0+)], [ARMv7-M (M3/M4/M7)], [ARMv8-M Main (M33)], [RISC-V (RV32 プラットフォーム)], [Xtensa (LX6/LX7)], [x86-64 (OS環境)]),
  [1. 入口], [SVC (HW stack 32B)], [SVC (HW stack 32/104B)], [SVC (HW stack 32/104B)], [ECALL (SW stack: sync時はcallerのみ)], [syscall (SW stack / Windowed時はspill)], [SYSCALL (RSP不変, SW stack)],
  [2. 遅延切替], [PendSV], [PendSV], [PendSV], [MSIP / ソフトウェア割込み (platform)], [ソフトウェア割込み (INT)], [ソフトウェア割込み / IPI],
  [3. stack上限], [SW検査のみ], [SW / MPU guard (opt)], [PSPLIM (HW検査, Mainline標準)], [PMP guard (opt) / SW検査], [SW検査 / MMU guard (opt)], [Page Guard (MMU)],
  [4. 特権], [Handler/Thread (nPRIV)], [Handler/Thread (nPRIV)], [Handler/Thread (nPRIV)], [M-mode / U-mode (opt)], [User / Privileged], [Ring 0 (Kernel) / Ring 3],
  [5. 保護], [MPU (opt, RP2040無)], [PMSAv7 MPU (opt)], [PMSAv8 MPU (opt)], [PMP (opt, C6は16エントリ)], [MPU (LX7 opt) / MMU (LX6 opt)], [Page Tables (CR3/PTE)],
  [6. 割込みmask], [PRIMASK (可変優先度のみ遮蔽; NMI/HardFault非遮蔽)], [BASEPRI (opt) / PRIMASK (NMI/HardFault非遮蔽)], [BASEPRI (opt) / PRIMASK (NMI/HardFault非遮蔽)], [mstatus.MIE (同hart maskableのみ; NMI/例外非遮蔽)], [PS.INTLEVEL (レベル別遮蔽; 高位NMI/例外非遮蔽)], [RFLAGS.IF (INTRのみ遮蔽; NMI/SMI/例外非遮蔽)],
  [7. ISR判定], [IPSR != 0], [IPSR != 0], [IPSR != 0], [mcause (bit31) + trap_depth], [PS.EXCM + trap_depth], [IDT handler + trap_depth],
  [8. atomic], [なし (SoC SIO spinlock 等)], [LDREX / STREX (Exclusive monitor; CASはloop)], [LDREX / STREX (Exclusive monitor; CASはloop)], [Zalrsc (LR/SC opt; CASはloop), Zaamo (AMO opt; CAS非対応), Zacas (native CAS opt)], [S32C1I (native CAS opt, 条件付き)], [LOCK CMPXCHG (native CAS), LOCK XADD (RMW)],
  [9. 保存reg], [r0-r3,r12,LR,PC,xPSR], [同左 (+FP auto opt)], [同左 (+FP lazy auto opt)], [sync: caller-saved, async: full], [Call0: 規約通り, Win: spill依存], [sync: RCX/R11破壊,他SW],
  [10. cycle], [なし (Timer代用)], [DWT CYCCNT (opt, 32b, TRCENA要)], [DWT CYCCNT (opt, 32b, TRCENA要)], [rdcycle / mcycle (opt, 64b, mcounteren要)], [CCOUNT (opt, 32b)], [RDTSC / RDTSCP (64b, Invariant TSC/LFENCE要)],
  [11. multicore], [SoC SIO spinlock 等], [DMB/DSB, 排他モニタ (cond)], [DMB/DSB, 排他モニタ (cond)], [FENCE, AMO (opt), IPI (platform)], [S32C1I (opt), MEMW, DPORT], [LOCK, MFENCE, LAPIC IPI],
)

=== 各アーキテクチャの公式仕様検証と詳細評価

==== ARMv6-M (Cortex-M0+, 代表: RP2040)
- *一次資料*:
  - ARM DDI 0419C (ARMv6-M Architecture Reference Manual) §A4.1, §B1.4, §B1.5。(confirmed)
  - ARM DDI 0484C (Cortex-M0+ Technical Reference Manual) §1.3。(confirmed)
  - Raspberry Pi RP2040 Datasheet (Raspberry Pi Ltd., 2024) §1.2, §2.3.1.7。(confirmed)
- *特権状態とメモリ保護の分離*: ARMv6-M は `CONTROL.nPRIV` による特権/非特権 Thread mode の切り替えを仕様上サポートしている (DDI 0419C §B1.4.2)。一方、PMSAv6 MPU は*オプション*であり (DDI 0484C §1.3)、RP2040 SoC では MPU が実装されていない (RP2040 Datasheet §1.2)。特権分離が存在してもハードウェアメモリ保護が存在しない実態を正しく区別する。(confirmed)
- *排他命令の不在*: ARMv6-M アーキテクチャには `LDREX` および `STREX` 命令が*一切存在しない* (DDI 0419C §A4.1)。そのため CPU 単体でのアトミック CAS や排他モニタは利用不能。(confirmed)
- *RP2040 での解決策 (SoC 固有オプション)*: RP2040 SoC では、SIO ブロックに 32 個のハードウェア・スピンロック (`SIO_SPINLOCKn`) が実装されており、単一コア内では `PRIMASK` による割込み禁止、マルチコア間ではこの SIO スピンロックを併用することで atomic 操作を実現する (RP2040 Datasheet §2.3.1.7, `armv6m.hpp:131–150`)。これは当該 SoC 固有の実装オプションであり、ARMv6-M ISA そのものの汎用保証としては扱わない。(confirmed SoC fact)
- *割込みマスクの範囲*: `PRIMASK` は可変優先度の例外・割込みのみを遮蔽し、NMI および HardFault はマスクされない (DDI 0419C §B1.4.3)。(confirmed)
- *スタック限界と計測*: `PSPLIM` は存在せず、ソフトウェアによるプロローグ検査または未保護となる。サイクルカウンタ (DWT CYCCNT) も未搭載であり、1 µs 分解能タイマによる多数回平均測定で代替する。(confirmed)

==== ARMv7-M (Cortex-M3/M4/M7)
- *一次資料*: ARM DDI 0403E (ARMv7-M Architecture Reference Manual) §A4.1, §B1.5, §B3.5, §C1.8。(confirmed)
- *オプション性の明示*: PMSAv7 MPU、単精度/倍精度 FPU、および DWT CYCCNT はアーキテクチャ上すべて*オプション*である。(confirmed)
- *排他命令とスコープ*: `LDREX`/`STREX` を完備し、排他モニタによる CAS ループを構成できる。ただし Local exclusive monitor はコア内のみを監視し、マルチコアやバスマスタ間の Global exclusive monitor の成否は SoC の Shareability domain およびバス調停機構に依存する (conditional)。(confirmed)

==== ARMv8-M Mainline (Cortex-M33, 代表: RP2350)
- *一次資料*:
  - ARM DDI 0553C (ARMv8-M Architecture Reference Manual) §B1.5.15, §B3.19 (MSPLIM/PSPLIM), §B4.22 (FP Context), §B4.25 (PMSAv8 MPU)。(confirmed)
  - Arm 100235_0100_06 (Cortex-M33 Technical Reference Manual) §3.3.1, §4.5, §4.6。(confirmed)
  - Raspberry Pi RP2350 Datasheet (Raspberry Pi Ltd., 2024) §1.2, §2.3。(confirmed)
- *Mainline 標準機能と SoC オプションの分離*:
  - `MSPLIM` および `PSPLIM` によるスタック境界検査は、ARMv8-M Mainline 仕様における標準機能として定義されている (DDI 0553C §B1.5.15, §B3.19; Cortex-M33 TRM §3.3.1)。(confirmed)
  - 一方、Security Extension (TrustZone)、FPU、および PMSAv8 MPU (0/4/8/12/16 領域) は、Cortex-M33 コアの実装オプションである (100235 §4.5, §4.6)。RP2350 SoC では TrustZone、単精度 FPU、8 領域 MPU、PSPLIM が組み込まれているが (RP2350 Datasheet §2.3)、ARMv8-M コア全般において MPU や FPU が必須であると一般化してはならない。(confirmed)
- *FPU Lazy Stacking*: FPU 搭載時は `FPCCR.LSPACT` による遅延退避が作動し、浮動小数点命令が実行されるまで物理スタッキングを遅延させる (100235 §4.6)。(confirmed)

==== RISC-V RV32 (代表: ESP32-C3/C6/H2/P4, Hazard3)
- *一次資料*:
  - RISC-V Unprivileged ISA Specification (Doc Ver 20240411) Chap 13 ("A" Extension, §13.2 Zalrsc, §13.3 Eventual Success, §13.4 Zaamo), Chap 15 ("Zacas" Extension), Chap 17 ("Zalasr" Extension)。(confirmed)
  - RISC-V Privileged ISA Specification (v1.10 / v1.13) Chap 2, 3。(confirmed)
  - Espressif ESP32-C6 Technical Reference Manual (TRM v1.1) Chap 2, 8, 9。(confirmed)
- *A 拡張の細分化と Zacas の依存関係*:
  - 従来の「A」拡張は、`Zaamo` (Atomic Memory Operations) と `Zalrsc` (Load-Reserved / Store-Conditional) に細分化・独立定義された (Unprivileged Spec Chap 13)。
  - `Zaamo` は不可分演算 (AMO: add, swap, and, or, xor, max, min) を提供するが、CAS 命令は含まない (Chap 13 §13.4)。
  - `Zacas` (Atomic Compare-and-Swap: `amocas.w` 等) 拡張は `Zaamo` に依存する (Chap 15 §15 statement: "The Zacas extension depends upon the Zaamo extension.")。
  - `Zalasr` は Atomic Load-Acquire / Store-Release を提供する (Chap 17)。(confirmed)
- *LR/SC の保証範囲と同期短経路・ISR 適格性の制約*:
  - RISC-V 仕様 §13.3 "Eventual Success of Store-Conditional Instructions" が規定する constrained LR/SC loop の保証は、*eventual success (結果的成功、ライブロックの防止)* に限定される。
  - これは有界実行時間 (Bounded Execution Time / 確定的な最悪実行時間 bound) を保証するものではない。キャッシュラインの競合 (cache line bouncing) やバス調停、頻発する割込みにより、SC が複数回失敗して再試行が繰り返される可能性があり、最大レイテンシが非有界となり得る。
  - したがって、厳格な時間予算が課される ISR や確定的同期短経路 CAS において、LR/SC 再試行ループは単独では不適格 (unsuitable) となる場合がある。
  - *代替手段*: 単一命令で確定的に完結する `Zacas` (`amocas.w`)、単一ハートでの割込みマスク (`mstatus.MIE = 0`) によるクリティカルセクション、またはハードウェアスピンロック / AMO (`Zaamo`) による排他制御を採用する。(confirmed)
- *A 拡張欠落時の限界*: A 拡張を持たない RV32I/E では、ハードウェア AMO も LR/SC もない。単一ハートにおける `mstatus.MIE = 0` による割込みマスクは、当該ハートの maskable 割込みおよびスレッドプリエンプションのみを遮蔽するものであり、マルチハート (SMP)、DMA バスマスタ、NMI、別エージェントに対する atomicity は一切成立しない。(confirmed)
- *mcycle / rdcycle の条件性*: `rdcycle` / `mcycle` の実装、アクセス権 (U-mode からのアクセスは `mcounteren.CY` に依存)、カウンタ幅はアーキテクチャ上 optional / 条件付きである。(confirmed)
- *SoC プラットフォーム機能の開示状況と分離*:
  - ESP32-C6: RV32IMAC (シングルコア, 16 PMP エントリ), PLIC / INTC。(confirmed via ESP32-C6 TRM v1.1)
  - ESP32-C3 / H2 / P4 / Hazard3: チップ固有のハート数、独自割込みコントローラ、タイマ仕様は一次資料 TRM 現物を直接開いて検証していないため、*未確認 (unknown)* として扱う。CLINT, PLIC, CLIC, タイマ, ハードウェアスピンロックは ISA ではなく platform / SoC 固有機構である。(unknown)
- *トラップフレームの分離*: RISC-V のトラップはハードウェア自動スタッキングを行わない。全 31 本の GPR を常時一律に退避するのではなく、同期システムコール (`ecall`) では呼出し規約の caller-saved レジスタ (a0–a7, t0–t6, ra) のみを退避し、非同期割込みではプリエンプトに備えて完全退避を行う。`ecall` 復帰時は `mepc += 4` が必須で、M-mode の `ecall` は例外入口の asm (`rv32_trap.S` の ECALL 枝) が `mepc` を進め、a0 の番号で表 (`shizuku_rv32_ecall_table`) を引いて各 API の処理関数へ直接入る (割込み・例外と U-mode の `ecall` は従来どおり `trap_route.hpp:16–19` の `route_trap`)。切替えが起きない API (CALL_METHOD・記述子 call・EXIT_METHOD・FORWARD_CHILD_EXIT・GET_*・SET_BUDGET) の処理関数は 1 API = 1 関数で、本体 (`native_call.hpp`、always_inline) を展開し、`context_t` を組まず trap フレームを直接扱う。診断と入れ子の記帳 (`trap_diag`・`trap_depth`・`active_*`・`pend_flag`) は割込み・例外と、実行権を渡す API (YIELD・RUN_FOR など、切替えが起きうる経路) の側にだけ残し、遅い経路 (停止・kobj への転送) は tail call にして成功経路に `ra`・`s*` の退避を持たせない。戻りの再開フレームは `mepc`・`mstatus`・目印だけを書き、`a0`/`a1` 以外のレジスタの消去は出口 `.Lrv32_exit_ecall` が行う。(confirmed)

==== Cadence / Tensilica Xtensa (代表: ESP32 LX6, ESP32-S3 LX7, ESP32-S2)
- *一次資料ステータス*:
  - Cadence Tensilica Xtensa ISA Reference Manual は一般非公開のため「一次資料直接未確認 (unknown)」。
  - Espressif 公開一次資料: ESP32 Technical Reference Manual (v5.1, Chap 1, 3, 24), ESP32-S3 Technical Reference Manual (v1.5, Chap 1)。(confirmed)
  - ESP-IDF Tensilica HAL ヘッダ: `components/xtensa/include/xtensa/hal.h`。(confirmed for header declarations, unknown for implementation internals)
- *Register Window と Call0 ABI の評価*:
  - *Windowed ABI*: 物理レジスタファイルとウィンドウ回転機構 (`ENTRY`/`RETW`) を使用。ウィンドウ枯渇時は `WindowOverflow`、復帰時は `WindowUnderflow` 例外が発生する仕様は外部文献からの推論 (inference)。
  - *`xthal_window_spill()`*: `xtensa/hal.h` に `extern void xthal_window_spill(void);` と宣言されている事実は確認済み (confirmed)。ただし、その内部アセンブリ実装の完全な実行挙動やコンテキスト制約は未確認 (unknown) として区別する。
  - *Call0 ABI*: ウィンドウ回転を使用せず、固定 16 レジスタ (`a0`–`a15`) で通常通りプロローグ/エピローグ退避を行う。ウィンドウ例外のオーバーヘッドを排除できるが、コードサイズ増加等のトレードオフがあり、中立な選択肢として評価する。(inference)
- *排他命令*: オプションの `S32C1I` (Store 32-bit Conditional) 命令が実装されている構成でのみハードウェア CAS が可能。未実装時は割込みレベル引き上げ (`PS.INTLEVEL`) または SoC 排他レジスタに依存。(conditional)

==== x86-64 (OS 環境) および UEFI Boot Phase
- *一次資料*:
  - Intel 64 and IA-32 Architectures Software Developer's Manual (Combined Volume, Order Number 325462-085US): Vol 1 Chap 13 (XSAVE/XCR0), Vol 2A Chap 3 (CPUID), Vol 2B Chap 4 (RDTSC, RDTSCP, SYSCALL), Vol 3A Chap 2 (CR4), Vol 3A Chap 6 (IDT/Exceptions)。(confirmed)
  - AMD64 Architecture Programmer's Manual (APM Vol 3, Publication No. 24594, Rev 3.36) Chap 3 (SYSCALL, SYSRET, SWAPGS)。(confirmed)
  - Unified Extensible Firmware Interface (UEFI) Specification v2.10: Chapter 7 §7.4.6 (Boot Services: ExitBootServices), Chapter 8 (Runtime Services)。(confirmed)
- *SYSCALL 命令のハードウェア挙動*:
  - `SYSCALL` 命令は RIP を `RCX` に、RFLAGS を `R11` に保存し、`IA32_LSTAR` MSR へジャンプする。
  - *重要*: `SYSCALL` 命令はスタックポインタ (`RSP`) を*自動変更しない* (Intel SDM Vol 2B SYSCALL)。(confirmed)
  - *スタック切替えの OS 選択肢*: 突入後に `SWAPGS` で per-CPU 領域からカーネルスタックを取得・切り替える手法は OS 設計の有力な選択肢であるが、ISA 上の唯一必須手段ではない。(inference)
- *拡張状態管理 (XSAVE / XRSTOR)*:
  - 有効化手順:
    + CPUID サポート確認: `CPUID.(EAX=01H):ECX.XSAVE[bit 26] == 1`
    + OS による有効化: `CR4.OSXSAVE[bit 18] = 1`
    + 有効化反映確認: `CPUID.(EAX=01H):ECX.OSXSAVE[bit 27] == 1`
    + 各ステートのサイズ・オフセット確認: `CPUID leaf 0DH`
    + 管理対象ステートの有効化: `XCR0` に各コンポーネント (bit 0: x87, bit 1: SSE, bit 2: AVX 等) をビットマスク設定。(confirmed)
- *UEFI ExitBootServices() と Runtime Services の境界*:
  - UEFI Specification v2.10 §7.4.6 に従い、OS ローダーが `gBS->ExitBootServices()` を呼び出すことで Boot Services は終了し、利用可能メモリの所有権が OS へ移管される。
  - 以後、Boot Services 関数および device-handle protocols は使用不能となるが、Chapter 8 で規定された Runtime Services (GetTime, SetVirtualAddressMap, ResetSystem 等) は残存する。(confirmed via UEFI 2.10 §7.4.6, Chap 8)
  - `MapKey` が最新メモリマップと一致しない場合は `EFI_INVALID_PARAMETER` で失敗するため、`GetMemoryMap()` で再取得してリトライする契約がある。
  - *重要*: `ExitBootServices()` 自体が IDT や CR3 (ページテーブル) やスケジューラを設定したり、OS メモリマップを永久固定するわけではない。x64 システムコール設計は UEFI runtime ではなく、ブートハンドオフ後の OS 実装として評価する。(confirmed)

== 元決定 1–11 の各 ISA 整合性評価

元文書における決定 1–11 を所与の不変条件とし、各 ISA ファミリー上での成立性、費用差、代替手法を評価する。「ソフトウェアだから普遍成立」と一括りにせず、不成立 (infeasible)、高費用 (high-cost)、代替 (alternative)、未知 (unknown) を明確に区分する。

#table(columns: (0.8fr, 1.6fr, 1.4fr, 2.2fr), inset: 3.5pt,
  table.header(repeat: true, [決定番号], [元文書の決定内容], [成立性 / 費用区分], [各 ISA バリアントでの成立性・費用差・代替・未知]),
  [決定 1], [IRQ 予算 (初期案 10 µs / mask 2 µs)、計測必須], [条件付き成立 (DWT/rdcycle 実装かつ有効化時のみ低費用; 未実装・未許可時はタイマ代替)], [ARMv7/v8: DWT CYCCNT が実装され `DEMCR.TRCENA` が有効化されている場合のみ低費用成立。RV32: `rdcycle` が実装され `mcounteren.CY` でアクセス許可されている場合のみ低費用成立。ARMv6-M / カウンタ未実装時: タイマ多数回平均測定へ代替 (単発計測は不能)。x64: Invariant TSC かつ `LFENCE; RDTSC` シリアライズが必要。],
  [決定 2], [native は explicit method、objectland raw SVC 解釈維持], [成立 (論理モデル共通)], [ディスパッチャの論理モデルであり成立。raw 番号は ABI 引数レジスタ (ARM: `r0`, RV: `a0`, x64: `rdi`/`rax`, Xtensa: `a2`) に載せて親へディスパッチ。],
  [決定 3], [context reuse呼出しは専用戻り経路で復元], [成立 (各アーキテクチャゲート設計)], [ARM: Handler EXC_RETURN で成立。RV32: 専用 MRET アセンブリゲートで成立。x64: SYSRET/IRET ゲートで成立。普通の C return 誤用禁止契約は全共通。],
  [決定 4], [保存契約 (register/flags) 維持], [成立 (費用差あり)], [ARMv8-M: HW 自動退避活用で最小差。RV32/x64: 自動退避がないため同期 syscall (caller-saved) と非同期割込み (full) の分離設計で費用抑制。],
  [決定 5], [動的ホットスワップ棚上げ、actor ID は定数に焼かない], [成立 (データ構造共通)], [純粋なデータ構造設計。世代記述子ポインタ経由で actor ID を取得する原則を全 ISA のヘッダで維持。],
  [決定 6], [atomic は 1 ID + op、カーネル H は CAS/load/store のみ], [x64/Zacasで成立、LL/SCは条件付き(非有界retryリスク)、Xtensa条件付き、ARMv6-M/RV32Iで不成立(委任/SoC代替)], [
    + *x64*: `LOCK CMPXCHG` によるネイティブ CAS 命令が直接成立。
    + *RV32 (Zacas 実装時)*: `amocas.w` によるネイティブ CAS 命令が直接成立。
    + *ARMv7-M / ARMv8-M / RV32 (Zalrsc 実装時)*: `LDREX`/`STREX` または `lr.w`/`sc.w` による排他モニタを用いた CAS 再試行ループとして構成可能。ただし競合時の retry が非有界になりうるため、厳格な有界時間 (Bounded Execution Time) や ISR 適合性を要する同期短経路では上限管理や代替設計が必要。
    + *Xtensa*: オプションの `S32C1I` 命令が実装されている構成でのみ単一命令 CAS が条件付きで成立 (conditional)。未実装構成では割込みレベル引き上げ (`PS.INTLEVEL`) または SoC 排他機構へフォールバック (Cadence 一次資料は unknown)。
    + *RV32 (Zaamo のみ)*: AMO は不可分演算 (add, swap 等) を提供するが CAS 命令ではないため CAS 自体は不成立。
    + *ARMv6-M / RV32I*: ハードウェア CAS も排他モニタも不在。単一コア割込みマスク、または RP2040 SIO スピンロック等の外付け SoC 排他機構 (プロトコル遵守前提)、あるいは単一コア所有への委任 (delegation) 等の代替設計へ劣化。
  ],
  [決定 7], [`//abi` 正本は Shizuku-ota-test、ID と権限同期], [成立 (共有 ID 定義のみ)], [ABI 正本ヘッダは可搬な通信/オブジェクト ID 定義 (portable wire / object-ID definition) であり、全ターゲットでの普遍的バイナリ互換性を証明するものではない。各 port のアダプタ実装責務が残る。],
  [決定 8], [宛先指定 YIELD 廃止、GRANT 貸付けと無引数 YIELD], [成立 (核アルゴリズム共通)], [スケジューラ核のアルゴリズムであり全 ISA で共通成立。現行コードの暫定引数 `yield(0)` との不一致は要修正。],
  [決定 9], [専用 wait/wake queue 不採用、mutex は CAS+GRANT], [成立 (核アルゴリズム共通)], [決定 8 と直結。futex キューを排除し、最小のメモリフットプリントを全 ISA で維持。],
  [決定 10], [ESP-IDF Wi-Fi/BLE shim は関数として閉じ object 化しない], [成立 (ESP32/C6 固有)], [RISC-V (C6) および Xtensa (ESP32) の移植において、shim 内部で `kernel_services` を直接呼ぶことでオーバーヘッドを極小化。他 ISA では適用外。],
  [決定 11], [小さい `kernel_services` 契約を V2/V3 が共通実装], [成立 (スコープ限定インターフェース契約)], [内部 C++ ヘッダ 1 枚に集約。各アーキテクチャの port 層が ABI バックエンド責務を実装することで成立。上位互換性の無条件保証ではない。],
)

== kernel_v3.typ の Cortex-M 固有記述と置換候補一覧

=== 分類基準と除外行の明確化 (Inclusion / Exclusion Criteria)

元文書 `docs/design/kernel_v3.typ` (スナップショット SHA-256: `625956303e0b7d7f9049a3bcc42f5fb565a1104f5e6afdf5026e6e3af97f97d0`、全 694 行) を対象に、Cortex 関連の 20 語キーワード (`Handler`, `Thread mode`, `SVC`, `PendSV`, `MSP`, `PSP`, `EXC_RETURN`, `CONTROL`, `PSPLIM`, `r4–r11` / `r4-r11`, `PRIMASK`, `IPSR`, `AAPCS`, `FPCCR`, `LSPACT`, `xPSR`, `Cortex`, `M0+`, `M33`, `MPU`) を大文字小文字無視で literal スキャンした結果、*130 unique source lines* が抽出された。

本書では、この 130 行に加え、差分比較表ブロックなどの関連行を含めた全行を厳密に監査し、*置換候補 (Candidates: C)* と *除外行 (Exclusions: E)* に完全分類した。

+ *分類集計の完全整合関係 (Audit Statistics)*:
  - *Keyword scan unique hits (N)*: `130` 行
  - *置換対象候補 (Candidates: C)*: `106` 行 (固有行、重複 0)
  - *除外行 (Exclusions: E)*: `32` 行 (固有行、重複 0)
  - *分類和集合 (Union: U)*: `138` 行 ($C + E = 106 + 32 = 138$)
  - *重複 (Overlap: O)*: `0` 行 ($C inter E = emptyset$)
  - *未分類キーワード行 (Unclassified keyword hits)*: `0` 行 ($N subset.eq U$)
  - *キーワード非含有の候補/除外行 (K)*: `8` 行 ($U without N$)
    - 候補側 (4 行): `L541`, `L546`, `L547`, `L548` (元文書 11 軸差分比較表ブロックの構造行)
    - 除外側 (4 行): `L65`, `L235`, `L563`, `L587` (実測値引用および一般的時間予算行)
+ *抽出・置換対象基準 (Inclusion Criterion)*:
  - Cortex-M 固有のハードウェア動作・レジスタ・例外機構を直接前提として規定されているアーキテクチャ設計・API traits 分類・コンテキスト退避・復帰ゲートの記述行、および差分比較表の全行 (全 106 行)。
+ *除外基準と内訳 (Exclusion Criteria and Reasoning: 全 32 行)*:
  - *基準 1: V2 過去実装・実測値の引用 (計 12 行)*:
    `L63`, `L65`, `L66`, `L67`, `L71`, `L72`, `L81`, `L82`, `L83`, `L563`, `L603`, `L605`。
    これらは RP2350 上の V2 カーネルの実測プロファイル値 (`cost3_report.md`, `cost2_trace.log` 等からの引用事実) であり、V3 の一般設計仕様そのものの記述ではないため除外。
  - *基準 2: MPU/PMP 汎用保護の並記 (計 7 行)*:
    `L32`, `L329`, `L346`, `L372`, `L377`, `L419`, `L454`。
    これらは既に元文書内で「MPU/PMP」として RISC-V PMP と併記された汎用メモリ保護の言及であり、Cortex-M 専用の偏った記述ではないため除外。
  - *基準 3: リポジトリ・ファイル名・メタデータ参照 (False Positive: 計 6 行)*:
    `L17` (git ブランチ名 `perf/svc-in-ram`), `L140` (ソース名 `handler.cpp:507–509`), `L143` (ソース名 `handler.cpp:368–435`), `L354` (テスト名 `handler_binding.cpp`), `L438` (ソース名 `handler.cpp:1205–1285`), `L649` (テスト名 `handler_binding.cpp:201–220`)。
    これらはファイルパスやブランチ名に文字列 "svc" / "handler" が含まれる単なる字句的ヒットであり、アーキテクチャ上の Cortex-M 依存記述ではないため除外。
  - *基準 4: オブジェクトモデル用語・権限属性 (False Positive: 計 3 行)*:
    `L150` (オブジェクトモデルの親ハンドラ `native host-handler`), `L364` (親ハンドラ経由の戻り `handler を経由せず直接 native RETURN`), `L583` (オブジェクト権限属性 `HANDLER 宣言`)。
    これらは Shizuku オブジェクトモデルにおける「親オブジェクトハンドラ」用語であり、ハードウェアの Handler mode ではないため除外。
  - *基準 5: 一般論・性能時間予算および資料リンク (計 4 行)*:
    `L208`, `L235`, `L587` (時間予算・測定境界), `L688` (Cortex-M33 Generic User Guide 資料リンク)。
    これらは一般的な時間制約または参考文献 URL であるため除外。

=== 置換候補一覧 (全 106 行 / 106 エントリ)

以下に、置換対象となる全 106 行の厳密な行番号、元文書記述、および一般化 / ISA 別の置換案を示す。

#table(columns: (0.5fr, 2.3fr, 2.2fr), inset: 3pt,
  table.header(repeat: true, [行番号], [元文書の記述 (Cortex-M 固有部)], [一般化および ISA 別の置換案]),
  [L025], [Thread mode のカーネル処理は、その世代のカーネル actor ID を持つ。], [通常実行文脈 (Execution / Task Context) のカーネル処理は、その世代のカーネル actor ID を持つ。],
  [L029], [native API 表を objectland raw SVC へ適用しない。], [native API 表を objectland の生システムコール・トラップ (raw Trap / ECALL) へ適用しない。],
  [L030], [objectland では raw SVC を受ける固定 parent-handler lane を使う。], [objectland では生のシステムコール・トラップを受ける固定 parent-handler レーンを使う。],
  [L031], [GET、短い stream batch、atomic は Handler mode で完結する。待ち・譲渡・入れ子は Thread mode で行う。], [同期短経路 (同期トラップ内) で完結する。待ち・譲渡・入れ子は通常実行文脈 (スレッド文脈) で行う。],
  [L064], [整数／PSP／CONTROL／PSPLIM／FP 復元。BX と unstack は含まない。], [整数／スレッドスタックポインタ／実行特権ステート／スタック限界／拡張レジスタの復元。復帰命令と unstack は含まない。],
  [L076], [V2 の GET_CURRENT_OBJECT は SVC 2 回、exception return 2 回、push/pop 各 1。], [V2 の GET_CURRENT_OBJECT は 同期トラップ 2 回、例外復帰 2 回、push/pop 各 1。],
  [L077], [method 1 段は SVC/return 各 5 回、push 3 回、pop 3 frame。], [method 1 段は システムコール・トラップ／例外復帰 各 5 回、push 3 回、pop 3 frame。],
  [L078], [caller → 親カーネル actor → primitive CALL → leaf → 親 EXIT_METHOD → primitive RETURN → カーネル handler return という中継が増やしている。], [中継チェーン末尾の復帰を「カーネル同期トラップ復帰 (kernel trap return / arch gate)」へ一般化。],
  [L093], [Handler mode に入った事実を根拠に、元の非特権 caller を特権と判定してはならない。], [例外・割込みハンドラ (同期トラップ文脈) に入った事実を根拠に、元の非特権呼出し元を特権と判定してはならない。],
  [L097], [以下は現行 API 31 個の V3 分類案である。H = Handler 内完結、T = Thread mode の kernel actor。], [H = 同期トラップ内完結 (同期短経路)、T = 通常実行文脈のカーネル actor。],
  [L104], [  [1], [CREATE_OBJECT], [非特権可], [×], [T/S1], [REPLACE は特権、HANDLER 宣言は composition authority。allocation／公開。],], [CREATE_OBJECT のモード・保存レベルを「deferred_kernel_task / S1」と中立定義し、HANDLER 宣言を「親ハンドラ権限宣言」として一般化。],
  [L138], [ISR stream 呼出しは driver 所有 buffer と driver 席であり、user svc の「origin thread stack 内」という copy 範囲をそのまま使わない。], [「user svc」を「ユーザー空間システムコール (user syscall / trap)」へ一般化し、呼出し元スレッドスタックのコピー境界を中立記述。],
  [L155], [以下の `EntryView` はその保存済み continuation を参照する。`prepare_kernel_landing` が C++ 内の live r4–r11 を...], [呼出し元の保存済み継続 (callee-saved レジスタ群) を参照する。],
  [L159], [`template<Api Id, class Arch> struct svc_traits; // exhaustive; no default traits`], [SVC 固有命名を排し、中立な `template<Api Id, class Arch> struct api_traits;` へ一般化。],
  [L163], [`using A = svc_traits<Id, Arch>;`], [中立型エイリアス `using A = api_traits<Id, Arch>;` へ置換。],
  [L168], [`if constexpr (A::mode == handler) {`], [Cortex-M の handler モード名を排し、`if constexpr (A::mode == inline_synchronous_trap)` へ置換。],
  [L170], [`return handler_service<Id>(origin, entry); // same exception, no registry callback`], [同期短経路呼出しを `return synchronous_trap_service<Id>(origin, entry);` へ置換。],
  [L176], [`// generated exhaustive switch; ID read from argument register, never SVC immediate`], [引数レジスタ (ABI 規定の第1引数) から ID を読み、トラップ命令の即値に依存しない。],
  [L187], [HW_STACKING、PSPLIM、FP、MPU/PMP、native AMO 等へ拡張する。], [機能変数 (`HAS_GPR_HARDWARE_STACKING`, `HAS_HARDWARE_STACK_LIMIT` 等) に基づき静的分岐する。],
  [L192], [== SVC 入口で保存する最小集合], [== システムコール・トラップ入口で保存する最小集合],
  [L194], [=== Cortex-M の同じ caller へ返す経路 (S0)], [節見出しを「同一呼出し元へ直接復帰する同期短経路 (S0)」へ一般化。],
  [L196], [Thread/PSP からの SVC では hardware が r0–r3、r12、LR、PC、xPSR を積む。], [通常実行文脈からの同期トラップでは、ハードウェア自動スタッキング (ARM) またはアセンブリ入口 (RISC-V/x64) により基本スクラッチを退避する。],
  [L197], [入口は EXC_RETURN を BL 前に保持し、stack selector と FP frame 種別から core frame の位置を求める。], [入口は例外復帰トークン (または復帰ステータス CSR) を保持し、スタックポインタおよびフレーム種別から基本フレーム位置を特定する。],
  [L201], [同一 Thread・同一 PSP/CONTROL/PSPLIM・同一保護領域へ同じ例外から返し、... r4–r11 を thread context へ常時丸ごと退避する必要はない。], [同一スレッド・同一スタック・同一特権・同一保護領域へ同例外から返す場合、callee-saved レジスタをスレッド継続へ常時丸ごと退避する必要はない。],
  [L202], [C の prologue が使う callee-saved を MSP 上へ保存することは依然必要であり、「r4–r11 を一切保存しない」という意味ではない。], [コンパイラが例外スタック上に作業用 callee-saved を退避することは依然必要である。],
  [L204], [上位 IRQ は現在の Handler レジスタを保全し、Thread 切替えを外側の例外終了まで延期する。PendSV の tail chaining 後にも...], [上位割込みは実行中レジスタを保全し、遅延切替割込みの終了までコンテキスト切替えを延期する。],
  [L206], [S0 のソフト保存は EXC_RETURN、frame view、必要な origin/IRQ mask の最小 scratch に限定する。], [S0 の退避は例外復帰情報、フレーム参照、必要な呼出し元/割込みマスクの最小スクラッチに限定する。],
  [L207], [PSP は参照するが変更しない。CONTROL/PSPLIM は origin の確認だけを行い、同一条件なら書戻さない。], [スレッドスタックポインタは参照するが変更しない。特権ステート・スタック限界レジスタは確認のみ行い、書戻さない。],
  [L209], [結果だけを元 core frame の r0/r1 に書き、保持した EXC_RETURN で *Handler mode から*復帰する。], [結果だけを元フレームの戻り値レジスタに書き、保持した復帰情報で *例外ハンドラから直接* 復帰する。],
  [L213], [static ID 分岐で T と分かった時点で、最初に r4–r11 を caller continuation に一度保存する。], [静的分岐で通常実行と判明した時点で、呼出し元の全 callee-saved レジスタをスレッド継続に保存する。],
  [L214], [capture 前に C 関数の作業で live registers を書き換えない。入口を caller-saved のみの assembly にするか、...], [入口を呼出し規約スクラッチのみのアセンブリとするか、確実に全レジスタを捕捉する。],
  [L215], [PSP、stack bounds、EXC_RETURN、CONTROL の caller privilege/SPSEL/FP 状態、必要な PSPLIM と保護 descriptor 世代を保存する。], [スレッド SP、スタック境界、例外復帰情報、特権・拡張ステート、保護記述子世代を保存する。],
  [L216], [現行 restore のように「今の CONTROL の FPCA を流用」するだけで異なる FP thread へ移らない。], [現在の特権ステータスの拡張フラグを安易に流用して異なる拡張コンテキストへ移らない。],
  [L218], [FP 有効時は EXC_RETURN FType、FPCCR.LSPACT 等を調べ、lazy frame がまだ実体化していないか区別する。], [拡張レジスタ有効時はハードウェアの遅延退避ステータスを調べ、実体化の有無を判定する (RISC-V/x64 では明示退避フラグを検査)。],
  [L224], [=== Handler 文脈からの呼出し], [=== 割込み・例外ハンドラ文脈からの呼出し],
  [L226], [SVC/ECALL を IRQ 内から再発行することを ISR API としない。], [システムコール・トラップ命令を割込みハンドラ内から再発行することを ISR API としない (直呼び出し契約とする)。],
  [L227], [Cortex-M は EXC_RETURN の stack selector に従い MSP の interrupted-handler frame も認識するが、...], [割込み内からの呼出しは例外スタック上のフレームを介さず、特権 `kernel_services` の直接呼出しを使用する。],
  [L229], [直呼び wrapper は通常の C 引数／戻り値で H 本体と接続し、SVC frame を捏造せず、ISR 自身の復帰を途中で行わない。], [直接呼出しラッパーは通常の C 関数呼出し規約で同期短経路本体と接続する。],
  [L230], [SVC 優先度によっては IRQ 内 SVC が安全に入れないため、「SVC を撃てば context 検査できる」という契約にはしない。], [トラップ例外の優先度やネスト制約に依存せず、C 言語関数レベルの直接呼出しで完結させる。],
  [L232], [== Handler mode の制約と上限], [== 同期トラップ内処理の制約と上限],
  [L236], [Handler span と PRIMASK 禁止区間の最大値・sample 数・単位を計測ビルドで必ず記録する。], [同期トラップ実行時間 (Trap Span) と大域割込み禁止区間の最大値を計測ビルドで記録する。],
  [L239], [- block、yield/grant/sleep、nested SVC、unbounded allocation、user callback、printf、無制限 table scan を禁止する。], [同期短経路内でのネストしたシステムコール発行やブロック操作を一律禁止する。],
  [L241], [- CAS retry／pin／lock acquisition の回数にも上限を証明する。既存 `handler.cpp:1000–1005,1057–1082` の loop をそのまま H へ移すことはできない。], [同期短経路 (inline_synchronous_trap) 内でのループ・再試行回数の有界時間証明 (work-bound 要件) へ一般化。],
  [L242], [- Thread が持つ lock を同じ core の SVC/IRQ が待つと持ち主が再開できない。共有 short lock は全取得側で該当割込みを遮蔽...], [通常スレッドが保持するロックを同一コアのトラップ/割込みハンドラが待つデッドロックを防止する。],
  [L247], [それ以外の kernel fault、MSP overflow、frame 不整合は kernel fault として診断停止する。], [特権・例外スタックのオーバーフローやフレーム不整合はカーネルフォールトとして診断停止する。],
  [L248], [SVC priority による fault escalation、M0+ の HardFault しか使えない経路も board ごとに設計・試験する。], [システムコール優先度によるフォールト昇格や、単一フォールトハンドラしか持たないコアの復旧経路を個別設計する。],
  [L250], [MSP 上限は入口 scratch + compiler stack-usage + 許可した最大 IRQ nesting + fault 診断余白から算出し、...], [特権・例外スタック上限は入口スクラッチ＋コンパイラ消費量＋最大割込みネスト＋診断余白から算出する。],
  [L252], [固定の MSP 1 KiB 等を根拠なく安全とせず、最大 chain を静的集計し、canary/high-water と nesting stress で確認する。], [固定の特権スタックサイズを根拠なく安全とせず、最大チェーンを静的集計する。],
  [L258], [T syscall の入口は origin continuation を保持し、その SVC の exception return 先を *特権 Thread mode の kernel landing* にする。], [システムコール入口は呼出し元継続を保持し、例外復帰先を *特権通常実行文脈のカーネル landing* に設定する。],
  [L263], [kernel service は内部関数として scheduler/call/return 機構を直接呼ぶ。V2 primitive を再 SVC する経路を設けない。], [カーネルサービスは内部関数として直接呼び、システムコール・トラップの再発行を行わない。],
  [L264], [しかし Cortex-M の Thread mode は、PSP/privilege を準備しただけで EXC_RETURN を実行できない。], [通常実行文脈では、スタックや特権を設定しただけで例外復帰命令を任意に発行することはできない (アーキテクチャ規約違反)。],
  [L274], [専用復元処理が元 PSP、callee-saved、元 hardware frame、CONTROL/PSPLIM/FP、保護 descriptor を対応させる。], [専用復元ゲートが元スレッドSP、callee-saved、元フレーム、特権ステート、スタック限界、保護記述子を復元する。],
  [L275], [実装を assembly にするか C++ と arch gate の組合せにするかは実装上の選択だが、普通の C return を EXC_RETURN と見なさない。], [通常の C return を例外復帰と見なす実装を一律禁止する。],
  [L276], [Handler 内なら元 frame を選んで exception return できる。Thread kernel 内で実行する最終復元も reuse 用 gate の責任...], [同期トラップ内なら直接例外復帰可能。通常実行カーネル内の最終復元も専用復元ゲートの責任とする。],
  [L281], [現行 inline SVC は r2/r3/r12 を input-only、LR/cc を clobber していない (`armv8m.hpp:415–423`)。], [現行のインライン・システムコール命令は特定の引数レジスタを input-only とし、戻り番地やフラグを破壊しない契約としている。],
  [L299], [actor、PSP、CONTROL/FP 状態、保護 domain を変更する最後の gate は、scheduler が途中状態を保存しない有限の transition 区間とする。], [actor、スレッドSP、特権/拡張ステート、保護ドメインを変更する最終遷移ゲートは、アトミックな有限区間とする。],
  [L300], [Thread 復帰直前の IRQ と PendSV も受入試験に含める。], [通常スレッド復帰直前の割込みおよび遅延切替要求の競合も受入試験に含める。],
  [L307], [kernel landing が result/error と必要な復元情報を保持し、対象 logical frames を閉じ、context reuse 専用の復元 gate で元 caller の SVC 直後へ戻す。], [context reuse 専用の復元ゲートで元呼出し元のシステムコール発行直後へ戻す。],
  [L318], [arch continuation: 元 HW frame への参照、PSP/stack bounds、EXC_RETURN、r4–r11、必要な CONTROL/PSPLIM、活性 FP high registers...], [アーキテクチャ継続: 元フレームへの参照、スレッドSP/スタック境界、例外復帰情報、全callee-saved、特権ステート、保護情報。],
  [L334], [「境界」は SVC/ECALL 発行 1 回とその trap からの復帰 1 回の組で数える。], [「境界」は システムコール・トラップ発行 1 回とその例外復帰 1 回の組で数える。],
  [L335], [Thread mode の tail branch による actor／保護ドメイン移行は別に数える。], [通常実行文脈における直接分岐による actor／保護ドメイン移行は別に数える。],
  [L352], [*objectland 内の raw SVC を parent/handler が解釈する意味論は残す*。], [objectland 内の生のシステムコール・トラップを親ハンドラが解釈する意味論を維持する。],
  [L356], [所属 `svc_domain={native,objectland}` は trusted composition が生成時に固定し、object generation と一緒に protected descriptor へ公開する。], [システムコールドメインの命名を `trap_domain={native,objectland}` へ一般化。],
  [L368], [この groundwork は mode による V2 dispatch 切替えを実装しない。V2 public SVC の既存 parent routing をそのまま保つ。], [V2 public トラップの既存 parent routing を維持する。],
  [L369], [内部 `kernel_services` の特権固定 SVC site だけが root kernel 処理を直接指定し、raw objectland ID の意味は変更しない。], [内部特権固定トラップのみが root kernel 処理を指定する。],
  [L398], [publish pointer を release store し、SVC 入口は acquire load した一つの generation を使用する。], [トラップ入口は acquire load した一つの世代記述子を使用する。],
  [L421], [SVC vector の最小入口、publication 管理、最終 tail/exception-return gate は初期 V3 では交換しない常駐 nucleus に置く。], [トラップベクタの最小入口および最終復帰ゲートは交換不能な常駐核に配置する。],
  [L450], [  [Atomic svc], [1 ID + op。カーネルは CAS と load/store のみ H/S0、非特権可、ISR 可。FETCH_ADD/SWAP/キュー等は objectland (スレッドモード) で処理 (決定 6)。番号と引数 layout は `//abi` 正本と合わせて確定。既存案の r0=op をそのまま使うと r0=API ID と衝突するため、op は a1、operand は a2…または request に置く。],], [アトミックシステムコールを中立命名し、引数レジスタ配置を各 ISA の ABI 規約 (ARM: r0/r1, RV: a0/a1, x64: rdi/rsi) に一般化。],
  [L460], [カーネルを大きくしないため、*カーネル Handler mode で処理するのは CAS と load/store のみ*。], [カーネルを肥大化させないため、*カーネルの同期トラップ短経路で処理するのは CAS と load/store のみ* とする。],
  [L463], [RP2350/C6 のユーザーコードはハード atomic (LDREX/STREX、AMO) を直接使ってよい。この svc は主に RP2040 (LDREX 無し) や ISR 保護領域の整合のための口である。], [排他命令 (LDREX/STREX vs AMO vs native CAS) の利用方針と、ハード排他欠落コア (RP2040/RV32I) へのシステムコール提供理由の一般化。],
  [L474], [M33 の LDREX/STREX retry も無制限なら H 不可。短い専用 kernel 排他、有限試行と既存契約で表現できる失敗結果、または arch の直接 AMO を選ぶ。], [排他モニタ (LDREX/STREX, LR/SC) の再試行非有界性と同期短経路 (inline_synchronous_trap) 不適合性の制約、および代替設計の明記。],
  [L506], [shim を object 化せず、CALL_METHOD/SVC の dispatcher に blob の OS 依存を混ぜない。], [外部 OS shim をオブジェクト化せず、システムコール・トラップのディスパッチャに依存を持ち込まない。],
  [L529], [一方、dispatcher の権限検査は現在の Handler 状態ではなく、保存した origin が ISR であったかを使う。], [ディスパッチャの権限検査は現在のトラップ実行状態ではなく、呼出し元 origin の ISR フラグを用いる。],
  [L530], [Thread 由来 SVC の中では current trap_depth>0、origin.from_isr=false となる。], [通常スレッド由来の同期トラップ内では `current trap_depth > 0` かつ `origin.from_isr == false` となる。],
  [L532], [Arm は IPSR 等の現在の例外番号から判定し、全入口／出口を通る per-core nesting depth も diagnostic として公開する。], [ARM では IPSR、RISC-V では mcause およびソフトウェア管理のトラップネスト深度カウンタを用いて判定する。],
  [L533], [C6 は ISA に Handler mode がないため、trap vector の software depth を保存し、割込みを再許可する前に加算、帰還直前に復元する。], [RISC-V 等の単一モード実行環境では、トラップベクタ入口でソフトウェアネスト深度を加算・復元する。],
  [L541], [`#table(columns: (1fr, 1.5fr, 1.5fr, 1.7fr), inset: 4pt,`], [11 軸比較表への一般化対象表ヘッダ開始行。],
  [L542], [`table.header(repeat: true, [差分], [RP2350 / M33], [RP2040 / M0+], [C6 / RV32IMAC]),`], [比較対象列の一般化 (特定チップ名から ISA / プラットフォームへの抽象化)。],
  [L543], [[入口保存], [HW core frame＋必要なら FP。S0/S1/S2。], [HW core frame、FP/PSPLIM 命令なし。], [GPR 自動 stacking なし。ecall 用 caller-saved と CSR を software 保存...]], [各 ISA の自動/手動スタッキング差分記述の一般化 (同期/非同期フレーム分離)。],
  [L544], [[復帰], [Handler EXC_RETURN。Thread tail gate は別途証明。], [同じモード区別。Thumb-1 で完全復元 gate を個別設計。], [mepc/mstatus と MRET。ECALL は 32bit なので再開 PC は +4。]], [復帰トークン/復帰命令 (EXC_RETURN, MRET, SYSRET) のアーキテクチャ別記述。],
  [L545], [[保護], [MPU/PSPLIM。], [現行は software stack limit、MPU guard 未実装。], [PMP 等を能力／IDF 所有権と合わせて実装。]], [メモリ保護 (MPU/PMP) とスタック限界 (PSPLIM/SW) の直交化記述。],
  [L546], [[atomic], [exclusive 命令と排他上限。], [hardware spinlock と IRQ 規約の裏取り。], [A extension の AMO/CAS backend、ordering。]], [排他命令 (LDREX, SIO spinlock, Zaamo/Zalrsc/Zacas) の成立性・費用差記述。],
  [L547], [[計測], [既存 DWT CYCCNT。], [HAS_CYCLE_COUNTER=false。timer による多数反復または GPIO/外部計測。], [cycle CSR の実装／アクセス権／clock 条件を確定し新 backend。]], [サイクルカウンタ有無とタイマ代替手段の機能変数記述。],
  [L548], [`)` (比較表終端)], [比較表ブロックの終端。],
  [L553], [“Handler service” は C6 では trap 内完結を指す設計用語で、RISC-V に Arm の Handler mode が存在するとは書かない。], [「同期トラップ短経路」は RISC-V では同期トラップハンドラ内完結を意味する中立用語として定義。],
  [L554], [ABI はレジスタに API 番号と引数を渡す。SVC 即値に番号を埋めず、ECALL の即値なしに対応する。], [引数レジスタに API ID を渡し、トラップ命令即値に依存しない ABI 共通規約。],
  [L564], [[冗長な r4–r11 の全 context snapshot 一組を仮に除去], [8 語 store＋8 語 load と付随命令を 16–40 cyc/組...]], [呼出し元 callee-saved 整数レジスタ一式の冗長退避除去に関する費用評価。],
  [L565], [[context reuse の必須 callee-saved], [S0 に余分な full snapshot を作らないのが先。], [caller を suspend して別 actor を実行する以上、r4–r11 の caller state は保存が必要。]], [context reuse における callee-saved 整数レジスタの必須保存契約記述。],
  [L569], [ただし「r4–r11 を保存しないこと」と「r2/r3/r12/LR/cc clobber を緩めること」は別の変更である。], [callee-saved の保存省略と caller-saved/フラグ破壊規約の変更を混同しない原則。],
  [L571], [現行 cost3 の save 約131 / restore 約120 cyc は lookup／hook／system registers 等を含み、これを r4–r11 単体費用とはしない。], [コンテキスト退避・復元実測値におけるシステム制御オーバーヘッドの分離評価。],
  [L577], [`kernel_services` は yield/grant、時刻、sleep、cas、IPSR の ISR 判定、IRQ save/restore を提供する特権内部ヘッダである。], [`kernel_services` の提供機能定義 (IPSR は中立な `in_isr()` へ抽象化)。],
  [L578], [V2 scheduling だけは保存済み context が必要なので、固定 resume PC の内部 SVC gate から root kernel handler を指定する。], [内部同期トラップゲートから root kernel handler を指定する。],
  [L579], [通常の SVC の数値や current parent は変更しない。C6 の ISR hook は宣言だけで、この worktree に推測実装を置かない。], [公開システムコール数値および親バインディングを維持する。],
  [L584], [V2 では mode/from_isr による public SVC 配送変更を行わない。from_isr を runtime に効かせるのは内部 scheduling 関数の拒否のみである。], [公開システムコール配送の静的属性適用制限に関する記述。],
  [L586], [計測 config は `--config=v3gw`。SVC assembly 入口→context restore 後の marker の *software Handler span* と、... PRIMASK *envelope* ...], [システムコール・アセンブリ入口から復元後までの *ソフトウェア・トラップ実行時間* と、割込みマスク区間を記録する。],
  [L588], [Handler span は hardware stacking/unstacking と最後の marker 退避復元 tail を除く。hook の費用／上位 IRQ は含み...], [ソフトウェア・トラップ実行時間測定区間の定義。],
  [L589], [PRIMASK は mask 前の clock read→restore 後の clock read を囲み、RP2040 atomic は spinlock 呼出し全体を囲むため...], [大域割込みマスク区間および外部スピンロック測定包絡線の定義。],
  [L597], [実機 selftest に、実 handler の privileged/nonprivileged GRANT/KILL の拒否整合、nested PRIMASK restore...], [実機テストにおける特権検査およびネストした割込みマスク復元検証。],
  [L610], [RP2040/C6 は別の測定基準を取得してから数値目標を置き、M33 の cyc を移植しない。], [ターゲット環境ごとのサイクルカウンタ有無・分解能に応じた測定基準分離設計へ一般化。],
  [L635], [[2], [H GET と最小入口], [r4–r11/flags/volatile regs 保持、同 PSP/CONTROL、IRQ nesting、MSP alignment、保護不変。GET entry=1/push=0。]], [段階的移行における最小入口検証基準 (全 callee-saved 保持、特権/スタック不変)。],
  [L650], [objectland の後二つの raw SVC 解釈は維持して試験する。native の explicit method 化は別のテストで確認する。], [objectland の生トラップ解釈維持の検証。],
  [L653], [V3 に新しく必要な試験は、各 traits の privileged/unprivileged×task/ISR matrix、IRQ 中の T 拒否と無副作用... MSP 8B...], [新テスト要件 (特権/コンテキストマトリクス、特権スタック境界整列等)。],
  [L671], [2. 決定: 通常の native は explicit method へ。*objectland の SVC ハンドリングは raw svc をハンドラが受ける意味論を残す*。], [決定 2 における生システムコール・トラップ解釈の維持。],
  [L675], [- objectland 経路 (root 型の参考 5774/11642 cyc): 契約緩和は 0–20 cyc... 冗長 snapshot 5 組の除去が 80–200 cyc... caller の r4–r11 は reuse では保存が必須。], [objectland 呼出しにおける callee-saved 保存の定量的見積り。],
  [L678], [6. 決定: atomic は 1 ID + op。カーネル Handler mode は CAS と load/store のみ。FETCH_ADD・SWAP・キュー等は objectland (スレッドモード) で処理...], [決定 6 における同期短経路完結とスレッドモード処理の分離。],
  [L689], [- #link("https://github.com/ARM-software/abi-aa/blob/main/aapcs32/aapcs32.rst")[Arm AAPCS32] の §6.1.1 / §6.2.1 (callee-saved と public interface の SP 8B 整列)。], [アーキテクチャ呼出し規約におけるレジスタ保存およびスタック整列仕様。],
)

== 未知の事項と判断事項 (Unknowns and Design Decisions)

=== 未知の前提・未確認事項 (Unknowns)
+ *ESP-IDF の正確な revision と NPL ソース行*: 上層が利用している正確な ESP-IDF revision およびコミットハッシュは未確定であり、公開 `esp-nimble` 参照版との完全一致は未確認である。(unknown)
+ *RP2040 における単発サイクル計測手法*: RP2040 (Cortex-M0+) には DWT CYCCNT が存在しないため、1 µs 分解能タイマによる多数回反復測定以外の「単発呼び出し精度測定」の手法はハードウェア的に未確定である。(unknown)
+ *個別 SoC の詳細プラットフォーム仕様*: ESP32-C3, ESP32-H2, ESP32-P4, および Raspberry Pi Hazard3 については、公式 TRM / データシートの現物を直接開いて検証していないため、ハート数、独自割込みコントローラ構造、タイマ仕様等の個別プラットフォーム値は未確認 (unknown) として扱う。(unknown)
+ *ESP32-C6 の PMP ハードウェア挙動と IDF 共存*: ESP32-C6 において IDF の 2nd stage bootloader が初期化する PMP 領域と、Shizuku カーネルが動的に設定する PMP エントリ (0..15) の所有権分割ルールは確定していない。(unknown)
+ *Cadence Tensilica Xtensa 一次仕様*: Cadence 公式の Xtensa ISA Reference Manual は一般 Web から直接アクセス不能であるため、一次仕様レベルでの直接確認は未確認 (unknown) とする。Espressif の公開 TRM (ESP32 TRM v5.1, ESP32-S3 TRM v1.5) および ESP-IDF HAL ヘッダ (`xtensa/hal.h`) の範囲で事実を確認している。(unknown)
+ *`xthal_window_spill()` の内部実装挙動*: ESP-IDF の `xtensa/hal.h` に関数宣言が存在することは確認したが、内部アセンブリ実装の完全な実行挙動やコンテキスト制約は未確認 (unknown) として扱う。(unknown)

=== 上位アーキテクトによる判断事項 (Decisions Required)
+ *判断事項 1 (非特権作成時の特権昇格防止ルール)*: `CREATE_OBJECT` において、非特権 origin が特権 actor を生成することを一律拒否するか、あるいは特定の descriptor 署名・capability を要求するか。
+ *判断事項 2 (無期限メソッド呼出し中のホットスワップ・タイムアウト処理)*: 将来ホットスワップを再開する際、旧世代内で永久ブロックしているスレッドが存在した場合、タイムアウトで更新を延期するか、それとも旧スレッドを強制終了 (kill) して破棄するか。
+ *判断事項 3 (Atomic システムコールの ABI 順序指定)*: カーネル atomic システムコールにおいて、`seq_cst` を初期標準として固定するか、あるいは引数フラグとして acquire / release / relaxed の明示指定を許容するか。
+ *判断事項 4 (Xtensa ターゲットにおける ABI 選定)*: ESP32 / ESP32-S3 への展開時、ウィンドウ例外オーバーヘッドを排除する `Call0 ABI` を正式採用するか、あるいは標準の `Windowed ABI` を維持して `xthal_window_spill()` を許容するか。

== 公式一次資料一覧 (Primary Official Sources)

本文書で引用・検証したすべての公式一次資料の書誌情報および確認箇所を以下に示す。直接閲覧不能なものは「一次資料未確認 (unknown)」と明記する。

#table(columns: (1.5fr, 1.2fr, 1.8fr, 1.1fr, 1.4fr), inset: 3.5pt,
  table.header(repeat: true, [資料名・文書番号], [発行元・版], [検証した主要な章・節], [確認日], [公式URL / 所在 / 状態]),
  [ARMv6-M Architecture Reference Manual (ARM DDI 0419C)], [Arm Ltd., Issue C (2018)], [§A4.1 (Instruction Set), §B1.4 (Privilege), §B1.5 (Exception Model, PRIMASK)], [2026-10-07], [#link("https://developer.arm.com/documentation/ddi0419/latest")[Arm Developer] (confirmed)],
  [Cortex-M0+ Technical Reference Manual (ARM DDI 0484C)], [Arm Ltd., Issue C (2014)], [§1.3 (Features, Optional PMSAv6 MPU)], [2026-10-07], [#link("https://developer.arm.com/documentation/ddi0484/latest")[Arm Developer] (confirmed)],
  [Raspberry Pi RP2040 Datasheet], [Raspberry Pi Ltd., v2.2 (2024)], [§1.2 (System Architecture), §2.3.1.7 (SIO Spinlocks, 32 locks)], [2026-10-07], [#link("https://datasheets.raspberrypi.com/rp2040/rp2040-datasheet.pdf")[Raspberry Pi Docs] (confirmed)],
  [ARMv7-M Architecture Reference Manual (ARM DDI 0403E)], [Arm Ltd., Issue E.e (2021)], [§A4.1 (Sync Primitives, LDREX/STREX), §B1.5, §B3.5 (PMSAv7 MPU), §C1.8 (DWT)], [2026-10-07], [#link("https://developer.arm.com/documentation/ddi0403/latest")[Arm Developer] (confirmed)],
  [ARMv8-M Architecture Reference Manual (ARM DDI 0553C)], [Arm Ltd., Issue C.a (2023)], [§B1.5.15, §B3.19 (MSPLIM/PSPLIM), §B4.22 (FP), §B4.25 (PMSAv8)], [2026-10-07], [#link("https://developer.arm.com/documentation/ddi0553/latest")[Arm Developer] (confirmed)],
  [Cortex-M33 Technical Reference Manual (Arm 100235_0100_06)], [Arm Ltd., Issue 06 (2020)], [§3.3.1 (Stack Limits), §4.5 (MPU opt), §4.6 (FPU opt)], [2026-10-07], [#link("https://developer.arm.com/documentation/100235/latest")[Arm Developer] (confirmed)],
  [Raspberry Pi RP2350 Datasheet], [Raspberry Pi Ltd., v1.0 (2024)], [§1.2 (Dual M33 / Hazard3), §2.3 (M33 with TZ, MPU, FPU, PSPLIM)], [2026-10-07], [#link("https://datasheets.raspberrypi.com/rp2350/rp2350-datasheet.pdf")[Raspberry Pi Docs] (confirmed)],
  [Arm AAPCS32 (Procedure Call Standard for Arm Architecture)], [Arm Software, r2023Q4], [§6.1.1 (Registers), §6.2.1 (Stack 8-byte Alignment)], [2026-10-07], [#link("https://github.com/ARM-software/abi-aa/blob/main/aapcs32/aapcs32.rst")[GitHub: abi-aa] (confirmed)],
  [RISC-V Unprivileged ISA Specification (Doc Ver 20240411)], [RISC-V International], [Chap 13 ("A", §13.2 Zalrsc, §13.3 Eventual Success, §13.4 Zaamo), Chap 15 ("Zacas"), Chap 17 ("Zalasr")], [2026-10-07], [#link("https://github.com/riscv/riscv-isa-manual/releases/download/20240411/riscv-spec-20240411.pdf")[riscv-spec-20240411.pdf] (confirmed)],
  [RISC-V Privileged Architecture (v1.10 / v1.13)], [RISC-V International], [Chap 2 (CSRs), Chap 3 (M-mode Traps, mepc, mstatus, PMP)], [2026-10-07], [#link("https://docs.riscv.org/reference/isa/")[docs.riscv.org] (confirmed)],
  [ESP32-C6 Technical Reference Manual (TRM)], [Espressif Systems, v1.1], [Chap 2 (RISC-V Core), Chap 8 (Interrupt Matrix), Chap 9 (PMP)], [2026-10-07], [#link("https://www.espressif.com/sites/default/files/documentation/esp32-c6_technical_reference_manual_en.pdf")[Espressif C6 TRM] (confirmed)],
  [ESP32 Technical Reference Manual (TRM)], [Espressif Systems, v5.1], [Chap 1 (Dual LX6), Chap 3 (DMA), Chap 24 (Interrupt Matrix)], [2026-10-07], [#link("https://www.espressif.com/sites/default/files/documentation/esp32_technical_reference_manual_en.pdf")[Espressif ESP32 TRM] (confirmed)],
  [ESP32-S3 Technical Reference Manual (TRM)], [Espressif Systems, v1.5], [Chap 1 (Dual LX7 Architecture)], [2026-10-07], [#link("https://www.espressif.com/sites/default/files/documentation/esp32-s3_technical_reference_manual_en.pdf")[Espressif S3 TRM] (confirmed)],
  [ESP-IDF Tensilica HAL Header (xtensa/hal.h)], [Espressif Systems / Tensilica], [xthal_window_spill 宣言, S32C1I 条件マクロ], [2026-10-07], [#link("https://github.com/espressif/esp-idf/blob/master/components/xtensa/include/xtensa/hal.h")[GitHub: esp-idf] (confirmed: 宣言のみ)],
  [Xtensa Instruction Set Architecture (ISA) Ref Manual], [Cadence Tensilica], [Web直接アクセス不能のため未確認], [2026-10-07], [*一次資料未確認 (unknown)*],
  [Intel 64 and IA-32 Architectures SDM (Combined Vol 1-4)], [Intel Corporation, Order 325462-085US], [Vol 1 Chap 13 (XSAVE), Vol 2A Chap 3 (CPUID), Vol 2B Chap 4 (SYSCALL, RDTSC), Vol 3A Chap 2/6 (CR4, IDT)], [2026-10-07], [#link("https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html")[Intel SDM] (confirmed)],
  [AMD64 Architecture Programmer's Manual (APM Vol 3)], [Advanced Micro Devices, Rev 3.36], [Chap 3 (System Instructions: SYSCALL, SYSRET, SWAPGS)], [2026-10-07], [#link("https://www.amd.com/system/files/TechDocs/24594.pdf")[AMD APM] (confirmed)],
  [Unified Extensible Firmware Interface (UEFI) Spec v2.10], [UEFI Forum, Aug 2022], [Chap 7 §7.4.6 (Boot Services: ExitBootServices), Chap 8 (Runtime Services)], [2026-10-07], [#link("https://uefi.org/sites/default/files/resources/UEFI_Spec_2_10_Aug29.pdf")[UEFI Spec 2.10 PDF] (confirmed)],
)
