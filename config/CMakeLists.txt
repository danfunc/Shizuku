set(SHIZUKU_PLATFORM
    "unknown"
    CACHE STRING "shizuku platform name")
set(SHIZUKU_PROCESSOR
    "unknown"
    CACHE STRING "shizuku processor name")
set(SHIZUKU_CONFIG
    "unknown"
    CACHE STRING "shizuku configuration file")
if(SHIZUKU_PLATFORM STREQUAL "unknown")
  message(SEND_ERROR SHIZUKU_PLATFORM is unknown)
else()
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${SHIZUKU_PLATFORM}/CMakeLists.txt)
    add_subdirectory(${SHIZUKU_PLATFORM})
  else()
    message(SEND_ERROR "SHIZUKU_PLATFORM configuration directory not found")
  endif()
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${SHIZUKU_CONFIG})
  include(${SHIZUKU_CONFIG})
else()
  message(
    "[[warning]]::${SHIZUKU_CONFIG} dose not exist on config dir instead to use default_conf.cmake"
  )
  include(default_conf.cmake)
endif()

message(SHIZUKU_PLATFORM:${SHIZUKU_PLATFORM})
message(SHIZUKU_PROCESSOR:${SHIZUKU_PROCESSOR})
