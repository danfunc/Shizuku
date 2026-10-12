#set document(title: "Shizuku V3 アーキテクチャ検討資料: x86-64 / UEFI", author: "Shizuku Architecture Team")
#set page(paper: "a4", margin: (x: 15mm, y: 15mm), numbering: "1")
#set text(font: ("Hiragino Sans", "Arial"), size: 8pt, lang: "ja")
#set par(leading: 0.55em, justify: true)
#show raw: set text(font: "Menlo", size: 6.8pt)

= x86-64 / UEFI アーキテクチャ検討 (Shizuku V3)

状態: *ISA検討資料 x64 UEFI 独立章骨格*。2026-10-07。
対象リポジトリ: `Shizuku-stream` (Git HEAD: `3239d3761fd1898b78411434a31f91790251bd3b`)。
参照元設計書: `docs/design/kernel_v3_arch.typ`。

== 1. 対象・構造検討のみ
- *要旨*: 多ISA構造の一環としてx64 UEFI環境を構造考慮の対象とするが、現在実装対象外（Xtensa/RISC-V実装優先）かつ仮想化支援（VT-x/AMD-V）は完全対象外とする。チップ固有固定分岐を排し、ハードウェア機能変数 (`HAS_PRIVILEGE_SEPARATION=true` (Ring 0/3), `HAS_MEMORY_PROTECTION=true` (Page Tables), `HAS_GPR_HARDWARE_STACKING=partial` (Return Frameのみ), `HAS_NATIVE_ATOMIC_CAS=true`, `HAS_CYCLE_COUNTER=true` (RDTSC/RDTSCP)) により能力を直交化して静的選択する。
- *由来 / 根拠*: 確定方針（多ISA構造、現在実装Xtensa/RISC-Vのみ、x64 UEFI構造考慮/仮想化対象外）; `docs/design/kernel_v3_arch.typ`: L20–22, L40–52, L172–188, L248–260。
- *未決*: 具象プラットフォームのCPUトポロジおよび拡張機能（AVX-512等）の静的選択ルール。

== 2. ISA/ABI/UEFI境界
- *要旨*: 3階層分離（ISA層、ABI層、SoC/ファームウェア層）を遵守。ISA層はトラップ入口・特権遷移（Ring 0/3）・atomic・counterを担当しカーネル方針（GRANT/YIELD）を関知しない。ABI層は引数レジスタ（`rdi`=API ID, `rsi`=op）、callee-saved レジスタ保存、16Bスタック整列を担当。ファームウェア層はUEFI ExitBootServices()ハンドオフ境界（Boot Services終了、Runtime Services残存）を担当し、公開APIとperipheral objectが差を吸収する。具体entry命令選定（`SYSCALL`/`SYSRET` vs `INT`/`IRET`）は未決。
- *由来 / 根拠*: 確定方針（ISA層・kernel方針不知、公開API/peripheral object）; `docs/design/kernel_v3_arch.typ`: L43, L84, L205–239, L249, L325–330, L340–346, L352。
- *未決*: 具体entry命令（`SYSCALL` vs `INT` 等）の選定、および peripheral object の kernel 結合形態。

== 3. MMU保護と変換
- *要旨*: 保護driver（MMU: Page Tables CR3/PTE）を核から分離し能力値（`constexpr granularity`, `alignment_rule`, `region_count`, `can_split_stack`, `translates=true`）を開示。保護（protection）と変換（translation）は別能力。MMUはXtensa/RISC-V実装後に着手し、ハイブリッド方式（空間分離objectと共有object混在）を本命、VA全体一意空間を前提とし、user開発objectは生成時属性変更のみでkernelへ移せる構造とする。
- *由来 / 根拠*: 確定方針（保護driver、MMU Xtensa/RV後hybrid本命、VA全体一意、生成時属性変更）; `docs/design/kernel_v3_arch.typ`: L50, L173, L253, L346。
- *本命/前提と詳細未決*: ハイブリッド方式・VA全体一意・生成時属性変更による移行は本命/前提。ページテーブル階層構造（4段/5段）および空間分離境界の詳細配置は詳細未決。

