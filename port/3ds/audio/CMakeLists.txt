# port/3ds/audio/ — stream C: ndsp output, spare-core drain thread, HLE-primary.
# Register new sources HERE, never in port/CMakeLists.txt.
# Shared-file needs (e.g. gdx_audio_thread.cpp hooks) are requests to stream E — see STATUS.md.
if(NINTENDO_3DS)
    # Real backend: ndsp channel 0, LightLock ring, drain thread on the spare-core ladder.
    add_library(gdx3ds_audio STATIC
        gdx3ds_audio_ndsp.c
    )
else()
    # Host build keeps the Phase 0 swallow-everything stub (no libctru on host).
    add_library(gdx3ds_audio STATIC
        gdx3ds_audio_stub.c
    )
endif()
target_include_directories(gdx3ds_audio PUBLIC ../include)
