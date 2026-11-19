#include "boards/rp2350_pico2/boot_diag.hpp"
#include "boards/rp2350_pico2/boot_diag_record.hpp"
#include "chips/rp2350/chip.hpp"
#include "hardware/regs/m33.h"
#include "hardware/structs/scb.h"
#include "hardware/structs/watchdog.h"
#include "hardware/watchdog.h"
#include "pico/runtime_init.h"
#include "pico/stdlib.h"
#include <cstdio>

extern "C" void shizuku_armv8m_fault_entry();
extern "C" uint32_t __vectors[], __vectors_end[];
namespace {
namespace diag = shizuku::boards::rp2350_boot_diag;
using chip = shizuku::chips::rp2350::chip;
// No initializers: SDK CRT0 copies neither .uninitialized_data nor these records.
volatile diag::record saved __attribute__((section(".uninitialized_data.shizuku_boot_diag"), used));
alignas(512) uint32_t early_vectors[128] __attribute__((section(".uninitialized_data.shizuku_boot_vectors")));
diag::record previous;
bool recovery_pending;
bool monitoring;
bool owns_watchdog;

diag::record load() {
  return {saved.tag, saved.version, saved.stage, saved.stage_inverse, saved.kind,
          saved.pc, saved.cfsr, saved.lr, saved.xpsr, saved.hfsr, saved.mmfar, saved.bfar,
          saved.exc_return, saved.sp, saved.core, saved.checksum};
}
void store(diag::record r) {
  r = diag::seal(r);
  saved.tag = 0; // commit last, so a reset during the write cannot validate a torn record
  saved.version = r.version; saved.stage = r.stage; saved.stage_inverse = r.stage_inverse;
  saved.kind = r.kind; saved.pc = r.pc; saved.cfsr = r.cfsr; saved.lr = r.lr;
  saved.xpsr = r.xpsr; saved.hfsr = r.hfsr; saved.mmfar = r.mmfar; saved.bfar = r.bfar;
  saved.exc_return = r.exc_return; saved.sp = r.sp; saved.core = r.core; saved.checksum = r.checksum;
  __asm volatile("dsb" ::: "memory");
  saved.tag = r.tag;
  watchdog_hw->scratch[1] = r.stage;
  watchdog_hw->scratch[2] = r.pc;
  watchdog_hw->scratch[3] = r.cfsr;
  watchdog_hw->scratch[0] = r.kind == diag::complete ? 0u : diag::magic;
  __asm volatile("dsb" ::: "memory");
}
void install_faults() {
  auto *vectors = reinterpret_cast<volatile uint32_t *>(scb_hw->vtor);
  const uint32_t handler = reinterpret_cast<uintptr_t>(&shizuku_armv8m_fault_entry);
  for (unsigned i = 3; i <= 6; ++i) vectors[i] = handler;
  __asm volatile("dsb\nisb" ::: "memory");
}
void early() {
  previous = load();
  // Do not call a ROM helper before SDK bootrom initialization (00050).
  const bool reset = watchdog_hw->reason != 0;
  recovery_pending = diag::recover(previous, reset);
  if (!recovery_pending && reset && watchdog_hw->scratch[0] == diag::magic) {
    previous = diag::seal({0, 0, watchdog_hw->scratch[1], 0, diag::scratch_only,
                           watchdog_hw->scratch[2], watchdog_hw->scratch[3]});
    recovery_pending = true; // scratch fallback: remaining register fields are unknown/zero
  }
  monitoring = true;
  const uintptr_t count = (reinterpret_cast<uintptr_t>(__vectors_end) -
                           reinterpret_cast<uintptr_t>(__vectors)) / sizeof(uint32_t);
  for (unsigned i = 0; i < 128; ++i) early_vectors[i] = i < count ? __vectors[i] : 0;
  scb_hw->vtor = reinterpret_cast<uintptr_t>(early_vectors);
  install_faults();
  shizuku_rp2350_boot_stage(1); // CRT copies + BSS clear completed; SDK runtime entered
}
void clocks_ready() {
  shizuku_rp2350_boot_stage(2);
  watchdog_enable(8000, true); // clocks_init configured the watchdog tick
  owns_watchdog = true;
}
void vectors_ready() {
  install_faults(); // SDK replaces VTOR at 10080; retain handlers on the new RAM table
  shizuku_rp2350_boot_stage(3);
}
PICO_RUNTIME_INIT_FUNC(early, "00001");
PICO_RUNTIME_INIT_FUNC(clocks_ready, "00501");
PICO_RUNTIME_INIT_FUNC(vectors_ready, "10081");
} // namespace

