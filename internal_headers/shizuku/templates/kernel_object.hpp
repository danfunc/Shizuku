#ifndef SHIZUKU_TEMPLATES_KERNEL_OBJECT_HPP
#define SHIZUKU_TEMPLATES_KERNEL_OBJECT_HPP
#include <cstdint>
#include "shizuku/hot.hpp"
#include "shizuku/kernel_abi.hpp"
#include "shizuku/object_api.hpp"
#include "shizuku/stream.hpp"

namespace shizuku {
namespace selftest {
struct compat_probe_access;
struct dispatcher_test_access;
}
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
//    (a) カーネルは dispatch 先の C 関数へ第 5 引数として unwind floor を渡す
//    (b) exit API は戻り先の絶対深さ d を受け取る
//    (c) ここが count = current_depth - d を RETURN へ渡し、カーネルが
//        d >= unwind floor と count の整合を検査する
//
//  ★identity 台帳はカーネルに貸す特権メモリに置く。オブジェクト側は読み出し API
//    から状態を導き、スタック上の申告値を信頼しない。
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
  static constexpr uintptr_t METHOD_DESCRIPTOR_CAPACITY = 16;
  static constexpr uintptr_t MAX_DEPTH = MAX_DEPTH_T;
#if defined(SHIZUKU_LEAN_LEDGER)
  // 項目が 16B → 40B になるので、台帳の DRAM を抑えるため容量を MAX_DEPTH + 2 にする (旧は 2 倍。
  // call 段の深さは MAX_DEPTH で別に縛られるので、足りない深さは NO_STACK で拒否される)。
  static constexpr uint32_t LEDGER_CAPACITY = MAX_DEPTH + 2;
#else
  static constexpr uint32_t LEDGER_CAPACITY = 2 * MAX_DEPTH + 2;
#endif
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

  // Trusted driver path. Interrupt-capable privileged code calls this directly;
  // it checks the kernel-owned producer seat and never enters the object svc.
  uintptr_t stream_push_kernel(uintptr_t id, const void *records,
                              uint32_t count);
  bool stream_bind_kernel(uintptr_t id);

  // 表を初期化する (ブート時・スレッドモード。まだ svc は飛んでこない)。
  void init();
  // カーネルに据えるハンドラの入口 (ARCH の ABI シムを通したアドレス)。
  static uintptr_t handler_entry();

  // ---- ハンドラ本体 -------------------------------------------------------
  // 引数は発行元が撃った syscall の a0..a3 そのもの (a0 = 番号)。戻り値は発行元へ
  // 返る値 (エラーは reply で返す)。
  // a0..a3 = syscall number + 3 API args, a4 = caller's unwind floor.
  uintptr_t handle(uintptr_t number, uintptr_t a1, uintptr_t a2, uintptr_t a3,
                   uintptr_t unwind_floor);

  // -------------------------------------------------------------------------
  //  共有台帳の相互排除 (2 コア用)
  // -------------------------------------------------------------------------
  //  ★1 コアのときは無償だった。svc は最優先、切替は最低優先度、さらに
  //    「ハンドラ走行中は取り上げを見送る」(pendsv の kind 判定) があるので、
  //    ハンドラの中は誰にも割り込まれなかった。**2 コアになるとこれが消える** —
  //    両方のコアが同時にハンドラへ入れる。参照実装も同じ罠を踏んで
  //    「協調型単一コアだから実質直列」に依存していた (デュアルコア化で崩れる)。
  //  ★守る対象はオブジェクト表・arena・名前・スレッドスタックの控え。
  //    per-thread の wake_at / budget は持ち主しか触らないので要らない。
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
    KERNEL::call_ledger_entry *ledger;
    uint32_t ledger_capacity;
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
  uintptr_t current_object(uint32_t thread) const;
  uintptr_t caller_object(uint32_t thread) const;
  // ★専用ハンドラ (HANDLER) 付与用の信頼済み内部フラグ (重大指摘 1)。
  //   公開 flags (object_api.hpp) には置かず、ROOT_OBJECT / KERNEL_OBJECT 本人による
  //   create_object 時のみ受理される。一般オブジェクトの指定は NOT_PRIVILEGED で拒否。
  static constexpr uintptr_t INTERNAL_FLAG_HANDLER = 1u << 2;
  // ★kernel object の役 (kernel_role) の付与。HANDLER と同じく信頼済みの生成者
  //   (ROOT_OBJECT か kernel_role を持つオブジェクト) だけが立てられる。
  static constexpr uintptr_t INTERNAL_FLAG_KERNEL_ROLE = 1u << 3;

  uint32_t object_parent_handler(uint32_t object_id) const {
    if (object_id >= OBJECT_COUNT_T || !row_load(m_objects[object_id].created))
      return NO_OBJECT;
    return row_load(m_objects[object_id].parent_handler_object);
  }

