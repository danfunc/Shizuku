#ifndef SHIZUKU_FREERTOS_COMPAT_FREERTOS_H
#define SHIZUKU_FREERTOS_COMPAT_FREERTOS_H
#include <stdint.h>
#if defined(__has_include)
#  if __has_include("esp_attr.h")
#    include "esp_attr.h"
#  endif
#endif
#if defined(__has_include)
#  if __has_include("sdkconfig.h")
#    include "sdkconfig.h"
#  endif
#endif
typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef uint32_t TickType_t;
// ESP-IDF v6.1 RISC-V portmacro.h defines portSTACK_TYPE as uint8_t.
typedef uint8_t StackType_t;
typedef void * TaskHandle_t;
typedef void * QueueHandle_t;
typedef void * SemaphoreHandle_t;
typedef void * EventGroupHandle_t;
typedef void * TimerHandle_t;
// FreeRTOS-Kernel v11 timeout state ABI used by xTaskCheckForTimeOut.
typedef struct { BaseType_t xOverflowCount; TickType_t xTimeOnEntering; } TimeOut_t;
typedef struct { volatile uint32_t owner; volatile uint32_t count; } portMUX_TYPE;
typedef struct {
#if defined(CONFIG_FREERTOS_USE_LIST_DATA_INTEGRITY_CHECK_BYTES) && CONFIG_FREERTOS_USE_LIST_DATA_INTEGRITY_CHECK_BYTES
  TickType_t xDummy1;
#endif
  TickType_t xDummy2; void *pvDummy3[4];
#if defined(CONFIG_FREERTOS_USE_LIST_DATA_INTEGRITY_CHECK_BYTES) && CONFIG_FREERTOS_USE_LIST_DATA_INTEGRITY_CHECK_BYTES
  TickType_t xDummy4;
#endif
} StaticListItem_t;
typedef struct {
#if defined(CONFIG_FREERTOS_USE_LIST_DATA_INTEGRITY_CHECK_BYTES) && CONFIG_FREERTOS_USE_LIST_DATA_INTEGRITY_CHECK_BYTES
  TickType_t xDummy1;
#endif
  TickType_t xDummy2; void *pvDummy3[2];
} StaticMiniListItem_t;
typedef struct {
#if defined(CONFIG_FREERTOS_USE_LIST_DATA_INTEGRITY_CHECK_BYTES) && CONFIG_FREERTOS_USE_LIST_DATA_INTEGRITY_CHECK_BYTES
  TickType_t xDummy1;
#endif
  UBaseType_t uxDummy2; void *pvDummy3; StaticMiniListItem_t xDummy4;
#if defined(CONFIG_FREERTOS_USE_LIST_DATA_INTEGRITY_CHECK_BYTES) && CONFIG_FREERTOS_USE_LIST_DATA_INTEGRITY_CHECK_BYTES
  TickType_t xDummy5;
#endif
} StaticList_t;
typedef struct xSTATIC_QUEUE {
  void *pvDummy1[3];
  union { void *pvDummy2; UBaseType_t uxDummy2; } u;
  StaticList_t xDummy3[2];
  UBaseType_t uxDummy4[3];
  uint8_t ucDummy5[2];
  uint8_t ucDummy6;
  void *pvDummy7; // IDF v6.1 configUSE_QUEUE_SETS = 1
#if defined(CONFIG_FREERTOS_USE_TRACE_FACILITY) && CONFIG_FREERTOS_USE_TRACE_FACILITY
  UBaseType_t uxDummy8;
  uint8_t ucDummy9;
#endif
  portMUX_TYPE xDummyQueueLock;
} StaticQueue_t;
typedef StaticQueue_t StaticSemaphore_t;
typedef struct xSTATIC_EVENT_GROUP {
  TickType_t xDummy1;
  StaticList_t xDummy2;
#if defined(CONFIG_FREERTOS_USE_TRACE_FACILITY) && CONFIG_FREERTOS_USE_TRACE_FACILITY
  UBaseType_t uxDummy3;
#endif
  uint8_t ucDummy4;
  portMUX_TYPE xDummyEventGroupLock;
} StaticEventGroup_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define pdFAIL 0
#define errQUEUE_EMPTY 0
#define errQUEUE_FULL 0
#define errQUEUE_BLOCKED (-4)
#define errQUEUE_YIELD (-5)
#define portMAX_DELAY UINT32_MAX
#define taskSCHEDULER_SUSPENDED 0
#define taskSCHEDULER_NOT_STARTED 1
#define taskSCHEDULER_RUNNING 2
#define tskIDLE_PRIORITY 0U
#ifndef CONFIG_FREERTOS_HZ
#define CONFIG_FREERTOS_HZ 100
#endif
#define pdMS_TO_TICKS(ms) ((TickType_t)(((uint64_t)(ms) * CONFIG_FREERTOS_HZ) / 1000U))
#define portMUX_INITIALIZER_UNLOCKED { UINT32_C(0xB33FFFFF), 0 }
#define portYIELD_FROM_ISR(x) shizuku_freertos_compat_yield_from_isr((BaseType_t)(x))
#define portEND_SWITCHING_ISR(x) portYIELD_FROM_ISR(x)
#ifdef __cplusplus
extern "C" {
#endif
BaseType_t shizuku_freertos_compat_bind(void *domain, uint32_t handler_object,
                                      uint32_t call_method_id, uint32_t tick_hz);
void shizuku_freertos_compat_set_clock(uint64_t (*clock_us)(void));
void shizuku_freertos_compat_run_scheduler(uint32_t grant_cycles);
void shizuku_freertos_compat_isr_enter(void);
void shizuku_freertos_compat_isr_exit(void);
void shizuku_freertos_compat_yield_from_isr(BaseType_t higher_priority_woken);
uintptr_t shizuku_freertos_compat_dispatch(uintptr_t packet);
BaseType_t xPortInIsrContext(void);
void vPortEnterCritical(portMUX_TYPE *mux);
void vPortExitCritical(portMUX_TYPE *mux);
void vTaskSetTimeOutState(TimeOut_t *state);
BaseType_t xTaskCheckForTimeOut(TimeOut_t *state, TickType_t *ticks_to_wait);
BaseType_t xQueueSemaphoreTake(QueueHandle_t queue, TickType_t ticks_to_wait);
void portENTER_CRITICAL(portMUX_TYPE *mux);
void portEXIT_CRITICAL(portMUX_TYPE *mux);
void portENTER_CRITICAL_ISR(portMUX_TYPE *mux);
void portEXIT_CRITICAL_ISR(portMUX_TYPE *mux);
#ifdef __cplusplus
}
// Layout contract checked against ESP-IDF v6.1:
// FreeRTOS-Kernel/portable/riscv/include/freertos/portmacro.h:95-107;
// FreeRTOS-Kernel/include/freertos/FreeRTOS.h:1227-1267, 1349-1377, 1393-1406;
// config/include/freertos/FreeRTOSConfig.h:107, 136-137; esp_hw_support/include/spinlock.h:42-45.
static_assert(sizeof(BaseType_t)==4 && sizeof(UBaseType_t)==4);
static_assert(sizeof(TickType_t)==4 && sizeof(StackType_t)==1);
// ESP-IDF v6.1 FreeRTOS-Kernel/include/freertos/projdefs.h and
// task.h:273-275. Keep these ABI values aligned: esp_libc locks branches on
// taskSCHEDULER_NOT_STARTED before deciding whether to take its mutex.
static_assert(pdFALSE == 0 && pdTRUE == 1 && pdFAIL == 0 && pdPASS == 1);
static_assert(errQUEUE_EMPTY == 0 && errQUEUE_FULL == 0 &&
              errQUEUE_BLOCKED == -4 && errQUEUE_YIELD == -5);
