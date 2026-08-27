#ifndef SHIZUKU_TEMPLATES_KERNEL_OBJECT_HPP
#define SHIZUKU_TEMPLATES_KERNEL_OBJECT_HPP
#include <cstdint>
#include "shizuku/object_api.hpp"
#include "shizuku/stream.hpp"

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
  static constexpr uintptr_t KERNEL_OBJECT_ID = 0; // システムコールハンドラ
  static constexpr uintptr_t ROOT_OBJECT = 1;      // ブートスレッドが名乗るオブジェクト
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

  // -------------------------------------------------------------------------
  //  共有台帳の相互排除 (2 コア用)
  // -------------------------------------------------------------------------
  //  ★1 コアのときは無償だった。svc は最優先、切替は最低優先度、さらに
  //    「ハンドラ走行中は取り上げを見送る」(pendsv のパリティ判定) があるので、
  //    ハンドラの中は誰にも割り込まれなかった。**2 コアになるとこれが消える** —
  //    両方のコアが同時にハンドラへ入れる。参照実装も同じ罠を踏んで
  //    「協調型単一コアだから実質直列」に依存していた (デュアルコア化で崩れる)。
  //  ★守る対象はオブジェクト表・arena・名前・スレッドスタックの控え。
  //    per-thread の台帳 (shadow / wake_at / budget) は持ち主しか触らないので要らない。
  //  ★★**握ったまま実行権を手放さないこと**。schedule() は中で SWITCH/GRANT を
  //    撃つので、その前に必ず放す。握ったまま切り替えると、相手のコアは
  //    「走っていない持ち主」を待って回り続ける (デッドロック)。
  //    そのため保護区間は**短く、syscall を含まない**ものだけにしてある。
  void table_lock();
  void table_unlock();

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

  // 2 本目のコアを起こす。★スタックも枠もこちらが用意する (D18) ので、
  //   起こすのが方針側の仕事になるのは自然。合成の一部で、ブート後に 1 回だけ呼ぶ。
  //   CORE_COUNT == 1 の構成では何もしないで false を返す。
  bool start_secondary_core();

  // ---- メモリ (オブジェクトランドの資源) ----------------------------------
  // ★カーネルはメモリを持たない。誰にどれだけ渡すかは方針なのでここが持つ。
  //   arena は 2 つ: 簿記用 (非特権から到達できない場所) と、オブジェクト用
  //   (非特権から届く必要があるのでヒープ)。**所有と保護は別の話**なので、
  //   どちらもこちらが用意して貸す点は同じで、違うのは置き場所だけ。
  // ★空きは**大きさの階級ごとのリスト**で持つ。first-fit の線形探索だと、借りる
  //   たびに「それまでの借り方」に依存して歩く距離が変わる = **定数時間で抜けられない**。
  //   svc ハンドラが定数時間で抜けるのに、その先の方針側が入力履歴で伸びるのでは
  //   意味が無い (最悪値が読めないと、貸した実行権の見積もりも立たない)。
  struct block {
    uintptr_t bytes;      // ヘッダ込みの大きさ (BLOCK_ALIGN 境界)
    // ★直前の**物理**ブロックの大きさ (0 = arena の先頭)。境界タグ。
    //   これがあると左隣を O(1) で見つけられる — 解放時の併合のために
    //   リストを頭から歩く必要が消える (それが旧実装で一番重かった)。
    uintptr_t prev_bytes;
    block *next; // 同じ階級の空きリスト (used のときは意味を持たない)
    block *prev; // ★双方向にするのは、併合のとき**途中の要素**を O(1) で
                 //   外す必要があるため (片方向だと前を探して歩くことになる)
    uint16_t owner; // 借りているオブジェクト (NO_OBJECT = 空き)
    uint16_t used;
    // ★詰め物。**貸す先頭 (= ヘッダの直後) を BLOCK_ALIGN 境界に乗せる**ために要る。
    //   ここが崩れると、借りた側が 8B 境界を仮定した書き込み (double / LDRD) で
    //   落ちる。memory.cpp の static_assert が実際にこれを捕まえた。
    uint32_t _align;
  };
  static constexpr uintptr_t BLOCK_ALIGN = 8;
  // 階級は 2 冪ごと。24 階級 = 16MB まで。arena がこれを超えないことは
  // arena_init が確かめる (超えると最上位階級が飽和し、そこだけ線形探索になる)。
  static constexpr uint32_t FREE_CLASSES = 24;
  struct arena {
    uintptr_t base;
    uintptr_t bytes;
    block *free_list[FREE_CLASSES];
    // ★空でない階級のビット。「要求以上で最小の空き階級」を、階級を順に舐めずに
    //   **1 命令 (ctz)** で引くために持つ。これが無いと階級の数だけ歩く。
    uint32_t free_map;
  };

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
    // ★名前はここではなく別配列に置く。object_t は呼び出し経路 (created / flags /
    //   methods) で毎回触るので、経路に関係ない名前を混ぜてキャッシュ行を汚さない。

    uint32_t flags; // OBJECT_* の宣言 (生成時に決まり、以後変わらない)
    method_t methods[METHOD_COUNT];
    // ★flags と違い、こちらは生成後に変わる (GRANT_REGION で書き換わる)。
    //   軸 B (Q8)。0/0 = 窓なし。
    uint32_t region_base;
    uint32_t region_limit;
  };
  // 名前の置き場。**コピーではなくポインタ**を持つ (kobj は文字列を触らない)。
  const char *m_object_name[OBJECT_COUNT_T];
  // ストリームの登録簿。番号は**こちらが割り当てる** (呼ぶ側が選ぶと、
  // オブジェクト番号で 2 回踏んだのと同じ衝突が起きる。D28)。
  static constexpr uintptr_t STREAM_COUNT = 16;
  stream::descriptor *m_streams[STREAM_COUNT];
  // 接続 (DMA ポンプ)。★1 本につき DMA チャネルを 1 つ握る。チャネルは
  //   オブジェクトへは渡さない — DMA は MPU を素通りするため。
  static constexpr uintptr_t CONNECTION_COUNT = 4;
  struct connection {
    volatile uint32_t active;
    uint32_t src;
    uint32_t dst;
    int32_t channel;
    uint32_t inflight; // 転送中のレコード数 (0 = 待機)
    uint32_t src_rd;   // 転送開始時の値 (完了時にここから進める)
    uint32_t dst_wr;
    uint32_t moved;    // 累計 (計装)
  };
  connection m_connections[CONNECTION_COUNT];
  volatile uint32_t m_connection_count;
  // ★ポンプは 1 コアだけが回す。両コアが同時に回すと、同じ接続に 2 本の
  //   転送を仕掛けてしまう。取れなかった側は**待たずに諦める** (次の周回で回る)。
  volatile uint32_t m_pump_lock;

  // per-thread の「今どのオブジェクトとして走っているか」の台帳。
  struct shadow_t {
    uint16_t object[MAX_DEPTH]; // 呼び出しごとの呼び先
    uint16_t caller[MAX_DEPTH]; // その呼び出しの発行元 (identity)
    uint32_t depth;
  };

  // 各 API。戻り値はそのまま発行元へ返る値。エラーは error 引数へ書く。
  uintptr_t create_object(uintptr_t id, uintptr_t entry, uintptr_t flags,
                          object_error &error);
  // 名乗り。★保持して返すだけで、比較はしない (D26)。
  uintptr_t declare_name(uintptr_t name, object_error &error);
  // ---- ストリームの制御プレーン ------------------------------------------
  // ★持つのは**ディスクリプタを指すだけ**。記憶は生成したオブジェクトのもので、
  //   ここは「誰が producer / consumer か」を強制するためだけに居る。
  uintptr_t stream_create(uintptr_t desc, object_error &error);
  uintptr_t stream_open(uintptr_t id, object_error &error);
  uintptr_t stream_bind(uintptr_t id, uintptr_t which, object_error &error);
  // ★src の consumer 席と dst の producer 席を **DMA で直結する**。以後 src へ
  //   流れたものは「途中のオブジェクトが pop して push する」段を通らずに dst へ届く。
  //   中継オブジェクトが要らなくなるのが眼目で、そのために DMA を使う。
  uintptr_t stream_connect(uintptr_t src, uintptr_t dst, object_error &error);
  // 接続を進める。★schedule() から呼ばれる。接続が 0 本なら数語読んで即戻る。
  void pump_connections();
  uintptr_t object_name(uintptr_t id, object_error &error);
  uintptr_t spawn_method(uintptr_t id, uintptr_t method, uintptr_t argument,
                         object_error &error);
  uintptr_t yield_to(uintptr_t target, object_error &error);
  uintptr_t sleep_us(uintptr_t microseconds, object_error &error);
  uintptr_t run_for(uintptr_t thread, uintptr_t cycles,
                    object_error &error);
  void exit_thread();
  // そのオブジェクトを走らせるときの保護指定 (PROTECTION_*)。
  uint32_t object_protection(uintptr_t id) const;
  // そのオブジェクトのスレッドを走らせてよいコア (0 = どこでもよい)。
  uint32_t object_affinity(uintptr_t id) const;
  // ★軸 B (Q8)。呼び出し元ではなく**対象オブジェクトの性質**として引く —
  //   object_protection/object_affinity と同じ形。
  uint32_t object_region_base(uintptr_t id) const;
  uint32_t object_region_limit(uintptr_t id) const;
  // target へ [base, limit) の読み専用窓を開示する。呼び出し元 (現在オブジェクト)
  // が特権でなければ拒否する — 「誰が誰に何を見せるか」を決める権限そのものが
  // 特権行為 (Q8)。base==limit==0 は「閉じる」。
  uintptr_t grant_region(uintptr_t target, uintptr_t base, uintptr_t limit,
                        object_error &error);
  // 生成時に宣言したアフィニティを差し替える。★次の SPAWN から効く
  //   (走っているスレッドは動かさない)。D48 の「コアごとに 1 本ずつ」用。
  uintptr_t set_object_affinity(uintptr_t id, uintptr_t cores,
                                object_error &error);
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
  // ★錠を**既に持っている**呼び出し元のための素の実装。錠は再入できないので、
  //   API 側 (memory_owner) から中でもう一度取ると自分で自分を待つ。
  //   実際にそれで固まった: release が錠を持ったまま owner を呼んでいた。
  uintptr_t owner_of(uintptr_t handle, object_error &error) const;

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
  // ★rotor はコアごと。共有すると 2 コアが同じ順で舐めて同じ相手を取り合う
  //   (claim の CAS が正しく弾くので壊れはしないが、片方が毎回競り負けて空回りする)。
  uint32_t m_rotor[KERNEL::CORE_COUNT];
  // 共有台帳の錠。0 = 空き。SIO のハードウェアスピンロックは E2 erratum で使えない
  // ので LDREX/STREX (cas32) で組む。
  volatile uint32_t m_table_lock;
  arena m_bookkeeping; // カーネルの簿記へ貸す (非特権から届かない場所)
  arena m_objects_arena; // オブジェクトが読み書きする (非特権から届く場所)
};

} // namespace templates
} // namespace shizuku
#endif // SHIZUKU_TEMPLATES_KERNEL_OBJECT_HPP