  // オブジェクト表の 1 行の整列 (バイト)。行の大きさを 2 の冪に揃えて、id から行を引くのを
  // シフトだけにする (旧: 748 バイトの行で `li 0x2ec; mul` が CALL 1 回に 3 回出ていた)。
  // 32 ビットの行は 108 バイトなので 128。METHOD_COUNT などを増やして収まらなくなったら、
  // 下の static_assert が落ちる (その場合はここを次の 2 の冪にする)。
  static constexpr uintptr_t OBJECT_ROW_ALIGN = 128;
  static_assert((METHOD_DESCRIPTOR_CAPACITY & (METHOD_DESCRIPTOR_CAPACITY - 1)) == 0,
                "descriptor capacity must be a power of two (index by shift)");
private:
  struct alignas(OBJECT_ROW_ALIGN) object_t {
    bool created;
    // ★名前はここではなく別配列に置く。object_t は呼び出し経路 (created / flags /
    //   methods) で毎回触るので、経路に関係ない名前を混ぜてキャッシュ行を汚さない。

    uint32_t flags; // OBJECT_* の宣言 (生成時に決まり、以後変わらない)
    // ★**種別**。flags のビットではなく独立したフィールドとして持つ — 「役」は
    //   属性の一つではなく、そのオブジェクトが何であるかそのものだから
    //   (2026-09-05 に第 2 世代の形へ戻した。object_api.hpp の経緯を参照)。
    //   HANDLER を名乗れるのは svc ハンドラを務めるオブジェクトだけで、これが
    //   「プリミティブを撃てるのは誰か」を決める。
    object_kind kind;
    // ★生成者 (CREATE_OBJECT の発行元)。方法記述子の作成権限が「親・直接の子」を
    //   引くために使う。ROOT / KERNEL_OBJECT 自身は生成されたものではないので NO_OBJECT。
    uint32_t creator_object;
    // ★kernel object の役を負うか。種別 (kind) とは別の軸で、ID では判定しない。
    //   kind は dispatch の callee_kind の偽装防止のため変えない。
    bool kernel_role;
    // ★親 handling object の情報 (解決済み binding: Codex 指摘 5)
    uint32_t parent_handler_object;
    uintptr_t parent_handler_entry;
    method_t methods[METHOD_COUNT];
    // ★flags と違い、こちらは生成後に変わる (GRANT_REGION で書き換わる)。
    //   軸 B (Q8)。0/0 = 窓なし。
    uint32_t region_base;
    uint32_t region_limit;
    volatile uint32_t generation;
    volatile uint32_t instance_generation;
    uint32_t seq;
  };
  static_assert(sizeof(uintptr_t) != 4 || sizeof(object_t) == OBJECT_ROW_ALIGN,
                "an object row must be exactly OBJECT_ROW_ALIGN bytes on 32-bit targets");
public:
  template <class T> static T row_load(const T &value) {
    if constexpr (KERNEL::CORE_COUNT == 1) return value;
    else {
      T copy;
      __atomic_load(&value, &copy, __ATOMIC_SEQ_CST);
      return copy;
    }
  }
  template <class T> static void row_store(T &value, T next) {
    if constexpr (KERNEL::CORE_COUNT == 1) value = next;
    else __atomic_store(&value, &next, __ATOMIC_SEQ_CST);
  }

