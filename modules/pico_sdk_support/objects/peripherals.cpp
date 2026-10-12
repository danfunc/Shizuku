// ===========================================================================
//  ペリフェラルオブジェクト (GPIO / SPI) — pico_sdk_support が提供する実体
// ===========================================================================
//  ★これらは特権を要求する数少ないオブジェクト。pico-sdk の init 系は RESETS /
//    CLOCKS を触るので本質的に特権側の仕事で、ここが引き受けることで上のドライバは
//    非特権のままでいられる (DESIGN §11.2 の分業)。
//  ★各オブジェクトの main は「自分のメソッドを export する」だけ。export は発行元
//    から所有者を導出するので、**自分として走っている間**にしか登録できない
//    (名乗って他人のメソッドを生やすことはできない)。
#include "hardware/adc.h"
#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "hardware/spi.h"
#if defined(SHIZUKU_ENABLE_BOOTSEL)
#include "hardware/structs/ioqspi.h"
#include "hardware/structs/sio.h"
#include "hardware/sync.h"
#include "hardware/regs/io_qspi.h"
#include "hardware/regs/sio.h"
#include "hardware/address_mapped.h"
#endif
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

// ★自分で名乗る。誰として登録されるかは発行元から導出されるので、他人の名は騙れない。
//   名前はコピーされないため、文字列リテラル (flash 上 = 常に見える) を渡すこと。
uintptr_t declare_name(const char *name) {
  return api(object_api::DECLARE_NAME, (uintptr_t)name).error;
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

#if defined(SHIZUKU_ENABLE_BOOTSEL)
// BOOTSEL は QSPI CS pin を Low に引く。QSPI の出力を一時的に Hi-Z にして
// SIO の入力値を読む間、XIP と割込みを使えないため関数全体を SRAM に置く。
bool __no_inline_not_in_flash_func(bootsel_read_pressed)(void) {
  constexpr uint CS_PIN_INDEX = 1;
  const uint32_t interrupts = save_and_disable_interrupts();
  const uint32_t saved_ctrl = ioqspi_hw->io[CS_PIN_INDEX].ctrl;
  hw_write_masked(&ioqspi_hw->io[CS_PIN_INDEX].ctrl,
                  GPIO_OVERRIDE_LOW << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                  IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);
  for (uint32_t delay = 0; delay < 1000; ++delay) __asm volatile("nop");
#if defined(SHIZUKU_RP2040)
  constexpr uint32_t kCsBit = 1u << 1;
#else
  constexpr uint32_t kCsBit = SIO_GPIO_HI_IN_QSPI_CSN_BITS;
#endif
  const bool pressed = (sio_hw->gpio_hi_in & kCsBit) == 0;
  hw_write_masked(&ioqspi_hw->io[CS_PIN_INDEX].ctrl,
                  saved_ctrl,
                  IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);
  restore_interrupts(interrupts);
  return pressed;
}

uintptr_t bootsel_read(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  KERNEL::BOARD::park_other_cores();
  const bool pressed = bootsel_read_pressed();
  KERNEL::BOARD::resume_other_cores();
  return pressed ? 1u : 0u;
}
#endif

uintptr_t gpio_main(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  uintptr_t failures = declare_name("gpio");
  failures += export_method((uintptr_t)gpio_method::CONFIGURE,
                            (uintptr_t)&gpio_configure);
  failures += export_method((uintptr_t)gpio_method::WRITE,
                            (uintptr_t)&gpio_write);
  failures +=
      export_method((uintptr_t)gpio_method::READ, (uintptr_t)&gpio_read);
#if defined(SHIZUKU_ENABLE_BOOTSEL)
  failures += export_method((uintptr_t)gpio_method::BOOTSEL_READ_PRESSED,
                            (uintptr_t)&bootsel_read);
#endif
  return failures; // 0 = 全部登録できた
}


// ---- LED ------------------------------------------------------------------
// ★ボードによって「LED がどこにあるか」が違うので、その差をこのオブジェクトに
//   閉じ込める。呼ぶ側 (合成側やドライバ) はピン番号も配線も知らなくてよい。
//     pico2   : GPIO 25 (PICO_DEFAULT_LED_PIN)
//     pico2_w : CYW43 チップの WL_GPIO0 — **GPIO を叩いても光らない**
// 希望状態 (論理値)。書込み・toggle は owner の有無に関係なく必ずここへ記録する。
// 全アクセスは __atomic 組み込みで行い、ordering は SEQ_CST に揃える。
static uint32_t g_led_state = 0;
// owner だけが触る「チップへ最後に反映した値」。__atomic は使わない (owner 専用)。
static uint32_t g_hw_led_state = 0xFFFFFFFFu;
// main が返す失敗コード (0 = 成功)。合成側がそのまま印字できるよう意味を持たせる。
constexpr uintptr_t LED_ARCH_INIT_FAILED = 1000;
constexpr uintptr_t LED_ABSENT = 1001;
// owner 未起動の書込みの戻り値。値は記録済みだが、まだチップへ反映されていないことを示す。
constexpr uintptr_t LED_NO_OWNER = 1002;

#if defined(CYW43_WL_GPIO_LED_PIN)
// ★owner (led_owner_started を呼んだスレッド) が立つまで 0。反映の可否だけに使い、
//   書込み側は値の記録とこのフラグの読みだけで、チップには一切触れない。
uint32_t g_led_owner_up = 0;
#endif

uintptr_t led_write(uintptr_t argument, uintptr_t, uintptr_t, uintptr_t) {
  const led_request *request = (const led_request *)argument;
  if (request == nullptr)
    return 0;
  // ★希望値を記録する (最新値で上書き = coalesce)。CYW43 への反映は owner が行う。
  __atomic_store_n(&g_led_state, request->value != 0 ? 1u : 0u,
                   __ATOMIC_SEQ_CST);
#if defined(CYW43_WL_GPIO_LED_PIN)
  if (__atomic_load_n(&g_led_owner_up, __ATOMIC_ACQUIRE) == 0)
    return LED_NO_OWNER;
#elif defined(PICO_DEFAULT_LED_PIN)
  bool pin_value = __atomic_load_n(&g_led_state, __ATOMIC_SEQ_CST) != 0;
#if defined(PICO_DEFAULT_LED_PIN_INVERTED) && PICO_DEFAULT_LED_PIN_INVERTED
  pin_value = !pin_value;
#endif
  ::gpio_put(PICO_DEFAULT_LED_PIN, pin_value);
#endif
  return __atomic_load_n(&g_led_state, __ATOMIC_SEQ_CST);
}

uintptr_t led_read(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  return __atomic_load_n(&g_led_state, __ATOMIC_SEQ_CST);
}

#if defined(CYW43_WL_GPIO_LED_PIN)
// ★CYW43 の owner (BLE 無しの RP2040 Pico W でだけ起きる)。init → poll → 反映を
//   このスレッドだけで回す。SPAWN はオブジェクトの affinity (core 0) を引き継ぐ。
uintptr_t led_poll(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  KERNEL::BOARD::diag_printf("[PERIPH] led: cyw43_arch_init() 開始 (owner)\n");
  const int arch_rc = ::cyw43_arch_init();
  KERNEL::BOARD::diag_printf("[PERIPH] led: cyw43_arch_init() -> %d\n", arch_rc);
  if (arch_rc != 0)
    return LED_ARCH_INIT_FAILED; // owner 未起動のまま。書込みは LED_NO_OWNER で失敗
  led_owner_started();
  while (true) {
    ::cyw43_arch_poll();
    led_sync_hw();
  }
}
#endif

uintptr_t led_main(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  // ★CYW43 の init はここではしない。立ち上げは owner スレッド (led_poll か
  //   ble_uart の poll_loop) だけが行う。ここは口を開けるだけ。
#if defined(CYW43_WL_GPIO_LED_PIN)
  // (CYW43 の LED は GPIO を直接触らない。下の export だけ行う)
#elif defined(PICO_DEFAULT_LED_PIN)
  ::gpio_init(PICO_DEFAULT_LED_PIN);
  ::gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
#if defined(PICO_DEFAULT_LED_PIN_INVERTED) && PICO_DEFAULT_LED_PIN_INVERTED
  ::gpio_put(PICO_DEFAULT_LED_PIN, true); // Active-low LEDs are off when high.
#else
  ::gpio_put(PICO_DEFAULT_LED_PIN, false);
#endif
#else
  return LED_ABSENT; // ボードに LED が無い
#endif
  uintptr_t failures = declare_name("led");
  failures += export_method((uintptr_t)led_method::WRITE, (uintptr_t)&led_write);
  failures += export_method((uintptr_t)led_method::READ, (uintptr_t)&led_read);
#if defined(CYW43_WL_GPIO_LED_PIN)
  failures += export_method((uintptr_t)led_method::POLL, (uintptr_t)&led_poll);
#endif
  return failures;
}

// ---- 温度 (SoC 内蔵センサ) --------------------------------------------------
// ★整数だけで換算する。データシートの式は
//     T = 27 - (V - 0.706) / 0.001721   [℃]
//   これを 1/100 ℃ の整数へ直す。**浮動小数を使わない**のは、呼び出し側が
//   非特権だったり割り込み文脈だったりしても同じ費用で済ませたいため
//   (FP 文脈の退避はフレームが伸びる)。
uintptr_t temperature_read(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  const uint32_t raw = ::adc_read(); // 12bit
  // 参照電圧 3.3V を µV で持つ (raw * 3300000 / 4096)。32bit では溢れるので 64bit。
  const int64_t microvolts = ((int64_t)raw * 3300000) / 4096;
  // 1721 µV / ℃。27℃ のとき 706000 µV。
  const int32_t centi =
      2700 - (int32_t)(((microvolts - 706000) * 100) / 1721);
  return (uintptr_t)centi;
}

uintptr_t temperature_main(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  uintptr_t failures = declare_name("temperature");
  // ★ここが特権を要る場所: adc_init は RESETS を解除しクロックを設定する。
  ::adc_init();
  ::adc_set_temp_sensor_enabled(true);
  ::adc_select_input(4); // ch4 = 内蔵温度センサ
  failures += export_method((uintptr_t)temperature_method::READ,
                            (uintptr_t)&temperature_read);
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
  uintptr_t failures = declare_name("spi");
  failures += export_method((uintptr_t)spi_method::CONFIGURE,
                            (uintptr_t)&spi_configure);
  failures += export_method((uintptr_t)spi_method::TRANSFER,
                            (uintptr_t)&spi_transfer_method);
  return failures;
}

// ---- I2C ------------------------------------------------------------------
// ★実測知見 (旧実装 flight_robocon_telemetory_sender の i2c_bus): 26 バイトの
//   バースト読み (BNO055 motion block / BME280 calib) で 2ms タイムアウトは
//   足りず、100kHz 運用では 10ms 要った。既定値をここでは持たず、呼び出し側
//   (i2c_config::timeout_us) に委ねる — 用途によって必要な値が変わるため。
constexpr uint32_t I2C_DEFAULT_TIMEOUT_US = 1000;
uint32_t g_i2c_timeout_us[2] = {I2C_DEFAULT_TIMEOUT_US, I2C_DEFAULT_TIMEOUT_US};

i2c_inst_t *i2c_instance_of(uint32_t instance) {
  return instance == 0 ? i2c0 : i2c1;
}

// ★I2C は 1 本のバスに複数のデバイスがぶら下がる (BNO055/BME280 が同じ
//   I2C0 を共有する構成が実測で発覚)。CALL_METHOD は呼び出し元スレッドの
//   コンテキストでそのまま実行されるので、2 つのオブジェクトが自分の
//   スレッドから同時に CONFIGURE/WRITE_READ 等を叩くと、片方の transaction
//   (write→repeated start→read) の途中でもう片方が i2c_init や別の
//   transaction を割り込ませ、両方とも化ける (実測: BNO055 単体では動いて
//   いたのに BME280 を足した途端どちらも chip id 不一致で初期化失敗した)。
//   バス単位の ARCH CAS ロックで直列化する (待ちは稀なので yield backoff で十分)。
volatile uint32_t g_i2c_owner[2] = {0, 0}; // 0 = 空き。所有者は caller_thread+1
constexpr uint32_t I2C_LOCK_BACKOFF_US = 100;

struct i2c_guard {
  uint32_t instance;
  explicit i2c_guard(uint32_t inst) : instance(inst) {
    while (!ARCH::cas32(&g_i2c_owner[instance], 0u, 1u)) {
      api(object_api::YIELD);
    }
  }
  ~i2c_guard() {
    ARCH::store_release32(&g_i2c_owner[instance], 0u);
  }
  i2c_guard(const i2c_guard &) = delete;
  i2c_guard &operator=(const i2c_guard &) = delete;
};

uintptr_t i2c_configure(uintptr_t argument, uintptr_t, uintptr_t, uintptr_t) {
  const i2c_config *config = (const i2c_config *)argument;
  if (config == nullptr || config->instance > 1)
    return 0;
  i2c_guard lock(config->instance);
  // ★ここが特権を要る場所: i2c_init は RESETS を解除しクロックを設定する。
  const uint32_t actual =
      ::i2c_init(i2c_instance_of(config->instance), config->baudrate);
  ::gpio_set_function(config->sda_pin, GPIO_FUNC_I2C);
  ::gpio_set_function(config->scl_pin, GPIO_FUNC_I2C);
  ::gpio_pull_up(config->sda_pin);
  ::gpio_pull_up(config->scl_pin);
  g_i2c_timeout_us[config->instance] =
      config->timeout_us != 0 ? config->timeout_us : I2C_DEFAULT_TIMEOUT_US;
  return actual; // 実際に設定されたボーレート
}

uintptr_t i2c_write_method(uintptr_t argument, uintptr_t, uintptr_t, uintptr_t) {
  const i2c_transfer *t = (const i2c_transfer *)argument;
  if (t == nullptr || t->instance > 1 || t->tx == nullptr || t->tx_len == 0)
    return 0;
  i2c_guard lock(t->instance);
  const int written = ::i2c_write_timeout_us(
      i2c_instance_of(t->instance), (uint8_t)t->address, t->tx, t->tx_len,
      false, g_i2c_timeout_us[t->instance]);
  return written > 0 ? (uintptr_t)written : 0;
}

uintptr_t i2c_read_method(uintptr_t argument, uintptr_t, uintptr_t, uintptr_t) {
  const i2c_transfer *t = (const i2c_transfer *)argument;
  if (t == nullptr || t->instance > 1 || t->rx == nullptr || t->rx_len == 0)
    return 0;
  i2c_guard lock(t->instance);
  const int read = ::i2c_read_timeout_us(
      i2c_instance_of(t->instance), (uint8_t)t->address, t->rx, t->rx_len,
      false, g_i2c_timeout_us[t->instance]);
  return read > 0 ? (uintptr_t)read : 0;
}

// レジスタ読みの定型: tx (通常はレジスタアドレス 1 byte) を no-stop で書き、
// 続けて rx を読む (repeated start)。BNO055/BME280 等のレジスタ越しセンサは
// ほぼ全てこの形。
uintptr_t i2c_write_read_method(uintptr_t argument, uintptr_t, uintptr_t,
                                uintptr_t) {
  const i2c_transfer *t = (const i2c_transfer *)argument;
  if (t == nullptr || t->instance > 1 || t->tx == nullptr || t->tx_len == 0 ||
      t->rx == nullptr || t->rx_len == 0)
    return 0;
  i2c_guard lock(t->instance);
  i2c_inst_t *bus = i2c_instance_of(t->instance);
  const uint32_t timeout = g_i2c_timeout_us[t->instance];
  const int written = ::i2c_write_timeout_us(bus, (uint8_t)t->address, t->tx,
                                             t->tx_len, true /*no stop*/,
                                             timeout);
  if (written != (int)t->tx_len)
    return 0;
  const int read = ::i2c_read_timeout_us(bus, (uint8_t)t->address, t->rx,
                                         t->rx_len, false, timeout);
  return read > 0 ? (uintptr_t)read : 0;
}

uintptr_t i2c_main(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  uintptr_t failures = declare_name("i2c");
  failures += export_method((uintptr_t)i2c_method::CONFIGURE,
                            (uintptr_t)&i2c_configure);
  failures +=
      export_method((uintptr_t)i2c_method::WRITE, (uintptr_t)&i2c_write_method);
  failures +=
      export_method((uintptr_t)i2c_method::READ, (uintptr_t)&i2c_read_method);
  failures += export_method((uintptr_t)i2c_method::WRITE_READ,
                            (uintptr_t)&i2c_write_read_method);
  return failures;
}

} // namespace

