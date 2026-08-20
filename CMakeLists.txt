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
add_subdirectory(firmware)
# target_precompile_headers(Shizuku PRIVATE
# "$<$<COMPILE_LANGUAGE:CXX>:${CMAKE_CURRENT_SOURCE_DIR}/internal_headers/shizuku/kernel.hpp>")

# ---------------------------------------------------------------------------
#  書き込み
# ---------------------------------------------------------------------------
#   cmake --build build --target flash   … ビルドしてから焼く (こちらが普段使い)
#   cmake --install build                … 焼くだけ
#
#  picotool は BOOTSEL で挿したデバイスにも、-f で「動いているファームを再起動させて」
#  も書ける (このファームは USB CDC を出しているのでリセット要求を受けられる)。
#  ★動いているファームが固まっていると、そのリセット要求を処理できず**書かずに
#    終わる**ことがある (参照実装 HANDOFF §7 で実際に切り分けを誤った罠)。下の
#    コマンドは終了コードを見て失敗を必ず表に出す。それでも怪しいときは BOOTSEL を
#    押しながら挿し直すこと。
find_program(PICOTOOL_EXECUTABLE picotool
             HINTS /opt/picotool/bin $ENV{HOME}/.pico-sdk/picotool/bin
                   $ENV{PICO_SDK_PATH}/../picotool/bin
             DOC "Raspberry Pi picotool (書き込み用)")
set(SHIZUKU_UF2 "${CMAKE_CURRENT_BINARY_DIR}/Shizuku.uf2")
# -x 書いたら実行 / -v 書き込みを検証 / -u 同一セクタは飛ばす / -f 動作中でも再起動
set(SHIZUKU_FLASH_ARGS load -x -v -u -f "${SHIZUKU_UF2}")

if(PICOTOOL_EXECUTABLE)
  add_custom_target(flash
                    COMMAND "${PICOTOOL_EXECUTABLE}" ${SHIZUKU_FLASH_ARGS}
                    USES_TERMINAL
                    COMMENT "picotool でファームを書き込んで実行する")
  add_dependencies(flash Shizuku)

  install(CODE "
    message(STATUS \"picotool load: ${SHIZUKU_UF2}\")
    execute_process(COMMAND \"${PICOTOOL_EXECUTABLE}\" ${SHIZUKU_FLASH_ARGS}
                    RESULT_VARIABLE shizuku_flash_result)
    if(NOT shizuku_flash_result EQUAL 0)
      message(FATAL_ERROR
              \"書き込みに失敗した (終了コード \${shizuku_flash_result})。\\n\"
              \"  ・デバイスが見つからない/固まっている場合は BOOTSEL を押しながら挿し直す\\n\"
              \"  ・書き込み後に自己テストの出力が出ない場合も、まず本当に書けたかを疑うこと\")
    endif()
  ")
else()
  message(WARNING "picotool が見つからないので flash / install は使えない "
                  "(-DPICOTOOL_EXECUTABLE=/path/to/picotool で指定できる)")
endif()
