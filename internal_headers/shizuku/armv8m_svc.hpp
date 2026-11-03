#ifndef SHIZUKU_ARMV8M_SVC_HPP
#define SHIZUKU_ARMV8M_SVC_HPP
#include "shizuku/armv8m_resume.hpp"
#include "shizuku/service_profile.hpp"
#include "shizuku/svc_direct.hpp"
#include "shizuku/cost_probe.hpp"
#if defined(SHIZUKU_DIAG_FP)
#include "shizuku/diag_fp.hpp"
#endif
extern "C" void shizuku_svc_breakdown_probe(uint32_t, uint32_t, uint32_t);
#if defined(__ARM_ARCH_8M_MAIN__) || defined(SHIZUKU_RP2350)
extern "C" {
extern volatile bool shizuku_cmd8_mpu_diag_b_pending;
void shizuku_dump_mpu_diag_b(uintptr_t ret_sp, uintptr_t spoof_sp, uintptr_t parent_base, uintptr_t parent_top);
}
#endif

namespace shizuku::armv8m_svc_detail {

static_assert(cost_probe::api_count == svc_direct_table_size);

[[gnu::always_inline]] inline void probe(uint32_t point, uint32_t exc_return, uint32_t api) {
  if constexpr (ARCH::SVC_BREAKDOWN_ENABLED)
    shizuku_svc_breakdown_probe(point, exc_return, api);
}
[[gnu::always_inline]] inline void probe_return(uint32_t exc_return, uint32_t api) {
  probe(3, exc_return, api); probe(4, exc_return, api);
  probe(5, exc_return, api); probe(6, exc_return, api);
}

struct profile_span {
  [[gnu::always_inline]] profile_span() {
    if constexpr (service_profile::enabled) shizuku_service_handler_begin();
  }
  [[gnu::always_inline]] ~profile_span() {
    if constexpr (service_profile::enabled) shizuku_service_handler_end();
  }
};

// The trusted limit of the activation that issued this SVC (not supplied by PSP).
template <class Thread>
[[gnu::always_inline]] inline uint32_t stack_ceiling(const Thread &activation) {
  return activation.call_stack.depth != 0
      ? (uint32_t)activation.ledger[activation.call_stack.depth - 1].caller_sp()
      : (activation.stack_rlar & ~31u) + 32u;
}

// The original thread is still current and still owns its state until PendSV's
// CTX_SAVE finishes. No READY publication can race that save.
[[gnu::always_inline]] inline void return_pended(KERNEL::CONTEXT *context, uintptr_t psp,
                                                 uint32_t exc_return, uint32_t api) {
  if (context->sp != reinterpret_cast<ARCH::exception_frame_t *>(psp))
    asm volatile("msr psp, %0" :: "r"(context->sp) : "memory");
  ARCH::pend_context_switch();
  probe_return(exc_return, api);
}

// Windows, privilege and PSP are re-applied only when the body changed the frame.
[[gnu::always_inline]] inline void return_resumed(KERNEL::CONTEXT *context, KERNEL::THREAD &thread,
                                                  ARCH::core_num core, uint32_t exc_return, uint32_t api) {
  probe(3, exc_return, api);
  armv8m_prepare_resume(thread, core);
  probe(4, exc_return, api);
  // FPCA/SPSEL remain hardware-owned. Only nPRIV changes on an object switch.
  uint32_t next_control;
  asm volatile("mrs %0, control" : "=r"(next_control));
  next_control = (next_control & ~1u) | (context->control & 1u);
  probe(5, exc_return, api);
  // No ISB: the exception return that follows is a context synchronization event, and
  // CONTROL.nPRIV only matters to Thread mode (the handler stays privileged).
  asm volatile("msr psp, %0\nmsr psplim, %1\nmsr control, %2"
      :: "r"(context->sp), "r"(context->psplim), "r"(next_control) : "memory");
  probe(6, exc_return, api);
#if defined(__ARM_ARCH_8M_MAIN__) || defined(SHIZUKU_RP2350)
  if (__builtin_expect(shizuku_cmd8_mpu_diag_b_pending, false) && thread.call_stack.depth != 0) {
    shizuku_cmd8_mpu_diag_b_pending = false;
    const uintptr_t ret_sp = thread.ledger[thread.call_stack.depth - 1].caller_sp();
    const uintptr_t spoof_sp = ret_sp + 120;
    const uintptr_t p_base = thread.stack_rbar & ~uintptr_t(31);
    const uintptr_t p_top = (thread.stack_rlar & ~uintptr_t(31)) + 32;
    shizuku_dump_mpu_diag_b(ret_sp, spoof_sp, p_base, p_top);
  }
#endif
}

// General path: every number, every caller. Anything the direct bodies decline
// (thread switches, kernel-object callers, dedicated parent handlers, the
// service gate, unknown numbers) ends up here.
// core_num は trap 入口で一度だけ構築され以後引き回される。
[[gnu::noinline, gnu::noclone]] static SHIZUKU_SVC_RAM bool generic(uint32_t exc_return,
                                                                    ARCH::core_num core) {
  uintptr_t psp;
  uint32_t control;
  asm volatile("mrs %0, psp\nmrs %1, control" : "=r"(psp), "=r"(control) :: "memory");
  auto &kernel = kernel_instance;
  auto *context = kernel.current_thread_on(core.index()).context;
  const uint32_t api = reinterpret_cast<ARCH::exception_frame_t *>(psp)->r0;
  if constexpr (route_cost_probe_enabled)
    cost_probe::routes<true>::record(core.index(), cost_probe::route::generic, api);
  probe(0, exc_return, api);
  profile_span profile;
  context->sp = reinterpret_cast<ARCH::exception_frame_t *>(psp);
  context->exc_return = exc_return;
  context->control = control;
  context->stack_ceiling = stack_ceiling(kernel.current_thread_on(core.index()));
  const uintptr_t pc = context->sp->pc;
  const auto depth = kernel.current_thread_on(core.index()).call_stack.depth;
  ARCH::s_in_svc[core.index()] = true;
  probe(1, exc_return, api);
  kernel.svc_dispatch(context, core);
  probe(2, exc_return, api);
  ARCH::s_in_svc[core.index()] = false;

  if (ARCH::s_deferred_svc[core.index()] || ARCH::s_resume_scrub[core.index()]) {
    return_pended(context, psp, exc_return, api);
    return false;
  }
  auto &thread = kernel.current_thread_on(core.index());
  if ((uintptr_t)context->sp == psp && context->sp->pc == pc &&
      context->control == control && thread.call_stack.depth == depth) {
    probe_return(exc_return, api);
    return false;
  }
  return_resumed(context, thread, core, exc_return, api);
  return true;
}

// Direct path (the counterpart of the C6 ecall_direct): the body of one API is
// expanded here and runs on the trapped frame. It skips svc_dispatch, the s_in_svc
// bookkeeping and the full context write, and re-applies the hardware windows only
// when the body actually moved the caller (pc / sp / control / depth).
// The context fields the in-place bodies read are written: sp, exc_return, control and,
// for calls, the activation's stack ceiling.
// A body that declines (FORWARD, e.g. the outermost method returning) and a caller that
// is not on the native route (kernel object itself, dedicated parent handler, the
// service gate) take the general path; the bodies change nothing before declining.
template <svc_direct_kind Kind>
[[gnu::always_inline]] inline bool direct(uint32_t exc_return, uintptr_t psp, ARCH::core_num core) {
  constexpr bool is_call = Kind == svc_direct_kind::CALL_METHOD ||
                           Kind == svc_direct_kind::CALL_DESCRIPTOR;
  constexpr bool scrubs = Kind == svc_direct_kind::EXIT_METHOD ||
                          Kind == svc_direct_kind::FORWARD_CHILD_EXIT;
  constexpr bool moves_caller = is_call || scrubs;
  auto &kernel = kernel_instance;
  // core_num は trap 入口で一度だけ構築され以後引き回される。
  auto &thread = kernel.current_thread_on(core.index());
  auto *frame = reinterpret_cast<ARCH::exception_frame_t *>(psp);
  const uintptr_t pc = frame->pc;
  if (pc == reinterpret_cast<uintptr_t>(&shizuku_v2_service_resume) ||
      !kernel.svc_native_route_of(thread))
    return generic(exc_return, core);
  uint32_t control;
  asm volatile("mrs %0, control" : "=r"(control));
  const uint32_t api = frame->r0;
  auto *context = thread.context;
  native::status result;
  {
    profile_span profile;
    probe(0, exc_return, api);
    context->sp = frame;
    context->exc_return = exc_return;
    context->control = control;
    if constexpr (is_call) context->stack_ceiling = stack_ceiling(thread);
    [[maybe_unused]] const auto depth = thread.call_stack.depth;
    probe(1, exc_return, api);
    if constexpr (Kind == svc_direct_kind::CALL_METHOD) {
      native::call_method_in_place(kernel, thread, *context, kernel_object_instance, core);
      result = native::status::DONE;
    } else if constexpr (Kind == svc_direct_kind::CALL_DESCRIPTOR) {
      native::call_descriptor_in_place(kernel, thread, *context, kernel_object_instance, core);
      result = native::status::DONE;
    } else if constexpr (Kind == svc_direct_kind::EXIT_METHOD) {
      result = native::exit_in_place<KERNEL, KERNEL_OBJECT, false>(
          kernel, kernel_object_instance, thread, *context, core);
    } else if constexpr (Kind == svc_direct_kind::FORWARD_CHILD_EXIT) {
      result = native::exit_in_place<KERNEL, KERNEL_OBJECT, true>(
          kernel, kernel_object_instance, thread, *context, core);
    } else if constexpr (Kind == svc_direct_kind::GET_CURRENT) {
      result = kernel.svc_get_current(context, core) ? native::status::DONE : native::status::FORWARD;
    } else if constexpr (Kind == svc_direct_kind::GET_CALLER) {
      result = kernel.svc_get_caller(context, core) ? native::status::DONE : native::status::FORWARD;
    } else {
      static_assert(Kind == svc_direct_kind::SET_BUDGET);
      result = kernel.svc_set_budget(context, core) ? native::status::DONE : native::status::FORWARD;
    }
    probe(2, exc_return, api);
    if (result == native::status::DONE) {
      if constexpr (route_cost_probe_enabled)
        cost_probe::routes<true>::record(core.index(), cost_probe::route::direct, api);
      if constexpr (!moves_caller) {
        // Only the reply registers changed: nothing to re-apply.
        probe_return(exc_return, api);
        return false;
      } else {
        if constexpr (scrubs) {
          if (ARCH::s_resume_scrub[core.index()]) {
            return_pended(context, psp, exc_return, api);
            return false;
          }
        }
        if ((uintptr_t)context->sp == psp && frame->pc == pc &&
            context->control == control && thread.call_stack.depth == depth) {
          probe_return(exc_return, api);
          return false;
        }
        return_resumed(context, thread, core, exc_return, api);
#if defined(SHIZUKU_DIAG_FP)
        if constexpr (is_call) {
          shizuku::diag_fp::record_point(shizuku::diag_fp::point::K2);
        } else if constexpr (Kind == svc_direct_kind::EXIT_METHOD) {
          shizuku::diag_fp::record_point(shizuku::diag_fp::point::K4);
          shizuku::diag_fp::commit_round();
        }
#endif
        return true;
      }
    }
  }
  // The ledger was partly unwound: the same stop the general path reaches.
  if (result == native::status::CORRUPT) KERNEL::BOARD::panic("call ledger count underflow");
  return generic(exc_return, core);
}

// One function per number. Each is an ordinary C++ exception function that the
// handler below jumps to with its own EXC_RETURN (passed as an argument, so the
// result does not depend on the compiler turning the call into a tail call).
[[gnu::noinline, gnu::noclone]] static SHIZUKU_SVC_RAM void call_method(uint32_t e, uintptr_t p, ARCH::core_num core) {
  direct<svc_direct_kind::CALL_METHOD>(e, p, core);
}
[[gnu::noinline, gnu::noclone]] static SHIZUKU_SVC_RAM void call_descriptor(uint32_t e, uintptr_t p, ARCH::core_num core) {
  direct<svc_direct_kind::CALL_DESCRIPTOR>(e, p, core);
}
extern "C" [[gnu::noinline, gnu::noclone]] SHIZUKU_SVC_RAM bool shizuku_armv8m_exit_method_body(uint32_t e, uintptr_t p, ARCH::core_num core) {
  return direct<svc_direct_kind::EXIT_METHOD>(e, p, core);
}
extern "C" [[gnu::noinline, gnu::noclone]] SHIZUKU_SVC_RAM bool shizuku_armv8m_forward_child_exit_body(uint32_t e, uintptr_t p, ARCH::core_num core) {
  return direct<svc_direct_kind::FORWARD_CHILD_EXIT>(e, p, core);
}
extern "C" [[gnu::noinline, gnu::noclone]] SHIZUKU_SVC_RAM bool shizuku_armv8m_generic_body(uint32_t e, uintptr_t, ARCH::core_num core) {
  return generic(e, core);
}
[[gnu::noinline, gnu::noclone]] static SHIZUKU_SVC_RAM void get_current(uint32_t e, uintptr_t p, ARCH::core_num core) {
  direct<svc_direct_kind::GET_CURRENT>(e, p, core);
}
[[gnu::noinline, gnu::noclone]] static SHIZUKU_SVC_RAM void get_caller(uint32_t e, uintptr_t p, ARCH::core_num core) {
  direct<svc_direct_kind::GET_CALLER>(e, p, core);
}
[[gnu::noinline, gnu::noclone]] static SHIZUKU_SVC_RAM void set_budget(uint32_t e, uintptr_t p, ARCH::core_num core) {
  direct<svc_direct_kind::SET_BUDGET>(e, p, core);
}

} // namespace shizuku::armv8m_svc_detail

