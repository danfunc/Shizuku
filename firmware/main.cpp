#include "pico/stdio.h"
#include "pico/stdlib.h"
#include "shizuku/app_entry.hpp"
#include "shizuku/kernel.hpp"
#include "shizuku/kernel_object.hpp"
#include "shizuku/object_api.hpp"
#include "shizuku/objects/flash_fs.hpp"
#if !defined(SHIZUKU_RP2040)
#include "shizuku/objects/gdb_stub.hpp"
#endif
#if defined(CYW43_WL_GPIO_LED_PIN)
#include "shizuku/objects/ble_uart.hpp"
#endif
#include "shizuku/objects/usb_cdc.hpp"
#include "shizuku/objects/peripherals.hpp"
#include "shizuku/selftest.hpp"

#if defined(SHIZUKU_FLASHFS_ONLY)
namespace shizuku::selftest {
extern uint32_t passed;
extern uint32_t failed;
}
#endif
#include "stdio.h"
#if defined(CYW43_WL_GPIO_LED_PIN)
#include "pico/multicore.h"
#endif

#ifndef SHIZUKU_BUILD_ID
#define SHIZUKU_BUILD_ID "v1"
#endif

#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_BOOT_STAGE_TRACE)
#include "hardware/watchdog.h"
#include "hardware/structs/watchdog.h"
#include "pico/bootrom.h"
#endif
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_CRASH_RECORD)
#include "hardware/watchdog.h"
#include "hardware/structs/watchdog.h"
#endif
#if defined(SHIZUKU_SELFTEST_PROGRESS) && SHIZUKU_SELFTEST_PROGRESS > 0
#include "hardware/watchdog.h"
#include "hardware/structs/watchdog.h"
#include "pico/bootrom.h"
#endif
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_BOOT_STAGE_TRACE) && SHIZUKU_BOOT_STAGE_TRACE > 0
extern "C" bool shizuku_boot_fault_registers(uint32_t *);
extern "C" void shizuku_boot_trace_stack(uint32_t,uint32_t);
extern "C" bool shizuku_boot_diag_data(uint32_t *, uint32_t);
extern "C" void shizuku_boot_diag_get_summary(uint32_t *, uint32_t *, uint32_t *);
extern "C" bool shizuku_boot_diag_get_ring_entry(uint32_t, uint32_t *, uint32_t *, uint32_t *, uint32_t *, uint32_t *, uint32_t *);
#endif

#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_BOOT_STAGE_TRACE) && SHIZUKU_BOOT_STAGE_TRACE > 0
namespace {
enum BootStage : uint32_t {
  kUsbInit = 10,
  kKernelInit = 20,
  kObjectInit = 30,
  kBootstrap = 40,
  kAppEntry = 50,
  kLaunchApp = 55,
  kPeripherals = 60,
  kFlashRegistration = 90,
  kCallLadder = 100,
  kHandlerBinding = 110,
  kCallCost = 120,
  kThreadLadder = 130,
  kMemoryLadder = 140,
  kUnprivileged = 150,
  kSecondaryCore = 160,
  kMulticore = 170,
  kStreamLadder = 180,
  kFlashFsProbe = 190,
  kFlashStream = 200,
  kStress = 220,
  kBootComplete = 230,
};
void boot_stage(uint32_t stage) {
  shizuku::KERNEL::BOARD::boot_trace_stage(stage);
}
}
#endif

// スレッド 0 が最初に走らせるコード = 系の組み立て。ここはまだどのオブジェクトの
// メソッドでもない (フレーム 0 段) ので、撃った svc はオブジェクトと同じ経路で
// カーネルオブジェクトのハンドラへ届く。
#if defined(SHIZUKU_SELFTEST_PROGRESS) && SHIZUKU_SELFTEST_PROGRESS > 0
extern "C" void shizuku_selftest_progress_mark(uint32_t, uint32_t, uint32_t);
extern "C" bool shizuku_selftest_fault_read(uint32_t *);
#if !defined(SHIZUKU_RP2040)
extern "C" bool shizuku_selftest_fault_read2(uint32_t *);
extern "C" bool shizuku_selftest_deep_read(uint32_t *);
#endif
#define APP_PROG(stage) shizuku_selftest_progress_mark((stage), 0, 0)
#else
#define APP_PROG(stage) ((void)0)
#endif
void shizuku::app_entry() {
#if defined(SHIZUKU_BOOT_LOG_WAIT) && defined(CYW43_WL_GPIO_LED_PIN)
  shizuku::objects::usb_cdc_boot_diag_set_stage(
      shizuku::objects::boot_diag_stage::APP_ENTRY);
#endif
  shizuku::KERNEL::BOARD::diag_printf("[BOOT] build: %s %s %s\n",
                                      SHIZUKU_BUILD_ID, __DATE__, __TIME__);
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_BOOT_STAGE_TRACE) && SHIZUKU_BOOT_STAGE_TRACE > 0
  boot_stage(kAppEntry);
