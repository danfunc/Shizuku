#ifndef SHIZUKU_ARM_INPLACE_FIXTURE_HPP
#define SHIZUKU_ARM_INPLACE_FIXTURE_HPP

#ifdef NDEBUG
#error "arm_inplace tests rely on assert(); do not build with NDEBUG (-c opt)"
#endif
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <type_traits>

#include "shizuku/kernel_abi.hpp"
#include "shizuku/native_call.hpp"
#include "shizuku/object_api.hpp"
#include "shizuku/templates/thread.hpp"

// ---------------------------------------------------------------------------
// 1. Shared Payload Trait & Size Definitions
// ---------------------------------------------------------------------------
struct test_arm_callee_saved_payload {
  uint32_t r4_r11[8];
};

// Simulated 32-bit call_ledger_entry to verify 32-bit target sizes on any host.
struct call_ledger_entry_32bit_empty_nonlean {
  uint32_t object;
  uint32_t handler_object;
  uint32_t handler_entry;
  uint8_t kind;
  uint8_t via;
  uint16_t unwind_floor;
  [[no_unique_address]] shizuku::empty_ledger_payload payload{};
};
static_assert(sizeof(call_ledger_entry_32bit_empty_nonlean) == 16,
              "32-bit non-LEAN call_ledger_entry with empty payload must be exactly 16 bytes");

struct call_ledger_entry_32bit_empty_lean {
  uint32_t object;
  uint32_t handler_object;
  uint32_t handler_entry;
  uint8_t kind;
  uint8_t via;
  uint16_t unwind_floor;
  uint32_t generation;
  uint32_t ret_pc;
  uint32_t ret_sp;
  uint32_t ret_region_base;
  uint32_t ret_region_limit;
  uint16_t prev_call;
  uint8_t ret_privileged;
  uint8_t reserved;
  [[no_unique_address]] shizuku::empty_ledger_payload payload{};
};
static_assert(sizeof(call_ledger_entry_32bit_empty_lean) == 40,
              "32-bit LEAN call_ledger_entry with empty payload must be exactly 40 bytes");

struct call_ledger_entry_32bit_with_payload_lean {
  uint32_t object;
  uint32_t handler_object;
  uint32_t handler_entry;
  uint8_t kind;
  uint8_t via;
  uint16_t unwind_floor;
  uint32_t generation;
  uint32_t ret_pc;
  uint32_t ret_sp;
  uint32_t ret_region_base;
  uint32_t ret_region_limit;
  uint16_t prev_call;
  uint8_t ret_privileged;
  uint8_t reserved;
  [[no_unique_address]] test_arm_callee_saved_payload payload{};
};
static_assert(sizeof(call_ledger_entry_32bit_with_payload_lean) == 72,
              "32-bit LEAN call_ledger_entry with 32B ARM payload must be exactly 72 bytes (40 + 32)");
static_assert(offsetof(call_ledger_entry_32bit_with_payload_lean, payload) == 40,
              "32-bit LEAN payload offset must be at byte 40");

// ---------------------------------------------------------------------------
// 2. Host Test-Only Derived Arch Adapter (Test Seams Only)
// ---------------------------------------------------------------------------
template <typename BaseArch>
struct test_inplace_adapter : public BaseArch {
  // Test-only enablement: production BaseArch keeps CALL_IN_PLACE == false
  static constexpr bool CALL_IN_PLACE = true;

  struct call_header_t {
    uintptr_t prev = 0;
    uintptr_t pc = 0;
    uint32_t caller_object = 0;
    uint32_t caller_generation = 0;
    uintptr_t region_base = 0;
    uintptr_t region_limit = 0;
    uint16_t unwind_floor = 0;
    uint8_t via = 0;
    uint8_t privileged = 0;
    uint32_t reserved = 0;
  };

  static bool call_header_has_room(const typename BaseArch::context_t &, uint32_t) {
    return true;
  }

  static uintptr_t call_return_sp(const typename BaseArch::context_t &ctx) {
    return reinterpret_cast<uintptr_t>(ctx.sp) + sizeof(typename BaseArch::exception_frame_t);
  }

