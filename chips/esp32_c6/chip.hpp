#ifndef SHIZUKU_CHIPS_ESP32_C6_CHIP_HPP
#define SHIZUKU_CHIPS_ESP32_C6_CHIP_HPP
// ESP32-C6 (SoC) の型の入口。config.hpp の CHIP 別名がこれを指す (configs/kit.bzl)。
// ★ESP-IDF / pico-sdk を一切 include しない (chips の依存閉包はそれを含まない)。
#include <cstdint>

namespace shizuku::chips::esp32_c6 {
struct chip {
  // 単一コア (configs/kit.bzl の rv32_c6 は SHIZUKU_CPU_COUNT=1)
  static constexpr uint32_t kCoreCount = 1;
};
} // namespace shizuku::chips::esp32_c6

#endif // SHIZUKU_CHIPS_ESP32_C6_CHIP_HPP