#endif
  // ★スレッドが落ちたときに実行権を渡す先を教えておく。誰に渡すかは方針なので
  //   カーネルは選ばない。ここではブートスレッド (アイドル役) を指定する。
  shizuku::kernel_instance.set_recovery_thread(0);


  // ボードが提供するペリフェラルオブジェクト (特権を宣言する数少ないオブジェクト)。
#if defined(SHIZUKU_BOOT_LOG_WAIT) && defined(CYW43_WL_GPIO_LED_PIN)
  shizuku::objects::usb_cdc_boot_diag_set_stage(
      shizuku::objects::boot_diag_stage::PERIPHERALS);
#endif
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_BOOT_STAGE_TRACE) && SHIZUKU_BOOT_STAGE_TRACE > 0
  boot_stage(kPeripherals);
#endif
  const uint32_t peripheral_failures = shizuku::objects::register_peripherals();
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_BOOT_STAGE_TRACE) && SHIZUKU_BOOT_STAGE_TRACE > 0
  boot_stage(kFlashRegistration);
#endif
  // 媒体を持つオブジェクト。読むのは XIP のアドレスを配るだけなので安いが、
  // 書くと XIP ごと止まるので、扱いはペリフェラルと同じく特権側。
#if defined(SHIZUKU_BOOT_LOG_WAIT) && defined(CYW43_WL_GPIO_LED_PIN)
  shizuku::objects::usb_cdc_boot_diag_set_stage(
      shizuku::objects::boot_diag_stage::FLASHFS);
#endif
  shizuku::objects::register_flash_fs();
  bool secondary_core_ready = false;
#if defined(SHIZUKU_FLASHFS_ONLY)
  // Keep flash writes after core 1 is running: erase pauses XIP, so every
  // executing core must participate in the flash-safe lockout.
  secondary_core_ready = shizuku::kernel_object_instance.start_secondary_core();
#if defined(CYW43_WL_GPIO_LED_PIN)
  if (secondary_core_ready) {
    const uint64_t lockout_deadline_us = time_us_64() + 1000000ull;
    while (!multicore_lockout_victim_is_initialized(1) &&
           time_us_64() < lockout_deadline_us)
      tight_loop_contents();
    secondary_core_ready = multicore_lockout_victim_is_initialized(1);
  }
#endif
  shizuku::KERNEL::BOARD::diag_printf(
      "[FLASHFS_ONLY] secondary core %s\n",
      secondary_core_ready ? "ready" : "unavailable");
  if (secondary_core_ready) {
    const uint64_t flashfs_started_us = time_us_64();
    const uint32_t flashfs_failures = shizuku::objects::flash_fs_probe();
    const uint32_t stream_passed_before = shizuku::selftest::passed;
    const uint32_t stream_failed_before = shizuku::selftest::failed;
    shizuku::selftest::flash_stream_ladder();
    const uint32_t stream_passed =
        shizuku::selftest::passed - stream_passed_before;
    const uint32_t stream_failed =
        shizuku::selftest::failed - stream_failed_before;
    shizuku::KERNEL::BOARD::diag_printf(
        "[FLASHFS_ONLY] flash_fs_probe: %s (failures=%lu); "
        "flash_stream_ladder: %lu passed, %lu failed; elapsed=%lu ms\n",
        flashfs_failures == 0 ? "PASS" : "FAIL",
        (unsigned long)flashfs_failures, (unsigned long)stream_passed,
        (unsigned long)stream_failed,
        (unsigned long)((time_us_64() - flashfs_started_us) / 1000ull));
  } else {
    shizuku::KERNEL::BOARD::diag_printf(
        "[FLASHFS_ONLY] FAIL: skipped flash writes because core 1 is not ready\n");
  }
#elif !defined(SHIZUKU_SKIP_SELFTEST)
#if defined(SHIZUKU_BOOT_LOG_WAIT) && defined(CYW43_WL_GPIO_LED_PIN)
  shizuku::objects::usb_cdc_boot_diag_set_stage(
      shizuku::objects::boot_diag_stage::SELFTEST);
#endif
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_BOOT_STAGE_TRACE) && SHIZUKU_BOOT_STAGE_TRACE > 0
  boot_stage(kCallLadder);
#endif
  shizuku::selftest::call_ladder();
  APP_PROG(10);
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_BOOT_STAGE_TRACE) && SHIZUKU_BOOT_STAGE_TRACE > 0
  boot_stage(kHandlerBinding);
#endif
  shizuku::selftest::handler_binding_probe();
  shizuku::selftest::kernel_services_probe();
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_BOOT_STAGE_TRACE) && SHIZUKU_BOOT_STAGE_TRACE > 0
  boot_stage(kCallCost);
#endif
  shizuku::selftest::call_cost();
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_BOOT_STAGE_TRACE) && SHIZUKU_BOOT_STAGE_TRACE > 0
  boot_stage(kThreadLadder);
