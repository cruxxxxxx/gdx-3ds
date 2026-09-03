# port/3ds/os/ — stream B: libctru window, HID input, fiber backend, INI config.
# Register new sources HERE, never in port/CMakeLists.txt.
#
# Source split:
#   3DS cross-build (NINTENDO_3DS, devkitARM): real libctru backend + thread-based fiber
#     implementation of port/gdx_fiber.h. The Phase 0 stub must NOT be compiled here or
#     its definitions would collide with the real ones.
#   Host build (GDX_PLATFORM_3DS=ON without the 3DS toolchain): keeps the Phase 0 stub;
#     the fiber backend stays the desktop ucontext/Win32 one and is never wired here.
#   gdx3ds_config.c is portable C and builds on both, so config parsing is host-testable.
if(NINTENDO_3DS)
    add_library(gdx3ds_os STATIC
        gdx3ds_os_ctru.c
        gdx_fiber_3ds.c
        gdx3ds_config.c
    )
    # gdx_fiber_3ds.c implements the existing desktop contract port/gdx_fiber.h verbatim.
    target_include_directories(gdx3ds_os PRIVATE ../..)
else()
    add_library(gdx3ds_os STATIC
        gdx3ds_os_stub.c
        gdx3ds_config.c
    )
endif()

target_include_directories(gdx3ds_os PUBLIC ../include)
# Export gdx3ds_config.h (this directory) to consumers: main_3ds.cpp and sibling streams
# read config keys through the gdx3ds_os link.
target_include_directories(gdx3ds_os PUBLIC .)
