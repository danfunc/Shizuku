= カーネル所有 stream

== 目的

`stream.hpp` の共有 descriptor は隔離された producer/consumer が形状・番号を
書き換えられる。特権側 DMA がその値を再読するため、DMA を任意番地アクセスの
代理にできる。stream の形状、wr/rd、席、リング記憶を kernel object の表へ移し、
利用側には世代付き ID だけを渡す。

== 契約

- `STREAM_CREATE` は生成者を owner とし、`rec_size * capacity` のリング領域を
  object arena から確保する。積の overflow、0 サイズ、0 容量、未知 flags は拒否。
- `STREAM_PUSH_N` / `STREAM_POP_N` はバッチ単位の svc。1 件版は薄い wrapper。
  lossless 満杯時は push を押し戻す。上書き mode の追越し数は pop 結果 `lost` に
 返し、順序と再読検査は従来どおりにする。1 回の batch count は capacity で制限し、
 返された件数だけ進めて残りは呼び出し側が再送する。
- `STREAM_BIND` は SPSC の席を ID の所有者 object に結び付ける。席がない object
  からの push/pop は `SEAT_TAKEN`。特権ドライバには svc を介さない kernel 内部
  `stream_bind_kernel` / `stream_push_kernel` 経路を用意する。
- `STREAM_CONNECT` の DMA pump はカーネル保管形状だけを読む。カウンタ差が容量を
  超えた場合は room=0、転送 count は容量と連続区間の範囲へ切り詰める。接続中の
  destroy は `BUSY`。DISCONNECT は従来どおり非同期で、完了後に destroy する。
- ID の下位 4 bit は表 slot、上位は generation。destroy ごとに generation を進め、
  古い ID は `BAD_STREAM`。generation が尽きた slot は再利用しない。
- owner だけが destroy できる。DMA 接続中は拒否する。未接続なら他 object が席を
  持っていても席を解放してリング領域を owner へ返す。
- object 終了に伴う自動片付けは今回対象外。object destroy の契機が定まったら、
  DISCONNECT 完了待ち → destroy と、他 object が保持する席の解除を接続する。
  現行は object destroy がなく、1 object に複数 thread を持てる。単一 thread の
  終了を object 終了と扱うと、残る thread の stream を壊す。`OBJECT_REPLACE` 中に
  旧 object の thread が動作中の場合の扱いも未決。
- flash read は `stream::extent { address, bytes, sequence }` のレコードを流す。
  kernel は extent の中身を検証せず、コピー対象データとして扱う。consumer の
  address access は MPU/隔離規則に委ねる (未決)。extent が指す XIP はリング記憶と
  別物で、D21 の「写さない」を保つ。
- 詰め替えは中継 object (D37)、flash 書き込みは byte/chunk stream (D34)、MP_PROD
  は復活させない (D46)。D33 のコピー中追越し検査も保持する。

== ABI と移行

現行使用済み ID は 0–27。stream create/open/bind/connect/disconnect は 19–22, 26。
28–31 を `STREAM_DESTROY`, `STREAM_PUSH_N`, `STREAM_POP_N`, `STREAM_AVAILABLE` に割り当てる。
create は `(rec_size, capacity, flags)`、batch は `(stream_id, request*)`。
request は buffer pointer, count, lost (pop) を含む。request と batch buffer は
特権 handler が任意番地へ copy する deputy にならないよう、現在 thread の stack 内に
限定する。svc 返値は転送件数。

番号の定義・dispatch は `internal_headers/shizuku/object_api.hpp` と
`source/kernel_object/handler.cpp`。利用者は
`modules/pico_sdk_support/objects/{ble_uart,ota,flash_fs}.cpp`,
`modules/pico_sdk_support/rp2350/objects/gdb_stub.cpp`,
`source/selftest/{stream_ladder,flash_stream}.cpp`。

移行順: (1) kernel table と世代付き ID、(2) create/bind/connect/destroy、(3) batch
push/pop と C++ handle、(4) flash_fs open/close と extent consumer、(5) BLE/OTA/GDB
および selftest、(6) 両 board profile build。

== 費用測定

`source/selftest/call_cost.cpp` の DWT CYCCNT 計測と同じ noinline 関数ポインタ枠で
空操作、既存 svc、`push_n`/`pop_n` (1, 4, 16 records) を測る。各値から空枠の最小値を
引き、min/mean と batch あたり・record あたりを記録する。実機でのみ結果が得られる。
期待値は batch 内 record 数に依らず svc 往復回数が 1 回ずつであること。

== 回帰確認と未決

selftest は lossless 押し戻し、lost、席拒否、DMA 接続、範囲切詰め、他者 destroy 拒否、
接続中 destroy 拒否、destroy 後 stale ID 拒否を確認する。`flash_stream` は extent を
受け取り XIP bytes を読む。flash write open/close は stream create/destroy を対にする。

未決: object 終了/置換の確定的な寿命契機、extent の consumer access grant、実機での
svc cycle 値。現行に object destroy はなく複数 thread が同一 object を使えるため、
thread exit で object 所有 stream を破棄しない。object destroy か OBJECT_REPLACE の
どちらを契機とするか、そのとき複数 thread が動作中なら拒否/停止/待機のどれにするか
を決めた後、DISCONNECT 完了待ち→destroy と、他 object が持つ席の解除を結ぶ。

== 将来項目 (今回は未実装)

- Atomic svc: 1 ID と op (`CAS`/`SWAP`/`FETCH_ADD`) を受け、`r0=op, r1=addr,
  r2=a, r3=b`、戻り `r0=old value`。不正番地は fault。kernel は ISA 共通の
  `ARCH::cas32` ループで実装し、ユーザー ABI header は
  `__GCC_HAVE_SYNC_COMPARE_AND_SWAP_4` で `if constexpr` を選ぶ。RP2040 の store
  も svc 経由にする。
- Wait/wake: futex 型。値比較と待ち登録を wake 側と同じ wait queue lock の下で行い、
  比較後・登録前に wake が抜ける lost wakeup を防ぐ。
- 大きい buffer のコピーなし受け渡し: byte copy をせず、記憶の持ち主を送信側から
  受信側へ付け替える API として設計する。
