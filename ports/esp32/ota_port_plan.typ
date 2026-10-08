#set page(paper: "a4", margin: (x: 1.8cm, y: 2cm))
#set text(size: 9pt)
#set par(justify: true, leading: 0.65em)
#set table(inset: (x: 5pt, y: 4pt), stroke: 0.5pt + luma(120))
#show table.cell: set par(justify: false)

#align(center)[
  #text(size: 15pt, weight: "bold")[ESP32-C6 への OTA 移植計画 (設計調査)] \
  #text(size: 8.5pt)[2026-10-07 | 設計のみ。ソース・ビルド・実機は未実施 | 対象: port/esp32-kernel (HEAD f138829 + 未コミット修正)]
]

*凡例*: [確定] = file:line を読んで確認、[推測] = 読んだ範囲からの見立て、[未検証] = 実行・実機で確かめていない。
Web 検索は権限が無く使えなかったため、ESP-IDF の記述はローカルの v6.1 (`.local/esp-idf`) のヘッダ・Kconfig に基づく。C6 の flash サイズの一次資料 (データシート) は未確認。

= 1. 結論 (推奨)

+ *コミット方式は IDF の OTA パーティション (ota_0 / ota_1 + otadata + bootloader ロールバック) を使う*。RP 版の「staging → 稼働中の領域へ上書きコピー (no return)」は C6 では採らない。理由: 自分自身を上書きする間は電源断で壊れ、IRAM 常駐のコピー関数を自前で持つ必要がある。IDF 方式は「空いている側に書く → 検証 → otadata を 1 セクタ更新 → 再起動」で、旧側が残るので戻れる。
+ *XNO の `xno::ota` は「フレーミング・再送・inflate・CRC・状態機械」だけを使い、flash と再起動は差し込み口 (flash_target) に切り出す*。現在の `ota.cpp` は RP 専用 API を直接呼ぶので、そのままでは C6 に載らない (§2)。
+ *順序は「OTA を先に、カーネルを後」*: 最初は FreeRTOS を普通に起動する IDF アプリの上で OTA だけを成立させ (USB-Serial/JTAG → BLE)、カーネル統合 (OTA をカーネルオブジェクト化) はカーネルが C6 で動いた後に行う (§4)。現状の C6 イメージは IDF の初期化を飛ばしており、`esp_partition` / `esp_ota` / NimBLE がどれも使えない (§3)。
+ *ホストツールは `ble_ota_send.py` をほぼそのまま共用できる*。デバイス側が UUID・フレーム形式・応答行を RP と同じにすれば足りる (§2.2)。

= 2. 構成要素の分類

== 2.1 チップ非依存 / C6 で新規

#table(
  columns: (1.5fr, 1.2fr, 4fr),
  align: (left, left, left),
  table.header([要素], [分類], [根拠・C6 で要ること]),
  [XNOR/XNCK フレーム、NEEDSEQ/QUERY/NEEDEND 再送、状態機械], [非依存],
  [`XNO-ota/objects/ota/ota.cpp` (1370 行)。ただし `shizuku::stream` と `object_api` の syscall (`EXPORT_METHOD`, `STREAM_BIND`, `YIELD`, `SLEEP_US`, `CALL_METHOD`: ota.cpp:35-40, 1169-1170 ほか) を使う = *カーネルが動いて初めて走る* [確定]。],
  [raw deflate 展開], [非依存], [`inflate.hpp` (328 行)。標準ヘッダのみのはず [推測: 内容は未精読]。C6 は RAM が小さい (HP SRAM 512KB) ため、窓・テーブルの静的サイズだけ確認する。],
  [CRC32 (nibble 表)、CRC16 (チャンクヘッダ)], [非依存], [ota.cpp:60-74。そのまま。],
  [フレーム型 `frame_t` (len + 244B)], [要設計], [`ble_uart.hpp:13-18` (Shizuku-stream) は RP 側ヘッダ。`xno/ota.hpp:3-4` が `shizuku/objects/ble_uart.hpp` と `flash_map.hpp` を include しており、後者は pico 定数 (`PICO_FLASH_*`, `XIP_BASE`) に依存 [確定]。型だけを共通ヘッダ (例: `ota_frame.hpp`) へ出す。244 = ATT MTU 247 − 3 で、ホスト側 `DEFAULT_PACKET_SIZE=244` と一致 (`ble_ota_send.py:51`)。],
  [flash 消去・書込・読戻し], [*C6 新規*], [RP: `flash_safe_execute` で 4KB 消去 / 256B ページ書込 (ota.cpp:124-131, 424, 479)。C6: `esp_ota_begin` / `esp_ota_write_with_offset` / `esp_partition_read` (§2.3)。],
  [staging の読み返し CRC], [*C6 新規*], [RP は XIP の直読み (ota.cpp:608 `XIP_BASE + STAGING_OFFSET`)。C6 は `esp_partition_read` で 4KB ずつ読む (cache/MMU マップに依存しない)。],
  [commit と再起動], [*C6 新規*], [RP: IRAM 常駐 `commit_blast` で 0x0 へコピーし ROM reboot (ota.cpp:145-187, 767-835)。C6: `esp_ota_end` → `esp_ota_set_boot_partition` → `esp_restart` (§2.4)。ベクタ表検査 (ota.cpp:788-790、SRAM/XIP 上位バイト) は C6 では *イメージヘッダ magic 0xE9 + `esp_ota_end` の検証* に置き換える [推測: `esp_ota_end` が magic/チェックサム/SHA256 を検証。実装の精読は未実施]。],
  [進捗行の副出力 (UART0)], [削除], [ota.cpp:92 `uart_puts(uart0, ...)` は pico 専用。flash_target/ログ口 (`say` の出力先) の差し込みにする。],
  [時刻・待ち], [カーネル依存], [`BOARD::time_us` / `diag_printf` は C6 では未実装スタブ (link_stubs.cpp:52, 55) [確定]。],
  [BLE GATT 輸送], [*C6 新規*], [§2.2。],
)

