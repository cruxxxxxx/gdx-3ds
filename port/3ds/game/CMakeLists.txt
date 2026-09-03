# port/3ds/game/ — M1 integration: the decomp game + port bridge layer compiled for 3DS.
#
# Reuses port/cmake/GdxGameSources.cmake (the same source list + filters the desktop
# gdiffuser_game object library builds from) so the two builds cannot drift. Everything
# here is 3DS-only; the host GDX_PLATFORM_3DS stub build skips this directory's targets.
#
# Two OBJECT libraries (objects always fully link, so decomp<->bridge symbol cycles and
# static-archive ordering never bite):
#   gdx3ds_game — decomp game C + the port game-core TUs (n64_sched, decomp_port, ...),
#                 compiled with the desktop's exact define/flag set + GDX_PLATFORM_3DS.
#   gdx3ds_glue — the port bridge/glue TUs from the desktop G-Diffuser executable list,
#                 minus the desktop-only surfaces (SDL/ImGui/Discord/first-boot GUI/crash
#                 handler); see the keep/drop table in docs/research/m1-link-status.md.

if(NOT NINTENDO_3DS)
    return()
endif()

# 3DS MVP is cartridge-content only (docs/research/3ds-port-plan.md §3 stream D / §7);
# the 64DD Expansion Kit sources stay desktop-only until post-MVP. Must be set before the
# include() so the shared filter logic picks the rom-side subsystems (CMP0077: option()
# honors the pre-set normal variable).
set(GDX_EXPANSION_KIT OFF)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/GdxGameSources.cmake")

# ---------------------------------------------------------------------------------
# cxd4 RSP interpreter (LLE audio) — same vendored sources as the desktop lib.
# ---------------------------------------------------------------------------------
add_library(gdx3ds_cxd4_rsp STATIC
    "${GDX_PORT_DIR}/rsp/cxd4/gdx_rsp_driver.c"
    "${GDX_PORT_DIR}/rsp/cxd4/su.c"
    "${GDX_PORT_DIR}/rsp/cxd4/vu/add.c"
    "${GDX_PORT_DIR}/rsp/cxd4/vu/divide.c"
    "${GDX_PORT_DIR}/rsp/cxd4/vu/logical.c"
    "${GDX_PORT_DIR}/rsp/cxd4/vu/multiply.c"
    "${GDX_PORT_DIR}/rsp/cxd4/vu/select.c"
    "${GDX_PORT_DIR}/rsp/cxd4/vu/vu.c")
target_include_directories(gdx3ds_cxd4_rsp PUBLIC "${GDX_PORT_DIR}/rsp/cxd4")
target_compile_definitions(gdx3ds_cxd4_rsp PRIVATE SP_EXECUTE_LOG=1 _CRT_SECURE_NO_WARNINGS=1)
target_compile_options(gdx3ds_cxd4_rsp PRIVATE -w -fno-strict-aliasing -fwrapv)

# ---------------------------------------------------------------------------------
# gdx3ds_game — decomp game sources (desktop parity flags; see port/CMakeLists.txt for
# the rationale on every flag). The fiber backend is NOT added here: gdx_fiber_3ds.c
# already lives in gdx3ds_os (stream B).
# ---------------------------------------------------------------------------------
add_library(gdx3ds_game OBJECT ${GAME_SOURCES} ${GDX_GAME_CORE_SOURCES}
    gdx3ds_us_linkstubs.c   # EK=OFF stand-ins for n64_leo.c/EkLinkStubs.c symbols
    gdx3ds_livery_diag.c    # SHIP-LIVERY-2 [livery] game-state receipts (weak-called by gfx)
)

target_include_directories(gdx3ds_game PRIVATE
    "${DECOMP}"            # decomp root: the C uses includes relative to it (e.g. "src/audio/...")
    "${DECOMP}/include"
    "${DECOMP}/src"
    "${GDX_PORT_DIR}/../include"   # Torch-generated asset headers (arrays + #defines)
)

target_compile_definitions(gdx3ds_game PRIVATE
    ${GDX_DEV_TOOLS_DEFS}
    ${GDX_GAME_DEFS}
    VERSION_US=1
    ASSET_VERSION=us
    ASSET_REVISION=rev0
    GDX_PLATFORM_3DS=1
)

# Same UB-hardening/legacy-quieting as the desktop gdiffuser_game (see port/CMakeLists.txt).
target_compile_options(gdx3ds_game PRIVATE -w -Werror=return-type -fcommon
    -fno-strict-aliasing -fwrapv
    -Wno-error=int-conversion
    -Wno-error=incompatible-pointer-types
    -Wno-error=implicit-function-declaration)

