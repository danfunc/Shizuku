# ESP-IDF v6.1 FreeRTOS symbol scan

Source root: `~/github/Shizuku-esp32/.local/esp-idf/components`. A textual scan of `.c`, `.h`, and `.cpp` production sources under `esp_wifi`, `bt`, `esp_phy`, `esp_coex`, `esp_event`, and `esp_netif` excluded paths matching `test` and `host_test`. Counts are call-site occurrences, not a proof of the complete transitive linker set.

## Required symbol families observed

- Tasks: `xTaskCreate` (18), `xTaskCreatePinnedToCore` (29), `xTaskCreatePinnedToCoreWithCaps` (1), `vTaskDelay` (23), `taskYIELD`/task deletion and task metadata require a wider pass.
- Queues: `xQueueCreate` (31), `xQueueCreateWithCaps` (1), `xQueueCreateStatic` (15), `xQueueCreateSet` (2), send/back/front and corresponding ISR variants, `xQueueReceive`, `xQueueReceiveFromISR`.
- Semaphores/mutexes: binary/counting/mutex/recursive creation, static counting/mutex/recursive creation, take/give, recursive take/give, and FromISR take/give.
- Event groups: create, set, clear, wait.
- Task notifications: `ulTaskNotifyTake`, `xTaskNotifyGive`, `xTaskNotifyWait`.
- Software timers: create, change period, reset, stop, delete, expiry query, active query, and `FromISR` reset/stop/change-period.
- Critical sections: `portENTER_CRITICAL`, `portEXIT_CRITICAL`, `_ISR`, and `_SAFE` forms. `_SAFE` also appears in BLE logging sources, including `components/bt/common/ble_log/src/internal_include/ble_log_util.h:56-60`.

Representative source evidence:

- `components/esp_coex/esp32c6/esp_coex_adapter.c:79-98,138-143` counting semaphore construction/take/give and ISR variants.
- `components/esp_event/esp_event.c:575-603,680-682` queue, recursive mutex, task creation, queue receive and mutex take.
- `components/esp_netif/vfs_l2tap/esp_vfs_l2tap.c:99-130,479-492` queue creation/send/receive, binary semaphore, task create, blocking take.
- `components/esp_coex/esp32c6/esp_coex_adapter.c:55-68` ISR and task critical sections.
- Timer and event-group calls occur throughout `esp_wifi` and `bt`; the grouped scan above reports distinct symbol names and their occurrence counts.

## Current compatibility code boundary

`include/freertos/{FreeRTOS,task,queue,semphr,event_groups,timers}.h` remains a partial C surface. `freertos_api.cpp` exports task/queue/semaphore names in those declarations. Event-group and timer names are fail-closed placeholders. Static allocation APIs, queue sets, several commonly used introspection/deletion APIs, and `_SAFE` critical variants remain absent, so this does not yet link the scanned IDF components.

`shizuku_port.{hpp,cpp}` now consumes handler object/method IDs in `shizuku_freertos_compat_bind()`. It maps task start to `SPAWN`, task scheduling to `RUN_FOR`, task yield/wait to `YIELD` and bounded `SLEEP_US` polling, exit to `EXIT_THREAD`, and clock reads to a `CALL_METHOD` request serviced by the exported compatibility dispatcher. The dispatcher needs a board `time_us` callback installed before bind and must be exported by the composition. Wakeups only update the HANDLER's in-memory READY state; no kernel unpark syscall exists. `portYIELD_FROM_ISR` defers rescheduling until a grant/scheduler boundary. This is a single-core C5 prototype, not a verified C5 runtime integration. ISR upcall glue, composition wiring, and a C5 device run remain outstanding.
