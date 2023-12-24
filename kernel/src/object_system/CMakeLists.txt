target_include_directories(shizuku_kernel PUBLIC include)
target_sources(shizuku_kernel PRIVATE call_function.cpp create_object.cpp
                                      add_thread.cpp add_func.cpp)
