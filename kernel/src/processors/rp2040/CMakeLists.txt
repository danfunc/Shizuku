target_include_directories(shizuku_kernel PUBLIC include)
file(GLOB rp2040_cpu_driver_sources *.cpp)
target_sources(
  shizuku_kernel PRIVATE entry_func.cpp load_context.cpp save_context.cpp
                         construct_entry.cpp auto_exit_request.cpp)
