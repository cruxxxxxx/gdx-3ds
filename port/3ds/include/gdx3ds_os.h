/* port/3ds/include/gdx3ds_os.h -- 3DS window + input contract (stream B implements).
 *
 * CONTRACT STATUS: FROZEN (Phase 0). Changes require orchestrator sign-off.
 *
 * Scope: framebuffer/window bring-up and HID input only. Fibers are NOT here --
 * stream B implements the existing port/gdx_fiber.h surface verbatim
 * (gdx_fiber_convert_thread / gdx_fiber_create / gdx_fiber_switch /
 * gdx_fiber_current_thread_id, no destroy) in port/3ds/os/gdx_fiber_3ds.c,
 * honoring that header's invariants (scheduler-thread-only calls; entries
 * never return).
 */
#ifndef GDX3DS_OS_H
#define GDX3DS_OS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Top screen is 400x240. The port renders there; bottom screen is reserved for a
 * future config UI (post-MVP) and stays untouched by this contract. */
int gdx3ds_os_window_init(int* outWidth, int* outHeight);
void gdx3ds_os_window_shutdown(void);

/* Present the frame; blocks on vblank (gsp). Returns 0 normally, nonzero when the
 * OS requests application exit (APT) so the main loop can unwind cleanly. */
int gdx3ds_os_window_swap(void);

/* One N64-shaped pad snapshot. Mapping (stream B owns the exact table):
 * circle pad -> analog stick, A/B/X/Y + shoulders + d-pad -> N64 buttons.
 * Feeds the existing controller abstraction consumed by port/main.cpp:319-380's
 * desktop equivalent; on 3DS only pad 0 is ever connected. */
typedef struct Gdx3dsPadState {
    uint16_t buttons; /* N64 CONT_* button mask, already mapped */
    int8_t stickX;    /* -80..80, N64 range, already scaled from circle pad */
    int8_t stickY;
    uint8_t connected;
} Gdx3dsPadState;

void gdx3ds_os_poll_input(Gdx3dsPadState* outPads, int maxPads);

/* Monotonic time for the scheduler/os_time seam. */
uint64_t gdx3ds_os_time_ns(void);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_OS_H */