#endif
  shizuku::selftest::thread_ladder();
  shizuku::selftest::kernel_services_isr_result();
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_BOOT_STAGE_TRACE) && SHIZUKU_BOOT_STAGE_TRACE > 0
  boot_stage(kMemoryLadder);
#endif
  shizuku::selftest::memory_ladder();
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_BOOT_STAGE_TRACE) && SHIZUKU_BOOT_STAGE_TRACE > 0
  boot_stage(kUnprivileged);
#endif
  shizuku::selftest::unprivileged_probe();

  // ★2 本目のコアを起こす。ここまでの自己テストが 1 コアで通っていることを
  //   確かめてから起こす — 先に起こすと、失敗したときに「並行のせいか元からか」を
  //   切り分けられない (梯子式の作法。DESIGN §16)。
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_BOOT_STAGE_TRACE) && SHIZUKU_BOOT_STAGE_TRACE > 0
  boot_stage(kSecondaryCore);
#endif
  secondary_core_ready = shizuku::kernel_object_instance.start_secondary_core();
#if defined(CYW43_WL_GPIO_LED_PIN)
  if (secondary_core_ready) {
    const uint64_t lockout_deadline_us = time_us_64() + 1000000ull;
    while (!multicore_lockout_victim_is_initialized(1) &&
           time_us_64() < lockout_deadline_us)
      tight_loop_contents();
    secondary_core_ready = multicore_lockout_victim_is_initialized(1);
  }
#endif
  if (secondary_core_ready)
    shizuku::KERNEL::BOARD::diag_printf("[BOOT] secondary core launched\n");
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_BOOT_STAGE_TRACE) && SHIZUKU_BOOT_STAGE_TRACE > 0
  boot_stage(kMulticore);
#endif
  shizuku::selftest::multicore_probe();
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_BOOT_STAGE_TRACE) && SHIZUKU_BOOT_STAGE_TRACE > 0
  boot_stage(kStreamLadder);
#endif
  shizuku::selftest::stream_ladder();
  // ★flash の書き込みは 2 コア目を起こした**後**に試す。消去中は XIP が止まるので、
  //   相手が止められていなければそのコアは flash 上のコードを踏んで即死する。
  //   つまりここで書けること自体が「止められている」ことの証拠になる。
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_BOOT_STAGE_TRACE) && SHIZUKU_BOOT_STAGE_TRACE > 0
  boot_stage(kFlashFsProbe);
#endif
  APP_PROG(60);
  shizuku::objects::flash_fs_probe();
  APP_PROG(61);
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_BOOT_STAGE_TRACE) && SHIZUKU_BOOT_STAGE_TRACE > 0
  boot_stage(kFlashStream);
#endif
  shizuku::selftest::flash_stream_ladder();
  APP_PROG(62);
#if !defined(SHIZUKU_RP2040)
  shizuku::selftest::debug_ladder();
#endif

  APP_PROG(63);

  // 負荷試験を起動する。以後、点滅と報告は専用スレッドが行う。
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_BOOT_STAGE_TRACE) && SHIZUKU_BOOT_STAGE_TRACE > 0
  boot_stage(kStress);
#endif
  const uint32_t blink_thread = shizuku::selftest::stress_launch();
  APP_PROG(64);
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_BOOT_STAGE_TRACE) && SHIZUKU_BOOT_STAGE_TRACE > 0
  boot_stage(kBootComplete);
  for (uint32_t second = 0; second < 40; ++second) {
    watchdog_update();
    sleep_ms(1000);
  }
  // Leave the final stage recorded and stop feeding: recovery reports completion,
  // then enters BOOTSEL so a diagnostic image cannot strand the board.
#endif
#endif // !SHIZUKU_SKIP_SELFTEST
#if defined(SHIZUKU_SKIP_SELFTEST) && !defined(SHIZUKU_FLASHFS_ONLY)
  // BLE/BTstack first-run may erase the bonding TLV through flash_safe_execute,
  // which requires core 1 to be running even when selftests are omitted.
#if defined(SHIZUKU_BOOT_LOG_WAIT) && defined(CYW43_WL_GPIO_LED_PIN)
  shizuku::objects::usb_cdc_boot_diag_set_stage(
      shizuku::objects::boot_diag_stage::SECONDARY_CORE);
#endif
  secondary_core_ready = shizuku::kernel_object_instance.start_secondary_core();
#if defined(CYW43_WL_GPIO_LED_PIN)
  if (secondary_core_ready) {
    const uint64_t lockout_deadline_us = time_us_64() + 1000000ull;
    while (!multicore_lockout_victim_is_initialized(1) &&
           time_us_64() < lockout_deadline_us)
      tight_loop_contents();
    secondary_core_ready = multicore_lockout_victim_is_initialized(1);
  }
#endif
  if (secondary_core_ready)
    shizuku::KERNEL::BOARD::diag_printf("[BOOT] secondary core launched\n");
  else
    shizuku::KERNEL::BOARD::diag_printf("[BOOT] secondary core unavailable\n");
