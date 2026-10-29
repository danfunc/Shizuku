// サンプル: cpu_manager = 1 コアの一生 (起動・例外の結線・保護の土台・スレッドモードへの移行・
// 切替え直前の準備) を、arch / chip / board を組み合わせて 1 か所に書く。
//
// 今のコードでこれに当たる処理は次に散っている:
//   boards/rp2350_pico2/board.cpp:47-124   ベクタ登録・優先度・SHCSR・DebugMon の SHPR 直書き
//   boards/rp2040_pico_w/board_rp2040.cpp:155-174  同じことの v6-M 版
//   boards/*/protection_init                region0 + 残り無効 + 有効化 (2 board でほぼ同文)
//   modules/pico_sdk_support/*/arch_glue*.cpp  dispatch の転送と restore_region_window (同文 2 本)
//   chips/esp32_c6/trap.cpp                 install_traps / interrupt_init / enter_thread_mode
// どれも SDK にも基板にも依存しない。違いは ARCH と CHIP の型が埋める。#ifdef は使わない。
#ifndef SHIZUKU_SAMPLE_CPU_MANAGER_HPP
#define SHIZUKU_SAMPLE_CPU_MANAGER_HPP
#include <stdint.h>

namespace sample {

template <class BOARD, class DIAG> struct cpu_manager {
  using CHIP = typename BOARD::chip;
  using ARCH = typename CHIP::arch;

  // ---- カーネルの不変条件はコンパイル時に縛る -------------------------------
  // SVC > タイマ > 切替え (ARM は数字が小さいほど高い)。これが 1 コア内の相互排除をただで与える。
  // 移植で静かに崩れやすいので、board の初期化コードではなくここで型として止める。
  // RISC-V は trap 中 MIE=0 で、切替えは trap 出口の 1 か所なので、順序は構造で成り立つ。
  static constexpr bool priority_order_ok() {
    if constexpr (ARCH::HAS_PRIORITY_REGISTERS) {
      using P = typename CHIP::priority;
      return P::SVC < P::TIMER && P::TIMER < P::SWITCH;
    } else {
      return true;
    }
  }
  static_assert(priority_order_ok(), "priority order must be SVC > TIMER > SWITCH");

  // 呼んだコアを起動する。優先度・MPU・SysTick はコアごとに別 (banked) なので、
  // 各コアが自分で呼ぶ。ベクタ表は共有なので登録は core 0 だけ。
  static void init_core(uint32_t core) {
    if (core == 0)
      ARCH::install_vectors();
    if constexpr (ARCH::HAS_PRIORITY_REGISTERS) {
      using P = typename CHIP::priority;
      ARCH::set_priority(ARCH::SVCALL, P::SVC);
      ARCH::set_priority(ARCH::SYSTICK, P::TIMER);
      ARCH::set_priority(ARCH::PENDSV, P::SWITCH);
      if constexpr (ARCH::HAS_FAULT_PRIORITY) {
        ARCH::set_priority(ARCH::MEM_MANAGE, P::FAULT);
        ARCH::set_priority(ARCH::BUS_FAULT, P::FAULT);
        ARCH::set_priority(ARCH::USAGE_FAULT, P::FAULT);
        ARCH::set_priority(ARCH::DEBUG_MONITOR, P::DEBUG);
        ARCH::enable_fault_exceptions();
      }
    } else {
      CHIP::interrupt_init(); // RISC-V: 割込み行列・PLIC/CLIC は chip の仕事
    }
    BOARD::template soc_irq_init<ARCH>(core);
    protection_baseline();
    cycle_counter_enable();
    DIAG::stage(1);
  }

  // 自己テスト用: レジスタから読み戻して順序を確かめる (static_assert は値の検算、
  // こちらは「書けたか」の検算)。
  static bool priorities_ok() {
    if constexpr (ARCH::HAS_PRIORITY_REGISTERS)
      return ARCH::priority(ARCH::SVCALL) < ARCH::priority(ARCH::SYSTICK) &&
             ARCH::priority(ARCH::SYSTICK) < ARCH::priority(ARCH::PENDSV);
    else
      return true;
  }

  // 保護の土台: region0 = ファームのコード (全員 読み+実行)、ほかは全部閉じる。
  // region の外は特権だけ (ARM は PRIVDEFENA、RISC-V は M モードが PMP を受けない)。
  // オブジェクトごとの窓は切替え時 (prepare_resume) に張る。
  static void protection_baseline() {
    if (!ARCH::region_set(0, BOARD::code_begin(), BOARD::code_end(), ARCH::RO_ALL, true))
      DIAG::stage(0xE001); // v6-M の 2 の冪 / PMP の粒度に乗らない。黙って広げない
    for (uint32_t index = 1; index < ARCH::MPU_REGIONS; ++index)
      ARCH::region_disable(index);
    ARCH::protection_enable();
  }

  // 切替えの直前 (shizuku_switch の最後) に 1 回だけ呼ぶ。次に走るスレッドの
  // 特権・スタック下限・窓を、ここでまとめて効かせる。
  // ★今は asm の CTX_RESTORE が pending_step / restore_region_window / current_context を
  //   別々に bl している。次が決まった時点で C が 1 回で済ませれば、asm は値を戻すだけになる。
  template <class THREAD> static void prepare_resume(THREAD &next) {
    ARCH::apply_privilege(next.context, next.privileged);
    if constexpr (ARCH::HAS_STACK_LIMIT)
      ARCH::set_stack_limit(next.stack_limit);
    for (const auto &window : next.windows) {
      if (window.end == 0)
        ARCH::region_disable(window.region);
      else
        ARCH::region_set(window.region, window.begin, window.end, window.access, false);
    }
  }

  template <class... ARGS> [[noreturn]] static void start(ARGS &...args) {
    ARCH::enter_thread_mode(args...);
  }

private:
  // サイクルカウンタは ISA にある (DWT) こともチップにある (C6 の mpccr) こともある。
  static void cycle_counter_enable() {
    if constexpr (requires { CHIP::cycle_counter_enable(); })
      CHIP::cycle_counter_enable();
    else if constexpr (ARCH::HAS_CYCLE_COUNTER)
      ARCH::cycle_counter_enable();
  }
};

} // namespace sample
#endif
