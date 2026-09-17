cmake_minimum_required(VERSION 3.24)

project(taskloom VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)

add_library(taskloom INTERFACE)
add_library(taskloom::taskloom ALIAS taskloom)

target_include_directories(taskloom INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_compile_features(taskloom INTERFACE cxx_std_20)

if(PROJECT_IS_TOP_LEVEL)
  include(CTest)
  include(CMakePackageConfigHelpers)
  option(TASKLOOM_BUILD_TESTS "Build taskloom tests" ON)
  option(TASKLOOM_BUILD_BENCH "Build taskloom benchmarks" ON)
  set(TASKLOOM_SANITIZE "" CACHE STRING "Sanitizer for tests: thread, address, undefined, or empty")
  if(TASKLOOM_BUILD_TESTS AND BUILD_TESTING)
    add_subdirectory(tests)
  endif()
  if(TASKLOOM_BUILD_BENCH)
    add_subdirectory(bench)
  endif()

  install(TARGETS taskloom EXPORT taskloom-targets)
  install(DIRECTORY include/taskloom DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
  # No dependencies to find, so the export file serves directly as the
  # package config; a generated wrapper would only include() it.
  install(EXPORT taskloom-targets
    FILE taskloomConfig.cmake
    NAMESPACE taskloom::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/taskloom)
  write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/taskloomConfigVersion.cmake"
    COMPATIBILITY SameMinorVersion
    ARCH_INDEPENDENT)
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/taskloomConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/taskloom)
endif()
