#include "hello.hpp"
#include "libshizuku.hpp"

using namespace shizuku::objectland;

extern "C" {

static uintptr_t hello_ping(uintptr_t val, uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  return val + 42;
}

static uintptr_t hello_push(uintptr_t desc_ptr, uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  if (desc_ptr == 0) {
    return 1;
  }
  auto *desc = reinterpret_cast<shizuku_stream_descriptor *>(desc_ptr);
  const uint32_t val = 0x48454c4fu; // "HELO"
  if (!stream_push(desc, val)) {
    return 2;
  }
  return 0;
}

uintptr_t hello_main(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  // SVC で自身の名前を名乗る
  syscall(SHIZUKU_SVC_DECLARE_NAME, (uintptr_t)"hello");

  // メソッドを export する
  syscall(SHIZUKU_SVC_EXPORT_METHOD, (uintptr_t)HELLO_METHOD_PING, (uintptr_t)&hello_ping);
  syscall(SHIZUKU_SVC_EXPORT_METHOD, (uintptr_t)HELLO_METHOD_PUSH, (uintptr_t)&hello_push);

  return 0;
}

void hello_entry(void) {
  hello_main(0, 0, 0, 0);
  while (true) {
    syscall(SHIZUKU_SVC_YIELD);
  }
}

}
