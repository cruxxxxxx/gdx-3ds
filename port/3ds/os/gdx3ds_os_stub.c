/* Phase 0 stub for gdx3ds_os.h. Stream B replaces this file with the real libctru
 * implementation (window/HID here, fibers in gdx_fiber_3ds.c). On 3DS the stub
 * boots to a cleared top screen and exits on START or APT close. */
#include "gdx3ds_os.h"

#ifdef __3DS__
#include <3ds.h>

int gdx3ds_os_window_init(int* outWidth, int* outHeight) {
    gfxInitDefault();
    if (outWidth != NULL) {
        *outWidth = 400;
    }
    if (outHeight != NULL) {
        *outHeight = 240;
    }
    return 0;
}

void gdx3ds_os_window_shutdown(void) {
    gfxExit();
}

int gdx3ds_os_window_swap(void) {
    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();
    if (!aptMainLoop()) {
        return 1;
    }
    hidScanInput();
    if (hidKeysDown() & KEY_START) {
        return 1;
    }
    return 0;
}

void gdx3ds_os_poll_input(Gdx3dsPadState* outPads, int maxPads) {
    if (outPads == NULL || maxPads < 1) {
        return;
    }
    /* Stub: connected flag only; the real mapping table is stream B's. */
    outPads[0].buttons = 0;
    outPads[0].stickX = 0;
    outPads[0].stickY = 0;
    outPads[0].connected = 1;
}

uint64_t gdx3ds_os_time_ns(void) {
    return svcGetSystemTick() * 1000000000ull / SYSCLOCK_ARM11;
}

#else /* host build: compiles everywhere, exits immediately */

int gdx3ds_os_window_init(int* outWidth, int* outHeight) {
    if (outWidth != NULL) {
        *outWidth = 400;
    }
    if (outHeight != NULL) {
        *outHeight = 240;
    }
    return 0;
}

void gdx3ds_os_window_shutdown(void) {
}

int gdx3ds_os_window_swap(void) {
    return 1; /* no display on host; unwind the main loop at once */
}

void gdx3ds_os_poll_input(Gdx3dsPadState* outPads, int maxPads) {
    if (outPads == NULL || maxPads < 1) {
        return;
    }
    outPads[0].buttons = 0;
    outPads[0].stickX = 0;
    outPads[0].stickY = 0;
    outPads[0].connected = 0;
}

uint64_t gdx3ds_os_time_ns(void) {
    return 0;
}

#endif
