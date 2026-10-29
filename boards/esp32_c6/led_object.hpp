#ifndef SHIZUKU_BOARDS_ESP32_C6_LED_OBJECT_HPP
#define SHIZUKU_BOARDS_ESP32_C6_LED_OBJECT_HPP

#include <cstdint>
#include "boards/esp32_c6/led_gpio.hpp"
#include "modules/led_blink/led_abi.hpp"

// C6 のユーザー LED オブジェクト。特権 (M-mode) で動き、led_blink と同じ WRITE の口を持つ。
// 極性 (active low) はここで吸収する: 呼び手は論理値 (0 = 消灯, それ以外 = 点灯) だけを渡す。
namespace shizuku::boards::esp32_c6_led {

// WRITE の本体。a0 = led_request*。戻り値は書いた論理値 (1 = 点灯, 0 = 消灯)。
// request が null なら何もせず 0 (peripherals の led_write と同じ流儀)。
template <class Mmio> uintptr_t write_request(const Mmio &mmio, uintptr_t argument) {
  const auto *request = (const led_blink::led_request *)argument;
  if (request == nullptr)
    return 0;
  const bool on = request->value != 0;
  gpio_set(mmio, on);
  return on ? 1 : 0;
}

// オブジェクトのメソッドとして登録される形 (実機レジスタへ書く)。
inline uintptr_t led_write(uintptr_t argument, uintptr_t, uintptr_t, uintptr_t) {
  return write_request(direct_mmio{}, argument);
}

// 起動時に一度だけ呼ぶ (GPIO15 を出力にし、消灯にする)。
inline void led_init() { gpio_init(direct_mmio{}); }

} // namespace shizuku::boards::esp32_c6_led

#endif // SHIZUKU_BOARDS_ESP32_C6_LED_OBJECT_HPP
