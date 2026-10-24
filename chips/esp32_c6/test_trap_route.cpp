#include <array>
#include <cassert>
#include <initializer_list>

#define SHIZUKU_C6_SKELETON_PERMITTED 1
#include "chips/esp32_c6/rv32_c6.hpp"
#include "arch/riscv/ecall_table.hpp"
#include "arch/riscv/trap_route.hpp"
using namespace shizuku::ports::riscv;
// ECALL を番号で直接振り分ける表の検査 (ecall_table_consistent)。
namespace {
using fn_t = int (*)();
int other_fn() { return 0; }
int api_fn() { return 1; }
constexpr auto native_ids = [](size_t id) { return id == 2 || id == 5; };
constexpr std::array<fn_t, 6> good_table{&other_fn, &other_fn, &api_fn, &other_fn, &other_fn, &api_fn};
constexpr std::array<fn_t, 6> zero_native{&api_fn, &other_fn, &api_fn, &other_fn, &other_fn, &api_fn};
constexpr std::array<fn_t, 6> missing_native{&other_fn, &other_fn, &api_fn, &other_fn, &other_fn, &other_fn};
constexpr std::array<fn_t, 6> extra_native{&other_fn, &api_fn, &api_fn, &other_fn, &other_fn, &api_fn};
constexpr std::array<fn_t, 5> short_table{&other_fn, &other_fn, &api_fn, &other_fn, &other_fn};
static_assert(ecall_table_consistent(good_table, 6, &other_fn, native_ids));
// 0 番が other でない (範囲外の行き先が壊れる)
static_assert(!ecall_table_consistent(zero_native, 6, &other_fn, native_ids));
// handler の番号が other のまま / handler でない番号が専用関数
static_assert(!ecall_table_consistent(missing_native, 6, &other_fn, native_ids));
static_assert(!ecall_table_consistent(extra_native, 6, &other_fn, native_ids));
// 表の大きさが番号の数と違う (asm の範囲外判定が合わない)
static_assert(!ecall_table_consistent(short_table, 6, &other_fn, native_ids));
}
using frame_t = shizuku::archs::rv32_c6::exception_frame_t;
int main() {
  volatile uint32_t depth = 0;
  unsigned ecalls = 0, irqs = 0, faults = 0;
  for (uint32_t cause : {8u, 11u, 2u, 0x8000000bu, 0x80000008u}) {
    frame_t f{}; f.mcause = cause; f.mepc = 0x1000; f.mtval = 0x123456;
    route_trap(f, depth, [&] { ++ecalls; assert(depth == 0); },
      [&] { ++irqs; assert(depth == 1); },
      [&] { ++faults; assert(depth == 0); });
    assert(f.mepc == (cause == 8 || cause == 11 ? 0x1004u : 0x1000u));
    assert(f.mcause == cause && f.mtval == 0x123456);
    assert(depth == 0);
  }
  assert(ecalls == 2 && irqs == 2 && faults == 1);
  // The direct ECALL path records the trapped caller's MPP, not handler privilege.
  using c6_arch_t = shizuku::archs::rv32_c6;
  c6_arch_t::context_t caller_context{};
  frame_t caller_frame{};
  caller_context.privileged = true;
  caller_frame.mstatus = RV32_MSTATUS_MPIE; // U caller (MPP=00)
  c6_arch_t::set_context_privilege_from_frame(caller_context, caller_frame);
  assert(!caller_context.privileged);
  caller_context.privileged = false;
  caller_frame.mstatus = RV32_MSTATUS_MPP | RV32_MSTATUS_MPIE; // M caller
  c6_arch_t::set_context_privilege_from_frame(caller_context, caller_frame);
  assert(caller_context.privileged);

  // U trap frames live on the trusted stack; call metadata must use the saved user SP.
  alignas(16) std::array<uint8_t, 2048> user_stack{};
  constexpr uintptr_t user_stack_base = 0x40820000u; // synthetic RV32 object stack range
  constexpr uintptr_t user_stack_limit = user_stack_base + 2048u;
  constexpr uintptr_t user_sp = user_stack_base + 1536u;
  auto *user_frame = reinterpret_cast<frame_t *>(user_stack.data() + 1024u);
  user_frame->reserved0 = static_cast<uint32_t>(user_sp);
  caller_context.sp = user_frame;
  caller_context.stack_limit = user_stack_base;
  caller_context.privileged = false;
  assert(reinterpret_cast<uintptr_t>(c6_arch_t::call_header_slot(caller_context)) == user_sp);
  assert(c6_arch_t::call_return_sp(caller_context) == user_sp);
  assert(c6_arch_t::call_header_has_room_in_user_stack(caller_context, user_stack_base,
                                                       user_stack_limit));
  user_frame->reserved0 = static_cast<uint32_t>(user_stack_base + 256u);
  assert(!c6_arch_t::call_header_has_room_in_user_stack(caller_context, user_stack_base,
                                                        user_stack_limit));
  user_frame->reserved0 = static_cast<uint32_t>(user_sp - 4u);
  assert(!c6_arch_t::call_header_has_room_in_user_stack(caller_context, user_stack_base,
                                                        user_stack_limit));

  // The protected ledger supplies the resume SP and privilege marker; metadata is rebuilt for U mret.
  c6_arch_t::set_unprivileged_resume_metadata(*user_frame, user_sp,
                                               0x40812000u, false);
  assert(user_frame->reserved0 == user_sp);
  assert(user_frame->reserved1 == 0x40812000u);
  assert(user_frame->mcause == RV32_TAG_U_ECALL);
  assert((user_frame->mstatus & RV32_MSTATUS_MPP) == 0u);
  c6_arch_t::set_unprivileged_resume_metadata(*user_frame, user_sp,
                                               0x40812000u, true);
  assert(user_frame->mcause == RV32_TAG_U_SCRUB);

  // call_resume_at restores the M SP needed by context_to_frame; U metadata then overrides it.
  alignas(16) std::array<uint8_t, 512> resume_memory{};
  auto *resume_storage = reinterpret_cast<frame_t *>(resume_memory.data() + 112u);
  const uintptr_t resume_sp = reinterpret_cast<uintptr_t>(resume_storage) + sizeof(frame_t);
  auto *m_resume = c6_arch_t::call_resume_at(caller_context, 0x12345678u,
      resume_sp, true, 0u, 0u, false);
  assert(m_resume == resume_storage);
  assert(m_resume->reserved0 == static_cast<uint32_t>(resume_sp));
  assert(m_resume->reserved1 == 0u);
  auto *u_resume = c6_arch_t::call_resume_at(caller_context, 0x42001000u,
      resume_sp, false, 0u, 0u, false);
  assert(u_resume == resume_storage && u_resume->reserved0 == static_cast<uint32_t>(resume_sp));
  c6_arch_t::set_unprivileged_resume_metadata(*u_resume, user_sp, 0x40812000u, false);
  assert(u_resume->reserved0 == user_sp);
  assert(u_resume->reserved1 == 0x40812000u && u_resume->mcause == RV32_TAG_U_ECALL);

  // M caller layout remains trap-frame-relative.
  alignas(16) std::array<uint8_t, 2048> m_stack{};
  const uintptr_t m_stack_base = reinterpret_cast<uintptr_t>(m_stack.data());
  auto *m_frame = reinterpret_cast<frame_t *>(m_stack_base + 1024u);
  caller_context.sp = m_frame;
  caller_context.stack_limit = m_stack_base;
  caller_context.privileged = true;
  const uintptr_t m_header = reinterpret_cast<uintptr_t>(m_frame) + sizeof(frame_t);
  assert(reinterpret_cast<uintptr_t>(c6_arch_t::call_header_slot(caller_context)) == m_header);
  assert(c6_arch_t::call_return_sp(caller_context) == m_header);
  frame_t outer{}; outer.mcause = 0x80000001;
  route_trap(outer, depth, [] { assert(false); }, [&] {
    assert(depth == 1);
    frame_t inner{}; inner.mcause = 0x80000002;
    route_trap(inner, depth, [] { assert(false); }, [&] { assert(depth == 2); }, [] { assert(false); });
    assert(depth == 1);
    inner.mcause = 11;
    route_trap(inner, depth, [&] { assert(depth == 1); }, [] { assert(false); }, [] { assert(false); });
    inner.mcause = 5;
    route_trap(inner, depth, [] { assert(false); }, [] { assert(false); }, [&] { assert(depth == 1); });
    assert(depth == 1);
  }, [] { assert(false); });
  assert(depth == 0);
}
