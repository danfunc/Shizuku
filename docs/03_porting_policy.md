# 03. 方針: 設計文書 → 現行リポジトリ流儀への写像

ここは「PORT §5 の層構造」と「現行リポジトリの組み方」を突き合わせて、
新実装のディレクトリ・型・名前を確定させる文書。**決定 (D-n)** と **未決 (Q-n)** を分けて書く。
未決はユーザーに確認するか、着手時に「暫定」と明記して進める。

## 1. 層構造の写像

PORT §5 の提案:

```
arch/<isa>/   文脈退避・例外フレーム・特権遷移・atomic・タイマ割り込み (ISA ごとに書く唯一の層)
board/<board>/ コア起動・時刻・UART/USB・DMA・ペリフェラル・クロック API
kernel/       スレッド・文脈・呼び出しフレーム・実行権委譲 (オブジェクト概念なし)
kobj/         カーネルオブジェクト: オブジェクト表・メソッド解決・svcルート・md (全ISA共通)
objects/      ドライバ / アプリ
```

これを現行リポジトリの流儀 (internal_headers + templates + concepts + modules + configs) に写像する:

| PORT の層 | 置き場所 | 形 |
|---|---|---|
| **kernel** (機構・ISA 非依存) | `internal_headers/shizuku/templates/kernel.hpp` ほか + `source/` | `templates::kernel<ARCH, BOARD, …>` クラステンプレート。実装は config.hpp で確定した別名への `template<>` 特殊化 (.cpp) |
| **arch** | `modules/<port>/internal_headers/shizuku/archs/<isa>.hpp` + `<isa>_ctx.S` | kernel のテンプレートパラメータ。`concepts::arch_requires` で縛る。現行の `cpu_drivers/` を **`archs/` に改名・再定義** (D2) |
| **board** | `modules/<port>/internal_headers/shizuku/boards/<board>.hpp` + .cpp | 同上 (`concepts::board_requires`)。現行の `memory_managers/` の役割を包含し、時刻・コア起動・クロック API も持つ |
| **kobj** | `internal_headers/shizuku/templates/kernel_object.hpp` + `source/kernel_object/` | ISA 非依存の C++。kernel テンプレートとは**別クラス**。カーネルからはハンドラとして起こされ、プリミティブ (CALL/RETURN) を撃ち返す |
| **objects** | **別リポジトリ「XNO」** (D17) | Shizuku 本体に残るのはカーネル検証用の自己テストオブジェクトのみ |
| 型注入・構成 | `configs/config_template.hpp.in` (現行機構を維持) | `SHIZUKU_ARCH` / `SHIZUKU_BOARD` / `SHIZUKU_CPU_COUNT` 等の kit 変数で確定 |

## 2. 決定事項 (設計文書由来。再議論しない)

- **D1. カーネルはオブジェクトを知らない** (PORT §3 / DESIGN §7)。カーネルが知るのは
  スレッド / 文脈 / スタック上限 / 呼び出しフレーム / 登録ハンドラ entry /
  スケジューリング機構 / MPU region だけ。`templates::kernel` から `OBJECT_T`
  パラメータと `object_table` を**外す** (現行骨格からの最大の変更点)。
  **identity (cookie) すらカーネルには無い** — 「誰がどのオブジェクトとして走って
  いるか」は kobj が自分の台帳 (影スタック) で追う。カーネルが持つのは
  「この枠はハンドラを起こしたものか」の 1 ビットだけ (§3.6.1)。
- **D2. arch と board を分離**し、cpu_driver という名前は廃止 (01 §4-6 のねじれ解消)。
  最初の実体は `archs::armv8m` (RP2350) と `boards::rp2350_pico2` (pico-sdk ベース)。
  ※ 参照実装の実績があるのは RP2350。RP2040 (ARMv6-M) は PORT §6 の移植順序 3 番目以降。
