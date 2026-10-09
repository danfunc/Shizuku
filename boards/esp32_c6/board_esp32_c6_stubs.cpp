// ===========================================================================
//  ESP32-C6 board (esp32_c6) の stub。modules/esp_idf_support/c6/link_stubs.cpp から移した。
// ===========================================================================
//  ★実装ではない。呼ばれたら即トラップ (成功を返す no-op は置かない)。
#include "shizuku/config.hpp"
#include <cstdarg>
#include <cstddef>
#include <cstdint>

extern "C" int vsnprintf(char *str, size_t size, const char *format, va_list ap);

namespace {
[[noreturn]] inline void c6_unimplemented() {
  for (;;) {
    __builtin_trap();
  }
}
} // namespace

namespace shizuku {
namespace boards {

void esp32_c6::launch_core(void (*)()) { c6_unimplemented(); }
void esp32_c6::park_other_cores() { c6_unimplemented(); }
void esp32_c6::resume_other_cores() { c6_unimplemented(); }
void esp32_c6::diag_mute(bool) { c6_unimplemented(); }
int esp32_c6::dma_claim() { c6_unimplemented(); }
void esp32_c6::dma_copy(int, const void *, void *, uint32_t) { c6_unimplemented(); }
bool esp32_c6::dma_busy(int) { c6_unimplemented(); }
void esp32_c6::dma_release(int) { c6_unimplemented(); }

extern "C" {
// JTAG/OpenOCD sysbus read から読取可能な有界 RAM 診断リングバッファ (4096B)
// ハードウェア UART/USB CDC 出力は未実装のため、物理ピン出力は未達。
// 代替として RAM 上に直近の診断文字列を保持し、panic 時の死後解析を可能とする。
struct shizuku_c6_diag_ring_t {
  uint32_t magic;          // 0x44494147 ("DIAG")
  uint32_t head;           // 次の書込み位置 (単調増加、ring 内オフセットは head % 4096)
  uint32_t total_chars;    // 累計書込み文字数
  uint32_t overflow_count; // バッファ循環による上書き文字数
  char buffer[4096];       // 固定長リングバッファ
};
volatile shizuku_c6_diag_ring_t shizuku_c6_diag_ring = {
  0x44494147u, 0, 0, 0, {}
};
}

void esp32_c6::diag_printf(const char *format, ...) {
  if (format == nullptr) return;
  char tmp[256];
  va_list args;
  va_start(args, format);
  int n = vsnprintf(tmp, sizeof(tmp), format, args);
  va_end(args);
  if (n <= 0) return;
  uint32_t len = (static_cast<size_t>(n) < sizeof(tmp)) ? static_cast<uint32_t>(n) : static_cast<uint32_t>(sizeof(tmp) - 1);
  for (uint32_t i = 0; i < len; ++i) {
    uint32_t cur_head = shizuku_c6_diag_ring.head;
    uint32_t idx = cur_head % sizeof(shizuku_c6_diag_ring.buffer);
    if (cur_head >= sizeof(shizuku_c6_diag_ring.buffer)) {
      shizuku_c6_diag_ring.overflow_count = shizuku_c6_diag_ring.overflow_count + 1;
    }
    shizuku_c6_diag_ring.buffer[idx] = tmp[i];
    shizuku_c6_diag_ring.head = cur_head + 1;
    shizuku_c6_diag_ring.total_chars = shizuku_c6_diag_ring.total_chars + 1;
  }
}

} // namespace boards
} // namespace shizuku