== 2.2 輸送 (BLE) とホストツール

- RP の GATT: OTA サービス `6E402001-…` の書込み専用 characteristic `6E402002-…` (Write / Write Without Response)、結果と進捗は NUS の TX notify `6E400003-…` へ行で返す (`ble_uart.gatt`、`ble_ota_send.py:39-44, 364`) [確定]。C6 でも *同じ UUID・同じ行文字列* を出せば `ble_ota_send.py` が変更なしで使える [推測: 行のパースは `parse_needseq_lines` 等 (`:105`) で文字列依存]。
- スタック: C6 は ESP-IDF の NimBLE (`components/bt/host/nimble`、Kconfig に `BT_NIMBLE_ROLE_PERIPHERAL`, `BT_NIMBLE_SM_SC`, `BT_NIMBLE_NVS_PERSIST`, `BT_NIMBLE_ATT_PREFERRED_MTU` を確認 [確定]) を推奨。Bluedroid は重い。
- MTU: `CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU=247` を設定 (既定値は未確認)。macOS 側が MTU 交換を始めるかは [未検証]。
- ペアリング: RP は接続直後に `sm_request_pairing` を出し (`ble_uart.cpp:776-777`)、*認可されていないリンクからの OTA 書込みは捨てる* (`ble_uart.cpp:468-474`) [確定]。C6 も同じ方針にする: OTA characteristic に `BLE_GATT_CHR_F_WRITE_ENC/AUTHEN` を付け、SC + bonding (NVS に保存。nvs 区画は既存: 0x9000-0xF000)。ゲート条件 (ペアリング窓の施錠など `ble_uart.cpp:173-205`) の移植範囲は未決 (§6)。
- 送出ペース: ホストは 1 チャンク (4KB) ごとに 55ms 待つ (`ble_ota_send.py:53`: erase 38ms + prog 12ms の RP 実測)。C6 の消去時間は [未検証] なので、実測してからツールの `--chunk-gap` を決める。RP は転送前に全域を事前消去して転送中の消去を避けている (ota.cpp:~590 の `ensure_erased` コメント、`g_nchunks` 設定箇所)。C6 でも `esp_ota_begin(part, total)` が全域消去するので同じ形になる (§2.3)。
- 転送中に flash を触る際の BLE への影響: RP は CYW43 の SPI を壊す問題があった (ota.hpp 内コメント)。C6 は無線が SoC 内蔵で別事情のはず [推測]。ただし flash 操作中はキャッシュが止まるため、ISR と BT コントローラのコードが IRAM にあるか (`SPI_FLASH_PLACE_FUNCTIONS_IN_IRAM`、`components/spi_flash/Kconfig:178`、`ESP_SYSTEM_CHECK_INT_LEVEL`) を設定で確認する [未検証]。

== 2.3 flash 書込み (IDF v6.1 API)

