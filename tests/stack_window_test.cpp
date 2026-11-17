// stack 窓の試験: 純関数 (open_window / extend_window / zero_window) と、STACK 方式の enter_in_place /
// pop_in_place を実メモリの stack 上で通した試験。
//
// 方式は mock の context の CALL_IN_PLACE (= STACK 方式) で決まる。STACK の窓は thread.stk の
// cur_win_{lo,hi} と u_depth だけで持ち (段ごとの記帳は無い)、呼出しの記録は caller の ecall 時点の SP の枠
// [return_sp + CALL_RECORD_OFFSET, +sizeof(record)) に置かれる。この試験の mock arch は CALL_RECORD_OFFSET = 0 (RV32 相当)。
// 旧版 (LEAN) の窓の検査との対応は report4d.md に表で残した。
#include <cassert>
#include <cstdint>
#include <array>
#include <type_traits>
#include <vector>

#include "shizuku/kernel_abi.hpp"
#include "shizuku/object_api.hpp"
#include "shizuku/stack_window.hpp"
#include "shizuku/templates/thread.hpp"
#include "shizuku/native_call.hpp"

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


// ===========================================================================
// Mock arch / kernel (STACK の記録 + HAS_STACK_WINDOW)
// ===========================================================================
struct mock_frame {
  uintptr_t pc = 0;
  uintptr_t ra = 0;
  uintptr_t sp = 0;
};

struct mock_context {
  static constexpr bool CALL_IN_PLACE = true;
  static constexpr bool HAS_STACK_WINDOW = true;
  mock_frame frame{};
  mock_frame *sp = &frame;
  uintptr_t user_sp = 0;
  bool privileged = false;
  uintptr_t region_base = 0;
  uintptr_t region_limit = 0;
};

struct test_fake_arch {
  static constexpr bool CALL_IN_PLACE = true;
  static constexpr uintptr_t CALL_RECORD_OFFSET = 0;
  static constexpr uintptr_t CALL_RECORD_BYTES = 32;
  static constexpr uintptr_t record_return_sp(uintptr_t record) { return record - CALL_RECORD_OFFSET; }

  static inline std::vector<shizuku::stack_window> zeroed_windows;
  static inline bool record_room = true;

  static void reset() {
    zeroed_windows.clear();
    record_room = true;
  }

  static void zero_stack_window(shizuku::stack_window win) {
    zeroed_windows.push_back(win);
    shizuku::zero_window(win);
  }

  // 本物の判定は stack の天井・下限を見る。この mock では試験が真偽を決める (false で NO_STACK の経路を通す)。
  static bool call_record_room_at(const mock_context &, uintptr_t) { return record_room; }
  static uintptr_t frame_pc(const mock_frame &f) { return f.pc; }
  static uintptr_t call_return_sp(const mock_context &c) { return c.user_sp; }
  static bool context_privileged(const mock_context &c) { return c.privileged; }
  static void set_entry(mock_frame &f, uintptr_t pc, uintptr_t ra) {
    f.pc = pc;
    f.ra = ra;
  }
  static uintptr_t return_stub() { return 0x1000u; }
  static void set_priv(mock_context &c, bool priv) { c.privileged = priv; }
  static void set_region_window(mock_context &c, uintptr_t b, uintptr_t l) {
    c.region_base = b;
    c.region_limit = l;
  }
  static void switch_callee_protection(mock_context &, mock_frame &, uint32_t, bool) {}
  static mock_frame *call_resume_at(mock_context &c, uintptr_t pc, uintptr_t sp, bool priv,
                                    uintptr_t, uintptr_t, bool) {
    c.frame.pc = pc;
    c.user_sp = sp;
    c.privileged = priv;
    return &c.frame;
  }
};

struct mock_kobj {
  struct callee_view {
    uint32_t id = 0;
    uint32_t parent_handler_object = 0;
    uintptr_t parent_handler_entry = 0;
    shizuku::object_kind kind = shizuku::object_kind::PLAIN;
    bool privileged = false;
    uintptr_t region_base = 0;
    uintptr_t region_limit = 0;
    uint32_t instance_generation = 1;
  };

  // STACK の戻りは、再開する caller の特権と、U callee の窓を持つ段かを object の表の flags から引く。
  bool m_privileged[16]{};
  bool privileged_of(uintptr_t id) const { return id < 16 && m_privileged[id]; }

  shizuku::native::return_region_window return_region(uint32_t) const {
    return {0, 0};
  }
};

struct mock_kernel {
  using ARCH = test_fake_arch;
  using CONTEXT = mock_context;
  using FRAME = mock_frame;
  using THREAD = shizuku::templates::thread<mock_context>;

  static constexpr uint8_t LEDGER_CALL = 1;
  static constexpr uint8_t LEDGER_DISPATCH = 2;
  static constexpr uint8_t LEDGER_DESCRIPTOR_CALL = 3;

  uint32_t svc_handler_object() const { return 0xFFFFu; }
};

using Thread = mock_kernel::THREAD;
using Record = Thread::call_ledger_entry;
static constexpr uintptr_t K = shizuku::kStackWindowStep;
// caller の枠の上 (記録の枠より上) に置く目印の位置。host (64bit) の記録は 32B より大きいので余裕を取る。
static constexpr uintptr_t kAboveRecord = 64;
static_assert(sizeof(Record) <= kAboveRecord);

// 深さ index + 1 の記録 (先頭から prev を辿る)。
static Record &rec_at(Thread &t, uint32_t index) {
  auto *r = reinterpret_cast<Record *>(t.call_stack.top);
  for (uint32_t n = 0; r != nullptr && r->depth != index + 1u && n < 64u; ++n)
    r = reinterpret_cast<Record *>(r->prev);
  assert(r != nullptr && r->depth == index + 1u);
  return *r;
}
static uintptr_t addr_of(const Record &r) { return reinterpret_cast<uintptr_t>(&r); }

static shizuku::kernel_error enter(mock_kernel &kernel, Thread &thread, mock_context &context,
                                   const mock_kobj::callee_view &view, uintptr_t pc) {
  return shizuku::native::enter_in_place<mock_kernel, mock_kobj>(
      kernel, thread, context, view, pc, mock_kernel::LEDGER_CALL);
}
static bool pop(mock_kobj &kobj, Thread &thread, mock_context &context) {
  mock_frame *res = nullptr;
  return shizuku::native::pop_in_place<mock_kernel, mock_kobj>(kobj, thread, context, res, true, false);
}
static shizuku::stack_window cur_win(const Thread &t) {
  return {t.stk.cur_win_lo, t.stk.cur_win_hi};
}
static bool same_win(shizuku::stack_window a, shizuku::stack_window b) {
  return a.lo == b.lo && a.hi == b.hi;
}
static void assert_zero(shizuku::stack_window w) {
  for (uintptr_t p = w.lo; p < w.hi; p += 4)
    assert(*reinterpret_cast<const volatile uint32_t *>(p) == 0);
}
static void put(uintptr_t addr, uint32_t v) { *reinterpret_cast<volatile uint32_t *>(addr) = v; }
static uint32_t get(uintptr_t addr) { return *reinterpret_cast<const volatile uint32_t *>(addr); }

