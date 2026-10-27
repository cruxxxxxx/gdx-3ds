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

# Build-time build id (branch@hash of CURRENT HEAD), baked into the FPS-HUD's static row,
# the crash/boot-trail reports, the ORACLE [fh] init line, AND port/3ds/net/'s "[net] link up
# build=..." / PING receipts, so an on-device report and a hw.py reload can both name the
# build they actually came from. Hoisted above add_subdirectory() because net/ consumes it.
#
# COORDINATOR FIX (hw bug n60-3d8, item 3): this used to be two execute_process() git calls
# run only when cmake reconfigures (GDX3DS_GIT_BRANCH/GDX3DS_GIT_HASH baked straight into each
# consumer's COMPILE_DEFINITIONS), so a rebuild-without-reconfigure silently kept re-stamping
# whatever HEAD was current at the LAST configure -- n60-3d8 shipped a binary built from
# 37dc7a52 that still reported n60-3d7's 88ab0de6. gdx3ds_gen_build_id.cmake now runs from an
# "always out of date" custom target (no OUTPUT, just BYPRODUCTS -- cmake/ninja/make re-run its
# COMMAND on every build) and regenerates a header from the CURRENT HEAD each time, but only
# actually touches the header's mtime when the id text changed, so consumers only recompile
# when the stamp actually moved: this does NOT turn into a full rebuild every build. See that
# script's own comment for the write-only-on-change guard.
set(GDX3DS_BUILD_ID_HEADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(GDX3DS_BUILD_ID_HEADER "${GDX3DS_BUILD_ID_HEADER_DIR}/gdx3ds_build_id.h")
file(MAKE_DIRECTORY "${GDX3DS_BUILD_ID_HEADER_DIR}")
add_custom_target(gdx3ds_build_id_header ALL
    COMMAND "${CMAKE_COMMAND}"
            "-DGDX3DS_SRC_DIR=${CMAKE_SOURCE_DIR}"
            "-DGDX3DS_OUT_FILE=${GDX3DS_BUILD_ID_HEADER}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/gdx3ds_gen_build_id.cmake"
    BYPRODUCTS "${GDX3DS_BUILD_ID_HEADER}"
    COMMENT "gdx3ds: refreshing build-id header from git rev-parse (HEAD, not last configure)"
    VERBATIM)

# --- C4g: RDRAM ARENA SIZE (-DGDX_3DS_RDRAM_MB=N, DEFAULT 16 = the shipped value) ---------
#
# port/n64_rdram.h's GDX_RDRAM_SIZE is the single largest row on the heap -- ONE 16 MB calloc,
# 37% of the shipped heap -- and 16 MB is not a console size: the N64 has 4 MB of RDRAM and
# 8 MB with the Expansion Pak. It is the PORT's arena (two bump cursors growing toward each
# other) and it is ALSO the top of the gfx bridge's bare-offset classification window, so it
# is a build-time knob with a guest receipt rather than an ini lever: `[rdram-res]` under
# `[debug] membudget=1` prints the occupancy this size has to cover.
#
# HAZARD, and it is the reason this defaults to 16: an undersized arena does NOT fail loudly.
# gdx_rdram_alloc_raw aborts, but the resolver narrowing does not -- an offset that used to
# resolve into the arena simply stops resolving, and the port's own failure mode under memory
# pressure is a hang with a frozen frame counter, not a crash (o3dssim-progress.md §3.1).
# Read [rdram-res] minSafeMB back from a run over the CONTENT you care about before lowering
# this, and read docs/research/native/rdramdiet-progress.md for what the corpus did and did
# not exercise.
set(GDX_3DS_RDRAM_MB "16" CACHE STRING
    "C4g: size of the decomp RDRAM arena in MiB (16 = the shipped default).")

# --- OLD 3DS PROFILE (-DGDX_3DS_O3DS=ON, DEFAULT OFF) -- NEXT.md C4b, o3dsfit-progress.md ---
#
# One flag for the Old 3DS CIA. The real console (memtype 0, CIA, 2026-09-29) grants a 24 MiB
# heap and a 29 MiB linear heap by default out of a 55,660,544 B budget. The port peaks at
# ~9.2 MB of linear and up to 27.8 MB of heap over the replay corpus (levers off), so the
# default split is 2.7 MB short on the heap side with 20 MB of linear idle. The profile bundles:
#   * C4g: RDRAM arena 8 MiB (unless GDX_3DS_RDRAM_MB is set to something other than 16);
#   * C4b: __ctru_linear_heap_size = GDX3DS_O3DS_LINEAR_KIB (unless GDX_3DS_LINEAR_HEAP_KIB is
#     set), so libctru takes `heap = avail - linear` with no 24 MiB cap.
# The size and its receipts are in docs/research/native/o3dsfit-progress.md. The ini levers
# the profile still needs ([debug] audio_stream=1, tex_upload_kb=512) are run-time and ship
# in the CIA's ini variants. OFF leaves every variable below at its shipped value.
option(GDX_3DS_O3DS
       "Old 3DS build profile: RDRAM 8 MiB + the C4b linear-heap pin (the O3DS CIA is this one flag)" OFF)
set(GDX3DS_O3DS_LINEAR_KIB 16384)
set(GDX3DS_O3DS_RDRAM_MB 8)
# O3DS + Expansion Kit. This used to be a FATAL_ERROR on GDX_EXPANSION_KIT, which never fired:
# that variable is set inside game/'s own scope, after this point, so the profile has always
# been applied to EK builds. Its premise ("the EK persist path overflows a 16 MiB arena",
# rdramdiet (g)) came from the 2026-09-23 EK boot, which was wedged (B11a: garbage medium 0x5F
# walking the program image). Measured on a working EK build at 8 MiB with a real streamed
# disk (o3dsek-plan.md section 2): boot, menus, Mute City, the DD-1 course, the course editor
# and the machine creator, 0 arena-exhausted fatals. GDX_3DS_EXPANSION_KIT is the cache option
# that IS visible here.
if(GDX_3DS_O3DS AND GDX_3DS_EXPANSION_KIT)
    message(STATUS "G-Diffuser-3DS: OLD 3DS + EXPANSION KIT -- RDRAM 8 MiB measured sufficient "
                   "(docs/research/native/o3dsek-plan.md). Ship with [debug] disk_stream=2.")
endif()
if(GDX_3DS_O3DS AND GDX_3DS_RDRAM_MB EQUAL 16)
    set(GDX_3DS_RDRAM_MB ${GDX3DS_O3DS_RDRAM_MB})
endif()

# C4o: the Old 3DS code generation profile (-Os cold / -O2 hot / hot-first .text order).
# `auto` follows GDX_3DS_O3DS; `stock` (the New 3DS default) changes nothing. Applied to the
# targets at the end of this file, once they exist. port/3ds/codegen/Gdx3dsCodegen.cmake.
include("${CMAKE_CURRENT_SOURCE_DIR}/codegen/Gdx3dsCodegen.cmake")

math(EXPR GDX3DS_RDRAM_BYTES "${GDX_3DS_RDRAM_MB} * 1024 * 1024")
if(GDX3DS_RDRAM_BYTES LESS 1048576)
    message(FATAL_ERROR "GDX_3DS_RDRAM_MB=${GDX_3DS_RDRAM_MB} is not a usable arena size.")
endif()
# Reaches every TU that includes n64_rdram.h -- the port glue, the bridge AND the decomp game
# object library, all of which are created by the add_subdirectory() calls below. An unset
# knob defines the macro to exactly the value the header already had, so the default build is
# the same translation it was before the knob existed.
add_compile_definitions(GDX_RDRAM_SIZE_BYTES=${GDX3DS_RDRAM_BYTES}u)

# --- C5-HOTCOLD: `cold` attribute (-DGDX_3DS_HOTCOLD_COLD=ON, DEFAULT OFF) -------------------
# GDX_COLD (port/3ds/gfx/gdx3ds_hotcold.h) keeps the slow-path extraction (noinline) either way.
# `cold` additionally makes GCC optimise those functions for size, and cold -Os on boot code is
# what crashed the Old 3DS at boot (rel1crash, cause still unknown; gdxn_logf runs at boot). Off
# until a hardware A/B clears it. Set before add_subdirectory() so the vendored citro3d sees it.
option(GDX_3DS_HOTCOLD_COLD "C5-HOTCOLD: apply __attribute__((cold)) to extracted slow paths" OFF)
if(NOT GDX_3DS_HOTCOLD_COLD)
    add_compile_definitions(GDX3DS_HOTCOLD_NO_COLD_ATTR)
endif()

if(NOT GDX_3DS_RDRAM_MB EQUAL 16)
    message(STATUS "G-Diffuser-3DS: RDRAM ARENA KNOB ACTIVE -- ${GDX_3DS_RDRAM_MB} MiB "
                   "(${GDX3DS_RDRAM_BYTES} B) instead of the shipped 16 MiB. Read [rdram-res] "
                   "size= back from the guest before believing it applied.")
endif()

# --- MEMPROBE (-DGDX_3DS_MEMPROBE=ON, DEFAULT OFF) ----------------------------------------
# Compile-time diagnostics for the memory tracks, OFF in every shipped build so the default
# .3dsx carries none of it:
#   * decomp_port.c  -- the RDRAM bump/persist high-water and the top-down non-zero scan that
#                       [rdram-res] reports (the scan is the honest occupancy measurement; it
#                       sees writes that never went through either allocator).
#   * n64_gfx_bridge.cpp -- the highest RDRAM offset any resolver ever admitted, which is the
#                       classification constraint on GDX_3DS_RDRAM_MB.
# Neither changes an allocation size, an allocation order or a frame; both are max-updates.
option(GDX_3DS_MEMPROBE
       "Compile the RDRAM occupancy + classification probes ([rdram-res] fields)" OFF)
if(GDX_3DS_MEMPROBE)
    add_compile_definitions(GDX_MEMPROBE=1)
    message(STATUS "G-Diffuser-3DS: MEMPROBE ACTIVE -- [rdram-res] reports real high-waters. "
                   "This is a measurement build, not the shipped one.")
endif()

# c5submit: citro3d is built from the vendored 1.7.1 source (port/3ds/vendor/citro3d), compiled
# with the prebuilt library's own flags, so the P3a submission diet can live inside it. The diet
# is a runtime lever ([native] c3d_diet, default 0 = the stock code paths). OFF links devkitPro's
# prebuilt libcitro3d.a instead (the A/B for "does vendoring alone change anything").
option(GDX_3DS_VENDOR_CITRO3D "Link the vendored citro3d (port/3ds/vendor/citro3d)" ON)
if(GDX_3DS_VENDOR_CITRO3D)
    add_subdirectory(vendor/citro3d)
    set(GDX3DS_CITRO3D_LIB gdx3ds_citro3d)
    message(STATUS "G-Diffuser-3DS: citro3d = vendored 1.7.1 (gdx3ds_citro3d)")
else()
    set(GDX3DS_CITRO3D_LIB "${CTR_ROOT}/lib/libcitro3d.a")
    message(STATUS "G-Diffuser-3DS: citro3d = prebuilt ${GDX3DS_CITRO3D_LIB}")
endif()

add_subdirectory(gfx)
add_subdirectory(os)
add_subdirectory(audio)
add_subdirectory(assets)
add_subdirectory(net)     # hwlink: WiFi dev link (3DS-only inside)
add_subdirectory(game)      # M1 integration: decomp game + bridge layer (3DS-only inside)
add_subdirectory(lus_glue)  # M1 integration: LUS link-stub TUs (3DS-only inside)
if(NINTENDO_3DS)
    add_subdirectory(harness) # DL replay test app (gdx3ds-dl-tests.3dsx)
endif()

add_executable(G-Diffuser-3DS main_3ds.cpp)
target_include_directories(G-Diffuser-3DS PRIVATE include)
# gdx3ds_build_id.h: see the gdx3ds_build_id_header target's comment above. Every consumer of
# GDX3DS_BUILD_ID now gets it by #include instead of a configure-time COMPILE_DEFINITIONS, so
# add_dependencies is what actually orders "header regenerated" before "TU compiled".
target_include_directories(G-Diffuser-3DS PRIVATE "${GDX3DS_BUILD_ID_HEADER_DIR}")
add_dependencies(G-Diffuser-3DS gdx3ds_build_id_header)
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
        $<TARGET_OBJECTS:gdx3ds_n64>   # [native] interp: owned interpreter (weak-hooked from the bridge)
    )
    target_link_libraries(G-Diffuser-3DS PRIVATE
        gdx3ds_gfx
        gdx3ds_lus_carve
        gdx3ds_zipshim
        gdx3ds_cxd4_rsp
        gdx3ds_os      # again after carve/glue: fiber + config symbols
        gdx3ds_net     # HWLINK: SOC log link + control listener ([net] enabled, DEFAULT OFF)
    )
    set_target_properties(G-Diffuser-3DS PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS ON
    )

    # REL1CRASH bisect titles: -DGDX_3DS_TITLE_TAG=X gives a build its own log-X.txt /
    # crash-X.txt, HOME-menu name "G-Diffuser X" and (with GDX_3DS_CIA_UNIQUE_ID) its own
    # title id, so several variants install side by side and share ONE gdiffuser.ini.
    # Empty (the default) = the shipped names and title 000400000FF3D500, byte-for-byte.
    # EKSHIP (o3dsek-plan.md §7, S1): an Expansion Kit build is a different compile of the whole game
    # (B11a) with its own save (saves/fzerox-ek.sav), so it is its own title: TITLE_TAG=EK and UniqueId
    # 0xff3d6 by default (title 000400000FF3D600, HOME name "G-Diffuser EK", log-EK.txt), installed
    # beside the shipped game and sharing its SD folder (ini, fzerox.o2r, media/, saves/).
    if(GDX_3DS_EXPANSION_KIT)
        set(_gdx3ds_title_tag_default "EK")
        set(_gdx3ds_uid_default "0xff3d6")
    else()
        set(_gdx3ds_title_tag_default "")
        set(_gdx3ds_uid_default "0xff3d5")
    endif()
    set(GDX_3DS_TITLE_TAG "${_gdx3ds_title_tag_default}" CACHE STRING
        "REL1CRASH: bisect-title tag (log-<tag>.txt, crash-<tag>.txt, HOME name); empty = shipped; EK builds default EK")
    set(GDX_3DS_CIA_UNIQUE_ID "${_gdx3ds_uid_default}" CACHE STRING
        "REL1CRASH: CIA UniqueId (0xff3d5 = the game, 0xff3d6 = the EK title; use a distinct id for a bisect title)")
    if(GDX_3DS_TITLE_TAG AND NOT GDX_3DS_TITLE_TAG MATCHES "^[A-Za-z0-9_]+$")
        message(FATAL_ERROR "GDX_3DS_TITLE_TAG=${GDX_3DS_TITLE_TAG}: letters, digits, _ only.")
    endif()
    if(GDX_3DS_EXPANSION_KIT AND GDX_3DS_CIA_UNIQUE_ID STREQUAL "0xff3d5")
        message(FATAL_ERROR "GDX_3DS_EXPANSION_KIT=ON with GDX_3DS_CIA_UNIQUE_ID=0xff3d5: an EK CIA would "
                            "install OVER the shipped game. Use 0xff3d6 (the EK title) or another id; a build "
                            "dir first configured without the EK keeps its cached id, so pass it explicitly.")
    endif()
    # N60CORE1 (docs/research/native/n60core1-progress.md): the CIA's core-1 CAP. The exheader
    # byte is 0x80|pct (bit 7 = the scheduling mode the rsf already selects); pm refuses any
    # APT_SetAppCpuTimeLimit above pct. 30 (the default) keeps packaging/gdiffuser.rsf verbatim
    # (MaxCpu 0x9E). The cap is NOT the grant: the game still asks for [debug] syscore_pct
    # (default 30) and pm starts every app at 0, so a raised cap changes nothing until an ini
    # asks for more. The Old 3DS unit is CIA-only (no hbmenu), so probe P needs this above 30.
    set(GDX_3DS_CIA_MAXCPU "30" CACHE STRING
        "N60CORE1: CIA core-1 cap in percent (rsf MaxCpu = 0x80|N); 30 = the shipped rsf verbatim")
    if(NOT GDX_3DS_CIA_MAXCPU MATCHES "^[0-9]+$" OR GDX_3DS_CIA_MAXCPU LESS 5 OR GDX_3DS_CIA_MAXCPU GREATER 89)
        message(FATAL_ERROR "GDX_3DS_CIA_MAXCPU=${GDX_3DS_CIA_MAXCPU}: a percent in 5..89.")
    endif()
    # n60boot: the shipped rsf maps VRAM READ-ONLY for the CPU (`1f000000-1f5fffff:r`). ON maps it
    # read-write, like hbldr's .3dsx environment. DEFAULT OFF and not used by any card: with
    # [native] lcd_vram=1 only the top screen (GPU-written) is in VRAM, so no CPU store targets
    # VRAM; this exists only as a documented fallback should a CPU writer to VRAM ever be needed.
    option(GDX_3DS_CIA_VRAM_RW "n60boot: CIA maps VRAM read-write (default OFF: the rsf's read-only map)" OFF)
    set(GDX3DS_LOG_TAG_DEF "")
    if(GDX_3DS_TITLE_TAG)
        set(GDX3DS_LOG_TAG_DEF "GDX3DS_LOG_TAG=\"-${GDX_3DS_TITLE_TAG}\"")
        message(STATUS "G-Diffuser-3DS: BISECT TITLE '${GDX_3DS_TITLE_TAG}' uid=${GDX_3DS_CIA_UNIQUE_ID} "
                       "-> log-${GDX_3DS_TITLE_TAG}.txt, crash-${GDX_3DS_TITLE_TAG}.txt")
    endif()

    # H-HARDWARE: SD file-log sink for the boot/watchdog/fatal tracers (svc debug goes
    # nowhere on real hardware). Integration-level like main_3ds.cpp, not a stream dir.
    target_sources(G-Diffuser-3DS PRIVATE gdx3ds_filelog.c)
    if(GDX3DS_LOG_TAG_DEF)
        set_source_files_properties(gdx3ds_filelog.c PROPERTIES COMPILE_DEFINITIONS
            "${GDX3DS_LOG_TAG_DEF}")
    endif()

    # REL1CRASH: ARM11 exception handler on every thread ([debug] crashdump, default 1 on an
    # Old 3DS) + the svcBreak tracer. --wrap=threadCreate arms each libctru thread at entry;
    # --wrap=svcBreak logs every library panic's caller before the break. Contract:
    # include/gdx3ds_crash.h.
    target_sources(G-Diffuser-3DS PRIVATE gdx3ds_crash.c)
    # GDX3DS_BUILD_ID: gdx3ds_crash.c #includes the generated header now (see
    # gdx3ds_build_id_header above) instead of taking it as a COMPILE_DEFINITIONS.
    if(GDX3DS_LOG_TAG_DEF)
        set_source_files_properties(gdx3ds_crash.c PROPERTIES COMPILE_DEFINITIONS
            "${GDX3DS_LOG_TAG_DEF}")
    endif()
    target_link_options(G-Diffuser-3DS PRIVATE
        "LINKER:--wrap=threadCreate" "LINKER:--wrap=svcBreak")

    # n60boot (docs/research/native/n60boot-progress.md): the crash-proof boot trail
    # (sdmc:/3ds/gdiffuser/boot<tag>.txt, one FS_WRITE_FLUSH write per boot milestone from main()
    # entry; DEFAULT ON, ini off-switch [debug] boot_trail=0), the VRAM guard ([debug] vram_guard)
    # and per-screen LCD framebuffer memory: vendor/libctru_gfx/gfx_split.c is an altered copy of
    # libctru v2.7.0's gfx.c linked as an object, so libctru.a's gfx.o is never pulled.
    option(GDX_3DS_BOOT_TRAIL "n60boot: compile the boot trail in (opens at main() entry)" ON)
    if(GDX_3DS_BOOT_TRAIL)
        set(_gdx3ds_boot_trail_on 1)
    else()
        set(_gdx3ds_boot_trail_on 0)
    endif()
    target_sources(G-Diffuser-3DS PRIVATE gdx3ds_boot_trail.c gdx3ds_vram_guard.c vendor/libctru_gfx/gfx_split.c)
    # GDX3DS_BUILD_ID: gdx3ds_boot_trail.c #includes the generated header now (see
    # gdx3ds_build_id_header above) instead of taking it as a COMPILE_DEFINITIONS.
    set_source_files_properties(gdx3ds_boot_trail.c PROPERTIES COMPILE_DEFINITIONS
        "GDX3DS_BOOT_TRAIL=${_gdx3ds_boot_trail_on};${GDX3DS_LOG_TAG_DEF}")

    # RENDER THREAD (LOCKED-60 Task H): core-2 render thread, [debug] renderthread killswitch.
    target_sources(G-Diffuser-3DS PRIVATE gdx3ds_renderthread.cpp)

    # FPS-HUD: bottom-screen fps counter (gdiffuser.ini [debug] fps, DEFAULT ON).
    # Integration-level like gdx3ds_filelog.c. The HUD's static row bakes the build-time
    # build id (branch@hash of HEAD, refreshed every build -- see gdx3ds_build_id_header
    # above) so on-device reports can name the build they actually came from.
    target_sources(G-Diffuser-3DS PRIVATE gdx3ds_fps_hud.c)

    # O3DS BUDGET: [mem-budget] grant receipt + [mem-res] ranked residency census
    # (gdiffuser.ini [debug] membudget, DEFAULT OFF). Diagnostic only; every call site
    # returns on one file-static load when the lever is off.
    target_sources(G-Diffuser-3DS PRIVATE gdx3ds_membudget.cpp)

    # C4c CUT #1: the Fast3D decoded-texture staging reservation ([debug] tex_upload_kb,
    # DEFAULT 0 = today's size exactly). Fast::Interpreter::Init holds one
    # malloc(GetMaxTextureSize()^2 * 4) = 4,194,304 B for the life of the process, which the
    # C4c allocation census put at rank 3 of the whole heap and which no [mem-res] row names.
    # An ini lever rather than a build knob because Fast3D's Init runs long after
    # gdiffuser.ini is read. See port/3ds/gdx3ds_texupload.c.
    target_sources(G-Diffuser-3DS PRIVATE gdx3ds_texupload.c)

    # O3DS GRANT KNOB (NEXT.md C4b / C4e) -- DEFAULT OFF, and off means byte-identical.
    #
    # libctru's __system_allocateHeaps() carves the malloc heap and the linear heap out of
    # the process commit budget from crt0, before main() and before any static constructor,
    # so NO ini lever can ever reach it. The only handle is the pair of weak symbols it reads
    # on entry; gdx3ds_heapcfg.c defines them strongly when -- and only when -- one of the two
    # cache variables below is non-zero. With both at 0 the TU defines no __ctru_* symbol and
    # contributes three constant-returning accessors for the [mem-budget] receipt.
    #
    #   -DGDX_3DS_SIM_AVAIL_KIB=N   C4e. Pin BOTH grants to what libctru would compute on a
    #                               console whose commit budget is N KiB. Azahar's "Old 3DS"
    #                               is memtype 2 (96 MB extended), so the emulator lane is
    #                               MORE generous than a real Old 3DS and cannot otherwise
    #                               reproduce the 64 MB memtype-0 constraint. This is how it
    #                               gets an unattended gate.
    #   -DGDX_3DS_SIM_O3DS=ON       shorthand for the memtype-0 budget: 54640 KiB =
    #                               55,951,360 B = the 64 MB APPLICATION region minus the
    #                               11,157,504 B of image+bss+slack that BOTH measured
    #                               profiles (memtype 6 and memtype 2) show for this .3dsx.
    #                               A CIA gets LESS: the real Old 3DS printed availBoot=
    #                               55,660,544 (54356 KiB). To simulate the O3DS CIA under a
    #                               linear pin, pass -DGDX_3DS_SIM_AVAIL_KIB=54356 instead,
    #                               or the simulated heap is 290,816 B more generous than the
    #                               console's (heapcfg host test case 7).
    #   -DGDX_3DS_LINEAR_HEAP_KIB=N C4b. Pin ONLY the linear grant and let libctru compute
    #                               heap = avail - linear, which is the branch with no 24 MiB
    #                               cap. This is the shape the cut would ship in.
    #
    # HAZARD: __system_allocateHeaps svcBreaks (USERBREAK_PANIC) if the PINNED heap+linear
    # exceeds the device's real budget, in crt0, with no log line and no [fatal] -- a black
    # screen. A GDX_3DS_SIM_AVAIL_KIB above the host console's own budget is that crash.
    option(GDX_3DS_SIM_O3DS
           "Simulate the Old 3DS memtype-0 (64 MB) commit budget so the emulator reproduces the real grant" OFF)
    set(GDX_3DS_SIM_AVAIL_KIB "0" CACHE STRING
        "C4e: simulate a commit budget of N KiB (0 = off). GDX_3DS_SIM_O3DS=ON sets 54640.")
    set(GDX_3DS_LINEAR_HEAP_KIB "0" CACHE STRING
        "C4b: pin __ctru_linear_heap_size to N KiB (0 = off, libctru's default split).")

    set(GDX3DS_SIM_AVAIL_KIB "${GDX_3DS_SIM_AVAIL_KIB}")
    if(GDX_3DS_SIM_O3DS AND GDX3DS_SIM_AVAIL_KIB EQUAL 0)
        set(GDX3DS_SIM_AVAIL_KIB 54640)   # 55,951,360 B -- see the host test's case 3
    endif()
    math(EXPR GDX3DS_SIM_AVAIL_BYTES "${GDX3DS_SIM_AVAIL_KIB} * 1024")
    set(GDX3DS_LINEAR_HEAP_KIB "${GDX_3DS_LINEAR_HEAP_KIB}")
    if(GDX_3DS_O3DS AND GDX3DS_LINEAR_HEAP_KIB EQUAL 0)
        set(GDX3DS_LINEAR_HEAP_KIB ${GDX3DS_O3DS_LINEAR_KIB})
    endif()
    math(EXPR GDX3DS_PIN_LINEAR_BYTES "${GDX3DS_LINEAR_HEAP_KIB} * 1024")
    target_sources(G-Diffuser-3DS PRIVATE gdx3ds_heapcfg.c)
    set_source_files_properties(gdx3ds_heapcfg.c PROPERTIES COMPILE_DEFINITIONS
        "GDX3DS_SIM_AVAIL_BYTES=${GDX3DS_SIM_AVAIL_BYTES}UL;GDX3DS_PIN_LINEAR_BYTES=${GDX3DS_PIN_LINEAR_BYTES}UL")
    if(NOT GDX3DS_SIM_AVAIL_BYTES EQUAL 0 OR NOT GDX3DS_PIN_LINEAR_BYTES EQUAL 0)
        message(STATUS "G-Diffuser-3DS: HEAP KNOB ACTIVE -- simAvail=${GDX3DS_SIM_AVAIL_BYTES} B, "
                       "pinLinear=${GDX3DS_PIN_LINEAR_BYTES} B. This is NOT the shipped memory layout; "
                       "read [mem-budget] heapCfg=/heapGrant= back from the guest before believing it applied.")
    endif()
    # [mem-hwm] (gdx3ds_membudget.cpp): the exact sbrk high-water against the heap grant, i.e.
    # the real headroom, which [mem-res] heapFree= (the free list) is not. Compiled into every
    # memory-knob build and no other, so the shipped New 3DS build stays the same translation.
    if(GDX_3DS_O3DS OR NOT GDX_3DS_RDRAM_MB EQUAL 16 OR NOT GDX3DS_SIM_AVAIL_BYTES EQUAL 0
       OR NOT GDX3DS_PIN_LINEAR_BYTES EQUAL 0)
        target_compile_definitions(G-Diffuser-3DS PRIVATE GDX3DS_MEM_HWM_RECEIPT=1)
    endif()
    if(GDX_3DS_O3DS)
        message(STATUS "G-Diffuser-3DS: OLD 3DS PROFILE -- RDRAM ${GDX_3DS_RDRAM_MB} MiB, linear heap "
                       "pinned to ${GDX3DS_LINEAR_HEAP_KIB} KiB. Ship with [debug] audio_stream=1 "
                       "tex_upload_kb=512 (o3dsfit-progress.md).")
    endif()

    # --- C4c: MALLOC INTERPOSITION + HEAP ATTRIBUTION -------------------------------------
    #
    # port/3ds/include/gdx3ds_heapattr.h carries the contract. Two knobs, deliberately
    # separate, because they trade off completely differently:
    #
    #   -DGDX_3DS_WRAP_MALLOC=ON   DEFAULT ON. The universal allocation-failure receipt.
    #                              `rdramdiet` landed [alloc-fail] at the C sites holding the
    #                              big blocks and stated the gap: the 7,138,708 B the census
    #                              could not attribute stayed silent. Under a tight grant this
    #                              port HANGS with a frozen frame counter rather than crashing
    #                              (o3dssim §3.1), which on a console is indistinguishable
    #                              from a deadlock by eye -- so a silent NULL is the most
    #                              expensive thing in the build. This makes every one of them
    #                              print its size AND its caller.
    #                              Cost: `malloc -> _malloc_r` becomes `__wrap_malloc ->
    #                              _malloc_r`, the SAME call depth (the wrapper REPLACES
    #                              newlib's thunk rather than wrapping it), plus one NULL
    #                              compare. OFF restores the original link exactly.
    #
    #   -DGDX_3DS_HEAPATTR=ON      DEFAULT OFF, requires the above. The ranked [heap-attr]
    #                              census: a live ptr -> (size, return address) map, so the
    #                              heap bytes [mem-res] cannot name get named. ~2 MB of .bss
    #                              and a lock on every allocation -- a MEASUREMENT build,
    #                              never a shipped one, and it does not fit a real memtype-0
    #                              console precisely because of that .bss.
    #
    # The wrap set is the `_r` PRIMITIVES plus the public entry points, not `malloc`/`free`
    # as C4c was originally specified: in devkitARM's newlib, 26 objects in libc.a call
    # `_malloc_r` directly against 17 that call `malloc`, so wrapping only the public names
    # would have measured a fraction of the C side and reported it as all of it.
    option(GDX_3DS_WRAP_MALLOC
           "Interpose newlib's allocator so every allocation failure prints [alloc-fail]" ON)
    option(GDX_3DS_HEAPATTR
           "Compile the live allocation map behind the [heap-attr] ranked census (measurement build)" OFF)
    if(GDX_3DS_HEAPATTR AND NOT GDX_3DS_WRAP_MALLOC)
        message(FATAL_ERROR
            "GDX_3DS_HEAPATTR=ON needs GDX_3DS_WRAP_MALLOC=ON: the map is populated by the "
            "wrappers, and without them it would print an empty table that reads like a "
            "measurement.")
    endif()
    # The TU is compiled either way so gdx3ds_membudget.cpp keeps a definition of the two
    # census entry points; with the option OFF everything inside it compiles away and no
    # --wrap reaches the linker, which is what makes "OFF is the original link" exact.
    target_sources(G-Diffuser-3DS PRIVATE gdx3ds_mallocwrap.c)
    if(GDX_3DS_WRAP_MALLOC)
        set(GDX3DS_MALLOCWRAP_DEFS "GDX3DS_WRAP_MALLOC=1")
        target_link_options(G-Diffuser-3DS PRIVATE
            "LINKER:--wrap=_malloc_r" "LINKER:--wrap=_free_r" "LINKER:--wrap=_calloc_r"
            "LINKER:--wrap=_realloc_r" "LINKER:--wrap=_memalign_r"
            "LINKER:--wrap=malloc" "LINKER:--wrap=free" "LINKER:--wrap=calloc"
            "LINKER:--wrap=realloc" "LINKER:--wrap=memalign")
        if(GDX_3DS_HEAPATTR)
            list(APPEND GDX3DS_MALLOCWRAP_DEFS "GDX3DS_HEAPATTR=1")
            message(STATUS "G-Diffuser-3DS: HEAPATTR ACTIVE -- [heap-attr] ranked allocation "
                           "census compiled in (~2 MB of .bss + a lock per allocation). This is "
                           "a measurement build, not the shipped one.")
        endif()
        set_source_files_properties(gdx3ds_mallocwrap.c PROPERTIES COMPILE_DEFINITIONS
            "${GDX3DS_MALLOCWRAP_DEFS}")
    else()
        message(STATUS "G-Diffuser-3DS: WRAP_MALLOC OFF -- no universal [alloc-fail] receipt. "
                       "A C-side allocation failure will be silent again.")
    endif()

    # MENU: v1 bottom-screen touch menu (gdiffuser.ini [menu] enabled, DEFAULT ON).
    # Integration-level like the HUD/filelog TUs; shares the HUD's console + build id.
    target_sources(G-Diffuser-3DS PRIVATE gdx3ds_menu.c)

    # BOTTOMHUD: the race map on the bottom screen (gdiffuser.ini [debug] bottom_map,
    # DEFAULT OFF). Sources the picture from minimap.c's own 64x64 CI8 bitmap and the racer
    # positions (port/gdx_racemap.h) and CPU-blits it into the MENU's console framebuffer:
    # no citro3d target, no extra draw, no top-screen change. Contract: gdx3ds_racemap.h.
    target_sources(G-Diffuser-3DS PRIVATE gdx3ds_racemap.c)

    # [ek] course_share (DEFAULT OFF): share Expansion Kit custom courses as .gdxcourse files and
    # QR codes from the menu's CRS tab. Contract: gdx3ds_course_share.h. EK builds only: the
    # shipped (EK=OFF) build compiles none of it and gdx3ds_menu.c without the CRS tab, so its
    # objects stay identical. The game-side MFS half is gdx3ds_course_share_game.c (game/).
    # The codec and QR TUs are pure C and host-tested (gdx3ds_course_share_host_tests).
    if(GDX_3DS_EXPANSION_KIT)
        target_sources(G-Diffuser-3DS PRIVATE gdx3ds_course_share.c gdx3ds_course_share_format.c
                                             gdx3ds_course_share_qr.c vendor/qrcodegen/qrcodegen.c
                                             gdx3ds_course_share_scan.c gdx3ds_course_share_cam.c
                                             vendor/quirc/quirc.c vendor/quirc/decode.c
                                             vendor/quirc/identify.c vendor/quirc/version_db.c)
        # SCAN (camera -> quirc on a worker thread): the vendored decoder is third-party code,
        # built as is (port/3ds/vendor/quirc/PROVENANCE.md).
        set_source_files_properties(vendor/quirc/quirc.c vendor/quirc/decode.c vendor/quirc/identify.c
                                    vendor/quirc/version_db.c PROPERTIES COMPILE_OPTIONS -w)
        set_property(SOURCE gdx3ds_menu.c APPEND PROPERTY COMPILE_DEFINITIONS GDX3DS_COURSE_SHARE=1)
        # The course checksum recomputation must round after every float op, as the game does.
        set_property(SOURCE gdx3ds_course_share_format.c APPEND PROPERTY COMPILE_OPTIONS -ffp-contract=off)
    endif()

    # nibottom Phase 2: the DASH race HUD on the bottom screen (gdiffuser.ini
    # [debug] bottom_race, DEFAULT OFF, and [hud] placement). Sibling of BOTTOMHUD above —
    # same cost model, same MENU-tab wiring, independent snapshot (port/gdx_racemap.h's
    # GdxRaceHudSnapshot). Contract: gdx3ds_bottomhud.h.
    target_sources(G-Diffuser-3DS PRIVATE gdx3ds_bottomhud.c)
    # ... whose game art (fonts, digits, portraits, chrome) is baked once at boot from the user's
    # fzerox.o2r by gdx3ds_hudbake.c's recipes; its Pillow-exact double arithmetic must round
    # after every operation.
    target_sources(G-Diffuser-3DS PRIVATE gdx3ds_hudbake.c)
    set_property(SOURCE gdx3ds_hudbake.c APPEND PROPERTY COMPILE_OPTIONS -ffp-contract=off)
    # The recipe id the SD cache / PC pack is keyed on: the same hash tools/hudpack computes.
    include(${CMAKE_CURRENT_SOURCE_DIR}/hudbake_recipe_id.cmake)
    gdx3ds_hudbake_recipe_id(GDX3DS_HUDBAKE_RECIPE_ID ${CMAKE_CURRENT_SOURCE_DIR})
    set_property(SOURCE gdx3ds_hudbake.c APPEND PROPERTY COMPILE_DEFINITIONS
                 GDX3DS_HUDBAKE_RECIPE_ID=${GDX3DS_HUDBAKE_RECIPE_ID})
    message(STATUS "G-Diffuser-3DS: HUD bake recipe id ${GDX3DS_HUDBAKE_RECIPE_ID}")

    # Bottom-screen product menus (artifacts/bottom-menus-design, [ui] bottom_menus): the UI kit
    # (pure C, a port of the design's mockup engine; host-tested by gdx3ds_bui_host_tests.c). It
    # draws into the DASH's shadow buffer and reads the DASH's atlases through bui_glyph_table()
    # (gdx3ds_bottomhud.c), so it adds no pixels of its own beyond the small extra-glyph/icon set.
    target_sources(G-Diffuser-3DS PRIVATE gdx3ds_bui_kit.c)
    # ... and the screens on top of it: pure painters + the gallery (host-tested too), the 3DS shell
    # (state, input ownership, the MP layer) and the [debug] bui_input scripted input.
    target_sources(G-Diffuser-3DS PRIVATE gdx3ds_bui_screens.c gdx3ds_bui_gallery.c gdx3ds_bui.c
                                         gdx3ds_bui_script.c)
    # ... and the multiplayer race dash (M3): pure painters (host-tested through the gallery) and
    # the shell that drives them from the DASH (gdx3ds_bottomhud.c) and the MP layer.
    target_sources(G-Diffuser-3DS PRIVATE gdx3ds_bui_racedash.c gdx3ds_bui_race.c)
    if(GDX_3DS_EXPANSION_KIT)
        set_property(SOURCE gdx3ds_bui.c APPEND PROPERTY COMPILE_DEFINITIONS GDX3DS_BUI_EK=1)
    endif()

    # nibottom Phase 2, CUSTOM HUD mode: pure clamp/snap/hit-test/ini-line logic (host-testable,
    # no <3ds.h>/decomp deps -- see gdx3ds_hudcustom_layout.h). The weak stand-in stub
    # (gdx3ds_hudcustom_stub.c) is gone as of this merge -- gdx3ds_hudcustom.h now just includes
    # the real port/gdx_hud_placement.h, so there is nothing left to link in here.
    target_sources(G-Diffuser-3DS PRIVATE gdx3ds_hudcustom_layout.c)

    # nihudtop Phase 3: dev-only [debug] hud_custom_top_mask/hud_custom_active lever (see the
    # file's own header). Independent of gdx3ds_bottomhud.c on purpose -- own TU, own ini keys,
    # checked before gdx_hud_placement_mode() is ever read.
    target_sources(G-Diffuser-3DS PRIVATE gdx3ds_hudtop_custom.c)

    # DYNLOD (LOCKED-60 Task J): automatic rival-detail tier from the render task's wall
    # time ([perf] rival_detail_auto, DEFAULT ON; manual [perf] rival_detail is the floor).
    target_sources(G-Diffuser-3DS PRIVATE gdx3ds_dynlod.c)
    # PERF PRESET (NEXT.md C4p): [perf] preset = auto|off|o3ds re-defaults the scene-density
    # levers per console class (auto = o3ds on an Old 3DS only); explicit [perf] keys win.
    target_sources(G-Diffuser-3DS PRIVATE gdx3ds_perf_preset.c)
    # WIDESCREEN: [display] widescreen (DEFAULT 0) -- race frames at the top screen's 5:3 with
    # the PC port's RemoveBorders/UltrawideMode/WidescreenUI switches. Contract: gdx3ds_widescreen.h.
    target_sources(G-Diffuser-3DS PRIVATE gdx3ds_widescreen.c)
    # n3ds: `[perf] preset = n60`, the n60 race stack as config-layer defaults (gdx3ds_preset_n60.h).
    target_sources(G-Diffuser-3DS PRIVATE gdx3ds_preset_n60.c)
    # preset-fix: the ni_* native levers are rows of both preset tables now (the GDX_3DS_PRESET_NI
    # staging option is gone; tools/ci-3ds.sh gdx3ds_preset_mustship_tests guards the rows).
    # MAINPROF (NEXT.md A8): [mprof] main-thread section profiler, gdiffuser.ini
    # [debug] mainprof (DEFAULT OFF). Diagnostic only; every hook site is one int
    # load + branch when the gate is off.
    target_sources(G-Diffuser-3DS PRIVATE gdx3ds_mainprof.c)
    # HWPROF (docs/research/native/hwprof-progress.md): [debug] c5pmu (CP15 counters per [c5cat]
    # bucket) and [debug] hwsite (per-call micro-timers at the hardware suspects). DEFAULT OFF;
    # diagnostic only, zero heap. Contract: gdx3ds_hwprof.h.
    target_sources(G-Diffuser-3DS PRIVATE gdx3ds_hwprof.c)
    # GXCOST (onesubmit, docs/research/native/onesubmit-progress.md): [debug] gxcost, the CPU cost
    # of GX / GSP dispatch per thread (main, the GSP event thread, others) and the GX interrupts per
    # frame. DEFAULT OFF; off, each wrapper is the real call behind one load + branch. Contract:
    # include/gdx3ds_gxcost.h.
    target_sources(G-Diffuser-3DS PRIVATE gdx3ds_gxcost.c)
    target_link_options(G-Diffuser-3DS PRIVATE
        "LINKER:--wrap=GSPGPU_FlushDataCache" "LINKER:--wrap=svcFlushProcessDataCache"
        "LINKER:--wrap=gspSubmitGxCommand" "LINKER:--wrap=GX_ProcessCommandList"
        "LINKER:--wrap=gxCmdQueueRun" "LINKER:--wrap=gxCmdQueueWait" "LINKER:--wrap=gspWaitForEvent"
        "LINKER:--wrap=svcWaitSynchronization" "LINKER:--wrap=C3D_FrameBegin" "LINKER:--wrap=C3D_FrameEnd")
    # C5TICK (docs/research/native/c5tick-progress.md): the per-tick DL build split
    # ([debug] dlbprof), the write-set check ([debug] dlwrite) and hollow skipped ticks
    # ([debug] dl_hollow). All DEFAULT OFF. Contract: gdx3ds_c5tick.h.
    target_sources(G-Diffuser-3DS PRIVATE gdx3ds_c5tick.c)
    # S5b (o3dsek-plan.md section 6): the course editor's test-drive race scope
    # ([native] nrr_testdrive) is `#ifdef EXPANSION_KIT` inside this one TU, so the define goes
    # on this source only -- the rest of the target has never been built with it. OFF: nothing
    # added, and the shipped .3dsx stays byte-identical.
    if(GDX_3DS_EXPANSION_KIT)
        set_property(SOURCE gdx3ds_c5tick.c APPEND PROPERTY COMPILE_DEFINITIONS EXPANSION_KIT=1)
    endif()
    # HITCH: the per-frame hitch receipt ([debug] hitch, default 0); counters in gfx/gdx3ds_hitch_ev.c.
    target_sources(G-Diffuser-3DS PRIVATE gdx3ds_hitch.c)
    # n60st3: the frame-spike attribution receipt ([debug] spike_ms, default 0).
    target_sources(G-Diffuser-3DS PRIVATE gdx3ds_spike.c)
    # N60TEX: [debug] autodrive (default 0), the CPU driver for player 1 (measurement lever).
    target_sources(G-Diffuser-3DS PRIVATE gdx3ds_autodrive.c)
    # nimenu: [debug] force_mode (default -1 = off), a one-shot boot-target override for
    # screens with no menu path and no finishable replay (measurement lever).
    target_sources(G-Diffuser-3DS PRIVATE gdx3ds_force_mode.c)
    # N60TICK: the core-1 probe ([debug] core1_probe, default 0). Contract: gdx3ds_core1probe.h.
    target_sources(G-Diffuser-3DS PRIVATE gdx3ds_core1probe.c)
    # N60TICK4: tick-ahead on the Old 3DS ([debug] tick_ahead, default 0). Contract: gdx3ds_tickahead.h.
    target_sources(G-Diffuser-3DS PRIVATE gdx3ds_tickahead.c)
    # mp-snapshot: game-state snapshot / restore + the synctest gate and the hardware probe
    # ([debug] snap_test / snap_probe, both default 0). Contract: ../gdx_snapshot.h. The link's
    # script is the installed 3dsx.ld plus two INCLUDE fragments that group the decomp simulation
    # state into one .data and one .bss block (gdx3ds_snapshot_ld.cmake).
    # snap-fast: the copy engine (LDM/STM line bursts, dirty-block sync), gdx3ds_snapcopy.h.
    target_sources(G-Diffuser-3DS PRIVATE gdx_snapshot.c gdx3ds_snapcopy.c)
    include(${CMAKE_CURRENT_SOURCE_DIR}/gdx3ds_snapshot_ld.cmake)
    gdx3ds_snapshot_link_setup(G-Diffuser-3DS)
    # N60LAUNCH: launch variants ([debug] launch_variants, default 0). Contract: gdx3ds_launch.h;
    # the selection rules (gdx3ds_launch_core.c) are host-tested (gdx3ds_launch_host_tests).
    target_sources(G-Diffuser-3DS PRIVATE gdx3ds_launch.c gdx3ds_launch_core.c)
    # N60SHIPS: [debug] ships_census / logic_split read-only receipts (default 0). Contract: gdx3ds_ships.h.
    target_sources(G-Diffuser-3DS PRIVATE gdx3ds_ships.c)
    # FRAMESKIP (NEXT.md C4l): 60 Hz game logic with skipped renders, gdiffuser.ini
    # [debug] frameskip = N | auto (DEFAULT 0 = off). rtMode 0 only. Contract:
    # gdx3ds_frameskip.h; the pure governor is host-tested (gdx3ds_frameskip_host_tests).
    target_sources(G-Diffuser-3DS PRIVATE gdx3ds_frameskip.c gdx3ds_frameskip_gov.c gdx3ds_frameskip_steady.c
                                         gdx3ds_frameskip_race0.c # n60fs: frameskip = race0
                                         gdx3ds_pipe.c) # o3ds60: top triple buffer / dash_pre_frame levers
    # ORACLE: true frame capture + [fh] receipts (docs/research/native/03-oracle-plan.md).
    target_sources(G-Diffuser-3DS PRIVATE gdx3ds_oracle.c)
    # GDX3DS_BUILD_ID: gdx3ds_oracle.c #includes the generated header now (see
    # gdx3ds_build_id_header above) instead of taking it as a COMPILE_DEFINITIONS.

    # H-HARDWARE: proper SMDH (48x48 icon + titles/publisher) instead of the devkitPro
    # placeholder. Assets + provenance/licensing: packaging/README.md.
    set(GDX3DS_SMDH "${CMAKE_CURRENT_BINARY_DIR}/G-Diffuser-3DS.smdh")
    set(GDX3DS_SMDH_NAME "G-Diffuser")
    if(GDX_3DS_TITLE_TAG)
        set(GDX3DS_SMDH_NAME "G-Diffuser ${GDX_3DS_TITLE_TAG}")
    endif()
    set(GDX3DS_SMDH_DESC "G-Diffuser — F-Zero X decompilation port")
    set(GDX3DS_BANNER_PNG "${CMAKE_CURRENT_SOURCE_DIR}/packaging/banner.png")
    if(GDX_3DS_EXPANSION_KIT)
        # EKSHIP: the EK title's own HOME text and banner (packaging/README.md).
        set(GDX3DS_SMDH_DESC "G-Diffuser — F-Zero X Expansion Kit (64DD)")
        set(GDX3DS_BANNER_PNG "${CMAKE_CURRENT_SOURCE_DIR}/packaging/banner-ek.png")
    endif()
    ctr_generate_smdh(
        OUTPUT "${GDX3DS_SMDH}"
        NAME "${GDX3DS_SMDH_NAME}"
        DESCRIPTION "${GDX3DS_SMDH_DESC}"
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
        # REL1CRASH: the rsf the CIA is made from. Shipped: the packaging file verbatim.
        # A bisect title: a generated copy with its own UniqueId and Title.
        set(GDX3DS_RSF "${CMAKE_CURRENT_SOURCE_DIR}/packaging/gdiffuser.rsf")
        if(GDX_3DS_TITLE_TAG OR NOT GDX_3DS_CIA_UNIQUE_ID STREQUAL "0xff3d5" OR NOT GDX_3DS_CIA_MAXCPU EQUAL 30
           OR GDX_3DS_CIA_VRAM_RW)
            file(READ "${GDX3DS_RSF}" _rsf)
            string(REGEX REPLACE "UniqueId( +): 0xff3d5" "UniqueId\\1: ${GDX_3DS_CIA_UNIQUE_ID}"
                   _rsf "${_rsf}")
            if(GDX_3DS_TITLE_TAG)
                string(REGEX REPLACE "Title( +): G-Diffuser" "Title\\1: GDX-${GDX_3DS_TITLE_TAG}"
                       _rsf "${_rsf}")
            endif()
            if(NOT GDX_3DS_CIA_MAXCPU EQUAL 30)
                math(EXPR _maxcpu "128 + ${GDX_3DS_CIA_MAXCPU}" OUTPUT_FORMAT HEXADECIMAL)
                string(REGEX REPLACE "MaxCpu( +): 0x9E[^\n]*" "MaxCpu\\1: ${_maxcpu} # N60CORE1 cap ${GDX_3DS_CIA_MAXCPU}%"
                       _rsf "${_rsf}")
                if(NOT _rsf MATCHES "MaxCpu +: ${_maxcpu}")
                    message(FATAL_ERROR "GDX_3DS_CIA_MAXCPU: the rsf has no 'MaxCpu : 0x9E' line to replace")
                endif()
                message(STATUS "G-Diffuser-3DS: CIA core-1 cap MaxCpu ${_maxcpu} (${GDX_3DS_CIA_MAXCPU}%)")
            endif()
            if(GDX_3DS_CIA_VRAM_RW)
                string(REGEX REPLACE "- 1f000000-1f5fffff:r( *)#" "- 1f000000-1f5fffff  \\1# n60boot GDX_3DS_CIA_VRAM_RW: read-write"
                       _rsf "${_rsf}")
                if(_rsf MATCHES "1f000000-1f5fffff:r")
                    message(FATAL_ERROR "GDX_3DS_CIA_VRAM_RW: the rsf's VRAM mapping line was not rewritten")
                endif()
                message(STATUS "G-Diffuser-3DS: CIA maps VRAM READ-WRITE (GDX_3DS_CIA_VRAM_RW)")
            endif()
            set(GDX3DS_RSF "${CMAKE_CURRENT_BINARY_DIR}/gdiffuser-variant.rsf")
            file(WRITE "${GDX3DS_RSF}" "${_rsf}")
        endif()
        add_custom_command(
            OUTPUT "${GDX3DS_BANNER}"
            COMMAND "${GDX_BANNERTOOL_EXE}" makebanner
                -i "${GDX3DS_BANNER_PNG}"
                -a "${CMAKE_CURRENT_SOURCE_DIR}/packaging/banner.wav"
                -o "${GDX3DS_BANNER}"
            DEPENDS
                "${GDX3DS_BANNER_PNG}"
                "${CMAKE_CURRENT_SOURCE_DIR}/packaging/banner.wav"
            COMMENT "Building HOME-menu banner G-Diffuser-3DS.bnr"
            VERBATIM
        )
        add_custom_command(
            OUTPUT "${GDX3DS_CIA}"
            COMMAND "${GDX_MAKEROM_EXE}" -f cia -o "${GDX3DS_CIA}" -target t -exefslogo
                -elf "$<TARGET_FILE:G-Diffuser-3DS>"
                -rsf "${GDX3DS_RSF}"
                -icon "${GDX3DS_SMDH}"
                -banner "${GDX3DS_BANNER}"
            DEPENDS
                G-Diffuser-3DS
                "${GDX3DS_SMDH}"
                "${GDX3DS_BANNER}"
                "${GDX3DS_RSF}"
            COMMENT "Building CIA G-Diffuser-3DS.cia (UniqueId ${GDX_3DS_CIA_UNIQUE_ID})"
            VERBATIM
        )
        add_custom_target(G-Diffuser-3DS-cia DEPENDS "${GDX3DS_CIA}")
    else()
        message(STATUS "G-Diffuser-3DS-cia target disabled: need makerom + bannertool "
                       "(see port/3ds/packaging/README.md)")
    endif()

    # C4o: per-target/per-TU -O levels + the hot-first .text order (no-op for `stock`).
    gdx3ds_codegen_apply()
endif()
