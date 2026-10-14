# Shizuku

**English** | [日本語](README.ja.md)

Shizuku is a small, protection-aware kernel for microcontrollers, written in C++.
It runs on Raspberry Pi RP2040 / RP2350 (Cortex-M0+ / Cortex-M33) and Espressif
ESP32-C5 / ESP32-C6 (RISC-V), and is designed to grow toward other ISA families.

> **Status: experimental.** The kernel and its board ports are under active
> development. Interfaces change without notice, and several features below are
> verified only on specific boards. See [Status](#status) before relying on anything.

## Highlights

- **Execution lending instead of wait queues.** A thread lends its CPU time to
  another with `GRANT` (nestable, with the inner budget clamped to what the outer
  grant has left) and gets it back with an argument-less `YIELD`. Mutexes are
  built from `GRANT(owner)` plus a CAS re-check rather than dedicated wait/wake queues.
- **Kernel objects behind a static attribute table.** System calls are routed by
  static per-API attributes (permission, ISR-callable, save level, authority), so
  the call path is decided at build time. Objects can be re-registered at run time
  (hot swap).
- **Memory protection where the hardware has it.** MPU (Cortex-M) and PMP/PMS
  (RISC-V) regions are managed by a protection driver. Memory is lent to
  unprivileged code only as borrowed `&` / `&mut` grants; grants that the hardware
  cannot express are rejected up front.
- **Layered for portability.** `arch/` (ISA) → `chips/` (no vendor SDK) →
  `boards/` → `modules/` (vendor SDK glue). Chip-independent objects live in the
  separate [XNO](https://github.com/danfunc/XNO) object collection; Shizuku keeps the
  kernel, kernel objects and peripherals.
- **Vendor stacks without their RTOS.** On ESP32-C5/C6, the ESP-IDF Bluetooth
  controller and NimBLE run on Shizuku through a FreeRTOS-compatible shim inside
  `modules/esp_idf_support` — no FreeRTOS kernel underneath. On Pico W, BTstack
  and CYW43 run through `modules/pico_sdk_support`.

## Status

| Board | Chip | Kernel + selftest | BLE | Notes |
|---|---|---|---|---|
| Raspberry Pi Pico W | RP2040 | Runs; call ladder passes. **The flash stream ladder hangs** (under investigation). | BTstack: advertising, GATT, pairing with approval over USB CDC | BLE OTA transfer and commit verified; booting into the new image after reboot not yet verified. |
| Raspberry Pi Pico 2 / Pico 2 W | RP2350 | Builds; earlier selftest runs passed | — | |
| Seeed XIAO RP2040 | RP2040 | Builds | — | |
| ESP32-C5 | RISC-V | Runs | NimBLE: advertising, connection, GATT read | Needs an external antenna for stable links. |
| Seeed XIAO ESP32-C6 | RISC-V | Runs; selftest 136 passed / 0 failed | NimBLE starts, but **nothing is on air yet** (under investigation) | BLE OTA is in progress as an opt-in build (`--define=esp_ota=on`). |

## Repository layout

| Path | Contents |
|---|---|
| `arch/` | ISA layer: `armv6m`, `armv8m`, `riscv` (trap entry, context switch, atomics, MPU/PMP) |
| `chips/` | Chip support without vendor SDKs: `rp2040`, `rp2350`, `esp32_c5`, `esp32_c6` |
| `boards/` | Board definitions |
| `source/` | Kernel (`kernel/`), kernel objects, memory and CPU managers, selftest, sample apps |
| `modules/pico_sdk_support/` | pico-sdk glue: USB CDC, flash service, CYW43 / BTstack, OTA |
| `modules/esp_idf_support/` | ESP-IDF glue: FreeRTOS-compatible shim, BT controller / NimBLE, OTA (opt-in) |
| `firmware/` | Top-level firmware image (`//firmware:shizuku_uf2`, …) |
| `configs/`, `toolchains/` | Build configuration and toolchains (including the RV32 ESP toolchain) |
| `tools/` | Host tools: BLE OTA sender, BLE throughput meter, flashing helpers |
| `docs/` | Design documents (Typst / Markdown, mostly Japanese) |

## Building

The build system is [Bazel](https://bazel.build/) (Bzlmod). pico-sdk 2.2.0 is
fetched through Bazel; ESP32 targets additionally need **ESP-IDF v6.1**
(`IDF_PATH` and `IDF_TOOLS_PATH` in the environment).

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

Other configs: `rp2040_pico`, `rp2040_xiao`, `rp2350_pico2`, `esp32c5`, `esp32c6`.
On macOS, add `--config=macos` to use a host-installed picotool.

`rbuild.toml` defines jobs for building on a remote Linux worker and flashing
from the local machine (e.g. `rbuild job build_rp2040_pico_w`).

### Flashing

```sh
# RP2040 / RP2350
picotool load -x -v -f bazel-bin/firmware/shizuku.uf2

# ESP32-C5 / C6 (application image at 0x10000, via the built-in USB-JTAG)
openocd -f board/esp32c6-builtin.cfg \
  -c "program_esp <image>.bin 0x10000 verify reset exit"
```

## Documentation

Design documents are in [`docs/`](docs/), mostly in Japanese:

- [`docs/design/kernel_v3.typ`](docs/design/kernel_v3.typ) — V3 kernel design (module boundaries, GRANT/YIELD, dispatcher)
- [`docs/design/kernel_v3_arch.typ`](docs/design/kernel_v3_arch.typ) — ISA family coverage (Cortex-M, RISC-V, Xtensa, x64)
- [`docs/explainer/kernel_v3_explained.pdf`](docs/explainer/kernel_v3_explained.pdf) — illustrated explainer
- [`docs/README.md`](docs/README.md) — index of porting and refactoring notes

## License

[MIT](LICENSE) © 2025 YUGA
