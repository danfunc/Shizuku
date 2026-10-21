// ===========================================================================
//  カーネルオブジェクト — オブジェクトランドの svc ハンドラ本体
// ===========================================================================
//  スレッドモードで走る方針側。番号の意味を持つのはここだけで、カーネルは
//  何も解釈せずにここへ渡してくる。
//
//  ★カーネルのプリミティブを解釈できるのは登録済み KOBJ だけ。dispatch の第 5 引数は
//    呼出し鎖の unwind floor、exit API は絶対戻り深さ d を指定する。KOBJ は差分を
//    RETURN count にし、kernel が ledger の floor と照合する。
#include "shizuku/kernel.hpp"
#include "shizuku/kernel_object.hpp"
#include "shizuku/svc_attributes.hpp"
#if defined(SHIZUKU_CRASH_RECORD)
#include "shizuku/objects/usb_cdc.hpp"
#endif

#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_BOOT_STAGE_TRACE) && SHIZUKU_BOOT_STAGE_TRACE > 0
extern "C" void shizuku_boot_trace_phase(uint32_t);
#endif

namespace shizuku {

KERNEL_OBJECT kernel_object_instance;

#if defined(SHIZUKU_CRASH_RECORD)
namespace {
inline void kobj_diag_mark(objects::boot_diag_breadcrumb id) {
  objects::usb_cdc_boot_diag_breadcrumb_at(
      id, (uintptr_t)__builtin_return_address(0));
}
}
#endif

// ★戻り口が撃つのはカーネルの RETURN なので、オブジェクトランドの exit API の番号は
//   それと同じ値でなければならない (経路判定だけが両者の意味を分ける)。
static_assert((uintptr_t)object_api::EXIT_METHOD == (uintptr_t)primitive::RETURN,
              "exit API の番号はカーネルの RETURN と一致させること");

// 実装より前に使う (下の入口が handle を呼ぶ) ので、特殊化の宣言を先に置く。
// これが無いと暗黙の実体化が先に起きて「実体化後の特殊化」になる。
template <>
uintptr_t KERNEL_OBJECT::handle(uintptr_t, uintptr_t, uintptr_t, uintptr_t,
                                uintptr_t);
template <> uintptr_t KERNEL_OBJECT::current_object(uint32_t) const;
template <> uintptr_t KERNEL_OBJECT::caller_object(uint32_t) const;
template <> void KERNEL_OBJECT::init();
template <> uintptr_t KERNEL_OBJECT::handler_entry();
template <> void KERNEL_OBJECT::reply(object_error, uintptr_t);
template <>
uintptr_t KERNEL_OBJECT::create_object(uintptr_t, uintptr_t, uintptr_t,
                                       object_error &);
template <> uint32_t KERNEL_OBJECT::object_protection(uintptr_t) const;
template <> uint32_t KERNEL_OBJECT::object_affinity(uintptr_t) const;
template <> uint32_t KERNEL_OBJECT::object_region_base(uintptr_t) const;
template <> uint32_t KERNEL_OBJECT::object_region_limit(uintptr_t) const;
template <>
uintptr_t KERNEL_OBJECT::grant_region(uintptr_t, uintptr_t, uintptr_t,
                                      object_error &);
template <>
uintptr_t KERNEL_OBJECT::set_object_affinity(uintptr_t, uintptr_t,
                                             object_error &);
template <>
uintptr_t KERNEL_OBJECT::export_method(uintptr_t, uintptr_t, object_error &);
template <>
uintptr_t KERNEL_OBJECT::call_method(uintptr_t, uintptr_t, uintptr_t,
                                     object_error &);
template <> void KERNEL_OBJECT::exit_method(uintptr_t, uintptr_t, uintptr_t);
template <> uintptr_t KERNEL_OBJECT::return_to_depth(uint32_t, uintptr_t,
                                                     uintptr_t);
template <> void KERNEL_OBJECT::forward_child_exit(uintptr_t, uintptr_t,
                                                   uintptr_t);
template <>
uintptr_t KERNEL_OBJECT::spawn_method(uintptr_t, uintptr_t, uintptr_t,
                                      object_error &);
template <> uintptr_t KERNEL_OBJECT::yield(object_error &);
template <> uintptr_t KERNEL_OBJECT::sleep_us(uintptr_t, object_error &);
template <>
uintptr_t KERNEL_OBJECT::run_for(uintptr_t, uintptr_t, object_error &);
template <> void KERNEL_OBJECT::exit_thread();
template <>
uintptr_t KERNEL_OBJECT::kill_thread(uintptr_t thread, object_error &error);
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
template <> uintptr_t KERNEL_OBJECT::stream_create(uintptr_t, uintptr_t, uintptr_t, object_error &);
template <> uintptr_t KERNEL_OBJECT::stream_open(uintptr_t, object_error &);
template <>
uintptr_t KERNEL_OBJECT::stream_bind(uintptr_t, uintptr_t, object_error &);
template <> uintptr_t KERNEL_OBJECT::stream_destroy(uintptr_t, object_error &);
template <> uintptr_t KERNEL_OBJECT::stream_push_n(uintptr_t, uintptr_t, object_error &);
template <> uintptr_t KERNEL_OBJECT::stream_pop_n(uintptr_t, uintptr_t, object_error &);
template <> uintptr_t KERNEL_OBJECT::stream_available(uintptr_t, object_error &);
template <> KERNEL_OBJECT::stream_entry *KERNEL_OBJECT::stream_lookup(uintptr_t);
template <> bool KERNEL_OBJECT::stream_pin(uintptr_t, KERNEL_OBJECT::stream_entry *&);
template <> void KERNEL_OBJECT::stream_unpin(KERNEL_OBJECT::stream_entry &);
template <> bool KERNEL_OBJECT::stream_stack_range(uint32_t, uintptr_t, uintptr_t) const;
template <> uintptr_t KERNEL_OBJECT::stream_push_kernel(uintptr_t, const void *, uint32_t);
template <> bool KERNEL_OBJECT::stream_bind_kernel(uintptr_t);
template <>
uintptr_t KERNEL_OBJECT::stream_connect(uintptr_t, uintptr_t, object_error &);
template <>
uintptr_t KERNEL_OBJECT::stream_disconnect(uintptr_t, object_error &);
template <> void KERNEL_OBJECT::pump_connections();
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
// ★番号は a0 に乗り、戻り境界 (unwind floor) は第 5 引数でカーネルが渡す。
//   どちらも呼び出しフレーム側にあるので
//   ネストしても混ざらず、C 関数が callee-saved を潰しても壊れない。
//   参照実装はこれを callee-saved レジスタで渡し、C の引数へ変換する naked シムを
//   必要としていた — push 順と引数位置の対応を何度も間違えた箇所なので、構造ごと
//   無くすのが正しい。
uintptr_t handler_entry_point(uintptr_t a0, uintptr_t a1, uintptr_t a2,
                              uintptr_t a3, uintptr_t unwind_floor) {
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_BOOT_STAGE_TRACE) && SHIZUKU_BOOT_STAGE_TRACE > 0
  shizuku_boot_trace_phase(419);
#endif
  return kernel_object_instance.handle(a0, a1, a2, a3, unwind_floor);
}

template <> uintptr_t KERNEL_OBJECT::handler_entry() {
  return (uintptr_t)&handler_entry_point;
}

// ★カーネルの簿記に貸す記憶。**静的領域に置く**のが要点で、ここは region の外
//   (= 特権のみ) なので非特権オブジェクトから届かない。用意するのはこちら
//   (オブジェクトランド) で、置き場所の条件はカーネルが検査する。
static_assert(sizeof(KERNEL::call_ledger_entry) == 16,
              "call ledger entry must be 16 bytes on supported targets");
static_assert(KERNEL_OBJECT::MAX_DEPTH <= KERNEL::THREAD::CALL_IDENTITY_CAPACITY,
              "per-thread call identity snapshot stack must cover MAX_DEPTH");
#if defined(SHIZUKU_C5_BLE)
alignas(8) static uint8_t g_bookkeeping_storage[16 * 1024];
#else
alignas(8) static uint8_t g_bookkeeping_storage[65536];
#endif
static_assert(KERNEL::thread_record_bytes() * KERNEL_OBJECT::THREAD_COUNT +
                  sizeof(KERNEL_OBJECT::block) + KERNEL_OBJECT::BLOCK_ALIGN - 1 +
                  KERNEL_OBJECT::THREAD_COUNT *
                      (KERNEL_OBJECT::LEDGER_CAPACITY * sizeof(KERNEL::call_ledger_entry) +
                       sizeof(KERNEL_OBJECT::block) + KERNEL_OBJECT::BLOCK_ALIGN - 1) <=
              sizeof(g_bookkeeping_storage),
              "bookkeeping arena must fit thread table and all per-thread ledgers");

