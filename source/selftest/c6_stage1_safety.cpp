// ===========================================================================
//  ESP32-C6 段1: U-mode + PMP 境界・trusted U stack・アプリ⇄ドライバ窓分離テスト
// ===========================================================================
//  ★段1 不変条件の検証:
//    1. U-mode thread が ecall で M-mode に入り、信頼 stack 上でのみディスパッチされ、
//       mret で U-mode に復帰する最小の安全な経路。
//    2. PMP allow-list により、実行対象 object code/data/U stack だけが許可され、
//       kernel/trusted stack/復帰情報および呼び出し元 (caller) スタックは U-mode から隔離され、
//       アクセスすると PMP fault になること。
//    3. アプリ⇄ドライバ窓分離:
//       - ドライバオブジェクト (role=Driver) のみ MMIO 窓 (0x60000000..0x60001000) を持ち、
//         MMIO へのアクセスが成功すること。
//       - アプリオブジェクト (role=App) は MMIO 窓を持たず、MMIO アクセスで PMP Load Fault が発生すること。
//       - アプリとドライバは互いの private data pool へのアクセスで PMP Load Fault が発生すること。
//    4. U-mode return trampoline が U 所有 code 範囲内に存在し、明示的 ecall 経由で
//       安全に M-mode に復帰すること (kernel 広域 RX 禁止)。
//    5. 0x802 U-mode read の成否を曖昧にせず、成功 / illegal-instruction を独立して診断・記録。
//    6. 非特権からの不正な特権操作・未認可呼出しが拒否されること。
//    7. U object identity (owner_id) に束縛され、未登録/別 ID は fail-closed で拒否されること。

#include <cstdint>
#include <cstddef>
#include "shizuku/kernel.hpp"
#include "shizuku/object_api.hpp"
#include "shizuku/object_ids.hpp"
#include "shizuku/selftest.hpp"
#include "chips/esp32_c6/selftest_record.h"
#include "chips/esp32_c6/pmp_c6.hpp"

using c6_stage1_object_bounds = shizuku::chips::esp32_c6::c6_stage1_object_bounds;
using c6_stage1_role = shizuku::chips::esp32_c6::c6_stage1_role;

extern "C" {
bool shizuku_c6_is_authorized_u_object(uint32_t owner_id, uint32_t current_obj);
uintptr_t shizuku_c6_get_m_handler_stack(uintptr_t current_sp, uintptr_t frame_sp);
bool shizuku_c6_is_dispatch_stack_address(uintptr_t addr);
extern volatile uint32_t shizuku_c6_trap_diag[5];
extern volatile uint32_t shizuku_c6_u_mode_diag[16];
extern volatile uint32_t shizuku_c6_u_ecall_diag[3];
extern volatile uintptr_t g_c6_probe_0x802_pc;
extern volatile uintptr_t g_c6_probe_pmp_fault_pc;
extern volatile uintptr_t g_c6_probe_caller_stack_fault_pc;
extern volatile uintptr_t g_c6_probe_combo_0x802_pc;
extern volatile uintptr_t g_c6_probe_combo_pmp_pc;
extern volatile uintptr_t g_c6_probe_ustore_kstack_pc;
extern volatile uintptr_t g_c6_probe_ustore_caller_pc;
extern volatile uintptr_t g_c6_probe_ustore_code_pc;
extern volatile uintptr_t g_c6_probe_ufetch_target;
extern volatile uintptr_t g_c6_probe_upmpcfg_pc;
extern volatile uintptr_t g_c6_probe_mstore_iram_pc;
extern volatile uintptr_t g_c6_probe_mfetch_target;
extern volatile uintptr_t g_c6_probe_app_mmio_fault_pc;
extern volatile uintptr_t g_c6_probe_app_driver_data_fault_pc;
extern volatile uintptr_t g_c6_probe_driver_app_data_fault_pc;
extern volatile uint32_t shizuku_c6_m_pma_diag[2];
extern c6_stage1_object_bounds g_c6_stage1_u_bounds;
extern c6_stage1_object_bounds g_c6_stage1_driver_bounds;
extern unsigned char shizuku_c6_kernel_stack[4096];
extern unsigned char shizuku_c6_dispatch_stack[4096];

// アプリ用データ領域・スタック (16B 整列)
alignas(16) uint8_t u_mode_stage1_data_pool[256];
alignas(16) uint8_t u_mode_stage1_stack[1024];

// ドライバ用データ領域・スタック (16B 整列)
alignas(16) uint8_t u_mode_driver_data_pool[256];
alignas(16) uint8_t u_mode_driver_stack[1024];

// プローブ命令の exact PC ラベル
extern const char c6_stage1_u_code_start[];
extern const char c6_stage1_u_code_end[];
extern const char c6_stage1_u_return_trampoline[];
extern const char c6_stage1_probe_0x802_insn_addr[];
extern const char c6_stage1_probe_pmp_fault_insn_addr[];
extern const char c6_stage1_probe_caller_stack_fault_insn_addr[];
extern const char c6_stage1_probe_ecall_insn_addr[];
extern const char c6_stage1_probe_ecall_addr[];
extern const char c6_stage1_probe_combo_0x802_insn_addr[];
extern const char c6_stage1_probe_combo_pmp_fault_insn_addr[];
extern const char c6_stage1_probe_ustore_kstack_insn_addr[];
extern const char c6_stage1_probe_ustore_caller_insn_addr[];
extern const char c6_stage1_probe_ustore_code_insn_addr[];
extern const char c6_stage1_probe_ufetch_insn_addr[];
extern const char c6_stage1_probe_upmpcfg_insn_addr[];
extern const char c6_stage1_probe_app_mmio_fault_insn_addr[];
extern const char c6_stage1_probe_app_driver_data_fault_insn_addr[];
extern const char c6_stage1_probe_driver_mmio_insn_addr[];
extern const char c6_stage1_probe_driver_app_data_fault_insn_addr[];
extern const char c6_stage1_probe_mstore_iram_insn_addr[];
extern const char c6_stage1_probe_mfetch_insn_addr[];
uintptr_t u_mode_probe_worker(uintptr_t cmd, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3);
uintptr_t u_mode_driver_worker(uintptr_t cmd, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3);
uintptr_t shizuku_c6_m_store_probe(uintptr_t target, uint32_t value);
uintptr_t shizuku_c6_m_fetch_probe(uintptr_t target);
}

