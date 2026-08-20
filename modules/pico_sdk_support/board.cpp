#include "hardware/clocks.h"
#include "hardware/exception.h"
#include "pico/stdlib.h"
#include "shizuku/archs/armv8m.hpp"
#include "shizuku/boards/rp2350_pico2.hpp"
#include <cstdarg>
#include <cstdio>

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
  }
  // 優先度は banked なので各コアで設定する。SVC 最優先 = プリミティブの原子性、
  // PendSV 最低 = 全 IRQ が捌けてからのスレッド切替 (DESIGN §14.5.1)。
  // ★この順序が 1 コア内の相互排除を無償で与える (DESIGN §14.5.1)。
  //   syscall 最優先 → その中ではタイマにも切替にも割り込まれない。
  //   切替は最低優先度なので、ハンドラを抜けるまで走らない。
  exception_set_priority(SVCALL_EXCEPTION, 0x00);
  exception_set_priority(SYSTICK_EXCEPTION, 0x40);
  exception_set_priority(PENDSV_EXCEPTION, PICO_LOWEST_IRQ_PRIORITY);
}

uint32_t rp2350_pico2::cycles_per_us() {
  // 実クロックから毎回引く (クロックを変えても追従する。PORT §2.3)。
  return (uint32_t)(::clock_get_hz(clk_sys) / 1000000u);
}

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
