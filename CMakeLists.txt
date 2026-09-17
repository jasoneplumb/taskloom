cmake_minimum_required(VERSION 3.24)

project(taskloom VERSION 0.1.0 LANGUAGES CXX)

add_library(taskloom INTERFACE)
add_library(taskloom::taskloom ALIAS taskloom)

target_include_directories(taskloom INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_compile_features(taskloom INTERFACE cxx_std_20)

if(PROJECT_IS_TOP_LEVEL)
  include(CTest)
  option(TASKLOOM_BUILD_TESTS "Build taskloom tests" ON)
  option(TASKLOOM_BUILD_BENCH "Build taskloom benchmarks" ON)
  set(TASKLOOM_SANITIZE "" CACHE STRING "Sanitizer for tests: thread, address, undefined, or empty")
  if(TASKLOOM_BUILD_TESTS AND BUILD_TESTING)
    add_subdirectory(tests)
  endif()
  if(TASKLOOM_BUILD_BENCH)
    add_subdirectory(bench)
  endif()
endif()
