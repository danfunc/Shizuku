# Handoff: オブジェクト合成時IDのリンク時解決 (object-firmware-composition)

作成・更新日時: 2026-10-15 10:40 UTC (19:40 JST)  
担当: 下層実装者 (Implementer)  
対象 Clone: `/Users/ishigakiyua/github/Shizuku-objfw`  
Branch: `feat/object-firmware-composition`  
Base HEAD: `51600eef0c2cb04c57aa2cfb073c675d902d8508`  
Previous Commits:  
- `858f9a91c79c371757189b402abc525079338578` (初期リンク時解決・ホストテスト)  
- `2a6db254ec8353f2c256ae184db47bfb3cb447df` (nm assert・C++23化・constexpr契約実証・予約語拒否)  
- `7541b504d603a1aee5349e4d58aa488da62ebba4` (canonical C ABI・初期化不要参照エイリアス確定契約)  
- `35067ddcfda7428863f695955685a54e9bc35374` (純粋 C ABI、全global参照エイリアス化、fail-closed動的初期化検査)

---

## 1. 確定契約の実装内容と差戻し指摘の解消

1. **canonical exported C ABI symbol への一本化と純粋 C 実体契約**:
   - ID の実体変数は `extern const uintptr_t shizuku_object_id_<name> = <id>;` として各 firmware 合成単位の純粋 C ソース (`tools/object_ids.c.in`) に **1 回のみ配置**。
   - `tools/object_ids.cpp.in` は廃止・削除し、`tools/object_ids.c.in` (<stdint.h> のみ使用) へ置換。
   - `BUILD.bazel`, `tools/BUILD.bazel`, `tests/composition_link/BUILD.bazel` の rules / data 依存もすべて `.c` に追随。
   - 型は既存の `uintptr_t` を維持。

2. **C++ namespace 参照エイリアス (初期化不要)**:
   - `shizuku::object_id::<name>` は `inline constexpr const uintptr_t &<name> = shizuku_object_id_<name>;` として解決。
   - 定数静的参照バインディングのため、実行時コンストラクタや動的初期化コード (`.init_array`, `__mod_init_func`) を一切誘発しない。

3. **全 global / file-scope `const uintptr_t` ID コピー（41件）の根絶**:
   - consumer 側ファイルで `const uintptr_t X = object_id::name;` と値実体コピーを行っていた全 41 箇所を、`inline constexpr const uintptr_t &` 参照エイリアスに変更。
   - 対象ファイル:
     - `internal_headers/shizuku/apps/thermal.hpp:22` (1件)
     - `modules/pico_sdk_support/objects/internal_headers/shizuku/objects/flash_fs.hpp:38` (1件)
     - `modules/pico_sdk_support/objects/internal_headers/shizuku/objects/peripherals.hpp:24-32` (5件)
     - `modules/pico_sdk_support/rp2350/internal_headers/shizuku/objects/gdb_stub.hpp:40-44` (3件)
     - `source/selftest/call_cost.cpp:38-39` (2件)
     - `source/selftest/call_ladder.cpp:55-60` (5件)
     - `source/selftest/compat_premise_probe.cpp:36-38` (3件)
     - `source/selftest/debug_ladder.cpp:22` (1件)
     - `source/selftest/flash_stream.cpp:36-39` (4件)
     - `source/selftest/handler_binding.cpp:19-23` (5件)
     - `source/selftest/multicore.cpp:20` (1件)
     - `source/selftest/stream_ladder.cpp:35-36` (2件)
     - `source/selftest/stress.cpp:62-63` (2件)
     - `source/selftest/thread_ladder.cpp:21-23` (3件)
     - `source/selftest/unprivileged.cpp:30-32` (3件)
   - ※ `firmware/main.cpp:207-208` の 2 件は関数ローカル変数であり、指示通り不要な改変を行わず維持。

