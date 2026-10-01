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
#include "shizuku/objects/usb_cdc.hpp"
#include "shizuku/objects/peripherals.hpp"
#include "shizuku/apps/thermal.hpp"
#include "shizuku/selftest.hpp"
#include "stdio.h"
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_BOOT_STAGE_TRACE)
#include "hardware/watchdog.h"
#include "hardware/structs/watchdog.h"
#include "pico/bootrom.h"
#endif
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_BOOT_STAGE_TRACE) && SHIZUKU_BOOT_STAGE_TRACE > 0
extern "C" bool shizuku_boot_fault_registers(uint32_t *);
extern "C" void shizuku_boot_trace_stack(uint32_t,uint32_t);
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
  kThermal = 210,
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
void shizuku::app_entry() {
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_BOOT_STAGE_TRACE) && SHIZUKU_BOOT_STAGE_TRACE > 0
  boot_stage(kAppEntry);
#endif
  // ★スレッドが落ちたときに実行権を渡す先を教えておく。誰に渡すかは方針なので
  //   カーネルは選ばない。ここではブートスレッド (アイドル役) を指定する。
  shizuku::kernel_instance.set_recovery_thread(0);

  // ボードが提供するペリフェラルオブジェクト (特権を宣言する数少ないオブジェクト)。
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_BOOT_STAGE_TRACE) && SHIZUKU_BOOT_STAGE_TRACE > 0
  boot_stage(kPeripherals);
#endif
  const uint32_t peripheral_failures = shizuku::objects::register_peripherals();
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_BOOT_STAGE_TRACE) && SHIZUKU_BOOT_STAGE_TRACE > 0
  boot_stage(kFlashRegistration);
#endif
  // 媒体を持つオブジェクト。読むのは XIP のアドレスを配るだけなので安いが、
  // 書くと XIP ごと止まるので、扱いはペリフェラルと同じく特権側。
  shizuku::objects::register_flash_fs();
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_BOOT_STAGE_TRACE) && SHIZUKU_BOOT_STAGE_TRACE > 0
  boot_stage(kCallLadder);
#endif
  shizuku::selftest::call_ladder();
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_BOOT_STAGE_TRACE) && SHIZUKU_BOOT_STAGE_TRACE > 0
  boot_stage(kHandlerBinding);
#endif
  shizuku::selftest::handler_binding_probe();
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_BOOT_STAGE_TRACE) && SHIZUKU_BOOT_STAGE_TRACE > 0
  boot_stage(kCallCost);
#endif
  shizuku::selftest::call_cost();
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_BOOT_STAGE_TRACE) && SHIZUKU_BOOT_STAGE_TRACE > 0
  boot_stage(kThreadLadder);
#endif
  shizuku::selftest::thread_ladder();
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
  if (shizuku::kernel_object_instance.start_secondary_core())
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
  shizuku::objects::flash_fs_probe();
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_BOOT_STAGE_TRACE) && SHIZUKU_BOOT_STAGE_TRACE > 0
  boot_stage(kFlashStream);
#endif
  shizuku::selftest::flash_stream_ladder();
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_BOOT_STAGE_TRACE) && SHIZUKU_BOOT_STAGE_TRACE > 0
  boot_stage(kThermal);
#endif
#if !defined(SHIZUKU_RP2040)
  shizuku::selftest::debug_ladder();
#endif

  // 温度の履歴アプリ。★負荷試験より前に起こして、負荷の下で周期がどれだけ
  //   揺らぐかを見る (静かな系で測っても揺らぎの話にならない)。
  shizuku::apps::start_thermal();

  // 負荷試験を起動する。以後、点滅と報告は専用スレッドが行う。
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_BOOT_STAGE_TRACE) && SHIZUKU_BOOT_STAGE_TRACE > 0
  boot_stage(kStress);
#endif
  const uint32_t blink_thread = shizuku::selftest::stress_launch();
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_BOOT_STAGE_TRACE) && SHIZUKU_BOOT_STAGE_TRACE > 0
  boot_stage(kBootComplete);
  for (uint32_t second = 0; second < 40; ++second) {
    watchdog_update();
    sleep_ms(1000);
  }
  // Leave the final stage recorded and stop feeding: recovery reports completion,
  // then enters BOOTSEL so a diagnostic image cannot strand the board.
#endif

#if !defined(SHIZUKU_RP2040)
  // ★GDB stub。繋がれるまでは何もしない (繋がれた瞬間に診断出力を GDB へ譲る)。
  // ★★2026-08-24 デモ用: 合成の debuggee ではなく実在の blink (LED を叩く
  //   スレッド) を対象にする。attach するだけで LED が目に見えて止まり、
  //   detach で再開する — DebugMonitor が「そのスレッドだけ」止めることの
  //   一番分かりやすい確認 (docs/03_porting_policy.md D43)。
  shizuku::objects::start_gdb_stub(blink_thread);
#endif

  // ★スレッド 0 は以後アイドル役に徹する。誰かが走れるなら渡し、誰も居なければ
  //   空回りするだけ。**ここで自分が仕事をしてはいけない** — アイドルが仕事を
  //   持つと、その仕事が他の全部の遅れになる。
  while (true)
    shizuku::KERNEL::ARCH::syscall((uintptr_t)shizuku::object_api::YIELD);
}

int main() {
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
  shizuku::kernel_instance.bootstrap(shizuku::app_entry, boot.base, boot.bytes);
#endif
#else
  // ★USB は自前で持つ (CDC 2 本: 診断と GDB)。pico_stdio_usb は 1 本前提で、
  //   記述子も差し替えられないため (D42)。
  shizuku::objects::usb_cdc_init();
  sleep_ms(1000); // ホストが CDC を開く前の出力を落とさないための待ち
  shizuku::kernel_instance.init();
  // 系の組み立て: カーネルオブジェクトの表を用意し、そのハンドラをカーネルへ据える。
  // これ以降、オブジェクトが撃った svc はすべてそのハンドラへ届く。
  shizuku::kernel_object_instance.init();
  shizuku::kernel_instance.set_object_handler(
      shizuku::KERNEL_OBJECT::handler_entry(),
      (uint32_t)shizuku::KERNEL_OBJECT::KERNEL_OBJECT_ID);
  // 今の実行をスレッド 0 として採用し、スレッドスタックへ移って app_entry へ。
  // ★最初の 1 本のスタックもオブジェクトランドから借りる (他のスレッドと同じ扱い)。
  const auto boot = shizuku::kernel_object_instance.lend_boot_stack();
  shizuku::kernel_instance.bootstrap(shizuku::app_entry, boot.base, boot.bytes);
#endif
}
