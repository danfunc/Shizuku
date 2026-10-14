idf_component_register(
    SRCS "esp_flash_backend.cpp" "esp_ble_service.cpp" "auth_gate.cpp" "status_format.cpp" "protocol.cpp"
    INCLUDE_DIRS "."
    REQUIRES app_update esp_partition bt)
