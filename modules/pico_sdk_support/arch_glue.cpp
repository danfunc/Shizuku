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

extern "C" void shizuku_fault_dispatch(shizuku_armv8m_context *context) {
  shizuku::kernel_instance.fault_dispatch(
      reinterpret_cast<shizuku::KERNEL::CONTEXT *>(context));
}

// タイマ例外。文脈を触らないので普通の C 関数でよい — ここは期限を見て
// 「切替を起票する」だけで、実際の切替は最低優先度の遅延例外が行う。
extern "C" void shizuku_armv8m_systick_entry() {
  shizuku::kernel_instance.timer_expired();
}
