#ifndef SHIZUKU_FREERTOS_COMPAT_QUEUE_H
#define SHIZUKU_FREERTOS_COMPAT_QUEUE_H
#include "freertos/FreeRTOS.h"
#define queueQUEUE_TYPE_BASE 0U
#define queueQUEUE_TYPE_SET 0U
#define queueQUEUE_TYPE_MUTEX 1U
#define queueQUEUE_TYPE_COUNTING_SEMAPHORE 2U
#define queueQUEUE_TYPE_BINARY_SEMAPHORE 3U
#define queueQUEUE_TYPE_RECURSIVE_MUTEX 4U
#define queueSEND_TO_BACK 0
#define queueSEND_TO_FRONT 1
#if defined(__cplusplus)
static_assert(queueQUEUE_TYPE_BASE == 0U && queueQUEUE_TYPE_SET == 0U &&
              queueQUEUE_TYPE_MUTEX == 1U &&
              queueQUEUE_TYPE_COUNTING_SEMAPHORE == 2U &&
              queueQUEUE_TYPE_BINARY_SEMAPHORE == 3U &&
              queueQUEUE_TYPE_RECURSIVE_MUTEX == 4U);
#endif
#ifdef __cplusplus
extern "C" {
#endif
QueueHandle_t xQueueCreate(UBaseType_t, UBaseType_t);
QueueHandle_t xQueueCreateWithCaps(UBaseType_t, UBaseType_t, uint32_t);
QueueHandle_t xQueueCreateMutex(uint8_t);
BaseType_t xQueueGenericSend(QueueHandle_t, const void *, TickType_t, BaseType_t);
BaseType_t xQueueTakeMutexRecursive(QueueHandle_t, TickType_t);
BaseType_t xQueueGiveMutexRecursive(QueueHandle_t);
BaseType_t xQueueSend(QueueHandle_t, const void *, TickType_t);
BaseType_t xQueueSendToBack(QueueHandle_t, const void *, TickType_t);
BaseType_t xQueueSendToFront(QueueHandle_t, const void *, TickType_t);
BaseType_t xQueueSendFromISR(QueueHandle_t, const void *, BaseType_t *);
BaseType_t xQueueSendToBackFromISR(QueueHandle_t, const void *, BaseType_t *);
BaseType_t xQueueSendToFrontFromISR(QueueHandle_t, const void *, BaseType_t *);
BaseType_t xQueueReceive(QueueHandle_t, void *, TickType_t);
BaseType_t xQueueReceiveFromISR(QueueHandle_t, void *, BaseType_t *);
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t);
BaseType_t xQueueIsQueueEmptyFromISR(QueueHandle_t);
#ifdef __cplusplus
}
#endif
#endif
