if(SHIZUKU_C5_BLE OR "$ENV{SHIZUKU_C5_BLE}" STREQUAL "1")
  set(shizuku_c5_ble_extra_srcs "")
  if("$ENV{SHIZUKU_C5_LIBC_LOCK_DIAG_WRAP}" STREQUAL "1")
    list(APPEND shizuku_c5_ble_extra_srcs "../../radio/c5_libc_lock_diag.cpp")
  endif()
  idf_component_register(
      SRCS "shizuku_boot.c" "usb_console.c" "early_diag.c"
           "c5_ble_stage1.cpp"
           "../../radio/c5_intr_alloc.cpp"
           ${shizuku_c5_ble_extra_srcs}
           "../../freertos_compat/compat.cpp"
           "../../freertos_compat/freertos_api.cpp"
           "../../freertos_compat/shizuku_port.cpp"
      PRIV_REQUIRES esp_hal_usb esp_hal_wdt esp_rom soc heap bt esp_system esp_libc esp_timer nvs_flash log esp_hw_support esp_app_format)
else()
  idf_component_register(SRCS "shizuku_boot.c" "usb_console.c" "early_diag.c"
                         PRIV_REQUIRES esp_hal_usb esp_hal_wdt esp_rom soc)
endif()
if(SHIZUKU_C5_BLE OR "$ENV{SHIZUKU_C5_BLE}" STREQUAL "1")
  target_compile_definitions(${COMPONENT_LIB} PRIVATE SHIZUKU_C5_BLE=1 SHIZUKU_FREERTOS_COMPAT_TINY=1 SHIZUKU_FREERTOS_LINK_WRAP=1)
  if("$ENV{SHIZUKU_C5_MUTEX_SELF_RETAKE_FAILFAST}" STREQUAL "1")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SHIZUKU_C5_MUTEX_SELF_RETAKE_FAILFAST=1)
  endif()
  target_include_directories(${COMPONENT_LIB} BEFORE PRIVATE
      "${CMAKE_CURRENT_LIST_DIR}/../../freertos_compat/include"
      "${CMAKE_CURRENT_LIST_DIR}/../../freertos_compat"
      "${CMAKE_CURRENT_LIST_DIR}/../../radio"
      "${CMAKE_CURRENT_LIST_DIR}/../../../../internal_headers")
  target_include_directories(${COMPONENT_LIB} PRIVATE
      "${CMAKE_CURRENT_LIST_DIR}/../../../../chips/esp32_c5")
  target_link_options(${COMPONENT_LIB} INTERFACE
      "-Wl,--wrap=esp_intr_alloc" "-Wl,--wrap=esp_intr_free"
      "-Wl,--wrap=esp_intr_enable" "-Wl,--wrap=esp_intr_disable")
  target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=heap_caps_malloc")
  target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--undefined=shizuku_c5_ble_stage1_start")
  target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,-Map=${CMAKE_BINARY_DIR}/shizuku_esp32c5.map")
  set(shizuku_freertos_wraps
      xPortInIsrContext portENTER_CRITICAL portEXIT_CRITICAL portENTER_CRITICAL_ISR portEXIT_CRITICAL_ISR
      vPortEnterCritical vPortExitCritical vPortClearInterruptMaskFromISR
      vPortYield vPortYieldFromISR xPortSetInterruptMaskFromISR
      vTaskSetTimeOutState vTaskInternalSetTimeOutState xTaskCheckForTimeOut xQueueSemaphoreTake
      pcTaskGetName pvTaskGetThreadLocalStoragePointer ulTaskGenericNotifyTake
      uxQueueMessagesWaitingFromISR uxQueueSpacesAvailable vListInitialise
      vQueueDelete vQueueDeleteWithCaps vSemaphoreDeleteWithCaps vTaskDelete
      vTaskGenericNotifyGiveFromISR vTaskGetSnapshot vTaskPlaceOnEventList
      vTaskPrioritySet vTaskSetThreadLocalStoragePointerAndDelCallback vTaskSuspendAll
      xQueueCreateCountingSemaphore xQueueCreateCountingSemaphoreStatic
      xQueueCreateMutex xQueueCreateMutexStatic xQueueGenericCreate
      xQueueGenericCreateStatic xQueueGenericReset xQueueGenericSend
      xQueueGenericSendFromISR xQueueGetMutexHolder xQueueGetMutexHolderFromISR
      xQueueGiveFromISR xQueueGiveMutexRecursive xQueueTakeMutexRecursive
      xSemaphoreCreateGenericWithCaps xTaskGenericNotify xTaskGenericNotifyWait
      xTaskGetCoreID xTaskGetCurrentTaskHandleForCore xTaskGetIdleTaskHandleForCore
      xTaskRemoveFromEventList xTaskResumeAll xTaskGenericNotifyFromISR
      xTaskGenericNotifyStateClear
      ulTaskGenericNotifyValueClear
      xTaskCreate xTaskCreatePinnedToCore xTaskCreatePinnedToCoreWithCaps vTaskDelay taskYIELD
      uxTaskPriorityGet xTaskGetTickCount xTaskGetTickCountFromISR xTaskGetCurrentTaskHandle
      xTaskGetSchedulerState vTaskSuspend vTaskResume xTaskResumeFromISR
      xQueueCreate xQueueCreateWithCaps xQueueSend xQueueSendToBack xQueueSendToFront
      xQueueSendFromISR xQueueSendToBackFromISR xQueueSendToFrontFromISR xQueueReceive
      xQueueReceiveFromISR uxQueueMessagesWaiting xQueueIsQueueEmptyFromISR
      xSemaphoreCreateBinary xSemaphoreCreateCounting xSemaphoreCreateMutex
      xSemaphoreCreateRecursiveMutex xSemaphoreTake xSemaphoreTakeRecursive
      xSemaphoreTakeFromISR xSemaphoreGive xSemaphoreGiveRecursive xSemaphoreGiveFromISR
      uxSemaphoreGetCount xEventGroupCreate xEventGroupSetBits xEventGroupClearBits
      xEventGroupWaitBits xTimerCreate xTimerStart xTimerStartFromISR xTimerStop
      xTimerReset xTimerDelete xTimerChangePeriod xTimerResetFromISR xTimerStopFromISR
      xTimerChangePeriodFromISR xTimerGetExpiryTime xTimerIsTimerActive pvTimerGetTimerID)
  if("$ENV{SHIZUKU_C5_LIBC_LOCK_DIAG_WRAP}" STREQUAL "1")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SHIZUKU_C5_LIBC_LOCK_DIAG_WRAP=1)
    list(APPEND shizuku_freertos_wraps
        _lock_acquire _lock_acquire_recursive _lock_try_acquire _lock_try_acquire_recursive
        __retarget_lock_acquire __retarget_lock_acquire_recursive
        __retarget_lock_try_acquire __retarget_lock_try_acquire_recursive)
  endif()
  foreach(api IN LISTS shizuku_freertos_wraps)
    target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=${api}")
  endforeach()