template <> void KERNEL_OBJECT::init() {
  for (uintptr_t id = 0; id < OBJECT_COUNT; ++id) {
    m_objects[id].created = false;
    m_objects[id].flags = 0;
    m_objects[id].creator_object = (uint32_t)NO_OBJECT;
    m_objects[id].kernel_role = (id == KERNEL_OBJECT_ID);
    m_objects[id].region_base = 0;
    m_objects[id].region_limit = 0;
    ARCH::store_release32(&m_objects[id].generation, 0u);
    ARCH::store_release32(&m_objects[id].instance_generation, 0u);
    m_object_name[id] = nullptr;
    for (uintptr_t method = 0; method < METHOD_COUNT; ++method)
      m_objects[id].methods[method] = nullptr;
    for (auto &d : m_objects[id].method_descriptors)
      d.valid = 0;
  }
  for (uintptr_t thread = 0; thread < THREAD_COUNT; ++thread) {
    m_kill_pending[thread] = 0;
    m_thread_object[thread] = (uint16_t)ROOT_OBJECT;
    m_wake_at[thread] = 0;
    // 既定は量つき。自分から返さないスレッドがいても系が凍らないようにする
    // (量は quantum ではなく安全網 — 正常時はこれより早く返るので発火しない)。
    m_budget[thread] = DEFAULT_BUDGET_CYCLES;
  }
  for (uintptr_t core = 0; core < KERNEL::CORE_COUNT; ++core)
    m_rotor[core] = 0;
  m_table_lock = 0;
  for (uintptr_t thread = 0; thread < THREAD_COUNT; ++thread) {
    m_thread_stack[thread] = 0;
    m_thread_ledger[thread] = 0;
  }

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
#if defined(SHIZUKU_C5_BLE)
  constexpr uintptr_t OBJECT_ARENA_BYTES = 56 * 1024;
#else
  constexpr uintptr_t OBJECT_ARENA_BYTES = 128 * 1024;
#endif
  auto heap = kernel_instance.memory_manager.kernel_malloc(OBJECT_ARENA_BYTES);
  if (!heap)
    KERNEL::BOARD::panic("no room for the object arena");
  arena_init(m_objects_arena, (uintptr_t)heap.value(), OBJECT_ARENA_BYTES);
  // KERNEL_OBJECT (0) とブートアプリ ROOT_OBJECT (1) は最初から在るものとして扱う。
  m_objects[KERNEL_OBJECT_ID].created = true;
  ARCH::store_release32(&m_objects[KERNEL_OBJECT_ID].generation, 1u);
  ARCH::store_release32(&m_objects[KERNEL_OBJECT_ID].instance_generation, 1u);
  m_object_name[KERNEL_OBJECT_ID] = "kernel_object";
  // ★svc ハンドラを務めるのはこのオブジェクト = 種別は HANDLER。カーネルが
  //   「0 番だから」で決めるのではなく、**こちらが役として宣言する**。
  //   差し替えや多重化をするときも、変わるのはこの宣言だけで済む。
  m_objects[KERNEL_OBJECT_ID].kind = object_kind::HANDLER;
  m_objects[KERNEL_OBJECT_ID].parent_handler_object = (uint32_t)KERNEL_OBJECT_ID;
  m_objects[KERNEL_OBJECT_ID].parent_handler_entry = handler_entry();
  m_objects[ROOT_OBJECT].created = true;
  ARCH::store_release32(&m_objects[ROOT_OBJECT].generation, 1u);
  ARCH::store_release32(&m_objects[ROOT_OBJECT].instance_generation, 1u);
  m_object_name[ROOT_OBJECT] = "root";
  m_objects[ROOT_OBJECT].kind = object_kind::PLAIN;
  m_objects[ROOT_OBJECT].parent_handler_object = (uint32_t)KERNEL_OBJECT_ID;
  m_objects[ROOT_OBJECT].parent_handler_entry = handler_entry();
  for (uintptr_t index = 0; index < STREAM_COUNT; ++index) {
    m_streams[index] = {};
    m_streams[index].generation = 1;
    m_streams[index].producer = stream::NO_OWNER;
    m_streams[index].consumer = stream::NO_OWNER;
  }
  for (uintptr_t index = 0; index < CONNECTION_COUNT; ++index)
    m_connections[index] = {};
  m_connection_count = 0;
  m_pump_lock = 0;
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
  const uintptr_t ledger = arena_allocate(
      m_bookkeeping, LEDGER_CAPACITY * sizeof(KERNEL::call_ledger_entry), ROOT_OBJECT);
  if (!ledger)
    KERNEL::BOARD::panic("no room for boot call ledger");
  m_thread_ledger[0] = ledger;
  return {base, BOOT_STACK_BYTES, (KERNEL::call_ledger_entry *)ledger,
          LEDGER_CAPACITY};
}

// ---- 2 本目のコア -----------------------------------------------------------
// ★このコアの「最初の 1 本」は他のスレッドと同じ扱い: 枠もスタックもここが用意する。
//   走らせる中身は**アイドル役** — 自分では何もせず、走れる相手が居れば渡すだけ。
//   ★アイドルが仕事を持つと、その仕事が他の全部の遅れになる (thread 0 と同じ規律)。
namespace {
uintptr_t g_secondary_stack = 0;
uintptr_t g_secondary_bytes = 0;
uint32_t g_secondary_thread = 0;
KERNEL::call_ledger_entry *g_secondary_ledger = nullptr;

// 採用されたあとに走る本体。
void secondary_idle() {
  while (true)
    KERNEL::ARCH::syscall((uintptr_t)object_api::YIELD);
}

// core1 の入口。まだ pico-sdk が用意した足場の上に居るので、ここで自分を
// スレッドとして採用し、借りたスタックへ移る。
void secondary_boot() {
  kernel_instance.bootstrap_secondary(g_secondary_thread, secondary_idle,
                                      g_secondary_stack, g_secondary_bytes,
                                      g_secondary_ledger,
                                      KERNEL_OBJECT::LEDGER_CAPACITY);
}
} // namespace

template <> bool KERNEL_OBJECT::start_secondary_core() {
  if (KERNEL::CORE_COUNT < 2)
    return false;
  const auto reserved = kernel_instance.reserve_thread();
  if (reserved.error != kernel_error::OK)
    return false;
  uintptr_t stack;
  uintptr_t ledger;
  {
    table_guard guard;
    stack = arena_allocate(m_objects_arena, THREAD_STACK_BYTES, ROOT_OBJECT);
    ledger = arena_allocate(
        m_bookkeeping, LEDGER_CAPACITY * sizeof(KERNEL::call_ledger_entry), ROOT_OBJECT);
    if (stack != 0 && ledger != 0) {
      m_thread_stack[reserved.thread] = stack;
      m_thread_ledger[reserved.thread] = ledger;
    } else {
      if (stack)
        arena_release(m_objects_arena, stack);
      if (ledger)
        arena_release(m_bookkeeping, ledger);
      stack = ledger = 0;
    }
  }
  if (stack == 0) {
    kernel_instance.terminate(reserved.thread);
    kernel_instance.release(reserved.thread);
    return false;
  }
  g_secondary_thread = reserved.thread;
  g_secondary_ledger = (KERNEL::call_ledger_entry *)ledger;
  g_secondary_stack = stack;
  g_secondary_bytes = THREAD_STACK_BYTES;
  m_thread_object[reserved.thread] = (uint16_t)ROOT_OBJECT;
  // ★アイドルは時限を持たない。持ち分を渡す側であって、借りる側ではない。
  m_budget[reserved.thread] = DEFAULT_BUDGET_CYCLES;
  KERNEL::BOARD::launch_core(secondary_boot);
  return true;
}

template <> uintptr_t KERNEL_OBJECT::current_object(uint32_t thread) const {
  return kernel_instance.current_object(thread);
}

template <> uintptr_t KERNEL_OBJECT::caller_object(uint32_t thread) const {
  return kernel_instance.caller_object(thread);
}

// 巻き戻さずにその場で答える。**エラーを黙って捨てない**ための共通口 (D12)。
template <> void KERNEL_OBJECT::reply(object_error error, uintptr_t value) {
  const uint32_t depth = kernel_instance.current_depth();
  if (depth == 0)
    return;
  return_to_depth(depth - 1, value, (uintptr_t)error);
  // 成功すれば戻らない。
}

template <> uintptr_t KERNEL_OBJECT::return_to_depth(uint32_t target_depth,
                                                     uintptr_t value,
                                                     uintptr_t error) {
  const uint32_t depth = kernel_instance.current_depth();
  if (target_depth >= depth)
    return (uintptr_t)kernel_error::BAD_COUNT;
  const uintptr_t count = depth - target_depth;
  const auto result = ARCH::syscall((uintptr_t)primitive::RETURN, count, value,
                                    error, target_depth);
  return result.error;
}

// ---- 診断専用 (挙動不変) ------------------------------------------------------
// create_object が失敗 return する直前の最後の error と、その生成者・番号。
// sysbus から llvm-nm -C で引けるよう外部リンケージにしてある (static だと読めない)。
extern "C" {
__attribute__((used)) volatile uint32_t shizuku_kobj_last_create_error = 0;
__attribute__((used)) volatile uint32_t shizuku_kobj_last_create_creator = 0;
__attribute__((used)) volatile uint32_t shizuku_kobj_last_create_id = 0;
}
static void note_create_failure(uintptr_t id, uint32_t creator, object_error error) {
  shizuku_kobj_last_create_error = (uint32_t)error;
  shizuku_kobj_last_create_creator = creator;
  shizuku_kobj_last_create_id = (uint32_t)id;
}