#endif

#if !defined(SHIZUKU_RP2040) && !defined(SHIZUKU_SKIP_SELFTEST)
  // ★GDB stub。繋がれるまでは何もしない (繋がれた瞬間に診断出力を GDB へ譲る)。
  // ★★2026-08-24 デモ用: 合成の debuggee ではなく実在の blink (LED を叩く
  //   スレッド) を対象にする。attach するだけで LED が目に見えて止まり、
  //   detach で再開する — DebugMonitor が「そのスレッドだけ」止めることの
  //   一番分かりやすい確認 (docs/record/kernel_v2_record.typ D43)。
  shizuku::objects::start_gdb_stub(blink_thread);
#endif

#if defined(CYW43_WL_GPIO_LED_PIN)
  const uintptr_t BLE_UART_OBJ = shizuku::object_id::ble_uart;

  if (!secondary_core_ready) {
#if defined(SHIZUKU_BOOT_LOG_WAIT)
    shizuku::objects::usb_cdc_boot_diag_set_stage(
        shizuku::objects::boot_diag_stage::ERROR);
    shizuku::objects::usb_cdc_boot_diag_set_ble("core1-unavailable");
#endif
    shizuku::KERNEL::BOARD::diag_printf(
        "[BOOT] BLE not started: core 1 unavailable for flash-safe CYW43 init\n");
  } else {
#if defined(SHIZUKU_BOOT_LOG_WAIT)
  shizuku::objects::usb_cdc_boot_diag_set_stage(
      shizuku::objects::boot_diag_stage::BLE_REGISTER);
#endif
  const uint32_t ble_rc =
      shizuku::objects::ble_uart::register_ble_uart(BLE_UART_OBJ);
  if (ble_rc != 0) {
    shizuku::KERNEL::BOARD::diag_printf("[BOOT] register_ble_uart failed: %lu\n",
                                        (unsigned long)ble_rc);
  }

  if (ble_rc == 0) {
    // 起動 (CYW43 の owner = ble_uart の poll スレッド。init もここで行う)
#if defined(SHIZUKU_BOOT_LOG_WAIT)
    shizuku::objects::usb_cdc_boot_diag_set_stage(
        shizuku::objects::boot_diag_stage::BLE_POLL_START);
#endif
#if defined(SHIZUKU_CRASH_RECORD)
    shizuku::objects::usb_cdc_boot_diag_crash_arm();
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_BOOT_LOG_WAIT)
    shizuku::objects::usb_cdc_boot_diag_flash_selftest(0);
#endif
#endif
    shizuku::objects::ble_uart::start_ble_uart(BLE_UART_OBJ);
  } else {
    // fail-closed: owner が立たないので CYW43 には一切触れない。LED 書込みは
    // LED_NO_OWNER で失敗し、ここに理由を残す。
#if defined(SHIZUKU_BOOT_LOG_WAIT)
    shizuku::objects::usb_cdc_boot_diag_set_stage(
        shizuku::objects::boot_diag_stage::ERROR);
    shizuku::objects::usb_cdc_boot_diag_set_ble("register-failed");
#endif
    shizuku::KERNEL::BOARD::diag_printf(
        "[BOOT] BLE not started (ble=%lu): CYW43 owner absent, LED disabled\n",
        (unsigned long)ble_rc);
  }
  }
#endif

  // ★スレッド 0 は以後アイドル役に徹する。誰かが走れるなら渡し、誰も居なければ
  //   空回りするだけ。**ここで自分が仕事をしてはいけない** — アイドルが仕事を
  //   持つと、その仕事が他の全部の遅れになる。
  while (true) {
#if defined(SHIZUKU_BOOT_LOG_WAIT)
    shizuku::objects::usb_cdc_boot_diag_poll();
#endif
#if defined(SHIZUKU_BOOT_LOG_WAIT) && defined(SHIZUKU_CRASH_RECORD)
    shizuku::objects::usb_cdc_boot_diag_breadcrumb(
        shizuku::objects::boot_diag_breadcrumb::CORE_IDLE_ENTRY);
#endif
    shizuku::KERNEL::ARCH::syscall((uintptr_t)shizuku::object_api::YIELD);
#if defined(SHIZUKU_BOOT_LOG_WAIT) && defined(SHIZUKU_CRASH_RECORD)
    shizuku::objects::usb_cdc_boot_diag_breadcrumb(
        shizuku::objects::boot_diag_breadcrumb::CORE_IDLE_EXIT);
#endif
  }
}

int main() {
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_CRASH_RECORD)
  const bool crash_recovered = watchdog_caused_reboot() &&
      (watchdog_hw->scratch[0] == 0x43524153u ||
       watchdog_hw->scratch[0] == 0x48465254u);
  uint32_t crash_record[8] = {};
  if (crash_recovered) {
    for (uint32_t i = 0; i < 8; ++i)
      crash_record[i] = watchdog_hw->scratch[i];
#if defined(SHIZUKU_BOOT_LOG_WAIT)
    shizuku::objects::usb_cdc_boot_diag_crash_report_capture(crash_record);
#endif
  }
