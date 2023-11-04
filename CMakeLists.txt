cmake_minimum_required(VERSION 3.26)
include(${CMAKE_CURRENT_SOURCE_DIR}/toolchains/pico_sdk.cmake)
project(shizuku C CXX ASM)
add_subdirectory(config)
add_subdirectory(kernel)
add_subdirectory(image)

