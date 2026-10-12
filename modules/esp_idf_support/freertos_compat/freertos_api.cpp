#include "compat.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"

using shizuku::freertos_compat::domain;
namespace {
domain *g_domain = nullptr;
// C5's selected single-core profile has no TLS runtime in the early Shizuku boot.
// Interrupts are masked while this nesting stack is modified.
volatile uint32_t critical_keys[16]{};
volatile unsigned critical_depth = 0;
void *encode(int id) { return id < 0 ? nullptr : reinterpret_cast<void *>(static_cast<uintptr_t>(id) + 1); }
int decode(void *p) { return !p ? -1 : static_cast<int>(reinterpret_cast<uintptr_t>(p) - 1); }
uint32_t tid(TaskHandle_t task) { return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(task)); }
}

extern "C" BaseType_t shizuku_freertos_compat_bind(void *d, uint32_t handler, uint32_t method,uint32_t tick_hz) {
  if (!d||!handler||!tick_hz) return pdFAIL;
  auto *next=static_cast<domain *>(d);
  if(!next->configure_backend(handler,method,tick_hz))return pdFAIL;
  g_domain=next;
  return pdPASS;
}
extern "C" BaseType_t xPortInIsrContext(void) { return g_domain&&g_domain->isr_context()?pdTRUE:pdFALSE; }
extern "C" void portENTER_CRITICAL(portMUX_TYPE *) {
  const unsigned depth=critical_depth;
  if(g_domain&&depth<16){critical_keys[depth]=g_domain->critical_enter();critical_depth=depth+1;}
}
extern "C" void portEXIT_CRITICAL(portMUX_TYPE *) {
  const unsigned depth=critical_depth;
  if(g_domain&&depth){critical_depth=depth-1;g_domain->critical_exit(critical_keys[depth-1]);}
}
extern "C" void portENTER_CRITICAL_ISR(portMUX_TYPE *m) { portENTER_CRITICAL(m); }
extern "C" void portEXIT_CRITICAL_ISR(portMUX_TYPE *m) { portEXIT_CRITICAL(m); }

extern "C" BaseType_t xTaskCreate(TaskFunction_t f,const char *n,uint32_t s,void *a,UBaseType_t p,TaskHandle_t *h) {
  uint32_t id=0; if(!g_domain||!g_domain->task_create(f,n,s,a,p,&id))return pdFAIL; if(h)*h=reinterpret_cast<void *>(static_cast<uintptr_t>(id)); return pdPASS;
}
extern "C" BaseType_t xTaskCreatePinnedToCore(TaskFunction_t f,const char *n,uint32_t s,void *a,UBaseType_t p,TaskHandle_t *h,BaseType_t core) {
  if(core!=0 && core!=-1)return pdFAIL; return xTaskCreate(f,n,s,a,p,h);
}
extern "C" BaseType_t xTaskCreatePinnedToCoreWithCaps(TaskFunction_t f,const char *n,uint32_t s,void *a,UBaseType_t p,TaskHandle_t *h,BaseType_t core,uint32_t caps) {
  if(caps)return pdFAIL; return xTaskCreatePinnedToCore(f,n,s,a,p,h,core);
}
extern "C" void vTaskDelay(TickType_t t) { if(g_domain)g_domain->task_delay(t); }
extern "C" void taskYIELD(void) { if(g_domain)g_domain->task_delay(0); }
extern "C" UBaseType_t uxTaskPriorityGet(TaskHandle_t h) { return g_domain?g_domain->task_priority(h?tid(h):0):0; }
extern "C" TickType_t xTaskGetTickCount(void) { return g_domain?static_cast<TickType_t>(g_domain->tick_count()):0; }
extern "C" void vTaskSuspend(TaskHandle_t h) { if(g_domain)g_domain->task_suspend(h?tid(h):0); }
extern "C" void vTaskResume(TaskHandle_t h) { if(g_domain)g_domain->task_resume(tid(h)); }
extern "C" BaseType_t xTaskResumeFromISR(TaskHandle_t h) { bool hp=false;return g_domain&&g_domain->task_resume(tid(h),true,&hp)&&hp?pdTRUE:pdFALSE; }