  // Writers are serialized by table_guard. Saturation leaves the sequence odd,
  // so that a wrapped sequence can never validate an obsolete reader snapshot.
  static bool row_write_begin(object_t &row) {
    if constexpr (KERNEL::CORE_COUNT == 1) return true;
    const uint32_t seq = __atomic_load_n(&row.seq, __ATOMIC_SEQ_CST);
    if ((seq & 1u) != 0) return false;
    if (seq >= UINT32_MAX - 1u) {
      __atomic_store_n(&row.seq, UINT32_MAX, __ATOMIC_SEQ_CST);
      return false;
    }
    __atomic_store_n(&row.seq, seq + 1u, __ATOMIC_SEQ_CST);
    return true;
  }
  static void row_write_end(object_t &row) {
    if constexpr (KERNEL::CORE_COUNT != 1) {
      const uint32_t seq = __atomic_load_n(&row.seq, __ATOMIC_SEQ_CST);
      __atomic_store_n(&row.seq, seq + 1u, __ATOMIC_SEQ_CST);
    }
  }
  bool snapshot_row(uintptr_t id, object_t &copy) const {
    if (id >= OBJECT_COUNT_T) return false;
    const object_t &row = m_objects[id];
    const uint32_t before = row_load(row.seq);
    if (before & 1u) return false;
    copy.created = row_load(row.created);
    copy.flags = row_load(row.flags);
    copy.kind = row_load(row.kind);
    copy.creator_object = row_load(row.creator_object);
    copy.kernel_role = row_load(row.kernel_role);
    copy.parent_handler_object = row_load(row.parent_handler_object);
    copy.parent_handler_entry = row_load(row.parent_handler_entry);
    for (uintptr_t i = 0; i < METHOD_COUNT_T; ++i)
      copy.methods[i] = row_load(row.methods[i]);
    copy.region_base = row_load(row.region_base);
    copy.region_limit = row_load(row.region_limit);
    copy.generation = ARCH::load_acquire32(const_cast<volatile uint32_t *>(&row.generation));
    copy.instance_generation = ARCH::load_acquire32(const_cast<volatile uint32_t *>(&row.instance_generation));
    const uint32_t after = row_load(row.seq);
    copy.seq = after;
    return before == after && !(after & 1u);
  }
  // 呼び先 1 件ぶんの入場に要る値 (台帳段と入口フレームへの書き出しが読む)。CALL_METHOD は行から
  //   (view_of_row)、方法記述子の直接 call は項目の前計算から (view_of_descriptor) 作る。形が同じなので
  //   入場の処理は 1 本で済む。
  struct callee_view {
    uint32_t id;
    object_kind kind;
    uint32_t parent_handler_object;
    uintptr_t parent_handler_entry;
    uint32_t instance_generation;
    bool privileged;
    uint32_t region_base;
    uint32_t region_limit;
  };
  // 方法記述子 (caller が callee を呼ぶ入口)。caller の表の 1 項目で、32 バイト (2 の冪なので index は
  //   シフトで引く)。呼出しに要る値は作成 (publish_descriptor) が呼び先の行から前計算して写す:
  //   入口 pc・親ハンドラの入口・種別・親ハンドラ・特権・窓・instance 世代、そして照合用の持ち主と
  //   呼び先の世代 (version)。trap 内の直接 call は「index → 世代照合 → 写しで入る」だけで、行を引かない
  //   (照合の version の 1 語だけ呼び先の行を読む)。
  //   ★写しで入る経路を作らないため、写しの元になる値が変わる操作は必ず呼び先の version (generation) を
  //     上げる。上げるのは 4 か所だけで、どれも項目を STALE にする:
  //     (1) CREATE_OBJECT の生成と差し替え (種別・親ハンドラ・特権の旗・instance 世代を書く)、
  //     (2) EXPORT_METHOD (入口を書く。項目の入口は作成時の写し)、
  //     (3) GRANT_REGION (窓を書く)、
  //     (4) init (表ごと無効にする)。
  //     SET_OBJECT_AFFINITY は flags のアフィニティ欄だけを書き、写す値 (特権の旗を含む) に触れないので
  //     version を上げない (test_dispatcher_v1.cpp の試験で縛る)。
  //     持ち主の差し替えは持ち主の項目を全部無効にする。
  //   ★callee == DESCRIPTOR_NONE が「無効」(旧 target == nullptr)。公開は callee を最後に release で書く。
  //   ★持ち主 (owner_object) は持たない: 表が持ち主ごとなので、項目の位置が持ち主を決める。
  //   ★created / 入口 0 の検査は呼出し側に残さない: 作成 (create_method_descriptor) が
  //     「呼び先が created、かつ methods[index] != nullptr」を確かめてから項目を公開する。
  struct method_descriptor_entry {
    uintptr_t target_entry;
    uintptr_t parent_handler_entry;
    uint32_t owner_generation;
    uint32_t target_generation;
    uint32_t instance_generation;
    uint32_t region_base;
    uint32_t region_limit;
    uint8_t callee;
    uint8_t parent_handler_object;
    uint8_t kind : 4;
    uint8_t privileged : 4;
    uint8_t seq;
  };
  static constexpr uint8_t DESCRIPTOR_NONE = 0xFF;
  static_assert(OBJECT_COUNT_T < DESCRIPTOR_NONE,
                "a descriptor's callee id must fit in a byte below the NONE marker");
  static_assert(sizeof(uintptr_t) != 4 || sizeof(method_descriptor_entry) == 32,
                "a descriptor entry must be 32 bytes on 32-bit targets (index by shift)");
  // 行から id を求める (行は m_objects の要素。2 の冪の大きさなのでシフトで済む)。
  template <class Row> uint32_t row_id(const Row &row) const {
    return (uint32_t)(&row - m_objects);
  }
  // 記述子項目を引く。trap 内の直接 call がシフトで届く。
  SHIZUKU_HOT const method_descriptor_entry &descriptor_entry(uint32_t owner, uint32_t index) const {
    return m_descriptors[owner][index];
  }
  // 行の version (generation) を読む。
  template <class Row> uint32_t row_version(const Row &row) const {
    return ARCH::load_acquire32(const_cast<volatile uint32_t *>(&row.generation));
  }
  // 記述子の引きと検査 (index・持ち主の世代・呼び先の version)。通れば項目を返し、通らなければ
  // nullptr と error。コピーしないので trap 内の直接 call はこれを読む。
  // 順序: index / 持ち主の範囲 (BAD) → seq 奇数 (STALE) → 無効 (BAD) → 持ち主の世代 (STALE) → 呼び先の version (STALE)。
  // 呼び先の行を読むのは version の 1 語だけ (種別・窓などは項目の写しを使う)。
  SHIZUKU_HOT const method_descriptor_entry *find_method_descriptor(
      uint32_t caller_id, uint32_t index, uint32_t caller_generation,
      object_error &error) const {
    if (index >= METHOD_DESCRIPTOR_CAPACITY || caller_id >= OBJECT_COUNT_T) {
      error = object_error::BAD_DESCRIPTOR;
      return nullptr;
    }
    const auto &desc = m_descriptors[caller_id][index];
    const uint8_t seq = __atomic_load_n(&desc.seq, __ATOMIC_ACQUIRE);
    if ((seq & 1u) != 0) {
      error = object_error::STALE_DESCRIPTOR;
      return nullptr;
    }
    const uint8_t callee = desc.callee;
    if (callee == DESCRIPTOR_NONE) {
      error = object_error::BAD_DESCRIPTOR;
      return nullptr;
    }
    if (desc.owner_generation != caller_generation) {
      error = object_error::STALE_DESCRIPTOR;
      return nullptr;
    }
    const uint32_t version = row_version(m_objects[callee]);
    if (version == 0 || desc.target_generation != version) {
      error = object_error::STALE_DESCRIPTOR;
      return nullptr;
    }
    error = object_error::OK;
    return &desc;
  }
  // 項目から入場の形を作る (前計算の写し。引きの検査は find_method_descriptor が済ませている)。
  SHIZUKU_HOT callee_view view_of_descriptor(const method_descriptor_entry &desc) const {
    return callee_view{(uint32_t)desc.callee,       (object_kind)desc.kind,
                       desc.parent_handler_object,  desc.parent_handler_entry,
                       desc.instance_generation,    desc.privileged != 0,
                       desc.region_base,            desc.region_limit};
  }
  // 呼び先の行から入場の形を作る (CALL_METHOD と、作成時の写しの元)。
  template <class Row> callee_view view_of_row(uint32_t id, const Row &row) const {
    return callee_view{id,
                       row.kind,
                       row.parent_handler_object,
                       row.parent_handler_entry,
                       instance_generation_of(row),
                       (row.flags & OBJECT_UNPRIVILEGED) == 0,
                       row.region_base,
                       row.region_limit};
  }
  // 項目に呼び先の前計算を書き、最後に偶数 seq を release で公開する (作成と試験の据付けが共有する)。
  // 書く側は seq を奇数にしてから中身を書き、書き終えたら偶数に進めて store-release で公開。
  // 呼び先は created・入口 0 でないことを呼出し側が確かめてあること。
  void publish_descriptor(method_descriptor_entry &desc, uintptr_t callee_id,
                          uintptr_t target_entry, uint32_t owner_generation) {
    const object_t &row = m_objects[callee_id];
    const uint8_t cur_seq = desc.seq;
    __atomic_store_n(&desc.seq, (uint8_t)(cur_seq + 1u), __ATOMIC_RELEASE);
    desc.target_entry = target_entry;
    desc.parent_handler_entry = row.parent_handler_entry;
    desc.owner_generation = owner_generation;
    desc.target_generation = row_version(row);
    desc.instance_generation = instance_generation_of(row);
    desc.region_base = row.region_base;
    desc.region_limit = row.region_limit;
    desc.kind = (uint8_t)row.kind;
    desc.parent_handler_object = (uint8_t)row.parent_handler_object;
    desc.privileged = (row.flags & OBJECT_UNPRIVILEGED) == 0 ? 1 : 0;
    desc.callee = (uint8_t)callee_id;
    __atomic_store_n(&desc.seq, (uint8_t)(cur_seq + 2u), __ATOMIC_RELEASE);
  }
  // 項目を無効にする (持ち主の差し替え・初期化)。
  // 削除は無効化 (callee=DESCRIPTOR_NONE 等) してから seq を進める。
  static void invalidate_descriptor(method_descriptor_entry &desc) {
    const uint8_t cur_seq = desc.seq;
    __atomic_store_n(&desc.seq, (uint8_t)(cur_seq + 1u), __ATOMIC_RELEASE);
    desc.callee = DESCRIPTOR_NONE;
    __atomic_store_n(&desc.seq, (uint8_t)(cur_seq + 2u), __ATOMIC_RELEASE);
  }
  SHIZUKU_HOT bool validate_method_descriptor(uint32_t caller_id,
                                              uint32_t index,
                                              uint32_t caller_generation,
                                              method_descriptor_entry &out_desc,
                                              object_error &error) const {
    const auto *desc =
        find_method_descriptor(caller_id, index, caller_generation, error);
    if (desc == nullptr)
      return false;
    out_desc = *desc;
    return true;
  }
  // そのオブジェクトを走らせるときの保護指定 (PROTECTION_*)。直接 call (記述子経路) も
  // 通常の CALL と同じく、呼び先オブジェクトの設定を呼び出し時に引く。
  uint32_t object_protection(uintptr_t id) const;
  uint32_t object_generation(uintptr_t id) const {
    return id < OBJECT_COUNT_T
               ? ARCH::load_acquire32(const_cast<volatile uint32_t *>(
                     &m_objects[id].instance_generation))
               : 0;
  }
  uint32_t object_version(uintptr_t id) const {
    return id < OBJECT_COUNT_T
               ? ARCH::load_acquire32(const_cast<volatile uint32_t *>(
                     &m_objects[id].generation))
               : 0;
  }
  // 呼び先 1 件ぶんの CALL 要求 (種別・親ハンドラ・保護・窓)。CALL_METHOD と
  // 方法記述子の直接 call が同じ引き方をするよう、ここでだけ作る。
  call_request callee_request(uintptr_t id, uintptr_t entry_pc) const;

