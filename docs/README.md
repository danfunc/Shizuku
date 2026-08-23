# Shizuku リファクタリング / 移植対応 — 作業ドキュメント索引

作成: 2026-08-19 (Claude セッション)。**別のセッション/モデル (Opus 等) がこの作業を
続行するための引き継ぎ資料一式**。コードを触る前に必ずここから読むこと。

## 作業の定義 (ユーザー指示の原文要約)

> Shizuku のリファクタリングと移植対応を行う。
> **コード設計・書き方は現行リポジトリ (このリポジトリのルート以下) の組み方をベース**とし、
> **機能面・設計面は `latest_ver_from_flight_robocon/` に保存されている資料を正しいもの**とする。
> latest 以下には docs の形で設計方針が記載され、コード中にもコメントがあるので適宜参照する。

つまり:
- **何を作るか** = `latest_ver_from_flight_robocon/flight_robocon_telemetory_sender/docs/SHIZUKU_DESIGN.md`
  (以下 **DESIGN**) が規範。移し方は同 `SHIZUKU_REDESIGN_PORTABILITY.md` (以下 **PORT**) が規範
- **どう書くか** = 現行リポジトリの骨格 (template + concepts、configs/ による型注入、
  modules/ による差し替え、`result<T>`、CMake 構成) を踏襲する
- 参照実装 (`latest_.../kernel.cpp` ほか) は「実機検証済みの機構の宝庫」だが、
  **設計としてはそのまま移植しない** (DESIGN §12 が明示する欠陥を含む)。
  機構 (フレーム幾何・CAS・優先度設計) は移植し、構造 (カーネルがオブジェクトを知る事) は移植しない

## 読み順

1. [01_current_repo_survey.md](01_current_repo_survey.md) — 現行リポジトリの調査結果 (構成・流儀・**発見済みの不具合一覧**)
2. [02_reference_survey.md](02_reference_survey.md) — 参照実装 (latest) の調査結果 (何がどこにあり、何が検証済みか)
3. [03_porting_policy.md](03_porting_policy.md) — 方針: 設計文書の層構造を現リポジトリの流儀へどう写像するか、決定事項と未決事項
4. [04_work_instructions.md](04_work_instructions.md) — **指示書**: フェーズ別の実装手順・受け入れ条件・地雷一覧

DESIGN / PORT の 2 文書は長いが**全文読むこと** (特に DESIGN §7, §8, §11, §12, §17 と
PORT §3, §4.5, §4.6, §5)。本 docs はそれらの要約 + 現リポジトリへの適用であり、代替ではない。

## 現在地 (2026-08-19 時点)

- 現行リポジトリはごく初期の骨格段階 (svc 1 発を通す実験レベル)。git 上は
  `interface/` → `internal_headers/` への改名等が未コミットで進行中 (branch `0.1`)
- **Phase 0 (リポジトリ衛生 + Bazel bring-up) と Phase 1 (arch 抽象 + ARMv8-M
  バックエンド) を完了・実機確認済み** (2026-08-19: pico2 で success の周期出力を確認)
- ビルドは CMake / Bazel の両方が通る (コマンドは 04 §Phase 0 完了記録。
  CMake は `-DSHIZUKU_ARCH=armv8m -DSHIZUKU_BOARD=rp2350_pico2` に変数が変わった)。
  変更はすべて**未コミット** (コミットはユーザー判断)
- 次の作業 = Phase 2 (カーネルテンプレート + プリミティブ、D1 のオブジェクト概念外し)。
  プリミティブは **REDISPATCH 廃止で 5 種に確定** (03 §3.6、2026-08-19 ユーザー決定)

## セッション中のユーザー決定 (2026-08-19。詳細は 03)

- **ビルドシステムは Bazel へ移行** (D15)。pico-sdk 2.2.0 が公式 Bazel サポートを
  同梱していることを確認済み。CMake は bring-up 完了まで並存
- **言語はカーネルコア C++ 維持、Rust は XNO 側の将来オプション** (D16)
- **オブジェクト集合は別リポジトリ「XNO」(苦悩, X Not an Object) に分離** (D17)。
  kernel object は Shizuku に含めてよいが、Shizuku 本体は小さく保つ

- `05_handoff.md` — **次に触る人が最初に読む 1 枚**。今どこまで来ていて、何が壊れていて、どこを踏むと痛いか。推奨プロンプト付き。
