# 引き継ぎ書 (2026-08-24 時点)

このファイルは「次に触る人が最初に読む 1 枚」。**設計の理由は書かない** —
それは `03_porting_policy.md` の決定事項 (D1〜D55) にある。ここに書くのは
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
| 保護 | MPU、非特権オブジェクト、拒否のテスト、`GRANT_REGION` (Q8: flash extent の動的開示) | 対象自身が CONTROL を申告 (=15)、開示外は落ちる・開示内は読める |
| 多コア | 2 コアで普通のスレッドが走る | `cores seen 0x3` |
| 記憶 | 階級別空きリストで O(1) | 穴 24 個でも費用が変わらない |
| ストリーム | SPSC / 席の強制 / DMA 接続 | 4000 レコード欠落 0、`every byte arrived through DMA` |
| flash FS | XIP アドレスを返す / 追記式 / 事前消去 | 定常 2.6ms・消去 0、再起動を跨いで残る |
| アプリ | 温度履歴 10Hz × 5 分 | 揺らぎ 平均 179µs / 最悪 1,060µs |
| デバッグ | DebugMonitor で**そのスレッドだけ**止める / **GDB に全スレッドが見え、切り替えられる** (D53 non-stop。走っているものは `(running)`) / GDB の continue・stepi・detach が何度でも通る / **走行中の Ctrl-C (一時停止) が効く** / **両コアにブレークポイントが載る** | debug ladder、HW ブレークポイント 8 個、LED を止めて resume するデモ、0x03 → T02 |

### VS Code の Debug Console の使い方 (実機で確認済み)

`-exec ` を頭に付けると GDB のコマンドがそのまま打てる (例: `-exec info threads`)。
式だけなら `-exec ` 無しでも評価される。**実機の MI 経路で確認済み**:

| できる | 例 |
|---|---|
| バックトレース | `-exec bt` |
| 変数を読む | `-exec print shizuku::selftest::passed` → `$1 = 97` |
| レジスタを読む | `-exec info registers sp pc` / `-exec print/x $pc` |
| メモリを覗く | `-exec x/4xw 0x10000000` |
| ブレークポイント一覧 | `-exec info breakpoints` |

★★**できないこと (2026-08-25 時点)**:
* **書き込みが無音で失敗する**。`-exec set var shizuku::selftest::passed = 12345`
  は**エラーを出さないのに、読み返すと元のまま**。メモリ書き込み (`M`/`X`) も
  レジスタ書き込み (`G`/`P`) も stub 側が未実装で、空返事 =「未対応」を返して
  いるが GDB がそれを黙って受け流す。**この repo の「無音の失敗を作らない」に
  反しているので、実装するか明示的にエラーを返すかを決めること**
* (解消) VS Code でも **全スレッドが見える**。`launch.json` が
  `set non-stop on` するので、走っているものは `(running)`、止まっている
  ものへ切り替えて `bt` / `print` が引ける (D53)

### GDB (プローブ不要) でできること

`arm-none-eabi-gdb bazel-bin/firmware/shizuku` → `target remote /dev/cu.usbmodem103`

アタッチ / シンボルとソース行 / レジスタ / メモリ / **ブレークポイント設置 →
continue → 命中 → continue → 命中 → stepi、が何度でも通る** (D43 で直した。
以前は 2 回目以降が `Cannot execute this command while the target is running`
で固まっていた) / バックトレース / 変数の表示 / **detach で確実に resume する**
(D43 続きで踏んだ別のバグ — `breakpoint_clear` の dsb+isb 抜け — も直した)。

★★2026-08-24 時点、**GDB stub の対象は合成の `debuggee` ではなく実在の
`blink` スレッド** (LED を叩く周期スレッド、`firmware/main.cpp`)。目で見て
「そのスレッドだけ止まる」ことを確かめるデモのために変更した
(`start_gdb_stub(blink_thread)`)。`break debuggee_step` は**もう存在しない**
— 対応する呼び出しは `break stress.cpp:126` あたり (LED 書き込み)。
元の合成 debuggee に戻したい場合は `start_gdb_stub()` を引数無しで呼べば良い
(既定へフォールバックする作りのまま残してある)。**この対象選択をどちらに
するかはユーザー未確定** — 次のセッションで決めてから進めること。

---

## 2. 壊れている・未着手のもの

### 壊れている (最優先)

**無し** (2026-08-26 時点)。D57 で「止めたのに走り続ける」の残り半分
(**寝ているスレッドが締切で勝手に起きていた**) を塞いだ — `sleep_us` の抜け条件に
状態を足しただけ。XNO 実機で `+80/8秒 → +0 (16 秒)` → 復帰で `+81/8秒`。
D43 で GDB の continue/stepi と breakpoint_clear、
D44 で Q8 (flash の好き勝手読み) を `GRANT_REGION` で、D45 で panic の診断ダンプと
「USB だけ生かして復旧できる」停止、D48 で GDB の走行中の割り込み (Ctrl-C) と
待機中の休眠、D49 で**コアごとに FPB を仕掛ける agent** (これが無いと対象が
反対のコアに居るとき永久に止まらなかった) を実装した。実機で `97 passed/0 failed`。

