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
#
# B11 (docs/research/native/ekbuild-progress.md): GDX_3DS_EXPANSION_KIT is the opt-in that
# flips it. It is OFF by default and the SHIPPED build is the OFF one -- EK=ON is not an
# additive flag, it swaps sys/rom -> sys/disk, audio/rom -> audio/disk and save_buffer.c ->
# dd_save.c and changes #ifdef EXPANSION_KIT behaviour in 65 decomp TUs, so an EK build is a
# different game binary, not the shipped one plus two overlays.
option(GDX_3DS_EXPANSION_KIT
       "Compile the 64DD Expansion Kit (course editor / machine creator) into the 3DS build -- EXPERIMENTAL, needs a ~64.9 MB .ndd disk image at runtime" OFF)
if(GDX_3DS_EXPANSION_KIT)
    message(STATUS "G-Diffuser-3DS: GDX_3DS_EXPANSION_KIT=ON -- Expansion Kit build (NOT the shipped configuration)")
    set(GDX_EXPANSION_KIT ON)

    # The EK build needs generated files that are NOT in the repo: port/gen/EkAssetPack.c
    # (.gitignore'd -- it can contain baked disk bytes) and include/assets/us/ek/**.h.
    # They come from tools/gen_ek_assets.py, and nothing used to run it, so EXPANSION_KIT=ON
    # could not be configured from a clean worktree at all -- every EK result so far came
    # from a tree where someone had produced them by hand. Run it at configure time.
    #
    # It imports PyYAML, which Homebrew's python3 refuses to install into (PEP 668). Prefer
    # an interpreter that actually has it over the first one on PATH, and say so plainly
    # rather than failing later with 345 undefined references.
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(GDX_EK_PY "${Python3_EXECUTABLE}")
    execute_process(COMMAND "${GDX_EK_PY}" -c "import yaml"
                    RESULT_VARIABLE GDX_EK_YAML_RC OUTPUT_QUIET ERROR_QUIET)
    if(NOT GDX_EK_YAML_RC EQUAL 0)
        foreach(_cand "$ENV{HOME}/.local/share/gdx-tools-venv/bin/python" "${CMAKE_SOURCE_DIR}/.venv/bin/python")
            if(NOT GDX_EK_YAML_RC EQUAL 0 AND EXISTS "${_cand}")
                execute_process(COMMAND "${_cand}" -c "import yaml"
                                RESULT_VARIABLE GDX_EK_YAML_RC OUTPUT_QUIET ERROR_QUIET)
                if(GDX_EK_YAML_RC EQUAL 0)
                    set(GDX_EK_PY "${_cand}")
                endif()
            endif()
        endforeach()
    endif()
    if(NOT GDX_EK_YAML_RC EQUAL 0)
        message(FATAL_ERROR
            "GDX_3DS_EXPANSION_KIT=ON needs PyYAML for tools/gen_ek_assets.py, and neither\n"
            "  ${Python3_EXECUTABLE}\n"
            "nor a known venv has it. Homebrew's python3 is PEP 668 externally-managed, so:\n"
            "  python3 -m venv ~/.local/share/gdx-tools-venv && \\\n"
            "  ~/.local/share/gdx-tools-venv/bin/pip install pyyaml pillow")
    endif()
    message(STATUS "G-Diffuser-3DS: generating EK assets with ${GDX_EK_PY}")
    execute_process(COMMAND "${GDX_EK_PY}" "${CMAKE_SOURCE_DIR}/tools/gen_ek_assets.py"
                    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
                    RESULT_VARIABLE GDX_EK_GEN_RC
                    OUTPUT_VARIABLE GDX_EK_GEN_OUT ERROR_VARIABLE GDX_EK_GEN_OUT)
    if(NOT GDX_EK_GEN_RC EQUAL 0)
        message(FATAL_ERROR "tools/gen_ek_assets.py failed:\n${GDX_EK_GEN_OUT}")
    endif()
    string(STRIP "${GDX_EK_GEN_OUT}" GDX_EK_GEN_OUT)
    message(STATUS "G-Diffuser-3DS: ${GDX_EK_GEN_OUT}")

    # B11m -- the English text pack (port/gen/EkTranslatedTextPack.c), also .gitignore'd and
    # also needed for the source list to resolve from a clean worktree. Written EMPTY here so
    # configuring never depends on owning a translated image; re-run
    # tools/gen_ek_text_pack.py --translated <image> --reference <image> by hand to fill it,
    # and do NOT overwrite a filled pack on a reconfigure.
    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/port/gen/EkTranslatedTextPack.c")
        execute_process(COMMAND "${GDX_EK_PY}" "${CMAKE_SOURCE_DIR}/tools/gen_ek_text_pack.py"
                        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
                        RESULT_VARIABLE GDX_EK_TEXT_RC
                        OUTPUT_VARIABLE GDX_EK_TEXT_OUT ERROR_VARIABLE GDX_EK_TEXT_OUT)
        if(NOT GDX_EK_TEXT_RC EQUAL 0)
            message(FATAL_ERROR "tools/gen_ek_text_pack.py failed:\n${GDX_EK_TEXT_OUT}")
        endif()
        string(STRIP "${GDX_EK_TEXT_OUT}" GDX_EK_TEXT_OUT)
        message(STATUS "G-Diffuser-3DS: ${GDX_EK_TEXT_OUT}")
    else()
        message(STATUS "G-Diffuser-3DS: keeping the existing port/gen/EkTranslatedTextPack.c")
    endif()
