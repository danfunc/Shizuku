#set document(title: "Shizuku V3 カーネル設計", author: "Shizuku")
#set page(paper: "a4", margin: (x: 18mm, y: 18mm), numbering: "1")
#set text(font: ("Hiragino Sans", "Arial"), size: 9pt, lang: "ja")
#set par(leading: 0.65em)
#set heading(numbering: "1.1")
#show raw: set text(font: "Menlo", size: 7.5pt)
#let ref(s) = text(size: 7.8pt, fill: rgb("555555"), s)

= Shizuku V3 カーネル設計

状態: *設計骨格 (段①: 第1章 範囲・設計契約)*。2026-10-07。
基点: `3239d3761fd1898b78411434a31f91790251bd3b` (`feat/v3-groundwork`)。
旧文書保存版: `/tmp/shizuku-v3-skeleton/kernel_v3.original.typ` (SHA-256: `625956303e0b7d7f9049a3bcc42f5fb565a1104f5e6afdf5026e6e3af97f97d0`)。
アーキテクチャ補完設計書: `docs/design/kernel_v3_arch.typ` (SHA-256: `0fe99730d0d419d555bd22f694b7f7bd4cb42f479788bd4bbef8aae4ee7cf183`)。

== 範囲・設計契約

現在実装は Xtensa および RISC-V のみであり、x64 は UEFI 構造を考慮した構造定義のみを対象とし (仮想化は対象外)、Cortex-M 等の他 ISA 展開資料は別ファイル (`docs/design/kernel_v3_arch.typ`) に分離する。
本書は旧保存版 (`kernel_v3.original.typ`) の「固定した判断と設計の結論」(L22–53) および「ユーザーに決めてほしい点」(L666–684) の決定 1–11 を継承し、段①として再構成した基本骨格である。
設計契約は、実行権の GRANT 貸付けと無引数 YIELD 返却 (決定 8 決着・wait キュー不採用; L34, L480–493)、多 ISA の能力別整理と三層分離 (ISA/保護ドライバ/カーネル核)、アドレス資源と保護ドメインの一方向依存、context reuse の保護粒度スタック分割、静的属性による API 経路決定 (L29–31, L89–136)、および `kernel_services` 最小契約の V2 先行維持 (L37, L577–580) を基本とする。IRQ 予算 (決定 1) は暫定保守的な「税金」として実測値を記録し遅延調査を保留する。

*現行との差・未決事項*: `kernel_services::yield()` は無引数に更新済み (`kernel_services.hpp:9`)。`irq_state` のフィールド差異や IRQ 予算超過、context 配置、非特権 origin の昇格防止条件、atomic ordering 等は未決・保留として扱う。

== モジュール構造

=== 責務一覧と境界定義

本書が対象とする 10 個のコンポーネントおよびその責務境界を以下に定める。現在実装は Xtensa および RISC-V のみを対象とし、x64 は UEFI 構造考慮 (仮想化対象外) を維持する (他 ISA は `docs/design/kernel_v3_arch.typ` 参照)。

+ *ISA層*: trap/syscall 入口・復帰、レジスタ保存・復帰、atomic 操作、タイマ/cycle counter、特権遷移を担当。カーネルの方針・スケジューリングを知らない独立層。
+ *保護driver*: MPU/PMP/PMS/MMU レジスタ制御。静的能力特性 (`constexpr granularity`, `alignment_rule`, `region_count`, `can_split_stack`, `translates`) を提供。ドメインからの指示でリージョン設定を行い、frames 確保は要求せずドメインから受け取る。
+ *address resource*: CPU 非依存の物理メモリ・アドレス空間管理 (physical map, UEFI memory map, PCIe BAR, DMA バッファ, MMIO 領域, page table 用 frames の確保・返却)。
+ *protection domain*: 論理保護ドメイン管理。address resource から frames を確保して保護 driver へ渡し、stack/heap/MMIO 境界を構成。context reuse のスタック幾何・保護境界を管理。
+ *scheduler (GRANT/YIELD)*: 実行権の GRANT 貸付け (A→B→C 入れ子可、内側予算の外残量 clamp (I-7)、祖先返却) と無引数 YIELD 返却 (親への即時返却、手数料なし)。タイマ装填・超過巻き取り (`grant_unwind`, `grant_charge`)。専用 wait/wake キューを作らず、mutex は GRANT(owner) + CAS 再確認で処理。
+ *dispatcher / API属性表*: 静的属性表 (`svc_attributes`: permission, from_isr, mode, save_level, authority) に基づく syscall ゲート。native / objectland のレーン判定、context reuse による `call_frame_push`/`pop`、特権検査。
+ *object / peripheral*: サービスロジックおよびハードウェア制御。公開 API (`EXPORT_METHOD` 等) により user/kernel 双方でドライバを作成可能とし、kernel 側 interface 差は peripheral object が吸収。
+ *kernel_services*: サードパーティ shim / blob 向けの最小限の特権サービス契約 (`yield()`, `grant()`, `sleep_us()`, `time_us()`, `cas()`, `in_isr()`, `irq_save/restore()`)。dispatcher 再入を避け、内部関数または直接ゲートで処理 (V2 先行維持)。
+ *esp-idf-support*: Wi-Fi OSI / BLE NPL shim 関数群。shim 自体は object 化せず単なるライブラリ関数として提供し、内部で `kernel_services` を呼ぶ。
+ *最上位composition*: 全モジュールの静的バインド、起動時初期化、依存性の注入、ルートコンテキストおよびスレッド 0 のブートストラップ。

