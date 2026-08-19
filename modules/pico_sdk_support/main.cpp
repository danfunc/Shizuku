#include "pico/stdio.h"
#include "pico/stdlib.h"
#include "shizuku/abis/rp2040_abi.hpp"
#include "shizuku/app_entry.hpp"
#include "shizuku/kernel.hpp"
#include "stdio.h"
#include "stdlib.h"

void shizuku::app_entry() { return; }

int main() {
  stdio_init_all();
  // 例外の結線・優先度 (board) とメモリマネージャの初期化。
  shizuku::kernel_instance.init();
  // スレッドモードのスタックを PSP へ移す (SPSEL=1)。以後 MSP は例外専用。
  void *entry_psp = (void *)(((uintptr_t)malloc(1024) + 1020) & (~0b11));
  uintptr_t control_mask = 2; // SPSEL=1, nPRIV=0
  asm volatile("MSR PSP,%[entry_psp];"
               "MSR CONTROL,%[control_mask];"
               "isb;"
               :
               : [entry_psp] "r"(entry_psp), [control_mask] "r"(control_mask)
               : "memory");
  sleep_ms(1000);
  // svc 往復の最小デモ (CTX_SAVE → dispatch → CTX_RESTORE の経路確認)。
  while (1) {
    (void)shizuku::abis::rp2040::svc_abi_caller_converter<1>(0, 0, 0, 0, 0);
    printf("success\n");
    sleep_ms(100);
  }
}