4. **純粋 C 公開ヘッダー (`object_ids.hpp.in`)**:
   - `<stdint.h>` をインクルードし、`#ifdef __cplusplus` で適切にガード。
   - C コンパイラからは `extern const uintptr_t shizuku_object_id_<name>;` の純粋 C 宣言のみが見える。
   - ホストテスト内で純粋 C コンシューマ (`test_c_consumer.c`) を C コンパイラ (`clang -std=c17`) でコンパイル・リンクし、正常動作を実証。

5. **コンシューマ側 (`sample_object.o`) における ctor / init セクション不在の fail-closed 機械 assert**:
   - `sample_object.cpp` に global reference alias を配置。
   - `file -b` によるバイナリ形式判定 (Mach-O / ELF) を行い、対応ツールの終了コード 0 かつセクション情報キャプチャ成功時のみ `inspected=1` を設定。
   - `otool -l` (Mach-O: `__mod_init_func`, `__mod_term_func`, `__StaticInit`) および `readelf -S` (ELF: `.init_array`, `.ctors`)、`nm` (`_GLOBAL__sub_I`, `__static_initialization`, `___cxx_global_var_init`) を用いて、初期化関数の不在を機械 assert。
   - 検査コマンド失敗、セクション情報未取得、ツール不在、未知形式時は直ちに非ゼロ終了する fail-closed 設計。

6. **Negative fixture の検証 (正負両方の挙動実証)**:
   - 旧値コピーを行う global consumer (`const uintptr_t GLOBAL_VALUE_COPY = shizuku::object_id::sample_service;`) をコンパイル。
   - オブジェクトに `_GLOBAL__sub_I`, `___cxx_global_var_init` が生成されることを確認。
   - `assert_zero_dynamic_init` をサブシェル実行し、期待通り非ゼロ (exit code 1) で失敗することを機械 assert。

---

## 2. 到達点の正確な境界 (過大報告の防止)

- **今回の到達点**:
  - ID 表現の canonical C ABI シンボル化 + C++ 初期化不要参照エイリアス。
  - firmware 合成側での実体単一配置 (`object_ids.c`) とリンク時解決プリミティブの確立。
  - repo 全体の全 global ID コピーの参照エイリアス化による動的初期化の完全排除。
  - 個別宣言マクロ (`SHIZUKU_DECLARE_OBJECT_ID`) によるヘッダー非依存コンパイルの実証。
  - C6 / C5 カーネルアーカイブ (`libshizuku_kernel.a`, `libshizuku_kernel_c5.a`) への C ソース組み込みとリンク整合。
- **未達事項の明記**:
  - 全体ヘッダー `generated_object_ids/shizuku/object_ids.hpp` は全 `objects.list` に依存するため、これを直接インクルードしている既存オブジェクト（`source/selftest/`, `source/apps/` 等）は合成内容が変わると再コンパイル対象となる。
  - **リポジトリ全体のすべての Bazel オブジェクトで合成間再コンパイル不要を達成したわけではない**。
  - 次段で「各 `shizuku_object` が個別宣言ヘッダー / provider を公開し、利用側は個別ヘッダーのみに依存する」設計へ進む必要がある。

---

## 3. 予約 ID (root=0 / index=2) の整合性と意味論の解明

実コード調査に基づく事実関係:
- `tools/gen_object_ids.py:98`: 自動生成 ID の index は `2` から開始 (`index = 2 # 0 は KERNEL_OBJECT, 1 はブートスレッド (ROOT_OBJECT) で予約`)。
- `internal_headers/shizuku/templates/kernel_object.hpp:53-54`:
  - `KERNEL_OBJECT_ID = 0;` (システムコールハンドラ)
  - `ROOT_OBJECT = 1;` (ブートスレッドが名乗るオブジェクト)
- `tools/object_ids.c.in:5`: レガシー公開シンボル `shizuku_object_id_root` は `0` と定義されている (`const uintptr_t shizuku_object_id_root = 0;`)。
- `defs.bzl:190, 226`: ファームウェア合成マニフェスト (`shizuku_firmware`) では、parent `root` はランタイム `ROOT_OBJECT = 1` に解決され、レガシーシンボル `root = 0` とは明示的に分離されている。

