#set document(title: "Shizuku V3 アーキテクチャ補完設計書 - Xtensa", author: "Shizuku Architecture Team")
#set page(paper: "a4", margin: (x: 15mm, y: 15mm), numbering: "1")
#set text(font: ("Hiragino Sans", "Arial"), size: 8pt, lang: "ja")
#set par(leading: 0.55em, justify: true)
#set heading(numbering: "1.1")

= Xtensa アーキテクチャ補完設計

`docs/design/kernel_v3_arch.typ` に基づく Xtensa コア向けの骨格設計書。本文化を避け要旨・由来・未確認事項に限定する。

== 対象と機能構成
- *要旨*: Cadence / Tensilica Xtensa コアファミリー全般を対象とし、ESP32 / S2 / S3 は具象 SoC 例示に留め機能有無の直交変数で静的選択する。保護ドライバは能力値タプル (`granularity`, `alignment_rule`, `region_count`, `can_split_stack`, `translates`) を開示。
- *confirmed*: 特権 (User/Privileged, `docs/design/kernel_v3_arch.typ:252`)、HW 自動退避欠落 (`:249`)、CCOUNT 32bit (`:258`)、ESP32/S3 TRM 一次資料 (`:312-316, 555-556`)、機能変数整理・継承/エイリアス設計 (今日方針2)。
- *unknown*: Cadence 公式 ISA Ref Manual 現物 (`docs/design/kernel_v3_arch.typ:314, 529, 558`)、個別 SoC 詳細機能値 (`:309, 527`, 今日方針2)。

== ISA/ABI/SoC境界
- *要旨*: 3 階層境界を分離。ISA 層はトラップ (`syscall`)・PS レジスタ・不可分命令を担当し、カーネル方針 (GRANT/YIELD) を知らず関知しない。ABI 層は呼出し規約・`a2` 生 ID 配送・スタック整列規約を担当。SoC 層はメモリマップ・割込みマトリクスを担当し、公開 API と peripheral object が差を吸収。
- *confirmed*: 3 階層分離原則 (`docs/design/kernel_v3_arch.typ:205-239`)、`syscall`・`a2` 配送 (`:43, 249, 352`)、ISA 層のカーネル方針非関知 (今日方針2)、公開 API/peripheral object 境界 (今日方針3)。
- *unknown*: 正式採用 ABI 未決 (Call0 vs Windowed, `docs/design/kernel_v3_arch.typ:317-320, 536`)。

== 保護/変換能力
- *要旨*: 保護 (protection) と変換 (translation) を直交能力として分離 (protection≠translation)。address resource と protection domain を一方向依存 (循環禁止) とし、MMU はハイブリッド方式・VA 全体一意を想定した物理保護完了後の後工程に配置。
- *confirmed*: MPU/MMU オプション存在 (`docs/design/kernel_v3_arch.typ:253`)、保護/変換分離 (今日方針6)、保護ドライバ能力値タプル宣言 (今日方針2)、resource/domain 一方向依存 (今日方針5)。
- *unknown*: 具象 SoC の MMU/PMS 実装値 (`docs/design/kernel_v3_arch.typ:309, 527`, 今日方針6)、MMU 詳細データ構造 (今日方針6)。

== context保存/stack分割
- *要旨*: context reuse は専用復元ゲート経由とし C return を禁止。保護境界で stack 分割し caller 上側を遮断、region 数不変・予約余白を保ち、分割不能環境は copy で代替。HW 自動退避欠落のため S0 (caller-saved 最小) と S1 (割込み full) を分離。Windowed は spill 依存、Call0 は callee-saved 退避。
- *confirmed*: context reuse 契約 (`docs/design/kernel_v3_arch.typ:353`)、S0 最小/割込み完全退避分離 (`:249, 257`)、stack 分割・予約・copy 代替 (今日方針4)、`xthal_window_spill` 宣言 (`:319, 557`)。
- *unknown*: `xthal_window_spill` 内部挙動 (`docs/design/kernel_v3_arch.typ:319, 530`)、callee 文脈の物理配置 (今日方針4)。

== atomic/IRQ/counter
- *要旨*: `S32C1I` 実装時のみネイティブ CAS 成立、未実装時は割込みレベル引き上げ (`PS.INTLEVEL`) 等へ劣化。同期短経路は 32bit CAS/load/store に限定。割込みレベルマスクは `PS.INTLEVEL`、ISR 判定は `PS.EXCM`。時間計測は `CCOUNT` (32bit)。RP2350 実測値は ARMv8-M 固有の非適用例であり、Xtensa 遅延調査は保留。
- *confirmed*: `S32C1I` 条件性 (`docs/design/kernel_v3_arch.typ:256, 321, 360`)、`PS.INTLEVEL`/`PS.EXCM` (`:254-255`)、`CCOUNT` サイクル計測 (`:258`)、RP2350 値の非適用例・Xtensa 調査保留 (今日方針7)。
- *unknown*: 具象 SoC での `S32C1I` 有無・最悪実行時間 bound (`docs/design/kernel_v3_arch.typ:321, 360`)、Xtensa 実機サイクル値 (今日方針7)。

== 既存決定成立条件
- *要旨*: 元文書の決定 1–11 に対する Xtensa での成立条件。
  - スケジューラ・同期 (決定 8, 9: GRANT/無引数 YIELD/CAS+GRANT mutex): 普遍成立 (今日方針1、`docs/design/kernel_v3_arch.typ:365-366`)。
  - トラップ・契約・通信 (決定 2, 3, 4, 5, 7, 11: `a2` 生 ID、専用復帰、S0/S1 分離、世代記述子、`//abi` 同期、共通ヘッダ): 成立 (今日方針8、`docs/design/kernel_v3_arch.typ:352-355, 364, 368`)。
  - ハードウェア依存 (決定 1: IRQ 予算、決定 6: atomic、決定 10: shim): 条件付き成立 (`CCOUNT` / `S32C1I` 依存、RP2350 値は非適用例・Xtensa 保留、ESP32 shim 直接呼出し、今日方針7, 8、`docs/design/kernel_v3_arch.typ:258, 351, 356, 360, 367`)。
- *confirmed*: 全決定の論理モデル成立性と条件性 (`docs/design/kernel_v3_arch.typ:349-369`、今日方針1, 7, 8)。
- *unknown*: 既存コードの暫定引数 `yield(0)` クリーンアップ時期 (`docs/design/kernel_v3_arch.typ:365`)。

== 未確認・後続
- *要旨*: 確定事実と未確認・後続課題を分離し、推測による仕様確定を排除。
- *confirmed*: Espressif TRM 一次資料 (`docs/design/kernel_v3_arch.typ:315, 555-556`)、`hal.h` 宣言 (`:316, 557`)、機能変数静的選択 (今日方針2)、保護≠変換と MMU 後工程 (今日方針6)。
- *unknown / 後続課題*: 1. Cadence 公式 ISA Ref Manual 現物 (`docs/design/kernel_v3_arch.typ:314, 529, 558`)、2. `xthal_window_spill` 内部挙動 (`:319, 530`)、3. ABI 選定 (Call0 vs Windowed, `:320, 536`)、4. 具象 SoC 能力値裏取り (`:309, 527`)、5. callee context 物理配置 (今日方針4)、6. MMU 詳細設計 (今日方針6)、7. Xtensa 実機実測値 (今日方針7)、8. ESP-IDF revision / NPL 一致確認 (`:525`)。
