# port/cmake/GdxGameSources.cmake — the decomp game-source list, its option-driven
# filters, and the port-side game-core TU lists, shared by the desktop build
# (port/CMakeLists.txt) and the 3DS build (port/3ds/game/CMakeLists.txt).
#
# include() this file; it defines:
#   DECOMP                 — absolute path to the decomp submodule
#   GDX_PORT_DIR           — absolute path to port/
#   GAME_SOURCES           — decomp game C sources (post-filter, option-dependent)
#   GDX_GAME_CORE_SOURCES  — always-on port TUs compiled with the decomp sources
#   GDX_GAME_EK_PORT_SOURCES — EK-only port TUs (list defined regardless of the option;
#                              consumers add them only when GDX_EXPANSION_KIT is ON)
#   GDX_GAME_DEFS          — region-independent compile definitions for game TUs
#   GDX_DEV_TOOLS_DEFS     — dev-gate master-switch generator expression
#   GDX_JP_INPUT_DEFS      — GDX_ALLOW_JP_INPUTS definitions (possibly an empty list)
# plus the options GDX_EXPANSION_KIT / GDX_FORCE_DEV_TOOLS / GDX_ALLOW_JP_INPUTS.
#
# CMAKE_CURRENT_LIST_DIR is this file's directory (port/cmake), so the paths below
# hold for any includer regardless of its own source directory.

set(GDX_PORT_DIR "${CMAKE_CURRENT_LIST_DIR}/..")
get_filename_component(GDX_PORT_DIR "${GDX_PORT_DIR}" ABSOLUTE)
set(DECOMP "${GDX_PORT_DIR}/../decomp")
get_filename_component(DECOMP "${DECOMP}" ABSOLUTE)

# Decomp game C compiled for the host. libultra/* is excluded (libultraship provides it) except
# gu/ (matrix math) and os/ — we run the decomp's own cooperative thread scheduler and supply only
# the low-level context switch in the port. rsp/* and leo/* are handled elsewhere.
file(GLOB_RECURSE GAME_SOURCES CONFIGURE_DEPENDS
    "${DECOMP}/src/game/*.c"
    "${DECOMP}/src/sys/*.c"
    "${DECOMP}/src/overlays/*.c"
    "${DECOMP}/src/audio/*.c"
    "${DECOMP}/src/framebuffers/*.c"
    "${DECOMP}/src/libultra/gu/*.c"
    "${DECOMP}/src/libultra/os/*.c"
)

# Time/timers come from libultraship (os_time.cpp) — exclude the decomp's to avoid duplicate
# symbols. Threading/messaging/events stay decomp.
list(FILTER GAME_SOURCES EXCLUDE REGEX "/libultra/os/gettime\\.c$")
list(FILTER GAME_SOURCES EXCLUDE REGEX "/libultra/os/settime\\.c$")
list(FILTER GAME_SOURCES EXCLUDE REGEX "/libultra/os/settimer\\.c$")
list(FILTER GAME_SOURCES EXCLUDE REGEX "/libultra/os/timerintr\\.c$")
# Our osCreateThread (port/n64_sched.c) captures entry+arg as full 64-bit pointers; the decomp's
# truncates them into u32 context fields.
list(FILTER GAME_SOURCES EXCLUDE REGEX "/libultra/os/createthread\\.c$")
# The decomp's osInitialize does real PIF/PI register I/O — replaced in port/n64_sched.c.
list(FILTER GAME_SOURCES EXCLUDE REGEX "/libultra/os/initialize\\.c$")
# The decomp's osGetMemSize dereferences N64 bus addresses — replaced in port/n64_sched.c.
list(FILTER GAME_SOURCES EXCLUDE REGEX "/libultra/os/getmemsize\\.c$")

# Expansion Kit: EXPANSION_KIT activates the decomp's EK hooks WITHOUT VERSION_JP — the
# VERSION_JP sites are JP-cart text/font differences deliberately kept on US behavior.
# EK modes are runtime-gated by disk-image presence (port/n64_leo.c).
option(GDX_EXPANSION_KIT "Compile Expansion Kit (64DD) support into the port" ON)

