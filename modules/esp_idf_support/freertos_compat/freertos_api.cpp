#include "compat.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"
#include <cstring>

using shizuku::freertos_compat::domain;
#if defined(SHIZUKU_FREERTOS_LINK_WRAP)
#define SHZ_FREERTOS_API(name) __wrap_##name
#else
#define SHZ_FREERTOS_API(name) name
#endif
extern "C" volatile uint32_t shizuku_c5_ble_timeout_shim_calls;
extern "C" volatile uint32_t shizuku_c5_ble_queue_sem_shim_calls;
extern "C" volatile uint32_t shizuku_c5_ble_critical_shim_calls;
extern "C" void shizuku_c5_ble_note_unimplemented(uint32_t api_id) __attribute__((weak));
extern "C" void shizuku_c5_ble_note_sync_alloc(uint32_t kind, uint32_t reason,
                                                uint32_t objects_used,
                                                uint32_t static_handles_used,
                                                uint32_t mutex_storage_used) __attribute__((weak));
extern "C" void shizuku_c5_ble_note_queue_shape(uint32_t capacity, uint32_t item_size) __attribute__((weak));
extern "C" void shizuku_c5_ble_note_notification(uint32_t api_kind, bool success) __attribute__((weak));
extern "C" void shizuku_c5_ble_note_task_create(uint32_t task_id, const char *name,
                                                uint32_t success, uint32_t priority)
    __attribute__((weak));
extern "C" void shizuku_c5_ble_note_wait(uint32_t task_id, uint32_t kind, uint32_t handle,
                                          uint32_t timeout_ticks, uint32_t entering,
                                          uint32_t result) __attribute__((weak));