  static typename BaseArch::exception_frame_t *call_resume_at(
      typename BaseArch::context_t &ctx, uintptr_t pc, uintptr_t sp,
      bool privileged, uintptr_t rbase, uintptr_t rlimit, bool scrub) {
    auto *frame = reinterpret_cast<typename BaseArch::exception_frame_t *>(
        sp - sizeof(typename BaseArch::exception_frame_t));
    frame->pc = (uint32_t)pc;
    if (scrub) {
      frame->r0 = 0;
      frame->r1 = 0;
      frame->r2 = 0;
      frame->r3 = 0;
      frame->r12 = 0;
    }
    ctx.sp = frame;
    BaseArch::set_priv(ctx, privileged);
    BaseArch::set_region_window(ctx, rbase, rlimit);
    return frame;
  }

  static typename BaseArch::exception_frame_t *call_resume(
      typename BaseArch::context_t &ctx, const call_header_t *header, bool scrub) {
    uintptr_t sp = reinterpret_cast<uintptr_t>(header);
    return call_resume_at(ctx, header->pc, sp, header->privileged != 0,
                          header->region_base, header->region_limit, scrub);
  }

  static void set_entry(typename BaseArch::exception_frame_t &frame, uintptr_t pc, uintptr_t lr) {
    frame.pc = (uint32_t)pc;
    frame.lr = (uint32_t)lr;
  }

  static uintptr_t return_stub() {
    return 0xDEAD0000u;
  }

  static call_header_t *call_header_slot(typename BaseArch::context_t &ctx) {
    return reinterpret_cast<call_header_t *>(
        reinterpret_cast<uintptr_t>(ctx.sp) + sizeof(typename BaseArch::exception_frame_t));
  }

  template <class Frame>
  static void switch_callee_protection(typename BaseArch::context_t &ctx, Frame &, uint32_t, bool priv) {
    BaseArch::set_priv(ctx, priv);
  }

  static void switch_caller_protection(uint32_t) {}

  static uintptr_t arg(const typename BaseArch::exception_frame_t &f, unsigned index) {
    switch (index) {
    case 0: return f.r0;
    case 1: return f.r1;
    case 2: return f.r2;
    case 3: return f.r3;
    case 4: return f.r12;
    default: return 0;
    }
  }

  static void set_result(typename BaseArch::exception_frame_t &f, uintptr_t err, uintptr_t val) {
    f.r0 = (uint32_t)err;
    f.r1 = (uint32_t)val;
  }
};

// ---------------------------------------------------------------------------
// 3. Mock Kernel & Kernel Object for Instantiation Testing
// ---------------------------------------------------------------------------
template <typename Adapter>
struct mock_kernel {
  using ARCH = Adapter;
  using CONTEXT = typename ARCH::context_t;
  using FRAME = typename ARCH::exception_frame_t;
  using THREAD = shizuku::templates::thread<CONTEXT>;

  static constexpr uint8_t LEDGER_CALL = 1;
  static constexpr uint8_t LEDGER_DESCRIPTOR_CALL = 2;
  static constexpr uint32_t CORE_COUNT = 1;

  uint32_t m_svc_handler_object = 1;

  uint32_t svc_handler_object() const {
    return m_svc_handler_object;
  }
};

struct mock_kobj {
  using method_t = uintptr_t;
  static constexpr uintptr_t MAX_DEPTH = 8;
  static constexpr uint32_t METHOD_DESCRIPTOR_CAPACITY = 16;
  static constexpr uint32_t OBJECT_COUNT = 32;
  static constexpr uint8_t DESCRIPTOR_NONE = 0xFF;

  struct method_row {
    uint32_t id;
    uint32_t parent_handler_object;
    uintptr_t parent_handler_entry;
    shizuku::object_kind kind;
    bool privileged;
    uintptr_t region_base;
    uintptr_t region_limit;
    uint32_t instance_generation;
    uintptr_t entry;
  };

  struct callee_view {
    uint32_t id;
    uint32_t parent_handler_object;
    uintptr_t parent_handler_entry;
    shizuku::object_kind kind;
    bool privileged;
    uintptr_t region_base;
    uintptr_t region_limit;
    uint32_t instance_generation;
  };

