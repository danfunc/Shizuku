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

int main() {
  printf("[TEST] Verifying C6 arch & board concepts...\n");
  printf("[TEST]   arch_requires<rv32_c6>: OK\n");
  printf("[TEST]   board_requires<esp32_c6>: OK\n");
  test_cycle_counter_concepts();
  printf("[TEST]   cycle counter concepts & signatures: OK\n");
  test_stage05_concepts();
  printf("[TEST]   stage 0.5 record concepts, invariants & fail-closed: OK\n");
  test_rv32_c6_frame_abi();
  test_initial_return();
  printf("[TEST]   rv32_c6 frame ABI & geometry assertions: OK\n");
  printf("[TEST] All concept & ABI validations PASSED!\n");
  return 0;
}
