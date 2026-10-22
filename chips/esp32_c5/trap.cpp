#include "shizuku/kernel.hpp"
#include "arch/riscv/trap_route.hpp"
#include "chips/esp32_c5/systimer.hpp"
#include "chips/esp32_c5/trap_api.hpp"

#ifndef SHIZUKU_ESP_BLE_IRQ_LATENCY_DIAG
#if defined(SHIZUKU_C5_BLE) && SHIZUKU_C5_BLE
#define SHIZUKU_ESP_BLE_IRQ_LATENCY_DIAG 1
#else
#define SHIZUKU_ESP_BLE_IRQ_LATENCY_DIAG 0
#endif
#endif

using arch = shizuku::ARCH;
static_assert((sizeof(shizuku::KERNEL::call_frame_header) + RV32_FRAME_BYTES) % 16 == 0,
              "V2 call-frame geometry must preserve the RV32 16-byte stack ABI");
extern "C" {
alignas(16) unsigned char shizuku_c5_kernel_stack[4096];
volatile uint32_t shizuku_c5_irq_depth;
volatile uint32_t shizuku_c5_trap_depth;
// status, cause, PC, mtval, ECALL count; JTAG-readable even on fatal traps.
volatile uint32_t shizuku_c5_trap_diag[5];
// SYSTIMER IRQ count, max IRQ depth, pend/switch count, last oneshot ticks,
// last IRQ mcause, clicint readback after init, pend_context_switch calls.
volatile uint32_t shizuku_c5_timer_diag[7];
// [0] dispatched IRQ count, [1] last raw mcause, [2] CPU IRQ number,
// [3] peripheral source, [4] result (1=dispatched, 0=unregistered fatal).
volatile uint32_t shizuku_c5_irq_diag[5];
// Last interrupt setup snapshot: mstatus before masking, mintthresh,
// configured CLIC index, and final CLIC control word.
volatile uint32_t shizuku_c5_irq_control_diag[4];
struct c5_irq_registration {
  uint32_t source;
  uint32_t cpu_irq;
  shizuku::chips::esp32_c5::irq_handler_t handler;
  void *arg;
  bool enabled;
  volatile uint32_t dispatch_count;
  uint32_t flags;
  uint32_t matrix_readback;
  uint32_t clic_readback;
};
static_assert(sizeof(c5_irq_registration) == 9 * sizeof(uint32_t),
              "OpenOCD IRQ table layout must stay nine words per source");
c5_irq_registration shizuku_c5_irq_registrations[16];
void shizuku_c5_vector_table();
void shizuku_c5_mtvt_table();
extern char _bss_end;
}
namespace {
uint32_t active_mstatus;
volatile uint32_t pend_flag;
uint64_t armed_target;
namespace c5 = shizuku::ports::c5;
extern "C" void shizuku_freertos_compat_isr_enter(void) __attribute__((weak));
extern "C" void shizuku_freertos_compat_isr_exit(void) __attribute__((weak));
extern "C" uint32_t shizuku_freertos_compat_current_task_id(void) __attribute__((weak));
extern "C" void shizuku_esp_ble_note_irq_dispatch(uint32_t source,
    uint32_t interrupted_mepc, uint32_t task_id) __attribute__((weak));
extern "C" void shizuku_esp_ble_irq_mask_begin(uint32_t kind,
    uint32_t caller_pc, uint32_t task_id) __attribute__((weak));
extern "C" void shizuku_esp_ble_irq_mask_end(uint32_t kind) __attribute__((weak));
uint32_t mask_global_interrupts(uint32_t kind) {
  uintptr_t old_mstatus = 0;
  asm volatile("csrrc %0, mstatus, %1" : "=r"(old_mstatus) : "r"(uintptr_t{8}) : "memory");
#if defined(SHIZUKU_ESP_BLE_IRQ_LATENCY_DIAG) && SHIZUKU_ESP_BLE_IRQ_LATENCY_DIAG
  if ((old_mstatus & 8u) && shizuku_esp_ble_irq_mask_begin)
    shizuku_esp_ble_irq_mask_begin(kind,
        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(__builtin_return_address(0))),
        shizuku_freertos_compat_current_task_id
            ? shizuku_freertos_compat_current_task_id() : 0u);
#else
  (void)kind;
#endif
  return static_cast<uint32_t>(old_mstatus);
}
void restore_global_interrupts(uint32_t old_mstatus, uint32_t kind) {
#if defined(SHIZUKU_ESP_BLE_IRQ_LATENCY_DIAG) && SHIZUKU_ESP_BLE_IRQ_LATENCY_DIAG
  if ((old_mstatus & 8u) && shizuku_esp_ble_irq_mask_end)
    shizuku_esp_ble_irq_mask_end(kind);
#else
  (void)kind;
#endif
  asm volatile("csrw mstatus, %0" :: "r"(static_cast<uintptr_t>(old_mstatus)) : "memory");
}
inline void reg_write(uint32_t address, uint32_t value) {
  *reinterpret_cast<volatile uint32_t *>(address) = value;
}
inline uint32_t reg_read(uint32_t address) {
  return *reinterpret_cast<volatile uint32_t *>(address);
}
void note_irq_control(uint32_t mstatus_before_mask, uint32_t clic_index,
                      uint32_t clic_readback) {
  uint32_t mintthresh = 0;
#if defined(__riscv)
  asm volatile("csrr %0, 0x347" : "=r"(mintthresh));
#endif
  shizuku_c5_irq_control_diag[0] = mstatus_before_mask;
  shizuku_c5_irq_control_diag[1] = mintthresh;
  shizuku_c5_irq_control_diag[2] = clic_index;
  shizuku_c5_irq_control_diag[3] = clic_readback;
}
uint64_t systimer_now() {
  reg_write(c5::SYSTIMER_UNIT0_OP, c5::UNIT0_OP_UPDATE);
  while (!(reg_read(c5::SYSTIMER_UNIT0_OP) & c5::UNIT0_OP_VALUE_VALID)) {}
  const uint32_t high = reg_read(c5::SYSTIMER_UNIT0_VALUE_HI);
  const uint32_t low = reg_read(c5::SYSTIMER_UNIT0_VALUE_LO);
  return (static_cast<uint64_t>(high) << 32) | low;
}
void systimer_disarm() {
  reg_write(c5::SYSTIMER_CONF, reg_read(c5::SYSTIMER_CONF) & ~c5::CONF_TARGET0_WORK_EN);
  reg_write(c5::SYSTIMER_INT_ENA, reg_read(c5::SYSTIMER_INT_ENA) & ~c5::TARGET0_INT_BIT);
  reg_write(c5::SYSTIMER_INT_CLR, c5::TARGET0_INT_BIT);
}
// MIE stays clear here; threads enable it through MPIE on mret.
// CLIC mode: mie/mip CSR は使わず、clicint[idx] の ie bit で有効化する (C6 の PLIC 手順とは別物)。
void interrupt_init() {
  reg_write(c5::SYSTIMER_CONF, reg_read(c5::SYSTIMER_CONF) | c5::CONF_CLK_EN | c5::CONF_UNIT0_WORK_EN);
  systimer_disarm();
  reg_write(c5::SYSTIMER_TARGET0_CONF, 0);  // one-shot, UNIT0
  // interrupt matrix の値は CLIC の割込み番号 (外部線 + 16)。IDF interrupt_clic_ll_route と同じ。
  reg_write(c5::INTMTX_SYSTIMER_TARGET0_MAP, c5::TIMER_CLIC_INDEX);
  // 全体の閾値 (mintthresh): この level 以下を mask する。IDF RVHAL_INTR_ENABLE_THRESH=0 相当。
  asm volatile("csrw 0x347, %0" :: "r"(c5::clic_level_byte(c5::CLIC_THRESHOLD_LEVEL)) : "memory");
  // level trigger / non-vectored (shv=0) / M mode、優先度 TIMER_PRIORITY、有効化。
  reg_write(c5::clic_ctrl_reg(c5::TIMER_CLIC_INDEX), c5::clicint_value(c5::TIMER_PRIORITY, true));
  shizuku_c5_timer_diag[5] = reg_read(c5::clic_ctrl_reg(c5::TIMER_CLIC_INDEX));  // clicint 読戻し
}
[[noreturn]] void stop(uint32_t status) {
  shizuku_c5_trap_diag[0] = status;
  asm volatile("csrci mstatus, 8\n fence rw,rw" ::: "memory");
  for (;;) asm volatile("nop" ::: "memory");
}
void install_traps() {
  asm volatile("csrci mstatus, 8" ::: "memory");
  const auto top = reinterpret_cast<uintptr_t>(shizuku_c5_kernel_stack + sizeof(shizuku_c5_kernel_stack));
  // mtvec: 64B 整列の単一エントリ + mode=3 (CLIC)。mtvt は hardware vectored 用の表 (shv=0 なら引かれない)。
  const auto vector = reinterpret_cast<uintptr_t>(&shizuku_c5_vector_table) | c5::MTVEC_MODE_CLIC;
  const auto mtvt = reinterpret_cast<uintptr_t>(&shizuku_c5_mtvt_table);
  asm volatile("csrw mscratch, %0\n csrw 0x307, %2\n csrw mtvec, %1" :: "r"(top), "r"(vector), "r"(mtvt) : "memory");
}
// frame が s0-s11 などの本物の保持者 (文脈の s* は使わない)。ECALL のフレームは s* を保存しない。
void frame_to_context(arch::exception_frame_t &f, arch::context_t &c) {
  c.sp = &f;
  c.privileged = (f.mstatus & RV32_MSTATUS_MPP) == RV32_MSTATUS_MPP;
}
void context_to_frame(arch::context_t &c) {
  auto &f = *c.sp;
  // Threads always resume with interrupts enabled; only the kernel runs with MIE=0.
  f.mstatus = ((f.mstatus & ~RV32_MSTATUS_MPP) | (c.privileged ? RV32_MSTATUS_MPP : 0)) | RV32_MSTATUS_MPIE;
  // Actual U-mode is not safe before PMP and a trusted U-entry stack exist.
  if (!c.privileged) stop(0xE004);
}
bool dispatch_peripheral_irq(uint32_t cpu_irq, uint32_t raw_mcause,
                             uint32_t interrupted_mepc) {
  for (auto &entry : shizuku_c5_irq_registrations) {
    if (entry.handler && entry.enabled && entry.cpu_irq == cpu_irq) {
      shizuku_c5_irq_diag[0] = shizuku_c5_irq_diag[0] + 1;
      entry.dispatch_count = entry.dispatch_count + 1u;
      if ((entry.source == 4u || entry.source == 7u) &&
          shizuku_esp_ble_note_irq_dispatch)
        shizuku_esp_ble_note_irq_dispatch(entry.source, interrupted_mepc,
            shizuku_freertos_compat_current_task_id
                ? shizuku_freertos_compat_current_task_id() : 0u);
      shizuku_c5_irq_diag[1] = raw_mcause;
      shizuku_c5_irq_diag[2] = cpu_irq;
      shizuku_c5_irq_diag[3] = entry.source;
      shizuku_c5_irq_diag[4] = 1;
#if defined(SHIZUKU_ESP_BLE_IRQ_LATENCY_DIAG) && SHIZUKU_ESP_BLE_IRQ_LATENCY_DIAG
      if (entry.source == 4u && shizuku_esp_ble_irq_mask_begin)
        shizuku_esp_ble_irq_mask_begin(4u, interrupted_mepc,
            shizuku_freertos_compat_current_task_id
                ? shizuku_freertos_compat_current_task_id() : 0u);
#endif
      if (shizuku_freertos_compat_isr_enter) shizuku_freertos_compat_isr_enter();
      entry.handler(entry.arg);
      if (shizuku_freertos_compat_isr_exit) shizuku_freertos_compat_isr_exit();
#if defined(SHIZUKU_ESP_BLE_IRQ_LATENCY_DIAG) && SHIZUKU_ESP_BLE_IRQ_LATENCY_DIAG
      if (entry.source == 4u && shizuku_esp_ble_irq_mask_end)
        shizuku_esp_ble_irq_mask_end(4u);
#endif
      return true;
    }
  }
  shizuku_c5_irq_diag[1] = raw_mcause;
  shizuku_c5_irq_diag[2] = cpu_irq;
  shizuku_c5_irq_diag[3] = UINT32_MAX;
  shizuku_c5_irq_diag[4] = 0;
  return false;
}
}
namespace {
// The trapped thread was already saved by the caller; run the deferred switch after it.
void run_pending_switch() {
  if (!pend_flag) return;
  pend_flag = 0;
  shizuku_c5_timer_diag[2] = shizuku_c5_timer_diag[2] + 1;
  auto &kernel = shizuku::kernel_instance;
  kernel.pendsv_dispatch(kernel.current_context());
}
}
extern "C" arch::exception_frame_t *shizuku_rv32_trap_dispatch(arch::exception_frame_t *frame) {
  const uint32_t previous_mstatus = active_mstatus;
  active_mstatus = frame->mstatus;
  shizuku_c5_trap_depth = shizuku_c5_trap_depth + 1;
  shizuku_c5_trap_diag[1] = frame->mcause;
  // CLIC の mcause は上位に minhv/mpp/mpie/mpil が載る。共通の route_trap は mcause 全体を
  // 比較するので、割込みビットと exccode だけに正規化して渡す (生の値は diag[1] に残す)。
  frame->mcause = (frame->mcause & c5::MCAUSE_INTERRUPT) | (frame->mcause & c5::MCAUSE_EXCCODE_MASK);
  shizuku_c5_trap_diag[2] = frame->mepc;
  shizuku_c5_trap_diag[3] = frame->mtval;
  auto *result = frame;
  shizuku::ports::riscv::route_trap(*frame, shizuku_c5_irq_depth, [&] {
    // ECALL from inside the kernel must never overwrite the current thread.
    if (shizuku_c5_trap_depth != 1) stop(0xE003);
    if ((frame->mstatus & RV32_MSTATUS_MPP) != RV32_MSTATUS_MPP) stop(0xE004);
    auto &kernel = shizuku::kernel_instance;
    auto *context = kernel.current_context();
    frame_to_context(*frame, *context);
    shizuku_c5_trap_diag[4] = shizuku_c5_trap_diag[4] + 1;
    kernel.svc_dispatch(context);
    run_pending_switch();
    context = kernel.current_context();
    // 呼び先の入口フレームは s*/gp/tp を caller の値のまま通す出口を使うので、この trap の中で
    // 別のスレッドへ切り替わってはならない。
    if (frame->mcause == RV32_TAG_ENTRY && context->sp != frame) stop(0xE00B);
    context_to_frame(*context);
    result = context->sp;
  }, [&] {
    // MIE is clear inside traps, so IRQs cannot nest: depth is exactly 1.
    if (shizuku_c5_trap_depth != 1 || shizuku_c5_irq_depth != 1) stop(0xE009);
    const uint32_t clic_index = frame->mcause & c5::MCAUSE_EXCCODE_MASK;
    if ((frame->mstatus & RV32_MSTATUS_MPP) != RV32_MSTATUS_MPP) stop(0xE004);
    auto &kernel = shizuku::kernel_instance;
    auto *context = kernel.current_context();
    frame_to_context(*frame, *context);
    if (clic_index == c5::TIMER_CLIC_INDEX) {
      shizuku_c5_timer_diag[0] = shizuku_c5_timer_diag[0] + 1;
      if (shizuku_c5_irq_depth > shizuku_c5_timer_diag[1]) shizuku_c5_timer_diag[1] = shizuku_c5_irq_depth;
      shizuku_c5_timer_diag[4] = frame->mcause;
      reg_write(c5::SYSTIMER_INT_CLR, c5::TARGET0_INT_BIT);
      kernel.timer_expired();
    } else {
      if (clic_index < c5::CLIC_EXT_OFFSET || clic_index >= c5::CLIC_EXT_OFFSET + 32u)
        stop(0xE011);
      const uint32_t cpu_irq = clic_index - c5::CLIC_EXT_OFFSET;
      if (!dispatch_peripheral_irq(cpu_irq, frame->mcause, frame->mepc)) stop(0xE012);
    }
    run_pending_switch();
    context = kernel.current_context();
    context_to_frame(*context);
    result = context->sp;
  }, [&] {
    // V2 fault recovery requires timer/switch support. Preserve cause and stop.
    stop(0xE002);
  });
  active_mstatus = previous_mstatus;
  shizuku_c5_trap_depth = shizuku_c5_trap_depth - 1;
  return result;
}
extern "C" [[noreturn]] void shizuku_rv32_return_failed() { stop(0xE005); }
// board 実装 (boards/esp32_c5) から呼ぶ SoC 側の入口。本体は上の無名 namespace の
// ままで、名前だけ chips の API として外へ出す (関数本体は変えない)。
namespace shizuku::chips::esp32_c5 {
void install_traps() { ::install_traps(); }
void interrupt_init() { ::interrupt_init(); }
[[noreturn]] void stop(uint32_t status) { ::stop(status); }
uint64_t systimer_now() { return ::systimer_now(); }
int irq_register(uint32_t source, uint32_t cpu_irq, irq_handler_t handler, void *arg,
                 uint32_t flags, bool enabled, uint32_t level, bool edge) {
  if (source >= 128u || cpu_irq < 16u || cpu_irq >= 32u ||
      cpu_irq == c5::TIMER_CPU_INT || !handler) return -1;
  const uint32_t old_mstatus = mask_global_interrupts(2u);
  c5_irq_registration *free_entry = nullptr;
  for (auto &entry : shizuku_c5_irq_registrations) {
    if (entry.handler && (entry.source == source || entry.cpu_irq == cpu_irq)) {
      restore_global_interrupts(old_mstatus, 2u);
      return -2;
    }
    if (!entry.handler && !free_entry) free_entry = &entry;
  }
  if (!free_entry) {
    restore_global_interrupts(old_mstatus, 2u);
    return -3;
  }
  free_entry->source = source;
  free_entry->cpu_irq = cpu_irq;
  free_entry->handler = handler;
  free_entry->arg = arg;
  free_entry->enabled = enabled;
  free_entry->dispatch_count = 0;
  free_entry->flags = flags;
  const uint32_t clic_index = c5::clic_index_for_external_irq(cpu_irq);
  reg_write(c5::INTMTX_BASE + 4u * source, clic_index);
  reg_write(c5::clic_ctrl_reg(clic_index),
            c5::clicint_value(level, enabled, edge));
  asm volatile("fence iorw, iorw" ::: "memory");
  free_entry->matrix_readback = reg_read(c5::INTMTX_BASE + 4u * source);
  free_entry->clic_readback = reg_read(c5::clic_ctrl_reg(clic_index));
  note_irq_control(static_cast<uint32_t>(old_mstatus), clic_index, free_entry->clic_readback);
  restore_global_interrupts(old_mstatus, 2u);
  return 0;
}
int irq_set_enabled(uint32_t source, uint32_t cpu_irq, irq_handler_t handler, void *arg,
                    bool enabled) {
  const uint32_t old_mstatus = mask_global_interrupts(2u);
  for (auto &entry : shizuku_c5_irq_registrations) {
    if (entry.source != source || entry.cpu_irq != cpu_irq ||
        entry.handler != handler || entry.arg != arg) continue;
    const uint32_t clic_index = c5::clic_index_for_external_irq(cpu_irq);
    reg_write(c5::clic_ctrl_reg(clic_index),
              c5::clicint_value(entry.clic_readback >> 29 & 7u, enabled,
                                (entry.clic_readback & c5::CLICINT_ATTR_EDGE) != 0));
    entry.enabled = enabled;
    asm volatile("fence iorw, iorw" ::: "memory");
    entry.matrix_readback = reg_read(c5::INTMTX_BASE + 4u * source);
    entry.clic_readback = reg_read(c5::clic_ctrl_reg(clic_index));
    note_irq_control(static_cast<uint32_t>(old_mstatus), clic_index, entry.clic_readback);
    restore_global_interrupts(old_mstatus, 2u);
    return 0;
  }
  restore_global_interrupts(old_mstatus, 2u);
  return -1;
}
int irq_unregister(uint32_t source, uint32_t cpu_irq, irq_handler_t handler, void *arg) {
  const uint32_t old_mstatus = mask_global_interrupts(2u);
  for (auto &entry : shizuku_c5_irq_registrations) {
    if (entry.source == source && entry.cpu_irq == cpu_irq &&
        entry.handler == handler && entry.arg == arg) {
      reg_write(c5::clic_ctrl_reg(cpu_irq + c5::CLIC_EXT_OFFSET),
                c5::clicint_value(c5::TIMER_PRIORITY, false));
      reg_write(c5::INTMTX_BASE + 4u * source, 0);
      entry = {};
      asm volatile("fence iorw, iorw" ::: "memory");
      restore_global_interrupts(old_mstatus, 2u);
      return 0;
    }
  }
  restore_global_interrupts(old_mstatus, 2u);
  return -1;
}
void clic_direct_mode::timer_oneshot(uint32_t cycles) {
  systimer_disarm();
  const uint64_t target = c5::alarm_target(systimer_now(), cycles);
  armed_target = target;
  shizuku_c5_timer_diag[3] = cycles;
  reg_write(c5::SYSTIMER_TARGET0_HI, static_cast<uint32_t>(target >> 32));
  reg_write(c5::SYSTIMER_TARGET0_LO, static_cast<uint32_t>(target));
  reg_write(c5::SYSTIMER_COMP0_LOAD, c5::COMP0_LOAD_BIT);
  reg_write(c5::SYSTIMER_INT_ENA, reg_read(c5::SYSTIMER_INT_ENA) | c5::TARGET0_INT_BIT);
  reg_write(c5::SYSTIMER_CONF, reg_read(c5::SYSTIMER_CONF) | c5::CONF_TARGET0_WORK_EN);
}
void clic_direct_mode::timer_cancel() { systimer_disarm(); }
uint32_t clic_direct_mode::timer_remaining(bool &wrapped) {
  return c5::remaining_ticks(armed_target, systimer_now(), wrapped);
}
void clic_direct_mode::pend_context_switch() {
  // Only kernel code inside a trap may pend; the flag is consumed at trap exit.
  if (shizuku_c5_trap_depth == 0) stop(0xE00A);
  shizuku_c5_timer_diag[6] = shizuku_c5_timer_diag[6] + 1;
  pend_flag = 1;
}
void clic_direct_mode::enter_thread_mode(uintptr_t top, uintptr_t limit, void (*entry)()) {
  auto *context = kernel_instance.current_context();
  if (top < limit || top - limit < RV32_FRAME_BYTES + arch::CALL_HEADROOM) stop(0xE006);
  arch::prepare_thread_entry(*context, top, reinterpret_cast<uintptr_t>(entry), arch::return_stub(), 0);
  context_to_frame(*context);
  shizuku_rv32_first_return(context->sp);
}
bool clic_direct_mode::in_interrupt() { return shizuku_c5_irq_depth != 0; }
bool clic_direct_mode::current_priv() {
  // The stage-2 runnable domain is M-only; inside a trap report the saved MPP.
  return shizuku_c5_trap_depth == 0 ||
    (active_mstatus & RV32_MSTATUS_MPP) == RV32_MSTATUS_MPP;
}
size_t irq_snapshot(irq_dispatch_snapshot *out, size_t capacity) {
  size_t registered = 0;
  for (const auto &entry : shizuku_c5_irq_registrations) {
    if (!entry.handler) continue;
    if (out && registered < capacity) {
      out[registered].source = entry.source;
      out[registered].dispatch_count = entry.dispatch_count;
    }
    ++registered;
  }
  return registered;
}
} // namespace shizuku::chips::esp32_c5

// FreeRTOS ISR-yield shim hook. Called from the C5 peripheral trap after an
// ISR notification unblocks a higher-priority Shizuku task; the ordinary
// trap-exit path consumes this deferred switch request.
extern "C" void shizuku_esp_ble_pend_compat_context_switch(void) {
  shizuku::chips::esp32_c5::clic_direct_mode::pend_context_switch();
}
