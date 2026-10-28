#include "shizuku/config.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>

namespace shizuku {
KERNEL kernel_instance;
KERNEL_OBJECT kernel_object_instance;

namespace selftest {
struct dispatcher_test_access {
  static void declare(uint32_t id, uint32_t creator, uintptr_t entry,
                      uintptr_t region_base, uintptr_t region_limit) {
    auto &row = kernel_object_instance.m_objects[id];
    row.created = true;
    row.creator_object = creator;
    row.kind = object_kind::PLAIN;
    row.flags = 0;
    row.parent_handler_object = 0;
    row.parent_handler_entry = 0x42010000u;
    row.methods[0] = (KERNEL_OBJECT::method_t)entry;
    row.region_base = (uint32_t)region_base;
    row.region_limit = (uint32_t)region_limit;
  }
};
}  // namespace selftest

template <>
call_request KERNEL_OBJECT::callee_request(uintptr_t id, uintptr_t entry_pc) const {
  call_request request{};
  request.entry_pc = entry_pc;
  request.callee_object = (uint32_t)id;
  request.callee_kind = (uint32_t)m_objects[id].kind;
  request.callee_generation = m_objects[id].instance_generation;
  request.parent_handler_object = m_objects[id].parent_handler_object;
  request.parent_handler_entry = m_objects[id].parent_handler_entry;
  request.protection = (m_objects[id].flags & OBJECT_UNPRIVILEGED)
                           ? PROTECTION_UNPRIVILEGED
                           : PROTECTION_PRIVILEGED;
  request.region_base = m_objects[id].region_base;
  request.region_limit = m_objects[id].region_limit;
  return request;
}
}  // namespace shizuku

// Link placeholders only: the ARM assembly veneers are not executed on host.
extern "C" void shizuku_armv8m_call_entry() {}
extern "C" void shizuku_armv8m_return_stub() {}

using namespace shizuku;
static_assert(ARCH::LEGACY_FRAME_HOST_TEST,
              "legacy frame test must compile with its host MPU adapter");

