#ifndef SHIZUKU_RP2350_BOOT_DIAG_RECORD_HPP
#define SHIZUKU_RP2350_BOOT_DIAG_RECORD_HPP
#include <cstdint>

namespace shizuku::boards::rp2350_boot_diag {
constexpr uint32_t magic = 0x53424432u; // SBD2; scratch[0..3] only (ROM owns 4..7).
enum : uint32_t { active = 1, complete = 2, fault = 3, scratch_only = 4 };
struct record {
  uint32_t tag, version, stage, stage_inverse, kind;
  uint32_t pc, cfsr, lr, xpsr, hfsr, mmfar, bfar, exc_return, sp, core, checksum;
};
constexpr uint32_t checksum(const record &r) {
  return r.version ^ r.stage ^ r.stage_inverse ^ r.kind ^ r.pc ^ r.cfsr ^ r.lr ^
         r.xpsr ^ r.hfsr ^ r.mmfar ^ r.bfar ^ r.exc_return ^ r.sp ^ r.core ^ 0xa17e936bu;
}
constexpr record seal(record r) {
  r.tag = magic; r.version = 1; r.stage_inverse = ~r.stage; r.checksum = checksum(r);
  return r;
}
constexpr bool valid(const record &r) {
  return r.tag == magic && r.version == 1 && r.stage_inverse == ~r.stage &&
         r.kind >= active && r.kind <= scratch_only && r.checksum == checksum(r);
}
constexpr bool recover(const record &r, bool watchdog_reset) {
  return watchdog_reset && valid(r) && r.kind != complete;
}
constexpr bool fatal_fault(bool boot_in_progress, uint32_t exc_return) {
  return boot_in_progress || (exc_return & 12u) != 12u;
}
constexpr bool readable_frame(uintptr_t sp, uintptr_t begin, uintptr_t end, bool stacking_error) {
  return !stacking_error && end >= begin && end - begin >= 32 &&
         sp >= begin && sp <= end - 32 && (sp & 3u) == 0;
}
} // namespace shizuku::boards::rp2350_boot_diag
#endif