  struct method_descriptor_entry {
    uint8_t seq = 0;
    uint8_t callee = DESCRIPTOR_NONE;
    uint32_t target_entry = 0;
    uint32_t owner_generation = 0;
    uint32_t target_generation = 0;
    uint32_t parent_handler_object = 1;
    uintptr_t parent_handler_entry = 0;
    shizuku::object_kind kind = shizuku::object_kind::PLAIN;
    bool privileged = false;
    uintptr_t region_base = 0;
    uintptr_t region_limit = 0;
  };

  method_row m_rows[OBJECT_COUNT][16]{};
  bool m_row_valid[OBJECT_COUNT][16]{};
  uint32_t m_object_versions[OBJECT_COUNT]{};
  method_descriptor_entry m_descriptors[OBJECT_COUNT][METHOD_DESCRIPTOR_CAPACITY]{};

  const method_row *find_method_row(uintptr_t id, uintptr_t method_idx, uint32_t caller_id,
                                    shizuku::object_error &err, method_t &entry) const {
    if (id >= OBJECT_COUNT) {
      err = shizuku::object_error::BAD_OBJECT;
      return nullptr;
    }
    if (caller_id != id && caller_id != 1) { // 1 is parent/root
      err = shizuku::object_error::NOT_PRIVILEGED;
      return nullptr;
    }
    if (method_idx >= 16) {
      err = shizuku::object_error::BAD_METHOD;
      return nullptr;
    }
    if (!m_row_valid[id][method_idx]) {
      err = shizuku::object_error::UNDECLARED_METHOD;
      return nullptr;
    }
    err = shizuku::object_error::OK;
    entry = m_rows[id][method_idx].entry;
    return &m_rows[id][method_idx];
  }

  callee_view view_of_row(uint32_t id, const method_row &row) const {
    return {id, row.parent_handler_object, row.parent_handler_entry, row.kind,
            row.privileged, row.region_base, row.region_limit, row.instance_generation};
  }

  const method_descriptor_entry &descriptor_entry(uint32_t owner, uint32_t index) const {
    return m_descriptors[owner][index];
  }

  uint32_t object_version(uint32_t callee) const {
    return callee < OBJECT_COUNT ? m_object_versions[callee] : 0;
  }

  callee_view view_of_descriptor(const method_descriptor_entry &desc) const {
    return {desc.callee, desc.parent_handler_object, desc.parent_handler_entry, desc.kind,
            desc.privileged, desc.region_base, desc.region_limit, desc.target_generation};
  }
};

// ---------------------------------------------------------------------------
// 4. Templated Test Suite for Both ARMv8-M and ARMv6-M
// ---------------------------------------------------------------------------
template <typename Adapter>
void run_inplace_method_call_success(const char *tag) {
  mock_kernel<Adapter> kernel;
  mock_kobj kobj;
  typename mock_kernel<Adapter>::CONTEXT ctx{};
  typename mock_kernel<Adapter>::FRAME frame{};
  ctx.sp = &frame;
  ctx.control = Adapter::CONTROL_UNPRIV_PSP;

  typename mock_kernel<Adapter>::THREAD thread;
  typename mock_kernel<Adapter>::THREAD::call_ledger_entry ledger[16]{};
  thread.ledger = ledger;
  thread.ledger_capacity = 16;
  thread.base_object = 1; // root caller

  // Callee setup
  kobj.m_row_valid[5][2] = true;
  kobj.m_rows[5][2] = {5, 1, 0, shizuku::object_kind::PLAIN, false, 0x20000000, 0x20001000, 1, 0x10002000};

  frame.pc = 0x10001000u;
  frame.r0 = 4; // CALL_METHOD
  frame.r1 = 5; // Callee ID
  frame.r2 = 2; // Method
  frame.r3 = 0xCAFEBABEu; // Argument
  frame.r12 = 0x12121212u; // Passthrough canary

  shizuku::native::call_method_in_place(kernel, thread, ctx, kobj);

  assert(frame.r0 == 0xCAFEBABEu);
  assert(frame.pc == 0x10002000u);
  assert(frame.lr == Adapter::return_stub());
  assert(frame.r12 == 0x12121212u);
  assert(thread.call_stack.depth == 1);
  assert(thread.call_stack.call_count == 1);
  assert(ledger[0].object == 5);
  assert(ledger[0].via == mock_kernel<Adapter>::LEDGER_CALL);
#if defined(SHIZUKU_LEAN_LEDGER)
  assert(ledger[0].ret_pc == 0x10001000u);
  assert(ledger[0].ret_privileged == 0);
#endif
  printf("  [%s] PASS: method_call_success\n", tag);
}

