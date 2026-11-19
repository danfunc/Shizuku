// [STACK 方式] C6 の呼出しの記録 (caller の stack 枠) と stack 窓の host 試験。
//
// 組めた範囲: 記録の位置と窓の分離 (open_window)、記録の鎖の照合 (stack_record_chain_ok)、
// 窓のゼロ化 (zero_window) の 3 つを、実物の inline 関数で直接確かめる。
// 組めなかった範囲 (enter_in_place / pop_in_place を通した U→U の入場・戻り、u_depth の増減、
// 特権 caller の NO_STACK 拒否) は、カーネル一式 (dispatcher_v1 の harness) を STACK 版でビルドできないため
// ここでは試験しない。報告書に明記する。
//
// 方式は rv32 の context_t::CALL_IN_PLACE (= STACK 方式) で決まる。

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "shizuku/native_call.hpp"
#include "shizuku/stack_window.hpp"
#include "shizuku/templates/thread.hpp"

namespace {

using shizuku::open_window;
using shizuku::stack_window;

// 記録 (stack_call_record) と同じ 32B の形。stack_record_chain_ok が見るのは depth と prev だけ。
struct FakeRecord {
  uintptr_t prev;
  uintptr_t prev_call;
  uintptr_t ret_pc;
  uintptr_t handler_entry;
  uint32_t generation;
  uint16_t object;
  uint16_t handler_object;
  uint16_t unwind_floor;
  uint16_t depth;
  uint8_t kind;
  uint8_t via;
  uint16_t reserved;
};
// 注: host (64bit) では uintptr_t が 8B のため sizeof は 48 になる。32B の確認は RV32 (実機) の
// thread.hpp の static_assert が受け持つ。ここでは depth と prev の位置だけを使う。

struct FakeCallStack {
  uintptr_t top = 0;
  uint32_t depth = 0;
};

struct FakeThread {
  using call_ledger_entry = FakeRecord;
  FakeCallStack call_stack;
};

// 試験ごとに同じ書式で記録を置く。depth と prev は呼び元が与える。
void put_record(FakeRecord *at, uintptr_t prev, uint16_t depth) {
  std::memset(at, 0, sizeof(*at));
  at->prev = prev;
  at->depth = depth;
}

// 1) 記録の位置は窓の外 (上) にある。
//    open_window は hi = ret_sp & ~15 を窓の上端にする。記録は [ret_sp, ret_sp+32) なので、
//    ret_sp が 16B 整列でも非整列でも、記録の先頭は hi 以上で窓の外になる。
void test_record_is_outside_window() {
  const uintptr_t stack_lo = 0x20000000u;
  const uintptr_t stack_hi = 0x20001000u;

  // 整列した ret_sp
  {
    const uintptr_t ret_sp = 0x20000800u;
    const stack_window win = open_window(ret_sp, stack_lo);
    assert(win.is_valid());
    assert(win.hi == 0x20000800u);
    assert(win.lo == 0x20000700u); // hi - K (K = 256)
    const uintptr_t rec = ret_sp; // CALL_RECORD_OFFSET = 0
    assert(!(rec >= win.lo && rec < win.hi));
    assert(rec >= win.hi);
    assert(rec + 32u <= stack_hi);
  }
  // 非整列の ret_sp (窓の上端は 16B 切り下げ、記録は ret_sp そのもの)
  {
    const uintptr_t ret_sp = 0x2000080Cu;
    const stack_window win = open_window(ret_sp, stack_lo);
    assert(win.is_valid());
    assert(win.hi == 0x20000800u);
    const uintptr_t rec = ret_sp;
    assert(rec >= win.hi); // 0x2000080C >= 0x20000800 (記録は窓より上)
    assert(!(rec >= win.lo && rec < win.hi));
  }
  // 余地がちょうど K のとき: 窓は [stack_lo, hi) に一致し、記録は窓の上
  {
    const uintptr_t ret_sp = 0x20000100u;
    const stack_window win = open_window(ret_sp, stack_lo);
    assert(win.is_valid());
    assert(win.lo == stack_lo); // hi - stack_lo = K
    assert(win.hi == 0x20000100u);
    assert(ret_sp >= win.hi);
  }
  std::printf("  1. record at ret_sp lies outside the callee window (aligned and unaligned ret_sp): PASS\n");
}

// 2) 記録の鎖 (depth と prev) の照合。戻りの前に必ず調べる。改ざんは検出され、拒否される。
void test_record_chain_detects_tamper() {
  using shizuku::stack_record_chain_ok;

  alignas(16) FakeRecord recs[3];
  FakeThread thread;

  // 空: depth 0 かつ top = 0 なら正しい
  assert(stack_record_chain_ok(thread));
  thread.call_stack.top = 1; // 空なのに top が非 0
  assert(!stack_record_chain_ok(thread));
  thread.call_stack.top = 0;

  // 2 段: recs[0] が外側 (depth 1, prev 0)、recs[1] が内側 (depth 2, prev -> recs[0])
  put_record(&recs[0], 0, 1);
  put_record(&recs[1], reinterpret_cast<uintptr_t>(&recs[0]), 2);
  thread.call_stack.depth = 2;
  thread.call_stack.top = reinterpret_cast<uintptr_t>(&recs[1]);
  assert(stack_record_chain_ok(thread));

  // 改ざん 1: 先頭の depth を書き換える (カーネルの depth と食い違う)
  recs[1].depth = 3;
  assert(!stack_record_chain_ok(thread));
  recs[1].depth = 2;
  assert(stack_record_chain_ok(thread));

  // 改ざん 2: prev の指す外側の記録の depth を壊す (鎖の段の連続が崩れる)
  recs[0].depth = 5;
  assert(!stack_record_chain_ok(thread));
  recs[0].depth = 1;
  assert(stack_record_chain_ok(thread));

  // 改ざん 3: prev を 0 にする (depth 2 なのに外側が無い)
  recs[1].prev = 0;
  assert(!stack_record_chain_ok(thread));
  recs[1].prev = reinterpret_cast<uintptr_t>(&recs[0]);
  assert(stack_record_chain_ok(thread));

  // 改ざん 4: depth 1 なのに prev が非 0 (最外段は prev = 0 のはず)
  thread.call_stack.depth = 1;
  thread.call_stack.top = reinterpret_cast<uintptr_t>(&recs[0]);
  recs[0].prev = reinterpret_cast<uintptr_t>(&recs[1]);
  assert(!stack_record_chain_ok(thread));
  recs[0].prev = 0;
  assert(stack_record_chain_ok(thread));

  std::printf("  2. chain check rejects depth/prev tampering (4 cases) and accepts the clean chain: PASS\n");
}

// 3) 窓のゼロ化: [lo, hi) だけがゼロになり、外側は変わらない。
void test_zero_window_only_inside() {
  alignas(16) static uint8_t buf[512];
  std::memset(buf, 0xA5, sizeof(buf));
  const uintptr_t lo = reinterpret_cast<uintptr_t>(&buf[128]);
  const uintptr_t hi = reinterpret_cast<uintptr_t>(&buf[384]);
  shizuku::zero_window(stack_window{lo, hi});

  for (size_t i = 0; i < sizeof(buf); ++i) {
    if (i >= 128 && i < 384) {
      assert(buf[i] == 0);
    } else {
      assert(buf[i] == 0xA5);
    }
  }
  std::printf("  3. zero_window clears exactly [lo, hi) and leaves the bytes outside it unchanged: PASS\n");
}

} // namespace

int main() {
  test_record_is_outside_window();
  test_record_chain_detects_tamper();
  test_zero_window_only_inside();
  std::printf("stack_ledger_window_test: 3 PASS\n");
  return 0;
}
