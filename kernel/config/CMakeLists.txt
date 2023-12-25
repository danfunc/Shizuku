target_include_directories(
  shizuku_kernel
  PUBLIC
  interface)
target_compile_definitions(
  shizuku_kernel
  PUBLIC SHIZUKU_PLATFORM=${SHIZUKU_PLATFORM}
  PUBLIC SHIZUKU_PROCESSOR=${SHIZUKU_PROCESSOR}
  PUBLIC SHIZUKU_PROCESSOR_COUNT=${SHIZUKU_PROCESSOR_COUNT}
  PUBLIC SHIZUKU_TIME_SLICE_ONCE=${SHIZUKU_TIME_SLICE_ONCE})
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/interface/shizuku/config.hpp.configure"
  "${CMAKE_CURRENT_SOURCE_DIR}/interface/shizuku/config.hpp")
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/interface/shizuku/processor.hpp.configure"
  "${CMAKE_CURRENT_SOURCE_DIR}/interface/shizuku/processor.hpp")
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/interface/shizuku/memory_manager.hpp.configure"
  "${CMAKE_CURRENT_SOURCE_DIR}/interface/shizuku/memory_manager.hpp")
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/interface/shizuku/platform.hpp.configure"
  "${CMAKE_CURRENT_SOURCE_DIR}/interface/shizuku/platform.hpp")
#[[ target_precompile_headers( shizuku_kernel PRIVATE $<$<COMPILE_LANGUAGE:CXX>:
 ${CMAKE_CURRENT_SOURCE_DIR}/interface/shizuku/config.hpp>)]]
