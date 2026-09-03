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
# Prereq: port/3ds/patches/lus-newlib-portability.patch must be applied to the
# libultraship/ submodule working tree (git apply) before building.

if(NOT NINTENDO_3DS)
    # Desktop configure of the 3DS tree: keep the Phase 0 placeholder so
    # port/3ds/CMakeLists.txt still resolves the target.
    add_library(gdx3ds_gfx INTERFACE)
    target_include_directories(gdx3ds_gfx INTERFACE ../include)
    return()
endif()

set(GDX3DS_LUS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../libultraship")

if(NOT EXISTS "${GDX3DS_LUS_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR "libultraship submodule not initialized (run: git submodule update --init --recursive)")
endif()

# CVar name macros (CVAR_VSYNC_ENABLED etc.) — replicate the desktop build by
# including LUS's own cvars.cmake; it calls add_compile_definitions() which is
# directory-scoped, so it applies to every target defined in this file.
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
# BEFORE libultraship/include and libultraship/src.
set(GDX3DS_LUS_INCLUDES
    "${CMAKE_CURRENT_SOURCE_DIR}/../lus_stubs"
    "${GDX3DS_GFX_THIRD_PARTY}"
    "${GDX3DS_LUS_ROOT}/include"
    "${GDX3DS_LUS_ROOT}/src"
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
    gdx3ds_gpu_prof.c # stream gpuprof: per-frame GPU timing + fill-rate telemetry
    gdx3ds_stereo.cpp # stream S: stereo foundation (config-gated, default off)
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
# citro3d ships with libctru under ${CTR_ROOT}; propagate to whoever links us.
target_link_libraries(gdx3ds_gfx PUBLIC
    "${CTR_ROOT}/lib/libcitro3d.a"
    "${CTR_ROOT}/lib/libctru.a"
    m
)

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
