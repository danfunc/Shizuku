# ESP32-C6 compile/link-only 検証記録 (2026-10-06)

起動しても何も動かさない compile/link-only の記録。実機・実行時の動作は未検証。
Bazel の主線 (`//firmware:esp32c6`) は **未達** (下記 §1)。

## 1. Bazel 主線 (未達)
- `bazelisk --batch --output_user_root=/tmp/claude/bazel --host_jvm_args=-Djdk.http.auth.tunneling.disabledSchemes= build --config=esp32c6 //firmware:esp32c6`
  - exit 32。`Failed to fetch registry file https://bcr.bazel.build/modules/bazel_skylib/1.6.1/MODULE.bazel: Unable to tunnel through proxy. Proxy returns "HTTP/1.1 403 Forbidden"`。
  - サンドボックス違反は `deny network-outbound bcr.bazel.build:443 (user denied)`。環境 (権限) 起因で、コード起因ではない。
  - ログ: `/tmp/claude/bazel_main.log` (最後の実行分のみ)。
- サンドボックス外実行 (`dangerouslyDisableSandbox`) は `Run outside of the sandbox` で 2 回拒否された。迂回していない。
- `--output_user_root` を worktree 内 (`build/bazel`) にすると repo contents cache が workspace 内になりエラー。`/tmp/claude/bazel` を使う。
- Bazel 側のソース (`toolchains/rv32_esp/`、`//:shizuku_kernel`、`//firmware:esp32c6`、`.bazelrc` の `--config=esp32c6`) は **一度も解析されておらず unverified**。
- 再開: `bcr.bazel.build` (と GitHub のアーカイブ) の読取が実際にこのセッションで許可された状態で上のコマンドを再実行する。

## 2. 手動で生成した成果物 (Bazel 経由ではない)
- `.a`: `riscv32-esp-elf-g++` (esp-15.2.0_20251204) で 6 ソースを手動コンパイルし `ar rcs` (`/tmp/claude/rvprobe.sh`, `/tmp/claude/rvlib.sh`)。
  - `/tmp/claude/rv/libshizuku_kernel.a` SHA256 `1f19fceba7862a14137e85d59ab25f5e5a9a45b5c7b9568dcf87002c610b6745`
  - メンバー 6: c6_link_stubs.o / cpu_manager_init.o / kernel_dispatch.o / kernel_init.o / kernel_thread.o / memory_manager_freestanding.o
- IDF: ESP-IDF v6.1 + GCC 15.2.0 (esp-15.2.0_20251204) で `idf.py set-target esp32c6` / `build` が rc=0 (`/tmp/claude/idfbuild.sh`, ログ `/tmp/claude/idfbuild.log`)。
  - `.local/build/esp32c6/shizuku_esp32c6.bin` 160160 B SHA256 `adb249b462ff969abe82ada3fddf28f6822aed4ac4540cde66c04a00c73af88e`
  - `shizuku_esp32c6.elf` 3803848 B SHA256 `7e1ce06b9ff5371f0178ba436f7b812ef490dce22a41583f7b8c69ac76ee3f82`
- `firmware/esp32c6/build.sh` の単体実行 (`/tmp/claude/postest.sh`): exit 0、`/tmp/claude/shizuku_esp32c6.bin` 160160 B SHA256 `6216a8fb55cb0663a81648d1c581579905a1b670fabb576d99f422dc7a7e126e` (パス埋め込みで SHA が上と異なる)。Bazel の action からは未実行。
  - 異常系: `IDF_PATH` 不正と GCC 版不一致はいずれも理由を出して exit 1 (`/tmp/claude/negtest.sh`)。

## 3. テスト (直接実行。Bazel の test ではない)
- `firmware/esp32c6/test_image.sh` 21 assert (check 行 38-72) すべて PASS、`fails=0`、exit 0。
- 変異確認: `libmain.a` を渡すと A1-A5 の 5 件が FAIL、exit 1。
- 証明するのは「kernel の実コードが ELF にあり、`start_cpu0` が `ebreak` で、スケジューラ関連シンボルが ELF に無い」ことだけ。

## 4. 未実装・未検証
- `ports/esp32/c6/link_stubs.cpp` の 26 関数 (arch 13: 22-36 行、board 13: 44-56 行) はすべて即トラップ。カーネルは動かない。
- 実機での起動、PMP/trap、IDF の private ABI (`start_cpu0` 差し替え) の安定性は未検証。