extern "C" void shizuku_c5_ble_note_wait_callsite(uint32_t pc) __attribute__((weak));
extern "C" void shizuku_c5_ble_note_scheduler(uint32_t event, uint32_t state) __attribute__((weak));
namespace {
domain *g_domain = nullptr;
// C5's selected single-core profile has no TLS runtime in the early Shizuku boot.
// Interrupts are masked while this nesting stack is modified.
volatile uint32_t critical_keys[16]{};
volatile unsigned critical_depth = 0;
volatile BaseType_t g_scheduler_state = taskSCHEDULER_NOT_STARTED;
struct static_handle_entry { const void *storage = nullptr; int object_id = -1; bool used = false; };
static_handle_entry static_handles[32]{};
StaticQueue_t dynamic_mutex_storage[32]{};
bool dynamic_mutex_storage_used[32]{};
void *last_mutex_handle = nullptr;
void *encode(int id) {
  if (id < 0) return nullptr;
  void *handle = reinterpret_cast<void *>(static_cast<uintptr_t>(id) + 1);
  if (g_domain) (void)g_domain->object_set_handle(id, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(handle)));
  return handle;
}
void *encode_static(int id, StaticQueue_t *storage) {
  if (id < 0 || !storage) return nullptr;
  for (auto &entry : static_handles) {
    if (!entry.used) {
      std::memset(storage, 0, sizeof(*storage));
      entry = {storage, id, true};
      last_mutex_handle = storage;
      if (g_domain) (void)g_domain->object_set_handle(id, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(storage)));
      return storage;
    }
  }
  return nullptr;
}
void *encode_mutex(int id) {
  if (id < 0) return nullptr;
  for (size_t i = 0; i < 32; ++i) {
    if (dynamic_mutex_storage_used[i]) continue;
    auto *storage = &dynamic_mutex_storage[i];
    void *handle = encode_static(id, storage);
    if (!handle) return nullptr;
    dynamic_mutex_storage_used[i] = true;
    return handle;
  }
  return nullptr;
}
int decode(void *p) {
  if (!p) return -1;
  for (const auto &entry : static_handles)
    if (entry.used && entry.storage == p) return entry.object_id;
  return static_cast<int>(reinterpret_cast<uintptr_t>(p) - 1);
}
void forget_static(int id) {
  for (auto &entry : static_handles) {
    if (!entry.used || entry.object_id != id) continue;
    for (size_t i = 0; i < 32; ++i) {
      if (entry.storage == &dynamic_mutex_storage[i]) dynamic_mutex_storage_used[i] = false;
    }
    entry = {};
  }
}
uint32_t static_handle_count() {
  uint32_t count = 0;
  for (const auto &entry : static_handles) if (entry.used) count = count + 1u;
  return count;
}
uint32_t dynamic_mutex_count() {
  uint32_t count = 0;
  for (bool used : dynamic_mutex_storage_used) if (used) count = count + 1u;
  return count;
}
void note_sync(uint32_t kind, uint32_t reason) {
  if (shizuku_c5_ble_note_sync_alloc) shizuku_c5_ble_note_sync_alloc(kind, reason,
      g_domain ? static_cast<uint32_t>(g_domain->objects_used()) : 0u,
      static_handle_count(), dynamic_mutex_count());
}
enum : uint32_t {
  kWaitNotifyTake = 1, kWaitNotifyWait = 2, kWaitSemaphore = 3,
  kWaitRecursiveMutex = 4, kWaitQueueSemaphore = 5, kWaitQueueReceive = 6,
  kWaitEventGroup = 7, kWaitQueueSend = 8, kWaitTaskDelay = 9,
};
void note_wait(uint32_t kind, const void *handle, TickType_t timeout, bool entering,
               uint32_t result = 0, uint32_t caller_pc = 0) {
  if (!shizuku_c5_ble_note_wait) return;
  shizuku_c5_ble_note_wait(g_domain ? g_domain->current_task() : 0u, kind,
      static_cast<uint32_t>(reinterpret_cast<uintptr_t>(handle)), timeout,
      entering ? 1u : 0u, result);
  if (entering && caller_pc && shizuku_c5_ble_note_wait_callsite)
    shizuku_c5_ble_note_wait_callsite(caller_pc);
}
uint32_t wait_kind_for_object(uint32_t api_kind, int object) {
  const uint32_t kind = g_domain ? g_domain->object_kind_code(object) : 0u;
  return (api_kind << 16) | kind;
}
bool generic_send_or_give(QueueHandle_t q, const void *item, TickType_t timeout,
                          BaseType_t position) {
  if (!g_domain) return false;
  const int handle = decode(q);
  const uint32_t kind = g_domain->object_kind_code(handle);
  // In ESP-IDF v6.1 xSemaphoreGive() is a macro for
  // xQueueGenericSend(handle, NULL, 0, queueSEND_TO_BACK). Controller/IDF
  // objects are built against the real IDF headers, so this wrapped entry
  // point must release semaphore and mutex objects as well as enqueue data.
  if (item == nullptr && timeout == 0 && position == 0 && kind == 3u)
    return g_domain->mutex_give(handle);
  if (item == nullptr && timeout == 0 && position == 0 && kind == 2u)
    return g_domain->semaphore_give(handle);
  return g_domain->queue_send(handle, item, timeout, position == 1);
}
enum : uint32_t {
  kSyncMutex = 1, kSyncSemaphore = 2, kSyncQueue = 3,
  kSyncEventGroup = 4, kSyncTimer = 5,
  kSyncFailNoDomain = 1, kSyncFailIsr = 2, kSyncFailObjectLimit = 3,
  kSyncFailInvalid = 4, kSyncFailStaticHandleTable = 5,
  kSyncFailDynamicMutexPool = 6, kSyncFailUnsupportedCaps = 7,
};
int create_mutex_object(bool recursive, uint32_t creator_pc, uint32_t *reason) {
  if (!g_domain) { *reason = kSyncFailNoDomain; return -1; }
  if (g_domain->isr_context()) { *reason = kSyncFailIsr; return -1; }
  if (g_domain->objects_used() >= shizuku::freertos_compat::kMaxObjects) {
    *reason = kSyncFailObjectLimit;
    return -1;
  }
  const int id = g_domain->create_mutex(recursive);
  if (id < 0) *reason = kSyncFailObjectLimit;
  else (void)g_domain->object_set_creator(id, creator_pc);
  return id;
}
int create_semaphore_object(UBaseType_t limit, UBaseType_t initial, uint32_t creator_pc,
                            uint32_t *reason) {
  if (!g_domain) { *reason = kSyncFailNoDomain; return -1; }
  if (g_domain->isr_context()) { *reason = kSyncFailIsr; return -1; }
  if (!limit || initial > limit) { *reason = kSyncFailInvalid; return -1; }
  if (g_domain->objects_used() >= shizuku::freertos_compat::kMaxObjects) {
    *reason = kSyncFailObjectLimit;
    return -1;
  }
  const int id = g_domain->create_semaphore(limit, initial);
  if (id < 0) *reason = kSyncFailObjectLimit;
  else (void)g_domain->object_set_creator(id, creator_pc);
  return id;
}
int create_queue_object(UBaseType_t capacity, UBaseType_t item_size, uint32_t *reason) {
  if (shizuku_c5_ble_note_queue_shape) shizuku_c5_ble_note_queue_shape(capacity, item_size);
  if (!g_domain) { *reason = kSyncFailNoDomain; return -1; }
  if (g_domain->isr_context()) { *reason = kSyncFailIsr; return -1; }
  if (!capacity || !item_size || capacity > SIZE_MAX / item_size ||
      capacity * item_size > shizuku::freertos_compat::kQueueStorageBytes) {
    *reason = kSyncFailInvalid;
    return -1;
  }
  if (g_domain->objects_used() >= shizuku::freertos_compat::kMaxObjects) {
    *reason = kSyncFailObjectLimit;
    return -1;
  }
  const int id = g_domain->create_queue(capacity, item_size);
  if (id < 0) *reason = kSyncFailObjectLimit;
  return id;
}
int create_event_group_object(uint32_t *reason) {
  if (!g_domain) { *reason = kSyncFailNoDomain; return -1; }
  if (g_domain->isr_context()) { *reason = kSyncFailIsr; return -1; }
  if (g_domain->objects_used() >= shizuku::freertos_compat::kMaxObjects) {
    *reason = kSyncFailObjectLimit;
    return -1;
  }
  const int id = g_domain->create_event_group();
  if (id < 0) *reason = kSyncFailObjectLimit;
  return id;
}
int create_timer_object(TickType_t period, TimerCallbackFunction_t callback,
                        void *timer_id, UBaseType_t auto_reload, uint32_t *reason) {
  if (!g_domain) { *reason = kSyncFailNoDomain; return -1; }
  if (g_domain->isr_context()) { *reason = kSyncFailIsr; return -1; }
  if (!period || !callback) { *reason = kSyncFailInvalid; return -1; }
  if (g_domain->objects_used() >= shizuku::freertos_compat::kMaxObjects) {
    *reason = kSyncFailObjectLimit;
    return -1;
  }
  const int id = g_domain->create_timer(period, auto_reload != pdFALSE, timer_id, callback);
  if (id < 0) *reason = kSyncFailObjectLimit;
  return id;
}
void *create_dynamic_mutex(bool recursive, uint32_t creator_pc) {
  uint32_t reason = 0;
  const int id = create_mutex_object(recursive, creator_pc, &reason);
  void *handle = nullptr;
  if (!reason) {
    if (dynamic_mutex_count() >= 32) reason = kSyncFailDynamicMutexPool;
    else if (static_handle_count() >= 32) reason = kSyncFailStaticHandleTable;
    else {
      handle = encode_mutex(id);
      if (!handle) reason = kSyncFailDynamicMutexPool;
    }
  }
  if (reason && id >= 0) (void)g_domain->object_delete(id);
  note_sync(kSyncMutex, reason);
  return handle;
}
void *create_static_mutex(bool recursive, StaticQueue_t *storage, uint32_t creator_pc) {
  uint32_t reason = 0;
  const int id = create_mutex_object(recursive, creator_pc, &reason);
  void *handle = nullptr;
  if (!reason) {
    if (!storage) reason = kSyncFailInvalid;
    else if (static_handle_count() >= 32) reason = kSyncFailStaticHandleTable;
    else handle = encode_static(id, storage);
    if (!handle && !reason) reason = kSyncFailStaticHandleTable;
  }
  if (reason && id >= 0) (void)g_domain->object_delete(id);
  note_sync(kSyncMutex, reason);
  return handle;
}
uint32_t tid(TaskHandle_t task) {
  const uint32_t raw = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(task));
  return raw == 1u ? 0u : raw; // handle 1 is the compat representation of root task 0.
}
[[maybe_unused]] void fail_closed(uint32_t id) {
  if (shizuku_c5_ble_note_unimplemented) shizuku_c5_ble_note_unimplemented(id);
}
volatile uint32_t suspend_depth = 0;
}

