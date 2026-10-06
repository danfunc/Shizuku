// ===========================================================================
//  ESP32-C6 リンク専用 (compile/link-only) のトラップスタブ
// ===========================================================================
//  ★これは実装ではない。rv32_c6 / esp32_c6 の骨格が宣言だけして定義を持たない
//  関数を、リンクを通すためだけに**呼ばれたら即トラップする**形で埋めている。
//  成功を返す no-op は 1 つも無い (戻り値を作る関数も trap で戻らない)。
//  このスタブを含むイメージは、起動してもカーネルは動かない (意図的)。
//  実装済みになった関数は、ここから消して本物へ置き換えること。
#include "shizuku/config.hpp"

namespace {
[[noreturn]] inline void c6_unimplemented() {
  for (;;) {
    __builtin_trap();
  }
}
} // namespace

namespace shizuku {
namespace archs {

uintptr_t rv32_c6::return_stub() { c6_unimplemented(); }
bool rv32_c6::current_priv() { c6_unimplemented(); }
void rv32_c6::timer_oneshot(uint32_t) { c6_unimplemented(); }
void rv32_c6::timer_cancel() { c6_unimplemented(); }
uint32_t rv32_c6::timer_remaining(bool &) { c6_unimplemented(); }
void rv32_c6::pend_context_switch() { c6_unimplemented(); }
uint32_t rv32_c6::debug_reason_take() { c6_unimplemented(); }
void rv32_c6::debug_step(bool) { c6_unimplemented(); }
bool rv32_c6::faulted_in_thread_mode(const context_t &) {
  c6_unimplemented();
}
uint32_t rv32_c6::fault_status() { c6_unimplemented(); }
uintptr_t rv32_c6::fault_address() { c6_unimplemented(); }
void rv32_c6::fault_status_clear() { c6_unimplemented(); }
void rv32_c6::enter_thread_mode(uintptr_t, uintptr_t, void (*)()) {
  c6_unimplemented();
}

} // namespace archs

namespace boards {

void esp32_c6::init(uint32_t) { c6_unimplemented(); }
void esp32_c6::launch_core(void (*)()) { c6_unimplemented(); }
void esp32_c6::park_other_cores() { c6_unimplemented(); }
void esp32_c6::resume_other_cores() { c6_unimplemented(); }
void esp32_c6::diag_mute(bool) { c6_unimplemented(); }
int esp32_c6::dma_claim() { c6_unimplemented(); }
bool esp32_c6::dma_busy(int) { c6_unimplemented(); }
void esp32_c6::dma_release(int) { c6_unimplemented(); }
uint64_t esp32_c6::time_us() { c6_unimplemented(); }
uint32_t esp32_c6::cycles_per_us() { c6_unimplemented(); }
uintptr_t esp32_c6::unprivileged_floor() { c6_unimplemented(); }
void esp32_c6::diag_printf(const char *, ...) { c6_unimplemented(); }
void esp32_c6::panic(const char *) { c6_unimplemented(); }

} // namespace boards
} // namespace shizuku