// thread の stack 登録 (spawn / bootstrap が set_stack_bounds を呼ぶ): lo は 8B 切上げ、hi は 8B 切下げ、
// 窓は全域に戻る。
static void test_set_stack_bounds_alignment() {
  Thread thread{};
  thread.set_stack_bounds(0x20000003u, 0x20001005u);
  assert(thread.stk.stack_lo == 0x20000008u);
  assert(thread.stk.stack_hi == 0x20001000u);
  assert(thread.stk.cur_win_lo == thread.stk.stack_lo);
  assert(thread.stk.cur_win_hi == thread.stk.stack_hi);
  assert(thread.stk.u_depth == 0);
  // 8B 整列済みならそのまま
  thread.set_stack_bounds(0x20002000u, 0x20003000u);
  assert(thread.stk.stack_lo == 0x20002000u && thread.stk.stack_hi == 0x20003000u);
  // 狭すぎて整列後に hi < lo なら空 (hi = lo)
  thread.set_stack_bounds(0x20004001u, 0x20004006u);
  assert(thread.stk.stack_hi >= thread.stk.stack_lo);
}

// ===========================================================================
// A -> A の入れ子。窓は caller の SP の下に開き、戻りで開いた分だけゼロ化され、外側の窓へ戻る。
//   STACK の外側の窓の戻し方: [戻った段の ret_sp - K, 外側の段の ret_sp)。旧 (LEAN) は入場時に保存した窓へ戻していた。
// ===========================================================================
static void test_stack_nesting_a_a() {
  test_fake_arch::reset();
  mock_kernel kernel{};
  mock_kobj kobj{};
  Thread thread{};
  mock_context context{};
  thread.base_object = 1;

  alignas(16) static uint8_t stack_mem[0x4000];
  const uintptr_t lo = reinterpret_cast<uintptr_t>(stack_mem);
  const uintptr_t hi = lo + sizeof(stack_mem);
  thread.set_user_stack(lo, hi);

  const uintptr_t s0 = lo + 0x3000u;
  context.user_sp = s0;
  context.frame.pc = 0x42000010u;
  // 記録の枠 [s0, s0 + sizeof(record)) の上に目印 (窓の外、記録の外)。
  put(s0 + kAboveRecord, 0xC001CAFEu);
  put(s0 + kAboveRecord + 16u, 0xC001BEEFu);

  mock_kobj::callee_view view_a{};
  view_a.id = 2;

  // 1. 最初の A
  assert(enter(kernel, thread, context, view_a, 0x42001000u) == shizuku::kernel_error::OK);
  assert(thread.call_stack.depth == 1 && thread.call_stack.call_count == 1);
  assert(thread.stk.u_depth == 1);
  Record &r0 = rec_at(thread, 0);
  assert(addr_of(r0) == s0); // 記録は caller の ecall 時点の SP の枠
  assert(test_fake_arch::record_return_sp(addr_of(r0)) == s0);
  assert(r0.ret_pc == 0x42000010u && r0.object == 2 && r0.via == mock_kernel::LEDGER_CALL);
  assert(r0.prev == 0 && r0.prev_call == 0 && r0.depth == 1);
  assert(thread.call_stack.top == s0 && thread.call_stack.call_top_rec == s0);
  assert(thread.call_stack.base_privileged == 0);
  const auto outer = cur_win(thread);
  assert(outer.hi == s0 && outer.lo == s0 - K);

  // A が窓に書き、128B の frame を使う
  put(outer.hi - 32u, 0xA1A1A1A1u);
  put(outer.lo + 16u, 0xA1A10001u);
  context.user_sp = outer.hi - 128u;
  context.frame.pc = 0x42001020u;

  // 2. A -> A
  assert(enter(kernel, thread, context, view_a, 0x42001000u) == shizuku::kernel_error::OK);
  assert(thread.call_stack.depth == 2 && thread.call_stack.call_count == 2);
  assert(thread.stk.u_depth == 2);
  const auto inner = cur_win(thread);
  assert(inner.hi == outer.hi - 128u && inner.lo == inner.hi - K);
  assert(inner.hi <= outer.hi - 128u); // 入れ子の窓は重ならず、外側の frame を残す
  Record &r1 = rec_at(thread, 1);
  assert(addr_of(r1) == outer.hi - 128u);
  assert(r1.prev == addr_of(r0) && r1.prev_call == addr_of(r0)); // 鎖
  assert(r1.ret_pc == 0x42001020u && r1.depth == 2);
  assert(thread.call_stack.top == addr_of(r1) && thread.call_stack.call_top_rec == addr_of(r1));
  put(inner.hi - 16u, 0xA2A2A2A2u);
  put(inner.lo + 16u, 0xA2A20002u);

  // 3. 内側を戻る
  assert(pop(kobj, thread, context));
  assert(thread.call_stack.depth == 1 && thread.call_stack.call_count == 1 && thread.stk.u_depth == 1);
  assert(thread.call_stack.top == addr_of(r0) && thread.call_stack.call_top_rec == addr_of(r0));
  assert(test_fake_arch::zeroed_windows.size() == 1);
  assert(same_win(test_fake_arch::zeroed_windows[0], inner)); // 開いた窓だけがゼロ化される
  assert_zero(inner);
  assert(get(outer.hi - 32u) == 0xA1A1A1A1u); // 外側の frame は残る
  assert(context.frame.pc == 0x42001020u && context.user_sp == outer.hi - 128u); // caller が再開する
  assert(!context.privileged);
  const shizuku::stack_window restored{outer.hi - 128u - K, outer.hi};
  assert(same_win(cur_win(thread), restored));

  // 4. 外側を戻る
  assert(pop(kobj, thread, context));
  assert(thread.call_stack.depth == 0 && thread.call_stack.call_count == 0 && thread.stk.u_depth == 0);
  assert(thread.call_stack.top == 0 && thread.call_stack.call_top_rec == 0);
  assert(test_fake_arch::zeroed_windows.size() == 2);
  assert(same_win(test_fake_arch::zeroed_windows[1], restored));
  assert_zero(restored);
  assert(get(s0 + kAboveRecord) == 0xC001CAFEu && get(s0 + kAboveRecord + 16u) == 0xC001BEEFu);
  assert(cur_win(thread).lo == lo && cur_win(thread).hi == hi); // stack 全域へ戻る
  assert(context.frame.pc == 0x42000010u && context.user_sp == s0);
}

