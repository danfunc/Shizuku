# Shizuku

[English](README.md) | **日本語**

Shizuku は C++ で書かれた、保護機能を意識したマイクロコントローラ向けの小型カーネルです。
Raspberry Pi RP2040 / RP2350 (Cortex-M0+ / Cortex-M33) および Espressif
ESP32-C5 / ESP32-C6 (RISC-V) 上で動作し、他の ISA ファミリへも拡張できるように設計されています。

> **現状: 実験段階。** カーネルおよびそのボードポートは活発に開発中です。
> インターフェースは予告なく変更されることがあり、後述の機能のうちいくつかは特定のボードでのみ検証されています。どの機能も、使う前に [現状](#現状) をご確認ください。

## 特徴

- **wait queue に代わる実行権の貸与。** スレッドは `GRANT` (ネスト可能、内側の予算は外側の grant の残量にクランプされます) によって自身の CPU 時間を別のスレッドに貸与し、引数なしの `YIELD` でそれを取り戻します。mutex は、専用の wait/wake queue ではなく、`GRANT(owner)` と CAS による再チェックを組み合わせて構築されています。
- **静的属性テーブルの背後にあるカーネルオブジェクト。** システムコールは API ごとの静的属性 (パーミッション、ISR 呼び出し可否、退避レベル、権限主体) に基づいてルーティングされるため、呼び出しパスはビルド時に決定されます。オブジェクトは実行時に再登録 (ホットスワップ) 可能です。
- **ハードウェアが備えるメモリ保護の活用。** MPU (Cortex-M) および PMP/PMS (RISC-V) の領域は保護ドライバによって管理されます。メモリは借用された `&` / `&mut` grant としてのみ非特権コードに貸与され、ハードウェアで表現できない grant は事前に拒絶されます。
- **移植性のための階層構造。** `arch/` (ISA) → `chips/` (ベンダ SDK なし) → `boards/` → `modules/` (ベンダ SDK グルー)。チップ非依存のオブジェクトは独立した [XNO](https://github.com/danfunc/XNO) オブジェクトコレクションに置かれ、Shizuku はカーネル、カーネルオブジェクト、周辺機器を保持します。
- **RTOS なしでのベンダスタックの動作。** ESP32-C5/C6 では、`modules/esp_idf_support` 内の FreeRTOS 互換 shim を介して ESP-IDF の Bluetooth コントローラと NimBLE が Shizuku 上で動作します (基盤に FreeRTOS カーネルは存在しません)。Pico W では、BTstack と CYW43 が `modules/pico_sdk_support` を介して動作します。

## 現状

| ボード | チップ | カーネル + セルフテスト | BLE | 備考 |
|---|---|---|---|---|
| Raspberry Pi Pico W | RP2040 | 動作; call ladder はパス。**flash stream ladder がハングします** (調査中)。 | BTstack: advertising、GATT、USB CDC 経由の承認付きペアリング | BLE OTA の転送とコミットは検証済み。リブート後に新しいイメージで起動することは未確認。 |
| Raspberry Pi Pico 2 / Pico 2 W | RP2350 | ビルド可能; 以前のセルフテスト実行はパス | — | |
| Seeed XIAO RP2040 | RP2040 | ビルド可能 | — | |
| ESP32-C5 | RISC-V | 動作 | NimBLE: advertising、connection、GATT read | 安定したリンクには外部アンテナが必要です。 |
| Seeed XIAO ESP32-C6 | RISC-V | 動作; セルフテスト 136 passed / 0 failed | NimBLE は起動しますが、**まだ電波は出ていません** (調査中) | BLE OTA は opt-in ビルド (`--define=esp_ota=on`) として進行中です。 |

## リポジトリ構成

| パス | 内容 |
|---|---|
| `arch/` | ISA レイヤ: `armv6m`, `armv8m`, `riscv` (trap entry、context switch、アトミックス、MPU/PMP) |
| `chips/` | ベンダ SDK を使用しないチップサポート: `rp2040`, `rp2350`, `esp32_c5`, `esp32_c6` |
| `boards/` | ボード定義 |
| `source/` | カーネル (`kernel/`)、カーネルオブジェクト、メモリおよび CPU マネージャ、セルフテスト、サンプルアプリ |
| `modules/pico_sdk_support/` | pico-sdk グルー: USB CDC、フラッシュサービス、CYW43 / BTstack、OTA |
| `modules/esp_idf_support/` | ESP-IDF グルー: FreeRTOS 互換 shim、BT コントローラ / NimBLE、OTA (opt-in) |
| `firmware/` | トップレベルファームウェアイメージ (`//firmware:shizuku_uf2` など) |
| `configs/`, `toolchains/` | ビルド設定およびツールチェーン (RV32 ESP ツールチェーンを含む) |
| `tools/` | ホストツール: BLE OTA 送信ツール、BLE スループット測定ツール、書き込みヘルパー |
| `docs/` | 設計ドキュメント (Typst / Markdown、主に日本語) |

## ビルド

ビルドシステムは [Bazel](https://bazel.build/) (Bzlmod) です。pico-sdk 2.2.0 は Bazel 経由で取得されます。ESP32 ターゲットでは追加で **ESP-IDF v6.1** が必要です (環境変数に `IDF_PATH` と `IDF_TOOLS_PATH` を設定)。

```sh
# Raspberry Pi Pico W (RP2040) -> bazel-bin/firmware/shizuku.uf2
bazel build --config=rp2040_pico_w //firmware:shizuku_uf2

# Raspberry Pi Pico 2 W (RP2350)
bazel build --config=rp2350_pico2_w //firmware:shizuku_uf2

# ESP32-C6 / ESP32-C5 (without and with BLE)
bazel build --config=esp32c6     //modules/esp_idf_support/firmware:esp32c6
bazel build --config=esp32c6_ble //modules/esp_idf_support/firmware:esp32c6_ble
bazel build --config=esp32c5_ble //modules/esp_idf_support/firmware:esp32c5_ble
```

その他の config: `rp2040_pico`, `rp2040_xiao`, `rp2350_pico2`, `esp32c5`, `esp32c6`。
macOS では、ホストにインストールされた picotool を使用するために `--config=macos` を追加してください。

`rbuild.toml` では、リモートの Linux ワーカー上でビルドし、ローカルマシンから書き込みを行うためのジョブが定義されています (例: `rbuild job build_rp2040_pico_w`)。

### 書き込み

```sh
# RP2040 / RP2350
picotool load -x -v -f bazel-bin/firmware/shizuku.uf2

# ESP32-C5 / C6 (application image at 0x10000, via the built-in USB-JTAG)
openocd -f board/esp32c6-builtin.cfg \
  -c "program_esp <image>.bin 0x10000 verify reset exit"
```

## ドキュメント

設計ドキュメントは [`docs/`](docs/) にあり、主に日本語で書かれています:

- [`docs/design/kernel_v3.typ`](docs/design/kernel_v3.typ) — V3 カーネル設計 (モジュール境界、GRANT/YIELD、ディスパッチャ)
- [`docs/design/kernel_v3_arch.typ`](docs/design/kernel_v3_arch.typ) — ISA ファミリのカバレッジ (Cortex-M、RISC-V、Xtensa、x64)
- [`docs/explainer/kernel_v3_explained.pdf`](docs/explainer/kernel_v3_explained.pdf) — 図解解説
- [`docs/README.md`](docs/README.md) — 移植およびリファクタリングに関するノートの索引

## ライセンス

[MIT](LICENSE) © 2025 YUGA