#if defined(SHIZUKU_BOOT_LOG_WAIT) && defined(CYW43_WL_GPIO_LED_PIN)
  const char flashdiag_image_id[] =
      SHIZUKU_BUILD_ID ":" __DATE__ ":" __TIME__;
  uint32_t flashdiag_image_stamp = 2166136261u;
  for (const char *p = flashdiag_image_id; *p != '\0'; ++p) {
    flashdiag_image_stamp ^= (uint8_t)*p;
    flashdiag_image_stamp *= 16777619u;
  }
  shizuku::objects::usb_cdc_boot_diag_flashdiag_begin_boot(
      crash_recovered, flashdiag_image_stamp);
#endif
  watchdog_disable();
#endif
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_BOOT_STAGE_TRACE)
  const uint32_t saved0 = watchdog_hw->scratch[0];
  const uint32_t saved1 = watchdog_hw->scratch[1];
  const uint32_t saved4 = watchdog_hw->scratch[4];
  const bool recovered_watchdog = watchdog_caused_reboot() &&
                                  (saved0 >> 16) == 0x5348u;
  const uint32_t failed_stage = saved1 & 0xffffu;
  const uint32_t fault_cause = (saved1 >> 16) & 0xffu;
  const bool hardfault_snapshot = (fault_cause & 0x80u) != 0;
  const uint32_t fault_xpsr = hardfault_snapshot ? watchdog_hw->scratch[5] : 0;
  const uint32_t fault_ipsr = hardfault_snapshot ? watchdog_hw->scratch[6] : 0;
  const uint32_t fault_primask = hardfault_snapshot ? watchdog_hw->scratch[7] : 0;
  const uint32_t fault_pc = hardfault_snapshot ? watchdog_hw->scratch[2] : 0;
  const uint32_t fault_lr = hardfault_snapshot ? watchdog_hw->scratch[3] : 0;
  uint32_t fault_regs[7]={};
  bool fault_regs_valid=false;
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_BOOT_STAGE_TRACE) && SHIZUKU_BOOT_STAGE_TRACE > 0
  fault_regs_valid=shizuku_boot_fault_registers(fault_regs) &&
                   recovered_watchdog && hardfault_snapshot;
  uint32_t diag_words[16]={};
  bool diag_valid=shizuku_boot_diag_data(diag_words, 16) && recovered_watchdog;
#endif
  watchdog_disable();
  if (recovered_watchdog)
    watchdog_hw->scratch[0] = 0;
  if (recovered_watchdog) {
    shizuku::objects::usb_cdc_init();
    for (uint32_t i = 0; i < 30; ++i) {
      const bool hardfault = hardfault_snapshot;
      printf("[WDT] stage=%lu phase=%lu cause=%02lx mode=%s stack=%s\n",
             (unsigned long)failed_stage, (unsigned long)saved4,
             (unsigned long)fault_cause,
             hardfault ? ((fault_cause & 2u) ? "Thread" : "Handler") : "-",
             hardfault ? ((fault_cause & 1u) ? "PSP" : "MSP") : "-");
      fflush(stdout);
      sleep_ms(100);
      printf("pc=%08lx lr=%08lx xpsr=%08lx ipsr=%08lx primask=%08lx\n",
             (unsigned long)fault_pc, (unsigned long)fault_lr,
             (unsigned long)fault_xpsr, (unsigned long)fault_ipsr,
             (unsigned long)fault_primask);
      if(fault_regs_valid)
        printf("r0=%08lx r1=%08lx r2=%08lx r3=%08lx r12=%08lx\n",
               (unsigned long)fault_regs[0], (unsigned long)fault_regs[1],
               (unsigned long)fault_regs[2], (unsigned long)fault_regs[3],
               (unsigned long)fault_regs[4]);
      if(fault_regs_valid)
        printf("boot_stack=[%08lx,%08lx)\n",
               (unsigned long)fault_regs[5], (unsigned long)fault_regs[6]);
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_BOOT_STAGE_TRACE) && SHIZUKU_BOOT_STAGE_TRACE > 0
      if(diag_valid) {
        printf("diag min_sp=%08lx last_sp=%08lx near_bottom_hits=%lu\n",
               (unsigned long)diag_words[0], (unsigned long)diag_words[1],
               (unsigned long)diag_words[2]);
        printf("diag last_prim=%lu sp_before=%08lx sp_after=%08lx\n",
               (unsigned long)diag_words[3], (unsigned long)diag_words[4],
               (unsigned long)diag_words[5]);
        printf("diag pools=[%08lx,%08lx,%08lx,%08lx]\n",
               (unsigned long)diag_words[6], (unsigned long)diag_words[7],
               (unsigned long)diag_words[8], (unsigned long)diag_words[9]);
        printf("diag def_pool=[%08lx,%08lx,%08lx,%08lx,%08lx,%08lx]\n",
               (unsigned long)diag_words[10], (unsigned long)diag_words[11],
               (unsigned long)diag_words[12], (unsigned long)diag_words[13],
               (unsigned long)diag_words[14], (unsigned long)diag_words[15]);
        uint32_t max_depth = 0, max_depth_sp = 0, ring_count = 0;
        shizuku_boot_diag_get_summary(&max_depth, &max_depth_sp, &ring_count);
        printf("diag max_depth=%lu max_depth_sp=%08lx total_pushes=%lu\n",
               (unsigned long)max_depth, (unsigned long)max_depth_sp,
               (unsigned long)ring_count);
        for(uint32_t ring_i = 0; ring_i < 24; ++ring_i) {
          uint32_t thr = 0, depth = 0, obj = 0, kind = 0, hobj = 0, sp = 0;
          if(shizuku_boot_diag_get_ring_entry(ring_i, &thr, &depth, &obj, &kind, &hobj, &sp)) {
            printf("diag ring %lu: thr=%lu depth=%lu obj=%lu kind=%lu hobj=%lu sp=%08lx\n",
                   (unsigned long)ring_i, (unsigned long)thr, (unsigned long)depth,
                   (unsigned long)obj, (unsigned long)kind, (unsigned long)hobj,
                   (unsigned long)sp);
          }
        }
      }
#endif
      fflush(stdout);
      sleep_ms(900);
    }
    reset_usb_boot(0, 0);
  }
