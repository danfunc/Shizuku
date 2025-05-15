set(INCLUDE_HEADERS_INSTRUCTION "")

foreach(header_path IN LISTS config_need_headers)
    if(header_path)
        set(INCLUDE_HEADERS_INSTRUCTION "${INCLUDE_HEADERS_INSTRUCTION}#include <${header_path}>\n")
    endif()
endforeach()


configure_file(config_template.hpp ${CMAKE_CURRENT_BINARY_DIR}/interface/shizuku/config.hpp)
target_include_directories(${PROJECT_NAME}_HEADERS INTERFACE ${CMAKE_CURRENT_BINARY_DIR}/interface)
