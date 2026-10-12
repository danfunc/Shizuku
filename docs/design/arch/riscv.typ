#set document(title: "Shizuku V3 アーキテクチャ検討資料: RISC-V ファミリー", author: "Shizuku Architecture Team")
#set page(paper: "a4", margin: (x: 15mm, y: 15mm), numbering: "1")
#set text(font: ("Hiragino Sans", "Arial"), size: 8pt, lang: "ja")
#set par(leading: 0.55em, justify: true)
#set heading(numbering: "1.1")
#show raw: set text(font: "Menlo", size: 6.8pt)

= RISC-V ファミリー アーキテクチャ検討 (Shizuku V3)

状態: *ISA検討資料 RISC-V family 独立章骨格 (第2稿・レビュー指摘反映版)*。2026-10-07。
対象リポジトリ: `Shizuku-stream` (Git HEAD: `3239d3761fd1898b78411434a31f91790251bd3b`)。
参照元設計書: `docs/design/kernel_v3_arch.typ`。

== 対象と機能構成
- *骨子と既決定*:
  - 多ISA構造の一環として実装済みISAである RISC-V (RV32) を対象とし、チップ固有分岐を排してハードウェア機能有無（機能変数）で能力を直交化する。
  - ESP32-C3 / C6 / H2 / P4 および Raspberry Pi Hazard3 は対象例示であり、個別SoC仕様の公式検証済みと混同しない。
  - 主要機能変数: `HAS_PRIVILEGE_SEPARATION` (M/U-mode opt), `HAS_MEMORY_PROTECTION` (PMP opt), `HAS_GPR_HARDWARE_STACKING` (欠落/false), `HAS_DEFERRED_TRAP` (MSIP opt), `HAS_HARDWARE_STACK_LIMIT` (欠落/false), `HAS_NATIVE_ATOMIC_CAS` (Zacas opt), `HAS_NATIVE_ATOMIC_RMW` (Zaamo opt), `HAS_EXCLUSIVE_MONITOR` (Zalrsc opt), `HAS_CYCLE_COUNTER` (rdcycle/mcycle opt)。
- *由来 / 根拠*: 確定方針 2; `docs/design/kernel_v3_arch.typ`: L20–22, L171–203, L248–260, L290–309。
- *未決*: カウンタや排他等のオプション拡張実装有無はターゲット依存の静的選択。

== ISA/ABI/SoC境界
- *骨子と既決定*:
  - 3階層分離: ISA層 (`ecall`/`mret`/CSR; kernel方針を知らない), ABI層 (ilp32規約, `a0`=API ID, `a1`=op, callee-saved `s0–s11` 保存, 16B整列), SoC層 (メモリマップ, INTC, タイマ, IPI)。
  - 公開APIでdriverをuser/kernel両方で作成可能とし、kernel側IF差は peripheral object が吸収する。
  - arch実装は family $arrow$ chip 継承、`using`、または薄いラッパーが有力。
- *由来 / 根拠*: 確定方針 3, 確定方針 4; `docs/design/kernel_v3_arch.typ`: L43, L48, L123, L205–239, L431, L554。
- *未決*: 具体的な peripheral object の kernel 結合形態。

== 保護/変換能力
- *骨子と既決定*:
  - 保護driver (PMP/PMS/MMU) を核から分離し、能力 (`constexpr granularity`, `alignment_rule`, `region_count`, `can_split_stack`, `translates`) を公開して kernel が静的選択。
  - `HAS_HARDWARE_STACK_LIMIT=false` のため PMP No-Access ガード領域または SW 検査で代替。
  - memory分割: address resource (CPU非依存) と protection domain (CPU/保護driver依存) の一方向依存。frames は domain 側が resource 側から確保し driver へ渡す（循環不可）。
  - MMU は Xtensa/RV 実装後、hybrid (空間分離と共有混在) 本命、VA 全体一意、user 開発 object は生成時属性変更のみで kernel 移行可。保護 (protection) と変換 (translation) は別能力。
- *由来 / 根拠*: 確定方針 4, 確定方針 6, 確定方針 7; `docs/design/kernel_v3_arch.typ`: L50, L185, L192, L251, L253, L308, L545。
- *未決*: 実機 PMP 領域の具体的分割設計、MMU hybrid の詳細配置。

== context保存/stack分割
- *骨子と既決定*:
  - HW自動退避なし (`HAS_GPR_HARDWARE_STACKING=false`)。トラップ入口で分離: S0 (caller-saved のみ最小退避), S1 (全 callee-saved をスレッド継続へ保存), 非同期割込み (full 退避)。`ecall` 復帰時は `mepc += 4`。
  - context reuse で入った呼出しは専用 MRET アセンブリ復元ゲートで戻す（普通の C return 禁止）。
  - stack分割: 保護粒度で丸めた境目でスタック分割し caller 上側をアクセス不可とすることで保護（入れ子でも region 数増加なし）。丸め余白は `call_frame_push` 予約。分割不可は能力で copy。
- *由来 / 根拠*: 確定方針 5; `docs/design/kernel_v3_arch.typ`: L72–75, L100–104, L110–118, L186–190, L249, L257, L310, L353, L354。
- *未決*: callee に見せる context 配置の幾何構造。

