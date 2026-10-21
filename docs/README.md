# Shizuku リファクタリング / 移植対応 — 作業ドキュメント索引

作成: 2026-08-19 (Claude セッション)。**別のセッション/モデル (Opus 等) がこの作業を
続行するための引き継ぎ資料一式**。コードを触る前に必ずここから読むこと。

## 作業の定義 (ユーザー指示の原文要約)

> Shizuku のリファクタリングと移植対応を行う。
> **コード設計・書き方は現行リポジトリ (このリポジトリのルート以下) の組み方をベース**とし、
> **機能面・設計面は `docs/design/kernel_v3.typ` と `docs/design/kernel_v3_arch.typ` に書かれた V3 カーネル設計を正しいもの**とする。
> 旧 V2 カーネルの記録は `docs/record/kernel_v2_record.typ` にある。

つまり:
- **何を作るか** = `docs/design/kernel_v3.typ`
  (以下 **DESIGN**) が規範。ISA 展開と非依存核モデルは `docs/design/kernel_v3_arch.typ` (以下 **ARCH**) を参照
- **どう書くか** = 現行リポジトリの骨格 (template + concepts、configs/ による型注入、
  modules/ による差し替え、`result<T>`、CMake 構成) を踏襲する
- 旧参照実装 (削除済み) は「実機検証済みの機構の宝庫」だが、
  **設計としてはそのまま移植しない** (DESIGN §12 が明示する欠陥を含む)。
  機構 (フレーム幾何・CAS・優先度設計) は移植する。カーネルとオブジェクトの境界は
  D58 (2026-09-28) に従い、再登録によるホットスワップを要件として設計する

## 読み順

1. [design/kernel_v3.typ](design/kernel_v3.typ) — 規範となる V3 カーネル設計 (DESIGN)
2. [design/kernel_v3_arch.typ](design/kernel_v3_arch.typ) — ISA 展開と非依存核モデル (ARCH)
3. [record/kernel_v2_record.typ](record/kernel_v2_record.typ) — V2 の記録と、決定 D 番号の正本 (削除した 03 から移した現行決定を含む)
4. [record/legacy_docs_summary.typ](record/legacy_docs_summary.typ) — 削除した 01〜05 (V1/V2 期の調査・方針・指示書・引き継ぎ) の要約と、全文の見方

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
- 次の作業 = 引き継ぎ書を参照。Phase 2 の D1 適用計画は 2026-09-28 に廃止された。
  プリミティブは **REDISPATCH 廃止で 5 種に確定** (D3)
- 削除した 04 のフェーズ表と 05 の現在地は [record/legacy_docs_summary.typ](record/legacy_docs_summary.typ) に要約してある (2026-08-24 時点)

## セッション中のユーザー決定 (詳細は record/kernel_v2_record.typ)

- **ビルドシステムは Bazel へ移行** (D15)。pico-sdk 2.2.0 が公式 Bazel サポートを
  同梱していることを確認済み。CMake は bring-up 完了まで並存
- **言語はカーネルコア C++ 維持、Rust は XNO 側の将来オプション** (D16)
- **オブジェクト集合は別リポジトリ「XNO」(苦悩, X Not an Object) に分離** (D17)。
  kernel object は Shizuku に含めてよいが、Shizuku 本体は小さく保つ
- **D1 を廃止し、ハンドラ/kobj の再登録によるホットスワップと密結合を許容** (D58,
  2026-09-28)。保護付き/カーネル空間オブジェクトの 2 種別を設ける

- `07_rust_and_cyrius_review.md` — D16 (言語) の再点検、Rust を入れるならの順序、n4mlz/Cyrius との比較 (2026-10-02)。
