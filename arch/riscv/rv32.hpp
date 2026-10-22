#ifndef SHIZUKU_ARCH_RISCV_RV32_HPP
#define SHIZUKU_ARCH_RISCV_RV32_HPP

#include <cstddef>
#include <cstdint>
#include "arch/riscv/pmp.hpp"
#include "arch/riscv/rv32_frame.h"
#include "shizuku/concepts/arch.hpp"
#include "shizuku/rv32_abi.hpp"

// 共通の RV32 trap 経路 (arch/riscv/rv32_trap.S) の入口・戻り口。
// dispatch と failure は chips 側の C++ が定義する (shizuku_rv32_trap_dispatch / shizuku_rv32_return_failed)。
extern "C" {
void shizuku_rv32_return_stub();
[[noreturn]] void shizuku_rv32_first_return(void *frame);
}

namespace shizuku::archs {

// RV32 の汎用 arch。チップ差は 3 つの型引数だけで決まる。
//   PMP_ENTRIES      : 物理 PMP エントリ数 (チップが決める。未確定のチップはここに値を置かない)
//   PMP_GRANULE_BYTES: PMP 粒度をバイト数で渡す (4 / 128)。仕様の cfg.G (ビット数) ではない
//   IntMode          : 割込み/タイマ/スレッド起動など、チップ・割込み方式に依る処理の policy
//                      (chips 側。このヘッダは chips を include しない)
template <unsigned PMP_ENTRIES, unsigned PMP_GRANULE_BYTES, class IntMode>
class rv32 {
  static_assert(PMP_ENTRIES > 0 && PMP_ENTRIES <= 64,
                "PMP entry count out of the RV32 range");
  static_assert(PMP_GRANULE_BYTES >= 4 && (PMP_GRANULE_BYTES & (PMP_GRANULE_BYTES - 1)) == 0,
                "PMP granule must be a power of two and at least 4 bytes");

public:
  static constexpr unsigned PMP_ENTRY_COUNT = PMP_ENTRIES;
  static constexpr unsigned PMP_GRANULE = PMP_GRANULE_BYTES;
  using pmp_codec_t = ports::riscv::pmp_codec<PMP_GRANULE_BYTES>;
  using interrupt_mode = IntMode;

