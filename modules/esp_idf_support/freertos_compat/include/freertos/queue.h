#ifndef SHIZUKU_FREERTOS_COMPAT_QUEUE_H
#define SHIZUKU_FREERTOS_COMPAT_QUEUE_H
#include "freertos/FreeRTOS.h"
#ifdef __cplusplus
extern "C" {
#endif
QueueHandle_t xQueueCreate(UBaseType_t, UBaseType_t);
QueueHandle_t xQueueCreateWithCaps(UBaseType_t, UBaseType_t, uint32_t);
BaseType_t xQueueSend(QueueHandle_t, const void *, TickType_t);
BaseType_t xQueueSendToBack(QueueHandle_t, const void *, TickType_t);
BaseType_t xQueueSendToFront(QueueHandle_t, const void *, TickType_t);
BaseType_t xQueueSendFromISR(QueueHandle_t, const void *, BaseType_t *);
BaseType_t xQueueSendToBackFromISR(QueueHandle_t, const void *, BaseType_t *);
BaseType_t xQueueSendToFrontFromISR(QueueHandle_t, const void *, BaseType_t *);
BaseType_t xQueueReceive(QueueHandle_t, void *, TickType_t);
BaseType_t xQueueReceiveFromISR(QueueHandle_t, void *, BaseType_t *);
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t);
#ifdef __cplusplus
}
#endif
#endif
