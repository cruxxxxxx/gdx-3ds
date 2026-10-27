// G-Diffuser — determinism pins (ORACLE track, plan §3).
//
// The decomp reads these through `extern` declarations added by
// port/3ds/patches/decomp-port-determinism-pins.patch. Every accessor keeps the ORIGINAL
// behaviour when nothing is pinned:
//   * gdx_det_rand_seed(): the two Math_Rand*Init seeds (sys_gfx.c). Unpinned -> the caller
//     keeps its osGetTime() expression. Desktop fallback: GDX_RAND_SEED1/2 env (the audio
//     harness pin that predates this module; on 3DS getenv is dead, which is why the pin lives
//     behind an accessor now).
//   * gdx_det_boot_hold_ticks(): the boot-logo hold (sys_gfx.c). 0 -> the wall-clock 230-frame
//     wait (its tick count is wall-clock dependent: THE reason menus matched byte for byte while
//     races differed); N -> exactly N gdx_yield()s, i.e. N host ticks.
//   * gdx_det_audio_pin(): audioRandom's osGetCount() term (audio thread.c) becomes a
//     deterministic counter, as the PCM-capture harness already does under capture.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Called once at boot (3DS: from the [debug] keys rand_seed1, rand_seed2, boot_hold_ticks,
// det_audio, det). Not calling it leaves the desktop env fallback in place.
void gdx_det_init(int seed1Have, unsigned int seed1, int seed2Have, unsigned int seed2, int bootHoldTicks,
                  int audioPin);

// which = 1 | 2. Returns 1 and writes *out when pinned.
int gdx_det_rand_seed(int which, unsigned int* out);

int gdx_det_boot_hold_ticks(void);
int gdx_det_audio_pin(void);

// Any pin active (receipt / .gdxrec header helper).
int gdx_det_active(void);

// niview: tick-driven attract demo under the audio pin ([debug] det_title_demo, default 0 = off).
void gdx_det_set_title_demo(int armTicks, int demoTicks);
int gdx_det_title_demo_step(int gameModeIsTitle, int state);
/* bottom-ui: 1 = a bottom-screen menu flow is open on the title; the attract demo is not armed. */
void gdx_title_demo_set_held(int held);
int gdx_title_demo_held(void);

#ifdef __cplusplus
}
#endif