  // Caller registers retain their old offsets. Full register image supports
  // nested M-mode traps; s0..s11 are mirrored into the V2 context at ECALL.
  struct exception_frame_t {
    uint32_t ra, t0, t1, t2;
    uint32_t a0, a1, a2, a3, a4, a5, a6, a7;
    uint32_t t3, t4, t5, t6, mepc, mstatus, mcause, mtval;
    uint32_t s0, s1, s2, s3, s4, s5, s6, s7, s8, s9, s10, s11;
    uint32_t gp, tp, reserved0, reserved1;
  };
  static_assert(sizeof(exception_frame_t) == RV32_FRAME_BYTES);
  static_assert(offsetof(exception_frame_t, ra) == RV32_F_RA);
  static_assert(offsetof(exception_frame_t, t0) == RV32_F_T0);
  static_assert(offsetof(exception_frame_t, t1) == RV32_F_T1);
  static_assert(offsetof(exception_frame_t, t2) == RV32_F_T2);
  static_assert(offsetof(exception_frame_t, a0) == RV32_F_A0);
  static_assert(offsetof(exception_frame_t, a1) == RV32_F_A1);
  static_assert(offsetof(exception_frame_t, a2) == RV32_F_A2);
  static_assert(offsetof(exception_frame_t, a3) == RV32_F_A3);
  static_assert(offsetof(exception_frame_t, a4) == RV32_F_A4);
  static_assert(offsetof(exception_frame_t, a5) == RV32_F_A5);
  static_assert(offsetof(exception_frame_t, a6) == RV32_F_A6);
  static_assert(offsetof(exception_frame_t, a7) == RV32_F_A7);
  static_assert(offsetof(exception_frame_t, t3) == RV32_F_T3);
  static_assert(offsetof(exception_frame_t, t4) == RV32_F_T4);
  static_assert(offsetof(exception_frame_t, t5) == RV32_F_T5);
  static_assert(offsetof(exception_frame_t, t6) == RV32_F_T6);
  static_assert(offsetof(exception_frame_t, mepc) == RV32_F_MEPC);
  static_assert(offsetof(exception_frame_t, mstatus) == RV32_F_MSTATUS);
  static_assert(offsetof(exception_frame_t, mcause) == RV32_F_MCAUSE);
  static_assert(offsetof(exception_frame_t, mtval) == RV32_F_MTVAL);
  static_assert(offsetof(exception_frame_t, s0) == RV32_F_S0);
  static_assert(offsetof(exception_frame_t, s1) == RV32_F_S1);
  static_assert(offsetof(exception_frame_t, s2) == RV32_F_S2);
  static_assert(offsetof(exception_frame_t, s3) == RV32_F_S3);
  static_assert(offsetof(exception_frame_t, s4) == RV32_F_S4);
  static_assert(offsetof(exception_frame_t, s5) == RV32_F_S5);
  static_assert(offsetof(exception_frame_t, s6) == RV32_F_S6);
  static_assert(offsetof(exception_frame_t, s7) == RV32_F_S7);
  static_assert(offsetof(exception_frame_t, s8) == RV32_F_S8);
  static_assert(offsetof(exception_frame_t, s9) == RV32_F_S9);
  static_assert(offsetof(exception_frame_t, s10) == RV32_F_S10);
  static_assert(offsetof(exception_frame_t, s11) == RV32_F_S11);
  static_assert(offsetof(exception_frame_t, gp) == RV32_F_GP);
  static_assert(offsetof(exception_frame_t, tp) == RV32_F_TP);
  static_assert(offsetof(exception_frame_t, reserved0) == RV32_F_RESERVED0);
  static_assert(offsetof(exception_frame_t, reserved1) == RV32_F_RESERVED1);

  // スレッド文脈 (callee-saved registers: sp, s0..s11)
  struct context_t {
    // 呼出しの記録は user stack 上のヘッダ枠にある (thread.hpp: identity 配列を縮める目印)。
    static constexpr bool CALL_HEADER_ON_STACK = true;
    exception_frame_t *sp = nullptr;
    uintptr_t s0 = 0, s1 = 0, s2 = 0, s3 = 0;
    uintptr_t s4 = 0, s5 = 0, s6 = 0, s7 = 0;
    uintptr_t s8 = 0, s9 = 0, s10 = 0, s11 = 0;
    uintptr_t stack_limit = 0;
    bool privileged = true;
    uintptr_t region_base = 0;
    uintptr_t region_limit = 0;
  };

  using method_t = uintptr_t (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t,
                                 uintptr_t);

  struct syscall_result {
    uintptr_t error;
    uintptr_t value;
  };

  // 以下の policy 値は IntMode (チップ・割込み方式側) から受ける。generic は値を持たない。
  static constexpr uint32_t CALL_HEADROOM = IntMode::CALL_HEADROOM;
  static constexpr uint32_t EXC_FRAME_MAX_BYTES = sizeof(exception_frame_t);
  static constexpr uint32_t HANDLER_STACK_ARG_BYTES = 0;
  static constexpr uint32_t FAULT_CONTEXT_BYTES = sizeof(context_t);
  static constexpr bool HAS_CYCLE_COUNTER = IntMode::HAS_CYCLE_COUNTER;
  static constexpr uint32_t TIMER_MAX_CYCLES = IntMode::TIMER_MAX_CYCLES;
  static constexpr uint32_t TIMER_MIN_CYCLES = IntMode::TIMER_MIN_CYCLES;

  static uint32_t exc_frame_bytes(const context_t &) {
    return static_cast<uint32_t>(sizeof(exception_frame_t));
  }

  static uintptr_t psp_after_return(const context_t &context) {
    return reinterpret_cast<uintptr_t>(context.sp) + sizeof(exception_frame_t);
  }

