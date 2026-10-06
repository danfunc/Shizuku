idf_component_register(SRCS "shizuku_boot.c")

# ★カーネルのアーカイブを「リンクした」だけでは中身が入らない。2 段の落とし穴がある:
#   1. 通常のリンクは、参照されないメンバーを抽出しない → WHOLE_ARCHIVE で全部抽出する。
#   2. 抽出しても --gc-sections が、起動経路から参照されないセクションを捨てる
#      (shim は意図的にカーネルを呼ばない) → アーカイブの全公開定義を
#      --undefined= で GC のルートにして、コードを実際にイメージへ残す。
#   生の -Wl,--whole-archive は CMake が並べ替えて効かなくなるので使わない。
add_library(shizuku_kernel STATIC IMPORTED GLOBAL)
set_target_properties(shizuku_kernel PROPERTIES IMPORTED_LOCATION "${SHIZUKU_KERNEL_ARCHIVE}")
target_link_libraries(${COMPONENT_LIB} INTERFACE "$<LINK_LIBRARY:WHOLE_ARCHIVE,shizuku_kernel>")

string(REGEX REPLACE "gcc(\\.exe)?$" "nm\\1" SHIZUKU_NM "${CMAKE_C_COMPILER}")
execute_process(
  COMMAND "${SHIZUKU_NM}" -g --defined-only -P "${SHIZUKU_KERNEL_ARCHIVE}"
  OUTPUT_VARIABLE SHIZUKU_NM_OUT RESULT_VARIABLE SHIZUKU_NM_RC)
if(NOT SHIZUKU_NM_RC EQUAL 0)
  message(FATAL_ERROR "nm が失敗: ${SHIZUKU_NM} (rc=${SHIZUKU_NM_RC})")
endif()
string(REGEX MATCHALL "(^|\n)[^ \n]+ [TBDRWV] " SHIZUKU_SYM_LINES "${SHIZUKU_NM_OUT}")
set(SHIZUKU_ROOTS "")
foreach(line ${SHIZUKU_SYM_LINES})
  string(REGEX REPLACE "^\n?([^ ]+) .*" "\\1" sym "${line}")
  list(APPEND SHIZUKU_ROOTS "-Wl,--undefined=${sym}")
endforeach()
list(LENGTH SHIZUKU_ROOTS SHIZUKU_ROOT_COUNT)
if(SHIZUKU_ROOT_COUNT LESS 10)
  message(FATAL_ERROR "アーカイブの公開シンボルが少なすぎる (${SHIZUKU_ROOT_COUNT}): 空に近い")
endif()
message(STATUS "Shizuku kernel archive roots: ${SHIZUKU_ROOT_COUNT} symbols")
target_link_options(${COMPONENT_LIB} INTERFACE ${SHIZUKU_ROOTS})
