#ifndef SHIZUKU_BOARDS_RP2350_PICO2_HPP
#define SHIZUKU_BOARDS_RP2350_PICO2_HPP
#include <cstdint>
#include "pico/stdlib.h"
#include "shizuku/concepts/board.hpp"

namespace shizuku {
namespace boards {

// Raspberry Pi Pico 2 (RP2350, pico-sdk) の board バックエンド。実装は board.cpp。
class rp2350_pico2 {
public:
  static constexpr uint32_t CORE_COUNT = 2;
  // 呼び出したコアの例外設定を行う。ベクタテーブル (RAM) は両コア共有なので
  // ハンドラ登録は core0 の 1 回だけ、優先度 (SHPR, banked) は各コアで設定する。
  // ★優先度規約: SVC 最優先 > (タイマ) > PendSV 最低 (concepts/arch.hpp 参照)。
  static void init(uint32_t core);
  // このコアのメモリ保護を張る (region は per-core banked なので各コアで呼ぶ)。
  static void protection_init();
  // ★もう一方のコアを起こす。起こされた側は自分で BOARD::init(core) を呼んでから
  //   スレッドモードへ移る (優先度・MPU・SysTick は per-core banked なので、
  //   起こす側から設定してやることはできない)。
  static void launch_core(void (*entry)());
  // ★flash を消す/焼く間だけ、もう一方のコアを**RAM 上のコードで止める**。
  //   消去中は XIP そのものが止まる = flash 上のコードを誰も実行できないので、
  //   相手が flash を踏んだ瞬間に死ぬ。止めるのは「行儀」ではなく機械の要求。
  //   ★止まっている間、相手のコアは何も進まない。**書き込みは両方のコアを
  //     止める操作**であって、周期スレッドの隣で気軽に呼ぶものではない。
  static void park_other_cores();
  static void resume_other_cores();
  static uint32_t core_num() { return (uint32_t)::get_core_num(); }
  static uint64_t time_us() { return ::time_us_64(); }
  // ★クロックを知っているのは board (PORT §2.3)。上位は µs でしか話さない。
  static uint32_t cycles_per_us();
  // ★非特権オブジェクトから届く最下位アドレス (= ヒープ先頭)。これより下 (静的領域)
  //   は region の外なので特権のみ。カーネルの簿記をどこに置いてよいかの境界。
  static uintptr_t unprivileged_floor();
  // 診断出力。現状は pico-sdk の printf 直行 (USB CDC)。
  // TODO(PORT §7): 「固まっても出る経路」(リング + タイマからの同期排出) へ置き換える。
  static void diag_printf(const char *format, ...) __attribute__((format(printf, 1, 2)));
  [[noreturn]] static void panic(const char *message);
};

static_assert(shizuku::concepts::board_requires<rp2350_pico2>,
              "rp2350_pico2 does not satisfy the board concept");

} // namespace boards
} // namespace shizuku
#endif // SHIZUKU_BOARDS_RP2350_PICO2_HPP