#if defined(SHIZUKU_ENABLE_BOOTSEL)
bool bootsel_read(bool &pressed) {
  const call_result result = api(object_api::CALL_METHOD, GPIO_OBJECT,
                                 (uintptr_t)gpio_method::BOOTSEL_READ_PRESSED, 0);
  if (result.error != 0) return false;
  pressed = result.value != 0;
  return true;
}
#endif

// ★led_sync_hw / led_toggle は shizuku::objects の外部リンケージに置く。anonymous
//   namespace に置くと ble_uart.cpp からは weak の空定義が呼ばれ、poll 側の反映と
//   OTA の toggle が黙って消える (2026-10-09 確認)。
#if defined(CYW43_WL_GPIO_LED_PIN)
// ★owner 自身が init 成功の直後に呼ぶ。チップの既定値は不明なので、反映済みの値を
//   無効化して、記録済みの最新値 (途中の toggle を含む) を次の sync で必ず書かせる。
void led_owner_started() {
  g_hw_led_state = 0xFFFFFFFFu;
  __atomic_store_n(&g_led_owner_up, 1u, __ATOMIC_RELEASE);
}

// ★owner 自身が、チップを解体する前に呼ぶ。以後は反映しない。記録は続く (取りこぼさない)。
void led_owner_stopped() {
  __atomic_store_n(&g_led_owner_up, 0u, __ATOMIC_RELEASE);
}

