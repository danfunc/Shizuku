#set page(paper: "a4", margin: (x: 1.8cm, y: 1.8cm))
#set text(size: 9pt)
#set par(justify: true, leading: 0.62em)
#set table(inset: (x: 4pt, y: 3pt), stroke: 0.5pt + luma(130))
#show table.cell: set par(justify: false)

#align(center)[
  #text(size: 15pt, weight: "bold")[RP2040 / RP2350 OTA commit 再試行設計] \
  #text(size: 9pt)[固定 stage3 と単一アプリ領域 | 設計案・実装/実機未検証 | 2026-10-14]
]

*この文書の状態*: commit 再試行の設計案。製品コード、ビルド設定、テストは変更していない。実機動作、電断原子性、提案配置の実リンクサイズは未確認である。`[確定]` は読取ソースまたは一次資料で確認した事実、`[提案]` は実装前の設計、`[Unknown]` は証拠不足を表す。

= 1. 推奨設計

RP2040/Pico W と RP2350/Pico 2 W の既存方式「BLE で staging に全像を受け、検証後に本体へコピー」を維持する。A/B スロットは採用しない。flash 先頭に OTA 対象外の固定 stage3 と journal を置き、`staged → committing(n) → done` を永続記録する。`staged` と `committing` の間は受信像を消さず、`committing` で再起動したらコピーを先頭から再試行する。

アプリ本体を上書きする以上、電断中に旧アプリへ戻す設計ではない。コピーが完了するまで stage3 のみが起動し、完了後に本体の全体検査が通った場合だけ `done` をsealして新アプリへ進む。stage3/journal の破損または再試行上限到達では本体を推測起動せず、USB BOOTSEL 復旧へ入る。

#table(
  columns: (1.2fr, 3.2fr, 3.2fr), align: left,
  table.header([対象], [RP2040 / Pico W], [RP2350 / Pico 2 W]),
  [ROM 起動入口], [ROM がflash先頭256 BをSRAMへ読み、末尾CRC32を検査してboot2を実行。boot2がVTORを`XIP_BASE+0x100`にし、vectorからstage3へ入る。], [固定boot2をROM前提にしない。ROMが先頭4 KiBのblock loopからIMAGE_DEFを発見してstage3入口を選ぶ。現SDKではboot2はoptionalなXIP初期化である。],
  [OTA対象外領域], [0x00000..0x0A000。boot2 256 B、stage3枠、journal 2 sectors。], [0x00000..0x0A000。stage3にROM用block loop/IMAGE_DEFを含め、journal 2 sectors。],
  [アプリ開始], [0x0000A000 (XIP 0x1000A000)。vector tableとアプリ本体。], [0x0000A000 (XIP 0x1000A000)。Arm imageをstage3が直接起動。],
)

RP2350 の対象は現在の Arm image とする。RP2350 RISC-V、署名/rollback OTP設定、複数IMAGE_DEF・partition-tableを含む起動選択は対象外であり、混在ビルドを許可する根拠はない。

= 2. 根拠となる現状

