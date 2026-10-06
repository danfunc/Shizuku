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
//   ヒープ・libc・ウォッチドッグ・PMP の初期化責任は誰も負っていない。
//
// このイメージは compile/link-only の到達点で、**起動しても何も動かさない**。
// start_cpu0 は即トラップする (カーネルは動かさない・FreeRTOS スケジューラは始めない)。
// カーネルの実体は --whole-archive で取り込まれているが、ここからは呼ばない。
#include <stdint.h>

__attribute__((noreturn)) void start_cpu0(void)
{
    for (;;) {
        __builtin_trap();
    }
}

// app_startup.c の main_task が強参照するので、リンクのために定義する。
// start_cpu0 が esp_startup_start_app に到達しない以上、これも実行されない。
void app_main(void)
{
    for (;;) {
        __builtin_trap();
    }
}
