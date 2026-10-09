#ifndef SHIZUKU_ARCHS_RV32_C6_HPP
#define SHIZUKU_ARCHS_RV32_C6_HPP

// ===========================================================================
//  ESP32-C6 (RV32IMAC 単核) の互換入口 (未完成・fail-closed)
// ===========================================================================
//  ★実体は arch/riscv/rv32.hpp の rv32<PMP_ENTRIES, PMP_GRANULE_BYTES, IntMode>。
//    C6 の選択 (16 エントリ / 4B 粒度 / PLIC vector mode) は chips/esp32_c6/chip.hpp の
//    chip::arch_type が持つ。ここの rv32_c6 はその同じ型への別名 (既存の API・テスト用)。
//  ★現在の到達点:
//    - RV32IMAC レジスタおよび trap frame / context レイアウト (arch 側)
//    - PMP 設定 (pmp_codec<4>) の統合
//    - M-only trap/ECALL、SYSTIMER oneshot/プリエンプション、初回 mret を実装 (chip 側)。
//    - PMP による U-mode 権限境界保護およびハードウェアデバッグは未実装 (M-only パイロット)。
//    - ファームウェア組み込み時の欺瞞的 no-op 動作を防ぐため、
//      SHIZUKU_C6_SKELETON_PERMITTED が定義されていない本番ビルドでは
//      明示的にコンパイルを拒否する (fail-closed)。ゲートは C6 側に残し、arch/riscv は見ない。

#if !defined(SHIZUKU_C6_SKELETON_PERMITTED)
#error "ESP32-C6 arch backend is incomplete (PMP ownership and U-mode privilege boundary remain incomplete). Build refused (fail-closed)."
#endif

#include "chips/esp32_c6/chip.hpp"
#include "shizuku/concepts/arch.hpp"

namespace shizuku {
namespace archs {

using rv32_c6 = shizuku::chips::esp32_c6::chip::arch_type;

static_assert(shizuku::concepts::arch_requires<rv32_c6>,
              "rv32_c6 must satisfy the arch concept");

} // namespace archs
} // namespace shizuku

#endif // SHIZUKU_ARCHS_RV32_C6_HPP
