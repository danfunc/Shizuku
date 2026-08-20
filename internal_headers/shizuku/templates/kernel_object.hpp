#ifndef SHIZUKU_TEMPLATES_KERNEL_OBJECT_HPP
#define SHIZUKU_TEMPLATES_KERNEL_OBJECT_HPP
#include <cstdint>
#include "shizuku/object_api.hpp"

namespace shizuku {
namespace templates {

// ===========================================================================
//  カーネルオブジェクト — 方針を持つ唯一の信頼オブジェクト (DESIGN §5 / §7.1)
// ===========================================================================
//  カーネルが持たないものは全部ここにある: オブジェクト表 / メソッド表 /
//  identity の台帳 / svc 番号の意味 / (将来) 親子関係・md・スケジューリング方針。
//
//  ★ここが「オブジェクトランドの svc ハンドラ」。カーネルの svc ハンドラ (例外文脈で
//    走る機構) とは別物で、こちらは**スレッドモードで**走る。カーネルは番号を見ずに
//    ここへ渡すだけなので、経路の決定も番号の解釈も全部こちら側にある。
//
//  ★カーネルのプリミティブを撃てるのはここだけ。オブジェクトは撃てないので:
//    (a) カーネルはこのハンドラを起こすとき**今のネスト数**を渡す (第 8 引数)
//    (b) オブジェクトは exit API に**何段戻すか**を載せて撃つ
//    (c) ここがその段数で RETURN する。段数は必ず申告し、カーネルが実際の深さと
//        突き合わせる (§9.3 の両側チェック)
//
//  ★identity の台帳は「影スタック」で持つ。呼び出しを積むのも巻き戻すのも自分なので
//    追跡でき、カーネルの助けは要らない (PORT §3.1 の「カーネル支援不要」の実装形)。
template <typename KERNEL_T, uintptr_t OBJECT_COUNT_T,
          uintptr_t METHOD_COUNT_T, uintptr_t MAX_DEPTH_T>
class kernel_object {
public:
  using KERNEL = KERNEL_T;
  using ARCH = typename KERNEL::ARCH;
  using method_t = uintptr_t (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t);
  static constexpr uintptr_t OBJECT_COUNT = OBJECT_COUNT_T;
  static constexpr uintptr_t METHOD_COUNT = METHOD_COUNT_T;
  static constexpr uintptr_t MAX_DEPTH = MAX_DEPTH_T;
  static constexpr uintptr_t ROOT_OBJECT = 0; // ブートスレッドが名乗るオブジェクト
  static constexpr uintptr_t NO_OBJECT = OBJECT_COUNT;

  // 表を初期化する (ブート時・スレッドモード。まだ svc は飛んでこない)。
  void init();
  // カーネルに据えるハンドラの入口 (ARCH の ABI シムを通したアドレス)。
  static uintptr_t handler_entry();

  // ---- ハンドラ本体 -------------------------------------------------------
  // number/a1..a3 は発行元が撃った syscall の引数、depth はカーネルが渡した
  // 「今のネスト数」。戻り値は発行元へ返る値 (エラーは 0)。
  uintptr_t handle(uintptr_t number, uintptr_t a1, uintptr_t a2, uintptr_t a3,
                   uintptr_t depth);

  // スケジューリングの方針 (どのスレッドを次に走らせるか) はここが持つ。
  // カーネルは「渡す機構」しか持たない (D1)。
  bool schedule(uint32_t self);

  // 台帳の読み出し (自己テスト・将来のアクセス制御用)。
  uintptr_t current_object(uint32_t thread) const {
    const shadow_t &shadow = m_shadow[thread];
    // 呼び出しの中でなければ「そのスレッドを持っているオブジェクト」。
    return shadow.depth == 0 ? m_thread_object[thread]
                             : shadow.object[shadow.depth - 1];
  }
  uintptr_t caller_object(uint32_t thread) const {
    const shadow_t &shadow = m_shadow[thread];
    return shadow.depth == 0 ? NO_OBJECT : shadow.caller[shadow.depth - 1];
  }

private:
  struct object_t {
    bool created;
    uint32_t flags; // OBJECT_* の宣言 (生成時に決まり、以後変わらない)
    method_t methods[METHOD_COUNT];
  };
  // per-thread の「今どのオブジェクトとして走っているか」の台帳。
  struct shadow_t {
    uint16_t object[MAX_DEPTH]; // 呼び出しごとの呼び先
    uint16_t caller[MAX_DEPTH]; // その呼び出しの発行元 (identity)
    uint32_t depth;
  };

  // 各 API。戻り値はそのまま発行元へ返る値。エラーは error 引数へ書く。
  uintptr_t create_object(uintptr_t id, uintptr_t entry, uintptr_t flags,
                          object_error &error);
  uintptr_t spawn_method(uintptr_t id, uintptr_t method, uintptr_t argument,
                         object_error &error);
  uintptr_t yield_to(uintptr_t target, object_error &error);
  uintptr_t run_for(uintptr_t thread, uintptr_t microseconds,
                    object_error &error);
  void exit_thread(uintptr_t depth);
  // そのオブジェクトを走らせるときの保護指定 (PROTECTION_*)。
  uint32_t object_protection(uintptr_t id) const;
  uintptr_t export_method(uintptr_t method, uintptr_t entry,
                          object_error &error);
  uintptr_t call_method(uintptr_t id, uintptr_t method, uintptr_t argument,
                        uintptr_t depth, object_error &error);
  void exit_method(uintptr_t levels, uintptr_t value, uintptr_t error,
                   uintptr_t depth);
  // 巻き戻さずにその場で答える (エラー返却)。
  void reply(object_error error, uintptr_t value, uintptr_t depth);

  object_t m_objects[OBJECT_COUNT];
  shadow_t m_shadow[KERNEL::THREAD_COUNT];
  // スレッドごとの「どのオブジェクトのために作ったか」。方針側の台帳なのでここ。
  uint16_t m_thread_object[KERNEL::THREAD_COUNT];
  // 次に見るスレッド (round-robin の回転子)。自分の直後だけを見ると飢餓が出る。
  uint32_t m_rotor;
};

} // namespace templates
} // namespace shizuku
#endif // SHIZUKU_TEMPLATES_KERNEL_OBJECT_HPP