  static void normalize_frame(exception_frame_t &) {
    // RV32 ではハードウェアによる SP の奇数ワード自動調整等は存在しない
  }

  // syscall ABI: a0..a4 = frame.a0..frame.a4 (source/kernel/dispatch.cpp:209, 218-239 と整合)
  static uintptr_t arg(const exception_frame_t &frame, unsigned index) {
    switch (index) {
    case 0: return frame.a0;
    case 1: return frame.a1;
    case 2: return frame.a2;
    case 3: return frame.a3;
    case 4: return frame.a4;
    case 5: return frame.a5;
    case 6: return frame.a6;
    case 7: return frame.a7;
    default: return 0;
    }
  }

  static void set_args(exception_frame_t &frame, const uintptr_t *args) {
    frame.a0 = args[0];
    frame.a1 = args[1];
    frame.a2 = args[2];
    frame.a3 = args[3];
    // a4 以降は書かない: レジスタは全部 caller からの入力として素通しする (callee は宣言した
    // 引数以外を信用しない)。5 引数目 (a4) が要る呼び先は set_handler_args が書く。
  }

  static void set_args_6(exception_frame_t &frame, const uintptr_t *args) {
    frame.a0 = args[0];
    frame.a1 = args[1];
    frame.a2 = args[2];
    frame.a3 = args[3];
    frame.a4 = args[4];
    frame.a5 = args[5];
  }

  // RV32's fifth AAPCS argument is a4, which is already in the saved trap frame.
  static void set_handler_args(exception_frame_t &frame,
                               const uintptr_t *args, uintptr_t) {
    set_args(frame, args);
    frame.a4 = args[4];
  }

  static void set_result(exception_frame_t &frame, uintptr_t error,
                         uintptr_t value) {
    frame.a0 = error;
    frame.a1 = value;
  }

  static void set_entry(exception_frame_t &frame, uintptr_t pc, uintptr_t lr) {
    frame.mepc = pc;
    frame.ra = lr;
    // 呼び先の入口: 戻りの出口は引数 (a0-a7) と ra だけを入れ、ほかは消さず素通しする
    // (s*/gp/tp は caller の値)。
    frame.mcause = RV32_TAG_ENTRY;
  }

  static uintptr_t return_stub() {
    return reinterpret_cast<uintptr_t>(&shizuku_rv32_return_stub);
  }

  // ---- 呼出しの記録 (design notes §5b、2026-10-22 ABI) ----------------------------
  // 全ての ecall (CALL_METHOD / descriptor call / kobj への dispatch) は caller の trap フレームを
  // コピーせず、そのまま呼び先の入口フレームにする。呼び先の SP は caller の ecall 時点の SP と
  // 同じ。caller を再開するのに要る最小限は、caller stub が ecall の前に空けた枠
  // [sp, sp + 32) (callee のスタックより上 = callee の窓の外) にカーネルが書く。
  // カーネルが持つのは先頭ヘッダへのポインタ (call_stack.top) と深さだけ。
  // 戻りでは caller の SP の直下に再開フレームを作る (a0/a1 以外は素通し。巻き戻しのときだけ消す)。
  static constexpr bool CALL_IN_PLACE = true;
  struct call_header_t {
    uintptr_t prev;            // 一つ外側のヘッダのアドレス (0 = 最外)
    uintptr_t pc;              // caller の再開 pc (ecall の次の命令)
    uint32_t caller_object;    // この段を積む前の caller_object / generation (call の段が戻すもの)
    uint32_t caller_generation;
    uintptr_t region_base;     // caller の軸 B の窓
    uintptr_t region_limit;
    uint16_t unwind_floor;     // 書かない (台帳 thread.ledger が持つ。ここの値は不定)
    uint8_t via;               // 書かない (同上)
    uint8_t privileged;        // caller の特権
    uint32_t reserved;         // 書かない (枠の大きさを 32B に保つ詰め物)
  };
  // 実機 (32bit) では枠にちょうど収まること。host 試験 (64bit) は uintptr_t が太いので検査しない。
  static_assert(sizeof(uintptr_t) != 4 || sizeof(call_header_t) == RV32_CALL_HEADER_BYTES);
  static_assert(RV32_CALL_HEADER_BYTES == rv32_abi::CALL_HEADER_BYTES);