#if SHIZUKU_BOOT_STAGE_TRACE == 0
  watchdog_hw->scratch[0] = 0x53480000;
  watchdog_hw->scratch[1] = 0;
  watchdog_hw->scratch[2] = 0;
  watchdog_hw->scratch[3] = 0;
  watchdog_enable(8000, true);
  shizuku::objects::usb_cdc_init();
  uint32_t alive = 0;
  while (alive < 40) {
    printf("alive %lu\n", (unsigned long)alive++);
    fflush(stdout);
    watchdog_update();
    sleep_ms(1000);
  }
  watchdog_disable();
  watchdog_hw->scratch[0] = 0;
  reset_usb_boot(0, 0);
#else
  watchdog_hw->scratch[0] = 0x53480000;
  watchdog_hw->scratch[1] = kUsbInit;
  watchdog_hw->scratch[2] = 0;
  watchdog_hw->scratch[3] = 0;
  watchdog_hw->scratch[4] = 0;
  watchdog_enable(8000, true);
  shizuku::objects::usb_cdc_init();
  boot_stage(kKernelInit);
  sleep_ms(1000);
  shizuku::kernel_instance.init();
  boot_stage(kObjectInit);
  shizuku::kernel_object_instance.init();
  boot_stage(kBootstrap);
  shizuku::kernel_instance.set_object_handler(
      shizuku::KERNEL_OBJECT::handler_entry(),
      (uint32_t)shizuku::KERNEL_OBJECT::KERNEL_OBJECT_ID);
  const auto boot = shizuku::kernel_object_instance.lend_boot_stack();
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_BOOT_STAGE_TRACE) && SHIZUKU_BOOT_STAGE_TRACE > 0
  shizuku_boot_trace_stack((uint32_t)boot.base,
                           (uint32_t)(boot.base+boot.bytes));
#endif
  boot_stage(kLaunchApp);
  shizuku::kernel_instance.bootstrap(shizuku::app_entry, boot.base, boot.bytes, boot.ledger,
                                     boot.ledger_capacity);
#endif
#else
#if defined(SHIZUKU_SELFTEST_PROGRESS) && SHIZUKU_SELFTEST_PROGRESS > 0
  // ★stream_ladder 等が割込み禁止のまま止まったとき (SysTick も死ぬので
  //   shizuku_armv6m_systick_dispatch の feed も止まる) に、watchdog リセット
  //   で確実に BOOTSEL へ戻す。scratch レジスタは電源断を伴わないリセットでは
  //   保持されるので、停止直前の進捗 (shizuku_selftest_progress_mark) を読める。
  constexpr uint32_t kSelftestProgressMagic = 0x53505247u; // "SPRG"
  const bool selftest_progress_recovered =
      watchdog_caused_reboot() && watchdog_hw->scratch[0] == kSelftestProgressMagic;
  const uint32_t selftest_progress_stage = watchdog_hw->scratch[1];
  const uint32_t selftest_progress_a = watchdog_hw->scratch[2];
  const uint32_t selftest_progress_b = watchdog_hw->scratch[3];
  const uint32_t selftest_fault4 = watchdog_hw->scratch[4];
  const uint32_t selftest_fault5 = watchdog_hw->scratch[5];
  const uint32_t selftest_fault6 = watchdog_hw->scratch[6];
  const uint32_t selftest_fault7 = watchdog_hw->scratch[7];
  uint32_t st_fault[26] = {};
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_CRASH_RECORD)
  const bool st_fault_valid =
      (selftest_progress_recovered || crash_recovered) &&
      shizuku_selftest_fault_read(st_fault);
