// ISA 層 (armv8m_ctx.S) とカーネルの継ぎ目。asm が呼ぶ extern "C" のフックを、
// 構成で確定したカーネル実体へ転送するだけの薄い層。ここがモジュール側にあるのは、
// フックの名前が ISA 固有だから (カーネル本体は ISA 非依存に保つ)。
#include "shizuku/archs/armv8m.hpp"
#include "shizuku/kernel.hpp"
#if defined(SHIZUKU_SELFTEST_PROGRESS) && SHIZUKU_SELFTEST_PROGRESS > 0
#include "hardware/structs/watchdog.h"
#include "hardware/watchdog.h"
#include "pico/platform.h"
#include "pico/time.h"
#endif

extern "C" shizuku_armv8m_context *shizuku_current_context() {
  return reinterpret_cast<shizuku_armv8m_context *>(
      shizuku::kernel_instance.current_context());
}

extern "C" void shizuku_svc_dispatch(shizuku_armv8m_context *context) {
  shizuku::kernel_instance.svc_dispatch(
      reinterpret_cast<shizuku::KERNEL::CONTEXT *>(context));
}

extern "C" void shizuku_pendsv_dispatch(shizuku_armv8m_context *context) {
  shizuku::kernel_instance.pendsv_dispatch(
      reinterpret_cast<shizuku::KERNEL::CONTEXT *>(context));
}

extern "C" void shizuku_debug_dispatch(shizuku_armv8m_context *context) {
  shizuku::kernel_instance.debug_dispatch(
      (shizuku::KERNEL::CONTEXT *)context);
}

extern "C" void shizuku_fault_dispatch(shizuku_armv8m_context *context) {
  shizuku::kernel_instance.fault_dispatch(
      reinterpret_cast<shizuku::KERNEL::CONTEXT *>(context));
}

// ★CTX_RESTORE の先頭から**あらゆる例外復帰**で呼ばれる (docs/05_handoff.md
//   の「2 回目以降の continue/stepi が固まる」の直し方)。復帰しようとしている
//   文脈が GDB stub の予約した相手なら、ここで初めて MON_STEP を立てる。
extern "C" void shizuku_arm_pending_step() {
  shizuku::kernel_instance.consume_pending_step();
}

// ★同じく CTX_RESTORE の先頭から毎回呼ばれる (Q8 / DESIGN §11.3)。復帰しようと
//   している文脈が持つ region_base/region_limit を、この時点で初めて MPU へ
//   書く。set_priv と同じ理由: 値を持たせるだけの側 (do_call/spawn) と
//   実際にハードウェアへ効かせる側 (ここ) を分けておかないと、対象がまだ
//   走っていない段階の「呼び出し元の次の命令」に効いてしまう。
extern "C" void shizuku_restore_region_window() {
  using ARCH = shizuku::archs::armv8m;
  const auto *context = shizuku::kernel_instance.current_context();
  if (context->region_limit != 0) {
    // ★PMSAv8 は 32B 粒度: RLAR.LIMIT はその 32B ブロックの**末尾まで丸ごと**
    //   カバーする。下丸めしても「削れる」わけではない (region_set 自身が
    //   & ~0x1F するのは単に RLAR のフィールド仕様であって、有効範囲を狭める
    //   効果は無い)。だから**要求された範囲を丸ごと覆う**には上丸めが要る —
    //   丸めずに渡すと hardware 側の丸め方向 (実質切り上げ) に運任せになり、
    //   境界ちょうどのバイトが読めたり読めなかったりする (実測で踏んだ:
    //   1 バイト外を読ませたら、たまたま同じブロックに収まって落ちなかった)。
    //   これは粒度の限界であって、**この extent の直後 32B 未満は道連れで
    //   読めてしまい得る** — 隣接ファイルとの間に十分な余白を置くのは
    //   呼び出し側 (flash_fs のセクタ境界配置) の仕事。
    const uint32_t limit = (context->region_limit + 31u) & ~31u;
    ARCH::region_set(ARCH::GRANT_REGION_INDEX, context->region_base, limit,
                     ARCH::ACCESS_RO_ALL, false, 0);
  } else {
    ARCH::region_disable(ARCH::GRANT_REGION_INDEX);
  }
}

