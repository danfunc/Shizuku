#ifndef SHIZUKU_BOARDS_ESP32_C6_LED_GPIO_HPP
#define SHIZUKU_BOARDS_ESP32_C6_LED_GPIO_HPP

#include <cstdint>

// Seeed XIAO ESP32C6 のユーザー LED (GPIO15, active low: GPIO を Low で点灯) の GPIO 操作。
// レジスタ値は ESP-IDF v6.1 の soc/esp32c6 (reg_base.h, gpio_reg.h, io_mux_reg.h) に揃える。
// Mmio は write(address, value) と read(address) を持つ型。実機は direct_mmio、試験は偽物を渡す。
namespace shizuku::boards::esp32_c6_led {

constexpr uint32_t LED_GPIO = 15;
constexpr uint32_t LED_BIT = 1u << LED_GPIO;
constexpr uint32_t IO_MUX_LED_REG = 0x60090000u + 0x04u + 4u * LED_GPIO; // PERIPHS_IO_MUX_GPIO15_U
constexpr uint32_t IO_MUX_MCU_SEL_MASK = 0x7u << 12;
constexpr uint32_t IO_MUX_MCU_SEL_GPIO = 1u << 12; // PIN_FUNC_GPIO = 1
constexpr uint32_t GPIO_OUT_W1TS = 0x60091000u + 0x08u;
constexpr uint32_t GPIO_OUT_W1TC = 0x60091000u + 0x0cu;
constexpr uint32_t GPIO_ENABLE_W1TS = 0x60091000u + 0x24u;
constexpr uint32_t GPIO_FUNC_OUT_SEL_CFG = 0x60091000u + 0x554u + 4u * LED_GPIO;
constexpr uint32_t GPIO_FUNC_OUT_SEL_MASK = 0xFFu;
constexpr uint32_t SIG_GPIO_OUT_IDX = 128; // GPIO matrix の simple GPIO 出力

// 実機のレジスタへ直接書く Mmio。
struct direct_mmio {
  void write(uint32_t address, uint32_t value) const {
    *reinterpret_cast<volatile uint32_t *>(address) = value;
  }
  uint32_t read(uint32_t address) const {
    return *reinterpret_cast<volatile uint32_t *>(address);
  }
};

// on = 点灯。active low なので点灯は Low (W1TC)、消灯は High (W1TS)。
template <class Mmio> void gpio_set(const Mmio &mmio, bool on) {
  mmio.write(on ? GPIO_OUT_W1TC : GPIO_OUT_W1TS, LED_BIT);
}

// LED 用の GPIO を出力として初期化し、消灯にしておく。
template <class Mmio> void gpio_init(const Mmio &mmio) {
  mmio.write(GPIO_ENABLE_W1TS, LED_BIT);
  mmio.write(GPIO_FUNC_OUT_SEL_CFG,
             (mmio.read(GPIO_FUNC_OUT_SEL_CFG) & ~GPIO_FUNC_OUT_SEL_MASK) | SIG_GPIO_OUT_IDX);
  mmio.write(IO_MUX_LED_REG,
             (mmio.read(IO_MUX_LED_REG) & ~IO_MUX_MCU_SEL_MASK) | IO_MUX_MCU_SEL_GPIO);
  gpio_set(mmio, false);
}

} // namespace shizuku::boards::esp32_c6_led

#endif // SHIZUKU_BOARDS_ESP32_C6_LED_GPIO_HPP