### 未着手 (docs の Q 番号に対応)

| | 内容 |
|---|---|
| Q7 | **idle スレッド**。`sleep_us` の空回りと同じ問題で、要る道具は「T まで何もすることがない」1 つ。WFI するとサイクルカウンタが止まるので、起こし方の機構が要る。★D57 で**止められている間も同じループを回す**ようになったので、ここが埋まると空回りの熱も一緒に消える |
| D26 残 | `schedule()` が O(THREAD_COUNT)。ready のビットマップ + ctz で定数にできる |
| D42 残 | CDC を**ストリームとして**扱う (今は `usb_cdc_read/write` の直呼び) |
| (D51 残は D53 で解消) | non-stop モードを実装したので、走っているスレッドも `(running)` として見える |
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
  * ★**VS Code のデバッグを止めても `arm-none-eabi-gdb --interpreter=mi` は
    生き残る**ことがあり、**SIGTERM では死なない** (D50 で実測)。
    2 つ目の GDB が同じ線に乗ると応答が混ざり、`Ignoring packet error` /
    `unrecognized item "timeout"` という **stub が壊れたように見える**形で
    出る。`tools/flash_and_wait.sh` が焼く前に始末する
* ★★**GDB で止めたのに走り続けるように見えたら、まず自分の測り方を疑う**。
  stub は**沈黙が続くと「客が居なくなった」と判断して全部再開する**
  (idle_rounds)。20 秒黙って観測すると、その間に戻されて「止まっていない」と
  読める (D54 で実際に踏み、結論を 1 度取り違えた)。測る間も GDB 側から
  何か送り続けること
* ★**デバッガ自身 (gdbserver / gdbagent) は GDB の一覧に出ない** (D55)。
  止められると FPB の面倒を見る者が居なくなる / GDB チャネルが死ぬため。
  `monitor list` では見えて `[transport: 止められない]` と印が付く。
  同じ表 (`g_protected_threads`) を転送スレッド (BLE/CDC) と共用している —
  **止めてはいけない相手を足すときはそこへ**。表を増やさないこと
* ★**syscall を一度も撃たないスレッドは止まりきらない** (D54/D57)。**どこかで
  譲るスレッドは完全に止まる** (`sleep_us` で寝るものを含む = D57)。だが
  `burn_microseconds` のような純粋な空回りは 97% 減までで 0 にはならない。
  「全部止められる」と書かないこと
* ★★**負荷の有無で挙動が反転する**ので、停止の確認は**寝ているだけの系**でも
  回すこと (D57)。常時 READY なスレッドが 1 本でも居ると止めた側は必ず CPU を
  手放すので**バグが隠れる** — Shizuku の試験ファーム (負荷 3 本) では再現せず、
  XNO (13 本ほぼ全部が寝ている) でだけ出た
* ★★**共有ボードでは「自分のビルドが載っている」を毎回確かめる** (D57 で 2 回
  誤判定した)。別のエージェント/セッションが同じ板を焼いていると、
  **スレッド番号が丸ごとずれる** (`loadprobe` が 1 本増えて `12` が `bno055` から
  `flight_controller` になっていた)。安い確かめ方が 2 つある:
  * 実機のフラッシュから**命令列を読み戻す** (`m<addr>,16`) → 手元の ELF の
    `llvm-objdump` と突き合わせる。1 往復で「載っているか」が確定する
  * スレッド番号は覚えずに**毎回引き直す** (`qThreadExtraInfo`)。
    番号を覚えていると、黙って別のスレッドを測る
* ★**「対象が止まったまま」で真っ先に疑うのは自分が繋いだ GDB**。attach は
  対象を suspend するので、detach せずに GDB を落とすと止まったままになる。
  GDB に一切触れずに観測して `[STRESS]` が流れるなら系は健全
* **焼いた直後、シリアルのデバイスノードは約 2.8 秒まるごと消える**
  (実測 2026-08-24: t=+6.0s に消え t=+8.8s に戻る)。`picotool` の終了 ≠
  USB の再列挙完了。焼いた直後にポートを開くものは**戻るまで待つ**こと —
  待たないと `could not open device: No such file or directory` になる
  (VS Code の F5 がこれで落ちていた。`.vscode/tasks.json` の「焼く」で
  「消えるのを待ってから戻るのを待つ」ようにして解決)。
  ★「今ある」で判定してはいけない — 再列挙前の**古いノードを見て即通過**し、
  結局同じところで落ちる
