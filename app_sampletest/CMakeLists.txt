cmake_minimum_required(VERSION 2.8 FATAL_ERROR)

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  message(STATUS "Setting CMAKE_BUILD_TYPE type to 'Release' as none was specified.")
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build." FORCE)

  # Set the possible values of build type for cmake-gui
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release")
endif()

project(app_sampletest)

set(CMAKE_C_FLAGS_DEBUG "-Wall -O0 -g -fsanitize=address")
set(CMAKE_C_FLAGS_RELEASE "-Wall -DNDEBUG=1 -O3 -g -mavx -funroll-loops")
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${CMAKE_CURRENT_LIST_DIR}/../cmakemodules)

find_package(Portaudio REQUIRED)
find_package(Portmidi REQUIRED)
include_directories(sampletest ${PORTAUDIO_INCLUDE_DIRS})
include_directories(sampletest ${PORTMIDI_INCLUDE_DIRS})
add_executable(sampletest app_sampletest.c)
target_link_libraries(sampletest od_audioengine fftset)
target_link_libraries(sampletest ${PORTAUDIO_LIBRARIES})
target_link_libraries(sampletest ${PORTMIDI_LIBRARIES})

add_subdirectory("../../cop" "${CMAKE_CURRENT_BINARY_DIR}/cop_dep")
add_subdirectory("../../fftset" "${CMAKE_CURRENT_BINARY_DIR}/fftset_dep")
add_subdirectory("../../opendiapason" "${CMAKE_CURRENT_BINARY_DIR}/opendiapason_dep")