extern "C" BaseType_t shizuku_freertos_compat_bind(void *d, uint32_t handler, uint32_t method,uint32_t tick_hz) {
  if (!d||!tick_hz) return pdFAIL;
  auto *next=static_cast<domain *>(d);
  if(!next->configure_backend(handler,method,tick_hz))return pdFAIL;
  g_domain=next;
  g_scheduler_state=taskSCHEDULER_NOT_STARTED;
  if (shizuku_c5_ble_note_scheduler)
    shizuku_c5_ble_note_scheduler(0, static_cast<uint32_t>(g_scheduler_state));
  return pdPASS;
}
extern "C" void shizuku_freertos_compat_set_scheduler_state(int32_t state) {
  if (!g_domain || (state != taskSCHEDULER_NOT_STARTED &&
                    state != taskSCHEDULER_SUSPENDED &&
                    state != taskSCHEDULER_RUNNING)) return;
  g_scheduler_state = state;
  if (shizuku_c5_ble_note_scheduler)
    shizuku_c5_ble_note_scheduler(0, static_cast<uint32_t>(g_scheduler_state));
}
extern "C" void *shizuku_freertos_compat_last_mutex_handle(void) { return last_mutex_handle; }
extern "C" BaseType_t SHZ_FREERTOS_API(xPortInIsrContext)(void) { return g_domain&&g_domain->isr_context()?pdTRUE:pdFALSE; }
extern "C" void SHZ_FREERTOS_API(portENTER_CRITICAL)(portMUX_TYPE *) {
  const unsigned depth=critical_depth;
  if(g_domain&&depth<16){critical_keys[depth]=g_domain->critical_enter();critical_depth=depth+1;}
}
extern "C" void SHZ_FREERTOS_API(portEXIT_CRITICAL)(portMUX_TYPE *) {
  const unsigned depth=critical_depth;
  if(g_domain&&depth){critical_depth=depth-1;g_domain->critical_exit(critical_keys[depth-1]);}
}
extern "C" void SHZ_FREERTOS_API(portENTER_CRITICAL_ISR)(portMUX_TYPE *m) { SHZ_FREERTOS_API(portENTER_CRITICAL)(m); }
extern "C" void SHZ_FREERTOS_API(portEXIT_CRITICAL_ISR)(portMUX_TYPE *m) { SHZ_FREERTOS_API(portEXIT_CRITICAL)(m); }
extern "C" void SHZ_FREERTOS_API(vPortEnterCritical)(portMUX_TYPE *m) { shizuku_c5_ble_critical_shim_calls = shizuku_c5_ble_critical_shim_calls + 1u; SHZ_FREERTOS_API(portENTER_CRITICAL)(m); }
extern "C" void SHZ_FREERTOS_API(vPortExitCritical)(portMUX_TYPE *m) { SHZ_FREERTOS_API(portEXIT_CRITICAL)(m); }
extern "C" UBaseType_t SHZ_FREERTOS_API(xPortSetInterruptMaskFromISR)(void) {
#if defined(__riscv)
  uintptr_t old = 0;
  asm volatile("csrrc %0, mstatus, %1" : "=r"(old) : "r"(uintptr_t{8}) : "memory");
  return static_cast<UBaseType_t>(old & 8u);
#else
  return 0;
#endif
}
extern "C" void SHZ_FREERTOS_API(vPortClearInterruptMaskFromISR)(UBaseType_t key) {
#if defined(__riscv)
  if (key & 8u) asm volatile("csrs mstatus, %0" : : "r"(uintptr_t{8}) : "memory");
#else
  (void)key;
#endif
}
extern "C" void SHZ_FREERTOS_API(vPortYield)(void) { if (g_domain) g_domain->task_delay(0); }
extern "C" void SHZ_FREERTOS_API(vPortYieldFromISR)(void) { shizuku_freertos_compat_yield_from_isr(pdTRUE); }

