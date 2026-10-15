# Handoff: Object Firmware Composition API (ESP32-C6 Slice)

- **Job ID**: `c619f1a0-afae-4cef-8e8d-6b14c96417c8`
- **Parent Job**: `c619f1a0-afae-4cef-8e8d-6b14c96417c8`
- **Followup / Caller Session**: `c619f1a0-afae-4cef-8e8d-6b14c96417c8`
- **Base Commit**: `a21a72d15e65986262aedb92a20024e7b6a8e807`
- **Branch**: `feat/object-firmware-composition`

---

## 1. 実装到達点 (Confirmed & Implemented)

1. **SDK非依存 公開 C ABI `shizuku_yield()` の実装**:
   - `internal_headers/shizuku/kernel_services.hpp` に宣言。
   - `source/kernel/services.cpp` に `object_api::YIELD` (index 8) を直接呼び出す RISC-V (`ecall`) / ARM (`svc 0`) 実装を配備。
   - `defs.bzl` の `_shizuku_sketch_entry_impl` 内で `loop(); shizuku_yield();` を呼び出すよう接続。架空シンボルを排除。
2. **SDK `build.sh` のインクリメンタルビルド対応**:
   - `modules/esp_idf_support/firmware/build.sh` において `BUILD_DIR/CMakeCache.txt` 内の `IDF_TARGET:STRING` を検査し、ターゲットが一致していれば `idf.py set-target` をスキップ。
   - `defs.bzl` の `_shizuku_firmware_binary` で `build_dir` 属性をサポート。
   - `modules/esp_idf_support/firmware/BUILD.bazel` で `build.sh` を `sh_binary` として公開。
3. **C6 Runtime Manifest Glue の実装**:
   - `modules/esp_idf_support/c6/early_boot.cpp` に弱シンボル参照による `shizuku_composition_manifest` 処理を実装。
   - 本番の `ARCH::syscall(CREATE_OBJECT, ...)` 経路でマニフェスト内の全オブジェクトを生成・検証。
   - `set_object_name` および `set_parent_handler` により名前と親関係を設定。
4. **複数 Sketch の衝突回避実証**:
   - `tests/firmware_composition/` に `sample_sketch_second.cpp` を追加（`setup_fn = "setup_second"`, `loop_fn = "loop_second"`）。
   - `sample_fw` に2つのスケッチを含めて合成し、`sample_fw_composition_gen_manifest.c` で `sample_sketch_obj` と `sample_sketch_obj_second` が別名エントリとして衝突なく生成されることを確認。
   - 単一アーカイブ `libsample_fw_archive.a` がビルド成功し、`riscv32-esp-elf-nm` で両方のエントリシンボル（`sample_sketch_obj_main`, `sample_sketch_obj_second_main`）および一意な ID シンボルが存在することを確認。
5. **Fail-Closed Negative Tests の実アサーション完了**:
   - `tests/firmware_composition/test_fail_closed.sh` により 2件の Positive テストと 7件の Negative テストが全て PASS（終了コード 0）。
6. **リモート rbuild Baseline 実証**:
   - `rbuild --worktree objfw job build_esp32c6` (exit 0, hash: `7a53781826125bb06fcda19295bbd5e9b517e418d16478eada52944e001ebbd4`)
   - `rbuild --worktree objfw job build_esp32c5` (exit 0, hash: `cae16ab68d96b4cf066ba35810787def412d9b58e35ccc23ab18dccc723b4975`)
   - 同一コミット・同一 worktree (`objfw`)・同一ワーカー (`main-lxc`) で C6/C5 の実証を達成。

---

## 2. 未達事項・技術境界 (Unresolved & Boundaries)

1. **外部 Consumer での最終 `.bin` リンク**:
   - 外部 Consumer (`/private/tmp/scratch_shizuku_consumer`) において `bazel build //:ext_sketch_archive` は exit 0 で成功し、11MB の単一 `.a` を出力。
   - トップレベル `//:ext_sketch` はホスト環境に `IDF_PATH` が未 export のため `build.sh` 内で安全に exit 1 で停止（アーカイブ先行生成済み）。

---

## 3. 再現・検証コマンド (Exact Commands)

```bash
# 1. 新規テストパッケージの全件アサーション実行 (Positive 2件 + Negative 7件)
bash tests/firmware_composition/test_fail_closed.sh

# 2. 複数スケッチを含む合成アーカイブのビルドとシンボル確認
bazel build //tests/firmware_composition:sample_fw_archive
/private/var/tmp/_bazel_ishigakiyua/eaa378ed3d2ea7b6b83ae02c66d7e36c/external/shizuku++_repo_rules+riscv32_esp_elf_gcc/bin/riscv32-esp-elf-nm bazel-bin/tests/firmware_composition/libsample_fw_archive.a | grep -E "(sample_sketch_obj.*main|shizuku_object_id_)"

# 3. 外部 scratch consumer 検証 (no .bazelrc)
cd /private/tmp/scratch_shizuku_consumer
bazel build //:ext_sketch_archive
bazel build //:ext_sketch  # IDF_PATH未設定で安全にexit 1

# 4. リモート rbuild baseline 検証
rbuild --worktree objfw job build_esp32c6
rbuild --worktree objfw job build_esp32c5
```
