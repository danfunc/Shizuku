set(SHIZUKU_PLATFORM pico_sdk CACHE STRING "shizuku platform name" FORCE)
set(SHIZUKU_PROCESSOR rp2040 CACHE STRING "shizuku platform name" FORCE)
set(CMAKE_C_COMPILER arm-none-eabi-gcc)
set(CMAKE_CXX_COMPILER arm-none-eabi-g++)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)