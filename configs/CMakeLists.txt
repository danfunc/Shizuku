# 構成変数の既定値 (kit や -D で上書きできる)。ここに既定を置くことで、
# 変数を渡し忘れても configure が壊れずに済む。
if(NOT SHIZUKU_THREAD_COUNT)
  set(SHIZUKU_THREAD_COUNT 32)
endif()
if(NOT SHIZUKU_CPU_COUNT)
  set(SHIZUKU_CPU_COUNT 1)
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
