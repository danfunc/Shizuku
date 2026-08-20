cmake_minimum_required(VERSION 3.26)
project(pico_sdk_support C CXX ASM)
add_library(pico_sdk_support)
target_sources(pico_sdk_support PRIVATE main.cpp board.cpp arch_glue.cpp
                                        memory_manager.cpp objects/peripherals.cpp
                                        objects/flash_fs.cpp
                                        armv8m_ctx.S)
target_compile_definitions(pico_sdk_support PUBLIC "$<$<CONFIG:DEBUG>:-DSHIZUKU_DEBUG>")
set(config_need_headers
    "shizuku/archs/armv8m.hpp;shizuku/boards/rp2350_pico2.hpp;shizuku/memory_managers/pico_sdk.hpp;shizuku/app_entry.hpp;${config_need_headers}"
    CACHE INTERNAL "")
target_link_libraries(Shizuku PRIVATE pico_sdk_support pico_stdio pico_stdlib)
target_link_libraries(pico_sdk_support PUBLIC pico_stdlib pico_stdio hardware_exception
                                             hardware_gpio hardware_spi hardware_flash hardware_sync)
# pico2_w は LED が無線チップ側にあるので、LED オブジェクトが cyw43_arch を使う。
# (lwIP も BTstack も要らないので _none。ボードが pico2_w のときだけリンクする)
if(PICO_CYW43_SUPPORTED)
  target_link_libraries(pico_sdk_support PUBLIC pico_cyw43_arch_none)
endif()
target_link_libraries(pico_sdk_support PUBLIC Shizuku_internal_HEADERS)
target_include_directories(pico_sdk_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/internal_headers)
pico_enable_stdio_usb(Shizuku 1)
# ★起動直後の診断はホストがポートを開く前に流れてしまう (参照実装も同じ罠を踏んで
#   いる: PORT §7「焼く前にシリアルキャプチャを張る」)。ホストの接続を少しだけ
#   待つことで、自己テストの結果が**必ず**見えるようにする。
target_compile_definitions(Shizuku PRIVATE PICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS=2000)