template <typename Adapter>
void run_inplace_descriptor_call_success(const char *tag) {
  mock_kernel<Adapter> kernel;
  mock_kobj kobj;
  typename mock_kernel<Adapter>::CONTEXT ctx{};
  typename mock_kernel<Adapter>::FRAME frame{};
  ctx.sp = &frame;
  ctx.control = Adapter::CONTROL_PRIV_PSP;

  typename mock_kernel<Adapter>::THREAD thread;
  typename mock_kernel<Adapter>::THREAD::call_ledger_entry ledger[16]{};
  thread.ledger = ledger;
  thread.ledger_capacity = 16;
  thread.base_object = 2;

  kobj.m_object_versions[6] = 4;
  kobj.m_descriptors[2][3] = {
      .seq = 2,
      .callee = 6,
      .target_entry = 0x10003000u,
      .owner_generation = 0,
      .target_generation = 4,
      .parent_handler_object = 1,
      .parent_handler_entry = 0,
      .kind = shizuku::object_kind::PLAIN,
      .privileged = false,
  };

  frame.pc = 0x10001500u;
  frame.r0 = 33; // CALL_METHOD_DESCRIPTOR
  frame.r1 = 3;  // Descriptor index
  frame.r2 = 0xDEADBEEFu; // Argument
  frame.r3 = 0x33333333u; // Passthrough canary
  frame.r12 = 0x12121212u; // Passthrough canary

  shizuku::native::call_descriptor_in_place(kernel, thread, ctx, kobj);

  // D2(a): 1 argument moved from r2 to r0
  assert(frame.r0 == 0xDEADBEEFu);
  assert(frame.pc == 0x10003000u);
  assert(frame.lr == Adapter::return_stub());
  assert(frame.r3 == 0x33333333u);
  assert(frame.r12 == 0x12121212u);
  assert(thread.call_stack.depth == 1);
  assert(ledger[0].object == 6);
  assert(ledger[0].via == mock_kernel<Adapter>::LEDGER_DESCRIPTOR_CALL);

  printf("  [%s] PASS: descriptor_call_success (ARM 1-arg D2(a))\n", tag);
}

