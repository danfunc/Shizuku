#ifndef SHIZUKU_CHIPS_ESP32_C6_CHIP_HPP
#define SHIZUKU_CHIPS_ESP32_C6_CHIP_HPP
// ESP32-C6 (SoC) の型の入口。config.hpp の CHIP 別名がこれを指す (configs/kit.bzl)。
// ★ESP-IDF / pico-sdk を一切 include しない (chips の依存閉包はそれを含まない)。
// ★fail-closed gate (SHIZUKU_C6_SKELETON_PERMITTED) はここに置かない。rv32_c6.hpp の互換入口に残す。
#include <cstdint>

// hot 経路 (CALL_METHOD_DESCRIPTOR とその RETURN) を IRAM へ置くための配置マクロ。
// kernel 側の既定は shizuku/hot.hpp の空定義で、この chip 以外は変わらない。
// 関数ごとに ".iram1.<N>" と一意の section 名にする (IDF の IRAM_ATTR と同じ方式)。
// 同名 section に COMDAT のテンプレートと通常関数を混ぜると GCC が section type conflict で止まる。
// リンカ側は ESP-IDF 標準の `*(.iram1 .iram1.*)` で拾う (この checkout 内の ld は未確認)。
// ★arch/riscv/rv32.hpp (経由で rv32_abi.hpp の入口 stub) より**前**に定義する。後だと stub が空のまま展開される。
#define SHIZUKU_CHIP_STR_(x) #x
#define SHIZUKU_CHIP_STR(x) SHIZUKU_CHIP_STR_(x)
#undef SHIZUKU_HOT
#if defined(__riscv)
#define SHIZUKU_HOT __attribute__((section(".iram1." SHIZUKU_CHIP_STR(__COUNTER__))))
#else
#define SHIZUKU_HOT
#endif

#include "arch/riscv/rv32.hpp"

namespace shizuku::chips::esp32_c6 {

// C6 の割込み方式 (PLIC + 256B 整列 32 スロットの vector table)。
// rv32<...>::IntMode としてだけ使う。定義は trap.cpp (本体) と modules/esp_idf_support/c6/link_stubs.cpp (未実装部)。
// 将来 C5 (CLIC 直接エントリ + mtvt 関数ポインタ表) を載せるなら、別の IntMode 型を同じ形で作る。
struct plic_vector_mode {
  static constexpr bool RESTRICT_UNPRIVILEGED_CREATE = true;
  // M-only pilot の policy 値 (従来値のまま。generic arch からは IntMode 経由で参照)。
  static constexpr uint32_t CALL_HEADROOM = 256;
  // M-only パイロット向けサイクルカウンタ (CSR 0x7E0/0x7E1/0x7E2)。U-mode は未検証。
  static constexpr bool HAS_CYCLE_COUNTER = true;
  static constexpr uint32_t TIMER_MAX_CYCLES = 0xFFFFFFFF;
  static constexpr uint32_t TIMER_MIN_CYCLES = 100;

  static bool cycle_counter_enable();
  static uint32_t cycle_counter_read();

  static bool is_kstack_address(uintptr_t addr);
  static uintptr_t kstack_limit();
  static uintptr_t kstack_top();
  static bool user_stack_bounds(uint32_t object_id, uintptr_t &base, uintptr_t &limit);
  static void switch_object_pmp(uint32_t object_id);
  template <class Context, class Frame>
  static void enter_u_object(Context &context, Frame &frame, uint32_t callee_id);

  static bool in_interrupt();
  static bool current_priv();
  static void timer_oneshot(uint32_t cycles);
  static void timer_cancel();
  static uint32_t timer_remaining(bool &wrapped);
  static void pend_context_switch();
  [[noreturn]] static void enter_thread_mode(uintptr_t stack_top,
                                             uintptr_t stack_limit,
                                             void (*entry)());
  static uint32_t debug_reason_take();
  static void debug_step(bool on);
  // 型は rv32<...>::context_t。link_stubs.cpp で明示インスタンス化する。
  template <class Context> static bool faulted_in_thread_mode(const Context &context);
  static uint32_t fault_status();
  static uintptr_t fault_address();
  static void fault_status_clear();
};

struct chip {
  // 単一コア (configs/kit.bzl の rv32_c6 は SHIZUKU_CPU_COUNT=1)
  static constexpr uint32_t kCoreCount = 1;
  // PMP: 物理エントリ 16 本 (pmpcfg0..3 / pmpaddr0..15)、粒度 4B。
  // arch/riscv/pmp.hpp の pmp_codec<kPmpGranuleBytes> と組む (現行動作を変えない)。
  static constexpr uint32_t kPmpEntries = 16;
  static constexpr uint32_t kPmpGranuleBytes = 4;
  // ARCH として注入される型 (configs/kit.bzl の ${SHIZUKU_ARCH_TYPE})。
  using arch_type = shizuku::archs::rv32<kPmpEntries, kPmpGranuleBytes, plic_vector_mode>;
};

} // namespace shizuku::chips::esp32_c6

#endif // SHIZUKU_CHIPS_ESP32_C6_CHIP_HPP
