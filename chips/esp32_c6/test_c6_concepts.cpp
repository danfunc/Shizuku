#include <type_traits>
#include <cassert>
#include <cstdio>
#include <initializer_list>
#define SHIZUKU_C6_SKELETON_PERMITTED 1
#include "chips/esp32_c6/rv32_c6.hpp"
#include "boards/esp32_c6/esp32_c6.hpp"
#include "chips/esp32_c6/stage05_trace.h"
#include "chips/esp32_c6/fastcall.h"

using arch = shizuku::archs::rv32_c6;
using board = shizuku::boards::esp32_c6;

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

static void test_stage05_concepts() {
  // 1. レコード初期化と幾何整合
  shizuku_c6_stage05_init(0);
  assert(shizuku_c6_stage05_record.magic == SHIZUKU_C6_STAGE05_MAGIC);
  assert(shizuku_c6_stage05_record.version == SHIZUKU_C6_STAGE05_VERSION);
  assert(shizuku_c6_stage05_record.status == SHIZUKU_C6_STAGE05_STATUS_UNINIT);
  assert(shizuku_c6_stage05_record.rounds_completed == 0);
  assert(shizuku_c6_stage05_record.placement_mode == 0);

  // 2. 正常サンプルのテレスコーピング和検証:
  //    12 区間のデルタの総和が T7 - T0 と厳密に一致すること
  shizuku_c6_stage05_sample_t sample{};
  sample.t0_caller_pre       = 1000;
  sample.t1_trap_entry       = 1050; // d0 = 50
  sample.t2_c_dispatch       = 1120; // d1 = 70
  sample.t3_svc_dispatch     = 1180; // d2 = 60
  sample.t5_return_prep      = 1350; // d3 = 170
  sample.t6_restore_begin    = 1370; // d4 = 20
  sample.t4_handler_entry    = 1430; // d5 = 60
  sample.t1_ret_trap_entry   = 1500; // d6 = 70
  sample.t2_ret_c_dispatch   = 1570; // d7 = 70
  sample.t3_ret_svc_dispatch = 1630; // d8 = 60
  sample.t5_ret_return_prep  = 1780; // d9 = 150
  sample.t6_ret_restore_begin= 1800; // d10 = 20
  sample.t7_caller_post      = 1860; // d11 = 60

  const uint32_t expected_total = sample.t7_caller_post - sample.t0_caller_pre; // 860
  assert(expected_total == 860);
  bool recorded = shizuku_c6_stage05_record_sample(&sample);
  assert(recorded == true);
  assert(shizuku_c6_stage05_record.rounds_completed == 1);
  assert(shizuku_c6_stage05_record.total_raw_min == 860);

  // 3. タイムスタンプ逆行 (順序不正) 時の fail-closed 検出
  shizuku_c6_stage05_sample_t bad_sample = sample;
  bad_sample.t2_c_dispatch = 1040; // T1 (1050) より前に逆行
  bool bad_recorded = shizuku_c6_stage05_record_sample(&bad_sample);
  assert(bad_recorded == false);
  assert(shizuku_c6_stage05_record.status == SHIZUKU_C6_STAGE05_STATUS_FAILED);
  assert(shizuku_c6_stage05_record.error_code == SHIZUKU_C6_STAGE05_ERR_SEQUENCE_ORDER);

  // 4. サンプル数不足 (< 128) での finish() fail-closed 検証
  shizuku_c6_stage05_init(1);
  assert(shizuku_c6_stage05_record.placement_mode == 1);
  shizuku_c6_stage05_finish(); // rounds_completed = 0
  assert(shizuku_c6_stage05_record.status == SHIZUKU_C6_STAGE05_STATUS_FAILED);
  assert(shizuku_c6_stage05_record.error_code == SHIZUKU_C6_STAGE05_ERR_SAMPLE_SHORTAGE);

  // 5. フック校正 (calibrate_hooks) の不変条件検証:
  //    - 校正終了後に active == 0 に戻っていること
  //    - current_sample がゼロ初期化されていること
  //    - t6 / is_return が 0 に戻っていること
  //    - hooks_per_round が 7 (Cフック実行数) であること
  shizuku_c6_stage05_calibrate_hooks();
  assert(shizuku_c6_stage05_active == 0);
  assert(shizuku_c6_stage05_current_sample.t0_caller_pre == 0);
  assert(shizuku_c6_stage05_current_sample.t7_caller_post == 0);
  assert(shizuku_c6_stage05_t6 == 0);
  assert(shizuku_c6_stage05_is_return == 0);
  assert(shizuku_c6_stage05_record.hooks_per_round == 7);

  // 6. is_return フラグの検証
  shizuku_c6_stage05_is_return = 1u;
  assert(shizuku_c6_stage05_is_return == 1u);
  shizuku_c6_stage05_is_return = 0u;
  assert(shizuku_c6_stage05_is_return == 0u);
}

