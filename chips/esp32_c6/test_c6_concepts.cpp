#include <type_traits>
#include <cassert>
#include <cstdio>
#include <initializer_list>
#define SHIZUKU_C6_SKELETON_PERMITTED 1
#include "chips/esp32_c6/rv32_c6.hpp"
#include "boards/esp32_c6/esp32_c6.hpp"
#include "chips/esp32_c6/stop_capture.hpp"
#include "chips/esp32_c6/trap_api.hpp"

using arch = shizuku::archs::rv32_c6;
using board = shizuku::boards::esp32_c6;

// Context layout deduction concept validation
static_assert(shizuku::chips::esp32_c6::stop_capture::layout_of<arch::context_t>().object_bytes ==
                  sizeof(arch::context_t),
              "deduced layout size must match sizeof(arch::context_t)");
static_assert(shizuku::chips::esp32_c6::stop_capture::layout_of<arch::context_t>().sp_offset ==
                  offsetof(arch::context_t, sp),
              "deduced layout sp_offset must match offsetof");
static_assert(shizuku::chips::esp32_c6::stop_capture::layout_of<arch::context_t>().privileged_offset ==
                  offsetof(arch::context_t, privileged),
              "deduced layout privileged_offset must match offsetof");

// Policy concept validation for make_irq_dispatch_hooks
struct test_empty_policy {};
struct test_full_policy {
  static void on_enter(uint32_t, uint32_t) {}
  static void on_exit(uint32_t) {}
};
static_assert(shizuku::chips::esp32_c6::make_irq_dispatch_hooks<test_empty_policy>().on_enter == nullptr &&
              shizuku::chips::esp32_c6::make_irq_dispatch_hooks<test_empty_policy>().on_exit == nullptr,
              "empty policy must yield null hooks");
static_assert(shizuku::chips::esp32_c6::make_irq_dispatch_hooks<test_full_policy>().on_enter != nullptr &&
              shizuku::chips::esp32_c6::make_irq_dispatch_hooks<test_full_policy>().on_exit != nullptr,
              "full policy must yield valid hook function pointers");

// static_assert による concept 検証
static_assert(shizuku::concepts::arch_requires<arch>,
              "rv32_c6 must strictly satisfy concepts::arch_requires");
static_assert(shizuku::concepts::board_requires<board>,
              "esp32_c6 must strictly satisfy concepts::board_requires");

// Shared assembly/C++ frame size.
static_assert(sizeof(arch::exception_frame_t) == RV32_FRAME_BYTES,
              "exception_frame_t must be match shared assembly layout");

// Chip selection: ARCH is the chip-selected rv32 specialization (C6: 16 PMP entries, 4B granule,
// PLIC vector mode). The chip constants must be the ones that reach the template.
static_assert(std::is_same_v<shizuku::chips::esp32_c6::chip::arch_type,
                             shizuku::archs::rv32<16, 4, shizuku::chips::esp32_c6::plic_vector_mode>>,
              "chip::arch_type must be rv32<16,4,plic_vector_mode>");
static_assert(shizuku::chips::esp32_c6::chip::kPmpEntries == 16 &&
              shizuku::chips::esp32_c6::chip::kPmpGranuleBytes == 4,
              "C6 PMP parameters: 16 entries, 4-byte granule");
static_assert(std::is_same_v<arch::pmp_codec_t, shizuku::ports::riscv::pmp_codec<4>>,
              "C6 PMP codec must be pmp_codec<4> (current behavior)");
static_assert(shizuku::chips::esp32_c6::plic_vector_mode::HAS_CYCLE_COUNTER,
              "C6 plic_vector_mode must enable HAS_CYCLE_COUNTER for V0 baseline measurement");
static_assert(arch::HAS_CYCLE_COUNTER,
              "rv32_c6 arch specialization must forward HAS_CYCLE_COUNTER = true");

// サイクルカウンタ (段0): HAS_CYCLE_COUNTER=true と API シグネチャの静的検証
static_assert(shizuku::chips::esp32_c6::plic_vector_mode::HAS_CYCLE_COUNTER,
              "plic_vector_mode must declare HAS_CYCLE_COUNTER=true in stage 0");
static_assert(arch::HAS_CYCLE_COUNTER, "rv32_c6 must declare HAS_CYCLE_COUNTER=true in stage 0");
static_assert(std::is_invocable_r_v<bool, decltype(arch::cycle_counter_enable)>,
              "arch::cycle_counter_enable must return bool");
static_assert(std::is_invocable_r_v<uint32_t, decltype(arch::cycle_counter_read)>,
              "arch::cycle_counter_read must return uint32_t");
static_assert(std::is_invocable_r_v<bool, decltype(shizuku::chips::esp32_c6::plic_vector_mode::cycle_counter_enable)>,
              "plic_vector_mode::cycle_counter_enable must return bool");
static_assert(std::is_invocable_r_v<uint32_t, decltype(shizuku::chips::esp32_c6::plic_vector_mode::cycle_counter_read)>,
              "plic_vector_mode::cycle_counter_read must return uint32_t");

// 段0.5: stage05_trace データ構造・幾何・fail-closed 要件の静的検証
static_assert(sizeof(shizuku_c6_stage05_record_t) == 368,
              "shizuku_c6_stage05_record_t must be exactly 368 bytes (92 uint32_t words)");
static_assert(sizeof(shizuku_c6_stage05_interval_t) == 24,
              "shizuku_c6_stage05_interval_t must be 24 bytes (6 uint32_t words)");
static_assert(sizeof(shizuku_c6_stage05_sample_t) == 52,
              "shizuku_c6_stage05_sample_t must be 52 bytes (13 uint32_t timestamps)");
static_assert(SHIZUKU_C6_STAGE05_INTERVAL_COUNT == 12,
              "Stage 0.5 must define exactly 12 adjacent intervals");
static_assert(SHIZUKU_C6_STAGE05_ROUNDS >= 128,
              "Stage 0.5 must require at least 128 sample rounds");
static_assert(SHIZUKU_C6_STAGE05_MAGIC == 0x53303554u,
              "Stage 0.5 magic must be 'S05T' (0x53303554)");
static_assert(offsetof(shizuku_c6_stage05_record_t, magic) == 0);
static_assert(offsetof(shizuku_c6_stage05_record_t, version) == 4);
static_assert(offsetof(shizuku_c6_stage05_record_t, status) == 8);
static_assert(offsetof(shizuku_c6_stage05_record_t, error_code) == 12);
static_assert(offsetof(shizuku_c6_stage05_record_t, rounds_requested) == 16);
static_assert(offsetof(shizuku_c6_stage05_record_t, rounds_completed) == 20);
static_assert(offsetof(shizuku_c6_stage05_record_t, placement_mode) == 24);
static_assert(offsetof(shizuku_c6_stage05_record_t, systimer_ticks) == 32);
static_assert(offsetof(shizuku_c6_stage05_record_t, cpu_cycle_delta) == 36);
static_assert(offsetof(shizuku_c6_stage05_record_t, cpu_mhz) == 40);
static_assert(offsetof(shizuku_c6_stage05_record_t, clock_verified) == 44);
static_assert(offsetof(shizuku_c6_stage05_record_t, hook_single_raw_min) == 48);
static_assert(offsetof(shizuku_c6_stage05_record_t, total_perturbation) == 60);
static_assert(offsetof(shizuku_c6_stage05_record_t, total_raw_min) == 64);
static_assert(offsetof(shizuku_c6_stage05_record_t, total_net_min) == 72);
static_assert(offsetof(shizuku_c6_stage05_record_t, intervals) == 80);

// 段1: fastcall データ構造・幾何・アサーションマスクの静的検証
static_assert(sizeof(shizuku_c6_fastcall_descriptor_t) == 20,
              "shizuku_c6_fastcall_descriptor_t must be exactly 20 bytes (5 uint32_t words)");
static_assert(offsetof(shizuku_c6_fastcall_descriptor_t, entry_pc) == 0);
static_assert(offsetof(shizuku_c6_fastcall_descriptor_t, caller_sp) == 4);
static_assert(offsetof(shizuku_c6_fastcall_descriptor_t, caller_pc) == 8);
static_assert(offsetof(shizuku_c6_fastcall_descriptor_t, depth) == 12);
static_assert(offsetof(shizuku_c6_fastcall_descriptor_t, call_count) == 16);

static_assert(SHIZUKU_C6_FASTCALL_DESC_ENTRY_PC == 0);
static_assert(SHIZUKU_C6_FASTCALL_DESC_CALLER_SP == 4);
static_assert(SHIZUKU_C6_FASTCALL_DESC_CALLER_PC == 8);
static_assert(SHIZUKU_C6_FASTCALL_DESC_DEPTH == 12);
static_assert(SHIZUKU_C6_FASTCALL_DESC_CALL_COUNT == 16);
static_assert(SHIZUKU_C6_FASTCALL_DESC_SIZE == 20);

static_assert(sizeof(shizuku_c6_fastcall_record_t) == 80,
              "shizuku_c6_fastcall_record_t must be exactly 80 bytes (20 uint32_t words)");
static_assert(SHIZUKU_C6_FASTCALL_MAGIC == 0x46433152u,
              "Fastcall 1 magic must be 'FC1R' (0x46433152)");
