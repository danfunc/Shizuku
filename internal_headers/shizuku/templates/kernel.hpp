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
//    登録済み svc ハンドラの entry と cookie / 実行権
//  カーネルが知らないこと: オブジェクト、オブジェクト ID、メソッド表、export、md、
//    svc 番号の意味
//
//  ★★2 つの「svc ハンドラ」は別概念。混ぜないこと:
//    (1) **カーネルの svc ハンドラ** = svc_dispatch。例外文脈 (Handler モード) で
//        走る機構そのもの。経路を決め、プリミティブを実行する。番号の意味は持たない
//    (2) **オブジェクトランドの svc ハンドラ** = SET_HANDLER で登録されるメソッド。
//        カーネルオブジェクトが持ち、**スレッドモードで**走る方針側。番号を解釈し、
//        担当サブシステムへ配るのはこちらの仕事
//
//  ★svc の経路は「発行元が信頼された活性化か」の 1 ビットだけで決まる (I-1)。
//    信頼された活性化 → プリミティブを直接実行 ((1) の中で完結し、(2) は経由しない)
//    それ以外         → **登録済みの (2) をメソッドとして呼ぶ**
//  カーネルは番号 → ハンドラの表を持たない。持っているのは登録された entry 1 個だけ。
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
  //    residing させ、下へ複製するのは書き換え用の作業コピーだけ (FP 拡張フレームの
  //    S0-S15 と乖離させないため)。
  //  ★I-4: 呼び先が復帰した直後の SP が退避域へ食い込んではならない。ISA が復帰時に
  //    SP を追加調整する場合 (ARMv8-M の xPSR bit9 による +4 など) は
  //    ARCH::normalize_frame で作業コピー側の調整を消してから積む。push はその結果を
  //    ARCH::psp_after_return で必ず検算する。
  //  ★I-5: pop は push 時に記録した値を読み戻す。再計算しない。
  struct call_frame_header {
    uintptr_t prev;        // 一つ外側のヘッダのアドレス (0 = 最外)
    uint32_t total_bytes;  // 退避域の総バイト数 (8B 境界に丸め済み)
    uint32_t frame_bytes;  // 例外フレーム実サイズ
    uintptr_t caller_cookie;        // 呼び出し元の「現在オブジェクト」
    uintptr_t caller_caller_cookie; // その呼び出し元の identity
    uint32_t caller_trusted;        // 呼び出し元が信頼された活性化だったか
    uint32_t reserved;              // 8B アライン維持
    CONTEXT saved; // 呼び出し元の文脈まるごと (sp を含む = 元フレームの位置)
  };

  CPU_MANAGER cpu_manager;
  MEMORY_MANAGER memory_manager;

  // 自コアの例外結線とメモリマネージャを初期化する。
  void init();
  // 今の実行を「信頼された活性化」= スレッド 0 として採用し、entry へ移る。
  // スレッドスタックへ切り替えるので戻らない (DESIGN §6 のブートストラップ)。
  [[noreturn]] void bootstrap(uintptr_t cookie, void (*entry)());

  // ---- ISA 層 (例外入口) から呼ばれる ----
  CONTEXT *current_context();
  void svc_dispatch(CONTEXT *context);
  void pendsv_dispatch(CONTEXT *context);

  // ---- 信頼境界の内側 (カーネルオブジェクト) 向けの読み出し ----
  uint32_t current_thread_id() const { return m_current[BOARD::core_num()]; }
  THREAD &current_thread() { return m_threads[current_thread_id()]; }
  const THREAD &current_thread() const { return m_threads[current_thread_id()]; }
  uintptr_t current_cookie() const { return current_thread().cookie; }
  uintptr_t current_caller_cookie() const {
    return current_thread().caller_cookie;
  }
  uint32_t current_depth() const { return current_thread().call_stack.depth; }

private:
  bool call_frame_push(THREAD &thread, CONTEXT *context, FRAME **frame);
  bool call_frame_pop(THREAD &thread, CONTEXT *context, FRAME **frame);
  // 保護されたサブルーチン呼び出しの本体。プリミティブ CALL と、オブジェクトランドの
  // svc ハンドラへの引き渡し (トランポリン) の**両方**がこれ 1 本を通る —
  // トランポリンは特別な機構ではなく「登録済みメソッドの呼び出し」であることを
  // 実装でも保つため。
  kernel_error do_call(THREAD &thread, CONTEXT *context, FRAME **frame,
                       const call_request &request, uintptr_t number);

  THREAD m_threads[THREAD_COUNT];
  CONTEXT m_contexts[THREAD_COUNT];
  uint32_t m_current[CORE_COUNT];
  // ★オブジェクトランドの svc ハンドラ (上の (2))。カーネルが持つのは登録された
  //   entry が 1 個だけで、番号 → ハンドラの表ではない。
  struct object_svc_handler_t {
    uintptr_t entry_pc;
    uintptr_t cookie;
    uint32_t protection;
  } m_object_svc_handler;
};

} // namespace templates
} // namespace shizuku
#endif // SHIZUKU_TEMPLATES_KERNEL_HPP
