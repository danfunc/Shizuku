target_include_directories(shizuku_kernel PUBLIC include)
target_sources(shizuku_kernel PRIVATE load_context.cpp save_context.cpp)
target_compile_options(shizuku_kernel PRIVATE --include=${CMAKE_CURRENT_SOURCE_DIR}/include/shizuku/processors/rp2040.hpp)