// ★owner スレッド専用。CYW43 の GPIO に触れるのはここだけ。
void led_sync_hw() {
  if (__atomic_load_n(&g_led_owner_up, __ATOMIC_ACQUIRE) == 0)
    return;
  const uint32_t target = __atomic_load_n(&g_led_state, __ATOMIC_SEQ_CST);
  if (g_hw_led_state != target) {
    g_hw_led_state = target;
    ::cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, target != 0);
  }
}

uint32_t start_led_cyw43_actor() {
  const auto spawned =
      api(object_api::SPAWN, LED_OBJECT, (uintptr_t)led_method::POLL, 0);
  if (spawned.error != 0) {
    KERNEL::BOARD::diag_printf(
        "[PERIPH] led: CYW43 owner could not start (%lu)\n",
        (unsigned long)spawned.error);
    return 1;
  }
  api(object_api::SET_BUDGET, spawned.value, 0);
  return 0;
}
#endif

void led_toggle() {
  // ★parity は owner の有無に関係なく必ず記録する (XOR は原子的で、並行 toggle も落ちない)。
  //   owner 不在の間の反転は、owner の立ち上げ (led_owner_started) 後の最初の sync で
  //   最新値として反映される。解体・再初期化の窓で来た toggle も捨てない。
#if defined(CYW43_WL_GPIO_LED_PIN)
  __atomic_fetch_xor(&g_led_state, 1u, __ATOMIC_SEQ_CST); // 反映は led_sync_hw
#elif defined(PICO_DEFAULT_LED_PIN)
  __atomic_fetch_xor(&g_led_state, 1u, __ATOMIC_SEQ_CST);
  bool pin_value = __atomic_load_n(&g_led_state, __ATOMIC_SEQ_CST) != 0;
#if defined(PICO_DEFAULT_LED_PIN_INVERTED) && PICO_DEFAULT_LED_PIN_INVERTED
  pin_value = !pin_value;
#endif
  ::gpio_put(PICO_DEFAULT_LED_PIN, pin_value);
#else
  __atomic_fetch_xor(&g_led_state, 1u, __ATOMIC_SEQ_CST);
#endif
}

