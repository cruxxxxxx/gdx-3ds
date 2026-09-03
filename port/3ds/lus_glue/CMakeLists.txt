# port/3ds/lus_glue/ — M1 integration: link-level replacement TUs for the LUS layers the
# carve deliberately leaves out (docs/research/spike-lus-carve-report.md,
# port/3ds/gfx/STATUS.md unresolved-symbol audit). Approach approved by stream A's
# STATUS: a small shim TU per family instead of widening the carve.
#
#   gdx3ds_context_stub.cpp   — Ship::Context singleton + the accessors/Init subset the
#                               carve and bridge call (replaces src/ship/Context.cpp,
#                               which would drag Console/CrashHandler/spdlog in).
#   gdx3ds_cvar_stub.cpp      — Ship::ConsoleVariable + the CVar C bridge, backed by
#                               defaults (3DS config comes from stream B's INI).
#   gdx3ds_window_stub.cpp    — Ship::Window base-class members without Config/Gui.
#   gdx3ds_fast3d_window.cpp  — Fast::Fast3dWindow replacement wiring the interpreter to
#                               Gdx3ds_GetCitro3dRenderer() + a GfxWindowBackend over
#                               gdx3ds_os (replaces src/fast/Fast3dWindow.cpp).
#   gdx3ds_audiobridge.c      — osAiSetNextBuffer/osAiGetLength -> gdx3ds_audio (hook R5,
#                               port/3ds/audio/STATUS.md).
#   gdx3ds_input_glue.c       — gdx_lus_read_pads over gdx3ds_os_poll_input (desktop
#                               defines it in port/main.cpp against the LUS ControlDeck).

if(NOT NINTENDO_3DS)
    return()
endif()

add_library(gdx3ds_lus_glue OBJECT
    gdx3ds_context_stub.cpp
    gdx3ds_cvar_stub.cpp
    gdx3ds_window_stub.cpp
    gdx3ds_fast3d_window.cpp
    gdx3ds_audiobridge.c
    gdx3ds_input_glue.c
    gdx3ds_libultra.cpp
)

target_include_directories(gdx3ds_lus_glue PRIVATE ../include)
target_link_libraries(gdx3ds_lus_glue PRIVATE gdx3ds_lus_carve gdx3ds_os gdx3ds_audio gdx3ds_gfx)

target_compile_definitions(gdx3ds_lus_glue PRIVATE GDX_PLATFORM_3DS=1 F3DEX_GBI_2=1)
target_compile_options(gdx3ds_lus_glue PRIVATE
    -Wall -Wno-unused-parameter
    $<$<COMPILE_LANGUAGE:CXX>:-fexceptions -frtti>
)
set_target_properties(gdx3ds_lus_glue PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS ON
)
