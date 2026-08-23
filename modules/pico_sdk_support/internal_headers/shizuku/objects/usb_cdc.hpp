#ifndef SHIZUKU_OBJECTS_USB_CDC_HPP
#define SHIZUKU_OBJECTS_USB_CDC_HPP
#include <cstdint>

// ===========================================================================
//  USB CDC を 2 本持つ (docs/03_porting_policy.md D42)
// ===========================================================================
//  ★1 本を診断と GDB で共有していたのが D41 の不具合の根だった。GDB が繋いだ瞬間も
//    [STRESS] が流れていて、最初の応答の前に人間向けの文字が混ざる → GDB は問い直し、
//    こちらは 2 回答える → 以後ずっと 1 つずれる。**混ざりようが無い形**にする。
//      channel 0 = 診断 (printf がここへ出る)
//      channel 1 = GDB stub
//  ★pico_stdio_usb は CDC 1 本前提で、記述子も weak ではないので差し替えられない。
//    そのため USB は自前で持つ。**リセットインターフェース (picotool -f が使う) は
//    必ず残す** — 無くすと固まったときに焼き直せなくなる。
namespace shizuku {
namespace objects {

// USB を立ち上げ、診断を channel 0 へ結ぶ。ブート直後に 1 回だけ呼ぶ。
void usb_cdc_init();

// channel 1 (GDB 用) の読み書き。★-1 = 今は何も来ていない。
int usb_cdc_read(uint32_t channel);
void usb_cdc_write(uint32_t channel, char value);
void usb_cdc_flush(uint32_t channel);
bool usb_cdc_connected(uint32_t channel);

} // namespace objects
} // namespace shizuku
#endif // SHIZUKU_OBJECTS_USB_CDC_HPP
