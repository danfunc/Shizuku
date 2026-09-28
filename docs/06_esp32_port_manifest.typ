#set document(title: "ESP32-S3 / ESP32-C6 移植マニフェスト")
#set page(margin: (x: 1.55cm, y: 1.55cm))
#set text(font: "Hiragino Sans", size: 9pt, lang: "ja")
#set par(leading: 0.65em, spacing: 0.55em)
#set table(stroke: 0.45pt + gray, inset: 4pt, gutter: 3pt)

= ESP32-S3 / ESP32-C6 移植マニフェスト

更新: 2026-09-28。対象は ESP32-S3 と ESP32-C6。出典リンクは Espressif / 各プロジェクトの一次資料を優先した。ESP-IDF は現行安定系列 v6.1 の署名付き bugfix release v6.1.1 をインストーラで固定する。#link("https://github.com/espressif/esp-idf/releases/tag/v6.1.1")[ESP-IDF v6.1.1]

== 判定

#table(
  columns: (2.2fr, 1.5fr, 8fr),
  table.header([チップ], [判定], [理由]),
  [ESP32-S3], [条件付き], [Xtensa 文脈切替と dual-core SMP が全面的な ARCH/board 書換えを要する。保護付きオブジェクトは PMS + World Controller による Secure/Non-secure world 境界を使える可能性があるが、S3 の cache MMU は通常の SRAM ページテーブル切替用ではない。ESP-IDF が PMS API を private と明記しており、FreeRTOS / IDF の権限設定との競合を解決する必要がある。],
  [ESP32-C6], [条件付き], [RV32IMAC、U-mode、ecall、16 PMP region と atomic があり、保護付きオブジェクトの ISA 基盤は最も素直。ただし HP CPU は1コアなので Shizuku の2コア前提を縮退する。ESP-IDF / Wi-Fi / BLE の FreeRTOS 依存、PMP の ESP-IDF 初期化所有権、SRAM競合が障害.],
)

=== 最大の障害

+ FreeRTOS と Shizuku の二重スケジューラ。Wi-Fi/Bluetooth/IDF system service を維持するには FreeRTOS を残すのが現実的だが、カーネル単独がコアを独占する構成とは両立しない。
+ 保護ハードウェアの所有権。S3 PMS/World Controller は IDF 内部実装と衝突し、C6 PMP は IDF early startup が設定する。タスク切替ごとに境界・権限を再設定し、例外/割込みから復帰する道を再設計する必要がある。
+ Xtensa / RISC-V の例外・コンテキスト切替、タイマ、コア同期を新規実装する工数。S3はコア間で独立した scheduler/context の整合も必要。C6のLP CPUは別ISA/低電力 subsystem であり、Shizukuの第2対称コアとして数えられない。

== 設計前提とオブジェクト構成

D1「カーネルはオブジェクトを知らない」は本移植の制約にしない。カーネルが syscall handler と kernel object の登録表を保持し、停止点で登録セットを差し替える構成を許す。ホットスワップは、(1) 新 handler/object registry を準備、(2) 世代番号付きで原子的に公開、(3) 実行中 call frame / capability / callback が旧世代を参照しなくなったことを確認、(4) 旧 object を破棄、の順を要件とする。再登録だけで参照中 object を安全に回収できるとはみなさない。handler 入口、object ID ABI、失敗時の旧 registry 維持は設計・試験が必要。

二つの実行種別を定義する。

+ *保護付き object*: U/Non-secure 実行。コードとデータをカーネルから隔離し、syscall 境界で移行する。object ごとの独立アドレス空間が使える場合は切替コストを受け入れる。
+ *カーネル空間 object*: 特権・共通アドレス空間で実行する性能優先種別。カーネルメモリへアクセスできるため、署名済み/信頼済みコードに限定し、保護付き object と明確に区別する。

#table(
  columns: (3fr, 4fr, 6fr),
  table.header([方式], [長所], [制約・採否]),
  [FreeRTOSを置換], [Shizukuがスレッド・保護・スケジューラを単独所有], [Wi-Fi/BLE、flash、USB、timer 等 IDF component の task/queue/event-group 前提を代替する必要。無線を残す条件では非推奨。],
  [FreeRTOSを残し Shizukuを1つの IDF task として実行], [IDF driver/無線を保てる。先行プロトタイプに適する], [FreeRTOS task と Shizuku thread の二階層 scheduling。Shizuku が単独でCPU/割込みを独占しない broker/task API が必要。Shizuku thread ごとの保護切替は未解決。],
  [FreeRTOSを残し task と Shizuku thread を統合], [IDF system task を保ちつつ共通 scheduler を目指せる], [FreeRTOS port と task lifecycle を深く改変する。更新追従と検証負担が最大。将来案.],
)

