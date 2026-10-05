// ===========================================================================
//  Hello Objectland 検証プローブ (自己テスト)
// ===========================================================================
#include "objectland/hello.hpp"
#include "shizuku/kernel.hpp"
#include "shizuku/object_api.hpp"
#include "shizuku/object_ids.hpp"
#include "shizuku/selftest.hpp"
#include "shizuku/stream.hpp"

namespace shizuku {
namespace selftest {
namespace {

using ARCH = KERNEL::ARCH;
using BOARD = KERNEL::BOARD;

constexpr uintptr_t OBJECT_HELLO = object_id::hello;

void check(const char *name, bool ok, unsigned long got, unsigned long want) {
  if (ok) {
    ++passed;
    BOARD::diag_printf("[SELFTEST] PASS %s (=%lu)\n", name, got);
  } else {
    ++failed;
    BOARD::diag_printf("[SELFTEST] FAIL %s: got %lu want %lu\n", name, got, want);
  }
}

struct api_result {
  uintptr_t error;
  uintptr_t value;
};

api_result api(object_api number, uintptr_t a1 = 0, uintptr_t a2 = 0,
               uintptr_t a3 = 0) {
  const auto result = ARCH::syscall((uintptr_t)number, a1, a2, a3);
  return {result.error, result.value};
}

} // namespace

void hello_probe() {
  BOARD::diag_printf("[SELFTEST] hello probe start\n");

  // 1. 本番登録経路 (CREATE_OBJECT) で hello オブジェクトを登録
  const auto created = api(object_api::CREATE_OBJECT, OBJECT_HELLO,
                           (uintptr_t)&hello_main, 0);
  check("hello: created", created.error == 0, (unsigned long)created.error, 0);

  // 2. hello_main を呼び出し、メソッドを export させる
  const auto started = api(object_api::CALL_METHOD, OBJECT_HELLO, 0, 0);
  check("hello: main called", started.error == 0 && started.value == 0,
        (unsigned long)started.error, 0);

  // 3. PING メソッドの動作確認 (引数 100 + 42 = 142)
  const auto ping = api(object_api::CALL_METHOD, OBJECT_HELLO,
                        (uintptr_t)hello_method::HELLO_METHOD_PING, 100);
  check("hello: ping 100+42", ping.error == 0 && ping.value == 142,
        (unsigned long)ping.value, 142);

  // 4. ストリームを作成し、hello_push メソッドで 1 item push させる
  stream::storage<uint32_t, 4> test_stream;
  test_stream.init(stream::LOSSLESS);

  const auto push_res = api(object_api::CALL_METHOD, OBJECT_HELLO,
                            (uintptr_t)hello_method::HELLO_METHOD_PUSH,
                            (uintptr_t)&test_stream.desc);
  check("hello: push stream item", push_res.error == 0 && push_res.value == 0,
        (unsigned long)push_res.value, 0);

  // 5. ストリームから pop して 1 item (0x48454c4f) が到着していることを検証
  uint32_t received = 0;
  auto hdl = test_stream.hdl();
  const bool popped = hdl.pop(&received);
  check("hello: received item from stream", popped && received == 0x48454c4f,
        (unsigned long)received, 0x48454c4f);

  BOARD::diag_printf("[SELFTEST] hello probe done\n");
}

} // namespace selftest
} // namespace shizuku