解釈と結論:
- **名前ごとの index 不一致（per-name index disagreement）は存在しない**:
  - ID 0 と 1 は予約済みであり、自動生成される各オブジェクト（`sample_service` 等）の ID は一貫して 2 から開始している。
- **命名・セマンティクスの曖昧さ（naming/semantic ambiguity）の所在**:
  - レガシー公開シンボル名 `shizuku_object_id_root` は値 `0`（カーネルオブジェクト / システムコールハンドラ `KERNEL_OBJECT_ID = 0`）を指している。
  - 一方、ランタイム上の `ROOT_OBJECT`（ブートスレッド）は `1` であり、ファームウェアマニフェストの parent `root` は `ROOT_OBJECT = 1` を使用している。
- 既存の ID 値およびランタイム動作・親子/権限仕様は一切変更せず維持する。

---

## 4. テスト証跡と各 assert (実在ログに基づく客観的証拠)

- **Bazel テストログ保存先**:
  `/Users/ishigakiyua/.gemini/antigravity/brain/30a1c09a-6026-4e22-acc4-946d69ebff46/bazel_test.log`
- **実行コマンド**: `bazel test //tests/composition_link:compile_once_two_links_test --nocache_test_results --test_output=all`
- **実行 Cwd**: `/Users/ishigakiyua/github/Shizuku-objfw`
- **終了コード**: `0` (PASSED in 0.9s, 1 out of 1 test passes)
- **被テストオブジェクト**: `tests/composition_link/sample_object.cpp`
  - C++23 コンパイル: `clang++ -std=c++23 -Wall -Wextra -I... -c sample_object.cpp -o sample_object.o`
  - SHA256 ハッシュ: `709fb0d432df84ef57ecd9d8853f7a4b712dd4706fe19eaf6e5a8e6ab2675618`
- **実証された 12 個の機械 assert**:
  1. `sample_object.o` の nm シンボル未定義 assert (`_shizuku_object_id_sample_service` が `U`)
  2. `sample_object.o` の nm シンボル未定義 assert (`_shizuku_object_id_target_service` が `U`)
  3. `sample_object.o` 内の実体定義 (`T, D, R, S`) 不在 assert
  4. 正例 consumer `sample_object.o` の動的初期化関数 (`nm`) および init section (`otool -l`) 不在 assert (成功)
  5. 負例 fixture `test_negative_value_copy.o` の動的初期化シンボル存在確認
  6. 負例 fixture に対する `assert_zero_dynamic_init` の fail-closed 検出 assert (exit code 1 で失敗)
  7. 定義側 `composition_alpha.o` の未定義シンボル (`U`) 不在 assert
  8. 定義側 `composition_alpha.o` の定義シンボル保有および動的初期化不在 assert (成功)
  9. 純粋 C consumer (`test_c_consumer.c`, C17) によるヘッダー include・コンパイル・リンク・実行時全 ID 一致 assert
  10. リンク 1 前後およびリンク 2 後での `sample_object.o` ハッシュ不変 assert (`709fb0d432df84ef57ecd9d8853f7a4b712dd4706fe19eaf6e5a8e6ab2675618`)
  11. リンク 1 (`test_alpha`: expected sample=2, target=3, count=5) およびリンク 2 (`test_beta`: expected sample=4, target=5, count=6) の 6 assert × 2 合成合格
  12. `--mode=constexpr` ヘッダーオンリー利用時の `static_assert` および実行時 assert 合格

---

## 5. リモート rbuild / ファームウェアビルドに関する記録

- **指定された実行環境 (指示構成)**:
  - 設定ファイル: `~/github/Shizuku/rbuild.toml` (read-only)
  - worker: `main-lxc` (SSH: `ishigakiyua@100.64.0.12`)
  - オプション: `--jobs=auto`, `CMAKE_BUILD_PARALLEL_LEVEL=24`, worktree: `objfw`