  struct region_window { uintptr_t base; uintptr_t limit; };
  [[gnu::always_inline]] inline region_window return_region(uintptr_t id) const {
    if constexpr (KERNEL::CORE_COUNT == 1) {
      if (id >= OBJECT_COUNT_T || !m_objects[id].created) return {0, 0};
      return {m_objects[id].region_base, m_objects[id].region_limit};
    } else {
      // 要るのは created と窓の 3 欄だけ。snapshot_row のように行ごと (methods[] と世代を含む) 写さず、
      //   同じ seq の組 (奇数なら書き手が途中、前後で違えば差し替えを跨いだ) でこの 3 欄だけを守る。
      //   判定は snapshot_row と同じ: 3 回試して揃わなければ閉じた窓 (0/0) に倒す。
      if (id >= OBJECT_COUNT_T) return {0, 0};
      const object_t &row = m_objects[id];
      for (unsigned attempt = 0; attempt < 3; ++attempt) {
        const uint32_t before = row_load(row.seq);
        if (before & 1u) continue;
        const bool created = row_load(row.created);
        const uint32_t base = row_load(row.region_base);
        const uint32_t limit = row_load(row.region_limit);
        if (row_load(row.seq) != before) continue;
        return created ? region_window{base, limit} : region_window{0, 0};
      }
      return {0, 0};
    }
  }

