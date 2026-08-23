# 引き継ぎ書 (2026-08-23 時点)

このファイルは「次に触る人が最初に読む 1 枚」。**設計の理由は書かない** —
それは `03_porting_policy.md` の決定事項 (D1〜D43) にある。ここに書くのは
**今どこまで来ていて、何が動いていて、何が壊れていて、どこを踏むと痛いか**。

---

## 0. 最初にこれだけ

```bash
bazelisk run //firmware:flash     # 作って焼く (VS Code なら Cmd+Shift+B)
cat /dev/cu.usbmodem101           # 診断 (自己テストと [STRESS])
```

* **ビルドは Bazel のみ**。CMake は 2026-08-21 に削除した (D38)
* **USB CDC は 2 本**。番号は環境で変わるので毎回 `ls /dev/cu.usbmodem*` で確認する
  * 若い方 = **診断** (`[SELFTEST]` / `[STRESS]` / `[THERMAL]` …)
  * 大きい方 = **GDB stub** (人間が読む文字は出ない)
* 実機は **pico2_w / RP2350 / 2 コア**

### 健全なときの出力

```
[SELFTEST] call ladder done: 24 passed, 0 failed
[SELFTEST] thread ladder done: 41 passed, 0 failed
[STRESS] budget=... | selftest=93 passed/0 failed ...
```

**`93 passed / 0 failed` が現在の基準線。** ここが減ったら、まずそれを直す。

---

## 1. 何が動いているか (すべて実機で確認済み)

| 層 | 中身 | 証拠 |
|---|---|---|
| カーネル | 呼び出しフレーム / パリティ経路 / 実行権の貸し借り (クロック基準) / フォールト隔離 | call ladder 24, thread ladder 41 |
| 保護 | MPU、非特権オブジェクト、拒否のテスト | 対象自身が CONTROL を申告 (=15) |
| 多コア | 2 コアで普通のスレッドが走る | `cores seen 0x3` |
| 記憶 | 階級別空きリストで O(1) | 穴 24 個でも費用が変わらない |
| ストリーム | SPSC / 席の強制 / DMA 接続 | 4000 レコード欠落 0、`every byte arrived through DMA` |
| flash FS | XIP アドレスを返す / 追記式 / 事前消去 | 定常 2.6ms・消去 0、再起動を跨いで残る |
| アプリ | 温度履歴 10Hz × 5 分 | 揺らぎ 平均 179µs / 最悪 1,060µs |
| デバッグ | DebugMonitor で**そのスレッドだけ**止める | debug ladder、HW ブレークポイント 8 個 |

### GDB (プローブ不要) でできること

`arm-none-eabi-gdb bazel-bin/firmware/shizuku` → `target remote /dev/cu.usbmodem103`

アタッチ / シンボルとソース行 / レジスタ / メモリ / **ブレークポイント設置 →
1 回目の continue → 命中** / バックトレース / 変数の表示。

---

## 2. 壊れている・未着手のもの

### 壊れている (最優先)

**2 回目以降の `continue` と `stepi` が GDB 側で拒否される**
(`Cannot execute this command while the target is running`)。

* RSP 単体は正しい (生の probe に `+$PacketSize=3ff#2f` と返る)
* ★**原因を 2 回誤診している**。1 回目「診断の混入」、2 回目「CDC 共有」。
  どちらも直したが症状は残った。**次は推測せず、`set debug remote 1` の生ログを
  clean な GDB チャネルで取ってから触ること**
  (前回はトレース有効だと qSupported すら返らず、そこで止まった。
  トレースで遅くなると別の問題が出る可能性も含めて疑う)
* 触る場所: `modules/pico_sdk_support/objects/gdb_stub.cpp` の
  `handle_packet` / `do_continue` / `do_step`

### 未着手 (docs の Q 番号に対応)

| | 内容 |
|---|---|
| Q7 | **idle スレッド**。`sleep_us` の空回りと同じ問題で、要る道具は「T まで何もすることがない」1 つ。WFI するとサイクルカウンタが止まるので、起こし方の機構が要る |
| Q8 | **flash を非特権から好き勝手読める**。原因は API ではなく MPU 配置 (region0 に XIP 全域を `ACCESS_RO_ALL`)。直すのはオブジェクトごとの region 切り替え (§11.3)。**API の形は変えなくてよい** |
| D26 残 | `schedule()` が O(THREAD_COUNT)。ready のビットマップ + ctz で定数にできる |
| D42 残 | CDC を**ストリームとして**扱う (今は `usb_cdc_read/write` の直呼び) |
| D17 | オブジェクト集合を別リポジトリ **XNO** へ。`source/apps/` は暫定の置き場 |

---

## 3. 踏むと痛いところ (実際に踏んだものだけ)

### 焼けなくなる / 板が消える

* **USB 記述子を間違えると列挙されず、シリアルも `picotool` も効かない**。
  復旧は **BOOTSEL 押下**しかない。記述子を触るときは覚悟すること
  * 特に: **記述子で名乗ったインターフェースには必ずドライバが要る**
    (`usbd_app_driver_get_cb`)。これで一度ブリックさせた
* **1200bps タッチ**で BOOTSEL へ落とせることがある:
  `python3 -c "import serial,time;s=serial.Serial('/dev/cu.usbmodemXXX',1200);time.sleep(0.3);s.dtr=False;s.close()"`