=== 依存DAGと実行時契約

依存の矢印は「利用側 → 提供側」とし、静的依存関係を以下の DAG に示す。address resource は CPU 非依存であり、ISA 層への静的依存を持たない。

```
                    [最上位 composition]
       +----------------------+----------------------+
       |                      |                      |
       v                      v                      v
[esp-idf-support]     [object/peripheral]            |
       |                   |          |              |
       v                   v          |              |
[kernel_services]  [dispatcher/API属性]       |              |
   |         |         |          |   |              |
   |         |         |          +---|-------+      |
   |         |         v              v       |      |
   |         +-------------------> [scheduler]|      |
   |                   |              |       |      |
   |                   v              |       |      |
   |             [protection domain]  |       |      |
   |                |          |      |       |      |
   |                v          v      |       |      |
   |            [address   [保護driver]       |      |
   |             resource]     |      |       |      |
   |            (CPU非依存)    |      |       |      |
   |                           |      |       |      |
   v                           v      v       v      v
+-------------------------------------------------------+
|                         ISA層                         |
+-------------------------------------------------------+
```

*静的依存の Topological Order (提供側 → 利用側)*:
1. address resource (CPU非依存・独立), ISA層 (独立) → 2. 保護driver, scheduler → 3. protection domain → 4. dispatcher / API属性表, kernel_services → 5. object / peripheral, esp-idf-support → 6. 最上位 composition。静的依存に循環は存在しない。

*実行時契約・注入および循環排除 (静的DAGと区別)*:
- *scheduler と dispatcher の循環排除*: 静的依存は `object/peripheral → dispatcher/API属性表 → scheduler` および `object/peripheral → scheduler` の利用側→提供側とする。scheduler から dispatcher への逆方向参照は排し、切替文脈を戻り値で返す「継続/結果契約 (continuation / result contract)」または最上位 composition から切替トリガーを渡す「起動時 composition 注入」を候補案とする (既存コードの確定実装ではない)。
- *保護領域と資源の契約*: `protection domain → address resource / 保護driver` の一方向。address resource は CPU 非依存。保護 driver は frames 確保を要求せず、domain が address resource から確保した frames を受け取る。
- *kernel_services の再入排除*: `esp-idf-support → kernel_services → kernel側 (scheduler, ISA層)`。shim は object 化しない。`kernel_services` は dispatcher を再入せず、scheduler / ISA 核へ直接委任する。
- *ドライバとペリフェラル境界*: user/kernel どちらでも公開 API で driver を作成でき、kernel 側 interface 差は peripheral object が吸収する。

=== 現行実装対応と推奨配置案

現行ツリー (`source/`, `internal_headers/`, `modules/`) の確認済み事実と、V3 推奨配置の対応を示す。なお現行リポジトリの ARMv6-M / ARMv8-M 関連ファイル (`modules/pico_sdk_support/` 等) は既存 V2 / 先行 groundwork コードの配置であり、厳密な V2/V3 帰属詳細は未確認・混在として扱う (確定方針「現在実装は Xtensa および RISC-V のみ」と既存 ARM コード配置を区別)。

