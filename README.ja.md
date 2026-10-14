# Shizuku

[English](README.md) | **日本語**

Shizuku は、マイコン向けの小さなカーネルです。C++ で書かれ、メモリ保護を前提に設計しています。
Raspberry Pi RP2040 / RP2350 (Cortex-M0+ / Cortex-M33) と Espressif ESP32-C5 / ESP32-C6
(RISC-V) で動き、ほかの ISA ファミリーへ広げることを視野に入れています。

> **状態: 実験段階です。** カーネルと各ボードへの移植は開発中です。インターフェースは
> 予告なく変わります。下の機能の多くは特定のボードでしか確かめていません。
> 使う前に [現状](#現状) を見てください。

## 特徴

- **wait キューの代わりに実行権の貸し借り。** スレッドは `GRANT` で CPU 時間を
  ほかのスレッドへ貸し (入れ子にでき、内側の予算は外側の残りで頭打ちになります)、
  引数なしの `YIELD` で返してもらいます。mutex も専用の wait/wake キューを作らず、
  `GRANT(owner)` と CAS の再確認で組みます。
- **静的な属性表で経路が決まるカーネルオブジェクト。** システムコールは API ごとの
  静的属性 (権限、ISR から呼べるか、保存レベル、権限主体) で振り分けるので、
  呼出し経路はビルド時に決まります。オブジェクトは実行中に再登録 (ホットスワップ) できます。
- **ハードウェアがあればメモリ保護。** MPU (Cortex-M) と PMP/PMS (RISC-V) の
  リージョンを保護ドライバが管理します。非特権コードへのメモリは `&` / `&mut` の
  借用 (grant) としてだけ渡し、ハードウェアで表現できない grant は最初に拒否します。
- **移植しやすい層構成。** `arch/` (ISA) → `chips/` (ベンダー SDK なし) →
  `boards/` → `modules/` (ベンダー SDK との接続)。チップに依存しないオブジェクトは
  別リポジトリ [XNO](https://github.com/danfunc/XNO) に置き、Shizuku はカーネル、
  カーネルオブジェクト、ペリフェラルを持ちます。
- **ベンダーのスタックを RTOS なしで動かす。** ESP32-C5/C6 では、ESP-IDF の
  Bluetooth コントローラと NimBLE を `modules/esp_idf_support` 内の FreeRTOS 互換層で
  Shizuku 上に載せています (FreeRTOS カーネルは使いません)。Pico W では BTstack と
  CYW43 を `modules/pico_sdk_support` で動かします。

## 現状

| ボード | チップ | カーネル + selftest | BLE | 備考 |
|---|---|---|---|---|
| Raspberry Pi Pico W | RP2040 | 動作。call ladder は通過。**flash stream ladder で停止する** (調査中) | BTstack: 広告、GATT、USB CDC での承認つきペアリング | BLE OTA の転送と commit は確認済み。再起動後に新しいイメージで起動するかは未確認 |
| Raspberry Pi Pico 2 / Pico 2 W | RP2350 | ビルド可。以前の selftest は通過 | — | |
| Seeed XIAO RP2040 | RP2040 | ビルド可 | — | |
| ESP32-C5 | RISC-V | 動作 | NimBLE: 広告、接続、GATT 読出し | 安定した接続には外付けアンテナが必要 |
| Seeed XIAO ESP32-C6 | RISC-V | 動作。selftest 136 通過 / 0 失敗 | NimBLE は起動するが **電波がまだ出ない** (調査中) | BLE OTA を opt-in ビルド (`--define=esp_ota=on`) として作成中 |

## リポジトリ構成

| パス | 内容 |
|---|---|
| `arch/` | ISA 層: `armv6m`、`armv8m`、`riscv` (trap 入口、コンテキスト切替、atomic、MPU/PMP) |
| `chips/` | ベンダー SDK を使わないチップ対応: `rp2040`、`rp2350`、`esp32_c5`、`esp32_c6` |
| `boards/` | ボード定義 |
| `source/` | カーネル (`kernel/`)、カーネルオブジェクト、メモリ・CPU 管理、selftest、サンプルアプリ |
| `modules/pico_sdk_support/` | pico-sdk との接続: USB CDC、flash サービス、CYW43 / BTstack、OTA |
| `modules/esp_idf_support/` | ESP-IDF との接続: FreeRTOS 互換層、BT コントローラ / NimBLE、OTA (opt-in) |
| `firmware/` | 最上位のファームウェアイメージ (`//firmware:shizuku_uf2` など) |
| `configs/`、`toolchains/` | ビルド設定とツールチェーン (ESP 用 RV32 ツールチェーンを含む) |
| `tools/` | ホスト側ツール: BLE OTA 送信、BLE スループット計測、書込み補助 |
| `docs/` | 設計文書 (Typst / Markdown、主に日本語) |

## ビルド

ビルドは [Bazel](https://bazel.build/) (Bzlmod) です。pico-sdk 2.2.0 は Bazel が取得します。
ESP32 向けにはさらに **ESP-IDF v6.1** が必要です (環境変数 `IDF_PATH` と `IDF_TOOLS_PATH`)。

```sh
# Raspberry Pi Pico W (RP2040) -> bazel-bin/firmware/shizuku.uf2
bazel build --config=rp2040_pico_w //firmware:shizuku_uf2

# Raspberry Pi Pico 2 W (RP2350)
bazel build --config=rp2350_pico2_w //firmware:shizuku_uf2

# ESP32-C6 / ESP32-C5 (BLE なし / あり)
bazel build --config=esp32c6     //modules/esp_idf_support/firmware:esp32c6
bazel build --config=esp32c6_ble //modules/esp_idf_support/firmware:esp32c6_ble
bazel build --config=esp32c5_ble //modules/esp_idf_support/firmware:esp32c5_ble
```

ほかの config: `rp2040_pico`、`rp2040_xiao`、`rp2350_pico2`、`esp32c5`、`esp32c6`。
macOS でホストに入れた picotool を使うときは `--config=macos` を足します。

`rbuild.toml` には、リモートの Linux worker でビルドして手元で書き込む job を定義しています
(例: `rbuild job build_rp2040_pico_w`)。

### 書込み

```sh
# RP2040 / RP2350
picotool load -x -v -f bazel-bin/firmware/shizuku.uf2

# ESP32-C5 / C6 (アプリイメージを 0x10000 へ、内蔵 USB-JTAG 経由)
openocd -f board/esp32c6-builtin.cfg \
  -c "program_esp <image>.bin 0x10000 verify reset exit"
```

## ドキュメント

設計文書は [`docs/`](docs/) にあります (主に日本語)。

- [`docs/design/kernel_v3.typ`](docs/design/kernel_v3.typ) — V3 カーネル設計 (モジュール境界、GRANT/YIELD、dispatcher)
- [`docs/design/kernel_v3_arch.typ`](docs/design/kernel_v3_arch.typ) — ISA ファミリー別の整理 (Cortex-M、RISC-V、Xtensa、x64)
- [`docs/explainer/kernel_v3_explained.pdf`](docs/explainer/kernel_v3_explained.pdf) — 図解の解説
- [`docs/README.md`](docs/README.md) — 移植・リファクタリング作業ノートの索引

## ライセンス

[MIT](LICENSE) © 2025 YUGA
