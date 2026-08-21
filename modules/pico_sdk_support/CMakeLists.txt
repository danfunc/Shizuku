shizuku_declare_objects(pico_sdk_support gpio spi led flashfs)
cmake_minimum_required(VERSION 3.26)
project(pico_sdk_support C CXX ASM)
# ★型の継ぎ目 (arch / board / memory manager のクラス) と、その**実装**を分ける。
#   カーネルが必要とするのは前者だけ (テンプレート引数として食う) で、後者は
#   カーネルと**相互に**呼び合う: 例外の入口はカーネルへ入っていき、カーネルは
#   BOARD::panic / diag_printf を呼び返す。この循環は設計上の事実なので、隠さずに
#   宣言する (静的ライブラリ間の循環は CMake が解決してくれる)。
add_library(pico_sdk_support_headers INTERFACE)
target_include_directories(pico_sdk_support_headers INTERFACE
                           ${CMAKE_CURRENT_SOURCE_DIR}/internal_headers)
target_link_libraries(pico_sdk_support_headers INTERFACE Shizuku_internal_HEADERS
                                                         pico_stdlib pico_stdio)

add_library(pico_sdk_support)
target_sources(pico_sdk_support PRIVATE board.cpp arch_glue.cpp
                                        memory_manager.cpp objects/peripherals.cpp
                                        objects/flash_fs.cpp
                                        armv8m_ctx.S)
target_compile_definitions(pico_sdk_support PUBLIC "$<$<CONFIG:DEBUG>:-DSHIZUKU_DEBUG>")
set(config_need_headers
    "shizuku/archs/armv8m.hpp;shizuku/boards/rp2350_pico2.hpp;shizuku/memory_managers/pico_sdk.hpp;shizuku/app_entry.hpp;${config_need_headers}"
    CACHE INTERNAL "")
target_link_libraries(pico_sdk_support PUBLIC pico_stdlib pico_stdio hardware_exception
                                             hardware_gpio hardware_spi hardware_flash hardware_sync)
# pico2_w は LED が無線チップ側にあるので、LED オブジェクトが cyw43_arch を使う。
# (lwIP も BTstack も要らないので _none。ボードが pico2_w のときだけリンクする)
if(PICO_CYW43_SUPPORTED)
  target_link_libraries(pico_sdk_support PUBLIC pico_cyw43_arch_none)
endif()
target_link_libraries(pico_sdk_support PUBLIC pico_sdk_support_headers shizuku_kernel)
