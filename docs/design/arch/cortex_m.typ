#set document(title: "Shizuku V3 アーキテクチャ検討資料: Cortex-M ファミリー", author: "Shizuku Architecture Team")
#set page(paper: "a4", margin: (x: 15mm, y: 15mm), numbering: "1")
#set text(font: ("Hiragino Sans", "Arial"), size: 8pt, lang: "ja")
#set par(leading: 0.55em, justify: true)
#set heading(numbering: "1.1")
#show raw: set text(font: "Menlo", size: 6.8pt)

= Cortex-M ファミリー アーキテクチャ検討 (Shizuku V3)

状態: *ISA検討資料 Cortex-M family 独立章骨格*。2026-10-07。
対象リポジトリ: `Shizuku-stream` (Git HEAD: `3239d3761fd1898b78411434a31f91790251bd3b`)。
参照元設計書: `docs/design/kernel_v3_arch.typ`。

== 対象と機能構成
- *骨子と既決定*:
  - 多ISA構造の一環として現在実装対象外（実装はXtensa/RISC-Vのみ）だが構造は維持。Cortex-M (v6-M/v7-M/v8-M) を対象とし、chip固定分岐を排してハードウェア機能有無（機能変数）で整理・静的選択。
  - 代表例示: ARMv6-M (Cortex-M0+, RP2040), ARMv7-M (Cortex-M3/M4/M7), ARMv8-M Mainline (Cortex-M33, RP2350)。
  - 主要機能変数: `HAS_PRIVILEGE_SEPARATION` (CONTROL.nPRIV), `HAS_MEMORY_PROTECTION` (PMSAv6/v7/v8 MPU opt, RP2040無), `HAS_GPR_HARDWARE_STACKING` (true: r0–r3, r12, LR, PC, xPSR), `HAS_DEFERRED_TRAP` (PendSV), `HAS_HARDWARE_STACK_LIMIT` (PSPLIMはISA能力: v8-M Mainline標準, v6-M/v7-M欠落), `HAS_NATIVE_ATOMIC_CAS` (欠落), `HAS_EXCLUSIVE_MONITOR` (v7-M/v8-M LDREX/STREX, v6-M欠落), `HAS_CYCLE_COUNTER` (DWT CYCCNT opt, v6-M欠落), `HAS_EXTENDED_REGISTERS` (FPU opt)。
- *由来 / 根拠*: 確定方針 2, 確定方針 4; `docs/design/kernel_v3_arch.typ`: L20–22, L40–52, L184–203, L248–260, L264–289。
- *未決*: カウンタやMPU等のSoC実装オプションは静的選択。

== ISA/ABI/SoC境界
- *骨子と既決定*:
  - 3階層分離: ISA層 (`SVC`/`EXC_RETURN`/`CONTROL`/`PRIMASK`; kernel方針を知らない), ABI層 (AAPCS32規約, `r0`=API ID, `r1`=op, callee-saved `r4–r11` 保存, SP 8B整列), SoC層 (メモリマップ, NVIC, SysTick, SIOスピンロック等)。
  - 公開APIでdriverをuser/kernel両方で作成可能とし、kernel側IF差は peripheral object が吸収。
  - arch実装は family $arrow$ chip 継承、`using`、または薄ラッパーが有力。
- *由来 / 根拠*: 確定方針 3, 確定方針 4; `docs/design/kernel_v3_arch.typ`: L41–48, L123, L205–239, L281, L554, L689。
- *未決*: 具体的な peripheral object の kernel 結合形態。

== 保護/変換能力
- *骨子と既決定*:
  - 保護driver (MPU: PMSAv7/PMSAv8) を核から分離し、能力 (`constexpr granularity`, `alignment_rule`, `region_count`, `can_split_stack`, `translates`) を公開して kernel が静的選択。PSPLIM は ISA 能力として併用。
  - memory分割: address resource (CPU非依存; 物理マップ/DMA/MMIO) と protection domain (CPU/保護driver依存) の一方向依存。frames は domain 側が resource 側から確保し driver へ渡し循環不可。
  - MMU (仮想記憶変換) は Cortex-M では対象外 (`translates=false`)。保護 (protection) と変換 (translation) は別能力。
- *由来 / 根拠*: 確定方針 4, 確定方針 6, 確定方針 7; `docs/design/kernel_v3_arch.typ`: L50, L185, L192, L251, L253, L277, L287。
- *未決*: MPU 非搭載 (v6-M / RP2040) におけるソフトウェア境界検査の具体構成。

== context保存/stack分割
- *骨子と既決定*:
  - HW自動退避 (`HAS_GPR_HARDWARE_STACKING=true`): 入口で r0–r3, r12, LR, PC, xPSR を自動スタック。S0 は元フレーム戻り値更新のみで Handler EXC_RETURN 直接復帰。S1 は全 callee-saved (`r4–r11`) をスレッド継続へ保存。FPU 有効時は `FPCCR.LSPACT` 遅延退避を考慮 (S2)。
  - context reuse 規約: 同一スレッド再利用呼出しは専用 EXC_RETURN 復元ゲートで戻す（普通の C return 誤用禁止）。
  - stack分割: 保護粒度で丸めた境目でスタック分割し caller 上側をアクセス不可として保護 (入れ子でも region 数不変)。丸め余白は `call_frame_push` 予約。分割不可 (v6-M MPUなし/RP2040等) は能力 (`can_split_stack=false`) に基づき copy (設計方針として記録、hardware検証済扱い禁止)。
