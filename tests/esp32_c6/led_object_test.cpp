// boards/esp32_c6/led_object.hpp の試験。レジスタは偽物の Mmio に書かれた内容で確かめる (実機には触れない)。
#include <cstdint>
#include <cstdio>
#include <vector>
#include "boards/esp32_c6/led_object.hpp"

namespace c6led = shizuku::boards::esp32_c6_led;
namespace lb = shizuku::led_blink;

#define CHECK(expr)                                                  \
  do {                                                               \
    if (!(expr)) {                                                   \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
      return 1;                                                      \
    }                                                                \
  } while (0)

namespace {

struct write_t {
  uint32_t address;
  uint32_t value;
};

// 書込みを記録するだけの Mmio。read は 0 を返す (初期化の read-modify-write は値の正しさを見ない)。
struct recording_mmio {
  std::vector<write_t> *log;
  void write(uint32_t address, uint32_t value) const { log->push_back({address, value}); }
  uint32_t read(uint32_t) const { return 0; }
};

// 最後に GPIO OUT (W1TS / W1TC) へ書かれた操作。
int last_out_write(const std::vector<write_t> &log, uint32_t &address) {
  for (auto it = log.rbegin(); it != log.rend(); ++it) {
    if (it->address == c6led::GPIO_OUT_W1TS || it->address == c6led::GPIO_OUT_W1TC) {
      address = it->address;
      return 1;
    }
  }
  return 0;
}

} // namespace

int main() {
  // 1. 点灯 (value = 1) は active low なので GPIO OUT の W1TC (Low) へ LED_BIT を書く。戻り値は 1。
  {
    std::vector<write_t> log;
    lb::led_request on{1};
    const uintptr_t result = c6led::write_request(recording_mmio{&log}, (uintptr_t)&on);
    uint32_t address = 0;
    CHECK(last_out_write(log, address) == 1);
    CHECK(address == c6led::GPIO_OUT_W1TC);
    CHECK(log.back().value == c6led::LED_BIT);
    CHECK(result == 1);
  }

  // 2. 消灯 (value = 0) は W1TS (High) へ LED_BIT を書く。戻り値は 0。
  {
    std::vector<write_t> log;
    lb::led_request off{0};
    const uintptr_t result = c6led::write_request(recording_mmio{&log}, (uintptr_t)&off);
    uint32_t address = 0;
    CHECK(last_out_write(log, address) == 1);
    CHECK(address == c6led::GPIO_OUT_W1TS);
    CHECK(log.back().value == c6led::LED_BIT);
    CHECK(result == 0);
  }

  // 3. 0 以外の値はすべて点灯 (論理値への正規化)。
  {
    std::vector<write_t> log;
    lb::led_request many{0xFFu};
    CHECK(c6led::write_request(recording_mmio{&log}, (uintptr_t)&many) == 1);
    uint32_t address = 0;
    CHECK(last_out_write(log, address) == 1);
    CHECK(address == c6led::GPIO_OUT_W1TC);
  }

  // 4. request が null なら何も書かず 0 を返す。
  {
    std::vector<write_t> log;
    CHECK(c6led::write_request(recording_mmio{&log}, 0) == 0);
    CHECK(log.empty());
  }

  // 5. 初期化は GPIO15 を出力に有効化し、最後に消灯 (W1TS) へ置く。
  {
    std::vector<write_t> log;
    c6led::gpio_init(recording_mmio{&log});
    bool enabled = false;
    for (const write_t &w : log)
      if (w.address == c6led::GPIO_ENABLE_W1TS && w.value == c6led::LED_BIT)
        enabled = true;
    CHECK(enabled);
    uint32_t address = 0;
    CHECK(last_out_write(log, address) == 1);
    CHECK(address == c6led::GPIO_OUT_W1TS);
  }

  std::puts("PASS: C6 LED object: active-low polarity (on=W1TC, off=W1TS), value normalization, null request, init");
  return 0;
}