- `[確定]` 現行 OTA は staged CRC とvector値を検査した後、`commit_blast` が本体のsectorを先頭から消去/書込みして再起動する。RP2040 vector検査位置はimage先頭+256 B、RP2350は+0 Bである。OTAコードは実行可能なA/B切替や再起動後commit回復を実装していない (`modules/pico_sdk_support/objects/ota.cpp:207-260, 878-963`)。
- `[確定]` linker ASSERTは本体上限をRP2040 512 KiB、RP2350 1 MiBに制限する。現 `flash_map.hpp` はRP2040で本体512 KiB・staging512 KiBを確保し、その間がFS。RP2350では本体1 MiB・FS1 MiBでstagingはBT予約直前までである (`modules/pico_sdk_support/objects/internal_headers/shizuku/objects/flash_layout_config.hpp:14-22`, `flash_map.hpp:33-39, 65-108`, `firmware/rp2040_flash_limit.ld:1-6`, `firmware/rp2350_flash_limit.ld:1-5`)。
- `[確定]` W boardのBTstack予約はSDKの`PICO_FLASH_BANK_STORAGE_OFFSET`から算出。RP2040 Wは末尾2 sectors=8 KiB、RP2350 A2 Wは末尾3 sectors=12 KiB (うち最終sectorはE10 workaround)。flash serviceはcore間排他、割込み停止、他core停止を行う (`flash_map.hpp:33-51`, `modules/pico_sdk_support/objects/flash_ops.cpp:14-43, 55-92`)。
- `[訂正]` 以前の「W app約0.9 MiB」は894,464-byte UF2コンテナのサイズで、実像サイズではない。2026-10-13 build artifact `/private/tmp/shizuku-selftest-owner/rp2040-w.uf2` を `picotool info -a /private/tmp/shizuku-selftest-owner/rp2040-w.uf2` で測ると、binary start `0x10000000`、end `0x1006d294`、像の範囲は447,124 bytes (436.64 KiB)。同じbuildのELF `/private/tmp/shizuku-selftest-owner/rp2040-w-elf.elf` は `arm-none-eabi-size -A /private/tmp/shizuku-selftest-owner/rp2040-w-elf.elf` でflash image関連section (`.boot2`, `.text`, `.rodata`, `.ARM.exidx`, `.binary_info`, `.data`) の合計447,120 bytes。RP2040 Pico Wの送信app上限472 KiBに対し36,204 bytes (約35.36 KiB) の余裕があり、測定した現像は上限内。この測定は現行配置のimageであり、提案する0xA000配置・fixed prefix込みの将来リンク像が472 KiB以内になることは別途計測が必要。FS 1016 KiBを維持した容量表の配置計算自体は変わらない。
- `[確定]` XNO-otaの比較資料は常駐bootloader+再開可能copyを「設計案」として記録するが、journal原子性・電断試験は未決である。RP2350 boot2をRP2040同様と断定せず、A/B/ATRANSは検証済み案ではない (`/Users/ishigakiyua/github/XNO-ota/docs/ota-comparison.typ:390-421`)。
- `[確定]` XNO `protocol` は外部flash操作をcallbacksへ分離し、XNOC前にreadbackとvalidateを呼ぶ設計だが、`activate`の永続再試行/flash layoutはport側責務である (`/Users/ishigakiyua/github/XNO-ota/objects/ota/protocol.hpp:41-52`, `protocol.cpp:175-193`)。

`tools/ble_ota_send.py` は画像を丸ごと読みCRCを計算し、XNOC時は「commit受理行」を待つ。ただし再起動後の新build識別は別途必要と明記しており、完了確認は別途設計する (`tools/ble_ota_send.py:543-547, 840-863`)。

= 3. 配置案と容量

sectorは4 KiB、pageは256 Bとする。以下は2 MiB/4 MiB実装の提案計算で、SDK/実機からの実測値ではない。全範囲は半開区間、flash offset表記である。

#table(
  columns: (1.2fr, 1.25fr, 1.25fr, 2.4fr, 2.1fr), align: left,
  table.header([領域], [Pico W 2 MiB], [Pico 2 W 4 MiB], [用途], [容量]),
  [固定 prefix], [0x000000–0x008000], [0x000000–0x008000], [stage3用ROM/entry block+コード枠。OTAで書かない。], [32 KiB],
  [journal], [0x008000–0x00A000], [0x008000–0x00A000], [2 × 4 KiB erase sector。別sectorにcopy-on-write。], [8 KiB],
  [アプリ領域], [0x00A000–0x080000], [0x00A000–0x100000], [stage3が直接起動する単一アプリ。], [472 / 984 KiB],
  [FS], [0x080000–0x17E000], [0x100000–0x200000], [RP2040は現FS範囲。RP2350も現FS範囲。], [1016 / 1024 KiB],
  [staging], [0x17E000–0x1FE000], [0x200000–0x3FD000], [受信header+prefix+新アプリをdoneまで保持。], [512 / 2036 KiB],
  [BTstack], [0x1FE000–0x200000], [0x3FD000–0x400000], [SDK tail reservation。], [8 / 12 KiB],
)