  // ecall した caller の枠。trap フレーム (context.sp) の直上 = caller の ecall 時点の SP。
  static call_header_t *call_header_slot(const context_t &context) {
    return reinterpret_cast<call_header_t *>(
        reinterpret_cast<uintptr_t>(context.sp) + sizeof(exception_frame_t));
  }

  // 枠に書いたあと呼び先の窓の上端になるアドレス (= 枠の下端 = ecall 時点の SP)。
  static uintptr_t call_window_top(const call_header_t *header) {
    return reinterpret_cast<uintptr_t>(header);
  }

  // 呼び先の入口に要る余白: 返りの trap フレーム 1 枚ぶんに加え、呼び先がもう 1 回カーネルを
  // 呼び返せること (トランポリン 1 枚 + fault 入口の退避 + ハンドラ連鎖)。
  static bool call_header_has_room(const context_t &context) {
    const uintptr_t limit = stack_limit(context);
    if (limit == 0)
      return true;
    const uintptr_t sp = reinterpret_cast<uintptr_t>(context.sp) +
                         sizeof(exception_frame_t);
    return sp >= limit &&
           sp - limit >= 2u * sizeof(exception_frame_t) + EXC_FRAME_MAX_BYTES +
                             FAULT_CONTEXT_BYTES + CALL_HEADROOM;
  }

  // 戻り: ヘッダの枠の直下に caller の再開フレームを作る。a0/a1 (戻り値) は呼び出し側が set_result で
  // 書く。出口が読むのは mepc / mstatus / a0 / a1 だけなので、フレームの残り (144B) は書かない
  // (旧: 全体を 0 で埋めていた)。呼び先が返ってきたときの trap フレームと重なることがある
  // (返りの SP が caller の SP のすぐ下のとき) ので、呼び出し側は戻り値などを先に読んでおくこと。
  // ヘッダ自身はフレームの上にあり、重ならない。
  //   scrub = false : 目印 RV32_TAG_ECALL (素通し)。callee が自分で戻った普通の戻り (1 段)。
  //   scrub = true  : 目印 RV32_TAG_ECALL_SCRUB。callee が自分の意思で戻らない巻き戻し (多段の
  //                   EXIT_METHOD / RETURN、FORWARD_CHILD_EXIT)。出口 (rv32_trap.S の
  //                   .Lrv32_exit_scrub) が ra・t*・a2–a7・s* をレジスタ上で 0 にする。
  [[gnu::always_inline]] static exception_frame_t *call_resume(context_t &context,
                                                                const call_header_t *header,
                                                                bool scrub) {
    auto *frame = reinterpret_cast<exception_frame_t *>(
        reinterpret_cast<uintptr_t>(header) - sizeof(exception_frame_t));
    const uintptr_t pc = header->pc;
    const uintptr_t region_base = header->region_base;
    const uintptr_t region_limit = header->region_limit;
    const bool privileged = header->privileged != 0;
    frame->mepc = pc;
    frame->mstatus = 0; // 呼び出し側が特権・MPIE を足す
    frame->mcause = scrub ? RV32_TAG_ECALL_SCRUB : RV32_TAG_ECALL;
    context.sp = frame;
    context.privileged = privileged;
    context.region_base = region_base;
    context.region_limit = region_limit;
    return frame;
  }

