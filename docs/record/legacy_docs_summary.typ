#set document(title: "消した文書の要約 (V1/V2 期の調査文書)", author: "")
#set page(paper: "a4", margin: 18mm, numbering: "1")
#set text(font: ("Hiragino Sans", "Arial"), size: 9pt, lang: "ja")
#set par(leading: 0.65em)
#set heading(numbering: "1.")
#show raw: set text(font: "Menlo", size: 7.5pt)

= 消した文書の要約 (V1/V2 期の調査文書)

本書は、`docs/` から削除した次の 5 文書の要約である。対象は `01_current_repo_survey.md`、`02_reference_survey.md`、`03_porting_policy.md` (1674 行)、`04_work_instructions.md`、`05_handoff.md`。

目的は二つ。第一に、削除前に各文書の決定や注意点のうち今も有効なものが現行の正本へ移ったかを確認すること。第二に、移っていないものを本書に残すこと。全文の経緯や測定値は、削除前のコミットから見られる (「全文の見方」節)。

== 状態欄の意味

- *有効*: 現行のコードまたは正本で確認できる。
- *廃止・置換*: 現行の決定で変わった。後継の決定番号を書く。
- *未確認*: 本作業では確認できなかった。推測で埋めていない。
- *未決*: 決まっていない。

== 全文の見方

#table(
  columns: (0.9fr, 1.2fr, 3.4fr),
  table.header([文書], [全文のコミット], [備考]),
  [01, 02, 04], [`ce5b304`], [stable の先頭コミット。5 文書とも削除前の内容がここにある。],
  [03], [`ce5b304`], [stable の先頭コミット。5 文書とも削除前の内容がここにある。],
  [05], [`ce5b304`], [stable の先頭コミット。5 文書とも削除前の内容がここにある。],
)

全文は次のように見られる。

```bash
git show ce5b304:docs/03_porting_policy.md
git show ce5b304:docs/01_current_repo_survey.md
git show ce5b304:docs/05_handoff.md
```

`ce5b304` が履歴から消えた場合は、`git log --oneline --diff-filter=D -- docs/03_porting_policy.md` で削除コミットを探し、その親 (`<削除コミット>^`) を指定すると削除直前の全文が見られる。

== 01 現行リポジトリ調査 (`01_current_repo_survey.md`)

*扱っていた内容*: 2026-08-19 時点の現行リポジトリの構成 (CMake 構成)、書き方の流儀、ファイルごとの状態、発見済みの不具合 10 件、ビルド方法、git の状態。

*主な内容と状態*:

- §2 書き方の流儀 (template と concepts による差し替え、configs による型注入、modules の作法、`result<T>`、命名)。状態: 有効。`templates/`、`concepts/`、`configs/`、`modules/` の骨格は残っている。ただし CMake 固有の作法 (`config_need_headers`、`add_subdirectory` の順序、CMake 3.26) は無効 (D38)。
- §4 不具合 1, 2, 3, 4, 6, 7, 10: 04 の Phase 0 から Phase 2 の完了記録で処理済みとされた。個別には再確認していない。
- §4 不具合 5, 8, 9 (`cmake-kits.json` の typo、`buildUtilities.cmake` の残骸、`configs/CMakeLists.txt`): CMake 削除で該当しない。
- §5 ビルド方法: 旧 CMake の手順は無効。Bazel の手順は本書の 04 の節を参照。
- §6 git の状態 (ブランチ `0.1`、未コミットの rename): 古い。

*引き継ぎ先*: 流儀の大枠はコードの構成そのもの。不具合の処理状況は正本に無く、04 の記録だけが残っている。

*全文*: `git show ce5b304:docs/01_current_repo_survey.md`

== 02 参照実装の調査 (`02_reference_survey.md`)

*扱っていた内容*: 参照実装 `latest_ver_from_flight_robocon/flight_robocon_telemetory_sender` (RP2350、2 コアで実機運用されていた Shizuku と BLE テレメトリ) の地図、実機で検証された機構 9 件、既知の欠陥 6 件、参照文書の一覧。

*主な内容と状態*:

- 規範の扱い: 「設計文書を正とし、機構の詳細だけをコードから採る。コードは丸写ししない」。状態: 有効。現行の作業方針として 03 を経て確立した。
- 検証済みの機構 (呼び出しフレーム幾何、CTX_SAVE と CTX_RESTORE の offset 縛り、grant、claim の CAS、例外優先度、MPU の W^X、トランポリン、診断インフラ): 現行コードに移植されている (`arch/armv8m/`、`modules/pico_sdk_support/rp2350/`、`source/kernel/`)。個別の対応関係は未確認。
- 既知の欠陥 (identity の潰し、親子関係が無い、`md_table` の引数索引、`r4 == 100/255` の臨時ハック、固定 `[128]` の表、未知番号の黙殺): 対策は現行の決定に入っている (D4、D8、D12、D26、D27 など)。`md` の現行実装は未確認 (D7 を参照)。
- 参照先の文書 (`SHIZUKU_DESIGN.md`、`SHIZUKU_REDESIGN_PORTABILITY.md`、`HANDOFF.md`、`XIP_FLASH_CLOCK.md`) はこのリポジトリに無い。参照実装のディレクトリもこの worktree に無いため、原本は確認していない。

*引き継ぎ先*: V2 の機構の要点は `docs/record/kernel_v2_record.typ` の「構造と制御フロー」節。参照先の設計文書は正本に無い。

*全文*: `git show ce5b304:docs/02_reference_survey.md`

== 03 方針 (`03_porting_policy.md`)

*扱っていた内容*: 設計文書の層構造 (PORT §5) を現行リポジトリの流儀へ写す方針、決定 D1 から D58、未決 Q1 から Q8、§3.5 ビルドと言語の再検討、§3.6 REDISPATCH の廃止、§4 名前の対応表、§5 差分のまとめ。D56 と Q2 は見出しが無く、欠番である。Q8 は D37 の節の中に書かれている。

*主な決定と状態*: 次の表の「移し先」は、`docs/record/kernel_v2_record.typ` の「03 から移した現行決定」節に行がある番号を指す。