  // ---- trap 内 (svc_dispatch) で直接処理する API の部品 ----------------------
  //  CALL_METHOD / SET_BUDGET は dispatch 段も kobj 再入も経由せず、カーネルの svc
  //  ハンドラがここを直接読み書きする (D1 廃止済み: カーネルと kobj の密結合は許容)。
  //  どちらも待たず・syscall を撃たず・ループしない。錠は取らない (kobj 経由だった
  //  従来も取っていなかった。表の書き換えと競っても、古い値で 1 回呼ぶだけ)。

  // 呼び出し元 (caller_id) が対象 (target_id) を呼び出せるか、または方法記述子を作成できるかを判定する。
  // 規則:
  // - 呼び元が kernel 役 (kernel_role) なら相手を問わず可
  // - 自分自身 (target_id == caller_id)
  // - 親 (target_id == m_objects[caller_id].creator_object)
  // - 直接の子 (m_objects[target_id].creator_object == caller_id)
  // は可。兄弟・孫・無関係は不可 (NOT_PRIVILEGED)。
  SHIZUKU_HOT bool can_call_method(uintptr_t caller_id, uintptr_t target_id) const {
    if constexpr (KERNEL::CORE_COUNT == 1) {
      if (caller_id >= OBJECT_COUNT_T || !m_objects[caller_id].created)
        return false;
      if (target_id >= OBJECT_COUNT_T || !m_objects[target_id].created)
        return false;
      const bool caller_is_kernel_role = m_objects[caller_id].kernel_role;
      return caller_is_kernel_role || target_id == caller_id ||
             m_objects[caller_id].creator_object == target_id ||
             m_objects[target_id].creator_object == caller_id;
    }
    if (caller_id >= OBJECT_COUNT_T || !row_load(m_objects[caller_id].created))
      return false;
    if (target_id >= OBJECT_COUNT_T || !row_load(m_objects[target_id].created))
      return false;
    const bool caller_is_kernel_role = row_load(m_objects[caller_id].kernel_role);
    return caller_is_kernel_role || target_id == caller_id ||
           row_load(m_objects[caller_id].creator_object) == target_id ||
           row_load(m_objects[target_id].creator_object) == caller_id;
  }