else()
    set(GDX_EXPANSION_KIT OFF)
endif()

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
if(GDX_3DS_EXPANSION_KIT)
    # EK=ON supplies the real leoBootID (port/n64_leo.c) and the ucode text markers
    # (port/gen/EkLinkStubs.c), so the EK=OFF stand-ins must NOT be compiled too.
    set(GDX3DS_EK_STUBS "")
    set(GDX3DS_EK_SOURCES ${GDX_GAME_EK_PORT_SOURCES}
        # ekeditmesh [native] ek_editmesh: the course editor preview's resident bake key (o3dsek
        # S5c). EK-only, so the shipped (EK=OFF) game links nothing new.
        gdx3ds_ek_editmesh_game.c
        # ekperf3 [native] ek_nimach: the EK machine select / settings machines on the NRR
        # (decomp-3ds-ek-nimach.patch). EK-only, like the line above.
        gdx3ds_ek_nimach_game.c
        # [ek] course_share: the MFS half of course export/import (../gdx3ds_course_share_game.h).
        gdx3ds_course_share_game.c)
else()
    set(GDX3DS_EK_STUBS gdx3ds_us_linkstubs.c)
    set(GDX3DS_EK_SOURCES "")
endif()

add_library(gdx3ds_game OBJECT ${GAME_SOURCES} ${GDX_GAME_CORE_SOURCES}
    ${GDX3DS_EK_STUBS}      # EK=OFF stand-ins for n64_leo.c/EkLinkStubs.c symbols
    ${GDX3DS_EK_SOURCES}    # EK=ON: n64_leo.c, gen/EkAssetBindings.c, gen/EkLinkStubs.c, gdx_ek_disk_overrides.c
    gdx3ds_livery_diag.c    # SHIP-LIVERY-2 [livery] game-state receipts (weak-called by gfx)
    gdx3ds_track_gbi.c      # [native] track: real GBI emitters behind gdx3ds_track_shim.h
    gdx3ds_track_bake_game.c # [native] track_resident: game-data side of the course bake (C5 Stage 3c)
    gdx3ds_nrr_trk_game.c   # n60 NRR track: the lean course walk (decomp-native-nrr-trk.patch)
    gdx3ds_fx_gbi.c         # [native] fx: real GBI emitters behind gdx3ds_fx_shim_begin.h
    gdx3ds_replay_game.c    # C6 [replay]: read-only game signals + GfxPool geometry for the pose replay
    gdx3ds_gad_gbi.c        # [native] gadget_batch: real GBI emitters behind gdx3ds_gad_shim_begin.h
    gdx3ds_c5tick_game.c    # C5TICK: dlwrite scratch ranges + the gstate_ext digest (decomp types)
    gdx3ds_frameskip_game.c # n60fs [debug] frameskip = race0: the race scope from the game's mode globals
    gdx3ds_snapshot_game.c  # mp-snapshot: the snapshot window scope + the required-symbol table (read only)
    gdx3ds_snapshot_nosnap.c # snap-fast: the runtime NOSNAP table (census-chosen objects no race writes)
    gdx3ds_nrr_sky_game.c   # [native] nrr_sky: the consumed SKY section's stub + snapshot (n60sky)
    gdx3ds_nrr_shd_game.c   # [native] nrr_shd: the consumed SHD section's stub + snapshot (n60fx)
    gdx3ds_nrr_fx_game.c    # [native] nrr_fx: the consumed BOOST section's stub + the effects' snapshot (n60fx)
    gdx3ds_nrr_dress_game.c # [native] nrr_dress: the consumed DRESS section's stub: gadget pass native, decorations stock (n60fx)
    gdx3ds_prewarm_game.c   # [native] prewarm: first-use work in the race's opaque appear frame (n60prewarm)
    gdx3ds_course_game.c    # [native] ni_course: the course preview's marker pair + generation (nicourse)
    gdx3ds_fullview_game.c  # MP phase 3 [net] mp_fullview / [debug] fullview_player: the local view full screen
    gdx3ds_mp_pick_game.c   # MP stockpick: the stock 1P menus as each player's picker (decomp-3ds-mp-stockpick.patch)
)

