if(DEFINED ENV{SHIZUKU_TOOLCHAIN})
    include(${CMAKE_CURRENT_SOURCE_DIR}/toolchains/$ENV{SHIZUKU_TOOLCHAIN}.cmake)
    message(loadchain)
endif()