#else
  const bool st_fault_valid = selftest_progress_recovered &&
                              shizuku_selftest_fault_read(st_fault);
#endif
#if !defined(SHIZUKU_RP2040)
  uint32_t st_fpre[10] = {};
  const bool st_fpre_valid = selftest_progress_recovered && shizuku_selftest_fault_read2(st_fpre);
  uint32_t st_deep[3] = {};
  const bool st_deep_valid = selftest_progress_recovered && shizuku_selftest_deep_read(st_deep);
#endif
  watchdog_disable();
  if (selftest_progress_recovered) {
    shizuku::objects::usb_cdc_init();
    for (uint32_t i = 0; i < 20; ++i) {
      printf("[SELFTEST_PROG] watchdog reset: stage=%lu a=%lu b=%lu\n",
             (unsigned long)selftest_progress_stage,
             (unsigned long)selftest_progress_a,
             (unsigned long)selftest_progress_b);
      printf("[SELFTEST_PROG] fs_fault=%08lx pc=%08lx lr=%08lx xpsr=%08lx\n",
             (unsigned long)selftest_fault4, (unsigned long)selftest_fault5,
             (unsigned long)selftest_fault6, (unsigned long)selftest_fault7);
      if (st_fault_valid && st_fault[18] == 0x53545047u) {
        printf("[SELFTEST_PROG] noinit mark stage=%lu run=%lu event=%lu value=%08lx\n",
               (unsigned long)st_fault[19],
               (unsigned long)(st_fault[20] >> 8),
               (unsigned long)(st_fault[20] & 0xffu),
               (unsigned long)st_fault[21]);
      }
#if !defined(SHIZUKU_RP2040)
      if (st_deep_valid)
        printf("[SELFTEST_PROG] deep last depth=%lu sp=%08lx calls=%lu\n",
               (unsigned long)st_deep[0], (unsigned long)st_deep[1], (unsigned long)st_deep[2]);
      if (st_fpre_valid) {
        printf("[SELFTEST_PROG] fault_pre count=%lu cfsr=%08lx hfsr=%08lx mmfar=%08lx bfar=%08lx\n",
               (unsigned long)st_fpre[0], (unsigned long)st_fpre[1], (unsigned long)st_fpre[2],
               (unsigned long)st_fpre[3], (unsigned long)st_fpre[4]);
        printf("[SELFTEST_PROG] fault_pre psp=%08lx psplim=%08lx excret=%08lx pc=%08lx stage=%lu\n",
               (unsigned long)st_fpre[5], (unsigned long)st_fpre[6], (unsigned long)st_fpre[7],
               (unsigned long)st_fpre[8], (unsigned long)st_fpre[9]);
      }
      if (st_fault_valid) {
        printf("[SELFTEST_PROG] tick sample pc=%08lx lr=%08lx xpsr=%08lx hits=%lu stage=%lu\n",
               (unsigned long)st_fault[12], (unsigned long)st_fault[13], (unsigned long)st_fault[14],
               (unsigned long)st_fault[15], (unsigned long)st_fault[16]);
        printf("[SELFTEST_PROG] cfsr=%08lx hfsr=%08lx primask=%lu control=%lu ipsr=%lu\n",
               (unsigned long)st_fault[5], (unsigned long)st_fault[6], (unsigned long)st_fault[7],
               (unsigned long)st_fault[10], (unsigned long)st_fault[11]);
      }
#else
      if (st_fault_valid) {
        if (st_fault[0] != 0) {
          printf("[SELFTEST_PROG] kfault phase=%lu arg=%lu count=%lu pc=%08lx lr=%08lx xpsr=%08lx\n",
                 (unsigned long)st_fault[0], (unsigned long)st_fault[17], (unsigned long)st_fault[1],
                 (unsigned long)st_fault[2], (unsigned long)st_fault[3], (unsigned long)st_fault[4]);
          printf("[SELFTEST_PROG] kfault r0-3=%08lx %08lx %08lx %08lx excret=%08lx ctl=%lu ipsr=%lu\n",
                 (unsigned long)st_fault[5], (unsigned long)st_fault[6], (unsigned long)st_fault[7],
                 (unsigned long)st_fault[8], (unsigned long)st_fault[9], (unsigned long)st_fault[10],
                 (unsigned long)st_fault[11]);
        } else {
          printf("[SELFTEST_PROG] no selftest fault frame; use crash-record report if present\n");
        }
        printf("[SELFTEST_PROG] tick sample pc=%08lx lr=%08lx xpsr=%08lx hits=%lu stage=%lu\n",
               (unsigned long)st_fault[12], (unsigned long)st_fault[13], (unsigned long)st_fault[14],
               (unsigned long)st_fault[15], (unsigned long)st_fault[16]);
#if defined(SHIZUKU_RP2040)
        printf("[SELFTEST_PROG] tick psp=%08lx control=%lu frame=%08lx frame_uses_psp=%lu\n",
               (unsigned long)st_fault[22], (unsigned long)st_fault[23],
               (unsigned long)st_fault[24], (unsigned long)st_fault[25]);
#endif
      }
#endif
      fflush(stdout);
      sleep_ms(200);
    }
    reset_usb_boot(0, 0);
  }
  watchdog_hw->scratch[0] = 0;
  watchdog_enable(8000, true);
