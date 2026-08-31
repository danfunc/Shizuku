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

uintptr_t gpio_main(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  uintptr_t failures = declare_name("gpio");
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
static volatile uint32_t g_led_state = 0;
static volatile uint32_t g_hw_led_state = 0xFFFFFFFFu;
// main が返す失敗コード (0 = 成功)。合成側がそのまま印字できるよう意味を持たせる。
constexpr uintptr_t LED_ARCH_INIT_FAILED = 1000;
constexpr uintptr_t LED_ABSENT = 1001;

#if defined(CYW43_WL_GPIO_LED_PIN)
void led_sync_hw() {
  const uint32_t target = g_led_state;
  if (g_hw_led_state != target) {
    g_hw_led_state = target;
    ::cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, target != 0);
  }
}
#endif

uintptr_t led_write(uintptr_t argument, uintptr_t, uintptr_t, uintptr_t) {
  const led_request *request = (const led_request *)argument;
  if (request == nullptr)
    return 0;
  g_led_state = request->value != 0 ? 1u : 0u;
#if defined(CYW43_WL_GPIO_LED_PIN)
  // Core 0 から呼ばれた場合は即時反映。Core 1 から呼ばれた場合は
  // ble_uart (Core 0) のポーリングループ (cyw43_arch_poll) が安全に反映する。
  if (BOARD::core_num() == 0) {
    led_sync_hw();
  }
#elif defined(PICO_DEFAULT_LED_PIN)
  ::gpio_put(PICO_DEFAULT_LED_PIN, g_led_state != 0);
#endif
  return g_led_state;
}

void led_toggle() {
  const uint32_t next = g_led_state ^ 1u;
  g_led_state = next;
#if defined(CYW43_WL_GPIO_LED_PIN)
  if (BOARD::core_num() == 0) {
    led_sync_hw();
  }
#elif defined(PICO_DEFAULT_LED_PIN)
  ::gpio_put(PICO_DEFAULT_LED_PIN, next != 0);
#endif
}

uintptr_t led_read(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  return g_led_state;
}

uintptr_t led_main(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  // ★自分のハードウェアは自分で立ち上げる。ここが特権を要る場所:
  //   cyw43_arch_init は PIO/DMA/IRQ を掴んでチップへファームを送り込む。
#if defined(CYW43_WL_GPIO_LED_PIN)
  // ★診断: BT を有効にしたビルドでここから戻ってこない事例を追っている
  //   (2026-08-24)。「入った」「返り値」を必ず外に出す。
  KERNEL::BOARD::diag_printf("[PERIPH] led: cyw43_arch_init() 開始\n");
  const int arch_rc = ::cyw43_arch_init();
  KERNEL::BOARD::diag_printf("[PERIPH] led: cyw43_arch_init() -> %d\n", arch_rc);
  if (arch_rc != 0)
    return LED_ARCH_INIT_FAILED; // 立ち上げ失敗をそのまま合成側へ返す
#elif defined(PICO_DEFAULT_LED_PIN)
  ::gpio_init(PICO_DEFAULT_LED_PIN);
  ::gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
#else
  return LED_ABSENT; // ボードに LED が無い
#endif
  uintptr_t failures = declare_name("led");
  failures += export_method((uintptr_t)led_method::WRITE, (uintptr_t)&led_write);
  failures += export_method((uintptr_t)led_method::READ, (uintptr_t)&led_read);
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
//   バス単位の CAS ロックで直列化する (待ちは稀なので yield backoff で十分)。
volatile uint32_t g_i2c_owner[2] = {0, 0}; // 0 = 空き。所有者は caller_thread+1
constexpr uint32_t I2C_LOCK_BACKOFF_US = 100;

struct i2c_guard {
  uint32_t instance;
  explicit i2c_guard(uint32_t inst) : instance(inst) {
    uint32_t expected = 0;
    while (!__atomic_compare_exchange_n(&g_i2c_owner[instance], &expected, 1u,
                                        /*weak=*/true, __ATOMIC_ACQUIRE,
                                        __ATOMIC_RELAXED)) {
      expected = 0;
      api(object_api::YIELD);
    }
  }
  ~i2c_guard() {
    __atomic_store_n(&g_i2c_owner[instance], 0u, __ATOMIC_RELEASE);
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
  return failures;
}

} // namespace objects
} // namespace shizuku
