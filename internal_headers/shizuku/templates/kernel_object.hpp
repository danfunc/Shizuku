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
          uintptr_t METHOD_COUNT_T, uintptr_t MAX_DEPTH_T,
          uintptr_t THREAD_COUNT_T>
class kernel_object {
public:
  using KERNEL = KERNEL_T;
  using ARCH = typename KERNEL::ARCH;
  using method_t = uintptr_t (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t);
  static constexpr uintptr_t OBJECT_COUNT = OBJECT_COUNT_T;
  static constexpr uintptr_t METHOD_COUNT = METHOD_COUNT_T;
  static constexpr uintptr_t MAX_DEPTH = MAX_DEPTH_T;
  // ★スレッドを何本まで作れるかは**こちらが決める** (記憶を出すのがこちらなので)。
  static constexpr uintptr_t THREAD_COUNT = THREAD_COUNT_T;
  // 1 スレッドあたりのスタック。深さの上限も方針。
  static constexpr uintptr_t THREAD_STACK_BYTES = 4096;
  // スレッドへ CPU を渡すときの既定の量 [クロック]。★時間ではなく仕事量で書く
  //   (kernel.hpp の grant_frame を参照)。目安として 150MHz なら約 2ms だが、
  //   **その換算はここの意味ではない** — クロックを上げれば同じ数字がより短い
  //   時間になる。それでよい: 縛りたいのは仕事量のほう。
  static constexpr uint32_t DEFAULT_BUDGET_CYCLES = 300000;
  static constexpr uintptr_t ROOT_OBJECT = 0; // ブートスレッドが名乗るオブジェクト
  static constexpr uintptr_t NO_OBJECT = OBJECT_COUNT;

  // 表を初期化する (ブート時・スレッドモード。まだ svc は飛んでこない)。
  void init();
  // カーネルに据えるハンドラの入口 (ARCH の ABI シムを通したアドレス)。
  static uintptr_t handler_entry();

  // ---- ハンドラ本体 -------------------------------------------------------
  // 引数は発行元が撃った syscall の a0..a3 そのもの (a0 = 番号)。戻り値は発行元へ
  // 返る値 (エラーは reply で返す)。
  // ★ネストの情報はレジスタで受け取らない。今のネスト数はカーネルが積んだフレームの
  //   段数として読め、こちらは自分の台帳から独立に数えた値を申告して突き合わせる。
  uintptr_t handle(uintptr_t number, uintptr_t a1, uintptr_t a2, uintptr_t a3);

  // スケジューリングの方針 (どのスレッドを次に走らせるか) はここが持つ。
  // カーネルは「渡す機構」しか持たない (D1)。
  bool schedule(uint32_t self);

  // 最初の 1 本 (スレッド 0) のスタックを貸す。組み立ての一部で、bootstrap の
  // 直前に 1 回だけ呼ぶ。**他のスレッドと同じ扱いにする**ためにここに置く。
  struct lent_stack {
    uintptr_t base;
    uintptr_t bytes;
  };
  lent_stack lend_boot_stack();

  // ---- メモリ (オブジェクトランドの資源) ----------------------------------
  // ★カーネルはメモリを持たない。誰にどれだけ渡すかは方針なのでここが持つ。
  //   arena は 2 つ: 簿記用 (非特権から到達できない場所) と、オブジェクト用
  //   (非特権から届く必要があるのでヒープ)。**所有と保護は別の話**なので、
  //   どちらもこちらが用意して貸す点は同じで、違うのは置き場所だけ。
  struct block {
    uintptr_t bytes; // ヘッダ込みの大きさ
    uint16_t owner;  // 借りているオブジェクト (NO_OBJECT = 空き)
    uint16_t used;
    block *next;
  };
  struct arena {
    uintptr_t base;
    uintptr_t bytes;
  };
  static constexpr uintptr_t BLOCK_ALIGN = 8;

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
  uintptr_t sleep_us(uintptr_t microseconds, object_error &error);
  uintptr_t run_for(uintptr_t thread, uintptr_t cycles,
                    object_error &error);
  void exit_thread();
  // そのオブジェクトを走らせるときの保護指定 (PROTECTION_*)。
  uint32_t object_protection(uintptr_t id) const;
  uintptr_t export_method(uintptr_t method, uintptr_t entry,
                          object_error &error);
  uintptr_t call_method(uintptr_t id, uintptr_t method, uintptr_t argument,
                        object_error &error);
  void exit_method(uintptr_t levels, uintptr_t value, uintptr_t error);
  // 巻き戻さずにその場で答える (エラー返却)。
  void reply(object_error error, uintptr_t value);
  // 巻き戻しで申告する「今のネスト数」を**自分の台帳から**計算する (§9.3)。
  uint32_t claimed_depth() const;

  void arena_init(arena &target, uintptr_t base, uintptr_t bytes);
  uintptr_t arena_allocate(arena &target, uintptr_t bytes, uintptr_t owner);
  bool arena_release(arena &target, uintptr_t handle);
  uintptr_t memory_allocate(uintptr_t bytes, object_error &error);
  uintptr_t memory_release(uintptr_t handle, object_error &error);
  uintptr_t memory_hand_over(uintptr_t handle, uintptr_t receiver,
                             object_error &error);
  uintptr_t memory_owner(uintptr_t handle, object_error &error);

  object_t m_objects[OBJECT_COUNT];
  shadow_t m_shadow[THREAD_COUNT];
  // スレッドごとの「どのオブジェクトのために作ったか」。方針側の台帳なのでここ。
  uint16_t m_thread_object[THREAD_COUNT];
  // ★起床時刻と時限は**方針**なのでカーネルではなくここが持つ (D1)。
  //   カーネルは「渡す機構」しか持たず、誰をいつ走らせるかは知らない。
  uint64_t m_wake_at[THREAD_COUNT];
  uint32_t m_budget[THREAD_COUNT];
  // 貸したスタック (終わったスレッドから返してもらうために覚えておく)。
  uintptr_t m_thread_stack[THREAD_COUNT];
  // 次に見るスレッド (round-robin の回転子)。自分の直後だけを見ると飢餓が出る。
  uint32_t m_rotor;
  arena m_bookkeeping; // カーネルの簿記へ貸す (非特権から届かない場所)
  arena m_objects_arena; // オブジェクトが読み書きする (非特権から届く場所)
};

} // namespace templates
} // namespace shizuku
#endif // SHIZUKU_TEMPLATES_KERNEL_OBJECT_HPP