- **D3. カーネルプリミティブは 4 種** (DESIGN §7.2 から REDISPATCH と SET_HANDLER を
  削除。経緯と根拠は §3.6 / 2026-08-19 改定):
  `CALL(call_request*)` / `RETURN(n, value, err, depth_claim)` /
  `SWITCH(tid)` / `GRANT(tid, us)`。
  - I-1: svc 番号で経路を分岐しない。**経路は呼び出しフレームの段数のパリティ
    だけで決まる** — 偶数段 = オブジェクト、奇数段 = ハンドラ。フレームを積むのは
    トランポリンと CALL の 2 つだけで必ず交互になるので、段数がそのまま実行主体を
    表す。旗も cookie も要らず、段数を書けるのはカーネルだけなので偽装もできない
  - I-2: プリミティブを撃てるのはカーネルオブジェクトのハンドラだけ。上の判定
    そのものが保証するので、検査して弾くコードは存在しない
  - **カーネルは identity (cookie) も信頼ビットも持たない**。誰が誰かは kobj の台帳
    (PORT §3.1「呼び出し元 identity はカーネル支援不要」)
  - ハンドラの登録は syscall ではなく系の組み立て (`set_object_handler`)
  - `protection` = 実行特権 + (将来) region set (DESIGN §11.3)
- **D4. 委譲は kobj のメソッド呼び出しで行う** (PORT §4.6 改め)。カーネルが持つのは
  「オブジェクトランドの svc ハンドラ」の入口 1 個だけで、番号 → 担当の表は kobj 側。
  identity は kobj の台帳から取るので、呼び出しに identity 引数は要らない。
- **D5. exit はオブジェクトランドの exit API 経由**。オブジェクトは RETURN を撃て
  ないので、(a) カーネルがハンドラを起こすとき**今のネスト数を渡し**、
  (b) オブジェクトは exit API に**何段戻すか**を載せて撃ち、
  (c) ハンドラが `RETURN(n, value, err, depth_claim)` で巻き戻す (1 段 = 2 枚)。
  段数は必ず申告し、カーネルが実際の深さと突き合わせる (§9.3)。
  **戻り口はカーネルの 1 本だけで、呼び出しごとに指定しない** — その戻り口が撃つ
  RETURN は、ハンドラから出ればプリミティブ、オブジェクトから出れば経路判定で
  exit API としてハンドラへ届く (だから exit API の番号は RETURN と同値)。
  REPLACE モードは作らない。
- **D18. ペリフェラルオブジェクトは board モジュールが持ち、特権を宣言する**
  (2026-08-20 ユーザー提案)。`spi_init` のような操作は RESETS/CLOCKS を触るので
  本質的に特権側かつ SoC 固有。**ペリフェラルオブジェクトが特権を引き受けることで、
  その上のドライバ (XNO 側) を非特権のままにできる** — DESIGN §11.2 の
  「ドライバは非特権で走れる。IO を触るから特権ではない」を成立させる分担。
  生成時に `OBJECT_PRIVILEGED` を宣言し、kobj の `object_protection()` が
  Phase 5 (MPU) でそれを実際に効かせる。現状は全オブジェクト特権 (MPU が無いので
  非特権にしても隔離にならず pico-sdk が黙って壊れるだけ = §11.2.1)。
- **D6. 特権の静的クラスを作らない** (DESIGN §11.6 / PORT §4)。信頼ビットは 1 個。
  サブカーネルへの特権貸し出しはしない (overhead を払う。決着済み 2026-08-17)。
  現行骨格の `obj_type::PRIVILEGED_LAND_OBJECT` 系 enum は削除し、オブジェクト属性は
  kobj 側の表 (軸 C) + CALL の protection 引数 (軸 A/B) で表す。
- **D7. md はカーネルオブジェクトが持つ** (DESIGN §12.3)。ただし実装は
  §17 の順序 (identity → 親子 → md → arena) を厳守。順序を飛ばした md 単体実装は無意味。
- **D8. 表は最初から疎に** (PORT §5.3)。`static_table<T,128>` の直接使用ではなく、
  スロット確保 (CAS 予約) 付きの疎テーブルテンプレートを `templates/table.hpp` に追加する。
  会計 (使用量に比例した予算) の前提。
