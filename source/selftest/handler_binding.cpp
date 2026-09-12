// ===========================================================================
//  親ハンドラ自動導出 & object-land syscall ルーティングの自己テスト
// ===========================================================================
#include "shizuku/kernel.hpp"
#include "shizuku/kernel_object.hpp"
#include "shizuku/object_ids.hpp"
#include "shizuku/object_api.hpp"
#include "shizuku/selftest.hpp"

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

constexpr uintptr_t CMD_SETUP   = 0x01;
constexpr uintptr_t CMD_COMPUTE = 0x10;
constexpr uintptr_t CMD_CALL_KOBJ = 0x20;
constexpr uintptr_t CMD_NEST    = 0x30;

void check(const char *name, bool ok, unsigned long got, unsigned long want) {
  if (ok) {
    ++passed;
    BOARD::diag_printf("[SELFTEST] PASS %s (=%lu)\n", name, got);
  } else {
    ++failed;
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
  const auto result = ARCH::syscall(CMD_COMPUTE, arg, 0, 0);
  return result.value;
}

uintptr_t child_a_try_primitive(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  // 子 A がカーネルプリミティブ (RETURN = 2) を直接撃とうと試みる
  // カーネルプリミティブは KERNEL_OBJECT 以外には実行されず、
  // 親ハンドラへ SVC 2 として配送されるだけ。
  const auto result = ARCH::syscall((uintptr_t)primitive::RETURN, 1, 0x55, 0);
  return result.value;
}

// 子 B の実装
uintptr_t child_b_compute(uintptr_t arg, uintptr_t, uintptr_t, uintptr_t) {
  const auto result = ARCH::syscall(CMD_COMPUTE, arg, 0, 0);
  return result.value;
}

// 親ハンドラ A のエントリポイント
uintptr_t handler_a_entry(uintptr_t a0, uintptr_t a1, uintptr_t a2,
                          uintptr_t a3) {
  (void)a2;
  (void)a3;
  if (a0 == CMD_SETUP) {
    // 親ハンドラ A 自身が子 A を生成する (親から自動導出: 指摘 1, 5, 7)
    // 作成者が HANDLER なので、child_a の parent_handler は OBJ_PARENT_A に自動設定される
    const auto res = api(object_api::CREATE_OBJECT, OBJ_CHILD_A,
                         (uintptr_t)&child_a_compute, 0);
    return res.error == 0 ? res.value : 0;
  }
  if (a0 == CMD_COMPUTE) {
    // 親ハンドラ A の解釈: arg + 100
    return a1 + 100;
  }
  if (a0 == (uintptr_t)primitive::RETURN) {
    // 子 A が primitive::RETURN を撃ってきたが、親ハンドラ A に通常の SVC として届いた証拠
    return 0x7777;
  }
  if (a0 == CMD_CALL_KOBJ) {
    // 専用ハンドラ自身の SVC が Root Kernel Object へ届くかの検証 (指摘 5)
    // kobj の YIELD (9) を呼んでみる
    const auto res = ARCH::syscall((uintptr_t)object_api::YIELD);
    return (res.error == 0) ? 0x9999 : 0xEEEE;
  }
  if (a0 == CMD_NEST) {
    // ネスト呼び出し: 親ハンドラ A から子 B のメソッドを呼ぶ
    const auto res = call_method(OBJ_CHILD_B, 0, a1);
    return res.value + 1;
  }
  return 0xEEEE;
}

// 親ハンドラ B のエントリポイント
uintptr_t handler_b_entry(uintptr_t a0, uintptr_t a1, uintptr_t a2,
                          uintptr_t a3) {
  (void)a2;
  (void)a3;
  if (a0 == CMD_SETUP) {
    // 親ハンドラ B 自身が子 B を生成する (親から自動導出)
    const auto res = api(object_api::CREATE_OBJECT, OBJ_CHILD_B,
                         (uintptr_t)&child_b_compute, 0);
    return res.error == 0 ? res.value : 0;
  }
  if (a0 == CMD_COMPUTE) {
    // 親ハンドラ B の解釈: arg + 200 (同じ API 番号 0x10 が別解釈される)
    return a1 + 200;
  }
  return 0xFFFF;
}

// 非特権オブジェクトのテスト用メソッド
uintptr_t unpriv_entry(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  // 非特権オブジェクトが OBJECT_HANDLER を指定してオブジェクト生成を試みる
  const auto res = api(object_api::CREATE_OBJECT, OBJ_CHILD_A, 0x1000,
                       OBJECT_HANDLER | OBJECT_REPLACE);
  return res.error == 0 ? 1 : 0;
}

} // namespace