// ===========================================================================
// A -> B -> A の入れ子 + 窓の伸長。伸ばした分も戻りでゼロ化される。
// ===========================================================================
static void test_stack_nesting_a_b_a() {
  test_fake_arch::reset();
  mock_kernel kernel{};
  mock_kobj kobj{};
  Thread thread{};
  mock_context context{};
  thread.base_object = 1;

  alignas(16) static uint8_t stack_mem[0x4000];
  const uintptr_t lo = reinterpret_cast<uintptr_t>(stack_mem);
  const uintptr_t hi = lo + sizeof(stack_mem);
  thread.set_user_stack(lo, hi);

  const uintptr_t s0 = lo + 0x3000u;
  context.user_sp = s0;
  put(s0 + kAboveRecord, 0xC001CAFEu);
  mock_kobj::callee_view view_a{};
  view_a.id = 2;
  mock_kobj::callee_view view_b{};
  view_b.id = 3;

  assert(enter(kernel, thread, context, view_a, 0x42001000u) == shizuku::kernel_error::OK);
  const auto w0 = cur_win(thread);
  put(w0.hi - 32u, 0xA1A1A1A1u);
  put(w0.lo + 16u, 0xA1A10001u);

  context.user_sp = w0.hi - 160u;
  assert(enter(kernel, thread, context, view_b, 0x42002000u) == shizuku::kernel_error::OK);
  const auto w1 = cur_win(thread);
  put(w1.hi - 32u, 0xBBBBBBBBu);
  put(w1.lo + 16u, 0xBBBB0001u);

  context.user_sp = w1.hi - 160u;
  assert(enter(kernel, thread, context, view_a, 0x42001000u) == shizuku::kernel_error::OK);
  assert(thread.call_stack.depth == 3 && thread.stk.u_depth == 3);
  const auto w2 = cur_win(thread);
  assert(w2.hi < w1.hi && w1.hi < w0.hi);
  assert(w2.hi <= w1.hi - 160u);
  assert(rec_at(thread, 2).prev_call == addr_of(rec_at(thread, 1)));
  assert(rec_at(thread, 1).prev_call == addr_of(rec_at(thread, 0)));

  // 最内の A が stack fault を起こし、窓が下へ伸びる (trap 側が cur_win_lo を更新する)
  const uintptr_t fault_addr = w2.lo - 100u;
  const auto extended = shizuku::extend_window(w2, fault_addr, lo);
  assert(extended.is_valid() && extended.lo < w2.lo && extended.hi == w2.hi);
  thread.stk.cur_win_lo = extended.lo;
  put(extended.lo + 4u, 0xE7E7E7E7u);
  put(extended.hi - 16u, 0xA2A2A2A2u);

  // 内側の A を戻る: 伸ばした範囲がゼロ化される
  assert(pop(kobj, thread, context));
  assert(thread.call_stack.depth == 2 && thread.stk.u_depth == 2);
  assert(test_fake_arch::zeroed_windows.size() == 1);
  assert(same_win(test_fake_arch::zeroed_windows[0], extended));
  assert_zero(extended);
  assert(get(w1.hi - 32u) == 0xBBBBBBBBu); // B の frame は残る
  const shizuku::stack_window back_in_b{w1.hi - 160u - K, w1.hi};
  assert(same_win(cur_win(thread), back_in_b));

  // B を戻る
  assert(pop(kobj, thread, context));
  assert(thread.call_stack.depth == 1 && thread.stk.u_depth == 1);
  assert(test_fake_arch::zeroed_windows.size() == 2);
  assert(same_win(test_fake_arch::zeroed_windows[1], back_in_b));
  assert_zero(back_in_b);
  assert(get(w0.hi - 32u) == 0xA1A1A1A1u); // 外側の A の frame は残る
  const shizuku::stack_window back_in_a{w0.hi - 160u - K, w0.hi};
  assert(same_win(cur_win(thread), back_in_a));

  // 外側の A を戻る
  assert(pop(kobj, thread, context));
  assert(thread.call_stack.depth == 0 && thread.stk.u_depth == 0);
  assert(test_fake_arch::zeroed_windows.size() == 3);
  assert(same_win(test_fake_arch::zeroed_windows[2], back_in_a));
  assert_zero(back_in_a);
  assert(get(s0 + kAboveRecord) == 0xC001CAFEu);
  assert(cur_win(thread).lo == lo && cur_win(thread).hi == hi);
}

// ===========================================================================
// 巻き戻し (複数段をまとめて戻る): 戻る段ごとにそのときの窓がゼロ化され、最後の段の caller だけが再開する。
// ===========================================================================
static void test_stack_unwind_in_place() {
  test_fake_arch::reset();
  mock_kernel kernel{};
  mock_kobj kobj{};
  Thread thread{};
  mock_context context{};
  thread.base_object = 1;

  alignas(16) static uint8_t stack_mem[0x4000];
  const uintptr_t lo = reinterpret_cast<uintptr_t>(stack_mem);
  const uintptr_t hi = lo + sizeof(stack_mem);
  thread.set_user_stack(lo, hi);

  const uintptr_t s0 = lo + 0x3000u;
  context.user_sp = s0;
  context.frame.pc = 0x42000010u;
  put(s0 + kAboveRecord, 0xC001CAFEu);
  mock_kobj::callee_view view{};
  view.id = 2;

  assert(enter(kernel, thread, context, view, 0x1000u) == shizuku::kernel_error::OK);
  const auto win0 = cur_win(thread);
  put(win0.hi - 32u, 0x11111111u);
  const uintptr_t s1 = win0.hi - 160u;
  context.user_sp = s1;
  assert(enter(kernel, thread, context, view, 0x1000u) == shizuku::kernel_error::OK);
  const auto win1 = cur_win(thread);
  put(win1.hi - 32u, 0x22222222u);
  put(win1.lo + 16u, 0x22220001u);
  const uintptr_t s2 = win1.hi - 160u;
  context.user_sp = s2;
  assert(enter(kernel, thread, context, view, 0x1000u) == shizuku::kernel_error::OK);
  const auto win2 = cur_win(thread);
  put(win2.hi - 32u, 0x33333333u);
  put(win2.lo + 16u, 0x33330001u);
  assert(thread.call_stack.depth == 3 && thread.stk.u_depth == 3);

  // 2 段まとめて戻る
  mock_frame *res = nullptr;
  assert((shizuku::native::unwind_in_place<mock_kernel, mock_kobj>(kobj, thread, context, res, 2, true)));
  assert(thread.call_stack.depth == 1 && thread.call_stack.call_count == 1 && thread.stk.u_depth == 1);
  assert(test_fake_arch::zeroed_windows.size() == 2);
  // 1 段目: 最内の窓。2 段目: 1 段目を戻した後に取り直した窓 [s2 - K, s1)。どちらも実メモリがゼロ。
  assert(same_win(test_fake_arch::zeroed_windows[0], win2));
  const shizuku::stack_window mid{s2 - K, s1};
  assert(same_win(test_fake_arch::zeroed_windows[1], mid));
  assert_zero(win2);
  assert_zero(mid);
  assert(get(win0.hi - 32u) == 0x11111111u); // 残る段の frame
  assert(get(s0 + kAboveRecord) == 0xC001CAFEu);
  assert(same_win(cur_win(thread), shizuku::stack_window{s1 - K, s0}));
  assert(context.user_sp == s1); // 再開するのは最後に落とした段の caller
}

