# Shizuku RP2040 Pico SDK final-link CMake project.
# Links SDK-dependent board/arch glue with SDK-free Shizuku archive to emit UF2.
# CMake 3.24+ is required for LINK_LIBRARY:WHOLE_ARCHIVE support.
cmake_minimum_required(VERSION 3.24)

# 1. Parameter and environment checks
if(NOT DEFINED PICO_SDK_PATH)
  if(DEFINED ENV{PICO_SDK_PATH} AND NOT "$ENV{PICO_SDK_PATH}" STREQUAL "")
    set(PICO_SDK_PATH "$ENV{PICO_SDK_PATH}")
  else()
    message(FATAL_ERROR "PICO_SDK_PATH is not set. Specify -DPICO_SDK_PATH or set PICO_SDK_PATH in environment.")
  endif()
endif()

if(NOT EXISTS "${PICO_SDK_PATH}/pico_sdk_init.cmake")
  message(FATAL_ERROR "PICO_SDK_PATH does not point to a valid Pico SDK: '${PICO_SDK_PATH}'")
endif()

# Enforce fail-closed platform constraint: RP2040 only (RP2350 is follow-up)
if(NOT DEFINED PICO_PLATFORM)
  set(PICO_PLATFORM "rp2040")
endif()
if(NOT PICO_PLATFORM STREQUAL "rp2040")
  message(FATAL_ERROR "Unsupported PICO_PLATFORM '${PICO_PLATFORM}'. Only 'rp2040' is currently supported. RP2350 must not be declared supported.")
endif()

if(NOT DEFINED PICO_BOARD)
  set(PICO_BOARD "pico")
endif()
if(NOT PICO_BOARD MATCHES "^(pico|pico_w)$")
  message(FATAL_ERROR "Unsupported PICO_BOARD '${PICO_BOARD}'. Supported boards: pico, pico_w.")
endif()

if(NOT DEFINED SHIZUKU_ARCHIVE OR NOT EXISTS "${SHIZUKU_ARCHIVE}")
  message(FATAL_ERROR "SHIZUKU_ARCHIVE is not specified or does not exist: '${SHIZUKU_ARCHIVE}'")
endif()
if(NOT DEFINED SHIZUKU_CONFIG_HEADER OR NOT EXISTS "${SHIZUKU_CONFIG_HEADER}")
  message(FATAL_ERROR "SHIZUKU_CONFIG_HEADER is not specified or does not exist: '${SHIZUKU_CONFIG_HEADER}'")
endif()

# Locate host picotool if available
if(NOT DEFINED picotool_DIR AND EXISTS "/opt/picotool/lib/cmake/picotool")
  set(picotool_DIR "/opt/picotool/lib/cmake/picotool" CACHE PATH "Path to picotool CMake directory")
endif()

# 2. Include Pico SDK
include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(shizuku_rp2040 C CXX ASM)
set(SHIZUKU_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../..")
get_filename_component(SHIZUKU_CONFIG_SHIZUKU_DIR "${SHIZUKU_CONFIG_HEADER}" DIRECTORY)
get_filename_component(SHIZUKU_CONFIG_INCLUDE_ROOT "${SHIZUKU_CONFIG_SHIZUKU_DIR}" DIRECTORY)
include_directories("${SHIZUKU_REPO_ROOT}/modules/pico_sdk_support")

# Ensure C++23, -Og, -g3
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_options(-Og -g3 -DSHIZUKU_RP2040=1)

# Initialize Pico SDK
pico_sdk_init()

# 3. Target definition
add_executable(shizuku_rp2040
  shizuku_rp2040_boot.cpp
  ${SHIZUKU_REPO_ROOT}/modules/pico_sdk_support/rp2040/arch_glue_v6m.cpp
  ${SHIZUKU_REPO_ROOT}/boards/rp2040_pico_w/board_rp2040.cpp
  ${SHIZUKU_REPO_ROOT}/modules/pico_sdk_support/objects/usb_cdc.cpp
)

target_include_directories(shizuku_rp2040 PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}
  ${SHIZUKU_CONFIG_INCLUDE_ROOT}
  ${SHIZUKU_REPO_ROOT}/internal_headers
  ${SHIZUKU_REPO_ROOT}
  ${SHIZUKU_REPO_ROOT}/modules/pico_sdk_support
  ${SHIZUKU_REPO_ROOT}/modules/pico_sdk_support/objects/internal_headers
)