uint32_t register_peripherals() {
  struct entry_t {
    const char *name;
    uintptr_t object;
    uintptr_t (*main)(uintptr_t, uintptr_t, uintptr_t, uintptr_t);
  } const entries[] = {{"gpio", GPIO_OBJECT, gpio_main},
                       {"spi", SPI_OBJECT, spi_main},
                       {"i2c", I2C_OBJECT, i2c_main},
                       {"led", LED_OBJECT, led_main},
                       {"temperature", TEMPERATURE_OBJECT, temperature_main}};

  uint32_t failures = 0;
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_BOOT_STAGE_TRACE) && SHIZUKU_BOOT_STAGE_TRACE > 0
  uint32_t entry_index = 0;
#endif
  for (const entry_t &entry : entries) {
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_BOOT_STAGE_TRACE) && SHIZUKU_BOOT_STAGE_TRACE > 0
    const uint32_t stage = 61u + entry_index * 4u;
    KERNEL::BOARD::boot_trace_stage(stage); // CREATE_OBJECT 直前
#endif
    // ペリフェラルを直接叩くので特権を宣言する (上のドライバは非特権のままでよい)。
#if defined(CYW43_WL_GPIO_LED_PIN)
    // ★LED の owner (CYW43) は core 0 に固定する。SPAWN はオブジェクトの affinity を継ぐ。
    const uintptr_t flags = entry.object == LED_OBJECT
                                ? (OBJECT_PRIVILEGED | OBJECT_ON_CORE(0))
                                : OBJECT_PRIVILEGED;