# GDX_DEV_TOOLS — master switch for the behaviour-altering (Bucket B) developer gates
# (port/gdx_dev_gates.h, docs/menu/DEVELOPER_TAB.md). When absent, every Bucket B gate is a
# hard-wired 0 (no getenv, no CVar read) and the Dev Tools page omits the section — safe because
# the gate table normalizes 0 == stock behaviour. ON everywhere except Release; the generator
# expression keeps one build tree correct under both single- and multi-config generators.
# GDX_FORCE_DEV_TOOLS keeps the gates in a Release soak/QA binary.
option(GDX_FORCE_DEV_TOOLS "Keep the behaviour-altering developer gates in Release builds too" OFF)
if(GDX_FORCE_DEV_TOOLS)
    set(GDX_DEV_TOOLS_DEFS "GDX_DEV_TOOLS=1")
else()
    set(GDX_DEV_TOOLS_DEFS "$<$<NOT:$<CONFIG:Release>>:GDX_DEV_TOOLS=1>")
endif()

# GDX_ALLOW_JP_INPUTS — whether the ORDINARY (US) binary may ingest Japanese-region game data.
# Not GDX_BUILD_JP (that builds a second VERSION_JP executable).
#
# Defaults OFF because the JP raw-ROM path plays through US-profile binding tables and produces
# loud static/blasting audio (observed, owner testing) — an ear-safety hazard, so release builds
# refuse JP inputs outright. Gated, not deleted; JP support is a later release.
# Covered sites: gdx_firstboot.cpp (refuses JP ROM by SHA-1 and header country code, and the
# retail JP EK disk), rom_buffer.cpp (runtime cartridge loader), disk_buffer.cpp (drops JP disk
# filenames, refuses the retail JP image by CRC-64). The 64DD IPL is deliberately NOT gated: the
# drive firmware is Japanese by existence and carries no game audio.
# The G-Diffuser-JP target always sets it — building that target is the opt-in.
option(GDX_ALLOW_JP_INPUTS
       "Accept Japanese-region game data (JP cartridge ROM, retail JP Expansion Kit disk). DEV ONLY — the JP raw-ROM path has broken audio." OFF)
if(GDX_ALLOW_JP_INPUTS)
    message(STATUS "G-Diffuser: GDX_ALLOW_JP_INPUTS=ON — Japanese ROM/disk inputs are ACCEPTED (experimental; JP audio is known-bad).")
    set(GDX_JP_INPUT_DEFS "GDX_ALLOW_JP_INPUTS=1")
else()
    # Empty LIST, not an empty string element, so the unquoted expansion contributes no argument.
    set(GDX_JP_INPUT_DEFS)
endif()

if(NOT GDX_EXPANSION_KIT)
    # Mirrors the decomp Makefile's EXPANSION_KIT=0 EXCLUSION_FILES list.
    list(FILTER GAME_SOURCES EXCLUDE REGEX "/sys/disk/")
    list(FILTER GAME_SOURCES EXCLUDE REGEX "/overlays/ovl_i2/dd_save\\.c$")
    list(FILTER GAME_SOURCES EXCLUDE REGEX "/overlays/ovl_i2/ovl_i2_data2\\.c$")
    list(FILTER GAME_SOURCES EXCLUDE REGEX "/overlays/ovl_i10/.*187510\\.c$")
    list(FILTER GAME_SOURCES EXCLUDE REGEX "/overlays/expansion_kit/")
    list(FILTER GAME_SOURCES EXCLUDE REGEX "/overlays/course_edit/")
    list(FILTER GAME_SOURCES EXCLUDE REGEX "/overlays/machine_create/")
    list(FILTER GAME_SOURCES EXCLUDE REGEX "/overlays/ead_demo/")
    list(FILTER GAME_SOURCES EXCLUDE REGEX "/audio/disk/")
