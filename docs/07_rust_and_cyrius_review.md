# 07. Rust 移行の再検討と Cyrius との比較 (2026-10-02)

対象: `feat/object-parent-handler-routing` (f9df772) + 本ブランチのモジュール分割。
比較先: [n4mlz/Cyrius](https://github.com/n4mlz/Cyrius) (81c9274, 2026-02-27)。

D16 (§3.5「カーネルコアは C++ 維持、Rust は XNO 側の将来オプション」) を前提に、
**D16 の理由が今も成り立つか**と、**移るならどの順か**を書く。結論を先に書くと、
D16 の結論は維持、ただし理由 1 の一部は事実として古いので直す。

---

## 1. 規模と形 (2026-10-02 時点の実測)

| 層 | 行数 | 中身 |
|---|---:|---|
| コア (`source/kernel`, `source/kernel_object`, `internal_headers/shizuku`) | 約 3,900 | 機構 + kobj。テンプレート + concept で ARCH/BOARD を注入 |
| ポート (`modules/pico_sdk_support/rp20x0`) | 約 1,800 | 文脈退避 .S、例外入口、MPU、board |
| オブジェクト (`objects/`, `rp2350/objects`) | 約 6,600 | flash_fs / usb_cdc / peripherals / ble_uart / ota / gdb_stub |
| 自己テスト | 約 2,300 | 梯子 (仕様書) |

インライン asm は C++ 側に 22 箇所、文脈退避は .S が 2 本 (v6m / v8m)。

## 2. D16 の 4 理由の再点検

1. **「no_std の core/alloc 再ビルドは nightly + cargo 前提」→ 半分は古い。**
   `thumbv6m-none-eabi` (RP2040) と `thumbv8m.main-none-eabi[hf]` (RP2350) は
   Tier 2 で、**ビルド済みの `core`/`alloc` が stable で配られている**。
   `-Z build-std` が要るのは Tier 3 ターゲットか、std を再設定したいときだけ。
   rules_rust も `extra_target_triples` でこれらを引ける。つまり
   「Rust を入れたら nightly + cargo が必須」は ARM の 2 チップについては成り立たない。
   ただし**クレートの依存解決 (crate_universe) は cargo を内部で回す**ので、
   外部クレートを使う限り「Bazel の中に cargo が居る」ことは変わらない。
   - ★浮動小数の ABI に注意: 現行は `-mfloat-abi=softfp`。Rust 側は
     `thumbv8m.main-none-eabihf` (hard) ではなく `thumbv8m.main-none-eabi` +
     `target-feature=+fp-armv8d16sp` 相当で揃える必要がある (呼出規約が違う)。
2. **「最難関は unsafe の中に残る」→ 変わらない。** 例外フレーム幾何・PendSV・
   svc シムは Rust でも `global_asm!` / `#[unsafe(naked)]` (1.88 で安定化) の中。
   型が守る範囲の外という点は同じ。
3. **「書き直しコストは片方向にしか安くない」→ 変わらない。** 自己テスト
   (93 passed) が通っている 3,900 行のコアを作り直す利益が見えていない。
4. **「Rust が効くのは XNO 側」→ むしろ強まった。** オブジェクトが 6,600 行に
   育ち、その中に**信用できない入力を舐めるパーサ**が 3 つある:
   `ota` (inflate, 328 行のヘッダ + 1,362 行)、`gdb_stub` の RSP パーサ、
   `ble_uart` の書き込み処理。所有権・境界検査が本当に仕事をするのはここ。

## 3. 移行するなら (推奨順)

**移行の継ぎ目は C++ のヘッダではなく svc**。カーネル ABI は
「a0 = 番号、a1..a4 = 引数 / 戻り a0 = エラー、a1 = 値」のレジスタ規約
(`kernel_abi.hpp`) なので、Rust のオブジェクトは C++ の型を 1 つも見ずに
`asm!("svc ...")` の薄いラッパだけで書ける。D16 の「ABI 境界に C++ 型を
露出させない」が効いている。

| 段 | 内容 | 前提 / 判断基準 |
|---|---|---|
| 0 | `tools/gen_object_ids.py` に `.rs` 出力を足す (番号の生成を 1 か所に保つ、D28) | 今すぐできる |
| 1 | `shizuku-abi` crate (no_std, 依存ゼロ): svc ラッパ・`result`・stream ハンドル | crate_universe を使わない = cargo 不要で rules_rust だけで組める |
| 2 | 新規オブジェクト 1 個を Rust で書き、`rust_static_library` を firmware にリンク | MPU 保護下の非特権で selftest の拒否テストが通ること |
| 3 | パーサ系 (ota の inflate → RSP パーサ) を Rust へ | 実バグ・ファジングで差が出たら |
| 4 | コア | D16 の再訪条件 (a)(b) を満たしたときだけ。満たしていない |

**やらない方がよいこと**: pico-sdk (TinyUSB / BTstack / CYW43) を bindgen 越しに
Rust から叩くこと。embassy-rp / rp-hal に乗り換えるなら別 OS を作るのと同じ規模に
なり、D17 の「XNO 側の実験は Shizuku 本体を汚さない」に反する。

## 4. Cyrius との比較

Cyrius は「コンテナを OS の第一級オブジェクトにする」Type-1 コンテナランタイム OS。
OCI ランタイムをカーネル内に持ち、ホスト世界とコンテナ世界の 2 つの syscall 表を
`Process.abi` で切り替える (コンテナ側は Linux 互換 ABI)。

| 観点 | Shizuku | Cyrius |
|---|---|---|
| 対象 | Cortex-M0+/M33 (RP2040/RP2350)、MPU のみ・MMU 無し | x86_64 UEFI、MMU・ページング前提 |
| 言語 / ビルド | C++23 + Bazel (pico-sdk BCR) | Rust nightly-2025-09-04 + cargo xtask |
| 規模 | 約 2 万行 (docs 込み) | 約 2.6 万行 Rust (unsafe 253 箇所) |
| 第一級の単位 | **オブジェクト** (メソッド表・呼び出しフレーム・実行権の貸し借り) | **コンテナ** (OCI Spec + 状態 + プロセス集合 + 専用 VFS) |
| 隔離 | MPU region + 特権/非特権 + 呼び出しの段数検査 | アドレス空間 + コンテナごとの VFS (chroot 脱出を構造で防ぐ) |
| ABI | 独自の svc (4 プリミティブ + オブジェクト呼び出し) | ホスト用 (create/start/stop) と Linux 互換の 2 表 |
| 抽象化 | テンプレート + concept (`arch_requires` / `board_requires`)、構成は config.hpp | trait (`ArchPlatform` / `ArchThread` …)、`cfg` で x86_64 を選ぶ |
| 差し替え | D58: syscall ハンドラと kobj の**再登録**でホットスワップ | 想定していない (コンテナの入れ替えが単位) |
| 検証 | 実機の自己テスト梯子 (拒否テスト必須) | QEMU 上の `cargo xtask test` |

### 似ている点

- **「OS が責任を持つ実体を 1 つ決め、それに状態を持たせる」**という発想。
  Cyrius はコンテナ、Shizuku はオブジェクト。どちらも「プロセスの属性の寄せ集め」
  からの脱却が動機になっている (Cyrius README の Motivation、Shizuku の D58)。
- **ABI を種別で切り替える**: Cyrius は `Process.abi` で表を選び、Shizuku は
  `object_kind` (PLAIN / HANDLER) で経路を選ぶ。番号ではなく「誰が走っているか」で
  決める点が同じ。ただし Shizuku 側は**互換オブジェクトを作れる構造が既にある**:
  合成主体 (ROOT / kobj) が `INTERNAL_FLAG_HANDLER` 付きで作ったオブジェクトは
  HANDLER になり、そのハンドラが作ったオブジェクトの svc は親ハンドラ
  (`parent_handler_object` / `parent_handler_entry`) へ届く
  (`handler.cpp` の `create_object`)。つまり Cyrius が syscall 表 2 本を
  カーネルに焼き込んでいるところを、Shizuku は「ABI = 差し替え可能なハンドラ
  オブジェクト」として持っており、表の数は構成で決まる。
- arch 抽象の切り方: Cyrius の `ArchPlatform / ArchTrap / ArchThread / ArchMemory`
  は Shizuku の `concepts::arch_requires` + `board_requires` とほぼ同じ境界。
  Rust 化するなら trait + const generics (`OBJECT_COUNT` 等) にほぼ 1 対 1 で写る。

### 違う点 (そのまま真似できない理由)

- **MMU が無い**。Cyrius の隔離はアドレス空間とコンテナ VFS に乗っているが、
  Shizuku は MPU region (RP2350 で 8 本) の付け替えでしか守れない。D58 の
  「保護付き / カーネル空間オブジェクトの 2 種別」は、まさにこの制約への答え。
- **Cyrius は Linux 互換を目的の一部にしている**。ただしその互換層
  (`syscall/linux.rs` 約 2,400 行 + VFS / プロセス / tty) は、Linux の意味論を
  カーネル内に作り直すものなので、規模が育つほど実質「Linux を fork して
  持ち歩く」のと同じ保守を背負う。コンテナを第一級にする理由が「Linux の
  寄せ集めから逃れる」ことなのに、その中身が Linux の再実装になるのは
  緊張関係にある (Cyrius 自身も「full parity は目指さない」と範囲を絞っている)。
  Shizuku は互換を持たない代わりに、
  呼び出しフレーム・実行権の貸し借り (GRANT) を ABI の一次語彙にしている。
  リアルタイム側の保証 (期限付きの貸し、強制回収) は Cyrius に相当物が無い。
- **Rust の効き方**: Cyrius は unsafe 253 箇所 / asm 系 9 箇所で、unsafe の多くが
  ページテーブル・MMIO・コンテキスト切替に集中している。つまり Cyrius でも
  「最難関は unsafe の中」(D16 理由 2) は同じで、Rust が稼いでいるのは
  VFS・ネットワーク・コンテナ表といった**データ構造の層**。Shizuku に当てはめると
  §2 の 4 (オブジェクト側、とくにパーサ) が該当し、§3 の順序と一致する。

### Shizuku に持ち込む価値があるもの

1. **外部からオブジェクトを構築する API** (Cyrius の create → start に相当する話)。
   Shizuku でも「作る」と「走らせる」は既に別操作になっている —
   `CREATE_OBJECT` は台帳に載せて入口 (methods[0]) を据えるだけで、走らせるのは
   同期の `CALL_METHOD` か非同期の `SPAWN`。足りないのは、2 個目以降のメソッドを
   **オブジェクト自身が** `EXPORT_METHOD` で登録する作りなので、使える形に
   するには一度 main を呼んで自分で組み立てさせる必要がある点 (selftest も
   CREATE_OBJECT の直後に CALL している)。生成側 (合成主体 / 親ハンドラ) が
   相手のメソッド表・保護設定を外から組める API が入れば、「構築済みだが
   まだ一度も走っていない」状態を作れ、D58 の再登録 (ホットスワップ) でも
   差し替え先を走らせずに据えてから切り替えられる。
2. **オブジェクトごとの名前空間**。Cyrius はコンテナ VFS によって、プロセスが
   ホストのパスを解決できないことを**構造で**保証している。Shizuku の flash_fs は
   今はグローバルなので、オブジェクト単位のハンドル (ストリーム) からしか触れない形に
   寄せると、「気をつけるで守らない」(05_handoff §5) に合う。
3. **設計メモの置き方**: Cyrius はモジュールごとに `DESIGN.md` を置いている。
   Shizuku は 03_porting_policy.md に集約しているので、今回分けた
   `modules/pico_sdk_support/{rp2040,rp2350,objects}` に短い README を置くと
   決定 (D-n) への索引として効く。

## 5. 未決にしておくこと

- rules_rust を MODULE.bazel に入れるかどうか (§3 段 1 から必要)。入れるなら
  **crate_universe は使わない** (依存ゼロの crate だけにする) ことを決定事項にするか。
- `gen_object_ids.py` の `.rs` 出力を先に入れるか (Rust を使わなくても無害)。
