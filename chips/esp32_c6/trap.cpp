#include "shizuku/kernel.hpp"
#include "shizuku/kernel_object.hpp"
#include "shizuku/native_call.hpp"
#include "arch/riscv/ecall_table.hpp"
#include "arch/riscv/trap_route.hpp"
#include "shizuku/svc_attributes.hpp"
#include "chips/esp32_c6/systimer.hpp"
#include "chips/esp32_c6/trap_api.hpp"
#include "chips/esp32_c6/stop_capture.hpp"
#include "chips/esp32_c6/pmp_c6.hpp"

#include <array>
#include <cstddef>

using arch = shizuku::ARCH;
namespace stop_capture = shizuku::chips::esp32_c6::stop_capture;
static_assert((sizeof(shizuku::KERNEL::call_frame_header) + RV32_FRAME_BYTES) % 16 == 0,
              "V2 call-frame geometry must preserve the RV32 16-byte stack ABI");
extern "C" {
alignas(16) unsigned char shizuku_c6_kernel_stack[4096];
volatile uint32_t shizuku_c6_irq_depth;
volatile uint32_t shizuku_c6_trap_depth;
// status, cause, PC, mtval, ECALL count; JTAG-readable even on fatal traps.
volatile uint32_t shizuku_c6_trap_diag[5];
// Durable failure snapshot. It is invalidated at the next explicit boot marker
// and committed by writing magic last.
__attribute__((used, aligned(16), section(".noinit")))
volatile stop_capture::capture_record shizuku_c6_stop_capture;
// [0..7] magic/version/probe_count/first_bad_task/address/expected/actual/flags;
// then nine six-word records {task_id, stack_base, stack_top, entry_sp, checks, flags}.
__attribute__((used, aligned(16), section(".noinit")))
volatile uint32_t shizuku_c6_stack_diag[8u + 9u * 6u];
__attribute__((used, aligned(4), section(".noinit")))
volatile uint32_t shizuku_c6_boot_marker;
__attribute__((used, aligned(4), section(".noinit")))
volatile uint32_t shizuku_c6_boot_generation;
__attribute__((used, aligned(4), section(".noinit")))
volatile uint32_t shizuku_c6_boot_marker_magic;
// SYSTIMER IRQ count, max IRQ depth, pend/switch count, last oneshot ticks,
// last IRQ mcause, last mie readback during init/IRQ setup, pend_context_switch calls.
volatile uint32_t shizuku_c6_timer_diag[7];
// 段1 U-mode / PMP 診断レコード:
// [0]=0x802 状態 (0:未実行, 1:成功, 2:Illegal Instruction trap),
// [1]=0x802 読取サイクル値 (成功時),
// [2]=U-mode ecall 通過数,
// [3]=PMP 違反検出数 (kernel stack 読取試行時の fault 捕捉),
// [4]=最後の fault mcause, [5]=最後の fault mepc, [6]=最後の fault mtval,
// [7]=Caller stack PMP 違反検出数 (caller stack 読取試行時の fault 捕捉),
// [8]=kernel stack store 違反数, [9]=caller stack store 違反数, [10]=U code store 違反数,
// [11]=U stack instruction fetch 違反数, [12]=pmpcfg0 (0x3A0) illegal instruction 捕捉数,
// [13]=App MMIO load 違反数, [14]=App -> Driver data load 違反数, [15]=Driver -> App data load 違反数
volatile uint32_t shizuku_c6_u_mode_diag[16];
// 段1 U-mode ecall 記録: 最初の U-mode ecall (mcause 8) の [0]=mcause, [1]=mepc, [2]=mtval。
volatile uint32_t shizuku_c6_u_ecall_diag[3];
#if defined(SHIZUKU_C6_UMODE)
// app の C wrapper (components/shizuku_c6_region) が PMA 0-15 を読み直した後に書く準備完了マーカー (0 = 未設定)。
extern volatile uint32_t shizuku_c6_stage1_pma_ready;
#endif
volatile uintptr_t g_c6_probe_0x802_pc;
volatile uintptr_t g_c6_probe_pmp_fault_pc;
volatile uintptr_t g_c6_probe_caller_stack_fault_pc;
volatile uintptr_t g_c6_probe_combo_0x802_pc;
volatile uintptr_t g_c6_probe_combo_pmp_pc;
#if defined(SHIZUKU_C6_UMODE)
volatile uintptr_t g_c6_probe_ustore_kstack_pc;
volatile uintptr_t g_c6_probe_ustore_caller_pc;
volatile uintptr_t g_c6_probe_ustore_code_pc;
volatile uintptr_t g_c6_probe_ufetch_target;
volatile uintptr_t g_c6_probe_upmpcfg_pc;
volatile uintptr_t g_c6_probe_mstore_iram_pc;
volatile uintptr_t g_c6_probe_mfetch_target;
volatile uintptr_t g_c6_probe_app_mmio_fault_pc;
volatile uintptr_t g_c6_probe_app_driver_data_fault_pc;
volatile uintptr_t g_c6_probe_driver_app_data_fault_pc;
volatile uint32_t shizuku_c6_m_pma_diag[2];
#endif
using c6_stage1_object_bounds = shizuku::chips::esp32_c6::c6_stage1_object_bounds;
c6_stage1_object_bounds g_c6_stage1_u_bounds;
c6_stage1_object_bounds g_c6_stage1_driver_bounds;

bool shizuku_c6_is_authorized_u_object(uint32_t owner_id, uint32_t current_obj) {
  if (g_c6_stage1_u_bounds.owner_id != 0 &&
      shizuku::chips::esp32_c6::is_authorized_u_object(
          g_c6_stage1_u_bounds.owner_id, owner_id, current_obj)) {
    return true;
  }
  if (g_c6_stage1_driver_bounds.owner_id != 0 &&
      shizuku::chips::esp32_c6::is_authorized_u_object(
          g_c6_stage1_driver_bounds.owner_id, owner_id, current_obj)) {
    return true;
  }
  return false;
}
bool shizuku_c6_is_kstack_address(uintptr_t addr) {
  return shizuku::chips::esp32_c6::is_kstack_range(
      addr, reinterpret_cast<uintptr_t>(shizuku_c6_kernel_stack), sizeof(shizuku_c6_kernel_stack));
}
uintptr_t shizuku_c6_kstack_limit(void) {
  return shizuku::chips::esp32_c6::compute_kstack_limit(
      reinterpret_cast<uintptr_t>(shizuku_c6_kernel_stack), shizuku::chips::esp32_c6::KSTACK_GUARD_BYTES);
}
uintptr_t shizuku_c6_get_m_handler_stack(uintptr_t current_sp, uintptr_t frame_sp) {
  uintptr_t s_base = g_c6_stage1_u_bounds.stack_base;
  uintptr_t s_limit = g_c6_stage1_u_bounds.stack_limit;
  if (g_c6_stage1_driver_bounds.owner_id != 0 &&
      current_sp >= g_c6_stage1_driver_bounds.stack_base &&
      current_sp <= g_c6_stage1_driver_bounds.stack_limit) {
    s_base = g_c6_stage1_driver_bounds.stack_base;
    s_limit = g_c6_stage1_driver_bounds.stack_limit;
  }
  return shizuku::chips::esp32_c6::get_m_handler_stack(
      current_sp,
      s_base,
      s_limit,
      frame_sp,
      reinterpret_cast<uintptr_t>(shizuku_c6_kernel_stack),
      sizeof(shizuku_c6_kernel_stack),
      shizuku::chips::esp32_c6::KSTACK_GUARD_BYTES);
}
bool shizuku_c6_handle_fault(arch::exception_frame_t *frame);
// OpenOCD rows: source, cpu_irq, handler, arg, enabled, dispatch_count, flags,
// INTMTX readback, PLIC enable/type bits, priority, threshold, mie bit, pending,
// last mstatus.MPIE observed in this ISR (UINT32_MAX means never dispatched).
struct c6_irq_registration {
  uint32_t source;
  uint32_t cpu_irq;
  shizuku::chips::esp32_c6::irq_handler_t handler;
  void *arg;
  bool enabled;
  volatile uint32_t dispatch_count;
  uint32_t flags;
  uint32_t intmtx_readback;
  uint32_t plic_enable_bit;
  uint32_t plic_type_bit;
  uint32_t plic_priority;
  uint32_t plic_threshold;
  uint32_t mie_bit;
  uint32_t emip_pending_bit;
  uint32_t last_mstatus_mpie;
};
static_assert(sizeof(c6_irq_registration) == 15 * sizeof(uint32_t),
              "OpenOCD C6 IRQ row is fifteen words");
c6_irq_registration shizuku_c6_irq_registrations[24];
void shizuku_c6_vectors();
extern char _bss_end;
}
namespace {
uint32_t active_mstatus;
volatile arch::exception_frame_t *active_frame;
volatile arch::context_t *active_context;
volatile uint32_t active_stop_phase;
volatile uint32_t stop_capture_busy;
volatile uint32_t pend_flag;
uint64_t armed_target;
uint32_t trap_probe_interval_count;
// 固定 PMP エントリ (0-3: 共有コード・共有窓) に今入っている値。M へ戻るときの pmp_controller::disable() が
// 消すので、そこと起動時に 0 へ戻し、U へ入るときに窓が変わっていれば張り直す。
// fixed_*: 固定 PMP エントリ (0-3: 共有コード・共有窓) に今入っている値。M へ戻るときの
// pmp_controller::disable() が消すので、そこと起動時に 0 へ戻し、U へ入るときに窓が変わっていれば張り直す。
// bounds / epoch: 固有 PMP エントリ (4-9) に今入っている object の bounds。固定エントリと同じく、disable() の後と起動時に
// nullptr へ戻す。同じ object で U へ戻るとき (切替えの無い ECALL など) は 8 本と fence を書かない。
// bounds の precomputed が作り直されたとき (pmp_epoch が進む) も不一致になって書き直す。
// 6 欄は U へ入るたびに全部読むので、1 つの構造体に置いて 1 つの基底レジスタで引く
// (別々の変数だと欄ごとに lui を作り直す)。
struct c6_pmp_shadow {
  uintptr_t fixed_code_base;
  uintptr_t fixed_code_limit;
  uintptr_t fixed_region_base;
  uintptr_t fixed_region_limit;
  const void *bounds;
  uint32_t epoch;
};
c6_pmp_shadow s_pmp_shadow = {};
namespace c6 = shizuku::ports::c6;
constexpr stop_capture::ram_range kC6Dram{0x40800000u, 0x40860000u};
constexpr uint32_t kBootMarkerMagic = 0x424D3643u; // "C6MB"
constexpr uint32_t kStackDiagMagic = 0x43533643u; // "C6SC"
constexpr uint32_t kStackDiagVersion = 1u;
constexpr uint32_t kKernelStackTaskId = 0u;
constexpr uint32_t kUnassignedStackTaskId = UINT32_MAX;
constexpr size_t kStackDiagHeaderWords = 8u;
constexpr size_t kStackDiagRecordWords = 6u;
constexpr size_t kStackDiagRecordCount = 9u;
constexpr uint32_t kStackCanary[4] = {
    0xC6A55A3Cu, 0x51ACCAFEu, 0x0F3CC3F0u, 0xA55AC66Au};
constexpr uint32_t kStackProbeInterval = 1024u;
static_assert(sizeof(shizuku_c6_kernel_stack) == 4096u,
              "C6 trap stack must remain 4096 bytes");
shizuku::chips::esp32_c6::irq_dispatch_hooks g_irq_dispatch_hooks{nullptr, nullptr};
size_t stack_record_offset(size_t slot) {
  return kStackDiagHeaderWords + slot * kStackDiagRecordWords;
}
void stack_diag_init() {
  shizuku_c6_stack_diag[0] = 0u;
  shizuku_c6_stack_diag[1] = kStackDiagVersion;
  for (size_t i = 2; i < 8; ++i) shizuku_c6_stack_diag[i] = 0u;
  for (size_t slot = 0; slot < kStackDiagRecordCount; ++slot) {
    const size_t base = stack_record_offset(slot);
    shizuku_c6_stack_diag[base] = kUnassignedStackTaskId;
    for (size_t i = 1; i < kStackDiagRecordWords; ++i)
      shizuku_c6_stack_diag[base + i] = 0u;
  }
  shizuku_c6_stack_diag[0] = kStackDiagMagic;
}
void record_stack_fault(uint32_t task_id, uintptr_t address,
                        uint32_t expected, uint32_t actual, uint32_t flags) {
  if (shizuku_c6_stack_diag[7] != 0u) return;
  shizuku_c6_stack_diag[3] = task_id;
  shizuku_c6_stack_diag[4] = static_cast<uint32_t>(address);
  shizuku_c6_stack_diag[5] = expected;
  shizuku_c6_stack_diag[6] = actual;
  shizuku_c6_stack_diag[7] = flags;
}
void write_stack_canary(uint32_t task_id, uintptr_t stack_base,
                        uintptr_t stack_top, uintptr_t entry_sp) {
  if (!stop_capture::range_contains(kC6Dram, stack_base, 4096u, 8u) ||
      stack_top != stack_base + 4096u || entry_sp < stack_base + 16u ||
      entry_sp > stack_top || entry_sp % 16u != 0u) {
    record_stack_fault(task_id, stack_base, 4096u,
                       static_cast<uint32_t>(entry_sp), 2u);
    return;
  }
  size_t slot = kStackDiagRecordCount;
  for (size_t i = 1; i < kStackDiagRecordCount; ++i) {
    const size_t base = stack_record_offset(i);
    if (shizuku_c6_stack_diag[base] == task_id) {
      slot = i;
      break;
    }
    if (slot == kStackDiagRecordCount &&
        shizuku_c6_stack_diag[base] == kUnassignedStackTaskId)
      slot = i;
  }
  if (slot == kStackDiagRecordCount) {
    record_stack_fault(task_id, stack_base, kStackDiagRecordCount - 1u,
                       kUnassignedStackTaskId, 3u);
    return;
  }
  const size_t record = stack_record_offset(slot);
  shizuku_c6_stack_diag[record] = task_id;
  shizuku_c6_stack_diag[record + 1u] = static_cast<uint32_t>(stack_base);
  shizuku_c6_stack_diag[record + 2u] = static_cast<uint32_t>(stack_top);
  shizuku_c6_stack_diag[record + 3u] = static_cast<uint32_t>(entry_sp);
  volatile uint32_t *guard = reinterpret_cast<volatile uint32_t *>(stack_base);
  for (size_t i = 0; i < 4; ++i) guard[i] = kStackCanary[i];
  shizuku_c6_stack_diag[record + 4u] = 0u;
  shizuku_c6_stack_diag[record + 5u] = 1u;
}
void check_stack_canary(size_t slot) {
  const size_t record = stack_record_offset(slot);
  const uint32_t task_id = shizuku_c6_stack_diag[record];
  const uintptr_t stack_base = shizuku_c6_stack_diag[record + 1u];
  const uintptr_t stack_top = shizuku_c6_stack_diag[record + 2u];
  if (task_id == kUnassignedStackTaskId ||
      shizuku_c6_stack_diag[record + 5u] != 1u) return;
  if (stack_base > UINTPTR_MAX - 4096u || stack_top != stack_base + 4096u ||
      !stop_capture::range_contains(kC6Dram, stack_base, 4096u, 8u)) {
    record_stack_fault(task_id, stack_base, 4096u,
                       static_cast<uint32_t>(stack_top), 2u);
    return;
  }
  volatile uint32_t *guard = reinterpret_cast<volatile uint32_t *>(stack_base);
  shizuku_c6_stack_diag[record + 4u] += 1u;
  shizuku_c6_stack_diag[2] += 1u;
  for (size_t i = 0; i < 4; ++i) {
    const uint32_t actual = guard[i];
    if (actual != kStackCanary[i])
      record_stack_fault(task_id, stack_base + i * sizeof(uint32_t),
                         kStackCanary[i], actual, 1u);
  }
}
void initialize_kernel_stack_canary() {
  const uintptr_t stack_base = reinterpret_cast<uintptr_t>(shizuku_c6_kernel_stack);
  const uintptr_t stack_top = stack_base + sizeof(shizuku_c6_kernel_stack);
  if (!stop_capture::range_contains(kC6Dram, stack_base,
                                   sizeof(shizuku_c6_kernel_stack), 16u)) {
    record_stack_fault(kKernelStackTaskId, stack_base,
                       sizeof(shizuku_c6_kernel_stack), 0u, 2u);
    return;
  }
  const size_t record = stack_record_offset(0);
  shizuku_c6_stack_diag[record] = kKernelStackTaskId;
  shizuku_c6_stack_diag[record + 1u] = static_cast<uint32_t>(stack_base);
  shizuku_c6_stack_diag[record + 2u] = static_cast<uint32_t>(stack_top);
  shizuku_c6_stack_diag[record + 3u] = static_cast<uint32_t>(stack_top);
  shizuku_c6_stack_diag[record + 4u] = 0u;
  volatile uint32_t *guard =
      reinterpret_cast<volatile uint32_t *>(stack_base);
  for (size_t i = 0; i < 4; ++i) guard[i] = kStackCanary[i];
  shizuku_c6_stack_diag[record + 5u] = 1u;
}
void capture_stop_state(uint32_t status, uint32_t caller_pc) {
  if (stop_capture_busy != 0u) return;
  stop_capture_busy = 1u;
  auto *frame = const_cast<arch::exception_frame_t *>(active_frame);
  auto *context = const_cast<arch::context_t *>(active_context);
  uint32_t cause = 0u, pc = 0u, tval = 0u, mstatus = 0u;
  uint32_t sp = 0u, ra = 0u;
  asm volatile("csrr %0, mcause" : "=r"(cause));
  asm volatile("csrr %0, mepc" : "=r"(pc));
  asm volatile("csrr %0, mtval" : "=r"(tval));
  asm volatile("csrr %0, mstatus" : "=r"(mstatus));
  asm volatile("mv %0, sp" : "=r"(sp));
  asm volatile("mv %0, ra" : "=r"(ra));
  if (shizuku_c6_stack_diag[0] == kStackDiagMagic &&
      shizuku_c6_stack_diag[1] == kStackDiagVersion) {
    for (size_t slot = 0; slot < kStackDiagRecordCount; ++slot)
      check_stack_canary(slot);
  }
  if (!context && shizuku_c6_boot_marker_magic == kBootMarkerMagic &&
      shizuku_c6_boot_marker >= 0xC602u) {
    auto *candidate = shizuku::kernel_instance.current_context();
    if (stop_capture::range_contains(
            kC6Dram, reinterpret_cast<uintptr_t>(candidate),
            sizeof(arch::context_t), alignof(uint32_t)))
      context = candidate;
  }
  if (!frame && context &&
      stop_capture::range_contains(kC6Dram,
          reinterpret_cast<uintptr_t>(context), sizeof(arch::context_t),
          alignof(uint32_t)))
    frame = context->sp;
  const uintptr_t frame_address = reinterpret_cast<uintptr_t>(frame);
  const uintptr_t context_address = reinterpret_cast<uintptr_t>(context);
  const bool boot_marker_valid =
      (shizuku_c6_boot_marker_magic == kBootMarkerMagic);
  const stop_capture::capture_input input{
      status, active_stop_phase, caller_pc,
      frame_address, context_address, cause, pc, tval, mstatus, sp, ra,
      shizuku_c6_trap_depth, shizuku_c6_irq_depth,
      boot_marker_valid ? shizuku_c6_boot_marker : 0u,
      boot_marker_valid ? shizuku_c6_boot_generation : 0u};
  stop_capture::capture<arch::context_t>(shizuku_c6_stop_capture, input,
                                        kC6Dram, kC6Dram);
}
inline void reg_write(uint32_t address, uint32_t value) {
  *reinterpret_cast<volatile uint32_t *>(address) = value;
}
inline uint32_t reg_read(uint32_t address) {
  return *reinterpret_cast<volatile uint32_t *>(address);
}
// CLEAR holds the bits written to it, so a set bit keeps its line clear; pulse and drop it.
void plic_pulse_clear(uint32_t bit) {
  reg_write(c6::PLIC_MXINT_CLEAR, reg_read(c6::PLIC_MXINT_CLEAR) | bit);
  reg_write(c6::PLIC_MXINT_CLEAR, reg_read(c6::PLIC_MXINT_CLEAR) & ~bit);
}
uint64_t systimer_now() {
  reg_write(c6::SYSTIMER_UNIT0_OP, c6::UNIT0_OP_UPDATE);
  while (!(reg_read(c6::SYSTIMER_UNIT0_OP) & c6::UNIT0_OP_VALUE_VALID)) {}
  const uint32_t high = reg_read(c6::SYSTIMER_UNIT0_VALUE_HI);
  const uint32_t low = reg_read(c6::SYSTIMER_UNIT0_VALUE_LO);
  return (static_cast<uint64_t>(high) << 32) | low;
}
void systimer_disarm() {
  reg_write(c6::SYSTIMER_CONF, reg_read(c6::SYSTIMER_CONF) & ~c6::CONF_TARGET0_WORK_EN);
  reg_write(c6::SYSTIMER_INT_ENA, reg_read(c6::SYSTIMER_INT_ENA) & ~c6::TARGET0_INT_BIT);
  reg_write(c6::SYSTIMER_INT_CLR, c6::TARGET0_INT_BIT);
}
int irq_find(uint32_t source, uint32_t cpu_irq) {
  for (size_t i = 0; i < 24; ++i) {
    const auto &entry = shizuku_c6_irq_registrations[i];
    if (entry.handler && entry.source == source && entry.cpu_irq == cpu_irq)
      return static_cast<int>(i);
  }
  return -1;
}
int irq_find_cpu(uint32_t cpu_irq) {
  for (size_t i = 0; i < 24; ++i) {
    const auto &entry = shizuku_c6_irq_registrations[i];
    if (entry.handler && entry.cpu_irq == cpu_irq) return static_cast<int>(i);
  }
  return -1;
}
int irq_find_source(uint32_t source) {
  for (size_t i = 0; i < 24; ++i) {
    const auto &entry = shizuku_c6_irq_registrations[i];
    if (entry.handler && entry.source == source) return static_cast<int>(i);
  }
  return -1;
}
void record_irq_hardware_state(c6_irq_registration &entry) {
  const uint32_t bit = 1u << entry.cpu_irq;
  uint32_t mie = 0;
  asm volatile("csrr %0, mie" : "=r"(mie));
  entry.intmtx_readback = reg_read(c6::INTMTX_SOURCE_MAP_REG(entry.source));
  entry.plic_enable_bit = (reg_read(c6::PLIC_MXINT_ENABLE) & bit) != 0;
  entry.plic_type_bit = (reg_read(c6::PLIC_MXINT_TYPE) & bit) != 0;
  entry.plic_priority = reg_read(c6::plic_pri_reg(entry.cpu_irq));
  entry.plic_threshold = reg_read(c6::PLIC_MXINT_THRESH);
  entry.mie_bit = (mie & bit) != 0;
  entry.emip_pending_bit = (reg_read(c6::PLIC_MXINT_PENDING) & bit) != 0;
  shizuku_c6_timer_diag[5] = mie;
}
// MIE stays clear here; threads enable it through MPIE on mret.
void interrupt_init() {
  reg_write(c6::SYSTIMER_CONF, reg_read(c6::SYSTIMER_CONF) | c6::CONF_CLK_EN | c6::CONF_UNIT0_WORK_EN);
  systimer_disarm();
  reg_write(c6::SYSTIMER_TARGET0_CONF, 0);  // one-shot, UNIT0
  reg_write(c6::INTMTX_SYSTIMER_TARGET0_MAP, c6::TIMER_CPU_INT);
  reg_write(c6::PLIC_MXINT_TYPE, 0);  // level
  reg_write(c6::plic_pri_reg(c6::TIMER_CPU_INT), c6::TIMER_PRIORITY);
  reg_write(c6::PLIC_MXINT_THRESH, c6::PLIC_THRESHOLD);
  reg_write(c6::PLIC_MXINT_ENABLE, 1u << c6::TIMER_CPU_INT);
  asm volatile("csrs mie, %0" :: "r"(1u << c6::TIMER_CPU_INT) : "memory");
  uint32_t mie;
  asm volatile("csrr %0, mie" : "=r"(mie));
  shizuku_c6_timer_diag[5] = mie;
}
__attribute__((noinline)) [[noreturn]] void stop(uint32_t status) {
  asm volatile("csrci mstatus, 8\n fence rw,rw" ::: "memory");
  if (active_stop_phase == 0u) active_stop_phase = status;
  const uint32_t caller_pc = static_cast<uint32_t>(
      reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
  capture_stop_state(status, caller_pc);
  shizuku_c6_trap_diag[0] = status;
  for (;;) asm volatile("nop" ::: "memory");
}
void install_traps() {
  asm volatile("csrci mstatus, 8" ::: "memory");
  if (shizuku_c6_stack_diag[0] != kStackDiagMagic ||
      shizuku_c6_stack_diag[1] != kStackDiagVersion)
    stack_diag_init();
  initialize_kernel_stack_canary();
  const auto vector = reinterpret_cast<uintptr_t>(&shizuku_c6_vectors) | 1u;
  asm volatile("csrw mscratch, zero\n csrw mtvec, %0" :: "r"(vector) : "memory");
}
// frame が s0-s11 などの本物の保持者 (文脈の s* は使わない)。ECALL のフレームは s* を保存しない
// (caller が自分で退避する ABI) ので、フレームと文脈の間でコピーするものは無い。
SHIZUKU_HOT void frame_to_context(arch::exception_frame_t &f, arch::context_t &c) {
  c.sp = &f;
  arch::set_context_privilege_from_frame(c, f);
}
#if defined(SHIZUKU_C6_UMODE)
// bounds だけで決まる U 復帰前の検査 (fail-closed)。context_to_frame が版ごとに 1 回呼ぶ。
__attribute__((noinline)) static bool bounds_static_ok(const c6_stage1_object_bounds &b) {
  namespace pmp = shizuku::chips::esp32_c6;
  // App オブジェクトは MMIO 窓を持つことができない
  if (b.role == pmp::c6_stage1_role::App && b.mmio_limit != 0) return false;
  if (b.code_base == 0 || b.code_limit <= b.code_base) return false;
  // U return trampoline がオブジェクト所有 code 範囲内にあること
  if (b.u_return_trampoline < b.code_base || b.u_return_trampoline >= b.code_limit) return false;
  // スタック境界
  if (b.stack_base == 0 || b.stack_limit <= b.stack_base) return false;
  const uintptr_t mk_lo = reinterpret_cast<uintptr_t>(&shizuku_c6_stage1_pma_ready);
  const uintptr_t mk_hi = mk_lo + sizeof(shizuku_c6_stage1_pma_ready);
  if (pmp::pmp_controller::ranges_overlap(b.code_base, b.code_limit, mk_lo, mk_hi) ||
      pmp::pmp_controller::ranges_overlap(b.stack_base, b.stack_limit, mk_lo, mk_hi) ||
      (b.data_limit != 0 && pmp::pmp_controller::ranges_overlap(b.data_base, b.data_limit, mk_lo, mk_hi)) ||
      (b.mmio_limit != 0 && pmp::pmp_controller::ranges_overlap(b.mmio_base, b.mmio_limit, mk_lo, mk_hi))) {
    return false;
  }
  return true;
}
#endif

// M-mode への復帰。PMP の全読出し (disable) が大きな枠を要るので、U へ入る側 (U→U の往復で
// 2 回通る) の枠・退避を太らせないよう別関数にしてある。
__attribute__((noinline)) static void context_to_frame_to_m(arch::context_t &c, arch::exception_frame_t &f) {
  f.reserved1 = 0;
#if defined(SHIZUKU_C6_UMODE)
  if (!shizuku::chips::esp32_c6::pmp_controller::disable()) {
    active_context = &c;
    active_frame = c.sp;
    active_stop_phase = 0xE004u;
    stop(0xE004);
  }
  // disable() が固定エントリも消したので、次に U へ入るとき張り直させる。
  s_pmp_shadow = {};
  const uintptr_t current_sp = f.reserved0 != 0
      ? static_cast<uintptr_t>(f.reserved0)
      : reinterpret_cast<uintptr_t>(c.sp) + RV32_FRAME_BYTES;
  const uintptr_t new_sp = shizuku_c6_get_m_handler_stack(current_sp, reinterpret_cast<uintptr_t>(c.sp));
  if (new_sp == 0) {
    active_context = &c;
    active_frame = c.sp;
    active_stop_phase = 0xE004u;
    stop(0xE004);
  }
  f.reserved0 = new_sp;
#else
  if (f.reserved0 == 0) {
    f.reserved0 = reinterpret_cast<uintptr_t>(c.sp) + RV32_FRAME_BYTES;
  }
#endif
}

SHIZUKU_HOT void context_to_frame(arch::context_t &c) {
  auto &f = *c.sp;
  // Threads always resume with interrupts enabled; only the kernel runs with MIE=0.
  f.mstatus = ((f.mstatus & ~RV32_MSTATUS_MPP) | (c.privileged ? RV32_MSTATUS_MPP : 0)) | RV32_MSTATUS_MPIE;
  // 段1: U-mode 復帰時は PMP allow-list を適用し、カーネルスタック/復帰情報を隔離。
  if (!c.privileged) {
#if !defined(SHIZUKU_C6_UMODE)
    active_context = &c;
    active_frame = c.sp;
    active_stop_phase = 0xE004u;
    stop(0xE004);
#else
    auto &kernel = shizuku::kernel_instance;
    // PMA 規則 (app の wrapper が設定) が有効であることを要求する。
    if (!shizuku::chips::esp32_c6::pma_c6::ready_marker_ok(shizuku_c6_stage1_pma_ready)) {
      active_context = &c;
      active_frame = c.sp;
      active_stop_phase = 0xE004u;
      stop(0xE004); // PMA ready marker 欠落 → fail-closed
    }
    const uint32_t cur_obj = kernel.current_thread().current_object();
    c6_stage1_object_bounds *bounds = nullptr;
    if (g_c6_stage1_u_bounds.owner_id != 0 && cur_obj == g_c6_stage1_u_bounds.owner_id) {
      bounds = &g_c6_stage1_u_bounds;
    } else if (g_c6_stage1_driver_bounds.owner_id != 0 && cur_obj == g_c6_stage1_driver_bounds.owner_id) {
      bounds = &g_c6_stage1_driver_bounds;
    }

    if (bounds == nullptr || !shizuku_c6_is_authorized_u_object(bounds->owner_id, cur_obj)) {
      active_context = &c;
      active_frame = c.sp;
      active_stop_phase = 0xE004u;
      stop(0xE004); // 未認可または未知の U object
    }

    // bounds だけで決まる検査 (role・code/trampoline/stack の形・PMA marker との非重複) は
    // bounds の版 (pmp_epoch) ごとに 1 回だけ行う。bounds の中身は update_precomputed() で版が進む。
    if (__builtin_expect(bounds->static_ok_epoch != bounds->pmp_epoch, 0)) {
      if (!bounds_static_ok(*bounds)) {
        active_context = &c;
        active_frame = c.sp;
        active_stop_phase = 0xE004u;
        stop(0xE004);
      }
      bounds->static_ok_epoch = bounds->pmp_epoch;
    }

    const uintptr_t code_base = bounds->code_base;
    const uintptr_t code_limit = bounds->code_limit;
    const uintptr_t data_base = bounds->data_base;
    const uintptr_t data_limit = bounds->data_limit;
    const uintptr_t stack_base = bounds->stack_base;
    const uintptr_t stack_limit = bounds->stack_limit;
    const uintptr_t trampoline = bounds->u_return_trampoline;
    const uintptr_t mmio_base = bounds->mmio_base;
    const uintptr_t mmio_limit = bounds->mmio_limit;

    // mepc がオブジェクト所有 code 範囲内にあることを厳格検査
    // (code_base != 0 && code_limit > code_base は bounds_static_ok で検査済み)
    if (f.mepc < code_base || f.mepc >= code_limit) {
      active_context = &c;
      active_frame = c.sp;
      active_stop_phase = 0xE004u;
      stop(0xE004);
    }

    // 戻り先 ra が kernel IRAM の return_stub の場合、U 所有の return trampoline に切替
    if (f.ra == arch::return_stub()) {
      f.ra = trampoline;
    }

    // active SP (f.reserved0) の設定および検証:
    if (f.reserved0 < stack_base || f.reserved0 > stack_limit) {
      f.reserved0 = stack_limit;
    }

    // U-mode 走行中の mscratch に設定する trusted kernel stack cursor:
    const uintptr_t kstack_top = reinterpret_cast<uintptr_t>(shizuku_c6_kernel_stack + sizeof(shizuku_c6_kernel_stack));
    f.reserved1 = kstack_top;

    // PMA ready marker は U が読書きできない範囲になければならない。
    // code/stack/data/mmio は bounds_static_ok で検査済み。共有窓は context ごとなので毎回見る。
    if (c.region_limit != 0) {
      const uintptr_t mk_lo = reinterpret_cast<uintptr_t>(&shizuku_c6_stage1_pma_ready);
      const uintptr_t mk_hi = mk_lo + sizeof(shizuku_c6_stage1_pma_ready);
      if (shizuku::chips::esp32_c6::pmp_controller::ranges_overlap(c.region_base, c.region_limit, mk_lo, mk_hi)) {
        active_context = &c;
        active_frame = c.sp;
        active_stop_phase = 0xE004u;
        stop(0xE004);
      }
    }

    // 固定エントリの初期化・更新 (M-mode 経由で PMP disable された後や、コード/共有窓切替時に再設定)
    auto &shadow = s_pmp_shadow;
    if (__builtin_expect(shadow.fixed_code_base != code_base ||
                         shadow.fixed_code_limit != code_limit ||
                         shadow.fixed_region_base != c.region_base ||
                         shadow.fixed_region_limit != c.region_limit, 0)) {
      shizuku::chips::esp32_c6::pmp_controller::init_fixed_entries(
          code_base, code_limit, c.region_base, c.region_limit);
      shadow.fixed_code_base = code_base;
      shadow.fixed_code_limit = code_limit;
      shadow.fixed_region_base = c.region_base;
      shadow.fixed_region_limit = c.region_limit;
    }

    // オブジェクト固有エントリへの切替え (前計算済み 8 本の CSR のみ書込み、固定エントリは触らない)。
    // CALL / RETURN の切替え (enter_u_object / switch_object_pmp) はここで書くので、U へ戻る書込みは
    // 1 往復につきここ 1 回だけ。今の bounds が入っているなら書かない。
    if (__builtin_expect(shadow.bounds != bounds || shadow.epoch != bounds->pmp_epoch, 1)) {
      shizuku::chips::esp32_c6::pmp_controller::switch_to_precomputed(bounds->precomputed);
      shadow.bounds = bounds;
      shadow.epoch = bounds->pmp_epoch;
    }
#endif
  } else {
    context_to_frame_to_m(c, f);
  }
}
}
extern "C" void shizuku_c6_mark_boot_phase(uint32_t phase) {
  if (phase == 0xC601u) {
    const uint32_t generation = shizuku_c6_boot_marker_magic == kBootMarkerMagic
        ? shizuku_c6_boot_generation + 1u : 1u;
    shizuku_c6_boot_marker_magic = 0u;
    shizuku_c6_boot_generation = generation ? generation : 1u;
    shizuku_c6_boot_marker = phase;
    shizuku_c6_stop_capture.magic = 0u;
    stop_capture_busy = 0u;
    active_frame = nullptr;
    active_context = nullptr;
    active_stop_phase = 0u;
    stack_diag_init();
    trap_probe_interval_count = 0u;
    s_pmp_shadow = {};
    shizuku_c6_boot_marker_magic = kBootMarkerMagic;
    return;
  }
  if (shizuku_c6_boot_marker_magic != kBootMarkerMagic) {
    shizuku_c6_boot_generation = 1u;
    shizuku_c6_boot_marker_magic = kBootMarkerMagic;
  }
  shizuku_c6_boot_marker = phase;
}
extern "C" void shizuku_c6_note_plain_task_stack_start(uint32_t task_id,
                                                         uintptr_t actual_sp) {
  if (shizuku_c6_stack_diag[0] != kStackDiagMagic ||
      shizuku_c6_stack_diag[1] != kStackDiagVersion)
    stack_diag_init();
  auto *context = shizuku::kernel_instance.current_context();
  const uintptr_t context_address = reinterpret_cast<uintptr_t>(context);
  if (!stop_capture::range_contains(kC6Dram, context_address,
                                   sizeof(arch::context_t), alignof(uint32_t))) {
    record_stack_fault(task_id, context_address, sizeof(arch::context_t),
                       static_cast<uint32_t>(actual_sp), 4u);
    return;
  }
  const uintptr_t stack_base = arch::stack_limit(*context);
  if (stack_base > UINTPTR_MAX - sizeof(shizuku_c6_kernel_stack)) {
    record_stack_fault(task_id, stack_base, sizeof(shizuku_c6_kernel_stack),
                       static_cast<uint32_t>(actual_sp), 2u);
    return;
  }
  write_stack_canary(task_id, stack_base,
                     stack_base + sizeof(shizuku_c6_kernel_stack), actual_sp);
}
extern "C" void shizuku_c6_note_plain_task_stack_probe(uint32_t task_id) {
  for (size_t slot = 1; slot < kStackDiagRecordCount; ++slot) {
    const size_t record = stack_record_offset(slot);
    if (shizuku_c6_stack_diag[record] == task_id) {
      check_stack_canary(slot);
      return;
    }
  }
}
namespace {
// The trapped thread was already saved by the caller; run the deferred switch after it.
SHIZUKU_HOT void run_pending_switch() {
  if (!pend_flag) return;
  pend_flag = 0;
  shizuku_c6_timer_diag[2] = shizuku_c6_timer_diag[2] + 1;
  auto &kernel = shizuku::kernel_instance;
  kernel.pendsv_dispatch(kernel.current_context());
}
// ---- M-mode ECALL: 例外入口 (arch/riscv/rv32_trap.S) が a0 の番号で shizuku_rv32_ecall_table を引いて
// 直接入る。route_trap の ecall 分岐 (mepc += 4 は asm 入口へ移した) と lambda の前後処理を、番号
// ごとの処理 (Body) の前後に付けたもの。割込み・例外は従来どおり shizuku_rv32_trap_dispatch。
using ecall_fn = arch::exception_frame_t *(*)(arch::exception_frame_t *);
#if defined(SHIZUKU_C6_UMODE)
static uintptr_t s_u_ecall_frame_latch = 0;
[[gnu::always_inline]] inline void record_u_mode_ecall_diag(const arch::exception_frame_t *frame) {
  arch::record_u_mode_ecall_diag(*frame, shizuku_c6_u_ecall_diag, shizuku_c6_u_mode_diag, &s_u_ecall_frame_latch);
}
// ecall_direct は 1 つの trap でこの記録を 1 回しか行わない (記録の後に別の記録点へ回らない) ので、
// 同じ frame の二重記録を防ぐ latch (読み・書き・戻りでの消去) は要らない。
[[gnu::always_inline]] inline void record_u_mode_ecall_diag_once(const arch::exception_frame_t *frame) {
  arch::record_u_mode_ecall_diag(*frame, shizuku_c6_u_ecall_diag, shizuku_c6_u_mode_diag);
}
#endif
template <class Body>
SHIZUKU_HOT inline arch::exception_frame_t *ecall_run(arch::exception_frame_t *frame, Body body) {
  // stack probe は従来どおり 1024 回の trap に 1 回 (ECALL も数える)。
  if (shizuku_c6_stack_diag[0] == kStackDiagMagic &&
      shizuku_c6_stack_diag[1] == kStackDiagVersion &&
      (++trap_probe_interval_count & (kStackProbeInterval - 1u)) == 0u)
    check_stack_canary(0u);
  active_frame = frame;
  active_context = nullptr;
  active_stop_phase = 0xC6F0u;
  const uint32_t previous_mstatus = active_mstatus;
  active_mstatus = frame->mstatus;
  shizuku_c6_trap_depth = shizuku_c6_trap_depth + 1;
  shizuku_c6_trap_diag[1] = frame->mcause;
  shizuku_c6_trap_diag[2] = frame->mepc - 4;  // diag は ECALL 命令の PC (frame の mepc は次の命令)
  shizuku_c6_trap_diag[3] = frame->mtval;
  // ECALL from inside the kernel must never overwrite the current thread.
  if (shizuku_c6_trap_depth != 1) stop(0xE003);
#if !defined(SHIZUKU_C6_UMODE)
  if ((frame->mstatus & RV32_MSTATUS_MPP) != RV32_MSTATUS_MPP) stop(0xE004);
#else
  record_u_mode_ecall_diag(frame);
#endif
  auto &kernel = shizuku::kernel_instance;
  auto *context = kernel.current_context();
  const auto *const entered = context;
  active_context = context;
  frame_to_context(*frame, *context);
  shizuku_c6_trap_diag[4] = shizuku_c6_trap_diag[4] + 1;
  body(kernel, context);
  run_pending_switch();
  context = kernel.current_context();
  active_context = context;
  active_frame = context->sp;
  // 呼び先の入口フレーム (RV32_TAG_ENTRY) は s*/gp/tp を caller の値のまま通す出口を使うので、
  // この trap の中で別のスレッドへ切り替わってはならない (切り替わるとそれらは別のスレッドの値)。
  if (frame->mcause == RV32_TAG_ENTRY && context->sp != frame) stop(0xE00B);
  // 別のスレッドへ切り替わったなら、再開するスレッドの素通しの出口を消去の出口に替える。
  if (context != entered) arch::scrub_on_switch(*context);
  context_to_frame(*context);
  auto *result = context->sp;
  active_mstatus = previous_mstatus;
  shizuku_c6_trap_depth = shizuku_c6_trap_depth - 1;
  active_frame = nullptr;
  active_context = nullptr;
  active_stop_phase = 0u;
#if defined(SHIZUKU_C6_UMODE)
  s_u_ecall_frame_latch = 0;
#endif
  return result;
}
// trap 内で直接処理する API (svc_attributes の mode == handler)。native の経路に立てていれば Body、
// 立てていない (kobj 本人・専用の親ハンドラ配下) か Body が false なら従来どおり svc_dispatch /
// svc_forward に回る (svc_entry)。
template <bool (shizuku::KERNEL::*Body)(shizuku::KERNEL::CONTEXT *)>
SHIZUKU_HOT arch::exception_frame_t *ecall_native(arch::exception_frame_t *frame) {
  return ecall_run(frame, [](auto &kernel, auto *context) {
    kernel.template svc_entry<Body>(context);
  });
}
// それ以外の番号 (thread mode の API・kobj 本人の primitive・objectland の raw 配送・範囲外)。
SHIZUKU_HOT arch::exception_frame_t *ecall_other(arch::exception_frame_t *frame) {
  return ecall_run(frame, [](auto &kernel, auto *context) { kernel.svc_dispatch(context); });
}
// ---- 切替えが起きない API の ECALL (ecall_direct) の、遅い経路。どれも ecall_direct の末尾から
// tail call で入る (成功経路は関数呼出しを持たず、ra と枠を作らない)。戻り値の型が frame なのは
// tail call にするためで、停止する関数は戻らない。noipa は「戻らない」ことをコンパイラに推論させない
// ため (推論されると tail call にならず、呼び出し側が ra を退避する枠を作ってしまう)。
__attribute__((noinline, noipa)) arch::exception_frame_t *ecall_fault_nested(arch::exception_frame_t *) {
  stop(0xE003);  // ECALL from inside the kernel must never overwrite the current thread.
}
__attribute__((noinline, noipa)) arch::exception_frame_t *ecall_fault_corrupt(arch::exception_frame_t *) {
  shizuku::KERNEL::BOARD::panic("call ledger count underflow");
}
// 呼び先の特権が落ちた (非特権の呼び先は M-only の C6 では復帰できない): context_to_frame と同じ停止。
__attribute__((noinline, noipa)) arch::exception_frame_t *ecall_fault_unprivileged(arch::context_t *context) {
  active_context = context;
  active_frame = context->sp;
  active_stop_phase = 0xE004u;
  stop(0xE004);
}
// 戻る前の仕上げ。Threads always resume with interrupts enabled; only the kernel runs with MIE=0.
[[gnu::always_inline]] inline arch::exception_frame_t *ecall_finish(arch::context_t *context) {
  auto *result = context->sp;
#if !defined(SHIZUKU_C6_UMODE)
  if (__builtin_expect(!context->privileged, 0)) return ecall_fault_unprivileged(context);
#endif
  if (__builtin_expect(context->privileged, 1)) {
    result->mstatus |= RV32_MSTATUS_MPP | RV32_MSTATUS_MPIE;
  } else {
    result->mstatus = (result->mstatus & ~RV32_MSTATUS_MPP) | RV32_MSTATUS_MPIE;
    context_to_frame(*context);
  }
  return result;
}
// 本体が kobj へ回した (スレッドの最外のメソッドが戻る・起点スレッドの YIELD): dispatch 段を積んで
// 親ハンドラ (kobj) の入口フレームを作る。
__attribute__((noinline, noipa)) arch::exception_frame_t *ecall_to_kobj(arch::context_t *context) {
  shizuku::kernel_instance.svc_forward(context, false);
  return ecall_finish(context);
}
// 切替えが起きない API (CALL_METHOD / CALL_METHOD_DESCRIPTOR / EXIT_METHOD / FORWARD_CHILD_EXIT /
// GET_CURRENT / GET_CALLER / SET_BUDGET) の ECALL。1 API = 1 関数: 本体 (native_call.hpp) をここへ
// 展開し、context_t を組まず trap フレームを直接扱う。診断と入れ子の記帳 (shizuku_c6_trap_diag・
// trap_depth・active_*・pend_flag) は割込み・例外 (shizuku_rv32_trap_dispatch) と、切替えが起きうる
// ecall_run の側にだけ残す。これらの API の本体は pend_context_switch を呼ばず、trap の中で
// 別のスレッドへ切り替わらない (pend_flag は常に 0)。
//   - stack probe は従来どおり 1024 回の trap に 1 回: 1024 回目に当たる ECALL は記帳つきの
//     ecall_other (ecall_run が数えて検査する) に回す。それ以外はカウンタを進めるだけ。
//   - cause 11 (M-mode の ECALL) なので mstatus.MPP は必ず M: 特権の検査は呼び先が落ちたときだけ。
//   - kernel の中からの ECALL (0xE003) は trap_depth の検査だけを残す (割込み・ecall_run が上げる)。
//   - 呼び出し元が kobj 本人・専用の親ハンドラ配下 (svc_native_route が偽) のときは、primitive や
//     raw 配送を決める従来の入口 (ecall_other = 記帳つき) に回す。
//   - 本体の結果 FORWARD は kobj へ回す (ecall_to_kobj)、CORRUPT は台帳の破れで停止する。
template <class Body>
SHIZUKU_HOT [[gnu::always_inline]] inline arch::exception_frame_t *ecall_direct(
    arch::exception_frame_t *frame, Body body) {
  const uint32_t probe = trap_probe_interval_count;
  if (__builtin_expect((probe & (kStackProbeInterval - 1u)) == kStackProbeInterval - 1u, 0))
    return ecall_other(frame);
  trap_probe_interval_count = probe + 1u;
  if (__builtin_expect(shizuku_c6_trap_depth != 0, 0)) return ecall_fault_nested(frame);
  auto &kernel = shizuku::kernel_instance;
  auto &thread = kernel.current_thread();
  if (__builtin_expect(!kernel.svc_native_route_of(thread), 0)) return ecall_other(frame);
#if defined(SHIZUKU_C6_UMODE)
  record_u_mode_ecall_diag_once(frame);
#endif
  arch::context_t *context = thread.context;
  context->sp = frame;
  // Keep the trapped caller privilege. U-mode ECALL handlers execute in M-mode, but native_call
  // must ledger the caller as U so the return path restores its user SP, PMP window, and U tag.
  arch::set_context_privilege_from_frame(*context, *frame);
  const shizuku::native::status result = body(kernel, thread, *context);
  if (__builtin_expect(result != shizuku::native::status::DONE, 0)) {
    if (result == shizuku::native::status::CORRUPT) return ecall_fault_corrupt(frame);
    return ecall_to_kobj(context);
  }
  return ecall_finish(context);
}
using K = shizuku::KERNEL;
using native_status = shizuku::native::status;
SHIZUKU_HOT arch::exception_frame_t *ecall_exit_method(arch::exception_frame_t *frame) {
  return ecall_direct(frame, [](K &k, auto &thread, auto &context) {
    return shizuku::native::exit_in_place<K, shizuku::KERNEL_OBJECT, false>(
        k, shizuku::kernel_object_instance, thread, context);
  });
}
SHIZUKU_HOT arch::exception_frame_t *ecall_forward_child_exit(arch::exception_frame_t *frame) {
  return ecall_direct(frame, [](K &k, auto &thread, auto &context) {
    return shizuku::native::exit_in_place<K, shizuku::KERNEL_OBJECT, true>(
        k, shizuku::kernel_object_instance, thread, context);
  });
}
SHIZUKU_HOT arch::exception_frame_t *ecall_call_method(arch::exception_frame_t *frame) {
  return ecall_direct(frame, [](K &k, auto &thread, auto &context) {
    shizuku::native::call_method_in_place(k, thread, context, shizuku::kernel_object_instance);
    return native_status::DONE;
  });
}
SHIZUKU_HOT arch::exception_frame_t *ecall_call_descriptor(arch::exception_frame_t *frame) {
  return ecall_direct(frame, [](K &k, auto &thread, auto &context) {
    shizuku::native::call_descriptor_in_place(k, thread, context, shizuku::kernel_object_instance);
    return native_status::DONE;
  });
}
SHIZUKU_HOT arch::exception_frame_t *ecall_get_current(arch::exception_frame_t *frame) {
  return ecall_direct(frame, [](K &k, auto &, auto &context) {
    return k.svc_get_current(&context) ? native_status::DONE : native_status::FORWARD;
  });
}
SHIZUKU_HOT arch::exception_frame_t *ecall_get_caller(arch::exception_frame_t *frame) {
  return ecall_direct(frame, [](K &k, auto &, auto &context) {
    return k.svc_get_caller(&context) ? native_status::DONE : native_status::FORWARD;
  });
}
SHIZUKU_HOT arch::exception_frame_t *ecall_set_budget(arch::exception_frame_t *frame) {
  return ecall_direct(frame, [](K &k, auto &, auto &context) {
    return k.svc_set_budget(&context) ? native_status::DONE : native_status::FORWARD;
  });
}
constexpr auto make_ecall_table() {
  using shizuku::object_api;
  std::array<ecall_fn, RV32_ECALL_TABLE_ENTRIES> t{};
  t.fill(&ecall_other);
  t[(unsigned)object_api::EXIT_METHOD] = &ecall_exit_method;
  t[(unsigned)object_api::CALL_METHOD] = &ecall_call_method;
  t[(unsigned)object_api::GET_CURRENT_OBJECT] = &ecall_get_current;
  t[(unsigned)object_api::GET_CALLER_OBJECT] = &ecall_get_caller;
  // 実行権を渡す API (切替えが起きる) は記帳つきの ecall_run を通る。
  t[(unsigned)object_api::YIELD] = &ecall_native<&K::svc_yield>;
  t[(unsigned)object_api::RUN_FOR] = &ecall_native<&K::svc_run_for>;
  t[(unsigned)object_api::SET_BUDGET] = &ecall_set_budget;
  t[(unsigned)object_api::FORWARD_CHILD_EXIT] = &ecall_forward_child_exit;
  t[(unsigned)object_api::CALL_METHOD_DESCRIPTOR] = &ecall_call_descriptor;
  return t;
}
}
// 番号 → 処理関数の表 (asm 入口が a0 で引く)。表の内容が svc_attributes の handler 集合と一致することを縛る。
extern "C" const std::array<ecall_fn, RV32_ECALL_TABLE_ENTRIES> shizuku_rv32_ecall_table = make_ecall_table();
static_assert(shizuku::ports::riscv::ecall_table_consistent(
                  make_ecall_table(), shizuku::svc_attributes.size(), &ecall_other,
                  [](size_t id) {
                    const auto *a = shizuku::svc_attribute_for(id);
                    return a != nullptr && a->mode == shizuku::svc_mode::handler;
                  }),
              "shizuku_rv32_ecall_table must match svc_attributes (mode == handler)");
bool shizuku_c6_handle_fault(arch::exception_frame_t *frame) {
#if !defined(SHIZUKU_C6_UMODE)
  (void)frame;
  return false;
#else
  shizuku_c6_u_mode_diag[4] = frame->mcause;
  shizuku_c6_u_mode_diag[5] = frame->mepc;
  shizuku_c6_u_mode_diag[6] = frame->mtval;

  // 1. 0x802 読み取りプローブ時の Illegal Instruction (mcause == 2) 捕捉
  if (frame->mcause == 2) {
    if (g_c6_probe_0x802_pc != 0 && frame->mepc == g_c6_probe_0x802_pc) {
      shizuku_c6_u_mode_diag[0] = 2; // Illegal instruction
      frame->a0 = 0xFFFFFFFFu;      // エラーマーカー返却
      frame->mepc += 4;             // 次命令へスキップ
      return true;
    }
    if (g_c6_probe_combo_0x802_pc != 0 && frame->mepc == g_c6_probe_combo_0x802_pc) {
      shizuku_c6_u_mode_diag[0] = 2; // Combo 内の Illegal instruction
      frame->mepc += 4;             // 次命令へスキップ
      return true;
    }
    if (g_c6_probe_upmpcfg_pc != 0 && frame->mepc == g_c6_probe_upmpcfg_pc) {
      shizuku_c6_u_mode_diag[12] = shizuku_c6_u_mode_diag[12] + 1;
      frame->a0 = 0xE00Eu;
      frame->mepc += 4;
      return true;
    }
  }

  // 2. Load access fault (mcause == 5)
  if (frame->mcause == 5) {
    if (g_c6_probe_pmp_fault_pc != 0 && frame->mepc == g_c6_probe_pmp_fault_pc) {
      shizuku_c6_u_mode_diag[3] = shizuku_c6_u_mode_diag[3] + 1;
      frame->a0 = 0xDEADBEEFu;
      frame->mepc += 4;
      return true;
    }
    if (g_c6_probe_combo_pmp_pc != 0 && frame->mepc == g_c6_probe_combo_pmp_pc) {
      shizuku_c6_u_mode_diag[3] = shizuku_c6_u_mode_diag[3] + 1;
      frame->mepc += 4;
      return true;
    }
    if (g_c6_probe_caller_stack_fault_pc != 0 && frame->mepc == g_c6_probe_caller_stack_fault_pc) {
      shizuku_c6_u_mode_diag[7] = shizuku_c6_u_mode_diag[7] + 1;
      frame->a0 = 0xCAFEBABEu;
      frame->mepc += 4;
      return true;
    }
    if (g_c6_probe_app_mmio_fault_pc != 0 && frame->mepc == g_c6_probe_app_mmio_fault_pc) {
      shizuku_c6_u_mode_diag[13] = shizuku_c6_u_mode_diag[13] + 1;
      frame->a0 = 0xE010u;
      frame->mepc += 4;
      return true;
    }
    if (g_c6_probe_app_driver_data_fault_pc != 0 && frame->mepc == g_c6_probe_app_driver_data_fault_pc) {
      shizuku_c6_u_mode_diag[14] = shizuku_c6_u_mode_diag[14] + 1;
      frame->a0 = 0xE011u;
      frame->mepc += 4;
      return true;
    }
    if (g_c6_probe_driver_app_data_fault_pc != 0 && frame->mepc == g_c6_probe_driver_app_data_fault_pc) {
      shizuku_c6_u_mode_diag[15] = shizuku_c6_u_mode_diag[15] + 1;
      frame->a0 = 0xE012u;
      frame->mepc += 4;
      return true;
    }
  }

  // 3. Store/AMO access fault (mcause == 7)
  if (frame->mcause == 7) {
    if (g_c6_probe_mstore_iram_pc != 0 && frame->mepc == g_c6_probe_mstore_iram_pc) {
      shizuku_c6_m_pma_diag[0] = shizuku_c6_m_pma_diag[0] + 1;
      frame->a0 = 0xE00Cu;
      frame->mepc += 4;
      return true;
    }
    if (g_c6_probe_ustore_kstack_pc != 0 && frame->mepc == g_c6_probe_ustore_kstack_pc) {
      shizuku_c6_u_mode_diag[8] = shizuku_c6_u_mode_diag[8] + 1;
      frame->a0 = 0xE008u;
      frame->mepc += 4;
      return true;
    }
    if (g_c6_probe_ustore_caller_pc != 0 && frame->mepc == g_c6_probe_ustore_caller_pc) {
      shizuku_c6_u_mode_diag[9] = shizuku_c6_u_mode_diag[9] + 1;
      frame->a0 = 0xE009u;
      frame->mepc += 4;
      return true;
    }
    if (g_c6_probe_ustore_code_pc != 0 && frame->mepc == g_c6_probe_ustore_code_pc) {
      shizuku_c6_u_mode_diag[10] = shizuku_c6_u_mode_diag[10] + 1;
      frame->a0 = 0xE00Au;
      frame->mepc += 4;
      return true;
    }
  }

  // 4. Instruction access fault (mcause == 1)
  if (frame->mcause == 1) {
    if (g_c6_probe_mfetch_target != 0 && frame->mepc == g_c6_probe_mfetch_target) {
      shizuku_c6_m_pma_diag[1] = shizuku_c6_m_pma_diag[1] + 1;
      frame->a0 = 0xE00Du;
      frame->mepc = frame->ra; // probe 呼出し元へ復帰
      return true;
    }
    if (g_c6_probe_ufetch_target != 0 && frame->mepc == g_c6_probe_ufetch_target) {
      shizuku_c6_u_mode_diag[11] = shizuku_c6_u_mode_diag[11] + 1;
      frame->a0 = 0xE00Bu;
      frame->mepc = frame->ra; // probe 呼出し元へ復帰
      return true;
    }
  }

  return false;
#endif
}

extern "C" SHIZUKU_HOT arch::exception_frame_t *shizuku_rv32_trap_dispatch(arch::exception_frame_t *frame) {
  if (shizuku_c6_stack_diag[0] == kStackDiagMagic &&
      shizuku_c6_stack_diag[1] == kStackDiagVersion &&
      (++trap_probe_interval_count & (kStackProbeInterval - 1u)) == 0u)
    check_stack_canary(0u);
  active_frame = frame;
  active_context = nullptr;
  active_stop_phase = 0xC6F0u;
  const uint32_t previous_mstatus = active_mstatus;
  active_mstatus = frame->mstatus;
  shizuku_c6_trap_depth = shizuku_c6_trap_depth + 1;
  shizuku_c6_trap_diag[1] = frame->mcause;
  shizuku_c6_trap_diag[2] = frame->mepc;
  shizuku_c6_trap_diag[3] = frame->mtval;
  auto *result = frame;
  shizuku::ports::riscv::route_trap(*frame, shizuku_c6_irq_depth, [&] {
    // ECALL from inside the kernel must never overwrite the current thread.
    if (shizuku_c6_trap_depth != 1) stop(0xE003);
#if !defined(SHIZUKU_C6_UMODE)
    if ((frame->mstatus & RV32_MSTATUS_MPP) != RV32_MSTATUS_MPP) stop(0xE004);
#else
    record_u_mode_ecall_diag(frame);
#endif
    auto &kernel = shizuku::kernel_instance;
    auto *context = kernel.current_context();
    const auto *const entered = context;
    active_context = context;
    frame_to_context(*frame, *context);
    shizuku_c6_trap_diag[4] = shizuku_c6_trap_diag[4] + 1;
    kernel.svc_dispatch(context);
    run_pending_switch();
    context = kernel.current_context();
    active_context = context;
    active_frame = context->sp;
    // 呼び先の入口フレーム (RV32_TAG_ENTRY) は s*/gp/tp を caller の値のまま通す出口を使うので、
    // この trap の中で別のスレッドへ切り替わってはならない (切り替わるとそれらは別のスレッドの値)。
    if (frame->mcause == RV32_TAG_ENTRY && context->sp != frame) stop(0xE00B);
    if (context != entered) arch::scrub_on_switch(*context);
    context_to_frame(*context);
    result = context->sp;
  }, [&] {
    // MIE is clear inside traps, so IRQs cannot nest: depth is exactly 1.
    if (shizuku_c6_trap_depth != 1 || shizuku_c6_irq_depth != 1) stop(0xE009);
#if !defined(SHIZUKU_C6_UMODE)
    if ((frame->mstatus & RV32_MSTATUS_MPP) != RV32_MSTATUS_MPP) stop(0xE004);
#endif
    const uint32_t cpu_irq = frame->mcause & 0x7fffffffu;
    if (cpu_irq != c6::TIMER_CPU_INT) {
      const int slot = irq_find_cpu(cpu_irq);
      if (slot < 0 || !shizuku_c6_irq_registrations[slot].enabled) stop(0xE001);
      auto &entry = shizuku_c6_irq_registrations[slot];
      entry.dispatch_count += 1u;
      entry.last_mstatus_mpie =
          (frame->mstatus & RV32_MSTATUS_MPIE) ? 1u : 0u;
      record_irq_hardware_state(entry);
      if ((reg_read(c6::PLIC_MXINT_TYPE) & (1u << cpu_irq)) &&
          (reg_read(c6::PLIC_MXINT_ENABLE) & (1u << cpu_irq)))
        plic_pulse_clear(1u << cpu_irq);
      auto &kernel = shizuku::kernel_instance;
      auto *context = kernel.current_context();
      const auto *const entered = context;
      active_context = context;
      frame_to_context(*frame, *context);
      if (g_irq_dispatch_hooks.on_enter)
        g_irq_dispatch_hooks.on_enter(entry.source, frame->mepc);
      entry.handler(entry.arg);
      if (g_irq_dispatch_hooks.on_exit)
        g_irq_dispatch_hooks.on_exit(entry.source);
      run_pending_switch();
      context = kernel.current_context();
      active_context = context;
      active_frame = context->sp;
      if (context != entered) arch::scrub_on_switch(*context);
      context_to_frame(*context);
      result = context->sp;
      return;
    }
    shizuku_c6_timer_diag[0] = shizuku_c6_timer_diag[0] + 1;
    if (shizuku_c6_irq_depth > shizuku_c6_timer_diag[1]) shizuku_c6_timer_diag[1] = shizuku_c6_irq_depth;
    shizuku_c6_timer_diag[4] = frame->mcause;
    reg_write(c6::SYSTIMER_INT_CLR, c6::TARGET0_INT_BIT);
    auto &kernel = shizuku::kernel_instance;
    auto *context = kernel.current_context();
    const auto *const entered = context;
    active_context = context;
    frame_to_context(*frame, *context);
    kernel.timer_expired();
    run_pending_switch();
    context = kernel.current_context();
    active_context = context;
    active_frame = context->sp;
    // 予算切れの取り上げで貸し手へ戻る場合もここ (別のスレッドの再開)。
    if (context != entered) arch::scrub_on_switch(*context);
    context_to_frame(*context);
    result = context->sp;
  }, [&] {
    // フォールト処理: 0x802 診断プローブおよび PMP fault 捕捉の処理
    if (shizuku_c6_handle_fault(frame)) {
      result = frame;
      return;
    }
    active_stop_phase = 0xE002u;
    stop(0xE002);
  });
  active_mstatus = previous_mstatus;
  shizuku_c6_trap_depth = shizuku_c6_trap_depth - 1;
  active_frame = nullptr;
  active_context = nullptr;
  active_stop_phase = 0u;
#if defined(SHIZUKU_C6_UMODE)
  s_u_ecall_frame_latch = 0;
#endif
  return result;
}
extern "C" [[noreturn]] void shizuku_rv32_return_failed() { stop(0xE005); }
// board 実装 (boards/esp32_c6) から呼ぶ SoC 側の入口。本体は上の無名 namespace の
// ままで、名前だけ chips の API として外へ出す (関数本体は変えない)。
namespace shizuku::chips::esp32_c6 {
void install_traps() { ::install_traps(); }
void interrupt_init() { ::interrupt_init(); }
[[noreturn]] void stop(uint32_t status) { ::stop(status); }
uint64_t systimer_now() { return ::systimer_now(); }
size_t irq_snapshot(irq_dispatch_snapshot *out, size_t capacity) {
  size_t count = 0;
  for (size_t i = 0; i < 24; ++i) {
    const auto &entry = shizuku_c6_irq_registrations[i];
    if (!entry.handler) continue;
    if (out && count < capacity) out[count] = {entry.source, entry.dispatch_count};
    ++count;
  }
  return count;
}
int irq_register(uint32_t source, uint32_t cpu_irq, irq_handler_t handler, void *arg,
                 uint32_t flags, bool enabled, uint32_t level, bool edge) {
  // Match IDF v6.1 esp32c6/esp_cpu_intr.c reservations and Shizuku's timer.
  // line 8 also rejected: hw test 2026-10-14 showed PLIC never pends it (see radio/c6_intr_alloc.cpp).
  if (source >= 128 || source == c6::SYSTIMER_TARGET0_SOURCE ||
      cpu_irq == 0 || cpu_irq > 30 || cpu_irq == 1 || cpu_irq == 3 ||
      cpu_irq == 4 || cpu_irq == 6 || cpu_irq == 7 || cpu_irq == 8 || cpu_irq == c6::TIMER_CPU_INT ||
      !handler || level == 0 || level > 7) return -1;
  if (::irq_find_source(source) >= 0 || ::irq_find_cpu(cpu_irq) >= 0) return -2;
  size_t slot = 24;
  for (size_t i = 0; i < 24; ++i) if (!shizuku_c6_irq_registrations[i].handler) { slot = i; break; }
  if (slot == 24) return -3;
  const uint32_t bit = 1u << cpu_irq;
  auto &entry = shizuku_c6_irq_registrations[slot];
  entry = {source, cpu_irq, handler, arg, false, 0u, flags};
  reg_write(c6::INTMTX_SOURCE_MAP_REG(source), enabled ? cpu_irq : 6u);
  uint32_t types = reg_read(c6::PLIC_MXINT_TYPE);
  reg_write(c6::PLIC_MXINT_TYPE, edge ? types | bit : types & ~bit);
  reg_write(c6::plic_pri_reg(cpu_irq), level);
  if (edge) plic_pulse_clear(bit);  // IDF clears only edge lines (intr_alloc.c:785-787)
  if (enabled) {
    reg_write(c6::PLIC_MXINT_ENABLE, reg_read(c6::PLIC_MXINT_ENABLE) | bit);
    asm volatile("csrs mie, %0" :: "r"(bit) : "memory");
    entry.enabled = true;
  }
  entry.last_mstatus_mpie = UINT32_MAX;
  record_irq_hardware_state(entry);
  return 0;
}
int irq_set_enabled(uint32_t source, uint32_t cpu_irq, irq_handler_t handler, void *arg,
                    bool enabled) {
  const int slot = ::irq_find(source, cpu_irq);
  if (slot < 0) return -1;
  auto &entry = shizuku_c6_irq_registrations[slot];
  if (entry.handler != handler || entry.arg != arg) return -1;
  const uint32_t bit = 1u << cpu_irq;
  const uint32_t mask = reg_read(c6::PLIC_MXINT_ENABLE);
  reg_write(c6::PLIC_MXINT_ENABLE, enabled ? mask | bit : mask & ~bit);
  if (enabled) asm volatile("csrs mie, %0" :: "r"(bit) : "memory");
  else asm volatile("csrc mie, %0" :: "r"(bit) : "memory");
  reg_write(c6::INTMTX_SOURCE_MAP_REG(source), enabled ? cpu_irq : 6u);
  entry.enabled = enabled;
  record_irq_hardware_state(entry);
  return 0;
}
int irq_unregister(uint32_t source, uint32_t cpu_irq, irq_handler_t handler, void *arg) {
  const int slot = ::irq_find(source, cpu_irq);
  if (slot < 0) return -1;
  auto &entry = shizuku_c6_irq_registrations[slot];
  if (entry.handler != handler || entry.arg != arg) return -1;
  const uint32_t bit = 1u << cpu_irq;
  reg_write(c6::PLIC_MXINT_ENABLE, reg_read(c6::PLIC_MXINT_ENABLE) & ~bit);
  asm volatile("csrc mie, %0" :: "r"(bit) : "memory");
  reg_write(c6::INTMTX_SOURCE_MAP_REG(source), 6u); // IDF's permanently-disabled input.
  entry = {};
  return 0;
}
void plic_vector_mode::timer_oneshot(uint32_t cycles) {
  systimer_disarm();
  const uint64_t target = c6::alarm_target(systimer_now(), cycles);
  armed_target = target;
  shizuku_c6_timer_diag[3] = cycles;
  reg_write(c6::SYSTIMER_TARGET0_HI, static_cast<uint32_t>(target >> 32));
  reg_write(c6::SYSTIMER_TARGET0_LO, static_cast<uint32_t>(target));
  reg_write(c6::SYSTIMER_COMP0_LOAD, c6::COMP0_LOAD_BIT);
  reg_write(c6::SYSTIMER_INT_ENA, reg_read(c6::SYSTIMER_INT_ENA) | c6::TARGET0_INT_BIT);
  reg_write(c6::SYSTIMER_CONF, reg_read(c6::SYSTIMER_CONF) | c6::CONF_TARGET0_WORK_EN);
}
void plic_vector_mode::timer_cancel() { systimer_disarm(); }
uint32_t plic_vector_mode::timer_remaining(bool &wrapped) {
  return c6::remaining_ticks(armed_target, systimer_now(), wrapped);
}
void plic_vector_mode::pend_context_switch() {
  // Only kernel code inside a trap may pend; the flag is consumed at trap exit.
  if (shizuku_c6_trap_depth == 0) stop(0xE00A);
  shizuku_c6_timer_diag[6] = shizuku_c6_timer_diag[6] + 1;
  pend_flag = 1;
}
void plic_vector_mode::enter_thread_mode(uintptr_t top, uintptr_t limit, void (*entry)()) {
  auto *context = kernel_instance.current_context();
  if (top < limit || top - limit < RV32_FRAME_BYTES + arch::CALL_HEADROOM) stop(0xE006);
  arch::prepare_thread_entry(*context, top, reinterpret_cast<uintptr_t>(entry), arch::return_stub(), 0);
  context_to_frame(*context);
  shizuku_rv32_first_return(context->sp);
}
bool plic_vector_mode::in_interrupt() { return shizuku_c6_irq_depth != 0; }
bool plic_vector_mode::current_priv() {
  // The stage-2 runnable domain is M-only; inside a trap report the saved MPP.
  return shizuku_c6_trap_depth == 0 ||
    (active_mstatus & RV32_MSTATUS_MPP) == RV32_MSTATUS_MPP;
}
bool plic_vector_mode::is_kstack_address(uintptr_t addr) {
  return shizuku_c6_is_kstack_address(addr);
}
uintptr_t plic_vector_mode::kstack_limit() {
  return shizuku_c6_kstack_limit();
}
uintptr_t plic_vector_mode::kstack_top() {
  return reinterpret_cast<uintptr_t>(shizuku_c6_kernel_stack + sizeof(shizuku_c6_kernel_stack));
}

bool plic_vector_mode::user_stack_bounds(uint32_t object_id, uintptr_t &base, uintptr_t &limit) {
#if defined(SHIZUKU_C6_UMODE)
  const c6_stage1_object_bounds *bounds = nullptr;
  if (g_c6_stage1_u_bounds.owner_id != 0 && object_id == g_c6_stage1_u_bounds.owner_id) {
    bounds = &g_c6_stage1_u_bounds;
  } else if (g_c6_stage1_driver_bounds.owner_id != 0 && object_id == g_c6_stage1_driver_bounds.owner_id) {
    bounds = &g_c6_stage1_driver_bounds;
  }
  if (bounds == nullptr || bounds->stack_base == 0 || bounds->stack_limit <= bounds->stack_base)
    return false;
  base = bounds->stack_base;
  limit = bounds->stack_limit;
  return true;
#else
  (void)object_id; (void)base; (void)limit;
  return false;
#endif
}
void plic_vector_mode::switch_object_pmp(uint32_t object_id) {
#if defined(SHIZUKU_C6_UMODE)
  const c6_stage1_object_bounds *bounds = nullptr;
  if (g_c6_stage1_u_bounds.owner_id != 0 && object_id == g_c6_stage1_u_bounds.owner_id) {
    bounds = &g_c6_stage1_u_bounds;
  } else if (g_c6_stage1_driver_bounds.owner_id != 0 && object_id == g_c6_stage1_driver_bounds.owner_id) {
    bounds = &g_c6_stage1_driver_bounds;
  }
  if (bounds == nullptr) stop(0xE004); // unknown caller window must never retain stale PMP state
  // 固有エントリは U へ戻る直前の context_to_frame が current_object() の bounds で書く (同じ object)。
  // ここでは書かない: 1 回の RETURN で同じ 8 本と fence を 2 回書いていた。
#else
  (void)object_id;
#endif
}

template <class Context, class Frame>
void plic_vector_mode::enter_u_object(Context &context, Frame &frame, uint32_t callee_id) {
#if defined(SHIZUKU_C6_UMODE)
  (void)context;
  const c6_stage1_object_bounds *bounds = nullptr;
  if (g_c6_stage1_u_bounds.owner_id != 0 && callee_id == g_c6_stage1_u_bounds.owner_id) {
    bounds = &g_c6_stage1_u_bounds;
  } else if (g_c6_stage1_driver_bounds.owner_id != 0 && callee_id == g_c6_stage1_driver_bounds.owner_id) {
    bounds = &g_c6_stage1_driver_bounds;
  }
  if (bounds != nullptr) {
    // 固有 PMP は書かない: U へ戻る直前の context_to_frame が current_object() (= callee) の bounds で書く。
    frame.ra = bounds->u_return_trampoline;
    frame.reserved0 = bounds->stack_limit;
    frame.reserved1 = reinterpret_cast<uintptr_t>(shizuku_c6_kernel_stack + sizeof(shizuku_c6_kernel_stack));
    frame.mcause = RV32_TAG_U_ENTRY;
    frame.mstatus = (frame.mstatus & ~0x1800u); // MPP = 0 (U-mode)
  }
#else
  (void)context; (void)frame; (void)callee_id;
#endif
}

template void plic_vector_mode::enter_u_object<chip::arch_type::context_t, chip::arch_type::exception_frame_t>(
    chip::arch_type::context_t &, chip::arch_type::exception_frame_t &, uint32_t);

bool plic_vector_mode::cycle_counter_enable() {
#if defined(__riscv)
  // ESP32-C6 HP core (RISC-V) M-mode performance counter:
  // CSR 0x7E0 (mpcer): イベント有効化 (bit 0: CPU cycles)
  // CSR 0x7E1 (mpcmr): モード設定 (bit 0: count in M-mode)
  // CSR 0x7E2 (mpccr): サイクルカウンタ値
  asm volatile("csrw 0x7e0, %0" :: "r"(1u) : "memory");
  asm volatile("csrw 0x7e1, %0" :: "r"(1u) : "memory");
  asm volatile("fence" ::: "memory");

  // 進行検出: カウンタが実際に進んでいることを確認して true を返す (ARMv8-M DWT 実装と同等の規律)
  // chip 層の busy-wait 禁止規律に基づき、spin-loop は排して連続 CSR 読取で判定する。
  uint32_t first = 0;
  asm volatile("fence\n csrr %0, 0x7e2\n fence" : "=r"(first) :: "memory");
  uint32_t second = 0;
  asm volatile("fence\n csrr %0, 0x7e2\n fence" : "=r"(second) :: "memory");
  return second != first;
#else
  return false;
#endif
}

SHIZUKU_HOT uint32_t plic_vector_mode::cycle_counter_read() {
#if defined(__riscv)
  uint32_t cycles = 0;
  asm volatile("csrr %0, 0x7e2" : "=r"(cycles));
  return cycles;
#else
  return 0;
#endif
}

void set_irq_dispatch_hooks(irq_dispatch_hooks hooks) {
  g_irq_dispatch_hooks = hooks;
}

} // namespace shizuku::chips::esp32_c6
