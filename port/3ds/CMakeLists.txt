# port/3ds/ — Nintendo 3DS port aggregator (Phase 0 skeleton).
#
# Build wiring rule (docs/research/3ds-port-plan.md): this file and the stream
# subdirectories below are the ONLY places 3DS sources get registered. Streams
# never edit port/CMakeLists.txt; each owns exactly one subdirectory here.
#   gfx/    — stream A    os/     — stream B
#   audio/  — stream C    assets/ — stream D
#
# Cross-compile: cmake -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/3DS.cmake -DGDX_PLATFORM_3DS=ON
# The devkitPro toolchain defines NINTENDO_3DS and provides ctr_create_3dsx().

add_subdirectory(gfx)
add_subdirectory(os)
add_subdirectory(audio)
add_subdirectory(assets)
add_subdirectory(game)      # M1 integration: decomp game + bridge layer (3DS-only inside)
add_subdirectory(lus_glue)  # M1 integration: LUS link-stub TUs (3DS-only inside)
if(NINTENDO_3DS)
    add_subdirectory(harness) # DL replay test app (gdx3ds-dl-tests.3dsx)
endif()

add_executable(G-Diffuser-3DS main_3ds.cpp)
target_include_directories(G-Diffuser-3DS PRIVATE include)
target_link_libraries(G-Diffuser-3DS PRIVATE gdx3ds_os gdx3ds_audio gdx3ds_assets)