## 5. `build.sh` の相対出力パスのバグ修正 (上層レビュー差戻し)
- バグ: Bazel の genrule は出力 `$@` を相対パスで渡す。旧 `build.sh` は `OUT=$2` / `ARCHIVE=$1` のまま、
  `mktemp -d "$OUT.idf..."` を元の cwd で作り、`cd "$SRC"` の後に `-B "$BUILD_DIR"` と `cp ... "$OUT"` を使っていたため、
  相対パスが別の場所を指した。絶対パスを渡す手動の `postest.sh` では露見しない。サンドボックス回避とは無関係の実バグ。
- 修正 (`firmware/esp32c6/build.sh:16-22`): `cd` の前に `ARCHIVE` / `OUT` / `SRC` を絶対化 (`abspath` = `os.path.realpath`。
  存在しない出力にも対応)。出力ディレクトリは `mkdir -p` (同 `:42`、`cd` は `:44`)。
- 旧挙動の mutant (`/tmp/claude/build_mut.sh`: `OUT=$2`, `ARCHIVE=$1`) を `/tmp/claude/wt2` から相対パスで実行 → exit 1、`.bin` なし
  (`set-target.log` が見つからず失敗。`/tmp/claude/mut_run.sh`)。バグの再現と、テストが捕まえることを確認。
- 検査: `firmware/esp32c6/test_wrapper.sh` を `/tmp/claude/wrap_test.sh` から実行 → exit 0、`fails=0`、ログ `/tmp/claude/wrap_test.log`。
  - positive (行 58-66、P5/P6 は §6): P1 相対 ARCHIVE + 未作成ディレクトリ配下の相対 OUT で exit 0 / P2 cwd 相対の位置に .bin / P3 先頭 0xE9 / P4 スクリプトディレクトリに迷い込まない。
    生成物 `/tmp/claude/wt/out/rel.bin` 160160 B SHA256 `75656b961003231f7ae40a7f399196d88fb2fa4d9b677c6476d3093fe52056d4`。
  - negative (行 69-77): N1 `IDF_PATH` 欠落 exit 1 / N2 `IDF_TOOLS_PATH` 欠落 exit 1 / N3 GCC 版不一致 exit 1 + メッセージ確認 / N4 アーカイブ欠落 exit 1。
    いずれも `.bin` を作らない (N1b-N4b)。
  - 注: 欠落 env の失敗は `set -u` による bash のエラーで、専用メッセージではない (終了コードと .bin 非生成のみを確認)。
- これは Bazel の action 経由ではない。Bazel の一括ビルドは §1 のとおり未解析・未成功のまま。

## 6. `sdkconfig` / `sdkconfig.old` がソースツリーに出る副作用の修正
- 問題: `idf.py set-target` / `build` がプロジェクトディレクトリ (`firmware/esp32c6/`) に `sdkconfig` / `sdkconfig.old` を生成し、
  Bazel action の入力 source tree を汚していた。
- IDF の確認: `.local/esp-idf/tools/cmake/project.cmake:101-102` が外部指定の `SDKCONFIG` (絶対パスに解決) を使う。
- 修正 (`firmware/esp32c6/build.sh:46-47`): `set-target` と `build` の両方に `-DSDKCONFIG="$BUILD_DIR/sdkconfig"` を渡す
  (`.old` も同じ場所に出る)。コメントは `:43`。
- 既存ファイルの保護: 作業開始時点でソース側に `sdkconfig` は無かった (自分の過去の実行が作ったものだけを削除済み)。
  テストは実行前後の存在/SHA を比較するので、既存ファイルがあっても削除・上書きしない。
- 追加 assert (`firmware/esp32c6/test_wrapper.sh`): `:63` P5 ソース側の `sdkconfig` / `sdkconfig.old` が作られず変更もされない
  (`snap` は `:50-54`、実行前状態は `:55`) / `:64` P6 `sdkconfig` が `out/rel.bin.idf.*/` 配下に生成されている。
- 実 IDF で `/tmp/claude/wrap_test.sh` を実行 → exit 0、`fails=0` (P1-P6、N1-N4 と各 N*b、N3c すべて PASS)、ログ `/tmp/claude/wrap_test.log`。
  実行後の `firmware/esp32c6/` は `CMakeLists.txt build.sh main sdkconfig.defaults test_image.sh test_wrapper.sh` のみ。
  生成物 `/tmp/claude/wt/out/rel.bin` 160160 B SHA256 `c0a410cbbb222d270cea51c95765b930aeccae0dd24bc2e6f83439e56a313c6f`
  (ビルドディレクトリのパスが埋め込まれるため、実行ごとに SHA が変わる)。
- Bazel の一括ビルドは §1 のとおり未解析・未成功のまま。これも Bazel action 経由の実行ではない。