  // (id, method) から呼び先の CALL 要求を O(1) で引く。OK 以外のとき request は触らない。
  // 判定と順序: BAD_OBJECT → NOT_PRIVILEGED → BAD_METHOD → UNDECLARED_METHOD。
  SHIZUKU_HOT object_error method_call_request(uintptr_t id, uintptr_t method,
                                               uintptr_t caller_id,
                                               call_request &request) const {
    if (id >= OBJECT_COUNT_T || !row_load(m_objects[id].created))
      return object_error::BAD_OBJECT;
    if (!can_call_method(caller_id, id))
      return object_error::NOT_PRIVILEGED;
    if (method >= METHOD_COUNT_T)
      return object_error::BAD_METHOD;
    const method_t entry = row_load(m_objects[id].methods[method]);
    if (entry == nullptr)
      return object_error::UNDECLARED_METHOD; // 未 export は panic ではなくエラー
    request = callee_request(id, (uintptr_t)entry);
    return object_error::OK;
  }
  // method 表の引き (method_call_request と同じ判定と順序: BAD_OBJECT → NOT_PRIVILEGED → BAD_METHOD → UNDECLARED_METHOD)。
  // OK のとき入口を返し、そうでなければ nullptr と error。
  SHIZUKU_HOT method_t find_method(uintptr_t id, uintptr_t method, uintptr_t caller_id,
                                   object_error &error) const {
    if (id >= OBJECT_COUNT_T || !row_load(m_objects[id].created)) {
      error = object_error::BAD_OBJECT;
      return nullptr;
    }
    if (!can_call_method(caller_id, id)) {
      error = object_error::NOT_PRIVILEGED;
      return nullptr;
    }
    if (method >= METHOD_COUNT_T) {
      error = object_error::BAD_METHOD;
      return nullptr;
    }
    const method_t entry = row_load(m_objects[id].methods[method]);
    if (entry == nullptr) {
      error = object_error::UNDECLARED_METHOD; // 未 export は panic ではなくエラー
      return nullptr;
    }
    error = object_error::OK;
    return entry;
  }
  // find_method の行つき版: 表の行 (呼び先の id の行) も一緒に返す。呼び出し側がその行を台帳段・入口フレームの
  // 書き出しにそのまま使えるので、id から行のアドレス (id × 行の大きさ) を作り直さない。判定と順序は
  // find_method と同じ (BAD_OBJECT → NOT_PRIVILEGED → BAD_METHOD → UNDECLARED_METHOD)。
  // OK のとき行を返して entry に入口を入れ、そうでなければ nullptr と error。
  SHIZUKU_HOT const auto *find_method_row(uintptr_t id, uintptr_t method, uintptr_t caller_id,
                                          object_error &error, method_t &entry) const {
    if constexpr (KERNEL::CORE_COUNT == 1) {
      const auto *row = id < OBJECT_COUNT_T ? &m_objects[id] : nullptr;
      if (row == nullptr || !row->created) {
        error = object_error::BAD_OBJECT;
        return decltype(row)(nullptr);
      }
      if (!can_call_method(caller_id, id)) {
        error = object_error::NOT_PRIVILEGED;
        return decltype(row)(nullptr);
      }
      if (method >= METHOD_COUNT_T) {
        error = object_error::BAD_METHOD;
        return decltype(row)(nullptr);
      }
      entry = row->methods[method];
      if (entry == nullptr) {
        error = object_error::UNDECLARED_METHOD;
        return decltype(row)(nullptr);
      }
      error = object_error::OK;
      return row;
    }
    const auto *row = id < OBJECT_COUNT_T ? &m_objects[id] : nullptr;
    if (row == nullptr || !row_load(row->created)) {
      error = object_error::BAD_OBJECT;
      return decltype(row)(nullptr);
    }
    if (!can_call_method(caller_id, id)) {
      error = object_error::NOT_PRIVILEGED;
      return decltype(row)(nullptr);
    }
    if (method >= METHOD_COUNT_T) {
      error = object_error::BAD_METHOD;
      return decltype(row)(nullptr);
    }
    entry = row_load(row->methods[method]);
    if (entry == nullptr) {
      error = object_error::UNDECLARED_METHOD; // 未 export は panic ではなくエラー
      return decltype(row)(nullptr);
    }
    error = object_error::OK;
    return row;
  }
  struct method_resolution { bool found; callee_view view; };
  [[gnu::always_inline]] inline method_resolution resolve_method_view(
      uintptr_t id, uintptr_t method, uintptr_t caller_id,
      object_error &error, method_t &entry) const {
    if constexpr (KERNEL::CORE_COUNT == 1) {
      const auto *row = find_method_row(id, method, caller_id, error, entry);
      return row ? method_resolution{true, view_of_row((uint32_t)id, *row)}
                 : method_resolution{false, {}};
    } else {
      // 行ごと写さず、判定と入場の形に要る欄だけを seq の間で読む (methods は選んだ 1 本だけ)。
      //   呼び手の行も権限の 3 欄 (created / kernel_role / creator_object) を揃った組で読む必要があるので、
      //   seq で守る (書き手が行を差し替える途中の欄を混ぜて権限を誤判定しないため)。
      if (id >= OBJECT_COUNT_T) {
        error = object_error::BAD_OBJECT;
        return {false, {}};
      }
      const object_t &trow = m_objects[id];
      const uint32_t tseq = row_load(trow.seq);
      if (tseq & 1u) {
        error = object_error::BAD_OBJECT;
        return {false, {}};
      }
      const bool tcreated = row_load(trow.created);
      const uint32_t tcreator = row_load(trow.creator_object);
      const method_t selected = method < METHOD_COUNT_T ? row_load(trow.methods[method]) : nullptr;
      object_t view_row{};
      view_row.flags = row_load(trow.flags);
      view_row.kind = row_load(trow.kind);
      view_row.parent_handler_object = row_load(trow.parent_handler_object);
      view_row.parent_handler_entry = row_load(trow.parent_handler_entry);
      view_row.region_base = row_load(trow.region_base);
      view_row.region_limit = row_load(trow.region_limit);
      view_row.instance_generation = instance_generation_of(trow);
      if (row_load(trow.seq) != tseq || !tcreated) {
        error = object_error::BAD_OBJECT;
        return {false, {}};
      }
      if (caller_id >= OBJECT_COUNT_T) {
        error = object_error::NOT_PRIVILEGED;
        return {false, {}};
      }
      const object_t &crow = m_objects[caller_id];
      const uint32_t cseq = row_load(crow.seq);
      if (cseq & 1u) {
        error = object_error::BAD_OBJECT;
        return {false, {}};
      }
      const bool ccreated = row_load(crow.created);
      const bool ckernel = row_load(crow.kernel_role);
      const uint32_t ccreator = row_load(crow.creator_object);
      if (row_load(crow.seq) != cseq || row_load(trow.seq) != tseq) {
        error = object_error::BAD_OBJECT;
        return {false, {}};
      }
      if (!ccreated || !(ckernel || caller_id == id || ccreator == id || tcreator == caller_id)) {
        error = object_error::NOT_PRIVILEGED;
        return {false, {}};
      }
      if (method >= METHOD_COUNT_T) {
        error = object_error::BAD_METHOD;
        return {false, {}};
      }
      if (!selected) {
        error = object_error::UNDECLARED_METHOD;
        return {false, {}};
      }
      entry = selected;
      error = object_error::OK;
      return {true, view_of_row((uint32_t)id, view_row)};
    }
  }
  // 行から世代を読む (object_generation(id) と同じ読み方。id の範囲検査は行を引いた側が済ませている)。
  template <class Row> uint32_t instance_generation_of(const Row &row) const {
    return ARCH::load_acquire32(const_cast<volatile uint32_t *>(&row.instance_generation));
  }
  // 表の錠の inline 版 (table_lock / table_unlock と同じ CAS。区間は短く syscall を含まない)。
  // trap 内の直接 call が関数呼出しなしで取る。診断用の breadcrumb は付けない。
  SHIZUKU_HOT void table_lock_inline() {
    while (!ARCH::cas32(&m_table_lock, 0u, 1u)) {
    }
  }
  SHIZUKU_HOT void table_unlock_inline() { ARCH::store_release32(&m_table_lock, 0u); }
  // スケジューラが後でそのスレッドを選んだときに渡す量の既定値 [クロック]。範囲外は
  // 従来どおり黙って何もしない。★権限検査は無い (誰でも任意スレッドの量を書ける。従来のまま)。
  void set_budget(uintptr_t thread, uintptr_t cycles) {
    if (thread < THREAD_COUNT_T)
      m_budget[thread] = (uint32_t)cycles;
  }

private:
  // The compatibility probe inspects a suspended thread's private ledger
  // shape without exposing that inspection through the object API.
  friend struct ::shizuku::selftest::compat_probe_access;
  // dispatcher の host 試験が、オブジェクト表とメソッド表を直接組み立てる。
  friend struct ::shizuku::selftest::dispatcher_test_access;