#if defined(__riscv)
asm(
    ".pushsection .text.c6_stage1_u, \"ax\", @progbits\n"
    ".balign 16\n"
    ".option push\n"
    ".option norvc\n"
    ".global c6_stage1_u_code_start\n"
    "c6_stage1_u_code_start:\n"
    "\n"
    ".global c6_stage1_u_return_trampoline\n"
    ".type c6_stage1_u_return_trampoline, @function\n"
    "c6_stage1_u_return_trampoline:\n"
    "  mv a2, a0\n"
    "  li a0, 2\n"
    "  li a1, 1\n"
    "  li a3, 0\n"
    "  li a4, 0xA55A\n"
    "  ecall\n"
    "1: j 1b\n"
    ".size c6_stage1_u_return_trampoline, .-c6_stage1_u_return_trampoline\n"
    "\n"
    ".balign 4\n"
    ".global u_mode_probe_worker\n"
    ".type u_mode_probe_worker, @function\n"
    "u_mode_probe_worker:\n"
    "  li t0, 1\n"
    "  beq a0, t0, .Lprobe_0x802\n"
    "  li t0, 2\n"
    "  beq a0, t0, .Lprobe_kstack\n"
    "  li t0, 3\n"
    "  beq a0, t0, .Lprobe_ecall\n"
    "  li t0, 4\n"
    "  beq a0, t0, .Lprobe_unpriv\n"
    "  li t0, 5\n"
    "  beq a0, t0, .Lprobe_caller_stack\n"
    "  li t0, 6\n"
    "  beq a0, t0, .Lprobe_multi_trap_combo\n"
    "  li t0, 7\n"
    "  beq a0, t0, .Lprobe_ustore_kstack\n"
    "  li t0, 8\n"
    "  beq a0, t0, .Lprobe_ustore_caller\n"
    "  li t0, 9\n"
    "  beq a0, t0, .Lprobe_ustore_code\n"
    "  li t0, 10\n"
    "  beq a0, t0, .Lprobe_ufetch\n"
    "  li t0, 11\n"
    "  beq a0, t0, .Lprobe_upmpcfg\n"
    "  li t0, 12\n"
    "  beq a0, t0, .Lprobe_app_mmio\n"
    "  li t0, 13\n"
    "  beq a0, t0, .Lprobe_app_driver_data\n"
    "  li t0, 14\n"
    "  beq a0, t0, .Lprobe_u_to_u_call_1\n"
    "  li t0, 15\n"
    "  beq a0, t0, .Lprobe_u_to_u_call_2\n"
    "  li t0, 16\n"
    "  beq a0, t0, .Lprobe_u_to_u_call_fault\n"
    "  li a0, 0\n"
    "  ret\n"
    "\n"
    ".Lprobe_0x802:\n"
    ".global c6_stage1_probe_0x802_insn_addr\n"
    "c6_stage1_probe_0x802_insn_addr:\n"
    "  csrr a0, 0x802\n"
    "  ret\n"
    "\n"
    ".Lprobe_kstack:\n"
    "  la t1, u_mode_stage1_data_pool\n"
    "  lw a1, 0(t1)\n"
    ".global c6_stage1_probe_pmp_fault_insn_addr\n"
    "c6_stage1_probe_pmp_fault_insn_addr:\n"
    "  lw a0, 0(a1)\n"
    "  ret\n"
    "\n"
    ".Lprobe_ecall:\n"
    ".global c6_stage1_probe_ecall_insn_addr\n"
    "c6_stage1_probe_ecall_insn_addr:\n"
    "  li a0, 5\n"
    "  li a1, 0\n"
    "  li a2, 0\n"
    "  li a3, 0\n"
    "  li a4, 0\n"
    ".global c6_stage1_probe_ecall_addr\n"
    "c6_stage1_probe_ecall_addr:\n"
    "  ecall\n"
    "  bnez a0, 2f\n"
    "  mv a0, a1\n"
    "  ret\n"
    "2:\n"
    "  li t0, 0xDEAD0000\n"
    "  or a0, a0, t0\n"
    "  ret\n"
    "\n"
    ".Lprobe_unpriv:\n"
    "  li a0, 0\n"
    "  li a1, 0\n"
    "  li a2, 0\n"
    "  li a3, 0\n"
    "  li a4, 0\n"
    "  ecall\n"
    "  ret\n"
    "\n"
    ".Lprobe_caller_stack:\n"
    "  la t1, u_mode_stage1_data_pool\n"
    "  lw a1, 4(t1)\n"
    ".global c6_stage1_probe_caller_stack_fault_insn_addr\n"
    "c6_stage1_probe_caller_stack_fault_insn_addr:\n"
    "  lw a0, 0(a1)\n"
    "  ret\n"
    "\n"
    ".Lprobe_ustore_kstack:\n"
    "  la t1, u_mode_stage1_data_pool\n"
    "  lw a1, 0(t1)\n"
    "  li t0, 0x0BADF00D\n"
    ".global c6_stage1_probe_ustore_kstack_insn_addr\n"
    "c6_stage1_probe_ustore_kstack_insn_addr:\n"
    "  sw t0, 0(a1)\n"
    "  ret\n"
    "\n"
    ".Lprobe_ustore_caller:\n"
    "  la t1, u_mode_stage1_data_pool\n"
    "  lw a1, 4(t1)\n"
    "  li t0, 0x0BADF00D\n"
    ".global c6_stage1_probe_ustore_caller_insn_addr\n"
    "c6_stage1_probe_ustore_caller_insn_addr:\n"
    "  sw t0, 0(a1)\n"
    "  ret\n"
    "\n"
    ".Lprobe_ustore_code:\n"
    "  la t1, u_mode_probe_worker\n"
    ".global c6_stage1_probe_ustore_code_insn_addr\n"
    "c6_stage1_probe_ustore_code_insn_addr:\n"
    "  sw zero, 0(t1)\n"
    "  ret\n"
    "\n"
    ".Lprobe_ufetch:\n"
    "  mv t2, ra\n"
    "  la t1, u_mode_stage1_stack\n"
    ".global c6_stage1_probe_ufetch_insn_addr\n"
    "c6_stage1_probe_ufetch_insn_addr:\n"
    "  jalr ra, 0(t1)\n"
    "  mv ra, t2\n"
    "  ret\n"
    "\n"
    ".Lprobe_upmpcfg:\n"
    ".global c6_stage1_probe_upmpcfg_insn_addr\n"
    "c6_stage1_probe_upmpcfg_insn_addr:\n"
    "  csrr a0, 0x3A0\n"
    "  ret\n"
    "\n"
    ".Lprobe_app_mmio:\n"
    "  la t1, u_mode_stage1_data_pool\n"
    "  lw a1, 8(t1)\n"
    ".global c6_stage1_probe_app_mmio_fault_insn_addr\n"
    "c6_stage1_probe_app_mmio_fault_insn_addr:\n"
    "  lw a0, 0(a1)\n"
    "  ret\n"
    "\n"
    ".Lprobe_app_driver_data:\n"
    "  la t1, u_mode_stage1_data_pool\n"
    "  lw a1, 12(t1)\n"
    ".global c6_stage1_probe_app_driver_data_fault_insn_addr\n"
    "c6_stage1_probe_app_driver_data_fault_insn_addr:\n"
    "  lw a0, 0(a1)\n"
    "  ret\n"
    "\n"
    ".Lprobe_multi_trap_combo:\n"
    ".global c6_stage1_probe_combo_0x802_insn_addr\n"
    "c6_stage1_probe_combo_0x802_insn_addr:\n"
    "  csrr t0, 0x802\n"
    "  la t1, u_mode_stage1_data_pool\n"
    "  lw a1, 0(t1)\n"
    ".global c6_stage1_probe_combo_pmp_fault_insn_addr\n"
    "c6_stage1_probe_combo_pmp_fault_insn_addr:\n"
    "  lw t0, 0(a1)\n"
    "  li a0, 5\n"
    "  li a1, 0\n"
    "  li a2, 0\n"
    "  li a3, 0\n"
    "  li a4, 0\n"
    "  ecall\n"
    "  bnez a0, 3f\n"
    "  li t0, 0x5AFE0000\n"
    "  or a0, a1, t0\n"
    "  ret\n"
    "3:\n"
    "  li t0, 0xDEAD0000\n"
    "  or a0, a0, t0\n"
    "  ret\n"
    "\n"
    ".Lprobe_u_to_u_call_1:\n"
    "  addi sp, sp, -32\n"
    "  sw ra, 28(sp)\n"
    "  sw s0, 24(sp)\n"
    "  sw s1, 20(sp)\n"
    "  sw s2, 16(sp)\n"
    "  la s0, u_mode_stage1_data_pool\n"
    "  lw s1, 20(s0)\n"
    "  lw s2, 24(s0)\n"
    "\n"
    "  li a0, 4\n"
    "  mv a1, s1\n"
    "  mv a2, s2\n"
    "  li a3, 3\n"
    "  li a4, 0\n"
    "  ecall\n"
    "  bnez a0, .Lu2u_fail_1\n"
    "  mv a0, a1\n"
    "\n"
    "  lw ra, 28(sp)\n"
    "  lw s0, 24(sp)\n"
    "  lw s1, 20(sp)\n"
    "  lw s2, 16(sp)\n"
    "  addi sp, sp, 32\n"
    "  ret\n"
    "\n"
    ".Lprobe_u_to_u_call_2:\n"
    "  addi sp, sp, -32\n"
    "  sw ra, 28(sp)\n"
    "  sw s0, 24(sp)\n"
    "  sw s1, 20(sp)\n"
    "  sw s2, 16(sp)\n"
    "  la s0, u_mode_stage1_data_pool\n"
    "  lw s1, 20(s0)\n"
    "  lw s2, 24(s0)\n"
    "\n"
    "  li a0, 4\n"
    "  mv a1, s1\n"
    "  mv a2, s2\n"
    "  li a3, 3\n"
    "  li a4, 0\n"
    "  ecall\n"
    "  bnez a0, .Lu2u_fail_2\n"
    "  mv a0, a1\n"
    "\n"
    "  lw ra, 28(sp)\n"
    "  lw s0, 24(sp)\n"
    "  lw s1, 20(sp)\n"
    "  lw s2, 16(sp)\n"
    "  addi sp, sp, 32\n"
    "  ret\n"
    "\n"
    ".Lprobe_u_to_u_call_fault:\n"
    "  addi sp, sp, -32\n"
    "  sw ra, 28(sp)\n"
    "  sw s0, 24(sp)\n"
    "  sw s1, 20(sp)\n"
    "  sw s2, 16(sp)\n"
    "  la s0, u_mode_stage1_data_pool\n"
    "  li t0, 0xA110CAFE\n"
    "  sw t0, 16(s0)\n"
    "  lw s1, 20(s0)\n"
    "  lw s2, 24(s0)\n"
    "\n"
    "  li a0, 4\n"
    "  mv a1, s1\n"
    "  mv a2, s2\n"
    "  li a3, 2\n"
    "  li a4, 0\n"
    "  ecall\n"
    "  li t0, 0xE012\n"
    "  bne a1, t0, .Lu2u_fail_3\n"
    "\n"
    "  lw t0, 16(s0)\n"
    "  li t1, 0xA110CAFE\n"
    "  bne t0, t1, .Lu2u_fail_4\n"
    "  li t2, 0xBEEFCAFE\n"
    "  sw t2, 16(s0)\n"
    "  lw t3, 16(s0)\n"
    "  bne t3, t2, .Lu2u_fail_5\n"
    "\n"
    "  lw ra, 28(sp)\n"
    "  lw s0, 24(sp)\n"
    "  lw s1, 20(sp)\n"
    "  lw s2, 16(sp)\n"
    "  addi sp, sp, 32\n"
    "  li a0, 0xE012\n"
    "  ret\n"
    "\n"
    ".Lu2u_fail_1:\n"
    "  lw ra, 28(sp)\n"
    "  lw s0, 24(sp)\n"
    "  lw s1, 20(sp)\n"
    "  lw s2, 16(sp)\n"
    "  addi sp, sp, 32\n"
    "  li a0, 0xDEAD0001\n"
    "  ret\n"
    "\n"
    ".Lu2u_fail_2:\n"
    "  lw ra, 28(sp)\n"
    "  lw s0, 24(sp)\n"
    "  lw s1, 20(sp)\n"
    "  lw s2, 16(sp)\n"
    "  addi sp, sp, 32\n"
    "  li a0, 0xDEAD0002\n"
    "  ret\n"
    "\n"
    ".Lu2u_fail_3:\n"
    "  lw ra, 28(sp)\n"
    "  lw s0, 24(sp)\n"
    "  lw s1, 20(sp)\n"
    "  lw s2, 16(sp)\n"
    "  addi sp, sp, 32\n"
    "  li a0, 0xDEAD0003\n"
    "  ret\n"
    "\n"
    ".Lu2u_fail_4:\n"
    "  lw ra, 28(sp)\n"
    "  lw s0, 24(sp)\n"
    "  lw s1, 20(sp)\n"
    "  lw s2, 16(sp)\n"
    "  addi sp, sp, 32\n"
    "  li a0, 0xDEAD0004\n"
    "  ret\n"
    "\n"
    ".Lu2u_fail_5:\n"
    "  lw ra, 28(sp)\n"
    "  lw s0, 24(sp)\n"
    "  lw s1, 20(sp)\n"
    "  lw s2, 16(sp)\n"
    "  addi sp, sp, 32\n"
    "  li a0, 0xDEAD0005\n"
    "  ret\n"
    "\n"
    ".size u_mode_probe_worker, .-u_mode_probe_worker\n"
    "\n"
    ".balign 4\n"
    ".global u_mode_driver_worker\n"
    ".type u_mode_driver_worker, @function\n"
    "u_mode_driver_worker:\n"
    "  li t0, 1\n"
    "  beq a0, t0, .Ldriver_probe_mmio\n"
    "  li t0, 2\n"
    "  beq a0, t0, .Ldriver_probe_app_data\n"
    "  li t0, 3\n"
    "  beq a0, t0, .Ldriver_probe_echo\n"
    "  li a0, 0\n"
    "  ret\n"
    "\n"
    ".Ldriver_probe_echo:\n"
    "  li a0, 0x42\n"
    "  ret\n"
    "\n"
    ".Ldriver_probe_mmio:\n"
    "  la t1, u_mode_driver_data_pool\n"
    "  lw a1, 0(t1)\n"
    ".global c6_stage1_probe_driver_mmio_insn_addr\n"
    "c6_stage1_probe_driver_mmio_insn_addr:\n"
    "  lw a0, 0(a1)\n"
    "  ret\n"
    "\n"
    ".Ldriver_probe_app_data:\n"
    "  la t1, u_mode_driver_data_pool\n"
    "  lw a1, 4(t1)\n"
    ".global c6_stage1_probe_driver_app_data_fault_insn_addr\n"
    "c6_stage1_probe_driver_app_data_fault_insn_addr:\n"
    "  lw a0, 0(a1)\n"
    "  ret\n"
    ".size u_mode_driver_worker, .-u_mode_driver_worker\n"
    "\n"
    ".balign 16\n"
    ".global c6_stage1_u_code_end\n"
    "c6_stage1_u_code_end:\n"
    ".option pop\n"
    ".popsection\n"
);
#else
// 非 RISC-V ホストテスト用スタブシンボル
const char c6_stage1_u_code_start[] = "";
const char c6_stage1_u_code_end[] = "";
const char c6_stage1_u_return_trampoline[] = "";
const char c6_stage1_probe_0x802_insn_addr[] = "";
const char c6_stage1_probe_pmp_fault_insn_addr[] = "";
const char c6_stage1_probe_caller_stack_fault_insn_addr[] = "";
const char c6_stage1_probe_ecall_insn_addr[] = "";
const char c6_stage1_probe_ecall_addr[] = "";
const char c6_stage1_probe_combo_0x802_insn_addr[] = "";
const char c6_stage1_probe_combo_pmp_fault_insn_addr[] = "";
const char c6_stage1_probe_ustore_kstack_insn_addr[] = "";
const char c6_stage1_probe_ustore_caller_insn_addr[] = "";
const char c6_stage1_probe_ustore_code_insn_addr[] = "";
const char c6_stage1_probe_ufetch_insn_addr[] = "";
const char c6_stage1_probe_upmpcfg_insn_addr[] = "";
const char c6_stage1_probe_app_mmio_fault_insn_addr[] = "";
const char c6_stage1_probe_app_driver_data_fault_insn_addr[] = "";
const char c6_stage1_probe_driver_mmio_insn_addr[] = "";
const char c6_stage1_probe_driver_app_data_fault_insn_addr[] = "";
uintptr_t u_mode_probe_worker(uintptr_t, uintptr_t, uintptr_t, uintptr_t) { return 0; }
uintptr_t u_mode_driver_worker(uintptr_t, uintptr_t, uintptr_t, uintptr_t) { return 0; }
#endif

