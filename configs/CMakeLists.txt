function(check_config)
if()

endif()

endfunction()



configure_file(config_template.hpp ${CMAKE_CURRENT_BINARY_DIR}/interface/shizuku/config.hpp)
target_include_directories(${PROJECT_NAME}_HEADERS INTERFACE ${CMAKE_CURRENT_BINARY_DIR}/interface)
