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
//      レジスタは素通し (callee の値が残る。見せたくない callee が自分で消す)。callee が自分の
//      意思で戻らない巻き戻し (2 段以上の EXIT_METHOD、FORWARD_CHILD_EXIT) と、別スレッドへ
//      切り替わったあとの再開だけは、カーネルが 0 に消す (arch/riscv/rv32_frame.h の目印)。
//    - だから callee-saved レジスタ・ra・gp・tp は caller が自分で退避・復元する。
//  ISA ごとに別実装 (ARM の converter は別)。ARCH の層 (arch/riscv/rv32.hpp) と、ARCH を
//  経由せず自前で ecall する側 (ESP-IDF 側の互換層) の両方がこのヘッダを使う。
namespace shizuku::rv32_abi {

// ecall 前に sp を下げる量: 呼出しの記録の枠 32 + gp/tp/s0 の退避 16。
inline constexpr unsigned ECALL_RESERVE_BYTES = 48;
inline constexpr unsigned CALL_HEADER_BYTES = 32;

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
//  ★ecall の前に sp を 48 バイト下げる。先頭 32 バイト [sp, sp+32) はカーネルが書く「呼出しの記録」
//    (call_header_t、RV32_CALL_HEADER_BYTES) の枠で、残り 16 バイトに gp/tp/s0 を退避する。
//    callee のスタックは ecall 時点の sp より下だけ (枠と退避域は callee の窓の外)。
//    asm の間はコンパイラの sp 相対アクセスは起きない (オペランドはすべてレジスタ)。
//    RV32_CALL_HEADER_BYTES を変えるときはここの 48 (= 枠 + 16) も合わせる (rv32.hpp に static_assert)。
struct call_result {
  uintptr_t error;
  uintptr_t value;
};

#if defined(__riscv)
// ---- 試験的な遅延退避 (既定は無効): SHIZUKU_ABI_LAZY_SAVE ---------------------------------------------
//  この翻訳単位を `-fcall-used-s2 … -fcall-used-s11` つきでコンパイルしたときだけ、.bazelrc の
//  `--config=abi_lazy_save` が -DSHIZUKU_ABI_LAZY_SAVE=1 を**同じ翻訳単位に**付ける (対象は
//  source/selftest/call_cost.cpp だけ)。そのとき:
//   - コンパイラはこの TU の関数の s2–s11 を「呼出しで壊れてよい (caller-saved)」として扱う。
//     関数の入口で s2–s11 を退避しない (旧: clobber 宣言のたびに入口で 10 本ぶん退避)。生きている
//     値だけが、呼出しをまたぐところで退避される。
//   - s2–s11 は asm の clobber に残す (callee を信用しない: callee が壊した値をコンパイラが
//     「保たれている」と思って使わないように)。-fcall-used は asm には効かないので宣言は必要。
//   - s1 は asm の中で gp/tp/s0 と一緒に自分のスタックへ退避・復元する (clobber から外す): s1 を
//     ecall をまたいで保持できる 1 本にする。枠の 44(sp) を使う (48 バイトの中に空きがある)。
//  ★危険: この TU の関数は s2–s11 を保たない。通常の ABI の TU から**直接呼ばれる**関数があると、
//    呼んだ側の s2–s11 が壊れる。call_cost.cpp の外から呼ばれる唯一の入口 call_cost() は境界で
//    s2–s11 を退避・復元する (call_cost.cpp)。ecall で入る callee (cost_leaf など) は、呼んだ側が
//    ecall で s を信用しないので安全。別の TU に広げるときは同じ境界が要る。
#if defined(SHIZUKU_ABI_LAZY_SAVE)
#define SHIZUKU_RV32_ABI_EXTRA_SAVE "sw s1, 44(sp)\n"
#define SHIZUKU_RV32_ABI_EXTRA_RESTORE "lw s1, 44(sp)\n"
#define SHIZUKU_RV32_ABI_SAVE_RESTORE_CLOBBERS                                   \
  "ra", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "s2", "s3", "s4", "s5", "s6", \
      "s7", "s8", "s9", "s10", "s11", "memory"
#else
#define SHIZUKU_RV32_ABI_EXTRA_SAVE ""
#define SHIZUKU_RV32_ABI_EXTRA_RESTORE ""
#define SHIZUKU_RV32_ABI_SAVE_RESTORE_CLOBBERS                                   \
  "ra", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "s1", "s2", "s3", "s4", "s5", \
      "s6", "s7", "s8", "s9", "s10", "s11", "memory"
#endif

// ★遅延退避の版は別名前空間 (inline) に置く: 同じ名前・別定義の関数が翻訳単位ごとに違う形で
//   存在すると ODR 違反になるため。通常の翻訳単位には現れない。
#if defined(SHIZUKU_ABI_LAZY_SAVE)
inline namespace lazy_save {
#endif

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
      "addi sp, sp, -48\n"
      "sw gp, 32(sp)\n"
      "sw tp, 36(sp)\n"
      "sw s0, 40(sp)\n" SHIZUKU_RV32_ABI_EXTRA_SAVE
      "ecall\n"
      "lw gp, 32(sp)\n"
      "lw tp, 36(sp)\n"
      "lw s0, 40(sp)\n" SHIZUKU_RV32_ABI_EXTRA_RESTORE
      "addi sp, sp, 48\n"
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
      "addi sp, sp, -48\n"
      "sw gp, 32(sp)\n"
      "sw tp, 36(sp)\n"
      "sw s0, 40(sp)\n" SHIZUKU_RV32_ABI_EXTRA_SAVE
      "ecall\n"
      "lw gp, 32(sp)\n"
      "lw tp, 36(sp)\n"
      "lw s0, 40(sp)\n" SHIZUKU_RV32_ABI_EXTRA_RESTORE
      "addi sp, sp, 48\n"
      : "+r"(r_a0), "+r"(r_a1), "+r"(r_a2), "+r"(r_a3), "+r"(r_a4),
        "+r"(r_a5), "+r"(r_a6), "+r"(r_a7)
      :
      : SHIZUKU_RV32_ABI_SAVE_RESTORE_CLOBBERS);
  return {r_a0, r_a1};
}
#if defined(SHIZUKU_ABI_LAZY_SAVE)
} // inline namespace lazy_save
#endif
#undef SHIZUKU_RV32_ABI_SAVE_RESTORE_CLOBBERS
#undef SHIZUKU_RV32_ABI_EXTRA_SAVE
#undef SHIZUKU_RV32_ABI_EXTRA_RESTORE
#endif

} // namespace shizuku::rv32_abi
#endif // SHIZUKU_RV32_ABI_HPP