extern "C" BaseType_t SHZ_FREERTOS_API(xTaskCreate)(TaskFunction_t f,const char *n,uint32_t s,void *a,UBaseType_t p,TaskHandle_t *h) {
  uint32_t id = 0;
  const bool success = g_domain && g_domain->task_create(f, n, s, a, p, &id);
  if (shizuku_c5_ble_note_task_create)
    shizuku_c5_ble_note_task_create(success ? id : 0u, n,
                                    success ? 1u : 0u, p);
  if (!success) return pdFAIL;
  if (h) *h = reinterpret_cast<void *>(static_cast<uintptr_t>(id));
  return pdPASS;
}
extern "C" BaseType_t SHZ_FREERTOS_API(xTaskCreatePinnedToCore)(TaskFunction_t f,const char *n,uint32_t s,void *a,UBaseType_t p,TaskHandle_t *h,BaseType_t core) {
  if(core!=0 && core!=-1)return pdFAIL;
  return xTaskCreate(f,n,s,a,p,h);
}
extern "C" BaseType_t SHZ_FREERTOS_API(xTaskCreatePinnedToCoreWithCaps)(TaskFunction_t f,const char *n,uint32_t s,void *a,UBaseType_t p,TaskHandle_t *h,BaseType_t core,uint32_t caps) {
  if(caps)return pdFAIL;
  return xTaskCreatePinnedToCore(f,n,s,a,p,h,core);
}
extern "C" void SHZ_FREERTOS_API(vTaskDelay)(TickType_t t) {
  note_wait(kWaitTaskDelay, nullptr, t, true);
  if (g_domain) g_domain->task_delay(t);
  note_wait(kWaitTaskDelay, nullptr, t, false, pdPASS);
}
extern "C" void SHZ_FREERTOS_API(taskYIELD)(void) { if(g_domain)g_domain->task_delay(0); }
extern "C" UBaseType_t SHZ_FREERTOS_API(uxTaskPriorityGet)(TaskHandle_t h) { return g_domain?g_domain->task_priority(h?tid(h):0):0; }
extern "C" TickType_t SHZ_FREERTOS_API(xTaskGetTickCount)(void) { return g_domain?static_cast<TickType_t>(g_domain->tick_count()):0; }
extern "C" TickType_t SHZ_FREERTOS_API(xTaskGetTickCountFromISR)(void) { return g_domain&&g_domain->isr_context()?static_cast<TickType_t>(g_domain->tick_count()):0; }
extern "C" TaskHandle_t SHZ_FREERTOS_API(xTaskGetCurrentTaskHandle)(void) { if(!g_domain)return nullptr; const uint32_t id=g_domain->current_task(); return reinterpret_cast<TaskHandle_t>(static_cast<uintptr_t>(id?id:1)); }
extern "C" BaseType_t SHZ_FREERTOS_API(xTaskGetSchedulerState)(void) {
  const BaseType_t state = g_domain ? g_scheduler_state : taskSCHEDULER_NOT_STARTED;
  if (shizuku_c5_ble_note_scheduler)
    shizuku_c5_ble_note_scheduler(1, static_cast<uint32_t>(state));
  return state;
}
extern "C" void SHZ_FREERTOS_API(vTaskSuspend)(TaskHandle_t h) { if(g_domain)g_domain->task_suspend(h?tid(h):0); }
extern "C" void SHZ_FREERTOS_API(vTaskResume)(TaskHandle_t h) { if(g_domain)g_domain->task_resume(tid(h)); }
extern "C" BaseType_t SHZ_FREERTOS_API(xTaskResumeFromISR)(TaskHandle_t h) { bool hp=false;return g_domain&&g_domain->task_resume(tid(h),true,&hp)&&hp?pdTRUE:pdFALSE; }
extern "C" void SHZ_FREERTOS_API(vTaskDelete)(TaskHandle_t h) { if (g_domain) g_domain->task_delete(h ? tid(h) : 0); }
extern "C" BaseType_t SHZ_FREERTOS_API(xTaskGetCoreID)(TaskHandle_t) { return 0; }
extern "C" TaskHandle_t SHZ_FREERTOS_API(xTaskGetCurrentTaskHandleForCore)(BaseType_t core) {
  return core == 0 ? SHZ_FREERTOS_API(xTaskGetCurrentTaskHandle)() : nullptr;
}
extern "C" void SHZ_FREERTOS_API(vTaskPrioritySet)(TaskHandle_t h, UBaseType_t p) {
  if (g_domain) (void)g_domain->task_set_priority(h ? tid(h) : g_domain->current_task(), p);
}
extern "C" void SHZ_FREERTOS_API(vTaskSuspendAll)(void) {
  if (suspend_depth != UINT32_MAX) {
    suspend_depth = suspend_depth + 1u;
    SHZ_FREERTOS_API(portENTER_CRITICAL)(nullptr);
  }
}
extern "C" BaseType_t SHZ_FREERTOS_API(xTaskResumeAll)(void) {
  if (!suspend_depth) return pdFALSE;
  suspend_depth = suspend_depth - 1u;
  SHZ_FREERTOS_API(portEXIT_CRITICAL)(nullptr);
  return pdFALSE;
}
extern "C" void SHZ_FREERTOS_API(vTaskSetTimeOutState)(TimeOut_t *state) {
  if (!state) return;
  state->xOverflowCount = 0;
  state->xTimeOnEntering = g_domain ? static_cast<TickType_t>(g_domain->tick_count()) : 0;
}
extern "C" void SHZ_FREERTOS_API(vTaskInternalSetTimeOutState)(TimeOut_t *state) { SHZ_FREERTOS_API(vTaskSetTimeOutState)(state); }
extern "C" BaseType_t SHZ_FREERTOS_API(xTaskCheckForTimeOut)(TimeOut_t *state, TickType_t *wait) {
  shizuku_c5_ble_timeout_shim_calls = shizuku_c5_ble_timeout_shim_calls + 1u;
  if (!state || !wait) return pdTRUE;
  if (*wait == portMAX_DELAY) return pdFALSE;
  const TickType_t now = g_domain ? static_cast<TickType_t>(g_domain->tick_count()) : 0;
  const TickType_t elapsed = static_cast<TickType_t>(now - state->xTimeOnEntering);
  if (elapsed >= *wait) { *wait = 0; state->xTimeOnEntering = now; return pdTRUE; }
  *wait -= elapsed;
  state->xTimeOnEntering = now;
  return pdFALSE;
}

