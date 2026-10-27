# tools/hudpack -- gdx-hudpack, the PC half of the 3DS bottom-screen HUD bake.
#
# Runs port/3ds/gdx3ds_hudbake.c -- the very recipes the console runs at boot -- over the user's
# own fzerox.o2r (and gdiffuser.o2r) on the PC, and writes hudbake.cache: the console's own SD
# cache format, keyed on the same archive record CRCs and the same recipe id
# (port/3ds/hudbake_recipe_id.cmake). Copied to sdmc:/3ds/gdiffuser/, it makes the console's first
# boot a cache hit instead of a bake. tools/prebake/prebake.py runs it; nothing ROM-derived is
# ever built into the binary.
#
#   cmake -S tools/hudpack -B build-hudpack -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-hudpack --config Release
cmake_minimum_required(VERSION 3.16)
project(gdx_hudpack C)

set(GDX_3DS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../port/3ds)
include(${GDX_3DS_DIR}/hudbake_recipe_id.cmake)
gdx3ds_hudbake_recipe_id(GDX3DS_HUDBAKE_RECIPE_ID ${GDX_3DS_DIR})
message(STATUS "gdx-hudpack: HUD bake recipe id ${GDX3DS_HUDBAKE_RECIPE_ID}")

add_executable(gdx-hudpack
    gdx_hudpack.c
    ${GDX_3DS_DIR}/gdx3ds_hudbake.c
    ${GDX_3DS_DIR}/assets/third_party/miniz/miniz.c
)
target_include_directories(gdx-hudpack PRIVATE ${GDX_3DS_DIR} ${GDX_3DS_DIR}/assets/third_party/miniz)
target_compile_definitions(gdx-hudpack PRIVATE GDX3DS_HUDBAKE_RECIPE_ID=${GDX3DS_HUDBAKE_RECIPE_ID} MINIZ_NO_TIME
                           _CRT_SECURE_NO_WARNINGS)
if(MSVC)
    # /fp:precise (the default) never contracts a*b+c; say so explicitly. Static CRT: one file.
    target_compile_options(gdx-hudpack PRIVATE /fp:precise)
    set_property(TARGET gdx-hudpack PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
else()
    # The recipes' double arithmetic must round after every operation (gdx3ds_hudbake.c's
    # header): no fused multiply-add, exactly as the console build compiles it.
    set_source_files_properties(${GDX_3DS_DIR}/gdx3ds_hudbake.c PROPERTIES COMPILE_OPTIONS -ffp-contract=off)
    target_link_libraries(gdx-hudpack PRIVATE m)
endif()
install(TARGETS gdx-hudpack RUNTIME DESTINATION bin)