`esp_ota_ops.h` で確認 [確定]:
- `esp_ota_begin(partition, image_size, &h)`: 指定サイズ分を消去 (`OTA_WITH_SEQUENTIAL_WRITES` なら逐次消去、`:27`)。XNOR のチャンクは *再送で順不同* になるため、`esp_ota_write_with_offset(h, data, len, offset)` (`:179`) を使い、全域は先に消す (`image_size` を渡す)。`esp_ota_write` と混在させない (`:171-172` の注意)。
- v6.1 には `esp_ota_set_final_partition` (`:126-137`) と `esp_partition_copy` (`esp_partition.h:629`) があり、「staging に受けて最終区画へコピー」も IDF 側が支える。本計画では不要 (slot 方式で足りる) が、RP と同じ「staging → 本体」にしたい場合の選択肢として残す。
- 4KB 消去・16B 単位書込み (暗号化時の制約は `esp_ota_ops.c:390, 452`)。チャンク = 4KB セクタ (ota.cpp:233) で整合する。
- キャッシュ停止中のコード配置は IDF が管理 (`esp_flash` の cache guard)。自前で `commit_blast` のような IRAM 常駐コードは *不要*。ただし Shizuku が trap/タイマ割込みを持つ構成では、flash 操作中に走り得る割込みハンドラを IRAM 配置にする必要がある [推測]。

== 2.4 commit・起動切替・再起動

#table(
  columns: (1fr, 2fr, 2fr),
  align: (left, left, left),
  table.header([], [案 A: IDF OTA スロット (推奨)], [案 B: RP 同様「staging → 稼働区画へコピー」]),
  [書込み先], [非稼働側 (ota_0 / ota_1)], [staging 区画 → factory (0x10000) へ上書き],
  [commit], [`esp_ota_end` → `esp_ota_set_boot_partition` (otadata を更新)], [自前の IRAM 常駐コピー+ROM 再起動。途中の電源断で稼働区画が壊れる],
  [ロールバック], [`BOOTLOADER_APP_ROLLBACK_ENABLE` (`Kconfig.app_rollback:3`)。新側が `esp_ota_mark_app_valid_cancel_rollback()` を呼ばずに再起動すると bootloader が旧側へ戻す], [無い (復旧は ROM ダウンロードモードで書き直し)],
  [必要領域], [2 スロット + otadata (§3)], [稼働 + staging (領域は同じ 2 枚分)],
  [コスト], [partition 表変更、`esp_ota_*` が使える状態 (§3.3)], [自前の flash 低レベルコードと検証が増える],
)

[推測] 案 B に利点が出るのは「IDF の初期化なしでカーネル単独で commit したい」場合だけである。その場合も ROM の `esp_rom_spiflash_erase_sector/write/read` (`components/esp_rom/include/esp32c6/rom/spi_flash.h:241,271,287` [確定]) を使う別案 C が考えられるが、キャッシュ制御と otadata の CRC 形式を自前で持つことになり、推奨しない。

再起動は `esp_restart()` (`esp_system.h:79`)。RP は commit 前に BLE を切り 400ms 待って最終行を送り切る (ota.cpp:~816-826): C6 でも「`XNOC` 応答行を notify してから `ble_gap_terminate` → 待機 → `esp_restart`」の順にする。

= 3. パーティション表

== 3.1 現状 [確定]

`.local/build/esp32c6/partition_table/partition-table.bin` を復号: `nvs 0x9000 (0x6000)`, `phy_init 0xf000 (0x1000)`, `factory 0x10000 (1MB)`。IDF 既定の `partitions_singleapp.csv` と同一。`sdkconfig.defaults` は 3 行のみ (`firmware/esp32c6/sdkconfig.defaults:1-3`) で flash サイズ未指定のため、IDF 既定の *2MB* (`esptool_py/Kconfig.projbuild:125`)。現イメージは 160KB (verification-20261006.md:23) なので 1MB の factory に余裕がある。

== 3.2 提案 (4MB 想定)

「チップは 4MB 内蔵」は依頼文の前提で、本調査では未確認。*最初に `esptool flash-id` (または `flash_size` 表示) で実サイズを確定する*こと (ユーザー作業)。`ESPTOOLPY_FLASHSIZE_4MB` を設定し、カスタム表を使う (`PARTITION_TABLE_CUSTOM`)。