  // 名前の置き場。**コピーではなくポインタ**を持つ (kobj は文字列を触らない)。
  const char *m_object_name[OBJECT_COUNT_T];
  // ストリームの登録簿。番号は**こちらが割り当てる** (呼ぶ側が選ぶと、
  // オブジェクト番号で 2 回踏んだのと同じ衝突が起きる。D28)。
  static constexpr uintptr_t STREAM_COUNT = 16;
  static constexpr uint32_t STREAM_SLOT_BITS = 4;
  static constexpr uint32_t STREAM_SLOT_MASK = (1u << STREAM_SLOT_BITS) - 1u;
  struct stream_entry {
    uint8_t *base;
    uintptr_t allocation;
    uintptr_t owner;
    uint32_t rec_size;
    uint32_t capacity;
    uint32_t flags;
    volatile uint32_t wr;
    volatile uint32_t rd;
    uint32_t producer;
    uint32_t consumer;
    uint32_t generation;
    volatile uint32_t active;
    volatile uint32_t users;
    bool retired;
  };
  stream_entry m_streams[STREAM_COUNT];
  // 接続 (DMA ポンプ)。★1 本につき DMA チャネルを 1 つ握る。チャネルは
  //   オブジェクトへは渡さない — DMA は MPU を素通りするため。
  static constexpr uintptr_t CONNECTION_COUNT = 4;
  // 接続の状態。★2 値ではなく 3 値にしてある — 「畳んでほしい」と「畳んだ」を
  //   分けないと、DMA が転送中の環の席を返してしまう (別の持ち主が bind できて
  //   しまう)。印を立てるのは呼び出し側、実際に畳むのはポンプ。
  static constexpr uint32_t CONNECTION_FREE = 0;
  static constexpr uint32_t CONNECTION_ACTIVE = 1;
  static constexpr uint32_t CONNECTION_CLOSING = 2;
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
  //  ★★スレッド自身のスタック上の呼び出しフレームから導出してはいけない。
  //    フレームは非特権オブジェクトが読み書きできる場所に住んでいるので、
  //    そこから identity を読むと**自分の呼び出し元を書き換えて他人を騙れる**。
  //    台帳をこちら側 (非特権から届かない場所) に持つのが偽装できない根拠。