if(NINTENDO_3DS)
    # Full game link: decomp + bridge objects, LUS carve + citro3d backend, the
    # miniz-backed libzip surface (O2rArchive), and the lus_glue link stubs
    # (Ship::Context / ConsoleVariable / Fast3dWindow replacements — see lus_glue/).
    target_compile_definitions(G-Diffuser-3DS PRIVATE GDX_PLATFORM_3DS=1)
    target_include_directories(G-Diffuser-3DS PRIVATE ..)   # port/: gdx_audio_thread.h, rom_buffer.h, resource/...
    target_sources(G-Diffuser-3DS PRIVATE
        $<TARGET_OBJECTS:gdx3ds_game>
        $<TARGET_OBJECTS:gdx3ds_glue>
        $<TARGET_OBJECTS:gdx3ds_lus_glue>
    )
    target_link_libraries(G-Diffuser-3DS PRIVATE
        gdx3ds_gfx
        gdx3ds_lus_carve
        gdx3ds_zipshim
        gdx3ds_cxd4_rsp
        gdx3ds_os      # again after carve/glue: fiber + config symbols
    )
    set_target_properties(G-Diffuser-3DS PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS ON
    )

    # H-HARDWARE: SD file-log sink for the boot/watchdog/fatal tracers (svc debug goes
    # nowhere on real hardware). Integration-level like main_3ds.cpp, not a stream dir.
    target_sources(G-Diffuser-3DS PRIVATE gdx3ds_filelog.c)

    # RENDER THREAD (LOCKED-60 Task H): core-2 render thread, [debug] renderthread killswitch.
    target_sources(G-Diffuser-3DS PRIVATE gdx3ds_renderthread.cpp)

    # FPS-HUD: bottom-screen fps counter (gdiffuser.ini [debug] fps, DEFAULT ON).
    # Integration-level like gdx3ds_filelog.c. The HUD's static row bakes a
    # configure-time build id (branch@hash of HEAD when cmake last configured — a
    # rebuild without reconfigure keeps the old id) so on-device reports can name
    # the build they came from.
    execute_process(COMMAND git rev-parse --abbrev-ref HEAD
                    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
                    OUTPUT_VARIABLE GDX3DS_GIT_BRANCH
                    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    execute_process(COMMAND git rev-parse --short=8 HEAD
                    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
                    OUTPUT_VARIABLE GDX3DS_GIT_HASH
                    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(NOT GDX3DS_GIT_BRANCH)
        set(GDX3DS_GIT_BRANCH "unknown")
    endif()
    if(NOT GDX3DS_GIT_HASH)
        set(GDX3DS_GIT_HASH "nohash")
    endif()
    target_sources(G-Diffuser-3DS PRIVATE gdx3ds_fps_hud.c)
    set_source_files_properties(gdx3ds_fps_hud.c PROPERTIES COMPILE_DEFINITIONS
        "GDX3DS_BUILD_ID=\"${GDX3DS_GIT_BRANCH}@${GDX3DS_GIT_HASH}\"")

    # MENU: v1 bottom-screen touch menu (gdiffuser.ini [menu] enabled, DEFAULT ON).
    # Integration-level like the HUD/filelog TUs; shares the HUD's console + build id.
    target_sources(G-Diffuser-3DS PRIVATE gdx3ds_menu.c)

    # H-HARDWARE: proper SMDH (48x48 icon + titles/publisher) instead of the devkitPro
    # placeholder. Assets + provenance/licensing: packaging/README.md.
    set(GDX3DS_SMDH "${CMAKE_CURRENT_BINARY_DIR}/G-Diffuser-3DS.smdh")
    ctr_generate_smdh(
        OUTPUT "${GDX3DS_SMDH}"
        NAME "G-Diffuser"
        DESCRIPTION "G-Diffuser — F-Zero X decompilation port"
        AUTHOR "G-Diffuser Project"
        ICON "${CMAKE_CURRENT_SOURCE_DIR}/packaging/icon.png"
    )
    ctr_create_3dsx(G-Diffuser-3DS SMDH "${GDX3DS_SMDH}")

    # Optional installable CIA (target: G-Diffuser-3DS-cia). Gated on makerom +
    # bannertool, which devkitPro does not ship — see packaging/README.md for where
    # to get them (tools/3ds-bin/ is the gitignored drop point).
    find_program(GDX_MAKEROM_EXE makerom HINTS "${CMAKE_SOURCE_DIR}/tools/3ds-bin")
    find_program(GDX_BANNERTOOL_EXE bannertool HINTS "${CMAKE_SOURCE_DIR}/tools/3ds-bin")
    if(GDX_MAKEROM_EXE AND GDX_BANNERTOOL_EXE)
        set(GDX3DS_BANNER "${CMAKE_CURRENT_BINARY_DIR}/G-Diffuser-3DS.bnr")
        set(GDX3DS_CIA "${CMAKE_CURRENT_BINARY_DIR}/G-Diffuser-3DS.cia")
        add_custom_command(
            OUTPUT "${GDX3DS_BANNER}"
            COMMAND "${GDX_BANNERTOOL_EXE}" makebanner
                -i "${CMAKE_CURRENT_SOURCE_DIR}/packaging/banner.png"
                -a "${CMAKE_CURRENT_SOURCE_DIR}/packaging/banner.wav"
                -o "${GDX3DS_BANNER}"
            DEPENDS
                "${CMAKE_CURRENT_SOURCE_DIR}/packaging/banner.png"
                "${CMAKE_CURRENT_SOURCE_DIR}/packaging/banner.wav"
            COMMENT "Building HOME-menu banner G-Diffuser-3DS.bnr"
            VERBATIM
        )
        add_custom_command(
            OUTPUT "${GDX3DS_CIA}"
            COMMAND "${GDX_MAKEROM_EXE}" -f cia -o "${GDX3DS_CIA}" -target t -exefslogo
                -elf "$<TARGET_FILE:G-Diffuser-3DS>"
                -rsf "${CMAKE_CURRENT_SOURCE_DIR}/packaging/gdiffuser.rsf"
                -icon "${GDX3DS_SMDH}"
                -banner "${GDX3DS_BANNER}"
            DEPENDS
                G-Diffuser-3DS
                "${GDX3DS_SMDH}"
                "${GDX3DS_BANNER}"
                "${CMAKE_CURRENT_SOURCE_DIR}/packaging/gdiffuser.rsf"
            COMMENT "Building CIA G-Diffuser-3DS.cia (title 000400000FF3D500)"
            VERBATIM
        )
        add_custom_target(G-Diffuser-3DS-cia DEPENDS "${GDX3DS_CIA}")
    else()
        message(STATUS "G-Diffuser-3DS-cia target disabled: need makerom + bannertool "
                       "(see port/3ds/packaging/README.md)")
    endif()
endif()
