// ===========================================================================
//  ESP-IDF Wi-Fi / BLE blob 用 OSI 同期部品 (試作 v0: 設計 §4 の「部品層」だけ)
// ===========================================================================
//  ★FreeRTOS 互換層ではない。blob が osi 表 (wifi_osi_funcs_t / BLE ext_funcs_t) 越しに
//    要求する「セマフォ / ミューテックス / キュー / イベントグループ」だけを、
//    Shizuku のスレッド (YIELD / SLEEP_US) と CAS で満たす。
//  ★Shizuku には「ブロックして起こされる」プリミティブが無い (object_api は SPAWN / YIELD /
//    SLEEP_US / RUN_FOR のみ)。したがって待ちは **試行 → 譲る → 短く眠る** の繰返しで作る。
//    起床遅延は最大 SLEEP_MAX_US。kernel に park/unpark が入れば wait_step だけを差し替える。
//  ★PORT 概念 (テストでは偽物を、実機では object_api syscall を包んだものを渡す):
//      uint64_t now_us();  void sleep_us(uint32_t);  void yield();
//      uint32_t irq_save();  void irq_restore(uint32_t);
//      uint32_t self();                 // 呼び元スレッド ID (非 0)
//      bool cas32(volatile uint32_t*, uint32_t expected, uint32_t desired);
//  ★tick = 1 ms と決める (_task_ms_to_tick が恒等)。0xFFFFFFFF = 永遠 (portMAX_DELAY)。
#ifndef SHIZUKU_ESP_IDF_SUPPORT_OSI_SYNC_HPP
#define SHIZUKU_ESP_IDF_SUPPORT_OSI_SYNC_HPP
#include <cstdint>
#include <cstring>

namespace shizuku::esp_idf_support::osi {

constexpr uint32_t WAIT_FOREVER = 0xFFFFFFFFu;
constexpr uint32_t SPIN_BEFORE_SLEEP = 16; // この回数までは yield だけで回る (他が走れるなら譲る)
constexpr uint32_t SLEEP_MAX_US = 200;     // 眠りの上限 = 起床遅延の上限

// 待ちの 1 歩。timeout を過ぎていたら false (呼び元は諦める)。
template <class PORT> struct waiter {
  uint64_t deadline;
  bool forever;
  uint32_t spins = 0;
  explicit waiter(uint32_t timeout_ms)
      : deadline(PORT::now_us() + uint64_t(timeout_ms) * 1000u), forever(timeout_ms == WAIT_FOREVER) {}
  bool step() {
    if (!forever && PORT::now_us() >= deadline) return false;
    if (spins < SPIN_BEFORE_SLEEP) { ++spins; PORT::yield(); return true; }
    PORT::sleep_us(SLEEP_MAX_US);
    return true;
  }
};

// 短い臨界区間 (キュー操作だけ) 用。IRQ を止めて CAS ロック。★中でブロックしないこと。
template <class PORT> struct spin_guard {
  volatile uint32_t &lock;
  uint32_t irq;
  explicit spin_guard(volatile uint32_t &l) : lock(l), irq(PORT::irq_save()) {
    while (!PORT::cas32(&lock, 0, 1)) {}
  }
  ~spin_guard() { lock = 0; PORT::irq_restore(irq); }
};

template <class PORT> struct semaphore {
  volatile uint32_t count;
  uint32_t max;
  semaphore(uint32_t max_count, uint32_t initial) : count(initial), max(max_count) {}
  bool try_take() {
    uint32_t c = count;
    while (c > 0) {
      if (PORT::cas32(&count, c, c - 1)) return true;
      c = count;
    }
    return false;
  }
  int32_t take(uint32_t timeout_ms) {
    waiter<PORT> w(timeout_ms);
    do { if (try_take()) return 1; } while (w.step());
    return try_take() ? 1 : 0;
  }
  int32_t give() {
    for (;;) {
      const uint32_t c = count;
      if (c >= max) return 0;
      if (PORT::cas32(&count, c, c + 1)) return 1;
    }
  }
};

// 再帰可能ミューテックス (osi の mutex_create / recursive_mutex_create 共通)。優先度継承はしない。
template <class PORT> struct mutex {
  volatile uint32_t owner = 0;
  uint32_t depth = 0;
  bool recursive;
  explicit mutex(bool rec) : recursive(rec) {}
  int32_t lock(uint32_t timeout_ms) {
    const uint32_t me = PORT::self();
    if (owner == me) { if (!recursive) return 0; ++depth; return 1; }
    waiter<PORT> w(timeout_ms);
    do {
      if (PORT::cas32(&owner, 0, me)) { depth = 1; return 1; }
    } while (w.step());
    if (PORT::cas32(&owner, 0, me)) { depth = 1; return 1; }
    return 0;
  }
  int32_t unlock() {
    if (owner != PORT::self()) return 0;
    if (--depth == 0) owner = 0;
    return 1;
  }
};

// 固定長リングキュー。item は memcpy でコピー (FreeRTOS と同じ値渡し)。
template <class PORT> struct queue {
  volatile uint32_t lock = 0;
  uint8_t *storage;
  uint32_t item_size, capacity, head = 0, used = 0;
  queue(uint8_t *buf, uint32_t len, uint32_t size) : storage(buf), item_size(size), capacity(len) {}
  bool try_push(const void *item, bool front) {
    spin_guard<PORT> g(lock);
    if (used == capacity) return false;
    const uint32_t slot = front ? (head + capacity - 1) % capacity : (head + used) % capacity;
    std::memcpy(storage + slot * item_size, item, item_size);
    if (front) head = slot;
    ++used;
    return true;
  }
  bool try_pop(void *item) {
    spin_guard<PORT> g(lock);
    if (used == 0) return false;
    std::memcpy(item, storage + head * item_size, item_size);
    head = (head + 1) % capacity;
    --used;
    return true;
  }
  int32_t send(const void *item, uint32_t timeout_ms, bool front = false) {
    waiter<PORT> w(timeout_ms);
    do { if (try_push(item, front)) return 1; } while (w.step());
    return try_push(item, front) ? 1 : 0;
  }
  int32_t recv(void *item, uint32_t timeout_ms) {
    waiter<PORT> w(timeout_ms);
    do { if (try_pop(item)) return 1; } while (w.step());
    return try_pop(item) ? 1 : 0;
  }
  uint32_t waiting() const { return used; }
};

template <class PORT> struct event_group {
  volatile uint32_t bits = 0;
  uint32_t set(uint32_t mask) {
    for (;;) { const uint32_t b = bits; if (PORT::cas32(&bits, b, b | mask)) return b | mask; }
  }
  uint32_t clear(uint32_t mask) { // 戻りは clear 前の値 (FreeRTOS 互換)
    for (;;) { const uint32_t b = bits; if (PORT::cas32(&bits, b, b & ~mask)) return b; }
  }
  uint32_t wait(uint32_t mask, bool clear_on_exit, bool all, uint32_t timeout_ms) {
    waiter<PORT> w(timeout_ms);
    for (;;) {
      const uint32_t b = bits;
      const bool met = all ? (b & mask) == mask : (b & mask) != 0;
      if (met) {
        if (clear_on_exit && !PORT::cas32(&bits, b, b & ~mask)) continue;
        return b;
      }
      if (!w.step()) return bits;
    }
  }
};

} // namespace shizuku::esp_idf_support::osi
#endif