#else
    const uintptr_t flags = OBJECT_PRIVILEGED;
#endif
    const call_result created = api(object_api::CREATE_OBJECT, entry.object,
                                    (uintptr_t)entry.main, flags);
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_BOOT_STAGE_TRACE) && SHIZUKU_BOOT_STAGE_TRACE > 0
    KERNEL::BOARD::boot_trace_stage(stage + 1u); // CREATE_OBJECT 復帰
    KERNEL::BOARD::boot_trace_stage(stage + 2u); // CALL_METHOD 直前
#endif
    // main を 1 回呼んで、自分のメソッドを自分で export させる。
    // main の戻り値は「export に失敗した数」なので、そこも見る。
    const call_result started =
        api(object_api::CALL_METHOD, entry.object, 0, 0);
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_BOOT_STAGE_TRACE) && SHIZUKU_BOOT_STAGE_TRACE > 0
    KERNEL::BOARD::boot_trace_stage(stage + 3u); // CALL_METHOD 復帰
    ++entry_index;
#endif
    if (created.error != 0 || started.error != 0 || started.value != 0) {
      ++failures;
      KERNEL::BOARD::diag_printf(
          "[PERIPH] %s FAILED: create=%lu call=%lu exports_failed=%lu\n",
          entry.name, (unsigned long)created.error,
          (unsigned long)started.error, (unsigned long)started.value);
    } else {
      KERNEL::BOARD::diag_printf("[PERIPH] %s ready (object %lu)\n", entry.name,
                                 (unsigned long)entry.object);
#if defined(CYW43_WL_GPIO_LED_PIN) && defined(SHIZUKU_CYW43_SCK_KHZ)
      // ★どの速度で喋っているかを毎回出す。分周比は黙って効くつまみなので、
      //   出力に残しておかないと後から「どの条件で測ったか」が分からなくなる。
      if (entry.object == LED_OBJECT)
        KERNEL::BOARD::diag_printf(
            "[PERIPH] led talks to cyw43 over gSPI at %lu kHz (div %lu)\n",
            (unsigned long)SHIZUKU_CYW43_SCK_KHZ,
            (unsigned long)CYW43_PIO_CLOCK_DIV_INT);
#endif
    }
  }
#if defined(SHIZUKU_RP2040) && defined(SHIZUKU_BOOT_STAGE_TRACE) && SHIZUKU_BOOT_STAGE_TRACE > 0
  KERNEL::BOARD::boot_trace_stage(81); // 5 オブジェクトの登録完了
#endif
  return failures;
}

uint32_t register_gpio() {
  const call_result created = api(object_api::CREATE_OBJECT, GPIO_OBJECT,
                                  (uintptr_t)&gpio_main, OBJECT_PRIVILEGED);
  const call_result started =
      api(object_api::CALL_METHOD, GPIO_OBJECT, 0, 0);
  const uint32_t failures = created.error != 0 || started.error != 0 ||
                                    started.value != 0
                                ? 1u
                                : 0u;
  if (failures != 0) {
    KERNEL::BOARD::diag_printf(
        "[PERIPH] gpio FAILED: create=%lu call=%lu exports_failed=%lu\n",
        (unsigned long)created.error, (unsigned long)started.error,
        (unsigned long)started.value);
  } else {
    KERNEL::BOARD::diag_printf("[PERIPH] gpio ready (object %lu)\n",
                               (unsigned long)GPIO_OBJECT);
  }
  return failures;
}

} // namespace objects
} // namespace shizuku