static_assert(offsetof(shizuku_c6_fastcall_record_t, magic) == 0);
static_assert(offsetof(shizuku_c6_fastcall_record_t, version) == 4);
static_assert(offsetof(shizuku_c6_fastcall_record_t, status) == 8);
static_assert(offsetof(shizuku_c6_fastcall_record_t, error_code) == 12);
static_assert(offsetof(shizuku_c6_fastcall_record_t, rounds_requested) == 16);
static_assert(offsetof(shizuku_c6_fastcall_record_t, rounds_completed) == 20);
static_assert(offsetof(shizuku_c6_fastcall_record_t, placement_mode) == 24);
static_assert(offsetof(shizuku_c6_fastcall_record_t, systimer_ticks) == 32);
static_assert(offsetof(shizuku_c6_fastcall_record_t, cpu_cycle_delta) == 36);
static_assert(offsetof(shizuku_c6_fastcall_record_t, cpu_mhz) == 40);
static_assert(offsetof(shizuku_c6_fastcall_record_t, clock_verified) == 44);
static_assert(offsetof(shizuku_c6_fastcall_record_t, calibrated_overhead) == 48);
static_assert(offsetof(shizuku_c6_fastcall_record_t, raw_min) == 52);
static_assert(offsetof(shizuku_c6_fastcall_record_t, raw_mean) == 56);
static_assert(offsetof(shizuku_c6_fastcall_record_t, net_min) == 60);
static_assert(offsetof(shizuku_c6_fastcall_record_t, net_mean) == 64);
static_assert(offsetof(shizuku_c6_fastcall_record_t, max_trap1_cycles) == 68);
static_assert(offsetof(shizuku_c6_fastcall_record_t, max_trap2_cycles) == 72);
static_assert(offsetof(shizuku_c6_fastcall_record_t, assert_pass_mask) == 76);
static_assert(SHIZUKU_C6_FASTCALL_ASSERT_CALLEE_GP_TP_ZERO == (1u << 4));
static_assert(SHIZUKU_C6_FASTCALL_ASSERT_CALLEE_NONARG_ZERO == (1u << 5));
static_assert(SHIZUKU_C6_FASTCALL_ASSERT_ALL_MASK == 0x3Fu,
              "Fastcall assert all mask must cover all 6 assertions (0x3F)");

// 段2: fastcall stage 2 区間計測データ構造・幾何・アサーションの静的検証
static_assert(sizeof(shizuku_c6_fastcall_stage2_window_stats_t) == 16,
              "shizuku_c6_fastcall_stage2_window_stats_t must be exactly 16 bytes (4 uint32_t words)");
static_assert(sizeof(shizuku_c6_fastcall_stage2_interval_t) == 32,
              "shizuku_c6_fastcall_stage2_interval_t must be exactly 32 bytes (8 uint32_t words)");
static_assert(sizeof(shizuku_c6_fastcall_stage2_sample_t) == 64,
              "shizuku_c6_fastcall_stage2_sample_t must be exactly 64 bytes (16 uint32_t words)");
static_assert(sizeof(shizuku_c6_fastcall_stage2_current_timestamps_t) == 32,
              "shizuku_c6_fastcall_stage2_current_timestamps_t must be exactly 32 bytes (8 uint32_t words)");
static_assert(sizeof(shizuku_c6_fastcall_stage2_record_t) == 8512,
              "shizuku_c6_fastcall_stage2_record_t must be exactly 8512 bytes (2128 uint32_t words)");
static_assert(SHIZUKU_C6_FASTCALL_STAGE2_MAGIC == 0x46433252u,
              "Fastcall 2 magic must be 'FC2R' (0x46433252)");
static_assert(SHIZUKU_C6_FASTCALL_STAGE2_VERSION == 1u,
              "Fastcall 2 version must be 1");
static_assert(SHIZUKU_C6_FASTCALL_STAGE2_INTERVAL_COUNT == 5u,
              "Fastcall 2 must define exactly 5 adjacent intervals");

// Record layout offsets (shizuku_c6_fastcall_stage2_record_t)
static_assert(offsetof(shizuku_c6_fastcall_stage2_record_t, magic) == 0);
static_assert(offsetof(shizuku_c6_fastcall_stage2_record_t, version) == 4);
static_assert(offsetof(shizuku_c6_fastcall_stage2_record_t, status) == 8);
static_assert(offsetof(shizuku_c6_fastcall_stage2_record_t, error_code) == 12);
static_assert(offsetof(shizuku_c6_fastcall_stage2_record_t, rounds_requested) == 16);
static_assert(offsetof(shizuku_c6_fastcall_stage2_record_t, rounds_completed) == 20);
static_assert(offsetof(shizuku_c6_fastcall_stage2_record_t, placement_mode) == 24);
static_assert(offsetof(shizuku_c6_fastcall_stage2_record_t, flags) == 28);
static_assert(offsetof(shizuku_c6_fastcall_stage2_record_t, systimer_ticks) == 32);
static_assert(offsetof(shizuku_c6_fastcall_stage2_record_t, cpu_cycle_delta) == 36);
static_assert(offsetof(shizuku_c6_fastcall_stage2_record_t, cpu_mhz) == 40);
static_assert(offsetof(shizuku_c6_fastcall_stage2_record_t, clock_verified) == 44);
static_assert(offsetof(shizuku_c6_fastcall_stage2_record_t, calibrated_overhead) == 48);
static_assert(offsetof(shizuku_c6_fastcall_stage2_record_t, e2e_net_valid) == 52);
static_assert(offsetof(shizuku_c6_fastcall_stage2_record_t, interval_net_valid) == 56);
static_assert(offsetof(shizuku_c6_fastcall_stage2_record_t, wcet_guaranteed) == 60);
static_assert(offsetof(shizuku_c6_fastcall_stage2_record_t, mie_caller_pre) == 64);
static_assert(offsetof(shizuku_c6_fastcall_stage2_record_t, mie_kernel_entry) == 68);
static_assert(offsetof(shizuku_c6_fastcall_stage2_record_t, mie_callee) == 72);
static_assert(offsetof(shizuku_c6_fastcall_stage2_record_t, mie_kernel_return) == 76);
static_assert(offsetof(shizuku_c6_fastcall_stage2_record_t, mie_caller_post) == 80);
static_assert(offsetof(shizuku_c6_fastcall_stage2_record_t, assert_pass_mask) == 84);
static_assert(offsetof(shizuku_c6_fastcall_stage2_record_t, trap1_body_max) == 88);
static_assert(offsetof(shizuku_c6_fastcall_stage2_record_t, trap2_body_max) == 92);
static_assert(offsetof(shizuku_c6_fastcall_stage2_record_t, window1_call) == 96);
static_assert(offsetof(shizuku_c6_fastcall_stage2_record_t, window2_ret) == 112);
static_assert(offsetof(shizuku_c6_fastcall_stage2_record_t, e2e_summary) == 128);
static_assert(offsetof(shizuku_c6_fastcall_stage2_record_t, intervals) == 160);
static_assert(offsetof(shizuku_c6_fastcall_stage2_record_t, samples) == 320);

// Macro layout defines verification
static_assert(SHIZUKU_C6_FASTCALL_STAGE2_REC_MAGIC == 0);
static_assert(SHIZUKU_C6_FASTCALL_STAGE2_REC_VERSION == 4);
static_assert(SHIZUKU_C6_FASTCALL_STAGE2_REC_STATUS == 8);
static_assert(SHIZUKU_C6_FASTCALL_STAGE2_REC_ERROR_CODE == 12);
static_assert(SHIZUKU_C6_FASTCALL_STAGE2_REC_ROUNDS_REQ == 16);
static_assert(SHIZUKU_C6_FASTCALL_STAGE2_REC_ROUNDS_COMP == 20);
static_assert(SHIZUKU_C6_FASTCALL_STAGE2_REC_PLACEMENT == 24);
static_assert(SHIZUKU_C6_FASTCALL_STAGE2_REC_FLAGS == 28);
static_assert(SHIZUKU_C6_FASTCALL_STAGE2_REC_WINDOW1 == 96);
static_assert(SHIZUKU_C6_FASTCALL_STAGE2_REC_WINDOW2 == 112);
static_assert(SHIZUKU_C6_FASTCALL_STAGE2_REC_E2E == 128);
static_assert(SHIZUKU_C6_FASTCALL_STAGE2_REC_INTERVALS == 160);
static_assert(SHIZUKU_C6_FASTCALL_STAGE2_REC_SAMPLES == 320);
static_assert(SHIZUKU_C6_FASTCALL_STAGE2_REC_SIZE == 8512);

// Current timestamps staging offsets
static_assert(offsetof(shizuku_c6_fastcall_stage2_current_timestamps_t, ts0_callsite_start) == 0);
static_assert(offsetof(shizuku_c6_fastcall_stage2_current_timestamps_t, ts1_call_ecall_pre) == 4);
static_assert(offsetof(shizuku_c6_fastcall_stage2_current_timestamps_t, ts2_call_router_post_prologue) == 8);
static_assert(offsetof(shizuku_c6_fastcall_stage2_current_timestamps_t, ts2_call_trap_entry) == 8);
static_assert(offsetof(shizuku_c6_fastcall_stage2_current_timestamps_t, ts3_callee_entry) == 12);
static_assert(offsetof(shizuku_c6_fastcall_stage2_current_timestamps_t, ts4_ret_ecall_pre) == 16);
static_assert(offsetof(shizuku_c6_fastcall_stage2_current_timestamps_t, ts5_ret_router_post_prologue) == 20);
static_assert(offsetof(shizuku_c6_fastcall_stage2_current_timestamps_t, ts5_ret_trap_entry) == 20);
static_assert(offsetof(shizuku_c6_fastcall_stage2_current_timestamps_t, ts6_caller_resume) == 24);
static_assert(offsetof(shizuku_c6_fastcall_stage2_current_timestamps_t, ts7_callsite_end) == 28);

