// ===========================================================================
//  ESP32-C6 リンク専用 (compile/link-only) のトラップスタブ
// ===========================================================================
//  ★これは実装ではない。rv32_c6 / esp32_c6 の骨格が宣言だけして定義を持たない
//  関数を、リンクを通すためだけに**呼ばれたら即トラップする**形で埋めている。
//  成功を返す no-op は 1 つも無い (戻り値を作る関数も trap で戻らない)。
//  段2の M-only bootstrap/trap は本実装へ移行。残る未対応サービスは即停止する。
//  実装済みになった関数は、ここから消して本物へ置き換えること。
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
namespace archs {

uint32_t rv32_c6::debug_reason_take() { c6_unimplemented(); }
void rv32_c6::debug_step(bool) { c6_unimplemented(); }
bool rv32_c6::faulted_in_thread_mode(const context_t &) {
  c6_unimplemented();
}
uint32_t rv32_c6::fault_status() { c6_unimplemented(); }
uintptr_t rv32_c6::fault_address() { c6_unimplemented(); }
void rv32_c6::fault_status_clear() { c6_unimplemented(); }

} // namespace archs

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
