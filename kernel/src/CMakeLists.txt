add_subdirectory(processors)
add_subdirectory(object_system)
target_sources(shizuku_kernel PRIVATE entry.cpp kernel.cpp)
