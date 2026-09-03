#ifndef GDX_FRAME_PACER_H
#define GDX_FRAME_PACER_H

// Wall-clock frame pacer for the host loop (port/main.cpp).
//
// Call gdx_frame_pacer_tick() exactly ONCE per host-loop iteration, immediately
// after the window EndFrame(). One loop iteration == one VI tick == one 60 Hz
// game frame, so pacing the loop paces the simulation.
//
// Gated on the integer CVar "gEnhancements.Graphics.FramePacing": 0 = no-op, 1 = hold the loop to
// the N64 NTSC field rate (60 / 1.001 ~= 59.94 Hz) with a sleep-to-deadline wait.
//
// DEFAULT IS PLATFORM-SPECIFIC — OFF on Windows, ON on Linux — set via CVarRegisterInteger in
// port/main.cpp before the menu registers the CVar, so a persisted user toggle still wins. See
// gdx_frame_pacer.c for why the two platforms differ and for the VSync interaction. Interpolated
// high-FPS rendering (>60 fps by tweening two sim ticks) is not this module's job — see
// port/gdx_interp.h.

#ifdef __cplusplus
extern "C" {
#endif

// Safe to call every frame; not thread-safe (main loop only).
void gdx_frame_pacer_tick(void);

#ifdef __cplusplus
}
#endif

#endif // GDX_FRAME_PACER_H
