add_library(od_audioengine STATIC wavldr.c reltable.c playeng.c)
target_link_libraries(od_audioengine fftset)
target_include_directories(od_audioengine PUBLIC "../..")

