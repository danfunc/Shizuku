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

// ---- caller 側 ------------------------------------------------------------
//  ecall の呼び出し規約: カーネルが保つのは sp / pc / a0 (error) / a1 (value) だけで、ほかの
//  レジスタは返ったときに 0 か callee の値になっていてよい。だから caller は、コンパイラ任せで
//  済むものはクラバー宣言で (ra・s1–s11・t*・a2–a7)、コンパイラが「触らない」と決めている
//  gp / tp と、フレームポインタに使われうる s0 は asm の中で自分のスタックへ退避・復元する。
//  ★クラバーに s1–s11 を宣言すると、コンパイラは inline したこの箇所で本当に生きているものだけを
//    関数の入口・出口で退避する (旧: カーネルが全部の s* を退避・復元していた)。
//  ★gp は callee が自分のものへ入れ替えてよい (入口 stub)。ここで復元するのは caller 自身の値。
//  ★スタックを一時的に 16 バイト使うが、asm の間はコンパイラの sp 相対アクセスは起きない
//    (オペランドはすべてレジスタ)。
struct call_result {
  uintptr_t error;
  uintptr_t value;
};

#if defined(__riscv)
#define SHIZUKU_RV32_ABI_SAVE_RESTORE_CLOBBERS                                   \
  "ra", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "s1", "s2", "s3", "s4", "s5", \
      "s6", "s7", "s8", "s9", "s10", "s11", "memory"

// a0 = 番号, a1..a4 = 引数。a5..a7 はカーネルに渡さず、返ったあと値が不定になるだけ。
[[gnu::always_inline]] inline call_result ecall(uintptr_t number, uintptr_t a1,
                                                uintptr_t a2, uintptr_t a3,
                                                uintptr_t a4) {
  register uintptr_t r_a0 asm("a0") = number;
  register uintptr_t r_a1 asm("a1") = a1;
  register uintptr_t r_a2 asm("a2") = a2;
  register uintptr_t r_a3 asm("a3") = a3;
  register uintptr_t r_a4 asm("a4") = a4;
  register uintptr_t r_a5 asm("a5");
  register uintptr_t r_a6 asm("a6");
  register uintptr_t r_a7 asm("a7");
  asm volatile(
      "addi sp, sp, -16\n"
      "sw gp, 0(sp)\n"
      "sw tp, 4(sp)\n"
      "sw s0, 8(sp)\n"
      "ecall\n"
      "lw gp, 0(sp)\n"
      "lw tp, 4(sp)\n"
      "lw s0, 8(sp)\n"
      "addi sp, sp, 16\n"
      : "+r"(r_a0), "+r"(r_a1), "+r"(r_a2), "+r"(r_a3), "+r"(r_a4),
        "=r"(r_a5), "=r"(r_a6), "=r"(r_a7)
      :
      : SHIZUKU_RV32_ABI_SAVE_RESTORE_CLOBBERS);
  return {r_a0, r_a1};
}

// a0 = 番号, a1..a7 = 引数 (CALL_METHOD_DESCRIPTOR は a2..a7 に 6 引数を載せる)。
[[gnu::always_inline]] inline call_result ecall(uintptr_t number, uintptr_t a1,
                                                uintptr_t a2, uintptr_t a3,
                                                uintptr_t a4, uintptr_t a5,
                                                uintptr_t a6, uintptr_t a7) {
  register uintptr_t r_a0 asm("a0") = number;
  register uintptr_t r_a1 asm("a1") = a1;
  register uintptr_t r_a2 asm("a2") = a2;
  register uintptr_t r_a3 asm("a3") = a3;
  register uintptr_t r_a4 asm("a4") = a4;
  register uintptr_t r_a5 asm("a5") = a5;
  register uintptr_t r_a6 asm("a6") = a6;
  register uintptr_t r_a7 asm("a7") = a7;
  asm volatile(
      "addi sp, sp, -16\n"
      "sw gp, 0(sp)\n"
      "sw tp, 4(sp)\n"
      "sw s0, 8(sp)\n"
      "ecall\n"
      "lw gp, 0(sp)\n"
      "lw tp, 4(sp)\n"
      "lw s0, 8(sp)\n"
      "addi sp, sp, 16\n"
      : "+r"(r_a0), "+r"(r_a1), "+r"(r_a2), "+r"(r_a3), "+r"(r_a4),
        "+r"(r_a5), "+r"(r_a6), "+r"(r_a7)
      :
      : SHIZUKU_RV32_ABI_SAVE_RESTORE_CLOBBERS);
  return {r_a0, r_a1};
}
#undef SHIZUKU_RV32_ABI_SAVE_RESTORE_CLOBBERS
#endif

} // namespace shizuku::rv32_abi
#endif // SHIZUKU_RV32_ABI_HPP