* `tud_task()` は低優先度 IRQ で回っているので、**スレッドが固まっても USB は生きる**。
  「焼けない = ファームが固まった」と決めつけないこと (一度誤診した)
* **`save_and_disable_interrupts()` で丸ごと止めると USB (picotool の焼き直し
  要求) も道連れに止まる**。USB の割り込みハンドラ自体が要るので、
  `tud_task()` を手でポーリングしても救えない (D45 で実際に踏んだ。BOOTSEL
  ボタンでの物理復旧が要った)。USB だけ生かして系を止めたいときは
  `usb_cdc_isolate_for_panic()` (usb_cdc.cpp) のように、USB に要る IRQ
  だけを名指しで残す

### 計測が嘘をつく

* **DEMCR に書いたら `dsb + isb`**。無いと「次の命令」ではなく数命令あとで止まる
* **対になる関数の片方だけ `dsb+isb` を付けて満足しない**。`breakpoint_set` には
  あったが `breakpoint_clear` に無く、「`detach` したのに resume されたスレッドが
  同じブレークポイントへ即再ヒットして止まったまま」という、`selftest` は健全
  なのに GDB 経由の操作だけ効かない、切り分けにくい壊れ方をした (D43)
* **例外ハンドラが書く記録は acquire ロードで読む**。素直に読むとレジスタに載る
* **`asm volatile` に `"memory"` を付ける**。無いと前後の読みが吊り上がる
* **窓の最大値だけ見ない**。`led_write` の最大は「プリエンプトされた時間」を含む。
  最小 (誰にも邪魔されなかった 1 回) が操作そのものの費用
* **オブジェクト番号の衝突は無音**。今はビルドが振るので起きないが、
  `objects.list` に足すのを忘れて手で書かないこと
* ★★**`tud_cdc_n_write_char` は FIFO が満杯だと黙って捨てる**。診断はそれで
  よいが (D42「溢れたら捨てる」)、**GDB の返事で 1 バイト落ちると
  チェックサムが合わず**プロトコルが壊れる。短い返事しか無い間は FIFO に
  収まって露見しないので、**長い返事を返すようになった瞬間に初めて出る**
  (D53 で target.xml 731 文字が 558 文字に欠けて発覚)。CDC へ長いものを
  書くときは `usb_cdc_write_available()` で空きを見て待つこと
* **1 本のストリームへ複数のオブジェクトから push しない**。ストリームは
  **object 対 object の路**で、席は `stream_bind` がオブジェクト単位で座らせる。
  `push`/`pop` はわざと svc を通らないので (§13 柱 1)、descriptor のポインタさえ
  持てば bind を通らずに呼べ、席の強制をすり抜けられる — 破ると `wr` の
  読み書きが競って**エラーも出ずにレコードが消える**。複数から流すなら
  **路をその数だけ作り、集約するならハブオブジェクトを立てる**
  (`A/B/C ──stream──→ [hub] ──stream──→ 消費側`)。どのリンクも object 対
  object のままになり、混ぜ方・優先順・溢れたときの捨て方という方針が
  ハブの中に収まる。「producer が複数」を名乗る `MP_PROD` という旗が
  あったが、宣言だけで実装が無く、モデルとも矛盾するので消した (D46)

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
docs/03_porting_policy.md     決定 D1〜D55 と未決 Q1〜Q7 ← **設計の理由はここ**
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

### (A) GDB stub の対象をどうするか決める — 最優先

```
Shizuku の続き。docs/05_handoff.md と docs/03_porting_policy.md (D43) を読んでから始めて。

GDB stub の対象を、2026-08-24 のデモのために合成の debuggee から実在の blink
スレッドへ変えたままになっている (firmware/main.cpp, gdb_stub.hpp/cpp)。
どちらを既定にするか決めて、決めた方に合わせて docs と (必要なら)
docs/05_handoff.md の「GDB でできること」の説明を直して。

判断材料: debuggee は中身が無いので「動かす練習」専用。blink は実在するので
目で確かめられるが、止めている間 LED の周期計測 (late win) が乱れるので
選ぶなら影響を承知した上で。
```

### (B) 次の機能へ進む

```
Shizuku の続き。docs/05_handoff.md と docs/03_porting_policy.md (D44) を読んでから始めて。

D44 で GRANT_REGION (軸 B の動的開示) を実装した。今は flash_stream の
自己テストからしか使っていない — 他に非特権化したい対象 (ペリフェラルの
デバイス窓など、§11.3 の 4-5 に相当する用途) があれば、同じ機構で開けるはず。
まず候補を探して、GRANT_REGION が本当に汎用かを確かめる形で進めて。

もしくは D26 残 (schedule() の O(THREAD_COUNT)) か D42 残 (CDC をストリーム化)
のどちらかを片付けて。
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