target_include_directories(gdx3ds_game PRIVATE
    "${DECOMP}"            # decomp root: the C uses includes relative to it (e.g. "src/audio/...")
    "${DECOMP}/include"
    "${DECOMP}/src"
    "${GDX_PORT_DIR}/../include"   # Torch-generated asset headers (arrays + #defines)
    "${CMAKE_CURRENT_SOURCE_DIR}/../gfx" # [native] track shim header (course.c, decomp-native-track-shim.patch)
)

target_compile_definitions(gdx3ds_game PRIVATE
    ${GDX_DEV_TOOLS_DEFS}
    ${GDX_GAME_DEFS}
    VERSION_US=1
    ASSET_VERSION=us
    ASSET_REVISION=rev0
    GDX_PLATFORM_3DS=1
    GDX_NATIVE_TRACK=1     # course.c includes gdx3ds_track_shim.h (runtime-gated: [native] track)
    GDX_NATIVE_FX=1        # racer.c includes gdx3ds_fx_shim.h (runtime-gated: [native] fx_sink)
    GDX_NATIVE_HUD=1       # race.c/racer.c compact the HUD's display list (runtime-gated: [native] hud_batch)
    GDX_NATIVE_DRESS=1     # course_gadgets.c/racer.c: gadget shim + direct-load scopes (runtime-gated, gdx3ds_dress.h)
    GDX_NATIVE_SKY=1       # background.c marks/captures Background_Draw (runtime-gated: [native] sky_retain/skyclock)
)

if(GDX_3DS_EXPANSION_KIT)
    # Same triplet the desktop gdiffuser_game uses (port/CMakeLists.txt:30). MFS_VERSION/
    # BUILD_VERSION are the 64DD filesystem's own build selectors, not a game region: the
    # US text/font behaviour stays on VERSION_US above.
    target_compile_definitions(gdx3ds_game PRIVATE EXPANSION_KIT=1 MFS_VERSION=1 BUILD_VERSION=VERSION_J)
endif()

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
    "${GDX_PORT_DIR}/gdx_hud_placement.c"  # nihudtop Phase 3: split out of input_bridge.c so the
                                            # predicate is host-testable standalone
    "${GDX_PORT_DIR}/gdx_input_script.c"
    "${GDX_PORT_DIR}/gdx_input_rec.c"     # ORACLE: .gdxrec record/replay
    "${GDX_PORT_DIR}/gdx_determinism.c"   # ORACLE: determinism pins
    "${GDX_PORT_DIR}/gdx_mp_lockstep.c"   # mp-spike: lockstep protocol core (pure; host-tested)
    "${GDX_PORT_DIR}/gdx_mp_drop.c"       # mp-uds: drop-out protocol (pure; host-tested)
    "${GDX_PORT_DIR}/gdx_mp_beacon.c"     # mp-uds: NEARBY room beacon (pure; host-tested + fuzzed)
    "${GDX_PORT_DIR}/n64_gfx_bridge.cpp"
    "${GDX_PORT_DIR}/gdx_interp.cpp"
    "${GDX_PORT_DIR}/n64_gfx_convert.cpp"
    "${GDX_PORT_DIR}/gdx_vi_convert.c"
    "${GDX_PORT_DIR}/n64_audio_hle.c"
    "${GDX_PORT_DIR}/n64_audio_hle_fast.c" # [debug] audio_simd: bit-exact fast HLE kernels (C4k)
    "${GDX_PORT_DIR}/gdx_audio_hle_cost.c" # [synx]: HLE synth CPU-vs-wall ledger (C4k)
    "${GDX_PORT_DIR}/gdx_ndsp_hash.c"   # [debug] ndsphash: the content-addressed render digest
    "${GDX_PORT_DIR}/gdx_audio_mix_census.c" # [debug] audiomix: the mix-bus headroom census
    "${GDX_PORT_DIR}/gdx_audio_lle.c"
    "${GDX_PORT_DIR}/gdx_audio_thread.cpp"
    "${GDX_PORT_DIR}/gdx_audio_capture.c"
    "${GDX_PORT_DIR}/gdx_frame_pacer.c"
    "${GDX_PORT_DIR}/shims.c"
    "${GDX_PORT_DIR}/AssetLoader.cpp"
    "${GDX_PORT_DIR}/resource/ResourceFactories.cpp"
    "${GDX_PORT_DIR}/rom_buffer.cpp"
    "${GDX_PORT_DIR}/sram_buffer.cpp"
    "${GDX_PORT_DIR}/gdx_save_replace.c" # n60save: crash-safe replace (the SD archive never renames over a file)
    "${GDX_PORT_DIR}/disk_savefile.cpp"
    "${GDX_PORT_DIR}/gdx_ghost_io.c"
    "${GDX_PORT_DIR}/gdx_perf.cpp"
    "${GDX_PORT_DIR}/gdx_dev_gates.c"
    "${GDX_PORT_DIR}/gdx_workshop.cpp"
    "${GDX_PORT_DIR}/mio0_wrap.c"
    "${GDX_PORT_DIR}/gdx_stall.c"
    "${GDX_PORT_DIR}/gdx_segment_source.c"
    # C4d ([debug] audio_stream / audiodiet, both default off): the audio_table diet and the
    # block reader it shares with the 64DD lever. gdx_block_cache is the cache diskstream
    # wrote, lifted out of gdx_disk_stream.cpp's file statics so there is ONE of it; it builds
    # unconditionally now because the audio arm is not EK-gated.
    "${GDX_PORT_DIR}/gdx_block_cache.cpp"
    "${GDX_PORT_DIR}/gdx_o2r_stream.cpp"
    "${GDX_PORT_DIR}/gdx_audio_diet.cpp"
    "${GDX_PORT_DIR}/../torch/lib/libmio0/mio0.c"
)

