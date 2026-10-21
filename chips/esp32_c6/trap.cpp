#include "shizuku/kernel.hpp"
#include "arch/riscv/trap_route.hpp"
#include "chips/esp32_c6/systimer.hpp"
#include "chips/esp32_c6/trap_api.hpp"
#include "chips/esp32_c6/stop_capture.hpp"

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
  const auto top = reinterpret_cast<uintptr_t>(shizuku_c6_kernel_stack + sizeof(shizuku_c6_kernel_stack));
  const auto vector = reinterpret_cast<uintptr_t>(&shizuku_c6_vectors) | 1u;
  asm volatile("csrw mscratch, %0\n csrw mtvec, %1" :: "r"(top), "r"(vector) : "memory");
}
void frame_to_context(arch::exception_frame_t &f, arch::context_t &c) {
  c.sp = &f;
#define COPY(n) c.s##n = f.s##n;
  COPY(0) COPY(1) COPY(2) COPY(3) COPY(4) COPY(5)
  COPY(6) COPY(7) COPY(8) COPY(9) COPY(10) COPY(11)
#undef COPY
  c.privileged = (f.mstatus & RV32_MSTATUS_MPP) == RV32_MSTATUS_MPP;
}
void context_to_frame(arch::context_t &c) {
  // Capture the untrusted context before dereferencing its saved SP.
  if (!c.privileged) {
    active_context = &c;
    active_frame = c.sp;
    active_stop_phase = 0xE004u;
    stop(0xE004);
  }
  auto &f = *c.sp;
#define COPY(n) f.s##n = c.s##n;
  COPY(0) COPY(1) COPY(2) COPY(3) COPY(4) COPY(5)
  COPY(6) COPY(7) COPY(8) COPY(9) COPY(10) COPY(11)
#undef COPY
  // Threads always resume with interrupts enabled; only the kernel runs with MIE=0.
  f.mstatus = ((f.mstatus & ~RV32_MSTATUS_MPP) | (c.privileged ? RV32_MSTATUS_MPP : 0)) | RV32_MSTATUS_MPIE;
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
void run_pending_switch() {
  if (!pend_flag) return;
  pend_flag = 0;
  shizuku_c6_timer_diag[2] = shizuku_c6_timer_diag[2] + 1;
  auto &kernel = shizuku::kernel_instance;
  kernel.pendsv_dispatch(kernel.current_context());
}
}
extern "C" arch::exception_frame_t *shizuku_rv32_trap_dispatch(arch::exception_frame_t *frame) {
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
    if ((frame->mstatus & RV32_MSTATUS_MPP) != RV32_MSTATUS_MPP) stop(0xE004);
    auto &kernel = shizuku::kernel_instance;
    auto *context = kernel.current_context();
    active_context = context;
    frame_to_context(*frame, *context);
    shizuku_c6_trap_diag[4] = shizuku_c6_trap_diag[4] + 1;
    kernel.svc_dispatch(context);
    run_pending_switch();
    context = kernel.current_context();
    active_context = context;
    active_frame = context->sp;
    context_to_frame(*context);
    result = context->sp;
  }, [&] {
    // MIE is clear inside traps, so IRQs cannot nest: depth is exactly 1.
    if (shizuku_c6_trap_depth != 1 || shizuku_c6_irq_depth != 1) stop(0xE009);
    if ((frame->mstatus & RV32_MSTATUS_MPP) != RV32_MSTATUS_MPP) stop(0xE004);
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
    active_context = context;
    frame_to_context(*frame, *context);
    kernel.timer_expired();
    run_pending_switch();
    context = kernel.current_context();
    active_context = context;
    active_frame = context->sp;
    context_to_frame(*context);
    result = context->sp;
  }, [&] {
    // V2 fault recovery requires timer/switch support. Preserve cause and stop.
    active_stop_phase = 0xE002u;
    stop(0xE002);
  });
  active_mstatus = previous_mstatus;
  shizuku_c6_trap_depth = shizuku_c6_trap_depth - 1;
  active_frame = nullptr;
  active_context = nullptr;
  active_stop_phase = 0u;
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

void set_irq_dispatch_hooks(irq_dispatch_hooks hooks) {
  g_irq_dispatch_hooks = hooks;
}

} // namespace shizuku::chips::esp32_c6
