// stack 窓の純関数 (open_window / extend_window / zero_window) と、thread の stack 登録の試験。
// 旧版は LEAN の台帳 (win_span / ledger[].ret_sp) を持つ mock で enter_in_place / pop_in_place を通した試験を
// 含んでいたが、LEAN の廃止とともに消した。STACK の窓は thread.stk の cur_win と u_depth で持ち、記録は caller の
// stack 枠にあるため、同じ mock では動かない。STACK 側の対応は tests/esp32_c6/stack_ledger_window_test.cpp。
#include <cassert>
#include <cstdint>
#include <array>
#include <type_traits>
#include <vector>

#include "shizuku/kernel_abi.hpp"
#include "shizuku/object_api.hpp"
#include "shizuku/stack_window.hpp"
#include "shizuku/templates/thread.hpp"

// ===========================================================================
// Pure function tests
// ===========================================================================

static void test_open_window() {
  using namespace shizuku;

  // Normal open with sufficient space
  auto win1 = open_window(0x20002000u, 0x20000000u);
  assert(win1.is_valid());
  assert(win1.hi == 0x20002000u);
  assert(win1.lo == 0x20001F00u);
  assert(win1.size() == 256u);

  // Unaligned caller_sp rounded down to 16B alignment
  auto win2 = open_window(0x2000200Fu, 0x20000000u);
  assert(win2.is_valid());
  assert(win2.hi == 0x20002000u);
  assert(win2.lo == 0x20001F00u);

  // 仕様変更 (段 C-0): U 復帰時に resume frame (128B) が caller の ret_sp-128 に
  // 作られるため、最小窓幅がそれより小さいとゼロ化済みの外に frame の未設定語が
  // 残りうる。このため kStackWindowMin = 256 (= K) に引き上げられ、
  // hi - stack_lo < 256 の場合は無効 (NO_STACK) となる。

  // Remaining space less than 256B (e.g. 128B): now invalid
  auto win3 = open_window(0x20000080u, 0x20000000u);
  assert(!win3.is_valid());

  // Remaining space 16B: now invalid (previously minimum boundary)
  auto win4 = open_window(0x20000010u, 0x20000000u);
  assert(!win4.is_valid());

  // Boundary: exactly 256B space is valid
  auto win_boundary = open_window(0x20000100u, 0x20000000u);
  assert(win_boundary.is_valid());
  assert(win_boundary.hi == 0x20000100u);
  assert(win_boundary.lo == 0x20000000u);
  assert(win_boundary.size() == 256u);

  // Just below boundary: 240B space is invalid
  auto win_below_boundary = open_window(0x200000F0u, 0x20000000u);
  assert(!win_below_boundary.is_valid());

  // Insufficient space: less than 16B
  auto win_invalid1 = open_window(0x2000000Fu, 0x20000000u);
  assert(!win_invalid1.is_valid());

  // Insufficient space: caller_sp <= stack_lo
  auto win_invalid2 = open_window(0x20000000u, 0x20000000u);
  assert(!win_invalid2.is_valid());

  auto win_invalid3 = open_window(0x1FFFF000u, 0x20000000u);
  assert(!win_invalid3.is_valid());

  // ARM default: stack_lo == 0 with typical SRAM caller_sp (sufficient space)
  auto win_arm = open_window(0x20010000u, 0u);
  assert(win_arm.is_valid());
  assert(win_arm.hi == 0x20010000u);
  assert(win_arm.lo == 0x2000FF00u);
  assert(win_arm.size() == 256u);

  // ARM default: stack_lo == 0 but caller_sp < kStackWindowMin (256B) is invalid
  auto win_arm_too_small = open_window(0x80u, 0u);
  assert(!win_arm_too_small.is_valid());
}

static void test_extend_window() {
  using namespace shizuku;

  const uintptr_t stack_lo = 0x20000000u;
  const auto initial = open_window(0x20002000u, stack_lo);
  assert(initial.is_valid());
  assert(initial.lo == 0x20001F00u && initial.hi == 0x20002000u);

  // 1. Fault within valid extend region: stack_lo <= a < win.lo
  auto ext1 = extend_window(initial, 0x20001E80u, stack_lo);
  assert(ext1.is_valid());
  assert(ext1.hi == initial.hi);
  assert(ext1.lo == 0x20001E00u);

  // 2. Fault deeper than 1 step:
  auto ext2 = extend_window(initial, 0x20001A03u, stack_lo);
  assert(ext2.is_valid());
  assert(ext2.hi == initial.hi);
  assert(ext2.lo == 0x20001A00u);

  // 3. Fault deeper: a & ~3 becomes new_lo when smaller than win.lo - K
  auto ext3 = extend_window(initial, 0x20000020u, stack_lo);
  assert(ext3.is_valid());
  assert(ext3.hi == initial.hi);
  assert(ext3.lo == 0x20000020u);

  // 4. Fault near bottom when win.lo - K < stack_lo (clamps to stack_lo):
  stack_window near_bottom{stack_lo + 100u, initial.hi};
  auto ext4 = extend_window(near_bottom, stack_lo + 50u, stack_lo);
  assert(ext4.is_valid());
  assert(ext4.hi == initial.hi);
  assert(ext4.lo == stack_lo);

  // 5. Fault at stack_lo exact boundary:
  auto ext5 = extend_window(initial, stack_lo, stack_lo);
  assert(ext5.is_valid());
  assert(ext5.hi == initial.hi);
  assert(ext5.lo == stack_lo);

  // 6. Rejection: a < stack_lo (true stack overflow)
  auto rej_underflow = extend_window(initial, stack_lo - 4u, stack_lo);
  assert(!rej_underflow.is_valid());

  // 7. Rejection: a >= win.lo (inside already opened window)
  auto rej_inside = extend_window(initial, initial.lo + 4u, stack_lo);
  assert(!rej_inside.is_valid());

  // 8. Rejection: a >= win.hi (caller frame violation) - exact boundary and above
  auto rej_caller_exact = extend_window(initial, initial.hi, stack_lo);
  assert(!rej_caller_exact.is_valid());

  auto rej_caller = extend_window(initial, initial.hi + 4u, stack_lo);
  assert(!rej_caller.is_valid());

  auto rej_caller_far = extend_window(initial, initial.hi + 0x1000u, stack_lo);
  assert(!rej_caller_far.is_valid());
}

