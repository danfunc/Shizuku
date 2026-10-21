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
//    4. mode == svc_mode::thread (YIELD = 8) や未移行 mode::handler (16) が
//       現行どおり actor (Root KOBJ dispatch frame) 経路に残ること。
//    5. 非特権 origin からの直接 GET で特権が昇格しないこと、および
//       mode == svc_mode::thread API (GRANT_REGION = 23) は trap で早期拒絶されず、
//       非特権のまま actor ハンドラへ配送されること。
//    6. 方法記述子の直接 call の戻り (LEDGER_DESCRIPTOR_CALL 段 + marker 付き RETURN) は
//       handler を再入させず caller へ直接戻ること。通常 CALL の戻りと検証失敗は従来経路のまま。

#define SHIZUKU_C6_SKELETON_PERMITTED 1
#include "shizuku/svc_attributes.hpp"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>

namespace shizuku {
KERNEL kernel_instance;
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

  alignas(16) static uint8_t simulated_stack[4096];
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

  // 4b. MEMORY_OWNER (16): mode == svc_mode::handler, but unmigrated in V1
  frame->a0 = (uintptr_t)object_api::MEMORY_OWNER;
  kernel_instance.svc_dispatch(ctx);
  assert(kernel_instance.current_depth() == 1);
  assert(ARCH::frame_pc(*ctx->sp) == ROOT_HANDLER_ENTRY);
  assert(ARCH::arg(*ctx->sp, 0) == (uintptr_t)object_api::MEMORY_OWNER);
  printf("  4b. unmigrated mode=handler MEMORY_OWNER remains on actor path: PASS\n");

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
  // 6. 方法記述子の直接 call の戻り (callee の return stub) の高速出口
  // -------------------------------------------------------------------------
  // 台帳の最上段が LEDGER_DESCRIPTOR_CALL で、戻りが marker 付き RETURN のときだけ、
  // handler を再入させず caller へ直接戻す。LEDGER_CALL の段や深さ検証に落ちる段は従来経路。
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

  // 6b. LEDGER_CALL (通常 CALL) の戻りは従来どおり handler へ再入する。
  arm_frame(KERNEL::LEDGER_CALL, 0, callee);
  kernel_instance.svc_dispatch(ctx);
  assert(kernel_instance.current_depth() == 2); // dispatch 段が積まれた
  assert(thread.ledger[1].via == KERNEL::LEDGER_DISPATCH);
  assert(ARCH::frame_pc(*ctx->sp) == ROOT_HANDLER_ENTRY);
  printf("  6b. plain CALL return keeps handler re-entry (depth=2, pc=root handler): PASS\n");

  // 6c. 戻り先が unwind floor を割る descriptor 段は fast path に入らず従来経路へ落ちる。
  thread.call_stack.depth = 0;
  thread.call_stack.call_count = 0;
  thread.call_stack.top = 0;
  arm_frame(KERNEL::LEDGER_DESCRIPTOR_CALL, 1, callee);
  kernel_instance.svc_dispatch(ctx);
  assert(kernel_instance.current_depth() == 2);
  assert(thread.ledger[1].via == KERNEL::LEDGER_DISPATCH);
  printf("  6c. descriptor return below unwind floor falls back to handler path (depth=2): PASS\n");

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
