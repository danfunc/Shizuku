#include <type_traits>
#include <cassert>
#define SHIZUKU_C5_SKELETON_PERMITTED 1
#include "chips/esp32_c5/rv32_c5.hpp"
#include "boards/esp32_c5/esp32_c5.hpp"

using arch = shizuku::archs::rv32_c5;
using board = shizuku::boards::esp32_c5;
namespace c5 = shizuku::chips::esp32_c5;

static_assert(shizuku::concepts::arch_requires<arch>, "rv32_c5 must satisfy arch concept");
static_assert(shizuku::concepts::board_requires<board>, "esp32_c5 must satisfy board concept");
static_assert(sizeof(arch::exception_frame_t) == RV32_FRAME_BYTES);

// C5 が arch template に渡す値: 粒度 128B (IDF SOC_CPU_PMP_REGION_GRANULARITY)、エントリ数 16 は推定。
static_assert(std::is_same_v<c5::chip::arch_type, shizuku::archs::rv32<16, 128, c5::clic_direct_mode>>);
static_assert(c5::chip::kPmpGranuleBytes == 128 && c5::chip::kPmpEntries == 16);
static_assert(std::is_same_v<arch::pmp_codec_t, shizuku::ports::riscv::pmp_codec<128>>);
static_assert(arch::PMP_GRANULE == 128);
static_assert(board::CORE_COUNT == 1);

int main() {
  // 128B 粒度: 64B の NAPOT / 4B 整列の TOR は表現できない (C6 の 4B 粒度では通る)。
  shizuku::ports::riscv::pmp_entry e;
  using C6codec = shizuku::ports::riscv::pmp_codec<4>;
  using namespace shizuku::ports::riscv;
  assert(!arch::pmp_codec_t::encode_napot(0x40800000, 64, pmp_perm::R, e));
  assert(C6codec::encode_napot(0x40800000, 64, pmp_perm::R, e));
  assert(arch::pmp_codec_t::encode_napot(0x40800000, 128, pmp_perm::R | pmp_perm::X, e));
  assert(!arch::pmp_codec_t::encode_tor(0x40800004, pmp_perm::R, e));
  assert(arch::pmp_codec_t::encode_tor(0x40800080, pmp_perm::R, e));
  return 0;
}