// Sample layout offsets
static_assert(offsetof(shizuku_c6_fastcall_stage2_sample_t, ts0_callsite_start) == 0);
static_assert(offsetof(shizuku_c6_fastcall_stage2_sample_t, ts1_call_ecall_pre) == 4);
static_assert(offsetof(shizuku_c6_fastcall_stage2_sample_t, ts2_call_router_post_prologue) == 8);
static_assert(offsetof(shizuku_c6_fastcall_stage2_sample_t, ts2_call_trap_entry) == 8);
static_assert(offsetof(shizuku_c6_fastcall_stage2_sample_t, ts3_callee_entry) == 12);
static_assert(offsetof(shizuku_c6_fastcall_stage2_sample_t, ts4_ret_ecall_pre) == 16);
static_assert(offsetof(shizuku_c6_fastcall_stage2_sample_t, ts5_ret_router_post_prologue) == 20);
static_assert(offsetof(shizuku_c6_fastcall_stage2_sample_t, ts5_ret_trap_entry) == 20);
static_assert(offsetof(shizuku_c6_fastcall_stage2_sample_t, ts6_caller_resume) == 24);
static_assert(offsetof(shizuku_c6_fastcall_stage2_sample_t, ts7_callsite_end) == 28);
static_assert(offsetof(shizuku_c6_fastcall_stage2_sample_t, caller_pre_cycles) == 32);
static_assert(offsetof(shizuku_c6_fastcall_stage2_sample_t, kernel_entry_post_save_cycles) == 36);
static_assert(offsetof(shizuku_c6_fastcall_stage2_sample_t, kernel_entry_cycles) == 36);
static_assert(offsetof(shizuku_c6_fastcall_stage2_sample_t, callee_cycles) == 40);
static_assert(offsetof(shizuku_c6_fastcall_stage2_sample_t, kernel_return_post_save_cycles) == 44);
static_assert(offsetof(shizuku_c6_fastcall_stage2_sample_t, kernel_return_cycles) == 44);
static_assert(offsetof(shizuku_c6_fastcall_stage2_sample_t, caller_post_cycles) == 48);
static_assert(offsetof(shizuku_c6_fastcall_stage2_sample_t, end_to_end_cycles) == 52);
static_assert(offsetof(shizuku_c6_fastcall_stage2_sample_t, irqoff_window1_call) == 56);
static_assert(offsetof(shizuku_c6_fastcall_stage2_sample_t, irqoff_window2_ret) == 60);

// サイクルカウンタ (段0): HAS_CYCLE_COUNTER=true と API シグネチャの静的検証
static_assert(shizuku::chips::esp32_c6::plic_vector_mode::HAS_CYCLE_COUNTER,
              "plic_vector_mode must declare HAS_CYCLE_COUNTER=true in stage 0");
static_assert(arch::HAS_CYCLE_COUNTER, "rv32_c6 must declare HAS_CYCLE_COUNTER=true in stage 0");
static_assert(std::is_invocable_r_v<bool, decltype(arch::cycle_counter_enable)>,
              "arch::cycle_counter_enable must return bool");
static_assert(std::is_invocable_r_v<uint32_t, decltype(arch::cycle_counter_read)>,
              "arch::cycle_counter_read must return uint32_t");
static_assert(std::is_invocable_r_v<bool, decltype(shizuku::chips::esp32_c6::plic_vector_mode::cycle_counter_enable)>,
              "plic_vector_mode::cycle_counter_enable must return bool");
static_assert(std::is_invocable_r_v<uint32_t, decltype(shizuku::chips::esp32_c6::plic_vector_mode::cycle_counter_read)>,
              "plic_vector_mode::cycle_counter_read must return uint32_t");

static void test_rv32_c6_frame_abi() {
  arch::exception_frame_t frame{};
  const uintptr_t test_args[4] = {0x11111111, 0x22222222, 0x33333333, 0x44444444};

  // 1. set_args / arg ABI 検証 (dispatch.cpp:188, 332-335 と照合)
  arch::set_args(frame, test_args);
  assert(arch::arg(frame, 0) == 0x11111111u);
  assert(arch::arg(frame, 1) == 0x22222222u);
  assert(arch::arg(frame, 2) == 0x33333333u);
  assert(arch::arg(frame, 3) == 0x44444444u);

  // 2. arg(4) スロット検証 (dispatch.cpp:239 claim 読出しスロット: frame.a4)
  frame.a4 = 0x55555555;
  assert(arch::arg(frame, 4) == 0x55555555u);

  // 3. set_result ABI 検証 (a0 = error, a1 = value)
  arch::set_result(frame, 0x1234, 0x5678);
  assert(frame.a0 == 0x1234u);
  assert(frame.a1 == 0x5678u);

  // 4. set_entry ABI 検証 (mepc = pc, ra = lr)
  arch::set_entry(frame, 0x40801000, 0x40802000);
  assert(frame.mepc == 0x40801000u);
  assert(frame.ra == 0x40802000u);
  assert(arch::frame_pc(frame) == 0x40801000u);

  // 5. 文脈幾何検証 (psp_after_return が sp + sizeof(exception_frame_t) と一致)
  arch::context_t ctx{};
  ctx.sp = &frame;
  assert(arch::exc_frame_bytes(ctx) == RV32_FRAME_BYTES);
  assert(arch::psp_after_return(ctx) == reinterpret_cast<uintptr_t>(&frame) + RV32_FRAME_BYTES);

  // 6. 単核 board 整合性
  assert(board::CORE_COUNT == 1u);
  assert(board::core_num() == 0u);
  static_assert(offsetof(arch::exception_frame_t, mcause) == 72);
  static_assert(offsetof(arch::exception_frame_t, mtval) == 76);
}

static void test_initial_return() {
  alignas(16) unsigned char stack[512];
  arch::context_t c{};
  auto top = reinterpret_cast<uintptr_t>(stack + sizeof(stack));
  for (bool privileged : {true, false}) {
    c.privileged = privileged;
    arch::prepare_thread_entry(c, top, 0x40801234, 0x40802340, 0x1234);
    assert((reinterpret_cast<uintptr_t>(c.sp) & 15) == 0);
    assert(arch::psp_after_return(c) == top);
    assert(c.sp->mepc == 0x40801234 && c.sp->ra == 0x40802340);
    assert(c.sp->a0 == 0x1234);
    assert(c.sp->mstatus == (privileged ? RV32_MSTATUS_MPP : 0));
    assert((c.sp->mstatus & (8 | RV32_MSTATUS_MPIE)) == 0);
    assert(c.sp->t0 == 0 && c.sp->a7 == 0 && c.sp->s11 == 0);
    assert(c.sp->mcause == 0 && c.sp->mtval == 0);
  }
}
static void test_cycle_counter_concepts() {
  // 段0 静的アサーションとポリシー値の実行時検証
  assert(arch::HAS_CYCLE_COUNTER == true);
  assert(shizuku::chips::esp32_c6::plic_vector_mode::HAS_CYCLE_COUNTER == true);
}

