cmake_minimum_required(VERSION 3.26)
set(CMAKE_CXX_STANDARD "23")
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/buildUtilities.cmake)
preloadtoolchain()
# ★-fPIE/-fPIC は付けない。ベアメタルのファームでは意味が無い上に、PIC だと関数の
# アドレスがリンク時定数でなくなり、asm へシンボルを即値として埋められなくなる
# (ハンドラの ABI シムがコンパイルできない)。Bazel 側も付けていない。
project(Shizuku C CXX ASM)

# ---------------------------------------------------------------------------
#  CYW43 (無線チップ) との gSPI クロック
# ---------------------------------------------------------------------------
#  ★分周比を直書きしない。参照実装が踏んだ罠そのもの: SDK の
#    CYW43_PIO_CLOCK_DIV_INT は **2 固定** で clk_sys から算出しないので、
#    clk_sys を上げると SPI も一緒に上がってチップが黙る (実測: 300MHz/div2 =
#    SCK 75MHz で無応答。div4 = 37.5MHz で復帰)。逆に 4 を直書きすると
#    150MHz のとき半速になる。**目標 SCK を宣言して div を導出する**。
#  ★SCK = clk_sys / (div * 2)。2 が余分に見えるが、PIO のプログラムが 1 ビットに
#    2 命令使う (out + jmp で side_set が SCK を上げ下げする) ため。
#    参照実装の「SPI target 75MHz」は**この 2 を含まない SM クロック**なので、
#    実際の SCK は 37.5MHz。名前に釣られて 2 倍で見積もらないこと。
#  ★上限は CYW43439 の gSPI 定格。既知の実測点は 37.5MHz = 可 / 75MHz = 不可 の
#    2 点しかないので、その間を上げるなら**実測してから**にする。
set(SHIZUKU_SYS_CLK_KHZ 150000 CACHE STRING
    "clk_sys [kHz]。今はクロックを設定していないので SDK 既定の値を書いてある")
set(SHIZUKU_CYW43_SCK_KHZ 37500 CACHE STRING
    "CYW43 gSPI の目標 SCK [kHz] (分周比ではなく周波数を宣言する)")
math(EXPR SHIZUKU_CYW43_PIO_DIV
     "(${SHIZUKU_SYS_CLK_KHZ} + 2 * ${SHIZUKU_CYW43_SCK_KHZ} - 1) / (2 * ${SHIZUKU_CYW43_SCK_KHZ})")
if(SHIZUKU_CYW43_PIO_DIV LESS 1)
  set(SHIZUKU_CYW43_PIO_DIV 1)
endif()
math(EXPR SHIZUKU_CYW43_SCK_ACTUAL_KHZ
     "${SHIZUKU_SYS_CLK_KHZ} / (2 * ${SHIZUKU_CYW43_PIO_DIV})")
message(STATUS
        "CYW43 gSPI: clk_sys ${SHIZUKU_SYS_CLK_KHZ} kHz / div ${SHIZUKU_CYW43_PIO_DIV} "
        "-> SCK ${SHIZUKU_CYW43_SCK_ACTUAL_KHZ} kHz (target ${SHIZUKU_CYW43_SCK_KHZ})")
#  ★★分周比は**自由なつまみではない**。SDK は PIO プログラムの種類を固定で選んで
#    おり (`spi_gap01_sample0` = "for high cpu speed" / `spi_gap0_sample1` =
#    "for lower cpu speed")、この 2 つは**応答をサンプルする位置**が違う。位置は
#    SM サイクル単位なので、分周比を変えるとサンプル点が実時間でずれる。
#    実測 (2026-08-20): div=2 のまま SCK を 18.75MHz へ落としたら
#    `Failed to read test pattern` でチップが上がらなかった。速くしても遅くしても
#    壊れる = 分周比とプログラムは**対で選ぶもの**。
#    参照実装が言う「固定分周の連鎖」と同じ構図で、今回固定されていたのは
#    分周比ではなく**プログラムの選択**のほうだった。
if(NOT SHIZUKU_CYW43_SPI_PROGRAM)
  if(SHIZUKU_CYW43_PIO_DIV GREATER 2)
    set(SHIZUKU_CYW43_SPI_PROGRAM spi_gap0_sample1)
  else()
    set(SHIZUKU_CYW43_SPI_PROGRAM spi_gap01_sample0)
  endif()
endif()
message(STATUS "CYW43 gSPI PIO program: ${SHIZUKU_CYW43_SPI_PROGRAM}")
# ★pico_sdk_init より前に置く必要がある (SDK 側のドライバをコンパイルするため)。
add_compile_definitions(CYW43_PIO_CLOCK_DIV_INT=${SHIZUKU_CYW43_PIO_DIV})
add_compile_definitions(CYW43_SPI_PROGRAM_NAME=${SHIZUKU_CYW43_SPI_PROGRAM})
add_compile_definitions(SHIZUKU_CYW43_SCK_KHZ=${SHIZUKU_CYW43_SCK_ACTUAL_KHZ})
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