#table(
  columns: (0.75fr, 3.3fr, 1.05fr, 1.5fr),
  table.header([番号], [内容 (要約)], [状態], [移し先]),
  [D1], [カーネルはオブジェクトを知らない], [廃止 (2026-09-28)], [D58],
  [D2], [arch と board を分離する。cpu_driver の名は廃止], [有効], [record: D2],
  [D3], [プリミティブは CALL、RETURN、SWITCH、GRANT の 4 種。経路は種別で決める], [有効], [record: D3],
  [D4], [委譲は kobj のメソッド呼び出しで行う。番号の表は kobj 側], [有効 (名称は変更)], [kernel_v2_record §構造],
  [D5], [exit は RETURN 1 本。段数を申告して突き合わせる], [有効], [record: D3],
  [D6], [特権の静的クラスを作らない。信頼ビット 1 個], [置換], [D58 (保護付きとカーネル空間の 2 種別)],
  [D7], [md はカーネルオブジェクトが持つ], [未確認 (md の実装を検索で確認できず)], [—],
  [D8], [表は最初から疎に。スロット確保付きのテンプレート], [未確認 (`templates/table.hpp` の存在のみ)], [—],
  [D9], [アトミック操作は arch のパラメータ], [有効], [`concepts/arch.hpp`],
  [D10], [例外優先度の規約 (svc、タイマ、切替の順) は arch の要件], [有効 (検証は未確認)], [`concepts/arch.hpp`],
  [D11], [クロック変更は board 層の API], [未確認 (参照先の XIP_FLASH_CLOCK.md が無い)], [—],
  [D12], [エラー方針 I-9。一般オブジェクトの失敗はエラー、panic はカーネルの不変条件のみ], [有効], [record: D12],
  [D13], [言語は C++ (template と concepts)], [有効], [コード。D16 も参照],
  [D14], [自己テストは最初から。梯子式で、拒否のテストも書く], [有効], [`source/selftest/` (仕様書として機能)。作法は本書の 05 の節],
  [D15], [ビルドは Bazel。CMake は並存後に削除], [有効], [record: D15、D38],
  [D16], [カーネルコアは C++ を維持。Rust はオブジェクト層の将来オプション], [有効], [`docs/07_rust_and_cyrius_review.md` §2],
  [D17], [オブジェクト集合は別リポジトリ XNO に分離する], [有効 (公開ヘッダの区分は未実装)], [record: D17],
  [D18], [スレッドの記憶はオブジェクト側から貸す], [有効], [record: D18、D19],
  [D19], [所有と保護は別問題。置き場所はカーネルが検査する], [有効], [record: D18、D19],
  [D20], [オブジェクトランドのメモリ授受 API。持ち主を記録する], [有効], [record: D20],
  [D21], [flash FS は XIP 前提でアドレスを返す。写さない], [有効], [`docs/design/kernel_stream.typ` が D21 を参照],
  [D22], [カーネルはライブラリとして提供する], [有効], [`BUILD.bazel` の分割。構成の説明は正本に無い],
  [D23], [オーバークロックは今は入れない。理由は実測で訂正された], [有効], [record: D23。実測値は本書に残す],
  [D24], [実行権の貸し借りの単位はクロック。SLEEP は µs のまま], [有効], [record: D24、D25],
  [D25], [他 ISA でのクロック基準の評価 (数える軸と期限の軸を分ける)], [有効 (評価。他 ISA は未実装)], [record: D24、D25],
  [D26], [オブジェクトシステムは定数時間で抜ける。文字列を持ち込まない], [有効], [record: D26],
  [D27], [名乗り (`DECLARE_NAME`)。比較はしない。解決は System Object の仕事], [有効], [record: D27、D28],
  [D28], [オブジェクト番号はビルドシステムが振る (`objects.list`)], [有効], [record: D27、D28],
  [D29], [2 コア目を起こす。kobj の原子性は table_lock で守る], [有効], [record: D29、D30、D32],
  [D30], [flash を書く間は他コアを止める], [有効], [record: D29、D30、D32],
  [D31], [消去は 1 に戻すときだけ。目録は追記式], [有効], [record: D31、D34],
  [D32], [世界を止める操作は入れ子で数える], [有効], [record: D29、D30、D32],
  [D33], [温度履歴アプリ。揺らぎの実測と seqlock の判断], [有効], [`source/apps/thermal.cpp`。実測値は本書に残す],
  [D34], [締切のある系では消去を走行中の経路に置かない (PREPARE)], [有効], [record: D31、D34],
  [D35], [ストリームは制御をメソッド、データをライブラリで運ぶ。運ぶものは用途で変える], [有効 (概ね)], [`docs/design/kernel_stream.typ`],
  [D36], [読みのストリームの実体は XIP そのもの], [有効], [`docs/design/kernel_stream.typ`],
  [D37], [ストリームの接続は DMA で直結する。中継オブジェクトを消す], [有効], [`docs/design/kernel_stream.typ` (回帰確認の節)],
  [D38], [ビルドを Bazel へ完全移管し、CMake を削除する], [有効], [record: D15、D38],
  [D39], [デバッグ UI は VS Code 標準の Run and Debug に寄せる], [有効], [record: D39],
  [D40], [自己ホスト型デバッグ (M33 の DebugMonitor)。プローブが要らない], [有効], [record: D40、D41、D43],
  [D41], [自己ホスト型デバッグの実装。動くところと動かないところ], [有効], [record: D40、D41、D43],
  [D42], [USB CDC を 2 本に分ける (診断と GDB)], [有効], [record: D42、D45、D47],
  [D43], [2 回目の continue が固まる原因は MON_STEP の arm の位置], [有効], [record: D40、D41、D43],
  [D44], [GRANT_REGION。flash の extent を対象オブジェクトごとに開示する (Q8 の実装)], [有効], [record: D44、Q8],
  [D45], [panic の診断ダンプ。系は止めず、USB だけ生かす], [有効], [record: D42、D45、D47],
  [D46], [stream::MP_PROD を消す。モデルに無い旗は置かない], [有効], [`docs/design/kernel_stream.typ` が D46 を参照],
  [D47], [GDB の CDC の名を RSP にする。macOS では効果が無い], [有効 (知見)], [record: D42、D45、D47],
  [D48], [GDB stub をノンブロッキングにする。待機中は休眠する], [有効], [record: D48、D51、D52、D53],
  [D49], [GDB stub を 3 スレッド (server と agent 2 本) に分ける], [有効], [record: D49、D55],
  [D50], [VS Code の F5 が通らない 2 つの理由 (残存 gdb と、ポートを聞くタイミング)], [有効], [`.vscode/launch.json`、`tools/flash_and_wait.sh`。手順の要約は本書の 05 の節],
  [D51], [GDB へスレッドを正しく見せる。スレッド ID は 16 進], [有効], [record: D48、D51、D52、D53],
  [D52], [デバッガが止めたものは自分が止めたものだけを戻す], [有効], [record: D48、D51、D52、D53],
  [D53], [GDB の non-stop モード。all-stop は運用上不可能], [有効], [record: D48、D51、D52、D53],
  [D54], [止めたのに走り続ける。復活させていた 2 行], [有効], [record: D54、D57],
  [D55], [デバッガ自身は止められない (止めてよい相手を絞る)], [有効], [record: D49、D55],
  [D56], [欠番 (見出しが無い)], [—], [—],
  [D57], [寝ているスレッドは締切で勝手に起きていた], [有効], [record: D54、D57],
  [D58], [D1 を廃止。再登録によるホットスワップを要件にする], [有効], [record: D58。`docs/06_esp32_port_manifest.typ`],
  [Q1], [svc 番号の渡し方 (レジスタ統一か即値か)], [未確認], [—],
  [Q2], [欠番 (見出しが無い)], [—], [—],
  [Q3], [スタック上限の縮退 (PSPLIM が無い ISA)], [未確認], [—],
  [Q4], [参照実装ディレクトリを git に入れるか], [廃止 (この worktree に無い。README は削除済みと記す)], [—],
  [Q5], [サブカーネル活性化中も PendSV の切替延期の対象にするか], [未確認], [—],
  [Q6], [configs の型注入変数の粒度], [未確認 (configs は Bazel 側へ移った)], [—],
  [Q7], [idle スレッドをどうするか], [未決 (未着手)], [record: 未決の節],
  [Q8], [flash を好き勝手に読める問題は MPU の配置の問題], [有効 (D44 で実装)], [record: D44、Q8],
  [§3.5], [ビルドと言語の再検討 (Bazel と C++ の維持)], [有効], [D15、D16],
  [§3.6], [REDISPATCH の廃止。委譲は CALL の 1 種に畳む], [有効 (コードに REDISPATCH は無い)], [record: D3],
  [§4], [参照実装から新実装への名前の対応表], [作業用。参照実装が無いため不要], [—],
  [§5], [現行骨格からの差分 (残す、変える、捨てる)], [大半は実施済み (04 の記録)], [—],
)