  // 別のスレッドへ切り替わったあとの再開: 切り替わり先が ECALL で止まっていた (目印 RV32_TAG_ECALL)
  // なら、出口でレジスタを消す目印 (RV32_TAG_ECALL_SCRUB) に替える。素通しの出口のままだと、切り替わる
  // 前のスレッドの値 (s*・t*・a2–a7・ra) がそのスレッドに見えてしまう。それ以外の目印は変えない
  // (全レジスタを復元して戻るフレームは自分の値、入口 ENTRY は同じ trap の中で切り替わらない)。
  static void scrub_on_switch(context_t &resumed) {
    exception_frame_t *frame = resumed.sp;
    if (frame->mcause == RV32_TAG_ECALL)
      frame->mcause = RV32_TAG_ECALL_SCRUB;
  }

  // ---- ABI converter, callee 側 (shizuku/method_entry.hpp, shizuku/rv32_abi.hpp) ----
  // F の入口 stub (gp を自分の __global_pointer$ にしてから本体へ tail jump する)。
  template <auto F> static void (*method_stub())() {
#if defined(__riscv)
    return &rv32_abi::method_stub_body<F>;
#else
    return reinterpret_cast<void (*)()>(F); // host 試験: stub を作れない
#endif
  }
  // trap 深さの問い合わせは IntMode に委ねる。ECALL は割込みではない。
  static bool in_interrupt() { return IntMode::in_interrupt(); }

  static uintptr_t frame_pc(const exception_frame_t &frame) {
    return frame.mepc;
  }

  // 特権の判定は IntMode に委ねる (trap 内の保存 MPP など)。
  static bool current_priv() { return IntMode::current_priv(); }

  static void set_priv(context_t &context, bool privileged) {
    context.privileged = privileged;
  }

  static void stack_limit_set(context_t &context, uintptr_t limit) {
    context.stack_limit = limit;
  }

  static uintptr_t stack_limit(const context_t &context) {
    return context.stack_limit;
  }
  // CALL 時に、戻りの dispatch が積む 1 枚ぶんの余白が残るか (dispatch.cpp の
  // do_call)。RV32 はハード検出が無くソフト検査のみなので、戻りの余白を CALL 時に
  // 先取りしないと、最深で return が NO_STACK に断られ止まる。
  // 式は call_frame_push と同じ構成要素 (ヘッダ・実フレーム・余白) から出す。
  static bool call_entry_has_return_room(const context_t &context,
                                         uint32_t header_bytes) {
    const uintptr_t limit = stack_limit(context);
    if (limit == 0)
      return true;
    const uintptr_t frame_bytes = exc_frame_bytes(context);
    const uintptr_t unaligned = header_bytes + frame_bytes + HANDLER_STACK_ARG_BYTES;
    const uintptr_t total = (unaligned + 7u) & ~(uintptr_t)7u;
    const uintptr_t dispatch_reserve = unaligned + EXC_FRAME_MAX_BYTES +
                                       FAULT_CONTEXT_BYTES + CALL_HEADROOM;
    const uintptr_t sp = reinterpret_cast<uintptr_t>(context.sp);
    // total は呼び先のフレーム、もう 1 つの total は戻りの dispatch のフレーム。
    return sp >= limit && sp - limit >= 2u * total + dispatch_reserve;
  }

  static void set_region_window(context_t &context, uintptr_t base,
                                uintptr_t limit) {
    context.region_base = base;
    context.region_limit = limit;
  }

  // RV32 A 拡張 (LR.W / SC.W) によるアトミック操作
  static bool cas32(volatile uint32_t *address, uint32_t expected,
                    uint32_t desired) {
    return __atomic_compare_exchange_n((uint32_t *)address, &expected, desired,
                                       false, __ATOMIC_ACQUIRE,
                                       __ATOMIC_RELAXED);
  }

  static void store_release32(volatile uint32_t *address, uint32_t value) {
    __atomic_store_n((uint32_t *)address, value, __ATOMIC_RELEASE);
  }

