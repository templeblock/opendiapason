cmake_minimum_required(VERSION 3.0 FATAL_ERROR)

if (${CMAKE_SOURCE_DIR} STREQUAL ${CMAKE_CURRENT_LIST_DIR})
  if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    message(STATUS "Setting CMAKE_BUILD_TYPE type to 'Debug' as none was specified.")
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Choose the type of build." FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release")
  endif()
  project(od_audioengine VERSION 0.1.0 LANGUAGES C)
endif()

add_library(od_audioengine STATIC decode_least16x1.h decode_least16x2.h decode_types.h interpdata.c interpdata.h playeng.c playeng.h reltable.c reltable.h wav_dumper.c wav_dumper.h wavldr.c wavldr.h)

if (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
  set_property(TARGET od_audioengine APPEND_STRING PROPERTY COMPILE_FLAGS " /W3")
else()
  set_property(TARGET od_audioengine APPEND_STRING PROPERTY COMPILE_FLAGS " -Wall")
endif()

target_include_directories(od_audioengine PRIVATE "../..")
target_link_libraries(od_audioengine odfilter fftset)
