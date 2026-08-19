set(config_need_headers
    ""
    CACHE INTERNAL "")
foreach(module IN LISTS USE_SHIZUKU_MODULES)
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${module}/CMakeLists.txt)
    add_subdirectory(${module})
  else()
    message(FATAL_ERROR "Module ${module} does not exist.")
  endif()
endforeach()
