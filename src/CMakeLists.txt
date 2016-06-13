add_library(od_audioengine STATIC interpdata.c interpdata.h wavldr.c wavldr.h reltable.c reltable.h playeng.c playeng.h wav_dumper.c wav_dumper.h)
target_link_libraries(od_audioengine fftset)
target_include_directories(od_audioengine PUBLIC "../..")

