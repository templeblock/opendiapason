cmake_minimum_required(VERSION 2.8 FATAL_ERROR)

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  message(STATUS "Setting CMAKE_BUILD_TYPE type to 'Debug' as none was specified.")
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Choose the type of build." FORCE)

  # Set the possible values of build type for cmake-gui
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release")
endif()

project(app_engprofile)

if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  set(CMAKE_C_FLAGS_DEBUG "-Wall -O0 -g -fsanitize=address")
  set(CMAKE_C_FLAGS_RELEASE "-Wall -DNDEBUG=1 -O3 -g")
endif()

add_executable(engprofile app_engprofile.c)
target_link_libraries(engprofile fftset od_audioengine cop)
target_include_directories(engprofile PRIVATE "../..")

add_subdirectory("../../cop" "${CMAKE_CURRENT_BINARY_DIR}/cop_dep")
add_subdirectory("../../fftset" "${CMAKE_CURRENT_BINARY_DIR}/fftset_dep")
add_subdirectory("../../opendiapason" "${CMAKE_CURRENT_BINARY_DIR}/opendiapason_dep")
