#ifndef SHIZUKU_ARCHS_RV32_C5_HPP
#define SHIZUKU_ARCHS_RV32_C5_HPP

// ===========================================================================
//  ESP32-C5 (RV32IMAC 単核) の互換入口 (未完成・fail-closed)
// ===========================================================================
//  ★実体は arch/riscv/rv32.hpp の rv32<PMP_ENTRIES, PMP_GRANULE_BYTES, IntMode>。
//    C5 の選択 (16 エントリ(推定) / 128B 粒度 / CLIC direct mode) は
//    chips/esp32_c5/chip.hpp の chip::arch_type が持つ。
//  ★到達点は C6 と同じ M-only pilot (trap/ECALL、SYSTIMER oneshot、初回 mret)。
//    PMP による U-mode 保護とデバッグは未実装。SHIZUKU_C5_SKELETON_PERMITTED が無い
//    ビルドは明示的に拒否する (fail-closed)。

#if !defined(SHIZUKU_C5_SKELETON_PERMITTED)
#error "ESP32-C5 arch backend is incomplete (PMP ownership and U-mode privilege boundary remain incomplete). Build refused (fail-closed)."
#endif

#include "chips/esp32_c5/chip.hpp"
#include "shizuku/concepts/arch.hpp"

namespace shizuku {
namespace archs {

using rv32_c5 = shizuku::chips::esp32_c5::chip::arch_type;

static_assert(shizuku::concepts::arch_requires<rv32_c5>,
              "rv32_c5 must satisfy the arch concept");

} // namespace archs
} // namespace shizuku

#endif // SHIZUKU_ARCHS_RV32_C5_HPP