# GNU ld WHOLE_ARCHIVE configuration
foreach(lang C CXX)
  set(CMAKE_${lang}_LINK_LIBRARY_USING_WHOLE_ARCHIVE "-Wl,--whole-archive" "<LIBRARY>" "-Wl,--no-whole-archive")
  set(CMAKE_${lang}_LINK_LIBRARY_USING_WHOLE_ARCHIVE_SUPPORTED TRUE)
endforeach()

add_library(shizuku_archive STATIC IMPORTED GLOBAL)
set_target_properties(shizuku_archive PROPERTIES IMPORTED_LOCATION "${SHIZUKU_ARCHIVE}")

target_link_libraries(shizuku_rp2040 PRIVATE
  pico_stdlib
  hardware_clocks
  hardware_exception
  hardware_irq
  hardware_sync
  hardware_timer
  hardware_watchdog
  hardware_dma
  pico_multicore
  pico_bootrom
  pico_unique_id
  pico_usb_reset_interface_headers
  tinyusb_device
  tinyusb_board
  "$<LINK_LIBRARY:WHOLE_ARCHIVE,shizuku_archive>"
)

pico_enable_stdio_usb(shizuku_rp2040 0)
set(SHIZUKU_RP2040_FLASH_LIMIT_LD "${CMAKE_CURRENT_LIST_DIR}/rp2040_flash_limit.ld")
if(NOT EXISTS "${SHIZUKU_RP2040_FLASH_LIMIT_LD}")
  set(SHIZUKU_RP2040_FLASH_LIMIT_LD "${SHIZUKU_REPO_ROOT}/firmware/rp2040_flash_limit.ld")
endif()
target_link_options(shizuku_rp2040 PRIVATE "-Wl,-T,${SHIZUKU_RP2040_FLASH_LIMIT_LD}")
target_link_options(shizuku_rp2040 PRIVATE "-Wl,-Map,${CMAKE_BINARY_DIR}/shizuku_rp2040.map")

# 4. Extract symbols from archive and fail-closed if invalid
if(CMAKE_C_COMPILER MATCHES "gcc\\.exe$")
  string(REGEX REPLACE "gcc\\.exe$" "nm.exe" SHIZUKU_NM "${CMAKE_C_COMPILER}")
elseif(CMAKE_C_COMPILER MATCHES "gcc$")
  string(REGEX REPLACE "gcc$" "nm" SHIZUKU_NM "${CMAKE_C_COMPILER}")
else()
  find_program(SHIZUKU_NM arm-none-eabi-nm)
endif()

if(NOT EXISTS "${SHIZUKU_NM}")
  message(FATAL_ERROR "nm tool not found for ${CMAKE_C_COMPILER}: cannot scan archive roots")
endif()

execute_process(
  COMMAND "${SHIZUKU_NM}" -g --defined-only -P "${SHIZUKU_ARCHIVE}"
  OUTPUT_VARIABLE SHIZUKU_NM_OUT
  RESULT_VARIABLE SHIZUKU_NM_RC
)
if(NOT SHIZUKU_NM_RC EQUAL 0)
  message(FATAL_ERROR "nm scan failed on archive '${SHIZUKU_ARCHIVE}' (exit code: ${SHIZUKU_NM_RC})")
endif()

# Check for required canonical entrypoint symbol
if(NOT "${SHIZUKU_NM_OUT}" MATCHES "(^|\n)shizuku_firmware_entry [TBDRWV] ")
  message(FATAL_ERROR "SHIZUKU_ARCHIVE '${SHIZUKU_ARCHIVE}' is missing required entrypoint 'shizuku_firmware_entry'")
endif()

string(REGEX MATCHALL "(^|\n)[^ \n]+ [TBDRWV] " SHIZUKU_SYM_LINES "${SHIZUKU_NM_OUT}")
set(SHIZUKU_ROOTS "")
foreach(line ${SHIZUKU_SYM_LINES})
  string(REGEX REPLACE "^\n?([^ ]+) .*" "\\1" sym "${line}")
  list(APPEND SHIZUKU_ROOTS "-Wl,--undefined=${sym}")
endforeach()

list(LENGTH SHIZUKU_ROOTS SHIZUKU_ROOT_COUNT)
if(SHIZUKU_ROOT_COUNT LESS 5)
  message(FATAL_ERROR "Archive contains too few public roots (${SHIZUKU_ROOT_COUNT}): archive appears nearly empty")
endif()

message(STATUS "Shizuku archive roots: ${SHIZUKU_ROOT_COUNT} symbol(s)")
target_link_options(shizuku_rp2040 PRIVATE ${SHIZUKU_ROOTS})

# 5. Generate UF2 output via SDK CMake
pico_add_extra_outputs(shizuku_rp2040)
