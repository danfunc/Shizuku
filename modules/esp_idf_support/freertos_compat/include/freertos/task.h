#ifndef SHIZUKU_FREERTOS_COMPAT_TASK_H
#define SHIZUKU_FREERTOS_COMPAT_TASK_H
#include "freertos/FreeRTOS.h"
typedef void (*TaskFunction_t)(void *);
typedef enum {
  eNoAction = 0,
  eSetBits = 1,
  eIncrement = 2,
  eSetValueWithOverwrite = 3,
  eSetValueWithoutOverwrite = 4
} eNotifyAction;
#if defined(__cplusplus)
static_assert(eNoAction == 0 && eSetBits == 1 && eIncrement == 2 &&
              eSetValueWithOverwrite == 3 && eSetValueWithoutOverwrite == 4);
#endif
#ifdef __cplusplus
extern "C" {
#endif
BaseType_t xTaskCreate(TaskFunction_t, const char *, uint32_t, void *, UBaseType_t, TaskHandle_t *);
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t, const char *, uint32_t, void *, UBaseType_t, TaskHandle_t *, BaseType_t);
BaseType_t xTaskCreatePinnedToCoreWithCaps(TaskFunction_t, const char *, uint32_t, void *, UBaseType_t, TaskHandle_t *, BaseType_t, uint32_t);
void vTaskDelay(TickType_t);
void taskYIELD(void);
UBaseType_t uxTaskPriorityGet(TaskHandle_t);
TickType_t xTaskGetTickCount(void);
void vTaskSuspend(TaskHandle_t);
void vTaskResume(TaskHandle_t);
BaseType_t xTaskResumeFromISR(TaskHandle_t);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
BaseType_t xTaskGetSchedulerState(void);
TickType_t xTaskGetTickCountFromISR(void);
BaseType_t xTaskGenericNotify(TaskHandle_t, UBaseType_t, uint32_t, eNotifyAction, uint32_t *);
BaseType_t xTaskGenericNotifyFromISR(TaskHandle_t, UBaseType_t, uint32_t, eNotifyAction,
                                     uint32_t *, BaseType_t *);
BaseType_t xTaskGenericNotifyWait(UBaseType_t, uint32_t, uint32_t, uint32_t *, TickType_t);
void vTaskGenericNotifyGiveFromISR(TaskHandle_t, UBaseType_t, BaseType_t *);
uint32_t ulTaskGenericNotifyTake(UBaseType_t, BaseType_t, TickType_t);
BaseType_t xTaskGenericNotifyStateClear(TaskHandle_t, UBaseType_t);
uint32_t ulTaskGenericNotifyValueClear(TaskHandle_t, UBaseType_t, uint32_t);
#ifdef __cplusplus
}
#endif
#endif
