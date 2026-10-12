#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"

_Static_assert(sizeof(BaseType_t) == 4, "IDF RISC-V BaseType_t ABI");
_Static_assert(sizeof(UBaseType_t) == 4, "IDF RISC-V UBaseType_t ABI");
_Static_assert(sizeof(TickType_t) == 4, "IDF C5 TickType_t ABI");
_Static_assert(sizeof(StackType_t) == 1, "IDF RISC-V StackType_t ABI");

void shizuku_freertos_compat_c_header_smoke(void) {
    QueueHandle_t q = xQueueCreate(1, sizeof(uint32_t));
    SemaphoreHandle_t s = xSemaphoreCreateBinary();
    TaskHandle_t task = (TaskHandle_t)0;
    (void)xTaskCreate((TaskFunction_t)0, "abi", 64, 0, 1, &task);
    (void)xQueueSendFromISR(q, &task, 0);
    (void)xSemaphoreGiveFromISR(s, 0);
    portYIELD_FROM_ISR(pdFALSE);
}
