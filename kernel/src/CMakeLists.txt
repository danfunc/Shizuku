add_subdirectory(processors)
add_subdirectory(cpu_manager)
add_subdirectory(object_system)
add_subdirectory(scheduler)
add_subdirectory(entry_point)
add_subdirectory(for_super_object_api)
target_include_directories(shizuku_kernel PRIVATE include)
target_sources(
  shizuku_kernel
  PRIVATE entry.cpp
          kernel.cpp
          context_switch.cpp
          add_task.cpp
          create_thread.cpp
          get_current_thread.cpp
          export_method.cpp
          create_object.cpp
          call_method.cpp)