template <typename Adapter>
void run_inplace_method_failures_ledger_invariant(const char *tag) {
  mock_kernel<Adapter> kernel;
  mock_kobj kobj;
  typename mock_kernel<Adapter>::CONTEXT ctx{};
  typename mock_kernel<Adapter>::FRAME frame{};
  ctx.sp = &frame;

  typename mock_kernel<Adapter>::THREAD thread;
  typename mock_kernel<Adapter>::THREAD::call_ledger_entry ledger[16]{};
  thread.ledger = ledger;
  thread.ledger_capacity = 16;
  thread.base_object = 3;

  // 1. Bad Object
  frame.r0 = 4;
  frame.r1 = mock_kobj::OBJECT_COUNT + 1;
  frame.r2 = 0;
  frame.r3 = 42;
  shizuku::native::call_method_in_place(kernel, thread, ctx, kobj);
  assert(frame.r0 == (uint32_t)shizuku::object_error::BAD_OBJECT);
  assert(thread.call_stack.depth == 0);
  assert(thread.call_stack.call_count == 0);

  // 2. Not Privileged
  frame.r0 = 4;
  frame.r1 = 5;
  frame.r2 = 0;
  frame.r3 = 42;
  shizuku::native::call_method_in_place(kernel, thread, ctx, kobj);
  assert(frame.r0 == (uint32_t)shizuku::object_error::NOT_PRIVILEGED);
  assert(thread.call_stack.depth == 0);
  assert(thread.call_stack.call_count == 0);

  // 3. Bad Method
  thread.base_object = 1;
  frame.r0 = 4;
  frame.r1 = 5;
  frame.r2 = 99;
  frame.r3 = 42;
  shizuku::native::call_method_in_place(kernel, thread, ctx, kobj);
  assert(frame.r0 == (uint32_t)shizuku::object_error::BAD_METHOD);
  assert(thread.call_stack.depth == 0);

  // 4. Undeclared Method
  frame.r0 = 4;
  frame.r1 = 5;
  frame.r2 = 7;
  frame.r3 = 42;
  shizuku::native::call_method_in_place(kernel, thread, ctx, kobj);
  assert(frame.r0 == (uint32_t)shizuku::object_error::UNDECLARED_METHOD);
  assert(thread.call_stack.depth == 0);

  // 5. MAX_DEPTH exceeded
  kobj.m_row_valid[5][0] = true;
  kobj.m_rows[5][0] = {5, 1, 0, shizuku::object_kind::PLAIN, false, 0, 0, 1, 0x1000};
  thread.base_object = 1;
  thread.call_stack.call_count = mock_kobj::MAX_DEPTH;
  thread.call_stack.current_call_object = 1;
#if defined(SHIZUKU_LEAN_LEDGER)
  thread.call_stack.call_top = 1;
  thread.ledger[0].object = 1;
#endif
  frame.r0 = 4;
  frame.r1 = 5;
  frame.r2 = 0;
  frame.r3 = 42;
  shizuku::native::call_method_in_place(kernel, thread, ctx, kobj);
  assert(frame.r0 == (uint32_t)shizuku::object_error::NO_STACK);
  assert(thread.call_stack.depth == 0);
  assert(thread.call_stack.call_count == mock_kobj::MAX_DEPTH);

  printf("  [%s] PASS: method_failures_ledger_invariant\n", tag);
}

template <typename Adapter>
void run_inplace_descriptor_failures_ledger_invariant(const char *tag) {
  mock_kernel<Adapter> kernel;
  mock_kobj kobj;
  typename mock_kernel<Adapter>::CONTEXT ctx{};
  typename mock_kernel<Adapter>::FRAME frame{};
  ctx.sp = &frame;

  typename mock_kernel<Adapter>::THREAD thread;
  typename mock_kernel<Adapter>::THREAD::call_ledger_entry ledger[16]{};
  thread.ledger = ledger;
  thread.ledger_capacity = 16;
  thread.base_object = 2;

  // 1. Bad Index
  frame.r0 = 33;
  frame.r1 = mock_kobj::METHOD_DESCRIPTOR_CAPACITY + 1;
  shizuku::native::call_descriptor_in_place(kernel, thread, ctx, kobj);
  assert(frame.r0 == (uint32_t)shizuku::object_error::BAD_DESCRIPTOR);
  assert(thread.call_stack.depth == 0);

  // 2. Odd Seq (stale)
  kobj.m_descriptors[2][1].seq = 1;
  frame.r0 = 33;
  frame.r1 = 1;
  shizuku::native::call_descriptor_in_place(kernel, thread, ctx, kobj);
  assert(frame.r0 == (uint32_t)shizuku::object_error::STALE_DESCRIPTOR);
  assert(thread.call_stack.depth == 0);

  // 3. Callee None
  kobj.m_descriptors[2][2].seq = 2;
  kobj.m_descriptors[2][2].callee = mock_kobj::DESCRIPTOR_NONE;
  frame.r0 = 33;
  frame.r1 = 2;
  shizuku::native::call_descriptor_in_place(kernel, thread, ctx, kobj);
  assert(frame.r0 == (uint32_t)shizuku::object_error::BAD_DESCRIPTOR);
  assert(thread.call_stack.depth == 0);

  // 4. Owner Generation Mismatch
  kobj.m_descriptors[2][3].seq = 2;
  kobj.m_descriptors[2][3].callee = 5;
  kobj.m_descriptors[2][3].owner_generation = 99;
  frame.r0 = 33;
  frame.r1 = 3;
  shizuku::native::call_descriptor_in_place(kernel, thread, ctx, kobj);
  assert(frame.r0 == (uint32_t)shizuku::object_error::STALE_DESCRIPTOR);
  assert(thread.call_stack.depth == 0);

  // 5. Target Version Mismatch
  kobj.m_descriptors[2][4].seq = 2;
  kobj.m_descriptors[2][4].callee = 5;
  kobj.m_descriptors[2][4].owner_generation = 0;
  kobj.m_descriptors[2][4].target_generation = 1;
  kobj.m_object_versions[5] = 2;
  frame.r0 = 33;
  frame.r1 = 4;
  shizuku::native::call_descriptor_in_place(kernel, thread, ctx, kobj);
  assert(frame.r0 == (uint32_t)shizuku::object_error::STALE_DESCRIPTOR);
  assert(thread.call_stack.depth == 0);

  printf("  [%s] PASS: descriptor_failures_ledger_invariant\n", tag);
}