#table(
  columns: (1.2fr, 1fr, 1fr, 3fr),
  align: (left, left, left, left),
  table.header([名前], [offset], [size], [備考]),
  [nvs], [0x9000], [0x6000], [現状維持。BLE bonding 保存先 (NimBLE `NVS_PERSIST`)。],
  [phy_init], [0xf000], [0x1000], [現状維持。],
  [otadata], [0x10000], [0x2000], [新規。IDF 規約の 2 セクタ。],
  [ota_0], [0x20000], [1M], [app は 0x10000 境界 (`gen_esp32part.py:106-107`)。現 factory と同サイズ。],
  [ota_1], [0x120000], [1M], [],
  [storage (任意)], [0x220000], [残り ≈ 1.8MB], [Shizuku の flash FS 用 (RP の `FS_OFFSET`/`FS_BYTES` 相当)。後で決める。],
)

- factory を残して recovery loader にする案: factory 1M + ota_0 + ota_1 = 0x20000 + 3M = 0x320000 で 4MB に収まり、残り 0xE0000。otadata が空なら factory から起動するので、「OTA 受信専用の最小 IDF アプリ」を常駐させる (§4 の S0 イメージをそのまま使える)。XNO-ota の比較書にある「常駐 bootloader 案」と同趣旨 [推測: 同書の該当節は未精読]。決定は §6。
- 2MB だった場合: `otadata 0x10000`, `ota_0 0x20000 (0xE0000)`, `ota_1 0x100000 (0xE0000)` で 0x1E0000 まで。factory 併設は不可。カーネルが 896KB を超えて成長するなら 4MB が前提。
- ロールバック: `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` で、新イメージの「健全」判定点 (§4 の S2/S4) を決める必要がある。

= 4. 前提条件と順序

== 4.1 現状 [確定]

- `start_cpu0` を差し替え、IDF の `do_core_init` / コンストラクタ / `do_secondary_init` を飛ばしている。ヒープ・libc・PMP は未初期化。FreeRTOS スケジューラは起動しない (README.md §3 / verification-20261006.md:92)。→ `esp_flash` / `esp_partition` / `esp_ota` / NimBLE (いずれも FreeRTOS・ヒープ・`esp_timer` 等に依存。[推測]) は *現イメージ形態では使えない*。
- カーネル側は `rv32_c6` 13 関数 + `esp32_c6` 13 関数が即トラップのスタブ (`link_stubs.cpp:22-36, 44-56`)。`KERNEL::init` は呼ばれていない。
- 未解決の実機問題: 早期入口で USB-Serial/JTAG に起動行が出ず ~1.9s で USB が切れる (verification:82-101)。原因未確定 (WDT/USB 起因を切り分け中)。

== 4.2 カーネル内 OTA に必要な C6 側残作業

[推測: カーネルのソースは今回精読していない。必要集合は呼び出し元を確認して確定する]

- arch: トラップ入口 (`ecall` = syscall、タイマ、例外) と `return_stub` / `enter_thread_mode` / `current_priv` / `pend_context_switch` / `timer_oneshot|cancel|remaining` / `fault_*`。ota.cpp は `ARCH::syscall` を直接使う (ota.cpp:41-45) ので *ecall 経路が先に必要*。
- board: `init` / `time_us` (SYSTIMER) / `cycles_per_us` / `diag_printf` (USB-Serial/JTAG か UART) / `panic` / `unprivileged_floor` (PMP の U-mode 下限)。`launch_core` 等は単核のため呼ばれないはず。
- PMP: OTA オブジェクトを U-mode で走らせるなら、flash 操作は *特権側サービス* に出すしかない。`CALL_METHOD` は引数 1 個 (ota.hpp:57-65 のコメント) なので、`{op, offset, len, ptr}` 構造体へのポインタを渡す形にする。ポインタの検証 (呼出し側 PMP 領域内か) はカーネル側の仕事 [推測]。
- stream: `shizuku::stream` (SPSC、`storage<frame_t,8>`) がカーネル内で動くこと。
- BLE: IDF の NimBLE は FreeRTOS タスクとして動く。カーネルと FreeRTOS が同一 CPU・同一トラップベクタを取り合うため、*共存できるかは README.md §5 の未調査事項そのもの*。

== 4.3 BLE をどこに置くか

- 案 1 (推奨・段階的): *BLE (NimBLE) は IDF タスクとして外に置き*、受信バイトを SPSC リング (`frame_t` 単位) に積む。OTA ロジックは (a) 最初は同じ IDF アプリ内のタスク、(b) カーネルが動いたら XNO の OTA オブジェクトが同じ形のストリームから汲む。RP の「ble_uart が運ぶだけ、中身は ota が見る」(ble_uart.cpp:271 コメント) と同じ分担になる。
- 案 2: BLE ホストをカーネルオブジェクト化する。NimBLE を U-mode で動かす/移植する作業が大きく、IDF 初期化・割り込みマトリクスにも触れるため、*FreeRTOS を捨てる決定をしない限り採らない* [推測]。