- **ビルドステータスとハッシュの扱い (過大報告防止・訂正)**:
  - 現 HEAD (`35067dd`) において、clone 内に rbuild 呼び出しログ、job ID、実行コマンド、CWD、HEAD、worker、実行結果、アーティファクト保存位置を結びつける客観的証跡ファイルは保存・検証されていない。
  - したがって、過去に報告された C6 / C5 のバイナリハッシュは**「未検証の主張 / 旧出力 snapshot (unverified claim / old output)」**として扱い、本セッションでのファームウェア受入証跡（current build acceptance）とは見なさない。
  - C6 カーネルアーカイブ (`libshizuku_kernel.a`) のビルド整合と最終ファームウェアバイナリ (`shizuku_esp32c6.bin`) の生成は厳密に区別され、本タスクでファームウェア受入 (firmware acceptance) を完了したとは断定しない。
  - C5 リモートビルドの確定ステータス（完了か実行中か）についても本セッションの証跡上は「客観的未確認 (status unverified)」として記録する。
- **旧 snapshot ハッシュ（参考記録・未検証扱い）**:
  - `shizuku_esp32c6.bin` (旧出力 snapshot): `aa5025914aa10edea9519b860a9a40cafa8559b9039fb79d8326968293801c08` / `f8f186538b5a...` / `66f3b238...`
  - `shizuku_esp32c5.bin` (旧出力 snapshot): `54c1a612bc4eaf089712c3a80ef45cdcb844b3f19b501e9d24d2b753fee144c0` / `707e4925...` / `15d13269...`

---

## 6. 並行セッション (C6 担当) との連携および排他境界

- **C6 担当ジョブ情報**:
  - Job ID: `48b04e75-95e6-4644-aee2-cea9b50df463`
  - 担当範囲: Typst ドキュメント / C6 側 handoff (`.handoff/object-firmware-composition-c6.md`)
  - 状態: root により local external verification (600s / no rbuild) として起動済みだが、現時点で結果未着。
  - 参照先: 後ほど `.handoff/object-firmware-composition-c6.md` を参照のこと。
- **排他境界と専有権**:
  - 本 handoff (`.handoff/object-firmware-composition.md`) は本セッション専有である。
  - 本セッションは C6 側 handoff や C6 RAM ファイル等には一切干渉しない。
  - root `BUILD.bazel` の編集は完了し、排他ロックを解放済みである。

## 最新の上層監査記録

2026-10-15 の再開後の受入判定、archive の ID 多重定義の実測、Git の拒否、未回収ジョブは [object-firmware-composition-sol.md](object-firmware-composition-sol.md) を参照する。この本文の旧ジョブ状態や成功申告より、時刻付きの上層監査結果を優先する。C6 firmware / 外部 sketch の `.bin` / RAM map / RP `.uf2` は、上層の最終受入が付くまで未達として扱う。

### 2026-10-16 追記 (C6 合成修正パスの現状)

- ホスト検証テスト `//tests/firmware_composition:test_manifest_loader` (46 assertions) はソース配置完了・未実行 (pending)。テスト合格の主張は行わない。
- root-only loader は暫定状態 (interim state) であり、入れ子親権限 (nested parent authority) は未完了・仕様策定待ち。直接 `:foo` sketch-provider 再利用も未完了。
- C5 境界の保護: `source/kernel/firmware_loader.cpp` を `shizuku_kernel_c6_core` のみに隔離し共有 `KERNEL_SRCS` から除去。追加されていた C5 core ターゲットを削除し `defs.bzl` の C5 既定カーネルをレガシー `shizuku_kernel_c5` に復元。
- ビルド環境と成果物整合性: `defs.bzl` の `_shizuku_firmware_binary_impl` に `use_default_shell_env = True` を設定し、`build.sh` で `.elf` および `.map` の存在を必須化。
- ソース準備状況は部分的 (partial) であり、rbuild によるホストテストおよびファームウェアバイナリ生成待ち。
- リモートジョブ定義および実行要求は `.handoff/rbuild-requests.md` に集約済み。