== 4. context保存/stack
- *要旨*: IDT割込み/例外ではハードウェアがReturn Frame（RIP, CS, RFLAGS等）を積むがGPRは積まない。`SYSCALL` はRSP不変でRCX/R11を上書きし、GPR退避を行わない。保存契約（S0 caller-saved最小、S1 全callee-saved整数継続保存、S2 XSAVE/XRSTOR拡張状態）を緩和せず維持。context reuseは専用復元ゲート（SYSRET/IRET）で戻しC returnを禁止。保護粒度で丸めた境目でstack分割しcaller上側を不可侵として保護（入れ子でもregion数不変、丸め余白は `call_frame_push` 予約）。
- *由来 / 根拠*: 確定方針（context reuse、stack分割、保存契約非緩和）; `docs/design/kernel_v3_arch.typ`: L72–75, L100–104, L110–118, L175–179, L249, L257, L326–339, L353–355。
- *未決*: calleeに見せるcontext配置の幾何構造、およびスタック切替えにおける `SWAPGS` 採用の可否（OS選択肢の1つに留まる）。

== 5. atomic/IRQ/counter
- *要旨*: atomicはネイティブCAS命令 (`LOCK CMPXCHG`) およびRMW (`LOCK XADD`) が直接成立 (`HAS_NATIVE_ATOMIC_CAS=true`, `HAS_NATIVE_ATOMIC_RMW=true`)。同期短経路は32bit CAS/load/storeに限定。割込みマスクは大域フラグ `RFLAGS.IF`（外部INTRのみ遮蔽、NMI/SMI/例外は非遮蔽）、ISR判定はIDTハンドラ文脈＋trap_depth。IRQ予算（決定 1）は暫定保守的「税金」として扱い遅延調査保留（RP2350実測値は非適用例）。時間計測はRDTSC/RDTSCP（Invariant TSC要、`LFENCE; RDTSC` シリアライズ必須）。
- *由来 / 根拠*: 確定方針（atomic、IRQ予算決定1暫定税金・保留）; `docs/design/kernel_v3_arch.typ`: L48, L51, L88, L184–187, L254–258, L351, L356–362。
- *未決*: SMP環境におけるLAPIC IPI遅延およびTSCドリフト補正の具体方針。

== 6. アドレス資源とUEFI memory map/PCIe BAR/DMA/MMIO
- *要旨*: メモリ分割は address resource（CPU非依存: physical map / UEFI memory map / PCIe BAR / DMA / MMIO）と protection domain（CPU/保護driver依存）の一方向依存とし循環禁止。page table frames は domain 側が resource 側から確保して保護 driver へ渡す。BootServices lifetime（`ExitBootServices()` 呼出しでBoot Services終了、Runtime Services残存、`MapKey` 不一致時再取得リトライ）とUEFI memory mapハンドオフは現行資料の範囲で検討項目として維持し新規断定不要。
- *由来 / 根拠*: 確定方針（memory分割: address resourceとprotection domain、frames供給一方向循環不可、BootServices lifetime/UEFI map現行資料範囲検討項目新規断定不要）; `docs/design/kernel_v3_arch.typ`: L340–346。
- *未決*: 物理フレームアロケータの具象アルゴリズム、およびSetVirtualAddressMap()呼び出しの有無。

== 7. 後続・未決
- *要旨*: 確定事実・前提方針と未確認・詳細未決事項を厳密に分離し推測による仕様確定を排除。
  - 確定・前提: 実行権GRANT/無引数YIELD（決定 8, 9）、3階層分離、多ISA構造（仮想化対象外）、保護driver能力公開、protection domain $arrow$ address resource $arrow$ frames供給driver の一方向依存、MMU hybrid本命/VA全体一意、ネイティブCAS成立、保存契約維持。
  - 後続・未決: 1. 具体entry命令（`SYSCALL` vs `INT`）の選定、2. スタック切替え手法（`SWAPGS` 利用の有無）、3. calleeに見せるcontext配置幾何、4. MMU hybrid詳細配置・ページテーブル階層、5. UEFIハンドオフ後のフレームアロケータとRuntime Services連携、6. x64実機実測値と遅延調査（現状保留）。
- *由来 / 根拠*: 確定方針（全決定、未決事項一覧）; `docs/design/kernel_v3_arch.typ`: L325–346, L350–369, L524–537。
- *未決*: 上記項目 1–6 の後続設計および具象実装。
