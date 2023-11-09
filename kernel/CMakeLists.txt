add_library(shizuku_kernel)
target_include_directories(
  shizuku_kernel
  PUBLIC
  interface)
add_subdirectory(config)
add_subdirectory(lib)
add_subdirectory(src)