== atomic/IRQ/counter
- *骨子と既決定*:
  - atomic: A拡張は `Zaamo` (AMO) と `Zalrsc` (LR/SC) に細分化され、`Zacas` (単一命令CAS: `amocas.w`) は `Zaamo` に依存。`Zaamo` 単独は CAS 不成立。`Zalrsc` (lr/sc) は eventual success (ライブロック防止) のみ保証され有界実行時間を保証しないため、厳格な時間予算が課される ISR や同期短経路 CAS には単独不適格となる場合がある。単一ハート `mstatus.MIE=0` や SoC プラットフォーム排他機構を代替とするが、`mstatus.MIE=0` は同ハートのみ有効で SMP/DMA/NMI には無効。
  - IRQ / 文脈識別: `in_isr()` は割込み実行中判定のみ (`mcause` 割込みbit等)。ディスパッチャ権限判定はトラップ入口の `origin.from_isr` を使用。IRQ 予算 (決定 1) は暫定保守的「税金」として扱い遅延調査保留。
  - counter: `HAS_CYCLE_COUNTER` は `rdcycle`/`mcycle` (64bit) 実装および U-mode アクセス権 (`mcounteren.CY`) に依存。未実装時は高分解能タイマ代替。
- *由来 / 根拠*: 確定方針 8; `docs/design/kernel_v3_arch.typ`: L51, L119–132, L136–140, L193–195, L200, L254–256, L258, L295–306, L351, L356–362, L483, L488–489。
- *未決*: ターゲットごとの A/Zacas 拡張有無、SoC 固有タイマの単発計測精度。

== 既存決定成立条件
- *骨子と既決定*:
  - 決定 1–11 の成立性整理:
    - 決定 1 (IRQ 予算): `rdcycle` 実装かつ `mcounteren.CY` 有効時のみ低費用成立、未実装時はタイマ代替 (暫定税金扱い、遅延調査保留)。
    - 決定 2 (native explicit method / raw トラップ解釈維持): 成立 (`a0` レジスタ)。
    - 決定 3 (context reuse 専用戻り経路): 専用 MRET 復元ゲートで成立。
    - 決定 4 (保存契約維持): 同期 (caller-saved) / 非同期 (full) 分離で成立。
    - 決定 5 (動的ホットスワップ棚上げ、actor ID 定数化禁止): 成立。
    - 決定 6 (atomic 1 ID + op, 短経路 CAS/load/store): Zacas で成立、Zalrsc は条件付き、Zaamo のみ不成立、RV32I は SoC/割込みマスク代替。
    - 決定 7 (`//abi` 正本同期): 成立 (スコープ限定 IF 契約)。
    - 決定 8 (宛先指定 YIELD 廃止、GRANT 貸付けと無引数 YIELD): 成立・決定 8 閉鎖。実行権 GRANT のみ (入れ子可、内予算 clamp)、YIELD 無引数で親返却・手数料なし、祖先 GRANT 返却扱いで記録積まない。Mutex は GRANT(owner) $arrow$ CAS 再試行。現行コード (`internal_headers/shizuku/kernel_services.hpp:9`, `source/kernel/services.cpp:34`) は無引数 `result yield();` であり確定方針と完全に一致。
    - 決定 9 (wait/wake キュー不採用、mutex は CAS+GRANT): 成立。
    - 決定 10 (ESP-IDF shim は関数として閉じ object 化しない): ESP32-C6 等で `kernel_services` 直呼び出し成立。
    - 決定 11 (小さい `kernel_services` 契約): port 層の ABI バックエンド責務として成立。
  - 核モデル規則: kernel と kernel object を呼び分けない。API ID 静的属性で経路決定、V3 外向き口は V2 先行維持。
- *由来 / 根拠*: 確定方針 1, 確定方針 8; `docs/design/kernel_v3_arch.typ`: L79–96, L163–166, L350–369; 実コード: `internal_headers/shizuku/kernel_services.hpp:9`, `source/kernel/services.cpp:34`。
- *入力設計書との相違*: `kernel_v3_arch.typ` L164–165, L365 では現行コードに `yield(0)` の暫定引数が残存していると記述されているが、実コードは既に無引数 `result yield();` に更新されており、確定方針に一致している（入力設計書の記述が実コードと乖離）。

== 未確認・後続
- *未確認事項 (Unknowns)*:
  - ESP32-C3 / H2 / P4 および Raspberry Pi Hazard3 の個別 SoC プラットフォーム仕様 (ハート数, INTC 構造, タイマ) は一次資料 TRM 現物未検証のため `unknown`。
  - ESP32-C6 における IDF 2nd stage bootloader PMP と Shizuku 動的 PMP (0..15) の所有権分割ルールは `unknown`。
  - ESP-IDF の正確な revision および NPL ソース行は `unknown`。
  - `shizuku_c6_in_isr()` の実装状態: `internal_headers/shizuku/kernel_services.hpp:19` に外部宣言 `extern "C" bool shizuku_c6_in_isr();` が確認されるが、定義の所在および実装状態は `source/`, `internal_headers/`, `modules/` の探索範囲内では見当たらず `unknown`。また `source/kernel/services.cpp:6` の `#if defined(__arm__)` 配下に `:11` の `#if defined(__riscv)` がネストしているため、RISC-V コンパイル環境での有効性は未確認。
  - callee に見せる context 配置は未決。
- *後続検討課題*:
  - 保護 driver の `constexpr` パラメータ具体値策定と実機 PMP 領域割り当て。
  - RISC-V 向け専用 MRET 復元ゲートと caller-saved 退避アセンブリの実装。
  - MMU hybrid モデルの詳細設計。
- *由来 / 根拠*: 確定方針 2, 確定方針 4, 確定方針 5; `docs/design/kernel_v3_arch.typ`: L163–166, L308–309, L506, L525, L527, L528; 実装観測: `internal_headers/shizuku/kernel_services.hpp:18–19`, `source/kernel/services.cpp:6–17`。
