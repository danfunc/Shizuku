add_subdirectory(memory_manager)
target_include_directories(shizuku_kernel PUBLIC include)
target_precompile_headers(
  shizuku_kernel
  PRIVATE
  "$<$<COMPILE_LANGUAGE:CXX>:${CMAKE_CURRENT_SOURCE_DIR}/include/shizuku/platforms/pico_sdk.hpp>"
)