// Thin asm entries defined in arch/armv8m/armv8m_ctx.S. They call the C++ bodies
// above, and after the compiler epilogue restores r4-r11, they zero r4-r11 and
// execute exception return (bx r12) when unwound to caller, so callee values never
// leak to caller.
extern "C" SHIZUKU_SVC_RAM void shizuku_armv8m_exit_entry(
    uint32_t exc_return, uintptr_t psp, shizuku::ARCH::core_num core);
extern "C" SHIZUKU_SVC_RAM void shizuku_armv8m_forward_child_exit_entry(
    uint32_t exc_return, uintptr_t psp, shizuku::ARCH::core_num core);
extern "C" SHIZUKU_SVC_RAM void shizuku_armv8m_generic_entry(
    uint32_t exc_return, uintptr_t psp, shizuku::ARCH::core_num core);

// The API number is in r0 of the stacked frame (`svc 0` carries no immediate), so the
// dispatch is one load and a jump table on the number; every path below is a tail call.
// The cases are written on the numbers (a table lookup first would add a flash read and a
// second range check); the asserts bind each of them to svc_direct_of, which the
// svc_attributes consistency check in svc_direct.hpp covers for all 34 numbers.
static_assert(shizuku::svc_direct_of((uintptr_t)shizuku::object_api::CALL_METHOD) == shizuku::svc_direct_kind::CALL_METHOD);
static_assert(shizuku::svc_direct_of((uintptr_t)shizuku::object_api::CALL_METHOD_DESCRIPTOR) == shizuku::svc_direct_kind::CALL_DESCRIPTOR);
static_assert(shizuku::svc_direct_of((uintptr_t)shizuku::object_api::EXIT_METHOD) == shizuku::svc_direct_kind::EXIT_METHOD);
static_assert(shizuku::svc_direct_of((uintptr_t)shizuku::object_api::FORWARD_CHILD_EXIT) == shizuku::svc_direct_kind::FORWARD_CHILD_EXIT);
static_assert(shizuku::svc_direct_of((uintptr_t)shizuku::object_api::GET_CURRENT_OBJECT) == shizuku::svc_direct_kind::GET_CURRENT);
static_assert(shizuku::svc_direct_of((uintptr_t)shizuku::object_api::GET_CALLER_OBJECT) == shizuku::svc_direct_kind::GET_CALLER);
static_assert(shizuku::svc_direct_of((uintptr_t)shizuku::object_api::SET_BUDGET) == shizuku::svc_direct_kind::SET_BUDGET);

