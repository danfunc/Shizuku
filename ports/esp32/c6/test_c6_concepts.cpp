#include <cassert>
#include <cstdio>
#define SHIZUKU_C6_SKELETON_PERMITTED 1
#include "internal_headers/shizuku/archs/rv32_c6.hpp"
#include "internal_headers/shizuku/boards/esp32_c6.hpp"

using arch = shizuku::archs::rv32_c6;
using board = shizuku::boards::esp32_c6;

// static_assert による concept 検証
static_assert(shizuku::concepts::arch_requires<arch>,
              "rv32_c6 must strictly satisfy concepts::arch_requires");
static_assert(shizuku::concepts::board_requires<board>,
              "esp32_c6 must strictly satisfy concepts::board_requires");

// フレームサイズとレジスタ本数の検証: 18 words = 72 bytes
static_assert(sizeof(arch::exception_frame_t) == 72,
              "exception_frame_t must be exactly 72 bytes (18 RV32 words)");

static void test_rv32_c6_frame_abi() {
  arch::exception_frame_t frame{};
  const uintptr_t test_args[4] = {0x11111111, 0x22222222, 0x33333333, 0x44444444};

  // 1. set_args / arg ABI 検証 (dispatch.cpp:188, 332-335 と照合)
  arch::set_args(frame, test_args);
  assert(arch::arg(frame, 0) == 0x11111111u);
  assert(arch::arg(frame, 1) == 0x22222222u);
  assert(arch::arg(frame, 2) == 0x33333333u);
  assert(arch::arg(frame, 3) == 0x44444444u);

  // 2. arg(4) スロット検証 (dispatch.cpp:239 claim 読出しスロット: frame.a4)
  frame.a4 = 0x55555555;
  assert(arch::arg(frame, 4) == 0x55555555u);

  // 3. set_result ABI 検証 (a0 = error, a1 = value)
  arch::set_result(frame, 0x1234, 0x5678);
  assert(frame.a0 == 0x1234u);
  assert(frame.a1 == 0x5678u);

  // 4. set_entry ABI 検証 (mepc = pc, ra = lr)
  arch::set_entry(frame, 0x40801000, 0x40802000);
  assert(frame.mepc == 0x40801000u);
  assert(frame.ra == 0x40802000u);
  assert(arch::frame_pc(frame) == 0x40801000u);

  // 5. 文脈幾何検証 (psp_after_return が sp + sizeof(exception_frame_t) と一致)
  arch::context_t ctx{};
  ctx.sp = &frame;
  assert(arch::exc_frame_bytes(ctx) == 72u);
  assert(arch::psp_after_return(ctx) == reinterpret_cast<uintptr_t>(&frame) + 72u);

  // 6. 単核 board 整合性
  assert(board::CORE_COUNT == 1u);
  assert(board::core_num() == 0u);
}

int main() {
  printf("[TEST] Verifying C6 arch & board concepts...\n");
  printf("[TEST]   arch_requires<rv32_c6>: OK\n");
  printf("[TEST]   board_requires<esp32_c6>: OK\n");
  test_rv32_c6_frame_abi();
  printf("[TEST]   rv32_c6 frame ABI & geometry assertions: OK\n");
  printf("[TEST] All concept & ABI validations PASSED!\n");
  return 0;
}