extern "C" BaseType_t SHZ_FREERTOS_API(xTaskGenericNotify)(
    TaskHandle_t task, UBaseType_t index, uint32_t value, eNotifyAction action,
    uint32_t *previous) {
  const bool ok = g_domain && task &&
      g_domain->task_notify(tid(task), index, value, static_cast<uint32_t>(action), previous);
  if (shizuku_c5_ble_note_notification) shizuku_c5_ble_note_notification(1, ok);
  return ok ? pdPASS : pdFAIL;
}
extern "C" BaseType_t SHZ_FREERTOS_API(xTaskGenericNotifyFromISR)(
    TaskHandle_t task, UBaseType_t index, uint32_t value, eNotifyAction action,
    uint32_t *previous, BaseType_t *higher) {
  bool wake = false;
  const bool ok = g_domain && task &&
      g_domain->task_notify(tid(task), index, value, static_cast<uint32_t>(action), previous, true, &wake);
  if (higher && wake) *higher = pdTRUE;
  if (shizuku_c5_ble_note_notification) shizuku_c5_ble_note_notification(2, ok);
  return ok ? pdPASS : pdFAIL;
}
extern "C" BaseType_t SHZ_FREERTOS_API(xTaskGenericNotifyWait)(
    UBaseType_t index, uint32_t clear_entry, uint32_t clear_exit,
    uint32_t *value, TickType_t timeout) {
  note_wait(kWaitNotifyWait, nullptr, timeout, true);
  const bool ok = g_domain && g_domain->task_notify_wait(index, clear_entry, clear_exit, value, timeout);
  note_wait(kWaitNotifyWait, nullptr, timeout, false, ok ? pdPASS : pdFAIL);
  if (shizuku_c5_ble_note_notification) shizuku_c5_ble_note_notification(3, ok);
  return ok ? pdPASS : pdFAIL;
}
extern "C" uint32_t SHZ_FREERTOS_API(ulTaskGenericNotifyTake)(
    UBaseType_t index, BaseType_t clear_on_exit, TickType_t timeout) {
  note_wait(kWaitNotifyTake, nullptr, timeout, true);
  const uint32_t value = g_domain ? g_domain->task_notify_take(index, clear_on_exit != pdFALSE, timeout) : 0;
  note_wait(kWaitNotifyTake, nullptr, timeout, false, value);
  if (shizuku_c5_ble_note_notification) shizuku_c5_ble_note_notification(4, value != 0);
  return value;
}
extern "C" void SHZ_FREERTOS_API(vTaskGenericNotifyGiveFromISR)(
    TaskHandle_t task, UBaseType_t index, BaseType_t *higher) {
  bool wake = false;
  const bool ok = g_domain && task &&
      g_domain->task_notify(tid(task), index, 0, static_cast<uint32_t>(eIncrement), nullptr, true, &wake);
  if (higher && wake) *higher = pdTRUE;
  if (shizuku_c5_ble_note_notification) shizuku_c5_ble_note_notification(5, ok);
}
extern "C" BaseType_t SHZ_FREERTOS_API(xTaskGenericNotifyStateClear)(
    TaskHandle_t task, UBaseType_t index) {
  const bool ok = g_domain && g_domain->task_notify_state_clear(task ? tid(task) : g_domain->current_task(), index);
  if (shizuku_c5_ble_note_notification) shizuku_c5_ble_note_notification(6, ok);
  return ok ? pdPASS : pdFAIL;
}
extern "C" uint32_t SHZ_FREERTOS_API(ulTaskGenericNotifyValueClear)(
    TaskHandle_t task, UBaseType_t index, uint32_t bits) {
  const uint32_t value = g_domain
      ? g_domain->task_notify_value_clear(task ? tid(task) : g_domain->current_task(), index, bits)
      : 0u;
  if (shizuku_c5_ble_note_notification) shizuku_c5_ble_note_notification(7, value != 0);
  return value;
}

