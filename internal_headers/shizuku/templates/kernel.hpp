#ifndef SHIZUKU_TEMPLATES_KERNEL_HPP
#define SHIZUKU_TEMPLATES_KERNEL_HPP
#include <cstdint>
#include "shizuku/kernel_abi.hpp"
#include "shizuku/templates/thread.hpp"

namespace shizuku {
namespace templates {

// ===========================================================================
//  カーネル — 機構だけを持つ層 (docs/03_porting_policy.md D1 / DESIGN §7)
// ===========================================================================
//  カーネルが知ること: スレッド / 文脈 / スタック上限 / 呼び出しフレームの push・pop /
//    実行権
//  カーネルが知らないこと: オブジェクト、オブジェクト ID、identity、メソッド表、
//    export、md、svc 番号の意味、そして「誰が偉いか」
//
//  ★★2 つの「svc ハンドラ」は別概念。混ぜないこと:
//    (1) **カーネルの svc ハンドラ** = svc_dispatch。例外文脈 (Handler モード) で
//        走る機構。経路を決め、プリミティブを実行する。番号の意味は持たない
//    (2) **オブジェクトランドの svc ハンドラ** = カーネルオブジェクトが持ち、
//        **スレッドモードで**走る方針側。番号を解釈し、担当へ配る
//
//  ★経路は**呼び出しフレームの段数のパリティ**だけで決まる (I-1):
//      偶数段 → オブジェクトが走っている → (2) をメソッドとして呼ぶ
//      奇数段 → ハンドラが走っている     → プリミティブを実行
//    積むのはトランポリンと CALL の 2 つだけで必ず交互になるので、パリティが
//    そのまま実行主体を表す。旗も cookie も要らない。段数を書けるのはカーネル
//    だけなので偽装もできない (オブジェクトが誰かは kobj の台帳の話。PORT §3.1)。
//
//  ★カーネルオブジェクト以外は RETURN を撃てない。そのため、ハンドラを起こすときに
//    **今のネスト数を渡し** (ARCH::set_handler_info)、オブジェクトは exit API に
//    **何段戻すか**を載せて撃ち、ハンドラがその段数で巻き戻す (D5)。
template <typename CPU_MANAGER_T, typename MEMORY_MANAGER_T,
          uintptr_t THREAD_COUNT_T>
class kernel {
public:
  using CPU_MANAGER = CPU_MANAGER_T;
  using MEMORY_MANAGER = MEMORY_MANAGER_T;
  using ARCH = typename CPU_MANAGER::ARCH;
  using BOARD = typename CPU_MANAGER::BOARD;
  using CONTEXT = typename ARCH::context_t;
  using FRAME = typename ARCH::exception_frame_t;
  using THREAD = thread<CONTEXT>;
  static constexpr uintptr_t THREAD_COUNT = THREAD_COUNT_T;
  static constexpr uintptr_t CORE_COUNT = CPU_MANAGER::CORE_COUNT;

  // -------------------------------------------------------------------------
  //  呼び出しフレーム — 退避先は**スレッド自身のスタック** (DESIGN §8)
  // -------------------------------------------------------------------------
  //  【幾何】スタックは下へ伸びる。呼び出し元の生スタック境界を X とすると:
  //
  //    低位 ←                                                        → 高位
  //    [ 呼び先が使う ][ 書き換え用フレーム ][ ヘッダ + 元の例外フレーム ][ 呼び出し元 ]
  //                    X-total-frame       X-total                    X
  //
  //  ★I-3: 元の例外フレームは 1 バイトも動かさない。退避域の一番上に元の位置のまま
  //    置き、下へ複製するのは書き換え用の作業コピーだけ (FP 拡張フレームの
  //    S0-S15 と乖離させないため)。
  //  ★I-4: 呼び先が復帰した直後の SP が退避域へ食い込んではならない。ISA が復帰時に
  //    SP を追加調整する場合 (ARMv8-M の xPSR bit9 による +4 など) は
  //    ARCH::normalize_frame で作業コピー側の調整を消してから積む。push はその結果を
  //    ARCH::psp_after_return で必ず検算する。
  //  ★I-5: pop は push 時に記録した値を読み戻す。再計算しない。
  struct call_frame_header {
    uintptr_t prev;       // 一つ外側のヘッダのアドレス (0 = 最外)
    uint32_t total_bytes; // 退避域の総バイト数 (8B 境界に丸め済み)
    uint32_t frame_bytes; // 例外フレーム実サイズ
    CONTEXT saved; // 呼び出し元の文脈まるごと (sp を含む = 元フレームの位置)
  };

  CPU_MANAGER cpu_manager;
  MEMORY_MANAGER memory_manager;

  // 自コアの例外結線とメモリマネージャを初期化する。
  void init();
  // オブジェクトランドの svc ハンドラを据える。系の組み立て (composition) の一部で
  // 実行時 API ではないため、ブート前に 1 回だけ呼ぶ。
  void set_object_handler(uintptr_t entry_pc);
  // 今の実行をスレッド 0 として採用し、entry へ移る (スレッドスタックへ
  // 切り替えるので戻らない)。
  [[noreturn]] void bootstrap(void (*entry)());

  // ---- ISA 層 (例外入口) から呼ばれる ----
  CONTEXT *current_context();
  void svc_dispatch(CONTEXT *context);
  void pendsv_dispatch(CONTEXT *context);

  uint32_t current_thread_id() const { return m_current[BOARD::core_num()]; }
  THREAD &current_thread() { return m_threads[current_thread_id()]; }
  const THREAD &current_thread() const { return m_threads[current_thread_id()]; }
  uint32_t current_depth() const { return current_thread().call_stack.depth; }

private:
  bool call_frame_push(THREAD &thread, CONTEXT *context, FRAME **frame);
  bool call_frame_pop(THREAD &thread, CONTEXT *context, FRAME **frame);

  kernel_error do_call(THREAD &thread, CONTEXT *context, FRAME **frame,
                       const call_request &request);

  THREAD m_threads[THREAD_COUNT];
  CONTEXT m_contexts[THREAD_COUNT];
  uint32_t m_current[CORE_COUNT];
  // オブジェクトランドの svc ハンドラの入口。表ではなく 1 個だけ。
  uintptr_t m_object_svc_handler;
};

} // namespace templates
} // namespace shizuku
#endif // SHIZUKU_TEMPLATES_KERNEL_HPP
