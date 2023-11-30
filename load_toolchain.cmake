
if(DEFINED ENV{SHIZUKU_TOOLCHAIN})
  if(EXISTS
     ${CMAKE_CURRENT_SOURCE_DIR}/toolchains/$ENV{SHIZUKU_TOOLCHAIN}.cmake)
    include(
      ${CMAKE_CURRENT_SOURCE_DIR}/toolchains/$ENV{SHIZUKU_TOOLCHAIN}.cmake)
  endif()
elseif(DEFINED SHIZUKU_TOOLCHAIN)
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/toolchains/${SHIZUKU_TOOLCHAIN}.cmake)
    include(${CMAKE_CURRENT_SOURCE_DIR}/toolchains/${SHIZUKU_TOOLCHAIN}.cmake)
  endif()
elseif(DEFINED CMAKE_TOOLCHAIN_FILE)
  if(EXISTS ${CMAKE_TOOLCHAIN_FILE})
    include(${CMAKE_TOOLCHAIN_FILE})
  endif()
  set(CMAKE_TOOLCHAIN_FILE "")
  message(reset_cmake_toolchain_file)
endif()
