# port/3ds/gfx/ — stream A: citro3d renderer backend.
# Register new sources HERE, never in port/CMakeLists.txt.
#
# Two libraries:
#   gdx3ds_lus_carve — the 36 LUS sources proven by the Phase 0 carve spike
#                      (docs/research/spike-lus-carve-report.md), compiled with the
#                      spike's include order (lus_stubs BEFORE libultraship) and the
#                      cvars.cmake compile definitions.
#   gdx3ds_gfx       — the citro3d Fast::GfxRenderingAPI backend (gfx_citro3d.cpp)
#                      exposed via Gdx3ds_GetCitro3dRenderer() (../include/gdx3ds_gfx.h).
#
# Plus gdx3ds_gfx_linktest (EXCLUDE_FROM_ALL): whole-archive link probe used for the
# unresolved-symbol audit — expected to FAIL to link until stubs/carve additions are
# decided at integration; see STATUS.md.
#
# The carve sources live in port/3ds/vendor/lus/ with every LUS patch folded in —
# no submodule, no patch step. See port/3ds/vendor/lus/PROVENANCE.md.

if(NOT NINTENDO_3DS)
    # Desktop configure of the 3DS tree: keep the Phase 0 placeholder so
    # port/3ds/CMakeLists.txt still resolves the target.
    add_library(gdx3ds_gfx INTERFACE)
    target_include_directories(gdx3ds_gfx INTERFACE ../include)
    return()
endif()

