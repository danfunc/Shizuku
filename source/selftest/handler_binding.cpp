// ===========================================================================
//  親ハンドラ自動導出 & object-land syscall ルーティングの自己テスト
// ===========================================================================
#include "shizuku/kernel.hpp"
#include "shizuku/kernel_object.hpp"
#include "shizuku/object_ids.hpp"
#include "shizuku/object_api.hpp"
#include "shizuku/method_entry.hpp"
#include "shizuku/selftest.hpp"
#include "shizuku/kernel_services.hpp"

namespace shizuku {
namespace selftest {

namespace {

using ARCH = KERNEL::ARCH;
using BOARD = KERNEL::BOARD;

constexpr uintptr_t OBJ_PARENT_A = object_id::binding_parent_a;
constexpr uintptr_t OBJ_PARENT_B = object_id::binding_parent_b;
constexpr uintptr_t OBJ_CHILD_A  = object_id::binding_child_a;
constexpr uintptr_t OBJ_CHILD_B  = object_id::binding_child_b;
constexpr uintptr_t OBJ_UNPRIV   = object_id::binding_unpriv;

constexpr uintptr_t CMD_SETUP           = 0x01;
constexpr uintptr_t CMD_COMPUTE         = 0x10;
constexpr uintptr_t CMD_CALL_KOBJ       = 0x20;
constexpr uintptr_t CMD_CALL_KOBJ_GET   = 0x21;
constexpr uintptr_t CMD_NEST            = 0x30;
constexpr uintptr_t CMD_NEST_NONZERO    = 0x31;
constexpr uintptr_t CMD_SETUP_PRIMITIVE = 0x40;
constexpr uintptr_t CMD_SETUP_COMPUTE   = 0x41;
constexpr uintptr_t CMD_CALL_CHILD      = 0x5000;

void check(const char *name, bool ok, unsigned long got, unsigned long want) {
  if (ok) {
    ++passed;
    BOARD::diag_printf("[SELFTEST] PASS %s (=%lu)\n", name, got);
  } else {
    ++failed;
    record_fail(name, got, want);
    BOARD::diag_printf("[SELFTEST] FAIL %s: got %lu want %lu\n", name, got,
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

api_result call_method(uintptr_t id, uintptr_t method, uintptr_t arg = 0) {
  return api(object_api::CALL_METHOD, id, method, arg);
}

// 子 A の実装
uintptr_t child_a_compute(uintptr_t arg, uintptr_t, uintptr_t, uintptr_t) {
  if (arg == 55) {
    // 段V1: 子Aから raw SVC 5 (GET_CURRENT_OBJECT 番号) を発行
    const auto result = ARCH::syscall((uintptr_t)object_api::GET_CURRENT_OBJECT);
    return result.value;
  }
  if (arg == 66) {
    // 段V1: 子Aから raw SVC 6 (GET_CALLER_OBJECT 番号) を発行
    const auto result = ARCH::syscall((uintptr_t)object_api::GET_CALLER_OBJECT);
    return result.value;
  }
  const auto result = ARCH::syscall(CMD_COMPUTE, arg, 0, 0);
#if defined(__arm__) || defined(__thumb__)
  const auto service = kernel_services::yield();
  // Its parent returns 0xEEEE for unknown requests. The internal gate must
  // reach native scheduling (value 0/1), while the raw request above stays +100.
  if (service.code != kernel_services::status::ok || service.value > 1)
    return 0xDEAD;
#endif
  return result.value;
}

uintptr_t child_a_try_primitive(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  // 子 A がカーネルプリミティブ (RETURN = 2) を直接撃とうと試みる
  // count = 99 (異常値), value = 0x55。
  // カーネルプリミティブは KERNEL_OBJECT 以外には実行されず、
  // 親ハンドラへ SVC 2 として配送される。
  const auto result = ARCH::syscall((uintptr_t)primitive::RETURN, 99, 0x55, 0);
  return result.value;
}

// 子 B の実装
uintptr_t child_b_compute(uintptr_t arg, uintptr_t, uintptr_t, uintptr_t) {
  const auto result = ARCH::syscall(CMD_COMPUTE, arg, 0, 0);
  return result.value;
}

// 親ハンドラ A のエントリポイント
uintptr_t handler_a_entry(uintptr_t a0, uintptr_t a1, uintptr_t a2,
                          uintptr_t a3, uintptr_t unwind_floor) {
  if (a0 == CMD_SETUP) {
    // 親ハンドラ A 自身が子 A を生成する (親から自動導出: 重大指摘 1, 3)
    const auto res = api(object_api::CREATE_OBJECT, OBJ_CHILD_A,
                         method_entry<ARCH, &child_a_compute>(), 0);
    return res.error == 0 ? res.value : 0;
  }
  if (a0 == CMD_SETUP_PRIMITIVE) {
    // 親ハンドラ A 自身が子 A を replace (親 binding を維持: 重大指摘 3)
    const auto res = api(object_api::CREATE_OBJECT, OBJ_CHILD_A,
                         method_entry<ARCH, &child_a_try_primitive>(), OBJECT_REPLACE);
    return res.error == 0 ? res.value : 0;
  }
  if (a0 == CMD_SETUP_COMPUTE) {
    // 親ハンドラ A 自身が子 A を compute に戻す (重大指摘 3)
    const auto res = api(object_api::CREATE_OBJECT, OBJ_CHILD_A,
                         method_entry<ARCH, &child_a_compute>(), OBJECT_REPLACE);
    return res.error == 0 ? res.value : 0;
  }
  if (a0 == CMD_COMPUTE) {
    // 親ハンドラ A の解釈: arg + 100 (通常 reply)
    return a1 + 100;
  }
  if (a0 == (uintptr_t)primitive::RETURN) {
    if (a1 == unwind_floor && a2 != 0x55) {
      // The return stub's marker has become the absolute caller depth.
      api(object_api::FORWARD_CHILD_EXIT, unwind_floor, a2, a3);
      return 0;
    }
    // 子 A が primitive::RETURN を直接撃ってきた場合 (a2 == 0x55)
    // 通常の SVC として受け取った証拠の値を返す (reply 経由で Child A へ戻る)
    return 0x7777;
  }
  if (a0 == (uintptr_t)object_api::GET_CURRENT_OBJECT) {
    // 段V1: 子Aからの生の SVC 5 (GET_CURRENT_OBJECT と同一番号) が
    // 親ハンドラへ横取りされず配送された証拠
    return 0x5555;
  }
  if (a0 == (uintptr_t)object_api::GET_CALLER_OBJECT) {
    // 段V1: 子Aからの生の SVC 6 (GET_CALLER_OBJECT と同一番号) が
    // 親ハンドラへ横取りされず配送された証拠
    return 0x6666;
  }
  if (a0 == CMD_CALL_KOBJ) {
    // 専用ハンドラ自身の SVC が Root Kernel Object へ届くかの検証
    const auto res = ARCH::syscall((uintptr_t)object_api::YIELD);
    return (res.error == 0) ? 0x9999 : 0xEEEE;
  }
  if (a0 == CMD_CALL_KOBJ_GET) {
    // 専用ハンドラ自身の親は Root KOBJ (to_root == true) なので、
    // GET_CURRENT_OBJECT はカーネル直接解釈され OBJ_PARENT_A が返る
    const auto res = ARCH::syscall((uintptr_t)object_api::GET_CURRENT_OBJECT);
    return (res.error == 0 && res.value == OBJ_PARENT_A) ? 0x8888 : 0xEEEE;
  }
  if ((a0 & 0xFF00) == CMD_CALL_CHILD) {
    const auto res = call_method(OBJ_CHILD_A, 0, a0 & 0xFF);
    return res.error == 0 ? res.value : 0xEEEE;
  }
  if (a0 == CMD_NEST || a0 == CMD_NEST_NONZERO) {
    // 親子チェック規則により、H_A から別親の子 OBJ_CHILD_B を直接 CALL_METHOD できない (兄弟の子)。
    // ハンドラ境界をまたぐ nested call は、自身の子 OBJ_CHILD_A を呼び、OBJ_CHILD_A の SVC が
    // 親 H_A へ配送されることで検証する。
    // child_argument を 100 / 107 とすることで、親の +100 と加算 +1 により期待値 201 / 208 を維持する。
    const uintptr_t child_argument = a0 == CMD_NEST ? 100 : 107;
    const auto res = call_method(OBJ_CHILD_A, 0, child_argument);
    return res.value + 1;
  }
  return 0xEEEE;
}

// 親ハンドラ B のエントリポイント
uintptr_t handler_b_entry(uintptr_t a0, uintptr_t a1, uintptr_t a2,
                          uintptr_t a3, uintptr_t unwind_floor) {
  if (a0 == CMD_SETUP) {
    const auto res = api(object_api::CREATE_OBJECT, OBJ_CHILD_B,
                         method_entry<ARCH, &child_b_compute>(), 0);
    return res.error == 0 ? res.value : 0;
  }
  if ((a0 & 0xFF00) == CMD_CALL_CHILD) {
    const auto res = call_method(OBJ_CHILD_B, 0, a0 & 0xFF);
    return res.error == 0 ? res.value : 0xFFFF;
  }
  if (a0 == CMD_COMPUTE) {
    return a1 + 200;
  }
  if (a0 == (uintptr_t)primitive::RETURN) {
    if (a1 == unwind_floor) {
      api(object_api::FORWARD_CHILD_EXIT, unwind_floor, a2, a3);
      return 0;
    }
    return 0xFFFF;
  }
  return 0xFFFF;
}

// 一般オブジェクト (privileged) のテスト用メソッド
uintptr_t unpriv_entry(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  // 一般オブジェクトが INTERNAL_FLAG_HANDLER を指定してオブジェクト生成を試みる (重大指摘 1)
  // creator が ROOT_OBJECT / KERNEL_OBJECT ではないため、NOT_PRIVILEGED で拒否されなければならない
  const auto res = api(object_api::CREATE_OBJECT, OBJ_CHILD_A, 0x1000,
                       KERNEL_OBJECT::INTERNAL_FLAG_HANDLER | OBJECT_REPLACE);
  return res.error == 0 ? 1 : 0;
}

} // namespace

void handler_binding_probe() {
  BOARD::diag_printf("[SELFTEST] === parent handling object routing probe ===\n");

  // 1. 特権オブジェクト (Root) による専用ハンドラ H_A, H_B の宣言
  // 信頼済み内部フラグ INTERNAL_FLAG_HANDLER を指定して生成 (重大指摘 1)
  const auto res_ha = api(object_api::CREATE_OBJECT, OBJ_PARENT_A,
                          method_entry<ARCH, &handler_a_entry>(),
                          KERNEL_OBJECT::INTERNAL_FLAG_HANDLER | OBJECT_REPLACE);
  check("create handler_a with INTERNAL_FLAG_HANDLER", res_ha.error == 0,
        res_ha.error, 0);

  const auto res_hb = api(object_api::CREATE_OBJECT, OBJ_PARENT_B,
                          method_entry<ARCH, &handler_b_entry>(),
                          KERNEL_OBJECT::INTERNAL_FLAG_HANDLER | OBJECT_REPLACE);
  check("create handler_b with INTERNAL_FLAG_HANDLER", res_hb.error == 0,
        res_hb.error, 0);

  // 専用ハンドラ自身の親ハンドラは root kernel object
  check("handler_a parent is root kobj",
        kernel_object_instance.object_parent_handler(OBJ_PARENT_A) == KERNEL_OBJECT::KERNEL_OBJECT_ID,
        kernel_object_instance.object_parent_handler(OBJ_PARENT_A),
        KERNEL_OBJECT::KERNEL_OBJECT_ID);
  check("handler_b parent is root kobj",
        kernel_object_instance.object_parent_handler(OBJ_PARENT_B) == KERNEL_OBJECT::KERNEL_OBJECT_ID,
        kernel_object_instance.object_parent_handler(OBJ_PARENT_B),
        KERNEL_OBJECT::KERNEL_OBJECT_ID);

  // 2. 一般オブジェクト (privileged) による INTERNAL_FLAG_HANDLER 宣言の偽装拒否 (重大指摘 1)
  const auto res_u = api(object_api::CREATE_OBJECT, OBJ_UNPRIV, method_entry<ARCH, &unpriv_entry>(),
                         OBJECT_REPLACE);
  check("create privileged non-root object", res_u.error == 0, res_u.error, 0);
  const auto unpriv_res = call_method(OBJ_UNPRIV, 0, 0);
  check("non-root privileged cannot specify INTERNAL_FLAG_HANDLER",
        unpriv_res.value == 0, unpriv_res.value, 0);

  // 3. 親ハンドラからの子の自動導出 (裏口 API なし: 重大指摘 1, 3)
  // H_A に setup を実行させ、H_A が C_A を生成
  const auto setup_a = call_method(OBJ_PARENT_A, 0, CMD_SETUP);
  check("handler_a created child_a", setup_a.value == OBJ_CHILD_A,
        setup_a.value, OBJ_CHILD_A);
  check("child_a parent automatically derived as handler_a",
        kernel_object_instance.object_parent_handler(OBJ_CHILD_A) == OBJ_PARENT_A,
        kernel_object_instance.object_parent_handler(OBJ_CHILD_A), OBJ_PARENT_A);

  // H_B に setup を実行させ、H_B が C_B を生成
  const auto setup_b = call_method(OBJ_PARENT_B, 0, CMD_SETUP);
  check("handler_b created child_b", setup_b.value == OBJ_CHILD_B,
        setup_b.value, OBJ_CHILD_B);
  check("child_b parent automatically derived as handler_b",
        kernel_object_instance.object_parent_handler(OBJ_CHILD_B) == OBJ_PARENT_B,
        kernel_object_instance.object_parent_handler(OBJ_CHILD_B), OBJ_PARENT_B);

  // 4. 同一 API 番号 (0x10) の独立解釈 & child SVC → 登録済み parent (重大指摘 2: reply & exit forwarding)
  // ROOT から孫へは直接 CALL_METHOD できないため、親 (H_A/H_B) の中継経由で呼ぶ
  const auto res_a = call_method(OBJ_PARENT_A, 0, CMD_CALL_CHILD | 10);
  check("child_a SVC routed to parent_a (+100) and exit forwarded",
        res_a.error == 0 && res_a.value == 110, res_a.value, 110);

  const auto res_b = call_method(OBJ_PARENT_B, 0, CMD_CALL_CHILD | 10);
  check("child_b SVC routed to parent_b (+200) and exit forwarded",
        res_b.error == 0 && res_b.value == 210, res_b.value, 210);

  // 4b. 段V1: native API と同一番号 (5, 6) の raw SVC が親ハンドラへ配送されることの検証
  const auto raw5_res = call_method(OBJ_PARENT_A, 0, CMD_CALL_CHILD | 55);
  check("child_a raw SVC 5 delivered to handler_a unchanged",
        raw5_res.error == 0 && raw5_res.value == 0x5555, raw5_res.value, 0x5555);

  const auto raw6_res = call_method(OBJ_PARENT_A, 0, CMD_CALL_CHILD | 66);
  check("child_a raw SVC 6 delivered to handler_a unchanged",
        raw6_res.error == 0 && raw6_res.value == 0x6666, raw6_res.value, 0x6666);

  // 5. 専用ハンドラ自身の SVC → Root Kernel Object
  // handler_a に CMD_CALL_KOBJ を実行させ、内部で SVC (YIELD) を呼ぶ
  const auto kobj_call_res = call_method(OBJ_PARENT_A, 0, CMD_CALL_KOBJ);
  check("handler_a own SVC routed to root kobj",
        kobj_call_res.value == 0x9999, kobj_call_res.value, 0x9999);

  // 5b. 段V1: 専用ハンドラ自身の GET_CURRENT_OBJECT は Root KOBJ (to_root=true) 経由で直接解釈
  const auto kobj_get_res = call_method(OBJ_PARENT_A, 0, CMD_CALL_KOBJ_GET);
  check("handler_a own GET_CURRENT_OBJECT handled directly by kernel",
        kobj_get_res.value == 0x8888, kobj_get_res.value, 0x8888);

  // 5c. 段V1: root direct GET_CURRENT_OBJECT / GET_CALLER_OBJECT で dispatch frame が積まれないことの検証
  const uint32_t d_before = kernel_instance.current_depth();
  const auto root_get_cur = api(object_api::GET_CURRENT_OBJECT);
  const uint32_t d_after = kernel_instance.current_depth();
  check("root direct GET_CURRENT_OBJECT value",
        root_get_cur.error == 0 && root_get_cur.value == KERNEL_OBJECT::ROOT_OBJECT,
        root_get_cur.value, KERNEL_OBJECT::ROOT_OBJECT);
  check("root direct GET_CURRENT_OBJECT no dispatch frame",
        d_before == d_after, d_after, d_before);

  const auto root_get_caller = api(object_api::GET_CALLER_OBJECT);
  check("root direct GET_CALLER_OBJECT at base is NO_OBJECT",
        root_get_caller.error == 0 && root_get_caller.value == KERNEL_OBJECT::NO_OBJECT,
        root_get_caller.value, KERNEL_OBJECT::NO_OBJECT);
  check("root direct GET_CALLER_OBJECT no dispatch frame",
        kernel_instance.current_depth() == d_before,
        kernel_instance.current_depth(), d_before);

  // 6. kernel primitive の直接利用は拒否 & 親自身による replace で parent binding 維持 (重大指摘 3)
  // handler_a 自身から child_a を child_a_try_primitive に replace させる
  const auto repl_prim_res = call_method(OBJ_PARENT_A, 0, CMD_SETUP_PRIMITIVE);
  check("handler_a replaced child_a method with primitive probe",
        repl_prim_res.value == OBJ_CHILD_A, repl_prim_res.value, OBJ_CHILD_A);
  check("child_a parent still handler_a after handler-initiated replace",
        kernel_object_instance.object_parent_handler(OBJ_CHILD_A) == OBJ_PARENT_A,
        kernel_object_instance.object_parent_handler(OBJ_CHILD_A), OBJ_PARENT_A);

  const auto prim_res = call_method(OBJ_PARENT_A, 0, CMD_CALL_CHILD | 0);
  check("child cannot invoke kernel primitive directly (routed to handler as SVC)",
        prim_res.value == 0x7777, prim_res.value, 0x7777);

  // 7. ネスト呼び出し (nested CALL/RETURN) とスタック復元
  // handler_a 自身から child_a を compute に戻す
  const auto repl_comp_res = call_method(OBJ_PARENT_A, 0, CMD_SETUP_COMPUTE);
  check("handler_a restored child_a to compute",
        repl_comp_res.value == OBJ_CHILD_A, repl_comp_res.value, OBJ_CHILD_A);

  const auto nest_res = call_method(OBJ_PARENT_A, 0, CMD_NEST);
  // handler_a が child_a に 100 を渡し、+100 の戻りに +1 して 201
  check("nested CALL/RETURN across handlers",
        nest_res.error == (uintptr_t)object_error::OK && nest_res.value == 201,
        nest_res.value, 201);
  const auto nest_nonzero_res = call_method(OBJ_PARENT_A, 0, CMD_NEST_NONZERO);
  check("nested CALL/RETURN with fixed nonzero child argument",
        nest_nonzero_res.error == (uintptr_t)object_error::OK &&
            nest_nonzero_res.value == 208,
        nest_nonzero_res.value, 208);
  check("depth fully restored after nested calls",
        kernel_instance.current_depth() == 0,
        kernel_instance.current_depth(), 0);

  BOARD::diag_printf("[SELFTEST] parent handling object routing probe completed.\n");
}

} // namespace selftest
} // namespace shizuku
