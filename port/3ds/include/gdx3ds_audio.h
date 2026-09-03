/* port/3ds/include/gdx3ds_audio.h -- 3DS audio output contract (stream C implements).
 *
 * CONTRACT STATUS: FROZEN (Phase 0). Changes require orchestrator sign-off.
 *
 * Shape mirrors the osAiSetNextBuffer push model the audio thread already speaks
 * (port/gdx_audio_thread.cpp): the game pushes interleaved stereo s16 at 32000 Hz
 * (title-fixed, see port/gdx_audio_capture.h) and queries backlog to pace itself.
 * Backend is ndsp; the audio thread runs on a spare ARM11 core (New3DS core 2 --
 * requires Luma3DS >= 10.1.1; document for users).
 */
#ifndef GDX3DS_AUDIO_H
#define GDX3DS_AUDIO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GDX3DS_AUDIO_SAMPLE_RATE 32000u
#define GDX3DS_AUDIO_CHANNELS 2u

/* bufferFrames: ring capacity in frames; pass 0 for the default (4096, matching the
 * desktop CVar default in port/main.cpp). Returns 0 on success. */
int gdx3ds_audio_init(uint32_t bufferFrames);
void gdx3ds_audio_shutdown(void);

/* Interleaved L/R s16, `frames` frames. Never blocks; on overrun drops the oldest
 * data and returns the number of frames actually queued. */
size_t gdx3ds_audio_push(const int16_t* samples, size_t frames);

/* Frames queued but not yet played -- the osAiGetLength analogue the game's pacing
 * logic needs. */
size_t gdx3ds_audio_buffered(void);

/* ---- APT lifecycle gate (feat/3ds-home; additive, outside the Phase 0 surface) ----
 * libctru runs ndspFinalize right after the app's ONSUSPEND/ONSLEEP hook returns and
 * only ndsp may touch the DSP across HOME/sleep, so the hook must park both audio
 * threads first. suspend() raises the gate and waits (bounded, ~50 ms) for the drain
 * and HLE producer threads to park; it returns 1 when both acknowledged and reports
 * each thread's state (absent thread == parked). resume() clears the ndsp channel
 * queue, resets slots + ring, and releases both threads. Main thread only, from the
 * APT hook; both are safe before init. Stub: no-ops. */
int gdx3ds_audio_suspend(int* drainParked, int* producerParked);
void gdx3ds_audio_resume(void);
/* 1 between suspend() and resume(). Read by main()'s teardown to record the exit cause:
 * a Close order delivered while suspended (HOME menu Close / power menu) never fires
 * ONRESTORE, so the gate is still up when the frame loop is left. Stub: 0. */
int gdx3ds_audio_suspended(void);
/* Percent granted by APT_SetAppCpuTimeLimit at init (0 = no core-1 rung taken), for the
 * ONRESTORE re-application NS's resume path otherwise discards. */
int gdx3ds_audio_syscore_limit_percent(void);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_AUDIO_H */
