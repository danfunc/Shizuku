#include "hardware/clocks.h"
#include "hardware/regs/addressmap.h"
#include "hardware/exception.h"
#include "hardware/irq.h"
#include "hardware/structs/scb.h"
#include "pico/stdlib.h"
#include "shizuku/archs/armv8m.hpp"
#include "shizuku/boards/rp2350_pico2.hpp"

// board は「どの ISA の上に居るか」を知っている層でもある (region を張る機構は
// arch 側)。ここだけの別名にして、上位のテンプレート引数とは混ぜない。
using ARCH_TYPE = shizuku::archs::armv8m;

// リンカが置く「静的データの終端 = ヒープの先頭」。名前空間の中で宣言すると
// 名前が飾られて別物になるので、必ずファイル先頭で C リンケージとして宣言する。
extern "C" char __end__[];
#include <cstdarg>
#include <cstdio>

// USB の割り込みハンドラ。フォールト報告を「割り込みが走れない状況でも」外へ
// 出すために、ポーリングで呼べる形で握っておく (board::init が拾う)。
static void (*g_usb_irq_poll)() = nullptr;

namespace shizuku {
namespace boards {

void rp2350_pico2::init(uint32_t core) {
  if (core == 0) {
    // RAM ベクタテーブルは両コア共有: 登録は core0 の 1 回だけ
    // (exclusive 登録は二重登録で panic するため、core1 で再登録しない)。
    exception_set_exclusive_handler(SVCALL_EXCEPTION, shizuku_armv8m_svc_entry);
    exception_set_exclusive_handler(PENDSV_EXCEPTION,
                                    shizuku_armv8m_pendsv_entry);
    exception_set_exclusive_handler(SYSTICK_EXCEPTION,
                                    shizuku_armv8m_systick_entry);
    // ★無言で固まらせないための最後の砦。ここを用意していなかったせいで、
    //   スタック枯渇 → HardFault → USB ごと停止 → 書き込みもできない、という
    //   一番情報の少ない壊れ方をした (PORT §7 が最初に用意しろと書いている項目)。
    exception_set_exclusive_handler(HARDFAULT_EXCEPTION,
                                    shizuku_armv8m_fault_entry);
    // ★スタック下限違反や MPU 違反は本来「設定可能な優先度を持つ例外」なので、
    //   有効にしておけば HardFault へ落ちない。HardFault は優先度 -1 で**あらゆる
    //   割り込みを止める**ため、そこから USB へ何かを出すことが構造的にできない。
    //   有効化して優先度を USB より下に置けば、報告中も USB 割り込みが走れる。
    exception_set_exclusive_handler(MEMMANAGE_EXCEPTION,
                                    shizuku_armv8m_fault_entry);
    exception_set_exclusive_handler(BUSFAULT_EXCEPTION,
                                    shizuku_armv8m_fault_entry);
    exception_set_exclusive_handler(USAGEFAULT_EXCEPTION,
                                    shizuku_armv8m_fault_entry);
  }
  // 優先度は banked なので各コアで設定する。SVC 最優先 = プリミティブの原子性、
  // PendSV 最低 = 全 IRQ が捌けてからのスレッド切替 (DESIGN §14.5.1)。
  // ★この順序が 1 コア内の相互排除を無償で与える (DESIGN §14.5.1)。
  //   syscall 最優先 → その中ではタイマにも切替にも割り込まれない。
  //   切替は最低優先度なので、ハンドラを抜けるまで走らない。
  exception_set_priority(SVCALL_EXCEPTION, 0x00);
  exception_set_priority(SYSTICK_EXCEPTION, 0x40);
  exception_set_priority(PENDSV_EXCEPTION, PICO_LOWEST_IRQ_PRIORITY);
  protection_init(); // region は per-core banked なので各コアで張る

  // ---- 落ちたときに必ず声が出るようにする -------------------------------
  // ★診断が届くかどうかは優先度で決まる。報告している最中に USB の割り込みが
  //   走れなければ、書いた文字はデバイスの中に留まったまま出て行かない。
  //   そこで USB を上げ (0x20)、フォールト例外はそれより下 (0x80) に置く。
  //   こうすると報告中も USB が動けるので、**落ちた事実と場所が必ず外へ出る**。
  irq_set_priority(USBCTRL_IRQ, 0x20);
  // 落ちたときに自分で回せるよう、USB の割り込みハンドラを控えておく。
  g_usb_irq_poll = (void (*)())irq_get_exclusive_handler(USBCTRL_IRQ);
  exception_set_priority(MEMMANAGE_EXCEPTION, 0x80);
  exception_set_priority(BUSFAULT_EXCEPTION, 0x80);
  exception_set_priority(USAGEFAULT_EXCEPTION, 0x80);
  // 設定可能なフォールトを有効化する (無効のままだと全部 HardFault へ落ちて
  // 優先度 -1 になり、上の工夫が効かなくなる)。SHCSR の該当ビット。
  scb_hw->shcsr |= (1u << 16) | (1u << 17) | (1u << 18); // MEM/BUS/USG FAULTENA
}

// ★どこに何を張るかは board の知識 (SoC のメモリ地図を知っているのはここだけ)。
//   狙いは 2 つ:
//     (i)  データを実行させない / コードを書き換えさせない (W^X)
//     (ii) region の外を特権だけに閉じる (PRIVDEFENA) — 単一アドレス空間で
//          「カーネル空間」を作る唯一の方法 (DESIGN §11.1)
//   ★静的データ (.bss/.data) は region の外に落ちる。つまり**非特権オブジェクトは
//     自分のグローバルに触れない**。これは事故ではなく設計で、オブジェクトの状態は
//     ヒープ上 (= region1) に置けという圧力になる (§11.2.2)。
void rp2350_pico2::protection_init() {
  // region0: XIP (フラッシュ窓) 全域 = 読み+実行のみ。フラッシュへの誤書き込みを止める。
  ARCH_TYPE::region_set(0, XIP_BASE, XIP_END - 32u, ARCH_TYPE::ACCESS_RO_ALL,
                        false, 0);
  // region1: ヒープ先頭〜SRAM 終端 = 読み書き可・実行不可。スレッドスタックは
  // ここに住むので、スタック上のデータを実行する事故が止まる。
  const uintptr_t heap = ((uintptr_t)__end__ + 31u) & ~(uintptr_t)31;
  ARCH_TYPE::region_set(1, heap, SRAM_END - 32u, ARCH_TYPE::ACCESS_RW_ALL, true,
                        1);
  for (uint32_t index = 2; index < 8; ++index)
    ARCH_TYPE::region_disable(index);
  ARCH_TYPE::protection_enable();
}

uint32_t rp2350_pico2::cycles_per_us() {
  // 実クロックから毎回引く (クロックを変えても追従する。PORT §2.3)。
  return (uint32_t)(::clock_get_hz(clk_sys) / 1000000u);
}

} // namespace boards
} // namespace shizuku