- **D9. アトミック操作は arch パラメータ** (DESIGN §14.5.3)。CAS / release store /
  acquire load を concept 要件にする。ktab_lock (コア間共有表の短スピンロック) も
  arch の CAS 上に作る。svc ハンドラ内では block/yield 禁止 (純スピン)。
- **D10. 例外優先度規約 (svc 最優先 > タイマ > 切替最低) は arch の要件として明記**
  (DESIGN §14.5.1)。concept のコメント + arch ごとの自己テストで守る。
- **D11. クロック変更は board 層 API** (PORT §2.3 / XIP_FLASH_CLOCK.md)。
  上位 (kernel/kobj/objects) はクロックを直接触らない。
- **D12. エラー方針 = I-9**。一般オブジェクト起因の失敗はすべてエラー復帰。
  panic はカーネル自身の不変条件破れのみ。**共通するのは「黙って捨てない」**で、
  返す先があるならエラー、無いなら panic (未知の svc 番号は、オブジェクトランドの
  ハンドラなら発行元へエラー返却、カーネルのプリミティブ選択なら panic — §3.6.1)。
  C++ 内 API は `templates::result<T>`、svc ワイヤは 0=成功の型付き enum
  (参照実装 `error_t<E>` の形を templates/result.hpp の隣に移植)。
- **D13. 言語は C++ (template + concepts)** (PORT §5.6)。現行リポジトリの流儀とも一致。
- **D14. 自己テストは最初から** (DESIGN §16)。梯子式 (1 段 → N 段)、identity e2e、
  状態の自己申告 (MRS CONTROL 等)、カウンタは期待値と突き合わせ、拒否のテスト。
  検証インフラ (固まっても出る診断経路 / panic ring / svc trace) も最初から (PORT §7)。

- **D15. ビルドシステムは Bazel** (2026-08-19 ユーザー確認済み:「Bazel でもいいし
  おそらくそっちの方が望ましい」)。再検討の結果と根拠は §3.5 参照。
  移行戦略: **CMake は Bazel bring-up が完了するまで並存させ、同等性確認後に削除**。
  configs/config_template.hpp.in の型注入機構は Bazel でも維持できる
  (`expand_template` / genrule で同じ config.hpp を生成する)。
- **D16. カーネルコアの言語は C++ を維持、Rust はオブジェクト層の将来オプション**
  (再検討の結果。§3.5 参照)。svc + メソッド呼び出しの安定 ABI 境界を保つことが
  「後から Rust オブジェクトを足せる」ための前提条件なので、ABI 境界の設計時に
  C++ 固有機能 (例外・RTTI・C++ 型のワイヤ露出) を跨がせないこと。
- **D17. オブジェクト集合 (ドライバ / アプリ) は別リポジトリ「XNO」に分離**
  (2026-08-19 ユーザー指示。XNO = 苦悩, "X Not an Object")。
  - Shizuku リポジトリに残るもの: kernel / **kernel object (kobj)** / arch / board /
    configs / **カーネル自己テスト用オブジェクト** (DESIGN §16 のテスト群はカーネルと
    同居しないと検証が回らない)。ユーザー確認済み:「kernel object は含めていいが、
    Shizuku を XNO ほど大規模にする理由はない」— Shizuku 本体は**小さく保つのが目標**
    (カーネル + 信頼境界内の最小構成のみ)
  - XNO 側に置くもの: 実ドライバ (BLE/BME280/BNO055/WS2812/USB 等の移植)、
    アプリ (TELEMETRY 等)、System Object / セッション / ユーザー等の上位構成
    (DESIGN §4.4 の階層) も方針オブジェクトなので XNO 側が自然
  - 境界 = **svc + メソッド呼び出しの安定 ABI** (D16 と同じ境界)。Shizuku は
    ABI ヘッダ (svc ラッパ・obj_api 相当・stream ライブラリ) を「公開ヘッダ」として
    エクスポートする必要がある — internal_headers と別に `public_headers/` 系の
    区分を設けること (現行は internal しか無い)
  - Bazel 化 (D15) と噛み合う: XNO は `bazel_dep` + `git_override` で Shizuku を
    引く。参照実装が 1 ディレクトリにカーネルとアプリを混ぜて負債化した反省とも整合
  - 未決: XNO リポジトリの作成タイミングと、stream.hpp のような「ライブラリだが
    データプレーン」の所属 (暫定: Shizuku 側の公開ヘッダ)