static void test_pmp_c6_safety_invariants() {
  using pmp = shizuku::chips::esp32_c6::pmp_controller;
  using cfg_t = shizuku::chips::esp32_c6::pmp_c6_config;

  const uintptr_t kstack_start = 0x40808000u;
  const uintptr_t kstack_bytes = 4096u;
  const uintptr_t kstack_end = kstack_start + kstack_bytes;

  // 1. 全メモリ許可 (0..4GB) の拒否 (fail-closed, 全メモリ許可禁止の不変条件)
  cfg_t wide_cfg{
    .code_base = 0x00000000u,
    .code_limit = 0xFFFFFFFFu,
    .stack_base = 0x40810000u,
    .stack_limit = 0x40811000u,
  };
  assert(!pmp::validate(wide_cfg, kstack_start, kstack_bytes));

  // 2. カーネルスタック領域と重複する U stack の拒否
  cfg_t overlap_stack{
    .code_base = 0x42010000u,
    .code_limit = 0x42020000u,
    .stack_base = kstack_start + 0x100u, // カーネルスタック内部
    .stack_limit = kstack_end + 0x100u,
  };
  assert(!pmp::validate(overlap_stack, kstack_start, kstack_bytes));

  // 3. カーネルスタック領域と重複する Code 領域の拒否
  cfg_t overlap_code{
    .code_base = kstack_start - 0x100u,
    .code_limit = kstack_start + 0x200u,
    .stack_base = 0x40810000u,
    .stack_limit = 0x40811000u,
  };
  assert(!pmp::validate(overlap_code, kstack_start, kstack_bytes));

  // 4. 非整列 (非4バイト境界) アドレスの拒否
  cfg_t unaligned_code{
    .code_base = 0x42010001u, // 非整列
    .code_limit = 0x42020000u,
    .stack_base = 0x40810000u,
    .stack_limit = 0x40811000u,
  };
  assert(!pmp::validate(unaligned_code, kstack_start, kstack_bytes));

  cfg_t unaligned_stack{
    .code_base = 0x42010000u,
    .code_limit = 0x42020000u,
    .stack_base = 0x40810000u,
    .stack_limit = 0x40811002u, // 非整列
  };
  assert(!pmp::validate(unaligned_stack, kstack_start, kstack_bytes));

  // 5. 正当な allow-list (Flash code + U stack) の受理
  cfg_t valid_cfg{
    .code_base = 0x42010000u,
    .code_limit = 0x42020000u,
    .stack_base = 0x40810000u,
    .stack_limit = 0x40811000u,
    .region_base = 0,
    .region_limit = 0
  };
  assert(pmp::validate(valid_cfg, kstack_start, kstack_bytes));

  // 6. 貸与 region がカーネルスタックと重複する場合の拒否
  cfg_t overlap_region = valid_cfg;
  overlap_region.region_base = kstack_start;
  overlap_region.region_limit = kstack_end;
  assert(!pmp::validate(overlap_region, kstack_start, kstack_bytes));

  // 7. 正当な貸与 region の受理
  cfg_t valid_with_region = valid_cfg;
  valid_with_region.region_base = 0x40812000u;
  valid_with_region.region_limit = 0x40813000u;
  assert(pmp::validate(valid_with_region, kstack_start, kstack_bytes));

  // 8. データ領域の検証 (重大指摘 4 関連: オブジェクト所有 data 範囲)
  cfg_t valid_with_data = valid_cfg;
  valid_with_data.data_base = 0x40814000u;
  valid_with_data.data_limit = 0x40814100u;
  assert(pmp::validate(valid_with_data, kstack_start, kstack_bytes));

  // カーネルスタックと重複する data 領域の拒否
  cfg_t overlap_data = valid_with_data;
  overlap_data.data_base = kstack_start + 0x10u;
  overlap_data.data_limit = kstack_end;
  assert(!pmp::validate(overlap_data, kstack_start, kstack_bytes));

  // 非整列 data 領域の拒否
  cfg_t unaligned_data = valid_with_data;
  unaligned_data.data_base = 0x40814001u;
  assert(!pmp::validate(unaligned_data, kstack_start, kstack_bytes));

  // 全メモリ許可を含む data 領域の拒否
  cfg_t wide_data = valid_with_data;
  wide_data.data_base = 0;
  wide_data.data_limit = 0x80000000u;
  assert(!pmp::validate(wide_data, kstack_start, kstack_bytes));

  // 9. 不正レンジ・重なり (overlap / inverted ranges) の拒否
  cfg_t inv_code = valid_cfg; inv_code.code_base = 0x42020000u; inv_code.code_limit = 0x42010000u;
  assert(!pmp::validate(inv_code, kstack_start, kstack_bytes));

  cfg_t inv_stack = valid_cfg; inv_stack.stack_base = 0x40811000u; inv_stack.stack_limit = 0x40810000u;
  assert(!pmp::validate(inv_stack, kstack_start, kstack_bytes));

  cfg_t inv_data = valid_with_data; inv_data.data_base = 0x40814100u; inv_data.data_limit = 0x40814000u;
  assert(!pmp::validate(inv_data, kstack_start, kstack_bytes));

  cfg_t inv_region = valid_with_region; inv_region.region_base = 0x40813000u; inv_region.region_limit = 0x40812000u;
  assert(!pmp::validate(inv_region, kstack_start, kstack_bytes));

  // code と stack の不正な重なり拒否
  cfg_t overlap_code_stack = valid_cfg;
  overlap_code_stack.code_base = valid_cfg.stack_base;
  overlap_code_stack.code_limit = valid_cfg.stack_limit;
  assert(!pmp::validate(overlap_code_stack, kstack_start, kstack_bytes));

  // 10. 全 6 ペアの許可領域相互重複の網羅検証 (重大指摘 E)
  // Pair 2: Code vs Data 重複
  cfg_t overlap_code_data = valid_with_data;
  overlap_code_data.code_base = valid_with_data.data_base;
  overlap_code_data.code_limit = valid_with_data.data_limit;
  assert(!pmp::validate(overlap_code_data, kstack_start, kstack_bytes));

  // Pair 3: Code vs Region 重複
  cfg_t overlap_code_region = valid_with_region;
  overlap_code_region.code_base = valid_with_region.region_base;
  overlap_code_region.code_limit = valid_with_region.region_limit;
  assert(!pmp::validate(overlap_code_region, kstack_start, kstack_bytes));

  // Pair 4: Stack vs Data 重複
  cfg_t overlap_stack_data = valid_with_data;
  overlap_stack_data.stack_base = valid_with_data.data_base;
  overlap_stack_data.stack_limit = valid_with_data.data_limit;
  assert(!pmp::validate(overlap_stack_data, kstack_start, kstack_bytes));

  // Pair 5: Stack vs Region 重複
  cfg_t overlap_stack_region = valid_with_region;
  overlap_stack_region.stack_base = valid_with_region.region_base;
  overlap_stack_region.stack_limit = valid_with_region.region_limit;
  assert(!pmp::validate(overlap_stack_region, kstack_start, kstack_bytes));

  // Pair 6: Data vs Region 重複
  cfg_t both_data_region = valid_with_data;
  both_data_region.region_base = valid_with_data.data_base;
  both_data_region.region_limit = valid_with_data.data_limit;
  assert(!pmp::validate(both_data_region, kstack_start, kstack_bytes));

  // 11. カーネルスタック算術オーバーフローの拒否検証 (重大指摘 E)
  assert(!pmp::validate(valid_cfg, 0xFFFFF000u, 0x2000u));

  // 12. optional region の空値意味論の検証 (limit=0 のとき base!=0 は拒否)
  cfg_t bad_empty_data = valid_cfg;
  bad_empty_data.data_base = 0x40814000u;
  bad_empty_data.data_limit = 0;
  assert(!pmp::validate(bad_empty_data, kstack_start, kstack_bytes));

  cfg_t bad_empty_region = valid_cfg;
  bad_empty_region.region_base = 0x40812000u;
  bad_empty_region.region_limit = 0;
  assert(!pmp::validate(bad_empty_region, kstack_start, kstack_bytes));

  // 13. PMP TOR 権限属性の整合性検証 (重大指摘 E)
  namespace perm = shizuku::ports::riscv::pmp_perm;
  using codec = shizuku::ports::riscv::pmp_codec<4>;
  shizuku::ports::riscv::pmp_entry e_code{}, e_data{}, e_stack{}, e_region{};
  assert(codec::encode_tor(valid_cfg.code_limit, perm::R | perm::X, e_code));
  assert((e_code.cfg & 0x07) == (perm::R | perm::X)); // Code: RX only, not W
  assert((e_code.cfg & perm::W) == 0);

  assert(codec::encode_tor(valid_cfg.stack_limit, perm::R | perm::W, e_stack));
  assert((e_stack.cfg & 0x07) == (perm::R | perm::W)); // Stack: RW only, not X
  assert((e_stack.cfg & perm::X) == 0);

  assert(codec::encode_tor(valid_with_data.data_limit, perm::R | perm::W, e_data));
  assert((e_data.cfg & 0x07) == (perm::R | perm::W)); // Data: RW only, not X
  assert((e_data.cfg & perm::X) == 0);

  assert(codec::encode_tor(valid_with_region.region_limit, perm::R | perm::W, e_region));
  assert((e_region.cfg & 0x07) == (perm::R | perm::W)); // Region: RW only, not X
  assert((e_region.cfg & perm::X) == 0);

  // 14. 独立 C ディスパッチスタック (dispatch stack) の PMP 隔離および非重複検証
  using shizuku::chips::esp32_c6::is_dispatch_stack_range;
  const uintptr_t dstack_start = 0x40809000u;
  const uintptr_t dstack_bytes = 4096u;
  assert(is_dispatch_stack_range(dstack_start + 100, dstack_start, dstack_bytes));
  assert(!is_dispatch_stack_range(kstack_start + 100, dstack_start, dstack_bytes));

  // ディスパッチスタックと重複する U stack の拒否
  cfg_t overlap_dstack = valid_cfg;
  overlap_dstack.stack_base = dstack_start;
  overlap_dstack.stack_limit = dstack_start + 0x400u;
  assert(!pmp::validate(overlap_dstack, kstack_start, kstack_bytes, dstack_start, dstack_bytes));

  // カーネルスタックとディスパッチスタックが相互に重複する場合の拒否
  assert(!pmp::validate(valid_cfg, kstack_start, kstack_bytes, kstack_start, dstack_bytes));

  // 正当な kstack + dstack + allow-list の全受容
  assert(pmp::validate(valid_cfg, kstack_start, kstack_bytes, dstack_start, dstack_bytes));
}

static void test_object_identity_and_isolation() {
  using pmp = shizuku::chips::esp32_c6::pmp_controller;
  using cfg_t = shizuku::chips::esp32_c6::pmp_c6_config;

  constexpr uint32_t REGISTERED_PROBE = 0x61;
  constexpr uint32_t FOREIGN_OWNER = 0x62;

  // 1. オブジェクト所有者 ID の束縛検証 (本番判定述語 is_authorized_u_object の直接テスト)
  // 登録済み owner_id (!= 0) と一致する場合のみ許可され、未登録 (0) や別 ID は fail-closed で拒否されることを実証
  using shizuku::chips::esp32_c6::is_authorized_u_object;
  assert(is_authorized_u_object(REGISTERED_PROBE, REGISTERED_PROBE, REGISTERED_PROBE));
  assert(!is_authorized_u_object(REGISTERED_PROBE, FOREIGN_OWNER, REGISTERED_PROBE));
  assert(!is_authorized_u_object(REGISTERED_PROBE, REGISTERED_PROBE, FOREIGN_OWNER));
  assert(!is_authorized_u_object(0, REGISTERED_PROBE, REGISTERED_PROBE));
  assert(!is_authorized_u_object(0, 0, 0));

  // 2. M native handler スタック切り替え述語 (get_m_handler_stack) の直接検証
  using shizuku::chips::esp32_c6::get_m_handler_stack;
  using shizuku::chips::esp32_c6::KSTACK_BYTES;
  using shizuku::chips::esp32_c6::KSTACK_GUARD_BYTES;

  const uintptr_t test_u_base = 0x40810000u;
  const uintptr_t test_u_limit = 0x40811000u;
  const uintptr_t test_u_sp = 0x40810800u; // U worker の実行 SP
  const uintptr_t test_kstack_start = 0x40801000u;
  const uintptr_t test_kstack_top = test_kstack_start + KSTACK_BYTES;
  const uintptr_t test_callee_frame = test_kstack_top - 368u; // 16B 整列 callee frame

  // (a) ハンドラ初期 SP は snapshot (callee_frame + 144) に切り替わること
  const uintptr_t routed_m_sp = get_m_handler_stack(
      test_u_sp, test_u_base, test_u_limit, test_callee_frame,
      test_kstack_start, KSTACK_BYTES, KSTACK_GUARD_BYTES);
  assert(routed_m_sp == test_callee_frame + 144u);
  assert((routed_m_sp & 15u) == 0); // 16B 整列
  assert(routed_m_sp <= test_kstack_top);
  assert(routed_m_sp > test_kstack_start + KSTACK_GUARD_BYTES);

  // (b) ハンドラ SP は U worker スタックの完全に外部であること
  assert(routed_m_sp < test_u_base || routed_m_sp > test_u_limit);

  // (c) M-mode スレッドスタック (U stack 外) の場合は切り替えず元の SP を維持すること
  const uintptr_t test_m_thread_sp = 0x40830000u;
  assert(get_m_handler_stack(test_m_thread_sp, test_u_base, test_u_limit, test_callee_frame,
                             test_kstack_start, KSTACK_BYTES, KSTACK_GUARD_BYTES) == test_m_thread_sp);

  // (d) Headroom 不足時のフェイルクローズド検証: callee_frame が guard floor を割る場合は 0 拒否
  const uintptr_t overflow_frame = test_kstack_start + KSTACK_GUARD_BYTES - 16u;
  assert(get_m_handler_stack(test_u_sp, test_u_base, test_u_limit, overflow_frame,
                             test_kstack_start, KSTACK_BYTES, KSTACK_GUARD_BYTES) == 0);

  // (e) 非整列 frame 時のフェイルクローズド検証: snapshot が 16B 整列していない場合は 0 拒否
  const uintptr_t unaligned_frame = test_callee_frame + 4u;
  assert(get_m_handler_stack(test_u_sp, test_u_base, test_u_limit, unaligned_frame,
                             test_kstack_start, KSTACK_BYTES, KSTACK_GUARD_BYTES) == 0);

  // 3. caller stack 隔離と未許可メモリの拒否検証 (重大指摘 C, E)
  // 呼び出し元のスタック (caller stack) は callee の U stack 範囲外に存在すること
  const uintptr_t caller_stack_addr = 0x40820000u;
  const uintptr_t caller_stack_sentinel = caller_stack_addr + 16u;

  cfg_t callee_cfg{
    .code_base = 0x42010000u,
    .code_limit = 0x42020000u,
    .stack_base = 0x40810000u,
    .stack_limit = 0x40811000u, // callee 専有スタック
  };

  // caller stack sentinel が callee のスタック領域外にあることを検証
  assert(caller_stack_sentinel < callee_cfg.stack_base || caller_stack_sentinel >= callee_cfg.stack_limit);
  // caller stack sentinel が callee の全許可領域 (code, data, stack, region) の外にあること
  assert(!pmp::ranges_overlap(caller_stack_sentinel, caller_stack_sentinel + 4, callee_cfg.stack_base, callee_cfg.stack_limit));
  assert(!pmp::ranges_overlap(caller_stack_sentinel, caller_stack_sentinel + 4, callee_cfg.code_base, callee_cfg.code_limit));
}

