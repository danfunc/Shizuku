#ifndef SHIZUKU_OBJECTS_PERIPHERALS_HPP
#define SHIZUKU_OBJECTS_PERIPHERALS_HPP
#include <cstdint>
#include "shizuku/object_ids.hpp"

// ===========================================================================
//  ペリフェラルオブジェクト — ボードが提供する、ハードウェアを持つオブジェクト
// ===========================================================================
//  ★ここに置く理由: `spi_init` のような操作は RESETS / CLOCKS を触るので本質的に
//    特権側の仕事で、しかも SoC 固有。**ペリフェラルオブジェクトが特権を引き受ける
//    ことで、その上のドライバ (XNO 側) を非特権のままにできる** —
//    DESIGN §11.2 の「ドライバは非特権で走れる。IO を触るから特権ではない」を
//    成立させる分担そのもの。
//  ★これらはカーネルでもカーネルオブジェクトでもない。ただのオブジェクトなので、
//    呼ばれ方も戻り方も他と同じ (メソッド呼び出し + exit)。
//
//  メソッドの引数は 1 語なので、複数の値は要求構造体のアドレスで渡す
//  (単一アドレス空間の利点。将来オブジェクト境界を跨ぐときは要 bounds-check)。
namespace shizuku {
namespace objects {

// オブジェクト ID (合成側が決める)。**埋まっている番号の表は
// shizuku/objects/flash_fs.hpp に集約してある** — 散らすと衝突に気づけない。
const uintptr_t GPIO_OBJECT = object_id::gpio;
const uintptr_t SPI_OBJECT = object_id::spi;
const uintptr_t I2C_OBJECT = object_id::i2c;
// ボード上の LED。**どのピンに繋がっているか (あるいは GPIO ですらないか) を
// 隠す**のがこのオブジェクトの仕事: pico2 は GPIO 25、pico2_w は CYW43 チップの
// WL_GPIO0 で、後者は GPIO を叩いても光らない。呼ぶ側はどちらか知らなくてよい。
const uintptr_t LED_OBJECT = object_id::led;
// SoC 内蔵の温度センサ (ADC ch4)。★ADC の立ち上げは RESETS を触るので特権側の仕事。
const uintptr_t TEMPERATURE_OBJECT = object_id::temperature;

// メソッド番号。0 は main (生成側が据え、自分で残りを export する)。
enum struct gpio_method : uintptr_t {
  MAIN = 0,
  CONFIGURE = 1, // a0 = gpio_request*  (出力として初期化)
  WRITE = 2,     // a0 = gpio_request*  (value を出力)
  READ = 3,      // a0 = gpio_request*  (戻り値 = ピンの値)
  BOOTSEL_READ_PRESSED = 4, // QSPI CS を安全な特権経路で読み、押下中なら 1
};
enum struct spi_method : uintptr_t {
  MAIN = 0,
  CONFIGURE = 1, // a0 = spi_config*
  TRANSFER = 2,  // a0 = spi_transfer* (戻り値 = 転送したバイト数)
};
enum struct i2c_method : uintptr_t {
  MAIN = 0,
  CONFIGURE = 1,  // a0 = i2c_config*
  WRITE = 2,      // a0 = i2c_transfer* (rx は無視。戻り値 = 書けたバイト数、失敗時 0)
  READ = 3,       // a0 = i2c_transfer* (tx は無視。戻り値 = 読めたバイト数、失敗時 0)
  // レジスタ読みの定型 (tx = レジスタアドレス列を no-stop で書き、続けて rx を
  // 読む)。BNO055 等の burst read はこれを使う。
  WRITE_READ = 4, // a0 = i2c_transfer* (tx/tx_len と rx/rx_len の両方を使う)
};

enum struct led_method : uintptr_t {
  MAIN = 0,
  WRITE = 1, // a0 = led_request*
  READ = 2,  // 戻り値 = 最後に書いた値
  POLL = 3,  // CYW43 の owner スレッド (pico2_w の BLE 無し構成のみ起こす)
};

struct led_request {
  uint32_t value;
};

enum struct temperature_method : uintptr_t {
  MAIN = 0,
  READ = 1, // 戻り値 = 摂氏の 1/100 (符号付き。25.31℃ なら 2531)
};

struct gpio_request {
  uint32_t pin;
  uint32_t value;
};

struct spi_config {
  uint32_t instance;  // 0 = spi0, 1 = spi1
  uint32_t baudrate;  // [Hz]
  uint32_t sck_pin;
  uint32_t tx_pin;
  uint32_t rx_pin;
};

struct spi_transfer {
  uint32_t instance;
  const uint8_t *tx; // nullptr なら 0 を送る
  uint8_t *rx;       // nullptr なら読み捨てる
  uint32_t length;
};

struct i2c_config {
  uint32_t instance;   // 0 = i2c0, 1 = i2c1
  uint32_t baudrate;   // [Hz]
  uint32_t sda_pin;
  uint32_t scl_pin;
  // ★I2C はバス busy/NACK で本当に固まることがある実測知見 (BNO055/BME280 の
  //   26 バイトバースト読みで 2ms は足りず 10ms 要った)。呼び出し側が用途に
  //   応じて指定すること。0 なら 1000us を既定値として使う。
  uint32_t timeout_us;
};

struct i2c_transfer {
  uint32_t instance;
  uint32_t address;    // 7bit スレーブアドレス
  const uint8_t *tx;
  uint32_t tx_len;
  uint8_t *rx;
  uint32_t rx_len;
};

// 合成側 (ブート後のスレッドモード) から呼ぶ。オブジェクトを生成し、各自の main を
// 一度呼んでメソッドを export させる。戻り = 失敗した手数 (0 なら全部成功)。
// ★戻り値を捨てないこと。ここが黙って失敗すると「呼んでいるのに何も起きない」に
//   なり、原因の切り分けが一気に難しくなる (D12)。
uint32_t register_peripherals();

// GPIOだけを使う組込みで、他のperipheral objectを起動せずGPIOを登録する。
uint32_t register_gpio();

// Pico 2 W など CYW43 経由の LED において、CYW43 の owner スレッドだけが呼ぶ
// (ポーリングループから状態を反映する)。他のスレッドから呼んではいけない。
void led_sync_hw();

// どのコア・スレッドからでも安全に呼べる LED トグル関数 (Pico 2 / Pico 2 W 両対応)。
// 状態 (parity) は owner の有無に関係なく記録する。CYW43 への反映は owner が行う。
void led_toggle();

// feature enabled の製品だけに使う。未有効時はGPIO method 4をexportしない。
#if defined(SHIZUKU_ENABLE_BOOTSEL)
bool bootsel_read(bool &pressed);
#endif


#if defined(CYW43_WL_GPIO_LED_PIN)
// CYW43 の owner が init に成功した時点で owner 自身が呼ぶ。以後 LED 書込みを受理する。
void led_owner_started();
// owner 自身が、チップを解体する前に呼ぶ。以後の書込みは LED_NO_OWNER になる。
void led_owner_stopped();

// BLE の無い CYW43 構成 (RP2040 Pico W) で LED の owner スレッドを起こす。
uint32_t start_led_cyw43_actor();
#endif

} // namespace objects
} // namespace shizuku
#endif // SHIZUKU_OBJECTS_PERIPHERALS_HPP