*引き継ぎ先*: 有効な決定のうち正本に無かったものは `docs/record/kernel_v2_record.typ` の「03 から移した現行決定」節へ移した。D 番号はコードのコメントから参照されているため、番号を残した。

*全文*: `git show ce5b304:docs/03_porting_policy.md`

== 04 指示書 (`04_work_instructions.md`)

*扱っていた内容*: フェーズごとの作業指示と完了記録、受け入れ条件、地雷一覧、Bazel のビルドと書き込みのコマンド、作業の進め方。

*フェーズの状態*:

#table(
  columns: (0.9fr, 2.4fr, 2.2fr),
  table.header([フェーズ], [内容], [状態]),
  [Phase 0], [リポジトリ衛生と Bazel の bring-up], [完了 (2026-08-19 の記録。その後 CMake は削除された)],
  [Phase 1], [arch 抽象と ARMv8-M バックエンド], [完了 (実機で svc 往復を確認)],
  [Phase 2], [カーネルテンプレートとプリミティブ。D1 に基づく計画は廃止された], [実装記録あり。2026-08-20 に作り直した。実機の検証は本作業では未確認],
  [Phase 3], [カーネルオブジェクト (kobj) と委譲ファミリ], [未確認 (受け入れ条件の個別の確認は未了)],
  [Phase 4], [親子関係と md、オブジェクト arena], [未確認 (親子関係と md の実装を確認していない)],
  [Phase 5], [MPU と非特権実行。段階 A、B、C を実機で確認], [段階 A から C は完了。次の段のうち per-object region は D44 で実装],
  [Phase 5.5], [資源の出どころ、flash FS、ライブラリ化], [完了 (2026-08-20 の記録)],
  [Phase 6 以降], [ストリーム、XNO リポジトリ、2nd ISA、クロック API], [ストリームは完了。XNO は D17 のとおり分離。2nd ISA とクロック API は未着手 (未確認)],
)