- *由来 / 根拠*: 確定方針 4, 確定方針 5; `docs/design/kernel_v3_arch.typ`: L47, L72–75, L100–104, L110–118, L186–190, L196–218, L264, L274–276, L288。
- *未決*: callee に見せる context 配置の幾何構造。

== atomic/IRQ/counter
- *骨子と既決定*:
  - atomic: 単一命令 CAS 欠落 (`HAS_NATIVE_ATOMIC_CAS=false`)。v7-M/v8-M は排他モニタ (`LDREX`/`STREX`) による CAS 再試行ループで構成可能だが、再試行非有界リスクのため同期短経路 CAS には上限管理または代替が必要。v6-M (RP2040) はハード排他命令が欠落し、SoC SIO スピンロックと `PRIMASK` によるクリティカルセクションへ代替 (SoC 固有解決策)。
  - IRQ / 文脈識別: `in_isr()` は `IPSR != 0` のみ判定。ディスパッチャ権限判定はトラップ入口の `origin.from_isr` を使用。`PRIMASK` は可変優先度のみ遮蔽 (NMI/HardFault 非遮蔽)。
  - IRQ 予算 (決定 1) は暫定保守的税金として扱う。RP2350 v3gw SVC max 11,728 cyc、PRIMASK envelope max 185,847 cyc (フック込み) を記録し遅延調査保留。
  - counter: v7-M/v8-M は `DWT CYCCNT` (32bit, `DEMCR.TRCENA` 要) opt。v6-M は欠落しタイマ多数回平均測定で代替。
- *由来 / 根拠*: 確定方針 8; `docs/design/kernel_v3_arch.typ`: L51, L129–132, L136–140, L193–195, L198–200, L254–256, L258, L270–273, L278, L287, L351, L356–362, L586–589。
- *未決*: RP2040 における単発サイクル計測手法 (未確定/unknown)。

== 既存決定成立条件
- *骨子と既決定*:
  - 決定 1–11 の成立性整理:
    - 決定 1 (IRQ 予算): DWT 有効時のみ低費用成立、v6-M タイマ代替 (暫定保守的税金扱い、遅延調査保留、RP2350 実測値記録)。
    - 決定 2 (native explicit method / raw トラップ解釈維持): 成立 (`r0` レジスタ)。
    - 決定 3 (context reuse 専用戻り経路): 専用 EXC_RETURN 復元ゲートで成立。
    - 決定 4 (保存契約維持): HW 自動退避活用 (S0: frame のみ, S1: `r4–r11` 保存) で成立。
    - 決定 5 (動的ホットスワップ棚上げ、actor ID 定数化禁止): 成立。
    - 決定 6 (atomic 1 ID + op, 短経路 CAS/load/store): v7/v8 LDREX/STREX 条件付き成立、v6-M SIO スピンロック代替成立。
    - 決定 7 (`//abi` 正本同期): 成立 (スコープ限定 IF 契約)。
    - 決定 8 (宛先指定 YIELD 廃止、GRANT 貸付けと無引数 YIELD): 成立・決定 8 閉鎖。実行権 GRANT のみ (入れ子可、内予算 clamp)、YIELD 無引数親返却・手数料なし、祖先 GRANT 返却扱いで記録積まない。Mutex は GRANT(owner) $arrow$ CAS 再試行。現行コード (`internal_headers/shizuku/kernel_services.hpp:9`, `source/kernel/services.cpp:34`) は無引数 `result yield();` であり確定方針と完全に一致。
    - 決定 9 (wait/wake キュー不採用、mutex は CAS+GRANT): 成立。
    - 決定 10 (ESP-IDF shim): Cortex-M では適用外。
    - 決定 11 (小さい `kernel_services` 契約): port 層の ABI バックエンド責務として成立。
  - 核モデル規則: kernel と kernel object を呼び分けない。API ID 静的属性で経路決定、V3 外向き口は V2 先行維持。
- *由来 / 根拠*: 確定方針 1, 確定方針 8; `docs/design/kernel_v3_arch.typ`: L79–96, L163–166, L350–369; 実コード: `internal_headers/shizuku/kernel_services.hpp:9`, `source/kernel/services.cpp:34`。
- *入力設計書との相違*: `kernel_v3_arch.typ` L164–165, L365 では現行コードに `yield(0)` の暫定引数が残存していると記述されているが、実コードは既に無引数 `result yield();` に更新されており、確定方針に一致している（入力設計書の記述が実コードと乖離）。

== 未確認・後続
- *未確認事項 (Unknowns)*:
  - RP2040 における単発サイクル計測手法 (DWT CYCCNT 欠落のためハードウェア的に未確定、由来: `docs/design/kernel_v3_arch.typ`: L175, L273)。
  - Cortex-M v6-M MPU なし / RP2040 における stack 分割不可・能力による copy 代替は、本日の設計方針として記録するが hardware 検証済扱い禁止 (未検証、由来: 確定方針 5)。
  - callee に見せる context 配置の幾何構造 (未決、由来: 確定方針 5)。
- *後続検討課題*:
  - Cortex-M は現在実装対象外 (現在実装は Xtensa/RISC-V のみ) のため、実機検証・port 実装は後続フェーズ。
  - 保護 driver の `constexpr` パラメータ具体値策定 (MPU 領域数・粒度)。
  - RP2350 SVC / PRIMASK 実測遅延の要因分析 (遅延調査保留の解除)。
- *由来 / 根拠*: 確定方針 2, 確定方針 4, 確定方針 5, 確定方針 8; `docs/design/kernel_v3_arch.typ`: L175, L273, L586–589。
