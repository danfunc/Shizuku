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

#define SHIZUKU_C6_SKELETON_PERMITTED 1
#include "shizuku/svc_attributes.hpp"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>

namespace shizuku {
KERNEL kernel_instance;
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
  return request;
}

namespace selftest {
// kobj の表 (private) を試験が直接組み立てるための口。
struct dispatcher_test_access {
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
}

namespace shizuku::chips::esp32_c6 {
void plic_vector_mode::debug_step(bool) {}
uint32_t plic_vector_mode::debug_reason_take() { return 0; }
uint32_t plic_vector_mode::fault_status() { return 0; }
void plic_vector_mode::fault_status_clear() {}
uintptr_t plic_vector_mode::fault_address() { return 0; }
void plic_vector_mode::timer_oneshot(unsigned int) {}
uint32_t plic_vector_mode::timer_remaining(bool &) { return 0; }
void plic_vector_mode::timer_cancel() {}
void plic_vector_mode::pend_context_switch() {}
template <>
bool plic_vector_mode::faulted_in_thread_mode<shizuku::archs::rv32_c6::context_t>(
    const shizuku::archs::rv32_c6::context_t &) { return true; }
}

using namespace shizuku;

static void run_dispatcher_tests() {
  printf("[TEST] Running V1 kernel dispatcher host tests...\n");

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
  thread.ledger[0] = {LEAF_ID, ROOT_KOBJ_ID, ROOT_HANDLER_ENTRY,
                      (uint8_t)object_kind::PLAIN, KERNEL::LEDGER_CALL, 0};
  thread.call_stack.depth = 1;
  thread.call_stack.call_count = 1;
  thread.call_stack.current_call_object = LEAF_ID;
  thread.call_stack.caller_object = ROOT_APP_ID;

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
  thread.ledger[1] = {NEST_ID, ROOT_KOBJ_ID, ROOT_HANDLER_ENTRY,
                      (uint8_t)object_kind::PLAIN, KERNEL::LEDGER_CALL, 1};
  thread.call_stack.depth = 2;
  thread.call_stack.call_count = 2;
  thread.call_stack.current_call_object = NEST_ID;
  thread.call_stack.caller_object = LEAF_ID;

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
    ctx->sp = frame;
  }

  // -------------------------------------------------------------------------
  // 4. Mode=thread actor path & unmigrated mode=handler API on root-bound object
  // -------------------------------------------------------------------------
  kernel_instance.set_thread_ledger(
      0, ledger, 32, ROOT_APP_ID, (uint32_t)object_kind::PLAIN,
      ROOT_KOBJ_ID, ROOT_HANDLER_ENTRY);

  // 4a. YIELD (8): mode == svc_mode::thread
  frame->a0 = (uintptr_t)object_api::YIELD;
  kernel_instance.svc_dispatch(ctx);
  assert(kernel_instance.current_depth() == 1);
  assert(ARCH::frame_pc(*ctx->sp) == ROOT_HANDLER_ENTRY);
  assert(ARCH::arg(*ctx->sp, 0) == (uintptr_t)object_api::YIELD);
  printf("  4a. mode=thread YIELD remains on actor dispatch path (depth=1, pc=root): PASS\n");

  thread.call_stack.depth = 0;
  thread.call_stack.top = 0;
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
  ctx->sp = frame;

