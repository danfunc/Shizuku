
set(SHIZUKU_PLATFORM "unknown" CACHE STRING "shizuku platform name")
set(SHIZUKU_PROCESSOR "unknown" CACHE STRING "shizuku processor name")
if(NOT (SHIZUKU_PLATFORM STREQUAL "unknown"))
add_subdirectory(${SHIZUKU_PLATFORM})
endif()
message(SHIZUKU_PLATFORM:${SHIZUKU_PLATFORM})
message(SHIZUKU_PROCESSOR:${SHIZUKU_PROCESSOR})