staging payload形式は`4 KiB manifest + 32 KiB fixed-prefix snapshot + app bytes`とする。mutableなjournal 8 KiBはstaging snapshotに含めない。manifestは4 KiB領域を確保し、chip/board/layout/ABI、app開始offset/length、stage3 version+hash、app CRC32+SHA-256、protocol/version、header CRCを含める。送信app上限は`min(app領域, staging−4KiB−32KiB)`で、Pico Wは*472 KiB*、Pico 2 Wは*984 KiB*。Pico W staging使用は4+32+472=508 KiBで4 KiB余り、Pico 2 Wは4+32+984=1020 KiBで1016 KiB余る。flash全体の固定占有にはprefix32 KiBとjournal8 KiBの両方を計上した。圧縮後受信サイズと展開後app長の双方を上限検査する。

RP2040の既存512 KiB firmware budgetからfixed prefix32 KiBとjournal8 KiBを除いた送信app枠は472 KiB。RP2350は現1 MiB firmware budgetから同じ40 KiBを除き984 KiB。現行Pico W像の実測447,124 bytes (436.64 KiB) はRP2040送信app上限より36,204 bytes (約35.36 KiB) 小さい。従って測定済み現像を基準にするとRP2040の容量不足という結論にはならない。ただし新しい0xA000配置のリンク像・prefixを含むstage3との合計は未測定であり、移行前の容量gateで確認する。FS縮小/退避/移行は測定で容量不足が判明した場合に別途判断し、配置を黙って変更しない。

2MiB/4MiBの物理容量、BT tail、flash sector/pageを実ボードとpico-sdk board定義から再確認する。構成時ASSERTは`stage3 + journal + app ≤ firmware window`、`manifest + prefix + app ≤ staging`、`FS end ≤ staging start`、`staging end ≤ PICO_FLASH_BANK_STORAGE_OFFSET`を各profileに要求する。Pico WのSDK経由TLVをRAMへ切り替える案はペアリング鍵の永続性を変えるため本設計では採らない。

= 4. stage3と直接起動

stage3はreset直後にだけ動く最小boot/commit環境で、BLE、FS、kernel、C++ constructor、ヒープ、割込みhandlerに依存しない。リンク時にstage3のtext、rodata/literal pool、flash driver、CRC/hash、copy loop、vector、stackと使用する関数テーブルがすべてRAM上にあることをASSERTとmap検査で証明する。main flash変更中はcore1停止、DMA/peripheral request停止、割込み禁止、flash XIP参照禁止。watchdogを安全なtimeoutに設定し、sector境界で進捗記録とfeed可能にする。flash API/ROM helperを使う場合も、その呼出しtree全体がXIP停止下で安全であることをlink/map/disassemblyで示す。

RP2040: 先頭256 BはSDKのboot2 format/CRCを保持し、256 B地点の固定vector/MSP/entryからstage3へ入る。stage3がアプリ起動時に`SCB->VTOR = XIP_BASE + 0xA000`、最初のwordをSRAM範囲内の初期MSP、2番目をThumb bit付きかつapp領域内のreset handlerとして検証する。MSP設定後にreset handlerへbranchする。RP2040 SDKのexit helperは通常`XIP_BASE+0x100`からvectorを読み`MSP`設定後`bx`する (`pico-sdk 2.2.0 src/rp2040/boot_stage2/asminclude/boot2_helpers/exit_from_boot2.S:12-26`)。OTA appを0xA000に置くには同等の専用stage3 handoffを設計し、既定helperへ依存しない。

RP2350: ROM用IMAGE_DEF/block loopとENTRY_POINT/VECTOR_TABLEをstage3 prefix内に置き、stage3がcold bootでROMに選ばれることを保証する。SDK 2.2.0のdefault linker scriptはIMAGE_DEFからentryを選び、boot2はoptional XIP setupである。appは`0xA000`リンクのArm imageとしてstage3が直接jumpし、app側のIMAGE_DEF/block loopをflash先頭のROM discovery範囲に出さない。stage3 block loopの有効性と、ROMがapp/staging側IMAGE_DEFをboot対象に選ばないことをSDK `picotool info` 相当および実ROMで確認する。RISC-V entry、署名/rollback、image transformationはサポート対象外。RP2350 boot2 checksumをRP2040 CRCと同一視しない。SDK 2.2.0のRP2350 `pad_checksum`は「boot2はcrt0から呼ばれ、bootromはCRCではなくIMAGE_DEFを確認」と記す。