## 3. 未決事項 (着手時にユーザー確認 or 暫定判断)

- **Q1. svc 番号の渡し方** (PORT §2.1/§8-2): 即値ハイブリッド維持か、レジスタ統一か。
  RISC-V (`ecall` に即値なし) を見据えるとレジスタ統一が共通化しやすい。
  **暫定推奨: 新 ABI はレジスタ渡しに統一** (即値は ARM 向け最適化として後日計測してから)。
  ただし未計測なので一方向ドアにしない (ABI 定数は 1 ヘッダに集約)。
- **Q3. スタック上限の縮退** (PORT §8-3): PSPLIM が無い ISA (ARMv7-M/RISC-V) での
  ソフト検査方式。ARMv8-M だけの間は `arch::stack_limit_set` の意味論だけ決めておく。
- **Q4. `latest_ver_from_flight_robocon/` を git に入れるか**。少なくとも docs 2 本は
  参照価値が高い。ビルド対象からは常に除外。
- **Q5. サブカーネル活性化中も PendSV 切替延期の対象にするか** (PORT §3.4 末尾の残課題)。
- **Q6. 現行 configs の型注入変数の粒度**。OBJECT 系 (THREAD_TABLE/METHOD_TABLE/
  MEMORY_TABLE) は D1 によりカーネルから消え、kobj 側の構成になる。kit 変数の
  置き直しが必要 (`SHIZUKU_ARCH` / `SHIZUKU_BOARD` / `SHIZUKU_KOBJ_*`)。

## 3.5 ビルドシステムと言語の再検討 (2026-08-19)

ユーザー指示「CMake か Bazel かは検討し直せ (Bazel の方がおそらく望ましい)。
Rust も同様に検討 (こちらは事実上 Cargo 強制なので考える要素が多い)」を受けた再検討。

### ビルドシステム → **Bazel に決定 (D15)**

PORT §5.5 の一般論 (差分ビルド 0.88s / 再現性 / 構成=target / toolchain rule) に加えて、
このマシンで確認した決定打:

- **pico-sdk 2.2.0 (手元の `~/.pico-sdk/sdk/2.2.0`) は公式 Bazel サポートを同梱**している。
  `MODULE.bazel` / `bazel/toolchain` (mac-aarch64-rp2350 含む) / `bazel/platform` /
  `bazel/config` (PICO_BOARD 等の build setting) / UF2 生成 aspect
  (`@pico-sdk//tools:uf2_aspect.bzl`)。PORT 執筆時に最大の懸念だった
  「C 資産 (pico-sdk/tinyusb/btstack) を Bazel に載せる手間」が公式に解決済み
- ローカルに bazel 9.2.0 / bazelisk 導入済み (`/opt/homebrew/bin`)
- 使い方の要点 (bazel/README.md より):
  `bazel_dep(name="pico-sdk", ...)` + `register_toolchains(@pico-sdk//bazel/toolchain:mac-aarch64-rp2350)` +
  `--platforms=@pico-sdk//bazel/platform:rp2350`。ボードは
  `--@pico-sdk//bazel/config:PICO_BOARD=pico2`
- 残リスク: SDK の Bazel ビルドは CMake 側より新しく、btstack/cyw43 系の網羅性は
  CMake に劣る可能性がある。**ただしそれらは XNO (D17) 側の話**で、Shizuku 本体
  (kernel/kobj/arch/board 最小) には効かない。bring-up はカーネル最小構成で行う
- CMake の扱い: Bazel で「configure → ビルド → UF2 → 実機で svc 往復」が通るまで
  並存。通ったら CMake / cmake-kits.json を削除 (二重管理は D15 の趣旨に反する)