// ===========================================================================
// 断る場合: 何も積まず、窓も動かさない。
// ===========================================================================
static void assert_untouched(const Thread &thread, uint32_t depth, uint32_t u_depth, uintptr_t top,
                             shizuku::stack_window win) {
  assert(thread.call_stack.depth == depth);
  assert(thread.stk.u_depth == u_depth);
  assert(thread.call_stack.top == top);
  assert(same_win(cur_win(thread), win));
}

static void test_stack_rejections() {
  test_fake_arch::reset();
  mock_kernel kernel{};
  mock_kobj kobj{};
  mock_kobj::callee_view view{};
  view.id = 2;

  alignas(16) static uint8_t stack_mem[0x4000];
  const uintptr_t lo = reinterpret_cast<uintptr_t>(stack_mem);
  const uintptr_t hi = lo + sizeof(stack_mem);

  // (a) 余白が足りない (caller の sp が stack の下限の 16B 手前以内) → 窓が開けず NO_STACK
  {
    Thread thread{};
    mock_context context{};
    thread.set_user_stack(lo, hi);
    context.user_sp = lo + 8u;
    assert(enter(kernel, thread, context, view, 0x1000u) == shizuku::kernel_error::NO_STACK);
    assert_untouched(thread, 0, 0, 0, shizuku::stack_window{lo, hi});
  }
  // (b) 記録の枠が取れない (arch の call_record_room_at が false) → NO_STACK。STACK の深さの上限はここで決まる。
  {
    Thread thread{};
    mock_context context{};
    thread.set_user_stack(lo, hi);
    context.user_sp = lo + 0x3000u;
    test_fake_arch::record_room = false;
    assert(enter(kernel, thread, context, view, 0x1000u) == shizuku::kernel_error::NO_STACK);
    test_fake_arch::record_room = true;
    assert_untouched(thread, 0, 0, 0, shizuku::stack_window{lo, hi});
    assert(thread.call_stack.call_count == 0);
  }
  // (c) 外側に U の窓がある (u_depth != 0) 特権 caller からの U 呼出しは、戻りで外側の窓を復元できないので NO_STACK。
  {
    Thread thread{};
    mock_context context{};
    thread.base_object = 1;
    thread.set_user_stack(lo, hi);
    context.user_sp = lo + 0x3000u;
    assert(enter(kernel, thread, context, view, 0x1000u) == shizuku::kernel_error::OK);
    const auto w = cur_win(thread);
    const uintptr_t top = thread.call_stack.top;
    context.privileged = true;
    context.user_sp = w.hi - 128u;
    assert(enter(kernel, thread, context, view, 0x1000u) == shizuku::kernel_error::NO_STACK);
    assert_untouched(thread, 1, 1, top, w);
    assert(thread.call_stack.call_count == 1);
  }
  (void)kobj;
}

// ===========================================================================
// dispatch 段が caller のとき。
// ===========================================================================
static void test_stack_dispatch_caller() {
  test_fake_arch::reset();
  mock_kernel kernel{};
  mock_kobj kobj{};
  mock_kobj::callee_view view{};
  view.id = 2;

  alignas(16) static uint8_t stack_mem[0x4000];
  const uintptr_t lo = reinterpret_cast<uintptr_t>(stack_mem);
  const uintptr_t hi = lo + sizeof(stack_mem);

  // Case 1: base (特権) -> dispatch 段 (窓なし) -> U call。戻りで cur_win は stack 全域へ戻り、特権で dispatch 段へ戻る。
  {
    Thread thread{};
    mock_context context{};
    thread.base_object = 1;
    thread.set_user_stack(lo, hi);
    context.privileged = true;
    context.user_sp = hi - 0x200u;
    context.frame.pc = 0x4200A000u;
    assert(shizuku::header_push_of<test_fake_arch>(thread, context, context.frame, mock_kernel::LEDGER_DISPATCH, 0));
    {
      auto &d = thread.ledger_top_mut();
      d.object = 1;
      d.kind = (uint8_t)shizuku::object_kind::PLAIN;
    }
    assert(thread.call_stack.depth == 1 && thread.call_stack.call_count == 0);
    assert(thread.call_stack.base_privileged == 1);
    Record &d0 = rec_at(thread, 0);

    context.user_sp = hi - 0x300u; // handler (特権) が自分の stack を使ってから U を呼ぶ
    assert(enter(kernel, thread, context, view, 0x1000u) == shizuku::kernel_error::OK);
    assert(thread.call_stack.depth == 2 && thread.call_stack.call_count == 1 && thread.stk.u_depth == 1);
    Record &r1 = rec_at(thread, 1);
    assert(r1.prev == addr_of(d0) && r1.prev_call == 0); // dispatch 段は call 段の鎖に入らない
    assert(same_win(cur_win(thread), shizuku::stack_window{(hi - 0x300u) - K, hi - 0x300u}));
    put((hi - 0x300u) - 64u, 0xCA11EE01u); // U callee が窓に残した値

    assert(pop(kobj, thread, context));
    assert(thread.call_stack.depth == 1 && thread.call_stack.call_count == 0 && thread.stk.u_depth == 0);
    assert(thread.call_stack.top == addr_of(d0));
    assert(cur_win(thread).lo == lo && cur_win(thread).hi == hi); // {0, 0} ではなく stack 全域
    assert(context.privileged); // dispatch で入った活動は常に特権
    // dispatch 段 (特権 handler) が呼び手の戻り (M -> U) は窓をゼロ化しない。窓は外側 (全域) へ戻る。
    assert(test_fake_arch::zeroed_windows.empty());
    assert(get((hi - 0x300u) - 64u) == 0xCA11EE01u);
  }
  // Case 2: U (A) -> dispatch 段 -> 特権 handler が U (B) を呼ぶ。handler は外側の U 窓 (u_depth != 0) の中にいるので NO_STACK。
  {
    Thread thread{};
    mock_context context{};
    thread.base_object = 1;
    thread.set_user_stack(lo, hi);
    context.user_sp = hi - 0x400u;
    assert(enter(kernel, thread, context, view, 0x1000u) == shizuku::kernel_error::OK);
    const auto wa = cur_win(thread);
    context.privileged = false;         // A (U) が svc を撃つ
    context.user_sp = wa.hi - 128u;
    assert(shizuku::header_push_of<test_fake_arch>(thread, context, context.frame, mock_kernel::LEDGER_DISPATCH, 1));
    {
      auto &d = thread.ledger_top_mut();
      d.object = 1;
      d.kind = (uint8_t)shizuku::object_kind::PLAIN;
    }
    context.privileged = true;          // handler は特権で走る
    context.user_sp = wa.hi - 256u;
    const uintptr_t top = thread.call_stack.top;
    assert(enter(kernel, thread, context, view, 0x1000u) == shizuku::kernel_error::NO_STACK);
    assert_untouched(thread, 2, 1, top, wa);
  }
}