template <>
uintptr_t KERNEL_OBJECT::create_object(uintptr_t id, uintptr_t entry,
                                       uintptr_t flags, object_error &error) {
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_BOOT_STAGE_TRACE) && SHIZUKU_BOOT_STAGE_TRACE > 0
  shizuku_boot_trace_phase(421);
#endif
  const uint32_t creator =
      (uint32_t)current_object(kernel_instance.current_thread_id());
  if (id >= OBJECT_COUNT || id == KERNEL_OBJECT_ID || id == ROOT_OBJECT) {
    error = object_error::BAD_OBJECT;
    note_create_failure(id, creator, error);
    return 0;
  }
  const bool is_privileged =
      (object_protection(creator) == PROTECTION_PRIVILEGED);

  // ★専用 handling object の宣言は信頼済み composition 主体 (ROOT / KERNEL_OBJECT) だけが行える (重大指摘 1)。
  //   単なる実行特権 bit (PROTECTION_PRIVILEGED) では絶対に許可しない。
  const bool is_handler = (flags & INTERNAL_FLAG_HANDLER) != 0;
  if (!svc_permission_allows((uintptr_t)object_api::CREATE_OBJECT, is_privileged,
          creator == ROOT_OBJECT || creator == KERNEL_OBJECT_ID, false, flags,
          INTERNAL_FLAG_HANDLER)) {
    error = object_error::NOT_PRIVILEGED;
    note_create_failure(id, creator, error);
    return 0;
  }

  // ★kernel 役の付与は ROOT か kernel 役の生成者だけ (HANDLER と同じ信頼の根)。
  //   一般オブジェクトが立てようとしたら、枠も表も触らずに NOT_PRIVILEGED で拒否する。
  const bool is_kernel_role = (flags & INTERNAL_FLAG_KERNEL_ROLE) != 0;
  if (is_kernel_role && creator != ROOT_OBJECT &&
      !m_objects[creator].kernel_role) {
    error = object_error::NOT_PRIVILEGED;
    note_create_failure(id, creator, error);
    return 0;
  }

  // ★既にある番号に**上書きで**生成する要求 (動的ロードの入れ替え) は、
  //   その番号を持っている誰かを黙って乗っ取る行為なので、特権オブジェクト
  //   だけに許す。名前を付けずにビットだけ立てると、衝突の検出 (下) を
  //   素通りする抜け道が無名のまま残る。
  const bool replace = (flags & OBJECT_REPLACE) != 0;
  const char *taken_by = nullptr;
  {
    table_guard guard;
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_BOOT_STAGE_TRACE) && SHIZUKU_BOOT_STAGE_TRACE > 0
    shizuku_boot_trace_phase(422);
#endif
    // 世代が尽きたら作成/差し替え/export を拒否 (fail-closed)。再利用させない
    const uint32_t current_generation =
        ARCH::load_acquire32(&m_objects[id].generation);
    const uint32_t current_instance_generation =
        ARCH::load_acquire32(&m_objects[id].instance_generation);
    if (replace && current_generation == UINT32_MAX) {
      error = object_error::BAD_OBJECT;
      note_create_failure(id, creator, error);
      return 0;
    }
    if (replace && current_instance_generation == UINT32_MAX) {
      error = object_error::BAD_OBJECT;
      note_create_failure(id, creator, error);
      return 0;
    }
    // ★差し替えても kernel 役は引き継ぐ (番号ではなく役で判定するので、差し替え後の
    //   新しい中身も同じ扱いになる)。付与の要求が無くても、旧い役は消えない。
    const bool kept_kernel_role =
        m_objects[id].created && m_objects[id].kernel_role;
    if (!m_objects[id].created || replace) {
      m_objects[id].created = true;
      // 生成者は差し替えのたびに記録し直す (旧 creator の権限を新しい中身へ残さない)。
      m_objects[id].creator_object = (uint32_t)creator;
      m_objects[id].kernel_role = kept_kernel_role || is_kernel_role;
      m_objects[id].flags =
          (uint32_t)(flags & ~(OBJECT_REPLACE | INTERNAL_FLAG_HANDLER |
                               INTERNAL_FLAG_KERNEL_ROLE));
      if (current_generation == 0 || replace) {
        ARCH::store_release32(&m_objects[id].generation,
                             current_generation + 1u);
      }
      if (current_instance_generation == 0 || replace) {
        ARCH::store_release32(&m_objects[id].instance_generation,
                             current_instance_generation + 1u);
      }
      if (replace) {
        for (auto &d : m_objects[id].method_descriptors) {
          ARCH::store_release32(&d.valid, 0u);
        }
      }
      for (uintptr_t method = 1; method < METHOD_COUNT; ++method)
        m_objects[id].methods[method] = nullptr;
      // 最初のメソッドは生成側が与える (オブジェクト自身はまだ走っていないので
      // 自分では登録できない)。以後は EXPORT_METHOD で自分が増やす。
      m_objects[id].methods[0] = (method_t)entry;
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_BOOT_STAGE_TRACE) && SHIZUKU_BOOT_STAGE_TRACE > 0
      shizuku_boot_trace_phase(423);
#endif

      if (is_handler) {
        // ★特権オブジェクトによって専用ハンドラ (HANDLER) として宣言された
        m_objects[id].kind = object_kind::HANDLER;
        // 専用ハンドラ自身の親ハンドラは root kernel object
        m_objects[id].parent_handler_object = (uint32_t)KERNEL_OBJECT_ID;
        m_objects[id].parent_handler_entry = handler_entry();
      } else {
        m_objects[id].kind = object_kind::PLAIN;
        // ★親ハンドラの自動導出 (指摘 1, 5, 7):
        // 作成者が専用ハンドラなら、その専用ハンドラが親になる。
        // 作成者が Root 等なら、root kernel object が親になる。
        if (creator != KERNEL_OBJECT_ID && creator != ROOT_OBJECT &&
            m_objects[creator].kind == object_kind::HANDLER) {
          m_objects[id].parent_handler_object = creator;
          m_objects[id].parent_handler_entry =
              (uintptr_t)m_objects[creator].methods[0];
        } else {
          m_objects[id].parent_handler_object = (uint32_t)KERNEL_OBJECT_ID;
          m_objects[id].parent_handler_entry = handler_entry();
        }
      }
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
    note_create_failure(id, creator, error);
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

// ---- 軸 B (Q8 / DESIGN §11.3) — オブジェクトへ動的に開く読み専用の窓 --------
// ★生成時の宣言を差し替える。**次の SPAWN から**効く (走っているスレッドの
//   アフィニティは変えない — 走っている文脈を別コアへ移す手段は無い)。
template <>
uintptr_t KERNEL_OBJECT::set_object_affinity(uintptr_t id, uintptr_t cores,
                                             object_error &error) {
  if (id >= OBJECT_COUNT || !m_objects[id].created) {
    error = object_error::BAD_OBJECT;
    return 0;
  }
  table_guard guard;
  m_objects[id].flags = (m_objects[id].flags & ~(uint32_t)OBJECT_AFFINITY_MASK) |
                        (uint32_t)((cores << OBJECT_AFFINITY_SHIFT) &
                                   OBJECT_AFFINITY_MASK);
  return 1;
}

template <> uint32_t KERNEL_OBJECT::object_region_base(uintptr_t id) const {
  return m_objects[id].region_base;
}
template <> uint32_t KERNEL_OBJECT::object_region_limit(uintptr_t id) const {
  return m_objects[id].region_limit;
}

// ★誰が誰に何を見せるかを決める権限そのものが特権行為。呼び出し元
//   (現在オブジェクト) が非特権なら、対象がどこであろうと拒否する
//   (対象を特権にする話ではない — 「開示できる側」の話)。
template <>
uintptr_t KERNEL_OBJECT::grant_region(uintptr_t target, uintptr_t base,
                                      uintptr_t limit, object_error &error) {
  const uint32_t thread = kernel_instance.current_thread_id();
  const uintptr_t granter = current_object(thread);
  if (!svc_permission_allows((uintptr_t)object_api::GRANT_REGION,
                              object_protection(granter) == PROTECTION_PRIVILEGED)) {
    error = object_error::NOT_PRIVILEGED;
    return 0;
  }
  if (target >= OBJECT_COUNT || !m_objects[target].created) {
    error = object_error::BAD_OBJECT;
    return 0;
  }
#if defined(SHIZUKU_RP2040)
  // Reject an unrepresentable grant before replacing the object's current
  // window. A zero limit remains the existing revoke operation.
  if (limit != 0 && !ARCH::region_range_representable(base, limit)) {
    error = object_error::BAD_MEMORY;
    return 0;
  }
#endif
  table_guard guard;
  if (!m_objects[target].created) {
    error = object_error::BAD_OBJECT;
    return 0;
  }
  const uint32_t generation = ARCH::load_acquire32(&m_objects[target].generation);
  if (generation == UINT32_MAX) {
    error = object_error::BAD_OBJECT;
    return 0;
  }
  m_objects[target].region_base = (uint32_t)base;
  m_objects[target].region_limit = (uint32_t)limit;
  ARCH::store_release32(&m_objects[target].generation, generation + 1u);
  return 1;
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
  if (self >= OBJECT_COUNT || !m_objects[self].created ||
      kernel_instance.current_object_generation() != object_generation(self)) {
    error = object_error::STALE_DESCRIPTOR;
    return 0;
  }
  // 世代が尽きたら作成/差し替え/export を拒否 (fail-closed)。再利用させない
  const uint32_t generation = ARCH::load_acquire32(&m_objects[self].generation);
  if (generation == UINT32_MAX) {
    error = object_error::BAD_OBJECT;
    return 0;
  }
  m_objects[self].methods[method] = (method_t)entry;
  ARCH::store_release32(&m_objects[self].generation, generation + 1u);
  return self;
}

