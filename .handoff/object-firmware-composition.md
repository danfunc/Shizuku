# Handoff: オブジェクト合成時IDのリンク時解決 (object-firmware-composition)

作成・更新日時: 2026-10-15 10:35 UTC (19:35 JST)  
担当: 下層実装者 (Implementer)  
対象 Clone: `/Users/ishigakiyua/github/Shizuku-objfw`  
Branch: `feat/object-firmware-composition`  
Base HEAD: `51600eef0c2cb04c57aa2cfb073c675d902d8508`  
Previous Commits:  
- `858f9a91c79c371757189b402abc525079338578` (初期リンク時解決・ホストテスト)  
- `2a6db254ec8353f2c256ae184db47bfb3cb447df` (nm assert・C++23化・constexpr契約実証・予約語拒否)  
- `7541b504d603a1aee5349e4d58aa488da62ebba4` (canonical C ABI・初期化不要参照エイリアス確定契約)  

---

## 1. 確定契約の実装内容と差戻し指摘の完全解消

1. **canonical exported C ABI symbol への一本化と純粋 C 実体契約**:
   - ID の実体変数は `extern const uintptr_t shizuku_object_id_<name> = <id>;` として各 firmware 合成単位の純粋 C ソース (`tools/object_ids.c.in`) に **1 回のみ配置**。
   - `tools/object_ids.cpp.in` は廃止・削除し、`tools/object_ids.c.in` へ置換。`<stdint.h>` のみを使用。
   - `BUILD.bazel`, `tools/BUILD.bazel`, `tests/composition_link/BUILD.bazel` の rules / data 依存もすべて追随。
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
   - `otool -l` (Mach-O: `__mod_init_func`, `__mod_term_func`) および `readelf -S` (ELF: `.init_array`, `.ctors`)、`nm` (`_GLOBAL__sub_I`, `__static_initialization`) を用いて、初期化関数の不在を機械 assert。
   - 検査ツールが検出できない場合はテスト失敗 (fail-closed) とする安全設計。

---

## 2. 到達点の正確な境界 (過大報告の防止)

- **今回の到達点**:
  - ID 表現の canonical C ABI シンボル化 + C++ 初期化不要参照エイリアス。
  - firmware 合成側での実体単一配置 (`object_ids.c`) とリンク時解決プリミティブの確立。
  - repo 全体の全 global ID コピーの参照エイリアス化による動的初期化の完全排除。
  - 個別宣言マクロ (`SHIZUKU_DECLARE_OBJECT_ID`) によるヘッダー非依存コンパイルの実証。
  - ファームウェアアーカイブ (`libshizuku_kernel.a`) とのリンク整合。
- **未達事項の明記**:
  - 全体ヘッダー `generated_object_ids/shizuku/object_ids.hpp` は全 `objects.list` に依存するため、これを直接インクルードしている既存オブジェクト（`source/selftest/`, `source/apps/` 等）は合成内容が変わると再コンパイル対象となる。
  - **リポジトリ全体のすべての Bazel オブジェクトで合成間再コンパイル不要を達成したわけではない**。
  - 次段で「各 `shizuku_object` が個別宣言ヘッダー / provider を公開し、利用側は個別ヘッダーのみに依存する」設計へ進む必要がある。

---

## 3. テスト証跡と各 assert

- **Bazel テストログ保存先**:
  `/Users/ishigakiyua/.gemini/antigravity/brain/30a1c09a-6026-4e22-acc4-946d69ebff46/bazel_test.log`
- **被テストオブジェクト**: `tests/composition_link/sample_object.cpp`
  - C++23 コンパイル: `clang++ -std=c++23 -Wall -Wextra -I... -c sample_object.cpp -o sample_object.o`
  - SHA256 ハッシュ: `709fb0d432df84ef57ecd9d8853f7a4b712dd4706fe19eaf6e5a8e6ab2675618`
- **nm シンボル未定義 assert (sample_object.o)**:
  - `_shizuku_object_id_sample_service` が未定義シンボル (`U`) であること
  - `_shizuku_object_id_target_service` が未定義シンボル (`U`) であること
  - 実体定義 (`T, D, R, S`) が存在しないこと
- **動的初期化不在 assert (sample_object.o & composition_alpha.o)**:
  - `nm` (ローカル含む) で `_GLOBAL__sub_I` / `__static_initialization` / `___cxx_global_var_init` が不在
  - `file -b` による形式判定 (Mach-O / ELF) に応じたツールの exit 0 出力キャプチャ (`otool -l` / `readelf -S`)、セクション存在確認、および `__mod_init_func`, `__mod_term_func`, `__StaticInit`, `.init_array`, `.ctors` の不在を fail-closed で機械 assert
