// osi_sync.hpp のホスト単体テスト (単一スレッドで挙動を決定的に確かめる偽 PORT)。
#include "osi_sync.hpp"
#include <cstdio>

namespace {
struct fake_port {
  static inline uint64_t t = 0;
  static uint64_t now_us() { return t; }
  static void sleep_us(uint32_t us) { t += us; }
  static void yield() { t += 1; }
  static uint32_t irq_save() { return 0; }
  static void irq_restore(uint32_t) {}
  static inline uint32_t me = 1;
  static uint32_t self() { return me; }
  static bool cas32(volatile uint32_t *p, uint32_t e, uint32_t d) {
    if (*p != e) return false;
    *p = d;
    return true;
  }
};
using namespace shizuku::esp_idf_support::osi;
int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)
} // namespace

int main() {
  semaphore<fake_port> s(2, 0);
  CHECK(s.take(0) == 0);
  const uint64_t t0 = fake_port::t;
  CHECK(s.take(5) == 0);                       // 5 ms 待って諦める
  CHECK(fake_port::t - t0 >= 5000);
  CHECK(s.give() == 1 && s.give() == 1 && s.give() == 0); // max=2
  CHECK(s.take(0) == 1 && s.take(0) == 1 && s.take(0) == 0);

  mutex<fake_port> m(true), nm(false);
  CHECK(m.lock(0) == 1 && m.lock(0) == 1);     // 再帰
  fake_port::me = 2;
  CHECK(m.lock(1) == 0);                       // 他スレッドは取れない
  fake_port::me = 1;
  CHECK(m.unlock() == 1 && m.unlock() == 1 && m.unlock() == 0);
  CHECK(nm.lock(0) == 1 && nm.lock(0) == 0);   // 非再帰の二重取得は失敗

  uint8_t buf[3 * 4];
  queue<fake_port> q(buf, 3, 4);
  uint32_t v = 1, out = 0;
  CHECK(q.send(&v, 0) == 1);
  v = 2; CHECK(q.send(&v, 0) == 1);
  v = 0; CHECK(q.send(&v, 0, true) == 1);      // 先頭へ
  v = 9; CHECK(q.send(&v, 0) == 0);            // 満杯
  CHECK(q.waiting() == 3);
  CHECK(q.recv(&out, 0) == 1 && out == 0);
  CHECK(q.recv(&out, 0) == 1 && out == 1);
  CHECK(q.recv(&out, 0) == 1 && out == 2);
  CHECK(q.recv(&out, 1) == 0);

  event_group<fake_port> e;
  e.set(0x5);
  CHECK(e.wait(0x1, true, true, 0) & 1);
  CHECK(e.bits == 0x4);                        // clear_on_exit
  CHECK((e.wait(0x6, false, true, 1) & 0x6) != 0x6); // all 待ち: 揃わず timeout
  CHECK((e.wait(0x6, false, false, 0) & 0x4) == 0x4);
  CHECK(e.clear(0x4) == 0x4 && e.bits == 0);

  std::printf(failures ? "FAILED\n" : "OK\n");
  return failures;
}
