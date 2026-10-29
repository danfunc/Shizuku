// サンプル: ARMv8-M Mainline の arch 層。ISA (と Arm が決めたシステム領域) の操作だけを持つ。
// ★ここに無いもの: コア数、優先度の値、MPU region の本数、メモリ地図 (chip が持つ)。
//                  どこに何を張るか、クロック、診断出力 (board と cpu_manager が持つ)。
// ★SDK を include しない。システム制御空間 (0xE000E000-) は Arm の仕様で全チップ共通。
#ifndef SHIZUKU_SAMPLE_ARCH_ARMV8M_HPP
#define SHIZUKU_SAMPLE_ARCH_ARMV8M_HPP
#include <stdint.h>
#include "../common/kernel_entry.hpp"

extern "C" void sample_armv8m_svc_entry();
extern "C" void sample_armv8m_pendsv_entry();
extern "C" void sample_armv8m_fault_entry();
extern "C" [[noreturn]] void sample_armv8m_enter_thread_mode(uintptr_t top, uintptr_t limit,
                                                             void (*entry)());

namespace sample::arch {

template <unsigned MPU_REGIONS_T, unsigned PRIORITY_BITS_T> struct armv8m {
  static constexpr unsigned MPU_REGIONS = MPU_REGIONS_T;
  static constexpr unsigned PRIORITY_BITS = PRIORITY_BITS_T;
  static constexpr uint32_t MPU_GRANULE = 32;
  static constexpr bool HAS_CYCLE_COUNTER = true;
  static constexpr bool HAS_STACK_LIMIT = true;   // PSPLIM
  static constexpr bool HAS_FAULT_PRIORITY = true; // MemManage/BusFault/UsageFault を個別に持つ
  static constexpr bool HAS_PRIORITY_REGISTERS = true;

  // ハードウェアが積む例外フレーム。FP 使用時はこの後ろに s0-s15, FPSCR, 予約が続く (計 104B)。
  struct exception_frame_t {
    uint32_t r0, r1, r2, r3, r12, lr, pc, xpsr;
  };
  // カーネル側に置く文脈は 2 語だけ。r4-r11 (と s16-s31) は PendSV がスレッドのスタックへ積む。
  struct context_t {
    uintptr_t sp;        // r4-r11 を積んだ後の PSP
    uint32_t exc_return; // スタックに置かない (非特権スレッドに偽造させない)
  };

  // ---- 例外番号 (Arm の定義) ----
  enum exception : uint32_t {
    HARD_FAULT = 3, MEM_MANAGE = 4, BUS_FAULT = 5, USAGE_FAULT = 6,
    SVCALL = 11, DEBUG_MONITOR = 12, PENDSV = 14, SYSTICK = 15,
  };

  // ---- ベクタと優先度 ------------------------------------------------------
  static volatile uint32_t &reg(uintptr_t address) {
    return *reinterpret_cast<volatile uint32_t *>(address);
  }
  // RAM 上のベクタ表 (VTOR が指す) へカーネルの入口を書く。表を RAM へ置くのは board/起動コードの責任。
  static void install_vectors() {
    auto *table = reinterpret_cast<void (**)()>(reg(0xE000ED08)); // VTOR
    table[SVCALL] = sample_armv8m_svc_entry;
    table[PENDSV] = sample_armv8m_pendsv_entry;
    table[SYSTICK] = shizuku_tick; // 例外入口は AAPCS 互換なので C を直接置ける
    table[HARD_FAULT] = sample_armv8m_fault_entry;
    table[MEM_MANAGE] = sample_armv8m_fault_entry;
    table[BUS_FAULT] = sample_armv8m_fault_entry;
    table[USAGE_FAULT] = sample_armv8m_fault_entry;
  }
  // システム例外 4..15 の優先度は SHPR1-3 にバイト単位で並ぶ (v8-M はバイトアクセス可)。
  static void set_priority(exception number, uint8_t priority) {
    reinterpret_cast<volatile uint8_t *>(0xE000ED18)[number - 4] = priority;
  }
  static uint8_t priority(exception number) {
    return reinterpret_cast<volatile uint8_t *>(0xE000ED18)[number - 4];
  }
  // 外部割込み (NVIC) の優先度。IPR もバイトアクセス可。
  static void set_irq_priority(uint32_t irq, uint8_t priority) {
    reinterpret_cast<volatile uint8_t *>(0xE000E400)[irq] = priority;
  }
  // MemManage/BusFault/UsageFault を有効化する (無効のままだと HardFault へ昇格し、優先度 -1 になる)。
  static void enable_fault_exceptions() {
    reg(0xE000ED24) |= (1u << 16) | (1u << 17) | (1u << 18); // SHCSR
  }

  // ---- 切替え・タイマ -------------------------------------------------------
  static void pend_switch() { reg(0xE000ED04) = 1u << 28; } // ICSR.PENDSVSET
  static constexpr uint32_t TIMER_MAX = 0x00FFFFFF;           // SysTick は 24bit
  static void timer_oneshot(uint32_t cycles) {
    reg(0xE000E010) = 0;          // CSR: 止める
    reg(0xE000E014) = cycles - 1; // RVR
    reg(0xE000E018) = 0;          // CVR: 書くと 0 になり、次の刻みで RVR を読み込む
    reg(0xE000E010) = 0b111;      // CLKSOURCE=CPU, TICKINT, ENABLE
  }
  static void timer_cancel() { reg(0xE000E010) = 0; }

  // ---- サイクルカウンタ (DWT) ------------------------------------------------
  static bool cycle_counter_enable() {
    reg(0xE000EDFC) |= 1u << 24; // DEMCR.TRCENA
    reg(0xE0001000) |= 1u;       // DWT_CTRL.CYCCNTENA
    return (reg(0xE0001000) & 1u) != 0;
  }
  static uint32_t cycle_counter() { return reg(0xE0001004); }

  // ---- 特権と FP の状態 (今走っているコードの自己申告) -------------------------
  static uint32_t control() {
    uint32_t value;
    asm volatile("mrs %0, control" : "=r"(value));
    return value;
  }
  static bool privileged() { return (control() & 1u) == 0; }
  // CONTROL.FPCA: このスレッドが FP を触ったか。converter の FP 消去はこれで判定する
  // (コンパイルオプションではなく実行時に読む。2026-10-27 決定)。
  static bool fp_active() { return (control() & 4u) != 0; }
  // 例外の中から「戻り先の特権」を変える。復帰した時点で効く。
  // ARM では特権は CPU のレジスタ (CONTROL.nPRIV) なので、文脈ではなくレジスタを書く。
  static void apply_privilege(context_t &, bool privileged) {
    uint32_t value = control();
    value = privileged ? (value & ~1u) : (value | 1u);
    asm volatile("msr control, %0\n isb" ::"r"(value) : "memory");
  }
  static void set_stack_limit(uintptr_t limit) {
    asm volatile("msr psplim, %0" ::"r"(limit));
  }

  // ---- MPU (PMSAv8) ---------------------------------------------------------
  // region は重ねられない。base/limit は 32B 粒度で limit は「含む側の最後のブロック」。
  enum access : uint32_t { RW_PRIV = 0b00, RW_ALL = 0b01, RO_PRIV = 0b10, RO_ALL = 0b11 };
  // 32B 粒度なので前後に最大 31B はみ出す。はみ出しを許さない配置は呼ぶ側 (割当て) の責任。
  // armv6m と同じ形にするため bool を返す (v8-M は常に張れる)。
  static bool region_set(uint32_t index, uintptr_t base, uintptr_t end_exclusive, access ap,
                         bool execute) {
    reg(0xE000ED98) = index;                                                  // RNR
    reg(0xE000ED9C) = (base & ~31u) | (ap << 1) | (execute ? 0u : 1u);        // RBAR
    reg(0xE000EDA0) = ((end_exclusive - 1) & ~31u) | 1u;                      // RLAR (attr0)
    return true;
  }
  static void region_disable(uint32_t index) {
    reg(0xE000ED98) = index;
    reg(0xE000EDA0) = 0;
  }
  // PRIVDEFENA: region の外は特権だけ。単一アドレス空間の「カーネル空間」はこれで作る。
  static void protection_enable() {
    reg(0xE000EDC0) = 0xFF;         // MAIR0: attr0 = 通常メモリ (WB)
    reg(0xE000ED94) = 0b101;        // MPU_CTRL: PRIVDEFENA | ENABLE
    asm volatile("dsb\n isb" ::: "memory");
  }

  [[noreturn]] static void enter_thread_mode(uintptr_t top, uintptr_t limit, void (*entry)()) {
    sample_armv8m_enter_thread_mode(top, limit, entry);
  }
};

} // namespace sample::arch
#endif
