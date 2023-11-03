target_include_directories(shizuku_kernel PUBLIC include)
target_sources(shizuku_kernel PRIVATE load_context.cpp save_context.cpp context_switch.cpp entry_func.cpp)
