// ===========================================================================
//  ESP32-C6 リンク専用 (compile/link-only) のトラップスタブ
// ===========================================================================
//  ★これは実装ではない。chips/esp32_c6 の plic_vector_mode (rv32 の IntMode) が宣言だけして定義を持たない
//  関数を、リンクを通すためだけに**呼ばれたら即トラップする**形で埋めている。
//  成功を返す no-op は 1 つも無い (戻り値を作る関数も trap で戻らない)。
//  段2の M-only bootstrap/trap は本実装へ移行。残る未対応サービスは即停止する。
//  実装済みになった関数は、ここから消して本物へ置き換えること。
//  esp32_c6 (board) の stub は boards/esp32_c6/board_esp32_c6_stubs.cpp へ移した。
#include "shizuku/config.hpp"
#include "chips/esp32_c6/chip.hpp"
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

namespace shizuku::chips::esp32_c6 {

using c6_context = chip::arch_type::context_t;

uint32_t plic_vector_mode::debug_reason_take() { c6_unimplemented(); }
void plic_vector_mode::debug_step(bool) { c6_unimplemented(); }
template <class Context>
bool plic_vector_mode::faulted_in_thread_mode(const Context &) {
  c6_unimplemented();
}
template bool plic_vector_mode::faulted_in_thread_mode<c6_context>(const c6_context &);
uint32_t plic_vector_mode::fault_status() { c6_unimplemented(); }
uintptr_t plic_vector_mode::fault_address() { c6_unimplemented(); }
void plic_vector_mode::fault_status_clear() { c6_unimplemented(); }

} // namespace shizuku::chips::esp32_c6
