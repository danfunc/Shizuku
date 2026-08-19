# 01. 現行リポジトリ調査結果 (= 「書き方のベース」の定義)

調査日: 2026-08-19。対象: リポジトリルート以下 (`latest_ver_from_flight_robocon/` を除く)。

## 1. 全体構成

```
CMakeLists.txt                     トップ。Shizuku 実行ファイル + Shizuku_internal_HEADERS (INTERFACE)
cmake/buildUtilities.cmake         preloadtoolchain()/postloadtoolchain() マクロ
cmake/pico_sdk/                    pico_sdk_import.cmake (SDK 公式コピー) / pico_sdk_setup.cmake
configs/                           config_template.hpp.in → build/…/shizuku/config.hpp を生成
internal_headers/shizuku/          カーネル本体のヘッダ (ISA 非依存を志向)
  concepts/                        concept 定義 (context / cpu_driver / memory_manager / register_sized)
  templates/                       kernel / cpu_manager / object / thread / table / result
  cpu_drivers/dummy.hpp            ダミー型
  kernel.hpp                       extern kernel_instance (config.hpp 経由で実体型が決まる)
  kernel_object.hpp                KernelObject クラス (骨のみ)
modules/                           差し替え可能モジュール群 (USE_SHIZUKU_MODULES で選択)
  pico_sdk_support/                pico-sdk 向けモジュール (main / cpu_driver / memory_manager)
    internal_headers/shizuku/      モジュール提供ヘッダ (cpu_drivers/rp2040, abis, memory_managers, app_entry)
source/                            カーネル実装 (.cpp)。kernel.cpp / cpu_manager/ / object_system/ / kernel_Object/
```

## 2. 踏襲すべき「書き方」の流儀 (ここが本作業の様式規範)

1. **template + concepts による差し替え** (仮想関数を使わない)。
   `shizuku::templates::kernel<CPU_MANAGER_T, MEMORY_MANAGER_T, OBJECT_T, SYSTEM_OBJECT_ENTRY>`
   のように ISA/ボード依存物を型パラメータで受け、`shizuku::concepts::*_requires` で縛る。
   PORT §5.4 の推奨と完全に一致する方向であり、これを拡張していく。
2. **configs/ による型の注入**。CMake キャッシュ変数 (`SHIZUKU_CPU_DRIVER` 等) を
   `config_template.hpp.in` に configure_file で流し込み、`shizuku/config.hpp` に
   `using KERNEL = templates::kernel<...>` 等の別名を確定させる。カーネル実装 (.cpp) は
   `shizuku::KERNEL` などの**確定済み別名に対する `template<>` 特殊化**として書く
   (例: `source/kernel.cpp` の `template <> void KERNEL::init()`)。
3. **modules/ の作法**。トップの `USE_SHIZUKU_MODULES` に列挙されたディレクトリだけが
   `add_subdirectory` される (pico_sdk_setup.cmake が `pico_sdk_support` を append)。
   モジュールは:
   - 自分のヘッダを `<module>/internal_headers/` に置き、`Shizuku_internal_HEADERS` にリンク
   - `config_need_headers` (CACHE INTERNAL) に自分のヘッダパスを **prepend** して
     config.hpp の先頭 `#include` 群に混ぜてもらう
   - `target_link_libraries(Shizuku PRIVATE <module>)` で本体へ繋ぐ
   ※ 順序が本質: トップ CMakeLists は `modules` → `configs` → `source` の順に
   add_subdirectory している。modules/CMakeLists.txt が configure のたびに
   `config_need_headers` を空にリセット → 各モジュールが積む → configs が読む。
4. **エラーは `shizuku::templates::result<T>`** ([[nodiscard]], code + union payload,
   `explicit operator bool`)。失敗時は `{result_code, "理由文字列"}`。panic は使わない方向
   (DESIGN I-9 とも一致)。svc ワイヤ上の型付きエラー (参照実装の `error_t<E>`) とは役割が
   違う点に注意 — result<T> は C++ 内 API 用、ワイヤは enum (0=成功) 用。
5. **命名**: namespace は小文字 (`shizuku::templates` / `shizuku::concepts` /
   `shizuku::cpu_drivers` / `shizuku::memory_managers` / `shizuku::abis`)。
   テンプレートパラメータと注入される型別名は大文字 (`CPU_MANAGER`, `THREAD`)。
   include guard は `SHIZUKU_*_HPP`。メンバは `m_` 接頭辞 (thread.hpp) または裸 (混在)。
6. **C++23 / CMake 3.26+**。`-fPIE -fPIC` をグローバル付与 (トップ CMakeLists)。

## 3. 各ファイルの現状 (実装の到達点)