- configs 機構の写像: kit のキャッシュ変数 → Bazel の `string_flag`/`label_flag`
  (bazel/config パッケージを作る)。config.hpp 生成は `expand_template` で同形にできる。
  「モジュールが config_need_headers を積む」は provider か単純に
  ラベルリストの flag で表現する

### 言語 → **カーネルコアは C++ 維持、Rust は XNO 側の将来オプション (D16)**

PORT §5.6 の 4 理由を再点検した結論。ユーザーの指摘どおり Rust の考慮点は
「言語の質」ではなく**ビルド編成**にある:

1. **Rust は事実上 Cargo 強制** (ユーザー認識と一致)。`rules_rust`/`crate_universe` は
   内部で cargo を回して lockfile を解決するし、組込み `no_std` の `core`/`alloc`
   再ビルド (`-Z build-std`) は nightly + cargo 前提。Bazel を D15 で選んだ以上、
   Rust をコアに入れる = **ビルドシステム 2 つを一番痛い継ぎ目 (カーネル/SDK 境界) で
   接ぐ**ことになる。これが最大の反対理由で、PORT 執筆時から状況は変わっていない
2. カーネルの最難関 (例外フレーム幾何 / naked asm シム / xPSR bit9) は Rust でも
   `unsafe` の中に残り、型システムの守備範囲外 (PORT §5.6 理由 2 の実バグ 3 件)
3. 実機で検証済みの C++ 資産 (フレーム幾何・CTX_SAVE/RESTORE) を書き直すコストは
   高く、逆方向 (後から Rust オブジェクトを足す) は安い — 片方向にしか安くない
4. **Rust が本当に効くのは XNO 側** (ドライバ・アプリ: 所有権/ライフタイムが仕事を
   する層)。D17 の分離により「XNO の一部を Rust + Cargo (または rules_rust) で書き、
   安定 ABI 越しに Shizuku とリンクする」実験が Shizuku 本体を汚さずにできる。
   その将来のために ABI 境界に C++ 型を露出させない (D16)

再訪条件: (a) rules_rust の no_std/build-std 対応が cargo 非依存になった場合、
(b) XNO 側で Rust ドライバが実績を積み、コアへ広げる動機が実測で出た場合。

## 3.6 REDISPATCH 廃止の決定 (2026-08-19, ユーザー提起)

ユーザー指摘「結局ネスト数を引き渡すから REDISPATCH 要らなくない?」を受けた再検討。
**結論: プリミティブとしての REDISPATCH は削除する** (D3/D4/D5 を上のとおり改定)。
DESIGN §7.2-7.3 / PORT §4.6 は「REDISPATCH + CALL の 2 種」としていたが、これを
さらに 1 種 (CALL) に畳む。

**★誤読注意 (ユーザー確認済み 2026-08-19): 「svc のオブジェクトランドハンドリング
(サブカーネル化)」という機能は廃止しない。** 消えるのは専用プリミティブだけで、
機能は「kobj のルート表 (svc 番号範囲 → 担当オブジェクト) + CALL_STRICT(arg0=svc
番号, r1-r3=元引数, caller_cookie=元発行元)」として維持する。委譲先は naked shim
でなく普通の export メソッドになる (書きやすくなる方向の変更)。

### §3.6.1 svc ハンドラは 2 つある。混同しないこと (2026-08-19 確定)

**★2026-08-19 追記 (ユーザー指摘による修正)**: 当初この節で「カーネルが
カーネルオブジェクトの cookie を持って比較する」と書いたが、それも不要だった。
経路判定に使えるのは**カーネル自身が積んだ呼び出しフレーム**で、そこに
「ハンドラを起こすために積んだ枠か」を記録すれば足りる (書けるのはカーネルだけ
なので偽装不能)。したがってカーネルは cookie も identity も信頼ビットも持たない。
また「活性化 (activation) に権限が付く」という概念も置かない — 参照実装の
`in_handler` はその形で、ハンドラから呼ばれた先まで特権化する穴だった。
なお「カーネルオブジェクトのルータを例外文脈で C++ 直呼びする」案も検討したが
**却下** (ユーザー判断)。ハンドラはスレッドモードで走る。