static_assert(taskSCHEDULER_SUSPENDED == 0 &&
              taskSCHEDULER_NOT_STARTED == 1 &&
              taskSCHEDULER_RUNNING == 2);
static_assert(portMAX_DELAY == UINT32_MAX); // C5 configUSE_16_BIT_TICKS=0.
#if UINTPTR_MAX == UINT32_MAX && (!defined(CONFIG_FREERTOS_USE_LIST_DATA_INTEGRITY_CHECK_BYTES) || !CONFIG_FREERTOS_USE_LIST_DATA_INTEGRITY_CHECK_BYTES) && (!defined(CONFIG_FREERTOS_USE_TRACE_FACILITY) || !CONFIG_FREERTOS_USE_TRACE_FACILITY)
static_assert(sizeof(portMUX_TYPE)==8 && alignof(portMUX_TYPE)==4);
static_assert(sizeof(StaticListItem_t)==20 && sizeof(StaticMiniListItem_t)==12);
static_assert(sizeof(StaticList_t)==20);
static_assert(sizeof(StaticQueue_t)==84 && alignof(StaticQueue_t)==4);
static_assert(sizeof(StaticSemaphore_t)==84 && alignof(StaticSemaphore_t)==4);
static_assert(sizeof(StaticEventGroup_t)==36 && alignof(StaticEventGroup_t)==4);
#endif
#endif
#endif