template <>
uintptr_t KERNEL_OBJECT::create_method_descriptor(uintptr_t target_id,
                                                   uintptr_t method_index,
                                                   object_error &error) {
  table_guard guard;
  if (target_id >= OBJECT_COUNT || !m_objects[target_id].created) {
    error = object_error::BAD_OBJECT;
    return 0;
  }
  if (method_index >= METHOD_COUNT) {
    error = object_error::BAD_METHOD;
    return 0;
  }
  if (m_objects[target_id].methods[method_index] == nullptr) {
    error = object_error::UNDECLARED_METHOD;
    return 0;
  }
  const uint32_t target_generation =
      ARCH::load_acquire32(&m_objects[target_id].generation);
  if (target_generation == 0) {
    error = object_error::BAD_DESCRIPTOR;
    return 0;
  }
  const uint32_t thread_id = kernel_instance.current_thread_id();
  const uint32_t caller_id = (uint32_t)current_object(thread_id);
  if (caller_id >= OBJECT_COUNT || !m_objects[caller_id].created) {
    error = object_error::BAD_OBJECT;
    return 0;
  }
  const uint32_t caller_generation = kernel_instance.current_object_generation();
  if (caller_generation != object_generation(caller_id)) {
    error = object_error::STALE_DESCRIPTOR;
    return 0;
  }
  // ★作成権限 (方針)。自分自身 / 親 (対象が呼び出し元の creator) / 直接の子 (呼び出し元が
  //   対象の creator) は許可。呼び出し元が kernel 役 (kernel_role) を負うなら相手を問わず
  //   許可 (役で判定し、ID では判定しない)。兄弟・孫・無関係は NOT_PRIVILEGED。
  //   拒否はここで返すので、枠も表も副作用は無い (枠探索より前)。
  //   NOT_PRIVILEGED を使うのは、既存の「権限の無い呼び出し」の拒否と同じ語彙に揃えるため。
  const bool caller_is_kernel_role = m_objects[caller_id].kernel_role;
  const bool permitted =
      caller_is_kernel_role || target_id == caller_id ||
      m_objects[caller_id].creator_object == target_id ||
      m_objects[target_id].creator_object == caller_id;
  if (!permitted) {
    error = object_error::NOT_PRIVILEGED;
    return 0;
  }
  int free_slot = -1;
  for (int i = 0; i < (int)METHOD_DESCRIPTOR_CAPACITY; ++i) {
    auto &candidate = m_objects[caller_id].method_descriptors[i];
    bool valid = ARCH::load_acquire32(&candidate.valid) != 0;
    if (valid && (candidate.target_object >= OBJECT_COUNT ||
                  candidate.target_generation == 0 ||
                  candidate.target_generation != ARCH::load_acquire32(
                      &m_objects[candidate.target_object].generation))) {
      // Stale indices are never recycled within this owner generation: API 33
      // carries only the index, so reuse would turn an old handle into a new call.
      continue;
    }
    if (!valid) {
      free_slot = i;
      break;
    }
  }
  if (free_slot < 0) {
    error = object_error::NO_DESCRIPTOR; // 21
    return 0;
  }
  auto &desc = m_objects[caller_id].method_descriptors[free_slot];
  desc.target_object = (uint32_t)target_id;
  desc.target_entry = (uintptr_t)m_objects[target_id].methods[method_index];
  desc.target_generation = target_generation;
  desc.owner_object = caller_id;
  desc.owner_generation = caller_generation;
  desc.method_index = (uint32_t)method_index;
  desc.flags = m_objects[target_id].flags;
  desc.region_base = m_objects[target_id].region_base;
  desc.region_limit = m_objects[target_id].region_limit;
  ARCH::store_release32(&desc.valid, 1u);
  return (uintptr_t)free_slot;
}

template <> call_request KERNEL_OBJECT::callee_request(uintptr_t id, uintptr_t entry_pc) const {
  call_request request{};
  request.entry_pc = entry_pc;
  request.callee_object = (uint32_t)id;                    // ★呼び先オブジェクトID
  request.callee_kind = (uint32_t)object_kind_of(id);      // ★呼び先の種別
  request.callee_generation = object_generation(id);
  request.parent_handler_object = m_objects[id].parent_handler_object;
  request.parent_handler_entry = m_objects[id].parent_handler_entry;
  request.protection = object_protection(id);
  request.region_base = object_region_base(id);
  request.region_limit = object_region_limit(id);
  return request;
}

template <>
uintptr_t KERNEL_OBJECT::call_method(uintptr_t id, uintptr_t method,
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
    // 未 export は panic ではなくエラー (呼び出し元は待って再試行できる)。
    error = object_error::UNDECLARED_METHOD;
    return 0;
  }
  if (kernel_instance.current_call_count() >= MAX_DEPTH) {
    error = object_error::NO_STACK;
    return 0;
  }

  call_request request = callee_request(id, (uintptr_t)entry);
  request.args[0] = argument;

  // CALL 段はカーネルが push する。失敗時は台帳に段が残らない。
  const auto result =
      ARCH::syscall((uintptr_t)primitive::CALL, (uintptr_t)&request);
  // ★成功した場合、この syscall は呼び先が戻ってから返る。戻ってきた時点で
  //   台帳は exit_method 側が既に戻している。
  if (result.error != (uintptr_t)kernel_error::OK) {
    error = result.error == (uintptr_t)kernel_error::NO_STACK
                ? object_error::NO_STACK
                : object_error::BAD_OBJECT;
    return 0;
  }
  return result.value;
}

template <>
void KERNEL_OBJECT::exit_method(uintptr_t target_depth, uintptr_t value,
                                uintptr_t error) {
  if (target_depth == 0 && kernel_instance.current_call_count() == 0) {
    exit_thread();
    return;
  }
  const uintptr_t result = return_to_depth((uint32_t)target_depth, value, error);
  reply(object_error::UNWIND_REJECTED, result);
}