#endif
  // ★USB は自前で持つ (CDC 2 本: 診断と GDB)。pico_stdio_usb は 1 本前提で、
  //   記述子も差し替えられないため (D42)。
#if defined(SHIZUKU_BOOT_LOG_WAIT)
  shizuku::objects::usb_cdc_boot_diag_set_stage(
      shizuku::objects::boot_diag_stage::USB_WAIT);
#endif
  shizuku::objects::usb_cdc_init();
#if defined(SHIZUKU_BOOT_LOG_WAIT)
  const uint64_t wait_start_us = time_us_64();
  const uint64_t wait_deadline_us = wait_start_us + 15000000ull;
  while (!shizuku::objects::usb_cdc_connected(0) &&
         time_us_64() < wait_deadline_us)
    sleep_ms(10);
  const bool cdc_connected = shizuku::objects::usb_cdc_connected(0);
  printf("[BOOT_LOG_WAIT] CDC0 connected=%u waited_ms=%lu\n",
         cdc_connected ? 1u : 0u,
         (unsigned long)((time_us_64() - wait_start_us) / 1000ull));
  fflush(stdout);
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_CRASH_RECORD)
  if (crash_recovered) {
    shizuku::objects::usb_cdc_boot_diag_crash_report_drain();
#if defined(SHIZUKU_SELFTEST_PROGRESS) && SHIZUKU_SELFTEST_PROGRESS > 0 && !defined(SHIZUKU_BOOT_STAGE_TRACE)
    if (st_fault_valid && st_fault[18] == 0x53545047u) {
      printf("[SELFTEST_PROG] noinit mark stage=%lu run=%lu event=%lu value=%08lx\n",
             (unsigned long)st_fault[19],
             (unsigned long)(st_fault[20] >> 8),
             (unsigned long)(st_fault[20] & 0xffu),
             (unsigned long)st_fault[21]);
      printf("[SELFTEST_PROG] tick sample pc=%08lx lr=%08lx xpsr=%08lx hits=%lu stage=%lu\n",
             (unsigned long)st_fault[12], (unsigned long)st_fault[13],
             (unsigned long)st_fault[14], (unsigned long)st_fault[15],
             (unsigned long)st_fault[16]);
      printf("[SELFTEST_PROG] tick psp=%08lx control=%lu frame=%08lx frame_uses_psp=%lu\n",
             (unsigned long)st_fault[22], (unsigned long)st_fault[23],
             (unsigned long)st_fault[24], (unsigned long)st_fault[25]);
      fflush(stdout);
    }
#endif
    watchdog_hw->scratch[0] = 0;
  }
#endif
  shizuku::objects::usb_cdc_boot_diag_set_stage(
      shizuku::objects::boot_diag_stage::KERNEL_INIT);
#else
  sleep_ms(1000); // ホストが CDC を開く前の出力を落とさないための待ち
#endif
  shizuku::kernel_instance.init();
  // 系の組み立て: カーネルオブジェクトの表を用意し、そのハンドラをカーネルへ据える。
  // これ以降、オブジェクトが撃った svc はすべてそのハンドラへ届く。
  shizuku::kernel_object_instance.init();
#if defined(SHIZUKU_BOOT_LOG_WAIT)
  shizuku::objects::usb_cdc_boot_diag_set_stage(
      shizuku::objects::boot_diag_stage::OBJECT_TABLE_INIT);
#endif
  shizuku::kernel_instance.set_object_handler(
      shizuku::KERNEL_OBJECT::handler_entry(),
      (uint32_t)shizuku::KERNEL_OBJECT::KERNEL_OBJECT_ID);
#if defined(SHIZUKU_BOOT_LOG_WAIT)
  shizuku::objects::usb_cdc_boot_diag_set_stage(
      shizuku::objects::boot_diag_stage::BOOTSTRAP);
#endif
  // 今の実行をスレッド 0 として採用し、スレッドスタックへ移って app_entry へ。
  // ★最初の 1 本のスタックもオブジェクトランドから借りる (他のスレッドと同じ扱い)。
  const auto boot = shizuku::kernel_object_instance.lend_boot_stack();
  shizuku::kernel_instance.bootstrap(shizuku::app_entry, boot.base, boot.bytes, boot.ledger,
                                     boot.ledger_capacity);
#endif
}