OTA app側からの`watchdog_reboot`/ROM rebootはstage3を経由するcold flash bootに固定し、watchdog scratchによるSRAM direct-PC bootやapp reset handlerへのdirect-PC resetを無効化する。RP2040 SDKのwatchdog helperはPCがnonzeroならscratchにdirect-PC用magicを設定し、PC=0なら通常boot用にmagicをclearする (`pico-sdk 2.2.0 src/rp2_common/hardware_watchdog/watchdog.c:98-109`)。RP2350では`REBOOT2_FLAG_REBOOT_TYPE_NORMAL`を使い、`REBOOT2_FLAG_REBOOT_TYPE_PC_SP`/direct-entry系flagとapp-address引数を禁止する (`pico_bootrom/include/pico/bootrom.h:456-504`)。`REBOOT2_FLAG_REBOOT_TYPE_FLASH_UPDATE`やslot選択がstage3以外を優先しないこともROM/API仕様と実機で確認する。reset直後にstage3を迂回できないことを両chipのpower-on resetとwatchdog resetで確認する。stage3の自己更新はOTAでは拒否し、stage3/boot prefix更新はUSB BOOTSEL経由の完全なprovision imageだけに許す。

= 5. stagingとjournalの永続状態

manifestとpayloadを受信した後、全staging readback CRC/hash、chip/layout/offset/length、stage3 prefix byte-for-byte一致、app vector/entry検査を通す。CRC32は転送破損検出、SHA-256は識別・readback照合用であり、どちらも署名/認証ではない。完全像が有効になるまでjournalへ`STAGED`をsealしない。

#table(
  columns: (1.2fr, 2.2fr, 3.2fr), align: left,
  table.header([状態], [永続化順序], [reset後の動作]),
  [done(g)], [現在稼働中appのgeneration/hash/lengthがseal済み。], [app領域を照合し一致時だけ起動。受信中はこのdoneを保持。],
  [staged(g+1)], [staging消去/受信/manifestとprefix/app読戻し検証 → 旧doneを含むstaged recordをseal。], [新像を無視し、旧done appを起動。staging受信が途中でも旧doneは有効。],
  [committing(g+1,n)], [staging再検証 → committing recordを別journal pageへ書く → seal page → readback全検査。ここまで成功する前に本体を変更しない。], [新像を再検証してappコピーを先頭から再試行。旧appへ戻らない。],
  [done(g+1)], [全app sector erase/program/readback後にapp全体hash/CRC照合 → done recordをseal/readback。], [done新appだけ起動。旧done保持は次回journal GCまで。],
)

headerは二段階(1 page body + 1 page seal)で、generation/seq、state、layout/chip、manifest hash、old done identity、copy bytes、CRCを含める。sealがmagic+generation+body CRCに一致しないrecordは無効。CRCを通らない、未知version、両sector損傷、sequence ambiguityはすべてrecoveryへ倒し、appを推測起動しない。

recordは2つの4 KiB journal sectorにappendする。空きがある側へbody→sealの順で書き、readback成功した最新recordを有効にする。GCは他方をeraseし、現在のlatest stateを新sectorへbody→sealで複製し、readback成功を確認してから旧sectorをeraseする。旧latest recordを消す前にnew latestが検査済みであることが電断不変条件。進捗`n`は診断用であり、初版のコピーは毎回sector 0から再実行する。sectorごとにerase→program→readback。`done`を書けないresetは再びcommitになる。

新しい転送開始時は旧`done`を残したままstagingを消す。complete stagingを検証して初めて`staged`、XNOC等で承認されて初めて`committing`を書く。staged状態なら旧appがまだ正常に起動可能。committingをsealした後は旧app identityへ巻き戻さず、stage3が同一staging像から本体コピーを再試行する。`done`検査値は少なくとも現在のdoneとstaged/committing recordに保持し、GC中に唯一の有効コピーを消さない。

