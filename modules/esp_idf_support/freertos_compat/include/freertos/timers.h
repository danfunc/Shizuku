#ifndef SHIZUKU_FREERTOS_COMPAT_TIMERS_H
#define SHIZUKU_FREERTOS_COMPAT_TIMERS_H
#include "freertos/FreeRTOS.h"
typedef void (*TimerCallbackFunction_t)(TimerHandle_t);
#ifdef __cplusplus
extern "C" {
#endif
TimerHandle_t xTimerCreate(const char *, TickType_t, UBaseType_t, void *, TimerCallbackFunction_t);
BaseType_t xTimerStart(TimerHandle_t, TickType_t);
BaseType_t xTimerStop(TimerHandle_t, TickType_t);
BaseType_t xTimerReset(TimerHandle_t, TickType_t);
BaseType_t xTimerDelete(TimerHandle_t, TickType_t);
BaseType_t xTimerChangePeriod(TimerHandle_t, TickType_t, TickType_t);
BaseType_t xTimerResetFromISR(TimerHandle_t, BaseType_t *);
BaseType_t xTimerStopFromISR(TimerHandle_t, BaseType_t *);
BaseType_t xTimerChangePeriodFromISR(TimerHandle_t, TickType_t, BaseType_t *);
TickType_t xTimerGetExpiryTime(TimerHandle_t);
BaseType_t xTimerIsTimerActive(TimerHandle_t);
#ifdef __cplusplus
}
#endif
#endif