#table(
  columns: (1.1fr, 2.2fr, 1fr, 1.7fr),
  inset: 3pt,
  table.header(repeat: true, [コンポーネント], [現行実装 (file:line / 確認済み事実)], [区分], [推奨移動案 (V3)]),
  [ISA層], [`modules/pico_sdk_support/rp2350/internal_headers/shizuku/archs/armv8m.hpp:16–38,43–120`\ `modules/pico_sdk_support/rp2350/armv8m_ctx.S:1–150`\ `modules/pico_sdk_support/rp2040/internal_headers/shizuku/archs/armv6m.hpp:12–110`\ `modules/pico_sdk_support/rp2040/armv6m_ctx.S:1–120`\ `internal_headers/shizuku/concepts/arch.hpp:1–30`], [既存配置 (ARM既存コード。帰属詳細未確認・V3対象はXtensa/RV)], [`internal_headers/shizuku/arch/`\ `source/arch/`\ (保護driverと分離・純化。Xtensa/RVへ適用)],
  [保護driver], [`modules/pico_sdk_support/rp2350/internal_headers/shizuku/archs/armv8m.hpp:309–328` (MPU直書き)\ `modules/pico_sdk_support/rp2040/internal_headers/shizuku/archs/armv6m.hpp:99–102` (MPU直書き)], [既存配置 (ARM ARCH混在。帰属詳細未確認)], [`internal_headers/shizuku/protection_drivers/`\ (静的能力特性の独立定義)],
  [address resource], [`source/kernel_object/memory.cpp:1–181` (arena/allocator)\ `modules/pico_sdk_support/memory_manager.cpp:1–22`\ `internal_headers/shizuku/concepts/memory_manager.hpp:1–2` (stub)], [既存配置 (V2 kobj等に混在)], [`internal_headers/shizuku/address_resource.hpp`\ `source/memory/address_resource.cpp`\ (CPU非依存の資源管理)],
  [protection domain], [`source/kernel/dispatch.cpp:190–191` (`set_region_window`)\ `source/kernel_object/handler.cpp:468–510` (`grant_region`)], [既存配置 (分散・帰属詳細未確認)], [`internal_headers/shizuku/protection_domain.hpp`\ `source/kernel/protection_domain.cpp`\ (frames受渡・context reuse境界)],
  [scheduler], [`source/kernel/thread.cpp:187–378` (`claim`, `do_grant`, `grant_unwind`, `grant_charge`, `arm_timer`)\ `source/kernel/dispatch.cpp:286–298` (`GRANT`), `360–384` (`pendsv_dispatch`)], [既存配置 (V2実装/先行混在)], [`internal_headers/shizuku/scheduler.hpp`\ `source/kernel/scheduler.cpp`\ (専用キュー排除、継続契約化)],
  [dispatcher / API属性], [`internal_headers/shizuku/svc_attributes.hpp:7–59` (`svc_attributes`)\ `source/kernel/dispatch.cpp:70–193` (`call_frame_push/pop`), `195–358` (`svc_dispatch`)\ `internal_headers/shizuku/object_api.hpp:15–140`], [既存配置 (属性表先行導入済み)], [`internal_headers/shizuku/dispatcher.hpp`\ `source/kernel/dispatcher.cpp`\ (静的属性レーン判定)],
  [object / peripheral], [`modules/pico_sdk_support/objects/peripherals.cpp:1–422` (GPIO/SPI等)\ `modules/pico_sdk_support/objects/flash_fs.cpp:1–200`\ `source/kernel_object/handler.cpp:368–435` (create), `1572–1690` (handle)], [既存配置 (ARMペリフェラル等)], [`modules/drivers/peripheral_objects/`\ `source/objects/`\ (user/kernel共通API、IF差吸収)],
  [kernel_services], [`internal_headers/shizuku/kernel_services.hpp:1–20` (API宣言: `yield`無引数)\ `source/kernel/services.cpp:7–58` (V2特権gate先行実装)\ `source/selftest/kernel_services.cpp:1–67`], [既存配置 (V2先行実装)], [`source/kernel/kernel_services.cpp`\ (dispatcher再入を排し直接委任)],
  [esp-idf-support], [旧保存版 `kernel_v3.original.typ:36, 503–538` に仕様規定], [未配置 / 提案先], [`modules/esp_idf_support/`\ (Wi-Fi OSI / BLE NPL shim関数群)],
  [最上位composition], [`firmware/main.cpp:80–120` (`app_entry`), `406–423` (`main`)\ `source/kernel/init.cpp:1–92`], [既存配置 (RP2040/2350向け)], [`source/composition/boot.cpp`\ `firmware/main.cpp`\ (静的バインド・注入専任化)],
)

*現行との差・未決事項*: 現行実装では ISA と MPU 操作、およびカーネル核とオブジェクトランドが密結合しており、多 ISA 展開時の境界分離が課題となる。callee context 配置、atomic ordering、非特権 origin の昇格防止条件、IRQ 予算超過等は未決・保留として扱う。

== 実行権とスケジューラ
後続章単位発注で骨格追記

== API属性とobject/driver境界
後続章単位発注で骨格追記

== contextと保護・メモリ
後続章単位発注で骨格追記

== kernel_servicesとV2移行
後続章単位発注で骨格追記

== 計測・未決事項
後続章単位発注で骨格追記
