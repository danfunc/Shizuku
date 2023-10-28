if(NOT(SHIZUKU_PLATFORM STREQUAL "unknown"))
add_subdirectory(${SHIZUKU_PLATFORM})
endif()
