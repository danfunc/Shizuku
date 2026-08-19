set(INCLUDE_HEADERS_INSTRUCTION "")
set(config_need_headers)

foreach(header_path IN LISTS config_need_headers)
  if(header_path)
    set(INCLUDE_HEADERS_INSTRUCTION "${INCLUDE_HEADERS_INSTRUCTION}#include <${header_path}>\n")
  endif()
endforeach()

configure_file(config_template.hpp.in ${CMAKE_CURRENT_BINARY_DIR}/internal_headers/shizuku/config.hpp)
target_include_directories(Shizuku_internal_HEADERS INTERFACE ${CMAKE_CURRENT_BINARY_DIR}/internal_headers)