// ===========================================================================
// 窓の伸長の意味 (fault 時)。
// ===========================================================================
static void test_stack_window_extension_fault_semantics() {
  test_fake_arch::reset();
  mock_kernel kernel{};
  Thread thread{};
  mock_context context{};
  thread.base_object = 1;

  alignas(16) static uint8_t stack_mem[0x4000];
  const uintptr_t stack_lo = reinterpret_cast<uintptr_t>(stack_mem);
  const uintptr_t stack_hi = stack_lo + sizeof(stack_mem);
  thread.set_user_stack(stack_lo, stack_hi);

  // depth == 0 (base): cur_win は stack 全域 (u_depth == 0 なので trap 側は伸長しない)
  assert(thread.call_stack.depth == 0 && thread.stk.u_depth == 0);
  assert(thread.stk.cur_win_lo == stack_lo && thread.stk.cur_win_hi == stack_hi);

  context.user_sp = stack_hi - 0x100u;
  mock_kobj::callee_view view{};
  view.id = 2;
  assert(enter(kernel, thread, context, view, 0x1000u) == shizuku::kernel_error::OK);
  assert(thread.call_stack.depth == 1 && thread.stk.u_depth == 1);
  const auto initial_win = cur_win(thread);
  assert(initial_win.is_valid());

  // [stack_lo, win.lo) の fault: 窓は下へ伸びる
  const uintptr_t fault_addr = initial_win.lo - 64u;
  assert(fault_addr >= stack_lo && fault_addr < initial_win.lo);
  const auto extended_win = shizuku::extend_window(initial_win, fault_addr, stack_lo);
  assert(extended_win.is_valid() && extended_win.lo <= fault_addr && extended_win.hi == initial_win.hi);
  // stack_lo より下 / win.hi 以上の fault は拒否
  assert(!shizuku::extend_window(initial_win, stack_lo - 4u, stack_lo).is_valid());
  assert(!shizuku::extend_window(initial_win, initial_win.hi, stack_lo).is_valid());
  assert(!shizuku::extend_window(initial_win, initial_win.hi + 16u, stack_lo).is_valid());
  // 窓の無い活動 ({}) は伸ばせない
  const shizuku::stack_window dispatch_win{};
  assert(!dispatch_win.is_valid());
  assert(!shizuku::extend_window(dispatch_win, fault_addr, stack_lo).is_valid());
}

// ===========================================================================
// M callee は窓を開かず、戻りでゼロ化しない (caller の trap frame を壊さない)。M -> U で caller の sp が
// thread の stack の外なら NO_STACK。
// ===========================================================================
static void test_stack_m_mode_callee_no_window_and_frame_protected() {
  test_fake_arch::reset();
  mock_kernel kernel{};
  mock_kobj kobj{};
  kobj.m_privileged[2] = true;
  mock_context context{};
  Thread thread{};
  thread.base_object = 1;

  const uintptr_t u_stack_lo = 0x20000000u;
  const uintptr_t u_stack_hi = 0x20004000u;
  thread.set_user_stack(u_stack_lo, u_stack_hi);

  alignas(16) static uint8_t kernel_stack[0x1000];
  const uintptr_t ret_sp = reinterpret_cast<uintptr_t>(kernel_stack + 0x800);
  context.user_sp = ret_sp;
  context.privileged = true;

  // 生きている trap frame の語 (ret_sp の下。記録は ret_sp 以上なのでここには届かない)
  put(ret_sp - 128u, 0x42001234u);
  put(ret_sp - 64u, 0x42005678u);
  put(ret_sp - 4u, 0xC001CAFEu);

  mock_kobj::callee_view view_m{};
  view_m.id = 2;
  view_m.privileged = true;

  // 1. M -> M
  assert(enter(kernel, thread, context, view_m, 0x42002000u) == shizuku::kernel_error::OK);
  assert(thread.call_stack.depth == 1 && thread.stk.u_depth == 0);
  assert(thread.stk.cur_win_lo == u_stack_lo && thread.stk.cur_win_hi == u_stack_hi);

  // 2. M から戻る
  assert(pop(kobj, thread, context));
  assert(thread.call_stack.depth == 0 && thread.stk.u_depth == 0);
  assert(test_fake_arch::zeroed_windows.empty()); // M callee ではゼロ化しない
  assert(context.privileged);
  assert(get(ret_sp - 128u) == 0x42001234u);
  assert(get(ret_sp - 64u) == 0x42005678u);
  assert(get(ret_sp - 4u) == 0xC001CAFEu);

  // 3. M -> U で caller の ret_sp が kernel stack (U の stack の外): 拒否
  mock_kobj::callee_view view_u{};
  view_u.id = 3;
  context.user_sp = ret_sp;
  assert(enter(kernel, thread, context, view_u, 0x42003000u) == shizuku::kernel_error::NO_STACK);
  assert_untouched(thread, 0, 0, 0, shizuku::stack_window{u_stack_lo, u_stack_hi});
}