else()
    # Mirrors the decomp Makefile's EXPANSION_KIT=1 exclusions: the disk-side subsystems REPLACE
    # their rom-side twins (audio/disk vs audio/rom, sys/disk vs sys/rom, dd_save.c vs
    # save_buffer.c) — alternatives, never compiled together.
    list(FILTER GAME_SOURCES EXCLUDE REGEX "/sys/rom/")
    list(FILTER GAME_SOURCES EXCLUDE REGEX "/audio/rom/")
    list(FILTER GAME_SOURCES EXCLUDE REGEX "/overlays/ovl_i2/save_buffer\\.c$")
    list(FILTER GAME_SOURCES EXCLUDE REGEX "/overlays/ovl_i11/")
    # ead_demo (64DD attract demo) has broken WIP C bodies upstream — exclude until the decomp
    # matures; cosmetic-only.
    list(FILTER GAME_SOURCES EXCLUDE REGEX "/overlays/ead_demo/")
    # libultraship provides the osAi* surface (same reason as aisetnextbuf.c below).
    list(FILTER GAME_SOURCES EXCLUDE REGEX "/audio/disk/lib/os\\.c$")
    # Only the pure-math leo files (LBA<->byte zone tables); the hardware command/interrupt
    # layer is replaced wholesale by port/n64_leo.c.
    file(GLOB EK_LEO_SOURCES
        "${DECOMP}/src/leo/lib/bytetolba.c"
        "${DECOMP}/src/leo/lib/lbatobyte.c"
        "${DECOMP}/src/leo/lib/leo_tbl.c"
        "${DECOMP}/src/leo/lib/leotranslat.c"
        "${DECOMP}/src/leo/lib/leoutil.c"
        "${DECOMP}/src/leo/mfs/*.c"
    )
    list(APPEND GAME_SOURCES ${EK_LEO_SOURCES})
endif()

# libultraship already provides these libultra functions — exclude to avoid duplicate symbols.
list(FILTER GAME_SOURCES EXCLUDE REGEX "/audio/rom/lib/aisetnextbuf\\.c$")  # osAiSetNextBuffer

# segment.c / cartridge_offsets.c: N64 ROM-segment system — linker segment symbols don't exist
# on host; replaced by libultraship resource loading.
list(FILTER GAME_SOURCES EXCLUDE REGEX "/sys/segment\\.c$")
list(FILTER GAME_SOURCES EXCLUDE REGEX "/libultra/os/virtualtophysical\\.c$")
list(FILTER GAME_SOURCES EXCLUDE REGEX "/libultra/os/physicaltovirtual\\.c$")
list(FILTER GAME_SOURCES EXCLUDE REGEX "/sys/cartridge_offsets\\.c$")

# ovl_i2/save.c is deliberately NOT excluded: Sram_Init/Sram_ReadWrite are PORT-ified in place to
# hit the host-backed SRAM image (port/sram_buffer.cpp). Save_LoadStaffGhostRecord stays stubbed,
# pending the EK ROM segment table.

# Port-side TUs that always compile with the decomp sources (the fiber backend is chosen by the
# consumer: Win32 / ucontext on desktop, gdx_fiber_3ds.c via gdx3ds_os on 3DS).
set(GDX_GAME_CORE_SOURCES
    "${GDX_PORT_DIR}/decomp_port.c"
    "${GDX_PORT_DIR}/n64_sched.c"
    "${GDX_PORT_DIR}/n64_vi.c"
    "${DECOMP}/src/libultra/io/devmgr.c"                  # outside the glob: PORT PI manager, host-backed DMA completion
    "${GDX_PORT_DIR}/gen/AssetBindings.c"
    "${GDX_PORT_DIR}/gen/LinkStubs.c")                     # placeholder defs so bring-up links

set(GDX_GAME_EK_PORT_SOURCES
    "${GDX_PORT_DIR}/n64_leo.c"
    "${GDX_PORT_DIR}/gen/EkAssetBindings.c"
    "${GDX_PORT_DIR}/gen/EkLinkStubs.c"
    "${GDX_PORT_DIR}/gdx_ek_disk_overrides.c")

# Region-independent compile definitions for the game object library (US targets add
# VERSION_US=1 / ASSET_VERSION=us, the JP target its own triplet).
set(GDX_GAME_DEFS
    PORT=1
    GDIFFUSER_PORT=1
    NON_MATCHING=1
    NON_EQUIVALENT=1
    AVOID_UB=1
    F3DEX_GBI_2=1
    _LANGUAGE_C=1
)
