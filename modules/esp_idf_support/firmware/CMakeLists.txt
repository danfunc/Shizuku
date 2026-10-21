# ESP-IDF v6.1 / ESP32-C6 の archive-link project。
# ★Shizuku のカーネルは Bazel が作る libshizuku_kernel.a (SHIZUKU_KERNEL_ARCHIVE) を
#   取り込むだけ。ここでカーネルは再コンパイルしない。起動経路は ROM pre-init entry
#   までで、KERNEL::init / scheduler は呼ばない (main/shizuku_boot.c を参照)。
cmake_minimum_required(VERSION 3.24)

if(NOT DEFINED SHIZUKU_KERNEL_ARCHIVE OR NOT EXISTS "${SHIZUKU_KERNEL_ARCHIVE}")
  message(FATAL_ERROR "SHIZUKU_KERNEL_ARCHIVE が未指定、または存在しない: '${SHIZUKU_KERNEL_ARCHIVE}'")
endif()
if(NOT "$ENV{IDF_PATH}" STREQUAL "" AND NOT EXISTS "$ENV{IDF_PATH}/tools/cmake/project.cmake")
  message(FATAL_ERROR "IDF_PATH が ESP-IDF を指していない: $ENV{IDF_PATH}")
endif()
# IDF target は build.sh が -DIDF_TARGET で渡す。未指定は従来どおり esp32c6。
if(NOT DEFINED IDF_TARGET)
  set(IDF_TARGET esp32c6)
endif()
if(IDF_TARGET STREQUAL "esp32c5")
  if(SHIZUKU_ESP_BLE OR SHIZUKU_C5_BLE)
    set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults.esp32c5.ble")
  else()
    set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults.esp32c5")
  endif()
else()
  if(SHIZUKU_ESP_BLE OR SHIZUKU_C5_BLE)
    if("$ENV{SHIZUKU_ESP_OTA}" STREQUAL "1")
      set(SHIZUKU_ESP_OTA_PROFILE ON)
      set(SDKCONFIG_DEFAULTS
          "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults.esp32c6.ble;${CMAKE_CURRENT_LIST_DIR}/esp32c6_ota_defaults.conf")
      list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../ble_ota")
    else()
      set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults.esp32c6.ble")
    endif()
  else()
    set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")
  endif()
endif()

# GNU ld 用の WHOLE_ARCHIVE を定義する (CMake 標準では Apple ホストの GNU ld に無い)。
foreach(lang C CXX)
  set(CMAKE_${lang}_LINK_LIBRARY_USING_WHOLE_ARCHIVE "-Wl,--whole-archive" "<LIBRARY>" "-Wl,--no-whole-archive")
  set(CMAKE_${lang}_LINK_LIBRARY_USING_WHOLE_ARCHIVE_SUPPORTED TRUE)
endforeach()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(shizuku_${IDF_TARGET})

if(NOT "${IDF_VER}" MATCHES "^v6\\.1")
  message(FATAL_ERROR "ESP-IDF v6.1 が必要。検出: '${IDF_VER}'")
endif()

if(SHIZUKU_ESP_OTA_PROFILE)
  idf_component_get_property(shizuku_main_lib main COMPONENT_LIB)
  idf_component_get_property(shizuku_ota_lib ble_ota COMPONENT_LIB)
  if(NOT shizuku_main_lib OR NOT shizuku_ota_lib)
    message(FATAL_ERROR "Opt-in BLE OTA components were not registered")
  endif()
  target_link_libraries(${shizuku_main_lib} INTERFACE ${shizuku_ota_lib})
  target_link_options(${shizuku_main_lib} INTERFACE
      "-Wl,--undefined=shizuku_esp_ota_register_service"
      "-Wl,--undefined=shizuku_esp_ota_gap_event"
      "-Wl,--undefined=shizuku_esp_ota_mark_running_app_valid")
endif()