extern "C" QueueHandle_t SHZ_FREERTOS_API(xQueueCreate)(UBaseType_t n,UBaseType_t sz) {
  uint32_t reason = 0;
  const int id = create_queue_object(n, sz, &reason);
  const auto handle = encode(id);
  note_sync(kSyncQueue, reason);
  return handle;
}
extern "C" QueueHandle_t SHZ_FREERTOS_API(xQueueGenericCreate)(UBaseType_t n,UBaseType_t sz,uint8_t) { return SHZ_FREERTOS_API(xQueueCreate)(n,sz); }
extern "C" QueueHandle_t SHZ_FREERTOS_API(xQueueGenericCreateStatic)(UBaseType_t n,UBaseType_t sz,uint8_t *,StaticQueue_t *storage,uint8_t) {
  uint32_t reason = 0;
  const int id = create_queue_object(n, sz, &reason);
  void *handle = nullptr;
  if (!reason) {
    if (!storage) reason = kSyncFailInvalid;
    else if (static_handle_count() >= 32) reason = kSyncFailStaticHandleTable;
    else handle = encode_static(id, storage);
    if (!handle && !reason) reason = kSyncFailStaticHandleTable;
  }
  if (reason && id >= 0) (void)g_domain->object_delete(id);
  note_sync(kSyncQueue, reason);
  return handle;
}
extern "C" QueueHandle_t SHZ_FREERTOS_API(xQueueCreateWithCaps)(UBaseType_t n,UBaseType_t sz,uint32_t caps) {
  if (caps) { note_sync(kSyncQueue, kSyncFailUnsupportedCaps); return nullptr; }
  return SHZ_FREERTOS_API(xQueueCreate)(n,sz);
}
extern "C" QueueHandle_t SHZ_FREERTOS_API(xQueueCreateCountingSemaphore)(UBaseType_t max,UBaseType_t initial) {
  uint32_t reason = 0;
  const uint32_t creator_pc = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
  const int id = create_semaphore_object(max, initial, creator_pc, &reason);
  const auto handle = encode(id);
  note_sync(kSyncSemaphore, reason);
  return handle;
}
extern "C" QueueHandle_t SHZ_FREERTOS_API(xQueueCreateCountingSemaphoreStatic)(UBaseType_t max,UBaseType_t initial,StaticQueue_t *storage) {
  uint32_t reason = 0;
  const uint32_t creator_pc = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
  const int id = create_semaphore_object(max, initial, creator_pc, &reason);
  void *handle = nullptr;
  if (!reason) {
    if (!storage) reason = kSyncFailInvalid;
    else if (static_handle_count() >= 32) reason = kSyncFailStaticHandleTable;
    else handle = encode_static(id, storage);
    if (!handle && !reason) reason = kSyncFailStaticHandleTable;
  }
  if (reason && id >= 0) (void)g_domain->object_delete(id);
  note_sync(kSyncSemaphore, reason);
  return handle;
}
extern "C" QueueHandle_t SHZ_FREERTOS_API(xQueueCreateMutex)(uint8_t type) {
  const uint32_t creator_pc = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
  return create_dynamic_mutex(type == 4u, creator_pc);
}
extern "C" QueueHandle_t SHZ_FREERTOS_API(xQueueCreateMutexStatic)(uint8_t type,StaticQueue_t *storage) {
  const uint32_t creator_pc = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
  return create_static_mutex(type == 4u, storage, creator_pc);
}
extern "C" BaseType_t SHZ_FREERTOS_API(xQueueSend)(QueueHandle_t q,const void *p,TickType_t t) {
  const uint32_t wait_kind = wait_kind_for_object(kWaitQueueSend, decode(q));
  note_wait(wait_kind, q, t, true);
  const bool ok = generic_send_or_give(q,p,t,0);
  note_wait(wait_kind, q, t, false, ok ? pdPASS : pdFAIL);
  return ok ? pdTRUE : pdFALSE;
}
extern "C" BaseType_t SHZ_FREERTOS_API(xQueueGenericSend)(QueueHandle_t q,const void *p,TickType_t t,BaseType_t position) {
  const uint32_t wait_kind = wait_kind_for_object(kWaitQueueSend, decode(q));
  note_wait(wait_kind, q, t, true);
  const bool ok = generic_send_or_give(q,p,t,position);
  note_wait(wait_kind, q, t, false, ok ? pdPASS : pdFAIL);
  return ok ? pdTRUE : pdFALSE;
}
extern "C" BaseType_t SHZ_FREERTOS_API(xQueueGenericSendFromISR)(QueueHandle_t q,const void *p,BaseType_t *w,BaseType_t position) { bool hp=false;const bool ok=g_domain&&g_domain->queue_send(decode(q),p,0,position==1,true,&hp);if(w&&hp)*w=pdTRUE;return ok?pdTRUE:pdFALSE; }
extern "C" BaseType_t SHZ_FREERTOS_API(xQueueSendToBack)(QueueHandle_t q,const void *p,TickType_t t) { return SHZ_FREERTOS_API(xQueueSend)(q,p,t); }
extern "C" BaseType_t SHZ_FREERTOS_API(xQueueSendToFront)(QueueHandle_t q,const void *p,TickType_t t) {
  const uint32_t wait_kind = wait_kind_for_object(kWaitQueueSend, decode(q));
  note_wait(wait_kind, q, t, true);
  const bool ok = generic_send_or_give(q,p,t,1);
  note_wait(wait_kind, q, t, false, ok ? pdPASS : pdFAIL);
  return ok ? pdTRUE : pdFALSE;
}
extern "C" BaseType_t SHZ_FREERTOS_API(xQueueSendFromISR)(QueueHandle_t q,const void *p,BaseType_t *w) { bool hp=false;auto ok=g_domain&&g_domain->queue_send(decode(q),p,0,false,true,&hp);if(w&&hp)*w=pdTRUE;return ok?pdTRUE:pdFALSE; }
extern "C" BaseType_t SHZ_FREERTOS_API(xQueueSendToBackFromISR)(QueueHandle_t q,const void *p,BaseType_t *w) { return SHZ_FREERTOS_API(xQueueSendFromISR)(q,p,w); }
extern "C" BaseType_t SHZ_FREERTOS_API(xQueueSendToFrontFromISR)(QueueHandle_t q,const void *p,BaseType_t *w) { bool hp=false;auto ok=g_domain&&g_domain->queue_send(decode(q),p,0,true,true,&hp);if(w&&hp)*w=pdTRUE;return ok?pdTRUE:pdFALSE; }
extern "C" BaseType_t SHZ_FREERTOS_API(xQueueReceive)(QueueHandle_t q,void *p,TickType_t t) {
  const uint32_t wait_kind = wait_kind_for_object(kWaitQueueReceive, decode(q));
  note_wait(wait_kind, q, t, true);
  const bool ok = g_domain && g_domain->queue_receive(decode(q),p,t);
  note_wait(wait_kind, q, t, false, ok ? pdPASS : pdFAIL);
  return ok ? pdTRUE : pdFALSE;
}
extern "C" BaseType_t SHZ_FREERTOS_API(xQueueReceiveFromISR)(QueueHandle_t q,void *p,BaseType_t *w) { bool hp=false;auto ok=g_domain&&g_domain->queue_receive(decode(q),p,0,true,&hp);if(w&&hp)*w=pdTRUE;return ok?pdTRUE:pdFALSE; }
extern "C" UBaseType_t SHZ_FREERTOS_API(uxQueueMessagesWaiting)(QueueHandle_t q) { return g_domain?g_domain->queue_messages(decode(q)):0; }
extern "C" UBaseType_t SHZ_FREERTOS_API(uxQueueMessagesWaitingFromISR)(QueueHandle_t q) { return g_domain&&g_domain->isr_context()?g_domain->queue_messages(decode(q)):0; }
extern "C" UBaseType_t SHZ_FREERTOS_API(uxQueueSpacesAvailable)(QueueHandle_t q) { return g_domain?g_domain->queue_spaces(decode(q)):0; }
// IDF v6.1's NimBLE NPL calls this from task context too. The upstream
// implementation is a nonblocking queue-state read and does not require ISR
// context; preserving that behavior is required by eventq_is_empty().
extern "C" BaseType_t SHZ_FREERTOS_API(xQueueIsQueueEmptyFromISR)(QueueHandle_t q) { return g_domain&&g_domain->queue_empty(decode(q))?pdTRUE:pdFALSE; }
extern "C" BaseType_t SHZ_FREERTOS_API(xQueueGenericReset)(QueueHandle_t q,BaseType_t) { return g_domain&&g_domain->queue_reset(decode(q))?pdPASS:pdFAIL; }
extern "C" void SHZ_FREERTOS_API(vQueueDelete)(QueueHandle_t q) { const int id=decode(q); if(g_domain&&g_domain->object_delete(id))forget_static(id); }
extern "C" BaseType_t SHZ_FREERTOS_API(xQueueGiveFromISR)(QueueHandle_t q,BaseType_t *w) { bool hp=false;const bool ok=g_domain&&g_domain->semaphore_give(decode(q),true,&hp);if(w&&hp)*w=pdTRUE;return ok?pdTRUE:pdFALSE; }
extern "C" BaseType_t SHZ_FREERTOS_API(xQueueGiveMutexRecursive)(QueueHandle_t q) { return g_domain&&g_domain->mutex_give(decode(q))?pdTRUE:pdFALSE; }
extern "C" BaseType_t SHZ_FREERTOS_API(xQueueTakeMutexRecursive)(QueueHandle_t q,TickType_t t) {
  const uint32_t wait_kind = wait_kind_for_object(kWaitRecursiveMutex, decode(q));
  note_wait(wait_kind, q, t, true);
  const bool ok = g_domain && g_domain->mutex_take(decode(q),t,false,true);
  note_wait(wait_kind, q, t, false, ok ? pdPASS : pdFAIL);
  return ok ? pdTRUE : pdFALSE;
}
extern "C" TaskHandle_t SHZ_FREERTOS_API(xQueueGetMutexHolder)(QueueHandle_t q) { return g_domain?reinterpret_cast<TaskHandle_t>(static_cast<uintptr_t>(g_domain->mutex_owner(decode(q)))):nullptr; }