static void test_zero_window() {
  using namespace shizuku;

  alignas(16) std::array<uint32_t, 128> buffer;
  buffer.fill(0xDEADBEEFu);

  const uintptr_t buf_start = reinterpret_cast<uintptr_t>(buffer.data());
  const uintptr_t zero_lo = buf_start + 64u;
  const uintptr_t zero_hi = buf_start + 192u;

  stack_window win{zero_lo, zero_hi};
  zero_window(win);

  for (uintptr_t p = buf_start; p < zero_lo; p += 4) {
    assert(*reinterpret_cast<uint32_t *>(p) == 0xDEADBEEFu);
  }
  for (uintptr_t p = zero_lo; p < zero_hi; p += 4) {
    assert(*reinterpret_cast<uint32_t *>(p) == 0u);
  }
  for (uintptr_t p = zero_hi; p < buf_start + sizeof(buffer); p += 4) {
    assert(*reinterpret_cast<uint32_t *>(p) == 0xDEADBEEFu);
  }
}

// zero_window_unrolled32 (RV32 の展開ゼロ化) の範囲: 任意の 4B 整列 lo と長さで [lo, hi) ちょうど。
// host では volatile 4B store の経路で、意味 (どこを 0 にするか) を固定する。asm の展開ループ自体は
// 実機/QEMU (rv32 virt) で同じ全組合せを実行して確かめた (報告書に実行コマンドを記載)。
static void test_zero_window_unrolled32() {
  using namespace shizuku;
  alignas(16) std::array<uint32_t, 256> buffer;
  const uintptr_t base = reinterpret_cast<uintptr_t>(buffer.data());
  for (uint32_t lo = 0; lo < 40; ++lo) {
    for (uint32_t len = 0; len <= 160; ++len) {
      buffer.fill(0xFFFFFFFFu);
      zero_window_unrolled32({base + lo * 4u, base + (lo + len) * 4u});
      for (uint32_t i = 0; i < buffer.size(); ++i) {
        const bool in = i >= lo && i < lo + len;
        assert(buffer[i] == (in ? 0u : 0xFFFFFFFFu));
      }
    }
  }
  buffer.fill(0xFFFFFFFFu);
  zero_window_unrolled32({base + 80u, base + 40u});  // lo > hi: 何も書かない
  zero_window_unrolled32({base + 40u, base + 40u});  // 空
  for (auto w : buffer) assert(w == 0xFFFFFFFFu);
}

struct mock_context {
  static constexpr bool HAS_STACK_WINDOW = true;
  uintptr_t user_sp = 0;
  bool privileged = false;
  uintptr_t region_base = 0;
  uintptr_t region_limit = 0;
};
using mock_thread = shizuku::templates::thread<mock_context>;

// thread の stack 登録 (spawn / bootstrap が set_stack_bounds を呼ぶ): lo は 8B 切上げ、hi は 8B 切下げ、
// 窓は全域に戻る。
static void test_set_stack_bounds_alignment() {
  mock_thread thread{};
  thread.set_stack_bounds(0x20000003u, 0x20001005u);
  assert(thread.stk.stack_lo == 0x20000008u);
  assert(thread.stk.stack_hi == 0x20001000u);
  assert(thread.stk.cur_win_lo == thread.stk.stack_lo);
  assert(thread.stk.cur_win_hi == thread.stk.stack_hi);
  // 8B 整列済みならそのまま
  thread.set_stack_bounds(0x20002000u, 0x20003000u);
  assert(thread.stk.stack_lo == 0x20002000u && thread.stk.stack_hi == 0x20003000u);
  // 狭すぎて整列後に hi < lo なら空 (hi = lo)
  thread.set_stack_bounds(0x20004001u, 0x20004006u);
  assert(thread.stk.stack_hi >= thread.stk.stack_lo);
}

int main() {
  test_open_window();
  test_extend_window();
  test_zero_window();
  test_zero_window_unrolled32();
  test_set_stack_bounds_alignment();
  return 0;
}