  // -------------------------------------------------------------------------
  // 5. Unprivileged origin: privilege preservation & privileged API rejection
  // -------------------------------------------------------------------------
  ctx->privileged = false;

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
  const auto *header = (const KERNEL::call_frame_header *)thread.call_stack.top;
  assert(header->saved.privileged == false); // Caller privilege preserved as unprivileged!

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
  auto arm_frame = [&](uint8_t via, uint16_t unwind_floor, ARCH::exception_frame_t *callee) {
    ctx->sp = frame;
    ctx->privileged = true;
    static KERNEL::call_frame_header header;
    header = {};
    header.saved = *ctx; // 呼び出し元の文脈 (sp = frame)
    thread.call_stack.top = (uintptr_t)&header;
    thread.call_stack.depth = 1;
    thread.call_stack.call_count = 1;
    thread.call_stack.identity[0] = {(uint32_t)KERNEL_OBJECT::NO_OBJECT, 0};
    thread.call_stack.caller_object = ROOT_APP_ID;
    thread.call_stack.current_call_object = LEAF_ID;
    thread.call_stack.caller_generation = 0;
    thread.call_stack.current_call_generation = 0;
    thread.ledger[0] = {LEAF_ID, ROOT_KOBJ_ID, ROOT_HANDLER_ENTRY,
                        (uint8_t)object_kind::PLAIN, via, unwind_floor};
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
  assert(thread.call_stack.current_call_object == ROOT_APP_ID); // identity を push 時の値へ戻す
  assert(thread.call_stack.caller_object == (uint32_t)KERNEL_OBJECT::NO_OBJECT);
  assert(ctx->sp == frame);
  assert(frame->a0 == (uintptr_t)kernel_error::OK);
  assert(frame->a1 == 0x77);
  assert(ctx->privileged == true);
  printf("  6a. descriptor return fast path (depth 1->0, caller frame, value 0x77, no dispatch frame): PASS\n");

  // 6b. LEDGER_CALL (通常 CALL) の戻りも同じ。handler を再入させない。
  thread.call_stack.depth = 0;
  thread.call_stack.call_count = 0;
  thread.call_stack.top = 0;
  arm_frame(KERNEL::LEDGER_CALL, 0, callee);
  kernel_instance.svc_dispatch(ctx);
  assert(kernel_instance.current_depth() == 0); // dispatch 段は積まれない
  assert(thread.call_stack.call_count == 0);
  assert(thread.call_stack.current_call_object == ROOT_APP_ID);
  assert(ctx->sp == frame);
  assert(frame->a0 == (uintptr_t)object_error::OK);
  assert(frame->a1 == 0x77);
  printf("  6b. plain CALL return handled in trap (depth 1->0, value 0x77, no dispatch frame): PASS\n");

  // 6c. 戻り先が unwind floor を割る (d = floor = 1 >= depth = 1) 段は 1 枚も落とさず
  // UNWIND_REJECTED + kernel_error::DEPTH_MISMATCH を呼び出し元 (callee フレーム) へ返す。
  thread.call_stack.depth = 0;
  thread.call_stack.call_count = 0;
  thread.call_stack.top = 0;
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
  // ユーザー決定: 2, 4, 5, 6, 9, 12, 33 (8, 10, 11 はスケジューラ設計待ちで thread のまま。
  // 16, 18, 20, 29, 30, 31 は O(1) でない / 錠を取るので thread に戻した)。
  for (unsigned id = 1; id < 34; ++id) {
    const bool expect_handler = id == 2 || id == 4 || id == 5 || id == 6 ||
                                id == 9 || id == 12 || id == 33;
    assert((svc_attribute_for(id)->mode == svc_mode::handler) == expect_handler);
  }
  printf("  7. svc_attributes handler set == {2,4,5,6,9,12,33}: PASS\n");

  // -------------------------------------------------------------------------
  // 8. CALL_METHOD (4) / EXIT_METHOD (2): A (base) → B → C
  // -------------------------------------------------------------------------
  using access = selftest::dispatcher_test_access;
  constexpr uint32_t B_ID = LEAF_ID, C_ID = NEST_ID;
  constexpr uintptr_t ENTRY_B = 0x42030000, ENTRY_C = 0x42031000;
  access::declare(B_ID, 0, reinterpret_cast<KERNEL_OBJECT::method_t>(ENTRY_B), 0,
                  ROOT_KOBJ_ID, ROOT_HANDLER_ENTRY);
  access::declare(C_ID, 0, reinterpret_cast<KERNEL_OBJECT::method_t>(ENTRY_C),
                  OBJECT_UNPRIVILEGED, ROOT_KOBJ_ID, ROOT_HANDLER_ENTRY);
  auto count_dispatch_frames = [&]() {
    unsigned n = 0;
    for (uint32_t i = 0; i < thread.call_stack.depth; ++i)
      n += thread.ledger[i].via == KERNEL::LEDGER_DISPATCH;
    return n;
  };

  // 8a. A → B。dispatch 段は積まれず、台帳の最上段は B の LEDGER_CALL。
  fresh(ROOT_APP_ID, true);
  frame->a0 = (uintptr_t)object_api::CALL_METHOD;
  frame->a1 = B_ID;
  frame->a2 = 0;
  frame->a3 = 0x55;
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
  assert(ctx->sp != frame);
  assert(ctx->privileged == true); // B は特権オブジェクト
  KERNEL::FRAME *frame_b = ctx->sp;
  printf("  8a. CALL_METHOD A->B: no dispatch frame, ledger top = B via LEDGER_CALL, pc=B entry, arg0=0x55: PASS\n");

  // 8b. B が GET_CALLER_OBJECT を撃つと本当の呼び出し元 A (kobj の ID 0 ではない)。
  frame_b->a0 = (uintptr_t)object_api::GET_CALLER_OBJECT;
  kernel_instance.svc_dispatch(ctx);
  assert(frame_b->a0 == (uintptr_t)object_error::OK);
  assert(frame_b->a1 == ROOT_APP_ID && frame_b->a1 != ROOT_KOBJ_ID);
  frame_b->a0 = (uintptr_t)object_api::GET_CURRENT_OBJECT;
  kernel_instance.svc_dispatch(ctx);
  assert(frame_b->a1 == B_ID);

  // 8c. B → C (C は非特権オブジェクト)。C から見た caller は B。
  frame_b->a0 = (uintptr_t)object_api::CALL_METHOD;
  frame_b->a1 = C_ID;
  frame_b->a2 = 0;
  frame_b->a3 = 0x66;
  kernel_instance.svc_dispatch(ctx);
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
  frame_c->a0 = (uintptr_t)object_api::EXIT_METHOD;
  frame_c->a1 = 1;
  frame_c->a2 = 0x32;
  frame_c->a3 = 0;
  kernel_instance.svc_dispatch(ctx);
  assert(kernel_instance.current_depth() == 1);
  assert(thread.call_stack.call_count == 1);
  assert(count_dispatch_frames() == 0);
  assert(ctx->sp == frame_b);
  assert(frame_b->a0 == (uintptr_t)object_error::OK);
  assert(frame_b->a1 == 0x32);
  assert(ctx->privileged == true); // B の文脈 (特権) が戻る
  frame_b->a0 = (uintptr_t)object_api::GET_CALLER_OBJECT;
  kernel_instance.svc_dispatch(ctx);
  assert(frame_b->a1 == ROOT_APP_ID); // 戻ったので B から見た caller は再び A
  printf("  8d. EXIT_METHOD explicit d: d=0/d=2 rejected (nothing popped), d=1 returns C->B with value 0x32, caller identity restored: PASS\n");

  // 8e. B が return stub (marker) で戻る → A へ直接。error (a3) と値 (a2) が A のフレームへ。
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
  assert(thread.call_stack.current_call_object == ROOT_APP_ID);
  printf("  8e. B return stub -> A directly (depth 0, value 0x44, no dispatch frame): PASS\n");

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
    fresh(ROOT_APP_ID, privileged);
    thread.call_stack.call_count = KERNEL_OBJECT::MAX_DEPTH;
    frame->a0 = (uintptr_t)object_api::CALL_METHOD;
    frame->a1 = B_ID;
    frame->a2 = 0;
    kernel_instance.svc_dispatch(ctx);
    assert(kernel_instance.current_depth() == 0);
    assert(frame->a0 == (uintptr_t)object_error::NO_STACK);
    assert(frame->a1 == 0);
    assert(ctx->privileged == privileged);
  }
  printf("  8f. CALL_METHOD errors BAD_OBJECT x2 / BAD_METHOD / UNDECLARED_METHOD / NO_STACK reply (error,0), depth 0, privilege unchanged: PASS\n");

  // 8g. 非特権の呼び出し元が非特権オブジェクトを呼んでも特権化しない。戻りで元の文脈。
  fresh(ROOT_APP_ID, false);
  frame->a0 = (uintptr_t)object_api::CALL_METHOD;
  frame->a1 = C_ID;
  frame->a2 = 0;
  frame->a3 = 0x11;
  kernel_instance.svc_dispatch(ctx);
  assert(kernel_instance.current_depth() == 1);
  assert(ctx->privileged == false);
  assert(((const KERNEL::call_frame_header *)thread.call_stack.top)->saved.privileged == false);
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
                         object_api::RUN_FOR, object_api::SET_BUDGET}) {
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

  // 後始末
  thread.call_stack.depth = 0;
  thread.call_stack.call_count = 0;
  thread.call_stack.top = 0;
  ctx->sp = frame;

  printf("[TEST] All V1 kernel dispatcher host tests PASSED successfully!\n");
}

int main() {
  run_dispatcher_tests();
  return 0;
}