extern "C" SemaphoreHandle_t SHZ_FREERTOS_API(xSemaphoreCreateBinary)(void) {
  uint32_t reason = 0;
  const uint32_t creator_pc = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
  const int id = create_semaphore_object(1, 0, creator_pc, &reason);
  const auto handle = encode(id);
  note_sync(kSyncSemaphore, reason);
  return handle;
}
extern "C" SemaphoreHandle_t SHZ_FREERTOS_API(xSemaphoreCreateCounting)(UBaseType_t n,UBaseType_t init) {
  uint32_t reason = 0;
  const uint32_t creator_pc = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
  const int id = create_semaphore_object(n, init, creator_pc, &reason);
  const auto handle = encode(id);
  note_sync(kSyncSemaphore, reason);
  return handle;
}
extern "C" SemaphoreHandle_t SHZ_FREERTOS_API(xSemaphoreCreateMutex)(void) {
  const uint32_t creator_pc = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
  return create_dynamic_mutex(false, creator_pc);
}
extern "C" SemaphoreHandle_t SHZ_FREERTOS_API(xSemaphoreCreateRecursiveMutex)(void) {
  const uint32_t creator_pc = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
  return create_dynamic_mutex(true, creator_pc);
}
extern "C" BaseType_t SHZ_FREERTOS_API(xSemaphoreTake)(SemaphoreHandle_t s,TickType_t t) {
  const uint32_t wait_kind = wait_kind_for_object(kWaitSemaphore, decode(s));
  note_wait(wait_kind, s, t, true);
  const int h=decode(s);
  const bool ok = g_domain&&(g_domain->mutex_take(h,t,false,false)||g_domain->semaphore_take(h,t));
  note_wait(wait_kind, s, t, false, ok ? pdPASS : pdFAIL);
  return ok?pdTRUE:pdFALSE;
}
extern "C" BaseType_t SHZ_FREERTOS_API(xQueueSemaphoreTake)(QueueHandle_t q,TickType_t t) {
  shizuku_c5_ble_queue_sem_shim_calls = shizuku_c5_ble_queue_sem_shim_calls + 1u;
  const uint32_t caller_pc = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
  const uint32_t wait_kind = wait_kind_for_object(kWaitQueueSemaphore, decode(q));
  note_wait(wait_kind, q, t, true, 0, caller_pc);
  const int h = decode(q);
  const bool ok = g_domain && (g_domain->mutex_take(h,t,false,false) || g_domain->semaphore_take(h,t));
  note_wait(wait_kind, q, t, false, ok ? pdPASS : pdFAIL, caller_pc);
  return ok ? pdTRUE : pdFALSE;
}
extern "C" BaseType_t SHZ_FREERTOS_API(xSemaphoreTakeRecursive)(SemaphoreHandle_t s,TickType_t t) {
  const uint32_t wait_kind = wait_kind_for_object(kWaitRecursiveMutex, decode(s));
  note_wait(wait_kind, s, t, true);
  const bool ok = g_domain && g_domain->mutex_take(decode(s),t,false,true);
  note_wait(wait_kind, s, t, false, ok ? pdPASS : pdFAIL);
  return ok?pdTRUE:pdFALSE;
}
extern "C" BaseType_t SHZ_FREERTOS_API(xSemaphoreTakeFromISR)(SemaphoreHandle_t s,BaseType_t *) { return g_domain&&g_domain->semaphore_take(decode(s),0,true)?pdTRUE:pdFALSE; }
extern "C" BaseType_t SHZ_FREERTOS_API(xSemaphoreGive)(SemaphoreHandle_t s) { int h=decode(s); return g_domain&&(g_domain->mutex_give(h)||g_domain->semaphore_give(h))?pdTRUE:pdFALSE; }
extern "C" BaseType_t SHZ_FREERTOS_API(xSemaphoreGiveRecursive)(SemaphoreHandle_t s) { return g_domain&&g_domain->mutex_give(decode(s))?pdTRUE:pdFALSE; }
extern "C" BaseType_t SHZ_FREERTOS_API(xSemaphoreGiveFromISR)(SemaphoreHandle_t s,BaseType_t *w) { bool hp=false;auto ok=g_domain&&g_domain->semaphore_give(decode(s),true,&hp);if(w&&hp)*w=pdTRUE;return ok?pdTRUE:pdFALSE; }
extern "C" UBaseType_t SHZ_FREERTOS_API(uxSemaphoreGetCount)(SemaphoreHandle_t s) { return g_domain?g_domain->semaphore_count(decode(s)):0; }