template <typename Adapter>
void run_inplace_lean_push_pop(const char *tag) {
  typename mock_kernel<Adapter>::CONTEXT ctx{};
  typename mock_kernel<Adapter>::FRAME frame{};
  ctx.sp = &frame;
  ctx.control = Adapter::CONTROL_UNPRIV_PSP;
  ctx.region_base = 0x20000000u;
  ctx.region_limit = 0x20001000u;

  typename mock_kernel<Adapter>::THREAD thread;
  typename mock_kernel<Adapter>::THREAD::call_ledger_entry ledger[16]{};
  thread.ledger = ledger;
  thread.ledger_capacity = 16;
  thread.base_object = 1;

  frame.pc = 0x10004000u;

  bool pushed = shizuku::header_push_of<Adapter>(thread, ctx, frame, 1, 0);
  assert(pushed);
  assert(thread.call_stack.depth == 1);

#if defined(SHIZUKU_LEAN_LEDGER)
  assert(ledger[0].ret_pc == 0x10004000u);
  assert(ledger[0].ret_sp == reinterpret_cast<uintptr_t>(&frame) + sizeof(typename mock_kernel<Adapter>::FRAME));
  assert(ledger[0].ret_privileged == 0);
  assert(ledger[0].ret_region_base == 0x20000000u);
  assert(ledger[0].ret_region_limit == 0x20001000u);

  ledger[0].via = mock_kernel<Adapter>::LEDGER_CALL;
  ledger[0].unwind_floor = 0;
  thread.call_stack.call_count = 1;
  thread.call_stack.call_top = 1;

  typename mock_kernel<Adapter>::FRAME *restored_frame = nullptr;
  bool popped = shizuku::native::pop_in_place<mock_kernel<Adapter>>(thread, ctx, restored_frame, true, false);
  assert(popped);
  assert(thread.call_stack.depth == 0);
  assert(thread.call_stack.call_count == 0);
  assert(restored_frame != nullptr);
  assert(restored_frame->pc == 0x10004000u);
  assert((ctx.control & 1u) == 1); // Unprivileged restored
#endif

  printf("  [%s] PASS: lean_push_pop\n", tag);
}

