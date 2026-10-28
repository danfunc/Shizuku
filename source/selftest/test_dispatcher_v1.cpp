// ===========================================================================
//  段 V1: カーネルディスパッチャ直接 native GET / objectland 配送 単体検証
// ===========================================================================
//  検証対象:
//    1. 宛先が Root KernelObject の場合、GET_CURRENT_OBJECT (5) および
//       GET_CALLER_OBJECT (6) が dispatch frame を積まずに直接処理されること。
//    2. ledger の呼出し段数に応じた caller identity が正しく導出されること
//       (base = NO_OBJECT、1-hop = caller A、nested 2-hop = caller B)。
//    3. 宛先が専用親ハンドラの場合 (to_root == false)、native API と同一番号
//       (5, 6, 16) であっても dispatch frame を積んで親ハンドラへ配送されること。
//    4. mode == svc_mode::thread (YIELD = 8, MEMORY_OWNER = 16) が
//       現行どおり actor (Root KOBJ dispatch frame) 経路に残ること。
//    5. 非特権 origin からの直接 GET で特権が昇格しないこと、および
//       mode == svc_mode::thread API (GRANT_REGION = 23) は trap で早期拒絶されず、
//       非特権のまま actor ハンドラへ配送されること。
//    6. EXIT_METHOD (2): marker 付き RETURN (LEDGER_CALL / LEDGER_DESCRIPTOR_CALL 段の戻り) は
//       handler を再入させず caller へ直接戻ること。専用親ハンドラ宛てと、スレッドの最外の
//       戻り (d == 0 かつ呼出し段なし) は従来経路のまま。
//    7. svc_attributes の mode == handler の集合が 2, 4, 5, 6, 9, 12, 33 だけであること。
//    8. EXIT_METHOD の明示 d: 検証 (floor / depth) と UNWIND_REJECTED、成功時の段数。
//    9. CALL_METHOD (4): dispatch 段を積まず呼び先へ直接入り、台帳に本当の呼び出し元が
//       載ること (A→B→C)。BAD_OBJECT / BAD_METHOD / UNDECLARED_METHOD / NO_STACK。
//   10. RUN_FOR (9): dispatch 段を積まず GRANT を直接撃つこと。NO_TIME / NOT_RUNNABLE 変換。
//   11. SET_BUDGET (12): dispatch 段を積まず m_budget を書くこと。範囲外は何もしない。
//   16. ecall フレームの目印 (入口 / 戻り) と、切替えをまたいだ文脈の不変 (RUN_FOR/YIELD)。
//   18. 戻りの出口の選び方: 素通し (RV32_TAG_ECALL) / 消去 (RV32_TAG_ECALL_SCRUB)。消去は
//       2 段以上を一度に落とす EXIT_METHOD・RETURN、FORWARD_CHILD_EXIT、別スレッドの再開だけ。
//   15. in-place call (2026-10-22 ABI): frame をコピーしない・ヘッダ無し・レジスタは呼び先へ通り抜け、
//       戻りは 1 段の普通の戻りなら素通し (RV32_TAG_ECALL)、2 段以上の巻き戻しと FORWARD_CHILD_EXIT は
//       消去の出口 (RV32_TAG_ECALL_SCRUB、18)。余白不足は NO_STACK、重なる返り trap フレーム (15a/15b)。

#define SHIZUKU_C6_SKELETON_PERMITTED 1
#include "shizuku/svc_attributes.hpp"
#include "shizuku/native_call.hpp"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>

namespace shizuku {
KERNEL kernel_instance;
#if defined(SHIZUKU_TEST_REAL_KERNEL_OBJECT)
extern KERNEL_OBJECT kernel_object_instance;
#else
KERNEL_OBJECT kernel_object_instance;

// 本物の callee_request (handler.cpp) は object_protection など kobj 全体に依存して
// host 試験には繋がらない。ここでは試験が据えた表から、同じ形の要求を作る。
template <>
call_request KERNEL_OBJECT::callee_request(uintptr_t id, uintptr_t entry_pc) const {
  call_request request{};
  request.entry_pc = entry_pc;
  request.callee_object = (uint32_t)id;
  request.callee_kind = (uint32_t)object_kind_of(id);
  request.callee_generation = object_generation(id);
  request.parent_handler_object = m_objects[id].parent_handler_object;
  request.parent_handler_entry = m_objects[id].parent_handler_entry;
  request.protection = (m_objects[id].flags & OBJECT_UNPRIVILEGED)
                           ? PROTECTION_UNPRIVILEGED
                           : PROTECTION_PRIVILEGED;
  request.region_base = m_objects[id].region_base;
  request.region_limit = m_objects[id].region_limit;
  return request;
}
#endif

namespace selftest {
// kobj の表 (private) を試験が直接組み立てるための口。
struct dispatcher_test_access {
  // 表の錠の語 (0 = 放されている)。trap 内の直接 call は inline の錠 (CAS) を取り・放す。
  static uint32_t lock_word() { return kernel_object_instance.m_table_lock; }
  // 方法記述子 (owner が target を呼ぶ入口) を据える。generation は呼び先の version と突き合わせる値。
  static void declare_descriptor(uint32_t owner, uint32_t index, uint32_t target,
                                 uintptr_t target_entry, uint32_t owner_generation,
                                 uint32_t target_generation) {
    auto &desc = kernel_object_instance.m_descriptors[owner][index];
    // 作成 (create_method_descriptor) と同じ条件・同じ写しで据える。呼び先が created で入口が非 0 の
    // ときだけ公開し、そうでなければ無効のまま (BAD_DESCRIPTOR)。
    const bool creatable = target < KERNEL_OBJECT::OBJECT_COUNT &&
                           kernel_object_instance.m_objects[target].created && target_entry != 0;
    if (!creatable) {
      KERNEL_OBJECT::invalidate_descriptor(desc);
      return;
    }
    kernel_object_instance.publish_descriptor(desc, target, target_entry, owner_generation);
    // 試験は呼び先の version を任意の値に据えて STALE を作るので、写した版を書き換える。
    desc.target_generation = target_generation;
  }
  static void invalidate_descriptor(uint32_t owner, uint32_t index) {
    KERNEL_OBJECT::invalidate_descriptor(kernel_object_instance.m_descriptors[owner][index]);
  }
  static void set_seq(uint32_t owner, uint32_t index, uint8_t seq) {
    kernel_object_instance.m_descriptors[owner][index].seq = seq;
  }
  static uint8_t get_seq(uint32_t owner, uint32_t index) {
    return kernel_object_instance.m_descriptors[owner][index].seq;
  }
  static void init_descriptors() {
    for (uint32_t o = 0; o < KERNEL_OBJECT::OBJECT_COUNT; ++o) {
      for (uint32_t i = 0; i < KERNEL_OBJECT::METHOD_DESCRIPTOR_CAPACITY; ++i) {
        invalidate_descriptor(o, i);
      }
    }
  }
  static void set_version(uint32_t id, uint32_t generation) {
    kernel_object_instance.m_objects[id].generation = generation;
  }
  static void set_instance_generation(uint32_t id, uint32_t generation) {
    kernel_object_instance.m_objects[id].instance_generation = generation;
  }
  static void set_region(uint32_t id, uint32_t base, uint32_t limit) {
    kernel_object_instance.m_objects[id].region_base = base;
    kernel_object_instance.m_objects[id].region_limit = limit;
  }
  static void set_created(uint32_t id, bool created) {
    kernel_object_instance.m_objects[id].created = created;
  }
  static uint32_t region_base(uint32_t id) {
    return kernel_object_instance.m_objects[id].region_base;
  }
  static uint32_t region_limit(uint32_t id) {
    return kernel_object_instance.m_objects[id].region_limit;
  }
  static bool can_call(uint32_t caller, uint32_t target) {
    return kernel_object_instance.can_call_method(caller, target);
  }
  static uintptr_t grant_region(uintptr_t target, uintptr_t base, uintptr_t limit,
                                object_error &error) {
    return kernel_object_instance.grant_region(target, base, limit, error);
  }
  static void declare(uint32_t id, uintptr_t method, KERNEL_OBJECT::method_t entry,
                      uint32_t flags, uint32_t parent_handler_object,
                      uintptr_t parent_handler_entry) {
    auto &object = kernel_object_instance.m_objects[id];
    object.created = true;
    object.kind = object_kind::PLAIN;
    object.flags = flags;
    object.parent_handler_object = parent_handler_object;
    object.parent_handler_entry = parent_handler_entry;
    object.methods[method] = entry;
  }
  static void set_creator(uint32_t id, uint32_t creator) {
    kernel_object_instance.m_objects[id].creator_object = creator;
  }
  static void set_kind(uint32_t id, object_kind kind) {
    kernel_object_instance.m_objects[id].kind = kind;
  }
  static void set_kernel_role(uint32_t id, bool role) {
    kernel_object_instance.m_objects[id].kernel_role = role;
  }
  static void set_parent_handler(uint32_t id, uint32_t parent, uintptr_t entry) {
    kernel_object_instance.m_objects[id].parent_handler_object = parent;
    kernel_object_instance.m_objects[id].parent_handler_entry = entry;
  }
  static void undeclare(uint32_t id, uintptr_t method) {
    kernel_object_instance.m_objects[id].methods[method] = nullptr;
  }
  static uint32_t budget(uint32_t thread) {
    return kernel_object_instance.m_budget[thread];
  }
  static void set_raw_budget(uint32_t thread, uint32_t value) {
    kernel_object_instance.m_budget[thread] = value;
  }
};
} // namespace selftest
}

extern "C" void shizuku_rv32_return_stub() {}

namespace shizuku::boards {
void esp32_c6::panic(const char *msg) {
  fprintf(stderr, "BOARD::panic called in host test: %s\n", msg);
  abort();
}
void esp32_c6::diag_printf(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vprintf(fmt, args);
  va_end(args);
}
uintptr_t esp32_c6::unprivileged_floor() { return (uintptr_t)-1; }
#if defined(SHIZUKU_TEST_REAL_KERNEL_OBJECT)
int esp32_c6::dma_claim() { std::abort(); }
void esp32_c6::dma_copy(int, const void *, void *, uint32_t) { std::abort(); }
bool esp32_c6::dma_busy(int) { std::abort(); }
void esp32_c6::dma_release(int) { std::abort(); }
uint64_t esp32_c6::time_us() { std::abort(); }
uint32_t esp32_c6::cycles_per_us() { std::abort(); }
#endif
}

#if defined(SHIZUKU_TEST_REAL_KERNEL_OBJECT)
namespace shizuku::archs {
using real_kobj_test_int_mode = shizuku::chips::esp32_c6::plic_vector_mode;
using real_kobj_test_arch = rv32<16, 4, real_kobj_test_int_mode>;
template <>
real_kobj_test_arch::syscall_result real_kobj_test_arch::syscall(
    uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  std::abort(); // Fail closed if this host test unexpectedly crosses the user syscall stub.
}
template <>
real_kobj_test_arch::syscall_result real_kobj_test_arch::syscall(
    uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  std::abort();
}
}
#endif

namespace shizuku::chips::esp32_c6 {
// debug_step(true) が掛かった回数 (consume_pending_step の fast path 試験が数える)。
volatile uint32_t g_debug_step_on = 0;
void plic_vector_mode::debug_step(bool on) { if (on) g_debug_step_on = g_debug_step_on + 1; }
uint32_t plic_vector_mode::debug_reason_take() { return 0; }
uint32_t plic_vector_mode::fault_status() { return 0; }
void plic_vector_mode::fault_status_clear() {}
uintptr_t plic_vector_mode::fault_address() { return 0; }
void plic_vector_mode::timer_oneshot(unsigned int) {}
// 試験が「タイマの残り」を決める (既定 0 = 装填した刻みを撃ち切った)。
volatile uint32_t g_timer_left = 0;
uint32_t plic_vector_mode::timer_remaining(bool &) { return g_timer_left; }
void plic_vector_mode::timer_cancel() {}
void plic_vector_mode::pend_context_switch() {}
template <>
bool plic_vector_mode::faulted_in_thread_mode<shizuku::archs::rv32_c6::context_t>(
    const shizuku::archs::rv32_c6::context_t &) { return true; }
#if defined(SHIZUKU_C6_UMODE)
static uintptr_t g_simulated_stack_base = 0;
static uintptr_t g_simulated_stack_limit = 0;
uintptr_t plic_vector_mode::kstack_top() { return 0x40812000u; }
void plic_vector_mode::switch_object_pmp(uint32_t) {}
bool plic_vector_mode::is_kstack_address(uintptr_t) { return false; }
uintptr_t plic_vector_mode::kstack_limit() { return 0; }
bool plic_vector_mode::user_stack_bounds(uint32_t, uintptr_t &base, uintptr_t &limit) {
  base = g_simulated_stack_base;
  limit = g_simulated_stack_limit;
  return base != 0 && limit > base;
}
template <class Context, class Frame>
void plic_vector_mode::enter_u_object(Context &, Frame &, uint32_t) {}
template void plic_vector_mode::enter_u_object<shizuku::archs::rv32_c6::context_t, shizuku::archs::rv32_c6::exception_frame_t>(
    shizuku::archs::rv32_c6::context_t &, shizuku::archs::rv32_c6::exception_frame_t &, uint32_t);
#endif
}

using namespace shizuku;

// ---- 呼出しの記録 (戻りの権威) を試験が直接据える・読む口 ---------------------------------------
//   既定の版は user stack 上のヘッダ (call_header_t) とスレッドの call_stack の識別欄、
//   SHIZUKU_LEAN_LEDGER の版は台帳の項目 (ret_* / generation / prev_call) と call_top。同じ試験が
//   どちらの版でも通ることを確かめるため、版の違いはここに閉じ込める。
//   台帳の index 段を call 段として据える (同定 + 識別の記帳。戻りの記録は fake_return_record)。
static void fake_call_entry(KERNEL::THREAD &t, uint32_t index, uint32_t object, uint32_t handler_object,
                            uintptr_t handler_entry, uint8_t via, uint16_t floor) {
#if defined(SHIZUKU_LEAN_LEDGER)
  KERNEL::call_ledger_entry entry = {object, handler_object, handler_entry,
                                     (uint8_t)object_kind::PLAIN, via, floor};
  entry.prev_call = (uint16_t)t.call_stack.call_top;
  t.ledger[index] = entry;
  t.call_stack.depth = index + 1;
  t.call_stack.call_count++;
  t.call_stack.call_top = index + 1;
#else
  t.ledger[index] = {object, handler_object, handler_entry, (uint8_t)object_kind::PLAIN, via, floor};
  t.call_stack.depth = index + 1;
  t.call_stack.caller_object =
      t.call_stack.call_count == 0 ? t.base_object : t.call_stack.current_call_object;
  t.call_stack.caller_generation =
      t.call_stack.call_count == 0 ? t.call_stack.base_object_generation : t.call_stack.current_call_generation;
  t.call_stack.current_call_object = object;
  t.call_stack.current_call_generation = 0;
  t.call_stack.call_count++;
#endif
}
//   index 段から戻るときの caller の再開点を据える (ctx->sp は caller の ecall 時点の trap フレーム)。
static void fake_return_record(KERNEL::CONTEXT &c, KERNEL::THREAD &t, uint32_t index, uintptr_t pc,
                               bool privileged) {
#if defined(SHIZUKU_LEAN_LEDGER)
  auto &e = t.ledger[index];
  e.ret_pc = pc;
  e.ret_sp = ARCH::call_return_sp(c);
  e.ret_privileged = privileged ? 1 : 0;
#else
  auto *header = ARCH::call_header_slot(c);
  *header = {};
  header->pc = pc;
  header->caller_object = (uint32_t)KERNEL_OBJECT::NO_OBJECT; // 呼び先を積む前の caller_object
  header->privileged = privileged ? 1 : 0;
  t.call_stack.top = (uintptr_t)header;
  (void)index;
#endif
}
//   最内の段の戻りの記録 (pc と caller の特権) を読む。
static uintptr_t top_return_pc(const KERNEL::THREAD &t) {
#if defined(SHIZUKU_LEAN_LEDGER)
  return t.ledger_top().ret_pc;
#else
  return ((const ARCH::call_header_t *)t.call_stack.top)->pc;
#endif
}
static bool top_return_privileged(const KERNEL::THREAD &t) {
#if defined(SHIZUKU_LEAN_LEDGER)
  return t.ledger_top().ret_privileged != 0;
#else
  return ((const ARCH::call_header_t *)t.call_stack.top)->privileged != 0;
#endif
}

