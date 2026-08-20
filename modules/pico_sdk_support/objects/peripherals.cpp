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

void register_peripherals() {
  struct entry_t {
    uintptr_t object;
    uintptr_t (*main)(uintptr_t, uintptr_t, uintptr_t, uintptr_t);
  } const entries[] = {{GPIO_OBJECT, gpio_main}, {SPI_OBJECT, spi_main}};

  for (const entry_t &entry : entries) {
    // ペリフェラルを直接叩くので特権を宣言する (上のドライバは非特権のままでよい)。
    api(object_api::CREATE_OBJECT, entry.object, (uintptr_t)entry.main,
        OBJECT_PRIVILEGED);
    // main を 1 回呼んで、自分のメソッドを自分で export させる。
    api(object_api::CALL_METHOD, entry.object, 0, 0);
  }
}

} // namespace objects
} // namespace shizuku