推奨は *FreeRTOSを残した1 Shizuku service task* から開始し、まずサービス境界と時間予算を測る。保護付き object が同じCPUで安全に切替できること、syscallと外部IRQの原子性、停止・再開を示せるまで「ShizukuがIDF schedulerを置換した」と扱わない。OS/ドライバ独立が決定条件なら、Wi-Fi/BLE機能を削る別ベアメタル移植として再評価する。

== 保護の評価

#table(
  columns: (1.8fr, 3.1fr, 4.3fr, 4.3fr, 1.5fr),
  table.header([機能], [RP2350実装 (file:line)], [ESP32-S3], [ESP32-C6], [影響]),
  [privilege / syscall], [`armv8m_ctx.S:112-119` SVC入口、`armv8m.hpp:319-333` privilege/syscall、`concepts/arch.hpp:43-46`], [Xtensa LX7 の例外レベルと S3 World Controller/PMS を使った world switch を検証する。S3 protected NuttX port は WC/PMS による isolation 例がある。IDF 標準アプリは一つの権限側で動く想定で、IDF private protection 設定との統合が必要。SVC 相当の syscall exception/trap wrapper を新設。], [M-mode kernel / U-mode object、`ecall` trap で実装可能。U-modeから必要な interrupt delegation と mstatus/mepc/mcause、trap stackを定義し、IDFが設定した PMP と衝突しない早期初期化を確立。], [書換えで対応可],
  [object memory isolation], [`board.cpp:120-139` code RO/X, heap RW/XN, grant; `arch_glue.cpp:44-63` grant restore], [Cache MMU は SPI flash/PSRAM と仮想領域のmapping API。通常の内部SRAMをobject毎に切り替える汎用ページテーブル機構としては使えない。PMS/WCで region permissions と world isolation を組む。linker配置、internal SRAM区分、DMA/peripheral master 迂回を計測・遮断。独立virtual address space は未確認。], [最大16 PMP region、M/U mode。NAPOT/TORで code RO/X、data RW/NX、kernel M-only を配置し、U-mode object の領域を切替える案。標準PMPはregion access controlであり、MMUのページ変換ではない。SRAM全域を被覆できるregion数か要確認。], [書換えで対応可],
  [動的 grant], [`armv8m.hpp:73-81`; `arch_glue.cpp:44-63`], [PMS region数/境界と world属性に収まるgrantのみ許可。対象SDKのPMS private APIを避けるか、起動/割込み境界を所有する必要。], [PMP entry予算を固定/優先度つきgrantに配賦。object間のgrant切替をtrap return時に更新。PMP lock bitで再設定を塞がない設計が必要。], [書換えで対応可],
  [stack limit], [`concepts/arch.hpp:36-37`; `armv8m_ctx.S:79-83, 181-182` PSPLIM], [PMS guard boundary か stack sentinel。IDF per-task HW stack guardとの所有権を分け、例外stackも保護。PSPLIM同等とは未確認。], [PMP guard regionはregion不足の可能性。違反trap検出の構成確認とIDF Hardware Stack Guard/Debug Assistantとの競合確認が必要。], [機能縮退 / 再設計],
  [multicore atomic], [`concepts/arch.hpp:40-42`; `rp2350_pico2.hpp:13`; `board.cpp:40-90`], [LX7 atomic/critical section。FreeRTOS SMP lockとの相互作用、割込み禁止範囲、cache coherenceを確認。2コアは維持可能。], [RV32IMAC A extensionあり。HPは1コア。LP CPUは別実行系なので Shizuku coreに数えず、必要ならmailbox service。], [C6縮退 / S3書換え],
  [timer / deferred switch], [`armv8m.hpp:160-169, 266-268`; `concepts/arch.hpp:47-54`], [Xtensa exception/vector、timer、IPI/PendSV相当を新設し、優先順位とper-core tickを検証。], [SYSTIMERとinterrupt matrix、software interruptで実装。`ecall > timer > deferred switch` の優先順とtickを確認。], [書換えで対応可],
  [context / fault / debugger], [`armv8m_ctx.S:25-52, 112-201`], [windowed ABI、WINDOWBASE/WINDOWSTART、例外vector、FPU状態を含むcontextを新規実装。frame/stack switchをIDF ABIと合わせる。DebugMonitorを置換。], [trap entryでcaller/callee registersとmepc/mstatusを保存。C6 HP RV32IMACはF無し。mretでU/M復帰。GDBとIDF/OpenOCDのtransport競合を避ける。], [書換えで対応可],
)

