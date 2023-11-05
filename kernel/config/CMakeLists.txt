target_include_directories(shizuku_kernel PUBLIC interface)
target_compile_definitions(shizuku_kernel PUBLIC SHIZUKU_PROCESSOR=${SHIZUKU_PROCESSOR} PUBLIC SHIZUKU_PLATFORM=${SHIZUKU_PLATFORM})
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/interface/shizuku/config.hpp.in"
    "${CMAKE_CURRENT_SOURCE_DIR}/interface/shizuku/config.hpp"
)
#configure_file(
 #   "${CMAKE_CURRENT_SOURCE_DIR}/interface/shizuku/processor.hpp.in"
  #  "${CMAKE_CURRENT_SOURCE_DIR}/interface/shizuku/processor.hpp"
#)