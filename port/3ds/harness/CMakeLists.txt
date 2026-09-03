# port/3ds/harness/ — synthetic F3DEX2 display-list replay tests for the citro3d
# backend (the standalone flavour of plan §2.6's test rig: hand-built DLs instead
# of a PC-captured stream). Register new sources HERE, never in port/CMakeLists.txt.
#
# Target: gdx3ds-dl-tests.3dsx — Fast::Interpreter::Run() over hand-built F3DEX2
# lists, one scene per START press, verifying STATUS.md's TODO(citra-verify)
# items. Per-scene ground truth: EXPECTED.md. Run instructions: README.md.
#
# Prereq (same as ../gfx): the LUS newlib patch must be applied to the
# libultraship/ submodule working tree.

if(NOT NINTENDO_3DS)
    return()
endif()

set(GDX3DS_LUS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../libultraship")
set(GDX3DS_DECOMP_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/../../../decomp/include")

# CVar name macros — same directory-scoped cvars.cmake trick as ../gfx.
include("${GDX3DS_LUS_ROOT}/cmake/cvars.cmake")

add_executable(gdx3ds-dl-tests
    dl_tests_main.cpp
    scenes.cpp
    link_stubs.cpp
)

# Include order matters (spike-proven, mirrors ../gfx): lus_stubs shadow desktop
# header leaks and must precede libultraship. decomp/include serves PR/gbi.h to
# scenes.cpp only (which includes no LUS headers — colliding G_* macros).
target_include_directories(gdx3ds-dl-tests PRIVATE
    ../include
    ../lus_stubs
    ../gfx/third_party
    "${GDX3DS_LUS_ROOT}/include"
    "${GDX3DS_LUS_ROOT}/src"
    "${GDX3DS_DECOMP_INCLUDE}"
)
target_compile_definitions(gdx3ds-dl-tests PRIVATE F3DEX_GBI_2) # never ENABLE_OPENGL, never PORT
target_compile_options(gdx3ds-dl-tests PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fexceptions -frtti>
)
set_target_properties(gdx3ds-dl-tests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS ON
)

# Order: backend first (it needs the carve's gfx_cc_* helpers), then the carve,
# then stream D's zipshim (libzip C API over miniz) closing the carve's O2R
# reader; link_stubs.cpp closes the remaining audit families in-target.
target_link_libraries(gdx3ds-dl-tests PRIVATE
    gdx3ds_gfx
    gdx3ds_lus_carve
    gdx3ds_zipshim
)

ctr_create_3dsx(gdx3ds-dl-tests)
