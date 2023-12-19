add_subdirectory(processors)
add_subdirectory(cpu_manager)
add_subdirectory(object_system)
add_subdirectory(scheduler)
target_include_directories(shizuku_kernel PRIVATE include)
target_sources(shizuku_kernel PRIVATE entry.cpp kernel.cpp context_switch.cpp
                                      add_thread.cpp)