// ===========================================================================
// M caller が U callee を呼ぶ。thread の stack = M が実際に走る stack そのもの。
// 窓は [ret_sp - K, ret_sp) で開く (M -> U でも U callee から M の stack を守る)。M への戻りでは
// ゼロ化しない (M は信頼できるソフトウェア。設計者の決定)。窓は stack 全域へ戻り、ret_sp 以上 (記録の枠より
// 上) の語は保存される。
// ===========================================================================
static void test_stack_m_caller_u_callee_window() {
  test_fake_arch::reset();
  mock_kernel kernel{};
  mock_kobj kobj{};
  mock_context context{};
  Thread thread{};
  thread.base_object = 1;

  alignas(16) static uint8_t m_thread_stack[0x1000];
  const uintptr_t lo = reinterpret_cast<uintptr_t>(m_thread_stack);
  const uintptr_t hi = lo + sizeof(m_thread_stack);
  thread.set_stack_bounds(lo, hi);
  assert(thread.stk.stack_lo == lo && thread.stk.stack_hi == hi);

  const uintptr_t ret_sp = lo + 0xC00u;
  context.privileged = true;
  context.user_sp = ret_sp;
  put(ret_sp + kAboveRecord, 0x4D43414Cu);
  put(ret_sp + kAboveRecord + 0x20u, 0x4D544F50u);
  for (uintptr_t p = ret_sp - K; p < ret_sp; p += 4)
    put(p, 0xDEADBEEFu); // 前の利用者の残り。M への戻りではゼロ化されず残る
  put(ret_sp - K - 4u, 0xBADBAD01u); // 窓の下は触られない

  mock_kobj::callee_view view_u{};
  view_u.id = 3;
  assert(enter(kernel, thread, context, view_u, 0x42003000u) == shizuku::kernel_error::OK);
  assert(thread.call_stack.depth == 1 && thread.stk.u_depth == 1);
  const auto win = cur_win(thread);
  assert(win.hi == ret_sp && win.lo == ret_sp - K);
  assert(addr_of(rec_at(thread, 0)) == ret_sp);
  assert(thread.call_stack.base_privileged == 1);

  assert(pop(kobj, thread, context));
  assert(thread.call_stack.depth == 0 && thread.stk.u_depth == 0);
  assert(test_fake_arch::zeroed_windows.empty()); // M -> U の戻りはゼロ化しない
  for (uintptr_t p = win.lo; p < win.hi; p += 4)
    assert(get(p) == 0xDEADBEEFu); // 窓の中の値が残る
  assert(cur_win(thread).lo == lo && cur_win(thread).hi == hi); // 窓は stack 全域へ戻る
  assert(context.privileged); // 最外の caller (M) の特権で再開する
  assert(get(ret_sp + kAboveRecord) == 0x4D43414Cu);
  assert(get(ret_sp + kAboveRecord + 0x20u) == 0x4D544F50u);
  assert(get(ret_sp - K - 4u) == 0xBADBAD01u);

  // 範囲外の ret_sp (thread stack の外) は NO_STACK。記録は書かれない。
  context.privileged = true;
  context.user_sp = lo - 16u;
  assert(enter(kernel, thread, context, view_u, 0x42003000u) == shizuku::kernel_error::NO_STACK);
  context.user_sp = hi + 16u;
  assert(enter(kernel, thread, context, view_u, 0x42003000u) == shizuku::kernel_error::NO_STACK);
  assert_untouched(thread, 0, 0, 0, shizuku::stack_window{lo, hi});
  // 記録の枠が thread stack の内側に収まる最も上の位置は受け付ける。
  // (枠が stack の上端を越える位置の拒否は arch の call_record_room_at の仕事で、この mock の範囲外。)
  context.user_sp = hi - kAboveRecord;
  assert(enter(kernel, thread, context, view_u, 0x42003000u) == shizuku::kernel_error::OK);
}

// ===========================================================================
// M -> U1 -> U2。U2 から U1 への戻り (呼び手が U) はゼロ化する。U1 から M への戻り (呼び手が M) はゼロ化しない。
// 決定の帰結: M が U1 の後に同じ位置で U2 を呼ぶと、U2 の窓に U1 の残りが見える (変えない)。
// ===========================================================================
static void test_stack_m_u1_u2_zero_only_for_u_caller() {
  test_fake_arch::reset();
  mock_kernel kernel{};
  mock_kobj kobj{};
  mock_context context{};
  Thread thread{};
  thread.base_object = 1;

  alignas(16) static uint8_t m_thread_stack[0x2000];
  const uintptr_t lo = reinterpret_cast<uintptr_t>(m_thread_stack);
  const uintptr_t hi = lo + sizeof(m_thread_stack);
  thread.set_stack_bounds(lo, hi);

  const uintptr_t s0 = lo + 0x1800u;
  context.privileged = true; // 最外の呼び手は M
  context.user_sp = s0;
  put(s0 + kAboveRecord, 0x4D43414Cu);

  mock_kobj::callee_view view_u1{};
  view_u1.id = 3;
  mock_kobj::callee_view view_u2{};
  view_u2.id = 4;

  // M -> U1
  assert(enter(kernel, thread, context, view_u1, 0x42003000u) == shizuku::kernel_error::OK);
  assert(thread.call_stack.base_privileged == 1 && thread.stk.u_depth == 1);
  const auto w1 = cur_win(thread);
  assert(w1.hi == s0 && w1.lo == s0 - K);
  put(w1.hi - 32u, 0xA1A1A1A1u);  // U1 の frame (U2 の窓の上)
  put(w1.hi - 48u, 0xA1A1A102u); // U2 の記録 ([w1.hi - 128, +64)) の上

  // U1 -> U2
  context.privileged = false;
  context.user_sp = w1.hi - 128u;
  assert(enter(kernel, thread, context, view_u2, 0x42004000u) == shizuku::kernel_error::OK);
  assert(thread.call_stack.depth == 2 && thread.stk.u_depth == 2);
  const auto w2 = cur_win(thread);
  assert(w2.hi == w1.hi - 128u && w2.lo == w2.hi - K);
  put(w2.hi - 16u, 0xB2B2B2B2u);
  put(w2.lo + 8u, 0xB2B20002u);

  // U2 -> U1: 呼び手が U なのでゼロ化する
  assert(pop(kobj, thread, context));
  assert(thread.call_stack.depth == 1 && thread.stk.u_depth == 1);
  assert(test_fake_arch::zeroed_windows.size() == 1);
  assert(same_win(test_fake_arch::zeroed_windows[0], w2));
  assert_zero(w2);
  assert(!context.privileged);
  assert(get(w1.hi - 32u) == 0xA1A1A1A1u && get(w1.hi - 48u) == 0xA1A1A102u); // U1 の frame は残る
  const shizuku::stack_window back_in_u1{w1.hi - 128u - K, w1.hi};
  assert(same_win(cur_win(thread), back_in_u1));

  // U1 -> M: 呼び手が M なのでゼロ化しない。窓は stack 全域へ戻る
  assert(pop(kobj, thread, context));
  assert(thread.call_stack.depth == 0 && thread.stk.u_depth == 0);
  assert(test_fake_arch::zeroed_windows.size() == 1); // 増えない
  assert(get(w1.hi - 32u) == 0xA1A1A1A1u && get(w1.hi - 48u) == 0xA1A1A102u); // U1 の残りが残る
  assert(cur_win(thread).lo == lo && cur_win(thread).hi == hi);
  assert(context.privileged);
  assert(get(s0 + kAboveRecord) == 0x4D43414Cu);

  // 帰結: M が同じ位置から U2 を呼ぶと、窓の中に U1 の残りが見える
  context.user_sp = s0;
  assert(enter(kernel, thread, context, view_u2, 0x42004000u) == shizuku::kernel_error::OK);
  const auto w3 = cur_win(thread);
  assert(w3.hi == w1.hi && get(w3.hi - 32u) == 0xA1A1A1A1u);
  assert(pop(kobj, thread, context));
  assert(test_fake_arch::zeroed_windows.size() == 1);
}