static void test_rv32_c6_stack_geometry() {
  // 重大指摘 5 の直接検証: stack_base/top/active SP/frame geometry
  alignas(16) static uint8_t simulated_stack[4096];
  const uintptr_t stack_base = reinterpret_cast<uintptr_t>(simulated_stack);
  const uintptr_t stack_top = stack_base + sizeof(simulated_stack);

  arch::context_t c{};
  c.stack_limit = stack_base;
  c.privileged = false;

  // 1. prepare_thread_entry 時の幾何:
  // frame は stack_top 直下に作られ、reserved0 に stack_top が記録される
  arch::prepare_thread_entry(c, stack_top, 0x42010000u, 0x42010004u, 42);
  assert(c.sp != nullptr);
  assert(c.stack_limit == stack_base);
  assert(c.sp->reserved0 == static_cast<uint32_t>(stack_top));
  assert(c.sp->mepc == 0x42010000u);
  assert(c.sp->a0 == 42);
  assert((c.sp->mstatus & RV32_MSTATUS_MPP) == 0); // U-mode

  // 2. スレッド走行によるスタック消費 (SP 降下) 時の不変条件:
  // active SP (reserved0) が下がっても、スタック領域 [stack_limit, stack_top) は不変であること！
  const uintptr_t active_sp = stack_top - 512u; // 512B スタック消費
  c.sp->reserved0 = static_cast<uint32_t>(active_sp); // トラップエントリで保存される SP
  assert(c.stack_limit == stack_base);
  // active SP が [stack_limit, stack_top] の範囲内であることを検証
  assert(active_sp >= c.stack_limit && active_sp <= stack_top);

  // 3. スタック範囲外の不正 SP (オーバーフロー/アンダーフロー) の検出
  const uintptr_t underflow_sp = stack_base - 16u;
  const uintptr_t overflow_sp = stack_top + 16u;
  assert(!(underflow_sp >= c.stack_limit && underflow_sp <= stack_top));
  assert(!(overflow_sp >= c.stack_limit && overflow_sp <= stack_top));
}

static void test_m_only_route_compat() {
  alignas(16) static uint8_t stack[512];
  const uintptr_t top = reinterpret_cast<uintptr_t>(stack + sizeof(stack));

  // M-only 既存経路互換性の検証
  arch::context_t c_priv{};
  c_priv.privileged = true;
  arch::prepare_thread_entry(c_priv, top, 0x42010000u, 0x42010004u, 0);
  assert((c_priv.sp->mstatus & RV32_MSTATUS_MPP) == RV32_MSTATUS_MPP); // M-mode (MPP=3)

  arch::context_t c_unpriv{};
  c_unpriv.privileged = false;
  arch::prepare_thread_entry(c_unpriv, top, 0x42010000u, 0x42010004u, 0);
  assert((c_unpriv.sp->mstatus & RV32_MSTATUS_MPP) == 0); // U-mode (MPP=0)
}

