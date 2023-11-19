add_subdirectory(processors)
add_subdirectory(object_system)
add_subdirectory(scheduler)
target_include_directories(shizuku_kernel PRIVATE include)
target_sources(shizuku_kernel PRIVATE entry.cpp kernel.cpp)

target_precompile_headers(
  shizuku_kernel
  PRIVATE
  "$<$<COMPILE_LANGUAGE:CXX>:${CMAKE_CURRENT_SOURCE_DIR}/include/shizuku/kernel.hpp>"
)