static void test_fastcall_stage2_concepts() {
  // 1. Structure sizing and alignment concepts
  assert(sizeof(shizuku_c6_fastcall_stage2_record_t) == 8512);
  assert(sizeof(shizuku_c6_fastcall_stage2_sample_t) == 64);
  assert(sizeof(shizuku_c6_fastcall_stage2_interval_t) == 32);
  assert(sizeof(shizuku_c6_fastcall_stage2_window_stats_t) == 16);
  assert(sizeof(shizuku_c6_fastcall_stage2_current_timestamps_t) == 32);

  // 2. Production helper: shizuku_c6_fastcall_stage2_init_record
  //    Verifies MIE semantics, fail-closed initial status, and magic/version constants.
  shizuku_c6_fastcall_stage2_record_t rec{};
  shizuku_c6_fastcall_stage2_init_record(&rec, SHIZUKU_C6_FASTCALL_MODE_STAGE2_TIMING);

  assert(rec.magic == SHIZUKU_C6_FASTCALL_STAGE2_MAGIC);
  assert(rec.version == SHIZUKU_C6_FASTCALL_STAGE2_VERSION);
  assert(rec.status == SHIZUKU_C6_FASTCALL_STATUS_UNINIT);
  assert(rec.error_code == SHIZUKU_C6_FASTCALL_ERR_NONE);
  assert(rec.rounds_requested == 128u);
  assert(rec.rounds_completed == 0u);
  assert(rec.placement_mode == 1u); // IRAM asm
  assert(rec.flags == SHIZUKU_C6_FASTCALL_MODE_STAGE2_TIMING);

  // MIE metadata verification:
  //   caller_pre:    UNKNOWN (2) - caller context not statically fixed
  //   kernel_entry:  DISABLED (0) - cleared by RISC-V hardware ecall
  //   callee:        UNKNOWN (2) - restored from caller MPIE
  //   kernel_return: DISABLED (0) - cleared by hardware return ecall
  //   caller_post:   UNKNOWN (2) - restored from callee MPIE
  assert(rec.mie_caller_pre == SHIZUKU_C6_MIE_UNKNOWN);
  assert(rec.mie_kernel_entry == SHIZUKU_C6_MIE_DISABLED);
  assert(rec.mie_callee == SHIZUKU_C6_MIE_UNKNOWN);
  assert(rec.mie_kernel_return == SHIZUKU_C6_MIE_DISABLED);
  assert(rec.mie_caller_post == SHIZUKU_C6_MIE_UNKNOWN);
  assert(rec.wcet_guaranteed == 0); // Empirical max only, NOT formal WCET guarantee
  assert(rec.window1_call.is_full_irqoff == 0);
  assert(rec.window2_ret.is_full_irqoff == 0);

  // 3. Production helper: shizuku_c6_fastcall_stage2_compute_sample
  //    Simulate a typical round with 8 distinct timestamp boundaries:
  //    ts0: 1000 (outer callsite start)
  //    ts1: 1030 (caller_pre = 30)
  //    ts2: 1045 (post-preservation router sample; ecall hardware + vector jump + router prologue = ts2 - ts1 = 15)
  //    ts3: 1070 (callee entry; kernel_entry_post_save = ts3 - ts2 = 25; win1 = ts3 - ts1 = 40)
  //    ts4: 1100 (callee = 30)
  //    ts5: 1115 (post-preservation ret router sample; ret ecall hardware + vector jump + router prologue = ts5 - ts4 = 15)
  //    ts6: 1135 (caller resume; kernel_return_post_save = ts6 - ts5 = 20; win2 = ts6 - ts4 = 35)
  //    ts7: 1170 (caller_post = 35; e2e = ts7 - ts0 = 170)
  shizuku_c6_fastcall_stage2_current_timestamps_t cur_ts{};
  cur_ts.ts0_callsite_start            = 1000;
  cur_ts.ts1_call_ecall_pre            = 1030;
  cur_ts.ts2_call_router_post_prologue = 1045;
  cur_ts.ts3_callee_entry              = 1070;
  cur_ts.ts4_ret_ecall_pre             = 1100;
  cur_ts.ts5_ret_router_post_prologue  = 1115;
  cur_ts.ts6_caller_resume             = 1135;
  cur_ts.ts7_callsite_end              = 1170;

  shizuku_c6_fastcall_stage2_sample_t sample{};
  uint32_t err = shizuku_c6_fastcall_stage2_compute_sample(&cur_ts, &sample);
  assert(err == SHIZUKU_C6_FASTCALL_ERR_NONE);

  assert(sample.caller_pre_cycles               == 30u);
  assert(sample.kernel_entry_post_save_cycles   == 25u);
  assert(sample.kernel_entry_cycles             == 25u);
  assert(sample.callee_cycles                   == 30u);
  assert(sample.kernel_return_post_save_cycles  == 20u);
  assert(sample.kernel_return_cycles            == 20u);
  assert(sample.caller_post_cycles              == 35u);
  assert(sample.irqoff_window1_call             == 40u);
  assert(sample.irqoff_window2_ret              == 35u);
  assert(sample.end_to_end_cycles               == 170u);

  // Exact mathematical relationship between intervals, windows, and E2E:
  // win1 = kernel_entry_post_save + (ts2 - ts1 = ecall hardware latency + vector jump + router prologue)
  assert(sample.irqoff_window1_call == sample.kernel_entry_post_save_cycles + 15u);
  // win2 = kernel_return_post_save + (ts5 - ts4 = ret ecall hardware latency + vector jump + router prologue)
  assert(sample.irqoff_window2_ret == sample.kernel_return_post_save_cycles + 15u);

  // Full E2E vs sum of 5 intervals:
  const uint32_t interval_sum = sample.caller_pre_cycles +
                                sample.kernel_entry_post_save_cycles +
                                sample.callee_cycles +
                                sample.kernel_return_post_save_cycles +
                                sample.caller_post_cycles;
  assert(interval_sum == 140u);
  // The exact difference (170 - 140 = 30) is the sum of the two ecall dispatch & router prologue latencies!
  assert(sample.end_to_end_cycles - interval_sum == 30u);

  // 4. Modulo 32-bit wrap around handling:
  //    Verify that counter wrap across 2^32 produces identical correct deltas
  shizuku_c6_fastcall_stage2_current_timestamps_t wrap_ts{};
  wrap_ts.ts0_callsite_start            = 0xFFFFFFF0u; // wraps across 0
  wrap_ts.ts1_call_ecall_pre            = 0x0000000Eu; // delta = 30
  wrap_ts.ts2_call_router_post_prologue = 0x0000001Du; // delta = 15
  wrap_ts.ts3_callee_entry              = 0x00000036u; // delta = 25
  wrap_ts.ts4_ret_ecall_pre             = 0x00000054u; // delta = 30
  wrap_ts.ts5_ret_router_post_prologue  = 0x00000063u; // delta = 15
  wrap_ts.ts6_caller_resume             = 0x00000077u; // delta = 20
  wrap_ts.ts7_callsite_end              = 0x0000009Au; // delta = 35 (e2e = 170)

  shizuku_c6_fastcall_stage2_sample_t wrap_sample{};
  err = shizuku_c6_fastcall_stage2_compute_sample(&wrap_ts, &wrap_sample);
  assert(err == SHIZUKU_C6_FASTCALL_ERR_NONE);
  assert(wrap_sample.caller_pre_cycles             == 30u);
  assert(wrap_sample.kernel_entry_post_save_cycles == 25u);
  assert(wrap_sample.callee_cycles                 == 30u);
  assert(wrap_sample.kernel_return_post_save_cycles== 20u);
  assert(wrap_sample.caller_post_cycles            == 35u);
  assert(wrap_sample.irqoff_window1_call           == 40u);
  assert(wrap_sample.irqoff_window2_ret            == 35u);
  assert(wrap_sample.end_to_end_cycles             == 170u);

  // 5. Fail-closed error detection: frozen counter
  shizuku_c6_fastcall_stage2_current_timestamps_t frozen_ts = cur_ts;
  frozen_ts.ts7_callsite_end = frozen_ts.ts0_callsite_start; // e2e == 0
  shizuku_c6_fastcall_stage2_sample_t bad_sample{};
  err = shizuku_c6_fastcall_stage2_compute_sample(&frozen_ts, &bad_sample);
  assert(err == SHIZUKU_C6_FASTCALL_ERR_SAMPLE_SHORTAGE);

  // 6. Fail-closed error detection: window nesting violation
  //    (win1 cannot be smaller than kernel_entry_post_save!)
  shizuku_c6_fastcall_stage2_current_timestamps_t nest_ts = cur_ts;
  nest_ts.ts2_call_router_post_prologue = nest_ts.ts1_call_ecall_pre - 5; // invalid inversion
  err = shizuku_c6_fastcall_stage2_compute_sample(&nest_ts, &bad_sample);
  assert(err == SHIZUKU_C6_FASTCALL_ERR_SAMPLE_SHORTAGE);

  // 7. Production helper: shizuku_c6_fastcall_stage2_finalize_stats
  //    Populate full 128 samples with our verified sample
  for (uint32_t i = 0; i < 128; ++i) {
    rec.samples[i] = sample;
  }
  rec.rounds_completed = 128;

  // Case A: Valid calibration (overhead = 6 cycles, clock verified)
  shizuku_c6_fastcall_stage2_finalize_stats(&rec, 6, 1);
  assert(rec.e2e_summary.raw_min == 170u);
  assert(rec.e2e_summary.raw_mean == 170u);
  assert(rec.e2e_summary.raw_max == 170u);
  assert(rec.e2e_summary.net_valid == SHIZUKU_C6_FASTCALL_NET_VALID);
  assert(rec.e2e_summary.net_min == 164u); // 170 - 6
  assert(rec.e2e_summary.net_mean == 164u);
  assert(rec.e2e_summary.net_max == 164u);
  assert(rec.e2e_net_valid == SHIZUKU_C6_FASTCALL_NET_VALID);

  // Interval net values must remain strictly Unknown (0)
  for (uint32_t i = 0; i < SHIZUKU_C6_FASTCALL_STAGE2_INTERVAL_COUNT; ++i) {
    assert(rec.intervals[i].net_valid == SHIZUKU_C6_FASTCALL_NET_UNKNOWN);
    assert(rec.intervals[i].net_min == 0);
    assert(rec.intervals[i].net_mean == 0);
    assert(rec.intervals[i].net_max == 0);
  }
  assert(rec.interval_net_valid == SHIZUKU_C6_FASTCALL_NET_UNKNOWN);

  // Conservative windows
  assert(rec.window1_call.raw_min == 40u);
  assert(rec.window1_call.raw_max == 40u);
  assert(rec.window1_call.is_full_irqoff == 0);
  assert(rec.window2_ret.raw_min == 35u);
  assert(rec.window2_ret.raw_max == 35u);
  assert(rec.window2_ret.is_full_irqoff == 0);

  // Case B: Fail-closed on shortage (< 128 completed rounds)
  shizuku_c6_fastcall_stage2_record_t rec_short{};
  shizuku_c6_fastcall_stage2_init_record(&rec_short, 0);
  for (uint32_t i = 0; i < 127; ++i) rec_short.samples[i] = sample;
  rec_short.rounds_completed = 127; // shortage!
  shizuku_c6_fastcall_stage2_finalize_stats(&rec_short, 6, 1);
  assert(rec_short.status == SHIZUKU_C6_FASTCALL_STATUS_FAILED);
  assert(rec_short.e2e_summary.net_valid == SHIZUKU_C6_FASTCALL_NET_UNKNOWN);
  assert(rec_short.e2e_summary.net_min == 0);

  // Case C: Fail-closed on unverified CPU clock
  shizuku_c6_fastcall_stage2_record_t rec_unverified{};
  shizuku_c6_fastcall_stage2_init_record(&rec_unverified, 0);
  for (uint32_t i = 0; i < 128; ++i) rec_unverified.samples[i] = sample;
  rec_unverified.rounds_completed = 128;
  shizuku_c6_fastcall_stage2_finalize_stats(&rec_unverified, 6, 0); // clock_verified = 0
  assert(rec_unverified.e2e_summary.net_valid == SHIZUKU_C6_FASTCALL_NET_UNKNOWN);
  assert(rec_unverified.e2e_summary.net_min == 0);

  // Case D: Fail-closed on overhead anomaly (overhead >= raw_min)
  shizuku_c6_fastcall_stage2_record_t rec_ovf{};
  shizuku_c6_fastcall_stage2_init_record(&rec_ovf, 0);
  for (uint32_t i = 0; i < 128; ++i) rec_ovf.samples[i] = sample;
  rec_ovf.rounds_completed = 128;
  shizuku_c6_fastcall_stage2_finalize_stats(&rec_ovf, 200, 1); // overhead 200 >= raw_min 170
  assert(rec_ovf.e2e_summary.net_valid == SHIZUKU_C6_FASTCALL_NET_UNKNOWN);
  assert(rec_ovf.e2e_summary.net_min == 0);
}

int main() {
  printf("[TEST] Verifying C6 arch & board concepts...\n");
  printf("[TEST]   arch_requires<rv32_c6>: OK\n");
  printf("[TEST]   board_requires<esp32_c6>: OK\n");
  test_cycle_counter_concepts();
  printf("[TEST]   cycle counter concepts & signatures: OK\n");
  test_stage05_concepts();
  printf("[TEST]   stage 0.5 record concepts, invariants & fail-closed: OK\n");
  test_fastcall_stage2_concepts();
  printf("[TEST]   fastcall stage 2 timing concepts, invariants & math: OK\n");
  test_rv32_c6_frame_abi();
  test_initial_return();
  printf("[TEST]   rv32_c6 frame ABI & geometry assertions: OK\n");
  printf("[TEST] All concept & ABI validations PASSED!\n");
  return 0;
}
