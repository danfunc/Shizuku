#include <cassert>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#ifdef NDEBUG
#error "This test requires assert()"
#endif
#include "arch/armv8m/armv8m.hpp"
#include "core_num_test_access.hpp"

using A = shizuku::archs::armv8m<8, 4>;
using Frame = A::exception_frame_t;
struct MockFP {
  static inline unsigned count = 0;
  static inline unsigned clear_upper_count = 0;
  static void materialize() { ++count; }
  static void clear_upper_fp_registers() { ++clear_upper_count; }
  static void reset() { count = 0; clear_upper_count = 0; }
};

int main() {
  const auto core = shizuku::core_num_test_access<typename A::core_num>::make(0);
  {
    struct guarded_frame {
      uint32_t before;
      Frame frame;
      uint32_t after;
    } guarded{0xA55AA55Au, {}, 0x5AA55AA5u};
    for (auto &word : *reinterpret_cast<uint32_t (*)[8]>(&guarded.frame))
      word = 0xFFFFFFFFu;
    A::zero_exception_frame(guarded.frame);
    assert(guarded.before == 0xA55AA55Au && guarded.after == 0x5AA55AA5u);
    const auto *words = reinterpret_cast<const uint32_t *>(&guarded.frame);
    for (unsigned i = 0; i < 8; ++i) assert(words[i] == 0);
  }
  static_assert(shizuku::armv8m_abi::max_reservation_bytes == 128);
  constexpr uintptr_t reserve = A::EXC_FRAME_MAX_BYTES + A::FAULT_CONTEXT_BYTES +
      A::CALL_HEADROOM + 128 + A::HANDLER_STACK_ARG_BYTES;
  constexpr uintptr_t sp = 0x20002000;
  static_assert(A::call_stack_has_room(sp, sp - reserve, sp));
  static_assert(!A::call_stack_has_room(sp, sp - reserve + 1, sp));
  static_assert(!A::call_stack_has_room(sp, sp - reserve, sp - 32));
  static_assert(!A::call_stack_has_room(sp + 4, 0, 0));
  static_assert(!A::call_stack_has_room(32, 0, 0));
  alignas(32) uint32_t storage[512] = {};
  const uintptr_t base = reinterpret_cast<uintptr_t>(storage);
  for (bool extended : {false, true}) for (bool padded : {false, true}) {
    A::context_t c{};
    // Select an SVC frame whose post-return SP is 32-byte aligned.
    const uintptr_t ret = base + 1024;
    const unsigned bytes = extended ? 104 : 32;
    c.sp = reinterpret_cast<Frame *>(ret - bytes - (padded ? 4 : 0));
    c.sp->xPSR = padded ? 1u << 9 : 0;
    c.exc_return = extended ? 0xffffffed : 0xfffffffd;
    assert(A::call_return_sp(c) == ret);
    assert(A::call_header_has_room(c, 0));
    constexpr uintptr_t reserve = A::EXC_FRAME_MAX_BYTES + A::FAULT_CONTEXT_BYTES +
        A::CALL_HEADROOM + shizuku::armv8m_abi::max_reservation_bytes +
        A::HANDLER_STACK_ARG_BYTES;
    if (ret <= UINT32_MAX && ret >= reserve && ret - reserve < UINT32_MAX) {
      c.psplim = static_cast<uint32_t>(ret - reserve);
      assert(A::call_header_has_room(c, 0));
      c.psplim++;
      assert(!A::call_header_has_room(c, 0));
      c.psplim = 0;
    }
    c.sp->xPSR ^= 1u << 9;
    assert(!A::call_header_has_room(c, 0));
  }
  // 1. CALL 側: caller の FP 値を callee に見せるため、18 語全部が保たれることを検証
  for (bool extended : {false, true}) for (bool overlap : {false, true}) {
    std::memset(storage, 0, sizeof(storage));
    A::context_t c{};
    c.exc_return = extended ? 0xffffffed : 0xfffffffd;
    c.sp = reinterpret_cast<Frame *>(base + 512);
    auto *fp = reinterpret_cast<uint32_t *>(c.sp + 1);
    for (unsigned i = 0; i < 18; ++i) fp[i] = 0xabc00000u + i;
    const uintptr_t target = base + (overlap ? 512 : 832) + (extended ? 24 : 0);
    const uintptr_t ret = target + (extended ? 104 : 32);
    MockFP::count = 0;
    auto *result = A::call_resume_at<false, MockFP>(c, 0x12345679, ret, overlap,
                                                    /*caller_fpca=*/true, 0x2000, 0x3000, false, core);
    assert(reinterpret_cast<uintptr_t>(result) == target && c.sp == result);
    assert(result->pc == 0x12345679 && result->xPSR == (1u << 24));
    assert(result->r0 == 0 && result->r1 == 0 && result->r2 == 0 &&
           result->r3 == 0 && result->r12 == 0 && result->lr == 0);
    assert(c.control == (overlap ? A::CONTROL_PRIV_PSP : A::CONTROL_UNPRIV_PSP));
    assert(c.region_base == 0x2000 && c.region_limit == 0x3000);
    assert(MockFP::count == (extended ? 1u : 0u));
    if (extended) for (unsigned i = 0; i < 18; ++i)
      assert(reinterpret_cast<uint32_t *>(result + 1)[i] == 0xabc00000u + i);
    assert(A::call_return_sp(c) == ret);
  }
  // 2. RETURN 側: 戻りでカーネルが返り値以外を消す (s0,s1 が保たれ、s2-s15/FPSCR は 0)
  for (bool extended : {false, true}) for (bool overlap : {false, true}) {
    std::memset(storage, 0, sizeof(storage));
    A::context_t c{};
    c.exc_return = extended ? 0xffffffed : 0xfffffffd;
    c.sp = reinterpret_cast<Frame *>(base + 512);
    auto *fp = reinterpret_cast<uint32_t *>(c.sp + 1);
    for (unsigned i = 0; i < 18; ++i) fp[i] = 0xabc00000u + i;
    const uintptr_t target = base + (overlap ? 512 : 832) + (extended ? 24 : 0);
    const uintptr_t ret = target + (extended ? 104 : 32);
    MockFP::count = 0;
    auto *result = A::call_resume_at<true, MockFP>(c, 0x12345679, ret, overlap,
                                                   /*caller_fpca=*/true, 0x2000, 0x3000, false, core);
    assert(reinterpret_cast<uintptr_t>(result) == target && c.sp == result);
    assert(result->pc == 0x12345679 && result->xPSR == (1u << 24));
    assert(result->r0 == 0 && result->r1 == 0 && result->r2 == 0 &&
           result->r3 == 0 && result->r12 == 0 && result->lr == 0);
    assert(c.control == (overlap ? A::CONTROL_PRIV_PSP : A::CONTROL_UNPRIV_PSP));
    assert(c.region_base == 0x2000 && c.region_limit == 0x3000);
    assert(MockFP::count == (extended ? 1u : 0u));
    if (extended) {
      const auto *saved = reinterpret_cast<const uint32_t *>(result + 1);
      assert(saved[0] == 0xabc00000u);
      assert(saved[1] == 0xabc00001u);
      for (unsigned i = 2; i < 18; ++i) assert(saved[i] == 0);
    }
    assert(A::call_return_sp(c) == ret);
  }
  // copy_fp_frame_72b 単体の重なりケース (dst<src, dst>src)
  {
    alignas(16) uint32_t buf[64];
    for (int offset = -18; offset <= 18; ++offset) {
      for (unsigned i = 0; i < 64; ++i) buf[i] = 0xdead0000u + i;
      const int src_idx = 20;
      const int dst_idx = src_idx + offset;
      for (unsigned i = 0; i < 18; ++i) buf[src_idx + i] = 0x11223300u + i;
      A::copy_fp_frame_72b(&buf[dst_idx], &buf[src_idx]);
      for (unsigned i = 0; i < 18; ++i) {
        assert(buf[dst_idx + i] == 0x11223300u + i);
      }
    }
  }
  // Model the two FP return stubs' extended exception frames, including the
  // overlap that occurs when the callee's frame sits below the caller SP.
  for (bool is_double : {false, true}) {
    std::memset(storage, 0, sizeof(storage));
    A::context_t c{};
    c.exc_return = 0xffffffed;
    c.sp = reinterpret_cast<Frame *>(base + 512);
    auto *fp = reinterpret_cast<uint32_t *>(c.sp + 1);
    fp[0] = is_double ? 0u : 0x3fc00000u;
    fp[1] = is_double ? 0x3ff80000u : 0u;
    for (unsigned i = 2; i < 18; ++i) fp[i] = 0xbeef0000u + i;
    const uintptr_t ret = base + 512 + 24 + 104;
    MockFP::count = 0;
    auto *resumed = A::call_resume_at<true, MockFP>(c, 0x12345679, ret, true, /*caller_fpca=*/true, 0, 0, false, core);
    const auto *saved = reinterpret_cast<const uint32_t *>(resumed + 1);
    assert(MockFP::count == 1 && saved[0] == (is_double ? 0u : 0x3fc00000u));
    assert(saved[1] == (is_double ? 0x3ff80000u : 0u));
    for (unsigned i = 2; i < 18; ++i) assert(saved[i] == 0);
    assert(resumed->r0 == 0 && resumed->r1 == 0 && A::call_return_sp(c) == ret);
  }
  // 3. 欠陥 3 試験: B2 分岐 (FloatingPointState::clear_upper_fp_registers) の縛り
  // (a) 拡張フレーム (104B) + caller_fpca=false -> materialize 1 回, clear_upper 1 回
  {
    std::memset(storage, 0, sizeof(storage));
    A::context_t c{};
    c.exc_return = 0xffffffed; // 拡張フレーム
    c.sp = reinterpret_cast<Frame *>(base + 512);
    const uintptr_t ret = base + 512 + 24 + 104;
    MockFP::reset();
    auto *res = A::call_resume_at<true, MockFP>(c, 0x12345679, ret, true, /*caller_fpca=*/false, 0, 0, false, core);
    assert(MockFP::count == 1);
    assert(MockFP::clear_upper_count == 1);
    assert(res != nullptr && c.sp == res);
  }
  // (b) 拡張フレーム (104B) + caller_fpca=true -> materialize 1 回, clear_upper 0 回
  {
    std::memset(storage, 0, sizeof(storage));
    A::context_t c{};
    c.exc_return = 0xffffffed; // 拡張フレーム
    c.sp = reinterpret_cast<Frame *>(base + 512);
    const uintptr_t ret = base + 512 + 24 + 104;
    MockFP::reset();
    auto *res = A::call_resume_at<true, MockFP>(c, 0x12345679, ret, true, /*caller_fpca=*/true, 0, 0, false, core);
    assert(MockFP::count == 1);
    assert(MockFP::clear_upper_count == 0);
    assert(res != nullptr && c.sp == res);
  }
  // (c) 基本フレーム (32B, caller_fpca は false/true 双方) -> どちらも 0 回
  for (bool caller_fpca : {false, true}) {
    std::memset(storage, 0, sizeof(storage));
    A::context_t c{};
    c.exc_return = 0xfffffffd; // 基本フレーム (32B)
    c.sp = reinterpret_cast<Frame *>(base + 512);
    const uintptr_t ret = base + 512 + 32;
    MockFP::reset();
    auto *res = A::call_resume_at<true, MockFP>(c, 0x12345679, ret, true, caller_fpca, 0, 0, false, core);
    assert(MockFP::count == 0);
    assert(MockFP::clear_upper_count == 0);
    assert(res != nullptr && c.sp == res);
  }
  for (uintptr_t original = 0x10000; original < 0x10020; original += 8) {
    for (bool fp : {false, true}) {
      const auto ret = shizuku::armv8m_abi::trap_sp(original, fp);
      assert((ret & 31u) == 0 && ret + 32 <= original);
      if (fp) assert(original - 64 >= ret && original - 64 >= ret + 32);
      const uint32_t rlar = A::shrink_stack_limit(0x12340015, ret);
      assert((rlar & 31u) == 0x15);
      assert((rlar & ~31u) + 31u == ret - 1);
    }
  }
}
