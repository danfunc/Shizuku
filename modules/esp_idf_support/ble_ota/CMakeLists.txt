set(shizuku_ota_sources "${SHIZUKU_ESP_OTA_SOURCES}")
set(shizuku_ota_include_dirs
    "."
    "${CMAKE_CURRENT_LIST_DIR}/../../../internal_headers"
    "${CMAKE_CURRENT_LIST_DIR}/../../../modules/pico_sdk_support/objects/internal_headers")
if(SHIZUKU_ESP_OTA_INCLUDE_DIRS)
    list(APPEND shizuku_ota_include_dirs ${SHIZUKU_ESP_OTA_INCLUDE_DIRS})
endif()
if(CMAKE_BUILD_EARLY_EXPANSION)
    # ESP-IDF 6.1 sets this while component_get_requirements.cmake includes
    # this file in a separate CMake -P process. That process restores tracked
    # IDF build properties, not the caller's arbitrary -D cache variables.
    # Only REQUIRES are consumed in this pass; the normal configure below
    # still requires and validates XNO's external inputs before registration.
    idf_component_register(REQUIRES app_update esp_partition bt esp_system freertos)
else()
    if(NOT SHIZUKU_ESP_OTA_SOURCES)
        message(FATAL_ERROR "BLE OTA requires external XNO sources (SHIZUKU_ESP_OTA_SOURCES)")
    endif()
    if(NOT SHIZUKU_ESP_OTA_INCLUDE_DIRS)
        message(FATAL_ERROR "BLE OTA requires external XNO include dirs (SHIZUKU_ESP_OTA_INCLUDE_DIRS)")
    endif()
    foreach(source IN LISTS shizuku_ota_sources)
        if(IS_ABSOLUTE "${source}")
            set(source_path "${source}")
        else()
            set(source_path "${CMAKE_CURRENT_LIST_DIR}/${source}")
        endif()
        if(NOT EXISTS "${source_path}")
            message(FATAL_ERROR "BLE OTA source does not exist: '${source}'")
        endif()
    endforeach()
    foreach(include_dir IN LISTS shizuku_ota_include_dirs)
        if(IS_ABSOLUTE "${include_dir}")
            set(include_path "${include_dir}")
        else()
            set(include_path "${CMAKE_CURRENT_LIST_DIR}/${include_dir}")
        endif()
        if(NOT IS_DIRECTORY "${include_path}")
            message(FATAL_ERROR "BLE OTA include directory does not exist: '${include_dir}'")
        endif()
    endforeach()

    idf_component_register(
        SRCS "esp_flash_backend.cpp" "esp_ble_service.cpp" ${shizuku_ota_sources}
        INCLUDE_DIRS ${shizuku_ota_include_dirs}
        REQUIRES app_update esp_partition bt esp_system freertos)
    if("$ENV{SHIZUKU_ESP_OTA_TEST_WRITES}" STREQUAL "1")
        target_compile_definitions(${COMPONENT_LIB} PRIVATE SHIZUKU_ESP_OTA_TEST_WRITES=1)
    endif()
endif()
