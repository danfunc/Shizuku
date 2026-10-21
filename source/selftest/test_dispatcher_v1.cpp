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

#define SHIZUKU_C6_SKELETON_PERMITTED 1
#include "chips/esp32_c6/chip.hpp"
#include "chips/esp32_c6/rv32_c6.hpp"
#include "boards/esp32_c6/esp32_c6.hpp"
#include "shizuku/templates/cpu_manager.hpp"
#include "shizuku/templates/kernel.hpp"
#include "shizuku/templates/kernel_object.hpp"
#include "shizuku/svc_attributes.hpp"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>

namespace shizuku {
using ARCH = archs::rv32_c6;
using CHIP = chips::esp32_c6::chip;
using BOARD = boards::esp32_c6;
using CPU_MANAGER = templates::cpu_manager<ARCH, CHIP, BOARD, 1>;
struct test_memory_manager {};
using MEMORY_MANAGER = test_memory_manager;
using KERNEL = templates::kernel<CPU_MANAGER, MEMORY_MANAGER>;
using KERNEL_OBJECT = templates::kernel_object<KERNEL, 64, 16, 32, 32>;

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

#include "source/kernel/thread.cpp"
#include "source/kernel/dispatch.cpp"

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

  printf("[TEST] All V1 kernel dispatcher host tests PASSED successfully!\n");
}

int main() {
  run_dispatcher_tests();
  return 0;
}
