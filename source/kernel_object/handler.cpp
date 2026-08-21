// ===========================================================================
//  カーネルオブジェクト — オブジェクトランドの svc ハンドラ本体
// ===========================================================================
//  スレッドモードで走る方針側。番号の意味を持つのはここだけで、カーネルは
//  何も解釈せずにここへ渡してくる。
//
//  ★カーネルのプリミティブを撃てるのはここだけ。オブジェクトは撃てないので、
//    カーネルは起動時に「今のネスト数」を渡し (第 5 引数)、オブジェクトは exit API に
//    「何段戻すか」を載せて撃ち、ここがその段数で巻き戻す (D5)。段数は必ず申告する。
#include "shizuku/kernel.hpp"
#include "shizuku/kernel_object.hpp"

namespace shizuku {

KERNEL_OBJECT kernel_object_instance;

// ★戻り口が撃つのはカーネルの RETURN なので、オブジェクトランドの exit API の番号は
//   それと同じ値でなければならない (経路判定だけが両者の意味を分ける)。
static_assert((uintptr_t)object_api::EXIT_METHOD == (uintptr_t)primitive::RETURN,
              "exit API の番号はカーネルの RETURN と一致させること");

// 実装より前に使う (下の入口が handle を呼ぶ) ので、特殊化の宣言を先に置く。
// これが無いと暗黙の実体化が先に起きて「実体化後の特殊化」になる。
template <>
uintptr_t KERNEL_OBJECT::handle(uintptr_t, uintptr_t, uintptr_t, uintptr_t);
template <> uint32_t KERNEL_OBJECT::claimed_depth() const;
template <> void KERNEL_OBJECT::init();
template <> uintptr_t KERNEL_OBJECT::handler_entry();
template <> void KERNEL_OBJECT::reply(object_error, uintptr_t);
template <>
uintptr_t KERNEL_OBJECT::create_object(uintptr_t, uintptr_t, uintptr_t,
                                       object_error &);
template <> uint32_t KERNEL_OBJECT::object_protection(uintptr_t) const;
template <> uint32_t KERNEL_OBJECT::object_affinity(uintptr_t) const;
template <>
uintptr_t KERNEL_OBJECT::export_method(uintptr_t, uintptr_t, object_error &);
template <>
uintptr_t KERNEL_OBJECT::call_method(uintptr_t, uintptr_t, uintptr_t,
                                     object_error &);
template <> void KERNEL_OBJECT::exit_method(uintptr_t, uintptr_t, uintptr_t);
template <>
uintptr_t KERNEL_OBJECT::spawn_method(uintptr_t, uintptr_t, uintptr_t,
                                      object_error &);
template <> uintptr_t KERNEL_OBJECT::yield_to(uintptr_t, object_error &);
template <> uintptr_t KERNEL_OBJECT::sleep_us(uintptr_t, object_error &);
template <>
uintptr_t KERNEL_OBJECT::run_for(uintptr_t, uintptr_t, object_error &);
template <> void KERNEL_OBJECT::exit_thread();
template <> bool KERNEL_OBJECT::schedule(uint32_t);
template <> void KERNEL_OBJECT::table_lock();
template <> void KERNEL_OBJECT::table_unlock();
template <> uintptr_t KERNEL_OBJECT::owner_of(uintptr_t, object_error &) const;

namespace {
// 錠の取り忘れ・放し忘れを型で防ぐ。この層は早期 return が多いので、手で放す形は
// いつか必ず 1 本落とす (落とすと系全体が止まるので、静かに壊れるより悪い)。
struct table_guard {
  table_guard() { kernel_object_instance.table_lock(); }
  ~table_guard() { kernel_object_instance.table_unlock(); }
  table_guard(const table_guard &) = delete;
  table_guard &operator=(const table_guard &) = delete;
};
} // namespace
template <> uintptr_t KERNEL_OBJECT::declare_name(uintptr_t, object_error &);
template <> uintptr_t KERNEL_OBJECT::object_name(uintptr_t, object_error &);
template <> uintptr_t KERNEL_OBJECT::stream_create(uintptr_t, object_error &);
template <> uintptr_t KERNEL_OBJECT::stream_open(uintptr_t, object_error &);
template <>
uintptr_t KERNEL_OBJECT::stream_bind(uintptr_t, uintptr_t, object_error &);
template <> KERNEL_OBJECT::lent_stack KERNEL_OBJECT::lend_boot_stack();
template <> bool KERNEL_OBJECT::start_secondary_core();
template <> uintptr_t KERNEL_OBJECT::memory_allocate(uintptr_t, object_error &);
template <> uintptr_t KERNEL_OBJECT::memory_release(uintptr_t, object_error &);
template <>
uintptr_t KERNEL_OBJECT::memory_hand_over(uintptr_t, uintptr_t, object_error &);
template <> uintptr_t KERNEL_OBJECT::memory_owner(uintptr_t, object_error &);
template <> void KERNEL_OBJECT::arena_init(arena &, uintptr_t, uintptr_t);
template <>
uintptr_t KERNEL_OBJECT::arena_allocate(arena &, uintptr_t, uintptr_t);
template <> bool KERNEL_OBJECT::arena_release(arena &, uintptr_t);

// カーネルが起こすハンドラの入口。**素の C 関数**でよい。
// ★ネストした呼び出しの情報をレジスタで持ち回らない: 番号は引数スロット (a0) に
//   そのまま乗っており、今のネスト数はカーネルが積んだフレームの段数そのもの
//   (kernel_instance.current_depth() で読める)。どちらも呼び出しフレーム側にあるので
//   ネストしても混ざらず、C 関数が callee-saved を潰しても壊れない。
//   参照実装はこれを callee-saved レジスタで渡し、C の引数へ変換する naked シムを
//   必要としていた — push 順と引数位置の対応を何度も間違えた箇所なので、構造ごと
//   無くすのが正しい。
uintptr_t handler_entry_point(uintptr_t a0, uintptr_t a1, uintptr_t a2,
                              uintptr_t a3) {
  return kernel_object_instance.handle(a0, a1, a2, a3);
}

template <> uintptr_t KERNEL_OBJECT::handler_entry() {
  return (uintptr_t)&handler_entry_point;
}

// ★カーネルの簿記に貸す記憶。**静的領域に置く**のが要点で、ここは region の外
//   (= 特権のみ) なので非特権オブジェクトから届かない。用意するのはこちら
//   (オブジェクトランド) で、置き場所の条件はカーネルが検査する。
alignas(8) static uint8_t g_bookkeeping_storage[8192];

template <> void KERNEL_OBJECT::init() {
  for (uintptr_t id = 0; id < OBJECT_COUNT; ++id) {
    m_objects[id].created = false;
    m_objects[id].flags = 0;
    m_object_name[id] = nullptr;
    for (uintptr_t method = 0; method < METHOD_COUNT; ++method)
      m_objects[id].methods[method] = nullptr;
  }
  for (uintptr_t thread = 0; thread < THREAD_COUNT; ++thread) {
    m_shadow[thread].depth = 0;
    m_thread_object[thread] = (uint16_t)ROOT_OBJECT;
    m_wake_at[thread] = 0;
    // 既定は量つき。自分から返さないスレッドがいても系が凍らないようにする
    // (量は quantum ではなく安全網 — 正常時はこれより早く返るので発火しない)。
    m_budget[thread] = DEFAULT_BUDGET_CYCLES;
  }
  for (uintptr_t core = 0; core < KERNEL::CORE_COUNT; ++core)
    m_rotor[core] = 0;
  m_table_lock = 0;
  for (uintptr_t thread = 0; thread < THREAD_COUNT; ++thread)
    m_thread_stack[thread] = 0;

  // 簿記用 arena: 静的領域 = 非特権から届かない場所。
  arena_init(m_bookkeeping, (uintptr_t)g_bookkeeping_storage,
             sizeof(g_bookkeeping_storage));
  // ★スレッド表をカーネルへ貸す。カーネルはこれを所有せず、渡された量が
  //   「何本作れるか」を決める (資源を持つのはオブジェクト = DESIGN §4.1 ルール 1)。
  const uintptr_t table_bytes =
      KERNEL::thread_record_bytes() * THREAD_COUNT;
  const uintptr_t table =
      arena_allocate(m_bookkeeping, table_bytes, ROOT_OBJECT);
  if (table == 0)
    KERNEL::BOARD::panic("no room for the thread table");
  kernel_instance.set_thread_storage((void *)table, table_bytes);

  // オブジェクト用 arena: 非特権からも届く必要があるのでヒープ側から取る。
  constexpr uintptr_t OBJECT_ARENA_BYTES = 128 * 1024;
  auto heap = kernel_instance.memory_manager.kernel_malloc(OBJECT_ARENA_BYTES);
  if (!heap)
    KERNEL::BOARD::panic("no room for the object arena");
  arena_init(m_objects_arena, (uintptr_t)heap.value(), OBJECT_ARENA_BYTES);
  // ブートスレッドが名乗るオブジェクトだけは最初から在るものとして扱う。
  m_objects[ROOT_OBJECT].created = true;
  m_object_name[ROOT_OBJECT] = "root";
  for (uintptr_t index = 0; index < STREAM_COUNT; ++index)
    m_streams[index] = nullptr;
}

// ★スレッド 0 のスタックも他と同じく arena から貸す。ここだけカーネルが自分で
//   malloc していると「スレッドの記憶は誰のものか」が二枚舌になる。
//   ブートスレッドは組み立てと自己テストを走らせるので少し厚めに取る。
template <> KERNEL_OBJECT::lent_stack KERNEL_OBJECT::lend_boot_stack() {
  constexpr uintptr_t BOOT_STACK_BYTES = 8192;
  const uintptr_t base =
      arena_allocate(m_objects_arena, BOOT_STACK_BYTES, ROOT_OBJECT);
  if (base == 0)
    KERNEL::BOARD::panic("no room for the boot stack");
  m_thread_stack[0] = base; // 記録はするが、スレッド 0 は終わらないので返らない
  return {base, BOOT_STACK_BYTES};
}

// ---- 2 本目のコア -----------------------------------------------------------
// ★このコアの「最初の 1 本」は他のスレッドと同じ扱い: 枠もスタックもここが用意する。
//   走らせる中身は**アイドル役** — 自分では何もせず、走れる相手が居れば渡すだけ。
//   ★アイドルが仕事を持つと、その仕事が他の全部の遅れになる (thread 0 と同じ規律)。
namespace {
uintptr_t g_secondary_stack = 0;
uintptr_t g_secondary_bytes = 0;
uint32_t g_secondary_thread = 0;

// 採用されたあとに走る本体。
void secondary_idle() {
  while (true)
    KERNEL::ARCH::syscall((uintptr_t)object_api::YIELD);
}

// core1 の入口。まだ pico-sdk が用意した足場の上に居るので、ここで自分を
// スレッドとして採用し、借りたスタックへ移る。
void secondary_boot() {
  kernel_instance.bootstrap_secondary(g_secondary_thread, secondary_idle,
                                      g_secondary_stack, g_secondary_bytes);
}
} // namespace

template <> bool KERNEL_OBJECT::start_secondary_core() {
  if (KERNEL::CORE_COUNT < 2)
    return false;
  const auto reserved = kernel_instance.reserve_thread();
  if (reserved.error != kernel_error::OK)
    return false;
  uintptr_t stack;
  {
    table_guard guard;
    stack = arena_allocate(m_objects_arena, THREAD_STACK_BYTES, ROOT_OBJECT);
    if (stack != 0)
      m_thread_stack[reserved.thread] = stack;
  }
  if (stack == 0) {
    kernel_instance.release(reserved.thread);
    return false;
  }
  g_secondary_thread = reserved.thread;
  g_secondary_stack = stack;
  g_secondary_bytes = THREAD_STACK_BYTES;
  m_thread_object[reserved.thread] = (uint16_t)ROOT_OBJECT;
  // ★アイドルは時限を持たない。持ち分を渡す側であって、借りる側ではない。
  m_budget[reserved.thread] = DEFAULT_BUDGET_CYCLES;
  KERNEL::BOARD::launch_core(secondary_boot);
  return true;
}

// ★両側チェックの申告値を**自分の台帳から**計算する (§9.3)。
//   カーネルのフレーム段数を写して返すのでは検算にならない。こちらは
//   「呼び出しを何段積んだか」を独立に数えており、フレームは 1 段あたり 2 枚
//   (呼び先の枠 + その中の svc を運ぶ枠) なので、ハンドラとして走っている今の
//   段数は 2 * 呼び出し段数 + 1 になるはず。ここがズレたら台帳とカーネルの
//   どちらかが壊れているので、カーネルが 1 枚も落とさずに弾く。
template <> uint32_t KERNEL_OBJECT::claimed_depth() const {
  return 2u * m_shadow[kernel_instance.current_thread_id()].depth + 1u;
}

// 巻き戻さずにその場で答える。**エラーを黙って捨てない**ための共通口 (D12)。
template <> void KERNEL_OBJECT::reply(object_error error, uintptr_t value) {
  ARCH::syscall((uintptr_t)primitive::RETURN, 1, value, (uintptr_t)error,
                claimed_depth());
  // 成功すれば戻らない。
}

template <>
uintptr_t KERNEL_OBJECT::create_object(uintptr_t id, uintptr_t entry,
                                       uintptr_t flags, object_error &error) {
  if (id >= OBJECT_COUNT || id == ROOT_OBJECT) {
    error = object_error::BAD_OBJECT;
    return 0;
  }
  const char *taken_by = nullptr;
  {
    table_guard guard;
    if (!m_objects[id].created) {
      m_objects[id].created = true;
      m_objects[id].flags = (uint32_t)flags;
      // 最初のメソッドは生成側が与える (オブジェクト自身はまだ走っていないので
      // 自分では登録できない)。以後は EXPORT_METHOD で自分が増やす。
      m_objects[id].methods[0] = (method_t)entry;
      return id;
    }
    taken_by = m_object_name[id];
  }
  {
    // ★**無音で失敗させない**。番号の衝突は今日 2 回起きて 2 回とも黙っていた
    //   (flash_fs 11 × 非特権プローブ 11 → 「非特権のはずが特権」という別物の
    //   失敗に化けた。sleeper 6 × blink 6 → 揺らぎの計測が止まったのに
    //   36 passed/0 failed のまま)。**戻り値を見ない呼び出し側が必ず居る**ので、
    //   気づけるかどうかを呼び出し側の行儀に賭けない。名乗りがここで効く —
    //   「6 は既に誰それが持っている」と言えるようになる。
    // ★印字は錠の**外**で行う。診断は遅く、ときに待つ (USB) ので、握ったまま
    //   呼ぶと相手のコアがその間ずっと錠を待つことになる。
    KERNEL::BOARD::diag_printf(
        "[KOBJ] object %lu is already taken by '%s' — create refused\n",
        (unsigned long)id, taken_by != nullptr ? taken_by : "(unnamed)");
    error = object_error::ALREADY_EXISTS;
    return 0;
  }
}

// そのオブジェクトを走らせるときの保護指定。
// ★既定は特権のまま。非特権で走れるのは「状態をヒープに置き、標準ライブラリを
//   直に呼ばない」オブジェクトだけで (静的データもペリフェラルも region の外 =
//   特権のみになるため。DESIGN §11.2.2)、今それを満たすのは宣言した相手だけ。
//   arena が入って既定を反転できるようになるまでは、opt-in にしておく。
template <> uint32_t KERNEL_OBJECT::object_protection(uintptr_t id) const {
  return (m_objects[id].flags & OBJECT_UNPRIVILEGED) ? PROTECTION_UNPRIVILEGED
                                                     : PROTECTION_PRIVILEGED;
}

// ★どこでも走れるのが既定。固定されているのは「機械の都合でそうなっている」
//   相手だけで、それは宣言してもらう (気をつけて置く、では守れない)。
template <> uint32_t KERNEL_OBJECT::object_affinity(uintptr_t id) const {
  return (uint32_t)((m_objects[id].flags & OBJECT_AFFINITY_MASK) >>
                    OBJECT_AFFINITY_SHIFT);
}

template <>
uintptr_t KERNEL_OBJECT::export_method(uintptr_t method, uintptr_t entry,
                                       object_error &error) {
  // ★誰のものとして登録するかは**発行元から導出する** (名乗らせない)。
  //   これが「呼び出し元 identity が偽装不能」であることの最初の使い道。
  const uintptr_t self = current_object(kernel_instance.current_thread_id());
  if (method >= METHOD_COUNT) {
    error = object_error::BAD_METHOD;
    return 0;
  }
  table_guard guard;
  m_objects[self].methods[method] = (method_t)entry;
  return self;
}

template <>
uintptr_t KERNEL_OBJECT::call_method(uintptr_t id, uintptr_t method,
                                     uintptr_t argument, object_error &error) {
  const uint32_t thread = kernel_instance.current_thread_id();
  const uintptr_t caller = current_object(thread);
  if (id >= OBJECT_COUNT || !m_objects[id].created) {
    error = object_error::BAD_OBJECT;
    return 0;
  }
  if (method >= METHOD_COUNT) {
    error = object_error::BAD_METHOD;
    return 0;
  }
  const method_t entry = m_objects[id].methods[method];
  if (entry == nullptr) {
    // 未 export は panic ではなくエラー (呼び出し元は待って再試行できる)。
    error = object_error::UNDECLARED_METHOD;
    return 0;
  }
  shadow_t &shadow = m_shadow[thread];
  if (shadow.depth >= MAX_DEPTH) {
    error = object_error::NO_STACK;
    return 0;
  }

  call_request request{};
  request.entry_pc = (uintptr_t)entry;
  request.protection = object_protection(id);
  request.args[0] = argument;

  // 台帳へ先に積む (カーネルが失敗したら戻す)。
  shadow.object[shadow.depth] = (uint16_t)id;
  shadow.caller[shadow.depth] = (uint16_t)caller;
  shadow.depth++;
  const auto result =
      ARCH::syscall((uintptr_t)primitive::CALL, (uintptr_t)&request);
  // ★成功した場合、この syscall は呼び先が戻ってから返る。戻ってきた時点で
  //   台帳は exit_method 側が既に戻している。
  if (result.error != (uintptr_t)kernel_error::OK) {
    shadow.depth--;
    error = result.error == (uintptr_t)kernel_error::NO_STACK
                ? object_error::NO_STACK
                : object_error::BAD_OBJECT;
    return 0;
  }
  return result.value;
}

template <>
void KERNEL_OBJECT::exit_method(uintptr_t levels, uintptr_t value,
                                uintptr_t error) {
  const uint32_t thread = kernel_instance.current_thread_id();
  const uint32_t depth = claimed_depth(); // 台帳から申告する値 (落とす前に取る)
  shadow_t &shadow = m_shadow[thread];
  // 呼び出し 1 段につきフレームは 2 枚 (この戻りを運んだ枠 + 戻ろうとしている
  // 呼び先の枠)。**枚数を知っているのは枠を積んだこちら側**なので、オブジェクトは
  // 「何段畳むか」だけを言い、変換はここで行う (D5)。段数は必ず申告する (§9.3)。
  const uintptr_t pops = levels == 0 ? 1 : levels;
  const uintptr_t count = 2 * pops;
  // ★戻り先が無い = スレッドの入口が return した。呼び出しを畳むのではなく
  //   「このスレッドが終わった」ということなので、そう扱う (§9.4 の exit)。
  //   自分の枠しか落とせないので、全オブジェクトに開放しても昇格にはならない。
  if (kernel_instance.current_depth() < count) {
    exit_thread();
    return;
  }
  // 巻き戻しに成功するとここへは戻らないので、台帳は**先に**落としておく。
  shadow.depth = shadow.depth >= pops ? shadow.depth - (uint32_t)pops : 0;
  const auto result =
      ARCH::syscall((uintptr_t)primitive::RETURN, count, value, error, depth);
  // ★ここへ戻ってきた = 巻き戻しが検算で弾かれた。台帳を元へ戻し、発行元へ
  //   エラーとして返す (系は落とさない = I-9)。
  shadow.depth += (uint32_t)pops;
  reply(object_error::UNWIND_REJECTED, (uintptr_t)result.error);
}

// ---- スレッドと実行権の方針 -------------------------------------------------
// ★カーネルは「渡す機構」しか持たない。誰にいつ渡すか、いつ取り上げるかを決めるのは
//   ここ (D1 / DESIGN §2 P1「機構はカーネル、方針はオブジェクト」)。
template <>
uintptr_t KERNEL_OBJECT::spawn_method(uintptr_t id, uintptr_t method,
                                      uintptr_t argument, object_error &error) {
  if (id >= OBJECT_COUNT || !m_objects[id].created) {
    error = object_error::BAD_OBJECT;
    return 0;
  }
  if (method >= METHOD_COUNT) {
    error = object_error::BAD_METHOD;
    return 0;
  }
  const method_t entry = m_objects[id].methods[method];
  if (entry == nullptr) {
    error = object_error::UNDECLARED_METHOD;
    return 0;
  }
  // ★スタックも**こちらが用意して貸す**。どれだけの深さを許すかは方針であって、
  //   カーネルが決めることではない。非特権スレッドも自分のスタックには触れないと
  //   困るので、オブジェクト用 arena (非特権から届く側) から取る。
  table_lock();
  const uintptr_t stack =
      arena_allocate(m_objects_arena, THREAD_STACK_BYTES, id);
  table_unlock();
  if (stack == 0) {
    error = object_error::NO_MEMORY;
    return 0;
  }
  KERNEL::spawn_request request{};
  request.entry_pc = (uintptr_t)entry;
  request.argument = argument;
  request.protection = object_protection(id);
  request.affinity = object_affinity(id);
  request.stack_base = stack;
  request.stack_bytes = THREAD_STACK_BYTES;
  const auto spawned = kernel_instance.spawn(request);
  if (spawned.error != kernel_error::OK) {
    table_lock();
    arena_release(m_objects_arena, stack); // 使わなかったので返す
    table_unlock();
    error = object_error::NO_THREAD;
    return 0;
  }
  {
    table_guard guard;
    m_thread_stack[spawned.thread] = stack;
  }
  // 新しいスレッドは「そのオブジェクトとして」走り始める。台帳の底をそう置く。
  m_thread_object[spawned.thread] = (uint16_t)id;
  m_shadow[spawned.thread].depth = 0;
  m_wake_at[spawned.thread] = 0;
  m_budget[spawned.thread] = DEFAULT_BUDGET_CYCLES;
  return spawned.thread;
}

// 次に走らせるスレッドを選んで渡す。戻り = 誰かへ渡せたか。
// ★ここが方針の中心。カーネルは「渡す機構」(バトン渡しと時限つきの貸し出し) しか
//   持たず、どちらでどれだけ渡すかはここが決める:
//     時限あり (既定) → 貸す。自分から返さないスレッドがいても期限で戻る
//     時限 0          → バトン渡し。返ってこない可能性を承知で渡す相手だけに使う
// ★回転子から順に見る。自分の直後だけを見ると同じ相手ばかり選ばれて飢餓が出る。
template <> bool KERNEL_OBJECT::schedule(uint32_t self) {
  using state_t = KERNEL::THREAD::state_t;
  const uint32_t core = KERNEL::BOARD::core_num();
  const uint64_t now = KERNEL::BOARD::time_us();
  for (uint32_t step = 1; step <= THREAD_COUNT; ++step) {
  const uint32_t candidate = (m_rotor[core] + step) % THREAD_COUNT;
    if (candidate == self)
      continue;
    // ★終わったスレッドの記憶をここで回収する。貸したのはこちらなので、返させるのも
    //   こちらの仕事 (DESIGN §4.1 ルール 3「自身が保有する資源を自由に開放できる」)。
    //   走り終えた本人には自分のスタックを返せない (その上で走っているため)。
    if (kernel_instance.thread_state(candidate) == state_t::TERMINATED &&
        m_thread_stack[candidate] != 0) {
      // ★arena は共有なので錠が要る。この区間に syscall は 1 つも無い
      //   (握ったまま実行権を手放すと、相手のコアが走っていない持ち主を待つ)。
      table_lock();
      if (m_thread_stack[candidate] == 0) { // 別のコアが先に回収していた
        table_unlock();
        continue;
      }
      arena_release(m_objects_arena, m_thread_stack[candidate]);
      m_thread_stack[candidate] = 0;
      // 台帳も畳む。枠は使い回されるので、前の住人の名残りを残してはいけない
      // (次の住人が他人の identity を継いでしまう)。
      m_shadow[candidate].depth = 0;
      m_thread_object[candidate] = (uint16_t)ROOT_OBJECT;
      m_wake_at[candidate] = 0;
      m_budget[candidate] = DEFAULT_BUDGET_CYCLES;
      kernel_instance.release(candidate); // 枠も返す (再利用できるようにする)
      table_unlock();
      continue;
    }
    if (kernel_instance.thread_state(candidate) != state_t::READY)
      continue;
    // ★このコアで走ってよい相手だけを候補にする。claim も弾いてくれるが、
    //   弾かれてから次を探すのは無駄で、しかも相手の枠を一瞬 CAS で叩くことになる。
    if ((kernel_instance.thread_affinity(candidate) & (1u << core)) == 0)
      continue;
    if (m_wake_at[candidate] > now)
      continue; // 眠っている相手は起こさない
    const uint32_t budget = m_budget[candidate];
    const auto result =
        budget == 0
            ? ARCH::syscall((uintptr_t)primitive::SWITCH, candidate)
            : ARCH::syscall((uintptr_t)primitive::GRANT, candidate, budget);
    if (result.error == (uintptr_t)kernel_error::OK) {
      m_rotor[core] = candidate;
      return true;
    }
    // ★自分の持ち分が尽きたのなら、次の候補を試しても同じ答えしか返らない。
    //   ここで諦めて戻り、外側の貸しが切れるに任せる (空回りしない)。
    if (result.error == (uintptr_t)kernel_error::GRANT_TOO_SMALL)
      return false;
  }
  return false;
}

// 締切まで自分を走らせない。★待っている間は他へ CPU を渡す — 「待つ」ことで
// 他の仕事が止まるなら、それは待ちではなく占有になってしまう。
template <>
uintptr_t KERNEL_OBJECT::sleep_us(uintptr_t microseconds, object_error &error) {
  (void)error;
  const uint32_t self = kernel_instance.current_thread_id();
  const uint64_t deadline = KERNEL::BOARD::time_us() + (uint64_t)microseconds;
  m_wake_at[self] = deadline;
  while ((int64_t)(deadline - KERNEL::BOARD::time_us()) > 0) {
    // 借り手として走っているなら、貸し手へ返すのが先 (又貸しはしない)。
    if (kernel_instance.grant_active()) {
      ARCH::syscall((uintptr_t)primitive::SWITCH, 0);
      continue;
    }
    schedule(self); // 誰も居なければ締切まで空回りする
  }
  m_wake_at[self] = 0;
  return 0;
}

template <>
uintptr_t KERNEL_OBJECT::yield_to(uintptr_t target, object_error &error) {
  const uint32_t self = kernel_instance.current_thread_id();
  // 借り手として走っているなら、譲る先を選ぶ権利は無い — 貸し手へ返すだけ
  // (カーネルの SWITCH が対象を無視してそう振る舞う)。
  if (kernel_instance.grant_active()) {
    ARCH::syscall((uintptr_t)primitive::SWITCH, 0);
    return 0;
  }
  if (target != 0) {
    const auto result = ARCH::syscall((uintptr_t)primitive::SWITCH, target);
    if (result.error != (uintptr_t)kernel_error::OK) {
      error = object_error::NOT_RUNNABLE;
      return 0;
    }
    return target;
  }
  return schedule(self) ? 1 : 0; // 誰も居なければ自分が続ける
}

template <>
uintptr_t KERNEL_OBJECT::run_for(uintptr_t thread, uintptr_t cycles,
                                 object_error &error) {
  const auto result =
      ARCH::syscall((uintptr_t)primitive::GRANT, thread, cycles);
  if (result.error == (uintptr_t)kernel_error::GRANT_TOO_SMALL) {
    // ★「相手が走れない」ではなく「こちらに配れる持ち分が無い」。区別しないと、
    //   スケジューラが次の候補を延々と試して空回りする。
    error = object_error::NO_TIME;
    return 0;
  }
  if (result.error != (uintptr_t)kernel_error::OK) {
    error = object_error::NOT_RUNNABLE;
    return 0;
  }
  return result.value; // grant_end (0 = 期限切れ, 1 = 相手が返した)
}

// スレッドを終える。**このスレッドはもう走らない**ので、終える前に次の相手へ
// 実行権を渡す。渡せなければ、この後カーネルが誰も選べないまま戻ることになるので、
// 借り手として走っていたなら貸し手へ返す道が残っている。
template <> void KERNEL_OBJECT::exit_thread() {
  const uint32_t self = kernel_instance.current_thread_id();
  m_shadow[self].depth = 0;
  kernel_instance.terminate(self);
  if (kernel_instance.grant_active()) {
    ARCH::syscall((uintptr_t)primitive::SWITCH, 0); // 貸し手へ返す
    return;
  }
  if (!schedule(self)) {
    // 誰も走れない。終わったスレッドの上で止まるしかないので、そのことを言う。
    KERNEL::BOARD::diag_printf("[KOBJ] thread %lu exited, nothing runnable\n",
                               (unsigned long)self);
    reply(object_error::OK, 0);
  }
}

// ---- 共有台帳の錠 ----------------------------------------------------------
// ★1 コアのときは無償だった相互排除を、2 コアでは明示的に取る (kernel_object.hpp)。
//   区間は短く、**syscall を含まない**ものだけ。握ったまま実行権を手放すと、
//   相手のコアが「走っていない持ち主」を待って回り続ける。
template <> void KERNEL_OBJECT::table_lock() {
  while (!ARCH::cas32(&m_table_lock, 0u, 1u)) {
    // 待つ。★ここで yield しない — 錠を待っている相手は必ず走っており
    //   (握ったまま手放さない規律があるので)、待ちは有界。yield すると
    //   「錠を待つために実行権を配る」ことになり、順序が絡んで長くなる。
  }
}

template <> void KERNEL_OBJECT::table_unlock() {
  ARCH::store_release32(&m_table_lock, 0u);
}



// ---- 名乗り ----------------------------------------------------------------
// ★誰として登録するかは**発行元から導出する** (EXPORT_METHOD と同じ作法)。
//   引数で対象を指定させると他人の名を騙れてしまう。
// ★★保持するだけで**比較しない**。比較 = 文字列処理を入れた瞬間に、オブジェクト
//   システムが定数時間で抜けられなくなる (D26)。名前で引くのは System Object の
//   仕事で、起動したら OBJECT_NAME で読み出して自分の索引を作り、引き継ぐ。
template <>
uintptr_t KERNEL_OBJECT::declare_name(uintptr_t name, object_error &error) {
  const uintptr_t self = current_object(kernel_instance.current_thread_id());
  if (name == 0) {
    error = object_error::BAD_OBJECT;
    return 0;
  }
  // 付け直しは断る。控えた名が別物を指すようになると、名前を頼りにした側が
  // 静かに間違った相手を掴む。
  table_guard guard;
  if (m_object_name[self] != nullptr) {
    error = object_error::ALREADY_NAMED;
    return 0;
  }
  m_object_name[self] = (const char *)name;
  return self;
}

template <>
uintptr_t KERNEL_OBJECT::object_name(uintptr_t id, object_error &error) {
  if (id >= OBJECT_COUNT || !m_objects[id].created) {
    error = object_error::BAD_OBJECT;
    return 0;
  }
  return (uintptr_t)m_object_name[id]; // 0 = 無名
}

// ---- ストリームの制御プレーン ----------------------------------------------
// ★番号は**こちらが割り当てる**。呼ぶ側に選ばせると、オブジェクト番号で 2 回
//   踏んだのと同じ衝突が起きる (D28)。返した番号を使ってもらう。
template <>
uintptr_t KERNEL_OBJECT::stream_create(uintptr_t desc, object_error &error) {
  if (desc == 0) {
    error = object_error::BAD_STREAM;
    return 0;
  }
  table_guard guard;
  for (uintptr_t index = 0; index < STREAM_COUNT; ++index) {
    if (m_streams[index] != nullptr)
      continue;
    m_streams[index] = (stream::descriptor *)desc;
    return index;
  }
  error = object_error::NO_STREAM;
  return 0;
}

template <>
uintptr_t KERNEL_OBJECT::stream_open(uintptr_t id, object_error &error) {
  table_guard guard;
  if (id >= STREAM_COUNT || m_streams[id] == nullptr) {
    error = object_error::BAD_STREAM;
    return 0;
  }
  return (uintptr_t)m_streams[id];
}

// ★席は 1 つずつしか無い。埋まっていたら断る — これが SPSC の強制で、
//   「規約で守る」形にしないための唯一の仕掛け (参照実装の直接ハンドル型は
//   ここが無く、役割を規約で守れと書いてあった)。
template <>
uintptr_t KERNEL_OBJECT::stream_bind(uintptr_t id, uintptr_t which,
                                     object_error &error) {
  const uintptr_t self = current_object(kernel_instance.current_thread_id());
  table_guard guard;
  if (id >= STREAM_COUNT || m_streams[id] == nullptr) {
    error = object_error::BAD_STREAM;
    return 0;
  }
  stream::descriptor *target = m_streams[id];
  uint32_t &seat = which == (uintptr_t)stream::role::PRODUCER
                       ? target->producer
                       : target->consumer;
  if (seat != stream::NO_OWNER && seat != (uint32_t)self) {
    error = object_error::SEAT_TAKEN;
    return 0;
  }
  seat = (uint32_t)self;
  return self;
}

// ---- メモリの授受 -----------------------------------------------------------
// ★参照実装の「オブジェクトごとに 16 語」は簡易な一形態でしかない。ここでは
//   大きさを指定して借り、**持ち主を付け替えられる**形にする。持ち主が付いていれば
//   「このオブジェクトが何を持っているか」が言えるので、会計にも解放にも使える。
template <>
uintptr_t KERNEL_OBJECT::memory_allocate(uintptr_t bytes, object_error &error) {
  const uintptr_t self = current_object(kernel_instance.current_thread_id());
  table_guard guard;
  const uintptr_t handle = arena_allocate(m_objects_arena, bytes, self);
  if (handle == 0)
    error = object_error::NO_MEMORY;
  return handle;
}

template <>
uintptr_t KERNEL_OBJECT::memory_release(uintptr_t handle, object_error &error) {
  const uintptr_t self = current_object(kernel_instance.current_thread_id());
  table_guard guard;
  // ★他人のものは返せない。持ち主を記録している意味はここに出る。
  if (owner_of(handle, error) != self) {
    error = object_error::NOT_OWNER;
    return 0;
  }
  if (!arena_release(m_objects_arena, handle)) {
    error = object_error::BAD_MEMORY;
    return 0;
  }
  return 1;
}

// 持ち主を付け替える = 渡す。**渡した側は以後それを返せない**ので、
// 「渡したのにまだ自分のもの」という曖昧さが残らない。
template <>
uintptr_t KERNEL_OBJECT::memory_hand_over(uintptr_t handle, uintptr_t receiver,
                                          object_error &error) {
  const uintptr_t self = current_object(kernel_instance.current_thread_id());
  table_guard guard;
  if (receiver >= OBJECT_COUNT || !m_objects[receiver].created) {
    error = object_error::BAD_OBJECT;
    return 0;
  }
  if (owner_of(handle, error) != self) {
    error = object_error::NOT_OWNER;
    return 0;
  }
  block *header = (block *)(handle - sizeof(block));
  header->owner = (uint16_t)receiver;
  return receiver;
}

// 錠を持っている前提の素の実装 (kernel_object.hpp の注記を参照)。
template <>
uintptr_t KERNEL_OBJECT::owner_of(uintptr_t handle, object_error &error) const {
  if (handle <= m_objects_arena.base ||
      handle >= m_objects_arena.base + m_objects_arena.bytes) {
    error = object_error::BAD_MEMORY;
    return NO_OBJECT;
  }
  const block *header = (const block *)(handle - sizeof(block));
  if (!header->used) {
    error = object_error::BAD_MEMORY;
    return NO_OBJECT;
  }
  return header->owner;
}

template <>
uintptr_t KERNEL_OBJECT::memory_owner(uintptr_t handle, object_error &error) {
  table_guard guard;
  return owner_of(handle, error);
}

template <>
uintptr_t KERNEL_OBJECT::handle(uintptr_t number, uintptr_t a1, uintptr_t a2,
                                uintptr_t a3) {
  object_error error = object_error::OK;
  uintptr_t value = 0;
  switch ((object_api)number) {
  case object_api::CREATE_OBJECT:
    value = create_object(a1, a2, a3, error);
    break;
  case object_api::EXPORT_METHOD:
    value = export_method(a1, a2, error);
    break;
  case object_api::CALL_METHOD:
    value = call_method(a1, a2, a3, error);
    break;
  case object_api::EXIT_METHOD:
    // カーネルの戻り口が撃った svc がここへ届く (a1 = 畳む段数, a2 = 戻り値,
    // a3 = エラー)。巻き戻しに成功すればここから戻らない。
    exit_method(a1, a2, a3);
    return 0;
  case object_api::GET_CURRENT_OBJECT:
    value = current_object(kernel_instance.current_thread_id());
    break;
  case object_api::GET_CALLER_OBJECT:
    value = caller_object(kernel_instance.current_thread_id());
    break;
  case object_api::SPAWN:
    value = spawn_method(a1, a2, a3, error);
    break;
  case object_api::YIELD:
    value = yield_to(a1, error);
    break;
  case object_api::RUN_FOR:
    value = run_for(a1, a2, error);
    break;
  case object_api::EXIT_THREAD:
    exit_thread();
    return 0;
  case object_api::SLEEP_US:
    value = sleep_us(a1, error);
    break;
  case object_api::MEMORY_ALLOCATE:
    value = memory_allocate(a1, error);
    break;
  case object_api::MEMORY_RELEASE:
    value = memory_release(a1, error);
    break;
  case object_api::MEMORY_HAND_OVER:
    value = memory_hand_over(a1, a2, error);
    break;
  case object_api::MEMORY_OWNER:
    value = memory_owner(a1, error);
    break;
  case object_api::DECLARE_NAME:
    value = declare_name(a1, error);
    break;
  case object_api::OBJECT_NAME:
    value = object_name(a1, error);
    break;
  case object_api::STREAM_CREATE:
    value = stream_create(a1, error);
    break;
  case object_api::STREAM_OPEN:
    value = stream_open(a1, error);
    break;
  case object_api::STREAM_BIND:
    value = stream_bind(a1, a2, error);
    break;
  case object_api::SET_BUDGET:
    if (a1 < THREAD_COUNT)
      m_budget[a1] = (uint32_t)a2;
    break;
  default:
    // ★未知の番号は**ここの語彙**。黙って捨てず必ずエラーで返す
    //   (DESIGN §11.2.0 の「無音で消えて誤認した」事故の対策)。
    error = object_error::UNKNOWN_API;
    break;
  }
  if (error != object_error::OK)
    reply(error, 0);
  // 普通に return すると、カーネルの戻り口が自分の枠を 1 枚落として発行元へ返る。
  return value;
}

} // namespace shizuku
