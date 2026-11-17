cmake_minimum_required(VERSION 3.24)

# Plain ESP-IDF FreeRTOS IPC cost benchmark for ESP32-C6. No Shizuku sources.
set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/frtos-ipc-c6.defaults")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(shizuku_esp32c6_frtos_ipc)

if(NOT "${IDF_VER}" MATCHES "^v6\\.1")
  message(FATAL_ERROR "ESP-IDF v6.1 is required; detected '${IDF_VER}'")
endif()