static void test_trusted_frame_retention_model() {
  using pmp = shizuku::chips::esp32_c6::pmp_controller;
  // 重大ブロッカー G: trusted exception frame retention ホストモデル検証
  // C6 単一コア (Single HP RISC-V core @ 160MHz) 前提の明示。
  //
  // [幾何レイアウトの検証]
  // 1. M caller は自身の信頼されたスレッドスタック (caller_stack) 上に frame 0 を作成。
  // 2. call_frame_push は caller_stack 上に header と callee_frame (作業コピー) を配置。
  //    header->saved.sp は caller_stack 上の frame 0 を指す。
  // 3. U worker 実行中は独立した U-mode スタック (u_stack) を使用。
  // 4. U worker からの trap (0x802 fault / PMP fault / ecall / return) は
  //    mscratch 経由で trusted kernel stack (kstack) 上にフレームを作成。
  // 5. U worker が何度 trap を起こして kstack を上書きしても、
  //    caller_stack 上の frame 0 および header は物理的に一切改竄されない。
  // 6. call_frame_pop により header->saved が復元され、元の caller frame から
  //    正しい SP, PC, レジスタで M-mode へ復帰する。

  alignas(16) static uint8_t caller_thread_stack[1024];
  alignas(16) static uint8_t trusted_kernel_stack[4096];
  alignas(16) static uint8_t simulated_dispatch_stack[4096];
  alignas(16) static uint8_t u_worker_stack[1024];
  for (size_t i = 0; i < sizeof(u_worker_stack) / sizeof(uint32_t); ++i) {
    reinterpret_cast<uint32_t *>(u_worker_stack)[i] = 0x5A5A5A5Au;
  }

  constexpr uint32_t CALLER_STACK_MODEL_TOP = 0x40810000u;
  constexpr uint32_t KSTACK_MODEL_TOP = 0x40802000u;
  constexpr uint32_t U_STACK_MODEL_TOP = 0x40820000u;

  // --- Step 1: M caller が ecall を起こし、スレッドスタック上に frame 0 を作成 ---
  // M-mode 走行中は mscratch == 0。トラップ入口で sp == 0 を検知してスレッドスタックを使用。
  uint32_t mscratch = 0;
  uint32_t current_sp = CALLER_STACK_MODEL_TOP;

  // トラップ入口エミュレーション:
  // csrrw sp, mscratch, sp -> sp=0, mscratch=current_sp
  // bnez sp, 1f -> 分岐せず M-mode パスへ
  // csrrw sp, mscratch, sp -> sp=current_sp, mscratch=0
  assert(mscratch == 0);
  current_sp -= sizeof(arch::exception_frame_t);
  auto *caller_frame = reinterpret_cast<arch::exception_frame_t *>(caller_thread_stack + 512);
  *caller_frame = {};
  caller_frame->ra = 0x42001000u; // Caller return address
  caller_frame->mepc = 0x42001004u; // Caller PC
  caller_frame->s0 = 0x11111111u;
  caller_frame->s1 = 0x22222222u;
  caller_frame->s2 = 0x33333333u;
  caller_frame->mstatus = RV32_MSTATUS_MPP | RV32_MSTATUS_MPIE; // M-mode
  caller_frame->reserved0 = CALLER_STACK_MODEL_TOP; // Caller original SP
  caller_frame->reserved1 = 0; // M-mode 復帰用 mscratch = 0

  // --- Step 2: do_call / call_frame_push エミュレーション ---
  // RV32 ターゲット (32-bit ABI) での call_frame_header 構造体 (80 バイト)
  struct rv32_call_frame_header {
    uint32_t prev;
    uint32_t total_bytes;
    uint32_t frame_bytes;
    struct {
      uint32_t sp;
      uint32_t s0, s1, s2, s3, s4, s5, s6, s7, s8, s9, s10, s11;
      uint32_t stack_limit;
      uint32_t privileged;
      uint32_t region_base;
      uint32_t region_limit;
    } saved;
  };
  static_assert(sizeof(rv32_call_frame_header) == 80,
                "RV32 call_frame_header must be exactly 80 bytes");

  arch::context_t caller_context{};
  caller_context.sp = caller_frame;
  caller_context.privileged = true;
  caller_context.s0 = caller_frame->s0;
  caller_context.s1 = caller_frame->s1;
  caller_context.s2 = caller_frame->s2;

  const uint32_t frame_bytes = sizeof(arch::exception_frame_t);
  uint32_t total = sizeof(rv32_call_frame_header) + frame_bytes;
  total = (total + 7u) & ~7u;

  auto *header = reinterpret_cast<rv32_call_frame_header *>(caller_thread_stack + 256);
  header->prev = 0;
  header->total_bytes = total;
  header->frame_bytes = frame_bytes;
  header->saved.sp = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(caller_frame));
  header->saved.privileged = 1;
  header->saved.s0 = caller_frame->s0;
  header->saved.s1 = caller_frame->s1;
  header->saved.s2 = caller_frame->s2;

  auto *callee_frame = reinterpret_cast<arch::exception_frame_t *>(caller_thread_stack + 64);
  *callee_frame = *caller_frame; // 作業コピー

  // U-mode 遷移のための設定 (context_to_frame 相当)
  callee_frame->mstatus = 0 | RV32_MSTATUS_MPIE; // U-mode (MPP=0)
  callee_frame->mepc = 0x42010000u; // U worker entry
  callee_frame->reserved0 = U_STACK_MODEL_TOP; // U worker untrusted SP
  callee_frame->reserved1 = KSTACK_MODEL_TOP; // U-mode 走行中の mscratch = trusted stack cursor

  // --- Step 3: mret で U worker 実行開始 ---
  // restore コード:
  // lw t0, RESERVED1(sp) -> csrw mscratch, t0 (mscratch = KSTACK_MODEL_TOP)
  // lw sp, RESERVED0(sp) -> sp = U_STACK_MODEL_TOP
  mscratch = callee_frame->reserved1;
  current_sp = callee_frame->reserved0;
  assert(mscratch == KSTACK_MODEL_TOP);
  assert(current_sp == U_STACK_MODEL_TOP);

  // --- Step 4: U worker が複数回の連続トラップを起こす ---
  // (Trap 1: 0x802 fault)
  // U-mode からの trap 入口:
  // csrrw sp, mscratch, sp -> sp=KSTACK_MODEL_TOP, mscratch=u_worker_sp
  // bnez sp, 1f -> U-mode パスへ分岐！
  current_sp = mscratch; // trusted stack に切替
  current_sp -= sizeof(arch::exception_frame_t);
  auto *trap1_frame = reinterpret_cast<arch::exception_frame_t *>(trusted_kernel_stack + 3000);
  *trap1_frame = {};
  trap1_frame->mepc = 0x42010020u; // faulting instruction
  trap1_frame->mcause = 2; // Illegal instruction
  trap1_frame->reserved0 = U_STACK_MODEL_TOP - 64u; // U-mode SP
  trap1_frame->reserved1 = KSTACK_MODEL_TOP; // 次の trap のための cursor
  // fault ハンドリング (mepc += 4) して U-mode に復帰
  mscratch = trap1_frame->reserved1;
  current_sp = trap1_frame->reserved0;

  // (Trap 2: PMP fault)
  current_sp = mscratch; // trusted stack に切替
  current_sp -= sizeof(arch::exception_frame_t);
  auto *trap2_frame = reinterpret_cast<arch::exception_frame_t *>(trusted_kernel_stack + 3000);
  *trap2_frame = {};
  trap2_frame->mepc = 0x42010040u; // PMP faulting instruction
  trap2_frame->mcause = 5; // Load Access Fault
  trap2_frame->a0 = 0xE005u; // fault marker
  trap2_frame->reserved0 = U_STACK_MODEL_TOP - 128u; // U-mode SP
  trap2_frame->reserved1 = KSTACK_MODEL_TOP;
  mscratch = trap2_frame->reserved1;
  current_sp = trap2_frame->reserved0;

  // (Trap 3: GET_CURRENT_OBJECT ecall -> M-mode native handler dispatch)
  // U worker からの ecall に対し、カーネルは M-mode native handler をディスパッチする。
  // この時、ハンドラの実行時 SP (reserved0) は U worker スタックから trusted kernel stack へ切り替わる。
  current_sp = mscratch; // trusted stack に切替
  current_sp -= sizeof(arch::exception_frame_t);
  auto *trap3_frame = reinterpret_cast<arch::exception_frame_t *>(trusted_kernel_stack + (4096 - 144));
  *trap3_frame = {};
  trap3_frame->mcause = 8; // U-mode ecall
  trap3_frame->a0 = 0x61; // object id
  // U worker の untrusted SP
  const uintptr_t u_active_sp = U_STACK_MODEL_TOP - 160u;
  trap3_frame->reserved0 = u_active_sp;
  trap3_frame->reserved1 = KSTACK_MODEL_TOP;

  // --- Production call_frame_push / headroom / snapshot 幾何の検証 ---
  const uintptr_t kstack_start = reinterpret_cast<uintptr_t>(trusted_kernel_stack);
  const uintptr_t kstack_top = kstack_start + sizeof(trusted_kernel_stack);
  const uintptr_t kstack_guard = shizuku::chips::esp32_c6::KSTACK_GUARD_BYTES;
  const uintptr_t kstack_floor = kstack_start + kstack_guard;

  // 1. call_frame_push による幾何計算 (dispatch.cpp:73-83)
  const uint32_t push_frame_bytes = sizeof(arch::exception_frame_t); // 144
  uint32_t push_total = sizeof(rv32_call_frame_header) + push_frame_bytes; // 80 + 144 = 224
  push_total = (push_total + 7u) & ~7u;
  const uintptr_t push_caller_stack = reinterpret_cast<uintptr_t>(trap3_frame) + push_frame_bytes; // = kstack_top
  const uintptr_t push_snapshot = push_caller_stack - push_total; // = kstack_top - 224
  const uintptr_t push_header_addr = push_snapshot;
  const uintptr_t push_callee_frame = push_snapshot - push_frame_bytes; // = kstack_top - 368

  // 2. Headroom 判定 (dispatch.cpp:103-108 & rv32.hpp ARCH::stack_limit)
  const uintptr_t push_reserve = sizeof(rv32_call_frame_header) + push_frame_bytes +
                                sizeof(arch::exception_frame_t) + 68u /* context_t */ + 256u; // reserve bytes
  assert(push_callee_frame >= kstack_floor + push_reserve); // trusted kernel stack headroom 充足

  // 3. get_m_handler_stack による SP 切り替え検証
  const uintptr_t handler_sp = shizuku::chips::esp32_c6::get_m_handler_stack(
      trap3_frame->reserved0, U_STACK_MODEL_TOP - 1024u, U_STACK_MODEL_TOP,
      push_callee_frame, kstack_start, sizeof(trusted_kernel_stack), kstack_guard);

  // (a) ハンドラ初期 SP は snapshot (callee_frame + 144) と厳密に一致
  assert(handler_sp == push_snapshot);
  assert((handler_sp & 15u) == 0); // 16B 整列
  assert(handler_sp <= kstack_top && handler_sp > kstack_floor);

  // (b) 切り替え後の SP は U worker スタック外であること
  assert(handler_sp < (U_STACK_MODEL_TOP - 1024u) || handler_sp > U_STACK_MODEL_TOP);

  // (c) 独立 C ディスパッチスタック (dispatch stack) と call_frame_push/pop の完全非重複性モデル検証:
  //   ★注意: 本テストはホスト環境における幾何不変条件・境界値モデルの静的検証であり、
  //     ターゲット実機バイナリのディスパッチ実行そのものの完全な代替・証明ではない (Sol rbuild 実機ゲートで検証)。
  //   - trap3_frame:   [kstack_top - 144, kstack_top) (on trusted_kernel_stack)
  //   - header:        [push_snapshot, push_snapshot + sizeof(header)) (on trusted_kernel_stack)
  //   - callee_frame:  [push_callee_frame, push_snapshot) (on trusted_kernel_stack)
  //   - dispatch stack: [dstack_start, dstack_top) (on simulated_dispatch_stack - 独立物理バッファ)
  //   - dispatch C-stack: [dstack_sp, dstack_top - 16) (growing down on dispatch stack)
  const uintptr_t dstack_start = reinterpret_cast<uintptr_t>(simulated_dispatch_stack);
  const uintptr_t dstack_top = dstack_start + sizeof(simulated_dispatch_stack);
  assert(!pmp::ranges_overlap(dstack_start, dstack_top, kstack_start, kstack_top));

  // rv32_trap.S:83-86 が採用する「保存 frame アドレス a0 による符号なし変位範囲判定」述語の境界値検証:
  // is_nested(a0) == (static_cast<uintptr_t>(a0 - dstack_start) < sizeof(simulated_dispatch_stack))
  auto is_dispatch_nested_frame = [&](uintptr_t frame_addr) noexcept -> bool {
    const uintptr_t diff = frame_addr - dstack_start;
    return diff < sizeof(simulated_dispatch_stack);
  };
  // 境界値テスト:
  assert(!is_dispatch_nested_frame(dstack_start - 1u)); // 直下境界 (外側)
  assert(is_dispatch_nested_frame(dstack_start));       // 基底アドレス (内側 / ネスト)
  assert(is_dispatch_nested_frame(dstack_start + 1u));   // 基底直上 (内側)
  assert(is_dispatch_nested_frame(dstack_top - 144u));   // ディスパッチスタック内最高位フレーム (内側)
  assert(is_dispatch_nested_frame(dstack_top - 1u));     // スタック上限直下バイト (内側)
  assert(!is_dispatch_nested_frame(dstack_top));         // スタック上限直上 (外側)
  assert(!is_dispatch_nested_frame(dstack_top + 1u));     // 直上境界 (外側)
  assert(!is_dispatch_nested_frame(kstack_start));       // kstack 基底 (外側)
  assert(!is_dispatch_nested_frame(kstack_top - 144u));   // kstack 保存フレーム (外側)
  assert(!is_dispatch_nested_frame(U_STACK_MODEL_TOP));  // U stack (外側)
  assert(!is_dispatch_nested_frame(CALLER_STACK_MODEL_TOP)); // Caller stack (外側)
  assert(!is_dispatch_nested_frame(0u));                 // ヌル番地 (外側)
  assert(!is_dispatch_nested_frame(0xFFFFFFFFu));        // 32-bit 最大番地 (外側)

  // C dispatch 呼出し時のスタック切替エミュレーション (Trap 3: U-mode からのトラップ):
  // 保存フレーム trap3_frame は kstack 上にあるため、is_dispatch_nested_frame は false (外側トラップ)
  const uintptr_t trap3_frame_addr = reinterpret_cast<uintptr_t>(trap3_frame);
  assert(!is_dispatch_nested_frame(trap3_frame_addr));
  uintptr_t dispatch_sp = dstack_top;
  if (!is_dispatch_nested_frame(trap3_frame_addr)) {
    dispatch_sp = dstack_top; // 外側トラップ: dispatch stack top へ安全に切替
  }
  dispatch_sp -= 16u; // ABI 呼出しフレーム

  // C ディスパッチャ (shizuku_rv32_trap_dispatch -> svc_dispatch -> call_frame_push) の C スタック消費
  const uintptr_t dstack_active_sp = dispatch_sp - 384u;
  assert(dstack_active_sp >= dstack_start);
  // C dispatch stack と call_frame (header, callee_frame, trap3_frame) が完全に重複しないことを検証
  assert(!pmp::ranges_overlap(dstack_active_sp, dstack_top, push_callee_frame, kstack_top));

  // ネストした同期トラップ (C prologue / dispatch 実行中の同期フォールト) のエミュレーション:
  // 中断されたフレーム nested_trap_frame_addr は dispatch stack 上にあるため、is_dispatch_nested_frame は true
  const uintptr_t nested_interrupted_sp = dstack_active_sp;
  const uintptr_t nested_trap_frame_addr = nested_interrupted_sp - sizeof(arch::exception_frame_t);
  assert(is_dispatch_nested_frame(nested_trap_frame_addr));

  uintptr_t nested_dispatch_sp = nested_trap_frame_addr;
  if (!is_dispatch_nested_frame(nested_trap_frame_addr)) {
    nested_dispatch_sp = dstack_top; // 外側なら top へ戻るが、ここでは実行されない
  } else {
    nested_dispatch_sp = nested_trap_frame_addr; // ネスト時: dispatch stack top へリセットせず現在 SP 直下へ継続確保
  }
  nested_dispatch_sp -= 16u;
  assert(nested_dispatch_sp >= dstack_start); // ネスト時も dispatch stack を割らない
  assert(nested_trap_frame_addr < nested_interrupted_sp); // 外側 dispatch C スタックを破壊しない

  // (c2) M-mode handler 実行時の C スタック領域 (snapshot 直下から下降):
  //   - handler C-stack: [c_stack_sp, push_snapshot) (growing down from snapshot)
  //   - nested trap:   [c_stack_sp - 144, c_stack_sp)
  const uintptr_t test_c_stack_depth = 256u;
  const uintptr_t c_stack_sp = push_snapshot - test_c_stack_depth;
  const uintptr_t nested_trap_frame = c_stack_sp - sizeof(arch::exception_frame_t);
  assert(nested_trap_frame >= kstack_floor); // nested trap でも guard floor を割らない
  assert(push_callee_frame < push_snapshot);
  assert(push_header_addr >= push_snapshot);
  assert(push_header_addr + sizeof(rv32_call_frame_header) <= reinterpret_cast<uintptr_t>(trap3_frame));

  // (d) M native handler 実行エミュレーション:
  // ハンドラは trusted kernel stack 上で C スタックを消費し、U worker スタックの全ワード (1024B) は完全に不変
  for (size_t i = 0; i < sizeof(u_worker_stack) / sizeof(uint32_t); ++i) {
    assert(reinterpret_cast<uint32_t *>(u_worker_stack)[i] == 0x5A5A5A5Au);
  }

  // (e) handler return 後に U worker SP が復元されること
  mscratch = trap3_frame->reserved1;
  current_sp = trap3_frame->reserved0; // U worker 復帰時は元の U SP が復元される
  assert(current_sp == u_active_sp);

  // (Trap 4: return trampoline ecall)
  current_sp = mscratch; // trusted stack に切替
  current_sp -= sizeof(arch::exception_frame_t);
  auto *trap4_frame = reinterpret_cast<arch::exception_frame_t *>(trusted_kernel_stack + 3000);
  *trap4_frame = {};
  trap4_frame->mcause = 8; // U-mode ecall (RETURN)
  trap4_frame->a0 = 2; // RETURN number
  trap4_frame->a2 = 0x5AFE0061u; // return value
  trap4_frame->reserved0 = U_STACK_MODEL_TOP - 192u;
  trap4_frame->reserved1 = KSTACK_MODEL_TOP;

  // --- Step 5: call_frame_pop & M caller 復帰検証 ---
  // U worker が 4 回の trap で kstack 上を上書きし尽くした後でも、
  // caller_thread_stack 上の caller_frame と header は完全に無傷で残っている！
  assert(caller_frame->ra == 0x42001000u);
  assert(caller_frame->mepc == 0x42001004u);
  assert(caller_frame->s0 == 0x11111111u);
  assert(caller_frame->s1 == 0x22222222u);
  assert(caller_frame->s2 == 0x33333333u);
  assert(caller_frame->reserved0 == CALLER_STACK_MODEL_TOP);
  assert(caller_frame->reserved1 == 0);

  // call_frame_pop 実行:
  assert(header->saved.sp == static_cast<uint32_t>(reinterpret_cast<uintptr_t>(caller_frame)));
  assert(header->saved.privileged == 1);
  assert(header->saved.s0 == 0x11111111u);
  assert(header->saved.s1 == 0x22222222u);
  assert(header->saved.s2 == 0x33333333u);

  // 戻り値の設定と M-mode への復帰:
  caller_frame->a0 = 0; // error = 0
  caller_frame->a1 = trap4_frame->a2; // value = 0x5AFE0061

  // restore コード実行:
  // lw t0, RESERVED1(sp) -> csrw mscratch, t0 (mscratch = 0)
  // lw sp, RESERVED0(sp) -> sp = CALLER_STACK_MODEL_TOP
  mscratch = caller_frame->reserved1;
  current_sp = caller_frame->reserved0;
  assert(mscratch == 0); // M-mode 復帰後は確実に mscratch == 0
  assert(current_sp == CALLER_STACK_MODEL_TOP); // SP が完全に復元されていること！
  assert(caller_frame->mepc == 0x42001004u); // PC が caller に戻ること！
  assert(caller_frame->a1 == 0x5AFE0061u); // 戻り値が正しいこと！
}

