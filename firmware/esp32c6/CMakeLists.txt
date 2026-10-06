# ESP-IDF v6.1 / ESP32-C6 の **compile/link-only** プロジェクト。
# ★Shizuku のカーネルは Bazel が作る libshizuku_kernel.a (SHIZUKU_KERNEL_ARCHIVE) を
#   取り込むだけ。ここでカーネルは再コンパイルしない。起動してもカーネルは動かない
#   (main/shizuku_boot.c と ports/esp32/c6/link_stubs.cpp を参照)。
cmake_minimum_required(VERSION 3.24)

if(NOT DEFINED SHIZUKU_KERNEL_ARCHIVE OR NOT EXISTS "${SHIZUKU_KERNEL_ARCHIVE}")
  message(FATAL_ERROR "SHIZUKU_KERNEL_ARCHIVE が未指定、または存在しない: '${SHIZUKU_KERNEL_ARCHIVE}'")
endif()
if(NOT "$ENV{IDF_PATH}" STREQUAL "" AND NOT EXISTS "$ENV{IDF_PATH}/tools/cmake/project.cmake")
  message(FATAL_ERROR "IDF_PATH が ESP-IDF を指していない: $ENV{IDF_PATH}")
endif()
set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")
set(IDF_TARGET esp32c6)

# GNU ld 用の WHOLE_ARCHIVE を定義する (CMake 標準では Apple ホストの GNU ld に無い)。
foreach(lang C CXX)
  set(CMAKE_${lang}_LINK_LIBRARY_USING_WHOLE_ARCHIVE "-Wl,--whole-archive" "<LIBRARY>" "-Wl,--no-whole-archive")
  set(CMAKE_${lang}_LINK_LIBRARY_USING_WHOLE_ARCHIVE_SUPPORTED TRUE)
endforeach()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(shizuku_esp32c6)

if(NOT "${IDF_VER}" MATCHES "^v6\\.1")
  message(FATAL_ERROR "ESP-IDF v6.1 が必要。検出: '${IDF_VER}'")
endif()