  static uint32_t load_acquire32(volatile uint32_t *address) {
    return __atomic_load_n((uint32_t *)address, __ATOMIC_ACQUIRE);
  }

#if defined(__riscv)
  // caller 側 ABI converter (shizuku/rv32_abi.hpp): カーネルは sp / pc / a0 / a1 以外を
  // 保証しない。callee-saved・ra・gp・tp の退避と復元は ecall の中 (クラバー + 明示退避)。
  [[gnu::always_inline]] static syscall_result syscall(uintptr_t number, uintptr_t a1 = 0,
                                uintptr_t a2 = 0, uintptr_t a3 = 0,
                                uintptr_t a4 = 0) {
    const auto result = rv32_abi::ecall(number, a1, a2, a3, a4);
    return {result.error, result.value};
  }

  [[gnu::always_inline]] static syscall_result syscall(uintptr_t number, uintptr_t a1,
                                uintptr_t a2, uintptr_t a3,
                                uintptr_t a4, uintptr_t a5,
                                uintptr_t a6, uintptr_t a7) {
    const auto result = rv32_abi::ecall(number, a1, a2, a3, a4, a5, a6, a7);
    return {result.error, result.value};
  }
#else
  // 非-RISC-V 環境でのホスト呼出しを成功に見せる no-op は禁止。未対応時リンク不能とする。
  static syscall_result syscall(uintptr_t number, uintptr_t a1 = 0,
                                uintptr_t a2 = 0, uintptr_t a3 = 0,
                                uintptr_t a4 = 0);
  static syscall_result syscall(uintptr_t number, uintptr_t a1,
                                uintptr_t a2, uintptr_t a3,
                                uintptr_t a4, uintptr_t a5,
                                uintptr_t a6, uintptr_t a7);
#endif

  static void prepare_thread_entry(context_t &context, uintptr_t stack_top,
                                   uintptr_t entry_pc, uintptr_t return_pc,
                                   uintptr_t argument) {
    auto *frame = reinterpret_cast<exception_frame_t *>(
        (stack_top & ~static_cast<uintptr_t>(15)) - sizeof(exception_frame_t));
    *frame = {};
#if defined(__riscv)
    // New RV32 contexts retain the kernel's global-pointer/TLS ABI.
    asm volatile("mv %0, gp\n mv %1, tp" : "=r"(frame->gp), "=r"(frame->tp));
#endif
    frame->ra = return_pc;
    frame->mepc = entry_pc;
    frame->a0 = argument;
    frame->a1 = frame->a2 = frame->a3 = frame->a4 = 0;
    frame->mstatus = context.privileged ? RV32_MSTATUS_MPP : 0;
    // MPIE stays clear until the interrupt matrix and timer are owned.
    context.sp = frame;
  }

  // タイマ・割込み方式の処理は IntMode (チップ側) に委ねる。
  static void timer_oneshot(uint32_t cycles) { IntMode::timer_oneshot(cycles); }
  static void timer_cancel() { IntMode::timer_cancel(); }
  static uint32_t timer_remaining(bool &wrapped) {
    return IntMode::timer_remaining(wrapped);
  }
  static void pend_context_switch() { IntMode::pend_context_switch(); }

  // サイクルカウンタの制御・読取りは IntMode (チップ側) に委ねる。
  static bool cycle_counter_enable() { return IntMode::cycle_counter_enable(); }
  static uint32_t cycle_counter_read() { return IntMode::cycle_counter_read(); }

  // デバッグおよびフォールト状態は IntMode に委ねる
  static uint32_t debug_reason_take() { return IntMode::debug_reason_take(); }
  static void debug_step(bool on) { IntMode::debug_step(on); }
  static bool faulted_in_thread_mode(const context_t &context) {
    return IntMode::faulted_in_thread_mode(context);
  }
  static uint32_t fault_status() { return IntMode::fault_status(); }
  static uintptr_t fault_address() { return IntMode::fault_address(); }
  static void fault_status_clear() { IntMode::fault_status_clear(); }

  [[noreturn]] static void enter_thread_mode(uintptr_t stack_top,
                                             uintptr_t stack_limit,
                                             void (*entry)()) {
    IntMode::enter_thread_mode(stack_top, stack_limit, entry);
  }
};

} // namespace shizuku::archs

#endif // SHIZUKU_ARCH_RISCV_RV32_HPP
