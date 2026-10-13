#include "shizuku/kernel.hpp"
#include "arch/riscv/trap_route.hpp"
#include "chips/esp32_c6/systimer.hpp"
#include "chips/esp32_c6/trap_api.hpp"

using arch = shizuku::ARCH;
static_assert((sizeof(shizuku::KERNEL::call_frame_header) + RV32_FRAME_BYTES) % 16 == 0,
              "V2 call-frame geometry must preserve the RV32 16-byte stack ABI");
extern "C" {
alignas(16) unsigned char shizuku_c6_kernel_stack[4096];
volatile uint32_t shizuku_c6_irq_depth;
volatile uint32_t shizuku_c6_trap_depth;
// status, cause, PC, mtval, ECALL count; JTAG-readable even on fatal traps.
volatile uint32_t shizuku_c6_trap_diag[5];
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
volatile uint32_t pend_flag;
uint64_t armed_target;
namespace c6 = shizuku::ports::c6;
extern "C" void shizuku_freertos_compat_isr_enter(void) __attribute__((weak));
extern "C" void shizuku_freertos_compat_isr_exit(void) __attribute__((weak));
extern "C" uint32_t shizuku_freertos_compat_current_task_id(void) __attribute__((weak));
extern "C" void shizuku_esp_ble_note_irq_dispatch(uint32_t, uint32_t, uint32_t) __attribute__((weak));
inline void reg_write(uint32_t address, uint32_t value) {
  *reinterpret_cast<volatile uint32_t *>(address) = value;
}
inline uint32_t reg_read(uint32_t address) {
  return *reinterpret_cast<volatile uint32_t *>(address);
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
[[noreturn]] void stop(uint32_t status) {
  shizuku_c6_trap_diag[0] = status;
  asm volatile("csrci mstatus, 8\n fence rw,rw" ::: "memory");
  for (;;) asm volatile("nop" ::: "memory");
}
void install_traps() {
  asm volatile("csrci mstatus, 8" ::: "memory");
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
  auto &f = *c.sp;
#define COPY(n) f.s##n = c.s##n;
  COPY(0) COPY(1) COPY(2) COPY(3) COPY(4) COPY(5)
  COPY(6) COPY(7) COPY(8) COPY(9) COPY(10) COPY(11)
#undef COPY
  // Threads always resume with interrupts enabled; only the kernel runs with MIE=0.
  f.mstatus = ((f.mstatus & ~RV32_MSTATUS_MPP) | (c.privileged ? RV32_MSTATUS_MPP : 0)) | RV32_MSTATUS_MPIE;
  // Actual U-mode is not safe before PMP and a trusted U-entry stack exist.
  if (!c.privileged) stop(0xE004);
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
    frame_to_context(*frame, *context);
    shizuku_c6_trap_diag[4] = shizuku_c6_trap_diag[4] + 1;
    kernel.svc_dispatch(context);
    run_pending_switch();
    context = kernel.current_context();
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
        reg_write(c6::PLIC_MXINT_CLEAR, 1u << cpu_irq);
      auto &kernel = shizuku::kernel_instance;
      auto *context = kernel.current_context();
      frame_to_context(*frame, *context);
      if (shizuku_freertos_compat_isr_enter) shizuku_freertos_compat_isr_enter();
      if (shizuku_esp_ble_note_irq_dispatch)
        shizuku_esp_ble_note_irq_dispatch(entry.source, frame->mepc,
            shizuku_freertos_compat_current_task_id
                ? shizuku_freertos_compat_current_task_id() : 0u);
      entry.handler(entry.arg);
      if (shizuku_freertos_compat_isr_exit) shizuku_freertos_compat_isr_exit();
      run_pending_switch();
      context = kernel.current_context();
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
    frame_to_context(*frame, *context);
    kernel.timer_expired();
    run_pending_switch();
    context = kernel.current_context();
    context_to_frame(*context);
    result = context->sp;
  }, [&] {
    // V2 fault recovery requires timer/switch support. Preserve cause and stop.
    stop(0xE002);
  });
  active_mstatus = previous_mstatus;
  shizuku_c6_trap_depth = shizuku_c6_trap_depth - 1;
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
  if (source >= 128 || source == c6::SYSTIMER_TARGET0_SOURCE ||
      cpu_irq == 0 || cpu_irq > 30 || cpu_irq == 1 || cpu_irq == 3 ||
      cpu_irq == 4 || cpu_irq == 6 || cpu_irq == 7 || cpu_irq == c6::TIMER_CPU_INT ||
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
  reg_write(c6::PLIC_MXINT_CLEAR, bit);
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
} // namespace shizuku::chips::esp32_c6

extern "C" void shizuku_esp_ble_pend_compat_context_switch(void) {
  shizuku::chips::esp32_c6::plic_vector_mode::pend_context_switch();
}
