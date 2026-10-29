// サンプル: 診断を #ifdef ではなく型で選ぶ。
//
// 今のコードは SHIZUKU_BOOT_STAGE_TRACE / SELFTEST_PROGRESS / CRASH_RECORD / BOOT_LOG_WAIT
// を各所の #if で囲んでいる (dispatch.cpp だけで十数か所)。代わりに、構成が DIAG 型を
// 1 つ選び、コードは DIAG::stage(...) と書くだけにする。無効の型は空の inline なので
// 最適化で消え、hot path の命令は増えない。有効の型も全構成でコンパイルされるので、
// 「普段のビルドでは壊れているのに気付かない」が起きない。
#ifndef SHIZUKU_SAMPLE_DIAG_HPP
#define SHIZUKU_SAMPLE_DIAG_HPP
#include <stdint.h>

namespace sample {

// 何もしない (製品構成)。
struct diag_off {
  static constexpr bool ENABLED = false;
  static void stage(uint32_t) {}
  static void fault(const void * /*frame*/, uint32_t /*status*/) {}
};

// リセットをまたいで残る場所 (RP なら watchdog scratch) に段階番号を書く。
// 書き込み先は board が知っているので、board の型を受ける。
template <class BOARD> struct diag_stage_trace {
  static constexpr bool ENABLED = true;
  static void stage(uint32_t number) { BOARD::persistent_word(0) = number; }
  static void fault(const void *frame, uint32_t status) {
    BOARD::persistent_word(1) = status;
    BOARD::persistent_word(2) = reinterpret_cast<uintptr_t>(frame);
  }
};

} // namespace sample
#endif