extern "C" QueueHandle_t xQueueCreate(UBaseType_t n,UBaseType_t sz) { return g_domain?encode(g_domain->create_queue(n,sz)):nullptr; }
extern "C" QueueHandle_t xQueueCreateWithCaps(UBaseType_t n,UBaseType_t sz,uint32_t caps) { return caps?nullptr:xQueueCreate(n,sz); }
extern "C" BaseType_t xQueueSend(QueueHandle_t q,const void *p,TickType_t t) { return g_domain&&g_domain->queue_send(decode(q),p,t)?pdTRUE:pdFALSE; }
extern "C" BaseType_t xQueueSendToBack(QueueHandle_t q,const void *p,TickType_t t) { return xQueueSend(q,p,t); }
extern "C" BaseType_t xQueueSendToFront(QueueHandle_t q,const void *p,TickType_t t) { return g_domain&&g_domain->queue_send(decode(q),p,t,true)?pdTRUE:pdFALSE; }
extern "C" BaseType_t xQueueSendFromISR(QueueHandle_t q,const void *p,BaseType_t *w) { bool hp=false;auto ok=g_domain&&g_domain->queue_send(decode(q),p,0,false,true,&hp);if(w&&hp)*w=pdTRUE;return ok?pdTRUE:pdFALSE; }
extern "C" BaseType_t xQueueSendToBackFromISR(QueueHandle_t q,const void *p,BaseType_t *w) { return xQueueSendFromISR(q,p,w); }
extern "C" BaseType_t xQueueSendToFrontFromISR(QueueHandle_t q,const void *p,BaseType_t *w) { bool hp=false;auto ok=g_domain&&g_domain->queue_send(decode(q),p,0,true,true,&hp);if(w&&hp)*w=pdTRUE;return ok?pdTRUE:pdFALSE; }
extern "C" BaseType_t xQueueReceive(QueueHandle_t q,void *p,TickType_t t) { return g_domain&&g_domain->queue_receive(decode(q),p,t)?pdTRUE:pdFALSE; }
extern "C" BaseType_t xQueueReceiveFromISR(QueueHandle_t q,void *p,BaseType_t *w) { bool hp=false;auto ok=g_domain&&g_domain->queue_receive(decode(q),p,0,true,&hp);if(w&&hp)*w=pdTRUE;return ok?pdTRUE:pdFALSE; }
extern "C" UBaseType_t uxQueueMessagesWaiting(QueueHandle_t q) { return g_domain?g_domain->queue_messages(decode(q)):0; }

extern "C" SemaphoreHandle_t xSemaphoreCreateBinary(void) { return g_domain?encode(g_domain->create_semaphore(1,0)):nullptr; }
extern "C" SemaphoreHandle_t xSemaphoreCreateCounting(UBaseType_t n,UBaseType_t init) { return g_domain?encode(g_domain->create_semaphore(n,init)):nullptr; }
extern "C" SemaphoreHandle_t xSemaphoreCreateMutex(void) { return g_domain?encode(g_domain->create_mutex(false)):nullptr; }
extern "C" SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void) { return g_domain?encode(g_domain->create_mutex(true)):nullptr; }
extern "C" BaseType_t xSemaphoreTake(SemaphoreHandle_t s,TickType_t t) { int h=decode(s); return g_domain&&(g_domain->mutex_take(h,t)||g_domain->semaphore_take(h,t))?pdTRUE:pdFALSE; }
extern "C" BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t s,TickType_t t) { return g_domain&&g_domain->mutex_take(decode(s),t)?pdTRUE:pdFALSE; }
extern "C" BaseType_t xSemaphoreTakeFromISR(SemaphoreHandle_t s,BaseType_t *) { return g_domain&&g_domain->semaphore_take(decode(s),0,true)?pdTRUE:pdFALSE; }
extern "C" BaseType_t xSemaphoreGive(SemaphoreHandle_t s) { int h=decode(s); return g_domain&&(g_domain->mutex_give(h)||g_domain->semaphore_give(h))?pdTRUE:pdFALSE; }
extern "C" BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t s) { return g_domain&&g_domain->mutex_give(decode(s))?pdTRUE:pdFALSE; }
extern "C" BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t s,BaseType_t *w) { bool hp=false;auto ok=g_domain&&g_domain->semaphore_give(decode(s),true,&hp);if(w&&hp)*w=pdTRUE;return ok?pdTRUE:pdFALSE; }

// Fail closed until these object kinds have concrete storage and wake semantics.
extern "C" EventGroupHandle_t xEventGroupCreate(void){return nullptr;}
extern "C" EventBits_t xEventGroupSetBits(EventGroupHandle_t,EventBits_t){return 0;}
extern "C" EventBits_t xEventGroupClearBits(EventGroupHandle_t,EventBits_t){return 0;}
extern "C" EventBits_t xEventGroupWaitBits(EventGroupHandle_t,EventBits_t,BaseType_t,BaseType_t,TickType_t){return 0;}
extern "C" TimerHandle_t xTimerCreate(const char*,TickType_t,UBaseType_t,void*,TimerCallbackFunction_t){return nullptr;}
extern "C" BaseType_t xTimerStart(TimerHandle_t,TickType_t){return pdFAIL;}
extern "C" BaseType_t xTimerStop(TimerHandle_t,TickType_t){return pdFAIL;}
extern "C" BaseType_t xTimerReset(TimerHandle_t,TickType_t){return pdFAIL;}
extern "C" BaseType_t xTimerDelete(TimerHandle_t,TickType_t){return pdFAIL;}
extern "C" BaseType_t xTimerChangePeriod(TimerHandle_t,TickType_t,TickType_t){return pdFAIL;}
extern "C" BaseType_t xTimerResetFromISR(TimerHandle_t,BaseType_t *){return pdFAIL;}
extern "C" BaseType_t xTimerStopFromISR(TimerHandle_t,BaseType_t *){return pdFAIL;}
extern "C" BaseType_t xTimerChangePeriodFromISR(TimerHandle_t,TickType_t,BaseType_t *){return pdFAIL;}
extern "C" TickType_t xTimerGetExpiryTime(TimerHandle_t){return 0;}
extern "C" BaseType_t xTimerIsTimerActive(TimerHandle_t){return pdFALSE;}