USB初回導入はprovision UF2でstage3+初期app+有効な`done(0)` recordを同時に配置する。valid doneが無い個体をfactory appと推測して起動しない。両journal sectorが壊れた場合、stage3はBLE受付を含む通常起動へ進まずBOOTSEL/USBへ移る。

= 6. 異常時動作と安全弁

- size/layout/chip/ABI不一致、short header、staging CRC/hash不一致、prefix mismatch、RP2040 boot2 CRC不一致、無効SP/PC/Thumb、範囲overlap、sector/page alignment違反はcommit拒否。stagingは消さず、現在`done`なら旧appへ戻る。
- `committing` durable write/readback前の失敗では本体を書かない。stage3のflash erase/program/readback失敗後は本体起動禁止。最大3回の自動コピー試行後はrecovery待機を提案する (回数/flash wearは未決の運用値)。次回bootでstagingとcommittingがvalidなら先頭から再試行。
- app起動後のwatchdog resetは毎回stage3を経る。health test/rollbackはないため、done後に新appが即faultする場合はBOOTSEL/USB再provisionが必要。committing状態から新appのself-test合格を旧appへ戻す仕組みは本案に含めない。
- BLE/CRC32/CRC16/SHA-256検査だけでは送信元を認証しない。現設計と同じくpairing gateに依存する。信頼できる配布物を制限する必要があれば署名鍵・検証・鍵管理を別設計する。

Fixed stage3/recordの両方をOTA対象に含めない。RP2040 boot2 CRC計算は固定256 Bの先頭252 Bをnon-reflected polynomial 0x04C11DB7, init 0xFFFFFFFF, xorout 0、little-endian末尾語とし、SDKの`pad_checksum`のbit-reflection変換を再利用/複製するときはgolden vectorを作る。RP2350 SDKのRP2350専用`pad_checksum`はCRCを付加せず、ROM向けIMAGE_DEFを要求する。prefix validationはサイズだけでなく、実flash固定prefix 32 KiB全byte比較とSHA-256/versionを行う。mutable journal 8 KiBは比較対象に含めない。

= 7. 移行と検証計画

1. 対象boardの2/4 MiB、SDK BT offsets、flash erase/page geometry、RP2350 silicon revision、RP2350 Arm-only imageのboot metadataを確定する。現SDK/ELF/map/artifactの最大byte数、RP2040で`app ≤472 KiB`が成立するかを計測し、FS使用量と併せて容量gateを判定する。
2. USB provision imageにstage3固定prefix、2 journal sector初期化、初期app、`done(0)` recordを含め、BOOTSELから初回導入する。cold reset/watchdog resetの両方でstage3からappへ進むことを確認する。
3. host側はfull prefix snapshot + manifest + appを送信。未知layout/chip、短いpayload、改ざんprefix、無効SP/PC/entry、CRC/hash誤りをcommit拒否する。正常な1像をstageする。
4. journalのbody/seal、STAGED→COMMITTING→DONE各program/erase前後、各journal GC操作前後、各app sector erase/program後に電源断またはresetを注入する。各再起動で本体がdoneまたはstage3に限られ、途中appを実行しないこと、staging CRCが保たれればcommitがsector0から再収束することを確認する。
5. 1st/2nd/3rd auto retry、retry上限、journal両壊れ、staging破損、app readback失敗でBOOTSELへfail-closedすることを確認する。完了後は新app build-idをUSB CDCで照合し、staging eraseは新doneを読み戻した後だけ許す。

設計段階のassert一覧 (テスト未作成): (a) `STAGED`中のresetでold done appのみ起動、(b) `COMMITTING`のseal readback前に本体write callが0、(c) valid COMMITTING resetごとにcopy offsetが0、(d) `DONE`前はapp entryを呼ばない、(e) GCで旧latest recordをeraseする時点で新recordがvalid、(f) 不一致chip/prefix/vector/layoutは本体未変更、(g) 両journal損傷はapp起動でなくUSB recovery、(h) app全体readback hash一致後にだけdoneをseal。

= 8. 未確定事項