// 落ちた場所を報告する。例外は MSP で走るので、スレッドのスタックが尽きて落ちた
// 場合でもここでは印字できる (原因がスタック枯渇のときこそ効く)。
extern "C" [[noreturn]] void shizuku_fault_report(const uint32_t *frame,
                                                  uint32_t stack_limit) {
  // CFSR: どの種類の違反か (bit4 = スタック時のフォールト, bit5 = 復帰時)。
  const uint32_t configurable_fault = *(volatile uint32_t *)0xE000ED28u;
  const uint32_t hard_fault = *(volatile uint32_t *)0xE000ED2Cu;
  ::printf("\n[FAULT] pc=%08lx lr=%08lx psr=%08lx sp=%08lx psplim=%08lx\n"
           "[FAULT] cfsr=%08lx hfsr=%08lx%s\n",
           (unsigned long)frame[6], (unsigned long)frame[5],
           (unsigned long)frame[7], (unsigned long)(uintptr_t)frame,
           (unsigned long)stack_limit, (unsigned long)configurable_fault,
           (unsigned long)hard_fault,
           ((uintptr_t)frame <= stack_limit + 64) ? " (スタック下限に接触)" : "");
  // ★HardFault (優先度 -1) まで落ちてしまった場合、USB の割り込みは走れない。
  //   それでも報告を届けるため、USB の割り込みハンドラを**自分で呼ぶ**。
  //   割り込みハンドラはただの関数なので、ポーリングで回せば送信は進む。
  //   これが無いと「書いたのに出ない」= 一番情報の少ない壊れ方に戻る。
  //   (設定可能なフォールトとして入って来た場合は優先度で USB が走れるので、
  //    この空回しは HardFault へ落ちたときの保険になる。)
  while (true)
    if (g_usb_irq_poll != nullptr)
      g_usb_irq_poll();
}

namespace shizuku {
namespace boards {

void rp2350_pico2::diag_printf(const char *format, ...) {
  va_list args;
  va_start(args, format);
  ::vprintf(format, args);
  va_end(args);
}

void rp2350_pico2::panic(const char *message) {
  ::panic("%s", message);
}

} // namespace boards
} // namespace shizuku
