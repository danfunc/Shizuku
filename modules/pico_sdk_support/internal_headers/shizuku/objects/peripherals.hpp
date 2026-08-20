#ifndef SHIZUKU_OBJECTS_PERIPHERALS_HPP
#define SHIZUKU_OBJECTS_PERIPHERALS_HPP
#include <cstdint>

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

// オブジェクト ID (合成側が決める。自己テスト用の 1..3 と衝突させない)。
constexpr uintptr_t GPIO_OBJECT = 8;
constexpr uintptr_t SPI_OBJECT = 9;
// ボード上の LED。**どのピンに繋がっているか (あるいは GPIO ですらないか) を
// 隠す**のがこのオブジェクトの仕事: pico2 は GPIO 25、pico2_w は CYW43 チップの
// WL_GPIO0 で、後者は GPIO を叩いても光らない。呼ぶ側はどちらか知らなくてよい。
constexpr uintptr_t LED_OBJECT = 10;

// メソッド番号。0 は main (生成側が据え、自分で残りを export する)。
enum struct gpio_method : uintptr_t {
  MAIN = 0,
  CONFIGURE = 1, // a0 = gpio_request*  (出力として初期化)
  WRITE = 2,     // a0 = gpio_request*  (value を出力)
  READ = 3,      // a0 = gpio_request*  (戻り値 = ピンの値)
};
enum struct spi_method : uintptr_t {
  MAIN = 0,
  CONFIGURE = 1, // a0 = spi_config*
  TRANSFER = 2,  // a0 = spi_transfer* (戻り値 = 転送したバイト数)
};

enum struct led_method : uintptr_t {
  MAIN = 0,
  WRITE = 1, // a0 = led_request*
  READ = 2,  // 戻り値 = 最後に書いた値
};

struct led_request {
  uint32_t value;
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

// 合成側 (ブート後のスレッドモード) から呼ぶ。オブジェクトを生成し、各自の main を
// 一度呼んでメソッドを export させる。戻り = 失敗した手数 (0 なら全部成功)。
// ★戻り値を捨てないこと。ここが黙って失敗すると「呼んでいるのに何も起きない」に
//   なり、原因の切り分けが一気に難しくなる (D12)。
uint32_t register_peripherals();

} // namespace objects
} // namespace shizuku
#endif // SHIZUKU_OBJECTS_PERIPHERALS_HPP