if(GDX_3DS_EXPANSION_KIT)
    # The desktop executable's EK glue (port/CMakeLists.txt:171): the disk-image loader plus
    # the fan-translation string/override layers n64_leo.c and the EK overlays call into.
    target_sources(gdx3ds_glue PRIVATE
        "${GDX_PORT_DIR}/disk_buffer.cpp"
        "${GDX_PORT_DIR}/gdx_disk_stream.cpp"   # B11c: SD-backed block reader ([debug] disk_stream)
        "${GDX_PORT_DIR}/gdx_disk_dcrc.cpp"      # B11m: the .dcrc cached identity fingerprint
        "${GDX_PORT_DIR}/gdx_save_ek_layout.c"   # ekship: the EK title's own save, proven-EK seed
        "${GDX_PORT_DIR}/gen/EkTranslatedOverrides.c"
        "${GDX_PORT_DIR}/gdx_ek_strings.c"
        "${GDX_PORT_DIR}/gen/EkTranslatedStrings.c"
        # B11m: the English text pack. The .c half is generated and .gitignore'd (it is disk
        # bytes) and is EMPTY unless tools/gen_ek_text_pack.py was run with a translated image,
        # in which case gdx_ek_text_pack_apply() is a no-op. Always in the list so the build
        # graph is stable, exactly as gen/EkAssetPack.c is.
        "${GDX_PORT_DIR}/gdx_ek_text_pack.c"
        "${GDX_PORT_DIR}/gen/EkTranslatedTextPack.c"
        gdx3ds_disk_async.c    # o3dsekperf [debug] audio_disk_async: the 64DD audio loader's I/O worker
        gdx3ds_ek_audio_bulk.c # ekaudio2 [debug] audio_sync_bulk_kb: sync audio_table loads in large SD reads
        gdx3ds_ek_stubs.cpp)   # 3DS stand-ins for the dropped gdx_extract_launch.cpp helpers
endif()

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
    # B11d/B11f: -DGDX_ALLOW_JP_INPUTS=ON was reaching ONLY the desktop G-Diffuser target
    # (port/CMakeLists.txt:178), so on 3DS it configured, printed "Japanese ROM/disk inputs
    # are ACCEPTED", and then did nothing: rom_buffer.cpp's SHA-1 gate and disk_buffer.cpp's
    # CRC-64 gate both still refused, and the Japanese .ndd FILENAMES were not even searched
    # -- which presents as "[leo] no disk image found" with no mention of the gate. Both of
    # those TUs are in THIS object library, so the definitions belong here. The option
    # defaults ON for 3DS builds (port/cmake/GdxGameSources.cmake) and OFF on desktop.
    ${GDX_JP_INPUT_DEFS}
    GDX_PLATFORM_3DS=1
    F3DEX_GBI_2=1
    $<$<BOOL:${GDX_3DS_EXPANSION_KIT}>:EXPANSION_KIT=1>
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
