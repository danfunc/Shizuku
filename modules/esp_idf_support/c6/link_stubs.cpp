// ===========================================================================
//  ESP32-C6 リンク専用 (compile/link-only) のトラップスタブ
// ===========================================================================
//  ★これは実装ではない。rv32_c6 / esp32_c6 の骨格が宣言だけして定義を持たない
//  関数を、リンクを通すためだけに**呼ばれたら即トラップする**形で埋めている。
//  成功を返す no-op は 1 つも無い (戻り値を作る関数も trap で戻らない)。
//  段2の M-only bootstrap/trap は本実装へ移行。残る未対応サービスは即停止する。
//  実装済みになった関数は、ここから消して本物へ置き換えること。
//  esp32_c6 (board) の stub は boards/esp32_c6/board_esp32_c6_stubs.cpp へ移した。
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

} // namespace shizuku