*S3 保護の注意*: #link("https://documentation.espressif.com/esp32-s3_technical_reference_manual_en.pdf")[S3 TRM PMS ch.15] は内部 SRAM を命令/データ領域に分割し、CPU bus/worldごとの permission を設定する。これは ESP-TEE/NuttX protected mode の設計手掛かりだが、ESP-IDFの標準アプリから使える安定 public API を意味しない。#link("https://github.com/apache/nuttx/blob/master/Documentation/platforms/xtensa/esp32s3/boards/esp32s3-devkit/index.rst")[NuttX S3 protected-mode notes]。S3独立アドレス空間を必要とする設計は、実際のMMU page table機構を確認できるまで採用しない。

*C6 保護の注意*: C6のPMP/PMA 16 region、U mode、interrupt delegation は chip datasheet で確認できる。ESP-IDFは初期化時にPMPを設定する。アプリケーション側が再利用可能な権限を得るには、early startupの移管か自前startupが必要。#link("https://documentation.espressif.com/esp32-c6_datasheet_en.html")[ESP32-C6 Datasheet] #link("https://docs.espressif.com/projects/esp-idf/en/stable/esp32c6/security/security.html")[ESP-IDF C6 Memory Protection]

== Shizuku前提との対応一覧

影響度: 「移植不能」は対象機能がハードウェア上成立しない場合、「機能縮退」は構成/能力を下げる場合、「書換えで対応可」は新backendで意味を維持できる見込みを示す。試作・実機確認前の設計評価であり、成立保証ではない。

