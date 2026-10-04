# 非同期 object destroy

`DESTROY_OBJECT(object)` は ticket を返し、`DESTROY_STATUS(ticket)` は
pending=0 / complete=1 を返す。自分を破棄した場合は呼び出し自体が戻らないことがある。
完了とは、対象を通る実行 context が終了し、登録ストリームと所有メモリが回収された状態。
スロットを再利用した後も直前の ticket は保持するが、同じ object ID の次の destroy が
受理されると古い ticket は `BAD_OBJECT` になる。ticket の wrap は拒否する。

## 機構と方針の境界

- `templates/task.hpp` / `cpu_manager`: コア・実行優先度ごとの grant 履歴帳。
- `source/kernel/destroy.cpp`: thread→task の登録、停止要求と epoch 付き ACK、
  全登録 task の巻き戻し。object ID、ストリーム、allocator の知識は持たない。
- `source/kernel_object/destroy.cpp`: 破棄対象の選定、権限、非同期 worker、各コアの
  復帰 context、DMA の終了待ち、外部参照の終了確認、メモリ回収。
- `modules/pico_sdk_support/core_notify`: hardware alarm IRQ で指定コアへ通知するだけ。
  callback は PendSV を起票する。object / thread / grant の処理は置かない。

Pico SDK の multicore FIFO は flash lockout が使うため流用しない。
代わりに起動するコアごとに hardware alarm を1個占有する。確保できなければ SDK が
panic する。アプリの alarm 利用との資源配分は composition で確認すること。

## 破棄の順序

1. 台帳ロック下で対象を closing にする。新しい CALL/SPAWN、ストリーム接続、
   対象による新規操作を拒否する。終了と YIELD は許可する。
2. 別スタックの worker へ GRANT する。worker が使用中・残時間不足なら要求は
   pending のまま通常の schedule から処理する。同時に受理する destroy は1件。
3. 全 task に移動禁止要求を出し、オンラインの他コアへ通知する。相手が PLAIN
   実行へ戻った PendSV で IRQ をマスクし、計時を精算して ACK を返すまで待つ。
   HANDLER/KERNEL_OBJECT を共有台帳ロックごと止めてはならない。SVC 終了時に再起票する。
   1ms で ACK が揃わなければ要求を解除し、context は無効化せず後で再試行する。
   ACK は epoch と一致させ、タイムアウト後に遅れて届いた ACK を次回に流用しない。
4. 生成元と信頼済み shadow stack から、その object を含む thread を全部選ぶ。
   **同期呼び出し途中の thread は丸ごと終了する**。途中の method frame だけを飛ばして
   呼び出し元を続行する方式ではない。
5. thread の task 登録マスクをたどり、全該当 grant 履歴から除く。実行中の被害 thread
   があれば、生存する WAIT_GRANT の貸し手へ EXPIRED を返す。SUSPENDED は復活させない。
   戻り先がなければ objectland が用意したコア専用 idle に切り替える。
6. 終了した context のスタックはまだ回収せず pin する。該当 DMA 接続を closing にし、
   転送完了を確認する。待ちが必要なら他コアを再開して延期する。通常の scheduler にも
   pin を見せ、先にスタックを回収させない。
7. 外部参照の終了 hook が成功したら、登録ストリーム、thread スタック、object 所有
   allocation を回収する。完了 ticket を公開し、各コアが自分のタイマを再設定して再開する。

worker 1本とコア専用 idle を必要時に確保する（2コアなら3 thread / 12KiB のスタック）。
新しい thread は RESERVED で作り、objectland の台帳を完成させてから READY として公開する。
初期化途中の thread が別コアで実行されることを防ぐため、通常の SPAWN にも適用した。
コアの追加起動は従来通りブート時の composition に限定し、destroy と並行させない。

## 外部参照と適用範囲

**MPU で実行を止めても、生ポインタやハードウェアの参照は失効しない。**
現行の stream は descriptor / buffer を直接公開する。IRQ callback、他 object が
保持するポインタ、flashFS の code extent などの寿命を generic kernel は推測できない。
このため trusted composition が `set_destroy_hook(id, hook)` を登録した object だけを
破棄可能とする。hook のない対象は `DESTROY_BUSY` で受理しない。

hook は対象 context の終了後、全コア停止・IRQ マスク・台帳ロック保持中に実行する。
割り込み登録・外部ポインタ・code extent などを解除し、再開する利用者が解放対象へ
触れないと保証できたら true を返す。外部資源の進捗が必要なら false で延期する。
**ブロック、メモリ確保、syscall、IRQ 完了待ちは禁止**。延期時にまだ他者が使うメモリを
解放してはいけない。外部参照を持たない object なら常に true の hook でよいが、
共有資源を持つ firmware object に無条件の hook を付けてはいけない。

既存の全 firmware object を自動的に安全な unload 対象にしたわけではない。
各 object の資源解放 hook の接続は composition の責任であり、今回既存ドライバへ
無条件 hook は追加していない。ROOT / kernel object 自身、存続する子を持つ HANDLER は
破棄を拒否する。対象 thread が debugger 保護対象なら完了を延期する。

権限は自分自身、ROOT、kernel object に限る。hot redirect / migrate / 子の移管・
再起動方針はこの API では規定しない。現行 port は各コアに実行優先度 slot 0 の1系列のみ。
thread 登録と victim 集合は32bitで、現行32 thread構成まで対応する。

## 検証と未検証

`bash tests/host/run.sh` は実際の kernel / kernel_object ソースを host arch に接続し、
UBSan と C++ の別スレッドを使って検証する。

- 既存 grant 回帰、世代検査、停止中の貸し手。
- 別コアの active borrower を停止し、生存 lender へ復帰。
- 自己破棄で worker への lender を消し、idle へ復帰。
- 複数 task への登録を全て除去、停止 lender を復活させない fallback。
- 同期 call の途中に対象がある、別 object 所有 thread の終了。
- hook / DMA の延期中にメモリを保持し、通常 scheduler にも回収させない。
- 停止 ACK のタイムアウトと古い epoch の拒否、closing 中の SPAWN 拒否。

RP2350 向け Bazel ビルドはこの作業環境で外部依存の archive 展開に失敗し、
コンパイル段階まで到達しなかった。ホスト試験は実機の例外優先度、MPU、タイマ IRQ、
USB/flash lockout との競合を代替しない。RP2040/RP2350 の実機試験は未実施。
