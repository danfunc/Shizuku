# Handoff: Object Firmware Composition API (ESP32-C6 Slice)

- **Job ID**: `1f4f09e3-6bdb-4365-a483-a51a04906bb8`
- **Parent Job**: `ad6a55a4-cfe5-4ab5-bbde-85892fc248c3`
- **Followup / Caller Session**: `ad6a55a4-cfe5-4ab5-bbde-85892fc248c3`
- **Base Commit**: `10534ff74ad63bfe155fe55dca89635e9ce61434`
- **Branch**: `feat/object-firmware-composition`

---

## 1. 実装到達点 (Confirmed & Implemented)

1. **`defs.bzl` の再構成と厳格な境界契約**:
   - `ShizukuObjectInfo` provider: `object_name`, `entry_point`, `cc_info` のみを保持。`shizuku_object` 側の `parent`/`privileged` を完全削除。
   - `shizuku_object(name, srcs, hdrs, deps, ...)`: firmware/ID/parent 未定の純粋 compile unit。
   - `shizuku_firmware(name, target, objects, parents, privileged_names, ...)`:
     - Manifest ownership を firmware 側に完全移行。
     - オブジェクト上限を予約スロット（0: KERNEL_OBJECT, 1: ROOT_OBJECT）を引いた **62** 個に設定。
     - 親解決: デフォルトまたは `"root"` の親 ID は runtime の `ROOT_OBJECT = 1` にマッピング（legacy symbol `shizuku_object_id_root = 0` とは分離）。
     - 静的検証 (Fail-closed): 重複名、予約名 (`root`, `count`, `kernel`, `kernel_object`)、親の不在、自己親 (`self-parent`)、循環階層 (`cycle`)、未知ターゲット (`unknown target`) を機械的に拒否。
     - トップレベルバイナリ (`_shizuku_firmware_binary`): incoming transition を持たせ、ホスト環境 (macOS arm64 等) から `.bazelrc` なしでビルドした際の incompatibility skip を完全解消。
   - `shizuku_sketch_object` / `shizuku_sketch`:
     - `setup()` 1回呼出し + `loop()` 無限反復のエントリラッパー `<name>_main()` を生成。
     - 単独ファームウェアだけでなく provider 付き object として他の `shizuku_firmware` の `objects` へ渡せるよう分離。
2. **新 API テストパッケージの分離**:
   - `/Users/ishigakiyua/github/Shizuku-objfw/tests/firmware_composition/` を新設し、既存 `tests/composition_link/` の境界を保護。
   - `test_fail_closed.sh`: unique `/private/tmp` fixture を使用した検証スクリプトを用意。
3. **設計文書の更新**:
   - `docs/design/object_firmware_composition.typ` に Section 4（実装確定方針と境界契約）を追記し、PDF を再生成。

---

## 2. 未達事項・技術境界 (Unresolved & Boundaries)

1. **純粋 C ABI の yield 関数**:
   - 現行カーネルには C++ の `shizuku::services::yield()` のみ実在し、外部から C ABI で直接呼べる `shizuku_yield(void)` は存在しない。架空シンボルによるリンクエラーを防ぐため未実装とし、次期カーネル C ABI 拡張へ引き継ぐ。
2. **SDK build.sh の `idf.py set-target` キャッシュ挙動**:
   - 第 5 引数による常設 builddir のパス受け入れは実装済みだが、`idf.py set-target` が既存の CMake キャッシュをクリアする挙動があるため、設定済みの場合は `set-target` をスキップするガードが今後必要。
3. **C6 実機 `.bin` の最終リンク**:
   - ID 担当（job `05c743b4-bd45-4625-bd43-c6f418ae440b`）が `tools/gen_object_ids.py` および `tools/object_ids.c.in`、root `BUILD.bazel` の移行を進行中。root BUILD のビルド整合性が回復次第、最終リンクを実施。

---

## 3. 再現・検証コマンド (Exact Commands)

```bash
# 1. 新規テストパッケージのメタデータ生成確認
bazel build //tests/firmware_composition:sample_fw_composition_gen
bazel build //tests/firmware_composition:direct_sketch_fw_composition_gen

# 2. 外部 scratch consumer 検証 (no .bazelrc)
cd /private/tmp/scratch_shizuku_consumer
bazel build //:ext_sketch_archive
riscv32-esp-elf-nm bazel-bin/libext_sketch_archive.a | grep -E "(ext_sketch_main|shizuku_object_id)"
```
