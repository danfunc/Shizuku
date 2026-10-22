#include "compat.hpp"
#include <array>
#include <cassert>

using namespace shizuku::freertos_compat;

// No waits or scheduling: exercise the production queue allocation/copy path.
struct queue_port final : port {
  uint64_t now_ticks() override { return 0; }
  uint64_t now_us() override { return 0; }
  uint32_t current_task() override { return 1; }
  bool in_isr() override { return false; }
  void yield() override { assert(false); }
  void park_until(uint64_t) override { assert(false); }
  bool wake(uint32_t) override { assert(false); return false; }
  bool spawn(void (*)(void *), void *, uint32_t, uint32_t, uint32_t *) override {
    assert(false); return false;
  }
  uint32_t irq_save() override { return 0; }
  void irq_restore(uint32_t) override {}
};

int main() {
  queue_port p;
  domain d(p);
  using packet = std::array<uint8_t, 518>; // OTA rx_packet's asserted wire buffer size
  const int q = d.create_queue(2, sizeof(packet));
#if defined(SHIZUKU_FREERTOS_COMPAT_TINY) && defined(SHIZUKU_ESP32_C6)
  static_assert(kQueueStorageBytes == 1036 && kMaxObjects == 24);
  assert(q >= 0); // Original failure: the two-packet queue must now fit.
  packet a{}, b{}, c{}, out{};
  a.fill(0x11); b.fill(0x22); c.fill(0x33);
  assert(d.queue_send(q, &a, 0) && d.queue_send(q, &b, 0));
  assert(!d.queue_send(q, &c, 0)); // Bounded capacity, no overwrite when full.
  assert(d.queue_receive(q, &out, 0) && out == a);
  assert(d.queue_send(q, &c, 0)); // Wrap the ring, copying all 518 bytes.
  assert(d.queue_receive(q, &out, 0) && out == b);
  assert(d.queue_receive(q, &out, 0) && out == c);
  assert(!d.queue_receive(q, &out, 0));
  assert(d.create_queue(3, sizeof(packet)) < 0);
#else
  static_assert(kQueueStorageBytes == 1024);
  assert(q < 0); // C5 TINY and non-TINY retain their existing bounds.
  assert(d.create_queue(1, sizeof(packet)) >= 0);
#endif
  assert(d.create_queue(1, kQueueStorageBytes + 1) < 0);
}
