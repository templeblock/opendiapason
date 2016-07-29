cmake_minimum_required(VERSION 3.0 FATAL_ERROR)

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  message(STATUS "Setting CMAKE_BUILD_TYPE type to 'Debug' as none was specified.")
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Choose the type of build." FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release")
endif()

if (NOT DEFINED ENV{MACOSX_DEPLOYMENT_TARGET})
  set(CMAKE_OSX_DEPLOYMENT_TARGET "10.6")
endif()
if (CMAKE_BUILD_TYPE STREQUAL "Debug" AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=address")
endif()
if (CMAKE_BUILD_TYPE STREQUAL "Release" AND NOT CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g")
endif()

project(app_geninterp)

add_executable(geninterp app_geninterp.c)

if (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
  set_property(TARGET geninterp APPEND_STRING PROPERTY COMPILE_FLAGS " /W3")
else()
  set_property(TARGET geninterp APPEND_STRING PROPERTY COMPILE_FLAGS " -Wall")
endif()

target_include_directories(geninterp PRIVATE "../..")
target_link_libraries(geninterp svgplot fftset)

add_subdirectory("../../cop" "${CMAKE_CURRENT_BINARY_DIR}/cop_dep")
add_subdirectory("../../fftset" "${CMAKE_CURRENT_BINARY_DIR}/fftset_dep")
add_subdirectory("../../svgplot" "${CMAKE_CURRENT_BINARY_DIR}/svgplot_dep")