# The vendored LUS carve — 36 sources + the 83-header closure, all 38 lus-*.patch
# already folded in (port/3ds/vendor/lus/PROVENANCE.md). The libultraship/ submodule
# is NOT needed to configure or build the 3DS target; it stays for the desktop build.
set(GDX3DS_LUS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../vendor/lus")

# CVar name macros (CVAR_VSYNC_ENABLED etc.) — replicate the desktop build by
# including LUS's own cvars.cmake (vendored verbatim); it calls
# add_compile_definitions() which is directory-scoped, so it applies to every
# target defined in this file.
include("${GDX3DS_LUS_ROOT}/cmake/cvars.cmake")

# ---------------------------------------------------------------------------------
# Third-party headers (vendored, see spike report: real headers required for
# nlohmann/json.hpp 3.11.3, BS_thread_pool.hpp 4.1.0, tinyxml2 10.0.0).
# tinyxml2.cpp is vendored too so XML resource factories resolve at link — noted in
# STATUS.md as the only compiled source beyond the 36-file spike carve (third-party,
# not a carve widening).
# ---------------------------------------------------------------------------------
set(GDX3DS_GFX_THIRD_PARTY "${CMAKE_CURRENT_SOURCE_DIR}/third_party")

# Include order matters (spike-proven): stubs shadow desktop header leaks
# (imconfig.h, Gui.h, spdlog, SDL2, zip.h, StormLib.h, monocypher.h) and must come
# BEFORE the vendored LUS headers.
# No "${GDX3DS_LUS_ROOT}/src" entry: the baseline build's depfiles show zero headers
# resolving out of libultraship/src, so that include dir was vestigial and is gone.
set(GDX3DS_LUS_INCLUDES
    "${CMAKE_CURRENT_SOURCE_DIR}/../lus_stubs"
    "${GDX3DS_GFX_THIRD_PARTY}"
    "${GDX3DS_LUS_ROOT}/include"
)

# ---------------------------------------------------------------------------------
# gdx3ds_lus_carve — explicit 36-source list from the spike (NO globbing).
# ---------------------------------------------------------------------------------
set(GDX3DS_LUS_CARVE_SOURCES
    # Fast3D interpreter + debug (2)
    "${GDX3DS_LUS_ROOT}/src/fast/interpreter.cpp"
    "${GDX3DS_LUS_ROOT}/src/fast/debug/GfxDebugger.cpp"
    # Fast3D resource factories (5)
    "${GDX3DS_LUS_ROOT}/src/fast/resource/factory/DisplayListFactory.cpp"
    "${GDX3DS_LUS_ROOT}/src/fast/resource/factory/LightFactory.cpp"
    "${GDX3DS_LUS_ROOT}/src/fast/resource/factory/MatrixFactory.cpp"
    "${GDX3DS_LUS_ROOT}/src/fast/resource/factory/TextureFactory.cpp"
    "${GDX3DS_LUS_ROOT}/src/fast/resource/factory/VertexFactory.cpp"
    # Fast3D resource types (5)
    "${GDX3DS_LUS_ROOT}/src/fast/resource/type/DisplayList.cpp"
    "${GDX3DS_LUS_ROOT}/src/fast/resource/type/Light.cpp"
    "${GDX3DS_LUS_ROOT}/src/fast/resource/type/Matrix.cpp"
    "${GDX3DS_LUS_ROOT}/src/fast/resource/type/Texture.cpp"
    "${GDX3DS_LUS_ROOT}/src/fast/resource/type/Vertex.cpp"
    # ship resource manager/loader/factories (5)
    "${GDX3DS_LUS_ROOT}/src/ship/resource/Resource.cpp"
    "${GDX3DS_LUS_ROOT}/src/ship/resource/ResourceFactoryBinary.cpp"
    "${GDX3DS_LUS_ROOT}/src/ship/resource/ResourceFactoryXML.cpp"
    "${GDX3DS_LUS_ROOT}/src/ship/resource/ResourceLoader.cpp"
    "${GDX3DS_LUS_ROOT}/src/ship/resource/ResourceManager.cpp"
    # ship resource factories (3)
    "${GDX3DS_LUS_ROOT}/src/ship/resource/factory/BlobFactory.cpp"
    "${GDX3DS_LUS_ROOT}/src/ship/resource/factory/JsonFactory.cpp"
    "${GDX3DS_LUS_ROOT}/src/ship/resource/factory/ShaderFactory.cpp"
    # ship resource types (3)
    "${GDX3DS_LUS_ROOT}/src/ship/resource/type/Blob.cpp"
    "${GDX3DS_LUS_ROOT}/src/ship/resource/type/Json.cpp"
    "${GDX3DS_LUS_ROOT}/src/ship/resource/type/Shader.cpp"
    # archives — OtrArchive/StormLib deliberately excluded (spike report) (4)
    "${GDX3DS_LUS_ROOT}/src/ship/resource/archive/Archive.cpp"
    "${GDX3DS_LUS_ROOT}/src/ship/resource/archive/ArchiveManager.cpp"
    "${GDX3DS_LUS_ROOT}/src/ship/resource/archive/FolderArchive.cpp"
    "${GDX3DS_LUS_ROOT}/src/ship/resource/archive/O2rArchive.cpp"
    # binarytools (4)
    "${GDX3DS_LUS_ROOT}/src/ship/utils/binarytools/BinaryReader.cpp"
    "${GDX3DS_LUS_ROOT}/src/ship/utils/binarytools/BinaryWriter.cpp"
    "${GDX3DS_LUS_ROOT}/src/ship/utils/binarytools/MemoryStream.cpp"
    "${GDX3DS_LUS_ROOT}/src/ship/utils/binarytools/Stream.cpp"
    # utils (5)
    "${GDX3DS_LUS_ROOT}/src/ship/utils/glob.c"
    "${GDX3DS_LUS_ROOT}/src/ship/utils/stox.cpp"
    "${GDX3DS_LUS_ROOT}/src/ship/utils/StrHash64.cpp"
    "${GDX3DS_LUS_ROOT}/src/ship/utils/StringHelper.cpp"
    "${GDX3DS_LUS_ROOT}/src/ship/utils/Utils.cpp"
)

add_library(gdx3ds_lus_carve STATIC
    ${GDX3DS_LUS_CARVE_SOURCES}
    "${GDX3DS_GFX_THIRD_PARTY}/tinyxml2.cpp"
)
# PUBLIC: integration targets (port/3ds/game/, lus_glue) compile against the same
# stub-shadowed include order the carve was proven with.
target_include_directories(gdx3ds_lus_carve PUBLIC ${GDX3DS_LUS_INCLUDES})
# [triloop] interpreter.cpp's __3DS__-gated packed-emission hunk includes
# gdx3ds_vbopack.h (this directory); desktop LUS builds never see the include.
target_include_directories(gdx3ds_lus_carve PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(gdx3ds_lus_carve PRIVATE F3DEX_GBI_2)  # never ENABLE_OPENGL
# Spike flags: exceptions/RTTI ON (Archive.cpp try/catch, ArchiveManager
# dynamic_pointer_cast), gnu++20.
target_compile_options(gdx3ds_lus_carve PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fexceptions -frtti>
)
set_target_properties(gdx3ds_lus_carve PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS ON
)

# ---------------------------------------------------------------------------------
# PICA vertex shader — picasso-assembled, embedded via bin2s.
# Symbols: gdx3ds_gfx_shader_shbin / gdx3ds_gfx_shader_shbin_size
# (header .dkp-generated/.../gdx3ds_gfx_shader_shbin.h).
# ---------------------------------------------------------------------------------
# NOTE: sources must be bare trailing arguments — the helper's SOURCES keyword
# branch is a FATAL_ERROR in the shipped devkitPro cmake.
ctr_add_shader_library(gdx3ds_gfx_shader
    OUTPUT gdx3ds_gfx_shader.shbin
    "${CMAKE_CURRENT_SOURCE_DIR}/shader.v.pica"
)
dkp_add_embedded_binary_library(gdx3ds_gfx_shbin gdx3ds_gfx_shader)

# ---------------------------------------------------------------------------------
# gdx3ds_gfx — the citro3d backend.
# ---------------------------------------------------------------------------------
add_library(gdx3ds_gfx STATIC
    gfx_citro3d.cpp
    gdx3ds_track_sink.c      # [native] track: game-side op recorder (gdx3ds_track_shim.h)
    gdx3ds_track_templates.c # [native] track: index-template decoder (lists from the o2r)
    gdx3ds_track_replay.cpp  # [native] track: Interpreter::GdxTrackMarker (marker replay)
    gdx3ds_native_tri.cpp    # [native] shared exact/fast triangle replay (track + fx)
    gdx3ds_track_retained.cpp # [native] track_retained: course runs as eye-rebased gpuxf draws (C5 S3)
    gdx3ds_track_resident.cpp # [native] track_resident: load-time course bake, resident rings (C5 S3c)
    gdx3ds_native_cfg.c      # [native] ini section reader (all master, boot receipt)
    gdx3ds_cov.c             # COVERAGE (B15/B43): [cov] per-frame feature census ([debug] cov).
                             # Here, not in the exe, because gdx3ds-dl-tests links this library
                             # too and every hook in it resolves gdx3ds_cov_mark.
    gdx3ds_fx_sink.c         # [native] fx: game-side effects recorder (gdx3ds_fx_shim.h)
    gdx3ds_fx_replay.cpp     # [native] fx: Interpreter::GdxFxMarker (marker replay)
    gdx3ds_hud_capture.c     # [native] hud_batch: game-side HUD display-list compaction (0x5E runs);
                             # also [native] ni_menu (gdxmenu_capture, folded in W5 -- was its own TU)
    gdx3ds_dress_sink.c      # [native] gadget_batch/dress_retained/fxshd_direct: levers + gadget recorder (C5 S3b)
    gdx3ds_gad_replay.cpp    # [native] gadget_batch: the gadget runs' marker replay (C5 S3b)
    gdx3ds_sky_capture.c     # [native] sky_retain / skyclock: Background_Draw's 0x65 markers + capture
    gdx3ds_seam_lus.cpp      # [native] GdxInterpSeam over the vendored LUS interpreter (marker + DL hook entries)
    gdx3ds_gpu_prof.c # stream gpuprof: per-frame GPU timing + fill-rate telemetry
    gdx3ds_pace.c # n60gp: swap-anchored vblank gate ([debug] pace_swapgate) + [pace] receipt
    gdx3ds_c3d_diet.c # c5submit (r60 P3a): [native] c3d_diet levers over the vendored citro3d
    gdx3ds_overlap.c  # C5 Stage 4 [overlap]: mid-frame GX queue kicks ([debug] overlap_kick)
    gdx3ds_gputl.c    # n60gpu: [debug] gputl -- per-eye GPU timeline from GSP completion stamps (hardware receipt)
    gdx3ds_prewrite.c # o3ds3d: [debug] prewrite / gpumem -- first-write window + GPU memory headroom receipts
    gdx3ds_gpucensus.c # n60gpu: [debug] gpucensus -- GPU work census per eye / category, fill counts
    nrr/gdx3ds_nrr_crumb.c # n60hwdiag: [debug] nrr_crumbs + nrr_bisect_cycle + gxq_watch_ms (in gfx: the harness links it too)
    gdx3ds_replay.cpp # C6 [replay]: pose-replay, "60 Hz motion" ([native] replay, default 0)
    gdx3ds_replay_tags.c # C6 [replay]: the interpreter-facing hook table + vertex tags (host-linkable)
    gdx3ds_hitch_ev.c # HITCH: per-frame event counters (here for gdx3ds-dl-tests, like gdx3ds_cov.c)
    gdx3ds_cpucut.c   # o3ds-cpucuts: the CPU cuts' shared A/B block clock + [cpucut] receipt
    gdx3ds_stereo.cpp # stream S: stereo foundation (config-gated, default off)
    gdx3ds_stereo_frame.cpp # n60stereo: the stereo frame replay ([native] stereo_replay)
    gdx3ds_skylate.cpp # n60tail: [native] sky_late -- the sky's lists after the opaque track (stencil)
    gdx3ds_mesh.cpp   # [native] machines: retained machine meshes (07-machines-plan)
    gdx3ds_mesh_walk.c
    gdx3ds_xf_census.c # [debug] xf_census: float24 shader-precision census (o3dsgpuxf)
)
target_include_directories(gdx3ds_gfx PUBLIC ../include)
target_include_directories(gdx3ds_gfx PRIVATE ${GDX3DS_LUS_INCLUDES})
target_compile_definitions(gdx3ds_gfx PRIVATE F3DEX_GBI_2)
target_compile_options(gdx3ds_gfx PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fexceptions -frtti>
)
set_target_properties(gdx3ds_gfx PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS ON
)
target_link_libraries(gdx3ds_gfx PRIVATE gdx3ds_gfx_shbin)
# citro3d: the vendored build (port/3ds/CMakeLists.txt GDX_3DS_VENDOR_CITRO3D) or the prebuilt
# one under ${CTR_ROOT}; propagate to whoever links us.
target_link_libraries(gdx3ds_gfx PUBLIC
    ${GDX3DS_CITRO3D_LIB}
    "${CTR_ROOT}/lib/libctru.a"
    m
)

# ---------------------------------------------------------------------------------
# gdx3ds_n64 -- [native] interp: the owned F3DEX2-to-citro3d interpreter
# (docs/research/native/06-interp-plan.md). Everything except gdx_rt_glue.cpp is
# host-compilable (no LUS headers); the glue adapts gdxn::Backend to the citro3d
# renderer and hooks gdx_gfx_job_run. Killswitch [native] interp (default 0).
# ---------------------------------------------------------------------------------
# OBJECT library: the bridge reaches gdx_ninterp_job_run through a WEAK reference, which
# never extracts a static-archive member, so the objects are added to the executable directly.
add_library(gdx3ds_n64 OBJECT
    n64/gdx_dlsrc.cpp
    n64/gdx_dl.cpp
    n64/gdx_interp.cpp
    n64/gdx_cc.cpp
    n64/gdx_tmem.cpp
    n64/gdx_texcache.cpp
    n64/gdx_rsp.cpp
    n64/gdx_tri.cpp
    n64/gdx_rect.cpp
    n64/gdx_texskip.cpp      # [native] machines_texskip (C5 Stage 1)
    n64/gdx_machsplit.cpp    # [debug] machsplit: the [c5cat] mach split (c5mach2)
    n64/gdx_nirace.cpp       # nirace: the race walk between the NRR markers (trace, compiled spans)
    n64/gdx_nicensus.cpp     # [debug] nicensus: per-mode census of what is still interpreted (nicensus)
    n64/gdx_nicourse.cpp     # [native] ni_course: the course preview's model runs from resident meshes (nicourse)
    n64/gdx_setload.cpp      # [native] machines_loadmemo: set-list loads memoised (c5mach2)
    n64/gdx_hud_replay.cpp   # [native] hud_batch: the HUD capture's 0x5E marker replay + source/hash memos
    n64/gdx_hud_direct.cpp   # [native] hud_direct: TMEM debt, per-source hash memo, direct HUD rects (C5-S2b)
    n64/gdx_hud_retain.cpp   # [native] hudelem census + hud_retain run-level retention (c5hud3)
    n64/gdx_nrr_hud.cpp      # [native] nrr_hud: the NRR HUD decoder + inventory census (n60hud)
    n64/gdx_sky_retain.cpp   # [native] sky_retain / skyclock: the sky's 0x65 census + retained replay (c5sky)
    n64/gdx_nrr_sky.cpp      # [native] nrr_sky: the NRR sky -- learned state blocks + packed draws (n60sky)
    n64/gdx_nrr_fx.cpp       # [native] nrr_shd / nrr_fx: shadows, boost flames, effects -- learned sprite blocks (n60fx)
    n64/gdx_dlmemo.cpp       # [native] dlmemo / deadsetup: cross-task G_DL resolve memo, dead setup lists (c5dlmemo)
    n64/gdx_seam_owned.cpp   # [native] GdxInterpSeam over the owned interpreter (track/fx/machines hooks)
    n64/gdx_rt_glue.cpp
    # [native] nrr (n60, docs/research/native/n60-design.md): the Compact Native Race Renderer.
    # Core + categories are OBJECTS of the executable: the core reaches each category's init
    # through a weak reference (nrr/gdx3ds_nrr.h "how to add a category"). Add categories here.
    nrr/gdx3ds_nrr_game.c    # core, game half: levers, snapshot frames, 0x63 marker, consume plumbing
    nrr/gdx3ds_nrr.cpp       # core, render half: hand-off, state blocks, ring, draws, [nrr] receipts
    nrr/gdx3ds_nrr_direct.cpp # [native] nrr_direct=1: precomputed GPU command words, no citro3d context update
    nrr/gdx3ds_nrr_dxverify.cpp # [native] nrr_direct=2: per-draw register-file compare of the direct words (debug)
    nrr/gdx3ds_nrr_selftest.cpp # [native] nrr_selftest: debug-only end-to-end hand-off quad
    nrr/gdx3ds_nrr_mach.cpp  # [native] nrr_mach (n60mach): BODY consumer snapshot, cold recorder, the hot draw
    nrr/gdx3ds_nrr_trk_draw.cpp # [native] nrr_trk (n60trk): resident-ring draws of the lean walk's runs
)
# ekeditmesh [native] ek_editmesh (o3dsek plan S5c): the course editor's preview memo. EK builds
# only, so the shipped (non-EK) gdx3ds_n64 object list is unchanged.
if(GDX_3DS_EXPANSION_KIT)
    target_sources(gdx3ds_n64 PRIVATE n64/gdx_ekpreview.cpp)
endif()
target_include_directories(gdx3ds_n64 PUBLIC n64)
target_include_directories(gdx3ds_n64 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}      # gdx3ds_vbopack.h, gfx_citro3d.h, gdx3ds_gpu_prof.h
    ../include
    ../../                           # port/: gdx_dlsrc_bridge.h
    ${GDX3DS_LUS_INCLUDES}
)
# B11o (docs/research/native/ekwidget-progress.md): gdx_rect.cpp's scissor pillar-safe
# widening and gdx_rt_glue.cpp's [debug] ek_widget_scissor lever are both inside
# `#ifdef EXPANSION_KIT`, so with the option OFF this expands to nothing and both TUs
# preprocess exactly as they did before -- which is what keeps the shipped .3dsx
# byte-identical. gdx3ds_glue already takes the same generator expression.
target_compile_definitions(gdx3ds_n64 PRIVATE F3DEX_GBI_2
    $<$<BOOL:${GDX_3DS_EXPANSION_KIT}>:EXPANSION_KIT=1>
)
target_compile_options(gdx3ds_n64 PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fexceptions -frtti>
)
set_target_properties(gdx3ds_n64 PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS ON
)
target_link_libraries(gdx3ds_n64 PRIVATE gdx3ds_gfx) # include dirs (citro3d) only

# ---------------------------------------------------------------------------------
# gdx3ds_gfx_linktest — unresolved-symbol audit target (deliberately not in ALL).
# Build with: cmake --build build-3ds --target gdx3ds_gfx_linktest
# --whole-archive forces every carve object in so ALL unresolved symbol families
# surface at once; the failure log is the audit input for STATUS.md.
# ---------------------------------------------------------------------------------
# The Generic-dkP platform module doesn't declare the WHOLE_ARCHIVE link feature;
# declare it for GNU ld ourselves (documented CMake extension point).
set(CMAKE_CXX_LINK_LIBRARY_USING_WHOLE_ARCHIVE
    "LINKER:--whole-archive" "<LINK_ITEM>" "LINKER:--no-whole-archive")
set(CMAKE_CXX_LINK_LIBRARY_USING_WHOLE_ARCHIVE_SUPPORTED TRUE)

add_executable(gdx3ds_gfx_linktest EXCLUDE_FROM_ALL linktest_main.cpp)
target_include_directories(gdx3ds_gfx_linktest PRIVATE ../include ${GDX3DS_LUS_INCLUDES})
set_target_properties(gdx3ds_gfx_linktest PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS ON
)
target_link_libraries(gdx3ds_gfx_linktest PRIVATE
    "$<LINK_LIBRARY:WHOLE_ARCHIVE,gdx3ds_lus_carve,gdx3ds_gfx>"
)
# 3dsx.specs enables --gc-sections, which silently discards the unreferenced carve
# sections (and their unresolved externals) — exactly what this audit target exists
# to surface. Disable GC so every undefined symbol must resolve.
target_link_options(gdx3ds_gfx_linktest PRIVATE "LINKER:--no-gc-sections")