extern "C" void shizuku_rp2350_boot_poll() {
  if (owns_watchdog) watchdog_update();
}
extern "C" void shizuku_rp2350_boot_stage(uint32_t stage) {
  // Stage 60 is the first thread-mode app entry: intentional U fault tests follow.
  if (chip::core_id() != 0) return;
  const bool done = stage == 60;
  store({0, 0, stage, 0, done ? diag::complete : diag::active});
  shizuku_rp2350_boot_poll();
  if (done) {
    monitoring = false;
#if !(defined(SHIZUKU_SELFTEST_PROGRESS) && SHIZUKU_SELFTEST_PROGRESS > 0)
    if (owns_watchdog) watchdog_disable();
#endif
    owns_watchdog = false;
  }
}
extern "C" void shizuku_rp2350_boot_fault(uint32_t exc_return, uintptr_t raw_sp) {
  // Expected Thread/PSP faults still follow the existing kernel recovery path.
  if (!diag::fatal_fault(monitoring, exc_return)) return;
  const uint32_t cfsr = scb_hw->cfsr;
  // SDK describes MMFSR as one byte: architectural MUNSTKERR/MSTKERR/MLSPERR = bits 3..5.
  constexpr uint32_t mmfsr_frame_errors = 0x38u;
  const uint32_t stack_errors = mmfsr_frame_errors | M33_CFSR_BFSR_UNSTKERR_BITS |
      M33_CFSR_BFSR_STKERR_BITS | M33_CFSR_BFSR_LSPERR_BITS | M33_CFSR_UFSR_STKOF_BITS;
  diag::record r = load();
  r.kind = diag::fault; r.pc = 0; r.lr = 0; r.xpsr = 0;
  r.cfsr = cfsr; r.hfsr = scb_hw->hfsr; r.mmfar = scb_hw->mmfar; r.bfar = scb_hw->bfar;
  r.exc_return = exc_return; r.sp = raw_sp; r.core = chip::core_id();
  if (diag::readable_frame(raw_sp, chip::kSramBase, chip::kSramEnd, (cfsr & stack_errors) != 0)) {
    // Same basic core-frame layout as arch/armv8m::exception_frame_t; no FP instructions here.
    const auto *frame = reinterpret_cast<const volatile uint32_t *>(raw_sp);
    r.lr = frame[5]; r.pc = frame[6]; r.xpsr = frame[7];
  }
  store(r);
  watchdog_reboot(0, 0, 0); // immediate trigger works even before timer/USB initialization
  for (;;) __asm volatile("wfi");
}
extern "C" void shizuku_rp2350_boot_recover_if_needed() {
  if (!recovery_pending) return;
  // Stay before kernel/MPU/bootstrap on this boot, so their fault cannot loop before CDC output.
  watchdog_disable(); owns_watchdog = false;
  for (;;) {
    printf("[RP2350_BOOT_PREV] stage=%lu kind=%lu core=%lu pc=%08lx cfsr=%08lx hfsr=%08lx\n",
           (unsigned long)previous.stage, (unsigned long)previous.kind, (unsigned long)previous.core,
           (unsigned long)previous.pc, (unsigned long)previous.cfsr, (unsigned long)previous.hfsr);
    printf("[RP2350_BOOT_PREV] lr=%08lx xpsr=%08lx sp=%08lx excret=%08lx mmfar=%08lx bfar=%08lx\n",
           (unsigned long)previous.lr, (unsigned long)previous.xpsr, (unsigned long)previous.sp,
           (unsigned long)previous.exc_return, (unsigned long)previous.mmfar, (unsigned long)previous.bfar);
    fflush(stdout);
    sleep_ms(250); // USB IRQ/alarm polling works without Shizuku bootstrap
  }
}
