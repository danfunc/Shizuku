cmake_minimum_required(VERSION 3.26)
project(shizukuimage C CXX ASM)
add_executable(shizuku main.cpp)
target_link_libraries(shizuku PRIVATE shizuku_kernel pico_multicore)
pico_enable_stdio_usb(shizuku 1)
pico_enable_stdio_uart(shizuku 0)
pico_add_extra_outputs(shizuku)
add_custom_command(
  TARGET shizuku
  POST_BUILD
  COMMAND cp ${CMAKE_CURRENT_BINARY_DIR}/shizuku.uf2 ${CMAKE_BINARY_DIR}/)