// Fail closed until these object kinds have concrete storage and wake semantics.
extern "C" EventGroupHandle_t SHZ_FREERTOS_API(xEventGroupCreate)(void) {
  uint32_t reason = 0;
  const int id = create_event_group_object(&reason);
  const auto handle = encode(id);
  note_sync(kSyncEventGroup, reason);
  return handle;
}
extern "C" EventBits_t SHZ_FREERTOS_API(xEventGroupSetBits)(EventGroupHandle_t h,EventBits_t b){return g_domain?g_domain->event_group_set(decode(h),b):0;}
extern "C" EventBits_t SHZ_FREERTOS_API(xEventGroupClearBits)(EventGroupHandle_t h,EventBits_t b){return g_domain?g_domain->event_group_clear(decode(h),b):0;}
extern "C" EventBits_t SHZ_FREERTOS_API(xEventGroupWaitBits)(EventGroupHandle_t h,EventBits_t b,BaseType_t clear,BaseType_t all,TickType_t t) {
  const uint32_t wait_kind = wait_kind_for_object(kWaitEventGroup, decode(h));
  note_wait(wait_kind, h, t, true);
  const EventBits_t result = g_domain ? g_domain->event_group_wait(decode(h),b,clear!=pdFALSE,all!=pdFALSE,t) : 0;
  note_wait(wait_kind, h, t, false, result);
  return result;
}
extern "C" TimerHandle_t SHZ_FREERTOS_API(xTimerCreate)(const char*,TickType_t p,UBaseType_t reload,void *id,TimerCallbackFunction_t cb) {
  uint32_t reason = 0;
  const int object = create_timer_object(p, cb, id, reload, &reason);
  const auto handle = encode(object);
  note_sync(kSyncTimer, reason);
  return handle;
}
extern "C" BaseType_t SHZ_FREERTOS_API(xTimerStart)(TimerHandle_t h,TickType_t){return g_domain&&g_domain->timer_start(decode(h))?pdPASS:pdFAIL;}
extern "C" BaseType_t SHZ_FREERTOS_API(xTimerStartFromISR)(TimerHandle_t h,BaseType_t *w){if(w)*w=pdFALSE;return g_domain&&g_domain->timer_start(decode(h),true)?pdPASS:pdFAIL;}
extern "C" BaseType_t SHZ_FREERTOS_API(xTimerStop)(TimerHandle_t h,TickType_t){return g_domain&&g_domain->timer_stop(decode(h))?pdPASS:pdFAIL;}
extern "C" BaseType_t SHZ_FREERTOS_API(xTimerReset)(TimerHandle_t h,TickType_t){return g_domain&&g_domain->timer_start(decode(h))?pdPASS:pdFAIL;}
extern "C" BaseType_t SHZ_FREERTOS_API(xTimerDelete)(TimerHandle_t h,TickType_t){return g_domain&&g_domain->timer_delete(decode(h))?pdPASS:pdFAIL;}
extern "C" BaseType_t SHZ_FREERTOS_API(xTimerChangePeriod)(TimerHandle_t h,TickType_t p,TickType_t){return g_domain&&g_domain->timer_change_period(decode(h),p)?pdPASS:pdFAIL;}
extern "C" BaseType_t SHZ_FREERTOS_API(xTimerResetFromISR)(TimerHandle_t h,BaseType_t *w){if(w)*w=pdFALSE;return g_domain&&g_domain->timer_start(decode(h),true)?pdPASS:pdFAIL;}
extern "C" BaseType_t SHZ_FREERTOS_API(xTimerStopFromISR)(TimerHandle_t h,BaseType_t *w){if(w)*w=pdFALSE;return g_domain&&g_domain->timer_stop(decode(h),true)?pdPASS:pdFAIL;}
extern "C" BaseType_t SHZ_FREERTOS_API(xTimerChangePeriodFromISR)(TimerHandle_t h,TickType_t p,BaseType_t *w){if(w)*w=pdFALSE;return g_domain&&g_domain->timer_change_period(decode(h),p,true)?pdPASS:pdFAIL;}
extern "C" TickType_t SHZ_FREERTOS_API(xTimerGetExpiryTime)(TimerHandle_t h){return g_domain?g_domain->timer_expiry(decode(h)):0;}
extern "C" BaseType_t SHZ_FREERTOS_API(xTimerIsTimerActive)(TimerHandle_t h){return g_domain&&g_domain->timer_active(decode(h))?pdTRUE:pdFALSE;}
extern "C" void *SHZ_FREERTOS_API(pvTimerGetTimerID)(TimerHandle_t h){return g_domain?g_domain->timer_id(decode(h)):nullptr;}

#if defined(SHIZUKU_FREERTOS_LINK_WRAP)
// Stable ids are copied into shizuku_c5_ble_diag.first_unimplemented_api.
enum : uint32_t {
  kApiPcTaskGetName = 1, kApiPvTaskGetTls = 2, kApiUlTaskNotifyTake = 3,
  kApiVListInitialise = 4, kApiVQueueDeleteWithCaps = 5,
  kApiVSemaphoreDeleteWithCaps = 6, kApiVTaskNotifyGiveFromIsr = 7,
  kApiVTaskGetSnapshot = 8, kApiVTaskPlaceOnEventList = 9,
  kApiVTaskSetTlsAndDeleteCallback = 10, kApiXQueueGetMutexHolderFromIsr = 11,
  kApiXSemaphoreCreateGenericWithCaps = 12, kApiXTaskGenericNotify = 13,
  kApiXTaskGenericNotifyWait = 14, kApiXTaskGetIdleTaskHandleForCore = 15,
  kApiXTaskRemoveFromEventList = 16
};
#define SHZ_FAIL_CLOSED(name, id) \
  extern "C" uintptr_t SHZ_FREERTOS_API(name)(...) { fail_closed(id); return 0; }
SHZ_FAIL_CLOSED(pcTaskGetName, kApiPcTaskGetName)
SHZ_FAIL_CLOSED(pvTaskGetThreadLocalStoragePointer, kApiPvTaskGetTls)
SHZ_FAIL_CLOSED(vListInitialise, kApiVListInitialise)
SHZ_FAIL_CLOSED(vQueueDeleteWithCaps, kApiVQueueDeleteWithCaps)
SHZ_FAIL_CLOSED(vSemaphoreDeleteWithCaps, kApiVSemaphoreDeleteWithCaps)
SHZ_FAIL_CLOSED(vTaskGetSnapshot, kApiVTaskGetSnapshot)
SHZ_FAIL_CLOSED(vTaskPlaceOnEventList, kApiVTaskPlaceOnEventList)
SHZ_FAIL_CLOSED(vTaskSetThreadLocalStoragePointerAndDelCallback, kApiVTaskSetTlsAndDeleteCallback)
SHZ_FAIL_CLOSED(xQueueGetMutexHolderFromISR, kApiXQueueGetMutexHolderFromIsr)
SHZ_FAIL_CLOSED(xSemaphoreCreateGenericWithCaps, kApiXSemaphoreCreateGenericWithCaps)
SHZ_FAIL_CLOSED(xTaskGetIdleTaskHandleForCore, kApiXTaskGetIdleTaskHandleForCore)
SHZ_FAIL_CLOSED(xTaskRemoveFromEventList, kApiXTaskRemoveFromEventList)
#undef SHZ_FAIL_CLOSED
#endif
