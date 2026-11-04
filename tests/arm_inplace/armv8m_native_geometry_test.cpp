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
  // Callee SP 偽装 (sp > ceiling) は call_stack_has_room の sp <= ceiling で拒絶される
  static_assert(!A::call_stack_has_room(sp + 32, sp - reserve, sp));
  static_assert(!A::call_stack_has_room(sp + 128, sp - reserve, sp));
  static_assert(!A::call_stack_has_room(sp + 4, sp - reserve, sp));
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
      // stack_ceiling による SP 偽装防止の動的検査
      c.stack_ceiling = static_cast<uint32_t>(ret);
      assert(A::call_header_has_room(c, 0));
      c.stack_ceiling = static_cast<uint32_t>(ret - 32); // ceiling < ret (SP 偽装)
      assert(!A::call_header_has_room(c, 0));
      c.stack_ceiling = 0;
    }
    c.sp->xPSR ^= 1u << 9;
    assert(!A::call_header_has_room(c, 0));
  }
  // 0. EXIT の印の判定ヘルパ: 通常 stub (0xA55A)
  static_assert(shizuku::RETURN_STUB_MARKER == 0xA55Au);
  static_assert(shizuku::is_return_stub_marker(0xA55Au));
  static_assert(!shizuku::is_return_stub_marker(0) && !shizuku::is_return_stub_marker(0xA559u) &&
                !shizuku::is_return_stub_marker(0xA55Cu) && !shizuku::is_return_stub_marker(0x1A55Au));
  // 1. RETURN 側: 戻りでカーネルは FP 部を素通しする (強制巻戻し scrub=true のみ 0 消去)。
  for (bool scrub : {false, true}) for (bool extended : {false, true}) for (bool overlap : {false, true}) {
    std::memset(storage, 0, sizeof(storage));
    A::context_t c{};
    c.exc_return = extended ? 0xffffffed : 0xfffffffd;
    c.sp = reinterpret_cast<Frame *>(base + 512);
    auto *fp = reinterpret_cast<uint32_t *>(c.sp + 1);
    for (unsigned i = 0; i < 18; ++i) fp[i] = 0xabc00000u + i;
    const uintptr_t target = base + (overlap ? 512 : 832) + (extended ? 24 : 0);
    const uintptr_t ret = target + (extended ? 104 : 32);
    MockFP::count = 0;
    auto *result = A::call_resume_at<MockFP>(c, 0x12345679, ret, overlap,
                                                   /*caller_fpca=*/true, 0x2000, 0x3000, scrub, false, core);
    assert(reinterpret_cast<uintptr_t>(result) == target && c.sp == result);
    assert(result->pc == 0x12345679 && result->xPSR == (1u << 24));
    assert(result->r0 == 0 && result->r1 == 0 && result->r2 == 0 &&
           result->r3 == 0 && result->r12 == 0 && result->lr == 0);
    assert(c.control == (overlap ? A::CONTROL_PRIV_PSP : A::CONTROL_UNPRIV_PSP));
    assert(c.region_base == 0x2000 && c.region_limit == 0x3000);
    assert(MockFP::count == (extended ? 1u : 0u));
    if (extended) {
      const auto *saved = reinterpret_cast<const uint32_t *>(result + 1);
      if (scrub) {
        for (unsigned i = 0; i < 18; ++i) assert(saved[i] == 0);
      } else {
        for (unsigned i = 0; i < 18; ++i) assert(saved[i] == (0xabc00000u + i));
      }
    }
    assert(A::call_return_sp(c) == ret);
  }
  // Model FP return passthrough
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
    auto *resumed = A::call_resume_at<MockFP>(c, 0x12345679, ret, true, /*caller_fpca=*/true, 0, 0, false,
                                              /*fp_result=*/false, core);
    const auto *saved = reinterpret_cast<const uint32_t *>(resumed + 1);
    assert(MockFP::count == 1 && saved[0] == (is_double ? 0u : 0x3fc00000u));
    assert(saved[1] == (is_double ? 0x3ff80000u : 0u));
    for (unsigned i = 2; i < 18; ++i) assert(saved[i] == (0xbeef0000u + i));
    assert(resumed->r0 == 0 && resumed->r1 == 0 && A::call_return_sp(c) == ret);
  }
  // 2. 強制巻戻し (scrub=true) 時の clear_upper_fp_registers の縛り
  // (a) 拡張フレーム (104B) + scrub=true -> materialize 1 回, clear_upper 1 回
  {
    std::memset(storage, 0, sizeof(storage));
    A::context_t c{};
    c.exc_return = 0xffffffed; // 拡張フレーム
    c.sp = reinterpret_cast<Frame *>(base + 512);
    const uintptr_t ret = base + 512 + 24 + 104;
    MockFP::reset();
    auto *res = A::call_resume_at<MockFP>(c, 0x12345679, ret, true, /*caller_fpca=*/false, 0, 0, /*scrub=*/true, core);
    assert(MockFP::count == 1);
    assert(MockFP::clear_upper_count == 1);
    assert(res != nullptr && c.sp == res);
  }
  // (b) 拡張フレーム (104B) + scrub=false (通常戻り) -> materialize 1 回, clear_upper 0 回 (素通し)
  {
    std::memset(storage, 0, sizeof(storage));
    A::context_t c{};
    c.exc_return = 0xffffffed; // 拡張フレーム
    c.sp = reinterpret_cast<Frame *>(base + 512);
    const uintptr_t ret = base + 512 + 24 + 104;
    MockFP::reset();
    auto *res = A::call_resume_at<MockFP>(c, 0x12345679, ret, true, /*caller_fpca=*/false, 0, 0, /*scrub=*/false, core);
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
    auto *res = A::call_resume_at<MockFP>(c, 0x12345679, ret, true, caller_fpca, 0, 0, false, core);
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

  // RP2350 cmd=8 (u_cost probe) の具体値による MPU Region 隔離検証:
  // caller 枠 [ret_sp, stack_top) が callee のどの Region にも入らないことを assert
  {
    constexpr uintptr_t stack_base = 0x2000ef80;
    constexpr uintptr_t stack_top = 0x2000f380;
    constexpr uintptr_t data_base = 0x2000ee80;
    constexpr uintptr_t data_limit = 0x2000ef80;
    constexpr uintptr_t flash_base = 0x10000000;
    constexpr uintptr_t flash_limit = 0x10026f80;

    const auto orig_stack = A::encode_stack_window(stack_base, stack_top);
    const auto data_win = A::encode_data_window(data_base, data_limit);

    // 親 P が CALL_METHOD を発行したときの ret_sp (ceiling)
    constexpr uintptr_t ret_sp = 0x2000f300;
    const uint32_t shrunk_rlar = A::shrink_stack_limit(orig_stack.rlar, ret_sp);

    // Region 2 の排他的終端アドレス
    const uintptr_t stack_exclusive_limit = ((shrunk_rlar & ~31u) + 32u);
    assert(stack_exclusive_limit == ret_sp);

    // caller 枠 [ret_sp, stack_top) 内の任意のアドレス p について:
    for (uintptr_t p = ret_sp; p < stack_top; p += 4) {
      // 1. Region 2 (縮小後の callee stack 領域) に入らない
      const bool in_stack = (p >= (orig_stack.rbar & ~31u)) && (p < stack_exclusive_limit);
      assert(!in_stack);

      // 2. Region 3 (callee data 領域) に入らない
      const uintptr_t data_exclusive_limit = ((data_win.rlar & ~31u) + 32u);
      const bool in_data = (p >= (data_win.rbar & ~31u)) && (p < data_exclusive_limit);
      assert(!in_data);

      // 3. Region 0 (Flash 領域) に入らない
      const bool in_flash = (p >= flash_base) && (p < flash_limit);
      assert(!in_flash);
    }
  }

  // RP2350 cmd=8 (上方偽装 SP と caller 枠の幾何学衝突・非整列 SP・控え復元検証):
  {
    constexpr uintptr_t stack_base = 0x2000ef80;
    constexpr uintptr_t stack_top = 0x2000f380;
    constexpr uintptr_t ret_sp = 0x2000f300;

    // 1. 上方偽装 SP の幾何学位置関係
    // callee 入場時 SP = ret_sp。
    // push {r4..r7, lr} (20B) + sub sp, #108 (108B) で sp = ret_sp - 128
    // add sp, #248 で偽装 SP = (ret_sp - 128) + 248 = ret_sp + 120
    constexpr uintptr_t spoof_sp = ret_sp + 120;
    assert(spoof_sp > ret_sp);
    assert(spoof_sp < stack_top);

    // FP 拡張フレーム (104B = 26語) がスタックされる領域 [spoof_sp - 104, spoof_sp)
    constexpr uintptr_t hw_frame_lo = spoof_sp - 104; // ret_sp + 16
    constexpr uintptr_t hw_frame_hi = spoof_sp;       // ret_sp + 120
    assert(hw_frame_lo >= ret_sp);
    assert(hw_frame_hi <= stack_top);

    // 親 P の syscall がレジスタ (sp, control, r7) を退避する領域 [ret_sp, ret_sp + 16)
    // hw_frame_lo (ret_sp + 16) は親の退避領域の直上 [ret_sp + 16, ret_sp + 120) に積まれ、
    // 親の局所変数 (sentinel など [ret_sp + 32, ret_sp + 120)) と重複する。
    assert(hw_frame_lo < ret_sp + 32);

    // 2. call_stack_has_room による上方偽装 SP (sp > ceiling) の拒絶判定
    assert(!A::call_stack_has_room(spoof_sp, stack_base, ret_sp));

    // 3. 非整列 SP (mode 95: sp + 4) の拒絶判定
    constexpr uintptr_t unaligned_sp = ret_sp - 128 + 4; // 非整列
    assert((unaligned_sp & 31u) != 0);
    assert(!A::call_stack_has_room(unaligned_sp, stack_base, ret_sp));

    // 4. 親 P の控え (saved_sp, saved_control, saved_r7) による caller 枠破壊検出と復元の論理検証
    uint32_t caller_stack[8] = {
        0x2000f350, // [sp, #0]: saved_sp
        0x00000000, // [sp, #4]: saved_control
        0xDEADBEEF, // [sp, #8]: padding / observe
        0x2000f320, // [sp, #12]: saved_r7
    };
    const uint32_t backup_sp = caller_stack[0];
    const uint32_t backup_control = caller_stack[1];
    const uint32_t backup_r7 = caller_stack[3];

    // 正常ケース: スタックが破壊されていない場合
    {
      const bool corrupted = (caller_stack[0] != backup_sp) ||
                             (caller_stack[1] != backup_control) ||
                             (caller_stack[3] != backup_r7);
      assert(!corrupted);
    }

    // 破壊ケース: HW stacking によりスタック上の saved_sp や saved_r7 が上書きされた場合
    {
      caller_stack[0] = 0xCAFEBABE; // 例外フレームの r0 等で上書き
      caller_stack[3] = 0xBAADF00D; // 例外フレームの r1 等で上書き
      const bool corrupted = (caller_stack[0] != backup_sp) ||
                             (caller_stack[1] != backup_control) ||
                             (caller_stack[3] != backup_r7);
      assert(corrupted);

      // 控えからの復元
      uint32_t restored_sp = corrupted ? backup_sp : caller_stack[0];
      uint32_t restored_r7 = corrupted ? backup_r7 : caller_stack[3];
      assert(restored_sp == 0x2000f350);
      assert(restored_r7 == 0x2000f320);
    }
  }
}