int main() {
  alignas(16) static uint8_t stack[8192]{};
  alignas(8) static uint8_t thread_storage[KERNEL::thread_record_bytes()];
  alignas(8) static KERNEL::call_ledger_entry ledger[8]{};
  kernel_instance.set_thread_storage(thread_storage, sizeof(thread_storage));
  kernel_instance.set_object_handler(0x42010000u, 0);
  kernel_instance.set_thread_ledger(0, ledger, 8, 1, (uint32_t)object_kind::PLAIN,
                                    0, 0x42010000u);

  selftest::dispatcher_test_access::declare(1, 0, 0x42001000u, 0x20001000u, 0x20002000u);
  selftest::dispatcher_test_access::declare(2, 1, 0x42002000u, 0, 0);
  selftest::dispatcher_test_access::declare(3, 2, 0x42003000u, 0, 0);
  auto *context = kernel_instance.current_context();
  auto *caller = reinterpret_cast<KERNEL::FRAME *>(
      stack + sizeof(stack) - sizeof(KERNEL::FRAME) - 64);
  context->sp = caller;
  ARCH::stack_limit_set(*context, (uintptr_t)stack);
  ARCH::set_priv(*context, true);
  context->r4 = 0x54u;
  caller->pc = 0x42000100u;
  caller->lr = 0x42000101u;
  caller->r0 = 0x11u;
  caller->r1 = 0x22u;
  caller->r2 = 0x33u;
  caller->r3 = 0x44u;

  auto call_b = [&]() {
    context->sp = caller;
    caller->pc = 0x42000100u;
    caller->r0 = (uint32_t)object_api::CALL_METHOD;
    caller->r1 = 2;
    caller->r2 = 0;
    caller->r3 = 0x51u;
    kernel_instance.svc_call_method(context);
    assert(kernel_instance.current_depth() == 1);
    assert(kernel_instance.current_call_count() == 1);
    assert(context->sp != caller && ARCH::frame_pc(*context->sp) == 0x42002000u);
    // S2 removes kernel-managed callee-saved scrubbing even on this legacy
    // frame adapter; the compiler caller converter now owns that contract.
    assert(context->r4 == 0x54u);
  };

  // Old ARM frame path, depth-zero caller: preserve the live row on return.
  call_b();
  auto *callee = context->sp;
  callee->r0 = (uint32_t)primitive::RETURN;
  callee->r1 = 0;
  callee->r2 = 0x61u;
  callee->r3 = 0;
  callee->r12 = (uint32_t)RETURN_STUB_MARKER;
  kernel_instance.svc_exit_method(context);
  assert(kernel_instance.current_depth() == 0 && kernel_instance.current_call_count() == 0);
  assert(context->sp == caller && ARCH::frame_pc(*caller) == 0x42000100u);
  assert(ARCH::context_privileged(*context));
  assert(context->r4 == 0x54u);
  assert(caller->r0 == 0 && caller->r1 == 0x61u);
  assert(context->region_base == 0x20001000u && context->region_limit == 0x20002000u);

  // Actual old frame push/pop: changing the caller's current row while its frame
  // is suspended revokes the return window.
  call_b();
  callee = context->sp;
  selftest::dispatcher_test_access::declare(1, 0, 0x42001000u, 0, 0);
  callee->r0 = (uint32_t)primitive::RETURN;
  callee->r1 = 0;
  callee->r2 = 0x62u;
  callee->r3 = 0;
  callee->r12 = (uint32_t)RETURN_STUB_MARKER;
  kernel_instance.svc_exit_method(context);
  assert(kernel_instance.current_depth() == 0 && kernel_instance.current_call_count() == 0);
  assert(context->sp == caller && ARCH::frame_pc(*caller) == 0x42000100u);
  assert(ARCH::context_privileged(*context));
  assert(context->r4 == 0x54u);
  assert(caller->r0 == 0 && caller->r1 == 0x62u);
  assert(context->region_base == 0 && context->region_limit == 0);

  // Actual svc_forward dispatch followed by a nested method call. Returning to the
  // dispatch activation must keep its closed 0/0 window.
  call_b();
  callee = context->sp;
  callee->pc = 0x42002040u;
  callee->r0 = (uint32_t)object_api::GRANT_REGION;
  callee->r1 = 2;
  callee->r2 = 0;
  callee->r3 = 0;
  kernel_instance.svc_dispatch(context);
  assert(kernel_instance.current_depth() == 2);
  assert(ledger[1].via == KERNEL::LEDGER_DISPATCH);
  assert(context->region_base == 0 && context->region_limit == 0);
  auto *handler_frame = context->sp;
  handler_frame->pc = 0x42010040u;
  handler_frame->r0 = (uint32_t)object_api::CALL_METHOD;
  handler_frame->r1 = 3;
  handler_frame->r2 = 0;
  handler_frame->r3 = 0x63u;
  kernel_instance.svc_call_method(context);
  assert(kernel_instance.current_depth() == 3 && kernel_instance.current_call_count() == 2);
  auto *nested = context->sp;
  nested->r0 = (uint32_t)primitive::RETURN;
  nested->r1 = 2;
  nested->r2 = 0x64u;
  nested->r3 = 0;
  nested->r12 = (uint32_t)RETURN_STUB_MARKER;
  kernel_instance.svc_exit_method(context);
  assert(kernel_instance.current_depth() == 2 && kernel_instance.current_call_count() == 1);
  assert(context->sp == handler_frame && ARCH::frame_pc(*handler_frame) == 0x42010040u);
  assert(ARCH::context_privileged(*context));
  assert(handler_frame->r0 == 0 && handler_frame->r1 == 0x64u);
  assert(context->region_base == 0 && context->region_limit == 0);

  printf("[PASS] actual ARM legacy call_frame/dispatch return windows\n");
  return 0;
}