// ===========================================================================
// 窓機構を持たない arch (HAS_STACK_WINDOW を宣言しない = ARM 相当): 窓の範囲検査・ゼロ化を一切しない。
// stack が thread 外でも NO_STACK にならず、thread の窓の領域も増えない。
// ===========================================================================
struct nowin_frame { uintptr_t pc = 0; uintptr_t ra = 0; };
struct nowin_context {
  static constexpr bool CALL_IN_PLACE = true;
  nowin_frame frame{};
  nowin_frame *sp = &frame;
  uintptr_t user_sp = 0;
  bool privileged = false;
};
struct nowin_arch {
  static constexpr bool CALL_IN_PLACE = true;
  static constexpr uintptr_t CALL_RECORD_OFFSET = 0;
  static constexpr uintptr_t CALL_RECORD_BYTES = 32;
  static constexpr uintptr_t record_return_sp(uintptr_t record) { return record - CALL_RECORD_OFFSET; }
  static inline int zero_calls = 0;
  static void zero_stack_window(shizuku::stack_window) { ++zero_calls; }
  static bool call_record_room_at(const nowin_context &, uintptr_t) { return true; }
  static uintptr_t frame_pc(const nowin_frame &f) { return f.pc; }
  static uintptr_t call_return_sp(const nowin_context &c) { return c.user_sp; }
  static bool context_privileged(const nowin_context &c) { return c.privileged; }
  static void set_entry(nowin_frame &f, uintptr_t pc, uintptr_t ra) { f.pc = pc; f.ra = ra; }
  static uintptr_t return_stub() { return 0x1000u; }
  static void set_priv(nowin_context &c, bool p) { c.privileged = p; }
  static void set_region_window(nowin_context &, uintptr_t, uintptr_t) {}
  static void switch_callee_protection(nowin_context &, nowin_frame &, uint32_t, bool) {}
  static nowin_frame *call_resume_at(nowin_context &c, uintptr_t pc, uintptr_t sp, bool priv,
                                     uintptr_t, uintptr_t, bool) {
    c.frame.pc = pc; c.user_sp = sp; c.privileged = priv; return &c.frame;
  }
};
struct nowin_kernel {
  using ARCH = nowin_arch;
  using CONTEXT = nowin_context;
  using FRAME = nowin_frame;
  using THREAD = shizuku::templates::thread<nowin_context>;
  static constexpr uint8_t LEDGER_CALL = 1;
  static constexpr uint8_t LEDGER_DISPATCH = 2;
  static constexpr uint8_t LEDGER_DESCRIPTOR_CALL = 3;
  uint32_t svc_handler_object() const { return 0xFFFFu; }
};

static void test_stack_gate_false_no_window_no_zero() {
  static_assert(!nowin_kernel::THREAD::HAS_STACK_WINDOW);
  static_assert(mock_kernel::THREAD::HAS_STACK_WINDOW);
  // thread のメモリ増分: gate=false は stack 範囲と窓 (stack_extent) を持たない。
  static_assert(sizeof(mock_kernel::THREAD) >= sizeof(nowin_kernel::THREAD) + 4u * sizeof(uintptr_t));
  static_assert(std::is_empty_v<decltype(nowin_kernel::THREAD{}.stk)>);

  nowin_arch::zero_calls = 0;
  nowin_kernel kernel{};
  mock_kobj kobj{};
  nowin_context context{};
  nowin_kernel::THREAD thread{};
  thread.base_object = 1;
  thread.set_stack_bounds(0x20000000u, 0x20001000u); // gate=false では何もしない

  // 窓機構があれば open_window が失敗する位置 (buffer の下限から 8B) でも NO_STACK にならない。
  alignas(16) static uint8_t buf[0x200];
  context.user_sp = reinterpret_cast<uintptr_t>(buf) + 16u;
  mock_kobj::callee_view view{};
  view.id = 2;
  auto err = shizuku::native::enter_in_place<nowin_kernel, mock_kobj>(
      kernel, thread, context, view, 0x1000u, 1);
  assert(err == shizuku::kernel_error::OK);
  assert(thread.call_stack.depth == 1);

  nowin_frame *frame_ptr = nullptr;
  bool pop_ok = shizuku::native::pop_in_place<nowin_kernel, mock_kobj>(
      kobj, thread, context, frame_ptr, true, false);
  assert(pop_ok);
  assert(thread.call_stack.depth == 0);
  assert(nowin_arch::zero_calls == 0);
}

