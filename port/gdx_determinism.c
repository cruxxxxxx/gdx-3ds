// G-Diffuser — determinism pins. Contract in gdx_determinism.h.
#define _CRT_SECURE_NO_WARNINGS 1

#include "gdx_determinism.h"

#include <stdlib.h>

static int sInited = 0;
static int sSeedHave[3] = { 0, 0, 0 };
static unsigned int sSeed[3] = { 0, 0, 0 };
static int sBootHoldTicks = 0;
static int sAudioPin = 0;

// Desktop fallback: the pre-existing GDX_RAND_SEED1/2 env pin, read once.
static void LazyEnvInit(void) {
    int which;
    if (sInited) {
        return;
    }
    sInited = 1;
    for (which = 1; which <= 2; which++) {
        const char* env = getenv(which == 1 ? "GDX_RAND_SEED1" : "GDX_RAND_SEED2");
        if (env != NULL && env[0] != '\0') {
            sSeed[which] = (unsigned int)strtoul(env, NULL, 0);
            sSeedHave[which] = 1;
        }
    }
}

void gdx_det_init(int seed1Have, unsigned int seed1, int seed2Have, unsigned int seed2, int bootHoldTicks,
                  int audioPin) {
    sInited = 1;
    sSeedHave[1] = seed1Have ? 1 : 0;
    sSeed[1] = seed1;
    sSeedHave[2] = seed2Have ? 1 : 0;
    sSeed[2] = seed2;
    sBootHoldTicks = bootHoldTicks > 0 ? bootHoldTicks : 0;
    sAudioPin = audioPin ? 1 : 0;
}

int gdx_det_rand_seed(int which, unsigned int* out) {
    LazyEnvInit();
    if (which < 1 || which > 2 || out == NULL || !sSeedHave[which]) {
        return 0;
    }
    *out = sSeed[which];
    return 1;
}

int gdx_det_boot_hold_ticks(void) {
    LazyEnvInit();
    return sBootHoldTicks;
}

int gdx_det_audio_pin(void) {
    LazyEnvInit();
    return sAudioPin;
}

int gdx_det_active(void) {
    LazyEnvInit();
    return sSeedHave[1] || sSeedHave[2] || sBootHoldTicks > 0 || sAudioPin;
}

/* [debug] det_title_demo / det_title_demo_len (niview): under the audio pin the attract demo is
 * never armed from the synthesis position; with det_title_demo = N > 0 it is armed by TICK COUNT
 * instead -- after N consecutive Audio_Update calls on the title with the demo inactive -- and
 * left (START_EXIT) after det_title_demo_len updates of an active demo outside the title. 0 (the
 * default) = off: every existing replay records the title without demos. Game thread only. */
static int sTitleDemoArm = 0;
static int sTitleDemoLen = 1500;
static int sTitleDemoTicks = 0;

void gdx_det_set_title_demo(int armTicks, int demoTicks) {
    sTitleDemoArm = armTicks > 0 ? armTicks : 0;
    sTitleDemoLen = demoTicks > 0 ? demoTicks : 1500;
    sTitleDemoTicks = 0;
}

/* bottom-ui (port/3ds/gdx3ds_bui.c): a bottom-screen menu flow is open on the title. The audio
 * thread reads it (decomp-3ds-bui-title-demo-hold.patch): while it is set the attract demo is
 * never armed, from audio state or by tick count. Written by the main thread only. */
static volatile int sTitleDemoHeld = 0;

void gdx_title_demo_set_held(int held) {
    sTitleDemoHeld = held ? 1 : 0;
}

int gdx_title_demo_held(void) {
    return sTitleDemoHeld;
}

int gdx_det_title_demo_step(int gameModeIsTitle, int state) {
    if (sTitleDemoArm <= 0) {
        return state;
    }
    if (sTitleDemoHeld && gameModeIsTitle && state == 0) {
        sTitleDemoTicks = 0;
        return state;
    }
    if (gameModeIsTitle && state == 0) {
        if (++sTitleDemoTicks >= sTitleDemoArm) {
            sTitleDemoTicks = 0;
            return 1; /* TITLE_DEMO_ACTIVE */
        }
        return state;
    }
    if (!gameModeIsTitle && state == 1) {
        if (++sTitleDemoTicks >= sTitleDemoLen) {
            sTitleDemoTicks = 0;
            return 3; /* TITLE_DEMO_START_EXIT */
        }
        return state;
    }
    sTitleDemoTicks = 0;
    return state;
}
