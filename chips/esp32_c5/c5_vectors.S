// ESP32-C5 専用の vector: CLIC mode (mtvec mode=3)。IDF vectors_clic.S と同じ配置規則。
//  - shizuku_c5_vector_table: mtvec[31:6]<<6 が指す 64B 整列の単一エントリ。例外と non-vectored 割込みが来る。
//  - shizuku_c5_mtvt_table  : mtvt が指す 64B 整列の関数アドレス表 (48 個)。hardware vectored 割込み用。
//    すべての割込みを shv=0 にするので通常は引かれないが、取り違えた場合も共通 entry へ入れる。
// どちらも arch/riscv/rv32_trap.S の共通 entry へ飛ぶ。save/restore・CSR・return は arch 側。
// IRAM 配置 (.iram1) はこの chip 側ラッパーだけが決め、共通本体はその中へ textual include する。
.section .iram1,"ax",@progbits
.option push
.option norvc
.option norelax
.balign 64
.global shizuku_c5_vector_table
shizuku_c5_vector_table:
  j shizuku_rv32_trap_entry
.balign 64
.global shizuku_c5_mtvt_table
shizuku_c5_mtvt_table:
.rept 48
  .word shizuku_rv32_trap_entry
.endr
.option pop
#include "arch/riscv/rv32_trap.S"