template <typename Adapter>
void run_inplace_exit_validation(const char *tag) {
  mock_kernel<Adapter> kernel;
  typename mock_kernel<Adapter>::CONTEXT ctx{};

  struct stack_slot_t {
    typename mock_kernel<Adapter>::FRAME frame;
    typename Adapter::call_header_t header;
  } slots[2]{};

  // Initially active frame is slot 1
  ctx.sp = &slots[1].frame;

  typename mock_kernel<Adapter>::THREAD thread;
  typename mock_kernel<Adapter>::THREAD::call_ledger_entry ledger[16]{};
  thread.ledger = ledger;
  thread.ledger_capacity = 16;
  thread.base_object = 1;

  // Setup 2 nested call frames
  thread.call_stack.depth = 2;
  thread.call_stack.call_count = 2;
  thread.call_stack.call_top = 2;

  // Setup headers for non-LEAN
  slots[0].header.prev = 0;
  slots[0].header.pc = 0x10001000;
  slots[0].header.caller_object = 1;
  slots[0].header.privileged = 0;
  slots[1].header.prev = reinterpret_cast<uintptr_t>(&slots[0].header);
  slots[1].header.pc = 0x10002000;
  slots[1].header.caller_object = 2;
  slots[1].header.privileged = 0;
  thread.call_stack.top = reinterpret_cast<uintptr_t>(&slots[1].header);

  // Setup ledger entries (both non-LEAN and LEAN)
  ledger[0].object = 1;
  ledger[0].handler_object = 1;
  ledger[0].via = mock_kernel<Adapter>::LEDGER_CALL;
  ledger[0].unwind_floor = 0;

  ledger[1].object = 2;
  ledger[1].handler_object = 1;
  ledger[1].via = mock_kernel<Adapter>::LEDGER_CALL;
  ledger[1].unwind_floor = 1;

#if defined(SHIZUKU_LEAN_LEDGER)
  ledger[0].ret_pc = 0x10001000;
  ledger[0].ret_sp = reinterpret_cast<uintptr_t>(&slots[0].header);
  ledger[0].prev_call = 0;
  ledger[1].ret_pc = 0x10002000;
  ledger[1].ret_sp = reinterpret_cast<uintptr_t>(&slots[1].header);
  ledger[1].prev_call = 1;
#endif

  // 1. Test security check: illegal return target d=0 from depth=2 with floor=1
  slots[1].frame.r0 = (uint32_t)shizuku::object_api::EXIT_METHOD;
  slots[1].frame.r1 = 0; // target depth < floor
  slots[1].frame.r2 = 42;
  slots[1].frame.r3 = (uint32_t)shizuku::object_error::OK;

  auto st_rejected = shizuku::native::exit_in_place<mock_kernel<Adapter>, false>(kernel, thread, ctx);
  assert(st_rejected == shizuku::native::status::DONE);
  assert(slots[1].frame.r0 == (uint32_t)shizuku::object_error::UNWIND_REJECTED);
  assert(slots[1].frame.r1 == (uint32_t)shizuku::kernel_error::DEPTH_MISMATCH);
  assert(thread.call_stack.depth == 2);
  assert(thread.call_stack.call_count == 2);

  // 2. Test valid 1-step return: d=1 from depth=2 (floor=1)
  slots[1].frame.r0 = (uint32_t)shizuku::object_api::EXIT_METHOD;
  slots[1].frame.r1 = 1; // target depth == floor
  slots[1].frame.r2 = 42; // value
  slots[1].frame.r3 = (uint32_t)shizuku::object_error::OK;

  auto st1 = shizuku::native::exit_in_place<mock_kernel<Adapter>, false>(kernel, thread, ctx);
  assert(st1 == shizuku::native::status::DONE);
  assert(thread.call_stack.depth == 1);
  assert(thread.call_stack.call_count == 1);
  assert(ctx.sp->r0 == (uint32_t)shizuku::object_error::OK);
  assert(ctx.sp->r1 == 42);

  // 3. Test valid 1-step return to base: d=0 from depth=1 (floor=0)
  ctx.sp->r0 = (uint32_t)shizuku::object_api::EXIT_METHOD;
  ctx.sp->r1 = 0; // target depth == floor
  ctx.sp->r2 = 84; // value
  ctx.sp->r3 = (uint32_t)shizuku::object_error::OK;

  auto st0 = shizuku::native::exit_in_place<mock_kernel<Adapter>, false>(kernel, thread, ctx);
  assert(st0 == shizuku::native::status::DONE);
  assert(thread.call_stack.depth == 0);
  assert(thread.call_stack.call_count == 0);
  assert(ctx.sp->r0 == (uint32_t)shizuku::object_error::OK);
  assert(ctx.sp->r1 == 84);

  printf("  [%s] PASS: exit_validation\n", tag);
}

#endif // SHIZUKU_ARM_INPLACE_FIXTURE_HPP
