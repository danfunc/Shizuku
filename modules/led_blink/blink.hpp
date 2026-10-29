#ifndef SHIZUKU_LED_BLINK_BLINK_HPP
#define SHIZUKU_LED_BLINK_BLINK_HPP

#include <cstdint>
#include "shizuku/object_api.hpp"
#include "modules/led_blink/blink_logic.hpp"
#include "modules/led_blink/led_abi.hpp"

// 全ボード共通の点滅。特権で走り、周期は syscall SLEEP_US で作る (チップ割込みには触れない)。
// LED への書込みは親 (ROOT_OBJECT) の中継メソッドを CALL_METHOD で叩く (親子呼出しのみ)。
// この中のどこにもチップ・ボード固有の名前は無い。XNO へ移すときはこのパッケージごと移す。
//
// Env は呼び出し側 (ボード) が与える口。ARCH / BOARD をここへ束ねる:
//   Env::syscall(uintptr_t number, uintptr_t a1, uintptr_t a2, uintptr_t a3) -> { value, error }
//   Env::now_us() -> uint64_t
//   Env::running() -> bool             (false で抜ける。実機は常に true)
//   Env::on_period(uint64_t late_us)   (周期ごとの遅れの観測口。実機では何もしなくてよい)
namespace shizuku::led_blink {

// 点滅スレッド。Parent = 中継メソッドを持つ親 (ROOT_OBJECT)、Relay = その中継メソッド番号。
// 締切は絶対グリッドで積み、SLEEP_US は残り時間だけ寝る (相対で寝ると遅れが積み上がる)。
template <class Env, uintptr_t Parent, uintptr_t Relay, uint64_t PeriodUs>
uintptr_t blink_entry(uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
  led_request request{0};
  schedule_t schedule{Env::now_us() + PeriodUs, false};
  while (Env::running()) {
    const uint64_t remaining = wait_us(schedule, Env::now_us());
    if (remaining > 0) {
      Env::syscall((uintptr_t)object_api::SLEEP_US, (uintptr_t)remaining, 0, 0);
      continue;
    }
    Env::on_period(late_us(schedule, Env::now_us()));
    advance(schedule, PeriodUs);
    request.value = schedule.on ? 1u : 0u;
    Env::syscall((uintptr_t)object_api::CALL_METHOD, Parent, Relay, (uintptr_t)&request);
  }
  return 0;
}

// 親 (ROOT_OBJECT) が export する中継メソッド。a0 = led_request*。
// LED オブジェクトの WRITE を親子呼出しで叩き、その error を返す。
template <class Env, uintptr_t Led>
uintptr_t led_relay_entry(uintptr_t request_ptr, uintptr_t, uintptr_t, uintptr_t) {
  return Env::syscall((uintptr_t)object_api::CALL_METHOD, Led, LED_WRITE_METHOD,
                      request_ptr)
      .error;
}

} // namespace shizuku::led_blink

#endif // SHIZUKU_LED_BLINK_BLINK_HPP
