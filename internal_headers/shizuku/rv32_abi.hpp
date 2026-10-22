#ifndef SHIZUKU_RV32_ABI_HPP
#define SHIZUKU_RV32_ABI_HPP
#include <cstdint>

// ===========================================================================
//  RV32 の ABI converter (2026-10-22 決定、.handoff/v3-design-notes.md §4/§5)
// ===========================================================================
//  method call のレジスタ ABI: 「レジスタは全部 caller からの入力」。
//    - callee の入口でカーネルが決めるのは pc / ra (戻り口) / sp / 引数 (a0–a5) だけ。
//      ほかのレジスタ (t*, s*, gp, tp, a6, a7) は caller の値のまま通る。
//    - 戻りでカーネルが保つのは sp / pc / 戻り値 (a0 = error, a1 = value) だけ。ほかの
//      レジスタは 0 に消される (callee の値を caller に見せない)。
//    - だから callee-saved レジスタ・ra・gp・tp は caller が自分で退避・復元する。
//  ISA ごとに別実装 (ARM の converter は別)。ARCH の層 (arch/riscv/rv32.hpp) と、ARCH を
//  経由せず自前で ecall する側 (ESP-IDF 側の互換層) の両方がこのヘッダを使う。
namespace shizuku::rv32_abi {

// ---- callee 側 ------------------------------------------------------------
//  入口 stub。オブジェクトと同じ翻訳単位で template から生成されるので、
//  __global_pointer$ はそのオブジェクト自身の値に解決される (カーネルは callee の gp を知らない)。
//    - norelax: la が gp 相対へ緩和されると gp を gp から作ることになる。
//    - tail は t1 を使う (t* は入力の一部ではないので潰してよい)。ra・a0–a5 は触らない。
//      本体の普通の return がそのまま戻り口 (shizuku_rv32_return_stub = EXIT_METHOD) へ届く。
//    - tp は触らない: メソッドは caller と同じスレッドで走るので、スレッドの TLS を指す
//      caller の tp がそのまま正しい (オブジェクト単位の TLS は無い)。
#if defined(__riscv)
template <auto F> [[gnu::naked, gnu::used]] void method_stub_body() {
  asm volatile(
      ".option push\n"
      ".option norelax\n"
      "la gp, __global_pointer$\n"
      ".option pop\n"
      "tail %0\n"
      :
      : "i"(F));
}
#endif

// F の入口 stub のアドレス。RISC-V 以外 (host 試験) では stub を作れないので F そのもの。
template <auto F> inline uintptr_t entry_address() {
#if defined(__riscv)
  return reinterpret_cast<uintptr_t>(&method_stub_body<F>);
#else
  return reinterpret_cast<uintptr_t>(F);
#endif
}

} // namespace shizuku::rv32_abi
#endif // SHIZUKU_RV32_ABI_HPP
