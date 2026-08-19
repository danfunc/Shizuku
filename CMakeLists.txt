cmake_minimum_required(VERSION 3.26)
set(CMAKE_CXX_STANDARD "23")
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/buildUtilities.cmake)
preloadtoolchain()
# ★-fPIE/-fPIC は付けない。ベアメタルのファームでは意味が無い上に、PIC だと関数の
# アドレスがリンク時定数でなくなり、asm へシンボルを即値として埋められなくなる
# (ハンドラの ABI シムがコンパイルできない)。Bazel 側も付けていない。
project(Shizuku C CXX ASM)
add_executable(Shizuku)
add_library(Shizuku_internal_HEADERS INTERFACE)
target_include_directories(Shizuku_internal_HEADERS INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/internal_headers")
target_link_libraries(Shizuku PUBLIC Shizuku_internal_HEADERS)
postloadtoolchain()
add_subdirectory(modules)
add_subdirectory(configs)
add_subdirectory(source)
# target_precompile_headers(Shizuku PRIVATE
# "$<$<COMPILE_LANGUAGE:CXX>:${CMAKE_CURRENT_SOURCE_DIR}/internal_headers/shizuku/kernel.hpp>")
install(CODE "execute_process(COMMAND pico-tools flash ${CMAKE_CURRENT_BINARY_DIR}/Shizuku.uf2)")
