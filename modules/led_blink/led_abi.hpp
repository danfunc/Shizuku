#ifndef SHIZUKU_LED_BLINK_LED_ABI_HPP
#define SHIZUKU_LED_BLINK_LED_ABI_HPP

#include <cstdint>

// LED オブジェクトの口 (全ボード共通)。pico_sdk_support の objects::led_method::WRITE と
// led_request と同じ ABI。ボード側の LED オブジェクトはこの形で WRITE を受ける。
namespace shizuku::led_blink {

constexpr uintptr_t LED_WRITE_METHOD = 1; // a0 = led_request*

struct led_request {
  uint32_t value; // 0 = 消灯, それ以外 = 点灯 (論理値。極性はボード側が吸収する)
};

} // namespace shizuku::led_blink

#endif // SHIZUKU_LED_BLINK_LED_ABI_HPP