- **Negative fixture assert (値コピー global consumer の動的初期化検出と checker fail-closed 実証)**:
  - `const uintptr_t GLOBAL_VALUE_COPY = shizuku::object_id::sample_service;` を含む `test_negative_value_copy.cpp` をコンパイル
  - `nm` で `_GLOBAL__sub_I` / `___cxx_global_var_init` が生成されることを確認
  - `assert_zero_dynamic_init` をサブシェル実行し、期待通り非ゼロ (exit code 1) で失敗することを機械 assert
- **C 言語 consumer assert (`test_c_consumer.c`)**:
  - C コンパイラ (`clang -std=c17`) によるコンパイル + `composition_alpha.o` とのリンク・実行 assert (全 ID 一致)
- **ハッシュ不変 assert**:
  - リンク 1 直前およびリンク 2 直後で `sample_object.o` のハッシュ (`709fb0d432df84ef57ecd9d8853f7a4b712dd4706fe19eaf6e5a8e6ab2675618`) が完全に一致
- **リンク 1 (Composition Alpha: expected sample=2, target=3, count=5)**:
  - `test_runner.cpp`: 6 assert すべて合格
- **リンク 2 (Composition Beta: expected sample=4, target=5, count=6)**:
  - `test_runner.cpp`: 6 assert すべて合格
- **constexpr 契約 assert**:
  - `--mode constexpr` ヘッダーのみ利用で `static_assert` および実行時 assert 合格

---

## 4. リモート rbuild / ファームウェアビルド証跡

- 実行環境: `~/github/Shizuku/rbuild.toml` (read-only)、worker: `main-lxc` (SSH: `ishigakiyua@100.64.0.12`)
- **現作業ツリーでの実測ハッシュ**:
  - `shizuku_esp32c6.bin`:
    `aa5025914aa10edea9519b860a9a40cafa8559b9039fb79d8326968293801c08`
    (binary size `0x30b90` bytes, Exit code 0, 81% free)
  - `shizuku_esp32c5.bin`:
    `54c1a612bc4eaf089712c3a80ef45cdcb844b3f19b501e9d24d2b753fee144c0`
    (binary size `0x32a70` bytes, Exit code 0, 80% free)
- **旧 snapshot ハッシュ (参考・区別用)**:
  - 前回 HEAD (`7541b50`): C6 `f8f18653...` / C5 `707e4925...`
  - 初回 snapshot: C6 `66f3b238...` / C5 `15d13269...`

---

## 5. 変更対象ファイルと排他ロックの解放通知

- **コミット対象ファイル**:
  - `BUILD.bazel`
  - `internal_headers/shizuku/apps/thermal.hpp`
  - `internal_headers/shizuku/object_id_decl.hpp`
  - `modules/pico_sdk_support/objects/internal_headers/shizuku/objects/flash_fs.hpp`
  - `modules/pico_sdk_support/objects/internal_headers/shizuku/objects/peripherals.hpp`
  - `modules/pico_sdk_support/rp2350/internal_headers/shizuku/objects/gdb_stub.hpp`
  - `source/selftest/call_cost.cpp`
  - `source/selftest/call_ladder.cpp`
  - `source/selftest/compat_premise_probe.cpp`
  - `source/selftest/debug_ladder.cpp`
  - `source/selftest/flash_stream.cpp`
  - `source/selftest/handler_binding.cpp`
  - `source/selftest/multicore.cpp`
  - `source/selftest/stream_ladder.cpp`
  - `source/selftest/stress.cpp`
  - `source/selftest/thread_ladder.cpp`
  - `source/selftest/unprivileged.cpp`
  - `tests/composition_link/BUILD.bazel`
  - `tests/composition_link/run_compile_once_two_links_test.sh`
  - `tests/composition_link/sample_object.cpp`
  - `tools/BUILD.bazel`
  - `tools/gen_object_ids.py`
  - `tools/object_ids.c.in` (新設)
  - `tools/object_ids.cpp.in` (削除)
  - `tools/object_ids.hpp.in`
  - `.handoff/object-firmware-composition.md`
- **保護対象（コミット除外）**:
  - RAM マクロ (`chips/esp32_c6/chip.hpp`, `chips/esp32_c6/trap.cpp`, `chips/rp2350/chip.hpp`)
  - `modules/esp_idf_support/firmware/build.sh`
  - `docs/design/object_firmware_composition.pdf`, `.typ`
  - `defs.bzl`, `tests/composition_link/sample_sketch.cpp`
- **root BUILD.bazel の解放**:
  - 本ジョブでの `BUILD.bazel` 編集は完了しました。他セッション（C6 RAM 等）が利用可能です。