*地雷一覧* (全フェーズ共通): 次の 14 件を、踏んだ事実とともに記録している。要点だけを残す。

- xPSR bit9 のパディングで復帰後の SP が +4 になる。`psp_after_return` に一元化する。
- 元の例外フレームを動かすと FP 拡張の S0 から S15 とずれる。フレームは複製だけにし、原本は動かさない。
- pop 時に幾何を再計算しない。push 時の記録値を読み戻す。
- ハンドラ shim の引数位置は共通ヘッダの 1 か所だけにし、自作しない。
- 委譲時に元の syscall の引数を転送し忘れると、引数が obj ID に化けて無限再帰する。委譲は CALL_STRICT に一本化し、identity の e2e テストで検出する。
- 「見てから作る」TOCTOU は、RESERVED への CAS 予約で防ぐ。
- READY は最後に release store で公開する。
- 未知の svc 番号を default で黙殺すると、状態の設定が無音で消える。未知番号はエラーで返す。
- 生存カウンタを機能の証拠にしない。期待値と突き合わせる。
- printf が系を固めることがある (pico_stdio の print_mutex)。診断は最初からリングと同期排出で作る。
- exclusive handler の二重登録は pico-sdk が panic する。登録は board の init の 1 か所に集める。
- 固まったファームは書き込みツールが「成功」と返しても書かれていないことがある。書き込み完了は出力行で判定する。
- オブジェクト番号の衝突は、保護が壊れたように見える。番号表は 1 か所に集め、ALREADY_EXISTS を無視しない。
- flash への書き込み元が flash 上にあると、書き込み中は XIP が止まっているので読めない。RAM の中継バッファへ写してから渡す。

*ビルドと書き込み* (Bazel のみ。CMake は削除済み):

```bash
bazelisk build //firmware:shizuku_uf2   # 作るだけ
bazelisk run   //firmware:flash         # 作って焼く
bazelisk clean                          # 出力を捨てる
```

- 構成のつまみは `.bazelrc` と `configs/BUILD.bazel`、CYW43 のクロックは `configs/cyw43_clock.bzl`、オブジェクト番号は各 `objects.list`。
- UF2 の aspect は Apple Silicon では使えない。`//:shizuku_uf2` は picotool を直接呼ぶ (非 hermetic)。SDK の更新で直ったら `.bazelrc` のコメントを戻す。
- `-Og` を全体に付けている。`-O0` だと svc ラッパの inline asm がレジスタ割り付けできないため。

*引き継ぎ先*: 地雷一覧とビルドの手順は正本に無い。本書の要約だけが残る。

*全文*: `git show ce5b304:docs/04_work_instructions.md`

== 05 引き継ぎ書 (`05_handoff.md`)

*扱っていた内容*: 次に触る人が最初に読む 1 枚。2026-08-24 時点の現在地 (動いているもの、壊れているもの、未着手)、踏むと痛いところ、測り方の作法、どこに何があるか、この repo の作法、次のセッションへの推奨プロンプト。

*現在地 (2026-08-24 時点。後の 2026-08-26 の D57 までを含む)*:

- 動いているもの (実機で確認済み): カーネル (呼び出しフレーム、種別による経路、クロック基準の貸し借り、フォールト隔離)、保護 (MPU、非特権オブジェクト、拒否のテスト、GRANT_REGION)、2 コア、記憶 (O(1) の arena)、ストリーム (SPSC、席の強制、DMA 接続)、flash FS (XIP アドレス、追記式、事前消去)、温度履歴アプリ、デバッグ (DebugMonitor、GDB の continue、stepi、non-stop、Ctrl-C)。
- 基準線は `93 passed / 0 failed`。その後の変更で `97 passed / 0 failed` に上がった記録がある。
- 壊れているもの: 2026-08-26 時点で無し (D57 で解消)。
- 未着手: Q7 (idle スレッド)、D26 残 (`schedule()` の O(THREAD_COUNT))、D42 残 (CDC のストリーム化)、D17 (XNO は一部。`source/apps/` は暫定の置き場)。
- 未解決の問題 (本書の要約にのみ残る): GDB の書き込み系 (`M`、`X`、`G`、`P`) が未実装で、空の返事のまま GDB に黙って無視される。VS Code の Debug Console で `set var` を実行しても値が変わらない。この repo の「無音の失敗を作らない」に反する。

