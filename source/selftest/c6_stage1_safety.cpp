// ===========================================================================
//  ESP32-C6 段1: U-mode + PMP 境界・trusted U stack・0x802 診断安全性テスト
// ===========================================================================
//  ★段1 不変条件の検証:
//    1. U-mode thread が ecall で M-mode に入り、信頼 stack 上でのみディスパッチされ、
//       mret で U-mode に復帰する最小の安全な経路。
//    2. PMP allow-list により、実行対象 object code/data/U stack だけが許可され、
//       kernel/trusted stack/復帰情報および呼び出し元 (caller) スタックは U-mode から隔離され、
//       アクセスすると PMP fault になること。
//    3. U-mode return trampoline が U 所有 code 範囲内に存在し、明示的 ecall 経由で
//       安全に M-mode に復帰すること (kernel 広域 RX 禁止)。
//    4. 0x802 U-mode read の成否を曖昧にせず、成功 / illegal-instruction を独立して診断・記録。
//       決して illegal trap を成功扱いしない。
//    5. 非特権からの不正な特権操作・未認可呼出しが拒否されること。
//    6. U object identity (owner_id) に束縛され、未登録/別 ID は fail-closed で拒否されること。

#include <cstdint>
#include <cstddef>
#include "shizuku/kernel.hpp"
#include "shizuku/object_api.hpp"
#include "shizuku/object_ids.hpp"
#include "shizuku/selftest.hpp"
#include "chips/esp32_c6/selftest_record.h"
#include "chips/esp32_c6/pmp_c6.hpp"

using c6_stage1_object_bounds = shizuku::chips::esp32_c6::c6_stage1_object_bounds;

