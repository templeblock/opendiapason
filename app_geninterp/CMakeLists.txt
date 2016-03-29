cmake_minimum_required(VERSION 2.8 FATAL_ERROR)

project(app_geninterp)
add_executable(geninterp app_geninterp.c)
target_link_libraries(geninterp svgplot fftset)

add_subdirectory("../../cop" "${CMAKE_CURRENT_BINARY_DIR}/cop_dep")
add_subdirectory("../../fftset" "${CMAKE_CURRENT_BINARY_DIR}/fftset_dep")
add_subdirectory("../../svgplot" "${CMAKE_CURRENT_BINARY_DIR}/svgplot_dep")
