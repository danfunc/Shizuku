cmake_minimum_required(VERSION 3.26)
project(pico_sdk_support C CXX ASM)
add_library(pico_sdk_support)
target_sources(pico_sdk_support PRIVATE main.cpp board.cpp memory_manager.cpp
                                        armv8m_ctx.S)
target_compile_definitions(pico_sdk_support PUBLIC "$<$<CONFIG:DEBUG>:-DSHIZUKU_DEBUG>")
set(config_need_headers
    "shizuku/archs/armv8m.hpp;shizuku/boards/rp2350_pico2.hpp;shizuku/abis/rp2040_abi.hpp;shizuku/memory_managers/pico_sdk.hpp;shizuku/app_entry.hpp;${config_need_headers}"
    CACHE INTERNAL "")
target_link_libraries(Shizuku PRIVATE pico_sdk_support pico_stdio pico_stdlib)
target_link_libraries(pico_sdk_support PUBLIC pico_stdlib pico_stdio hardware_exception)
target_link_libraries(pico_sdk_support PUBLIC Shizuku_internal_HEADERS)
target_include_directories(pico_sdk_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/internal_headers)
pico_enable_stdio_usb(Shizuku 1)
