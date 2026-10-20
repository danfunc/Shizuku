// ESP32-C6 専用の vector table: 256 バイト整列、4 バイトスロット 32 個 (IDF vectors_intc.S と同じ)。
// 各スロットは arch/riscv/rv32_trap.S の共通 entry へ飛ぶ。save/restore・CSR・return は arch 側。
// IRAM 配置 (.iram1) はこの chip 側ラッパーだけが決め、共通本体はその中へ textual include する。
.section .iram1,"ax",@progbits
.balign 256
.option push
.option norvc
.option norelax
.global shizuku_c6_vectors
shizuku_c6_vectors:
.rept 32
  j shizuku_rv32_trap_entry
.endr
.option pop
#include "arch/riscv/rv32_trap.S"
#if defined(SHIZUKU_EXP_FASTCALL) && SHIZUKU_EXP_FASTCALL
#include "chips/esp32_c6/c6_fastcall.S"
#endif