#table(
  columns: (1.5fr, 3.5fr, 2.5fr), align: left,
  table.header([項目], [Unknown / 必要な一次証拠], [関係する根拠]),
  [新配置の実image bytes], [2026-10-13現行Pico W像は447,124 bytes (`picotool info -a`でUF2のbinary start/end、ELF `arm-none-eabi-size -A`でflash section合計)。0xA000配置後のappとprefix込みサイズは未測定。], [`/private/tmp/shizuku-selftest-owner/rp2040-w.uf2`, `/private/tmp/shizuku-selftest-owner/rp2040-w-elf.elf`; `firmware/rp*flash_limit.ld:1-6` は上限のみ。],
  [FSの実使用量], [flash_fs watermark/最大保存量、既存データの縮退・移行手順。FSを借りる場合は旧データ破壊とmount lockoutを具体化する。], [`flash_map.hpp:85-102` は区画だけ。],
  [RP2350 ROM入口/署名], [現Bazel profileでのARM/RISC-V、silicon revision、ROM IMAGE_DEF選択・secure boot/rollback OTP設定。RP2350 A2/E10と将来revision差。], [SDK 2.2.0 scriptとRP2350 datasheet §5.1.4-5.1.5。],
  [power-loss semantics], [各flash vendorのerase/program interruption後のreadback状態、journal seal pageの部分書込確率/原子単位、brownout範囲。CRCはatomicity証明ではない。], [`flash_ops.cpp:62-92` はAPI呼出しだけ。],
  [stage3 RAM closure], [各chipでのRAM size/map、SDK ROM flash helper依存、割込み/DMA/core1 shutdown経路、watchdog feedを含むXIP外実行証明。], [`flash_ops.cpp:14-43` は現app service。stage3には未実装。],
  [USB recovery], [RP2040/RP2350 BOOTSEL/ROM APIからstage3+app+done recordを再provisionする実機手順と破損状態別の可否。], [RP2040 datasheet §2.8.1はUSB UF2 bootを説明。],
)

ここまでの作業で製品テストは作成/実行していない。上層が読むべきテストコードのfile:lineは*該当なし*。上の8項目は将来の設計assertであり、テスト結果や電断耐性の証明ではない。

= 9. 一次資料

- RP2040 Datasheet §2.8.1, §2.8.1.3.1: ROMが先頭256 Bを読みCRC確認、boot2がvector tableへhandoff。checksum polynomial/reflect/init/xoroutは同節。 #link("https://datasheets.raspberrypi.com/rp2040/rp2040-datasheet.pdf")[PDF]
- RP2350 Datasheet §5.1.4–5.1.5: IMAGE_DEF、block validation、flash bootでは最初の4 KiBからblock loopを探索。#link("https://pip-assets.raspberrypi.com/categories/1214-rp2350/documents/RP-008373-DS-2-rp2350-datasheet.pdf?disposition=inline")[PDF]
- Pico SDK 2.2.0 source (repository `git describe --tags` = `2.2.0`): `src/rp2040/boot_stage2/pad_checksum:20-43`; `src/rp2350/boot_stage2/pad_checksum:18-41`; `src/rp2_common/pico_crt0/rp2350/memmap_default.ld:38-44,96-105`; `src/rp2_common/pico_crt0/embedded_start_block.inc.S:91-117`; `src/rp2040/boot_stage2/asminclude/boot2_helpers/exit_from_boot2.S:12-26`; `src/rp2_common/hardware_watchdog/watchdog.c:92-113`; `src/rp2_common/pico_bootrom/include/pico/bootrom.h:456-504`; `src/rp2_common/pico_flash/flash.c:201-205` and `pico_flash/include/pico/flash.h:87-115`. #link("https://github.com/raspberrypi/pico-sdk/tree/2.2.0")[raspberrypi/pico-sdk 2.2.0]
- Raspberry Pi pico-examples universal binary note explains RP2040 boot2 versus RP2350 block-loop selection; used only as secondary clarification to datasheet/SDK. #link("https://github.com/raspberrypi/pico-examples/blob/master/universal/README.md")[universal README]

= 10. 変更・検証境界

編集許可は本ファイルのみ。設計書compileは内容/Typst構文の確認であり、製品build、OTA、journal原子性、stage3 RAM safety、デバイス挙動の検証ではない。実機の電断/reset注入は計画のみで未実施。commitは行わない。
