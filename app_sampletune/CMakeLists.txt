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

project(app_sampletune)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${CMAKE_CURRENT_LIST_DIR}/../cmakemodules)

add_executable(sampletune app_sampletune.c)

if (x${CMAKE_CXX_COMPILER_ID} STREQUAL "xMSVC")
  set_property(TARGET sampletune APPEND_STRING PROPERTY COMPILE_FLAGS " /W3")
else()
  set_property(TARGET sampletune APPEND_STRING PROPERTY COMPILE_FLAGS " -Wall")
endif()

if(WIN32)
  set_property(TARGET sampletune APPEND_STRING PROPERTY COMPILE_FLAGS " -D_CRT_SECURE_NO_WARNINGS")
endif()

if (EXISTS "${CMAKE_CURRENT_LIST_DIR}/../../portaudio" AND IS_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/../../portaudio")
  target_link_libraries(sampletune portaudio_static)
  add_subdirectory("../../portaudio" "${CMAKE_CURRENT_BINARY_DIR}/portaudio_dep")
else ()
  find_package(Portaudio REQUIRED)
  target_include_directories(sampletune PRIVATE ${PORTAUDIO_INCLUDE_DIRS})
  target_link_libraries(sampletune ${PORTAUDIO_LIBRARIES})
endif ()

target_include_directories(sampletune PRIVATE "../..")
target_link_libraries(sampletune od_audioengine fftset smplwav)

add_subdirectory("../../cop" "${CMAKE_CURRENT_BINARY_DIR}/cop_dep")
add_subdirectory("../../smplwav" "${CMAKE_CURRENT_BINARY_DIR}/smplwav_dep")
add_subdirectory("../../fftset" "${CMAKE_CURRENT_BINARY_DIR}/fftset_dep")

# Set the flag here so that it is not included on any source files other than
# those that are a part of open diapason. There is probably a better way to do
# this.
if (OPENDIAPASON_VERBOSE_DEBUG)
  target_link_libraries(sampletune svgplot)
  add_subdirectory("../../svgplot" "${CMAKE_CURRENT_BINARY_DIR}/svgplot_dep")
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DOPENDIAPASON_VERBOSE_DEBUG=1")
endif ()

add_subdirectory("../../opendiapason" "${CMAKE_CURRENT_BINARY_DIR}/opendiapason_dep")
