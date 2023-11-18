target_include_directories(shizuku_kernel PUBLIC include)
file(GLOB rp2040_cpu_driver_sources *.cpp)
target_sources(
  shizuku_kernel
  PRIVATE add_task.cpp
          context_switch.cpp
          entry_func.cpp
          get_current_context.cpp
          get_current_object.cpp
          load_context.cpp
          save_context.cpp
          change_current_task_property.cpp
          abort_current_task.cpp)
