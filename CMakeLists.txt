cmake_minimum_required(VERSION 3.26)
include(load_toolchain.cmake)
project(shizuku C CXX ASM)
add_subdirectory(config)
add_subdirectory(kernel)
add_subdirectory(image)