// タイマ例外。文脈を触らないので普通の C 関数でよい — ここは期限を見て
// 「切替を起票する」だけで、実際の切替は最低優先度の遅延例外が行う。
#if defined(SHIZUKU_SELFTEST_PROGRESS) && SHIZUKU_SELFTEST_PROGRESS > 0
// 診断専用。停止箇所の進捗は watchdog scratch、SysTick が見た割込まれた側の標本は
// リセットをまたぐ uninitialized RAM に残す。
namespace {
struct selftest_tick_t {
  uint32_t magic, pc, lr, xpsr, hits, stage, primask, control, cfsr, hfsr, ipsr;
  uint32_t fmagic, fcount, fcfsr, fhfsr, fmmfar, fbfar, fpsp, fpsplim, fexc, fpc, fstage;
};
volatile selftest_tick_t __uninitialized_ram(g_st_tick);
constexpr uint32_t kStTickMagic = 0x53545432u;
}
extern "C" void shizuku_selftest_progress_mark(uint32_t stage, uint32_t a, uint32_t b) {
  watchdog_hw->scratch[0] = 0x53505247u; // "SPRG"
  watchdog_hw->scratch[1] = stage;
  watchdog_hw->scratch[2] = a;
  watchdog_hw->scratch[3] = b;
}
// fault 入口 (armv8m_ctx.S) から CTX_SAVE の前に呼ばれる。最後の 1 回を残す。
extern "C" void shizuku_selftest_fault_pre(uint32_t exc_return, uint32_t psp) {
  uint32_t psplim;
  __asm volatile("mrs %0, psplim" : "=r"(psplim));
  g_st_tick.fcount = g_st_tick.fmagic == kStTickMagic ? g_st_tick.fcount + 1u : 1u;
  g_st_tick.fcfsr = *(volatile uint32_t *)0xE000ED28u;
  g_st_tick.fhfsr = *(volatile uint32_t *)0xE000ED2Cu;
  g_st_tick.fmmfar = *(volatile uint32_t *)0xE000ED34u;
  g_st_tick.fbfar = *(volatile uint32_t *)0xE000ED38u;
  g_st_tick.fpsp = psp;
  g_st_tick.fpsplim = psplim;
  g_st_tick.fexc = exc_return;
  g_st_tick.fpc = (psp >= 0x20000000u && psp < 0x20081fe0u && (psp & 3u) == 0) ? ((const uint32_t *)psp)[6] : 0;
  g_st_tick.fstage = watchdog_hw->scratch[1];
  g_st_tick.fmagic = kStTickMagic;
}
extern "C" bool shizuku_selftest_fault_read2(uint32_t *out) {
  if (g_st_tick.fmagic != kStTickMagic) return false;
  out[0] = g_st_tick.fcount; out[1] = g_st_tick.fcfsr; out[2] = g_st_tick.fhfsr;
  out[3] = g_st_tick.fmmfar; out[4] = g_st_tick.fbfar; out[5] = g_st_tick.fpsp;
  out[6] = g_st_tick.fpsplim; out[7] = g_st_tick.fexc; out[8] = g_st_tick.fpc;
  out[9] = g_st_tick.fstage;
  g_st_tick.fmagic = 0;
  return true;
}
// out は firmware/main.cpp の st_fault[18] 配置に合わせる (12..16 = tick 標本、
// 5/6/7/10/11 = cfsr/hfsr/primask/control/ipsr)。
extern "C" bool shizuku_selftest_fault_read(uint32_t *out) {
  for (uint32_t i = 0; i < 18; ++i) out[i] = 0;
  if (g_st_tick.magic != kStTickMagic) return false;
  out[5] = g_st_tick.cfsr; out[6] = g_st_tick.hfsr; out[7] = g_st_tick.primask;
  out[10] = g_st_tick.control; out[11] = g_st_tick.ipsr;
  out[12] = g_st_tick.pc; out[13] = g_st_tick.lr; out[14] = g_st_tick.xpsr;
  out[15] = g_st_tick.hits; out[16] = g_st_tick.stage;
  g_st_tick.magic = 0;
  return true;
}
// ★段階が 3 秒動かなければ標本を残して feed を止め、watchdog に落とす。
//   割込み禁止の停止では SysTick 自体が来ないので、feed は勝手に止まる。
extern "C" void shizuku_selftest_systick_c(const uint32_t *frame) {
  static uint32_t last_stage = 0, since_us = 0;
  const uint32_t stage = watchdog_hw->scratch[1];
  const uint32_t now = time_us_32();
  bool feed = true;
  if (stage != last_stage) {
    last_stage = stage;
    since_us = now;
  } else if (stage >= 800) {
    uint32_t primask, control, ipsr;
    __asm volatile("mrs %0, primask" : "=r"(primask));
    __asm volatile("mrs %0, control" : "=r"(control));
    __asm volatile("mrs %0, ipsr" : "=r"(ipsr));
    g_st_tick.pc = frame[6]; g_st_tick.lr = frame[5]; g_st_tick.xpsr = frame[7];
    g_st_tick.stage = stage; g_st_tick.primask = primask; g_st_tick.control = control;
    g_st_tick.ipsr = ipsr;
    g_st_tick.cfsr = *(volatile uint32_t *)0xE000ED28u;
    g_st_tick.hfsr = *(volatile uint32_t *)0xE000ED2Cu;
    g_st_tick.hits = g_st_tick.hits + 1u;
    g_st_tick.magic = kStTickMagic;
    if (now - since_us > 3000000u) feed = false;
  }
  if (feed) watchdog_update();
  shizuku::kernel_instance.timer_expired();
}
extern "C" __attribute__((naked)) void shizuku_armv8m_systick_entry() {
  __asm volatile("tst lr, #4\n"
                 "ite eq\n"
                 "mrseq r0, msp\n"
                 "mrsne r0, psp\n"
                 "b shizuku_selftest_systick_c\n");
}
#else
extern "C" void shizuku_armv8m_systick_entry() {
  shizuku::kernel_instance.timer_expired();
}
#endif