// M-mode PMA probe (段1): IRAM text の .iram1 に置く
#if defined(__riscv)
asm(
    ".pushsection .iram1, \"ax\", @progbits\n"
    ".balign 4\n"
    ".option push\n"
    ".option norvc\n"
    ".global shizuku_c6_m_store_probe\n"
    ".type shizuku_c6_m_store_probe, @function\n"
    "shizuku_c6_m_store_probe:\n"
    ".global c6_stage1_probe_mstore_iram_insn_addr\n"
    "c6_stage1_probe_mstore_iram_insn_addr:\n"
    "  sw a1, 0(a0)\n"
    "  ret\n"
    ".size shizuku_c6_m_store_probe, .-shizuku_c6_m_store_probe\n"
    "\n"
    ".global shizuku_c6_m_fetch_probe\n"
    ".type shizuku_c6_m_fetch_probe, @function\n"
    "shizuku_c6_m_fetch_probe:\n"
    "  mv t2, ra\n"
    "  mv t1, a0\n"
    ".global c6_stage1_probe_mfetch_insn_addr\n"
    "c6_stage1_probe_mfetch_insn_addr:\n"
    "  jalr ra, 0(t1)\n"
    "  mv ra, t2\n"
    "  ret\n"
    ".size shizuku_c6_m_fetch_probe, .-shizuku_c6_m_fetch_probe\n"
    ".option pop\n"
    ".popsection\n"
);
#else
const char c6_stage1_probe_mstore_iram_insn_addr[] = "";
const char c6_stage1_probe_mfetch_insn_addr[] = "";
uintptr_t shizuku_c6_m_store_probe(uintptr_t, uint32_t) { return 0; }
uintptr_t shizuku_c6_m_fetch_probe(uintptr_t) { return 0; }
#endif