// ===========================================================================
// M callee は窓の状態に触れない。cur_win は U の入場・戻りでしか動かない。
//   STACK では「外側に U の窓がある特権 caller (M) からの U 呼出し」は NO_STACK (戻りで外側の窓を復元できない)。
//   旧 (LEAN) の U1 -> M -> U2 は成り立たない呼び方で、拒否されることに置き換えた。
// ===========================================================================
static void test_stack_m_callee_leaves_window_state_untouched() {
  test_fake_arch::reset();
  mock_kernel kernel{};
  mock_kobj kobj{};
  kobj.m_privileged[4] = true; // 4 = M callee
  mock_context context{};
  Thread thread{};
  thread.base_object = 1;

  alignas(16) static uint8_t stack_mem[0x4000];
  const uintptr_t lo = reinterpret_cast<uintptr_t>(stack_mem);
  const uintptr_t hi = lo + sizeof(stack_mem);
  thread.set_stack_bounds(lo, hi);
  const shizuku::stack_window full{thread.stk.cur_win_lo, thread.stk.cur_win_hi};

  mock_kobj::callee_view view_u{};
  view_u.id = 3;
  mock_kobj::callee_view view_m{};
  view_m.id = 4;
  view_m.privileged = true;

  // (1) M -> M を 3 段: 窓は開かず cur_win も動かない
  context.privileged = true;
  context.user_sp = lo + 0x3000u;
  for (uint32_t i = 0; i < 3; ++i) {
    assert(enter(kernel, thread, context, view_m, 0x42004000u) == shizuku::kernel_error::OK);
    assert(thread.call_stack.depth == i + 1);
    assert(thread.stk.u_depth == 0);
    assert(same_win(cur_win(thread), full));
    context.user_sp -= 0x80u; // M callee は自分の stack を使ってから次を呼ぶ
  }
  for (uint32_t i = 0; i < 3; ++i) {
    assert(pop(kobj, thread, context));
    assert(same_win(cur_win(thread), full));
  }
  assert(thread.call_stack.depth == 0);
  assert(test_fake_arch::zeroed_windows.empty());

  // (2) U1 -> M。M は窓に触れない。M から U2 を呼ぶのは NO_STACK。
  context.privileged = false;
  context.user_sp = lo + 0x3000u;
  assert(enter(kernel, thread, context, view_u, 0x42001000u) == shizuku::kernel_error::OK);
  const auto w1 = cur_win(thread);
  assert(w1.is_valid() && thread.stk.u_depth == 1);

  context.user_sp = w1.lo + 0x80u; // U1 が走っている sp
  assert(enter(kernel, thread, context, view_m, 0x42004000u) == shizuku::kernel_error::OK);
  assert(thread.stk.u_depth == 1 && same_win(cur_win(thread), w1));
  assert(test_fake_arch::zeroed_windows.empty());

  context.user_sp = lo + 0x1000u; // M が走っている sp
  const uintptr_t m_top = thread.call_stack.top;
  assert(enter(kernel, thread, context, view_u, 0x42002000u) == shizuku::kernel_error::NO_STACK);
  assert_untouched(thread, 2, 1, m_top, w1);

  // M -> U1 へ戻る: ゼロ化なし、cur_win は U1 の窓のまま、特権は U1 (非特権) へ
  assert(pop(kobj, thread, context));
  assert(thread.call_stack.depth == 1 && thread.stk.u_depth == 1);
  assert(test_fake_arch::zeroed_windows.empty());
  assert(same_win(cur_win(thread), w1));
  assert(!context.privileged);

  // U1 を戻す: U1 の窓をゼロ化し、cur_win は stack 全域へ
  assert(pop(kobj, thread, context));
  assert(test_fake_arch::zeroed_windows.size() == 1);
  assert(same_win(test_fake_arch::zeroed_windows[0], w1));
  assert(same_win(cur_win(thread), full));
  assert(thread.call_stack.depth == 0 && thread.stk.u_depth == 0);
}

// ===========================================================================
// 【現状の振る舞いの記録。上層が設計判断待ち。製品コードは直さない】
// 呼び先が、既にある先頭の記録と重なる位置から ecall したときの振る舞い。
//   U の呼び先 A が、自分の window の天井 (= 入場時の caller の SP = 先頭の記録 r0 の位置 s0) の 16B 下を SP にして
//   ecall する (stub を通さない生の ecall を想定)。記録の枠 [s0 - 16, s0 - 16 + sizeof(record)) は r0 と重なる。
//   読んだ限りの現状:
//     - enter_in_place は重なりを検査しない (arch の call_record_room_at は stack の天井と余白だけを見る)。入場は OK。
//     - 新しい記録の書込みが r0 の先頭 (prev / prev_call / ret_pc ...) を上書きする。
//     - 内側 (新しい記録) の pop は通る (鎖の照合は新しい記録と、ひとつ下の記録の depth だけを見る)。
//     - 外側 (r0) の pop は stack_record_chain_ok が false (r0.prev が壊れ、depth 1 なのに prev != 0) で、
//       pop_in_place が false を返す (呼び出し側が panic する)。何も変えずに止まる。
//   つまり「A が自分の caller の記録を壊せる。検出は外側の戻りの時点で、panic になる (DoS)」。
//   期待値は読みに基づく。実行結果で食い違ったら読みを直す。
// ===========================================================================
static void test_current_behavior_overlapping_record_not_detected() {
  test_fake_arch::reset();
  mock_kernel kernel{};
  mock_kobj kobj{};
  Thread thread{};
  mock_context context{};
  thread.base_object = 1;

  alignas(16) static uint8_t stack_mem[0x4000];
  const uintptr_t lo = reinterpret_cast<uintptr_t>(stack_mem);
  const uintptr_t hi = lo + sizeof(stack_mem);
  thread.set_user_stack(lo, hi);

  const uintptr_t s0 = lo + 0x3000u;
  context.user_sp = s0;
  context.frame.pc = 0x42000010u;
  mock_kobj::callee_view view_a{};
  view_a.id = 2;
  mock_kobj::callee_view view_b{};
  view_b.id = 3;

  assert(enter(kernel, thread, context, view_a, 0x42001000u) == shizuku::kernel_error::OK);
  assert(rec_at(thread, 0).ret_pc == 0x42000010u);

  // A が r0 と重なる位置から B を呼ぶ
  context.frame.pc = 0x42001020u;
  context.user_sp = s0 - 16u;
  assert(enter(kernel, thread, context, view_b, 0x42002000u) == shizuku::kernel_error::OK); // 検出されない
  assert(thread.call_stack.depth == 2 && thread.call_stack.top == s0 - 16u);
  // r0 の内容が新しい記録に上書きされている (元の ret_pc ではなくなっている)
  assert(reinterpret_cast<Record *>(s0)->ret_pc != 0x42000010u);

  // 内側の pop は通る
  assert(pop(kobj, thread, context));
  assert(thread.call_stack.depth == 1);
  // 外側の pop は鎖の照合で拒否される (pop_in_place が false を返し、状態は変わらない)
  assert(!pop(kobj, thread, context));
  assert(thread.call_stack.depth == 1);
}

int main() {
  test_open_window();
  test_extend_window();
  test_zero_window();
  test_zero_window_unrolled32();
  test_set_stack_bounds_alignment();
  test_stack_nesting_a_a();
  test_stack_nesting_a_b_a();
  test_stack_unwind_in_place();
  test_stack_rejections();
  test_stack_dispatch_caller();
  test_stack_window_extension_fault_semantics();
  test_stack_m_mode_callee_no_window_and_frame_protected();
  test_stack_m_caller_u_callee_window();
  test_stack_m_u1_u2_zero_only_for_u_caller();
  test_stack_gate_false_no_window_no_zero();
  test_stack_m_callee_leaves_window_state_untouched();
  test_current_behavior_overlapping_record_not_detected();
  return 0;
}