「カーネルだけで svc dispatch は完結すべきでは / kobj を経由する必要があるのか」
というユーザーの問いに対し、**リファレンス実装の形が正しい**と確定した。

**★用語 (ここを混ぜると設計が壊れる)**:

| | カーネルの svc ハンドラ | オブジェクトランドの svc ハンドラ |
|---|---|---|
| 実体 | `KERNEL::svc_dispatch` (参照: `svc_cpp_handler`) | SET_HANDLER で登録するメソッド (参照: `kernel_obj_svc_handler`) |
| 走る場所 | 例外文脈 (Handler モード) | **スレッドモード** (呼ばれた側として) |
| 役割 | 機構。経路を決めてプリミティブを実行する | 方針。番号を解釈して担当へ配る |
| 番号の知識 | **持たない** | 持つ (それが仕事) |

- **カーネルが持つのは登録された entry 1 個** (`{ entry_pc, cookie, protection }`)。
  参照実装の `cpu_manager::svc_handler_descriptor` と同型。**番号 → ハンドラの表は
  カーネルに置かない** (置くと I-1 違反 + 方針がカーネルに入る)
- 信頼された活性化の svc → プリミティブを直接実行し、**カーネルの svc ハンドラ内で
  完結**する (オブジェクトランドのハンドラは経由しない)
- それ以外の svc → **登録済みのオブジェクトランドのハンドラをメソッドとして呼ぶ**。
  トランポリンは特別な機構ではなく、CALL と同じ「保護されたサブルーチン呼び出し」の
  一形態。実装でも `do_call()` 1 本を共有させ、これが分岐しないことをコードで担保する
- 番号での振り分け (サブカーネルへの委譲) はオブジェクトランド側の仕事。
  カーネルは番号の意味を持たない
- **したがってカーネルの語彙に「未知の番号」は存在しない**: 信頼された活性化しか
  プリミティブ選択に到達せず、そこで番号が引けないのは**カーネル自身の不変条件の
  破れ**なので panic する (参照実装と同じ)。信頼されていない活性化の svc は番号を
  見られることなく渡されるので、そもそも「未知」の判定が起きない。
  同様に「権限がない」も存在しない — 経路そのものが I-2 を強制する

**なぜハンドラをスレッドモードで走らせる必要があるか** (例外ハンドラ内で直接呼べば
1 回の例外で済むのでは、への回答。3 つとも回避不能):

1. **ARMv8-M の Handler モードは常に特権**。CONTROL.nPRIV は Thread モードにしか
   効かないので、例外ハンドラ内でサブカーネルを走らせると、ルート表に登録された
   だけのオブジェクトが全特権を得る = I-8 の否定であり、参照実装が削除した
   `SET_SVC_HANDLER` 乗っ取りの穴そのもの
2. **Handler モードからは svc を撃てない** (同優先度で HardFault へエスカレート)。
   ハンドラが CALL / RETURN を撃てなくなる
3. **スタックが MSP になる**。オブジェクト由来の処理が共有例外スタックを食うので、
   深い再帰が系全体を落とす (§8.1 の「消費したスレッドだけを止める」が崩れる)

参照実装で REDISPATCH_SVC が METHOD_CALL に対して持っていた優位と、その帰結:

| 参照実装での優位 | 新設計では |
|---|---|
| identity が潰れない (METHOD_CALL は kobj に化けた) | caller_cookie で CALL 側が獲得 → 差なし |
| 元 syscall の r0-r3 が潰れない | CALL は引数明示。番号 (r0 予約) + 実引数 r1-r3 = 4 スロットで収まる → 差なし |
| in_handler=1 で戻り経路にプリミティブが撃てる | **ネスト数申告 + 両側チェックで戻れる** (ユーザー指摘の核心)。そもそも I-8 でハンドラ特権は廃止済み → 差なし |
| カーネルが退避フレームから元引数を復元 (転送ミス防止) | 転送するのは信頼境界内の kobj のみ。identity e2e 自己テストが同じ網で捕まえる → 自己テストで代替 |
| REPLACE モード (exit, DESIGN §9.4) | `RETURN(n=2, depth_claim)` で同値 (PORT §4.5 の退避策)。+1 は kobj の 1 ヶ所に閉じ、I-6 的にも kobj が自分の積んだ 2 枚を落とすだけなので綺麗 |

