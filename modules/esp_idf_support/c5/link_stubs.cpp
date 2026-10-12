// ===========================================================================
//  ESP32-C5 リンク専用 (compile/link-only) のトラップスタブ
// ===========================================================================
//  ★これは実装ではない。chips/esp32_c5 の clic_direct_mode (rv32 の IntMode) が宣言だけして定義を持たない
//  関数を、リンクを通すためだけに**呼ばれたら即トラップする**形で埋めている。
//  成功を返す no-op は 1 つも無い (戻り値を作る関数も trap で戻らない)。
//  段2の M-only bootstrap/trap は本実装へ移行。残る未対応サービスは即停止する。
//  実装済みになった関数は、ここから消して本物へ置き換えること。
//  esp32_c5 (board) の stub は boards/esp32_c5/board_esp32_c5_stubs.cpp へ移した。
#include "shizuku/config.hpp"
#include "chips/esp32_c5/chip.hpp"
#include <cstdarg>
#include <cstddef>
#include <cstdint>

extern "C" int vsnprintf(char *str, size_t size, const char *format, va_list ap);

namespace {
[[noreturn]] inline void c5_unimplemented() {
  for (;;) {
    __builtin_trap();
  }
}
} // namespace

namespace shizuku::chips::esp32_c5 {

using c5_context = chip::arch_type::context_t;

uint32_t clic_direct_mode::debug_reason_take() { c5_unimplemented(); }
void clic_direct_mode::debug_step(bool) { c5_unimplemented(); }
template <class Context>
bool clic_direct_mode::faulted_in_thread_mode(const Context &) {
  c5_unimplemented();
}
template bool clic_direct_mode::faulted_in_thread_mode<c5_context>(const c5_context &);
uint32_t clic_direct_mode::fault_status() { c5_unimplemented(); }
uintptr_t clic_direct_mode::fault_address() { c5_unimplemented(); }
void clic_direct_mode::fault_status_clear() { c5_unimplemented(); }

} // namespace shizuku::chips::esp32_c5
