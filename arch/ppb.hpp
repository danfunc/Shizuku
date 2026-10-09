#ifndef SHIZUKU_ARCHS_PPB_HPP
#define SHIZUKU_ARCHS_PPB_HPP
#include <cstdint>

// ARM Cortex-M の PPB (Private Peripheral Bus) レジスタ。番地とオフセットは
// ARM 仕様で固定されている。arch は外部ライブラリに依存しないので、ここで直接指す。
// RP2350 (Cortex-M33) の M33 レジスタ定義と照合した値 (SYST_* 0xE010 / ICSR 0xED04 /
// MPU_* 0xED90 / MAIR0 0xEDC0 / MPU_CTRL の PRIVDEFENA=bit2, ENABLE=bit0) と
// 一致することを確認して写した。
namespace shizuku::archs::ppb {
struct systick_t {
  volatile uint32_t csr;   // 0x00
  volatile uint32_t rvr;   // 0x04
  volatile uint32_t cvr;   // 0x08
  volatile uint32_t calib; // 0x0C
};
static_assert(sizeof(systick_t) == 0x10);
inline volatile systick_t *const systick = reinterpret_cast<volatile systick_t *>(0xE000E010u);

inline volatile uint32_t *const scb_icsr = reinterpret_cast<volatile uint32_t *>(0xE000ED04u);

struct mpu_t {
  volatile uint32_t type; // 0x00
  volatile uint32_t ctrl; // 0x04
  volatile uint32_t rnr;  // 0x08
  volatile uint32_t rbar; // 0x0C
  volatile uint32_t rlar; // 0x10
};
static_assert(sizeof(mpu_t) == 0x14);
inline volatile mpu_t *const mpu = reinterpret_cast<volatile mpu_t *>(0xE000ED90u);
inline volatile uint32_t *const mpu_mair0 = reinterpret_cast<volatile uint32_t *>(0xE000EDC0u);
inline constexpr uint32_t MPU_CTRL_PRIVDEFENA = 0x4u;
inline constexpr uint32_t MPU_CTRL_ENABLE = 0x1u;
} // namespace shizuku::archs::ppb
#endif // SHIZUKU_ARCHS_PPB_HPP