* **ホスト側でポートを掴んだままのプロセス** (`cat` や gdb) があると
  `picotool` が失敗する。**焼く前に必ず殺す**
* `tud_task()` は低優先度 IRQ で回っているので、**スレッドが固まっても USB は生きる**。
  「焼けない = ファームが固まった」と決めつけないこと (一度誤診した)

### 計測が嘘をつく

* **DEMCR に書いたら `dsb + isb`**。無いと「次の命令」ではなく数命令あとで止まる
* **例外ハンドラが書く記録は acquire ロードで読む**。素直に読むとレジスタに載る
* **`asm volatile` に `"memory"` を付ける**。無いと前後の読みが吊り上がる
* **窓の最大値だけ見ない**。`led_write` の最大は「プリエンプトされた時間」を含む。
  最小 (誰にも邪魔されなかった 1 回) が操作そのものの費用
* **オブジェクト番号の衝突は無音**。今はビルドが振るので起きないが、
  `objects.list` に足すのを忘れて手で書かないこと

### 測るときの作法

条件は**交互に**回す。1 回の計測で優劣を判断しない。分布が重なるなら「差なし」。
64KB のブロック消去は**境界に揃えないと効かない** (揃えず測って 6.7 倍の差を
見落としかけた)。

---

## 4. どこに何があるか

```
internal_headers/shizuku/     カーネルの語彙 (kernel_abi / object_api / stream / templates)
source/kernel/                機構だけ (dispatch / thread / init)
source/kernel_object/         方針 (handler = svc の配り口, memory = O(1) arena)
source/apps/                  アプリ (thermal)。いずれ XNO へ
source/selftest/              梯子。**ここが仕様書**
modules/pico_sdk_support/     board / arch / ペリフェラル / flash FS / USB / GDB stub
configs/                      型注入 (config.hpp) と CYW43 クロックの導出
tools/gen_object_ids.py       オブジェクト番号を振る (各 objects.list を読む)
docs/03_porting_policy.md     決定 D1〜D43 と未決 Q1〜Q8 ← **設計の理由はここ**
```

**自己テストが仕様書**。何かを変えたら、まず対応する ladder が落ちるかを見る。
落ちないなら、その変更は**確かめられていない**。

---

## 5. この repo の作法 (守られていないと浮く)

* **測って決める**。「速い/遅い」ではなく内訳を出す (消去 89% のような形で)
* **許可のテストだけでは証拠にならない**。拒否のテストを必ず書く
* **無音の失敗を作らない**。戻り値を見ない呼び出し側が必ず居るので、
  気づけるかどうかを行儀に賭けない
* **「気をつける」で守らない**。型・機構・ビルドで守れないか先に考える
* **確かめていないことを「できる」と書かない**。未検証なら未検証と書く
* コメントは**なぜそうしたか**を書く。とくに**踏んだ罠は必ず残す**
* 日本語。コミットメッセージも日本語

---

## 6. 次のセッションへの推奨プロンプト

そのまま貼れる形。**やることを 1 つに絞る**のが要点で、この repo は
「実機で測って直す」往復が長いので、複数を並行させると必ず取りこぼす。

### (A) 壊れているものを直す — 最優先

```
Shizuku の続き。docs/05_handoff.md と docs/03_porting_policy.md を読んでから始めて。

GDB stub の「2 回目以降の continue / stepi が Cannot execute this command while
the target is running になる」を直したい。原因は 2 回誤診しているので、まず
推測せずに証拠を取ること:
  1. GDB を clean な CDC (大きい方の usbmodem) に繋いで set debug remote 1 の
     生ログを取る。トレースを有効にすると qSupported すら返らなくなる現象も
     出ているので、それ自体も観測対象にしていい
  2. 生ログから、GDB がどのパケットの後に「running」と判断しているかを特定する
  3. 直したら、ブレークポイント → continue → 命中 → continue → 命中 →
     stepi が通ることを実機で確認する

焼く前に cat や gdb がポートを掴んでいないか確認すること。
```

### (B) 次の機能へ進む

```
Shizuku の続き。docs/05_handoff.md と docs/03_porting_policy.md を読んでから始めて。

Q8 (非特権から flash を好き勝手読める) を潰したい。原因は API ではなく MPU 配置で、
直すのはオブジェクトごとの region 切り替え (§11.3)。call_request に region を
載せる ABI 変更が要る。API の形は変えなくてよいはず (ディスクリプタの
base + capacity がそのまま権限になる)。

非特権オブジェクトが「渡された extent の外を読もうとしたら落ちる」ことを
拒否のテストとして書いて、実機で確かめるところまでやって。
```

### (C) 片付け

```
Shizuku の続き。docs/05_handoff.md と docs/03_porting_policy.md を読んでから始めて。

Q7 (idle スレッドをどうするか) を決めて実装したい。sleep_us の空回りと同じ問題で、
要る道具は「T まで何もすることがない」1 つ。WFI するとサイクルカウンタが止まるので
起こし方の機構が要る。board のアラームをもう 1 本使うか、grant のタイマの意味を
広げるかの二択で、後者は貸し借りの契約の意味を変えるので判断が要る。
どちらが良いか根拠つきで提案してから実装して。
```

### 共通の注意 (プロンプトに足すと効く)

```
・実機で確かめていないことを「できる」と書かないこと
・変更したら対応する selftest ladder を回して、93 passed / 0 failed が
  減っていないか見ること
・踏んだ罠はコメントと docs に残すこと
```