#table(
  columns: (1.8fr, 3fr, 4fr, 4fr, 2fr),
  table.header([前提], [RP2350実装 (file:line)], [S3], [C6], [影響・資料]),
  [board & core], [`rp2350_pico2.hpp:10-29`; `board.cpp:40-90`], [2 LX7 core、new board implementation.], [HP single core。LP coreは別subsystem。CPU_COUNT=1とboard contract見直し。], [C6縮退; #link("https://documentation.espressif.com/esp32-c6_datasheet_en.html")[C6 datasheet]],
  [syscall / privilege], [`armv8m_ctx.S:112-119`; `armv8m.hpp:319-333`], [Xtensa trap + WC/PMS world switch.], [`ecall` + U/M mode.], [書換えで対応可; chip TRM/datasheet],
  [memory protection], [`board.cpp:120-139`; `arch_glue.cpp:44-63`], [SRAM PMS regions; cache MMU maps SPI flash/PSRAM only. Internal-SRAM page tableは未確認.], [16 PMP regions; physical isolation, no virtual memory translation.], [書換え; #link("https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/system/mm.html")[S3 MMU]],
  [stack overflow], [`armv8m_ctx.S:79-83, 181-182`], [PMS or software canary/guard; PSPLIM counterpart未確認.], [PMP guard / software check; PMP budgetを計測.], [縮退・再設計],
  [exception priority], [`board.cpp:82-109`], [Xtensa levels/vectors and IDF interrupt allocator; map kernel switch priority around IDF IRQs.], [interrupt matrix/CPU priority; reserve levels for trap/timer/switch.], [書換えで対応可],
  [scheduling / FreeRTOS], [`concepts/arch.hpp:47-54`], [IDF starts both CPU and FreeRTOS scheduler before `app_main`; naked-core entryではない.], [IDF `app_main` FreeRTOS task; system tasks need FreeRTOS.], [IDF coexistence; #link("https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/startup.html")[S3 startup]],
  [timers], [`armv8m.hpp:160-169`], [IDF timer/Xtensa interrupt path; per-core invariantsを測定.], [C6 esp_timer uses SYSTIMER; low-level oneshot/deferred switch backend.], [書換え; #link("https://docs.espressif.com/projects/esp-idf/en/stable/esp32c6/api-reference/system/esp_timer.html")[C6 esp_timer]],
  [atomic], [`concepts/arch.hpp:40-42`], [Xtensa atomic/SMP backend.], [RV32 A extension; Shizuku CPU count one.], [書換え / C6縮退],
  [BLE UART / OTA], [`modules/pico_sdk_support/BUILD.bazel:70-88`], [ESP IDF BLE controller/HCI; OTA writes inactive app slot.], [BTstack upstream has IDF ESP32 port.], [書換え; #link("https://github.com/bluekitchen/btstack/tree/master/port/esp32")[BTstack]],
  [USB CDC], [`objects/usb_cdc.cpp:14-22, 43-60`], [USB OTG FS device CDC; TinyUSB lists S3. Check routing and CDC count.], [USB Serial/JTAG fixed CDC/JTAG, not general USB OTG; one console, no custom multi-interface device without external USB.], [S3対応 / C6縮退; #link("https://github.com/hathach/tinyusb")[TinyUSB]],
  [flash FS / OTA], [`flash_map.hpp:26-89`], [ESP partition table: custom FS data + two app slots + otadata; confirm sector/wear constraints.], [Same; module-flash dependent; two slots + otadata for safe OTA.], [書換え; #link("https://docs.espressif.com/projects/esp-idf/en/latest/esp32c6/api-guides/partition-tables.html")[partitions]],
  [GDB stub / diag], [`rp2350_pico2.hpp:36-53`; `usb_cdc.cpp:28-48`], [USB CDC/JTAG overlap; dedicate separate transport.], [USB Serial/JTAG for debug; UART/CDC console, no log/RSP multiplexing.], [書換えで対応可],
  [memory budget], [`handler.cpp:157-162` arena 128 KiB; `:114-117` bookkeeping 8 KiB], [IRAM/DRAM and cache reduce heap; measure ELF/runtime caps.], [SRAM shared with IDF/Wi-Fi/BT; measure ELF/free heap.], [未確定; #link("https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/system/mem_alloc.html")[S3 RAM]],
)

== 無線・USB・flash・debug

*BLE UART*: BTstack's upstream ESP32 port is documented as ESP-IDF based; BTstack v1.7+ sources are built out-of-tree, not copied into IDF components. Its chipset list includes S3 and C6 LE/Wi-Fi controllers. Integrate as a version-pinned IDF component and run BTstack event loop in a dedicated service task. Confirm the exact GATT UART profile and ESP-IDF v6.1.1 compatibility; upstream README's tested IDF version is older. #link("https://github.com/bluekitchen/btstack/blob/master/port/esp32/README.md")[BTstack ESP32 port]

*OTA*: replace raw staging writes with IDF app OTA API and partition table: bootloader, partition table, nvs/phy_init, otadata, `ota_0`, `ota_1`, plus a custom filesystem data partition. Define image signature/rollback and filesystem migration separately. BLE transfers only the candidate image; IDF verifies and selects next boot slot. Do not overwrite the running slot. Partition sizes are module-flash-dependent and unresolved.

*USB*: S3 can host TinyUSB device CDC via USB OTG. C6 built-in USB Serial/JTAG is a fixed-purpose CDC/JTAG controller and is not equivalent to general TinyUSB USB device controller. If the Shizuku requirement is two CDC channels (diagnostics + GDB), S3 can likely recreate it; C6 needs UART for one channel or external USB hardware. #link("https://github.com/hathach/tinyusb")[TinyUSB support matrix] #link("https://docs.espressif.com/projects/esp-idf/en/stable/esp32c6/api-guides/usb-serial-jtag-console.html")[C6 USB Serial/JTAG]

*GDB*: preserve GDB RSP only on a dedicated transport. S3 has JTAG/OpenOCD and USB CDC options; C6 has built-in USB-JTAG. Never mix console logs with RSP. Shizuku's self-host DebugMonitor cannot be carried over; implement a debug exception/task-stop backend or disable self-host single-thread stepping in the initial port.

== Build arrangement

ESP-IDF's supported project build is CMake + Ninja + `idf.py`; no official Espressif Bazel build for ESP-IDF was found in the researched primary documentation. Keep `idf.py` authoritative for firmware link, bootloader, partition table, generated sdkconfig, IDF components and linker scripts. First add `ports/esp32/idf/` project and use an IDF component that compiles Shizuku source from the workspace. Retain Bazel for existing Pico platforms and possibly for host-only core libraries/probes. If Bazel must invoke IDF, wrap `idf.py build` as an explicitly environment-pinned action with declared sdkconfig/IDF/submodule inputs and ELF/bin/map outputs; do not model IDF as a plain `cc_library` or separately link an IDF-built static archive without its generated link flags and scripts. #link("https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/build-system.html")[ESP-IDF build system]

== 段階計画と見積り

#table(
  columns: (2fr, 8fr, 2fr),
  table.header([段階], [目的・確認], [概算]),
  [0. 環境/最小起動], [固定IDFを導入。S3/C6でHello + `idf.py size`, linker map, FreeRTOS task baseline。実機接続・書込みは別承認まで行わない。], [0.5–1人日],
  [1. IDF component統合], [Shizuku pure C++ coreをcomponent化、config/arch/board選択をESP targetへ分岐。empty handler/object registryからリンク。], [2–4人日],
  [2. C6 single-core ARCH], [RV32IMAC context/trap, M/U, PMP, syscall, SYSTIMER oneshot, fault report。clang probeから始め、IDF compile/link。], [5–10人日],
  [3. S3 ARCH / protection], [windowed ABI、per-core context、exception vectors、PMS/WC layoutとIDF coexistence proof。], [10–20人日],
  [4. object lifecycle / hot swap], [handler/object registry generations, reference quiescence, rollback, protected vs kernel-space APIs, negative access tests.], [5–10人日],
  [5. drivers / radio / storage], [BLE GATT UART, OTA slot+rollback, filesystem partition, USB/console/GDB plan, resource accounting.], [5–10人日],
  [6. verification], [QEMU/unit where supported, on-device fault/stack/grant/core/timing/OTA interruption tests. Real hardware necessary for final acceptance.], [5–10人日],
)

概算は設計・実装範囲からの初期推定で、ボード選定・PMS設計・IDF component compileが未検証のため ±2倍以上の幅がある。

== ESP-IDF installer

`ports/esp32/tools/install_esp_idf.sh` は Apple Silicon macOS 向け。v6.1.1 tagを固定し、Homebrew prerequisiteを確認し、不足時は対話して導入、`~/esp/esp-idf` を clone/reuse し `./install.sh esp32s3,esp32c6` を実行する。既存ディレクトリのtagが異なると上書きせず停止する。スクリプトは本作業では実行しない。初回は toolchain + Python environment を含み数 GB の通信/保存領域を見込む (正確な容量は未測定)。

実行後は `. ~/esp/esp-idf/export.sh`, `idf.py --version` を使い、ESP-IDFの最小exampleで `idf.py set-target esp32c6 build` と `idf.py set-target esp32s3 build` を実施する。script自体は`flash`も`monitor`も起動しない。

== この作業での試作・未確認

`ports/esp32/c6/pmp_arch_probe.S` は `mstatus`, `pmpcfg0`, `pmpaddr0`, LR/SC, ECALL の assembler mnemonic を一箇所で確認するだけのprobe。trap handler、context layout、PMP policy、Shizuku integrationではない。`llvm-mc` による構文・オブジェクト生成は通過した。Apple clang の同等 compile は LLVM option parser が `-riscv-add-build-attributes` を拒否して失敗。既存 `rv32_isa_probe.S` もISA probeのまま保持する。

未確認: ESP-IDFは未導入で、IDF component compile、S3 Xtensa cross-assembly/link、RAM/flash使用量、PMS/WCのIDF共存、C6 PMP entry ownership/region予算、実機の権限拒否・scheduler負荷・OTA rollback・USB列挙を確認していない。C6のIDF v6.1.1 releaseにおけるearly PMP具体配置、S3 public PMS control API、対象ボードのflash/PSRAM容量も未確認。再開時は環境構築後にidf minimal build、PMP/WC ownership spike、then core context+trap skeletonを順に行う。

== 出典

+ ESP-IDF #link("https://github.com/espressif/esp-idf/releases")[releases], #link("https://github.com/espressif/esp-idf/releases/tag/v6.1.1")[v6.1.1], #link("https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/macos-setup.html")[macOS install guide]
+ Espressif #link("https://documentation.espressif.com/esp32-s3_technical_reference_manual_en.pdf")[ESP32-S3 TRM], #link("https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/system/mm.html")[S3 MMU API], #link("https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/startup.html")[S3 startup]
+ Espressif #link("https://documentation.espressif.com/esp32-c6_datasheet_en.html")[ESP32-C6 Datasheet], #link("https://documentation.espressif.com/esp32-c6_technical_reference_manual_en.pdf")[C6 TRM], #link("https://docs.espressif.com/projects/esp-idf/en/stable/esp32c6/security/security.html")[C6 memory protection]
+ Espressif #link("https://docs.espressif.com/projects/esp-idf/en/stable/esp32c6/api-guides/usb-serial-jtag-console.html")[C6 USB Serial/JTAG], #link("https://docs.espressif.com/projects/esp-idf/en/latest/esp32c6/api-guides/partition-tables.html")[partition tables], #link("https://docs.espressif.com/projects/esp-idf/en/latest/esp32c6/api-reference/system/ota.html")[OTA]
+ #link("https://github.com/bluekitchen/btstack/tree/master/port/esp32")[BTstack ESP32], #link("https://github.com/hathach/tinyusb")[TinyUSB], #link("https://github.com/apache/nuttx/tree/master/arch/xtensa/src/esp32s3")[NuttX ESP32-S3 protected port]

Typst compile and local cross-assembly results are recorded in the delivery report; no device was accessed.