// A normal C++ exception function. The compiler preserves its own MSP frame and
// incoming return address. __builtin_return_address reads that original address
// even when the prologue has saved LR; no naked asm-to-C call boundary is used.
extern "C" SHIZUKU_SVC_RAM void shizuku_armv8m_svc_handler() {
#if defined(SHIZUKU_DIAG_FP)
  const uint32_t diag_entry_ts = shizuku::diag_fp::read_cyccnt();
#endif
  using namespace shizuku;
  using namespace shizuku::armv8m_svc_detail;
  const ARCH::core_num core;
  const uint32_t exc_return = (uint32_t)(uintptr_t)__builtin_return_address(0);
  uintptr_t psp;
  asm volatile("mrs %0, psp" : "=r"(psp));
  switch (reinterpret_cast<ARCH::exception_frame_t *>(psp)->r0) {
  case (uint32_t)object_api::CALL_METHOD:
#if defined(SHIZUKU_DIAG_FP)
    shizuku::diag_fp::on_k1(diag_entry_ts);
#endif
    return call_method(exc_return, psp, core);
  case (uint32_t)object_api::CALL_METHOD_DESCRIPTOR:
#if defined(SHIZUKU_DIAG_FP)
    shizuku::diag_fp::on_k1(diag_entry_ts);
#endif
    return call_descriptor(exc_return, psp, core);
  case (uint32_t)object_api::EXIT_METHOD:
#if defined(SHIZUKU_DIAG_FP)
    shizuku::diag_fp::on_k3(diag_entry_ts);
#endif
    return shizuku_armv8m_exit_entry(exc_return, psp, core);
  case (uint32_t)object_api::FORWARD_CHILD_EXIT: return shizuku_armv8m_forward_child_exit_entry(exc_return, psp, core);
  case (uint32_t)object_api::GET_CURRENT_OBJECT: return get_current(exc_return, psp, core);
  case (uint32_t)object_api::GET_CALLER_OBJECT: return get_caller(exc_return, psp, core);
  case (uint32_t)object_api::SET_BUDGET: return set_budget(exc_return, psp, core);
  default: return shizuku_armv8m_generic_entry(exc_return, psp, core);
  }
}

// SDK versions use either spelling in their stored vector table. Strong Thumb
// aliases put the ordinary C++ handler itself in that table as well as in the
// RAM vector installed by board::init; these aliases add no executable veneer.
asm(".global isr_svcall\n"
    ".thumb_set isr_svcall, shizuku_armv8m_svc_handler\n"
    ".global SVC_Handler\n"
    ".thumb_set SVC_Handler, shizuku_armv8m_svc_handler\n");
#endif
