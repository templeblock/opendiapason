add_library(odfilter STATIC odfilter.c ../odfilter.h)
target_link_libraries(odfilter fftset)
target_include_directories(odfilter PUBLIC "../..")