  // 各 API。戻り値はそのまま発行元へ返る値。エラーは error 引数へ書く。
  uintptr_t create_object(uintptr_t id, uintptr_t entry, uintptr_t flags,
                          object_error &error);
  uintptr_t create_method_descriptor(uintptr_t target_id, uintptr_t method_index,
                                     object_error &error);
  // 名乗り。★保持して返すだけで、比較はしない (D26)。
  uintptr_t declare_name(uintptr_t name, object_error &error);
  // ---- ストリームの制御プレーン ------------------------------------------
  // ★持つのは**ディスクリプタを指すだけ**。記憶は生成したオブジェクトのもので、
  //   ここは「誰が producer / consumer か」を強制するためだけに居る。
  uintptr_t stream_create(uintptr_t rec_size, uintptr_t capacity,
                          uintptr_t flags, object_error &error);
  uintptr_t stream_open(uintptr_t id, object_error &error);
  uintptr_t stream_bind(uintptr_t id, uintptr_t which, object_error &error);
  uintptr_t stream_destroy(uintptr_t id, object_error &error);
  uintptr_t stream_push_n(uintptr_t id, uintptr_t request,
                          object_error &error);
  uintptr_t stream_pop_n(uintptr_t id, uintptr_t request,
                         object_error &error);
  uintptr_t stream_available(uintptr_t id, object_error &error);
  stream_entry *stream_lookup(uintptr_t id);
  bool stream_pin(uintptr_t id, stream_entry *&entry);
  void stream_unpin(stream_entry &entry);
  bool stream_stack_range(uint32_t thread, uintptr_t address,
                          uintptr_t bytes) const;
  // ★src の consumer 席と dst の producer 席を **DMA で直結する**。以後 src へ
  //   流れたものは「途中のオブジェクトが pop して push する」段を通らずに dst へ届く。
  //   中継オブジェクトが要らなくなるのが眼目で、そのために DMA を使う。
  uintptr_t stream_connect(uintptr_t src, uintptr_t dst, object_error &error);
  uintptr_t stream_disconnect(uintptr_t slot, object_error &error);
  // 接続を進める。★schedule() から呼ばれる。接続が 0 本なら数語読んで即戻る。
  void pump_connections();
  uintptr_t object_name(uintptr_t id, object_error &error);
  uintptr_t spawn_method(uintptr_t id, uintptr_t method, uintptr_t argument,
                         object_error &error);
  uintptr_t yield(object_error &error);
  uintptr_t sleep_us(uintptr_t microseconds, object_error &error);
  void exit_thread();
  uintptr_t kill_thread(uintptr_t thread, object_error &error);
  // そのオブジェクトのスレッドを走らせてよいコア (0 = どこでもよい)。
  uint32_t object_affinity(uintptr_t id) const;
  // ★軸 B (Q8)。呼び出し元ではなく**対象オブジェクトの性質**として引く —
  //   object_protection/object_affinity と同じ形。
  uint32_t object_region_base(uintptr_t id) const;
  uint32_t object_region_limit(uintptr_t id) const;
  // そのオブジェクトの**種別**。カーネルは台帳を持たないので、CALL / SPAWN の
  // たびにここから引いて申告する (どの ID が何の役かを知っているのはこちらだけ)。
  object_kind object_kind_of(uintptr_t id) const {
    return id < OBJECT_COUNT ? row_load(m_objects[id].kind) : object_kind::PLAIN;
  }
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
  void exit_method(uintptr_t target_depth, uintptr_t value, uintptr_t error);
  // 専用ハンドラ (HANDLER) からの子オブジェクトのメソッド終了転送 (重大指摘 2)
  void forward_child_exit(uintptr_t target_depth, uintptr_t value,
                          uintptr_t error);
  // 巻き戻さずにその場で答える (エラー返却)。
  void reply(object_error error, uintptr_t value);
  // 戻り先の絶対深さへ巻き戻す。失敗時は kernel_error を返す。
  uintptr_t return_to_depth(uint32_t target_depth, uintptr_t value,
                            uintptr_t error);

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
  // 記述子の表: [持ち主][index]。行の外に出した平坦な配列なので、持ち主の行を引かずに
  // (持ち主 × 16 + index) × 16 バイトのシフトだけで項目に届く。
  method_descriptor_entry m_descriptors[OBJECT_COUNT][METHOD_DESCRIPTOR_CAPACITY];
  // スレッドごとの「どのオブジェクトのために作ったか」。方針側の台帳なのでここ。
  uint16_t m_thread_object[THREAD_COUNT];
  // 停止要求が出ているスレッド。★要求と回収を分けるための旗 (D55)。
  //   KILL_THREAD は suspend を撃ってここに印を付けるだけで、記憶を返すのは
  //   schedule() が「どのコアでも走っていない」ことを確かめたあと。
  //   ★錠の外からも読む (schedule の走査) ので volatile。見落としても
  //     次の周回で拾えるだけなので、原子性までは要らない。
  volatile uint8_t m_kill_pending[THREAD_COUNT];
  // ★起床時刻と時限は**方針**なのでカーネルではなくここが持つ (D1)。
  //   カーネルは「渡す機構」しか持たず、誰をいつ走らせるかは知らない。
  uint64_t m_wake_at[THREAD_COUNT];
  uint32_t m_budget[THREAD_COUNT];
  // 貸したスタック (終わったスレッドから返してもらうために覚えておく)。
  uintptr_t m_thread_stack[THREAD_COUNT];
  // 台帳は静的な特権専用簿記 arena から借りる。スタック上端への配置と MPU
  // 境界分割は採らない: PMSAv8 は有効 region の重複を許さず、他スレッド台帳の
  // 穴を開けられないため、休止中の別スレッドの台帳を書けてしまう。RP2040 も同様。
  uintptr_t m_thread_ledger[THREAD_COUNT];
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