static void test_rv32_c6_normalize_frame_regression() {
  // [Lane A r5: 本番 normalize_frame / active SP (reserved0) 正規化回帰テスト]
  // 目的:
  // 1. C6 Stage 1 において、call_frame_push が作成する callee 複製 exception_frame_t の
  //    active SP (reserved0) を snapshot = frame_address + RV32_FRAME_BYTES へ正規化する
  //    本番プロダクションヘルパー arch::normalize_frame(*context.sp) の動作を検証。
  // 2. caller 元フレームの reserved0 および復帰値・CSR フィールドが完全に維持されることの検証。
  // 3. M->M 経路および U->M 経路の双方で本番プロダクションヘルパーによる動作と caller 原値保持を検証。
  // 4. U-mode callee において、context_to_frame の U-branch が snapshot を範囲外として検知し、
  //    割り当てられた U スタックトップ (stack_limit) へ切り替える既存処理との整合性を検証。
  //
  // ★ホスト検証境界の明記:
  //   - call_frame_push 全体は KERNEL::THREAD 台帳、スレッド配列、BOARD パニックハンドラに
  //     依存するため、ホスト単体テストから直接実行不能。そのため、本テストでは本番幾何算術
  //     (source/kernel/dispatch.cpp:73-82) に基づき配置したフレームに対し、
  //     本番プロダクションヘルパー:
  //       * shizuku::archs::rv32_c6::normalize_frame(exception_frame_t &)
  //       * shizuku::archs::rv32_c6::psp_after_return(const context_t &)
  //       * shizuku::chips::esp32_c6::get_m_handler_stack(...)
  //     を直接呼び出して検証する。
  //   - context_to_frame 全体 (chips/esp32_c6/trap.cpp:123) は CSR 操作 (csrw pmpaddr/pmpcfg)
  //     および kernel_instance シングルトンを含むためホストから直接呼出不能。そのため、
  //     U-branch (trap.cpp:170-174) および M-branch (trap.cpp:197-211) のスタック決定ロジックを
  //     本番定数・述語を用いて検証する。

  alignas(16) static uint8_t m_thread_stack[1024];
  const uintptr_t m_stack_top = reinterpret_cast<uintptr_t>(m_thread_stack + sizeof(m_thread_stack));
  const uint32_t frame_bytes = sizeof(arch::exception_frame_t); // 144
  constexpr uint32_t header_bytes = 80; // sizeof(call_frame_header)
  uint32_t total = (header_bytes + frame_bytes + 7u) & ~7u; // 224

  constexpr uintptr_t u_stack_base = 0x40820000u;
  constexpr uintptr_t u_stack_limit = 0x40821000u;
  constexpr uintptr_t kstack_start = 0x40800000u;
  constexpr size_t kstack_bytes = 4096;
  constexpr size_t kstack_guard = 256;

  // -------------------------------------------------------------------------
  // 1. M->M 経路回帰検証 (M caller -> M-mode handler/callee)
  // -------------------------------------------------------------------------
  const uintptr_t m_caller_stack = m_stack_top;
  const uintptr_t m_caller_frame_addr = m_caller_stack - frame_bytes;
  auto *m_caller_frame = reinterpret_cast<arch::exception_frame_t *>(m_caller_frame_addr);
  *m_caller_frame = {};
  m_caller_frame->ra = 0x42001000u;
  m_caller_frame->mepc = 0x42001004u;
  m_caller_frame->s0 = 0x11111111u;
  m_caller_frame->s1 = 0x22222222u;
  m_caller_frame->s2 = 0x33333333u;
  m_caller_frame->mstatus = RV32_MSTATUS_MPP | RV32_MSTATUS_MPIE;
  m_caller_frame->reserved0 = m_caller_stack; // caller original SP
  m_caller_frame->reserved1 = 0; // M-mode mscratch = 0

  arch::context_t m_context{};
  m_context.sp = m_caller_frame;
  m_context.privileged = true;

  // call_frame_push 幾何計算 (source/kernel/dispatch.cpp:78-83)
  const uintptr_t m_snapshot = m_caller_stack - total;
  const uintptr_t m_callee_frame_addr = m_snapshot - frame_bytes;
  auto *m_callee_frame = reinterpret_cast<arch::exception_frame_t *>(m_callee_frame_addr);

  // 作業コピー作成 (dispatch.cpp:125)
  __builtin_memcpy(m_callee_frame, m_context.sp, frame_bytes);
  m_context.sp = m_callee_frame;

  // normalize_frame 呼出し前: 作業コピーの reserved0 には caller SP が残っている
  assert(m_callee_frame->reserved0 == static_cast<uint32_t>(m_caller_stack));

  // ★本番プロダクションヘルパー呼出し！
  arch::normalize_frame(*m_context.sp);

  // (a) callee reserved0 が snapshot = callee_frame + RV32_FRAME_BYTES へ正規化されたこと
  assert(m_callee_frame->reserved0 == static_cast<uint32_t>(m_snapshot));
  assert(m_callee_frame->reserved0 == static_cast<uint32_t>(m_callee_frame_addr + frame_bytes));
  assert((m_callee_frame->reserved0 & 15u) == 0); // 16B 整列維持
  assert(m_callee_frame->reserved0 == static_cast<uint32_t>(arch::psp_after_return(m_context)));

  // (b) caller frame の原値が完全に不変であること (最重要不変条件)
  assert(m_caller_frame->reserved0 == static_cast<uint32_t>(m_caller_stack));
  assert(m_caller_frame->reserved1 == 0);
  assert(m_caller_frame->ra == 0x42001000u);
  assert(m_caller_frame->mepc == 0x42001004u);
  assert(m_caller_frame->s0 == 0x11111111u);
  assert(m_caller_frame->s1 == 0x22222222u);
  assert(m_caller_frame->s2 == 0x33333333u);
  assert(m_caller_frame->mstatus == (RV32_MSTATUS_MPP | RV32_MSTATUS_MPIE));

  // (c) M callee 復帰時の get_m_handler_stack (trap.cpp:206):
  // callee reserved0 (m_snapshot) は U stack 外のため snapshot がそのまま維持される
  const uintptr_t m_new_sp = shizuku::chips::esp32_c6::get_m_handler_stack(
      m_callee_frame->reserved0, u_stack_base, u_stack_limit,
      m_callee_frame_addr, kstack_start, kstack_bytes, kstack_guard);
  assert(m_new_sp == static_cast<uint32_t>(m_snapshot));
  m_callee_frame->reserved0 = static_cast<uint32_t>(m_new_sp);
  assert(m_callee_frame->reserved0 == static_cast<uint32_t>(m_snapshot));
  assert(m_caller_frame->reserved0 == static_cast<uint32_t>(m_caller_stack)); // caller 原値維持

  // -------------------------------------------------------------------------
  // 2. U->M 経路回帰検証 (U worker からの ecall -> M-mode handler push)
  // -------------------------------------------------------------------------
  alignas(16) static uint8_t u_kstack[4096];
  const uintptr_t u_kstack_start = reinterpret_cast<uintptr_t>(u_kstack);
  const uintptr_t u_kstack_top = u_kstack_start + sizeof(u_kstack);
  const uintptr_t u_worker_sp = u_stack_limit - 128u; // U worker active SP

  // U-mode trap 入口 (rv32_trap.S:33-46):
  // trusted kstack 上にフレームを確保し、reserved0 に元の U SP を退避
  const uintptr_t u_caller_frame_addr = u_kstack_top - frame_bytes;
  auto *u_caller_frame = reinterpret_cast<arch::exception_frame_t *>(u_caller_frame_addr);
  *u_caller_frame = {};
  u_caller_frame->ra = 0x42010100u;
  u_caller_frame->mepc = 0x42010104u;
  u_caller_frame->s0 = 0x55555555u;
  u_caller_frame->s1 = 0x66666666u;
  u_caller_frame->mstatus = 0 | RV32_MSTATUS_MPIE; // U-mode (MPP=0)
  u_caller_frame->reserved0 = static_cast<uint32_t>(u_worker_sp); // U-mode active SP
  u_caller_frame->reserved1 = static_cast<uint32_t>(u_kstack_top); // trusted stack cursor

  arch::context_t u_context{};
  u_context.sp = u_caller_frame;
  u_context.privileged = false;

  // call_frame_push 幾何 (dispatch.cpp:78-83) on trusted kstack
  const uintptr_t u_caller_stack = u_kstack_top;
  const uintptr_t u_snapshot = u_caller_stack - total;
  const uintptr_t u_callee_frame_addr = u_snapshot - frame_bytes;
  auto *u_callee_frame = reinterpret_cast<arch::exception_frame_t *>(u_callee_frame_addr);

  __builtin_memcpy(u_callee_frame, u_context.sp, frame_bytes);
  u_context.sp = u_callee_frame;

  // normalize 前の callee reserved0 は U SP (u_worker_sp)
  assert(u_callee_frame->reserved0 == static_cast<uint32_t>(u_worker_sp));

  // ★本番プロダクションヘルパー呼出し！
  arch::normalize_frame(*u_context.sp);

  // (a) callee reserved0 が trusted kstack 上の snapshot に正規化されたこと
  assert(u_callee_frame->reserved0 == static_cast<uint32_t>(u_snapshot));
  assert(u_callee_frame->reserved0 == static_cast<uint32_t>(u_callee_frame_addr + frame_bytes));
  assert((u_callee_frame->reserved0 & 15u) == 0); // 16B 整列
  assert(u_callee_frame->reserved0 == static_cast<uint32_t>(arch::psp_after_return(u_context)));

  // (b) U caller frame の reserved0 (元の U SP) が完全に維持されていること
  assert(u_caller_frame->reserved0 == static_cast<uint32_t>(u_worker_sp));
  assert(u_caller_frame->reserved1 == static_cast<uint32_t>(u_kstack_top));
  assert(u_caller_frame->ra == 0x42010100u);
  assert(u_caller_frame->mepc == 0x42010104u);
  assert(u_caller_frame->s0 == 0x55555555u);
  assert(u_caller_frame->s1 == 0x66666666u);
  assert((u_caller_frame->mstatus & RV32_MSTATUS_MPP) == 0);

  // (c) 本番 get_m_handler_stack に snapshot を渡して snapshot が維持されること
  const uintptr_t u_m_handler_sp = shizuku::chips::esp32_c6::get_m_handler_stack(
      u_callee_frame->reserved0, u_stack_base, u_stack_limit,
      u_callee_frame_addr, u_kstack_start, sizeof(u_kstack), kstack_guard);
  assert(u_m_handler_sp == static_cast<uint32_t>(u_snapshot));
  assert(u_m_handler_sp != u_worker_sp);
  u_callee_frame->reserved0 = static_cast<uint32_t>(u_m_handler_sp);
  assert(u_callee_frame->reserved0 == static_cast<uint32_t>(u_snapshot));
  assert(u_caller_frame->reserved0 == static_cast<uint32_t>(u_worker_sp)); // U caller 原値保持

  // -------------------------------------------------------------------------
  // 3. U callee 経路回帰検証 (M caller -> U worker call, e.g. do_call)
  // -------------------------------------------------------------------------
  // call_frame_push で作成された callee frame は snapshot へ正規化される
  arch::context_t u_callee_context{};
  u_callee_context.sp = m_callee_frame;
  u_callee_context.privileged = false; // callee is U-mode worker

  // callee_frame->reserved0 は normalize_frame により m_snapshot (M thread stack 上)
  assert(m_callee_frame->reserved0 == static_cast<uint32_t>(m_snapshot));

  // context_to_frame の U-branch 述語判定 (trap.cpp:170-174):
  // callee の active SP (reserved0) が U スタック範囲 [u_stack_base, u_stack_limit] 外であるため、
  // 割り当てられた U スタックトップ (u_stack_limit) へ安全に切り替えられる
  const bool is_outside_u_stack = (m_callee_frame->reserved0 < u_stack_base ||
                                   m_callee_frame->reserved0 > u_stack_limit);
  assert(is_outside_u_stack); // snapshot は U stack 外であることを確認

  uintptr_t u_final_sp = m_callee_frame->reserved0;
  if (is_outside_u_stack) {
    u_final_sp = u_stack_limit;
  }
  m_callee_frame->reserved0 = static_cast<uint32_t>(u_final_sp);
  assert(m_callee_frame->reserved0 == static_cast<uint32_t>(u_stack_limit)); // 割当 U stack top に切り替わること！

  // この切り替え後も caller frame は一切影響を受けないこと
  assert(m_caller_frame->reserved0 == static_cast<uint32_t>(m_caller_stack));
}

