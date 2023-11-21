add_library(shizuku_kernel)
target_include_directories(
  shizuku_kernel
  PRIVATE
  interface)
add_subdirectory(config)
add_subdirectory(lib)
add_subdirectory(src)
add_subdirectory(test)
add_library(shizuku_interface INTERFACE)
target_include_directories(
  shizuku_interface
  INTERFACE
  interface)
target_link_libraries(shizuku_kernel INTERFACE shizuku_interface)
