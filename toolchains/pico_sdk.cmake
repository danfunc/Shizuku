
set(CMAKE_C_COMPILER arm-none-eabi-gcc)
set(CMAKE_CXX_COMPILER arm-none-eabi-g++)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(SHIZUKU_PLATFORM pico_sdk CACHE STRING "shizuku platform name" FORCE)
set(SHIZUKU_PROCESSOR rp2040 CACHE STRING "shizuku platform name" FORCE)
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)