| ファイル | 状態 |
|---|---|
| templates/kernel.hpp | 型パラメータと `object_table` (static_table<OBJECT*,128>)、`init()` 宣言のみ |
| templates/cpu_manager.hpp | `cpu_drivers[CORE_COUNT]`、init/execute_thread/entry の骨 |
| templates/object.hpp | obj_type enum (KERNEL/PRIVILEGED_LAND/UNPRIVILEGED_LAND)、各種 TABLE 型パラメータ |
| templates/thread.hpp | context ポインタ + id + status enum のみ |
| templates/table.hpp | static_table<T,N>: 範囲検査つき operator[] → result<T&> |
| templates/result.hpp | 上述。result<void> 特殊化あり |
| concepts/*.hpp | ほぼ空 (`true` を要求するだけ / 空ファイル)。**要実装** |
| kernel.hpp / kernel_object.hpp | extern 宣言と空クラス |
| source/kernel.cpp | KERNEL::init() の途中。`return;` 以降は到達しないスケッチコード |
| source/cpu_manager/init.cpp | cpu_drivers[i].init(i) を回すだけ |
| source/object_system/*, kernel_Object/* | 空実装 |
| modules/pico_sdk_support/cpu_driver.cpp | svc ハンドラ (番号印字 + 255=INIT_INVOKE で context 復帰)、pendsv 空 |
| modules/pico_sdk_support/main.cpp | 実験用 main (svc 往復のみ)。カーネルを起動していない |
| modules/pico_sdk_support/memory_manager.cpp | malloc/free ラッパ |
| abis/rp2040_abi.hpp | svc caller/callee コンバータ (inline asm) |

## 4. 発見済みの不具合・負債 (Phase 0 で潰す対象)

1. **CMake の大文字小文字不一致**: `source/CMakeLists.txt` は `Object_System` /
   `Kernel_Object` を add_subdirectory するが、実ディレクトリは `object_system` /
   `kernel_Object`。macOS (大文字小文字非区別 FS) では通るが Linux で即死。
   ディレクトリ名を小文字 snake_case に統一し、参照も合わせる
   (git 上も `Object_System` → `object_system` の rename が中途半端な状態)。
2. **include guard 衝突 (実害あり)**:
   - `modules/.../abis/rp2040_abi.hpp` と `modules/.../kernel_object_abi/rp2040_abi.hpp` が
     **同じガード `SHIZUKU_RP2040_ABI_HPP`**。両方 include すると後の方が丸ごと消える。
     しかも両者は同名クラス `shizuku::abis::rp2040` を**異なる定義**で持つ (ODR 違反)。
     main.cpp は現に両方 include している。→ kernel_object_abi 側は namespace ごと分離
     するか統合する。
   - `internal_headers/shizuku/dummy.hpp` と `internal_headers/shizuku/cpu_drivers/dummy.hpp`
     も同じ `SHIZUKU_DUMMY_HPP` で衝突 + 同名 struct の二重定義。
3. **inline asm のオペランド方向バグ** (`modules/pico_sdk_support/main.cpp`):
   `MSR PSP,%[entry_psp]` / `MSR CONTROL,...` の入力を **出力制約 `"=r"`** で渡している。
   未初期化レジスタが MSR に入る未定義動作。参照実装 (shizu.hpp) の入力制約版が正しい。
4. **SVC ハンドラの二重登録**: main.cpp と cpu_driver.cpp の両方が
   `exception_set_exclusive_handler(SVCALL_EXCEPTION, …)` を呼ぶ。pico-sdk の exclusive
   登録は二重登録で assert/panic する。svc ハンドラ実装も 2 ヶ所に重複 (main.cpp 側は削除)。
5. **kit 変数の typo**: `.vscode/cmake-kits.json` の `SHIZUKU_SYSTEM_OBJECT_ENTR` (Y 欠け)。
   ただし config_template.hpp.in はこの変数を使っておらず `app_entry` を直書きしている。
   どちらかに寄せる (テンプレート変数化を推奨)。
6. **命名のねじれ**: cpu driver 名が `rp2040` だが、kit は `PICO_BOARD=pico2` /
   `PICO_PLATFORM=rp2350`。RP2040 は ARMv6-M (LDREX 無し / PSPLIM 無し)、RP2350 は
   ARMv8-M で、DESIGN/PORT が前提にする機構 (PSPLIM, LDREX/STREX, MPU PMSAv8) は
   **RP2350 側**。層の名前は ISA (armv8m) とボード (rp2350/pico2) に分けて付け直す
   (03 の写像参照)。`SVCALL_EXCEPTION` 番号等も pico-sdk 定数なので実害はまだ無いが、
   名前が嘘をついている状態。
7. **source/kernel.cpp のスケッチ**: `return;` 以降のデッドコード、
   `memory_manager.init()` 失敗時に `panic("memory_manager_uninitialized")` (I-9 的には
   ブート時なので panic 可だが、メッセージと分類は整理)。object_table[0] 経由の
   カーネルオブジェクト生成コードは DESIGN の「カーネルはオブジェクトを知らない」(§7)
   と矛盾するので、**このスケッチは廃棄対象** (03 参照)。
8. **buildUtilities.cmake の空関数** `setting()` は `set()` を無引数で呼ぶだけの残骸。削除可。
9. **configs/CMakeLists.txt の `set(config_need_headers)`**: ローカル変数を消して
   キャッシュ値を透過させる意図なら動くが紛らわしい。コメントを付けるか
   `$CACHE{config_need_headers}` 参照に明示する。
10. **templates/object.hpp の obj_type** に `PRIVILEGED_LAND_OBJECT` /
    `UNPRIVILEGED_LAND_OBJECT` がある。DESIGN §11 は「特権オブジェクト」概念を
    3 軸に分解して**静的クラス化を禁止**した (§11.6, PORT §4)。この enum は
    そのままにしない (03 の決定 D6)。

## 5. ビルド方法

**最新の検証済みコマンドは [04_work_instructions.md](04_work_instructions.md) の
「Phase 0 完了記録」を参照** (Bazel が主、CMake は並存中)。
書き込み: トップ CMakeLists の `install` が `pico-tools flash <build>/Shizuku.uf2` を呼ぶ
(Bazel 側の書き込みターゲットは未整備 — `pico-tools flash bazel-bin/shizuku.uf2` を手で)。

## 6. git の現状

- branch: `0.1` (PR 先は `stable`)。未コミットの大規模 rename が進行中:
  `interface/` → `internal_headers/`、`configs/config_template.hpp` →
  `config_template.hpp.in`、モジュール内 internal_headers 化、
  `pendsv.S` / `syscall.S` の削除など。
- `latest_ver_from_flight_robocon/` は**参照資料であり untracked**。ビルド対象に含めない
  (git に入れるかはユーザー判断。少なくとも docs 2 本は価値が高い)。
