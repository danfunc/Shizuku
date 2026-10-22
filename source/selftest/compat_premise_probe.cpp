// ===========================================================================
//  FreeRTOS 互換 段1 前提 (a)(b) の実測 probe (テスト専用)
// ===========================================================================
//  (a) 互換 (HANDLER) が svc を受けている最中に CREATE_OBJECT を撃って作った
//      「孫」の親ハンドラは誰か。孫の raw svc はどこへ届くか。
//  (b) HANDLER 枠の中で YIELD して貸し手へ戻り、再 GRANT でその枠から続きを
//      走れるか。
//  ★既存 selftest の passed/failed とは混ぜない。専用カウンタ g_cp_pass / g_cp_fail
//    と extern "C" の結果表 shizuku_compat_probe を持つ。
#include "shizuku/kernel.hpp"
#include "shizuku/kernel_object.hpp"
#include "shizuku/object_ids.hpp"
#include "shizuku/object_api.hpp"
#include "shizuku/method_entry.hpp"
#include "shizuku/selftest.hpp"

namespace shizuku {
namespace selftest {

extern "C" {
volatile uint32_t shizuku_compat_probe_state[16];
}

// Read-only access to the suspended thread's private ledger for shape
// assertions. This does not add an object API or a C ABI entry point.
struct compat_probe_access {
  static uint32_t base_object(uint32_t thread) {
    return kernel_object_instance.m_thread_object[thread];
  }
};

namespace {

using ARCH = KERNEL::ARCH;
using BOARD = KERNEL::BOARD;

constexpr uintptr_t OBJ_H = object_id::compat_h;   // 互換役の HANDLER
constexpr uintptr_t OBJ_C = object_id::compat_c;   // H の子
constexpr uintptr_t OBJ_GC = object_id::compat_gc; // 「H が子の要求で作った」孫

constexpr uintptr_t CMD_SETUP = 0x01;
constexpr uintptr_t CMD_RECREATE = 0x02;
constexpr uintptr_t CMD_MKGC = 0x50; // 子の svc: H に孫を作らせる
constexpr uintptr_t CMD_WHO = 0x51;  // 子の svc: 受け手が H なら 0x4848
constexpr uintptr_t CMD_YIELD = 0x60; // 子の svc: H の枠の中で YIELD する
constexpr uintptr_t CMD_EXIT = 0x61;  // 子の svc: H に自分の thread を終了させる
constexpr uintptr_t WHO_H = 0x4848;
constexpr uintptr_t YIELD_DONE = 0x6060;

uint32_t g_cp_pass = 0;
uint32_t g_cp_fail = 0;

volatile uint32_t g_h_phase = 0;       // H が YIELD の前後で刻む (1: 前, 2: 後)
volatile uint32_t g_h_unwind_floor = UINT32_MAX;
volatile uint32_t g_c_after_svc = 0;   // 子が svc から戻ったあとに刻む
volatile uintptr_t g_c_svc_value = 0;
volatile uintptr_t g_gc_who_error = 0xFFFFFFF0u;
volatile uintptr_t g_gc_who_value = 0xFFFFFFF0u;
volatile uintptr_t g_mkgc_value = 0xFFFFFFF0u;

void check(const char *name, bool ok, unsigned long got, unsigned long want) {
  if (ok) {
    ++g_cp_pass;
    BOARD::diag_printf("[COMPAT-PROBE] PASS %s (=%lu)\n", name, got);
  } else {
    ++g_cp_fail;
    BOARD::diag_printf("[COMPAT-PROBE] FAIL %s: got %lu want %lu\n", name, got,
                       want);
  }
}

struct api_result {
  uintptr_t error;
  uintptr_t value;
};

api_result api(object_api number, uintptr_t a1 = 0, uintptr_t a2 = 0,
               uintptr_t a3 = 0) {
  const auto result = ARCH::syscall((uintptr_t)number, a1, a2, a3);
  if (result.error & KERNEL_ERROR_MARK)
    return {(uintptr_t)object_error::KERNEL_REFUSED, result.error};
  return {result.error, result.value};
}

// 孫: 自分の raw svc が誰に届くかを申告する。
uintptr_t gc_main(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  const auto result = ARCH::syscall(CMD_WHO, 0, 0, 0);
  g_gc_who_error = result.error;
  g_gc_who_value = result.value;
  return result.value;
}

uintptr_t child_main(uintptr_t op, uintptr_t, uintptr_t, uintptr_t) {
  if (op == 1) { // 孫を作らせる
    const auto result = ARCH::syscall(CMD_MKGC, 0, 0, 0);
    g_mkgc_value = result.value;
    // (a) ではサービス応答を観測した後、通常の関数return契約を混ぜずに
    // base taskを終了する。
    ARCH::syscall(CMD_EXIT, 0, 0, 0);
    return result.value;
  }
  if (op == 2) { // H の枠の中で YIELD させる
    const auto result = ARCH::syscall(CMD_YIELD, 0, 0, 0);
    g_c_svc_value = result.value;
    g_c_after_svc = 1;
    // Do not return from the SPAWN base object: its ordinary method-exit path
    // is a separate contract from this handler-response probe.
    ARCH::syscall(CMD_EXIT, 0, 0, 0);
    return result.value;
  }
  return 0;
}

// 互換役の HANDLER。handler_binding.cpp の handler_a_entry と同じ約束事。
uintptr_t handler_entry(uintptr_t a0, uintptr_t a1, uintptr_t a2,
                        uintptr_t a3, uintptr_t unwind_floor) {
  g_h_unwind_floor = (uint32_t)unwind_floor;
  if (a0 == CMD_SETUP) { // H 自身が最初の呼出しになる文脈で子を作る
    const auto res = api(object_api::CREATE_OBJECT, OBJ_C,
                         method_entry<ARCH, &child_main>(), 0);
    return res.error == 0 ? res.value : 0;
  }
  if (a0 == CMD_RECREATE) { // テストで一時reparentしたCをHの子へ戻す
    const auto res = api(object_api::CREATE_OBJECT, OBJ_C,
                         method_entry<ARCH, &child_main>(), OBJECT_REPLACE);
    return res.error == 0 ? res.value : 0;
  }
  if (a0 == CMD_MKGC) { // 子の svc を H が受けている最中に孫を作る
    const auto res = api(object_api::CREATE_OBJECT, OBJ_GC,
                         method_entry<ARCH, &gc_main>(), 0);
    return res.error == 0 ? res.value : (0xE000u | (res.error & 0xFFu));
  }
  if (a0 == CMD_WHO)
    return WHO_H;
  if (a0 == CMD_YIELD) {
    g_h_phase = 1;
    api(object_api::YIELD); // HANDLER 枠のまま貸し手へ戻る
    g_h_phase = 2;
    shizuku_compat_probe_state[14] = g_h_phase; // re-GRANT後の継続を即時記録
    return YIELD_DONE;
  }
  if (a0 == CMD_EXIT) {
    // EXIT_THREAD terminates the granted base thread and returns its grant to
    // the lender without exercising the separate ordinary-return path.
    api(object_api::EXIT_THREAD);
    return 0;
  }
  if (a0 == (uintptr_t)primitive::RETURN) {
    if (a1 == unwind_floor) { // return stub 通知。自分の scope floor へ戻す
      api(object_api::FORWARD_CHILD_EXIT, unwind_floor, a2, a3);
      return 0;
    }
    return 0xFFFF;
  }
  return 0xEEEE;
}

} // namespace

void compat_premise_probe() {
  g_cp_pass = 0;
  g_cp_fail = 0;
  BOARD::diag_printf("[COMPAT-PROBE] === start ===\n");

  // ---- 準備: H (HANDLER) と、H が root 文脈で作る子 C ------------------
  const auto res_h = api(object_api::CREATE_OBJECT, OBJ_H,
                         method_entry<ARCH, &handler_entry>(),
                         KERNEL_OBJECT::INTERNAL_FLAG_HANDLER | OBJECT_REPLACE);
  check("setup: create H with INTERNAL_FLAG_HANDLER", res_h.error == 0,
        res_h.error, 0);
  const auto setup_c = api(object_api::CALL_METHOD, OBJ_H, 0, CMD_SETUP);
  check("setup: H created C", setup_c.value == OBJ_C, setup_c.value, OBJ_C);
  check("setup: C parent handler is H",
        kernel_object_instance.object_parent_handler(OBJ_C) == OBJ_H,
        kernel_object_instance.object_parent_handler(OBJ_C), OBJ_H);

  // ---- (a) 孫 ----------------------------------------------------------
  // H の子 C をSPAWN base PLAIN taskとして動かし、raw svc応答と
  // grandchild parentを観測する。通常のCALL_METHOD returnはこの前提probeに
  // 混ぜず、Cは応答記録後にEXIT_THREADで終える。
  g_mkgc_value = 0xFFFFFFF0u;
  const uint32_t lend = 2000u * BOARD::cycles_per_us();
  const auto a_spawn = api(object_api::SPAWN, OBJ_C, 0, 1);
  check("(a) spawn C base PLAIN task", a_spawn.error == 0, a_spawn.error, 0);
  if (a_spawn.error == 0) {
    const auto a_run = api(object_api::RUN_FOR, a_spawn.value, lend);
    shizuku_compat_probe_state[0] = (uint32_t)a_run.error;
    shizuku_compat_probe_state[1] = (uint32_t)a_run.value;
    shizuku_compat_probe_state[2] = (uint32_t)g_mkgc_value;
    check("(a) spawned C exits after receiving H service reply",
          a_run.error == 0 && a_run.value == 1, a_run.value, 1);
    check("(a) H's CREATE_OBJECT response reached C",
          g_mkgc_value == OBJ_GC, g_mkgc_value, OBJ_GC);
  }
  const uint32_t gc_parent = kernel_object_instance.object_parent_handler(OBJ_GC);
  shizuku_compat_probe_state[3] = gc_parent;
  BOARD::diag_printf(
      "[COMPAT-PROBE] (a) spawned mkgc value=%lu gc_parent=%lu (H=%lu root_kobj=%lu)\n",
      (unsigned long)g_mkgc_value,
      (unsigned long)gc_parent, (unsigned long)OBJ_H,
      (unsigned long)KERNEL_OBJECT::KERNEL_OBJECT_ID);
  check("(a) grandchild parent handler is H (not root kobj)",
        gc_parent == OBJ_H, gc_parent, OBJ_H);

  const auto who = api(object_api::CALL_METHOD, OBJ_GC, 0, 0);
  shizuku_compat_probe_state[4] = (uint32_t)g_gc_who_value;
  BOARD::diag_printf(
      "[COMPAT-PROBE] (a) grandchild raw svc: call error=%lu value=%lu raw error=%lu raw value=%lu\n",
      (unsigned long)who.error, (unsigned long)who.value,
      (unsigned long)g_gc_who_error, (unsigned long)g_gc_who_value);
  check("(a) grandchild raw svc reached H", who.value == WHO_H, who.value,
        WHO_H);

  // ---- (b) HANDLER 枠の中の YIELD と再 GRANT -----------------------------
  g_h_phase = 0;
  g_c_after_svc = 0;
  g_c_svc_value = 0;
  const auto spawned = api(object_api::SPAWN, OBJ_C, 0, 2);
  check("(b) spawn C(op=2)", spawned.error == (uintptr_t)object_error::OK,
        spawned.error, 0);
  if (spawned.error == (uintptr_t)object_error::OK) {
    const auto g1 = api(object_api::RUN_FOR, spawned.value, lend);
    shizuku_compat_probe_state[5] = (uint32_t)g1.error;
    shizuku_compat_probe_state[6] = (uint32_t)g1.value;
    shizuku_compat_probe_state[7] = g_h_phase;
    BOARD::diag_printf(
        "[COMPAT-PROBE] (b) 1st RUN_FOR error=%lu value=%lu h_phase=%lu c_after=%lu\n",
        (unsigned long)g1.error, (unsigned long)g1.value,
        (unsigned long)g_h_phase, (unsigned long)g_c_after_svc);
    check("(b) 1st grant returned to lender by YIELD from HANDLER frame",
          g1.error == 0 && g1.value == 1, g1.value, 1);
    check("(b) H reached YIELD (phase 1) but did not finish", g_h_phase == 1 &&
          g_c_after_svc == 0, g_h_phase, 1);

    const auto *handler_frame = kernel_instance.ledger_entry(spawned.value, 0);
    const auto *root_frame = kernel_instance.ledger_entry(spawned.value, 1);
    const bool base_shape = kernel_instance.ledger_size(spawned.value) == 2 &&
        root_frame && root_frame->via == KERNEL::LEDGER_DISPATCH &&
        root_frame->object == KERNEL_OBJECT::KERNEL_OBJECT_ID &&
        root_frame->kind == (uint8_t)object_kind::KERNEL_OBJECT &&
        root_frame->handler_object == KERNEL_OBJECT::KERNEL_OBJECT_ID &&
        root_frame->handler_entry == kernel_instance.object_handler_entry() &&
        handler_frame && handler_frame->via == KERNEL::LEDGER_DISPATCH &&
        handler_frame->object == OBJ_H &&
        handler_frame->kind == (uint8_t)object_kind::HANDLER &&
        handler_frame->handler_object == KERNEL_OBJECT::KERNEL_OBJECT_ID &&
        handler_frame->handler_entry == kernel_instance.object_handler_entry() &&
        kernel_instance.ledger_entry(spawned.value, 2) == nullptr &&
        compat_probe_access::base_object(spawned.value) == OBJ_C;
    check("(b) SPAWN base thread has exact KOBJ/HANDLER two-frame ledger",
          base_shape, kernel_instance.ledger_size(spawned.value), 2);
    check("(b) handler dispatch records base unwind floor",
          handler_frame && handler_frame->unwind_floor == 0 &&
              g_h_unwind_floor == handler_frame->unwind_floor,
          g_h_unwind_floor, 0);

    // Replacing the registered parent while the spawned thread is suspended
    // must fail closed. The kernel-copied handler remains H, but the object
    // registry now says KOBJ; the interposition helper must not trust shape
    // alone or allow a mismatched parent to unwind frames.
    const auto wrong_parent = api(object_api::CREATE_OBJECT, OBJ_C,
                                  method_entry<ARCH, &child_main>(), OBJECT_REPLACE);
    check("(b) root replacement changes registered parent to KOBJ",
          wrong_parent.error == 0 &&
              kernel_object_instance.object_parent_handler(OBJ_C) ==
                  KERNEL_OBJECT::KERNEL_OBJECT_ID,
          kernel_object_instance.object_parent_handler(OBJ_C),
          KERNEL_OBJECT::KERNEL_OBJECT_ID);
    check("(b) policy replacement does not alter active unwind floor",
          handler_frame && handler_frame->unwind_floor == g_h_unwind_floor,
          g_h_unwind_floor, handler_frame ? handler_frame->unwind_floor : UINT32_MAX);

    const auto restore_parent = api(object_api::CALL_METHOD, OBJ_H, 0,
                                    CMD_RECREATE);
    check("(b) H restores C as its registered child",
          restore_parent.error == 0 && restore_parent.value == OBJ_C &&
              kernel_object_instance.object_parent_handler(OBJ_C) == OBJ_H,
          kernel_object_instance.object_parent_handler(OBJ_C), OBJ_H);
    check("(b) restored parent leaves active unwind floor unchanged",
          handler_frame && handler_frame->unwind_floor == g_h_unwind_floor,
          g_h_unwind_floor, handler_frame ? handler_frame->unwind_floor : UINT32_MAX);

    const auto g2 = api(object_api::RUN_FOR, spawned.value, lend);
    shizuku_compat_probe_state[8] = (uint32_t)g2.error;
    shizuku_compat_probe_state[9] = (uint32_t)g2.value;
    shizuku_compat_probe_state[10] = g_h_phase;
    shizuku_compat_probe_state[11] = g_c_after_svc;
    BOARD::diag_printf(
        "[COMPAT-PROBE] (b) 2nd RUN_FOR error=%lu value=%lu h_phase=%lu c_after=%lu c_value=%lx\n",
        (unsigned long)g2.error, (unsigned long)g2.value,
        (unsigned long)g_h_phase, (unsigned long)g_c_after_svc,
        (unsigned long)g_c_svc_value);
    check("(b) re-GRANT resumed H from YIELD (phase 2)", g_h_phase == 2,
          g_h_phase, 2);
    check("(b) HANDLER recorded phase 2 before replying to the base task",
          shizuku_compat_probe_state[14] == 2,
          shizuku_compat_probe_state[14], 2);
    check("(b) C resumed after the svc returned with H's value",
          g_c_after_svc == 1 && g_c_svc_value == YIELD_DONE, g_c_svc_value,
          YIELD_DONE);
    check("(b) base task exited by EXIT_THREAD and yielded its grant",
          g2.error == 0 && g2.value == 1, g2.value, 1);
  }
  check("depth restored on lender", kernel_instance.current_depth() == 0,
        kernel_instance.current_depth(), 0);

  shizuku_compat_probe_state[12] = g_cp_pass;
  shizuku_compat_probe_state[13] = g_cp_fail;
  shizuku_compat_probe_state[15] = 0xC0DEC0DEu; // 完了印
  // 起動直後の出力はホストが繋ぐ前に消えるので、結果を繰り返し出す。
  for (unsigned round = 0; round < 40; ++round) {
    BOARD::diag_printf(
        "[COMPAT-PROBE] SUMMARY pass=%lu fail=%lu gc_parent=%lu (H=%lu)\n",
        (unsigned long)g_cp_pass, (unsigned long)g_cp_fail,
        (unsigned long)gc_parent, (unsigned long)OBJ_H);
    api(object_api::SLEEP_US, 500000);
  }
}

} // namespace selftest
} // namespace shizuku
