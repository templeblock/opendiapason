cmake_minimum_required(VERSION 2.8 FATAL_ERROR)

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  message(STATUS "Setting CMAKE_BUILD_TYPE type to 'Debug' as none was specified.")
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Choose the type of build." FORCE)

  # Set the possible values of build type for cmake-gui
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release")
endif()

project(app_sampletune)

if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall")
  set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -fsanitize=address")
  set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -g")
endif()

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${CMAKE_CURRENT_LIST_DIR}/../cmakemodules)

find_package(Portaudio REQUIRED)
include_directories(sampletune ${PORTAUDIO_INCLUDE_DIRS})
add_executable(sampletune app_sampletune.c)
target_link_libraries(sampletune od_audioengine fftset)
target_link_libraries(sampletune ${PORTAUDIO_LIBRARIES})

add_subdirectory("../../cop" "${CMAKE_CURRENT_BINARY_DIR}/cop_dep")
add_subdirectory("../../fftset" "${CMAKE_CURRENT_BINARY_DIR}/fftset_dep")
add_subdirectory("../../opendiapason" "${CMAKE_CURRENT_BINARY_DIR}/opendiapason_dep")