*踏むと痛い注意 (正本に無いもの。要約として残す)*:

- USB の記述子を間違えると列挙されず、シリアルも picotool も効かなくなる。BOOTSEL でしか戻せない。記述子で名乗ったインターフェースには必ずドライバが要る。
- 1200bps タッチで BOOTSEL へ落とせることがある。
- 焼いた直後は、シリアルのデバイスノードが約 2.8 秒消える。picotool の終了は USB の再列挙の完了ではない。「今ある」で判定すると、古いノードを見て通過してしまう。
- ホスト側でポートを掴んだままのプロセス (cat や gdb) があると picotool が失敗する。焼く前に殺す。VS Code を止めても gdb が残ることがある。SIGTERM では死なない。
- 止まったように見えるときは、まず自分が繋いだ GDB を疑う。attach は対象を suspend するので、detach せずに落とすと止まったままになる。
- 20 秒黙って観測すると、stub の idle 判定で全部再開されて「走り続ける」と読めることがある。測る間は GDB 側から何か送り続ける。
- 負荷スレッドの有無で挙動が反転する。停止の確認は寝ているだけの系でも回す。
- 共有ボードでは、別のエージェントが焼いた別のビルドが載っていることがある。スレッド番号が丸ごとずれる。実機のフラッシュから命令列を読み戻して手元の ELF と突き合わせる。スレッド番号は毎回引き直す。
- DEMCR に書いたら dsb と isb を入れる。対になる関数 (set と clear) の両方に入れる。
- 例外ハンドラが書く記録は acquire ロードで読む。`asm volatile` には memory clobber を付ける。
- `tud_cdc_n_write_char` は FIFO が満杯だと黙って捨てる。GDB の返事は 1 バイトも落とせないので、長い返事は空きを見て待つ。
- 1 本のストリームへ複数のオブジェクトから push しない。路をその数だけ作り、集約するならハブを立てる。
- 測るときは条件を交互に回す。1 回の計測で優劣を判断しない。64KB のブロック消去は境界に揃えないと効かない。

*この repo の作法* (正本に無いもの): 測って決める。許可のテストだけでは証拠にならず、拒否のテストを必ず書く。無音の失敗を作らない。「気をつける」で守らず、型と機構とビルドで守れないか先に考える。確かめていないことを「できる」と書かない。踏んだ罠はコメントと docs に残す。日本語で書き、コミットメッセージも日本語にする。

*陳腐化している部分*: §1 の表と §6 の推奨プロンプトは 2026-08-24 時点の作業指示で、現状とずれている。§4 の「どこに何があるか」は一部が古い (`source/apps/` の位置づけなど)。

*引き継ぎ先*: 決定の要点は `docs/record/kernel_v2_record.typ` の「03 から移した現行決定」節 (D40 から D58 まで)。踏む罠と作法は正本に無く、本書にだけ残る。

*全文*: `git show ce5b304:docs/05_handoff.md`

== 削除に伴う参照の付け替え

削除に伴い、次の参照を付け替えた。

- `docs/README.md` (索引) は、正本 (`docs/design/`、`docs/record/`) と本書を指す形にした。
- `docs/07_rust_and_cyrius_review.md` は、05 の §5 を本書の 05 節へ、03 の集約先を `kernel_v2_record.typ` へ向けた。
- `docs/design/survey/object_call_api.typ` は、01 と 02 を本書へ向けた。
- コードのコメント中の文書名 (03 の D 番号、05、04) は、D 番号の正本である `kernel_v2_record.typ` か、廃止された決定と 05 や 04 の記述を指すよう、差し替えた。削除前の全文へは `ce5b304` を示した。

未了: `.vscode/launch.json:3` と `.vscode/tasks.json:3` は編集権限が無く、`docs/03_porting_policy.md` の名を残している。また、コメントの `PORT §` と `DESIGN §` は、この repo に該当文書が無いまま残っている。