副次効果 (すべて簡素化方向):
- サブハンドラ用 naked shim が不要になる (委譲先は普通の export メソッド)。
  shim が要るのはカーネル→kobj のトランポリン 1 ヶ所だけになり、
  「shim 自作は必ず間違える」地雷の面積が縮む
- §4.5 の「末尾委譲禁止」が規律でなく**機構的に不可能**になる
- `inv_svc_num / inv_caller_obj / inv_caller_thread` が完全に消える (PORT §4.6 予告の完成)
- kobj のルート委譲は「表引き → CALL_STRICT(svc番号, r1-r3)」に一本化。
  サブカーネルの「方針の局所性」は保たれ、さらに下への委譲も従来どおり kobj 経由

再訪条件: サブシステムが「kobj を 1 呼び出しごとに経由しない生 syscall 引き受け」を
性能上必要とした場合 — ただしそれは PORT §3.4 が会計成立のために明示的に退けた方向
なので、実測で問題が出てから議論すること。

## 4. 名前の対応表 (参照実装 → 新実装)

| 参照実装 | 新実装 (案) | 備考 |
|---|---|---|
| `shizu::` | `shizuku::` | 現行リポジトリに合わせる |
| `context_t` (固定オフセット) | `archs::armv8m::context_t` | オフセットは asm と .equ/static_assert で両縛り (手法継承) |
| `exception_frame_t` | `archs::armv8m::exception_frame_t` | サイズ/幾何は arch concept の関数経由でしか触らない (PORT §5.2) |
| `call_frame_hdr_t` + push/pop | `templates::kernel` 内 + arch 幾何関数 | 幾何計算のうち ISA 固有部 (fs, +8 余裕, has_fp) を arch へ寄せる |
| `kernel_object_svc_num` (200 番台) | カーネルプリミティブ enum (D3 の 5 種に整理) | METHOD_CALL/EXIT → CALL/RETURN。番号は新規採番 |
| `obj_api::svc_num` | kobj の svc ハンドラ API | 表・方針はすべて kobj 側 |
| `cpu_manager::current_thread_id[2]` | `templates::cpu_manager` (per-core 配列) | 現行骨格の cpu_manager を維持しつつ per-core 状態の置き場に |
| `FOR_KERNEL_OBJECT::*` | kobj からカーネルへの C++ 直接呼び出し層 | 信頼境界内の直結 API (svc を撃たない) |
| `thread_t::state_t` + claim CAS | `templates::thread` + `arch::cas` | RESERVED 状態と READY release-publish を必ず継承 |
| `svc_handler_shim<F>` / `wrapper<T>` | `archs::armv8m::handler_shim<F>` | ABI シムは arch 持ち (naked asm のため) |
| `error_t<E>` / `svc_result_t` | `templates::wire_result.hpp` (新規) | 0=成功規約ごと移植 |
| panic_ring / svc_trace | `internal_headers/shizuku/diag/` + board の noinit 供給 | noinit RAM の実体化は board 依存 |

## 5. 現行骨格からの差分まとめ (何を残し何を変えるか)

- 残す: リポジトリレイアウト、configs 機構、modules 機構、`result<T>`、
  concepts+templates の様式、`static_table` (疎テーブルが乗るまでの土台として)
- 変える: `templates::kernel` の型パラメータ (OBJECT を外し ARCH/BOARD を入れる)、
  `templates::object` (kobj 側へ移動・全面再設計)、`cpu_drivers/` → `archs/`+`boards/`、
  `abis/` の svc ラッパ (新 ABI で書き直し。Q1 の決定に従う)
- 捨てる: `source/kernel.cpp` のスケッチ、`kernel_object_abi/` (ガード衝突ファイル)、
  `dummy.hpp` の重複定義、obj_type の特権 enum、`modules/pico_sdk_support/main.cpp` の実験コード
