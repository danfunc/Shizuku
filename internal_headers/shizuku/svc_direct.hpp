#ifndef SHIZUKU_SVC_DIRECT_HPP
#define SHIZUKU_SVC_DIRECT_HPP
// ===========================================================================
//  例外入口が番号の表から直接入る API の集合 (ARM の SVC_Handler)
// ===========================================================================
//  C6 の shizuku_rv32_ecall_table に相当する。svc の番号は即値ではなくレジスタ (r0 = a0) で渡るので、
//  入口は退避済みの例外フレームから r0 を 1 回読んで表を引く。
//  直接入る API は、svc_attributes の mode == handler のうち、実行権の切替えを起こさないもの。
//  YIELD / RUN_FOR は pend_context_switch を使うので汎用の経路 (svc_dispatch) のままにする。
#include <array>
#include <cstdint>
#include "shizuku/object_api.hpp"
#include "shizuku/svc_attributes.hpp"

namespace shizuku {

enum class svc_direct_kind : uint8_t {
  NONE = 0,
  GET_CURRENT,
  GET_CALLER,
  EXIT_METHOD,
  FORWARD_CHILD_EXIT,
  CALL_METHOD,
  SET_BUDGET,
  CALL_DESCRIPTOR,
};

inline constexpr unsigned svc_direct_table_size = 34;
// 番号 → 種別。switch で書くのは、例外入口が番号を直接ジャンプ表 (tbb) にできるようにするため
// (配列を引くと、フラッシュの表の読みと範囲検査が 1 段増える)。下の static_assert が全番号で
// svc_attributes と突き合わせる。
constexpr svc_direct_kind svc_direct_of(uintptr_t id) {
  switch (id) {
  case (uintptr_t)object_api::GET_CURRENT_OBJECT: return svc_direct_kind::GET_CURRENT;
  case (uintptr_t)object_api::GET_CALLER_OBJECT: return svc_direct_kind::GET_CALLER;
  case (uintptr_t)object_api::EXIT_METHOD: return svc_direct_kind::EXIT_METHOD;
  case (uintptr_t)object_api::FORWARD_CHILD_EXIT: return svc_direct_kind::FORWARD_CHILD_EXIT;
  case (uintptr_t)object_api::CALL_METHOD: return svc_direct_kind::CALL_METHOD;
  case (uintptr_t)object_api::SET_BUDGET: return svc_direct_kind::SET_BUDGET;
  case (uintptr_t)object_api::CALL_METHOD_DESCRIPTOR: return svc_direct_kind::CALL_DESCRIPTOR;
  default: return svc_direct_kind::NONE;
  }
}

// 表と svc_attributes が食い違わないこと: 直接入る API は mode == handler で、handler のうち
// 直接入らないのは実行権を渡す YIELD と RUN_FOR だけ。
constexpr bool svc_direct_table_consistent() {
  static_assert(svc_direct_table_size == svc_attributes.size());
  // 範囲外 (表の外) の番号は直接入らない。
  if (svc_direct_of(svc_direct_table_size) != svc_direct_kind::NONE ||
      svc_direct_of(UINTPTR_MAX) != svc_direct_kind::NONE)
    return false;
  for (unsigned id = 0; id < svc_direct_table_size; ++id) {
    const auto *a = svc_attribute_for(id);
    const bool handler = a != nullptr && a->mode == svc_mode::handler;
    const bool direct = svc_direct_of(id) != svc_direct_kind::NONE;
    const bool switches = id == (unsigned)object_api::YIELD || id == (unsigned)object_api::RUN_FOR;
    if (direct && !handler) return false;
    if (handler && direct == switches) return false;
    if (direct && switches) return false;
  }
  return true;
}
static_assert(svc_direct_table_consistent(),
              "svc_direct_of must match svc_attributes (mode == handler, minus YIELD / RUN_FOR)");

} // namespace shizuku
#endif // SHIZUKU_SVC_DIRECT_HPP
