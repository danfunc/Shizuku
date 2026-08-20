// ===========================================================================
//  ペリフェラルオブジェクト (GPIO / SPI) — pico_sdk_support が提供する実体
// ===========================================================================
//  ★これらは特権を要求する数少ないオブジェクト。pico-sdk の init 系は RESETS /
//    CLOCKS を触るので本質的に特権側の仕事で、ここが引き受けることで上のドライバは
//    非特権のままでいられる (DESIGN §11.2 の分業)。
//  ★各オブジェクトの main は「自分のメソッドを export する」だけ。export は発行元
//    から所有者を導出するので、**自分として走っている間**にしか登録できない
//    (名乗って他人のメソッドを生やすことはできない)。
#include "hardware/gpio.h"
#include "hardware/spi.h"
#if defined(CYW43_WL_GPIO_LED_PIN)
#include "pico/cyw43_arch.h" // pico2_w: LED は無線チップ側の GPIO
#endif
#include "shizuku/kernel.hpp"
#include "shizuku/object_api.hpp"
#include "shizuku/objects/peripherals.hpp"

namespace shizuku {
namespace objects {
namespace {

using ARCH = KERNEL::ARCH;

struct call_result {
  uintptr_t error;
  uintptr_t value;
};

call_result api(object_api number, uintptr_t a1 = 0, uintptr_t a2 = 0,
                uintptr_t a3 = 0) {
  const auto result = ARCH::syscall((uintptr_t)number, a1, a2, a3);
  return {result.error, result.value};
}

uintptr_t export_method(uintptr_t method, uintptr_t entry) {
  return api(object_api::EXPORT_METHOD, method, entry).error;
}

spi_inst_t *instance_of(uint32_t instance) {
  return instance == 0 ? spi0 : spi1;
}

// ---- GPIO -----------------------------------------------------------------
uintptr_t gpio_configure(uintptr_t argument, uintptr_t, uintptr_t, uintptr_t) {
  const gpio_request *request = (const gpio_request *)argument;
  if (request == nullptr)
    return 0;
  ::gpio_init(request->pin);
  ::gpio_set_dir(request->pin, GPIO_OUT);
  ::gpio_put(request->pin, request->value != 0);
  return request->pin;
}

uintptr_t gpio_write(uintptr_t argument, uintptr_t, uintptr_t, uintptr_t) {
  const gpio_request *request = (const gpio_request *)argument;
  if (request == nullptr)
    return 0;
  ::gpio_put(request->pin, request->value != 0);
  return request->value;
}

uintptr_t gpio_read(uintptr_t argument, uintptr_t, uintptr_t, uintptr_t) {
  const gpio_request *request = (const gpio_request *)argument;
  if (request == nullptr)
    return 0;
  return ::gpio_get(request->pin) ? 1u : 0u;
}

uintptr_t gpio_main(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  uintptr_t failures = 0;
  failures += export_method((uintptr_t)gpio_method::CONFIGURE,
                            (uintptr_t)&gpio_configure);
  failures += export_method((uintptr_t)gpio_method::WRITE,
                            (uintptr_t)&gpio_write);
  failures +=
      export_method((uintptr_t)gpio_method::READ, (uintptr_t)&gpio_read);
  return failures; // 0 = 全部登録できた
}

// ---- LED ------------------------------------------------------------------
// ★ボードによって「LED がどこにあるか」が違うので、その差をこのオブジェクトに
//   閉じ込める。呼ぶ側 (合成側やドライバ) はピン番号も配線も知らなくてよい。
//     pico2   : GPIO 25 (PICO_DEFAULT_LED_PIN)
//     pico2_w : CYW43 チップの WL_GPIO0 — **GPIO を叩いても光らない**
uint32_t g_led_state = 0;
// main が返す失敗コード (0 = 成功)。合成側がそのまま印字できるよう意味を持たせる。
constexpr uintptr_t LED_ARCH_INIT_FAILED = 1000;
constexpr uintptr_t LED_ABSENT = 1001;

uintptr_t led_write(uintptr_t argument, uintptr_t, uintptr_t, uintptr_t) {
  const led_request *request = (const led_request *)argument;
  if (request == nullptr)
    return 0;
  g_led_state = request->value != 0 ? 1u : 0u;
#if defined(CYW43_WL_GPIO_LED_PIN)
  ::cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, g_led_state != 0);
#elif defined(PICO_DEFAULT_LED_PIN)
  ::gpio_put(PICO_DEFAULT_LED_PIN, g_led_state != 0);
#endif
  return g_led_state;
}

uintptr_t led_read(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  return g_led_state;
}

uintptr_t led_main(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  // ★自分のハードウェアは自分で立ち上げる。ここが特権を要る場所:
  //   cyw43_arch_init は PIO/DMA/IRQ を掴んでチップへファームを送り込む。
#if defined(CYW43_WL_GPIO_LED_PIN)
  if (::cyw43_arch_init() != 0)
    return LED_ARCH_INIT_FAILED; // 立ち上げ失敗をそのまま合成側へ返す
#elif defined(PICO_DEFAULT_LED_PIN)
  ::gpio_init(PICO_DEFAULT_LED_PIN);
  ::gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
#else
  return LED_ABSENT; // ボードに LED が無い
#endif
  uintptr_t failures = 0;
  failures += export_method((uintptr_t)led_method::WRITE, (uintptr_t)&led_write);
  failures += export_method((uintptr_t)led_method::READ, (uintptr_t)&led_read);
  return failures;
}

// ---- SPI ------------------------------------------------------------------
uintptr_t spi_configure(uintptr_t argument, uintptr_t, uintptr_t, uintptr_t) {
  const spi_config *config = (const spi_config *)argument;
  if (config == nullptr || config->instance > 1)
    return 0;
  // ★ここが特権を要る場所: spi_init は RESETS を解除しクロックを設定する。
  const uint32_t actual = ::spi_init(instance_of(config->instance),
                                     config->baudrate);
  ::gpio_set_function(config->sck_pin, GPIO_FUNC_SPI);
  ::gpio_set_function(config->tx_pin, GPIO_FUNC_SPI);
  ::gpio_set_function(config->rx_pin, GPIO_FUNC_SPI);
  return actual; // 実際に設定されたボーレート
}

uintptr_t spi_transfer_method(uintptr_t argument, uintptr_t, uintptr_t,
                              uintptr_t) {
  const spi_transfer *transfer = (const spi_transfer *)argument;
  if (transfer == nullptr || transfer->instance > 1 || transfer->length == 0)
    return 0;
  spi_inst_t *spi = instance_of(transfer->instance);
  if (transfer->tx != nullptr && transfer->rx != nullptr)
    return (uintptr_t)::spi_write_read_blocking(spi, transfer->tx, transfer->rx,
                                                transfer->length);
  if (transfer->tx != nullptr)
    return (uintptr_t)::spi_write_blocking(spi, transfer->tx, transfer->length);
  if (transfer->rx != nullptr)
    return (uintptr_t)::spi_read_blocking(spi, 0, transfer->rx,
                                          transfer->length);
  return 0;
}

uintptr_t spi_main(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  uintptr_t failures = 0;
  failures += export_method((uintptr_t)spi_method::CONFIGURE,
                            (uintptr_t)&spi_configure);
  failures += export_method((uintptr_t)spi_method::TRANSFER,
                            (uintptr_t)&spi_transfer_method);
  return failures;
}

} // namespace

uint32_t register_peripherals() {
  struct entry_t {
    const char *name;
    uintptr_t object;
    uintptr_t (*main)(uintptr_t, uintptr_t, uintptr_t, uintptr_t);
  } const entries[] = {{"gpio", GPIO_OBJECT, gpio_main},
                       {"spi", SPI_OBJECT, spi_main},
                       {"led", LED_OBJECT, led_main}};

  uint32_t failures = 0;
  for (const entry_t &entry : entries) {
    // ペリフェラルを直接叩くので特権を宣言する (上のドライバは非特権のままでよい)。
    const call_result created =
        api(object_api::CREATE_OBJECT, entry.object, (uintptr_t)entry.main,
            OBJECT_PRIVILEGED);
    // main を 1 回呼んで、自分のメソッドを自分で export させる。
    // main の戻り値は「export に失敗した数」なので、そこも見る。
    const call_result started =
        api(object_api::CALL_METHOD, entry.object, 0, 0);
    if (created.error != 0 || started.error != 0 || started.value != 0) {
      ++failures;
      KERNEL::BOARD::diag_printf(
          "[PERIPH] %s FAILED: create=%lu call=%lu exports_failed=%lu\n",
          entry.name, (unsigned long)created.error,
          (unsigned long)started.error, (unsigned long)started.value);
    } else {
      KERNEL::BOARD::diag_printf("[PERIPH] %s ready (object %lu)\n", entry.name,
                                 (unsigned long)entry.object);
    }
  }
  return failures;
}

} // namespace objects
} // namespace shizuku
