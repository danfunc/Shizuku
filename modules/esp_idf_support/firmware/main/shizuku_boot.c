// ESP-IDF v6.1 の起動連鎖 (直接確認済み):
//   ROM → bootloader → call_start_cpu0 (components/esp_system/port/cpu_start.c)
//   → SYS_STARTUP_FN() = g_startup_fn[0] (components/esp_system/startup.c:75-83)
//   → start_cpu0 (弱シンボル。既定は start_cpu0_default)。
// ★ここで強シンボルとして start_cpu0 を定義すると既定を置き換える。call_start_cpu0 は
//   ここへ**到達する側**なので、呼んではならない (再帰する)。
//
// ★置き換えの代償 (IDF の private ABI、v6.1 固定): start_cpu0_default が行う
//   do_core_init / グローバルコンストラクタ / do_secondary_init /
//   esp_startup_start_app (= main タスク生成と vTaskStartScheduler) を**すべて飛ばす**。
//   ヒープ・libc・PMP の初期化責任は誰も負っていない。RTC WDTは従来同様snapshot後に停止し、他WDTには書込み/給餌しない。
//
// start_cpu0 は C6 kernel archive の pre-init entry を呼ぶ。
// entry は reset/WDT状態をnoinit RAMへ記録後、M-only stage-2 kernel/thread0 pilotへ進む。
// ctor / heap / PMP / timer / FreeRTOS は初期化しない。
#include "early_diag.h"

#include "sdkconfig.h"

#if CONFIG_IDF_TARGET_ESP32C5
#define SHIZUKU_EARLY_BOOT shizuku_c5_early_boot
#else
#define SHIZUKU_EARLY_BOOT shizuku_c6_early_boot
#endif

__attribute__((noreturn)) void SHIZUKU_EARLY_BOOT(void);

__attribute__((noreturn)) void start_cpu0(void)
{
    shizuku_c6_early_diag_begin();
    SHIZUKU_EARLY_BOOT();
}

// app_startup.c の main_task が強参照するので、リンクのために定義する。
// start_cpu0 が esp_startup_start_app に到達しない以上、これも実行されない。
void app_main(void)
{
    for (;;) {
        __builtin_trap();
    }
}