# ---------------------------------------------------------------------------------
# gdx3ds_glue — the port bridge layer (desktop G-Diffuser executable TU list, minus
# desktop-only files). Dropped TUs and why:
#   main.cpp                          -> replaced by port/3ds/main_3ds.cpp
#   gdx_discord.cpp                   -> Discord RPC dropped on 3DS
#   gdx_gui.cpp gdx_menu*.cpp gdx_console_log.cpp ui/UIWidgets.cpp gdx_imgui_nav.cpp
#   gdx_ghost_window.cpp gdx_input_viewer.cpp gdx_fps_overlay.cpp
#                                     -> ImGui enhancement/debug UI dropped on 3DS
#   enhancements/tuning/BoostDuration.cpp -> registers itself with the ImGui menu registry
#   gdx_firstboot.cpp gdx_firstboot_gui.cpp gdx_extract_launch.cpp gdx_dump_launch.cpp
#                                     -> first-boot Torch extraction moved to tools/prebake (PC)
#   disk_buffer.cpp + EK gen TUs      -> GDX_EXPANSION_KIT=OFF on 3DS (MVP)
# ---------------------------------------------------------------------------------
add_library(gdx3ds_glue OBJECT
    "${GDX_PORT_DIR}/enhancements/events/GameEvents.cpp"   # decomp racer.c references GameEvents_FireOnBoostStart
    "${GDX_PORT_DIR}/input_bridge.c"
    "${GDX_PORT_DIR}/gdx_input_script.c"
    "${GDX_PORT_DIR}/n64_gfx_bridge.cpp"
    "${GDX_PORT_DIR}/gdx_interp.cpp"
    "${GDX_PORT_DIR}/n64_gfx_convert.cpp"
    "${GDX_PORT_DIR}/gdx_vi_convert.c"
    "${GDX_PORT_DIR}/n64_audio_hle.c"
    "${GDX_PORT_DIR}/gdx_audio_lle.c"
    "${GDX_PORT_DIR}/gdx_audio_thread.cpp"
    "${GDX_PORT_DIR}/gdx_audio_capture.c"
    "${GDX_PORT_DIR}/gdx_frame_pacer.c"
    "${GDX_PORT_DIR}/shims.c"
    "${GDX_PORT_DIR}/AssetLoader.cpp"
    "${GDX_PORT_DIR}/resource/ResourceFactories.cpp"
    "${GDX_PORT_DIR}/rom_buffer.cpp"
    "${GDX_PORT_DIR}/sram_buffer.cpp"
    "${GDX_PORT_DIR}/disk_savefile.cpp"
    "${GDX_PORT_DIR}/gdx_ghost_io.c"
    "${GDX_PORT_DIR}/gdx_perf.cpp"
    "${GDX_PORT_DIR}/gdx_dev_gates.c"
    "${GDX_PORT_DIR}/gdx_workshop.cpp"
    "${GDX_PORT_DIR}/mio0_wrap.c"
    "${GDX_PORT_DIR}/gdx_segment_source.c"
    "${GDX_PORT_DIR}/../torch/lib/libmio0/mio0.c"
)

target_include_directories(gdx3ds_glue PRIVATE
    "${GDX_PORT_DIR}"                       # "resource/ResourceFactories.h" etc.
    "${DECOMP}/include"
    "${DECOMP}/src"
    "${GDX_PORT_DIR}/../torch/lib/libmio0"  # mio0.h + utils.h
    "${CMAKE_CURRENT_SOURCE_DIR}/../include"  # gdx3ds_* contract headers
)
# The carve's include set (lus_stubs -> third_party -> LUS include/src) comes via the
# gdx3ds_lus_carve INTERFACE include dirs (gfx/CMakeLists.txt exports them).
target_link_libraries(gdx3ds_glue PRIVATE gdx3ds_lus_carve gdx3ds_cxd4_rsp)

target_compile_definitions(gdx3ds_glue PRIVATE
    ${GDX_DEV_TOOLS_DEFS}
    GDX_PLATFORM_3DS=1
    F3DEX_GBI_2=1
)

target_compile_options(gdx3ds_glue PRIVATE
    -Wall -Wno-unused-parameter
    -fno-strict-aliasing -fwrapv
    $<$<COMPILE_LANGUAGE:CXX>:-fexceptions -frtti>
)
set_target_properties(gdx3ds_glue PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS ON
)
