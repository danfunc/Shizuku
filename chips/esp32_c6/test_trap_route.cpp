#include <array>
#include <cassert>
#include <initializer_list>

#define SHIZUKU_C6_SKELETON_PERMITTED 1
#include "chips/esp32_c6/rv32_c6.hpp"
#include "arch/riscv/ecall_table.hpp"
#include "arch/riscv/trap_route.hpp"
using namespace shizuku::ports::riscv;
// ECALL を番号で直接振り分ける表の検査 (ecall_table_consistent)。
namespace {
using fn_t = int (*)();
int other_fn() { return 0; }
int api_fn() { return 1; }
constexpr auto native_ids = [](size_t id) { return id == 2 || id == 5; };
constexpr std::array<fn_t, 6> good_table{&other_fn, &other_fn, &api_fn, &other_fn, &other_fn, &api_fn};
constexpr std::array<fn_t, 6> zero_native{&api_fn, &other_fn, &api_fn, &other_fn, &other_fn, &api_fn};
constexpr std::array<fn_t, 6> missing_native{&other_fn, &other_fn, &api_fn, &other_fn, &other_fn, &other_fn};
constexpr std::array<fn_t, 6> extra_native{&other_fn, &api_fn, &api_fn, &other_fn, &other_fn, &api_fn};
constexpr std::array<fn_t, 5> short_table{&other_fn, &other_fn, &api_fn, &other_fn, &other_fn};
static_assert(ecall_table_consistent(good_table, 6, &other_fn, native_ids));
// 0 番が other でない (範囲外の行き先が壊れる)
static_assert(!ecall_table_consistent(zero_native, 6, &other_fn, native_ids));
// handler の番号が other のまま / handler でない番号が専用関数
static_assert(!ecall_table_consistent(missing_native, 6, &other_fn, native_ids));
static_assert(!ecall_table_consistent(extra_native, 6, &other_fn, native_ids));
// 表の大きさが番号の数と違う (asm の範囲外判定が合わない)
static_assert(!ecall_table_consistent(short_table, 6, &other_fn, native_ids));
}
using frame_t = shizuku::archs::rv32_c6::exception_frame_t;
int main() {
  volatile uint32_t depth = 0;
  unsigned ecalls = 0, irqs = 0, faults = 0;
  for (uint32_t cause : {8u, 11u, 2u, 0x8000000bu, 0x80000008u}) {
    frame_t f{}; f.mcause = cause; f.mepc = 0x1000; f.mtval = 0x123456;
    route_trap(f, depth, [&] { ++ecalls; assert(depth == 0); },
      [&] { ++irqs; assert(depth == 1); },
      [&] { ++faults; assert(depth == 0); });
    assert(f.mepc == (cause == 8 || cause == 11 ? 0x1004u : 0x1000u));
    assert(f.mcause == cause && f.mtval == 0x123456);
    assert(depth == 0);
  }
  assert(ecalls == 2 && irqs == 2 && faults == 1);
  frame_t outer{}; outer.mcause = 0x80000001;
  route_trap(outer, depth, [] { assert(false); }, [&] {
    assert(depth == 1);
    frame_t inner{}; inner.mcause = 0x80000002;
    route_trap(inner, depth, [] { assert(false); }, [&] { assert(depth == 2); }, [] { assert(false); });
    assert(depth == 1);
    inner.mcause = 11;
    route_trap(inner, depth, [&] { assert(depth == 1); }, [] { assert(false); }, [] { assert(false); });
    inner.mcause = 5;
    route_trap(inner, depth, [] { assert(false); }, [] { assert(false); }, [&] { assert(depth == 1); });
    assert(depth == 1);
  }, [] { assert(false); });
  assert(depth == 0);
}
