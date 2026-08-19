add_subdirectory(object_system)
add_subdirectory(cpu_manager)
add_subdirectory(kernel_object)
target_sources(${PROJECT_NAME} PRIVATE kernel.cpp)
