// サンプル: RV32 (M/U モード + PMP) の arch 層。RISC-V の特権仕様で決まる操作だけを持つ。
// 名前は armv8m.hpp / armv6m.hpp とそろえてある (cpu_manager が同じ書き方で呼べるように)。
// ★割込みコントローラ (C6 の割込み行列 + PLIC、C5 の CLIC) とタイマ (SYSTIMER) は
//   RISC-V の仕様の外なので chip が持つ。ARM で NVIC/SysTick が arch にあるのと非対称に
//   見えるが、境界は「ISA の仕様書に書いてあるか」で引く。
#ifndef SHIZUKU_SAMPLE_ARCH_RV32_HPP
#define SHIZUKU_SAMPLE_ARCH_RV32_HPP
#include <stdint.h>

extern "C" void sample_rv32_trap_entry();
extern "C" volatile uint32_t sample_rv32_pend;

namespace sample::arch {

// PMP_ENTRIES / PMP_GRANULE は chip が渡す (C6 は 16 本・4B、C5 は 128B)。
template <unsigned PMP_ENTRIES_T, unsigned PMP_GRANULE_T> struct rv32 {
  static constexpr unsigned PMP_ENTRIES = PMP_ENTRIES_T;
  static constexpr unsigned PMP_GRANULE = PMP_GRANULE_T;
  // region 1 個 = TOR の entry 2 本 (下端と上端)。
  static constexpr unsigned MPU_REGIONS = PMP_ENTRIES / 2;
  static constexpr bool HAS_PRIORITY_REGISTERS = false; // 例外の優先度は CPU に無い (PLIC は chip)
  static constexpr bool HAS_FAULT_PRIORITY = false;
  static constexpr bool HAS_STACK_LIMIT = false;
  static constexpr bool HAS_CYCLE_COUNTER = false;      // 標準の mcycle は C6 に無い。chip の CSR を使う

  // 文脈ブロック (rv32_entry.S の CTX_* と同じ並び)。カーネルのメモリに置く。
  struct context_t {
    uint32_t x[31]; // x1..x31 (x[1] = sp)
    uint32_t mepc;
    uint32_t mstatus;
  };
  static_assert(sizeof(context_t) == 33 * 4);

  static constexpr uint32_t MSTATUS_MPIE = 1u << 7;
  static constexpr uint32_t MSTATUS_MPP = 3u << 11; // 11 = M, 00 = U

  // direct mode (下位 2bit = 0)。ベクタ化は chip の選択 (C6 の PLIC vector mode など)。
  static void install_vectors() {
    asm volatile("csrw mtvec, %0" ::"r"(reinterpret_cast<uintptr_t>(&sample_rv32_trap_entry))
                 : "memory");
  }
  // 「切替え要求」はソフトの旗。trap 出口が見る (ARM の PENDSVSET に相当)。
  static void pend_switch() { sample_rv32_pend = 1; }

  // 戻り先の特権は mstatus.MPP。ARM と違って CPU のレジスタではなく文脈の値なので、
  // 文脈を書き換えるだけで、trap 出口の mret が効かせる。
  static void apply_privilege(context_t &context, bool privileged) {
    context.mstatus = (context.mstatus & ~MSTATUS_MPP) | (privileged ? MSTATUS_MPP : 0u);
    context.mstatus |= MSTATUS_MPIE; // スレッドは割込み許可で走る
  }
  static void set_stack_limit(uintptr_t) {}

  // ---- PMP (TOR) -------------------------------------------------------------
  // cfg 1 本 = 8bit: L(7) A(4:3) X(2) W(1) R(0)。A = 1 が TOR。
  // ★M モードは L を立てない限り PMP の制限を受けない (Arm の PRIVDEFENA に相当する
  //   「特権は全部見える」が既定)。U モードは一致する entry が無いアクセスを全部拒否する。
  enum access : uint8_t { RO_ALL = 0b001, RW_ALL = 0b011 };
  static bool region_set(uint32_t index, uintptr_t base, uintptr_t end_exclusive, access ap,
                         bool execute) {
    if ((base | end_exclusive) & (PMP_GRANULE - 1))
      return false; // 粒度に乗らない範囲は張らない (黙って広げない)
    const unsigned low = 2 * index, high = 2 * index + 1;
    write_pmpaddr(low, uint32_t(base >> 2));
    write_pmpaddr(high, uint32_t(end_exclusive >> 2));
    write_pmpcfg(low, 0); // 下端用は OFF
    write_pmpcfg(high, uint8_t(ap | (execute ? 0b100u : 0u) | (1u << 3)));
    return true;
  }
  static void region_disable(uint32_t index) { write_pmpcfg(2 * index + 1, 0); }
  static void protection_enable() {} // PMP は常に有効。U モードは既定で全部拒否

  static void write_pmpaddr(unsigned index, uint32_t value); // csrw pmpaddrN (N は即値なので表で引く)
  static void write_pmpcfg(unsigned index, uint8_t cfg);     // pmpcfg(N/4) の該当バイトを書き換える

  // 初回もふつうの trap 出口 (全部戻して mret) と同じ道で入る。
  [[noreturn]] static void enter_thread_mode(context_t &context) {
    asm volatile("csrw mscratch, %0\n mv sp, %0\n j sample_rv32_resume_first" ::"r"(&context));
    __builtin_unreachable();
  }
};

} // namespace sample::arch
#endif