void handler_binding_probe() {
  BOARD::diag_printf("[SELFTEST] === parent handling object routing probe ===\n");

  // 1. 特権オブジェクト (Root) による専用ハンドラ H_A, H_B の宣言
  // OBJECT_HANDLER フラグを指定して生成 (指摘 1)
  const auto res_ha = api(object_api::CREATE_OBJECT, OBJ_PARENT_A,
                          (uintptr_t)&handler_a_entry,
                          OBJECT_HANDLER | OBJECT_REPLACE);
  check("create handler_a with OBJECT_HANDLER", res_ha.error == 0,
        res_ha.error, 0);

  const auto res_hb = api(object_api::CREATE_OBJECT, OBJ_PARENT_B,
                          (uintptr_t)&handler_b_entry,
                          OBJECT_HANDLER | OBJECT_REPLACE);
  check("create handler_b with OBJECT_HANDLER", res_hb.error == 0,
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

  // 2. 非特権オブジェクトによる OBJECT_HANDLER 宣言の偽装拒否 (指摘 1)
  const auto res_u = api(object_api::CREATE_OBJECT, OBJ_UNPRIV, (uintptr_t)&unpriv_entry,
                         OBJECT_UNPRIVILEGED | OBJECT_REPLACE);
  check("create unprivileged object", res_u.error == 0, res_u.error, 0);
  const auto unpriv_res = call_method(OBJ_UNPRIV, 0, 0);
  check("unprivileged cannot specify OBJECT_HANDLER",
        unpriv_res.value == 0, unpriv_res.value, 0);

  // 3. 親ハンドラからの子の自動導出 (裏口 API なし: 指摘 1, 5, 7)
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

  // 4. 同一 API 番号 (0x10) の独立解釈 & child SVC → 登録済み parent (指摘 5, 要件)
  const auto res_a = call_method(OBJ_CHILD_A, 0, 10);
  check("child_a SVC routed to parent_a (+100)",
        res_a.error == 0 && res_a.value == 110, res_a.value, 110);

  const auto res_b = call_method(OBJ_CHILD_B, 0, 10);
  check("child_b SVC routed to parent_b (+200)",
        res_b.error == 0 && res_b.value == 210, res_b.value, 210);

  // 5. 専用ハンドラ自身の SVC → Root Kernel Object (指摘 5, 要件)
  // handler_a に CMD_CALL_KOBJ を実行させ、内部で SVC (YIELD) を呼ぶ
  const auto kobj_call_res = call_method(OBJ_PARENT_A, 0, CMD_CALL_KOBJ);
  check("handler_a own SVC routed to root kobj",
        kobj_call_res.value == 0x9999, kobj_call_res.value, 0x9999);

  // 6. kernel primitive の直接利用は拒否 & 偽装拒否 (指摘 3, 4, 要件)
  // child_a から primitive::RETURN を撃つ → 親ハンドラへ通常の SVC として届く
  api(object_api::CREATE_OBJECT, OBJ_CHILD_A, (uintptr_t)&child_a_try_primitive,
      OBJECT_REPLACE);
  const auto prim_res = call_method(OBJ_CHILD_A, 0, 0);
  check("child cannot invoke kernel primitive directly (routed to handler as SVC)",
        prim_res.value == 0x7777, prim_res.value, 0x7777);

  // 7. ネスト呼び出し (nested CALL/RETURN) とスタック復元 (指摘 6, 要件)
  // child_a を再度 compute に戻す
  api(object_api::CREATE_OBJECT, OBJ_CHILD_A, (uintptr_t)&child_a_compute,
      OBJECT_REPLACE);
  const auto nest_res = call_method(OBJ_PARENT_A, 0, CMD_NEST);
  // handler_a から child_b (arg=0, +200) を呼び出し、+1 して 201
  check("nested CALL/RETURN across handlers",
        nest_res.value == 201, nest_res.value, 201);
  check("depth fully restored after nested calls",
        kernel_instance.current_depth() == 0,
        kernel_instance.current_depth(), 0);

  BOARD::diag_printf("[SELFTEST] parent handling object routing probe completed.\n");
}

} // namespace selftest
} // namespace shizuku
