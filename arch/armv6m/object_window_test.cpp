// RP2040 (ARMv6-M) の非特権オブジェクト保護窓: MPU レジスタには触らず、前計算した
// RBAR/RASR (2 の冪 + subregion) の値と、表現できない窓の拒否を host で縛る。
#include "armv6m.hpp"

#include <cassert>

using arch = shizuku::archs::armv6m<8, 2>;

int main() {
  // ---- stack 窓: 4096B を 4096B 境界に置くと 4KB 領域でちょうど (SRD なし) ----
  {
    const auto w = arch::encode_stack_window(0x20011000u, 0x20012000u);
    assert(w.rbar == 0x20011000u);
    assert(w.rlar == 0x130C0017u);  // AP=RW_ALL XN S/TEX=Normal SRD=0 SIZE=4KB EN
    // 1KB ずれた置き方は 16KB 領域に化けて隣を巻き込むので、窓にしない (空 = 触れない)。
    assert(arch::encode_stack_window(0x20011400u, 0x20012400u).rlar == 0);
    assert(arch::STACK_WINDOW_ALIGN == 4096u);
  }

  // ---- data 窓: 256B ちょうど。2048B 以上の 1/8 単位は SRD で端を削る ----
  {
    const auto w = arch::encode_data_window(0x20012100u, 0x20012200u);
    assert(w.rbar == 0x20012100u && w.rlar == 0x130C000Fu);
    // 3KB: 4KB 領域の末尾 2/8 を subregion 無効化 (bit7,bit6 = 0xC0)。
    const auto sub = arch::encode_data_window(0x20012000u, 0x20012C00u);
    assert(sub.rbar == 0x20012000u && sub.rlar == 0x130CC017u);
    assert(((sub.rlar >> 8) & 0xFFu) == 0xC0u);
    // 大きさが 2 の冪でも 1/8 単位でもない窓、空・逆転・base 0 は窓なし。
    assert(arch::encode_data_window(0x20012100u, 0x20012300u).rlar == 0);
    assert(arch::encode_data_window(0x20012000u, 0x20012000u).rlar == 0);
    assert(arch::encode_data_window(0x20012200u, 0x20012000u).rlar == 0);
    assert(arch::encode_data_window(0, 0x100u).rlar == 0);
  }

  // ---- MMIO 窓: Device shareable (TEX=0 C=0 B=1 S=1)。DMA / PIO / PPB / 周辺外は出さない ----
  {
    const auto uart = arch::encode_mmio_window(0x40034000u, 0x40035000u);
    assert(uart.rbar == 0x40034000u && uart.rlar == 0x13050017u);
    assert(arch::encode_mmio_window(0x50000000u, 0x50001000u).rlar == 0);  // DMA
    assert(arch::encode_mmio_window(0x50200000u, 0x50201000u).rlar == 0);  // PIO0
    assert(arch::encode_mmio_window(0xE000E000u, 0xE000F000u).rlar == 0);  // NVIC / SCB (PPB)
    assert(arch::encode_mmio_window(0xD0000000u, 0xD0001000u).rlar == 0);  // SIO
    assert(arch::encode_mmio_window(0x20000000u, 0x20001000u).rlar == 0);  // SRAM は MMIO 窓にしない
    assert(arch::encode_mmio_window(0x4FFFF000u, 0x50000100u).rlar == 0);  // DMA に 1 バイトでもかかる
  }

  // ---- 窓の表: 表現できる窓だけ登録し、できなければ false で何も残さない ----
  {
    assert(arch::set_object_bounds(5, 0x20012100u, 0x20012200u, 0, 0));
    assert(arch::has_object_bounds(5));
    assert(arch::s_object_windows[5].data.rlar == 0x130C000Fu);
    assert(arch::s_object_windows[5].mmio.rlar == 0);
    assert(!arch::set_object_bounds(6, 0x20012100u, 0x20012300u, 0, 0));  // 表現できない data
    assert(!arch::has_object_bounds(6));
    assert(!arch::set_object_bounds(7, 0x20012100u, 0x20012200u, 0x50000000u, 0x50001000u));  // DMA
    assert(!arch::has_object_bounds(7));
    assert(!arch::set_object_bounds(arch::kMaxObjectWindows, 0x20012100u, 0x20012200u, 0, 0));
    assert(!arch::has_object_bounds(arch::kMaxObjectWindows));
    // data と MMIO を両方持つ (ドライバ)。
    assert(arch::set_object_bounds(8, 0x20012100u, 0x20012200u, 0x40034000u, 0x40035000u));
    assert(arch::s_object_windows[8].mmio.rlar == 0x13050017u);
  }
  return 0;
}
