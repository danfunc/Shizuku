cmake_minimum_required(VERSION 3.24)

# Keep the control firmware's exact BLE sdkconfig profile for an A/B image.
set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/idf-reference-c6-ble.defaults")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(shizuku_esp32c6_idf_ref)

if(NOT "${IDF_VER}" MATCHES "^v6\\.1")
  message(FATAL_ERROR "ESP-IDF v6.1 is required; detected '${IDF_VER}'")
endif()
