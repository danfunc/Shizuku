cmake_minimum_required(VERSION 3.26)
project(pico_sdk_support C CXX ASM)
add_library(pico_sdk_support)
target_sources(pico_sdk_support
    PRIVATE
    main.cpp
    cpu_driver.cpp
)

target_link_libraries(Shizuku PRIVATE pico_sdk_support)
target_link_libraries(pico_sdk_support PUBLIC pico_stdlib)
target_link_libraries(pico_sdk_support PRIVATE hardware_interp)
target_link_libraries(pico_sdk_support PUBLIC Shizuku_HEADERS)
target_include_directories(pico_sdk_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/interface)