extern "C" {
bool shizuku_c6_is_authorized_u_object(uint32_t owner_id, uint32_t current_obj);
uintptr_t shizuku_c6_get_m_handler_stack(uintptr_t current_sp, uintptr_t frame_sp);
bool shizuku_c6_is_dispatch_stack_address(uintptr_t addr);
extern volatile uint32_t shizuku_c6_trap_diag[5];
extern volatile uint32_t shizuku_c6_u_mode_diag[8];
extern volatile uintptr_t g_c6_probe_0x802_pc;
extern volatile uintptr_t g_c6_probe_pmp_fault_pc;
extern volatile uintptr_t g_c6_probe_caller_stack_fault_pc;
extern volatile uintptr_t g_c6_probe_combo_0x802_pc;
extern volatile uintptr_t g_c6_probe_combo_pmp_pc;
extern c6_stage1_object_bounds g_c6_stage1_u_bounds;
extern unsigned char shizuku_c6_kernel_stack[4096];
extern unsigned char shizuku_c6_dispatch_stack[4096];

// オブジェクト所有データ領域 (256B, 16B 整列) - U-mode から読み書き可能な唯一の data pool
alignas(16) uint8_t u_mode_stage1_data_pool[256];
// オブジェクト専用スタック (1024B, 16B 整列) - caller stack から完全に物理分離
alignas(16) uint8_t u_mode_stage1_stack[1024];

// プローブ命令の exact PC ラベル
extern const char c6_stage1_u_code_start[];
extern const char c6_stage1_u_code_end[];
extern const char c6_stage1_u_return_trampoline[];
extern const char c6_stage1_probe_0x802_insn_addr[];
extern const char c6_stage1_probe_pmp_fault_insn_addr[];
extern const char c6_stage1_probe_caller_stack_fault_insn_addr[];
extern const char c6_stage1_probe_ecall_insn_addr[];
extern const char c6_stage1_probe_combo_0x802_insn_addr[];
extern const char c6_stage1_probe_combo_pmp_fault_insn_addr[];
uintptr_t u_mode_probe_worker(uintptr_t cmd, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3);
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
    ".size u_mode_probe_worker, .-u_mode_probe_worker\n"
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
const char c6_stage1_probe_combo_0x802_insn_addr[] = "";
const char c6_stage1_probe_combo_pmp_fault_insn_addr[] = "";
uintptr_t u_mode_probe_worker(uintptr_t, uintptr_t, uintptr_t, uintptr_t) { return 0; }
#endif

namespace shizuku::selftest {

namespace {

using ARCH = KERNEL::ARCH;
using BOARD = KERNEL::BOARD;

// 生成された named slot ID を使用 (OBJECT_COUNT 手動拡張は行わない)
constexpr uintptr_t OBJECT_STAGE1_PROBE = object_id::c6_stage1_probe;
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
  // ★純粋アセンブリにより実境界を証明。fallback は一切禁止 (fail-closed)。
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

  // U worker スタックのカナリア初期化 (M-mode handler スタック非露出の検証用)
  constexpr uint32_t U_STACK_CANARY = 0x5A5A5A5Au;
  auto *u_stack_words = reinterpret_cast<uint32_t *>(u_mode_stage1_stack);
  for (size_t i = 0; i < sizeof(u_mode_stage1_stack) / sizeof(uint32_t); ++i) {
    u_stack_words[i] = U_STACK_CANARY;
  }

  // U-readable な data pool にフォールト検証用 target pointer を格納 (legacy CALL_METHOD 1引数 ABI 維持)
  const uintptr_t kstack_addr = reinterpret_cast<uintptr_t>(shizuku_c6_kernel_stack) + 16u;
  volatile uint32_t caller_stack_sentinel = 0xC001CAFEu;
  const uintptr_t caller_sentinel_addr = reinterpret_cast<uintptr_t>(&caller_stack_sentinel);

  auto *data_pool_words = reinterpret_cast<volatile uintptr_t *>(u_mode_stage1_data_pool);
  data_pool_words[0] = kstack_addr;
  data_pool_words[1] = caller_sentinel_addr;

  // 1. オブジェクト登録: OBJECT_UNPRIVILEGED を指定して U-mode オブジェクトとして登録
  const auto create_res = ARCH::syscall(
      static_cast<uintptr_t>(object_api::CREATE_OBJECT),
      OBJECT_STAGE1_PROBE,
      reinterpret_cast<uintptr_t>(&u_mode_probe_worker),
      OBJECT_UNPRIVILEGED);
  const bool create_ok = (create_res.error == 0);
  record_check("stage1: create unprivileged probe object",
               create_ok, create_res.error, 0);

  // Precondition gate: オブジェクト生成に失敗した場合は後続の依存試験を即座に打切り
  if (!create_ok) {
    BOARD::diag_printf("[C6_STAGE1] ABORT: probe object creation failed (id=%lu, error=%lu); dependent tests aborted/unrun\n",
                       (unsigned long)OBJECT_STAGE1_PROBE, (unsigned long)create_res.error);
    return;
  }

  // 2. 0x802 U-mode 読み取り診断の実行
  // exact probe 命令 PC を登録
  g_c6_probe_0x802_pc = reinterpret_cast<uintptr_t>(&c6_stage1_probe_0x802_insn_addr);
  const auto diag_0x802 = ARCH::syscall(
      static_cast<uintptr_t>(object_api::CALL_METHOD),
      OBJECT_STAGE1_PROBE,
      METHOD_STAGE1_PROBE,
      1 /* cmd=1 */);

  // 0x802 の結果判定:
  // - Illegal instruction trap 捕捉時: トラップハンドラが shizuku_c6_u_mode_diag[0] = 2 を設定
  // - 読取成功時: 例外は発生せず、diag_0x802 にサイクル値が返る。M-mode 側で diag[0] = 1, diag[1] = value を記録
  if (shizuku_c6_u_mode_diag[0] == 0) {
    if (diag_0x802.error == 0 && diag_0x802.value != 0xFFFFFFFFu) {
      shizuku_c6_u_mode_diag[0] = 1; // 成功
      shizuku_c6_u_mode_diag[1] = diag_0x802.value;
    }
  }

  const uint32_t status_0x802 = shizuku_c6_u_mode_diag[0];
  const bool status_valid = (status_0x802 == 1u || status_0x802 == 2u);
  record_check("stage1: 0x802 U-mode read status is deterministic (1=ok, 2=illegal)",
               status_valid, status_0x802, status_0x802 ? status_0x802 : 2u);

  if (status_0x802 == 1u) {
    BOARD::diag_printf("[C6_STAGE1] 0x802 is readable in U-mode, cycle=%lu\n",
                       (unsigned long)shizuku_c6_u_mode_diag[1]);
    record_check("stage1: 0x802 read value returned non-error",
                 diag_0x802.error == 0 && diag_0x802.value != 0xFFFFFFFFu, 1, 1);
  } else if (status_0x802 == 2u) {
    BOARD::diag_printf("[C6_STAGE1] 0x802 read generated illegal-instruction trap in U-mode (captured safely)\n");
    record_check("stage1: illegal instruction trap confirmed (not disguised as success)",
                 diag_0x802.value == 0xFFFFFFFFu, 1, 1);
    record_check("stage1: 0x802 illegal instruction exact mcause is 2",
                 shizuku_c6_u_mode_diag[4] == 2u, shizuku_c6_u_mode_diag[4], 2u);
    record_check("stage1: 0x802 illegal instruction exact mepc is probe label",
                 shizuku_c6_u_mode_diag[5] == reinterpret_cast<uintptr_t>(&c6_stage1_probe_0x802_insn_addr),
                 shizuku_c6_u_mode_diag[5], reinterpret_cast<uintptr_t>(&c6_stage1_probe_0x802_insn_addr));
  } else {
    BOARD::diag_printf("[C6_STAGE1] FAIL 0x802 status unknown\n");
    record_check("stage1: 0x802 status must not be unknown", false, 0, 1);
  }

  // 3. PMP 境界隔離テスト: U-mode からカーネルスタックの読み取り試行
  // exact PMP fault 命令 PC を登録
  g_c6_probe_pmp_fault_pc = reinterpret_cast<uintptr_t>(&c6_stage1_probe_pmp_fault_insn_addr);
  const auto pmp_res = ARCH::syscall(
      static_cast<uintptr_t>(object_api::CALL_METHOD),
      OBJECT_STAGE1_PROBE,
      METHOD_STAGE1_PROBE,
      2 /* cmd=2 */);

  // PMP により Load Access Fault が発生し、trap handler で 0xE005 が返され、
  // 診断レコードに exact mcause (5), mepc, mtval (kstack_addr) が記録されたことを検証
  record_check("stage1: PMP denied U-mode reading kernel stack (fault trapped)",
               pmp_res.value == 0xE005u && shizuku_c6_u_mode_diag[3] > 0,
               shizuku_c6_u_mode_diag[3], 1);
  record_check("stage1: PMP fault exact mcause is 5 (Load Access Fault)",
               shizuku_c6_u_mode_diag[4] == 5u, shizuku_c6_u_mode_diag[4], 5u);
  record_check("stage1: PMP fault exact mepc is probe insn",
               shizuku_c6_u_mode_diag[5] == reinterpret_cast<uintptr_t>(&c6_stage1_probe_pmp_fault_insn_addr),
               shizuku_c6_u_mode_diag[5], reinterpret_cast<uintptr_t>(&c6_stage1_probe_pmp_fault_insn_addr));
  record_check("stage1: PMP fault exact mtval is target kstack address",
               shizuku_c6_u_mode_diag[6] == kstack_addr, shizuku_c6_u_mode_diag[6], kstack_addr);

  // 4. caller-stack 隔離テスト (重大指摘 C): callee U-mode から caller stack sentinel の読取試行
  g_c6_probe_caller_stack_fault_pc = reinterpret_cast<uintptr_t>(&c6_stage1_probe_caller_stack_fault_insn_addr);
  const auto caller_stack_res = ARCH::syscall(
      static_cast<uintptr_t>(object_api::CALL_METHOD),
      OBJECT_STAGE1_PROBE,
      METHOD_STAGE1_PROBE,
      5 /* cmd=5: read caller stack sentinel */);

  record_check("stage1: PMP denied U-mode reading caller stack sentinel (fault trapped)",
               caller_stack_res.value == 0xE006u && shizuku_c6_u_mode_diag[7] > 0,
               shizuku_c6_u_mode_diag[7], 1);
  record_check("stage1: caller stack fault exact mcause is 5 (Load Access Fault)",
               shizuku_c6_u_mode_diag[4] == 5u, shizuku_c6_u_mode_diag[4], 5u);
  record_check("stage1: caller stack fault exact mepc is probe insn",
               shizuku_c6_u_mode_diag[5] == reinterpret_cast<uintptr_t>(&c6_stage1_probe_caller_stack_fault_insn_addr),
               shizuku_c6_u_mode_diag[5], reinterpret_cast<uintptr_t>(&c6_stage1_probe_caller_stack_fault_insn_addr));
  record_check("stage1: caller stack fault exact mtval is caller sentinel address",
               shizuku_c6_u_mode_diag[6] == caller_sentinel_addr, shizuku_c6_u_mode_diag[6], caller_sentinel_addr);

  // 復帰後の caller stack の完全性検証: sentinel 値が改竄されていないこと
  record_check("stage1: caller stack sentinel intact after return",
               caller_stack_sentinel == 0xC001CAFEu, caller_stack_sentinel, 0xC001CAFEu);

  // 5. U-mode ecall 往復と信頼 stack 切り替えの検証 (U return trampoline 経由)
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
               shizuku_c6_trap_diag[1] == 8u, shizuku_c6_trap_diag[1], 8u);
  record_check("stage1: U-mode ecall trap mepc within object code bounds",
               shizuku_c6_trap_diag[2] >= g_c6_stage1_u_bounds.code_base &&
               shizuku_c6_trap_diag[2] < g_c6_stage1_u_bounds.code_limit, 1, 1);
  record_check("stage1: U-mode ecall trap exact mepc is probe ecall insn",
               shizuku_c6_trap_diag[2] == reinterpret_cast<uintptr_t>(&c6_stage1_probe_ecall_insn_addr),
               shizuku_c6_trap_diag[2], reinterpret_cast<uintptr_t>(&c6_stage1_probe_ecall_insn_addr));

  // 6. 非特権からの特権昇格・不正呼出しの拒否検証
  const auto unpriv_res = ARCH::syscall(
      static_cast<uintptr_t>(object_api::CALL_METHOD),
      OBJECT_STAGE1_PROBE,
      METHOD_STAGE1_PROBE,
      4 /* cmd=4 */);
  record_check("stage1: unprivileged primitive call was denied",
               unpriv_res.value != 0, unpriv_res.value, 1);

  // 7. Multi-trap call frame retention & safe return 実証 (重大ブロッカー G)
  volatile uintptr_t caller_sp_before = 0;
#if defined(__riscv)
  asm volatile("mv %0, sp" : "=r"(caller_sp_before));
#endif

  g_c6_probe_combo_0x802_pc = reinterpret_cast<uintptr_t>(&c6_stage1_probe_combo_0x802_insn_addr);
  g_c6_probe_combo_pmp_pc = reinterpret_cast<uintptr_t>(&c6_stage1_probe_combo_pmp_fault_insn_addr);

  // combo 実行前の PMP fault カウンタのスナップショットを取得
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

  // (1) 戻り値の検証: 4 回の trap を経て正しく計算された値 (0x5AFE0000 | probe_id) が戻ること
  record_check("stage1: multi-trap combo returned correct computed value",
               combo_res.error == 0 && combo_res.value == (0x5AFE0000u | OBJECT_STAGE1_PROBE),
               combo_res.value, 0x5AFE0000u | OBJECT_STAGE1_PROBE);

  // (2) caller SP の完全保持: 復帰後の SP が呼び出し前と完全に一致すること
  record_check("stage1: caller SP exactly retained across multi-trap invocation",
               caller_sp_after == caller_sp_before, caller_sp_after, caller_sp_before);

  // (3) privileged state の保持: 復帰後の特権モードが M-mode であること
  const bool is_privileged = ARCH::current_priv();
  record_check("stage1: caller privileged state maintained as M-mode after return",
               is_privileged, is_privileged ? 1 : 0, 1);

  // (4) combo 内の exact PMP fault 発生検証:
  // PMP fault カウンタ (u_mode_diag[3]) が呼出し前から厳密に +1 されていること
  record_check("stage1: multi-trap combo incremented PMP fault counter by 1",
               shizuku_c6_u_mode_diag[3] == combo_pmp_faults_before + 1u,
               shizuku_c6_u_mode_diag[3], combo_pmp_faults_before + 1u);

  // (5) combo 内の exact fault mcause は 5 (Load Access Fault) であること
  record_check("stage1: multi-trap combo exact fault mcause is 5",
               shizuku_c6_u_mode_diag[4] == 5u, shizuku_c6_u_mode_diag[4], 5u);

  // (6) combo 内の exact fault mepc は combo PMP fault 命令ラベルであること
  record_check("stage1: multi-trap combo exact fault mepc is combo probe insn",
               shizuku_c6_u_mode_diag[5] == reinterpret_cast<uintptr_t>(&c6_stage1_probe_combo_pmp_fault_insn_addr),
               shizuku_c6_u_mode_diag[5], reinterpret_cast<uintptr_t>(&c6_stage1_probe_combo_pmp_fault_insn_addr));

  // (7) combo 内の exact fault mtval はターゲット kstack_addr であること
  record_check("stage1: multi-trap combo exact fault mtval is kstack address",
               shizuku_c6_u_mode_diag[6] == kstack_addr, shizuku_c6_u_mode_diag[6], kstack_addr);

  // 8. M-mode native handler スタック非露出検証 (U-mode スタックのカナリア保護)
  bool canary_intact = true;
  for (size_t i = 0; i < sizeof(u_mode_stage1_stack) / sizeof(uint32_t); ++i) {
    if (u_stack_words[i] != U_STACK_CANARY) {
      canary_intact = false;
      break;
    }
  }
  record_check("stage1: privileged M handler stack did not spill into U stack canary (full stack)",
               canary_intact, canary_intact ? 1 : 0, 1);

  // 9. 本番判定述語によるオブジェクト ID 束縛・拒否の直接検証 (重大指摘 B 補強)
  const bool auth_probe = shizuku_c6_is_authorized_u_object(OBJECT_STAGE1_PROBE, OBJECT_STAGE1_PROBE);
  const bool reject_foreign_owner = !shizuku_c6_is_authorized_u_object(0x62u, OBJECT_STAGE1_PROBE);
  const bool reject_foreign_cur = !shizuku_c6_is_authorized_u_object(OBJECT_STAGE1_PROBE, 0x62u);
  const bool reject_zero = !shizuku_c6_is_authorized_u_object(0u, 0u);
  record_check("stage1: production predicate accepts authorized probe", auth_probe, auth_probe ? 1 : 0, 1);
  record_check("stage1: production predicate rejects foreign owner ID", reject_foreign_owner, reject_foreign_owner ? 1 : 0, 1);
  record_check("stage1: production predicate rejects foreign current_object", reject_foreign_cur, reject_foreign_cur ? 1 : 0, 1);
  record_check("stage1: production predicate rejects uninitialized 0x0", reject_zero, reject_zero ? 1 : 0, 1);

  // 10. 本番スタック切り替え述語による U-mode スタック保護の直接検証
  // 有効な 16 バイト整列フレーム (3600: 3600 % 16 == 0, 3600 + 144 = 3744 <= 4096, 3600 >= 256) を使用
  const uintptr_t test_u_sp = g_c6_stage1_u_bounds.stack_base + 512u;
  const uintptr_t kstack_valid_frame = reinterpret_cast<uintptr_t>(shizuku_c6_kernel_stack) + 3600u;
  const uintptr_t routed_m_sp = shizuku_c6_get_m_handler_stack(test_u_sp, kstack_valid_frame);
  const uintptr_t expected_snapshot = (kstack_valid_frame + 144u);
  const bool routed_inside_kstack = (routed_m_sp >= reinterpret_cast<uintptr_t>(shizuku_c6_kernel_stack) &&
                                     routed_m_sp <= reinterpret_cast<uintptr_t>(shizuku_c6_kernel_stack + sizeof(shizuku_c6_kernel_stack)));
  const bool routed_outside_u_stack = (routed_m_sp < g_c6_stage1_u_bounds.stack_base ||
                                       routed_m_sp > g_c6_stage1_u_bounds.stack_limit);
  const bool routed_is_snapshot = (routed_m_sp == expected_snapshot);
  record_check("stage1: M handler stack routed inside trusted kernel stack as valid aligned snapshot",
               routed_inside_kstack && routed_is_snapshot, routed_m_sp, expected_snapshot);
  record_check("stage1: M handler stack routed strictly outside U stack",
               routed_outside_u_stack, routed_outside_u_stack ? 1 : 0, 1);

  // 別アサーション: 非整列 frame (kstack + 3604: (3604+144)%16 != 0) は fail-closed で 0 拒否されること
  const uintptr_t kstack_unaligned_frame = kstack_valid_frame + 4u;
  const uintptr_t rejected_unaligned_sp = shizuku_c6_get_m_handler_stack(test_u_sp, kstack_unaligned_frame);
  record_check("stage1: M handler stack rejects unaligned frame (fail-closed 0)",
               rejected_unaligned_sp == 0, rejected_unaligned_sp, 0);

  // 別アサーション: ガード領域割れ (headroom 不足) frame は fail-closed で 0 拒否されること
  const uintptr_t kstack_underflow_frame = reinterpret_cast<uintptr_t>(shizuku_c6_kernel_stack) + 128u;
  const uintptr_t rejected_underflow_sp = shizuku_c6_get_m_handler_stack(test_u_sp, kstack_underflow_frame);
  record_check("stage1: M handler stack rejects frame below guard headroom (fail-closed 0)",
               rejected_underflow_sp == 0, rejected_underflow_sp, 0);

  // 11. C ディスパッチスタック (dispatch stack) の完全非重複性・隔離検証
  const uintptr_t dstack_start = reinterpret_cast<uintptr_t>(shizuku_c6_dispatch_stack);
  const uintptr_t dstack_limit = dstack_start + sizeof(shizuku_c6_dispatch_stack);
  const uintptr_t kstack_start = reinterpret_cast<uintptr_t>(shizuku_c6_kernel_stack);
  const uintptr_t kstack_limit = kstack_start + sizeof(shizuku_c6_kernel_stack);

  using pmp = shizuku::chips::esp32_c6::pmp_controller;
  const bool dstack_disjoint_kstack = !pmp::ranges_overlap(dstack_start, dstack_limit, kstack_start, kstack_limit);
  const bool dstack_disjoint_ustack = !pmp::ranges_overlap(dstack_start, dstack_limit, g_c6_stage1_u_bounds.stack_base, g_c6_stage1_u_bounds.stack_limit);
  const bool dstack_is_recognized = shizuku_c6_is_dispatch_stack_address(dstack_start + 16u) &&
                                    !shizuku_c6_is_dispatch_stack_address(test_u_sp);
  record_check("stage1: dispatch stack strictly disjoint from kernel stack and U stack",
               dstack_disjoint_kstack && dstack_disjoint_ustack && dstack_is_recognized, 1, 1);

  BOARD::diag_printf("[C6_STAGE1] stage 1 verification complete: passed=%lu failed=%lu\n",
                     (unsigned long)passed, (unsigned long)failed);
}

} // namespace shizuku::selftest
