#ifndef SHIZUKU_HOT_HPP
#define SHIZUKU_HOT_HPP

// ===========================================================================
//  hot 関数の配置マクロ (既定は空)
// ===========================================================================
//  ★ESP32-C6 は chips/esp32_c6/chip.hpp が SHIZUKU_HOT を .iram1 に定義する。
//    それ以外の chip / ISA はここの空定義のまま (出力コードは変わらない)。
//  ★データには使わない。可変状態 (台帳・記述子表) は既定の配置のまま。

#ifndef SHIZUKU_HOT
#define SHIZUKU_HOT
#endif

#if defined(SHIZUKU_SVC_IN_RAM) && SHIZUKU_SVC_IN_RAM && defined(__ARM_ARCH_8M_MAIN__)
#define SHIZUKU_SVC_RAM __attribute__((section(".time_critical.shizuku_svc")))
#else
#define SHIZUKU_SVC_RAM
#endif

#endif // SHIZUKU_HOT_HPP