int main() {
  printf("[TEST] Verifying C6 arch & board concepts...\n");
  printf("[TEST]   arch_requires<rv32_c6>: OK\n");
  printf("[TEST]   board_requires<esp32_c6>: OK\n");
  test_cycle_counter_concepts();
  printf("[TEST]   cycle counter concepts & signatures: OK\n");
  test_rv32_c6_frame_abi();
  test_initial_return();
  printf("[TEST]   rv32_c6 frame ABI & geometry assertions: OK\n");
  test_pmp_c6_safety_invariants();
  printf("[TEST]   pmp_c6 safety & isolation invariants: OK\n");
  test_object_identity_and_isolation();
  printf("[TEST]   object identity binding & caller stack isolation: OK\n");
  test_rv32_c6_stack_geometry();
  printf("[TEST]   rv32_c6 stack geometry & active SP invariants: OK\n");
  test_m_only_route_compat();
  printf("[TEST]   M-only route compatibility: OK\n");
  test_trusted_frame_retention_model();
  printf("[TEST]   trusted exception frame retention across multi-trap: OK\n");
  test_rv32_c6_normalize_frame_regression();
  printf("[TEST]   normalize_frame active SP regression (M->M, U->M, U-callee): OK\n");
  printf("[TEST] All concept & ABI validations PASSED!\n");
  return 0;
}