namespace shizuku::selftest {

namespace {

using ARCH = KERNEL::ARCH;
using BOARD = KERNEL::BOARD;

constexpr uintptr_t OBJECT_STAGE1_PROBE = object_id::c6_stage1_probe;
constexpr uintptr_t OBJECT_STAGE1_DRIVER = object_id::c6_stage1_driver;
constexpr uintptr_t METHOD_STAGE1_PROBE = 0;

void record_check(const char *name, bool ok, unsigned long got, unsigned long want) {
  if (ok) {
    ++passed;
    BOARD::diag_printf("[C6_STAGE1] PASS %s (=%lu)\n", name, got);
  } else {
    ++failed;
    record_fail(name, got, want);
    BOARD::diag_printf("[C6_STAGE1] FAIL %s: got %lu want %lu\n", name, got, want);
  }
}

} // namespace

void c6_stage1_safety() {
  BOARD::diag_printf("[C6_STAGE1] starting stage 1 U-mode + PMP safety verification\n");

  // 0. 明示的なオブジェクト所有 range (code / data / stack / return trampoline) の登録
  g_c6_stage1_u_bounds.owner_id = OBJECT_STAGE1_PROBE;
  g_c6_stage1_u_bounds.code_base = reinterpret_cast<uintptr_t>(&c6_stage1_u_code_start);
  g_c6_stage1_u_bounds.code_limit = reinterpret_cast<uintptr_t>(&c6_stage1_u_code_end);
  if (g_c6_stage1_u_bounds.code_limit <= g_c6_stage1_u_bounds.code_base) {
    BOARD::panic("c6_stage1: invalid code bounds (fallback prohibited)");
  }
  g_c6_stage1_u_bounds.data_base = reinterpret_cast<uintptr_t>(u_mode_stage1_data_pool);
  g_c6_stage1_u_bounds.data_limit = g_c6_stage1_u_bounds.data_base + sizeof(u_mode_stage1_data_pool);
  g_c6_stage1_u_bounds.stack_base = reinterpret_cast<uintptr_t>(u_mode_stage1_stack);
  g_c6_stage1_u_bounds.stack_limit = g_c6_stage1_u_bounds.stack_base + sizeof(u_mode_stage1_stack);
  g_c6_stage1_u_bounds.u_return_trampoline = reinterpret_cast<uintptr_t>(&c6_stage1_u_return_trampoline);
  g_c6_stage1_u_bounds.role = c6_stage1_role::App;
  g_c6_stage1_u_bounds.mmio_base = 0;
  g_c6_stage1_u_bounds.mmio_limit = 0;
  g_c6_stage1_u_bounds.update_precomputed();

  // ドライバオブジェクトの登録 (role=Driver, MMIO 窓: 0x60000000..0x60001000)
  g_c6_stage1_driver_bounds.owner_id = OBJECT_STAGE1_DRIVER;
  g_c6_stage1_driver_bounds.code_base = reinterpret_cast<uintptr_t>(&c6_stage1_u_code_start);
  g_c6_stage1_driver_bounds.code_limit = reinterpret_cast<uintptr_t>(&c6_stage1_u_code_end);
  g_c6_stage1_driver_bounds.data_base = reinterpret_cast<uintptr_t>(u_mode_driver_data_pool);
  g_c6_stage1_driver_bounds.data_limit = g_c6_stage1_driver_bounds.data_base + sizeof(u_mode_driver_data_pool);
  g_c6_stage1_driver_bounds.stack_base = reinterpret_cast<uintptr_t>(u_mode_driver_stack);
  g_c6_stage1_driver_bounds.stack_limit = g_c6_stage1_driver_bounds.stack_base + sizeof(u_mode_driver_stack);
  g_c6_stage1_driver_bounds.u_return_trampoline = reinterpret_cast<uintptr_t>(&c6_stage1_u_return_trampoline);
  g_c6_stage1_driver_bounds.role = c6_stage1_role::Driver;
  g_c6_stage1_driver_bounds.mmio_base = 0x60000000u;
  g_c6_stage1_driver_bounds.mmio_limit = 0x60001000u;
  g_c6_stage1_driver_bounds.update_precomputed();

  // U worker スタックのカナリア初期化
  constexpr uint32_t U_STACK_CANARY = 0x5A5A5A5Au;
  auto *u_stack_words = reinterpret_cast<uint32_t *>(u_mode_stage1_stack);
  for (size_t i = 0; i < sizeof(u_mode_stage1_stack) / sizeof(uint32_t); ++i) {
    u_stack_words[i] = U_STACK_CANARY;
  }
  auto *drv_stack_words = reinterpret_cast<uint32_t *>(u_mode_driver_stack);
  for (size_t i = 0; i < sizeof(u_mode_driver_stack) / sizeof(uint32_t); ++i) {
    drv_stack_words[i] = U_STACK_CANARY;
  }

  // target pointer の設定
  const uintptr_t kstack_addr = reinterpret_cast<uintptr_t>(shizuku_c6_kernel_stack) + 16u;
  volatile uint32_t caller_stack_sentinel = 0xC001CAFEu;
  const uintptr_t caller_sentinel_addr = reinterpret_cast<uintptr_t>(&caller_stack_sentinel);
  constexpr uintptr_t target_mmio_addr = 0x60000008u; // UART0 CONF0 register (safe read)

  auto *data_pool_words = reinterpret_cast<volatile uintptr_t *>(u_mode_stage1_data_pool);
  data_pool_words[0] = kstack_addr;
  data_pool_words[1] = caller_sentinel_addr;
  data_pool_words[2] = target_mmio_addr;
  data_pool_words[3] = reinterpret_cast<uintptr_t>(u_mode_driver_data_pool);
  data_pool_words[5] = OBJECT_STAGE1_DRIVER;
  data_pool_words[6] = METHOD_STAGE1_PROBE;

  auto *drv_pool_words = reinterpret_cast<volatile uintptr_t *>(u_mode_driver_data_pool);
  drv_pool_words[0] = target_mmio_addr;
  drv_pool_words[1] = reinterpret_cast<uintptr_t>(u_mode_stage1_data_pool);

  // 1. オブジェクト登録
  const auto create_res = ARCH::syscall(
      static_cast<uintptr_t>(object_api::CREATE_OBJECT),
      OBJECT_STAGE1_PROBE,
      reinterpret_cast<uintptr_t>(&u_mode_probe_worker),
      OBJECT_UNPRIVILEGED);
  record_check("stage1: create unprivileged probe object",
               create_res.error == 0, create_res.error, 0);

  const auto create_drv_res = ARCH::syscall(
      static_cast<uintptr_t>(object_api::CREATE_OBJECT),
      OBJECT_STAGE1_DRIVER,
      reinterpret_cast<uintptr_t>(&u_mode_driver_worker),
      OBJECT_UNPRIVILEGED);
  record_check("stage1: create unprivileged driver object",
               create_drv_res.error == 0, create_drv_res.error, 0);

  if (create_res.error != 0 || create_drv_res.error != 0) {
    BOARD::diag_printf("[C6_STAGE1] ABORT: probe object creation failed; dependent tests aborted\n");
    return;
  }

  // 2. 0x802 U-mode 読み取り診断
  g_c6_probe_0x802_pc = reinterpret_cast<uintptr_t>(&c6_stage1_probe_0x802_insn_addr);
  const auto diag_0x802 = ARCH::syscall(
      static_cast<uintptr_t>(object_api::CALL_METHOD),
      OBJECT_STAGE1_PROBE,
      METHOD_STAGE1_PROBE,
      1 /* cmd=1 */);

  if (shizuku_c6_u_mode_diag[0] == 0) {
    if (diag_0x802.error == 0 && diag_0x802.value != 0xFFFFFFFFu) {
      shizuku_c6_u_mode_diag[0] = 1;
      shizuku_c6_u_mode_diag[1] = diag_0x802.value;
    }
  }

  const uint32_t status_0x802 = shizuku_c6_u_mode_diag[0];
  const bool status_valid = (status_0x802 == 1u || status_0x802 == 2u);
  record_check("stage1: 0x802 U-mode read status is deterministic (1=ok, 2=illegal)",
               status_valid, status_0x802, status_0x802 ? status_0x802 : 2u);

  if (status_0x802 == 1u) {
    record_check("stage1: 0x802 read value returned non-error",
                 diag_0x802.error == 0 && diag_0x802.value != 0xFFFFFFFFu, 1, 1);
  } else if (status_0x802 == 2u) {
    record_check("stage1: illegal instruction trap confirmed (not disguised as success)",
                 diag_0x802.value == 0xFFFFFFFFu, 1, 1);
  }

  // 3. PMP 境界隔離テスト: U-mode からカーネルスタックの読み取り試行
  g_c6_probe_pmp_fault_pc = reinterpret_cast<uintptr_t>(&c6_stage1_probe_pmp_fault_insn_addr);
  const auto pmp_res = ARCH::syscall(
      static_cast<uintptr_t>(object_api::CALL_METHOD),
      OBJECT_STAGE1_PROBE,
      METHOD_STAGE1_PROBE,
      2 /* cmd=2 */);

  record_check("stage1: PMP denied U-mode reading kernel stack (fault trapped)",
               pmp_res.value == 0xDEADBEEFu && shizuku_c6_u_mode_diag[3] > 0,
               shizuku_c6_u_mode_diag[3], 1);
  record_check("stage1: PMP fault exact mcause is 5 (Load Access Fault)",
               shizuku_c6_u_mode_diag[4] == 5u, shizuku_c6_u_mode_diag[4], 5u);
  record_check("stage1: PMP fault exact mepc is probe insn",
               shizuku_c6_u_mode_diag[5] == reinterpret_cast<uintptr_t>(&c6_stage1_probe_pmp_fault_insn_addr),
               shizuku_c6_u_mode_diag[5], reinterpret_cast<uintptr_t>(&c6_stage1_probe_pmp_fault_insn_addr));
  record_check("stage1: PMP fault exact mtval is target kstack address",
               shizuku_c6_u_mode_diag[6] == kstack_addr, shizuku_c6_u_mode_diag[6], kstack_addr);

  // 4. caller-stack 隔離テスト
  g_c6_probe_caller_stack_fault_pc = reinterpret_cast<uintptr_t>(&c6_stage1_probe_caller_stack_fault_insn_addr);
  const auto caller_stack_res = ARCH::syscall(
      static_cast<uintptr_t>(object_api::CALL_METHOD),
      OBJECT_STAGE1_PROBE,
      METHOD_STAGE1_PROBE,
      5 /* cmd=5 */);

  record_check("stage1: PMP denied U-mode reading caller stack sentinel (fault trapped)",
               caller_stack_res.value == 0xCAFEBABEu && shizuku_c6_u_mode_diag[7] > 0,
               shizuku_c6_u_mode_diag[7], 1);
  record_check("stage1: caller stack fault exact mcause is 5 (Load Access Fault)",
               shizuku_c6_u_mode_diag[4] == 5u, shizuku_c6_u_mode_diag[4], 5u);
  record_check("stage1: caller stack fault exact mepc is probe insn",
               shizuku_c6_u_mode_diag[5] == reinterpret_cast<uintptr_t>(&c6_stage1_probe_caller_stack_fault_insn_addr),
               shizuku_c6_u_mode_diag[5], reinterpret_cast<uintptr_t>(&c6_stage1_probe_caller_stack_fault_insn_addr));
  record_check("stage1: caller stack fault exact mtval is caller sentinel address",
               shizuku_c6_u_mode_diag[6] == caller_sentinel_addr, shizuku_c6_u_mode_diag[6], caller_sentinel_addr);
  record_check("stage1: caller stack sentinel intact after return",
               caller_stack_sentinel == 0xC001CAFEu, caller_stack_sentinel, 0xC001CAFEu);

  // 5. U-mode ecall 往復と信頼 stack 切り替えの検証
  for (size_t i = 0; i < 3; ++i) shizuku_c6_u_ecall_diag[i] = 0;
  const auto ecall_res = ARCH::syscall(
      static_cast<uintptr_t>(object_api::CALL_METHOD),
      OBJECT_STAGE1_PROBE,
      METHOD_STAGE1_PROBE,
      3 /* cmd=3 */);
  record_check("stage1: U-mode ecall successfully entered M-mode and returned via trampoline",
               ecall_res.error == 0 && ecall_res.value == OBJECT_STAGE1_PROBE,
               ecall_res.value, OBJECT_STAGE1_PROBE);
  record_check("stage1: U-mode ecall counter incremented",
               shizuku_c6_u_mode_diag[2] > 0, shizuku_c6_u_mode_diag[2], 1);
  record_check("stage1: U-mode ecall trap mcause is 8 (Environment call from U-mode)",
               shizuku_c6_u_ecall_diag[0] == 8u, shizuku_c6_u_ecall_diag[0], 8u);
  const uintptr_t u_ecall_mepc = shizuku_c6_u_ecall_diag[1];
  const uintptr_t u_code_base = g_c6_stage1_u_bounds.code_base;
  const uintptr_t u_code_limit = g_c6_stage1_u_bounds.code_limit;
  const bool u_ecall_mepc_in_bounds = u_ecall_mepc >= u_code_base && u_ecall_mepc < u_code_limit;
  record_check("stage1: U-mode ecall trap mepc within object code bounds",
               u_ecall_mepc_in_bounds, u_ecall_mepc_in_bounds ? 1 : 0, 1);
  const uintptr_t u_ecall_resume_pc = reinterpret_cast<uintptr_t>(&c6_stage1_probe_ecall_addr) + 4;
  record_check("stage1: U-mode ecall trap exact mepc is probe ecall addr + 4 (resume pc)",
               shizuku_c6_u_ecall_diag[1] == u_ecall_resume_pc,
               shizuku_c6_u_ecall_diag[1], u_ecall_resume_pc);

  // 6. 非特権からの特権操作の拒否検証 (U から特権 API 拒否)
  const auto unpriv_res = ARCH::syscall(
      static_cast<uintptr_t>(object_api::CALL_METHOD),
      OBJECT_STAGE1_PROBE,
      METHOD_STAGE1_PROBE,
      4 /* cmd=4: call unprivileged primitive */);
  record_check("stage1: unprivileged primitive call was denied",
               unpriv_res.value != 0, unpriv_res.value, 1);

  // 7. Multi-trap combo
  volatile uintptr_t caller_sp_before = 0;
#if defined(__riscv)
  asm volatile("mv %0, sp" : "=r"(caller_sp_before));
#endif
  g_c6_probe_combo_0x802_pc = reinterpret_cast<uintptr_t>(&c6_stage1_probe_combo_0x802_insn_addr);
  g_c6_probe_combo_pmp_pc = reinterpret_cast<uintptr_t>(&c6_stage1_probe_combo_pmp_fault_insn_addr);
  const uint32_t combo_pmp_faults_before = shizuku_c6_u_mode_diag[3];
  const auto combo_res = ARCH::syscall(
      static_cast<uintptr_t>(object_api::CALL_METHOD),
      OBJECT_STAGE1_PROBE,
      METHOD_STAGE1_PROBE,
      6 /* cmd=6: multi-trap combo */);
  volatile uintptr_t caller_sp_after = 0;
#if defined(__riscv)
  asm volatile("mv %0, sp" : "=r"(caller_sp_after));
#endif

  record_check("stage1: multi-trap combo returned correct computed value",
               combo_res.error == 0 && combo_res.value == (0x5AFE0000u | OBJECT_STAGE1_PROBE),
               combo_res.value, 0x5AFE0000u | OBJECT_STAGE1_PROBE);
  record_check("stage1: caller SP exactly retained across multi-trap invocation",
               caller_sp_after == caller_sp_before, caller_sp_after, caller_sp_before);
  const bool is_privileged = ARCH::current_priv();
  record_check("stage1: caller privileged state maintained as M-mode after return",
               is_privileged, is_privileged ? 1 : 0, 1);
  record_check("stage1: multi-trap combo incremented PMP fault counter by 1",
               shizuku_c6_u_mode_diag[3] == combo_pmp_faults_before + 1u,
               shizuku_c6_u_mode_diag[3], combo_pmp_faults_before + 1u);
  record_check("stage1: multi-trap combo exact fault mcause is 5",
               shizuku_c6_u_mode_diag[4] == 5u, shizuku_c6_u_mode_diag[4], 5u);
  record_check("stage1: multi-trap combo exact fault mepc is combo probe insn",
               shizuku_c6_u_mode_diag[5] == reinterpret_cast<uintptr_t>(&c6_stage1_probe_combo_pmp_fault_insn_addr),
               shizuku_c6_u_mode_diag[5], reinterpret_cast<uintptr_t>(&c6_stage1_probe_combo_pmp_fault_insn_addr));
  record_check("stage1: multi-trap combo exact fault mtval is kstack address",
               shizuku_c6_u_mode_diag[6] == kstack_addr, shizuku_c6_u_mode_diag[6], kstack_addr);

  // 8. M-mode native handler スタック非露出検証
  bool canary_intact = true;
  for (size_t i = 0; i < sizeof(u_mode_stage1_stack) / sizeof(uint32_t); ++i) {
    if (u_stack_words[i] != U_STACK_CANARY) {
      canary_intact = false;
      break;
    }
  }
  record_check("stage1: privileged M handler stack did not spill into U stack canary (full stack)",
               canary_intact, canary_intact ? 1 : 0, 1);

  // 9. 本番判定述語によるオブジェクト ID 束縛・拒否の検証
  const bool auth_probe = shizuku_c6_is_authorized_u_object(OBJECT_STAGE1_PROBE, OBJECT_STAGE1_PROBE);
  const bool auth_driver = shizuku_c6_is_authorized_u_object(OBJECT_STAGE1_DRIVER, OBJECT_STAGE1_DRIVER);
  const bool reject_foreign_owner = !shizuku_c6_is_authorized_u_object(0x62u, OBJECT_STAGE1_PROBE);
  const bool reject_foreign_cur = !shizuku_c6_is_authorized_u_object(OBJECT_STAGE1_PROBE, 0x62u);
  const bool reject_zero = !shizuku_c6_is_authorized_u_object(0u, 0u);
  record_check("stage1: production predicate accepts authorized probe and driver",
               auth_probe && auth_driver, 1, 1);
  record_check("stage1: production predicate rejects foreign owner ID", reject_foreign_owner, reject_foreign_owner ? 1 : 0, 1);
  record_check("stage1: production predicate rejects foreign current_object", reject_foreign_cur, reject_foreign_cur ? 1 : 0, 1);
  record_check("stage1: production predicate rejects uninitialized 0x0", reject_zero, reject_zero ? 1 : 0, 1);

  // 10. 本番スタック切り替え述語の検証
  const uintptr_t test_u_sp = g_c6_stage1_u_bounds.stack_base + 512u;
  const uintptr_t kstack_valid_frame = reinterpret_cast<uintptr_t>(shizuku_c6_kernel_stack) + 3600u;
  const uintptr_t routed_m_sp = shizuku_c6_get_m_handler_stack(test_u_sp, kstack_valid_frame);
  const uintptr_t expected_snapshot = (kstack_valid_frame + 144u);
  const bool routed_inside_kstack = (routed_m_sp >= reinterpret_cast<uintptr_t>(shizuku_c6_kernel_stack) &&
                                     routed_m_sp <= reinterpret_cast<uintptr_t>(shizuku_c6_kernel_stack + sizeof(shizuku_c6_kernel_stack)));
  const bool routed_outside_u_stack = (routed_m_sp < g_c6_stage1_u_bounds.stack_base ||
                                       routed_m_sp > g_c6_stage1_u_bounds.stack_limit);
  record_check("stage1: M handler stack routed inside trusted kernel stack as valid aligned snapshot",
               routed_inside_kstack && (routed_m_sp == expected_snapshot), routed_m_sp, expected_snapshot);
  record_check("stage1: M handler stack routed strictly outside U stack",
               routed_outside_u_stack, routed_outside_u_stack ? 1 : 0, 1);

  const uintptr_t kstack_unaligned_frame = kstack_valid_frame + 4u;
  const uintptr_t rejected_unaligned_sp = shizuku_c6_get_m_handler_stack(test_u_sp, kstack_unaligned_frame);
  record_check("stage1: M handler stack rejects unaligned frame (fail-closed 0)",
               rejected_unaligned_sp == 0, rejected_unaligned_sp, 0);

  const uintptr_t kstack_underflow_frame = reinterpret_cast<uintptr_t>(shizuku_c6_kernel_stack) + 128u;
  const uintptr_t rejected_underflow_sp = shizuku_c6_get_m_handler_stack(test_u_sp, kstack_underflow_frame);
  record_check("stage1: M handler stack rejects frame below guard headroom (fail-closed 0)",
               rejected_underflow_sp == 0, rejected_underflow_sp, 0);

  // 11. 窓外ストアアクセスで fault
  // 11a. U store to kernel stack (cmd 7)
  volatile uint32_t *kstack_word = reinterpret_cast<volatile uint32_t *>(kstack_addr);
  *kstack_word = 0x5EA1ED00u;
  g_c6_probe_ustore_kstack_pc = reinterpret_cast<uintptr_t>(&c6_stage1_probe_ustore_kstack_insn_addr);
  const auto ustore_kstack_res = ARCH::syscall(
      static_cast<uintptr_t>(object_api::CALL_METHOD),
      OBJECT_STAGE1_PROBE,
      METHOD_STAGE1_PROBE,
      7 /* cmd=7 */);
  g_c6_probe_ustore_kstack_pc = 0;
  record_check("stage1: U store to kernel stack denied (fault trapped)",
               ustore_kstack_res.value == 0x0BADF00Du && shizuku_c6_u_mode_diag[8] == 1u,
               shizuku_c6_u_mode_diag[8], 1);
  record_check("stage1: U kernel stack store exact mcause is 7 (Store/AMO Access Fault)",
               shizuku_c6_u_mode_diag[4] == 7u, shizuku_c6_u_mode_diag[4], 7u);
  record_check("stage1: U kernel stack store exact mepc is probe insn",
               shizuku_c6_u_mode_diag[5] == reinterpret_cast<uintptr_t>(&c6_stage1_probe_ustore_kstack_insn_addr),
               shizuku_c6_u_mode_diag[5], reinterpret_cast<uintptr_t>(&c6_stage1_probe_ustore_kstack_insn_addr));
  record_check("stage1: U kernel stack store exact mtval is kstack address",
               shizuku_c6_u_mode_diag[6] == kstack_addr, shizuku_c6_u_mode_diag[6], kstack_addr);
  record_check("stage1: kernel stack word intact after denied U store",
               *kstack_word == 0x5EA1ED00u, *kstack_word, 0x5EA1ED00u);

  // 11b. U store to caller stack sentinel (cmd 8)
  g_c6_probe_ustore_caller_pc = reinterpret_cast<uintptr_t>(&c6_stage1_probe_ustore_caller_insn_addr);
  const auto ustore_caller_res = ARCH::syscall(
      static_cast<uintptr_t>(object_api::CALL_METHOD),
      OBJECT_STAGE1_PROBE,
      METHOD_STAGE1_PROBE,
      8 /* cmd=8 */);
  g_c6_probe_ustore_caller_pc = 0;
  record_check("stage1: U store to caller stack denied (fault trapped)",
               ustore_caller_res.value == 0x0BADF00Du && shizuku_c6_u_mode_diag[9] == 1u,
               shizuku_c6_u_mode_diag[9], 1);
  record_check("stage1: U caller stack store exact mcause is 7 (Store/AMO Access Fault)",
               shizuku_c6_u_mode_diag[4] == 7u, shizuku_c6_u_mode_diag[4], 7u);
  record_check("stage1: U caller stack store exact mepc is probe insn",
               shizuku_c6_u_mode_diag[5] == reinterpret_cast<uintptr_t>(&c6_stage1_probe_ustore_caller_insn_addr),
               shizuku_c6_u_mode_diag[5], reinterpret_cast<uintptr_t>(&c6_stage1_probe_ustore_caller_insn_addr));
  record_check("stage1: U caller stack store exact mtval is caller sentinel address",
               shizuku_c6_u_mode_diag[6] == caller_sentinel_addr, shizuku_c6_u_mode_diag[6], caller_sentinel_addr);
  record_check("stage1: caller stack sentinel intact after denied U store",
               caller_stack_sentinel == 0xC001CAFEu, caller_stack_sentinel, 0xC001CAFEu);

  // 11c. U store into own code region (cmd 9)
  const uintptr_t u_worker_addr = reinterpret_cast<uintptr_t>(&u_mode_probe_worker);
  volatile uint32_t *code_word = reinterpret_cast<volatile uint32_t *>(u_worker_addr);
  const uint32_t code_word_before = *code_word;
  g_c6_probe_ustore_code_pc = reinterpret_cast<uintptr_t>(&c6_stage1_probe_ustore_code_insn_addr);
  const auto ustore_code_res = ARCH::syscall(
      static_cast<uintptr_t>(object_api::CALL_METHOD),
      OBJECT_STAGE1_PROBE,
      METHOD_STAGE1_PROBE,
      9 /* cmd=9 */);
  g_c6_probe_ustore_code_pc = 0;
  const uint32_t code_word_after = *code_word;
  record_check("stage1: U store into own code region denied (fault trapped)",
               ustore_code_res.value == 0 && shizuku_c6_u_mode_diag[10] == 1u,
               shizuku_c6_u_mode_diag[10], 1);
  record_check("stage1: U code store exact mcause is 7 (Store/AMO Access Fault)",
               shizuku_c6_u_mode_diag[4] == 7u, shizuku_c6_u_mode_diag[4], 7u);
  record_check("stage1: U code store exact mepc is probe insn",
               shizuku_c6_u_mode_diag[5] == reinterpret_cast<uintptr_t>(&c6_stage1_probe_ustore_code_insn_addr),
               shizuku_c6_u_mode_diag[5], reinterpret_cast<uintptr_t>(&c6_stage1_probe_ustore_code_insn_addr));
  record_check("stage1: U code store exact mtval is probe worker code address",
               shizuku_c6_u_mode_diag[6] == u_worker_addr, shizuku_c6_u_mode_diag[6], u_worker_addr);
  record_check("stage1: U code word unchanged after denied U store",
               code_word_after == code_word_before, code_word_after, code_word_before);

  // 11d. U instruction fetch from U stack (cmd 10)
  const uintptr_t ufetch_target = reinterpret_cast<uintptr_t>(u_mode_stage1_stack);
  g_c6_probe_ufetch_target = ufetch_target;
  const auto ufetch_res = ARCH::syscall(
      static_cast<uintptr_t>(object_api::CALL_METHOD),
      OBJECT_STAGE1_PROBE,
      METHOD_STAGE1_PROBE,
      10 /* cmd=10 */);
  g_c6_probe_ufetch_target = 0;
  record_check("stage1: U instruction fetch from U stack denied (fault trapped)",
               ufetch_res.value == 0 && shizuku_c6_u_mode_diag[11] == 1u,
               shizuku_c6_u_mode_diag[11], 1);
  record_check("stage1: U fetch exact mcause is 1 (Instruction Access Fault)",
               shizuku_c6_u_mode_diag[4] == 1u, shizuku_c6_u_mode_diag[4], 1u);
  record_check("stage1: U fetch exact mepc is fetch target (U stack)",
               shizuku_c6_u_mode_diag[5] == ufetch_target, shizuku_c6_u_mode_diag[5], ufetch_target);
  record_check("stage1: U fetch exact mtval is fetch target (U stack)",
               shizuku_c6_u_mode_diag[6] == ufetch_target, shizuku_c6_u_mode_diag[6], ufetch_target);

  // 11e. U csrr pmpcfg0 (0x3A0) (cmd 11)
  g_c6_probe_upmpcfg_pc = reinterpret_cast<uintptr_t>(&c6_stage1_probe_upmpcfg_insn_addr);
  const auto upmpcfg_res = ARCH::syscall(
      static_cast<uintptr_t>(object_api::CALL_METHOD),
      OBJECT_STAGE1_PROBE,
      METHOD_STAGE1_PROBE,
      11 /* cmd=11 */);
  g_c6_probe_upmpcfg_pc = 0;
  record_check("stage1: U csrr pmpcfg0 denied as illegal instruction (trapped)",
               upmpcfg_res.value == 0xE00Eu && shizuku_c6_u_mode_diag[12] == 1u,
               shizuku_c6_u_mode_diag[12], 1);
  record_check("stage1: U pmpcfg0 illegal exact mcause is 2",
               shizuku_c6_u_mode_diag[4] == 2u, shizuku_c6_u_mode_diag[4], 2u);
  record_check("stage1: U pmpcfg0 illegal exact mepc is probe insn",
               shizuku_c6_u_mode_diag[5] == reinterpret_cast<uintptr_t>(&c6_stage1_probe_upmpcfg_insn_addr),
               shizuku_c6_u_mode_diag[5], reinterpret_cast<uintptr_t>(&c6_stage1_probe_upmpcfg_insn_addr));

  // 12. アプリ⇄ドライバ窓分離テスト (MMIO 窓と private data 窓の分離)
  // 12a. ドライバは MMIO 窓 (0x60000008) を正常に読める
  const auto drv_mmio_res = ARCH::syscall(
      static_cast<uintptr_t>(object_api::CALL_METHOD),
      OBJECT_STAGE1_DRIVER,
      METHOD_STAGE1_PROBE,
      1 /* cmd=1: driver read mmio */);
  record_check("stage1: driver reads MMIO successfully without fault",
               drv_mmio_res.error == 0 && shizuku_c6_u_mode_diag[13] == 0u,
               drv_mmio_res.error, 0);

  // 12b. アプリは MMIO に触れると PMP Load Access Fault (mcause 5)
  g_c6_probe_app_mmio_fault_pc = reinterpret_cast<uintptr_t>(&c6_stage1_probe_app_mmio_fault_insn_addr);
  const auto app_mmio_res = ARCH::syscall(
      static_cast<uintptr_t>(object_api::CALL_METHOD),
      OBJECT_STAGE1_PROBE,
      METHOD_STAGE1_PROBE,
      12 /* cmd=12: app read mmio */);
  g_c6_probe_app_mmio_fault_pc = 0;
  record_check("stage1: app access to MMIO denied by PMP (fault trapped)",
               app_mmio_res.value == 0xE010u && shizuku_c6_u_mode_diag[13] == 1u,
               shizuku_c6_u_mode_diag[13], 1);
  record_check("stage1: app MMIO fault exact mcause is 5 (Load Access Fault)",
               shizuku_c6_u_mode_diag[4] == 5u, shizuku_c6_u_mode_diag[4], 5u);
  record_check("stage1: app MMIO fault exact mepc is probe insn",
               shizuku_c6_u_mode_diag[5] == reinterpret_cast<uintptr_t>(&c6_stage1_probe_app_mmio_fault_insn_addr),
               shizuku_c6_u_mode_diag[5], reinterpret_cast<uintptr_t>(&c6_stage1_probe_app_mmio_fault_insn_addr));
  record_check("stage1: app MMIO fault exact mtval is MMIO target address",
               shizuku_c6_u_mode_diag[6] == target_mmio_addr, shizuku_c6_u_mode_diag[6], target_mmio_addr);

  // 12c. アプリはドライバの private data pool に触れると PMP Load Access Fault
  g_c6_probe_app_driver_data_fault_pc = reinterpret_cast<uintptr_t>(&c6_stage1_probe_app_driver_data_fault_insn_addr);
  const auto app_drv_data_res = ARCH::syscall(
      static_cast<uintptr_t>(object_api::CALL_METHOD),
      OBJECT_STAGE1_PROBE,
      METHOD_STAGE1_PROBE,
      13 /* cmd=13: app read driver data */);
  g_c6_probe_app_driver_data_fault_pc = 0;
  record_check("stage1: app access to driver private data denied by PMP (fault trapped)",
               app_drv_data_res.value == 0xE011u && shizuku_c6_u_mode_diag[14] == 1u,
               shizuku_c6_u_mode_diag[14], 1);
  record_check("stage1: app driver data fault exact mcause is 5",
               shizuku_c6_u_mode_diag[4] == 5u, shizuku_c6_u_mode_diag[4], 5u);

  // 12d. ドライバはアプリの private data pool に触れると PMP Load Access Fault
  g_c6_probe_driver_app_data_fault_pc = reinterpret_cast<uintptr_t>(&c6_stage1_probe_driver_app_data_fault_insn_addr);
  const auto drv_app_data_res = ARCH::syscall(
      static_cast<uintptr_t>(object_api::CALL_METHOD),
      OBJECT_STAGE1_DRIVER,
      METHOD_STAGE1_PROBE,
      2 /* cmd=2: driver read app data */);
  g_c6_probe_driver_app_data_fault_pc = 0;
  record_check("stage1: driver access to app private data denied by PMP (fault trapped)",
               drv_app_data_res.value == 0xE012u && shizuku_c6_u_mode_diag[15] == 1u,
               shizuku_c6_u_mode_diag[15], 1);
  record_check("stage1: driver app data fault exact mcause is 5",
               shizuku_c6_u_mode_diag[4] == 5u, shizuku_c6_u_mode_diag[4], 5u);

  // 13. M-mode PMA 境界 probe (段1)
#if defined(__riscv)
  const uintptr_t iram_stub_addr = ARCH::return_stub();
  volatile uint32_t *iram_word = reinterpret_cast<volatile uint32_t *>(iram_stub_addr);
  const uint32_t iram_word_read = *iram_word;
  const uint32_t iram_word_reread = *iram_word;
  record_check("stage1: M-mode lw from IRAM text word returns stable non-zero value (positive control)",
               iram_word_read != 0 && iram_word_read == iram_word_reread, iram_word_read, iram_word_reread);

  g_c6_probe_mstore_iram_pc = reinterpret_cast<uintptr_t>(&c6_stage1_probe_mstore_iram_insn_addr);
  const uintptr_t mstore_res = shizuku_c6_m_store_probe(iram_stub_addr, iram_word_read);
  g_c6_probe_mstore_iram_pc = 0;
  record_check("stage1: M-mode store to IRAM text denied by PMA (fault trapped)",
               mstore_res == 0 && shizuku_c6_m_pma_diag[0] == 1u,
               shizuku_c6_m_pma_diag[0], 1);
  record_check("stage1: M-mode IRAM text store exact mcause is 7 (Store/AMO Access Fault)",
               shizuku_c6_u_mode_diag[4] == 7u, shizuku_c6_u_mode_diag[4], 7u);
  record_check("stage1: M-mode IRAM text store exact mepc is probe insn",
               shizuku_c6_u_mode_diag[5] == reinterpret_cast<uintptr_t>(&c6_stage1_probe_mstore_iram_insn_addr),
               shizuku_c6_u_mode_diag[5], reinterpret_cast<uintptr_t>(&c6_stage1_probe_mstore_iram_insn_addr));
  record_check("stage1: M-mode IRAM text store exact mtval is IRAM target address",
               shizuku_c6_u_mode_diag[6] == iram_stub_addr, shizuku_c6_u_mode_diag[6], iram_stub_addr);
  record_check("stage1: IRAM text word unchanged after denied M-mode store",
               *iram_word == iram_word_read, *iram_word, iram_word_read);

  static volatile uint32_t m_mode_dram_word;
  m_mode_dram_word = 0xD0A1C0DEu;
  const uint32_t m_mode_dram_read = m_mode_dram_word;
  record_check("stage1: M-mode DRAM volatile store+load roundtrip matches (positive control)",
               m_mode_dram_read == 0xD0A1C0DEu, m_mode_dram_read, 0xD0A1C0DEu);

  static volatile uint32_t m_mode_dram_code[2] = {0x00008067u, 0x00008067u}; // ret; ret (DRAM, X 無し)
  const uintptr_t mfetch_target = reinterpret_cast<uintptr_t>(m_mode_dram_code);
  g_c6_probe_mfetch_target = mfetch_target;
  const uintptr_t mfetch_res = shizuku_c6_m_fetch_probe(mfetch_target);
  g_c6_probe_mfetch_target = 0;
  record_check("stage1: M-mode instruction fetch from DRAM denied by PMA (fault trapped)",
               mfetch_res == 0 && shizuku_c6_m_pma_diag[1] == 1u,
               shizuku_c6_m_pma_diag[1], 1);
  record_check("stage1: M-mode DRAM fetch exact mcause is 1 (Instruction Access Fault)",
               shizuku_c6_u_mode_diag[4] == 1u, shizuku_c6_u_mode_diag[4], 1u);
  record_check("stage1: M-mode DRAM fetch exact mepc is DRAM fetch target",
               shizuku_c6_u_mode_diag[5] == mfetch_target, shizuku_c6_u_mode_diag[5], mfetch_target);
  record_check("stage1: M-mode DRAM fetch exact mtval is DRAM fetch target",
               shizuku_c6_u_mode_diag[6] == mfetch_target, shizuku_c6_u_mode_diag[6], mfetch_target);

  // 14. U→U method call 連続 2 回の検証 (1回目と2回目を別々に assert)
  const auto u2u_res1 = ARCH::syscall(
      static_cast<uintptr_t>(object_api::CALL_METHOD),
      OBJECT_STAGE1_PROBE,
      METHOD_STAGE1_PROBE,
      14 /* cmd=14: U->U call test 1st */);
  record_check("stage1: U-to-U call 1st iteration returned 0x42",
               u2u_res1.error == 0 && u2u_res1.value == 0x42u,
               u2u_res1.value, 0x42u);

  const auto u2u_res2 = ARCH::syscall(
      static_cast<uintptr_t>(object_api::CALL_METHOD),
      OBJECT_STAGE1_PROBE,
      METHOD_STAGE1_PROBE,
      15 /* cmd=15: U->U call test 2nd */);
  record_check("stage1: U-to-U call 2nd iteration returned 0x42",
               u2u_res2.error == 0 && u2u_res2.value == 0x42u,
               u2u_res2.value, 0x42u);

  // 15. 2回目の後の mscratch 健全性確認、および U callee 窓アクセス fault、caller 自窓復帰の検証
  // 実行前に diag[15] (および fault diag 4, 6) を 0 に初期化
  shizuku_c6_u_mode_diag[15] = 0;
  shizuku_c6_u_mode_diag[4] = 0;
  shizuku_c6_u_mode_diag[6] = 0;

  g_c6_probe_driver_app_data_fault_pc = reinterpret_cast<uintptr_t>(&c6_stage1_probe_driver_app_data_fault_insn_addr);
  const auto u2u_res3 = ARCH::syscall(
      static_cast<uintptr_t>(object_api::CALL_METHOD),
      OBJECT_STAGE1_PROBE,
      METHOD_STAGE1_PROBE,
      16 /* cmd=16: U->U fault test */);
  g_c6_probe_driver_app_data_fault_pc = 0;

  // 2回目の後に 3回目の trap が成功したこと (mscratch の健全性)
  record_check("stage1: 3rd trap after 2 consecutive U-to-U calls succeeded (mscratch sound)",
               u2u_res3.error == 0 && u2u_res3.value == 0xE012u,
               u2u_res3.value, 0xE012u);

  // callee がアクセスした直後に fault した (mcause=5, mtval が caller 窓アドレス) ことを assert
  const uintptr_t caller_window_addr = reinterpret_cast<uintptr_t>(u_mode_stage1_data_pool);
  record_check("stage1: U callee access to caller window caused fault trapped (diag[15] == 1)",
               shizuku_c6_u_mode_diag[15] == 1u,
               shizuku_c6_u_mode_diag[15], 1u);
  record_check("stage1: U callee fault exact mcause is 5 (Load Access Fault)",
               shizuku_c6_u_mode_diag[4] == 5u,
               shizuku_c6_u_mode_diag[4], 5u);
  record_check("stage1: U callee fault exact mtval matches caller data pool address",
               shizuku_c6_u_mode_diag[6] == caller_window_addr,
               shizuku_c6_u_mode_diag[6], caller_window_addr);

  // caller 復帰後の自窓アクセス健全性
  record_check("stage1: caller restored its own data window successfully",
               data_pool_words[4] == 0xBEEFCAFEu,
               data_pool_words[4], 0xBEEFCAFEu);
#endif

  BOARD::diag_printf("[C6_STAGE1] stage 1 verification complete: passed=%lu failed=%lu\n",
                     (unsigned long)passed, (unsigned long)failed);
}

} // namespace shizuku::selftest
