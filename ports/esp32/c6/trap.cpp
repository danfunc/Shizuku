#include "shizuku/kernel.hpp"
#include "ports/esp32/c6/trap_route.hpp"

using arch = shizuku::ARCH;
static_assert((sizeof(shizuku::KERNEL::call_frame_header) + C6_FRAME_BYTES) % 16 == 0,
              "V2 call-frame geometry must preserve the RV32 16-byte stack ABI");
extern "C" {
alignas(16) unsigned char shizuku_c6_kernel_stack[4096];
volatile uint32_t shizuku_c6_irq_depth;
volatile uint32_t shizuku_c6_trap_depth;
// status, cause, PC, mtval, ECALL count; JTAG-readable even on fatal traps.
volatile uint32_t shizuku_c6_trap_diag[5];
void shizuku_c6_vectors();
void shizuku_c6_return_stub();
[[noreturn]] void shizuku_c6_first_return(arch::exception_frame_t *);
extern char _bss_end;
}
namespace {
uint32_t active_mstatus;
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
  c.privileged = (f.mstatus & C6_MSTATUS_MPP) == C6_MSTATUS_MPP;
}
void context_to_frame(arch::context_t &c) {
  auto &f = *c.sp;
#define COPY(n) f.s##n = c.s##n;
  COPY(0) COPY(1) COPY(2) COPY(3) COPY(4) COPY(5)
  COPY(6) COPY(7) COPY(8) COPY(9) COPY(10) COPY(11)
#undef COPY
  f.mstatus = (f.mstatus & ~C6_MSTATUS_MPP) | (c.privileged ? C6_MSTATUS_MPP : 0);
  // Actual U-mode is not safe before PMP and a trusted U-entry stack exist.
  if (!c.privileged) stop(0xE004);
}
}
extern "C" arch::exception_frame_t *shizuku_c6_trap_dispatch(arch::exception_frame_t *frame) {
  const uint32_t previous_mstatus = active_mstatus;
  active_mstatus = frame->mstatus;
  shizuku_c6_trap_depth = shizuku_c6_trap_depth + 1;
  shizuku_c6_trap_diag[1] = frame->mcause;
  shizuku_c6_trap_diag[2] = frame->mepc;
  shizuku_c6_trap_diag[3] = frame->mtval;
  auto *result = frame;
  shizuku::ports::c6::route_trap(*frame, shizuku_c6_irq_depth, [&] {
    // ECALL from inside the kernel must never overwrite the current thread.
    if (shizuku_c6_trap_depth != 1) stop(0xE003);
    if ((frame->mstatus & C6_MSTATUS_MPP) != C6_MSTATUS_MPP) stop(0xE004);
    auto &kernel = shizuku::kernel_instance;
    auto *context = kernel.current_context();
    frame_to_context(*frame, *context);
    shizuku_c6_trap_diag[4] = shizuku_c6_trap_diag[4] + 1;
    kernel.svc_dispatch(context);
    context = kernel.current_context();
    context_to_frame(*context);
    result = context->sp;
  }, [&] {
    // No interrupt acknowledgement exists yet. Do not return into an IRQ storm.
    stop(0xE001);
  }, [&] {
    // V2 fault recovery requires timer/switch support. Preserve cause and stop.
    stop(0xE002);
  });
  active_mstatus = previous_mstatus;
  shizuku_c6_trap_depth = shizuku_c6_trap_depth - 1;
  return result;
}
extern "C" [[noreturn]] void shizuku_c6_return_failed() { stop(0xE005); }
namespace shizuku::archs {
uintptr_t rv32_c6::return_stub() { return reinterpret_cast<uintptr_t>(&shizuku_c6_return_stub); }
bool rv32_c6::in_interrupt() { return shizuku_c6_irq_depth != 0; }
bool rv32_c6::current_priv() {
  // The stage-2 runnable domain is M-only; inside a trap report the saved MPP.
  return shizuku_c6_trap_depth == 0 ||
    (active_mstatus & C6_MSTATUS_MPP) == C6_MSTATUS_MPP;
}
void rv32_c6::enter_thread_mode(uintptr_t top, uintptr_t limit, void (*entry)()) {
  auto *context = kernel_instance.current_context();
  if (top < limit || top - limit < C6_FRAME_BYTES + CALL_HEADROOM) stop(0xE006);
  prepare_thread_entry(*context, top, reinterpret_cast<uintptr_t>(entry), return_stub(), 0);
  context_to_frame(*context);
  shizuku_c6_first_return(context->sp);
}
}
namespace shizuku::boards {
void esp32_c6::init(uint32_t core) {
  if (core != 0) stop(0xE007);
  install_traps();
}
uintptr_t esp32_c6::unprivileged_floor() {
  // Stage-2 privileged-only pilot: BSS bookkeeping is below this boundary.
  // This is not a PMP security boundary. U-mode return is refused above.
  return reinterpret_cast<uintptr_t>(&_bss_end);
}
[[noreturn]] void esp32_c6::panic(const char *) { stop(0xE008); }
}
