#include "hardware/exception.h"
#include "pico/stdlib.h"
#include "shizuku/abis/rp2040_abi.hpp"
#include "shizuku/archs/armv8m.hpp"
#include "shizuku/boards/rp2350_pico2.hpp"
#include "stdio.h"

using shizuku::archs::armv8m;
using shizuku::boards::rp2350_pico2;

namespace shizuku {
namespace boards {

void rp2350_pico2::init(uint32_t core) {
  if (core == 0) {
    // RAM ベクタテーブルは両コア共有: 登録は core0 の 1 回だけ
    // (exclusive 登録は二重登録で panic するため、core1 で再登録しない)。
    exception_set_exclusive_handler(SVCALL_EXCEPTION,
                                    shizuku_armv8m_svc_entry);
    exception_set_exclusive_handler(PENDSV_EXCEPTION,
                                    shizuku_armv8m_pendsv_entry);
  }
  // 優先度は banked なので各コアで設定する。SVC 最優先 = プリミティブの原子性、
  // PendSV 最低 = 全 IRQ が捌けてからのスレッド切替 (DESIGN §14.5.1)。
  exception_set_priority(SVCALL_EXCEPTION, 0x00);
  exception_set_priority(PENDSV_EXCEPTION, PICO_LOWEST_IRQ_PRIORITY);
}

} // namespace boards
} // namespace shizuku

// ---------------------------------------------------------------------------
//  armv8m_ctx.S から呼ばれる現在文脈フック (Phase 1 の暫定実装)。
//  Phase 2 でスレッド表を持つカーネルテンプレート側へ移す。ここでは
//  「コアごとの現在文脈ポインタ + ブート文脈」だけを提供する。
// ---------------------------------------------------------------------------
namespace {
armv8m::context_t g_boot_context[rp2350_pico2::CORE_COUNT];
armv8m::context_t *g_current_context[rp2350_pico2::CORE_COUNT] = {
    &g_boot_context[0], &g_boot_context[1]};
} // namespace

extern "C" shizuku_armv8m_context *shizuku_current_context() {
  return reinterpret_cast<shizuku_armv8m_context *>(
      g_current_context[rp2350_pico2::core_num()]);
}

extern "C" void shizuku_svc_dispatch(shizuku_armv8m_context *opaque) {
  armv8m::context_t *context = reinterpret_cast<armv8m::context_t *>(opaque);
  armv8m::exception_frame_t *frame = context->sp;
  // svc 命令 (2 byte) の即値。ARM の例外エントリは戻り先 pc をフレームへ積むので、
  // その 2 byte 手前が発行された svc 命令になる。
  const uint32_t svc_num = *(const uint16_t *)((uintptr_t)frame->pc - 2) & 0xffu;
#ifdef SHIZUKU_DEBUG
  printf("svc handled num:%lu\n", (unsigned long)svc_num);
#endif
  switch (svc_num) {
  case shizuku::abis::INIT_INVOKE: {
    // r0 = 実行を移す先の context_t。現在文脈を差し替えるだけで、CTX_RESTORE が
    // その文脈へ復帰する (= 最初のスレッド起動)。
    g_current_context[rp2350_pico2::core_num()] =
        (armv8m::context_t *)frame->r0;
    break;
  }
  default:
    break;
  }
}

extern "C" void shizuku_pendsv_dispatch(shizuku_armv8m_context *opaque) {
  (void)opaque; // 時限実行権 (GRANT) の期限回収は Phase 2 で実装
}