= 5. 段取り (各段の完了条件と確認方法)

#table(
  columns: (0.4fr, 2.4fr, 2.6fr, 2.6fr),
  align: (left, left, left, left),
  table.header([段], [作業], [完了条件], [確認方法 (実機はユーザー許可制)]),
  [S0], [flash_target 差し込み口を `xno::ota` に切り出し、ホストでフェイク flash に対して `ota.cpp` を単体テスト。flash サイズ確定、partition 表案の確定], [ホストテストで XNOR 全チャンク受信 → CRC 一致 → commit 呼出し。RP の挙動 (行文字列) が不変], [Bazel host テスト (実機不要)。実 flash サイズは `esptool flash-id` をユーザーが実行],
  [S1], [*IDF 通常起動* (FreeRTOS 有効) の最小アプリに OTA タスク + flash_target(esp_ota) + USB-Serial/JTAG 輸送を載せる。otadata・ロールバック設定を有効化], [ota_0 からの起動 → USB で 100KB 級イメージ送信 → CRC 読戻し一致 → ota_1 へ commit → 再起動で新バージョン文字列を確認。`mark_valid` を呼ばない版でロールバックして戻ることを確認], [USB-Serial/JTAG のシリアル (既存ポート)。送信は `ble_ota_send.py` の BLE 部だけ差し替えたシリアル版 (RP 版 `ota_send.py` 相当を C6 向けに) [推測: 既存ツールの UART 経路を流用]],
  [S2], [NimBLE で OTA サービス (UUID 同一) + NUS TX notify。MTU 247、SC ペアリング + 認可ゲート、bonding を NVS へ], [Mac から `ble_ota_send.py` (無改造または `--chunk-gap` のみ調整) で S1 と同じ検証が通る。切断・再接続で NEEDSEQ 再送が成立], [実機 + bleak。消去時間・BLE 欠落率を実測し chunk-gap を決める],
  [S3], [C6 でカーネルが `init` → スケジューラ → オブジェクト → stream まで動く (別作業、link_stubs の 26 関数を実装に置換)。FreeRTOS 共存可否を決定], [selftest (Shizuku 側で閉じる) が通る], [Shizuku の selftest],
  [S4], [XNO の OTA オブジェクトをカーネル上へ。BLE リングから汲み、flash は特権サービス経由。健全判定 (init + tick + OTA 待受け復帰) 後に `mark_valid`], [S2 と同じホスト検証をカーネル配下で再実行。壊れたイメージを送ってロールバックで戻る], [実機 + bleak],
)

S1-S2 は S3 を待たずに進められる (カーネル不要) ので、「OTA でカーネル開発の焼き直しを楽にする」目的にも沿う [推測]。ただし S1 の IDF 通常起動イメージは、現在調査中の早期入口 (USB 切断) の問題とは別系統で、*同じ USB 切断が出るかは未検証* (出るなら S1 の USB 輸送の前に原因が必要)。

= 6. 未決事項 (ユーザーに決めてほしい点)

+ *IDF の OTA パーティション (案 A) で良いか*。RP 同様の「staging → 本体コピー」(案 B) を望むなら理由 (IDF 初期化なしで commit したい等) を教えてほしい。
+ *FreeRTOS を残すか* (README §5 の未調査事項)。残す = S1/S2 の IDF 版がそのまま製品経路になり得る。残さない = BLE と flash の IDF 依存を剥がす別設計が要る。
+ *factory を recovery loader として残すか* (1M 余分に使う。3 スロット構成)。
+ *flash の実サイズ* (4MB / 2MB)。S0 で確定するが、storage 領域や slot サイズに影響する。
+ *スロット 1MB で足りるか* (現イメージ 160KB。RP は圧縮を使うが C6 のイメージ増分は未見積)。
+ *ペアリング・認可ゲートの移植範囲* (RP の施錠・窓の機能を全部持つか、SC + bonding のみで始めるか)。
+ *ホストツール*: 既存 `ble_ota_send.py` を共用 (推奨)。シリアル経路 (S1) 用に別ツールを持つか、ツールにシリアルバックエンドを足すか。
+ *flash_target の置き場*: XNO リポジトリ側で ABI 化する (XNO-ota の作業ブランチと調整が要る。Shizuku-ota-test の ABI 分離途中の方向と揃えるか確認)。
