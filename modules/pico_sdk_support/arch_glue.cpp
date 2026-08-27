// ISA 層 (armv8m_ctx.S) とカーネルの継ぎ目。asm が呼ぶ extern "C" のフックを、
// 構成で確定したカーネル実体へ転送するだけの薄い層。ここがモジュール側にあるのは、
// フックの名前が ISA 固有だから (カーネル本体は ISA 非依存に保つ)。
#include "shizuku/archs/armv8m.hpp"
#include "shizuku/kernel.hpp"

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
extern "C" void shizuku_armv8m_systick_entry() {
  shizuku::kernel_instance.timer_expired();
}
