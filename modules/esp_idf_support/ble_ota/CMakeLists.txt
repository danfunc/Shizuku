idf_component_register(
    SRCS "esp_flash_backend.cpp" "esp_ble_service.cpp" "auth_gate.cpp" "status_format.cpp" "protocol.cpp"
    INCLUDE_DIRS "."
    REQUIRES app_update esp_partition bt esp_system freertos)

if("$ENV{SHIZUKU_ESP_OTA_TEST_WRITES}" STREQUAL "1")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SHIZUKU_ESP_OTA_TEST_WRITES=1)
endif()