template <>
void KERNEL_OBJECT::forward_child_exit(uintptr_t target_depth,
                                       uintptr_t value, uintptr_t error) {
  const uint32_t thread = kernel_instance.current_thread_id();
  // The immediately preceding ledger entry identifies the API issuer's role.
  // Check provenance without matching a fixed ledger shape.
  const auto *source = kernel_instance.ledger_entry(thread, 1);
  const bool from_handler = source &&
      source->kind == (uint8_t)object_kind::HANDLER &&
      source->object != KERNEL_OBJECT_ID;
  if (!svc_permission_allows((uintptr_t)object_api::FORWARD_CHILD_EXIT, false,
                              false, from_handler)) {
    reply(object_error::NOT_PRIVILEGED, 0);
    return;
  }
  exit_method(target_depth, value, error);
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
  const uintptr_t ledger = arena_allocate(
      m_bookkeeping, LEDGER_CAPACITY * sizeof(KERNEL::call_ledger_entry), id);
  table_unlock();
  if (stack == 0 || ledger == 0) {
    table_lock();
    if (stack) arena_release(m_objects_arena, stack);
    if (ledger) arena_release(m_bookkeeping, ledger);
    table_unlock();
    error = object_error::NO_MEMORY;
    return 0;
  }
  KERNEL::spawn_request request{};
  request.entry_pc = (uintptr_t)entry;
  request.argument = argument;
  request.protection = object_protection(id);
  request.affinity = object_affinity(id);
  request.region_base = object_region_base(id);
  request.region_limit = object_region_limit(id);
  request.stack_base = stack;
  request.stack_bytes = THREAD_STACK_BYTES;
  request.object_id = (uint32_t)id; // ★所属オブジェクトIDをカーネルに渡す
  request.object_generation = object_generation(id);
  request.kind = (uint32_t)object_kind_of(id); // ★種別も同じく渡す
  request.parent_handler_object = m_objects[id].parent_handler_object;
  request.parent_handler_entry = m_objects[id].parent_handler_entry;
  request.ledger = (KERNEL::call_ledger_entry *)ledger;
  request.ledger_capacity = LEDGER_CAPACITY;
  const auto spawned = kernel_instance.spawn(request);
  if (spawned.error != kernel_error::OK) {
    table_lock();
    arena_release(m_objects_arena, stack); // 使わなかったので返す
    arena_release(m_bookkeeping, ledger);
    table_unlock();
    error = object_error::NO_THREAD;
    return 0;
  }
  {
    table_guard guard;
    m_thread_stack[spawned.thread] = stack;
    m_thread_ledger[spawned.thread] = ledger;
  }
  // 新しいスレッドは「そのオブジェクトとして」走り始める。台帳の底をそう置く。
  m_thread_object[spawned.thread] = (uint16_t)id;
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
#if defined(SHIZUKU_CRASH_RECORD)
  kobj_diag_mark(objects::boot_diag_breadcrumb::KOBJ_SCHEDULE_ENTRY);
#endif
  using state_t = KERNEL::THREAD::state_t;
  const uint32_t core = KERNEL::BOARD::core_num();
  const uint64_t now = KERNEL::BOARD::time_us();
  // ★接続を進めるのはここ。譲るたびに 1 歩進むので、専用スレッドが要らない
  //   (専用スレッドにすると、それ自体が予算を食う口になる)。
  pump_connections();
  for (uint32_t step = 1; step <= THREAD_COUNT; ++step) {
    const uint32_t candidate = (m_rotor[core] + step) % THREAD_COUNT;
    if (candidate == self)
      continue;
    // ★止めてくれと言われた相手は、**どのコアでも二度と選ばない**。見るのは
    //   自分たちの旗だけで、共有の状態語には触らない — ここが要点で、
    //   **SUSPENDED を使うとデバッガと喧嘩する** (D55):
    //     ・こちらが SUSPENDED を書く → GDB の `c` が resume() で
    //       CAS(SUSPENDED→READY) して**こちらの停止を解除する**
    //     ・こちらが SUSPENDED から回収する → **GDB が止めている相手を消す**
    //       (デバッガは g_target_thread を握ったままなので、枠が再利用された
    //        あと無関係のスレッドのレジスタを読み書きしに行く)
    //   状態語は 1 つしか無く持ち主も書いていないので、2 人が別々の意図で
    //   書いた瞬間に区別が付かなくなる。だから**共有しない**。
    // 回収は READY からの CAS でだけ行う。READY はこの系では「文脈の退避が
    //   済み、どのコアも走らせていない」と同義なので、取れた瞬間だけが足場を
    //   返してよい瞬間 (判定と確定を 1 命令に畳んで、見てから書くまでの隙を消す)。
    // ★デバッガが止めている間、kill は**完了しない**。再開して READY へ
    //   戻った時点で完了する。待たせるほうが、横から消すより安全。
    if (m_kill_pending[candidate]) {
      // ★既に TERMINATED の相手も拾う。止めるつもりでいる間に、相手が自分で
      //   走り終えたり (exit_thread) 保護違反で落ちたり (fault_dispatch) する
      //   ことがある。CAS は READY からしか取れないので、この枝が無いと
      //   「旗は立っているが二度と READY にならない」相手の記憶が永久に
      //   返らなくなる (旗を見て continue するので、下の回収にも届かない)。
      const bool mine =
          kernel_instance.terminate_if_idle(candidate) ||
          kernel_instance.thread_state(candidate) == state_t::TERMINATED;
      if (mine) {
        table_lock();
        if (m_kill_pending[candidate]) { // 別のコアが先に片付けていた
          if (m_thread_stack[candidate] != 0) {
            arena_release(m_objects_arena, m_thread_stack[candidate]);
            m_thread_stack[candidate] = 0;
          }
          if (m_thread_ledger[candidate]) {
            arena_release(m_bookkeeping, m_thread_ledger[candidate]);
            m_thread_ledger[candidate] = 0;
          }
          m_thread_object[candidate] = (uint16_t)ROOT_OBJECT;
          m_wake_at[candidate] = 0;
          m_budget[candidate] = DEFAULT_BUDGET_CYCLES;
          // ★★旗を消してから枠を返す。順を逆にすると、返した枠を他コアの
          //   spawn が (こちらの錠とは無関係に) CAS で取り、生まれたばかりの
          //   スレッドが**前の住人宛の停止要求を相続する**。
          m_kill_pending[candidate] = 0;
          kernel_instance.release(candidate);
        }
        table_unlock();
      }
      continue; // 取れても取れなくても走らせない
    }
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
      if (m_thread_ledger[candidate]) {
        arena_release(m_bookkeeping, m_thread_ledger[candidate]);
        m_thread_ledger[candidate] = 0;
      }
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
  // ★★★**締切だけでは抜けない。止められている間は抜けない** (D57)。
  //   `suspend()` が保証するのは「**次に選ばれない**」ことだけで、
  //   **既に CPU を持っている相手には効かない**。ここは
  //   `schedule()` が候補を見つけられなければ CPU を手放さずに回り続ける
  //   ループなので、止められていても締切が来れば素通りして、そのまま
  //   ユーザコードへ戻ってしまう。
  //   ★実測 (2026-08-26、XNO 13 スレッド): `monitor target` で止めた
  //     bno055 の周回カウンタが **88/8秒 → 89/8秒** と**まったく減速
  //     しなかった** (フル速度)。同じファームの blink は止まっていた —
  //     差は「止めた瞬間に CPU を手放したかどうか」だけだった。
  //     Shizuku の試験ファームは常時 READY な負荷スレッドが 3 本居るので
  //     必ず手放しており、そちらでは再現しなかった。
  //   ★ここを直すのは方針側 (kobj) の仕事。「寝るとはどういうことか」を
  //     決めているのはここで、カーネルは状態を持つだけ。**あらゆる例外復帰**
  //     に検査を足す (= 系で一番熱い経路に触る) より、譲る場所で見る方が安い。
  //   ★止められている間もユーザコードは **1 命令も進まない**。回り続ける
  //     ぶんは無駄だが、`schedule()` は毎周呼ぶので他が走れるなら譲る。
  while ((int64_t)(deadline - KERNEL::BOARD::time_us()) > 0 ||
         kernel_instance.thread_state(self) ==
             KERNEL::THREAD::state_t::SUSPENDED) {
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
uintptr_t KERNEL_OBJECT::yield(object_error &error) {
  (void)error;
  const uint32_t self = kernel_instance.current_thread_id();
  // 借り手として走っているなら、譲る先を選ぶ権利は無い — 貸し手へ返すだけ
  // (カーネルの SWITCH が対象を無視してそう振る舞う)。
  if (kernel_instance.grant_active()) {
    ARCH::syscall((uintptr_t)primitive::SWITCH, 0);
    return 0;
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
  // ★★自分のスタックも自分の枠も、ここでは返さない。**今その上で走っている**
  //   ので、返した瞬間に他コアの割り当てや spawn がその領域と枠を掴める
  //   (返してから SWITCH を撃つまでの窓で、自分は解放済みの足場を使い続ける)。
  //   回収するのは schedule() — 走っていない相手だけを見て返す (D55)。
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

// ---- 他人を止める (D55) ----------------------------------------------------
// ★ここでやるのは**自分たちの旗を 1 つ立てること**だけ。カーネルの状態語には
//   触らない。
//   GDB stub の停止 (kernel_instance.suspend) が確実に効くのは、賢いからでは
//   なく**弱いから**である — 状態語を 1 つ書くだけで何も回収しないので、
//   「止まった」の意味が「スケジューラが二度と選ばない」で足り、即座である
//   必要が無い。だが**その状態語を借りてはいけない**: SUSPENDED には持ち主が
//   書いていないので、デバッガとこちらが別々の意図で同じ語を書いた瞬間に
//   区別が付かなくなり、GDB の resume がこちらの停止を解除し、こちらの回収が
//   GDB の止めた相手を消す (どちらの向きも事故)。
//   なので停止の意思はこちら側の台帳 (m_kill_pending) に持ち、
//   ・選ばないこと    → schedule() が旗を見て飛ばす (状態語に触らない)
//   ・記憶を返すこと  → READY からの CAS が取れたときだけ (terminate_if_idle)
//   に分ける。
template <>
uintptr_t KERNEL_OBJECT::kill_thread(uintptr_t thread, object_error &error) {
  using state_t = KERNEL::THREAD::state_t;
  const uint32_t self = kernel_instance.current_thread_id();
  // 「誰が誰を止めてよいか」は方針。止める権利は特権オブジェクトだけが持つ
  //  (GRANT_REGION と同じ理由 — 権限そのものを配る行為は特権行為)。
  if (!svc_permission_allows((uintptr_t)object_api::KILL_THREAD,
                              object_protection(current_object(self)) == PROTECTION_PRIVILEGED)) {
    error = object_error::NOT_PRIVILEGED;
    return 0;
  }
  // ★各コアの「最初の 1 本」は止めない。core0 はスレッド 0、core1 は
  //   start_secondary_core が採った枠 (g_secondary_thread)。これらは
  //   **実行権の渡し先**であってアプリではないので、止めるとそのコアに
  //   走らせる相手が居なくなる。しかもアイドルは時限 0 で回るので
  //   READY にも降りてくる = 回収条件を満たしてしまう (黙って死ぬ)。
  if (thread == 0 || thread >= THREAD_COUNT ||
      thread == g_secondary_thread) {
    error = object_error::BAD_OBJECT;
    return 0;
  }
  if (thread == self) {
    exit_thread(); // 自分を止めるのは「走り終える」のと同じ経路
    return 1;
  }
  // ★デバッガ自身のスレッド (stub / agent) は止めない。止めると、止めたことを
  //   報告する経路ごと消える。デバッガが「自分は止めない」ために付けている印を
  //   そのまま使う。
  if (kernel_instance.thread_debug_protected((uint32_t)thread)) {
    error = object_error::NOT_PRIVILEGED;
    return 0;
  }
  const state_t state = kernel_instance.thread_state((uint32_t)thread);
  if (state == state_t::UNINITIALIZED || state == state_t::TERMINATED)
    return 1; // もう居ない (冪等)
  table_guard guard;
  m_kill_pending[thread] = 1;
  return 1;
}

// ---- 共有台帳の錠 ----------------------------------------------------------
// ★1 コアのときは無償だった相互排除を、2 コアでは明示的に取る (kernel_object.hpp)。
//   区間は短く、**syscall を含まない**ものだけ。握ったまま実行権を手放すと、
//   相手のコアが「走っていない持ち主」を待って回り続ける。
template <> void KERNEL_OBJECT::table_lock() {
  while (true) {
    if (ARCH::cas32(&m_table_lock, 0u, 1u)) {
#if defined(SHIZUKU_CRASH_RECORD)
      kobj_diag_mark(objects::boot_diag_breadcrumb::KOBJ_TABLE_LOCK_ACQUIRED);
#endif
      return;
    }
#if defined(SHIZUKU_CRASH_RECORD)
    kobj_diag_mark(objects::boot_diag_breadcrumb::KOBJ_TABLE_LOCK_WAIT);
#endif
    // 待つ。★ここで yield しない — 錠を待っている相手は必ず走っており
    //   (握ったまま手放さない規律があるので)、待ちは有界。yield すると
    //   「錠を待つために実行権を配る」ことになり、順序が絡んで長くなる。
  }
}

template <> void KERNEL_OBJECT::table_unlock() {
  ARCH::store_release32(&m_table_lock, 0u);
#if defined(SHIZUKU_CRASH_RECORD)
  kobj_diag_mark(objects::boot_diag_breadcrumb::KOBJ_TABLE_UNLOCK);
#endif
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

// ---- Kernel-owned stream table and data plane -------------------------------
template <> KERNEL_OBJECT::stream_entry *KERNEL_OBJECT::stream_lookup(uintptr_t id) {
  if (id > UINT32_MAX) return nullptr;
  const uint32_t slot = (uint32_t)(id & STREAM_SLOT_MASK);
  const uint32_t generation = (uint32_t)(id >> STREAM_SLOT_BITS);
  if (slot >= STREAM_COUNT || generation == 0)
    return nullptr;
  stream_entry &entry = m_streams[slot];
#if defined(SHIZUKU_RP2040)
  // Pico SDK lowers ARMv6-M __atomic_load_4 to PICO_SPINLOCK_ID_ATOMIC.
  // This lookup runs inside the SVC handler; use the aligned lock-free acquire
  // load supplied by the RP2040 arch instead of nesting another global lock.
  const uint32_t active = ARCH::load_acquire32(&entry.active);
#else
  const uint32_t active = __atomic_load_n(&entry.active, __ATOMIC_ACQUIRE);
#endif
  return active == 1u &&
                 entry.generation == generation ? &entry : nullptr;
}

template <> bool KERNEL_OBJECT::stream_pin(uintptr_t id, stream_entry *&entry) {
  entry = stream_lookup(id);
  if (!entry) return false;
  uint32_t users;
  do {
    users = ARCH::load_acquire32(&entry->users);
    if (users & 0x80000000u || users == 0x7FFFFFFFu) {
      entry = nullptr;
      return false;
    }
  } while (!ARCH::cas32(&entry->users, users, users + 1));
#if defined(SHIZUKU_RP2040)
  const uint32_t active = ARCH::load_acquire32(&entry->active);
#else
  const uint32_t active = __atomic_load_n(&entry->active, __ATOMIC_ACQUIRE);
#endif
  if (active != 1u ||
      entry->generation != (uint32_t)(id >> STREAM_SLOT_BITS)) {
    stream_unpin(*entry);
    entry = nullptr;
    return false;
  }
  return true;
}

template <> void KERNEL_OBJECT::stream_unpin(stream_entry &entry) {
  uint32_t users;
  do {
    users = ARCH::load_acquire32(&entry.users);
  } while ((users & 0x7FFFFFFFu) != 0 &&
           !ARCH::cas32(&entry.users, users, users - 1));
}

template <> bool KERNEL_OBJECT::stream_stack_range(uint32_t thread,
                                                   uintptr_t address,
                                                   uintptr_t bytes) const {
  if (thread >= THREAD_COUNT || m_thread_stack[thread] == 0 || bytes == 0 ||
      address > UINTPTR_MAX - bytes)
    return false;
  const uintptr_t base = m_thread_stack[thread];
  const uintptr_t size = thread == 0 ? 8192u : THREAD_STACK_BYTES;
  return base <= UINTPTR_MAX - size && address >= base &&
         address + bytes <= base + size;
}

template <>
uintptr_t KERNEL_OBJECT::stream_create(uintptr_t rec_size, uintptr_t capacity,
                                       uintptr_t flags, object_error &error) {
  const uintptr_t owner = current_object(kernel_instance.current_thread_id());
  if (rec_size == 0 || capacity == 0 || rec_size > UINT32_MAX ||
      capacity > UINT32_MAX || (flags & ~stream::LOSSLESS) != 0 ||
      rec_size > UINTPTR_MAX / capacity || rec_size * capacity > UINT32_MAX) {
    error = object_error::BAD_STREAM;
    return 0;
  }
  table_guard guard;
  uintptr_t slot = STREAM_COUNT;
  for (uintptr_t i = 0; i < STREAM_COUNT; ++i)
    if (!m_streams[i].active && !m_streams[i].retired) { slot = i; break; }
  if (slot == STREAM_COUNT) { error = object_error::NO_STREAM; return 0; }
  const uintptr_t allocation = arena_allocate(
      m_objects_arena, rec_size * capacity, owner);
  if (!allocation) { error = object_error::NO_MEMORY; return 0; }
  stream_entry &entry = m_streams[slot];
  entry.base = (uint8_t *)allocation;
  entry.allocation = allocation;
  entry.owner = owner;
  entry.rec_size = (uint32_t)rec_size;
  entry.capacity = (uint32_t)capacity;
  entry.flags = (uint32_t)flags;
  entry.wr = 0;
  entry.rd = 0;
  entry.producer = entry.consumer = stream::NO_OWNER;
  entry.users = 0;
  entry.active = true;
  return ((uintptr_t)entry.generation << STREAM_SLOT_BITS) | slot;
}

template <>
uintptr_t KERNEL_OBJECT::stream_open(uintptr_t id, object_error &error) {
  table_guard guard;
  if (!stream_lookup(id)) { error = object_error::BAD_STREAM; return 0; }
  return id;
}

template <>
uintptr_t KERNEL_OBJECT::stream_bind(uintptr_t id, uintptr_t which,
                                     object_error &error) {
  if (which > (uintptr_t)stream::role::CONSUMER) {
    error = object_error::BAD_STREAM; return 0;
  }
  const uintptr_t self = current_object(kernel_instance.current_thread_id());
  table_guard guard;
  stream_entry *target = stream_lookup(id);
  if (!target) { error = object_error::BAD_STREAM; return 0; }
  uint32_t &seat = which == (uintptr_t)stream::role::PRODUCER
                       ? target->producer : target->consumer;
  if (seat != stream::NO_OWNER && seat != (uint32_t)self) {
    error = object_error::SEAT_TAKEN; return 0;
  }
  seat = (uint32_t)self;
  return self;
}

template <>
uintptr_t KERNEL_OBJECT::stream_destroy(uintptr_t id, object_error &error) {
  const uintptr_t self = current_object(kernel_instance.current_thread_id());
  stream_entry *entry = nullptr;
  uintptr_t allocation = 0;
  {
    table_guard guard;
    entry = stream_lookup(id);
    if (!entry) { error = object_error::BAD_STREAM; return 0; }
    if (entry->owner != self) { error = object_error::NOT_OWNER; return 0; }
    for (uintptr_t i = 0; i < CONNECTION_COUNT; ++i) {
      const uint32_t state = ARCH::load_acquire32(&m_connections[i].active);
      if (state != CONNECTION_FREE &&
          (m_connections[i].src == id || m_connections[i].dst == id)) {
        error = object_error::STREAM_BUSY; return 0;
      }
    }
    uint32_t no_users = 0;
    if (!ARCH::cas32(&entry->users, no_users, 0x80000000u)) {
      error = object_error::STREAM_BUSY;
      return 0;
    }
    allocation = entry->allocation;
    __atomic_store_n(&entry->active, 2u, __ATOMIC_RELEASE); // reserve slot while freeing
  }
  const uint32_t slot = (uint32_t)(id & STREAM_SLOT_MASK);
  table_guard guard;
  if (!arena_release(m_objects_arena, allocation)) {
    ARCH::store_release32(&entry->users, 0u);
    __atomic_store_n(&entry->active, 1u, __ATOMIC_RELEASE);
    error = object_error::BAD_MEMORY; return 0;
  }
  entry->base = nullptr;
  entry->allocation = entry->owner = 0;
  entry->rec_size = entry->capacity = entry->flags = 0;
  entry->wr = 0;
  entry->rd = 0;
  entry->producer = entry->consumer = stream::NO_OWNER;
  if (entry->generation == ((uintptr_t)-1 >> STREAM_SLOT_BITS))
    entry->retired = true;
  else
    ++entry->generation;
  ARCH::store_release32(&entry->users, 0u);
  __atomic_store_n(&entry->active, 0u, __ATOMIC_RELEASE);
  (void)slot;
  return 1;
}

template <>
uintptr_t KERNEL_OBJECT::stream_push_n(uintptr_t id, uintptr_t request_ptr,
                                       object_error &error) {
  const uint32_t thread = kernel_instance.current_thread_id();
  if (!stream_stack_range(thread, request_ptr, sizeof(stream::push_request))) {
    error = object_error::BAD_STREAM; return 0;
  }
  const stream::push_request request = *(const stream::push_request *)request_ptr;
  stream_entry *entry = nullptr;
  if (!stream_pin(id, entry)) { error = object_error::BAD_STREAM; return 0; }
  const uint32_t limit = request.count > entry->capacity ? entry->capacity : request.count;
  const uintptr_t bytes = (uintptr_t)limit * entry->rec_size;
  if ((limit && !request.records) ||
      (bytes && !stream_stack_range(thread, (uintptr_t)request.records, bytes))) {
    stream_unpin(*entry); error = object_error::BAD_STREAM; return 0;
  }
  const uintptr_t self = current_object(kernel_instance.current_thread_id());
  if (entry->producer != (uint32_t)self) { stream_unpin(*entry); error = object_error::SEAT_TAKEN; return 0; }
  uint32_t pushed = 0;
  while (pushed < limit) {
    const uint32_t wr = entry->wr;
    const uint32_t rd = ARCH::load_acquire32(&entry->rd);
    const uint32_t used = wr - rd;
    if ((entry->flags & stream::LOSSLESS) && used >= entry->capacity) break;
    const uint32_t index = wr % entry->capacity;
    shizuku_memcpy(entry->base + (uintptr_t)index * entry->rec_size,
                   (const uint8_t *)request.records +
                       (uintptr_t)pushed * entry->rec_size,
                   entry->rec_size);
    ARCH::store_release32(&entry->wr, wr + 1);
    ++pushed;
  }
  stream_unpin(*entry);
  return pushed;
}

template <>
uintptr_t KERNEL_OBJECT::stream_pop_n(uintptr_t id, uintptr_t request_ptr,
                                      object_error &error) {
  const uint32_t thread = kernel_instance.current_thread_id();
  if (!stream_stack_range(thread, request_ptr, sizeof(stream::pop_request))) {
    error = object_error::BAD_STREAM; return 0;
  }
  stream::pop_request request_copy = *(const stream::pop_request *)request_ptr;
  stream::pop_request *request = &request_copy;
  request->lost = 0;
  stream_entry *entry = nullptr;
  if (!stream_pin(id, entry)) { error = object_error::BAD_STREAM; return 0; }
  const uint32_t limit = request->count > entry->capacity ? entry->capacity : request->count;
  const uintptr_t bytes = (uintptr_t)limit * entry->rec_size;
  if ((limit && !request->records) ||
      (bytes && !stream_stack_range(thread, (uintptr_t)request->records, bytes))) {
    stream_unpin(*entry); error = object_error::BAD_STREAM; return 0;
  }
  const uintptr_t self = current_object(kernel_instance.current_thread_id());
  if (entry->consumer != (uint32_t)self) { stream_unpin(*entry); error = object_error::SEAT_TAKEN; return 0; }
  uint32_t popped = 0;
  while (popped < limit) {
    const uint32_t wr = ARCH::load_acquire32(&entry->wr);
    uint32_t rd = entry->rd;
    if (rd == wr) break;
    if ((uint32_t)(wr - rd) > entry->capacity) {
      const uint32_t fresh = wr - entry->capacity + stream::RESYNC_MARGIN;
      request->lost += fresh - rd;
      rd = fresh;
    }
    shizuku_memcpy((uint8_t *)request->records +
                       (uintptr_t)popped * entry->rec_size,
                   entry->base + (uintptr_t)(rd % entry->capacity) * entry->rec_size,
                   entry->rec_size);
    const uint32_t after = ARCH::load_acquire32(&entry->wr);
    if ((uint32_t)(after - rd) > entry->capacity) {
      const uint32_t fresh = after - entry->capacity + stream::RESYNC_MARGIN;
      request->lost += fresh - rd;
      ARCH::store_release32(&entry->rd, fresh);
      break;
    }
    ARCH::store_release32(&entry->rd, rd + 1);
    ++popped;
  }
  ((stream::pop_request *)request_ptr)->lost = request->lost;
  stream_unpin(*entry);
  return popped;
}

template <>
uintptr_t KERNEL_OBJECT::stream_available(uintptr_t id, object_error &error) {
  stream_entry *entry = nullptr;
  if (!stream_pin(id, entry)) { error = object_error::BAD_STREAM; return 0; }
  const uint32_t wr = ARCH::load_acquire32(&entry->wr);
  const uint32_t used = wr - entry->rd;
  const uintptr_t available = used > entry->capacity ? entry->capacity : used;
  stream_unpin(*entry);
  return available;
}

template <>
uintptr_t KERNEL_OBJECT::stream_push_kernel(uintptr_t id, const void *records,
                                            uint32_t count) {
  stream_entry *entry = nullptr;
  if (!stream_pin(id, entry) || entry->producer != stream::KERNEL_OWNER || !records) {
    if (entry) stream_unpin(*entry);
    return 0;
  }
  uint32_t pushed = 0;
  if (count > entry->capacity) count = entry->capacity;
  while (pushed < count) {
    const uint32_t wr = entry->wr, rd = ARCH::load_acquire32(&entry->rd);
    if ((entry->flags & stream::LOSSLESS) && wr - rd >= entry->capacity) break;
    shizuku_memcpy(entry->base + (uintptr_t)(wr % entry->capacity) * entry->rec_size,
                   (const uint8_t *)records + (uintptr_t)pushed * entry->rec_size,
                   entry->rec_size);
    ARCH::store_release32(&entry->wr, wr + 1);
    ++pushed;
  }
  stream_unpin(*entry);
  return pushed;
}

template <> bool KERNEL_OBJECT::stream_bind_kernel(uintptr_t id) {
  table_guard guard;
  stream_entry *entry = stream_lookup(id);
  if (!entry || entry->producer != stream::NO_OWNER) return false;
  entry->producer = stream::KERNEL_OWNER;
  return true;
}

// ★src の consumer 席と dst の producer 席を DMA で直結する。
//   以後 src へ流れたものは、**途中のオブジェクトが pop して push する段を通らずに**
//   dst へ届く。中継が要らなくなるのが眼目。
template <>
uintptr_t KERNEL_OBJECT::stream_connect(uintptr_t src, uintptr_t dst,
                                        object_error &error) {
  table_guard guard;
  stream_entry *from = stream_lookup(src);
  stream_entry *to = stream_lookup(dst);
  if (!from || !to) { error = object_error::BAD_STREAM; return 0; }
  // ★レコードの大きさが違うものは繋げない。詰め替えは「変換」であって「接続」では
  //   ないので、要るなら中継オブジェクトを書くべき (黙って詰め替えない)。
  if (from->rec_size != to->rec_size) {
    error = object_error::BAD_STREAM;
    return 0;
  }
  if (from->consumer != stream::NO_OWNER || to->producer != stream::NO_OWNER) {
    error = object_error::SEAT_TAKEN;
    return 0;
  }
  uintptr_t slot = CONNECTION_COUNT;
  for (uintptr_t index = 0; index < CONNECTION_COUNT; ++index)
    if (m_connections[index].active == 0) {
      slot = index;
      break;
    }
  if (slot == CONNECTION_COUNT) {
    error = object_error::NO_STREAM;
    return 0;
  }
  const int channel = KERNEL::BOARD::dma_claim();
  if (channel < 0) {
    error = object_error::NO_STREAM;
    return 0;
  }
  // ★席を接続が占める。以後オブジェクトは bind できない — 接続と手押しの
  //   二重供給を機構で防ぐ。
  from->consumer = stream::CONNECTED;
  to->producer = stream::CONNECTED;
  connection &link = m_connections[slot];
  link.src = (uint32_t)src;
  link.dst = (uint32_t)dst;
  link.channel = channel;
  link.inflight = 0;
  link.moved = 0;
  ARCH::store_release32(&link.active, CONNECTION_ACTIVE);
  uint32_t connection_count;
  do {
    connection_count = ARCH::load_acquire32(&m_connection_count);
  } while (!ARCH::cas32(&m_connection_count, connection_count,
                         connection_count + 1));
  return slot;
}

// 接続を畳む。★**ここでは待たない。** 転送中なら「畳む」印を立てて戻り、
//   ポンプが在庫を運び終えてから席と DMA を返す。カーネルの中で DMA の完了を
//   待つと、その待ちがカーネルに入り込む (誰も進められない時間ができる)。
// ★席を返すのは実際に畳んだ瞬間だけ。印を立てた時点で返すと、まだ DMA が
//   触っている環へ別の持ち主が bind できてしまう。
template <>
uintptr_t KERNEL_OBJECT::stream_disconnect(uintptr_t slot, object_error &error) {
  table_guard guard;
  if (slot >= CONNECTION_COUNT) {
    error = object_error::BAD_STREAM;
    return 0;
  }
  connection &link = m_connections[slot];
  const uint32_t state = ARCH::load_acquire32(&link.active);
  if (state == CONNECTION_FREE) {
    error = object_error::BAD_STREAM;
    return 0;
  }
  // 既に closing なら黙って成功にする (二度呼ばれても壊れない)。
  if (state == CONNECTION_ACTIVE)
    ARCH::store_release32(&link.active, CONNECTION_CLOSING);
  return 0;
}

// 接続を 1 歩進める。★schedule() から呼ばれるので、接続が 0 本なら数語読んで即戻る。
//   両コアが同時に回さないよう、取れなければ**待たずに諦める** (次の周回で回る)。
template <> void KERNEL_OBJECT::pump_connections() {
  if (ARCH::load_acquire32(&m_connection_count) == 0)
    return;
  if (!ARCH::cas32(&m_pump_lock, 0u, 1u)) {
#if defined(SHIZUKU_CRASH_RECORD)
    kobj_diag_mark(objects::boot_diag_breadcrumb::KOBJ_PUMP_LOCK_BUSY);
#endif
    return;
  }
#if defined(SHIZUKU_CRASH_RECORD)
  kobj_diag_mark(objects::boot_diag_breadcrumb::KOBJ_PUMP_ENTRY);
#endif
  for (uintptr_t index = 0; index < CONNECTION_COUNT; ++index) {
    connection &link = m_connections[index];
    const uint32_t state = ARCH::load_acquire32(&link.active);
    if (state == CONNECTION_FREE)
      continue;
    stream_entry *from = stream_lookup(link.src);
    stream_entry *to = stream_lookup(link.dst);
    if (!from || !to) {
      if (link.channel >= 0 && KERNEL::BOARD::dma_busy((int)link.channel))
        continue;
      // Fail closed if table corruption is ever observed.
      if (link.channel >= 0) KERNEL::BOARD::dma_release((int)link.channel);
      link.channel = -1;
      link.inflight = 0;
      ARCH::store_release32(&link.active, CONNECTION_FREE);
      uint32_t connection_count;
      do {
        connection_count = ARCH::load_acquire32(&m_connection_count);
      } while (connection_count != 0 &&
               !ARCH::cas32(&m_connection_count, connection_count,
                            connection_count - 1));
      continue;
    }
    if (link.inflight != 0) {
      if (KERNEL::BOARD::dma_busy((int)link.channel))
        continue; // まだ運んでいる
      // ★中身が届いてから番号を進める。逆にすると、揃う前の場所を読ませてしまう
      //   (push と同じ規律を、ポンプが producer として守る)。
      from->rd = link.src_rd + link.inflight;
      const uint32_t source_wr = ARCH::load_acquire32(&from->wr);
      if ((uint32_t)(source_wr - link.src_rd) > from->capacity) {
        const uint32_t fresh = source_wr - from->capacity + stream::RESYNC_MARGIN;
        ARCH::store_release32(&from->rd, fresh);
        link.inflight = 0;
        continue; // DMA bytes were overwritten while in flight; do not publish dst->wr
      }
      ARCH::store_release32(&to->wr, link.dst_wr + link.inflight);
      link.moved += link.inflight;
      link.inflight = 0;
    }
    // ★畳むのは**在庫を運び終えてから**。ここに来た時点で inflight == 0 が
    //   保証されている (上で待っている) ので、DMA はもうこの環を触っていない。
    //   残りを運ばずに捨てるのは、畳めと言われた以上そのほうが正しい
    //   (運び切るまで畳めないと、詰まった接続が永久に外せなくなる)。
    if (state == CONNECTION_CLOSING) {
      KERNEL::BOARD::dma_release((int)link.channel);
      from->consumer = stream::NO_OWNER;
      to->producer = stream::NO_OWNER;
      link.channel = -1;
      ARCH::store_release32(&link.active, CONNECTION_FREE);
      uint32_t connection_count;
      do {
        connection_count = ARCH::load_acquire32(&m_connection_count);
      } while (connection_count != 0 &&
               !ARCH::cas32(&m_connection_count, connection_count,
                            connection_count - 1));
      continue;
    }
    uint32_t src_rd = from->rd;
    const uint32_t src_wr = ARCH::load_acquire32(&from->wr);
    const uint32_t available = src_wr - src_rd;
    const uint32_t dst_wr = to->wr;
    const uint32_t dst_rd = ARCH::load_acquire32(&to->rd);
    const stream::dma_window window = stream::bounded_dma_window(
        from->capacity, to->capacity, available, src_rd, dst_wr, dst_rd);
    uint32_t count = window.count;
    src_rd = window.source_rd;
    if (count == 0)
      continue; // 受け側が満杯。**押し戻す** (溢れは src 側に溜まる)
    // bounded_dma_window has already clipped to both contiguous ring runs.
    const uint32_t rec = from->rec_size;
    const uint8_t *source =
        from->base + (uintptr_t)(src_rd % from->capacity) * rec;
    uint8_t *target = to->base + (uintptr_t)(dst_wr % to->capacity) * rec;
    link.src_rd = src_rd;
    link.dst_wr = dst_wr;
    link.inflight = count;
    KERNEL::BOARD::dma_copy((int)link.channel, source, target, count * rec);
  }
  ARCH::store_release32(&m_pump_lock, 0u);
#if defined(SHIZUKU_CRASH_RECORD)
  kobj_diag_mark(objects::boot_diag_breadcrumb::KOBJ_PUMP_EXIT);
#endif
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
  for (uintptr_t i = 0; i < STREAM_COUNT; ++i) {
    if (m_streams[i].active != 0 && m_streams[i].allocation == handle) {
      error = object_error::STREAM_BUSY;
      return 0;
    }
  }
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
  for (uintptr_t i = 0; i < STREAM_COUNT; ++i) {
    if (m_streams[i].active != 0 && m_streams[i].allocation == handle) {
      error = object_error::STREAM_BUSY;
      return 0;
    }
  }
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
                                uintptr_t a3, uintptr_t unwind_floor) {
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_BOOT_STAGE_TRACE) && SHIZUKU_BOOT_STAGE_TRACE > 0
  shizuku_boot_trace_phase(420);
#endif
  object_error error = object_error::OK;
  uintptr_t value = 0;
  (void)unwind_floor; // Scope is also kernel-authored in the active ledger entry.
  switch ((object_api)number) {
  case object_api::CREATE_OBJECT:
    value = create_object(a1, a2, a3, error);
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_BOOT_STAGE_TRACE) && SHIZUKU_BOOT_STAGE_TRACE > 0
    shizuku_boot_trace_phase(424);
#endif
    break;
  case object_api::CREATE_METHOD_DESCRIPTOR:
    value = create_method_descriptor(a1, a2, error);
    break;
  case object_api::EXPORT_METHOD:
    value = export_method(a1, a2, error);
    break;
  case object_api::CALL_METHOD:
    value = call_method(a1, a2, a3, error);
    break;
  case object_api::EXIT_METHOD:
    // a1 = absolute return depth, a2 = value, a3 = error.
    exit_method(a1, a2, a3);
    return 0;
  case object_api::FORWARD_CHILD_EXIT:
    // a1 = absolute return depth, a2 = value, a3 = error.
    forward_child_exit(a1, a2, a3);
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
    value = yield(error);
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
    value = stream_create(a1, a2, a3, error);
    break;
  case object_api::STREAM_OPEN:
    value = stream_open(a1, error);
    break;
  case object_api::STREAM_BIND:
    value = stream_bind(a1, a2, error);
    break;
  case object_api::STREAM_CONNECT:
    value = stream_connect(a1, a2, error);
    break;
  case object_api::STREAM_DISCONNECT:
    value = stream_disconnect(a1, error);
    break;
  case object_api::STREAM_DESTROY:
    value = stream_destroy(a1, error);
    break;
  case object_api::STREAM_PUSH_N:
    value = stream_push_n(a1, a2, error);
    break;
  case object_api::STREAM_POP_N:
    value = stream_pop_n(a1, a2, error);
    break;
  case object_api::STREAM_AVAILABLE:
    value = stream_available(a1, error);
    break;
  case object_api::GRANT_REGION:
    value = grant_region(a1, a2, a3, error);
    break;
  case object_api::SET_OBJECT_AFFINITY:
    value = set_object_affinity(a1, a2, error);
    break;
  case object_api::KILL_THREAD:
    value = kill_thread(a1, error);
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
