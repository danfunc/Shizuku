target_include_directories(shizuku_kernel PUBLIC include)
target_sources(
  shizuku_kernel
  PRIVATE object_constructor.cpp export_method.cpp create_thread.cpp
          remove_thread.cpp delete_exported_method.cpp)
