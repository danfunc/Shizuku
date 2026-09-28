# ESP32 port exploration

This directory holds isolated ESP32 migration experiments. It is not wired into
the repository build and does not claim a functional port.

## Local state at the time of the exploration

- `~/esp` and `~/.espressif` were absent.
- `idf.py`, `xtensa-esp32s3-elf-gcc`, and `riscv32-esp-elf-gcc` were not on `PATH`.
- Apple clang 21 was present, but its cross-target compile failed because its LLVM
  option parser rejected `-riscv-add-build-attributes`. The installed `llvm-mc`
  successfully assembled the RV32 probes instead.
- No probe was executed on a device.

## RV32 probe

`c6/rv32_isa_probe.S` is an assembler-only check for a few RV32IMAC CSR, atomic,
and trap mnemonics. It is not an interrupt entry or an ARCH implementation.
Compile from this directory with:

```sh
llvm-mc -triple=riscv32-unknown-elf -mattr=+m,+a,+c -filetype=obj \
  c6/rv32_isa_probe.S -o build/rv32_isa_probe.o
llvm-mc -triple=riscv32-unknown-elf -mattr=+m,+a,+c -filetype=obj \
  c6/pmp_arch_probe.S -o build/pmp_arch_probe.o
```

The output belongs under `build/`, which is ignored. An ESP-IDF compile must be
done after installing/exporting the matching ESP-IDF toolchain; no package was
downloaded as part of this exploration.

`tools/install_esp_idf.sh` pins the current stable ESP-IDF bugfix release and is
intentionally not run in this workspace. Validate it with `bash -n` and run it
from an Apple Silicon macOS terminal when installation is wanted.
