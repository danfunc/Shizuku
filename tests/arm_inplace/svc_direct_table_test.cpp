// ARM の SVC_Handler が番号の表から直接入る API の集合 (svc_direct.hpp) が、svc_attributes の
// mode == handler から実行権を渡す YIELD / RUN_FOR を除いたものと一致することを全番号で確かめる。
#include <cstdint>
#include <cstdio>
#include "shizuku/svc_direct.hpp"

using namespace shizuku;

static int failures = 0;
#define CHECK(cond, what)                                            \
  do {                                                               \
    if (!(cond)) {                                                   \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, what);     \
      ++failures;                                                    \
    }                                                                \
  } while (0)

int main() {
  struct expect {
    object_api api;
    svc_direct_kind kind;
  };
  // 直接入る 7 本は、それぞれ別の種別へ振り分けられる。
  const expect direct[] = {
      {object_api::EXIT_METHOD, svc_direct_kind::EXIT_METHOD},
      {object_api::CALL_METHOD, svc_direct_kind::CALL_METHOD},
      {object_api::GET_CURRENT_OBJECT, svc_direct_kind::GET_CURRENT},
      {object_api::GET_CALLER_OBJECT, svc_direct_kind::GET_CALLER},
      {object_api::SET_BUDGET, svc_direct_kind::SET_BUDGET},
      {object_api::FORWARD_CHILD_EXIT, svc_direct_kind::FORWARD_CHILD_EXIT},
      {object_api::CALL_METHOD_DESCRIPTOR, svc_direct_kind::CALL_DESCRIPTOR},
  };
  for (const auto &e : direct)
    CHECK(svc_direct_of((uintptr_t)e.api) == e.kind, "direct API maps to its own kind");

  // 実行権を渡す API は handler 属性でも直接入らない (汎用の経路で切替えを扱う)。
  CHECK(svc_direct_of((uintptr_t)object_api::YIELD) == svc_direct_kind::NONE, "YIELD is not direct");
  CHECK(svc_direct_of((uintptr_t)object_api::RUN_FOR) == svc_direct_kind::NONE, "RUN_FOR is not direct");

  // 全番号: 直接入るものは mode == handler、handler の残りは YIELD と RUN_FOR だけ。
  unsigned direct_count = 0, handler_count = 0;
  for (unsigned id = 0; id < svc_attributes.size(); ++id) {
    const bool handler = svc_attributes[id].mode == svc_mode::handler;
    const bool is_direct = svc_direct_of(id) != svc_direct_kind::NONE;
    handler_count += handler;
    direct_count += is_direct;
    if (is_direct) CHECK(handler, "a direct API has svc_mode::handler");
    if (handler && !is_direct)
      CHECK(id == (unsigned)object_api::YIELD || id == (unsigned)object_api::RUN_FOR,
            "the only handler APIs that are not direct are YIELD and RUN_FOR");
  }
  CHECK(direct_count == 7, "seven APIs are direct");
  CHECK(handler_count == direct_count + 2, "handler APIs = direct + YIELD + RUN_FOR");

  // thread モードの API (例: STREAM_PUSH_N / EXIT_THREAD / SLEEP_US) と範囲外は直接入らない。
  CHECK(svc_direct_of((uintptr_t)object_api::SLEEP_US) == svc_direct_kind::NONE, "SLEEP_US is not direct");
  CHECK(svc_direct_of((uintptr_t)object_api::EXIT_THREAD) == svc_direct_kind::NONE, "EXIT_THREAD is not direct");
  CHECK(svc_direct_of(0) == svc_direct_kind::NONE, "number 0 is not direct");
  CHECK(svc_direct_of(svc_attributes.size()) == svc_direct_kind::NONE, "the first out-of-range number is not direct");
  CHECK(svc_direct_of(0xFFFFFFFFu) == svc_direct_kind::NONE, "0xFFFFFFFF is not direct");

  if (failures == 0) std::printf("svc_direct_table_test: PASS\n");
  return failures == 0 ? 0 : 1;
}