endif()

# ★カーネルのアーカイブを「リンクした」だけでは中身が入らない。2 段の落とし穴がある:
#   1. 通常のリンクは、参照されないメンバーを抽出しない → WHOLE_ARCHIVE で全部抽出する。
#   2. 抽出しても --gc-sections が、起動経路から参照されないセクションを捨てる
#      (shim は意図的に KERNEL を呼ばない) → アーカイブの全公開定義を
#      --undefined= で GC のルートにして、コードを実際にイメージへ残す。
#   生の -Wl,--whole-archive は CMake が並べ替えて効かなくなるので使わない。
add_library(shizuku_kernel STATIC IMPORTED GLOBAL)
set_target_properties(shizuku_kernel PROPERTIES IMPORTED_LOCATION "${SHIZUKU_KERNEL_ARCHIVE}")
target_link_libraries(${COMPONENT_LIB} INTERFACE "$<LINK_LIBRARY:WHOLE_ARCHIVE,shizuku_kernel>")

if(CMAKE_C_COMPILER MATCHES "gcc\\.exe$")
  string(REGEX REPLACE "gcc\\.exe$" "nm.exe" SHIZUKU_NM "${CMAKE_C_COMPILER}")
else()
  string(REGEX REPLACE "gcc$" "nm" SHIZUKU_NM "${CMAKE_C_COMPILER}")
endif()
execute_process(
  COMMAND "${SHIZUKU_NM}" -g --defined-only -P "${SHIZUKU_KERNEL_ARCHIVE}"
  OUTPUT_VARIABLE SHIZUKU_NM_OUT RESULT_VARIABLE SHIZUKU_NM_RC)
if(NOT SHIZUKU_NM_RC EQUAL 0)
  message(FATAL_ERROR "nm が失敗: ${SHIZUKU_NM} (rc=${SHIZUKU_NM_RC})")
endif()
string(REGEX MATCHALL "(^|\n)[^ \n]+ [TBDRWV] " SHIZUKU_SYM_LINES "${SHIZUKU_NM_OUT}")
set(SHIZUKU_ROOTS "")
foreach(line ${SHIZUKU_SYM_LINES})
  string(REGEX REPLACE "^\n?([^ ]+) .*" "\\1" sym "${line}")
  list(APPEND SHIZUKU_ROOTS "-Wl,--undefined=${sym}")
endforeach()
list(LENGTH SHIZUKU_ROOTS SHIZUKU_ROOT_COUNT)
if(SHIZUKU_ROOT_COUNT LESS 10)
  message(FATAL_ERROR "アーカイブの公開シンボルが少なすぎる (${SHIZUKU_ROOT_COUNT}): 空に近い")
endif()
message(STATUS "Shizuku kernel archive roots: ${SHIZUKU_ROOT_COUNT} symbols")
target_link_options(${COMPONENT_LIB} INTERFACE ${SHIZUKU_ROOTS})

# early_boot.o is in the later WHOLE_ARCHIVE kernel archive and references this IDF main-component
# helper. Force its extraction from libmain.a and keep it as a GC root, before archive scanning.
target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--undefined=shizuku_c6_usb_console_write_and_flush")
