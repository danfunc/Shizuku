#ifndef SHIZUKU_FREERTOS_COMPAT_TASK_H
#define SHIZUKU_FREERTOS_COMPAT_TASK_H
#include "freertos/FreeRTOS.h"
typedef void (*TaskFunction_t)(void *);
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
#ifdef __cplusplus
}
#endif
#endif
