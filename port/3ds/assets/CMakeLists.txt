# port/3ds/assets/ — stream D: SD-backed O2R reading + save persistence.
# Register new sources HERE, never in port/CMakeLists.txt.
# PC-side pre-bake tooling lives in tools/prebake/ (also stream D).

# Vendored miniz 3.0.2 (single-file, MIT — third_party/miniz/). Chosen over
# libzip/zlib because /opt/devkitpro/portlibs/3ds ships empty and installing
# portlibs needs root; miniz needs neither. Definitions are PUBLIC so every TU
# that includes miniz.h agrees with the miniz.c build:
#   MINIZ_NO_TIME         — no zip timestamps (drops utime.h paths)
#   MINIZ_NO_DEFLATE_APIS — inflate only; miniz.h derives
#                           MINIZ_NO_ARCHIVE_WRITING_APIS from it, so the
#                           read-only archive build comes along for free
add_library(gdx3ds_miniz STATIC third_party/miniz/miniz.c)
target_include_directories(gdx3ds_miniz PUBLIC third_party/miniz)
target_compile_definitions(gdx3ds_miniz PUBLIC
    MINIZ_NO_TIME
    MINIZ_NO_DEFLATE_APIS
)

# Contract library. Device builds get the real SD implementation; host builds
# keep the Phase 0 stub (host targets have their own asset path) but still
# compile-check the SD code below so both build gates cover it.
if(NINTENDO_3DS)
    set(GDX3DS_FS_IMPL gdx3ds_fs_sd.c)
else()
    set(GDX3DS_FS_IMPL gdx3ds_fs_stub.c)
endif()
add_library(gdx3ds_assets STATIC ${GDX3DS_FS_IMPL})
target_include_directories(gdx3ds_assets PUBLIC ../include)
if(NINTENDO_3DS)
    target_link_libraries(gdx3ds_assets PRIVATE gdx3ds_miniz)
endif()

if(NOT NINTENDO_3DS)
    # Never linked into anything; exists so the host gate compiles the exact
    # sources the device build ships (gdx3ds_fs_sd.c is portable C by design).
    add_library(gdx3ds_assets_sd_checkbuild STATIC gdx3ds_fs_sd.c)
    target_include_directories(gdx3ds_assets_sd_checkbuild PRIVATE ../include)
    target_link_libraries(gdx3ds_assets_sd_checkbuild PRIVATE gdx3ds_miniz)

    # Functional smoke test for the SD implementation + zipshim (run manually
    # from a scratch CWD; "sdmc:" is an ordinary directory to the host). It
    # compiles its OWN full-API miniz (writer enabled, to fabricate fixture
    # archives), so it must not link gdx3ds_miniz or duplicate symbols.
    add_executable(gdx3ds_fs_host_smoke
        test/gdx3ds_fs_host_smoke.c
        gdx3ds_fs_sd.c
        zipshim/gdx3ds_zipshim.c
        third_party/miniz/miniz.c
    )
    target_include_directories(gdx3ds_fs_host_smoke PRIVATE
        ../include
        ../lus_stubs
        third_party/miniz
    )
    target_compile_definitions(gdx3ds_fs_host_smoke PRIVATE MINIZ_NO_TIME)
endif()

# libzip API surface over miniz, for stream A: lets libultraship's O2rArchive
# link on 3DS without libzip. Publishes ../lus_stubs so consumers compile
# against the same zip.h the shim implements (the shim itself never uses the
# other stub headers in that directory).
add_library(gdx3ds_zipshim STATIC zipshim/gdx3ds_zipshim.c)
target_include_directories(gdx3ds_zipshim PUBLIC ../lus_stubs)
target_link_libraries(gdx3ds_zipshim PRIVATE gdx3ds_miniz)