static void run_dispatcher_tests() {
  printf("[TEST] Running V1 kernel dispatcher host tests...\n");
  selftest::dispatcher_test_access::init_descriptors();

  alignas(8) static uint8_t thread_storage[KERNEL::thread_record_bytes() * 4];
  kernel_instance.set_thread_storage(thread_storage, sizeof(thread_storage));

  alignas(8) static KERNEL::call_ledger_entry ledger[32];
  constexpr uintptr_t ROOT_HANDLER_ENTRY = 0x42010000;
  constexpr uint32_t ROOT_KOBJ_ID = 0;
  constexpr uint32_t ROOT_APP_ID = 1;
  constexpr uintptr_t CUSTOM_HANDLER_ENTRY = 0x42020000;
  constexpr uint32_t CUSTOM_HANDLER_ID = 42;
  constexpr uint32_t CHILD_ID = 43;

  kernel_instance.set_object_handler(ROOT_HANDLER_ENTRY, ROOT_KOBJ_ID);

  // -------------------------------------------------------------------------
  // 1. Root-bound thread at depth 0 (base level)
  // -------------------------------------------------------------------------
  kernel_instance.set_thread_ledger(
      0, ledger, 32, ROOT_APP_ID, (uint32_t)object_kind::PLAIN,
      ROOT_KOBJ_ID, ROOT_HANDLER_ENTRY);

  alignas(16) static uint8_t simulated_stack[16384]; // A→B→C の 3 段が余白検査に収まる大きさ
#if defined(SHIZUKU_C6_UMODE)
  chips::esp32_c6::g_simulated_stack_base = reinterpret_cast<uintptr_t>(simulated_stack);
  chips::esp32_c6::g_simulated_stack_limit = reinterpret_cast<uintptr_t>(simulated_stack + sizeof(simulated_stack));
#endif
  auto *ctx = kernel_instance.current_context();
  ARCH::stack_limit_set(*ctx, reinterpret_cast<uintptr_t>(simulated_stack));
  // exception_frame placed on stack with room for frames
  auto *frame = reinterpret_cast<ARCH::exception_frame_t *>(
      simulated_stack + sizeof(simulated_stack) - sizeof(ARCH::exception_frame_t) - 128);
  ctx->sp = frame;
  ctx->privileged = true;

  // 1a. GET_CURRENT_OBJECT (5) direct in trap
  frame->a0 = (uintptr_t)object_api::GET_CURRENT_OBJECT;
  frame->a1 = 0xDEADBEEF;
  kernel_instance.svc_dispatch(ctx);

  assert(frame->a0 == (uintptr_t)object_error::OK);
  assert(frame->a1 == ROOT_APP_ID);
  assert(kernel_instance.current_depth() == 0); // No dispatch frame pushed
  assert(ctx->privileged == true);
  printf("  1a. root direct GET_CURRENT_OBJECT (no dispatch frame, depth=0): PASS\n");

  // 1b. GET_CALLER_OBJECT (6) direct in trap at depth 0
  frame->a0 = (uintptr_t)object_api::GET_CALLER_OBJECT;
  frame->a1 = 0xDEADBEEF;
  kernel_instance.svc_dispatch(ctx);

  assert(frame->a0 == (uintptr_t)object_error::OK);
  assert(frame->a1 == (uintptr_t)KERNEL_OBJECT::NO_OBJECT);
  assert(kernel_instance.current_depth() == 0);
  printf("  1b. root direct GET_CALLER_OBJECT at base (returns NO_OBJECT, depth=0): PASS\n");

  // -------------------------------------------------------------------------
  // 2. Caller identity resolution across simulated call chains
  // -------------------------------------------------------------------------
  constexpr uint32_t LEAF_ID = 10;
  constexpr uint32_t NEST_ID = 20;

  // Simulate 1-hop call: caller ROOT_APP (1) -> callee LEAF (10)
  auto &thread = kernel_instance.current_thread();
  fake_call_entry(thread, 0, LEAF_ID, ROOT_KOBJ_ID, ROOT_HANDLER_ENTRY, KERNEL::LEDGER_CALL, 0);

  frame->a0 = (uintptr_t)object_api::GET_CURRENT_OBJECT;
  kernel_instance.svc_dispatch(ctx);
  assert(frame->a0 == (uintptr_t)object_error::OK);
  assert(frame->a1 == LEAF_ID);
  assert(kernel_instance.current_depth() == 1);

  frame->a0 = (uintptr_t)object_api::GET_CALLER_OBJECT;
  kernel_instance.svc_dispatch(ctx);
  assert(frame->a0 == (uintptr_t)object_error::OK);
  assert(frame->a1 == ROOT_APP_ID);
  assert(kernel_instance.current_depth() == 1);
  printf("  2a. 1-hop call caller identity (current=LEAF, caller=ROOT_APP, depth=1): PASS\n");

  // Simulate 2-hop nested call: ROOT_APP (1) -> LEAF (10) -> NEST (20)
  fake_call_entry(thread, 1, NEST_ID, ROOT_KOBJ_ID, ROOT_HANDLER_ENTRY, KERNEL::LEDGER_CALL, 1);

  frame->a0 = (uintptr_t)object_api::GET_CURRENT_OBJECT;
  kernel_instance.svc_dispatch(ctx);
  assert(frame->a0 == (uintptr_t)object_error::OK);
  assert(frame->a1 == NEST_ID);
  assert(kernel_instance.current_depth() == 2);

  frame->a0 = (uintptr_t)object_api::GET_CALLER_OBJECT;
  kernel_instance.svc_dispatch(ctx);
  assert(frame->a0 == (uintptr_t)object_error::OK);
  assert(frame->a1 == LEAF_ID);
  assert(kernel_instance.current_depth() == 2);
  printf("  2b. 2-hop nested call caller identity (current=NEST, caller=LEAF, depth=2): PASS\n");

  // Reset depth
  thread.call_stack.depth = 0;
  thread.call_stack.call_count = 0;
  thread.call_stack.call_top = 0;
  thread.call_stack.current_call_object = 0;
  thread.call_stack.caller_object = 0;

  // -------------------------------------------------------------------------
  // 3. Objectland same-number raw delivery to custom parent handler
  // -------------------------------------------------------------------------
  // Child has parent CUSTOM_HANDLER_ID (42) with CUSTOM_HANDLER_ENTRY
  kernel_instance.set_thread_ledger(
      0, ledger, 32, CHILD_ID, (uint32_t)object_kind::PLAIN,
      CUSTOM_HANDLER_ID, CUSTOM_HANDLER_ENTRY);

  // 3a. Child issues raw SVC 5 (same numeric ID as GET_CURRENT_OBJECT)
  ctx->sp = frame;
  frame->a0 = 5;
  frame->a1 = 0xAA;
  frame->a2 = 0xBB;
  frame->a3 = 0xCC;
  kernel_instance.svc_dispatch(ctx);

  assert(kernel_instance.current_depth() == 1); // Dispatch frame WAS pushed
  assert(ARCH::frame_pc(*ctx->sp) == CUSTOM_HANDLER_ENTRY); // Dispatched to custom handler
  assert(ARCH::arg(*ctx->sp, 0) == 5); // Raw number 5 passed unchanged
  assert(ARCH::arg(*ctx->sp, 1) == 0xAA);
  assert(thread.ledger[0].via == KERNEL::LEDGER_DISPATCH);
  assert(thread.ledger[0].object == CUSTOM_HANDLER_ID);
  printf("  3a. objectland raw SVC 5 delivered to custom handler (depth=1, pc=custom): PASS\n");

  // Pop the dispatch frame
  thread.call_stack.depth = 0;
  thread.call_stack.top = 0;
  thread.call_stack.call_top = 0;
  ctx->sp = frame;

  // 3b. Child issues raw SVC 6 (same numeric ID as GET_CALLER_OBJECT)
  frame->a0 = 6;
  kernel_instance.svc_dispatch(ctx);
  assert(kernel_instance.current_depth() == 1);
  assert(ARCH::frame_pc(*ctx->sp) == CUSTOM_HANDLER_ENTRY);
  assert(ARCH::arg(*ctx->sp, 0) == 6);
  printf("  3b. objectland raw SVC 6 delivered to custom handler (depth=1, pc=custom): PASS\n");

  thread.call_stack.depth = 0;
  thread.call_stack.top = 0;
  thread.call_stack.call_top = 0;
  ctx->sp = frame;

  // 3c. Child issues raw SVC 16 (CMD_COMPUTE / MEMORY_OWNER number)
  frame->a0 = 16;
  kernel_instance.svc_dispatch(ctx);
  assert(kernel_instance.current_depth() == 1);
  assert(ARCH::frame_pc(*ctx->sp) == CUSTOM_HANDLER_ENTRY);
  assert(ARCH::arg(*ctx->sp, 0) == 16);
  printf("  3c. objectland raw SVC 16 (CMD_COMPUTE) delivered to custom handler: PASS\n");

  thread.call_stack.depth = 0;
  thread.call_stack.top = 0;
  thread.call_stack.call_top = 0;
  ctx->sp = frame;

  // 3d/e. The V2 descriptor API numbers remain objectland numbers for a custom
  // parent handler: the kernel must preserve and deliver each raw request.
  const uintptr_t descriptor_api_ids[] = {
      (uintptr_t)object_api::CREATE_METHOD_DESCRIPTOR,
      (uintptr_t)object_api::CALL_METHOD_DESCRIPTOR};
  for (uintptr_t raw_id : descriptor_api_ids) {
    frame->a0 = raw_id;
    frame->a1 = 0x11223344;
    frame->a2 = 0x55667788;
    kernel_instance.svc_dispatch(ctx);
    assert(kernel_instance.current_depth() == 1);
    assert(ARCH::frame_pc(*ctx->sp) == CUSTOM_HANDLER_ENTRY);
    assert(ARCH::arg(*ctx->sp, 0) == raw_id);
    assert(ARCH::arg(*ctx->sp, 1) == 0x11223344);
    assert(ARCH::arg(*ctx->sp, 2) == 0x55667788);
    printf("  objectland raw SVC %lu delivered unchanged to custom handler: PASS\n",
           (unsigned long)raw_id);
    thread.call_stack.depth = 0;
    thread.call_stack.top = 0;
    thread.call_stack.call_top = 0;
    ctx->sp = frame;
  }

  // -------------------------------------------------------------------------
  // 4. Mode=thread actor path & unmigrated mode=handler API on root-bound object
  // -------------------------------------------------------------------------
  kernel_instance.set_thread_ledger(
      0, ledger, 32, ROOT_APP_ID, (uint32_t)object_kind::PLAIN,
      ROOT_KOBJ_ID, ROOT_HANDLER_ENTRY);

  // 4a. YIELD (8) で grant 段が無い (貸し手が居ない = 起点のスレッド): trap では処理せず
  //     従来どおり kobj (schedule) へ転送する。
  assert(!kernel_instance.grant_active());
  frame->a0 = (uintptr_t)object_api::YIELD;
  kernel_instance.svc_dispatch(ctx);
  assert(kernel_instance.current_depth() == 1);
  assert(ARCH::frame_pc(*ctx->sp) == ROOT_HANDLER_ENTRY);
  assert(ARCH::arg(*ctx->sp, 0) == (uintptr_t)object_api::YIELD);
  printf("  4a. YIELD without a grant frame is forwarded to kobj (depth=1, pc=root): PASS\n");

  thread.call_stack.depth = 0;
  thread.call_stack.top = 0;
  thread.call_stack.call_top = 0;
  ctx->sp = frame;

  // 4b. MEMORY_OWNER (16): mode == svc_mode::thread (O(1) でない / 錠を取るので trap には入れない)
  frame->a0 = (uintptr_t)object_api::MEMORY_OWNER;
  kernel_instance.svc_dispatch(ctx);
  assert(kernel_instance.current_depth() == 1);
  assert(ARCH::frame_pc(*ctx->sp) == ROOT_HANDLER_ENTRY);
  assert(ARCH::arg(*ctx->sp, 0) == (uintptr_t)object_api::MEMORY_OWNER);
  printf("  4b. mode=thread MEMORY_OWNER remains on actor path: PASS\n");

  thread.call_stack.depth = 0;
  thread.call_stack.top = 0;
  thread.call_stack.call_top = 0;
  ctx->sp = frame;

  // -------------------------------------------------------------------------
  // 5. Unprivileged origin: privilege preservation & privileged API rejection
  // -------------------------------------------------------------------------
  ctx->privileged = false;
#if defined(SHIZUKU_C6_UMODE)
  ARCH::set_u_stack_pointer(*frame, reinterpret_cast<uintptr_t>(frame) + sizeof(ARCH::exception_frame_t));
#endif

  // 5a. Direct GET with unprivileged origin
  frame->a0 = (uintptr_t)object_api::GET_CURRENT_OBJECT;
  kernel_instance.svc_dispatch(ctx);
  assert(frame->a0 == (uintptr_t)object_error::OK);
  assert(frame->a1 == ROOT_APP_ID);
  assert(kernel_instance.current_depth() == 0);
  assert(ctx->privileged == false); // Privilege NOT elevated!
  printf("  5a. unprivileged origin calling GET: privilege NOT elevated (privileged=false): PASS\n");

  // 5b. Unprivileged calling mode=thread API (GRANT_REGION = 23, privileged_only)
  // Must NOT be rejected early in trap path: passed through to root actor handler
  // with depth == 1, same API ID and arguments, and caller privilege unchanged in saved context.
  // Note: the ultimate NOT_PRIVILEGED decision is enforced downstream by KERNEL_OBJECT::grant_region
  // (tested in source/selftest/unprivileged.cpp:111).
  frame->a0 = (uintptr_t)object_api::GRANT_REGION;
  frame->a1 = 0x1234;
  frame->a2 = 0x1000;
  frame->a3 = 0x2000;
  kernel_instance.svc_dispatch(ctx);
  assert(kernel_instance.current_depth() == 1);
  assert(ARCH::frame_pc(*ctx->sp) == ROOT_HANDLER_ENTRY);
  assert(ARCH::arg(*ctx->sp, 0) == (uintptr_t)object_api::GRANT_REGION);
  assert(ARCH::arg(*ctx->sp, 1) == 0x1234);
  assert(ARCH::arg(*ctx->sp, 2) == 0x1000);
  assert(ARCH::arg(*ctx->sp, 3) == 0x2000);
  assert(!top_return_privileged(thread)); // Caller privilege preserved as unprivileged!

  // Return from actor handler via primitive::RETURN (with RETURN_STUB_MARKER),
  // verifying caller's unprivileged context is restored upon frame pop.
  ctx->sp->a0 = (uintptr_t)primitive::RETURN;
  ctx->sp->a4 = RETURN_STUB_MARKER;
  kernel_instance.svc_dispatch(ctx);
  assert(kernel_instance.current_depth() == 0);
  assert(ctx->privileged == false); // Restored caller context is unprivileged!
  printf("  5b. unprivileged mode=thread API (GRANT_REGION): passed to actor handler (depth=1, saved priv=false, restored priv=false): PASS\n");

  // -------------------------------------------------------------------------
  // 6. EXIT_METHOD (2): callee の return stub (marker 付き RETURN) の trap 内処理
  // -------------------------------------------------------------------------
  // 宛先が root の戻りは、LEDGER_CALL / LEDGER_DESCRIPTOR_CALL のどちらの段でも handler を
  // 再入させず caller へ直接戻す。検証に落ちたら 1 枚も落とさず UNWIND_REJECTED を返す。
  // 専用親ハンドラ宛ては従来経路。
  constexpr uintptr_t A_PC_6 = 0x42000200;
  auto arm_frame = [&](uint8_t via, uint16_t unwind_floor, ARCH::exception_frame_t *callee) {
    ctx->sp = frame;
    ctx->privileged = true;
    // 呼出しの記録: caller (sp = frame + trap フレーム) の再開点を据える (既定の版は caller stub が
    // 空けた枠のヘッダ、SHIZUKU_LEAN_LEDGER の版は台帳の項目)。
    thread.call_stack.depth = 0;
    thread.call_stack.call_count = 0;
    thread.call_stack.call_top = 0;
    fake_call_entry(thread, 0, LEAF_ID, ROOT_KOBJ_ID, ROOT_HANDLER_ENTRY, via, unwind_floor);
    fake_return_record(*ctx, thread, 0, A_PC_6, true);
    callee->a0 = (uintptr_t)primitive::RETURN;
    callee->a1 = 1;
    callee->a2 = 0x77;
    callee->a3 = 0;
    callee->a4 = RETURN_STUB_MARKER;
    ctx->sp = callee;
  };
  // Keep enough lower-stack room for the fallback dispatch frame in 6b-6d.
  auto *callee = frame - 2;

  // 6a. descriptor 段の戻りは caller へ直接戻る。dispatch 段は積まれず、戻り値は caller の a0/a1 に載る。
  arm_frame(KERNEL::LEDGER_DESCRIPTOR_CALL, 0, callee);
  kernel_instance.svc_dispatch(ctx);
  assert(kernel_instance.current_depth() == 0);
  assert(thread.call_stack.call_count == 0);
  assert(thread.identity_object() == ROOT_APP_ID); // identity を push 時の値へ戻す
  assert(kernel_instance.caller_object(kernel_instance.current_thread_id()) ==
         (uintptr_t)KERNEL_OBJECT::NO_OBJECT);
  assert(ctx->sp == frame);
  assert(frame->a0 == (uintptr_t)kernel_error::OK);
  assert(frame->a1 == 0x77);
  assert(ctx->privileged == true);
  printf("  6a. descriptor return fast path (depth 1->0, caller frame, value 0x77, no dispatch frame): PASS\n");

  // 6b. LEDGER_CALL (通常 CALL) の戻りも同じ。handler を再入させない。
  thread.call_stack.depth = 0;
  thread.call_stack.call_count = 0;
  thread.call_stack.top = 0;
  thread.call_stack.call_top = 0;
  arm_frame(KERNEL::LEDGER_CALL, 0, callee);
  kernel_instance.svc_dispatch(ctx);
  assert(kernel_instance.current_depth() == 0); // dispatch 段は積まれない
  assert(thread.call_stack.call_count == 0);
  assert(thread.identity_object() == ROOT_APP_ID);
  assert(ctx->sp == frame);
  assert(frame->a0 == (uintptr_t)object_error::OK);
  assert(frame->a1 == 0x77);
  printf("  6b. plain CALL return handled in trap (depth 1->0, value 0x77, no dispatch frame): PASS\n");

  // 6c. 戻り先が unwind floor を割る (d = floor = 1 >= depth = 1) 段は 1 枚も落とさず
  // UNWIND_REJECTED + kernel_error::DEPTH_MISMATCH を呼び出し元 (callee フレーム) へ返す。
  thread.call_stack.depth = 0;
  thread.call_stack.call_count = 0;
  thread.call_stack.top = 0;
  thread.call_stack.call_top = 0;
  arm_frame(KERNEL::LEDGER_DESCRIPTOR_CALL, 1, callee);
  kernel_instance.svc_dispatch(ctx);
  assert(kernel_instance.current_depth() == 1); // 落としていない・dispatch 段も積まない
  assert(thread.call_stack.call_count == 1);
  assert(ctx->sp == callee);
  assert(callee->a0 == (uintptr_t)object_error::UNWIND_REJECTED);
  assert(callee->a1 == (uintptr_t)kernel_error::DEPTH_MISMATCH);
  printf("  6c. return below unwind floor rejected in trap (depth stays 1, UNWIND_REJECTED/DEPTH_MISMATCH): PASS\n");

  // 6d. 親ハンドラが専用ハンドラ (root 以外) の descriptor 段は、子の戻りを専用ハンドラへ見せるため従来経路へ落ちる。
  thread.call_stack.depth = 0;
  thread.call_stack.call_count = 0;
  thread.call_stack.top = 0;
  thread.call_stack.call_top = 0;
  arm_frame(KERNEL::LEDGER_DESCRIPTOR_CALL, 0, callee);
  thread.ledger[0].handler_object = CUSTOM_HANDLER_ID;
  thread.ledger[0].handler_entry = CUSTOM_HANDLER_ENTRY;
  kernel_instance.svc_dispatch(ctx);
  assert(kernel_instance.current_depth() == 2);
  assert(thread.ledger[1].object == CUSTOM_HANDLER_ID);
  assert(thread.ledger[1].kind == (uint8_t)object_kind::HANDLER);
  assert(ARCH::frame_pc(*ctx->sp) == CUSTOM_HANDLER_ENTRY);
  printf("  6d. descriptor return to dedicated parent handler keeps handler path (depth=2, pc=custom): PASS\n");

  // -------------------------------------------------------------------------
  // 6e. スレッドの最外のメソッドが戻った (d == 0 かつ呼出し段なし) 場合は「スレッドの終わり」で、
  //     kobj の exit_thread に任せるため従来どおり dispatch 段を積んで root handler へ届ける。
  //     marker 付き (return stub) と、明示 d = 0 の API 呼出しの両方。
  // -------------------------------------------------------------------------
  auto fresh = [&](uint32_t base_object, bool privileged) {
    kernel_instance.set_thread_ledger(0, ledger, 32, base_object,
                                      (uint32_t)object_kind::PLAIN, ROOT_KOBJ_ID,
                                      ROOT_HANDLER_ENTRY);
    ctx->sp = frame;
    ctx->privileged = privileged;
    frame->a4 = 0;
#if defined(SHIZUKU_C6_UMODE)
    ARCH::set_u_stack_pointer(*frame, reinterpret_cast<uintptr_t>(frame) + sizeof(ARCH::exception_frame_t));
#endif
  };
  fresh(ROOT_APP_ID, true);
  frame->a0 = (uintptr_t)primitive::RETURN;
  frame->a1 = 1;
  frame->a2 = 0x99;
  frame->a3 = 0;
  frame->a4 = RETURN_STUB_MARKER;
  kernel_instance.svc_dispatch(ctx);
  assert(kernel_instance.current_depth() == 1);
  assert(thread.ledger[0].via == KERNEL::LEDGER_DISPATCH);
  assert(ARCH::frame_pc(*ctx->sp) == ROOT_HANDLER_ENTRY);
  assert(ARCH::arg(*ctx->sp, 0) == (uintptr_t)object_api::EXIT_METHOD);
  assert(ARCH::arg(*ctx->sp, 1) == 0); // marker は a1 を unwind floor (0) に置き換える
  fresh(ROOT_APP_ID, true);
  frame->a0 = (uintptr_t)object_api::EXIT_METHOD;
  frame->a1 = 0;
  frame->a2 = 0x98;
  kernel_instance.svc_dispatch(ctx);
  assert(kernel_instance.current_depth() == 1);
  assert(thread.ledger[0].via == KERNEL::LEDGER_DISPATCH);
  assert(ARCH::frame_pc(*ctx->sp) == ROOT_HANDLER_ENTRY);
  assert(ARCH::arg(*ctx->sp, 1) == 0 && ARCH::arg(*ctx->sp, 2) == 0x98);
  printf("  6e. thread-base return (d=0, no call frames) still takes the kobj exit_thread path (depth=1, pc=root): PASS\n");

  // -------------------------------------------------------------------------
  // 7. svc_attributes: trap 内で処理する (mode == handler) 番号の集合
  // -------------------------------------------------------------------------
  // ユーザー決定: 2, 4, 5, 6, 8, 9, 12, 27, 33 (10, 11 はスケジューラ設計待ちで thread のまま。
  // 16, 18, 20, 29, 30, 31 は O(1) でない / 錠を取るので thread に戻した)。
  for (unsigned id = 1; id < 34; ++id) {
    const bool expect_handler = id == 2 || id == 4 || id == 5 || id == 6 ||
                                id == 8 || id == 9 || id == 12 || id == 27 ||
                                id == 33;
    assert((svc_attribute_for(id)->mode == svc_mode::handler) == expect_handler);
  }
  printf("  7. svc_attributes handler set == {2,4,5,6,8,9,12,27,33}: PASS\n");

  // -------------------------------------------------------------------------
  // 8. CALL_METHOD (4) / EXIT_METHOD (2): A (base) → B → C
  // -------------------------------------------------------------------------
  using access = selftest::dispatcher_test_access;
  constexpr uint32_t B_ID = LEAF_ID, C_ID = NEST_ID;
  constexpr uintptr_t ENTRY_B = 0x42030000, ENTRY_C = 0x42031000;
  access::declare(ROOT_APP_ID, 0, reinterpret_cast<KERNEL_OBJECT::method_t>(0x42000000), 0,
                  ROOT_KOBJ_ID, ROOT_HANDLER_ENTRY);
  access::declare(B_ID, 0, reinterpret_cast<KERNEL_OBJECT::method_t>(ENTRY_B), 0,
                  ROOT_KOBJ_ID, ROOT_HANDLER_ENTRY);
  access::declare(C_ID, 0, reinterpret_cast<KERNEL_OBJECT::method_t>(ENTRY_C),
                  OBJECT_UNPRIVILEGED, ROOT_KOBJ_ID, ROOT_HANDLER_ENTRY);
  // 親子関係の樹立: A (ROOT_APP_ID) が B の親、B が C の親 (8c の B->C 呼出しのため)。
  // B に kernel_role は付けず、正当な親子関係 (A->B, B->C) で検証する。
  access::set_creator(B_ID, ROOT_APP_ID);
  access::set_creator(C_ID, B_ID);
  auto count_dispatch_frames = [&]() {
    unsigned n = 0;
    for (uint32_t i = 0; i < thread.call_stack.depth; ++i)
      n += thread.ledger[i].via == KERNEL::LEDGER_DISPATCH;
    return n;
  };

  // ABI (2026-10-22): method call は caller の frame をコピーせず、レジスタは全部 callee への入力。
  //   カーネルはルータに徹し、入口も戻りもレジスタを素通しする (消すのは callee が自分の意思で
  //   戻らない巻き戻しだけ)。以下の補助は、引数・戻り口・sp/pc 以外の全レジスタに印をつけ、
  //   通り抜けを確かめる。
  constexpr uintptr_t MARK = 0xA5000000u;
  auto mark_regs = [&](KERNEL::FRAME *f, uintptr_t seed) {
    f->ra = MARK | (seed + 1); f->t0 = MARK | (seed + 2); f->t1 = MARK | (seed + 3);
    f->t2 = MARK | (seed + 4); f->t3 = MARK | (seed + 5); f->t4 = MARK | (seed + 6);
    f->t5 = MARK | (seed + 7); f->t6 = MARK | (seed + 8); f->a6 = MARK | (seed + 9);
    f->a7 = MARK | (seed + 10); f->gp = MARK | (seed + 11); f->tp = MARK | (seed + 12);
    f->s0 = MARK | (seed + 13); f->s1 = MARK | (seed + 14); f->s2 = MARK | (seed + 15);
    f->s3 = MARK | (seed + 16); f->s4 = MARK | (seed + 17); f->s5 = MARK | (seed + 18);
    f->s6 = MARK | (seed + 19); f->s7 = MARK | (seed + 20); f->s8 = MARK | (seed + 21);
    f->s9 = MARK | (seed + 22); f->s10 = MARK | (seed + 23); f->s11 = MARK | (seed + 24);
  };
  // 呼び先の入口で、引数と ra(戻り口)・pc 以外が caller の値のまま通っているか。
  auto regs_pass_through = [&](const KERNEL::FRAME *f, uintptr_t seed) {
    return f->t0 == (MARK | (seed + 2)) && f->t1 == (MARK | (seed + 3)) &&
           f->t2 == (MARK | (seed + 4)) && f->t3 == (MARK | (seed + 5)) &&
           f->t4 == (MARK | (seed + 6)) && f->t5 == (MARK | (seed + 7)) &&
           f->t6 == (MARK | (seed + 8)) && f->a6 == (MARK | (seed + 9)) &&
           f->a7 == (MARK | (seed + 10)) && f->gp == (MARK | (seed + 11)) &&
           f->tp == (MARK | (seed + 12)) && f->s0 == (MARK | (seed + 13)) &&
           f->s1 == (MARK | (seed + 14)) && f->s2 == (MARK | (seed + 15)) &&
           f->s3 == (MARK | (seed + 16)) && f->s4 == (MARK | (seed + 17)) &&
           f->s5 == (MARK | (seed + 18)) && f->s6 == (MARK | (seed + 19)) &&
           f->s7 == (MARK | (seed + 20)) && f->s8 == (MARK | (seed + 21)) &&
           f->s9 == (MARK | (seed + 22)) && f->s10 == (MARK | (seed + 23)) &&
           f->s11 == (MARK | (seed + 24));
  };
  // 戻った caller の再開フレームが乗る出口 (目印)。カーネルはルータに徹し、普通の戻り (callee が自分で
  // 戻る 1 段) は RV32_TAG_ECALL (素通し: a0/a1 以外を消さない) の出口に乗せ、callee が自分の意思で
  // 戻らない巻き戻し (2 段以上の EXIT_METHOD / RETURN、FORWARD_CHILD_EXIT) だけ RV32_TAG_ECALL_SCRUB
  // (a0/a1 以外を 0 に消す) の出口に乗せる。再開フレームは出口が読む mepc / mstatus / a0 / a1 と
  // 目印だけを書く (フレームの残りは書かない)。出口がレジスタを消す/消さないことは
  // test_trap_static.py が asm で確かめる。ここでは再開フレームが必ず正しい出口に乗ること (目印) を見る。
  auto resumes_passthrough = [&](const KERNEL::FRAME *f) {
    return f->mcause == RV32_TAG_ECALL;
  };
  auto resumes_scrubbed = [&](const KERNEL::FRAME *f) {
    return f->mcause == RV32_TAG_ECALL_SCRUB;
  };
  constexpr uintptr_t A_PC = 0x42000100; // A が ecall した次の命令

  // 8a. A → B。dispatch 段は積まれず、台帳の最上段は B の LEDGER_CALL。
  //     frame はコピーされず (ctx->sp == frame、ヘッダ無し)、レジスタは B にそのまま通る。
  fresh(ROOT_APP_ID, true);
  mark_regs(frame, 0x100);
  frame->mepc = A_PC;
  frame->a0 = (uintptr_t)object_api::CALL_METHOD;
  frame->a1 = B_ID;
  frame->a2 = 0;
  frame->a3 = 0x55;
  frame->a4 = 0x44; // caller の入力 (呼び先の入口で 0 埋めされず、そのまま通る)
  frame->a5 = 0x45;
  kernel_instance.svc_dispatch(ctx);
  assert(kernel_instance.current_depth() == 1);
  assert(count_dispatch_frames() == 0);
  assert(thread.ledger[0].via == KERNEL::LEDGER_CALL);
  assert(thread.ledger[0].object == B_ID);
  assert(thread.ledger[0].kind == (uint8_t)object_kind::PLAIN);
  assert(thread.ledger[0].unwind_floor == 0);
  assert(thread.ledger[0].handler_object == ROOT_KOBJ_ID);
  assert(thread.ledger[0].handler_entry == ROOT_HANDLER_ENTRY);
  assert(thread.call_stack.call_count == 1);
  assert(ARCH::frame_pc(*ctx->sp) == ENTRY_B);
  assert(ARCH::arg(*ctx->sp, 0) == 0x55); // 引数 a3 は呼び先の第 1 引数
  assert(ctx->sp == frame); // frame をコピーしない (in-place)
  {
    // 呼出しの記録は caller の枠 (trap フレームの直上 = caller の ecall 時点の SP) にある。
    // カーネルが持つのはその先頭へのポインタと深さだけ。callee の窓の上端は枠の下端 (= 枠のアドレス)。
#if defined(SHIZUKU_LEAN_LEDGER)
    // SHIZUKU_LEAN_LEDGER の版: user stack にヘッダは無い。戻りの権威は台帳の項目 (特権メモリ) に 1 つ。
    const auto &rec = thread.ledger[0];
    assert(rec.ret_sp == reinterpret_cast<uintptr_t>(frame) + sizeof(KERNEL::FRAME));
    assert(rec.prev_call == 0 && rec.ret_pc == A_PC && rec.ret_privileged == 1);
    assert(rec.generation == 0);
    assert(thread.call_stack.call_top == 1 && thread.call_stack.call_count == 1);
    assert(thread.identity_object() == B_ID && thread.call_caller_object() == ROOT_APP_ID /* fresh() の base */);
#else
    const auto *h = (const ARCH::call_header_t *)thread.call_stack.top;
    assert((uintptr_t)h == reinterpret_cast<uintptr_t>(frame) + sizeof(KERNEL::FRAME));
    assert(ARCH::call_window_top(h) == (uintptr_t)h);
    assert(h->prev == 0 && h->pc == A_PC && h->privileged == 1);
    // via / unwind_floor はヘッダに書かない (台帳が持つ。上で thread.ledger[0] を検査済み)。
    assert(h->caller_object == 0 /* fresh() の caller_object */);
#endif
  }
  // ra は 32bit レジスタ。host (64bit) では関数ポインタが切り詰められるので下位 32bit で比べる。
  assert(frame->ra == (uint32_t)ARCH::return_stub()); // 戻り口はカーネルが決める
  assert(regs_pass_through(frame, 0x100)); // ra・引数・pc 以外は caller の値が通る
  // 呼び先の引数は a0 だけをカーネルが決める。a1 以降は 0 埋めせず、caller の ecall 入力が素通し
  // (呼び先は宣言した引数以外を信用しない)。a1 = 対象 ID、a2 = メソッド番号、a3 = 引数、
  // a4/a5 は mark_regs が置いた caller の値のまま。
  assert(frame->a1 == B_ID && frame->a2 == 0 && frame->a3 == 0x55);
  assert(frame->a4 == 0x44 && frame->a5 == 0x45);
  assert(ctx->privileged == true); // B は特権オブジェクト
  KERNEL::FRAME *frame_b = ctx->sp;
  printf("  8a. CALL_METHOD A->B: no dispatch frame, in-place (frame not copied, no header), registers pass through (a1-a5 not zeroed), ledger top = B via LEDGER_CALL, pc=B entry, arg0=0x55: PASS\n");

  // 8b. B が GET_CALLER_OBJECT を撃つと本当の呼び出し元 A (kobj の ID 0 ではない)。
  frame_b->a0 = (uintptr_t)object_api::GET_CALLER_OBJECT;
  kernel_instance.svc_dispatch(ctx);
  assert(frame_b->a0 == (uintptr_t)object_error::OK);
  assert(frame_b->a1 == ROOT_APP_ID && frame_b->a1 != ROOT_KOBJ_ID);
  frame_b->a0 = (uintptr_t)object_api::GET_CURRENT_OBJECT;
  kernel_instance.svc_dispatch(ctx);
  assert(frame_b->a1 == B_ID);

  // 8c. B → C (C は非特権オブジェクト)。C から見た caller は B。
  // B は自分のスタックを 400 バイト使ってから C を呼ぶ (呼出しの記録の枠は B の枠とは別)。
  auto *frame_b_call = reinterpret_cast<KERNEL::FRAME *>(reinterpret_cast<uint8_t *>(frame_b) - 400);
  ctx->sp = frame_b_call;
  frame_b_call->mepc = ENTRY_B; // B が C を呼んだ点 (試験は ecall の +4 を模さない)
  frame_b_call->a0 = (uintptr_t)object_api::CALL_METHOD;
  frame_b_call->a1 = C_ID;
  frame_b_call->a2 = 0;
  frame_b_call->a3 = 0x66;
  kernel_instance.svc_dispatch(ctx);
  frame_b = frame_b_call; // C から戻ったとき B が再開するのは、B が ecall した枠の直下
  assert(kernel_instance.current_depth() == 2);
  assert(count_dispatch_frames() == 0);
  assert(thread.ledger[1].via == KERNEL::LEDGER_CALL);
  assert(thread.ledger[1].object == C_ID);
  assert(thread.ledger[1].unwind_floor == 1);
  assert(thread.call_stack.call_count == 2);
  assert(ARCH::frame_pc(*ctx->sp) == ENTRY_C);
  assert(ARCH::arg(*ctx->sp, 0) == 0x66);
  assert(ctx->privileged == false); // C は OBJECT_UNPRIVILEGED
  KERNEL::FRAME *frame_c = ctx->sp;
  frame_c->a0 = (uintptr_t)object_api::GET_CALLER_OBJECT;
  kernel_instance.svc_dispatch(ctx);
  assert(frame_c->a0 == (uintptr_t)object_error::OK);
  assert(frame_c->a1 == B_ID); // A でも kobj (0) でもなく B
  frame_c->a0 = (uintptr_t)object_api::GET_CURRENT_OBJECT;
  kernel_instance.svc_dispatch(ctx);
  assert(frame_c->a1 == C_ID);
  assert(kernel_instance.current_depth() == 2); // GET は段を積まない
  printf("  8c. A->B->C: C sees GET_CALLER_OBJECT == B, GET_CURRENT_OBJECT == C (no dispatch frames, depth=2): PASS\n");

  // 8d. 明示 d の EXIT_METHOD。C の台帳の unwind floor は 1。
  //     d = 0 (floor 未満) と d = 2 (現在深さ以上) は 1 枚も落とさず UNWIND_REJECTED/DEPTH_MISMATCH。
  for (uintptr_t bad_d : {(uintptr_t)0, (uintptr_t)2}) {
    frame_c->a0 = (uintptr_t)object_api::EXIT_METHOD;
    frame_c->a1 = bad_d;
    frame_c->a2 = 0x31;
    frame_c->a3 = 0;
    frame_c->a4 = 0;
    kernel_instance.svc_dispatch(ctx);
    assert(kernel_instance.current_depth() == 2);
    assert(thread.call_stack.call_count == 2);
    assert(ctx->sp == frame_c);
    assert(frame_c->a0 == (uintptr_t)object_error::UNWIND_REJECTED);
    assert(frame_c->a1 == (uintptr_t)kernel_error::DEPTH_MISMATCH);
  }
  // d = 1: C の 1 段だけ落として B へ戻る。値 a2 と error a3 は B のフレームに載る。
  //     1 段の普通の戻りなので素通しの出口 (RV32_TAG_ECALL)。B の pc は B が C を呼んだ点。
  mark_regs(frame_c, 0x200);
  frame_c->a0 = (uintptr_t)object_api::EXIT_METHOD;
  frame_c->a1 = 1;
  frame_c->a2 = 0x32;
  frame_c->a3 = 0;
  frame_c->a4 = 0;
  frame_c->a5 = 0;
  frame_c->a6 = 0;
  frame_c->a7 = 0;
  kernel_instance.svc_dispatch(ctx);
  assert(kernel_instance.current_depth() == 1);
  assert(thread.call_stack.call_count == 1);
  assert(count_dispatch_frames() == 0);
  assert(ctx->sp == frame_b);
  assert(frame_b->a0 == (uintptr_t)object_error::OK);
  assert(frame_b->a1 == 0x32);
  assert(resumes_passthrough(frame_b));
  assert(frame_b->mepc == ENTRY_B); // B が C を呼んだ時点の pc (試験は ecall の +4 を模さない)
  assert(ctx->privileged == true); // B の文脈 (特権) が戻る
  frame_b->a0 = (uintptr_t)object_api::GET_CALLER_OBJECT;
  kernel_instance.svc_dispatch(ctx);
  assert(frame_b->a1 == ROOT_APP_ID); // 戻ったので B から見た caller は再び A
  printf("  8d. EXIT_METHOD explicit d: d=0/d=2 rejected (nothing popped), d=1 returns C->B with value 0x32, caller identity restored: PASS\n");

  // 8e. B が return stub (marker) で戻る → A へ直接。error (a3) と値 (a2) が A のフレームへ。
  mark_regs(frame_b, 0x300);
  frame_b->a0 = (uintptr_t)primitive::RETURN;
  frame_b->a1 = 1;
  frame_b->a2 = 0x44;
  frame_b->a3 = 0;
  frame_b->a4 = RETURN_STUB_MARKER;
  kernel_instance.svc_dispatch(ctx);
  assert(kernel_instance.current_depth() == 0);
  assert(thread.call_stack.call_count == 0);
  assert(ctx->sp == frame);
  assert(frame->a0 == (uintptr_t)object_error::OK);
  assert(frame->a1 == 0x44);
  assert(resumes_passthrough(frame)); // B の値は A に見えない。A の値も戻らない (A が自分で復元する)
  assert(frame->mepc == A_PC); // A の ecall の次の命令から再開
  assert(thread.identity_object() == ROOT_APP_ID);
  printf("  8e. B return stub -> A directly (depth 0, value 0x44, no dispatch frame), plain 1-level return resumes on the pass-through exit (RV32_TAG_ECALL), resume pc = A_PC: PASS\n");

  // 8f. エラー応答: (error, 0) を呼び出し元へ。段は積まれず、特権も変わらない。
  struct error_case {
    uintptr_t id, method;
    object_error expect;
  };
  const error_case cases[] = {
      {KERNEL_OBJECT::OBJECT_COUNT, 0, object_error::BAD_OBJECT}, // 範囲外
      {7, 0, object_error::BAD_OBJECT},                           // 未生成
      {B_ID, KERNEL_OBJECT::METHOD_COUNT, object_error::BAD_METHOD},
      {B_ID, 1, object_error::UNDECLARED_METHOD}, // 未 export
  };
  for (bool privileged : {true, false}) {
    for (const auto &c : cases) {
      fresh(ROOT_APP_ID, privileged);
      frame->a0 = (uintptr_t)object_api::CALL_METHOD;
      frame->a1 = c.id;
      frame->a2 = c.method;
      frame->a3 = 0x77;
      kernel_instance.svc_dispatch(ctx);
      assert(kernel_instance.current_depth() == 0);
      assert(ctx->sp == frame);
      assert(frame->a0 == (uintptr_t)c.expect);
      assert(frame->a1 == 0);
      assert(ctx->privileged == privileged);
    }
    // NO_STACK: 呼出し段が MAX_DEPTH に達している。
    // (呼び出し元の識別は call_count != 0 なので current_call_object / ledger から引く)
    fresh(ROOT_APP_ID, privileged);
#if defined(SHIZUKU_LEAN_LEDGER)
    thread.ledger[0].object = ROOT_APP_ID;
    thread.ledger[0].generation = 0;
    thread.ledger[0].prev_call = 0;
    thread.call_stack.call_top = 1;
#else
    thread.call_stack.current_call_object = ROOT_APP_ID;
    thread.call_stack.current_call_generation = 0;
#endif
    thread.call_stack.call_count = KERNEL_OBJECT::MAX_DEPTH;
    frame->a0 = (uintptr_t)object_api::CALL_METHOD;
    frame->a1 = B_ID;
    frame->a2 = 0;
    kernel_instance.svc_dispatch(ctx);
    assert(kernel_instance.current_depth() == 0);
    assert(frame->a0 == (uintptr_t)object_error::NO_STACK);
    assert(frame->a1 == 0);
    assert(ctx->privileged == privileged);
#if defined(SHIZUKU_LEAN_LEDGER)
    thread.call_stack.call_top = 0;
#else
    thread.call_stack.current_call_object = 0;
#endif
    thread.call_stack.call_count = 0;
  }
  printf("  8f. CALL_METHOD errors BAD_OBJECT x2 / BAD_METHOD / UNDECLARED_METHOD / NO_STACK reply (error,0), depth 0, privilege unchanged: PASS\n");

  // 8g. 非特権の呼び出し元が非特権オブジェクトを呼んでも特権化しない。戻りで元の文脈。
  // 8g では A から C への直接呼出し (親→子) を検証するため、C の作成者を A (ROOT_APP_ID) に設定する。
  access::set_creator(C_ID, ROOT_APP_ID);
  fresh(ROOT_APP_ID, false);
  frame->a0 = (uintptr_t)object_api::CALL_METHOD;
  frame->a1 = C_ID;
  frame->a2 = 0;
  frame->a3 = 0x11;
  kernel_instance.svc_dispatch(ctx);
  assert(kernel_instance.current_depth() == 1);
  assert(ctx->privileged == false);
  assert(!top_return_privileged(thread)); // caller の特権は戻りの記録 (ヘッダ / 台帳の項目) に残る
  ctx->sp->a0 = (uintptr_t)primitive::RETURN;
  ctx->sp->a1 = 1;
  ctx->sp->a2 = 0x12;
  ctx->sp->a3 = 0;
  ctx->sp->a4 = RETURN_STUB_MARKER;
  kernel_instance.svc_dispatch(ctx);
  assert(kernel_instance.current_depth() == 0);
  assert(ctx->privileged == false);
  assert(frame->a1 == 0x12);
  printf("  8g. unprivileged A -> unprivileged C -> return: privilege stays false throughout: PASS\n");

  // 8h. CALL_METHOD_DESCRIPTOR (33)。A (ROOT_APP_ID, 世代 0) が持つ記述子 3 = B、4 = C (非特権、窓あり)。
  //     記述子の検査は表の錠の中 (取り・放しの対が 1 回)。台帳段は LEDGER_DESCRIPTOR_CALL で dispatch 段なし。
  for (uint32_t o = 0; o < KERNEL_OBJECT::OBJECT_COUNT; ++o) {
    for (uint32_t i = 0; i < KERNEL_OBJECT::METHOD_DESCRIPTOR_CAPACITY; ++i) {
      access::invalidate_descriptor(o, i);
    }
  }
  constexpr uintptr_t ENTRY_BD = 0x42040000, ENTRY_CD = 0x42041000;
  access::declare(ROOT_APP_ID, 5, reinterpret_cast<KERNEL_OBJECT::method_t>(0x42001000), 0,
                  ROOT_KOBJ_ID, ROOT_HANDLER_ENTRY); // A を表に作る (記述子の持ち主は created であること)
  access::set_version(B_ID, 7);
  access::set_instance_generation(B_ID, 9);
  access::set_version(C_ID, 4);
  access::set_instance_generation(C_ID, 5);
  access::set_region(C_ID, 0x1000, 0x2000);
  access::declare_descriptor(ROOT_APP_ID, 3, B_ID, ENTRY_BD, 0, 7);
  access::declare_descriptor(ROOT_APP_ID, 4, C_ID, ENTRY_CD, 0, 4);
  {
    fresh(ROOT_APP_ID, true);
    mark_regs(frame, 0x700);
    frame->mepc = A_PC;
    frame->a0 = (uintptr_t)object_api::CALL_METHOD_DESCRIPTOR;
    frame->a1 = 3;
    frame->a2 = 0x11; frame->a3 = 0x12; frame->a4 = 0x13;
    frame->a5 = 0x14; frame->a6 = 0x15; frame->a7 = 0x16;
    kernel_instance.svc_dispatch(ctx);
    assert(access::lock_word() == 0);
    assert(kernel_instance.current_depth() == 1 && count_dispatch_frames() == 0);
    assert(thread.ledger[0].via == KERNEL::LEDGER_DESCRIPTOR_CALL && thread.ledger[0].object == B_ID);
    assert(thread.ledger[0].kind == (uint8_t)object_kind::PLAIN && thread.ledger[0].unwind_floor == 0);
    assert(thread.ledger[0].handler_object == ROOT_KOBJ_ID && thread.ledger[0].handler_entry == ROOT_HANDLER_ENTRY);
    assert(thread.call_stack.call_count == 1 && thread.identity_object() == B_ID);
    assert(thread.identity_generation() == 9 && thread.call_caller_object() == ROOT_APP_ID);
    assert(ctx->sp == frame && frame->mcause == RV32_TAG_ENTRY);
    assert(ARCH::frame_pc(*ctx->sp) == ENTRY_BD);
    // 引数: a2..a7 が呼び先の a0..a5 へ 1 つずつずれて渡る。
    assert(frame->a0 == 0x11 && frame->a1 == 0x12 && frame->a2 == 0x13);
    assert(frame->a3 == 0x14 && frame->a4 == 0x15 && frame->a5 == 0x16);
    // 引数を入口検査のあとでずらしても、caller の s*/gp/tp は入力のまま。
    assert(frame->s0 == (MARK | 0x70D) && frame->s1 == (MARK | 0x70E));
    assert(frame->s2 == (MARK | 0x70F) && frame->s3 == (MARK | 0x710));
    assert(frame->s4 == (MARK | 0x711) && frame->s5 == (MARK | 0x712));
    assert(frame->s6 == (MARK | 0x713) && frame->s7 == (MARK | 0x714));
    assert(frame->s8 == (MARK | 0x715) && frame->s9 == (MARK | 0x716));
    assert(frame->s10 == (MARK | 0x717) && frame->s11 == (MARK | 0x718));
    assert(frame->gp == (MARK | 0x70B) && frame->tp == (MARK | 0x70C));
    assert(frame->ra == (uint32_t)ARCH::return_stub());
    assert(ctx->privileged == true);
    {
      assert(top_return_pc(thread) == A_PC && top_return_privileged(thread)); // via は台帳 (上で検査済み) が持つ
    }
    frame->a0 = (uintptr_t)object_api::GET_CALLER_OBJECT;
    kernel_instance.svc_dispatch(ctx);
    assert(frame->a1 == ROOT_APP_ID); // 本当の呼び出し元 (kobj ではない)
    mark_regs(frame, 0x710);
    frame->a0 = (uintptr_t)primitive::RETURN; // return stub
    frame->a1 = 1;
    frame->a2 = 0x46;
    frame->a3 = 0;
    frame->a4 = RETURN_STUB_MARKER;
    kernel_instance.svc_dispatch(ctx);
    assert(kernel_instance.current_depth() == 0 && thread.call_stack.call_count == 0);
    assert(frame->a0 == (uintptr_t)object_error::OK && frame->a1 == 0x46);
    assert(resumes_passthrough(frame) && frame->mepc == A_PC);
    assert(thread.identity_object() == ROOT_APP_ID);
  }
  printf("  8h. CALL_METHOD_DESCRIPTOR A->B: lock word back to 0, no dispatch frame, ledger via=LEDGER_DESCRIPTOR_CALL, args a2..a7 -> a0..a5, GET_CALLER == A, return stub back to A (value 0x46, pass-through exit): PASS\n");

  // 8i. 記述子のエラーは (error, 0)。段は積まれず、錠は必ず放される。
  access::declare_descriptor(ROOT_APP_ID, 6, B_ID, ENTRY_BD, 1, 7);        // 持ち主の世代が合わない
  access::declare_descriptor(ROOT_APP_ID, 7, B_ID, ENTRY_BD, 0, 8);        // 呼び先の version が合わない
  access::declare_descriptor(ROOT_APP_ID, 8, B_ID, ENTRY_BD, 0, 0);        // 世代 0
  access::declare_descriptor(ROOT_APP_ID, 9, KERNEL_OBJECT::OBJECT_COUNT, ENTRY_BD, 0, 1); // 範囲外の呼び先
  access::set_version(8, 3);
  access::declare_descriptor(ROOT_APP_ID, 10, 8, ENTRY_BD, 0, 3);          // 呼び先が未生成
  access::declare_descriptor(ROOT_APP_ID, 11, B_ID, 0, 0, 7);              // 入口が 0
  access::declare_descriptor(ROOT_APP_ID, 12, B_ID, ENTRY_BD, 0, 7);
  access::invalidate_descriptor(ROOT_APP_ID, 12);                           // 無効化済み
  struct descriptor_case {
    uintptr_t index;
    object_error expect;
  };
  const descriptor_case descriptor_cases[] = {
      {0xFFFF, object_error::BAD_DESCRIPTOR}, {12, object_error::BAD_DESCRIPTOR},
      {6, object_error::STALE_DESCRIPTOR},    {7, object_error::STALE_DESCRIPTOR},
      {8, object_error::STALE_DESCRIPTOR},    {9, object_error::BAD_DESCRIPTOR},
      {10, object_error::BAD_DESCRIPTOR},     {11, object_error::BAD_DESCRIPTOR},
  };
  for (const auto &c : descriptor_cases) {
    fresh(ROOT_APP_ID, true);
    mark_regs(frame, 0x800);
    frame->a2 = 0x81; frame->a3 = 0x82; frame->a4 = 0x83;
    frame->a5 = 0x84;
    frame->a0 = (uintptr_t)object_api::CALL_METHOD_DESCRIPTOR;
    frame->a1 = c.index;
    kernel_instance.svc_dispatch(ctx);
    assert(access::lock_word() == 0);
    assert(kernel_instance.current_depth() == 0 && ctx->sp == frame);
    assert(frame->a0 == (uintptr_t)c.expect && frame->a1 == 0);
    assert(frame->a2 == 0x81 && frame->a3 == 0x82 && frame->a4 == 0x83 && frame->a5 == 0x84);
    assert(regs_pass_through(frame, 0x800)); // 拒否時は入力をずらさない
    assert(ctx->privileged == true);
  }
  // 持ち主が表に居ない (A が created でない別オブジェクトの身) も BAD_DESCRIPTOR。
  fresh(99 % KERNEL_OBJECT::OBJECT_COUNT, true);
  frame->a0 = (uintptr_t)object_api::CALL_METHOD_DESCRIPTOR;
  frame->a1 = 3;
  kernel_instance.svc_dispatch(ctx);
  assert(frame->a0 == (uintptr_t)object_error::BAD_DESCRIPTOR && frame->a1 == 0);
  assert(kernel_instance.current_depth() == 0 && access::lock_word() == 0);
  // NO_STACK: 呼出し段が MAX_DEPTH (呼び出し元の識別は call_count != 0 なので current_call_object から引く)。
  fresh(ROOT_APP_ID, true);
#if defined(SHIZUKU_LEAN_LEDGER)
  // 識別は最内の call 段 (call_top) から引くので、深さ 0 のまま call 段が MAX_DEPTH 個あることにして
  // 先頭の項目を ROOT_APP_ID の段として据える。
  thread.ledger[0].object = ROOT_APP_ID;
  thread.ledger[0].generation = 0;
  thread.ledger[0].prev_call = 0;
  thread.call_stack.call_top = 1;
#else
  thread.call_stack.current_call_object = ROOT_APP_ID;
  thread.call_stack.current_call_generation = 0;
#endif
  thread.call_stack.call_count = KERNEL_OBJECT::MAX_DEPTH;
  frame->a0 = (uintptr_t)object_api::CALL_METHOD_DESCRIPTOR;
  frame->a1 = 3;
  kernel_instance.svc_dispatch(ctx);
  assert(frame->a0 == (uintptr_t)object_error::NO_STACK && frame->a1 == 0);
  assert(kernel_instance.current_depth() == 0 && ctx->privileged == true && access::lock_word() == 0);
  printf("  8i. CALL_METHOD_DESCRIPTOR errors: index range / invalidated / owner gen / target version / gen 0 / target range / not created / entry 0 / owner not created (BAD_DESCRIPTOR or STALE_DESCRIPTOR), NO_STACK: reply (error,0), nothing pushed, lock released: PASS\n");

  // 8j. 非特権の呼び先 (記述子 4 = C): 特権が落ち、呼び先の窓が開く。戻りで caller の特権・窓に戻る。
  fresh(ROOT_APP_ID, true);
  access::set_region(ROOT_APP_ID, 0x5000, 0x6000);
  ctx->region_base = 0x5000;
  ctx->region_limit = 0x6000;
  frame->a0 = (uintptr_t)object_api::CALL_METHOD_DESCRIPTOR;
  frame->a1 = 4;
  frame->a2 = 0x21;
  kernel_instance.svc_dispatch(ctx);
  assert(kernel_instance.current_depth() == 1 && ctx->privileged == false);
  assert(ctx->region_base == 0x1000 && ctx->region_limit == 0x2000);
  assert(frame->a0 == 0x21 && ARCH::frame_pc(*ctx->sp) == ENTRY_CD);
  assert(thread.identity_generation() == 5);
  ctx->sp->a0 = (uintptr_t)primitive::RETURN;
  ctx->sp->a1 = 1;
  ctx->sp->a2 = 0x22;
  ctx->sp->a3 = 0;
  ctx->sp->a4 = RETURN_STUB_MARKER;
  kernel_instance.svc_dispatch(ctx);
  assert(kernel_instance.current_depth() == 0 && ctx->privileged == true);
  assert(ctx->region_base == 0x5000 && ctx->region_limit == 0x6000 && frame->a1 == 0x22);
  assert(ctx->sp == frame && frame->mepc == A_PC);
  access::set_region(C_ID, 0, 0);
  access::set_region(ROOT_APP_ID, 0, 0);
  printf("  8j. CALL_METHOD_DESCRIPTOR to an unprivileged target: target window opens, return resolves caller's current row and preserves PC/SP/privilege/value: PASS\n");

  // 8l. 記述子の項目は呼出しに要る値を作成時に前計算した写しを持つ (入口・親の入口・種別・親・特権・窓・
  //     instance 世代・持ち主と呼び先の世代)。呼出しは「index → 世代照合 → 写しで入る」だけで、行を引かない。
  //     写しの元の値は version を上げる操作 (生成・差し替え・EXPORT_METHOD・GRANT_REGION) でしか変わらないので、
  //     それ以外で写しが古くなる経路は無い。
  {
    auto desc_return = [&](uintptr_t value) {
      ctx->sp->a0 = (uintptr_t)primitive::RETURN;
      ctx->sp->a1 = 1;
      ctx->sp->a2 = value;
      ctx->sp->a3 = 0;
      ctx->sp->a4 = RETURN_STUB_MARKER;
      kernel_instance.svc_dispatch(ctx);
    };
    auto desc_call = [&](uint32_t owner, uintptr_t index, uintptr_t arg) {
      fresh(owner, true);
      frame->a0 = (uintptr_t)object_api::CALL_METHOD_DESCRIPTOR;
      frame->a1 = index;
      frame->a2 = arg;
      kernel_instance.svc_dispatch(ctx);
    };
    // (1) 窓は前計算: 行の窓を version を上げずに変えても、項目の写し (据えた時点の窓 0x1000..0x2000) で入る。
    //     窓を変える正規の経路 (GRANT_REGION) は version を上げるので STALE になり、据え直すと新しい窓で入る。
    access::set_region(C_ID, 0x3000, 0x4000);
    desc_call(ROOT_APP_ID, 4, 0x31);
    assert(kernel_instance.current_depth() == 1 && ctx->privileged == false);
    assert(ctx->region_base == 0x1000 && ctx->region_limit == 0x2000);
    desc_return(0x32);
    assert(kernel_instance.current_depth() == 0 && ctx->privileged == true && frame->a1 == 0x32);
    access::set_version(C_ID, 5); // GRANT_REGION 相当 (版を上げる)
    desc_call(ROOT_APP_ID, 4, 0x38);
    assert(frame->a0 == (uintptr_t)object_error::STALE_DESCRIPTOR && kernel_instance.current_depth() == 0);
    access::declare_descriptor(ROOT_APP_ID, 4, C_ID, ENTRY_CD, 0, 5); // 据え直すと行の新しい窓を写す
    desc_call(ROOT_APP_ID, 4, 0x39);
    assert(kernel_instance.current_depth() == 1 && ctx->region_base == 0x3000 && ctx->region_limit == 0x4000);
    desc_return(0x3a);
    assert(kernel_instance.current_depth() == 0 && frame->a1 == 0x3a);
    access::set_region(C_ID, 0, 0);
    access::set_version(C_ID, 4);
    access::declare_descriptor(ROOT_APP_ID, 4, C_ID, ENTRY_CD, 0, 4);
    // (2) 呼び先の version が上がる (差し替え / EXPORT_METHOD / GRANT_REGION が上げる) と STALE。何も積まない。
    access::set_version(B_ID, 8);
    desc_call(ROOT_APP_ID, 3, 0x33);
    assert(frame->a0 == (uintptr_t)object_error::STALE_DESCRIPTOR && frame->a1 == 0);
    assert(kernel_instance.current_depth() == 0 && ctx->sp == frame && access::lock_word() == 0);
    access::set_version(B_ID, 7); // 戻せば同じ項目がまた通る (項目は書き換えていない)
    desc_call(ROOT_APP_ID, 3, 0x34);
    assert(kernel_instance.current_depth() == 1 && thread.ledger[0].object == B_ID);
    desc_return(0x35);
    assert(kernel_instance.current_depth() == 0 && frame->a1 == 0x35);
    // (3) 表は持ち主ごと: 別の持ち主 (7) の同じ index 13 は、その持ち主の項目に届く。
    access::declare_descriptor(ROOT_APP_ID, 13, B_ID, ENTRY_BD, 0, 7);
    access::declare_descriptor(7, 13, C_ID, ENTRY_CD, 0, 4);
    desc_call(ROOT_APP_ID, 13, 0x36);
    assert(kernel_instance.current_depth() == 1 && thread.ledger[0].object == B_ID);
    assert(ARCH::frame_pc(*ctx->sp) == ENTRY_BD);
    desc_call(7, 13, 0x37);
    assert(kernel_instance.current_depth() == 1 && thread.ledger[0].object == C_ID);
    assert(ARCH::frame_pc(*ctx->sp) == ENTRY_CD && ctx->privileged == false);
    assert(thread.call_caller_object() == 7); // 呼び出し元は持ち主 7
    access::invalidate_descriptor(ROOT_APP_ID, 13);
    access::invalidate_descriptor(7, 13);
    desc_call(ROOT_APP_ID, 13, 0);
    assert(frame->a0 == (uintptr_t)object_error::BAD_DESCRIPTOR && kernel_instance.current_depth() == 0);
    // (4) 呼び先の行が項目に据えられない条件 (未生成・入口 0・範囲外) は項目が無効 = BAD_DESCRIPTOR。
    access::declare_descriptor(ROOT_APP_ID, 14, 8, ENTRY_BD, 0, 3);
    access::declare_descriptor(ROOT_APP_ID, 15, B_ID, 0, 0, 7);
    for (uintptr_t index : {(uintptr_t)14, (uintptr_t)15}) {
      desc_call(ROOT_APP_ID, index, 0);
      assert(frame->a0 == (uintptr_t)object_error::BAD_DESCRIPTOR && kernel_instance.current_depth() == 0);
    }
    access::invalidate_descriptor(ROOT_APP_ID, 14);
    access::invalidate_descriptor(ROOT_APP_ID, 15);

    // (5) seq が奇数の項目を読むと STALE (書込み中の検出)。偶数に戻すと正常に通る。
    access::declare_descriptor(ROOT_APP_ID, 5, B_ID, ENTRY_BD, 0, 7);
    desc_call(ROOT_APP_ID, 5, 0x41);
    assert(kernel_instance.current_depth() == 1 && thread.ledger[0].object == B_ID);
    desc_return(0x42);
    assert(kernel_instance.current_depth() == 0 && frame->a1 == 0x42);
    // seq を奇数に据える -> STALE_DESCRIPTOR (項目は有効な呼び先を持つが、書き込み中と判定)
    access::set_seq(ROOT_APP_ID, 5, 1);
    desc_call(ROOT_APP_ID, 5, 0x43);
    assert(frame->a0 == (uintptr_t)object_error::STALE_DESCRIPTOR && kernel_instance.current_depth() == 0);
    // seq を偶数に戻す -> 通る
    access::set_seq(ROOT_APP_ID, 5, 2);
    desc_call(ROOT_APP_ID, 5, 0x44);
    assert(kernel_instance.current_depth() == 1 && thread.ledger[0].object == B_ID);
    desc_return(0x45);
    assert(kernel_instance.current_depth() == 0 && frame->a1 == 0x45);

    // (6) 削除直後・差し替え直後の desc call
    // 呼び先の差し替え直後: 呼び先オブジェクトが再生成 (REPLACE) されると version が上がり STALE
    access::set_version(B_ID, 9);
    desc_call(ROOT_APP_ID, 5, 0x46);
    assert(frame->a0 == (uintptr_t)object_error::STALE_DESCRIPTOR && kernel_instance.current_depth() == 0);
    access::set_version(B_ID, 7); // 戻す
    // 持ち主の差し替え直後: 持ち主の世代不一致は STALE
    access::declare_descriptor(ROOT_APP_ID, 5, B_ID, ENTRY_BD, 3, 7); // owner_gen = 3 (caller は 0)
    desc_call(ROOT_APP_ID, 5, 0x47);
    assert(frame->a0 == (uintptr_t)object_error::STALE_DESCRIPTOR && kernel_instance.current_depth() == 0);
    // 記述子の無効化 (invalidate_descriptor) 直後: callee=NONE で BAD_DESCRIPTOR
    access::invalidate_descriptor(ROOT_APP_ID, 5);
    desc_call(ROOT_APP_ID, 5, 0x48);
    assert(frame->a0 == (uintptr_t)object_error::BAD_DESCRIPTOR && kernel_instance.current_depth() == 0);
  }
  printf("  8l. descriptor entry = precomputed window (a row window change without a version bump is not seen), version bump -> STALE and nothing pushed, re-declare picks up the new window, restored version passes again, per-owner tables (owner 7 index 13 != root index 13), uncreatable entries stay invalid, seq odd -> STALE, replace/invalidate -> STALE/BAD: PASS\n");

  // 8k. 深さ上限と BAD_REQUEST が同時に成り立つ CALL_METHOD は、従来どおり NO_STACK を先に返す。
  //     (呼び先が root ハンドラの ID を名乗る場合の BAD_REQUEST → BAD_OBJECT より、深さの検査が先)
  access::declare(ROOT_KOBJ_ID, 0, reinterpret_cast<KERNEL_OBJECT::method_t>(0x42070000), 0,
                  ROOT_KOBJ_ID, ROOT_HANDLER_ENTRY);
  access::set_creator(ROOT_KOBJ_ID, ROOT_APP_ID);
  fresh(ROOT_APP_ID, true);
  frame->a0 = (uintptr_t)object_api::CALL_METHOD;
  frame->a1 = ROOT_KOBJ_ID;
  frame->a2 = 0;
  kernel_instance.svc_dispatch(ctx);
  assert(kernel_instance.current_depth() == 0 && frame->a0 == (uintptr_t)object_error::BAD_OBJECT && frame->a1 == 0);
  fresh(ROOT_APP_ID, true);
#if defined(SHIZUKU_LEAN_LEDGER)
  thread.ledger[0].object = ROOT_APP_ID;
  thread.ledger[0].generation = 0;
  thread.ledger[0].prev_call = 0;
  thread.call_stack.call_top = 1;
#else
  thread.call_stack.current_call_object = ROOT_APP_ID;
  thread.call_stack.current_call_generation = 0;
#endif
  thread.call_stack.call_count = KERNEL_OBJECT::MAX_DEPTH;
  frame->a0 = (uintptr_t)object_api::CALL_METHOD;
  frame->a1 = ROOT_KOBJ_ID;
  frame->a2 = 0;
  kernel_instance.svc_dispatch(ctx);
  assert(kernel_instance.current_depth() == 0 && frame->a0 == (uintptr_t)object_error::NO_STACK && frame->a1 == 0);
#if defined(SHIZUKU_LEAN_LEDGER)
  thread.call_stack.call_top = 0;
#else
  thread.call_stack.current_call_object = 0;
#endif
  thread.call_stack.call_count = 0;
  printf("  8k. CALL_METHOD to the root handler's ID: BAD_OBJECT (impersonation refused, nothing pushed); with the depth limit also reached NO_STACK wins, as before: PASS\n");

  // 8m. CALL_METHOD 親子・権限チェックの網羅検証:
  //     - 兄弟・孫・無関係への CALL_METHOD は NOT_PRIVILEGED
  //     - 親・直接の子・自分自身・kernel 役からは OK
  //     - 存在しない相手は BAD_OBJECT のまま
  //     - エラー優先度: BAD_OBJECT → NOT_PRIVILEGED → BAD_METHOD → UNDECLARED_METHOD
  {
    constexpr uint32_t P_ID = 20;  // 親
    constexpr uint32_t X_ID = 21;  // 主体 (caller)
    constexpr uint32_t S_ID = 22;  // 兄弟 (creator = P_ID)
    constexpr uint32_t C_ID = 23;  // 直接の子 (creator = X_ID)
    constexpr uint32_t GC_ID = 24; // 孫 (creator = C_ID)
    constexpr uint32_t U_ID = 25;  // 無関係 (creator = 30)
    constexpr uint32_t K_ID = 26;  // kernel 役 (kernel_role = true)

    constexpr uintptr_t ENTRY_P = 0x42080000;
    constexpr uintptr_t ENTRY_X = 0x42081000;
    constexpr uintptr_t ENTRY_S = 0x42082000;
    constexpr uintptr_t ENTRY_C = 0x42083000;
    constexpr uintptr_t ENTRY_GC = 0x42084000;
    constexpr uintptr_t ENTRY_U = 0x42085000;
    constexpr uintptr_t ENTRY_K = 0x42086000;

    access::declare(P_ID, 0, reinterpret_cast<KERNEL_OBJECT::method_t>(ENTRY_P), 0, ROOT_KOBJ_ID, ROOT_HANDLER_ENTRY);
    access::declare(X_ID, 0, reinterpret_cast<KERNEL_OBJECT::method_t>(ENTRY_X), 0, ROOT_KOBJ_ID, ROOT_HANDLER_ENTRY);
    access::declare(S_ID, 0, reinterpret_cast<KERNEL_OBJECT::method_t>(ENTRY_S), 0, ROOT_KOBJ_ID, ROOT_HANDLER_ENTRY);
    access::declare(C_ID, 0, reinterpret_cast<KERNEL_OBJECT::method_t>(ENTRY_C), 0, ROOT_KOBJ_ID, ROOT_HANDLER_ENTRY);
    access::declare(GC_ID, 0, reinterpret_cast<KERNEL_OBJECT::method_t>(ENTRY_GC), 0, ROOT_KOBJ_ID, ROOT_HANDLER_ENTRY);
    access::declare(U_ID, 0, reinterpret_cast<KERNEL_OBJECT::method_t>(ENTRY_U), 0, ROOT_KOBJ_ID, ROOT_HANDLER_ENTRY);
    access::declare(K_ID, 0, reinterpret_cast<KERNEL_OBJECT::method_t>(ENTRY_K), 0, ROOT_KOBJ_ID, ROOT_HANDLER_ENTRY);

    access::set_creator(P_ID, ROOT_APP_ID);
    access::set_creator(X_ID, P_ID);
    access::set_creator(S_ID, P_ID);
    access::set_creator(C_ID, X_ID);
    access::set_creator(GC_ID, C_ID);
    access::set_creator(U_ID, 30);
    access::set_creator(K_ID, ROOT_APP_ID);
    access::set_kernel_role(K_ID, true);

    auto try_call = [&](uint32_t caller, uint32_t target, uintptr_t method) -> uintptr_t {
      fresh(caller, true);
      frame->a0 = (uintptr_t)object_api::CALL_METHOD;
      frame->a1 = target;
      frame->a2 = method;
      frame->a3 = 0x88;
      kernel_instance.svc_dispatch(ctx);
      return frame->a0;
    };

    // 1. 存在しない相手は BAD_OBJECT のまま (NOT_PRIVILEGED より先)
    assert(try_call(X_ID, KERNEL_OBJECT::OBJECT_COUNT, 0) == (uintptr_t)object_error::BAD_OBJECT);
    assert(try_call(X_ID, 29, 0) == (uintptr_t)object_error::BAD_OBJECT); // 29 は未作成

    // 2. 権限のない相手 (兄弟・孫・無関係) への呼出しは NOT_PRIVILEGED
    assert(try_call(X_ID, S_ID, 0) == (uintptr_t)object_error::NOT_PRIVILEGED);
    assert(try_call(X_ID, GC_ID, 0) == (uintptr_t)object_error::NOT_PRIVILEGED);
    assert(try_call(X_ID, U_ID, 0) == (uintptr_t)object_error::NOT_PRIVILEGED);

    // 3. エラー順序: NOT_PRIVILEGED は BAD_METHOD / UNDECLARED_METHOD より先に返る
    // 兄弟 S_ID に対し、存在しない method 番号 (BAD_METHOD 相当) を呼んでも NOT_PRIVILEGED が返る
    assert(try_call(X_ID, S_ID, KERNEL_OBJECT::METHOD_COUNT) == (uintptr_t)object_error::NOT_PRIVILEGED);
    // 兄弟 S_ID に対し、未 export の method 1 (UNDECLARED_METHOD 相当) を呼んでも NOT_PRIVILEGED が返る
    assert(try_call(X_ID, S_ID, 1) == (uintptr_t)object_error::NOT_PRIVILEGED);
    // 孫 GC_ID に対しても同様
    assert(try_call(X_ID, GC_ID, KERNEL_OBJECT::METHOD_COUNT) == (uintptr_t)object_error::NOT_PRIVILEGED);
    assert(try_call(X_ID, GC_ID, 1) == (uintptr_t)object_error::NOT_PRIVILEGED);
    // 無関係 U_ID に対しても同様
    assert(try_call(X_ID, U_ID, KERNEL_OBJECT::METHOD_COUNT) == (uintptr_t)object_error::NOT_PRIVILEGED);
    assert(try_call(X_ID, U_ID, 1) == (uintptr_t)object_error::NOT_PRIVILEGED);

    // 4. 許可される相手への呼出し (親・直接の子・自分自身・kernel 役)
    // 自分自身:
    fresh(X_ID, true);
    frame->a0 = (uintptr_t)object_api::CALL_METHOD;
    frame->a1 = X_ID;
    frame->a2 = 0;
    kernel_instance.svc_dispatch(ctx);
    assert(kernel_instance.current_depth() == 1 && thread.ledger[0].object == X_ID);
    assert(ARCH::frame_pc(*ctx->sp) == ENTRY_X);

    // 親:
    fresh(X_ID, true);
    frame->a0 = (uintptr_t)object_api::CALL_METHOD;
    frame->a1 = P_ID;
    frame->a2 = 0;
    kernel_instance.svc_dispatch(ctx);
    assert(kernel_instance.current_depth() == 1 && thread.ledger[0].object == P_ID);
    assert(ARCH::frame_pc(*ctx->sp) == ENTRY_P);

    // 直接の子:
    fresh(X_ID, true);
    frame->a0 = (uintptr_t)object_api::CALL_METHOD;
    frame->a1 = C_ID;
    frame->a2 = 0;
    kernel_instance.svc_dispatch(ctx);
    assert(kernel_instance.current_depth() == 1 && thread.ledger[0].object == C_ID);
    assert(ARCH::frame_pc(*ctx->sp) == ENTRY_C);

    // 許可される相手 (子 C_ID) に対する BAD_METHOD と UNDECLARED_METHOD:
    assert(try_call(X_ID, C_ID, KERNEL_OBJECT::METHOD_COUNT) == (uintptr_t)object_error::BAD_METHOD);
    assert(try_call(X_ID, C_ID, 1) == (uintptr_t)object_error::UNDECLARED_METHOD);

    // 5. kernel 役からの呼出しは相手を問わず許可
    // kernel 役 K_ID から兄弟 S_ID へ:
    fresh(K_ID, true);
    frame->a0 = (uintptr_t)object_api::CALL_METHOD;
    frame->a1 = S_ID;
    frame->a2 = 0;
    kernel_instance.svc_dispatch(ctx);
    assert(kernel_instance.current_depth() == 1 && thread.ledger[0].object == S_ID);
    assert(ARCH::frame_pc(*ctx->sp) == ENTRY_S);

    // kernel 役 K_ID から孫 GC_ID へ:
    fresh(K_ID, true);
    frame->a0 = (uintptr_t)object_api::CALL_METHOD;
    frame->a1 = GC_ID;
    frame->a2 = 0;
    kernel_instance.svc_dispatch(ctx);
    assert(kernel_instance.current_depth() == 1 && thread.ledger[0].object == GC_ID);
    assert(ARCH::frame_pc(*ctx->sp) == ENTRY_GC);

    // kernel 役 K_ID から無関係 U_ID へ:
    fresh(K_ID, true);
    frame->a0 = (uintptr_t)object_api::CALL_METHOD;
    frame->a1 = U_ID;
    frame->a2 = 0;
    kernel_instance.svc_dispatch(ctx);
    assert(kernel_instance.current_depth() == 1 && thread.ledger[0].object == U_ID);
    assert(ARCH::frame_pc(*ctx->sp) == ENTRY_U);
  }
  printf("  8m. CALL_METHOD parent/child/kernel_role permissions, NOT_PRIVILEGED for siblings/grandchildren/unrelated, error precedence (BAD_OBJECT -> NOT_PRIVILEGED -> BAD_METHOD -> UNDECLARED): PASS\n");

  // -------------------------------------------------------------------------
  // 9. RUN_FOR (9): GRANT を trap 内で直接撃つ
  // -------------------------------------------------------------------------
  alignas(16) static uint8_t borrower_stack[2048];
  alignas(8) static KERNEL::call_ledger_entry borrower_ledger[32];
  KERNEL::spawn_request borrower_request{};
  borrower_request.entry_pc = 0x42050000;
  borrower_request.protection = 0;
  borrower_request.stack_base = (uintptr_t)borrower_stack;
  borrower_request.stack_bytes = sizeof(borrower_stack);
  borrower_request.object_id = B_ID;
  borrower_request.parent_handler_object = ROOT_KOBJ_ID;
  borrower_request.parent_handler_entry = ROOT_HANDLER_ENTRY;
  borrower_request.ledger = borrower_ledger;
  borrower_request.ledger_capacity = 32;
  const auto spawned = kernel_instance.spawn(borrower_request);
  assert(spawned.error == kernel_error::OK && spawned.thread == 1);
  using thread_state = KERNEL::THREAD::state_t;

  // 9a. 貸せない (持ち分が最小未満): NO_TIME。相手は claim されず READY のまま。
  for (bool privileged : {true, false}) {
    fresh(ROOT_APP_ID, privileged);
    frame->a0 = (uintptr_t)object_api::RUN_FOR;
    frame->a1 = 1;
    frame->a2 = ARCH::TIMER_MIN_CYCLES - 1;
    kernel_instance.svc_dispatch(ctx);
    assert(kernel_instance.current_depth() == 0);
    assert(frame->a0 == (uintptr_t)object_error::NO_TIME);
    assert(frame->a1 == 0);
    assert(kernel_instance.current_thread_id() == 0);
    assert(kernel_instance.thread_state(1) == thread_state::READY);
    assert(ctx->privileged == privileged);
  }
  // 9b. 相手が走れない (未生成の枠): NOT_RUNNABLE。
  fresh(ROOT_APP_ID, true);
  frame->a0 = (uintptr_t)object_api::RUN_FOR;
  frame->a1 = 3;
  frame->a2 = 5000;
  kernel_instance.svc_dispatch(ctx);
  assert(kernel_instance.current_depth() == 0);
  assert(frame->a0 == (uintptr_t)object_error::NOT_RUNNABLE);
  assert(frame->a1 == 0);
  assert(kernel_instance.current_thread_id() == 0);
  // 9c. 貸せた: 貸し手の応答には既定値 (OK, YIELDED) が置かれ、実行権は相手へ移り、
  //     貸し手は WAIT_GRANT。dispatch 段は積まれない。
  fresh(ROOT_APP_ID, false);
  frame->a0 = (uintptr_t)object_api::RUN_FOR;
  frame->a1 = 1;
  frame->a2 = 5000;
  kernel_instance.svc_dispatch(ctx);
  assert(kernel_instance.current_depth() == 0);
  assert(kernel_instance.current_thread_id() == 1);
  assert(kernel_instance.grant_active());
  assert(kernel_instance.thread_state(0) == thread_state::WAIT_GRANT);
  assert(kernel_instance.thread_state(1) == thread_state::RUNNING);
  assert(frame->a0 == (uintptr_t)object_error::OK);
  assert(frame->a1 == (uintptr_t)grant_end::YIELDED);
  assert(ctx->privileged == false); // 貸し手の文脈は非特権のまま
  // 期限切れで巻き取る: 貸し手の応答が (OK, EXPIRED) に書き換わって実行権が戻る。
  kernel_instance.pendsv_dispatch(ctx);
  assert(!kernel_instance.grant_active());
  assert(kernel_instance.current_thread_id() == 0);
  assert(kernel_instance.thread_state(0) == thread_state::RUNNING);
  assert(kernel_instance.thread_state(1) == thread_state::READY);
  assert(frame->a0 == (uintptr_t)object_error::OK);
  assert(frame->a1 == (uintptr_t)grant_end::EXPIRED);
  printf("  9. RUN_FOR in trap: NO_TIME / NOT_RUNNABLE / grant (lender WAIT_GRANT, reply (OK,YIELDED)) and expiry reply (OK,EXPIRED); no dispatch frame, privilege unchanged: PASS\n");

  // -------------------------------------------------------------------------
  // 10. SET_BUDGET (12): m_budget[a1] = a2 を trap 内で。範囲外は何もしない
  // -------------------------------------------------------------------------
  for (bool privileged : {true, false}) {
    access::set_raw_budget(2, 111);
    access::set_raw_budget(KERNEL_OBJECT::THREAD_COUNT - 1, 222);
    fresh(ROOT_APP_ID, privileged);
    frame->a0 = (uintptr_t)object_api::SET_BUDGET;
    frame->a1 = 2;
    frame->a2 = 12345;
    kernel_instance.svc_dispatch(ctx);
    assert(kernel_instance.current_depth() == 0);
    assert(access::budget(2) == 12345);
    assert(frame->a0 == (uintptr_t)object_error::OK && frame->a1 == 0);
    assert(ctx->privileged == privileged);
    frame->a0 = (uintptr_t)object_api::SET_BUDGET;
    frame->a1 = KERNEL_OBJECT::THREAD_COUNT; // 範囲外
    frame->a2 = 99;
    kernel_instance.svc_dispatch(ctx);
    assert(kernel_instance.current_depth() == 0);
    assert(access::budget(2) == 12345);
    assert(access::budget(KERNEL_OBJECT::THREAD_COUNT - 1) == 222);
    assert(frame->a0 == (uintptr_t)object_error::OK && frame->a1 == 0);
    assert(ctx->privileged == privileged);
  }
  printf("  10. SET_BUDGET in trap: budget written, out-of-range ignored, reply (OK,0), no dispatch frame: PASS\n");

  // -------------------------------------------------------------------------
  // 11. 専用親ハンドラ宛ての 2/4/9/12 は trap で奪わず従来どおり無加工で配送する
  // -------------------------------------------------------------------------
  for (object_api raw : {object_api::EXIT_METHOD, object_api::CALL_METHOD,
                         object_api::YIELD, object_api::RUN_FOR,
                         object_api::SET_BUDGET, object_api::FORWARD_CHILD_EXIT}) {
    kernel_instance.set_thread_ledger(0, ledger, 32, CHILD_ID,
                                      (uint32_t)object_kind::PLAIN,
                                      CUSTOM_HANDLER_ID, CUSTOM_HANDLER_ENTRY);
    ctx->sp = frame;
    ctx->privileged = true;
    frame->a0 = (uintptr_t)raw;
    frame->a1 = B_ID;
    frame->a2 = 0;
    frame->a3 = 0x5A;
    frame->a4 = 0;
    kernel_instance.svc_dispatch(ctx);
    assert(kernel_instance.current_depth() == 1);
    assert(ARCH::frame_pc(*ctx->sp) == CUSTOM_HANDLER_ENTRY);
    assert(ARCH::arg(*ctx->sp, 0) == (uintptr_t)raw);
    assert(ARCH::arg(*ctx->sp, 1) == B_ID);
    assert(ARCH::arg(*ctx->sp, 3) == 0x5A);
    printf("  11. raw SVC %u to dedicated parent handler delivered unchanged: PASS\n",
           (unsigned)raw);
  }

  // -------------------------------------------------------------------------
  // 12. YIELD (8): 借り手なら trap 内で貸し手へ返す (dispatch 段なし)
  // -------------------------------------------------------------------------
  // 貸し手 A = thread 0 (PLAIN, 特権)。借り手 = thread 1。
  fresh(ROOT_APP_ID, true);
  frame->a0 = (uintptr_t)object_api::RUN_FOR;
  frame->a1 = 1;
  frame->a2 = 5000;
  kernel_instance.svc_dispatch(ctx);
  assert(kernel_instance.current_thread_id() == 1 && kernel_instance.grant_active());
  auto *ctx1 = kernel_instance.thread_context(1);
  KERNEL::FRAME *frame1 = ctx1->sp;
  assert(kernel_instance.current_depth() == 0); // 借り手 (thread 1) の台帳は空
  frame1->a0 = (uintptr_t)object_api::YIELD;
  kernel_instance.svc_dispatch(ctx1);
  assert(kernel_instance.current_thread_id() == 0); // 貸し手へ戻った
  assert(!kernel_instance.grant_active());
  assert(kernel_instance.thread_state(0) == thread_state::RUNNING);
  assert(kernel_instance.thread_state(1) == thread_state::READY);
  assert(frame->a0 == (uintptr_t)object_error::OK);
  assert(frame->a1 == (uintptr_t)grant_end::YIELDED); // 貸し手の RUN_FOR の戻り値
  assert(frame1->a0 == (uintptr_t)object_error::OK && frame1->a1 == 0); // 借り手の YIELD の応答
  assert(kernel_instance.thread_context(1)->sp == frame1);
  assert(kernel_instance.ledger_size(1) == 0); // 借り手の台帳に dispatch 段は積まれていない
  printf("  12. borrower YIELD returns to lender in trap (lender reply (OK,YIELDED), borrower reply (OK,0), lender RUNNING, borrower READY, no dispatch frame): PASS\n");

  // -------------------------------------------------------------------------
  // 13. SWITCH = GRANT(無制限) (primitive。kobj から撃つ)  A(kobj) → B(kobj) → C(plain)
  // -------------------------------------------------------------------------
  alignas(16) static uint8_t b_stack[2048];
  alignas(8) static KERNEL::call_ledger_entry b_ledger[32];
  KERNEL::spawn_request b_request{};
  b_request.entry_pc = 0x42060000;
  b_request.stack_base = (uintptr_t)b_stack;
  b_request.stack_bytes = sizeof(b_stack);
  b_request.object_id = ROOT_KOBJ_ID;
  b_request.kind = (uint32_t)object_kind::KERNEL_OBJECT;
  b_request.parent_handler_object = ROOT_KOBJ_ID;
  b_request.parent_handler_entry = ROOT_HANDLER_ENTRY;
  b_request.ledger = b_ledger;
  b_request.ledger_capacity = 32;
  const auto spawned_b = kernel_instance.spawn(b_request);
  assert(spawned_b.error == kernel_error::OK && spawned_b.thread == 2);
  auto *ctx2 = kernel_instance.thread_context(2);
  KERNEL::FRAME *frame2 = ctx2->sp;
  auto as_kobj_a = [&]() {
    kernel_instance.set_thread_ledger(0, ledger, 32, ROOT_KOBJ_ID,
                                      (uint32_t)object_kind::KERNEL_OBJECT,
                                      ROOT_KOBJ_ID, ROOT_HANDLER_ENTRY);
    ctx->sp = frame;
    ctx->privileged = true;
  };

  // 13a. A の SWITCH(B): grant 段が積まれる。期限が無いので pendsv でも取り上げられない。
  as_kobj_a();
  frame->a0 = (uintptr_t)primitive::SWITCH;
  frame->a1 = 2;
  chips::esp32_c6::g_timer_left = 0;
  kernel_instance.svc_dispatch(ctx);
  assert(frame->a0 == (uintptr_t)kernel_error::OK);
  assert(kernel_instance.current_thread_id() == 2 && kernel_instance.grant_active());
  assert(kernel_instance.grant_lender() == 0);
  assert(kernel_instance.thread_state(0) == thread_state::WAIT_GRANT); // READY ではなく貸し手として待つ
  assert(kernel_instance.thread_state(2) == thread_state::RUNNING);
  assert(thread.call_stack.depth == 0); // kobj の primitive は dispatch 段を積まない
  kernel_instance.pendsv_dispatch(ctx);
  assert(kernel_instance.current_thread_id() == 2 && kernel_instance.grant_active());
  printf("  13a. SWITCH(B) from origin A: grant frame pushed (lender A WAIT_GRANT), unlimited (pendsv does not revoke): PASS\n");

  // 13b. 借り手 B の SWITCH(C) は C への又貸し (A→B→C)。
  frame2->a0 = (uintptr_t)primitive::SWITCH;
  frame2->a1 = 1;
  kernel_instance.svc_dispatch(ctx2);
  assert(frame2->a0 == (uintptr_t)kernel_error::OK);
  assert(kernel_instance.current_thread_id() == 1);
  assert(kernel_instance.grant_lender() == 2);
  assert(kernel_instance.thread_state(2) == thread_state::WAIT_GRANT);
  assert(kernel_instance.thread_state(0) == thread_state::WAIT_GRANT);
  printf("  13b. SWITCH(C) from borrower B: re-lend A->B->C (B WAIT_GRANT, lender of C == B): PASS\n");

  // 13c. C の YIELD は B へ戻る (A へは戻らない)。B の SWITCH の戻りは (OK, YIELDED)。
  frame1->a0 = (uintptr_t)object_api::YIELD;
  kernel_instance.svc_dispatch(ctx1);
  assert(kernel_instance.current_thread_id() == 2 && kernel_instance.grant_active());
  assert(kernel_instance.grant_lender() == 0);
  assert(frame2->a0 == (uintptr_t)kernel_error::OK && frame2->a1 == (uintptr_t)grant_end::YIELDED);
  assert(kernel_instance.thread_state(2) == thread_state::RUNNING);
  assert(kernel_instance.thread_state(1) == thread_state::READY);
  assert(kernel_instance.thread_state(0) == thread_state::WAIT_GRANT);
  printf("  13c. C YIELD returns to B (not A); B resumes with (OK,YIELDED), A still waiting: PASS\n");

  // 13d. B (kobj) が貸し手 A を指して SWITCH すると 1 段巻き取られて A へ戻る (grant_lender())。
  frame2->a0 = (uintptr_t)primitive::SWITCH;
  frame2->a1 = kernel_instance.grant_lender();
  kernel_instance.svc_dispatch(ctx2);
  assert(kernel_instance.current_thread_id() == 0 && !kernel_instance.grant_active());
  assert(kernel_instance.thread_state(0) == thread_state::RUNNING);
  assert(kernel_instance.thread_state(2) == thread_state::READY);
  assert(frame->a0 == (uintptr_t)kernel_error::OK && frame->a1 == (uintptr_t)grant_end::YIELDED);
  printf("  13d. SWITCH(lender) unwinds one grant frame back to A (A reply (OK,YIELDED)): PASS\n");

  // 13e. 有限の貸し出し (primitive GRANT 5000) の内側の SWITCH は外側の残りでクランプされる:
  //      タイマが撃ち切ると C の段も B の段も 0 で、A まで一気に巻き取られる (EXPIRED)。
  as_kobj_a();
  frame->a0 = (uintptr_t)primitive::GRANT;
  frame->a1 = 2;
  frame->a2 = 5000;
  chips::esp32_c6::g_timer_left = 5000; // 刻みはまだ減っていない
  kernel_instance.svc_dispatch(ctx);
  assert(kernel_instance.current_thread_id() == 2);
  frame2->a0 = (uintptr_t)primitive::SWITCH;
  frame2->a1 = 1;
  kernel_instance.svc_dispatch(ctx2);
  assert(frame2->a0 == (uintptr_t)kernel_error::OK); // B の残りは有限でも TIMER_MIN 以上
  assert(kernel_instance.current_thread_id() == 1 && kernel_instance.grant_lender() == 2);
  chips::esp32_c6::g_timer_left = 0; // 撃ち切った
  kernel_instance.pendsv_dispatch(ctx);
  assert(kernel_instance.current_thread_id() == 0 && !kernel_instance.grant_active());
  assert(frame->a0 == (uintptr_t)kernel_error::OK && frame->a1 == (uintptr_t)grant_end::EXPIRED);
  assert(frame2->a1 == (uintptr_t)grant_end::EXPIRED);
  assert(kernel_instance.thread_state(1) == thread_state::READY);
  assert(kernel_instance.thread_state(2) == thread_state::READY);
  printf("  13e. SWITCH inside a finite GRANT is clamped to the outer remaining (expiry unwinds C and B to A, EXPIRED): PASS\n");

  // 13f. 持ち分が尽きていれば SWITCH も断られる (GRANT_TOO_SMALL)。相手は claim されない。
  as_kobj_a();
  frame->a0 = (uintptr_t)primitive::GRANT;
  frame->a1 = 2;
  frame->a2 = 5000;
  chips::esp32_c6::g_timer_left = 5000;
  kernel_instance.svc_dispatch(ctx);
  assert(kernel_instance.current_thread_id() == 2);
  chips::esp32_c6::g_timer_left = 0; // B の持ち分を使い切った状態で SWITCH
  frame2->a0 = (uintptr_t)primitive::SWITCH;
  frame2->a1 = 1;
  kernel_instance.svc_dispatch(ctx2);
  assert(frame2->a0 == (uintptr_t)kernel_error::GRANT_TOO_SMALL);
  assert(kernel_instance.current_thread_id() == 2);
  assert(kernel_instance.thread_state(1) == thread_state::READY);
  // 後始末: B (kobj) が貸し手 A へ SWITCH して戻る。
  frame2->a0 = (uintptr_t)primitive::SWITCH;
  frame2->a1 = kernel_instance.grant_lender();
  kernel_instance.svc_dispatch(ctx2);
  assert(kernel_instance.current_thread_id() == 0 && !kernel_instance.grant_active());
  printf("  13f. SWITCH with no budget left is refused (GRANT_TOO_SMALL), target stays READY: PASS\n");

  // 13g. EXIT_THREAD (kobj の exit_thread) の grant_active 側: terminate(self) のあと
  //      SWITCH(grant_lender()) で貸し手へ返す。終わったスレッドは TERMINATED のまま、
  //      貸し手の応答は (OK, YIELDED)。
  as_kobj_a();
  frame->a0 = (uintptr_t)primitive::SWITCH;
  frame->a1 = 2;
  kernel_instance.svc_dispatch(ctx);
  assert(kernel_instance.current_thread_id() == 2 && kernel_instance.grant_active());
  kernel_instance.terminate(2);
  frame2->a0 = (uintptr_t)primitive::SWITCH;
  frame2->a1 = kernel_instance.grant_lender();
  kernel_instance.svc_dispatch(ctx2);
  assert(kernel_instance.current_thread_id() == 0 && !kernel_instance.grant_active());
  assert(kernel_instance.thread_state(2) == thread_state::TERMINATED);
  assert(kernel_instance.thread_state(0) == thread_state::RUNNING);
  assert(frame->a0 == (uintptr_t)kernel_error::OK && frame->a1 == (uintptr_t)grant_end::YIELDED);
  printf("  13g. exit_thread-style return (terminate + SWITCH(lender)): borrower stays TERMINATED, lender resumes with (OK,YIELDED): PASS\n");

  // -------------------------------------------------------------------------
  // 14. FORWARD_CHILD_EXIT (27): 出どころが専用 HANDLER のときだけ、2 と同じ pop
  // -------------------------------------------------------------------------
  constexpr uint32_t D_ID = 44;
  access::declare(D_ID, 0, reinterpret_cast<KERNEL_OBJECT::method_t>(0x42070000), 0,
                  CUSTOM_HANDLER_ID, CUSTOM_HANDLER_ENTRY);
  access::set_creator(D_ID, ROOT_APP_ID);
  auto into_handler = [&]() {
    // A → D (親ハンドラは専用 HANDLER 42)。D の raw SVC が専用 HANDLER へ配送され、
    // HANDLER 42 の段 (kind HANDLER, 親 = root) が積まれる。ctx->sp は HANDLER のフレーム。
    fresh(ROOT_APP_ID, true);
    frame->a0 = (uintptr_t)object_api::CALL_METHOD;
    frame->a1 = D_ID;
    frame->a2 = 0;
    frame->a3 = 0x21;
    kernel_instance.svc_dispatch(ctx);
    assert(kernel_instance.current_depth() == 1 && thread.ledger[0].handler_object == CUSTOM_HANDLER_ID);
    ctx->sp->a0 = 77; // 専用 HANDLER への raw SVC
    kernel_instance.svc_dispatch(ctx);
    assert(kernel_instance.current_depth() == 2);
    assert(thread.ledger[1].via == KERNEL::LEDGER_DISPATCH);
    assert(thread.ledger[1].kind == (uint8_t)object_kind::HANDLER);
    assert(thread.ledger[1].object == CUSTOM_HANDLER_ID);
    assert(thread.ledger[1].handler_object == ROOT_KOBJ_ID);
  };
  // 14a. 正しい出どころ: HANDLER の段 + D の段の 2 枚を落として A へ (値 0x55, error 0)。
  into_handler();
  KERNEL::FRAME *frame_h = ctx->sp;
  frame_h->a0 = (uintptr_t)object_api::FORWARD_CHILD_EXIT;
  frame_h->a1 = 0;
  frame_h->a2 = 0x55;
  frame_h->a3 = 0;
  kernel_instance.svc_dispatch(ctx);
  assert(kernel_instance.current_depth() == 0);
  assert(thread.call_stack.call_count == 0);
  assert(ctx->sp == frame);
  assert(frame->a0 == (uintptr_t)object_error::OK && frame->a1 == 0x55);
  printf("  14a. FORWARD_CHILD_EXIT from dedicated HANDLER pops HANDLER+child frames in trap (depth 2->0, value 0x55): PASS\n");

  // 14b. HANDLER でも root でもない出どころは NOT_PRIVILEGED。1 枚も落とさない。
  fresh(ROOT_APP_ID, true);
  frame->a0 = (uintptr_t)object_api::CALL_METHOD;
  frame->a1 = B_ID;
  frame->a2 = 0;
  kernel_instance.svc_dispatch(ctx); // 呼び先 B (PLAIN, 親 = root)
  assert(kernel_instance.current_depth() == 1);
  ctx->sp->a0 = (uintptr_t)object_api::FORWARD_CHILD_EXIT;
  ctx->sp->a1 = 0;
  ctx->sp->a2 = 0x56;
  kernel_instance.svc_dispatch(ctx);
  assert(kernel_instance.current_depth() == 1);
  assert(ctx->sp->a0 == (uintptr_t)object_error::NOT_PRIVILEGED && ctx->sp->a1 == 0);
  printf("  14b. FORWARD_CHILD_EXIT from a PLAIN object: NOT_PRIVILEGED, nothing popped: PASS\n");

  // 14c. 戻り先が floor 未満なら HANDLER からでも UNWIND_REJECTED/DEPTH_MISMATCH。
  into_handler();
  frame_h = ctx->sp;
  frame_h->a0 = (uintptr_t)object_api::FORWARD_CHILD_EXIT;
  frame_h->a1 = 2; // >= depth
  frame_h->a2 = 0x57;
  frame_h->a3 = 0;
  kernel_instance.svc_dispatch(ctx);
  assert(kernel_instance.current_depth() == 2);
  assert(frame_h->a0 == (uintptr_t)object_error::UNWIND_REJECTED);
  assert(frame_h->a1 == (uintptr_t)kernel_error::DEPTH_MISMATCH);
  printf("  14c. FORWARD_CHILD_EXIT with bad d: UNWIND_REJECTED/DEPTH_MISMATCH, nothing popped: PASS\n");

  // -------------------------------------------------------------------------
  // 15. in-place call の境界
  // -------------------------------------------------------------------------
  // 15a. 呼び先の入口に要る余白が無ければ NO_STACK。台帳・identity・文脈は 1 つも動かない。
  {
    fresh(ROOT_APP_ID, true);
    mark_regs(frame, 0x400);
    const uintptr_t saved_limit = ARCH::stack_limit(*ctx);
    // frame のすぐ下まで下限を上げる (残り 100 バイト)。
    ARCH::stack_limit_set(*ctx, reinterpret_cast<uintptr_t>(frame) + sizeof(KERNEL::FRAME) - 100);
    frame->a0 = (uintptr_t)object_api::CALL_METHOD;
    frame->a1 = B_ID;
    frame->a2 = 0;
    frame->a3 = 0x5A;
    kernel_instance.svc_dispatch(ctx);
    assert(kernel_instance.current_depth() == 0);
    assert(thread.call_stack.call_count == 0 && thread.call_stack.top == 0);
    assert(ctx->sp == frame && ctx->privileged == true);
    assert(frame->a0 == (uintptr_t)object_error::NO_STACK && frame->a1 == 0);
    assert(regs_pass_through(frame, 0x400)); // 入れなかった caller のレジスタは消えない
    ARCH::stack_limit_set(*ctx, saved_limit);
    printf("  15a. in-place CALL_METHOD without return room: NO_STACK, nothing pushed, caller registers untouched: PASS\n");
  }
  // 15b. 呼び先の trap フレームが caller の再開フレームに重なる位置 (呼び先が SP を少し下げて
  //      から戻る) でも、戻り値は重なる前に読まれ、再開フレームは caller の SP の直下に作られる。
  {
    fresh(ROOT_APP_ID, true);
    frame->mepc = A_PC;
    frame->a0 = (uintptr_t)object_api::CALL_METHOD;
    frame->a1 = B_ID;
    frame->a2 = 0;
    frame->a3 = 0x5B;
    kernel_instance.svc_dispatch(ctx);
    assert(kernel_instance.current_depth() == 1);
    auto *lower = reinterpret_cast<KERNEL::FRAME *>(reinterpret_cast<uint8_t *>(frame) - 48);
    ctx->sp = lower; // 呼び先が 48 バイト使ってから ecall した (trap フレームが再開フレームと重なる)
    lower->a0 = (uintptr_t)primitive::RETURN;
    lower->a1 = 1;
    lower->a2 = 0x4B;
    lower->a3 = 0;
    lower->a4 = RETURN_STUB_MARKER;
    kernel_instance.svc_dispatch(ctx);
    assert(kernel_instance.current_depth() == 0);
    assert(ctx->sp == frame); // 再開は caller の SP の直下 (呼び先の trap フレームの位置ではない)
    assert(frame->a0 == (uintptr_t)object_error::OK && frame->a1 == 0x4B);
    assert(frame->mepc == A_PC && resumes_passthrough(frame));
    printf("  15b. in-place return from a lower SP (frames overlap): resumes at the caller's SP with value 0x4B, pass-through exit: PASS\n");
  }

  // 15c. 全段 in-place (段数の上限なし): 8 段 (旧: 浅い 4 段まで) の CALL_METHOD。各段は前の段の
  //      callee が自分のスタックを 400 バイト使ってから ecall した、という形 (枠は段ごとに別の
  //      アドレス)。ヘッダは prev でつながり、戻りは 8 段すべてで素通しの出口 (1 段ずつの普通の戻り)。
  {
    constexpr uint32_t LEVELS = 8;
    fresh(ROOT_APP_ID, true);
    frame->mepc = A_PC;
    KERNEL::FRAME *cur = frame;
    KERNEL::FRAME *frames[LEVELS];
    uintptr_t headers[LEVELS];
    for (uint32_t i = 0; i < LEVELS; ++i) {
      mark_regs(cur, 0x500 + 0x40 * i);
      cur->mepc = A_PC + 0x10 * i;
      cur->a0 = (uintptr_t)object_api::CALL_METHOD;
      cur->a1 = B_ID;
      cur->a2 = 0;
      cur->a3 = i;
      frames[i] = cur;
      kernel_instance.svc_dispatch(ctx);
      assert(kernel_instance.current_depth() == i + 1);
      assert(ARCH::arg(*ctx->sp, 0) == i);
      assert(ctx->sp == cur); // どの段も frame をコピーしない
      assert(regs_pass_through(cur, 0x500 + 0x40 * i));
#if defined(SHIZUKU_LEAN_LEDGER)
      // 戻りの権威は台帳の項目: 段 i の caller の再開点 (pc・sp) と、ひとつ外側の call 段への鎖 (prev_call)。
      const auto &rec = thread.ledger[i];
      headers[i] = rec.ret_sp;
      assert(rec.ret_sp == reinterpret_cast<uintptr_t>(cur) + sizeof(KERNEL::FRAME));
      assert(rec.prev_call == i && rec.ret_pc == A_PC + 0x10 * i);
      assert(thread.call_stack.call_top == i + 1);
      // 各段の caller の再開点は前の段の再開点より下 (スタックは下へ伸びる)。
      assert(i == 0 || rec.ret_sp < thread.ledger[i - 1].ret_sp);
      (void)headers;
#else
      headers[i] = thread.call_stack.top;
      assert(headers[i] == reinterpret_cast<uintptr_t>(cur) + sizeof(KERNEL::FRAME));
      const auto *h = (const ARCH::call_header_t *)headers[i];
      assert(h->prev == (i == 0 ? 0 : headers[i - 1]) && h->pc == A_PC + 0x10 * i);
      // callee の窓の上端は前の段のヘッダの下端: 呼び先 (と次の枠) はそれより下にあり、前の段の
      // ヘッダ (戻り pc・caller の識別) の領域へは届かない。
      assert(i == 0 || (headers[i] + sizeof(ARCH::call_header_t) <=
                        ARCH::call_window_top((const ARCH::call_header_t *)headers[i - 1])));
#endif
      // 呼び先は自分のスタックを 400 バイト使ってから次の ecall をする (窓は枠の下端まで)。
      cur = reinterpret_cast<KERNEL::FRAME *>(reinterpret_cast<uint8_t *>(cur) - 400);
      ctx->sp = cur;
    }
    assert(thread.call_stack.call_count == LEVELS);
    for (uint32_t i = LEVELS; i-- > 0;) {
      cur->a0 = (uintptr_t)primitive::RETURN;
      cur->a1 = 1;
      cur->a2 = 0x70 + i;
      cur->a3 = 0;
      cur->a4 = RETURN_STUB_MARKER;
      kernel_instance.svc_dispatch(ctx);
      assert(kernel_instance.current_depth() == i);
#if defined(SHIZUKU_LEAN_LEDGER)
      assert(thread.call_stack.call_top == i); // 鎖が一つ外側の call 段へ戻る
#else
      assert(thread.call_stack.top == (i == 0 ? 0 : headers[i - 1]));
#endif
      cur = ctx->sp;
      assert(cur == frames[i]); // 段 i を呼んだ caller の枠の直下で再開する
      assert(cur->a0 == (uintptr_t)object_error::OK && cur->a1 == 0x70 + i);
      assert(resumes_passthrough(cur)); // 全段が 1 段の普通の戻り = 素通しの出口
      assert(cur->mepc == A_PC + 0x10 * i);
    }
    assert(thread.call_stack.call_count == 0 && cur == frame);
    printf("  15c. %u nested CALL_METHOD, all in-place (headers chained by prev, window top = header): every 1-level return resumes the caller on the pass-through exit: PASS\n",
           (unsigned)LEVELS);
  }

  // -------------------------------------------------------------------------
  // 16. ecall 入口が caller のレジスタを保存しない (ECALL のフレームは a0-a7/mepc/mstatus/目印だけ)
  //     経路でも、スレッド切替えをまたいで文脈が壊れないこと。出口はフレームの mcause (目印) で選ぶ。
  // -------------------------------------------------------------------------
  {
    // 16a. 目印: 呼び先の入口は RV32_TAG_ENTRY (s*/gp/tp を通す出口)、戻った caller の再開フレームは
    //      RV32_TAG_ECALL (素通しの出口。2 段以上の巻き戻しだけ RV32_TAG_ECALL_SCRUB、18)、入口を作らない
    //      trap 内 API の応答は目印を変えない。
    fresh(ROOT_APP_ID, true);
    frame->mcause = RV32_TAG_ECALL; // 入口 asm が書く
    frame->mepc = A_PC;
    frame->a0 = (uintptr_t)object_api::GET_CURRENT_OBJECT;
    kernel_instance.svc_dispatch(ctx);
    assert(frame->mcause == RV32_TAG_ECALL && frame->a1 == ROOT_APP_ID);
    frame->a0 = (uintptr_t)object_api::CALL_METHOD;
    frame->a1 = B_ID;
    frame->a2 = 0;
    frame->a3 = 1;
    kernel_instance.svc_dispatch(ctx);
    assert(ctx->sp == frame && frame->mcause == RV32_TAG_ENTRY);
    frame->mcause = RV32_TAG_ECALL; // 呼び先の return stub の ecall (入口 asm が書く)
    frame->a0 = (uintptr_t)primitive::RETURN;
    frame->a1 = 1;
    frame->a2 = 0x66;
    frame->a3 = 0;
    frame->a4 = RETURN_STUB_MARKER;
    kernel_instance.svc_dispatch(ctx);
    assert(ctx->sp == frame && frame->mcause == RV32_TAG_ECALL && frame->a1 == 0x66);
    assert(frame->mepc == A_PC);
    // エラー応答 (入れなかった) も入口の目印にならない。
    frame->a0 = (uintptr_t)object_api::CALL_METHOD;
    frame->a1 = KERNEL_OBJECT::OBJECT_COUNT;
    frame->a2 = 0;
    kernel_instance.svc_dispatch(ctx);
    assert(frame->mcause == RV32_TAG_ECALL && frame->a0 == (uintptr_t)object_error::BAD_OBJECT);

    // 16b. 貸し手 A (thread 0) が ecall で RUN_FOR → 借り手 B (thread 1) へ切替え → B が ecall で
    //      YIELD → A へ戻る。A のフレームは切替えの間まったく変わらず (mepc・目印・文脈の sp)、
    //      B の初期フレーム (全レジスタ復元で戻る目印 0) は A の RUN_FOR で触られない。
    fresh(ROOT_APP_ID, true);
    assert(kernel_instance.thread_state(1) == thread_state::READY);
    auto *ctx_b = kernel_instance.thread_context(1);
    KERNEL::FRAME *frame_b_init = ctx_b->sp;
    KERNEL::FRAME b_before = *frame_b_init;
    frame->mcause = RV32_TAG_ECALL;
    frame->mepc = A_PC + 4;
    frame->a0 = (uintptr_t)object_api::RUN_FOR;
    frame->a1 = 1;
    frame->a2 = 5000;
    kernel_instance.svc_dispatch(ctx);
    assert(kernel_instance.current_thread_id() == 1);
    // 現在のスレッドの 1 語キャッシュ (m_current_thread) が m_current と一緒に動く: 借り手へ切り替わった。
    assert(kernel_instance.current_context() == ctx_b && kernel_instance.current_thread().context == ctx_b);
    assert(kernel_instance.thread_context(0)->sp == frame); // A の文脈は ecall フレームを指したまま
    assert(frame->mcause == RV32_TAG_ECALL && frame->mepc == A_PC + 4); // A の再開点は動かない
    assert(ctx_b->sp == frame_b_init && __builtin_memcmp(&b_before, frame_b_init, sizeof(b_before)) == 0);
    assert(frame_b_init->mcause == 0); // B の初めての実行は全レジスタ復元の出口
    // B が ecall で YIELD する (入口 asm が目印・mepc・mstatus を書く)。
    frame_b_init->mcause = RV32_TAG_ECALL;
    frame_b_init->mepc = 0x42060040;
    frame_b_init->a0 = (uintptr_t)object_api::YIELD;
    kernel_instance.svc_dispatch(ctx_b);
    assert(kernel_instance.current_thread_id() == 0);
    assert(kernel_instance.current_context() == ctx && kernel_instance.current_thread().context == ctx); // 貸し手へ戻った
    assert(kernel_instance.thread_context(0)->sp == frame);
    assert(frame->mcause == RV32_TAG_ECALL && frame->mepc == A_PC + 4 && ctx->privileged);
    assert(frame->a0 == (uintptr_t)object_error::OK && frame->a1 == (uintptr_t)grant_end::YIELDED);
    // B は次に走るとき ECALL の出口 (a0/a1 = (OK, 0)) で再開し、再開点も動かない。
    assert(kernel_instance.thread_context(1)->sp == frame_b_init);
    assert(frame_b_init->mcause == RV32_TAG_ECALL && frame_b_init->mepc == 0x42060040);
    assert(frame_b_init->a0 == (uintptr_t)object_error::OK && frame_b_init->a1 == 0);
    printf("  16. ecall frames across thread switches: entry/return tags (ENTRY after call, ECALL after return/native API), lender and borrower resume points and tags unchanged across RUN_FOR/YIELD, borrower's initial full frame untouched: PASS\n");
  }

  // -------------------------------------------------------------------------
  // 17. ecall の直接入口 (svc_entry): RV32 の例外入口は a0 の番号の表からこれを呼ぶ。
  //     native の経路に立てていれば svc_dispatch と同じ結果、立てていなければ svc_dispatch へ回る。
  // -------------------------------------------------------------------------
  {
    assert(!kernel_instance.grant_active() && kernel_instance.current_thread_id() == 0);
    // 17a. 経路の判定: 一般オブジェクト + root 宛て = native、dedicated 親ハンドラ配下 / kobj 本人 = native でない。
    fresh(ROOT_APP_ID, true);
    assert(kernel_instance.svc_native_route());
    kernel_instance.set_thread_ledger(0, ledger, 32, CHILD_ID, (uint32_t)object_kind::PLAIN,
                                      CUSTOM_HANDLER_ID, CUSTOM_HANDLER_ENTRY);
    assert(!kernel_instance.svc_native_route());
    as_kobj_a();
    assert(!kernel_instance.svc_native_route());
    printf("  17a. svc_native_route: plain object under root == true, under a dedicated handler == false, kobj itself == false: PASS\n");

    // 17b. GET_CURRENT / GET_CALLER / CALL_METHOD / EXIT_METHOD を entry から (dispatch 段なし)。
    fresh(ROOT_APP_ID, true);
    frame->mcause = RV32_TAG_ECALL;
    frame->mepc = A_PC;
    frame->a0 = (uintptr_t)object_api::GET_CURRENT_OBJECT;
    kernel_instance.svc_entry<&KERNEL::svc_get_current>(ctx);
    assert(frame->a0 == (uintptr_t)object_error::OK && frame->a1 == ROOT_APP_ID);
    frame->a0 = (uintptr_t)object_api::GET_CALLER_OBJECT;
    kernel_instance.svc_entry<&KERNEL::svc_get_caller>(ctx);
    assert(frame->a0 == (uintptr_t)object_error::OK && frame->a1 == (uintptr_t)KERNEL_OBJECT::NO_OBJECT);
    assert(kernel_instance.current_depth() == 0);
    mark_regs(frame, 0x600);
    frame->a0 = (uintptr_t)object_api::CALL_METHOD;
    frame->a1 = B_ID;
    frame->a2 = 0;
    frame->a3 = 0x21;
    kernel_instance.svc_entry<&KERNEL::svc_call_method>(ctx);
    assert(kernel_instance.current_depth() == 1 && count_dispatch_frames() == 0);
    assert(thread.ledger[0].via == KERNEL::LEDGER_CALL && thread.ledger[0].object == B_ID);
    assert(ARCH::frame_pc(*ctx->sp) == ENTRY_B && ARCH::arg(*ctx->sp, 0) == 0x21);
    assert(ctx->sp == frame && frame->mcause == RV32_TAG_ENTRY); // 呼び先の入口の目印
    frame->mcause = RV32_TAG_ECALL;
    frame->a0 = (uintptr_t)object_api::GET_CALLER_OBJECT;
    kernel_instance.svc_entry<&KERNEL::svc_get_caller>(ctx);
    assert(frame->a1 == ROOT_APP_ID); // 本当の呼び出し元 (kobj ではない)
    frame->a0 = (uintptr_t)object_api::EXIT_METHOD;
    frame->a1 = 0;
    frame->a2 = 0x22;
    frame->a3 = 0;
    frame->a4 = 0;
    kernel_instance.svc_entry<&KERNEL::svc_exit_method>(ctx);
    assert(kernel_instance.current_depth() == 0 && count_dispatch_frames() == 0);
    assert(ctx->sp == frame && frame->a0 == (uintptr_t)object_error::OK && frame->a1 == 0x22);
    assert(frame->mepc == A_PC && resumes_passthrough(frame));
    printf("  17b. svc_entry GET_CURRENT/GET_CALLER/CALL_METHOD/EXIT_METHOD: no dispatch frame, caller seen by callee == A, return value 0x22 on the pass-through exit: PASS\n");

    // 17c. 本体が false を返す場合 (スレッドの最外のメソッドの EXIT_METHOD / 起点スレッドの YIELD) は
    //      kobj へ配送 (dispatch 段 1 枚、宛先は root handler)。
    fresh(ROOT_APP_ID, true);
    frame->a0 = (uintptr_t)object_api::EXIT_METHOD;
    frame->a1 = 0;
    frame->a2 = 0x23;
    frame->a3 = 0;
    kernel_instance.svc_entry<&KERNEL::svc_exit_method>(ctx);
    assert(kernel_instance.current_depth() == 1 && count_dispatch_frames() == 1);
    assert(thread.ledger[0].via == KERNEL::LEDGER_DISPATCH);
    assert(ARCH::frame_pc(*ctx->sp) == ROOT_HANDLER_ENTRY);
    assert(ARCH::arg(*ctx->sp, 0) == (uintptr_t)object_api::EXIT_METHOD && ARCH::arg(*ctx->sp, 2) == 0x23);
    fresh(ROOT_APP_ID, true);
    frame->a0 = (uintptr_t)object_api::YIELD;
    kernel_instance.svc_entry<&KERNEL::svc_yield>(ctx);
    assert(kernel_instance.current_depth() == 1 && count_dispatch_frames() == 1);
    assert(ARCH::frame_pc(*ctx->sp) == ROOT_HANDLER_ENTRY);
    assert(ARCH::arg(*ctx->sp, 0) == (uintptr_t)object_api::YIELD);
    printf("  17c. svc_entry outermost EXIT_METHOD and originating-thread YIELD (no lender) fall through to the kobj (one dispatch frame, pc = root handler): PASS\n");

    // 17d. RUN_FOR / SET_BUDGET / YIELD (借り手) を entry から。
    fresh(ROOT_APP_ID, true);
    frame->a0 = (uintptr_t)object_api::RUN_FOR;
    frame->a1 = 1;
    frame->a2 = ARCH::TIMER_MIN_CYCLES - 1;
    kernel_instance.svc_entry<&KERNEL::svc_run_for>(ctx);
    assert(frame->a0 == (uintptr_t)object_error::NO_TIME && frame->a1 == 0);
    frame->a0 = (uintptr_t)object_api::RUN_FOR;
    frame->a1 = 3;
    frame->a2 = 5000;
    kernel_instance.svc_entry<&KERNEL::svc_run_for>(ctx);
    assert(frame->a0 == (uintptr_t)object_error::NOT_RUNNABLE && frame->a1 == 0);
    access::set_raw_budget(2, 111);
    frame->a0 = (uintptr_t)object_api::SET_BUDGET;
    frame->a1 = 2;
    frame->a2 = 4321;
    kernel_instance.svc_entry<&KERNEL::svc_set_budget>(ctx);
    assert(access::budget(2) == 4321 && frame->a0 == (uintptr_t)object_error::OK);
    assert(kernel_instance.current_depth() == 0);
    frame->a0 = (uintptr_t)object_api::RUN_FOR;
    frame->a1 = 1;
    frame->a2 = 5000;
    kernel_instance.svc_entry<&KERNEL::svc_run_for>(ctx);
    assert(kernel_instance.current_thread_id() == 1 && kernel_instance.grant_active());
    assert(frame->a0 == (uintptr_t)object_error::OK && frame->a1 == (uintptr_t)grant_end::YIELDED);
    KERNEL::FRAME *borrower = ctx1->sp;
    borrower->a0 = (uintptr_t)object_api::YIELD;
    kernel_instance.svc_entry<&KERNEL::svc_yield>(ctx1);
    assert(kernel_instance.current_thread_id() == 0 && !kernel_instance.grant_active());
    assert(kernel_instance.ledger_size(1) == 0); // 借り手の台帳に dispatch 段なし
    assert(borrower->a0 == (uintptr_t)object_error::OK && borrower->a1 == 0);
    assert(frame->a0 == (uintptr_t)object_error::OK && frame->a1 == (uintptr_t)grant_end::YIELDED);
    printf("  17d. svc_entry RUN_FOR (NO_TIME / NOT_RUNNABLE / grant), SET_BUDGET, borrower YIELD back to lender: no dispatch frame: PASS\n");

    // 17e. FORWARD_CHILD_EXIT: PLAIN の出どころは NOT_PRIVILEGED、dedicated HANDLER からは 2 段 pop。
    fresh(ROOT_APP_ID, true);
    frame->a0 = (uintptr_t)object_api::CALL_METHOD;
    frame->a1 = B_ID;
    frame->a2 = 0;
    kernel_instance.svc_entry<&KERNEL::svc_call_method>(ctx);
    assert(kernel_instance.current_depth() == 1);
    ctx->sp->a0 = (uintptr_t)object_api::FORWARD_CHILD_EXIT;
    ctx->sp->a1 = 0;
    ctx->sp->a2 = 0x24;
    kernel_instance.svc_entry<&KERNEL::svc_forward_child_exit>(ctx);
    assert(kernel_instance.current_depth() == 1);
    assert(ctx->sp->a0 == (uintptr_t)object_error::NOT_PRIVILEGED && ctx->sp->a1 == 0);
    into_handler();
    frame_h = ctx->sp;
    frame_h->a0 = (uintptr_t)object_api::FORWARD_CHILD_EXIT;
    frame_h->a1 = 0;
    frame_h->a2 = 0x25;
    frame_h->a3 = 0;
    kernel_instance.svc_entry<&KERNEL::svc_forward_child_exit>(ctx);
    assert(kernel_instance.current_depth() == 0 && ctx->sp == frame);
    assert(frame->a0 == (uintptr_t)object_error::OK && frame->a1 == 0x25);
    printf("  17e. svc_entry FORWARD_CHILD_EXIT: PLAIN origin NOT_PRIVILEGED (nothing popped), dedicated HANDLER origin pops 2 frames (value 0x25): PASS\n");

    // 17f. native でない立場は番号に関係なく svc_dispatch へ回る:
    //      dedicated 親ハンドラ配下の raw svc は無加工で配送、kobj 本人の SWITCH は primitive として処理。
    kernel_instance.set_thread_ledger(0, ledger, 32, CHILD_ID, (uint32_t)object_kind::PLAIN,
                                      CUSTOM_HANDLER_ID, CUSTOM_HANDLER_ENTRY);
    ctx->sp = frame;
    ctx->privileged = true;
    frame->a0 = (uintptr_t)object_api::GET_CURRENT_OBJECT;
    frame->a1 = B_ID;
    frame->a3 = 0x5B;
    kernel_instance.svc_entry<&KERNEL::svc_get_current>(ctx);
    assert(kernel_instance.current_depth() == 1);
    assert(thread.ledger[0].kind == (uint8_t)object_kind::HANDLER);
    assert(ARCH::frame_pc(*ctx->sp) == CUSTOM_HANDLER_ENTRY);
    assert(ARCH::arg(*ctx->sp, 0) == (uintptr_t)object_api::GET_CURRENT_OBJECT);
    assert(ARCH::arg(*ctx->sp, 1) == B_ID && ARCH::arg(*ctx->sp, 3) == 0x5B);
    as_kobj_a();
    frame->a0 = (uintptr_t)primitive::SWITCH;
    frame->a1 = 1;
    kernel_instance.svc_entry<&KERNEL::svc_get_current>(ctx); // 番号は SWITCH。kobj 本人なので primitive
    assert(frame->a0 == (uintptr_t)kernel_error::OK);
    assert(kernel_instance.current_thread_id() == 1 && kernel_instance.grant_active());
    assert(kernel_instance.thread_state(0) == thread_state::WAIT_GRANT);
    assert(thread.call_stack.depth == 0); // primitive は dispatch 段を積まない
    ctx1->sp->a0 = (uintptr_t)object_api::YIELD;
    kernel_instance.svc_entry<&KERNEL::svc_yield>(ctx1);
    assert(kernel_instance.current_thread_id() == 0 && !kernel_instance.grant_active());
    printf("  17f. svc_entry from a dedicated-handler child delivers the raw svc unchanged; from the kobj itself runs the primitive (SWITCH grants, borrower YIELD returns): PASS\n");
  }

  // -------------------------------------------------------------------------
  // 18. 戻りの出口の選び方: 素通し (RV32_TAG_ECALL) と消去 (RV32_TAG_ECALL_SCRUB)
  //     消去は「経路」ではなく「巻き戻しの意味」で決まる: callee が自分の意思で戻らない
  //     (2 段以上を一度に落とす EXIT_METHOD / RETURN、FORWARD_CHILD_EXIT) ときだけ。
  //     1 段の普通の戻りは、どの経路 (return stub の RETURN、明示 d の EXIT_METHOD) でも素通し。
  //     別スレッドへ切り替わった再開の目印替え (scrub_on_switch) は chips/*/trap.cpp が呼ぶので、
  //     ここでは関数そのものの規則を見る (呼ぶ場所は test_trap_static.py が縛る)。
  // -------------------------------------------------------------------------
  {
    // 18a. dedicated HANDLER の段 (深さ 2、floor 0) から d = 0 の EXIT_METHOD: 2 段を一度に落とす
    //      巻き戻し → 消去の目印。呼出し元 A の再開は dispatch 段なし、値 0x71。
    into_handler();
    KERNEL::FRAME *frame_h2 = ctx->sp;
    frame_h2->mcause = RV32_TAG_ECALL;
    frame_h2->a0 = (uintptr_t)object_api::EXIT_METHOD;
    frame_h2->a1 = 0;
    frame_h2->a2 = 0x71;
    frame_h2->a3 = 0;
    frame_h2->a4 = 0;
    kernel_instance.svc_dispatch(ctx);
    assert(kernel_instance.current_depth() == 0 && thread.call_stack.call_count == 0);
    assert(ctx->sp == frame);
    assert(frame->a0 == (uintptr_t)object_error::OK && frame->a1 == 0x71);
    assert(resumes_scrubbed(frame) && !resumes_passthrough(frame));
    printf("  18a. EXIT_METHOD d=0 from depth 2 (2 frames at once): resume frame tagged RV32_TAG_ECALL_SCRUB (value 0x71): PASS\n");

    // 18b. FORWARD_CHILD_EXIT は 1 段だけ落とす場合も消去 (親ハンドラが子を取り上げて戻す巻き戻し)。
    into_handler();
    frame_h2 = ctx->sp;
    frame_h2->mcause = RV32_TAG_ECALL;
    frame_h2->a0 = (uintptr_t)object_api::FORWARD_CHILD_EXIT;
    frame_h2->a1 = 1; // HANDLER の段 1 枚だけ落として D へ戻る
    frame_h2->a2 = 0x73;
    frame_h2->a3 = 0;
    kernel_instance.svc_dispatch(ctx);
    assert(kernel_instance.current_depth() == 1 && thread.call_stack.call_count == 1);
    assert(ctx->sp->a0 == (uintptr_t)object_error::OK && ctx->sp->a1 == 0x73);
    assert(resumes_scrubbed(ctx->sp) && !resumes_passthrough(ctx->sp));
    printf("  18b. FORWARD_CHILD_EXIT popping a single frame: resume frame still RV32_TAG_ECALL_SCRUB: PASS\n");

    // 18c/18d. kobj の段 (thread mode の API を撃った dispatch 段) からの primitive RETURN。
    //     B (A から CALL_METHOD、深さ 1) が自分のスタックを 400 バイト使ってから OBJECT_NAME を撃つ。
    auto into_kobj_stage = [&]() {
      fresh(ROOT_APP_ID, true);
      frame->mepc = A_PC;
      frame->a0 = (uintptr_t)object_api::CALL_METHOD;
      frame->a1 = B_ID;
      frame->a2 = 0;
      frame->a3 = 0x75;
      kernel_instance.svc_dispatch(ctx);
      assert(kernel_instance.current_depth() == 1 && ctx->sp == frame);
      auto *frame_b2 = reinterpret_cast<KERNEL::FRAME *>(reinterpret_cast<uint8_t *>(frame) - 400);
      ctx->sp = frame_b2;
      frame_b2->mcause = RV32_TAG_ECALL;
      frame_b2->mepc = ENTRY_B;
      frame_b2->a0 = (uintptr_t)object_api::OBJECT_NAME;
      frame_b2->a1 = ROOT_APP_ID;
      kernel_instance.svc_dispatch(ctx);
      assert(kernel_instance.current_depth() == 2);
      assert(thread.ledger[1].via == KERNEL::LEDGER_DISPATCH);
      assert(thread.ledger[1].kind == (uint8_t)object_kind::KERNEL_OBJECT);
      assert(ARCH::frame_pc(*ctx->sp) == ROOT_HANDLER_ENTRY);
      return frame_b2;
    };
    // 18c. kobj の段が目印付き (return stub) の RETURN で 1 段だけ戻る → B が素通しの出口で再開。
    KERNEL::FRAME *frame_b2 = into_kobj_stage();
    ctx->sp->mcause = RV32_TAG_ECALL;
    ctx->sp->a0 = (uintptr_t)primitive::RETURN;
    ctx->sp->a1 = 1;
    ctx->sp->a2 = 0x74;
    ctx->sp->a3 = 0;
    ctx->sp->a4 = RETURN_STUB_MARKER;
    kernel_instance.svc_dispatch(ctx);
    assert(kernel_instance.current_depth() == 1 && ctx->sp == frame_b2);
    assert(frame_b2->a0 == (uintptr_t)object_error::OK && frame_b2->a1 == 0x74);
    assert(resumes_passthrough(frame_b2) && !resumes_scrubbed(frame_b2));
    printf("  18c. kobj stage RETURN (return-stub marker, 1 frame): caller resumes on RV32_TAG_ECALL (pass-through), not scrubbed: PASS\n");

    // 18d. 同じ段が明示の目標深さ 0 (count = 2) で戻る → A まで 2 段を一度に落とす巻き戻し = 消去。
    into_kobj_stage();
    ctx->sp->mcause = RV32_TAG_ECALL;
    ctx->sp->a0 = (uintptr_t)primitive::RETURN;
    ctx->sp->a1 = 2;
    ctx->sp->a2 = 0x76;
    ctx->sp->a3 = 0;
    ctx->sp->a4 = 0; // 目標の深さ (marker ではない)
    kernel_instance.svc_dispatch(ctx);
    assert(kernel_instance.current_depth() == 0 && thread.call_stack.call_count == 0);
    assert(ctx->sp == frame && frame->mepc == A_PC);
    assert(frame->a0 == (uintptr_t)object_error::OK && frame->a1 == 0x76);
    assert(resumes_scrubbed(frame) && !resumes_passthrough(frame));
    printf("  18d. primitive RETURN to depth 0 from depth 2 (2 frames at once): resume frame tagged RV32_TAG_ECALL_SCRUB: PASS\n");

    // 18e. 別スレッドへ切り替わったあとの再開: ECALL で止まっていたフレームだけ消去の目印に替える。
    //      全レジスタを復元して戻るフレーム (0)、呼び先の入口 (ENTRY)、すでに消去のものは変えない。
    KERNEL::FRAME parked = {};
    ARCH::context_t resumed;
    resumed.sp = &parked;
    parked.mcause = RV32_TAG_ECALL;
    ARCH::scrub_on_switch(resumed);
    assert(parked.mcause == RV32_TAG_ECALL_SCRUB);
    ARCH::scrub_on_switch(resumed); // 冪等
    assert(parked.mcause == RV32_TAG_ECALL_SCRUB);
    parked.mcause = 0;
    ARCH::scrub_on_switch(resumed);
    assert(parked.mcause == 0);
    parked.mcause = RV32_TAG_ENTRY;
    ARCH::scrub_on_switch(resumed);
    assert(parked.mcause == RV32_TAG_ENTRY);
    parked.mcause = 0x80000007u; // 割込みで保存された全レジスタのフレーム
    ARCH::scrub_on_switch(resumed);
    assert(parked.mcause == 0x80000007u);
    printf("  18e. scrub_on_switch: only a frame parked at an ECALL (RV32_TAG_ECALL) becomes RV32_TAG_ECALL_SCRUB; full-restore frames / ENTRY / already-scrubbed untouched: PASS\n");
  }

  // -------------------------------------------------------------------------
  // 19. 4A Boundary: Alternating dedicated handlers and calls reaching depth=33,
  //     call_count < 32. Verify that call entrance reserves the final ledger slot
  //     for return dispatch.
  // -------------------------------------------------------------------------
  {
    printf("[19] 4A boundary: alternating dedicated handlers and calls reaching depth=33...\n");
    thread.call_stack.depth = 0;
    thread.call_stack.call_count = 0;
    thread.call_stack.top = 0;
    thread.call_stack.call_top = 0;
    thread.call_stack.caller_object = 0;
    thread.call_stack.current_call_object = 0;

    alignas(8) static KERNEL::call_ledger_entry ledger_34[34];
    for (size_t i = 0; i < 34; ++i) ledger_34[i] = {};

    constexpr uint32_t APP0_ID = 2;
    constexpr uint32_t H_BASE_ID = 10; // H1..H16: 11..26
    constexpr uint32_t C_BASE_ID = 30; // C1..C18: 31..48

    access::declare(APP0_ID, 0, reinterpret_cast<KERNEL_OBJECT::method_t>(0x50000000), 0,
                    ROOT_KOBJ_ID, ROOT_HANDLER_ENTRY);
    access::set_creator(APP0_ID, ROOT_APP_ID);
    access::set_kernel_role(APP0_ID, false);

    kernel_instance.set_thread_ledger(
        0, ledger_34, 34, APP0_ID, (uint32_t)object_kind::PLAIN,
        ROOT_KOBJ_ID, ROOT_HANDLER_ENTRY);

    for (uint32_t k = 1; k <= 16; ++k) {
      uint32_t hk_id = H_BASE_ID + k;
      uintptr_t hk_entry = 0x51000000 + k * 0x10000;
      access::declare(hk_id, 0, reinterpret_cast<KERNEL_OBJECT::method_t>(hk_entry), 0,
                      ROOT_KOBJ_ID, ROOT_HANDLER_ENTRY);
      access::set_kind(hk_id, object_kind::HANDLER);
      access::set_creator(hk_id, ROOT_APP_ID);
      access::set_kernel_role(hk_id, false);

      uint32_t ck_id = C_BASE_ID + k;
      uintptr_t ck_entry = 0x61000000 + k * 0x10000;
      access::declare(ck_id, 0, reinterpret_cast<KERNEL_OBJECT::method_t>(ck_entry), 0,
                      hk_id, hk_entry);
      access::set_creator(ck_id, (k == 1 ? APP0_ID : (C_BASE_ID + (k - 1))));
      access::set_kernel_role(ck_id, false);
    }

    // 19a. Reach depth=32 via alternating calls and dispatches:
    //      H_{k-1} calls C_k (call, depth 2k - 1), C_k raw SVC -> H_k (dispatch, depth 2k)
    for (uint32_t k = 1; k <= 16; ++k) {
      uint32_t ck_id = C_BASE_ID + k;
      uint32_t hk_id = H_BASE_ID + k;

      ctx->sp = frame;
      frame->a0 = (uintptr_t)object_api::CALL_METHOD;
      frame->a1 = ck_id;
      frame->a2 = 0;
      frame->a3 = k * 10;
      kernel_instance.svc_dispatch(ctx);
      assert(thread.call_stack.depth == 2 * k - 1);
      assert(thread.call_stack.call_count == k);
      assert(thread.ledger[2 * k - 2].via == KERNEL::LEDGER_CALL);
      assert(thread.ledger[2 * k - 2].object == ck_id);

      ctx->sp = frame;
      frame->a0 = 0x100 + k;
      kernel_instance.svc_dispatch(ctx);
      assert(thread.call_stack.depth == 2 * k);
      assert(thread.call_stack.call_count == k);
      assert(thread.ledger[2 * k - 1].via == KERNEL::LEDGER_DISPATCH);
      assert(thread.ledger[2 * k - 1].object == hk_id);
    }

    // At this point (k=16 completed):
    // depth == 32, call_count == 16, H16 is running at depth 32.
    assert(thread.call_stack.depth == 32);
    assert(thread.call_stack.call_count == 16);

    // H16 calls C17 (parent handler ROOT_KOBJ_ID so C17 can issue CALL_METHOD directly to trap)
    uint32_t c17_id = C_BASE_ID + 17;
    uintptr_t c17_entry = 0x61000000 + 17 * 0x10000;
    access::declare(c17_id, 0, reinterpret_cast<KERNEL_OBJECT::method_t>(c17_entry), 0,
                    ROOT_KOBJ_ID, ROOT_HANDLER_ENTRY);
    access::set_creator(c17_id, C_BASE_ID + 16);
    access::set_kernel_role(c17_id, false);

    ctx->sp = frame;
    frame->a0 = (uintptr_t)object_api::CALL_METHOD;
    frame->a1 = c17_id;
    frame->a2 = 0;
    frame->a3 = 170;
    kernel_instance.svc_dispatch(ctx);
    assert(thread.call_stack.depth == 33);
    assert(thread.call_stack.call_count == 17);
    assert(thread.ledger[32].via == KERNEL::LEDGER_CALL);
    assert(thread.ledger[32].object == c17_id);

    // Now we are at:
    // depth == 33 (entries 0..32 filled, exactly 1 slot left: slot 33)
    // call_count == 17 (< 32)
    // ledger_capacity == 34
    assert(thread.call_stack.depth == 33);
    assert(thread.call_stack.call_count == 17);
    assert(thread.ledger_capacity == 34);

    // Declare C18 (child of C17)
    uint32_t c18_id = C_BASE_ID + 18;
    uintptr_t c18_entry = 0x61000000 + 18 * 0x10000;
    access::declare(c18_id, 0, reinterpret_cast<KERNEL_OBJECT::method_t>(c18_entry), 0,
                    ROOT_KOBJ_ID, ROOT_HANDLER_ENTRY);
    access::set_creator(c18_id, c17_id);
    access::set_kernel_role(c18_id, false);

    // Snapshot entire state before attempting CALL_METHOD at depth=33
    const uint32_t depth_before = thread.call_stack.depth;
    const uint32_t call_count_before = thread.call_stack.call_count;
    const uint32_t call_top_before = thread.call_stack.call_top;
    const uintptr_t reg_base_before = ctx->region_base;
    const uintptr_t reg_limit_before = ctx->region_limit;
    const bool priv_before = ctx->privileged;
    const void *sp_before = ctx->sp;

    KERNEL::call_ledger_entry ledger_saved[34];
    for (size_t i = 0; i < 34; ++i) ledger_saved[i] = thread.ledger[i];

    // C17 attempts to call C18 via CALL_METHOD.
    // 4A: this MUST be rejected with NO_STACK because slot 33 is reserved for return dispatch!
    ctx->sp = frame;
    frame->a0 = (uintptr_t)object_api::CALL_METHOD;
    frame->a1 = c18_id;
    frame->a2 = 0;
    frame->a3 = 999;
    frame->a4 = 0xAA55;
    frame->a5 = 0x55AA;
    kernel_instance.svc_dispatch(ctx);

    // 4A assertions:
    assert(frame->a0 == (uintptr_t)object_error::NO_STACK);
    assert(frame->a1 == 0); // response value register
    assert(frame->a2 == 0);
    assert(frame->a3 == 999);
    assert(frame->a4 == 0xAA55);
    assert(frame->a5 == 0x55AA);
    assert(thread.call_stack.depth == depth_before);
    assert(thread.call_stack.call_count == call_count_before);
    assert(thread.call_stack.call_top == call_top_before);
    assert(ctx->region_base == reg_base_before);
    assert(ctx->region_limit == reg_limit_before);
    assert(ctx->privileged == priv_before);
    assert(ctx->sp == sp_before);
    // Entire ledger must be completely untouched!
    for (size_t i = 0; i < 34; ++i) {
      assert(thread.ledger[i].object == ledger_saved[i].object);
      assert(thread.ledger[i].via == ledger_saved[i].via);
      assert(thread.ledger[i].kind == ledger_saved[i].kind);
      assert(thread.ledger[i].handler_object == ledger_saved[i].handler_object);
      assert(thread.ledger[i].unwind_floor == ledger_saved[i].unwind_floor);
    }
    printf("  19a-1. 4A boundary at depth=33, call_count=17: CALL_METHOD rejected with NO_STACK, full invariants (all 34 ledger entries, context, frame inputs) preserved: PASS\n");

    // 19a-2. Return dispatch (svc_forward) CAN use the reserved slot 33 -> reaching depth=34!
    // Set C17's parent handler to H16 (both in object table and in active call ledger frame)
    uint32_t h16_id = H_BASE_ID + 16;
    uintptr_t h16_entry = 0x51000000 + 16 * 0x10000;
    access::set_parent_handler(c17_id, h16_id, h16_entry);
    thread.ledger[32].handler_object = h16_id;
    thread.ledger[32].handler_entry = h16_entry;

    // C17 returns to dedicated parent handler H16 (to_root == false) via raw SVC
    ctx->sp = frame;
    frame->a0 = 0x77; // raw SVC notification to parent handler H16
    kernel_instance.svc_dispatch(ctx);
    assert(thread.call_stack.depth == 34); // The 34th slot (index 33) is used by return dispatch!
    assert(thread.call_stack.call_count == 17);
    assert(thread.ledger[33].via == KERNEL::LEDGER_DISPATCH);
    assert(thread.ledger[33].object == h16_id);
    assert(thread.ledger[33].kind == (uint8_t)object_kind::HANDLER);
    printf("  19a-2. return dispatch (svc_forward) successfully uses the reserved 34th slot (depth=34, object=H16): PASS\n");

    // 19a-3. FORWARD_CHILD_EXIT from depth 34 unwinds back to depth 32 (H16)
    ctx->sp->a0 = (uintptr_t)object_api::FORWARD_CHILD_EXIT;
    ctx->sp->a1 = 32; // target depth: unwinds H16 dispatch (slot 33) and C17 call (slot 32)
    ctx->sp->a2 = 0x88; // return value
    ctx->sp->a3 = 0; // return error
    kernel_instance.svc_dispatch(ctx);
    assert(thread.call_stack.depth == 32);
    assert(thread.call_stack.call_count == 16);
    assert(ctx->sp->a0 == (uintptr_t)object_error::OK);
    assert(ctx->sp->a1 == 0x88);
    printf("  19a-3. FORWARD_CHILD_EXIT from depth=34 pops both frames cleanly back to depth=32 (value=0x88): PASS\n");

    // 19b. Verify normal 32-level call nesting is preserved
    // Built purely with creator relationships (no kernel_role bypass)
    // and distinct stack frames per depth (safe for non-LEAN user-stack headers)
    thread.call_stack.depth = 0;
    thread.call_stack.call_count = 0;
    thread.call_stack.top = 0;
    thread.call_stack.call_top = 0;
    kernel_instance.set_thread_ledger(
        0, ledger_34, 34, ROOT_APP_ID, (uint32_t)object_kind::PLAIN,
        ROOT_KOBJ_ID, ROOT_HANDLER_ENTRY);

    access::set_kernel_role(ROOT_APP_ID, false);
    for (uint32_t i = 1; i <= 32; ++i) {
      uint32_t nid = C_BASE_ID + i;
      access::declare(nid, 0, reinterpret_cast<KERNEL_OBJECT::method_t>(0x70000000 + i * 0x1000), 0,
                      ROOT_KOBJ_ID, ROOT_HANDLER_ENTRY);
      access::set_kernel_role(nid, false);
      access::set_creator(nid, (i == 1) ? ROOT_APP_ID : (C_BASE_ID + (i - 1)));
    }

    // Allocate stack frames per depth in simulated_stack
    KERNEL::FRAME *cur = frame;
    KERNEL::FRAME *frames[32];
    for (uint32_t i = 0; i < 32; ++i) {
      cur->mepc = A_PC + 0x10 * i;
      cur->a0 = (uintptr_t)object_api::CALL_METHOD;
      cur->a1 = C_BASE_ID + 1 + i;
      cur->a2 = 0;
      cur->a3 = 0x5500 + i;
      frames[i] = cur;
      ctx->sp = cur;
      kernel_instance.svc_dispatch(ctx);
      assert(thread.call_stack.depth == i + 1);
      assert(thread.call_stack.call_count == i + 1);
      assert(ARCH::arg(*ctx->sp, 0) == 0x5500 + i);
      // Move down stack for next depth
      cur = reinterpret_cast<KERNEL::FRAME *>(reinterpret_cast<uint8_t *>(cur) - 400);
    }
    assert(thread.call_stack.depth == 32);
    assert(thread.call_stack.call_count == 32);

    // Call 33 rejected with NO_STACK because call_count == MAX_DEPTH
    cur->a0 = (uintptr_t)object_api::CALL_METHOD;
    cur->a1 = C_BASE_ID + 32;
    cur->a2 = 0;
    cur->a3 = 999;
    ctx->sp = cur;
    kernel_instance.svc_dispatch(ctx);
    assert(cur->a0 == (uintptr_t)object_error::NO_STACK);
    assert(thread.call_stack.depth == 32);
    assert(thread.call_stack.call_count == 32);

    // Unwind all 32 levels back to depth 0
    for (uint32_t i = 32; i-- > 0;) {
      cur->a0 = (uintptr_t)primitive::RETURN;
      cur->a1 = 1;
      cur->a2 = 0x70 + i;
      cur->a3 = 0;
      cur->a4 = RETURN_STUB_MARKER;
      ctx->sp = cur;
      kernel_instance.svc_dispatch(ctx);
      assert(thread.call_stack.depth == i);
      assert(thread.call_stack.call_count == i);
      cur = ctx->sp;
      assert(cur == frames[i]);
      assert(cur->a0 == (uintptr_t)object_error::OK && cur->a1 == 0x70 + i);
    }
    assert(thread.call_stack.depth == 0);
    assert(thread.call_stack.call_count == 0);
    assert(cur == frame);
    printf("  19b. normal 32-level call nesting (pure creator chain, distinct stack frames, call 33 rejected, full 32-level unwind): PASS\n");

    // Clean up
    thread.call_stack.depth = 0;
    thread.call_stack.call_count = 0;
    thread.call_stack.top = 0;
    thread.call_stack.call_top = 0;
    kernel_instance.set_thread_ledger(
        0, ledger, 32, ROOT_APP_ID, (uint32_t)object_kind::PLAIN,
        ROOT_KOBJ_ID, ROOT_HANDLER_ENTRY);
    ctx->sp = frame;
  }

#if defined(SHIZUKU_TEST_REAL_KERNEL_OBJECT)
  // 20. 3A through the real dispatcher and KERNEL_OBJECT::grant_region. This
  // target links handler.cpp; the ordinary dispatcher fixtures above retain their
  // lighter test-only callee_request implementation.
  {
    constexpr uint32_t kBase = 1;
    constexpr uintptr_t kBaseWindow = 0x5000;
    constexpr uintptr_t kBaseLimit = 0x6000;
    // Exercise the production accessor through the shared resolver, including
    // a stale nonzero row and IDs beyond the object table.
    access::set_region(kBase, kBaseWindow, kBaseLimit);
    auto window = shizuku::native::resolve_return_region_window(kernel_object_instance, 1, 2, kBase);
    assert(window.base == kBaseWindow && window.limit == kBaseLimit);
    access::set_created(kBase, false);
    window = shizuku::native::resolve_return_region_window(kernel_object_instance, 1, 2, kBase);
    assert(window.base == 0 && window.limit == 0);
    access::set_created(kBase, true);
    window = shizuku::native::resolve_return_region_window(kernel_object_instance, 1, 2, KERNEL_OBJECT::OBJECT_COUNT);
    assert(window.base == 0 && window.limit == 0);
    window = shizuku::native::resolve_return_region_window(kernel_object_instance, 1, 2, UINT32_MAX);
    assert(window.base == 0 && window.limit == 0);
    window = shizuku::native::resolve_return_region_window(kernel_object_instance, 2, 2, kBase);
    assert(window.base == 0 && window.limit == 0);
    access::set_creator(C_ID, B_ID); // 8g above temporarily changed C's creator to A.
    auto call_b = [&]() {
      fresh(kBase, true);
      access::set_region(kBase, kBaseWindow, kBaseLimit);
      ctx->region_base = kBaseWindow;
      ctx->region_limit = kBaseLimit;
      frame->mepc = A_PC;
      frame->a0 = (uintptr_t)object_api::CALL_METHOD;
      frame->a1 = B_ID;
      frame->a2 = 0;
      frame->a3 = 0x51;
      kernel_instance.svc_dispatch(ctx);
      assert(kernel_instance.current_depth() == 1 && thread.call_stack.call_count == 1);
      assert(thread.ledger[0].via == KERNEL::LEDGER_CALL && thread.ledger[0].object == B_ID);
    };

    // Depth-zero caller: ordinary native method return restores the caller's current row.
    call_b();
    frame->a0 = (uintptr_t)primitive::RETURN;
    frame->a1 = 1;
    frame->a2 = 0x5201;
    frame->a3 = 0;
    frame->a4 = RETURN_STUB_MARKER;
    kernel_instance.svc_dispatch(ctx);
    assert(kernel_instance.current_depth() == 0 && thread.call_stack.call_count == 0);
    assert(ctx->sp == frame && frame->mepc == A_PC && ctx->privileged);
    assert(frame->a0 == (uintptr_t)object_error::OK && frame->a1 == 0x5201);
    assert(ctx->region_base == kBaseWindow && ctx->region_limit == kBaseLimit);
    assert(thread.identity_object() == kBase);

    // Nonzero caller: B -> C native method return uses B's current row.
    constexpr uintptr_t kBWindow = 0x22000;
    constexpr uintptr_t kBLimit = 0x23000;
    access::set_region(B_ID, kBWindow, kBLimit);
    access::set_region(C_ID, 0x30000, 0x31000);
    assert(access::can_call(B_ID, C_ID));
    call_b();
    frame->mepc = 0x42030100;
    frame->a0 = (uintptr_t)object_api::CALL_METHOD;
    frame->a1 = C_ID;
    frame->a2 = 0;
    frame->a3 = 0x52;
    kernel_instance.svc_dispatch(ctx);
    if (kernel_instance.current_depth() != 2 || thread.call_stack.call_count != 2)
      fprintf(stderr, "3A B->C method result: depth=%u calls=%u err=%lu value=%lu via=%u\n",
              kernel_instance.current_depth(), thread.call_stack.call_count,
              (unsigned long)frame->a0, (unsigned long)frame->a1,
              kernel_instance.current_depth() > 0 ? thread.ledger[kernel_instance.current_depth() - 1].via : 0);
    assert(kernel_instance.current_depth() == 2 && thread.call_stack.call_count == 2);
    assert(thread.ledger[1].via == KERNEL::LEDGER_CALL && thread.ledger[0].via == KERNEL::LEDGER_CALL);
    frame->a0 = (uintptr_t)primitive::RETURN;
    frame->a1 = 1;
    frame->a2 = 0x5202;
    frame->a3 = 0;
    frame->a4 = RETURN_STUB_MARKER;
    kernel_instance.svc_dispatch(ctx);
    assert(kernel_instance.current_depth() == 1 && thread.call_stack.call_count == 1);
    assert(ctx->sp == frame && frame->mepc == 0x42030100 && ctx->privileged);
    assert(frame->a0 == (uintptr_t)object_error::OK && frame->a1 == 0x5202);
    assert(ctx->region_base == kBWindow && ctx->region_limit == kBLimit);
    assert(thread.identity_object() == B_ID);

    // Descriptor route also returns to the nonzero caller row.
    access::set_instance_generation(B_ID, 3);
    access::set_instance_generation(C_ID, 4);
    access::set_version(C_ID, 1);
    access::declare_descriptor(B_ID, 0, C_ID, ENTRY_C, 3, 1);
    call_b();
    // B is the active caller in the ledger; create the same descriptor call from B.
    frame->mepc = 0x42030200;
    frame->a0 = (uintptr_t)object_api::CALL_METHOD_DESCRIPTOR;
    frame->a1 = 0;
    frame->a2 = 0x53;
    kernel_instance.svc_dispatch(ctx);
    assert(kernel_instance.current_depth() == 2 && thread.call_stack.call_count == 2);
    assert(thread.ledger[1].via == KERNEL::LEDGER_DESCRIPTOR_CALL);
    frame->a0 = (uintptr_t)primitive::RETURN;
    frame->a1 = 1;
    frame->a2 = 0x5203;
    frame->a3 = 0;
    frame->a4 = RETURN_STUB_MARKER;
    kernel_instance.svc_dispatch(ctx);
    assert(kernel_instance.current_depth() == 1 && thread.call_stack.call_count == 1);
    assert(ctx->sp == frame && frame->mepc == 0x42030200 && ctx->privileged);
    assert(frame->a0 == (uintptr_t)object_error::OK && frame->a1 == 0x5203);
    assert(ctx->region_base == kBWindow && ctx->region_limit == kBLimit);
    assert(thread.identity_object() == B_ID);

    // A real parent-handler dispatch occurs while B is active. Its nested native
    // method return must leave the handler's window closed at 0/0.
    call_b();
    frame->mepc = 0x42030300;
    frame->a0 = (uintptr_t)object_api::GRANT_REGION;
    frame->a1 = B_ID;
    frame->a2 = kBWindow;
    frame->a3 = kBLimit;
    kernel_instance.svc_dispatch(ctx);
    assert(kernel_instance.current_depth() == 2);
    assert(thread.ledger[1].via == KERNEL::LEDGER_DISPATCH);
    assert(ctx->privileged && ctx->region_base == 0 && ctx->region_limit == 0);
    frame->mepc = 0x42030340;
    frame->a0 = (uintptr_t)object_api::CALL_METHOD;
    frame->a1 = C_ID;
    frame->a2 = 0;
    frame->a3 = 0x54;
    kernel_instance.svc_call_method(ctx); // actual native body, executing as the dispatched handler
    assert(kernel_instance.current_depth() == 3 && thread.call_stack.call_count == 2);
    frame->a0 = (uintptr_t)primitive::RETURN;
    frame->a1 = 2;
    frame->a2 = 0x5204;
    frame->a3 = 0;
    frame->a4 = RETURN_STUB_MARKER;
    kernel_instance.svc_exit_method(ctx);
    assert(kernel_instance.current_depth() == 2 && thread.call_stack.call_count == 1);
    assert(thread.ledger[1].via == KERNEL::LEDGER_DISPATCH);
    assert(ctx->sp == frame && frame->mepc == 0x42030340 && ctx->privileged);
    assert(frame->a0 == (uintptr_t)object_error::OK && frame->a1 == 0x5204);
    assert(ctx->region_base == 0 && ctx->region_limit == 0);

    // The production grant_region body revokes A while A is suspended in privileged B;
    // B then returns through the real native exit path and A must resume at 0/0.
    call_b();
    object_error grant_error = object_error::OK;
    const uintptr_t revoked = access::grant_region(kBase, 0, 0, grant_error);
    assert(grant_error == object_error::OK && revoked == 1);
    assert(access::region_base(kBase) == 0 && access::region_limit(kBase) == 0);
    frame->a0 = (uintptr_t)primitive::RETURN;
    frame->a1 = 0;
    frame->a2 = 0x5300;
    frame->a3 = 0;
    frame->a4 = RETURN_STUB_MARKER;
    kernel_instance.svc_dispatch(ctx);
    assert(kernel_instance.current_depth() == 0 && thread.call_stack.call_count == 0);
    assert(ctx->sp == frame && frame->mepc == A_PC && ctx->privileged);
    assert(frame->a0 == (uintptr_t)object_error::OK && frame->a1 == 0x5300);
    assert(ctx->region_base == 0 && ctx->region_limit == 0);
    assert(thread.identity_object() == kBase);
    printf("  20. REAL KOBJ 3A: current caller row (depth 0/nonzero), method/descriptor returns, nested dispatch 0/0, production grant_region revoke 0/0: PASS\n");
  }
#endif

  // 21. consume_pending_step の fast path。予約が無ければ素通り (debug_step を掛けない)、
  //   別スレッド宛ての予約は消費せず、自分宛ての予約はちょうど 1 回だけ MON_STEP を掛ける。
  {
    auto &steps = chips::esp32_c6::g_debug_step_on;
    const uint32_t self = kernel_instance.current_thread_id();
    const uint32_t other = self + 1;
    // この試験は init() を通らない (旗が 0 のまま) ので、まず「予約なし」へ明示的に戻す。
    kernel_instance.arm_step(KERNEL::NO_STEP_TARGET);
    const uint32_t before = steps;
    kernel_instance.consume_pending_step();
    assert(steps == before); // 予約なし: 何も起きない
    kernel_instance.arm_step(other);
    kernel_instance.consume_pending_step();
    assert(steps == before); // 他スレッド宛て: 掛けない
    kernel_instance.consume_pending_step();
    assert(steps == before); // 予約は残っている (消費されていない)
    kernel_instance.arm_step(self);
    kernel_instance.consume_pending_step();
    assert(steps == before + 1); // 自分宛て: 次の復帰で必ず 1 回
    kernel_instance.consume_pending_step();
    assert(steps == before + 1); // 消費済み: 2 回目は掛けない
    kernel_instance.arm_step(other);
    kernel_instance.arm_step(self); // 上書き予約 (予約は 1 件)
    kernel_instance.consume_pending_step();
    assert(steps == before + 2);
    printf("  21. consume_pending_step fast path: no-pending passes through, foreign arm kept, own arm fires exactly once: PASS\n");
  }

  // 後始末
  thread.call_stack.depth = 0;
  thread.call_stack.call_count = 0;
  thread.call_stack.top = 0;
  thread.call_stack.call_top = 0;
  ctx->sp = frame;

  printf("[TEST] All V1 kernel dispatcher host tests PASSED successfully!\n");
}

int main() {
  run_dispatcher_tests();
  return 0;
}
