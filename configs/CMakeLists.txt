# 構成変数の既定値 (kit や -D で上書きできる)。ここに既定を置くことで、
# 変数を渡し忘れても configure が壊れずに済む。
if(NOT SHIZUKU_THREAD_COUNT)
  set(SHIZUKU_THREAD_COUNT 32)
endif()
if(NOT SHIZUKU_CPU_COUNT)
  set(SHIZUKU_CPU_COUNT 1)
endif()
if(NOT SHIZUKU_OBJECT_COUNT)
  # ★キャッシュへ置く。オブジェクト番号の生成 (トップの
  #   shizuku_generate_object_ids) が上限の検査に使うので、この下位ディレクトリの
  #   外からも見えている必要がある。既定値を 2 か所に書かないための置き方。
  set(SHIZUKU_OBJECT_COUNT 32 CACHE INTERNAL "オブジェクト表の大きさ")
endif()
if(NOT SHIZUKU_METHOD_COUNT)
  set(SHIZUKU_METHOD_COUNT 8)
endif()
if(NOT SHIZUKU_CALL_DEPTH)
  set(SHIZUKU_CALL_DEPTH 32)
endif()

set(INCLUDE_HEADERS_INSTRUCTION "")
set(config_need_headers)

foreach(header_path IN LISTS config_need_headers)
  if(header_path)
    set(INCLUDE_HEADERS_INSTRUCTION "${INCLUDE_HEADERS_INSTRUCTION}#include <${header_path}>\n")
  endif()
endforeach()

configure_file(config_template.hpp.in ${CMAKE_CURRENT_BINARY_DIR}/internal_headers/shizuku/config.hpp)
target_include_directories(Shizuku_internal_HEADERS INTERFACE ${CMAKE_CURRENT_BINARY_DIR}/internal_headers)
