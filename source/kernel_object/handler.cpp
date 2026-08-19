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

// 実装より前に使う (下の入口が handle を呼ぶ) ので、特殊化の宣言を先に置く。
// これが無いと暗黙の実体化が先に起きて「実体化後の特殊化」になる。
template <>
uintptr_t KERNEL_OBJECT::handle(uintptr_t, uintptr_t, uintptr_t, uintptr_t,
                                uintptr_t);
template <> void KERNEL_OBJECT::init();
template <> uintptr_t KERNEL_OBJECT::handler_entry();
template <> void KERNEL_OBJECT::reply(object_error, uintptr_t, uintptr_t);
template <>
uintptr_t KERNEL_OBJECT::create_object(uintptr_t, uintptr_t, object_error &);
template <>
uintptr_t KERNEL_OBJECT::export_method(uintptr_t, uintptr_t, object_error &);
template <>
uintptr_t KERNEL_OBJECT::call_method(uintptr_t, uintptr_t, uintptr_t, uintptr_t,
                                     object_error &);
template <> void KERNEL_OBJECT::exit_method(uintptr_t, uintptr_t, uintptr_t);

// カーネルが起こすハンドラの入口。ARCH の ABI シムが callee-saved で渡された情報を
// 第 5..8 引数へ変換してくれるので、素の C++ 関数として書ける。
// (a0..a3 = 発行元の引数, 第 5 = svc 番号, 第 8 = 今のネスト数)
// ★内部リンケージにしないこと — アドレスをシム内のリテラルへ即値として埋めるので、
//   シンボルとして見える必要がある。
uintptr_t handler_entry_point(uintptr_t a0, uintptr_t a1, uintptr_t a2,
                              uintptr_t a3, uintptr_t number, uintptr_t,
                              uintptr_t, uintptr_t depth) {
  (void)a0; // a0 は番号そのもの (レジスタ渡し ABI)。番号は第 5 引数で受ける
  return kernel_object_instance.handle(number, a1, a2, a3, depth);
}

template <> uintptr_t KERNEL_OBJECT::handler_entry() {
  return ARCH::handler_entry<&handler_entry_point>();
}

template <> void KERNEL_OBJECT::init() {
  for (uintptr_t id = 0; id < OBJECT_COUNT; ++id) {
    m_objects[id].created = false;
    for (uintptr_t method = 0; method < METHOD_COUNT; ++method)
      m_objects[id].methods[method] = nullptr;
  }
  for (uintptr_t thread = 0; thread < KERNEL::THREAD_COUNT; ++thread)
    m_shadow[thread].depth = 0;
  // ブートスレッドが名乗るオブジェクトだけは最初から在るものとして扱う。
  m_objects[ROOT_OBJECT].created = true;
}

// 巻き戻さずにその場で答える。**エラーを黙って捨てない**ための共通口 (D12)。
template <>
void KERNEL_OBJECT::reply(object_error error, uintptr_t value,
                          uintptr_t depth) {
  ARCH::syscall((uintptr_t)primitive::RETURN, 1, value, (uintptr_t)error, depth);
  // 成功すれば戻らない。
}

template <>
uintptr_t KERNEL_OBJECT::create_object(uintptr_t id, uintptr_t entry,
                                       object_error &error) {
  if (id >= OBJECT_COUNT || id == ROOT_OBJECT) {
    error = object_error::BAD_OBJECT;
    return 0;
  }
  if (m_objects[id].created) {
    error = object_error::ALREADY_EXISTS;
    return 0;
  }
  m_objects[id].created = true;
  // 最初のメソッドは生成側が与える (オブジェクト自身はまだ走っていないので
  // 自分では登録できない)。以後は EXPORT_METHOD で自分が増やす。
  m_objects[id].methods[0] = (method_t)entry;
  return id;
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
  m_objects[self].methods[method] = (method_t)entry;
  return self;
}

template <>
uintptr_t KERNEL_OBJECT::call_method(uintptr_t id, uintptr_t method,
                                     uintptr_t argument, uintptr_t depth,
                                     object_error &error) {
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
  // 呼び先は RETURN を撃てないので、戻り口は exit API を撃つコードにする (D5)。
  request.return_pc =
      ARCH::object_exit_stub<(uintptr_t)object_api::EXIT_METHOD>();
  request.protection = PROTECTION_PRIVILEGED; // 非特権化は Phase 5
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
  (void)depth;
  return result.value;
}

template <>
void KERNEL_OBJECT::exit_method(uintptr_t value, uintptr_t extra,
                                uintptr_t depth) {
  const uint32_t thread = kernel_instance.current_thread_id();
  shadow_t &shadow = m_shadow[thread];
  // 落とすのは「この exit を運んだ枠」+「戻ろうとしている呼び先の枠」= 2 枚。
  // 追加段数 (extra) は発行元が「もっと畳んでほしい」と申告した分 (異常系の
  // 連鎖巻き戻し用)。段数は必ず申告して両側チェックを通す (§9.3)。
  const uintptr_t count = 2 + extra;
  const uintptr_t pops = 1 + extra; // 台帳から落ちるのは呼び先ぶんだけ
  // 巻き戻しに成功するとここへは戻らないので、台帳は**先に**落としておく。
  shadow.depth = shadow.depth >= pops ? shadow.depth - (uint32_t)pops : 0;
  const auto result =
      ARCH::syscall((uintptr_t)primitive::RETURN, count, value, 0, depth);
  // ★ここへ戻ってきた = 巻き戻しが検算で弾かれた。台帳を元へ戻し、発行元へ
  //   エラーとして返す (系は落とさない = I-9)。
  shadow.depth += (uint32_t)pops;
  reply(object_error::UNWIND_REJECTED, (uintptr_t)result.error, depth);
}

template <>
uintptr_t KERNEL_OBJECT::handle(uintptr_t number, uintptr_t a1, uintptr_t a2,
                                uintptr_t a3, uintptr_t depth) {
  (void)a3;
  object_error error = object_error::OK;
  uintptr_t value = 0;
  switch ((object_api)number) {
  case object_api::CREATE_OBJECT:
    value = create_object(a1, a2, error);
    break;
  case object_api::EXPORT_METHOD:
    value = export_method(a1, a2, error);
    break;
  case object_api::CALL_METHOD:
    value = call_method(a1, a2, a3, depth, error);
    break;
  case object_api::EXIT_METHOD:
    // 巻き戻しに成功すればここから戻らない。
    exit_method(a1, a2, depth);
    return 0;
  case object_api::GET_CURRENT_OBJECT:
    value = current_object(kernel_instance.current_thread_id());
    break;
  case object_api::GET_CALLER_OBJECT:
    value = caller_object(kernel_instance.current_thread_id());
    break;
  default:
    // ★未知の番号は**ここの語彙**。黙って捨てず必ずエラーで返す
    //   (DESIGN §11.2.0 の「無音で消えて誤認した」事故の対策)。
    error = object_error::UNKNOWN_API;
    break;
  }
  if (error != object_error::OK)
    reply(error, 0, depth);
  // 普通に return すると、カーネルの戻り口が自分の枠を 1 枚落として発行元へ返る。
  return value;
}

} // namespace shizuku
